#include "tinypy/jit/regalloc.h"

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MovePair {
    JitVRegId dst;
    JitVRegId src;
} MovePair;

typedef struct Liveness {
    uint32_t block_count;
    uint32_t vreg_count;
    uint32_t words;
    uint64_t* use;
    uint64_t* def;
    uint64_t* in;
    uint64_t* out;
} Liveness;

typedef struct ColorGraph {
    uint32_t n;
    uint32_t words;
    uint64_t* adjacency;
    bool* allocatable;
    bool* crosses_call;
    uint64_t* allowed;
    double* cost;
    uint32_t* parent;
    int32_t* color;
    bool* spilled;
    c11_vector moves; /* MovePair */
    uint32_t edge_count;
} ColorGraph;

typedef struct Replacement {
    JitVRegId original;
    JitLirOperand temp;
    bool loaded;
    bool stored;
} Replacement;

static bool failf(char* error, uint32_t error_size, const char* fmt, ...) {
    if(error != NULL && error_size != 0u) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_size, fmt, args);
        va_end(args);
        error[error_size - 1u] = '\0';
    }
    return false;
}

static JitImmediate imm_zero(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

static uint32_t popcount64(uint64_t value) {
    uint32_t count = 0u;
    while(value != 0u) {
        value &= value - 1u;
        count++;
    }
    return count;
}

static uint32_t ctz64(uint64_t value) {
    uint32_t n = 0u;
    while((value & 1u) == 0u) {
        value >>= 1u;
        n++;
    }
    return n;
}

void jit_graph_color_config_init(JitGraphColorConfig* config) {
    if(config == NULL) return;
    config->gpr_scratch_count = 8;
    config->gpr_saved_count = 4;
    config->fpr_scratch_count = 8;
    config->fpr_saved_count = 4;
    config->reserved_gpr_scratch = 0;
    config->reserved_gpr_saved = 0;
    config->reserved_fpr_scratch = 0;
    config->reserved_fpr_saved = 0;
    config->tvalue_size = 32u;
    config->tvalue_alignment = 8u;
    config->stack_alignment = 16u;
    config->max_spill_rounds = 8u;
    config->enable_coalescing = true;
}

void jit_regalloc_result_init(JitRegAllocResult* result) {
    if(result == NULL) return;
    c11_vector__ctor(&result->locations, sizeof(JitLocation));
    memset(&result->stats, 0, sizeof(result->stats));
}

void jit_regalloc_result_destroy(JitRegAllocResult* result) {
    if(result == NULL) return;
    c11_vector__dtor(&result->locations);
    memset(result, 0, sizeof(*result));
}

static void ensure_location_count(JitRegAllocResult* result, uint32_t count) {
    while((uint32_t)result->locations.length < count) {
        JitLocation loc;
        memset(&loc, 0, sizeof(loc));
        loc.kind = JIT_LOCATION_NONE;
        loc.preg.bank = JIT_PHYS_NONE;
        loc.stack_slot = JIT_INVALID_STACK_SLOT;
        c11_vector__push(JitLocation, &result->locations, loc);
    }
}

static bool allocatable_vreg(const JitLirFunction* lir, JitVRegId id) {
    if(id >= (JitVRegId)lir->vregs.length) return false;
    const JitVReg* vreg = c11__at(JitVReg, &lir->vregs, id);
    if(vreg->force_stack) return false;
    return vreg->reg_class == JIT_VREG_GPR || vreg->reg_class == JIT_VREG_FPR;
}

static bool operand_allocatable(const JitLirFunction* lir, JitLirOperand operand) {
    return operand.kind == JIT_LIR_OPERAND_VREG && allocatable_vreg(lir, operand.as.vreg);
}

static uint64_t* row(uint64_t* base, uint32_t words, uint32_t index) {
    return base + (size_t)words * index;
}

static const uint64_t* row_const(const uint64_t* base, uint32_t words, uint32_t index) {
    return base + (size_t)words * index;
}

static void bitset_set(uint64_t* bits, uint32_t bit) {
    bits[bit >> 6] |= UINT64_C(1) << (bit & 63u);
}

static void bitset_clear(uint64_t* bits, uint32_t bit) {
    bits[bit >> 6] &= ~(UINT64_C(1) << (bit & 63u));
}

static bool bitset_test(const uint64_t* bits, uint32_t bit) {
    return (bits[bit >> 6] & (UINT64_C(1) << (bit & 63u))) != 0u;
}

static bool bitset_equal(const uint64_t* a, const uint64_t* b, uint32_t words) {
    return memcmp(a, b, sizeof(uint64_t) * words) == 0;
}

static void bitset_copy(uint64_t* dst, const uint64_t* src, uint32_t words) {
    memcpy(dst, src, sizeof(uint64_t) * words);
}

static void bitset_or(uint64_t* dst, const uint64_t* src, uint32_t words) {
    for(uint32_t i = 0; i < words; i++) dst[i] |= src[i];
}

static void bitset_andnot(uint64_t* dst, const uint64_t* remove, uint32_t words) {
    for(uint32_t i = 0; i < words; i++) dst[i] &= ~remove[i];
}

static bool liveness_init(Liveness* live, uint32_t blocks, uint32_t vregs) {
    memset(live, 0, sizeof(*live));
    live->block_count = blocks;
    live->vreg_count = vregs;
    live->words = (vregs + 63u) / 64u;
    if(live->words == 0u || blocks == 0u) return true;
    size_t count = (size_t)blocks * live->words;
    live->use = calloc(count, sizeof(uint64_t));
    live->def = calloc(count, sizeof(uint64_t));
    live->in = calloc(count, sizeof(uint64_t));
    live->out = calloc(count, sizeof(uint64_t));
    if(live->use == NULL || live->def == NULL || live->in == NULL || live->out == NULL) {
        free(live->use);
        free(live->def);
        free(live->in);
        free(live->out);
        memset(live, 0, sizeof(*live));
        return false;
    }
    return true;
}

static void liveness_destroy(Liveness* live) {
    free(live->use);
    free(live->def);
    free(live->in);
    free(live->out);
    memset(live, 0, sizeof(*live));
}

static void add_block_use(const JitLirFunction* lir,
                          uint64_t* use,
                          const uint64_t* def,
                          JitLirOperand operand) {
    if(!operand_allocatable(lir, operand)) return;
    JitVRegId id = operand.as.vreg;
    if(!bitset_test(def, id)) bitset_set(use, id);
}

static void add_block_def(const JitLirFunction* lir, uint64_t* def, JitLirOperand operand) {
    if(!operand_allocatable(lir, operand)) return;
    bitset_set(def, operand.as.vreg);
}

static void visit_state_operands(const JitLirFunction* lir,
                                 JitLirStateId state_id,
                                 void (*visit)(const JitLirFunction*, void*, JitLirOperand),
                                 void* ctx) {
    if(state_id == JIT_INVALID_LIR_STATE || state_id >= (JitLirStateId)lir->states.length) return;
    const JitLirState* state = c11__at(JitLirState, &lir->states, state_id);
    const JitLirOperand* locals = jit_lir_state_locals(lir, state);
    const JitLirOperand* stack = jit_lir_state_stack(lir, state);
    for(uint32_t i = 0; i < state->locals.count; i++) visit(lir, ctx, locals[i]);
    for(uint32_t i = 0; i < state->stack.count; i++) visit(lir, ctx, stack[i]);
}

typedef struct BlockUseCtx {
    uint64_t* use;
    const uint64_t* def;
} BlockUseCtx;

static void state_block_use(const JitLirFunction* lir, void* opaque, JitLirOperand operand) {
    BlockUseCtx* ctx = opaque;
    add_block_use(lir, ctx->use, ctx->def, operand);
}

static bool build_liveness(JitLirFunction* lir, Liveness* live) {
    uint32_t blocks = (uint32_t)lir->blocks.length;
    uint32_t vregs = (uint32_t)lir->vregs.length;
    if(!liveness_init(live, blocks, vregs)) return false;
    if(live->words == 0u) return true;

    c11__foreach(JitVReg, &lir->vregs, vreg) vreg->spill_weight = 0.0;

    c11__foreach(JitLirBlock, &lir->blocks, block) {
        uint64_t* use = row(live->use, live->words, block->id);
        uint64_t* def = row(live->def, live->words, block->id);
        double weight = block->frequency == 0u ? 1.0 : (double)block->frequency;
        c11__foreach(JitLirInst, &block->insts, inst) {
            if(inst->removed || inst->op == JIT_LIR_NOP) continue;
            const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);
            const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
            for(uint32_t i = 0; i < inst->uses.count; i++) {
                add_block_use(lir, use, def, uses[i]);
                if(operand_allocatable(lir, uses[i]))
                    c11__at(JitVReg, &lir->vregs, uses[i].as.vreg)->spill_weight += weight;
            }
            BlockUseCtx state_ctx = {use, def};
            visit_state_operands(lir, inst->state, state_block_use, &state_ctx);
            if(inst->state != JIT_INVALID_LIR_STATE && inst->state < (JitLirStateId)lir->states.length) {
                const JitLirState* state = c11__at(JitLirState, &lir->states, inst->state);
                const JitLirOperand* locals = jit_lir_state_locals(lir, state);
                const JitLirOperand* stack = jit_lir_state_stack(lir, state);
                for(uint32_t i = 0; i < state->locals.count; i++) {
                    if(operand_allocatable(lir, locals[i]))
                        c11__at(JitVReg, &lir->vregs, locals[i].as.vreg)->spill_weight += weight * 0.25;
                }
                for(uint32_t i = 0; i < state->stack.count; i++) {
                    if(operand_allocatable(lir, stack[i]))
                        c11__at(JitVReg, &lir->vregs, stack[i].as.vreg)->spill_weight += weight * 0.25;
                }
            }
            for(uint32_t i = 0; i < inst->defs.count; i++) {
                add_block_def(lir, def, defs[i]);
                if(operand_allocatable(lir, defs[i]))
                    c11__at(JitVReg, &lir->vregs, defs[i].as.vreg)->spill_weight += weight * 0.5;
            }
        }
    }

    uint64_t* scratch = live->words == 0u ? NULL : malloc(sizeof(uint64_t) * live->words);
    uint64_t* new_in = live->words == 0u ? NULL : malloc(sizeof(uint64_t) * live->words);
    if(live->words != 0u && (scratch == NULL || new_in == NULL)) {
        free(scratch);
        free(new_in);
        liveness_destroy(live);
        return false;
    }

    bool changed;
    do {
        changed = false;
        for(int bi = (int)blocks - 1; bi >= 0; bi--) {
            const JitLirBlock* block = c11__at(JitLirBlock, &lir->blocks, bi);
            uint64_t* out = row(live->out, live->words, (uint32_t)bi);
            uint64_t* in = row(live->in, live->words, (uint32_t)bi);
            memset(scratch, 0, sizeof(uint64_t) * live->words);
            c11__foreach(JitLirBlockId, &block->successors, succ) {
                if(*succ < blocks) bitset_or(scratch, row_const(live->in, live->words, *succ), live->words);
            }
            if(!bitset_equal(out, scratch, live->words)) {
                bitset_copy(out, scratch, live->words);
                changed = true;
            }
            bitset_copy(new_in, out, live->words);
            bitset_andnot(new_in, row_const(live->def, live->words, (uint32_t)bi), live->words);
            bitset_or(new_in, row_const(live->use, live->words, (uint32_t)bi), live->words);
            if(!bitset_equal(in, new_in, live->words)) {
                bitset_copy(in, new_in, live->words);
                changed = true;
            }
        }
    } while(changed);

    free(scratch);
    free(new_in);
    return true;
}

