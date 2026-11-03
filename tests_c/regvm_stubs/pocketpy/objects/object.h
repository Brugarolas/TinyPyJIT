#pragma once
#include "pocketpy/pocketpy.h"
#include "pocketpy/objects/namedict.h"
py_TValue* PyObject__slots(PyObject* self);
NameDict* PyObject__dict(PyObject* self);
void* PyObject__userdata(PyObject* self);
