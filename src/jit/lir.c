#include "pocketpy/jit/lir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JitImmediate imm_zero(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
    if(alignment <= 1u) return value;
    uint32_t rem = value % alignment;
    if(rem == 0u) return value;
    return value + (alignment - rem);
}

static bool failf(char* error, uint32_t error_size, const char* fmt, ...) {
    if(error != NULL && error_size != 0u) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_size, fmt, args);
        va_end(args);
        error[error_size - 1u] = '\0';
    }
    return false;
}

JitTypeInfo jit_lir_type_void(void) {
    return jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN);
}

JitLirOperand jit_lir_none(void) {
    JitLirOperand op;
    memset(&op, 0, sizeof(op));
    op.kind = JIT_LIR_OPERAND_NONE;
    op.type = jit_lir_type_void();
    return op;
}

JitLirOperand jit_lir_vreg(JitVRegId id, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_VREG;
    op.type = type;
    op.as.vreg = id;
    return op;
}

JitLirOperand jit_lir_preg(JitPhysRegBank bank, uint16_t index, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_PREG;
    op.type = type;
    op.as.preg.bank = bank;
    op.as.preg.index = index;
    return op;
}

JitLirOperand jit_lir_stack(JitStackSlotId slot, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_STACK_SLOT;
    op.type = type;
    op.as.stack_slot = slot;
    return op;
}

JitLirOperand jit_lir_imm_i64(int64_t value, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_IMM_I64;
    op.type = type;
    op.as.i64 = value;
    return op;
}

JitLirOperand jit_lir_imm_f64(double value, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_IMM_F64;
    op.type = type;
    op.as.f64 = value;
    return op;
}

JitLirOperand jit_lir_ptr(uintptr_t value, JitTypeInfo type) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_PTR;
    op.type = type;
    op.as.ptr = value;
    return op;
}

JitLirOperand jit_lir_block_operand(JitLirBlockId block) {
    JitLirOperand op = jit_lir_none();
    op.kind = JIT_LIR_OPERAND_BLOCK;
    op.as.block = block;
    return op;
}

void jit_lir_init(JitLirFunction* lir, uint32_t tvalue_size, uint32_t stack_alignment) {
    memset(lir, 0, sizeof(*lir));
    c11_vector__ctor(&lir->blocks, sizeof(JitLirBlock));
    c11_vector__ctor(&lir->operands, sizeof(JitLirOperand));
    c11_vector__ctor(&lir->vregs, sizeof(JitVReg));
    c11_vector__ctor(&lir->stack_slots, sizeof(JitStackSlot));
    c11_vector__ctor(&lir->states, sizeof(JitLirState));
    c11_vector__ctor(&lir->state_operands, sizeof(JitLirOperand));
    c11_vector__ctor(&lir->edge_copies, sizeof(JitLirEdgeCopy));
    c11_vector__ctor(&lir->mir_value_map, sizeof(JitLirOperand));
    lir->entry_block = JIT_INVALID_LIR_BLOCK;
    lir->local_stack_size = 0;
    lir->tvalue_size = tvalue_size == 0u ? 32u : tvalue_size;
    lir->stack_alignment = stack_alignment == 0u ? 16u : stack_alignment;
}

void jit_lir_destroy(JitLirFunction* lir) {
    c11__foreach(JitLirBlock, &lir->blocks, block) {
        c11_vector__dtor(&block->insts);
        c11_vector__dtor(&block->predecessors);
        c11_vector__dtor(&block->successors);
    }
    c11__foreach(JitLirEdgeCopy, &lir->edge_copies, edge) {
        c11_vector__dtor(&edge->dsts);
        c11_vector__dtor(&edge->srcs);
    }
    c11_vector__dtor(&lir->blocks);
    c11_vector__dtor(&lir->operands);
    c11_vector__dtor(&lir->vregs);
    c11_vector__dtor(&lir->stack_slots);
    c11_vector__dtor(&lir->states);
    c11_vector__dtor(&lir->state_operands);
    c11_vector__dtor(&lir->edge_copies);
    c11_vector__dtor(&lir->mir_value_map);
    memset(lir, 0, sizeof(*lir));
}

