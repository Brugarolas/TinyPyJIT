#include "tinypy/jit/backend.h"
#include "tinypy/jit/regalloc.h"

#include <stdio.h>

static void dump_stdout(const char* text, void* ctx) {
    (void)ctx;
    fputs(text, stdout);
}

int main(void) {
    JitRegion region;
    jit_region_init(&region, JIT_REGION_FUNCTION, NULL, 0, -1);
    JitBlockId b0 = jit_region_add_block(&region, 100, JIT_BLOCK_ENTRY);
    JitBlockId b1 = jit_region_add_block(&region, 40, 0);
    JitBlockId b2 = jit_region_add_block(&region, 100, 0);

    JitBuilder builder;
    jit_builder_init(&builder, &region, b0);
    JitImmediate imm = {0};
    JitValueId cond = jit_builder_emit_value(&builder,
                                             JIT_OP_LOCAL,
                                             jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                             NULL,
                                             0,
                                             imm,
                                             0);
    JitValueId a = jit_builder_const_i64(&builder, 10);
    jit_builder_branch(&builder, cond, b2, b1);

    jit_builder_set_block(&builder, b1);
    JitValueId b = jit_builder_const_i64(&builder, 20);
    jit_builder_jump(&builder, b2);

    /* Build predecessor order before constructing the PHI. */
    jit_region_rebuild_cfg(&region);
    if(region.blocks.length != 3 || jit_region_block(&region, b2)->predecessors.length != 2) return 1;

    jit_builder_set_block(&builder, b2);
    JitValueId incoming[2];
    for(int i = 0; i < jit_region_block(&region, b2)->predecessors.length; i++) {
        JitBlockId pred = c11__getitem(JitBlockId, &jit_region_block(&region, b2)->predecessors, i);
        incoming[i] = pred == b0 ? a : b;
    }
    JitValueId phi = jit_builder_emit_value(&builder,
                                            JIT_OP_PHI,
                                            jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                            incoming,
                                            2,
                                            imm,
                                            0);
    jit_builder_return(&builder, phi);
    jit_region_rebuild_cfg(&region);
    jit_region_recompute_uses(&region);

    char error[256];
    if(!jit_region_verify(&region, error, sizeof(error))) {
        fprintf(stderr, "MIR verify failed: %s\n", error);
        return 2;
    }

    JitBackendOptions options;
    jit_backend_options_init(&options);
    JitBackendStats stats;
    JitBackendError backend_error;
    JitLirFunction lir;
    if(!jit_lower_mir_to_lir(&region, &options, &lir, &stats, &backend_error)) {
        fprintf(stderr, "backend failed: %s\n", backend_error.message);
        return 3;
    }
    if(stats.phi_copies != 2u || stats.critical_edges_split != 1u) {
        fprintf(stderr,
                "expected 2 phi copies and 1 critical-edge split, got %u/%u\n",
                stats.phi_copies,
                stats.critical_edges_split);
        return 4;
    }

    JitGraphColorConfig config;
    jit_graph_color_config_init(&config);
    config.gpr_scratch_count = 2;
    config.gpr_saved_count = 1;
    JitRegAllocResult alloc;
    jit_regalloc_result_init(&alloc);
    if(!jit_graph_color_allocate(&lir, &config, &alloc, error, sizeof(error))) {
        fprintf(stderr, "allocation failed: %s\n", error);
        return 5;
    }
    if(!jit_regalloc_apply(&lir, &alloc, error, sizeof(error))) {
        fprintf(stderr, "apply failed: %s\n", error);
        return 6;
    }
    if(!jit_lir_verify(&lir, error, sizeof(error))) {
        fprintf(stderr, "final verify failed: %s\n", error);
        return 7;
    }
    jit_lir_dump(&lir, dump_stdout, NULL);

    jit_regalloc_result_destroy(&alloc);
    jit_lir_destroy(&lir);
    jit_region_destroy(&region);
    return 0;
}
