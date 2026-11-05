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

/* Legacy stack interpreter entry point, implemented by ceval.c after integration. */
FrameResult VM__run_top_frame_stack(VM* self);

#ifdef __cplusplus
}  // extern "C"
#endif
