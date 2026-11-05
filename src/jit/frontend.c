#include "tinypy/jit/frontend.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct BcBlock {
    int32_t start_ip;
    int32_t end_ip;
    c11_vector predecessors; /* int32_t block index */
    c11_vector successors;   /* int32_t block index */
    bool reachable;
    bool selected;
    int32_t entry_depth;

    JitBlockId mir_block;
    c11_vector mir_predecessors; /* JitBlockId */
    c11_vector entry_locals;     /* JitValueId */
    c11_vector entry_stack;      /* JitValueId */
} BcBlock;

typedef struct BcGraph {
    const CodeObject* co;
    c11_vector blocks; /* BcBlock */
    int32_t* ip_to_block;
    int32_t code_count;
    uint32_t max_stack_depth;
} BcGraph;

typedef struct LowerContext {
    const CodeObject* co;
    const JitFrontendOptions* options;
    JitRegion* region;
    JitFrontendStats* stats;
    JitLowerError* error;
    BcGraph graph;
    int32_t entry_bc_block;
    JitBlockId synthetic_entry;
    bool failed;
} LowerContext;

typedef struct LowerState {
    JitValueId* locals;
    uint32_t local_count;
    JitValueId* stack;
    uint32_t stack_size;
    uint32_t stack_capacity;
} LowerState;

static JitImmediate imm_zero(void) {
    JitImmediate imm;
    memset(&imm, 0, sizeof(imm));
    return imm;
}

static void clear_error(JitLowerError* error) {
    if(error == NULL) return;
    memset(error, 0, sizeof(*error));
    error->status = JIT_LOWER_OK;
    error->bytecode_ip = -1;
}

static bool lower_fail(LowerContext* ctx, JitLowerStatus status, int32_t ip, const char* fmt, ...) {
    ctx->failed = true;
    if(ctx->error != NULL) {
        ctx->error->status = status;
        ctx->error->bytecode_ip = ip;
        va_list args;
        va_start(args, fmt);
        vsnprintf(ctx->error->message, sizeof(ctx->error->message), fmt, args);
        va_end(args);
    }
    return false;
}

void jit_frontend_options_init(JitFrontendOptions* options) {
    memset(options, 0, sizeof(*options));
    options->estimated_execution_count = 10;
    options->poll_loop_backedges = true;
    options->allow_generators = false;
    options->verify = true;
}

const char* jit_lower_status_name(JitLowerStatus status) {
    switch(status) {
        case JIT_LOWER_OK: return "ok";
        case JIT_LOWER_INVALID_ARGUMENT: return "invalid argument";
        case JIT_LOWER_INVALID_BYTECODE: return "invalid bytecode";
        case JIT_LOWER_STACK_MISMATCH: return "stack mismatch";
        case JIT_LOWER_UNSUPPORTED_GENERATOR: return "unsupported generator";
        case JIT_LOWER_LOOP_NOT_FOUND: return "loop not found";
        case JIT_LOWER_VERIFY_FAILED: return "MIR verification failed";
    }
    return "unknown";
}

const char* jit_helper_name(JitHelperId helper) {
#define CASE(name)                                                                                 \
    case JIT_HELPER_##name: return #name
    switch(helper) {
        CASE(INVALID);
        CASE(PRINT_EXPR);
        CASE(LOAD_ELLIPSIS);
        CASE(LOAD_NAME_AS_INT);
        CASE(LOAD_FUNCTION);
        CASE(LOAD_NAME);
        CASE(LOAD_CLASS_GLOBAL);
        CASE(STORE_NAME);
        CASE(DELETE_FAST);
        CASE(DELETE_NAME);
        CASE(DELETE_GLOBAL);
        CASE(DELETE_ATTR);
        CASE(DELETE_SUBSCR);
        CASE(BUILD_IMAG);
        CASE(BUILD_BYTES);
        CASE(BUILD_SLICE);
        CASE(BUILD_STRING);
        CASE(BINARY_LSHIFT);
        CASE(BINARY_RSHIFT);
        CASE(BINARY_AND);
        CASE(BINARY_OR);
        CASE(BINARY_XOR);
        CASE(BINARY_MATMUL);
        CASE(CONTAINS);
        CASE(UNARY_NEGATIVE);
        CASE(UNARY_STAR);
        CASE(UNARY_INVERT);
        CASE(LOAD_METHOD);
        CASE(ITER_NEXT);
        CASE(CALL_VARGS);
        CASE(LIST_APPEND);
        CASE(DICT_ADD);
        CASE(SET_ADD);
        CASE(IMPORT_PATH);
        CASE(POP_IMPORT_STAR);
        CASE(UNPACK_SEQUENCE);
        CASE(UNPACK_EX);
        CASE(BEGIN_CLASS);
        CASE(END_CLASS);
        CASE(STORE_CLASS_ATTR);
        CASE(ADD_CLASS_ANNOTATION);
        CASE(WITH_ENTER);
        CASE(WITH_EXIT);
        CASE(BEGIN_TRY);
        CASE(END_TRY);
        CASE(EXCEPTION_MATCH);
        CASE(HANDLE_EXCEPTION);
        CASE(RAISE);
        CASE(RAISE_ASSERT);
        CASE(RE_RAISE);
        CASE(PUSH_EXCEPTION);
        CASE(FORMAT_STRING);
        CASE(POP_JUMP_IF_NOT_MATCH);
        CASE(_COUNT);
    }
#undef CASE
    return "<invalid-helper>";
}

static void bc_block_init(BcBlock* block, int32_t start_ip, int32_t end_ip) {
    memset(block, 0, sizeof(*block));
    block->start_ip = start_ip;
    block->end_ip = end_ip;
    block->entry_depth = -1;
    block->mir_block = JIT_INVALID_BLOCK;
    c11_vector__ctor(&block->predecessors, sizeof(int32_t));
    c11_vector__ctor(&block->successors, sizeof(int32_t));
    c11_vector__ctor(&block->mir_predecessors, sizeof(JitBlockId));
    c11_vector__ctor(&block->entry_locals, sizeof(JitValueId));
    c11_vector__ctor(&block->entry_stack, sizeof(JitValueId));
}

static void bc_block_destroy(BcBlock* block) {
    c11_vector__dtor(&block->predecessors);
    c11_vector__dtor(&block->successors);
    c11_vector__dtor(&block->mir_predecessors);
    c11_vector__dtor(&block->entry_locals);
    c11_vector__dtor(&block->entry_stack);
}

static void bc_graph_destroy(BcGraph* graph) {
    c11__foreach(BcBlock, &graph->blocks, block) { bc_block_destroy(block); }
    c11_vector__dtor(&graph->blocks);
    free(graph->ip_to_block);
    memset(graph, 0, sizeof(*graph));
}

static bool vector_has_i32(const c11_vector* vector, int32_t value) {
    c11__foreach(int32_t, vector, p) {
        if(*p == value) return true;
    }
    return false;
}

static void vector_push_unique_i32(c11_vector* vector, int32_t value) {
    if(!vector_has_i32(vector, value)) c11_vector__push(int32_t, vector, value);
}

static bool vector_has_block(const c11_vector* vector, JitBlockId value) {
    c11__foreach(JitBlockId, vector, p) {
        if(*p == value) return true;
    }
    return false;
}

static void vector_push_unique_block(c11_vector* vector, JitBlockId value) {
    if(!vector_has_block(vector, value)) c11_vector__push(JitBlockId, vector, value);
}

static bool op_is_unconditional_jump(Opcode op) {
    switch(op) {
        case OP_JUMP_FORWARD:
        case OP_LOOP_CONTINUE:
        case OP_LOOP_BREAK: return true;
        default: return false;
    }
}

static bool op_is_conditional_jump(Opcode op) {
    switch(op) {
        case OP_POP_JUMP_IF_NOT_MATCH:
        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE:
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP:
        case OP_SHORTCUT_IF_FALSE_OR_POP:
        case OP_FOR_ITER: return true;
        default: return false;
    }
}

static bool op_is_terminal(Opcode op) {
    switch(op) {
        case OP_RETURN_VALUE:
        case OP_YIELD_VALUE:
        case OP_FOR_ITER_YIELD_VALUE:
        case OP_RAISE:
        case OP_RAISE_ASSERT:
        case OP_RE_RAISE: return true;
        default: return false;
    }
}

static int32_t jump_target(int32_t ip, uint16_t arg) { return ip + (int16_t)arg; }

static int32_t opcode_stack_required(Opcode op, uint16_t arg) {
    uint32_t argc;
    uint32_t kwargc;
    switch(op) {
        case OP_POP_TOP:
        case OP_DUP_TOP:
        case OP_PRINT_EXPR:
        case OP_LOAD_ATTR:
        case OP_LOAD_METHOD:
        case OP_STORE_FAST:
        case OP_STORE_NAME:
        case OP_STORE_GLOBAL:
        case OP_STORE_SELF_ATTR:
        case OP_DELETE_ATTR:
        case OP_BUILD_IMAG:
        case OP_UNARY_NEGATIVE:
        case OP_UNARY_NOT:
        case OP_UNARY_STAR:
        case OP_UNARY_INVERT:
        case OP_GET_ITER:
        case OP_FOR_ITER:
        case OP_POP_IMPORT_STAR:
        case OP_UNPACK_SEQUENCE:
        case OP_UNPACK_EX:
        case OP_BEGIN_CLASS:
        case OP_END_CLASS:
        case OP_STORE_CLASS_ATTR:
        case OP_ADD_CLASS_ANNOTATION:
        case OP_WITH_ENTER:
        case OP_WITH_EXIT:
        case OP_EXCEPTION_MATCH:
        case OP_RAISE:
        case OP_FORMAT_STRING:
            return 1;

        case OP_DUP_TOP_TWO:
        case OP_ROT_TWO:
        case OP_LOAD_SUBSCR:
        case OP_STORE_ATTR:
        case OP_DELETE_SUBSCR:
        case OP_BINARY_ADD:
        case OP_BINARY_SUB:
        case OP_BINARY_MUL:
        case OP_BINARY_TRUEDIV:
        case OP_BINARY_FLOORDIV:
        case OP_BINARY_MOD:
        case OP_BINARY_POW:
        case OP_BINARY_LSHIFT:
        case OP_BINARY_RSHIFT:
        case OP_BINARY_AND:
        case OP_BINARY_OR:
        case OP_BINARY_XOR:
        case OP_BINARY_MATMUL:
        case OP_COMPARE_LT:
        case OP_COMPARE_LE:
        case OP_COMPARE_EQ:
        case OP_COMPARE_NE:
        case OP_COMPARE_GT:
        case OP_COMPARE_GE:
        case OP_IS_OP:
        case OP_CONTAINS_OP:
        case OP_POP_JUMP_IF_NOT_MATCH:
        case OP_SHORTCUT_IF_FALSE_OR_POP:
            return 2;

        case OP_ROT_THREE:
        case OP_STORE_SUBSCR:
        case OP_BUILD_SLICE:
        case OP_LIST_APPEND:
        case OP_SET_ADD: return 3;

        case OP_DICT_ADD: return 4;

        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE:
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP: return 1;

        case OP_CALL:
        case OP_CALL_VARGS:
            argc = arg & 0xFFu;
            kwargc = arg >> 8;
            return (int32_t)(2u + argc + 2u * kwargc);

        case OP_RETURN_VALUE: return arg == BC_NOARG ? 1 : 0;
        case OP_YIELD_VALUE: return arg == 1 ? 0 : 1;
        case OP_RAISE_ASSERT: return arg ? 1 : 0;

        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST:
        case OP_BUILD_SET:
        case OP_BUILD_STRING: return (int32_t)arg;
        case OP_BUILD_DICT: return (int32_t)arg * 2;

        default: return 0;
    }
}

