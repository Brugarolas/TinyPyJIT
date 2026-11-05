#include "tinypy/interpreter/objectpool.h"

#include "tinypy/objects/object.h"
#include "tinypy/common/sstream.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

static PoolArena* PoolArena__new(int block_size) {
    assert(kPoolArenaSize % block_size == 0);
    int block_count = kPoolArenaSize / block_size;
    PoolArena* self = py_malloc(sizeof(PoolArena) + sizeof(int) * block_count);
    self->block_size = block_size;
    self->block_count = block_count;
    self->unused_length = block_count;
    for(int i = 0; i < block_count; i++) {
        self->unused[i] = i;
    }
    memset(self->data, 0, kPoolArenaSize);
    return self;
}

static void* PoolArena__alloc(PoolArena* self) {
    assert(self->unused_length > 0);
    int index = self->unused[self->unused_length - 1];
    self->unused_length--;
    return self->data + index * self->block_size;
}

static int PoolArena__sweep_dealloc(PoolArena* self, int* out_types,
                                    c11_vector* old_objects) {
    int unused_length_before = self->unused_length;
    self->unused_length = 0;
    for(int i = 0; i < self->block_count; i++) {
        PyObject* obj = (PyObject*)(self->data + i * self->block_size);
        if(obj->type == 0) {
            // free slot
            self->unused[self->unused_length] = i;
            self->unused_length++;
        } else {
            if(obj->gc_marked & PK_GC_MARKED) {
                // marked, clear mark
                obj->gc_marked = (obj->gc_marked & PK_GC_UNTRACKED) | PK_GC_OLD;
                c11_vector__push(PyObject*, old_objects, obj);
            } else {
                // not marked, need to free
                if(out_types) out_types[obj->type]++;
                PyObject__dtor(obj);
                obj->type = 0;
                obj->gc_marked = 0;
                self->unused[self->unused_length] = i;
                self->unused_length++;
            }
        }
    }
    return self->unused_length - unused_length_before;
}

static void Pool__ctor(Pool* self, int block_size) {
    c11_vector__ctor(&self->arenas, sizeof(PoolArena*));
    self->available_index = 0;
    self->block_size = block_size;
}

static void Pool__dtor(Pool* self) {
    for(int i = 0; i < self->arenas.length; i++) {
        PoolArena* arena = c11__getitem(PoolArena*, &self->arenas, i);
        for(int i = 0; i < arena->block_count; i++) {
            PyObject* obj = (PyObject*)(arena->data + i * self->block_size);
            if(obj->type != 0) PyObject__dtor(obj);
        }
        py_free(arena);
    }
    c11_vector__dtor(&self->arenas);
}

static void* Pool__alloc(Pool* self, PoolArena** out_arena) {
    /* Minor collections can create holes in previously full arenas. */
    while(self->available_index < self->arenas.length) {
        PoolArena* arena = c11__getitem(PoolArena*, &self->arenas, self->available_index);
        if(arena->unused_length != 0) break;
        self->available_index++;
    }

    PoolArena* arena;
    if(self->available_index < self->arenas.length) {
        arena = c11__getitem(PoolArena*, &self->arenas, self->available_index);
    } else {
        arena = PoolArena__new(self->block_size);
        c11_vector__push(PoolArena*, &self->arenas, arena);
        self->available_index = self->arenas.length - 1;
    }
    void* ptr = PoolArena__alloc(arena);
    if(arena->unused_length == 0) self->available_index++;
    *out_arena = arena;
    return ptr;
}

static int Pool__sweep_dealloc(Pool* self, int* out_types,
                              c11_vector* old_objects) {
    PoolArena** p = self->arenas.data;

    int freed = 0;
    for(int i = 0; i < self->arenas.length; i++) {
        freed += PoolArena__sweep_dealloc(p[i], out_types, old_objects);
    }

    // move arenas with `unused_length == 0` to the front
    int j = 0;
    for(int i = 0; i < self->arenas.length; i++) {
        if(p[i]->unused_length == 0) {
            PoolArena* tmp = p[i];
            p[i] = p[j];
            p[j] = tmp;
            j++;
        }
    }

    // move arenas with `unused_length < block_count` to the front
    int k = j;
    for(int i = j; i < self->arenas.length; i++) {
        if(p[i]->unused_length < p[i]->block_count) {
            PoolArena* tmp = p[i];
            p[i] = p[k];
            p[k] = tmp;
            k++;
        }
    }

    // free excess free arenas
    int free_quota = self->arenas.length / 2;
    int min_length = c11__max(free_quota, j + 1);
    while(self->arenas.length > min_length) {
        PoolArena* back_arena = c11_vector__back(PoolArena*, &self->arenas);
        if(back_arena->unused_length == back_arena->block_count) {
            py_free(back_arena);
            c11_vector__pop(&self->arenas);
        } else {
            break;
        }
    }

    // [[0, 0, 0, 0, 0, 1], 1, 1, 1, 2, 2]
    //                  ^j=5         ^k
    self->available_index = j;
    return freed;
}

void* MultiPool__alloc(MultiPool* self, int size) {
    assert(size > 0);
    int index = (size - 1) >> 5;
    if(index < kMultiPoolCount) {
        Pool* pool = &self->pools[index];
        PoolArena* arena = NULL;
        void* object = Pool__alloc(pool, &arena);
        PoolNurseryEntry* entry = c11_vector__emplace(&self->nursery);
        entry->object = object;
        entry->arena = arena;
        return object;
    }
    return NULL;
}

