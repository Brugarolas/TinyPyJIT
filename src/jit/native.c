/*
 * TinyPy native baseline JIT: RegVM register bytecode -> SLJIT machine code.
 *
 * Compilation model
 * -----------------
 * A function is compiled once it (or one of its loops) becomes hot. Native code
 * operates directly on the frame's RegVM registers (py_TValue array in S0), so
 * the interpreter and native code always share one precise state:
 *
 *  - straight-line integer arithmetic/comparisons form LIR "islands" that are
 *    register-allocated with the graph-coloring allocator; every result is
 *    written back immediately (SYNC_STATE), so any guard can resume RegVM at
 *    the failing bytecode without reconstructing anything;
 *  - control flow, constants, moves and range iteration are emitted directly;
 *  - globals and attributes run natively through the RegVM's inline caches;
 *  - CALL and LOAD_METHOD call back into the RegVM's own implementation
 *    (the RegVM__jit_step_fn() helpers) and continue natively after
 *    the callee returns; an exception leaves through a precise error exit;
 *  - every other opcode is a side exit: native code returns to the interpreter
 *    at that PC, which executes it and later re-enters native code at the next
 *    hot loop header. A function is therefore never rejected just because it
 *    contains an unsupported operation somewhere.
 *
 * Helper calls may run arbitrary Python code (and the GC). That is safe because
 * no value lives only in a machine register across a helper call: every result
 * is already stored in the frame's registers, and the per-entry state that the
 * callee may invalidate (inline cache array, type_version) is refreshed by the
 * helper wrapper before native code continues.
 *
 * Native entries happen at PC 0 (function entry) or at loop headers. Entries
 * that repeatedly return without doing native work are disabled at runtime.
 */
#include "tinypy/jit/native.h"

#include "tinypy/interpreter/jitbridge.h"
#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/interpreter/regprofile.h"
#include "tinypy/interpreter/regvm.h"
#include "tinypy/interpreter/shape.h"
#include "tinypy/interpreter/typeinfo.h"
#include "tinypy/objects/namedict.h"
#include "tinypy/jit/escape.h"
#include "tinypy/jit/lir.h"
#include "tinypy/jit/regalloc.h"
#include "tinypy/objects/base.h"
#include "tinypy/objects/iterator.h"
#include "tinypy/objects/object.h"
#include "sljitLir.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(PK_ENABLE_JIT) || !PK_ENABLE_JIT
#error "native.c must be compiled only when PK_ENABLE_JIT=1"
#endif

#if UINTPTR_MAX != UINT64_MAX
#error "The initial native JIT requires a 64-bit target"
#endif

_Static_assert(sizeof(py_TValue) % sizeof(uint64_t) == 0,
               "py_TValue must be word-sized for native value copies");
_Static_assert(sizeof(sljit_sw) == sizeof(void*), "Unexpected SLJIT word size");
_Static_assert(sizeof(py_i64) == sizeof(sljit_sw), "Native int arithmetic needs 64-bit words");

#define TINYJIT_MAX_CODE 4096
#define TINYJIT_MAX_REGS 256
#define TINYJIT_LIR_MAX_ISLAND 64
#define TINYJIT_LIR_GPRS 8
#define TINYJIT_LIR_RESERVED_GPRS 2
#define TINYJIT_RESUME_BASE 8192
#define TINYJIT_ERROR_BASE 16384
#define TINYJIT_RETURN_NONE 256
#define TINYJIT_NOT_ENTERED (-1)
#define TINYJIT_LOOP_FUEL 4096
#define TINYJIT_FUTILE_LIMIT 32
#define TINYJIT_CACHE_LIMIT_BYTES ((size_t)32 * 1024 * 1024)

/*
 * Per-entry state shared with native code through SLJIT_S1. The inline caches
 * are read live, so whatever the interpreter fills in is used by native code
 * immediately (shared caches). `caches` and `type_version` can change while a
 * helper runs Python code, so the helper wrappers refresh them.
 */
typedef struct TinyJitRun {
    sljit_sw fuel;              /* backedges left before returning to the interpreter */
    RegInlineCache* caches;     /* RegCode::inline_caches.data (moves when caches are added) */
    uint64_t type_version;      /* VM::type_version (refreshed after helper calls) */
    NameDict* globals;          /* the frame's module namespace, or NULL */
    NameDict* builtins;         /* builtins namespace, or NULL */
    sljit_sw has_closure;       /* the frame's function has a closure (LOAD_NONLOCAL) */
    VM* vm;                     /* helper-call context */
    py_Frame* frame;
    RegCode* code;
    sljit_sw helper_calls;      /* helper calls completed during this entry */
    /*
     * Scalar replacement (escape.h). Bit i of `pending` means virtual object i
     * exists only as its components in `vsave`; `pc` is the PC of the helper
     * call in progress, where a frame introspection may force it.
     */
    sljit_sw pending;
    py_TValue* vsave;
    sljit_sw pc;
    const struct TinyJitCode* compiled;
    py_JitRoots roots;
} TinyJitRun;

/* S0 = RegVM registers, S1 = TinyJitRun*, S2 = entry PC. */
typedef sljit_sw (*TinyJitEntry)(py_TValue* values, TinyJitRun* run, sljit_sw start_pc);

typedef struct TinyJitCode {
    const RegCode* regcode;
    const CodeObject* source;
    void* executable;
    TinyJitEntry entry;
    size_t executable_bytes;
    uint64_t signature;
    uint8_t* valid_entry_pc;
    uint8_t* futile; /* consecutive entries at a PC that did no native work */
    uint32_t lir_islands;
    uint32_t lir_instructions;
    uint32_t colored_vregs;
    uint32_t spilled_vregs;
    uint32_t side_exit_sites;
    uint32_t shared_ic_sites;
    uint32_t helper_call_sites;
    uint32_t virtual_sites;
    /*
     * Native activations of this code on the C stack. A helper call can run
     * Python code that requests compilation or evicts old code; executable
     * memory that an outer activation will return into must stay alive.
     */
    uint32_t active;
    /* Scalar-replaced allocations and the type_version each object virtual was
     * last validated at (read by native code). */
    JitEaResult ea;
    uint64_t validated[JIT_EA_MAX_VIRTUALS];
    struct TinyJitCode* next;
} TinyJitCode;

typedef struct TinyJitContext {
    TinyJitCode* code;
    TinyJitCode* last; /* most recently entered code: hot loops hit this cache */
    uint64_t compiled;
    uint64_t entered;
    uint64_t completed;
    uint64_t resumed;
    uint64_t disabled_entries;
    size_t generated_bytes;
    uint64_t lir_islands;
    uint64_t lir_instructions;
    uint64_t colored_vregs;
    uint64_t spilled_vregs;
    uint64_t side_exit_sites;
    uint64_t shared_ic_sites;
    uint64_t helper_call_sites;
    uint64_t helper_errors;
    uint64_t released; /* code dropped because its function died */
    uint64_t virtual_sites;   /* allocations compiled as scalar-replaced objects */
    uint64_t materialized;    /* virtual objects created for real (deopt/force) */
} TinyJitContext;

typedef enum TinyJitTarget {
    TINYJIT_TARGET_BLOCK = 0, /* native code of that PC */
    TINYJIT_TARGET_RESUME,    /* return to the interpreter at that PC */
    TINYJIT_TARGET_ERROR,     /* a helper raised at that PC */
    TINYJIT_TARGET_SLOW,      /* IC miss: the helper slow path of that PC */
} TinyJitTarget;

typedef struct TinyJitFixup {
    struct sljit_jump* jump;
    int pc;
    uint8_t target;
} TinyJitFixup;

typedef struct TinyJitEmitter {
    struct sljit_compiler* compiler;
    struct sljit_label** blocks;
    struct sljit_label** resume;
    struct sljit_label** error; /* only for helper-call PCs */
    struct sljit_label** slow;  /* only for cached ops with a native fast path */
    const JitEaResult* ea;
    const struct TinyJitCode* target; /* the code being built (for absolute addresses) */
    sljit_sw* kill_mask;        /* per PC: pending bits cleared before it */
    uint8_t* materialize;       /* per PC: the resume exit must materialize objects */
    TinyJitFixup* fixes;
    size_t fix_count;
    size_t fix_capacity;
    int code_length;
    bool failed;
    const RegCode* code;
    const uint8_t* kind; /* TinyJitSiteKind per PC */
} TinyJitEmitter;

/* Each island is a straight-line integer region with its own SSA LIR and RA. */
typedef struct TinyJitIsland {
    int first_pc;
    int last_pc;
    JitLirFunction lir;
    JitRegAllocStats stats;
    uint32_t physical_vregs;
} TinyJitIsland;

/* ------------------------------------------------------------------------ */
/* Diagnostics: TINYPY_JIT_TRACE=1 explains every compile decision.          */
/* ------------------------------------------------------------------------ */

