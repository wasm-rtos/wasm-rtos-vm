/*
 * Linear memory. A memory is one block when the heap has room for it; when
 * it has not, it is paged: frames of WM_FRAME_SIZE bytes (4 KiB unless
 * built otherwise) anywhere in the heap, so it never
 * needs a large free block. Every frame is made when the memory is made or
 * grows, so a memory.grow that succeeds has the memory behind it and an
 * access never runs out. A block that cannot grow becomes paged without a
 * copy: its frames stay where they are.
 *
 * The interpreter reaches a one-block memory directly and a paged one
 * through the TLB (wm_exec.c); everything else (segments, bulk
 * instructions, snapshots, the host) goes through the functions here.
 */
#include "wm_internal.h"

#include <string.h>

#define E_OOB WM_ERR_TRAP("trap: out of bounds memory access")
#define E_MEMORY WM_ERR_EXHAUSTION("exhaustion: out of memory")

static int in_bounds(const WmMemory* memory, uint32_t address, uint32_t length)
{
    return memory && !wm_out_of_bounds(address, length, WM_MEMORY_SIZE(memory));
}

#if WM_HAS_MMU

/* ---- TLBs and address spaces ---- */

WmTlb wm_tlb_default;
static WmTlb* g_tlbs = &wm_tlb_default;
/* Address space numbers in use, one bit each; number 0 is never handed out. */
static uint32_t g_spaces[(WM_SPACES + 31u) / 32u] = {1u};
static uint8_t g_tlbs_ready;

void wm_tlb_init(WmTlb* tlb)
{
    for (uint32_t i = 0; i < WM_TLB_ENTRIES; ++i)
        tlb->entries[i].tag = tlb->entries[i].wtag = WM_TLB_EMPTY;
    if (tlb != &wm_tlb_default)
    {
        tlb->next = g_tlbs;
        g_tlbs = tlb;
    }
}

/* Entries of a memory in any TLB go: its frames moved or went. */
static void tlb_forget(uint32_t space)
{
    for (WmTlb* t = g_tlbs; t; t = t->next)
        for (uint32_t i = 0; i < WM_TLB_ENTRIES; ++i)
            if (((t->entries[i].tag >> 3) & (WM_SPACES - 1u)) == space)
                t->entries[i].tag = t->entries[i].wtag = WM_TLB_EMPTY;
}

static void space_release(WmMemory* memory)
{
    if (!memory->space || memory->space == WM_SPACE_UNCACHED)
        return;
    tlb_forget(memory->space);
    g_spaces[memory->space / 32u] &= ~(1u << (memory->space % 32u));
    memory->space = 0;
}

/*
 * A fresh number for a paged memory. With none left it gets
 * WM_SPACE_UNCACHED and is never in a TLB, which is still correct, only
 * slower: no number but the empty entries' 0 is ever cached under it.
 */
static void space_renew(WmMemory* memory)
{
    if (!g_tlbs_ready)
    {
        wm_tlb_init(&wm_tlb_default);
        g_tlbs_ready = 1;
    }
    space_release(memory);
    for (uint32_t n = 1; n < WM_SPACE_UNCACHED; ++n)
        if (!(g_spaces[n / 32u] & (1u << (n % 32u))))
        {
            g_spaces[n / 32u] |= 1u << (n % 32u);
            memory->space = (uint16_t)n;
            return;
        }
    memory->space = WM_SPACE_UNCACHED;
}

/* ---- Pager ---- */

#define WM_MARK_TOUCHED 0x8000u
#define WM_MARK_DIRTY 0x4000u
/* Being read in the background: in RAM, but not to be used before it ends. */
#define WM_MARK_LOADING 0x2000u
/* A write of it in the background, until the host says it ended: it may not start another. */
#define WM_MARK_WRITING 0x1000u
/* On its way out with that write: its RAM goes when the write ends, unless it is touched first. */
#define WM_MARK_LEAVING 0x0800u
#define WM_MARK_PINS 0x07FFu
/* A frame slot whose frame is in the pager's storage. */
#define WM_FRAME_OUT ((uint8_t*)(uintptr_t)1u)

static const WmPager* g_pager;
/* Why the last frame could not come into RAM: no heap for it, or what the pager said. */
static WmResult g_frame_error;
/* Memories with a pager, and their frames in RAM. */
static WmMemory* g_swapped;
static uint32_t g_resident;

void wm_set_pager(const WmPager* pager) { g_pager = pager; }

uint32_t wm_pager_resident(void) { return g_resident; }

uint32_t wm_frame_size(void) { return WM_FRAME; }

/*
 * The frames in RAM of every memory with a pager, as a ring the pager's
 * CLOCK hand goes round (an OS's core map): taking a frame out looks only
 * at frames that are in RAM, however much is in storage. A memory keeps
 * each frame's place in the ring (ring_place), so a frame leaves it at once.
 */
typedef struct WmRingEntry
{
    WmMemory* memory;
    uint32_t index;
} WmRingEntry;
static WmRingEntry* g_ring;
static uint32_t g_ring_count, g_ring_size, g_ring_hand;

/* Room in the ring for one more frame: 0 when the heap has none. */
static int ring_room(void)
{
    WmRingEntry* ring;
    uint32_t size;
    if (g_ring_count < g_ring_size)
        return 1;
    size = g_ring_size ? 2u * g_ring_size : 16u;
    if (!(ring = (WmRingEntry*)wm_realloc(g_ring, (size_t)size * sizeof(WmRingEntry))))
        return 0;
    g_ring = ring;
    g_ring_size = size;
    return 1;
}

static void ring_add(WmMemory* memory, uint32_t index)
{
    if (!memory->ring_place || !ring_room())
        return;
    g_ring[g_ring_count].memory = memory;
    g_ring[g_ring_count].index = index;
    memory->ring_place[index] = g_ring_count++;
}

static void ring_remove(WmMemory* memory, uint32_t index)
{
    uint32_t place;
    if (!memory->ring_place || (place = memory->ring_place[index]) == WM_NONE)
        return;
    memory->ring_place[index] = WM_NONE;
    /* The last frame in the ring takes its place. */
    if (place != --g_ring_count)
    {
        g_ring[place] = g_ring[g_ring_count];
        g_ring[place].memory->ring_place[g_ring[place].index] = place;
    }
    if (!g_ring_count)
    {
        wm_free(g_ring);
        g_ring = NULL;
        g_ring_size = g_ring_hand = 0;
    }
}

int wm_pager_hand(WmMemory** memory, uint32_t* index)
{
    if (!g_ring_count)
        return 0;
    if (g_ring_hand >= g_ring_count)
        g_ring_hand = 0;
    *memory = g_ring[g_ring_hand].memory;
    *index = g_ring[g_ring_hand].index;
    ++g_ring_hand;
    return 1;
}

