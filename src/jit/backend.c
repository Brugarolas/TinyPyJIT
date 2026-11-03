#include "pocketpy/jit/backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool backend_fail(JitBackendError* error,
                         JitBackendStatus status,
                         JitBlockId block,
                         JitInstId inst,
                         const char* fmt,
                         ...) {
    if(error != NULL) {
        error->status = status;
        error->mir_block = block;
        error->mir_inst = inst;
        va_list args;
        va_start(args, fmt);
        vsnprintf(error->message, sizeof(error->message), fmt, args);
        va_end(args);
        error->message[sizeof(error->message) - 1u] = '\0';
    }
    return false;
}

void jit_backend_options_init(JitBackendOptions* options) {
    if(options == NULL) return;
    options->tvalue_size = 32u;
    options->stack_alignment = 16u;
    options->resolve_phis = true;
    options->run_peephole = true;
    options->verify = true;
}

const char* jit_backend_status_name(JitBackendStatus status) {
    switch(status) {
        case JIT_BACKEND_OK: return "ok";
        case JIT_BACKEND_INVALID_ARGUMENT: return "invalid argument";
        case JIT_BACKEND_INVALID_MIR: return "invalid MIR";
        case JIT_BACKEND_UNSUPPORTED_MIR: return "unsupported MIR";
        case JIT_BACKEND_PHI_ERROR: return "phi lowering error";
        case JIT_BACKEND_VERIFY_FAILED: return "LIR verification failed";
        case JIT_BACKEND_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown";
}

static JitImmediate imm_zero(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

static JitVRegClass class_for_type(JitType type) {
    switch(type) {
        case JIT_TYPE_VOID:
        case JIT_TYPE_TOKEN: return JIT_VREG_NONE;
        case JIT_TYPE_VALUE: return JIT_VREG_VALUE;
        case JIT_TYPE_F64: return JIT_VREG_FPR;
        default: return JIT_VREG_GPR;
    }
}

static uint32_t map_block_flags(uint32_t flags) {
    uint32_t out = JIT_LIR_BLOCK_NONE;
    if((flags & JIT_BLOCK_ENTRY) != 0u) out |= JIT_LIR_BLOCK_ENTRY;
    if((flags & JIT_BLOCK_LOOP_HEADER) != 0u) out |= JIT_LIR_BLOCK_LOOP_HEADER;
    if((flags & JIT_BLOCK_LOOP_LATCH) != 0u) out |= JIT_LIR_BLOCK_LOOP_LATCH;
    if((flags & JIT_BLOCK_COLD) != 0u) out |= JIT_LIR_BLOCK_COLD;
    return out;
}

static uint32_t lir_flags_from_effects(uint32_t effects) {
    uint32_t flags = 0u;
    if((effects & JIT_EFFECT_CALL) != 0u) flags |= JIT_LIR_FLAG_CALL;
    if((effects & JIT_EFFECT_RAISE) != 0u) flags |= JIT_LIR_FLAG_RAISE;
    if((effects & JIT_EFFECT_BARRIER) != 0u) flags |= JIT_LIR_FLAG_BARRIER;
    return flags;
}

static JitLirOperand map_value(const JitLirFunction* lir, JitValueId value) {
    if(value == JIT_INVALID_VALUE || value >= (JitValueId)lir->mir_value_map.length)
        return jit_lir_none();
    return c11__getitem(JitLirOperand, &lir->mir_value_map, value);
}

static bool precreate_vregs(const JitRegion* region,
                            JitLirFunction* lir,
                            JitBackendStats* stats,
                            JitBackendError* error) {
    for(int i = 0; i < region->values.length; i++) {
        const JitValue* value = c11__at(JitValue, &region->values, i);
        JitVRegClass cls = class_for_type(value->type.representation);
        JitLirOperand mapped = jit_lir_none();
        if(cls != JIT_VREG_NONE) {
            JitVRegId id = jit_lir_new_vreg(lir, cls, value->type, (JitValueId)i);
            mapped = jit_lir_vreg(id, value->type);
            stats->vregs++;
            if(cls == JIT_VREG_VALUE) stats->value_vregs++;
        }
        c11_vector__push(JitLirOperand, &lir->mir_value_map, mapped);
    }

    /* Multiple EXTRACT_TOKEN instructions for the same token/index are aliases. */
    for(int i = 0; i < region->insts.length; i++) {
        const JitInst* inst = c11__at(JitInst, &region->insts, i);
        if(inst->removed || inst->op != JIT_OP_EXTRACT_TOKEN || inst->result == JIT_INVALID_VALUE)
            continue;
        const JitValueId* operands = jit_inst_operands(region, inst);
        if(inst->operands.count != 1u || operands == NULL)
            return backend_fail(error,
                                JIT_BACKEND_INVALID_MIR,
                                JIT_INVALID_BLOCK,
                                (JitInstId)i,
                                "EXTRACT_TOKEN must have exactly one token operand");
        JitValueId token = operands[0];
        for(int j = 0; j < i; j++) {
            const JitInst* prev = c11__at(JitInst, &region->insts, j);
            if(prev->removed || prev->op != JIT_OP_EXTRACT_TOKEN || prev->aux != inst->aux ||
               prev->result == JIT_INVALID_VALUE)
                continue;
            const JitValueId* prev_ops = jit_inst_operands(region, prev);
            if(prev->operands.count == 1u && prev_ops != NULL && prev_ops[0] == token) {
                c11__setitem(JitLirOperand,
                             &lir->mir_value_map,
                             inst->result,
                             map_value(lir, prev->result));
                break;
            }
        }
    }
    return true;
}

static bool build_states(const JitRegion* region,
                         JitLirFunction* lir,
                         JitBackendError* error) {
    for(int i = 0; i < region->states.length; i++) {
        const JitState* state = c11__at(JitState, &region->states, i);
        JitLirOperand* locals = NULL;
        JitLirOperand* stack = NULL;
        if(state->locals.count != 0u) {
            locals = malloc(sizeof(JitLirOperand) * state->locals.count);
            if(locals == NULL)
                return backend_fail(error,
                                    JIT_BACKEND_OUT_OF_MEMORY,
                                    JIT_INVALID_BLOCK,
                                    JIT_INVALID_INST,
                                    "out of memory lowering resume-state locals");
            const JitValueId* values = jit_state_locals(region, state);
            for(uint32_t n = 0; n < state->locals.count; n++) {
                locals[n] = map_value(lir, values[n]);
                if(locals[n].kind == JIT_LIR_OPERAND_NONE) {
                    free(locals);
                    return backend_fail(error,
                                        JIT_BACKEND_INVALID_MIR,
                                        JIT_INVALID_BLOCK,
                                        JIT_INVALID_INST,
                                        "resume state references unmaterializable local value");
                }
            }
        }
        if(state->stack.count != 0u) {
            stack = malloc(sizeof(JitLirOperand) * state->stack.count);
            if(stack == NULL) {
                free(locals);
                return backend_fail(error,
                                    JIT_BACKEND_OUT_OF_MEMORY,
                                    JIT_INVALID_BLOCK,
                                    JIT_INVALID_INST,
                                    "out of memory lowering resume-state stack");
            }
            const JitValueId* values = jit_state_stack(region, state);
            for(uint32_t n = 0; n < state->stack.count; n++) {
                stack[n] = map_value(lir, values[n]);
                if(stack[n].kind == JIT_LIR_OPERAND_NONE) {
                    free(locals);
                    free(stack);
                    return backend_fail(error,
                                        JIT_BACKEND_INVALID_MIR,
                                        JIT_INVALID_BLOCK,
                                        JIT_INVALID_INST,
                                        "resume state references unmaterializable stack value");
                }
            }
        }
        JitLirStateId sid = jit_lir_add_state(lir,
                                              state->bytecode_ip,
                                              locals,
                                              state->locals.count,
                                              stack,
                                              state->stack.count);
        free(locals);
        free(stack);
        if(sid != (JitLirStateId)i)
            return backend_fail(error,
                                JIT_BACKEND_INVALID_MIR,
                                JIT_INVALID_BLOCK,
                                JIT_INVALID_INST,
                                "LIR state numbering diverged from MIR state numbering");
    }
    return true;
}

static bool map_uses(const JitRegion* region,
                     const JitLirFunction* lir,
                     const JitInst* inst,
                     JitLirOperand** out_uses,
                     uint32_t* out_count,
                     JitBackendError* error,
                     JitBlockId block,
                     JitInstId inst_id) {
    uint32_t count = inst->operands.count;
    *out_count = count;
    *out_uses = NULL;
    if(count == 0u) return true;
    JitLirOperand* uses = malloc(sizeof(JitLirOperand) * count);
    if(uses == NULL)
        return backend_fail(error,
                            JIT_BACKEND_OUT_OF_MEMORY,
                            block,
                            inst_id,
                            "out of memory lowering MIR operands");
    const JitValueId* values = jit_inst_operands(region, inst);
    for(uint32_t i = 0; i < count; i++) {
        uses[i] = map_value(lir, values[i]);
        if(uses[i].kind == JIT_LIR_OPERAND_NONE) {
            free(uses);
            return backend_fail(error,
                                JIT_BACKEND_INVALID_MIR,
                                block,
                                inst_id,
                                "MIR operand v%u has no LIR representation",
                                values[i]);
        }
    }
    *out_uses = uses;
    return true;
}

static JitLirCondition condition_for_mir(JitOpcode op) {
    switch(op) {
        case JIT_OP_EQ_I64:
        case JIT_OP_EQ_F64: return JIT_LIR_COND_EQ;
        case JIT_OP_NE_I64:
        case JIT_OP_NE_F64: return JIT_LIR_COND_NE;
        case JIT_OP_LT_I64:
        case JIT_OP_LT_F64: return JIT_LIR_COND_LT;
        case JIT_OP_LE_I64:
        case JIT_OP_LE_F64: return JIT_LIR_COND_LE;
        case JIT_OP_GT_I64:
        case JIT_OP_GT_F64: return JIT_LIR_COND_GT;
        case JIT_OP_GE_I64:
        case JIT_OP_GE_F64: return JIT_LIR_COND_GE;
        default: return JIT_LIR_COND_EQ;
    }
}

static JitLirOpcode simple_opcode(JitOpcode op) {
    switch(op) {
        case JIT_OP_BOX_I64: return JIT_LIR_BOX_I64;
        case JIT_OP_BOX_F64: return JIT_LIR_BOX_F64;
        case JIT_OP_BOX_BOOL: return JIT_LIR_BOX_BOOL;
        case JIT_OP_UNBOX_I64: return JIT_LIR_UNBOX_I64;
        case JIT_OP_UNBOX_F64: return JIT_LIR_UNBOX_F64;
        case JIT_OP_UNBOX_BOOL: return JIT_LIR_UNBOX_BOOL;
        case JIT_OP_IS_TYPE: return JIT_LIR_IS_TYPE;
        case JIT_OP_IS_IDENTICAL: return JIT_LIR_IS_IDENTICAL;
        case JIT_OP_NOT_BOOL: return JIT_LIR_NOT_BOOL;
        case JIT_OP_ADD_I64: return JIT_LIR_ADD_I64;
        case JIT_OP_SUB_I64: return JIT_LIR_SUB_I64;
        case JIT_OP_MUL_I64: return JIT_LIR_MUL_I64;
        case JIT_OP_DIV_I64: return JIT_LIR_DIV_I64;
        case JIT_OP_MOD_I64: return JIT_LIR_MOD_I64;
        case JIT_OP_AND_I64: return JIT_LIR_AND_I64;
        case JIT_OP_OR_I64: return JIT_LIR_OR_I64;
        case JIT_OP_XOR_I64: return JIT_LIR_XOR_I64;
        case JIT_OP_SHL_I64: return JIT_LIR_SHL_I64;
        case JIT_OP_SHR_I64: return JIT_LIR_SHR_I64;
        case JIT_OP_NEG_I64: return JIT_LIR_NEG_I64;
        case JIT_OP_NOT_I64: return JIT_LIR_NOT_I64;
        case JIT_OP_ADD_F64: return JIT_LIR_ADD_F64;
        case JIT_OP_SUB_F64: return JIT_LIR_SUB_F64;
        case JIT_OP_MUL_F64: return JIT_LIR_MUL_F64;
        case JIT_OP_DIV_F64: return JIT_LIR_DIV_F64;
        case JIT_OP_NEG_F64: return JIT_LIR_NEG_F64;
        case JIT_OP_EQ_I64:
        case JIT_OP_NE_I64:
        case JIT_OP_LT_I64:
        case JIT_OP_LE_I64:
        case JIT_OP_GT_I64:
        case JIT_OP_GE_I64: return JIT_LIR_CMP_I64;
        case JIT_OP_EQ_F64:
        case JIT_OP_NE_F64:
        case JIT_OP_LT_F64:
        case JIT_OP_LE_F64:
        case JIT_OP_GT_F64:
        case JIT_OP_GE_F64: return JIT_LIR_CMP_F64;
        default: return JIT_LIR__COUNT;
    }
}

static bool token_defs_for_producer(const JitRegion* region,
                                    JitLirFunction* lir,
                                    JitValueId token,
                                    JitLirOperand** out_defs,
                                    uint32_t* out_count,
                                    JitBackendError* error,
                                    JitBlockId block,
                                    JitInstId inst_id) {
    uint32_t max_index = 0u;
    bool found = false;
    for(int i = 0; i < region->insts.length; i++) {
        const JitInst* extract = c11__at(JitInst, &region->insts, i);
        if(extract->removed || extract->op != JIT_OP_EXTRACT_TOKEN || extract->result == JIT_INVALID_VALUE)
            continue;
        const JitValueId* operands = jit_inst_operands(region, extract);
        if(extract->operands.count == 1u && operands != NULL && operands[0] == token) {
            found = true;
            if(extract->aux > max_index) max_index = extract->aux;
        }
    }
    if(!found) {
        *out_defs = NULL;
        *out_count = 0u;
        return true;
    }

    uint32_t count = max_index + 1u;
    JitLirOperand* defs = malloc(sizeof(JitLirOperand) * count);
    if(defs == NULL)
        return backend_fail(error,
                            JIT_BACKEND_OUT_OF_MEMORY,
                            block,
                            inst_id,
                            "out of memory lowering token results");
    for(uint32_t index = 0; index < count; index++) defs[index] = jit_lir_none();

    for(int i = 0; i < region->insts.length; i++) {
        const JitInst* extract = c11__at(JitInst, &region->insts, i);
        if(extract->removed || extract->op != JIT_OP_EXTRACT_TOKEN || extract->result == JIT_INVALID_VALUE)
            continue;
        const JitValueId* operands = jit_inst_operands(region, extract);
        if(extract->operands.count != 1u || operands == NULL || operands[0] != token) continue;
        if(defs[extract->aux].kind == JIT_LIR_OPERAND_NONE)
            defs[extract->aux] = map_value(lir, extract->result);
    }

    for(uint32_t index = 0; index < count; index++) {
        if(defs[index].kind != JIT_LIR_OPERAND_NONE) continue;
        JitTypeInfo type = jit_type_value();
        JitVRegId vreg = jit_lir_new_vreg(lir, JIT_VREG_VALUE, type, JIT_INVALID_VALUE);
        defs[index] = jit_lir_vreg(vreg, type);
    }
    *out_defs = defs;
    *out_count = count;
    return true;
}

static bool emit_runtime(const JitRegion* region,
                         JitLirFunction* lir,
                         JitLirBlockId block,
                         JitInstId inst_id,
                         const JitInst* inst,
                         JitLirRuntimeKind kind,
                         uint32_t runtime_id,
                         JitBackendStats* stats,
                         JitBackendError* error) {
    JitLirOperand* uses = NULL;
    uint32_t use_count = 0u;
    if(!map_uses(region, lir, inst, &uses, &use_count, error, block, inst_id)) return false;

    JitLirOperand single_def;
    JitLirOperand* defs = NULL;
    uint32_t def_count = 0u;
    if(inst->result != JIT_INVALID_VALUE) {
        const JitValue* result = jit_region_value_const(region, inst->result);
        if(result->type.representation == JIT_TYPE_TOKEN) {
            if(!token_defs_for_producer(region,
                                        lir,
                                        inst->result,
                                        &defs,
                                        &def_count,
                                        error,
                                        block,
                                        inst_id)) {
                free(uses);
                return false;
            }
        } else if(result->type.representation != JIT_TYPE_VOID) {
            single_def = map_value(lir, inst->result);
            defs = &single_def;
            def_count = 1u;
        }
    }

    uint32_t flags = lir_flags_from_effects(inst->effects);
    JitLirStateId state = inst->resume_state == JIT_INVALID_STATE
                              ? JIT_INVALID_LIR_STATE
                              : (JitLirStateId)inst->resume_state;
    jit_lir_emit(lir,
                 block,
                 JIT_LIR_RUNTIME,
                 defs,
                 def_count,
                 uses,
                 use_count,
                 inst->imm,
                 inst->aux,
                 (uint32_t)kind,
                 runtime_id,
                 flags,
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_BLOCK,
                 state,
                 inst->source);
    if(def_count != 0u && defs != &single_def) free(defs);
    free(uses);
    stats->runtime_ops++;
    return true;
}

static bool emit_simple(const JitRegion* region,
                        JitLirFunction* lir,
                        JitLirBlockId block,
                        JitInstId inst_id,
                        const JitInst* inst,
                        JitLirOpcode op,
                        uint32_t aux,
                        JitBackendStats* stats,
                        JitBackendError* error) {
    JitLirOperand* uses = NULL;
    uint32_t use_count = 0u;
    if(!map_uses(region, lir, inst, &uses, &use_count, error, block, inst_id)) return false;
    JitLirOperand def;
    JitLirOperand* defs = NULL;
    uint32_t def_count = 0u;
    if(inst->result != JIT_INVALID_VALUE) {
        def = map_value(lir, inst->result);
        if(def.kind != JIT_LIR_OPERAND_NONE) {
            defs = &def;
            def_count = 1u;
        }
    }
    JitLirStateId state = inst->resume_state == JIT_INVALID_STATE
                              ? JIT_INVALID_LIR_STATE
                              : (JitLirStateId)inst->resume_state;
    jit_lir_emit(lir,
                 block,
                 op,
                 defs,
                 def_count,
                 uses,
                 use_count,
                 inst->imm,
                 aux,
                 0,
                 0,
                 lir_flags_from_effects(inst->effects),
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_BLOCK,
                 state,
                 inst->source);
    free(uses);
    stats->native_ops++;
    return true;
}

static bool lower_instruction(const JitRegion* region,
                              JitLirFunction* lir,
                              JitLirBlockId block,
                              JitInstId inst_id,
                              const JitInst* inst,
                              JitBackendStats* stats,
                              JitBackendError* error) {
    if(inst->removed || inst->op == JIT_OP_NOP || inst->op == JIT_OP_PHI ||
       inst->op == JIT_OP_EXTRACT_TOKEN)
        return true;

    JitLirOpcode simple = simple_opcode(inst->op);
    if(simple != JIT_LIR__COUNT) {
        uint32_t aux = simple == JIT_LIR_CMP_I64 || simple == JIT_LIR_CMP_F64
                           ? (uint32_t)condition_for_mir(inst->op)
                           : inst->aux;
        return emit_simple(region, lir, block, inst_id, inst, simple, aux, stats, error);
    }

    JitLirOperand def;
    JitLirOperand use;
    JitLirStateId state;
    switch(inst->op) {
        case JIT_OP_VM:
        case JIT_OP_FRAME:
            def = map_value(lir, inst->result);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_INPUT,
                         &def,
                         1,
                         NULL,
                         0,
                         inst->imm,
                         inst->op == JIT_OP_VM ? 0u : 1u,
                         0,
                         0,
                         0,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            stats->native_ops++;
            return true;
        case JIT_OP_LOCAL:
        case JIT_OP_STACK_VALUE:
            def = map_value(lir, inst->result);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_VALUE_INPUT,
                         &def,
                         1,
                         NULL,
                         0,
                         inst->imm,
                         inst->op == JIT_OP_LOCAL ? 0u : 1u,
                         inst->aux,
                         0,
                         0,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            stats->native_ops++;
            return true;
        case JIT_OP_CONST_I64:
        case JIT_OP_CONST_BOOL:
            def = map_value(lir, inst->result);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_CONST_I64,
                         &def,
                         1,
                         NULL,
                         0,
                         inst->imm,
                         inst->op == JIT_OP_CONST_BOOL ? 1u : 0u,
                         0,
                         0,
                         0,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            stats->native_ops++;
            return true;
        case JIT_OP_CONST_F64:
            def = map_value(lir, inst->result);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_CONST_F64,
                         &def,
                         1,
                         NULL,
                         0,
                         inst->imm,
                         0,
                         0,
                         0,
                         0,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            stats->native_ops++;
            return true;
        case JIT_OP_CONST_NONE:
        case JIT_OP_CONST_NIL:
        case JIT_OP_CONST_ELLIPSIS:
        case JIT_OP_CONST_POOL:
            def = map_value(lir, inst->result);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_CONST_VALUE,
                         &def,
                         1,
                         NULL,
                         0,
                         inst->imm,
                         inst->aux,
                         0,
                         (uint32_t)inst->op,
                         0,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            stats->native_ops++;
            return true;

        case JIT_OP_JUMP:
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_JUMP,
                         NULL,
                         0,
                         NULL,
                         0,
                         imm_zero(),
                         0,
                         0,
                         0,
                         0,
                         (JitLirBlockId)inst->aux,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            return true;
        case JIT_OP_BRANCH: {
            const JitValueId* operands = jit_inst_operands(region, inst);
            if(inst->operands.count != 1u || operands == NULL)
                return backend_fail(error,
                                    JIT_BACKEND_INVALID_MIR,
                                    block,
                                    inst_id,
                                    "BRANCH must have exactly one condition operand");
            use = map_value(lir, operands[0]);
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_BRANCH,
                         NULL,
                         0,
                         &use,
                         1,
                         imm_zero(),
                         0,
                         0,
                         0,
                         0,
                         (JitLirBlockId)inst->imm.u32,
                         (JitLirBlockId)inst->aux,
                         JIT_INVALID_LIR_STATE,
                         inst->source);
            return true;
        }
        case JIT_OP_RETURN:
        case JIT_OP_YIELD: {
            JitLirOperand* uses = NULL;
            uint32_t use_count = 0u;
            if(!map_uses(region, lir, inst, &uses, &use_count, error, block, inst_id)) return false;
            jit_lir_emit(lir,
                         block,
                         inst->op == JIT_OP_RETURN ? JIT_LIR_RETURN : JIT_LIR_YIELD,
                         NULL,
                         0,
                         uses,
                         use_count,
                         inst->imm,
                         inst->aux,
                         0,
                         0,
                         lir_flags_from_effects(inst->effects),
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         inst->resume_state == JIT_INVALID_STATE ? JIT_INVALID_LIR_STATE
                                                                 : (JitLirStateId)inst->resume_state,
                         inst->source);
            free(uses);
            return true;
        }
        case JIT_OP_EXIT_INTERPRETER:
            state = (JitLirStateId)inst->aux;
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_EXIT_INTERPRETER,
                         NULL,
                         0,
                         NULL,
                         0,
                         inst->imm,
                         inst->aux,
                         0,
                         0,
                         JIT_LIR_FLAG_BARRIER,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         state,
                         inst->source);
            return true;
        case JIT_OP_ERROR:
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_ERROR,
                         NULL,
                         0,
                         NULL,
                         0,
                         inst->imm,
                         inst->aux,
                         0,
                         0,
                         JIT_LIR_FLAG_BARRIER,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         inst->resume_state == JIT_INVALID_STATE ? JIT_INVALID_LIR_STATE
                                                                 : (JitLirStateId)inst->resume_state,
                         inst->source);
            return true;
        case JIT_OP_SYNC_STATE: {
            state = (JitLirStateId)inst->aux;
            if(state >= (JitLirStateId)lir->states.length)
                return backend_fail(error,
                                    JIT_BACKEND_INVALID_MIR,
                                    block,
                                    inst_id,
                                    "SYNC_STATE references invalid state %u",
                                    state);
            const JitLirState* st = c11__at(JitLirState, &lir->states, state);
            uint32_t count = st->locals.count + st->stack.count;
            JitLirOperand* uses = NULL;
            if(count != 0u) {
                uses = malloc(sizeof(JitLirOperand) * count);
                if(uses == NULL)
                    return backend_fail(error,
                                        JIT_BACKEND_OUT_OF_MEMORY,
                                        block,
                                        inst_id,
                                        "out of memory lowering SYNC_STATE");
                const JitLirOperand* locals = jit_lir_state_locals(lir, st);
                const JitLirOperand* stack = jit_lir_state_stack(lir, st);
                if(st->locals.count != 0u)
                    memcpy(uses, locals, sizeof(JitLirOperand) * st->locals.count);
                if(st->stack.count != 0u)
                    memcpy(uses + st->locals.count,
                           stack,
                           sizeof(JitLirOperand) * st->stack.count);
            }
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_SYNC_STATE,
                         NULL,
                         0,
                         uses,
                         count,
                         imm_zero(),
                         0,
                         0,
                         0,
                         JIT_LIR_FLAG_BARRIER,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         state,
                         inst->source);
            free(uses);
            return true;
        }
        case JIT_OP_SAFEPOINT:
        case JIT_OP_POLL:
            jit_lir_emit(lir,
                         block,
                         inst->op == JIT_OP_SAFEPOINT ? JIT_LIR_SAFEPOINT : JIT_LIR_POLL,
                         NULL,
                         0,
                         NULL,
                         0,
                         inst->imm,
                         inst->aux,
                         0,
                         0,
                         lir_flags_from_effects(inst->effects),
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         inst->resume_state == JIT_INVALID_STATE ? JIT_INVALID_LIR_STATE
                                                                 : (JitLirStateId)inst->resume_state,
                         inst->source);
            return true;

        case JIT_OP_CALL_HELPER:
            return emit_runtime(region,
                                lir,
                                block,
                                inst_id,
                                inst,
                                JIT_LIR_RUNTIME_FRONTEND_HELPER,
                                inst->aux,
                                stats,
                                error);
        case JIT_OP_CALL:
        case JIT_OP_CALL_NATIVE: {
            JitLirOperand* uses = NULL;
            uint32_t use_count = 0u;
            if(!map_uses(region, lir, inst, &uses, &use_count, error, block, inst_id)) return false;
            JitLirOperand def = map_value(lir, inst->result);
            JitLirOperand* defs = def.kind == JIT_LIR_OPERAND_NONE ? NULL : &def;
            uint32_t def_count = defs == NULL ? 0u : 1u;
            jit_lir_emit(lir,
                         block,
                         JIT_LIR_CALL,
                         defs,
                         def_count,
                         uses,
                         use_count,
                         inst->imm,
                         inst->aux,
                         inst->op == JIT_OP_CALL_NATIVE ? 1u : 0u,
                         0,
                         lir_flags_from_effects(inst->effects) | JIT_LIR_FLAG_CALL,
                         JIT_INVALID_LIR_BLOCK,
                         JIT_INVALID_LIR_BLOCK,
                         inst->resume_state == JIT_INVALID_STATE ? JIT_INVALID_LIR_STATE
                                                                 : (JitLirStateId)inst->resume_state,
                         inst->source);
            free(uses);
            stats->runtime_ops++;
            return true;
        }

        case JIT_OP_SWITCH:
            return backend_fail(error,
                                JIT_BACKEND_UNSUPPORTED_MIR,
                                block,
                                inst_id,
                                "SWITCH is reserved in MIR but has no current frontend encoding");

        default:
            return emit_runtime(region,
                                lir,
                                block,
                                inst_id,
                                inst,
                                JIT_LIR_RUNTIME_MIR_OPCODE,
                                (uint32_t)inst->op,
                                stats,
                                error);
    }
}

