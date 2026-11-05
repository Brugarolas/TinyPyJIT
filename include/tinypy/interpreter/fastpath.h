#pragma once

#include "tinypy/interpreter/bindings.h"
#include "tinypy/objects/base.h"
#include "tinypy/tinypy.h"

#if defined(__GNUC__) || defined(__clang__)
#define PK_CEVAL_LIKELY(x) __builtin_expect(!!(x), 1)
#define PK_CEVAL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PK_CEVAL_COLD __attribute__((cold, noinline))
#define PK_CEVAL_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PK_CEVAL_LIKELY(x) (!!(x))
#define PK_CEVAL_UNLIKELY(x) (!!(x))
#define PK_CEVAL_COLD __declspec(noinline)
#define PK_CEVAL_NOINLINE __declspec(noinline)
#else
#define PK_CEVAL_LIKELY(x) (!!(x))
#define PK_CEVAL_UNLIKELY(x) (!!(x))
#define PK_CEVAL_COLD
#define PK_CEVAL_NOINLINE
#endif

/*
 * Fast truthiness path for interpreter branches.
 *
 * These four types cover the cheap cases already handled by py_bool(). Keeping
 * them inline in ceval avoids a C call on branch-heavy numeric code while the
 * fallback preserves the complete Python protocol (__bool__, then __len__).
 */
static PK_INLINE int pk_ceval_bool(py_Ref val) {
    switch(val->type) {
        case tp_bool: return val->_bool;
        case tp_int: return val->_i64 != 0;
        case tp_float: return val->_f64 != 0.0;
        case tp_NoneType: return 0;
        default: return py_bool(val);
    }
}

/*
 * Fast iterator dispatch for FOR_ITER.
 *
 * Built-in iterators already have non-allocating exhaustion paths. Dispatching
 * them directly from ceval removes the extra py_next() call/switch layer. The
 * fallback handles user-defined iterators and preserves StopIteration semantics.
 */
static PK_INLINE int pk_ceval_next(py_Ref val) {
    switch(val->type) {
        case tp_generator: return generator__iternext(val);
        case tp_array2d_like_iterator: return array2d_like_iterator__iternext(val);
        case tp_list_iterator: return list_iterator__iternext(val);
        case tp_tuple_iterator: return tuple_iterator__iternext(val);
        case tp_dict_iterator: return dict_items__iternext(val);
        case tp_range_iterator: return range_iterator__iternext(val);
        case tp_str_iterator: return str_iterator__iternext(val);
        default: return py_next(val);
    }
}