static bool jit_trace_enabled(void) {
    static int cached = -1;
    if(cached < 0) {
        const char* value = getenv("TINYPY_JIT_TRACE");
        cached = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    return cached != 0;
}

static void jit_trace(const FuncDecl* decl, const char* fmt, ...) {
    if(!jit_trace_enabled()) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[TinyPy JIT] %s: ", decl != NULL && decl->code.name != NULL
                                            ? decl->code.name->data : "?");
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

/* ------------------------------------------------------------------------ */
/* Opcode classification                                                     */
/* ------------------------------------------------------------------------ */

static bool is_arith_i8(RegOpcode op) {
    return op == RBC_ADD_I8 || op == RBC_SUB_I8 || op == RBC_MUL_I8;
}

static bool is_compare_i8(RegOpcode op) {
    return op >= RBC_CMP_LT_I8 && op <= RBC_CMP_GE_I8;
}

static bool is_compare_binary(RegOpcode op) {
    return (op >= RBC_CMP_LT && op <= RBC_CMP_GE) ||
           (op >= RBC_CMP_LT_INT && op <= RBC_CMP_GE_INT) ||
           (op >= RBC_CMP_LT_NUM && op <= RBC_CMP_GE_NUM);
}

static bool is_compare(RegOpcode op) {
    return is_compare_binary(op) || is_compare_i8(op);
}

/* Integer operations lowered into LIR islands (comparisons excluded). */
static JitLirOpcode int_binary_lir(RegOpcode op) {
    switch(op) {
        case RBC_ADD: case RBC_ADD_INT: case RBC_ADD_NUM: case RBC_ADD_I8: return JIT_LIR_ADD_I64;
        case RBC_SUB: case RBC_SUB_INT: case RBC_SUB_NUM: case RBC_SUB_I8: return JIT_LIR_SUB_I64;
        case RBC_MUL: case RBC_MUL_INT: case RBC_MUL_NUM: case RBC_MUL_I8: return JIT_LIR_MUL_I64;
        case RBC_FLOORDIV: return JIT_LIR_DIV_I64;
        case RBC_MOD: return JIT_LIR_MOD_I64;
        case RBC_BITAND: return JIT_LIR_AND_I64;
        case RBC_BITOR: return JIT_LIR_OR_I64;
        case RBC_BITXOR: return JIT_LIR_XOR_I64;
        case RBC_LSHIFT: return JIT_LIR_SHL_I64;
        case RBC_RSHIFT: return JIT_LIR_SHR_I64;
        default: return JIT_LIR__COUNT;
    }
}

static bool is_island_op(RegOpcode op) {
    return int_binary_lir(op) != JIT_LIR__COUNT || is_compare(op);
}

static bool is_range_iteration(RegOpcode op) {
    return op == RBC_FOR_ITER || op == RBC_FOR_RANGE;
}

/* Opcodes executed by calling back into the RegVM without leaving native code. */
static bool is_helper_op(RegOpcode op) {
    return op == RBC_CALL || op == RBC_CALL_IC;
}

/* Opcodes with a native implementation. Everything else is a side exit. */
static bool is_native_op(RegOpcode op) {
    if(is_island_op(op) || is_range_iteration(op) || is_helper_op(op)) return true;
    switch(op) {
        case RBC_NOP: case RBC_MOVE: case RBC_CHECK_BOUND:
        case RBC_LOAD_CONST: case RBC_LOAD_NONE: case RBC_LOAD_NIL:
        case RBC_LOAD_BOOL: case RBC_LOAD_I16:
        case RBC_JUMP: case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP: case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: case RBC_SUPER_DATA:
        case RBC_RETURN: case RBC_RETURN_NONE:
            return true;
        default: return false;
    }
}

/* Work that native code does faster than the interpreter (not just plumbing). */
static bool is_native_work(RegOpcode op) {
    return is_island_op(op) || is_range_iteration(op) ||
           op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH;
}

/* Opcodes executed natively through the interpreter's own inline caches. */
static bool is_cached_op(RegOpcode op) {
    switch(op) {
        case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
        case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
        case RBC_LOAD_ATTR: case RBC_LOAD_ATTR_IC:
        case RBC_STORE_ATTR: case RBC_STORE_ATTR_IC:
        case RBC_LOAD_METHOD: case RBC_LOAD_METHOD_IC:
            return true;
        default: return false;
    }
}

/* How one PC is compiled, decided once per compilation from shared feedback. */
typedef enum TinyJitSiteKind {
    TINYJIT_SITE_EXIT = 0, /* side exit to the interpreter */
    TINYJIT_SITE_NATIVE,   /* emitted directly (cached ops: IC fast path + helper slow path) */
    TINYJIT_SITE_ISLAND,   /* part of an integer LIR island */
    TINYJIT_SITE_HELPER,   /* RegVM helper call, staying in native code */
    TINYJIT_SITE_VIRTUAL,  /* scalar-replaced allocation or one of its uses */
} TinyJitSiteKind;

static uint16_t site_cache_index(const RegCode* code, int pc) {
    if(pc < 0 || pc >= code->runtime_sites.length) return REG_IC_NONE;
    return c11__at(RegRuntimeSite, &code->runtime_sites, pc)->cache_index;
}

/*
 * The interpreter's quickening and type profile tell us what this site really
 * sees. Compiling an int guard that the profile says will fail just converts
 * every execution into a deoptimization, so such sites stay interpreted.
 */
static bool profile_expects_int(const RegCode* code, int pc, RegOpcode op) {
    if((op >= RBC_ADD_NUM && op <= RBC_MUL_NUM) || (op >= RBC_CMP_LT_NUM && op <= RBC_CMP_GE_NUM))
        return false; /* quickened for floats */
    const RegProfileSite* site = RegProfile__site(code, pc);
    if(site == NULL || site->types.first_count == 0) return true;
    uint32_t pair = 0;
    const uint32_t confidence = RegProfile__type_pair_confidence_16(&site->types, &pair);
    const uint32_t int_pair = (uint32_t)(uint16_t)tp_int | ((uint32_t)(uint16_t)tp_int << 16);
    return pair == int_pair || confidence < 0x8000u;
}

/* A cache the interpreter has allocated and that mostly hits. */
static bool profile_cache_usable(const RegCode* code, int pc) {
    if(site_cache_index(code, pc) == REG_IC_NONE) return false;
    const RegProfileSite* site = RegProfile__site(code, pc);
    return site == NULL || site->ic_misses <= site->ic_hits;
}

static TinyJitSiteKind classify_site(const RegCode* code, int pc, RegOpcode op) {
    if(is_island_op(op)) {
        return profile_expects_int(code, pc, op) ? TINYJIT_SITE_ISLAND : TINYJIT_SITE_EXIT;
    }
    /*
     * A cached op gets the native IC fast path only when the interpreter has
     * allocated its cache (the index is baked into the code) and it mostly hits.
     * Otherwise, e.g. a site first reached after compilation, it still stays in
     * native code through the helper, which also fills the cache.
     */
    if(is_cached_op(op)) {
        return profile_cache_usable(code, pc) ? TINYJIT_SITE_NATIVE : TINYJIT_SITE_HELPER;
    }
    if(is_helper_op(op)) return TINYJIT_SITE_HELPER;
    return is_native_op(op) ? TINYJIT_SITE_NATIVE : TINYJIT_SITE_EXIT;
}

/* PCs that call a RegVM helper (always, or on an IC miss) and so need an error exit. */
static bool site_calls_helper(const RegCode* code, const uint8_t* kind, int pc) {
    return kind[pc] == TINYJIT_SITE_HELPER ||
           (kind[pc] == TINYJIT_SITE_NATIVE &&
            is_cached_op(RBC_OP(c11__getitem(RegBytecode, &code->code, pc))));
}

static sljit_s32 compare_condition(RegOpcode op) {
    switch(op) {
        case RBC_CMP_LT: case RBC_CMP_LT_INT: case RBC_CMP_LT_NUM:
        case RBC_CMP_LT_I8: return SLJIT_SIG_LESS;
        case RBC_CMP_LE: case RBC_CMP_LE_INT: case RBC_CMP_LE_NUM:
        case RBC_CMP_LE_I8: return SLJIT_SIG_LESS_EQUAL;
        case RBC_CMP_EQ: case RBC_CMP_EQ_INT: case RBC_CMP_EQ_NUM:
        case RBC_CMP_EQ_I8: return SLJIT_EQUAL;
        case RBC_CMP_NE: case RBC_CMP_NE_INT: case RBC_CMP_NE_NUM:
        case RBC_CMP_NE_I8: return SLJIT_NOT_EQUAL;
        case RBC_CMP_GT: case RBC_CMP_GT_INT: case RBC_CMP_GT_NUM:
        case RBC_CMP_GT_I8: return SLJIT_SIG_GREATER;
        case RBC_CMP_GE: case RBC_CMP_GE_INT: case RBC_CMP_GE_NUM:
        case RBC_CMP_GE_I8: return SLJIT_SIG_GREATER_EQUAL;
        default: return -1;
    }
}

static sljit_s32 invert_condition(sljit_s32 condition) {
    switch(condition) {
        case SLJIT_EQUAL: return SLJIT_NOT_EQUAL;
        case SLJIT_NOT_EQUAL: return SLJIT_EQUAL;
        case SLJIT_SIG_LESS: return SLJIT_SIG_GREATER_EQUAL;
        case SLJIT_SIG_LESS_EQUAL: return SLJIT_SIG_GREATER;
        case SLJIT_SIG_GREATER: return SLJIT_SIG_LESS_EQUAL;
        case SLJIT_SIG_GREATER_EQUAL: return SLJIT_SIG_LESS;
        default: return -1;
    }
}

static RegOpcode compare_from_code(unsigned code) {
    switch(code & REG_CMP_BRANCH_CODE_MASK) {
        case REG_CMP_LT: return RBC_CMP_LT;
        case REG_CMP_LE: return RBC_CMP_LE;
        case REG_CMP_EQ: return RBC_CMP_EQ;
        case REG_CMP_NE: return RBC_CMP_NE;
        case REG_CMP_GT: return RBC_CMP_GT;
        case REG_CMP_GE: return RBC_CMP_GE;
        default: return RBC_NOP;
    }
}

/* ------------------------------------------------------------------------ */
/* Emission helpers                                                          */
/* ------------------------------------------------------------------------ */

/* A hardware register has no stable meaning until after jit_regalloc_apply(). */
static sljit_s32 lir_phys_reg(JitLirOperand operand) {
    if(operand.kind != JIT_LIR_OPERAND_PREG ||
       operand.as.preg.bank != JIT_PHYS_GPR_SCRATCH ||
       operand.as.preg.index < TINYJIT_LIR_RESERVED_GPRS ||
       operand.as.preg.index >= TINYJIT_LIR_GPRS) return -1;
    return SLJIT_R(operand.as.preg.index);
}

static sljit_sw field_offset(unsigned reg, size_t field) {
    return (sljit_sw)((size_t)reg * sizeof(py_TValue) + field);
}

static bool remember_target(TinyJitEmitter* out, struct sljit_jump* jump, int pc,
                            TinyJitTarget target) {
    if(jump == NULL || pc < 0 || pc >= out->code_length ||
       out->fix_count >= out->fix_capacity) {
        out->failed = true;
        return false;
    }
    TinyJitFixup* item = &out->fixes[out->fix_count++];
    item->jump = jump;
    item->pc = pc;
    item->target = (uint8_t)target;
    return true;
}

static bool remember(TinyJitEmitter* out, struct sljit_jump* jump, int pc, bool resume) {
    return remember_target(out, jump, pc, resume ? TINYJIT_TARGET_RESUME : TINYJIT_TARGET_BLOCK);
}

/* Resume the interpreter at pc with the (always precise) register state. */
static void emit_side_exit(TinyJitEmitter* out, int pc) {
    remember(out, sljit_emit_jump(out->compiler, SLJIT_JUMP), pc, true);
}

static void emit_copy_word(struct sljit_compiler* compiler, unsigned dst,
                           unsigned src, size_t offset) {
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0,
                   SLJIT_MEM1(SLJIT_S0), field_offset(src, offset));
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offset), SLJIT_R0, 0);
}

static void emit_copy_value(struct sljit_compiler* compiler, unsigned dst, unsigned src) {
    if(dst == src) return;
    for(size_t i = 0; i < sizeof(py_TValue); i += sizeof(uint64_t)) {
        emit_copy_word(compiler, dst, src, i);
    }
}

static void emit_template(struct sljit_compiler* compiler, unsigned dst,
                          const py_TValue* value) {
    for(size_t i = 0; i < sizeof(py_TValue); i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, (const unsigned char*)value + i, sizeof(word));
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
                       field_offset(dst, i), SLJIT_IMM, (sljit_sw)word);
    }
}

/* Same field writes as py_newint(): type, is_ptr and the payload. */
static void emit_store_int(struct sljit_compiler* compiler, unsigned dst,
                           sljit_s32 src, sljit_sw srcw) {
    sljit_emit_op1(compiler, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, type)), SLJIT_IMM, tp_int);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, is_ptr)), SLJIT_IMM, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, _i64)), src, srcw);
}

/* Same field writes as py_newbool(). */
static void emit_store_bool(struct sljit_compiler* compiler, unsigned dst,
                            sljit_s32 src, sljit_sw srcw) {
    sljit_emit_op1(compiler, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, type)), SLJIT_IMM, tp_bool);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, is_ptr)), SLJIT_IMM, 0);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, _bool)), src, srcw);
}

static void emit_load_int(struct sljit_compiler* compiler, unsigned reg, sljit_s32 target) {
    sljit_emit_op1(compiler, SLJIT_MOV, target, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, _i64)));
}

static void emit_guard_type(TinyJitEmitter* out, unsigned reg, py_Type type, int pc) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, type)));
    remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL,
                                 SLJIT_R2, 0, SLJIT_IMM, type), pc, true);
}

static void emit_check_bound(TinyJitEmitter* out, unsigned reg, int pc) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, type)));
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL,
                                 SLJIT_R2, 0, SLJIT_IMM, tp_nil), pc, true);
}

/* ------------------------------------------------------------------------ */
/* LIR islands                                                               */
/* ------------------------------------------------------------------------ */

/*
 * Model the unboxed dataflow of an integer island in LIR. Every result is
 * written back immediately, so an overflow/type exit can resume RegVM with
 * precisely the state before the failing bytecode. SSA values are also kept
 * available for later instructions within this island: coloring can eliminate
 * redundant reloads without making GC-visible roots depend on native regs.
 */
