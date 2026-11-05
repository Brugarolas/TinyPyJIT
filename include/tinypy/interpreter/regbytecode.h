#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "tinypy/common/vector.h"
#include "tinypy/objects/codeobject.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 32-bit register bytecode.
 *
 * ABC:   [ C:8 | B:8 | A:8 | OP:8 ]
 * ABx:   [       Bx:16 | A:8 | OP:8 ]
 * AsBx:  [      sBx:16 | A:8 | OP:8 ]
 * Ax:    [             Ax:24 | OP:8 ]
 *
 * Registers 0..co->nlocals-1 are function locals. Remaining registers are
 * canonical operand-stack slots and block-local temporaries produced by the
 * stack-to-register lowering pass.
 */
typedef uint32_t RegBytecode;

typedef enum RegOpcode {
    RBC_NOP = 0,
    RBC_MOVE,
    RBC_CHECK_BOUND,

    RBC_LOAD_CONST,
    RBC_LOAD_NONE,
    RBC_LOAD_NIL,
    RBC_LOAD_ELLIPSIS,
    RBC_LOAD_BOOL,
    RBC_LOAD_I16,
    RBC_LOAD_NAME_INT,

    RBC_LOAD_GLOBAL,
    RBC_LOAD_NONLOCAL,
    RBC_LOAD_FUNCTION,
    RBC_LOAD_ATTR,
    RBC_LOAD_METHOD,
    RBC_LOAD_SUBSCR,

    RBC_STORE_GLOBAL,
    RBC_STORE_ATTR,
    RBC_STORE_SUBSCR,
    RBC_DELETE_FAST,
    RBC_DELETE_GLOBAL,
    RBC_DELETE_ATTR,
    RBC_DELETE_SUBSCR,

    RBC_BUILD_IMAG,
    RBC_BUILD_BYTES,
    RBC_BUILD_TUPLE,
    RBC_BUILD_LIST,
    RBC_BUILD_DICT,
    RBC_BUILD_SLICE,
    RBC_BUILD_STRING,

    RBC_ADD,
    RBC_SUB,
    RBC_MUL,
    RBC_TRUEDIV,
    RBC_FLOORDIV,
    RBC_MOD,
    RBC_POW,
    RBC_LSHIFT,
    RBC_RSHIFT,
    RBC_BITAND,
    RBC_BITOR,
    RBC_BITXOR,
    RBC_MATMUL,

    RBC_CMP_LT,
    RBC_CMP_LE,
    RBC_CMP_EQ,
    RBC_CMP_NE,
    RBC_CMP_GT,
    RBC_CMP_GE,
    RBC_IS,
    RBC_IS_NOT,
    RBC_CONTAINS,
    RBC_NOT_CONTAINS,

    RBC_NEG,
    RBC_NOT,
    RBC_INVERT,

    RBC_GET_ITER,
    RBC_FOR_ITER,

    RBC_JUMP,
    RBC_JUMP_IF_FALSE,
    RBC_JUMP_IF_TRUE,
    RBC_JUMP_IF_TRUE_OR_POP,
    RBC_JUMP_IF_FALSE_OR_POP,
    RBC_SHORTCUT_IF_FALSE_OR_POP,
    RBC_JUMP_IF_NOT_MATCH,

    RBC_CALL,

    RBC_LIST_APPEND,
    RBC_DICT_ADD,

    RBC_UNPACK_SEQUENCE,
    RBC_UNPACK_EX,
    RBC_FORMAT_STRING,

    RBC_RETURN,
    RBC_RETURN_NONE,

    /* Adaptive arithmetic specializations. Operand layout matches the generic op. */
    RBC_ADD_INT,
    RBC_SUB_INT,
    RBC_MUL_INT,
    RBC_ADD_NUM,
    RBC_SUB_NUM,
    RBC_MUL_NUM,

    RBC_CMP_LT_INT,
    RBC_CMP_LE_INT,
    RBC_CMP_EQ_INT,
    RBC_CMP_NE_INT,
    RBC_CMP_GT_INT,
    RBC_CMP_GE_INT,

    RBC_CMP_LT_NUM,
    RBC_CMP_LE_NUM,
    RBC_CMP_EQ_NUM,
    RBC_CMP_NE_NUM,
    RBC_CMP_GT_NUM,
    RBC_CMP_GE_NUM,

    /* Inline-cache specializations. */
    RBC_LOAD_GLOBAL_IC,
    RBC_LOAD_ATTR_IC,
    RBC_LOAD_METHOD_IC,
    RBC_CALL_IC,

    /* Iterator specializations. */
    RBC_FOR_GENERATOR,
    RBC_FOR_ARRAY2D,
    RBC_FOR_LIST,
    RBC_FOR_TUPLE,
    RBC_FOR_DICT,
    RBC_FOR_RANGE,
    RBC_FOR_STR,

    /* Static superinstructions emitted after lowering. */
    RBC_ADD_I8,
    RBC_SUB_I8,
    RBC_MUL_I8,
    RBC_CMP_LT_I8,
    RBC_CMP_LE_I8,
    RBC_CMP_EQ_I8,
    RBC_CMP_NE_I8,
    RBC_CMP_GT_I8,
    RBC_CMP_GE_I8,
    RBC_CMP_BRANCH,
    RBC_CMP_I8_BRANCH,
    RBC_SUPER_DATA,

    RBC__COUNT,
} RegOpcode;

