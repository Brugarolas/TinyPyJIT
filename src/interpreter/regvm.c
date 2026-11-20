#include "tinypy/interpreter/regvm.h"
#include "tinypy/interpreter/jitbridge.h"
#include "tinypy/interpreter/regprofile.h"
#include "tinypy/interpreter/perf.h"
#include "tinypy/interpreter/shape.h"
#include "tinypy/interpreter/regvm_slow.h"

#include "tinypy/common/sstream.h"
#include "tinypy/common/utils.h"
#include "tinypy/interpreter/fastpath.h"
#include "tinypy/interpreter/bindings.h"
#include "tinypy/interpreter/typeinfo.h"
#include "tinypy/objects/namedict.h"
#include "tinypy/objects/object.h"
#include "tinypy/objects/error.h"
#include "tinypy/objects/exception.h"
#include "tinypy/tinypy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Select the interpreter at build time. The register engine is the default. */
#ifndef PK_ENABLE_REGVM
#define PK_ENABLE_REGVM 1
#endif

#if PK_ENABLE_REGVM != 0 && PK_ENABLE_REGVM != 1
#error "PK_ENABLE_REGVM must be 0 (stack) or 1 (register)"
#endif

static PK_INLINE bool reg_isnum(const py_TValue* v) {
    return v->type == tp_int || v->type == tp_float;
}

static PK_INLINE py_f64 reg_tof64(const py_TValue* v) {
    return v->type == tp_int ? (py_f64)v->_i64 : v->_f64;
}

static PK_INLINE uintptr_t reg_shape_identity(py_Ref value) {
    if(value == NULL || !value->is_ptr || value->_obj == NULL ||
       value->_obj->slots != PK_OBJ_SHAPED_SLOTS)
        return (uintptr_t)0;
    ShapeStorage* storage = ShapeObject__storage(value->_obj);
    return storage == NULL ? (uintptr_t)0 : (uintptr_t)ShapeStorage__shape(storage);
}

static PK_INLINE uintptr_t reg_call_identity(py_Ref callable) {
    if(callable->type == tp_nativefunc) return (uintptr_t)callable->_cfunc;
    if(callable->type == tp_function) return (uintptr_t)callable->_obj;
    return (uintptr_t)(uint16_t)callable->type;
}


static PK_INLINE RegRuntimeSite* reg_runtime_site(RegCode* code, int32_t pc) {
    assert(pc >= 0 && pc < code->runtime_sites.length);
    return c11__at(RegRuntimeSite, &code->runtime_sites, pc);
}

static PK_INLINE void reg_rewrite_opcode(RegCode* code, int32_t pc, RegOpcode op) {
    RegBytecode* word = c11__at(RegBytecode, &code->code, pc);
    *word = (*word & ~UINT32_C(0xFF)) | (uint32_t)op;
}

static PK_INLINE uint32_t reg_pack_types(py_Type lhs, py_Type rhs) {
    return (uint32_t)(uint16_t)lhs | ((uint32_t)(uint16_t)rhs << 16);
}

static bool reg_observe(RegRuntimeSite* site, uint32_t profile) {
    if(site->profile == profile) {
        if(site->streak != UINT8_MAX) site->streak++;
    } else {
        site->profile = profile;
        site->streak = 1;
        site->misses = 0;
    }
    return site->streak >= REG_ADAPTIVE_WARMUP;
}

static RegInlineCache* reg_cache_at(RegCode* code, const RegRuntimeSite* site) {
    if(site->cache_index == REG_IC_NONE || site->cache_index >= (uint32_t)code->inline_caches.length)
        return NULL;
    return c11__at(RegInlineCache, &code->inline_caches, (int)site->cache_index);
}

static RegInlineCache* reg_cache_alloc(RegCode* code,
                                       RegRuntimeSite* site,
                                       RegInlineCacheKind kind) {
    RegInlineCache* cache = reg_cache_at(code, site);
    if(cache == NULL) {
        if(code->inline_caches.length >= (int)UINT16_MAX) return NULL;
        cache = c11_vector__emplace(&code->inline_caches);
        memset(cache, 0, sizeof(*cache));
        site->cache_index = (uint16_t)(code->inline_caches.length - 1);
    } else {
        memset(cache, 0, sizeof(*cache));
    }
    cache->kind = (uint8_t)kind;
    return cache;
}

static void reg_mark_quickened(RegCode* code, int32_t pc, RegOpcode op) {
    RegOpcode old = RBC_OP(c11__getitem(RegBytecode, &code->code, pc));
    if(old == op) return;
    reg_rewrite_opcode(code, pc, op);
    code->quickened_count++;
    RegRuntimeSite* site = reg_runtime_site(code, pc);
    site->misses = 0;
}

void RegVM__prepare_jit_caches(RegCode* code) {
    for(int pc = 0; pc < code->code.length; pc++) {
        RegRuntimeSite* site = reg_runtime_site(code, pc);
        if(site->cache_index != REG_IC_NONE) continue;
        RegInlineCacheKind kind;
        switch(RBC_OP(c11__getitem(RegBytecode, &code->code, pc))) {
            case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC:
            case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC:
            case RBC_STORE_GLOBAL: kind = REG_IC_KIND_GLOBAL; break;
            case RBC_LOAD_ATTR: case RBC_LOAD_ATTR_IC: kind = REG_IC_KIND_ATTR; break;
            case RBC_STORE_ATTR: case RBC_STORE_ATTR_IC: kind = REG_IC_KIND_STORE_ATTR; break;
            case RBC_LOAD_METHOD: case RBC_LOAD_METHOD_IC: kind = REG_IC_KIND_METHOD; break;
            default: continue;
        }
        (void)reg_cache_alloc(code, site, kind);
    }
}

static void reg_dequicken(RegCode* code, int32_t pc, RegOpcode base) {
    RegOpcode old = RBC_OP(c11__getitem(RegBytecode, &code->code, pc));
    if(old != base) {
        reg_rewrite_opcode(code, pc, base);
        if(code->quickened_count) code->quickened_count--;
    }
    RegRuntimeSite* site = reg_runtime_site(code, pc);
    site->streak = 0;
    site->misses = 0;
    site->profile = 0;
}

static bool reg_specialization_miss(RegCode* code, int32_t pc, RegOpcode base) {
    RegRuntimeSite* site = reg_runtime_site(code, pc);
    if(site->misses != UINT8_MAX) site->misses++;
    if(site->misses >= REG_ADAPTIVE_MISS_LIMIT) {
        reg_dequicken(code, pc, base);
        return true;
    }
    return false;
}

static RegOpcode reg_int_specialization(RegOpcode op) {
    switch(op) {
        case RBC_ADD: return RBC_ADD_INT;
        case RBC_SUB: return RBC_SUB_INT;
        case RBC_MUL: return RBC_MUL_INT;
        case RBC_CMP_LT: return RBC_CMP_LT_INT;
        case RBC_CMP_LE: return RBC_CMP_LE_INT;
        case RBC_CMP_EQ: return RBC_CMP_EQ_INT;
        case RBC_CMP_NE: return RBC_CMP_NE_INT;
        case RBC_CMP_GT: return RBC_CMP_GT_INT;
        case RBC_CMP_GE: return RBC_CMP_GE_INT;
        default: return RBC_NOP;
    }
}

static RegOpcode reg_num_specialization(RegOpcode op) {
    switch(op) {
        case RBC_ADD: return RBC_ADD_NUM;
        case RBC_SUB: return RBC_SUB_NUM;
        case RBC_MUL: return RBC_MUL_NUM;
        case RBC_CMP_LT: return RBC_CMP_LT_NUM;
        case RBC_CMP_LE: return RBC_CMP_LE_NUM;
        case RBC_CMP_EQ: return RBC_CMP_EQ_NUM;
        case RBC_CMP_NE: return RBC_CMP_NE_NUM;
        case RBC_CMP_GT: return RBC_CMP_GT_NUM;
        case RBC_CMP_GE: return RBC_CMP_GE_NUM;
        default: return RBC_NOP;
    }
}

static void reg_maybe_quicken_binary(RegCode* code,
                                     int32_t pc,
                                     RegOpcode op,
                                     py_Type lhs_type,
                                     py_Type rhs_type) {
    RegOpcode specialized = RBC_NOP;
    if(lhs_type == tp_int && rhs_type == tp_int) {
        specialized = reg_int_specialization(op);
    } else if((lhs_type == tp_int || lhs_type == tp_float) &&
              (rhs_type == tp_int || rhs_type == tp_float)) {
        specialized = reg_num_specialization(op);
    }
    if(specialized == RBC_NOP) return;

    RegRuntimeSite* site = reg_runtime_site(code, pc);
    if(reg_observe(site, reg_pack_types(lhs_type, rhs_type))) {
        reg_mark_quickened(code, pc, specialized);
    }
}

static RegOpcode reg_specialized_base(RegOpcode op) {
    switch(op) {
        case RBC_ADD_INT:
        case RBC_ADD_NUM: return RBC_ADD;
        case RBC_SUB_INT:
        case RBC_SUB_NUM: return RBC_SUB;
        case RBC_MUL_INT:
        case RBC_MUL_NUM: return RBC_MUL;
        case RBC_CMP_LT_INT:
        case RBC_CMP_LT_NUM: return RBC_CMP_LT;
        case RBC_CMP_LE_INT:
        case RBC_CMP_LE_NUM: return RBC_CMP_LE;
        case RBC_CMP_EQ_INT:
        case RBC_CMP_EQ_NUM: return RBC_CMP_EQ;
        case RBC_CMP_NE_INT:
        case RBC_CMP_NE_NUM: return RBC_CMP_NE;
        case RBC_CMP_GT_INT:
        case RBC_CMP_GT_NUM: return RBC_CMP_GT;
        case RBC_CMP_GE_INT:
        case RBC_CMP_GE_NUM: return RBC_CMP_GE;
        default: return RBC_NOP;
    }
}


static bool reg_is_int_specialization(RegOpcode op) {
    switch(op) {
        case RBC_ADD_INT:
        case RBC_SUB_INT:
        case RBC_MUL_INT:
        case RBC_CMP_LT_INT:
        case RBC_CMP_LE_INT:
        case RBC_CMP_EQ_INT:
        case RBC_CMP_NE_INT:
        case RBC_CMP_GT_INT:
        case RBC_CMP_GE_INT: return true;
        default: return false;
    }
}

static bool reg_is_num_specialization(RegOpcode op) {
    switch(op) {
        case RBC_ADD_NUM:
        case RBC_SUB_NUM:
        case RBC_MUL_NUM:
        case RBC_CMP_LT_NUM:
        case RBC_CMP_LE_NUM:
        case RBC_CMP_EQ_NUM:
        case RBC_CMP_NE_NUM:
        case RBC_CMP_GT_NUM:
        case RBC_CMP_GE_NUM: return true;
        default: return false;
    }
}

static bool reg_compare_i64(RegOpcode op, py_i64 a, py_i64 b) {
    switch(op) {
        case RBC_CMP_LT:
        case RBC_CMP_LT_INT:
        case RBC_CMP_LT_NUM:
        case RBC_CMP_LT_I8: return a < b;
        case RBC_CMP_LE:
        case RBC_CMP_LE_INT:
        case RBC_CMP_LE_NUM:
        case RBC_CMP_LE_I8: return a <= b;
        case RBC_CMP_EQ:
        case RBC_CMP_EQ_INT:
        case RBC_CMP_EQ_NUM:
        case RBC_CMP_EQ_I8: return a == b;
        case RBC_CMP_NE:
        case RBC_CMP_NE_INT:
        case RBC_CMP_NE_NUM:
        case RBC_CMP_NE_I8: return a != b;
        case RBC_CMP_GT:
        case RBC_CMP_GT_INT:
        case RBC_CMP_GT_NUM:
        case RBC_CMP_GT_I8: return a > b;
        case RBC_CMP_GE:
        case RBC_CMP_GE_INT:
        case RBC_CMP_GE_NUM:
        case RBC_CMP_GE_I8: return a >= b;
        default: c11__unreachable();
    }
}

static bool reg_compare_f64(RegOpcode op, py_f64 a, py_f64 b) {
    switch(op) {
        case RBC_CMP_LT:
        case RBC_CMP_LT_INT:
        case RBC_CMP_LT_NUM:
        case RBC_CMP_LT_I8: return a < b;
        case RBC_CMP_LE:
        case RBC_CMP_LE_INT:
        case RBC_CMP_LE_NUM:
        case RBC_CMP_LE_I8: return a <= b;
        case RBC_CMP_EQ:
        case RBC_CMP_EQ_INT:
        case RBC_CMP_EQ_NUM:
        case RBC_CMP_EQ_I8: return a == b;
        case RBC_CMP_NE:
        case RBC_CMP_NE_INT:
        case RBC_CMP_NE_NUM:
        case RBC_CMP_NE_I8: return a != b;
        case RBC_CMP_GT:
        case RBC_CMP_GT_INT:
        case RBC_CMP_GT_NUM:
        case RBC_CMP_GT_I8: return a > b;
        case RBC_CMP_GE:
        case RBC_CMP_GE_INT:
        case RBC_CMP_GE_NUM:
        case RBC_CMP_GE_I8: return a >= b;
        default: c11__unreachable();
    }
}

static RegOpcode reg_compare_code_opcode(uint8_t code) {
    switch(code & REG_CMP_BRANCH_CODE_MASK) {
        case REG_CMP_LT: return RBC_CMP_LT;
        case REG_CMP_LE: return RBC_CMP_LE;
        case REG_CMP_EQ: return RBC_CMP_EQ;
        case REG_CMP_NE: return RBC_CMP_NE;
        case REG_CMP_GT: return RBC_CMP_GT;
        case REG_CMP_GE: return RBC_CMP_GE;
        default: c11__unreachable();
    }
}

static NameDict* reg_namedict(py_Ref value) {
    if(value == NULL || !value->is_ptr || value->_obj == NULL || value->_obj->slots != -1) return NULL;
    return PyObject__dict(value->_obj);
}