static int32_t opcode_fallthrough_delta(Opcode op, uint16_t arg) {
    uint32_t argc;
    uint32_t kwargc;
    switch(op) {
        case OP_POP_TOP: return -1;
        case OP_DUP_TOP: return 1;
        case OP_DUP_TOP_TWO: return 2;
        case OP_ROT_TWO:
        case OP_ROT_THREE:
        case OP_NO_OP: return 0;
        case OP_PRINT_EXPR: return -1;

        case OP_LOAD_CONST:
        case OP_LOAD_NONE:
        case OP_LOAD_TRUE:
        case OP_LOAD_FALSE:
        case OP_LOAD_SMALL_INT:
        case OP_LOAD_NAME_AS_INT:
        case OP_LOAD_ELLIPSIS:
        case OP_LOAD_FUNCTION:
        case OP_LOAD_NULL:
        case OP_LOAD_FAST:
        case OP_LOAD_NAME:
        case OP_LOAD_NONLOCAL:
        case OP_LOAD_GLOBAL:
        case OP_LOAD_SELF_ATTR:
        case OP_LOAD_CLASS_GLOBAL:
        case OP_BUILD_BYTES:
        case OP_IMPORT_PATH:
        case OP_PUSH_EXCEPTION:
            return 1;

        case OP_LOAD_ATTR:
        case OP_BUILD_IMAG:
        case OP_UNARY_NEGATIVE:
        case OP_UNARY_NOT:
        case OP_UNARY_STAR:
        case OP_UNARY_INVERT:
        case OP_GET_ITER:
        case OP_BEGIN_CLASS:
        case OP_EXCEPTION_MATCH:
        case OP_FORMAT_STRING:
            return 0;

        case OP_LOAD_METHOD: return 1;
        case OP_LOAD_SUBSCR: return -1;

        case OP_STORE_FAST:
        case OP_STORE_NAME:
        case OP_STORE_GLOBAL:
        case OP_STORE_SELF_ATTR:
        case OP_END_CLASS:
        case OP_STORE_CLASS_ATTR:
        case OP_ADD_CLASS_ANNOTATION:
        case OP_POP_IMPORT_STAR:
        case OP_WITH_EXIT:
            return -1;

        case OP_STORE_ATTR: return -2;
        case OP_STORE_SUBSCR: return -3;

        case OP_DELETE_FAST:
        case OP_DELETE_NAME:
        case OP_DELETE_GLOBAL:
        case OP_DELETE_ATTR:
            return 0;
        case OP_DELETE_SUBSCR: return -2;

        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST:
        case OP_BUILD_SET:
        case OP_BUILD_STRING: return 1 - (int32_t)arg;
        case OP_BUILD_DICT: return 1 - (int32_t)arg * 2;
        case OP_BUILD_SLICE: return -2;

        case OP_BINARY_ADD:
        case OP_BINARY_SUB:
        case OP_BINARY_MUL:
        case OP_BINARY_TRUEDIV:
        case OP_BINARY_FLOORDIV:
        case OP_BINARY_MOD:
        case OP_BINARY_POW:
        case OP_BINARY_LSHIFT:
        case OP_BINARY_RSHIFT:
        case OP_BINARY_AND:
        case OP_BINARY_OR:
        case OP_BINARY_XOR:
        case OP_BINARY_MATMUL:
        case OP_COMPARE_LT:
        case OP_COMPARE_LE:
        case OP_COMPARE_EQ:
        case OP_COMPARE_NE:
        case OP_COMPARE_GT:
        case OP_COMPARE_GE:
        case OP_IS_OP:
        case OP_CONTAINS_OP:
            return -1;

        case OP_POP_JUMP_IF_NOT_MATCH: return -2;
        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE: return -1;
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP: return -1;
        case OP_SHORTCUT_IF_FALSE_OR_POP: return -1;

        case OP_CALL:
        case OP_CALL_VARGS:
            argc = arg & 0xFFu;
            kwargc = arg >> 8;
            return -(int32_t)(1u + argc + 2u * kwargc);

        case OP_LIST_APPEND:
        case OP_SET_ADD: return -1;
        case OP_DICT_ADD: return -2;

        case OP_UNPACK_SEQUENCE: return (int32_t)arg - 1;
        case OP_UNPACK_EX: return (int32_t)arg;
        case OP_WITH_ENTER: return 1;

        case OP_BEGIN_TRY:
        case OP_END_TRY:
        case OP_HANDLE_EXCEPTION:
            return 0;

        default: return 0;
    }
}

static bool stack_depth_for_edge(Opcode op,
                                 uint16_t arg,
                                 int32_t before,
                                 bool taken,
                                 int32_t* out_depth) {
    switch(op) {
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP:
            *out_depth = taken ? before : before - 1;
            return true;
        case OP_FOR_ITER:
            *out_depth = taken ? before - 1 : before + 1;
            return true;
        case OP_SHORTCUT_IF_FALSE_OR_POP:
            *out_depth = before - 1;
            return true;
        default:
            *out_depth = before + opcode_fallthrough_delta(op, arg);
            return true;
    }
}

static bool mark_leader(bool* leaders, int32_t count, int32_t ip) {
    if(ip < 0 || ip >= count) return false;
    leaders[ip] = true;
    return true;
}

static bool build_bc_graph(LowerContext* ctx) {
    const CodeObject* co = ctx->co;
    BcGraph* graph = &ctx->graph;
    memset(graph, 0, sizeof(*graph));
    graph->co = co;
    graph->code_count = co->codes.length;
    c11_vector__ctor(&graph->blocks, sizeof(BcBlock));

    if(graph->code_count <= 0) return lower_fail(ctx, JIT_LOWER_INVALID_BYTECODE, -1, "empty CodeObject");

    bool* leaders = calloc((size_t)graph->code_count, sizeof(bool));
    if(leaders == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, -1, "out of memory");
    leaders[0] = true;

    for(int32_t ip = 0; ip < graph->code_count; ip++) {
        Bytecode byte = c11__getitem(Bytecode, &co->codes, ip);
        Opcode op = (Opcode)byte.op;
        if(op_is_unconditional_jump(op) || op_is_conditional_jump(op)) {
            int32_t target = jump_target(ip, byte.arg);
            if(!mark_leader(leaders, graph->code_count, target)) {
                free(leaders);
                return lower_fail(ctx,
                                  JIT_LOWER_INVALID_BYTECODE,
                                  ip,
                                  "jump target %d is outside bytecode range",
                                  target);
            }
        }
        if((op_is_unconditional_jump(op) || op_is_conditional_jump(op) || op_is_terminal(op)) &&
           ip + 1 < graph->code_count) {
            leaders[ip + 1] = true;
        }
    }

    int32_t start = 0;
    for(int32_t ip = 1; ip <= graph->code_count; ip++) {
        if(ip == graph->code_count || leaders[ip]) {
            BcBlock* block = c11_vector__emplace(&graph->blocks);
            bc_block_init(block, start, ip);
            start = ip;
        }
    }
    free(leaders);

    graph->ip_to_block = malloc(sizeof(int32_t) * (size_t)graph->code_count);
    if(graph->ip_to_block == NULL)
        return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, -1, "out of memory");

    for(int32_t bi = 0; bi < graph->blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &graph->blocks, bi);
        for(int32_t ip = block->start_ip; ip < block->end_ip; ip++) graph->ip_to_block[ip] = bi;
    }

    for(int32_t bi = 0; bi < graph->blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &graph->blocks, bi);
        int32_t last_ip = block->end_ip - 1;
        Bytecode byte = c11__getitem(Bytecode, &co->codes, last_ip);
        Opcode op = (Opcode)byte.op;

        if(op_is_unconditional_jump(op)) {
            int32_t target = graph->ip_to_block[jump_target(last_ip, byte.arg)];
            vector_push_unique_i32(&block->successors, target);
        } else if(op_is_conditional_jump(op)) {
            int32_t target = graph->ip_to_block[jump_target(last_ip, byte.arg)];
            vector_push_unique_i32(&block->successors, target);
            if(block->end_ip < graph->code_count)
                vector_push_unique_i32(&block->successors, graph->ip_to_block[block->end_ip]);
        } else if(!op_is_terminal(op) && block->end_ip < graph->code_count) {
            vector_push_unique_i32(&block->successors, graph->ip_to_block[block->end_ip]);
        }
    }

    for(int32_t bi = 0; bi < graph->blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &graph->blocks, bi);
        c11__foreach(int32_t, &block->successors, succ) {
            vector_push_unique_i32(&c11__at(BcBlock, &graph->blocks, *succ)->predecessors, bi);
        }
    }

    return true;
}

static bool set_depth(LowerContext* ctx, int32_t block_index, int32_t depth, c11_vector* worklist) {
    BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, block_index);
    if(depth < 0)
        return lower_fail(ctx,
                          JIT_LOWER_STACK_MISMATCH,
                          block->start_ip,
                          "negative operand-stack depth at block entry");
    if(block->entry_depth == -1) {
        block->entry_depth = depth;
        c11_vector__push(int32_t, worklist, block_index);
        return true;
    }
    if(block->entry_depth != depth)
        return lower_fail(ctx,
                          JIT_LOWER_STACK_MISMATCH,
                          block->start_ip,
                          "stack depth mismatch at merge: %d vs %d",
                          block->entry_depth,
                          depth);
    return true;
}

