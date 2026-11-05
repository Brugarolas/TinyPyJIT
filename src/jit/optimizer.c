#include "tinypy/jit/optimizer.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool safe_add_i64(int64_t a, int64_t b, int64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, out);
#else
    if((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
#endif
}

static bool safe_sub_i64(int64_t a, int64_t b, int64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, out);
#else
    if((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
#endif
}

static bool safe_mul_i64(int64_t a, int64_t b, int64_t* out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    if(a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if(a == -1 && b == INT64_MIN) return false;
    if(b == -1 && a == INT64_MIN) return false;
    if(a > 0) {
        if(b > 0) {
            if(a > INT64_MAX / b) return false;
        } else if(b < INT64_MIN / a) {
            return false;
        }
    } else {
        if(b > 0) {
            if(a < INT64_MIN / b) return false;
        } else if(a != 0 && b < INT64_MAX / a) {
            return false;
        }
    }
    *out = a * b;
    return true;
#endif
}

static const JitInst* defining_inst(const JitRegion* region, JitValueId value) {
    if(value >= (JitValueId)region->values.length) return NULL;
    const JitValue* v = jit_region_value_const(region, value);
    if(v->def >= (JitInstId)region->insts.length) return NULL;
    const JitInst* inst = jit_region_inst_const(region, v->def);
    return inst->removed ? NULL : inst;
}

static bool const_i64(const JitRegion* region, JitValueId value, int64_t* out) {
    const JitInst* inst = defining_inst(region, value);
    if(inst == NULL || inst->op != JIT_OP_CONST_I64) return false;
    *out = inst->imm.i64;
    return true;
}

static bool const_f64(const JitRegion* region, JitValueId value, double* out) {
    const JitInst* inst = defining_inst(region, value);
    if(inst == NULL || inst->op != JIT_OP_CONST_F64) return false;
    *out = inst->imm.f64;
    return true;
}

static bool const_bool(const JitRegion* region, JitValueId value, bool* out) {
    const JitInst* inst = defining_inst(region, value);
    if(inst == NULL || inst->op != JIT_OP_CONST_BOOL) return false;
    *out = inst->imm.u32 != 0;
    return true;
}

static void make_const_i64(JitRegion* region, JitInst* inst, int64_t value) {
    inst->op = JIT_OP_CONST_I64;
    inst->type = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
    inst->operands.count = 0;
    inst->imm.i64 = value;
    inst->aux = 0;
    inst->effects = JIT_EFFECT_NONE;
    jit_region_value(region, inst->result)->type = inst->type;
}

static void make_const_f64(JitRegion* region, JitInst* inst, double value) {
    inst->op = JIT_OP_CONST_F64;
    inst->type = jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT);
    inst->operands.count = 0;
    inst->imm.f64 = value;
    inst->aux = 0;
    inst->effects = JIT_EFFECT_NONE;
    jit_region_value(region, inst->result)->type = inst->type;
}

static void make_const_bool(JitRegion* region, JitInst* inst, bool value) {
    inst->op = JIT_OP_CONST_BOOL;
    inst->type = jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL);
    inst->operands.count = 0;
    inst->imm.u32 = value ? 1u : 0u;
    inst->aux = 0;
    inst->effects = JIT_EFFECT_NONE;
    jit_region_value(region, inst->result)->type = inst->type;
}

static bool replace_and_remove(JitRegion* region, JitInst* inst, JitValueId replacement) {
    if(inst->result == JIT_INVALID_VALUE) return false;
    JitValueId old = inst->result;
    bool changed = jit_region_replace_all_uses(region, old, replacement);
    inst->removed = true;
    inst->op = JIT_OP_NOP;
    inst->operands.count = 0;
    inst->effects = JIT_EFFECT_NONE;
    return changed || old != replacement;
}

void jit_optimizer_init(JitOptimizer* optimizer) {
    c11_vector__ctor(&optimizer->passes, sizeof(JitOptimizationPass));
#ifndef NDEBUG
    optimizer->verify_after_each_pass = true;
#else
    optimizer->verify_after_each_pass = false;
#endif
    optimizer->max_rounds = 4;
}

void jit_optimizer_destroy(JitOptimizer* optimizer) {
    c11_vector__dtor(&optimizer->passes);
    memset(optimizer, 0, sizeof(*optimizer));
}

void jit_optimizer_add(JitOptimizer* optimizer, const char* name, JitOptimizationFn run) {
    assert(name != NULL);
    assert(run != NULL);
    JitOptimizationPass pass = {name, run};
    c11_vector__push(JitOptimizationPass, &optimizer->passes, pass);
}

void jit_optimizer_add_default_pipeline(JitOptimizer* optimizer) {
    jit_optimizer_add(optimizer, "canonicalize", jit_opt_canonicalize);
    jit_optimizer_add(optimizer, "type-propagation", jit_opt_type_propagation);
    jit_optimizer_add(optimizer, "constant-fold", jit_opt_constant_fold);
    jit_optimizer_add(optimizer, "peephole", jit_opt_peephole);
    jit_optimizer_add(optimizer, "phi-simplify", jit_opt_phi_simplify);
    jit_optimizer_add(optimizer, "gvn", jit_opt_gvn);
    jit_optimizer_add(optimizer, "branch-fold", jit_opt_branch_fold);
    jit_optimizer_add(optimizer, "dce", jit_opt_dce);
}

bool jit_optimizer_run(JitOptimizer* optimizer, JitRegion* region, char* error, uint32_t error_size) {
    if(!jit_region_verify(region, error, error_size)) return false;

    uint32_t max_rounds = optimizer->max_rounds == 0 ? 1 : optimizer->max_rounds;
    for(uint32_t round = 0; round < max_rounds; round++) {
        bool round_changed = false;
        c11__foreach(JitOptimizationPass, &optimizer->passes, pass) {
            bool changed = pass->run(region);
            round_changed |= changed;
            if(optimizer->verify_after_each_pass) {
                jit_region_rebuild_cfg(region);
                jit_region_recompute_uses(region);
                if(!jit_region_verify(region, error, error_size)) {
                    if(error != NULL && error_size > 32) {
                        char original[256];
                        strncpy(original, error, sizeof(original) - 1);
                        original[sizeof(original) - 1] = '\0';
                        snprintf(error, error_size, "after %s: %s", pass->name, original);
                        error[error_size - 1] = '\0';
                    }
                    return false;
                }
            }
        }
        if(!round_changed) break;
    }

    jit_region_rebuild_cfg(region);
    jit_region_recompute_uses(region);
    return jit_region_verify(region, error, error_size);
}

bool jit_opt_canonicalize(JitRegion* region) {
    bool changed = false;
    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed) continue;
        if(inst->op == JIT_OP_BRANCH && inst->imm.u32 == inst->aux) {
            inst->op = JIT_OP_JUMP;
            inst->operands.count = 0;
            inst->aux = inst->imm.u32;
            inst->imm.u64 = 0;
            inst->effects = JIT_EFFECT_NONE;
            changed = true;
        }
    }
    if(changed) jit_region_rebuild_cfg(region);
    return changed;
}

