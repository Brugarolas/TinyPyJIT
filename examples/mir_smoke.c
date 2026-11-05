#include "tinypy/jit/backend.h"
#include "tinypy/jit/ir.h"
#include "tinypy/jit/optimizer.h"
#include "tinypy/jit/regalloc.h"
#include "tinypy/jit/trace.h"

#include <stdio.h>

static void dump_to_stdout(const char* text, void* context) {
    (void)context;
    fputs(text, stdout);
}

int main(void) {
    JitRegion region;
    jit_region_init(&region, JIT_REGION_FUNCTION, NULL, 0, -1);

    JitBlockId entry = jit_region_add_block(&region, 100, JIT_BLOCK_ENTRY);
    JitBuilder builder;
    jit_builder_init(&builder, &region, entry);

    JitValueId lhs = jit_builder_const_i64(&builder, 10);
    JitValueId rhs = jit_builder_const_i64(&builder, 20);
    JitValueId operands[2] = {lhs, rhs};
    JitImmediate no_imm = {0};

    JitValueId sum = jit_builder_emit_value(&builder,
                                            JIT_OP_ADD_I64,
                                            jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                            operands,
                                            2,
                                            no_imm,
                                            0);
    jit_builder_return(&builder, sum);

    jit_region_rebuild_cfg(&region);
    jit_region_recompute_uses(&region);

    char error[256];
    if(!jit_region_verify(&region, error, sizeof(error))) {
        fprintf(stderr, "MIR verification failed: %s\n", error);
        return 1;
    }

    JitOptimizer optimizer;
    jit_optimizer_init(&optimizer);
    jit_optimizer_add_default_pipeline(&optimizer);
    if(!jit_optimizer_run(&optimizer, &region, error, sizeof(error))) {
        fprintf(stderr, "MIR optimization failed: %s\n", error);
        return 2;
    }
    jit_region_dump(&region, dump_to_stdout, NULL);

    JitBackendOptions backend_options;
    jit_backend_options_init(&backend_options);
    JitBackendStats backend_stats;
    JitBackendError backend_error;
    JitLirFunction lir;
    if(!jit_lower_mir_to_lir(&region,
                             &backend_options,
                             &lir,
                             &backend_stats,
                             &backend_error)) {
        fprintf(stderr, "MIR -> LIR failed: %s\n", backend_error.message);
        return 3;
    }

    JitGraphColorConfig config;
    jit_graph_color_config_init(&config);
    config.gpr_scratch_count = 4;
    config.gpr_saved_count = 2;
    config.fpr_scratch_count = 4;
    config.fpr_saved_count = 2;

    JitRegAllocResult allocation;
    jit_regalloc_result_init(&allocation);
    if(!jit_graph_color_allocate(&lir, &config, &allocation, error, sizeof(error))) {
        fprintf(stderr, "Graph-color allocation failed: %s\n", error);
        return 4;
    }
    if(!jit_regalloc_apply(&lir, &allocation, error, sizeof(error))) {
        fprintf(stderr, "Physical allocation rewrite failed: %s\n", error);
        return 5;
    }
    if(!jit_lir_verify(&lir, error, sizeof(error))) {
        fprintf(stderr, "Final LIR verification failed: %s\n", error);
        return 6;
    }

    jit_lir_dump(&lir, dump_to_stdout, NULL);
    printf("graph-color: vregs=%u stack=%u spills=%u coalesced=%u\n",
           allocation.stats.final_vregs,
           allocation.stats.stack_bytes,
           allocation.stats.spilled_vregs,
           allocation.stats.coalesced_pairs);

    jit_regalloc_result_destroy(&allocation);
    jit_lir_destroy(&lir);
    jit_optimizer_destroy(&optimizer);
    jit_region_destroy(&region);
    return 0;
}
