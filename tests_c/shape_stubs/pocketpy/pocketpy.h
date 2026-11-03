#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define py_malloc malloc
#define py_realloc realloc
#define py_free free
#define c11__abort(...) abort()
typedef struct py_OpaqueName* py_Name;
typedef struct py_TValue { int type; long long payload; } py_TValue;
typedef py_TValue* py_Ref;
static inline bool py_isnil(py_Ref v){ return v->type == 0; }
static inline void py_newnil(py_Ref v){ v->type=0; v->payload=0; }
