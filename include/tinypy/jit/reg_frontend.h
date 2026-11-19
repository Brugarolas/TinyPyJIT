#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/interpreter/regbytecode.h"
#include "tinypy/jit/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JitRegLowerStatus {
    JIT_REG_LOWER_OK = 0,
    JIT_REG_LOWER_INVALID_ARGUMENT,
    JIT_REG_LOWER_INVALID_BYTECODE,
    JIT_REG_LOWER_TOO_LARGE,
    JIT_REG_LOWER_OUT_OF_MEMORY,
    JIT_REG_LOWER_VERIFY_FAILED,
} JitRegLowerStatus;

typedef struct JitRegLowerError {
    JitRegLowerStatus status;
    int32_t reg_pc;
    char message[256];
} JitRegLowerError;

typedef struct JitRegFrontendOptions {
    uint32_t estimated_execution_count;
    uint32_t max_phi_cells;
    bool poll_loop_headers;
    bool verify;
} JitRegFrontendOptions;

typedef struct JitRegFrontendStats {
    uint32_t bytecode_words;
    uint32_t reachable_blocks;
    uint32_t lowered_instructions;
    uint32_t phi_count;
    uint32_t side_exits;
    uint32_t backedges;
    uint32_t poll_count;
} JitRegFrontendStats;

void jit_reg_frontend_options_init(JitRegFrontendOptions* options);
const char* jit_reg_lower_status_name(JitRegLowerStatus status);

/*
 * Lower RegVM's register bytecode to the existing SSA MIR.
 *
 * The CodeObject is used for constants and source lines; it may be NULL only
 * when the RegCode contains no constant-pool loads. All source.bytecode_ip and
 * JitState.bytecode_ip fields refer to RegVM PC, not stack-bytecode IP.
 *
 * out_region is initialized on success only. The caller owns it and must call
 * jit_region_destroy(). Unsupported opcodes become explicit side exits with
 * the current register state. This frontend does not install or execute JIT code.
 */
bool jit_lower_regcode_to_mir(const RegCode* code,
                              const CodeObject* source,
                              const JitRegFrontendOptions* options,
                              JitRegion* out_region,
                              JitRegFrontendStats* stats,
                              JitRegLowerError* error);

#ifdef __cplusplus
}
#endif