JitLirBlockId jit_lir_add_block(JitLirFunction* lir,
                                JitBlockId mir_block,
                                uint32_t frequency,
                                uint32_t flags) {
    JitLirBlockId id = (JitLirBlockId)lir->blocks.length;
    JitLirBlock* block = c11_vector__emplace(&lir->blocks);
    memset(block, 0, sizeof(*block));
    block->id = id;
    block->mir_block = mir_block;
    block->frequency = frequency == 0u ? 1u : frequency;
    block->flags = flags;
    c11_vector__ctor(&block->insts, sizeof(JitLirInst));
    c11_vector__ctor(&block->predecessors, sizeof(JitLirBlockId));
    c11_vector__ctor(&block->successors, sizeof(JitLirBlockId));
    return id;
}

JitLirBlock* jit_lir_block(JitLirFunction* lir, JitLirBlockId id) {
    if(id >= (JitLirBlockId)lir->blocks.length) return NULL;
    return c11__at(JitLirBlock, &lir->blocks, id);
}

const JitLirBlock* jit_lir_block_const(const JitLirFunction* lir, JitLirBlockId id) {
    if(id >= (JitLirBlockId)lir->blocks.length) return NULL;
    return c11__at(JitLirBlock, &lir->blocks, id);
}

JitVRegId jit_lir_new_vreg(JitLirFunction* lir,
                           JitVRegClass reg_class,
                           JitTypeInfo type,
                           JitValueId mir_value) {
    JitVRegId id = (JitVRegId)lir->vregs.length;
    JitVReg value;
    value.reg_class = reg_class;
    value.type = type;
    value.mir_value = mir_value;
    value.spill_weight = 0.0;
    value.force_stack = reg_class == JIT_VREG_VALUE;
    c11_vector__push(JitVReg, &lir->vregs, value);
    return id;
}

JitStackSlotId jit_lir_new_stack_slot(JitLirFunction* lir,
                                      uint32_t size,
                                      uint32_t alignment,
                                      JitStackSlotKind kind) {
    JitStackSlotId id = (JitStackSlotId)lir->stack_slots.length;
    JitStackSlot slot;
    slot.size = size == 0u ? 8u : size;
    slot.alignment = alignment == 0u ? 1u : alignment;
    slot.offset = 0u;
    slot.kind = kind;
    c11_vector__push(JitStackSlot, &lir->stack_slots, slot);
    return id;
}

JitLirOperandRange jit_lir_append_operands(JitLirFunction* lir,
                                           const JitLirOperand* operands,
                                           uint32_t count) {
    JitLirOperandRange range;
    range.offset = (uint32_t)lir->operands.length;
    range.count = count;
    if(count != 0u) c11_vector__extend(&lir->operands, operands, (int)count);
    return range;
}

const JitLirOperand* jit_lir_inst_defs(const JitLirFunction* lir, const JitLirInst* inst) {
    if(inst->defs.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->operands, inst->defs.offset);
}

const JitLirOperand* jit_lir_inst_uses(const JitLirFunction* lir, const JitLirInst* inst) {
    if(inst->uses.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->operands, inst->uses.offset);
}

JitLirOperand* jit_lir_inst_defs_mut(JitLirFunction* lir, JitLirInst* inst) {
    if(inst->defs.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->operands, inst->defs.offset);
}

JitLirOperand* jit_lir_inst_uses_mut(JitLirFunction* lir, JitLirInst* inst) {
    if(inst->uses.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->operands, inst->uses.offset);
}

void jit_lir_emit(JitLirFunction* lir,
                  JitLirBlockId block_id,
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
                  JitSourceLocation source) {
    JitLirBlock* block = jit_lir_block(lir, block_id);
    if(block == NULL) return;
    JitLirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op;
    inst.defs = jit_lir_append_operands(lir, defs, def_count);
    inst.uses = jit_lir_append_operands(lir, uses, use_count);
    inst.imm = imm;
    inst.aux = aux;
    inst.aux2 = aux2;
    inst.aux3 = aux3;
    inst.flags = flags;
    inst.target = target;
    inst.target2 = target2;
    inst.state = state;
    inst.source = source;
    inst.removed = false;
    c11_vector__push(JitLirInst, &block->insts, inst);
}