static bool build_lir_island(TinyJitIsland* island, const RegCode* code,
                             int first, int last) {
    island->first_pc = first;
    island->last_pc = last;
    jit_lir_init(&island->lir, (uint32_t)sizeof(py_TValue), 16u);
    JitLirFunction* lir = &island->lir;
    const JitLirBlockId block = jit_lir_add_block(lir, JIT_INVALID_BLOCK, 1u,
                                                   JIT_LIR_BLOCK_ENTRY);
    lir->entry_block = block;
    const JitTypeInfo type = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
    JitLirOperand cache[TINYJIT_MAX_REGS];
    for(unsigned i = 0; i < TINYJIT_MAX_REGS; ++i) cache[i] = jit_lir_none();

    for(int pc = first; pc < last; ++pc) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(word);
        const unsigned dst = RBC_A(word);
        const unsigned lhs = RBC_B(word);
        const bool immediate = is_arith_i8(op) || is_compare_i8(op);
        const unsigned rhs = RBC_C(word);
        const JitSourceLocation source = {pc, -1};
        const JitImmediate zero = {0};
        JitLirOperand inputs[2];
        const unsigned regs[2] = {lhs, rhs};

        if(dst >= code->nregs || lhs >= code->nregs ||
           (!immediate && rhs >= code->nregs)) return false;
        for(int i = 0; i < (immediate ? 1 : 2); ++i) {
            const unsigned index = regs[i];
            if(cache[index].kind == JIT_LIR_OPERAND_NONE) {
                JitVRegId id = jit_lir_new_vreg(lir, JIT_VREG_GPR, type,
                                                 JIT_INVALID_VALUE);
                cache[index] = jit_lir_vreg(id, type);
                jit_lir_emit(lir, block, JIT_LIR_INPUT, &cache[index], 1,
                             NULL, 0, zero, index, 0, 0, 0,
                             JIT_INVALID_LIR_BLOCK, JIT_INVALID_LIR_BLOCK,
                             JIT_INVALID_LIR_STATE, source);
            }
            inputs[i] = cache[index];
        }
        if(immediate) inputs[1] = jit_lir_imm_i64((int8_t)rhs, type);

        JitVRegId id = jit_lir_new_vreg(lir, JIT_VREG_GPR, type,
                                         JIT_INVALID_VALUE);
        JitLirOperand output = jit_lir_vreg(id, type);
        const JitLirOpcode lir_op = is_compare(op) ? JIT_LIR_CMP_I64 : int_binary_lir(op);
        if(lir_op == JIT_LIR__COUNT) return false;
        jit_lir_emit(lir, block, lir_op, &output, 1, inputs, 2,
                     zero, (uint32_t)op, 0, 0, 0,
                     JIT_INVALID_LIR_BLOCK, JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_STATE, source);

        /* aux: RegVM destination; aux2: 0 => int, 1 => bool. */
        jit_lir_emit(lir, block, JIT_LIR_SYNC_STATE, NULL, 0, &output, 1,
                     zero, dst, lir_op == JIT_LIR_CMP_I64 ? 1u : 0u, 0, 0,
                     JIT_INVALID_LIR_BLOCK, JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_STATE, source);
        cache[dst] = lir_op == JIT_LIR_CMP_I64 ? jit_lir_none() : output;
    }
    const JitImmediate zero = {0};
    jit_lir_emit(lir, block, JIT_LIR_RETURN, NULL, 0, NULL, 0,
                 zero, 0, 0, 0, 0,
                 JIT_INVALID_LIR_BLOCK, JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_STATE, (JitSourceLocation){last - 1, -1});

    char error[256];
    if(!jit_lir_verify(lir, error, sizeof(error))) return false;
    JitGraphColorConfig config;
    jit_graph_color_config_init(&config);
    config.gpr_scratch_count = TINYJIT_LIR_GPRS;
    config.reserved_gpr_scratch = TINYJIT_LIR_RESERVED_GPRS; /* R0/R1: emitter temporaries */
    config.gpr_saved_count = 3;
    config.reserved_gpr_saved = 3; /* S0-S2 hold registers, run state, entry PC. */
    config.fpr_scratch_count = 0;
    config.fpr_saved_count = 0;
    config.tvalue_size = (uint32_t)sizeof(py_TValue);
    config.tvalue_alignment = (uint32_t)_Alignof(py_TValue);
    config.stack_alignment = 16;
    JitRegAllocResult result;
    jit_regalloc_result_init(&result);
    const bool allocated = jit_graph_color_allocate(lir, &config, &result,
                                                     error, sizeof(error));
    bool applied = false;
    if(allocated) {
        island->stats = result.stats;
        c11__foreach(JitLocation, &result.locations, loc) {
            if(loc->kind == JIT_LOCATION_REGISTER) island->physical_vregs++;
        }
        applied = jit_regalloc_apply(lir, &result, error, sizeof(error));
    }
    jit_regalloc_result_destroy(&result);
    return allocated && applied && jit_lir_verify(lir, error, sizeof(error));
}

static void free_lir_islands(TinyJitIsland* islands, int count) {
    if(islands == NULL) return;
    for(int i = 0; i < count; i++) jit_lir_destroy(&islands[i].lir);
    free(islands);
}

/* Block starts: PC 0, every branch target and every profiled loop header. */
static bool native_entry_points(const RegCode* code, bool* entry) {
    entry[0] = true;
    for(int pc = 0; pc < code->code.length; ++pc) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(word);
        int target = -1;
        switch(op) {
            case RBC_JUMP:
            case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_TRUE:
            case RBC_JUMP_IF_TRUE_OR_POP: case RBC_JUMP_IF_FALSE_OR_POP:
            case RBC_FOR_ITER: case RBC_FOR_RANGE:
                target = pc + RBC_SBX(word);
                break;
            case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: {
                if(pc + 1 >= code->code.length) return false;
                const RegBytecode ext = c11__getitem(RegBytecode, &code->code, pc + 1);
                if(RBC_OP(ext) != RBC_SUPER_DATA) return false;
                target = pc + 1 + RBC_SBX(ext);
                int other = pc + 2 + RBC_A(ext);
                if(other < 0 || other >= code->code.length) return false;
                entry[other] = true;
                pc++; /* skip the payload word */
                break;
            }
            default: break;
        }
        if(target != -1) {
            if(target < 0 || target >= code->code.length) return false;
            entry[target] = true;
        }
    }
    c11__foreach(RegLoopProfile, &code->loop_profiles, loop) {
        if(loop->header_pc < 0 || loop->header_pc >= code->code.length) return false;
        entry[loop->header_pc] = true;
    }
    return true;
}

static bool prepare_lir_islands(const RegCode* code, const bool* entry, const uint8_t* kind,
                                TinyJitIsland** result,
                                int* island_count, uint32_t* max_stack,
                                uint32_t* colored, uint32_t* spilled,
                                uint32_t* lir_instructions) {
    const int length = code->code.length;
    TinyJitIsland* islands = calloc((size_t)length, sizeof(*islands));
    if(islands == NULL) return false;
    int count = 0;
    for(int pc = 0; pc < length;) {
        RegOpcode op = RBC_OP(c11__getitem(RegBytecode, &code->code, pc));
        if(op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH) {
            pc += 2; /* never treat the payload word as an instruction */
            continue;
        }
        if(kind[pc] != TINYJIT_SITE_ISLAND) {
            pc++;
            continue;
        }
        int first = pc;
        do {
            pc++;
        } while(pc < length && pc - first < TINYJIT_LIR_MAX_ISLAND && !entry[pc] &&
                kind[pc] == TINYJIT_SITE_ISLAND);
        TinyJitIsland* island = &islands[count];
        if(!build_lir_island(island, code, first, pc)) {
            free_lir_islands(islands, count + 1);
            return false;
        }
        *max_stack = *max_stack > island->lir.local_stack_size
                         ? *max_stack : island->lir.local_stack_size;
        *colored += island->physical_vregs;
        *spilled += island->stats.spilled_vregs;
        *lir_instructions += (uint32_t)c11__at(JitLirBlock, &island->lir.blocks, 0)->insts.length;
        count++;
    }
    *island_count = count;
    *result = islands;
    return true;
}

/* Convert a physically allocated LIR operand to SLJIT's source addressing. */
static bool lir_source(const JitLirFunction* lir, JitLirOperand operand,
                       sljit_s32* src, sljit_sw* srcw) {
    if(operand.kind == JIT_LIR_OPERAND_IMM_I64) {
        *src = SLJIT_IMM;
        *srcw = (sljit_sw)operand.as.i64;
        return true;
    }
    sljit_s32 r = lir_phys_reg(operand);
    if(r >= 0) {
        *src = r;
        *srcw = 0;
        return true;
    }
    if(operand.kind == JIT_LIR_OPERAND_STACK_SLOT &&
       operand.as.stack_slot < (JitStackSlotId)lir->stack_slots.length) {
        const JitStackSlot* slot = c11__at(JitStackSlot, &lir->stack_slots, operand.as.stack_slot);
        *src = SLJIT_MEM1(SLJIT_SP);
        *srcw = (sljit_sw)slot->offset;
        return true;
    }
    return false;
}

/*
 * Python floor division / modulo restricted to the RegVM int fast-path domain:
 * a zero divisor (ZeroDivisionError) and INT64_MIN operands (where the
 * interpreter's i64_abs-based helpers overflow) deoptimize to the interpreter,
 * so native and interpreted results are always identical.
 */
static void emit_divmod(TinyJitEmitter* out, JitLirOpcode op, sljit_s32 dst,
                        sljit_s32 lhs, sljit_sw lhsw, sljit_s32 rhs, sljit_sw rhsw, int pc) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, rhs, rhsw);
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0), pc, true);
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, INT64_MIN), pc, true);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, lhs, lhsw);
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, INT64_MIN), pc, true);
    /* Only R0, R1 and SLJIT's private temporary are touched; colored values survive. */
    sljit_emit_op0(c, SLJIT_DIVMOD_SW); /* R0 = truncated quotient, R1 = remainder */

    /* Truncated -> floored: adjust when the remainder's sign differs from the divisor's. */
    struct sljit_jump* exact = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
    struct sljit_jump* rem_negative = sljit_emit_cmp(c, SLJIT_SIG_LESS, SLJIT_R1, 0, SLJIT_IMM, 0);
    /* rhs is a colored register (never R0/R1) or a spill slot, so it is still intact. */
    struct sljit_jump* same_sign_pos = sljit_emit_cmp(c, SLJIT_SIG_GREATER_EQUAL,
                                                       rhs, rhsw, SLJIT_IMM, 0);
    struct sljit_jump* to_adjust = sljit_emit_jump(c, SLJIT_JUMP);
    struct sljit_label* negative = sljit_emit_label(c);
    struct sljit_jump* same_sign_neg = sljit_emit_cmp(c, SLJIT_SIG_LESS, rhs, rhsw, SLJIT_IMM, 0);
    struct sljit_label* adjust = sljit_emit_label(c);
    sljit_emit_op2(c, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0, rhs, rhsw);
    struct sljit_label* done = sljit_emit_label(c);
    if(exact == NULL || rem_negative == NULL || same_sign_pos == NULL || to_adjust == NULL ||
       negative == NULL || same_sign_neg == NULL || adjust == NULL || done == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(rem_negative, negative);
    sljit_set_label(to_adjust, adjust);
    sljit_set_label(exact, done);
    sljit_set_label(same_sign_pos, done);
    sljit_set_label(same_sign_neg, done);
    sljit_emit_op1(c, SLJIT_MOV, dst, 0, op == JIT_LIR_DIV_I64 ? SLJIT_R0 : SLJIT_R1, 0);
}

static bool emit_lir_int_binary(TinyJitEmitter* out, const JitLirFunction* lir,
                                const JitLirInst* inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
    const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);
    if(inst->defs.count != 1 || inst->uses.count != 2) return false;
    const sljit_s32 dst = lir_phys_reg(defs[0]);
    sljit_s32 lhs, rhs;
    sljit_sw lhsw, rhsw;
    if(dst < 0 || !lir_source(lir, uses[0], &lhs, &lhsw) ||
       !lir_source(lir, uses[1], &rhs, &rhsw)) return false;
    switch(inst->op) {
        case JIT_LIR_ADD_I64:
        case JIT_LIR_SUB_I64:
        case JIT_LIR_MUL_I64: {
            const sljit_s32 op = inst->op == JIT_LIR_ADD_I64 ? SLJIT_ADD :
                                 inst->op == JIT_LIR_SUB_I64 ? SLJIT_SUB : SLJIT_MUL;
            /* The interpreter wraps on overflow; resume it to get its exact result. */
            sljit_emit_op2(c, op | SLJIT_SET_OVERFLOW, dst, 0, lhs, lhsw, rhs, rhsw);
            remember(out, sljit_emit_jump(c, SLJIT_OVERFLOW), pc, true);
            return true;
        }
        case JIT_LIR_AND_I64:
        case JIT_LIR_OR_I64:
        case JIT_LIR_XOR_I64: {
            const sljit_s32 op = inst->op == JIT_LIR_AND_I64 ? SLJIT_AND :
                                 inst->op == JIT_LIR_OR_I64 ? SLJIT_OR : SLJIT_XOR;
            sljit_emit_op2(c, op, dst, 0, lhs, lhsw, rhs, rhsw);
            return true;
        }
        case JIT_LIR_SHL_I64:
        case JIT_LIR_SHR_I64:
            /* Negative or >= 64 shift counts are not defined by the int fast path. */
            if(rhs == SLJIT_IMM) {
                if((sljit_uw)rhsw >= 64u) emit_side_exit(out, pc);
            } else {
                remember(out, sljit_emit_cmp(c, SLJIT_GREATER_EQUAL, rhs, rhsw,
                                             SLJIT_IMM, 64), pc, true);
            }
            sljit_emit_op2(c, inst->op == JIT_LIR_SHL_I64 ? SLJIT_SHL : SLJIT_ASHR,
                           dst, 0, lhs, lhsw, rhs, rhsw);
            return true;
        case JIT_LIR_DIV_I64:
        case JIT_LIR_MOD_I64:
            if(rhs == SLJIT_IMM) return false; /* FLOORDIV/MOD have no immediate forms */
            emit_divmod(out, inst->op, dst, lhs, lhsw, rhs, rhsw, pc);
            return true;
        default: return false;
    }
}

