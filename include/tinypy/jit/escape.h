#pragma once

/*
 * Escape analysis + scalar replacement (SROA) over RegVM register bytecode.
 *
 * A candidate allocation is a BUILD_TUPLE, a BUILD_LIST, or a CALL whose
 * CONSTRUCT inline cache says it instantiates a Python class with
 * object.__new__ and an __init__ that only stores its arguments or constants
 * into fields of self. The analysis follows the registers that name the new
 * object (the result register and MOVE copies of it) through the rest of its
 * basic block. The only accepted uses are:
 *
 *   UNPACK_SEQUENCE     of a tuple/list with the matching length;
 *   LOAD_SUBSCR         of a tuple/list with a constant (LOAD_I16) index;
 *   LOAD_ATTR[_IC]      of an object field written by __init__;
 *   MOVE                (another register names the object).
 *
 * Any other read of such a register is an escape. When the block ends, every
 * register still naming the object must be dead (no path reads it before
 * writing it) - except at most one named local, which keeps the object alive
 * for frame introspection until it is overwritten (the "holding range").
 *
 * The native compiler then never allocates the object: its components are
 * copied into a GC-rooted save area and every use reads them from there. To
 * keep deoptimization exact, every PC of the object's region or holding range
 * records which registers name it; a side exit, or a frame introspection
 * (RegJit__force_frame), materializes the real object into exactly those
 * registers first.
 */

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/common/vector.h"
#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/objects/base.h"
#include "tinypy/objects/codeobject.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JIT_EA_MAX_VIRTUALS 16
#define JIT_EA_MAX_COMPONENTS 8
#define JIT_EA_MAX_FIELDS 8
/* Save-area slots per compiled body: lives on the C stack of every native
 * activation, so deep recursion multiplies it (16 slots = 256 bytes). */
#define JIT_EA_MAX_SAVE 16

typedef enum JitEaKind {
    JIT_EA_TUPLE = 0,
    JIT_EA_LIST,
    JIT_EA_OBJECT,
} JitEaKind;

typedef enum JitEaAction {
    JIT_EA_NONE = 0,
    JIT_EA_BUILD,  /* the allocation: save the components, mark the object pending */
    JIT_EA_UNPACK, /* UNPACK_SEQUENCE of the object: copy every component */
    JIT_EA_SUBSCR, /* LOAD_SUBSCR with a constant index: copy component `arg` */
    JIT_EA_ATTR,   /* LOAD_ATTR of field `arg` */
} JitEaAction;

/* A 256-bit register set. */
typedef struct JitEaRegs {
    uint64_t bits[4];
} JitEaRegs;

/* Registers naming the object right before the instruction at `pc` executes. */
typedef struct JitEaPoint {
    int32_t pc;
    JitEaRegs regs;
} JitEaPoint;

/* An object field as __init__ leaves it: a constructor argument or a constant. */
typedef struct JitEaField {
    py_Name name;
    int8_t component; /* argument index (0 = first after self), or -1 for `constant` */
    py_TValue constant;
} JitEaField;

typedef struct JitEaVirtual {
    JitEaKind kind;
    int32_t build_pc;
    int32_t last_use_pc;     /* -1: the object is never read */
    uint8_t result_reg;
    uint8_t component_base;  /* first component register at the build */
    uint8_t component_count;
    uint16_t save_offset;    /* save-area index of component 0 */
    int16_t holding_reg;     /* named local alive past the block, or -1 */

    /* JIT_EA_OBJECT */
    py_Type type;
    FuncDecl* init_decl;     /* referenced (+1) while the result lives */
    uint8_t field_count;
    JitEaField fields[JIT_EA_MAX_FIELDS]; /* in __init__'s first-store order */

    c11_vector /*T=JitEaPoint*/ points; /* sorted by pc */
    c11_vector /*T=int32_t*/ kills;     /* holding range ends right before these PCs */
} JitEaVirtual;

typedef struct JitEaSite {
    uint8_t action;  /* JitEaAction */
    uint8_t virtual_index;
    uint8_t arg;
} JitEaSite;

typedef struct JitEaResult {
    int virtual_count;
    uint16_t save_count;
    JitEaVirtual virtuals[JIT_EA_MAX_VIRTUALS];
    JitEaSite* sites; /* one per PC */
    int site_count;
} JitEaResult;

/*
 * Analyze one compiled body. Never fails the compilation: on any problem the
 * result simply has fewer (or no) virtual objects. `out` must be destroyed with
 * JitEa__dtor().
 */
void JitEa__analyze(const RegCode* code, const CodeObject* co, JitEaResult* out);
void JitEa__dtor(JitEaResult* self);

/* Registers naming `v` before `pc`, or NULL when `pc` is outside its lifetime. */
const JitEaRegs* JitEa__points_at(const JitEaVirtual* v, int32_t pc);

/*
 * True when the class state still matches what the analysis assumed for an
 * object virtual: object.__new__, the analyzed __init__, no attribute hooks and
 * no data descriptor shadowing a field. Requires a current VM.
 */
bool JitEa__object_valid(const JitEaVirtual* v);

static inline bool JitEaRegs__has(const JitEaRegs* s, unsigned r) {
    return (s->bits[r >> 6] >> (r & 63)) & 1u;
}

#ifdef __cplusplus
}  // extern "C"
#endif