static bool reg_global_cache_fill(VM* self,
                                  py_Frame* frame,
                                  py_Name name,
                                  RegInlineCache* cache) {
    if(frame->globals == NULL || frame->globals->type != tp_module) return false;
    NameDict* globals = reg_namedict(frame->globals);
    NameDict* builtins = reg_namedict(self->builtins);
    if(globals == NULL || builtins == NULL) return false;

    memset(cache, 0, sizeof(*cache));
    cache->kind = REG_IC_KIND_GLOBAL;
    cache->guard0 = (uintptr_t)globals;
    cache->version0 = globals->version;

    int index = NameDict__find_index(globals, name);
    if(index >= 0) {
        cache->variant = REG_GLOBAL_CACHE_GLOBAL;
        cache->index0 = (uint32_t)index;
        return true;
    }

    index = NameDict__find_index(builtins, name);
    if(index < 0) return false;
    cache->variant = REG_GLOBAL_CACHE_BUILTIN;
    cache->guard1 = (uintptr_t)builtins;
    cache->version1 = builtins->version;
    cache->index1 = (uint32_t)index;
    return true;
}

static PK_INLINE bool reg_global_cache_apply(VM* self,
                                   py_Frame* frame,
                                   py_Name name,
                                   const RegInlineCache* cache,
                                   py_Ref out) {
    if(cache == NULL || cache->kind != REG_IC_KIND_GLOBAL ||
       frame->globals == NULL || frame->globals->type != tp_module)
        return false;

    NameDict* globals = reg_namedict(frame->globals);
    if(globals == NULL || (uintptr_t)globals != cache->guard0 || globals->version != cache->version0)
        return false;

    if(cache->variant == REG_GLOBAL_CACHE_GLOBAL) {
        if(cache->index0 >= (uint32_t)globals->capacity) return false;
        NameDict_KV* kv = &globals->items[cache->index0];
        if(kv->key != name) return false;
        *out = kv->value;
        return true;
    }

    if(cache->variant == REG_GLOBAL_CACHE_BUILTIN) {
        NameDict* builtins = reg_namedict(self->builtins);
        if(builtins == NULL || (uintptr_t)builtins != cache->guard1 ||
           builtins->version != cache->version1 || cache->index1 >= (uint32_t)builtins->capacity)
            return false;
        NameDict_KV* kv = &builtins->items[cache->index1];
        if(kv->key != name) return false;
        *out = kv->value;
        return true;
    }

    return false;
}

static bool reg_attr_cache_fill(VM* self,
                                py_Ref object,
                                py_Name name,
                                RegInlineCache* cache) {
    if(object->type == tp_type || object->type == tp_super) return false;
    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getattribute != NULL) return false;

    py_Ref cls_var = pk_tpfindname(ti, name);
    uint8_t variant = REG_ATTR_CACHE_NONE;
    ShapeStorage* shaped = NULL;
    int shaped_slot = -1;

    /* Data descriptors still win over instance storage. */
    if(cls_var != NULL && cls_var->type == tp_property) {
        variant = REG_ATTR_CACHE_PROPERTY;
    } else {
        if(object->is_ptr && object->_obj->slots == PK_OBJ_SHAPED_SLOTS) {
            shaped = ShapeObject__storage(object->_obj);
            if(shaped != NULL) shaped_slot = Shape__lookup(ShapeStorage__shape(shaped), name);
            if(shaped_slot >= 0 && !py_isnil(ShapeStorage__slot(shaped, (uint16_t)shaped_slot))) {
                variant = REG_ATTR_CACHE_SHAPE_SLOT;
            }
        }

        if(variant == REG_ATTR_CACHE_NONE && cls_var != NULL) {
            switch(cls_var->type) {
                case tp_function: variant = REG_ATTR_CACHE_FUNCTION; break;
                case tp_nativefunc: variant = REG_ATTR_CACHE_NATIVE_FUNCTION; break;
                case tp_staticmethod: variant = REG_ATTR_CACHE_STATICMETHOD; break;
                case tp_classmethod: variant = REG_ATTR_CACHE_CLASSMETHOD; break;
                default: variant = REG_ATTR_CACHE_CLASS_VALUE; break;
            }
        } else if(variant == REG_ATTR_CACHE_NONE && object->is_ptr && object->_obj->slots == -1) {
            variant = REG_ATTR_CACHE_INSTANCE_FIRST;
        } else if(variant == REG_ATTR_CACHE_NONE) {
            return false;
        }
    }

    memset(cache, 0, sizeof(*cache));
    cache->kind = REG_IC_KIND_ATTR;
    cache->variant = variant;
    cache->type = object->type;
    cache->version0 = self->type_version;
    cache->slot = cls_var;
    if(variant == REG_ATTR_CACHE_SHAPE_SLOT) {
        cache->guard0 = (uintptr_t)ShapeStorage__shape(shaped);
        cache->index0 = (uint32_t)shaped_slot;
    }
    return true;
}

/* 1 = hit, 0 = guarded fallback, -1 = exception. */
static PK_INLINE int reg_attr_cache_apply(VM* self,
                                py_Ref object,
                                py_Name name,
                                const RegInlineCache* cache,
                                py_Ref out) {
    if(cache == NULL || cache->kind != REG_IC_KIND_ATTR || object->type != cache->type ||
       self->type_version != cache->version0)
        return 0;

    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getattribute != NULL || object->type == tp_type || object->type == tp_super) return 0;

    if(cache->variant == REG_ATTR_CACHE_PROPERTY) {
        py_Ref getter = py_getslot(cache->slot, 0);
        if(!py_call(getter, 1, object)) return -1;
        *out = self->last_retval;
        return 1;
    }

    if(cache->variant == REG_ATTR_CACHE_SHAPE_SLOT) {
        if(!object->is_ptr || object->_obj->slots != PK_OBJ_SHAPED_SLOTS) return 0;
        ShapeStorage* shaped = ShapeObject__storage(object->_obj);
        if(shaped == NULL || (uintptr_t)ShapeStorage__shape(shaped) != cache->guard0) return 0;
        py_Ref value = ShapeStorage__slot(shaped, (uint16_t)cache->index0);
        if(value == NULL || py_isnil(value)) return 0;
        *out = *value;
        return 1;
    }

    if(object->is_ptr && (object->_obj->slots == -1 || object->_obj->slots == PK_OBJ_SHAPED_SLOTS)) {
        py_Ref value = py_getdict(object, name);
        if(value != NULL) {
            *out = *value;
            return 1;
        }
    }

    switch(cache->variant) {
        case REG_ATTR_CACHE_FUNCTION:
        case REG_ATTR_CACHE_NATIVE_FUNCTION:
            py_newboundmethod(out, object, cache->slot);
            return 1;
        case REG_ATTR_CACHE_STATICMETHOD:
            *out = *py_getslot(cache->slot, 0);
            return 1;
        case REG_ATTR_CACHE_CLASSMETHOD:
            py_newboundmethod(out, &ti->self, py_getslot(cache->slot, 0));
            return 1;
        case REG_ATTR_CACHE_CLASS_VALUE:
            *out = *cache->slot;
            return 1;
        case REG_ATTR_CACHE_INSTANCE_FIRST:
        default: return 0;
    }
}

/*
 * STORE_ATTR inline cache over shapes. Filled after a successful generic store
 * from the shape observed before and after it: an unchanged shape is a plain
 * slot store, a one-step child is a transition. Types with setattr hooks or a
 * data descriptor (property) of that name are never cached; type_version
 * guards both facts.
 */
static bool reg_store_attr_cache_fill(VM* self,
                                      py_Ref object,
                                      py_Name name,
                                      Shape* before,
                                      RegInlineCache* cache) {
    if(before == NULL || !object->is_ptr || object->_obj->slots != PK_OBJ_SHAPED_SLOTS) return false;
    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->setattribute != NULL) return false;
    py_Ref cls_var = pk_tpfindname(ti, name);
    if(cls_var != NULL && cls_var->type == tp_property) return false;

    Shape* after = ShapeStorage__shape(ShapeObject__storage(object->_obj));
    uint8_t variant;
    int slot;
    if(after == before) {
        slot = Shape__lookup(before, name);
        variant = REG_STORE_ATTR_CACHE_SLOT;
    } else if(after->parent == before && after->added_name == name) {
        slot = after->added_slot;
        variant = REG_STORE_ATTR_CACHE_TRANSITION;
    } else {
        return false;
    }
    if(slot < 0) return false;

    memset(cache, 0, sizeof(*cache));
    cache->kind = REG_IC_KIND_STORE_ATTR;
    cache->variant = variant;
    cache->type = object->type;
    cache->version0 = self->type_version;
    cache->guard0 = (uintptr_t)before;
    cache->guard1 = (uintptr_t)after;
    cache->index0 = (uint32_t)slot;
    return true;
}

static PK_INLINE bool reg_store_attr_cache_apply(VM* self,
                                       py_Ref object,
                                       py_Ref value,
                                       const RegInlineCache* cache) {
    if(cache == NULL || cache->kind != REG_IC_KIND_STORE_ATTR || object->type != cache->type ||
       self->type_version != cache->version0 || !object->is_ptr ||
       object->_obj->slots != PK_OBJ_SHAPED_SLOTS)
        return false;
    ShapeStorage* storage = ShapeObject__storage(object->_obj);
    if((uintptr_t)storage->shape != cache->guard0) return false;
    if(cache->variant == REG_STORE_ATTR_CACHE_TRANSITION) {
        if(cache->index0 >= storage->capacity) return false; /* growth: generic path */
        storage->shape = (Shape*)cache->guard1;
    } else if(cache->variant != REG_STORE_ATTR_CACHE_SLOT) {
        return false;
    }
    storage->values[cache->index0] = *value;
    return true;
}

static bool reg_method_cache_fill(VM* self,
                                  py_Ref object,
                                  py_Name name,
                                  RegInlineCache* cache) {
    if(name == __new__ || object->type == tp_super) return false;
    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getunboundmethod != NULL) return false;

    py_Ref cls_var = pk_tpfindname(ti, name);
    if(cls_var == NULL) {
        /* `module.function(...)`: the value lives in the module namespace. */
        if(object->type != tp_module || !object->is_ptr || object->_obj->slots != -1 ||
           ti->getattribute != NULL)
            return false;
        NameDict* dict = PyObject__dict(object->_obj);
        int index = NameDict__find_index(dict, name);
        if(index < 0) return false;
        memset(cache, 0, sizeof(*cache));
        cache->kind = REG_IC_KIND_METHOD;
        cache->variant = REG_METHOD_CACHE_MODULE_ATTR;
        cache->type = tp_module;
        cache->version0 = self->type_version;
        cache->guard0 = (uintptr_t)dict;
        cache->version1 = dict->version;
        cache->index0 = (uint32_t)index;
        return true;
    }
    /* An instance attribute shadows the method (non-data descriptor). */
    if(object->type != tp_type && object->is_ptr && PyObject__has_namespace(object->_obj) &&
       py_getdict(object, name) != NULL)
        return false;

    uint8_t variant;
    switch(cls_var->type) {
        case tp_function:
        case tp_nativefunc: variant = REG_METHOD_CACHE_BIND_SELF; break;
        case tp_staticmethod: variant = REG_METHOD_CACHE_STATIC; break;
        case tp_classmethod: variant = REG_METHOD_CACHE_CLASS; break;
        default: return false;
    }

    memset(cache, 0, sizeof(*cache));
    cache->kind = REG_IC_KIND_METHOD;
    cache->variant = variant;
    cache->type = object->type;
    cache->version0 = self->type_version;
    cache->slot = cls_var;
    cache->guard0 = reg_shape_identity(object); /* last shape known not to shadow `name` */
    return true;
}

/*
 * True when the instance has no attribute named `name` that would shadow the
 * cached method. Shapes record which names exist, so a shape already verified
 * once (cache->guard0) needs no lookup at all.
 */
static PK_INLINE bool reg_method_not_shadowed(py_Ref object, py_Name name, RegInlineCache* cache) {
    if(!object->is_ptr || object->type == tp_type) return true;
    PyObject* obj = object->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) {
        Shape* shape = ShapeStorage__shape(ShapeObject__storage(obj));
        if((uintptr_t)shape == cache->guard0) return true;
        if(Shape__lookup(shape, name) >= 0) return false;
        cache->guard0 = (uintptr_t)shape;
        return true;
    }
    if(obj->slots == PK_OBJ_DICT_SLOTS) return py_getdict(object, name) == NULL;
    return true;
}

static PK_INLINE bool reg_method_cache_apply(VM* self,
                                   py_Ref object,
                                   py_Name name,
                                   RegInlineCache* cache,
                                   py_Ref out_callable,
                                   py_Ref out_self) {
    if(cache == NULL || cache->kind != REG_IC_KIND_METHOD || object->type != cache->type ||
       self->type_version != cache->version0)
        return false;

    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getunboundmethod != NULL || object->type == tp_super) return false;
    if(cache->variant == REG_METHOD_CACHE_MODULE_ATTR) {
        if(!object->is_ptr || object->_obj->slots != -1 || ti->getattribute != NULL) return false;
        NameDict* dict = PyObject__dict(object->_obj);
        if((uintptr_t)dict != cache->guard0 || dict->version != cache->version1 ||
           cache->index0 >= (uint32_t)dict->capacity || dict->items[cache->index0].key != name)
            return false;
        *out_callable = dict->items[cache->index0].value;
        py_newnil(out_self);
        return true;
    }
    if(!reg_method_not_shadowed(object, name, cache)) return false;

    switch(cache->variant) {
        case REG_METHOD_CACHE_BIND_SELF:
            *out_callable = *cache->slot;
            *out_self = *object;
            return true;
        case REG_METHOD_CACHE_STATIC:
            *out_callable = *py_getslot(cache->slot, 0);
            py_newnil(out_self);
            return true;
        case REG_METHOD_CACHE_CLASS:
            *out_callable = *py_getslot(cache->slot, 0);
            *out_self = ti->self;
            return true;
        default: return false;
    }
}

