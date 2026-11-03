#include "pocketpy/interpreter/regvm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the implementation so internal adaptive helpers are testable. */
#include "../src/interpreter/regvm.c"

static void dummy_native(void) {}

py_Name __new__;

static py_TypeInfo g_typeinfo[64];
static PyObject g_globals_obj;
static PyObject g_builtins_obj;
static PyObject g_instance_obj;
static NameDict g_globals_dict;
static NameDict g_builtins_dict;
static NameDict g_instance_dict;
static NameDict_KV g_globals_items[4];
static NameDict_KV g_builtins_items[4];
static NameDict_KV g_instance_items[4];
static py_TValue g_class_slot;
static py_Name g_class_name;
static py_TValue g_retval;

static py_Name fake_name(uintptr_t value) { return (py_Name)value; }

static void init_fake_dict(NameDict* dict, NameDict_KV* items, int capacity, uint64_t version) {
    memset(dict, 0, sizeof(*dict));
    memset(items, 0, (size_t)capacity * sizeof(*items));
    dict->capacity = capacity;
    dict->mask = (uintptr_t)capacity - 1u;
    dict->items = items;
    dict->version = version;
}

NameDict* PyObject__dict(PyObject* object) {
    if(object == &g_globals_obj) return &g_globals_dict;
    if(object == &g_builtins_obj) return &g_builtins_dict;
    if(object == &g_instance_obj) return &g_instance_dict;
    return NULL;
}

py_TValue* PyObject__slots(PyObject* object) { (void)object; return NULL; }
void* PyObject__userdata(PyObject* object) { (void)object; return NULL; }

int NameDict__find_index(NameDict* dict, py_Name name) {
    for(int i = 0; i < dict->capacity; i++) {
        if(dict->items[i].key == name) return i;
    }
    return -1;
}

py_Ref py_getdict(py_Ref self, py_Name name) {
    NameDict* dict = reg_namedict(self);
    int index = dict ? NameDict__find_index(dict, name) : -1;
    return index >= 0 ? &dict->items[index].value : NULL;
}

py_TypeInfo* pk_typeinfo(py_Type type) {
    assert(type >= 0 && type < (py_Type)(sizeof(g_typeinfo) / sizeof(g_typeinfo[0])));
    return &g_typeinfo[type];
}

py_ItemRef pk_tpfindname(py_TypeInfo* ti, py_Name name) {
    if(ti == &g_typeinfo[tp_list] && name == g_class_name && g_class_slot.type != tp_nil) {
        return &g_class_slot;
    }
    return NULL;
}

py_ItemRef pk_tpfindmagic_cached(py_TypeInfo* ti, py_Name name) { return pk_tpfindname(ti, name); }

py_Ref py_getslot(py_Ref self, int index) {
    (void)index;
    return self;
}

void py_newboundmethod(py_Ref out, py_Ref self, py_Ref function) {
    out->type = tp_boundmethod;
    out->is_ptr = false;
    out->_ptr = function;
    out->extra = (int)self->type;
}

bool py_isnil(py_Ref value) { return value->type == tp_nil; }
void py_newnil(py_Ref out) { memset(out, 0, sizeof(*out)); }

py_Ref py_retval(void) { return &g_retval; }

bool py_call(py_Ref function, int argc, py_Ref argv) {
    (void)function;
    (void)argc;
    (void)argv;
    return false;
}

static void init_single_site(RegCode* code, RegOpcode op) {
    RegCode__ctor(code);
    c11_vector__push(RegBytecode, &code->code, RBC_ABC(op, 0, 1, 2));
    c11_vector__push(int32_t, &code->source_ip, 0);
    RegRuntimeSite site;
    memset(&site, 0, sizeof(site));
    site.cache_index = REG_IC_NONE;
    c11_vector__push(RegRuntimeSite, &code->runtime_sites, site);
}

