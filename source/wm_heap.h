#ifndef WM_HEAP_H
#define WM_HEAP_H

/*
 * A heap of the engine's own (decision 20): TLSF over one block of memory
 * the host gives, the same on every platform. Allocation and freeing take
 * constant time; what the engine can move (the code area's chunks) is cut
 * from the top of the heap and everything else from the bottom, so the
 * code area's compaction frees one piece instead of holes between blocks
 * that stay.
 *
 *   static uint8_t arena[96 * 1024];
 *   wm_heap_use(wm_heap_new(arena, sizeof(arena)));   (the engine's memory, code from the top)
 *
 * or give wm_heap_allocator and wm_heap_code_allocator to wm_set_allocator
 * and wm_set_code_allocator yourself (a kernel that counts memory wraps
 * them). One thread at a time; interrupt handlers do not allocate.
 */

#include <stddef.h>

typedef struct WmHeap WmHeap;

/* A heap in memory (its control takes the first bytes); NULL when bytes is too small. Up to 2^WM_HEAP_MAX_LOG2 bytes. */
WmHeap* wm_heap_new(void* memory, size_t bytes);
/* From the bottom; NULL when nothing fits. */
void* wm_heap_malloc(WmHeap* heap, size_t size);
/* From the top: for what the engine moves (code). */
void* wm_heap_malloc_high(WmHeap* heap, size_t size);
/* In place when it can (into a free neighbour, or shrinking), else moved, from the same end. */
void* wm_heap_realloc(WmHeap* heap, void* ptr, size_t size);
void wm_heap_free(WmHeap* heap, void* ptr);

typedef struct WmHeapStats
{
    size_t free;    /* bytes in free blocks */
    size_t largest; /* the largest free block */
    size_t used;    /* bytes in blocks given out, headers included */
    size_t blocks;  /* blocks given out */
} WmHeapStats;
void wm_heap_stats(const WmHeap* heap, WmHeapStats* out);
/* Checking: every block walks to the next and agrees with the free lists; 0 when the heap is broken. */
int wm_heap_check(const WmHeap* heap);

/* The heap the allocators below use, installed with wm_set_allocator and wm_set_code_allocator (NULL: neither). */
void wm_heap_use(WmHeap* heap);
WmHeap* wm_heap_current(void);
/* WmAllocator over that heap: wm_heap_allocator from the bottom, wm_heap_code_allocator from the top. */
void* wm_heap_allocator(void* ptr, size_t size);
void* wm_heap_code_allocator(void* ptr, size_t size);

#endif
