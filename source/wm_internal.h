/* Internal structures shared by the loader, the instances and the interpreter. */
#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

#include "wm.h"

#include <stddef.h>
#include <string.h>

#define WM_NONE 0xFFFFFFFFu
#define WM_PAGE_SIZE 65536u
/* An implementation limit: memories bigger than this grow no further (memory.grow returns -1). */
/*
 * Sizes of memories: 64-bit where pointers are, otherwise 32-bit, which
 * caps a memory one page short of 4 GiB.
 */
/* WM_SMALL_SIZES forces the 32-bit form, to test it on a 64-bit computer. */
#if UINTPTR_MAX > 0xFFFFFFFFu && !defined(WM_SMALL_SIZES)
typedef uint64_t wm_size;
#define WM_PAGE_LIMIT 65536u
#else
typedef uint32_t wm_size;
#define WM_PAGE_LIMIT 65535u
#endif
#ifndef WM_MAX_PAGES
#define WM_MAX_PAGES WM_PAGE_LIMIT
#endif

/* Whether count items from start overrun size, without overflow. */
static inline int wm_out_of_bounds(uint32_t start, uint32_t count, wm_size size)
{
    return count > size || start > size - count;
}

/*
 * WM_TINY: the smallest engine that runs most programs, for chips like an
 * Uno's (32 KiB of flash): modules checked before they reach the chip (no
 * validation), no 64-bit integers, no references. 28 KiB on an ATmega328P
 * against 35 KiB. Each setting it implies can still be given.
 */
#ifdef WM_TINY
#ifndef WM_HAS_VALIDATION
#define WM_HAS_VALIDATION 0
#endif
#ifndef WM_HAS_I64
#define WM_HAS_I64 0
#endif
#ifndef WM_HAS_REF_TYPES
#define WM_HAS_REF_TYPES 0
#endif
#ifndef WM_HAS_STORES
#define WM_HAS_STORES 0
#endif
#endif

/*
 * Stores (decision 25): objects in a store's lists, a module's count of
 * its instances. 0 (WM_TINY: one program, no kernel to own processes):
 * the functions of 1.0 alone, nothing of stores in the build.
 */
#ifndef WM_HAS_STORES
#define WM_HAS_STORES 1
#endif

/*
 * Floating point: 1 computes f32 and f64 with the compiler's float and
 * double, which must be IEEE 754 binary32 and binary64. 0 rejects modules
 * that use them. The default is 1 where double has 64 bits (not on AVR).
 */
#ifndef WM_HAS_FLOAT
#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ != 8
#define WM_HAS_FLOAT 0
#else
#define WM_HAS_FLOAT 1
#endif
#endif

/*
 * Validation (WM_HAS_VALIDATION, on by default): loading checks the whole
 * module as the standard requires. Without it the same pass still builds
 * the side table, but the program is trusted: type, alignment and other
 * validity checks are left out, and an invalid program can misbehave.
 * Checks that keep the engine's own arrays in bounds stay.
 */
#ifndef WM_HAS_VALIDATION
#define WM_HAS_VALIDATION 1
#endif

/*
 * What must hold (WM_ASSERT): checked in the test builds (WM_CHECKS=1) and
 * known to the static analyzer; nothing in the others, so it costs a chip
 * neither time nor flash.
 */
#ifndef WM_CHECKS
#define WM_CHECKS 0
#endif
#if WM_CHECKS || defined(__clang_analyzer__)
#include <assert.h>
#define WM_ASSERT(x) assert(x)
#else
#define WM_ASSERT(x) ((void)0)
#endif

/*
 * Traces (WM_HAS_TRACE, off on AVR): after a trap the runtime tells the
 * functions it was in, and functions have the names of the module's name
 * section. It costs no time: the trap's own exit notes where it was.
 */
#ifndef WM_HAS_TRACE
#ifdef __AVR__
#define WM_HAS_TRACE 0
#else
#define WM_HAS_TRACE 1
#endif
#endif

/*
 * Optional parts, on unless turned off: i64 values (WM_HAS_I64) and reference
 * types with the table instructions (WM_HAS_REF_TYPES; call_indirect stays).
 * Modules that need a part left out are refused when they load.
 */
/*
 * Allocation through the host's allocator (WM_HAS_ALLOCATOR, off on AVR,
 * where wm_set_allocator does nothing): otherwise the C library's.
 */
#ifndef WM_HAS_ALLOCATOR
#ifdef __AVR__
#define WM_HAS_ALLOCATOR 0
#else
#define WM_HAS_ALLOCATOR 1
#endif
#endif
/*
 * WM_STORE_BYTES (wm.h): a store counts every byte allocated for its
 * objects (decision 25), for a host that reports them: each allocation then
 * carries the store it is charged to and its size, in a header before it,
 * so freeing it, or growing it, charges the same store whatever is being
 * done when. Needs a host allocator (not on AVR).
 */
#if WM_STORE_BYTES && !WM_HAS_ALLOCATOR
#error "WM_STORE_BYTES needs WM_HAS_ALLOCATOR"
#endif

