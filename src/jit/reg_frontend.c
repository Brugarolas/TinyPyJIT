#include "tinypy/jit/reg_frontend.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RegMirBlock {
    int32_t start;
    int32_t end;
    int32_t last;
    int32_t successors[2];
    uint8_t successor_count;
    bool reachable;
    bool loop_header;
    bool loop_latch;
    JitBlockId mir;
    int32_t* predecessors;
    uint32_t predecessor_count;
    uint32_t predecessor_written;
    JitValueId* phis;
} RegMirBlock;

typedef struct RegMirContext {
    const RegCode* code;
    const CodeObject* source;
    const JitRegFrontendOptions* options;
    JitRegFrontendStats* stats;
    JitRegLowerError* error;
    JitRegion* region;
    RegMirBlock* blocks;
    int32_t block_count;
    int32_t word_count;
    int32_t* ip_to_block;
    uint8_t* spans;
    bool* leaders;
    bool initialized;
} RegMirContext;

static JitImmediate zero_imm(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

static bool fail(RegMirContext* ctx, JitRegLowerStatus status, int32_t pc, const char* fmt, ...) {
    if(ctx->error != NULL) {
        ctx->error->status = status;
        ctx->error->reg_pc = pc;
        va_list args;
        va_start(args, fmt);
        vsnprintf(ctx->error->message, sizeof(ctx->error->message), fmt, args);
        va_end(args);
        ctx->error->message[sizeof(ctx->error->message) - 1u] = '\0';
    }
    return false;
}

static RegBytecode word_at(const RegMirContext* ctx, int32_t pc) {
    return c11__getitem(RegBytecode, &ctx->code->code, pc);
}

static bool register_valid(const RegMirContext* ctx, uint32_t reg) {
    return reg < ctx->code->nregs;
}

static bool is_fused_branch(RegOpcode op) {
    return op == RBC_CMP_BRANCH || op == RBC_CMP_I8_BRANCH;
}

static bool is_conditional(RegOpcode op) {
    switch(op) {
        case RBC_JUMP_IF_FALSE:
        case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP:
        case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_CMP_BRANCH:
        case RBC_CMP_I8_BRANCH: return true;
        default: return false;
    }
}

static bool supported(RegOpcode op) {
    switch(op) {
        case RBC_NOP:
        case RBC_MOVE:
        case RBC_LOAD_CONST:
        case RBC_LOAD_NONE:
        case RBC_LOAD_NIL:
        case RBC_LOAD_ELLIPSIS:
        case RBC_LOAD_BOOL:
        case RBC_LOAD_I16:
        case RBC_ADD:
        case RBC_SUB:
        case RBC_MUL:
        case RBC_TRUEDIV:
        case RBC_FLOORDIV:
        case RBC_MOD:
        case RBC_POW:
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
        case RBC_ADD_I8:
        case RBC_SUB_I8:
        case RBC_MUL_I8:
        case RBC_CMP_LT_I8:
        case RBC_CMP_LE_I8:
        case RBC_CMP_EQ_I8:
        case RBC_CMP_NE_I8:
        case RBC_CMP_GT_I8:
        case RBC_CMP_GE_I8:
        case RBC_CMP_BRANCH:
        case RBC_CMP_I8_BRANCH:
        case RBC_JUMP:
        case RBC_JUMP_IF_FALSE:
        case RBC_JUMP_IF_TRUE:
        case RBC_JUMP_IF_TRUE_OR_POP:
        case RBC_JUMP_IF_FALSE_OR_POP:
        case RBC_RETURN:
        case RBC_RETURN_NONE: return true;
        default: return false;
    }
}

static bool valid_target(const RegMirContext* ctx, int32_t target) {
    return target >= 0 && target < ctx->word_count && ctx->spans[target] != 0;
}

static int32_t branch_target(const RegMirContext* ctx, int32_t pc, RegOpcode op) {
    if(is_fused_branch(op)) return pc + 1 + RBC_SBX(word_at(ctx, pc + 1));
    return pc + RBC_SBX(word_at(ctx, pc));
}

static int32_t branch_fallthrough(const RegMirContext* ctx, int32_t pc, RegOpcode op) {
    if(is_fused_branch(op)) {
        const RegBytecode extension = word_at(ctx, pc + 1);
        return pc + 2 + RBC_A(extension);
    }
    return pc + 1;
}

static bool scan_instructions(RegMirContext* ctx) {
    const int32_t n = ctx->word_count;
    for(int32_t pc = 0; pc < n;) {
        const RegOpcode op = RBC_OP(word_at(ctx, pc));
        if(op == RBC_SUPER_DATA)
            return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "orphan RBC_SUPER_DATA");
        if(op >= RBC__COUNT)
            return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "invalid opcode %u", (unsigned)op);
        const int size = is_fused_branch(op) ? 2 : 1;
        if(size == 2 && (pc + 1 >= n || RBC_OP(word_at(ctx, pc + 1)) != RBC_SUPER_DATA))
            return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "missing fused-branch extension");
        ctx->spans[pc] = (uint8_t)size;
        pc += size;
    }

    ctx->leaders[0] = true;
    for(int32_t pc = 0; pc < n; pc += ctx->spans[pc]) {
        const RegOpcode op = RBC_OP(word_at(ctx, pc));
        const int32_t next = pc + ctx->spans[pc];
        if(op == RBC_JUMP || is_conditional(op)) {
            const int32_t target = branch_target(ctx, pc, op);
            if(!valid_target(ctx, target))
                return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "invalid jump target %d", target);
            ctx->leaders[target] = true;
            if(is_conditional(op)) {
                const int32_t fall = branch_fallthrough(ctx, pc, op);
                if(!valid_target(ctx, fall))
                    return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "invalid fallthrough %d", fall);
                ctx->leaders[fall] = true;
            }
        }
        if(op == RBC_RETURN || op == RBC_RETURN_NONE || op == RBC_JUMP ||
           is_conditional(op) || !supported(op)) {
            if(next < n) ctx->leaders[next] = true;
        }
        if(!supported(op)) ctx->leaders[pc] = true;
    }
    return true;
}

