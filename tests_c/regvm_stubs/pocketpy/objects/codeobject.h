#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pocketpy/pocketpy.h"
#include "pocketpy/common/vector.h"
#define BC_NOARG 0
#define BC_RETURN_VIRTUAL 5

typedef enum FuncType { FuncType_UNSET, FuncType_NORMAL, FuncType_SIMPLE, FuncType_GENERATOR } FuncType;
typedef enum Opcode {
    OP_NO_OP, OP_POP_TOP, OP_DUP_TOP, OP_DUP_TOP_TWO, OP_ROT_TWO, OP_ROT_THREE, OP_PRINT_EXPR,
    OP_LOAD_CONST, OP_LOAD_NONE, OP_LOAD_TRUE, OP_LOAD_FALSE, OP_LOAD_SMALL_INT, OP_LOAD_NAME_AS_INT,
    OP_LOAD_ELLIPSIS, OP_LOAD_FUNCTION, OP_LOAD_NULL, OP_LOAD_FAST, OP_LOAD_NAME, OP_LOAD_NONLOCAL,
    OP_LOAD_GLOBAL, OP_LOAD_ATTR, OP_LOAD_SELF_ATTR, OP_LOAD_CLASS_GLOBAL, OP_LOAD_METHOD, OP_LOAD_SUBSCR,
    OP_STORE_FAST, OP_STORE_NAME, OP_STORE_GLOBAL, OP_STORE_ATTR, OP_STORE_SELF_ATTR, OP_STORE_SUBSCR,
    OP_DELETE_FAST, OP_DELETE_NAME, OP_DELETE_GLOBAL, OP_DELETE_ATTR, OP_DELETE_SUBSCR,
    OP_BUILD_IMAG, OP_BUILD_BYTES, OP_BUILD_TUPLE, OP_BUILD_LIST, OP_BUILD_DICT, OP_BUILD_SET,
    OP_BUILD_SLICE, OP_BUILD_STRING, OP_BINARY_ADD, OP_BINARY_SUB, OP_BINARY_MUL, OP_BINARY_TRUEDIV,
    OP_BINARY_FLOORDIV, OP_BINARY_MOD, OP_BINARY_POW, OP_BINARY_LSHIFT, OP_BINARY_RSHIFT, OP_BINARY_AND,
    OP_BINARY_OR, OP_BINARY_XOR, OP_BINARY_MATMUL, OP_COMPARE_LT, OP_COMPARE_LE, OP_COMPARE_EQ,
    OP_COMPARE_NE, OP_COMPARE_GT, OP_COMPARE_GE, OP_IS_OP, OP_CONTAINS_OP, OP_JUMP_FORWARD,
    OP_POP_JUMP_IF_NOT_MATCH, OP_POP_JUMP_IF_FALSE, OP_POP_JUMP_IF_TRUE, OP_JUMP_IF_TRUE_OR_POP,
    OP_JUMP_IF_FALSE_OR_POP, OP_SHORTCUT_IF_FALSE_OR_POP, OP_LOOP_CONTINUE, OP_LOOP_BREAK, OP_CALL,
    OP_CALL_VARGS, OP_RETURN_VALUE, OP_YIELD_VALUE, OP_FOR_ITER_YIELD_VALUE, OP_LIST_APPEND, OP_DICT_ADD,
    OP_SET_ADD, OP_UNARY_NEGATIVE, OP_UNARY_NOT, OP_UNARY_STAR, OP_UNARY_INVERT, OP_GET_ITER, OP_FOR_ITER,
    OP_IMPORT_PATH, OP_POP_IMPORT_STAR, OP_UNPACK_SEQUENCE, OP_UNPACK_EX, OP_BEGIN_CLASS, OP_END_CLASS,
    OP_STORE_CLASS_ATTR, OP_ADD_CLASS_ANNOTATION, OP_WITH_ENTER, OP_WITH_EXIT, OP_BEGIN_TRY, OP_END_TRY,
    OP_EXCEPTION_MATCH, OP_HANDLE_EXCEPTION, OP_RAISE, OP_RAISE_ASSERT, OP_RE_RAISE, OP_PUSH_EXCEPTION,
    OP_FORMAT_STRING
} Opcode;

typedef struct Bytecode { uint16_t op; uint16_t arg; } Bytecode;
typedef struct BytecodeEx { int32_t lineno; int32_t iblock; } BytecodeEx;
typedef struct CodeObject {
    void* src;
    c11_string* name;
    c11_vector codes;
    c11_vector codes_ex;
    c11_vector consts;
    c11_vector varnames;
    c11_vector names;
    int nlocals;
    c11_vector blocks;
    c11_vector func_decls;
} CodeObject;
typedef struct FuncDeclKwArg { int index; py_Name key; py_TValue value; } FuncDeclKwArg;
typedef struct RegCode RegCode;
typedef struct FuncDecl {
    CodeObject code;
    c11_vector args;
    c11_vector kwargs;
    int starred_arg;
    int starred_kwarg;
    bool nested;
    FuncType type;
    struct RegCode* regcode;
    uint8_t regcode_state;
} FuncDecl;

typedef FuncDecl* FuncDecl_;
