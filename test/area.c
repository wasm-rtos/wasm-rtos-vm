/*
 * The code area (decision 18): records in chunks of the code allocator,
 * moved together when there are holes. Spans are made and freed here
 * through the area's own calls, with owners standing in for functions;
 * what a span holds is checked after every move, and the area, the cache
 * and the heap must agree. Build with -DWM_CHECKS=1: the area then walks
 * itself on every call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm_internal.h"

static int g_failures;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/* The code allocator: counts the bytes it holds; with g_move_on_shrink every smaller size gets a new address. */
static size_t g_heap, g_limit = (size_t)-1;
static int g_move_on_shrink, g_moved_blocks;

static void* code_alloc(void* ptr, size_t size)
{
    size_t* block = ptr ? (size_t*)ptr - 1 : NULL;
    size_t old = block ? *block : 0;
    size_t* fresh;
    if (!size)
    {
        g_heap -= old;
        free(block);
        return NULL;
    }
    if (g_heap - old + size > g_limit)
        return NULL;
    if (block && size < old && g_move_on_shrink)
    {
        if (!(fresh = (size_t*)malloc(sizeof(size_t) + size)))
            return NULL;
        memcpy(fresh + 1, ptr, size);
        free(block);
        ++g_moved_blocks;
    }
    else if (!(fresh = (size_t*)realloc(block, sizeof(size_t) + size)))
        return NULL;
    *fresh = size;
    g_heap = g_heap - old + size;
    return fresh + 1;
}

/* Owners: what functions are to the area. A span's cells hold its owner's number and their index. */
enum
{
    OWNERS = 64
};
static WmCode g_owner[OWNERS];
static uint32_t g_cells[OWNERS];

static void fill(int i)
{
    for (uint32_t k = 0; k < g_cells[i]; ++k)
        g_owner[i].own[k].u32 = (uint32_t)i << 16 | k;
}

static int intact(int i)
{
    for (uint32_t k = 0; k < g_cells[i]; ++k)
        if (g_owner[i].own[k].u32 != ((uint32_t)i << 16 | k))
            return 0;
    return 1;
}

static int all_intact(void)
{
    for (int i = 0; i < OWNERS; ++i)
        if (g_owner[i].own && !intact(i))
            return 0;
    return 1;
}

static int make(int i, uint32_t cells)
{
    g_cells[i] = cells;
    if (!(g_owner[i].own = wm_area_alloc(&g_owner[i], cells)))
        return 0;
    fill(i);
    return 1;
}

static void drop(int i)
{
    wm_area_free(g_owner[i].own);
    g_owner[i].own = NULL;
}

static void drop_all(void)
{
    for (int i = 0; i < OWNERS; ++i)
        if (g_owner[i].own)
            drop(i);
    wm_area_settle();
}