static void add_successor(RegMirBlock* block, int32_t next) {
    for(uint8_t i = 0; i < block->successor_count; i++) {
        if(block->successors[i] == next) return;
    }
    block->successors[block->successor_count++] = next;
}

static bool build_blocks(RegMirContext* ctx) {
    const int32_t n = ctx->word_count;
    for(int32_t pc = 0; pc < n; pc++) if(ctx->leaders[pc]) ctx->block_count++;
    ctx->blocks = calloc((size_t)ctx->block_count, sizeof(RegMirBlock));
    if(ctx->blocks == NULL) return fail(ctx, JIT_REG_LOWER_OUT_OF_MEMORY, -1, "allocating CFG blocks");

    int32_t current = -1;
    for(int32_t pc = 0; pc < n; pc++) {
        if(ctx->leaders[pc]) {
            current++;
            RegMirBlock* block = &ctx->blocks[current];
            block->start = pc;
            block->end = n;
            block->mir = JIT_INVALID_BLOCK;
            ctx->ip_to_block[pc] = current;
            if(current > 0) ctx->blocks[current - 1].end = pc;
        }
    }

    for(int32_t i = 0; i < ctx->block_count; i++) {
        RegMirBlock* block = &ctx->blocks[i];
        for(int32_t pc = block->start; pc < block->end; pc += ctx->spans[pc]) {
            if(ctx->spans[pc] == 0 || pc + ctx->spans[pc] > block->end)
                return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "block bisects an instruction");
            block->last = pc;
        }
        const RegOpcode op = RBC_OP(word_at(ctx, block->last));
        if(!supported(op) || op == RBC_RETURN || op == RBC_RETURN_NONE) continue;
        if(op == RBC_JUMP || is_conditional(op)) {
            int32_t target = branch_target(ctx, block->last, op);
            add_successor(block, ctx->ip_to_block[target]);
            if(is_conditional(op)) {
                target = branch_fallthrough(ctx, block->last, op);
                add_successor(block, ctx->ip_to_block[target]);
            }
        } else if(block->end < n) {
            add_successor(block, ctx->ip_to_block[block->end]);
        } else {
            return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, block->last,
                        "bytecode falls off the end without RETURN");
        }
    }

    int32_t* queue = malloc((size_t)ctx->block_count * sizeof(int32_t));
    if(queue == NULL) return fail(ctx, JIT_REG_LOWER_OUT_OF_MEMORY, -1, "allocating reachability queue");
    int32_t read = 0;
    int32_t write = 1;
    queue[0] = 0;
    ctx->blocks[0].reachable = true;
    while(read != write) {
        const int32_t bi = queue[read++];
        const RegMirBlock* block = &ctx->blocks[bi];
        for(uint8_t i = 0; i < block->successor_count; i++) {
            RegMirBlock* next = &ctx->blocks[block->successors[i]];
            if(!next->reachable) {
                next->reachable = true;
                queue[write++] = block->successors[i];
            }
        }
    }
    free(queue);

    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        const RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        for(uint8_t i = 0; i < block->successor_count; i++) {
            RegMirBlock* target = &ctx->blocks[block->successors[i]];
            if(target->start <= block->start) {
                target->loop_header = true;
                ctx->blocks[bi].loop_latch = true;
                ctx->stats->backedges++;
            }
        }
    }

    /* Count predecessors and fill their vectors in linear CFG time. */
    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        RegMirBlock* block = &ctx->blocks[bi];
        if(block->reachable && bi == 0) block->predecessor_count = 1;
    }
    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        const RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        for(uint8_t j = 0; j < block->successor_count; j++) {
            ctx->blocks[block->successors[j]].predecessor_count++;
        }
    }
    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        if(block->predecessor_count == 0)
            return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, block->start,
                        "reachable block has no predecessors");
        block->predecessors = malloc((size_t)block->predecessor_count * sizeof(int32_t));
        if(block->predecessors == NULL)
            return fail(ctx, JIT_REG_LOWER_OUT_OF_MEMORY, block->start, "allocating predecessors");
        if(bi == 0) block->predecessors[block->predecessor_written++] = -1;
    }
    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        const RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        for(uint8_t j = 0; j < block->successor_count; j++) {
            RegMirBlock* target = &ctx->blocks[block->successors[j]];
            target->predecessors[target->predecessor_written++] = bi;
        }
    }
    return true;
}

