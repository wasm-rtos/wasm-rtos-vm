#!/usr/bin/env bash
# Runs the wasm-rtos tests with the kernel built on this engine.
#
#   tools/kernel-tests.sh [test names...]
#
# KERNEL (default ~/wasm-rtos/wasm-rtos) and TESTS_DIR
# (default ~/wasm-rtos/wasm-rtos-tests/tests) say where they are. The
# tests run in a copy, with the .wasm programs already in the repository.
set -uo pipefail
cd "$(dirname "$0")/.."
VM="$PWD"
KERNEL="${KERNEL:-$HOME/wasm-rtos/wasm-rtos}"
TESTS_DIR="${TESTS_DIR:-$HOME/wasm-rtos/wasm-rtos-tests/tests}"
CC_BIN="${CC:-cc}"
out="$VM/build/kernel-tests"
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
        "$wasm_cc" --target=wasm32 -nostdlib -fPIC -O0 -c "$dir/library.c" -o "$dir/library.o" &&
            "${WASM_LD:-wasm-ld}" -m wasm32 --experimental-pic --shared --no-entry \
                --unresolved-symbols=import-dynamic --export=library_add --export=library_name \
                --export=library_fill --export=library_apply --export=library_increment \
                -o "$dir/libdylink0.so" "$dir/library.o" >"$dir/build.txt" 2>&1
    fi
    if ! "$CC_BIN" -std=c11 -O1 -g ${SANITIZE:+-fsanitize=address,undefined} -w -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
            ${flags[@]+"${flags[@]}"} -I"$out/kernel" "$dir/main.c" "$out"/kernel/*.c "$VM"/source/wm_*.c -lm -o "$dir/runner" >"$dir/build.txt" 2>&1; then
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