static bool reg_call_cache_fill(py_Ref callable,
                                uint8_t argc,
                                uint8_t kwargc,
                                RegInlineCache* cache) {
    if(kwargc != 0) return false;
    memset(cache, 0, sizeof(*cache));
    cache->kind = REG_IC_KIND_CALL;
    cache->argc = argc;

    if(callable->type == tp_nativefunc) {
        cache->variant = REG_CALL_CACHE_NATIVE;
        cache->guard0 = (uintptr_t)callable->_cfunc;
        return true;
    }

    if(callable->type == tp_type) {
        /* Instantiation: object.__new__ followed by a simple Python __init__. */
        py_TypeInfo* ti = py_touserdata(callable);
        VM* vm = pk_current_vm;
        if(ti->magics_version != vm->type_version) pk_tpresolvemagics(ti);
        if(ti->cached_new.type == tp_nativefunc && ti->cached_new._cfunc != pk__object_new &&
           py_isnil(&ti->cached_init)) {
            cache->variant = REG_CALL_CACHE_NATIVE_TYPE;
            cache->version0 = vm->type_version;
            cache->guard0 = (uintptr_t)callable->_obj;
            cache->guard1 = (uintptr_t)ti->cached_new._cfunc;
            return true;
        }
        if(!ti->is_python || ti->cached_new.type != tp_nativefunc ||
           ti->cached_new._cfunc != pk__object_new || ti->cached_init.type != tp_function)
            return false;
        Function* init = py_touserdata(&ti->cached_init);
        if(init->decl == NULL || init->decl->type != FuncType_SIMPLE || init->cfunc != NULL ||
           init->decl->args.length != (int)argc + 1)
            return false;
        cache->variant = REG_CALL_CACHE_CONSTRUCT;
        cache->type = ti->index;
        cache->version0 = vm->type_version;
        cache->guard0 = (uintptr_t)callable->_obj;
        cache->guard1 = (uintptr_t)init->decl;
        cache->slot = &ti->cached_init;
        return true;
    }

    if(callable->type != tp_function) return false;
    Function* fn = py_touserdata(callable);
    if(fn->decl == NULL || fn->decl->type != FuncType_SIMPLE) return false;
    cache->variant = fn->cfunc ? REG_CALL_CACHE_DECL_CFUNC_SIMPLE : REG_CALL_CACHE_PY_SIMPLE;
    cache->guard0 = (uintptr_t)callable->_obj;
    cache->guard1 = (uintptr_t)fn->decl;
    return true;
}

static PK_INLINE FrameResult reg_run_callee(VM* self);

static PK_INLINE bool reg_call_simple_prepare(VM* self,
                                    py_TValue* frame_limit,
                                    py_Ref callbase,
                                    uint8_t argc,
                                    Function* fn,
                                    FrameResult* result) {
    const CodeObject* co = &fn->decl->code;
    bool self_is_nil = py_isnil(callbase + 1);
    int actual_argc = (int)argc + (self_is_nil ? 0 : 1);
    if(actual_argc != fn->decl->args.length) return false;

    uint16_t window = (uint16_t)(2u + argc);
    if(frame_limit + window > self->stack.end) {
        py_exception(tp_RecursionError, "value stack overflow");
        *result = RES_ERROR;
        return true;
    }

    py_StackRef p0 = frame_limit;
    memcpy(p0, callbase, (size_t)window * sizeof(py_TValue));
    py_StackRef p1 = p0 + window;
    py_StackRef argv = p0 + 1 + (int)self_is_nil;
    self->curr_function = p0;

    int frame_slots = fn->cfunc ? co->nlocals : RegVM__frame_slots(fn->decl);
    if(argv + frame_slots > self->stack.end) {
        py_exception(tp_RecursionError, "value stack overflow");
        self->stack.sp = frame_limit;
        *result = RES_ERROR;
        return true;
    }

    self->stack.sp = argv + frame_slots;
    if(p1 < self->stack.sp) {
        memset(p1, 0, (size_t)((char*)self->stack.sp - (char*)p1));
    }

    if(fn->cfunc != NULL) {
        bool ok = py_callcfunc(fn->cfunc, co->nlocals, argv);
        self->stack.sp = frame_limit;
        *result = ok ? RES_RETURN : RES_ERROR;
        return true;
    }

    py_Frame frame;
    Frame__init_native(&frame, co, p0, fn->module, &fn->globals, argv);
    VM__push_frame(self, &frame);
    *result = reg_run_callee(self);
    return true;
}

static PK_INLINE FrameResult reg_run_callee(VM* self);

/*
 * REG_CALL_CACHE_CONSTRUCT: the same steps as VM__vectorcall() on a type
 * (object.__new__, then __init__), without re-resolving either. The new
 * instance replaces the class in the caller's register, which keeps it rooted
 * while __init__ runs and is where the result belongs anyway.
 */
static PK_REGVM_NOINLINE bool reg_call_construct(VM* self,
                                                 py_TValue* frame_limit,
                                                 py_Ref callbase,
                                                 uint8_t argc,
                                                 const RegInlineCache* cache,
                                                 FrameResult* result) {
    Function* init = py_touserdata(cache->slot);
#if PK_ENABLE_JIT
    if(RegJit__construct(self, cache->type, argc, callbase + 2)) {
        *callbase = self->last_retval;
        *result = RES_RETURN;
        return true;
    }
#endif
    const uint16_t window = (uint16_t)(2u + argc);
    int frame_slots = RegVM__frame_slots(init->decl);
    if(frame_limit + 1 + frame_slots > self->stack.end || frame_limit + window > self->stack.end) {
        py_exception(tp_RecursionError, "value stack overflow");
        *result = RES_ERROR;
        return true;
    }

    py_newobject(callbase, cache->type, PK_OBJ_SHAPED_SLOTS, 0);
    py_StackRef p0 = frame_limit;
    p0[0] = *cache->slot; /* __init__ */
    p0[1] = *callbase;    /* self */
    memcpy(p0 + 2, callbase + 2, (size_t)argc * sizeof(py_TValue));
    py_StackRef argv = p0 + 1;
    py_StackRef p1 = p0 + window;
    self->curr_function = p0;
    self->stack.sp = argv + frame_slots;
    if(p1 < self->stack.sp) memset(p1, 0, (size_t)((char*)self->stack.sp - (char*)p1));

    py_Frame frame;
    Frame__init_native(&frame, &init->decl->code, p0, init->module, &init->globals, argv);
    VM__push_frame(self, &frame);
    FrameResult res = reg_run_callee(self);
    self->stack.sp = frame_limit;
    if(res == RES_ERROR) {
        *result = RES_ERROR;
        return true;
    }
    self->last_retval = *callbase; /* the instance, not __init__'s None */
    *result = RES_RETURN;
    return true;
}

static PK_INLINE bool reg_call_cache_apply(VM* self,
                                 py_TValue* frame_limit,
                                 py_Ref callbase,
                                 uint8_t argc,
                                 uint8_t kwargc,
                                 const RegInlineCache* cache,
                                 FrameResult* result) {
    if(cache == NULL || cache->kind != REG_IC_KIND_CALL || cache->argc != argc || kwargc != 0)
        return false;

    switch(cache->variant) {
        case REG_CALL_CACHE_NATIVE: {
            if(callbase->type != tp_nativefunc || (uintptr_t)callbase->_cfunc != cache->guard0)
                return false;
            bool self_is_nil = py_isnil(callbase + 1);
            py_Ref argv = callbase + 1 + (int)self_is_nil;
            int effective_argc = (int)argc + (self_is_nil ? 0 : 1);
            self->curr_function = callbase;
            bool ok = py_callcfunc(callbase->_cfunc, effective_argc, argv);
            *result = ok ? RES_RETURN : RES_ERROR;
            return true;
        }
        case REG_CALL_CACHE_PY_SIMPLE:
        case REG_CALL_CACHE_DECL_CFUNC_SIMPLE: {
            if(callbase->type != tp_function || (uintptr_t)callbase->_obj != cache->guard0)
                return false;
            Function* fn = py_touserdata(callbase);
            if((uintptr_t)fn->decl != cache->guard1 || fn->decl->type != FuncType_SIMPLE)
                return false;
            if((cache->variant == REG_CALL_CACHE_PY_SIMPLE) != (fn->cfunc == NULL)) return false;
            return reg_call_simple_prepare(self, frame_limit, callbase, argc, fn, result);
        }
        case REG_CALL_CACHE_CONSTRUCT:
            if(callbase->type != tp_type || (uintptr_t)callbase->_obj != cache->guard0 ||
               self->type_version != cache->version0 || !py_isnil(callbase + 1))
                return false;
            return reg_call_construct(self, frame_limit, callbase, argc, cache, result);
        case REG_CALL_CACHE_NATIVE_TYPE: {
            /* VM__vectorcall() on such a type: __new__(cls, *args) and no __init__. */
            if(callbase->type != tp_type || (uintptr_t)callbase->_obj != cache->guard0 ||
               self->type_version != cache->version0 || !py_isnil(callbase + 1))
                return false;
            callbase[1] = callbase[0];
            self->curr_function = callbase;
            py_CFunction new_fn = (py_CFunction)cache->guard1;
            bool ok = py_callcfunc(new_fn, (int)argc + 1, callbase + 1);
            *result = ok ? RES_RETURN : RES_ERROR;
            return true;
        }
        default: return false;
    }
}

static RegOpcode reg_iterator_specialization(py_Type type) {
    switch(type) {
        case tp_generator: return RBC_FOR_GENERATOR;
        case tp_array2d_like_iterator: return RBC_FOR_ARRAY2D;
        case tp_list_iterator: return RBC_FOR_LIST;
        case tp_tuple_iterator: return RBC_FOR_TUPLE;
        case tp_dict_iterator: return RBC_FOR_DICT;
        case tp_range_iterator: return RBC_FOR_RANGE;
        case tp_str_iterator: return RBC_FOR_STR;
        default: return RBC_NOP;
    }
}

static int reg_specialized_next(RegOpcode op, py_Ref iterator) {
    switch(op) {
        case RBC_FOR_GENERATOR:
            if(iterator->type != tp_generator) return -2;
            return generator__iternext(iterator);
        case RBC_FOR_ARRAY2D:
            if(iterator->type != tp_array2d_like_iterator) return -2;
            return array2d_like_iterator__iternext(iterator);
        case RBC_FOR_LIST:
            if(iterator->type != tp_list_iterator) return -2;
            return list_iterator__iternext(iterator);
        case RBC_FOR_TUPLE:
            if(iterator->type != tp_tuple_iterator) return -2;
            return tuple_iterator__iternext(iterator);
        case RBC_FOR_DICT:
            if(iterator->type != tp_dict_iterator) return -2;
            return dict_items__iternext(iterator);
        case RBC_FOR_RANGE:
            if(iterator->type != tp_range_iterator) return -2;
            return range_iterator__iternext(iterator);
        case RBC_FOR_STR:
            if(iterator->type != tp_str_iterator) return -2;
            return str_iterator__iternext(iterator);
        default: return -2;
    }
}

/* TINYPY_REGVM_DUMP=1 prints every function's register bytecode once lowered. */
static PK_REGVM_COLD void reg_dump_code(const FuncDecl* decl, const RegCode* code) {
    static int enabled = -1;
    if(enabled < 0) {
        const char* value = getenv("TINYPY_REGVM_DUMP");
        enabled = value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
    }
    if(!enabled) return;
    fprintf(stderr, "[TinyPy RegVM] %s: nregs=%d nlocals=%d length=%d\n", decl->code.name->data,
            code->nregs, decl->code.nlocals, code->code.length);
    for(int pc = 0; pc < code->code.length; pc++) {
        const RegBytecode word = c11__getitem(RegBytecode, &code->code, pc);
        fprintf(stderr, "    %4d  %-20s A=%-3u B=%-3u C=%-3u sBx=%d\n", pc,
                RegOpcode__name(RBC_OP(word)), RBC_A(word), RBC_B(word), RBC_C(word),
                RBC_SBX(word));
    }
}

RegCode* RegVM__ensure_code(FuncDecl* decl) {
    if(decl == NULL || decl->type == FuncType_GENERATOR) return NULL;
    if(decl->regcode_state == REGCODE_READY) return decl->regcode;
    if(decl->regcode_state == REGCODE_UNSUPPORTED) return NULL;

    RegCode* code = RegCode__new();
    if(code == NULL) {
        decl->regcode_state = REGCODE_UNSUPPORTED;
        return NULL;
    }

    RegLowerError error;
    if(!RegCode__lower_function(code, decl, &error)) {
        RegCode__delete(code);
        decl->regcode = NULL;
        decl->regcode_state = REGCODE_UNSUPPORTED;
        return NULL;
    }

    decl->regcode = code;
    decl->regcode_state = REGCODE_READY;
    reg_dump_code(decl, code);
    return code;
}

int RegVM__frame_slots(FuncDecl* decl) {
#if PK_ENABLE_REGVM
    RegCode* code = RegVM__ensure_code(decl);
    return code ? code->nregs : decl->code.nlocals;
#else
    /* Legacy mode does not lower functions or reserve register-only slots. */
    return decl->code.nlocals;
#endif
}

bool RegVM__can_run_frame(py_Frame* frame, RegCode** out_code) {
#if PK_ENABLE_REGVM
    if(frame == NULL || frame->is_locals_special) return false;
    if(frame->p0 == NULL || frame->p0->type != tp_function) return false;
    Function* fn = py_touserdata(frame->p0);
    if(fn->cfunc != NULL || &fn->decl->code != frame->co) return false;
    RegCode* code = RegVM__ensure_code(fn->decl);
    if(code == NULL) return false;
    if(out_code) *out_code = code;
    return true;
#else
    (void)frame;
    (void)out_code;
    return false;
#endif
}

bool RegVM__can_execute_now(VM* self, py_Frame* frame, RegCode** out_code) {
#if PK_ENABLE_REGVM
    if(self->trace_info.func != NULL) return false;
#if PK_ENABLE_WATCHDOG
    if(self->watchdog_info.max_reset_time > 0) return false;
#endif
    return RegVM__can_run_frame(frame, out_code);
#else
    (void)self;
    (void)frame;
    (void)out_code;
    return false;
#endif
}

static bool reg_apply_jit_exit(VM* self,
                               py_Frame* frame,
                               RegCode* code,
                               const RegJitExit* exit,
                               int32_t* pc,
                               FrameResult* result) {
    switch(exit->kind) {
        case REG_JIT_EXIT_RESUME:
            if(exit->resume_pc < 0 || exit->resume_pc >= code->code.length) {
                py_exception(tp_RuntimeError, "JIT returned invalid resume pc");
                *result = RegVM__slow_error(self, frame, frame->ip);
                return true;
            }
            *pc = exit->resume_pc;
            return false;
        case REG_JIT_EXIT_RETURN:
            VM__pop_frame(self);
            *result = RES_RETURN;
            return true;
        case REG_JIT_EXIT_ERROR:
            *result = RegVM__slow_error(self, frame, frame->ip);
            return true;
        case REG_JIT_EXIT_YIELD:
            *result = RES_YIELD;
            return true;
        default: c11__unreachable();
    }
}