static int32_t source_line(const RegMirContext* ctx, int32_t reg_pc) {
    if(ctx->source == NULL || reg_pc >= ctx->code->source_ip.length) return -1;
    const int32_t bc_ip = c11__getitem(int32_t, &ctx->code->source_ip, reg_pc);
    if(bc_ip < 0 || bc_ip >= ctx->source->codes_ex.length) return -1;
    return c11__getitem(BytecodeEx, &ctx->source->codes_ex, bc_ip).lineno;
}

static bool create_mir(RegMirContext* ctx) {
    uint32_t frequency = ctx->options->estimated_execution_count;
    if(frequency == 0) frequency = 1;
    JitRegion* region = ctx->region;
    jit_region_init(region, JIT_REGION_FUNCTION, ctx->source, 0, -1);
    ctx->initialized = true;
    region->estimated_execution_count = frequency;
    const JitBlockId entry = jit_region_add_block(region, frequency, JIT_BLOCK_ENTRY);
    (void)entry;

    uint64_t phi_cells = 0;
    for(int32_t i = 0; i < ctx->block_count; i++) {
        RegMirBlock* block = &ctx->blocks[i];
        if(!block->reachable) continue;
        ctx->stats->reachable_blocks++;
        phi_cells += ctx->code->nregs;
        if(phi_cells > ctx->options->max_phi_cells)
            return fail(ctx, JIT_REG_LOWER_TOO_LARGE, block->start,
                        "SSA state exceeds max_phi_cells (%u)", ctx->options->max_phi_cells);
        uint32_t flags = 0;
        if(block->loop_header) flags |= JIT_BLOCK_LOOP_HEADER;
        if(block->loop_latch) flags |= JIT_BLOCK_LOOP_LATCH;
        block->mir = jit_region_add_block(region, frequency, flags);
    }

    for(int32_t i = 0; i < ctx->block_count; i++) {
        RegMirBlock* block = &ctx->blocks[i];
        if(!block->reachable) continue;
        block->phis = malloc(sizeof(JitValueId) * ctx->code->nregs);
        JitValueId* placeholders = malloc(sizeof(JitValueId) * block->predecessor_count);
        if(block->phis == NULL || placeholders == NULL) {
            free(placeholders);
            return fail(ctx, JIT_REG_LOWER_OUT_OF_MEMORY, block->start, "allocating SSA phis");
        }
        for(uint32_t p = 0; p < block->predecessor_count; p++) placeholders[p] = JIT_INVALID_VALUE;
        JitBuilder builder;
        jit_builder_init(&builder, region, block->mir);
        jit_builder_set_source(&builder, block->start, source_line(ctx, block->start));
        for(uint32_t r = 0; r < ctx->code->nregs; r++) {
            block->phis[r] = jit_builder_emit_value(&builder, JIT_OP_PHI, jit_type_value(),
                                                    placeholders, block->predecessor_count,
                                                    zero_imm(), 0);
            ctx->stats->phi_count++;
        }
        free(placeholders);
    }
    return true;
}