static void test_int_quickening_and_dequickening(void) {
    RegCode code;
    init_single_site(&code, RBC_ADD);

    reg_maybe_quicken_binary(&code, 0, RBC_ADD, tp_int, tp_int);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD);
    reg_maybe_quicken_binary(&code, 0, RBC_ADD, tp_int, tp_int);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD);
    reg_maybe_quicken_binary(&code, 0, RBC_ADD, tp_int, tp_int);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD_INT);
    assert(code.quickened_count == 1);

    assert(!reg_specialization_miss(&code, 0, RBC_ADD));
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD_INT);
    assert(reg_specialization_miss(&code, 0, RBC_ADD));
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD);
    assert(code.quickened_count == 0);

    RegCode__dtor(&code);
}


static void test_numeric_specialization(void) {
    RegCode code;
    init_single_site(&code, RBC_MUL);
    for(int i = 0; i < 3; i++) reg_maybe_quicken_binary(&code, 0, RBC_MUL, tp_float, tp_int);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_MUL_NUM);
    RegCode__dtor(&code);
}

static void test_runtime_reset(void) {
    RegCode code;
    init_single_site(&code, RBC_ADD);
    for(int i = 0; i < 3; i++) reg_maybe_quicken_binary(&code, 0, RBC_ADD, tp_int, tp_int);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD_INT);
    assert(code.quickened_count == 1);

    RegInlineCache cache;
    memset(&cache, 0, sizeof(cache));
    cache.kind = REG_IC_KIND_CALL;
    c11_vector__push(RegInlineCache, &code.inline_caches, cache);
    reg_runtime_site(&code, 0)->cache_index = 0;

    RegCode__reset_runtime(&code);
    assert(RBC_OP(c11__getitem(RegBytecode, &code.code, 0)) == RBC_ADD);
    assert(code.quickened_count == 0);
    assert(code.inline_caches.length == 0);
    assert(code.runtime_sites.length == code.code.length);
    assert(reg_runtime_site(&code, 0)->cache_index == REG_IC_NONE);

    RegCode__dtor(&code);
}

static void test_iterator_selection(void) {
    assert(reg_iterator_specialization(tp_range_iterator) == RBC_FOR_RANGE);
    assert(reg_iterator_specialization(tp_list_iterator) == RBC_FOR_LIST);
    assert(reg_iterator_specialization(tp_tuple_iterator) == RBC_FOR_TUPLE);
    assert(reg_iterator_specialization(tp_str_iterator) == RBC_FOR_STR);
    assert(reg_iterator_specialization(tp_int) == RBC_NOP);
}


static void test_global_inline_cache(void) {
    VM vm;
    py_Frame frame;
    py_TValue globals_value;
    py_TValue builtins_value;
    py_TValue out;
    RegInlineCache cache;
    py_Name name = fake_name(0x1010);

    memset(&vm, 0, sizeof(vm));
    memset(&frame, 0, sizeof(frame));
    memset(&globals_value, 0, sizeof(globals_value));
    memset(&builtins_value, 0, sizeof(builtins_value));
    init_fake_dict(&g_globals_dict, g_globals_items, 4, 11);
    init_fake_dict(&g_builtins_dict, g_builtins_items, 4, 23);
    g_globals_obj.slots = -1;
    g_builtins_obj.slots = -1;
    globals_value.type = tp_module;
    globals_value.is_ptr = true;
    globals_value._obj = &g_globals_obj;
    builtins_value.type = tp_module;
    builtins_value.is_ptr = true;
    builtins_value._obj = &g_builtins_obj;
    frame.globals = &globals_value;
    vm.builtins = &builtins_value;

    g_globals_items[1].key = name;
    g_globals_items[1].value.type = tp_int;
    g_globals_items[1].value._i64 = 41;
    assert(reg_global_cache_fill(&vm, &frame, name, &cache));
    assert(cache.variant == REG_GLOBAL_CACHE_GLOBAL);
    assert(reg_global_cache_apply(&vm, &frame, name, &cache, &out));
    assert(out.type == tp_int && out._i64 == 41);

    g_globals_dict.version++;
    assert(!reg_global_cache_apply(&vm, &frame, name, &cache, &out));

    g_globals_items[1].key = NULL;
    g_globals_dict.version++;
    g_builtins_items[2].key = name;
    g_builtins_items[2].value.type = tp_int;
    g_builtins_items[2].value._i64 = 99;
    assert(reg_global_cache_fill(&vm, &frame, name, &cache));
    assert(cache.variant == REG_GLOBAL_CACHE_BUILTIN);
    assert(reg_global_cache_apply(&vm, &frame, name, &cache, &out));
    assert(out._i64 == 99);

    /* A new global must invalidate a builtins cache before it can shadow it. */
    g_globals_items[0].key = name;
    g_globals_items[0].value.type = tp_int;
    g_globals_items[0].value._i64 = 7;
    g_globals_dict.version++;
    assert(!reg_global_cache_apply(&vm, &frame, name, &cache, &out));
}

