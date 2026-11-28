# Decisions

Why the engine is the way it is: each entry says what was decided, why, and
what was considered instead. New entries start as **Proposed** and are
discussed before any code; once built they say what was measured.
What was tried and not kept, with its numbers, is in
[experiments.md](experiments.md).

| # | Decision | Built | What it gave |
|---|---|---|---|
| [1](#1-the-interpreter-counts-fuel-itself) | The interpreter counts fuel itself | 2026-11-08 | fuel = the module's instructions, the same on every device |
| [2](#2-records-follow-instructions-one-to-one) | Records follow instructions one to one | 2026-11-09 | snapshots move between builds and chips |
| [3](#3-code-is-a-cache) | Code is a cache | 2026-11-12 | 14–36% less RAM for code; ESP32 runs 18 programs at once, not 12 |
| [4](#4-memory-in-one-block-while-ram-has-room) | Memory in one block while RAM has room | 2026-11-12 | Mac CoreMark with a pager +10.7% |
| [5](#5-no-tuning-of-code-layout-per-processor) | No tuning of code layout per processor | 2026-11-12 | — (a rule for changes) |
| [6](#6-frames-come-from-a-pool-a-block-is-frames-in-a-row) | Frames come from a pool | 2026-11-12 | an ESP32 memory gets its block back (912 ms) |
| [7](#7-an-instance-and-a-module-keep-only-the-memory-they-use) | An instance and a module keep only what they use | 2026-11-15 | module −26%, −920 bytes a process on ESP32; Uno runs again |
| [8](#8-the-fast-loop-does-what-other-interpreters-do) | The fast loop does what other interpreters do | 2026-11-15 | CoreMark ESP32 +28.8%, Black Pill +21.7%, Mac +15.5% |
| [9](#9-the-instructions-are-described-once) | The instructions are described once | 2026-11-15 | one list for every record; speed unchanged |
| [10](#10-the-engine-is-fuzzed-and-against-itself) | The engine is fuzzed, and against itself | 2026-11-15 | two loader bugs found and fixed; the two loops agree |
| [11](#11-a-snapshot-is-checked-as-a-module-is) | A snapshot is checked as a module is | 2026-11-15 | no use after free on references; a bad snapshot is refused |
| [12](#12-what-runs-is-counted-and-speed-is-guarded-by-counts) | What runs is counted, and speed is guarded by counts | 2026-11-16 | a profile of records and pairs; CI fails a change 2% slower |
| [13](#13-hot-code-is-written-for-people-and-checked-by-its-machine-code) | Hot code is written for people, and checked by its machine code | 2026-11-16 | rules for the loops; fast loops as variants |
| [14](#14-operands-in-records-as-overlays-on-them) | Operands in records, as overlays on them (replaced by 16) | 2026-11-16 | 18 forms; CoreMark ESP32 +3.7%, Black Pill +8.8%; exact fuel and snapshots kept |
| [15](#15-records-in-storage-the-processor-reads-ram-for-the-hot-ones) | Records in storage the processor reads, RAM for the hot ones | 2026-11-20 | code from flash with no RAM: ESP32 −3.2%, Black Pill −5.8%; short of RAM −1.6% instead of −48% |
| [16](#16-operands-in-slots-fixed-by-depth-in-records-that-overlay-the-stacks) | Operands in slots fixed by depth, in records that overlay the stack's | 2026-11-21 | the register loop: CoreMark ESP32 +33%, Black Pill +28%; fuel and snapshots exact as before |

## 1. The interpreter counts fuel itself

**Decided.** One unit of fuel is one WebAssembly instruction of the module,
counted by the interpreter as it runs.

**Why.** Preemption, fair shares and snapshots need a program to stop at a
known point, on every device the same. Counting in the interpreter needs
nothing from the program: any `.wasm` from any compiler is counted.

**Instead.** Rewriting modules before they run to count for themselves
(instrumentation): an extra tool, and programs differ from what was built.
Stopping by time: points differ from device to device, snapshots lose their
meaning.

## 2. Records follow instructions one to one

**Decided.** Predecoded records are one per instruction (fused runs are the
first record of the run), so a record's position converts to an offset in
the module and back.

**Why.** Fuel stays the module's instructions, and a snapshot taken from
records loads into a build that runs the bytes in place, on another chip,
with other pointer sizes.

**Instead.** A compiler to a denser form (registers, superinstructions that
drop records): faster on paper, but positions would no longer map back, and
snapshots and fuel would depend on the build.

**Amended by decision 16 (2026-11-21).** What this decision is for stays:
fuel is the module's instructions, and positions are the module's, in every
build. How it holds changed: the register loop's records stand for several
instructions each, and it stops only where its frame is the stack
machine's (a segment's start); fuel is charged by segments, and the loop in
place counts the rest of one exactly. Positions convert there, and anywhere
else the loop in place runs.

## 3. Code is a cache

**Decided.** A function's records are made at its first call and dropped by
CLOCK when memory is short; a call into a function without records stops
the fast loop like a TLB miss; functions on a call stack keep theirs.
Memory of programs comes before code.

**Why.** Only code that runs takes RAM (14–36% less in the wasm-rtos tests),
and a board without room for all of it still runs fast (Mac CoreMark 2045
with a 1-byte cache against 973 in place).

**Instead.** All records at load, or none (the module in place): what came
before, two to three times slower on boards that could not hold them all.

## 4. Memory in one block while RAM has room

**Decided.** With a pager a memory stays in one block, reached by one bounds
check, while the host's RAM budget has room for all of it; it leaves RAM
whole when RAM is short and goes on paged; it goes back into one block when
RAM has room again (`wm_memory_block_out`, `wm_memory_block_in`).

**Why.** The paged path costs on every access: about 10% on the Mac
(CoreMark 2343 against 2595). As an OS keeps huge pages and splits them
under pressure, programs that fit pay nothing.

**Instead.** Every memory paged once there is a pager: what came before.

## 5. No tuning of code layout per processor

**Decided (2026-11-12).** The fast loop is not aligned or laid out by hand
for one processor or toolchain.

**Why.** On an ESP32 a change elsewhere moved 34 of the 76 handlers across
4-byte words, and a loop over memory went 6.6% slower; aligning the
handlers fixes that build only, and moves again with the next change.
Gains have to come from doing less work, which every processor sees.
Single loops on a board may move by a few percent between builds; builds
are compared by CoreMark and the board checks together.

**Instead.** `-falign-labels` or per-handler alignment: per toolchain,
fragile (on Xtensa `-falign-labels` built code that crashed).

## 6. Frames come from a pool; a block is frames in a row

**Decided and built (2026-11-12).**

### Problem

Frames of paged memories are allocated one by one from the heap, among
everything else, and cut it into pieces. On an ESP32 with a page store,
after a sleeping program's memory was paged, the heap had 163 KB free but
no piece larger than 59 KB, so its memory of 64 KB could not go back into
one block: `wm_memory_block_in` asks the heap for one piece and the heap had
none. Decision 4 then only works where the heap happens to allow it.

### Decision

As an OS manages physical memory: a fixed pool of frames, and a huge page
as frames in a row.

- **The pool.** The host gives the engine one region for frames,
  `wm_set_frame_pool(memory, frames)`, at the start. With a pool, every
  frame of a memory with a pager comes from it, never from the heap. The
  kernel makes it from the page store's RAM budget (`ram_frames`, already
  the number of frames it keeps in RAM, plus the few a store writing in the
  background may have on their way out: `os_page_ram_bytes`), or takes a
  region the platform names (`OsPageStore.ram`: external RAM, for one).
- **One way to a block.** A memory with a pager is made paged and goes into
  one block at once when the pager lets it (`wm_memory_block_in`): making a
  block and putting a memory back into one are the same code.
- **A block is a run.** A memory in one block is its frames in the pool,
  one after another in order; `data` points at the first. No heap
  allocation is needed to make a block or to grow one.
- **Frames move.** The engine knows every pointer to a frame (the memory's
  frame table, the TLB), so it can copy a frame to another place in the
  pool and point there. To make a block or put a memory back into one, it
  finds a window of the pool with room and moves the frames in the way to
  free places outside it. Putting a memory back into one block moves its
  own frames into place, reads those in storage, and needs no storage at
  all when they are in RAM: no write to the card, no heap.
- **Without a pool** nothing changes: frames and blocks from the heap, as
  now.

### What may not move

- A frame pinned by the host (a transfer in the background) or being read
  or written in the background.
- The frames and the block of a memory in use: the memory of an instance
  whose function is on a call stack of any runtime that is running or
  suspended in a host function. The engine finds them as it finds the
  functions whose code it may not drop (decision 3). Both loops read the
  memory's address again after every host call and `memory.grow`, so a
  memory may move between calls.
- A pointer from `wm_memory_pointer` holds only until frames move: a host
  that keeps one while anything may make frames pins the range. The kernel
  pins a block for a transfer in the background too (today it pins only a
  paged memory, as a block never moved).

### Costs

- The pool is RAM set aside at the start. It is the RAM budget the host
  gives the pager already, now a real region instead of a count.
- Moving a frame is a copy of 4 KiB, between slices or in a host function,
  only when a block is wanted and the pool has no run for it.
- The engine keeps, for each frame of the pool, which memory and index use
  it (two words per frame: 320 B for 40 frames on a 32-bit chip).

### Instead

- **Write out, then collapse:** when the heap has no piece, send the frames
  to storage, free them, ask again and read them back. Costs a write and a
  read of the whole memory each time, and still depends on how the heap
  happens to be cut.
- **Leave it:** blocks only where the heap allows; on an ESP32 that is
  rarely after paging.

### Checks

- Engine, `test/pool.c`: frames of three memories in turn in the pool, and
  one of them still goes into one block, every byte of the three where it
  was; frames pinned all over the pool keep a memory paged until unpinned;
  while a program is in a host function its frames do not move under the
  host's pointer, and between calls they do; a program goes into one block
  between two fuel slices and ends with the same result; a block grows in
  place, or goes to storage and grows paged; freed, the whole pool is one
  block again. Each of the three rules (memory in use, pins, frame table
  pointed at the new place) checked by breaking it: the test fails.
- Kernel: `page_block` with the pool (the sleeper's memory back into one
  block through the pool, no heap); `page_store` with background writes
  (the pool has room for frames on their way out). Every kernel test with
  the pool wherever it sets a RAM budget.
- Mac CoreMark with a pager: paged 2350, one block from the heap 2595, one
  block from the pool 2605.
- ESP32 (SD card store, 24 frames of RAM, programs of one 64 KiB page): the
  sleeper's block went to the card when the worker needed RAM (197 ms) and
  its memory came back as one block once the worker had ended (912 ms),
  both results right. Before the pool it never came back: the heap had no
  64 KiB piece left.
- On an ESP32 without external RAM the pool takes the largest piece of
  byte-addressable RAM: with 24 frames (96 KiB) 64 KiB of it was left, in
  pieces of 16 KiB at most, too small for a value stack of 16 KiB. A board
  sets its budget by what else it runs (16 to 20 frames there), or gives
  the pool external RAM (`OsPageStore.ram` in wasm-rtos).

## 7. An instance and a module keep only the memory they use

**Decided and built (2026-11-15).**

### Problem

An instance made a function object (`WmFunc`, 40 bytes on ESP32) for every
function of its module, imports included, though a linked import calls its
target and never uses its own. The tour program has 23 imports: 920 bytes
of every process went to nothing. The module's arrays (types, imports,
exports, the side table and the rest) grow by doubling while it loads and
stayed that size, a quarter empty on average. On the Uno the test program
could no longer make its instance: 482 bytes were free, and the C library
keeps 128 of them for the stack.

### Decision

- **Function objects for what needs one.** `own_funcs` holds the module's
  own functions, then a stub for each function import nothing was linked
  to (it traps until `dylink.0` links it). A linked import takes no room.
  Code that turned a function into its index by its place in `own_funcs`
  (snapshots, `dylink.0`) goes through `funcs` instead.
- **Arrays trimmed once loaded.** Loading still grows them by doubling,
  which keeps moves and holes few while the count is unknown; when the
  module is loaded each is reallocated to its elements.

### Why

Both are memory every program pays and nothing uses, on every platform.
The heap after loading is what the device has for programs.

### Instead

Counting each section first and allocating exactly: imports of four kinds
share one section, so it needs a second pass over it, and the side table's
size is known only after validation. Trimming once is one place for all.

### Measured

Module after loading, 64-bit build:

| Program | Before | After | Change |
|---|---:|---:|---:|
| tour | 42 568 B | 31 376 B | −26% |
| Arduino sketch test | 44 636 B | 36 220 B | −19% |
| CoreMark | 10 376 B | 6 248 B | −40% |

Every process of the tour program is 920 bytes smaller on an ESP32. The
Uno test program runs again: 305 bytes free after its instance,
`fib(10)` in 31 ms as before; flash +570 bytes.

## 8. The fast loop does what other interpreters do

**Decided and built (2026-11-15).**

### Problem

The fast loop had several costs that mature interpreters avoid, on every
processor:

- A helper for the rare bulk memory instructions took the address of the
  loop's stack pointer (`misc(..., &sp)`). A variable whose address is
  taken is hard to keep in a register: on Xtensa the stack pointer lived in
  memory and every change of it was a store.
- The top of the stack was in a register and in its slot, written twice.
- `call_indirect` compared signatures byte by byte, through a function call
  in the loop; and every function object held a copy of its signature.
- Byte and 16-bit loads and stores, all over C code with `char` and
  `short`, went through one handler for every access with a table of sizes.
- Global access worked out the global's width each time; C programs touch
  `__stack_pointer` in most functions.

### Decision

- **`misc` returns the stack**, as the numeric helpers do.
- **The top of the stack in a register** (`tos`), as in wasm3 (`_r0`) and
  HotSpot's interpreter: its slot is written only when the stack goes to
  memory (`FAST_SPILL`: a call, a helper, a branch that moves values, a
  stop). The stack in memory at those points is laid out as before:
  snapshots and the loop in place are unchanged.
- **Canonical types**, as wasm3 and V8: equal types of a module are one
  (`WmType.canon`, `WM_TYPE`), a function points to its type, and
  `call_indirect` compares pointers; only a host's or another module's
  function needs its signature compared.
- **Handlers of their own** for `i32.load8_s/u`, `i32.load16_s/u`,
  `i32.store8`, `i32.store16`, as for `i32.load` and `i32.store` (wasm3,
  WAMR); and for `global.get`/`global.set` of one-slot globals.

### Why

Each is the usual way and each removes work rather than moving it. Speed
is the same on every chip for the same reason: fewer loads and stores per
instruction.

### Instead

- **Not reloading the instance and memory after a call** within one
  module: a call may grow memory, so the memory must be read again after
  it, as in wasm3; what is left to save is a load or two, the cost of the
  test.
- **Taking `bytes` and `fuel_on` out of the loop's state** to free a
  register on Xtensa: the fuel test then reads the runtime in every
  handler, and the loop grew by 44%.

### Measured

CoreMark:

| Machine | Before | After | Change |
|---|---:|---:|---:|
| ESP32 (GCC -O2) | 12.03 | 15.49 | +28.8% |
| Mac | 2587 | 2988 | +15.5% |
| Mac, built as for chips | 2503 | 3065 | +22% |

The ESP32 has 544 bytes less heap. The first two steps, against the
loop before the decision:

| Steps | ESP32 | Mac | Mac, as for chips |
|---|---:|---:|---:|
| `misc` returns the stack | +6.6% | ±0 | +1.3% |
| and the top of the stack in a register | +6.1% | +7.6% | +14.7% |

So the register cost the ESP32 0.5%: on Xtensa the compiler keeps `tos`
in memory, too few registers. The rest of the decision gave the ESP32
the remaining +21%.

The Black Pill (STM32F411, Cortex-M4, GCC -O2), measured after decisions
8 to 12 together, against the engine before them: CoreMark 5.86 → 7.13
(+21.7%), where wasm3 makes 6.20 on it.

## 9. The instructions are described once

**Decided and built (2026-11-15).**

### Problem

A predecoded record was written in four places: its kind (`WM_F_*`), the
fast loop's table of handlers, the predecoder's choice of record for an
opcode, and its handler. The i32 operations with records of their own were
written up to three times: in the shared `op_i32` the loop in place runs,
in the fast loop's handler, and in the fused handlers that end in a branch.
Forgetting one place was not an error the compiler could see.

### Decision

- **One list, `source/wm_ops.def`**: a line per record in the order of the
  kinds, of five sorts (a record; an i32 access with its type; an i32
  operation with its expression; a fused record; a fused one ending in a
  branch, only with `WM_FUSE`). The kinds, the handler table and the
  predecoder's choice are made from it by defining the sorts needed and
  including it, the usual way of a C list of instructions.
- **Each i32 operation's expression once**, as an always-inlined function
  (`i32_add(a, b)`) made from its line: `op_i32`, the fast handlers and the
  fused compares call it.
- **Handlers stay written by hand**: calls, branches, accesses and the
  fused runs each do their own work with the stack and the operands.

### Why

A list of instructions in one file is how interpreters in C keep their
tables in step, and it needs nothing but the preprocessor: no tool in the
build, nothing generated to keep in the repository for the boards' builds.

### Instead

- **Generating both loops from one description** of every instruction's
  body: worth it for hundreds of instructions and several tiers; here about
  eighty records, and a generator would have to know two stacks (the top
  in a register in one loop, in memory in the other). A tool in the build
  and generated files would cost more than they save.
- **The list as a macro taking a macro** instead of a file included: no
  `#if` inside it, and records only built with `WM_FUSE` need one.
- **Sign extension back to shifts**, for the 0.6% the ESP32 lost with the
  analyzer's change: +0.9% there, but −2.4% on a Mac
  ([experiments](experiments.md#sign-extension-shifts-or-a-switch)).

### Measured

| | Before | After | Change |
|---|---:|---:|---:|
| ESP32 CoreMark, GCC -O2 | 15.40 | 15.42 | ±0 |
| ESP32 CoreMark, GCC -Os | 13.65 | 13.77 | +0.9% |
| Mac CoreMark | 2833 | 2865 | ±0 (noise) |
| Mac CoreMark, built as for chips | 2978 | 3011 | ±0 (noise) |
| AVR flash | 35 584 B | 35 588 B | +4 B |

The fast loop's machine code is the same on a Mac (clang -O2). On Xtensa
(GCC 14.2 -O2) `run_fast` is 3 instructions and 3 bytes of padding
shorter and `op_i32` 6 instructions shorter; the rest moved. CoreMark ran
the same 41 963 863 173 instructions; the ESP32's heap is the same.

## 10. The engine is fuzzed, and against itself

**Decided and built (2026-11-15).**

### Problem

Every byte a program, a page store or an SD card hands the engine comes
from outside it: a module to load and run, a snapshot to go on from. The
spec suite checks what the standard says a module does; nothing checks
what the engine does with bytes nobody wrote on purpose. And with two
loops, fused records, the top of the stack in a register and snapshots,
a wrong answer can come without a crash, where no sanitizer sees it.

### Decision

Four libFuzzer targets in `test/fuzz/`, built with the address sanitizer
(and the undefined behaviour one where it does not stop predecoding), as
wasm3 and WAMR are fuzzed in OSS-Fuzz:

- **`fuzz_load`**: any bytes through `wm_module_load`.
- **`fuzz_run`**: a module loaded, instantiated with every import made for
  it (functions that return zeros, memories, tables and globals of the
  types it asks for), its start function and every exported function run
  with a budget of fuel.
- **`fuzz_diff`**: the same module run three ways, which must agree on
  every result, trap, fuel spent, global and byte of memory: predecoded
  in one slice of fuel; in place; predecoded in small slices with a
  snapshot written and read back at the first stop. This is the engine
  fuzzed against itself, as wasmtime fuzzes engines against each other.
- **`fuzz_snapshot`**: any bytes through `wm_snapshot_read` into a known
  program stopped in the middle, which then goes on.

No allocation over 16 MiB succeeds while fuzzing, so a module that asks
for 4 GiB of memory meets a failed allocation, as on a board, and not the
fuzzer's limit. `fuzz_run` and `fuzz_diff` are also built as for chips
(fused runs) and with every memory paged in small frames.

Inputs start from the spec suite's modules and the engine's own test
programs; the fuzzer mutates them from there. `tools/fuzz.sh` runs a
target locally; CI runs each for a minute on every change. Every bug found
is fixed with its input kept in `test/fuzz/regress/`, which `check.sh`
runs every time.

### Why

Mature engines are fuzzed continuously; most of their memory bugs are
found that way. Two loops that must agree are a free oracle: a difference
is a bug in one of them, found without knowing the right answer.

### Instead

- **Generating valid modules** (`wasm-smith`): reaches deeper into the
  interpreter than mutated bytes, but needs Rust in the build. Later, if
  mutation stops finding anything.
- **OSS-Fuzz now**: it runs fuzzers around the clock for free, but takes
  an application and a project people use. Later.

### Found

| Target | What | Now |
|---|---|---|
| `fuzz_load` | a type's count of values past the section's end: counted anyway, read past the module | fixed |
| `fuzz_run` | an export name longer than what was left: compared at its length | fixed |
| `fuzz_diff` | the fuel left after a trap differs between the loops: neither writes it back there | not defined (`wm.h`); compared only without a trap |
| `fuzz_snapshot` | a frame between instructions passed, and the loop in place read past its arrays | [decision 11](#11-a-snapshot-is-checked-as-a-module-is) |

### Measured

Each target for 2 to 5 minutes on a Mac, in every build, with nothing
left: `fuzz_load` 6.2 million inputs, `fuzz_run` 11 million, `fuzz_diff`
5.1 million (as for chips 2.2, paged 1.6), `fuzz_snapshot` 255 000. The
corpora reach 97% of the fast loop's lines (`fuzz_run`), 85% of the loop
in place and 76% of the snapshot code (`fuzz_diff`), 94% of the loader.

## 11. A snapshot is checked as a module is

**Decided and built (2026-11-15).**

### Problem

Fuzzing snapshots (decision 10) found two things. A snapshot's positions
were checked only roughly: an offset inside its function's body, a side
table index no larger than the table, frames inside the stack. One that
passed could stand between two instructions, with a side table entry of
another place or a stack of the wrong height, and the loop in place then
read past its arrays. And a reference on the value stack was written as
the host address it is: read into another instance of the module (a
program swapped out and back, or moved to another device) it pointed at
the old instance's function, freed by then.

### Decision

- **Each frame is checked against its function**, where the module is
  validated (`WM_HAS_VALIDATION`): the function's body is validated again
  up to the frame's position, which gives what the stack holds there. The
  position must be the start of a reachable instruction, its side table
  index the one there, and its stack exactly as high as validation says.
  A caller's position must follow a call of the function above it (or of
  a function of the type a `call_indirect` names), and that function's
  frame start where the call's arguments are. The entry function's frame
  starts at the call's base. All of it before memory, globals or tables
  are touched: a snapshot that does not fit changes nothing of them.
- **References on the stack are written as references**, as globals and
  tables already are: a function as its index, in every build; the types
  of the stack's slots come from the same validation, run only for
  modules that can have references there (C programs cannot).
- **A CRC-32 of the whole stream** at its end, against a card or a cable
  that changed a byte; the snapshot format's version goes to 2. A version
  1 snapshot does not load.
- Without `WM_HAS_VALIDATION` (the Uno, trusted builds) a snapshot is
  trusted as the module is: no checks beyond the CRC, no code for them.

### Why

A snapshot is a program's state from outside the engine, as a module is
its code: an SD card, a page store, another device. The loop runs both
without checking what validation checked once. The checks are made when
a snapshot is read, never while the program runs: the loops are the same.

### Instead

- **Trusting snapshots, with a CRC only**, as checkpoint tools for
  processes do: a corrupted card is caught, a snapshot made to break the
  engine is not, and moving programs between devices needs exactly that.
- **Keeping the stack's types in the snapshot**: larger snapshots, and
  still to be checked against the code.

### Measured

| | Before | After |
|---|---:|---:|
| ESP32 flash, loader and snapshots (GCC -O2) | 19 420 B | 22 371 B (+2 951) |
| Black Pill flash, the same (GCC -O2) | 18 000 B | 20 040 B (+2 040) |
| Uno test program flash | 35 604 B | 35 512 B (snapshots not linked) |
| A funcref on the stack, read into a fresh instance | use after free | the fresh instance's function |

`test/refs.c` checks references and that a snapshot changed in a byte, a
position, a side table index or a frame is refused, in both loops;
`fuzz_snapshot` ran 5 minutes on the checks with the CRC skipped, and
`fuzz_diff` writes and reads a snapshot in every run.

## 12. What runs is counted, and speed is guarded by counts

**Decided and built (2026-11-16).**

### Problem

What to make faster was chosen from what the code contains, not from what
runs: `local.get` is a quarter of CoreMark's instructions, but how often
each record and each pair of records runs was not known. And speed was
measured by hand, on boards and a Mac, before a pull request: a change that
cost a few percent elsewhere could get in unseen.

### Decision

- **`WM_PROFILE`**, a build flag off by default (no code without it): the
  fast loop counts each record by kind and each pair in a row, the loop in
  place each opcode; at exit the counts go to a file, and
  `tools/profile.py` adds files up and prints them. As wasm3 counts its
  operations (`d_m3EnableOpProfiling`) and CPython its instructions
  (`--enable-pystats`).
- **Speed in CI by instructions, not time**: a short CoreMark (a clock
  that moves 5 s a call, so the same run each time) under cachegrind;
  machine instructions per WebAssembly instruction, plain and as for chips,
  against the pull request's base built by the same compiler, as SQLite
  (speedtest1) and rustc-perf measure. The AVR test program's flash the
  same way. Over 2% worse fails, unless the pull request has the label
  `slower-ok`.
- CoreMark comes from its sources and a bare port at pinned commits
  (`tools/coremark.sh`), built for WebAssembly 2.0: nothing of it is kept
  in the repository.

### Why

Time on a shared runner moves by ±10%; counts of instructions move by
about 0.1% and say the same thing about the loop's work. Comparing with the
base in the same job needs no stored numbers that would differ by compiler.

### Instead

- **Time in CI**: noise larger than the changes to catch.
- **A stored baseline of counts**: it depends on the runner's compiler
  version, and goes stale with each upgrade.
- **The boards in CI**: they stay on a desk; their numbers are measured by
  hand before a change that touches the loops, as before.

### Measured

CoreMark as for chips with `WM_PROFILE`: 20.8 million records ran for
32.9 million instructions, 41.8% of them fused. The commonest pairs of
plain records: `local_tee1 → const16` 2.4%, `const16 → shr_u` 1.8%,
`i32_store → local_get1` 1.5%. Without the flag the machine code of every
build is the same as before (Mac clang, Xtensa and ARM GCC).


## 13. Hot code is written for people, and checked by its machine code

**Decided (2026-11-16).**

### Problem

The loops are where speed lives, so they collect tricks: the top of the
stack in a register, fused runs, handlers generated from lists. Each one
was measured, and each one makes the code harder to read. More are coming:
fused records chosen by profile, and perhaps a loop with operands inside
its records, a register machine, as wasm3 is (its operations name their
operands as slots, the top of the stack in `_r0`). Without rules the loops
would end up fast and unreadable.

### Decision

- **The loop in place is the reference**: its cases, one per
  instruction, are the code a person reads to learn what an instruction
  does. Its short paths (built where labels are values) and the fast
  loops may be clever; `fuzz_diff` keeps them doing what the cases do.
- **A change for readability keeps the machine code**: the same bytes on a
  Mac and on Xtensa (the hash of the objects), or else it is measured on
  the boards like any change of speed.
- **Functions where the compiler makes the same code**: `static inline`
  with `always_inline` is checked by types and reads as C; macros only
  where C has nothing else (labels, `goto`, handlers made from a list),
  and one level of them.
- **Every trick has a name and one place**: a macro or function, a comment
  saying why, the decision it comes from.
- **One description, many uses** (decision 9): what can be made from
  `wm_ops.def` is made from it, fused records included.
- **Files by what they are**: predecoder, loop, fused runs, profile; each
  begins with the format of what it works on.
- **Fast loops as variants**: a loop with operands in its records would
  be a fast loop of its own, in files of its own, chosen by a macro
  (`WM_FAST_LOOP`), as WAMR chooses its classic or fast interpreter; both
  share `wm_ops.def`, the operations, memory access and the code cache,
  and `fuzz_diff` compares all of them. Whether to build one is a decision
  of its own: it must still count fuel by instruction and stop at any of
  them (decisions 1 and 2), and its records then hold several.

### Why

Speed and clarity part ways only where nothing checks the clever code
against the plain one. With a reference loop, a fuzzer that compares them
and a hash that proves a rewrite changed nothing, the fast code can be as
clever as it needs and still be learned from the plain one.

### Instead

- **One loop only, as fast as it can be**: nothing simple to read or to
  compare against.
- **A generator of the loops**: see decision 9.

## 14. Operands in records, as overlays on them

**Decided and built (2026-11-16).**

**Replaced by decision 16 (2026-11-21).** The register loop names its
operands directly and leaves out the moves without forms; the forms and
fused records went with the stack loop. What follows is how they were.

### Problem

After decision 13's fused records, 39% of what CoreMark runs (as for
chips) is moving values: `local.get`, `local.set`, `local.tee`,
constants. A register machine names its operands in its instructions
(wasm3: slots and `_r0`) and does no moving of its own; but its
instructions are several WebAssembly instructions each, which breaks
fuel by the instruction and stopping at any of them (decisions 1 and 2).

### Decision

- **Forms of an i32 instruction**: its operands from the records before
  it, a local or a constant, and its result into a `local.set` after it.
  A form is a fused record: the handler of the first record of the
  sequence, the records after it kept as they are. So fuel stays exact
  (the form runs only with fuel for the whole sequence, else only the
  first instruction), and between dispatches the stack is what the stack
  machine has: snapshots, their format and moving them between builds
  and chips are as before.
- **Named by the sequence**: `get_get_add` is `local.get a; local.get b;
  i32.add`, `get_const_add_set` adds a constant into a local. The forms
  fused so far (`get_get_add`, `get_const_add`, `const_add`, `add_set`,
  …) are forms of this one rule.
- **Shapes**, each one macro: `GET_GET`, `GET_CONST`, `CONST`, `GET`
  (operand B a local), `SET` (the result into a local), and the
  three-address `GET_GET_SET` and `GET_CONST_SET`. A form is a line of
  `wm_ops.def` naming itself, its shape and its instruction; its handler
  and its pattern for the predecoder come from that line. The predecoder
  gives a record the longest form that matches.
- **Only forms the profile asks for**: a form that runs too seldom costs
  room in the loop, and on ESP32 the loop runs from flash through its
  cache. Forms with traps (division, memory) are not made; a memory
  access in a form would leave it to the plain records when not in one
  block, as `get_load` does.

### Why

The gain of a register machine is in doing the moving inside the
operation. As overlays, the forms get that gain and keep everything the
engine is for: exact fuel, stops anywhere, portable snapshots, the loop in
place as the reference, Uno untouched.

### Instead

- **Register code of its own, with maps back to the stack machine**
  (as V8's and HotSpot's interpreters deoptimize): fuel and snapshots
  kept, but two copies of every function's code in RAM and maps for every
  stop.
- **Register code as wasm3 has it**: no way back to an instruction;
  breaks decisions 1 and 2.
- **Every shape of every instruction** (wasm3 has some 500 operations):
  the loop too large for a cache of flash.

### Costs

A value cannot stay in a processor register from one form to the next
(only `tos` does); a deep expression is fused in parts, as forms are
straight sequences from their first record. Each form is room in the
loop.

### Measured

Of 63 forms tried (nine instructions in seven shapes), 18 run 0.1% or
more of the records of CoreMark or of the kernel's test programs; the
others were left out.

| | Before (decision 13) | After |
|---|---:|---:|
| CoreMark records run, as for chips | 19.5 million | 18.1 million (−7.2%), 55% fused |
| CoreMark records run, plain | 23.4 million | 22.0 million (−6.0%) |
| ESP32 CoreMark, -O2 | 15.86 | 16.44 (+3.7%) |
| ESP32 CoreMark, -Os | 13.88 | 14.09 (+1.5%) |
| Black Pill CoreMark | 7.40 | 8.05 (+8.8%) |
| Mac CoreMark, as for chips | 3091 | 3154 (+2.0%) |
| Mac CoreMark, plain | 2985 | 2968 (within noise) |

The risk, a loop too large for ESP32's cache of flash, measured with the
kernel's board checks against main (before decision 13): 19 of 19 pass,
CoreMark in the system 14.75 → 15.86, 500 round trips through a queue
97 → 56 ms, `fib(24)` 231 → 233 ms, 19 programs at once in both. The
queue's gain is more than the forms explain (it runs little of them):
the layout of code in flash, most likely.

Found on the way: a record that writes a local without touching the stack
must write `tos` too when the local is the slot under an empty stack
(`FAST_SET_LOCAL`); the spec suite hung on a loop that showed it.

**Later (2026-11-20).** Eleven more fused records by the same rule, for
moves and what takes them next (experiments.md): records run −12.4%,
ESP32 +7.7%, Black Pill +11.0%, Mac +7.5%; board checks 19 of 19.

## 15. Records in storage the processor reads, RAM for the hot ones

**Decided and built (2026-11-20).**

### Problem

A program's records take about twice its bytes again in RAM: CoreMark is
7.8 KB of `.wasm` and 16.2 KB of records. The code cache (decision 3)
keeps only what runs, but what it drops is made again at the next call:
the function is validated again for its side table entries and
predecoded, and while there is no room the module runs in place, two to
three times slower. Every start of a program predecodes every function
once to learn whether it can (277 µs for CoreMark on a Mac). Most boards
have far more flash than RAM (Black Pill 512 KB to 128 KB), and some have
PSRAM: both are read by the processor as memory.

### Decision

- **A code image**: every function's records of a module, written once
  (`wm_code_image_write`, through a write callback as snapshots are), in
  pieces, one function in RAM at a time. A header holds a format version,
  the build's tag, the module's CRC-32 and an index of functions (offset
  and cells; none for a function that runs in place).
- **The build's tag**: records hold handlers as offsets from the first
  one, branches as distances, and no pointers, so an image is valid
  anywhere in memory but only for the build that wrote it. The tag is a
  CRC-32 of the handlers' offsets, the predecoding flags and the format;
  an image of another build is refused, and the module goes on as today
  (the host writes a new image), as Android remakes its `.oat` files
  after an update of the runtime. The image is the records, then where
  each function starts, then a trailer with the header's fields and the
  CRC, so it is written in one pass, as a ZIP file's directory comes
  last.
- **Mapped** (`wm_code_image_map`): the host gives the image where the
  processor reads it, internal flash or PSRAM. A function without records
  in RAM runs from the image: no stop, no allocation, nothing made
  again. While the cache has room a function called again is copied to
  RAM (the hot ones, as ESP-IDF puts hot code in IRAM and AXFS copies hot
  pages); CLOCK drops the copy back to the image, never to nothing. The
  fast loop is the same: it reads records through `code->fast`, wherever
  that points (its machine code is unchanged, by sha, on a Mac and on
  Xtensa). A function that found no RAM even after CLOCK dropped others
  stays in the image until the host trims the cache
  (`wm_code_cache_trim`): letting the hand send such functions back to be
  copied in made them stop the loop again and again when RAM was short
  (ESP32 15.88 against 16.02).
- **Checked as a snapshot is** (decision 11): the tag and the CRC always;
  with `WM_HAS_VALIDATION` each function's records are made once more at
  `wm_code_image_map` and compared, one function in RAM at a time, so a
  broken or changed image is refused before anything runs from it. A
  record is a jump into the loop: it is never trusted unchecked.
- **Several places, by priority, are the host's**: the engine takes one
  image a module; which storage holds it, by priority as frames' stores
  are (kernel decision 10), is the kernel's.

### Why

- On the boards, records from flash cost little and RAM costs a lot:
  CoreMark −3.1% on an ESP32, −5.7% on a Black Pill, with the 16.2 KB of
  records out of RAM. A function dropped from RAM costs a few percent, not
  a factor of two or three.
- A program starts and a dropped function comes back without validating
  and predecoding again.
- Nothing changes in the loops, the records, fuel or snapshots.

### Instead

- **Records always in flash, never copied**: the most RAM saved, but the
  3–6% lost even when RAM has room for them (decision 4 keeps memory in
  one block while RAM has room for the same reason).
- **Images on storage the processor cannot read (an SD card)**, records
  read into RAM at a miss: cheaper than validating and predecoding again,
  but the module's bytes are still in RAM for loading, and checking each
  piece read costs what making it costs. Programs' bytes on such storage
  are a decision of their own, with the loader.
- **Trusting images with a CRC only**: what decision 11 rejected for
  snapshots; a record holds the handler it jumps to.
- **Images made on a computer** (as WAMR's AOT files): only for the build
  that will run them, which the computer would have to know; the device
  that runs them makes them.

### Checks

- `test/image.c`: calls from the image and copied in, through a host
  function that empties the cache, in slices of fuel through snapshots,
  and a snapshot taken running from the image gone on in an instance
  without it; an image misaligned, cut, of another module or build, and
  every word changed with its CRC made again, all refused (with
  validation). Built plain, as for chips, without validation and without
  predecoding.
- The spec suite with every module's image mapped (`WM_SPEC_IMAGE`):
  every function from the image, and copied in through a cache of one
  byte. 1243 modules, all mapped.
- Fuzzing: `fuzz_image` maps the fuzzer's bytes as the image of the
  snapshot program and runs it (CRC not checked there): plain, as for
  chips and paged, 16 million runs, nothing found.
- `test/image.c` also found that the CLOCK hand, moving to the next
  module, kept its place in the last one: modules after the first with no
  more functions were never visited, their records never dropped. Fixed.

### Measured

CoreMark, 16 220 bytes of records; on the ESP32 (-O2) the image is in an
app partition mapped as data, on the Black Pill in its last flash sector:

| | ESP32 | Black Pill | Records in RAM |
|---|---:|---:|---:|
| In RAM, no image | 16.55 | 8.08 | 16 220 B |
| From the image, no RAM for records | 16.02 (−3.2%) | 7.61 (−5.8%) | 0 |
| The image, copied into RAM | 16.55 | 8.07 | 16 220 B |
| A code cache of one byte, no image | 8.55 (−48%) | 4.46 (−45%) | 4 456 B |
| A code cache of one byte, the image | 16.28 (−1.6%) | 7.94 (−1.7%) | 4 456 B |

Starting the program (map and instantiate) on the ESP32: 32 ms without an
image (instantiating predecodes every function once); with an image 36 ms
with validation (every function made again and compared, and the CRCs),
4 ms without. On the Black Pill 68 ms without an image, 75 ms with one,
validated. Writing the image to flash: 128 ms on the ESP32, 401 ms on the
Black Pill (a byte at a time). The Uno's flash is the same
(35 512 bytes): predecoding is not built there.

## 16. Operands in slots fixed by depth, in records that overlay the stack's

**Decided and built (2026-11-21).** It replaces the stack loop and its forms (decision 14).

### Problem

After the forms (decision 14) and the moves fused with what takes them
next (experiments.md, 2026-11-20), CoreMark as for chips runs 15.9
million records for 32.9 million instructions, 2.07 a dispatch. About
31% of the records still compute nothing:

| Records that only move values or mark blocks | Share of records |
|---|---:|
| `local.get`, `local.tee`, `local.set`, constants, alone | 12.2% |
| `nop` runs (`block`, `loop`, `end`, `nop`) | 7.4% |
| Fused moves: `set_get`, `get_get`, `set_get_get`, `get_tee`, … | 11.6% |

Each fused record by the profile is a handler more in a loop that runs
from ESP32's cache of flash, and each takes one sequence only. The
operand stack itself costs on every record: `sp` moves, and `tos` is
written back whenever a record needs the slot under it.

A register machine does no moving of its own: Shi, Casey, Ertl and Gregg
(*Virtual machine showdown: stack versus registers*, 2005/2008) translated
JVM stack code to registers and ran 47% fewer instructions in 32% less
time; WAMR's fast interpreter gives operands as offsets of slots in the
frame and leaves out most `local.get`s; wasm3 names slots and `_r0`.

### Decision

- **The stack in slots fixed by its depth.** Validation knows the
  operand stack's height at every instruction; the predecoder gives each
  value the slot of its depth (after the locals), so the record of
  `i32.add` at depth 3 reads slots 1 and 2 and writes slot 1. No `sp` at
  run time. At every record boundary the frame's slots are exactly what
  the stack machine's stack holds there, so snapshots, their format,
  stopping anywhere and moving between builds stay as they are (decision
  2 and 11): what changes is how a record finds its operands, not where
  they are.
- **Copy propagation over the overlays.** As forms are (decision 14), a
  register record is the first record of a sequence, the records after it
  kept as they are. An operand that is a local or a constant is named
  directly (`L3`, `#5`), not copied to a slot first; the `local.get`,
  `local.set` and constants it absorbed are overlaid, still there for a
  stop or a short budget of fuel (then only the first record runs, as
  now). Fuel stays exact: a sequence runs only with fuel for all of it.
- **Operand kinds by shape, not by every combination.** A handler per
  operation and shape (`slot, slot`, `slot, local`, `local, const`, …) as
  wasm3 has them; the shapes made are the ones the profile asks for
  (decision 13), the others reading operands through one general
  decoder. Markers (`nop` runs) with no stack effect are skipped by the
  record before them, which carries the fuel they cost.
- **A loop of its own.** The register loop is a variant of the fast loop
  chosen by `WM_FAST_LOOP` (decision 13); the loop in place stays the
  reference and the stack loop stays for builds and boards where it wins.

### Why

- Fuel, stops at any instruction and portable snapshots are what the
  engine is for; with slots fixed by depth they cost nothing, because the
  frame at a record boundary is the stack machine's state, not a map of
  it (decision 14 rejected maps).
- The moves and the stack pointer are the work left that every processor
  sees. The fused moves took 12.4% of the records for +7.7% on the ESP32;
  half of the 31% left would be about +10% there, the bar below.
- Records grow (operands in each), but decision 15 lets a board keep them
  in flash, and RAM holds the hot ones.

### Instead

- **More fused records by the profile**: done first (2026-11-20), the
  baseline this decision must beat: eleven records took 12.4% of the
  records, ESP32 +7.7%, Black Pill +11.0%. What is left is spread over
  more sequences, each a handler more.
- **Register code with maps back to the stack machine** (V8, HotSpot):
  rejected in decision 14; two copies of every function and maps for
  every stop.
- **Stack caching in more registers** (Ertl's dynamic stack caching, a
  second `tos`): each handler in several states, the loop several times
  larger; on Xtensa `tos` alone cost 0.5%.

### Risks

- The loop larger for ESP32's cache of flash: the kernel's board checks
  (a queue of 500 exchanges) are the guard, as for decision 14.
- Records with operands are larger: RAM for the code cache up, until a
  board maps code images.
- `tos` goes: on the Mac it gave 7.6%; slots fixed by depth must make that
  up first.

### How it is decided

An experiment first, kept out of `main` until it wins, against the build
with the fused moves (ESP32 17.83, Black Pill 8.96, Mac as for chips
3342): the register loop
for i32 arithmetic, comparisons, branches, loads and stores, locals and
constants, every other instruction through the stack records. Kept only
if CoreMark gains at least 10% on the ESP32 and the Black Pill, loses on
no machine (the Mac too), the board checks pass, and the Uno does not
change. Either way it goes into experiments.md with its numbers.

### Measured: the prototype

A prototype (experiments.md, *A register loop, as a prototype*) is above
the bar on every machine: CoreMark +16.2% on a Mac, +17.3% on an ESP32
(-Os +35.8%), +12.4% on a Black Pill, against the fused moves; records
6% smaller. It is an upper bound: fuel one a record, no snapshots,
memory in one block only.

### What the exact version needs

- **Fuel by the instruction, charged by the segment.** A segment runs
  from a label, a call or a branch to the next; the translator knows how
  many instructions it holds, and its first record takes them all, as
  fuel metering by basic blocks does (wasm-instrument's gas, wasmtime's
  fuel at block heads). With too little fuel for the whole segment the
  call goes on in the loop in place from the segment's first instruction,
  where the frame is the stack machine's, and stops exactly where the
  fuel ends. One check a segment instead of one a record.
- **Stops only where the frame is the stack machine's.** Fuel (in place,
  above), a yield after a host function and a call into a function
  without records come at calls or labels, which flush every pending
  value. So snapshots stay as they are, taken there.
- **Paged memory.** A TLB miss inside a segment has values pending:
  either each access record keeps the list of what is pending there (a
  small deopt map, as V8 keeps for its stops), or a paged memory runs on
  the stack loop. The second needs both loops in the firmware and code
  dropped when a memory leaves its block.
- **Positions of traps** for traces: the translator run again for the
  function that trapped, to find the instruction of the record.

### Measured: built

Against the stack loop with the fused moves (main before it), the same
CoreMark:

| | Stack loop | Register loop |
|---|---:|---:|
| ESP32 -O2 | 17.83 | 23.72 (+33%) |
| ESP32 -Os | 15.40 | 20.22 (+31%) |
| Black Pill | 8.96 | 11.49 (+28%) |
| Mac, no fuel | 3390 | 3882 (+14.5%) |
| Mac, slices of 10 000 (the kernel's) | 3337 | 3638 (+9.0%) |
| CoreMark's records | 16 220 B | 17 028 B |
| The loop's machine code, Xtensa -O2 | 11 772 B | 6 608 B |
| Uno flash | 35 512 B | 35 512 B |

The kernel's board checks on the ESP32, 19 of 19: CoreMark in the system
17.51 → 22.10, `fib(24)` 225 → 158 ms, 500 round trips through a queue
50 → 50 ms, programs at once 19 → 18. The one program fewer: about 600
bytes more a program, of records (a call and a branch hold their top and
targets) and of tails; the next step is records of branches without the
cells they do not need.

Fuel: CoreMark counts the same 32 876 269 instructions as the stack loop in
slices of 7, 1000 and 10 000. The spec suite in every build (paged and
swapped too), the wasm-rtos kernel's 86 tests (paged too), snapshots
between builds and `test/fuel.c` pass.

### Checks

The ones every loop has: the spec suite in every build, `test/fuel.c`
(fuel exact at every instruction), snapshots between builds and loops
(taken in the register loop, read back in place), `fuzz_diff` comparing
the register loop with the loop in place, the kernel's tests.

## 17. Frames written ahead of need, between runs

**Decided and built (2026-11-21).**

### Problem

When RAM is short, the kernel takes the block of a task that sleeps out
of RAM whole (`wm_memory_block_out`): every frame of it written there and
then, while the task that needs the room waits, and with it the whole
system. A memory of two pages is 32 frames: 13 ms through a compressed
store on an ESP32 (421 µs a frame), and tens of milliseconds to an SD
card. The time to write them was there before, while nothing ran.

### Decision

- **`wm_memory_clean(memory, &written)`** writes one more frame to the
  pager's storage and keeps it in RAM, for the host to call between runs,
  one frame at a time, as Linux writes dirty pages back ahead of need:
  - for a memory in one block, the next frame not copied since the memory
    last could change, the copies made in order from the first frame;
  - for a paged memory, the next dirty frame, which is clean then (out of
    the TLB, so the next store marks it dirty again);
  - a frame all zero is not written; its slot goes (decision of 2026-11-21
    on frames of zeros).
- **A memory in one block keeps no marks of its own stores**, so it counts
  what may change it, `changes`: a call into its instance (`wm_call`), a
  call from one instance into another's function (in both loops), a run
  resumed (every instance with a frame on the stack), a host write
  (`wm_memory_write`, `fill`, `copy`, `pointer`), growing, a snapshot
  restored into it, going back into one block. The copies hold while
  `changes` is what it was when they were made; then `wm_memory_block_out`
  writes only the frames past them, often none.
- `wm_memory_data`'s pointer is for reading: a host that writes a memory
  with a pager goes through the functions that count.

### Why

Linux's flusher threads and kswapd write dirty pages before the memory
is needed, so that reclaim finds clean pages it can drop at once; macOS
and Windows (the modified page writer) do the same. Here the kernel knows
when a task will not run for a while, which no counter of touches can
tell, and it has idle time between the tasks' wakings.

Counting changes instead of marking frames keeps a memory in one block at
full speed: one compare more per call between functions (two loads of
instances already at hand), nothing per access.

### Checks

`test/clean.c`: a block copied ahead leaves RAM with no write and reads
back as it was; half copied, only the rest is written; after a run of its
code, its code called from another instance, a host write, growing it, a
host pointer into it, or a run resumed after the copies were made, it is
written again and reads back as changed. Taking out any one of the counts
in a call, a call between instances (each loop) or a resume makes its
check fail. A paged memory's dirty frames come out clean once each, leave
RAM with no write, and a store marks one dirty again. Frames of 256 B,
4 KiB and 64 KiB, and the loop in place.

## 18. Records in an area of their own, moved together when it has holes

**Proposed (2026-11-22).**

### Problem

On a Black Pill (STM32F411, 128 KiB of RAM) the system checks' CoreMark
ends with "exhaustion: out of memory" while the board has room for it.
Measured with the allocator wrapped (the board's `malloc`, `realloc` and
`calloc` reporting what they refuse):

| | bytes |
|---|---:|
| heap at the start of the checks | 110 592 |
| free after CoreMark is loaded (memory 64 KiB, stack 8 KiB, module, kernel) | 31 692 |
| CoreMark's records, all of them | about 15 300 |
| refused: `realloc` to 4 096 for a function's records | |
| free at that moment, in pieces | 9 896 |

The memory is there, in holes. Every allocation of the engine shares one
heap: the program's memory (one block of 64 KiB), its stack, the module,
the records of each function, a function's tail (what the loop in place
needs inside a segment), and the translator's work: its state, its
arrays, and the records it grows by doubling (`realloc` 1, 2, 4 KiB).
Records live short and come and go in any order (the code cache drops
them by CLOCK and makes them again), between blocks that live long, so
the heap ends in pieces none of which takes the next function. Dropping
more code does not help: a dropped function leaves a hole where it was.

Before the register loop the same CoreMark ran on this board (2026-11-12);
its translation now takes more at once (decision 16).

### Decision

**1. An area of the engine's own for records** (`wm_code.c`).

- **Chunks.** The area is a list of chunks, each from the code allocator
  (`wm_set_code_allocator`, or the engine's), with a header of 16 bytes
  (24 with 64-bit pointers): next, size, top, live. Records of one
  function lie in one chunk. A new chunk takes what the request needs,
  at least half of what the area holds already and at least 256 bytes:
  chunks stay few, as a vector grows, and the area does not become small
  pieces among the heap's again (a chunk cut to each function would).
- **Only what is needed stays.** The unused top of a chunk is given back
  (`realloc` down) once it is more than a quarter of the area and more
  than 256 bytes, and all of it at once when the heap is short (memory
  before code, below): room the area holds back never keeps memory from
  anything else.
- **A function's records are a span**: its `WmCode` (`fast`, `own`,
  `fast_cells`) already says where and how long; no header per span.
- **Allocation is a bump** at a chunk's top: the last chunk first, then
  any with room at its top.
- **Dropping a function leaves a hole**, counted per chunk (`dead`).
- **Compaction** when a request finds no top with room but the area has
  holes enough: the spans of each chunk slide down over the holes
  (`memmove`), in address order (the codes of all modules sorted by
  `fast`), and each moved function's `fast` and `own` are set to its new
  place. A function pinned (on a stack: `pin_stacks`, and the one being
  made) does not move; spans after it slide up to it. Spans may also move
  into an earlier chunk with room, so the last ones empty.
- **Empty chunks go back** to the allocator at once; a chunk is cut to its
  top (`realloc` down) only when it has no pinned span: an allocator may answer a smaller size with a new address
  (the kernel's is its own), and then every span of the chunk is moved
  with it, which only unpinned spans may be.
- **Records need nothing changed when they move**: handlers are offsets
  from the first, branches relative, calls by function index
  (`inst->funcs`), so a function runs wherever its records are (a code
  image already runs them from flash, decision 15).

What points into records and how each stays right:

| Holder | Kept right by |
|---|---|
| `WmCode.fast`, `own` | compaction sets them |
| a frame's return (`WmFrame.pc`) | its function is pinned (on the stack) |
| a stopped run (`rt->saved.pc`) | `saved.func` pinned while suspended |
| the function calling a host (`rt->caller`) and the nested callers | pinned while `rt->nesting` |
| the loop's own `ip` | only inside a run, whose function is on the stack |
| a code image (`mapped`) | not in the area: never moves |
| tails (`WmTailEntry`) | cell indices, not addresses |
| the profile (`WM_PROFILE`) | counts by record kind |
| writing or checking a code image (`wm_code_image_write`, `_verify`) | reads a function's records right after making them, with no request to the area between |

- **Pinning counts.** A function's `pinned` becomes a count: the area
  compacts from inside code that pinned the stacks already (making a
  function, giving the heap room), and an inner unpin must not unpin the
  outer.
- **A mode that moves always** (`WM_CODE_MOVE_ALWAYS`, tests only): every
  request to the area moves every unpinned span to a new chunk and fills
  the old place with a pattern, so a pointer into records kept anywhere
  they may move fails at once under AddressSanitizer, not on a board a
  week later (the classic Mac OS's bug of dangling pointers into blocks
  that moved).

**2. Translation straight into the area, at a chunk's top.** The
translator writes cells at the top of the chunk with the most room; when
that runs out it asks the area for more, which may compact (its own
growing span moves too: the translator takes its `cells` back from the
area after every request) or take a new chunk of twice what it has
written (one copy). At the end the span is cut to its count. No `realloc`
chain in the shared heap, and no second pass.

**3. Tails** (already measured on the board, part of this work): a
translation only for a tail keeps the last 256 cells in a ring (1 KiB;
it reads back no more than the last record, its cells are let go), and
making a tail drops other code and tries again when the heap refuses it,
as records do.

**4. Memory before code.** `wm_code_drop_one`, which the memory calls
when the heap refuses a frame or a block, gives the heap memory back now,
not a hole in the area: it drops by CLOCK until a chunk empties (or the
last chunk can be cut by what was asked), compacts, and frees it. 1 when
the heap got something back, 0 when nothing could go.

**5. Accounting stays**: `wm_code_cache_bytes` counts the records as now;
`wm_code_area_bytes` (new) the chunks, holes and tops included;
`wm_code_cache_limit` limits the records.

### Why

- Records are what an engine can move cheaply: no pointers inside, one
  owner, the code on the stacks known. This is how mature code caches
  keep their memory whole: V8 compacts its code space (with relocation
  information, which records here do not need), HotSpot keeps compiled
  code in a heap of its own and flushes the cold; the Memory Manager of
  the classic Mac OS compacted relocatable blocks behind handles, and
  zsmalloc compacts zram's pages in Linux. `WmCode` is the handle.
- Short-lived blocks apart from long-lived ones is the rule that keeps a
  heap whole; the program's memory and stack, which live as long as the
  program, stay in the shared heap, the code that comes and goes leaves
  it.
- Nothing changes while a program runs: the same records, the same loop.
  The cost is a `memmove` when a request would otherwise fail.

### Instead

- **A better allocator in the port (TLSF, size classes).** Less
  fragmentation in general, but short and long lives still mix in one
  heap; and the engine must work on the allocator a board has.
- **Counting the records first, then translating into an exact span.**
  No compaction, but the translation twice; and a span of the exact size
  still needs a hole that large.
- **A fixed region for code.** Simple, but its room sits idle when there
  is little code and is missing when there is much; the program's memory
  could not have it back.
- **Running a function in place when its records find no room.** A
  guarantee for code larger than RAM altogether, slower for that
  function: worth having, as its own decision (19), after this one.
- **Smaller cells.** Saves room everywhere, costs speed in the loop.

### Costs

- Flash: the area and compaction, an estimated 1–2 KiB on Xtensa and
  Cortex-M (measured with the work); nothing on AVR (no predecoding).
- RAM: 16 bytes a chunk; a top not yet used in the last chunk, given back
  when the heap needs it.
- Time: a compaction moves at most the area's live records (15 KiB for
  CoreMark: about 0.1 ms on a Black Pill); translation as now.

### Checks

- `test/area.c`, under AddressSanitizer:
  - holes: functions filling chunks, every other dropped, a function
    larger than any hole and smaller than their sum is made, by
    compaction, in no more memory;
  - a function on the stack keeps its address while a host function it
    called makes another module's code and compacts (its records' address
    before and after the import), and the run goes on;
  - a translation that outgrows its chunk while it runs (chunks of 256
    bytes) is moved by the compaction it asked for and ends right;
  - memory before code: `memory.grow` by more than the heap has is done
    once chunks are freed, and the area is smaller after;
  - an allocator that answers every smaller size with a new address: a
    chunk with a pinned span is not cut, one without is, its spans moved;
  - `wm_code_cache_bytes`, `wm_code_area_bytes` and the heap agree after
    each step; everything given back at the end.
- The spec tests with a code cache of almost nothing and chunks of 256
  bytes (records dropped, made and moved all the time), in the fast loop,
  paged and swapped; fuel exact at every slicing (`fuel (as on chips)`);
  the snapshot tests; the fuzzers' finds.
- The loop's machine code unchanged (its hash, as `quick.sh` checks).
- Boards: the Black Pill's system checks all pass, CoreMark in them
  included; ESP32 19/19, programs at once not fewer than 18, CoreMark not
  lower; Mac CoreMark not lower.

### Work, all of it

| # | What | Where |
|---|---|---|
| 1 | The area: chunks, bump, holes, compaction, freeing, `wm_code_area_bytes` | `wm_code.c`, `wm_internal.h`, `wm.h` |
| 2 | Translation straight into the area | `wm_fast.c` |
| 3 | Tails: the ring and the room (done in the branch) | `wm_fast.c` |
| 4 | Memory before code gives the heap memory | `wm_code.c`, `wm_memory.c` |
| 5 | Code images copied into RAM go into the area | `wm_code.c` |
| 6 | `test/area.c`, spec runs under pressure in `check.sh` and CI | tests, tools |
| 7 | Boards, `docs/experiments.md` with the numbers, README, CHANGELOG | |

## 19. WASI without files in the interpreter

**Decided and built (2026-11-22).** Part of the operating system's
decision 15 (each layer holds what is its own).

### Problem

WASI preview 1 is a standard for WebAssembly programs, not for one
operating system, yet its calls lived in the kernel: the layouts of
`iovec`, `subscription` and `event`, the error numbers, both namespaces,
the ENOSYS answer for calls a build leaves out, about 400 lines. Anyone
embedding the interpreter without the kernel (a mini OS of the
interpreter and the devices, a board with no scheduler) had none of it,
so a program built with a WASI C library did not even link there.

### Decision

- **`source/wm_wasi.c` and `wm_wasi.h`**: the calls that need no file
  system: `clock_res_get`, `clock_time_get`, `sched_yield`,
  `poll_oneoff` (clocks sleep, file subscriptions are ready at once),
  `fd_write` to the standard streams, `fd_read` of an empty standard
  input, `fd_fdstat_get`, `fd_close`, `fd_seek`, `fd_prestat_*`,
  `args_*`, `environ_*` (none), `random_get`, `proc_exit`. Both
  namespaces, `wasi_snapshot_preview1` and `wasi_unstable`.
- **What the calls mean on a device is the host's**, in a `WmWasiHost`
  of plain functions: the clocks, sleeping and yielding the caller (a
  host with tasks blocks only the task; a host without waits there),
  where output goes and what writing another fd answers, the arguments,
  random bytes (NULL: a small generator of its own, not for secrets), and
  the trap `proc_exit` ends with.
- **`wm_wasi_resolve_absent`**: any other WASI call links, made to the
  type the program declares, and returns ENOSYS, so a program that only
  might call it still loads.
- A host with files answers the file calls itself first and asks this
  module for the rest, as the kernel does.
- Memory goes through `wm_memory_read`, `wm_memory_write` and
  `wm_memory_pointer`, so the calls work in a paged memory too. A range
  outside the memory answers what it always did: EFAULT, or a trap for
  the calls that always trapped.

### Why

Every engine that runs WASI programs keeps the standard's part apart
from what a platform gives it: wasm3's `m3_api_wasi.c` and WAMR's
`libc-wasi` sit beside the interpreter, and the host only supplies the
clock, the output and the files. Here the kernel was the only host, so
the line was never drawn.

The calls answer byte for byte what the kernel answered: the same
checks in the same order, the same errors, the same generator.

### Checks

`test/wasi.c`, the module alone with a host of a few functions, in a
memory of one block and an always paged one: both buffers of an
`fd_write` and the count, another fd answering the host's errno, a bad
buffer writing nothing; `clock_time_get` through `wasi_unstable`, a
clock that is not there, a result past the memory; arguments; random
bytes, and past the memory a trap; `poll_oneoff` on one clock sleeping
5 ms; `proc_exit` ending with the host's trap and its code; two file
calls nobody answers giving ENOSYS. The kernel's suite runs through it
unchanged (1734 checks).

## 20. A heap of the engine's own: TLSF, and code from the other end

**Decided (2026-11-22).** The user: «сделаем щас… как лучше правильно в
целом по скорости, стандартам и как делают другие».

### Problem

On the Black Pill, CoreMark in the system runs out of memory with enough
of it free: 3 to 12 KB free in a 110 KB heap, in pieces smaller than the
2.6 KB chunk the code area asks for (docs/experiments.md, "Records in an
area of their own"). The C library's allocator mixes what never moves
(programs' memory, stacks, the kernel's objects) with what the engine can
move (records, decision 18) and with short-lived buffers, so the holes
the code area leaves are taken by blocks that stay, and the area cannot
grow even after it compacts. Its time per call is not bounded either,
and it differs on every platform (newlib, ESP-IDF, glibc, Emscripten).

### Decision

- **`source/wm_heap.c`, `wm_heap.h`**: an allocator over one block of
  memory the host gives (`wm_heap_new(memory, bytes)`), the same on every
  platform, used through the allocators the engine already takes
  (`wm_set_allocator`, `wm_set_code_allocator`). Optional: a host that
  keeps its own allocator changes nothing.
- **TLSF** (two-level segregated fit): free blocks in lists by size, a
  first level by power of two and 16 classes in each, found through two
  bitmaps; allocation and freeing in constant time, a block split on
  allocation and merged with free neighbours on freeing (boundary tags).
  Good fit: a request takes a block of its class or the next one up.
- **Code from the other end.** Allocations that the engine can move (the
  code area's chunks, through `wm_heap_code_allocator`) are cut from the
  top of the highest free block that fits; everything else from the
  bottom of the block TLSF finds. What moves gathers at the top, what
  stays at the bottom, and the area's compaction then frees one piece at
  the boundary instead of holes between blocks that stay. Finding the
  highest block walks the free lists: chunks are few and large, so this
  is rare; ordinary allocations stay constant time.
- `realloc` grows into a free neighbour and shrinks in place, splitting
  off the rest; otherwise it moves, from the same end.
- `wm_heap_stats`: bytes free, the largest free block, bytes used, for
  hosts and tests.
- One thread at a time, as the engine; interrupt handlers do not allocate.

### Why

TLSF (Masmano, Ripoll, Crespo, Real, 2004) is the allocator real-time
systems use where time per call must be bounded and fragmentation low:
NuttX offers it as its heap, Zephyr's sys_heap is a near relative
(buckets by power of two, constant time), and WAMR's runtime has its own
heap for the same reasons; FreeRTOS's heap_4 is first fit
with merging, slower and more fragmented. Measured studies give TLSF
fragmentation as low as the best general allocators' (Doug Lea's) at a
fraction of their worst case time.

Keeping what moves apart from what stays is how compacting systems avoid
holes: managed runtimes put movable objects in their own spaces, and the
two-ended heap of many embedded systems grows fixed data from one end and
transient data from the other.

### Instead

- **A fixed region for code**: simple, but its size is a guess: too
  small and code is dropped, too large and programs lose memory.
- **Pages of one size for everything** (frames, as Linux gives memory):
  no external fragmentation at all, but stacks and chunks would need
  rework; after 1.0, on top of this heap.
- **Running from bytecode when records do not fit**: keeps calls from
  failing but slow; it stays the fallback when memory is short in fact.

### Checks

`test/heap.c`: blocks of many sizes allocated, freed in random order and
merged back to one; `realloc` in place and moving, contents kept; code
allocations above all others; the Black Pill's pattern (programs, stacks,
chunks) leaves the free memory in one piece after the area compacts.
Boards: the Black Pill with the system's memory from `wm_heap`, CoreMark
in it. Larger tests later (fuzzing the allocator, every platform).

### Work, all of it

| # | What | Where |
|---|---|---|
| 1 | This decision | docs |
| 2 | `wm_heap.c/.h`, TLSF, high end, stats, allocator adapters | source |
| 3 | `test/heap.c`, `check.sh`, API list | test, tools |
| 4 | The kernel takes code from the high end when the heap is in use | wasm-rtos |
| 5 | Black Pill on `wm_heap`, CoreMark measured, experiments.md | port |


## 21. A function runs in place by itself: when its records find no room, and when the host asks

**Decided (2026-11-23).** The user asked for the second case: «чтобы
если задача была типа не важной или idle подобной ее можно было чисто
так выполнять», with one API the same on every board, the Uno included,
changeable while the program runs.

### Problem

1. **A call fails for want of records.** On the Black Pill with
   `wm_heap` at 104 KB (decision 20, docs/experiments.md "The engine's
   heap on the Black Pill"), CoreMark is predecoded at load, and during
   the run a function's records find no room once the program's 64 KB
   memory, its 8 KB stack and the other functions' records are in. The
   code cache drops what it can (`wm_code_drop_one`, on every allocation
   path already), but the code left belongs to the functions on the call
   stack, so `wm_code_load` returns "exhaustion" and the call fails. The
   choice between the register loop and the loop in place is made per
   module at load, not per function when room runs out.
2. **Every task's code costs the same.** A task of no importance (a
   kernel's idle task, a background job) makes records like any other:
   it takes memory from the code cache and time to translate, and can
   push out the records of tasks that matter.

### Decision

**1. A function whose records find no room runs in place, by itself.**
`wm_code_load` failing with "exhaustion" is no longer the call's end:
the function runs in the loop in place, over the module's bytes, with
the side table entries of that function only (`wm_code_side`), while the
other functions keep their records. The two loops already meet inside
one function (`mixed`: fuel ending inside a segment, a page still being
read, a snapshot); this adds the whole function as the unit. A call out
of it goes to the register loop when the callee has records, and back.
Only when even the side table entries find no room does the call fail.

**2. One bit on the runtime: whether its calls may make records.**

```c
/*
 * Whether calls on this runtime may make records (decision 21); 1 by
 * default. With 0 they use the records other runtimes made and run the
 * rest in place, taking no memory and no time from the others for code.
 * The same results and fuel either way. Can change at any time, also
 * from a host function or between slices of a suspended call; takes
 * effect at the next call of a function without records. Does nothing
 * in builds without predecoding, where everything runs in place.
 */
void wm_set_makes_code(WmRuntime* runtime, int makes);
```

The bit is read in one place, where a function without records is about
to get them (`wm_code_load`): there, "not allowed" and "no room" take
the same branch to the loop in place, except that "not allowed" never
drops others' code to make room. The name may change before the API
list is frozen (the user: «оставим как есть пока»).

**3. Back to records on resume (second step).** A function running in
place when the bit turns to 1 keeps running in place until it returns;
a task whose `main` is an endless loop would stay slow. On resume after
a slice, the engine moves the stopped function to its records when the
bit is 1 and they fit, with the position mapping snapshots already use
(`to_record`, wm_snapshot.c). Same fuel, same results.

**4. The kernel: idle does not make code.** One line where the
scheduler starts a slice (sched.c, beside `wm_set_fuel`):

```c
wm_set_makes_code(task->wasm_runtime, task->priority > OS_TASK_PRIORITY_IDLE);
```

There it follows every change of priority by itself: `os_task_set_priority`,
priority inherited through a mutex (an idle task holding a mutex a high
task waits for gets records while it holds it, which is what inheritance
is for), and a restored snapshot.

**Uno.** `WM_PREDECODE 0`: the same function with an empty body; the
kernel's line is the same. Nothing else changes there.

### Why

Engines with two tiers run each function in one of them and move
between them: the JVM and .NET run a method interpreted until it earns
compiled code, can be told to stay interpreted (`-Xint`, per method in
.NET), and move a running method between tiers in the middle (on-stack
replacement, as JavaScript engines do too). Our register loop is the
upper tier; the loop in place is the lower one, which also needs almost
no memory. What is new here is only who decides: memory (case 1) and
the host (case 2), with mechanism in the engine and policy in the
kernel, as everywhere else.

### Instead

- **A mode with three values** (fast / shared / in place, with a getter):
  "fast" is just the default, "always in place" is only for tests, which
  have the build without predecoding; the host knows what it set.
- **No API: the kernel gives the idle task a code allocator that refuses**:
  the engine answers a refusal by dropping others' code
  (`wm_code_drop_one`), the opposite of what is wanted; it must know
  "not allowed" from "no room".
- **Per module at load, as now**: one large function forces the whole
  module in place (Black Pill at 94 KB: 2.52 for all of CoreMark).
- **A budget of bytes per runtime**: wider, but needs counting records
  per runtime while they are shared between them; only 0 or "as memory
  allows" is needed now.

### Checks

- The same module in the three ways (records, every function in place
  by the bit, records refused at random by a test allocator) gives the
  same results and **the same fuel to the instruction**, also with a
  snapshot in the middle restored the other way; the spec tests run with
  records refused (`check.sh`).
- With the bit 0, `wm_code_cache_bytes()` does not grow and others'
  records stay.
- A call whose records find no room goes on in place (test allocator;
  Black Pill CoreMark at 104 KB, measured).
- Step 3: a task whose bit turns to 1 in the middle of an endless loop
  speeds up, the fuel the same.
- The Uno builds the same kernel line; flash grows by no more than a few
  bytes (AVR size check).

### Open

- Frames: the loop in place and the register loop must agree on a
  frame at a call between them; `mixed` does so inside one function, to
  check for calls both ways before building.
- How much the side table entries of one function take, against its
  records (to measure: CoreMark, the largest functions).

### As built

- A function in place by itself keeps its side table in its tail
  (`wm_reg_side`), without the translator's entries; they are made into
  the same tail when part of it later runs inside the register loop, so
  frames pointing into its side table stay valid. A function without
  branches gets one empty entry, so such a frame never has `st` NULL.
- The frames answer the open question: the register loop's slots are the
  stack machine's (decision 16), so a call between the loops needs no
  conversion; a return reads its caller's frame (`st` NULL: the register
  loop) and goes on in that loop. One test more per return in the
  register loop: Mac CoreMark ±0 (0.333 against 0.336 ns per
  instruction, within the runs' spread).
- A call from a function in place to one without records stops where
  making room is safe (as the register loop does, `wm_code_stop`), and
  there it gets them or runs in place too.
- `memory.grow` and the misc instructions in the loop in place now name
  their function as the caller (`WM_TRACKS_CALLERS`), as the register
  loop does: the cache could otherwise let its side table go meanwhile.
- Uno: flash 35 644 B against 35 688 B before, RAM the same.
- Checks: `test/inplace.c` (the three ways, sliced, snapshots changing
  the setting in turn, a loop that never calls moving to its records);
  spec with `-DWM_SPEC_IN_PLACE=1` and `=2`, also paged with code always
  moved, in `check.sh` (ASan only: UBSan's interpreter predecodes nothing).

### Work, all of it

| # | What | Where |
|---|---|---|
| 1 | This decision | docs |
| 2 | Function in place when records find no room; calls between the loops | source |
| 3 | `wm_set_makes_code`, the bit in `WmRuntime`, the API list | source, docs |
| 4 | Tests: three ways the same, fuel, refusing allocator, spec with refusals | test, tools |
| 5 | Back to records on resume | source, test |
| 6 | The kernel's line, a kernel test with an idle task | wasm-rtos, tests |
| 7 | Black Pill CoreMark at 104 KB, ESP32 unchanged, experiments.md | port |

## 22. One description of WebAssembly's instructions

**Decided (2026-11-23).** The user: «пусть все опкоды и тд все что
касается wasm операндов и тд будет в одном файле defs так же проще искать
а не бегать по файлам», and the firmware smaller, the code clearer, not
slower.

### Problem

What the engine knows about an instruction is written in six places, each
its own way: the validator's switch (`wm_module.c`), a table of 128 bare
numbers (`wm_numeric`), the loop in place's dispatch and switch, the
translator's `step()` and its switch (`wm_fast.c`), the shared helpers'
switches (`op_i32`, `op_i64`, `op_float`: 82 cases), and constant
expressions (`wm_instance.c`). Two `.def` files hold parts: the i32
operations (`wm_ops.def`) and the register loop's records
(`wm_records.def`). Adding or checking an instruction means visiting all
of them; reading the immediates of one is written four times. The
numeric instructions' types were 128 bare numbers, the accesses' two
tables of 23 more.

### Decision

**One file, `source/wm_defs.def`**, in parts, included with the
kinds a reader needs defined (X-macros, as now):

1. **WebAssembly's instructions**, one line each, in opcode order:
   `WM_OP(0x6A, I32_ADD, i32_add, NONE, I32_I32__I32, a + b)`: opcode,
   names, what immediates follow, the signature (operands, results), and
   for the i32 instructions both loops share, the expression. The `0xFC`
   instructions the same way (`WM_OP_FC`). What `wm_ops.def` holds now
   becomes this part's last column.
2. **The register loop's records** (what `wm_records.def` holds now),
   after it: the engine's own instructions, made from the first part.

**Made from it**, replacing what is written by hand:

- `WM_OP_*` and `WM_FC_*`: every opcode by name, used where instructions
  are read (the validator, both loops, the translator, constant
  expressions): `case WM_OP_END`, not `case 0x0B`.
- `wm_numeric` (the numeric instructions' types) and the two access
  tables (the validator's, the shared helpers'), from the lines.
- The translator's choice of records for i32 accesses, from their list.
- The profile's names (`tools/profile.py`).

**Not changed:** the two loops' hot code. Their dispatch is made from the
same lines, but each handler stays what it is now; their machine code is
checked against the last commit's (decision 13, `SHA=1`) and must not move
unless a measurement says it is faster.

### Why

The engines that last describe their instruction set once: WABT
(`opcode.def`: opcode, names, signature, immediates), V8
(`wasm-opcodes.h`, `FOREACH_*_OPCODE` with signatures), SpiderMonkey
(`WasmConstants.h`, `FOR_EACH_*`), WAMR (its opcode tables). One line per
instruction makes the instruction set readable as a list, and the
compiler checks what a person used to check by eye.

### Instead

- **Only moving the two `.def` files into one**: easier to find, but the
  six places stay; little gained.
- **Generating C from a script**: the user wants no tools on the computer,
  and X-macros need none.
- **One reader of immediates for the validator, `step()` and the loop in
  place**: each reads them for its own end (checking, counting cells,
  running), so one reader would branch on what it is for; left as is.
- **The validator's simple instructions through a signature**: they are
  already (numeric through `wm_numeric`, accesses through their table);
  what stays written out is control, locals, calls and tables, as in
  every engine.
- **The loops' handlers made from the table too**: their hot code is tuned
  by profile (decisions 13, 14); a generic handler would be slower.

### Checks

spec in every mode `check.sh` runs; the loops' machine code unchanged
(Mac, Xtensa); flash on AVR, M4, Xtensa against main; CoreMark on a Mac,
ESP32 and Black Pill against main. Kept only if smaller and not slower.

### As built

- 184 opcodes and 18 after `0xFC`, each once (checked: no gaps from 0x28
  to 0x3E and 0x45 to 0xC4, no repeats); the generated tables equal the
  numbers they replace byte for byte.
- Offsets into the tables use unsigned bases (`WM_FIRST_ACCESS`,
  `WM_FIRST_NUMERIC`): with the signed enum the Mac compiler laid the
  loop out differently. The loops' machine code is the same as before on
  the Mac and Xtensa; Uno flash the same to the byte (35 644 B).
- Records of i32 accesses take the instructions' names (`I32_LOAD8_S`).
- Flash does not shrink: the validator was table driven already. What is
  gained is reading: one list, names instead of numbers.

### Work, all of it

| # | What | Where |
|---|---|---|
| 1 | This decision | docs |
| 2 | `wm_defs.def`: every instruction (MVP and 2.0 without SIMD), signatures, immediates; `wm_ops.def` and `wm_records.def` folded in | source |
| 3 | `wm_numeric` from it | source |
| 4 | One reader of immediates for the validator, `step()`, the loop in place, constant expressions | source |
| 5 | The validator's simple instructions through the signature | source |
| 6 | Names for the profile and traces | source |
| 7 | Sizes (AVR, M4, Xtensa), CoreMark on three machines, loops' machine code, spec; experiments.md | test, docs |

## 23. Linkers outside the engine: modules read and linked through `wm.h`, linked instances in one snapshot

**Decided and built (2026-11-27)**, with four more functions than on
paper (see "Built" below). The user: «dylink из репо вытеснить и сделать
не по def а как .h… чтобы можно было сделать похожий например свой
механизм при желании», «мне нужно максимально элегантное решение». With
wasm-rtos decision 16 (loaders), which takes dylink out of the kernel too.

### Problem

1. **dylink.0 is in the engine, and only it can be.** `wm_dylink.c` (975
   lines) includes `wm_internal.h`: it reads `m->imports` and
   `m->exports`, finds its custom section in `module->bytes`, binds a
   function import by overwriting the instance's stub (`*stub = *found`)
   and writes `table->elems`. None of that is in `wm.h`, so no other way
   of making a program out of modules can be written beside it. Yet
   dylink.0 is not WebAssembly's standard but a convention of the tools
   (WebAssembly/tool-conventions, `DynamicLinking.md`).
2. **`wm.h` promises a function it does not have.** `WmResolver`'s comment
   says an unresolved function import traps "until
   wm_instance_link_function binds it"; there is no such function.
3. **A snapshot is of one instance.** `wm_snapshot.c` refuses a call of
   another instance on the stack, a reference to another instance's
   function, and anything but one instance: a program linked from
   modules cannot be saved, swapped out as its state or put to deep
   sleep.

Size and speed are not the problem: `wm_dylink.c` is 6630 bytes on a
Cortex-M4 (`-O2`), only in builds that link it, and runs only while a
program loads; calls between modules go straight in, as within one.

### Decision

**1. Reading a module**, named after WebAssembly's JS API
(`Module.imports`, `Module.exports`, `Module.customSections`) and
wasm-c-api (`wasm_module_imports`, `wasm_module_exports`). Each returns
how many there are and fills `out` when `index` is below that:

```c
typedef struct WmExternType {
    const char* module; uint32_t module_length;   /* imports only */
    const char* name;   uint32_t name_length;
    uint8_t kind;                                 /* WM_EXTERN_* */
    uint32_t index;                               /* in its kind's index space */
    /* func: the signature; table, memory: limits; global: type, mutable */
    ...
} WmExternType;

uint32_t wm_module_imports(const WmModule* module, uint32_t index, WmExternType* out);
uint32_t wm_module_exports(const WmModule* module, uint32_t index, WmExternType* out);
uint32_t wm_module_custom_sections(const WmModule* module, const char* name, uint32_t name_length,
                                   uint32_t index, const uint8_t** bytes, uint32_t* size);
```

`wm_module_import_type` stays (1.0 names are only added).

**2. Linking late**, what a linker of modules that call each other needs
(an import can only be bound once the other module is instantiated):

```c
/* Binds a function import left unresolved; its type must match. Calls go straight in. */
WmResult wm_instance_link_function(WmInstance* instance, uint32_t import_index, WmFunc* func);
uint32_t wm_table_size(const WmTable* table);
WmFunc*  wm_table_get(const WmTable* table, uint32_t index);
WmResult wm_table_set(WmTable* table, uint32_t index, WmFunc* func);
```

`wm_instance_link_function` is what the resolver's comment already
promises; it does what dylink does to the stub now, behind a type check.
The table's three are JS API's `Table.length`, `get`, `set`.

**3. A snapshot is of the program's instance and every instance linked to
it.** `wm_snapshot_write` and `wm_snapshot_read` keep their signatures;
what "the instance" covers grows. The engine finds the group itself,
from the instance it is given: the instances its bound function imports
belong to, and those its tables' functions belong to, depth first in
import order, then table order. Nobody hands it a list.

- Shared memories and tables are written once; each instance's own
  globals, tables and dropped segments as now.
- Frames and function references are (instance in the group, function):
  calls between modules on the stack and other modules' functions in a
  table are saved.
- On reading, the same walk over the instances given must find as many,
  each of a module with the hash written (already in the header):
  otherwise `wm_snapshot_invalid`, before anything changes.
- The header's version goes up to 3; the old version is refused
  (`wm_snapshot_invalid`): 1.0 is not tagged, no snapshot of it is kept
  anywhere, and reading both would be two readers for one format.
- A host function in a table or on the stack is written as the first
  function import bound to it. Version 2 wrote it as null, so such a
  program trapped after it was read back.

A linker then writes nothing for snapshots. Its host builds the same
instances again, as the kernel does now for one: instantiated with no
runtime, started unless the memory was kept apart (back from swap, where
nothing runs), and reads the snapshot over them, which replaces whatever
their start functions wrote.
That holds for a linker that builds the same group from the same modules
and keeps its state in WebAssembly's objects (memories, tables, globals),
not in C variables of its own.

**4. dylink.0 leaves the engine.** `wm_dylink.c` and `wm_dylink.h` move
to their own repository (wasm-rtos-dylink, wasm-rtos decision 16),
rewritten on `wm.h` and `wm_heap.h` only, following DynamicLinking.md as
now. Their names leave `docs/api-1.0.txt`; 1.0 is not tagged yet.

### Why

- WebAssembly's specification leaves linking to the embedder
  (`instantiate(module, imports)`); in the browser, Emscripten implements
  dylink.0 in JavaScript on the JS API, and V8 knows nothing of it. The
  engine should hold the standard and let conventions live beside it, as
  WASI does (decision 19).
- Linux's dynamic linker is not in the kernel either (`ld.so`, named by
  `PT_INTERP`; `dyld` on macOS): the kernel loads, a program links.
- Saving a group by rebuilding it and laying the state over it is how
  CRIU restores processes; not running start functions again is what
  Wizer does for snapshots of initialised modules. WebAssembly has no
  standard for snapshots: ours stays ours, and says so.

### Instead

- **dylink stays, behind `OS_HAS_DYLINK`**: one way to link, in the
  engine, unsaved. What we have.
- **The linker hands the engine a list of its instances** (and a recipe
  to rebuild them): the engine can find them from the links, and a list
  is one more thing every linker gets wrong.
- **A mode in which no WebAssembly code runs, for restoring**: not
  needed; building without running is already how instances are made.
- **A loader in WebAssembly** (a `dylink.wasm` linking through calls of
  the host): possible later through the same functions given to programs
  as handles, with a rule for who may call them. Not for 1.0.
- **The Component Model**: the standard way to compose modules, too
  heavy for these boards now; a loader of its own later, the engine
  unchanged.

### Steps

| # | What | Where |
|---|---|---|
| 1 | `wm_module_imports`, `wm_module_exports`, `wm_module_custom_sections`; test | source, test |
| 2 | `wm_instance_link_function`, `wm_table_size/get/set`; the resolver's comment true | source, test |
| 3 | Snapshots of linked instances: the walk, (instance, function), version; old snapshots read | source, test |
| 4 | `wm_dylink` out, to wasm-rtos-dylink on `wm.h`; its tests with it | source, docs, CI |
| 5 | `api-1.0.txt`, internals, README, CHANGELOG | docs |

### Built

- Rewriting dylink.0 on `wm.h` showed what a linker needs besides what was
  on paper: **`wm_module_memories` and `wm_module_tables`** (a program
  defines its memory without exporting it, so imports and exports do not
  say it is there), **`wm_func_equal`** (an import bound late is a copy of
  its target, and C needs `&f` to be one value whichever module takes
  it), **`wm_global_set`** (the GOT is filled once every module is in;
  `Global.value` in the JS API), and **`wm_allocate`** (a linker's memory
  goes through the engine's allocator, so a kernel counts it per task).
- `wm_instance_link_function` refuses a target that is itself an import
  still unlinked: binding to it would copy a stub.
- The group is found from the copies, the tables and the globals, breadth
  first; nothing needed a list from the linker, nor a mode in which code
  does not run.
- Checked: `test/link.c` (reading, linking, tables; 4 builds),
  `test/group.c` (a call stopped with frames in two modules that call each
  other, five fuel points, a host function in a table, a group that
  differs refused; 5 builds), the kernel's tests, and wasm-rtos-dylink's
  test, which fails with the snapshots of version 2. Machine code of the
  loops unchanged (Mac, Xtensa); Uno (`WM_TINY`) 28330 bytes as before.

## 24. A kept memory may be the one a module imports

**Decided and built (2026-11-27).** The user: «да делай своп» — a task of a
loader (wasm-rtos decision 17) leaves RAM as any task does.

### Problem

A swapped-out task comes back on the memory kept apart from it:
`wm_instantiate_with_memory` gives a new instance the memory of the old
one, writes no data segments into it (its bytes are there already) and
runs no start function. It takes only a memory the module defines. A
program linked from modules has one memory that its program defines and
its libraries import (dylink.0): the program can be rebuilt on it, but a
library cannot. Instantiated as new, a library writes its active data
segments into the memory again, over the values the program left there,
and the state read back afterwards (a state snapshot leaves the memory
as it is) does not repair them.

### Decision

**`wm_instantiate_with_memory` takes a kept memory the module imports,
too.** The signature stays:

```c
WmResult wm_instantiate_with_memory(WmInstance** out, WmModule* module, WmMemory* memory,
                                    WmResolver resolver, void* context);
```

- A module that **defines** its memory takes it, as now: the instance
  owns it and `wm_instance_detach_memory` gives it back.
- A module that **imports** its memory has it bound to that import: the
  resolver is not asked for it, the import's limits and page size are
  checked as any import's, and the memory stays the caller's
  (`wm_instance_detach_memory` gives NULL, as for any import).
- Either way **no active data segment is written** (each counts as
  dropped, as now) and no start function runs. Element segments are
  written as now: tables are rebuilt and the state read over them.

One rule for both: a memory given here has its bytes already.

### Why

- A linker rebuilds the same instances after a swap as after a snapshot
  (decision 23): the program on its own memory, each library on the
  memory it imports. With one function for "this memory has its bytes",
  a linker needs nothing else to come back from swap.
- The memory a module imports is still one memory: which module defines
  it says who frees it, not whether its bytes are there.

### Instead

- **A flag on `wm_instantiate`** ("no data segments"): one more parameter
  of the call every host makes, for what only a kept memory needs.
- **A function of its own for imported memories**: two names for one
  idea.
- **The linker saving its libraries' data and writing it back**: copies
  the memory the MMU just paged out, and every linker would write it.

### Steps

| # | What | Where |
|---|---|---|
| 1 | `wm_instantiate_with_memory` binds a kept memory to the memory import, no data segments, no start | source |
| 2 | Test: a library rebuilt on a kept memory leaves its data as the program left it; limits checked; detach gives NULL | test |
| 3 | `wm.h` comment, CHANGELOG | docs |

### Built

As on paper. Checked: `test/kept.c` in three builds; `tools/check.sh` in
full (the spec suite, the kernel's tests also paged); wasm-rtos-dylink's
test, where a linked task swapped out inside a library comes back and
ends as a run that never left, and fails when the libraries' data
segments are written again; on the boards, a linked task swapped out
inside the library and back (ESP32, Black Pill). The loops' machine code
is the same (Mac, Xtensa); `WM_TINY` 28386 bytes, 56 more (the import
binding, run when a module is instantiated).

## 25. A store owns what a process makes

**Decided and built (2026-11-28).** The user, after a shared
module charged to a task that was gone: «я хочу сделать правильней»,
«сначала продумай архитектуру наилучшим образом … можем ли мы переделать
архитектуру эту к стандарту», and on this paper: «лучше сразу нормально
делай» (built directly, measured as it is built, no sketch first).

### Problem

The engine has no notion of a process, so whoever holds a group of
objects decides alone who frees what, and each did it its own way:

| What | Who frees it now |
|---|---|
| A module | the caller, after every instance of it; a shared one by a count of its own: the kernel's programs (`module_users`, kernel decision 9), dylink's libraries (`users`, `DylinkOptions.release`, wasm-rtos-dylink decision 2) |
| A memory | the instance that defines it (`owns_memory`); kept apart for swap it belongs to nobody (`wm_instance_detach_memory`, decision 24) until a new instance takes it |
| Tables, globals shared between instances | the linker: `owns_memory`, `owns_table` and a list of globals in dylink.c, instances freed first |
| An instance that failed after its imports were linked | "stays as long as the tables it is in" |
| Host functions | the kernel, never (shared by every task) |
| Interpreter memory charged to a task | the kernel: a global "account now" switched in six places (`OS_HEAP_ENTER`), an account pointer in a header before every allocation |

Each rule is right where it stands; together they are six rules for one
question, and where two of them meet, a hole: a shared module is charged
to the task that parsed it, and freed after that task is gone the
kernel writes into the freed task (AddressSanitizer; kernel decision 9
since 2026-11-13, dylink decision 2 since today). The same lack of a
process is why a program importing a memory a loader made cannot swap
out (kernel decision 17: nobody owns the memory to keep it apart), and
why the kernel cannot hold a task to a memory limit: it can only count,
and only with `OS_HAS_MEMORY_ACCOUNTING`.

### Decision

**A store, as in wasm-c-api (`wasm_store_t`): the owner of everything a
process makes.** A task of the kernel is one store; a linker builds its
instances in the store it is given.

```c
typedef struct WmStore WmStore;
WmStore* wm_store_new(void* host);          /* host: the owner's, given back to the allocator */
void wm_store_free(WmStore* store);         /* everything in it, in the order the engine needs */
void* wm_store_host(const WmStore* store);
```

1. **Objects made in a store belong to it**: instances, memories, tables,
   globals, runtimes, and modules loaded for it alone. New functions take
   the store first, as wasm-c-api's do (`wasm_instance_new(store, ...)`):
   `wm_instance_new`, `wm_memory_new_in`, `wm_table_new_in`,
   `wm_global_new_in`, `wm_runtime_new_in`, `wm_module_load_in`. Freeing
   the store frees them all: no order to keep, no flags of who owns a
   memory or a table. An instance that failed stays in the store until it
   goes. One object can still be freed alone (`wm_instance_free` and the
   others work on an object of a store), which wasm-c-api does not allow:
   a library unloaded from a running program (ABI 1.9 later) and a long
   task that makes and drops instances need it.
2. **A module loaded with no store is the engine's**, shared by any
   number of stores, as wasm-c-api shares one (`wasm_module_share`,
   `wasm_module_obtain`). It counts its instances, and tells its owner
   when the last one goes:
   ```c
   void wm_module_on_unused(WmModule* module, void (*unused)(void* context, WmModule* module), void* context);
   ```
   The owner (a program of the kernel, a library of the loader) frees it
   or keeps it then. This is the one count the engine keeps: instances
   point to their module and a module to nothing, so it has no cycles.
   It replaces the kernel's `module_users` and dylink's `users` and
   `release`.
3. **The host is asked before a store's memory or table grows**, as the
   pager's `reserve` is asked before frames are promised: how much a
   program may have is the host's policy, not the engine's.
   `wm_set_store_growing(growing)`: before a memory or a table of a store
   is made or grows (`memory.grow`, `table.grow`, an instance's own, the
   host's `wm_memory_grow`), `growing(host, store, kind, bytes)` is told
   what the store's memories and tables would then hold, walked from its
   objects (a handful: nothing is kept that could drift). No: `memory.grow`
   and `table.grow` give -1, an instance or memory is not made. That is
   what a program can grow by while it runs; what an instance takes
   besides is fixed by its module when it is made, and a creation that
   finds no room fails alone already. Nothing is counted for it, so it
   holds in every build, AVR too.
4. **A store counts every byte of its objects only when asked to**
   (`WM_STORE_BYTES=1`, for a host that reports them):
   `wm_store_bytes(store)` and the most it has held. What is the engine's
   (its modules, the code cache, host functions, WASI's functions) is
   charged to no store. Each allocation then carries the store it is
   charged to and its size in a header, taken from the object it is for,
   and a store outlives what is charged to it, so nothing is ever charged
   to a store that is gone. Off by default: the header is 8 bytes of every
   allocation on the boards.
5. **Swap: a store gives back all but its memories.**
   `wm_store_release(store)` frees its instances, tables, globals,
   runtimes and the modules loaded for it, and keeps its memories with
   their frames and slots. A new instance is made in the same store on
   one of them (`wm_instance_new_with_memory`, decision 24's rules: no
   data segments written, no start function), found with
   `wm_store_memory(store, index)`. The memory never belongs to nobody,
   so a memory a loader made swaps out like a program's own.
6. **Snapshots do not change.** They are still of an instance and those
   linked to it, found by their links (decision 23), in the same format:
   a store is not written into them and is not needed to read one back.
7. **Host functions stay the engine's**, made once and shared by every
   store. wasm-c-api makes them per store (`wasm_func_new(store, ...)`);
   here that would cost each task its imports again in RAM.
8. **The functions of 1.0 keep working as now**: objects they make belong
   to no store and keep today's rules (`wm_instantiate`,
   `wm_instance_detach_memory`, `wm_instantiate_with_memory`). The
   interpreter, the kernel and dylink move to stores; a host that never
   makes one (wasm-rtos-hw, examples, the site's simulator) is unchanged.
   The old ones are marked as for hosts without a store in `wm.h`.

What stays outside wasm-c-api, as extensions: fuel, runtimes that stop
and go on, snapshots, paging, code images, an instance on a memory kept
from another, objects freed alone, host functions of the engine. A
`wasm.h` of wasm-c-api's own names can sit on `wm.h` later, in a file of
its own that a build carries only when it calls it (a decision of its
own).

### Why

- One owner for everything a process has: each of the six rules above
  becomes "it is the store's", and the hole cannot happen rather than
  being patched.
- The standard's model where it fits these boards, its names where we
  add functions: a store, a shared module, objects made in a store.
- A limit per task is what "one program does not take the system down"
  needs for memory, and the host decides it: the engine asks, as it asks
  the pager, and knows from its objects what the store holds.
- Nothing on the hot path changes: calls, fuel, memory access and the
  code cache are as they are; only making and freeing objects do.

### Instead

- **A count on every object** (a memory, a table, a function, an
  instance): a table holds an instance's functions and the instance its
  table, a cycle that is never freed, which is every dylink.0 process; it
  does not fix charging either.
- **Charge shared modules to no task in the kernel** (about 20 lines):
  fixes the hole, and leaves the other five rules, the swap of a
  loader's memory and the missing limit.
- **The limit as a number of every byte in the store, the header always
  on** (built first): about 300 bytes more a task on the boards (8 bytes
  of each of some 26 allocations), every allocation paying for a count
  only a report needs, and the policy in the engine, not the host.
- **Counting each object's bytes by hand** (no header): some 50 places to
  keep right, a wrong one silently off; for 200 bytes a task.
- **A store freed only whole**, as wasm-c-api's: no library could be
  unloaded from a running program, and a long task that makes instances
  would grow.
- **A store as one block of RAM** (an arena): fewer pieces in the heap,
  but memories and tables grow and objects are freed alone. Possible
  later inside a store, nothing in the API changes.
- **The store in snapshots, as the list of instances**: decision 23 found
  them by their links so that no linker has to hand a list; the format
  stays.
- **All of wasm-c-api now** (`wasm_*_vec_t`, `wasm_valtype_t` objects,
  copies of every reference): many small allocations, too much for an
  ATmega328P. Later, as a layer.

### Cost

- RAM: a store (about 32 bytes), and a store pointer and a link in each
  object of one (8 bytes on 32-bit, a handful of objects per task), a
  store pointer in each table. With `WM_STORE_BYTES` the header too.
- Code: an estimate of 400 to 800 bytes; `WM_TINY` (28386 bytes now)
  measured before and after.
- Two ways for a while: the functions without a store stay until a 2.0.

### Steps

| # | What | Where |
|---|---|---|
| 1 | Measured as built: `WM_TINY`, Cortex-M4, lines that go in the kernel and dylink; this paper corrected by what is found | source, measured |
| 2 | `WmStore`, objects in a store, freed with it or alone | `wm_instance.c`, `wm_memory.c`, `wm.h` |
| 3 | Modules of the engine: the count of instances, `wm_module_on_unused` | `wm_module.c` |
| 4 | Bytes and limit of a store; the allocator told the store | `wm_heap.c`, `wm.h` |
| 5 | `wm_store_release`, `wm_store_memory`, `wm_instance_new_with_memory` | `wm_instance.c` |
| 6 | Tests: everything of a store freed in any order (ASan); a failed instance; a module's last instance tells its owner, in any store; limits refused as the heap's are; swap through release, also of an imported memory; old functions unchanged; each rule broken on purpose fails | `test/` |
| 7 | Kernel decision 18, dylink decision 3 on it; the boards (ESP32, Black Pill, Uno) | other repositories |

### Built

With what building and measuring it found:

- **Built first with the count always on and a limit on it**
  (`wm_store_limit`), then changed (items 3 and 4 above): measured on the
  boards, a task paid about 300 bytes for a count only a report reads,
  and the limit was the engine's policy. The engine now asks the host,
  and counts only with `WM_STORE_BYTES=1`.
- **Counting, when on: each allocation is charged to the store of the
  object it is for**, given by that object (`wm_store_calloc(store,
  ...)`), and kept in a header with its size, so growing or freeing it
  later charges the same store whatever runs then. Only loading a module
  for a store charges a store without an object at hand (`wm_charge`,
  set for that load alone). A store freed with bytes still charged to it
  (a linker's records not freed yet) is kept until the last of them goes.
- **Asking**: in `wm_memory_new_in` and `wm_memory_grow`, in `grow_table`
  (`table.grow` and `wm_table_grow`), and before an instance's own tables
  and memory are made; a table knows its store for it. Restoring a
  snapshot's table sizes is not asked: it puts back what was allowed.
  Refused growth drops no code: it is not the heap's.
- **Tables and globals made alone in a store** are nodes around the
  object, flagged in its padding: those an instance embeds grow by the
  store pointer only.
- **The module's count is of instances, not of holders**: a host that
  keeps a pointer to a module of the engine with no instance of it (the
  kernel between parsing and instantiating) asks for it again after an
  instance of it failed (kernel decision 18).
- `wm_instance_detach_memory` gives NULL for an instance of a store: its
  memory is the store's.
- **Not in `WM_TINY`** (`WM_HAS_STORES=0`): an Uno runs one program on the
  functions of 1.0 and has no kernel to own processes, and the lists and
  counts cost it about 700 bytes of 1.3 KiB left. A build that wants
  stores there sets it. The walk of a store for the host is linked only
  into a build that calls `wm_set_store_growing`.

### Checks

- `tools/check.sh` passes whole: the spec suite (27437) in every build it
  has, swapping, snapshots between builds, the code cache and images, the
  kernel's 92 tests, also with every memory paged.
- `test/store.c`, 37 checks, with AddressSanitizer, with UBSan, without
  predecoding, with every memory paged, with every byte counted
  (`WM_STORE_BYTES=1`, its byte checks only there): a program and a library that imports its
  memory, a table, a global, a second memory and a runtime in one store,
  freed in one call, the heap as before; a module of the engine charged
  to no store, one loaded for the store charged to it; a start function
  that traps leaves its instance in the store, which frees it; a module's
  owner told once, after the last instance in either of two stores; the
  host told the store's memory and table bytes, refusing an instance as a
  full heap would, then memory.grow and table.grow (-1), allowing one
  element more, no code dropped, the other store growing, an instance of
  no store never asked; released, a store keeps its memory,
  and a program and its library made again on it go on from what it held;
  another store's memory refused; a linker's record keeps a freed store
  until it goes.
- Size: `WM_TINY` on an ATmega328P 28666 bytes, 280 more than before
  stores (the functions of 1.0 now wrappers of those with a store); 29136
  with stores in it and 29656 with the walk always linked, as first
  built. The engine on a Cortex-M4 (`-Os`, all objects whole) about 1.4
  KiB more. The hot loops are untouched; growing a memory or a table asks
  a pointer first.
- ESP32 as built now (growth asked, no header), the same board: 24 of 24,
  nothing leaked, 18 programs at once as before stores; with the card, a
  task linked with its library 29768 bytes of heap and the second 26704,
  28 more than before stores (29740, 26676; 30236 and 27104 with the
  header); 500 round trips 95 ms (88 before stores, 113 with the header:
  where the code lies in the flash cache, no work more a message).
- Black Pill as built now: the same 21 of 22 (CoreMark does not fit,
  before stores too), nothing leaked; a task with CoreMark's module 96
  bytes more than before stores (26776 free after it, 26872 before,
  26592 with the header); 500 round trips 74 ms as before; 10 programs at
  once (9 before: where the last creation fails moved, not a gain).
  As first built, with the header on every allocation:
- ESP32 (esp32dev), the kernel's board checks before and after, on the
  same board: 24 of 24 both, nothing leaked. A task takes about 340 bytes
  more of heap (the header before each allocation, the store): 17
  programs at once where 18 fitted. 500 round trips through queues 88 ms
  before, 113 after, every run the same; with `WM_STORE_BYTES=0` 95 ms
  and 18 programs. No allocation is made per message (about 8000 in the
  whole run of the checks, before and after alike on a computer), so the
  time is where the code lies in the 32 KiB flash cache, which moved the
  same exchange from 45 to 89 ms before when the interpreter's loop grew;
  CoreMark 21.40 before, 21.94 after.
- Black Pill (STM32F411, the engine's heap of 104 KiB), before and after:
  the same 21 of 22 (CoreMark does not fit, before as after); fib(24)
  318 ms both, 500 round trips 74 and 75 ms; 9 programs at once both; a
  task with CoreMark's module 280 bytes more.
- What a task costs, measured on a computer with the same heap (TLSF, an
  arena of 104 KiB) and the board checks' program: 12288 bytes before,
  12888 after, 12440 with `WM_STORE_BYTES=0`; 26 allocations live in a
  task (25 before: the store). The header is 16 bytes on 64-bit, 8 on the
  boards, and the heap takes for each block what it took before plus the
  header, no more: the cost is the header times the allocations a task
  keeps, about 2.5 percent on the boards. 8 tasks fit the arena before,
  7 after. Fewer, larger allocations would lower it, and the heap's own
  overhead with it.