static bool fill_edge(RegMirContext* ctx, int32_t source_bi,
                      int32_t target_bi, const JitValueId* values) {
    RegMirBlock* dest = &ctx->blocks[target_bi];
    int32_t predecessor_index = -1;
    for(uint32_t p = 0; p < dest->predecessor_count; p++) {
        if(dest->predecessors[p] == source_bi) {
            predecessor_index = (int32_t)p;
            break;
        }
    }
    if(predecessor_index < 0)
        return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, dest->start,
                    "missing predecessor for SSA edge");
    for(uint32_t r = 0; r < ctx->code->nregs; r++) {
        const JitValueId phi = dest->phis[r];
        const JitInst* inst = jit_region_inst_const(ctx->region,
                                                   jit_region_value_const(ctx->region, phi)->def);
        c11__setitem(JitValueId, &ctx->region->operands,
                     inst->operands.offset + (uint32_t)predecessor_index, values[r]);
    }
    return true;
}

static JitStateId add_state(RegMirContext* ctx, int32_t pc, const JitValueId* regs) {
    return jit_region_add_state(ctx->region, pc, regs, ctx->code->nregs, NULL, 0);
}

static void attach_state_if_raising(RegMirContext* ctx, JitValueId value,
                                    int32_t pc, const JitValueId* before) {
    JitInst* inst = jit_region_inst(ctx->region,
                                   jit_region_value_const(ctx->region, value)->def);
    if((inst->effects & JIT_EFFECT_RAISE) != 0)
        inst->resume_state = add_state(ctx, pc, before);
}

static JitValueId emit_boxed_int(JitBuilder* b, int64_t integer) {
    JitValueId raw = jit_builder_const_i64(b, integer);
    return jit_builder_emit_value(b, JIT_OP_BOX_I64,
                                  jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_INT),
                                  &raw, 1, zero_imm(), 0);
}

static JitValueId emit_boxed_bool(JitBuilder* b, bool value) {
    JitValueId raw = jit_builder_const_bool(b, value);
    return jit_builder_emit_value(b, JIT_OP_BOX_BOOL,
                                  jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_BOOL),
                                  &raw, 1, zero_imm(), 0);
}

