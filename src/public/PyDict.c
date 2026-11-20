#include "tinypy/tinypy.h"

#include "tinypy/common/utils.h"
#include "tinypy/common/sstream.h"
#include "tinypy/interpreter/types.h"
#include "tinypy/interpreter/vm.h"
#include "tinypy/interpreter/bindings.h"

typedef struct {
    Dict* dict;  // weakref for slot 0
    Dict dict_backup;
    DictEntry* curr;
    DictEntry* end;
    int mode;  // 0: keys, 1: values, 2: items
} DictIterator;

/*
 * The index table has a power-of-two capacity and stores raw hashes (an int's
 * hash is its value, as in CPython). The slot folds the bits above the table
 * size into the index: keys below the capacity keep their natural order, so
 * consecutive ints fill consecutive slots and entries (no cache misses while
 * building or probing such dicts), while strided or pointer-like hashes still
 * spread through their high bits. No 64-bit division is needed.
 */
#define Dict__step(x) (((x) + 1) & mask)
#define Dict__INITIAL_CAPACITY 16

static PK_INLINE unsigned Dict__log2(uint32_t pow2) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(pow2);
#else
    unsigned n = 0;
    while((pow2 >>= 1) != 0) n++;
    return n;
#endif
}

static PK_INLINE uint32_t Dict__slot(uint64_t hash, uint32_t mask) {
    hash ^= hash >> 32;
    hash ^= hash >> Dict__log2(mask + 1);
    return (uint32_t)hash & mask;
}

static uint32_t Dict__next_cap(uint32_t cap) {
    if(cap >= UINT32_C(0x80000000)) c11__abort("dict is too large");
    return cap * 2;
}

// Allocate an empty index array for the given capacity. `entries` is not touched.
static void Dict__alloc_indices(Dict* self, uint32_t capacity) {
    self->capacity = capacity;

    size_t indices_size;
    if(self->capacity < UINT16_MAX) {
        self->index_is_short = true;
        indices_size = self->capacity * sizeof(uint16_t);
        self->null_index_value = UINT16_MAX;
    } else {
        self->index_is_short = false;
        indices_size = self->capacity * sizeof(uint32_t);
        self->null_index_value = UINT32_MAX;
    }

    self->indices = py_malloc(indices_size);
    memset(self->indices, -1, indices_size);
}

static void Dict__ctor(Dict* self, uint32_t capacity, int entries_capacity) {
    self->length = 0;
    Dict__alloc_indices(self, capacity);
    c11_vector__ctor(&self->entries, sizeof(DictEntry));
    c11_vector__reserve(&self->entries, entries_capacity);
}

static void Dict__dtor(Dict* self) {
    self->length = 0;
    self->capacity = 0;
    py_free(self->indices);
    c11_vector__dtor(&self->entries);
}

static uint32_t Dict__get_index(Dict* self, uint32_t index) {
    if(self->index_is_short) {
        uint16_t* indices = self->indices;
        return indices[index];
    } else {
        uint32_t* indices = self->indices;
        return indices[index];
    }
}

/*
 * Deleted index slots hold a tombstone (one below the empty marker), as in
 * CPython: deletion is O(1) and never has to shift a probe cluster. Lookups skip
 * tombstones, insertions reuse the first one they pass, and every rebuild
 * (growth or compaction) drops them. Entry indices always stay below it.
 */
#define Dict__dummy_index(self) ((self)->null_index_value - 1u)
static void Dict__set_index(Dict* self, uint32_t index, uint32_t value) {
    if(self->index_is_short) {
        uint16_t* indices = self->indices;
        indices[index] = (uint16_t)value;
    } else {
        uint32_t* indices = self->indices;
        indices[index] = value;
    }
}

