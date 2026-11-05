#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/common/vector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t JitValueId;
typedef uint32_t JitInstId;
typedef uint32_t JitBlockId;
typedef uint32_t JitStateId;
typedef uint32_t JitLoopId;

#define JIT_INVALID_VALUE UINT32_MAX
#define JIT_INVALID_INST UINT32_MAX
#define JIT_INVALID_BLOCK UINT32_MAX
#define JIT_INVALID_STATE UINT32_MAX
#define JIT_INVALID_LOOP UINT32_MAX

typedef enum JitRegionKind {
    JIT_REGION_FUNCTION = 0,
    JIT_REGION_LOOP = 1,
} JitRegionKind;

typedef enum JitType {
    JIT_TYPE_VOID = 0,

    /* Logical TinyPy value. Eliminated or materialized before register allocation. */
    JIT_TYPE_VALUE,

    /* Native scalar representations. */
    JIT_TYPE_I64,
    JIT_TYPE_F64,
    JIT_TYPE_BOOL,

    /* Runtime metadata. */
    JIT_TYPE_PY_TYPE,
    JIT_TYPE_NAME,

    /* Native pointers. */
    JIT_TYPE_PTR,
    JIT_TYPE_OBJECT_PTR,
    JIT_TYPE_VALUE_PTR,
    JIT_TYPE_VM_PTR,
    JIT_TYPE_FRAME_PTR,

    /* Opaque multi-result value produced by mid-level runtime operations. */
    JIT_TYPE_TOKEN,
} JitType;

typedef enum JitKnownType {
    JIT_KNOWN_UNKNOWN = 0,
    JIT_KNOWN_NONE,
    JIT_KNOWN_NIL,
    JIT_KNOWN_ELLIPSIS,
    JIT_KNOWN_BOOL,
    JIT_KNOWN_INT,
    JIT_KNOWN_FLOAT,
    JIT_KNOWN_STR,
    JIT_KNOWN_BYTES,
    JIT_KNOWN_LIST,
    JIT_KNOWN_TUPLE,
    JIT_KNOWN_DICT,
    JIT_KNOWN_SET,
    JIT_KNOWN_FUNCTION,
    JIT_KNOWN_NATIVE_FUNCTION,
    JIT_KNOWN_OBJECT,
    JIT_KNOWN_SLICE,
    JIT_KNOWN_MODULE,
    JIT_KNOWN_TYPE,
    JIT_KNOWN_ITERATOR,
    JIT_KNOWN_STAR_WRAPPER,
} JitKnownType;

typedef struct JitTypeInfo {
    JitType representation;
    JitKnownType known_type;
    bool exact;
    bool nullable;
} JitTypeInfo;

typedef enum JitEffect {
    JIT_EFFECT_NONE = 0,
    JIT_EFFECT_READ_RUNTIME = 1u << 0,
    JIT_EFFECT_WRITE_RUNTIME = 1u << 1,
    JIT_EFFECT_READ_HEAP = 1u << 2,
    JIT_EFFECT_WRITE_HEAP = 1u << 3,
    JIT_EFFECT_ALLOCATE = 1u << 4,
    JIT_EFFECT_CALL = 1u << 5,
    JIT_EFFECT_RAISE = 1u << 6,
    JIT_EFFECT_BARRIER = 1u << 7,
} JitEffect;