static JitOpcode mapped_binary_opcode(RegOpcode op) {
    switch(op) {
        case RBC_ADD: case RBC_ADD_INT: case RBC_ADD_NUM: case RBC_ADD_I8: return JIT_OP_ADD_VALUE;
        case RBC_SUB: case RBC_SUB_INT: case RBC_SUB_NUM: case RBC_SUB_I8: return JIT_OP_SUB_VALUE;
        case RBC_MUL: case RBC_MUL_INT: case RBC_MUL_NUM: case RBC_MUL_I8: return JIT_OP_MUL_VALUE;
        case RBC_TRUEDIV: return JIT_OP_TRUEDIV_VALUE;
        case RBC_FLOORDIV: return JIT_OP_FLOORDIV_VALUE;
        case RBC_MOD: return JIT_OP_MOD_VALUE;
        case RBC_POW: return JIT_OP_POW_VALUE;
        case RBC_CMP_LT: case RBC_CMP_LT_INT: case RBC_CMP_LT_NUM: case RBC_CMP_LT_I8:
            return JIT_OP_LT_VALUE;
        case RBC_CMP_LE: case RBC_CMP_LE_INT: case RBC_CMP_LE_NUM: case RBC_CMP_LE_I8:
            return JIT_OP_LE_VALUE;
        case RBC_CMP_EQ: case RBC_CMP_EQ_INT: case RBC_CMP_EQ_NUM: case RBC_CMP_EQ_I8:
            return JIT_OP_EQ_VALUE;
        case RBC_CMP_NE: case RBC_CMP_NE_INT: case RBC_CMP_NE_NUM: case RBC_CMP_NE_I8:
            return JIT_OP_NE_VALUE;
        case RBC_CMP_GT: case RBC_CMP_GT_INT: case RBC_CMP_GT_NUM: case RBC_CMP_GT_I8:
            return JIT_OP_GT_VALUE;
        case RBC_CMP_GE: case RBC_CMP_GE_INT: case RBC_CMP_GE_NUM: case RBC_CMP_GE_I8:
            return JIT_OP_GE_VALUE;
        default: return JIT_OP_NOP;
    }
}

static JitOpcode mapped_compare_code(uint8_t code) {
    switch(code & REG_CMP_BRANCH_CODE_MASK) {
        case REG_CMP_LT: return JIT_OP_LT_VALUE;
        case REG_CMP_LE: return JIT_OP_LE_VALUE;
        case REG_CMP_EQ: return JIT_OP_EQ_VALUE;
        case REG_CMP_NE: return JIT_OP_NE_VALUE;
        case REG_CMP_GT: return JIT_OP_GT_VALUE;
        case REG_CMP_GE: return JIT_OP_GE_VALUE;
        default: return JIT_OP_NOP;
    }
}

