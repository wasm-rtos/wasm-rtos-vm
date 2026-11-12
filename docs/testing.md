# Testing wasm-rtos-vm

## Everything at once

```sh
tools/check.sh
```

Runs all of the below that needs no board, in about ten minutes, and must
pass after every change. It expects the official test suite converted to
JSON in `~/wasm-rtos/third-party/spec-2.0/json` (`SPEC` sets another place)
and the wasm-rtos kernel and tests next to this repository (see
[wasm-rtos tests](#wasm-rtos-tests)).

## Spec tests

The official WebAssembly test suite (tag `wg-2.0`), 27437 checks:

```sh
cc -std=gnu99 -O1 source/*.c tools/wm-spec.c -lm -o build/wm-spec
wast2json test.wast -o json/test.json     # for every .wast of the suite
tools/spec.py build/wm-spec json/
```

`check.sh` runs them in many builds: with AddressSanitizer and UBSan, with
32-bit sizes and terse errors, with a `switch` loop, in place, with fused
records, as on chips, with every memory paged, swapped with 3 frames of RAM
(`-DWM_SPEC_PAGER=3`), with frames of 256, 1024 and 65536 bytes, and with a
code cache of one byte (`-DWM_SPEC_CODE_LIMIT=1`), so that every call that
is not on the stack makes its records again.

UBSan makes the interpreter too large to predecode, so its runs go through
the loop in place; the runs with AddressSanitizer alone go through the
predecoded one. `test/predecoded.c` checks that a plain optimized build
predecodes.

For a build without validation, run `tools/spec.py` with `--trusted`: it
skips the checks that invalid programs are refused.

## Fuel

`test/fuel.c` runs functions of `test/fuel.wat` whole and in slices of
fuel, and checks that the results and the fuel used match instruction for
instruction; also host functions that yield or set the fuel to 0, and a
trap after many resumes:

```sh
wat2wasm test/fuel.wat -o test/fuel.wasm   # only after changing the .wat
cc -std=gnu99 -O1 source/*.c test/fuel.c -lm -o build/fuel && build/fuel
```

## Memory, swapping and the code cache

- `test/paged.c`: a memory that turns paged when the heap refuses a block,
  `memory.copy` across frames, untouched frames read as zeros.
- `test/swap.c`: frames to a pager's storage and back, dirty and clean,
  background reads and writes.
- `test/state.c`: a call stopped by fuel, its runtime, instance and module
  freed but its memory kept, goes on in a new instance.
- `test/room.c`: a heap capped by a counting allocator; `memory.grow`
  gets its frames by dropping another function's records, and fails
  rather than drop those of the function that runs.
- `test/code.c`: calls with code caches from unlimited down to one byte,
  through recursion, a host function that calls back in and empties the
  cache, and calls sliced by fuel, each slice resumed in a new runtime from
  a snapshot.

## Snapshots between builds

`test/snapshot.sh` stops programs at many points by fuel in one build and
finishes them in another: one with 32-bit pointers (with emcc and node),
one running in place, one with every memory paged. The result and the
total fuel must match a run in one go.

## wasm-rtos tests

`tools/kernel-tests.sh [names...]` builds the wasm-rtos kernel on this
engine and runs its tests: `KERNEL` (default `~/wasm-rtos/wasm-rtos`) and
`TESTS_DIR` (default `~/wasm-rtos/wasm-rtos-tests/tests`) say where they
are, `CFLAGS` adds compiler flags (`-DWM_MEMORY_ALWAYS_PAGED=1` pages every
memory).

## Size and speed

- `tools/avr-size.sh [mcu] [flags]`: flash and static RAM on AVR.
- `tools/wm-bench.c`: CoreMark (or another program exporting `run`) on the
  computer; arguments are the program, a fuel slice (0: none) and a code
  cache limit. Noise is a few percent: compare builds by turns.

```sh
cc -std=gnu99 -O3 source/*.c tools/wm-bench.c -lm -o build/wm-bench
build/wm-bench build/coremark.wasm 0
```
