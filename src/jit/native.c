#include "tinypy/jit/native.h"

#include "tinypy/interpreter/jitbridge.h"
#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/interpreter/regvm.h"
#include "tinypy/jit/lir.h"
#include "tinypy/jit/regalloc.h"
#include "tinypy/objects/base.h"
#include "sljitLir.h"

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

#define TINYJIT_MAX_CODE 4096
#define TINYJIT_MAX_REGS 256
#define TINYJIT_LIR_MAX_ISLAND 64
#define TINYJIT_LIR_GPRS 8
#define TINYJIT_LIR_RESERVED_GPRS 2
#define TINYJIT_RESUME_BASE 8192
#define TINYJIT_RETURN_NONE 256
#define TINYJIT_LOOP_FUEL 4096
#define TINYJIT_CACHE_LIMIT_BYTES ((size_t)32 * 1024 * 1024)

typedef sljit_sw (*TinyJitEntry)(py_TValue* values, sljit_sw fuel, sljit_sw start_pc);

typedef struct TinyJitCode {
    const RegCode* regcode;
    const CodeObject* source;
    void* executable;
    TinyJitEntry entry;
    size_t executable_bytes;
    uint64_t signature;
    uint8_t* valid_entry_pc;
    uint32_t lir_islands;
    uint32_t lir_instructions;
    uint32_t colored_vregs;
    uint32_t spilled_vregs;
    struct TinyJitCode* next;
} TinyJitCode;

typedef struct TinyJitContext {
    TinyJitCode* code;
    uint64_t compiled;
    uint64_t entered;
    uint64_t completed;
    uint64_t resumed;
    size_t generated_bytes;
    uint64_t lir_islands;
    uint64_t lir_instructions;
    uint64_t colored_vregs;
    uint64_t spilled_vregs;
} TinyJitContext;

typedef struct TinyJitFixup {
    struct sljit_jump* jump;
    int pc;
    bool resume;
} TinyJitFixup;

typedef struct TinyJitEmitter {
    struct sljit_compiler* compiler;
    struct sljit_label** blocks;
    struct sljit_label** resume;
    TinyJitFixup* fixes;
    size_t fix_count;
    size_t fix_capacity;
    int code_length;
    bool failed;
} TinyJitEmitter;

/* Each island is a straight-line numeric region with its own SSA LIR and RA. */
typedef struct TinyJitIsland {
    int first_pc;
    int last_pc;
    JitLirFunction lir;
    JitRegAllocStats stats;
    uint32_t physical_vregs;
} TinyJitIsland;

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

static bool remember(TinyJitEmitter* out, struct sljit_jump* jump, int pc, bool resume) {
    if(jump == NULL || pc < 0 || pc >= out->code_length ||
       out->fix_count >= out->fix_capacity) {
        out->failed = true;
        return false;
    }
    TinyJitFixup* item = &out->fixes[out->fix_count++];
    item->jump = jump;
    item->pc = pc;
    item->resume = resume;
    return true;
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

static void emit_int_value(struct sljit_compiler* compiler, unsigned dst) {
    sljit_emit_op1(compiler, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, type)), SLJIT_IMM, tp_int);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, is_ptr)), SLJIT_IMM, 0);
    sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, _i64)), SLJIT_R0, 0);
}

static void emit_bool_value(struct sljit_compiler* compiler, unsigned dst, bool value) {
    sljit_emit_op1(compiler, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, type)), SLJIT_IMM, tp_bool);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, is_ptr)), SLJIT_IMM, 0);
    sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                   field_offset(dst, offsetof(py_TValue, _bool)), SLJIT_IMM, value ? 1 : 0);
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

static bool is_arithmetic(RegOpcode op) {
    switch(op) {
        case RBC_ADD: case RBC_SUB: case RBC_MUL:
        case RBC_ADD_INT: case RBC_SUB_INT: case RBC_MUL_INT:
        case RBC_ADD_NUM: case RBC_SUB_NUM: case RBC_MUL_NUM:
        case RBC_ADD_I8: case RBC_SUB_I8: case RBC_MUL_I8:
            return true;
        default: return false;
    }
}

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

static sljit_s32 arithmetic_opcode(RegOpcode op) {
    switch(op) {
        case RBC_ADD: case RBC_ADD_INT: case RBC_ADD_NUM: case RBC_ADD_I8: return SLJIT_ADD;
        case RBC_SUB: case RBC_SUB_INT: case RBC_SUB_NUM: case RBC_SUB_I8: return SLJIT_SUB;
        case RBC_MUL: case RBC_MUL_INT: case RBC_MUL_NUM: case RBC_MUL_I8: return SLJIT_MUL;
        default: return -1;
    }
}


/*
 * Model the unboxed dataflow of a numeric island in LIR. Every result is
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
        JitLirOpcode lir_op;
        if(is_arithmetic(op)) {
            switch(arithmetic_opcode(op)) {
                case SLJIT_ADD: lir_op = JIT_LIR_ADD_I64; break;
                case SLJIT_SUB: lir_op = JIT_LIR_SUB_I64; break;
                case SLJIT_MUL: lir_op = JIT_LIR_MUL_I64; break;
                default: return false;
            }
        } else {
            lir_op = JIT_LIR_CMP_I64;
        }
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
    config.reserved_gpr_scratch = TINYJIT_LIR_RESERVED_GPRS;
    config.gpr_saved_count = 3;
    config.reserved_gpr_saved = 3; /* S0-S2 hold VM state, fuel, entry PC. */
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

/* Split islands at every possible control-flow entry and every loop header. */
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

static bool prepare_lir_islands(const RegCode* code, TinyJitIsland** result,
                                int* island_count, uint32_t* max_stack,
                                uint32_t* colored, uint32_t* spilled,
                                uint32_t* lir_instructions) {
    const int length = code->code.length;
    bool* entry = calloc((size_t)length, sizeof(bool));
    TinyJitIsland* islands = calloc((size_t)length, sizeof(*islands));
    if(entry == NULL || islands == NULL || !native_entry_points(code, entry)) {
        free(entry);
        free(islands);
        return false;
    }
    int count = 0;
    for(int pc = 0; pc < length;) {
        RegOpcode op = RBC_OP(c11__getitem(RegBytecode, &code->code, pc));
        if(!is_arithmetic(op) && !is_compare_binary(op) && !is_compare_i8(op)) {
            pc++;
            continue;
        }
        int first = pc;
        do {
            pc++;
            if(pc >= length || pc - first >= TINYJIT_LIR_MAX_ISLAND || entry[pc]) break;
            op = RBC_OP(c11__getitem(RegBytecode, &code->code, pc));
        } while(is_arithmetic(op) || is_compare_binary(op) || is_compare_i8(op));
        TinyJitIsland* island = &islands[count];
        if(!build_lir_island(island, code, first, pc)) {
            free(entry);
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
    free(entry);
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
        } else if(inst->op == JIT_LIR_ADD_I64 || inst->op == JIT_LIR_SUB_I64 ||
                  inst->op == JIT_LIR_MUL_I64) {
            if(inst->defs.count != 1 || inst->uses.count != 2) return false;
            const sljit_s32 dst = lir_phys_reg(defs[0]);
            sljit_s32 lhs, rhs;
            sljit_sw lhsw, rhsw;
            if(dst < 0 || !lir_source(lir, uses[0], &lhs, &lhsw) ||
               !lir_source(lir, uses[1], &rhs, &rhsw)) return false;
            const sljit_s32 op = inst->op == JIT_LIR_ADD_I64 ? SLJIT_ADD :
                                 inst->op == JIT_LIR_SUB_I64 ? SLJIT_SUB : SLJIT_MUL;
            sljit_emit_op2(c, op | SLJIT_SET_OVERFLOW, dst, 0, lhs, lhsw, rhs, rhsw);
            remember(out, sljit_emit_jump(c, SLJIT_OVERFLOW), pc, true);
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
            const int t = inst->aux2 ? tp_bool : tp_int;
            sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0),
                           field_offset(inst->aux, offsetof(py_TValue, type)), SLJIT_IMM, t);
            sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0),
                           field_offset(inst->aux, offsetof(py_TValue, is_ptr)), SLJIT_IMM, 0);
            sljit_emit_op1(c, inst->aux2 ? SLJIT_MOV_U8 : SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_S0),
                           field_offset(inst->aux, inst->aux2 ? offsetof(py_TValue, _bool) :
                                                        offsetof(py_TValue, _i64)),
                           src, srcw);
        } else {
            return false;
        }
        if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
    }
    return true;
}

