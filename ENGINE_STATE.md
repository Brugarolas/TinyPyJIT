# TinyPy lightweight tracing-region JIT

## Current state: native baseline JIT (`src/jit/native.c`)

This is the JIT that actually runs today. Build it with `--jit on` (requires `--interpreter regvm`):

```powershell
bun run build/tinypy.build.ts --release --compiler mingw --allocator mimalloc --interpreter regvm --jit on
```

Pipeline: `RegVM register bytecode -> integer LIR islands -> graph coloring -> SLJIT`.

The optional scalar-recursion entry (`src/jit/scalar.c`) performs forward type
dataflow on acyclic bytecode paths and emits direct native self calls with
unboxed integer locals. It accepts up to 8 integer arguments, 64 registers and
512 instructions. It checks the live global binding and excludes closures,
tracing and watchdog execution. Unsupported paths, overflow or exhausted
recursion/value-stack budgets fail without Python effects and restart through
the ordinary entry. Native recursion is additionally capped at 128 activations;
8 consecutive speculative failures disable that entry. It performs the original
recursive computation, with no memoization. `TINYPY_JIT_STATS=1` includes
`scalar_returns` to confirm that this entry actually ran.

- **When**: a function is compiled after `PK_JIT_FUNCTION_THRESHOLD` calls or when one of its loops takes
  `PK_JIT_LOOP_THRESHOLD` backedges (`regprofile.h`, both 10). Native entry happens at PC 0 or at a loop header.
- **State**: native code works in place on the frame's RegVM registers (`py_TValue*` in `S0`). Every result is
  written back immediately, so any guard failure resumes the interpreter at the failing bytecode with a precise
  state, and helper calls may run arbitrary Python code (and the GC) without exposing a half-updated frame.
- **Native opcodes**: int `+ - *` (overflow deopts), `// %` (Python floor semantics; zero divisor and
  `INT64_MIN` operands deopt), `& | ^ << >>` (shift counts outside `[0, 63]` deopt), all int comparisons
  and fused compare-branches, int/bool truthiness branches, `for ... in range(...)` (iterates the
  `RangeIterator` in place), constants, moves, `CHECK_BOUND`, returns and `not` on int/bool.
- **List accesses**: `LOAD_SUBSCR` / `STORE_SUBSCR` use exact-list and integer
  guards, negative-index normalization and bounds checks. Canonical builtin
  methods and `type_version` are checked; custom methods, slices and errors go
  through the shared RegVM helper and continue in native code. Swapping list
  items no longer materializes scalar-replaced tuples on each store.
- **Escaping constructors**: the existing field-only `__init__` analysis also
  builds a plan for real allocations. Both stack and register call paths can
  allocate the object and populate its final shape directly. Guards cover the
  class, type version, `__new__`, `__init__`, properties, attribute hooks, arity,
  tracing/watchdog and recursion/stack limits. Other constructors use the
  ordinary call protocol. The object remains a real, independent GC allocation.
- **Native calls**: `CALL` stays in native code. It calls the RegVM's own implementation (`reg_jit_call`, found
  through `RegVM__jit_step_fn()`), which runs the callee - natively if it is compiled - and returns; native
  code then continues with the next bytecode. Compiled simple Python callees use
  a shorter bridge with a C-owned Python frame, preserving GC roots, inspection,
  recursion checks and precise interpreter resumption. An exception raised by the callee leaves through a per-PC error
  exit (`REG_JIT_EXIT_ERROR`) and unwinds exactly like the interpreter's `REG_FAIL`, with the same traceback.
  After a helper call the per-entry state the callee may have changed (inline cache array, `type_version`) is
  refreshed.
- **Shared inline caches**: `LOAD_GLOBAL`, `LOAD_NONLOCAL` (closure-free functions), `LOAD_ATTR` (shape slot),
  `STORE_ATTR` (shape slot / transition) and `LOAD_METHOD` (bound method; shaped objects at the shape verified
  not to shadow it) have a native fast path that reads the RegVM's own `RegInlineCache` for the site. A failed
  guard does not leave native code: it takes the site's slow path, a helper call into the interpreter's
  implementation, which may refill the cache for the next execution. Sites without a cache at compile time
  get a reserved cache index before compilation, so their native path becomes
  usable as soon as the helper fills the cache, without recompilation.