JitLirStateId jit_lir_add_state(JitLirFunction* lir,
                                int32_t bytecode_ip,
                                const JitLirOperand* locals,
                                uint32_t locals_count,
                                const JitLirOperand* stack,
                                uint32_t stack_count) {
    JitLirStateId id = (JitLirStateId)lir->states.length;
    JitLirState state;
    state.bytecode_ip = bytecode_ip;
    state.locals.offset = (uint32_t)lir->state_operands.length;
    state.locals.count = locals_count;
    if(locals_count != 0u) c11_vector__extend(&lir->state_operands, locals, (int)locals_count);
    state.stack.offset = (uint32_t)lir->state_operands.length;
    state.stack.count = stack_count;
    if(stack_count != 0u) c11_vector__extend(&lir->state_operands, stack, (int)stack_count);
    c11_vector__push(JitLirState, &lir->states, state);
    return id;
}

const JitLirOperand* jit_lir_state_locals(const JitLirFunction* lir, const JitLirState* state) {
    if(state->locals.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->state_operands, state->locals.offset);
}

const JitLirOperand* jit_lir_state_stack(const JitLirFunction* lir, const JitLirState* state) {
    if(state->stack.count == 0u) return NULL;
    return c11__at(JitLirOperand, &lir->state_operands, state->stack.offset);
}

JitLirEdgeCopy* jit_lir_get_edge_copy(JitLirFunction* lir,
                                      JitLirBlockId from,
                                      JitLirBlockId to,
                                      bool create) {
    c11__foreach(JitLirEdgeCopy, &lir->edge_copies, edge) {
        if(edge->from == from && edge->to == to) return edge;
    }
    if(!create) return NULL;
    JitLirEdgeCopy* edge = c11_vector__emplace(&lir->edge_copies);
    memset(edge, 0, sizeof(*edge));
    edge->from = from;
    edge->to = to;
    c11_vector__ctor(&edge->dsts, sizeof(JitLirOperand));
    c11_vector__ctor(&edge->srcs, sizeof(JitLirOperand));
    return edge;
}

bool jit_lir_add_edge_copy(JitLirFunction* lir,
                           JitLirBlockId from,
                           JitLirBlockId to,
                           JitLirOperand dst,
                           JitLirOperand src) {
    if(dst.kind != JIT_LIR_OPERAND_VREG) return false;
    JitLirEdgeCopy* edge = jit_lir_get_edge_copy(lir, from, to, true);
    if(edge == NULL) return false;
    c11_vector__push(JitLirOperand, &edge->dsts, dst);
    c11_vector__push(JitLirOperand, &edge->srcs, src);
    return true;
}

static bool operand_same_location(JitLirOperand a, JitLirOperand b) {
    if(a.kind != b.kind) return false;
    switch(a.kind) {
        case JIT_LIR_OPERAND_NONE: return true;
        case JIT_LIR_OPERAND_VREG: return a.as.vreg == b.as.vreg;
        case JIT_LIR_OPERAND_PREG:
            return a.as.preg.bank == b.as.preg.bank && a.as.preg.index == b.as.preg.index;
        case JIT_LIR_OPERAND_STACK_SLOT: return a.as.stack_slot == b.as.stack_slot;
        case JIT_LIR_OPERAND_IMM_I64: return a.as.i64 == b.as.i64;
        case JIT_LIR_OPERAND_IMM_F64:
            return memcmp(&a.as.f64, &b.as.f64, sizeof(double)) == 0;
        case JIT_LIR_OPERAND_PTR: return a.as.ptr == b.as.ptr;
        case JIT_LIR_OPERAND_BLOCK: return a.as.block == b.as.block;
    }
    return false;
}

static void add_unique_block(c11_vector* vec, JitLirBlockId id) {
    c11__foreach(JitLirBlockId, vec, current) {
        if(*current == id) return;
    }
    c11_vector__push(JitLirBlockId, vec, id);
}

void jit_lir_rebuild_cfg(JitLirFunction* lir) {
    c11__foreach(JitLirBlock, &lir->blocks, block) {
        c11_vector__clear(&block->predecessors);
        c11_vector__clear(&block->successors);
    }

    c11__foreach(JitLirBlock, &lir->blocks, block) {
        const JitLirInst* term = NULL;
        for(int i = block->insts.length - 1; i >= 0; i--) {
            const JitLirInst* inst = c11__at(JitLirInst, &block->insts, i);
            if(inst->removed || inst->op == JIT_LIR_NOP) continue;
            term = inst;
            break;
        }
        if(term == NULL) continue;
        if(term->op == JIT_LIR_JUMP) {
            if(term->target < (JitLirBlockId)lir->blocks.length)
                add_unique_block(&block->successors, term->target);
        } else if(term->op == JIT_LIR_BRANCH) {
            if(term->target < (JitLirBlockId)lir->blocks.length)
                add_unique_block(&block->successors, term->target);
            if(term->target2 < (JitLirBlockId)lir->blocks.length)
                add_unique_block(&block->successors, term->target2);
        }
    }

    c11__foreach(JitLirBlock, &lir->blocks, block) {
        c11__foreach(JitLirBlockId, &block->successors, succ_id) {
            JitLirBlock* succ = jit_lir_block(lir, *succ_id);
            if(succ != NULL) add_unique_block(&succ->predecessors, block->id);
        }
    }
}