static bool lower_phis(const JitRegion* region,
                       JitLirFunction* lir,
                       JitBackendStats* stats,
                       JitBackendError* error) {
    c11__foreach(JitBlock, &region->blocks, block) {
        c11__foreach(JitInstId, &block->insts, inst_id_ptr) {
            JitInstId inst_id = *inst_id_ptr;
            const JitInst* inst = jit_region_inst_const(region, inst_id);
            if(inst->removed || inst->op != JIT_OP_PHI) continue;
            if(inst->result == JIT_INVALID_VALUE)
                return backend_fail(error,
                                    JIT_BACKEND_PHI_ERROR,
                                    block->id,
                                    inst_id,
                                    "PHI has no result value");
            if(inst->operands.count != (uint32_t)block->predecessors.length)
                return backend_fail(error,
                                    JIT_BACKEND_PHI_ERROR,
                                    block->id,
                                    inst_id,
                                    "PHI operand count %u does not match predecessor count %d",
                                    inst->operands.count,
                                    block->predecessors.length);
            JitLirOperand dst = map_value(lir, inst->result);
            if(dst.kind != JIT_LIR_OPERAND_VREG)
                return backend_fail(error,
                                    JIT_BACKEND_PHI_ERROR,
                                    block->id,
                                    inst_id,
                                    "PHI result is not a virtual register");
            const JitValueId* incoming = jit_inst_operands(region, inst);
            for(uint32_t i = 0; i < inst->operands.count; i++) {
                JitBlockId pred = c11__getitem(JitBlockId, &block->predecessors, i);
                JitLirOperand src = map_value(lir, incoming[i]);
                if(src.kind == JIT_LIR_OPERAND_NONE)
                    return backend_fail(error,
                                        JIT_BACKEND_PHI_ERROR,
                                        block->id,
                                        inst_id,
                                        "PHI incoming value v%u has no LIR representation",
                                        incoming[i]);
                if(!jit_lir_add_edge_copy(lir, pred, block->id, dst, src))
                    return backend_fail(error,
                                        JIT_BACKEND_PHI_ERROR,
                                        block->id,
                                        inst_id,
                                        "failed to add edge copy for PHI");
                stats->phi_copies++;
            }
        }
    }
    return true;
}

