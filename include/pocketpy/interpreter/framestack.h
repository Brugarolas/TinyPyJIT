#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pocketpy/interpreter/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PK_FRAME_STACK_SLAB_CAPACITY
#define PK_FRAME_STACK_SLAB_CAPACITY 64
#endif

typedef struct FrameStackSlab {
    struct FrameStackSlab* prev;
    struct FrameStackSlab* next;
    uint16_t used;
    uint16_t capacity;
    py_Frame frames[PK_FRAME_STACK_SLAB_CAPACITY];
} FrameStackSlab;

typedef struct FrameStack {
    FrameStackSlab* first;
    FrameStackSlab* current;
    uint32_t depth;
    uint32_t high_watermark;
    uint32_t slab_count;
} FrameStack;

void FrameStack__ctor(FrameStack* self);
void FrameStack__dtor(FrameStack* self);

/*
 * Allocate/release one frame in strict LIFO order. Returned frame addresses are
 * stable for the lifetime of the frame because slabs are never reallocated.
 */
py_Frame* FrameStack__alloc(FrameStack* self);
void FrameStack__release(FrameStack* self, py_Frame* frame);

/* Fast ownership check used only by assertions/debug tooling. */
bool FrameStack__owns(const FrameStack* self, const py_Frame* frame);

#ifdef __cplusplus
}  // extern "C"
#endif
