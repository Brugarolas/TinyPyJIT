#pragma once
#include "tinypy/objects/codeobject.h"
typedef struct py_Frame { struct py_Frame* f_back; const CodeObject* co; py_StackRef p0; py_GlobalRef module; py_Ref globals; py_Ref locals; bool is_locals_special; int ip; } py_Frame;
int Frame__getglobal(py_Frame*,py_Name); bool Frame__setglobal(py_Frame*,py_Name,py_TValue*); int Frame__delglobal(py_Frame*,py_Name); py_Ref Frame__getclosure(py_Frame*,py_Name); int Frame__lineno(const py_Frame*); void* FastLocals__to_namedict(py_TValue*,const CodeObject*);

py_Frame* Frame__new(const CodeObject*, py_StackRef, py_GlobalRef, py_Ref, py_Ref, bool);
py_Frame* Frame__new_stack(const CodeObject*, py_StackRef, py_GlobalRef, py_Ref, py_Ref, bool);