static bool analyze_stack_depths(LowerContext* ctx) {
    c11_vector worklist;
    c11_vector__ctor(&worklist, sizeof(int32_t));
    if(!set_depth(ctx, 0, 0, &worklist)) {
        c11_vector__dtor(&worklist);
        return false;
    }

    for(int wi = 0; wi < worklist.length; wi++) {
        int32_t bi = c11__getitem(int32_t, &worklist, wi);
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        block->reachable = true;
        int32_t depth = block->entry_depth;
        if((uint32_t)depth > ctx->graph.max_stack_depth) ctx->graph.max_stack_depth = (uint32_t)depth;

        for(int32_t ip = block->start_ip; ip < block->end_ip; ip++) {
            Bytecode byte = c11__getitem(Bytecode, &ctx->co->codes, ip);
            Opcode op = (Opcode)byte.op;
            int32_t required = opcode_stack_required(op, byte.arg);
            if(depth < required) {
                c11_vector__dtor(&worklist);
                return lower_fail(ctx,
                                  JIT_LOWER_STACK_MISMATCH,
                                  ip,
                                  "opcode %u requires %d stack values, only %d available",
                                  (unsigned)byte.op,
                                  required,
                                  depth);
            }

            bool is_last = ip == block->end_ip - 1;
            if(is_last && op_is_conditional_jump(op)) {
                int32_t target_bi = ctx->graph.ip_to_block[jump_target(ip, byte.arg)];
                int32_t fall_bi = block->end_ip < ctx->graph.code_count
                                      ? ctx->graph.ip_to_block[block->end_ip]
                                      : -1;
                int32_t taken_depth;
                int32_t fall_depth;

                bool jump_on_true = true;
                switch(op) {
                    case OP_POP_JUMP_IF_FALSE:
                    case OP_JUMP_IF_FALSE_OR_POP:
                    case OP_SHORTCUT_IF_FALSE_OR_POP:
                    case OP_POP_JUMP_IF_NOT_MATCH:
                    case OP_FOR_ITER: jump_on_true = false; break;
                    default: break;
                }

                if(op == OP_FOR_ITER) {
                    /* taken means the jump-to-exit edge. */
                    stack_depth_for_edge(op, byte.arg, depth, true, &taken_depth);
                    stack_depth_for_edge(op, byte.arg, depth, false, &fall_depth);
                } else if(op == OP_JUMP_IF_TRUE_OR_POP || op == OP_JUMP_IF_FALSE_OR_POP) {
                    bool target_preserves = op == OP_JUMP_IF_TRUE_OR_POP || op == OP_JUMP_IF_FALSE_OR_POP;
                    (void)target_preserves;
                    taken_depth = depth;
                    fall_depth = depth - 1;
                } else {
                    int32_t after = depth + opcode_fallthrough_delta(op, byte.arg);
                    taken_depth = after;
                    fall_depth = after;
                }

                (void)jump_on_true;
                if(!set_depth(ctx, target_bi, taken_depth, &worklist)) {
                    c11_vector__dtor(&worklist);
                    return false;
                }
                if(fall_bi >= 0 && !set_depth(ctx, fall_bi, fall_depth, &worklist)) {
                    c11_vector__dtor(&worklist);
                    return false;
                }
                depth = fall_depth;
            } else if(is_last && op_is_unconditional_jump(op)) {
                int32_t target_bi = ctx->graph.ip_to_block[jump_target(ip, byte.arg)];
                if(!set_depth(ctx, target_bi, depth, &worklist)) {
                    c11_vector__dtor(&worklist);
                    return false;
                }
            } else if(is_last && op_is_terminal(op)) {
                /* No normal successor. */
            } else {
                depth += opcode_fallthrough_delta(op, byte.arg);
                if(depth < 0) {
                    c11_vector__dtor(&worklist);
                    return lower_fail(ctx, JIT_LOWER_STACK_MISMATCH, ip, "stack underflow");
                }
                if((uint32_t)depth > ctx->graph.max_stack_depth)
                    ctx->graph.max_stack_depth = (uint32_t)depth;

                if(is_last && block->end_ip < ctx->graph.code_count) {
                    int32_t next_bi = ctx->graph.ip_to_block[block->end_ip];
                    if(!set_depth(ctx, next_bi, depth, &worklist)) {
                        c11_vector__dtor(&worklist);
                        return false;
                    }
                }
            }
        }
    }

    c11_vector__dtor(&worklist);
    return true;
}

static bool block_is_backedge(const BcGraph* graph, int32_t from, int32_t to) {
    const BcBlock* src = c11__at(BcBlock, &graph->blocks, from);
    const BcBlock* dst = c11__at(BcBlock, &graph->blocks, to);
    return dst->start_ip <= src->start_ip;
}

static bool select_function_blocks(LowerContext* ctx) {
    ctx->entry_bc_block = 0;
    c11__foreach(BcBlock, &ctx->graph.blocks, block) { block->selected = block->reachable; }
    return true;
}

static bool select_loop_blocks(LowerContext* ctx, int32_t header_ip) {
    if(header_ip < 0 || header_ip >= ctx->graph.code_count)
        return lower_fail(ctx, JIT_LOWER_LOOP_NOT_FOUND, header_ip, "loop header is outside bytecode range");

    int32_t header_bi = ctx->graph.ip_to_block[header_ip];
    BcBlock* header = c11__at(BcBlock, &ctx->graph.blocks, header_bi);
    if(header->start_ip != header_ip)
        return lower_fail(ctx,
                          JIT_LOWER_LOOP_NOT_FOUND,
                          header_ip,
                          "requested loop header is not a basic-block boundary");

    bool found = false;
    bool* selected = calloc((size_t)ctx->graph.blocks.length, sizeof(bool));
    if(selected == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, header_ip, "out of memory");
    selected[header_bi] = true;

    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        if(!block->reachable) continue;
        c11__foreach(int32_t, &block->successors, succ) {
            if(*succ != header_bi || !block_is_backedge(&ctx->graph, bi, *succ)) continue;
            found = true;
            selected[bi] = true;

            c11_vector work;
            c11_vector__ctor(&work, sizeof(int32_t));
            c11_vector__push(int32_t, &work, bi);
            for(int wi = 0; wi < work.length; wi++) {
                int32_t curr = c11__getitem(int32_t, &work, wi);
                BcBlock* cb = c11__at(BcBlock, &ctx->graph.blocks, curr);
                c11__foreach(int32_t, &cb->predecessors, pred) {
                    if(*pred == header_bi || selected[*pred]) continue;
                    BcBlock* pb = c11__at(BcBlock, &ctx->graph.blocks, *pred);
                    if(pb->start_ip < header->start_ip) continue;
                    selected[*pred] = true;
                    c11_vector__push(int32_t, &work, *pred);
                }
            }
            c11_vector__dtor(&work);
        }
    }

    if(!found) {
        free(selected);
        return lower_fail(ctx, JIT_LOWER_LOOP_NOT_FOUND, header_ip, "no backward edge targets this header");
    }

    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        block->selected = selected[bi] && block->reachable;
    }
    free(selected);
    ctx->entry_bc_block = header_bi;
    return true;
}

static bool lower_state_init(LowerState* state, uint32_t locals, uint32_t stack_capacity) {
    memset(state, 0, sizeof(*state));
    state->local_count = locals;
    state->stack_capacity = stack_capacity == 0 ? 1 : stack_capacity;
    state->locals = malloc(sizeof(JitValueId) * (size_t)(locals == 0 ? 1 : locals));
    state->stack = malloc(sizeof(JitValueId) * (size_t)state->stack_capacity);
    return state->locals != NULL && state->stack != NULL;
}

static void lower_state_destroy(LowerState* state) {
    free(state->locals);
    free(state->stack);
    memset(state, 0, sizeof(*state));
}

static bool lower_state_clone(LowerContext* ctx,
                              const LowerState* source,
                              LowerState* destination,
                              int32_t ip) {
    if(!lower_state_init(destination, source->local_count, source->stack_capacity))
        return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
    destination->stack_size = source->stack_size;
    if(source->local_count != 0)
        memcpy(destination->locals,
               source->locals,
               sizeof(JitValueId) * (size_t)source->local_count);
    if(source->stack_size != 0)
        memcpy(destination->stack,
               source->stack,
               sizeof(JitValueId) * (size_t)source->stack_size);
    return true;
}

static bool lower_state_from_block(LowerContext* ctx, BcBlock* block, LowerState* state) {
    if(!lower_state_init(state, (uint32_t)ctx->co->nlocals, ctx->graph.max_stack_depth + 8))
        return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, block->start_ip, "out of memory");
    if(block->entry_locals.length != ctx->co->nlocals || block->entry_stack.length != block->entry_depth) {
        lower_state_destroy(state);
        return lower_fail(ctx,
                          JIT_LOWER_INVALID_BYTECODE,
                          block->start_ip,
                          "internal SSA entry-state size mismatch");
    }
    if(state->local_count != 0)
        memcpy(state->locals,
               block->entry_locals.data,
               sizeof(JitValueId) * (size_t)state->local_count);
    state->stack_size = (uint32_t)block->entry_depth;
    if(state->stack_size != 0)
        memcpy(state->stack,
               block->entry_stack.data,
               sizeof(JitValueId) * (size_t)state->stack_size);
    return true;
}

static JitValueId stack_pop(LowerContext* ctx, LowerState* state, int32_t ip) {
    if(state->stack_size == 0) {
        lower_fail(ctx, JIT_LOWER_STACK_MISMATCH, ip, "MIR lowering stack underflow");
        return JIT_INVALID_VALUE;
    }
    return state->stack[--state->stack_size];
}

static JitValueId stack_peek(LowerContext* ctx, LowerState* state, uint32_t depth, int32_t ip) {
    if(depth >= state->stack_size) {
        lower_fail(ctx, JIT_LOWER_STACK_MISMATCH, ip, "MIR lowering stack underflow");
        return JIT_INVALID_VALUE;
    }
    return state->stack[state->stack_size - 1 - depth];
}

static bool stack_push(LowerContext* ctx, LowerState* state, JitValueId value, int32_t ip) {
    if(state->stack_size >= state->stack_capacity)
        return lower_fail(ctx, JIT_LOWER_STACK_MISMATCH, ip, "operand stack exceeds analyzed maximum");
    state->stack[state->stack_size++] = value;
    return true;
}

static JitTypeInfo value_type(const LowerContext* ctx, JitValueId value) {
    if(value >= (JitValueId)ctx->region->values.length) return jit_type_value();
    return jit_region_value_const(ctx->region, value)->type;
}

static JitValueId emit_helper(LowerContext* ctx,
                              JitBuilder* builder,
                              JitHelperId helper,
                              JitTypeInfo type,
                              const JitValueId* operands,
                              uint32_t operand_count,
                              uint32_t byte_arg) {
    JitImmediate imm = imm_zero();
    imm.u32 = byte_arg;
    JitValueId value = jit_builder_emit_value(builder,
                                              JIT_OP_CALL_HELPER,
                                              type,
                                              operands,
                                              operand_count,
                                              imm,
                                              (uint32_t)helper);
    ctx->stats->helper_count++;
    return value;
}

