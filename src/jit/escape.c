/*
 * Escape analysis + scalar replacement over RegVM register bytecode.
 * See include/tinypy/jit/escape.h for the model and its guarantees.
 */
#include "tinypy/jit/escape.h"

#include "tinypy/common/utils.h"
#include "tinypy/interpreter/regprofile.h"
#include "tinypy/interpreter/regvm.h"
#include "tinypy/interpreter/typeinfo.h"
#include "tinypy/interpreter/vm.h"
#include "tinypy/objects/object.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Register sets                                                             */
/* ------------------------------------------------------------------------ */

static void regs_add(JitEaRegs* s, unsigned r) { s->bits[r >> 6] |= UINT64_C(1) << (r & 63); }
static void regs_del(JitEaRegs* s, unsigned r) { s->bits[r >> 6] &= ~(UINT64_C(1) << (r & 63)); }

static void regs_add_range(JitEaRegs* s, unsigned base, unsigned count) {
    for(unsigned i = 0; i < count && base + i < 256; i++) regs_add(s, base + i);
}

static bool regs_empty(const JitEaRegs* s) {
    return (s->bits[0] | s->bits[1] | s->bits[2] | s->bits[3]) == 0;
}

static bool regs_intersect(const JitEaRegs* a, const JitEaRegs* b) {
    for(int i = 0; i < 4; i++) {
        if(a->bits[i] & b->bits[i]) return true;
    }
    return false;
}

static void regs_minus(JitEaRegs* a, const JitEaRegs* b) {
    for(int i = 0; i < 4; i++) a->bits[i] &= ~b->bits[i];
}

/* ------------------------------------------------------------------------ */
/* Per-opcode register effects (conservative)                                */
/*                                                                           */
/* `reads` over-approximates, `writes` under-approximates (only registers    */
/* every execution writes), so liveness built on them is always safe. An     */
/* opcode without a description reads every register.                        */
/* ------------------------------------------------------------------------ */

typedef struct EaEffects {
    JitEaRegs reads;
    JitEaRegs writes;     /* written by every execution */
    JitEaRegs may_writes; /* written by some execution (superset of writes) */
    bool reads_all;
} EaEffects;

static RegBytecode code_at(const RegCode* code, int pc) {
    return c11__getitem(RegBytecode, &code->code, pc);
}