static bool graph_init(ColorGraph* graph, uint32_t n) {
    memset(graph, 0, sizeof(*graph));
    graph->n = n;
    graph->words = (n + 63u) / 64u;
    c11_vector__ctor(&graph->moves, sizeof(MovePair));
    size_t matrix_words = (size_t)n * graph->words;
    if(matrix_words != 0u) graph->adjacency = calloc(matrix_words, sizeof(uint64_t));
    graph->allocatable = calloc(n == 0u ? 1u : n, sizeof(bool));
    graph->crosses_call = calloc(n == 0u ? 1u : n, sizeof(bool));
    graph->allowed = calloc(n == 0u ? 1u : n, sizeof(uint64_t));
    graph->cost = calloc(n == 0u ? 1u : n, sizeof(double));
    graph->parent = malloc(sizeof(uint32_t) * (n == 0u ? 1u : n));
    graph->color = malloc(sizeof(int32_t) * (n == 0u ? 1u : n));
    graph->spilled = calloc(n == 0u ? 1u : n, sizeof(bool));
    if((matrix_words != 0u && graph->adjacency == NULL) || graph->allocatable == NULL ||
       graph->crosses_call == NULL || graph->allowed == NULL || graph->cost == NULL ||
       graph->parent == NULL || graph->color == NULL || graph->spilled == NULL) {
        return false;
    }
    for(uint32_t i = 0; i < n; i++) {
        graph->parent[i] = i;
        graph->color[i] = -1;
    }
    return true;
}

static void graph_destroy(ColorGraph* graph) {
    free(graph->adjacency);
    free(graph->allocatable);
    free(graph->crosses_call);
    free(graph->allowed);
    free(graph->cost);
    free(graph->parent);
    free(graph->color);
    free(graph->spilled);
    c11_vector__dtor(&graph->moves);
    memset(graph, 0, sizeof(*graph));
}

static uint32_t find_root(ColorGraph* graph, uint32_t node) {
    uint32_t root = node;
    while(graph->parent[root] != root) root = graph->parent[root];
    while(graph->parent[node] != node) {
        uint32_t next = graph->parent[node];
        graph->parent[node] = root;
        node = next;
    }
    return root;
}

static bool graph_has_edge(const ColorGraph* graph, uint32_t a, uint32_t b) {
    if(a == b || graph->words == 0u) return false;
    return bitset_test(row_const(graph->adjacency, graph->words, a), b);
}

static void graph_add_edge(ColorGraph* graph, uint32_t a, uint32_t b) {
    if(a == b || a >= graph->n || b >= graph->n) return;
    if(graph_has_edge(graph, a, b)) return;
    bitset_set(row(graph->adjacency, graph->words, a), b);
    bitset_set(row(graph->adjacency, graph->words, b), a);
    graph->edge_count++;
}

static void mark_live_crosses_call(ColorGraph* graph, const uint64_t* live) {
    for(uint32_t w = 0; w < graph->words; w++) {
        uint64_t bits = live[w];
        while(bits != 0u) {
            uint32_t bit = ctz64(bits);
            uint32_t id = (w << 6) + bit;
            if(id < graph->n && graph->allocatable[id]) graph->crosses_call[id] = true;
            bits &= bits - 1u;
        }
    }
}

static void add_state_to_live(const JitLirFunction* lir,
                              JitLirStateId state_id,
                              uint64_t* live,
                              const ColorGraph* graph) {
    if(state_id == JIT_INVALID_LIR_STATE || state_id >= (JitLirStateId)lir->states.length) return;
    const JitLirState* state = c11__at(JitLirState, &lir->states, state_id);
    const JitLirOperand* locals = jit_lir_state_locals(lir, state);
    const JitLirOperand* stack = jit_lir_state_stack(lir, state);
    for(uint32_t i = 0; i < state->locals.count; i++) {
        if(locals[i].kind == JIT_LIR_OPERAND_VREG && locals[i].as.vreg < graph->n &&
           graph->allocatable[locals[i].as.vreg])
            bitset_set(live, locals[i].as.vreg);
    }
    for(uint32_t i = 0; i < state->stack.count; i++) {
        if(stack[i].kind == JIT_LIR_OPERAND_VREG && stack[i].as.vreg < graph->n &&
           graph->allocatable[stack[i].as.vreg])
            bitset_set(live, stack[i].as.vreg);
    }
}

