#pragma once

#include "tinypy/interpreter/regvm.h"

typedef struct JitScalarCode JitScalarCode;

/* A speculative, effect-free integer entry. Failure leaves Python state intact
 * so the ordinary native/interpreter entry can execute with precise frames. */
JitScalarCode* JitScalar__compile(const FuncDecl* decl, const RegCode* code);
void JitScalar__delete(JitScalarCode* code);
size_t JitScalar__size(const JitScalarCode* code);
bool JitScalar__call(JitScalarCode* code, VM* vm, py_Frame* frame);
