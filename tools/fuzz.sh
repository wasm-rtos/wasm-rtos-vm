#!/usr/bin/env bash
# Builds and runs a fuzz target of test/fuzz/ with libFuzzer.
#   tools/fuzz.sh target [seconds] [build]     build: plain (default), chips, paged
# The corpus starts from the spec suite's modules ($SPEC) and test/*.wasm and
# grows in build/fuzz/corpus-<target>; crashes land in build/fuzz/.
set -euo pipefail
cd "$(dirname "$0")/.."
target="${1:?usage: tools/fuzz.sh load|run|diff|snapshot [seconds] [plain|chips|paged]}"
seconds="${2:-60}"
build="${3:-plain}"
SPEC="${SPEC:-$HOME/wasm-rtos/third-party/spec-2.0/json}"
if [ -z "${FUZZ_CC:-}" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang; else FUZZ_CC=clang; fi
fi
case "$build" in
    plain) flags="" ;;
    chips) flags="-DWM_FUSE=1 -DWM_NOP_RUNS=1" ;;
    paged) flags="-DWM_MEMORY_ALWAYS_PAGED=1 -DWM_FRAME_SIZE=256" ;;
    *) echo "unknown build $build"; exit 2 ;;
esac
# UBSan makes the interpreter too large to predecode: the targets that run code are built with ASan only.
case "$target" in
    load|snapshot) sanitizers="fuzzer,address,undefined" ;;
    *) sanitizers="fuzzer,address" ;;
esac
out=build/fuzz
mkdir -p "$out/corpus-$target"
if [ "$target" = snapshot ]; then
    xxd -i test/fuzz/snapshot.wasm > "$out/snapshot_wasm.h"
fi
# shellcheck disable=SC2086
"$FUZZ_CC" -std=gnu99 -O1 -g -fsanitize="$sanitizers" -fno-sanitize-recover=all -I"$out" $flags \
    source/*.c "test/fuzz/fuzz_$target.c" -lm -o "$out/fuzz_$target-$build"
max_len=65536
if [ "$target" != snapshot ]; then
    seeds=(test)
    [ -d "$SPEC" ] && seeds+=("$SPEC")
else
    # Real snapshots of the program to start from; a memory of 64 KiB is in each.
    mkdir -p "$out/seeds-snapshot"
    FUZZ_SEEDS="$out/seeds-snapshot" "$out/fuzz_$target-$build" -runs=0 "$out/seeds-snapshot" >/dev/null 2>&1
    seeds=("$out/seeds-snapshot")
    max_len=200000
fi
"$out/fuzz_$target-$build" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 -max_len="$max_len" \
    -artifact_prefix="$out/crash-$target-$build-" "$out/corpus-$target" "${seeds[@]}" 2>&1 | tail -20
