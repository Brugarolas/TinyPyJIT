#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/interpreter/regprofile.h"

#include "tinypy/common/utils.h"
#include "tinypy/tinypy.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define REG_DEPTH_UNKNOWN INT32_MIN
#define REG_MAX_REGISTERS 256u

typedef struct RegPatch {
    uint32_t code_index;
    int32_t target_ip;
} RegPatch;

typedef struct RegAnalysis {
    int32_t* depth;
    uint8_t* leader;
    uint64_t* bound_in;
    int32_t max_stack;
} RegAnalysis;

typedef struct RegBuilder {
    RegCode* out;
    const CodeObject* co;
    RegLowerError* error;

    RegAnalysis analysis;
    int32_t* legacy_to_reg;
    c11_vector /*T=RegPatch*/ patches;

    uint16_t* vstack;
    int32_t vdepth;
    uint16_t stack_base;
    uint16_t temp_base;
    uint16_t next_temp;
    uint16_t max_reg_plus_one;

    int32_t source_ip;
    uint16_t last_def;
    int32_t last_def_code_index;
    bool last_def_retargetable;
} RegBuilder;

typedef struct StackEffect {
    int32_t fall_effect;
    int32_t branch_effect;
    int32_t target;
    bool has_fallthrough;
    bool has_branch;
    bool terminal;
} StackEffect;

static void set_error(RegLowerError* error,
                      RegLowerStatus status,
                      int32_t ip,
                      const char* fmt,
                      ...) {
    if(error == NULL) return;
    error->status = status;
    error->bytecode_ip = ip;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error->message, sizeof(error->message), fmt, ap);
    va_end(ap);
}

static bool is_binary_opcode(Opcode op) {
    return op >= OP_BINARY_ADD && op <= OP_COMPARE_GE;
}

static bool is_supported_opcode(Opcode op) {
    switch(op) {
        case OP_NO_OP:
        case OP_POP_TOP:
        case OP_DUP_TOP:
        case OP_DUP_TOP_TWO:
        case OP_ROT_TWO:
        case OP_ROT_THREE:
        case OP_LOAD_CONST:
        case OP_LOAD_NONE:
        case OP_LOAD_TRUE:
        case OP_LOAD_FALSE:
        case OP_LOAD_SMALL_INT:
        case OP_LOAD_NAME_AS_INT:
        case OP_LOAD_ELLIPSIS:
        case OP_LOAD_FUNCTION:
        case OP_LOAD_NULL:
        case OP_LOAD_FAST:
        case OP_LOAD_NONLOCAL:
        case OP_LOAD_GLOBAL:
        case OP_LOAD_ATTR:
        case OP_LOAD_SELF_ATTR:
        case OP_LOAD_METHOD:
        case OP_LOAD_SUBSCR:
        case OP_STORE_FAST:
        case OP_STORE_GLOBAL:
        case OP_STORE_ATTR:
        case OP_STORE_SELF_ATTR:
        case OP_STORE_SUBSCR:
        case OP_DELETE_FAST:
        case OP_DELETE_GLOBAL:
        case OP_DELETE_ATTR:
        case OP_DELETE_SUBSCR:
        case OP_BUILD_IMAG:
        case OP_BUILD_BYTES:
        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST:
        case OP_BUILD_DICT:
        case OP_BUILD_SLICE:
        case OP_BUILD_STRING:
        case OP_BINARY_ADD:
        case OP_BINARY_SUB:
        case OP_BINARY_MUL:
        case OP_BINARY_TRUEDIV:
        case OP_BINARY_FLOORDIV:
        case OP_BINARY_MOD:
        case OP_BINARY_POW:
        case OP_BINARY_LSHIFT:
        case OP_BINARY_RSHIFT:
        case OP_BINARY_AND:
        case OP_BINARY_OR:
        case OP_BINARY_XOR:
        case OP_BINARY_MATMUL:
        case OP_COMPARE_LT:
        case OP_COMPARE_LE:
        case OP_COMPARE_EQ:
        case OP_COMPARE_NE:
        case OP_COMPARE_GT:
        case OP_COMPARE_GE:
        case OP_IS_OP:
        case OP_CONTAINS_OP:
        case OP_JUMP_FORWARD:
        case OP_POP_JUMP_IF_NOT_MATCH:
        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE:
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP:
        case OP_SHORTCUT_IF_FALSE_OR_POP:
        case OP_LOOP_CONTINUE:
        case OP_LOOP_BREAK:
        case OP_CALL:
        case OP_RETURN_VALUE:
        case OP_LIST_APPEND:
        case OP_DICT_ADD:
        case OP_UNARY_NEGATIVE:
        case OP_UNARY_NOT:
        case OP_UNARY_INVERT:
        case OP_GET_ITER:
        case OP_FOR_ITER:
        case OP_UNPACK_SEQUENCE:
        case OP_UNPACK_EX:
        case OP_FORMAT_STRING: return true;
        default: return false;
    }
}

static bool stack_effect(const Bytecode* bc, int32_t ip, StackEffect* out) {
    memset(out, 0, sizeof(*out));
    out->has_fallthrough = true;

    Opcode op = (Opcode)bc->op;
    if(!is_supported_opcode(op)) return false;

    if(is_binary_opcode(op) || op == OP_IS_OP || op == OP_CONTAINS_OP) {
        out->fall_effect = -1;
        return true;
    }

    switch(op) {
        case OP_NO_OP:
        case OP_ROT_TWO:
        case OP_ROT_THREE:
        case OP_BUILD_IMAG:
        case OP_UNARY_NEGATIVE:
        case OP_UNARY_NOT:
        case OP_UNARY_INVERT:
        case OP_GET_ITER:
        case OP_FORMAT_STRING: out->fall_effect = 0; break;

        case OP_POP_TOP:
        case OP_STORE_FAST:
        case OP_STORE_GLOBAL:
        case OP_LIST_APPEND: out->fall_effect = -1; break;
        case OP_DELETE_ATTR: out->fall_effect = 0; break;

        case OP_DUP_TOP:
        case OP_LOAD_CONST:
        case OP_LOAD_NONE:
        case OP_LOAD_TRUE:
        case OP_LOAD_FALSE:
        case OP_LOAD_SMALL_INT:
        case OP_LOAD_NAME_AS_INT:
        case OP_LOAD_ELLIPSIS:
        case OP_LOAD_FUNCTION:
        case OP_LOAD_NULL:
        case OP_LOAD_FAST:
        case OP_LOAD_NONLOCAL:
        case OP_LOAD_GLOBAL:
        case OP_BUILD_BYTES: out->fall_effect = 1; break;

        case OP_DUP_TOP_TWO: out->fall_effect = 2; break;
        case OP_LOAD_ATTR: out->fall_effect = 0; break;
        case OP_LOAD_SELF_ATTR: out->fall_effect = 1; break;
        case OP_LOAD_METHOD: out->fall_effect = 1; break;
        case OP_LOAD_SUBSCR: out->fall_effect = -1; break;
        case OP_STORE_ATTR: out->fall_effect = -2; break;
        case OP_STORE_SELF_ATTR: out->fall_effect = -1; break;
        case OP_STORE_SUBSCR: out->fall_effect = -3; break;
        case OP_DELETE_FAST:
        case OP_DELETE_GLOBAL: out->fall_effect = 0; break;
        case OP_DELETE_SUBSCR: out->fall_effect = -2; break;
        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST: out->fall_effect = 1 - (int32_t)bc->arg; break;
        case OP_BUILD_DICT: out->fall_effect = 1 - (int32_t)bc->arg * 2; break;
        case OP_BUILD_SLICE: out->fall_effect = -2; break;
        case OP_BUILD_STRING: out->fall_effect = 1 - (int32_t)bc->arg; break;
        case OP_DICT_ADD: out->fall_effect = -2; break;

        case OP_UNPACK_SEQUENCE: out->fall_effect = (int32_t)bc->arg - 1; break;
        case OP_UNPACK_EX: out->fall_effect = (int32_t)bc->arg; break;

        case OP_JUMP_FORWARD:
        case OP_LOOP_CONTINUE:
        case OP_LOOP_BREAK:
            out->has_fallthrough = false;
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->branch_effect = 0;
            break;

        case OP_POP_JUMP_IF_NOT_MATCH:
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->fall_effect = -2;
            out->branch_effect = -2;
            break;

        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE:
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->fall_effect = -1;
            out->branch_effect = -1;
            break;

        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP:
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->fall_effect = -1;
            out->branch_effect = 0;
            break;

        case OP_SHORTCUT_IF_FALSE_OR_POP:
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->fall_effect = -1;
            out->branch_effect = -1;
            break;

        case OP_FOR_ITER:
            out->has_branch = true;
            out->target = ip + (int16_t)bc->arg;
            out->fall_effect = 1;
            out->branch_effect = -1;
            break;

        case OP_CALL: {
            uint16_t argc = bc->arg & 0xFFu;
            uint16_t kwargc = bc->arg >> 8;
            out->fall_effect = -(int32_t)(1u + argc + kwargc * 2u);
            break;
        }

        case OP_RETURN_VALUE:
            out->has_fallthrough = false;
            out->terminal = true;
            out->fall_effect = bc->arg == BC_NOARG ? -1 : 0;
            break;

        default: return false;
    }
    return true;
}

static bool merge_depth(int32_t* depths,
                        int32_t target,
                        int32_t value,
                        int32_t n,
                        int32_t* work,
                        int32_t* work_len,
                        RegLowerError* error,
                        int32_t from_ip) {
    if(target < 0 || target > n) {
        set_error(error,
                  REG_LOWER_INVALID_BYTECODE,
                  from_ip,
                  "jump target %d is outside bytecode range [0, %d]",
                  target,
                  n);
        return false;
    }
    if(value < 0) {
        set_error(error,
                  REG_LOWER_STACK_MISMATCH,
                  from_ip,
                  "negative stack depth %d",
                  value);
        return false;
    }
    if(depths[target] == REG_DEPTH_UNKNOWN) {
        depths[target] = value;
        work[(*work_len)++] = target;
        return true;
    }
    if(depths[target] != value) {
        set_error(error,
                  REG_LOWER_STACK_MISMATCH,
                  from_ip,
                  "stack depth mismatch at bytecode %d: %d vs %d",
                  target,
                  depths[target],
                  value);
        return false;
    }
    return true;
}

