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
#define WM_HOT __attribute__((section(".iram1.n3")))
#endif
#ifndef WM_HOT
#define WM_HOT
#endif
#define WM_NOINLINE __attribute__((noinline))
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
 * Fusing the commonest instruction pairs saves a dispatch but costs a test:
 * a gain on microcontrollers, a loss where branch prediction makes dispatch
 * cheap (desktop and server processors), so it is off there.
 */
#ifndef WM_FUSE
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
#define WM_FUSE 0
#else
#define WM_FUSE 1
#endif
#endif
/*
 * Runs of block, loop, end and nop skipped by one handler, their fuel paid
 * at once: fewer dispatches, but on processors with branch prediction those
 * dispatches help predict the next ones, so it is off there.
 */
#ifndef WM_NOP_RUNS
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
#define WM_NOP_RUNS 0
#else
#define WM_NOP_RUNS 1
#endif
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

/* Read-only tables: in flash on AVR, read with WM_ROM8. */
#ifdef __AVR__
#include <avr/pgmspace.h>
#define WM_ROM PROGMEM
#define WM_ROM8(p) pgm_read_byte(p)
#else
#define WM_ROM
#define WM_ROM8(p) (*(p))
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
} WmType;

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
 * Predecoding (WM_PREDECODE): at load each function is also written as
 * cells, one record per WebAssembly instruction: the address of the code
 * that runs it, then its immediates decoded and its branch targets
 * resolved. Records match instructions one to one, so fuel and snapshot
 * positions stay those of the module's own instructions. It needs labels
 * as values, and RAM: off on AVR.
 */
/*
 * Paged memory (WM_HAS_MMU, off on AVR): a memory the heap has no block
 * for is paged instead, 4 KiB frames made when first touched.
 * WM_MEMORY_ALWAYS_PAGED pages every memory, for tests.
 */
#ifndef WM_HAS_MMU
#ifdef __AVR__
#define WM_HAS_MMU 0
#else
#define WM_HAS_MMU 1
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
 * A predecoded cell: 32 bits on every target. A record's first cell holds
 * its handler, as an offset from the first handler, in the low 16 bits and
 * a small operand in the high 16; the cells after it hold what does not fit.
 */
typedef union WmCell
{
    uint32_t u32;
    int32_t s32;
} WmCell;

/* The predecoded records' kinds, each the code of one handler. */
enum
{
    WM_F_UNREACHABLE,
    WM_F_NOP,
    WM_F_IF,
    WM_F_JUMP,
    WM_F_BR,
    WM_F_BR_IF,
    WM_F_BR_TABLE,
    WM_F_RETURN,
    WM_F_CALL,
    WM_F_CALL_INDIRECT,
    WM_F_DROP,
    WM_F_SELECT,
    WM_F_LOCAL_GET1,
    WM_F_LOCAL_SET1,
    WM_F_LOCAL_TEE1,
    WM_F_LOCAL_GET,
    WM_F_LOCAL_SET,
    WM_F_LOCAL_TEE,
    WM_F_GLOBAL_GET,
    WM_F_GLOBAL_SET,
    WM_F_TABLE_GET,
    WM_F_TABLE_SET,
    WM_F_ACCESS,
    WM_F_I32_LOAD,
    WM_F_I32_STORE,
    WM_F_MEMORY_SIZE,
    WM_F_MEMORY_GROW,
    WM_F_CONST16,
    WM_F_CONST32,
    WM_F_CONST64,
    WM_F_NUMERIC,
    WM_F_EQZ,
    WM_F_EQ,
    WM_F_NE,
    WM_F_LT_S,
    WM_F_LT_U,
    WM_F_GT_S,
    WM_F_GT_U,
    WM_F_LE_S,
    WM_F_LE_U,
    WM_F_GE_S,
    WM_F_GE_U,
    WM_F_ADD,
    WM_F_SUB,
    WM_F_MUL,
    WM_F_AND,
    WM_F_OR,
    WM_F_XOR,
    WM_F_SHL,
    WM_F_SHR_S,
    WM_F_SHR_U,
    WM_F_REF_NULL,
    WM_F_REF_IS_NULL,
    WM_F_REF_FUNC,
    WM_F_SAT,
    WM_F_MISC,
    /*
     * Fused: the first record of a common run gets one of these; it runs the
     * whole run when there is fuel for it, else only its own instruction.
     */
    WM_F_GET_CONST_ADD,
    WM_F_GET_GET_ADD,
    WM_F_GET_CONST,
    WM_F_GET_GET,
    WM_F_SET_GET,
    WM_F_CONST_ADD,
    WM_F_CONST_AND,
    WM_F_CONST_SUB,
    WM_F_ADD_SET,
    WM_F_GET_LOAD,
    WM_F_TEE_BR_IF,
    WM_F_SET_GET_BR_IF,
    WM_F_EQ_BR_IF,
    WM_F_NE_BR_IF,
    WM_F_EQZ_BR_IF,
    WM_F_LE_U_BR_IF,
    WM_F_LT_U_BR_IF,
    WM_F_EQ_IF,
    WM_F_NE_IF,
    WM_F_EQZ_IF,
    WM_F_COUNT
};

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
    /* Predecoded records, or NULL. */
    WmCell* fast;
    uint32_t fast_cells;
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