- **Escape analysis + scalar replacement** (`src/jit/escape.c`, `include/tinypy/jit/escape.h`): a
  `BUILD_TUPLE`, `BUILD_LIST` or class instantiation whose object never escapes is not allocated at all.
  Instantiations qualify through the `CONSTRUCT` call IC when `__new__` is `object.__new__` and `__init__`
  only stores its arguments or scalar constants into fields of self. A forward may-alias dataflow over the
  whole CFG (union at merges, restarted at the allocation) finds the registers that may name the object at
  each PC; every read of one must be an accepted use - `UNPACK_SEQUENCE`, `LOAD_SUBSCR` with a constant
  index, `len(x)` (per the site's call IC), `LOAD_ATTR` / `STORE_ATTR` of a field, or `MOVE` - anything
  else is an escape. Uses may sit after branches, and up to 4 allocations may reach the same use.
  At run time each virtual object has an alias mask (one bit per register that may name it): the allocation
  sets it to the result register, `MOVE` copies the bit, other writes clear it - only where the dataflow
  says the bit may be set. Slots (tuple items, object fields) live in a per-activation save area registered as
  GC roots (`VM::jit_roots`); uses read or write them. A use whose register bit is clear runs the instruction
  on the real object through the interpreter's helper, without leaving native code. A side exit, or a frame
  introspection (`locals()`, one-argument `eval`/`exec`, via `RegJit__force_frame`), materializes each object
  into exactly the registers whose bits are set (objects by writing their fields in `__init__`'s store order,
  never by re-running it), so aliases that die on only some paths stay exact. Objects revalidate the class
  state when `type_version` moves; a `len` that is no longer the builtin materializes and calls it.
  `MOVE t <- p` feeding a use through a dead temporary is fused (no alias bookkeeping).
  `tests/jit/escape_semantics.py` and `tests/jit/escape_paths.py` cover deopts mid-lifetime, introspection,
  identity, field writes then materialization, class changes, `len` shadowing, path-dependent aliases,
  loop-carried values, GC of slots held only by the save area and genuine escapes.
- **Side exits**: every remaining opcode (dicts, floats, iteration over lists, ...) returns to the interpreter at that
  PC. The interpreter executes it and re-enters native code at the next hot loop header. Entries that return 32
  times in a row without completing a backedge, a helper call or a return are disabled for that PC.
- **Polling**: native loops return to the interpreter every 4096 backedges (`TINYJIT_LOOP_FUEL`).
- **Call fast path in the RegVM**: a `PY_SIMPLE` call cache hit pushes the callee frame and, when the callee is
  compiled, enters its native code directly (`reg_enter_native`), falling back to the interpreter at the
  resume PC (`reg_run(rc, start_pc)`) only if native code returns early. The interpreter and every helper
  share one implementation per opcode family (`reg_exec_*`, always inlined into the dispatch loop).
- **Code lifetime**: `RegCode::jit_code` points at the installed native code (O(1) lookup).
  `RegJit__release_code()` drops it before a `FuncDecl` dies, so native code of discarded functions (e.g. from
  `exec()`) is freed. Code that is still executing further up the C stack (`TinyJitCode::active`) is never
  recompiled away or evicted.
- **Shared profiling**: the RegVM collects per-op feedback only while a function is still cold (it stops once
  the function is compiled). The JIT uses it: int sites whose profile/quickening says "float" and caches that
  mostly miss stay interpreted instead of compiling guards that always fail. Entries the JIT gives up on are
  reported back (`RegJitExit::disable`) and the interpreter stops trying them.

## Object shapes

Instances of Python classes store their attributes in shape-backed storage (`PK_OBJ_SHAPED_SLOTS`,
`shape.h`): an immutable per-type transition tree maps names to slots, and the object holds a pointer to a
growable `ShapeStorage`. Types, modules and other namespaces keep `NameDict`. Every namespace accessor
(`py_getdict`/`py_setdict`/`py_deldict`/`py_applydict`/`py_cleardict`, `__dict__`, pickle, json, GC) handles
both. Inline caches guard the `Shape*`; class-level changes and `py_tphookattributes` bump `type_version`.
`tests/jit/shape_semantics.py` and `tests/jit/ic_semantics.py` pin down the Python-visible semantics;
`tests/jit/call_semantics.py` and `tests/jit/exec_globals.py` cover native calls and code release.

## Build optimization: LTO, PGO, dispatch

```powershell
bun run build/tinypy.build.ts --release --lto full ...              # LTO (recommended for releases)
bun run build/tinypy.build.ts --release --lto full --pgo-train ...  # instrumented build + training + PGO build
bun run build/tinypy.build.ts --release --pgo generate|use ...      # manual PGO phases
```

`--pgo-train` trains on the test suite, the benchmarks and `tools/pgo_workload.py`, and refuses to build if no
profile was recorded. Measured on GCC 16 / x86-64 (interpreter only): LTO is a consistent 3-8% win; PGO on top
is mixed (1-4% better on loops, 6-7% worse on call-heavy code), so it is opt-in. Threaded dispatch
(`PK_REGVM_DISPATCH_THREADED=1`) shares the single handler body but measured slower than the switch and is off
(see `perf.h`).

Diagnostics:

```text
TINYPY_JIT_STATS=1   print compile/entry/exit counters at VM shutdown
TINYPY_JIT_TRACE=1   explain every compile decision and dump the compiled register bytecode
TINYPY_REGVM_DUMP=1  dump every function's register bytecode when it is lowered
```

Tests: `tests/jit/*.py` (run with the JIT build) and the RegVM -> MIR frontend test:

```powershell
bun run build/tinypy.build.ts --release --compiler mingw --allocator mimalloc --interpreter regvm --jit on --mir-test
.\build-ts-mimalloc-jit\release-mingw-x64\bin\tinypy-mir-test.exe
```

Not done yet (in priority order):

1. Float arithmetic in islands (needs FPR allocation; the allocator already models FPR banks).
2. General inlining or a native calling convention for small object methods.
   Scalar self-recursion and field-only constructors now bypass most call setup,
   but ordinary calls still initialize a rooted register window and Python frame.
3. Native fast paths for the opcodes that now run through RegVM helpers (`DICT_ADD`, `CONTAINS`,
   iteration over lists/dicts, generic binary operators); they no longer side-exit, but still pay a call.
4. Partial escape analysis: today an object that escapes on any path stays a real allocation everywhere;
   materializing it only at the escaping instruction would keep the other paths allocation-free. Also
   item stores on virtual lists (`xs[0] = v`) and field additions/deletions on virtual objects.
5. Wiring the MIR path (`reg_frontend.c` -> optimizer -> `backend.c` -> `regalloc.c`) to SLJIT emission. The
   RegVM -> MIR frontend (`include/tinypy/jit/reg_frontend.h`) is built and tested but not used for code
   generation: `JitState.locals` represents all RegVM registers and `bytecode_ip` values are RegVM PCs.
6. Module-level statements outside loops still run in the stack interpreter (loops are outlined, see below).
7. Copy propagation for the register bytecode (call windows are filled through MOVE chains).

## Optimization round 2 (2026-11-20): every benchmark faster than CPython

Changes, grouped by layer:

- **Module-level loops run in the RegVM/JIT.** The compiler outlines every `for`/`while` at module
  level (EXEC mode, not dynamic, not in a class body) into a synthetic function whose names all stay
  global (`LOAD_GLOBAL`/`STORE_GLOBAL`); functions and classes defined inside are module-level ones and
  `return`/`yield` remain syntax errors. `CodeObject.is_outline` (serialized as bit 1 of a FuncDecl's
  `nested` byte, so older `.pyc` files still load) makes the runtime present the frame as the module:
  tracebacks show it once (`BaseException.skip_caller_frame`), and `locals()`/`exec`/`eval` behave as at
  module level. Covered by `tests/jit/module_loop_outline.py`.
- **Globals.** `NameDict::version` only changes on insertion/deletion/rehash, not when an existing name is
  rebound, so a global store no longer invalidates every `LOAD_GLOBAL` cache. `STORE_GLOBAL` has an inline
  cache in the RegVM and a native fast path in the JIT.
- **Calls.** Calls that run to completion in C (call-cache `PY_SIMPLE` hits, constructor `__init__`,
  `py_call`/`VM__vectorcall` from C) use a C-stack `py_Frame` (`Frame__init_native`) instead of the
  frame-stack allocator. The JIT inlines callees whose body only returns a constant (guarded by the exact
  `FuncDecl`, retained by the compiled code). Native types without `__init__` (`str(x)`, `int(x)`, ...) get
  a call cache (`REG_CALL_CACHE_NATIVE_TYPE`), `module.func(...)` gets a method cache
  (`REG_METHOD_CACHE_MODULE_ATTR`), and a call site whose cache fill fails stops retrying.
- **JIT coverage.** Generic arithmetic/comparisons, `IS`, `CONTAINS`, `GET_ITER`, `BUILD_DICT`, `DICT_ADD`,
  `LIST_APPEND`, `NEG`, `BUILD_STRING`, `FORMAT_STRING`, `DELETE_SUBSCR` and iteration over
  lists/tuples/dicts/strings/generators call RegVM helpers instead of side-exiting. `for k, v in ...`
  fuses the iterator step with the 2-way unpack; `dict.items()` then yields pairs without tuples.
  `raise`/`assert` lower to `RBC_RAISE`, so they no longer make a whole function interpreter-only.
  Compare-and-branch superinstructions on non-int operands (`x == None`, objects, floats, strings) and
  truthiness branches on values other than int/bool (`if node.next:`; `None` is inline) call the
  interpreter's generic operation instead of deoptimizing (lru.py: 819k resumes -> 48). Covered by
  `tests/jit/generic_branches.py`.
- **Code generation.** Floor division/modulo by a small constant is strength-reduced (shift/mask for powers
  of two, multiply-high by a magic number otherwise; INT64_MIN still deoptimizes). Scalar `py_TValue`
  headers are written with one 8-byte store (JIT and the inline `py_newint`/`py_newbool`/... macros), which
  removes store-forwarding stalls when values are copied right after being produced. GCC/MinGW x86 builds
  compile the interpreter library with `-mstringop-strategy=unrolled_loop`: generic tuning emitted
  `rep movs` for many 24-byte `py_TValue` copies (mimalloc keeps libc memcpy for its large copies).
- **Hot API calls are inline** for the interpreter: `py_istype`/`py_isint`/..., scalar constructors,
  value-stack ops and `py_retval()` (the exported functions remain for embedders).
- **Dicts.** Power-of-two index tables with raw hashes (an int hashes to itself, as in CPython) and a
  fold of the high bits into the slot: consecutive int keys fill consecutive slots and entries. Deletion
  leaves a tombstone instead of shifting probe clusters (O(1) `del`); rebuilds drop them. Integer keys
  compare without dispatch. Covered by `tests/083_dict_stress.py`.
- **Memory.** GC pacing scales with the tracing work of the last collection (minor collections trace all
  old objects, so a fixed nursery made big live heaps quadratic). Shaped objects carry their first 4-slot
  `ShapeStorage` inline (one allocation per instance). The mimalloc bootstrap keeps freed memory committed
  (`purge_delay = -1`, overridable with `MIMALLOC_PURGE_DELAY`): growing containers otherwise page-fault on
  every fresh region.
- **Library.** `json.loads` parses JSON directly in C (falling back to the old evaluation path for anything
  it does not handle identically); `pickle` fills lists in bulk; integer formatting no longer uses `snprintf`.
- **Fixes found on the way.** `FuncDecl__deserialize` left `regcode`/`regcode_state` uninitialized (a
  debug-build crash in `922_py_compile.py`). The CLI printed its usage line when a script got
  arguments; everything after the script name now belongs to the script (`sys.argv`).
- **Diagnostics.** `TINYPY_JIT_PERFMAP=<file>` writes the native address of every bytecode block, so an
  external sampling profiler can attribute JIT samples to bytecode. `TINYPY_EXTRA_CFLAGS` (all targets) and
  `TINYPY_LIB_CFLAGS` (interpreter library only) add compiler flags for local experiments.

### Measurements

Windows 11 x64, MinGW GCC 16.2.0, CPython 3.15.2; release + mimalloc + RegVM + JIT, no LTO/PGO.
Whole-process wall time of the unmodified `benchmarks/*.py` (fastest of five fresh runs; process start and
JIT warmup included). "Start of round" is the build before these changes (one run each).

| Benchmark | Start of round (s) | Now (s) | CPython (s) | Now / CPython |
| --- | ---: | ---: | ---: | ---: |
| dict_0 | 0.238 | 0.137 | 0.207 | 0.66 |
| dict_1 | 0.410 | 0.243 | 0.305 | 0.80 |
| dumps_loads_json | 1.821 | 0.459 | 0.617 | 0.74 |
| dumps_loads_pkl | 0.215 | 0.165 | 0.219 | 0.76 |
| fib | 0.082 | 0.081 | 0.878 | 0.09 |
| function_0 | 1.858 | 0.159 | 0.684 | 0.23 |
| function_1 | 2.643 | 0.201 | 0.751 | 0.27 |
| ldtk_json | 0.305 | 0.061 | 0.134 | 0.45 |
| loop_0 | 0.097 | 0.028 | 0.125 | 0.23 |
| loop_0_if | 0.201 | 0.039 | 0.205 | 0.19 |
| loop_1 | 0.186 | 0.050 | 0.235 | 0.21 |
| loop_2 | 0.209 | 0.053 | 0.284 | 0.19 |
| loop_3 | 0.720 | 0.140 | 1.106 | 0.13 |
| loop_4 | 0.570 | 0.140 | 0.159 | 0.88 |
| primes | 1.859 | 1.099 | 1.395 | 0.79 |
| recursive | 0.013 | 0.011 | 0.021 | 0.51 |
| simple | 0.046 | 0.025 | 0.101 | 0.25 |
| sort | 0.041 | 0.034 | 0.098 | 0.34 |
| sum | 0.012 | 0.010 | 0.020 | 0.48 |
| vec | 2.112 | 0.977 | 1.369 | 0.71 |

`loop_4` has the thinnest margin and varies most between runs (it is dominated by page faults while a
2M-entry dict grows: entries are 56 bytes against CPython's 24). These numbers do not establish
performance on other machines or workloads.

`benchmarks/official/` (Benchmarks Game programs) mostly cannot run on TinyPy yet; the blockers are
language/library features, not speed: `helloworld` (0.010 s vs 0.022 s) and `lru` (`100 500000`:
0.239 s vs 0.237 s; `1000 1000000`: 0.467 s vs 0.511 s) run. The others need arbitrary-precision ints
(`edigits`, `pidigits`), non-literal default arguments (`binarytrees`, `nbody`, `fasta`), generator
expressions (`spectral-norm`), `async`/`await` and `asyncio` (`coro-prime-sieve`), slice-object item
assignment (`nsieve`), `str(bool)` via `__str__` (`merkletrees`), an f-string parser that accepts `:` in
subscripts (`edigits`), and `itertools`/`re`/`multiprocessing`/`stdout.buffer` (`fasta`, `knucleotide`).

Validation: all 110 scripts in `tests/` and `tests/jit/` pass in the release JIT, debug JIT, release
interpreter-only (`--jit off`) and stack-interpreter builds, and the RegVM-to-MIR C test passes. New tests:
`tests/083_dict_stress.py`, `tests/734_json_parser.py`, `tests/jit/const_divmod.py`,
`tests/jit/generic_branches.py`, `tests/jit/iter_unpack_fusion.py` and `tests/jit/module_loop_outline.py`. Run test paths relative to the
repo root; `922_py_compile.py` also needs the tested binary accessible as `main.exe` on PATH.
---

## Phase 3 design notes: MIR -> LIR backend

Phase 3 adds the real MIR -> LIR backend and replaces the provisional linear-scan path with an optimistic
**graph-coloring register allocator**. The package now takes the TinyPy bytecode lowered by phase 2 all the
way to physically allocated, verified LIR. The only deliberately missing backend layer is final SLJIT emission.

The complete compiler path is now:

```text
TinyPy bytecode
    -> CFG + stack-depth analysis
    -> function / natural-loop region selection
    -> stack SSA + Phi construction
    -> Pocket MIR
    -> MIR optimizer
    -> representation-aware MIR -> LIR lowering
    -> Phi elimination / critical-edge splitting
    -> parallel-copy resolution
    -> LIR liveness
    -> interference graph
    -> conservative move coalescing
    -> optimistic graph coloring
    -> spill rewrite + re-color
    -> physical register / stack rewrite
    -> final LIR peepholes
    -> [next phase: SLJIT emission]
```

## New backend files

- `include/tinypy/jit/backend.h`
- `src/jit/backend.c`
  - representation-aware MIR -> LIR lowering;
  - token/multi-result lowering;
  - MIR resume-state translation;
  - native operation selection;
  - semantic runtime-call LIR for dynamic Python operations;
  - Phi edge-copy creation;
  - critical-edge splitting;
  - parallel-copy resolution.

- `include/tinypy/jit/regalloc.h`
- `src/jit/regalloc.c`
  - block liveness;
  - live-through-call detection;
  - weighted spill costs from block frequencies;
  - interference graph construction;
  - GPR/FPR register classes;
  - scratch/saved register banks;
  - Briggs-style conservative coalescing;
  - optimistic simplify/select coloring;
  - spill selection;
  - spill-slot assignment;
  - spill reload/store insertion;
  - iterative re-coloring;
  - final VREG -> physical-register/stack rewrite.

- `include/tinypy/jit/lir.h`
- `src/jit/lir.c`
  - replaced by a richer virtual-register LIR;
  - variadic defs/uses for runtime multi-result operations;
  - generic TValue homes;
  - stack slots;
  - physical register operands;
  - VM resume states;
  - edge parallel-copy bundles;
  - critical-edge handling;
  - verifier, stack layout and dumper.

## Register model

The LIR deliberately has three virtual-register classes:

```text
GPR    -> I64, bool, pointers, type/name IDs
FPR    -> F64
VALUE  -> full py_TValue values
```

`VALUE` virtual registers do **not** enter the native interference graph. A `py_TValue` is an aggregate and is
therefore assigned a stack home. This keeps generic Python values precise and GC/runtime-friendly without
pretending that they fit in one machine register.

Proven native values remain unboxed and participate in coloring normally:

```text
JIT_TYPE_I64   -> GPR
JIT_TYPE_BOOL  -> GPR
JIT_TYPE_F64   -> FPR
pointer types  -> GPR
JIT_TYPE_VALUE -> TValue stack home
```

This is the main representation boundary between MIR and machine-oriented LIR.

## Scratch and saved registers

The allocator models SLJIT's two useful physical-register families directly:

```text
GPR scratch
GPR saved
FPR scratch
FPR saved
```

A value that is live across an instruction carrying `JIT_LIR_FLAG_CALL` is restricted to the corresponding
**saved** register bank. Short-lived values may use either scratch or saved registers.

The allocator configuration also allows the emitter/runtime to reserve the first N registers in each bank:

```c
JitGraphColorConfig config;
jit_graph_color_config_init(&config);

config.gpr_scratch_count = 8;
config.gpr_saved_count = 4;
config.fpr_scratch_count = 8;
config.fpr_saved_count = 4;

config.reserved_gpr_saved = 2; /* e.g. VM* and py_Frame* */
```

The future SLJIT emitter can map these abstract banks directly to `SLJIT_R*`, `SLJIT_S*`, `SLJIT_FR*` and
`SLJIT_FS*`.

## Liveness and resume states

Liveness is CFG-based rather than interval-based. The allocator computes block `use`, `def`, `live-in` and
`live-out` sets to a fixed point.

Resume states attached to potentially raising MIR/LIR operations are also treated as liveness uses. This means
a value required to reconstruct TinyPy state cannot be accidentally overwritten before an exception/runtime
boundary.

State values used only to materialize the VM immediately before a call are not unnecessarily classified as
live *through* that call.

## Interference graph

The allocator builds an undirected interference graph for allocatable GPR/FPR virtual registers.

It handles:

- ordinary defs versus live values;
- multi-result runtime operations;
- values live across CFG edges;
- loop-carried values;
- calls and saved-register restrictions;
- copy-specific interference suppression so moves can coalesce.

GPR and FPR values never interfere with one another because they use independent physical banks.

## Conservative move coalescing

`COPY` instructions generated by Phi elimination are candidates for coalescing.

The allocator merges two non-interfering nodes when:

1. they belong to the same register class;
2. they share at least one legal physical color;
3. the union's conservative degree remains below the number of common colors.

The merged node receives the intersection of both legal-color masks, so a value that must survive a call cannot
be incorrectly coalesced onto a scratch-only register.

After physical rewrite, copies whose source and destination received the same physical location are removed by
the LIR peephole pass.

## Phi elimination

MIR remains SSA-ish. LIR is not required to remain SSA.

Each MIR Phi becomes parallel copies on incoming CFG edges:

```text
B1 -> B3:  phi_result <- value_from_B1
B2 -> B3:  phi_result <- value_from_B2
```

Critical edges are split automatically when neither predecessor nor successor is a safe insertion point.

Parallel-copy cycles are resolved with exactly one temporary when required. For example:

```text
a <- b
b <- a
```

becomes conceptually:

```text
tmp <- a
a   <- b
b   <- tmp
```

The dedicated `parallel_copy_cycle` test exercises this path.

## Multi-result TinyPy operations

Phase 2 represents operations such as `LOAD_METHOD`, `FOR_ITER` and unpacking using MIR `TOKEN` values plus
`EXTRACT_TOKEN`.

Phase 3 removes the token abstraction before register allocation. The producing LIR runtime instruction simply
has multiple explicit definitions:

```text
LOAD_METHOD runtime op
    defs: callable, self

ITER_NEXT runtime op
    defs: has_value, item
```

`EXTRACT_TOKEN` itself emits no LIR instruction.

This avoids allocating an artificial token object or register.

## Runtime operations

Operations that are already proven native become machine-oriented LIR immediately:

```text
ADD_I64
SUB_I64
MUL_I64
CMP_I64
ADD_F64
MUL_F64
...
```

Dynamic Python semantics become `JIT_LIR_RUNTIME` operations carrying:

- the semantic MIR opcode or frontend-helper ID;
- original immediates/auxiliary fields;
- explicit defs and uses;
- call/raise/barrier flags;
- optional resume state.

The next SLJIT emitter can choose per runtime operation whether to emit:

```text
inline checked fast path
runtime helper call
or a combination of both
```

without changing MIR or the allocator.

## Spill rewriting

Coloring is optimistic. If a node cannot be colored, the allocator assigns it a native stack slot and rewrites
its uses/defs:

```text
... use v7 ...
```

becomes:

```text
t0 = SPILL_LOAD slot(v7)
... use t0 ...
```

and a definition:

```text
v7 = ...
```

becomes:

```text
t1 = ...
SPILL_STORE slot(v7), t1
```

The new temporaries are then recolored from scratch. This repeats until there are no spills or the configured
spill-round limit is reached.

Coalesced virtual registers that spill share the same spill slot.

## Backend API

```c
JitBackendOptions backend_options;
jit_backend_options_init(&backend_options);

JitLirFunction lir;
JitBackendStats backend_stats;
JitBackendError backend_error;

if(!jit_lower_mir_to_lir(&region,
                         &backend_options,
                         &lir,
                         &backend_stats,
                         &backend_error)) {
    /* Fall back to interpretation. */
}
```

Then allocate registers:

```c
JitGraphColorConfig config;
jit_graph_color_config_init(&config);

JitRegAllocResult allocation;
jit_regalloc_result_init(&allocation);

char error[256];
if(!jit_graph_color_allocate(&lir,
                             &config,
                             &allocation,
                             error,
                             sizeof(error))) {
    /* Fall back to interpretation. */
}

if(!jit_regalloc_apply(&lir, &allocation, error, sizeof(error))) {
    /* Fall back to interpretation. */
}
```

After `jit_regalloc_apply()`, all LIR instruction/state operands have been rewritten from virtual registers to:

```text
physical GPR/FPR registers
or
native stack slots
```

and `lir.local_stack_size` contains the final aligned native-frame local area required by the future SLJIT
function prologue.

## Tests

`tests/run_frontend_tests.sh` builds all compiler translation units under strict C11 with:

```text
-std=c11 -O2 -Wall -Wextra -Werror
```

The suite now runs:

- `frontend_smoke.c`
  - whole-function and natural-loop bytecode -> MIR lowering;
- `frontend_edgecases.c`
  - edge-dependent stack heights;
  - `FOR_ITER`;
  - `LOAD_METHOD` + `CALL`;
  - full MIR -> LIR -> graph-color -> physical rewrite for those cases;
- `mir_smoke.c`
  - optimized typed MIR -> LIR -> physical allocation;
- `backend_phi_smoke.c`
  - Phi lowering;
  - critical-edge splitting;
  - move coalescing;
- `regalloc_stress.c`
  - values live across a call;
  - deliberate register pressure;
  - spill rewrite and re-coloring;
- `parallel_copy_cycle.c`
  - cyclic Phi/parallel-copy resolution.

The same suite has also been run successfully with AddressSanitizer + UndefinedBehaviorSanitizer:

```text
-fsanitize=address,undefined -fno-omit-frame-pointer
```

The stress test deliberately provides only two saved GPRs for twelve values live across a call. The current
allocator produces 10 spills in one rewrite round, inserts 10 reloads and 10 stores, recolors the new temporaries,
and finishes with verified physical LIR.

## Existing frontend/compiler core

Phase 3 retains the Phase 2 frontend and MIR optimizer:

- `include/tinypy/jit/frontend.h` / `src/jit/frontend.c`
- `include/tinypy/jit/ir.h` / `src/jit/ir.c`
- `include/tinypy/jit/optimizer.h` / `src/jit/optimizer.c`
- `include/tinypy/jit/trace.h` / `src/jit/trace.c`

Function hotness and loop-backedge hotness remain independent from backend correctness. A threshold around 10
executions can therefore stay aggressive without making the compiled code speculative.

## Recommended next phase

Phase 4 can now be almost entirely mechanical backend work:

1. add SLJIT as the machine-code emitter;
2. map abstract physical banks to SLJIT registers;
3. generate the native prologue/epilogue and local stack area;
4. emit integer/F64 LIR instructions directly;
5. define the runtime-helper ABI for `JIT_LIR_RUNTIME` and `JIT_LIR_CALL`;
6. implement TValue boxing/unboxing and stack-home addressing;
7. materialize `JitLirState` at error/interpreter-exit boundaries;
8. install function and loop entry points into the hotness/runtime layer;
9. add executable end-to-end benchmarks.

At this point SLJIT no longer needs to understand TinyPy bytecode, SSA, Phis, liveness, register pressure, or
spilling. It only has to emit already-selected, already-allocated LIR.