static uint64_t transfer_bound(const Bytecode* bc, uint64_t mask) {
    if(bc->op == OP_STORE_FAST && bc->arg < 64) mask |= UINT64_C(1) << bc->arg;
    if(bc->op == OP_DELETE_FAST && bc->arg < 64) mask &= ~(UINT64_C(1) << bc->arg);
    return mask;
}

static bool analyze(const CodeObject* co,
                    uint64_t entry_bound,
                    RegAnalysis* out,
                    RegLowerError* error) {
    const int32_t n = co->codes.length;
    int32_t* depth = py_malloc((size_t)(n + 1) * sizeof(int32_t));
    uint8_t* leader = py_malloc((size_t)(n + 1));
    uint64_t* bound_in = py_malloc((size_t)(n + 1) * sizeof(uint64_t));
    uint8_t* bound_seen = py_malloc((size_t)(n + 1));
    int32_t* work = py_malloc((size_t)(n + 1) * sizeof(int32_t));
    if(depth == NULL || leader == NULL || bound_in == NULL || bound_seen == NULL || work == NULL) {
        if(depth) py_free(depth);
        if(leader) py_free(leader);
        if(bound_in) py_free(bound_in);
        if(bound_seen) py_free(bound_seen);
        if(work) py_free(work);
        set_error(error, REG_LOWER_OOM, -1, "out of memory during register-bytecode analysis");
        return false;
    }

    for(int32_t i = 0; i <= n; i++) depth[i] = REG_DEPTH_UNKNOWN;
    memset(leader, 0, (size_t)(n + 1));
    memset(bound_seen, 0, (size_t)(n + 1));
    depth[0] = 0;
    leader[0] = 1;

    int32_t work_len = 1;
    work[0] = 0;
    int32_t max_stack = 0;

    const Bytecode* codes = (const Bytecode*)co->codes.data;
    while(work_len > 0) {
        int32_t ip = work[--work_len];
        if(ip == n) continue;
        int32_t current = depth[ip];
        StackEffect effect;
        if(!stack_effect(&codes[ip], ip, &effect)) {
            set_error(error,
                      REG_LOWER_UNSUPPORTED,
                      ip,
                      "opcode %u is not yet supported by the register interpreter",
                      (unsigned)codes[ip].op);
            py_free(depth);
            py_free(leader);
            py_free(bound_in);
            py_free(bound_seen);
            py_free(work);
            return false;
        }

        if(current > max_stack) max_stack = current;
        if(effect.has_fallthrough) {
            int32_t next_depth = current + effect.fall_effect;
            if(next_depth > max_stack) max_stack = next_depth;
            if(!merge_depth(depth, ip + 1, next_depth, n, work, &work_len, error, ip)) {
                py_free(depth);
                py_free(leader);
                py_free(bound_in);
                py_free(bound_seen);
                py_free(work);
                return false;
            }
        }
        if(effect.has_branch) {
            int32_t branch_depth = current + effect.branch_effect;
            if(branch_depth > max_stack) max_stack = branch_depth;
            if(!merge_depth(depth,
                            effect.target,
                            branch_depth,
                            n,
                            work,
                            &work_len,
                            error,
                            ip)) {
                py_free(depth);
                py_free(leader);
                py_free(bound_in);
                py_free(bound_seen);
                py_free(work);
                return false;
            }
            leader[effect.target] = 1;
            if(effect.has_fallthrough && ip + 1 <= n) leader[ip + 1] = 1;
        }
    }

    /* Definite-assignment analysis. Merge is intersection: a local is definitely
     * bound only if it is bound on every incoming edge. This lattice only loses
     * bits after a state has first been discovered, so the fixed point is finite. */
    bound_seen[0] = 1;
    bound_in[0] = entry_bound;
    bool changed;
    do {
        changed = false;
        for(int32_t ip = 0; ip < n; ip++) {
            if(depth[ip] == REG_DEPTH_UNKNOWN || !bound_seen[ip]) continue;
            StackEffect effect;
            c11__rtassert(stack_effect(&codes[ip], ip, &effect));
            uint64_t out_mask = transfer_bound(&codes[ip], bound_in[ip]);

            int32_t succ[2];
            int succ_count = 0;
            if(effect.has_fallthrough) succ[succ_count++] = ip + 1;
            if(effect.has_branch) succ[succ_count++] = effect.target;

            for(int j = 0; j < succ_count; j++) {
                int32_t target = succ[j];
                if(target < 0 || target > n || depth[target] == REG_DEPTH_UNKNOWN) continue;
                if(!bound_seen[target]) {
                    bound_seen[target] = 1;
                    bound_in[target] = out_mask;
                    changed = true;
                } else {
                    uint64_t merged = bound_in[target] & out_mask;
                    if(merged != bound_in[target]) {
                        bound_in[target] = merged;
                        changed = true;
                    }
                }
            }
        }
    } while(changed);

    py_free(work);
    py_free(bound_seen);
    out->depth = depth;
    out->leader = leader;
    out->bound_in = bound_in;
    out->max_stack = max_stack;
    return true;
}

static void analysis_dtor(RegAnalysis* a) {
    if(a->depth) py_free(a->depth);
    if(a->leader) py_free(a->leader);
    if(a->bound_in) py_free(a->bound_in);
    a->depth = NULL;
    a->leader = NULL;
    a->bound_in = NULL;
}

void RegCode__ctor(RegCode* self) {
    c11_vector__ctor(&self->code, sizeof(RegBytecode));
    c11_vector__ctor(&self->source_ip, sizeof(int32_t));
    c11_vector__ctor(&self->runtime_sites, sizeof(RegRuntimeSite));
    c11_vector__ctor(&self->inline_caches, sizeof(RegInlineCache));
    c11_vector__ctor(&self->profile_sites, sizeof(RegProfileSite));
    c11_vector__ctor(&self->loop_profiles, sizeof(RegLoopProfile));
    self->nregs = 0;
    self->max_stack = 0;
    self->temp_base = 0;
    self->max_call_window = 0;
    self->quickened_count = 0;
    self->superinstruction_count = 0;
    self->function_entries = 0;
    self->function_hotness = 0;
    self->function_jit_state = (uint8_t)REG_JIT_COLD;
    self->reserved_profile = 0;
}

void RegCode__dtor(RegCode* self) {
    c11_vector__dtor(&self->code);
    c11_vector__dtor(&self->source_ip);
    c11_vector__dtor(&self->runtime_sites);
    c11_vector__dtor(&self->inline_caches);
    c11_vector__dtor(&self->profile_sites);
    c11_vector__dtor(&self->loop_profiles);
    memset(self, 0, sizeof(*self));
}

RegCode* RegCode__new(void) {
    RegCode* self = py_malloc(sizeof(RegCode));
    if(self) RegCode__ctor(self);
    return self;
}

void RegCode__delete(RegCode* self) {
    if(self == NULL) return;
    RegCode__dtor(self);
    py_free(self);
}

static bool ensure_reg(RegBuilder* b, uint32_t reg) {
    if(reg >= REG_MAX_REGISTERS) {
        set_error(b->error,
                  REG_LOWER_TOO_MANY_REGISTERS,
                  b->source_ip,
                  "register index %u exceeds 8-bit register encoding",
                  reg);
        return false;
    }
    if(reg + 1u > b->max_reg_plus_one) b->max_reg_plus_one = (uint16_t)(reg + 1u);
    return true;
}

static bool alloc_range(RegBuilder* b, uint16_t count, uint16_t* out) {
    uint32_t base = b->next_temp;
    uint32_t end = base + count;
    if(end > REG_MAX_REGISTERS) {
        set_error(b->error,
                  REG_LOWER_TOO_MANY_REGISTERS,
                  b->source_ip,
                  "register-bytecode lowering requires %u registers",
                  end);
        return false;
    }
    *out = (uint16_t)base;
    b->next_temp = (uint16_t)end;
    if(end > b->max_reg_plus_one) b->max_reg_plus_one = (uint16_t)end;
    return true;
}

static bool alloc_temp(RegBuilder* b, uint16_t* out) { return alloc_range(b, 1, out); }

static bool emit_raw(RegBuilder* b, RegBytecode inst) {
    c11_vector__push(RegBytecode, &b->out->code, inst);
    c11_vector__push(int32_t, &b->out->source_ip, b->source_ip);
    b->last_def_retargetable = false;
    return true;
}

static bool emit_def(RegBuilder* b, RegBytecode inst, uint16_t def, bool retargetable) {
    if(!ensure_reg(b, def)) return false;
    c11_vector__push(RegBytecode, &b->out->code, inst);
    c11_vector__push(int32_t, &b->out->source_ip, b->source_ip);
    b->last_def = def;
    b->last_def_code_index = b->out->code.length - 1;
    b->last_def_retargetable = retargetable;
    return true;
}

static bool emit_move(RegBuilder* b, uint16_t dst, uint16_t src) {
    if(dst == src) return true;
    if(!ensure_reg(b, dst) || !ensure_reg(b, src)) return false;
    return emit_def(b, RBC_ABC(RBC_MOVE, dst, src, 0), dst, true);
}

static bool vpush(RegBuilder* b, uint16_t reg) {
    if(b->vdepth >= b->analysis.max_stack + 2) {
        set_error(b->error,
                  REG_LOWER_STACK_MISMATCH,
                  b->source_ip,
                  "virtual stack overflow while lowering");
        return false;
    }
    b->vstack[b->vdepth++] = reg;
    return true;
}

static bool vpop(RegBuilder* b, uint16_t* out) {
    if(b->vdepth <= 0) {
        set_error(b->error,
                  REG_LOWER_STACK_MISMATCH,
                  b->source_ip,
                  "virtual stack underflow while lowering");
        return false;
    }
    *out = b->vstack[--b->vdepth];
    return true;
}

static int count_stack_reg(const RegBuilder* b, uint16_t reg) {
    int count = 0;
    for(int32_t i = 0; i < b->vdepth; i++) {
        if(b->vstack[i] == reg) count++;
    }
    return count;
}

