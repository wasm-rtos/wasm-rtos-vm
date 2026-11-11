#!/usr/bin/env bash
# Everything that must pass after a change: the spec suite (also with 32-bit
# sizes and terse errors), the fuel test and the wasm-rtos tests on the engine.
set -uo pipefail
cd "$(dirname "$0")/.."
SPEC="${SPEC:-$HOME/wasm-rtos/third-party/spec-2.0/json}"
mkdir -p build
fail=0
cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec || exit 1
echo "spec:          $(python3 tools/spec.py build/wm-spec "$SPEC" | tail -1)" ; python3 tools/spec.py build/wm-spec "$SPEC" >/dev/null || fail=1
cc -std=gnu99 -O1 -g -DWM_SMALL_SIZES -DWM_TERSE_ERRORS=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-small || exit 1
python3 tools/spec.py build/wm-spec-small "$SPEC" >/dev/null && echo "spec (small):  ok" || { echo "spec (small):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_COMPUTED_GOTO=0 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-switch || exit 1
python3 tools/spec.py build/wm-spec-switch "$SPEC" >/dev/null && echo "spec (switch): ok" || { echo "spec (switch): FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_PREDECODE=0 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-inplace || exit 1
python3 tools/spec.py build/wm-spec-inplace "$SPEC" >/dev/null && echo "spec (in place): ok" || { echo "spec (in place): FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_PREDECODE=0 -DWM_FUSE=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-fuse || exit 1
python3 tools/spec.py build/wm-spec-fuse "$SPEC" >/dev/null && echo "spec (fused):  ok" || { echo "spec (fused):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_NOP_RUNS=1 -DWM_FUSE=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-nop || exit 1
python3 tools/spec.py build/wm-spec-nop "$SPEC" >/dev/null && echo "spec (as on chips): ok" || { echo "spec (as on chips): FAIL"; fail=1; }
cc -std=gnu99 -O1 -DWM_NOP_RUNS=1 -DWM_FUSE=1 -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
build/fuel >/dev/null && echo "fuel (as on chips): ok" || { echo "fuel (as on chips): FAIL"; fail=1; }
for fuse in 0 1; do
    cc -std=gnu99 -O1 -g -DWM_FUSE=$fuse -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
    build/fuel >/dev/null && echo "fuel (fuse $fuse): ok" || { echo "fuel (fuse $fuse): FAIL"; fail=1; }
done
cc -std=gnu99 -O1 -g -DWM_MEMORY_ALWAYS_PAGED=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-paged || exit 1
python3 tools/spec.py build/wm-spec-paged "$SPEC" >/dev/null && echo "spec (paged):  ok" || { echo "spec (paged):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -DWM_MEMORY_ALWAYS_PAGED=1 -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
build/fuel >/dev/null && echo "fuel (paged):  ok" || { echo "fuel (paged):  FAIL"; fail=1; }
for mode in "" "-DWM_PREDECODE=0"; do
    cc -std=gnu99 -O1 -g $mode -fsanitize=address,undefined source/*.c test/paged.c -lm -o build/paged || exit 1
    build/paged >/dev/null && echo "paged memory:  ok ${mode:-(predecoded)}" || { echo "paged memory:  FAIL $mode"; fail=1; }
done
cc -std=gnu99 -O1 -g -DWM_SPEC_PAGER=3 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-swap || exit 1
python3 tools/spec.py build/wm-spec-swap "$SPEC" >/dev/null 2>&1 && echo "spec (swapped, 3 frames of RAM): ok" || { echo "spec (swapped): FAIL"; fail=1; }
for mode in "" "-DWM_PREDECODE=0"; do
    cc -std=gnu99 -O1 -g $mode -fsanitize=address,undefined source/*.c test/swap.c -lm -o build/swap || exit 1
    build/swap >/dev/null && echo "swapping:      ok ${mode:-(predecoded)}" || { echo "swapping:      FAIL $mode"; fail=1; }
done
test/snapshot.sh >/dev/null && echo "snapshots:     ok (between builds)" || { echo "snapshots:     FAIL"; fail=1; }
echo "kernel tests:  $(tools/kernel-tests.sh | tail -1)"
echo "kernel tests (paged): $(CFLAGS=-DWM_MEMORY_ALWAYS_PAGED=1 tools/kernel-tests.sh | tail -1)"
exit $fail