static JitLirInst make_copy_inst(JitLirFunction* lir,
                                 JitLirOperand dst,
                                 JitLirOperand src,
                                 JitSourceLocation source) {
    JitLirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = JIT_LIR_COPY;
    inst.defs = jit_lir_append_operands(lir, &dst, 1);
    inst.uses = jit_lir_append_operands(lir, &src, 1);
    inst.target = JIT_INVALID_LIR_BLOCK;
    inst.target2 = JIT_INVALID_LIR_BLOCK;
    inst.state = JIT_INVALID_LIR_STATE;
    inst.source = source;
    return inst;
}

static bool src_mentions_dst(const JitLirOperand* srcs,
                             const JitLirOperand* dsts,
                             const bool* pending,
                             uint32_t count,
                             uint32_t candidate) {
    JitLirOperand dst = dsts[candidate];
    if(dst.kind != JIT_LIR_OPERAND_VREG) return false;
    for(uint32_t i = 0; i < count; i++) {
        if(!pending[i] || i == candidate) continue;
        if(srcs[i].kind == JIT_LIR_OPERAND_VREG && srcs[i].as.vreg == dst.as.vreg) return true;
    }
    return false;
}

static bool build_parallel_copy_sequence(JitLirFunction* lir,
                                         const JitLirEdgeCopy* edge,
                                         c11_vector* out,
                                         char* error,
                                         uint32_t error_size) {
    uint32_t count = (uint32_t)edge->dsts.length;
    if(count != (uint32_t)edge->srcs.length)
        return failf(error, error_size, "parallel-copy destination/source count mismatch");
    if(count == 0u) return true;

    JitLirOperand* dsts = malloc(sizeof(JitLirOperand) * count);
    JitLirOperand* srcs = malloc(sizeof(JitLirOperand) * count);
    bool* pending = malloc(sizeof(bool) * count);
    if(dsts == NULL || srcs == NULL || pending == NULL) {
        free(dsts);
        free(srcs);
        free(pending);
        return failf(error, error_size, "out of memory resolving parallel copies");
    }
    memcpy(dsts, edge->dsts.data, sizeof(JitLirOperand) * count);
    memcpy(srcs, edge->srcs.data, sizeof(JitLirOperand) * count);

    uint32_t remaining = 0;
    for(uint32_t i = 0; i < count; i++) {
        pending[i] = !operand_same_location(dsts[i], srcs[i]);
        if(pending[i]) remaining++;
    }

    JitSourceLocation source = {-1, -1};
    while(remaining != 0u) {
        bool progress = false;
        for(uint32_t i = 0; i < count; i++) {
            if(!pending[i]) continue;
            if(src_mentions_dst(srcs, dsts, pending, count, i)) continue;
            JitLirInst inst = make_copy_inst(lir, dsts[i], srcs[i], source);
            c11_vector__push(JitLirInst, out, inst);
            pending[i] = false;
            remaining--;
            progress = true;
        }
        if(progress) continue;

        uint32_t cycle = 0;
        while(cycle < count && !pending[cycle]) cycle++;
        if(cycle == count) break;
        if(dsts[cycle].kind != JIT_LIR_OPERAND_VREG)
            goto invalid_cycle;
        JitVRegId dst_id = dsts[cycle].as.vreg;
        if(dst_id >= (JitVRegId)lir->vregs.length) goto invalid_cycle;
        JitVReg* dst_vreg = c11__at(JitVReg, &lir->vregs, dst_id);
        JitVRegId temp_id = jit_lir_new_vreg(lir,
                                             dst_vreg->reg_class,
                                             dst_vreg->type,
                                             JIT_INVALID_VALUE);
        JitLirOperand temp = jit_lir_vreg(temp_id, dst_vreg->type);
        JitLirInst save = make_copy_inst(lir, temp, dsts[cycle], source);
        c11_vector__push(JitLirInst, out, save);
        for(uint32_t i = 0; i < count; i++) {
            if(!pending[i]) continue;
            if(srcs[i].kind == JIT_LIR_OPERAND_VREG && srcs[i].as.vreg == dst_id) srcs[i] = temp;
        }
    }

    free(dsts);
    free(srcs);
    free(pending);
    return true;

invalid_cycle:
    free(dsts);
    free(srcs);
    free(pending);
    return failf(error, error_size, "invalid parallel-copy cycle");
}