WmMemory* wm_memory_next(WmMemory* after) { return after ? after->next : g_swapped; }

static int frame_out(const uint8_t* frame) { return frame == WM_FRAME_OUT; }

static int frame_here(const uint8_t* frame) { return frame && !frame_out(frame); }

/* A frame's entry goes from every TLB: it left RAM. */
static void tlb_forget_frame(uint32_t space, uint32_t index)
{
    uint32_t tag = (index << WM_FRAME_LOG2) | (space << 3);
    for (WmTlb* t = g_tlbs; t; t = t->next)
    {
        WmTlbEntry* e = &t->entries[index & (WM_TLB_ENTRIES - 1u)];
        if (e->tag == tag)
            e->tag = e->wtag = WM_TLB_EMPTY;
    }
}

/* ---- Frames ---- */

/* Frames a memory of size bytes takes. */
static uint32_t frames_for(wm_size size)
{
    return (uint32_t)((size + WM_FRAME - 1u) >> WM_FRAME_LOG2);
}

static int frame_in_block(const WmMemory* memory, uint32_t index)
{
    return memory->block && index < memory->block_frames;
}

/*
 * Allocations for frames and their tables. When the heap has no room,
 * predecoded code goes first, one function's records at a time: it is
 * made again when called, while a program's memory cannot be refused.
 */
static void* frames_calloc(size_t count, size_t size)
{
    void* p;
    while (!(p = wm_calloc(count, size)) && wm_code_drop_one())
        ;
    return p;
}

static void* frames_realloc(void* ptr, size_t size)
{
    void* p;
    while (!(p = wm_realloc(ptr, size)) && wm_code_drop_one())
        ;
    return p;
}

/*
 * The frame pool (decision 6): a region the host gives for frames, so they
 * do not cut up the heap. With a pool every frame of a memory with a pager
 * comes from it, and a memory in one block is its frames in a row there.
 * The engine knows which memory and index use each frame of the pool, and
 * every pointer to it (the memory's frame table, the TLB), so it moves
 * frames to make room for a block.
 */
typedef struct WmPoolOwner
{
    /* NULL: the frame is free. */
    WmMemory* memory;
    uint32_t index;
} WmPoolOwner;
static uint8_t* g_pool;
static WmPoolOwner* g_pool_owner;
static uint32_t g_pool_frames, g_pool_hint;

static uint8_t* pool_frame(uint32_t slot) { return g_pool + ((size_t)slot << WM_FRAME_LOG2); }
static int in_pool(const uint8_t* frame) { return g_pool && frame >= g_pool && frame < pool_frame(g_pool_frames); }
static uint32_t pool_slot(const uint8_t* frame) { return (uint32_t)((size_t)(frame - g_pool) >> WM_FRAME_LOG2); }

void wm_set_frame_pool(uint8_t* memory, uint32_t frames)
{
    wm_free(g_pool_owner);
    g_pool = NULL;
    g_pool_owner = NULL;
    g_pool_frames = g_pool_hint = 0;
    if (memory && frames && (g_pool_owner = (WmPoolOwner*)wm_calloc(frames, sizeof(WmPoolOwner))))
    {
        g_pool = memory;
        g_pool_frames = frames;
    }
}

/* A free frame of the pool for frame index of memory, zeroed; NULL when every one is used. */
static uint8_t* pool_take(WmMemory* memory, uint32_t index)
{
    for (uint32_t n = 0; n < g_pool_frames; ++n)
    {
        uint32_t s = (g_pool_hint + n) % g_pool_frames;
        if (!g_pool_owner[s].memory)
        {
            g_pool_owner[s].memory = memory;
            g_pool_owner[s].index = index;
            g_pool_hint = s + 1u;
            memset(pool_frame(s), 0, WM_FRAME);
            return pool_frame(s);
        }
    }
    return NULL;
}

/* A block's frames go back to the pool. */
static void pool_release_block(const uint8_t* data, uint32_t frames)
{
    for (uint32_t i = 0, s = pool_slot(data); i < frames; ++i)
        g_pool_owner[s + i].memory = NULL;
}

/* A frame leaves RAM or its memory: back to the pool it came from, or to the heap. */
static void frame_free(uint8_t* frame)
{
    if (in_pool(frame))
        g_pool_owner[pool_slot(frame)].memory = NULL;
    else
        wm_free(frame);
}

/* A zeroed frame coming into RAM; with a pager, once it has made room. */
static uint8_t* new_frame(WmMemory* memory, uint32_t index)
{
    const WmPager* pager = memory->pager;
    uint8_t* frame;
    if (!pager)
        return (uint8_t*)frames_calloc(WM_FRAME, 1);
    if (!ring_room() || (pager->room && !pager->room(pager->context, 0)))
        return NULL;
    /*
     * The pager's choice first (a frame to storage); without a pool the code
     * cache when it has none (records are in the heap, not in the pool).
     */
    while (!(frame = g_pool ? pool_take(memory, index) : (uint8_t*)wm_calloc(WM_FRAME, 1)))
        if (!(pager->room && pager->room(pager->context, 1)) && (g_pool || !wm_code_drop_one()))
            return NULL;
    ++g_resident;
    return frame;
}

/*
 * A frame still being read in the background is needed now: wait for the
 * read to end. 0 when it failed, and then the frame is back in storage.
 */
static int finish_loading(WmMemory* memory, uint32_t index)
{
    if (!memory->marks || !(memory->marks[index] & WM_MARK_LOADING))
        return 1;
    memory->marks[index] &= (uint16_t)~WM_MARK_LOADING;
    if (!(g_frame_error = memory->pager->finish(memory->pager->context, memory->slots[index])))
        return 1;
    ring_remove(memory, index);
    frame_free(memory->frames[index]);
    memory->frames[index] = WM_FRAME_OUT;
    --g_resident;
    return 0;
}

/* A frame of a memory leaves it: freed, or its slot let go. */
static void free_frame(WmMemory* memory, uint32_t index)
{
    uint8_t* frame;
    if (frame_in_block(memory, index))
        return;
    /* No read or write may go on with a frame that is freed. */
    (void)finish_loading(memory, index);
    if (memory->marks && (memory->marks[index] & WM_MARK_WRITING))
    {
        memory->marks[index] &= (uint16_t)~(WM_MARK_WRITING | WM_MARK_LEAVING);
        (void)memory->pager->finish(memory->pager->context, memory->slots[index]);
    }
    frame = memory->frames[index];
    if (memory->slots && memory->slots[index] != WM_NONE)
    {
        memory->pager->drop(memory->pager->context, memory->slots[index]);
        memory->slots[index] = WM_NONE;
    }
    if (frame_here(frame))
    {
        ring_remove(memory, index);
        frame_free(frame);
        if (memory->pager)
            --g_resident;
    }
    memory->frames[index] = NULL;
}

