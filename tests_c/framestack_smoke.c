#include "pocketpy/interpreter/framestack.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    FrameStack stack;
    FrameStack__ctor(&stack);

    py_Frame* frames[PK_FRAME_STACK_SLAB_CAPACITY * 3 + 7];
    const int count = (int)(sizeof(frames) / sizeof(frames[0]));
    for(int i = 0; i < count; i++) {
        frames[i] = FrameStack__alloc(&stack);
        frames[i]->ip = i;
        assert(FrameStack__owns(&stack, frames[i]));
        if(i > 0) assert(frames[i] != frames[i - 1]);
    }
    assert(stack.depth == (uint32_t)count);
    assert(stack.high_watermark == (uint32_t)count);
    assert(stack.slab_count >= 4);

    for(int i = count - 1; i >= 0; i--) {
        assert(frames[i]->ip == i);
        FrameStack__release(&stack, frames[i]);
    }
    assert(stack.depth == 0);

    py_Frame* first = FrameStack__alloc(&stack);
    FrameStack__release(&stack, first);
    FrameStack__dtor(&stack);
    puts("framestack smoke: ok");
    return 0;
}