struct WmStore;
#if WM_HAS_ALLOCATOR
void* wm_malloc(size_t size);
void* wm_calloc(size_t count, size_t size);
void* wm_realloc(void* ptr, size_t size);
void wm_free(void* ptr);
#else
#include <stdlib.h>
#define wm_malloc malloc
#define wm_calloc calloc
#define wm_realloc realloc
#define wm_free free
#endif
#if WM_STORE_BYTES
/*
 * Allocations charged to store (NULL: the engine's, charged to none).
 * wm_malloc and wm_calloc charge wm_charge, NULL but while a module is
 * loaded for a store; wm_realloc and wm_free keep the store an allocation
 * was made for. Only what goes with the store, or before it, is charged to
 * it: charging NULL is always safe.
 */
void* wm_store_malloc(struct WmStore* store, size_t size);
void* wm_store_calloc(struct WmStore* store, size_t count, size_t size);
extern struct WmStore* wm_charge;
#else
#define wm_store_malloc(store, size) ((void)(store), wm_malloc(size))
#define wm_store_calloc(store, count, size) ((void)(store), wm_calloc((count), (size)))
#endif
/*
 * Whether a memory or table of store may grow by more bytes: the host is
 * asked (wm_set_store_growing) with what the store's memories and tables
 * then hold. Yes for an object of no store, or with no host function set:
 * the check is linked only into a build that sets one.
 */
#if WM_HAS_STORES
extern int (*wm_store_check)(struct WmStore* store, uint8_t kind, size_t more);
#define wm_store_may_grow(store, kind, more) \
    (!(store) || !wm_store_check || wm_store_check((store), (kind), (more)))
#else
#define wm_store_may_grow(store, kind, more) ((void)(store), (void)(kind), (void)(more), 1)
#endif

#ifndef WM_HAS_I64
#define WM_HAS_I64 1
#endif
#ifndef WM_HAS_REF_TYPES
#define WM_HAS_REF_TYPES 1
#endif

/*
 * Speed against size. WM_INLINE_OPS builds the per-type numeric functions
 * into the interpreter loop. Not on AVR, where one copy is smaller, nor on
 * Xtensa: there code runs from flash through a small cache, and a compact
 * loop leaves room for the kernel's code (programs that talk through the
 * kernel ran 1.7 times faster; CoreMark lost 1%). A port can set WM_HOT to
 * put the interpreter in fast memory (IRAM on ESP32).
 */
#ifndef WM_INLINE_OPS
#if defined(__AVR__) || defined(__XTENSA__)
#define WM_INLINE_OPS 0
#else
#define WM_INLINE_OPS 1
#endif
#endif
#if !defined(WM_HOT) && defined(ESP_PLATFORM) && defined(WM_IRAM)
#define WM_HOT __attribute__((section(".iram1.wm")))
#endif
#ifndef WM_HOT
#define WM_HOT
#endif
#define WM_NOINLINE __attribute__((noinline))
#define WM_ALWAYS_INLINE inline __attribute__((always_inline))
/*
 * Short immediates are read in the instruction's own code where calls cost
 * much (Xtensa); elsewhere the compiler decides, which measured faster.
 */
#ifndef WM_IMM
#if defined(__XTENSA__)
#define WM_IMM inline __attribute__((always_inline))
#else
#define WM_IMM
#endif
#endif
/*
 * The loop in place fusing the commonest instruction pairs saves a dispatch
 * but costs a test: a gain on microcontrollers, a loss where branch
 * prediction makes dispatch cheap (desktop and server processors), so it
 * is off there.
 */
#ifndef WM_FUSE
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
#define WM_FUSE 0
#else
#define WM_FUSE 1
#endif
#endif
/*
 * WM_PROFILE: count what runs, for tools/profile.py (a desktop build, off
 * by default): every record by kind and every pair of records in a row in
 * the fast loop (and the opcodes its shared numeric and access records
 * run), every opcode in the loop in place. At exit the counts go
 * to the file WM_PROFILE_FILE names, the process id after it, if it is set.
 */
#ifndef WM_PROFILE
#define WM_PROFILE 0
#endif
/* Computed goto for the interpreter's dispatch, where labels can be values. */
#ifndef WM_COMPUTED_GOTO
#if defined(__GNUC__) && !defined(__AVR__)
#define WM_COMPUTED_GOTO 1
#else
#define WM_COMPUTED_GOTO 0
#endif
#endif
#if WM_INLINE_OPS
#define WM_OP static inline __attribute__((always_inline))
#else
#define WM_OP static __attribute__((noinline))
#endif

/*
 * Error messages. Terse errors (the default on AVR, where string constants
 * take RAM) keep only the kind: "trap", "invalid" and so on.
 */
#ifndef WM_TERSE_ERRORS
#ifdef __AVR__
#define WM_TERSE_ERRORS 1
#else
#define WM_TERSE_ERRORS 0
#endif
#endif

#if WM_TERSE_ERRORS
extern const char wm_error_malformed[];
extern const char wm_error_invalid[];
extern const char wm_error_unlinkable[];
extern const char wm_error_trap[];
extern const char wm_error_exhaustion[];
extern const char wm_error_unsupported[];
#define WM_ERR_MALFORMED(text) wm_error_malformed
#define WM_ERR_INVALID(text) wm_error_invalid
#define WM_ERR_UNLINKABLE(text) wm_error_unlinkable
#define WM_ERR_TRAP(text) wm_error_trap
#define WM_ERR_EXHAUSTION(text) wm_error_exhaustion
#define WM_ERR_UNSUPPORTED(text) wm_error_unsupported
#else
#define WM_ERR_MALFORMED(text) text
#define WM_ERR_INVALID(text) text
#define WM_ERR_UNLINKABLE(text) text
#define WM_ERR_TRAP(text) text
#define WM_ERR_EXHAUSTION(text) text
#define WM_ERR_UNSUPPORTED(text) text
#endif

