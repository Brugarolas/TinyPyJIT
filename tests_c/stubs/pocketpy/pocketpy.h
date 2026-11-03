#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define py_malloc malloc
#define py_free free
#define c11__rtassert assert
#define c11__unreachable() abort()
typedef int16_t py_Type;
typedef int64_t py_i64;
typedef double py_f64;
typedef struct py_TValue { int type; int64_t payload[2]; } py_TValue;
typedef void* py_Name;