static bool lower_blocks(RegMirContext* ctx) {
    const uint32_t nregs = ctx->code->nregs;
    JitValueId* regs = malloc(sizeof(JitValueId) * nregs);
    if(regs == NULL) return fail(ctx, JIT_REG_LOWER_OUT_OF_MEMORY, -1, "allocating register state");

    JitBuilder builder;
    jit_builder_init(&builder, ctx->region, ctx->region->entry_block);
    jit_builder_set_source(&builder, 0, source_line(ctx, 0));
    for(uint32_t r = 0; r < nregs; r++) {
        JitImmediate imm = zero_imm();
        imm.u32 = r;
        regs[r] = jit_builder_emit_value(&builder, JIT_OP_LOCAL, jit_type_value(), NULL, 0, imm, r);
    }
    if(!fill_edge(ctx, -1, 0, regs)) {
        free(regs);
        return false;
    }
    jit_builder_jump(&builder, ctx->blocks[0].mir);

    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        memcpy(regs, block->phis, sizeof(JitValueId) * nregs);
        jit_builder_set_block(&builder, block->mir);
        jit_builder_set_source(&builder, block->start, source_line(ctx, block->start));

        if(block->loop_header && ctx->options->poll_loop_headers) {
            JitInstId poll = jit_builder_emit(&builder, JIT_OP_POLL,
                                             jit_type_unknown(JIT_TYPE_VOID), NULL, 0,
                                             zero_imm(), 0);
            jit_region_inst(ctx->region, poll)->resume_state = add_state(ctx, block->start, regs);
            ctx->stats->poll_count++;
        }

        bool terminated = false;
        for(int32_t pc = block->start; pc < block->end; pc += ctx->spans[pc]) {
            const RegBytecode byte = word_at(ctx, pc);
            const RegOpcode op = RBC_OP(byte);
            const uint32_t a = RBC_A(byte);
            const uint32_t b = RBC_B(byte);
            const uint32_t c = RBC_C(byte);
            jit_builder_set_source(&builder, pc, source_line(ctx, pc));
            ctx->stats->lowered_instructions++;

            if(!supported(op)) {
                jit_builder_exit_interpreter(&builder, add_state(ctx, pc, regs));
                ctx->stats->side_exits++;
                terminated = true;
                break;
            }
            if(op == RBC_NOP) continue;
            if(op == RBC_RETURN_NONE) {
                jit_builder_return(&builder, jit_builder_const_none(&builder));
                terminated = true;
                break;
            }
            if(op == RBC_JUMP) {
                const int32_t target = ctx->ip_to_block[branch_target(ctx, pc, op)];
                if(!fill_edge(ctx, bi, target, regs)) goto failure;
                jit_builder_jump(&builder, ctx->blocks[target].mir);
                terminated = true;
                break;
            }
            if(op == RBC_RETURN) {
                if(!register_valid(ctx, a)) goto invalid_reg;
                jit_builder_return(&builder, regs[a]);
                terminated = true;
                break;
            }
            if(op == RBC_MOVE) {
                if(!register_valid(ctx, a) || !register_valid(ctx, b)) goto invalid_reg;
                regs[a] = regs[b];
                continue;
            }
            if(op == RBC_LOAD_CONST) {
                if(!register_valid(ctx, a)) goto invalid_reg;
                const uint32_t index = RBC_BX(byte);
                if(ctx->source == NULL || index >= (uint32_t)ctx->source->consts.length) {
                    fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "invalid const-pool index %u", index);
                    goto failure;
                }
                JitImmediate imm = zero_imm();
                imm.u32 = index;
                regs[a] = jit_builder_emit_value(&builder, JIT_OP_CONST_POOL,
                                                 jit_type_value(), NULL, 0, imm, index);
                continue;
            }
            if(op == RBC_LOAD_NONE || op == RBC_LOAD_NIL || op == RBC_LOAD_ELLIPSIS ||
               op == RBC_LOAD_BOOL || op == RBC_LOAD_I16) {
                if(!register_valid(ctx, a)) goto invalid_reg;
                if(op == RBC_LOAD_NONE) regs[a] = jit_builder_const_none(&builder);
                else if(op == RBC_LOAD_NIL) regs[a] = jit_builder_const_nil(&builder);
                else if(op == RBC_LOAD_ELLIPSIS) regs[a] = jit_builder_const_ellipsis(&builder);
                else if(op == RBC_LOAD_BOOL) regs[a] = emit_boxed_bool(&builder, b != 0);
                else regs[a] = emit_boxed_int(&builder, RBC_SBX(byte));
                continue;
            }
            if(is_conditional(op)) {
                JitValueId condition;
                bool target_on_true;
                if(is_fused_branch(op)) {
                    if(!register_valid(ctx, a) ||
                       (op == RBC_CMP_BRANCH && !register_valid(ctx, b))) goto invalid_reg;
                    JitOpcode comparison = mapped_compare_code((uint8_t)c);
                    if(comparison == JIT_OP_NOP) {
                        fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "invalid fused comparison code");
                        goto failure;
                    }
                    JitValueId operands[2];
                    operands[0] = regs[a];
                    operands[1] = op == RBC_CMP_BRANCH ? regs[b]
                                                          : emit_boxed_int(&builder, (int8_t)b);
                    JitValueId result = jit_builder_emit_value(&builder, comparison,
                                                                 jit_type_value(), operands, 2,
                                                                 zero_imm(), 0);
                    attach_state_if_raising(ctx, result, pc, regs);
                    condition = jit_builder_emit_value(&builder, JIT_OP_TRUTHY,
                                  jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                  &result, 1, zero_imm(), 0);
                    attach_state_if_raising(ctx, condition, pc, regs);
                    target_on_true = (c & REG_CMP_BRANCH_TRUE) != 0;
                } else {
                    if(!register_valid(ctx, a)) goto invalid_reg;
                    condition = jit_builder_emit_value(&builder, JIT_OP_TRUTHY,
                                  jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                  &regs[a], 1, zero_imm(), 0);
                    attach_state_if_raising(ctx, condition, pc, regs);
                    target_on_true = op == RBC_JUMP_IF_TRUE || op == RBC_JUMP_IF_TRUE_OR_POP;
                }
                const int32_t target = ctx->ip_to_block[branch_target(ctx, pc, op)];
                const int32_t fall = ctx->ip_to_block[branch_fallthrough(ctx, pc, op)];
                if(!fill_edge(ctx, bi, target, regs) || !fill_edge(ctx, bi, fall, regs)) goto failure;
                jit_builder_branch(&builder, condition,
                    ctx->blocks[target_on_true ? target : fall].mir,
                    ctx->blocks[target_on_true ? fall : target].mir);
                terminated = true;
                break;
            }

            {
                const JitOpcode operation = mapped_binary_opcode(op);
                if(operation == JIT_OP_NOP) {
                    fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc,
                         "supported opcode lacks MIR lowering: %u", (unsigned)op);
                    goto failure;
                }
                if(!register_valid(ctx, a) || !register_valid(ctx, b)) goto invalid_reg;
                const bool immediate = op >= RBC_ADD_I8 && op <= RBC_CMP_GE_I8;
                if(!immediate && !register_valid(ctx, c)) goto invalid_reg;
                JitValueId operands[2] = {regs[b],
                    immediate ? emit_boxed_int(&builder, RBC_I8(byte)) : regs[c]};
                JitValueId result = jit_builder_emit_value(&builder, operation,
                                                            jit_type_value(), operands, 2,
                                                            zero_imm(), 0);
                attach_state_if_raising(ctx, result, pc, regs);
                regs[a] = result;
            }
            continue;

        invalid_reg:
            fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, pc, "register operand outside nregs");
        failure:
            free(regs);
            return false;
        }

        if(!terminated) {
            if(block->end >= ctx->word_count) {
                fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, block->last,
                     "fallthrough outside code range");
                free(regs);
                return false;
            }
            const int32_t target = ctx->ip_to_block[block->end];
            if(!fill_edge(ctx, bi, target, regs)) {
                free(regs);
                return false;
            }
            jit_builder_jump(&builder, ctx->blocks[target].mir);
        }
    }
    free(regs);
    return true;
}

