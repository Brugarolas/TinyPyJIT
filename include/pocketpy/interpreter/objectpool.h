#pragma once

#include "tinypy/common/vector.h"
#include "tinypy/common/str.h"
#include <stdint.h>

#define kPoolArenaSize (120 * 1024)
#define kMultiPoolCount 5
// #define kPoolMaxBlockSize (32 * kMultiPoolCount)

typedef struct PoolArena {
    int block_size;
    int block_count;
    int unused_length;

    union {
        char data[kPoolArenaSize];
        int64_t _align64;
    };

    int unused[];
} PoolArena;

typedef struct Pool {
    c11_vector /* PoolArena* */ arenas;
    int available_index;
    int block_size;
} Pool;

/* Object header flags: bit 0 is marking; bit 1 is Python gc.untrack(). */
#define PK_GC_MARKED UINT8_C(0x01)
#define PK_GC_UNTRACKED UINT8_C(0x02)
#define PK_GC_SURVIVED UINT8_C(0x04)
#define PK_GC_OLD UINT8_C(0x08)

struct PyObject;

typedef struct PoolNurseryEntry {
    struct PyObject* object;
    PoolArena* arena;
} PoolNurseryEntry;

typedef struct MultiPool {
    Pool pools[kMultiPoolCount];
    c11_vector /* PoolNurseryEntry */ nursery;
    c11_vector /* PyObject* */ old_objects;
} MultiPool;

void* MultiPool__alloc(MultiPool* self, int size);
int MultiPool__sweep_dealloc(MultiPool* self, int* out_types);
int MultiPool__sweep_nursery(MultiPool* self, int* out_types);
void MultiPool__clear_marks(MultiPool* self);
void MultiPool__ctor(MultiPool* self);
void MultiPool__dtor(MultiPool* self);
size_t MultiPool__total_allocated_bytes(MultiPool* self);
c11_string* MultiPool__summary(MultiPool* self);