/*
 * A frame of a memory, made (zeroed) if it was untouched or brought back
 * from the pager's storage, and marked dirty when it is for writing; NULL
 * without memory, past the end, or when it cannot come into RAM.
 */
static uint8_t* wm_memory_frame(WmMemory* memory, uint32_t index, int write)
{
    uint8_t* frame;
    if (memory->data)
        return memory->data + ((wm_size)index << WM_FRAME_LOG2);
    if (index >= memory->frame_count)
        return NULL;
    if (!finish_loading(memory, index))
        return NULL;
    /* On its way out but touched: it stays, and counts as dirty, its copy may not hold. */
    if (memory->marks && (memory->marks[index] & WM_MARK_LEAVING))
        memory->marks[index] = (uint16_t)((memory->marks[index] & ~WM_MARK_LEAVING) | WM_MARK_DIRTY);
    frame = memory->frames[index];
    if (!frame_here(frame))
    {
        /* Untouched, or in the pager's storage: into RAM now. */
        uint8_t* made = new_frame(memory, index);
        if (!made)
        {
            g_frame_error = E_MEMORY;
            return NULL;
        }
        /* Back from storage, its slot keeps a copy: clean until written. */
        if (frame_out(memory->frames[index]) &&
            (g_frame_error = memory->pager->read(memory->pager->context, memory->slots[index], made)))
        {
            frame_free(made);
            --g_resident;
            return NULL;
        }
        memory->frames[index] = frame = made;
        ring_add(memory, index);
    }
    if (memory->marks)
        memory->marks[index] |= (uint16_t)(WM_MARK_TOUCHED | (write ? WM_MARK_DIRTY : 0u));
    return frame;
}

/* A frame to read from: an untouched one reads as zeros, without being made. NULL: it could not come back. */
static const uint8_t* frame_to_read(const WmMemory* memory, uint32_t index)
{
    static const uint8_t zeros[WM_FRAME];
    if (memory->data)
        return memory->data + ((wm_size)index << WM_FRAME_LOG2);
    if (!memory->frames[index])
        return zeros;
    return wm_memory_frame((WmMemory*)memory, index, 0);
}

uint8_t wm_memory_byte(const WmMemory* memory, uint32_t address)
{
    const uint8_t* frame = frame_to_read(memory, address >> WM_FRAME_LOG2);
    return frame ? frame[address & (WM_FRAME - 1u)] : 0;
}

/* A one-block memory becomes paged: its whole frames stay in the block, a partial last one is copied. */
static int make_paged(WmMemory* memory, uint32_t frame_count)
{
    wm_size size = WM_MEMORY_SIZE(memory);
    uint32_t whole = (uint32_t)(size >> WM_FRAME_LOG2);
    uint8_t** frames = (uint8_t**)frames_calloc(frame_count + 1u, sizeof(uint8_t*));
    if (!frames)
        return 0;
    for (uint32_t i = 0; i < whole; ++i)
        frames[i] = memory->data + ((wm_size)i << WM_FRAME_LOG2);
    if (size & (WM_FRAME - 1u))
    {
        frames[whole] = (uint8_t*)frames_calloc(WM_FRAME, 1);
        if (!frames[whole])
        {
            wm_free(frames);
            return 0;
        }
        memcpy(frames[whole], memory->data + ((wm_size)whole << WM_FRAME_LOG2), (size_t)(size & (WM_FRAME - 1u)));
    }
    memory->frames = frames;
    memory->frame_count = frame_count;
    memory->block = memory->data;
    memory->block_frames = whole;
    memory->data = NULL;
    space_renew(memory);
    return 1;
}

/* A paged memory gets the pager: its frames from now on are reserved, made when touched, and may be taken out. */
static int take_pager(WmMemory* memory)
{
    uint32_t reserved = memory->frame_count - memory->block_frames;
    if (!g_pager)
        return 1;
    memory->marks = (uint16_t*)frames_calloc(memory->frame_count + 1u, sizeof(uint16_t));
    memory->slots = (uint32_t*)frames_calloc((size_t)memory->frame_count + 1u, sizeof(uint32_t));
    memory->ring_place = (uint32_t*)frames_calloc((size_t)memory->frame_count + 1u, sizeof(uint32_t));
    if (!memory->marks || !memory->slots || !memory->ring_place || !g_pager->reserve(g_pager->context, (int32_t)reserved))
    {
        wm_free(memory->marks);
        wm_free(memory->slots);
        wm_free(memory->ring_place);
        memory->marks = NULL;
        memory->slots = NULL;
        memory->ring_place = NULL;
        return 0;
    }
    memset(memory->slots, 0xFF, ((size_t)memory->frame_count + 1u) * sizeof(uint32_t));
    memcpy(memory->ring_place, memory->slots, ((size_t)memory->frame_count + 1u) * sizeof(uint32_t));
    /* Frames it has already (a copied partial frame) have no copy in storage. */
    for (uint32_t i = memory->block_frames; i < memory->frame_count; ++i)
        if (frame_here(memory->frames[i]))
        {
            ++g_resident;
            memory->marks[i] = WM_MARK_DIRTY;
            ring_add(memory, i);
        }
    memory->pager = g_pager;
    memory->next = g_swapped;
    g_swapped = memory;
    return 1;
}

/* A memory in one block with the pager: its frames are all in RAM, in the block, and reserved in storage. */
static int is_pager_block(const WmMemory* memory) { return memory->data && memory->pager; }

/* Bytes of a block: whole frames with a pager, so each frame can go to storage as it is. */
static size_t block_bytes(int paged, wm_size size)
{
    return paged ? (size_t)frames_for(size) << WM_FRAME_LOG2 : (size_t)size;
}

static void leave_pager(WmMemory* memory)
{
    WmMemory** at = &g_swapped;
    if (!memory->pager)
        return;
    while (*at != memory)
        at = &(*at)->next;
    *at = memory->next;
    memory->pager->reserve(memory->pager->context, -(int32_t)(memory->frame_count - memory->block_frames));
}

/*
 * Make the frames from first up to count, all still missing; none stay
 * made when one cannot be.
 */
static int make_frames(WmMemory* memory, uint32_t first, uint32_t count)
{
    for (uint32_t i = first; i < count; ++i)
    {
        memory->frames[i] = (uint8_t*)frames_calloc(WM_FRAME, 1);
        if (!memory->frames[i])
        {
            while (i-- > first)
            {
                wm_free(memory->frames[i]);
                memory->frames[i] = NULL;
            }
            return 0;
        }
    }
    return 1;
}