static bool check_phi_inputs(RegMirContext* ctx) {
    for(int32_t bi = 0; bi < ctx->block_count; bi++) {
        const RegMirBlock* block = &ctx->blocks[bi];
        if(!block->reachable) continue;
        for(uint32_t r = 0; r < ctx->code->nregs; r++) {
            JitValueId phi = block->phis[r];
            const JitInst* inst = jit_region_inst_const(ctx->region,
                                                       jit_region_value_const(ctx->region, phi)->def);
            const JitValueId* inputs = jit_inst_operands(ctx->region, inst);
            for(uint32_t p = 0; p < inst->operands.count; p++) {
                if(inputs[p] == JIT_INVALID_VALUE)
                    return fail(ctx, JIT_REG_LOWER_INVALID_BYTECODE, block->start,
                                "unfilled PHI input for register %u", r);
            }
        }
    }
    return true;
}

static void cleanup(RegMirContext* ctx) {
    if(ctx->blocks != NULL) {
        for(int32_t i = 0; i < ctx->block_count; i++) {
            free(ctx->blocks[i].predecessors);
            free(ctx->blocks[i].phis);
        }
    }
    free(ctx->blocks);
    free(ctx->spans);
    free(ctx->leaders);
    free(ctx->ip_to_block);
}

void jit_reg_frontend_options_init(JitRegFrontendOptions* options) {
    if(options == NULL) return;
    options->estimated_execution_count = 10;
    options->max_phi_cells = 200000;
    options->poll_loop_headers = true;
    options->verify = true;
}

