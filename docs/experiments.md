# Experiments

What was tried on the engine and how it came out, kept or not, so nobody
measures it again without knowing. [decisions.md](decisions.md) says what
the engine is; this file also says what it is not, and why.

A change stays only if it loses on none of the machines: a Mac, an ESP32
(Xtensa), a Black Pill (STM32F411, Cortex-M4), and an Uno for size.
CoreMark is the benchmark unless an entry says otherwise; on a Mac it runs
in slices of fuel, as an operating system runs it. Changes are against
the build before the experiment; ±0 is within noise.

## Summary

| Experiment | Result | ESP32 | Mac | Black Pill |
|---|---|---:|---:|---:|
| **Dispatch** | | | | |
| [Handlers the loop does not need](#handlers-the-loop-does-not-need) | rejected | | −24% | |
| [Fused instruction runs](#fused-instruction-runs) | microcontrollers only | gain | loss | gain |
| [Runs of block, loop, end, nop in one step](#runs-of-block-loop-end-and-nop-in-one-step) | microcontrollers only | gain | −12% | |
| [No records for block and end](#no-records-for-block-and-end) | rejected on paper | | | |
| [Aligning handlers](#aligning-handlers) | rejected | crash / +323 B | | |
| [The two loops as separate .c files](#the-two-loops-as-separate-c-files) | rejected | 2 copies | no inlining | |
| [Both loops generated from one description](#both-loops-generated-from-one-description) | rejected on paper | | | |
| [Fused records chosen by the profile](#fused-records-chosen-by-the-profile) | kept | +2.9% | +4.3% | +3.8% |
| [Forms: operands in the records before an instruction](#forms-operands-in-the-records-before-an-instruction) | kept, 18 of 63 | +3.7% | +2.0% chips | +8.8% |
| [Moves fused with what takes them next](#moves-fused-with-what-takes-them-next) | kept, 11 records | +7.7% | +7.5% chips | +11.0% |
| [Records of their own for 64-bit instructions](#records-of-their-own-for-64-bit-instructions) | rejected by the profile | | | |
| **Stack and registers** | | | | |
| [The top of the stack in a register](#the-top-of-the-stack-in-a-register) | kept | −0.5% | +7.6% | |
| [`misc` taking the stack pointer's address](#misc-taking-the-stack-pointers-address) | fixed | +6.6% | ±0 | |
| [`bytes` and `fuel_on` out of the loop's state](#bytes-and-fuel_on-out-of-the-loops-state) | rejected | code +44% | | |
| [Not reloading the instance after a call](#not-reloading-the-instance-after-a-call) | rejected on paper | | | |
| [Sign extension: shifts or a switch](#sign-extension-shifts-or-a-switch) | switch stays | +0.9% | −2.4% | |
| **Calls inside the loop** | | | | |
| [A call from a handler](#a-call-from-a-handler) | rejected | −15% | | |
| **Memory and paging** | | | | |
| [A window of memory in registers](#a-window-of-memory-in-registers) | rejected | −7.7% | −1% | +2.4% |
| [The paged path: an addend and fused loads](#the-paged-path-an-addend-and-fused-loads) | kept | paged +6.1% | paged +4.6% | |
| [The TLB and its space in the loop's state](#the-tlb-and-its-space-in-the-loops-state) | rejected | block −0.7% | | |
| [A TLB of 16 entries, not 64](#a-tlb-of-16-entries-not-64) | kept | | ±0 | |
| [Every memory paged once there is a pager](#every-memory-paged-once-there-is-a-pager) | replaced | | −10% | |
| [A block gathered from the heap](#a-block-gathered-from-the-heap) | replaced by a pool | no block | | |
| [Dirty frames](#dirty-frames) | kept | SD writes −38% | | |
| **Records and code** | | | | |
| [Records in 32-bit cells](#records-in-32-bit-cells) | kept, a trade | −6% | −11% | −8%, in the OS ×2.06 |
| [All records at load, or none](#all-records-at-load-or-none) | replaced by a cache | 12 → 18 programs | | 6 → 9 programs |
| [A compact loop on the ESP32](#a-compact-loop-on-the-esp32) | kept | queue 89 → 53 ms | | |
| [Records read from flash](#records-read-from-flash) | kept, as decision 15 | −3.1% | | −5.7% |
| [Functions without room sent back to be copied in](#functions-without-room-sent-back-to-be-copied-in) | rejected | −0.9% | | |
| **Checks** | | | | |
| [Writing the fuel back at every trap](#writing-the-fuel-back-at-every-trap) | rejected on paper | | | |
| [Trusting snapshots, with a CRC only](#trusting-snapshots-with-a-crc-only) | rejected | | | |
| [Checking snapshots against the code](#checking-snapshots-against-the-code) | kept | +2.9 KB flash | | +2.0 KB flash |
| **Building** | | | | |
| [ESP32 Arduino's default flags](#esp32-arduinos-default-flags) | flags set | about +20% | | |

## Dispatch

### Handlers the loop does not need

**Rejected** · 2026-11-09

**Tried.** More handlers in the fast loop than a build uses.

| Machine | Result |
|---|---|
| Mac | −24% with handlers that never ran |

**Why.** Branch prediction and the loop's size suffer from every handler
there is. Each handler has to earn its place; a function whose operands do
not fit the records runs in place rather than getting handlers of its own.

### Fused instruction runs

**Kept on microcontrollers only** (`WM_FUSE`) · 2026-11-09

**Tried.** The commonest pairs and runs as one handler (superinstructions):
`local.get` + `i32.const`, a compare + `br_if`, and so on.

| Machine | Result |
|---|---|
| ESP32, Black Pill | faster |
| Mac | slower: dispatch is cheap where branch prediction is good |

**Why.** Off on x86 and 64-bit ARM, on elsewhere.

### Runs of block, loop, end and nop in one step

**Kept on microcontrollers only** (`WM_NOP_RUNS`) · 2026-11-09

**Tried.** One handler skips a whole run of these and pays its fuel at
once; they are about 13% of the instructions run.

| Machine | Result |
|---|---|
| Mac | −12%: the skipped dispatches helped predict the next ones |
| Mac, second try | no gain |

### No records for block and end

**Rejected on paper** · 2026-11-15

**Tried.** Leaving `block` and `end` (9–11% of instructions) without records.

**Why.** Records would no longer follow instructions one to one; fuel and
snapshots depend on that ([decision 2](decisions.md#2-records-follow-instructions-one-to-one)).

### Aligning handlers

**Rejected** · 2026-11-12

**Found.** An ESP32 loop over memory went 6.6% slower after an unrelated
change: 34 of 76 handlers had moved across 4-byte words, and a 3-byte
first instruction across a word costs a cycle on every jump to it.

| Tried | Result |
|---|---|
| `-falign-labels` for the library | crashed with an illegal instruction |
| aligning only the loop's labels | +323 bytes; moves again with the next change |

**Why.** It fixes one build of one toolchain. Builds are compared by
CoreMark and the board checks together, not by one loop
([decision 5](decisions.md#5-no-tuning-of-code-layout-per-processor)).

### The two loops as separate .c files

**Rejected** · 2026-11-15

**Tried.** `wm_exec_fast.c` and `wm_exec_inplace.c` compiled on their own.

| Machine | Result |
|---|---|
| desktop | the shared numeric helpers no longer inline into the loops (`WM_INLINE_OPS`) |
| ESP32 | a copy of them for each loop in flash |

**Kept instead.** `.c.h` files included at the end of `wm_exec.c`, as
CPython's `ceval.c` includes its cases: one unit to the compiler, machine
code the same byte for byte.

### Both loops generated from one description

**Rejected on paper** · 2026-11-15

**Tried.** A generator writing both loops from one description of every
instruction's body, as CPython does.

**Why.** A tool in the build and generated files to keep, for about eighty
records and two stacks of different shape. A list included with the
preprocessor does the part worth doing ([decision 9](decisions.md#9-the-instructions-are-described-once)).

### Fused records chosen by the profile

**Kept** · 2026-11-16

**Tried.** The commonest pairs of plain records in CoreMark's profile
(`tools/profile.py`) as fused records made from `wm_ops.def`: a
`local.tee` and the `i32.const` after it; `local.get` and an 8- or 16-bit
load; `i32.const` and `shl` or `shr_u`. With a constant, `or`, `xor` and
`shr_s` ran too seldom (0.09% at most) and were left out.

| | Before | After |
|---|---:|---:|
| CoreMark records run, as for chips | 20.8 million | 19.5 million (−6.0%) |
| CoreMark records run, plain | 24.7 million | 23.4 million (−5.1%) |
| ESP32 CoreMark, -O2 | 15.41 | 15.86 (+2.9%) |
| ESP32 CoreMark, -Os | 13.77 | 13.88 (+0.8%) |
| Black Pill CoreMark | 7.13 | 7.40 (+3.8%) |
| Mac CoreMark, plain | 2811 | 2932 (+4.3%) |
| Mac CoreMark, as for chips | 2946 | 3019 (+2.5%) |
| ESP32 free heap | 73 556 B | 73 540 B |

**Seen.** In the kernel's test programs the new records barely run
(`tee_const` 0.03% of records, the narrow loads 0.05%): the gain is code
like CoreMark's, with `char` and `short`. Where they do not run they cost
only their place in the loop.

### Forms: operands in the records before an instruction

**Kept, 18 of 63** · 2026-11-16

**Tried.** Nine i32 instructions (add, sub, mul, and, or, xor, shl,
shr_s, shr_u) in seven shapes each, operands from locals and constants,
results into a local. Kept the 18 that run 0.1% or more of the records of
CoreMark or of the kernel's test programs. See
[decision 14](decisions.md#14-operands-in-records-as-overlays-on-them) for
the numbers.

**Left out**, as they ran under 0.1% in both: every shape of `or` and
`xor` but two, `sub` but its constant, `mul` but its constant, every
shape of `shr_s`, and most of the three-address ones (`get_get_mul_set`,
`get_const_shl_set`, …). The most used were `const_and`, `const_add`,
`get_const_add_set` (`i = i + c`), `get_const_add`, `get_add`.

### Moves fused with what takes them next

**Kept** · 2026-11-20

**Tried.** After the forms, 38% of CoreMark's records only moved values or
marked blocks. Counted among pairs whose first record is a move, fused
ones included (the profile's list showed plain pairs only, which made the
rest look spread thin): the ten commonest were 14% of all records. Eleven
records for them, each one line of `wm_ops.def` and one pattern:
`set_get_const_add`, `set_get_get`, `tee_const_shr_u`, `tee_const_xor`,
`tee_load`, `get_tee`, `get_store`, `get_br_table`, and with `WM_FUSE`
`get_set_get_br_if`, `get_const_eq_br_if`, `get_const_ne_if`.

| CoreMark | Before | After |
|---|---:|---:|
| Records run, as for chips | 18.17 million | 15.93 million (−12.4%) |
| ESP32 -O2 | 16.55 | 17.83 (+7.7%) |
| ESP32 -Os | 14.23 | 15.40 (+8.2%) |
| Black Pill | 8.07 | 8.96 (+11.0%) |
| Mac, as for chips | 3109 | 3342 (+7.5%) |
| Mac, plain | 2985 | 3188 (+6.8%) |

**Found.** The kernel's board checks on the ESP32, the guard against a loop
too large for its cache of flash: 19 of 19 pass, CoreMark in the system
15.87 → 17.51, 500 round trips through a queue 110 → 50 ms, `fib(24)` 229
→ 225 ms, 19 programs at once in both. The Uno does not change.

### Records of their own for 64-bit instructions

**Rejected by the profile** · 2026-11-16

**Found.** The fast loop's shared numeric and access records run 0.01%
of CoreMark's records (`i64.store` most of it): records of their own for
64-bit instructions would gain nothing there.

## Stack and registers

### The top of the stack in a register

**Kept** · 2026-11-15

**Tried.** `tos` in a register, its slot written only when the stack goes
to memory, as wasm3's `_r0` and HotSpot's interpreter do.

| Machine | Result (with `misc` returning the stack, against it alone) |
|---|---|
| ESP32 | −0.5%: GCC keeps `tos` in the C stack, too few registers |
| Mac | +7.6% |
| Mac, built as for chips | +14.7% |

**Why kept.** With the rest of [decision 8](decisions.md#8-the-fast-loop-does-what-other-interpreters-do)
the ESP32 went +28.8%.

### `misc` taking the stack pointer's address

**Fixed** · 2026-11-15

**Found.** `misc(..., &sp)` for the rare bulk instructions made the
compiler keep the loop's stack pointer in memory on Xtensa: a store on
every change of it.

| Machine | `misc` returning the stack |
|---|---|
| ESP32 | 12.03 → 12.82 (+6.6%) |
| Mac | ±0 |
| Mac, built as for chips | +1.3% |

### `bytes` and `fuel_on` out of the loop's state

**Rejected** · 2026-11-15

**Tried.** Freeing a register on Xtensa by reading these from the runtime.

| Machine | Result |
|---|---|
| ESP32 | the fuel test reads the runtime in every handler; the loop's code +44% |

### Not reloading the instance after a call

**Rejected on paper** · 2026-11-15

**Why.** A call may grow memory, so memory must be read again after a call
in any case, as in wasm3; what was left to save was a load or two, the
cost of the test for it.

### Sign extension: shifts or a switch

**The switch stays** · 2026-11-15

**Found.** Since the static analyzer's change sign extension is a
`switch` (the shifts could shift by 64 as far as the analyzer knew), and
the ESP32 gives `run_fast` other registers.

| Variant | ESP32 -O2 | ESP32 -Os | Mac | Mac, as for chips |
|---|---:|---:|---:|---:|
| shifts (before the analyzer) | 15.49 | | | |
| `switch` (now) | 15.40 | 13.77 | 2819 | 2930 |
| shifts + `WM_ASSERT(size >= 1 && size < 8)` | 15.54 | 13.78 | 2750 (−2.4%) | 2921 (−0.3%) |
| `(v ^ sign) - sign` | 15.35 | | | |

Mac figures are the mean of four runs each, the shifts slower in all
four. **Why.** No change may lose on one machine to win on another.

## Calls inside the loop

### A call from a handler

**Rejected** · 2026-11-09

**Found.** On an ESP32 any function call in a handler costs the whole
loop, even when it only runs on a TLB miss: Xtensa's windowed calls make
the compiler give the loop worse registers everywhere.

| Variant (ESP32, memory in one block) | CoreMark | Against no MMU |
|---|---:|---:|
| no MMU | 12.29 | |
| a miss handled by a call | 10.49 | −15% |
| the slow path written inside the handlers | 12.06 | −1.9% (larger code) |
| a miss as an exception that stops the loop | 12.19 | −0.8% |

**Kept.** A TLB miss and a call into a function without records stop the
loop like fuel; the instruction runs again. Nothing is called from inside
the loop.

## Memory and paging

### A window of memory in registers

**Rejected** · 2026-11-09

**Tried.** The bounds of the whole memory, or of the last page used, in
registers before the TLB.

| Machine | Whole memory | Last page |
|---|---:|---:|
| Mac | ±0 | −1% |
| Black Pill | +2.4% | |
| ESP32 | −7.7% (the TLB alone −2.4%) | |

**Why.** It loses on the ESP32: too few registers, the window goes to the
stack.

### The paged path: an addend and fused loads

**Kept** · 2026-11-16

**Tried.** A TLB entry holding an addend, as QEMU's does (the host
address is `ea + addend`, no masking on a hit), and the fused
`local.get`-and-load records going through the TLB too instead of leaving
paged memories to the plain records.

| CoreMark | main | First version | Kept |
|---|---:|---:|---:|
| ESP32 -O2, memory in one block | 16.44 | 15.95 | 16.55 |
| ESP32 -Os, memory in one block | 14.09 | | 14.23 |
| ESP32 -O2, every memory paged | 13.39 | 14.11 | 14.21 (+6.1%) |
| Mac plain, one block / paged | 3144 / 2760 | | 3138 / 2886 (+4.6%) |
| Mac as for chips, one block | 3195 | | 3160 (−1.1%) |
| Black Pill, memory in one block | 8.05 | | 8.08 |

**Found.** The first version set the address to NULL and tested it after
the lookup: a test more on every access of a memory in one block, −3% on
ESP32. Kept: the bounds check first and alone, the TLB only in its else.
The Mac as for chips loses 1.1% with the fused loads put back as they were
too, so it is the layout of the loop there; the chips themselves gain.
The x86 runner in CI counts 2.2% more machine instructions each as for
chips (11.83 to 12.09) and 3.3% fewer plain: the same layout.

### The TLB and its space in the loop's state

**Rejected** · 2026-11-16

**Tried.** The TLB pointer and the memory's space in variables of the
fast loop, set where `mem` and `mem_size` are, so an access needs neither
`rt->tlb` nor `inst->memory->space`.

| ESP32 CoreMark | Without | With |
|---|---:|---:|
| -O2, memory in one block | 16.55 | 16.33 (−1.3%) |
| -Os, memory in one block | 14.23 | 14.22 |
| -O2, every memory paged | 14.21 | 14.76 (+3.9%) |

**Why not.** Memory in one block is the common case and loses: two more
values in the loop's state on Xtensa, where its registers are few, as the
window of memory in registers before.

### A TLB of 16 entries, not 64

**Kept** · 2026-11-09

| Machine | 64 entries | 16 entries |
|---|---:|---:|
| Mac | 2408 | 2405 |

16 entries are 128 bytes.

### Every memory paged once there is a pager

**Replaced** · 2026-11-12

| Machine | Paged | One block |
|---|---:|---:|
| Mac | 2343 | 2595 (+10.7%) |

**Kept instead.** Memory in one block while the RAM budget has room
([decision 4](decisions.md#4-memory-in-one-block-while-ram-has-room)), as
an operating system keeps huge pages and splits them under pressure.

### A block gathered from the heap

**Replaced by a pool** · 2026-11-12

**Found.** On an ESP32 with a page store, after paging the heap had
163 KB free but no piece over 59 KB, so a 64 KB memory never went back
into one block.

| Tried | Result |
|---|---|
| write the frames out, free them, read them back | the whole memory written and read each time; still depends on the heap |
| frames from a pool, a block is frames in a row | the block came back in 912 ms |

See [decision 6](decisions.md#6-frames-come-from-a-pool-a-block-is-frames-in-a-row).

### Dirty frames

**Kept** · 2026-11-11

**Tried.** A frame not written since it was read is not written again.

| ESP32 with an SD card, same card state | Before | After |
|---|---:|---:|
| writes | 216 | 134 (−38%) |
| time | 6270 ms | 5854 ms (−7%) |

## Records and code

### Records in 32-bit cells

**Kept, a trade** · 2026-11-09

**Tried.** A record's first cell holds its handler as a 16-bit offset and
a 16-bit operand on every target, instead of a pointer and full operands.

| | Before | After |
|---|---:|---:|
| CoreMark's records | 30.9 KB (61.8 KB on a Mac) | 16.7 KB (−46%) |
| Mac CoreMark | 2940 | 2620 (−11%) |
| ESP32 CoreMark | 13.09 | 12.29 (−6%) |
| Black Pill CoreMark, bare | 6.30 | 5.79 (−8%) |
| Black Pill CoreMark, in the OS | 2.79 (records did not fit) | 5.74 (×2.06) |
| ESP32 programs at once | 10 | 12 |

**Why kept.** On a board the records have to fit, or the program runs in
place, two to three times slower.

### All records at load, or none

**Replaced by a code cache** · 2026-11-12

| | Before | After |
|---|---:|---:|
| RAM for code in the wasm-rtos tests | | 14–36% less |
| ESP32 programs at once | 12 | 18 |
| Black Pill programs at once | 6 | 9 |
| Mac CoreMark | 2619 | 2619 |

Only the module in place is two to three times slower. See
[decision 3](decisions.md#3-code-is-a-cache).

### A compact loop on the ESP32

**Kept** · 2026-11-08

**Found.** Code runs from flash through a 32 KB cache; a larger loop
pushed the kernel's code out of it.

| ESP32, a queue of 500 exchanges | Time |
|---|---:|
| before | 45 ms |
| larger loop | 89 ms |
| compact loop | 53 ms |

### Records read from flash

**Kept, as decision 15** · 2026-11-20

**Tried.** CoreMark's records made in RAM, written to flash, and every
function pointed at its copy there (memory-mapped: an ESP32 app partition,
the Black Pill's last sector), against the same records in RAM, on one
instance in the same run. The loop only reads records, so nothing else
changed.

| CoreMark, 16 220 B of records | RAM | Flash | RAM again |
|---|---:|---:|---:|
| ESP32 -O2 | 16.55 | 16.03 (−3.1%) | 16.55 |
| Black Pill | 8.07 | 7.61 (−5.7%) | 8.07 |

**Found.** Reading records goes through the flash cache (ESP32) or the
flash accelerator (STM32): a few percent, against a factor of two or three
for a function that runs in place. Making CoreMark's records again
(instantiating, which predecodes every function once) takes 277 µs on a
Mac.

### Functions without room sent back to be copied in

**Rejected** · 2026-11-20

**Tried.** The CLOCK hand letting go of functions that ran from the code
image because there was no RAM for them, so that their next call copies
them in if there is room by then.

| ESP32, CoreMark from the image, no RAM for records | CoreMark |
|---|---:|
| sent back by the hand | 15.88 |
| kept in the image until the host trims the cache | 16.02 |

**Found.** With RAM short, every allocation for records sends the hand
round, and it sent those functions back to stop the loop at their next
call, again and again. `wm_code_cache_trim` lets them go instead.

## Checks

### Writing the fuel back at every trap

**Rejected on paper** · 2026-11-15

**Found.** `fuzz_diff`: after a call that trapped, the fuel left differs
between the loops. Both keep it in a register and write it back only
where the program can go on (a stop, a host call); the predecoded loop had
written it at a miss in the code cache, the loop in place had not.

**Why not.** A store on every trap path of the fast loop, for a number no
host needs: a trapped program ends. `wm.h` says it is not defined.

### Trusting snapshots, with a CRC only

**Rejected** · 2026-11-15

**Found.** `fuzz_snapshot`: a frame placed between two instructions
passed the old checks, and the loop in place read past its arrays.

**Why not.** A CRC catches a card that changed a byte, not a snapshot
made to break the engine, and moving programs between devices needs
exactly the second. See [decision 11](decisions.md#11-a-snapshot-is-checked-as-a-module-is).

### Checking snapshots against the code

**Kept** · 2026-11-15

**Tried.** Each frame's function validated again up to the frame's
position: the frames checked against what the code holds there, and the
stack's references found.

| | Before | After |
|---|---:|---:|
| ESP32 flash, loader and snapshots | 19 420 B | 22 371 B |
| Black Pill flash, the same | 18 000 B | 20 040 B |
| funcref on the stack in a fresh instance | use after free | right |

**Found on the way.** A branch lands on its block's `end`, which the code
before it may never reach: the first version refused real snapshots
stopped there (`fuzz_diff` found it in its corpus). At an `end` the stack
is the block's and its results, however it is reached.

**Kept too.** Fuzzing builds skip comparing the CRC
(`FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`, the libFuzzer and OSS-Fuzz
custom), or mutated snapshots would never get past it.

## Building

### ESP32 Arduino's default flags

**Flags set for the engine** · 2026-11-08

**Found.** The Arduino core builds with `-fno-jump-tables` and
`-fno-builtin-memcpy`: the interpreter's switches become chains of
compares, its copies calls.

**Kept.** The engine's files are built with `-fjump-tables
-ftree-switch-conversion` there (about 20% faster; see the README), and
the loop copies with `__builtin_memcpy`, which stays inline.