/* More frame slots for a paged memory that grows. */
static int grow_frames(WmMemory* memory, uint32_t frame_count)
{
    uint8_t** frames;
    if (frame_count <= memory->frame_count)
        return 1;
    frames = (uint8_t**)frames_realloc(memory->frames, ((size_t)frame_count + 1u) * sizeof(uint8_t*));
    if (!frames)
        return 0;
    memset(frames + memory->frame_count, 0, (size_t)(frame_count - memory->frame_count) * sizeof(uint8_t*));
    memory->frames = frames;
    if (memory->marks)
    {
        uint16_t* marks = (uint16_t*)frames_realloc(memory->marks, ((size_t)frame_count + 1u) * sizeof(uint16_t));
        if (!marks)
            return 0;
        memset(marks + memory->frame_count, 0, (size_t)(frame_count - memory->frame_count) * sizeof(uint16_t));
        memory->marks = marks;
    }
    if (memory->slots)
    {
        uint32_t* slots = (uint32_t*)frames_realloc(memory->slots, ((size_t)frame_count + 1u) * sizeof(uint32_t));
        if (!slots)
            return 0;
        memset(slots + memory->frame_count, 0xFF, (size_t)(frame_count - memory->frame_count) * sizeof(uint32_t));
        memory->slots = slots;
    }
    if (memory->ring_place)
    {
        uint32_t* places = (uint32_t*)frames_realloc(memory->ring_place, ((size_t)frame_count + 1u) * sizeof(uint32_t));
        if (!places)
            return 0;
        memset(places + memory->frame_count, 0xFF, (size_t)(frame_count - memory->frame_count) * sizeof(uint32_t));
        memory->ring_place = places;
    }
    memory->frame_count = frame_count;
    return 1;
}

#endif

/* ---- Memories ---- */

WmMemory* wm_memory_new(uint32_t pages, uint32_t max_pages) { return wm_memory_new_paged(pages, max_pages, 16); }

WmMemory* wm_memory_new_paged(uint32_t pages, uint32_t max_pages, uint8_t page_log2)
{
    WmMemory* mem = (WmMemory*)wm_calloc(1, sizeof(WmMemory));
    wm_size size = (wm_size)pages << page_log2;
    if (!mem)
        return NULL;
    mem->pages = pages;
    mem->max_pages = max_pages;
    mem->page_log2 = page_log2;
#if WM_HAS_MMU
    /* Without a pager: one block from the heap, if it has one. */
    if (!WM_MEMORY_ALWAYS_PAGED && !g_pager)
        mem->data = (uint8_t*)wm_calloc((size_t)size + 1u, 1);
    if (!mem->data)
    {
        /* No block that large: paged, the frames made now, or reserved and made when touched. */
        uint32_t count = frames_for(size);
        mem->frames = (uint8_t**)frames_calloc(count + 1u, sizeof(uint8_t*));
        mem->frame_count = count;
        if (!mem->frames || !(g_pager ? take_pager(mem) : make_frames(mem, 0, count)))
        {
            wm_free(mem->frames);
            wm_free(mem);
            return NULL;
        }
        space_renew(mem);
        /* With a pager it starts paged, and goes into one block (from the pool if there is one) when the pager lets it. */
        if (!WM_MEMORY_ALWAYS_PAGED && g_pager && g_pager->block && g_pager->block(g_pager->context, count))
            (void)wm_memory_block_in(mem);
    }
#else
    mem->data = (uint8_t*)wm_calloc((size_t)size + 1u, 1);
    if (!mem->data)
    {
        wm_free(mem);
        return NULL;
    }
#endif
    return mem;
}

void wm_memory_free(WmMemory* mem)
{
    if (!mem)
        return;
#if WM_HAS_MMU
    space_release(mem);
    if (is_pager_block(mem))
        g_resident -= mem->frame_count;
    for (uint32_t i = 0; mem->frames && i < mem->frame_count; ++i)
        free_frame(mem, i);
    leave_pager(mem);
    wm_free(mem->frames);
    wm_free(mem->marks);
    wm_free(mem->slots);
    wm_free(mem->ring_place);
    wm_free(mem->block);
    if (mem->data && in_pool(mem->data))
    {
        pool_release_block(mem->data, mem->frame_count);
        mem->data = NULL;
    }
#endif
    wm_free(mem->data);
    wm_free(mem);
}

uint8_t* wm_memory_data(const WmMemory* mem, uint32_t* size)
{
    if (size)
        *size = (uint32_t)WM_MEMORY_SIZE(mem);
    return mem->data;
}

uint32_t wm_memory_resident(const WmMemory* mem)
{
    uint32_t bytes;
    if (mem->data)
        return (uint32_t)WM_MEMORY_SIZE(mem);
    bytes = 0;
#if WM_HAS_MMU
    if (mem->block)
        bytes = mem->block_frames << WM_FRAME_LOG2;
    for (uint32_t i = 0; i < mem->frame_count; ++i)
        if (frame_here(mem->frames[i]) && !frame_in_block(mem, i))
            bytes += WM_FRAME;
#endif
    return bytes;
}

