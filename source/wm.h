/*
 * wasm-rtos-vm core: a small in-place WebAssembly interpreter.
 *
 * Modules run from their bytes as loaded: nothing is compiled. Loading
 * decodes and validates the whole module once and builds a side table
 * with the target of every branch, so execution never searches the code.
 * Values live in 32-bit slots (i64 and f64 take two), which keeps stacks
 * small on 8- and 32-bit chips.
 *
 * Errors are static strings that start with their kind: "malformed:",
 * "invalid:", "unlinkable:", "trap:" or "exhaustion:". NULL is success.
 */
#ifndef WM_H
#define WM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const char* WmResult;

typedef struct WmModule WmModule;
typedef struct WmInstance WmInstance;
typedef struct WmRuntime WmRuntime;
typedef struct WmFunc WmFunc;
typedef struct WmGlobal WmGlobal;
typedef struct WmTable WmTable;
typedef struct WmMemory WmMemory;

/* Value types, as encoded in the binary format. */
#define WM_I32 0x7F
#define WM_I64 0x7E
#define WM_F32 0x7D
#define WM_F64 0x7C
#define WM_FUNCREF 0x70
#define WM_EXTERNREF 0x6F

/* Import and export kinds. */
#define WM_EXTERN_FUNC 0
#define WM_EXTERN_TABLE 1
#define WM_EXTERN_MEMORY 2
#define WM_EXTERN_GLOBAL 3

typedef struct WmExtern
{
    uint8_t kind;
    union
    {
        WmFunc* func;
        WmTable* table;
        WmMemory* memory;
        WmGlobal* global;
    } as;
} WmExtern;

/* Slots one value of a type takes: 2 for i64 and f64, references as wide as a pointer. */
uint32_t wm_type_slots(uint8_t type);

/*
 * Every allocation the engine makes goes through allocator when one is set
 * (NULL: the C library): it works as realloc, and a size of 0 frees ptr.
 * Set it before anything is created; a host can count memory with it.
 */
typedef void* (*WmAllocator)(void* ptr, size_t size);
void wm_set_allocator(WmAllocator allocator);

/* Decode and validate. The bytes must stay valid while the module is used. */
WmResult wm_module_load(WmModule** out, const uint8_t* bytes, uint32_t size);
void wm_module_free(WmModule* module);
/* Bytes the module's code takes besides the module itself: side table, predecoded records, local offsets. */
uint32_t wm_module_code_bytes(const WmModule* module);

/*
 * Finds what an import binds to. Returning an error fails the
 * instantiation; leaving a function import unresolved (out->as.func NULL)
 * makes calls to it trap until wm_instance_link_function binds it.
 */
typedef WmResult (*WmResolver)(void* context, const char* module, uint32_t module_length, const char* name,
                               uint32_t name_length, uint8_t kind, WmExtern* out);
/*
 * The type a function import declares, for a resolver that makes the
 * function itself: name is the pointer the resolver was given. 0 when it
 * names no function import of the module.
 */
int wm_module_import_type(const WmModule* module, const char* name, const uint8_t** params, uint32_t* param_count,
                          const uint8_t** results, uint32_t* result_count);

/*
 * Instantiate: imports, segments, then the start function, run on runtime.
 * With runtime NULL the start function waits for wm_instance_start, so
 * imports can be linked first.
 */
WmResult wm_instantiate(WmInstance** out, const WmModule* module, WmRuntime* runtime, WmResolver resolver,
                        void* context);
WmResult wm_instance_start(WmInstance* instance, WmRuntime* runtime);
void wm_instance_free(WmInstance* instance);
WmResult wm_instance_export(const WmInstance* instance, const char* name, uint32_t length, WmExtern* out);
WmMemory* wm_instance_memory(const WmInstance* instance);

/*
 * Host functions get the arguments in slots and leave the results there,
 * from slots[0]. memory is the calling instance's (NULL without one).
 */
typedef WmResult (*WmHostFunction)(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory);

WmFunc* wm_host_function_new(const uint8_t* params, uint32_t param_count, const uint8_t* results,
                             uint32_t result_count, WmHostFunction function, void* user);
