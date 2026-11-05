#include "tinypy/jit/ir.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static JitImmediate jit_imm_zero(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

JitTypeInfo jit_type_value(void) {
    JitTypeInfo type = {JIT_TYPE_VALUE, JIT_KNOWN_UNKNOWN, false, true};
    return type;
}

JitTypeInfo jit_type_exact(JitType representation, JitKnownType known_type) {
    JitTypeInfo type = {representation, known_type, true,
                            known_type == JIT_KNOWN_NONE || known_type == JIT_KNOWN_NIL};
    return type;
}

JitTypeInfo jit_type_unknown(JitType representation) {
    JitTypeInfo type = {representation, JIT_KNOWN_UNKNOWN, false, true};
    return type;
}

bool jit_type_equal(JitTypeInfo a, JitTypeInfo b) {
    return a.representation == b.representation && a.known_type == b.known_type &&
           a.exact == b.exact && a.nullable == b.nullable;
}

uint32_t jit_opcode_effects(JitOpcode op) {
    switch(op) {
        case JIT_OP_NOP:
        case JIT_OP_PHI:
        case JIT_OP_VM:
        case JIT_OP_FRAME:
        case JIT_OP_LOCAL:
        case JIT_OP_STACK_VALUE:
        case JIT_OP_CONST_NONE:
        case JIT_OP_CONST_NIL:
        case JIT_OP_CONST_ELLIPSIS:
        case JIT_OP_CONST_BOOL:
        case JIT_OP_CONST_I64:
        case JIT_OP_CONST_F64:
        case JIT_OP_CONST_POOL:
        case JIT_OP_BOX_I64:
        case JIT_OP_BOX_F64:
        case JIT_OP_BOX_BOOL:
        case JIT_OP_UNBOX_I64:
        case JIT_OP_UNBOX_F64:
        case JIT_OP_UNBOX_BOOL:
        case JIT_OP_IS_TYPE:
        case JIT_OP_EXTRACT_TOKEN:
        case JIT_OP_IS_IDENTICAL:
        case JIT_OP_NOT_BOOL:
        case JIT_OP_ADD_I64:
        case JIT_OP_SUB_I64:
        case JIT_OP_MUL_I64:
        case JIT_OP_DIV_I64:
        case JIT_OP_MOD_I64:
        case JIT_OP_AND_I64:
        case JIT_OP_OR_I64:
        case JIT_OP_XOR_I64:
        case JIT_OP_SHL_I64:
        case JIT_OP_SHR_I64:
        case JIT_OP_NEG_I64:
        case JIT_OP_NOT_I64:
        case JIT_OP_ADD_F64:
        case JIT_OP_SUB_F64:
        case JIT_OP_MUL_F64:
        case JIT_OP_DIV_F64:
        case JIT_OP_NEG_F64:
        case JIT_OP_EQ_I64:
        case JIT_OP_NE_I64:
        case JIT_OP_LT_I64:
        case JIT_OP_LE_I64:
        case JIT_OP_GT_I64:
        case JIT_OP_GE_I64:
        case JIT_OP_EQ_F64:
        case JIT_OP_NE_F64:
        case JIT_OP_LT_F64:
        case JIT_OP_LE_F64:
        case JIT_OP_GT_F64:
        case JIT_OP_GE_F64:
            return JIT_EFFECT_NONE;

        case JIT_OP_LOAD_LOCAL:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_RAISE;
        case JIT_OP_STORE_LOCAL:
            return JIT_EFFECT_WRITE_RUNTIME;

        case JIT_OP_LOAD_GLOBAL:
        case JIT_OP_LOAD_NONLOCAL:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_READ_HEAP | JIT_EFFECT_RAISE;
        case JIT_OP_STORE_GLOBAL:
            return JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_WRITE_HEAP | JIT_EFFECT_RAISE;

        case JIT_OP_ADD_VALUE:
        case JIT_OP_SUB_VALUE:
        case JIT_OP_MUL_VALUE:
        case JIT_OP_TRUEDIV_VALUE:
        case JIT_OP_FLOORDIV_VALUE:
        case JIT_OP_MOD_VALUE:
        case JIT_OP_POW_VALUE:
        case JIT_OP_EQ_VALUE:
        case JIT_OP_NE_VALUE:
        case JIT_OP_LT_VALUE:
        case JIT_OP_LE_VALUE:
        case JIT_OP_GT_VALUE:
        case JIT_OP_GE_VALUE:
        case JIT_OP_TRUTHY:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_READ_HEAP | JIT_EFFECT_CALL |
                   JIT_EFFECT_RAISE;

        case JIT_OP_LOAD_ATTR:
        case JIT_OP_LOAD_METHOD:
        case JIT_OP_LOAD_SUBSCR:
        case JIT_OP_GET_ITER:
        case JIT_OP_ITER_NEXT:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_READ_HEAP | JIT_EFFECT_CALL |
                   JIT_EFFECT_RAISE;

        case JIT_OP_STORE_ATTR:
        case JIT_OP_STORE_SUBSCR:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_READ_HEAP |
                   JIT_EFFECT_WRITE_HEAP | JIT_EFFECT_CALL | JIT_EFFECT_RAISE;

        case JIT_OP_CALL:
        case JIT_OP_CALL_NATIVE:
        case JIT_OP_CALL_HELPER:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_READ_HEAP |
                   JIT_EFFECT_WRITE_HEAP | JIT_EFFECT_ALLOCATE | JIT_EFFECT_CALL |
                   JIT_EFFECT_RAISE | JIT_EFFECT_BARRIER;

        case JIT_OP_BUILD_TUPLE:
        case JIT_OP_BUILD_LIST:
        case JIT_OP_BUILD_DICT:
        case JIT_OP_BUILD_SET:
            return JIT_EFFECT_WRITE_HEAP | JIT_EFFECT_ALLOCATE | JIT_EFFECT_RAISE |
                   JIT_EFFECT_BARRIER;

        case JIT_OP_JUMP:
        case JIT_OP_BRANCH:
        case JIT_OP_SWITCH:
        case JIT_OP_RETURN:
            return JIT_EFFECT_NONE;

        case JIT_OP_SYNC_STATE:
            return JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_BARRIER;
        case JIT_OP_SAFEPOINT:
        case JIT_OP_POLL:
            return JIT_EFFECT_READ_RUNTIME | JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_CALL |
                   JIT_EFFECT_RAISE | JIT_EFFECT_BARRIER;

        case JIT_OP_YIELD:
        case JIT_OP_EXIT_INTERPRETER:
        case JIT_OP_ERROR:
            return JIT_EFFECT_WRITE_RUNTIME | JIT_EFFECT_BARRIER;

        case JIT_OP__COUNT: break;
    }
    return JIT_EFFECT_BARRIER;
}

