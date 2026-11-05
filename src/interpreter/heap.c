#include "tinypy/interpreter/heap.h"
#include "tinypy/config.h"
#include "tinypy/interpreter/objectpool.h"
#include "tinypy/interpreter/shape.h"
#include "tinypy/interpreter/typeinfo.h"
#include "tinypy/objects/base.h"
#include "tinypy/common/sstream.h"
#include "tinypy/tinypy.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static uint8_t encode_size_8b(int size, int* out_size) {
    int bit_length = c11__bit_length(size);
    int min_val = 1 << (bit_length - 1);
    int gap = min_val;
    float ratio = (float)(size - min_val) / gap;
    int ratio_3bit = (int)(ratio * 7.999f);
    *out_size = min_val + (int)(gap * ((float)ratio_3bit / 7.999f));
    return (uint8_t)((bit_length << 3) | ratio_3bit);
}

static int decode_size_8b(uint8_t byte) {
    int bit_length = byte >> 3;
    int ratio_3bit = byte & 0x07;
    int min_val = 1 << (bit_length - 1);
    int gap = min_val;
    float ratio = (float)ratio_3bit / 7.999f;
    return min_val + (int)(gap * ratio);
}

/*
 * Return the lazily created shape arena owned by the given Python type.
 * VM__dtor() releases these arenas after all heap objects are destroyed.
 */
ShapeArena* pk_type_shape_arena(py_Type type) {
    py_TypeInfo* typeinfo = pk_typeinfo(type);
    ShapeArena* arena = typeinfo->shape_arena;
    if(arena == NULL) {
        arena = ShapeArena__new();
        typeinfo->shape_arena = arena;
    }
    return arena;
}

void ManagedHeap__ctor(ManagedHeap* self) {
    MultiPool__ctor(&self->small_objects);
    c11_vector__ctor(&self->large_objects, sizeof(PyObject*));
    c11_vector__ctor(&self->gc_roots, sizeof(PyObject*));
    self->large_total_size = 0;

    for(int i = 0; i < (int)c11__count_array(self->freed_ma); i++) {
        self->freed_ma[i] = PK_GC_MIN_THRESHOLD;
    }
    self->gc_threshold = PK_GC_MIN_THRESHOLD;
    self->gc_counter = 0;
    self->minor_collections = 0;
    self->total_minor_collections = 0;
    self->total_major_collections = 0;
    self->total_objects_freed = 0;
    self->gc_enabled = true;
    self->gc_collecting = false;
    self->debug_callback = *py_None();
}

void ManagedHeap__dtor(ManagedHeap* self) {
    // small_objects
    MultiPool__dtor(&self->small_objects);
    // large_objects
    for(int i = 0; i < self->large_objects.length; i++) {
        PyObject* obj = c11__getitem(PyObject*, &self->large_objects, i);
        PyObject__dtor(obj);
        py_free(obj);
    }
    c11_vector__dtor(&self->large_objects);
    c11_vector__dtor(&self->gc_roots);
}

static void ManagedHeap__fire_debug_callback_start(ManagedHeap* self) {
    py_push(&self->debug_callback);
    py_pushnil();
    py_newstr(py_pushtmp(), "start");
    py_newstr(py_pushtmp(), "");
    bool ok = py_vectorcall(2, 0);
    if(!ok) {
        char* msg = py_formatexc();
        c11__abort("gc_debug_callback error!!\n%s", msg);
    }
}