// Dict__probe won't raise exception for string keys
static bool Dict__probe(Dict* self,
                        py_TValue* key,
                        uint64_t* p_hash,
                        uint32_t* p_idx,
                        DictEntry** p_entry) {
    if(key->type == tp_int) {
        // fast path: `int.__hash__` is the identity, so skip the generic dispatch
        *p_hash = (uint64_t)key->_i64;
    } else if(py_isstr(key)) {
        *p_hash = c11_sv__hash(py_tosv(key));
    } else {
        py_i64 h_user;
        if(!py_hash(key, &h_user)) return false;
        *p_hash = (uint64_t)h_user;
    }
    uint32_t mask = self->capacity - 1;
    uint32_t idx = Dict__slot(*p_hash, mask);
    const uint32_t dummy = Dict__dummy_index(self);
    uint32_t first_dummy = UINT32_MAX;
    while(true) {
        uint32_t idx2 = Dict__get_index(self, idx);
        if(idx2 == self->null_index_value) break;
        if(idx2 == dummy) {
            if(first_dummy == UINT32_MAX) first_dummy = idx;
            idx = Dict__step(idx);
            continue;
        }
        DictEntry* entry = c11__at(DictEntry, &self->entries, idx2);
        if(entry->hash == (*p_hash)) {
            if(key->type == tp_int && entry->key.type == tp_int) {
                /* int == int never dispatches; distinct ints may share a hash */
                if(entry->key._i64 == key->_i64) {
                    *p_idx = idx;
                    *p_entry = entry;
                    return true;
                }
            } else if(py_isstr(&entry->key) && py_isstr(key)) {
                c11_sv lhs = py_tosv(&entry->key);
                c11_sv rhs = py_tosv(key);
                if(c11__sveq(lhs, rhs)) {
                    *p_idx = idx;
                    *p_entry = entry;
                    return true;
                }
            } else {
                int res = py_equal(&entry->key, key);
                if(res == 1) {
                    *p_idx = idx;
                    *p_entry = entry;
                    return true;
                }
                if(res == -1) return false;  // error
            }
        }
        // try next index
        idx = Dict__step(idx);
    }
    // not found: an insertion reuses the first tombstone on the probe path
    *p_idx = first_dummy != UINT32_MAX ? first_dummy : idx;
    *p_entry = NULL;
    return true;
}

static bool Dict__try_get(Dict* self, py_TValue* key, DictEntry** out) {
    uint64_t hash;
    uint32_t idx;
    return Dict__probe(self, key, &hash, &idx, out);
}

static void Dict__clear(Dict* self) {
    size_t indices_size = self->index_is_short ? self->capacity * sizeof(uint16_t)
                                               : self->capacity * sizeof(uint32_t);
    memset(self->indices, -1, indices_size);
    c11_vector__clear(&self->entries);
    self->length = 0;
}

/* Squeeze deleted entries out (keeping order) and rebuild the index at `new_capacity`. */
static void Dict__rebuild(Dict* self, uint32_t new_capacity) {
    uint32_t mask = new_capacity - 1;

    // squeeze out deleted entries in place; the old indices are discarded anyway
    if(self->length != self->entries.length) {
        int n = 0;
        for(int i = 0; i < self->entries.length; i++) {
            DictEntry* entry = c11__at(DictEntry, &self->entries, i);
            if(py_isnil(&entry->key)) continue;  // skip deleted
            if(i != n) *c11__at(DictEntry, &self->entries, n) = *entry;
            n++;
        }
        assert(n == self->length);
        self->entries.length = n;
    }

    // `entries` is already dense, so it keeps its buffer and its layout;
    // only the index array has to be rebuilt for the new capacity
    py_free(self->indices);
    Dict__alloc_indices(self, new_capacity);
    for(int i = 0; i < self->entries.length; i++) {
        DictEntry* entry = c11__at(DictEntry, &self->entries, i);
        uint32_t idx = Dict__slot(entry->hash, mask);
        while(Dict__get_index(self, idx) != self->null_index_value) {
            idx = Dict__step(idx);  // try next index
        }
        Dict__set_index(self, idx, i);
    }
}
static bool Dict__set(Dict* self, py_TValue* key, py_TValue* val) {
    uint64_t hash;
    uint32_t idx;
    DictEntry* entry;
    if(!Dict__probe(self, key, &hash, &idx, &entry)) return false;
    if(entry) {
        // update existing entry
        entry->val = *val;
        return true;
    }
    // insert new entry
    DictEntry* new_entry = c11_vector__emplace(&self->entries);
    new_entry->hash = hash;
    new_entry->key = *key;
    new_entry->val = *val;
    Dict__set_index(self, idx, self->entries.length - 1);
    self->length++;
    /*
     * Occupied index slots are at most entries.length (live entries plus the
     * tombstones of deleted ones still in `entries`), so bounding it keeps an
     * empty slot on every probe path. Grow only if live entries need it.
     */
    const float max_load = self->index_is_short ? 0.4f : 0.5f;
    if((float)self->entries.length / self->capacity > max_load) {
        bool grow = (float)self->length / self->capacity > max_load * 0.5f;
        Dict__rebuild(self, grow ? Dict__next_cap(self->capacity) : self->capacity);
    }
    return true;
}

