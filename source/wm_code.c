/*
 * The code cache: the records of predecoded functions (wm_fast.c), made when
 * a function is first called and dropped when room is needed. The hand of a
 * CLOCK goes round the functions of every module that runs predecoded; a
 * call marks its function used, the hand clears the mark and drops the
 * records of a function it finds unmarked. Functions on a call stack are
 * never dropped: the frames of every runtime, where a suspended one
 * stopped, and the callers of host functions that called back in.
 *
 * Room is made only outside the fast loop (at a call that stopped for
 * records, or a host function), where all of that is written down.
 */
#include "wm_internal.h"

#include <string.h>

#if WM_PREDECODE

const char wm_code_stop[] = "code miss";
const char wm_code_alone[] = "code in place";
const char wm_code_back[] = "code back in place";

static WmModule* g_modules;
static size_t g_bytes;
static size_t g_limit;
static WmAllocator g_allocator;
/* The CLOCK hand: a function of a module in g_modules. */
static WmModule* g_hand_module;
static uint32_t g_hand;

static uint32_t defined(const WmModule* m) { return m->func_count - m->func_imports; }

void wm_set_code_allocator(WmAllocator allocator) { g_allocator = allocator; }
void wm_code_cache_limit(size_t bytes) { g_limit = bytes; }
size_t wm_code_cache_bytes(void) { return g_bytes; }

void* wm_code_malloc(size_t size) { return g_allocator ? g_allocator(NULL, size ? size : 1u) : wm_malloc(size); }

void* wm_code_realloc(void* ptr, size_t size)
{
    return g_allocator ? g_allocator(ptr, size ? size : 1u) : wm_realloc(ptr, size ? size : 1u);
}

void wm_code_free(void* ptr)
{
    if (!ptr)
        return;
    if (g_allocator)
        g_allocator(ptr, 0);
    else
        wm_free(ptr);
}

static void wm_code_drop(WmCode* code)
{
#if WM_PREDECODE
    /* A function that ran in place by itself has a tail without records (decision 21). */
    wm_reg_tail_free(code->tail);
    code->tail = NULL;
#endif
    if (!code->fast)
        return;
    if (!code->mapped)
    {
        wm_area_free(code->own);
        g_bytes -= (size_t)code->fast_cells * sizeof(WmCell);
    }
    code->fast = NULL;
    code->fast_cells = 0;
    code->used = 0;
    code->mapped = 0;
}

#if WM_CHECKS
static void area_check_owner_gone(const WmModule* m);
#else
#define area_check_owner_gone(m) ((void)0)
#endif
void wm_code_add_module(WmModule* m)
{
    m->cached_next = g_modules;
    g_modules = m;
}

void wm_code_remove_module(WmModule* m)
{
    for (WmModule** at = &g_modules; *at; at = &(*at)->cached_next)
        if (*at == m)
        {
            *at = m->cached_next;
            break;
        }
    if (g_hand_module == m)
    {
        g_hand_module = NULL;
        g_hand = 0;
    }
    for (uint32_t i = 0; i < defined(m); ++i)
        wm_code_drop(&m->codes[i]);
    area_check_owner_gone(m);
}

/* Pinning counts (decision 18): the area compacts inside code that pinned the stacks already. */
static void pin_func(const WmFunc* f, uint8_t on)
{
    if (f && f->code)
    {
        if (on)
            ++f->code->pinned;
        else
            --f->code->pinned;
    }
}

/* Mark (or unmark) every function on a call stack; calls nest, as long as the stacks do not change between. */
static void pin_stacks(uint8_t on)
{
    for (WmRuntime* rt = wm_runtimes(); rt; rt = rt->next)
    {
        for (uint32_t i = 0; i < rt->depth; ++i)
            pin_func(rt->frames[i].func, on);
        if (rt->suspended)
            pin_func(rt->saved.func, on);
        if (rt->nesting)
        {
            pin_func(rt->caller, on);
            for (const WmNest* n = rt->nest; n; n = n->up)
                pin_func(n->caller, on);
        }
    }
}

/* The function at the hand, the hand then one on; only when some module has functions. */
static WmCode* hand_next(void)
{
    for (;;)
    {
        if (!g_hand_module)
        {
            g_hand_module = g_modules;
            g_hand = 0;
        }
        if (g_hand < defined(g_hand_module))
            return &g_hand_module->codes[g_hand++];
        g_hand_module = g_hand_module->cached_next;
        g_hand = 0;
    }
}