void wm_host_function_free(WmFunc* func);
void wm_func_type(const WmFunc* func, const uint8_t** params, uint32_t* param_count, const uint8_t** results,
                  uint32_t* result_count);

WmGlobal* wm_global_new(uint8_t type, uint8_t mutable_, uint64_t value);
void wm_global_free(WmGlobal* global);
uint8_t wm_global_type(const WmGlobal* global);
uint64_t wm_global_get(const WmGlobal* global);

WmTable* wm_table_new(uint8_t type, uint32_t size, uint32_t max);
void wm_table_free(WmTable* table);
/* Grow by delta entries (null), delta pages: the old size, or 0xFFFFFFFF when it cannot grow. */
uint32_t wm_table_grow(WmTable* table, uint32_t delta);

WmMemory* wm_memory_new(uint32_t pages, uint32_t max_pages);
/* page_log2 16 for 64 KiB pages, 0 for 1-byte pages (custom page sizes). */
WmMemory* wm_memory_new_paged(uint32_t pages, uint32_t max_pages, uint8_t page_log2);
void wm_memory_free(WmMemory* memory);
uint32_t wm_memory_grow(WmMemory* memory, uint32_t delta);
uint8_t* wm_memory_data(const WmMemory* memory, uint32_t* size);

/* An execution stack: stack_bytes for values and locals, max_frames calls deep. */
WmRuntime* wm_runtime_new(uint32_t stack_bytes, uint32_t max_frames);
void wm_runtime_free(WmRuntime* runtime);
/* The value stack: bytes long, growing up from the start. A host may fill it to measure how much is used. */
uint8_t* wm_runtime_stack(const WmRuntime* runtime, uint32_t* bytes);

/*
 * Call with arguments in slots; results are written to results, in slots.
 * Returns wm_fuel_exhausted or wm_yielded when the call stopped early: it
 * is then suspended where it stood, and wm_resume goes on from there.
 */
WmResult wm_call(WmRuntime* runtime, WmFunc* func, const uint32_t* args, uint32_t* results);
WmResult wm_resume(WmRuntime* runtime, uint32_t* results);
int wm_is_suspended(const WmRuntime* runtime);
/* Calls in progress below the current one: deep inside a recursion, many. */
uint32_t wm_call_depth(const WmRuntime* runtime);

/*
 * Fuel: one unit is one WebAssembly instruction. With fuel set, a call
 * stops before the instruction that finds none left. A host function may
 * set it to 0 to stop the program once it returns.
 */
void wm_set_fuel(WmRuntime* runtime, uint32_t fuel);
void wm_disable_fuel(WmRuntime* runtime);
uint32_t wm_get_fuel(const WmRuntime* runtime);
int wm_is_fuel_enabled(const WmRuntime* runtime);

/* From a host function: stop the program once this call returns, its results still to come. */
WmResult wm_yield(WmRuntime* runtime);

/*
 * After a call ends in a trap: the functions it was in, innermost first.
 * Returns how many there are and writes the first max of them to out; kept
 * until the runtime's next call. Always 0 without WM_HAS_TRACE.
 */
uint32_t wm_trace(const WmRuntime* runtime, WmFunc** out, uint32_t max);
/* A function's name in its module's name section, length bytes, not terminated; NULL if it has none. */
const char* wm_func_name(const WmFunc* func, uint32_t* length);

extern const char wm_fuel_exhausted[];
extern const char wm_yielded[];

/*
 * Snapshots of a suspended call, portable between builds and chips: they
 * load into a fresh runtime and an instance of the same module, which then
 * goes on with wm_resume. The streams return NULL or an error.
 */
typedef WmResult (*WmSnapshotWrite)(void* context, const void* data, uint32_t size);
typedef WmResult (*WmSnapshotRead)(void* context, void* data, uint32_t size);
WmResult wm_snapshot_write(const WmRuntime* runtime, const WmInstance* instance, WmSnapshotWrite write, void* context);
WmResult wm_snapshot_read(WmRuntime* runtime, WmInstance* instance, WmSnapshotRead read, void* context);
extern const char wm_snapshot_invalid[];

#ifdef __cplusplus
}
#endif

#endif