static JitTypeInfo merge_type(JitTypeInfo a, JitTypeInfo b) {
    if(jit_type_equal(a, b)) return a;

    JitTypeInfo out = jit_type_value();
    if(a.representation == b.representation) out.representation = a.representation;
    if(a.known_type == b.known_type) out.known_type = a.known_type;
    out.exact = a.exact && b.exact && a.known_type == b.known_type;
    out.nullable = a.nullable || b.nullable;
    return out;
}

bool jit_opt_type_propagation(JitRegion* region) {
    bool any_changed = false;
    bool changed;
    uint32_t iterations = 0;

    do {
        changed = false;
        c11__foreach(JitInst, &region->insts, inst) {
            if(inst->removed || inst->result == JIT_INVALID_VALUE) continue;
            JitTypeInfo next = inst->type;
            const JitValueId* ops = jit_inst_operands(region, inst);

            switch(inst->op) {
                case JIT_OP_CONST_NONE:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_NONE);
                    break;
                case JIT_OP_CONST_NIL:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_NIL);
                    break;
                case JIT_OP_CONST_ELLIPSIS:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_ELLIPSIS);
                    break;
                case JIT_OP_CONST_BOOL:
                    next = jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL);
                    break;
                case JIT_OP_CONST_I64:
                    next = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
                    break;
                case JIT_OP_CONST_F64:
                    next = jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT);
                    break;
                case JIT_OP_LOAD_LOCAL:
                    if(inst->operands.count == 1 && ops[0] != JIT_INVALID_VALUE &&
                       ops[0] < (JitValueId)region->values.length)
                        next = c11__getitem(JitValue, &region->values, ops[0]).type;
                    break;
                case JIT_OP_BOX_I64:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_INT);
                    break;
                case JIT_OP_BOX_F64:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_FLOAT);
                    break;
                case JIT_OP_BOX_BOOL:
                    next = jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_BOOL);
                    break;
                case JIT_OP_UNBOX_I64:
                    next = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
                    break;
                case JIT_OP_UNBOX_F64:
                    next = jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT);
                    break;
                case JIT_OP_UNBOX_BOOL:
                case JIT_OP_IS_TYPE:
                case JIT_OP_TRUTHY:
                case JIT_OP_IS_IDENTICAL:
                case JIT_OP_NOT_BOOL:
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
                    next = jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL);
                    break;
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
                    next = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
                    break;
                case JIT_OP_ADD_F64:
                case JIT_OP_SUB_F64:
                case JIT_OP_MUL_F64:
                case JIT_OP_DIV_F64:
                case JIT_OP_NEG_F64:
                    next = jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT);
                    break;
                case JIT_OP_PHI:
                    if(inst->operands.count != 0) {
                        next = jit_region_value_const(region, ops[0])->type;
                        for(uint32_t i = 1; i < inst->operands.count; i++)
                            next = merge_type(next, jit_region_value_const(region, ops[i])->type);
                    }
                    break;
                default: break;
            }

            JitValue* value = jit_region_value(region, inst->result);
            if(!jit_type_equal(value->type, next)) {
                value->type = next;
                inst->type = next;
                changed = true;
                any_changed = true;
            }
        }
        iterations++;
    } while(changed && iterations < 8);

    return any_changed;
}