static JitInstId emit_helper_void(LowerContext* ctx,
                                  JitBuilder* builder,
                                  JitHelperId helper,
                                  const JitValueId* operands,
                                  uint32_t operand_count,
                                  uint32_t byte_arg) {
    JitImmediate imm = imm_zero();
    imm.u32 = byte_arg;
    ctx->stats->helper_count++;
    return jit_builder_emit(builder,
                            JIT_OP_CALL_HELPER,
                            jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                            operands,
                            operand_count,
                            imm,
                            (uint32_t)helper);
}

static JitValueId emit_extract(JitBuilder* builder,
                               JitValueId token,
                               uint32_t index,
                               JitTypeInfo type) {
    JitImmediate imm = imm_zero();
    return jit_builder_emit_value(builder, JIT_OP_EXTRACT_TOKEN, type, &token, 1, imm, index);
}

static bool block_expected_pred(const BcBlock* block, JitBlockId pred, uint32_t* out_index) {
    c11__foreach(JitBlockId, &block->mir_predecessors, p) {
        if(*p == pred) {
            *out_index = (uint32_t)(p - (JitBlockId*)block->mir_predecessors.data);
            return true;
        }
    }
    return false;
}

static bool fill_phi_operand(LowerContext* ctx,
                             JitValueId phi_value,
                             uint32_t pred_index,
                             JitValueId incoming) {
    JitValue* value = jit_region_value(ctx->region, phi_value);
    JitInst* phi = jit_region_inst(ctx->region, value->def);
    assert(phi->op == JIT_OP_PHI);
    if(pred_index >= phi->operands.count) return false;
    JitValueId* operands = c11__at(JitValueId, &ctx->region->operands, phi->operands.offset);
    operands[pred_index] = incoming;
    return true;
}

static bool fill_edge_state(LowerContext* ctx,
                            JitBlockId source,
                            int32_t target_bi,
                            const LowerState* state) {
    BcBlock* target = c11__at(BcBlock, &ctx->graph.blocks, target_bi);
    uint32_t pred_index;
    if(!block_expected_pred(target, source, &pred_index))
        return lower_fail(ctx,
                          JIT_LOWER_INVALID_BYTECODE,
                          target->start_ip,
                          "missing MIR predecessor B%u for bytecode block",
                          source);
    if(target->entry_locals.length != (int)state->local_count ||
       target->entry_stack.length != (int)state->stack_size)
        return lower_fail(ctx,
                          JIT_LOWER_STACK_MISMATCH,
                          target->start_ip,
                          "edge state shape does not match block entry");

    for(uint32_t i = 0; i < state->local_count; i++) {
        JitValueId phi = c11__getitem(JitValueId, &target->entry_locals, (int)i);
        if(!fill_phi_operand(ctx, phi, pred_index, state->locals[i])) return false;
    }
    for(uint32_t i = 0; i < state->stack_size; i++) {
        JitValueId phi = c11__getitem(JitValueId, &target->entry_stack, (int)i);
        if(!fill_phi_operand(ctx, phi, pred_index, state->stack[i])) return false;
    }
    return true;
}

static JitStateId add_resume_state(LowerContext* ctx, int32_t ip, const LowerState* state) {
    return jit_region_add_state(ctx->region,
                                ip,
                                state->locals,
                                state->local_count,
                                state->stack,
                                state->stack_size);
}

static JitBlockId make_exit_stub(LowerContext* ctx, int32_t target_ip, const LowerState* state) {
    JitBlockId block = jit_region_add_block(ctx->region, 1, JIT_BLOCK_COLD);
    JitBuilder builder;
    jit_builder_init(&builder, ctx->region, block);
    jit_builder_set_source(&builder, target_ip, -1);
    JitStateId state_id = add_resume_state(ctx, target_ip, state);
    jit_builder_exit_interpreter(&builder, state_id);
    ctx->stats->exit_stub_count++;
    return block;
}

static bool emit_edge_or_exit(LowerContext* ctx,
                              JitBuilder* builder,
                              BcBlock* source_block,
                              int32_t target_ip,
                              const LowerState* state,
                              JitBlockId* out_target) {
    if(target_ip < 0 || target_ip >= ctx->graph.code_count) {
        *out_target = make_exit_stub(ctx, target_ip, state);
        return true;
    }
    int32_t target_bi = ctx->graph.ip_to_block[target_ip];
    BcBlock* target = c11__at(BcBlock, &ctx->graph.blocks, target_bi);
    if(!target->selected) {
        *out_target = make_exit_stub(ctx, target_ip, state);
        return true;
    }
    if(!fill_edge_state(ctx, source_block->mir_block, target_bi, state)) return false;
    *out_target = target->mir_block;
    (void)builder;
    return true;
}

static JitValueId emit_load_fast(LowerContext* ctx,
                                 JitBuilder* builder,
                                 LowerState* state,
                                 uint16_t index) {
    if(index >= state->local_count) {
        lower_fail(ctx,
                   JIT_LOWER_INVALID_BYTECODE,
                   builder->source.bytecode_ip,
                   "LOAD_FAST local index %u is out of range",
                   (unsigned)index);
        return JIT_INVALID_VALUE;
    }
    JitValueId input = state->locals[index];
    JitImmediate imm = imm_zero();
    JitTypeInfo type = value_type(ctx, input);
    return jit_builder_emit_value(builder, JIT_OP_LOAD_LOCAL, type, &input, 1, imm, index);
}

static bool emit_store_fast(LowerContext* ctx,
                            JitBuilder* builder,
                            LowerState* state,
                            uint16_t index,
                            JitValueId value) {
    if(index >= state->local_count)
        return lower_fail(ctx,
                          JIT_LOWER_INVALID_BYTECODE,
                          builder->source.bytecode_ip,
                          "STORE_FAST local index %u is out of range",
                          (unsigned)index);
    JitImmediate imm = imm_zero();
    jit_builder_emit(builder,
                     JIT_OP_STORE_LOCAL,
                     jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                     &value,
                     1,
                     imm,
                     index);
    state->locals[index] = value;
    return true;
}

static JitOpcode generic_binary_opcode(Opcode op) {
    switch(op) {
        case OP_BINARY_ADD: return JIT_OP_ADD_VALUE;
        case OP_BINARY_SUB: return JIT_OP_SUB_VALUE;
        case OP_BINARY_MUL: return JIT_OP_MUL_VALUE;
        case OP_BINARY_TRUEDIV: return JIT_OP_TRUEDIV_VALUE;
        case OP_BINARY_FLOORDIV: return JIT_OP_FLOORDIV_VALUE;
        case OP_BINARY_MOD: return JIT_OP_MOD_VALUE;
        case OP_BINARY_POW: return JIT_OP_POW_VALUE;
        case OP_COMPARE_LT: return JIT_OP_LT_VALUE;
        case OP_COMPARE_LE: return JIT_OP_LE_VALUE;
        case OP_COMPARE_EQ: return JIT_OP_EQ_VALUE;
        case OP_COMPARE_NE: return JIT_OP_NE_VALUE;
        case OP_COMPARE_GT: return JIT_OP_GT_VALUE;
        case OP_COMPARE_GE: return JIT_OP_GE_VALUE;
        default: return JIT_OP_NOP;
    }
}

static JitOpcode i64_binary_opcode(Opcode op) {
    switch(op) {
        case OP_BINARY_ADD: return JIT_OP_ADD_I64;
        case OP_BINARY_SUB: return JIT_OP_SUB_I64;
        case OP_BINARY_MUL: return JIT_OP_MUL_I64;
        case OP_COMPARE_LT: return JIT_OP_LT_I64;
        case OP_COMPARE_LE: return JIT_OP_LE_I64;
        case OP_COMPARE_EQ: return JIT_OP_EQ_I64;
        case OP_COMPARE_NE: return JIT_OP_NE_I64;
        case OP_COMPARE_GT: return JIT_OP_GT_I64;
        case OP_COMPARE_GE: return JIT_OP_GE_I64;
        default: return JIT_OP_NOP;
    }
}

static JitOpcode f64_binary_opcode(Opcode op) {
    switch(op) {
        case OP_BINARY_ADD: return JIT_OP_ADD_F64;
        case OP_BINARY_SUB: return JIT_OP_SUB_F64;
        case OP_BINARY_MUL: return JIT_OP_MUL_F64;
        case OP_BINARY_TRUEDIV: return JIT_OP_DIV_F64;
        case OP_COMPARE_LT: return JIT_OP_LT_F64;
        case OP_COMPARE_LE: return JIT_OP_LE_F64;
        case OP_COMPARE_EQ: return JIT_OP_EQ_F64;
        case OP_COMPARE_NE: return JIT_OP_NE_F64;
        case OP_COMPARE_GT: return JIT_OP_GT_F64;
        case OP_COMPARE_GE: return JIT_OP_GE_F64;
        default: return JIT_OP_NOP;
    }
}

static JitValueId emit_binary(LowerContext* ctx,
                              JitBuilder* builder,
                              Opcode op,
                              JitValueId lhs,
                              JitValueId rhs) {
    JitValueId operands[2] = {lhs, rhs};
    JitTypeInfo lt = value_type(ctx, lhs);
    JitTypeInfo rt = value_type(ctx, rhs);
    JitOpcode typed = JIT_OP_NOP;
    JitTypeInfo out_type = jit_type_value();

    if(lt.representation == JIT_TYPE_I64 && rt.representation == JIT_TYPE_I64) {
        typed = i64_binary_opcode(op);
        if(typed != JIT_OP_NOP) {
            bool cmp = op >= OP_COMPARE_LT && op <= OP_COMPARE_GE;
            out_type = cmp ? jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL)
                           : jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT);
        }
    } else if(lt.representation == JIT_TYPE_F64 && rt.representation == JIT_TYPE_F64) {
        typed = f64_binary_opcode(op);
        if(typed != JIT_OP_NOP) {
            bool cmp = op >= OP_COMPARE_LT && op <= OP_COMPARE_GE;
            out_type = cmp ? jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL)
                           : jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT);
        }
    }

    if(typed != JIT_OP_NOP)
        return jit_builder_emit_value(builder, typed, out_type, operands, 2, imm_zero(), 0);

    JitOpcode generic = generic_binary_opcode(op);
    if(generic != JIT_OP_NOP)
        return jit_builder_emit_value(builder, generic, jit_type_value(), operands, 2, imm_zero(), 0);

    JitHelperId helper = JIT_HELPER_INVALID;
    switch(op) {
        case OP_BINARY_LSHIFT: helper = JIT_HELPER_BINARY_LSHIFT; break;
        case OP_BINARY_RSHIFT: helper = JIT_HELPER_BINARY_RSHIFT; break;
        case OP_BINARY_AND: helper = JIT_HELPER_BINARY_AND; break;
        case OP_BINARY_OR: helper = JIT_HELPER_BINARY_OR; break;
        case OP_BINARY_XOR: helper = JIT_HELPER_BINARY_XOR; break;
        case OP_BINARY_MATMUL: helper = JIT_HELPER_BINARY_MATMUL; break;
        default: break;
    }
    return emit_helper(ctx, builder, helper, jit_type_value(), operands, 2, 0);
}

