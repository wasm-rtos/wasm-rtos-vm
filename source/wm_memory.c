/*
 * Linear memory. A memory is one block when the heap has room for it; when
 * it has not, it is paged: 4 KiB frames anywhere in the heap, so it never
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
static uint32_t g_spaces[512u / 32u] = {1u};
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
            if (((t->entries[i].tag >> 3) & 0x1FFu) == space)
                t->entries[i].tag = t->entries[i].wtag = WM_TLB_EMPTY;
}

static void space_release(WmMemory* memory)
{
    if (!memory->space)
        return;
    tlb_forget(memory->space);
    g_spaces[memory->space / 32u] &= ~(1u << (memory->space % 32u));
    memory->space = 0;
}

/* A fresh number for a paged memory; none left leaves it uncached, which is still correct. */
static void space_renew(WmMemory* memory)
{
    if (!g_tlbs_ready)
    {
        wm_tlb_init(&wm_tlb_default);
        g_tlbs_ready = 1;
    }
    space_release(memory);
    for (uint32_t n = 1; n < 512u; ++n)
        if (!(g_spaces[n / 32u] & (1u << (n % 32u))))
        {
            g_spaces[n / 32u] |= 1u << (n % 32u);
            memory->space = (uint16_t)n;
            return;
        }
}

/* ---- Pager ---- */

#define WM_MARK_TOUCHED 0x80u
#define WM_MARK_DIRTY 0x40u
/* Being read in the background: in RAM, but not to be used before it ends. */
#define WM_MARK_LOADING 0x20u
#define WM_MARK_PINS 0x1Fu
/* A frame slot whose frame is in the pager's storage. */
#define WM_FRAME_OUT ((uint8_t*)(uintptr_t)1u)

static const WmPager* g_pager;
/* Memories with a pager, and their frames in RAM. */
static WmMemory* g_swapped;
static uint32_t g_resident;

void wm_set_pager(const WmPager* pager) { g_pager = pager; }

uint32_t wm_pager_resident(void) { return g_resident; }

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

/* A zeroed frame coming into RAM; with a pager, once it has made room. */
static uint8_t* new_frame(const WmPager* pager)
{
    uint8_t* frame;
    if (!pager)
        return (uint8_t*)wm_calloc(WM_FRAME, 1);
    if (pager->room && !pager->room(pager->context, 0))
        return NULL;
    while (!(frame = (uint8_t*)wm_calloc(WM_FRAME, 1)))
        if (!pager->room || !pager->room(pager->context, 1))
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
    memory->marks[index] &= (uint8_t)~WM_MARK_LOADING;
    if (!memory->pager->finish(memory->pager->context, memory->slots[index]))
        return 1;
    wm_free(memory->frames[index]);
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
    /* No read may go on into a frame that is freed. */
    (void)finish_loading(memory, index);
    frame = memory->frames[index];
    if (memory->slots && memory->slots[index] != WM_NONE)
    {
        memory->pager->drop(memory->pager->context, memory->slots[index]);
        memory->slots[index] = WM_NONE;
    }
    if (frame_here(frame))
    {
        wm_free(frame);
        if (memory->pager)
            --g_resident;
    }
    memory->frames[index] = NULL;
}