/* Drop the records of one function, by CLOCK, with the stacks pinned; 0 when none can go. */
static int evict_one(void)
{
    uint32_t total = 0;
    for (const WmModule* m = g_modules; m; m = m->cached_next)
        total += defined(m);
    /* Twice round: the first pass may only clear marks. */
    for (uint32_t i = 0; i < 2u * total; ++i)
    {
        WmCode* code = hand_next();
        /* Records in the image take no RAM: nothing to gain. */
        if (!code->fast || code->pinned || code->mapped)
            continue;
        if (code->used)
        {
            code->used = 0;
            continue;
        }
        wm_code_drop(code);
        return 1;
    }
    return 0;
}

/*
 * Room for an allocation the heap refused: first a tail the loop in place
 * kept (made again in a moment when needed), then records by CLOCK. With
 * the stacks pinned; 0 when nothing can go.
 */
static int make_room(void)
{
    for (WmModule* m = g_modules; m; m = m->cached_next)
        for (uint32_t i = 0; i < defined(m); ++i)
            if (m->codes[i].tail && !m->codes[i].pinned)
            {
                wm_reg_tail_free(m->codes[i].tail);
                m->codes[i].tail = NULL;
                return 1;
            }
    return evict_one();
}

/*
 * The area (decision 18): records live in chunks of the code allocator.
 * A chunk holds spans one after another from its start to its top: a span
 * is a header (its owner, its cells) and a function's records; a dropped
 * one stays as a hole (no owner) until compaction slides the spans after
 * it down, and into earlier chunks, and sets each owner's own; a pinned
 * span stays. Records need nothing changed to move: handlers are offsets,
 * branches relative, calls by function index. A chunk is walked from
 * header to header, so compaction costs the area's size, not the number
 * of functions.
 *
 * At most one span is being written at a time (wm_area_begin ... _end, the
 * translator's): its owner is BUILD, it lies at its chunk's top, moves
 * with compaction, and the translator takes its address back each time.
 */
typedef struct WmChunk
{
    struct WmChunk* next;
    /* In cells: room, the end of the last span, the cells of spans with their headers. */
    uint32_t size, top, live;
} WmChunk;

typedef struct WmSpan
{
    WmCode* owner;
    uint32_t cells;
} WmSpan;

#define SPAN_CELLS ((uint32_t)((sizeof(WmSpan) + sizeof(WmCell) - 1u) / sizeof(WmCell)))
#define BUILD ((WmCode*)(uintptr_t)1)

#ifndef WM_CODE_SLACK
/* Cells a chunk keeps unused at its top before they go back (256 bytes). */
#define WM_CODE_SLACK 64u
#endif
#ifndef WM_CODE_MOVE_ALWAYS
#define WM_CODE_MOVE_ALWAYS 0
#endif

static WmChunk* g_chunks;
static size_t g_area;
/* The span being written: its chunk (NULL: none) and where its header is. */
static WmChunk* g_build;
static uint32_t g_build_at;

size_t wm_code_area_bytes(void) { return g_area; }

#if WM_CHECKS
#include <stdio.h>
#include <stdlib.h>
/* Checking builds: the area walks right, every owner points at its span, the counts agree. */
static void area_verify(const char* where)
{
    for (WmChunk* c = g_chunks; c; c = c->next)
    {
        uint32_t at = 0, live = 0;
        while (at < c->top)
        {
            WmSpan* h = (WmSpan*)(void*)((WmCell*)(void*)(c + 1) + at);
            if (c->top - at < SPAN_CELLS || h->cells > c->top - at - SPAN_CELLS || (h->owner && h->owner != BUILD && h->owner->own != (WmCell*)(void*)h + SPAN_CELLS) ||
                (h->owner == BUILD && (g_build != c || g_build_at != at)))
            {
                fprintf(stderr, "wm code area broken (%s): span at %u of %u\n", where, at, c->top);
                abort();
            }
            if (h->owner)
                live += SPAN_CELLS + h->cells;
            at += SPAN_CELLS + h->cells;
        }
        if (at != c->top || live != c->live || c->top > c->size)
        {
            fprintf(stderr, "wm code area broken (%s): walk %u top %u size %u live %u counted %u\n", where, at, c->top,
                    c->size, c->live, live);
            abort();
        }
    }
}