static bool reg_handle_backedge(VM* self,
                                py_Frame* frame,
                                FuncDecl* decl,
                                RegCode* code,
                                int32_t from_pc,
                                int32_t target_pc,
                                int32_t* pc,
                                FrameResult* result) {
    if(self->regjit_hooks == NULL || target_pc >= from_pc) return false;

    /* Fast path: a loop whose native entry was disabled costs one load. */
    const RegRuntimeSite* branch = c11__at(RegRuntimeSite, &code->runtime_sites, from_pc);
    if(branch->loop_index != REG_PROFILE_NONE &&
       branch->loop_index < (uint32_t)code->loop_profiles.length &&
       c11__at(RegLoopProfile, &code->loop_profiles, branch->loop_index)->state == REG_JIT_DISABLED)
        return false;

    int32_t source_ip = target_pc < code->source_ip.length
                            ? c11__getitem(int32_t, &code->source_ip, target_pc)
                            : -1;
    RegLoopProfile* loop = RegProfile__loop_backedge(
        code, from_pc, target_pc, source_ip);
    if(loop == NULL) return false;

    if(loop->state == REG_JIT_COLD && loop->hotness >= PK_JIT_LOOP_THRESHOLD) {
        (void)RegJit__request_loop(self, decl, code, loop);
    }

    if(loop->state == REG_JIT_COMPILED) {
        RegJitExit exit;
        frame->ip = source_ip;
        if(RegJit__enter_loop(self, frame, code, target_pc, &exit)) {
            return reg_apply_jit_exit(self, frame, code, &exit, pc, result);
        }
    }
    return false;
}

static bool reg_binary(VM* self,
                       py_Frame* frame,
                       int32_t source_ip,
                       RegOpcode op,
                       py_Ref out,
                       py_Ref lhs,
                       py_Ref rhs) {
    if(PK_CEVAL_LIKELY(lhs->type == tp_int && rhs->type == tp_int)) {
        py_i64 a = lhs->_i64;
        py_i64 b = rhs->_i64;
        switch(op) {
            case RBC_ADD: py_newint(out, a + b); return true;
            case RBC_SUB: py_newint(out, a - b); return true;
            case RBC_MUL: py_newint(out, a * b); return true;
            case RBC_FLOORDIV:
                if(b != 0) {
                    py_newint(out, cpy312__int_floordiv(a, b));
                    return true;
                }
                break;
            case RBC_MOD:
                if(b != 0) {
                    py_newint(out, cpy312__int_mod(a, b));
                    return true;
                }
                break;
            case RBC_LSHIFT: py_newint(out, a << b); return true;
            case RBC_RSHIFT: py_newint(out, a >> b); return true;
            case RBC_BITAND: py_newint(out, a & b); return true;
            case RBC_BITOR: py_newint(out, a | b); return true;
            case RBC_BITXOR: py_newint(out, a ^ b); return true;
            case RBC_CMP_LT: py_newbool(out, a < b); return true;
            case RBC_CMP_LE: py_newbool(out, a <= b); return true;
            case RBC_CMP_EQ: py_newbool(out, a == b); return true;
            case RBC_CMP_NE: py_newbool(out, a != b); return true;
            case RBC_CMP_GT: py_newbool(out, a > b); return true;
            case RBC_CMP_GE: py_newbool(out, a >= b); return true;
            default: break;
        }
    }

    if((op == RBC_ADD || op == RBC_SUB || op == RBC_MUL ||
        (op >= RBC_CMP_LT && op <= RBC_CMP_GE)) &&
       reg_isnum(lhs) && reg_isnum(rhs)) {
        py_f64 a = reg_tof64(lhs);
        py_f64 b = reg_tof64(rhs);
        switch(op) {
            case RBC_ADD: py_newfloat(out, a + b); return true;
            case RBC_SUB: py_newfloat(out, a - b); return true;
            case RBC_MUL: py_newfloat(out, a * b); return true;
            case RBC_CMP_LT: py_newbool(out, a < b); return true;
            case RBC_CMP_LE: py_newbool(out, a <= b); return true;
            case RBC_CMP_EQ: py_newbool(out, a == b); return true;
            case RBC_CMP_NE: py_newbool(out, a != b); return true;
            case RBC_CMP_GT: py_newbool(out, a > b); return true;
            case RBC_CMP_GE: py_newbool(out, a >= b); return true;
            default: c11__unreachable();
        }
    }

    (void)self;
    frame->ip = source_ip;
    return RegVM__slow_binary(op, out, lhs, rhs);
}

static FrameResult reg_run(VM* self, RegCode* rc, int32_t start_pc);

FrameResult RegVM__resume_frame(VM* self, RegCode* code, int32_t pc) {
    return reg_run(self, code, pc);
}

/*
 * Direct native entry for a freshly pushed frame whose body is JIT-compiled:
 * skips the interpreter's setup entirely when native code runs to completion.
 * Returns false (nothing done) when the interpreter must handle the entry.
 */
static PK_REGVM_NOINLINE bool reg_enter_native(VM* self, py_Frame* frame, RegCode* rc,
                                               FrameResult* result) {
    py_TValue* frame_limit = frame->locals + rc->nregs;
    if(self->recursion_depth >= self->max_recursion_depth || frame_limit > self->stack.end)
        return false; /* reg_run() raises the precise error */
    self->stack.sp = frame_limit;
    frame->ip = c11__getitem(int32_t, &rc->source_ip, 0);
    RegJitExit exit;
    if(!RegJit__enter_function(self, frame, rc, &exit)) return false;
    switch(exit.kind) {
        case REG_JIT_EXIT_RETURN:
            VM__pop_frame(self);
            *result = RES_RETURN;
            return true;
        case REG_JIT_EXIT_ERROR:
            *result = RegVM__slow_error(self, frame, frame->ip);
            return true;
        case REG_JIT_EXIT_YIELD:
            *result = RES_YIELD;
            return true;
        case REG_JIT_EXIT_RESUME:
            if(exit.resume_pc < 0 || exit.resume_pc >= rc->code.length) {
                py_exception(tp_RuntimeError, "JIT returned invalid resume pc");
                *result = RegVM__slow_error(self, frame, frame->ip);
                return true;
            }
            *result = reg_run(self, rc, exit.resume_pc);
            return true;
        default: c11__unreachable();
    }
}

/*
 * A PY_SIMPLE call cache has just pushed a plain function frame, which already
 * satisfies RegVM__can_run_frame() once its body is lowered: skip re-validating,
 * and enter compiled bodies natively without the interpreter's setup.
 */
static PK_INLINE FrameResult reg_run_callee(VM* self) {
    py_Frame* frame = self->top_frame;
    const FuncDecl* decl = ((Function*)py_touserdata(frame->p0))->decl;
    if(PK_REGVM_LIKELY(decl->regcode_state == REGCODE_READY && self->trace_info.func == NULL
#if PK_ENABLE_WATCHDOG
                       && self->watchdog_info.max_reset_time <= 0
#endif
                       )) {
        RegCode* rc = decl->regcode;
        FrameResult result;
        if(rc->function_jit_state == (uint8_t)REG_JIT_COMPILED &&
           reg_enter_native(self, frame, rc, &result))
            return result;
        return reg_run(self, rc, -1);
    }
    return VM__run_top_frame(self);
}

/*
 * CALL / CALL_IC A, B (argc), C (kwargc): R[A] = R[A](...). Shared by the
 * interpreter handler (inlined) and native code (reg_jit_call), so both
 * engines run one implementation. The caller has synced frame->ip. Returns
 * false with an exception set.
 */
static PK_INLINE bool reg_exec_call(VM* self, RegCode* rc, py_TValue* R, py_TValue* frame_limit,
                                    int32_t here, RegOpcode op, unsigned A, unsigned B, unsigned C,
                                    bool profile_enabled) {
    /* Match the legacy CALL safepoint; the register window is rooted. */
    if(PK_REGVM_UNLIKELY(self->heap.gc_enabled &&
                        self->heap.gc_counter >= self->heap.gc_threshold)) {
        ManagedHeap__collect_hint(&self->heap);
    }
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    uintptr_t call_identity = reg_call_identity(&R[A]);
    if(profile_enabled) RegProfile__record_call(rc, here, call_identity);

    if(op == RBC_CALL_IC) {
        FrameResult cached_result;
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(reg_call_cache_apply(self, frame_limit, &R[A], (uint8_t)B, (uint8_t)C, cache,
                                &cached_result)) {
            if(profile_enabled) RegProfile__record_ic_hit(rc, here);
            if(cached_result == RES_CALL) cached_result = reg_run_callee(self);
            self->stack.sp = frame_limit;
            if(cached_result == RES_ERROR) return false;
            c11__rtassert(cached_result == RES_RETURN);
            R[A] = self->last_retval;
            site->misses = 0;
            return true;
        }

        if(profile_enabled) RegProfile__record_ic_miss(rc, here);
        bool dequickened = reg_specialization_miss(rc, here, RBC_CALL);
        if(!dequickened && cache != NULL) {
            (void)reg_call_cache_fill(&R[A], (uint8_t)B, (uint8_t)C, cache);
        }
    } else if(site->misses != UINT8_MAX) {
        /* misses == UINT8_MAX: a fill failed once, the call shape is not cacheable. */
        if(reg_observe(site, (uint32_t)(call_identity ^ (call_identity >> 32)))) {
            RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_CALL);
            if(cache != NULL && reg_call_cache_fill(&R[A], (uint8_t)B, (uint8_t)C, cache)) {
                reg_mark_quickened(rc, here, RBC_CALL_IC);
            } else {
                site->misses = UINT8_MAX;
            }
        }
    }

    uint16_t window = (uint16_t)(2u + B + C * 2u);
    if(frame_limit + window > self->stack.end) {
        py_exception(tp_RecursionError, "value stack overflow");
        return false;
    }
    self->stack.sp = frame_limit;
    memcpy(self->stack.sp, &R[A], (size_t)window * sizeof(py_TValue));
    self->stack.sp += window;

    FrameResult res = VM__vectorcall(self, (uint16_t)B, (uint16_t)C, true);
    if(res == RES_CALL) res = VM__run_top_frame(self);
    self->stack.sp = frame_limit;
    if(res == RES_ERROR) return false;
    c11__rtassert(res == RES_RETURN);
    R[A] = self->last_retval;
    return true;
}

/*
 * LOAD_METHOD / LOAD_METHOD_IC A, Bx: R[A], R[A + 1] = callable, self (or nil).
 * Shared like reg_exec_call(); the caller has synced frame->ip.
 */
static PK_INLINE bool reg_exec_load_method(VM* self, RegCode* rc, py_TValue* R, int32_t here,
                                           RegOpcode op, unsigned A, py_Name name,
                                           bool profile_enabled) {
    py_TValue object = R[A];
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    bool dequickened = false;
    if(profile_enabled)
        RegProfile__record_shape(rc, here, object.type, reg_shape_identity(&object));

    if(op == RBC_LOAD_METHOD_IC) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(reg_method_cache_apply(self, &object, name, cache, &R[A], &R[A + 1])) {
            site->misses = 0;
            if(profile_enabled) RegProfile__record_ic_hit(rc, here);
            return true;
        }
        if(profile_enabled) RegProfile__record_ic_miss(rc, here);
        dequickened = reg_specialization_miss(rc, here, RBC_LOAD_METHOD);
        if(!dequickened && cache != NULL) {
            (void)reg_method_cache_fill(self, &object, name, cache);
        }
    }

    R[A] = object;
    if(!pk_loadmethod(&R[A], name)) {
        R[A] = object;
        if(!py_getattr(&R[A], name)) return false;
        R[A] = self->last_retval;
        py_newnil(&R[A + 1]);
    }

    if(op == RBC_LOAD_METHOD) {
        if(reg_observe(site, (uint32_t)(uint16_t)object.type)) {
            RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_METHOD);
            if(cache != NULL && reg_method_cache_fill(self, &object, name, cache)) {
                reg_mark_quickened(rc, here, RBC_LOAD_METHOD_IC);
            }
        }
    } else if(!dequickened) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(cache != NULL) (void)reg_method_cache_fill(self, &object, name, cache);
    }
    return true;
}

/* LOAD_GLOBAL / LOAD_GLOBAL_IC A, Bx. Shared like reg_exec_call(). */
static PK_INLINE bool reg_exec_load_global(VM* self, py_Frame* frame, RegCode* rc, py_TValue* R,
                                           int32_t here, RegOpcode op, unsigned A, py_Name name,
                                           bool profile_enabled) {
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    bool dequickened = false;

    if(op == RBC_LOAD_GLOBAL_IC) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(reg_global_cache_apply(self, frame, name, cache, &R[A])) {
            site->misses = 0;
            if(profile_enabled) RegProfile__record_ic_hit(rc, here);
            return true;
        }
        if(profile_enabled) RegProfile__record_ic_miss(rc, here);
        dequickened = reg_specialization_miss(rc, here, RBC_LOAD_GLOBAL);
        if(!dequickened && cache != NULL) {
            (void)reg_global_cache_fill(self, frame, name, cache);
        }
    }

    int res = Frame__getglobal(frame, name);
    if(res == 1) {
        R[A] = self->last_retval;
    } else if(res == -1) {
        return false;
    } else {
        py_Ref tmp = py_getdict(self->builtins, name);
        if(tmp == NULL) {
            NameError(name);
            return false;
        }
        R[A] = *tmp;
    }

    if(op == RBC_LOAD_GLOBAL) {
        uintptr_t p = (uintptr_t)frame->globals;
        if(reg_observe(site, (uint32_t)(p ^ (p >> 32)))) {
            RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_GLOBAL);
            if(cache != NULL && reg_global_cache_fill(self, frame, name, cache)) {
                reg_mark_quickened(rc, here, RBC_LOAD_GLOBAL_IC);
            }
        }
    } else if(!dequickened) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(cache != NULL) (void)reg_global_cache_fill(self, frame, name, cache);
    }
    return true;
}

/*
 * STORE_GLOBAL A, Bx: globals[name] = R[A]. Rebinding an existing module global
 * writes the slot recorded in the site's global cache: the slot cannot move
 * while the namespace version is unchanged (only insertions, deletions and
 * rehashes bump it). Anything else takes the generic path and refills the cache.
 */
static PK_INLINE bool reg_exec_store_global(VM* self, py_Frame* frame, RegCode* rc, py_TValue* R,
                                            int32_t here, unsigned A, py_Name name) {
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    RegInlineCache* cache = reg_cache_at(rc, site);
    if(cache != NULL && cache->kind == REG_IC_KIND_GLOBAL &&
       cache->variant == REG_GLOBAL_CACHE_GLOBAL && frame->globals->type == tp_module) {
        NameDict* globals = reg_namedict(frame->globals);
        if(globals != NULL && (uintptr_t)globals == cache->guard0 &&
           globals->version == cache->version0 && cache->index0 < (uint32_t)globals->capacity &&
           globals->items[cache->index0].key == name) {
            globals->items[cache->index0].value = R[A];
            return true;
        }
    }
    if(!Frame__setglobal(frame, name, &R[A])) return false;
    if(frame->globals->type == tp_module) {
        cache = reg_cache_alloc(rc, site, REG_IC_KIND_GLOBAL);
        if(cache != NULL) (void)reg_global_cache_fill(self, frame, name, cache);
    }
    return true;
}