/* Read-only tables: in flash on AVR, read with WM_ROM8 and WM_ROM32. */
#ifdef __AVR__
#include <avr/pgmspace.h>
#define WM_ROM PROGMEM
#define WM_ROM8(p) pgm_read_byte(p)
#define WM_ROM32(p) pgm_read_dword(p)
#else
#define WM_ROM
#define WM_ROM8(p) (*(p))
#define WM_ROM32(p) (*(p))
#endif

/* All reads of module bytes go through this, so code can later live where plain loads cannot reach. */
#if defined(__AVR__) && defined(WM_PROGRAM_IN_FLASH)
/* The program's bytes in flash (PROGMEM): names handed to resolvers are in flash too. */
#define WM_RD8(p) pgm_read_byte(p)
#else
#define WM_RD8(p) (*(const uint8_t*)(p))
#endif

/* Slots of a reference: as wide as a pointer. */
#ifndef WM_REF_SLOTS
#define WM_REF_SLOTS ((uint32_t)((sizeof(uintptr_t) + 3u) / 4u))
#endif

/* WebAssembly's opcodes by name, and those after 0xFC (wm_defs.def). */
enum
{
#define WM_WASM(OP, NAME, name) WM_OP_##NAME = OP,
#define WM_WASM_ACCESS(OP, NAME, name, VALUE, SIZE, STORE, SIGNED) WM_OP_##NAME = OP,
#define WM_WASM_NUM(OP, NAME, name, OPERAND, ARITY, RESULT) WM_OP_##NAME = OP,
#include "wm_defs.def"
};
enum
{
#define WM_WASM_FC(SUB, NAME, name) WM_FC_##NAME = SUB,
#include "wm_defs.def"
};
/* Where the memory accesses and the numeric instructions start, unsigned as an index from an opcode is. */
#define WM_FIRST_ACCESS ((uint32_t)WM_OP_I32_LOAD)
#define WM_FIRST_NUMERIC ((uint32_t)WM_OP_I32_EQZ)

/* Block kinds while validating. */
#define WM_BLOCK 0x02
#define WM_LOOP 0x03
#define WM_IF 0x04
#define WM_ELSE 0x05
#define WM_FUNCTION 0x00

/* Side entry fields: 16 bits on AVR, where programs stay below 64 KiB, saving half the RAM. */
#ifdef __AVR__
typedef uint16_t wm_side_t;
#else
typedef uint32_t wm_side_t;
#endif
#define WM_SIDE_MAX ((wm_side_t)~(wm_side_t)0)
/* Counts of parameters, results and their slots: 16 bits on AVR too. */
typedef wm_side_t wm_count_t;

typedef struct WmType
{
    const uint8_t* params;
    const uint8_t* results;
    wm_count_t param_count;
    wm_count_t result_count;
    wm_count_t param_slots;
    wm_count_t result_slots;
    /* The first type of the module equal to it: functions point there, so the same type is the same pointer. */
    wm_count_t canon;
} WmType;

/* A module's type by index, as functions point to it. */
#define WM_TYPE(m, index) (&(m)->types[(m)->types[index].canon])

/*
 * One branch-like instruction's entry: where execution goes when it is
 * taken (pc as an offset in the module's bytes, st the side table index
 * there), and how the operand stack changes: the top keep slots stay,
 * the drop slots under them go. drop and untyped select use keep for the
 * size of their operand.
 */

typedef struct WmSide
{
    wm_side_t pc;
    wm_side_t st;
    wm_side_t keep;
    wm_side_t drop;
} WmSide;

/*
 * Predecoding (WM_PREDECODE): at its first call a function is also written
 * as cells (the code cache, wm_code.c), one record per WebAssembly instruction: the address of the code
 * that runs it, then its immediates decoded and its branch targets
 * resolved. Records match instructions one to one, so fuel and snapshot
 * positions stay those of the module's own instructions. It needs labels
 * as values, and RAM: off on AVR.
 */
/*
 * Paged memory: a memory the heap has no block for is paged instead, in
 * frames of WM_FRAME_SIZE bytes, a power of two from 256 to 65536 (4096 by
 * default); with a pager every memory is, and frames go to storage and
 * back. WM_FRAME_SIZE 0 (the default on AVR) leaves a memory always in one
 * block, one frame as large as the memory: no paging is built in at all
 * (WM_HAS_MMU follows from it). WM_MEMORY_ALWAYS_PAGED pages every memory,
 * for tests.
 */
