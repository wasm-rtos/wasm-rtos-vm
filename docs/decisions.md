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
| [14](#14-operands-in-records-as-overlays-on-them) | Operands in records, as overlays on them | 2026-11-16 | 18 forms; CoreMark ESP32 +3.7%, Black Pill +8.8%; exact fuel and snapshots kept |
| [15](#15-records-in-storage-the-processor-reads-ram-for-the-hot-ones) | Records in storage the processor reads, RAM for the hot ones | 2026-11-20 | code from flash with no RAM: ESP32 −3.2%, Black Pill −5.8%; short of RAM −1.6% instead of −48% |

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