#if defined(__cplusplus)
static_assert(RBC__COUNT <= 256, "register opcode must fit in 8 bits");
static_assert(sizeof(RegBytecode) == 4, "register bytecode word must stay 32-bit");
#else
_Static_assert(RBC__COUNT <= 256, "register opcode must fit in 8 bits");
_Static_assert(sizeof(RegBytecode) == 4, "register bytecode word must stay 32-bit");
#endif

#define RBC_OP(i) ((RegOpcode)((i) & 0xFFu))
#define RBC_A(i) ((uint8_t)(((i) >> 8) & 0xFFu))
#define RBC_B(i) ((uint8_t)(((i) >> 16) & 0xFFu))
#define RBC_C(i) ((uint8_t)(((i) >> 24) & 0xFFu))
#define RBC_I8(i) ((int8_t)RBC_C(i))
#define RBC_BX(i) ((uint16_t)(((i) >> 16) & 0xFFFFu))
#define RBC_SBX(i) ((int16_t)RBC_BX(i))
#define RBC_AX(i) ((uint32_t)((i) >> 8))

#define RBC_ABC(op, a, b, c)                                                                      \
    ((RegBytecode)((uint32_t)(op) | ((uint32_t)(uint8_t)(a) << 8) |                               \
                   ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 24)))
#define RBC_ABX(op, a, bx)                                                                         \
    ((RegBytecode)((uint32_t)(op) | ((uint32_t)(uint8_t)(a) << 8) |                               \
                   ((uint32_t)(uint16_t)(bx) << 16)))
#define RBC_ASBX(op, a, sbx) RBC_ABX((op), (a), (uint16_t)(int16_t)(sbx))
#define RBC_AX_ENCODE(op, ax) ((RegBytecode)((uint32_t)(op) | ((uint32_t)(ax) << 8)))

#define REG_ADAPTIVE_WARMUP 3u
#define REG_ADAPTIVE_MISS_LIMIT 2u
#define REG_IC_NONE UINT16_MAX

typedef enum RegCompareCode {
    REG_CMP_LT = 0,
    REG_CMP_LE = 1,
    REG_CMP_EQ = 2,
    REG_CMP_NE = 3,
    REG_CMP_GT = 4,
    REG_CMP_GE = 5,
} RegCompareCode;

#define REG_CMP_BRANCH_TRUE 0x80u
#define REG_CMP_BRANCH_CODE_MASK 0x07u

typedef enum RegInlineCacheKind {
    REG_IC_KIND_NONE = 0,
    REG_IC_KIND_GLOBAL,
    REG_IC_KIND_ATTR,
    REG_IC_KIND_METHOD,
    REG_IC_KIND_CALL,
} RegInlineCacheKind;

typedef enum RegGlobalCacheVariant {
    REG_GLOBAL_CACHE_NONE = 0,
    REG_GLOBAL_CACHE_GLOBAL,
    REG_GLOBAL_CACHE_BUILTIN,
} RegGlobalCacheVariant;

typedef enum RegAttrCacheVariant {
    REG_ATTR_CACHE_NONE = 0,
    REG_ATTR_CACHE_INSTANCE_FIRST,
    REG_ATTR_CACHE_SHAPE_SLOT,
    REG_ATTR_CACHE_PROPERTY,
    REG_ATTR_CACHE_FUNCTION,
    REG_ATTR_CACHE_NATIVE_FUNCTION,
    REG_ATTR_CACHE_STATICMETHOD,
    REG_ATTR_CACHE_CLASSMETHOD,
    REG_ATTR_CACHE_CLASS_VALUE,
} RegAttrCacheVariant;