static bool lower_normal_opcode(LowerContext* ctx,
                                BcBlock* bc_block,
                                JitBuilder* builder,
                                LowerState* state,
                                int32_t ip,
                                Bytecode byte,
                                bool* terminated) {
    Opcode op = (Opcode)byte.op;
    JitValueId a, b, c, result;
    JitValueId ops[8];
    *terminated = false;

    switch(op) {
        case OP_NO_OP: return true;
        case OP_POP_TOP:
            (void)stack_pop(ctx, state, ip);
            return !ctx->failed;
        case OP_DUP_TOP:
            a = stack_peek(ctx, state, 0, ip);
            return !ctx->failed && stack_push(ctx, state, a, ip);
        case OP_DUP_TOP_TWO:
            a = stack_peek(ctx, state, 1, ip);
            b = stack_peek(ctx, state, 0, ip);
            return !ctx->failed && stack_push(ctx, state, a, ip) && stack_push(ctx, state, b, ip);
        case OP_ROT_TWO:
            a = stack_peek(ctx, state, 0, ip);
            b = stack_peek(ctx, state, 1, ip);
            if(ctx->failed) return false;
            state->stack[state->stack_size - 1] = b;
            state->stack[state->stack_size - 2] = a;
            return true;
        case OP_ROT_THREE: {
            JitValueId top = stack_peek(ctx, state, 0, ip);
            JitValueId second = stack_peek(ctx, state, 1, ip);
            JitValueId third = stack_peek(ctx, state, 2, ip);
            if(ctx->failed) return false;
            state->stack[state->stack_size - 3] = top;
            state->stack[state->stack_size - 2] = third;
            state->stack[state->stack_size - 1] = second;
            return true;
        }
        case OP_PRINT_EXPR:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_PRINT_EXPR, &a, 1, byte.arg);
            return true;

        case OP_LOAD_CONST: {
            JitImmediate imm = imm_zero();
            imm.u32 = byte.arg;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_CONST_POOL,
                                            jit_type_value(),
                                            NULL,
                                            0,
                                            imm,
                                            byte.arg);
            return stack_push(ctx, state, result, ip);
        }
        case OP_LOAD_NONE: return stack_push(ctx, state, jit_builder_const_none(builder), ip);
        case OP_LOAD_TRUE: return stack_push(ctx, state, jit_builder_const_bool(builder, true), ip);
        case OP_LOAD_FALSE: return stack_push(ctx, state, jit_builder_const_bool(builder, false), ip);
        case OP_LOAD_SMALL_INT:
            return stack_push(ctx, state, jit_builder_const_i64(builder, (int16_t)byte.arg), ip);
        case OP_LOAD_NAME_AS_INT:
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_LOAD_NAME_AS_INT,
                                 jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                 NULL,
                                 0,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_ELLIPSIS: return stack_push(ctx, state, jit_builder_const_ellipsis(builder), ip);
        case OP_LOAD_FUNCTION:
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_LOAD_FUNCTION,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_FUNCTION),
                                 NULL,
                                 0,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_NULL: return stack_push(ctx, state, jit_builder_const_nil(builder), ip);

        case OP_LOAD_FAST:
            result = emit_load_fast(ctx, builder, state, byte.arg);
            return !ctx->failed && stack_push(ctx, state, result, ip);
        case OP_LOAD_NAME:
            result = emit_helper(ctx, builder, JIT_HELPER_LOAD_NAME, jit_type_value(), NULL, 0, byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_NONLOCAL: {
            JitImmediate imm = imm_zero();
            imm.u32 = byte.arg;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_NONLOCAL,
                                            jit_type_value(),
                                            NULL,
                                            0,
                                            imm,
                                            byte.arg);
            return stack_push(ctx, state, result, ip);
        }
        case OP_LOAD_GLOBAL: {
            JitImmediate imm = imm_zero();
            imm.u32 = byte.arg;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_GLOBAL,
                                            jit_type_value(),
                                            NULL,
                                            0,
                                            imm,
                                            byte.arg);
            return stack_push(ctx, state, result, ip);
        }
        case OP_LOAD_ATTR:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_ATTR,
                                            jit_type_value(),
                                            &a,
                                            1,
                                            imm_zero(),
                                            byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_SELF_ATTR:
            a = emit_load_fast(ctx, builder, state, 0);
            if(ctx->failed) return false;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_ATTR,
                                            jit_type_value(),
                                            &a,
                                            1,
                                            imm_zero(),
                                            byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_CLASS_GLOBAL:
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_LOAD_CLASS_GLOBAL,
                                 jit_type_value(),
                                 NULL,
                                 0,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_LOAD_METHOD:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_METHOD,
                                            jit_type_unknown(JIT_TYPE_TOKEN),
                                            &a,
                                            1,
                                            imm_zero(),
                                            byte.arg);
            a = emit_extract(builder, result, 0, jit_type_value());
            b = emit_extract(builder, result, 1, jit_type_value());
            return stack_push(ctx, state, a, ip) && stack_push(ctx, state, b, ip);
        case OP_LOAD_SUBSCR:
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_LOAD_SUBSCR,
                                            jit_type_value(),
                                            ops,
                                            2,
                                            imm_zero(),
                                            0);
            return stack_push(ctx, state, result, ip);

        case OP_STORE_FAST:
            a = stack_pop(ctx, state, ip);
            return !ctx->failed && emit_store_fast(ctx, builder, state, byte.arg, a);
        case OP_STORE_NAME:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_STORE_NAME, &a, 1, byte.arg);
            return true;
        case OP_STORE_GLOBAL:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            jit_builder_emit(builder,
                             JIT_OP_STORE_GLOBAL,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             &a,
                             1,
                             imm_zero(),
                             byte.arg);
            return true;
        case OP_STORE_ATTR:
            a = stack_pop(ctx, state, ip); /* object */
            b = stack_pop(ctx, state, ip); /* value */
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            jit_builder_emit(builder,
                             JIT_OP_STORE_ATTR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             ops,
                             2,
                             imm_zero(),
                             byte.arg);
            return true;
        case OP_STORE_SELF_ATTR:
            b = stack_pop(ctx, state, ip);
            a = emit_load_fast(ctx, builder, state, 0);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            jit_builder_emit(builder,
                             JIT_OP_STORE_ATTR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             ops,
                             2,
                             imm_zero(),
                             byte.arg);
            return true;
        case OP_STORE_SUBSCR:
            c = stack_pop(ctx, state, ip); /* index */
            b = stack_pop(ctx, state, ip); /* object */
            a = stack_pop(ctx, state, ip); /* value */
            if(ctx->failed) return false;
            ops[0] = b;
            ops[1] = c;
            ops[2] = a;
            jit_builder_emit(builder,
                             JIT_OP_STORE_SUBSCR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             ops,
                             3,
                             imm_zero(),
                             0);
            return true;

        case OP_DELETE_FAST:
            if(byte.arg >= state->local_count)
                return lower_fail(ctx, JIT_LOWER_INVALID_BYTECODE, ip, "DELETE_FAST local out of range");
            a = state->locals[byte.arg];
            emit_helper_void(ctx, builder, JIT_HELPER_DELETE_FAST, &a, 1, byte.arg);
            state->locals[byte.arg] = jit_builder_const_nil(builder);
            return true;
        case OP_DELETE_NAME:
            emit_helper_void(ctx, builder, JIT_HELPER_DELETE_NAME, NULL, 0, byte.arg);
            return true;
        case OP_DELETE_GLOBAL:
            emit_helper_void(ctx, builder, JIT_HELPER_DELETE_GLOBAL, NULL, 0, byte.arg);
            return true;
        case OP_DELETE_ATTR:
            a = stack_peek(ctx, state, 0, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_DELETE_ATTR, &a, 1, byte.arg);
            return true;
        case OP_DELETE_SUBSCR:
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            emit_helper_void(ctx, builder, JIT_HELPER_DELETE_SUBSCR, ops, 2, byte.arg);
            return true;

        case OP_BUILD_IMAG:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx, builder, JIT_HELPER_BUILD_IMAG, jit_type_value(), &a, 1, byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_BUILD_BYTES:
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_BUILD_BYTES,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_BYTES),
                                 NULL,
                                 0,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_BUILD_TUPLE:
        case OP_BUILD_LIST:
        case OP_BUILD_SET: {
            uint32_t count = byte.arg;
            JitValueId* values = count == 0 ? NULL : malloc(sizeof(JitValueId) * (size_t)count);
            if(count != 0 && values == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
            for(uint32_t i = count; i > 0; i--) values[i - 1] = stack_pop(ctx, state, ip);
            if(ctx->failed) {
                free(values);
                return false;
            }
            JitOpcode mir_op = op == OP_BUILD_TUPLE   ? JIT_OP_BUILD_TUPLE
                               : op == OP_BUILD_LIST ? JIT_OP_BUILD_LIST
                                                     : JIT_OP_BUILD_SET;
            JitKnownType known = op == OP_BUILD_TUPLE   ? JIT_KNOWN_TUPLE
                                 : op == OP_BUILD_LIST ? JIT_KNOWN_LIST
                                                       : JIT_KNOWN_SET;
            result = jit_builder_emit_value(builder,
                                            mir_op,
                                            jit_type_exact(JIT_TYPE_VALUE, known),
                                            values,
                                            count,
                                            imm_zero(),
                                            count);
            free(values);
            return stack_push(ctx, state, result, ip);
        }
        case OP_BUILD_DICT: {
            uint32_t count = (uint32_t)byte.arg * 2u;
            JitValueId* values = count == 0 ? NULL : malloc(sizeof(JitValueId) * (size_t)count);
            if(count != 0 && values == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
            for(uint32_t i = count; i > 0; i--) values[i - 1] = stack_pop(ctx, state, ip);
            if(ctx->failed) {
                free(values);
                return false;
            }
            result = jit_builder_emit_value(builder,
                                            JIT_OP_BUILD_DICT,
                                            jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_DICT),
                                            values,
                                            count,
                                            imm_zero(),
                                            byte.arg);
            free(values);
            return stack_push(ctx, state, result, ip);
        }
        case OP_BUILD_SLICE:
            c = stack_pop(ctx, state, ip);
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            ops[2] = c;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_BUILD_SLICE,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_SLICE),
                                 ops,
                                 3,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_BUILD_STRING: {
            uint32_t count = byte.arg;
            JitValueId* values = count == 0 ? NULL : malloc(sizeof(JitValueId) * (size_t)count);
            if(count != 0 && values == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
            for(uint32_t i = count; i > 0; i--) values[i - 1] = stack_pop(ctx, state, ip);
            if(ctx->failed) {
                free(values);
                return false;
            }
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_BUILD_STRING,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_STR),
                                 values,
                                 count,
                                 byte.arg);
            free(values);
            return stack_push(ctx, state, result, ip);
        }

        case OP_BINARY_ADD:
        case OP_BINARY_SUB:
        case OP_BINARY_MUL:
        case OP_BINARY_TRUEDIV:
        case OP_BINARY_FLOORDIV:
        case OP_BINARY_MOD:
        case OP_BINARY_POW:
        case OP_BINARY_LSHIFT:
        case OP_BINARY_RSHIFT:
        case OP_BINARY_AND:
        case OP_BINARY_OR:
        case OP_BINARY_XOR:
        case OP_BINARY_MATMUL:
        case OP_COMPARE_LT:
        case OP_COMPARE_LE:
        case OP_COMPARE_EQ:
        case OP_COMPARE_NE:
        case OP_COMPARE_GT:
        case OP_COMPARE_GE:
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_binary(ctx, builder, op, a, b);
            return stack_push(ctx, state, result, ip);

        case OP_IS_OP:
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_IS_IDENTICAL,
                                            jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                            ops,
                                            2,
                                            imm_zero(),
                                            byte.arg);
            if(byte.arg)
                result = jit_builder_emit_value(builder,
                                                JIT_OP_NOT_BOOL,
                                                jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                                &result,
                                                1,
                                                imm_zero(),
                                                0);
            return stack_push(ctx, state, result, ip);
        case OP_CONTAINS_OP:
            b = stack_pop(ctx, state, ip);
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            ops[0] = a;
            ops[1] = b;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_CONTAINS,
                                 jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                 ops,
                                 2,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);

        case OP_CALL:
        case OP_CALL_VARGS: {
            uint32_t argc = byte.arg & 0xFFu;
            uint32_t kwargc = byte.arg >> 8;
            uint32_t count = 2u + argc + kwargc * 2u;
            JitValueId* values = malloc(sizeof(JitValueId) * (size_t)count);
            if(values == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
            for(uint32_t i = count; i > 0; i--) values[i - 1] = stack_pop(ctx, state, ip);
            if(ctx->failed) {
                free(values);
                return false;
            }
            JitImmediate imm = imm_zero();
            uint32_t aux = (uint32_t)byte.arg;
            if(op == OP_CALL_VARGS) aux |= JIT_CALL_FLAG_VARGS;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_CALL,
                                            jit_type_value(),
                                            values,
                                            count,
                                            imm,
                                            aux);
            free(values);
            return stack_push(ctx, state, result, ip);
        }

        case OP_LIST_APPEND:
            a = stack_pop(ctx, state, ip); /* value */
            if(ctx->failed) return false;
            b = stack_peek(ctx, state, 1, ip); /* list below iterator */
            if(ctx->failed) return false;
            ops[0] = b;
            ops[1] = a;
            emit_helper_void(ctx, builder, JIT_HELPER_LIST_APPEND, ops, 2, byte.arg);
            return true;
        case OP_DICT_ADD:
            a = stack_pop(ctx, state, ip); /* value */
            b = stack_pop(ctx, state, ip); /* key */
            if(ctx->failed) return false;
            c = stack_peek(ctx, state, 1, ip); /* dict below iterator */
            if(ctx->failed) return false;
            ops[0] = c;
            ops[1] = b;
            ops[2] = a;
            emit_helper_void(ctx, builder, JIT_HELPER_DICT_ADD, ops, 3, byte.arg);
            return true;
        case OP_SET_ADD:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            b = stack_peek(ctx, state, 1, ip);
            if(ctx->failed) return false;
            ops[0] = b;
            ops[1] = a;
            emit_helper_void(ctx, builder, JIT_HELPER_SET_ADD, ops, 2, byte.arg);
            return true;

        case OP_UNARY_NEGATIVE:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            if(value_type(ctx, a).representation == JIT_TYPE_I64)
                result = jit_builder_emit_value(builder,
                                                JIT_OP_NEG_I64,
                                                jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                                &a,
                                                1,
                                                imm_zero(),
                                                0);
            else if(value_type(ctx, a).representation == JIT_TYPE_F64)
                result = jit_builder_emit_value(builder,
                                                JIT_OP_NEG_F64,
                                                jit_type_exact(JIT_TYPE_F64, JIT_KNOWN_FLOAT),
                                                &a,
                                                1,
                                                imm_zero(),
                                                0);
            else
                result = emit_helper(ctx,
                                     builder,
                                     JIT_HELPER_UNARY_NEGATIVE,
                                     jit_type_value(),
                                     &a,
                                     1,
                                     byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_UNARY_NOT:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_TRUTHY,
                                            jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                            &a,
                                            1,
                                            imm_zero(),
                                            0);
            result = jit_builder_emit_value(builder,
                                            JIT_OP_NOT_BOOL,
                                            jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                            &result,
                                            1,
                                            imm_zero(),
                                            0);
            return stack_push(ctx, state, result, ip);
        case OP_UNARY_STAR:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_UNARY_STAR,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_STAR_WRAPPER),
                                 &a,
                                 1,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_UNARY_INVERT:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            if(value_type(ctx, a).representation == JIT_TYPE_I64)
                result = jit_builder_emit_value(builder,
                                                JIT_OP_NOT_I64,
                                                jit_type_exact(JIT_TYPE_I64, JIT_KNOWN_INT),
                                                &a,
                                                1,
                                                imm_zero(),
                                                0);
            else
                result = emit_helper(ctx,
                                     builder,
                                     JIT_HELPER_UNARY_INVERT,
                                     jit_type_value(),
                                     &a,
                                     1,
                                     byte.arg);
            return stack_push(ctx, state, result, ip);

        case OP_GET_ITER:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = jit_builder_emit_value(builder,
                                            JIT_OP_GET_ITER,
                                            jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_ITERATOR),
                                            &a,
                                            1,
                                            imm_zero(),
                                            0);
            return stack_push(ctx, state, result, ip);

        case OP_IMPORT_PATH:
            result = emit_helper(ctx, builder, JIT_HELPER_IMPORT_PATH, jit_type_value(), NULL, 0, byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_POP_IMPORT_STAR:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_POP_IMPORT_STAR, &a, 1, byte.arg);
            return true;

        case OP_UNPACK_SEQUENCE:
        case OP_UNPACK_EX: {
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            JitHelperId helper = op == OP_UNPACK_SEQUENCE ? JIT_HELPER_UNPACK_SEQUENCE : JIT_HELPER_UNPACK_EX;
            result = emit_helper(ctx,
                                 builder,
                                 helper,
                                 jit_type_unknown(JIT_TYPE_TOKEN),
                                 &a,
                                 1,
                                 byte.arg);
            uint32_t count = op == OP_UNPACK_SEQUENCE ? byte.arg : (uint32_t)byte.arg + 1u;
            for(uint32_t i = 0; i < count; i++) {
                JitTypeInfo type = op == OP_UNPACK_EX && i == count - 1
                                       ? jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_LIST)
                                       : jit_type_value();
                JitValueId item = emit_extract(builder, result, i, type);
                if(!stack_push(ctx, state, item, ip)) return false;
            }
            return true;
        }

        case OP_BEGIN_CLASS:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_BEGIN_CLASS,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_TYPE),
                                 &a,
                                 1,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_END_CLASS:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_END_CLASS, &a, 1, byte.arg);
            return true;
        case OP_STORE_CLASS_ATTR:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_STORE_CLASS_ATTR, &a, 1, byte.arg);
            return true;
        case OP_ADD_CLASS_ANNOTATION:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_ADD_CLASS_ANNOTATION, &a, 1, byte.arg);
            return true;

        case OP_WITH_ENTER:
            a = stack_peek(ctx, state, 0, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx, builder, JIT_HELPER_WITH_ENTER, jit_type_value(), &a, 1, byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_WITH_EXIT:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_WITH_EXIT, &a, 1, byte.arg);
            return true;

        case OP_BEGIN_TRY: {
            JitStateId sid = add_resume_state(ctx, ip, state);
            JitImmediate imm = imm_zero();
            jit_builder_emit(builder,
                             JIT_OP_SYNC_STATE,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             NULL,
                             0,
                             imm,
                             sid);
            emit_helper_void(ctx, builder, JIT_HELPER_BEGIN_TRY, NULL, 0, state->stack_size);
            ctx->region->contains_exception_handlers = true;
            return true;
        }
        case OP_END_TRY:
            emit_helper_void(ctx, builder, JIT_HELPER_END_TRY, NULL, 0, byte.arg);
            ctx->region->contains_exception_handlers = true;
            return true;
        case OP_EXCEPTION_MATCH:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_EXCEPTION_MATCH,
                                 jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                 &a,
                                 1,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);
        case OP_HANDLE_EXCEPTION:
            emit_helper_void(ctx, builder, JIT_HELPER_HANDLE_EXCEPTION, NULL, 0, byte.arg);
            return true;
        case OP_PUSH_EXCEPTION:
            result = emit_helper(ctx, builder, JIT_HELPER_PUSH_EXCEPTION, jit_type_value(), NULL, 0, byte.arg);
            return stack_push(ctx, state, result, ip);

        case OP_FORMAT_STRING:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            result = emit_helper(ctx,
                                 builder,
                                 JIT_HELPER_FORMAT_STRING,
                                 jit_type_exact(JIT_TYPE_VALUE, JIT_KNOWN_STR),
                                 &a,
                                 1,
                                 byte.arg);
            return stack_push(ctx, state, result, ip);

        case OP_JUMP_FORWARD:
        case OP_LOOP_CONTINUE:
        case OP_LOOP_BREAK: {
            int32_t target_ip = jump_target(ip, byte.arg);
            if(ctx->options->poll_loop_backedges && target_ip <= ip)
                jit_builder_emit(builder,
                                 JIT_OP_POLL,
                                 jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                                 NULL,
                                 0,
                                 imm_zero(),
                                 0);
            JitBlockId target;
            if(!emit_edge_or_exit(ctx, builder, bc_block, target_ip, state, &target)) return false;
            jit_builder_jump(builder, target);
            *terminated = true;
            return true;
        }

        case OP_POP_JUMP_IF_NOT_MATCH:
        case OP_POP_JUMP_IF_FALSE:
        case OP_POP_JUMP_IF_TRUE:
        case OP_JUMP_IF_TRUE_OR_POP:
        case OP_JUMP_IF_FALSE_OR_POP:
        case OP_SHORTCUT_IF_FALSE_OR_POP:
        case OP_FOR_ITER: {
            int32_t target_ip = jump_target(ip, byte.arg);
            int32_t fall_ip = ip + 1;
            LowerState target_state;
            LowerState fall_state;
            if(!lower_state_init(&target_state, state->local_count, state->stack_capacity) ||
               !lower_state_init(&fall_state, state->local_count, state->stack_capacity)) {
                lower_state_destroy(&target_state);
                lower_state_destroy(&fall_state);
                return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, ip, "out of memory");
            }
            memcpy(target_state.locals, state->locals, sizeof(JitValueId) * state->local_count);
            memcpy(fall_state.locals, state->locals, sizeof(JitValueId) * state->local_count);
            memcpy(target_state.stack, state->stack, sizeof(JitValueId) * state->stack_size);
            memcpy(fall_state.stack, state->stack, sizeof(JitValueId) * state->stack_size);
            target_state.stack_size = state->stack_size;
            fall_state.stack_size = state->stack_size;

            JitValueId cond;
            bool target_on_true = true;

            if(op == OP_FOR_ITER) {
                a = stack_peek(ctx, state, 0, ip);
                if(ctx->failed) goto branch_fail;
                result = jit_builder_emit_value(builder,
                                                JIT_OP_ITER_NEXT,
                                                jit_type_unknown(JIT_TYPE_TOKEN),
                                                &a,
                                                1,
                                                imm_zero(),
                                                0);
                cond = emit_extract(builder,
                                    result,
                                    0,
                                    jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL));
                JitValueId item = emit_extract(builder, result, 1, jit_type_value());
                (void)stack_pop(ctx, &target_state, ip); /* iterator is removed on exhaustion */
                if(!stack_push(ctx, &fall_state, item, ip)) goto branch_fail;
                target_on_true = false; /* true means an item was produced -> fallthrough */
            } else if(op == OP_POP_JUMP_IF_NOT_MATCH) {
                b = stack_pop(ctx, state, ip);
                a = stack_pop(ctx, state, ip);
                if(ctx->failed) goto branch_fail;
                ops[0] = a;
                ops[1] = b;
                cond = emit_helper(ctx,
                                   builder,
                                   JIT_HELPER_POP_JUMP_IF_NOT_MATCH,
                                   jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                   ops,
                                   2,
                                   byte.arg);
                (void)stack_pop(ctx, &target_state, ip);
                (void)stack_pop(ctx, &target_state, ip);
                (void)stack_pop(ctx, &fall_state, ip);
                (void)stack_pop(ctx, &fall_state, ip);
                target_on_true = false;
            } else if(op == OP_SHORTCUT_IF_FALSE_OR_POP) {
                a = stack_peek(ctx, state, 0, ip);
                if(ctx->failed) goto branch_fail;
                cond = jit_builder_emit_value(builder,
                                              JIT_OP_TRUTHY,
                                              jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                              &a,
                                              1,
                                              imm_zero(),
                                              0);
                (void)stack_pop(ctx, &fall_state, ip);
                (void)stack_pop(ctx, &target_state, ip);
                (void)stack_pop(ctx, &target_state, ip);
                if(!stack_push(ctx, &target_state, jit_builder_const_bool(builder, false), ip))
                    goto branch_fail;
                target_on_true = false;
            } else {
                a = stack_peek(ctx, state, 0, ip);
                if(ctx->failed) goto branch_fail;
                cond = jit_builder_emit_value(builder,
                                              JIT_OP_TRUTHY,
                                              jit_type_exact(JIT_TYPE_BOOL, JIT_KNOWN_BOOL),
                                              &a,
                                              1,
                                              imm_zero(),
                                              0);
                if(op == OP_POP_JUMP_IF_FALSE || op == OP_POP_JUMP_IF_TRUE) {
                    (void)stack_pop(ctx, &target_state, ip);
                    (void)stack_pop(ctx, &fall_state, ip);
                } else {
                    (void)stack_pop(ctx, &fall_state, ip);
                }
                target_on_true = op == OP_POP_JUMP_IF_TRUE || op == OP_JUMP_IF_TRUE_OR_POP;
            }

            {
                JitBlockId target_block;
                JitBlockId fall_block;
                if(!emit_edge_or_exit(ctx, builder, bc_block, target_ip, &target_state, &target_block) ||
                   !emit_edge_or_exit(ctx, builder, bc_block, fall_ip, &fall_state, &fall_block))
                    goto branch_fail;

                if(ctx->options->poll_loop_backedges && target_ip <= ip)
                    jit_builder_emit(builder,
                                     JIT_OP_POLL,
                                     jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                                     NULL,
                                     0,
                                     imm_zero(),
                                     0);

                if(target_on_true)
                    jit_builder_branch(builder, cond, target_block, fall_block);
                else
                    jit_builder_branch(builder, cond, fall_block, target_block);
            }
            lower_state_destroy(&target_state);
            lower_state_destroy(&fall_state);
            *terminated = true;
            return true;

        branch_fail:
            lower_state_destroy(&target_state);
            lower_state_destroy(&fall_state);
            return false;
        }

        case OP_RETURN_VALUE:
            if(byte.arg == BC_NOARG)
                a = stack_pop(ctx, state, ip);
            else
                a = jit_builder_const_none(builder);
            if(ctx->failed) return false;
            jit_builder_return(builder, a);
            *terminated = true;
            return true;

        case OP_YIELD_VALUE:
        case OP_FOR_ITER_YIELD_VALUE: {
            if(!ctx->options->allow_generators)
                return lower_fail(ctx,
                                  JIT_LOWER_UNSUPPORTED_GENERATOR,
                                  ip,
                                  "generator bytecode requires a resumable JIT entry table");
            JitStateId sid = add_resume_state(ctx, ip, state);
            jit_builder_exit_interpreter(builder, sid);
            *terminated = true;
            return true;
        }

        case OP_RAISE:
            a = stack_pop(ctx, state, ip);
            if(ctx->failed) return false;
            emit_helper_void(ctx, builder, JIT_HELPER_RAISE, &a, 1, byte.arg);
            jit_builder_emit(builder,
                             JIT_OP_ERROR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             NULL,
                             0,
                             imm_zero(),
                             0);
            *terminated = true;
            return true;
        case OP_RAISE_ASSERT:
            if(byte.arg) {
                a = stack_pop(ctx, state, ip);
                if(ctx->failed) return false;
                emit_helper_void(ctx, builder, JIT_HELPER_RAISE_ASSERT, &a, 1, byte.arg);
            } else {
                emit_helper_void(ctx, builder, JIT_HELPER_RAISE_ASSERT, NULL, 0, byte.arg);
            }
            jit_builder_emit(builder,
                             JIT_OP_ERROR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             NULL,
                             0,
                             imm_zero(),
                             0);
            *terminated = true;
            return true;
        case OP_RE_RAISE:
            emit_helper_void(ctx, builder, JIT_HELPER_RE_RAISE, NULL, 0, byte.arg);
            jit_builder_emit(builder,
                             JIT_OP_ERROR,
                             jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                             NULL,
                             0,
                             imm_zero(),
                             0);
            *terminated = true;
            return true;

    }

    return lower_fail(ctx, JIT_LOWER_INVALID_BYTECODE, ip, "unhandled opcode %u", (unsigned)byte.op);
}

