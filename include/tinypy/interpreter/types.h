#pragma once

#include "tinypy/common/vector.h"
#include "tinypy/objects/base.h"
#include <stdio.h>

typedef struct {
    uint64_t hash;
    py_TValue key;
    py_TValue val;
} DictEntry;

typedef struct {
    int length;
    uint32_t capacity;
    uint32_t null_index_value;
    bool index_is_short;
    void* indices;
    c11_vector /*T=DictEntry*/ entries;
} Dict;

typedef c11_vector List;

/* Canonical methods, also used to guard the JIT's exact-list fast paths. */
bool pk_list__getitem__(int argc, py_Ref argv);
bool pk_list__setitem__(int argc, py_Ref argv);

void c11_chunked_array2d__mark(void* ud, c11_vector* p_stack);
void function__gc_mark(void* ud, c11_vector* p_stack);

typedef struct {
    FILE* file;
    const char* path;
    const char* mode;
} io_FileIO;