const char* jit_opcode_name(JitOpcode op) {
#define CASE(name)                                                                                 \
    case JIT_OP_##name: return #name
    switch(op) {
        CASE(NOP);
        CASE(PHI);
        CASE(VM);
        CASE(FRAME);
        CASE(LOCAL);
        CASE(STACK_VALUE);
        CASE(CONST_NONE);
        CASE(CONST_NIL);
        CASE(CONST_ELLIPSIS);
        CASE(CONST_BOOL);
        CASE(CONST_I64);
        CASE(CONST_F64);
        CASE(CONST_POOL);
        CASE(LOAD_LOCAL);
        CASE(STORE_LOCAL);
        CASE(LOAD_GLOBAL);
        CASE(STORE_GLOBAL);
        CASE(LOAD_NONLOCAL);
        CASE(BOX_I64);
        CASE(BOX_F64);
        CASE(BOX_BOOL);
        CASE(UNBOX_I64);
        CASE(UNBOX_F64);
        CASE(UNBOX_BOOL);
        CASE(IS_TYPE);
        CASE(EXTRACT_TOKEN);
        CASE(IS_IDENTICAL);
        CASE(NOT_BOOL);
        CASE(ADD_VALUE);
        CASE(SUB_VALUE);
        CASE(MUL_VALUE);
        CASE(TRUEDIV_VALUE);
        CASE(FLOORDIV_VALUE);
        CASE(MOD_VALUE);
        CASE(POW_VALUE);
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
        CASE(EQ_VALUE);
        CASE(NE_VALUE);
        CASE(LT_VALUE);
        CASE(LE_VALUE);
        CASE(GT_VALUE);
        CASE(GE_VALUE);
        CASE(EQ_I64);
        CASE(NE_I64);
        CASE(LT_I64);
        CASE(LE_I64);
        CASE(GT_I64);
        CASE(GE_I64);
        CASE(EQ_F64);
        CASE(NE_F64);
        CASE(LT_F64);
        CASE(LE_F64);
        CASE(GT_F64);
        CASE(GE_F64);
        CASE(TRUTHY);
        CASE(LOAD_ATTR);
        CASE(STORE_ATTR);
        CASE(LOAD_METHOD);
        CASE(LOAD_SUBSCR);
        CASE(STORE_SUBSCR);
        CASE(GET_ITER);
        CASE(ITER_NEXT);
        CASE(CALL);
        CASE(CALL_NATIVE);
        CASE(CALL_HELPER);
        CASE(BUILD_TUPLE);
        CASE(BUILD_LIST);
        CASE(BUILD_DICT);
        CASE(BUILD_SET);
        CASE(JUMP);
        CASE(BRANCH);
        CASE(SWITCH);
        CASE(SYNC_STATE);
        CASE(SAFEPOINT);
        CASE(POLL);
        CASE(RETURN);
        CASE(YIELD);
        CASE(EXIT_INTERPRETER);
        CASE(ERROR);
        case JIT_OP__COUNT: return "<count>";
    }
#undef CASE
    return "<invalid-opcode>";
}

