# wasm-rtos-vm

A small WebAssembly interpreter for microcontrollers down to 8-bit AVR, the
engine of [wasm-rtos](https://github.com/wasm-rtos/wasm-rtos). It runs the
module's own instructions: fuel counts them, and a stopped program is saved
and continued anywhere by its position in the module.

Modules run from their bytes as loaded: loading decodes
and validates the whole module once and builds a side table with every
branch target, so nothing is compiled and execution never searches the
code. Values live in 32-bit slots.

## Status

- WebAssembly 2.0 without SIMD: the official test suite (tag `wg-2.0`)
  passes, 27437 checks, also under AddressSanitizer and UBSan.
- Optional parts, each on by default: `WM_HAS_FLOAT` (on where `double`
  has 64 bits, so not on AVR yet), `WM_HAS_I64`, `WM_HAS_REF_TYPES`
  (reference types and table instructions; `call_indirect` stays). A module
  that needs a part left out is refused with an `unsupported:` error.
- Validation (`WM_HAS_VALIDATION`, on by default): loading checks the whole
  program as the standard requires. Built without it, loading still builds
  the side table on the device, but trusts the program: type, alignment and
  other validity checks are left out, so an invalid program can misbehave.
  Checks that keep the engine's own arrays in bounds stay.

Flash and static RAM on AVR (ATmega328P), without floating point, measured
with `tools/avr-size.sh [mcu] [flags]`:

| Build | Flash | RAM |
|---|---|---|
| WebAssembly 2.0 | 32.6 KB | 130 B |
| without i64 and reference types | 29.5 KB | 130 B |
| without validation | 28.7 KB | 130 B |
| without validation, i64 and reference types | 25.5 KB | 130 B |

## Predecoding

With `WM_PREDECODE` (the default where labels can be values, except on
AVR) each function is also written at load as records, one per
instruction, in 32-bit cells: the first holds the handler and a 16-bit
operand (a local's index, a small constant, a call's function, a branch's
distance), so most instructions take one cell and branches two; the side
table is freed then. The records follow the instructions one to one, so
fuel counts the module's instructions and a snapshot position converts to
an offset in the module and back: snapshots move between predecoding
builds and ones that run the bytes in place. The cost is RAM, about twice
the code (CoreMark: 7.2 KB of code, 16.7 KB of records). A module whose
operands do not fit, or a chip without the memory for its records, runs
the bytes in place.

## Fuel and suspension

One unit of fuel is one WebAssembly instruction, so the same program uses
the same fuel on every device. With fuel set (`wm_set_fuel`), a call stops
before the instruction that finds none left and returns `wm_fuel_exhausted`;
a host function can return `wm_yield(runtime)` to stop the program once it
returns, leaving its results to be written before `wm_resume` goes on. All
the state is plain data in the runtime. `test/fuel.c` checks that sliced
runs match whole ones instruction for instruction:

```sh
wat2wasm test/fuel.wat -o test/fuel.wasm
cc -std=c99 -O1 source/*.c test/fuel.c -lm -o build/fuel && build/fuel
```

## Snapshots

`wm_snapshot_write` saves a suspended call, `wm_snapshot_read` loads it
into a fresh runtime and instance of the same module, which goes on with
`wm_resume`. Positions are offsets in the module and side table indices,
functions are indices, and nothing holds a host address, so a snapshot
from one build and chip loads into another: `test/snapshot.sh` stops
programs in a 64-bit build and finishes them in a 32-bit one (emcc), with
the same result and fuel as a run in one go. The width of references
matters only to modules that can have them on the value stack.

A state snapshot (`wm_snapshot_write_state`) is all of it but the memory's
bytes, for a program swapped out with its memory left to the pager: the
instance gives the memory up (`wm_instance_detach_memory`), everything else
is freed, and a new instance of the module takes it back
(`wm_instantiate_with_memory`, no data segments, no start function) before
the state loads.

## dylink.0

`source/wm_dylink.c` (optional) loads a position-independent program with
the shared libraries it needs into one runtime, as an ELF process: one
linear memory and one function table, the convention's globals made by the
loader. A call into a library is a call into its code, so fuel and stops
work across modules.

## Memory

`wm_set_allocator` routes every allocation through the host, which can
count what each program takes; `wm_module_code_bytes` tells what a module's
side table and records take, `wm_runtime_stack` where the value stack is.

## wasm-rtos tests

`tools/kernel-tests.sh` builds the wasm-rtos kernel on this engine and runs
its tests (`KERNEL` and `TESTS_DIR` say where they are).

## Spec tests

```sh
cc -std=c99 -O1 source/*.c tools/wm-spec.c -lm -o build/wm-spec
wast2json test.wast -o json/test.json     # for every .wast of the suite
tools/spec.py build/wm-spec json/
```

For a build without validation, run `tools/spec.py` with `--trusted`: it
skips the checks that invalid programs are refused.

## ESP32

Arduino and ESP-IDF build with `-fno-jump-tables -fno-tree-switch-conversion`,
which turns the interpreter's `switch` into a chain of comparisons. Build
the engine's files with `-fjump-tables -ftree-switch-conversion` (about 20%
faster).

`-DWM_IRAM` (off by default) places the interpreter loop in IRAM, about
10 KB of heap less. Code runs from flash through a 32 KB cache, and with
the loop in IRAM a kernel that switches programs often has the cache to
itself: on a classic ESP32 with wasm-rtos, 500 round trips between two
programs through a queue take 27 ms with it and 52 ms without. A program
that runs alone gains nothing (CoreMark 12.97 against 12.96). Turn it on
when programs talk to each other a lot and the heap can spare it.
