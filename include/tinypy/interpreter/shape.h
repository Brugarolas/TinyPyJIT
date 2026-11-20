#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/objects/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PyObject::slots sentinel for shape-backed dynamic attributes (also in object.h). */
#ifndef PK_OBJ_SHAPED_SLOTS
#define PK_OBJ_SHAPED_SLOTS (-2)
#endif

typedef struct PyObject PyObject;
typedef struct Shape Shape;
typedef struct ShapeArena ShapeArena;
typedef struct ShapeStorage ShapeStorage;

/*
 * Shapes are immutable transition nodes shared by instances of one Python type.
 * A node records the attribute introduced by that transition and its fixed slot.
 */
struct Shape {
    Shape* parent;
    Shape* first_child;
    Shape* next_sibling;
    Shape* next_alloc;
    py_Name added_name;
    uint16_t added_slot;
    uint16_t slot_count;
    uint32_t id;
};

struct ShapeArena {
    Shape* root;
    Shape* all_shapes;
    uint32_t shape_count;
    uint32_t next_id;
};

/*
 * The PyObject itself never moves. Only this auxiliary storage may be realloc'd
 * when an instance grows. ICs guard the immutable Shape* rather than this pointer.
 */
struct ShapeStorage {
    Shape* shape;
    uint16_t capacity;
    uint16_t is_inline; /* lives inside its PyObject's allocation: never freed or realloc'd */
    py_TValue values[];
};

/*
 * Every shaped object is allocated with room for a first ShapeStorage of
 * PK_SHAPE_INLINE_CAPACITY slots right after its ShapeStorage* (one allocation
 * per instance, fields next to the header). Growing past it moves the values to
 * a heap storage; the inline block then stays unused.
 */
#define PK_SHAPE_INLINE_CAPACITY 4u
#define PK_SHAPE_INLINE_BYTES (sizeof(ShapeStorage) + PK_SHAPE_INLINE_CAPACITY * sizeof(py_TValue))

ShapeArena* ShapeArena__new(void);
void ShapeArena__delete(ShapeArena* self);
Shape* ShapeArena__root(ShapeArena* self);
Shape* ShapeArena__transition(ShapeArena* self, Shape* from, py_Name name);
int Shape__lookup(const Shape* shape, py_Name name);

ShapeStorage* ShapeStorage__new(Shape* root);
/* Initialize the inline storage block of a freshly allocated shaped object. */
ShapeStorage* ShapeStorage__init_inline(void* block, Shape* root);
ShapeStorage* ShapeObject__storage(PyObject* object);
ShapeStorage** ShapeObject__storage_ref(PyObject* object);
void ShapeStorage__delete(ShapeStorage* self);
Shape* ShapeStorage__shape(const ShapeStorage* self);
uint16_t ShapeStorage__slot_count(const ShapeStorage* self);
py_Ref ShapeStorage__slot(ShapeStorage* self, uint16_t slot);
py_Ref ShapeStorage__try_get(ShapeStorage* self, py_Name name);
void ShapeStorage__set(ShapeArena* arena, ShapeStorage** self, py_Name name, py_Ref value);
bool ShapeStorage__del(ShapeStorage* self, py_Name name);
void ShapeStorage__clear(ShapeStorage* self);
bool ShapeStorage__apply(ShapeStorage* self,
                         bool (*f)(py_Name name, py_Ref value, void* ctx),
                         void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif
