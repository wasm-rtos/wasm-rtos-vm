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
typedef struct WmStore WmStore;

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
/* Allocate as the engine does, through that allocator (as realloc; size 0 frees): for a host's linker, counted alike. */
void* wm_allocate(void* ptr, size_t size);

/*
 * Decode and validate. The bytes must stay valid while the module is used.
 * Loaded with no store the module is the engine's: any number of stores
 * make instances of it, and it is freed by whoever loaded it, after its
 * last instance (wm_module_on_unused says when).
 */
WmResult wm_module_load(WmModule** out, const uint8_t* bytes, uint32_t size);
void wm_module_free(WmModule* module);
/* Bytes the module's code takes besides the module itself: side table, predecoded records, local offsets. */
uint32_t wm_module_code_bytes(const WmModule* module);
/*
 * NULL when the module's functions run predecoded, the fast way (done when
 * its first instance is made); otherwise why they run in place, two to
 * three times slower: built without it, no memory for the records, or an
 * interpreter too large for them (as UBSan makes it). Worth a warning.
 */
WmResult wm_module_predecoded(const WmModule* module);

/*
 * The code cache: a predecoded function's records are made when it is first
 * called and dropped when they are needed for room: when an allocation for
 * records fails, when the cache is over its limit, or by
 * wm_code_cache_trim. Functions on a call stack keep theirs. One cache for
 * every module and runtime, used from one thread at a time.
 *
 * The records come from allocator when one is set (NULL: the engine's, see
 * wm_set_allocator): a host that charges memory to its programs can leave
 * the cache, which they share, out of their accounts.
 */
void wm_set_code_allocator(WmAllocator allocator);
/* Bytes of records the cache may hold before it drops some; 0 (the default): as many as memory allows. */
void wm_code_cache_limit(size_t bytes);
/* Bytes of records it holds. */
size_t wm_code_cache_bytes(void);
/* Bytes the area holding the records takes from the code allocator, holes and unused tops included (decision 18). */
size_t wm_code_area_bytes(void);
/*
 * Drop records until at most keep bytes are left, or only those of
 * functions on a call stack; the bytes left. Not from inside the engine's
 * calls other than a host function's.
 */
size_t wm_code_cache_trim(size_t keep);

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
 * What a module imports or exports, for a host that links modules itself
 * (decision 23). Names, signatures and section bytes point into the
 * module's bytes: they are read as those are (WM_RD8 where a program is
 * in program memory) and live as long as they do.
 */
typedef struct WmExternType
{
    /* Imports only: the module named; NULL for an export. */
    const char* module;
    uint32_t module_length;
    const char* name;
    uint32_t name_length;
    uint8_t kind;
    /* In its kind's index space: an import's comes before the module's own. */
    uint32_t index;
    /* Functions: the signature. */
    const uint8_t* params;
    const uint8_t* results;
    uint32_t param_count;
    uint32_t result_count;
    /* Tables: the element type; globals: the value type, and whether it is mutable. */
    uint8_t type;
    uint8_t mutable_;
    /* Tables and memories: the limits; page_log2 for memories, 16 or 0 (custom page sizes). */
    uint32_t min;
    uint32_t max;
    uint8_t has_max;
    uint8_t page_log2;
} WmExternType;

/*
 * The imports and exports, in the module's order, as WebAssembly's JS API
 * gives them (Module.imports, Module.exports): how many there are, and out
 * filled when index is below that.
 */
uint32_t wm_module_imports(const WmModule* module, uint32_t index, WmExternType* out);
uint32_t wm_module_exports(const WmModule* module, uint32_t index, WmExternType* out);
/*
 * Every memory and table the module has, in their index space: the
 * imported ones first, with the names they are imported by, then its own
 * (module and name NULL).
 */
uint32_t wm_module_memories(const WmModule* module, uint32_t index, WmExternType* out);
uint32_t wm_module_tables(const WmModule* module, uint32_t index, WmExternType* out);
/*
 * The custom sections of that name (Module.customSections): how many there
 * are, and the contents of the one at index, after its name, when index is
 * below that.
 */
uint32_t wm_module_custom_sections(const WmModule* module, const char* name, uint32_t name_length, uint32_t index,
                                   const uint8_t** bytes, uint32_t* size);

/*
 * Stores (decision 25), as wasm-c-api's wasm_store_t: the owner of what a
 * process makes. Instances, memories, tables, globals, runtimes and modules
 * made in a store go with it, in whatever order they were made and linked;
 * one can still be freed alone with its own free function. What is made
 * with the functions without a store (1.0's) belongs to none and keeps
 * their rules. host is the owner's, for the owner's own use.
 */