static void ManagedHeap__fire_debug_callback_stop(ManagedHeap* self,
                                                  ManagedHeapSwpetInfo* out_info) {
    assert(out_info != NULL);

    c11_sbuf buf;
    c11_sbuf__ctor(&buf);

    const int64_t NANOS_PER_MS = 1000000000 / 1000;
    const char* DIVIDER = "------------------------------------------------------------\n";

    int64_t mark_ms = (out_info->mark_end_ns - out_info->start_ns) / NANOS_PER_MS;
    int64_t swpet_ms = (out_info->swpet_end_ns - out_info->mark_end_ns) / NANOS_PER_MS;

    c11_sbuf__write_cstr(&buf, DIVIDER);
    pk_sprintf(&buf, "start:        %f\n", out_info->start_ns / 1e9);
    pk_sprintf(&buf, "mark_ms:      %i\n", (py_i64)mark_ms);
    pk_sprintf(&buf, "swpet_ms:     %i\n", (py_i64)swpet_ms);
    pk_sprintf(&buf, "total_ms:     %i\n", (py_i64)(mark_ms + swpet_ms));
    c11_sbuf__write_cstr(&buf, DIVIDER);
    pk_sprintf(&buf, "types_length: %d\n", out_info->types_length);
    pk_sprintf(&buf, "small_freed:  %d\n", out_info->small_freed);
    pk_sprintf(&buf, "large_freed:  %d\n", out_info->large_freed);
    c11_sbuf__write_cstr(&buf, DIVIDER);

    if(out_info->small_freed != 0 || out_info->large_freed != 0) {
        char line_buf[256];
        for(int i = 0; i < out_info->types_length; i++) {
            const char* type_name = py_tpname(i);
            int s_freed = out_info->small_types[i];
            int l_freed = out_info->large_types[i];
            if(s_freed == 0 && l_freed == 0) continue;
            snprintf(line_buf,
                     sizeof(line_buf),
                     "[%-24s] small: %6d  large: %6d\n",
                     type_name,
                     s_freed,
                     l_freed);
            c11_sbuf__write_cstr(&buf, line_buf);
        }
        c11_sbuf__write_cstr(&buf, DIVIDER);
    }

    pk_sprintf(&buf, "auto_thres.before:        %d\n", out_info->auto_thres.before);
    pk_sprintf(&buf, "auto_thres.after:         %d\n", out_info->auto_thres.after);
    pk_sprintf(&buf, "auto_thres.upper:         %d\n", out_info->auto_thres.upper);
    pk_sprintf(&buf, "auto_thres.lower:         %d\n", out_info->auto_thres.lower);
    pk_sprintf(&buf, "auto_thres.avg_freed:     %d\n", out_info->auto_thres.avg_freed);
    pk_sprintf(&buf, "auto_thres.free_ratio:    %f\n", out_info->auto_thres.free_ratio);
    c11_sbuf__write_cstr(&buf, DIVIDER);

    py_push(&self->debug_callback);
    py_pushnil();
    py_newstr(py_pushtmp(), "stop");
    py_StackRef arg = py_pushtmp();
    c11_sbuf__py_submit(&buf, arg);
    bool ok = py_vectorcall(2, 0);
    if(!ok) {
        char* msg = py_formatexc();
        c11__abort("gc_debug_callback error!!\n%s", msg);
    }
}

static int ManagedHeap__sweep_minor(ManagedHeap* self, ManagedHeapSwpetInfo* out_info);

#ifndef PK_GC_MAJOR_INTERVAL
#define PK_GC_MAJOR_INTERVAL 8U
#endif

#if PK_GC_MAJOR_INTERVAL < 1
#error "PK_GC_MAJOR_INTERVAL must be at least 1"
#endif

static int ManagedHeap__young_live_count(const ManagedHeap* self) {
    int count = self->small_objects.nursery.length;
    for(int i = 0; i < self->large_objects.length; i++) {
        const PyObject* obj = c11__getitem(PyObject*, &self->large_objects, i);
        if(!(obj->gc_marked & PK_GC_OLD) && count < INT_MAX) count++;
    }
    return count;
}

