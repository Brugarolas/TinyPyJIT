#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/common/vector.h"
#include "tinypy/common/smallmap.h"
#include "tinypy/objects/base.h"
#include "tinypy/objects/sourcedata.h"
#include "tinypy/objects/namedict.h"
#include "tinypy/tinypy.h"

#define BC_NOARG 0
#define BC_KEEPLINE -1
#define BC_RETURN_VIRTUAL 5

typedef enum FuncType {
    FuncType_UNSET,
    FuncType_NORMAL,
    FuncType_SIMPLE,
    FuncType_GENERATOR,
} FuncType;

typedef enum NameScope {
    NAME_LOCAL,
    NAME_GLOBAL,
} NameScope;

typedef enum CodeBlockType {
    CodeBlockType_NO_BLOCK,
    CodeBlockType_WHILE_LOOP,
    CodeBlockType_TRY,
    /* context blocks (stack-based) */
    CodeBlockType_FOR_LOOP,
    CodeBlockType_WITH,
    /* context blocks (flag-based) */
    CodeBlockType_EXCEPT,
} CodeBlockType;

typedef enum Opcode {

#define OPCODE(name) OP_##name,
#include "tinypy/xmacros/opcodes.h"
#undef OPCODE
} Opcode;

typedef struct Bytecode {
    uint16_t op;
    uint16_t arg;
} Bytecode;

void Bytecode__set_signed_arg(Bytecode* self, int arg);
bool Bytecode__is_forward_jump(const Bytecode* self);

typedef struct BytecodeEx {
    int32_t lineno;
    int32_t iblock;
} BytecodeEx;

typedef struct CodeBlock {
    int32_t type;
    int32_t parent;
    int32_t start;
    int32_t end;
    int32_t end2;
} CodeBlock;

typedef struct CodeObject {
    SourceData_ src;
    c11_string* name;

    c11_vector /*T=Bytecode*/ codes;
    c11_vector /*T=BytecodeEx*/ codes_ex;

    c11_vector /*T=py_TValue*/ consts;
    c11_vector /*T=py_Name*/ varnames;
    c11_vector /*T=py_Name*/ names;
    int nlocals;

    c11_smallmap_n2d varnames_inv;
    c11_smallmap_n2d names_inv;

    c11_vector /*T=CodeBlock*/ blocks;
    c11_vector /*T=FuncDecl_*/ func_decls;

    int start_line;
    int end_line;
} CodeObject;

void CodeObject__ctor(CodeObject* self, SourceData_ src, c11_sv name);
void CodeObject__dtor(CodeObject* self);
int CodeObject__add_varname(CodeObject* self, py_Name name);
int CodeObject__add_name(CodeObject* self, py_Name name);
void CodeObject__gc_mark(const CodeObject* self, c11_vector* p_stack);

// Serialization
void* CodeObject__dumps(const CodeObject* co, int* size);
char* CodeObject__loads(const void* data, int size, const char* filename, CodeObject* out);

typedef struct FuncDeclKwArg {
    int index;
    py_Name key;
    py_TValue value;
} FuncDeclKwArg;

typedef struct RegCode RegCode;

typedef struct FuncDecl {
    RefCounted rc;
    CodeObject code;

    c11_vector /*T=int32_t*/ args;
    c11_vector /*T=FuncDeclKwArg*/ kwargs;

    int starred_arg;
    int starred_kwarg;
    bool nested;

    char* docstring;

    FuncType type;
    c11_smallmap_n2d kw_to_index;

    /* Lazily generated register bytecode; never serialized. */
    RegCode* regcode;
    uint8_t regcode_state;
} FuncDecl;

typedef FuncDecl* FuncDecl_;

FuncDecl_ FuncDecl__rcnew(SourceData_ src, c11_sv name);
bool FuncDecl__is_duplicated_arg(const FuncDecl* self, py_Name name);
void FuncDecl__add_arg(FuncDecl* self, py_Name name);
void FuncDecl__add_kwarg(FuncDecl* self, py_Name name, const py_TValue* value);
void FuncDecl__add_starred_arg(FuncDecl* self, py_Name name);
void FuncDecl__add_starred_kwarg(FuncDecl* self, py_Name name);
void FuncDecl__gc_mark(const FuncDecl* self, c11_vector* p_stack);
void FuncDecl__dtor(void* p);

typedef struct Function {
    FuncDecl_ decl;
    py_GlobalRef module;
    py_TValue globals;
    NameDict* closure;
    PyObject* clazz;
    py_CFunction cfunc;
} Function;

void Function__ctor(Function* self, FuncDecl_ decl, py_GlobalRef module, py_Ref globals);
void Function__dtor(Function* self);

// https://github.com/tinypy/tinypy/issues/456
// Function may be created from `execdyn` and return
// Weakrefs like `.globals` and `.clazz` may invalidate