static void effects(const RegCode* code, int pc, EaEffects* e) {
    memset(e, 0, sizeof(*e));
    const RegBytecode inst = code_at(code, pc);
    const unsigned a = RBC_A(inst), b = RBC_B(inst), c = RBC_C(inst);
    switch(RBC_OP(inst)) {
        case RBC_NOP: case RBC_SUPER_DATA: case RBC_JUMP: case RBC_RETURN_NONE:
        case RBC_DELETE_GLOBAL:
            break;
        case RBC_MOVE: regs_add(&e->reads, b); regs_add(&e->writes, a); break;
        case RBC_CHECK_BOUND: case RBC_STORE_GLOBAL: case RBC_DELETE_ATTR: case RBC_RETURN:
        case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP: case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_CMP_I8_BRANCH:
            regs_add(&e->reads, a);
            break;
        case RBC_SHORTCUT_IF_FALSE_OR_POP: regs_add(&e->reads, a + 1); break;
        case RBC_JUMP_IF_NOT_MATCH: case RBC_STORE_ATTR: case RBC_STORE_ATTR_IC:
            regs_add(&e->reads, a);
            regs_add(&e->reads, a + 1);
            break;
        case RBC_CMP_BRANCH: regs_add(&e->reads, a); regs_add(&e->reads, b); break;
        case RBC_LOAD_CONST: case RBC_LOAD_NONE: case RBC_LOAD_NIL: case RBC_LOAD_ELLIPSIS:
        case RBC_LOAD_BOOL: case RBC_LOAD_I16: case RBC_LOAD_NAME_INT:
        case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
        case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
        case RBC_BUILD_IMAG: case RBC_BUILD_BYTES:
            regs_add(&e->writes, a);
            break;
        case RBC_LOAD_FUNCTION:
            /* A nested function captures the named locals by value. */
            regs_add_range(&e->reads, 0, 256);
            regs_add(&e->writes, a);
            break;
        case RBC_LOAD_ATTR: case RBC_LOAD_ATTR_IC: case RBC_FORMAT_STRING:
            regs_add(&e->reads, a);
            regs_add(&e->writes, a);
            break;
        case RBC_LOAD_METHOD: case RBC_LOAD_METHOD_IC:
            regs_add(&e->reads, a);
            regs_add(&e->writes, a);
            regs_add(&e->writes, a + 1);
            break;
        case RBC_LOAD_SUBSCR:
            regs_add(&e->reads, b);
            regs_add(&e->reads, c);
            regs_add(&e->writes, a);
            break;
        case RBC_STORE_SUBSCR: case RBC_DICT_ADD:
            regs_add(&e->reads, a);
            regs_add(&e->reads, b);
            regs_add(&e->reads, c);
            break;
        case RBC_DELETE_FAST: regs_add(&e->reads, a); regs_add(&e->writes, a); break;
        case RBC_DELETE_SUBSCR: case RBC_LIST_APPEND:
            regs_add(&e->reads, a);
            regs_add(&e->reads, b);
            break;
        case RBC_BUILD_TUPLE: case RBC_BUILD_LIST: case RBC_BUILD_STRING:
            regs_add_range(&e->reads, b, c);
            regs_add(&e->writes, a);
            break;
        case RBC_BUILD_DICT:
            regs_add_range(&e->reads, b, c * 2u);
            regs_add(&e->writes, a);
            break;
        case RBC_BUILD_SLICE:
            regs_add_range(&e->reads, a, 3);
            regs_add(&e->writes, a);
            break;
        case RBC_ADD: case RBC_SUB: case RBC_MUL: case RBC_TRUEDIV: case RBC_FLOORDIV:
        case RBC_MOD: case RBC_POW: case RBC_LSHIFT: case RBC_RSHIFT: case RBC_BITAND:
        case RBC_BITOR: case RBC_BITXOR: case RBC_MATMUL:
        case RBC_CMP_LT: case RBC_CMP_LE: case RBC_CMP_EQ: case RBC_CMP_NE:
        case RBC_CMP_GT: case RBC_CMP_GE:
        case RBC_IS: case RBC_IS_NOT: case RBC_CONTAINS: case RBC_NOT_CONTAINS:
        case RBC_ADD_INT: case RBC_SUB_INT: case RBC_MUL_INT:
        case RBC_ADD_NUM: case RBC_SUB_NUM: case RBC_MUL_NUM:
        case RBC_CMP_LT_INT: case RBC_CMP_LE_INT: case RBC_CMP_EQ_INT:
        case RBC_CMP_NE_INT: case RBC_CMP_GT_INT: case RBC_CMP_GE_INT:
        case RBC_CMP_LT_NUM: case RBC_CMP_LE_NUM: case RBC_CMP_EQ_NUM:
        case RBC_CMP_NE_NUM: case RBC_CMP_GT_NUM: case RBC_CMP_GE_NUM:
            regs_add(&e->reads, b);
            regs_add(&e->reads, c);
            regs_add(&e->writes, a);
            break;
        case RBC_ADD_I8: case RBC_SUB_I8: case RBC_MUL_I8:
        case RBC_CMP_LT_I8: case RBC_CMP_LE_I8: case RBC_CMP_EQ_I8:
        case RBC_CMP_NE_I8: case RBC_CMP_GT_I8: case RBC_CMP_GE_I8:
        case RBC_NEG: case RBC_NOT: case RBC_INVERT: case RBC_GET_ITER:
            regs_add(&e->reads, b);
            regs_add(&e->writes, a);
            break;
        case RBC_FOR_ITER: case RBC_FOR_GENERATOR: case RBC_FOR_ARRAY2D: case RBC_FOR_LIST:
        case RBC_FOR_TUPLE: case RBC_FOR_DICT: case RBC_FOR_RANGE: case RBC_FOR_STR:
            /* A+1 is only written when an item is produced. */
            regs_add(&e->reads, a);
            break;
        case RBC_CALL: case RBC_CALL_IC:
            regs_add_range(&e->reads, a, 2u + b + 2u * c);
            regs_add(&e->writes, a);
            break;
        case RBC_UNPACK_SEQUENCE:
            regs_add(&e->reads, b);
            regs_add_range(&e->writes, a, c);
            break;
        case RBC_UNPACK_EX: regs_add(&e->reads, b); break;
        default: e->reads_all = true; break;
    }
    e->may_writes = e->writes;
    switch(RBC_OP(inst)) {
        case RBC_FOR_ITER: case RBC_FOR_GENERATOR: case RBC_FOR_ARRAY2D: case RBC_FOR_LIST:
        case RBC_FOR_TUPLE: case RBC_FOR_DICT: case RBC_FOR_RANGE: case RBC_FOR_STR:
            regs_add(&e->may_writes, a + 1);
            break;
        case RBC_SHORTCUT_IF_FALSE_OR_POP: regs_add(&e->may_writes, a); break;
        case RBC_UNPACK_EX: regs_add_range(&e->may_writes, a, c + 1u); break;
        default:
            if(e->reads_all) regs_add_range(&e->may_writes, 0, 256);
            break;
    }
}

