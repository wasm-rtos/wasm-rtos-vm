#!/usr/bin/env bash
# Everything that must pass after a change: the static analyzer, the spec
# suite (also with 32-bit sizes and terse errors), the fuel test and the
# wasm-rtos tests on the engine.
set -uo pipefail
cd "$(dirname "$0")/.."
SPEC="${SPEC:-$HOME/wasm-rtos/third-party/spec-2.0/json}"
mkdir -p build
fail=0
cc -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -DWM_CHECKS=1 source/*.c tools/wm-spec.c -lm -o build/wm-spec || exit 1
echo "spec:          $(python3 tools/spec.py build/wm-spec "$SPEC" | tail -1)" ; python3 tools/spec.py build/wm-spec "$SPEC" >/dev/null || fail=1
out="$(tools/analyze.sh)" && echo "analyzer:      ok" || { echo "analyzer:      FAIL"; echo "$out"; fail=1; }
# A plain optimized build must predecode, or every program runs in place.
cc -std=gnu99 -O2 source/*.c test/predecoded.c -lm -o build/predecoded || exit 1
build/predecoded >/dev/null && echo "predecoded:    ok (-O2)" || { echo "predecoded:    FAIL $(build/predecoded)"; fail=1; }
# UBSan makes the interpreter too large to predecode, so the runs above go
# through the code in place; these, with ASan only, run the predecoded loop.
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 source/*.c tools/wm-spec.c -lm -o build/wm-spec-fast || exit 1
python3 tools/spec.py build/wm-spec-fast "$SPEC" >/dev/null && echo "spec (predecoded): ok" || { echo "spec (predecoded): FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 -DWM_SPEC_PAGER=3 source/*.c tools/wm-spec.c -lm -o build/wm-spec-fast-swap || exit 1
python3 tools/spec.py build/wm-spec-fast-swap "$SPEC" >/dev/null 2>&1 && echo "spec (predecoded, swapped): ok" || { echo "spec (predecoded, swapped): FAIL"; fail=1; }
# A code cache of one byte: records only for the functions on the stack, every other call makes them again.
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_SPEC_CODE_LIMIT=1 source/*.c tools/wm-spec.c -lm -o build/wm-spec-code || exit 1
python3 tools/spec.py build/wm-spec-code "$SPEC" >/dev/null && echo "spec (code cache of 1 byte): ok" || { echo "spec (code cache of 1 byte): FAIL"; fail=1; }
# The engine's heap (decision 20).
cc -std=gnu99 -O1 -g -Wall -Wextra -fsanitize=address,undefined source/*.c test/heap.c -lm -o build/heap || exit 1
build/heap >/dev/null && echo "heap:          ok" || { echo "heap:          FAIL"; build/heap | grep FAIL; fail=1; }
# The code area (decision 18): its own test, and the spec suite with every unpinned span moved at each
# allocation (old places filled with a pattern), with a cache of one byte, in the fast loop and paged.
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -DWM_CHECKS=1 source/*.c test/area.c -lm -o build/area || exit 1
build/area >/dev/null && echo "code area:     ok" || { echo "code area:     FAIL"; build/area | grep FAIL; fail=1; }
for mode in "" "-DWM_SPEC_CODE_LIMIT=1" "-DWM_SPEC_CODE_LIMIT=1 -DWM_MEMORY_ALWAYS_PAGED=1"; do
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 -DWM_CODE_MOVE_ALWAYS=1 $mode source/*.c tools/wm-spec.c -lm -o build/wm-spec-move || exit 1
    python3 tools/spec.py build/wm-spec-move "$SPEC" >/dev/null && echo "spec (code always moved): ok $mode" || { echo "spec (code always moved): FAIL $mode"; fail=1; }
done
cc -std=gnu99 -O1 -g -fsanitize=address source/*.c test/code.c -lm -o build/code || exit 1
build/code >/dev/null && echo "code cache:    ok" || { echo "code cache:    FAIL"; build/code | grep FAIL; fail=1; }
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 source/*.c test/inplace.c -lm -o build/inplace || exit 1
build/inplace >/dev/null && echo "in place:      ok" || { echo "in place:      FAIL"; build/inplace | grep FAIL; fail=1; }
# Functions in place by themselves (decision 21): every one, and mixed with records at random,
# also paged and with code always moved. ASan only: under UBSan the interpreter does not predecode.
for mode in 1 2 "2 -DWM_MEMORY_ALWAYS_PAGED=1 -DWM_CODE_MOVE_ALWAYS=1"; do
    # shellcheck disable=SC2086
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 -DWM_SPEC_IN_PLACE=$mode source/*.c tools/wm-spec.c -lm -o build/wm-spec-alone || exit 1
    python3 tools/spec.py build/wm-spec-alone "$SPEC" >/dev/null && echo "spec (in place $mode): ok" || { echo "spec (in place $mode): FAIL"; fail=1; }
done
# Code images (decision 15): the spec suite from images only, and copied in through a cache of one byte.
for mode in 1 2; do
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 -DWM_SPEC_IMAGE=$mode source/*.c tools/wm-spec.c -lm -o build/wm-spec-image || exit 1
    python3 tools/spec.py build/wm-spec-image "$SPEC" >/dev/null && echo "spec (code image $mode): ok" || { echo "spec (code image $mode): FAIL"; fail=1; }
done
for mode in "" -DWM_HAS_VALIDATION=0 "-DWM_FUSE=1" -DWM_PREDECODE=0; do
    # shellcheck disable=SC2086
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 $mode source/*.c test/image.c -lm -o build/image || exit 1
    build/image >/dev/null && echo "code images:   ok $mode" || { echo "code images:   FAIL $mode"; build/image | grep FAIL; fail=1; }
done
for mode in "" -DWM_PREDECODE=0; do
    cc -std=gnu99 -O1 -g -fsanitize=address $mode source/*.c test/refs.c -lm -o build/refs || exit 1
    build/refs >/dev/null && echo "snapshot refs and checks: ok $mode" || { echo "snapshot refs and checks: FAIL $mode"; build/refs | grep FAIL; fail=1; }
done
cc -std=gnu99 -O1 -g -fsanitize=address,undefined source/*.c test/host_objects.c -lm -o build/host_objects || exit 1
build/host_objects >/dev/null && echo "host globals and tables: ok" || { echo "host globals and tables: FAIL"; build/host_objects | grep FAIL; fail=1; }
# Reading and linking modules through wm.h (decision 23), in both loops and without validation.
for mode in "-fsanitize=address,undefined" "-fsanitize=address" "-fsanitize=address -DWM_PREDECODE=0" "-fsanitize=address -DWM_HAS_VALIDATION=0"; do
    # shellcheck disable=SC2086
    cc -std=gnu99 -O1 -g -DWM_CHECKS=1 $mode source/*.c test/link.c -lm -o build/link || exit 1
    build/link >/dev/null && echo "linking modules: ok $mode" || { echo "linking modules: FAIL $mode"; build/link | grep FAIL; fail=1; }
done
# Snapshots of linked instances (decision 23): frames in both modules, shared memory and global, a host function in a table.
for mode in "-fsanitize=address,undefined" "-fsanitize=address" "-fsanitize=address -DWM_PREDECODE=0" "-fsanitize=address -DWM_HAS_VALIDATION=0" "-fsanitize=address -DWM_MEMORY_ALWAYS_PAGED=1"; do
    # shellcheck disable=SC2086
    cc -std=gnu99 -O1 -g -DWM_CHECKS=1 $mode source/*.c test/group.c -lm -o build/group || exit 1
    build/group >/dev/null && echo "linked snapshots: ok $mode" || { echo "linked snapshots: FAIL $mode"; build/group | grep FAIL; fail=1; }
done
cc -std=gnu99 -O1 -g -fsanitize=address source/*.c test/room.c -lm -o build/room || exit 1
build/room >/dev/null && echo "memory before code: ok" || { echo "memory before code: FAIL"; build/room | grep FAIL; fail=1; }
cc -std=gnu99 -O1 -g -fsanitize=address source/*.c test/pool.c -lm -o build/pool || exit 1
build/pool >/dev/null && echo "frame pool:    ok" || { echo "frame pool:    FAIL"; build/pool | grep FAIL; fail=1; }
cc -std=gnu99 -O1 -g -fsanitize=address,undefined source/*.c test/unstore.c -lm -o build/unstore || exit 1
build/unstore >/dev/null && echo "frame unstore: ok" || { echo "frame unstore: FAIL"; build/unstore | grep FAIL; fail=1; }
for size in 4096 256; do
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_FRAME_SIZE=$size source/*.c test/block.c -lm -o build/block || exit 1
    build/block >/dev/null && echo "block memory:  ok (frames $size)" || { echo "block memory:  FAIL (frames $size)"; build/block | grep FAIL; fail=1; }
done
for mode in "-DWM_FRAME_SIZE=4096" "-DWM_FRAME_SIZE=256" "-DWM_PREDECODE=0"; do
    cc -std=gnu99 -O1 -g $mode -fsanitize=address,undefined source/*.c test/clean.c -lm -o build/clean || exit 1
    build/clean >/dev/null && echo "written ahead: ok ($mode)" || { echo "written ahead: FAIL ($mode)"; build/clean | grep FAIL; fail=1; }
done
cc -std=gnu99 -O1 -g -DWM_SMALL_SIZES -DWM_TERSE_ERRORS=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-small || exit 1
python3 tools/spec.py build/wm-spec-small "$SPEC" >/dev/null && echo "spec (small):  ok" || { echo "spec (small):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_COMPUTED_GOTO=0 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-switch || exit 1
python3 tools/spec.py build/wm-spec-switch "$SPEC" >/dev/null && echo "spec (switch): ok" || { echo "spec (switch): FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_PREDECODE=0 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-inplace || exit 1
python3 tools/spec.py build/wm-spec-inplace "$SPEC" >/dev/null && echo "spec (in place): ok" || { echo "spec (in place): FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_PREDECODE=0 -DWM_FUSE=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-fuse || exit 1
python3 tools/spec.py build/wm-spec-fuse "$SPEC" >/dev/null && echo "spec (fused):  ok" || { echo "spec (fused):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -DWM_FUSE=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-nop || exit 1
python3 tools/spec.py build/wm-spec-nop "$SPEC" >/dev/null && echo "spec (as on chips): ok" || { echo "spec (as on chips): FAIL"; fail=1; }
cc -std=gnu99 -O1 -DWM_FUSE=1 -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
build/fuel >/dev/null && echo "fuel (as on chips): ok" || { echo "fuel (as on chips): FAIL"; fail=1; }
for fuse in 0 1; do
    cc -std=gnu99 -O1 -g -DWM_FUSE=$fuse -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
    build/fuel >/dev/null && echo "fuel (fuse $fuse): ok" || { echo "fuel (fuse $fuse): FAIL"; fail=1; }
done
cc -std=gnu99 -O1 -g -DWM_MEMORY_ALWAYS_PAGED=1 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-paged || exit 1
python3 tools/spec.py build/wm-spec-paged "$SPEC" >/dev/null && echo "spec (paged):  ok" || { echo "spec (paged):  FAIL"; fail=1; }
cc -std=gnu99 -O1 -DWM_MEMORY_ALWAYS_PAGED=1 -fsanitize=address,undefined source/*.c test/fuel.c -lm -o build/fuel || exit 1
build/fuel >/dev/null && echo "fuel (paged):  ok" || { echo "fuel (paged):  FAIL"; fail=1; }
for mode in "" "-DWM_MEMORY_ALWAYS_PAGED=1"; do
    cc -std=gnu99 -O1 -g $mode -fsanitize=address,undefined source/*.c test/wasi.c -lm -o build/wasi || exit 1
    build/wasi >/dev/null && echo "wasi:          ok ${mode:-(one block)}" || { echo "wasi:          FAIL $mode"; fail=1; }
done
for mode in "" "-DWM_PREDECODE=0"; do
    # Predecoded needs ASan alone (see above); in place keeps UBSan too.
    san="-fsanitize=address${mode:+,undefined}"
    cc -std=gnu99 -O1 -g $mode $san source/*.c test/paged.c -lm -o build/paged || exit 1
    build/paged >/dev/null && echo "paged memory:  ok ${mode:-(predecoded)}" || { echo "paged memory:  FAIL $mode"; fail=1; }
done
cc -std=gnu99 -O1 -g -DWM_SPEC_PAGER=3 -fsanitize=address,undefined source/*.c tools/wm-spec.c -lm -o build/wm-spec-swap || exit 1
python3 tools/spec.py build/wm-spec-swap "$SPEC" >/dev/null 2>&1 && echo "spec (swapped, 3 frames of RAM): ok" || { echo "spec (swapped): FAIL"; fail=1; }
for mode in "-fsanitize=address" "-fsanitize=address,undefined"; do
    cc -std=gnu99 -O1 -g $mode source/*.c test/swap.c -lm -o build/swap || exit 1
    build/swap >/dev/null && echo "swapping:      ok ($mode)" || { echo "swapping:      FAIL $mode"; fail=1; }
done
for mode in "-fsanitize=address" "-fsanitize=address,undefined" "-fsanitize=address -DWM_MEMORY_ALWAYS_PAGED=1"; do
    cc -std=gnu99 -O1 -g $mode source/*.c test/state.c -lm -o build/state || exit 1
    build/state >/dev/null && echo "state snapshot: ok ($mode)" || { echo "state snapshot: FAIL $mode"; fail=1; }
done
# Other frame sizes: the spec suite swapped through 3 frames, and the paging tests.
for size in 256 1024 65536; do
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_FRAME_SIZE=$size -DWM_SPEC_PAGER=3 source/*.c tools/wm-spec.c -lm -o build/wm-spec-frames || exit 1
    python3 tools/spec.py build/wm-spec-frames "$SPEC" >/dev/null 2>&1 || { echo "frames $size: FAIL spec swapped"; fail=1; continue; }
    ok=1
    for t in paged swap state; do
        cc -std=gnu99 -O1 -g -fsanitize=address -DWM_FRAME_SIZE=$size source/*.c test/$t.c -lm -o build/frames-$t || exit 1
        build/frames-$t >/dev/null || { echo "frames $size: FAIL $t"; ok=0; fail=1; }
    done
    [ $ok = 1 ] && echo "frames $size:  ok (spec swapped, paged, swap, state)"
done
# The inputs that once made a fuzz target find a bug (test/fuzz/regress/), each through its target.
xxd -i test/fuzz/snapshot.wasm > build/snapshot_wasm.h
for t in load run diff snapshot; do
    ls test/fuzz/regress/$t/* >/dev/null 2>&1 || continue
    cc -std=gnu99 -O1 -g -fsanitize=address,undefined -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -Ibuild source/*.c test/fuzz/fuzz_$t.c test/fuzz/replay.c -lm \
        -o build/replay-$t || exit 1
    build/replay-$t test/fuzz/regress/$t/* 2>/dev/null && echo "fuzz finds ($t): ok" || { echo "fuzz finds ($t): FAIL"; fail=1; }
done
# The Uno's budget: WM_TINY with the size test's host fits 30 KiB, leaving the bootloader and a host program room.
if [ -x "${AVR_BIN:-$HOME/.platformio/packages/toolchain-atmelavr/bin}/avr-gcc" ]; then
    tiny=$(tools/avr-size.sh atmega328p -DWM_TINY | awk '{print $2}')
    [ "$tiny" -le 30720 ] && echo "Uno budget:    ok ($tiny of 30720 bytes, WM_TINY)" || { echo "Uno budget:    FAIL ($tiny > 30720 bytes, WM_TINY)"; fail=1; }
fi
test/snapshot.sh >/dev/null && echo "snapshots:     ok (between builds)" || { echo "snapshots:     FAIL"; fail=1; }
out="$(tools/kernel-tests.sh)" || fail=1
echo "kernel tests:  $(tail -1 <<<"$out")"
grep '^FAIL' <<<"$out"
out="$(CFLAGS=-DWM_MEMORY_ALWAYS_PAGED=1 tools/kernel-tests.sh)" || fail=1
echo "kernel tests (paged): $(tail -1 <<<"$out")"
grep '^FAIL' <<<"$out"
exit $fail