/* Checking builds: a module leaving the cache leaves no span. */
static void area_check_owner_gone(const WmModule* m)
{
    for (WmChunk* c = g_chunks; c; c = c->next)
        for (uint32_t at = 0; at < c->top; at += SPAN_CELLS + ((WmSpan*)(void*)((WmCell*)(void*)(c + 1) + at))->cells)
        {
            const WmCode* owner = ((WmSpan*)(void*)((WmCell*)(void*)(c + 1) + at))->owner;
            if (owner && owner != BUILD && owner >= m->codes && owner < m->codes + defined(m))
            {
                fprintf(stderr, "wm code area broken: a span of a module that left\n");
                abort();
            }
        }
}
#else
#define area_verify(where) ((void)0)
#define area_check_owner_gone(m) ((void)0)
#endif

static WmCell* chunk_cells(WmChunk* c) { return (WmCell*)(void*)(c + 1); }
static size_t chunk_bytes(uint32_t cells) { return sizeof(WmChunk) + (size_t)cells * sizeof(WmCell); }
static WmSpan* span_at(WmChunk* c, uint32_t at) { return (WmSpan*)(void*)(chunk_cells(c) + at); }
static uint32_t span_len(const WmSpan* h) { return SPAN_CELLS + h->cells; }
static WmSpan* span_of(WmCell* records) { return (WmSpan*)(void*)(records - SPAN_CELLS); }

static WmChunk* chunk_of(const WmCell* p)
{
    for (WmChunk* c = g_chunks; c; c = c->next)
        if (p >= chunk_cells(c) && p < chunk_cells(c) + c->size)
            return c;
    return NULL;
}

/* The owner of a span takes its new place. */
static void span_placed(WmChunk* c, uint32_t at)
{
    WmSpan* h = span_at(c, at);
    if (h->owner == BUILD)
    {
        g_build = c;
        g_build_at = at;
    }
    else if (h->owner)
        h->owner->own = chunk_cells(c) + at + SPAN_CELLS;
}

static int span_stays(const WmSpan* h) { return h->owner && h->owner != BUILD && h->owner->pinned; }

static int chunk_has_pinned(WmChunk* c)
{
    for (uint32_t at = 0; at < c->top; at += span_len(span_at(c, at)))
        if (span_stays(span_at(c, at)))
            return 1;
    return 0;
}

/* The end of the last span with an owner: holes at the top are no span. */
static void chunk_settle_top(WmChunk* c)
{
    uint32_t end = 0;
    for (uint32_t at = 0; at < c->top; at += span_len(span_at(c, at)))
        if (span_at(c, at)->owner)
            end = at + span_len(span_at(c, at));
    c->top = end;
}

static void chunk_unlink(WmChunk* c)
{
    for (WmChunk** at = &g_chunks; *at; at = &(*at)->next)
        if (*at == c)
        {
            *at = c->next;
            return;
        }
}

static void chunk_free(WmChunk* c)
{
    chunk_unlink(c);
    g_area -= chunk_bytes(c->size);
    wm_code_free(c);
}

/*
 * A chunk's unused top given back, when it is more than a few cells: only
 * a chunk with no pinned span and not being written into, as the allocator
 * may answer a smaller size with a new address (then every span's owner
 * follows). With the stacks pinned.
 */
static void chunk_cut(WmChunk* c)
{
    WmChunk* cut;
    WmChunk** link;
    uint32_t size = c->size, keep = c->top ? c->top : 1u;
    if (size - c->top <= WM_CODE_SLACK / 4u || c == g_build || chunk_has_pinned(c))
        return;
    for (link = &g_chunks; *link != c; link = &(*link)->next)
        ;
    if (!(cut = (WmChunk*)wm_code_realloc(c, chunk_bytes(keep))))
        return;
    *link = cut;
    cut->size = keep;
    g_area -= chunk_bytes(size) - chunk_bytes(keep);
    if (cut != c)
        for (uint32_t at = 0; at < cut->top; at += span_len(span_at(cut, at)))
            span_placed(cut, at);
}

/* A new chunk for at least need cells: half of what the area holds if the heap has it, else just need. */
static WmChunk* chunk_new(uint32_t need)
{
    WmChunk* c;
    WmChunk** at = &g_chunks;
    size_t held = 0;
    uint32_t cells = need;
    for (WmChunk* k = g_chunks; k; k = k->next)
        held += k->size;
    if (cells < held / 2u)
        cells = (uint32_t)(held / 2u);
    if (cells < WM_CODE_SLACK)
        cells = WM_CODE_SLACK;
    if (!(c = (WmChunk*)wm_code_malloc(chunk_bytes(cells))) && cells > need)
        c = (WmChunk*)wm_code_malloc(chunk_bytes(cells = need));
    if (!c)
        return NULL;
    c->next = NULL;
    c->size = cells;
    c->top = 0;
    c->live = 0;
    while (*at)
        at = &(*at)->next;
    *at = c;
    g_area += chunk_bytes(cells);
    return c;
}

