#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pocketpy/common/vector.h"
#include "pocketpy/jit/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t JitVRegId;
typedef uint32_t JitStackSlotId;
typedef uint32_t JitLirBlockId;
typedef uint32_t JitLirStateId;

#define JIT_INVALID_VREG UINT32_MAX
#define JIT_INVALID_STACK_SLOT UINT32_MAX
#define JIT_INVALID_LIR_BLOCK UINT32_MAX
#define JIT_INVALID_LIR_STATE UINT32_MAX

typedef enum JitVRegClass {
    JIT_VREG_NONE = 0,
    JIT_VREG_GPR,
    JIT_VREG_FPR,
    /* Full py_TValue values are intentionally memory-resident. */
    JIT_VREG_VALUE,
} JitVRegClass;

typedef enum JitPhysRegBank {
    JIT_PHYS_NONE = 0,
    JIT_PHYS_GPR_SCRATCH,
    JIT_PHYS_GPR_SAVED,
    JIT_PHYS_FPR_SCRATCH,
    JIT_PHYS_FPR_SAVED,
} JitPhysRegBank;

typedef struct JitPhysReg {
    JitPhysRegBank bank;
    uint16_t index;
} JitPhysReg;

typedef struct JitVReg {
    JitVRegClass reg_class;
    JitTypeInfo type;
    JitValueId mir_value;
    double spill_weight;
    bool force_stack;
} JitVReg;

typedef enum JitStackSlotKind {
    JIT_STACK_SLOT_VALUE_HOME = 0,
    JIT_STACK_SLOT_SPILL,
    JIT_STACK_SLOT_TEMPORARY,
} JitStackSlotKind;

typedef struct JitStackSlot {
    uint32_t size;
    uint32_t alignment;
    uint32_t offset;
    JitStackSlotKind kind;
} JitStackSlot;

typedef enum JitLirOperandKind {
    JIT_LIR_OPERAND_NONE = 0,
    JIT_LIR_OPERAND_VREG,
    JIT_LIR_OPERAND_PREG,
    JIT_LIR_OPERAND_STACK_SLOT,
    JIT_LIR_OPERAND_IMM_I64,
    JIT_LIR_OPERAND_IMM_F64,
    JIT_LIR_OPERAND_PTR,
    JIT_LIR_OPERAND_BLOCK,
} JitLirOperandKind;

typedef struct JitLirOperand {
    JitLirOperandKind kind;
    JitTypeInfo type;
    union {
        JitVRegId vreg;
        JitPhysReg preg;
        JitStackSlotId stack_slot;
        int64_t i64;
        double f64;
        uintptr_t ptr;
        JitLirBlockId block;
    } as;
} JitLirOperand;

typedef struct JitLirOperandRange {
    uint32_t offset;
    uint32_t count;
} JitLirOperandRange;

typedef enum JitLirCondition {
    JIT_LIR_COND_EQ = 0,
    JIT_LIR_COND_NE,
    JIT_LIR_COND_LT,
    JIT_LIR_COND_LE,
    JIT_LIR_COND_GT,
    JIT_LIR_COND_GE,
} JitLirCondition;

typedef enum JitLirRuntimeKind {
    JIT_LIR_RUNTIME_MIR_OPCODE = 0,
    JIT_LIR_RUNTIME_FRONTEND_HELPER,
} JitLirRuntimeKind;

