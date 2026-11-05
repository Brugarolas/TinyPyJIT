#pragma once

#include "tinypy/common/vector.h"
#include "tinypy/objects/object.h"

typedef struct ShapeArena ShapeArena;

#define PK_MAGIC_CACHE_SIZE 8

typedef struct py_MagicCacheEntry {
    py_Name name;
    py_ItemRef value;
    uint64_t version;
} py_MagicCacheEntry;

typedef struct py_TypeInfo {
    py_Name name;
    py_Type index;
    py_Type base;
    struct py_TypeInfo* base_ti;

    py_TValue self;
    py_GlobalRef module;

    bool is_python;  // is it a python class? (not derived from c object)
    bool is_final;  // can it be subclassed?

    bool (*getattribute)(py_Ref self, py_Name name) PY_RAISE PY_RETURN;
    bool (*setattribute)(py_Ref self, py_Name name, py_Ref val) PY_RAISE;
    bool (*delattribute)(py_Ref self, py_Name name) PY_RAISE;
    bool (*getunboundmethod)(py_Ref self, py_Name name) PY_RETURN;

    // Resolved `__new__`/`__init__`, valid while `magics_version` matches
    // `vm->type_version`. `cached_init` is nil when the type has no `__init__`.
    py_TValue cached_new;
    py_TValue cached_init;
    uint64_t magics_version;

    /* Lazily allocated transition tree for Python-instance attributes. */
    ShapeArena* shape_arena;

    /*
     * Small direct-mapped cache for magic-method resolution.
     *
     * Entries point directly into a type NameDict. Any write to any type dict
     * increments VM::type_version, invalidating all cached pointers before a
     * potentially reallocating dictionary mutation can make them observable.
     * Misses are cached by storing value == NULL.
     */
    py_MagicCacheEntry magic_cache[PK_MAGIC_CACHE_SIZE];

    py_TValue annotations;
    py_Dtor dtor;  // destructor for this type, NULL if no dtor
    void (*on_end_subclass)(struct py_TypeInfo*);  // backdoor for enum module
} py_TypeInfo;

py_TypeInfo* pk_typeinfo(py_Type type);
py_ItemRef pk_tpfindname(py_TypeInfo* ti, py_Name name);
py_ItemRef pk_tpfindmagic_cached(py_TypeInfo* ti, py_Name name);
/// Re-resolve `cached_new`/`cached_init` against the current `type_version`.
void pk_tpresolvemagics(py_TypeInfo* ti);
ShapeArena* pk_type_shape_arena(py_Type type);
#define pk_tpfindmagic pk_tpfindmagic_cached

py_Type pk_newtype(const char* name,
                   py_Type base,
                   const py_GlobalRef module,
                   void (*dtor)(void*),
                   bool is_python,
                   bool is_final);

py_Type pk_newtypewithmode(py_Name name,
                           py_Type base,
                           const py_GlobalRef module,
                           void (*dtor)(void*),
                           bool is_python,
                           bool is_final,
                           enum py_CompileMode mode);
