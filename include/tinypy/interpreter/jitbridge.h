#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/interpreter/regprofile.h"
#include "tinypy/interpreter/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RegJitCompileKind {
    REG_JIT_COMPILE_FUNCTION = 0,
    REG_JIT_COMPILE_LOOP,
} RegJitCompileKind;

typedef enum RegJitExitKind {
    REG_JIT_EXIT_RESUME = 0,
    REG_JIT_EXIT_RETURN,
    REG_JIT_EXIT_ERROR,
    REG_JIT_EXIT_YIELD,
} RegJitExitKind;

typedef struct RegJitExit {
    RegJitExitKind kind;
    int32_t resume_pc;
} RegJitExit;

typedef struct RegJitCompileRequest {
    RegJitCompileKind kind;
    FuncDecl* decl;
    RegCode* code;
    int32_t loop_header_pc;
    int32_t loop_header_source_ip;
    RegProfileView profile;
} RegJitCompileRequest;

typedef bool (*RegJitCompileFn)(VM* vm,
                                const RegJitCompileRequest* request,
                                void* userdata);

typedef bool (*RegJitEnterFunctionFn)(VM* vm,
                                      py_Frame* frame,
                                      RegCode* code,
                                      RegJitExit* exit,
                                      void* userdata);

typedef bool (*RegJitEnterLoopFn)(VM* vm,
                                  py_Frame* frame,
                                  RegCode* code,
                                  int32_t header_pc,
                                  RegJitExit* exit,
                                  void* userdata);

typedef struct RegJitHooks {
    RegJitCompileFn compile;
    RegJitEnterFunctionFn enter_function;
    RegJitEnterLoopFn enter_loop;
} RegJitHooks;

/* VM owns neither hooks nor userdata. They must outlive their registration. */
void RegJit__set_hooks(VM* vm, const RegJitHooks* hooks, void* userdata);

bool RegJit__request_function(VM* vm, FuncDecl* decl, RegCode* code);
bool RegJit__request_loop(VM* vm, FuncDecl* decl, RegCode* code, RegLoopProfile* loop);

/* Returns true only when native code actually ran and filled exit. */
bool RegJit__enter_function(VM* vm,
                            py_Frame* frame,
                            RegCode* code,
                            RegJitExit* exit);
bool RegJit__enter_loop(VM* vm,
                        py_Frame* frame,
                        RegCode* code,
                        int32_t header_pc,
                        RegJitExit* exit);

#ifdef __cplusplus
}  // extern "C"
#endif
