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
 * The production default stays on the dense switch dispatch. GCC/Clang lower
 * it to a jump table, while MSVC is PocketPy's supported Windows compiler and
 * has no labels-as-values extension. A computed-goto experiment therefore does
 * not justify a second full handler body in the maintained runtime.
 */
#define PK_REGVM_DISPATCH_SWITCH 1
