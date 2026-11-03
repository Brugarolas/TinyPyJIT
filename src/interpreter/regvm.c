#include "pocketpy/interpreter/regvm.h"
#include "pocketpy/interpreter/jitbridge.h"
#include "pocketpy/interpreter/regprofile.h"
#include "pocketpy/interpreter/perf.h"
#include "pocketpy/interpreter/shape.h"
#include "pocketpy/interpreter/regvm_slow.h"

#include "pocketpy/common/sstream.h"
#include "pocketpy/common/utils.h"
#include "pocketpy/interpreter/fastpath.h"
#include "pocketpy/interpreter/bindings.h"
#include "pocketpy/interpreter/typeinfo.h"
#include "pocketpy/objects/namedict.h"
#include "pocketpy/objects/object.h"
#include "pocketpy/objects/error.h"
#include "pocketpy/objects/exception.h"
#include "pocketpy/pocketpy.h"

#include <assert.h>
#include <string.h>

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

static bool reg_global_cache_apply(VM* self,
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
static int reg_attr_cache_apply(VM* self,
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

static bool reg_method_cache_fill(VM* self,
                                  py_Ref object,
                                  py_Name name,
                                  RegInlineCache* cache) {
    if(name == __new__ || object->type == tp_super) return false;
    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getunboundmethod != NULL) return false;

    py_Ref cls_var = pk_tpfindname(ti, name);
    if(cls_var == NULL) return false;

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
    return true;
}

static bool reg_method_cache_apply(VM* self,
                                   py_Ref object,
                                   const RegInlineCache* cache,
                                   py_Ref out_callable,
                                   py_Ref out_self) {
    if(cache == NULL || cache->kind != REG_IC_KIND_METHOD || object->type != cache->type ||
       self->type_version != cache->version0)
        return false;

    py_TypeInfo* ti = pk_typeinfo(object->type);
    if(ti->getunboundmethod != NULL || object->type == tp_super) return false;

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

    if(callable->type != tp_function) return false;
    Function* fn = py_touserdata(callable);
    if(fn->decl == NULL || fn->decl->type != FuncType_SIMPLE) return false;
    cache->variant = fn->cfunc ? REG_CALL_CACHE_DECL_CFUNC_SIMPLE : REG_CALL_CACHE_PY_SIMPLE;
    cache->guard0 = (uintptr_t)callable->_obj;
    cache->guard1 = (uintptr_t)fn->decl;
    return true;
}

static bool reg_call_simple_prepare(VM* self,
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

    VM__push_frame(self, Frame__new_stack(co, p0, fn->module, &fn->globals, argv, false));
    *result = RES_CALL;
    return true;
}

static bool reg_call_cache_apply(VM* self,
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
    return code;
}

int RegVM__frame_slots(FuncDecl* decl) {
    RegCode* code = RegVM__ensure_code(decl);
    return code ? code->nregs : decl->code.nlocals;
}

bool RegVM__can_run_frame(py_Frame* frame, RegCode** out_code) {
    if(frame == NULL || frame->is_locals_special) return false;
    if(frame->p0 == NULL || frame->p0->type != tp_function) return false;
    Function* fn = py_touserdata(frame->p0);
    if(fn->cfunc != NULL || &fn->decl->code != frame->co) return false;
    RegCode* code = RegVM__ensure_code(fn->decl);
    if(code == NULL) return false;
    if(out_code) *out_code = code;
    return true;
}

bool RegVM__can_execute_now(VM* self, py_Frame* frame, RegCode** out_code) {
    if(self->trace_info.func != NULL) return false;
#if PK_ENABLE_WATCHDOG
    if(self->watchdog_info.max_reset_time > 0) return false;
#endif
    return RegVM__can_run_frame(frame, out_code);
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

PK_REGVM_HOT PK_REGVM_ALIGN64 FrameResult VM__run_top_frame_reg(VM* self) {
    py_Frame* frame = self->top_frame;
    RegCode* rc;
    if(!RegVM__can_run_frame(frame, &rc)) return VM__run_top_frame_stack(self);

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
    const bool profile_enabled = self->regjit_hooks != NULL;
    const int32_t code_length = rc->code.length;
    int32_t pc = 0;

    if(profile_enabled) {
        if(RegProfile__function_entry(rc)) (void)RegJit__request_function(self, decl, rc);
        if(rc->function_jit_state == (uint8_t)REG_JIT_COMPILED) {
            RegJitExit exit;
            if(source_ip != NULL && rc->source_ip.length > 0) frame->ip = source_ip[0];
            if(RegJit__enter_function(self, frame, rc, &exit)) {
                FrameResult jit_result;
                if(reg_apply_jit_exit(self, frame, rc, &exit, &pc, &jit_result)) return jit_result;
            }
        }
    }

#define REG_SYNC_IP() do { frame->ip = source_ip[here]; } while(0)
#define REG_FAIL() do { return RegVM__slow_error(self, frame, source_ip[here]); } while(0)
#define REG_BRANCH_TO(target_expr)                                                                 \
    do {                                                                                            \
        int32_t _target_pc = (target_expr);                                                         \
        pc = _target_pc;                                                                            \
        if(PK_REGVM_UNLIKELY(profile_enabled && _target_pc < here)) {                               \
            FrameResult _jit_result;                                                                \
            if(reg_handle_backedge(self, frame, decl, rc, here, _target_pc, &pc, &_jit_result))    \
                return _jit_result;                                                                 \
        }                                                                                           \
    } while(0)
#define REG_SYNC_IF_DYNAMIC_TRUTH(v)                                                                \
    do {                                                                                            \
        py_Type _t = (v)->type;                                                                     \
        if(_t != tp_bool && _t != tp_int && _t != tp_float && _t != tp_NoneType) REG_SYNC_IP();    \
    } while(0)

    while(PK_REGVM_LIKELY((uint32_t)pc < (uint32_t)code_length)) {
        const int32_t here = pc;
        RegBytecode inst = code[pc++];
        RegOpcode op = RBC_OP(inst);
        PK_REGVM_ASSUME((unsigned)op < RBC__COUNT);
        uint8_t A = RBC_A(inst);
        uint8_t B = RBC_B(inst);
        uint8_t C = RBC_C(inst);
        switch(op) {
            case RBC_NOP: break;
            case RBC_MOVE: R[A] = R[B]; break;
            case RBC_CHECK_BOUND:
                if(PK_CEVAL_UNLIKELY(py_isnil(&R[A]))) {
                    py_Name name = varnames[A];
                    UnboundLocalError(name);
                    REG_FAIL();
                }
                break;

            case RBC_LOAD_CONST: R[A] = consts[RBC_BX(inst)]; break;
            case RBC_LOAD_NONE: py_newnone(&R[A]); break;
            case RBC_LOAD_NIL: py_newnil(&R[A]); break;
            case RBC_LOAD_ELLIPSIS: py_newellipsis(&R[A]); break;
            case RBC_LOAD_BOOL: py_newbool(&R[A], B != 0); break;
            case RBC_LOAD_I16: py_newint(&R[A], RBC_SBX(inst)); break;
            case RBC_LOAD_NAME_INT: {
                py_Name name = names[RBC_BX(inst)];
                py_newint(&R[A], (uintptr_t)name);
                break;
            }

            case RBC_LOAD_GLOBAL:
            case RBC_LOAD_GLOBAL_IC: {
                py_Name name = names[RBC_BX(inst)];
                RegRuntimeSite* site = &runtime_sites[here];
                bool dequickened = false;

                if(op == RBC_LOAD_GLOBAL_IC) {
                    RegInlineCache* cache = reg_cache_at(rc, site);
                    if(reg_global_cache_apply(self, frame, name, cache, &R[A])) {
                        site->misses = 0;
                        if(profile_enabled) RegProfile__record_ic_hit(rc, here);
                        break;
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
                    REG_FAIL();
                } else {
                    py_Ref tmp = py_getdict(self->builtins, name);
                    if(tmp == NULL) {
                        NameError(name);
                        REG_FAIL();
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
                break;
            }

            case RBC_LOAD_NONLOCAL: {
                py_Name name = names[RBC_BX(inst)];
                py_Ref tmp = Frame__getclosure(frame, name);
                if(tmp != NULL) {
                    R[A] = *tmp;
                    break;
                }
                int res = Frame__getglobal(frame, name);
                if(res == 1) {
                    R[A] = self->last_retval;
                    break;
                }
                if(res == -1) REG_FAIL();
                tmp = py_getdict(self->builtins, name);
                if(tmp != NULL) {
                    R[A] = *tmp;
                    break;
                }
                NameError(name);
                REG_FAIL();
            }

            case RBC_LOAD_FUNCTION: {
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
                break;
            }

            case RBC_LOAD_ATTR:
            case RBC_LOAD_ATTR_IC: {
                py_Name name = names[RBC_BX(inst)];
                py_TValue object = R[A];
                RegRuntimeSite* site = &runtime_sites[here];
                bool dequickened = false;
                if(profile_enabled)
                    RegProfile__record_shape(rc, here, object.type, reg_shape_identity(&object));

                REG_SYNC_IP();
                if(op == RBC_LOAD_ATTR_IC) {
                    RegInlineCache* cache = reg_cache_at(rc, site);
                    int cached = reg_attr_cache_apply(self, &object, name, cache, &R[A]);
                    if(cached > 0) {
                        site->misses = 0;
                        if(profile_enabled) RegProfile__record_ic_hit(rc, here);
                        break;
                    }
                    if(cached < 0) REG_FAIL();
                    if(profile_enabled) RegProfile__record_ic_miss(rc, here);
                    dequickened = reg_specialization_miss(rc, here, RBC_LOAD_ATTR);
                    if(!dequickened && cache != NULL) {
                        (void)reg_attr_cache_fill(self, &object, name, cache);
                    }
                }

                if(!py_getattr(&object, name)) REG_FAIL();
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
                break;
            }

            case RBC_LOAD_METHOD:
            case RBC_LOAD_METHOD_IC: {
                py_Name name = names[RBC_BX(inst)];
                py_TValue object = R[A];
                RegRuntimeSite* site = &runtime_sites[here];
                bool dequickened = false;
                if(profile_enabled)
                    RegProfile__record_shape(rc, here, object.type, reg_shape_identity(&object));

                REG_SYNC_IP();
                if(op == RBC_LOAD_METHOD_IC) {
                    RegInlineCache* cache = reg_cache_at(rc, site);
                    if(reg_method_cache_apply(self, &object, cache, &R[A], &R[A + 1])) {
                        site->misses = 0;
                        if(profile_enabled) RegProfile__record_ic_hit(rc, here);
                        break;
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
                    if(!py_getattr(&R[A], name)) REG_FAIL();
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
                break;
            }

            case RBC_LOAD_SUBSCR:
                REG_SYNC_IP();
                if(!py_getitem(&R[B], &R[C])) REG_FAIL();
                R[A] = self->last_retval;
                break;

            case RBC_STORE_GLOBAL: {
                py_Name name = names[RBC_BX(inst)];
                if(!Frame__setglobal(frame, name, &R[A])) REG_FAIL();
                break;
            }
            case RBC_STORE_ATTR: {
                REG_SYNC_IP();
                py_Name name = names[RBC_BX(inst)];
                if(!py_setattr(&R[A + 1], name, &R[A])) REG_FAIL();
                break;
            }
            case RBC_STORE_SUBSCR:
                REG_SYNC_IP();
                if(!py_setitem(&R[B], &R[C], &R[A])) REG_FAIL();
                break;

            case RBC_DELETE_FAST:
                if(py_isnil(&R[A])) {
                    py_Name name = varnames[A];
                    UnboundLocalError(name);
                    REG_FAIL();
                }
                py_newnil(&R[A]);
                break;
            case RBC_DELETE_GLOBAL: {
                py_Name name = names[RBC_BX(inst)];
                int res = Frame__delglobal(frame, name);
                if(res == 1) break;
                if(res == -1) REG_FAIL();
                NameError(name);
                REG_FAIL();
            }
            case RBC_DELETE_ATTR: {
                REG_SYNC_IP();
                py_Name name = names[RBC_BX(inst)];
                if(!py_delattr(&R[A], name)) REG_FAIL();
                break;
            }
            case RBC_DELETE_SUBSCR:
                REG_SYNC_IP();
                if(!py_delitem(&R[A], &R[B])) REG_FAIL();
                break;

            case RBC_BUILD_IMAG:
                REG_SYNC_IP();
                if(!RegVM__slow_build_imag(self, &R[A], &R[A])) REG_FAIL();
                break;
            case RBC_BUILD_BYTES: {
                REG_SYNC_IP();
                py_Ref string = (py_Ref)&consts[RBC_BX(inst)];
                int size;
                const char* data = py_tostrn(string, &size);
                unsigned char* p = py_newbytes(&R[A], size);
                memcpy(p, data, (size_t)size);
                break;
            }
            case RBC_BUILD_TUPLE: {
                REG_SYNC_IP();
                uint8_t count = C;
                bool need_track = false;
                py_Ref p = py_newtuple(&R[A], count);
                for(uint8_t i = 0; i < count; i++) {
                    p[i] = R[B + i];
                    if(p[i].is_ptr) need_track = true;
                }
                if(!need_track) R[A]._obj->gc_marked |= 0b10;
                break;
            }
            case RBC_BUILD_LIST:
                REG_SYNC_IP();
                py_newlistn(&R[A], C);
                for(uint8_t i = 0; i < C; i++) py_list_setitem(&R[A], i, &R[B + i]);
                break;
            case RBC_BUILD_DICT:
                REG_SYNC_IP();
                py_newdict(&R[A]);
                for(uint8_t i = 0; i < C; i++) {
                    if(!py_dict_setitem(&R[A], &R[B + i * 2], &R[B + i * 2 + 1]))
                        REG_FAIL();
                }
                break;
            case RBC_BUILD_SLICE: {
                REG_SYNC_IP();
                py_Ref slots = py_newslice(py_retval());
                slots[0] = R[A];
                slots[1] = R[A + 1];
                slots[2] = R[A + 2];
                R[A] = self->last_retval;
                break;
            }
            case RBC_BUILD_STRING:
                REG_SYNC_IP();
                if(!RegVM__slow_build_string(self, &R[A], &R[B], C)) REG_FAIL();
                break;

            case RBC_ADD:
            case RBC_SUB:
            case RBC_MUL:
            case RBC_TRUEDIV:
            case RBC_FLOORDIV:
            case RBC_MOD:
            case RBC_POW:
            case RBC_LSHIFT:
            case RBC_RSHIFT:
            case RBC_BITAND:
            case RBC_BITOR:
            case RBC_BITXOR:
            case RBC_MATMUL:
            case RBC_CMP_LT:
            case RBC_CMP_LE:
            case RBC_CMP_EQ:
            case RBC_CMP_NE:
            case RBC_CMP_GT:
            case RBC_CMP_GE: {
                py_Type lhs_type = R[B].type;
                py_Type rhs_type = R[C].type;
                if(profile_enabled) RegProfile__record_type_pair(rc, here, lhs_type, rhs_type);
                if(!reg_binary(self, frame, source_ip[here], op, &R[A], &R[B], &R[C])) REG_FAIL();
                reg_maybe_quicken_binary(rc, here, op, lhs_type, rhs_type);
                break;
            }

            case RBC_ADD_INT:
            case RBC_SUB_INT:
            case RBC_MUL_INT:
            case RBC_ADD_NUM:
            case RBC_SUB_NUM:
            case RBC_MUL_NUM:
            case RBC_CMP_LT_INT:
            case RBC_CMP_LE_INT:
            case RBC_CMP_EQ_INT:
            case RBC_CMP_NE_INT:
            case RBC_CMP_GT_INT:
            case RBC_CMP_GE_INT:
            case RBC_CMP_LT_NUM:
            case RBC_CMP_LE_NUM:
            case RBC_CMP_EQ_NUM:
            case RBC_CMP_NE_NUM:
            case RBC_CMP_GT_NUM:
            case RBC_CMP_GE_NUM: {
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
                break;
            }

            case RBC_ADD_I8:
            case RBC_SUB_I8:
            case RBC_MUL_I8:
            case RBC_CMP_LT_I8:
            case RBC_CMP_LE_I8:
            case RBC_CMP_EQ_I8:
            case RBC_CMP_NE_I8:
            case RBC_CMP_GT_I8:
            case RBC_CMP_GE_I8: {
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
                break;
            }

            case RBC_CMP_BRANCH: {
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
                break;
            }
            case RBC_CMP_I8_BRANCH: {
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
                break;
            }
            case RBC_SUPER_DATA: c11__abort("entered register superinstruction payload");

            case RBC_IS:
            case RBC_IS_NOT: {
                bool value = py_isidentical(&R[B], &R[C]);
                if(op == RBC_IS_NOT) value = !value;
                py_newbool(&R[A], value);
                break;
            }
            case RBC_CONTAINS:
            case RBC_NOT_CONTAINS:
                REG_SYNC_IP();
                if(!RegVM__slow_contains(&R[A], &R[B], &R[C], op == RBC_NOT_CONTAINS))
                    REG_FAIL();
                break;

            case RBC_NEG:
                REG_SYNC_IP();
                if(!pk_callmagic(__neg__, 1, &R[B])) REG_FAIL();
                R[A] = self->last_retval;
                break;
            case RBC_NOT: {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[B]);
                int value = pk_ceval_bool(&R[B]);
                if(value < 0) REG_FAIL();
                py_newbool(&R[A], !value);
                break;
            }
            case RBC_INVERT:
                if(R[B].type == tp_int) {
                    py_newint(&R[A], ~R[B]._i64);
                } else {
                    REG_SYNC_IP();
                    if(!pk_callmagic(__invert__, 1, &R[B])) REG_FAIL();
                    R[A] = self->last_retval;
                }
                break;

            case RBC_GET_ITER:
                REG_SYNC_IP();
                if(!py_iter(&R[B])) REG_FAIL();
                R[A] = self->last_retval;
                break;

            case RBC_FOR_ITER: {
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
                break;
            }

            case RBC_FOR_GENERATOR:
            case RBC_FOR_ARRAY2D:
            case RBC_FOR_LIST:
            case RBC_FOR_TUPLE:
            case RBC_FOR_DICT:
            case RBC_FOR_RANGE:
            case RBC_FOR_STR: {
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
                break;
            }

            case RBC_JUMP: REG_BRANCH_TO(here + RBC_SBX(inst)); break;
            case RBC_JUMP_IF_FALSE:
            case RBC_JUMP_IF_TRUE:
            case RBC_JUMP_IF_TRUE_OR_POP:
            case RBC_JUMP_IF_FALSE_OR_POP: {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[A]);
                int value = pk_ceval_bool(&R[A]);
                if(value < 0) REG_FAIL();
                bool take = op == RBC_JUMP_IF_FALSE || op == RBC_JUMP_IF_FALSE_OR_POP ? !value : value;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) REG_BRANCH_TO(here + RBC_SBX(inst));
                break;
            }
            case RBC_SHORTCUT_IF_FALSE_OR_POP: {
                REG_SYNC_IF_DYNAMIC_TRUTH(&R[A + 1]);
                int value = pk_ceval_bool(&R[A + 1]);
                if(value < 0) REG_FAIL();
                bool take = !value;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) {
                    py_newbool(&R[A], false);
                    REG_BRANCH_TO(here + RBC_SBX(inst));
                }
                break;
            }
            case RBC_JUMP_IF_NOT_MATCH: {
                REG_SYNC_IP();
                int equal = py_equal(&R[A], &R[A + 1]);
                if(equal < 0) REG_FAIL();
                bool take = !equal;
                if(profile_enabled) RegProfile__record_branch(rc, here, take);
                if(take) REG_BRANCH_TO(here + RBC_SBX(inst));
                break;
            }

            case RBC_CALL:
            case RBC_CALL_IC: {
                REG_SYNC_IP();
                RegRuntimeSite* site = &runtime_sites[here];
                bool dequickened = false;
                uintptr_t call_identity = reg_call_identity(&R[A]);
                if(profile_enabled) RegProfile__record_call(rc, here, call_identity);

                if(op == RBC_CALL_IC) {
                    FrameResult cached_result;
                    RegInlineCache* cache = reg_cache_at(rc, site);
                    if(reg_call_cache_apply(self, frame_limit, &R[A], B, C, cache, &cached_result)) {
                        if(profile_enabled) RegProfile__record_ic_hit(rc, here);
                        if(cached_result == RES_CALL) cached_result = VM__run_top_frame(self);
                        if(cached_result == RES_ERROR) {
                            self->stack.sp = frame_limit;
                            REG_FAIL();
                        }
                        c11__rtassert(cached_result == RES_RETURN);
                        self->stack.sp = frame_limit;
                        R[A] = self->last_retval;
                        site->misses = 0;
                        break;
                    }

                    if(profile_enabled) RegProfile__record_ic_miss(rc, here);
                    dequickened = reg_specialization_miss(rc, here, RBC_CALL);
                    if(!dequickened && cache != NULL) {
                        (void)reg_call_cache_fill(&R[A], B, C, cache);
                    }
                } else {
                    if(reg_observe(site, (uint32_t)(call_identity ^ (call_identity >> 32)))) {
                        RegInlineCache* cache = reg_cache_alloc(rc, site, REG_IC_KIND_CALL);
                        if(cache != NULL && reg_call_cache_fill(&R[A], B, C, cache)) {
                            reg_mark_quickened(rc, here, RBC_CALL_IC);
                        }
                    }
                }

                uint16_t window = (uint16_t)(2u + B + C * 2u);
                if(frame_limit + window > self->stack.end) {
                    py_exception(tp_RecursionError, "value stack overflow");
                    REG_FAIL();
                }
                self->stack.sp = frame_limit;
                memcpy(self->stack.sp, &R[A], (size_t)window * sizeof(py_TValue));
                self->stack.sp += window;

                FrameResult res = VM__vectorcall(self, B, C, true);
                if(res == RES_CALL) res = VM__run_top_frame(self);
                if(res == RES_ERROR) {
                    self->stack.sp = frame_limit;
                    REG_FAIL();
                }
                c11__rtassert(res == RES_RETURN);
                self->stack.sp = frame_limit;
                R[A] = self->last_retval;
                (void)dequickened;
                break;
            }

            case RBC_LIST_APPEND:
                py_list_append(&R[A], &R[B]);
                break;
            case RBC_DICT_ADD:
                REG_SYNC_IP();
                if(!py_dict_setitem(&R[A], &R[B], &R[C])) REG_FAIL();
                break;

            case RBC_UNPACK_SEQUENCE:
                if(!RegVM__slow_unpack_sequence(&R[B], &R[A], C)) REG_FAIL();
                break;
            case RBC_UNPACK_EX:
                if(!RegVM__slow_unpack_ex(&R[B], &R[A], C)) REG_FAIL();
                break;
            case RBC_FORMAT_STRING: {
                REG_SYNC_IP();
                py_Ref spec = (py_Ref)&consts[RBC_BX(inst)];
                if(!pk_format_object(self, &R[A], py_tosv(spec))) REG_FAIL();
                R[A] = self->last_retval;
                break;
            }

            case RBC_RETURN:
                self->last_retval = R[A];
                VM__pop_frame(self);
                return RES_RETURN;
            case RBC_RETURN_NONE:
                py_newnone(&self->last_retval);
                VM__pop_frame(self);
                return RES_RETURN;

            default: c11__unreachable();
        }
    }

#undef REG_SYNC_IF_DYNAMIC_TRUTH
#undef REG_BRANCH_TO
#undef REG_FAIL
#undef REG_SYNC_IP
    c11__abort("register bytecode fell off the end without RETURN");
}

FrameResult VM__run_top_frame(VM* self) {
    RegCode* code;
    if(RegVM__can_execute_now(self, self->top_frame, &code)) {
        (void)code;
        return VM__run_top_frame_reg(self);
    }
    return VM__run_top_frame_stack(self);
}
