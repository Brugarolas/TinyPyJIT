#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PK_INLINE inline
#define PK_ENABLE_WATCHDOG 0
#define PY_RAISE
#define PY_RETURN
#define PY_MAYBENULL
#define py_malloc malloc
#define py_realloc realloc
#define py_free free
#define c11__rtassert assert
#define c11__unreachable() abort()
#define c11__abort(...) abort()
typedef int16_t py_Type;
typedef int64_t py_i64;
typedef double py_f64;
typedef void (*py_CFunction)(void);
typedef void (*py_Dtor)(void*);
enum py_CompileMode { EXEC_MODE=0, EVAL_MODE=1, SINGLE_MODE=2, RELOAD_MODE=3 };
typedef struct py_OpaqueName* py_Name;
typedef struct PyObject { py_Type type; uint8_t size_8b; uint8_t gc_marked; int slots; unsigned char flex[sizeof(void*)]; } PyObject;
typedef struct { float x,y; } c11_vec2;
typedef struct { int x,y; } c11_vec2i;
typedef struct { float x,y,z; } c11_vec3;
typedef struct { int x,y,z; } c11_vec3i;
typedef struct py_TValue { py_Type type; bool is_ptr; int extra; union { int64_t _i64; double _f64; bool _bool; py_CFunction _cfunc; PyObject* _obj; void* _ptr; char _chars[16]; }; } py_TValue;
typedef py_TValue* py_Ref;
typedef py_TValue* py_StackRef;
typedef py_TValue* py_GlobalRef;
typedef py_TValue* py_ObjectRef;
typedef py_TValue* py_ItemRef;
typedef struct c11_sv { const char* data; int size; } c11_sv;
typedef struct c11_string { char* data; int size; } c11_string;
enum { tp_nil=0, tp_int=1, tp_float, tp_bool, tp_NoneType, tp_function, tp_nativefunc, tp_type, tp_tuple, tp_list, tp_vec2i, tp_vec2, tp_vec3i, tp_vec3, tp_BaseException, tp_RecursionError, tp_RuntimeError, tp_generator, tp_array2d_like_iterator, tp_list_iterator, tp_tuple_iterator, tp_dict_iterator, tp_range_iterator, tp_str_iterator, tp_module, tp_super, tp_property, tp_staticmethod, tp_classmethod, tp_boundmethod };
extern py_Name __contains__, __neg__, __invert__, __new__;
void py_newint(py_Ref, py_i64); void py_newfloat(py_Ref, py_f64); void py_newbool(py_Ref,bool); void py_newnone(py_Ref); void py_newnil(py_Ref); void py_newellipsis(py_Ref); void py_newboundmethod(py_Ref,py_Ref,py_Ref);
bool py_isnil(py_Ref); bool py_isidentical(py_Ref,py_Ref); int py_bool(py_Ref);
bool py_binaryadd(py_Ref,py_Ref); bool py_binarysub(py_Ref,py_Ref); bool py_binarymul(py_Ref,py_Ref); bool py_binarytruediv(py_Ref,py_Ref); bool py_binaryfloordiv(py_Ref,py_Ref); bool py_binarymod(py_Ref,py_Ref); bool py_binarypow(py_Ref,py_Ref); bool py_binarylshift(py_Ref,py_Ref); bool py_binaryrshift(py_Ref,py_Ref); bool py_binaryand(py_Ref,py_Ref); bool py_binaryor(py_Ref,py_Ref); bool py_binaryxor(py_Ref,py_Ref); bool py_binarymatmul(py_Ref,py_Ref);
bool py_lt(py_Ref,py_Ref); bool py_le(py_Ref,py_Ref); bool py_eq(py_Ref,py_Ref); bool py_ne(py_Ref,py_Ref); bool py_gt(py_Ref,py_Ref); bool py_ge(py_Ref,py_Ref);
py_Ref py_retval(void); py_Ref py_tpfindmagic(py_Type,py_Name); bool py_call(py_Ref,int,py_Ref); bool py_callcfunc(py_CFunction,int,py_Ref); py_Name py_name(const char*); py_Ref py_getdict(py_Ref,py_Name); py_Ref py_getslot(py_Ref,int); const char* py_tostrn(py_Ref,int*); c11_sv py_tosv(py_Ref);
unsigned char* py_newbytes(py_Ref,int); py_Ref py_newtuple(py_Ref,int); int py_tuple_len(py_Ref); py_TValue* py_tuple_data(py_Ref); void py_newlistn(py_Ref,int); int py_list_len(py_Ref); py_TValue* py_list_data(py_Ref); void py_list_setitem(py_Ref,int,py_Ref); void py_list_append(py_Ref,py_Ref); void py_newdict(py_Ref); bool py_dict_setitem(py_Ref,py_Ref,py_Ref); py_ObjectRef py_newslice(py_Ref);
c11_vec2i py_tovec2i(py_Ref); c11_vec2 py_tovec2(py_Ref); c11_vec3i py_tovec3i(py_Ref); c11_vec3 py_tovec3(py_Ref);
bool py_getattr(py_Ref,py_Name); bool py_setattr(py_Ref,py_Name,py_Ref); bool py_delattr(py_Ref,py_Name); bool py_getitem(py_Ref,py_Ref); bool py_setitem(py_Ref,py_Ref,py_Ref); bool py_delitem(py_Ref,py_Ref); bool py_iter(py_Ref); int py_equal(py_Ref,py_Ref); bool py_str(py_Ref);
void* py_newobject(py_Ref,py_Type,int,size_t); void py_exception(py_Type,const char*,...);
py_i64 cpy312__int_floordiv(py_i64,py_i64); py_i64 cpy312__int_mod(py_i64,py_i64);
typedef struct NameDict NameDict; void NameDict__set(NameDict*,py_Name,py_TValue*);
