#include "pocketpy/jit/lir.h"
#include "pocketpy/jit/regalloc.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    JitLirFunction lir;
    jit_lir_init(&lir, 32, 16);
    JitLirBlockId b0 = jit_lir_add_block(&lir, JIT_INVALID_BLOCK, 100, JIT_LIR_BLOCK_ENTRY);
    JitLirBlockId b1 = jit_lir_add_block(&lir, JIT_INVALID_BLOCK, 100, 0);
    lir.entry_block = b0;
    JitTypeInfo type = jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
    JitVRegId a_id = jit_lir_new_vreg(&lir, JIT_VREG_GPR, type, JIT_INVALID_VALUE);
    JitVRegId b_id = jit_lir_new_vreg(&lir, JIT_VREG_GPR, type, JIT_INVALID_VALUE);
    JitLirOperand a = jit_lir_vreg(a_id, type);
    JitLirOperand b = jit_lir_vreg(b_id, type);
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    JitSourceLocation source = {0, 1};
    imm.i64 = 1;
    jit_lir_emit(&lir,b0,JIT_LIR_CONST_I64,&a,1,NULL,0,imm,0,0,0,0,
                 JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_STATE,source);
    imm.i64 = 2;
    jit_lir_emit(&lir,b0,JIT_LIR_CONST_I64,&b,1,NULL,0,imm,0,0,0,0,
                 JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_STATE,source);
    jit_lir_emit(&lir,b0,JIT_LIR_JUMP,NULL,0,NULL,0,imm,0,0,0,0,b1,
                 JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_STATE,source);
    jit_lir_emit(&lir,b1,JIT_LIR_RETURN,NULL,0,&a,1,imm,0,0,0,0,
                 JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_BLOCK,JIT_INVALID_LIR_STATE,source);
    jit_lir_rebuild_cfg(&lir);
    if(!jit_lir_add_edge_copy(&lir,b0,b1,a,b) || !jit_lir_add_edge_copy(&lir,b0,b1,b,a)) return 1;
    char error[256];
    uint32_t before = (uint32_t)lir.vregs.length;
    if(!jit_lir_resolve_parallel_copies(&lir,error,sizeof(error))) {
        fprintf(stderr,"parallel copy resolution failed: %s\n",error); return 2;
    }
    if((uint32_t)lir.vregs.length != before + 1u) {
        fprintf(stderr,"cycle did not allocate exactly one temporary\n"); return 3;
    }
    JitGraphColorConfig config; jit_graph_color_config_init(&config);
    config.gpr_scratch_count=2; config.gpr_saved_count=1;
    JitRegAllocResult alloc; jit_regalloc_result_init(&alloc);
    if(!jit_graph_color_allocate(&lir,&config,&alloc,error,sizeof(error)) ||
       !jit_regalloc_apply(&lir,&alloc,error,sizeof(error)) ||
       !jit_lir_verify(&lir,error,sizeof(error))) {
        fprintf(stderr,"cycle allocation failed: %s\n",error); return 4;
    }
    puts("parallel-copy cycle: ok");
    jit_regalloc_result_destroy(&alloc);
    jit_lir_destroy(&lir);
    return 0;
}
