#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${TMPDIR:-/tmp}/tinypy-jit-phase3-tests
CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=c11 -O2 -Wall -Wextra -Werror}

rm -rf "$BUILD"
mkdir -p "$BUILD"

INCLUDES="-I$ROOT/tests/stubs -I$ROOT/include"

$CC $CFLAGS $INCLUDES -c "$ROOT/tests/stubs/vector_stub.c" -o "$BUILD/vector_stub.o"
for unit in ir optimizer trace frontend lir backend regalloc; do
    $CC $CFLAGS $INCLUDES -c "$ROOT/src/jit/$unit.c" -o "$BUILD/$unit.o"
done

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/frontend_smoke.c" \
    "$BUILD/ir.o" "$BUILD/frontend.o" "$BUILD/optimizer.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/frontend_smoke"

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/frontend_edgecases.c" \
    "$BUILD/ir.o" "$BUILD/frontend.o" "$BUILD/lir.o" "$BUILD/backend.o" "$BUILD/regalloc.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/frontend_edgecases"

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/mir_smoke.c" \
    "$BUILD/ir.o" "$BUILD/optimizer.o" "$BUILD/lir.o" "$BUILD/backend.o" "$BUILD/regalloc.o" \
    "$BUILD/trace.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/mir_smoke"

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/backend_phi_smoke.c" \
    "$BUILD/ir.o" "$BUILD/lir.o" "$BUILD/backend.o" "$BUILD/regalloc.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/backend_phi_smoke"

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/regalloc_stress.c" \
    "$BUILD/ir.o" "$BUILD/lir.o" "$BUILD/regalloc.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/regalloc_stress"

$CC $CFLAGS $INCLUDES \
    "$ROOT/examples/parallel_copy_cycle.c" \
    "$BUILD/ir.o" "$BUILD/lir.o" "$BUILD/regalloc.o" "$BUILD/vector_stub.o" \
    -o "$BUILD/parallel_copy_cycle"
"$BUILD/parallel_copy_cycle"

"$BUILD/frontend_smoke"
"$BUILD/frontend_edgecases"
"$BUILD/mir_smoke"
"$BUILD/backend_phi_smoke"
"$BUILD/regalloc_stress"
"$BUILD/parallel_copy_cycle"