/* Writes `reg` on some but not every execution. */
static bool conditional_write(const EaEffects* e, unsigned reg) {
    return JitEaRegs__has(&e->may_writes, reg) && !JitEaRegs__has(&e->writes, reg);
}

/* Successor PCs; returns their count (0 for returns). */
static int successors(const RegCode* code, int pc, int out[2]) {
    const RegBytecode inst = code_at(code, pc);
    switch(RBC_OP(inst)) {
        case RBC_RETURN: case RBC_RETURN_NONE: return 0;
        case RBC_JUMP: out[0] = pc + RBC_SBX(inst); return 1;
        case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP: case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_SHORTCUT_IF_FALSE_OR_POP: case RBC_JUMP_IF_NOT_MATCH:
        case RBC_FOR_ITER: case RBC_FOR_GENERATOR: case RBC_FOR_ARRAY2D: case RBC_FOR_LIST:
        case RBC_FOR_TUPLE: case RBC_FOR_DICT: case RBC_FOR_RANGE: case RBC_FOR_STR:
            out[0] = pc + 1;
            out[1] = pc + RBC_SBX(inst);
            return 2;
        case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: {
            const RegBytecode ext = code_at(code, pc + 1);
            out[0] = pc + 1 + RBC_SBX(ext);
            out[1] = pc + 2 + RBC_A(ext);
            return 2;
        }
        default: out[0] = pc + 1; return 1;
    }
}