WmStore* wm_store_new(void* host);
void wm_store_free(WmStore* store);
void* wm_store_host(const WmStore* store);
/*
 * Bytes allocated for the store's objects, and the most it has held: its
 * instances, memories (their frames in RAM from the heap, not from a frame
 * pool), tables, globals, runtimes, modules loaded for it, and what a
 * linker allocates for it (wm_store_allocate). What is the engine's is
 * charged to no store: modules loaded with no store, predecoded code, host
 * functions. 0 in builds without a host allocator (AVR).
 */
size_t wm_store_bytes(const WmStore* store);
size_t wm_store_peak_bytes(const WmStore* store);
/*
 * At most bytes charged to the store (0: no limit). An allocation over it
 * fails as one does when the heap is full: a module does not load or
 * instantiate, memory.grow gives -1. The other stores go on.
 */
void wm_store_limit(WmStore* store, size_t bytes);
/* As wm_allocate, charged to store: for a linker's own records, freed before the store or with wm_allocate. */
void* wm_store_allocate(WmStore* store, void* ptr, size_t size);
/*
 * Everything in the store but its memories goes: instances, tables,
 * globals, runtimes, modules loaded for it. The memories stay, their frames
 * where they are (a swapped-out task leaves them to the pager), for
 * instances made again on them (wm_instance_new_with_memory).
 */
void wm_store_release(WmStore* store);
/* The store's memories, oldest first: NULL past the last. */
WmMemory* wm_store_memory(const WmStore* store, uint32_t index);

/* A module for this store alone, freed with it. */
WmResult wm_module_load_in(WmModule** out, WmStore* store, const uint8_t* bytes, uint32_t size);
/*
 * Call unused(context, module) each time the module's last instance goes,
 * in any store: its owner may free it then, or keep it for the next. NULL
 * calls nothing.
 */
void wm_module_on_unused(WmModule* module, void (*unused)(void* context, WmModule* module), void* context);
/* Instances of the module now, in every store and none. */
uint32_t wm_module_instance_count(const WmModule* module);

/*
 * An instance in store, as wm_instantiate makes one: imports, segments, the
 * start function on runtime (NULL: wm_instance_start later). A memory the
 * module defines is made in the store, and stays there when the instance
 * goes. A failure once imports are linked leaves *out in the store.
 */
WmResult wm_instance_new(WmInstance** out, WmStore* store, WmModule* module, WmRuntime* runtime,
                         WmResolver resolver, void* context);
/*
 * An instance in store on memory, one of the store's (wm_store_memory), as
 * wm_instantiate_with_memory: no data segments written into it, no start
 * function; a module that imports its memory has memory bound to that
 * import. The memory stays the store's.
 */
WmResult wm_instance_new_with_memory(WmInstance** out, WmStore* store, WmModule* module, WmMemory* memory,
                                     WmResolver resolver, void* context);
WmMemory* wm_memory_new_in(WmStore* store, uint32_t pages, uint32_t max_pages, uint8_t page_log2);
WmTable* wm_table_new_in(WmStore* store, uint8_t type, uint32_t size, uint32_t max);
WmGlobal* wm_global_new_in(WmStore* store, uint8_t type, uint8_t mutable_, uint64_t value);
WmRuntime* wm_runtime_new_in(WmStore* store, uint32_t stack_bytes, uint32_t max_frames);

/*
 * Instantiate: imports, segments, then the start function, run on runtime.
 * With runtime NULL the start function waits for wm_instance_start, so
 * imports can be linked first. A failure once the imports are linked (a
 * segment out of bounds, a trap in the start function) leaves *out set:
 * the instance's functions may already be in imported tables, so it stays
 * as long as those do, and is then freed with wm_instance_free.
 */
WmResult wm_instantiate(WmInstance** out, WmModule* module, WmRuntime* runtime, WmResolver resolver,
                        void* context);
WmResult wm_instance_start(WmInstance* instance, WmRuntime* runtime);
void wm_instance_free(WmInstance* instance);
/*
 * Keep an instance's memory past it: the instance no longer owns it, and
 * freeing the instance leaves it (NULL when it owns none, as an instance in
 * a store never does: its memory is the store's). A new instance
 * of the same module takes it back with wm_instantiate_with_memory, which
 * writes no data segments into it and runs no start function: the program
 * goes on from a state snapshot (wm_snapshot_write_state). On failure the
 * memory stays the caller's. A module that imports its memory has the
 * memory given here bound to that import instead (the resolver is not asked
 * for it), with no data segments written either, and the memory stays the
 * caller's: a library rebuilt on a program's memory kept apart (decision 24).
 */