static bool create_mir_blocks(LowerContext* ctx) {
    uint32_t frequency = ctx->options->estimated_execution_count;
    if(frequency == 0) frequency = 1;

    ctx->synthetic_entry = jit_region_add_block(ctx->region, frequency, JIT_BLOCK_ENTRY);

    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        if(!block->selected) continue;
        uint32_t flags = 0;
        c11__foreach(int32_t, &block->successors, succ) {
            if(!c11__at(BcBlock, &ctx->graph.blocks, *succ)->selected) continue;
            if(block_is_backedge(&ctx->graph, bi, *succ)) {
                flags |= JIT_BLOCK_LOOP_LATCH;
                c11__at(BcBlock, &ctx->graph.blocks, *succ)->selected = true;
            }
        }
        block->mir_block = jit_region_add_block(ctx->region, frequency, flags);
        ctx->stats->selected_block_count++;
        ctx->stats->selected_bytecode_count += (uint32_t)(block->end_ip - block->start_ip);
    }

    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        if(!block->selected) continue;
        if(bi == ctx->entry_bc_block) vector_push_unique_block(&block->mir_predecessors, ctx->synthetic_entry);
        c11__foreach(int32_t, &block->predecessors, pred) {
            BcBlock* pb = c11__at(BcBlock, &ctx->graph.blocks, *pred);
            if(pb->selected) vector_push_unique_block(&block->mir_predecessors, pb->mir_block);
        }

        if(block->mir_predecessors.length == 0)
            return lower_fail(ctx,
                              JIT_LOWER_INVALID_BYTECODE,
                              block->start_ip,
                              "selected block has no region predecessor");

        JitBuilder builder;
        jit_builder_init(&builder, ctx->region, block->mir_block);
        jit_builder_set_source(&builder, block->start_ip, -1);
        uint32_t pred_count = (uint32_t)block->mir_predecessors.length;
        JitValueId* placeholders = malloc(sizeof(JitValueId) * pred_count);
        if(placeholders == NULL) return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, -1, "out of memory");
        for(uint32_t i = 0; i < pred_count; i++) placeholders[i] = JIT_INVALID_VALUE;

        for(int32_t i = 0; i < ctx->co->nlocals; i++) {
            JitValueId phi = jit_builder_emit_value(&builder,
                                                    JIT_OP_PHI,
                                                    jit_type_value(),
                                                    placeholders,
                                                    pred_count,
                                                    imm_zero(),
                                                    0);
            c11_vector__push(JitValueId, &block->entry_locals, phi);
            ctx->stats->phi_count++;
        }
        for(int32_t i = 0; i < block->entry_depth; i++) {
            JitValueId phi = jit_builder_emit_value(&builder,
                                                    JIT_OP_PHI,
                                                    jit_type_value(),
                                                    placeholders,
                                                    pred_count,
                                                    imm_zero(),
                                                    0);
            c11_vector__push(JitValueId, &block->entry_stack, phi);
            ctx->stats->phi_count++;
        }
        free(placeholders);
    }

    return true;
}