static bool emit_lir_island(TinyJitEmitter* out, const TinyJitIsland* island) {
    const JitLirFunction* lir = &island->lir;
    const JitLirBlock* block = c11__at(JitLirBlock, &lir->blocks, 0);
    struct sljit_compiler* c = out->compiler;
    int last_pc = island->first_pc - 1;
    c11__foreach(JitLirInst, &block->insts, inst) {
        if(inst->removed || inst->op == JIT_LIR_NOP) continue;
        const int pc = inst->source.bytecode_ip;
        if(pc < island->first_pc || pc >= island->last_pc || pc < last_pc)
            return false;
        if(pc != last_pc) {
            out->blocks[pc] = sljit_emit_label(c);
            if(out->blocks[pc] == NULL) return false;
            last_pc = pc;
        }
        const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
        const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);
        if(inst->op == JIT_LIR_RETURN) continue;
        if(inst->op == JIT_LIR_INPUT) {
            if(inst->defs.count != 1 || inst->aux >= TINYJIT_MAX_REGS) return false;
            const sljit_s32 dst = lir_phys_reg(defs[0]);
            if(dst < 0) return false;
            /* R0/R1 are reserved emitters; they cannot hold colored values. */
            sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R0, 0,
                           SLJIT_MEM1(SLJIT_S0), field_offset(inst->aux, offsetof(py_TValue, type)));
            remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R0, 0,
                                         SLJIT_IMM, tp_int), pc, true);
            sljit_emit_op1(c, SLJIT_MOV, dst, 0, SLJIT_MEM1(SLJIT_S0),
                           field_offset(inst->aux, offsetof(py_TValue, _i64)));
        } else if(inst->op == JIT_LIR_SPILL_LOAD) {
            if(inst->defs.count != 1 || inst->uses.count != 1) return false;
            sljit_s32 src;
            sljit_sw srcw;
            const sljit_s32 dst = lir_phys_reg(defs[0]);
            if(dst < 0 || !lir_source(lir, uses[0], &src, &srcw)) return false;
            sljit_emit_op1(c, SLJIT_MOV, dst, 0, src, srcw);
        } else if(inst->op == JIT_LIR_SPILL_STORE) {
            if(inst->defs.count != 0 || inst->uses.count != 2 ||
               uses[0].kind != JIT_LIR_OPERAND_STACK_SLOT) return false;
            sljit_s32 src;
            sljit_sw srcw;
            if(!lir_source(lir, uses[1], &src, &srcw)) return false;
            const JitStackSlot* slot = c11__at(JitStackSlot, &lir->stack_slots,
                                                uses[0].as.stack_slot);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), slot->offset,
                           src, srcw);
        } else if(inst->op == JIT_LIR_CMP_I64) {
            if(inst->defs.count != 1 || inst->uses.count != 2) return false;
            const sljit_s32 dst = lir_phys_reg(defs[0]);
            sljit_s32 lhs, rhs;
            sljit_sw lhsw, rhsw;
            if(dst < 0 || !lir_source(lir, uses[0], &lhs, &lhsw) ||
               !lir_source(lir, uses[1], &rhs, &rhsw)) return false;
            sljit_s32 cond = compare_condition((RegOpcode)inst->aux);
            if(cond < 0) return false;
            struct sljit_jump* yes = sljit_emit_cmp(c, cond, lhs, lhsw, rhs, rhsw);
            sljit_emit_op1(c, SLJIT_MOV, dst, 0, SLJIT_IMM, 0);
            struct sljit_jump* done = sljit_emit_jump(c, SLJIT_JUMP);
            struct sljit_label* yes_label = sljit_emit_label(c);
            if(yes == NULL || done == NULL || yes_label == NULL) return false;
            sljit_set_label(yes, yes_label);
            sljit_emit_op1(c, SLJIT_MOV, dst, 0, SLJIT_IMM, 1);
            struct sljit_label* end_label = sljit_emit_label(c);
            if(end_label == NULL) return false;
            sljit_set_label(done, end_label);
        } else if(inst->op == JIT_LIR_SYNC_STATE) {
            if(inst->defs.count != 0 || inst->uses.count != 1 ||
               inst->aux >= TINYJIT_MAX_REGS || inst->aux2 > 1u) return false;
            sljit_s32 src;
            sljit_sw srcw;
            if(!lir_source(lir, uses[0], &src, &srcw)) return false;
            if(inst->aux2) {
                emit_store_bool(c, inst->aux, src, srcw);
            } else {
                emit_store_int(c, inst->aux, src, srcw);
            }
        } else if(!emit_lir_int_binary(out, lir, inst, pc)) {
            return false;
        }
        if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Control flow and non-island instructions                                  */
/* ------------------------------------------------------------------------ */

/* Bound native loops so GC, signals and profiling regain control regularly. */
static void emit_backedge_poll(TinyJitEmitter* out, int target) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_Z,
                   SLJIT_MEM1(SLJIT_S1), offsetof(TinyJitRun, fuel),
                   SLJIT_MEM1(SLJIT_S1), offsetof(TinyJitRun, fuel), SLJIT_IMM, 1);
    remember(out, sljit_emit_jump(c, SLJIT_ZERO), target, true);
}

static void emit_branch(TinyJitEmitter* out, int from, int target) {
    if(target <= from) emit_backedge_poll(out, target);
    remember(out, sljit_emit_jump(out->compiler, SLJIT_JUMP), target, false);
}

static void emit_conditionally_branch(TinyJitEmitter* out, sljit_s32 type,
                                      sljit_s32 lhs, sljit_sw lhsw,
                                      sljit_s32 rhs, sljit_sw rhsw,
                                      int from, int target) {
    struct sljit_compiler* c = out->compiler;
    if(target > from) {
        remember(out, sljit_emit_cmp(c, type, lhs, lhsw, rhs, rhsw), target, false);
        return;
    }
    struct sljit_jump* skip = sljit_emit_cmp(c, invert_condition(type),
                                             lhs, lhsw, rhs, rhsw);
    if(skip == NULL) { out->failed = true; return; }
    emit_branch(out, from, target);
    struct sljit_label* after = sljit_emit_label(c);
    if(after == NULL) { out->failed = true; return; }
    sljit_set_label(skip, after);
}

/* Interpret int and bool exactly as RegVM does for the common branch cases. */
static void emit_truthy_input(TinyJitEmitter* out, unsigned reg, int pc) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, type)));
    struct sljit_jump* is_int = sljit_emit_cmp(c, SLJIT_EQUAL,
                                               SLJIT_R2, 0, SLJIT_IMM, tp_int);
    remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL,
                                 SLJIT_R2, 0, SLJIT_IMM, tp_bool), pc, true);
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, _bool)));
    struct sljit_jump* join = sljit_emit_jump(c, SLJIT_JUMP);
    struct sljit_label* int_label = sljit_emit_label(c);
    if(is_int == NULL || join == NULL || int_label == NULL) {
        out->failed = true; return;
    }
    sljit_set_label(is_int, int_label);
    emit_load_int(c, reg, SLJIT_R0);
    struct sljit_label* done = sljit_emit_label(c);
    if(done == NULL) { out->failed = true; return; }
    sljit_set_label(join, done);
}

static void emit_cmp_branch(TinyJitEmitter* out, RegOpcode op,
                            RegBytecode inst, RegBytecode ext, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    const unsigned b = RBC_B(inst);
    const unsigned mode = RBC_C(inst);
    const bool imm = op == RBC_CMP_I8_BRANCH;
    const sljit_s32 condition = compare_condition(compare_from_code(mode));
    const sljit_s32 test = (mode & REG_CMP_BRANCH_TRUE) != 0
                               ? condition : invert_condition(condition);
    const int target = pc + 1 + RBC_SBX(ext);
    const int other = pc + 2 + RBC_A(ext);
    emit_guard_type(out, a, tp_int, pc);
    if(!imm) emit_guard_type(out, b, tp_int, pc);
    emit_load_int(c, a, SLJIT_R0);
    if(imm) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (int8_t)b);
    } else {
        emit_load_int(c, b, SLJIT_R1);
    }
    emit_conditionally_branch(out, test, SLJIT_R0, 0, SLJIT_R1, 0, pc, target);
    emit_branch(out, pc, other);
}

/*
 * FOR_ITER over a range_iterator, mirroring range_iterator__iternext() in place:
 * R[A] holds the iterator, R[A + 1] receives the next int, and an exhausted
 * iterator branches to pc + sBx. Any other iterator type is a side exit.
 */
static void emit_range_iteration(TinyJitEmitter* out, RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    const int target = pc + RBC_SBX(inst);
    const sljit_sw ud = (sljit_sw)offsetof(PyObject, flex);
    const sljit_sw current = ud + (sljit_sw)offsetof(RangeIterator, current);
    const sljit_sw stop = ud + (sljit_sw)offsetof(RangeIterator, range.stop);
    const sljit_sw step = ud + (sljit_sw)offsetof(RangeIterator, range.step);

    emit_guard_type(out, a, tp_range_iterator, pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(a, offsetof(py_TValue, _obj)));
    /* Userdata directly follows the header only when the object has no slots. */
    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0),
                   (sljit_sw)offsetof(PyObject, slots));
    remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0), pc, true);

    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), current);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), step);
    struct sljit_jump* descending = sljit_emit_cmp(c, SLJIT_SIG_LESS_EQUAL,
                                                   SLJIT_R2, 0, SLJIT_IMM, 0);
    struct sljit_jump* done_up = sljit_emit_cmp(c, SLJIT_SIG_GREATER_EQUAL,
                                                SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), stop);
    struct sljit_jump* produce = sljit_emit_jump(c, SLJIT_JUMP);
    struct sljit_label* down = sljit_emit_label(c);
    struct sljit_jump* done_down = sljit_emit_cmp(c, SLJIT_SIG_LESS_EQUAL,
                                                  SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), stop);
    struct sljit_label* value = sljit_emit_label(c);
    emit_store_int(c, a + 1, SLJIT_R1, 0);
    /* Like the interpreter, current += step wraps without a check. */
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_MEM1(SLJIT_R0), current,
                   SLJIT_R1, 0, SLJIT_R2, 0);
    struct sljit_jump* next = sljit_emit_jump(c, SLJIT_JUMP);
    struct sljit_label* exhausted = sljit_emit_label(c);
    if(descending == NULL || done_up == NULL || produce == NULL || down == NULL ||
       done_down == NULL || value == NULL || next == NULL || exhausted == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(descending, down);
    sljit_set_label(produce, value);
    sljit_set_label(done_up, exhausted);
    sljit_set_label(done_down, exhausted);
    emit_branch(out, pc, target);
    struct sljit_label* after = sljit_emit_label(c);
    if(after == NULL) { out->failed = true; return; }
    sljit_set_label(next, after);
}

/* ------------------------------------------------------------------------ */
/* Shared inline caches                                                      */
/*                                                                           */
/* These read the RegVM's RegInlineCache for the site at run time and mirror */
/* reg_*_cache_apply() exactly. Any guard failure jumps to the site's slow   */
/* path, which runs the interpreter's own implementation (RegVM helper)     */
/* and may refill the cache - native code uses the refilled cache on the     */
/* very next execution. Only R0-R2 are used: no island value is live outside */
/* islands.                                                                  */
/* ------------------------------------------------------------------------ */

#define IC_FIELD(field) ((sljit_sw)offsetof(RegInlineCache, field))
#define RUN_FIELD(field) ((sljit_sw)offsetof(TinyJitRun, field))

/* A failed IC guard runs the interpreter's implementation and stays native. */
static void ic_miss(TinyJitEmitter* out, struct sljit_jump* jump, int pc) {
    remember_target(out, jump, pc, TINYJIT_TARGET_SLOW);
}

/* R0 = &run->caches[cache index of this site]; exits unless its kind matches. */
static void emit_cache_entry(TinyJitEmitter* out, int pc, RegInlineCacheKind kind) {
    struct sljit_compiler* c = out->compiler;
    const sljit_sw offset = (sljit_sw)site_cache_index(out->code, pc) * (sljit_sw)sizeof(RegInlineCache);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(caches));
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, offset);
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(kind));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, kind), pc);
}

/* Exit unless the cache's variant byte equals `variant`. Uses R1. */
static void emit_require_variant(TinyJitEmitter* out, int pc, unsigned variant) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(variant));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, variant), pc);
}

/* Copy one py_TValue from [src + offset] to RegVM register `dst`, via R0. */
static void emit_load_value(struct sljit_compiler* c, unsigned dst, sljit_s32 src, sljit_sw offset) {
    for(size_t i = 0; i < sizeof(py_TValue); i += sizeof(uint64_t)) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(src), offset + (sljit_sw)i);
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), field_offset(dst, i), SLJIT_R0, 0);
    }
}

/* Copy RegVM register `src` to [dst + offset], via R0. */
static void emit_store_value(struct sljit_compiler* c, sljit_s32 dst, sljit_sw offset, unsigned src) {
    for(size_t i = 0; i < sizeof(py_TValue); i += sizeof(uint64_t)) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), field_offset(src, i));
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(dst), offset + (sljit_sw)i, SLJIT_R0, 0);
    }
}

/* R1 = run->dict; exit unless it is the cached dict at the cached version. Uses R2. */
static void emit_dict_guard(TinyJitEmitter* out, int pc, sljit_sw run_field,
                            sljit_sw guard_field, sljit_sw version_field) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), run_field);
    /*
     * No namespace (e.g. exec() with plain dict globals): a cache whose fill
     * failed is still zeroed with guard 0, which must not "match" NULL.
     */
    ic_miss(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0), pc);
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0,
                                 SLJIT_MEM1(SLJIT_R0), guard_field), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(NameDict, version));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                 SLJIT_MEM1(SLJIT_R0), version_field), pc);
}