static bool build_interference(JitLirFunction* lir,
                               const JitGraphColorConfig* config,
                               const Liveness* live,
                               ColorGraph* graph,
                               JitRegAllocStats* stats) {
    if(!graph_init(graph, (uint32_t)lir->vregs.length)) return false;
    for(uint32_t i = 0; i < graph->n; i++) {
        const JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
        graph->allocatable[i] = allocatable_vreg(lir, i);
        graph->cost[i] = vreg->spill_weight <= 0.0 ? 0.001 : vreg->spill_weight;
    }

    uint64_t* current = graph->words == 0u ? NULL : malloc(sizeof(uint64_t) * graph->words);
    if(graph->words != 0u && current == NULL) {
        graph_destroy(graph);
        return false;
    }

    for(int bi = lir->blocks.length - 1; bi >= 0; bi--) {
        const JitLirBlock* block = c11__at(JitLirBlock, &lir->blocks, bi);
        if(graph->words != 0u)
            bitset_copy(current, row_const(live->out, live->words, block->id), graph->words);
        for(int ii = block->insts.length - 1; ii >= 0; ii--) {
            const JitLirInst* inst = c11__at(JitLirInst, &block->insts, ii);
            if(inst->removed || inst->op == JIT_LIR_NOP) continue;

            if((inst->flags & JIT_LIR_FLAG_CALL) != 0u) mark_live_crosses_call(graph, current);

            const JitLirOperand* defs = jit_lir_inst_defs(lir, inst);
            const JitLirOperand* uses = jit_lir_inst_uses(lir, inst);

            /* Multiple results are simultaneously live after the instruction. */
            for(uint32_t i = 0; i < inst->defs.count; i++) {
                if(!operand_allocatable(lir, defs[i])) continue;
                for(uint32_t j = i + 1u; j < inst->defs.count; j++) {
                    if(operand_allocatable(lir, defs[j])) graph_add_edge(graph, defs[i].as.vreg, defs[j].as.vreg);
                }
            }

            JitVRegId copy_src = JIT_INVALID_VREG;
            JitVRegId copy_dst = JIT_INVALID_VREG;
            if(inst->op == JIT_LIR_COPY && inst->defs.count == 1u && inst->uses.count == 1u &&
               operand_allocatable(lir, defs[0]) && operand_allocatable(lir, uses[0]) &&
               c11__at(JitVReg, &lir->vregs, defs[0].as.vreg)->reg_class ==
                   c11__at(JitVReg, &lir->vregs, uses[0].as.vreg)->reg_class) {
                copy_dst = defs[0].as.vreg;
                copy_src = uses[0].as.vreg;
                MovePair pair = {copy_dst, copy_src};
                c11_vector__push(MovePair, &graph->moves, pair);
            }

            for(uint32_t i = 0; i < inst->defs.count; i++) {
                if(!operand_allocatable(lir, defs[i])) continue;
                JitVRegId def = defs[i].as.vreg;
                for(uint32_t w = 0; w < graph->words; w++) {
                    uint64_t bits = current[w];
                    while(bits != 0u) {
                        uint32_t bit = ctz64(bits);
                        JitVRegId other = (w << 6) + bit;
                        if(other < graph->n && graph->allocatable[other] &&
                           !(def == copy_dst && other == copy_src))
                            graph_add_edge(graph, def, other);
                        bits &= bits - 1u;
                    }
                }
            }

            for(uint32_t i = 0; i < inst->defs.count; i++) {
                if(operand_allocatable(lir, defs[i])) bitset_clear(current, defs[i].as.vreg);
            }
            for(uint32_t i = 0; i < inst->uses.count; i++) {
                if(operand_allocatable(lir, uses[i])) bitset_set(current, uses[i].as.vreg);
            }
            add_state_to_live(lir, inst->state, current, graph);
        }
    }
    free(current);

    uint32_t gs = config->gpr_scratch_count;
    uint32_t gv = config->gpr_saved_count;
    uint32_t fs = config->fpr_scratch_count;
    uint32_t fv = config->fpr_saved_count;
    uint64_t gpr_scratch = 0u;
    uint64_t gpr_saved = 0u;
    uint64_t fpr_scratch = 0u;
    uint64_t fpr_saved = 0u;
    for(uint32_t i = config->reserved_gpr_scratch; i < gs; i++) gpr_scratch |= UINT64_C(1) << i;
    for(uint32_t i = config->reserved_gpr_saved; i < gv; i++) gpr_saved |= UINT64_C(1) << (gs + i);
    for(uint32_t i = config->reserved_fpr_scratch; i < fs; i++) fpr_scratch |= UINT64_C(1) << i;
    for(uint32_t i = config->reserved_fpr_saved; i < fv; i++) fpr_saved |= UINT64_C(1) << (fs + i);

    for(uint32_t i = 0; i < graph->n; i++) {
        if(!graph->allocatable[i]) continue;
        const JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
        if(vreg->reg_class == JIT_VREG_GPR)
            graph->allowed[i] = graph->crosses_call[i] ? gpr_saved : (gpr_scratch | gpr_saved);
        else
            graph->allowed[i] = graph->crosses_call[i] ? fpr_saved : (fpr_scratch | fpr_saved);
    }

    stats->interference_edges += graph->edge_count;
    stats->move_pairs += (uint32_t)graph->moves.length;
    return true;
}

static bool groups_interfere(ColorGraph* graph, uint32_t a, uint32_t b) {
    a = find_root(graph, a);
    b = find_root(graph, b);
    if(a == b) return false;
    const uint64_t* adj = row_const(graph->adjacency, graph->words, a);
    for(uint32_t w = 0; w < graph->words; w++) {
        uint64_t bits = adj[w];
        while(bits != 0u) {
            uint32_t bit = ctz64(bits);
            uint32_t n = (w << 6) + bit;
            if(n < graph->n && find_root(graph, n) == b) return true;
            bits &= bits - 1u;
        }
    }
    return false;
}

static uint32_t union_degree(ColorGraph* graph, uint32_t a, uint32_t b, uint8_t* seen) {
    memset(seen, 0, graph->n);
    uint32_t count = 0u;
    uint32_t roots[2] = {find_root(graph, a), find_root(graph, b)};
    for(uint32_t r = 0; r < 2u; r++) {
        const uint64_t* adj = row_const(graph->adjacency, graph->words, roots[r]);
        for(uint32_t w = 0; w < graph->words; w++) {
            uint64_t bits = adj[w];
            while(bits != 0u) {
                uint32_t bit = ctz64(bits);
                uint32_t n = (w << 6) + bit;
                if(n < graph->n) {
                    uint32_t nr = find_root(graph, n);
                    if(nr != roots[0] && nr != roots[1] && graph->allocatable[nr] && !seen[nr]) {
                        seen[nr] = 1u;
                        count++;
                    }
                }
                bits &= bits - 1u;
            }
        }
    }
    return count;
}

static void merge_roots(ColorGraph* graph, uint32_t a, uint32_t b) {
    a = find_root(graph, a);
    b = find_root(graph, b);
    if(a == b) return;
    graph->parent[b] = a;
    graph->allowed[a] &= graph->allowed[b];
    graph->cost[a] += graph->cost[b];
    graph->crosses_call[a] = graph->crosses_call[a] || graph->crosses_call[b];
    uint64_t* arow = row(graph->adjacency, graph->words, a);
    const uint64_t* brow = row_const(graph->adjacency, graph->words, b);
    bitset_or(arow, brow, graph->words);
    bitset_clear(arow, a);
    bitset_clear(arow, b);
}

