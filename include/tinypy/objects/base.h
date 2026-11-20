#pragma once

#include "tinypy/tinypy.h"

#include <string.h>

typedef struct PyObject PyObject;
typedef struct VM VM;
extern PK_THREAD_LOCAL VM* pk_current_vm;

typedef struct py_TValue {
    py_Type type;
    bool is_ptr;
    int extra;

    union {
        int64_t _i64;
        double _f64;
        bool _bool;
        py_CFunction _cfunc;
        PyObject* _obj;
        c11_vec2 _vec2;
        c11_vec2i _vec2i;
        c11_vec3 _vec3;
        c11_vec3i _vec3i;
        c11_vec4i _vec4i;
        c11_color32 _color32;
        void* _ptr;
        char _chars[16];
    };
} py_TValue;

/*
 * Inline scalar constructors for the interpreter. The first word (type,
 * is_ptr = false, extra = 0) is one 8-byte store and payloads are written as
 * whole words, so copying a freshly created value never stalls on store
 * forwarding from narrower stores. The exported functions stay available as
 * (py_newint)(...) etc.
 */
#if !defined(PK_NO_INLINE_VALUES) && !(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static inline void pk__scalar_header(py_TValue* out, py_Type type) {
    uint64_t word = (uint16_t)type;
    memcpy(out, &word, sizeof(word));
}
static inline void pk__inline_newint(py_TValue* out, int64_t val) {
    pk__scalar_header(out, tp_int);
    out->_i64 = val;
}
static inline void pk__inline_newfloat(py_TValue* out, double val) {
    pk__scalar_header(out, tp_float);
    out->_f64 = val;
}
static inline void pk__inline_newbool(py_TValue* out, bool val) {
    pk__scalar_header(out, tp_bool);
    out->_i64 = val ? 1 : 0;
}
static inline void pk__inline_newscalar(py_TValue* out, py_Type type) {
    pk__scalar_header(out, type);
    out->_i64 = 0;
}
#define py_newint(out, val) pk__inline_newint((out), (val))
#define py_newfloat(out, val) pk__inline_newfloat((out), (val))
#define py_newbool(out, val) pk__inline_newbool((out), (val))
#define py_newnone(out) pk__inline_newscalar((out), tp_NoneType)
#define py_newnil(out) pk__inline_newscalar((out), tp_nil)
#endif