static bool is_terminator(RegOpcode op) {
    switch(op) {
        case RBC_RETURN: case RBC_RETURN_NONE: case RBC_JUMP:
        case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP: case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_SHORTCUT_IF_FALSE_OR_POP: case RBC_JUMP_IF_NOT_MATCH:
        case RBC_FOR_ITER: case RBC_FOR_GENERATOR: case RBC_FOR_ARRAY2D: case RBC_FOR_LIST:
        case RBC_FOR_TUPLE: case RBC_FOR_DICT: case RBC_FOR_RANGE: case RBC_FOR_STR:
        case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH:
            return true;
        default: return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Analysis context                                                          */
/* ------------------------------------------------------------------------ */

typedef struct EaCtx {
    const RegCode* code;
    const CodeObject* co;
    int length;
    uint8_t* block_start; /* jump targets, loop headers, PC 0 */
    uint8_t* is_payload;  /* SUPER_DATA words */
    uint8_t* visited;     /* DFS scratch */
    int* stack;           /* DFS scratch */
} EaCtx;

static bool ctx_init(EaCtx* ctx, const RegCode* code, const CodeObject* co) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->code = code;
    ctx->co = co;
    ctx->length = code->code.length;
    ctx->block_start = calloc((size_t)ctx->length + 1, 1);
    ctx->is_payload = calloc((size_t)ctx->length + 1, 1);
    ctx->visited = calloc((size_t)ctx->length + 1, 1);
    ctx->stack = malloc(sizeof(int) * ((size_t)ctx->length * 2 + 2));
    if(!ctx->block_start || !ctx->is_payload || !ctx->visited || !ctx->stack) return false;
    ctx->block_start[0] = 1;
    for(int pc = 0; pc < ctx->length; pc++) {
        const RegOpcode op = RBC_OP(code_at(code, pc));
        if(op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH) {
            if(pc + 1 >= ctx->length || RBC_OP(code_at(code, pc + 1)) != RBC_SUPER_DATA) return false;
            ctx->is_payload[pc + 1] = 1;
        }
        if(!is_terminator(op) || ctx->is_payload[pc]) continue;
        int succ[2];
        const int n = successors(code, pc, succ);
        for(int i = 0; i < n; i++) {
            if(succ[i] < 0 || succ[i] >= ctx->length) return false;
            ctx->block_start[succ[i]] = 1;
        }
    }
    c11__foreach(RegLoopProfile, &code->loop_profiles, loop) {
        if(loop->header_pc >= 0 && loop->header_pc < ctx->length) ctx->block_start[loop->header_pc] = 1;
    }
    return true;
}

static void ctx_dtor(EaCtx* ctx) {
    free(ctx->block_start);
    free(ctx->is_payload);
    free(ctx->visited);
    free(ctx->stack);
}

/* True when some path from any of `starts` reads `reg` before writing it. */
static bool reg_live(EaCtx* ctx, const int* starts, int nstarts, unsigned reg) {
    memset(ctx->visited, 0, (size_t)ctx->length);
    int top = 0;
    for(int i = 0; i < nstarts; i++) ctx->stack[top++] = starts[i];
    while(top > 0) {
        const int pc = ctx->stack[--top];
        if(pc < 0 || pc >= ctx->length) return true; /* malformed: be conservative */
        if(ctx->visited[pc]) continue;
        ctx->visited[pc] = 1;
        EaEffects e;
        effects(ctx->code, pc, &e);
        if(e.reads_all || JitEaRegs__has(&e.reads, reg)) return true;
        if(JitEaRegs__has(&e.writes, reg)) continue;
        int succ[2];
        const int n = successors(ctx->code, pc, succ);
        for(int i = 0; i < n; i++) ctx->stack[top++] = succ[i];
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Simple constructors                                                       */
/* ------------------------------------------------------------------------ */

typedef enum EaValueKind { EA_UNKNOWN = 0, EA_PARAM, EA_CONST } EaValueKind;

typedef struct EaValue {
    uint8_t kind;
    uint8_t param;
    py_TValue constant;
} EaValue;

static bool scalar_constant(const py_TValue* value) {
    return !value->is_ptr; /* int/float/bool/None: no heap object to keep alive */
}

/*
 * __init__(self, a, b, ...) whose body only stores arguments or scalar
 * constants into fields of self, then returns None.
 */
static bool analyze_init(FuncDecl* decl, int argc, JitEaVirtual* v) {
    if(decl == NULL || decl->type != FuncType_SIMPLE || decl->args.length != argc + 1) return false;
    RegCode* init = RegVM__ensure_code(decl);
    if(init == NULL || init->nregs == 0) return false;
    EaValue* regs = calloc(init->nregs, sizeof(EaValue));
    if(regs == NULL) return false;
    for(int i = 0; i <= argc && i < init->nregs; i++) {
        regs[i].kind = EA_PARAM;
        regs[i].param = (uint8_t)i;
    }
    bool ok = false;
    v->field_count = 0;
    for(int pc = 0; pc < init->code.length; pc++) {
        const RegBytecode inst = code_at(init, pc);
        const unsigned a = RBC_A(inst), b = RBC_B(inst);
        switch(RBC_OP(inst)) {
            case RBC_NOP: continue;
            case RBC_CHECK_BOUND:
                if(a >= init->nregs || regs[a].kind == EA_UNKNOWN) goto done;
                continue;
            case RBC_MOVE:
                if(a >= init->nregs || b >= init->nregs) goto done;
                regs[a] = regs[b];
                continue;
            case RBC_LOAD_NONE:
            case RBC_LOAD_BOOL:
            case RBC_LOAD_I16:
            case RBC_LOAD_CONST: {
                if(a >= init->nregs) goto done;
                py_TValue value = {0};
                const RegOpcode op = RBC_OP(inst);
                if(op == RBC_LOAD_NONE) {
                    py_newnone(&value);
                } else if(op == RBC_LOAD_BOOL) {
                    py_newbool(&value, b != 0);
                } else if(op == RBC_LOAD_I16) {
                    py_newint(&value, (int16_t)RBC_BX(inst));
                } else {
                    if(RBC_BX(inst) >= decl->code.consts.length) goto done;
                    value = c11__getitem(py_TValue, &decl->code.consts, RBC_BX(inst));
                }
                if(!scalar_constant(&value)) {
                    regs[a].kind = EA_UNKNOWN;
                    continue;
                }
                regs[a].kind = EA_CONST;
                regs[a].constant = value;
                continue;
            }
            case RBC_STORE_ATTR:
            case RBC_STORE_ATTR_IC: {
                if(a + 1u >= init->nregs || RBC_BX(inst) >= decl->code.names.length) goto done;
                const EaValue* target = &regs[a + 1];
                const EaValue* value = &regs[a];
                if(target->kind != EA_PARAM || target->param != 0) goto done;
                if(value->kind == EA_UNKNOWN || (value->kind == EA_PARAM && value->param == 0))
                    goto done; /* self stored into itself would escape */
                const py_Name name = c11__getitem(py_Name, &decl->code.names, RBC_BX(inst));
                JitEaField* field = NULL;
                for(int i = 0; i < v->field_count; i++) {
                    if(v->fields[i].name == name) field = &v->fields[i];
                }
                if(field == NULL) {
                    if(v->field_count >= JIT_EA_MAX_FIELDS) goto done;
                    field = &v->fields[v->field_count++];
                    field->name = name;
                }
                if(value->kind == EA_PARAM) {
                    field->component = (int8_t)(value->param - 1);
                    py_newnil(&field->constant);
                } else {
                    field->component = -1;
                    field->constant = value->constant;
                }
                continue;
            }
            case RBC_RETURN_NONE: ok = true; goto done;
            default: goto done;
        }
    }
done:
    free(regs);
    return ok;
}

bool JitEa__object_valid(const JitEaVirtual* v) {
    VM* vm = pk_current_vm;
    py_TypeInfo* ti = pk_typeinfo(v->type);
    if(ti->magics_version != vm->type_version) pk_tpresolvemagics(ti);
    if(!ti->is_python || ti->getattribute != NULL || ti->setattribute != NULL) return false;
    if(ti->cached_new.type != tp_nativefunc || ti->cached_new._cfunc != pk__object_new) return false;
    if(ti->cached_init.type != tp_function) return false;
    const Function* init = py_touserdata(&ti->cached_init);
    if(init->decl != v->init_decl || init->cfunc != NULL) return false;
    for(int i = 0; i < v->field_count; i++) {
        py_Ref cls_var = pk_tpfindname(ti, v->fields[i].name);
        if(cls_var != NULL && cls_var->type == tp_property) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Candidate allocations                                                     */
/* ------------------------------------------------------------------------ */

static bool candidate(EaCtx* ctx, int pc, JitEaVirtual* v) {
    const RegCode* code = ctx->code;
    const RegBytecode inst = code_at(code, pc);
    const unsigned a = RBC_A(inst), b = RBC_B(inst), c = RBC_C(inst);
    memset(v, 0, sizeof(*v));
    v->build_pc = pc;
    v->last_use_pc = -1;
    v->holding_reg = -1;
    v->result_reg = (uint8_t)a;
    switch(RBC_OP(inst)) {
        case RBC_BUILD_TUPLE:
        case RBC_BUILD_LIST:
            if(c == 0 || c > JIT_EA_MAX_COMPONENTS || (a >= b && a < b + c)) return false;
            v->kind = RBC_OP(inst) == RBC_BUILD_TUPLE ? JIT_EA_TUPLE : JIT_EA_LIST;
            v->component_base = (uint8_t)b;
            v->component_count = (uint8_t)c;
            return true;
        case RBC_CALL:
        case RBC_CALL_IC: {
            if(c != 0 || b > JIT_EA_MAX_COMPONENTS) return false;
            const RegRuntimeSite* site = c11__at(RegRuntimeSite, &code->runtime_sites, pc);
            if(site->cache_index == REG_IC_NONE ||
               site->cache_index >= (uint32_t)code->inline_caches.length)
                return false;
            const RegInlineCache* cache = c11__at(RegInlineCache, &code->inline_caches,
                                                  (int)site->cache_index);
            if(cache->kind != REG_IC_KIND_CALL || cache->variant != REG_CALL_CACHE_CONSTRUCT ||
               cache->argc != b)
                return false;
            v->kind = JIT_EA_OBJECT;
            v->type = cache->type;
            /*
             * The cached decl pointer is only an identity: read the live
             * __init__ through the (immortal) class and require it to match.
             */
            py_TypeInfo* ti = pk_typeinfo(cache->type);
            if(ti->magics_version != pk_current_vm->type_version) pk_tpresolvemagics(ti);
            if(ti->cached_init.type != tp_function) return false;
            const Function* init = py_touserdata(&ti->cached_init);
            if((uintptr_t)init->decl != cache->guard1) return false;
            v->init_decl = init->decl;
            v->component_base = (uint8_t)(a + 2);
            v->component_count = (uint8_t)b;
            if(!analyze_init(v->init_decl, (int)b, v)) return false;
            for(int i = 0; i < v->field_count; i++) {
                if(v->fields[i].component >= (int)b) return false;
            }
            return JitEa__object_valid(v);
        }
        default: return false;
    }
}

static int field_index(const JitEaVirtual* v, py_Name name) {
    for(int i = 0; i < v->field_count; i++) {
        if(v->fields[i].name == name) return i;
    }
    return -1;
}

static bool push_point(JitEaVirtual* v, int pc, const JitEaRegs* regs) {
    JitEaPoint point;
    point.pc = pc;
    point.regs = *regs;
    c11_vector__push(JitEaPoint, &v->points, point);
    return true;
}

static int compare_points(const void* lhs, const void* rhs) {
    const JitEaPoint* a = lhs;
    const JitEaPoint* b = rhs;
    return (a->pc > b->pc) - (a->pc < b->pc);
}

/* Instructions that can neither exit native code nor run Python code. */
static bool quiet(RegOpcode op) {
    switch(op) {
        case RBC_NOP: case RBC_MOVE: case RBC_LOAD_I16: case RBC_LOAD_CONST:
        case RBC_LOAD_NONE: case RBC_LOAD_NIL: case RBC_LOAD_BOOL:
            return true;
        default: return false;
    }
}

/*
 * Follow the object from its allocation. Fills v->points / v->kills and the
 * per-PC actions in `sites` (only committed by the caller on success).
 */
static bool follow(EaCtx* ctx, JitEaVirtual* v, JitEaSite* sites, int vindex) {
    const RegCode* code = ctx->code;
    const int nlocals = ctx->co->nlocals;
    JitEaRegs aliases = {{0}};
    regs_add(&aliases, v->result_reg);

    /* Known small-int registers in this block (LOAD_SUBSCR constant indices). */
    int16_t konst[256];
    uint8_t known[256];
    memset(known, 0, sizeof(known));
    int block = v->build_pc;
    while(block > 0 && !ctx->block_start[block]) block--;
    for(int pc = block; pc < v->build_pc; pc++) {
        if(ctx->is_payload[pc]) continue;
        EaEffects e;
        effects(code, pc, &e);
        for(unsigned r = 0; r < 256; r++) {
            if(JitEaRegs__has(&e.may_writes, r)) known[r] = 0;
        }
        const RegBytecode inst = code_at(code, pc);
        if(RBC_OP(inst) == RBC_LOAD_I16) {
            known[RBC_A(inst)] = 1;
            konst[RBC_A(inst)] = (int16_t)RBC_BX(inst);
        }
    }
    known[v->result_reg] = 0;

    int stop_succ[2];
    int nstop = 0;
    int pc = v->build_pc + 1;
    for(;; pc++) {
        if(pc >= ctx->length) return false;
        if(ctx->block_start[pc]) {
            stop_succ[0] = pc;
            nstop = 1;
            break;
        }
        push_point(v, pc, &aliases);
        const RegBytecode inst = code_at(code, pc);
        const RegOpcode op = RBC_OP(inst);
        const unsigned a = RBC_A(inst), b = RBC_B(inst), c = RBC_C(inst);
        EaEffects e;
        effects(code, pc, &e);
        bool handled = false;

        if(op == RBC_MOVE && JitEaRegs__has(&aliases, b)) {
            regs_add(&aliases, a); /* plain register copy: both name the object */
            handled = true;
        } else if(op == RBC_UNPACK_SEQUENCE && JitEaRegs__has(&aliases, b) &&
                  v->kind != JIT_EA_OBJECT && c == v->component_count) {
            sites[pc] = (JitEaSite){JIT_EA_UNPACK, (uint8_t)vindex, 0};
            v->last_use_pc = pc;
            regs_minus(&aliases, &e.writes);
            handled = true;
        } else if(op == RBC_LOAD_SUBSCR && JitEaRegs__has(&aliases, b) &&
                  !JitEaRegs__has(&aliases, c) && v->kind != JIT_EA_OBJECT && known[c]) {
            int index = konst[c];
            if(index < 0) index += v->component_count;
            if(index >= 0 && index < v->component_count) {
                sites[pc] = (JitEaSite){JIT_EA_SUBSCR, (uint8_t)vindex, (uint8_t)index};
                v->last_use_pc = pc;
                regs_del(&aliases, a);
                handled = true;
            }
        } else if((op == RBC_LOAD_ATTR || op == RBC_LOAD_ATTR_IC) &&
                  JitEaRegs__has(&aliases, a) && v->kind == JIT_EA_OBJECT &&
                  RBC_BX(inst) < ctx->co->names.length) {
            const int f = field_index(v, c11__getitem(py_Name, &ctx->co->names, RBC_BX(inst)));
            if(f >= 0) {
                sites[pc] = (JitEaSite){JIT_EA_ATTR, (uint8_t)vindex, (uint8_t)f};
                v->last_use_pc = pc;
                regs_del(&aliases, a);
                handled = true;
            }
        }
        if(!handled) {
            if(e.reads_all || regs_intersect(&e.reads, &aliases)) return false; /* escapes */
            /*
             * A register that may or may not be overwritten cannot be tracked:
             * materializing into it later could clobber a live value.
             */
            for(unsigned r = 0; r < 256; r++) {
                if(JitEaRegs__has(&aliases, r) && conditional_write(&e, r)) return false;
            }
            regs_minus(&aliases, &e.writes);
        }
        for(unsigned r = 0; r < 256; r++) {
            if(JitEaRegs__has(&e.may_writes, r)) known[r] = 0;
        }
        if(op == RBC_LOAD_I16) {
            known[a] = 1;
            konst[a] = (int16_t)RBC_BX(inst);
        }
        if(regs_empty(&aliases)) {
            nstop = 0;
            break;
        }
        if(is_terminator(op)) {
            nstop = successors(code, pc, stop_succ);
            break;
        }
    }

    /* Registers still naming the object leave the block: they must be dead. */
    JitEaRegs held = {{0}}; /* named locals among them */
    for(unsigned r = 0; r < 256; r++) {
        if(!JitEaRegs__has(&aliases, r)) continue;
        if(nstop > 0 && reg_live(ctx, stop_succ, nstop, r)) return false;
        if((int)r < nlocals) regs_add(&held, r);
    }

    /*
     * Past the last use only named locals still matter (frame introspection
     * can see them); dead temporaries are never read again.
     */
    c11__foreach(JitEaPoint, &v->points, point) {
        if(point->pc <= v->last_use_pc) continue;
        JitEaRegs kept = {{0}};
        for(unsigned r = 0; r < (unsigned)nlocals && r < 256; r++) {
            if(JitEaRegs__has(&point->regs, r)) regs_add(&kept, r);
        }
        point->regs = kept;
    }

    if(!regs_empty(&held) && nstop > 0) {
        /*
         * Holding range: from the block exit until every named alias has been
         * overwritten. The set of aliases still alive must not depend on the
         * path that reached a PC; the object stops being pending right before
         * the write that kills its last alias.
         */
        for(unsigned r = 0; r < 256; r++) {
            if(JitEaRegs__has(&held, r)) {
                v->holding_reg = (int16_t)r;
                break;
            }
        }
        JitEaRegs* state = calloc((size_t)ctx->length, sizeof(JitEaRegs));
        if(state == NULL) return false;
        memset(ctx->visited, 0, (size_t)ctx->length);
        int top = 0;
        for(int i = 0; i < nstop; i++) {
            const int at = stop_succ[i];
            if(at < 0 || at >= ctx->length) {
                free(state);
                return false;
            }
            if(ctx->visited[at] && memcmp(&state[at], &held, sizeof(held)) != 0) {
                free(state);
                return false;
            }
            state[at] = held;
            ctx->visited[at] = 1;
            ctx->stack[top++] = at;
        }
        bool reaches_build = false;
        bool ok = true;
        while(ok && top > 0) {
            const int at = ctx->stack[--top];
            const JitEaRegs live = state[at];
            EaEffects e;
            effects(code, at, &e);
            JitEaRegs after = live;
            for(unsigned r = 0; r < 256; r++) {
                if(!JitEaRegs__has(&live, r)) continue;
                if(conditional_write(&e, r)) ok = false;
                if(JitEaRegs__has(&e.writes, r)) regs_del(&after, r);
            }
            if(!ok) break;
            if(regs_empty(&after)) {
                /* The last alias dies here: clear the pending bit before it. */
                c11_vector__push(int32_t, &v->kills, at);
                continue;
            }
            push_point(v, at, &live);
            if(at == v->build_pc) {
                /* Rebuilt while the previous object is still named: the old one
                 * must be overwritten before anything can observe the new save area. */
                reaches_build = true;
                continue;
            }
            int succ[2];
            const int n = successors(code, at, succ);
            for(int i = 0; i < n; i++) {
                const int next = succ[i];
                if(next < 0 || next >= ctx->length) {
                    ok = false;
                    break;
                }
                if(ctx->visited[next]) {
                    if(memcmp(&state[next], &after, sizeof(after)) != 0) ok = false;
                    continue;
                }
                ctx->visited[next] = 1;
                state[next] = after;
                ctx->stack[top++] = next;
            }
        }
        JitEaRegs old = {{0}};
        if(ok && reaches_build) old = state[v->build_pc];
        free(state);
        if(!ok) return false;
        if(reaches_build) {
            for(int at = v->build_pc + 1; at < ctx->length && !ctx->block_start[at]; at++) {
                const RegOpcode op = RBC_OP(code_at(code, at));
                if(!quiet(op)) break;
                EaEffects e;
                effects(code, at, &e);
                regs_minus(&old, &e.writes);
                if(regs_empty(&old)) break;
            }
            if(!regs_empty(&old)) return false;
        }
    }
    qsort(v->points.data, (size_t)v->points.length, sizeof(JitEaPoint), compare_points);
    /* A PC can be reached both inside the block and through the holding range
     * only as the build itself; anything else would be ambiguous. */
    for(int i = 1; i < v->points.length; i++) {
        const JitEaPoint* prev = c11__at(JitEaPoint, &v->points, i - 1);
        const JitEaPoint* cur = c11__at(JitEaPoint, &v->points, i);
        if(prev->pc == cur->pc) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Driver                                                                    */
/* ------------------------------------------------------------------------ */

static void virtual_dtor(JitEaVirtual* v) {
    c11_vector__dtor(&v->points);
    c11_vector__dtor(&v->kills);
}

void JitEa__analyze(const RegCode* code, const CodeObject* co, JitEaResult* out) {
    memset(out, 0, sizeof(*out));
    out->site_count = code->code.length;
    out->sites = calloc((size_t)code->code.length + 1, sizeof(JitEaSite));
    if(out->sites == NULL) return;
    EaCtx ctx;
    if(!ctx_init(&ctx, code, co)) {
        ctx_dtor(&ctx);
        return;
    }
    JitEaSite* scratch = calloc((size_t)code->code.length + 1, sizeof(JitEaSite));
    if(scratch == NULL) {
        ctx_dtor(&ctx);
        return;
    }
    for(int pc = 0; pc < ctx.length && out->virtual_count < JIT_EA_MAX_VIRTUALS; pc++) {
        if(ctx.is_payload[pc] || out->sites[pc].action != JIT_EA_NONE) continue;
        JitEaVirtual* v = &out->virtuals[out->virtual_count];
        if(!candidate(&ctx, pc, v)) continue;
        if(out->save_count + v->component_count > JIT_EA_MAX_SAVE) continue;
        c11_vector__ctor(&v->points, sizeof(JitEaPoint));
        c11_vector__ctor(&v->kills, sizeof(int32_t));
        memset(scratch, 0, sizeof(JitEaSite) * (size_t)ctx.length);
        bool ok = follow(&ctx, v, scratch, out->virtual_count);
        /* A use already claimed by another object (should not happen) is a conflict. */
        for(int i = 0; ok && i < ctx.length; i++) {
            if(scratch[i].action != JIT_EA_NONE && out->sites[i].action != JIT_EA_NONE) ok = false;
        }
        if(!ok) {
            virtual_dtor(v);
            continue;
        }
        for(int i = 0; i < ctx.length; i++) {
            if(scratch[i].action != JIT_EA_NONE) out->sites[i] = scratch[i];
        }
        out->sites[pc] = (JitEaSite){JIT_EA_BUILD, (uint8_t)out->virtual_count, 0};
        v->save_offset = out->save_count;
        out->save_count = (uint16_t)(out->save_count + v->component_count);
        if(v->kind == JIT_EA_OBJECT) PK_INCREF(v->init_decl);
        out->virtual_count++;
    }
    free(scratch);
    ctx_dtor(&ctx);
}

void JitEa__dtor(JitEaResult* self) {
    for(int i = 0; i < self->virtual_count; i++) {
        JitEaVirtual* v = &self->virtuals[i];
        if(v->kind == JIT_EA_OBJECT && v->init_decl != NULL) PK_DECREF(v->init_decl);
        virtual_dtor(v);
    }
    free(self->sites);
    memset(self, 0, sizeof(*self));
}

const JitEaRegs* JitEa__points_at(const JitEaVirtual* v, int32_t pc) {
    int lo = 0, hi = v->points.length - 1;
    while(lo <= hi) {
        const int mid = (lo + hi) / 2;
        const JitEaPoint* point = c11__at(JitEaPoint, &v->points, mid);
        if(point->pc == pc) return &point->regs;
        if(point->pc < pc) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}