#ifdef WM_HAS_MMU
#error "WM_HAS_MMU is set by WM_FRAME_SIZE now: 0 for memory in one block, or the frame size in bytes"
#endif
#ifndef WM_FRAME_SIZE
#ifdef __AVR__
#define WM_FRAME_SIZE 0
#else
#define WM_FRAME_SIZE 4096
#endif
#endif
#if WM_FRAME_SIZE == 0
#define WM_HAS_MMU 0
#else
#define WM_HAS_MMU 1
#if WM_FRAME_SIZE == 256
#define WM_FRAME_LOG2 8u
#elif WM_FRAME_SIZE == 512
#define WM_FRAME_LOG2 9u
#elif WM_FRAME_SIZE == 1024
#define WM_FRAME_LOG2 10u
#elif WM_FRAME_SIZE == 2048
#define WM_FRAME_LOG2 11u
#elif WM_FRAME_SIZE == 4096
#define WM_FRAME_LOG2 12u
#elif WM_FRAME_SIZE == 8192
#define WM_FRAME_LOG2 13u
#elif WM_FRAME_SIZE == 16384
#define WM_FRAME_LOG2 14u
#elif WM_FRAME_SIZE == 32768
#define WM_FRAME_LOG2 15u
#elif WM_FRAME_SIZE == 65536
#define WM_FRAME_LOG2 16u
#else
#error "WM_FRAME_SIZE is 0 or a power of two from 256 to 65536"
#endif
#endif
#ifndef WM_MEMORY_ALWAYS_PAGED
#define WM_MEMORY_ALWAYS_PAGED 0
#endif

#ifndef WM_PREDECODE
#if WM_COMPUTED_GOTO && !defined(__AVR__)
#define WM_PREDECODE 1
#else
#define WM_PREDECODE 0
#endif
#endif
/*
 * The engine knows every runtime and, while calls are in progress, the
 * functions that called host functions: the code cache (decision 3) and
 * the frame pool (decision 6) leave alone what those calls use.
 */
#define WM_TRACKS_CALLERS (WM_PREDECODE || WM_HAS_MMU)

/*
 * A predecoded cell: 32 bits on every target. A record's first cell holds
 * its handler, as an offset from the first handler, in the low 16 bits and
 * a small operand in the high 16; the cells after it hold what does not fit.
 */
typedef union WmCell
{
    uint32_t u32;
    int32_t s32;
} WmCell;


/*
 * The register loop (decision 16): what the loop in place needs to run part of a
 * function that runs in the register loop, where fuel ends inside a
 * segment or a snapshot stopped: the function's side table entries (its
 * own, from 0), and the places the register loop can take over again.
 */
typedef struct WmTailEntry
{
    uint32_t offset;
    uint32_t cell;
    /* Side table entries of the instructions before it. */
    uint16_t sides;
    /* The stack's top there, fp-relative. */
    uint16_t top;
    /* A call or return: the loop in place has charged its fuel. */
    uint8_t charge;
} WmTailEntry;
typedef struct WmTail
{
    WmSide* side;
    WmTailEntry* entries;
    uint32_t count, capacity;
    /*
     * The entries are made: a function that ran in place by itself (decision
     * 21) has its side table alone, until part of it runs in place inside
     * the register loop; the side table stays where it is.
     */
    uint8_t translated;
} WmTail;

typedef struct WmCode
{
    const uint8_t* body;
    /* The function's final end. */
    const uint8_t* end;
    uint32_t side;
    uint32_t local_count;
    uint32_t local_slots;
    uint32_t max_stack;
    /* Slot offset of each local, then the total; NULL when every local takes one slot. */
    uint32_t* local_offsets;
    /*
     * Predecoded records, or NULL: made at the first call, dropped when the
     * code cache needs room (wm_code.c). They are the cache's own (own, the
     * same records) unless mapped: then they are in the module's code image.
     */
    union
    {
        const WmCell* records;
        WmCell* own;
    };
    /* At most 0x7FFF: a longer function is not predecoded. */
    uint16_t record_cells;
    /* For the code cache's CLOCK: called since the hand last passed; pinned (a count): on a call stack, or being made. */
    uint8_t used;
    uint8_t pinned;
    /* fast points into the code image: nothing to free, not counted in the cache. */
    uint8_t mapped;
    /* What the loop in place needs to run part of it (wm_reg_tail), or NULL. */
    WmTail* tail;
} WmCode;

typedef struct WmLimits
{
    uint32_t min;
    uint32_t max;
    uint8_t has_max;
    /* Memories only: log2 of the page size, 16 or (custom page sizes) 0. */
    uint8_t page_log2;
} WmLimits;

typedef struct WmTableType
{
    uint8_t type;
    WmLimits limits;
} WmTableType;

typedef struct WmGlobalType
{
    uint8_t type;
    uint8_t mutable_;
    /* Constant expression, NULL for imports. */
    const uint8_t* init;
} WmGlobalType;

typedef struct WmImport
{
    const uint8_t* module;
    const uint8_t* name;
    uint32_t module_length;
    uint32_t name_length;
    uint8_t kind;
    /* Index in the kind's index space. */
    uint32_t index;
} WmImport;

typedef struct WmExport
{
    const uint8_t* name;
    uint32_t name_length;
    uint8_t kind;
    uint32_t index;
} WmExport;

#define WM_SEGMENT_ACTIVE 0
#define WM_SEGMENT_PASSIVE 1
#define WM_SEGMENT_DECLARATIVE 2

typedef struct WmElemSegment
{
    uint8_t mode;
    uint8_t type;
    /* Items are constant expressions rather than function indices. */
    uint8_t expressions;
    uint32_t table;
    const uint8_t* offset;
    uint32_t count;
    const uint8_t* items;
} WmElemSegment;

typedef struct WmDataSegment
{
    uint8_t mode;
    const uint8_t* offset;
    const uint8_t* data;
    uint32_t size;
} WmDataSegment;