static uint32_t count_lir_insts(const JitLirFunction* lir) {
    uint32_t count = 0u;
    c11__foreach(JitLirBlock, &lir->blocks, block) {
        c11__foreach(JitLirInst, &block->insts, inst) {
            if(!inst->removed && inst->op != JIT_LIR_NOP) count++;
        }
    }
    return count;
}

bool jit_lower_mir_to_lir(const JitRegion* region,
                          const JitBackendOptions* options_in,
                          JitLirFunction* out_lir,
                          JitBackendStats* out_stats,
                          JitBackendError* error) {
    if(region == NULL || out_lir == NULL)
        return backend_fail(error,
                            JIT_BACKEND_INVALID_ARGUMENT,
                            JIT_INVALID_BLOCK,
                            JIT_INVALID_INST,
                            "region and output LIR must be non-NULL");

    JitBackendOptions defaults;
    jit_backend_options_init(&defaults);
    const JitBackendOptions* options = options_in == NULL ? &defaults : options_in;
    JitBackendStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.mir_blocks = (uint32_t)region->blocks.length;
    stats.mir_insts = (uint32_t)region->insts.length;
    stats.states = (uint32_t)region->states.length;

    char verify_error[256];
    if(!jit_region_verify(region, verify_error, sizeof(verify_error)))
        return backend_fail(error,
                            JIT_BACKEND_INVALID_MIR,
                            JIT_INVALID_BLOCK,
                            JIT_INVALID_INST,
                            "MIR verification failed before backend lowering: %s",
                            verify_error);

    jit_lir_init(out_lir, options->tvalue_size, options->stack_alignment);

    c11__foreach(JitBlock, &region->blocks, block) {
        JitLirBlockId id = jit_lir_add_block(out_lir,
                                             block->id,
                                             block->frequency,
                                             map_block_flags(block->flags));
        if(id != block->id) {
            jit_lir_destroy(out_lir);
            return backend_fail(error,
                                JIT_BACKEND_INVALID_MIR,
                                block->id,
                                JIT_INVALID_INST,
                                "MIR/LIR block numbering diverged");
        }
    }
    out_lir->entry_block = region->entry_block;

    if(!precreate_vregs(region, out_lir, &stats, error) || !build_states(region, out_lir, error)) {
        jit_lir_destroy(out_lir);
        return false;
    }

    c11__foreach(JitBlock, &region->blocks, block) {
        c11__foreach(JitInstId, &block->insts, inst_id_ptr) {
            JitInstId inst_id = *inst_id_ptr;
            const JitInst* inst = jit_region_inst_const(region, inst_id);
            if(!lower_instruction(region, out_lir, block->id, inst_id, inst, &stats, error)) {
                jit_lir_destroy(out_lir);
                return false;
            }
        }
    }

    jit_lir_rebuild_cfg(out_lir);
    if(!lower_phis(region, out_lir, &stats, error)) {
        jit_lir_destroy(out_lir);
        return false;
    }

    uint32_t blocks_before = (uint32_t)out_lir->blocks.length;
    if(options->resolve_phis && !jit_lir_resolve_parallel_copies(out_lir,
                                                                 verify_error,
                                                                 sizeof(verify_error))) {
        jit_lir_destroy(out_lir);
        return backend_fail(error,
                            JIT_BACKEND_PHI_ERROR,
                            JIT_INVALID_BLOCK,
                            JIT_INVALID_INST,
                            "%s",
                            verify_error);
    }
    stats.critical_edges_split = (uint32_t)out_lir->blocks.length - blocks_before;

    if(options->run_peephole) {
        while(jit_lir_peephole(out_lir)) {
        }
    }
    jit_lir_rebuild_cfg(out_lir);

    if(options->verify && !jit_lir_verify(out_lir, verify_error, sizeof(verify_error))) {
        jit_lir_destroy(out_lir);
        return backend_fail(error,
                            JIT_BACKEND_VERIFY_FAILED,
                            JIT_INVALID_BLOCK,
                            JIT_INVALID_INST,
                            "%s",
                            verify_error);
    }

    stats.lir_blocks = (uint32_t)out_lir->blocks.length;
    stats.lir_insts = count_lir_insts(out_lir);
    if(out_stats != NULL) *out_stats = stats;
    if(error != NULL) {
        error->status = JIT_BACKEND_OK;
        error->mir_inst = JIT_INVALID_INST;
        error->mir_block = JIT_INVALID_BLOCK;
        error->message[0] = '\0';
    }
    return true;
}
