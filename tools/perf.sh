#!/usr/bin/env bash
# The engine's speed and size against another tree of it, by counts that do
# not move with the machine's load: machine instructions per WebAssembly
# instruction of a short CoreMark under cachegrind (the same run every time:
# a clock that moves 5 s a call), and the Uno test program's flash.
#   tools/perf.sh base-tree [program.wasm]
# Both trees are built here by the same compiler, the engine's sources of
# each with this tree's wm-bench. Fails when this tree is worse than the base
# by more than PERF_LIMIT percent (default 2); a Markdown table goes to
# $GITHUB_STEP_SUMMARY when it is set.
set -euo pipefail
cd "$(dirname "$0")/.."
base="${1:?usage: tools/perf.sh base-tree [program.wasm]}"
wasm="${2:-build/coremark-wm.wasm}"
limit="${PERF_LIMIT:-2}"
out=build/perf
mkdir -p "$out"
fail=0
rows=""

# Machine instructions per WebAssembly instruction: tree, build flags.
per_instruction() {
    local bin="$out/bench-$(echo "$1 $2" | cksum | cut -d' ' -f1)"
    # shellcheck disable=SC2086
    cc -std=gnu99 -O2 $2 "$1"/source/*.c tools/wm-bench.c -lm -o "$bin"
    WM_BENCH_CLOCK_STEP=5000 valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null \
        "$bin" "$wasm" 10000 > "$out/run.txt" 2> "$out/grind.txt"
    local machine wasm_count
    machine=$(sed -n 's/.*I *refs: *\([0-9,]*\).*/\1/p' "$out/grind.txt" | tr -d ,)
    wasm_count=$(sed -n 's/^\([0-9]*\) instructions.*/\1/p' "$out/run.txt")
    if [ -z "$machine" ] || [ -z "$wasm_count" ]; then
        echo "no counts from $1 [$2]:" >&2
        cat "$out/run.txt" "$out/grind.txt" >&2
        exit 1
    fi
    awk -v m="$machine" -v w="$wasm_count" 'BEGIN { printf "%.4f", m / w }'
}

compare() { # what, base value, this value, unit
    local change
    change=$(awk -v a="$2" -v b="$3" 'BEGIN { printf "%+.2f", (b - a) / a * 100 }')
    rows+="| $1 | $2 | $3 | $change% |"$'\n'
    printf "%-52s %12s %12s %8s%%\n" "$1" "$2" "$3" "$change"
    if awk -v c="$change" -v l="$limit" 'BEGIN { exit !(c > l) }'; then
        echo "  worse by more than $limit%"
        fail=1
    fi
}

printf "%-52s %12s %12s %9s\n" "" "base" "this" "change"
for build in "plain:" "as for chips:-DWM_FUSE=1 -DWM_NOP_RUNS=1"; do
    name="${build%%:*}"
    flags="${build#*:}"
    compare "CoreMark, machine instructions each ($name)" "$(per_instruction "$base" "$flags")" \
        "$(per_instruction . "$flags")"
done
if command -v avr-gcc > /dev/null || [ -x "${AVR_BIN:-}/avr-gcc" ]; then
    export AVR_BIN="${AVR_BIN:-$(dirname "$(command -v avr-gcc)")}"
    # Each tree's own script: it builds that tree's engine.
    base_flash=$("$base/tools/avr-size.sh" | sed -n 's/^flash \([0-9]*\).*/\1/p')
    this_flash=$(tools/avr-size.sh | sed -n 's/^flash \([0-9]*\).*/\1/p')
    compare "Uno test program, flash bytes" "$base_flash" "$this_flash"
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "| | base | this | change |"
        echo "|---|---:|---:|---:|"
        printf "%s" "$rows"
    } >> "$GITHUB_STEP_SUMMARY"
fi
exit $fail
