#include "tinypy/tinypy.h"
#include "mimalloc.h"

/*
 * The executable redirects py_initialize() to this function at compile time.
 * Install the allocation callbacks before the VM allocates its first object.
 * All three callbacks must use the same allocator for pointer ownership.
 */
void tinypy_initialize_mimalloc(void) {
    py_AppCallbacks* callbacks = py_appcallbacks();
    callbacks->malloc = mi_malloc;
    callbacks->realloc = mi_realloc;
    callbacks->free = mi_free;
    py_initialize();
}