uint32_t wm_memory_grow(WmMemory* mem, uint32_t delta)
{
    uint32_t old = mem->pages;
    uint32_t pages = old + delta;
    /* The size limit in this memory's pages: the 64 KiB limit, or that many bytes for 1-byte pages. */
    wm_size limit = mem->page_log2 ? WM_MAX_PAGES : (wm_size)WM_MAX_PAGES << 16;
    wm_size size = (wm_size)pages << mem->page_log2;
    if (delta > limit || pages > limit || pages < old || (mem->max_pages != WM_NONE && pages > mem->max_pages))
        return WM_NONE;
    if (delta == 0)
        return old;
#if WM_HAS_MMU
    if (mem->data && in_pool(mem->data))
    {
        /* A block of the pool grows in place, into free frames right after it, if the pager lets it. */
        uint32_t have = mem->frame_count, more = frames_for(size) - have;
        uint32_t first = pool_slot(mem->data) + have;
        int room = mem->pager->block && mem->pager->block(mem->pager->context, more) && first + more <= g_pool_frames;
        for (uint32_t i = 0; room && i < more; ++i)
            room = !g_pool_owner[first + i].memory;
        if (room)
        {
            if (!mem->pager->reserve(mem->pager->context, (int32_t)more))
                return WM_NONE;
            if (!grow_frames(mem, have + more))
            {
                mem->pager->reserve(mem->pager->context, -(int32_t)more);
                return WM_NONE;
            }
            for (uint32_t i = 0; i < more; ++i)
            {
                g_pool_owner[first + i].memory = mem;
                g_pool_owner[first + i].index = have + i;
            }
            if (more)
                memset(pool_frame(first), 0, (size_t)more << WM_FRAME_LOG2);
            g_resident += more;
            mem->pages = pages;
            return old;
        }
        /* No room after it: the block goes to storage and the memory grows paged. */
        if (wm_memory_block_out(mem))
            return WM_NONE;
    }
#endif
    if (mem->data)
    {
        uint8_t* data = NULL;
#if WM_HAS_MMU
        /* A block with a pager grows in one block only if the pager lets the new frames stay in RAM too. */
        uint32_t more = mem->pager ? frames_for(size) - mem->frame_count : 0u;
        if (!mem->pager || (mem->pager->block && mem->pager->block(mem->pager->context, more)))
            data = (uint8_t*)wm_realloc(mem->data, block_bytes(mem->pager != NULL, size) + 1u);
#else
        data = (uint8_t*)wm_realloc(mem->data, (size_t)size + 1u);
#endif
        if (data)
        {
            mem->data = data;
#if WM_HAS_MMU
            if (more)
            {
                if (!mem->pager->reserve(mem->pager->context, (int32_t)more))
                    return WM_NONE;
                if (!grow_frames(mem, mem->frame_count + more))
                {
                    mem->pager->reserve(mem->pager->context, -(int32_t)more);
                    return WM_NONE;
                }
                g_resident += more;
            }
#endif
            memset(data + ((wm_size)old << mem->page_log2), 0, (size_t)((wm_size)delta << mem->page_log2));
            mem->pages = pages;
            return old;
        }
#if WM_HAS_MMU
        /*
         * No room to grow in one block: the memory goes on paged. With a
         * pager its block goes to storage first, without one the block
         * keeps its frames.
         */
        if (mem->pager ? wm_memory_block_out(mem) != NULL
                       : !make_paged(mem, frames_for((wm_size)old << mem->page_log2)) || !take_pager(mem))
            return WM_NONE;
#else
        return WM_NONE;
#endif
    }
#if WM_HAS_MMU
    {
        uint32_t first = frames_for((wm_size)old << mem->page_log2);
        uint32_t count = frames_for(size);
        if (mem->pager)
        {
            /* Reserved now, made when touched. */
            if (!mem->pager->reserve(mem->pager->context, (int32_t)(count - mem->frame_count)))
                return WM_NONE;
            if (!grow_frames(mem, count))
            {
                mem->pager->reserve(mem->pager->context, -(int32_t)(count - mem->frame_count));
                return WM_NONE;
            }
        }
        /* The frames past the old end are all new: make them now, or memory.grow fails. */
        else if (!grow_frames(mem, count) || !make_frames(mem, first, count))
            return WM_NONE;
    }
    mem->pages = pages;
    return old;
#else
    return WM_NONE;
#endif
}

#if WM_HAS_MMU

/* ---- Page faults ---- */

/*
 * A program touched a frame in storage: the pager may read it in the
 * background (wm_page_wait, the program waits) or has read it already.
 */
static WmResult start_loading(WmMemory* memory, uint32_t index)
{
    uint8_t* made;
    WmResult result;
    if (memory->marks[index] & WM_MARK_LOADING)
        return wm_page_wait;
    if (!frame_out(memory->frames[index]) || !memory->pager->read_start)
        return NULL;
    if (!(made = new_frame(memory, index)))
        return E_MEMORY;
    result = memory->pager->read_start(memory->pager->context, memory->slots[index], made, memory, index);
    if (result && result != wm_page_wait)
    {
        frame_free(made);
        --g_resident;
        return result;
    }
    memory->frames[index] = made;
    ring_add(memory, index);
    if (result)
        memory->marks[index] |= WM_MARK_LOADING;
    return result;
}

void wm_memory_frame_stored(WmMemory* memory, uint32_t index, WmResult result)
{
    uint16_t marks;
    if (!memory->marks || index >= memory->frame_count || !(memory->marks[index] & WM_MARK_WRITING))
        return;
    marks = memory->marks[index];
    memory->marks[index] &= (uint16_t)~(WM_MARK_WRITING | WM_MARK_LEAVING);
    /* Not stored: it stays in RAM, still dirty. Touched meanwhile: it stayed, dirty already. */
    if (result || !(marks & WM_MARK_LEAVING))
    {
        memory->marks[index] |= WM_MARK_DIRTY;
        return;
    }
    ring_remove(memory, index);
    frame_free(memory->frames[index]);
    memory->frames[index] = WM_FRAME_OUT;
    --g_resident;
}

void wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result)
{
    if (!memory->marks || index >= memory->frame_count || !(memory->marks[index] & WM_MARK_LOADING))
        return;
    memory->marks[index] &= (uint16_t)~WM_MARK_LOADING;
    if (result)
    {
        ring_remove(memory, index);
        frame_free(memory->frames[index]);
        memory->frames[index] = WM_FRAME_OUT;
        --g_resident;
    }
}

const char wm_page_emulate[] = "emulate the access";

