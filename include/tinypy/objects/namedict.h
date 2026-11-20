#pragma once

#include "tinypy/objects/base.h"

#include <stdint.h>

typedef struct NameDict_KV {
    py_Name key;
    py_TValue value;
} NameDict_KV;

typedef struct NameDict {
    int length;
    float load_factor;
    int capacity;
    int critical_size;
    uintptr_t mask;
    NameDict_KV* items;

    /*
     * Incremented on every write that can change which slot holds a name or
     * invalidate an item pointer (insertion, deletion, rehash, clear), but not
     * when an existing name is rebound in place. Inline caches guard this value
     * and then read NameDict_KV::value live, so a rebinding is always visible.
     */
    uint64_t version;
} NameDict;

NameDict* NameDict__new(float load_factor);
void NameDict__delete(NameDict* self);
void NameDict__ctor(NameDict* self, float load_factor);
void NameDict__dtor(NameDict* self);
py_TValue* NameDict__try_get(NameDict* self, py_Name key);
int NameDict__find_index(NameDict* self, py_Name key);
bool NameDict__contains(NameDict* self, py_Name key);
void NameDict__set(NameDict* self, py_Name key, py_TValue* value);
bool NameDict__del(NameDict* self, py_Name key);
void NameDict__clear(NameDict* self);
