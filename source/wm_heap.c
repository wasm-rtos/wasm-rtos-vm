/*
 * TLSF, two-level segregated fit (decision 20; Masmano, Ripoll, Crespo,
 * Real, "TLSF: a new dynamic memory allocator for real-time systems",
 * 2004). Free blocks sit in lists by size: a first level by power of two,
 * SL classes in each, two bitmaps say which lists have blocks, so finding
 * one is a couple of bit scans. Every block starts with a header: the
 * block before it in memory (valid while that one is free) and its size,
 * whose low bits say whether it is free, whether the one before is, and
 * whether it was cut from the top. A free block holds its list links where
 * its data would be. The last block of the heap is a sentinel of size 0
 * that is never free.
 */
#include "wm_heap.h"

#include <stdint.h>
#include <string.h>

#include "wm.h"

#ifndef WM_HEAP_MAX_LOG2
/* 4 GiB, 16 MiB on 32-bit chips, 32 KiB where size_t has 16 bits (AVR). */
#define WM_HEAP_MAX_LOG2 (sizeof(size_t) > 4 ? 32 : sizeof(size_t) > 2 ? 24 : 16)
#endif

enum
{
    ALIGN_LOG2 = 3, /* 8 bytes: doubles and i64 */
    ALIGN = 1 << ALIGN_LOG2,
    SL_LOG2 = 4, /* 16 classes per power of two */
    SL_COUNT = 1 << SL_LOG2,
    FL_SHIFT = SL_LOG2 + ALIGN_LOG2, /* sizes below 2^FL_SHIFT are all in the first level, by ALIGN */
    FL_COUNT = WM_HEAP_MAX_LOG2 - FL_SHIFT + 1,
    FREE = 1,
    PREV_FREE = 2,
    HIGH = 4,
    FLAGS = 7
};

typedef struct Block
{
    struct Block* prev_phys;
    size_t size; /* bytes after this header, with FLAGS in the low bits */
    /* Only while free: */
    struct Block* next_free;
    struct Block* prev_free;
} Block;

/* The header given blocks keep; the links are the start of their data. */
#define HEADER offsetof(Block, next_free)
#define MIN_SIZE (sizeof(Block) - HEADER)
#define MAX_SIZE ((size_t)1 << (WM_HEAP_MAX_LOG2 - 1))

struct WmHeap
{
    uint32_t fl_bitmap;
    uint32_t sl_bitmap[FL_COUNT];
    Block* lists[FL_COUNT][SL_COUNT];
    Block* first;
    size_t used, blocks;
};

/* The first level's bitmap is 32 bits. */
typedef char wm_heap_fl_fits[FL_COUNT <= 32 ? 1 : -1];

static int fls_size(size_t x) /* the highest set bit, x > 0 */
{
#if defined(__GNUC__)
    return sizeof(size_t) > sizeof(unsigned) ? 63 - __builtin_clzll((unsigned long long)x) : 31 - __builtin_clz((unsigned)x);
#else
    int bit = -1;
    while (x)
        ++bit, x >>= 1;
    return bit;
#endif
}

static int ffs32(uint32_t x) /* the lowest set bit, x > 0 */
{
#if defined(__GNUC__)
    return __builtin_ctz(x);
#else
    int bit = 0;
    while (!(x & 1u))
        ++bit, x >>= 1;
    return bit;
#endif
}

static size_t size_of(const Block* b) { return b->size & ~(size_t)FLAGS; }
static int is_free(const Block* b) { return (b->size & FREE) != 0; }
static Block* next_phys(const Block* b) { return (Block*)(void*)((uint8_t*)(void*)(uintptr_t)b + HEADER + size_of(b)); }
static void* data_of(Block* b) { return (uint8_t*)(void*)b + HEADER; }
static Block* block_of(void* p) { return (Block*)(void*)((uint8_t*)p - HEADER); }
static void set_size(Block* b, size_t size) { b->size = size | (b->size & FLAGS); }

static void mapping(size_t size, int* fl, int* sl)
{
    if (size < ((size_t)1 << FL_SHIFT))
    {
        *fl = 0;
        *sl = (int)(size >> ALIGN_LOG2);
    }
    else
    {
        int top = fls_size(size);
        *sl = (int)(size >> (top - SL_LOG2)) ^ SL_COUNT;
        *fl = top - (FL_SHIFT - 1);
    }
}

/* The list to search for size: rounded up, so any block there fits. */
static void mapping_search(size_t size, int* fl, int* sl)
{
    if (size >= ((size_t)1 << FL_SHIFT))
        size += ((size_t)1 << (fls_size(size) - SL_LOG2)) - 1;
    mapping(size, fl, sl);
}

static void list_remove(WmHeap* h, Block* b)
{
    int fl, sl;
    mapping(size_of(b), &fl, &sl);
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
    {
        h->lists[fl][sl] = b->next_free;
        if (!b->next_free)
        {
            h->sl_bitmap[fl] &= ~(1u << sl);
            if (!h->sl_bitmap[fl])
                h->fl_bitmap &= ~(1u << fl);
        }
    }
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
}

