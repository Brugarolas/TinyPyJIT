#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pocketpy/interpreter/regbytecode.h"
#include "pocketpy/interpreter/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

bool RegVM__slow_binary(RegOpcode op, py_Ref out, py_Ref lhs, py_Ref rhs);
bool RegVM__slow_contains(py_Ref out, py_Ref container, py_Ref needle, bool invert);
bool RegVM__slow_build_imag(VM* self, py_Ref out, py_Ref value);
bool RegVM__slow_build_string(VM* self, py_Ref out, py_Ref begin, uint8_t count);
bool RegVM__slow_unpack_sequence(py_Ref src, py_Ref dst, uint8_t expected);
bool RegVM__slow_unpack_ex(py_Ref src, py_Ref dst, uint8_t prefix);
FrameResult RegVM__slow_error(VM* self, py_Frame* frame, int32_t source_ip);

#ifdef __cplusplus
}  // extern "C"
#endif
