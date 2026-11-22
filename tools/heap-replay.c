/*
 * Replays an allocation trace through wm_heap (decision 20) and measures
 * what a heap is judged by: time per operation, bytes the heap keeps for
 * itself, and how whole the free memory stays. The trace is the board
 * checks' (test/heap-trace.txt, recorded on a computer over a Black Pill
 * sized heap): one line per call, "a|h|r id size" or "f id", h for code.
 *
 *   tools/heap-replay [trace] [arena bytes] [rounds]
 *   tools/heap-replay [trace] min       the smallest arena the trace fits whole, by bisection
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../source/wm_heap.h"

typedef struct Op
{
    char op;
    unsigned id;
    size_t size;
} Op;

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* Whether the trace runs in an arena of bytes with no allocation refused. */
static int fits(const Op* ops, size_t count, unsigned max_id, size_t bytes)
{
    unsigned char* arena = (unsigned char*)malloc(bytes);
    void** live = (void**)calloc(max_id + 1u, sizeof(void*));
    WmHeap* h = wm_heap_new(arena, bytes);
    int ok = h != NULL;
    for (size_t i = 0; ok && i < count; ++i)
    {
        const Op* o = &ops[i];
        void* p;
        if (o->op == 'a' || o->op == 'h')
            ok = (live[o->id] = o->op == 'h' ? wm_heap_malloc_high(h, o->size) : wm_heap_malloc(h, o->size)) != NULL;
        else if (o->op == 'r' && live[o->id])
            ok = (p = wm_heap_realloc(h, live[o->id], o->size)) != NULL, live[o->id] = ok ? p : live[o->id];
        else if (o->op == 'f' && live[o->id])
            wm_heap_free(h, live[o->id]), live[o->id] = NULL;
    }
    free(arena);
    free(live);
    return ok;
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "test/heap-trace.txt";
    size_t arena_bytes = argc > 2 ? (size_t)strtoul(argv[2], NULL, 0) : 106496u;
    int rounds = argc > 3 ? atoi(argv[3]) : 200;
    FILE* f = fopen(path, "r");
    Op* ops = NULL;
    size_t count = 0, cap = 0;
    unsigned max_id = 0;
    char line[64];
    if (!f)
        return fprintf(stderr, "no trace %s\n", path), 1;
    while (fgets(line, sizeof(line), f))
    {
        Op o = {0};
        unsigned long size = 0;
        if (sscanf(line, "%c %u %lu", &o.op, &o.id, &size) < 2)
            continue;
        o.size = size;
        if (count == cap && !(ops = (Op*)realloc(ops, (cap = cap ? cap * 2 : 4096) * sizeof(Op))))
            return 1;
        ops[count++] = o;
        if (o.id > max_id)
            max_id = o.id;
    }
    fclose(f);
    if (argc > 2 && !strcmp(argv[2], "min"))
    {
        size_t low = 4096, high = 64u << 20;
        while (high - low > 64)
        {
            size_t mid = (low + high) / 2;
            if (fits(ops, count, max_id, mid))
                high = mid;
            else
                low = mid;
        }
        printf("%zu ops: the smallest arena with nothing refused is %zu B\n", count, high);
        free(ops);
        return 0;
    }
    {
        unsigned char* arena = (unsigned char*)malloc(arena_bytes);
        void** live = (void**)calloc(max_id + 1u, sizeof(void*));
        size_t* asked = (size_t*)calloc(max_id + 1u, sizeof(size_t));
        size_t refused = 0, requested = 0, peak_requested = 0, peak_used = 0, worst_largest = (size_t)-1, worst_free = 0;
        double best = 1e30;
        WmHeapStats st;
        for (int round = 0; round < rounds; ++round)
        {
            WmHeap* h = wm_heap_new(arena, arena_bytes);
            double t0;
            memset(live, 0, (max_id + 1u) * sizeof(void*));
            requested = 0;
            t0 = now_ns();
            for (size_t i = 0; i < count; ++i)
            {
                Op* o = &ops[i];
                void* p;
                switch (o->op)
                {
                    case 'a':
                    case 'h':
                        p = o->op == 'h' ? wm_heap_malloc_high(h, o->size) : wm_heap_malloc(h, o->size);
                        live[o->id] = p;
                        if (p)
                            requested += asked[o->id] = o->size;
                        else if (round == 0)
                            ++refused;
                        break;
                    case 'r':
                        if (!live[o->id])
                            break;
                        if ((p = wm_heap_realloc(h, live[o->id], o->size)))
                        {
                            requested += o->size - asked[o->id];
                            asked[o->id] = o->size;
                            live[o->id] = p;
                        }
                        else if (round == 0)
                            ++refused;
                        break;
                    case 'f':
                        if (live[o->id])
                        {
                            wm_heap_free(h, live[o->id]);
                            requested -= asked[o->id];
                            live[o->id] = NULL;
                        }
                        break;
                }
                if (round == 0 && (i % 16) == 0)
                {
                    wm_heap_stats(h, &st);
                    if (requested > peak_requested)
                        peak_requested = requested, peak_used = st.used;
                    /* How whole the free memory is when more than a quarter of the arena is free. */
                    if (st.free > arena_bytes / 4 && st.largest < worst_largest)
                        worst_largest = st.largest, worst_free = st.free;
                }
            }
            if (round == 0 && !wm_heap_check(h))
                return fprintf(stderr, "heap broken\n"), 1;
            {
                double t = (now_ns() - t0) / (double)count;
                if (t < best)
                    best = t;
            }
        }
        printf("%zu ops, arena %zu B: %.1f ns per op (best of %d), refused %zu\n", count, arena_bytes, best, rounds, refused);
        printf("at the peak: asked %zu B, held %zu B, the heap's own %zu B (%.1f%%)\n", peak_requested, peak_used,
               peak_used - peak_requested, 100.0 * (double)(peak_used - peak_requested) / (double)peak_requested);
        printf("worst wholeness with over a quarter free: largest %zu of %zu B free (%.0f%%)\n", worst_largest, worst_free,
               worst_free ? 100.0 * (double)worst_largest / (double)worst_free : 0.0);
        free(arena);
        free(live);
        free(asked);
    }
    free(ops);
    return 0;
}
