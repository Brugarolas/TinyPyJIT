#include "tinypy/common/utils.h"
#include "tinypy/interpreter/shape.h"

#include "tinypy/tinypy.h"
#include "tinypy/objects/object.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define SHAPE_INITIAL_CAPACITY 4u

static Shape* shape_new(ShapeArena* arena,
                        Shape* parent,
                        py_Name added_name,
                        uint16_t added_slot) {
    Shape* shape = py_malloc(sizeof(Shape));
    if(shape == NULL) c11__abort("ShapeArena: out of memory");
    memset(shape, 0, sizeof(*shape));
    shape->parent = parent;
    shape->added_name = added_name;
    shape->added_slot = added_slot;
    shape->slot_count = parent == NULL ? 0 : (uint16_t)(parent->slot_count + 1u);
    shape->id = arena->next_id++;
    shape->next_alloc = arena->all_shapes;
    arena->all_shapes = shape;
    arena->shape_count++;
    return shape;
}

ShapeArena* ShapeArena__new(void) {
    ShapeArena* arena = py_malloc(sizeof(ShapeArena));
    if(arena == NULL) c11__abort("ShapeArena: out of memory");
    memset(arena, 0, sizeof(*arena));
    arena->next_id = 1;
    arena->root = shape_new(arena, NULL, NULL, 0);
    return arena;
}

void ShapeArena__delete(ShapeArena* self) {
    if(self == NULL) return;
    Shape* shape = self->all_shapes;
    while(shape != NULL) {
        Shape* next = shape->next_alloc;
        py_free(shape);
        shape = next;
    }
    py_free(self);
}

Shape* ShapeArena__root(ShapeArena* self) {
    assert(self != NULL);
    return self->root;
}

int Shape__lookup(const Shape* shape, py_Name name) {
    for(const Shape* p = shape; p != NULL && p->parent != NULL; p = p->parent) {
        if(p->added_name == name) return (int)p->added_slot;
    }
    return -1;
}

Shape* ShapeArena__transition(ShapeArena* self, Shape* from, py_Name name) {
    assert(self != NULL);
    assert(from != NULL);
    assert(name != NULL);

    int existing = Shape__lookup(from, name);
    if(existing >= 0) return from;

    for(Shape* child = from->first_child; child != NULL; child = child->next_sibling) {
        if(child->added_name == name) return child;
    }

    if(from->slot_count == UINT16_MAX) c11__abort("too many shaped attributes");
    Shape* child = shape_new(self, from, name, from->slot_count);
    child->next_sibling = from->first_child;
    from->first_child = child;
    return child;
}

static size_t storage_size(uint16_t capacity) {
    return sizeof(ShapeStorage) + (size_t)capacity * sizeof(py_TValue);
}

ShapeStorage* ShapeObject__storage(PyObject* object) {
    if(object == NULL || object->slots != PK_OBJ_SHAPED_SLOTS) return NULL;
    return *(ShapeStorage**)object->flex;
}

ShapeStorage** ShapeObject__storage_ref(PyObject* object) {
    assert(object != NULL && object->slots == PK_OBJ_SHAPED_SLOTS);
    return (ShapeStorage**)object->flex;
}

ShapeStorage* ShapeStorage__new(Shape* root) {
    assert(root != NULL && root->parent == NULL);
    ShapeStorage* storage = py_malloc(storage_size(SHAPE_INITIAL_CAPACITY));
    if(storage == NULL) c11__abort("ShapeStorage: out of memory");
    storage->shape = root;
    storage->capacity = SHAPE_INITIAL_CAPACITY;
    storage->reserved = 0;
    memset(storage->values, 0, (size_t)storage->capacity * sizeof(py_TValue));
    return storage;
}

void ShapeStorage__delete(ShapeStorage* self) {
    py_free(self);
}

Shape* ShapeStorage__shape(const ShapeStorage* self) {
    return self != NULL ? self->shape : NULL;
}

uint16_t ShapeStorage__slot_count(const ShapeStorage* self) {
    return self != NULL && self->shape != NULL ? self->shape->slot_count : 0;
}

py_Ref ShapeStorage__slot(ShapeStorage* self, uint16_t slot) {
    if(self == NULL || self->shape == NULL || slot >= self->shape->slot_count) return NULL;
    return &self->values[slot];
}

py_Ref ShapeStorage__try_get(ShapeStorage* self, py_Name name) {
    if(self == NULL) return NULL;
    int slot = Shape__lookup(self->shape, name);
    if(slot < 0) return NULL;
    py_Ref value = &self->values[slot];
    return py_isnil(value) ? NULL : value;
}

static ShapeStorage* storage_reserve(ShapeStorage* self, uint16_t required) {
    if(required <= self->capacity) return self;
    uint32_t capacity = self->capacity != 0 ? self->capacity : SHAPE_INITIAL_CAPACITY;
    while(capacity < required) {
        uint32_t next = capacity * 2u;
        if(next > UINT16_MAX) next = UINT16_MAX;
        if(next == capacity) c11__abort("too many shaped attributes");
        capacity = next;
    }

    uint16_t old_capacity = self->capacity;
    ShapeStorage* resized = py_realloc(self, storage_size((uint16_t)capacity));
    if(resized == NULL) c11__abort("ShapeStorage: out of memory");
    resized->capacity = (uint16_t)capacity;
    memset(resized->values + old_capacity,
           0,
           (size_t)(resized->capacity - old_capacity) * sizeof(py_TValue));
    return resized;
}

void ShapeStorage__set(ShapeArena* arena,
                       ShapeStorage** self,
                       py_Name name,
                       py_Ref value) {
    assert(arena != NULL);
    assert(self != NULL && *self != NULL);
    assert(name != NULL && value != NULL);

    ShapeStorage* storage = *self;
    int slot = Shape__lookup(storage->shape, name);
    if(slot < 0) {
        Shape* next = ShapeArena__transition(arena, storage->shape, name);
        storage = storage_reserve(storage, next->slot_count);
        storage->shape = next;
        slot = next->added_slot;
        *self = storage;
    }
    storage->values[slot] = *value;
}

bool ShapeStorage__del(ShapeStorage* self, py_Name name) {
    if(self == NULL) return false;
    int slot = Shape__lookup(self->shape, name);
    if(slot < 0 || py_isnil(&self->values[slot])) return false;
    py_newnil(&self->values[slot]);
    return true;
}

void ShapeStorage__clear(ShapeStorage* self) {
    if(self == NULL || self->shape == NULL) return;
    for(uint16_t i = 0; i < self->shape->slot_count; i++) py_newnil(&self->values[i]);
}

bool ShapeStorage__apply(ShapeStorage* self,
                         bool (*f)(py_Name name, py_Ref value, void* ctx),
                         void* ctx) {
    if(self == NULL || self->shape == NULL) return true;

    /*
     * A name occurs only once on a shape path, because setting an existing name
     * reuses its slot instead of creating a transition. Iteration order is not
     * part of NameDict's public contract, so walking newest-to-oldest is valid.
     */
    for(Shape* shape = self->shape; shape != NULL && shape->parent != NULL; shape = shape->parent) {
        py_Ref value = &self->values[shape->added_slot];
        if(py_isnil(value)) continue;
        if(!f(shape->added_name, value, ctx)) return false;
    }
    return true;
}