static bool supported(RegOpcode op) {
    if(is_arithmetic(op) || is_compare_binary(op) || is_compare_i8(op)) return true;
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

static void emit_backedge_poll(TinyJitEmitter* out, int target) {
    struct sljit_compiler* c = out->compiler;
    sljit_emit_op2(c, SLJIT_SUB, SLJIT_S1, 0, SLJIT_S1, 0, SLJIT_IMM, 1);
    remember(out, sljit_emit_cmp(c, SLJIT_EQUAL,
                                 SLJIT_S1, 0, SLJIT_IMM, 0), target, true);
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

static void emit_binary(TinyJitEmitter* out, RegOpcode op,
                        unsigned a, unsigned b, unsigned c, int pc) {
    struct sljit_compiler* sljit = out->compiler;
    emit_guard_type(out, b, tp_int, pc);
    if(!is_arith_i8(op) && !is_compare_i8(op)) emit_guard_type(out, c, tp_int, pc);
    emit_load_int(sljit, b, SLJIT_R0);
    if(is_arith_i8(op) || is_compare_i8(op)) {
        sljit_emit_op1(sljit, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (int8_t)c);
    } else {
        emit_load_int(sljit, c, SLJIT_R1);
    }

    if(is_arithmetic(op)) {
        sljit_emit_op2(sljit, arithmetic_opcode(op) | SLJIT_SET_OVERFLOW,
                       SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
        remember(out, sljit_emit_jump(sljit, SLJIT_OVERFLOW), pc, true);
        emit_int_value(sljit, a);
        return;
    }

    sljit_s32 cond = compare_condition(op);
    struct sljit_jump* take = sljit_emit_cmp(sljit, cond,
                                             SLJIT_R0, 0, SLJIT_R1, 0);
    emit_bool_value(sljit, a, false);
    struct sljit_jump* done = sljit_emit_jump(sljit, SLJIT_JUMP);
    struct sljit_label* yes = sljit_emit_label(sljit);
    if(take == NULL || done == NULL || yes == NULL) { out->failed = true; return; }
    sljit_set_label(take, yes);
    emit_bool_value(sljit, a, true);
    struct sljit_label* after = sljit_emit_label(sljit);
    if(after == NULL) { out->failed = true; return; }
    sljit_set_label(done, after);
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

static bool emit_instructions(TinyJitEmitter* out,
                              const RegCode* code, const CodeObject* source,
                              const TinyJitIsland* islands, int island_count) {
    struct sljit_compiler* c = out->compiler;
    int island_index = 0;
    for(int pc = 0; pc < code->code.length; pc++) {
        if(island_index < island_count && islands[island_index].first_pc == pc) {
            if(!emit_lir_island(out, &islands[island_index])) return false;
            pc = islands[island_index].last_pc - 1;
            island_index++;
            continue;
        }
        const RegBytecode inst = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(inst);
        const unsigned a = RBC_A(inst);
        const unsigned b = RBC_B(inst);
        const unsigned v = RBC_C(inst);
        out->blocks[pc] = sljit_emit_label(c);
        if(out->blocks[pc] == NULL) return false;

        if(is_arithmetic(op) || is_compare_binary(op) || is_compare_i8(op)) {
            emit_binary(out, op, a, b, v, pc);
        } else {
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
                case RBC_RETURN: sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, (sljit_sw)a); break;
                case RBC_RETURN_NONE:
                    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, TINYJIT_RETURN_NONE); break;
                default: return false;
            }
        }
        if(out->failed || sljit_get_compiler_error(c) != SLJIT_SUCCESS) return false;
    }
    sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM,
                      TINYJIT_RESUME_BASE + code->code.length - 1);
    for(int pc = 0; pc < code->code.length; pc++) {
        out->resume[pc] = sljit_emit_label(c);
        if(out->resume[pc] == NULL) return false;
        sljit_emit_return(c, SLJIT_MOV, SLJIT_IMM, TINYJIT_RESUME_BASE + pc);
    }
    for(size_t i = 0; i < out->fix_count; i++) {
        TinyJitFixup* fixup = &out->fixes[i];
        struct sljit_label* label = fixup->resume ? out->resume[fixup->pc]
                                                  : out->blocks[fixup->pc];
        if(label == NULL) return false;
        sljit_set_label(fixup->jump, label);
    }
    return sljit_get_compiler_error(c) == SLJIT_SUCCESS;
}

