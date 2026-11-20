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
bool JitEa__analyze_init(FuncDecl* decl, int argc, JitEaVirtual* v) {
    if(decl == NULL || argc < 0 || argc > JIT_EA_MAX_COMPONENTS ||
       decl->type != FuncType_SIMPLE || decl->args.length != argc + 1) return false;
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
    v->result_reg = (uint8_t)a;
    switch(RBC_OP(inst)) {
        case RBC_BUILD_TUPLE:
        case RBC_BUILD_LIST:
            if(c == 0 || c > JIT_EA_MAX_COMPONENTS || (a >= b && a < b + c)) return false;
            v->kind = RBC_OP(inst) == RBC_BUILD_TUPLE ? JIT_EA_TUPLE : JIT_EA_LIST;
            v->component_base = (uint8_t)b;
            v->component_count = (uint8_t)c;
            v->slot_count = (uint8_t)c;
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
            if(!JitEa__analyze_init(v->init_decl, (int)b, v)) return false;
            for(int i = 0; i < v->field_count; i++) {
                if(v->fields[i].component >= (int)b) return false;
            }
            v->slot_count = v->field_count;
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
 * The small int `reg` holds right before `pc`, when its last write in the same
 * basic block is a LOAD_I16.
 */
static bool known_int(const EaCtx* ctx, int pc, unsigned reg, int* value) {
    if(ctx->block_start[pc]) return false; /* the previous instruction is another path */
    for(int at = pc - 1; at >= 0; at--) {
        if(!ctx->is_payload[at]) {
            EaEffects e;
            effects(ctx->code, at, &e);
            if(JitEaRegs__has(&e.may_writes, reg)) {
                const RegBytecode inst = code_at(ctx->code, at);
                if(RBC_OP(inst) != RBC_LOAD_I16 || RBC_A(inst) != reg) return false;
                *value = (int16_t)RBC_BX(inst);
                return true;
            }
        }
        if(ctx->block_start[at]) return false; /* other paths may enter here */
    }
    return false;
}

static bool regs_equal(const JitEaRegs* a, const JitEaRegs* b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void regs_union(JitEaRegs* dst, const JitEaRegs* src) {
    for(int i = 0; i < 4; i++) dst->bits[i] |= src->bits[i];
}

/* The CALL at `pc` is a cached call of the builtin len (per its inline cache). */
static bool calls_len(const RegCode* code, int pc, py_CFunction len_cfunc) {
    if(len_cfunc == NULL) return false;
    const RegRuntimeSite* site = c11__at(RegRuntimeSite, &code->runtime_sites, pc);
    if(site->cache_index == REG_IC_NONE || site->cache_index >= (uint32_t)code->inline_caches.length)
        return false;
    const RegInlineCache* cache = c11__at(RegInlineCache, &code->inline_caches, (int)site->cache_index);
    return cache->kind == REG_IC_KIND_CALL && cache->variant == REG_CALL_CACHE_NATIVE &&
           cache->guard0 == (uintptr_t)len_cfunc;
}

static JitEaSite site_for(JitEaAction action, int vindex, int arg, unsigned reg) {
    JitEaSite site;
    memset(&site, 0, sizeof(site));
    site.action = (uint8_t)action;
    site.reg = (uint8_t)reg;
    site.count = 1;
    site.virtual_index[0] = (uint8_t)vindex;
    site.arg[0] = (uint8_t)arg;
    return site;
}

/* Add one object's use to the uses other objects already make of the same PC. */
static bool merge_site(JitEaSite* into, const JitEaSite* use) {
    if(into->action == JIT_EA_NONE) {
        *into = *use;
        return true;
    }
    if(into->action == JIT_EA_BUILD || into->action != use->action || into->reg != use->reg ||
       into->count >= JIT_EA_MAX_SITE_VIRTUALS)
        return false;
    into->virtual_index[into->count] = use->virtual_index[0];
    into->arg[into->count] = use->arg[0];
    into->count++;
    return true;
}

/*
 * One instruction of the dataflow: `in` = registers that may name the object
 * before `pc`. Records accepted uses in `sites`; returns false on an escape.
 */
static bool transfer(const EaCtx* ctx, const JitEaVirtual* v, int vindex, int pc,
                     const JitEaRegs* in, JitEaRegs* out, JitEaSite* sites,
                     py_CFunction len_cfunc) {
    const RegCode* code = ctx->code;
    const RegBytecode inst = code_at(code, pc);
    const RegOpcode op = RBC_OP(inst);
    const unsigned a = RBC_A(inst), b = RBC_B(inst), c = RBC_C(inst);
    EaEffects e;
    effects(code, pc, &e);
    *out = *in;
    const bool items = v->kind != JIT_EA_OBJECT;

    if(op == RBC_MOVE) {
        if(JitEaRegs__has(in, b)) {
            regs_add(out, a);
        } else {
            regs_del(out, a);
        }
        return true;
    }
    if(op == RBC_UNPACK_SEQUENCE && items && JitEaRegs__has(in, b) && c == v->component_count) {
        sites[pc] = site_for(JIT_EA_UNPACK, vindex, 0, b);
        regs_minus(out, &e.writes);
        return true;
    }
    if(op == RBC_LOAD_SUBSCR && items && JitEaRegs__has(in, b) && !JitEaRegs__has(in, c)) {
        int index;
        if(known_int(ctx, pc, c, &index)) {
            if(index < 0) index += v->component_count;
            if(index >= 0 && index < v->component_count) {
                sites[pc] = site_for(JIT_EA_SUBSCR, vindex, index, b);
                regs_del(out, a);
                return true;
            }
        }
    }
    if((op == RBC_CALL || op == RBC_CALL_IC) && items && b == 1 && c == 0 &&
       JitEaRegs__has(in, a + 2) && !JitEaRegs__has(in, a) && !JitEaRegs__has(in, a + 1) &&
       calls_len(code, pc, len_cfunc)) {
        sites[pc] = site_for(JIT_EA_LEN, vindex, 0, a + 2);
        regs_del(out, a);
        return true;
    }
    if((op == RBC_LOAD_ATTR || op == RBC_LOAD_ATTR_IC) && !items && JitEaRegs__has(in, a) &&
       RBC_BX(inst) < ctx->co->names.length) {
        const int f = field_index(v, c11__getitem(py_Name, &ctx->co->names, RBC_BX(inst)));
        if(f >= 0) {
            sites[pc] = site_for(JIT_EA_ATTR, vindex, f, a);
            regs_del(out, a);
            return true;
        }
    }
    if((op == RBC_STORE_ATTR || op == RBC_STORE_ATTR_IC) && !items &&
       JitEaRegs__has(in, a + 1) && !JitEaRegs__has(in, a) &&
       RBC_BX(inst) < ctx->co->names.length) {
        const int f = field_index(v, c11__getitem(py_Name, &ctx->co->names, RBC_BX(inst)));
        if(f >= 0) {
            sites[pc] = site_for(JIT_EA_SETATTR, vindex, f, a + 1);
            return true;
        }
    }
    if(e.reads_all || regs_intersect(&e.reads, in)) return false; /* escapes */
    /*
     * A register that may or may not be overwritten cannot be tracked: its
     * alias bit would be wrong on one of the paths.
     */
    for(unsigned r = 0; r < 256; r++) {
        if(JitEaRegs__has(in, r) && conditional_write(&e, r)) return false;
    }
    regs_minus(out, &e.writes);
    return true;
}

/*
 * Forward may-alias dataflow from the allocation over the whole CFG. On
 * success fills the alias register set and the per-PC liveness.
 */
static bool follow(EaCtx* ctx, JitEaVirtual* v, int vindex, JitEaSite* sites,
                   py_CFunction len_cfunc, JitEaRegs* aliases, uint8_t* live, JitEaRegs** in_out) {
    const RegCode* code = ctx->code;
    const int length = ctx->length;
    const int nlocals = ctx->co->nlocals;
    JitEaRegs* in = calloc((size_t)length, sizeof(JitEaRegs));
    uint8_t* queued = calloc((size_t)length, 1);
    if(in == NULL || queued == NULL) {
        free(in);
        free(queued);
        return false;
    }
    bool ok = true;
    int top = 0;
    JitEaRegs start = {{0}};
    regs_add(&start, v->result_reg);
    *aliases = start;

    int succ[2];
    int n = successors(code, v->build_pc, succ);
    for(int i = 0; i < n; i++) {
        if(succ[i] < 0 || succ[i] >= length) { ok = false; break; }
        regs_union(&in[succ[i]], &start);
        if(!queued[succ[i]]) {
            queued[succ[i]] = 1;
            ctx->stack[top++] = succ[i];
        }
    }
    while(ok && top > 0) {
        const int pc = ctx->stack[--top];
        queued[pc] = 0;
        if(pc == v->build_pc || regs_empty(&in[pc])) continue; /* the build restarts it */
        JitEaRegs out;
        if(!transfer(ctx, v, vindex, pc, &in[pc], &out, sites, len_cfunc)) {
            ok = false;
            break;
        }
        regs_union(aliases, &out);
        if(regs_empty(&out)) continue;
        n = successors(code, pc, succ);
        for(int i = 0; i < n; i++) {
            if(succ[i] < 0 || succ[i] >= length) { ok = false; break; }
            JitEaRegs merged = in[succ[i]];
            regs_union(&merged, &out);
            if(regs_equal(&merged, &in[succ[i]])) continue;
            in[succ[i]] = merged;
            if(!queued[succ[i]]) {
                if(top >= length * 2 + 2) { ok = false; break; }
                queued[succ[i]] = 1;
                ctx->stack[top++] = succ[i];
            }
        }
    }

    /*
     * Reaching the allocation again while registers may still name the previous
     * object: those registers lose it when the new one takes over the save area.
     * They must never be read again, and a named local must be overwritten before
     * anything can exit or observe the frame.
     */
    if(ok) {
        JitEaRegs old = in[v->build_pc];
        EaEffects build;
        effects(code, v->build_pc, &build);
        if(regs_intersect(&build.reads, &old)) ok = false; /* nested in itself */
        regs_del(&old, v->result_reg);
        n = successors(code, v->build_pc, succ);
        JitEaRegs named = {{0}};
        for(unsigned r = 0; ok && r < 256; r++) {
            if(!JitEaRegs__has(&old, r)) continue;
            if(reg_live(ctx, succ, n, r)) ok = false;
            if((int)r < nlocals) regs_add(&named, r);
        }
        if(ok && !regs_empty(&named)) {
            for(int at = v->build_pc + 1; at < length && !ctx->block_start[at]; at++) {
                if(!quiet(RBC_OP(code_at(code, at)))) break;
                EaEffects e;
                effects(code, at, &e);
                regs_minus(&named, &e.writes);
                if(regs_empty(&named)) break;
            }
            if(!regs_empty(&named)) ok = false;
        }
    }
    if(ok) {
        for(int pc = 0; pc < length; pc++) {
            if(!regs_empty(&in[pc])) live[pc] = 1;
        }
        *in_out = in; /* the caller derives the per-PC alias bit masks */
    } else {
        free(in);
    }
    free(queued);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Driver                                                                    */
/* ------------------------------------------------------------------------ */

static void virtual_dtor(JitEaVirtual* v) {
    free(v->clear);
    free(v->maybe);
    v->clear = NULL;
    v->maybe = NULL;
}

static uint64_t alias_bits(const JitEaVirtual* v, const JitEaRegs* regs) {
    uint64_t mask = 0;
    for(int i = 0; i < v->alias_count; i++) {
        if(JitEaRegs__has(regs, v->alias_regs[i])) mask |= UINT64_C(1) << i;
    }
    return mask;
}

/*
 * Alias bits of `v` that the instruction at `pc` overwrites and that may be set
 * there (MOVE copies them instead; the build sets the mask itself).
 */
static uint64_t clear_mask(const EaCtx* ctx, const JitEaVirtual* v, int pc) {
    if(ctx->is_payload[pc] || pc == v->build_pc) return 0;
    if(RBC_OP(code_at(ctx->code, pc)) == RBC_MOVE) return 0;
    EaEffects e;
    effects(ctx->code, pc, &e);
    return alias_bits(v, &e.writes) & v->maybe[pc];
}

void JitEa__analyze(const RegCode* code, const CodeObject* co, JitEaResult* out) {
    memset(out, 0, sizeof(*out));
    out->site_count = code->code.length;
    out->sites = calloc((size_t)code->code.length + 1, sizeof(JitEaSite));
    out->live = calloc((size_t)code->code.length + 1, 1);
    if(out->sites == NULL || out->live == NULL) return;
    py_Ref len = pk_current_vm != NULL && pk_current_vm->builtins != NULL
                     ? py_getdict(pk_current_vm->builtins, py_name("len"))
                     : NULL;
    if(len != NULL && len->type == tp_nativefunc) out->len_cfunc = len->_cfunc;

    EaCtx ctx;
    if(!ctx_init(&ctx, code, co)) {
        ctx_dtor(&ctx);
        return;
    }
    JitEaSite* scratch = calloc((size_t)code->code.length + 1, sizeof(JitEaSite));
    uint8_t* live = calloc((size_t)code->code.length + 1, 1);
    if(scratch == NULL || live == NULL) {
        free(scratch);
        free(live);
        ctx_dtor(&ctx);
        return;
    }
    for(int pc = 0; pc < ctx.length && out->virtual_count < JIT_EA_MAX_VIRTUALS; pc++) {
        if(ctx.is_payload[pc] || (out->sites[pc].action != JIT_EA_NONE && out->sites[pc].action != JIT_EA_BUILD)) continue;
        JitEaVirtual* v = &out->virtuals[out->virtual_count];
        if(!candidate(&ctx, pc, v)) continue;
        if(out->save_count + v->slot_count > JIT_EA_MAX_SAVE) continue;
        memset(scratch, 0, sizeof(JitEaSite) * (size_t)ctx.length);
        memset(live, 0, (size_t)ctx.length);
        JitEaRegs aliases;
        JitEaRegs* in = NULL;
        bool ok = follow(&ctx, v, out->virtual_count, scratch, out->len_cfunc, &aliases, live, &in);
        /* Uses shared with other objects must agree on what the PC does. */
        for(int i = 0; ok && i < ctx.length; i++) {
            if(scratch[i].action == JIT_EA_NONE) continue;
            JitEaSite probe = out->sites[i];
            if(!merge_site(&probe, &scratch[i])) ok = false;
        }
        if(ok) {
            memset(v->reg_bit, -1, sizeof(v->reg_bit));
            v->alias_count = 0;
            for(unsigned r = 0; ok && r < 256; r++) {
                if(!JitEaRegs__has(&aliases, r)) continue;
                if(v->alias_count >= JIT_EA_MAX_ALIASES) {
                    ok = false;
                    break;
                }
                v->reg_bit[r] = (int8_t)v->alias_count;
                v->alias_regs[v->alias_count++] = (uint8_t)r;
            }
        }
        if(ok) {
            v->clear = calloc((size_t)ctx.length + 1, sizeof(uint64_t));
            v->maybe = calloc((size_t)ctx.length + 1, sizeof(uint64_t));
            if(v->clear == NULL || v->maybe == NULL) ok = false;
        }
        if(ok) {
            for(int i = 0; i < ctx.length; i++) v->maybe[i] = alias_bits(v, &in[i]);
        }
        free(in);
        if(!ok) {
            virtual_dtor(v);
            continue;
        }
        for(int i = 0; i < ctx.length; i++) {
            v->clear[i] = clear_mask(&ctx, v, i);
            if(scratch[i].action != JIT_EA_NONE) (void)merge_site(&out->sites[i], &scratch[i]);
            if(live[i]) out->live[i] = 1;
        }
        out->sites[pc] = site_for(JIT_EA_BUILD, out->virtual_count, 0, v->result_reg);
        v->save_offset = out->save_count;
        out->save_count = (uint16_t)(out->save_count + v->slot_count);
        if(v->kind == JIT_EA_OBJECT) PK_INCREF(v->init_decl);
        out->virtual_count++;
    }
    /*
     * MOVE+use fusion: `MOVE t <- p` immediately followed by a use through t.
     * Reaching the use means executing that MOVE (no block starts between), so
     * t's alias bit equals p's there; if t is overwritten by the use or dead
     * afterwards, nobody else needs t's bit at all.
     */
    for(int pc = 1; pc < ctx.length && out->virtual_count > 0; pc++) {
        JitEaSite* site = &out->sites[pc];
        if(site->action == JIT_EA_NONE || site->action == JIT_EA_BUILD || ctx.block_start[pc] ||
           ctx.is_payload[pc - 1])
            continue;
        const RegBytecode prev = code_at(code, pc - 1);
        if(RBC_OP(prev) != RBC_MOVE || RBC_A(prev) != site->reg || RBC_B(prev) == site->reg) continue;
        /* A named local stays visible to frame introspection even when dead. */
        if((int)site->reg < co->nlocals) continue;
        const unsigned src = RBC_B(prev);
        bool ok = true;
        for(int k = 0; ok && k < site->count; k++) {
            if(out->virtuals[site->virtual_index[k]].reg_bit[src] < 0) ok = false;
        }
        if(!ok) continue;
        EaEffects e;
        effects(code, pc, &e);
        if(!JitEaRegs__has(&e.writes, site->reg)) {
            int succ[2];
            const int n = successors(code, pc, succ);
            if(reg_live(&ctx, succ, n, site->reg)) continue;
        }
        site->fused = 1;
        site->fused_src = (uint8_t)src;
        /* The fused MOVE leaves the use register's bit clear: nothing to clear after. */
        for(int k = 0; k < site->count; k++) {
            JitEaVirtual* v = &out->virtuals[site->virtual_index[k]];
            v->clear[pc] &= ~(UINT64_C(1) << v->reg_bit[site->reg]);
        }
    }
    free(scratch);
    free(live);
    ctx_dtor(&ctx);
}

void JitEa__dtor(JitEaResult* self) {
    for(int i = 0; i < self->virtual_count; i++) {
        JitEaVirtual* v = &self->virtuals[i];
        if(v->kind == JIT_EA_OBJECT && v->init_decl != NULL) PK_DECREF(v->init_decl);
        virtual_dtor(v);
    }
    free(self->sites);
    free(self->live);
    memset(self, 0, sizeof(*self));
}