WmMemory* wm_instance_detach_memory(WmInstance* instance);
WmResult wm_instantiate_with_memory(WmInstance** out, WmModule* module, WmMemory* memory, WmResolver resolver,
                                    void* context);
WmResult wm_instance_export(const WmInstance* instance, const char* name, uint32_t length, WmExtern* out);
/*
 * Bind a function import the resolver left unresolved (index in the
 * function index space) to a host function or a function of another
 * instance, of the import's type: calls go straight into it, and table
 * entries made of the import see it. A function import still unlinked is
 * refused as the target.
 */
WmResult wm_instance_link_function(WmInstance* instance, uint32_t index, WmFunc* func);
WmMemory* wm_instance_memory(const WmInstance* instance);

/*
 * Host functions get the arguments in slots and leave the results there,
 * from slots[0]. memory is the calling instance's (NULL without one).
 */
typedef WmResult (*WmHostFunction)(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory);

WmFunc* wm_host_function_new(const uint8_t* params, uint32_t param_count, const uint8_t* results,
                             uint32_t result_count, WmHostFunction function, void* user);
void wm_host_function_free(WmFunc* func);
/*
 * Whether two functions are the same one: the same object, or an import
 * wm_instance_link_function bound and its target (the import is a copy of
 * it), as a table entry made of either is.
 */
int wm_func_equal(const WmFunc* a, const WmFunc* b);
void wm_func_type(const WmFunc* func, const uint8_t** params, uint32_t* param_count, const uint8_t** results,
                  uint32_t* result_count);

WmGlobal* wm_global_new(uint8_t type, uint8_t mutable_, uint64_t value);
void wm_global_free(WmGlobal* global);
uint8_t wm_global_type(const WmGlobal* global);
uint64_t wm_global_get(const WmGlobal* global);
/* For the host: a linker fills an immutable global it made before the module that imports it runs. */
void wm_global_set(WmGlobal* global, uint64_t value);

WmTable* wm_table_new(uint8_t type, uint32_t size, uint32_t max);
void wm_table_free(WmTable* table);
/* Grow by delta entries (null): the old size, or 0xFFFFFFFF when it cannot grow. */
uint32_t wm_table_grow(WmTable* table, uint32_t delta);
/* Table.length, get and set of WebAssembly's JS API, for funcref tables; get gives NULL for null. */
uint32_t wm_table_size(const WmTable* table);
WmFunc* wm_table_get(const WmTable* table, uint32_t index);
WmResult wm_table_set(WmTable* table, uint32_t index, WmFunc* func);

WmMemory* wm_memory_new(uint32_t pages, uint32_t max_pages);
/* page_log2 16 for 64 KiB pages, 0 for 1-byte pages (custom page sizes). */
WmMemory* wm_memory_new_paged(uint32_t pages, uint32_t max_pages, uint8_t page_log2);
void wm_memory_free(WmMemory* memory);
uint32_t wm_memory_grow(WmMemory* memory, uint32_t delta);
/*
 * The memory's bytes in one block, and its size; NULL when it is paged (the
 * heap had no block that large), and then wm_memory_read and
 * wm_memory_write reach it. For reading: a host that writes a memory with
 * a pager writes through wm_memory_write or wm_memory_pointer, which count
 * the change (wm_memory_clean).
 */
uint8_t* wm_memory_data(const WmMemory* memory, uint32_t* size);
/* Copy bytes out of a memory or into it, one block or paged: a trap error when they are out of bounds. */
WmResult wm_memory_read(WmMemory* memory, uint32_t address, void* out, uint32_t length);
WmResult wm_memory_write(WmMemory* memory, uint32_t address, const void* data, uint32_t length);
/*
 * The length bytes at address as one piece of host memory, for a host that
 * works on them in place (a port reading a buffer, DMA): always in a
 * one-block memory, in a paged one when they lie in one frame or in the
 * block it grew out of. *out is NULL when they are split across frames
 * (copy them with wm_memory_read and wm_memory_write instead), and the
 * result is a trap error when they are out of bounds. The pointer holds
 * until the memory grows.
 */
WmResult wm_memory_pointer(WmMemory* memory, uint32_t address, uint32_t length, uint8_t** out);
/* The bytes of a memory in RAM: all of a one-block memory, the frames of a paged one in RAM. */
uint32_t wm_memory_resident(const WmMemory* memory);