/*
 * LOAD_NONLOCAL / LOAD_NONLOCAL_IC A, Bx. Without a closure, a nonlocal lookup
 * is exactly the global IC's lookup.
 */
static PK_INLINE bool reg_exec_load_nonlocal(VM* self, py_Frame* frame, RegCode* rc, py_TValue* R,
                                             int32_t here, RegOpcode op, unsigned A, py_Name name,
                                             bool no_closure, bool profile_enabled) {
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    if(op == RBC_LOAD_NONLOCAL_IC) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(no_closure && reg_global_cache_apply(self, frame, name, cache, &R[A])) {
            site->misses = 0;
            if(profile_enabled) RegProfile__record_ic_hit(rc, here);
            return true;
        }
        if(profile_enabled) RegProfile__record_ic_miss(rc, here);
        if(!reg_specialization_miss(rc, here, RBC_LOAD_NONLOCAL) && cache != NULL && no_closure) {
            (void)reg_global_cache_fill(self, frame, name, cache);
        }
    }
    py_Ref tmp = Frame__getclosure(frame, name);
    if(tmp != NULL) {
        R[A] = *tmp;
        return true;
    }
    if(op == RBC_LOAD_NONLOCAL && no_closure && reg_observe(site, (uint32_t)(uintptr_t)name)) {
        RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_GLOBAL);
        if(cache != NULL && reg_global_cache_fill(self, frame, name, cache)) {
            reg_mark_quickened(rc, here, RBC_LOAD_NONLOCAL_IC);
        }
    }
    int res = Frame__getglobal(frame, name);
    if(res == 1) {
        R[A] = self->last_retval;
        return true;
    }
    if(res == -1) return false;
    tmp = py_getdict(self->builtins, name);
    if(tmp != NULL) {
        R[A] = *tmp;
        return true;
    }
    NameError(name);
    return false;
}

/* LOAD_ATTR / LOAD_ATTR_IC A, Bx (in place). The caller has synced frame->ip. */
static PK_INLINE bool reg_exec_load_attr(VM* self, RegCode* rc, py_TValue* R, int32_t here,
                                         RegOpcode op, unsigned A, py_Name name,
                                         bool profile_enabled) {
    py_TValue object = R[A];
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    bool dequickened = false;
    if(profile_enabled)
        RegProfile__record_shape(rc, here, object.type, reg_shape_identity(&object));

    if(op == RBC_LOAD_ATTR_IC) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        int cached = reg_attr_cache_apply(self, &object, name, cache, &R[A]);
        if(cached > 0) {
            site->misses = 0;
            if(profile_enabled) RegProfile__record_ic_hit(rc, here);
            return true;
        }
        if(cached < 0) return false;
        if(profile_enabled) RegProfile__record_ic_miss(rc, here);
        dequickened = reg_specialization_miss(rc, here, RBC_LOAD_ATTR);
        if(!dequickened && cache != NULL) {
            (void)reg_attr_cache_fill(self, &object, name, cache);
        }
    }

    if(!py_getattr(&object, name)) return false;
    R[A] = self->last_retval;

    if(op == RBC_LOAD_ATTR) {
        if(reg_observe(site, (uint32_t)(uint16_t)object.type)) {
            RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_ATTR);
            if(cache != NULL && reg_attr_cache_fill(self, &object, name, cache)) {
                reg_mark_quickened(rc, here, RBC_LOAD_ATTR_IC);
            }
        }
    } else if(!dequickened) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(cache != NULL) (void)reg_attr_cache_fill(self, &object, name, cache);
    }
    return true;
}

/* STORE_ATTR / STORE_ATTR_IC A, Bx: R[A + 1].name = R[A]. Syncs frame->ip itself. */
static PK_INLINE bool reg_exec_store_attr(VM* self, py_Frame* frame, RegCode* rc, py_TValue* R,
                                          int32_t here, int32_t source_ip, RegOpcode op,
                                          unsigned A, py_Name name) {
    py_Ref object = &R[A + 1];
    RegRuntimeSite* site = reg_runtime_site(rc, here);
    if(op == RBC_STORE_ATTR_IC) {
        RegInlineCache* cache = reg_cache_at(rc, site);
        if(reg_store_attr_cache_apply(self, object, &R[A], cache)) {
            site->misses = 0;
            return true;
        }
        (void)reg_specialization_miss(rc, here, RBC_STORE_ATTR);
    }
    frame->ip = source_ip;
    Shape* before = reg_shape_identity(object) != 0
                        ? ShapeStorage__shape(ShapeObject__storage(object->_obj))
                        : NULL;
    if(!py_setattr(object, name, &R[A])) return false;
    if(before != NULL && reg_observe(site, (uint32_t)(uintptr_t)before)) {
        RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_STORE_ATTR);
        if(cache != NULL && reg_store_attr_cache_fill(self, object, name, before, cache)) {
            reg_mark_quickened(rc, here, RBC_STORE_ATTR_IC);
        }
    }
    return true;
}

/*
 * Native-code helpers: run the instruction at `pc` of `frame` (the top frame,
 * registers in place) exactly as the interpreter would. Native code calls them
 * only after compilation, so per-op profiling is already off. One small helper
 * per opcode family (the opcode itself may flip between its base and _IC form
 * at run time, which the shared handler reads).
 */
#define REG_JIT_HELPER_PROLOGUE()                                                                  \
    const RegBytecode inst = c11__getitem(RegBytecode, &rc->code, pc);                            \
    const RegOpcode op = RBC_OP(inst);                                                             \
    const int32_t ip = c11__getitem(int32_t, &rc->source_ip, pc);                                 \
    py_TValue* R = frame->locals;                                                                  \
    (void)ip

static PK_REGVM_NOINLINE bool reg_jit_call(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    frame->ip = ip;
    return reg_exec_call(self, rc, R, R + rc->nregs, pc, op, RBC_A(inst), RBC_B(inst),
                         RBC_C(inst), false);
}

static PK_REGVM_NOINLINE bool reg_jit_load_method(VM* self, py_Frame* frame, RegCode* rc,
                                                  int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    frame->ip = ip;
    return reg_exec_load_method(self, rc, R, pc, op, RBC_A(inst),
                                c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)), false);
}

static PK_REGVM_NOINLINE bool reg_jit_load_global(VM* self, py_Frame* frame, RegCode* rc,
                                                  int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    frame->ip = ip;
    return reg_exec_load_global(self, frame, rc, R, pc, op, RBC_A(inst),
                                c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)), false);
}

static PK_REGVM_NOINLINE bool reg_jit_store_global(VM* self, py_Frame* frame, RegCode* rc,
                                                   int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)op;
    frame->ip = ip;
    return reg_exec_store_global(self, frame, rc, R, pc, RBC_A(inst),
                                 c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)));
}

static PK_REGVM_NOINLINE bool reg_jit_load_nonlocal(VM* self, py_Frame* frame, RegCode* rc,
                                                    int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    frame->ip = ip;
    const Function* fn = py_touserdata(frame->p0);
    return reg_exec_load_nonlocal(self, frame, rc, R, pc, op, RBC_A(inst),
                                  c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)),
                                  fn->closure == NULL, false);
}

static PK_REGVM_NOINLINE bool reg_jit_load_attr(VM* self, py_Frame* frame, RegCode* rc,
                                                int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    frame->ip = ip;
    return reg_exec_load_attr(self, rc, R, pc, op, RBC_A(inst),
                              c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)), false);
}

static PK_REGVM_NOINLINE bool reg_jit_store_attr(VM* self, py_Frame* frame, RegCode* rc,
                                                 int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    return reg_exec_store_attr(self, frame, rc, R, pc, ip, op, RBC_A(inst),
                               c11__getitem(py_Name, &frame->co->names, RBC_BX(inst)));
}

static PK_REGVM_NOINLINE bool reg_jit_load_subscr(VM* self, py_Frame* frame, RegCode* rc,
                                                  int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)op;
    frame->ip = ip;
    if(!py_getitem(&R[RBC_B(inst)], &R[RBC_C(inst)])) return false;
    R[RBC_A(inst)] = self->last_retval;
    return true;
}

static PK_REGVM_NOINLINE bool reg_jit_unpack_sequence(VM* self, py_Frame* frame, RegCode* rc,
                                                      int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)op;
    (void)self;
    frame->ip = ip;
    return RegVM__slow_unpack_sequence(&R[RBC_B(inst)], &R[RBC_A(inst)], RBC_C(inst));
}

static PK_REGVM_NOINLINE bool reg_jit_store_subscr(VM* self, py_Frame* frame, RegCode* rc,
                                                   int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)self;
    (void)op;
    frame->ip = ip;
    return py_setitem(&R[RBC_B(inst)], &R[RBC_C(inst)], &R[RBC_A(inst)]);
}

/* BUILD_TUPLE / BUILD_LIST A, B, C: the same steps as their interpreter handlers. */
static PK_REGVM_NOINLINE bool reg_jit_build_sequence(VM* self, py_Frame* frame, RegCode* rc,
                                                     int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)self;
    frame->ip = ip;
    const unsigned A = RBC_A(inst), B = RBC_B(inst), count = RBC_C(inst);
    if(op == RBC_BUILD_TUPLE) {
        bool need_track = false;
        py_Ref p = py_newtuple(&R[A], (int)count);
        for(unsigned i = 0; i < count; i++) {
            p[i] = R[B + i];
            if(p[i].is_ptr) need_track = true;
        }
        if(!need_track) R[A]._obj->gc_marked |= 0b10;
        return true;
    }
    py_newlistn(&R[A], (int)count);
    for(unsigned i = 0; i < count; i++) py_list_setitem(&R[A], (int)i, &R[B + i]);
    return true;
}

static RegOpcode reg_i8_base(RegOpcode op) {
    switch(op) {
        case RBC_ADD_I8: return RBC_ADD;
        case RBC_SUB_I8: return RBC_SUB;
        case RBC_MUL_I8: return RBC_MUL;
        case RBC_CMP_LT_I8: return RBC_CMP_LT;
        case RBC_CMP_LE_I8: return RBC_CMP_LE;
        case RBC_CMP_EQ_I8: return RBC_CMP_EQ;
        case RBC_CMP_NE_I8: return RBC_CMP_NE;
        case RBC_CMP_GT_I8: return RBC_CMP_GT;
        case RBC_CMP_GE_I8: return RBC_CMP_GE;
        default: return RBC_NOP;
    }
}

/*
 * Arithmetic and comparisons whose operands are not the ints native code
 * specializes for (objects with dunders, strings, floats, ...). Same steps as
 * the interpreter's generic handler, without quickening (the code is compiled).
 */
static PK_REGVM_NOINLINE bool reg_jit_binary(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    const unsigned A = RBC_A(inst), B = RBC_B(inst);
    RegOpcode base = reg_i8_base(op);
    py_TValue imm;
    py_Ref rhs;
    if(base != RBC_NOP) {
        py_newint(&imm, (py_i64)RBC_I8(inst));
        rhs = &imm;
    } else {
        base = (op >= RBC_ADD && op <= RBC_CMP_GE) ? op : reg_specialized_base(op);
        rhs = &R[RBC_C(inst)];
    }
    return reg_binary(self, frame, ip, base, &R[A], &R[B], rhs);
}

static PK_REGVM_NOINLINE bool reg_jit_simple(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    const unsigned A = RBC_A(inst), B = RBC_B(inst), C = RBC_C(inst);
    frame->ip = ip;
    switch(op) {
        case RBC_IS:
        case RBC_IS_NOT: {
            bool value = py_isidentical(&R[B], &R[C]);
            py_newbool(&R[A], op == RBC_IS_NOT ? !value : value);
            return true;
        }
        case RBC_CONTAINS:
        case RBC_NOT_CONTAINS:
            return RegVM__slow_contains(&R[A], &R[B], &R[C], op == RBC_NOT_CONTAINS);
        case RBC_GET_ITER:
            if(!py_iter(&R[B])) return false;
            R[A] = self->last_retval;
            return true;
        case RBC_BUILD_DICT:
            py_newdict(&R[A]);
            for(unsigned i = 0; i < C; i++) {
                if(!py_dict_setitem(&R[A], &R[B + i * 2], &R[B + i * 2 + 1])) return false;
            }
            return true;
        case RBC_DICT_ADD: return py_dict_setitem(&R[A], &R[B], &R[C]);
        case RBC_LIST_APPEND: py_list_append(&R[A], &R[B]); return true;
        case RBC_NEG:
            if(!pk_callmagic(__neg__, 1, &R[B])) return false;
            R[A] = self->last_retval;
            return true;
        case RBC_BUILD_STRING: return RegVM__slow_build_string(self, &R[A], &R[B], C);
        case RBC_FORMAT_STRING: {
            py_Ref spec = c11__at(py_TValue, &frame->co->consts, RBC_BX(inst));
            if(!pk_format_object(self, &R[A], py_tosv(spec))) return false;
            R[A] = self->last_retval;
            return true;
        }
        case RBC_DELETE_SUBSCR: return py_delitem(&R[A], &R[B]);
        default: c11__unreachable();
    }
}

int RegVM__jit_iter_next(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    const unsigned A = RBC_A(inst);
    frame->ip = ip;
    int res = op == RBC_FOR_ITER ? -2 : reg_specialized_next(op, &R[A]);
    if(res == -2) res = pk_ceval_next(&R[A]);
    if(res < 0) return 0;
    if(res == 0) return 2;
    R[A + 1] = self->last_retval;
    return 1;
}

/*
 * FOR_* A at `pc` immediately followed by UNPACK_SEQUENCE dst, A + 1, 2 (the
 * item register is a temporary consumed by the unpack): items() of a dict
 * yields key and value straight into R[dst], R[dst + 1] without allocating the
 * pair. Other iterators take the ordinary steps. Same results as
 * RegVM__jit_iter_next().
 */