/*
 * A store (decision 25): what a process made, freed with it. Each kind of
 * object in its own list, newest first; tables and globals made alone are
 * nodes around the object (WmStoreTable, WmStoreGlobal), so the ones an
 * instance embeds grow by nothing.
 */
struct WmStoreTable;
struct WmStoreGlobal;
struct WmStore
{
    void* host;
    struct WmInstance* instances;
    struct WmMemory* memories;
    struct WmRuntime* runtimes;
    struct WmModule* modules;
    struct WmStoreTable* tables;
    struct WmStoreGlobal* globals;
#if WM_STORE_BYTES
    size_t bytes;
    size_t peak;
    /* Freed while bytes were still charged to it: kept until they are, so nothing points at a store that is gone. */
    uint8_t freed;
#endif
};

struct WmModule
{
    const uint8_t* bytes;
    uint32_t size;
    /* The store it was loaded for (its own), or NULL: the engine's, shared by any. */
    struct WmStore* store;
    struct WmModule* store_next;
    /* Its instances, in any store; told to unused when the last goes. */
    uint32_t instance_count;
    void (*unused)(void* context, struct WmModule* module);
    void* unused_context;

    WmType* types;
    uint32_t type_count;
    WmImport* imports;
    uint32_t import_count;

    /* Type index of every function, imported ones first. */
    uint32_t* func_types;
    uint32_t func_count;
    uint32_t func_imports;
    WmCode* codes;

    WmTableType* tables;
    uint32_t table_count;
    uint32_t table_imports;
    WmLimits memory;
    uint32_t memory_count;
    uint32_t memory_imports;
    WmGlobalType* globals;
    uint32_t global_count;
    uint32_t global_imports;

    WmExport* exports;
    uint32_t export_count;
    uint32_t start;
    WmElemSegment* elems;
    uint32_t elem_count;
    WmDataSegment* datas;
    uint32_t data_count;

    WmSide* side;
    uint32_t side_count;
#if WM_HAS_TRACE
    /* The name section's contents, after its name: offset in bytes (0 if there is none) and size. */
    uint32_t names;
    uint32_t names_size;
#endif
    /* References can be on the value stack (in signatures, locals or operands): its layout follows WM_REF_SLOTS. */
    uint8_t stack_refs;
    uint8_t predecode_tried;
    /* Why predecoding failed and the functions run in place; NULL when it worked. */
    WmResult predecode_result;
    /* The code section's first entry: functions are validated again from here (wm_code_side, wm_code_stack). */
    const uint8_t* code_entries;
#if WM_PREDECODE
    /* The next module in the code cache, when this one runs predecoded. */
    struct WmModule* cached_next;
    /* The code image the host mapped (wm_code_image_map), or NULL: its records, and where each function's start, then the end. */
    const WmCell* image;
    const uint32_t* image_index;
#endif
};

struct WmFunc
{
    /* NULL for host functions. */
    WmInstance* instance;
    WmCode* code;
    /* Its module's type (WM_TYPE), or a host function's own, after it in the same block. */
    const WmType* type;
    /* The type's, here as every call needs them. */
    wm_count_t param_slots;
    wm_count_t result_slots;
    WmHostFunction host;
    void* user;
};

struct WmGlobal
{
    uint8_t type;
    uint8_t mutable_;
    /* Made alone in a store: inside a WmStoreGlobal (in padding, no room taken). */
    uint8_t in_store;
    uint64_t value;
};

typedef struct WmStoreGlobal
{
    struct WmStore* store;
    struct WmStoreGlobal* store_next;
    struct WmGlobal global;
} WmStoreGlobal;

/* Elements are references: an WmFunc pointer for funcref, the host's value for externref, 0 for null. */
struct WmTable
{
    uint8_t type;
    /* Made alone in a store: inside a WmStoreTable (in padding, no room taken). */
    uint8_t in_store;
    /* The store whose host is asked before it grows: its own, or its instance's; NULL for none. */
    struct WmStore* store;
    uint32_t size;
    uint32_t max;
    uintptr_t* elems;
};

typedef struct WmStoreTable
{
    struct WmStore* store;
    struct WmStoreTable* store_next;
    struct WmTable table;
} WmStoreTable;

struct WmMemory
{
    /* The memory in one block; NULL when it is paged. */
    uint8_t* data;
    /* Its store, or NULL: an instance's own or the caller's (the functions of 1.0). */
    struct WmStore* store;
    struct WmMemory* store_next;
    uint32_t pages;
    uint32_t max_pages;
    uint8_t page_log2;
#if WM_HAS_MMU
    /* Its address space number in TLB keys, 1..WM_SPACES-2; WM_SPACE_UNCACHED when none was free (never cached). */
    uint16_t space;
    /* Paged: a 4 KiB frame per slot (a NULL slot is made when touched). */
    uint8_t** frames;
    uint32_t frame_count;
    /* A former one-block memory: its first block_frames frames are in it. */
    uint8_t* block;
    uint32_t block_frames;
    /*
     * Swapped by a pager: frames are made when touched, and a frame the host
     * took out is WM_FRAME_OUT. slots has the pager's slot of each frame
     * (WM_NONE: none), which keeps a copy of a clean frame in RAM too. marks
     * has 16 bits per frame: touched, dirty, read or written in the
     * background, and a pin count. next links the memories with a pager.
     */
    const struct WmPager* pager;
    uint16_t* marks;
    uint32_t* slots;
    /* Each frame's place in the pager's ring of frames in RAM (WM_NONE: not in it). */
    uint32_t* ring_place;
    struct WmMemory* next;
    /*
     * Counts what may have changed its bytes (a run of its instance's code,
     * a host write, growing): a memory in one block has no dirty frames, so
     * its first copied frames have their copy in storage only while changes
     * is still copied_at (wm_memory_clean).
     */
    uint32_t changes;
    uint32_t copied_at;
    uint32_t copied;
#endif
};