int MultiPool__sweep_dealloc(MultiPool* self, int* out_types) {
    self->old_objects.length = 0;
    int freed = 0;
    for(int i = 0; i < kMultiPoolCount; i++) {
        Pool* item = &self->pools[i];
        freed += Pool__sweep_dealloc(item, out_types, &self->old_objects);
    }
    /* Full collection tenures all survivors and invalidates nursery entries. */
    self->nursery.length = 0;
    return freed;
}

/* Minor sweep touches only allocated young slots, not every old pool slot. */
int MultiPool__sweep_nursery(MultiPool* self, int* out_types) {
    int freed = 0;
    int surviving_young = 0;
    PoolNurseryEntry* entries = self->nursery.data;
    for(int i = 0; i < self->nursery.length; i++) {
        PoolNurseryEntry entry = entries[i];
        PyObject* obj = entry.object;
        assert(obj->type != 0);
        assert(!(obj->gc_marked & PK_GC_OLD));
        if(obj->gc_marked & PK_GC_MARKED) {
            obj->gc_marked &= (uint8_t)~PK_GC_MARKED;
            if(obj->gc_marked & PK_GC_SURVIVED) {
                obj->gc_marked = (obj->gc_marked & PK_GC_UNTRACKED) | PK_GC_OLD;
                c11_vector__push(PyObject*, &self->old_objects, obj);
            } else {
                obj->gc_marked |= PK_GC_SURVIVED;
                entries[surviving_young++] = entry;
            }
        } else {
            if(out_types) out_types[obj->type]++;
            PyObject__dtor(obj);
            obj->type = 0;
            obj->gc_marked = 0;
            assert(entry.arena->unused_length < entry.arena->block_count);
            uintptr_t offset = (uintptr_t)((char*)obj - entry.arena->data);
            assert(offset % (uintptr_t)entry.arena->block_size == 0);
            int index = (int)(offset / (uintptr_t)entry.arena->block_size);
            assert(index >= 0 && index < entry.arena->block_count);
            entry.arena->unused[entry.arena->unused_length++] = index;
            freed++;
        }
    }
    self->nursery.length = surviving_young;

    if(freed != 0) {
        for(int i = 0; i < kMultiPoolCount; i++) {
            self->pools[i].available_index = 0;
        }
    }
    return freed;
}

/* Old objects remain marked after minor collections. Reset before a full mark. */
void MultiPool__clear_marks(MultiPool* self) {
    for(int i = 0; i < self->old_objects.length; i++) {
        PyObject* obj = c11__getitem(PyObject*, &self->old_objects, i);
        obj->gc_marked &= (uint8_t)~PK_GC_MARKED;
    }
    for(int i = 0; i < self->nursery.length; i++) {
        PoolNurseryEntry* item = c11__at(PoolNurseryEntry, &self->nursery, i);
        item->object->gc_marked &= (uint8_t)~PK_GC_MARKED;
    }
}

void MultiPool__ctor(MultiPool* self) {
    c11_vector__ctor(&self->nursery, sizeof(PoolNurseryEntry));
    c11_vector__ctor(&self->old_objects, sizeof(PyObject*));
    for(int i = 0; i < kMultiPoolCount; i++) {
        Pool__ctor(&self->pools[i], 32 * (i + 1));
    }
}

void MultiPool__dtor(MultiPool* self) {
    for(int i = 0; i < kMultiPoolCount; i++) {
        Pool__dtor(&self->pools[i]);
    }
    c11_vector__dtor(&self->nursery);
    c11_vector__dtor(&self->old_objects);
}

size_t MultiPool__total_allocated_bytes(MultiPool* self) {
    size_t total = 0;
    for(int i = 0; i < kMultiPoolCount; i++) {
        Pool* item = &self->pools[i];
        total += (size_t)item->arenas.length * kPoolArenaSize;
    }
    return total;
}

c11_string* MultiPool__summary(MultiPool* self) {
    c11_sbuf sbuf;
    c11_sbuf__ctor(&sbuf);
    int arena_count = 0;
    char buf[256];
    for(int i = 0; i < kMultiPoolCount; i++) {
        Pool* item = &self->pools[i];
        arena_count += item->arenas.length;
        int total_bytes = item->arenas.length * kPoolArenaSize;
        int used_bytes = 0;
        for(int j = 0; j < item->arenas.length; j++) {
            PoolArena* arena = c11__getitem(PoolArena*, &item->arenas, j);
            used_bytes += (arena->block_count - arena->unused_length) * arena->block_size;
        }
        float used_pct = (float)used_bytes / total_bytes * 100;
        if(total_bytes == 0) used_pct = 0.0f;
        snprintf(buf,
                 sizeof(buf),
                 "Pool %3d: len(arenas)=%d (%d full), size=%d/%d (%.1f%% used)\n",
                 item->block_size,
                 item->arenas.length,
                 item->available_index,
                 used_bytes,
                 total_bytes,
                 used_pct);
        c11_sbuf__write_cstr(&sbuf, buf);
    }
    long long total_size = arena_count * kPoolArenaSize;
    double total_size_mb = (long long)(total_size / 1024) / 1024.0;
    snprintf(buf, sizeof(buf), "Total: %.2f MB\n", total_size_mb);
    c11_sbuf__write_cstr(&sbuf, buf);
    return c11_sbuf__submit(&sbuf);
}