const char* jit_reg_lower_status_name(JitRegLowerStatus status) {
    switch(status) {
        case JIT_REG_LOWER_OK: return "ok";
        case JIT_REG_LOWER_INVALID_ARGUMENT: return "invalid argument";
        case JIT_REG_LOWER_INVALID_BYTECODE: return "invalid bytecode";
        case JIT_REG_LOWER_TOO_LARGE: return "SSA graph too large";
        case JIT_REG_LOWER_OUT_OF_MEMORY: return "out of memory";
        case JIT_REG_LOWER_VERIFY_FAILED: return "MIR verification failed";
    }
    return "unknown";
}

bool jit_lower_regcode_to_mir(const RegCode* code, const CodeObject* source,
                              const JitRegFrontendOptions* options_in,
                              JitRegion* out_region, JitRegFrontendStats* stats_out,
                              JitRegLowerError* error) {
    JitRegFrontendOptions defaults;
    jit_reg_frontend_options_init(&defaults);
    JitRegFrontendStats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    if(stats_out == NULL) stats_out = &local_stats;
    else memset(stats_out, 0, sizeof(*stats_out));
    if(error != NULL) {
        memset(error, 0, sizeof(*error));
        error->reg_pc = -1;
    }
    RegMirContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.code = code;
    ctx.source = source;
    ctx.options = options_in != NULL ? options_in : &defaults;
    ctx.stats = stats_out;
    ctx.error = error;
    ctx.region = out_region;

    if(code == NULL || out_region == NULL || code->code.length <= 0 || code->nregs == 0 ||
       ctx.options->max_phi_cells == 0)
        return fail(&ctx, JIT_REG_LOWER_INVALID_ARGUMENT, -1, "invalid RegCode or options");
    if(code->code.length > 65536 || code->nregs > 256)
        return fail(&ctx, JIT_REG_LOWER_TOO_LARGE, -1, "RegCode exceeds frontend safety limits");
    ctx.word_count = code->code.length;
    ctx.stats->bytecode_words = (uint32_t)ctx.word_count;
    ctx.spans = calloc((size_t)ctx.word_count, sizeof(uint8_t));
    ctx.leaders = calloc((size_t)ctx.word_count, sizeof(bool));
    ctx.ip_to_block = malloc((size_t)ctx.word_count * sizeof(int32_t));
    if(ctx.spans == NULL || ctx.leaders == NULL || ctx.ip_to_block == NULL) {
        fail(&ctx, JIT_REG_LOWER_OUT_OF_MEMORY, -1, "allocating bytecode maps");
        cleanup(&ctx);
        return false;
    }
    for(int32_t i = 0; i < ctx.word_count; i++) ctx.ip_to_block[i] = -1;
    bool ok = scan_instructions(&ctx) && build_blocks(&ctx) && create_mir(&ctx) &&
              lower_blocks(&ctx) && check_phi_inputs(&ctx);
    if(ok) {
        jit_region_rebuild_cfg(out_region);
        jit_region_recompute_uses(out_region);
        if(ctx.options->verify) {
            char message[256];
            if(!jit_region_verify(out_region, message, sizeof(message)))
                ok = fail(&ctx, JIT_REG_LOWER_VERIFY_FAILED, -1, "%s", message);
        }
    }
    if(!ok && ctx.initialized) jit_region_destroy(out_region);
    cleanup(&ctx);
    return ok;
}