/* R1 = dict, R0 = cache: R[dst] = dict->items[cache->index].value if its key is `name`. */
static void emit_dict_item(TinyJitEmitter* out, int pc, sljit_sw index_field,
                           py_Name name, unsigned dst) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), index_field);
    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(NameDict, capacity));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_GREATER_EQUAL, SLJIT_R2, 0, SLJIT_R0, 0), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(NameDict, items));
    sljit_emit_op2(c, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)sizeof(NameDict_KV));
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R1, 0);
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_MEM1(SLJIT_R2),
                                 (sljit_sw)offsetof(NameDict_KV, key),
                                 SLJIT_IMM, (sljit_sw)(uintptr_t)name), pc);
    emit_load_value(c, dst, SLJIT_R2, (sljit_sw)offsetof(NameDict_KV, value));
}

/* LOAD_GLOBAL[_IC] A, Bx: mirrors reg_global_cache_apply(). */
static void emit_load_global(TinyJitEmitter* out, const CodeObject* source,
                             RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    const py_Name name = c11__getitem(py_Name, &source->names, RBC_BX(inst));
    emit_cache_entry(out, pc, REG_IC_KIND_GLOBAL);
    /* The module namespace is always validated, even for builtins. */
    emit_dict_guard(out, pc, RUN_FIELD(globals), IC_FIELD(guard0), IC_FIELD(version0));
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(variant));
    struct sljit_jump* builtin = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R2, 0,
                                                SLJIT_IMM, REG_GLOBAL_CACHE_BUILTIN);
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                 SLJIT_IMM, REG_GLOBAL_CACHE_GLOBAL), pc);
    emit_dict_item(out, pc, IC_FIELD(index0), name, a);
    struct sljit_jump* done = sljit_emit_jump(c, SLJIT_JUMP);
    struct sljit_label* builtin_label = sljit_emit_label(c);
    emit_dict_guard(out, pc, RUN_FIELD(builtins), IC_FIELD(guard1), IC_FIELD(version1));
    emit_dict_item(out, pc, IC_FIELD(index1), name, a);
    struct sljit_label* done_label = sljit_emit_label(c);
    if(builtin == NULL || done == NULL || builtin_label == NULL || done_label == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(builtin, builtin_label);
    sljit_set_label(done, done_label);
}

/*
 * R0 = cache, `reg` = the instance: exit unless it is a shape-backed object of
 * the cached type at the cached type_version whose shape is cache->guard0.
 * Leaves R1 = ShapeStorage*. Uses R2.
 */
static void emit_shape_guard(TinyJitEmitter* out, int pc, unsigned reg) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, type)));
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(type));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_R2, 0), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(type_version));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0,
                                 SLJIT_MEM1(SLJIT_R0), IC_FIELD(version0)), pc);
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, is_ptr)));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(reg, offsetof(py_TValue, _obj)));
    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(PyObject, slots));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                 SLJIT_IMM, PK_OBJ_SHAPED_SLOTS), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(PyObject, flex));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(ShapeStorage, shape));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                 SLJIT_MEM1(SLJIT_R0), IC_FIELD(guard0)), pc);
}

/* R2 = &storage(R1)->values[cache(R0)->index0] - offsetof(values). */
static void emit_shape_slot_address(struct sljit_compiler* c) {
    sljit_emit_op1(c, SLJIT_MOV_U32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(index0));
    sljit_emit_op2(c, SLJIT_MUL, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)sizeof(py_TValue));
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R1, 0);
}

/* LOAD_ATTR[_IC] A, Bx (in place): the REG_ATTR_CACHE_SHAPE_SLOT variant. */
static void emit_load_attr(TinyJitEmitter* out, RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    const sljit_sw values = (sljit_sw)offsetof(ShapeStorage, values);
    emit_cache_entry(out, pc, REG_IC_KIND_ATTR);
    emit_require_variant(out, pc, REG_ATTR_CACHE_SHAPE_SLOT);
    emit_shape_guard(out, pc, a);
    emit_shape_slot_address(c);
    /* A deleted attribute leaves a nil slot: the interpreter raises/falls back. */
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R2),
                   values + (sljit_sw)offsetof(py_TValue, type));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, tp_nil), pc);
    emit_load_value(c, a, SLJIT_R2, values);
}

/* STORE_ATTR[_IC] A, Bx: R[A + 1].name = R[A]; mirrors reg_store_attr_cache_apply(). */
static void emit_store_attr(TinyJitEmitter* out, RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    emit_cache_entry(out, pc, REG_IC_KIND_STORE_ATTR);
    emit_shape_guard(out, pc, a + 1);
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(variant));
    struct sljit_jump* plain = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R2, 0,
                                              SLJIT_IMM, REG_STORE_ATTR_CACHE_SLOT);
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                 SLJIT_IMM, REG_STORE_ATTR_CACHE_TRANSITION), pc);
    /* Transition: only while the storage has room (growth reallocates). */
    sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(ShapeStorage, capacity));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_LESS_EQUAL | SLJIT_32, SLJIT_R2, 0,
                                 SLJIT_MEM1(SLJIT_R0), IC_FIELD(index0)), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(guard1));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), (sljit_sw)offsetof(ShapeStorage, shape),
                   SLJIT_R2, 0);
    struct sljit_label* store = sljit_emit_label(c);
    if(plain == NULL || store == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(plain, store);
    emit_shape_slot_address(c);
    emit_store_value(c, SLJIT_R2, (sljit_sw)offsetof(ShapeStorage, values), a);
}

/*
 * LOAD_METHOD[_IC] A, Bx: R[A + 1] = R[A]; R[A] = *cache->slot. Mirrors the
 * REG_METHOD_CACHE_BIND_SELF variant of reg_method_cache_apply(): the fill
 * already rejected getunboundmethod hooks and super (both guarded by
 * type_version). An instance must not shadow the method: objects without a
 * namespace never do, a shaped object only at the shape verified in guard0;
 * everything else takes the slow path.
 */
static void emit_load_method(TinyJitEmitter* out, RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const unsigned a = RBC_A(inst);
    emit_cache_entry(out, pc, REG_IC_KIND_METHOD);
    emit_require_variant(out, pc, REG_METHOD_CACHE_BIND_SELF);
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(a, offsetof(py_TValue, type)));
    sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(type));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_R2, 0), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(type_version));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0,
                                SLJIT_MEM1(SLJIT_R0), IC_FIELD(version0)), pc);

    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(a, offsetof(py_TValue, is_ptr)));
    struct sljit_jump* scalar = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                   field_offset(a, offsetof(py_TValue, _obj)));
    sljit_emit_op1(c, SLJIT_MOV_S32, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(PyObject, slots));
    struct sljit_jump* no_namespace = sljit_emit_cmp(c, SLJIT_SIG_GREATER_EQUAL,
                                                     SLJIT_R2, 0, SLJIT_IMM, 0);
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                SLJIT_IMM, PK_OBJ_SHAPED_SLOTS), pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(PyObject, flex));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1),
                   (sljit_sw)offsetof(ShapeStorage, shape));
    ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R2, 0,
                                SLJIT_MEM1(SLJIT_R0), IC_FIELD(guard0)), pc);
    struct sljit_label* bind = sljit_emit_label(c);
    if(scalar == NULL || no_namespace == NULL || bind == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(scalar, bind);
    sljit_set_label(no_namespace, bind);

    /* self first (R0 still holds the cache), then the cached function. */
    for(size_t i = 0; i < sizeof(py_TValue); i += sizeof(uint64_t)) {
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), field_offset(a, i));
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), field_offset(a + 1, i), SLJIT_R1, 0);
    }
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R0), IC_FIELD(slot));
    emit_load_value(c, a, SLJIT_R2, 0);
}

/* ------------------------------------------------------------------------ */
/* Helper calls                                                              */
/* ------------------------------------------------------------------------ */

/*
 * Runs the instruction at `pc` through the interpreter's implementation. The
 * callee may have added inline caches (moving the array) or changed types, so
 * the per-entry state is refreshed before native code continues.
 */
static sljit_sw SLJIT_FUNC tinyjit_helper_step(TinyJitRun* run, sljit_sw pc, RegJitStepFn step) {
    run->pc = pc; /* where a frame introspection during the call would force objects */
    const bool ok = step(run->vm, run->frame, run->code, (int32_t)pc);
    run->caches = (RegInlineCache*)run->code->inline_caches.data;
    run->type_version = run->vm->type_version;
    run->helper_calls++;
    return ok;
}

/*
 * helper(run, pc, step); a false result leaves through the error exit of `pc`.
 * Only S0/S1/S2 survive the call, which is all native code keeps outside islands.
 */
static void emit_helper_call(TinyJitEmitter* out, int pc) {
    struct sljit_compiler* c = out->compiler;
    const RegJitStepFn step =
        RegVM__jit_step_fn(RBC_OP(c11__getitem(RegBytecode, &out->code->code, pc)));
    if(step == NULL) {
        out->failed = true;
        return;
    }
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)pc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)(uintptr_t)step);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS3(W, P, W, P), SLJIT_IMM,
                     SLJIT_FUNC_ADDR(tinyjit_helper_step));
    remember_target(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0),
                    pc, TINYJIT_TARGET_ERROR);
}

/* ------------------------------------------------------------------------ */
/* Scalar replacement (escape.h)                                             */
/*                                                                           */
/* A virtual object lives as its components in the GC-rooted save area      */
/* (run->vsave) while its pending bit is set; the registers naming it hold   */
/* nil. Whatever leaves native code first stores the real object into those  */
/* registers, so the interpreter always sees the unoptimized state.          */
/* ------------------------------------------------------------------------ */

static sljit_sw virtual_bit(int index) { return (sljit_sw)1 << index; }

/* Create virtual `index` for real into every register of `regs`. */
static void materialize_virtual(TinyJitRun* run, int index, const JitEaRegs* regs) {
    const JitEaVirtual* v = &run->compiled->ea.virtuals[index];
    py_TValue* R = run->frame->locals;
    run->pending &= ~virtual_bit(index);
    int first = -1;
    for(int r = 0; r < 256 && first < 0; r++) {
        if(JitEaRegs__has(regs, (unsigned)r)) first = r;
    }
    if(first < 0) return;
    py_TValue* save = run->vsave + v->save_offset;
    /* The result register is rooted before anything else can allocate. */
    switch(v->kind) {
        case JIT_EA_TUPLE: {
            py_Ref items = py_newtuple(&R[first], v->component_count);
            for(int i = 0; i < v->component_count; i++) items[i] = save[i];
            break;
        }
        case JIT_EA_LIST:
            py_newlistn(&R[first], v->component_count);
            for(int i = 0; i < v->component_count; i++) py_list_setitem(&R[first], i, &save[i]);
            break;
        case JIT_EA_OBJECT:
            /*
             * Exactly the state __init__ produced at the allocation, written
             * directly in its store order: re-running __init__ (or setattr)
             * would observe class changes made since.
             */
            py_newobject(&R[first], v->type, PK_OBJ_SHAPED_SLOTS, 0);
            for(int i = 0; i < v->field_count; i++) {
                const JitEaField* field = &v->fields[i];
                py_TValue value = field->component >= 0 ? save[field->component] : field->constant;
                py_setdict(&R[first], field->name, &value);
            }
            break;
    }
    for(int r = first + 1; r < 256; r++) {
        if(JitEaRegs__has(regs, (unsigned)r)) R[r] = R[first];
    }
    TinyJitContext* context = run->vm->regjit_userdata;
    if(context != NULL) context->materialized++;
}

/* Every pending virtual object named at `pc` becomes real. */
static void materialize_at(TinyJitRun* run, int32_t pc) {
    const JitEaResult* ea = &run->compiled->ea;
    for(int i = 0; i < ea->virtual_count; i++) {
        if((run->pending & virtual_bit(i)) == 0) continue;
        const JitEaRegs* regs = JitEa__points_at(&ea->virtuals[i], pc);
        if(regs != NULL) materialize_virtual(run, i, regs);
    }
}

/* Resume exit of a PC inside some object's lifetime. */
static sljit_sw SLJIT_FUNC tinyjit_materialize(TinyJitRun* run, sljit_sw pc) {
    materialize_at(run, (int32_t)pc);
    return 1;
}

/* The VM's type_version moved: are the assumptions about this class still true? */
static sljit_sw SLJIT_FUNC tinyjit_revalidate(TinyJitRun* run, sljit_sw index) {
    TinyJitCode* compiled = (TinyJitCode*)run->compiled;
    if(!JitEa__object_valid(&compiled->ea.virtuals[index])) return 0;
    compiled->validated[index] = run->vm->type_version;
    return 1;
}

