#include "tinypy/interpreter/jitbridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int compile_calls;
static int enter_function_calls;
static int enter_loop_calls;

static bool compile_cb(VM* vm, const RegJitCompileRequest* request, void* userdata) {
    (void)vm;
    assert(userdata == (void*)0x1234);
    assert(request->decl != NULL);
    assert(request->code != NULL);
    assert(request->profile.code == request->code);
    compile_calls++;
    return true;
}

static bool enter_function_cb(VM* vm,
                              py_Frame* frame,
                              RegCode* code,
                              RegJitExit* exit,
                              void* userdata) {
    (void)vm;
    (void)frame;
    (void)code;
    assert(userdata == (void*)0x1234);
    enter_function_calls++;
    exit->kind = REG_JIT_EXIT_RESUME;
    exit->resume_pc = 0;
    return true;
}

static bool enter_loop_cb(VM* vm,
                          py_Frame* frame,
                          RegCode* code,
                          int32_t header_pc,
                          RegJitExit* exit,
                          void* userdata) {
    (void)vm;
    (void)frame;
    (void)code;
    assert(header_pc == 0);
    assert(userdata == (void*)0x1234);
    enter_loop_calls++;
    exit->kind = REG_JIT_EXIT_RESUME;
    exit->resume_pc = 0;
    return true;
}

int main(void) {
    VM vm;
    memset(&vm, 0, sizeof(vm));
    RegJitHooks hooks = {compile_cb, enter_function_cb, enter_loop_cb};
    RegJit__set_hooks(&vm, &hooks, (void*)0x1234);

    FuncDecl decl;
    memset(&decl, 0, sizeof(decl));
    RegCode code;
    RegCode__ctor(&code);
    c11_vector__push(RegBytecode, &code.code, RBC_ABC(RBC_NOP, 0, 0, 0));
    c11_vector__push(int32_t, &code.source_ip, 7);
    RegRuntimeSite runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.cache_index = REG_IC_NONE;
    runtime.feedback_index = REG_PROFILE_NONE;
    runtime.loop_index = REG_PROFILE_NONE;
    c11_vector__push(RegRuntimeSite, &code.runtime_sites, runtime);

    code.function_hotness = PK_JIT_FUNCTION_THRESHOLD;
    assert(RegJit__request_function(&vm, &decl, &code));
    assert(code.function_jit_state == REG_JIT_COMPILED);

    py_Frame frame;
    memset(&frame, 0, sizeof(frame));
    RegJitExit exit;
    assert(RegJit__enter_function(&vm, &frame, &code, &exit));
    assert(exit.kind == REG_JIT_EXIT_RESUME && exit.resume_pc == 0);

    RegLoopProfile* loop = RegProfile__loop_backedge(&code, 0, 0, 7);
    assert(loop != NULL);
    loop->hotness = PK_JIT_LOOP_THRESHOLD;
    assert(RegJit__request_loop(&vm, &decl, &code, loop));
    assert(loop->state == REG_JIT_COMPILED);
    assert(RegJit__enter_loop(&vm, &frame, &code, 0, &exit));

    assert(compile_calls == 2);
    assert(enter_function_calls == 1);
    assert(enter_loop_calls == 1);

    RegJit__set_hooks(&vm, NULL, NULL);
    assert(!RegJit__enter_function(&vm, &frame, &code, &exit));

    RegCode__dtor(&code);
    puts("jit bridge smoke: ok");
    return 0;
}
