#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketpy/jit/frontend.h"
#include "pocketpy/jit/optimizer.h"

static void dump_writer(const char* text, void* ctx) {
    (void)ctx;
    fputs(text, stdout);
}

static void code_init(CodeObject* co, int nlocals) {
    memset(co, 0, sizeof(*co));
    c11_vector__ctor(&co->codes, sizeof(Bytecode));
    c11_vector__ctor(&co->codes_ex, sizeof(BytecodeEx));
    c11_vector__ctor(&co->consts, 1);
    c11_vector__ctor(&co->varnames, 1);
    c11_vector__ctor(&co->names, 1);
    c11_vector__ctor(&co->blocks, 1);
    c11_vector__ctor(&co->func_decls, 1);
    co->nlocals = nlocals;
}

static void code_destroy(CodeObject* co) {
    c11_vector__dtor(&co->codes);
    c11_vector__dtor(&co->codes_ex);
    c11_vector__dtor(&co->consts);
    c11_vector__dtor(&co->varnames);
    c11_vector__dtor(&co->names);
    c11_vector__dtor(&co->blocks);
    c11_vector__dtor(&co->func_decls);
}

static void emit(CodeObject* co, Opcode op, int arg, int line) {
    Bytecode bc = {(uint16_t)op, (uint16_t)arg};
    BytecodeEx ex = {line, -1};
    c11_vector__push(Bytecode, &co->codes, bc);
    c11_vector__push(BytecodeEx, &co->codes_ex, ex);
}

int main(void) {
    CodeObject co;
    code_init(&co, 3);

    emit(&co, OP_LOAD_SMALL_INT, 0, 1);      /* 0 */
    emit(&co, OP_STORE_FAST, 1, 1);          /* 1 */
    emit(&co, OP_LOAD_SMALL_INT, 0, 2);      /* 2 */
    emit(&co, OP_STORE_FAST, 2, 2);          /* 3 */
    emit(&co, OP_LOAD_FAST, 1, 3);           /* 4 loop header */
    emit(&co, OP_LOAD_FAST, 0, 3);           /* 5 */
    emit(&co, OP_COMPARE_LT, 0, 3);          /* 6 */
    emit(&co, OP_POP_JUMP_IF_FALSE, 10, 3);  /* 7 -> 17 */
    emit(&co, OP_LOAD_FAST, 2, 4);           /* 8 */
    emit(&co, OP_LOAD_FAST, 1, 4);           /* 9 */
    emit(&co, OP_BINARY_ADD, 0, 4);          /* 10 */
    emit(&co, OP_STORE_FAST, 2, 4);          /* 11 */
    emit(&co, OP_LOAD_FAST, 1, 5);           /* 12 */
    emit(&co, OP_LOAD_SMALL_INT, 1, 5);      /* 13 */
    emit(&co, OP_BINARY_ADD, 0, 5);          /* 14 */
    emit(&co, OP_STORE_FAST, 1, 5);          /* 15 */
    emit(&co, OP_JUMP_FORWARD, -12, 5);      /* 16 -> 4 */
    emit(&co, OP_LOAD_FAST, 2, 6);           /* 17 */
    emit(&co, OP_RETURN_VALUE, BC_NOARG, 6); /* 18 */

    JitFrontendOptions options;
    jit_frontend_options_init(&options);

    JitFrontendStats stats;
    JitLowerError error;
    JitRegion function_region;
    if(!jit_lower_code_object(&co, &options, &function_region, &stats, &error)) {
        fprintf(stderr, "function lowering failed at %d: %s\n", error.bytecode_ip, error.message);
        code_destroy(&co);
        return 1;
    }

    printf("function: blocks=%u selected=%u phis=%u loops=%u max_stack=%u\n",
           stats.basic_block_count,
           stats.selected_block_count,
           stats.phi_count,
           stats.loop_count,
           stats.max_stack_depth);

    JitOptimizer optimizer;
    jit_optimizer_init(&optimizer);
    jit_optimizer_add_default_pipeline(&optimizer);
    char opt_error[256];
    if(!jit_optimizer_run(&optimizer, &function_region, opt_error, sizeof(opt_error))) {
        fprintf(stderr, "optimizer failed: %s\n", opt_error);
        jit_optimizer_destroy(&optimizer);
        jit_region_destroy(&function_region);
        code_destroy(&co);
        return 2;
    }
    jit_optimizer_destroy(&optimizer);

    jit_region_dump(&function_region, dump_writer, NULL);
    jit_region_destroy(&function_region);

    JitRegion loop_region;
    if(!jit_lower_loop(&co, 4, &options, &loop_region, &stats, &error)) {
        fprintf(stderr, "loop lowering failed at %d: %s\n", error.bytecode_ip, error.message);
        code_destroy(&co);
        return 3;
    }
    printf("loop: selected=%u exit_stubs=%u phis=%u loops=%u\n",
           stats.selected_block_count,
           stats.exit_stub_count,
           stats.phi_count,
           stats.loop_count);
    jit_region_dump(&loop_region, dump_writer, NULL);
    jit_region_destroy(&loop_region);

    code_destroy(&co);
    return 0;
}