/* A span of cells records at the top of a chunk with room (not the one written into): its header, or NULL. */
static WmSpan* chunk_put(uint32_t cells, WmCode* owner, WmChunk** out_chunk, uint32_t* out_at)
{
    uint32_t len = SPAN_CELLS + cells;
    for (WmChunk* c = g_chunks; c; c = c->next)
        if (c != g_build && c->size - c->top >= len)
        {
            WmSpan* h = span_at(c, c->top);
            h->owner = owner;
            h->cells = cells;
            *out_chunk = c;
            *out_at = c->top;
            c->top += len;
            c->live += len;
            return h;
        }
    return NULL;
}

/*
 * Spans slid down over the holes in each chunk (a hole kept before a span
 * that stays), then the spans of later chunks into earlier ones with room,
 * so the last ones may empty and go. With the stacks pinned.
 */
static void compact(void)
{
    for (WmChunk* c = g_chunks; c; c = c->next)
    {
        uint32_t cursor = 0, at = 0;
        while (at < c->top)
        {
            WmSpan* h = span_at(c, at);
            uint32_t len = span_len(h), next = at + len;
            if (!h->owner)
            {
                at = next;
                continue;
            }
            if (span_stays(h))
            {
                if (at > cursor)
                {
                    /* What could not slide in front of it stays a hole. */
                    WmSpan* hole = span_at(c, cursor);
                    hole->owner = NULL;
                    hole->cells = at - cursor - SPAN_CELLS;
                }
                cursor = next;
            }
            else
            {
                if (at > cursor)
                {
                    memmove(chunk_cells(c) + cursor, chunk_cells(c) + at, (size_t)len * sizeof(WmCell));
                    span_placed(c, cursor);
                }
                cursor += len;
            }
            at = next;
        }
        c->top = cursor;
    }
    for (WmChunk* late = g_chunks ? g_chunks->next : NULL; late; late = late->next)
        for (uint32_t at = 0; at < late->top; at += span_len(span_at(late, at)))
        {
            WmSpan* h = span_at(late, at);
            uint32_t len = span_len(h);
            if (!h->owner || h->owner == BUILD || h->owner->pinned)
                continue;
            for (WmChunk* early = g_chunks; early != late; early = early->next)
                if (early != g_build && early->size - early->top >= len)
                {
                    memcpy(chunk_cells(early) + early->top, h, (size_t)len * sizeof(WmCell));
                    span_placed(early, early->top);
                    early->top += len;
                    early->live += len;
                    late->live -= len;
                    h->owner = NULL;
                    break;
                }
        }
    for (WmChunk* c = g_chunks; c;)
    {
        WmChunk* next = c->next;
        if (!c->live && c != g_build)
            chunk_free(c);
        else
            chunk_settle_top(c);
        c = next;
    }
}

/* Cells the area could give by compacting: room at the tops and in the holes. */
static size_t area_free_cells(void)
{
    size_t free_cells = 0;
    for (WmChunk* c = g_chunks; c; c = c->next)
        free_cells += c->size - c->live;
    return free_cells;
}

#if WM_CODE_MOVE_ALWAYS
/* Tests only: every unpinned span, the one written included, to a new chunk; the old places filled with a pattern. */
static void move_always(void)
{
    uint32_t total = 0;
    WmChunk* fresh;
    for (WmChunk* c = g_chunks; c; c = c->next)
        for (uint32_t at = 0; at < c->top; at += span_len(span_at(c, at)))
            if (span_at(c, at)->owner && !span_stays(span_at(c, at)))
                total += span_len(span_at(c, at));
    if (!total || !(fresh = (WmChunk*)wm_code_malloc(chunk_bytes(total))))
        return;
    fresh->size = total;
    fresh->top = 0;
    fresh->live = 0;
    fresh->next = NULL;
    for (WmChunk* c = g_chunks; c; c = c->next)
        for (uint32_t at = 0; at < c->top; at += span_len(span_at(c, at)))
        {
            WmSpan* h = span_at(c, at);
            uint32_t len = span_len(h);
            WmCode* owner = h->owner;
            if (!owner || span_stays(h))
                continue;
            memcpy(chunk_cells(fresh) + fresh->top, h, (size_t)len * sizeof(WmCell));
            memset((WmCell*)(void*)h + SPAN_CELLS, 0xA5, (size_t)h->cells * sizeof(WmCell));
            h->owner = NULL;
            c->live -= len;
            span_placed(fresh, fresh->top);
            fresh->top += len;
            fresh->live += len;
        }
    fresh->next = g_chunks;
    g_chunks = fresh;
    g_area += chunk_bytes(total);
    for (WmChunk* c = g_chunks; c;)
    {
        WmChunk* next = c->next;
        if (!c->live && c != g_build)
            chunk_free(c);
        c = next;
    }
}
#endif

