#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define py_malloc malloc
#define py_free free
#define c11__rtassert assert
typedef struct NameKey { uint64_t hash; } NameKey;
typedef NameKey* py_Name;
typedef struct py_TValue { int type; int64_t value; } py_TValue;
static inline py_TValue* py_NIL(void) { static py_TValue nil = {0, 0}; return &nil; }