int RegVM__jit_iter_next_unpack2(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    const unsigned A = RBC_A(inst);
    const unsigned dst = RBC_A(c11__getitem(RegBytecode, &rc->code, pc + 1));
    frame->ip = ip;
    if(R[A].type == tp_dict_iterator) {
        int res = dict_items__iternext2(&R[A], &R[dst]);
        if(res != -2) return res < 0 ? 0 : res == 0 ? 2 : 1;
    }
    int res = op == RBC_FOR_ITER ? -2 : reg_specialized_next(op, &R[A]);
    if(res == -2) res = pk_ceval_next(&R[A]);
    if(res < 0) return 0;
    if(res == 0) return 2;
    R[A + 1] = self->last_retval;
    frame->ip = c11__getitem(int32_t, &rc->source_ip, pc + 1);
    return RegVM__slow_unpack_sequence(&R[A + 1], &R[dst], 2) ? 1 : 0;
}

int RegVM__jit_compare_truth(VM* self, py_Frame* frame, RegCode* rc, int32_t pc) {
    REG_JIT_HELPER_PROLOGUE();
    (void)self;
    const RegOpcode cmp = reg_compare_code_opcode(RBC_C(inst));
    py_TValue imm;
    py_Ref lhs = &R[RBC_A(inst)];
    py_Ref rhs;
    if(op == RBC_CMP_I8_BRANCH) {
        py_newint(&imm, (py_i64)(int8_t)RBC_B(inst));
        rhs = &imm;
    } else {
        rhs = &R[RBC_B(inst)];
    }
    if(lhs->type == tp_int && rhs->type == tp_int) return reg_compare_i64(cmp, lhs->_i64, rhs->_i64);
    if(reg_isnum(lhs) && reg_isnum(rhs)) return reg_compare_f64(cmp, reg_tof64(lhs), reg_tof64(rhs));
    frame->ip = ip;
    py_TValue result;
    if(!RegVM__slow_binary(cmp, &result, lhs, rhs)) return -1;
    const int truth = pk_ceval_bool(&result);
    return truth < 0 ? -1 : truth != 0;
}

#undef REG_JIT_HELPER_PROLOGUE

RegJitStepFn RegVM__jit_step_fn(RegOpcode op) {
    if((op >= RBC_ADD && op <= RBC_CMP_GE) || (op >= RBC_ADD_INT && op <= RBC_CMP_GE_NUM) ||
       reg_i8_base(op) != RBC_NOP)
        return reg_jit_binary;
    switch(op) {
        case RBC_CALL: case RBC_CALL_IC: return reg_jit_call;
        case RBC_LOAD_METHOD: case RBC_LOAD_METHOD_IC: return reg_jit_load_method;
        case RBC_LOAD_GLOBAL: case RBC_LOAD_GLOBAL_IC: return reg_jit_load_global;
        case RBC_STORE_GLOBAL: return reg_jit_store_global;
        case RBC_LOAD_NONLOCAL: case RBC_LOAD_NONLOCAL_IC: return reg_jit_load_nonlocal;
        case RBC_LOAD_ATTR: case RBC_LOAD_ATTR_IC: return reg_jit_load_attr;
        case RBC_STORE_ATTR: case RBC_STORE_ATTR_IC: return reg_jit_store_attr;
        case RBC_LOAD_SUBSCR: return reg_jit_load_subscr;
        case RBC_STORE_SUBSCR: return reg_jit_store_subscr;
        case RBC_UNPACK_SEQUENCE: return reg_jit_unpack_sequence;
        case RBC_BUILD_TUPLE: case RBC_BUILD_LIST: return reg_jit_build_sequence;
        case RBC_IS: case RBC_IS_NOT: case RBC_CONTAINS: case RBC_NOT_CONTAINS:
        case RBC_GET_ITER: case RBC_BUILD_DICT: case RBC_DICT_ADD: case RBC_LIST_APPEND:
        case RBC_NEG: case RBC_BUILD_STRING: case RBC_FORMAT_STRING: case RBC_DELETE_SUBSCR:
            return reg_jit_simple;
        default: return NULL;
    }
}

FrameResult VM__run_top_frame_reg(VM* self) {
    RegCode* rc;
    if(!RegVM__can_run_frame(self->top_frame, &rc)) return VM__run_top_frame_stack(self);
    return reg_run(self, rc, -1);
}

/*
 * The top frame satisfies RegVM__can_run_frame() with body `rc`. A fresh call
 * passes start_pc = -1 (function-entry profiling and JIT entry); start_pc >= 0
 * continues a frame whose native code returned to the interpreter at that pc.
 */
