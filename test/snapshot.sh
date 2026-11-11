#!/usr/bin/env bash
# Snapshots between builds: each program stopped by fuel in one build and
# finished in the other must give the result and the total fuel of a run in
# one go. With emcc and node, the other build has 32-bit pointers; without,
# it is the same build.
set -uo pipefail
cd "$(dirname "$0")/.."
CLANG="${WASM_CC:-/opt/homebrew/opt/llvm/bin/clang}"
mkdir -p build
cc -std=gnu99 -O2 source/*.c tools/wm-snapshot.c -lm -o build/wm-snapshot || exit 1
other=(build/wm-snapshot)
if command -v emcc >/dev/null && command -v node >/dev/null; then
    emcc -std=gnu99 -O2 -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 source/*.c tools/wm-snapshot.c \
        -o build/wm-snapshot32.js 2>/dev/null && other=(node build/wm-snapshot32.js)
fi
"$CLANG" --target=wasm32 -nostdlib -O2 -Wl,--no-entry -o build/sort.wasm test/sort.c || exit 1
fail=0
check() # program function argument fuel
{
    local saved loaded total after expected
    saved="$(build/wm-snapshot save "$1" "$2" "$3" "$4" build/snapshot.bin)"
    loaded="$("${other[@]}" load "$1" "$2" build/snapshot.bin)"
    expected="$(sed -n 's/.*going on here: result \([0-9]*\), \([0-9]*\) instructions.*/\1 \2/p' <<<"$saved")"
    after="$(sed -n 's/.*went on: result \([0-9]*\), \([0-9]*\) instructions.*/\1 \2/p' <<<"$loaded")"
    total="${after%% *} $(( ${after##* } + $4 ))"
    if [ -n "$after" ] && [ "$total" = "$expected" ]; then
        echo "ok   $1 $2($3) stopped at $4: result and fuel ($expected) match"
    else
        echo "FAIL $1 $2($3) stopped at $4: expected '$expected', got '$total'"
        fail=1
    fi
}
check test/fuel.wasm fib 25 777777
check build/sort.wasm run 20 1234567
# Between a predecoding build and one that runs the bytes in place.
cc -std=gnu99 -O2 -DWM_PREDECODE=0 source/*.c tools/wm-snapshot.c -lm -o build/wm-snapshot-inplace || exit 1
other=(build/wm-snapshot-inplace)
check test/fuel.wasm fib 25 333333
check build/sort.wasm run 20 4567890
# Between a memory in one block and a paged one.
cc -std=gnu99 -O2 -DWM_MEMORY_ALWAYS_PAGED=1 source/*.c tools/wm-snapshot.c -lm -o build/wm-snapshot-paged || exit 1
other=(build/wm-snapshot-paged)
check test/fuel.wasm fib 25 555555
check build/sort.wasm run 20 3456789
exit $fail
