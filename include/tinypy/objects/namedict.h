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
     * Incremented on every write that can change lookup results or invalidate
     * an item pointer. Register-interpreter inline caches guard this value
     * before dereferencing cached NameDict_KV::value addresses.
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
