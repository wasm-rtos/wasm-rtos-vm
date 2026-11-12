#!/usr/bin/env bash
# Runs the wasm-rtos tests with the kernel built on this engine.
#
#   tools/kernel-tests.sh [test names...]
#
# KERNEL (default ~/wasm-rtos/wasm-rtos) and TESTS_DIR
# (default ~/wasm-rtos/wasm-rtos-tests/tests) say where they are; CFLAGS
# adds compiler flags (-DWM_MEMORY_ALWAYS_PAGED=1 pages every memory). The
# tests run in a copy, with the .wasm programs already in the repository.
set -uo pipefail
cd "$(dirname "$0")/.."
VM="$PWD"
KERNEL="${KERNEL:-$HOME/wasm-rtos/wasm-rtos}"
TESTS_DIR="${TESTS_DIR:-$HOME/wasm-rtos/wasm-rtos-tests/tests}"
CC_BIN="${CC:-cc}"
EXTRA_CFLAGS="${CFLAGS:-}"
out="$VM/build/kernel-tests${CFLAGS:+-paged}"
rm -rf "$out"
mkdir -p "$out/kernel/vm" "$out/tests"
for f in "$KERNEL"/*.c "$KERNEL"/*.h; do ln -s "$f" "$out/kernel/"; done
ln -s "$KERNEL/include" "$out/kernel/include"
ln -s "$VM/source" "$out/kernel/vm/source"
cp -R "$TESTS_DIR"/. "$out/tests/"

names=("$@")
if [ "${#names[@]}" -eq 0 ]; then
    for dir in "$out"/tests/*/; do names+=("$(basename "$dir")"); done
fi
passed=0
failed=0
for name in "${names[@]}"; do
    dir="$out/tests/$name"
    flags=()
    case "$name" in
        proc_disabled) flags+=(-DOS_HAS_PROCESSES=0) ;;
        preempt_memory_grow) flags+=(-DOS_TASK_SLICE_FUEL=1ULL) ;;
        dylink0) flags+=(-DOS_HAS_DYLINK=1) ;;
        task_memory_usage) flags+=(-DOS_HAS_MEMORY_ACCOUNTING=1) ;;
        snapshot_portable) flags+=(-DOS_TASK_SLICE_FUEL=997ULL) ;;
    esac
    # The dylink.0 test's library is built here, as the test runner builds it.
    if [ "$name" = "dylink0" ] && [ ! -f "$dir/libdylink0.so" ]; then
        wasm_cc="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
        wasm_ld="${WASM_LD:-wasm-ld}"
        pic_flags=()
        # grep without -q reads the whole help: with pipefail an early exit would count as no.
        if "$wasm_ld" --help 2>&1 | grep -- '--experimental-pic' >/dev/null; then
            pic_flags+=(--experimental-pic)
        fi
        "$wasm_cc" --target=wasm32 -nostdlib -fPIC -O0 -c "$dir/library.c" -o "$dir/library.o" &&
            "$wasm_ld" -m wasm32 ${pic_flags[@]+"${pic_flags[@]}"} --shared --no-entry \
                --unresolved-symbols=import-dynamic --export=library_add --export=library_name \
                --export=library_fill --export=library_apply --export=library_increment \
                -o "$dir/libdylink0.so" "$dir/library.o" >"$dir/build.txt" 2>&1
    fi
    if ! "$CC_BIN" -std=c11 -O1 -g ${SANITIZE:+-fsanitize=address,undefined} -w -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            $EXTRA_CFLAGS ${flags[@]+"${flags[@]}"} -I"$out/kernel" "$dir/main.c" "$out"/kernel/*.c "$VM"/source/wm_*.c -lm -o "$dir/runner" >"$dir/build.txt" 2>&1; then
        echo "FAIL $name (does not build: $(grep -m1 -E 'error' "$dir/build.txt"))"
        failed=$((failed + 1))
        continue
    fi
    if (cd "$dir" && timeout 120 ./runner >"$dir/out.txt" 2>&1); then
        echo "ok   $name ($(grep -c '^PASS' "$dir/out.txt") checks)"
        passed=$((passed + 1))
    else
        echo "FAIL $name: $(grep -m2 '^FAIL' "$dir/out.txt" | tr '\n' ' ')"
        failed=$((failed + 1))
    fi
done
echo "total: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