/// Delete an entry from the dict.
/// -1: error, 0: not found, 1: found and deleted
static int Dict__pop(Dict* self, py_Ref key) {
    // Dict__log_index(self, "before pop");
    uint64_t hash;
    uint32_t idx;
    DictEntry* entry;
    if(!Dict__probe(self, key, &hash, &idx, &entry)) return -1;
    if(!entry) return 0;  // not found

    // found the entry, delete and return it
    py_assign(py_retval(), &entry->val);
    Dict__set_index(self, idx, Dict__dummy_index(self));
    py_newnil(&entry->key);
    py_newnil(&entry->val);
    self->length--;

    // compact entries (and drop tombstones) when most of them are deleted
    if(self->entries.length > 16 && (self->length < self->entries.length >> 1)) {
        Dict__rebuild(self, self->capacity);
    }    // Dict__log_index(self, "after pop");
    return 1;
}

static void DictIterator__ctor(DictIterator* self, Dict* dict, int mode) {
    assert(mode >= 0 && mode <= 2);
    self->dict = dict;
    self->dict_backup = *dict;  // backup the dict
    self->curr = dict->entries.data;
    self->end = self->curr + dict->entries.length;
    self->mode = mode;
}

static DictEntry* DictIterator__next(DictIterator* self) {
    DictEntry* retval;
    do {
        if(self->curr == self->end) return NULL;
        retval = self->curr++;
    } while(py_isnil(&retval->key));
    return retval;
}

static bool DictIterator__modified(DictIterator* self) {
    return memcmp(self->dict, &self->dict_backup, sizeof(Dict)) != 0;
}

///////////////////////////////
static bool dict__new__(int argc, py_Ref argv) {
    py_Type cls = py_totype(argv);
    int slots = cls == tp_dict ? 0 : -1;
    Dict* ud = py_newobject(py_retval(), cls, slots, sizeof(Dict));
    Dict__ctor(ud, Dict__INITIAL_CAPACITY, 4);
    return true;
}

void py_newdict(py_OutRef out) {
    Dict* ud = py_newobject(out, tp_dict, 0, sizeof(Dict));
    Dict__ctor(ud, Dict__INITIAL_CAPACITY, 4);
}

static bool dict__init__(int argc, py_Ref argv) {
    if(argc > 2) return TypeError("dict.__init__() takes at most 2 arguments (%d given)", argc);
    if(argc == 1) {
        py_newnone(py_retval());
        return true;
    }
    assert(argc == 2);
    py_TValue* p;
    int length = pk_arrayview(py_arg(1), &p);
    if(length == -1) { return TypeError("dict.__init__() expects a list or tuple"); }

    Dict* self = py_touserdata(argv);
    for(int i = 0; i < length; i++) {
        py_Ref tuple = &p[i];
        if(!py_istuple(tuple) || py_tuple_len(tuple) != 2) {
            return ValueError("dict.__init__() argument must be a list of tuple-2");
        }
        py_Ref key = py_tuple_getitem(tuple, 0);
        py_Ref val = py_tuple_getitem(tuple, 1);
        if(!Dict__set(self, key, val)) return false;
    }
    py_newnone(py_retval());
    return true;
}

static bool dict__getitem__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    Dict* self = py_touserdata(argv);
    DictEntry* entry;
    if(!Dict__try_get(self, py_arg(1), &entry)) return false;
    if(entry) {
        *py_retval() = entry->val;
        return true;
    }
    // try __missing__
    py_Ref missing = py_tpfindmagic(argv->type, __missing__);
    if(missing) return py_call(missing, argc, argv);
    return KeyError(py_arg(1));
}

