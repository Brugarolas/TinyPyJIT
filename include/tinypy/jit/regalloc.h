#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/jit/lir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JitLocationKind {
    JIT_LOCATION_NONE = 0,
    JIT_LOCATION_REGISTER,
    JIT_LOCATION_STACK,
} JitLocationKind;

typedef struct JitLocation {
    JitLocationKind kind;
    JitPhysReg preg;
    JitStackSlotId stack_slot;
} JitLocation;

typedef struct JitGraphColorConfig {
    uint16_t gpr_scratch_count;
    uint16_t gpr_saved_count;
    uint16_t fpr_scratch_count;
    uint16_t fpr_saved_count;

    /* The first N registers in each bank are reserved for the emitter/runtime. */
    uint16_t reserved_gpr_scratch;
    uint16_t reserved_gpr_saved;
    uint16_t reserved_fpr_scratch;
    uint16_t reserved_fpr_saved;

    uint32_t tvalue_size;
    uint32_t tvalue_alignment;
    uint32_t stack_alignment;

    uint32_t max_spill_rounds;
    bool enable_coalescing;
} JitGraphColorConfig;

typedef struct JitRegAllocStats {
    uint32_t initial_vregs;
    uint32_t final_vregs;
    uint32_t interference_edges;
    uint32_t move_pairs;
    uint32_t coalesced_pairs;
    uint32_t spill_rounds;
    uint32_t spilled_vregs;
    uint32_t spill_loads;
    uint32_t spill_stores;
    uint32_t value_homes;
    uint32_t stack_slots;
    uint32_t stack_bytes;
} JitRegAllocStats;

typedef struct JitRegAllocResult {
    c11_vector locations; /* JitLocation indexed by JitVRegId */
    JitRegAllocStats stats;
} JitRegAllocResult;

void jit_graph_color_config_init(JitGraphColorConfig* config);
void jit_regalloc_result_init(JitRegAllocResult* result);
void jit_regalloc_result_destroy(JitRegAllocResult* result);

/*
 * Mutates LIR when spilling: spilled virtual registers are given stack slots,
 * short-lived reload/store temporaries are inserted, and the graph is rebuilt.
 */
bool jit_graph_color_allocate(JitLirFunction* lir,
                              const JitGraphColorConfig* config,
                              JitRegAllocResult* result,
                              char* error,
                              uint32_t error_size);

/* Rewrite every VREG operand/state entry into its final physical register or stack home. */
bool jit_regalloc_apply(JitLirFunction* lir,
                        const JitRegAllocResult* result,
                        char* error,
                        uint32_t error_size);

#ifdef __cplusplus
}
#endif