/* Copy n values between RegVM registers and the save area (base in R1), via R0. */
static void emit_save_copy(struct sljit_compiler* c, bool to_save, unsigned reg,
                           unsigned slot, unsigned n) {
    for(unsigned i = 0; i < n; i++) {
        for(size_t w = 0; w < sizeof(py_TValue); w += sizeof(uint64_t)) {
            const sljit_sw at_reg = field_offset(reg + i, w);
            const sljit_sw at_save = (sljit_sw)((slot + i) * sizeof(py_TValue) + w);
            if(to_save) {
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), at_reg);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_R1), at_save, SLJIT_R0, 0);
            } else {
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), at_save);
                sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), at_reg, SLJIT_R0, 0);
            }
        }
    }
}

/* Exit unless run->type_version is the version virtual `index` was validated at. */
static void emit_validated_guard(TinyJitEmitter* out, int index, int pc, bool revalidate) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(type_version));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM,
                   (sljit_sw)(uintptr_t)&out->target->validated[index]);
    if(!revalidate) {
        remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R2), 0),
                 pc, true);
        return;
    }
    struct sljit_jump* valid = sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R2), 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)index);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2(W, P, W), SLJIT_IMM,
                     SLJIT_FUNC_ADDR(tinyjit_revalidate));
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0), pc, true);
    struct sljit_label* ok = sljit_emit_label(c);
    if(valid == NULL || ok == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(valid, ok);
}

/*
 * The allocation. Objects are guarded first (exactly this class, the class
 * state the analysis assumed); a failed guard leaves before anything changed,
 * so the interpreter performs the real call.
 */
static void emit_virtual_build(TinyJitEmitter* out, int index, int pc) {
    struct sljit_compiler* c = out->compiler;
    const JitEaVirtual* v = &out->ea->virtuals[index];
    const unsigned a = v->result_reg;
    if(v->kind == JIT_EA_OBJECT) {
        const py_TypeInfo* ti = pk_typeinfo(v->type);
        sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                       field_offset(a, offsetof(py_TValue, type)));
        remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, tp_type), pc, true);
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                       field_offset(a, offsetof(py_TValue, _obj)));
        remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM,
                                     (sljit_sw)(uintptr_t)ti->self._obj), pc, true);
        sljit_emit_op1(c, SLJIT_MOV_S16, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0),
                       field_offset(a + 1, offsetof(py_TValue, type)));
        remember(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, tp_nil), pc, true);
        emit_validated_guard(out, index, pc, true);
    }
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(vsave));
    emit_save_copy(c, true, v->component_base, v->save_offset, v->component_count);
    sljit_emit_op2(c, SLJIT_OR, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(pending),
                   SLJIT_MEM1(SLJIT_S1), RUN_FIELD(pending), SLJIT_IMM, virtual_bit(index));
    emit_template(c, a, py_NIL());
}

/*
 * A use reads the components. If a frame introspection forced the object in
 * the meantime (pending bit clear), the registers hold the real object and the
 * interpreter performs the real operation; for objects, a class change since
 * validation does the same after materializing.
 */
static void emit_virtual_use(TinyJitEmitter* out, const JitEaSite* site, RegBytecode inst, int pc) {
    struct sljit_compiler* c = out->compiler;
    const JitEaVirtual* v = &out->ea->virtuals[site->virtual_index];
    const unsigned a = RBC_A(inst);
    sljit_emit_op2u(c, SLJIT_AND | SLJIT_SET_Z, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(pending),
                    SLJIT_IMM, virtual_bit(site->virtual_index));
    remember(out, sljit_emit_jump(c, SLJIT_ZERO), pc, true);
    if(v->kind == JIT_EA_OBJECT) emit_validated_guard(out, site->virtual_index, pc, false);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(vsave));
    switch(site->action) {
        case JIT_EA_UNPACK:
            emit_save_copy(c, false, a, v->save_offset, v->component_count);
            break;
        case JIT_EA_SUBSCR:
            emit_save_copy(c, false, a, v->save_offset + site->arg, 1);
            break;
        case JIT_EA_ATTR: {
            const JitEaField* field = &v->fields[site->arg];
            if(field->component >= 0) {
                emit_save_copy(c, false, a, v->save_offset + (unsigned)field->component, 1);
            } else {
                emit_template(c, a, &field->constant);
            }
            break;
        }
        default: out->failed = true; break;
    }
}

/* Pending bits of objects whose holding range ends before `pc`. */
static void emit_kill(TinyJitEmitter* out, sljit_sw mask) {
    if(mask == 0) return;
    sljit_emit_op2(out->compiler, SLJIT_AND, SLJIT_MEM1(SLJIT_S1), RUN_FIELD(pending),
                   SLJIT_MEM1(SLJIT_S1), RUN_FIELD(pending), SLJIT_IMM, ~mask);
}

/* IC fast path already emitted: skip over the slow path that follows it. */
static void emit_slow_path(TinyJitEmitter* out, int pc) {
    struct sljit_compiler* c = out->compiler;
    struct sljit_jump* done = sljit_emit_jump(c, SLJIT_JUMP);
    out->slow[pc] = sljit_emit_label(c);
    emit_helper_call(out, pc);
    struct sljit_label* after = sljit_emit_label(c);
    if(done == NULL || out->slow[pc] == NULL || after == NULL) {
        out->failed = true;
        return;
    }
    sljit_set_label(done, after);
}

static bool emit_instructions(TinyJitEmitter* out,
                              const RegCode* code, const CodeObject* source,
                              const TinyJitIsland* islands, int island_count) {
    struct sljit_compiler* c = out->compiler;
    int island_index = 0;
    for(int pc = 0; pc < code->code.length; pc++) {
        if(island_index < island_count && islands[island_index].first_pc == pc) {
            const TinyJitIsland* island = &islands[island_index];
            /* A holding range ending inside the island ends at its start: island
             * instructions only do integer arithmetic and cannot observe it. */
            sljit_sw mask = 0;
            for(int at = island->first_pc; at < island->last_pc; at++) mask |= out->kill_mask[at];
            struct sljit_label* head = NULL;
            if(mask != 0) {
                head = sljit_emit_label(c);
                if(head == NULL) return false;
                emit_kill(out, mask);
            }
            if(!emit_lir_island(out, island)) return false;
            if(head != NULL) out->blocks[island->first_pc] = head;
            pc = island->last_pc - 1;
            island_index++;
            continue;
        }
        const RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(inst);
        const unsigned a = RBC_A(inst);
        const unsigned b = RBC_B(inst);
        out->blocks[pc] = sljit_emit_label(c);
        if(out->blocks[pc] == NULL) return false;
        emit_kill(out, out->kill_mask[pc]);

        if(out->kind[pc] == TINYJIT_SITE_VIRTUAL) {
            const JitEaSite* site = &out->ea->sites[pc];
            if(site->action == JIT_EA_BUILD) {
                emit_virtual_build(out, site->virtual_index, pc);
            } else {
                emit_virtual_use(out, site, inst, pc);
            }
            if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
            continue;
        }

        if(out->kind[pc] == TINYJIT_SITE_EXIT) {
            /* Unsupported, or the shared profile says native code would not pay off. */
            emit_side_exit(out, pc);
            if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
            continue;
        }
        /* Every island site belongs to an island; reaching one here is a bug. */
        if(out->kind[pc] == TINYJIT_SITE_ISLAND) return false;
        if(out->kind[pc] == TINYJIT_SITE_HELPER) {
            emit_helper_call(out, pc);
            if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
            continue;
        }
        switch(op) {
            case RBC_NOP: case RBC_SUPER_DATA: break;
            case RBC_MOVE: emit_copy_value(c, a, b); break;
            case RBC_CHECK_BOUND: emit_check_bound(out, a, pc); break;
            case RBC_LOAD_CONST: {
                const uint16_t index = RBC_BX(inst);
                const py_TValue* value = c11__at(py_TValue, &source->consts, index);
                emit_template(c, a, value);
                break;
            }
            case RBC_LOAD_NONE: emit_template(c, a, py_None()); break;
            case RBC_LOAD_NIL: emit_template(c, a, py_NIL()); break;
            case RBC_LOAD_I16: {
                py_TValue value = {0};
                py_newint(&value, (int16_t)RBC_BX(inst));
                emit_template(c, a, &value);
                break;
            }
            case RBC_LOAD_BOOL: {
                py_TValue value = {0};
                py_newbool(&value, b != 0);
                emit_template(c, a, &value);
                break;
            }
            case RBC_JUMP: emit_branch(out, pc, pc + RBC_SBX(inst)); break;
            case RBC_JUMP_IF_TRUE: case RBC_JUMP_IF_TRUE_OR_POP:
            case RBC_JUMP_IF_FALSE: case RBC_JUMP_IF_FALSE_OR_POP:
                emit_truthy_input(out, a, pc);
                emit_conditionally_branch(out,
                                           (op == RBC_JUMP_IF_TRUE || op == RBC_JUMP_IF_TRUE_OR_POP)
                                               ? SLJIT_NOT_EQUAL : SLJIT_EQUAL,
                                           SLJIT_R0, 0, SLJIT_IMM, 0,
                                           pc, pc + RBC_SBX(inst));
                break;
            case RBC_CMP_BRANCH: case RBC_CMP_I8_BRANCH: {
                if(pc + 1 >= code->code.length) return false;
                const RegBytecode ext = c11__getitem(RegBytecode, &code->code, pc + 1);
                emit_cmp_branch(out, op, inst, ext, pc);
                break;
            }
            case RBC_FOR_ITER: case RBC_FOR_RANGE:
                emit_range_iteration(out, inst, pc);
                break;
            case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
                emit_load_global(out, source, inst, pc);
                emit_slow_path(out, pc);
                break;
            case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
                /* Only a closure-free function makes this a plain global lookup. */
                ic_miss(out, sljit_emit_cmp(c, SLJIT_NOT_EQUAL, SLJIT_MEM1(SLJIT_S1),
                                            RUN_FIELD(has_closure), SLJIT_IMM, 0), pc);
                emit_load_global(out, source, inst, pc);
                emit_slow_path(out, pc);
                break;
            case RBC_LOAD_ATTR: case RBC_LOAD_ATTR_IC:
                emit_load_attr(out, inst, pc);
                emit_slow_path(out, pc);
                break;
            case RBC_STORE_ATTR: case RBC_STORE_ATTR_IC:
                emit_store_attr(out, inst, pc);
                emit_slow_path(out, pc);
                break;
            case RBC_LOAD_METHOD: case RBC_LOAD_METHOD_IC:
                emit_load_method(out, inst, pc);
                emit_slow_path(out, pc);
                break;
            case RBC_RETURN: sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, (sljit_sw)a); break;
            case RBC_RETURN_NONE:
                sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, TINYJIT_RETURN_NONE); break;
            default:
                /* No native implementation: let the interpreter execute it. */
                emit_side_exit(out, pc);
                break;
        }
        if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
    }
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM,
                      TINYJIT_RESUME_BASE + code->code.length - 1);
    for(int pc = 0; pc < code->code.length; pc++) {
        out->resume[pc] = sljit_emit_label(c);
        if(out->resume[pc] == NULL) return false;
        if(out->materialize[pc]) {
            /* Inside some object's lifetime: the interpreter needs it for real. */
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S1, 0);
            sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)pc);
            sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2(W, P, W), SLJIT_IMM,
                             SLJIT_FUNC_ADDR(tinyjit_materialize));
        }
        sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, TINYJIT_RESUME_BASE + pc);
    }
    for(int pc = 0; pc < code->code.length; pc++) {
        if(!site_calls_helper(code, out->kind, pc)) continue;
        out->error[pc] = sljit_emit_label(c);
        if(out->error[pc] == NULL) return false;
        sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, TINYJIT_ERROR_BASE + pc);
    }
    for(size_t i = 0; i < out->fix_count; i++) {
        TinyJitFixup* fixup = &out->fixes[i];
        struct sljit_label* label;
        switch(fixup->target) {
            case TINYJIT_TARGET_RESUME: label = out->resume[fixup->pc]; break;
            case TINYJIT_TARGET_ERROR: label = out->error[fixup->pc]; break;
            case TINYJIT_TARGET_SLOW: label = out->slow[fixup->pc]; break;
            default: label = out->blocks[fixup->pc]; break;
        }
        if(label == NULL) return false;
        sljit_set_label(fixup->jump, label);
    }
    return sljit_get_compiler_error(c) == SLJIT_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Function compilation                                                      */
/* ------------------------------------------------------------------------ */