static bool dict__setitem__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(3);
    Dict* self = py_touserdata(argv);
    bool ok = Dict__set(self, py_arg(1), py_arg(2));
    if(ok) py_newnone(py_retval());
    return ok;
}

static bool dict__delitem__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    Dict* self = py_touserdata(argv);
    int res = Dict__pop(self, py_arg(1));
    if(res == 1) {
        py_newnone(py_retval());
        return true;
    }
    if(res == 0) return KeyError(py_arg(1));
    return false;
}

static bool dict__contains__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    Dict* self = py_touserdata(argv);
    DictEntry* entry;
    if(!Dict__try_get(self, py_arg(1), &entry)) return false;
    py_newbool(py_retval(), entry != NULL);
    return true;
}

static bool dict__len__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    py_newint(py_retval(), self->length);
    return true;
}

static bool dict__repr__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    c11_sbuf__write_char(&buf, '{');
    bool is_first = true;
    for(int i = 0; i < self->entries.length; i++) {
        DictEntry* entry = c11__at(DictEntry, &self->entries, i);
        if(py_isnil(&entry->key)) continue;
        if(!is_first) c11_sbuf__write_cstr(&buf, ", ");
        if(!py_repr(&entry->key)) return false;
        c11_sbuf__write_sv(&buf, py_tosv(py_retval()));
        c11_sbuf__write_cstr(&buf, ": ");
        if(!py_repr(&entry->val)) return false;
        c11_sbuf__write_sv(&buf, py_tosv(py_retval()));
        is_first = false;
    }
    c11_sbuf__write_char(&buf, '}');
    c11_sbuf__py_submit(&buf, py_retval());
    return true;
}

static bool dict__eq__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    Dict* self = py_touserdata(py_arg(0));
    if(!py_isdict(py_arg(1))) {
        py_newnotimplemented(py_retval());
        return true;
    }
    Dict* other = py_touserdata(py_arg(1));
    if(self->length != other->length) {
        py_newbool(py_retval(), false);
        return true;
    }
    DictIterator iter;
    DictIterator__ctor(&iter, self, 2);
    // for each self key
    while(1) {
        DictEntry* entry = DictIterator__next(&iter);
        if(!entry) break;
        DictEntry* other_entry;
        if(!Dict__try_get(other, &entry->key, &other_entry)) return false;
        if(!other_entry) {
            py_newbool(py_retval(), false);
            return true;
        }
        if(entry->hash != other_entry->hash) {
            py_newbool(py_retval(), false);
            return true;
        }
        int res = py_equal(&entry->val, &other_entry->val);
        if(res == 0) {
            py_newbool(py_retval(), false);
            return true;
        }
        if(res == -1) return false;  // error
    }
    py_newbool(py_retval(), true);
    return true;
}

static bool dict__ne__(int argc, py_Ref argv) {
    if(!dict__eq__(argc, argv)) return false;
    if(py_isbool(py_retval())) {
        bool res = py_tobool(py_retval());
        py_newbool(py_retval(), !res);
    }
    return true;
}

static bool dict_clear(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    Dict__clear(self);
    py_newnone(py_retval());
    return true;
}

static bool dict_copy(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    Dict* new_dict = py_newobject(py_retval(), tp_dict, 0, sizeof(Dict));
    new_dict->length = self->length;
    new_dict->capacity = self->capacity;
    new_dict->null_index_value = self->null_index_value;
    new_dict->index_is_short = self->index_is_short;
    // copy entries
    new_dict->entries = c11_vector__copy(&self->entries);
    // copy indices
    size_t indices_size = self->index_is_short ? self->capacity * sizeof(uint16_t)
                                               : self->capacity * sizeof(uint32_t);
    new_dict->indices = py_malloc(indices_size);
    memcpy(new_dict->indices, self->indices, indices_size);
    return true;
}

static bool dict_update(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(1, tp_dict);
    Dict* self = py_touserdata(argv);
    Dict* other = py_touserdata(py_arg(1));
    for(int i = 0; i < other->entries.length; i++) {
        DictEntry* entry = c11__at(DictEntry, &other->entries, i);
        if(py_isnil(&entry->key)) continue;
        if(!Dict__set(self, &entry->key, &entry->val)) return false;
    }
    py_newnone(py_retval());
    return true;
}