bool jit_opcode_is_terminator(JitOpcode op) {
    switch(op) {
        case JIT_OP_JUMP:
        case JIT_OP_BRANCH:
        case JIT_OP_SWITCH:
        case JIT_OP_RETURN:
        case JIT_OP_YIELD:
        case JIT_OP_EXIT_INTERPRETER:
        case JIT_OP_ERROR: return true;
        default: return false;
    }
}

bool jit_opcode_is_pure(JitOpcode op) {
    return jit_opcode_effects(op) == JIT_EFFECT_NONE && !jit_opcode_is_terminator(op);
}

void jit_region_init(JitRegion* region,
                     JitRegionKind kind,
                     const void* code_object,
                     int32_t entry_ip,
                     int32_t loop_header_ip) {
    memset(region, 0, sizeof(*region));
    region->kind = kind;
    region->code_object = code_object;
    region->entry_ip = entry_ip;
    region->loop_header_ip = loop_header_ip;
    region->entry_block = JIT_INVALID_BLOCK;

    c11_vector__ctor(&region->blocks, sizeof(JitBlock));
    c11_vector__ctor(&region->insts, sizeof(JitInst));
    c11_vector__ctor(&region->values, sizeof(JitValue));
    c11_vector__ctor(&region->operands, sizeof(JitValueId));
    c11_vector__ctor(&region->states, sizeof(JitState));
    c11_vector__ctor(&region->state_values, sizeof(JitValueId));
    c11_vector__ctor(&region->loops, sizeof(JitLoop));
}

void jit_region_destroy(JitRegion* region) {
    c11__foreach(JitBlock, &region->blocks, block) {
        c11_vector__dtor(&block->insts);
        c11_vector__dtor(&block->predecessors);
        c11_vector__dtor(&block->successors);
    }
    c11__foreach(JitLoop, &region->loops, loop) { c11_vector__dtor(&loop->blocks); }

    c11_vector__dtor(&region->blocks);
    c11_vector__dtor(&region->insts);
    c11_vector__dtor(&region->values);
    c11_vector__dtor(&region->operands);
    c11_vector__dtor(&region->states);
    c11_vector__dtor(&region->state_values);
    c11_vector__dtor(&region->loops);
    memset(region, 0, sizeof(*region));
}

JitBlockId jit_region_add_block(JitRegion* region, uint32_t frequency, uint32_t flags) {
    JitBlockId id = (JitBlockId)region->blocks.length;
    JitBlock* block = c11_vector__emplace(&region->blocks);
    block->id = id;
    block->frequency = frequency;
    block->flags = flags;
    c11_vector__ctor(&block->insts, sizeof(JitInstId));
    c11_vector__ctor(&block->predecessors, sizeof(JitBlockId));
    c11_vector__ctor(&block->successors, sizeof(JitBlockId));
    if(region->entry_block == JIT_INVALID_BLOCK) {
        region->entry_block = id;
        block->flags |= JIT_BLOCK_ENTRY;
    }
    return id;
}

static JitOperandRange append_values(c11_vector* vector, const JitValueId* values, uint32_t count) {
    JitOperandRange range = {(uint32_t)vector->length, count};
    if(count != 0) {
        assert(values != NULL);
        c11_vector__extend(vector, values, (int)count);
    }
    return range;
}