#if WM_HAS_MMU
/* A memory's bytes may change: the copies wm_memory_clean made of a memory in one block may not hold. */
#define WM_MEMORY_CHANGES(m) ((m) ? (void)++((WmMemory*)(m))->changes : (void)0)
#else
#define WM_MEMORY_CHANGES(m) ((void)0)
#endif

#if WM_HAS_MMU
#define WM_FRAME (1u << WM_FRAME_LOG2)
/* The address bits that name a frame. */
#define WM_FRAME_MASK ((uint32_t)~(WM_FRAME - 1u))
/*
 * Address space numbers fit the offset bits a frame address leaves free,
 * from bit 3 (an access's alignment bits are below). Two are kept back:
 * 0, which empty TLB entries carry, and WM_SPACE_UNCACHED, which a paged
 * memory gets when the others are all taken, and which no entry ever
 * carries: every key the fast loop makes can be a real one, so neither may
 * belong to a memory the TLB caches. That leaves 510 with 4 KiB frames, 30
 * with 256 bytes; a paged memory beyond them still works, without the TLB.
 */
#define WM_SPACE_BITS (WM_FRAME_LOG2 - 3u < 9u ? WM_FRAME_LOG2 - 3u : 9u)
#define WM_SPACES (1u << WM_SPACE_BITS)
#define WM_SPACE_UNCACHED (WM_SPACES - 1u)

/*
 * A TLB, as a CPU core has one: frames of paged memories, each entry tagged
 * with its memory's address space number in the key bits from 3 up, which
 * no frame address sets. An empty entry's tag is 0, which no key is, as
 * address space numbers start at 1. A runtime uses one; by default all share
 * wm_tlb_default, and a host that runs programs on several threads gives
 * each its own (wm_tlb_init).
 */
#ifndef WM_TLB_ENTRIES
#define WM_TLB_ENTRIES 16u
#endif
#define WM_TLB_EMPTY 0u

/*
 * tag lets loads through, wtag stores: a frame a pager may store is entered
 * for stores only once it is dirty, so the first store to a clean frame
 * misses and marks it.
 */
typedef struct WmTlbEntry
{
    uint32_t tag;
    uint32_t wtag;
    /* The host address of an address ea in the frame is ea + addend, as in QEMU's TLB: no masking on a hit. */
    uintptr_t addend;
} WmTlbEntry;

typedef struct WmTlb
{
    WmTlbEntry entries[WM_TLB_ENTRIES];
    struct WmTlb* next;
} WmTlb;

extern WmTlb wm_tlb_default;
void wm_tlb_init(WmTlb* tlb);
/*
 * A TLB miss on a paged memory: the frame of the access made and put in the
 * TLB (NULL: run it again), wm_page_emulate when the TLB cannot hold it
 * (unaligned, across frames, no address space number), or a trap.
 */
WmResult wm_page_fault(WmTlb* tlb, WmMemory* memory, uint32_t address, uint32_t offset, uint32_t size, int write);
extern const char wm_page_emulate[];
/* One byte of a memory, reading an untouched frame as zero. */
uint8_t wm_memory_byte(WmMemory* memory, uint32_t address);
#endif

/* memory.fill and memory.copy, one block or paged. */
WmResult wm_memory_fill(WmMemory* memory, uint32_t address, uint8_t value, uint32_t length);
WmResult wm_memory_copy(WmMemory* memory, uint32_t to, uint32_t from, uint32_t length);

/* A memory's size in bytes. */
#define WM_MEMORY_SIZE(mem) ((wm_size)(mem)->pages << (mem)->page_log2)

struct WmInstance
{
    WmModule* module;
    /* Its store, or NULL (wm_instantiate): it owns its memory itself then. */
    struct WmStore* store;
    struct WmInstance* store_next;
    WmFunc** funcs;
    /* The module's own functions, then a stub for each function import nothing was linked to. */
    WmFunc* own_funcs;
    uint32_t own_func_count;
    WmGlobal** globals;
    WmGlobal* own_globals;
    WmTable** tables;
    WmTable* own_tables;
    WmMemory* memory;
    uint8_t owns_memory;
    uint8_t* elem_dropped;
    uint8_t* data_dropped;
    uint8_t started;
};

typedef struct WmFrame
{
    const uint8_t* pc;
    const WmSide* st;
    uint32_t* fp;
    WmFunc* func;
} WmFrame;

/* A host function's caller, kept while the host calls back in (wm_call), so the code cache leaves its records. */
typedef struct WmNest
{
    WmFunc* caller;
    struct WmNest* up;
} WmNest;

/* Where a call stopped early (fuel, or a host function that yielded), to go on from. */
typedef struct WmSuspended
{
    WmFunc* entry;
    uint32_t* base;
    uint32_t entry_depth;
    WmFunc* func;
    const uint8_t* pc;
    const WmSide* st;
    uint32_t* fp;
    uint32_t* sp;
    /*
     * Stopped in the loop in place in a module that runs predecoded: 1 inside
     * a function of the register loop (part of it), 2 in a function that runs
     * in place by itself (decision 21). Its frames' st is not NULL.
     */
    uint8_t inplace;
} WmSuspended;