/*
 * Swapping (not on AVR). A host with storage for frames sets a pager before
 * it makes memories. Then every memory is paged and takes its frames only
 * when they are touched, as RAM a cache of what the memory holds: the host
 * may take a frame out of RAM into its storage (wm_memory_frame_take), and
 * a touch brings it back. Nothing is promised that cannot be kept: making
 * or growing a memory first reserves its frames with the host, which says
 * yes only for as many as its storage holds.
 *
 * A frame keeps its slot from when it is first stored until its memory
 * goes, so a frame that came back and was not written since (clean, as a
 * store to it was never let through) leaves RAM without being written
 * again, and one that was never written leaves as zeros.
 */
typedef struct WmPager
{
    void* context;
    /* Room for frames more (fewer when negative) in storage: nonzero says yes. */
    int (*reserve)(void* context, int32_t frames);
    /*
     * A frame is about to come into RAM: the host takes another out first
     * if it keeps RAM to a budget, and again (again nonzero) each time the
     * heap had no room for it. 0: it cannot make room.
     */
    int (*room)(void* context, int again);
    /*
     * Store a frame's bytes. *slot is the frame's slot from before
     * (0xFFFFFFFF: none); the pager may write over it or choose another,
     * which it puts in *slot, and the old one is then its to reuse.
     */
    WmResult (*write)(void* context, uint32_t* slot, const uint8_t* frame);
    WmResult (*read)(void* context, uint32_t slot, uint8_t* frame);
    /* The frame with this slot went (its memory was freed): the slot is free. */
    void (*drop)(void* context, uint32_t slot);
    /*
     * Optional, for storage that reads in the background: start reading a
     * frame a program touched (frame index of memory). NULL: read already;
     * wm_page_wait: started, and the program's call stops with
     * wm_page_wait, to go on (wm_resume, the access runs again) once the
     * host has said wm_memory_frame_loaded. finish waits for a read in
     * flight to end, for anyone else who needs the frame; its result is the
     * read's, and no wm_memory_frame_loaded follows.
     */
    WmResult (*read_start)(void* context, uint32_t slot, uint8_t* frame, WmMemory* memory, uint32_t index);
    WmResult (*finish)(void* context, uint32_t slot);
    /*
     * Optional, for storage that writes in the background: start writing a
     * dirty frame that leaves RAM (choosing *slot as write does). NULL:
     * written already; wm_page_wait: started, and the frame stays in RAM,
     * out of the TLB, until the host says wm_memory_frame_stored; a touch
     * meanwhile keeps it in RAM. finish waits for such a write too.
     */
    WmResult (*write_start)(void* context, uint32_t* slot, const uint8_t* frame, WmMemory* memory, uint32_t index);
    /*
     * Optional: may a memory of this many frames more stay in RAM in one
     * block? Nonzero: yes. A memory in one block is reached as one without a
     * pager is, at full speed; all its frames count as in RAM, and its
     * storage is reserved as for any other. It leaves RAM whole, through
     * wm_memory_block_out, and goes on paged. NULL: every memory is paged.
     */
    int (*block)(void* context, uint32_t frames);
} WmPager;
/* The pager memories made from now on use; NULL: none, memories stay in RAM. */
void wm_set_pager(const WmPager* pager);
/*
 * The region frames of memories with a pager come from: frames of
 * wm_frame_size() bytes each, as many as the host keeps in RAM for them
 * (NULL: frames from the heap). Set it with the pager, before memories are
 * made. Frames then never cut up the heap, and a memory in one block is its
 * frames in a row in the pool: the engine moves frames, between calls, to
 * make room for a block (not those of a memory a call in progress uses, nor
 * pinned ones).
 */
void wm_set_frame_pool(uint8_t* memory, uint32_t frames);
/* Paged memories with a pager, one after another from NULL. */
WmMemory* wm_memory_next(WmMemory* after);
/*
 * The pager's CLOCK hand: the next frame in RAM, going round the frames in
 * RAM of all memories with a pager (only those, however much is in
 * storage). 0 when none is in RAM. The host looks at the frame and may take
 * it out (wm_memory_frame_look, wm_memory_frame_take).
 */
int wm_pager_hand(WmMemory** memory, uint32_t* index);
/* A memory's frames, of wm_frame_size() bytes each. */
uint32_t wm_memory_frames(const WmMemory* memory);
/*
 * Whether frame index may be taken out of RAM: -1 when it is not in RAM or
 * must stay (pinned, in the block the memory grew out of), else 1 when it
 * was touched since the last look and 0 when not. Looking clears the mark,
 * so the next touch sets it again (a CLOCK hand).
 */