JitStateId jit_region_add_state(JitRegion* region,
                                int32_t bytecode_ip,
                                const JitValueId* locals,
                                uint32_t locals_count,
                                const JitValueId* stack,
                                uint32_t stack_count) {
    JitStateId id = (JitStateId)region->states.length;
    JitState* state = c11_vector__emplace(&region->states);
    state->bytecode_ip = bytecode_ip;
    state->locals = append_values(&region->state_values, locals, locals_count);
    state->stack = append_values(&region->state_values, stack, stack_count);
    return id;
}

JitLoopId jit_region_add_loop(JitRegion* region,
                              JitBlockId header,
                              JitBlockId latch,
                              const JitBlockId* blocks,
                              uint32_t block_count,
                              uint32_t depth) {
    JitLoopId id = (JitLoopId)region->loops.length;
    JitLoop* loop = c11_vector__emplace(&region->loops);
    loop->header = header;
    loop->latch = latch;
    loop->depth = depth;
    c11_vector__ctor(&loop->blocks, sizeof(JitBlockId));
    if(block_count != 0) {
        assert(blocks != NULL);
        c11_vector__extend(&loop->blocks, blocks, (int)block_count);
    }
    return id;
}

JitBlock* jit_region_block(JitRegion* region, JitBlockId id) {
    assert(id < (JitBlockId)region->blocks.length);
    return c11__at(JitBlock, &region->blocks, id);
}

const JitBlock* jit_region_block_const(const JitRegion* region, JitBlockId id) {
    assert(id < (JitBlockId)region->blocks.length);
    return c11__at(JitBlock, &region->blocks, id);
}

JitInst* jit_region_inst(JitRegion* region, JitInstId id) {
    assert(id < (JitInstId)region->insts.length);
    return c11__at(JitInst, &region->insts, id);
}

const JitInst* jit_region_inst_const(const JitRegion* region, JitInstId id) {
    assert(id < (JitInstId)region->insts.length);
    return c11__at(JitInst, &region->insts, id);
}

JitValue* jit_region_value(JitRegion* region, JitValueId id) {
    assert(id < (JitValueId)region->values.length);
    return c11__at(JitValue, &region->values, id);
}

const JitValue* jit_region_value_const(const JitRegion* region, JitValueId id) {
    assert(id < (JitValueId)region->values.length);
    return c11__at(JitValue, &region->values, id);
}

const JitValueId* jit_inst_operands(const JitRegion* region, const JitInst* inst) {
    if(inst->operands.count == 0) return NULL;
    return c11__at(JitValueId, &region->operands, inst->operands.offset);
}

const JitValueId* jit_state_locals(const JitRegion* region, const JitState* state) {
    if(state->locals.count == 0) return NULL;
    return c11__at(JitValueId, &region->state_values, state->locals.offset);
}

const JitValueId* jit_state_stack(const JitRegion* region, const JitState* state) {
    if(state->stack.count == 0) return NULL;
    return c11__at(JitValueId, &region->state_values, state->stack.offset);
}

void jit_builder_init(JitBuilder* builder, JitRegion* region, JitBlockId block) {
    assert(block < (JitBlockId)region->blocks.length);
    builder->region = region;
    builder->block = block;
    builder->source.bytecode_ip = -1;
    builder->source.line = -1;
}

void jit_builder_set_block(JitBuilder* builder, JitBlockId block) {
    assert(block < (JitBlockId)builder->region->blocks.length);
    builder->block = block;
}

void jit_builder_set_source(JitBuilder* builder, int32_t bytecode_ip, int32_t line) {
    builder->source.bytecode_ip = bytecode_ip;
    builder->source.line = line;
}

JitInstId jit_builder_emit(JitBuilder* builder,
                           JitOpcode op,
                           JitTypeInfo result_type,
                           const JitValueId* operands,
                           uint32_t operand_count,
                           JitImmediate imm,
                           uint32_t aux) {
    JitRegion* region = builder->region;
    JitBlock* block = jit_region_block(region, builder->block);
    if(block->insts.length != 0) {
        JitInstId last_id = c11_vector__back(JitInstId, &block->insts);
        assert(!jit_opcode_is_terminator(jit_region_inst(region, last_id)->op));
    }

    JitInstId id = (JitInstId)region->insts.length;
    JitInst* inst = c11_vector__emplace(&region->insts);
    memset(inst, 0, sizeof(*inst));
    inst->op = op;
    inst->result = JIT_INVALID_VALUE;
    inst->type = result_type;
    inst->operands = append_values(&region->operands, operands, operand_count);
    inst->imm = imm;
    inst->aux = aux;
    inst->effects = jit_opcode_effects(op);
    inst->resume_state = JIT_INVALID_STATE;
    inst->source = builder->source;

    if(result_type.representation != JIT_TYPE_VOID) {
        JitValueId value_id = (JitValueId)region->values.length;
        JitValue* value = c11_vector__emplace(&region->values);
        value->type = result_type;
        value->def = id;
        value->use_count = 0;
        inst = jit_region_inst(region, id);
        inst->result = value_id;
    }

    c11_vector__push(JitInstId, &block->insts, id);

    if(inst->effects & JIT_EFFECT_CALL) region->contains_calls = true;
    if(inst->effects & JIT_EFFECT_ALLOCATE) region->contains_allocations = true;

    return id;
}