static int block_terminator_index(const JitLirBlock* block) {
    for(int i = block->insts.length - 1; i >= 0; i--) {
        const JitLirInst* inst = c11__at(JitLirInst, &block->insts, i);
        if(inst->removed || inst->op == JIT_LIR_NOP) continue;
        switch(inst->op) {
            case JIT_LIR_JUMP:
            case JIT_LIR_BRANCH:
            case JIT_LIR_RETURN:
            case JIT_LIR_YIELD:
            case JIT_LIR_EXIT_INTERPRETER:
            case JIT_LIR_ERROR: return i;
            default: return block->insts.length;
        }
    }
    return block->insts.length;
}

static void vector_insert_inst(c11_vector* vec, int at, JitLirInst inst) {
    if(vec->length == vec->capacity) c11_vector__reserve(vec, c11_vector__nextcap(vec));
    JitLirInst* data = (JitLirInst*)vec->data;
    memmove(data + at + 1, data + at, (size_t)(vec->length - at) * sizeof(JitLirInst));
    data[at] = inst;
    vec->length++;
}

static void insert_sequence(JitLirBlock* block, int at, const c11_vector* sequence) {
    c11__foreach(JitLirInst, sequence, inst) {
        vector_insert_inst(&block->insts, at, *inst);
        at++;
    }
}

static bool patch_edge_target(JitLirBlock* from, JitLirBlockId old_target, JitLirBlockId new_target) {
    for(int i = from->insts.length - 1; i >= 0; i--) {
        JitLirInst* inst = c11__at(JitLirInst, &from->insts, i);
        if(inst->removed || inst->op == JIT_LIR_NOP) continue;
        if(inst->op == JIT_LIR_JUMP) {
            if(inst->target != old_target) return false;
            inst->target = new_target;
            return true;
        }
        if(inst->op == JIT_LIR_BRANCH) {
            bool found = false;
            if(inst->target == old_target) {
                inst->target = new_target;
                found = true;
            }
            if(inst->target2 == old_target) {
                inst->target2 = new_target;
                found = true;
            }
            return found;
        }
        return false;
    }
    return false;
}