int wm_memory_frame_look(WmMemory* memory, uint32_t index);
/*
 * Take frame index out of RAM into the pager's storage: NULL when it is
 * out, wm_page_wait when it is being written in the background (its RAM
 * goes once the write ends).
 */
WmResult wm_memory_frame_take(WmMemory* memory, uint32_t index);
/*
 * Keep the frames of a range in RAM (delta 1) while the host works on them
 * through a pointer, or let them go again (delta -1).
 */
WmResult wm_memory_pin(WmMemory* memory, uint32_t address, uint32_t length, int delta);
/* Frames in RAM across all memories with a pager. */
uint32_t wm_pager_resident(void);
/* The size of a frame in bytes, as built (WM_FRAME_SIZE); 0 when memory is always one block. */
uint32_t wm_frame_size(void);
/* A background read the pager started has ended: the frame is in RAM (result NULL) or back in storage. */
void wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result);
/* A background write the pager started has ended: the frame's RAM goes (result NULL), or it stays, dirty. */
void wm_memory_frame_stored(WmMemory* memory, uint32_t index, WmResult result);
/* The bytes of a memory that are only in the pager's storage, not in RAM. */
uint32_t wm_memory_stored(const WmMemory* memory);
/* Frame index's slot in the pager's storage, or 0xFFFFFFFF: none (never stored, or in a block). */
uint32_t wm_memory_frame_slot(const WmMemory* memory, uint32_t index);
/*
 * Forget frame index's slot: the frame comes into RAM (if it was out), the
 * pager drops the slot, and the frame counts as dirty, so it is written to
 * a new slot when it leaves again. For a host that empties one of its
 * stores. NULL when done or when the frame had no slot.
 */
WmResult wm_memory_frame_unstore(WmMemory* memory, uint32_t index);
/*
 * Write one more frame of a memory with a pager to storage ahead of need,
 * keeping it in RAM, so that it leaves RAM later with no write: the next
 * frame of a memory in one block not copied since its bytes last could
 * change (a run of its code, a host write), or a dirty frame of a paged
 * memory, which is clean then. A frame all zero is not written, its slot
 * let go. *out_written is 1 when a frame was written, 0 when all have their
 * copy. For the host between runs, one frame at a time, for memories of
 * programs that will not run soon (as Linux writes back dirty pages ahead).
 */
WmResult wm_memory_clean(WmMemory* memory, uint32_t* out_written);
/* The frames of a memory with a pager that are in RAM in one block (see block in WmPager); 0 when it is paged. */
uint32_t wm_memory_block_frames(const WmMemory* memory);
/*
 * A memory in one block leaves RAM whole: its frames go to the pager's
 * storage (those still all zero need none), the block is freed and the
 * memory goes on paged, its frames coming back when touched. NULL when it
 * is done or there was no block; an error (the block stays) when a write
 * failed or a host has a range of it pinned. Not for the memory of a
 * program that is running (its calls hold the block's address): from the
 * host between calls, or while another program's call is in a host
 * function or the pager.
 */
WmResult wm_memory_block_out(WmMemory* memory);
/*
 * A paged memory with a pager goes back into one block, reached at full
 * speed again: its frames in RAM are copied in, those in storage read back
 * (an untouched one is zero), and their RAM and storage slots go; its
 * frames stay reserved. The host calls it when RAM has room for the block
 * (all its frames then count in RAM). NULL when done or it was a block
 * already; wm_page_wait when a frame is on its way in or out in the
 * background or pinned (try later); an error when the heap has no block
 * that large or a read failed, and the memory stays paged. Not for the
 * memory of a program that is running, as wm_memory_block_out.
 */
WmResult wm_memory_block_in(WmMemory* memory);

/* An execution stack: stack_bytes for values and locals, max_frames calls deep. */
WmRuntime* wm_runtime_new(uint32_t stack_bytes, uint32_t max_frames);
void wm_runtime_free(WmRuntime* runtime);
/* The value stack: bytes long, growing up from the start. A host may fill it to measure how much is used. */
uint8_t* wm_runtime_stack(const WmRuntime* runtime, uint32_t* bytes);

/*
 * Call with arguments in slots; results are written to results, in slots.
 * Returns wm_fuel_exhausted, wm_yielded or wm_page_wait when the call
 * stopped early: it is then suspended where it stood, and wm_resume goes
 * on from there.
 */
