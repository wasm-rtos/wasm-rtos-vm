# How wasm-rtos-vm works

For people who change the engine. Using it needs only the
[README](../README.md). Why it is so: [decisions.md](decisions.md); what
was tried and not kept: [experiments.md](experiments.md).

## Loading

Loading decodes and validates the whole module once and builds a side
table with the target of every branch, so execution never searches the
code. Nothing is compiled: the module's bytes stay as they are and are run
from where they were loaded. Values live in 32-bit slots (i64 and f64 take
two), which keeps stacks small on 8- and 32-bit chips.

Without validation (`WM_HAS_VALIDATION=0`) the same pass still builds the
side table, but trusts the program: type, alignment and other validity
checks are left out, so an invalid program can misbehave. Checks that keep
the engine's own arrays in bounds stay.

## Two loops

What they share (traps, immediates, stack slots, memory accesses, numeric
and bulk instructions) is `source/wm_exec.c`, which includes the two loops
so the three are one unit for the compiler.

- **In place** (`wm_execute`, `source/wm_exec_inplace.c.h`): runs the
  module's bytes with the side table. Every build has it; on AVR it is the
  only one.
- **Predecoded** (`run_fast`, `source/wm_exec_fast.c.h`, with
  `WM_PREDECODE`, the default where labels can be values): runs records,
  one per instruction, in 32-bit cells. The
  first cell holds the handler (an offset from the first handler) and a
  16-bit operand (a local's index, a small constant, a call's function, a
  branch's distance), so most instructions take one cell and branches two.
  It is two to three times faster. A module whose operands do not fit runs
  in place.

The records are listed once, in `source/wm_ops.def` (decision 9): their
kinds, the loop's table of handlers and the predecoder's choice of record
come from it, and so do the i32 operations with records of their own
(`i32_add` and the rest), which both loops run. A new record is a line
there and its handler; a new i32 operation is only the line.

The top of the stack stays in a register (`tos`); its slot is written only
when the stack goes to memory (`FAST_SPILL`): before a call, a helper, a
branch that moves values, a stop. There the stack is in memory as the loop
in place keeps it, so either loop goes on from where the other stopped.

Records follow instructions one to one, so positions convert both ways
(`wm_fast_position`, `wm_fast_ip`): fuel and snapshots stay those of the
module's own instructions, whichever loop runs.

Nothing is called from inside the predecoded loop for the rare cases: on
Xtensa a call anywhere in the handlers costs the whole loop its registers.
A TLB miss and a call into a function without records are exceptions: the
loop stops as it does for fuel, `wm_execute` deals with them and resumes,
and the instruction runs again (a miss) or the callee starts (code).

## The code cache

Records are a cache (`source/wm_code.c`). When the first instance is made
every function is predecoded once, to know the module can be, and the side
table is freed. A function's records are then made at its first call and
dropped by CLOCK when memory for records runs out, when the cache is over
`wm_code_cache_limit`, or on `wm_code_cache_trim`. To make them again the
function is validated once more for its side table entries
(`wm_code_side`), which are dropped right after.

A call marks its function used; the hand clears marks and drops a function
it finds unmarked. Functions on a call stack are never dropped: every
runtime's frames, where a suspended one stopped, and the callers of host
functions that called back in (`rt->caller`, and a `WmNest` chain on the C
stack for nested `wm_call`). Room is made only outside the fast loop, where
all of that is written down.

Memory comes before code: when the heap refuses a frame of a program's
memory or its tables, records go first, one function at a time, and only
then does the allocation fail (with a pager, its own choice of a frame to
store comes first). `memory.grow`, `memory.init`, `memory.copy` and
`memory.fill` note the running function in `rt->caller` too, as they may
make frames from inside the fast loop.

With a code image mapped (decision 15, `wm_code_image_map`), a function
without records copies them from the image instead of being validated and
predecoded again; when no RAM can be had, even after dropping others, its
records stay where they are in the image (`WmCode.mapped`: nothing to free,
not counted). The hand passes such functions by, as they hold no RAM;
`wm_code_cache_trim` lets them go, so their next call copies them in if
there is room by then. The image is the records of every function, then
the cell each one starts at, then a trailer of eight words: magic,
version, the build's tag, the module's CRC-32 and size, the count of
functions, the image's size and its CRC-32. The tag is a CRC-32 of the
handlers' offsets and the flags predecoding follows; with validation,
mapping makes each function's records again and compares them.

Only called functions take RAM:

| Program | All records | Records made |
|---|---|---|
| CoreMark | 16.8 KB | 16.8 KB |
| C program with WASI stdio | 7.2 KB | 4.6 KB |
| Arduino sketch (wasm-rtos test) | 81.8 KB | 69.5 KB |

A cache too small for the program still beats running in place: Mac
CoreMark 2619 with every record, 2045 with a 1-byte cache (only the
functions on the stack keep records), 973 in place.

## Fuel

One unit of fuel is one WebAssembly instruction of the module. The
interpreter counts it itself: programs are not rewritten or instrumented,
so any `.wasm` from any compiler is counted the same way.

Each record pays its fuel before it runs:

```c
if (fuel-- == 0) {
    if (fuel_on) { fuel = 0; stop = wm_fuel_exhausted; goto suspend; }
    fuel = WM_NONE - 1u;   /* fuel off: the counter just goes round */
}
```

So a stop is always between two instructions, and `goto suspend` writes
the whole state (function, position, frame and stack pointers, call depth)
into `rt->saved`, as plain data. With fuel off the counter still runs from
its top, so the hot path has no test for whether fuel is on.

Fused records (`WM_FUSE`: `local.get + i32.const + i32.add` and the like)
pay for every instruction they stand for; without fuel for all of them only
the first runs, and the rest follow one by one. Runs of `nop`, `block`,
`loop` and `end` (`WM_NOP_RUNS`) pay the same way. A record that stops for
a TLB miss gets its fuel back, as it runs again.

Only the outermost call stops (`rt->nesting == 1`): a host function's own
calls back into the program run to the end, as the host's C stack cannot
be suspended.

## Snapshots

A position is an offset in the module and a side table index; functions are
indices; nothing holds a host address. So a snapshot from one build and
chip loads into another: a 64-bit build with records and a 32-bit one
running in place agree. Loading a snapshot of a predecoded module makes the
records once every frame is on the runtime, so making some cannot drop
others the snapshot stopped in. The width of references matters only to
modules that can have them on the value stack, where a function reference
is written as its index, as in globals and tables.

Reading a snapshot, each frame's function is validated again up to the
frame's position (`wm_code_stack`): that says what the stack holds there,
so the frames are checked against their code and each other (decision 11),
and the slots with references are found, before memory, globals or tables
change. A branch lands on its block's `end`, so a frame may stand on one
the code before it never reaches; there the stack is the block's and its
results. A CRC-32 ends the stream; fuzzing builds
(`FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`) skip comparing it.

A state snapshot (`wm_snapshot_write_state`) is all of it but the memory's
bytes, for a program swapped out with its memory left to the pager: the
instance gives the memory up (`wm_instance_detach_memory`), everything else
is freed, and a new instance of the module takes it back
(`wm_instantiate_with_memory`, no data segments, no start function) before
the state loads.

## Paged memory

A memory the heap has no block for is paged instead, in frames of
`WM_FRAME_SIZE` bytes. With a pager a memory is paged too, unless the pager
lets it stay in one block (its `block` callback, from the RAM it has to
spare): then all its frames count as in RAM and are reserved in storage,
and `wm_memory_block_out` sends it to storage whole (frames still all zero
need no copy) and lets it go on paged. `wm_memory_block_in` makes a paged
memory one block again when RAM has room: frames in RAM are copied, those
in storage read back, and their RAM and slots go (as the kernel of an OS
collapses small pages into a huge one).

With a frame pool (`wm_set_frame_pool`, decision 6 in
[decisions.md](decisions.md)) frames of memories with a pager come from a
region set aside at the start, never from the heap, and a block is the
memory's frames in a row in the pool. To make one, the engine picks a
window of the pool that holds nothing that may not move (another block, a
pinned or busy frame, a frame of a memory a call in progress uses), swaps
frames out of the way and the memory's own into place, and reads or zeroes
the rest: no heap and, for frames in RAM, no storage. It keeps for each
frame of the pool which memory and index use it, and moving one updates
that memory's frame table and drops its TLB entry. That keeps the fast path for every
program that fits (Mac CoreMark with a pager: 2343 paged, 2595 in one
block). A memory in one block is accessed as ever, by a bounds check. A paged one goes through a TLB
shared by the memories (each has an address space number in the key), with
16 entries by default; a miss stops the fast loop and `wm_execute` makes or
loads the frame. An access that crosses frames is done through the memory
functions and the loop steps past it.

## dylink.0

`source/wm_dylink.c` loads a position-independent program with the shared
libraries it needs into one runtime, as an ELF process: one linear memory
and one function table, the convention's globals made by the loader. A
function imported from another module is a copy of the target `WmFunc`, so
a call into a library is a call into its code, and fuel and stops work
across modules.

## ESP32

`-DWM_IRAM` places the interpreter loop in IRAM, about 10 KB of heap less.
Code runs from flash through a 32 KB cache, and with the loop in IRAM a
kernel that switches programs often has the cache to itself: on a classic
ESP32 with wasm-rtos, 500 round trips between two programs through a queue
take 27 ms with it and 52 ms without. A program that runs alone gains
nothing (CoreMark 12.97 against 12.96).

Several choices are made per processor, from measurements: fusing pairs
(`WM_FUSE`) and skipping `nop` runs (`WM_NOP_RUNS`) gain on microcontrollers
and lose where branch prediction makes dispatch cheap; numeric helpers are
not inlined on Xtensa (`WM_INLINE_OPS`), where a compact loop leaves the
flash cache room for the kernel.