bool jit_lir_resolve_parallel_copies(JitLirFunction* lir,
                                     char* error,
                                     uint32_t error_size) {
    jit_lir_rebuild_cfg(lir);

    for(int edge_index = 0; edge_index < lir->edge_copies.length; edge_index++) {
        JitLirEdgeCopy* edge = c11__at(JitLirEdgeCopy, &lir->edge_copies, edge_index);
        if(edge->dsts.length == 0) continue;
        JitLirBlock* from = jit_lir_block(lir, edge->from);
        JitLirBlock* to = jit_lir_block(lir, edge->to);
        if(from == NULL || to == NULL)
            return failf(error, error_size, "parallel copy references invalid edge b%u -> b%u",
                         edge->from,
                         edge->to);

        c11_vector sequence;
        c11_vector__ctor(&sequence, sizeof(JitLirInst));
        if(!build_parallel_copy_sequence(lir, edge, &sequence, error, error_size)) {
            c11_vector__dtor(&sequence);
            return false;
        }

        if(from->successors.length == 1) {
            insert_sequence(from, block_terminator_index(from), &sequence);
        } else if(to->predecessors.length == 1) {
            insert_sequence(to, 0, &sequence);
        } else {
            uint32_t flags = JIT_LIR_BLOCK_SYNTHETIC;
            if((from->flags & JIT_LIR_BLOCK_COLD) != 0u || (to->flags & JIT_LIR_BLOCK_COLD) != 0u)
                flags |= JIT_LIR_BLOCK_COLD;
            uint32_t freq = from->frequency < to->frequency ? from->frequency : to->frequency;
            JitLirBlockId split = jit_lir_add_block(lir, JIT_INVALID_BLOCK, freq, flags);
            from = jit_lir_block(lir, edge->from);
            to = jit_lir_block(lir, edge->to);
            JitLirBlock* middle = jit_lir_block(lir, split);
            if(from == NULL || to == NULL || middle == NULL || !patch_edge_target(from, edge->to, split)) {
                c11_vector__dtor(&sequence);
                return failf(error, error_size, "failed to split critical edge b%u -> b%u",
                             edge->from,
                             edge->to);
            }
            insert_sequence(middle, 0, &sequence);
            jit_lir_emit(lir,
                         split,
                         JIT_LIR_JUMP,
                         NULL,
                         0,
                         NULL,
                         0,
                         imm_zero(),
                         0,
                         0,
                         0,
                         JIT_LIR_FLAG_NONE,
                         edge->to,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         (JitSourceLocation){-1, -1});
            jit_lir_rebuild_cfg(lir);
        }
        c11_vector__dtor(&sequence);
    }

    c11__foreach(JitLirEdgeCopy, &lir->edge_copies, edge) {
        c11_vector__dtor(&edge->dsts);
        c11_vector__dtor(&edge->srcs);
    }
    c11_vector__clear(&lir->edge_copies);
    jit_lir_rebuild_cfg(lir);
    if(error != NULL && error_size != 0u) error[0] = '\0';
    return true;
}

static bool is_terminator(JitLirOpcode op) {
    switch(op) {
        case JIT_LIR_JUMP:
        case JIT_LIR_BRANCH:
        case JIT_LIR_RETURN:
        case JIT_LIR_YIELD:
        case JIT_LIR_EXIT_INTERPRETER:
        case JIT_LIR_ERROR: return true;
        default: return false;
    }
}

static bool verify_operand(const JitLirFunction* lir,
                           JitLirOperand op,
                           bool is_def,
                           char* error,
                           uint32_t error_size) {
    switch(op.kind) {
        case JIT_LIR_OPERAND_NONE: return !is_def;
        case JIT_LIR_OPERAND_VREG:
            if(op.as.vreg >= (JitVRegId)lir->vregs.length)
                return failf(error, error_size, "invalid virtual register v%u", op.as.vreg);
            return true;
        case JIT_LIR_OPERAND_PREG:
            return op.as.preg.bank != JIT_PHYS_NONE;
        case JIT_LIR_OPERAND_STACK_SLOT:
            if(op.as.stack_slot >= (JitStackSlotId)lir->stack_slots.length)
                return failf(error, error_size, "invalid stack slot s%u", op.as.stack_slot);
            return true;
        case JIT_LIR_OPERAND_IMM_I64:
        case JIT_LIR_OPERAND_IMM_F64:
        case JIT_LIR_OPERAND_PTR:
        case JIT_LIR_OPERAND_BLOCK:
            if(is_def) return failf(error, error_size, "immediate/block used as LIR definition");
            return true;
    }
    return failf(error, error_size, "unknown LIR operand kind");
}

