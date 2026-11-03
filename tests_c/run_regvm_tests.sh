#!/usr/bin/env sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra -Werror}"
OUT="${TMPDIR:-/tmp}/pocketpy_regvm_tests"
mkdir -p "$OUT"

"$CC" $CFLAGS \
    -I"$ROOT/tests/stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/regbytecode.c" \
    "$ROOT/src/interpreter/regprofile.c" \
    "$ROOT/tests/vector_stub.c" \
    "$ROOT/tests/reglower_smoke.c" \
    -o "$OUT/reglower_smoke"
"$OUT/reglower_smoke"

"$CC" -std=c11 -fsyntax-only -Wall -Wextra -Werror \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/regvm.c"

"$CC" -std=c11 -fsyntax-only -Wall -Wextra -Werror \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/regvm_slow.c"

"$CC" -std=c11 -fsyntax-only -Wall -Wextra -Werror \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/regprofile.c"

"$CC" -std=c11 -fsyntax-only -Wall -Wextra -Werror \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/jitbridge.c"

"$CC" $CFLAGS -ffunction-sections -fdata-sections \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/include" \
    "$ROOT/tests/adaptive_smoke.c" \
    "$ROOT/src/interpreter/regbytecode.c" \
    "$ROOT/src/interpreter/regprofile.c" \
    "$ROOT/src/interpreter/shape.c" \
    "$ROOT/tests/vector_stub.c" \
    -Wl,--gc-sections \
    -o "$OUT/adaptive_smoke"
"$OUT/adaptive_smoke"

"$CC" $CFLAGS \
    -I"$ROOT/tests/namedict_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/objects/namedict.c" \
    "$ROOT/tests/namedict_smoke.c" \
    -o "$OUT/namedict_smoke"
"$OUT/namedict_smoke"

"$CC" $CFLAGS \
    -I"$ROOT/tests/frame_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/framestack.c" \
    "$ROOT/tests/framestack_smoke.c" \
    -o "$OUT/framestack_smoke"
"$OUT/framestack_smoke"

"$CC" $CFLAGS \
    -I"$ROOT/tests/shape_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/shape.c" \
    "$ROOT/tests/shape_smoke.c" \
    -o "$OUT/shape_smoke"
"$OUT/shape_smoke"

"$CC" $CFLAGS \
    -I"$ROOT/tests/shape_stubs" \
    -I"$ROOT/include" \
    "$ROOT/src/interpreter/shape.c" \
    "$ROOT/tests/shape_ic_smoke.c" \
    -o "$OUT/shape_ic_smoke"
"$OUT/shape_ic_smoke"


"$CC" $CFLAGS -ffunction-sections -fdata-sections \
    -I"$ROOT/tests/stubs" \
    -I"$ROOT/include" \
    "$ROOT/tests/profile_smoke.c" \
    "$ROOT/src/interpreter/regbytecode.c" \
    "$ROOT/src/interpreter/regprofile.c" \
    "$ROOT/tests/vector_stub.c" \
    -Wl,--gc-sections \
    -o "$OUT/profile_smoke"
"$OUT/profile_smoke"

"$CC" $CFLAGS -ffunction-sections -fdata-sections \
    -I"$ROOT/tests/regvm_stubs" \
    -I"$ROOT/tests/stubs" \
    -I"$ROOT/include" \
    "$ROOT/tests/jitbridge_smoke.c" \
    "$ROOT/src/interpreter/jitbridge.c" \
    "$ROOT/src/interpreter/regprofile.c" \
    "$ROOT/src/interpreter/regbytecode.c" \
    "$ROOT/tests/vector_stub.c" \
    -Wl,--gc-sections \
    -o "$OUT/jitbridge_smoke"
"$OUT/jitbridge_smoke"

"$CC" -std=c11 -O3 -Wall -Wextra -Werror \
    "$ROOT/tools/dispatch_probe.c" \
    -o "$OUT/dispatch_probe"
"$OUT/dispatch_probe"

python3 "$ROOT/tests/hotloop_layout_smoke.py"

echo "regvm phase 13-14 tests: ok"
