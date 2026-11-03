#include "pocketpy/interpreter/framestack.h"

#include "pocketpy/pocketpy.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static FrameStackSlab* frame_stack_new_slab(FrameStackSlab* prev) {
    FrameStackSlab* slab = py_malloc(sizeof(FrameStackSlab));
    if(slab == NULL) c11__abort("FrameStack: out of memory");
    slab->prev = prev;
    slab->next = NULL;
    slab->used = 0;
    slab->capacity = PK_FRAME_STACK_SLAB_CAPACITY;
    memset(slab->frames, 0, sizeof(slab->frames));
    if(prev != NULL) prev->next = slab;
    return slab;
}

void FrameStack__ctor(FrameStack* self) {
    memset(self, 0, sizeof(*self));
    self->first = frame_stack_new_slab(NULL);
    self->current = self->first;
    self->slab_count = 1;
}

void FrameStack__dtor(FrameStack* self) {
    assert(self->depth == 0);
    FrameStackSlab* slab = self->first;
    while(slab != NULL) {
        FrameStackSlab* next = slab->next;
        assert(slab->used == 0);
        py_free(slab);
        slab = next;
    }
    memset(self, 0, sizeof(*self));
}

py_Frame* FrameStack__alloc(FrameStack* self) {
    FrameStackSlab* slab = self->current;
    if(slab == NULL) {
        slab = frame_stack_new_slab(NULL);
        self->first = slab;
        self->current = slab;
        self->slab_count = 1;
    }

    if(slab->used == slab->capacity) {
        if(slab->next != NULL) {
            slab = slab->next;
            assert(slab->used == 0);
        } else {
            slab = frame_stack_new_slab(slab);
            self->slab_count++;
        }
        self->current = slab;
    }

    py_Frame* frame = &slab->frames[slab->used++];
    memset(frame, 0, sizeof(*frame));
    self->depth++;
    if(self->depth > self->high_watermark) self->high_watermark = self->depth;
    return frame;
}

void FrameStack__release(FrameStack* self, py_Frame* frame) {
    assert(self->depth > 0);
    FrameStackSlab* slab = self->current;
    assert(slab != NULL);
    assert(slab->used > 0);
    assert(frame == &slab->frames[slab->used - 1]);

    memset(frame, 0, sizeof(*frame));
    slab->used--;
    self->depth--;

    if(slab->used == 0 && slab->prev != NULL) self->current = slab->prev;
}

bool FrameStack__owns(const FrameStack* self, const py_Frame* frame) {
    for(const FrameStackSlab* slab = self->first; slab != NULL; slab = slab->next) {
        const py_Frame* begin = slab->frames;
        const py_Frame* end = begin + slab->capacity;
        if(frame >= begin && frame < end) return true;
    }
    return false;
}
