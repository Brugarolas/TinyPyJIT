#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pocketpy/jit/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*JitOptimizationFn)(JitRegion* region);

typedef struct JitOptimizationPass {
    const char* name;
    JitOptimizationFn run;
} JitOptimizationPass;

typedef struct JitOptimizer {
    c11_vector passes; /* JitOptimizationPass */
    bool verify_after_each_pass;
    uint32_t max_rounds;
} JitOptimizer;

void jit_optimizer_init(JitOptimizer* optimizer);
void jit_optimizer_destroy(JitOptimizer* optimizer);
void jit_optimizer_add(JitOptimizer* optimizer, const char* name, JitOptimizationFn run);
void jit_optimizer_add_default_pipeline(JitOptimizer* optimizer);
bool jit_optimizer_run(JitOptimizer* optimizer, JitRegion* region, char* error, uint32_t error_size);

bool jit_opt_canonicalize(JitRegion* region);
bool jit_opt_type_propagation(JitRegion* region);
bool jit_opt_constant_fold(JitRegion* region);
bool jit_opt_peephole(JitRegion* region);
bool jit_opt_phi_simplify(JitRegion* region);
bool jit_opt_gvn(JitRegion* region);
bool jit_opt_branch_fold(JitRegion* region);
bool jit_opt_dce(JitRegion* region);

#ifdef __cplusplus
}
#endif