static int ManagedHeap__run_collection(ManagedHeap* self, bool major, bool automatic) {
    /* Finalizers/debug callbacks must not re-enter a half-finished sweep. */
    if(self->gc_collecting) return 0;
    self->gc_collecting = true;
    const int before = self->gc_threshold;
    self->gc_counter = 0;

    ManagedHeapSwpetInfo* out_info = NULL;
    if(!py_isnone(&self->debug_callback)) {
        out_info = ManagedHeapSwpetInfo__new();
        ManagedHeap__fire_debug_callback_start(self);
    }

    if(major) {
        /* A full collection reclaims old cycles and tenures all survivors. */
        ManagedHeap__mark(self);
    } else {
        /* Minor collections preserve old objects and reclaim the nursery. */
        ManagedHeap__mark_minor(self);
    }
    if(out_info) out_info->mark_end_ns = time_ns();

    int freed = major ? ManagedHeap__sweep(self, out_info)
                      : ManagedHeap__sweep_minor(self, out_info);
    if(out_info) out_info->swpet_end_ns = time_ns();

    if(major) {
        self->minor_collections = 0;
        self->total_major_collections++;
    } else {
        self->minor_collections++;
        self->total_minor_collections++;
    }
    self->total_objects_freed += (uint64_t)freed;

    if(automatic) {
        /*
         * Adapt nursery size to the number of survivors, without division by
         * zero or overflowing the threshold calculation. This biases toward
         * fewer collections when many objects survive a nursery pass.
         */
        const int lower = PK_GC_MIN_THRESHOLD;
        const int upper = PK_GC_MIN_THRESHOLD > INT_MAX / 8
                              ? INT_MAX : PK_GC_MIN_THRESHOLD * 8;
        const int young = ManagedHeap__young_live_count(self);
        const int available = (upper - lower) / 2;
        int next = young >= available ? upper : lower + 2 * young;
        if(next < lower) next = lower;
        if(next > upper) next = upper;
        self->gc_threshold = next;
        self->freed_ma[0] = self->freed_ma[1];
        self->freed_ma[1] = self->freed_ma[2];
        self->freed_ma[2] = freed;
    }

    if(out_info) {
        const int avg = (int)(((int64_t)self->freed_ma[0] + self->freed_ma[1] +
                               self->freed_ma[2]) / 3);
        out_info->auto_thres.before = before;
        out_info->auto_thres.after = self->gc_threshold;
        out_info->auto_thres.lower = PK_GC_MIN_THRESHOLD;
        out_info->auto_thres.upper = PK_GC_MIN_THRESHOLD > INT_MAX / 8
                                        ? INT_MAX : PK_GC_MIN_THRESHOLD * 8;
        out_info->auto_thres.avg_freed = avg;
        out_info->auto_thres.free_ratio = before > 0 ? (float)avg / (float)before : 0.0f;
        ManagedHeap__fire_debug_callback_stop(self, out_info);
        ManagedHeapSwpetInfo__delete(out_info);
    }
    self->gc_collecting = false;
    return freed;
}

int ManagedHeap__collect_hint(ManagedHeap* self) {
    if(!self->gc_enabled || self->gc_counter < self->gc_threshold) return 0;
    const bool major = self->minor_collections >= PK_GC_MAJOR_INTERVAL - 1;
    return ManagedHeap__run_collection(self, major, true);
}

int ManagedHeap__collect(ManagedHeap* self) {
    return ManagedHeap__run_collection(self, true, false);
}

int ManagedHeap__collect_minor(ManagedHeap* self) {
    return ManagedHeap__run_collection(self, false, false);
}

static int ManagedHeap__sweep_minor(ManagedHeap* self, ManagedHeapSwpetInfo* out_info) {
    int small_freed = MultiPool__sweep_nursery(&self->small_objects,
                                              out_info ? out_info->small_types : NULL);
    int living_count = 0;
    const int old_length = self->large_objects.length;
    for(int i = 0; i < old_length; i++) {
        PyObject* obj = c11__getitem(PyObject*, &self->large_objects, i);
        if(obj->gc_marked & PK_GC_OLD) {
            /* Old objects retain the mark bit until the next full marking. */
            c11__setitem(PyObject*, &self->large_objects, living_count++, obj);
        } else if(obj->gc_marked & PK_GC_MARKED) {
            obj->gc_marked &= (uint8_t)~PK_GC_MARKED;
            if(obj->gc_marked & PK_GC_SURVIVED)
                obj->gc_marked = (obj->gc_marked & PK_GC_UNTRACKED) | PK_GC_OLD;
            else
                obj->gc_marked |= PK_GC_SURVIVED;
            c11__setitem(PyObject*, &self->large_objects, living_count++, obj);
        } else {
            if(out_info) out_info->large_types[obj->type]++;
            self->large_total_size -= decode_size_8b(obj->size_8b);
            PyObject__dtor(obj);
            py_free(obj);
        }
    }
    const int large_freed = old_length - living_count;
    self->large_objects.length = living_count;
    if(out_info) {
        out_info->small_freed = small_freed;
        out_info->large_freed = large_freed;
    }
    return small_freed + large_freed;
}