WmResult wm_page_fault(WmTlb* tlb, WmMemory* memory, uint32_t address, uint32_t offset, uint32_t size, int write)
{
    uint64_t end = (uint64_t)address + offset + size;
    uint32_t ea = address + offset;
    uint8_t* frame;
    WmTlbEntry* e;
    if (end > WM_MEMORY_SIZE(memory))
        return E_OOB;
    /* A TLB entry holds aligned accesses within one frame of a memory with a number. */
    /* Frames of the access in storage come back in the background, also for one made through the memory functions. */
    if (memory->marks)
    {
        WmResult first = start_loading(memory, ea >> WM_FRAME_LOG2);
        WmResult last = (uint32_t)(end - 1u) >> WM_FRAME_LOG2 != ea >> WM_FRAME_LOG2
            ? start_loading(memory, (uint32_t)(end - 1u) >> WM_FRAME_LOG2) : NULL;
        if (first && first != wm_page_wait)
            return first;
        if (first || last)
            return last && last != wm_page_wait ? last : wm_page_wait;
    }
    if (memory->space == WM_SPACE_UNCACHED || (ea & (size - 1u)) || (ea & (WM_FRAME - 1u)) + size > WM_FRAME)
        return wm_page_emulate;
    frame = wm_memory_frame(memory, ea >> WM_FRAME_LOG2, write);
    if (!frame)
        return g_frame_error ? g_frame_error : E_MEMORY;
    e = &tlb->entries[(ea >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];
    e->tag = (ea & WM_FRAME_MASK) | ((uint32_t)memory->space << 3);
    /* Stores go through once the frame is dirty (or nothing stores it). */
    e->wtag = !memory->marks || (memory->marks[ea >> WM_FRAME_LOG2] & WM_MARK_DIRTY) ? e->tag : WM_TLB_EMPTY;
    e->frame = frame;
    return NULL;
}

/* ---- Taking frames out of RAM ---- */


uint32_t wm_memory_frames(const WmMemory* memory) { return memory->data ? 0 : memory->frame_count; }

uint32_t wm_memory_stored(const WmMemory* memory)
{
    uint32_t bytes = 0;
    for (uint32_t i = 0; !memory->data && i < memory->frame_count; ++i)
        if (frame_out(memory->frames[i]))
            bytes += WM_FRAME;
    return bytes;
}

int wm_memory_frame_look(WmMemory* memory, uint32_t index)
{
    int touched;
    if (!memory->pager || index >= memory->frame_count || !frame_here(memory->frames[index]) ||
        frame_in_block(memory, index) || (memory->marks[index] & (WM_MARK_PINS | WM_MARK_LOADING | WM_MARK_WRITING)))
        return -1;
    touched = (memory->marks[index] & WM_MARK_TOUCHED) != 0;
    /* The next touch must miss the TLB to mark the frame again. */
    memory->marks[index] &= (uint16_t)~WM_MARK_TOUCHED;
    tlb_forget_frame(memory->space, index);
    return touched;
}

WmResult wm_memory_frame_take(WmMemory* memory, uint32_t index)
{
    uint32_t slot = 0;
    WmResult result;
    uint8_t* frame;
    if (wm_memory_frame_look(memory, index) < 0)
        return WM_ERR_UNSUPPORTED("unsupported: this frame cannot leave RAM");
    frame = memory->frames[index];
    slot = memory->slots[index];
    /* Only a dirty frame is written; a clean one has its copy, or was never written (zeros). */
    if (memory->marks[index] & WM_MARK_DIRTY)
    {
        /* In the background, it stays in RAM, out of the TLB, until the write ends. */
        if (memory->pager->write_start)
        {
            result = memory->pager->write_start(memory->pager->context, &slot, frame, memory, index);
            if (result == wm_page_wait)
            {
                tlb_forget_frame(memory->space, index);
                memory->slots[index] = slot;
                memory->marks[index] = (uint16_t)((memory->marks[index] & ~WM_MARK_DIRTY) | WM_MARK_WRITING | WM_MARK_LEAVING);
                return wm_page_wait;
            }
        }
        else
            result = memory->pager->write(memory->pager->context, &slot, frame);
        if (result)
            return result;
        memory->slots[index] = slot;
        memory->marks[index] &= (uint16_t)~WM_MARK_DIRTY;
    }
    ring_remove(memory, index);
    memory->frames[index] = slot != WM_NONE ? WM_FRAME_OUT : NULL;
    frame_free(frame);
    --g_resident;
    return NULL;
}

uint32_t wm_memory_block_frames(const WmMemory* memory) { return is_pager_block(memory) ? memory->frame_count : 0u; }

/* Without a pool: a block from the heap, the frames copied or read into it. */
static WmResult heap_block_in(WmMemory* memory)
{
    uint8_t* data;
    if (!(data = (uint8_t*)wm_calloc(((size_t)memory->frame_count << WM_FRAME_LOG2) + 1u, 1)))
        return E_MEMORY;
    /* Every frame into the block first: until all are, the frames stay the memory. */
    for (uint32_t i = 0; i < memory->frame_count; ++i)
    {
        uint8_t* to = data + ((size_t)i << WM_FRAME_LOG2);
        WmResult result = NULL;
        if (frame_here(memory->frames[i]))
            memcpy(to, memory->frames[i], WM_FRAME);
        else if (frame_out(memory->frames[i]))
            result = memory->pager->read(memory->pager->context, memory->slots[i], to);
        if (result)
        {
            wm_free(data);
            return result;
        }
    }
    for (uint32_t i = 0; i < memory->frame_count; ++i)
    {
        free_frame(memory, i);
        memory->marks[i] = 0;
    }
    /* No frame of it in the TLB any more, and no address space number needed. */
    space_release(memory);
    memory->data = data;
    g_resident += memory->frame_count;
    return NULL;
}

/* Marks a frame has while something else is using it: it stays where it is. */
#define WM_MARK_BUSY (WM_MARK_PINS | WM_MARK_LOADING | WM_MARK_WRITING | WM_MARK_LEAVING)

/*
 * A memory a call in progress uses: an instance of one of the functions on
 * the call stack of a runtime that is running or in a host function. The
 * host may hold pointers into it, so its frames and block do not move.
 */
static int memory_in_use(const WmMemory* memory)
{
#if WM_TRACKS_CALLERS
    for (const WmRuntime* rt = wm_runtimes(); rt; rt = rt->next)
    {
        if (!rt->nesting)
            continue;
        if (rt->caller && rt->caller->instance && rt->caller->instance->memory == memory)
            return 1;
        for (const WmNest* n = rt->nest; n; n = n->up)
            if (n->caller && n->caller->instance && n->caller->instance->memory == memory)
                return 1;
        for (uint32_t i = 0; i < rt->depth; ++i)
            if (rt->frames[i].func && rt->frames[i].func->instance && rt->frames[i].func->instance->memory == memory)
                return 1;
    }
#else
    (void)memory;
#endif
    return 0;
}

/* The owner of pool frame slot finds it there again: its frame table, and no TLB entry left for the old place. */
static void pool_point(uint32_t slot)
{
    WmPoolOwner* o = &g_pool_owner[slot];
    if (o->memory && !o->memory->data)
    {
        o->memory->frames[o->index] = pool_frame(slot);
        tlb_forget_frame(o->memory->space, o->index);
    }
}

/* Two frames of the pool change places, bytes and owners. */
static void pool_swap(uint32_t a, uint32_t b)
{
    uint8_t* x = pool_frame(a);
    uint8_t* y = pool_frame(b);
    WmPoolOwner owner = g_pool_owner[a];
    for (uint32_t i = 0; i < WM_FRAME; i += 64u)
    {
        uint8_t keep[64];
        memcpy(keep, x + i, 64u);
        memcpy(x + i, y + i, 64u);
        memcpy(y + i, keep, 64u);
    }
    g_pool_owner[a] = g_pool_owner[b];
    g_pool_owner[b] = owner;
    pool_point(a);
    pool_point(b);
}

/* A free frame of the pool outside from..to (inclusive), or else inside after to; WM_NONE when none is. */
static uint32_t pool_free_slot(uint32_t from, uint32_t to, uint32_t end)
{
    for (uint32_t s = 0; s < g_pool_frames; ++s)
        if ((s < from || s >= end) && !g_pool_owner[s].memory)
            return s;
    for (uint32_t s = to + 1u; s < end; ++s)
        if (!g_pool_owner[s].memory)
            return s;
    return WM_NONE;
}

/*
 * With a pool: the memory's frames in a row in it. A window of the pool is
 * chosen that holds nothing that may not move (another block, a frame in
 * use or busy) and the fewest frames of other memories; then frame j of the
 * memory is put in place j of the window, other frames moving out of the
 * way, and those still to come are read from storage or zeroed. No heap,
 * and no storage when the frames are in RAM.
 */
static WmResult pool_block_in(WmMemory* memory)
{
    uint32_t n = memory->frame_count, best = WM_NONE, best_cost = WM_NONE, free_count = 0, own = 0, here = 0;
    uint8_t* fixed;
    if (n > g_pool_frames)
        return E_MEMORY;
    if (!(fixed = (uint8_t*)wm_malloc(g_pool_frames + 1u)))
        return E_MEMORY;
    for (uint32_t s = 0; s < g_pool_frames; ++s)
    {
        WmMemory* m = g_pool_owner[s].memory;
        free_count += !m;
        own += m == memory;
        fixed[s] = m && m != memory && (m->data || (m->marks[g_pool_owner[s].index] & WM_MARK_BUSY) || memory_in_use(m));
    }
    /* Room for the frames it has not in RAM yet. */
    if (free_count < n - own)
    {
        wm_free(fixed);
        return E_MEMORY;
    }
    for (uint32_t s = 0; s + n <= g_pool_frames; ++s)
    {
        uint32_t cost = 0, j = 0;
        for (; j < n && !fixed[s + j]; ++j)
            if (g_pool_owner[s + j].memory && !(g_pool_owner[s + j].memory == memory && g_pool_owner[s + j].index == j))
                ++cost;
        if (j == n && cost < best_cost)
            best = s, best_cost = cost;
    }
    wm_free(fixed);
    /* Something that may not move is in the way of every window: later, when it may. */
    if (best == WM_NONE)
        return wm_page_wait;
    for (uint32_t j = 0; j < n; ++j)
    {
        uint32_t t = best + j;
        uint8_t* frame = memory->frames[j];
        if (frame_here(frame) && in_pool(frame))
        {
            if (pool_slot(frame) != t)
                pool_swap(t, pool_slot(frame));
            continue;
        }
        if (g_pool_owner[t].memory)
        {
            uint32_t to = pool_free_slot(best, t, best + n);
            if (to == WM_NONE)
                return E_MEMORY;
            pool_swap(t, to);
        }
        /* Taken for frame j, to be read or zeroed below; frames[j] still says where it is. */
        g_pool_owner[t].memory = memory;
        g_pool_owner[t].index = j;
    }
    /* The frames not in RAM come in: from storage, or zero. */
    for (uint32_t j = 0; j < n; ++j)
    {
        uint8_t* to = pool_frame(best + j);
        uint8_t* frame = memory->frames[j];
        WmResult result = NULL;
        if (frame_here(frame) && in_pool(frame))
        {
            ++here;
            continue;
        }
        if (frame_here(frame))
            memcpy(to, frame, WM_FRAME);
        else if (frame_out(frame))
            result = memory->pager->read(memory->pager->context, memory->slots[j], to);
        else
            memset(to, 0, WM_FRAME);
        if (result)
        {
            /* The places taken for frames still to come are free again; the memory stays paged, its frames moved. */
            for (uint32_t k = 0; k < n; ++k)
                if (!(frame_here(memory->frames[k]) && in_pool(memory->frames[k])))
                    g_pool_owner[best + k].memory = NULL;
            return result;
        }
    }
    /* The memory is its block now: frames, ring places, storage slots and marks go. */
    for (uint32_t j = 0; j < n; ++j)
    {
        uint8_t* frame = memory->frames[j];
        if (frame_here(frame))
        {
            ring_remove(memory, j);
            if (!in_pool(frame))
            {
                wm_free(frame);
                --g_resident;
            }
        }
        if (memory->slots[j] != WM_NONE)
        {
            memory->pager->drop(memory->pager->context, memory->slots[j]);
            memory->slots[j] = WM_NONE;
        }
        memory->frames[j] = NULL;
        memory->marks[j] = 0;
    }
    space_release(memory);
    memory->data = pool_frame(best);
    g_resident += n - here;
    return NULL;
}

WmResult wm_memory_block_in(WmMemory* memory)
{
    if (memory->data)
        return NULL;
    if (!memory->pager || memory->block_frames)
        return WM_ERR_UNSUPPORTED("unsupported: only a memory with a pager goes back into one block");
    for (uint32_t i = 0; i < memory->frame_count; ++i)
        if (memory->marks[i] & WM_MARK_BUSY)
            return wm_page_wait;
    return g_pool ? pool_block_in(memory) : heap_block_in(memory);
}

/* A frame of bytes all zero: it needs no copy in storage, as an untouched frame reads as zero. */
static int frame_is_zero(const uint8_t* frame)
{
    for (uint32_t i = 0; i < WM_FRAME; ++i)
        if (frame[i])
            return 0;
    return 1;
}

WmResult wm_memory_block_out(WmMemory* memory)
{
    uint8_t* data = memory->data;
    WmResult result = NULL;
    if (!is_pager_block(memory))
        return NULL;
    for (uint32_t i = 0; i < memory->frame_count; ++i)
        if (memory->marks[i] & WM_MARK_PINS)
            return WM_ERR_EXHAUSTION("exhaustion: a host holds part of the memory");
    /* Each frame is written from the block as it is; until all are, the block stays the memory. */
    for (uint32_t i = 0; i < memory->frame_count && !result; ++i)
    {
        const uint8_t* frame = data + ((size_t)i << WM_FRAME_LOG2);
        if (!frame_is_zero(frame) && !(result = memory->pager->write(memory->pager->context, &memory->slots[i], frame)))
            memory->frames[i] = WM_FRAME_OUT;
    }
    if (result)
    {
        /* The frames stored so far let their slots go: nothing of the block stays in storage. */
        for (uint32_t i = 0; i < memory->frame_count; ++i)
        {
            if (frame_out(memory->frames[i]))
            {
                memory->pager->drop(memory->pager->context, memory->slots[i]);
                memory->slots[i] = WM_NONE;
            }
            memory->frames[i] = NULL;
        }
        return result;
    }
    memory->data = NULL;
    if (in_pool(data))
        pool_release_block(data, memory->frame_count);
    else
        wm_free(data);
    g_resident -= memory->frame_count;
    for (uint32_t i = 0; i < memory->frame_count; ++i)
        memory->marks[i] = 0;
    space_renew(memory);
    return NULL;
}

WmResult wm_memory_pin(WmMemory* memory, uint32_t address, uint32_t length, int delta)
{
    if (!in_bounds(memory, address, length))
        return E_OOB;
    /* A block with a pager counts its pins too: it does not leave RAM while one is held. */
    if (!memory->marks || !length)
        return NULL;
    for (uint32_t i = address >> WM_FRAME_LOG2; i <= (address + length - 1u) >> WM_FRAME_LOG2; ++i)
    {
        if (delta > 0)
        {
            /* A host with a pointer may write: the frame counts as dirty. */
            int full = (memory->marks[i] & WM_MARK_PINS) == WM_MARK_PINS;
            if (full || (!memory->data && !wm_memory_frame(memory, i, 1)))
            {
                /* Unpin what this call pinned. */
                while (i-- > address >> WM_FRAME_LOG2)
                    --memory->marks[i];
                return full || !g_frame_error ? E_MEMORY : g_frame_error;
            }
            ++memory->marks[i];
        }
        else if (memory->marks[i] & WM_MARK_PINS)
            --memory->marks[i];
    }
    return NULL;
}

#else

void wm_set_pager(const WmPager* pager) { (void)pager; }
void wm_set_frame_pool(uint8_t* memory, uint32_t frames) { (void)memory, (void)frames; }
uint32_t wm_pager_resident(void) { return 0; }
uint32_t wm_frame_size(void) { return 0; }
int wm_pager_hand(WmMemory** memory, uint32_t* index) { (void)memory, (void)index; return 0; }
void wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result) { (void)memory, (void)index, (void)result; }
void wm_memory_frame_stored(WmMemory* memory, uint32_t index, WmResult result) { (void)memory, (void)index, (void)result; }
WmMemory* wm_memory_next(WmMemory* after) { (void)after; return NULL; }
uint32_t wm_memory_frames(const WmMemory* memory) { (void)memory; return 0; }
uint32_t wm_memory_stored(const WmMemory* memory) { (void)memory; return 0; }
uint32_t wm_memory_block_frames(const WmMemory* memory) { (void)memory; return 0; }
WmResult wm_memory_block_out(WmMemory* memory) { (void)memory; return NULL; }
WmResult wm_memory_block_in(WmMemory* memory) { (void)memory; return NULL; }
int wm_memory_frame_look(WmMemory* memory, uint32_t index) { (void)memory, (void)index; return -1; }
WmResult wm_memory_frame_take(WmMemory* memory, uint32_t index)
{
    (void)memory, (void)index;
    return WM_ERR_UNSUPPORTED("unsupported: built without WM_HAS_MMU");
}
WmResult wm_memory_pin(WmMemory* memory, uint32_t address, uint32_t length, int delta)
{
    (void)memory, (void)address, (void)length, (void)delta;
    return NULL;
}