typedef enum JitOpcode {
    JIT_OP_NOP = 0,
    JIT_OP_PHI,

    /* Region inputs. */
    JIT_OP_VM,
    JIT_OP_FRAME,
    JIT_OP_LOCAL,
    JIT_OP_STACK_VALUE,

    /* Constants. */
    JIT_OP_CONST_NONE,
    JIT_OP_CONST_NIL,
    JIT_OP_CONST_ELLIPSIS,
    JIT_OP_CONST_BOOL,
    JIT_OP_CONST_I64,
    JIT_OP_CONST_F64,
    JIT_OP_CONST_POOL,

    /* Locals and names. */
    JIT_OP_LOAD_LOCAL,
    JIT_OP_STORE_LOCAL,
    JIT_OP_LOAD_GLOBAL,
    JIT_OP_STORE_GLOBAL,
    JIT_OP_LOAD_NONLOCAL,

    /* Representation operations. */
    JIT_OP_BOX_I64,
    JIT_OP_BOX_F64,
    JIT_OP_BOX_BOOL,
    JIT_OP_UNBOX_I64,
    JIT_OP_UNBOX_F64,
    JIT_OP_UNBOX_BOOL,
    JIT_OP_IS_TYPE,
    JIT_OP_EXTRACT_TOKEN,
    JIT_OP_IS_IDENTICAL,
    JIT_OP_NOT_BOOL,

    /* Generic Python arithmetic. */
    JIT_OP_ADD_VALUE,
    JIT_OP_SUB_VALUE,
    JIT_OP_MUL_VALUE,
    JIT_OP_TRUEDIV_VALUE,
    JIT_OP_FLOORDIV_VALUE,
    JIT_OP_MOD_VALUE,
    JIT_OP_POW_VALUE,

    /* Integer arithmetic. */
    JIT_OP_ADD_I64,
    JIT_OP_SUB_I64,
    JIT_OP_MUL_I64,
    JIT_OP_DIV_I64,
    JIT_OP_MOD_I64,
    JIT_OP_AND_I64,
    JIT_OP_OR_I64,
    JIT_OP_XOR_I64,
    JIT_OP_SHL_I64,
    JIT_OP_SHR_I64,
    JIT_OP_NEG_I64,
    JIT_OP_NOT_I64,

    /* Floating-point arithmetic. */
    JIT_OP_ADD_F64,
    JIT_OP_SUB_F64,
    JIT_OP_MUL_F64,
    JIT_OP_DIV_F64,
    JIT_OP_NEG_F64,

    /* Generic comparison. */
    JIT_OP_EQ_VALUE,
    JIT_OP_NE_VALUE,
    JIT_OP_LT_VALUE,
    JIT_OP_LE_VALUE,
    JIT_OP_GT_VALUE,
    JIT_OP_GE_VALUE,

    /* Integer comparison. */
    JIT_OP_EQ_I64,
    JIT_OP_NE_I64,
    JIT_OP_LT_I64,
    JIT_OP_LE_I64,
    JIT_OP_GT_I64,
    JIT_OP_GE_I64,

    /* Floating-point comparison. */
    JIT_OP_EQ_F64,
    JIT_OP_NE_F64,
    JIT_OP_LT_F64,
    JIT_OP_LE_F64,
    JIT_OP_GT_F64,
    JIT_OP_GE_F64,

    /* Python truthiness. */
    JIT_OP_TRUTHY,

    /* Object operations. */
    JIT_OP_LOAD_ATTR,
    JIT_OP_STORE_ATTR,
    JIT_OP_LOAD_METHOD,
    JIT_OP_LOAD_SUBSCR,
    JIT_OP_STORE_SUBSCR,

    /* Iteration. */
    JIT_OP_GET_ITER,
    JIT_OP_ITER_NEXT,

    /* Calls. */
    JIT_OP_CALL,
    JIT_OP_CALL_NATIVE,
    JIT_OP_CALL_HELPER,

    /* Construction. */
    JIT_OP_BUILD_TUPLE,
    JIT_OP_BUILD_LIST,
    JIT_OP_BUILD_DICT,
    JIT_OP_BUILD_SET,

    /* Control flow. */
    JIT_OP_JUMP,
    JIT_OP_BRANCH,
    JIT_OP_SWITCH,

    /* VM synchronization. */
    JIT_OP_SYNC_STATE,
    JIT_OP_SAFEPOINT,
    JIT_OP_POLL,

    /* Region exits. */
    JIT_OP_RETURN,
    JIT_OP_YIELD,
    JIT_OP_EXIT_INTERPRETER,
    JIT_OP_ERROR,

    JIT_OP__COUNT,
} JitOpcode;

typedef struct JitSourceLocation {
    int32_t bytecode_ip;
    int32_t line;
} JitSourceLocation;

typedef struct JitOperandRange {
    uint32_t offset;
    uint32_t count;
} JitOperandRange;

typedef union JitImmediate {
    int64_t i64;
    double f64;
    uint64_t u64;
    uintptr_t ptr;
    uint32_t u32;
    int32_t i32;
} JitImmediate;

typedef struct JitInst {
    JitOpcode op;
    JitValueId result;
    JitTypeInfo type;
    JitOperandRange operands;
    JitImmediate imm;
    uint32_t aux;
    uint32_t effects;
    JitStateId resume_state;
    JitSourceLocation source;
    bool removed;
} JitInst;

typedef struct JitValue {
    JitTypeInfo type;
    JitInstId def;
    uint32_t use_count;
} JitValue;

typedef enum JitBlockFlags {
    JIT_BLOCK_NONE = 0,
    JIT_BLOCK_LOOP_HEADER = 1u << 0,
    JIT_BLOCK_LOOP_LATCH = 1u << 1,
    JIT_BLOCK_COLD = 1u << 2,
    JIT_BLOCK_ENTRY = 1u << 3,
} JitBlockFlags;

typedef struct JitBlock {
    JitBlockId id;
    c11_vector insts;         /* JitInstId */
    c11_vector predecessors;  /* JitBlockId */
    c11_vector successors;    /* JitBlockId */
    uint32_t frequency;
    uint32_t flags;
} JitBlock;

typedef struct JitState {
    int32_t bytecode_ip;
    JitOperandRange locals;
    JitOperandRange stack;
} JitState;

typedef struct JitLoop {
    JitBlockId header;
    JitBlockId latch;
    c11_vector blocks; /* JitBlockId */
    uint32_t depth;
} JitLoop;

