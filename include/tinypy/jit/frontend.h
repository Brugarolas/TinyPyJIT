#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tinypy/jit/ir.h"
#include "tinypy/objects/codeobject.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime helpers referenced by CALL_HELPER MIR instructions.
 *
 * The MIR -> LIR lowering owns the actual helper ABI. Keeping helper identities
 * explicit here lets the bytecode frontend remain independent of SLJIT details.
 */
typedef enum JitHelperId {
    JIT_HELPER_INVALID = 0,

    JIT_HELPER_PRINT_EXPR,
    JIT_HELPER_LOAD_ELLIPSIS,
    JIT_HELPER_LOAD_NAME_AS_INT,
    JIT_HELPER_LOAD_FUNCTION,
    JIT_HELPER_LOAD_NAME,
    JIT_HELPER_LOAD_CLASS_GLOBAL,
    JIT_HELPER_STORE_NAME,
    JIT_HELPER_DELETE_FAST,
    JIT_HELPER_DELETE_NAME,
    JIT_HELPER_DELETE_GLOBAL,
    JIT_HELPER_DELETE_ATTR,
    JIT_HELPER_DELETE_SUBSCR,

    JIT_HELPER_BUILD_IMAG,
    JIT_HELPER_BUILD_BYTES,
    JIT_HELPER_BUILD_SLICE,
    JIT_HELPER_BUILD_STRING,

    JIT_HELPER_BINARY_LSHIFT,
    JIT_HELPER_BINARY_RSHIFT,
    JIT_HELPER_BINARY_AND,
    JIT_HELPER_BINARY_OR,
    JIT_HELPER_BINARY_XOR,
    JIT_HELPER_BINARY_MATMUL,
    JIT_HELPER_CONTAINS,

    JIT_HELPER_UNARY_NEGATIVE,
    JIT_HELPER_UNARY_STAR,
    JIT_HELPER_UNARY_INVERT,

    JIT_HELPER_LOAD_METHOD,
    JIT_HELPER_ITER_NEXT,
    JIT_HELPER_CALL_VARGS,

    JIT_HELPER_LIST_APPEND,
    JIT_HELPER_DICT_ADD,
    JIT_HELPER_SET_ADD,

    JIT_HELPER_IMPORT_PATH,
    JIT_HELPER_POP_IMPORT_STAR,
    JIT_HELPER_UNPACK_SEQUENCE,
    JIT_HELPER_UNPACK_EX,

    JIT_HELPER_BEGIN_CLASS,
    JIT_HELPER_END_CLASS,
    JIT_HELPER_STORE_CLASS_ATTR,
    JIT_HELPER_ADD_CLASS_ANNOTATION,

    JIT_HELPER_WITH_ENTER,
    JIT_HELPER_WITH_EXIT,

    JIT_HELPER_BEGIN_TRY,
    JIT_HELPER_END_TRY,
    JIT_HELPER_EXCEPTION_MATCH,
    JIT_HELPER_HANDLE_EXCEPTION,
    JIT_HELPER_RAISE,
    JIT_HELPER_RAISE_ASSERT,
    JIT_HELPER_RE_RAISE,
    JIT_HELPER_PUSH_EXCEPTION,

    JIT_HELPER_FORMAT_STRING,
    JIT_HELPER_POP_JUMP_IF_NOT_MATCH,

    JIT_HELPER__COUNT,
} JitHelperId;


#define JIT_CALL_ARGC(aux) ((uint32_t)(aux) & 0xFFu)
#define JIT_CALL_KWARGC(aux) (((uint32_t)(aux) >> 8) & 0xFFu)
#define JIT_CALL_FLAG_VARGS (1u << 16)

typedef enum JitLowerStatus {
    JIT_LOWER_OK = 0,
    JIT_LOWER_INVALID_ARGUMENT,
    JIT_LOWER_INVALID_BYTECODE,
    JIT_LOWER_STACK_MISMATCH,
    JIT_LOWER_UNSUPPORTED_GENERATOR,
    JIT_LOWER_LOOP_NOT_FOUND,
    JIT_LOWER_VERIFY_FAILED,
} JitLowerStatus;

typedef struct JitLowerError {
    JitLowerStatus status;
    int32_t bytecode_ip;
    char message[256];
} JitLowerError;

typedef struct JitFrontendOptions {
    /* Estimated execution count used for block frequency seeding. */
    uint32_t estimated_execution_count;

    /* Emit POLL before backward edges. */
    bool poll_loop_backedges;

    /*
     * Generator bytecodes require resumable native entries. The frontend can
     * describe ordinary YIELD_VALUE, but this is disabled by default until the
     * backend has a resume-entry table.
     */
    bool allow_generators;

    /* Verify the completed MIR before returning success. */
    bool verify;
} JitFrontendOptions;

typedef struct JitFrontendStats {
    uint32_t bytecode_count;
    uint32_t selected_bytecode_count;
    uint32_t basic_block_count;
    uint32_t selected_block_count;
    uint32_t phi_count;
    uint32_t helper_count;
    uint32_t exit_stub_count;
    uint32_t loop_count;
    uint32_t max_stack_depth;
} JitFrontendStats;

void jit_frontend_options_init(JitFrontendOptions* options);
const char* jit_lower_status_name(JitLowerStatus status);
const char* jit_helper_name(JitHelperId helper);

/* Lower all normally reachable bytecode in a CodeObject. */
bool jit_lower_code_object(const CodeObject* co,
                           const JitFrontendOptions* options,
                           JitRegion* out_region,
                           JitFrontendStats* out_stats,
                           JitLowerError* error);

/* Convenience wrapper for ordinary function declarations. */
bool jit_lower_funcdecl(const FuncDecl* decl,
                        const JitFrontendOptions* options,
                        JitRegion* out_region,
                        JitFrontendStats* out_stats,
                        JitLowerError* error);

/*
 * Lower the natural loop whose header begins at header_ip. The generated MIR
 * contains a synthetic entry block and explicit EXIT_INTERPRETER stubs for
 * edges leaving the loop region.
 */
bool jit_lower_loop(const CodeObject* co,
                    int32_t header_ip,
                    const JitFrontendOptions* options,
                    JitRegion* out_region,
                    JitFrontendStats* out_stats,
                    JitLowerError* error);

#ifdef __cplusplus
}
#endif
