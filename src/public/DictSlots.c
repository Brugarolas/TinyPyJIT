#include "tinypy/interpreter/vm.h"
#include "tinypy/interpreter/shape.h"
#include "tinypy/interpreter/typeinfo.h"

PK_INLINE py_Ref py_getreg(int i) { return pk_current_vm->reg + i; }

PK_INLINE void py_setreg(int i, py_Ref val) { pk_current_vm->reg[i] = *val; }

PK_INLINE py_Ref(py_retval)() { return &pk_current_vm->last_retval; }

/*
 * Instance namespaces come in two representations: a NameDict (types, modules
 * and other dict-backed objects) or a ShapeStorage (instances of Python
 * classes, see shape.h). Every accessor below handles both.
 */

PK_INLINE py_Ref py_getdict(py_Ref self, py_Name name) {
    assert(self && self->is_ptr);
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) return ShapeStorage__try_get(ShapeObject__storage(obj), name);
    return NameDict__try_get(PyObject__dict(obj), name);
}

PK_INLINE void py_setdict(py_Ref self, py_Name name, py_Ref val) {
    assert(self && self->is_ptr);
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) {
        ShapeStorage__set(pk_type_shape_arena(obj->type), ShapeObject__storage_ref(obj), name, val);
        return;
    }
    // writing to a type's dict may change what `__new__`/`__init__` resolve to,
    // for this type and for every subclass of it
    if(self->type == tp_type) pk_current_vm->type_version++;
    NameDict__set(PyObject__dict(obj), name, val);
}

bool py_deldict(py_Ref self, py_Name name) {
    assert(self && self->is_ptr);
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) return ShapeStorage__del(ShapeObject__storage(obj), name);
    if(self->type == tp_type) pk_current_vm->type_version++;
    return NameDict__del(PyObject__dict(obj), name);
}

py_ItemRef py_emplacedict(py_Ref self, py_Name name) {
    py_setdict(self, name, py_NIL());
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) {
        /* try_get hides nil slots, so address the freshly created slot directly. */
        ShapeStorage* storage = ShapeObject__storage(obj);
        int slot = Shape__lookup(ShapeStorage__shape(storage), name);
        return slot < 0 ? NULL : ShapeStorage__slot(storage, (uint16_t)slot);
    }
    return py_getdict(self, name);
}

bool py_applydict(py_Ref self, bool (*f)(py_Name, py_Ref, void*), void* ctx) {
    assert(self && self->is_ptr);
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) return ShapeStorage__apply(ShapeObject__storage(obj), f, ctx);
    NameDict* dict = PyObject__dict(obj);
    for(int i = 0; i < dict->capacity; i++) {
        NameDict_KV* kv = &dict->items[i];
        if(kv->key == NULL) continue;
        bool ok = f(kv->key, &kv->value, ctx);
        if(!ok) return false;
    }
    return true;
}

void py_cleardict(py_Ref self) {
    assert(self && self->is_ptr);
    PyObject* obj = self->_obj;
    if(obj->slots == PK_OBJ_SHAPED_SLOTS) {
        ShapeStorage__clear(ShapeObject__storage(obj));
        return;
    }
    if(self->type == tp_type) pk_current_vm->type_version++;
    NameDict* dict = PyObject__dict(obj);
    NameDict__clear(dict);
}

py_Ref py_getslot(py_Ref self, int i) {
    assert(self && self->is_ptr);
    assert(i >= 0 && i < self->_obj->slots);
    return PyObject__slots(self->_obj) + i;
}

void py_setslot(py_Ref self, int i, py_Ref val) {
    assert(self && self->is_ptr);
    assert(i >= 0 && i < self->_obj->slots);
    PyObject__slots(self->_obj)[i] = *val;
}

py_Ref py_getbuiltin(py_Name name) { return py_getdict(pk_current_vm->builtins, name); }

py_Ref py_getglobal(py_Name name) { return py_getdict(pk_current_vm->main, name); }

void py_setglobal(py_Name name, py_Ref val) { py_setdict(pk_current_vm->main, name, val); }
