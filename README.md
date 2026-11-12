# wasm-rtos-vm

**A WebAssembly interpreter you can stop at any instruction, move to
another chip and continue.** It runs the full WebAssembly 2.0 standard on
microcontrollers down to an 8-bit Arduino Uno, and it is the engine of
[wasm-rtos](https://github.com/wasm-rtos/wasm-rtos).

```c
wm_set_fuel(rt, 10000);                 /* run 10000 instructions */
WmResult r = wm_call(rt, main_func, NULL, NULL);
while (r == wm_fuel_exhausted)          /* stopped between two instructions */
{
    save_or_run_something_else();       /* the state is plain data: snapshot it, swap it out */
    wm_set_fuel(rt, 10000);
    r = wm_resume(rt, NULL);
}
```

## What makes it different

- **The interpreter counts the work itself.** One unit of fuel is one
  WebAssembly instruction of the module. Programs are not rewritten or
  instrumented on a computer first: any `.wasm` from any compiler is
  counted, and the same program uses the same fuel on every device.
- **Every instruction boundary is a safe point.** A call stops when the
  fuel runs out, when a host function asks (`wm_yield`), or when memory it
  touches is still on its way from storage. Everything about the stopped
  program is plain data in the runtime: no host stack, no native state.
- **Portable snapshots.** A stopped program is saved by its position in the
  module, not by host addresses: it continues in another build, on another
  chip, with other pointer sizes (`wm_snapshot_write`, `wm_snapshot_read`).
- **Memory larger than RAM.** A software MMU pages programs' memory in
  frames (4 KiB by default) and a pager of the host's moves them to an SD
  card, SPI RAM, FRAM or flash and back, in the background if it can. A
  program sees nothing but time.
- **Code as a cache.** A function is decoded for the fast loop at its first
  call, and the decoded form is dropped again when memory runs short, so
  only the code that runs takes RAM, and a board too small for all of it
  still runs fast.
- **Small.** On an ATmega328P (Uno): 34.7 KB of flash and 172 B of static
  RAM for the whole standard; less without the parts a program does not
  need. Values live in 32-bit slots, so stacks stay small on 8- and 32-bit
  chips.

How these work inside: [docs/internals.md](docs/internals.md).

## Status

- WebAssembly 2.0 without SIMD: the official test suite (tag `wg-2.0`)
  passes, 27437 checks, also under AddressSanitizer and UBSan.
- Runs on AVR (Arduino Uno), ARM Cortex-M (STM32F411), ESP32, desktop
  computers and in the browser (Emscripten).
- Optional `dylink.0` shared libraries (`wm_dylink.h`): a program and its
  libraries in one runtime, like an ELF process.

## Building

Add the files of `source/` to your project; there is nothing to configure
or generate:

```sh
cc -std=gnu99 -O2 your_main.c source/*.c -lm
```

`wm_dylink.c` is only needed for `dylink.0` programs. On Arduino and
ESP-IDF build the engine's files with `-fjump-tables
-ftree-switch-conversion` (their default turns the interpreter's `switch`
into a chain of comparisons; about 20% faster).

### Options

Defaults are chosen per processor; set any of them with `-D`.

| Option | Default | Effect |
|---|---|---|
| `WM_HAS_FLOAT` | 1 where `double` has 64 bits (not AVR) | f32 and f64; 0 refuses modules that use them |
| `WM_HAS_I64` | 1 | 64-bit integers |
| `WM_HAS_REF_TYPES` | 1 | reference types and table instructions (`call_indirect` stays) |
| `WM_HAS_VALIDATION` | 1 | check modules as the standard requires; 0 trusts them (smaller, faster to load) |
| `WM_HAS_TRACE` | 1 (0 on AVR) | after a trap, the functions it was in, with names (`wm_trace`) |
| `WM_HAS_ALLOCATOR` | 1 (0 on AVR) | allocations through `wm_set_allocator` |
| `WM_PREDECODE` | 1 where labels can be values (not AVR) | the fast loop: 2–3 times faster, costs RAM for decoded code |
| `WM_FRAME_SIZE` | 4096 (0 on AVR) | frame size of paged memory, a power of two from 256 to 65536; 0: memory always in one block, no paging built in |
| `WM_TERSE_ERRORS` | 0 (1 on AVR) | errors keep only their kind (`"trap:"`), saving RAM for strings |
| `WM_IRAM` | off | ESP32: the interpreter loop in IRAM (see [docs/internals.md](docs/internals.md#esp32)) |
| `WM_MAX_PAGES` | 65536 (65535 with 32-bit pointers) | a memory grows no further than this many 64 KiB pages |

A module that needs a part left out is refused when it loads, with an
`unsupported:` error.

Flash and static RAM on AVR (ATmega328P, `tools/avr-size.sh`):

| Build | Flash | RAM |
|---|---|---|
| WebAssembly 2.0 | 34.7 KB | 172 B |
| without i64 and reference types | 31.5 KB | 172 B |
| without validation | 30.8 KB | 172 B |
| without validation, i64 and reference types | 27.5 KB | 172 B |

## Example

A host function, an exported function called in slices of fuel:

```c
#include <stdio.h>
#include <string.h>
#include "wm.h"

static WmResult print(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    printf("%u\n", slots[0]);             /* arguments in slots[0..] */
    return NULL;                          /* NULL: no trap */
}

static WmFunc* g_print;

static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    if (kind == WM_EXTERN_FUNC && name_length == 5 && !memcmp(name, "print", 5))
        out->as.func = g_print;
    out->kind = kind;
    return NULL;
}

