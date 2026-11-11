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
        tlb->entries[i].tag = WM_TLB_EMPTY;
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
                t->entries[i].tag = WM_TLB_EMPTY;
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

uint8_t* wm_memory_frame(WmMemory* memory, uint32_t index)
{
    uint8_t* frame;
    if (memory->data)
        return memory->data + ((wm_size)index << WM_FRAME_LOG2);
    if (index >= memory->frame_count)
        return NULL;
    frame = memory->frames[index];
    if (!frame)
    {
        frame = (uint8_t*)wm_calloc(WM_FRAME, 1);
        memory->frames[index] = frame;
    }
    return frame;
}

/* A frame to read from: an untouched one reads as zeros, without being made. */
static const uint8_t* frame_to_read(const WmMemory* memory, uint32_t index)
{
    static const uint8_t zeros[WM_FRAME];
    if (memory->data)
        return memory->data + ((wm_size)index << WM_FRAME_LOG2);
    return memory->frames[index] ? memory->frames[index] : zeros;
}

uint8_t wm_memory_byte(const WmMemory* memory, uint32_t address)
{
    return frame_to_read(memory, address >> WM_FRAME_LOG2)[address & (WM_FRAME - 1u)];
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
#if WM_HAS_MMU && WM_MEMORY_ALWAYS_PAGED
    mem->data = NULL;
#else
    mem->data = (uint8_t*)wm_calloc((size_t)size + 1u, 1);
#endif
#if WM_HAS_MMU
    if (!mem->data)
    {
        /* No block that large: paged. */
        uint32_t count = frames_for(size);
        mem->frames = (uint8_t**)wm_calloc(count + 1u, sizeof(uint8_t*));
        if (!mem->frames || !make_frames(mem, 0, count))
        {
            wm_free(mem->frames);
            wm_free(mem);
            return NULL;
        }
        mem->frame_count = count;
        space_renew(mem);
    }
#else
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
        if (!frame_in_block(mem, i))
            wm_free(mem->frames[i]);
    wm_free(mem->frames);
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
        if (mem->frames[i] && !frame_in_block(mem, i))
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
        /* No room to grow in one block: the memory goes on paged. */
        if (!make_paged(mem, frames_for(size)))
            return WM_NONE;
#else
        return WM_NONE;
#endif
    }
#if WM_HAS_MMU
    /* The frames past the old end are all new: make them now, or memory.grow fails. */
    if (!grow_frames(mem, frames_for(size)) ||
        !make_frames(mem, frames_for((wm_size)old << mem->page_log2), frames_for(size)))
        return WM_NONE;
    mem->pages = pages;
    return old;
#else
    return WM_NONE;
#endif
}

#if WM_HAS_MMU

/* ---- Page faults ---- */

const char wm_page_emulate[] = "emulate the access";

WmResult wm_page_fault(WmTlb* tlb, WmMemory* memory, uint32_t address, uint32_t offset, uint32_t size)
{
    uint64_t end = (uint64_t)address + offset + size;
    uint32_t ea = address + offset;
    uint8_t* frame;
    WmTlbEntry* e;
    if (end > WM_MEMORY_SIZE(memory))
        return E_OOB;
    /* A TLB entry holds aligned accesses within one frame of a memory with a number. */
    if (!memory->space || (ea & (size - 1u)) || (ea & (WM_FRAME - 1u)) + size > WM_FRAME)
        return wm_page_emulate;
    frame = wm_memory_frame(memory, ea >> WM_FRAME_LOG2);
    if (!frame)
        return E_MEMORY;
    e = &tlb->entries[(ea >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];
    e->tag = (ea & 0xFFFFF000u) | ((uint32_t)memory->space << 3);
    e->frame = frame;
    return NULL;
}

#endif

/* ---- Reading and writing across frames ---- */

static int in_bounds(const WmMemory* memory, uint32_t address, uint32_t length)
{
    return memory && !wm_out_of_bounds(address, length, WM_MEMORY_SIZE(memory));
}

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
            uint8_t* frame = wm_memory_frame(memory, first);
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
        memcpy(to, frame_to_read(memory, address >> WM_FRAME_LOG2) + at, n);
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
        uint8_t* frame = wm_memory_frame(memory, address >> WM_FRAME_LOG2);
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
            uint8_t* frame = wm_memory_frame(memory, index);
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

