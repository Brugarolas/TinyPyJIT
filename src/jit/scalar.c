/* Speculative scalar recursion.
 *
 * Forward dataflow proves the type of every operand on the supported paths.
 * Each native activation holds only unboxed i64 locals, an output pointer and
 * a depth budget. Calls may only recurse into this same function. Unsupported
 * instructions, overflow and exhausted depth return failure BEFORE any Python
 * effect; the original entry then runs normally, including errors/tracebacks.
 * No memoization, algebraic rewrite or benchmark-specific pattern is used.
 */
#include "tinypy/jit/scalar.h"
#include "tinypy/objects/object.h"
#include "sljitLir.h"
#include <stdlib.h>
#include <string.h>

#define SCALAR_REGS 64
#define SCALAR_CODE 512
enum { UNKNOWN, INTEGER, BOOLEAN, SELF, NIL };
typedef sljit_sw (*ScalarEntry)(const py_i64*, py_i64*, sljit_sw);
struct JitScalarCode {
    void* executable;
    ScalarEntry entry;
    size_t bytes;
    py_Name self_name;
    int argc;
    int nregs;
    unsigned failures;
};

static int arithmetic(RegOpcode op) {
    switch(op) {
        case RBC_ADD: case RBC_ADD_INT: case RBC_ADD_I8: return SLJIT_ADD;
        case RBC_SUB: case RBC_SUB_INT: case RBC_SUB_I8: return SLJIT_SUB;
        case RBC_MUL: case RBC_MUL_INT: case RBC_MUL_I8: return SLJIT_MUL;
        default: return 0;
    }
}

static int condition(unsigned mode) {
    static const int conditions[] = {SLJIT_SIG_LESS, SLJIT_SIG_LESS_EQUAL, SLJIT_EQUAL,
                                     SLJIT_NOT_EQUAL, SLJIT_SIG_GREATER, SLJIT_SIG_GREATER_EQUAL};
    unsigned index = mode & REG_CMP_BRANCH_CODE_MASK;
    if(index >= 6) return -1;
    int result = conditions[index];
    return (mode & REG_CMP_BRANCH_TRUE) ? result : (result ^ 1);
}

static bool merge(unsigned char* states, unsigned char* reached, int nregs, int length,
                  int pc, int target, const unsigned char* state) {
    /* Backedges need polling and are deliberately left to the baseline JIT. */
    if(target <= pc || target >= length) return false;
    unsigned char* dst = states + target * nregs;
    if(!reached[target]) memcpy(dst, state, (size_t)nregs);
    else for(int i = 0; i < nregs; i++) if(dst[i] != state[i]) dst[i] = UNKNOWN;
    reached[target] = 1;
    return true;
}

