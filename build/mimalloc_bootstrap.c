#include "tinypy/tinypy.h"
#include "mimalloc.h"

/*
 * The executable redirects py_initialize() to this function at compile time.
 * Install the allocation callbacks before the VM allocates its first object.
 * All three callbacks must use the same allocator for pointer ownership.
 */
void tinypy_initialize_mimalloc(void) {
    /*
     * Keep freed memory committed instead of returning it to the OS. Growing
     * containers (dict entries, lists) reallocate into fresh regions, and every
     * newly committed page costs a kernel page fault on first touch; reusing
     * already-committed memory avoids most of them (measured ~30% on building
     * a 2M-entry dict). The trade-off is that the resident size does not shrink
     * after a peak. MIMALLOC_PURGE_DELAY still overrides this default.
     */
    mi_option_set_default(mi_option_purge_delay, -1);
    py_AppCallbacks* callbacks = py_appcallbacks();
    callbacks->malloc = mi_malloc;
    callbacks->realloc = mi_realloc;
    callbacks->free = mi_free;
    py_initialize();
}