static void list_insert(WmHeap* h, Block* b)
{
    int fl, sl;
    mapping(size_of(b), &fl, &sl);
    b->prev_free = NULL;
    b->next_free = h->lists[fl][sl];
    if (b->next_free)
        b->next_free->prev_free = b;
    h->lists[fl][sl] = b;
    h->sl_bitmap[fl] |= 1u << sl;
    h->fl_bitmap |= 1u << fl;
}

/* A block free from now: flagged, its successor told, put in its list. */
static void mark_free(WmHeap* h, Block* b)
{
    Block* next = next_phys(b);
    b->size |= FREE;
    b->size &= ~(size_t)HIGH;
    next->prev_phys = b;
    next->size |= PREV_FREE;
    list_insert(h, b);
}

static void mark_used(Block* b)
{
    b->size &= ~(size_t)FREE;
    next_phys(b)->size &= ~(size_t)PREV_FREE;
}

/* A free block merged with free neighbours (out of their lists), not yet in a list. */
static Block* merge(WmHeap* h, Block* b)
{
    Block* next = next_phys(b);
    if (b->size & PREV_FREE)
    {
        Block* prev = b->prev_phys;
        list_remove(h, prev);
        set_size(prev, size_of(prev) + HEADER + size_of(b));
        b = prev;
    }
    if (is_free(next))
    {
        list_remove(h, next);
        set_size(b, size_of(b) + HEADER + size_of(next));
    }
    next_phys(b)->prev_phys = b;
    return b;
}

