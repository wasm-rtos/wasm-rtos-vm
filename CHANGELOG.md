# Changelog

## Unreleased

- **Fused records chosen by the profile**, made from `wm_ops.def`:
  `local.tee` with a constant, `local.get` with an 8- or 16-bit load, a
  constant with `shl` or `shr_u`. CoreMark +2.9% on ESP32, +3.8% on a
  Black Pill, +4.3% on a Mac. The loop in place's short i32 paths come
  from the list too.
- **Rules for the hot code** (decision 13), and checkpoints for testing:
  `tools/quick.sh` after an edit, `check.sh` after a step, the boards
  and a pull request after a piece of work.
- **What runs, counted** (decision 12): a `WM_PROFILE` build counts every
  record and pair of records the fast loop runs and every opcode run in
  place; `tools/profile.py` prints them. CI compares the speed (machine
  instructions per WebAssembly instruction of a short CoreMark under
  cachegrind) and the AVR test program's flash with a pull request's base, and fails past
  2% worse.
- **Snapshots are checked as modules are** (decision 11): reading one
  validates each frame's function again up to the frame's position, and
  refuses a frame between instructions, with another side table entry or
  a stack of the wrong height, before anything of the instance changes.
  A function reference on the stack is written as its index: it no longer
  points into the instance it was taken from (that was a use after free
  in a fresh instance). A CRC-32 ends every snapshot. **The snapshot
  format is now version 2**: version 1 snapshots do not load.
- **Fuzzing** (decision 10): libFuzzer targets for the loader, running a
  module, snapshots, and the engine against itself (predecoded, in place
  and in slices with a snapshot must agree); `tools/fuzz.sh`, a minute of
  each in CI. It found two reads past the end of a module in the loader,
  fixed. After a call that trapped the fuel left is not defined.
- **The instructions described once** (decision 9): the predecoded records
  and the i32 operations both loops run are lines of `source/wm_ops.def`,
  from which their kinds, the handler table, the predecoder's choice and
  the operations themselves are made. Speed unchanged (ESP32 -Os +0.9%).
- **A faster fast loop** (decision 8): the top of the stack in a register,
  canonical types with `call_indirect` comparing pointers, handlers of
  their own for byte and 16-bit loads and stores and for one-slot globals,
  and the stack pointer kept in a register on Xtensa. CoreMark +28.8% on
  ESP32, +15.5% on a Mac; a function object is 12 bytes smaller on 32-bit
  chips.
- **Less memory for every instance and module** (decision 7): function
  objects only for a module's own functions and its unlinked imports, and
  the module's arrays trimmed to their elements once it is loaded. The
  tour program's module is 26% smaller and each of its processes 920 bytes
  smaller on ESP32; the Uno test program runs again.

## 1.0.0 — 2026-11-13

The first release: the WebAssembly interpreter wasm-rtos runs programs on.

- **An interpreter that can stop anywhere.** Programs run for a budget of
  fuel (`wm_set_fuel`) and stop at any instruction; a running program is
  saved and restored (`wm_snapshot_*`) as a stream that names functions and
  memory, not host addresses, so it goes on in another build or on another
  machine.
- **Predecoded code as a cache.** Functions are decoded into records packed
  in 32-bit cells when first called; the code cache drops them when memory
  is short, and a build can run the bytecode in place instead.
- **Paged memory.** Linear memory in frames (`WM_FRAME_SIZE`, 4 KiB by
  default) behind a TLB, with a pager the host gives: frames leave RAM and
  come back on touch, read and written in the background while the program
  waits or goes on, chosen by a CLOCK hand. A memory stays one block, at
  full speed, while RAM has room, leaves whole and comes back whole; frames
  come from a pool set aside so they never cut up the heap. A frame's slot
  can be read and forgotten (`wm_memory_frame_slot`,
  `wm_memory_frame_unstore`) for hosts with several stores.
- **State snapshots** of an instance apart from its memory, for swapping a
  program out.
- **`dylink.0`** programs and shared libraries, each process with its own
  library data.
- **Build options** for small chips: no i64, no floats, no reference types,
  no validation, no predecoding, memory always one block.
- Checked by `tools/check.sh` in CI: the spec tests in every build, paged,
  swapped and with three frames of RAM; snapshots between builds; the
  kernel's tests.

Decisions: [`docs/decisions.md`](docs/decisions.md).