static void coalesce_graph(JitLirFunction* lir,
                           ColorGraph* graph,
                           JitRegAllocStats* stats,
                           bool enabled) {
    if(!enabled || graph->n == 0u) return;
    uint8_t* seen = malloc(graph->n);
    if(seen == NULL) return;
    c11__foreach(MovePair, &graph->moves, move) {
        uint32_t a = find_root(graph, move->dst);
        uint32_t b = find_root(graph, move->src);
        if(a == b || !graph->allocatable[a] || !graph->allocatable[b]) continue;
        const JitVReg* va = c11__at(JitVReg, &lir->vregs, a);
        const JitVReg* vb = c11__at(JitVReg, &lir->vregs, b);
        if(va->reg_class != vb->reg_class || groups_interfere(graph, a, b)) continue;
        uint64_t common = graph->allowed[a] & graph->allowed[b];
        uint32_t k = popcount64(common);
        if(k == 0u) continue;
        if(union_degree(graph, a, b, seen) >= k) continue;
        merge_roots(graph, a, b);
        stats->coalesced_pairs++;
    }
    free(seen);
}

static uint32_t active_degree(ColorGraph* graph, uint32_t root_id, const bool* active, uint8_t* seen) {
    memset(seen, 0, graph->n);
    uint32_t degree = 0u;
    const uint64_t* adj = row_const(graph->adjacency, graph->words, root_id);
    for(uint32_t w = 0; w < graph->words; w++) {
        uint64_t bits = adj[w];
        while(bits != 0u) {
            uint32_t bit = ctz64(bits);
            uint32_t n = (w << 6) + bit;
            if(n < graph->n) {
                uint32_t nr = find_root(graph, n);
                if(nr != root_id && active[nr] && !seen[nr]) {
                    seen[nr] = 1u;
                    degree++;
                }
            }
            bits &= bits - 1u;
        }
    }
    return degree;
}

static int32_t preferred_move_color(ColorGraph* graph, uint32_t root_id, uint64_t available) {
    c11__foreach(MovePair, &graph->moves, move) {
        uint32_t a = find_root(graph, move->dst);
        uint32_t b = find_root(graph, move->src);
        uint32_t other;
        if(a == root_id)
            other = b;
        else if(b == root_id)
            other = a;
        else
            continue;
        if(other < graph->n && graph->color[other] >= 0) {
            uint32_t color = (uint32_t)graph->color[other];
            if(color < 64u && (available & (UINT64_C(1) << color)) != 0u) return (int32_t)color;
        }
    }
    return -1;
}

static bool color_graph(ColorGraph* graph) {
    if(graph->n == 0u) return true;
    bool* active = calloc(graph->n, sizeof(bool));
    uint8_t* seen = malloc(graph->n);
    c11_vector stack;
    c11_vector__ctor(&stack, sizeof(uint32_t));
    if(active == NULL || seen == NULL) {
        free(active);
        free(seen);
        c11_vector__dtor(&stack);
        return false;
    }

    uint32_t remaining = 0u;
    for(uint32_t i = 0; i < graph->n; i++) {
        if(graph->allocatable[i] && find_root(graph, i) == i) {
            active[i] = true;
            remaining++;
        }
    }

    while(remaining != 0u) {
        int32_t chosen = -1;
        for(uint32_t i = 0; i < graph->n; i++) {
            if(!active[i]) continue;
            uint32_t k = popcount64(graph->allowed[i]);
            uint32_t degree = active_degree(graph, i, active, seen);
            if(k != 0u && degree < k) {
                chosen = (int32_t)i;
                break;
            }
        }
        if(chosen < 0) {
            double best = DBL_MAX;
            for(uint32_t i = 0; i < graph->n; i++) {
                if(!active[i]) continue;
                uint32_t degree = active_degree(graph, i, active, seen);
                double score = graph->allowed[i] == 0u ? -1.0 : graph->cost[i] / (double)(degree + 1u);
                if(score < best) {
                    best = score;
                    chosen = (int32_t)i;
                }
            }
        }
        if(chosen < 0) break;
        uint32_t node = (uint32_t)chosen;
        c11_vector__push(uint32_t, &stack, node);
        active[node] = false;
        remaining--;
    }

    for(int si = stack.length - 1; si >= 0; si--) {
        uint32_t node = c11__getitem(uint32_t, &stack, si);
        uint64_t available = graph->allowed[node];
        memset(seen, 0, graph->n);
        const uint64_t* adj = row_const(graph->adjacency, graph->words, node);
        for(uint32_t w = 0; w < graph->words; w++) {
            uint64_t bits = adj[w];
            while(bits != 0u) {
                uint32_t bit = ctz64(bits);
                uint32_t n = (w << 6) + bit;
                if(n < graph->n) {
                    uint32_t nr = find_root(graph, n);
                    if(nr != node && !seen[nr]) {
                        seen[nr] = 1u;
                        if(graph->color[nr] >= 0) {
                            uint32_t color = (uint32_t)graph->color[nr];
                            if(color < 64u) available &= ~(UINT64_C(1) << color);
                        }
                    }
                }
                bits &= bits - 1u;
            }
        }
        if(available == 0u) {
            graph->spilled[node] = true;
            continue;
        }
        int32_t preferred = preferred_move_color(graph, node, available);
        uint32_t color = preferred >= 0 ? (uint32_t)preferred : ctz64(available);
        graph->color[node] = (int32_t)color;
    }

    for(uint32_t i = 0; i < graph->n; i++) {
        if(!graph->allocatable[i]) continue;
        uint32_t root_id = find_root(graph, i);
        graph->color[i] = graph->color[root_id];
        graph->spilled[i] = graph->spilled[root_id];
    }

    free(active);
    free(seen);
    c11_vector__dtor(&stack);
    return true;
}

static JitPhysReg color_to_reg(const JitGraphColorConfig* config,
                               JitVRegClass cls,
                               uint32_t color) {
    JitPhysReg preg;
    preg.bank = JIT_PHYS_NONE;
    preg.index = 0;
    if(cls == JIT_VREG_GPR) {
        if(color < config->gpr_scratch_count) {
            preg.bank = JIT_PHYS_GPR_SCRATCH;
            preg.index = (uint16_t)color;
        } else {
            preg.bank = JIT_PHYS_GPR_SAVED;
            preg.index = (uint16_t)(color - config->gpr_scratch_count);
        }
    } else if(cls == JIT_VREG_FPR) {
        if(color < config->fpr_scratch_count) {
            preg.bank = JIT_PHYS_FPR_SCRATCH;
            preg.index = (uint16_t)color;
        } else {
            preg.bank = JIT_PHYS_FPR_SAVED;
            preg.index = (uint16_t)(color - config->fpr_scratch_count);
        }
    }
    return preg;
}