typedef enum RegMethodCacheVariant {
    REG_METHOD_CACHE_NONE = 0,
    REG_METHOD_CACHE_BIND_SELF,
    REG_METHOD_CACHE_STATIC,
    REG_METHOD_CACHE_CLASS,
} RegMethodCacheVariant;

typedef enum RegCallCacheVariant {
    REG_CALL_CACHE_NONE = 0,
    REG_CALL_CACHE_NATIVE,
    REG_CALL_CACHE_PY_SIMPLE,
    REG_CALL_CACHE_DECL_CFUNC_SIMPLE,
} RegCallCacheVariant;

/*
 * Compact per-PC adaptive state. Most instructions only pay for this structure;
 * larger inline-cache payloads are allocated only for sites that specialize.
 */
typedef struct RegRuntimeSite {
    uint32_t profile;
    uint16_t cache_index;
    uint16_t feedback_index;
    uint16_t loop_index;
    uint8_t streak;
    uint8_t misses;
} RegRuntimeSite;

/*
 * Side-table cache used by global/attribute/method/call specializations.
 * Pointers into NameDict storage are always guarded by an owning dictionary
 * version or VM::type_version before dereference.
 */
typedef struct RegInlineCache {
    uint8_t kind;
    uint8_t variant;
    uint8_t flags;
    uint8_t reserved0;
    py_Type type;
    uint16_t argc;
    uint64_t version0;
    uint64_t version1;
    uintptr_t guard0;
    uintptr_t guard1;
    uint32_t index0;
    uint32_t index1;
    py_TValue* slot;
} RegInlineCache;

typedef enum RegLowerStatus {
    REG_LOWER_OK = 0,
    REG_LOWER_UNSUPPORTED,
    REG_LOWER_INVALID_BYTECODE,
    REG_LOWER_STACK_MISMATCH,
    REG_LOWER_TOO_MANY_REGISTERS,
    REG_LOWER_JUMP_TOO_FAR,
    REG_LOWER_OOM,
} RegLowerStatus;

typedef struct RegLowerError {
    RegLowerStatus status;
    int32_t bytecode_ip;
    char message[192];
} RegLowerError;

typedef struct RegProfileSite RegProfileSite;
typedef struct RegLoopProfile RegLoopProfile;

typedef struct RegCode {
    c11_vector /*T=RegBytecode*/ code;
    c11_vector /*T=int32_t*/ source_ip;
    c11_vector /*T=RegRuntimeSite*/ runtime_sites;
    c11_vector /*T=RegInlineCache*/ inline_caches;
    c11_vector /*T=RegProfileSite*/ profile_sites;
    c11_vector /*T=RegLoopProfile*/ loop_profiles;
    uint16_t nregs;
    uint16_t max_stack;
    uint16_t temp_base;
    uint16_t max_call_window;
    uint32_t quickened_count;
    uint32_t superinstruction_count;
    uint32_t function_entries;
    uint16_t function_hotness;
    uint8_t function_jit_state;
    uint8_t reserved_profile;
} RegCode;

void RegCode__ctor(RegCode* self);
void RegCode__dtor(RegCode* self);
RegCode* RegCode__new(void);
void RegCode__delete(RegCode* self);

/*
 * Lower the existing stack bytecode into register bytecode. The pass is
 * conservative: unsupported cold language features reject the whole CodeObject
 * so execution can transparently fall back to the legacy interpreter.
 */
bool RegCode__lower(RegCode* out, const CodeObject* co, RegLowerError* error);

/* Lower a function with precise knowledge of arguments that are bound at entry. */
bool RegCode__lower_function(RegCode* out, const FuncDecl* decl, RegLowerError* error);

/* Validate register bounds, pool indices and control-flow targets. */
bool RegCode__verify(const RegCode* code, const CodeObject* co, RegLowerError* error);

/* Build compact runtime-site state and run static register-bytecode peepholes. */
bool RegCode__prepare_runtime(RegCode* code, const CodeObject* co, RegLowerError* error);

/* Drop all runtime specialization state while preserving lowered bytecode. */
void RegCode__reset_runtime(RegCode* code);

/* Drop execution/JIT feedback without changing quickened bytecode or ICs. */
void RegCode__reset_profile(RegCode* code);

const char* RegOpcode__name(RegOpcode op);
const char* RegLowerStatus__name(RegLowerStatus status);

#ifdef __cplusplus
}  // extern "C"
#endif
