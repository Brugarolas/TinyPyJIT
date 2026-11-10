#pragma once

#include "tinypy/interpreter/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A VM owns the generated code for its entire lifetime. */
void TinyJit__attach(VM* vm);
void TinyJit__detach(VM* vm);

#ifdef __cplusplus
}
#endif