struct WmRuntime
{
    uint32_t* stack;
    /* Its store, or NULL. */
    struct WmStore* store;
    struct WmRuntime* store_next;
#if WM_HAS_MMU
    struct WmTlb* tlb;
    /* A TLB miss the fast loop stopped for: the access of the record it stopped at. */
    uint32_t fault_address;
    uint32_t fault_offset;
    uint32_t fault_cells;
    uint8_t fault_op;
#endif
    uint32_t* stack_end;
    uint32_t* sp;
    WmFrame* frames;
    uint32_t max_frames;
    uint32_t depth;
    /* Instructions left before the call stops, when fuel_on. */
    uint32_t fuel;
    uint8_t fuel_on;
    /* A host function asked to stop after it returns (wm_yield). */
    uint8_t yielding;
    /* A trap a host function asked for (wm_trap), taken when it returns. */
    WmResult host_trap;
    uint8_t suspended;
    /* Calls in progress: only the outermost one stops for fuel. */
    uint8_t nesting;
#if WM_PREDECODE
    /* Calls may make records (wm_set_makes_code, decision 21); otherwise a function without them runs in place. */
    uint8_t makes_code;
#endif
    WmSuspended saved;
#if WM_TRACKS_CALLERS
    /* Every runtime, for the code cache and the frame pool to see what is on the call stacks. */
    struct WmRuntime* next;
    /*
     * The function that called the running host function (or runs
     * memory.grow, init, copy or fill, which may make frames); the callers
     * of outer ones, while calls nest.
     */
    WmFunc* caller;
    WmNest* nest;
#endif
#if WM_HAS_TRACE
    /* Where the last trap was: its function, with frames[0..trap_depth) the calls it was in. */
    WmFunc* trap_func;
    uint32_t trap_depth;
#endif
};

/*
 * The numeric instructions 0x45-0xC4, one byte each: bits 0-1 the operand
 * type, bit 2 a second operand, bits 3-4 the result type. Types are 0 i32,
 * 1 i64, 2 f32, 3 f64, so bit 3 alone says the result takes two slots.
 */
#define WM_NUM_I32 0u
#define WM_NUM_I64 1u
#define WM_NUM_F32 2u
#define WM_NUM_F64 3u
#define WM_NUM_BINARY 4u
#define WM_NUM_WIDE 8u
extern const uint8_t wm_numeric[128];

/* Predecoding: the handlers' addresses, writing a function's records, and positions both ways. */
/* Each handler's offset from the first, by kind; NULL when they span more than 16 bits can reach. */
const uint16_t* wm_fast_handlers(void);
/* side: the function's side table entries. */
WmResult wm_predecode(WmModule* module, WmCode* code, const WmSide* side);
void wm_module_predecode(WmModule* module);
/* The module's functions run predecoded: their records come from the code cache. */
#define WM_RUNS_FAST(module) ((module)->predecode_tried && !(module)->predecode_result)

/*
 * The code cache (wm_code.c): records made for a function when it is called,
 * dropped by CLOCK when memory runs short or the cache is over its limit,
 * except for functions on a call stack. Making room happens only outside
 * the fast loop, where every running function is known.
 */
WmResult wm_code_load(WmFunc* func);
WmResult wm_code_make(WmModule* module, WmCode* code, const WmSide* side);
WmResult wm_code_side(const WmModule* module, uint32_t index, WmSide** out);
#if WM_PREDECODE
enum
{
#define WM_REC(NAME, name) WM_R_##NAME,
#include "wm_defs.def"
    WM_R_COUNT
};
/* The register loop (decision 16): the stack's height in slots before each instruction, in their order. */
WmResult wm_code_heights(const WmModule* module, uint32_t index, uint16_t* heights);
/* Side table entries of the instructions of code before offset (in the module). */
uint32_t wm_reg_sides_before(const WmModule* module, const WmCode* code, uint32_t offset);
/* The function's tail (WmCode.tail), made if it has none. */
WmResult wm_reg_tail(WmModule* module, WmCode* code, WmTail** out);
/* What the loop in place needs to run all of a function by itself (decision 21): its side table, in its tail. */
WmResult wm_reg_side(WmModule* module, WmCode* code, WmTail** out);
void wm_reg_tail_free(WmTail* tail);
const WmTailEntry* wm_reg_entry(const WmTail* tail, uint32_t offset);
/* The segment start whose SEG is the record at ip of func (its tail made if need be), or NULL. */
const WmTailEntry* wm_reg_segment(WmFunc* func, const WmCell* ip);
const WmTailEntry* wm_reg_at_cell(const WmTail* tail, uint32_t cell);
/* The stack machine's frame at the memory access whose record is at cell; its offset and top. */
WmResult wm_reg_stack_at(WmModule* module, WmCode* code, uint32_t cell, uint32_t* fp, uint32_t* offset, uint32_t* top);
/* The register loop stopped at a segment it has not the fuel for: the loop in place goes on. */
extern const char wm_fuel_short[];
#endif
/* What a function's operand stack and locals hold at one of its instructions (wm_code_stack). */
typedef struct WmCodeStack
{
    /* The side table entry the instruction is at, in the module's table. */
    uint32_t side;
    /* Offset of the instruction before it in the module, WM_NONE for the first. */
    uint32_t prev;
    /* Operand types, the bottom first, and their slots; the locals' types, parameters first (both wm_free). */
    uint8_t* types;
    uint32_t count;
    uint32_t slots;
    uint8_t* locals;
    uint32_t local_count;
} WmCodeStack;
WmResult wm_code_stack(const WmModule* module, uint32_t index, uint32_t offset, WmCodeStack* out);
/* One function's records dropped, by CLOCK, for an allocation the heap refused; 0 when none can go. */
int wm_code_drop_one(void);
/*
 * The code area (decision 18, wm_code.c): records in chunks of the code
 * allocator, moved together when there are holes. A span of cells, or
 * NULL when nothing can make room; one span being written at a time
 * (begin, grow: it may move, take its address back; end: cut to count,
 * its owner takes it, then settle).
 */