JitValueId jit_builder_emit_value(JitBuilder* builder,
                                  JitOpcode op,
                                  JitTypeInfo result_type,
                                  const JitValueId* operands,
                                  uint32_t operand_count,
                                  JitImmediate imm,
                                  uint32_t aux) {
    assert(result_type.representation != JIT_TYPE_VOID);
    JitInstId inst = jit_builder_emit(builder, op, result_type, operands, operand_count, imm, aux);
    return jit_region_inst(builder->region, inst)->result;
}

JitValueId jit_builder_const_i64(JitBuilder* builder, int64_t value) {
    JitImmediate imm = jit_imm_zero();
    imm.i64 = value;
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_I64,
                                  jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                  NULL,
                                  0,
                                  imm,
                                  0);
}

JitValueId jit_builder_const_f64(JitBuilder* builder, double value) {
    JitImmediate imm = jit_imm_zero();
    imm.f64 = value;
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_F64,
                                  jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT),
                                  NULL,
                                  0,
                                  imm,
                                  0);
}

JitValueId jit_builder_const_bool(JitBuilder* builder, bool value) {
    JitImmediate imm = jit_imm_zero();
    imm.u32 = value ? 1u : 0u;
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_BOOL,
                                  jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                  NULL,
                                  0,
                                  imm,
                                  0);
}

JitValueId jit_builder_const_none(JitBuilder* builder) {
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_NONE,
                                  jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_NONE),
                                  NULL,
                                  0,
                                  jit_imm_zero(),
                                  0);
}

JitValueId jit_builder_const_nil(JitBuilder* builder) {
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_NIL,
                                  jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_NIL),
                                  NULL,
                                  0,
                                  jit_imm_zero(),
                                  0);
}

JitValueId jit_builder_const_ellipsis(JitBuilder* builder) {
    return jit_builder_emit_value(builder,
                                  JIT_OP_CONST_ELLIPSIS,
                                  jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_ELLIPSIS),
                                  NULL,
                                  0,
                                  jit_imm_zero(),
                                  0);
}

JitInstId jit_builder_jump(JitBuilder* builder, JitBlockId target) {
    assert(target < (JitBlockId)builder->region->blocks.length);
    return jit_builder_emit(builder,
                            JIT_OP_JUMP,
                            jit_type_unknown(JIT_TYPE_VOID),
                            NULL,
                            0,
                            jit_imm_zero(),
                            target);
}

JitInstId jit_builder_branch(JitBuilder* builder,
                             JitValueId condition,
                             JitBlockId if_true,
                             JitBlockId if_false) {
    assert(if_true < (JitBlockId)builder->region->blocks.length);
    assert(if_false < (JitBlockId)builder->region->blocks.length);
    JitImmediate imm = jit_imm_zero();
    imm.u32 = if_true;
    return jit_builder_emit(builder,
                            JIT_OP_BRANCH,
                            jit_type_unknown(JIT_TYPE_VOID),
                            &condition,
                            1,
                            imm,
                            if_false);
}

JitInstId jit_builder_return(JitBuilder* builder, JitValueId value) {
    return jit_builder_emit(builder,
                            JIT_OP_RETURN,
                            jit_type_unknown(JIT_TYPE_VOID),
                            &value,
                            1,
                            jit_imm_zero(),
                            0);
}

JitInstId jit_builder_exit_interpreter(JitBuilder* builder, JitStateId state) {
    assert(state < (JitStateId)builder->region->states.length);
    return jit_builder_emit(builder,
                            JIT_OP_EXIT_INTERPRETER,
                            jit_type_unknown(JIT_TYPE_VOID),
                            NULL,
                            0,
                            jit_imm_zero(),
                            state);
}