int main(void)
{
    size_t before;
    int ok;
    wm_set_code_allocator(code_alloc);

    /* Holes: every other span freed; one larger than any hole and smaller than their sum. */
    ok = 1;
    for (int i = 0; i < 48; ++i)
        ok &= make(i, 6);
    check(ok && all_intact(), "48 spans made");
    check(wm_code_area_bytes() == g_heap, "the area's bytes are the heap's");
    for (int i = 1; i < 48; i += 2)
        drop(i);
    before = wm_code_area_bytes();
    check(make(48, 40), "a span larger than any hole, smaller than their sum");
    check(wm_code_area_bytes() <= before, "made by compaction, in no more memory");
    check(all_intact(), "every span moved whole");
    check(wm_code_area_bytes() == g_heap, "the area's bytes are the heap's");
    drop_all();
    check(g_heap == 0 && wm_code_area_bytes() == 0, "all given back");

    /* A pinned span keeps its address through compaction; the others around it move. */
    ok = 1;
    for (int i = 0; i < 32; ++i)
        ok &= make(i, 8);
    {
        WmCell* pinned_at = g_owner[9].own;
        WmCell* moved_from = g_owner[12].own;
        g_owner[9].pinned = 1;
        for (int i = 0; i < 9; ++i)
            drop(i);
        drop(10), drop(11);
        check(ok && make(40, 60), "a span that needs compaction, around a pinned one");
        check(g_owner[9].own == pinned_at, "the pinned span did not move");
        check(g_owner[12].own != moved_from, "the span after it did");
        check(all_intact(), "every span whole");
        g_owner[9].pinned = 0;
    }
    drop_all();
    check(g_heap == 0, "all given back");

    /* A span being written grows past its chunk: it moves, what was written comes along. */
    ok = 1;
    for (int i = 0; i < 8; ++i)
        ok &= make(i, 12);
    {
        WmCell* p = wm_area_begin(4);
        uint32_t cells = 4;
        int kept = p != NULL;
        for (uint32_t k = 0; p && k < cells; ++k)
            p[k].u32 = 0xB000u + k;
        drop(2), drop(5);
        while (p && cells < 400)
        {
            uint32_t grown = cells * 2;
            if (!(p = wm_area_grow(grown)))
                break;
            for (uint32_t k = 0; k < cells; ++k)
                kept &= p[k].u32 == 0xB000u + k;
            for (uint32_t k = cells; k < grown; ++k)
                p[k].u32 = 0xB000u + k;
            cells = grown;
        }
        check(ok && p && kept, "a span written while it grows to 512 cells keeps what was written");
        g_cells[60] = 300;
        g_owner[60].own = wm_area_end(&g_owner[60], 300);
        check(g_owner[60].own != NULL && g_owner[60].own[299].u32 == 0xB000u + 299, "cut to its count, its owner takes it");
        for (uint32_t k = 0; g_owner[60].own && k < 300; ++k)
            g_owner[60].own[k].u32 = 60u << 16 | k;
        wm_area_settle();
        check(all_intact(), "the others whole");
        check(wm_code_area_bytes() == g_heap, "the area's bytes are the heap's");
    }
    drop_all();
    check(g_heap == 0, "all given back");

    /*
     * Memory before code (what the memory calls when the heap refuses it),
     * with an allocator that moves every block it shrinks: the area compacts
     * and cuts its chunks; one with a pinned span is not cut, the others are,
     * their spans moved.
     */
    g_move_on_shrink = 1;
    ok = 1;
    for (int i = 0; i < 8; ++i)
        ok &= make(i, 40);
    {
        WmCell* pinned_at = g_owner[0].own;
        g_owner[0].pinned = 1;
        for (int i = 4; i < 8; ++i)
            drop(i);
        g_moved_blocks = 0;
        before = wm_code_area_bytes();
        check(ok && wm_code_drop_one() == 1 && wm_code_area_bytes() < before, "memory before code: the area gives bytes back");
        check(g_owner[0].own == pinned_at, "the pinned span's chunk is not cut");
        check(g_moved_blocks > 0, "chunks without a pinned span are cut, moved by the allocator");
        check(all_intact(), "their spans whole at their new places");
        check(wm_code_area_bytes() == g_heap, "the area's bytes are the heap's");
        g_owner[0].pinned = 0;
    }
    drop_all();
    g_move_on_shrink = 0;
    check(g_heap == 0, "all given back");

    /* A heap that refuses: the area makes what fits and says NULL for the rest, never more than the limit. */
    g_limit = 2048;
    ok = 1;
    {
        int made = 0;
        for (int i = 0; i < OWNERS; ++i)
            if (make(i, 20))
                ++made;
        check(made > 0 && made < OWNERS && g_heap <= g_limit, "a full heap: some spans, then NULL");
        check(all_intact(), "those made whole");
    }
    drop_all();
    g_limit = (size_t)-1;
    check(g_heap == 0 && wm_code_area_bytes() == 0, "all given back");

    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
