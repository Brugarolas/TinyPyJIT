#pragma once

#include "tinypy/objects/object.h"
#include "tinypy/interpreter/objectpool.h"
#include <time.h>

/*
 * Generational, non-moving mark-and-sweep heap.
 * The existing object header stores the mark, tracking, age, and generation bits.
 * Minor collections conservatively scan old objects, so no write barrier is
 * required for correctness, including native extensions and shaped objects.
 */
typedef struct ManagedHeap {
    MultiPool small_objects;
    c11_vector /* PyObject* */ large_objects;
    c11_vector /* PyObject* */ gc_roots;
    size_t large_total_size;

    int freed_ma[3];
    int gc_threshold;
    int gc_counter;
    bool gc_enabled;
    bool gc_collecting;
    py_TValue debug_callback;

    unsigned minor_collections;
    uint64_t total_minor_collections;
    uint64_t total_major_collections;
    uint64_t total_objects_freed;
} ManagedHeap;

typedef struct {
    int64_t start_ns;
    int64_t mark_end_ns;
    int64_t swpet_end_ns;

    int types_length;
    int* small_types;
    int* large_types;

    int small_freed;
    int large_freed;

    struct {
        int before;
        int after;
        int upper;
        int lower;
        int avg_freed;
        float free_ratio;
    } auto_thres;
} ManagedHeapSwpetInfo;

void ManagedHeap__ctor(ManagedHeap* self);
void ManagedHeap__dtor(ManagedHeap* self);

ManagedHeapSwpetInfo* ManagedHeapSwpetInfo__new();
void ManagedHeapSwpetInfo__delete(ManagedHeapSwpetInfo* self);

int ManagedHeap__collect_hint(ManagedHeap* self);
int ManagedHeap__collect(ManagedHeap* self);
int ManagedHeap__collect_minor(ManagedHeap* self);
int ManagedHeap__sweep(ManagedHeap* self, ManagedHeapSwpetInfo* out_info);

#define ManagedHeap__new(self, type, slots, udsize)                                                \
    ManagedHeap__gcnew((self), (type), (slots), (udsize))
PyObject* ManagedHeap__gcnew(ManagedHeap* self, py_Type type, int slots, int udsize);

void ManagedHeap__mark(ManagedHeap* self);
void ManagedHeap__mark_minor(ManagedHeap* self);
