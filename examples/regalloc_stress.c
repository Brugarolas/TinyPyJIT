#include "pocketpy/jit/lir.h"
#include "pocketpy/jit/regalloc.h"

#include <stdio.h>
#include <string.h>

static JitImmediate i64_imm(int64_t value) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    imm.i64 = value;
    return imm;
}

int main(void) {
    JitLirFunction lir;
    jit_lir_init(&lir, 32, 16);
    JitLirBlockId entry = jit_lir_add_block(&lir, JIT_INVALID_BLOCK, 100, JIT_LIR_BLOCK_ENTRY);
    lir.entry_block = entry;
    JitTypeInfo i64_type = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
    JitSourceLocation source = {0, 1};

    JitLirOperand values[12];
    for(uint32_t i = 0; i < 12u; i++) {
        JitVRegId id = jit_lir_new_vreg(&lir, JIT_VREG_GPR, i64_type, JIT_INVALID_VALUE);
        values[i] = jit_lir_vreg(id, i64_type);
        jit_lir_emit(&lir,
                     entry,
                     JIT_LIR_CONST_I64,
                     &values[i],
                     1,
                     NULL,
                     0,
                     i64_imm((int64_t)i + 1),
                     0,
                     0,
                     0,
                     0,
                     JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_STATE,
                     source);
    }

    jit_lir_emit(&lir,
                 entry,
                 JIT_LIR_CALL,
                 NULL,
                 0,
                 NULL,
                 0,
                 i64_imm(0),
                 0,
                 0,
                 0,
                 JIT_LIR_FLAG_CALL,
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_STATE,
                 source);

    JitLirOperand acc = values[0];
    for(uint32_t i = 1; i < 12u; i++) {
        JitVRegId id = jit_lir_new_vreg(&lir, JIT_VREG_GPR, i64_type, JIT_INVALID_VALUE);
        JitLirOperand next = jit_lir_vreg(id, i64_type);
        JitLirOperand uses[2] = {acc, values[i]};
        jit_lir_emit(&lir,
                     entry,
                     JIT_LIR_ADD_I64,
                     &next,
                     1,
                     uses,
                     2,
                     i64_imm(0),
                     0,
                     0,
                     0,
                     0,
                     JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_BLOCK,
                     JIT_INVALID_LIR_STATE,
                     source);
        acc = next;
    }
    jit_lir_emit(&lir,
                 entry,
                 JIT_LIR_RETURN,
                 NULL,
                 0,
                 &acc,
                 1,
                 i64_imm(0),
                 0,
                 0,
                 0,
                 0,
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_BLOCK,
                 JIT_INVALID_LIR_STATE,
                 source);
    jit_lir_rebuild_cfg(&lir);

    char error[256];
    if(!jit_lir_verify(&lir, error, sizeof(error))) {
        fprintf(stderr, "pre-allocation verify failed: %s\n", error);
        return 1;
    }

    JitGraphColorConfig config;
    jit_graph_color_config_init(&config);
    config.gpr_scratch_count = 2;
    config.gpr_saved_count = 2;
    config.fpr_scratch_count = 2;
    config.fpr_saved_count = 1;
    config.max_spill_rounds = 8;

    JitRegAllocResult result;
    jit_regalloc_result_init(&result);
    if(!jit_graph_color_allocate(&lir, &config, &result, error, sizeof(error))) {
        fprintf(stderr, "stress allocation failed: %s\n", error);
        return 2;
    }
    if(result.stats.spilled_vregs == 0u) {
        fprintf(stderr, "stress test expected spills\n");
        return 3;
    }
    if(!jit_regalloc_apply(&lir, &result, error, sizeof(error))) {
        fprintf(stderr, "stress apply failed: %s\n", error);
        return 4;
    }
    if(!jit_lir_verify(&lir, error, sizeof(error))) {
        fprintf(stderr, "post-allocation verify failed: %s\n", error);
        return 5;
    }

    printf("stress: spills=%u rounds=%u loads=%u stores=%u stack=%u bytes final_vregs=%u\n",
           result.stats.spilled_vregs,
           result.stats.spill_rounds,
           result.stats.spill_loads,
           result.stats.spill_stores,
           result.stats.stack_bytes,
           result.stats.final_vregs);

    jit_regalloc_result_destroy(&result);
    jit_lir_destroy(&lir);
    return 0;
}
