#!/usr/bin/env bash
# The check after an edit, in under a minute (docs/testing.md,
# Checkpoints): the engine built with warnings as errors, the spec suite
# predecoded, with the loop in place fusing pairs, in place, paged and swapped, the inputs the fuzzers
# once found bugs with.
# With SHA=1 also whether the loops' machine code differs from the last
# commit's (Mac -O2, and Xtensa -O2 where its GCC is installed): a change
# for readability must not change it (decision 13).
set -uo pipefail
cd "$(dirname "$0")/.."
SPEC="${SPEC:-$HOME/wasm-rtos/third-party/spec-2.0/json}"
XTENSA="${XTENSA:-$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf}"
mkdir -p build
fail=0
python3 tools/check-api.py || fail=1
cc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Wcast-qual -Werror source/*.c tools/wm-spec.c -lm \
    -o build/quick-werror && echo "build:         ok" || { echo "build:         FAIL"; exit 1; }
for mode in "" "-DWM_FUSE=1" -DWM_PREDECODE=0 -DWM_MEMORY_ALWAYS_PAGED=1 -DWM_SPEC_PAGER=3; do
    cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 $mode source/*.c tools/wm-spec.c -lm -o build/quick-spec || exit 1
    python3 tools/spec.py build/quick-spec "$SPEC" > /dev/null && echo "spec:          ok ${mode:-(predecoded)}" ||
        { echo "spec:          FAIL $mode"; fail=1; }
done
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 source/*.c test/area.c -lm -o build/quick-area || exit 1
build/quick-area >/dev/null && echo "code area:     ok" || { echo "code area:     FAIL"; fail=1; }
cc -std=gnu99 -O1 -g -fsanitize=address -DWM_CHECKS=1 source/*.c test/image.c -lm -o build/quick-image || exit 1
build/quick-image > /dev/null && echo "code images:   ok" || { echo "code images:   FAIL"; fail=1; }
xxd -i test/fuzz/snapshot.wasm > build/snapshot_wasm.h
for t in load run diff snapshot image; do
    ls test/fuzz/regress/$t/* > /dev/null 2>&1 || continue
    cc -std=gnu99 -O1 -g -fsanitize=address -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -Ibuild source/*.c \
        test/fuzz/fuzz_$t.c test/fuzz/replay.c -lm -o build/quick-$t || exit 1
    build/quick-$t test/fuzz/regress/$t/* 2> /dev/null && echo "fuzz finds:    ok ($t)" ||
        { echo "fuzz finds:    FAIL ($t)"; fail=1; }
done
if [ "${SHA:-0}" = 1 ]; then
    base=build/quick-base
    rm -rf "$base" && git worktree add -q --detach "$base" HEAD
    code() { # tree compiler objdump flags...
        local tree=$1 cc=$2 dump=$3
        shift 3
        for f in wm_exec wm_fast; do
            $cc -std=gnu99 "$@" -c "$tree/source/$f.c" -o build/quick-$f.o && $dump -d build/quick-$f.o | tail -n +4
        done | shasum | cut -c1-12
    }
    same() { [ "$(code "$base" "$@")" = "$(code . "$@")" ] && echo "same" || echo "CHANGED"; }
    echo "machine code:  Mac -O2 $(same cc objdump -O2), as for chips $(same cc objdump -O2 -DWM_FUSE=1)"
    [ -x "$XTENSA-gcc" ] && echo "machine code:  Xtensa -O2 $(same "$XTENSA-gcc" "$XTENSA-objdump" -O2 -mlongcalls)"
    git worktree remove --force "$base"
fi
exit $fail