#endif

/* ---- Reading and writing across frames ---- */

WmResult wm_memory_pointer(WmMemory* memory, uint32_t address, uint32_t length, uint8_t** out)
{
    *out = NULL;
    if (!in_bounds(memory, address, length))
        return E_OOB;
    if (memory->data)
    {
        *out = memory->data + address;
        return NULL;
    }
#if WM_HAS_MMU
    if (length)
    {
        uint32_t first = address >> WM_FRAME_LOG2;
        uint32_t last = (address + length - 1u) >> WM_FRAME_LOG2;
        if (first == last)
        {
            uint8_t* frame = wm_memory_frame(memory, first, 1);
            if (!frame)
                return g_frame_error ? g_frame_error : E_MEMORY;
            *out = frame + (address & (WM_FRAME - 1u));
        }
        else if (memory->block && last < memory->block_frames)
            *out = memory->block + address;
    }
#endif
    return NULL;
}

WmResult wm_memory_read(const WmMemory* memory, uint32_t address, void* out, uint32_t length)
{
    uint8_t* to = (uint8_t*)out;
    if (!in_bounds(memory, address, length))
        return E_OOB;
    if (memory->data)
    {
        memcpy(to, memory->data + address, length);
        return NULL;
    }
#if WM_HAS_MMU
    while (length)
    {
        uint32_t at = address & (WM_FRAME - 1u);
        uint32_t n = WM_FRAME - at < length ? WM_FRAME - at : length;
        const uint8_t* frame = frame_to_read(memory, address >> WM_FRAME_LOG2);
        if (!frame)
            return g_frame_error ? g_frame_error : E_MEMORY;
        memcpy(to, frame + at, n);
        to += n, address += n, length -= n;
    }
#endif
    return NULL;
}