JitScalarCode* JitScalar__compile(const FuncDecl* decl, const RegCode* code) {
    const int nr = code->nregs, len = code->code.length, argc = decl->args.length;
    if(decl->type != FuncType_SIMPLE || argc < 1 || argc > 8 || nr < argc ||
       nr > SCALAR_REGS || len < 1 || len > SCALAR_CODE) return NULL;
    unsigned char* states = calloc((size_t)nr * len, 1);
    unsigned char reached[SCALAR_CODE] = {1}, supported[SCALAR_CODE] = {0};
    if(states == NULL) return NULL;
    for(int i = 0; i < argc; i++) states[i] = INTEGER;
    py_Name self_name = py_name(decl->code.name->data);
    int calls = 0, returns = 0;
    for(int pc = 0; pc < len; pc++) {
        if(!reached[pc]) continue;
        unsigned char state[SCALAR_REGS];
        memcpy(state, states + pc * nr, (size_t)nr);
        RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        RegOpcode op = RBC_OP(inst);
        unsigned a = RBC_A(inst), b = RBC_B(inst), d = RBC_C(inst);
        int next = pc + 1, target = -1;
        bool terminal = false, ok = a < (unsigned)nr;
        if(!ok) continue;
        if(arithmetic(op)) {
            bool imm = op == RBC_ADD_I8 || op == RBC_SUB_I8 || op == RBC_MUL_I8;
            ok = b < (unsigned)nr && state[b] == INTEGER &&
                 (imm || (d < (unsigned)nr && state[d] == INTEGER));
            state[a] = INTEGER;
        } else switch(op) {
            case RBC_NOP: break;
            case RBC_CHECK_BOUND: ok = state[a] != UNKNOWN && state[a] != NIL; break;
            case RBC_MOVE:
                ok = b < (unsigned)nr && state[b] != UNKNOWN;
                if(ok) state[a] = state[b];
                break;
            case RBC_LOAD_I16: state[a] = INTEGER; break;
            case RBC_LOAD_BOOL: state[a] = BOOLEAN; break;
            case RBC_LOAD_NIL: state[a] = NIL; break;
            case RBC_LOAD_CONST: {
                py_TValue* v = c11__at(py_TValue, &decl->code.consts, RBC_BX(inst));
                ok = v->type == tp_int;
                state[a] = INTEGER;
                break;
            }
            case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
            case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
                ok = c11__getitem(py_Name, &decl->code.names, RBC_BX(inst)) == self_name;
                state[a] = SELF;
                break;
            case RBC_CALL: case RBC_CALL_IC:
                ok = d == 0 && b == (unsigned)argc && a + 1 + b < (unsigned)nr &&
                     state[a] == SELF && state[a + 1] == NIL;
                for(unsigned i = 0; ok && i < b; i++) ok = state[a + 2 + i] == INTEGER;
                state[a] = INTEGER;
                if(ok) calls++;
                break;
            case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: {
                ok = pc + 1 < len && state[a] == INTEGER && condition(d) >= 0 &&
                     (op == RBC_CMP_I8_BRANCH || (b < (unsigned)nr && state[b] == INTEGER));
                if(ok) {
                    RegBytecode ext = c11__getitem(RegBytecode, &code->code, pc + 1);
                    target = pc + 1 + RBC_SBX(ext);
                    next = pc + 2 + RBC_A(ext);
                }
                break;
            }
            case RBC_JUMP: next = pc + RBC_SBX(inst); break;
            case RBC_JUMP_IF_TRUE: case RBC_JUMP_IF_FALSE:
                ok = state[a] == INTEGER || state[a] == BOOLEAN;
                target = pc + RBC_SBX(inst);
                break;
            case RBC_RETURN:
                ok = state[a] == INTEGER;
                terminal = true;
                if(ok) returns++;
                break;
            default: ok = false; break;
        }
        if(!ok) continue; /* This path fails before executing the instruction. */
        if(!terminal && (next <= pc || next >= len ||
                        (target != -1 && (target <= pc || target >= len)))) continue;
        supported[pc] = 1;
        if(!terminal) {
            merge(states, reached, nr, len, pc, next, state);
            if(target != -1) merge(states, reached, nr, len, pc, target, state);
        }
    }
    free(states);
    if(!calls || !returns) return NULL;

    struct sljit_compiler* c = sljit_create_compiler(NULL);
    if(c == NULL) return NULL;
    struct sljit_label* labels[SCALAR_CODE] = {0};
    struct sljit_jump* jumps[SCALAR_CODE * 3 + 1];
    int targets[SCALAR_CODE * 3 + 1], count = 0;
    #define BRANCH(j, t) do { jumps[count] = (j); targets[count++] = (t); } while(0)
    #define LOCAL(r) SLJIT_MEM1(SLJIT_SP), ((sljit_sw)(r) * sizeof(py_i64))
    /* R0 receives argv; S0=output, S1=remaining depth. */
    struct sljit_label* entry = sljit_emit_label(c);
    sljit_emit_enter(c, 0, SLJIT_ARGS3(W, P_R, P, W), 3, 2, nr * sizeof(py_i64));
    BRANCH(sljit_emit_cmp(c, SLJIT_SIG_LESS_EQUAL, SLJIT_S1, 0, SLJIT_IMM, 0), -1);
    for(int i = 0; i < argc; i++) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), i * sizeof(py_i64));
        sljit_emit_op1(c, SLJIT_MOV, LOCAL(i), SLJIT_R1, 0);
    }
    for(int pc = 0; pc < len; pc++) {
        labels[pc] = sljit_emit_label(c);
        if(!supported[pc]) { BRANCH(sljit_emit_jump(c, SLJIT_JUMP), -1); continue; }
        RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        RegOpcode op = RBC_OP(inst);
        unsigned a = RBC_A(inst), b = RBC_B(inst), d = RBC_C(inst);
        int arith = arithmetic(op);
        if(arith) {
            bool imm = op == RBC_ADD_I8 || op == RBC_SUB_I8 || op == RBC_MUL_I8;
            sljit_emit_op2(c, arith | SLJIT_SET_OVERFLOW, SLJIT_R0, 0, LOCAL(b),
                           imm ? SLJIT_IMM : SLJIT_MEM1(SLJIT_SP),
                           imm ? (sljit_sw)(int8_t)d : (sljit_sw)(d * sizeof(py_i64)));
            BRANCH(sljit_emit_jump(c, SLJIT_OVERFLOW), -1);
            sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), SLJIT_R0, 0);
            continue;
        }
        switch(op) {
            case RBC_LOAD_NIL:
            case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
            case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
                sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), SLJIT_IMM, 0); break;
            case RBC_MOVE: sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), LOCAL(b)); break;
            case RBC_LOAD_I16:
                sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), SLJIT_IMM, (int16_t)RBC_BX(inst)); break;
            case RBC_LOAD_BOOL:
                sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), SLJIT_IMM, b != 0); break;
            case RBC_LOAD_CONST:
                sljit_emit_op1(c, SLJIT_MOV, LOCAL(a), SLJIT_IMM,
                               c11__at(py_TValue, &decl->code.consts, RBC_BX(inst))->_i64); break;
            case RBC_CALL: case RBC_CALL_IC: {
                sljit_get_local_base(c, SLJIT_R0, 0, (a + 2) * sizeof(py_i64));
                sljit_get_local_base(c, SLJIT_R1, 0, a * sizeof(py_i64));
                sljit_emit_op2(c, SLJIT_SUB, SLJIT_R2, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
                struct sljit_jump* call = sljit_emit_call(c, SLJIT_CALL, SLJIT_ARGS3(W, P, P, W));
                if(call != NULL) sljit_set_label(call, entry);
                BRANCH(sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0), -1);
                break;
            }
            case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: {
                RegBytecode ext = c11__getitem(RegBytecode, &code->code, pc + 1);
                BRANCH(sljit_emit_cmp(c, condition(d), LOCAL(a),
                                      op == RBC_CMP_I8_BRANCH ? SLJIT_IMM : SLJIT_MEM1(SLJIT_SP),
                                      op == RBC_CMP_I8_BRANCH ? (sljit_sw)(int8_t)b :
                                                               (sljit_sw)(b * sizeof(py_i64))),
                       pc + 1 + RBC_SBX(ext));
                BRANCH(sljit_emit_jump(c, SLJIT_JUMP), pc + 2 + RBC_A(ext));
                break;
            }
            case RBC_JUMP: BRANCH(sljit_emit_jump(c, SLJIT_JUMP), pc + RBC_SBX(inst)); break;
            case RBC_JUMP_IF_TRUE: case RBC_JUMP_IF_FALSE:
                BRANCH(sljit_emit_cmp(c, op == RBC_JUMP_IF_TRUE ? SLJIT_NOT_EQUAL : SLJIT_EQUAL,
                                      LOCAL(a), SLJIT_IMM, 0), pc + RBC_SBX(inst)); break;
            case RBC_RETURN:
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), 0, LOCAL(a));
                sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, 1); break;
            default: break; /* bound checks and symbolic SELF/NIL need no runtime work */
        }
    }
    struct sljit_label* fail = sljit_emit_label(c);
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, 0);
    bool ok = entry != NULL && fail != NULL;
    for(int i = 0; i < count; i++) {
        struct sljit_label* target = targets[i] < 0 ? fail : labels[targets[i]];
        if(jumps[i] == NULL || target == NULL) { ok = false; break; }
        sljit_set_label(jumps[i], target);
    }
    #undef LOCAL
    #undef BRANCH
    JitScalarCode* result = NULL;
    if(ok && sljit_get_compiler_error(c) == SLJIT_SUCCESS) {
        result = calloc(1, sizeof(*result));
        if(result != NULL) {
            result->executable = sljit_generate_code(c, 0, NULL);
            if(result->executable == NULL) { free(result); result = NULL; }
            else {
                memcpy(&result->entry, &result->executable, sizeof(result->entry));
                result->bytes = sljit_get_generated_code_size(c);
                result->self_name = self_name;
                result->argc = argc;
                result->nregs = nr;
            }
        }
    }
    sljit_free_compiler(c);
    return result;
}

