#include "tinypy/tinypy.h"
#include "tinypy/interpreter/vm.h"

static bool gc_enable(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    heap->gc_enabled = true;
    py_newnone(py_retval());
    return true;
}

static bool gc_disable(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    heap->gc_enabled = false;
    py_newnone(py_retval());
    return true;
}

static bool gc_isenabled(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    py_newbool(py_retval(), heap->gc_enabled);
    return true;
}

static bool gc_collect(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    int freed = ManagedHeap__collect(heap);
    py_newint(py_retval(), freed);
    return true;
}

static bool gc_collect_minor(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    int freed = ManagedHeap__collect_minor(&pk_current_vm->heap);
    py_newint(py_retval(), freed);
    return true;
}

static bool gc_collect_hint(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    int freed = ManagedHeap__collect_hint(heap);
    py_newint(py_retval(), freed);
    return true;
}

static bool gc_setup_debug_callback(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    ManagedHeap* heap = &pk_current_vm->heap;
    heap->debug_callback = *argv;
    py_newnone(py_retval());
    return true;
}

/* Return a snapshot; do not expose internal bookkeeping memory to Python. */
static bool gc_stats(int argc, py_Ref argv) {
    PY_CHECK_ARGC(0);
    ManagedHeap* heap = &pk_current_vm->heap;
    py_newdict(py_retval());
    py_TValue number;

#define GC_STAT(name, value) do { \
    py_newint(&number, (py_i64)(value)); \
    if(!py_dict_setitem_by_str(py_retval(), (name), &number)) return false; \
} while(0)
    GC_STAT("minor_collections", heap->total_minor_collections);
    GC_STAT("major_collections", heap->total_major_collections);
    GC_STAT("objects_freed", heap->total_objects_freed);
    GC_STAT("nursery_small", heap->small_objects.nursery.length);
    GC_STAT("tenured_small", heap->small_objects.old_objects.length);
    GC_STAT("large_objects", heap->large_objects.length);
    GC_STAT("threshold", heap->gc_threshold);
#undef GC_STAT
    return true;
}

static bool gc_is_tracked(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    if(!argv->is_ptr) {
        py_newbool(py_retval(), false);
        return true;
    }
    bool res = !(argv->_obj->gc_marked & 0b10);
    py_newbool(py_retval(), res);
    return true;
}

static bool gc_track(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    if(!argv->is_ptr) return TypeError("gc.track() only accepts objects");
    /* Preserve generation/age metadata when changing tracking state. */
    argv->_obj->gc_marked &= (uint8_t)~PK_GC_UNTRACKED;
    py_newnone(py_retval());
    return true;
}

static bool gc_untrack(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    if(!argv->is_ptr) return TypeError("gc.untrack() only accepts objects");
    argv->_obj->gc_marked |= PK_GC_UNTRACKED;
    py_newnone(py_retval());
    return true;
}

void pk__add_module_gc() {
    py_Ref mod = py_newmodule("gc");

    py_bindfunc(mod, "enable", gc_enable);
    py_bindfunc(mod, "disable", gc_disable);
    py_bindfunc(mod, "isenabled", gc_isenabled);

    py_bindfunc(mod, "collect", gc_collect);
    py_bindfunc(mod, "collect_minor", gc_collect_minor);
    py_bindfunc(mod, "stats", gc_stats);
    py_bindfunc(mod, "collect_hint", gc_collect_hint);
    py_bindfunc(mod, "setup_debug_callback", gc_setup_debug_callback);

    py_bindfunc(mod, "is_tracked", gc_is_tracked);
    py_bindfunc(mod, "track", gc_track);
    py_bindfunc(mod, "untrack", gc_untrack);
}