static bool block_has_id(const c11_vector* vector, JitBlockId id) {
    c11__foreach(JitBlockId, vector, item) {
        if(*item == id) return true;
    }
    return false;
}

static void cfg_add_edge(JitRegion* region, JitBlockId from, JitBlockId to) {
    assert(from < (JitBlockId)region->blocks.length);
    assert(to < (JitBlockId)region->blocks.length);
    JitBlock* src = jit_region_block(region, from);
    JitBlock* dst = jit_region_block(region, to);
    if(!block_has_id(&src->successors, to)) c11_vector__push(JitBlockId, &src->successors, to);
    if(!block_has_id(&dst->predecessors, from)) c11_vector__push(JitBlockId, &dst->predecessors, from);
}

void jit_region_rebuild_cfg(JitRegion* region) {
    c11__foreach(JitBlock, &region->blocks, block) {
        c11_vector__clear(&block->predecessors);
        c11_vector__clear(&block->successors);
    }

    c11__foreach(JitBlock, &region->blocks, block) {
        if(block->insts.length == 0) continue;
        JitInstId id = c11_vector__back(JitInstId, &block->insts);
        JitInst* inst = jit_region_inst(region, id);
        if(inst->removed) continue;
        switch(inst->op) {
            case JIT_OP_JUMP:
                if(inst->aux < (uint32_t)region->blocks.length)
                    cfg_add_edge(region, block->id, inst->aux);
                break;
            case JIT_OP_BRANCH:
                if(inst->imm.u32 < (uint32_t)region->blocks.length)
                    cfg_add_edge(region, block->id, inst->imm.u32);
                if(inst->aux < (uint32_t)region->blocks.length)
                    cfg_add_edge(region, block->id, inst->aux);
                break;
            default: break;
        }
    }
}

void jit_region_recompute_uses(JitRegion* region) {
    c11__foreach(JitValue, &region->values, value) { value->use_count = 0; }

    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed) continue;
        const JitValueId* operands = jit_inst_operands(region, inst);
        for(uint32_t i = 0; i < inst->operands.count; i++) {
            if(operands[i] < (JitValueId)region->values.length)
                jit_region_value(region, operands[i])->use_count++;
        }
    }

    c11__foreach(JitState, &region->states, state) {
        const JitValueId* locals = jit_state_locals(region, state);
        const JitValueId* stack = jit_state_stack(region, state);
        for(uint32_t i = 0; i < state->locals.count; i++) {
            if(locals[i] < (JitValueId)region->values.length)
                jit_region_value(region, locals[i])->use_count++;
        }
        for(uint32_t i = 0; i < state->stack.count; i++) {
            if(stack[i] < (JitValueId)region->values.length)
                jit_region_value(region, stack[i])->use_count++;
        }
    }
}

bool jit_region_replace_all_uses(JitRegion* region, JitValueId from, JitValueId to) {
    if(from == to) return false;
    assert(from < (JitValueId)region->values.length);
    assert(to < (JitValueId)region->values.length);
    bool changed = false;

    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed) continue;
        JitValueId* operands = inst->operands.count == 0
                                   ? NULL
                                   : c11__at(JitValueId, &region->operands, inst->operands.offset);
        for(uint32_t i = 0; i < inst->operands.count; i++) {
            if(operands[i] == from) {
                operands[i] = to;
                changed = true;
            }
        }
    }

    c11__foreach(JitState, &region->states, state) {
        JitValueId* locals = state->locals.count == 0
                                 ? NULL
                                 : c11__at(JitValueId, &region->state_values, state->locals.offset);
        JitValueId* stack = state->stack.count == 0
                                ? NULL
                                : c11__at(JitValueId, &region->state_values, state->stack.offset);
        for(uint32_t i = 0; i < state->locals.count; i++) {
            if(locals[i] == from) {
                locals[i] = to;
                changed = true;
            }
        }
        for(uint32_t i = 0; i < state->stack.count; i++) {
            if(stack[i] == from) {
                stack[i] = to;
                changed = true;
            }
        }
    }

    if(changed) jit_region_recompute_uses(region);
    return changed;
}

static bool verify_fail(char* error, uint32_t error_size, const char* fmt, ...) {
    if(error != NULL && error_size != 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_size, fmt, args);
        va_end(args);
        error[error_size - 1] = '\0';
    }
    return false;
}