/*
 * A span of cells records for owner: at a top with room, else after
 * compaction when the area has the room in holes, else in a new chunk,
 * else dropping records by CLOCK and trying again. Its header, or NULL when
 * nothing more can go. The stacks pinned meanwhile.
 */
static WmSpan* area_take(uint32_t cells, WmCode* owner, WmChunk** out_chunk, uint32_t* out_at)
{
    WmSpan* h = NULL;
    pin_stacks(1);
#if WM_CODE_MOVE_ALWAYS
    move_always();
#endif
    for (;;)
    {
        if ((h = chunk_put(cells, owner, out_chunk, out_at)))
            break;
        if (area_free_cells() >= SPAN_CELLS + cells)
        {
            compact();
            if ((h = chunk_put(cells, owner, out_chunk, out_at)))
                break;
        }
        if (chunk_new(SPAN_CELLS + cells) && (h = chunk_put(cells, owner, out_chunk, out_at)))
            break;
        if (!make_room())
            break;
    }
    pin_stacks(0);
    return h;
}

WmCell* wm_area_alloc(WmCode* owner, uint32_t cells)
{
    area_verify(__func__);
    WmChunk* c;
    uint32_t at;
    WmSpan* h = area_take(cells ? cells : 1u, owner, &c, &at);
    return h ? (WmCell*)(void*)h + SPAN_CELLS : NULL;
}

void wm_area_free(WmCell* records)
{
    area_verify(__func__);
    WmChunk* c = records ? chunk_of(records) : NULL;
    WmSpan* h;
    if (!c)
        return;
    h = span_of(records);
#if WM_CODE_MOVE_ALWAYS
    memset(records, 0xA5, (size_t)h->cells * sizeof(WmCell));
#endif
    h->owner = NULL;
    c->live -= span_len(h);
    if ((WmCell*)(void*)h + span_len(h) == chunk_cells(c) + c->top)
        chunk_settle_top(c);
    if (!c->live && c != g_build)
        chunk_free(c);
}

WmCell* wm_area_begin(uint32_t cells)
{
    area_verify(__func__);
    WmChunk* c;
    uint32_t at;
    if (!area_take(cells, BUILD, &c, &at))
        return NULL;
    g_build = c;
    g_build_at = at;
    return chunk_cells(c) + at + SPAN_CELLS;
}

/* The span being written grows in place when it is at its chunk's top and the chunk has room for cells. */
static int build_grows(uint32_t cells)
{
    WmChunk* c = g_build;
    WmSpan* h = span_at(c, g_build_at);
    if (g_build_at + span_len(h) != c->top || c->size - g_build_at < SPAN_CELLS + cells)
        return 0;
    c->live += cells - h->cells;
    h->cells = cells;
    c->top = g_build_at + span_len(h);
    return 1;
}

/*
 * The span being written, with room for cells now: in place, else after
 * compaction (which may move it), else in a chunk of its own with what was
 * written copied once, else dropping records and trying again. Its first
 * cell, or NULL (it stays as it was) when nothing can make room.
 */
WmCell* wm_area_grow(uint32_t cells)
{
    WmCell* p = NULL;
    if (!g_build)
        return NULL;
    pin_stacks(1);
#if WM_CODE_MOVE_ALWAYS
    move_always();
#endif
    for (;;)
    {
        WmChunk* c;
        WmChunk* fresh;
        WmSpan* h;
        if (build_grows(cells))
            break;
        if (area_free_cells() >= cells)
        {
            compact();
            if (build_grows(cells))
                break;
        }
        c = g_build;
        h = span_at(c, g_build_at);
        if ((fresh = chunk_new(SPAN_CELLS + cells)))
        {
            uint32_t len = span_len(h);
            memcpy(chunk_cells(fresh), h, (size_t)len * sizeof(WmCell));
            h->owner = NULL;
            c->live -= len;
            chunk_settle_top(c);
            g_build = fresh;
            g_build_at = 0;
            fresh->top = len;
            fresh->live = len;
            if (!c->live)
                chunk_free(c);
            if (build_grows(cells))
                break;
        }
        if (!make_room())
        {
            pin_stacks(0);
            return NULL;
        }
    }
    p = chunk_cells(g_build) + g_build_at + SPAN_CELLS;
    pin_stacks(0);
    return p;
}