int run(const uint8_t* bytes, uint32_t size)
{
    static const uint8_t i32[] = {WM_I32};
    WmModule* module;
    WmInstance* instance;
    WmExtern count;
    WmResult r;
    uint32_t n = 5;

    g_print = wm_host_function_new(i32, 1, NULL, 0, print, NULL);
    WmRuntime* rt = wm_runtime_new(8 * 1024, 64);   /* 8 KiB value stack, 64 calls deep */
    if ((r = wm_module_load(&module, bytes, size)) ||
        (r = wm_instantiate(&instance, module, rt, resolve, NULL)) ||
        (r = wm_instance_export(instance, "count", 5, &count)))
        return printf("%s\n", r), 1;

    wm_set_fuel(rt, 10);
    r = wm_call(rt, count.as.func, &n, NULL);
    while (r == wm_fuel_exhausted)
    {
        wm_set_fuel(rt, 10);
        r = wm_resume(rt, NULL);
    }
    return r != NULL;
}
```

## API reference

Every public function of `wm.h`. Errors are static strings that start with
their kind: `"malformed:"`, `"invalid:"`, `"unlinkable:"`, `"trap:"`,
`"exhaustion:"` or `"unsupported:"`; `NULL` is success.

### Modules and instances

```c
WmResult wm_module_load(WmModule** out, const uint8_t* bytes, uint32_t size);  /* bytes must outlive the module */
void     wm_module_free(WmModule* module);
uint32_t wm_module_code_bytes(const WmModule* module);   /* RAM its code takes besides the bytes */
WmResult wm_module_predecoded(const WmModule* module);   /* NULL: runs in the fast loop; else why not */
int      wm_module_import_type(const WmModule* module, const char* name,
                               const uint8_t** params, uint32_t* param_count,
                               const uint8_t** results, uint32_t* result_count);

typedef WmResult (*WmResolver)(void* context, const char* module, uint32_t module_length,
                               const char* name, uint32_t name_length, uint8_t kind, WmExtern* out);
WmResult wm_instantiate(WmInstance** out, const WmModule* module, WmRuntime* runtime,
                        WmResolver resolver, void* context);   /* runtime NULL: start later */
WmResult wm_instance_start(WmInstance* instance, WmRuntime* runtime);
void     wm_instance_free(WmInstance* instance);
WmResult wm_instance_export(const WmInstance* instance, const char* name, uint32_t length, WmExtern* out);
WmMemory* wm_instance_memory(const WmInstance* instance);
```

### Host functions, globals, tables

```c
typedef WmResult (*WmHostFunction)(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory);
WmFunc*  wm_host_function_new(const uint8_t* params, uint32_t param_count,
                              const uint8_t* results, uint32_t result_count,
                              WmHostFunction function, void* user);
void     wm_host_function_free(WmFunc* func);
void     wm_func_type(const WmFunc* func, const uint8_t** params, uint32_t* param_count,
                      const uint8_t** results, uint32_t* result_count);
uint32_t wm_type_slots(uint8_t type);                    /* 2 for i64 and f64 */

WmGlobal* wm_global_new(uint8_t type, uint8_t mutable_, uint64_t value);
void      wm_global_free(WmGlobal* global);
uint8_t   wm_global_type(const WmGlobal* global);
uint64_t  wm_global_get(const WmGlobal* global);

WmTable* wm_table_new(uint8_t type, uint32_t size, uint32_t max);
void     wm_table_free(WmTable* table);
uint32_t wm_table_grow(WmTable* table, uint32_t delta);
```

### Calls, fuel and stops

```c
WmRuntime* wm_runtime_new(uint32_t stack_bytes, uint32_t max_frames);
void       wm_runtime_free(WmRuntime* runtime);
uint8_t*   wm_runtime_stack(const WmRuntime* runtime, uint32_t* bytes);

WmResult wm_call(WmRuntime* runtime, WmFunc* func, const uint32_t* args, uint32_t* results);
WmResult wm_resume(WmRuntime* runtime, uint32_t* results);   /* go on after a stop */
int      wm_is_suspended(const WmRuntime* runtime);
uint32_t wm_call_depth(const WmRuntime* runtime);

void     wm_set_fuel(WmRuntime* runtime, uint32_t fuel);     /* instructions until the call stops */
void     wm_disable_fuel(WmRuntime* runtime);
uint32_t wm_get_fuel(const WmRuntime* runtime);
int      wm_is_fuel_enabled(const WmRuntime* runtime);

WmResult wm_yield(WmRuntime* runtime);                       /* from a host function: stop after it */
WmResult wm_trap(WmRuntime* runtime, WmResult trap);         /* from a host function: trap after it */