uint8_t* wm_memory_frame(WmMemory* memory, uint32_t index, int write)
{
    uint8_t* frame;
    if (memory->data)
        return memory->data + ((wm_size)index << WM_FRAME_LOG2);
    if (index >= memory->frame_count)
        return NULL;
    if (!finish_loading(memory, index))
        return NULL;
    frame = memory->frames[index];
    if (!frame_here(frame))
    {
        /* Untouched, or in the pager's storage: into RAM now. */
        uint8_t* made = new_frame(memory->pager);
        if (!made)
            return NULL;
        /* Back from storage, its slot keeps a copy: clean until written. */
        if (frame_out(memory->frames[index]) && memory->pager->read(memory->pager->context, memory->slots[index], made))
        {
            wm_free(made);
            --g_resident;
            return NULL;
        }
        memory->frames[index] = frame = made;
    }
    if (memory->marks)
        memory->marks[index] |= (uint8_t)(WM_MARK_TOUCHED | (write ? WM_MARK_DIRTY : 0u));
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
    uint8_t** frames = (uint8_t**)wm_calloc(frame_count + 1u, sizeof(uint8_t*));
    if (!frames)
        return 0;
    for (uint32_t i = 0; i < whole; ++i)
        frames[i] = memory->data + ((wm_size)i << WM_FRAME_LOG2);
    if (size & (WM_FRAME - 1u))
    {
        frames[whole] = (uint8_t*)wm_calloc(WM_FRAME, 1);
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
    memory->marks = (uint8_t*)wm_calloc(memory->frame_count + 1u, 1);
    memory->slots = (uint32_t*)wm_malloc(((size_t)memory->frame_count + 1u) * sizeof(uint32_t));
    if (!memory->marks || !memory->slots || !g_pager->reserve(g_pager->context, (int32_t)reserved))
    {
        wm_free(memory->marks);
        wm_free(memory->slots);
        memory->marks = NULL;
        memory->slots = NULL;
        return 0;
    }
    memset(memory->slots, 0xFF, ((size_t)memory->frame_count + 1u) * sizeof(uint32_t));
    /* Frames it has already (a copied partial frame) have no copy in storage. */
    for (uint32_t i = memory->block_frames; i < memory->frame_count; ++i)
        if (frame_here(memory->frames[i]))
        {
            ++g_resident;
            memory->marks[i] = WM_MARK_DIRTY;
        }
    memory->pager = g_pager;
    memory->next = g_swapped;
    g_swapped = memory;
    return 1;
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
        memory->frames[i] = (uint8_t*)wm_calloc(WM_FRAME, 1);
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
    frames = (uint8_t**)wm_realloc(memory->frames, ((size_t)frame_count + 1u) * sizeof(uint8_t*));
    if (!frames)
        return 0;
    memset(frames + memory->frame_count, 0, (size_t)(frame_count - memory->frame_count) * sizeof(uint8_t*));
    memory->frames = frames;
    if (memory->marks)
    {
        uint8_t* marks = (uint8_t*)wm_realloc(memory->marks, (size_t)frame_count + 1u);
        if (!marks)
            return 0;
        memset(marks + memory->frame_count, 0, (size_t)(frame_count - memory->frame_count));
        memory->marks = marks;
    }
    if (memory->slots)
    {
        uint32_t* slots = (uint32_t*)wm_realloc(memory->slots, ((size_t)frame_count + 1u) * sizeof(uint32_t));
        if (!slots)
            return 0;
        memset(slots + memory->frame_count, 0xFF, (size_t)(frame_count - memory->frame_count) * sizeof(uint32_t));
        memory->slots = slots;
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
    /* With a pager every memory is paged, so any frame may leave RAM. */
    if (!WM_MEMORY_ALWAYS_PAGED && !g_pager)
        mem->data = (uint8_t*)wm_calloc((size_t)size + 1u, 1);
    if (!mem->data)
    {
        /* No block that large: paged, the frames made now, or reserved and made when touched. */
        uint32_t count = frames_for(size);
        mem->frames = (uint8_t**)wm_calloc(count + 1u, sizeof(uint8_t*));
        mem->frame_count = count;
        if (!mem->frames || !(g_pager ? take_pager(mem) : make_frames(mem, 0, count)))
        {
            wm_free(mem->frames);
            wm_free(mem);
            return NULL;
        }
        space_renew(mem);
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
    for (uint32_t i = 0; mem->frames && i < mem->frame_count; ++i)
        free_frame(mem, i);
    leave_pager(mem);
    wm_free(mem->frames);
    wm_free(mem->marks);
    wm_free(mem->slots);
    wm_free(mem->block);
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
    if (mem->data)
    {
        uint8_t* data = (uint8_t*)wm_realloc(mem->data, (size_t)size + 1u);
        if (data)
        {
            memset(data + ((wm_size)old << mem->page_log2), 0, (size_t)((wm_size)delta << mem->page_log2));
            mem->data = data;
            mem->pages = pages;
            return old;
        }
#if WM_HAS_MMU
        /* No room to grow in one block: the memory goes on paged, swapped too when there is a pager. */
        if (!make_paged(mem, frames_for((wm_size)old << mem->page_log2)) || !take_pager(mem))
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
    if (!(made = new_frame(memory->pager)))
        return E_MEMORY;
    result = memory->pager->read_start(memory->pager->context, memory->slots[index], made, memory, index);
    if (result && result != wm_page_wait)
    {
        wm_free(made);
        --g_resident;
        return result;
    }
    memory->frames[index] = made;
    if (result)
        memory->marks[index] |= WM_MARK_LOADING;
    return result;
}

void wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result)
{
    if (!memory->marks || index >= memory->frame_count || !(memory->marks[index] & WM_MARK_LOADING))
        return;
    memory->marks[index] &= (uint8_t)~WM_MARK_LOADING;
    if (result)
    {
        wm_free(memory->frames[index]);
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
    if (!memory->space || (ea & (size - 1u)) || (ea & (WM_FRAME - 1u)) + size > WM_FRAME)
        return wm_page_emulate;
    frame = wm_memory_frame(memory, ea >> WM_FRAME_LOG2, write);
    if (!frame)
        return E_MEMORY;
    e = &tlb->entries[(ea >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];
    e->tag = (ea & 0xFFFFF000u) | ((uint32_t)memory->space << 3);
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
        frame_in_block(memory, index) || (memory->marks[index] & (WM_MARK_PINS | WM_MARK_LOADING)))
        return -1;
    touched = (memory->marks[index] & WM_MARK_TOUCHED) != 0;
    /* The next touch must miss the TLB to mark the frame again. */
    memory->marks[index] &= (uint8_t)~WM_MARK_TOUCHED;
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
        if ((result = memory->pager->write(memory->pager->context, &slot, frame)))
            return result;
        memory->slots[index] = slot;
        memory->marks[index] &= (uint8_t)~WM_MARK_DIRTY;
    }
    memory->frames[index] = slot != WM_NONE ? WM_FRAME_OUT : NULL;
    wm_free(frame);
    --g_resident;
    return NULL;
}

WmResult wm_memory_pin(WmMemory* memory, uint32_t address, uint32_t length, int delta)
{
    if (!in_bounds(memory, address, length))
        return E_OOB;
    if (memory->data || !memory->marks || !length)
        return NULL;
    for (uint32_t i = address >> WM_FRAME_LOG2; i <= (address + length - 1u) >> WM_FRAME_LOG2; ++i)
    {
        if (delta > 0)
        {
            /* A host with a pointer may write: the frame counts as dirty. */
            if ((memory->marks[i] & WM_MARK_PINS) == WM_MARK_PINS || !wm_memory_frame(memory, i, 1))
            {
                /* Unpin what this call pinned. */
                while (i-- > address >> WM_FRAME_LOG2)
                    --memory->marks[i];
                return E_MEMORY;
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
uint32_t wm_pager_resident(void) { return 0; }
void wm_memory_frame_loaded(WmMemory* memory, uint32_t index, WmResult result) { (void)memory, (void)index, (void)result; }
WmMemory* wm_memory_next(WmMemory* after) { (void)after; return NULL; }
uint32_t wm_memory_frames(const WmMemory* memory) { (void)memory; return 0; }
uint32_t wm_memory_stored(const WmMemory* memory) { (void)memory; return 0; }
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
                return E_MEMORY;
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
            return E_MEMORY;
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
            return E_MEMORY;
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
                return E_MEMORY;
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

