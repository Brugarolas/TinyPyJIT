#include "tinypy/jit/reg_frontend.h"
#include "tinypy/jit/optimizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unlike assert(), CHECK always evaluates its argument, including in release builds. */
#define CHECK(expr)                                                                  \
    do {                                                                             \
        if(!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                 \
        }                                                                            \
    } while(0)

static void code_init(RegCode* code, uint16_t nregs) {
    memset(code, 0, sizeof(*code));
    c11_vector__ctor(&code->code, sizeof(RegBytecode));
    c11_vector__ctor(&code->source_ip, sizeof(int32_t));
    code->nregs = nregs;
}

static void code_add(RegCode* code, RegBytecode ins) {
    c11_vector__push(RegBytecode, &code->code, ins);
    c11_vector__push(int32_t, &code->source_ip, code->code.length - 1);
}

static void code_destroy(RegCode* code) {
    c11_vector__dtor(&code->code);
    c11_vector__dtor(&code->source_ip);
}

static JitRegion lower(RegCode* code, JitRegFrontendStats* stats) {
    JitRegion region;
    JitRegLowerError error;
    bool ok = jit_lower_regcode_to_mir(code, NULL, NULL, &region, stats, &error);
    if(!ok) fprintf(stderr, "MIR lowering failed at %d: %s\n", error.reg_pc, error.message);
    CHECK(ok);
    char reason[256];
    CHECK(jit_region_verify(&region, reason, sizeof(reason)));
    for(int bi = 0; bi < region.blocks.length; bi++) {
        const JitBlock* block = c11__at(JitBlock, &region.blocks, bi);
        for(int i = 0; i < block->insts.length; i++) {
            const JitInst* inst = jit_region_inst_const(&region,
                c11__getitem(JitInstId, &block->insts, i));
            if(inst->op == JIT_OP_PHI) {
                CHECK(inst->operands.count == (uint32_t)block->predecessors.length);
            }
        }
    }
    JitOptimizer optimizer;
    jit_optimizer_init(&optimizer);
    jit_optimizer_add_default_pipeline(&optimizer);
    optimizer.verify_after_each_pass = true;
    bool optimized = jit_optimizer_run(&optimizer, &region, reason, sizeof(reason));
    if(!optimized) fprintf(stderr, "optimizer rejected MIR: %s\n", reason);
    CHECK(optimized);
    jit_optimizer_destroy(&optimizer);
    return region;
}

static void test_simple(void) {
    RegCode code;
    code_init(&code, 3);
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 0, 37));
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 1, 5));
    code_add(&code, RBC_ABC(RBC_ADD_INT, 2, 0, 1));
    code_add(&code, RBC_ABC(RBC_RETURN, 2, 0, 0));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.reachable_blocks == 1);
    CHECK(stats.phi_count == 3);
    CHECK(stats.side_exits == 0);
    jit_region_destroy(&region);
    code_destroy(&code);
}

static void test_loop_fused(void) {
    RegCode code;
    code_init(&code, 1);
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 0, 0));
    code_add(&code, RBC_ABC(RBC_CMP_I8_BRANCH, 0, 10, REG_CMP_LT | REG_CMP_BRANCH_TRUE));
    code_add(&code, RBC_ASBX(RBC_SUPER_DATA, 0, 2));
    code_add(&code, RBC_ABC(RBC_RETURN, 0, 0, 0));
    code_add(&code, RBC_ABC(RBC_ADD_I8, 0, 0, 1));
    code_add(&code, RBC_ASBX(RBC_JUMP, 0, -4));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.backedges == 1);
    CHECK(stats.poll_count == 1);
    CHECK(stats.reachable_blocks >= 4);
    CHECK(stats.side_exits == 0);
    bool seen_phi_with_loop = false;
    for(int bi=0;bi<region.blocks.length;bi++) {
        const JitBlock* b = c11__at(JitBlock, &region.blocks, bi);
        if(!(b->flags & JIT_BLOCK_LOOP_HEADER)) continue;
        for(int i=0;i<b->insts.length;i++) {
            const JitInst* inst=jit_region_inst_const(&region,c11__getitem(JitInstId,&b->insts,i));
            if(inst->op==JIT_OP_PHI && inst->operands.count==2) seen_phi_with_loop=true;
        }
    }
    CHECK(seen_phi_with_loop);
    jit_region_destroy(&region);
    code_destroy(&code);
}

static void test_diamond(void) {
    RegCode code;
    code_init(&code, 2);
    code_add(&code, RBC_ABC(RBC_LOAD_BOOL, 0, 1, 0));
    code_add(&code, RBC_ASBX(RBC_JUMP_IF_FALSE, 0, 3));
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 1, 7));
    code_add(&code, RBC_ASBX(RBC_JUMP, 0, 2));
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 1, 9));
    code_add(&code, RBC_ABC(RBC_RETURN, 1, 0, 0));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.reachable_blocks >= 4);
    jit_region_destroy(&region);
    code_destroy(&code);
}