bool jit_opt_constant_fold(JitRegion* region) {
    bool changed = false;
    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed || inst->result == JIT_INVALID_VALUE) continue;
        const JitValueId* ops = jit_inst_operands(region, inst);
        if(inst->operands.count == 2) {
            int64_t ai, bi, ri;
            double af, bf;
            bool folded = false;

            if(const_i64(region, ops[0], &ai) && const_i64(region, ops[1], &bi)) {
                switch(inst->op) {
                    case JIT_OP_ADD_I64:
                        if(safe_add_i64(ai, bi, &ri)) {
                            make_const_i64(region, inst, ri);
                            folded = true;
                        }
                        break;
                    case JIT_OP_SUB_I64:
                        if(safe_sub_i64(ai, bi, &ri)) {
                            make_const_i64(region, inst, ri);
                            folded = true;
                        }
                        break;
                    case JIT_OP_MUL_I64:
                        if(safe_mul_i64(ai, bi, &ri)) {
                            make_const_i64(region, inst, ri);
                            folded = true;
                        }
                        break;
                    case JIT_OP_DIV_I64:
                        if(bi != 0 && !(ai == INT64_MIN && bi == -1)) {
                            make_const_i64(region, inst, ai / bi);
                            folded = true;
                        }
                        break;
                    case JIT_OP_MOD_I64:
                        if(bi != 0 && !(ai == INT64_MIN && bi == -1)) {
                            make_const_i64(region, inst, ai % bi);
                            folded = true;
                        }
                        break;
                    case JIT_OP_AND_I64: make_const_i64(region, inst, ai & bi); folded = true; break;
                    case JIT_OP_OR_I64: make_const_i64(region, inst, ai | bi); folded = true; break;
                    case JIT_OP_XOR_I64: make_const_i64(region, inst, ai ^ bi); folded = true; break;
                    case JIT_OP_EQ_I64: make_const_bool(region, inst, ai == bi); folded = true; break;
                    case JIT_OP_NE_I64: make_const_bool(region, inst, ai != bi); folded = true; break;
                    case JIT_OP_LT_I64: make_const_bool(region, inst, ai < bi); folded = true; break;
                    case JIT_OP_LE_I64: make_const_bool(region, inst, ai <= bi); folded = true; break;
                    case JIT_OP_GT_I64: make_const_bool(region, inst, ai > bi); folded = true; break;
                    case JIT_OP_GE_I64: make_const_bool(region, inst, ai >= bi); folded = true; break;
                    default: break;
                }
            } else if(const_f64(region, ops[0], &af) && const_f64(region, ops[1], &bf)) {
                switch(inst->op) {
                    case JIT_OP_ADD_F64: make_const_f64(region, inst, af + bf); folded = true; break;
                    case JIT_OP_SUB_F64: make_const_f64(region, inst, af - bf); folded = true; break;
                    case JIT_OP_MUL_F64: make_const_f64(region, inst, af * bf); folded = true; break;
                    case JIT_OP_DIV_F64:
                        if(bf != 0.0) {
                            make_const_f64(region, inst, af / bf);
                            folded = true;
                        }
                        break;
                    case JIT_OP_EQ_F64: make_const_bool(region, inst, af == bf); folded = true; break;
                    case JIT_OP_NE_F64: make_const_bool(region, inst, af != bf); folded = true; break;
                    case JIT_OP_LT_F64: make_const_bool(region, inst, af < bf); folded = true; break;
                    case JIT_OP_LE_F64: make_const_bool(region, inst, af <= bf); folded = true; break;
                    case JIT_OP_GT_F64: make_const_bool(region, inst, af > bf); folded = true; break;
                    case JIT_OP_GE_F64: make_const_bool(region, inst, af >= bf); folded = true; break;
                    default: break;
                }
            }
            changed |= folded;
        } else if(inst->operands.count == 1) {
            int64_t ai;
            double af;
            if(const_i64(region, ops[0], &ai)) {
                if(inst->op == JIT_OP_NEG_I64 && ai != INT64_MIN) {
                    make_const_i64(region, inst, -ai);
                    changed = true;
                } else if(inst->op == JIT_OP_NOT_I64) {
                    make_const_i64(region, inst, ~ai);
                    changed = true;
                }
            } else if(const_f64(region, ops[0], &af) && inst->op == JIT_OP_NEG_F64) {
                make_const_f64(region, inst, -af);
                changed = true;
            }
        }
    }
    if(changed) jit_region_recompute_uses(region);
    return changed;
}