static void jit_trace_dump(const FuncDecl* decl, const RegCode* code, const uint8_t* kind,
                           const JitEaResult* ea) {
    if(!jit_trace_enabled()) return;
    jit_trace(decl, "nregs=%d length=%d", code->nregs, code->code.length);
    for(int pc = 0; pc < code->code.length; pc++) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(word);
        const JitEaSite* site = ea->sites != NULL ? &ea->sites[pc] : NULL;
        const char* how = site != NULL && site->action == JIT_EA_BUILD ? "  <virtual: allocation removed>"
                          : kind[pc] == TINYJIT_SITE_VIRTUAL ? "  <virtual: scalar use>"
                          : kind[pc] == TINYJIT_SITE_ISLAND ? "  <island>"
                          : kind[pc] == TINYJIT_SITE_EXIT  ? "  <side exit>"
                          : kind[pc] == TINYJIT_SITE_HELPER ? "  <helper call>"
                          : is_cached_op(op)               ? "  <shared IC + helper slow path>"
                                                           : "";
        fprintf(stderr, "    %4d  %-20s A=%-3u B=%-3u C=%-3u sBx=%-6d%s\n", pc,
                RegOpcode__name(op), RBC_A(word), RBC_B(word), RBC_C(word),
                RBC_SBX(word), how);
    }
}

static TinyJitCode* compile_function(const FuncDecl* decl, const RegCode* code) {
    if(code->nregs == 0 || code->nregs > TINYJIT_MAX_REGS ||
       code->code.length < 2 || code->code.length > TINYJIT_MAX_CODE ||
       code->source_ip.length != code->code.length ||
       code->runtime_sites.length != code->code.length) {
        jit_trace(decl, "rejected: shape (nregs=%d, length=%d)", code->nregs, code->code.length);
        return NULL;
    }
    uint8_t* kind = calloc((size_t)code->code.length, sizeof(uint8_t));
    if(kind == NULL) return NULL;
    bool has_native_work = false;
    uint32_t side_exit_sites = 0;
    uint32_t shared_ic_sites = 0;
    uint32_t helper_call_sites = 0;
    for(int pc = 0; pc < code->code.length; pc++) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(word);
        kind[pc] = (uint8_t)classify_site(code, pc, op);
        if(kind[pc] == TINYJIT_SITE_EXIT) {
            side_exit_sites++;
        } else if(is_native_work(op) || is_cached_op(op) || is_helper_op(op)) {
            /* A helper call keeps the rest of the function native. */
            has_native_work = true;
            if(kind[pc] == TINYJIT_SITE_HELPER) {
                helper_call_sites++;
            } else if(is_cached_op(op)) {
                shared_ic_sites++;
            }
        }
        const char* problem = NULL;
        if(is_range_iteration(op) && RBC_A(word) + 1u >= code->nregs) {
            problem = "FOR_ITER result register out of range";
        } else if((op == RBC_LOAD_METHOD || op == RBC_LOAD_METHOD_IC) &&
                  (RBC_A(word) + 1u >= code->nregs || RBC_BX(word) >= decl->code.names.length)) {
            problem = "LOAD_METHOD operand out of range";
        } else if((op == RBC_CALL || op == RBC_CALL_IC) &&
                  RBC_A(word) + 2u + RBC_B(word) + 2u * RBC_C(word) > (unsigned)code->nregs) {
            problem = "CALL window out of range";
        } else if((op == RBC_STORE_ATTR || op == RBC_STORE_ATTR_IC) &&
                  RBC_A(word) + 1u >= code->nregs) {
            problem = "STORE_ATTR object register out of range";
        } else if((op == RBC_LOAD_GLOBAL || op == RBC_LOAD_GLOBAL_IC ||
                   op == RBC_LOAD_NONLOCAL || op == RBC_LOAD_NONLOCAL_IC) &&
                  RBC_BX(word) >= decl->code.names.length) {
            problem = "name index out of range";
        } else if(op == RBC_LOAD_CONST && RBC_BX(word) >= decl->code.consts.length) {
            problem = "constant index out of range";
        } else if(op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH) {
            if(pc + 1 >= code->code.length ||
               RBC_OP(c11__getitem(RegBytecode, &code->code, pc + 1)) != RBC_SUPER_DATA) {
                problem = "malformed superinstruction";
            } else {
                kind[++pc] = TINYJIT_SITE_NATIVE; /* payload word */
            }
        }
        if(problem != NULL) {
            jit_trace(decl, "rejected: %s at pc=%d", problem, pc);
            free(kind);
            return NULL;
        }
    }
    /* Escape analysis: allocations whose objects never escape are removed. */
    JitEaResult ea;
    JitEa__analyze(code, &decl->code, &ea);
    uint32_t virtual_sites = 0;
    for(int pc = 0; ea.sites != NULL && pc < code->code.length; pc++) {
        if(ea.sites[pc].action == JIT_EA_NONE) continue;
        if(kind[pc] == TINYJIT_SITE_EXIT) side_exit_sites--;
        if(kind[pc] == TINYJIT_SITE_HELPER) helper_call_sites--;
        kind[pc] = TINYJIT_SITE_VIRTUAL;
        if(ea.sites[pc].action == JIT_EA_BUILD) {
            virtual_sites++;
            has_native_work = true;
        }
    }
    jit_trace_dump(decl, code, kind, &ea);
    for(int i = 0; i < ea.virtual_count; i++) {
        static const char* const names[] = {"tuple", "list", "object"};
        const JitEaVirtual* v = &ea.virtuals[i];
        jit_trace(decl, "virtual %d: %s at pc=%d, %d components, last use pc=%d, holding r%d, "
                  "%d deopt points", i, names[v->kind], v->build_pc, v->component_count,
                  v->last_use_pc, v->holding_reg, v->points.length);
    }
    if(!has_native_work) {
        jit_trace(decl, "rejected: no native work");
        JitEa__dtor(&ea);
        free(kind);
        return NULL;
    }

    TinyJitIsland* islands = NULL;
    int island_count = 0;
    uint32_t max_stack_bytes = 0;
    uint32_t colored_vregs = 0;
    uint32_t spilled_vregs = 0;
    uint32_t lir_instructions = 0;
    TinyJitCode* result = NULL;
    TinyJitEmitter out = {0};
    bool* entry = calloc((size_t)code->code.length, sizeof(bool));
    if(entry == NULL || !native_entry_points(code, entry)) {
        jit_trace(decl, "rejected: invalid control flow");
        goto failure;
    }
    if(!prepare_lir_islands(code, entry, kind, &islands, &island_count, &max_stack_bytes,
                            &colored_vregs, &spilled_vregs, &lir_instructions)) {
        jit_trace(decl, "rejected: LIR island preparation failed");
        islands = NULL;
        island_count = 0;
        goto failure;
    }
    if(max_stack_bytes > SLJIT_MAX_LOCAL_SIZE) {
        jit_trace(decl, "rejected: native frame too large (%u bytes)", max_stack_bytes);
        goto failure;
    }

    /* Allocated first: native code addresses result->validated[] directly. */
    result = calloc(1, sizeof(*result));
    out.code = code;
    out.kind = kind;
    out.ea = &ea;
    out.target = result;
    out.code_length = code->code.length;
    out.fix_capacity = (size_t)out.code_length * 16u;
    out.blocks = calloc((size_t)out.code_length, sizeof(*out.blocks));
    out.resume = calloc((size_t)out.code_length, sizeof(*out.resume));
    out.error = calloc((size_t)out.code_length, sizeof(*out.error));
    out.slow = calloc((size_t)out.code_length, sizeof(*out.slow));
    out.kill_mask = calloc((size_t)out.code_length, sizeof(*out.kill_mask));
    out.materialize = calloc((size_t)out.code_length, sizeof(*out.materialize));
    out.fixes = calloc(out.fix_capacity, sizeof(*out.fixes));
    out.compiler = sljit_create_compiler(NULL);
    if(result == NULL || out.blocks == NULL || out.resume == NULL || out.error == NULL ||
       out.slow == NULL || out.kill_mask == NULL || out.materialize == NULL ||
       out.fixes == NULL || out.compiler == NULL) goto failure;
    for(int i = 0; i < ea.virtual_count; i++) {
        const JitEaVirtual* v = &ea.virtuals[i];
        c11__foreach(JitEaPoint, &v->points, point) out.materialize[point->pc] = 1;
        c11__foreach(int32_t, &v->kills, kill) out.kill_mask[*kill] |= virtual_bit(i);
        result->validated[i] = pk_current_vm->type_version; /* checked by the analysis */
    }

    /*
     * Integer arguments without the _R suffix are delivered by SLJIT directly in
     * saved registers: S0 = RegVM registers, S1 = TinyJitRun*, S2 = entry PC.
     */
    sljit_emit_enter(out.compiler, 0, SLJIT_ARGS3(W, P, P, W),
                     TINYJIT_LIR_GPRS, 3, (sljit_s32)max_stack_bytes);
    if(sljit_get_compiler_error(out.compiler) != SLJIT_SUCCESS) goto failure;
    /* Function entry uses PC 0; loop JIT requests enter at a hot loop header. */
    for(int pc = 0; pc < code->code.length; pc++) {
        if(!entry[pc]) continue;
        if(!remember(&out, sljit_emit_cmp(out.compiler, SLJIT_EQUAL,
                    SLJIT_S2, 0, SLJIT_IMM, pc), pc, false)) goto failure;
    }
    /* Never guess a PC: an unknown entry leaves the interpreter state untouched. */
    sljit_emit_return(out.compiler, SLJIT_MOV, SLJIT_IMM, TINYJIT_NOT_ENTERED);
    if(sljit_get_compiler_error(out.compiler) != SLJIT_SUCCESS) goto failure;
    if(!emit_instructions(&out, code, &decl->code, islands, island_count)) goto failure;

    void* executable = sljit_generate_code(out.compiler, 0, NULL);
    if(executable == NULL) goto failure;
    result->regcode = code;
    result->source = &decl->code;
    result->executable = executable;
    result->executable_bytes = (size_t)sljit_get_generated_code_size(out.compiler);
    result->valid_entry_pc = malloc((size_t)code->code.length);
    result->futile = calloc((size_t)code->code.length, sizeof(uint8_t));
    if(result->valid_entry_pc == NULL || result->futile == NULL) {
        sljit_free_code(executable, NULL);
        free(result->valid_entry_pc);
        free(result->futile);
        result->valid_entry_pc = NULL;
        result->futile = NULL;
        goto failure;
    }
    result->ea = ea; /* ownership moves to the compiled code */
    ea.sites = NULL;
    ea.virtual_count = 0;
    /*
     * Islands are split at block starts, so no island interior is ever an
     * entry. Entering where the first instruction is a side exit would only
     * bounce straight back to the interpreter.
     */
    for(int pc = 0; pc < code->code.length; pc++) {
        result->valid_entry_pc[pc] = entry[pc] && kind[pc] != TINYJIT_SITE_EXIT ? 1 : 0;
    }
    result->lir_islands = (uint32_t)island_count;
    result->lir_instructions = lir_instructions;
    result->colored_vregs = colored_vregs;
    result->spilled_vregs = spilled_vregs;
    result->side_exit_sites = side_exit_sites;
    result->shared_ic_sites = shared_ic_sites;
    result->helper_call_sites = helper_call_sites;
    result->virtual_sites = virtual_sites;
    _Static_assert(sizeof(TinyJitEntry) == sizeof(void*), "Incompatible native code pointer");
    memcpy(&result->entry, &executable, sizeof(result->entry));
    sljit_free_compiler(out.compiler);
    free(entry);
    free(kind);
    free(out.blocks);
    free(out.resume);
    free(out.error);
    free(out.slow);
    free(out.kill_mask);
    free(out.materialize);
    free(out.fixes);
    free_lir_islands(islands, island_count);
    jit_trace(decl, "compiled: %zu bytes, %u LIR islands, %u shared-IC sites, "
              "%u helper calls, %u virtual allocations, %u side-exit sites",
              result->executable_bytes, result->lir_islands, result->shared_ic_sites,
              result->helper_call_sites, result->virtual_sites, result->side_exit_sites);
    return result;

failure:
    jit_trace(decl, "rejected: native emission failed (sljit error %d)",
              out.compiler != NULL ? (int)sljit_get_compiler_error(out.compiler) : -1);
    if(out.compiler != NULL) sljit_free_compiler(out.compiler);
    JitEa__dtor(&ea);
    free(result);
    free(entry);
    free(kind);
    free(out.blocks);
    free(out.resume);
    free(out.error);
    free(out.slow);
    free(out.kill_mask);
    free(out.materialize);
    free(out.fixes);
    free_lir_islands(islands, island_count);
    return NULL;
}

/*
 * Every listed TinyJitCode belongs to a live RegCode: RegJit__release_code()
 * removes entries before their RegCode is destroyed (native_release).
 */