static void test_fused_register_compare(void) {
    RegCode code;
    code_init(&code, 2);
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 0, 4));
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 1, 4));
    code_add(&code, RBC_ABC(RBC_CMP_BRANCH, 0, 1, REG_CMP_EQ | REG_CMP_BRANCH_TRUE));
    code_add(&code, RBC_ASBX(RBC_SUPER_DATA, 0, 2));
    code_add(&code, RBC_ABC(RBC_RETURN, 0, 0, 0));
    code_add(&code, RBC_ABC(RBC_RETURN, 1, 0, 0));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.side_exits == 0);
    CHECK(stats.reachable_blocks >= 3);
    jit_region_destroy(&region);
    code_destroy(&code);
}

static void test_const_pool(void) {
    RegCode code;
    code_init(&code, 1);
    code_add(&code, RBC_ABX(RBC_LOAD_CONST, 0, 0));
    code_add(&code, RBC_ABC(RBC_RETURN, 0, 0, 0));
    JitRegion region;
    JitRegLowerError error;
    CHECK(!jit_lower_regcode_to_mir(&code, NULL, NULL, &region, NULL, &error));
    CHECK(error.status == JIT_REG_LOWER_INVALID_BYTECODE);
    CodeObject source;
    memset(&source, 0, sizeof(source));
    c11_vector__ctor(&source.consts, sizeof(py_TValue));
    c11_vector__ctor(&source.codes_ex, sizeof(BytecodeEx));
    py_TValue value;
    memset(&value, 0, sizeof(value));
    c11_vector__push(py_TValue, &source.consts, value);
    JitRegFrontendStats stats;
    bool ok = jit_lower_regcode_to_mir(&code, &source, NULL, &region, &stats, &error);
    CHECK(ok);
    jit_region_destroy(&region);
    c11_vector__dtor(&source.consts);
    c11_vector__dtor(&source.codes_ex);
    code_destroy(&code);
}

static void test_unsupported_and_errors(void) {
    RegCode code;
    code_init(&code, 1);
    code_add(&code, RBC_ASBX(RBC_LOAD_I16, 0, 1));
    code_add(&code, RBC_ABC(RBC_CALL, 0, 0, 0));
    code_add(&code, RBC_ABC(RBC_RETURN, 0, 0, 0));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.side_exits == 1);
    CHECK(region.states.length >= 1);
    const JitState* state = c11__at(JitState, &region.states, 0);
    CHECK(state->bytecode_ip == 1);
    CHECK(state->locals.count == 1);
    jit_region_destroy(&region);
    code_destroy(&code);

    code_init(&code, 1);
    code_add(&code, RBC_ASBX(RBC_JUMP, 0, 2));
    code_add(&code, RBC_ABC(RBC_CMP_I8_BRANCH, 0, 3, REG_CMP_LT));
    code_add(&code, RBC_ASBX(RBC_SUPER_DATA, 0, 1));
    code_add(&code, RBC_ABC(RBC_RETURN_NONE, 0, 0, 0));
    JitRegLowerError err;
    CHECK(!jit_lower_regcode_to_mir(&code, NULL, NULL, &region, NULL, &err));
    CHECK(err.status == JIT_REG_LOWER_INVALID_BYTECODE);
    code_destroy(&code);
}


static void test_invalid_register(void) {
    RegCode code;
    code_init(&code, 2);
    code_add(&code, RBC_ABC(RBC_ADD, 9, 0, 1));
    code_add(&code, RBC_ABC(RBC_RETURN_NONE, 0, 0, 0));
    JitRegion region;
    JitRegLowerError err;
    CHECK(!jit_lower_regcode_to_mir(&code, NULL, NULL, &region, NULL, &err));
    CHECK(err.status == JIT_REG_LOWER_INVALID_BYTECODE);
    CHECK(err.reg_pc == 0);
    code_destroy(&code);
}

static void test_many_blocks(void) {
    RegCode code;
    code_init(&code, 8);
    for(int i = 0; i < 1024; i++) code_add(&code, RBC_ASBX(RBC_JUMP, 0, 1));
    code_add(&code, RBC_ABC(RBC_RETURN_NONE, 0, 0, 0));
    JitRegFrontendStats stats;
    JitRegion region = lower(&code, &stats);
    CHECK(stats.reachable_blocks == 1025);
    CHECK(stats.phi_count == 1025 * 8);
    jit_region_destroy(&region);
    code_destroy(&code);
}

int main(void) {
    test_simple();
    test_loop_fused();
    test_diamond();
    test_fused_register_compare();
    test_const_pool();
    test_unsupported_and_errors();
    test_invalid_register();
    test_many_blocks();
    puts("RegVM -> MIR tests passed");
    return 0;
}
