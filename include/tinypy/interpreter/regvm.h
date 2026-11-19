#pragma once

#include <stdbool.h>

#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/interpreter/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    REGCODE_UNBUILT = 0,
    REGCODE_READY = 1,
    REGCODE_UNSUPPORTED = 2,
};

/* Build and cache a register-bytecode body for a function declaration. */
RegCode* RegVM__ensure_code(FuncDecl* decl);

/* Number of TValue slots required by an interpreted function frame. */
int RegVM__frame_slots(FuncDecl* decl);

/* True only for ordinary function frames with a ready register body. */
bool RegVM__can_run_frame(py_Frame* frame, RegCode** out_code);

/* Also checks tracing/watchdog state, which requires reference interpreter semantics. */
bool RegVM__can_execute_now(VM* self, py_Frame* frame, RegCode** out_code);

/* Fast register interpreter. The frame must satisfy RegVM__can_run_frame(). */
FrameResult VM__run_top_frame_reg(VM* self);

/*
 * Native-code helpers. A helper executes the instruction at `pc` of `frame`
 * (the top frame, registers in place) with the interpreter's own
 * implementation and inline caches. It returns false with an exception set;
 * the caller must then unwind exactly like the interpreter's REG_FAIL
 * (RegVM__slow_error at that pc's source ip).
 *
 * RegVM__jit_step_fn() returns the helper for an opcode family, or NULL:
 * CALL, LOAD_METHOD, LOAD_GLOBAL, LOAD_NONLOCAL, LOAD_ATTR and STORE_ATTR
 * (each helper accepts both the base and the _IC form).
 */
typedef bool (*RegJitStepFn)(VM* self, py_Frame* frame, RegCode* rc, int32_t pc);
RegJitStepFn RegVM__jit_step_fn(RegOpcode op);

/* Legacy stack interpreter entry point, implemented by ceval.c after integration. */
FrameResult VM__run_top_frame_stack(VM* self);

#ifdef __cplusplus
}  // extern "C"
#endif