typedef struct JitRegion {
    JitRegionKind kind;
    const void* code_object;
    int32_t entry_ip;
    int32_t loop_header_ip;
    JitBlockId entry_block;

    c11_vector blocks;       /* JitBlock */
    c11_vector insts;        /* JitInst */
    c11_vector values;       /* JitValue */
    c11_vector operands;     /* JitValueId */
    c11_vector states;       /* JitState */
    c11_vector state_values; /* JitValueId */
    c11_vector loops;        /* JitLoop */

    uint32_t estimated_execution_count;
    bool contains_calls;
    bool contains_allocations;
    bool contains_exception_handlers;
} JitRegion;

typedef struct JitBuilder {
    JitRegion* region;
    JitBlockId block;
    JitSourceLocation source;
} JitBuilder;

JitTypeInfo jit_type_value(void);
JitTypeInfo jit_type_exact(JitType representation, JitKnownType known_type);
JitTypeInfo jit_type_unknown(JitType representation);
bool jit_type_equal(JitTypeInfo a, JitTypeInfo b);

uint32_t jit_opcode_effects(JitOpcode op);
const char* jit_opcode_name(JitOpcode op);
bool jit_opcode_is_terminator(JitOpcode op);
bool jit_opcode_is_pure(JitOpcode op);

void jit_region_init(JitRegion* region,
                     JitRegionKind kind,
                     const void* code_object,
                     int32_t entry_ip,
                     int32_t loop_header_ip);
void jit_region_destroy(JitRegion* region);

JitBlockId jit_region_add_block(JitRegion* region, uint32_t frequency, uint32_t flags);
JitStateId jit_region_add_state(JitRegion* region,
                                int32_t bytecode_ip,
                                const JitValueId* locals,
                                uint32_t locals_count,
                                const JitValueId* stack,
                                uint32_t stack_count);
JitLoopId jit_region_add_loop(JitRegion* region,
                              JitBlockId header,
                              JitBlockId latch,
                              const JitBlockId* blocks,
                              uint32_t block_count,
                              uint32_t depth);

JitBlock* jit_region_block(JitRegion* region, JitBlockId id);
const JitBlock* jit_region_block_const(const JitRegion* region, JitBlockId id);
JitInst* jit_region_inst(JitRegion* region, JitInstId id);
const JitInst* jit_region_inst_const(const JitRegion* region, JitInstId id);
JitValue* jit_region_value(JitRegion* region, JitValueId id);
const JitValue* jit_region_value_const(const JitRegion* region, JitValueId id);
const JitValueId* jit_inst_operands(const JitRegion* region, const JitInst* inst);
const JitValueId* jit_state_locals(const JitRegion* region, const JitState* state);
const JitValueId* jit_state_stack(const JitRegion* region, const JitState* state);

void jit_builder_init(JitBuilder* builder, JitRegion* region, JitBlockId block);
void jit_builder_set_block(JitBuilder* builder, JitBlockId block);
void jit_builder_set_source(JitBuilder* builder, int32_t bytecode_ip, int32_t line);

JitInstId jit_builder_emit(JitBuilder* builder,
                           JitOpcode op,
                           JitTypeInfo result_type,
                           const JitValueId* operands,
                           uint32_t operand_count,
                           JitImmediate imm,
                           uint32_t aux);

JitValueId jit_builder_emit_value(JitBuilder* builder,
                                  JitOpcode op,
                                  JitTypeInfo result_type,
                                  const JitValueId* operands,
                                  uint32_t operand_count,
                                  JitImmediate imm,
                                  uint32_t aux);

JitValueId jit_builder_const_i64(JitBuilder* builder, int64_t value);
JitValueId jit_builder_const_f64(JitBuilder* builder, double value);
JitValueId jit_builder_const_bool(JitBuilder* builder, bool value);
JitValueId jit_builder_const_none(JitBuilder* builder);
JitValueId jit_builder_const_nil(JitBuilder* builder);
JitValueId jit_builder_const_ellipsis(JitBuilder* builder);

JitInstId jit_builder_jump(JitBuilder* builder, JitBlockId target);
JitInstId jit_builder_branch(JitBuilder* builder,
                             JitValueId condition,
                             JitBlockId if_true,
                             JitBlockId if_false);
JitInstId jit_builder_return(JitBuilder* builder, JitValueId value);
JitInstId jit_builder_exit_interpreter(JitBuilder* builder, JitStateId state);

void jit_region_rebuild_cfg(JitRegion* region);
void jit_region_recompute_uses(JitRegion* region);
bool jit_region_replace_all_uses(JitRegion* region, JitValueId from, JitValueId to);

bool jit_region_verify(const JitRegion* region, char* error, uint32_t error_size);
void jit_region_dump(const JitRegion* region, void (*write)(const char* text, void* ctx), void* ctx);

#ifdef __cplusplus
}
#endif
