# Decisions

Why the engine is the way it is: each entry says what was decided, why, and
what was considered instead. New entries start as **Proposed** and are
discussed before any code; once built they say what was measured.

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
- Boards: the ESP32 scenario of a sleeper and a worker, where the block
  could not come back before, must show it back; the same program on an
  ESP32 paged and in one block, to know what the paged path costs there;
  Mac CoreMark the same; ESP32 and Black Pill board checks.
