#pragma once

/*
 * Interpreter-specific compiler hints. PGO remains the primary source of
 * branch/layout information; these attributes only express invariants that are
 * stable across workloads.
 */
#if defined(__GNUC__) || defined(__clang__)
    #define PK_REGVM_HOT __attribute__((hot))
    #define PK_REGVM_COLD __attribute__((cold, noinline))
    #define PK_REGVM_NOINLINE __attribute__((noinline))
    #define PK_REGVM_ALIGN64 __attribute__((aligned(64)))
    #define PK_REGVM_LIKELY(x) __builtin_expect(!!(x), 1)
    #define PK_REGVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #if defined(__clang__)
        #define PK_REGVM_ASSUME(x) __builtin_assume(x)
    #else
        #define PK_REGVM_ASSUME(x) do { if(!(x)) __builtin_unreachable(); } while(0)
    #endif
#elif defined(_MSC_VER)
    #define PK_REGVM_HOT
    #define PK_REGVM_COLD __declspec(noinline)
    #define PK_REGVM_NOINLINE __declspec(noinline)
    #define PK_REGVM_ALIGN64
    #define PK_REGVM_LIKELY(x) (!!(x))
    #define PK_REGVM_UNLIKELY(x) (!!(x))
    #define PK_REGVM_ASSUME(x) __assume(x)
#else
    #define PK_REGVM_HOT
    #define PK_REGVM_COLD
    #define PK_REGVM_NOINLINE
    #define PK_REGVM_ALIGN64
    #define PK_REGVM_LIKELY(x) (!!(x))
    #define PK_REGVM_UNLIKELY(x) (!!(x))
    #define PK_REGVM_ASSUME(x) ((void)0)
#endif

/*
 * RegVM dispatch. There is a single handler body: every handler is a `case`
 * that also carries a label (REG_CASE) and ends with REG_DISPATCH_NEXT().
 *
 *  - Switch (default): REG_DISPATCH_NEXT() is a plain `break` back to the
 *    central switch, which GCC/Clang/MSVC lower to one jump table.
 *  - Threaded (-DPK_REGVM_DISPATCH_THREADED=1, GCC/Clang only):
 *    REG_DISPATCH_NEXT() decodes the next instruction and jumps through a label
 *    table, replicating the indirect branch per handler. Opcodes missing from
 *    the table fall back to the switch.
 *
 * Measured on GCC 16 / x86-64 (Windows, mimalloc, release, best of 3) the
 * threaded variant was 5-19% slower than the switch (e.g. collatz 0.225s vs
 * 0.265s; 0.234s with no-gcse), and 1.4-6x slower with LTO+PGO (attr_read
 * 0.053s vs 0.241s): the replicated decode bloats the giant handler function
 * and defeats GCC's profile-driven block layout. Re-measure before enabling it
 * with another compiler or CPU.
 */
#ifndef PK_REGVM_DISPATCH_THREADED
    #define PK_REGVM_DISPATCH_THREADED 0
#endif
#if PK_REGVM_DISPATCH_THREADED && !(defined(__GNUC__) || defined(__clang__))
    #error "PK_REGVM_DISPATCH_THREADED requires labels-as-values (GCC or Clang)"
#endif
