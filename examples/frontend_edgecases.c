#include <stdio.h>
#include <string.h>
#include "pocketpy/jit/frontend.h"
#include "pocketpy/jit/backend.h"
#include "pocketpy/jit/regalloc.h"

static void init_code(CodeObject* co, int nlocals) {
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
static void destroy_code(CodeObject* co) {
    c11_vector__dtor(&co->codes); c11_vector__dtor(&co->codes_ex);
    c11_vector__dtor(&co->consts); c11_vector__dtor(&co->varnames);
    c11_vector__dtor(&co->names); c11_vector__dtor(&co->blocks);
    c11_vector__dtor(&co->func_decls);
}
static void emit(CodeObject* co, Opcode op, int arg) {
    Bytecode bc={(uint16_t)op,(uint16_t)arg}; BytecodeEx ex={1,-1};
    c11_vector__push(Bytecode,&co->codes,bc); c11_vector__push(BytecodeEx,&co->codes_ex,ex);
}
static int lower_ok(CodeObject* co, int loop_header) {
    JitFrontendOptions options; jit_frontend_options_init(&options);
    JitFrontendStats stats; JitLowerError error; JitRegion region;
    bool ok = loop_header < 0
        ? jit_lower_code_object(co,&options,&region,&stats,&error)
        : jit_lower_loop(co,loop_header,&options,&region,&stats,&error);
    if(!ok) { fprintf(stderr,"lower failed ip=%d: %s\n",error.bytecode_ip,error.message); return 0; }
    char verify[256];
    if(!jit_region_verify(&region,verify,sizeof(verify))) { fprintf(stderr,"verify: %s\n",verify); jit_region_destroy(&region); return 0; }
    JitBackendOptions backend_options; jit_backend_options_init(&backend_options);
    JitBackendStats backend_stats; JitBackendError backend_error; JitLirFunction lir;
    if(!jit_lower_mir_to_lir(&region,&backend_options,&lir,&backend_stats,&backend_error)) {
        fprintf(stderr,"backend: %s\n",backend_error.message); jit_region_destroy(&region); return 0;
    }
    JitGraphColorConfig config; jit_graph_color_config_init(&config);
    config.gpr_scratch_count=4; config.gpr_saved_count=2;
    config.fpr_scratch_count=4; config.fpr_saved_count=2;
    JitRegAllocResult alloc; jit_regalloc_result_init(&alloc);
    if(!jit_graph_color_allocate(&lir,&config,&alloc,verify,sizeof(verify)) ||
       !jit_regalloc_apply(&lir,&alloc,verify,sizeof(verify)) ||
       !jit_lir_verify(&lir,verify,sizeof(verify))) {
        fprintf(stderr,"backend allocation/verify: %s\n",verify);
        jit_regalloc_result_destroy(&alloc); jit_lir_destroy(&lir); jit_region_destroy(&region); return 0;
    }
    jit_regalloc_result_destroy(&alloc); jit_lir_destroy(&lir);
    jit_region_destroy(&region); return 1;
}

int main(void) {
    CodeObject co;

    /* Edge-dependent stack height: a or b. */
    init_code(&co,2);
    emit(&co,OP_LOAD_FAST,0);               /* 0 */
    emit(&co,OP_JUMP_IF_FALSE_OR_POP,3);    /* 1 -> 4 */
    emit(&co,OP_LOAD_FAST,1);               /* 2 */
    emit(&co,OP_JUMP_FORWARD,1);            /* 3 -> 4 */
    emit(&co,OP_RETURN_VALUE,BC_NOARG);      /* 4 */
    if(!lower_ok(&co,-1)) return 1;
    destroy_code(&co);

    /* Iterator loop: success adds an item, exhaustion removes the iterator. */
    init_code(&co,2);
    emit(&co,OP_LOAD_FAST,0);               /* 0 */
    emit(&co,OP_GET_ITER,0);                /* 1 */
    emit(&co,OP_FOR_ITER,4);                /* 2 -> 6 */
    emit(&co,OP_STORE_FAST,1);              /* 3 */
    emit(&co,OP_JUMP_FORWARD,-2);           /* 4 -> 2 */
    emit(&co,OP_NO_OP,0);                   /* 5 unreachable */
    emit(&co,OP_LOAD_NONE,0);               /* 6 */
    emit(&co,OP_RETURN_VALUE,BC_NOARG);      /* 7 */
    if(!lower_ok(&co,-1) || !lower_ok(&co,2)) return 2;
    destroy_code(&co);

    /* LOAD_METHOD has two logical outputs consumed by CALL. */
    init_code(&co,1);
    emit(&co,OP_LOAD_FAST,0);
    emit(&co,OP_LOAD_METHOD,0);
    emit(&co,OP_CALL,0);
    emit(&co,OP_RETURN_VALUE,BC_NOARG);
    if(!lower_ok(&co,-1)) return 3;
    destroy_code(&co);

    puts("frontend edge cases: ok");
    return 0;
}
