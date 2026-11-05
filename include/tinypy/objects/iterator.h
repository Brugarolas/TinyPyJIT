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