bool jit_region_verify(const JitRegion* region, char* error, uint32_t error_size) {
    if(region->blocks.length == 0) return verify_fail(error, error_size, "region has no blocks");
    if(region->entry_block >= (JitBlockId)region->blocks.length)
        return verify_fail(error, error_size, "invalid entry block %u", region->entry_block);

    for(int bi = 0; bi < region->blocks.length; bi++) {
        const JitBlock* block = c11__at(JitBlock, &region->blocks, bi);
        if(block->id != (JitBlockId)bi)
            return verify_fail(error, error_size, "block id mismatch at index %d", bi);
        if(block->insts.length == 0)
            return verify_fail(error, error_size, "block B%u is empty", block->id);

        bool seen_terminator = false;
        c11__foreach(JitInstId, &block->insts, inst_id) {
            if(*inst_id >= (JitInstId)region->insts.length)
                return verify_fail(error,
                                   error_size,
                                   "block B%u contains invalid instruction %u",
                                   block->id,
                                   *inst_id);
            const JitInst* inst = c11__at(JitInst, &region->insts, *inst_id);
            if(inst->removed) continue;
            if(seen_terminator)
                return verify_fail(error,
                                   error_size,
                                   "instruction follows terminator in B%u",
                                   block->id);
            seen_terminator = jit_opcode_is_terminator(inst->op);

            if(inst->operands.offset > (uint32_t)region->operands.length ||
               inst->operands.count > (uint32_t)region->operands.length - inst->operands.offset)
                return verify_fail(error, error_size, "invalid operand range in I%u", *inst_id);

            const JitValueId* operands = jit_inst_operands(region, inst);
            for(uint32_t i = 0; i < inst->operands.count; i++) {
                if(operands[i] >= (JitValueId)region->values.length)
                    return verify_fail(error,
                                       error_size,
                                       "I%u references invalid value v%u",
                                       *inst_id,
                                       operands[i]);
            }

            if(inst->result != JIT_INVALID_VALUE) {
                if(inst->result >= (JitValueId)region->values.length)
                    return verify_fail(error, error_size, "I%u has invalid result", *inst_id);
                const JitValue* value = c11__at(JitValue, &region->values, inst->result);
                if(value->def != *inst_id)
                    return verify_fail(error,
                                       error_size,
                                       "v%u definition mismatch",
                                       inst->result);
                if(!jit_type_equal(value->type, inst->type))
                    return verify_fail(error,
                                       error_size,
                                       "v%u type mismatch",
                                       inst->result);
            }

            if(inst->op == JIT_OP_JUMP && inst->aux >= (uint32_t)region->blocks.length)
                return verify_fail(error, error_size, "I%u jumps to invalid block", *inst_id);
            if(inst->op == JIT_OP_BRANCH &&
               (inst->imm.u32 >= (uint32_t)region->blocks.length ||
                inst->aux >= (uint32_t)region->blocks.length))
                return verify_fail(error, error_size, "I%u branches to invalid block", *inst_id);
            if(inst->op == JIT_OP_EXIT_INTERPRETER &&
               inst->aux >= (uint32_t)region->states.length)
                return verify_fail(error, error_size, "I%u has invalid resume state", *inst_id);
            if(inst->resume_state != JIT_INVALID_STATE &&
               inst->resume_state >= (JitStateId)region->states.length)
                return verify_fail(error,
                                   error_size,
                                   "I%u has invalid attached resume state",
                                   *inst_id);
        }

        if(!seen_terminator)
            return verify_fail(error, error_size, "block B%u has no terminator", block->id);
    }

    for(int si = 0; si < region->states.length; si++) {
        const JitState* state = c11__at(JitState, &region->states, si);
        if(state->locals.offset > (uint32_t)region->state_values.length ||
           state->locals.count >
               (uint32_t)region->state_values.length - state->locals.offset)
            return verify_fail(error, error_size, "state S%d has invalid locals", si);
        if(state->stack.offset > (uint32_t)region->state_values.length ||
           state->stack.count > (uint32_t)region->state_values.length - state->stack.offset)
            return verify_fail(error, error_size, "state S%d has invalid stack", si);

        const JitValueId* locals = jit_state_locals(region, state);
        const JitValueId* stack = jit_state_stack(region, state);
        for(uint32_t i = 0; i < state->locals.count; i++) {
            if(locals[i] >= (JitValueId)region->values.length)
                return verify_fail(error, error_size, "state S%d has invalid local value", si);
        }
        for(uint32_t i = 0; i < state->stack.count; i++) {
            if(stack[i] >= (JitValueId)region->values.length)
                return verify_fail(error, error_size, "state S%d has invalid stack value", si);
        }
    }

    if(error != NULL && error_size != 0) error[0] = '\0';
    return true;
}