bool jit_opt_peephole(JitRegion* region) {
    bool changed = false;
    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed || inst->result == JIT_INVALID_VALUE || inst->operands.count != 2) continue;
        const JitValueId* ops = jit_inst_operands(region, inst);
        int64_t rhs;
        if(!const_i64(region, ops[1], &rhs)) continue;

        switch(inst->op) {
            case JIT_OP_ADD_I64:
            case JIT_OP_SUB_I64:
            case JIT_OP_OR_I64:
            case JIT_OP_XOR_I64:
                if(rhs == 0) changed |= replace_and_remove(region, inst, ops[0]);
                break;
            case JIT_OP_MUL_I64:
                if(rhs == 1) changed |= replace_and_remove(region, inst, ops[0]);
                break;
            case JIT_OP_AND_I64:
                if(rhs == -1) changed |= replace_and_remove(region, inst, ops[0]);
                break;
            default: break;
        }
    }
    return changed;
}

bool jit_opt_phi_simplify(JitRegion* region) {
    bool changed = false;
    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed || inst->op != JIT_OP_PHI || inst->result == JIT_INVALID_VALUE ||
           inst->operands.count == 0)
            continue;
        const JitValueId* ops = jit_inst_operands(region, inst);
        JitValueId candidate = JIT_INVALID_VALUE;
        bool same = true;
        for(uint32_t i = 0; i < inst->operands.count; i++) {
            if(ops[i] == inst->result) continue;
            if(candidate == JIT_INVALID_VALUE)
                candidate = ops[i];
            else if(ops[i] != candidate) {
                same = false;
                break;
            }
        }
        if(same && candidate != JIT_INVALID_VALUE)
            changed |= replace_and_remove(region, inst, candidate);
    }
    return changed;
}

