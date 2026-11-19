#include "tinypy/objects/object.h"
#include "tinypy/interpreter/shape.h"
#include "tinypy/tinypy.h"
#include <assert.h>

_Static_assert(PK_OBJ_SHAPED_SLOTS == -2, "object.h and shape.h disagree on the shaped sentinel");

static bool count_live(py_Name name, py_Ref value, void* ctx) {
    (void)name;
    (void)value;
    (*(int*)ctx)++;
    return true;
}

int PyObject__namespace_length(PyObject* self) {
    if(self->slots == PK_OBJ_DICT_SLOTS) return ((NameDict*)self->flex)->length;
    if(self->slots != PK_OBJ_SHAPED_SLOTS) return 0;
    int count = 0;
    ShapeStorage__apply(ShapeObject__storage(self), count_live, &count);
    return count;
}

PK_INLINE void* PyObject__userdata(PyObject* self) {
    return self->flex + PK_OBJ_SLOTS_SIZE(self->slots);
}

PK_INLINE NameDict* PyObject__dict(PyObject* self) {
    assert(self->slots == -1);
    return (NameDict*)(self->flex);
}

PK_INLINE py_TValue* PyObject__slots(PyObject* self) {
    assert(self->slots >= 0);
    return (py_TValue*)(self->flex);
}