static bool snapshot_stack_aliases(RegBuilder* b, uint16_t reg) {
    bool found = false;
    for(int32_t i = 0; i < b->vdepth; i++) {
        if(b->vstack[i] == reg) {
            found = true;
            break;
        }
    }
    if(!found) return true;

    uint16_t tmp;
    if(!alloc_temp(b, &tmp)) return false;
    if(!emit_move(b, tmp, reg)) return false;
    for(int32_t i = 0; i < b->vdepth; i++) {
        if(b->vstack[i] == reg) b->vstack[i] = tmp;
    }
    return true;
}

static bool retarget_last_def(RegBuilder* b, uint16_t old_reg, uint16_t new_reg) {
    if(!b->last_def_retargetable || b->last_def != old_reg) return false;
    if(count_stack_reg(b, old_reg) != 0) return false;
    if(!ensure_reg(b, new_reg)) return false;

    RegBytecode* inst = c11__at(RegBytecode, &b->out->code, b->last_def_code_index);
    *inst = (*inst & ~0x0000FF00u) | ((uint32_t)new_reg << 8);
    b->last_def = new_reg;
    return true;
}

static bool materialize_stack(RegBuilder* b) {
    const int32_t n = b->vdepth;
    if(n <= 0) return true;

    uint16_t* src = py_malloc((size_t)n * sizeof(uint16_t));
    uint16_t* dst = py_malloc((size_t)n * sizeof(uint16_t));
    uint8_t* pending = py_malloc((size_t)n);
    if(src == NULL || dst == NULL || pending == NULL) {
        if(src) py_free(src);
        if(dst) py_free(dst);
        if(pending) py_free(pending);
        set_error(b->error, REG_LOWER_OOM, b->source_ip, "out of memory resolving stack copies");
        return false;
    }

    int pending_count = 0;
    for(int32_t i = 0; i < n; i++) {
        src[i] = b->vstack[i];
        dst[i] = (uint16_t)(b->stack_base + i);
        pending[i] = (uint8_t)(src[i] != dst[i]);
        if(pending[i]) pending_count++;
    }

    while(pending_count > 0) {
        bool progress = false;
        for(int32_t i = 0; i < n; i++) {
            if(!pending[i]) continue;
            bool dst_is_source = false;
            for(int32_t j = 0; j < n; j++) {
                if(i == j || !pending[j]) continue;
                if(src[j] == dst[i]) {
                    dst_is_source = true;
                    break;
                }
            }
            if(dst_is_source) continue;
            if(!emit_move(b, dst[i], src[i])) goto fail;
            pending[i] = 0;
            pending_count--;
            progress = true;
        }
        if(progress) continue;

        int32_t cycle = -1;
        for(int32_t i = 0; i < n; i++) {
            if(pending[i]) {
                cycle = i;
                break;
            }
        }
        c11__rtassert(cycle >= 0);

        uint16_t tmp;
        if(!alloc_temp(b, &tmp)) goto fail;
        if(!emit_move(b, tmp, dst[cycle])) goto fail;
        for(int32_t j = 0; j < n; j++) {
            if(pending[j] && src[j] == dst[cycle]) src[j] = tmp;
        }
    }

    for(int32_t i = 0; i < n; i++) b->vstack[i] = (uint16_t)(b->stack_base + i);
    py_free(src);
    py_free(dst);
    py_free(pending);
    return true;

fail:
    py_free(src);
    py_free(dst);
    py_free(pending);
    return false;
}

static bool emit_patch(RegBuilder* b, RegBytecode inst, int32_t target_ip) {
    uint32_t index = (uint32_t)b->out->code.length;
    if(!emit_raw(b, inst)) return false;
    RegPatch patch = {index, target_ip};
    c11_vector__push(RegPatch, &b->patches, patch);
    return true;
}

static RegOpcode binary_reg_opcode(Opcode op, uint16_t arg) {
    (void)arg;
    switch(op) {
        case OP_BINARY_ADD: return RBC_ADD;
        case OP_BINARY_SUB: return RBC_SUB;
        case OP_BINARY_MUL: return RBC_MUL;
        case OP_BINARY_TRUEDIV: return RBC_TRUEDIV;
        case OP_BINARY_FLOORDIV: return RBC_FLOORDIV;
        case OP_BINARY_MOD: return RBC_MOD;
        case OP_BINARY_POW: return RBC_POW;
        case OP_BINARY_LSHIFT: return RBC_LSHIFT;
        case OP_BINARY_RSHIFT: return RBC_RSHIFT;
        case OP_BINARY_AND: return RBC_BITAND;
        case OP_BINARY_OR: return RBC_BITOR;
        case OP_BINARY_XOR: return RBC_BITXOR;
        case OP_BINARY_MATMUL: return RBC_MATMUL;
        case OP_COMPARE_LT: return RBC_CMP_LT;
        case OP_COMPARE_LE: return RBC_CMP_LE;
        case OP_COMPARE_EQ: return RBC_CMP_EQ;
        case OP_COMPARE_NE: return RBC_CMP_NE;
        case OP_COMPARE_GT: return RBC_CMP_GT;
        case OP_COMPARE_GE: return RBC_CMP_GE;
        case OP_IS_OP: return arg ? RBC_IS_NOT : RBC_IS;
        case OP_CONTAINS_OP: return arg ? RBC_NOT_CONTAINS : RBC_CONTAINS;
        default: return RBC_NOP;
    }
}

static bool emit_binary(RegBuilder* b, Opcode op, uint16_t arg) {
    uint16_t rhs, lhs, dst;
    if(!vpop(b, &rhs) || !vpop(b, &lhs) || !alloc_temp(b, &dst)) return false;
    RegOpcode rop = binary_reg_opcode(op, arg);
    if(rop == RBC_NOP) return false;
    if(!emit_def(b, RBC_ABC(rop, dst, lhs, rhs), dst, true)) return false;
    return vpush(b, dst);
}

static bool gather_window(RegBuilder* b,
                          int32_t start,
                          uint16_t count,
                          uint16_t* out_base) {
    if(count == 0) {
        *out_base = 0;
        return true;
    }
    uint16_t base;
    if(!alloc_range(b, count, &base)) return false;
    for(uint16_t i = 0; i < count; i++) {
        if(!emit_move(b, (uint16_t)(base + i), b->vstack[start + i])) return false;
    }
    *out_base = base;
    return true;
}

