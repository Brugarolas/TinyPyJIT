#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/common/vector.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PK_JIT_FUNCTION_THRESHOLD
#define PK_JIT_FUNCTION_THRESHOLD 10u
#endif

#ifndef PK_JIT_LOOP_THRESHOLD
#define PK_JIT_LOOP_THRESHOLD 10u
#endif

typedef enum JitHotState {
    JIT_HOT_COLD = 0,
    JIT_HOT_COMPILING,
    JIT_HOT_COMPILED,
    JIT_HOT_DISABLED,
} JitHotState;

typedef struct JitBranchProfile {
    uint32_t taken;
    uint32_t not_taken;
} JitBranchProfile;

typedef struct JitTypeProfile {
    int32_t first_type;
    int32_t second_type;
    uint32_t first_count;
    uint32_t second_count;
    uint32_t other_count;
} JitTypeProfile;

typedef struct JitCallProfile {
    uintptr_t first_target;
    uintptr_t second_target;
    uint32_t first_count;
    uint32_t second_count;
    uint32_t other_count;
} JitCallProfile;

typedef struct JitLoopProfile {
    int32_t header_ip;
    uint16_t counter;
    uint16_t reserved;
    JitHotState state;
    uint32_t entries;
    uint32_t backedges;
} JitLoopProfile;

typedef struct JitCodeProfile {
    uint16_t function_counter;
    uint16_t reserved;
    JitHotState function_state;
    uint32_t calls;
    c11_vector loops; /* JitLoopProfile */
} JitCodeProfile;

void jit_code_profile_init(JitCodeProfile* profile);
void jit_code_profile_destroy(JitCodeProfile* profile);

/* Returns true exactly when the function first reaches the compilation threshold. */
bool jit_profile_function_entry(JitCodeProfile* profile);

/* Returns true exactly when this loop first reaches the compilation threshold. */
bool jit_profile_loop_backedge(JitCodeProfile* profile, int32_t header_ip);

JitLoopProfile* jit_profile_find_loop(JitCodeProfile* profile, int32_t header_ip);

void jit_branch_profile_record(JitBranchProfile* profile, bool taken);
void jit_type_profile_record(JitTypeProfile* profile, int32_t type);
void jit_call_profile_record(JitCallProfile* profile, uintptr_t target);

uint32_t jit_branch_probability_16(const JitBranchProfile* profile);
int32_t jit_type_profile_dominant(const JitTypeProfile* profile, uint32_t* confidence_16);
uintptr_t jit_call_profile_dominant(const JitCallProfile* profile, uint32_t* confidence_16);

#ifdef __cplusplus
}
#endif