static void test_attr_and_method_inline_cache(void) {
    VM vm;
    py_TValue object;
    py_TValue out;
    py_TValue callable;
    py_TValue self_value;
    RegInlineCache cache;
    py_Name name = fake_name(0x2020);

    memset(&vm, 0, sizeof(vm));
    memset(g_typeinfo, 0, sizeof(g_typeinfo));
    memset(&object, 0, sizeof(object));
    memset(&g_class_slot, 0, sizeof(g_class_slot));
    init_fake_dict(&g_instance_dict, g_instance_items, 4, 5);
    g_instance_obj.slots = -1;
    object.type = tp_list;
    object.is_ptr = true;
    object._obj = &g_instance_obj;
    g_class_name = name;
    g_class_slot.type = tp_function;
    g_typeinfo[tp_list].index = tp_list;
    g_typeinfo[tp_list].self.type = tp_type;
    vm.type_version = 17;

    assert(reg_attr_cache_fill(&vm, &object, name, &cache));
    assert(cache.variant == REG_ATTR_CACHE_FUNCTION);

    /* Instance attributes shadow non-data descriptors such as functions. */
    g_instance_items[1].key = name;
    g_instance_items[1].value.type = tp_int;
    g_instance_items[1].value._i64 = 123;
    assert(reg_attr_cache_apply(&vm, &object, name, &cache, &out) == 1);
    assert(out.type == tp_int && out._i64 == 123);

    g_instance_items[1].key = NULL;
    assert(reg_attr_cache_apply(&vm, &object, name, &cache, &out) == 1);
    assert(out.type == tp_boundmethod);

    assert(reg_method_cache_fill(&vm, &object, name, &cache));
    assert(cache.variant == REG_METHOD_CACHE_BIND_SELF);
    assert(reg_method_cache_apply(&vm, &object, &cache, &callable, &self_value));
    assert(callable.type == tp_function);
    assert(self_value._obj == object._obj);

    vm.type_version++;
    assert(!reg_method_cache_apply(&vm, &object, &cache, &callable, &self_value));
}

static void test_native_call_cache_shape(void) {
    py_TValue callable;
    memset(&callable, 0, sizeof(callable));
    callable.type = tp_nativefunc;
    callable._cfunc = dummy_native;

    RegInlineCache cache;
    assert(reg_call_cache_fill(&callable, 2, 0, &cache));
    assert(cache.kind == REG_IC_KIND_CALL);
    assert(cache.variant == REG_CALL_CACHE_NATIVE);
    assert(cache.argc == 2);
    assert(cache.guard0 == (uintptr_t)dummy_native);
    assert(!reg_call_cache_fill(&callable, 2, 1, &cache));
}

int main(void) {
    test_int_quickening_and_dequickening();
    test_numeric_specialization();
    test_runtime_reset();
    test_iterator_selection();
    test_native_call_cache_shape();
    test_global_inline_cache();
    test_attr_and_method_inline_cache();
    puts("adaptive smoke: ok");
    return 0;
}
