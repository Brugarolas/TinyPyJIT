#pragma once

/*
 * Escape analysis + scalar replacement (SROA) over RegVM register bytecode.
 *
 * A candidate allocation is a BUILD_TUPLE, a BUILD_LIST, or a CALL whose
 * CONSTRUCT inline cache says it instantiates a Python class with
 * object.__new__ and an __init__ that only stores its arguments or scalar
 * constants into fields of self.
 *
 * A forward dataflow over the whole CFG computes, before every PC, the set of
 * registers that MAY name the new object (union at merges): MOVE propagates
 * it, any other write removes the register, and reaching the allocation again
 * restarts it. Every read of such a register must be one of these uses:
 *
 *   UNPACK_SEQUENCE     of a tuple/list with the matching length;
 *   LOAD_SUBSCR         of a tuple/list with a constant (LOAD_I16) index;
 *   CALL len(x)         of a tuple/list, when the site's IC says it calls len;
 *   LOAD_ATTR[_IC]      of an object field written by __init__;
 *   STORE_ATTR[_IC]     of an object field written by __init__ (field write);
 *   MOVE                (another register names the object).
 *
 * Anything else that reads such a register is an escape and keeps the real
 * allocation.
 *
 * At run time the native code keeps, per virtual object, an alias mask with
 * one bit per register that may name it (alias_regs): the allocation sets it to
 * the result register, MOVE copies the bit, other writes clear it. The object
 * itself never exists; its slots (tuple items, or object fields) live in a
 * GC-rooted save area. A use whose register bit is clear operates on whatever
 * real object the register holds instead, through the interpreter's own helper.
 * Leaving native code (side exit) or a frame introspection (RegJit__force_frame)
 * materializes the real object into exactly the registers whose bits are set,
 * so the interpreter always observes the unoptimized state - whatever path led
 * there.
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
#define JIT_EA_MAX_ALIASES 64
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
    JIT_EA_BUILD,   /* the allocation: fill the slots, alias mask = {result} */
    JIT_EA_UNPACK,  /* UNPACK_SEQUENCE: copy every slot */
    JIT_EA_SUBSCR,  /* LOAD_SUBSCR with a constant index: copy slot `arg` */
    JIT_EA_LEN,     /* CALL len(x): the item count */
    JIT_EA_ATTR,    /* LOAD_ATTR of field `arg` */
    JIT_EA_SETATTR, /* STORE_ATTR of field `arg` */
} JitEaAction;

/* A 256-bit register set. */
typedef struct JitEaRegs {
    uint64_t bits[4];
} JitEaRegs;

/* An object field as __init__ leaves it: a constructor argument or a constant. */
typedef struct JitEaField {
    py_Name name;
    int8_t component; /* argument index (0 = first after self), or -1 for `constant` */
    py_TValue constant;
} JitEaField;

typedef struct JitEaVirtual {
    JitEaKind kind;
    int32_t build_pc;
    uint8_t result_reg;
    uint8_t component_base;  /* first component register at the build */
    uint8_t component_count; /* tuple/list items, or constructor arguments */
    uint8_t slot_count;      /* save-area slots: tuple/list items, or object fields */
    uint16_t save_offset;    /* save-area index of slot 0 */

    /* JIT_EA_OBJECT */
    py_Type type;
    FuncDecl* init_decl;     /* referenced (+1) while the result lives */
    uint8_t field_count;
    JitEaField fields[JIT_EA_MAX_FIELDS]; /* in __init__'s first-store order */

    /* Runtime alias mask: bit i stands for register alias_regs[i]. */
    uint8_t alias_count;
    uint8_t alias_regs[JIT_EA_MAX_ALIASES];
    int8_t reg_bit[256];     /* register -> bit, or -1 */
    uint64_t* maybe;         /* per PC: alias bits that may be set before it */
    uint64_t* clear;         /* per PC: set-able alias bits the instruction overwrites (not MOVE) */
} JitEaVirtual;

#define JIT_EA_MAX_SITE_VIRTUALS 4

/*
 * What happens at one PC. An allocation names exactly one virtual object; a use
 * may be reached by several (e.g. `t = (a, b) if c else (b, a); x, y = t`), and
 * tests their alias bits in turn.
 */
typedef struct JitEaSite {
    uint8_t action;  /* JitEaAction */
    uint8_t reg;     /* the register through which a use reaches the object */
    uint8_t count;   /* virtual objects that may reach this PC */
    /*
     * Fused with the `MOVE reg <- fused_src` right before it (same block, reg
     * overwritten or dead after the use): the use tests fused_src's alias bit
     * and that MOVE only copies the register, without alias bookkeeping.
     */
    uint8_t fused;
    uint8_t fused_src;
    uint8_t virtual_index[JIT_EA_MAX_SITE_VIRTUALS];
    uint8_t arg[JIT_EA_MAX_SITE_VIRTUALS]; /* slot or field index, per object */
} JitEaSite;

typedef struct JitEaResult {
    int virtual_count;
    uint16_t save_count;
    JitEaVirtual virtuals[JIT_EA_MAX_VIRTUALS];
    JitEaSite* sites;     /* one per PC */
    uint8_t* live;        /* per PC: some virtual object may exist before it */
    int site_count;
    py_CFunction len_cfunc; /* builtin len (JIT_EA_LEN guards) */
} JitEaResult;

/*
 * Analyze one compiled body. Never fails the compilation: on any problem the
 * result simply has fewer (or no) virtual objects. `out` must be destroyed with
 * JitEa__dtor().
 */
void JitEa__analyze(const RegCode* code, const CodeObject* co, JitEaResult* out);
void JitEa__dtor(JitEaResult* self);

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