static bool validate_config(const JitGraphColorConfig* config,
                            char* error,
                            uint32_t error_size) {
    if(config->gpr_scratch_count + config->gpr_saved_count > 64u ||
       config->fpr_scratch_count + config->fpr_saved_count > 64u)
        return failf(error, error_size, "graph-color allocator supports at most 64 colors per register class");
    if(config->reserved_gpr_scratch > config->gpr_scratch_count ||
       config->reserved_gpr_saved > config->gpr_saved_count ||
       config->reserved_fpr_scratch > config->fpr_scratch_count ||
       config->reserved_fpr_saved > config->fpr_saved_count)
        return failf(error, error_size, "reserved register count exceeds available register count");
    if(config->tvalue_size == 0u || config->tvalue_alignment == 0u || config->stack_alignment == 0u)
        return failf(error, error_size, "stack/value sizes and alignments must be non-zero");
    return true;
}

static void assign_value_homes(JitLirFunction* lir,
                               const JitGraphColorConfig* config,
                               JitRegAllocResult* result) {
    ensure_location_count(result, (uint32_t)lir->vregs.length);
    for(uint32_t i = 0; i < (uint32_t)lir->vregs.length; i++) {
        JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
        if(vreg->reg_class != JIT_VREG_VALUE) continue;
        JitLocation* loc = c11__at(JitLocation, &result->locations, i);
        if(loc->kind != JIT_LOCATION_NONE) continue;
        JitStackSlotId slot = jit_lir_new_stack_slot(lir,
                                                     config->tvalue_size,
                                                     config->tvalue_alignment,
                                                     JIT_STACK_SLOT_VALUE_HOME);
        loc->kind = JIT_LOCATION_STACK;
        loc->stack_slot = slot;
        loc->preg.bank = JIT_PHYS_NONE;
        vreg->force_stack = true;
        result->stats.value_homes++;
    }
}

static JitLirInst make_inst(JitLirFunction* lir,
                            JitLirOpcode op,
                            const JitLirOperand* defs,
                            uint32_t def_count,
                            const JitLirOperand* uses,
                            uint32_t use_count,
                            const JitLirInst* model) {
    JitLirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op;
    inst.defs = jit_lir_append_operands(lir, defs, def_count);
    inst.uses = jit_lir_append_operands(lir, uses, use_count);
    if(model != NULL) {
        inst.imm = model->imm;
        inst.aux = model->aux;
        inst.aux2 = model->aux2;
        inst.aux3 = model->aux3;
        inst.flags = model->flags;
        inst.target = model->target;
        inst.target2 = model->target2;
        inst.state = model->state;
        inst.source = model->source;
    } else {
        inst.imm = imm_zero();
        inst.target = JIT_INVALID_LIR_BLOCK;
        inst.target2 = JIT_INVALID_LIR_BLOCK;
        inst.state = JIT_INVALID_LIR_STATE;
        inst.source = (JitSourceLocation){-1, -1};
    }
    return inst;
}

static Replacement* find_replacement(Replacement* replacements,
                                     uint32_t count,
                                     JitVRegId original) {
    for(uint32_t i = 0; i < count; i++) {
        if(replacements[i].original == original) return &replacements[i];
    }
    return NULL;
}

