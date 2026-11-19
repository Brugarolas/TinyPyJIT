#include "tinypy/interpreter/jitbridge.h"

#include <string.h>

void RegJit__set_hooks(VM* vm, const RegJitHooks* hooks, void* userdata) {
    if(vm == NULL) return;
    vm->regjit_hooks = hooks;
    vm->regjit_userdata = userdata;
}

void RegJit__force_frame(VM* vm, py_Frame* frame) {
    if(vm == NULL || frame == NULL || vm->regjit_hooks == NULL || vm->regjit_hooks->force == NULL)
        return;
    vm->regjit_hooks->force(vm, frame, vm->regjit_userdata);
}

void RegJit__release_code(VM* vm, RegCode* code) {
    if(code == NULL || code->jit_code == NULL) return;
    if(vm != NULL && vm->regjit_hooks != NULL && vm->regjit_hooks->release != NULL) {
        vm->regjit_hooks->release(vm, code, vm->regjit_userdata);
    }
    code->jit_code = NULL;
}

bool RegJit__request_function(VM* vm, FuncDecl* decl, RegCode* code) {
    if(vm == NULL || decl == NULL || code == NULL || vm->regjit_hooks == NULL ||
       vm->regjit_hooks->compile == NULL)
        return false;
    if(code->function_jit_state != (uint8_t)REG_JIT_COLD ||
       code->function_hotness < PK_JIT_FUNCTION_THRESHOLD)
        return code->function_jit_state == (uint8_t)REG_JIT_COMPILED;

    RegJitCompileRequest request;
    memset(&request, 0, sizeof(request));
    request.kind = REG_JIT_COMPILE_FUNCTION;
    request.decl = decl;
    request.code = code;
    request.loop_header_pc = -1;
    request.loop_header_source_ip = -1;
    RegProfile__view(code, &request.profile);

    code->function_jit_state = (uint8_t)REG_JIT_REQUESTED;
    bool installed = vm->regjit_hooks->compile(vm, &request, vm->regjit_userdata);
    code->function_jit_state = (uint8_t)(installed ? REG_JIT_COMPILED : REG_JIT_DISABLED);
    return installed;
}

bool RegJit__request_loop(VM* vm, FuncDecl* decl, RegCode* code, RegLoopProfile* loop) {
    if(vm == NULL || decl == NULL || code == NULL || loop == NULL || vm->regjit_hooks == NULL ||
       vm->regjit_hooks->compile == NULL)
        return false;
    if(loop->state != REG_JIT_COLD || loop->hotness < PK_JIT_LOOP_THRESHOLD)
        return loop->state == REG_JIT_COMPILED;

    RegJitCompileRequest request;
    memset(&request, 0, sizeof(request));
    request.kind = REG_JIT_COMPILE_LOOP;
    request.decl = decl;
    request.code = code;
    request.loop_header_pc = loop->header_pc;
    request.loop_header_source_ip = loop->source_ip;
    RegProfile__view(code, &request.profile);

    loop->state = REG_JIT_REQUESTED;
    bool installed = vm->regjit_hooks->compile(vm, &request, vm->regjit_userdata);
    loop->state = installed ? REG_JIT_COMPILED : REG_JIT_DISABLED;
    /*
     * A compile request always produces code for the whole function, so the
     * function entry is available as well. This also ends per-op profiling for
     * this code (it only runs while the function is still cold).
     */
    if(code->function_jit_state == (uint8_t)REG_JIT_COLD) {
        code->function_jit_state = (uint8_t)(installed ? REG_JIT_COMPILED : REG_JIT_DISABLED);
    }
    return installed;
}

bool RegJit__enter_function(VM* vm,
                            py_Frame* frame,
                            RegCode* code,
                            RegJitExit* exit) {
    if(vm == NULL || frame == NULL || code == NULL || exit == NULL ||
       code->function_jit_state != (uint8_t)REG_JIT_COMPILED || vm->regjit_hooks == NULL ||
       vm->regjit_hooks->enter_function == NULL)
        return false;
    exit->kind = REG_JIT_EXIT_RESUME;
    exit->resume_pc = 0;
    exit->disable = false;
    bool ran = vm->regjit_hooks->enter_function(vm, frame, code, exit, vm->regjit_userdata);
    if(!ran && exit->disable) code->function_jit_state = (uint8_t)REG_JIT_DISABLED;
    return ran;
}

bool RegJit__enter_loop(VM* vm,
                        py_Frame* frame,
                        RegCode* code,
                        int32_t header_pc,
                        RegJitExit* exit) {
    if(vm == NULL || frame == NULL || code == NULL || exit == NULL || vm->regjit_hooks == NULL ||
       vm->regjit_hooks->enter_loop == NULL)
        return false;
    RegLoopProfile* loop = (RegLoopProfile*)RegProfile__loop(code, header_pc);
    if(loop == NULL || loop->state != REG_JIT_COMPILED) return false;
    exit->kind = REG_JIT_EXIT_RESUME;
    exit->resume_pc = header_pc;
    exit->disable = false;
    bool ran = vm->regjit_hooks->enter_loop(vm, frame, code, header_pc, exit, vm->regjit_userdata);
    if(!ran && exit->disable) loop->state = REG_JIT_DISABLED;
    return ran;
}
