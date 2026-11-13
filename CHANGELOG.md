# Changelog

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