/*
 * The span written, cut to count cells and given to its owner: its first
 * cell (NULL: none was begun). It is its chunk's last span, so the top
 * follows its end.
 */
WmCell* wm_area_end(WmCode* owner, uint32_t count)
{
    area_verify(__func__);
    WmChunk* c = g_build;
    WmSpan* h;
    if (!c)
        return NULL;
    h = span_at(c, g_build_at);
    c->live -= h->cells - count;
    h->cells = count;
    h->owner = owner;
    c->top = g_build_at + span_len(h);
    g_build = NULL;
    return chunk_cells(c) + g_build_at + SPAN_CELLS;
}

void wm_area_cancel(void)
{
    area_verify(__func__);
    WmChunk* c = g_build;
    WmSpan* h;
    if (!c)
        return;
    h = span_at(c, g_build_at);
    c->live -= span_len(h);
    c->top = g_build_at;
    g_build = NULL;
    chunk_settle_top(c);
    if (!c->live)
        chunk_free(c);
}

/* After a span found its owner: a chunk whose unused top is more than a quarter of the area gives it back. */
void wm_area_settle(void)
{
    size_t held = 0;
    for (WmChunk* k = g_chunks; k; k = k->next)
        held += k->size;
    for (WmChunk* c = g_chunks; c;)
    {
        WmChunk* next = c->next;
        if ((size_t)(c->size - c->top) * 4u > held && c->size - c->top > WM_CODE_SLACK)
        {
            pin_stacks(1);
            chunk_cut(c);
            pin_stacks(0);
        }
        c = next;
    }
}

/* Every chunk's unused top back to the heap, empty chunks freed (memory before code); 1 when the heap got some. */
static int area_give_back(void)
{
    size_t before = g_area;
    if (!g_chunks)
        return 0;
    pin_stacks(1);
    compact();
    for (WmChunk* c = g_chunks; c;)
    {
        WmChunk* next = c->next;
        chunk_cut(c);
        c = next;
    }
    pin_stacks(0);
    return g_area < before;
}

/* A function's records copied from the image into the area, dropping others for the room; with none, it runs from the image. */
static void from_image(WmModule* m, WmCode* code)
{
    uint32_t i = (uint32_t)(code - m->codes);
    const WmCell* cells = m->image + m->image_index[i];
    uint32_t count = m->image_index[i + 1u] - m->image_index[i];
    WmCell* copy;
    ++code->pinned;
    copy = wm_area_alloc(code, count);
    --code->pinned;
    code->fast_cells = (uint16_t)count;
    code->mapped = !copy;
    if (copy)
    {
        memcpy(copy, cells, (size_t)count * sizeof(WmCell));
        code->own = copy;
        wm_area_settle();
    }
    else
        code->fast = cells;
}

/*
 * A function's records, dropping others while there is no memory for them.
 * side: its side table entries, NULL to make them again. Not counted in
 * the cache yet. With a code image they are copied from it, or read there.
 */
WmResult wm_code_make(WmModule* m, WmCode* code, const WmSide* side)
{
    WmResult result;
    if (m->image && !side)
    {
        from_image(m, code);
        return NULL;
    }
    pin_stacks(1);
    ++code->pinned;
    do
    {
        WmSide* made = NULL;
        if (side || !(result = wm_code_side(m, (uint32_t)(code - m->codes), &made)))
            result = wm_predecode(m, code, side ? side : made);
        wm_free(made);
    } while (result && !strncmp(result, "exhaustion", 10) && make_room());
    --code->pinned;
    pin_stacks(0);
    if (!result)
        wm_area_settle();
    return result;
}

WmResult wm_code_load(WmFunc* func)
{
    WmModule* m = func->instance->module;
    WmCode* code = func->code;
    WmResult result;
    if (code->fast)
        return NULL;
    if (!WM_RUNS_FAST(m))
        return WM_ERR_TRAP("trap: a call into code that is not predecoded");
    if ((result = wm_code_make(m, code, NULL)))
        return result;
    code->used = 1;
    if (code->mapped)
        return NULL;
    g_bytes += (size_t)code->fast_cells * sizeof(WmCell);
    if (g_limit && g_bytes > g_limit)
    {
        pin_stacks(1);
        ++code->pinned;
        while (g_bytes > g_limit && evict_one())
            ;
        --code->pinned;
        pin_stacks(0);
    }
    return NULL;
}

