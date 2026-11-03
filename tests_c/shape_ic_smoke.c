#include "pocketpy/interpreter/shape.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct py_OpaqueName { int id; } FakeName;

int main(void) {
    FakeName x0={1}, y0={2};
    py_Name x=(py_Name)&x0, y=(py_Name)&y0;
    ShapeArena* arena = ShapeArena__new();
    ShapeStorage* storage = ShapeStorage__new(ShapeArena__root(arena));
    py_TValue v1={1,11}, v2={1,22}, vy={1,33};

    ShapeStorage__set(arena, &storage, x, &v1);
    Shape* cached_shape = ShapeStorage__shape(storage);
    int x_slot = Shape__lookup(cached_shape, x);
    assert(x_slot >= 0);
    assert(ShapeStorage__slot(storage, (uint16_t)x_slot)->payload == 11);

    /* Value updates keep the shape and therefore preserve the fixed-slot IC. */
    ShapeStorage__set(arena, &storage, x, &v2);
    assert(ShapeStorage__shape(storage) == cached_shape);
    assert(ShapeStorage__slot(storage, (uint16_t)x_slot)->payload == 22);

    /* Adding a property transitions the shape and invalidates a monomorphic guard. */
    ShapeStorage__set(arena, &storage, y, &vy);
    assert(ShapeStorage__shape(storage) != cached_shape);

    Shape* new_shape = ShapeStorage__shape(storage);
    x_slot = Shape__lookup(new_shape, x);
    assert(x_slot >= 0);
    assert(ShapeStorage__del(storage, x));
    assert(ShapeStorage__shape(storage) == new_shape);
    assert(py_isnil(ShapeStorage__slot(storage, (uint16_t)x_slot)));

    ShapeStorage__delete(storage);
    ShapeArena__delete(arena);
    puts("shape IC smoke: ok");
    return 0;
}