static TinyJitCode* compile_function(const FuncDecl* decl, const RegCode* code) {
    if(code->nregs == 0 || code->nregs > TINYJIT_MAX_REGS ||
       code->code.length < 2 || code->code.length > TINYJIT_MAX_CODE ||
       code->source_ip.length != code->code.length) return NULL;
    bool has_native_work = false;
    for(int pc = 0; pc < code->code.length; pc++) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        const RegOpcode op = RBC_OP(word);
        if(!supported(op)) return NULL;
        if(is_arithmetic(op) || is_compare_binary(op) || op == RBC_CMP_BRANCH ||
           op == RBC_CMP_I8_BRANCH || is_compare_i8(op)) has_native_work = true;
        if(op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH) {
            if(pc + 1 >= code->code.length ||
               RBC_OP(c11__getitem(RegBytecode, &code->code, pc + 1)) != RBC_SUPER_DATA) return NULL;
            pc++;
        }
        if(op == RBC_LOAD_CONST && RBC_BX(word) >= decl->code.consts.length) return NULL;
    }
    if(!has_native_work) return NULL;

    TinyJitIsland* islands = NULL;
    int island_count = 0;
    uint32_t max_stack_bytes = 0;
    uint32_t colored_vregs = 0;
    uint32_t spilled_vregs = 0;
    uint32_t lir_instructions = 0;
    if(!prepare_lir_islands(code, &islands, &island_count, &max_stack_bytes,
                            &colored_vregs, &spilled_vregs, &lir_instructions)) return NULL;
    if(max_stack_bytes > SLJIT_MAX_LOCAL_SIZE) {
        free_lir_islands(islands, island_count);
        return NULL;
    }

    TinyJitEmitter out = {0};
    out.code_length = code->code.length;
    out.fix_capacity = (size_t)out.code_length * 16u;
    out.blocks = calloc((size_t)out.code_length, sizeof(*out.blocks));
    out.resume = calloc((size_t)out.code_length, sizeof(*out.resume));
    out.fixes = calloc(out.fix_capacity, sizeof(*out.fixes));
    out.compiler = sljit_create_compiler(NULL);
    if(out.blocks == NULL || out.resume == NULL || out.fixes == NULL ||
       out.compiler == NULL) goto failure;

    /* Saved registers hold the TValue array, remaining loop fuel and entry PC. */
    sljit_emit_enter(out.compiler, 0, SLJIT_ARGS3(W, P, W, W),
                     TINYJIT_LIR_GPRS, 3, (sljit_s32)max_stack_bytes);
    sljit_emit_op1(out.compiler, SLJIT_MOV, SLJIT_S0, 0, SLJIT_R0, 0);
    sljit_emit_op1(out.compiler, SLJIT_MOV, SLJIT_S1, 0, SLJIT_R1, 0);
    sljit_emit_op1(out.compiler, SLJIT_MOV, SLJIT_S2, 0, SLJIT_R2, 0);
    if(sljit_get_compiler_error(out.compiler) != SLJIT_SUCCESS) goto failure;
    /* Loop JIT requests may enter the same function at a hot loop header. */
    for(int pc = 0; pc < code->code.length; pc++) {
        if(!remember(&out, sljit_emit_cmp(out.compiler, SLJIT_EQUAL,
                    SLJIT_S2, 0, SLJIT_IMM, pc), pc, false)) goto failure;
    }
    sljit_emit_return(out.compiler, SLJIT_MOV, SLJIT_IMM, TINYJIT_RESUME_BASE);
    if(sljit_get_compiler_error(out.compiler) != SLJIT_SUCCESS) goto failure;
    if(!emit_instructions(&out, code, &decl->code, islands, island_count)) goto failure;

    void* executable = sljit_generate_code(out.compiler, 0, NULL);
    if(executable == NULL) goto failure;
    TinyJitCode* result = calloc(1, sizeof(*result));
    if(result == NULL) {
        sljit_free_code(executable, NULL);
        goto failure;
    }
    result->regcode = code;
    result->source = &decl->code;
    result->executable = executable;
    result->executable_bytes = (size_t)sljit_get_generated_code_size(out.compiler);
    result->valid_entry_pc = malloc((size_t)code->code.length);
    if(result->valid_entry_pc == NULL) {
        sljit_free_code(executable, NULL);
        free(result);
        goto failure;
    }
    memset(result->valid_entry_pc, 1, (size_t)code->code.length);
    for(int i = 0; i < island_count; i++) {
        for(int pc = islands[i].first_pc + 1; pc < islands[i].last_pc; pc++) {
            result->valid_entry_pc[pc] = 0;
        }
    }
    result->lir_islands = (uint32_t)island_count;
    result->lir_instructions = lir_instructions;
    result->colored_vregs = colored_vregs;
    result->spilled_vregs = spilled_vregs;
    _Static_assert(sizeof(TinyJitEntry) == sizeof(void*), "Incompatible native code pointer");
    memcpy(&result->entry, &executable, sizeof(result->entry));
    sljit_free_compiler(out.compiler);
    free(out.blocks);
    free(out.resume);
    free(out.fixes);
    free_lir_islands(islands, island_count);
    return result;

failure:
    if(out.compiler != NULL) sljit_free_compiler(out.compiler);
    free(out.blocks);
    free(out.resume);
    free(out.fixes);
    free_lir_islands(islands, island_count);
    return NULL;
}

static void release_code(TinyJitCode* code) {
    if(code->executable != NULL) sljit_free_code(code->executable, NULL);
    free(code->valid_entry_pc);
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
            *node = old->next;
            context->generated_bytes -= old->executable_bytes;
            release_code(old);
            break;
        }
        node = &(*node)->next;
    }

    TinyJitCode* compiled = compile_function(request->decl, request->code);
    if(compiled == NULL) return false;
    compiled->signature = signature;
    compiled->next = context->code;
    context->code = compiled;
    context->compiled++;
    context->lir_islands += compiled->lir_islands;
    context->lir_instructions += compiled->lir_instructions;
    context->colored_vregs += compiled->colored_vregs;
    context->spilled_vregs += compiled->spilled_vregs;
    context->generated_bytes += compiled->executable_bytes;
    /* Bound executable memory even when Python functions are discarded. */
    while(context->generated_bytes > TINYJIT_CACHE_LIMIT_BYTES &&
          context->code != NULL && context->code->next != NULL) {
        TinyJitCode* previous = context->code;
        while(previous->next->next != NULL) previous = previous->next;
        TinyJitCode* oldest = previous->next;
        previous->next = NULL;
        context->generated_bytes -= oldest->executable_bytes;
        release_code(oldest);
    }
    return true;
}

static bool native_enter_at(VM* vm, py_Frame* frame, RegCode* code,
                            int32_t start_pc, RegJitExit* exit, void* userdata) {
    TinyJitContext* context = userdata;
    if(context == NULL || frame == NULL || code == NULL || exit == NULL ||
       frame->locals == NULL || code->nregs == 0 || code->nregs > TINYJIT_MAX_REGS ||
       start_pc < 0 || start_pc >= code->code.length) return false;
    TinyJitCode* compiled = context->code;
    while(compiled != NULL && (compiled->regcode != code || compiled->source != frame->co)) {
        compiled = compiled->next;
    }
    if(compiled == NULL || compiled->valid_entry_pc[start_pc] == 0) return false;

    py_TValue working[TINYJIT_MAX_REGS];
    memcpy(working, frame->locals, (size_t)code->nregs * sizeof(py_TValue));
    context->entered++;
    const sljit_sw result = compiled->entry(working, TINYJIT_LOOP_FUEL, start_pc);
    if(result >= 0 && result < code->nregs) {
        vm->last_retval = working[result];
        exit->kind = REG_JIT_EXIT_RETURN;
        context->completed++;
        return true;
    }
    if(result == TINYJIT_RETURN_NONE) {
        py_newnone(&vm->last_retval);
        exit->kind = REG_JIT_EXIT_RETURN;
        context->completed++;
        return true;
    }
    if(result >= TINYJIT_RESUME_BASE &&
       result - TINYJIT_RESUME_BASE < code->code.length) {
        const int pc = (int)(result - TINYJIT_RESUME_BASE);
        memcpy(frame->locals, working, (size_t)code->nregs * sizeof(py_TValue));
        frame->ip = c11__getitem(int32_t, &code->source_ip, pc);
        exit->kind = REG_JIT_EXIT_RESUME;
        exit->resume_pc = pc;
        context->resumed++;
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
                "deopts=%llu executable_bytes=%zu "
                "lir_islands=%llu lir_instructions=%llu "
                "colored_vregs=%llu spills=%llu\n",
                (unsigned long long)context->compiled,
                (unsigned long long)context->entered,
                (unsigned long long)context->completed,
                (unsigned long long)context->resumed,
                context->generated_bytes,
                (unsigned long long)context->lir_islands,
                (unsigned long long)context->lir_instructions,
                (unsigned long long)context->colored_vregs,
                (unsigned long long)context->spilled_vregs);
    }
    TinyJitCode* code = context->code;
    while(code != NULL) {
        TinyJitCode* next = code->next;
        release_code(code);
        code = next;
    }
    free(context);
}