void JitScalar__delete(JitScalarCode* code) {
    if(code == NULL) return;
    sljit_free_code(code->executable, NULL);
    free(code);
}

size_t JitScalar__size(const JitScalarCode* code) { return code == NULL ? 0 : code->bytes; }

bool JitScalar__call(JitScalarCode* code, VM* vm, py_Frame* frame) {
    if(code == NULL || code->failures >= 8 || vm->trace_info.func != NULL || frame->p0 == NULL ||
       frame->p0->type != tp_function || frame->globals->type != tp_module
#if PK_ENABLE_WATCHDOG
       || vm->watchdog_info.max_reset_time > 0
#endif
       ) return false;
    Function* fn = py_touserdata(frame->p0);
    if(fn->closure != NULL) return false;
    py_ItemRef binding = NameDict__try_get(PyObject__dict(frame->globals->_obj), code->self_name);
    if(binding == NULL || binding->type != tp_function || binding->_obj != frame->p0->_obj)
        return false;
    py_i64 args[8], value;
    for(int i = 0; i < code->argc; i++) {
        if(frame->locals[i].type != tp_int) return false;
        args[i] = frame->locals[i]._i64;
    }
    /* Match both Python limits; cap C-stack use independently of user limits. */
    int depth = vm->max_recursion_depth - vm->recursion_depth;
    ptrdiff_t slots = vm->stack.end - vm->stack.sp;
    int stack_depth = (int)(slots / (code->nregs + 2)) + 1;
    if(depth > stack_depth) depth = stack_depth;
    if(depth > 128) depth = 128;
    if(!code->entry(args, &value, depth)) {
        /* A rarely taken effectful path is fine; persistent failures must not
         * repeat speculative work on every call of a mostly unsupported body. */
        code->failures++;
        return false;
    }
    code->failures = 0;
    py_newint(&vm->last_retval, value);
    return true;
}