struct WmModule
{
    const uint8_t* bytes;
    uint32_t size;

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
};

struct WmFunc
{
    /* NULL for host functions. */
    WmInstance* instance;
    const WmCode* code;
    const uint8_t* params;
    const uint8_t* results;
    wm_count_t param_count;
    wm_count_t result_count;
    wm_count_t param_slots;
    wm_count_t result_slots;
    WmHostFunction host;
    void* user;
};

struct WmGlobal
{
    uint8_t type;
    uint8_t mutable_;
    uint64_t value;
};

/* Elements are references: an WmFunc pointer for funcref, the host's value for externref, 0 for null. */
struct WmTable
{
    uint8_t type;
    uint32_t size;
    uint32_t max;
    uintptr_t* elems;
};

struct WmMemory
{
    /* The memory in one block; NULL when it is paged. */
    uint8_t* data;
    uint32_t pages;
    uint32_t max_pages;
    uint8_t page_log2;
#if WM_HAS_MMU
    /* Its address space number in TLB keys, 1..511; 0 when none was free (never cached). */
    uint16_t space;
    /* Paged: a 4 KiB frame per slot, NULL until first touched. */
    uint8_t** frames;
    uint32_t frame_count;
    /* A former one-block memory: its first block_frames frames are in it. */
    uint8_t* block;
    uint32_t block_frames;
#endif
};

#if WM_HAS_MMU
#define WM_FRAME_LOG2 12u
#define WM_FRAME (1u << WM_FRAME_LOG2)

/*
 * A TLB, as a CPU core has one: frames of paged memories, each entry tagged
 * with its memory's address space number in key bits 3..11, which no
 * frame address sets. A runtime uses one; by default all share
 * wm_tlb_default, and a host that runs programs on several threads gives
 * each its own (wm_tlb_init).
 */
#ifndef WM_TLB_ENTRIES
#define WM_TLB_ENTRIES 16u
#endif
#define WM_TLB_EMPTY 0x800u

typedef struct WmTlbEntry
{
    uint32_t tag;
    uint8_t* frame;
} WmTlbEntry;

typedef struct WmTlb
{
    WmTlbEntry entries[WM_TLB_ENTRIES];
    struct WmTlb* next;
} WmTlb;

extern WmTlb wm_tlb_default;
void wm_tlb_init(WmTlb* tlb);
/* A frame of a memory, made (zeroed) if it was untouched; NULL without memory or past the end. */
uint8_t* wm_memory_frame(WmMemory* memory, uint32_t index);
WmResult wm_memory_fill(WmMemory* memory, uint32_t address, uint8_t value, uint32_t length);
/*
 * A TLB miss on a paged memory: the frame of the access made and put in the
 * TLB (NULL: run it again), wm_page_emulate when the TLB cannot hold it
 * (unaligned, across frames, no address space number), or a trap.
 */
WmResult wm_page_fault(WmTlb* tlb, WmMemory* memory, uint32_t address, uint32_t offset, uint32_t size);
extern const char wm_page_emulate[];
/* One byte of a memory, reading an untouched frame as zero. */
uint8_t wm_memory_byte(const WmMemory* memory, uint32_t address);
WmResult wm_memory_copy(WmMemory* memory, uint32_t to, uint32_t from, uint32_t length);
#endif

/* A memory's size in bytes. */
#define WM_MEMORY_SIZE(mem) ((wm_size)(mem)->pages << (mem)->page_log2)

struct WmInstance
{
    const WmModule* module;
    WmFunc** funcs;
    WmFunc* own_funcs;
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
} WmSuspended;

struct WmRuntime
{
    uint32_t* stack;
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
    uint8_t suspended;
    /* Calls in progress: only the outermost one stops for fuel. */
    uint8_t nesting;
    WmSuspended saved;
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
WmResult wm_predecode(WmModule* module, WmCode* code);
void wm_module_predecode(WmModule* module);
int wm_fast_position(const WmModule* module, const WmCode* code, const WmCell* ip, uint32_t* offset, uint32_t* side);
const WmCell* wm_fast_ip(const WmModule* module, const WmCode* code, uint32_t offset);

/* Shared helpers. */
int wm_types_equal(const uint8_t* a, uint32_t a_count, const uint8_t* b, uint32_t b_count);
/* Program bytes (read with WM_RD8) against bytes in RAM. */
int wm_bytes_equal(const uint8_t* program, uint32_t program_count, const uint8_t* ram, uint32_t ram_count);
int wm_func_has_type(const WmFunc* f, const WmType* t);
WmResult wm_eval_const(WmInstance* instance, const uint8_t* expr, uint64_t* out);
WmResult wm_execute(WmRuntime* runtime, WmFunc* func, int resume);

#endif