int ManagedHeap__sweep(ManagedHeap* self, ManagedHeapSwpetInfo* out_info) {
    // small_objects
    int small_freed =
        MultiPool__sweep_dealloc(&self->small_objects, out_info ? out_info->small_types : NULL);
    // large_objects
    int large_living_count = 0;
    for(int i = 0; i < self->large_objects.length; i++) {
        PyObject* obj = c11__getitem(PyObject*, &self->large_objects, i);
        if(obj->gc_marked & PK_GC_MARKED) {
            obj->gc_marked = (obj->gc_marked & PK_GC_UNTRACKED) | PK_GC_OLD;
            c11__setitem(PyObject*, &self->large_objects, large_living_count, obj);
            large_living_count++;
        } else {
            if(out_info) out_info->large_types[obj->type]++;
            self->large_total_size -= decode_size_8b(obj->size_8b);
            PyObject__dtor(obj);
            py_free(obj);
        }
    }
    // shrink `self->large_objects`
    int large_freed = self->large_objects.length - large_living_count;
    self->large_objects.length = large_living_count;
    if(out_info) {
        out_info->small_freed = small_freed;
        out_info->large_freed = large_freed;
    }
    return small_freed + large_freed;
}

PyObject* ManagedHeap__gcnew(ManagedHeap* self, py_Type type, int slots, int udsize) {
    assert(slots >= 0 || slots == -1 || slots == PK_OBJ_SHAPED_SLOTS);
    // Header + slots/shapes/dict + native userdata.
    int size = sizeof(PyObject) + PK_OBJ_SLOTS_SIZE(slots) + udsize;
    PyObject* obj = MultiPool__alloc(&self->small_objects, size);
    uint8_t size_8b = 0;
    if(obj == NULL) {
        obj = py_malloc(size);
        int quantized_size;
        size_8b = encode_size_8b(size, &quantized_size);
        self->large_total_size += quantized_size;
        c11_vector__push(PyObject*, &self->large_objects, obj);
    }
    obj->type = type;
    obj->size_8b = size_8b;
    obj->gc_marked = 0;
    obj->slots = slots;

    // initialize slots or dict
    if(slots >= 0) {
        memset(obj->flex, 0, slots * sizeof(py_TValue));
    } else if(slots == -1) {
        float load_factor = (type == tp_type || type == tp_module) ? PK_TYPE_ATTR_LOAD_FACTOR
                                                                   : PK_INST_ATTR_LOAD_FACTOR;
        NameDict__ctor((void*)obj->flex, load_factor);
    } else {
        ShapeArena* arena = pk_type_shape_arena(type);
        *(ShapeStorage**)obj->flex = ShapeStorage__new(ShapeArena__root(arena));
    }

    /*
     * A recycled pool slot may contain bytes from a previously destroyed type.
     * Initialize type userdata before py_TypeInfo__common_init() fills it in,
     * so lazy shape arenas are always NULL for newly created type objects.
     */
    if(type == tp_type && udsize >= (int)sizeof(py_TypeInfo)) {
        memset(obj->flex + PK_OBJ_SLOTS_SIZE(slots), 0, (size_t)udsize);
    }

    if(self->gc_counter < INT_MAX) self->gc_counter++;
    return obj;
}