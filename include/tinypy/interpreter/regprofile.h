#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "tinypy/interpreter/regbytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PK_JIT_FUNCTION_THRESHOLD
#define PK_JIT_FUNCTION_THRESHOLD 10u
#endif

#ifndef PK_JIT_LOOP_THRESHOLD
#define PK_JIT_LOOP_THRESHOLD 10u
#endif

#define REG_PROFILE_NONE UINT16_MAX

typedef enum RegJitState {
    REG_JIT_COLD = 0,
    REG_JIT_REQUESTED,
    REG_JIT_COMPILED,
    REG_JIT_DISABLED,
} RegJitState;

typedef struct RegBranchProfile {
    uint32_t taken;
    uint32_t not_taken;
} RegBranchProfile;

typedef struct RegTypePairProfile {
    uint32_t first_pair;
    uint32_t second_pair;
    uint32_t first_count;
    uint32_t second_count;
    uint32_t other_count;
} RegTypePairProfile;

typedef struct RegCallTargetProfile {
    uintptr_t first_target;
    uintptr_t second_target;
    uint32_t first_count;
    uint32_t second_count;
    uint32_t other_count;
} RegCallTargetProfile;

typedef struct RegShapeObservation {
    uintptr_t shape;
    py_Type type;
    uint32_t count;
} RegShapeObservation;

typedef struct RegShapeProfile {
    RegShapeObservation first;
    RegShapeObservation second;
    uint32_t other_count;
} RegShapeProfile;

typedef struct RegProfileSite {
    int32_t pc;
    int32_t source_ip;
    uint32_t executions;
    uint32_t ic_hits;
    uint32_t ic_misses;
    RegBranchProfile branch;
    RegTypePairProfile types;
    RegCallTargetProfile calls;
    RegShapeProfile shapes;
} RegProfileSite;

typedef struct RegLoopProfile {
    int32_t header_pc;
    int32_t source_ip;
    uint32_t entries;
    uint32_t backedges;
    uint16_t hotness;
    uint16_t reserved;
    RegJitState state;
} RegLoopProfile;

typedef struct RegProfileView {
    const RegCode* code;
    const RegProfileSite* sites;
    int site_count;
    const RegLoopProfile* loops;
    int loop_count;
    uint32_t function_entries;
    uint16_t function_hotness;
    RegJitState function_state;
} RegProfileView;

/* Profile lifetime is owned by RegCode. */
void RegProfile__reset(RegCode* code);

/* Returns true while the function is hot and still eligible for JIT compilation. */
bool RegProfile__function_entry(RegCode* code);

/* Records a taken backedge and returns the stable loop profile for this edge. */
RegLoopProfile* RegProfile__loop_backedge(RegCode* code,
                                          int32_t branch_pc,
                                          int32_t header_pc,
                                          int32_t source_ip);

void RegProfile__record_type_pair(RegCode* code, int32_t pc, py_Type lhs, py_Type rhs);
void RegProfile__record_branch(RegCode* code, int32_t pc, bool taken);
void RegProfile__record_call(RegCode* code, int32_t pc, uintptr_t target);
void RegProfile__record_shape(RegCode* code, int32_t pc, py_Type type, uintptr_t shape);
void RegProfile__record_ic_hit(RegCode* code, int32_t pc);
void RegProfile__record_ic_miss(RegCode* code, int32_t pc);

const RegProfileSite* RegProfile__site(const RegCode* code, int32_t pc);
const RegLoopProfile* RegProfile__loop(const RegCode* code, int32_t header_pc);
const RegInlineCache* RegProfile__inline_cache(const RegCode* code, int32_t pc);

void RegProfile__view(const RegCode* code, RegProfileView* out);

uint32_t RegProfile__branch_probability_16(const RegBranchProfile* profile);
uint32_t RegProfile__type_pair_confidence_16(const RegTypePairProfile* profile,
                                             uint32_t* pair_out);
uint32_t RegProfile__call_confidence_16(const RegCallTargetProfile* profile,
                                       uintptr_t* target_out);
uint32_t RegProfile__shape_confidence_16(const RegShapeProfile* profile,
                                        py_Type* type_out,
                                        uintptr_t* shape_out);

#ifdef __cplusplus
}  // extern "C"
#endif
