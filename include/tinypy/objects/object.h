#pragma once

#include "tinypy/objects/namedict.h"
#include "tinypy/objects/base.h"
#include "tinypy/interpreter/shape.h"

typedef struct PyObject {
    py_Type type;  // we have a duplicated type here for convenience
    uint8_t size_8b;
    uint8_t gc_marked;  // lsb (self is marked), 2nd lsb (no recursively mark)
    int slots;          // number of slots in the object
    char flex[];
} PyObject;

// slots >= 0, allocate N slots
// slots == -1 (PK_OBJ_DICT_SLOTS), allocate a dict (types, modules, ...)
// slots == -2 (PK_OBJ_SHAPED_SLOTS), a ShapeStorage* (instances of Python classes)

// | HEADER | <N slots>     | <userdata>
// | HEADER | <dict>        | <userdata>
// | HEADER | ShapeStorage* | <inline ShapeStorage> | <userdata>

#define PK_OBJ_DICT_SLOTS (-1)
#ifndef PK_OBJ_SHAPED_SLOTS
#define PK_OBJ_SHAPED_SLOTS (-2)
#endif

py_TValue* PyObject__slots(PyObject* self);
NameDict* PyObject__dict(PyObject* self);
void* PyObject__userdata(PyObject* self);

/* True when the object has a per-instance attribute namespace (dict or shape). */
#define PyObject__has_namespace(self)                                                              \
    ((self)->slots == PK_OBJ_DICT_SLOTS || (self)->slots == PK_OBJ_SHAPED_SLOTS)

/* Number of live attributes in an object's namespace (0 when it has none). */
int PyObject__namespace_length(PyObject* self);

#define PK_OBJ_SLOTS_SIZE(slots)                                                                   \
    ((slots) >= 0 ? sizeof(py_TValue) * (slots)                                                    \
                  : (slots) == PK_OBJ_DICT_SLOTS ? sizeof(NameDict)                             \
                                                 : sizeof(void*) + PK_SHAPE_INLINE_BYTES)

void PyObject__dtor(PyObject* self);

#define pk__mark_value(val)                                                                        \
    if((val)->is_ptr) {                                                                            \
        PyObject* obj = (val)->_obj;                                                               \
        if(!(obj->gc_marked & 0b01)) {                                                             \
            obj->gc_marked |= 0b01;                                                                \
            if(!(obj->gc_marked & 0b10)) { c11_vector__push(PyObject*, p_stack, obj); }            \
        }                                                                                          \
    }
