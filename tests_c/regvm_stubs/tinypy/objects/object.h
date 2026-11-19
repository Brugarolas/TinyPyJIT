#pragma once
#include "tinypy/tinypy.h"
#include "tinypy/objects/namedict.h"
py_TValue* PyObject__slots(PyObject* self);
NameDict* PyObject__dict(PyObject* self);
void* PyObject__userdata(PyObject* self);
#define PK_OBJ_DICT_SLOTS (-1)
#ifndef PK_OBJ_SHAPED_SLOTS
#define PK_OBJ_SHAPED_SLOTS (-2)
#endif
#define PyObject__has_namespace(self) \
    ((self)->slots == PK_OBJ_DICT_SLOTS || (self)->slots == PK_OBJ_SHAPED_SLOTS)