static bool rewrite_spills(JitLirFunction* lir,
                           const bool* spilled,
                           const JitStackSlotId* slots,
                           JitRegAllocResult* result) {
    for(uint32_t bi = 0; bi < (uint32_t)lir->blocks.length; bi++) {
        JitLirBlock* block = c11__at(JitLirBlock, &lir->blocks, bi);
        c11_vector new_insts;
        c11_vector__ctor(&new_insts, sizeof(JitLirInst));

        for(int ii = 0; ii < block->insts.length; ii++) {
            const JitLirInst* old = c11__at(JitLirInst, &block->insts, ii);
            if(old->removed || old->op == JIT_LIR_NOP) {
                c11_vector__push(JitLirInst, &new_insts, *old);
                continue;
            }
            const JitLirOperand* old_defs = jit_lir_inst_defs(lir, old);
            const JitLirOperand* old_uses = jit_lir_inst_uses(lir, old);
            JitLirOperand* defs = NULL;
            JitLirOperand* uses = NULL;
            if(old->defs.count != 0u) {
                defs = malloc(sizeof(JitLirOperand) * old->defs.count);
                if(defs == NULL) {
                    c11_vector__dtor(&new_insts);
                    return false;
                }
                memcpy(defs, old_defs, sizeof(JitLirOperand) * old->defs.count);
            }
            if(old->uses.count != 0u) {
                uses = malloc(sizeof(JitLirOperand) * old->uses.count);
                if(uses == NULL) {
                    free(defs);
                    c11_vector__dtor(&new_insts);
                    return false;
                }
                memcpy(uses, old_uses, sizeof(JitLirOperand) * old->uses.count);
            }

            uint32_t replacement_capacity = old->defs.count + old->uses.count;
            Replacement* replacements = replacement_capacity == 0u
                                            ? NULL
                                            : calloc(replacement_capacity, sizeof(Replacement));
            if(replacement_capacity != 0u && replacements == NULL) {
                free(defs);
                free(uses);
                c11_vector__dtor(&new_insts);
                return false;
            }
            uint32_t replacement_count = 0u;

            for(uint32_t ui = 0; ui < old->uses.count; ui++) {
                if(uses[ui].kind != JIT_LIR_OPERAND_VREG || uses[ui].as.vreg >= (JitVRegId)lir->vregs.length ||
                   !spilled[uses[ui].as.vreg])
                    continue;
                JitVRegId original = uses[ui].as.vreg;
                Replacement* replacement = find_replacement(replacements, replacement_count, original);
                if(replacement == NULL) {
                    JitVReg* vreg = c11__at(JitVReg, &lir->vregs, original);
                    JitVRegId temp_id = jit_lir_new_vreg(lir,
                                                         vreg->reg_class,
                                                         vreg->type,
                                                         JIT_INVALID_VALUE);
                    Replacement fresh;
                    fresh.original = original;
                    fresh.temp = jit_lir_vreg(temp_id, vreg->type);
                    fresh.loaded = false;
                    fresh.stored = false;
                    replacements[replacement_count++] = fresh;
                    replacement = &replacements[replacement_count - 1u];
                }
                if(!replacement->loaded) {
                    JitLirOperand load_use = jit_lir_stack(slots[original], replacement->temp.type);
                    JitLirInst load = make_inst(lir,
                                                JIT_LIR_SPILL_LOAD,
                                                &replacement->temp,
                                                1,
                                                &load_use,
                                                1,
                                                old);
                    load.flags = JIT_LIR_FLAG_NONE;
                    load.state = JIT_INVALID_LIR_STATE;
                    c11_vector__push(JitLirInst, &new_insts, load);
                    replacement->loaded = true;
                    result->stats.spill_loads++;
                }
                uses[ui] = replacement->temp;
            }

            for(uint32_t di = 0; di < old->defs.count; di++) {
                if(defs[di].kind != JIT_LIR_OPERAND_VREG || defs[di].as.vreg >= (JitVRegId)lir->vregs.length ||
                   !spilled[defs[di].as.vreg])
                    continue;
                JitVRegId original = defs[di].as.vreg;
                Replacement* replacement = find_replacement(replacements, replacement_count, original);
                if(replacement == NULL) {
                    JitVReg* vreg = c11__at(JitVReg, &lir->vregs, original);
                    JitVRegId temp_id = jit_lir_new_vreg(lir,
                                                         vreg->reg_class,
                                                         vreg->type,
                                                         JIT_INVALID_VALUE);
                    Replacement fresh;
                    fresh.original = original;
                    fresh.temp = jit_lir_vreg(temp_id, vreg->type);
                    fresh.loaded = false;
                    fresh.stored = false;
                    replacements[replacement_count++] = fresh;
                    replacement = &replacements[replacement_count - 1u];
                }
                defs[di] = replacement->temp;
                replacement->stored = true;
            }

            JitLirInst rewritten = make_inst(lir,
                                             old->op,
                                             defs,
                                             old->defs.count,
                                             uses,
                                             old->uses.count,
                                             old);
            c11_vector__push(JitLirInst, &new_insts, rewritten);

            for(uint32_t ri = 0; ri < replacement_count; ri++) {
                Replacement* replacement = &replacements[ri];
                if(!replacement->stored) continue;
                JitLirOperand store_uses[2] = {
                    jit_lir_stack(slots[replacement->original], replacement->temp.type),
                    replacement->temp,
                };
                JitLirInst store = make_inst(lir,
                                             JIT_LIR_SPILL_STORE,
                                             NULL,
                                             0,
                                             store_uses,
                                             2,
                                             old);
                store.flags = JIT_LIR_FLAG_NONE;
                store.state = JIT_INVALID_LIR_STATE;
                c11_vector__push(JitLirInst, &new_insts, store);
                result->stats.spill_stores++;
            }

            free(replacements);
            free(defs);
            free(uses);
        }
        c11_vector__dtor(&block->insts);
        block->insts = new_insts;
    }
    return true;
}

static bool assign_spill_slots(JitLirFunction* lir,
                               ColorGraph* graph,
                               JitRegAllocResult* result,
                               bool** out_spilled,
                               JitStackSlotId** out_slots,
                               uint32_t* out_count) {
    uint32_t n = graph->n;
    bool* spilled = calloc(n == 0u ? 1u : n, sizeof(bool));
    JitStackSlotId* slots = malloc(sizeof(JitStackSlotId) * (n == 0u ? 1u : n));
    if(spilled == NULL || slots == NULL) {
        free(spilled);
        free(slots);
        return false;
    }
    for(uint32_t i = 0; i < n; i++) slots[i] = JIT_INVALID_STACK_SLOT;
    ensure_location_count(result, (uint32_t)lir->vregs.length);

    uint32_t spill_count = 0u;
    for(uint32_t root_id = 0; root_id < n; root_id++) {
        if(find_root(graph, root_id) != root_id || !graph->spilled[root_id]) continue;
        JitStackSlotId slot = jit_lir_new_stack_slot(lir, 8u, 8u, JIT_STACK_SLOT_SPILL);
        for(uint32_t i = 0; i < n; i++) {
            if(!graph->allocatable[i] || find_root(graph, i) != root_id) continue;
            spilled[i] = true;
            slots[i] = slot;
            JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
            vreg->force_stack = true;
            JitLocation* loc = c11__at(JitLocation, &result->locations, i);
            loc->kind = JIT_LOCATION_STACK;
            loc->stack_slot = slot;
            loc->preg.bank = JIT_PHYS_NONE;
            spill_count++;
        }
    }
    *out_spilled = spilled;
    *out_slots = slots;
    *out_count = spill_count;
    return true;
}

static void assign_register_locations(JitLirFunction* lir,
                                      const JitGraphColorConfig* config,
                                      ColorGraph* graph,
                                      JitRegAllocResult* result) {
    ensure_location_count(result, (uint32_t)lir->vregs.length);
    for(uint32_t i = 0; i < (uint32_t)lir->vregs.length && i < graph->n; i++) {
        if(!graph->allocatable[i] || graph->color[i] < 0) continue;
        JitLocation* loc = c11__at(JitLocation, &result->locations, i);
        if(loc->kind == JIT_LOCATION_STACK) continue;
        const JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
        loc->kind = JIT_LOCATION_REGISTER;
        loc->preg = color_to_reg(config, vreg->reg_class, (uint32_t)graph->color[i]);
        loc->stack_slot = JIT_INVALID_STACK_SLOT;
    }
}

