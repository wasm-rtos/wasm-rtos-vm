/*
 * The engine's heap (decision 20): TLSF with code from the top. Blocks of
 * every size allocated and freed in random order, the heap checked after
 * each step and whole again at the end; realloc in place and moving, data
 * kept; code blocks above all others; and the pattern that broke CoreMark
 * on the Black Pill (programs, stacks and buffers that stay, code that
 * comes and goes) leaving the free memory in one piece. Larger tests
 * (fuzzing, every platform) come later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm_heap.h"
#include "test.h"

static unsigned g_seed = 12345;

static unsigned rnd(unsigned n)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) % n;
}

static void fill(unsigned char* p, size_t n, unsigned tag)
{
    for (size_t i = 0; i < n; ++i)
        p[i] = (unsigned char)(tag + i);
}

static int same(const unsigned char* p, size_t n, unsigned tag)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i] != (unsigned char)(tag + i))
            return 0;
    return 1;
}

enum
{
    SLOTS = 200
};

/* The Black Pill's pattern: programs' memory, stacks and kernel objects that stay; code chunks and buffers that come and go. */
static size_t pattern(int code_high, size_t* largest, size_t* free_bytes)
{
    static unsigned char arena[110 * 1024];
    WmHeap* h = wm_heap_new(arena, sizeof(arena));
    void* stay[64] = {0};
    void* code[64] = {0};
    WmHeapStats st;
    int n_stay = 0, n_code = 0, failed = 0;
    for (int round = 0; round < 40; ++round)
    {
        void* buffer = wm_heap_malloc(h, 1024 + rnd(1024)); /* a translator's buffer, gone soon */
        void* c = code_high ? wm_heap_malloc_high(h, 2600 + rnd(5000)) : wm_heap_malloc(h, 2600 + rnd(5000));
        if (c && n_code < 64)
            code[n_code++] = c;
        else if (!c)
            ++failed;
        if (round % 3 == 0 && n_stay < 64)
            stay[n_stay++] = wm_heap_malloc(h, round % 9 == 0 ? 4096 : 200 + rnd(600)); /* a stack, an object */
        wm_heap_free(h, buffer);
    }
    /* The code area compacts and gives its chunks back (memory before code). */
    for (int i = 0; i < n_code; ++i)
        wm_heap_free(h, code[i]);
    wm_heap_stats(h, &st);
    *largest = st.largest;
    *free_bytes = st.free;
    (void)stay;
    return (size_t)failed;
}

int main(void)
{
    static unsigned char arena[256 * 1024];
    WmHeap* h = wm_heap_new(arena, sizeof(arena));
    WmHeapStats st, start;
    void* p[SLOTS] = {0};
    size_t size[SLOTS] = {0};
    int ok = 1;

    check(h != NULL && wm_heap_check(h), "a heap in 256 KB");
    wm_heap_stats(h, &start);

    /* Random sizes, random order, every block's data intact. */
    for (int step = 0; step < 20000 && ok; ++step)
    {
        int i = (int)rnd(SLOTS);
        if (p[i])
        {
            ok &= same(p[i], size[i], (unsigned)i);
            if (rnd(3) == 0)
            {
                size_t grown = 1 + rnd(rnd(8) == 0 ? 8000 : 300);
                void* q = wm_heap_realloc(h, p[i], grown);
                if (q)
                {
                    ok &= same(q, size[i] < grown ? size[i] : grown, (unsigned)i);
                    p[i] = q;
                    size[i] = grown;
                    fill(p[i], size[i], (unsigned)i);
                }
            }
            else
            {
                wm_heap_free(h, p[i]);
                p[i] = NULL;
            }
        }
        else
        {
            size[i] = 1 + rnd(rnd(8) == 0 ? 8000 : 300);
            p[i] = rnd(4) == 0 ? wm_heap_malloc_high(h, size[i]) : wm_heap_malloc(h, size[i]);
            if (p[i])
                fill(p[i], size[i], (unsigned)i);
        }
        if (step % 97 == 0)
            ok &= wm_heap_check(h);
    }
    check(ok && wm_heap_check(h), "20 000 random allocations, reallocations and frees: data intact, heap consistent");
    for (int i = 0; i < SLOTS; ++i)
        wm_heap_free(h, p[i]), p[i] = NULL;
    wm_heap_stats(h, &st);
    check(wm_heap_check(h) && st.blocks == 0 && st.used == 0 && st.free == start.free && st.largest == start.largest,
          "all freed: one free block again, as at the start");

    /* realloc grows in place into a free neighbour. */
    {
        void* a = wm_heap_malloc(h, 100);
        void* b = wm_heap_malloc(h, 100);
        void* c = wm_heap_malloc(h, 100);
        fill(a, 100, 7);
        wm_heap_free(h, b);
        check(wm_heap_realloc(h, a, 180) == a && same(a, 100, 7), "realloc grows in place into the free block after it");
        check(wm_heap_realloc(h, a, 40) == a && same(a, 40, 7), "and shrinks in place");
        wm_heap_free(h, a);
        wm_heap_free(h, c);
    }

    /* Code from the top, the rest from the bottom. */
    {
        void* low[8];
        void* high[8];
        int above = 1;
        for (int i = 0; i < 8; ++i)
        {
            low[i] = wm_heap_malloc(h, 500 + 100 * (size_t)i);
            high[i] = wm_heap_malloc_high(h, 3000);
        }
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                above &= (char*)high[i] > (char*)low[j];
        check(above, "code blocks lie above all the others");
        {
            /* A hole low down that would fit: code still goes to the top. */
            void* hole = low[2];
            void* code;
            low[2] = NULL;
            wm_heap_free(h, hole);
            code = wm_heap_malloc_high(h, 600);
            check(code && (char*)code > (char*)low[7], "code passes a hole below that fits, for the highest free block");
            wm_heap_free(h, code);
        }
        {
            void* moved = wm_heap_realloc(h, high[3], 9000); /* no room above it: it moves, from the top */
            check(moved && (char*)moved > (char*)low[7], "a code block that moves stays at the top");
            high[3] = moved;
        }
        for (int i = 0; i < 8; ++i)
            wm_heap_free(h, low[i]), wm_heap_free(h, high[i]);
        wm_heap_stats(h, &st);
        check(wm_heap_check(h) && st.largest == start.largest, "freed: whole again");
    }

    /* The Black Pill's pattern, 110 KB: with code from the top the memory it leaves is one piece. */
    {
        size_t largest_high, free_high, largest_low, free_low;
        size_t failed_high = pattern(1, &largest_high, &free_high);
        size_t failed_low = pattern(0, &largest_low, &free_low);
        printf("     code from the top: %zu refused, %zu free, largest piece %zu\n", failed_high, free_high, largest_high);
        printf("     code mixed in:     %zu refused, %zu free, largest piece %zu\n", failed_low, free_low, largest_low);
        check(largest_high * 10 >= free_high * 9, "with code from the top, the free memory is (almost) one piece");
        check(largest_high > largest_low, "larger than with code mixed in");
    }

    return summary();
}