WmResult wm_call(WmRuntime* runtime, WmFunc* func, const uint32_t* args, uint32_t* results);
WmResult wm_resume(WmRuntime* runtime, uint32_t* results);
int wm_is_suspended(const WmRuntime* runtime);
/* Calls in progress below the current one: deep inside a recursion, many. */
uint32_t wm_call_depth(const WmRuntime* runtime);

/*
 * Fuel: one unit is one WebAssembly instruction. With fuel set, a call
 * stops before the instruction that finds none left. A host function may
 * set it to 0 to stop the program once it returns. After a call that
 * trapped, the fuel left is not defined: the loops keep it in a register
 * and write it back only where the program can go on.
 */
void wm_set_fuel(WmRuntime* runtime, uint32_t fuel);
/*
 * Whether calls on this runtime may make records (decision 21); 1 by
 * default. With 0 they use the records other runtimes made and run the
 * rest in place, taking no memory and no time from the others for code.
 * The same results and fuel either way. Can change at any time, also from
 * a host function or between slices of a suspended call; takes effect at
 * the next call of a function without records. Does nothing in builds
 * without predecoding, where everything runs in place.
 *
 * Whatever this says, a function whose records find no room runs in place
 * by itself instead of failing the call.
 */
void wm_set_makes_code(WmRuntime* runtime, int makes);
void wm_disable_fuel(WmRuntime* runtime);
uint32_t wm_get_fuel(const WmRuntime* runtime);
int wm_is_fuel_enabled(const WmRuntime* runtime);

/* From a host function: stop the program once this call returns, its results still to come. */
WmResult wm_yield(WmRuntime* runtime);
/*
 * From a host function: end the program's call with trap once the host
 * function returns, whatever it returns (the first trap said counts). For
 * an error in the program's memory the host met on the program's behalf,
 * such as a frame its pager could not bring back, which the program would
 * have trapped on itself. Returns trap.
 */
WmResult wm_trap(WmRuntime* runtime, WmResult trap);

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
/* The call stopped for a frame the pager reads in the background; wm_resume once it is loaded. */
extern const char wm_page_wait[];

/*
 * Snapshots of a suspended call, portable between builds and chips: they
 * load into a fresh runtime and an instance of the same module, which then
 * goes on with wm_resume. The streams return NULL or an error.
 */
typedef WmResult (*WmSnapshotWrite)(void* context, const void* data, uint32_t size);
typedef WmResult (*WmSnapshotRead)(void* context, void* data, uint32_t size);
WmResult wm_snapshot_write(const WmRuntime* runtime, const WmInstance* instance, WmSnapshotWrite write, void* context);
/*
 * A state snapshot: all of it but the memory's bytes, which stay in the
 * memory. It loads only with the same memory, kept apart from the
 * instance (wm_instance_detach_memory) and given to the new one
 * (wm_instantiate_with_memory): a program swapped out, its memory left
 * to the pager.
 */
WmResult wm_snapshot_write_state(const WmRuntime* runtime, const WmInstance* instance, WmSnapshotWrite write,
                                 void* context);
WmResult wm_snapshot_read(WmRuntime* runtime, WmInstance* instance, WmSnapshotRead read, void* context);
extern const char wm_snapshot_invalid[];

/*
 * Code images (decision 15): a module's predecoded records, written once to
 * storage the processor reads as memory (internal flash, PSRAM) and mapped
 * from there. A function whose records are not in RAM then runs from the
 * image instead of being validated and predecoded again: the code cache
 * copies records in from it while there is room and drops them back to it.
 * A function that found no room runs from the image until the cache is
 * trimmed (wm_code_cache_trim), when RAM may have room again.
 * An image fits only the module and the build that wrote it; anything
 * else is refused with wm_code_image_invalid and the module goes on
 * without it (write a new one).
 *
 * wm_code_image_write: through write as a snapshot is, one function in RAM
 * at a time; refused for a module that cannot be predecoded.
 * wm_code_image_map: image 4-byte aligned, valid until wm_module_free;
 * before the first instance it also saves predecoding every function to
 * start. With validation each function's records are made again and
 * compared, so an image is never trusted unchecked.
 */
WmResult wm_code_image_write(WmModule* module, WmSnapshotWrite write, void* context);
WmResult wm_code_image_map(WmModule* module, const void* image, uint32_t size);
extern const char wm_code_image_invalid[];

#ifdef __cplusplus
}
#endif

#endif