static bool dict_get(int argc, py_Ref argv) {
    Dict* self = py_touserdata(argv);
    if(argc > 3) return TypeError("get() takes at most 3 arguments (%d given)", argc);
    py_Ref default_val = argc == 3 ? py_arg(2) : py_None();
    DictEntry* entry;
    if(!Dict__try_get(self, py_arg(1), &entry)) return false;
    *py_retval() = entry ? entry->val : *default_val;
    return true;
}

static bool dict_pop(int argc, py_Ref argv) {
    Dict* self = py_touserdata(argv);
    if(argc < 2 || argc > 3) return TypeError("pop() takes 1 or 2 arguments (%d given)", argc - 1);
    py_Ref default_val = argc == 3 ? py_arg(2) : py_None();
    int res = Dict__pop(self, py_arg(1));
    if(res == -1) return false;
    if(res == 0) py_assign(py_retval(), default_val);
    return true;
}

static bool dict_popitem(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    for(int i = self->entries.length - 1; i >= 0; i--) {
        DictEntry* entry = c11__at(DictEntry, &self->entries, i);
        if(py_isnil(&entry->key)) continue;
        py_Ref p = py_newtuple(py_pushtmp(), 2);
        p[0] = entry->key;
        p[1] = entry->val;
        int res = Dict__pop(self, &p[0]);
        c11__rtassert(res == 1);
        py_assign(py_retval(), py_peek(-1));
        py_pop();
        return true;
    }
    return KeyError(py_None());
}

static bool dict_keys(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    DictIterator* ud = py_newobject(py_retval(), tp_dict_iterator, 1, sizeof(DictIterator));
    DictIterator__ctor(ud, self, 0);
    py_setslot(py_retval(), 0, argv);  // keep a reference to the dict
    return true;
}

static bool dict_values(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    DictIterator* ud = py_newobject(py_retval(), tp_dict_iterator, 1, sizeof(DictIterator));
    DictIterator__ctor(ud, self, 1);
    py_setslot(py_retval(), 0, argv);  // keep a reference to the dict
    return true;
}

static bool dict_items(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    Dict* self = py_touserdata(argv);
    DictIterator* ud = py_newobject(py_retval(), tp_dict_iterator, 1, sizeof(DictIterator));
    DictIterator__ctor(ud, self, 2);
    py_setslot(py_retval(), 0, argv);  // keep a reference to the dict
    return true;
}

py_Type pk_dict__register() {
    py_Type type = pk_newtype("dict", tp_object, NULL, (void (*)(void*))Dict__dtor, false, false);

    py_bindmagic(type, __new__, dict__new__);
    py_bindmagic(type, __init__, dict__init__);
    py_bindmagic(type, __getitem__, dict__getitem__);
    py_bindmagic(type, __setitem__, dict__setitem__);
    py_bindmagic(type, __delitem__, dict__delitem__);
    py_bindmagic(type, __contains__, dict__contains__);
    py_bindmagic(type, __len__, dict__len__);
    py_bindmagic(type, __repr__, dict__repr__);
    py_bindmagic(type, __eq__, dict__eq__);
    py_bindmagic(type, __ne__, dict__ne__);
    py_bindmagic(type, __iter__, dict_keys);

    py_bindmethod(type, "clear", dict_clear);
    py_bindmethod(type, "copy", dict_copy);
    py_bindmethod(type, "update", dict_update);
    py_bindmethod(type, "get", dict_get);
    py_bindmethod(type, "pop", dict_pop);
    py_bindmethod(type, "popitem", dict_popitem);
    py_bindmethod(type, "keys", dict_keys);
    py_bindmethod(type, "values", dict_values);
    py_bindmethod(type, "items", dict_items);

    py_setdict(py_tpobject(type), __hash__, py_None());
    return type;
}

//////////////////////////
PK_DEFINE_NEXT_WRAPPER(dict_items)