static bool lower_synthetic_entry(LowerContext* ctx) {
    BcBlock* entry = c11__at(BcBlock, &ctx->graph.blocks, ctx->entry_bc_block);
    LowerState state;
    if(!lower_state_init(&state, (uint32_t)ctx->co->nlocals, ctx->graph.max_stack_depth + 8))
        return lower_fail(ctx, JIT_LOWER_INVALID_ARGUMENT, -1, "out of memory");

    JitBuilder builder;
    jit_builder_init(&builder, ctx->region, ctx->synthetic_entry);
    jit_builder_set_source(&builder, entry->start_ip, -1);

    for(uint32_t i = 0; i < state.local_count; i++) {
        JitImmediate imm = imm_zero();
        imm.u32 = i;
        state.locals[i] = jit_builder_emit_value(&builder,
                                                 JIT_OP_LOCAL,
                                                 jit_type_value(),
                                                 NULL,
                                                 0,
                                                 imm,
                                                 i);
    }
    state.stack_size = (uint32_t)entry->entry_depth;
    for(uint32_t i = 0; i < state.stack_size; i++) {
        JitImmediate imm = imm_zero();
        imm.u32 = i;
        state.stack[i] = jit_builder_emit_value(&builder,
                                                JIT_OP_STACK_VALUE,
                                                jit_type_value(),
                                                NULL,
                                                0,
                                                imm,
                                                i);
    }

    if(!fill_edge_state(ctx, ctx->synthetic_entry, ctx->entry_bc_block, &state)) {
        lower_state_destroy(&state);
        return false;
    }
    jit_builder_jump(&builder, entry->mir_block);
    lower_state_destroy(&state);
    return true;
}