static size_t adjust(size_t size)
{
    size = (size + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    return size < MIN_SIZE ? MIN_SIZE : size;
}

/* The used block b keeps size bytes; the rest, when large enough for a block, goes back free. */
static void trim(WmHeap* h, Block* b, size_t size)
{
    size_t have = size_of(b);
    if (have >= size + sizeof(Block))
    {
        Block* rest = (Block*)(void*)((uint8_t*)data_of(b) + size);
        rest->size = (have - size - HEADER) | (size_t)0;
        rest->prev_phys = b;
        set_size(b, size);
        next_phys(rest)->prev_phys = rest;
        rest = merge(h, rest);
        mark_free(h, rest);
    }
}

WmHeap* wm_heap_new(void* memory, size_t bytes)
{
    uintptr_t start = ((uintptr_t)memory + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    size_t lost = start - (uintptr_t)memory;
    size_t control = (sizeof(WmHeap) + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    WmHeap* h = (WmHeap*)(void*)start;
    Block *b, *sentinel;
    size_t size;
    if (!memory || bytes < lost + control + 2 * sizeof(Block) + 64)
        return NULL;
    bytes = (bytes - lost) & ~(size_t)(ALIGN - 1);
    memset(h, 0, sizeof(*h));
    size = bytes - control - 2 * HEADER; /* the block's header, and the sentinel's */
    if (size > MAX_SIZE)
        size = MAX_SIZE & ~(size_t)(ALIGN - 1);
    b = (Block*)(void*)(start + control);
    b->prev_phys = NULL;
    b->size = size;
    sentinel = next_phys(b);
    sentinel->size = 0;
    sentinel->prev_phys = b;
    h->first = b;
    mark_free(h, b);
    return h;
}

void* wm_heap_malloc(WmHeap* h, size_t size)
{
    int fl, sl;
    uint32_t map;
    Block* b;
    if (!h || size > MAX_SIZE)
        return NULL;
    size = adjust(size);
    mapping_search(size, &fl, &sl);
    if (fl >= FL_COUNT)
        return NULL;
    map = h->sl_bitmap[fl] & (~0u << sl);
    if (!map)
    {
        uint32_t fls = fl + 1 < 32 ? h->fl_bitmap & (~0u << (fl + 1)) : 0;
        if (!fls)
            return NULL;
        fl = ffs32(fls);
        map = h->sl_bitmap[fl];
    }
    sl = ffs32(map);
    b = h->lists[fl][sl];
    list_remove(h, b);
    mark_used(b);
    trim(h, b, size);
    h->used += HEADER + size_of(b);
    ++h->blocks;
    return data_of(b);
}

void* wm_heap_malloc_high(WmHeap* h, size_t size)
{
    Block* best = NULL;
    Block* b;
    size_t have;
    if (!h || size > MAX_SIZE)
        return NULL;
    size = adjust(size);
    /* The highest free block that fits: every list from the size's first level up (rare: chunks are few). */
    int first, unused;
    mapping(size, &first, &unused);
    for (int fl = first; fl < FL_COUNT; ++fl)
        if (h->fl_bitmap & (1u << fl))
            for (int sl = 0; sl < SL_COUNT; ++sl)
                for (b = h->lists[fl][sl]; b; b = b->next_free)
                    if (size_of(b) >= size && b > best)
                        best = b;
    if (!best)
        return NULL;
    list_remove(h, best);
    have = size_of(best);
    if (have >= size + sizeof(Block))
    {
        /* The top size bytes become the block given; the bottom stays free. */
        Block* top = (Block*)(void*)((uint8_t*)data_of(best) + have - size - HEADER);
        set_size(best, have - size - HEADER);
        top->prev_phys = best;
        top->size = size;
        next_phys(top)->prev_phys = top;
        list_insert(h, best); /* still FREE, its successor now top */
        top->size |= PREV_FREE;
        b = top;
        next_phys(b)->size &= ~(size_t)PREV_FREE;
    }
    else
    {
        b = best;
        mark_used(b);
    }
    b->size |= HIGH;
    h->used += HEADER + size_of(b);
    ++h->blocks;
    return data_of(b);
}

void wm_heap_free(WmHeap* h, void* p)
{
    Block* b;
    if (!h || !p)
        return;
    b = block_of(p);
    h->used -= HEADER + size_of(b);
    --h->blocks;
    b->size |= FREE;
    b->size &= ~(size_t)HIGH;
    b = merge(h, b);
    mark_free(h, b);
}

void* wm_heap_realloc(WmHeap* h, void* p, size_t size)
{
    Block *b, *next;
    size_t have, want;
    void* fresh;
    if (!p)
        return wm_heap_malloc(h, size);
    if (!size)
    {
        wm_heap_free(h, p);
        return NULL;
    }
    if (size > MAX_SIZE)
        return NULL;
    b = block_of(p);
    have = size_of(b);
    want = adjust(size);
    next = next_phys(b);
    h->used -= HEADER + have;
    if (want <= have || (is_free(next) && have + HEADER + size_of(next) >= want))
    {
        if (want > have)
        {
            list_remove(h, next);
            set_size(b, have + HEADER + size_of(next));
            next_phys(b)->prev_phys = b;
            next_phys(b)->size &= ~(size_t)PREV_FREE;
        }
        trim(h, b, want);
        h->used += HEADER + size_of(b);
        return p;
    }
    h->used += HEADER + have;
    fresh = (b->size & HIGH) ? wm_heap_malloc_high(h, size) : wm_heap_malloc(h, size);
    if (!fresh)
        return NULL;
    memcpy(fresh, p, have < want ? have : want);
    wm_heap_free(h, p);
    return fresh;
}

void wm_heap_stats(const WmHeap* h, WmHeapStats* out)
{
    memset(out, 0, sizeof(*out));
    if (!h)
        return;
    for (const Block* b = h->first; size_of(b); b = next_phys(b))
        if (is_free(b))
        {
            out->free += size_of(b);
            if (size_of(b) > out->largest)
                out->largest = size_of(b);
        }
    out->used = h->used;
    out->blocks = h->blocks;
}

int wm_heap_check(const WmHeap* h)
{
    size_t in_lists = 0, walked = 0, used = 0, blocks = 0;
    const Block* prev = NULL;
    if (!h)
        return 0;
    for (const Block* b = h->first;; b = next_phys(b))
    {
        if (prev && b->prev_phys != prev && is_free(prev))
            return 0;
        if (prev && (((b->size & PREV_FREE) != 0) != is_free(prev)))
            return 0;
        if (prev && is_free(prev) && is_free(b))
            return 0; /* two free neighbours were not merged */
        if (!size_of(b))
            break;
        if (is_free(b))
            ++walked;
        else
            used += HEADER + size_of(b), ++blocks;
        prev = b;
    }
    for (int fl = 0; fl < FL_COUNT; ++fl)
        for (int sl = 0; sl < SL_COUNT; ++sl)
        {
            uint32_t has = h->lists[fl][sl] != NULL;
            if (has != ((h->sl_bitmap[fl] >> sl) & 1u) || (has && !((h->fl_bitmap >> fl) & 1u)))
                return 0;
            for (const Block* b = h->lists[fl][sl]; b; b = b->next_free)
            {
                int f, s;
                mapping(size_of(b), &f, &s);
                if (!is_free(b) || f != fl || s != sl || (b->next_free && b->next_free->prev_free != b))
                    return 0;
                ++in_lists;
            }
        }
    return in_lists == walked && used == h->used && blocks == h->blocks;
}

static WmHeap* g_heap;

void wm_heap_use(WmHeap* heap)
{
    g_heap = heap;
    wm_set_allocator(heap ? wm_heap_allocator : NULL);
    wm_set_code_allocator(heap ? wm_heap_code_allocator : NULL);
}

WmHeap* wm_heap_current(void) { return g_heap; }

void* wm_heap_allocator(void* ptr, size_t size)
{
    if (!size)
    {
        wm_heap_free(g_heap, ptr);
        return NULL;
    }
    return ptr ? wm_heap_realloc(g_heap, ptr, size) : wm_heap_malloc(g_heap, size);
}

void* wm_heap_code_allocator(void* ptr, size_t size)
{
    if (!size)
    {
        wm_heap_free(g_heap, ptr);
        return NULL;
    }
    return ptr ? wm_heap_realloc(g_heap, ptr, size) : wm_heap_malloc_high(g_heap, size);
}