extern const char wm_fuel_exhausted[];   /* a call stopped: out of fuel */
extern const char wm_yielded[];          /* a call stopped: a host function yielded */
extern const char wm_page_wait[];        /* a call stopped: a frame is being read in the background */

uint32_t    wm_trace(const WmRuntime* runtime, WmFunc** out, uint32_t max);   /* after a trap */
const char* wm_func_name(const WmFunc* func, uint32_t* length);
```

### Snapshots

```c
typedef WmResult (*WmSnapshotWrite)(void* context, const void* data, uint32_t size);
typedef WmResult (*WmSnapshotRead)(void* context, void* data, uint32_t size);
WmResult wm_snapshot_write(const WmRuntime* runtime, const WmInstance* instance,
                           WmSnapshotWrite write, void* context);
WmResult wm_snapshot_read(WmRuntime* runtime, WmInstance* instance, WmSnapshotRead read, void* context);

/* All but the memory's bytes, for a program whose memory stays with a pager: */
WmResult  wm_snapshot_write_state(const WmRuntime* runtime, const WmInstance* instance,
                                  WmSnapshotWrite write, void* context);
WmMemory* wm_instance_detach_memory(WmInstance* instance);
WmResult  wm_instantiate_with_memory(WmInstance** out, const WmModule* module, WmMemory* memory,
                                     WmResolver resolver, void* context);
extern const char wm_snapshot_invalid[];
```

### Memory

```c
WmMemory* wm_memory_new(uint32_t pages, uint32_t max_pages);
WmMemory* wm_memory_new_paged(uint32_t pages, uint32_t max_pages, uint8_t page_log2);
void      wm_memory_free(WmMemory* memory);
uint32_t  wm_memory_grow(WmMemory* memory, uint32_t delta);
uint8_t*  wm_memory_data(const WmMemory* memory, uint32_t* size);   /* NULL when paged */
WmResult  wm_memory_read(const WmMemory* memory, uint32_t address, void* out, uint32_t length);
WmResult  wm_memory_write(WmMemory* memory, uint32_t address, const void* data, uint32_t length);
WmResult  wm_memory_pointer(WmMemory* memory, uint32_t address, uint32_t length, uint8_t** out);
WmResult  wm_memory_pin(WmMemory* memory, uint32_t address, uint32_t length, int delta);  /* keep in RAM, for DMA */
uint32_t  wm_memory_resident(const WmMemory* memory);   /* bytes in RAM */
uint32_t  wm_frame_size(void);
```

### Swapping memory to storage

```c
typedef struct WmPager
{
    void* context;
    int      (*reserve)(void* context, int32_t frames);       /* room for more frames in storage? */
    int      (*room)(void* context, int again);               /* make room in RAM for a frame */
    WmResult (*write)(void* context, uint32_t* slot, const uint8_t* frame);
    WmResult (*read)(void* context, uint32_t slot, uint8_t* frame);
    void     (*drop)(void* context, uint32_t slot);
    /* optional, for storage that works in the background: */
    WmResult (*read_start)(void* context, uint32_t slot, uint8_t* frame, WmMemory* memory, uint32_t index);
    WmResult (*finish)(void* context, uint32_t slot);
    WmResult (*write_start)(void* context, uint32_t* slot, const uint8_t* frame, WmMemory* memory, uint32_t index);
} WmPager;

void      wm_set_pager(const WmPager* pager);
WmMemory* wm_memory_next(WmMemory* after);
int       wm_pager_hand(WmMemory** memory, uint32_t* index);   /* CLOCK over frames in RAM */
uint32_t  wm_memory_frames(const WmMemory* memory);
int       wm_memory_frame_look(WmMemory* memory, uint32_t index);
WmResult  wm_memory_frame_take(WmMemory* memory, uint32_t index);
void      wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result);
void      wm_memory_frame_stored(WmMemory* memory, uint32_t index, WmResult result);
uint32_t  wm_pager_resident(void);
uint32_t  wm_memory_stored(const WmMemory* memory);
```

### Allocation and the code cache

```c
typedef void* (*WmAllocator)(void* ptr, size_t size);   /* realloc; size 0 frees */
void   wm_set_allocator(WmAllocator allocator);
void   wm_set_code_allocator(WmAllocator allocator);    /* for decoded code, shared by all programs */
void   wm_code_cache_limit(size_t bytes);               /* 0: as much as memory allows */
size_t wm_code_cache_bytes(void);
size_t wm_code_cache_trim(size_t keep);
```

### dylink.0 (`wm_dylink.h`)

```c
int          wm_module_is_dylink(const WmModule* module);
WmResult     wm_dylink_load(WmDylink** out, const WmModule* program, const char* name,
                            WmRuntime* runtime, const WmDylinkOptions* options);
WmInstance*  wm_dylink_instance(const WmDylink* process);
void         wm_dylink_free(WmDylink* process);
```

The comments in `wm.h` and `wm_dylink.h` give the details of each call.

## Tests

[docs/testing.md](docs/testing.md): the spec suite, fuel, snapshots
between builds, swapping, the code cache and the wasm-rtos kernel's tests,
all run by `tools/check.sh`.

## License

Public domain (see `LICENSE`).