bool jit_lir_verify(const JitLirFunction* lir, char* error, uint32_t error_size) {
    if(lir == NULL) return failf(error, error_size, "NULL LIR function");
    if(lir->entry_block >= (JitLirBlockId)lir->blocks.length)
        return failf(error, error_size, "invalid LIR entry block");

    c11__foreach(JitLirBlock, &lir->blocks, block) {
        if(block->id >= (JitLirBlockId)lir->blocks.length)
            return failf(error, error_size, "invalid LIR block id");
        bool saw_terminator = false;
        c11__foreach(JitLirInst, &block->insts, inst) {
            if(inst->removed || inst->op == JIT_LIR_NOP) continue;
            if(saw_terminator)
                return failf(error, error_size, "instruction follows terminator in b%u", block->id);
            const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
            const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);
            for(uint32_t i = 0; i < inst->defs.count; i++) {
                if(!verify_operand(lir, defs[i], true, error, error_size)) return false;
            }
            for(uint32_t i = 0; i < inst->uses.count; i++) {
                if(!verify_operand(lir, uses[i], false, error, error_size)) return false;
            }
            if(inst->target != JIT_INVALID_LIR_BLOCK && inst->target >= (JitLirBlockId)lir->blocks.length)
                return failf(error, error_size, "invalid branch target in b%u", block->id);
            if(inst->target2 != JIT_INVALID_LIR_BLOCK && inst->target2 >= (JitLirBlockId)lir->blocks.length)
                return failf(error, error_size, "invalid second branch target in b%u", block->id);
            if(inst->state != JIT_INVALID_LIR_STATE && inst->state >= (JitLirStateId)lir->states.length)
                return failf(error, error_size, "invalid LIR state id");
            if(is_terminator(inst->op)) saw_terminator = true;
        }
        if(block->insts.length != 0 && !saw_terminator)
            return failf(error, error_size, "LIR block b%u has no terminator", block->id);
    }

    c11__foreach(JitLirState, &lir->states, state) {
        const JitLirOperand* locals = jit_lir_state_locals(lir, state);
        const JitLirOperand* stack = jit_lir_state_stack(lir, state);
        for(uint32_t i = 0; i < state->locals.count; i++) {
            if(!verify_operand(lir, locals[i], false, error, error_size)) return false;
        }
        for(uint32_t i = 0; i < state->stack.count; i++) {
            if(!verify_operand(lir, stack[i], false, error, error_size)) return false;
        }
    }

    if(error != NULL && error_size != 0u) error[0] = '\0';
    return true;
}

bool jit_lir_peephole(JitLirFunction* lir) {
    bool changed = false;
    c11__foreach(JitLirBlock, &lir->blocks, block) {
        c11__foreach(JitLirInst, &block->insts, inst) {
            if(inst->removed || inst->op == JIT_LIR_NOP) continue;
            if(inst->op == JIT_LIR_COPY && inst->defs.count == 1u && inst->uses.count == 1u) {
                JitLirOperand* defs = jit_lir_inst_defs_mut(lir, inst);
                JitLirOperand* uses = jit_lir_inst_uses_mut(lir, inst);
                if(operand_same_location(defs[0], uses[0])) {
                    inst->removed = true;
                    changed = true;
                }
            }
            if(inst->op == JIT_LIR_JUMP && inst->target == block->id) {
                /* Keep intentional self-loops. */
                continue;
            }
        }
    }
    return changed;
}

void jit_lir_layout_stack(JitLirFunction* lir) {
    uint32_t offset = 0u;
    c11__foreach(JitStackSlot, &lir->stack_slots, slot) {
        offset = align_up_u32(offset, slot->alignment);
        slot->offset = offset;
        offset += slot->size;
    }
    lir->local_stack_size = align_up_u32(offset, lir->stack_alignment);
}

const char* jit_lir_opcode_name(JitLirOpcode op) {
#define CASE(x) case JIT_LIR_##x: return #x
    switch(op) {
        CASE(NOP);
        CASE(INPUT);
        CASE(VALUE_INPUT);
        CASE(CONST_I64);
        CASE(CONST_F64);
        CASE(CONST_VALUE);
        CASE(COPY);
        CASE(SPILL_LOAD);
        CASE(SPILL_STORE);
        CASE(BOX_I64);
        CASE(BOX_F64);
        CASE(BOX_BOOL);
        CASE(UNBOX_I64);
        CASE(UNBOX_F64);
        CASE(UNBOX_BOOL);
        CASE(IS_TYPE);
        CASE(IS_IDENTICAL);
        CASE(NOT_BOOL);
        CASE(ADD_I64);
        CASE(SUB_I64);
        CASE(MUL_I64);
        CASE(DIV_I64);
        CASE(MOD_I64);
        CASE(AND_I64);
        CASE(OR_I64);
        CASE(XOR_I64);
        CASE(SHL_I64);
        CASE(SHR_I64);
        CASE(NEG_I64);
        CASE(NOT_I64);
        CASE(ADD_F64);
        CASE(SUB_F64);
        CASE(MUL_F64);
        CASE(DIV_F64);
        CASE(NEG_F64);
        CASE(CMP_I64);
        CASE(CMP_F64);
        CASE(RUNTIME);
        CASE(CALL);
        CASE(SYNC_STATE);
        CASE(SAFEPOINT);
        CASE(POLL);
        CASE(JUMP);
        CASE(BRANCH);
        CASE(RETURN);
        CASE(YIELD);
        CASE(EXIT_INTERPRETER);
        CASE(ERROR);
        CASE(_COUNT);
    }
#undef CASE
    return "UNKNOWN";
}

