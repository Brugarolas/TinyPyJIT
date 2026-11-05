#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/jit/frontend.h"
#include "tinypy/jit/ir.h"
#include "tinypy/jit/lir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JitBackendStatus {
    JIT_BACKEND_OK = 0,
    JIT_BACKEND_INVALID_ARGUMENT,
    JIT_BACKEND_INVALID_MIR,
    JIT_BACKEND_UNSUPPORTED_MIR,
    JIT_BACKEND_PHI_ERROR,
    JIT_BACKEND_VERIFY_FAILED,
    JIT_BACKEND_OUT_OF_MEMORY,
} JitBackendStatus;

typedef struct JitBackendError {
    JitBackendStatus status;
    JitInstId mir_inst;
    JitBlockId mir_block;
    char message[256];
} JitBackendError;

typedef struct JitBackendOptions {
    uint32_t tvalue_size;
    uint32_t stack_alignment;
    bool resolve_phis;
    bool run_peephole;
    bool verify;
} JitBackendOptions;

typedef struct JitBackendStats {
    uint32_t mir_blocks;
    uint32_t lir_blocks;
    uint32_t mir_insts;
    uint32_t lir_insts;
    uint32_t vregs;
    uint32_t value_vregs;
    uint32_t phi_copies;
    uint32_t critical_edges_split;
    uint32_t runtime_ops;
    uint32_t native_ops;
    uint32_t states;
} JitBackendStats;

void jit_backend_options_init(JitBackendOptions* options);
const char* jit_backend_status_name(JitBackendStatus status);

bool jit_lower_mir_to_lir(const JitRegion* region,
                          const JitBackendOptions* options,
                          JitLirFunction* out_lir,
                          JitBackendStats* out_stats,
                          JitBackendError* error);

#ifdef __cplusplus
}
#endif