static bool lower_selected_blocks(LowerContext* ctx) {
    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* block = c11__at(BcBlock, &ctx->graph.blocks, bi);
        if(!block->selected) continue;

        LowerState state;
        if(!lower_state_from_block(ctx, block, &state)) return false;
        JitBuilder builder;
        jit_builder_init(&builder, ctx->region, block->mir_block);

        bool terminated = false;
        for(int32_t ip = block->start_ip; ip < block->end_ip; ip++) {
            Bytecode byte = c11__getitem(Bytecode, &ctx->co->codes, ip);
            int32_t line = -1;
            if(ip < ctx->co->codes_ex.length) line = c11__getitem(BytecodeEx, &ctx->co->codes_ex, ip).lineno;
            jit_builder_set_source(&builder, ip, line);

            LowerState before;
            if(!lower_state_clone(ctx, &state, &before, ip)) {
                lower_state_destroy(&state);
                return false;
            }
            int32_t first_inst = ctx->region->insts.length;

            if(!lower_normal_opcode(ctx, block, &builder, &state, ip, byte, &terminated)) {
                lower_state_destroy(&before);
                lower_state_destroy(&state);
                return false;
            }

            bool needs_resume_state = false;
            for(int32_t ii = first_inst; ii < ctx->region->insts.length; ii++) {
                JitInst* inst = c11__at(JitInst, &ctx->region->insts, ii);
                if((inst->effects & JIT_EFFECT_RAISE) && inst->resume_state == JIT_INVALID_STATE) {
                    needs_resume_state = true;
                    break;
                }
            }
            if(needs_resume_state) {
                JitStateId state_id = add_resume_state(ctx, ip, &before);
                for(int32_t ii = first_inst; ii < ctx->region->insts.length; ii++) {
                    JitInst* inst = c11__at(JitInst, &ctx->region->insts, ii);
                    if((inst->effects & JIT_EFFECT_RAISE) && inst->resume_state == JIT_INVALID_STATE)
                        inst->resume_state = state_id;
                }
            }
            lower_state_destroy(&before);
            if(terminated) break;
        }

        if(!terminated) {
            int32_t target_ip = block->end_ip;
            JitBlockId target;
            if(!emit_edge_or_exit(ctx, &builder, block, target_ip, &state, &target)) {
                lower_state_destroy(&state);
                return false;
            }
            if(ctx->options->poll_loop_backedges && target_ip <= block->start_ip)
                jit_builder_emit(&builder,
                                 JIT_OP_POLL,
                                 jit_type_exact(JIT_TYPE_VOID, JIT_KNOWN_UNKNOWN),
                                 NULL,
                                 0,
                                 imm_zero(),
                                 0);
            jit_builder_jump(&builder, target);
        }

        lower_state_destroy(&state);
    }
    return true;
}

static bool validate_all_phi_inputs(LowerContext* ctx) {
    c11__foreach(JitInst, &ctx->region->insts, inst) {
        if(inst->removed || inst->op != JIT_OP_PHI) continue;
        const JitValueId* operands = jit_inst_operands(ctx->region, inst);
        for(uint32_t i = 0; i < inst->operands.count; i++) {
            if(operands[i] == JIT_INVALID_VALUE)
                return lower_fail(ctx,
                                  JIT_LOWER_INVALID_BYTECODE,
                                  inst->source.bytecode_ip,
                                  "Phi has an unfilled predecessor operand");
        }
    }
    return true;
}

static void add_loop_metadata(LowerContext* ctx) {
    for(int32_t bi = 0; bi < ctx->graph.blocks.length; bi++) {
        BcBlock* latch = c11__at(BcBlock, &ctx->graph.blocks, bi);
        if(!latch->selected) continue;
        c11__foreach(int32_t, &latch->successors, succ) {
            BcBlock* header = c11__at(BcBlock, &ctx->graph.blocks, *succ);
            if(!header->selected || !block_is_backedge(&ctx->graph, bi, *succ)) continue;

            jit_region_block(ctx->region, header->mir_block)->flags |= JIT_BLOCK_LOOP_HEADER;
            jit_region_block(ctx->region, latch->mir_block)->flags |= JIT_BLOCK_LOOP_LATCH;

            c11_vector blocks;
            c11_vector__ctor(&blocks, sizeof(JitBlockId));
            vector_push_unique_block(&blocks, header->mir_block);
            vector_push_unique_block(&blocks, latch->mir_block);

            bool* seen = calloc((size_t)ctx->graph.blocks.length, sizeof(bool));
            c11_vector work;
            c11_vector__ctor(&work, sizeof(int32_t));
            if(seen != NULL) {
                seen[*succ] = true;
                seen[bi] = true;
                c11_vector__push(int32_t, &work, bi);
                for(int wi = 0; wi < work.length; wi++) {
                    int32_t curr = c11__getitem(int32_t, &work, wi);
                    BcBlock* cb = c11__at(BcBlock, &ctx->graph.blocks, curr);
                    c11__foreach(int32_t, &cb->predecessors, pred) {
                        if(seen[*pred]) continue;
                        BcBlock* pb = c11__at(BcBlock, &ctx->graph.blocks, *pred);
                        if(!pb->selected) continue;
                        seen[*pred] = true;
                        vector_push_unique_block(&blocks, pb->mir_block);
                        if(*pred != *succ) c11_vector__push(int32_t, &work, *pred);
                    }
                }
            }
            free(seen);
            c11_vector__dtor(&work);

            jit_region_add_loop(ctx->region,
                                header->mir_block,
                                latch->mir_block,
                                blocks.data,
                                (uint32_t)blocks.length,
                                1);
            c11_vector__dtor(&blocks);
            ctx->stats->loop_count++;
        }
    }
}

static bool lower_impl(const CodeObject* co,
                       int32_t loop_header_ip,
                       const JitFrontendOptions* options_in,
                       JitRegion* out_region,
                       JitFrontendStats* out_stats,
                       JitLowerError* error) {
    clear_error(error);
    if(co == NULL || out_region == NULL) {
        if(error != NULL) {
            error->status = JIT_LOWER_INVALID_ARGUMENT;
            snprintf(error->message, sizeof(error->message), "CodeObject and out_region must be non-null");
        }
        return false;
    }

    JitFrontendOptions default_options;
    if(options_in == NULL) {
        jit_frontend_options_init(&default_options);
        options_in = &default_options;
    }

    JitFrontendStats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    JitFrontendStats* stats = out_stats != NULL ? out_stats : &local_stats;
    memset(stats, 0, sizeof(*stats));
    stats->bytecode_count = (uint32_t)co->codes.length;

    LowerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.co = co;
    ctx.options = options_in;
    ctx.region = out_region;
    ctx.stats = stats;
    ctx.error = error;

    if(!build_bc_graph(&ctx) || !analyze_stack_depths(&ctx)) {
        bc_graph_destroy(&ctx.graph);
        return false;
    }
    stats->basic_block_count = (uint32_t)ctx.graph.blocks.length;
    stats->max_stack_depth = ctx.graph.max_stack_depth;

    bool loop_mode = loop_header_ip >= 0;
    if(loop_mode) {
        if(!select_loop_blocks(&ctx, loop_header_ip)) {
            bc_graph_destroy(&ctx.graph);
            return false;
        }
    } else {
        select_function_blocks(&ctx);
    }

    JitRegionKind kind = loop_mode ? JIT_REGION_LOOP : JIT_REGION_FUNCTION;
    jit_region_init(out_region, kind, co, c11__at(BcBlock, &ctx.graph.blocks, ctx.entry_bc_block)->start_ip, loop_header_ip);
    out_region->estimated_execution_count = options_in->estimated_execution_count;

    if(!create_mir_blocks(&ctx) || !lower_synthetic_entry(&ctx) || !lower_selected_blocks(&ctx) ||
       !validate_all_phi_inputs(&ctx)) {
        jit_region_destroy(out_region);
        bc_graph_destroy(&ctx.graph);
        return false;
    }

    jit_region_rebuild_cfg(out_region);
    jit_region_recompute_uses(out_region);
    add_loop_metadata(&ctx);

    if(options_in->verify) {
        char verify_error[256];
        if(!jit_region_verify(out_region, verify_error, sizeof(verify_error))) {
            lower_fail(&ctx, JIT_LOWER_VERIFY_FAILED, -1, "%s", verify_error);
            jit_region_destroy(out_region);
            bc_graph_destroy(&ctx.graph);
            return false;
        }
    }

    bc_graph_destroy(&ctx.graph);
    return true;
}

bool jit_lower_code_object(const CodeObject* co,
                           const JitFrontendOptions* options,
                           JitRegion* out_region,
                           JitFrontendStats* out_stats,
                           JitLowerError* error) {
    return lower_impl(co, -1, options, out_region, out_stats, error);
}

bool jit_lower_funcdecl(const FuncDecl* decl,
                        const JitFrontendOptions* options,
                        JitRegion* out_region,
                        JitFrontendStats* out_stats,
                        JitLowerError* error) {
    if(decl == NULL) {
        clear_error(error);
        if(error != NULL) {
            error->status = JIT_LOWER_INVALID_ARGUMENT;
            snprintf(error->message, sizeof(error->message), "FuncDecl must be non-null");
        }
        return false;
    }
    if(decl->type == FuncType_GENERATOR && (options == NULL || !options->allow_generators)) {
        clear_error(error);
        if(error != NULL) {
            error->status = JIT_LOWER_UNSUPPORTED_GENERATOR;
            error->bytecode_ip = 0;
            snprintf(error->message,
                     sizeof(error->message),
                     "generator JIT lowering is disabled until resumable native entries are available");
        }
        return false;
    }
    return jit_lower_code_object(&decl->code, options, out_region, out_stats, error);
}

bool jit_lower_loop(const CodeObject* co,
                    int32_t header_ip,
                    const JitFrontendOptions* options,
                    JitRegion* out_region,
                    JitFrontendStats* out_stats,
                    JitLowerError* error) {
    return lower_impl(co, header_ip, options, out_region, out_stats, error);
}