WmCell* wm_area_alloc(WmCode* owner, uint32_t cells);
void wm_area_free(WmCell* records);
WmCell* wm_area_begin(uint32_t cells);
WmCell* wm_area_grow(uint32_t cells);
WmCell* wm_area_end(WmCode* owner, uint32_t count);
void wm_area_cancel(void);
void wm_area_settle(void);
void* wm_code_malloc(size_t size);
void* wm_code_realloc(void* ptr, size_t size);
void wm_code_free(void* ptr);
void wm_code_add_module(WmModule* module);
void wm_code_remove_module(WmModule* module);
/* The fast loop stopped at a call into a function without records: the frame is made, the records are not. */
extern const char wm_code_stop[];
/*
 * Decision 21: the function runs in place by itself, its side table made
 * (wm_code_alone); a function of the register loop returned to a caller
 * running in place (wm_code_back). Inside wm_execute only.
 */
extern const char wm_code_alone[];
extern const char wm_code_back[];
int wm_fast_position(const WmModule* module, const WmCode* code, const WmCell* ip, uint32_t* offset, uint32_t* side);

/* Shared helpers. */
/* CRC-32 (the polynomial of zlib and Ethernet) of data added to crc; start from 0xFFFFFFFF and invert at the end. */
uint32_t wm_crc32(uint32_t crc, const void* data, uint32_t size);
int wm_types_equal(const uint8_t* a, uint32_t a_count, const uint8_t* b, uint32_t b_count);
/* Program bytes (read with WM_RD8) against bytes in RAM. */
int wm_bytes_equal(const uint8_t* program, uint32_t program_count, const uint8_t* ram, uint32_t ram_count);
int wm_func_has_type(const WmFunc* f, const WmType* t);
WmResult wm_execute(WmRuntime* runtime, WmFunc* func, int resume);
#if WM_TRACKS_CALLERS
/* Every runtime, newest first, linked through next. */
WmRuntime* wm_runtimes(void);
#endif


/* A store's lists (decision 25): an object joins its store's list, newest first, and leaves it when freed alone. */
#define WM_STORE_LIST(Type, list)                                                                                   \
    static inline void wm_store_link_##list(WmStore* store, Type* object)                                          \
    {                                                                                                               \
        object->store = store;                                                                                      \
        if (store)                                                                                                  \
        {                                                                                                           \
            object->store_next = store->list;                                                                       \
            store->list = object;                                                                                   \
        }                                                                                                           \
    }                                                                                                               \
    static inline void wm_store_unlink_##list(Type* object)                                                        \
    {                                                                                                               \
        Type** at;                                                                                                  \
        if (!object->store)                                                                                         \
            return;                                                                                                 \
        for (at = &object->store->list; *at; at = &(*at)->store_next)                                              \
            if (*at == object)                                                                                      \
            {                                                                                                       \
                *at = object->store_next;                                                                           \
                break;                                                                                              \
            }                                                                                                       \
        object->store = NULL;                                                                                       \
    }
#if WM_HAS_STORES
WM_STORE_LIST(struct WmInstance, instances)
WM_STORE_LIST(struct WmMemory, memories)
WM_STORE_LIST(struct WmRuntime, runtimes)
WM_STORE_LIST(struct WmModule, modules)
WM_STORE_LIST(WmStoreTable, tables)
WM_STORE_LIST(WmStoreGlobal, globals)
#else
/* No stores: every object is of none; the store arguments are NULL. */
#define wm_store_link_instances(store, object) ((void)(store), (void)(object))
#define wm_store_link_memories(store, object) ((void)(store), (void)(object))
#define wm_store_link_runtimes(store, object) ((void)(store), (void)(object))
#define wm_store_link_modules(store, object) ((void)(store), (void)(object))
#define wm_store_link_tables(store, object) ((void)(store), (void)(object))
#define wm_store_link_globals(store, object) ((void)(store), (void)(object))
#define wm_store_unlink_instances(object) ((void)(object))
#define wm_store_unlink_memories(object) ((void)(object))
#define wm_store_unlink_runtimes(object) ((void)(object))
#define wm_store_unlink_modules(object) ((void)(object))
#define wm_store_unlink_tables(object) ((void)(object))
#define wm_store_unlink_globals(object) ((void)(object))
#endif

/* The module's last instance went: its owner is told (it may free it). */
void wm_module_instance_gone(WmModule* module);

#endif
