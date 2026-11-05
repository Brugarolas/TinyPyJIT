#include "tinypy/interpreter/shape.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct py_OpaqueName { int id; } FakeName;

static bool count_cb(py_Name name, py_Ref value, void* ctx) {
    (void)name;
    assert(value->type != 0);
    (*(int*)ctx)++;
    return true;
}

int main(void) {
    FakeName a0={1}, b0={2}, c0={3}, d0={4}, e0={5};
    py_Name a=(py_Name)&a0, b=(py_Name)&b0, c=(py_Name)&c0, d=(py_Name)&d0, e=(py_Name)&e0;
    ShapeArena* arena = ShapeArena__new();
    ShapeStorage* s1 = ShapeStorage__new(ShapeArena__root(arena));
    ShapeStorage* s2 = ShapeStorage__new(ShapeArena__root(arena));
    py_TValue va={1,10}, vb={1,20}, vc={1,30}, vd={1,40}, ve={1,50};

    ShapeStorage__set(arena, &s1, a, &va);
    ShapeStorage__set(arena, &s1, b, &vb);
    ShapeStorage__set(arena, &s2, a, &va);
    ShapeStorage__set(arena, &s2, b, &vb);
    assert(ShapeStorage__shape(s1) == ShapeStorage__shape(s2));
    assert(ShapeStorage__try_get(s1, a)->payload == 10);
    assert(ShapeStorage__try_get(s1, b)->payload == 20);

    /* Force storage growth while keeping the object-level Shape* contract stable. */
    ShapeStorage__set(arena, &s1, c, &vc);
    ShapeStorage__set(arena, &s1, d, &vd);
    ShapeStorage__set(arena, &s1, e, &ve);
    assert(ShapeStorage__slot_count(s1) == 5);
    assert(ShapeStorage__try_get(s1, e)->payload == 50);

    assert(ShapeStorage__del(s1, c));
    assert(ShapeStorage__try_get(s1, c) == NULL);
    ShapeStorage__set(arena, &s1, c, &vc);
    assert(ShapeStorage__try_get(s1, c)->payload == 30);

    int count=0;
    assert(ShapeStorage__apply(s1, count_cb, &count));
    assert(count == 5);
    ShapeStorage__clear(s1);
    count=0;
    assert(ShapeStorage__apply(s1, count_cb, &count));
    assert(count == 0);

    ShapeStorage__delete(s1);
    ShapeStorage__delete(s2);
    ShapeArena__delete(arena);
    puts("shape smoke: ok");
    return 0;
}