bool jit_graph_color_allocate(JitLirFunction* lir,
                              const JitGraphColorConfig* config_in,
                              JitRegAllocResult* result,
                              char* error,
                              uint32_t error_size) {
    if(lir == NULL || result == NULL)
        return failf(error, error_size, "LIR and result must be non-NULL");
    JitGraphColorConfig defaults;
    jit_graph_color_config_init(&defaults);
    const JitGraphColorConfig* config = config_in == NULL ? &defaults : config_in;
    if(!validate_config(config, error, error_size)) return false;

    c11_vector__clear(&result->locations);
    memset(&result->stats, 0, sizeof(result->stats));
    result->stats.initial_vregs = (uint32_t)lir->vregs.length;
    lir->tvalue_size = config->tvalue_size;
    lir->stack_alignment = config->stack_alignment;
    assign_value_homes(lir, config, result);

    uint32_t max_rounds = config->max_spill_rounds == 0u ? 1u : config->max_spill_rounds;
    for(uint32_t round = 0; round <= max_rounds; round++) {
        jit_lir_rebuild_cfg(lir);
        Liveness live;
        if(!build_liveness(lir, &live))
            return failf(error, error_size, "out of memory building LIR liveness");

        ColorGraph graph;
        if(!build_interference(lir, config, &live, &graph, &result->stats)) {
            liveness_destroy(&live);
            return failf(error, error_size, "out of memory building interference graph");
        }
        liveness_destroy(&live);

        coalesce_graph(lir, &graph, &result->stats, config->enable_coalescing);
        if(!color_graph(&graph)) {
            graph_destroy(&graph);
            return failf(error, error_size, "out of memory coloring interference graph");
        }

        bool any_spill = false;
        for(uint32_t i = 0; i < graph.n; i++) {
            if(graph.allocatable[i] && graph.spilled[i]) {
                any_spill = true;
                break;
            }
        }
        if(!any_spill) {
            assign_register_locations(lir, config, &graph, result);
            graph_destroy(&graph);
            break;
        }
        if(round == max_rounds) {
            graph_destroy(&graph);
            return failf(error,
                         error_size,
                         "register allocation still spills after %u rewrite rounds",
                         max_rounds);
        }

        bool* spilled = NULL;
        JitStackSlotId* slots = NULL;
        uint32_t spill_count = 0u;
        if(!assign_spill_slots(lir, &graph, result, &spilled, &slots, &spill_count)) {
            graph_destroy(&graph);
            return failf(error, error_size, "out of memory assigning spill slots");
        }
        graph_destroy(&graph);
        result->stats.spill_rounds++;
        result->stats.spilled_vregs += spill_count;
        if(!rewrite_spills(lir, spilled, slots, result)) {
            free(spilled);
            free(slots);
            return failf(error, error_size, "out of memory rewriting spills");
        }
        free(spilled);
        free(slots);
        assign_value_homes(lir, config, result);
    }

    ensure_location_count(result, (uint32_t)lir->vregs.length);
    result->stats.final_vregs = (uint32_t)lir->vregs.length;
    jit_lir_layout_stack(lir);
    result->stats.stack_slots = (uint32_t)lir->stack_slots.length;
    result->stats.stack_bytes = lir->local_stack_size;

    for(uint32_t i = 0; i < (uint32_t)lir->vregs.length; i++) {
        const JitVReg* vreg = c11__at(JitVReg, &lir->vregs, i);
        if(vreg->reg_class == JIT_VREG_NONE) continue;
        const JitLocation* loc = c11__at(JitLocation, &result->locations, i);
        if(loc->kind == JIT_LOCATION_NONE)
            return failf(error, error_size, "allocator left v%u without a physical location", i);
    }

    if(error != NULL && error_size != 0u) error[0] = '\0';
    return true;
}

static bool rewrite_operand(const JitRegAllocResult* result,
                            JitLirOperand* operand,
                            char* error,
                            uint32_t error_size) {
    if(operand->kind != JIT_LIR_OPERAND_VREG) return true;
    if(operand->as.vreg >= (JitVRegId)result->locations.length)
        return failf(error, error_size, "physical rewrite references unknown vreg v%u", operand->as.vreg);
    const JitLocation* loc = c11__at(JitLocation, &result->locations, operand->as.vreg);
    if(loc->kind == JIT_LOCATION_REGISTER) {
        JitTypeInfo type = operand->type;
        *operand = jit_lir_preg(loc->preg.bank, loc->preg.index, type);
        return true;
    }
    if(loc->kind == JIT_LOCATION_STACK) {
        JitTypeInfo type = operand->type;
        *operand = jit_lir_stack(loc->stack_slot, type);
        return true;
    }
    return failf(error, error_size, "vreg v%u has no assigned location", operand->as.vreg);
}

bool jit_regalloc_apply(JitLirFunction* lir,
                        const JitRegAllocResult* result,
                        char* error,
                        uint32_t error_size) {
    if(lir == NULL || result == NULL)
        return failf(error, error_size, "LIR and allocation result must be non-NULL");
    c11__foreach(JitLirOperand, &lir->operands, operand) {
        if(!rewrite_operand(result, operand, error, error_size)) return false;
    }
    c11__foreach(JitLirOperand, &lir->state_operands, operand) {
        if(!rewrite_operand(result, operand, error, error_size)) return false;
    }
    c11__foreach(JitLirOperand, &lir->mir_value_map, operand) {
        if(!rewrite_operand(result, operand, error, error_size)) return false;
    }
    c11__foreach(JitLirEdgeCopy, &lir->edge_copies, edge) {
        c11__foreach(JitLirOperand, &edge->dsts, operand) {
            if(!rewrite_operand(result, operand, error, error_size)) return false;
        }
        c11__foreach(JitLirOperand, &edge->srcs, operand) {
            if(!rewrite_operand(result, operand, error, error_size)) return false;
        }
    }
    jit_lir_layout_stack(lir);
    while(jit_lir_peephole(lir)) {
    }
    if(error != NULL && error_size != 0u) error[0] = '\0';
    return true;
}