/*
 * Memory before code: the heap is short. The area's unused tops and empty
 * chunks go back first, keeping every function; else one function's
 * records (or a tail) go, and what that frees in the area goes back too.
 * 1 when something went, 0 when nothing could.
 */
int wm_code_drop_one(void)
{
    int dropped;
    if (area_give_back())
        return 1;
    pin_stacks(1);
    dropped = make_room();
    pin_stacks(0);
    if (dropped)
        (void)area_give_back();
    return dropped;
}

size_t wm_code_cache_trim(size_t keep)
{
    pin_stacks(1);
    while (g_bytes > keep && evict_one())
        ;
    /*
     * Functions that found no room run from the image until here: their next
     * call copies them in if there is room. Tails the loop in place kept go.
     */
    for (WmModule* m = g_modules; m; m = m->cached_next)
        for (uint32_t i = 0; i < defined(m); ++i)
            if (!m->codes[i].pinned)
            {
                if (m->codes[i].mapped)
                    wm_code_drop(&m->codes[i]);
                wm_reg_tail_free(m->codes[i].tail);
                m->codes[i].tail = NULL;
            }
    pin_stacks(0);
    return g_bytes;
}

/*
 * Code images (decision 15): every function's records, then where each
 * starts (in cells, and the end), then a trailer, so the image is written
 * in one pass, as a ZIP file's directory comes last. Words in the
 * processor's own order: an image is only for the build that wrote it.
 */
enum
{
    IMAGE_MAGIC = 0x49434D57u, /* "WMCI" */
    IMAGE_VERSION = 2u,
    IMAGE_TRAILER = 8u
};

/* What a record's meaning depends on besides the module: the handlers' offsets and the flags predecoding follows. */
static uint32_t build_tag(void)
{
    const uint16_t* handlers = wm_fast_handlers();
    const uint32_t flags[] = {IMAGE_VERSION,  (uint32_t)sizeof(void*), WM_R_COUNT, WM_REF_SLOTS, WM_FRAME_SIZE,
                              WM_HAS_FLOAT,   WM_HAS_I64,              WM_HAS_REF_TYPES, WM_PROFILE, WM_MAX_PAGES};
    uint32_t crc = wm_crc32(0xFFFFFFFFu, flags, sizeof(flags));
    return ~wm_crc32(crc, handlers, WM_R_COUNT * (uint32_t)sizeof(uint16_t));
}

/* Predecoding is never built where program bytes need WM_RD8 (AVR): they are read as memory. */
static uint32_t module_crc(const WmModule* m) { return ~wm_crc32(0xFFFFFFFFu, m->bytes, m->size); }

typedef struct ImageOut
{
    WmSnapshotWrite write;
    void* context;
    WmResult error;
    uint32_t crc;
} ImageOut;

static void image_put(ImageOut* o, const void* data, uint32_t size)
{
    if (!o->error && size)
    {
        o->crc = wm_crc32(o->crc, data, size);
        o->error = o->write(o->context, data, size);
    }
}

WmResult wm_code_image_write(WmModule* m, WmSnapshotWrite write, void* context)
{
    uint32_t defined_count = defined(m), at = 0;
    uint32_t* index;
    uint32_t trailer[IMAGE_TRAILER];
    ImageOut o = {write, context, NULL, 0xFFFFFFFFu};
    wm_module_predecode(m);
    if (!WM_RUNS_FAST(m) || m->image)
        return WM_ERR_UNSUPPORTED("unsupported: a code image of a module not predecoded, or already mapped");
    index = (uint32_t*)wm_malloc(((size_t)defined_count + 1u) * sizeof(uint32_t));
    if (!index)
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    /* Each function's records as the cache has them, or made for the image and let go. */
    for (uint32_t i = 0; !o.error && i < defined_count; ++i)
    {
        WmCode* code = &m->codes[i];
        int made = !code->fast;
        if (made && (o.error = wm_code_make(m, code, NULL)))
            break;
        index[i] = at;
        at += code->fast_cells;
        image_put(&o, code->fast, code->fast_cells * (uint32_t)sizeof(WmCell));
        if (made)
        {
            wm_area_free(code->own);
            code->fast = NULL;
            code->fast_cells = 0;
        }
    }
    index[defined_count] = at;
    image_put(&o, index, (defined_count + 1u) * (uint32_t)sizeof(uint32_t));
    wm_free(index);
    trailer[0] = IMAGE_MAGIC;
    trailer[1] = IMAGE_VERSION;
    trailer[2] = build_tag();
    trailer[3] = module_crc(m);
    trailer[4] = m->size;
    trailer[5] = defined_count;
    trailer[6] = (at + defined_count + 1u + IMAGE_TRAILER) * (uint32_t)sizeof(uint32_t);
    image_put(&o, trailer, (IMAGE_TRAILER - 1u) * (uint32_t)sizeof(uint32_t));
    trailer[7] = ~o.crc;
    image_put(&o, &trailer[7], (uint32_t)sizeof(uint32_t));
    return o.error;
}

