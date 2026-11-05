#pragma once
#include "tinypy/tinypy.h"
#include "tinypy/objects/namedict.h"
py_TValue* PyObject__slots(PyObject* self);
NameDict* PyObject__dict(PyObject* self);
void* PyObject__userdata(PyObject* self);