static void release_code(TinyJitContext* context, TinyJitCode* code) {
    if(context != NULL && context->last == code) context->last = NULL;
    RegCode* regcode = (RegCode*)code->regcode;
    if(regcode != NULL && regcode->jit_code == code) regcode->jit_code = NULL;
    if(code->executable != NULL) sljit_free_code(code->executable, NULL);
    free(code->valid_entry_pc);
    free(code->futile);
    /*
     * Last: dropping the references to analyzed __init__ decls can free them,
     * which re-enters native_release() for their own code. This code is
     * already unlinked by then.
     */
    JitEa__dtor(&code->ea);
    free(code);
}

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t length) {
    const unsigned char* bytes = data;
    for(size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t code_signature(const FuncDecl* decl, const RegCode* code) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = hash_bytes(hash, &code->nregs, sizeof(code->nregs));
    hash = hash_bytes(hash, &code->code.length, sizeof(code->code.length));
    hash = hash_bytes(hash, code->code.data,
                      (size_t)code->code.length * sizeof(RegBytecode));
    hash = hash_bytes(hash, &decl->code.consts.length,
                      sizeof(decl->code.consts.length));
    hash = hash_bytes(hash, decl->code.consts.data,
                      (size_t)decl->code.consts.length * sizeof(py_TValue));
    return hash;
}

static bool native_compile(VM* vm, const RegJitCompileRequest* request, void* userdata) {
    (void)vm;
    TinyJitContext* context = userdata;
    if(context == NULL || request == NULL || request->decl == NULL || request->code == NULL)
        return false;
    jit_trace(request->decl, "compile request (%s, header_pc=%d)",
              request->kind == REG_JIT_COMPILE_LOOP ? "loop" : "function",
              (int)request->loop_header_pc);

    const uint64_t signature = code_signature(request->decl, request->code);
    /* The compiled function body also provides native loop-header entries. */
    for(TinyJitCode* existing = context->code; existing != NULL; existing = existing->next) {
        if(existing->regcode == request->code && existing->source == &request->decl->code &&
           existing->signature == signature)
            return true;
    }

    /* A recycled RegCode address must never retain stale executable code. */
    TinyJitCode** node = &context->code;
    while(*node != NULL) {
        if((*node)->regcode == request->code) {
            TinyJitCode* old = *node;
            /*
             * Still executing further up the C stack, so this RegCode is alive
             * and the signature differs only through quickening, which native
             * code handles anyway: keep it.
             */
            if(old->active != 0) return true;
            *node = old->next;
            context->generated_bytes -= old->executable_bytes;
            release_code(context, old);
            break;
        }
        node = &(*node)->next;
    }

    TinyJitCode* compiled = compile_function(request->decl, request->code);
    if(compiled == NULL) return false;
    compiled->signature = signature;
    compiled->next = context->code;
    context->code = compiled;
    request->code->jit_code = compiled;
    context->compiled++;
    context->lir_islands += compiled->lir_islands;
    context->lir_instructions += compiled->lir_instructions;
    context->colored_vregs += compiled->colored_vregs;
    context->spilled_vregs += compiled->spilled_vregs;
    context->side_exit_sites += compiled->side_exit_sites;
    context->shared_ic_sites += compiled->shared_ic_sites;
    context->helper_call_sites += compiled->helper_call_sites;
    context->virtual_sites += compiled->virtual_sites;
    context->generated_bytes += compiled->executable_bytes;
    /* Bound executable memory even when Python functions are discarded. */
    while(context->generated_bytes > TINYJIT_CACHE_LIMIT_BYTES) {
        /* Evict the oldest inactive code; never the one just compiled. */
        TinyJitCode** victim = NULL;
        for(TinyJitCode** it = &context->code->next; *it != NULL; it = &(*it)->next) {
            if((*it)->active == 0) victim = it;
        }
        if(victim == NULL) break;
        TinyJitCode* oldest = *victim;
        *victim = oldest->next;
        context->generated_bytes -= oldest->executable_bytes;
        release_code(context, oldest);
    }
    return true;
}

static TinyJitCode* find_code(TinyJitContext* context, const RegCode* code,
                              const CodeObject* source) {
    (void)context;
    TinyJitCode* compiled = code->jit_code;
    return compiled != NULL && compiled->regcode == code && compiled->source == source
               ? compiled
               : NULL;
}

/*
 * Frame introspection (locals(), eval/exec with the frame's namespace): every
 * native activation running `frame` stores the objects pending at the PC of
 * its helper call in progress into their registers.
 */
static void native_force(VM* vm, py_Frame* frame, void* userdata) {
    (void)userdata;
    for(py_JitRoots* roots = vm->jit_roots; roots != NULL; roots = roots->prev) {
        if(roots->frame != frame || roots->owner == NULL) continue;
        TinyJitRun* run = roots->owner;
        if(run->pending != 0) materialize_at(run, (int32_t)run->pc);
    }
}

static void native_release(VM* vm, RegCode* code, void* userdata) {
    (void)vm;
    TinyJitContext* context = userdata;
    if(context == NULL) return;
    for(TinyJitCode** node = &context->code; *node != NULL; node = &(*node)->next) {
        if((*node)->regcode == code) {
            TinyJitCode* old = *node;
            *node = old->next;
            context->generated_bytes -= old->executable_bytes;
            context->released++;
            release_code(context, old);
            return;
        }
    }
}

/*
 * An entry that keeps returning without completing a backedge or a return
 * (e.g. a loop whose body always side-exits) costs more than it saves.
 */
static void note_entry_outcome(TinyJitContext* context, TinyJitCode* compiled,
                               int32_t start_pc, bool useful) {
    if(useful) {
        compiled->futile[start_pc] = 0;
        return;
    }
    if(++compiled->futile[start_pc] >= TINYJIT_FUTILE_LIMIT) {
        compiled->valid_entry_pc[start_pc] = 0;
        context->disabled_entries++;
    }
}

static bool native_enter_at(VM* vm, py_Frame* frame, RegCode* code,
                            int32_t start_pc, RegJitExit* exit, void* userdata) {
    TinyJitContext* context = userdata;
    if(context == NULL || frame == NULL || code == NULL || exit == NULL ||
       frame->locals == NULL || code->nregs == 0 || code->nregs > TINYJIT_MAX_REGS ||
       start_pc < 0 || start_pc >= code->code.length) return false;
    TinyJitCode* compiled = find_code(context, code, frame->co);
    if(compiled == NULL || compiled->valid_entry_pc[start_pc] == 0) {
        exit->disable = true; /* permanent: tell the interpreter to stop asking */
        return false;
    }

    /*
     * Native code works in place on the frame's registers, which are always
     * precise, so the GC may run inside helper calls without seeing a
     * half-updated frame.
     */
    TinyJitRun run;
    run.fuel = TINYJIT_LOOP_FUEL;
    run.vm = vm;
    run.frame = frame;
    run.code = code;
    run.helper_calls = 0;
    run.caches = (RegInlineCache*)code->inline_caches.data;
    run.type_version = vm->type_version;
    run.globals = frame->globals != NULL && frame->globals->type == tp_module
                      ? PyObject__dict(frame->globals->_obj)
                      : NULL;
    run.builtins = vm->builtins != NULL && vm->builtins->type == tp_module
                       ? PyObject__dict(vm->builtins->_obj)
                       : NULL;
    run.has_closure = frame->p0 != NULL && frame->p0->type == tp_function &&
                      ((Function*)py_touserdata(frame->p0))->closure != NULL;
    run.pending = 0;
    run.pc = start_pc;
    run.compiled = compiled;
    /* Components of scalar-replaced objects: GC roots for this activation only. */
    const int save_count = compiled->ea.save_count;
    py_TValue vsave[save_count > 0 ? save_count : 1];
    run.vsave = vsave;
    if(save_count > 0) {
        memset(vsave, 0, sizeof(py_TValue) * (size_t)save_count); /* nil */
        run.roots.values = vsave;
        run.roots.count = save_count;
        run.roots.frame = frame;
        run.roots.owner = &run;
        run.roots.prev = vm->jit_roots;
        vm->jit_roots = &run.roots;
    }
    context->entered++;
    compiled->active++;
    const sljit_sw result = compiled->entry(frame->locals, &run, start_pc);
    compiled->active--;
    if(save_count > 0) vm->jit_roots = run.roots.prev;
    /* Native work worth re-entering for: a loop iteration or a helper call. */
    const bool did_backedge = run.fuel != TINYJIT_LOOP_FUEL || run.helper_calls != 0;
    if(result >= TINYJIT_ERROR_BASE && result - TINYJIT_ERROR_BASE < code->code.length) {
        /* A helper raised: unwind exactly like the interpreter's REG_FAIL. */
        const int pc = (int)(result - TINYJIT_ERROR_BASE);
        frame->ip = c11__getitem(int32_t, &code->source_ip, pc);
        exit->kind = REG_JIT_EXIT_ERROR;
        context->helper_errors++;
        note_entry_outcome(context, compiled, start_pc, true);
        return true;
    }
    if(result >= 0 && result < code->nregs) {
        vm->last_retval = frame->locals[result];
        exit->kind = REG_JIT_EXIT_RETURN;
        context->completed++;
        note_entry_outcome(context, compiled, start_pc, true);
        return true;
    }
    if(result == TINYJIT_RETURN_NONE) {
        py_newnone(&vm->last_retval);
        exit->kind = REG_JIT_EXIT_RETURN;
        context->completed++;
        note_entry_outcome(context, compiled, start_pc, true);
        return true;
    }
    if(result >= TINYJIT_RESUME_BASE &&
       result - TINYJIT_RESUME_BASE < code->code.length) {
        const int pc = (int)(result - TINYJIT_RESUME_BASE);
        frame->ip = c11__getitem(int32_t, &code->source_ip, pc);
        exit->kind = REG_JIT_EXIT_RESUME;
        exit->resume_pc = pc;
        context->resumed++;
        note_entry_outcome(context, compiled, start_pc, did_backedge);
        return true;
    }
    return false;
}

static bool native_enter_function(VM* vm, py_Frame* frame, RegCode* code,
                                  RegJitExit* exit, void* userdata) {
    return native_enter_at(vm, frame, code, 0, exit, userdata);
}

static bool native_enter_loop(VM* vm, py_Frame* frame, RegCode* code,
                              int32_t header_pc, RegJitExit* exit, void* userdata) {
    return native_enter_at(vm, frame, code, header_pc, exit, userdata);
}

static const RegJitHooks kTinyJitHooks = {
    .compile = native_compile,
    .enter_function = native_enter_function,
    .enter_loop = native_enter_loop,
    .release = native_release,
    .force = native_force,
};

void TinyJit__attach(VM* vm) {
    if(vm == NULL || vm->regjit_hooks != NULL) return;
    TinyJitContext* context = calloc(1, sizeof(*context));
    if(context != NULL) RegJit__set_hooks(vm, &kTinyJitHooks, context);
}

void TinyJit__detach(VM* vm) {
    if(vm == NULL || vm->regjit_hooks != &kTinyJitHooks) return;
    TinyJitContext* context = vm->regjit_userdata;
    RegJit__set_hooks(vm, NULL, NULL);
    if(context == NULL) return;

    const char* stats = getenv("TINYPY_JIT_STATS");
    if(stats != NULL && stats[0] != '\0' && strcmp(stats, "0") != 0) {
        fprintf(stderr,
                "[TinyPy SLJIT] compiled=%llu entered=%llu native_returns=%llu "
                "resumes=%llu disabled_entries=%llu executable_bytes=%zu "
                "lir_islands=%llu lir_instructions=%llu "
                "colored_vregs=%llu spills=%llu shared_ic_sites=%llu helper_call_sites=%llu "
                "helper_errors=%llu side_exit_sites=%llu released=%llu "
                "virtual_allocations=%llu materialized=%llu\n",
                (unsigned long long)context->compiled,
                (unsigned long long)context->entered,
                (unsigned long long)context->completed,
                (unsigned long long)context->resumed,
                (unsigned long long)context->disabled_entries,
                context->generated_bytes,
                (unsigned long long)context->lir_islands,
                (unsigned long long)context->lir_instructions,
                (unsigned long long)context->colored_vregs,
                (unsigned long long)context->spilled_vregs,
                (unsigned long long)context->shared_ic_sites,
                (unsigned long long)context->helper_call_sites,
                (unsigned long long)context->helper_errors,
                (unsigned long long)context->side_exit_sites,
                (unsigned long long)context->released,
                (unsigned long long)context->virtual_sites,
                (unsigned long long)context->materialized);
    }
    /*
     * Two passes: freeing one code can drop the last reference to a FuncDecl
     * whose RegCode owns another listed code, so every RegCode link is cut
     * before anything is freed.
     */
    for(TinyJitCode* code = context->code; code != NULL; code = code->next) {
        RegCode* regcode = (RegCode*)code->regcode;
        if(regcode != NULL && regcode->jit_code == code) regcode->jit_code = NULL;
        code->regcode = NULL;
    }
    TinyJitCode* code = context->code;
    context->code = NULL;
    while(code != NULL) {
        TinyJitCode* next = code->next;
        release_code(NULL, code);
        code = next;
    }
    free(context);
}