static bool same_expression(const JitRegion* region, const JitInst* a, const JitInst* b) {
    if(a->op != b->op || a->operands.count != b->operands.count || a->aux != b->aux ||
       !jit_type_equal(a->type, b->type))
        return false;
    if(memcmp(&a->imm, &b->imm, sizeof(a->imm)) != 0) return false;
    const JitValueId* ao = jit_inst_operands(region, a);
    const JitValueId* bo = jit_inst_operands(region, b);
    for(uint32_t i = 0; i < a->operands.count; i++) {
        if(ao[i] != bo[i]) return false;
    }
    return true;
}

bool jit_opt_gvn(JitRegion* region) {
    bool changed = false;
    /* Deliberately local and quadratic. Regions are small and this keeps compile latency tiny. */
    c11__foreach(JitBlock, &region->blocks, block) {
        for(int i = 0; i < block->insts.length; i++) {
            JitInst* inst = jit_region_inst(region, c11__getitem(JitInstId, &block->insts, i));
            if(inst->removed || inst->result == JIT_INVALID_VALUE || !jit_opcode_is_pure(inst->op) ||
               inst->op == JIT_OP_PHI)
                continue;
            for(int j = 0; j < i; j++) {
                JitInst* prev = jit_region_inst(region, c11__getitem(JitInstId, &block->insts, j));
                if(prev->removed || prev->result == JIT_INVALID_VALUE ||
                   !jit_opcode_is_pure(prev->op))
                    continue;
                if(same_expression(region, inst, prev)) {
                    changed |= replace_and_remove(region, inst, prev->result);
                    break;
                }
            }
        }
    }
    return changed;
}

bool jit_opt_branch_fold(JitRegion* region) {
    bool changed = false;
    c11__foreach(JitInst, &region->insts, inst) {
        if(inst->removed || inst->op != JIT_OP_BRANCH || inst->operands.count != 1) continue;
        const JitValueId* ops = jit_inst_operands(region, inst);
        bool condition;
        if(!const_bool(region, ops[0], &condition)) continue;
        JitBlockId target = condition ? inst->imm.u32 : inst->aux;
        inst->op = JIT_OP_JUMP;
        inst->operands.count = 0;
        inst->imm.u64 = 0;
        inst->aux = target;
        inst->effects = JIT_EFFECT_NONE;
        changed = true;
    }
    if(changed) {
        jit_region_rebuild_cfg(region);
        jit_region_recompute_uses(region);
    }
    return changed;
}

bool jit_opt_dce(JitRegion* region) {
    bool any_changed = false;
    bool changed;
    do {
        changed = false;
        jit_region_recompute_uses(region);
        c11__foreach(JitInst, &region->insts, inst) {
            if(inst->removed || inst->result == JIT_INVALID_VALUE || !jit_opcode_is_pure(inst->op))
                continue;
            const JitValue* value = jit_region_value_const(region, inst->result);
            if(value->use_count == 0) {
                inst->removed = true;
                inst->op = JIT_OP_NOP;
                inst->operands.count = 0;
                inst->effects = JIT_EFFECT_NONE;
                changed = true;
                any_changed = true;
            }
        }
    } while(changed);

    if(any_changed) jit_region_recompute_uses(region);
    return any_changed;
}