const char wm_code_image_invalid[] = "code image does not fit this module or build";

WmResult wm_code_image_map(WmModule* m, const void* image, uint32_t size)
{
    const uint32_t* words = (const uint32_t*)image;
    const uint32_t* trailer;
    const uint32_t* index;
    uint32_t defined_count = defined(m), cells;
    if (m->image || (m->predecode_tried && m->predecode_result))
        return WM_ERR_UNSUPPORTED("unsupported: a code image of a module not predecoded, or already mapped");
    if (!wm_fast_handlers())
        return WM_ERR_UNSUPPORTED("unsupported: handlers too far apart for predecoding");
    /* The trailer, then the index, then the records: each in bounds before it is read. */
    if (((uintptr_t)image & 3u) || (size & 3u) || size < IMAGE_TRAILER * 4u)
        return wm_code_image_invalid;
    trailer = words + size / 4u - IMAGE_TRAILER;
    if (trailer[0] != IMAGE_MAGIC || trailer[1] != IMAGE_VERSION || trailer[2] != build_tag() ||
        trailer[4] != m->size || trailer[5] != defined_count || trailer[6] != size ||
        size / 4u - IMAGE_TRAILER < defined_count + 1u)
        return wm_code_image_invalid;
    index = trailer - (defined_count + 1u);
    cells = (uint32_t)(index - words);
    if (index[0] != 0 || index[defined_count] != cells)
        return wm_code_image_invalid;
    for (uint32_t i = 0; i < defined_count; ++i)
        if (index[i + 1u] <= index[i] || index[i + 1u] - index[i] > 0x7FFFu)
            return wm_code_image_invalid;
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (~wm_crc32(0xFFFFFFFFu, image, size - 4u) != trailer[7] || trailer[3] != module_crc(m))
        return wm_code_image_invalid;
#endif
#if WM_HAS_VALIDATION
    /* A record is a jump into the loop: each function's are made again and compared, one at a time. */
    for (uint32_t i = 0; i < defined_count; ++i)
    {
        WmCode* code = &m->codes[i];
        uint32_t count = index[i + 1u] - index[i];
        int made = !code->fast, same;
        WmResult result;
        if (made && (result = wm_code_make(m, code, m->side ? m->side + code->side : NULL)))
            return result;
        same = code->fast_cells == count && !memcmp(code->fast, words + index[i], (size_t)count * sizeof(WmCell));
        if (made)
        {
            wm_area_free(code->own);
            code->fast = NULL;
            code->fast_cells = 0;
        }
        if (!same)
            return wm_code_image_invalid;
    }
#endif
    m->image = (const WmCell*)image;
    m->image_index = index;
    return NULL;
}

#else

const char wm_code_image_invalid[] = "code image does not fit this module or build";

WmResult wm_code_image_write(WmModule* module, WmSnapshotWrite write, void* context)
{
    (void)module, (void)write, (void)context;
    return WM_ERR_UNSUPPORTED("unsupported: built without predecoding");
}

WmResult wm_code_image_map(WmModule* module, const void* image, uint32_t size)
{
    (void)module, (void)image, (void)size;
    return WM_ERR_UNSUPPORTED("unsupported: built without predecoding");
}

int wm_code_drop_one(void) { return 0; }
void wm_set_code_allocator(WmAllocator allocator) { (void)allocator; }
void wm_code_cache_limit(size_t bytes) { (void)bytes; }
size_t wm_code_cache_bytes(void) { return 0; }
size_t wm_code_area_bytes(void) { return 0; }
size_t wm_code_cache_trim(size_t keep)
{
    (void)keep;
    return 0;
}

#endif