static bool lower_instruction(RegBuilder* b, const Bytecode* bc) {
    Opcode op = (Opcode)bc->op;

    if(is_binary_opcode(op) || op == OP_IS_OP || op == OP_CONTAINS_OP)
        return emit_binary(b, op, bc->arg);

    switch(op) {
        case OP_NO_OP: return true;
        case OP_POP_TOP: {
            uint16_t ignored;
            return vpop(b, &ignored);
        }
        case OP_DUP_TOP:
            if(b->vdepth < 1) return false;
            return vpush(b, b->vstack[b->vdepth - 1]);
        case OP_DUP_TOP_TWO:
            if(b->vdepth < 2) return false;
            if(!vpush(b, b->vstack[b->vdepth - 2])) return false;
            return vpush(b, b->vstack[b->vdepth - 2]);
        case OP_ROT_TWO:
            if(b->vdepth < 2) return false;
            {
                uint16_t t = b->vstack[b->vdepth - 1];
                b->vstack[b->vdepth - 1] = b->vstack[b->vdepth - 2];
                b->vstack[b->vdepth - 2] = t;
            }
            return true;
        case OP_ROT_THREE:
            if(b->vdepth < 3) return false;
            {
                uint16_t c = b->vstack[b->vdepth - 1];
                b->vstack[b->vdepth - 1] = b->vstack[b->vdepth - 2];
                b->vstack[b->vdepth - 2] = b->vstack[b->vdepth - 3];
                b->vstack[b->vdepth - 3] = c;
            }
            return true;

        case OP_LOAD_FAST:
            if(bc->arg >= b->co->nlocals) return false;
            if((b->analysis.bound_in[b->source_ip] & (UINT64_C(1) << bc->arg)) == 0) {
                if(!emit_raw(b, RBC_ABC(RBC_CHECK_BOUND, bc->arg, 0, 0))) return false;
            }
            return vpush(b, bc->arg);

        case OP_STORE_FAST: {
            uint16_t value;
            if(bc->arg >= b->co->nlocals || !vpop(b, &value)) return false;
            if(!snapshot_stack_aliases(b, bc->arg)) return false;
            if(retarget_last_def(b, value, bc->arg)) return true;
            return emit_move(b, bc->arg, value);
        }

        case OP_LOAD_CONST:
        case OP_LOAD_NONE:
        case OP_LOAD_TRUE:
        case OP_LOAD_FALSE:
        case OP_LOAD_SMALL_INT:
        case OP_LOAD_NAME_AS_INT:
        case OP_LOAD_ELLIPSIS:
        case OP_LOAD_FUNCTION:
        case OP_LOAD_NULL:
        case OP_LOAD_NONLOCAL:
        case OP_LOAD_GLOBAL:
        case OP_BUILD_BYTES: {
            uint16_t dst;
            if(!alloc_temp(b, &dst)) return false;
            RegBytecode inst;
            switch(op) {
                case OP_LOAD_CONST: inst = RBC_ABX(RBC_LOAD_CONST, dst, bc->arg); break;
                case OP_LOAD_NONE: inst = RBC_ABC(RBC_LOAD_NONE, dst, 0, 0); break;
                case OP_LOAD_TRUE: inst = RBC_ABC(RBC_LOAD_BOOL, dst, 1, 0); break;
                case OP_LOAD_FALSE: inst = RBC_ABC(RBC_LOAD_BOOL, dst, 0, 0); break;
                case OP_LOAD_SMALL_INT:
                    inst = RBC_ASBX(RBC_LOAD_I16, dst, (int16_t)bc->arg);
                    break;
                case OP_LOAD_NAME_AS_INT:
                    inst = RBC_ABX(RBC_LOAD_NAME_INT, dst, bc->arg);
                    break;
                case OP_LOAD_ELLIPSIS: inst = RBC_ABC(RBC_LOAD_ELLIPSIS, dst, 0, 0); break;
                case OP_LOAD_FUNCTION: inst = RBC_ABX(RBC_LOAD_FUNCTION, dst, bc->arg); break;
                case OP_LOAD_NULL: inst = RBC_ABC(RBC_LOAD_NIL, dst, 0, 0); break;
                case OP_LOAD_NONLOCAL: inst = RBC_ABX(RBC_LOAD_NONLOCAL, dst, bc->arg); break;
                case OP_LOAD_GLOBAL: inst = RBC_ABX(RBC_LOAD_GLOBAL, dst, bc->arg); break;
                case OP_BUILD_BYTES: inst = RBC_ABX(RBC_BUILD_BYTES, dst, bc->arg); break;
                default: c11__unreachable();
            }
            if(!emit_def(b, inst, dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_LOAD_ATTR:
        case OP_LOAD_SELF_ATTR: {
            uint16_t object;
            if(op == OP_LOAD_SELF_ATTR) {
                object = 0;
                if((b->analysis.bound_in[b->source_ip] & UINT64_C(1)) == 0) {
                    if(!emit_raw(b, RBC_ABC(RBC_CHECK_BOUND, 0, 0, 0))) return false;
                }
            } else if(!vpop(b, &object)) {
                return false;
            }
            uint16_t dst;
            if(!alloc_temp(b, &dst) || !emit_move(b, dst, object)) return false;
            if(!emit_def(b, RBC_ABX(RBC_LOAD_ATTR, dst, bc->arg), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_LOAD_METHOD: {
            uint16_t object, base;
            if(!vpop(b, &object) || !alloc_range(b, 2, &base)) return false;
            if(!emit_move(b, base, object)) return false;
            if(!emit_def(b, RBC_ABX(RBC_LOAD_METHOD, base, bc->arg), base, false)) return false;
            if(!vpush(b, base)) return false;
            return vpush(b, (uint16_t)(base + 1));
        }

        case OP_LOAD_SUBSCR: {
            uint16_t key, object, dst;
            if(!vpop(b, &key) || !vpop(b, &object) || !alloc_temp(b, &dst)) return false;
            if(!emit_def(b, RBC_ABC(RBC_LOAD_SUBSCR, dst, object, key), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_STORE_GLOBAL: {
            uint16_t value;
            if(!vpop(b, &value)) return false;
            return emit_raw(b, RBC_ABX(RBC_STORE_GLOBAL, value, bc->arg));
        }

        case OP_STORE_ATTR:
        case OP_STORE_SELF_ATTR: {
            uint16_t object, value, base;
            if(op == OP_STORE_SELF_ATTR) {
                object = 0;
                if(!vpop(b, &value)) return false;
                if((b->analysis.bound_in[b->source_ip] & UINT64_C(1)) == 0) {
                    if(!emit_raw(b, RBC_ABC(RBC_CHECK_BOUND, 0, 0, 0))) return false;
                }
            } else {
                if(!vpop(b, &object) || !vpop(b, &value)) return false;
            }
            if(!alloc_range(b, 2, &base)) return false;
            if(!emit_move(b, base, value) || !emit_move(b, (uint16_t)(base + 1), object))
                return false;
            return emit_raw(b, RBC_ABX(RBC_STORE_ATTR, base, bc->arg));
        }

        case OP_STORE_SUBSCR: {
            uint16_t key, object, value;
            if(!vpop(b, &key) || !vpop(b, &object) || !vpop(b, &value)) return false;
            return emit_raw(b, RBC_ABC(RBC_STORE_SUBSCR, value, object, key));
        }

        case OP_DELETE_FAST:
            if(bc->arg >= b->co->nlocals) return false;
            if(!snapshot_stack_aliases(b, bc->arg)) return false;
            return emit_raw(b, RBC_ABX(RBC_DELETE_FAST, bc->arg, 0));
        case OP_DELETE_GLOBAL: return emit_raw(b, RBC_ABX(RBC_DELETE_GLOBAL, 0, bc->arg));
        case OP_DELETE_ATTR: {
            if(b->vdepth < 1) return false;
            uint16_t object = b->vstack[b->vdepth - 1];
            return emit_raw(b, RBC_ABX(RBC_DELETE_ATTR, object, bc->arg));
        }
        case OP_DELETE_SUBSCR: {
            uint16_t key, object;
            if(!vpop(b, &key) || !vpop(b, &object)) return false;
            return emit_raw(b, RBC_ABC(RBC_DELETE_SUBSCR, object, key, 0));
        }

        case OP_BUILD_IMAG: {
            uint16_t value, dst;
            if(!vpop(b, &value) || !alloc_temp(b, &dst) || !emit_move(b, dst, value))
                return false;
            if(!emit_def(b, RBC_ABC(RBC_BUILD_IMAG, dst, 0, 0), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST:
        case OP_BUILD_STRING: {
            uint16_t count = bc->arg;
            if(count > 255u || b->vdepth < count) return false;
            int32_t start = b->vdepth - count;
            uint16_t base = 0;
            if(!gather_window(b, start, count, &base)) return false;
            b->vdepth = start;
            uint16_t dst;
            if(!alloc_temp(b, &dst)) return false;
            RegOpcode rop = op == OP_BUILD_TUPLE ? RBC_BUILD_TUPLE
                              : op == OP_BUILD_LIST ? RBC_BUILD_LIST
                                                    : RBC_BUILD_STRING;
            if(!emit_def(b, RBC_ABC(rop, dst, base, count), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_BUILD_DICT: {
            uint16_t pairs = bc->arg;
            uint32_t values = (uint32_t)pairs * 2u;
            if(pairs > 127u || b->vdepth < (int32_t)values) return false;
            int32_t start = b->vdepth - (int32_t)values;
            uint16_t base = 0;
            if(!gather_window(b, start, (uint16_t)values, &base)) return false;
            b->vdepth = start;
            uint16_t dst;
            if(!alloc_temp(b, &dst)) return false;
            if(!emit_def(b, RBC_ABC(RBC_BUILD_DICT, dst, base, pairs), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_BUILD_SLICE: {
            uint16_t step, stop, start, base;
            if(!vpop(b, &step) || !vpop(b, &stop) || !vpop(b, &start) ||
               !alloc_range(b, 3, &base))
                return false;
            if(!emit_move(b, base, start) || !emit_move(b, base + 1, stop) ||
               !emit_move(b, base + 2, step))
                return false;
            if(!emit_def(b, RBC_ABC(RBC_BUILD_SLICE, base, 0, 0), base, false)) return false;
            return vpush(b, base);
        }

        case OP_UNARY_NEGATIVE:
        case OP_UNARY_NOT:
        case OP_UNARY_INVERT:
        case OP_GET_ITER: {
            uint16_t src, dst;
            if(!vpop(b, &src) || !alloc_temp(b, &dst)) return false;
            RegOpcode rop = op == OP_UNARY_NEGATIVE ? RBC_NEG
                              : op == OP_UNARY_NOT   ? RBC_NOT
                              : op == OP_UNARY_INVERT ? RBC_INVERT
                                                      : RBC_GET_ITER;
            if(!emit_def(b, RBC_ABC(rop, dst, src, 0), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_FORMAT_STRING: {
            uint16_t src, dst;
            if(!vpop(b, &src) || !alloc_temp(b, &dst) || !emit_move(b, dst, src)) return false;
            if(!emit_def(b, RBC_ABX(RBC_FORMAT_STRING, dst, bc->arg), dst, true)) return false;
            return vpush(b, dst);
        }

        case OP_CALL: {
            uint16_t argc = bc->arg & 0xFFu;
            uint16_t kwargc = bc->arg >> 8;
            uint32_t window = 2u + argc + kwargc * 2u;
            if(window > 255u || b->vdepth < (int32_t)window) return false;
            int32_t start = b->vdepth - (int32_t)window;
            uint16_t base;
            if(!gather_window(b, start, (uint16_t)window, &base)) return false;
            if(window > b->out->max_call_window) b->out->max_call_window = (uint16_t)window;
            b->vdepth = start;
            if(!emit_def(b, RBC_ABC(RBC_CALL, base, argc, kwargc), base, false)) return false;
            return vpush(b, base);
        }

        case OP_LIST_APPEND: {
            if(b->vdepth < 3) return false;
            uint16_t value;
            if(!vpop(b, &value)) return false;
            uint16_t list = b->vstack[b->vdepth - 2];
            return emit_raw(b, RBC_ABC(RBC_LIST_APPEND, list, value, 0));
        }

        case OP_DICT_ADD: {
            if(b->vdepth < 4) return false;
            uint16_t value, key;
            if(!vpop(b, &value) || !vpop(b, &key)) return false;
            uint16_t dict = b->vstack[b->vdepth - 2];
            return emit_raw(b, RBC_ABC(RBC_DICT_ADD, dict, key, value));
        }

        case OP_UNPACK_SEQUENCE: {
            if(bc->arg > 255u) return false;
            uint16_t src, base;
            if(!vpop(b, &src) || !alloc_range(b, bc->arg, &base)) return false;
            if(!emit_def(b, RBC_ABC(RBC_UNPACK_SEQUENCE, base, src, (uint8_t)bc->arg), base, false))
                return false;
            for(uint16_t i = 0; i < bc->arg; i++) {
                if(!vpush(b, (uint16_t)(base + i))) return false;
            }
            return true;
        }

        case OP_UNPACK_EX: {
            uint32_t outputs = (uint32_t)bc->arg + 1u;
            if(outputs > 255u) return false;
            uint16_t src, base;
            if(!vpop(b, &src) || !alloc_range(b, (uint16_t)outputs, &base)) return false;
            if(!emit_def(b, RBC_ABC(RBC_UNPACK_EX, base, src, (uint8_t)bc->arg), base, false))
                return false;
            for(uint16_t i = 0; i < outputs; i++) {
                if(!vpush(b, (uint16_t)(base + i))) return false;
            }
            return true;
        }

        case OP_JUMP_FORWARD:
        case OP_LOOP_CONTINUE:
        case OP_LOOP_BREAK:
            if(!materialize_stack(b)) return false;
            return emit_patch(b,
                              RBC_ASBX(RBC_JUMP, 0, 0),
                              b->source_ip + (int16_t)bc->arg);

        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE: {
            if(!materialize_stack(b)) return false;
            uint16_t cond;
            if(!vpop(b, &cond)) return false;
            RegOpcode rop = op == OP_POP_JUMP_IF_FALSE ? RBC_JUMP_IF_FALSE : RBC_JUMP_IF_TRUE;
            return emit_patch(b,
                              RBC_ASBX(rop, cond, 0),
                              b->source_ip + (int16_t)bc->arg);
        }

        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP: {
            if(!materialize_stack(b) || b->vdepth < 1) return false;
            uint16_t cond = b->vstack[b->vdepth - 1];
            RegOpcode rop = op == OP_JUMP_IF_TRUE_OR_POP ? RBC_JUMP_IF_TRUE_OR_POP
                                                         : RBC_JUMP_IF_FALSE_OR_POP;
            if(!emit_patch(b,
                           RBC_ASBX(rop, cond, 0),
                           b->source_ip + (int16_t)bc->arg))
                return false;
            b->vdepth--;
            return true;
        }

        case OP_SHORTCUT_IF_FALSE_OR_POP: {
            if(!materialize_stack(b) || b->vdepth < 2) return false;
            uint16_t base = b->vstack[b->vdepth - 2];
            if(!emit_patch(b,
                           RBC_ASBX(RBC_SHORTCUT_IF_FALSE_OR_POP, base, 0),
                           b->source_ip + (int16_t)bc->arg))
                return false;
            b->vdepth--;
            return true;
        }

        case OP_POP_JUMP_IF_NOT_MATCH: {
            if(!materialize_stack(b) || b->vdepth < 2) return false;
            uint16_t base = b->vstack[b->vdepth - 2];
            if(!emit_patch(b,
                           RBC_ASBX(RBC_JUMP_IF_NOT_MATCH, base, 0),
                           b->source_ip + (int16_t)bc->arg))
                return false;
            b->vdepth -= 2;
            return true;
        }

        case OP_FOR_ITER: {
            if(!materialize_stack(b) || b->vdepth < 1) return false;
            uint16_t iterator = b->vstack[b->vdepth - 1];
            if(iterator + 1u >= REG_MAX_REGISTERS) return false;
            if(!emit_patch(b,
                           RBC_ASBX(RBC_FOR_ITER, iterator, 0),
                           b->source_ip + (int16_t)bc->arg))
                return false;
            return vpush(b, (uint16_t)(iterator + 1));
        }

        case OP_RETURN_VALUE:
            if(bc->arg == BC_NOARG) {
                uint16_t value;
                if(!vpop(b, &value)) return false;
                return emit_raw(b, RBC_ABC(RBC_RETURN, value, 0, 0));
            }
            return emit_raw(b, RBC_ABC(RBC_RETURN_NONE, 0, 0, 0));

        default: return false;
    }
}

static bool lower_impl(RegCode* out,
                       const CodeObject* co,
                       uint64_t entry_bound,
                       RegLowerError* error) {
    if(error) {
        error->status = REG_LOWER_OK;
        error->bytecode_ip = -1;
        error->message[0] = '\0';
    }
    if(out == NULL || co == NULL) {
        set_error(error, REG_LOWER_INVALID_BYTECODE, -1, "null RegCode or CodeObject");
        return false;
    }
    if(co->nlocals < 0 || co->nlocals > 255) {
        set_error(error,
                  REG_LOWER_TOO_MANY_REGISTERS,
                  -1,
                  "CodeObject has %d locals; register VM supports at most 255",
                  co->nlocals);
        return false;
    }

    RegAnalysis analysis = {0};
    if(!analyze(co, entry_bound, &analysis, error)) return false;
    if((uint32_t)co->nlocals + (uint32_t)analysis.max_stack >= REG_MAX_REGISTERS) {
        set_error(error,
                  REG_LOWER_TOO_MANY_REGISTERS,
                  -1,
                  "locals (%d) + operand stack (%d) exhaust 8-bit register space",
                  co->nlocals,
                  analysis.max_stack);
        analysis_dtor(&analysis);
        return false;
    }

    c11_vector__clear(&out->code);
    c11_vector__clear(&out->source_ip);
    c11_vector__clear(&out->runtime_sites);
    c11_vector__clear(&out->inline_caches);
    out->quickened_count = 0;
    out->superinstruction_count = 0;
    out->nregs = 0;
    out->max_stack = (uint16_t)analysis.max_stack;
    out->max_call_window = 0;

    const int32_t n = co->codes.length;
    int32_t* legacy_to_reg = py_malloc((size_t)(n + 1) * sizeof(int32_t));
    uint16_t* vstack = py_malloc((size_t)(analysis.max_stack + 2) * sizeof(uint16_t));
    if(legacy_to_reg == NULL || vstack == NULL) {
        if(legacy_to_reg) py_free(legacy_to_reg);
        if(vstack) py_free(vstack);
        analysis_dtor(&analysis);
        set_error(error, REG_LOWER_OOM, -1, "out of memory during register lowering");
        return false;
    }
    for(int32_t i = 0; i <= n; i++) legacy_to_reg[i] = -1;

    RegBuilder b;
    memset(&b, 0, sizeof(b));
    b.out = out;
    b.co = co;
    b.error = error;
    b.analysis = analysis;
    b.legacy_to_reg = legacy_to_reg;
    c11_vector__ctor(&b.patches, sizeof(RegPatch));
    b.vstack = vstack;
    b.stack_base = (uint16_t)co->nlocals;
    b.temp_base = (uint16_t)(co->nlocals + analysis.max_stack);
    b.next_temp = b.temp_base;
    b.max_reg_plus_one = b.temp_base;
    b.last_def_code_index = -1;
    out->temp_base = b.temp_base;

    const Bytecode* codes = (const Bytecode*)co->codes.data;
    bool ok = true;

    for(int32_t ip = 0; ip < n; ip++) {
        if(analysis.depth[ip] == REG_DEPTH_UNKNOWN) continue;
        b.source_ip = ip;
        b.legacy_to_reg[ip] = b.out->code.length;

        if(analysis.leader[ip]) {
            b.vdepth = analysis.depth[ip];
            b.next_temp = b.temp_base;
            b.last_def_retargetable = false;
            for(int32_t i = 0; i < b.vdepth; i++) b.vstack[i] = (uint16_t)(b.stack_base + i);
        }

        StackEffect effect;
        if(!stack_effect(&codes[ip], ip, &effect)) {
            set_error(error,
                      REG_LOWER_UNSUPPORTED,
                      ip,
                      "opcode %u is not supported",
                      (unsigned)codes[ip].op);
            ok = false;
            break;
        }

        if(!lower_instruction(&b, &codes[ip])) {
            if(error && error->status == REG_LOWER_OK) {
                set_error(error,
                          REG_LOWER_INVALID_BYTECODE,
                          ip,
                          "failed to lower opcode %u",
                          (unsigned)codes[ip].op);
            }
            ok = false;
            break;
        }

        if(effect.has_fallthrough && ip + 1 <= n && analysis.leader[ip + 1]) {
            if(!materialize_stack(&b)) {
                ok = false;
                break;
            }
        }
    }

    if(ok) {
        b.legacy_to_reg[n] = b.out->code.length;
        c11__foreach(RegPatch, &b.patches, patch) {
            if(patch->target_ip < 0 || patch->target_ip > n ||
               b.legacy_to_reg[patch->target_ip] < 0) {
                set_error(error,
                          REG_LOWER_INVALID_BYTECODE,
                          -1,
                          "register jump targets unreachable legacy bytecode %d",
                          patch->target_ip);
                ok = false;
                break;
            }
            int32_t offset = b.legacy_to_reg[patch->target_ip] - (int32_t)patch->code_index;
            if(offset < INT16_MIN || offset > INT16_MAX) {
                set_error(error,
                          REG_LOWER_JUMP_TOO_FAR,
                          patch->target_ip,
                          "register-bytecode jump offset %d does not fit int16",
                          offset);
                ok = false;
                break;
            }
            RegBytecode* inst = c11__at(RegBytecode, &b.out->code, patch->code_index);
            *inst = (*inst & 0x0000FFFFu) | ((uint32_t)(uint16_t)(int16_t)offset << 16);
        }
    }

    if(ok) {
        out->nregs = b.max_reg_plus_one;
        if(out->nregs < (uint16_t)co->nlocals) out->nregs = (uint16_t)co->nlocals;
        ok = RegCode__prepare_runtime(out, co, error);
    }
    if(!ok) {
        c11_vector__clear(&out->code);
        c11_vector__clear(&out->source_ip);
        c11_vector__clear(&out->runtime_sites);
        c11_vector__clear(&out->inline_caches);
        out->nregs = 0;
    }

    c11_vector__dtor(&b.patches);
    py_free(legacy_to_reg);
    py_free(vstack);
    analysis_dtor(&analysis);
    return ok;
}


static RegOpcode immediate_variant(RegOpcode op) {
    switch(op) {
        case RBC_ADD: return RBC_ADD_I8;
        case RBC_SUB: return RBC_SUB_I8;
        case RBC_MUL: return RBC_MUL_I8;
        case RBC_CMP_LT: return RBC_CMP_LT_I8;
        case RBC_CMP_LE: return RBC_CMP_LE_I8;
        case RBC_CMP_EQ: return RBC_CMP_EQ_I8;
        case RBC_CMP_NE: return RBC_CMP_NE_I8;
        case RBC_CMP_GT: return RBC_CMP_GT_I8;
        case RBC_CMP_GE: return RBC_CMP_GE_I8;
        default: return RBC_NOP;
    }
}

static RegOpcode reverse_compare(RegOpcode op) {
    switch(op) {
        case RBC_CMP_LT: return RBC_CMP_GT;
        case RBC_CMP_LE: return RBC_CMP_GE;
        case RBC_CMP_EQ: return RBC_CMP_EQ;
        case RBC_CMP_NE: return RBC_CMP_NE;
        case RBC_CMP_GT: return RBC_CMP_LT;
        case RBC_CMP_GE: return RBC_CMP_LE;
        default: return RBC_NOP;
    }
}

static int compare_code(RegOpcode op) {
    switch(op) {
        case RBC_CMP_LT: return REG_CMP_LT;
        case RBC_CMP_LE: return REG_CMP_LE;
        case RBC_CMP_EQ: return REG_CMP_EQ;
        case RBC_CMP_NE: return REG_CMP_NE;
        case RBC_CMP_GT: return REG_CMP_GT;
        case RBC_CMP_GE: return REG_CMP_GE;
        default: return -1;
    }
}

static int immediate_compare_code(RegOpcode op) {
    switch(op) {
        case RBC_CMP_LT_I8: return REG_CMP_LT;
        case RBC_CMP_LE_I8: return REG_CMP_LE;
        case RBC_CMP_EQ_I8: return REG_CMP_EQ;
        case RBC_CMP_NE_I8: return REG_CMP_NE;
        case RBC_CMP_GT_I8: return REG_CMP_GT;
        case RBC_CMP_GE_I8: return REG_CMP_GE;
        default: return -1;
    }
}

static void mark_jump_targets(const RegCode* code, uint8_t* target) {
    memset(target, 0, (size_t)code->code.length);
    for(int32_t pc = 0; pc < code->code.length; pc++) {
        RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        RegOpcode op = RBC_OP(inst);
        int32_t dst = -1;
        switch(op) {
            case RBC_FOR_ITER:
            case RBC_FOR_GENERATOR:
            case RBC_FOR_ARRAY2D:
            case RBC_FOR_LIST:
            case RBC_FOR_TUPLE:
            case RBC_FOR_DICT:
            case RBC_FOR_RANGE:
            case RBC_FOR_STR:
            case RBC_JUMP:
            case RBC_JUMP_IF_FALSE:
            case RBC_JUMP_IF_TRUE:
            case RBC_JUMP_IF_TRUE_OR_POP:
            case RBC_JUMP_IF_FALSE_OR_POP:
            case RBC_SHORTCUT_IF_FALSE_OR_POP:
            case RBC_JUMP_IF_NOT_MATCH:
                dst = pc + RBC_SBX(inst);
                break;
            default: break;
        }
        if(dst >= 0 && dst < code->code.length) target[dst] = 1;
    }
}

static bool is_reg_control_terminator(RegOpcode op) {
    switch(op) {
        case RBC_FOR_ITER:
        case RBC_FOR_GENERATOR:
        case RBC_FOR_ARRAY2D:
        case RBC_FOR_LIST:
        case RBC_FOR_TUPLE:
        case RBC_FOR_DICT:
        case RBC_FOR_RANGE:
        case RBC_FOR_STR:
        case RBC_JUMP:
        case RBC_JUMP_IF_FALSE:
        case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP:
        case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_SHORTCUT_IF_FALSE_OR_POP:
        case RBC_JUMP_IF_NOT_MATCH:
        case RBC_RETURN:
        case RBC_RETURN_NONE:
            return true;
        default:
            return false;
    }
}

/*
 * Temp registers are block-local and intentionally reused in later blocks.
 * Before deleting a small-int producer, make sure the produced temp is not
 * consumed again in the same block. The raw-field check is conservative: an
 * immediate byte equal to the temp number may suppress a fold, but can never
 * make an unsafe fold legal.
 */
static bool temp_has_later_block_use(const RegCode* code,
                                     const uint8_t* target,
                                     int32_t first_pc,
                                     uint8_t temp) {
    const RegBytecode* words = (const RegBytecode*)code->code.data;
    for(int32_t pc = first_pc; pc < code->code.length; pc++) {
        if(target[pc]) break;
        RegBytecode inst = words[pc];
        if(RBC_A(inst) == temp || RBC_B(inst) == temp || RBC_C(inst) == temp) return true;
        if(is_reg_control_terminator(RBC_OP(inst))) break;
    }
    return false;
}

static void optimize_superinstructions(RegCode* code) {
    if(code->code.length < 2) return;

    uint8_t* target = py_malloc((size_t)code->code.length);
    if(target == NULL) return;
    mark_jump_targets(code, target);

    RegBytecode* words = (RegBytecode*)code->code.data;

    /* Fold a temporary small-int load into the immediately consuming operation. */
    for(int32_t pc = 0; pc + 1 < code->code.length; pc++) {
        RegBytecode load = words[pc];
        if(RBC_OP(load) != RBC_LOAD_I16) continue;
        int32_t imm = RBC_SBX(load);
        if(imm < INT8_MIN || imm > INT8_MAX) continue;

        uint8_t tmp = RBC_A(load);
        if(tmp < code->temp_base) continue;

        RegBytecode use = words[pc + 1];
        RegOpcode op = RBC_OP(use);
        RegOpcode folded = RBC_NOP;
        uint8_t other = 0;

        /* A duplicated operand still needs the temporary value after folding. */
        if(RBC_B(use) == tmp && RBC_C(use) == tmp) continue;
        if(target[pc + 1]) continue;

        if(RBC_C(use) == tmp) {
            folded = immediate_variant(op);
            other = RBC_B(use);
        } else if(RBC_B(use) == tmp) {
            if(op == RBC_ADD || op == RBC_MUL || op == RBC_CMP_EQ || op == RBC_CMP_NE) {
                folded = immediate_variant(op);
                other = RBC_C(use);
            } else {
                RegOpcode reversed = reverse_compare(op);
                if(reversed != RBC_NOP) {
                    folded = immediate_variant(reversed);
                    other = RBC_C(use);
                }
            }
        }

        if(folded == RBC_NOP) continue;
        if(temp_has_later_block_use(code, target, pc + 2, tmp)) continue;
        words[pc] = RBC_ABC(RBC_NOP, 0, 0, 0);
        words[pc + 1] = RBC_ABC(folded, RBC_A(use), other, (uint8_t)(int8_t)imm);
        code->superinstruction_count++;
    }

    /*
     * Fuse compare + pop-branch without changing code size. A CFG boundary can
     * force the lowering pass to materialize the comparison result through one
     * MOVE; recognize that form too. The extension word stores both the branch
     * delta and how many trailing words the interpreter should skip.
     */
    for(int32_t pc = 0; pc + 1 < code->code.length; pc++) {
        RegBytecode cmp = words[pc];
        int cc = compare_code(RBC_OP(cmp));
        bool immediate = false;
        if(cc < 0) {
            cc = immediate_compare_code(RBC_OP(cmp));
            immediate = cc >= 0;
        }
        if(cc < 0) continue;
        if(RBC_A(cmp) < code->temp_base) continue;

        int32_t branch_pc = pc + 1;
        uint8_t skip_words = 0;
        uint8_t branch_reg = RBC_A(cmp);

        if(branch_pc < code->code.length && RBC_OP(words[branch_pc]) == RBC_MOVE &&
           RBC_B(words[branch_pc]) == RBC_A(cmp)) {
            if(branch_pc + 1 >= code->code.length || target[branch_pc]) continue;
            branch_reg = RBC_A(words[branch_pc]);
            branch_pc++;
            skip_words = 1;
        }

        RegBytecode branch = words[branch_pc];
        RegOpcode bop = RBC_OP(branch);
        if((bop != RBC_JUMP_IF_FALSE && bop != RBC_JUMP_IF_TRUE) ||
           RBC_A(branch) != branch_reg || target[branch_pc])
            continue;

        int32_t target_pc = branch_pc + RBC_SBX(branch);
        int32_t ext_offset = target_pc - (pc + 1);
        if(ext_offset < INT16_MIN || ext_offset > INT16_MAX) continue;

        uint8_t control = (uint8_t)cc;
        if(bop == RBC_JUMP_IF_TRUE) control |= REG_CMP_BRANCH_TRUE;
        if(immediate) {
            words[pc] = RBC_ABC(RBC_CMP_I8_BRANCH, RBC_B(cmp), RBC_C(cmp), control);
        } else {
            words[pc] = RBC_ABC(RBC_CMP_BRANCH, RBC_B(cmp), RBC_C(cmp), control);
        }
        words[pc + 1] = RBC_ASBX(RBC_SUPER_DATA, skip_words, (int16_t)ext_offset);
        if(skip_words) words[pc + 2] = RBC_ABC(RBC_NOP, 0, 0, 0);
        code->superinstruction_count++;
        pc += 1 + skip_words;
    }

    py_free(target);
}

static void init_runtime_sites(RegCode* code) {
    c11_vector__clear(&code->runtime_sites);
    c11_vector__clear(&code->inline_caches);
    for(int32_t pc = 0; pc < code->code.length; pc++) {
        RegRuntimeSite site;
        memset(&site, 0, sizeof(site));
        site.cache_index = REG_IC_NONE;
        site.feedback_index = REG_PROFILE_NONE;
        site.loop_index = REG_PROFILE_NONE;
        c11_vector__push(RegRuntimeSite, &code->runtime_sites, site);
    }
    code->quickened_count = 0;
    RegProfile__reset(code);
}

static RegOpcode adaptive_base_opcode(RegOpcode op) {
    switch(op) {
        case RBC_ADD_INT:
        case RBC_ADD_NUM: return RBC_ADD;
        case RBC_SUB_INT:
        case RBC_SUB_NUM: return RBC_SUB;
        case RBC_MUL_INT:
        case RBC_MUL_NUM: return RBC_MUL;
        case RBC_CMP_LT_INT:
        case RBC_CMP_LT_NUM: return RBC_CMP_LT;
        case RBC_CMP_LE_INT:
        case RBC_CMP_LE_NUM: return RBC_CMP_LE;
        case RBC_CMP_EQ_INT:
        case RBC_CMP_EQ_NUM: return RBC_CMP_EQ;
        case RBC_CMP_NE_INT:
        case RBC_CMP_NE_NUM: return RBC_CMP_NE;
        case RBC_CMP_GT_INT:
        case RBC_CMP_GT_NUM: return RBC_CMP_GT;
        case RBC_CMP_GE_INT:
        case RBC_CMP_GE_NUM: return RBC_CMP_GE;
        case RBC_LOAD_GLOBAL_IC: return RBC_LOAD_GLOBAL;
        case RBC_LOAD_ATTR_IC: return RBC_LOAD_ATTR;
        case RBC_LOAD_METHOD_IC: return RBC_LOAD_METHOD;
        case RBC_CALL_IC: return RBC_CALL;
        case RBC_FOR_GENERATOR:
        case RBC_FOR_ARRAY2D:
        case RBC_FOR_LIST:
        case RBC_FOR_TUPLE:
        case RBC_FOR_DICT:
        case RBC_FOR_RANGE:
        case RBC_FOR_STR: return RBC_FOR_ITER;
        default: return op;
    }
}

void RegCode__reset_runtime(RegCode* code) {
    if(code == NULL) return;
    RegBytecode* words = (RegBytecode*)code->code.data;
    for(int32_t pc = 0; pc < code->code.length; pc++) {
        RegOpcode op = RBC_OP(words[pc]);
        RegOpcode base = adaptive_base_opcode(op);
        if(base != op) words[pc] = (words[pc] & ~UINT32_C(0xFF)) | (uint32_t)base;
    }
    init_runtime_sites(code);
}

void RegCode__reset_profile(RegCode* code) { RegProfile__reset(code); }

bool RegCode__prepare_runtime(RegCode* code, const CodeObject* co, RegLowerError* error) {
    if(code == NULL || co == NULL) {
        set_error(error, REG_LOWER_INVALID_BYTECODE, -1, "null register bytecode");
        return false;
    }
    if(!RegCode__verify(code, co, error)) return false;
    code->superinstruction_count = 0;
    optimize_superinstructions(code);
    if(!RegCode__verify(code, co, error)) return false;
    init_runtime_sites(code);
    return true;
}


static bool verify_reg(const RegCode* code, uint32_t reg) {
    return reg < code->nregs;
}

static bool verify_range(const RegCode* code, uint32_t base, uint32_t count) {
    return count == 0 || (base < code->nregs && base + count <= code->nregs);
}

bool RegCode__verify(const RegCode* code, const CodeObject* co, RegLowerError* error) {
    if(code == NULL || co == NULL || code->code.length != code->source_ip.length || code->nregs > 256u) {
        set_error(error, REG_LOWER_INVALID_BYTECODE, -1, "invalid register-bytecode container");
        return false;
    }

    for(int32_t pc = 0; pc < code->code.length; pc++) {
        RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        RegOpcode op = RBC_OP(inst);
        uint32_t a = RBC_A(inst), b = RBC_B(inst), c = RBC_C(inst), bx = RBC_BX(inst);
        bool ok = true;

        if((unsigned)op >= RBC__COUNT) ok = false;
        switch(op) {
            case RBC_NOP: break;
            case RBC_MOVE: ok = verify_reg(code, a) && verify_reg(code, b); break;
            case RBC_CHECK_BOUND: ok = a < (uint32_t)co->nlocals; break;
            case RBC_LOAD_CONST:
                ok = verify_reg(code, a) && bx < (uint32_t)co->consts.length;
                break;
            case RBC_LOAD_NONE:
            case RBC_LOAD_NIL:
            case RBC_LOAD_ELLIPSIS:
            case RBC_LOAD_BOOL:
            case RBC_LOAD_I16: ok = verify_reg(code, a); break;
            case RBC_LOAD_NAME_INT:
            case RBC_LOAD_GLOBAL:
            case RBC_LOAD_GLOBAL_IC:
            case RBC_LOAD_NONLOCAL:
            case RBC_LOAD_ATTR:
            case RBC_LOAD_ATTR_IC:
            case RBC_LOAD_METHOD:
            case RBC_LOAD_METHOD_IC:
            case RBC_STORE_GLOBAL:
            case RBC_STORE_ATTR:
            case RBC_DELETE_GLOBAL:
            case RBC_DELETE_ATTR:
                ok = verify_reg(code, a) && bx < (uint32_t)co->names.length;
                if(op == RBC_LOAD_METHOD || op == RBC_LOAD_METHOD_IC || op == RBC_STORE_ATTR)
                    ok = ok && verify_reg(code, a + 1u);
                break;
            case RBC_LOAD_FUNCTION:
                ok = verify_reg(code, a) && bx < (uint32_t)co->func_decls.length;
                break;
            case RBC_BUILD_BYTES:
            case RBC_FORMAT_STRING:
                ok = verify_reg(code, a) && bx < (uint32_t)co->consts.length;
                break;
            case RBC_LOAD_SUBSCR:
            case RBC_STORE_SUBSCR:
            case RBC_ADD:
            case RBC_SUB:
            case RBC_MUL:
            case RBC_TRUEDIV:
            case RBC_FLOORDIV:
            case RBC_MOD:
            case RBC_POW:
            case RBC_LSHIFT:
            case RBC_RSHIFT:
            case RBC_BITAND:
            case RBC_BITOR:
            case RBC_BITXOR:
            case RBC_MATMUL:
            case RBC_CMP_LT:
            case RBC_CMP_LE:
            case RBC_CMP_EQ:
            case RBC_CMP_NE:
            case RBC_CMP_GT:
            case RBC_CMP_GE:
            case RBC_ADD_INT:
            case RBC_SUB_INT:
            case RBC_MUL_INT:
            case RBC_ADD_NUM:
            case RBC_SUB_NUM:
            case RBC_MUL_NUM:
            case RBC_CMP_LT_INT:
            case RBC_CMP_LE_INT:
            case RBC_CMP_EQ_INT:
            case RBC_CMP_NE_INT:
            case RBC_CMP_GT_INT:
            case RBC_CMP_GE_INT:
            case RBC_CMP_LT_NUM:
            case RBC_CMP_LE_NUM:
            case RBC_CMP_EQ_NUM:
            case RBC_CMP_NE_NUM:
            case RBC_CMP_GT_NUM:
            case RBC_CMP_GE_NUM:
            case RBC_IS:
            case RBC_IS_NOT:
            case RBC_CONTAINS:
            case RBC_NOT_CONTAINS:
            case RBC_DICT_ADD:
                ok = verify_reg(code, a) && verify_reg(code, b) && verify_reg(code, c);
                break;
            case RBC_DELETE_FAST: ok = a < (uint32_t)co->nlocals; break;
            case RBC_DELETE_SUBSCR:
            case RBC_LIST_APPEND:
                ok = verify_reg(code, a) && verify_reg(code, b);
                break;
            case RBC_BUILD_IMAG:
            case RBC_BUILD_SLICE:
                ok = verify_reg(code, a);
                if(op == RBC_BUILD_SLICE) ok = ok && verify_range(code, a, 3);
                break;
            case RBC_BUILD_TUPLE:
            case RBC_BUILD_LIST:
            case RBC_BUILD_STRING:
                ok = verify_reg(code, a) && verify_range(code, b, c);
                break;
            case RBC_BUILD_DICT:
                ok = verify_reg(code, a) && verify_range(code, b, c * 2u);
                break;
            case RBC_NEG:
            case RBC_NOT:
            case RBC_INVERT:
            case RBC_GET_ITER:
                ok = verify_reg(code, a) && verify_reg(code, b);
                break;
            case RBC_FOR_ITER:
            case RBC_FOR_GENERATOR:
            case RBC_FOR_ARRAY2D:
            case RBC_FOR_LIST:
            case RBC_FOR_TUPLE:
            case RBC_FOR_DICT:
            case RBC_FOR_RANGE:
            case RBC_FOR_STR: {
                int32_t target = pc + RBC_SBX(inst);
                ok = verify_range(code, a, 2) && target >= 0 && target < code->code.length;
                break;
            }
            case RBC_JUMP: {
                int32_t target = pc + RBC_SBX(inst);
                ok = target >= 0 && target < code->code.length;
                break;
            }
            case RBC_JUMP_IF_FALSE:
            case RBC_JUMP_IF_TRUE:
            case RBC_JUMP_IF_TRUE_OR_POP:
            case RBC_JUMP_IF_FALSE_OR_POP: {
                int32_t target = pc + RBC_SBX(inst);
                ok = verify_reg(code, a) && target >= 0 && target < code->code.length;
                break;
            }
            case RBC_SHORTCUT_IF_FALSE_OR_POP:
            case RBC_JUMP_IF_NOT_MATCH: {
                int32_t target = pc + RBC_SBX(inst);
                ok = verify_range(code, a, 2) && target >= 0 && target < code->code.length;
                break;
            }
            case RBC_CALL:
            case RBC_CALL_IC: {
                uint32_t window = 2u + b + c * 2u;
                ok = verify_range(code, a, window);
                break;
            }
            case RBC_ADD_I8:
            case RBC_SUB_I8:
            case RBC_MUL_I8:
            case RBC_CMP_LT_I8:
            case RBC_CMP_LE_I8:
            case RBC_CMP_EQ_I8:
            case RBC_CMP_NE_I8:
            case RBC_CMP_GT_I8:
            case RBC_CMP_GE_I8:
                ok = verify_reg(code, a) && verify_reg(code, b);
                break;
            case RBC_CMP_BRANCH:
            case RBC_CMP_I8_BRANCH: {
                if(!verify_reg(code, a) ||
                   (op == RBC_CMP_BRANCH && !verify_reg(code, b)) ||
                   (c & REG_CMP_BRANCH_CODE_MASK) > REG_CMP_GE || pc + 1 >= code->code.length) {
                    ok = false;
                    break;
                }
                RegBytecode ext = c11__getitem(RegBytecode, &code->code, pc + 1);
                if(RBC_OP(ext) != RBC_SUPER_DATA) {
                    ok = false;
                    break;
                }
                int32_t target = (pc + 1) + RBC_SBX(ext);
                ok = target >= 0 && target < code->code.length;
                break;
            }
            case RBC_SUPER_DATA:
                if(pc <= 0) {
                    ok = false;
                } else {
                    RegOpcode prev = RBC_OP(c11__getitem(RegBytecode, &code->code, pc - 1));
                    ok = prev == RBC_CMP_BRANCH || prev == RBC_CMP_I8_BRANCH;
                }
                break;
            case RBC_UNPACK_SEQUENCE:
                ok = verify_reg(code, b) && verify_range(code, a, c);
                break;
            case RBC_UNPACK_EX:
                ok = verify_reg(code, b) && verify_range(code, a, c + 1u);
                break;
            case RBC_RETURN: ok = verify_reg(code, a); break;
            case RBC_RETURN_NONE: break;
            default: ok = false; break;
        }

        if(!ok) {
            set_error(error,
                      REG_LOWER_INVALID_BYTECODE,
                      c11__getitem(int32_t, &code->source_ip, pc),
                      "invalid %s at register pc %d",
                      RegOpcode__name(op),
                      pc);
            return false;
        }
    }
    return true;
}

bool RegCode__lower(RegCode* out, const CodeObject* co, RegLowerError* error) {
    return lower_impl(out, co, 0, error);
}

bool RegCode__lower_function(RegCode* out, const FuncDecl* decl, RegLowerError* error) {
    if(decl == NULL) {
        set_error(error, REG_LOWER_INVALID_BYTECODE, -1, "null FuncDecl");
        return false;
    }
    uint64_t entry_bound = 0;
    c11__foreach(int32_t, &decl->args, index) {
        if(*index >= 0 && *index < 64) entry_bound |= UINT64_C(1) << *index;
    }
    c11__foreach(FuncDeclKwArg, &decl->kwargs, kv) {
        if(kv->index >= 0 && kv->index < 64) entry_bound |= UINT64_C(1) << kv->index;
    }
    if(decl->starred_arg >= 0 && decl->starred_arg < 64)
        entry_bound |= UINT64_C(1) << decl->starred_arg;
    if(decl->starred_kwarg >= 0 && decl->starred_kwarg < 64)
        entry_bound |= UINT64_C(1) << decl->starred_kwarg;
    return lower_impl(out, &decl->code, entry_bound, error);
}

const char* RegOpcode__name(RegOpcode op) {
    static const char* names[RBC__COUNT] = {
        [RBC_NOP] = "NOP",
        [RBC_MOVE] = "MOVE",
        [RBC_CHECK_BOUND] = "CHECK_BOUND",
        [RBC_LOAD_CONST] = "LOAD_CONST",
        [RBC_LOAD_NONE] = "LOAD_NONE",
        [RBC_LOAD_NIL] = "LOAD_NIL",
        [RBC_LOAD_ELLIPSIS] = "LOAD_ELLIPSIS",
        [RBC_LOAD_BOOL] = "LOAD_BOOL",
        [RBC_LOAD_I16] = "LOAD_I16",
        [RBC_LOAD_NAME_INT] = "LOAD_NAME_INT",
        [RBC_LOAD_GLOBAL] = "LOAD_GLOBAL",
        [RBC_LOAD_NONLOCAL] = "LOAD_NONLOCAL",
        [RBC_LOAD_FUNCTION] = "LOAD_FUNCTION",
        [RBC_LOAD_ATTR] = "LOAD_ATTR",
        [RBC_LOAD_METHOD] = "LOAD_METHOD",
        [RBC_LOAD_SUBSCR] = "LOAD_SUBSCR",
        [RBC_STORE_GLOBAL] = "STORE_GLOBAL",
        [RBC_STORE_ATTR] = "STORE_ATTR",
        [RBC_STORE_SUBSCR] = "STORE_SUBSCR",
        [RBC_DELETE_FAST] = "DELETE_FAST",
        [RBC_DELETE_GLOBAL] = "DELETE_GLOBAL",
        [RBC_DELETE_ATTR] = "DELETE_ATTR",
        [RBC_DELETE_SUBSCR] = "DELETE_SUBSCR",
        [RBC_BUILD_IMAG] = "BUILD_IMAG",
        [RBC_BUILD_BYTES] = "BUILD_BYTES",
        [RBC_BUILD_TUPLE] = "BUILD_TUPLE",
        [RBC_BUILD_LIST] = "BUILD_LIST",
        [RBC_BUILD_DICT] = "BUILD_DICT",
        [RBC_BUILD_SLICE] = "BUILD_SLICE",
        [RBC_BUILD_STRING] = "BUILD_STRING",
        [RBC_ADD] = "ADD",
        [RBC_SUB] = "SUB",
        [RBC_MUL] = "MUL",
        [RBC_TRUEDIV] = "TRUEDIV",
        [RBC_FLOORDIV] = "FLOORDIV",
        [RBC_MOD] = "MOD",
        [RBC_POW] = "POW",
        [RBC_LSHIFT] = "LSHIFT",
        [RBC_RSHIFT] = "RSHIFT",
        [RBC_BITAND] = "BITAND",
        [RBC_BITOR] = "BITOR",
        [RBC_BITXOR] = "BITXOR",
        [RBC_MATMUL] = "MATMUL",
        [RBC_CMP_LT] = "CMP_LT",
        [RBC_CMP_LE] = "CMP_LE",
        [RBC_CMP_EQ] = "CMP_EQ",
        [RBC_CMP_NE] = "CMP_NE",
        [RBC_CMP_GT] = "CMP_GT",
        [RBC_CMP_GE] = "CMP_GE",
        [RBC_IS] = "IS",
        [RBC_IS_NOT] = "IS_NOT",
        [RBC_CONTAINS] = "CONTAINS",
        [RBC_NOT_CONTAINS] = "NOT_CONTAINS",
        [RBC_NEG] = "NEG",
        [RBC_NOT] = "NOT",
        [RBC_INVERT] = "INVERT",
        [RBC_GET_ITER] = "GET_ITER",
        [RBC_FOR_ITER] = "FOR_ITER",
        [RBC_JUMP] = "JUMP",
        [RBC_JUMP_IF_FALSE] = "JUMP_IF_FALSE",
        [RBC_JUMP_IF_TRUE] = "JUMP_IF_TRUE",
        [RBC_JUMP_IF_TRUE_OR_POP] = "JUMP_IF_TRUE_OR_POP",
        [RBC_JUMP_IF_FALSE_OR_POP] = "JUMP_IF_FALSE_OR_POP",
        [RBC_SHORTCUT_IF_FALSE_OR_POP] = "SHORTCUT_IF_FALSE_OR_POP",
        [RBC_JUMP_IF_NOT_MATCH] = "JUMP_IF_NOT_MATCH",
        [RBC_CALL] = "CALL",
        [RBC_LIST_APPEND] = "LIST_APPEND",
        [RBC_DICT_ADD] = "DICT_ADD",
        [RBC_UNPACK_SEQUENCE] = "UNPACK_SEQUENCE",
        [RBC_UNPACK_EX] = "UNPACK_EX",
        [RBC_FORMAT_STRING] = "FORMAT_STRING",
        [RBC_RETURN] = "RETURN",
        [RBC_RETURN_NONE] = "RETURN_NONE",
        [RBC_ADD_INT] = "ADD_INT",
        [RBC_SUB_INT] = "SUB_INT",
        [RBC_MUL_INT] = "MUL_INT",
        [RBC_ADD_NUM] = "ADD_NUM",
        [RBC_SUB_NUM] = "SUB_NUM",
        [RBC_MUL_NUM] = "MUL_NUM",
        [RBC_CMP_LT_INT] = "CMP_LT_INT",
        [RBC_CMP_LE_INT] = "CMP_LE_INT",
        [RBC_CMP_EQ_INT] = "CMP_EQ_INT",
        [RBC_CMP_NE_INT] = "CMP_NE_INT",
        [RBC_CMP_GT_INT] = "CMP_GT_INT",
        [RBC_CMP_GE_INT] = "CMP_GE_INT",
        [RBC_CMP_LT_NUM] = "CMP_LT_NUM",
        [RBC_CMP_LE_NUM] = "CMP_LE_NUM",
        [RBC_CMP_EQ_NUM] = "CMP_EQ_NUM",
        [RBC_CMP_NE_NUM] = "CMP_NE_NUM",
        [RBC_CMP_GT_NUM] = "CMP_GT_NUM",
        [RBC_CMP_GE_NUM] = "CMP_GE_NUM",
        [RBC_LOAD_GLOBAL_IC] = "LOAD_GLOBAL_IC",
        [RBC_LOAD_ATTR_IC] = "LOAD_ATTR_IC",
        [RBC_LOAD_METHOD_IC] = "LOAD_METHOD_IC",
        [RBC_CALL_IC] = "CALL_IC",
        [RBC_FOR_GENERATOR] = "FOR_GENERATOR",
        [RBC_FOR_ARRAY2D] = "FOR_ARRAY2D",
        [RBC_FOR_LIST] = "FOR_LIST",
        [RBC_FOR_TUPLE] = "FOR_TUPLE",
        [RBC_FOR_DICT] = "FOR_DICT",
        [RBC_FOR_RANGE] = "FOR_RANGE",
        [RBC_FOR_STR] = "FOR_STR",
        [RBC_ADD_I8] = "ADD_I8",
        [RBC_SUB_I8] = "SUB_I8",
        [RBC_MUL_I8] = "MUL_I8",
        [RBC_CMP_LT_I8] = "CMP_LT_I8",
        [RBC_CMP_LE_I8] = "CMP_LE_I8",
        [RBC_CMP_EQ_I8] = "CMP_EQ_I8",
        [RBC_CMP_NE_I8] = "CMP_NE_I8",
        [RBC_CMP_GT_I8] = "CMP_GT_I8",
        [RBC_CMP_GE_I8] = "CMP_GE_I8",
        [RBC_CMP_BRANCH] = "CMP_BRANCH",
        [RBC_CMP_I8_BRANCH] = "CMP_I8_BRANCH",
        [RBC_SUPER_DATA] = "SUPER_DATA",
    };
    if((unsigned)op >= RBC__COUNT || names[op] == NULL) return "<invalid>";
    return names[op];
}

const char* RegLowerStatus__name(RegLowerStatus status) {
    switch(status) {
        case REG_LOWER_OK: return "ok";
        case REG_LOWER_UNSUPPORTED: return "unsupported";
        case REG_LOWER_INVALID_BYTECODE: return "invalid-bytecode";
        case REG_LOWER_STACK_MISMATCH: return "stack-mismatch";
        case REG_LOWER_TOO_MANY_REGISTERS: return "too-many-registers";
        case REG_LOWER_JUMP_TOO_FAR: return "jump-too-far";
        case REG_LOWER_OOM: return "out-of-memory";
        default: return "unknown";
    }
}