static PK_REGVM_HOT PK_REGVM_ALIGN64 FrameResult reg_run(VM* self, RegCode* rc, int32_t start_pc) {
    py_Frame* frame = self->top_frame;
    if(PK_REGVM_UNLIKELY(self->recursion_depth >= self->max_recursion_depth)) {
        py_exception(tp_RecursionError, "maximum recursion depth exceeded");
        return RegVM__slow_error(self, frame, frame->ip);
    }

    Function* current_fn = py_touserdata(frame->p0);
    FuncDecl* decl = current_fn->decl;
    py_TValue* R = frame->locals;
    py_TValue* frame_limit = R + rc->nregs;
    if(PK_REGVM_UNLIKELY(frame_limit > self->stack.end)) {
        py_exception(tp_RecursionError, "value stack overflow");
        return RegVM__slow_error(self, frame, frame->ip);
    }
    self->stack.sp = frame_limit;

    RegBytecode* code = (RegBytecode*)rc->code.data;
    RegRuntimeSite* runtime_sites = (RegRuntimeSite*)rc->runtime_sites.data;
    const int32_t* source_ip = (const int32_t*)rc->source_ip.data;
    const py_TValue* consts = (const py_TValue*)frame->co->consts.data;
    const py_Name* names = (const py_Name*)frame->co->names.data;
    const py_Name* varnames = (const py_Name*)frame->co->varnames.data;
    FuncDecl_* func_decls = (FuncDecl_*)frame->co->func_decls.data;
    /*
     * Entries and backedges are routed to the JIT whenever it is attached, but
     * per-op feedback is collected only while this code is still cold: once it
     * has been compiled (or rejected) the profile has served its purpose.
     */
    const bool jit_enabled = self->regjit_hooks != NULL;
    bool profile_enabled = jit_enabled && rc->function_jit_state == (uint8_t)REG_JIT_COLD;
    const int32_t code_length = rc->code.length;
    int32_t pc = start_pc < 0 ? 0 : start_pc;

    if(jit_enabled && start_pc < 0) {
        if(profile_enabled && RegProfile__function_entry(rc))
            (void)RegJit__request_function(self, decl, rc);
        if(rc->function_jit_state == (uint8_t)REG_JIT_COMPILED) {
            RegJitExit exit;
            if(source_ip != NULL && rc->source_ip.length > 0) frame->ip = source_ip[0];
            if(RegJit__enter_function(self, frame, rc, &exit)) {
                FrameResult jit_result;
                if(reg_apply_jit_exit(self, frame, rc, &exit, &pc, &jit_result)) return jit_result;
            }
        }
        profile_enabled = rc->function_jit_state == (uint8_t)REG_JIT_COLD;
    }

#define REG_SYNC_IP() do { frame->ip = source_ip[here]; } while(0)
#define REG_FAIL() do { return RegVM__slow_error(self, frame, source_ip[here]); } while(0)
#define REG_BRANCH_TO(target_expr)                                                                 \
    do {                                                                                            \
        int32_t _target_pc = (target_expr);                                                         \
        pc = _target_pc;                                                                            \
        if(PK_REGVM_UNLIKELY(_target_pc < here)) {                                                   \
            if(PK_REGVM_UNLIKELY(self->heap.gc_enabled &&                                           \
                                self->heap.gc_counter >= self->heap.gc_threshold)) {               \
                frame->ip = source_ip[here];                                                        \
                ManagedHeap__collect_hint(&self->heap);                                            \
            }                                                                                       \
            if(jit_enabled) {                                                                       \
                FrameResult _jit_result;                                                            \
                if(reg_handle_backedge(self, frame, decl, rc, here, _target_pc, &pc, &_jit_result))\
                    return _jit_result;                                                             \
                profile_enabled = rc->function_jit_state == (uint8_t)REG_JIT_COLD;                 \
            }                                                                                       \
        }                                                                                           \
    } while(0)
#if PK_REGVM_DISPATCH_THREADED
#define REG_CASE(opcode) case opcode: reg_op_##opcode:
#define REG_DISPATCH_NEXT()                                                                        \
    do {                                                                                           \
        here = pc;                                                                                 \
        inst = code[pc++];                                                                         \
        op = RBC_OP(inst);                                                                         \
        A = RBC_A(inst);                                                                           \
        B = RBC_B(inst);                                                                           \
        C = RBC_C(inst);                                                                           \
        goto* reg_dispatch[op];                                                                    \
    } while(0)
#else
#define REG_CASE(opcode) case opcode:
/* Must stay a bare `break`: it leaves the switch, not a do/while wrapper. */
#define REG_DISPATCH_NEXT() break
#endif
#define REG_SYNC_IF_DYNAMIC_TRUTH(v)                                                                \
    do {                                                                                            \
        py_Type _t = (v)->type;                                                                     \
        if(_t != tp_bool && _t != tp_int && _t != tp_float && _t != tp_NoneType) REG_SYNC_IP();    \
    } while(0)

#if PK_REGVM_DISPATCH_THREADED
    /*
     * Direct threading: every handler that ends in REG_DISPATCH_NEXT() decodes
     * the next instruction and jumps straight to its handler, giving each one
     * its own indirect branch (better prediction than one shared switch jump).
     * Opcodes without an entry go through the switch, so a missing entry is
     * only slower, never wrong. Generated from the case labels below.
     */
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma clang diagnostic ignored "-Winitializer-overrides"
#else
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
    static const void* const reg_dispatch[RBC__COUNT] = {
        [0 ... RBC__COUNT - 1] = &&reg_dispatch_switch,
        [RBC_NOP] = &&reg_op_RBC_NOP,
        [RBC_MOVE] = &&reg_op_RBC_MOVE,
        [RBC_CHECK_BOUND] = &&reg_op_RBC_CHECK_BOUND,
        [RBC_LOAD_CONST] = &&reg_op_RBC_LOAD_CONST,
        [RBC_LOAD_NONE] = &&reg_op_RBC_LOAD_NONE,
        [RBC_LOAD_NIL] = &&reg_op_RBC_LOAD_NIL,
        [RBC_LOAD_ELLIPSIS] = &&reg_op_RBC_LOAD_ELLIPSIS,
        [RBC_LOAD_BOOL] = &&reg_op_RBC_LOAD_BOOL,
        [RBC_LOAD_I16] = &&reg_op_RBC_LOAD_I16,
        [RBC_LOAD_NAME_INT] = &&reg_op_RBC_LOAD_NAME_INT,
        [RBC_LOAD_GLOBAL] = &&reg_op_RBC_LOAD_GLOBAL,
        [RBC_LOAD_GLOBAL_IC] = &&reg_op_RBC_LOAD_GLOBAL_IC,
        [RBC_LOAD_NONLOCAL] = &&reg_op_RBC_LOAD_NONLOCAL,
        [RBC_LOAD_NONLOCAL_IC] = &&reg_op_RBC_LOAD_NONLOCAL_IC,
        [RBC_LOAD_FUNCTION] = &&reg_op_RBC_LOAD_FUNCTION,
        [RBC_LOAD_ATTR] = &&reg_op_RBC_LOAD_ATTR,
        [RBC_LOAD_ATTR_IC] = &&reg_op_RBC_LOAD_ATTR_IC,
        [RBC_LOAD_METHOD] = &&reg_op_RBC_LOAD_METHOD,
        [RBC_LOAD_METHOD_IC] = &&reg_op_RBC_LOAD_METHOD_IC,
        [RBC_LOAD_SUBSCR] = &&reg_op_RBC_LOAD_SUBSCR,
        [RBC_STORE_GLOBAL] = &&reg_op_RBC_STORE_GLOBAL,
        [RBC_STORE_ATTR] = &&reg_op_RBC_STORE_ATTR,
        [RBC_STORE_ATTR_IC] = &&reg_op_RBC_STORE_ATTR_IC,
        [RBC_STORE_SUBSCR] = &&reg_op_RBC_STORE_SUBSCR,
        [RBC_DELETE_FAST] = &&reg_op_RBC_DELETE_FAST,
        [RBC_DELETE_GLOBAL] = &&reg_op_RBC_DELETE_GLOBAL,
        [RBC_DELETE_ATTR] = &&reg_op_RBC_DELETE_ATTR,
        [RBC_DELETE_SUBSCR] = &&reg_op_RBC_DELETE_SUBSCR,
        [RBC_BUILD_IMAG] = &&reg_op_RBC_BUILD_IMAG,
        [RBC_BUILD_BYTES] = &&reg_op_RBC_BUILD_BYTES,
        [RBC_BUILD_TUPLE] = &&reg_op_RBC_BUILD_TUPLE,
        [RBC_BUILD_LIST] = &&reg_op_RBC_BUILD_LIST,
        [RBC_BUILD_DICT] = &&reg_op_RBC_BUILD_DICT,
        [RBC_BUILD_SLICE] = &&reg_op_RBC_BUILD_SLICE,
        [RBC_BUILD_STRING] = &&reg_op_RBC_BUILD_STRING,
        [RBC_ADD] = &&reg_op_RBC_ADD,
        [RBC_SUB] = &&reg_op_RBC_SUB,
        [RBC_MUL] = &&reg_op_RBC_MUL,
        [RBC_TRUEDIV] = &&reg_op_RBC_TRUEDIV,
        [RBC_FLOORDIV] = &&reg_op_RBC_FLOORDIV,
        [RBC_MOD] = &&reg_op_RBC_MOD,
        [RBC_POW] = &&reg_op_RBC_POW,
        [RBC_LSHIFT] = &&reg_op_RBC_LSHIFT,
        [RBC_RSHIFT] = &&reg_op_RBC_RSHIFT,
        [RBC_BITAND] = &&reg_op_RBC_BITAND,
        [RBC_BITOR] = &&reg_op_RBC_BITOR,
        [RBC_BITXOR] = &&reg_op_RBC_BITXOR,
        [RBC_MATMUL] = &&reg_op_RBC_MATMUL,
        [RBC_CMP_LT] = &&reg_op_RBC_CMP_LT,
        [RBC_CMP_LE] = &&reg_op_RBC_CMP_LE,
        [RBC_CMP_EQ] = &&reg_op_RBC_CMP_EQ,
        [RBC_CMP_NE] = &&reg_op_RBC_CMP_NE,
        [RBC_CMP_GT] = &&reg_op_RBC_CMP_GT,
        [RBC_CMP_GE] = &&reg_op_RBC_CMP_GE,
        [RBC_ADD_INT] = &&reg_op_RBC_ADD_INT,
        [RBC_SUB_INT] = &&reg_op_RBC_SUB_INT,
        [RBC_MUL_INT] = &&reg_op_RBC_MUL_INT,
        [RBC_ADD_NUM] = &&reg_op_RBC_ADD_NUM,
        [RBC_SUB_NUM] = &&reg_op_RBC_SUB_NUM,
        [RBC_MUL_NUM] = &&reg_op_RBC_MUL_NUM,
        [RBC_CMP_LT_INT] = &&reg_op_RBC_CMP_LT_INT,
        [RBC_CMP_LE_INT] = &&reg_op_RBC_CMP_LE_INT,
        [RBC_CMP_EQ_INT] = &&reg_op_RBC_CMP_EQ_INT,
        [RBC_CMP_NE_INT] = &&reg_op_RBC_CMP_NE_INT,
        [RBC_CMP_GT_INT] = &&reg_op_RBC_CMP_GT_INT,
        [RBC_CMP_GE_INT] = &&reg_op_RBC_CMP_GE_INT,
        [RBC_CMP_LT_NUM] = &&reg_op_RBC_CMP_LT_NUM,
        [RBC_CMP_LE_NUM] = &&reg_op_RBC_CMP_LE_NUM,
        [RBC_CMP_EQ_NUM] = &&reg_op_RBC_CMP_EQ_NUM,
        [RBC_CMP_NE_NUM] = &&reg_op_RBC_CMP_NE_NUM,
        [RBC_CMP_GT_NUM] = &&reg_op_RBC_CMP_GT_NUM,
        [RBC_CMP_GE_NUM] = &&reg_op_RBC_CMP_GE_NUM,
        [RBC_ADD_I8] = &&reg_op_RBC_ADD_I8,
        [RBC_SUB_I8] = &&reg_op_RBC_SUB_I8,
        [RBC_MUL_I8] = &&reg_op_RBC_MUL_I8,
        [RBC_CMP_LT_I8] = &&reg_op_RBC_CMP_LT_I8,
        [RBC_CMP_LE_I8] = &&reg_op_RBC_CMP_LE_I8,
        [RBC_CMP_EQ_I8] = &&reg_op_RBC_CMP_EQ_I8,
        [RBC_CMP_NE_I8] = &&reg_op_RBC_CMP_NE_I8,
        [RBC_CMP_GT_I8] = &&reg_op_RBC_CMP_GT_I8,
        [RBC_CMP_GE_I8] = &&reg_op_RBC_CMP_GE_I8,
        [RBC_CMP_BRANCH] = &&reg_op_RBC_CMP_BRANCH,
        [RBC_CMP_I8_BRANCH] = &&reg_op_RBC_CMP_I8_BRANCH,
        [RBC_SUPER_DATA] = &&reg_op_RBC_SUPER_DATA,
        [RBC_IS] = &&reg_op_RBC_IS,
        [RBC_IS_NOT] = &&reg_op_RBC_IS_NOT,
        [RBC_CONTAINS] = &&reg_op_RBC_CONTAINS,
        [RBC_NOT_CONTAINS] = &&reg_op_RBC_NOT_CONTAINS,
        [RBC_NEG] = &&reg_op_RBC_NEG,
        [RBC_NOT] = &&reg_op_RBC_NOT,
        [RBC_INVERT] = &&reg_op_RBC_INVERT,
        [RBC_GET_ITER] = &&reg_op_RBC_GET_ITER,
        [RBC_FOR_ITER] = &&reg_op_RBC_FOR_ITER,
        [RBC_FOR_GENERATOR] = &&reg_op_RBC_FOR_GENERATOR,
        [RBC_FOR_ARRAY2D] = &&reg_op_RBC_FOR_ARRAY2D,
        [RBC_FOR_LIST] = &&reg_op_RBC_FOR_LIST,
        [RBC_FOR_TUPLE] = &&reg_op_RBC_FOR_TUPLE,
        [RBC_FOR_DICT] = &&reg_op_RBC_FOR_DICT,
        [RBC_FOR_RANGE] = &&reg_op_RBC_FOR_RANGE,
        [RBC_FOR_STR] = &&reg_op_RBC_FOR_STR,
        [RBC_JUMP] = &&reg_op_RBC_JUMP,
        [RBC_JUMP_IF_FALSE] = &&reg_op_RBC_JUMP_IF_FALSE,
        [RBC_JUMP_IF_TRUE] = &&reg_op_RBC_JUMP_IF_TRUE,
        [RBC_JUMP_IF_TRUE_OR_POP] = &&reg_op_RBC_JUMP_IF_TRUE_OR_POP,
        [RBC_JUMP_IF_FALSE_OR_POP] = &&reg_op_RBC_JUMP_IF_FALSE_OR_POP,
        [RBC_SHORTCUT_IF_FALSE_OR_POP] = &&reg_op_RBC_SHORTCUT_IF_FALSE_OR_POP,
        [RBC_JUMP_IF_NOT_MATCH] = &&reg_op_RBC_JUMP_IF_NOT_MATCH,
        [RBC_CALL] = &&reg_op_RBC_CALL,
        [RBC_CALL_IC] = &&reg_op_RBC_CALL_IC,
        [RBC_LIST_APPEND] = &&reg_op_RBC_LIST_APPEND,
        [RBC_DICT_ADD] = &&reg_op_RBC_DICT_ADD,
        [RBC_UNPACK_SEQUENCE] = &&reg_op_RBC_UNPACK_SEQUENCE,
        [RBC_UNPACK_EX] = &&reg_op_RBC_UNPACK_EX,
        [RBC_FORMAT_STRING] = &&reg_op_RBC_FORMAT_STRING,
        [RBC_RETURN] = &&reg_op_RBC_RETURN,
        [RBC_RETURN_NONE] = &&reg_op_RBC_RETURN_NONE,
    };
#pragma GCC diagnostic pop
#endif
    int32_t here;
    RegBytecode inst;
    RegOpcode op;
    uint8_t A, B, C;

    while(PK_REGVM_LIKELY((uint32_t)pc < (uint32_t)code_length)) {
        here = pc;
        inst = code[pc++];
        op = RBC_OP(inst);
        PK_REGVM_ASSUME((unsigned)op < RBC__COUNT);
        A = RBC_A(inst);
        B = RBC_B(inst);
        C = RBC_C(inst);
#if PK_REGVM_DISPATCH_THREADED
    reg_dispatch_switch:
#endif
        switch(op) {
            REG_CASE(RBC_NOP) REG_DISPATCH_NEXT();
            REG_CASE(RBC_MOVE) R[A] = R[B]; REG_DISPATCH_NEXT();
            REG_CASE(RBC_CHECK_BOUND)
                if(PK_CEVAL_UNLIKELY(py_isnil(&R[A]))) {
                    py_Name name = varnames[A];
                    UnboundLocalError(name);
                    REG_FAIL();
                }
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LOAD_CONST) R[A] = consts[RBC_BX(inst)]; REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_NONE) py_newnone(&R[A]); REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_NIL) py_newnil(&R[A]); REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_ELLIPSIS) py_newellipsis(&R[A]); REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_BOOL) py_newbool(&R[A], B != 0); REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_I16) py_newint(&R[A], RBC_SBX(inst)); REG_DISPATCH_NEXT();
            REG_CASE(RBC_LOAD_NAME_INT) {
                py_Name name = names[RBC_BX(inst)];
                py_newint(&R[A], (uintptr_t)name);
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_LOAD_GLOBAL)
            REG_CASE(RBC_LOAD_GLOBAL_IC)
                if(!reg_exec_load_global(self, frame, rc, R, here, op, A, names[RBC_BX(inst)],
                                         profile_enabled))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LOAD_NONLOCAL)
            REG_CASE(RBC_LOAD_NONLOCAL_IC)
                if(!reg_exec_load_nonlocal(self, frame, rc, R, here, op, A, names[RBC_BX(inst)],
                                           current_fn->closure == NULL, profile_enabled))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LOAD_FUNCTION) {
                REG_SYNC_IP();
                FuncDecl_ decl = func_decls[RBC_BX(inst)];
                Function* fn = py_newobject(&R[A], tp_function, 0, sizeof(Function));
                Function__ctor(fn, decl, frame->module, frame->globals);
                if(decl->nested) {
                    fn->closure = FastLocals__to_namedict(frame->locals, frame->co);
                    py_Name name = py_name(decl->code.name->data);
                    NameDict__set(fn->closure, name, &R[A]);
                } else if(self->curr_class) {
                    fn->clazz = self->curr_class->_obj;
                }
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_LOAD_ATTR)
            REG_CASE(RBC_LOAD_ATTR_IC)
                REG_SYNC_IP();
                if(!reg_exec_load_attr(self, rc, R, here, op, A, names[RBC_BX(inst)],
                                       profile_enabled))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LOAD_METHOD)
            REG_CASE(RBC_LOAD_METHOD_IC)
                REG_SYNC_IP();
                if(!reg_exec_load_method(self, rc, R, here, op, A, names[RBC_BX(inst)],
                                         profile_enabled))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LOAD_SUBSCR)
                REG_SYNC_IP();
                if(!py_getitem(&R[B], &R[C])) REG_FAIL();
                R[A] = self->last_retval;
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_STORE_GLOBAL)
                if(!reg_exec_store_global(self, frame, rc, R, here, A, names[RBC_BX(inst)]))
                    REG_FAIL();
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_STORE_ATTR)
            REG_CASE(RBC_STORE_ATTR_IC)
                if(!reg_exec_store_attr(self, frame, rc, R, here, source_ip[here], op, A,
                                        names[RBC_BX(inst)]))
                    REG_FAIL();
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_STORE_SUBSCR)
                REG_SYNC_IP();
                if(!py_setitem(&R[B], &R[C], &R[A])) REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_DELETE_FAST)
                if(py_isnil(&R[A])) {
                    py_Name name = varnames[A];
                    UnboundLocalError(name);
                    REG_FAIL();
                }
                py_newnil(&R[A]);
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_DELETE_GLOBAL) {
                py_Name name = names[RBC_BX(inst)];
                int res = Frame__delglobal(frame, name);
                if(res == 1) break;
                if(res == -1) REG_FAIL();
                NameError(name);
                REG_FAIL();
            }
            REG_CASE(RBC_DELETE_ATTR) {
                REG_SYNC_IP();
                py_Name name = names[RBC_BX(inst)];
                if(!py_delattr(&R[A], name)) REG_FAIL();
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_DELETE_SUBSCR)
                REG_SYNC_IP();
                if(!py_delitem(&R[A], &R[B])) REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_BUILD_IMAG)
                REG_SYNC_IP();
                if(!RegVM__slow_build_imag(self, &R[A], &R[A])) REG_FAIL();
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_BUILD_BYTES) {
                REG_SYNC_IP();
                py_Ref string = (py_Ref)&consts[RBC_BX(inst)];
                int size;
                const char* data = py_tostrn(string, &size);
                unsigned char* p = py_newbytes(&R[A], size);
                memcpy(p, data, (size_t)size);
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_BUILD_TUPLE) {
                REG_SYNC_IP();
                uint8_t count = C;
                bool need_track = false;
                py_Ref p = py_newtuple(&R[A], count);
                for(uint8_t i = 0; i < count; i++) {
                    p[i] = R[B + i];
                    if(p[i].is_ptr) need_track = true;
                }
                if(!need_track) R[A]._obj->gc_marked |= 0b10;
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_BUILD_LIST)
                REG_SYNC_IP();
                py_newlistn(&R[A], C);
                for(uint8_t i = 0; i < C; i++) py_list_setitem(&R[A], i, &R[B + i]);
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_BUILD_DICT)
                REG_SYNC_IP();
                py_newdict(&R[A]);
                for(uint8_t i = 0; i < C; i++) {
                    if(!py_dict_setitem(&R[A], &R[B + i * 2], &R[B + i * 2 + 1]))
                        REG_FAIL();
                }
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_BUILD_SLICE) {
                REG_SYNC_IP();
                py_Ref slots = py_newslice(py_retval());
                slots[0] = R[A];
                slots[1] = R[A + 1];
                slots[2] = R[A + 2];
                R[A] = self->last_retval;
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_BUILD_STRING)
                REG_SYNC_IP();
                if(!RegVM__slow_build_string(self, &R[A], &R[B], C)) REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_ADD)
            REG_CASE(RBC_SUB)
            REG_CASE(RBC_MUL)
            REG_CASE(RBC_TRUEDIV)
            REG_CASE(RBC_FLOORDIV)
            REG_CASE(RBC_MOD)
            REG_CASE(RBC_POW)
            REG_CASE(RBC_LSHIFT)
            REG_CASE(RBC_RSHIFT)
            REG_CASE(RBC_BITAND)
            REG_CASE(RBC_BITOR)
            REG_CASE(RBC_BITXOR)
            REG_CASE(RBC_MATMUL)
            REG_CASE(RBC_CMP_LT)
            REG_CASE(RBC_CMP_LE)
            REG_CASE(RBC_CMP_EQ)
            REG_CASE(RBC_CMP_NE)
            REG_CASE(RBC_CMP_GT)
            REG_CASE(RBC_CMP_GE) {
                py_Type lhs_type = R[B].type;
                py_Type rhs_type = R[C].type;
                if(profile_enabled) RegProfile__record_type_pair(rc, here, lhs_type, rhs_type);
                if(!reg_binary(self, frame, source_ip[here], op, &R[A], &R[B], &R[C])) REG_FAIL();
                reg_maybe_quicken_binary(rc, here, op, lhs_type, rhs_type);
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_ADD_INT)
            REG_CASE(RBC_SUB_INT)
            REG_CASE(RBC_MUL_INT)
            REG_CASE(RBC_ADD_NUM)
            REG_CASE(RBC_SUB_NUM)
            REG_CASE(RBC_MUL_NUM)
            REG_CASE(RBC_CMP_LT_INT)
            REG_CASE(RBC_CMP_LE_INT)
            REG_CASE(RBC_CMP_EQ_INT)
            REG_CASE(RBC_CMP_NE_INT)
            REG_CASE(RBC_CMP_GT_INT)
            REG_CASE(RBC_CMP_GE_INT)
            REG_CASE(RBC_CMP_LT_NUM)
            REG_CASE(RBC_CMP_LE_NUM)
            REG_CASE(RBC_CMP_EQ_NUM)
            REG_CASE(RBC_CMP_NE_NUM)
            REG_CASE(RBC_CMP_GT_NUM)
            REG_CASE(RBC_CMP_GE_NUM) {
                RegOpcode base = reg_specialized_base(op);
                bool handled = false;
                if(profile_enabled) RegProfile__record_type_pair(rc, here, R[B].type, R[C].type);

                if(reg_is_int_specialization(op) && R[B].type == tp_int && R[C].type == tp_int) {
                    py_i64 lhs = R[B]._i64;
                    py_i64 rhs = R[C]._i64;
                    switch(base) {
                        case RBC_ADD: py_newint(&R[A], lhs + rhs); break;
                        case RBC_SUB: py_newint(&R[A], lhs - rhs); break;
                        case RBC_MUL: py_newint(&R[A], lhs * rhs); break;
                        default: py_newbool(&R[A], reg_compare_i64(base, lhs, rhs)); break;
                    }
                    handled = true;
                } else if(reg_is_num_specialization(op) && reg_isnum(&R[B]) && reg_isnum(&R[C]) &&
                          (R[B].type == tp_float || R[C].type == tp_float)) {
                    py_f64 lhs = reg_tof64(&R[B]);
                    py_f64 rhs = reg_tof64(&R[C]);
                    switch(base) {
                        case RBC_ADD: py_newfloat(&R[A], lhs + rhs); break;
                        case RBC_SUB: py_newfloat(&R[A], lhs - rhs); break;
                        case RBC_MUL: py_newfloat(&R[A], lhs * rhs); break;
                        default: py_newbool(&R[A], reg_compare_f64(base, lhs, rhs)); break;
                    }
                    handled = true;
                }

                if(handled) {
                    runtime_sites[here].misses = 0;
                    break;
                }

                (void)reg_specialization_miss(rc, here, base);
                if(!reg_binary(self, frame, source_ip[here], base, &R[A], &R[B], &R[C])) REG_FAIL();
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_ADD_I8)
            REG_CASE(RBC_SUB_I8)
            REG_CASE(RBC_MUL_I8)
            REG_CASE(RBC_CMP_LT_I8)
            REG_CASE(RBC_CMP_LE_I8)
            REG_CASE(RBC_CMP_EQ_I8)
            REG_CASE(RBC_CMP_NE_I8)
            REG_CASE(RBC_CMP_GT_I8)
            REG_CASE(RBC_CMP_GE_I8) {
                py_i64 imm = (py_i64)RBC_I8(inst);
                if(profile_enabled) RegProfile__record_type_pair(rc, here, R[B].type, tp_int);
                if(R[B].type == tp_int) {
                    py_i64 lhs = R[B]._i64;
                    switch(op) {
                        case RBC_ADD_I8: py_newint(&R[A], lhs + imm); break;
                        case RBC_SUB_I8: py_newint(&R[A], lhs - imm); break;
                        case RBC_MUL_I8: py_newint(&R[A], lhs * imm); break;
                        default: py_newbool(&R[A], reg_compare_i64(op, lhs, imm)); break;
                    }
                    break;
                }
                if(R[B].type == tp_float) {
                    py_f64 lhs = R[B]._f64;
                    py_f64 rhs = (py_f64)imm;
                    switch(op) {
                        case RBC_ADD_I8: py_newfloat(&R[A], lhs + rhs); break;
                        case RBC_SUB_I8: py_newfloat(&R[A], lhs - rhs); break;
                        case RBC_MUL_I8: py_newfloat(&R[A], lhs * rhs); break;
                        default: py_newbool(&R[A], reg_compare_f64(op, lhs, rhs)); break;
                    }
                    break;
                }

                py_TValue rhs;
                py_newint(&rhs, imm);
                RegOpcode base;
                switch(op) {
                    case RBC_ADD_I8: base = RBC_ADD; break;
                    case RBC_SUB_I8: base = RBC_SUB; break;
                    case RBC_MUL_I8: base = RBC_MUL; break;
                    case RBC_CMP_LT_I8: base = RBC_CMP_LT; break;
                    case RBC_CMP_LE_I8: base = RBC_CMP_LE; break;
                    case RBC_CMP_EQ_I8: base = RBC_CMP_EQ; break;
                    case RBC_CMP_NE_I8: base = RBC_CMP_NE; break;
                    case RBC_CMP_GT_I8: base = RBC_CMP_GT; break;
                    case RBC_CMP_GE_I8: base = RBC_CMP_GE; break;
                    default: c11__unreachable();
                }
                if(!reg_binary(self, frame, source_ip[here], base, &R[A], &R[B], &rhs)) REG_FAIL();
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_CMP_BRANCH) {
                RegBytecode ext = code[pc++];
                c11__rtassert(RBC_OP(ext) == RBC_SUPER_DATA);
                RegOpcode cmp = reg_compare_code_opcode(C);
                int truth;
                if(R[A].type == tp_int && R[B].type == tp_int) {
                    truth = reg_compare_i64(cmp, R[A]._i64, R[B]._i64);
                } else if(reg_isnum(&R[A]) && reg_isnum(&R[B])) {
                    truth = reg_compare_f64(cmp, reg_tof64(&R[A]), reg_tof64(&R[B]));
                } else {
                    py_TValue result;
                    REG_SYNC_IP();
                    if(!RegVM__slow_binary(cmp, &result, &R[A], &R[B])) REG_FAIL();
                    truth = pk_ceval_bool(&result);
                    if(truth < 0) REG_FAIL();
                }
                bool branch_on_true = (C & REG_CMP_BRANCH_TRUE) != 0;
                bool take = (truth != 0) == branch_on_true;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) {
                    REG_BRANCH_TO((here + 1) + RBC_SBX(ext));
                } else {
                    pc += RBC_A(ext);
                }
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_CMP_I8_BRANCH) {
                RegBytecode ext = code[pc++];
                c11__rtassert(RBC_OP(ext) == RBC_SUPER_DATA);
                RegOpcode cmp = reg_compare_code_opcode(C);
                py_i64 imm = (py_i64)(int8_t)B;
                int truth;
                if(R[A].type == tp_int) {
                    truth = reg_compare_i64(cmp, R[A]._i64, imm);
                } else if(R[A].type == tp_float) {
                    truth = reg_compare_f64(cmp, R[A]._f64, (py_f64)imm);
                } else {
                    py_TValue rhs;
                    py_newint(&rhs, imm);
                    py_TValue result;
                    REG_SYNC_IP();
                    if(!RegVM__slow_binary(cmp, &result, &R[A], &rhs)) REG_FAIL();
                    truth = pk_ceval_bool(&result);
                    if(truth < 0) REG_FAIL();
                }
                bool branch_on_true = (C & REG_CMP_BRANCH_TRUE) != 0;
                bool take = (truth != 0) == branch_on_true;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) {
                    REG_BRANCH_TO((here + 1) + RBC_SBX(ext));
                } else {
                    pc += RBC_A(ext);
                }
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_SUPER_DATA) c11__abort("entered register superinstruction payload");

            REG_CASE(RBC_IS)
            REG_CASE(RBC_IS_NOT) {
                bool value = py_isidentical(&R[B], &R[C]);
                if(op == RBC_IS_NOT) value = !value;
                py_newbool(&R[A], value);
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_CONTAINS)
            REG_CASE(RBC_NOT_CONTAINS)
                REG_SYNC_IP();
                if(!RegVM__slow_contains(&R[A], &R[B], &R[C], op == RBC_NOT_CONTAINS))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_NEG)
                REG_SYNC_IP();
                if(!pk_callmagic(__neg__, 1, &R[B])) REG_FAIL();
                R[A] = self->last_retval;
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_NOT) {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[B]);
                int value = pk_ceval_bool(&R[B]);
                if(value < 0) REG_FAIL();
                py_newbool(&R[A], !value);
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_INVERT)
                if(R[B].type == tp_int) {
                    py_newint(&R[A], ~R[B]._i64);
                } else {
                    REG_SYNC_IP();
                    if(!pk_callmagic(__invert__, 1, &R[B])) REG_FAIL();
                    R[A] = self->last_retval;
                }
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_GET_ITER)
                REG_SYNC_IP();
                if(!py_iter(&R[B])) REG_FAIL();
                R[A] = self->last_retval;
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_FOR_ITER) {
                py_Type iterator_type = R[A].type;
                if(iterator_type == tp_generator ||
                   (iterator_type != tp_array2d_like_iterator && iterator_type != tp_list_iterator &&
                    iterator_type != tp_tuple_iterator && iterator_type != tp_dict_iterator &&
                    iterator_type != tp_range_iterator && iterator_type != tp_str_iterator)) {
                    REG_SYNC_IP();
                }
                int res = pk_ceval_next(&R[A]);
                if(res < 0) REG_FAIL();
                if(profile_enabled) RegProfile__record_branch(rc, here, res == 0);
                if(res) {
                    R[A + 1] = self->last_retval;
                } else {
                    REG_BRANCH_TO(here + RBC_SBX(inst));
                }

                RegOpcode specialized = reg_iterator_specialization(iterator_type);
                if(specialized != RBC_NOP) {
                    RegRuntimeSite* site = &runtime_sites[here];
                    if(reg_observe(site, (uint32_t)(uint16_t)iterator_type)) {
                        reg_mark_quickened(rc, here, specialized);
                    }
                }
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_FOR_GENERATOR)
            REG_CASE(RBC_FOR_ARRAY2D)
            REG_CASE(RBC_FOR_LIST)
            REG_CASE(RBC_FOR_TUPLE)
            REG_CASE(RBC_FOR_DICT)
            REG_CASE(RBC_FOR_RANGE)
            REG_CASE(RBC_FOR_STR) {
                if(op == RBC_FOR_GENERATOR) REG_SYNC_IP();
                int res = reg_specialized_next(op, &R[A]);
                if(res == -2) {
                    (void)reg_specialization_miss(rc, here, RBC_FOR_ITER);
                    res = pk_ceval_next(&R[A]);
                } else {
                    runtime_sites[here].misses = 0;
                }
                if(res < 0) REG_FAIL();
                if(profile_enabled) RegProfile__record_branch(rc, here, res == 0);
                if(res) {
                    R[A + 1] = self->last_retval;
                } else {
                    REG_BRANCH_TO(here + RBC_SBX(inst));
                }
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_JUMP) REG_BRANCH_TO(here + RBC_SBX(inst)); REG_DISPATCH_NEXT();
            REG_CASE(RBC_JUMP_IF_FALSE)
            REG_CASE(RBC_JUMP_IF_TRUE)
            REG_CASE(RBC_JUMP_IF_TRUE_OR_POP)
            REG_CASE(RBC_JUMP_IF_FALSE_OR_POP) {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[A]);
                int value = pk_ceval_bool(&R[A]);
                if(value < 0) REG_FAIL();
                bool take = op == RBC_JUMP_IF_FALSE || op == RBC_JUMP_IF_FALSE_OR_POP ? !value : value;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) REG_BRANCH_TO(here + RBC_SBX(inst));
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_SHORTCUT_IF_FALSE_OR_POP) {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[A + 1]);
                int value = pk_ceval_bool(&R[A + 1]);
                if(value < 0) REG_FAIL();
                bool take = !value;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) {
                    py_newbool(&R[A], false);
                    REG_BRANCH_TO(here + RBC_SBX(inst));
                }
                REG_DISPATCH_NEXT();
            }
            REG_CASE(RBC_JUMP_IF_NOT_MATCH) {
                REG_SYNC_IP();
                int equal = py_equal(&R[A], &R[A + 1]);
                if(equal < 0) REG_FAIL();
                bool take = !equal;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) REG_BRANCH_TO(here + RBC_SBX(inst));
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_CALL)
            REG_CASE(RBC_CALL_IC)
                REG_SYNC_IP();
                if(!reg_exec_call(self, rc, R, frame_limit, here, op, A, B, C, profile_enabled))
                    REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_LIST_APPEND)
                py_list_append(&R[A], &R[B]);
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_DICT_ADD)
                REG_SYNC_IP();
                if(!py_dict_setitem(&R[A], &R[B], &R[C])) REG_FAIL();
                REG_DISPATCH_NEXT();

            REG_CASE(RBC_UNPACK_SEQUENCE)
                if(!RegVM__slow_unpack_sequence(&R[B], &R[A], C)) REG_FAIL();
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_UNPACK_EX)
                if(!RegVM__slow_unpack_ex(&R[B], &R[A], C)) REG_FAIL();
                REG_DISPATCH_NEXT();
            REG_CASE(RBC_FORMAT_STRING) {
                REG_SYNC_IP();
                py_Ref spec = (py_Ref)&consts[RBC_BX(inst)];
                if(!pk_format_object(self, &R[A], py_tosv(spec))) REG_FAIL();
                R[A] = self->last_retval;
                REG_DISPATCH_NEXT();
            }

            REG_CASE(RBC_RETURN)
                self->last_retval = R[A];
                VM__pop_frame(self);
                return RES_RETURN;
            REG_CASE(RBC_RETURN_NONE)
                py_newnone(&self->last_retval);
                VM__pop_frame(self);
                return RES_RETURN;
            REG_CASE(RBC_RAISE)
                REG_SYNC_IP();
                if(B == 0) {
                    /* Same steps as the stack interpreter's OP_RAISE. */
                    if(py_istype(&R[A], tp_type)) {
                        if(!py_tpcall(py_totype(&R[A]), 0, NULL)) REG_FAIL();
                        R[A] = self->last_retval;
                    }
                    if(!py_isinstance(&R[A], tp_BaseException)) {
                        TypeError("exceptions must derive from BaseException");
                        REG_FAIL();
                    }
                    py_raise(&R[A]);
                } else if(B == 1) {
                    if(!py_str(&R[A])) REG_FAIL();
                    py_exception(tp_AssertionError, "%s", py_tostr(py_retval()));
                } else {
                    py_exception(tp_AssertionError, "");
                }
                REG_FAIL();

            default: c11__unreachable();
        }
    }

#undef REG_CASE
#undef REG_DISPATCH_NEXT
#undef REG_SYNC_IF_DYNAMIC_TRUTH
#undef REG_BRANCH_TO
#undef REG_FAIL
#undef REG_SYNC_IP
    c11__abort("register bytecode fell off the end without RETURN");
}

FrameResult VM__run_top_frame(VM* self) {
#if PK_ENABLE_REGVM
    RegCode* code;
    if(RegVM__can_execute_now(self, self->top_frame, &code)) return reg_run(self, code, -1);
#endif
    return VM__run_top_frame_stack(self);
}