int dict_items__iternext(py_Ref self) {
    DictIterator* iter = py_touserdata(self);
    if(DictIterator__modified(iter)) {
        RuntimeError("dictionary modified during iteration");
        return -1;
    }
    DictEntry* entry = (DictIterator__next(iter));
    if(entry) {
        switch(iter->mode) {
            case 0:  // keys
                py_assign(py_retval(), &entry->key);
                return 1;
            case 1:  // values
                py_assign(py_retval(), &entry->val);
                return 1;
            case 2:  // items
            {
                py_Ref p = py_newtuple(py_retval(), 2);
                p[0] = entry->key;
                p[1] = entry->val;
                return 1;
            }
            default: c11__unreachable();
        }
    }
    py_newnil(py_retval());
    return 0;
}

int dict_items__iternext2(py_Ref self, py_Ref out) {
    DictIterator* iter = py_touserdata(self);
    if(iter->mode != 2) return -2;
    if(DictIterator__modified(iter)) {
        RuntimeError("dictionary modified during iteration");
        return -1;
    }
    DictEntry* entry = DictIterator__next(iter);
    if(entry == NULL) return 0;
    out[0] = entry->key;
    out[1] = entry->val;
    return 1;
}

bool dict_items__len__(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    DictIterator* iter = py_touserdata(py_arg(0));
    py_newint(py_retval(), iter->dict->length);
    return true;
}

py_Type pk_dict_items__register() {
    py_Type type = pk_newtype("dict_iterator", tp_object, NULL, NULL, false, true);
    py_bindmagic(type, __iter__, pk_wrapper__self);
    py_bindmagic(type, __next__, dict_items__next__);
    py_bindmagic(type, __len__, dict_items__len__);
    return type;
}

//////////////////////////

int py_dict_getitem(py_Ref self, py_Ref key) {
    assert(py_isdict(self));
    Dict* ud = py_touserdata(self);
    DictEntry* entry;
    if(!Dict__try_get(ud, key, &entry)) return -1;
    if(entry) {
        py_assign(py_retval(), &entry->val);
        return 1;
    }
    return 0;
}

bool py_dict_setitem(py_Ref self, py_Ref key, py_Ref val) {
    assert(py_isdict(self));
    Dict* ud = py_touserdata(self);
    return Dict__set(ud, key, val);
}

int py_dict_delitem(py_Ref self, py_Ref key) {
    assert(py_isdict(self));
    Dict* ud = py_touserdata(self);
    return Dict__pop(ud, key);
}

int py_dict_getitem_by_str(py_Ref self, const char* key) {
    py_Ref tmp = py_pushtmp();
    py_newstr(tmp, key);
    int res = py_dict_getitem(self, tmp);
    py_pop();
    return res;
}

bool py_dict_setitem_by_str(py_Ref self, const char* key, py_Ref val) {
    py_Ref tmp = py_pushtmp();
    py_newstr(tmp, key);
    bool res = py_dict_setitem(self, tmp, val);
    py_pop();
    return res;
}

int py_dict_delitem_by_str(py_Ref self, const char* key) {
    py_Ref tmp = py_pushtmp();
    py_newstr(tmp, key);
    int res = py_dict_delitem(self, tmp);
    py_pop();
    return res;
}

int py_dict_getitem_by_int(py_Ref self, py_i64 key) {
    py_TValue tmp;
    py_newint(&tmp, key);
    return py_dict_getitem(self, &tmp);
}

bool py_dict_setitem_by_int(py_Ref self, py_i64 key, py_Ref val) {
    py_TValue tmp;
    py_newint(&tmp, key);
    return py_dict_setitem(self, &tmp, val);
}

int py_dict_delitem_by_int(py_Ref self, py_i64 key) {
    py_TValue tmp;
    py_newint(&tmp, key);
    return py_dict_delitem(self, &tmp);
}

int py_dict_len(py_Ref self) {
    assert(py_isdict(self));
    Dict* ud = py_touserdata(self);
    return ud->length;
}

bool py_dict_apply(py_Ref self, bool (*f)(py_Ref, py_Ref, void*), void* ctx) {
    Dict* ud = py_touserdata(self);
    for(int i = 0; i < ud->entries.length; i++) {
        DictEntry* entry = c11__at(DictEntry, &ud->entries, i);
        if(py_isnil(&entry->key)) continue;
        if(!f(&entry->key, &entry->val, ctx)) return false;
    }
    return true;
}

#undef Dict__step