WmResult wm_memory_write(WmMemory* memory, uint32_t address, const void* data, uint32_t length)
{
    const uint8_t* from = (const uint8_t*)data;
    if (!in_bounds(memory, address, length))
        return E_OOB;
    if (memory->data)
    {
        memcpy(memory->data + address, from, length);
        return NULL;
    }
#if WM_HAS_MMU
    while (length)
    {
        uint32_t at = address & (WM_FRAME - 1u);
        uint32_t n = WM_FRAME - at < length ? WM_FRAME - at : length;
        uint8_t* frame = wm_memory_frame(memory, address >> WM_FRAME_LOG2, 1);
        if (!frame)
            return g_frame_error ? g_frame_error : E_MEMORY;
        memcpy(frame + at, from, n);
        from += n, address += n, length -= n;
    }
#endif
    return NULL;
}

WmResult wm_memory_fill(WmMemory* memory, uint32_t address, uint8_t value, uint32_t length)
{
    if (!in_bounds(memory, address, length))
        return E_OOB;
    if (memory->data)
    {
        memset(memory->data + address, value, length);
        return NULL;
    }
#if WM_HAS_MMU
    while (length)
    {
        uint32_t at = address & (WM_FRAME - 1u);
        uint32_t n = WM_FRAME - at < length ? WM_FRAME - at : length;
        uint32_t index = address >> WM_FRAME_LOG2;
        /* Zeros on an untouched frame need no frame. */
        if (value || memory->frames[index])
        {
            uint8_t* frame = wm_memory_frame(memory, index, 1);
            if (!frame)
                return g_frame_error ? g_frame_error : E_MEMORY;
            memset(frame + at, value, n);
        }
        address += n, length -= n;
    }
#endif
    return NULL;
}

WmResult wm_memory_copy(WmMemory* memory, uint32_t to, uint32_t from, uint32_t length)
{
    uint8_t chunk[64];
    if (!in_bounds(memory, to, length) || !in_bounds(memory, from, length))
        return E_OOB;
    if (memory->data)
    {
        memmove(memory->data + to, memory->data + from, length);
        return NULL;
    }
    /* In chunks, from the end when the destination is after the source, so overlaps copy right. */
    while (length)
    {
        uint32_t n = length < sizeof(chunk) ? length : (uint32_t)sizeof(chunk);
        uint32_t at = to <= from ? 0 : length - n;
        WmResult result;
        if ((result = wm_memory_read(memory, from + at, chunk, n)) || (result = wm_memory_write(memory, to + at, chunk, n)))
            return result;
        if (to <= from)
            to += n, from += n;
        length -= n;
    }
    return NULL;
}