typedef enum JitLirOpcode {
    JIT_LIR_NOP = 0,

    /* Region/runtime inputs and constants. */
    JIT_LIR_INPUT,
    JIT_LIR_VALUE_INPUT,
    JIT_LIR_CONST_I64,
    JIT_LIR_CONST_F64,
    JIT_LIR_CONST_VALUE,

    /* Data movement. */
    JIT_LIR_COPY,
    JIT_LIR_SPILL_LOAD,
    JIT_LIR_SPILL_STORE,

    /* TValue/native representation changes. */
    JIT_LIR_BOX_I64,
    JIT_LIR_BOX_F64,
    JIT_LIR_BOX_BOOL,
    JIT_LIR_UNBOX_I64,
    JIT_LIR_UNBOX_F64,
    JIT_LIR_UNBOX_BOOL,
    JIT_LIR_IS_TYPE,
    JIT_LIR_IS_IDENTICAL,
    JIT_LIR_NOT_BOOL,

    /* Native integer operations. */
    JIT_LIR_ADD_I64,
    JIT_LIR_SUB_I64,
    JIT_LIR_MUL_I64,
    JIT_LIR_DIV_I64,
    JIT_LIR_MOD_I64,
    JIT_LIR_AND_I64,
    JIT_LIR_OR_I64,
    JIT_LIR_XOR_I64,
    JIT_LIR_SHL_I64,
    JIT_LIR_SHR_I64,
    JIT_LIR_NEG_I64,
    JIT_LIR_NOT_I64,

    /* Native floating-point operations. */
    JIT_LIR_ADD_F64,
    JIT_LIR_SUB_F64,
    JIT_LIR_MUL_F64,
    JIT_LIR_DIV_F64,
    JIT_LIR_NEG_F64,

    /* Comparisons produce a GPR boolean. */
    JIT_LIR_CMP_I64,
    JIT_LIR_CMP_F64,

    /* Runtime operations are semantic calls that the SLJIT emitter expands. */
    JIT_LIR_RUNTIME,
    JIT_LIR_CALL,

    /* VM synchronization and polling. */
    JIT_LIR_SYNC_STATE,
    JIT_LIR_SAFEPOINT,
    JIT_LIR_POLL,

    /* Control flow. */
    JIT_LIR_JUMP,
    JIT_LIR_BRANCH,
    JIT_LIR_RETURN,
    JIT_LIR_YIELD,
    JIT_LIR_EXIT_INTERPRETER,
    JIT_LIR_ERROR,

    JIT_LIR__COUNT,
} JitLirOpcode;

typedef enum JitLirFlags {
    JIT_LIR_FLAG_NONE = 0,
    JIT_LIR_FLAG_CALL = 1u << 0,
    JIT_LIR_FLAG_RAISE = 1u << 1,
    JIT_LIR_FLAG_BARRIER = 1u << 2,
    JIT_LIR_FLAG_COLD = 1u << 3,
} JitLirFlags;

typedef struct JitLirInst {
    JitLirOpcode op;
    JitLirOperandRange defs;
    JitLirOperandRange uses;
    JitImmediate imm;
    uint32_t aux;
    uint32_t aux2;
    uint32_t aux3;
    uint32_t flags;
    JitLirBlockId target;
    JitLirBlockId target2;
    JitLirStateId state;
    JitSourceLocation source;
    bool removed;
} JitLirInst;

typedef enum JitLirBlockFlags {
    JIT_LIR_BLOCK_NONE = 0,
    JIT_LIR_BLOCK_ENTRY = 1u << 0,
    JIT_LIR_BLOCK_LOOP_HEADER = 1u << 1,
    JIT_LIR_BLOCK_LOOP_LATCH = 1u << 2,
    JIT_LIR_BLOCK_COLD = 1u << 3,
    JIT_LIR_BLOCK_SYNTHETIC = 1u << 4,
} JitLirBlockFlags;

typedef struct JitLirBlock {
    JitLirBlockId id;
    JitBlockId mir_block;
    c11_vector insts;        /* JitLirInst */
    c11_vector predecessors; /* JitLirBlockId */
    c11_vector successors;   /* JitLirBlockId */
    uint32_t frequency;
    uint32_t flags;
} JitLirBlock;

typedef struct JitLirState {
    int32_t bytecode_ip;
    JitLirOperandRange locals;
    JitLirOperandRange stack;
} JitLirState;

typedef struct JitLirEdgeCopy {
    JitLirBlockId from;
    JitLirBlockId to;
    c11_vector dsts; /* JitLirOperand; VREG destinations */
    c11_vector srcs; /* JitLirOperand */
} JitLirEdgeCopy;

typedef struct JitLirFunction {
    c11_vector blocks;          /* JitLirBlock */
    c11_vector operands;        /* JitLirOperand */
    c11_vector vregs;           /* JitVReg */
    c11_vector stack_slots;     /* JitStackSlot */
    c11_vector states;          /* JitLirState */
    c11_vector state_operands;  /* JitLirOperand */
    c11_vector edge_copies;     /* JitLirEdgeCopy */
    c11_vector mir_value_map;   /* JitLirOperand, indexed by MIR JitValueId */

    JitLirBlockId entry_block;
    uint32_t local_stack_size;
    uint32_t tvalue_size;
    uint32_t stack_alignment;
} JitLirFunction;