const char* jit_vreg_class_name(JitVRegClass reg_class) {
    switch(reg_class) {
        case JIT_VREG_NONE: return "none";
        case JIT_VREG_GPR: return "gpr";
        case JIT_VREG_FPR: return "fpr";
        case JIT_VREG_VALUE: return "value";
    }
    return "unknown";
}

static void default_writer(const char* text, void* ctx) {
    (void)ctx;
    fputs(text, stdout);
}

static void dump_operand(const JitLirFunction* lir, JitLirOperand op, char* buffer, size_t size) {
    switch(op.kind) {
        case JIT_LIR_OPERAND_NONE: snprintf(buffer, size, "-"); break;
        case JIT_LIR_OPERAND_VREG: snprintf(buffer, size, "v%u", op.as.vreg); break;
        case JIT_LIR_OPERAND_PREG:
            snprintf(buffer, size, "p%d:%u", (int)op.as.preg.bank, (unsigned)op.as.preg.index);
            break;
        case JIT_LIR_OPERAND_STACK_SLOT:
            if(op.as.stack_slot < (JitStackSlotId)lir->stack_slots.length) {
                const JitStackSlot* slot = c11__at(JitStackSlot, &lir->stack_slots, op.as.stack_slot);
                snprintf(buffer, size, "s%u(+%u)", op.as.stack_slot, slot->offset);
            } else {
                snprintf(buffer, size, "s%u", op.as.stack_slot);
            }
            break;
        case JIT_LIR_OPERAND_IMM_I64: snprintf(buffer, size, "%lld", (long long)op.as.i64); break;
        case JIT_LIR_OPERAND_IMM_F64: snprintf(buffer, size, "%g", op.as.f64); break;
        case JIT_LIR_OPERAND_PTR: snprintf(buffer, size, "ptr(%p)", (void*)op.as.ptr); break;
        case JIT_LIR_OPERAND_BLOCK: snprintf(buffer, size, "b%u", op.as.block); break;
    }
}

void jit_lir_dump(const JitLirFunction* lir, void (*write)(const char*, void*), void* ctx) {
    if(write == NULL) write = default_writer;
    char line[1024];
    snprintf(line,
             sizeof(line),
             "LIR blocks=%d vregs=%d stack=%u\n",
             lir->blocks.length,
             lir->vregs.length,
             lir->local_stack_size);
    write(line, ctx);
    c11__foreach(JitLirBlock, &lir->blocks, block) {
        snprintf(line,
                 sizeof(line),
                 "b%u [freq=%u%s]:\n",
                 block->id,
                 block->frequency,
                 (block->flags & JIT_LIR_BLOCK_COLD) != 0u ? ", cold" : "");
        write(line, ctx);
        c11__foreach(JitLirInst, &block->insts, inst) {
            if(inst->removed) continue;
            snprintf(line, sizeof(line), "  %-18s", jit_lir_opcode_name(inst->op));
            write(line, ctx);
            const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
            const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);
            if(inst->defs.count != 0u) {
                write(" ", ctx);
                for(uint32_t i = 0; i < inst->defs.count; i++) {
                    char opbuf[96];
                    dump_operand(lir, defs[i], opbuf, sizeof(opbuf));
                    if(i != 0u) write(", ", ctx);
                    write(opbuf, ctx);
                }
                write(" <-", ctx);
            }
            if(inst->uses.count != 0u) {
                write(" ", ctx);
                for(uint32_t i = 0; i < inst->uses.count; i++) {
                    char opbuf[96];
                    dump_operand(lir, uses[i], opbuf, sizeof(opbuf));
                    if(i != 0u) write(", ", ctx);
                    write(opbuf, ctx);
                }
            }
            if(inst->op == JIT_LIR_JUMP) {
                snprintf(line, sizeof(line), " -> b%u", inst->target);
                write(line, ctx);
            } else if(inst->op == JIT_LIR_BRANCH) {
                snprintf(line, sizeof(line), " -> b%u, b%u", inst->target, inst->target2);
                write(line, ctx);
            }
            if(inst->state != JIT_INVALID_LIR_STATE) {
                snprintf(line, sizeof(line), " [state=%u]", inst->state);
                write(line, ctx);
            }
            write("\n", ctx);
        }
    }
}