static const char* type_name(JitType type) {
    switch(type) {
        case JIT_TYPE_VOID: return "void";
        case JIT_TYPE_VALUE: return "value";
        case JIT_TYPE_I64: return "i64";
        case JIT_TYPE_F64: return "f64";
        case JIT_TYPE_BOOL: return "bool";
        case JIT_TYPE_PY_TYPE: return "pytype";
        case JIT_TYPE_NAME: return "name";
        case JIT_TYPE_PTR: return "ptr";
        case JIT_TYPE_OBJECT_PTR: return "objptr";
        case JIT_TYPE_VALUE_PTR: return "valueptr";
        case JIT_TYPE_VM_PTR: return "vmptr";
        case JIT_TYPE_FRAME_PTR: return "frameptr";
        case JIT_TYPE_TOKEN: return "token";
    }
    return "?";
}

static void dump_writef(void (*write)(const char*, void*), void* ctx, const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    write(buffer, ctx);
}

void jit_region_dump(const JitRegion* region, void (*write)(const char* text, void* ctx), void* ctx) {
    if(write == NULL) return;
    dump_writef(write,
                ctx,
                "region %s entry_ip=%d loop_header=%d blocks=%d values=%d\n",
                region->kind == JIT_REGION_FUNCTION ? "function" : "loop",
                region->entry_ip,
                region->loop_header_ip,
                region->blocks.length,
                region->values.length);

    c11__foreach(JitBlock, &region->blocks, block) {
        dump_writef(write,
                    ctx,
                    "\nB%u freq=%u flags=0x%x preds=[",
                    block->id,
                    block->frequency,
                    block->flags);
        for(int i = 0; i < block->predecessors.length; i++) {
            if(i) write(",", ctx);
            dump_writef(write, ctx, "B%u", c11__getitem(JitBlockId, &block->predecessors, i));
        }
        write("] succs=[", ctx);
        for(int i = 0; i < block->successors.length; i++) {
            if(i) write(",", ctx);
            dump_writef(write, ctx, "B%u", c11__getitem(JitBlockId, &block->successors, i));
        }
        write("]\n", ctx);

        c11__foreach(JitInstId, &block->insts, inst_id) {
            const JitInst* inst = jit_region_inst_const(region, *inst_id);
            if(inst->removed) continue;
            dump_writef(write, ctx, "  I%u: ", *inst_id);
            if(inst->result != JIT_INVALID_VALUE)
                dump_writef(write,
                            ctx,
                            "v%u:%s = ",
                            inst->result,
                            type_name(inst->type.representation));
            dump_writef(write, ctx, "%s", jit_opcode_name(inst->op));

            const JitValueId* operands = jit_inst_operands(region, inst);
            for(uint32_t i = 0; i < inst->operands.count; i++)
                dump_writef(write, ctx, "%s v%u", i == 0 ? "" : ",", operands[i]);

            switch(inst->op) {
                case JIT_OP_CONST_I64: dump_writef(write, ctx, " %lld", (long long)inst->imm.i64); break;
                case JIT_OP_CONST_F64: dump_writef(write, ctx, " %.17g", inst->imm.f64); break;
                case JIT_OP_CONST_BOOL: dump_writef(write, ctx, " %s", inst->imm.u32 ? "true" : "false"); break;
                case JIT_OP_JUMP: dump_writef(write, ctx, " B%u", inst->aux); break;
                case JIT_OP_BRANCH:
                    dump_writef(write, ctx, " ? B%u : B%u", inst->imm.u32, inst->aux);
                    break;
                case JIT_OP_EXIT_INTERPRETER: dump_writef(write, ctx, " S%u", inst->aux); break;
                default: break;
            }
            if(inst->resume_state != JIT_INVALID_STATE)
                dump_writef(write, ctx, " state=S%u", inst->resume_state);
            dump_writef(write,
                        ctx,
                        "  ; bc=%d line=%d effects=0x%x\n",
                        inst->source.bytecode_ip,
                        inst->source.line,
                        inst->effects);
        }
    }
}