JitTypeInfo jit_lir_type_void(void);
JitLirOperand jit_lir_none(void);
JitLirOperand jit_lir_vreg(JitVRegId id, JitTypeInfo type);
JitLirOperand jit_lir_preg(JitPhysRegBank bank, uint16_t index, JitTypeInfo type);
JitLirOperand jit_lir_stack(JitStackSlotId slot, JitTypeInfo type);
JitLirOperand jit_lir_imm_i64(int64_t value, JitTypeInfo type);
JitLirOperand jit_lir_imm_f64(double value, JitTypeInfo type);
JitLirOperand jit_lir_ptr(uintptr_t value, JitTypeInfo type);
JitLirOperand jit_lir_block_operand(JitLirBlockId block);

void jit_lir_init(JitLirFunction* lir, uint32_t tvalue_size, uint32_t stack_alignment);
void jit_lir_destroy(JitLirFunction* lir);

JitLirBlockId jit_lir_add_block(JitLirFunction* lir,
                                JitBlockId mir_block,
                                uint32_t frequency,
                                uint32_t flags);
JitLirBlock* jit_lir_block(JitLirFunction* lir, JitLirBlockId id);
const JitLirBlock* jit_lir_block_const(const JitLirFunction* lir, JitLirBlockId id);

JitVRegId jit_lir_new_vreg(JitLirFunction* lir,
                           JitVRegClass reg_class,
                           JitTypeInfo type,
                           JitValueId mir_value);
JitStackSlotId jit_lir_new_stack_slot(JitLirFunction* lir,
                                      uint32_t size,
                                      uint32_t alignment,
                                      JitStackSlotKind kind);

JitLirOperandRange jit_lir_append_operands(JitLirFunction* lir,
                                           const JitLirOperand* operands,
                                           uint32_t count);
const JitLirOperand* jit_lir_inst_defs(const JitLirFunction* lir, const JitLirInst* inst);
const JitLirOperand* jit_lir_inst_uses(const JitLirFunction* lir, const JitLirInst* inst);
JitLirOperand* jit_lir_inst_defs_mut(JitLirFunction* lir, JitLirInst* inst);
JitLirOperand* jit_lir_inst_uses_mut(JitLirFunction* lir, JitLirInst* inst);

void jit_lir_emit(JitLirFunction* lir,
                  JitLirBlockId block,
                  JitLirOpcode op,
                  const JitLirOperand* defs,
                  uint32_t def_count,
                  const JitLirOperand* uses,
                  uint32_t use_count,
                  JitImmediate imm,
                  uint32_t aux,
                  uint32_t aux2,
                  uint32_t aux3,
                  uint32_t flags,
                  JitLirBlockId target,
                  JitLirBlockId target2,
                  JitLirStateId state,
                  JitSourceLocation source);

JitLirStateId jit_lir_add_state(JitLirFunction* lir,
                                int32_t bytecode_ip,
                                const JitLirOperand* locals,
                                uint32_t locals_count,
                                const JitLirOperand* stack,
                                uint32_t stack_count);
const JitLirOperand* jit_lir_state_locals(const JitLirFunction* lir, const JitLirState* state);
const JitLirOperand* jit_lir_state_stack(const JitLirFunction* lir, const JitLirState* state);

JitLirEdgeCopy* jit_lir_get_edge_copy(JitLirFunction* lir,
                                      JitLirBlockId from,
                                      JitLirBlockId to,
                                      bool create);
bool jit_lir_add_edge_copy(JitLirFunction* lir,
                           JitLirBlockId from,
                           JitLirBlockId to,
                           JitLirOperand dst,
                           JitLirOperand src);

void jit_lir_rebuild_cfg(JitLirFunction* lir);
bool jit_lir_resolve_parallel_copies(JitLirFunction* lir,
                                     char* error,
                                     uint32_t error_size);
bool jit_lir_verify(const JitLirFunction* lir, char* error, uint32_t error_size);
bool jit_lir_peephole(JitLirFunction* lir);
void jit_lir_layout_stack(JitLirFunction* lir);
void jit_lir_dump(const JitLirFunction* lir, void (*write)(const char*, void*), void* ctx);

const char* jit_lir_opcode_name(JitLirOpcode op);
const char* jit_vreg_class_name(JitVRegClass reg_class);

#ifdef __cplusplus
}
#endif
