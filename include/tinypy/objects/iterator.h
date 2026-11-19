#pragma once

#include "tinypy/objects/base.h"
#include "tinypy/common/vector.h"

typedef struct tuple_iterator {
    py_TValue* p;
    int length;
    int index;
} tuple_iterator;

typedef struct list_iterator {
    c11_vector* vec;
    int index;
} list_iterator;

/* Userdata of `range` objects. */
typedef struct Range {
    py_i64 start;
    py_i64 stop;
    py_i64 step;
} Range;

/* Userdata of `range_iterator` objects (allocated with zero slots). The native
 * JIT iterates it in place, so keep this layout in sync with src/jit/native.c. */
typedef struct RangeIterator {
    Range range;
    py_i64 current;
} RangeIterator;
