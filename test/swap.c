/*
 * Swapping: with a pager every memory is paged, frames come into RAM when
 * touched, and the pager takes them out into its storage and brings them
 * back. Here RAM holds 3 frames, so nearly every access swaps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

#define BUDGET 3u

static int g_failures;
/* Frames promised to memories, and how many the pager will promise. */
static int32_t g_reserved, g_reserve_limit = 1 << 30;
static uint8_t* g_slots[4096];
static unsigned long g_taken, g_back, g_written;
static uint32_t g_hand;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static int reserve(void* context, int32_t frames)
{
    (void)context;
    if (frames > 0 && g_reserved + frames > g_reserve_limit)
        return 0;
    g_reserved += frames;
    return 1;
}

/* CLOCK over every frame of every memory with the pager. */
static int take_one(void)
{
    uint32_t total = 0;
    for (WmMemory* m = wm_memory_next(NULL); m; m = wm_memory_next(m))
        total += wm_memory_frames(m);
    for (uint32_t step = 0; total && step < 3u * total; ++step)
    {
        uint32_t at = g_hand++ % total;
        for (WmMemory* m = wm_memory_next(NULL); m; m = wm_memory_next(m))
        {
            if (at < wm_memory_frames(m))
            {
                if (wm_memory_frame_look(m, at) == 0 && !wm_memory_frame_take(m, at))
                    return (int)++g_taken;
                break;
            }
            at -= wm_memory_frames(m);
        }
    }
    return 0;
}

static int room(void* context, int again)
{
    (void)context;
    if (again)
        return take_one();
    while (wm_pager_resident() >= BUDGET)
        if (!take_one())
            return 1;
    return 1;
}

static WmResult store(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t i = 0;
    (void)context;
    ++g_written;
    if (*slot != 0xFFFFFFFFu)
    {
        memcpy(g_slots[*slot], frame, 4096);
        return NULL;
    }
    while (i < 4096u && g_slots[i])
        ++i;
    if (i == 4096u || !(g_slots[i] = (uint8_t*)malloc(4096)))
        return "exhaustion: storage full";
    memcpy(g_slots[i], frame, 4096);
    *slot = i;
    return NULL;
}

static WmResult load(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], 4096);
    ++g_back;
    return NULL;
}

static void drop(void* context, uint32_t slot)
{
    (void)context;
    free(g_slots[slot]);
    g_slots[slot] = NULL;
}

static const WmPager g_pager = {NULL, reserve, room, store, load, drop};

static uint32_t slots_used(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4096u; ++i)
        n += g_slots[i] != NULL;
    return n;
}

static uint32_t call1(WmRuntime* rt, WmInstance* inst, const char* name, uint32_t a, uint32_t b, uint32_t c)
{
    WmExtern f;
    uint32_t args[3] = {a, b, c};
    uint32_t result = 0xDEADBEEFu;
    WmResult r;
    if (wm_instance_export(inst, name, (uint32_t)strlen(name), &f))
        return 0xDEADBEEFu;
    if ((r = wm_call(rt, f.as.func, args, &result)))
        printf("  %s: %s\n", name, r);
    return result;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/paged.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmInstance* other = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmMemory* memory;
    uint32_t sum, value = 0;
    uint8_t* p = NULL;
    if (f)
        fclose(f);

    wm_set_pager(&g_pager);
    check(size && !wm_module_load(&module, bytes, size), "load the module");
    check(module && !wm_instantiate(&inst, module, rt, NULL, NULL), "instantiate");
    if (!inst)
        return 1;
    memory = wm_instance_memory(inst);
    check(wm_memory_data(memory, NULL) == NULL && g_reserved == 16, "the memory is paged, its 16 frames reserved");
    check(wm_memory_resident(memory) == 0, "no frame is in RAM before it is touched");

    /* 4 pages, 64 frames, through 3 frames of RAM. */
    check(call1(rt, inst, "grow", 3, 0, 0) == 1 && g_reserved == 64, "memory.grow reserves the new frames");
    check(wm_memory_resident(memory) == 0, "growing makes no frame");
    call1(rt, inst, "fill", 4, 0, 0);
    check(wm_pager_resident() <= BUDGET && g_taken > 0, "filling the memory keeps RAM to the budget");
    sum = call1(rt, inst, "sum", 4, 0, 0);
    check(sum == call1(rt, inst, "sum", 4, 0, 0) && g_back > 0, "frames come back with what was written");
    check(wm_memory_stored(memory) > 0 && wm_memory_stored(memory) + wm_memory_resident(memory) <= 4 * 65536,
          "the memory is part in RAM, part stored");
    check(call1(rt, inst, "peek", 4 * 65536 - 4, 0, 0) == 0, "an untouched page reads as zeros");
    {
        /* Reading only: frames leave RAM clean, so nothing is written but the few dirty at the start. */
        unsigned long written = g_written, taken = g_taken;
        call1(rt, inst, "sum", 4, 0, 0);
        check(g_taken - taken > 30 && g_written - written <= BUDGET, "frames that were only read leave without a write");
    }

    /* A second memory shares the same RAM. */
    check(!wm_instantiate(&other, module, rt, NULL, NULL), "instantiate a second memory");
    call1(rt, other, "fill", 1, 0, 0);
    check(call1(rt, inst, "sum", 4, 0, 0) == sum, "the first memory's contents survive the second one's use");
    check(wm_pager_resident() <= BUDGET, "both memories together keep to the budget");

    /* The host reads and writes swapped frames, and copies run across them. */
    value = 0x5EEDF00Du;
    check(!wm_memory_write(memory, 3 * 65536 + 4094, &value, 4) && call1(rt, inst, "peek", 3 * 65536 + 4094, 0, 0) == value,
          "a host write across swapped frames reads back");
    call1(rt, inst, "copy", 65536, 2 * 65536, 3 * 4096);
    check(call1(rt, inst, "peek", 65536, 0, 0) == call1(rt, inst, "peek", 2 * 65536, 0, 0),
          "memory.copy between swapped frames");

    /* A pinned frame stays in RAM: a host may keep a pointer into it. */
    check(!wm_memory_pin(memory, 100, 8, 1) && !wm_memory_pointer(memory, 100, 8, &p) && p, "pin a range");
    memcpy(p, "PINNED!!", 8);
    call1(rt, inst, "sum", 4, 0, 0);
    check(memcmp(p, "PINNED!!", 8) == 0 && wm_memory_frame_look(memory, 0) < 0, "a pinned frame is never taken");
    check(!wm_memory_pin(memory, 100, 8, -1) && wm_memory_frame_look(memory, 0) >= 0, "unpinned, it may go again");

    /* The pager may say no: memory.grow fails and nothing changes. */
    g_reserve_limit = g_reserved;
    check(call1(rt, inst, "grow", 1, 0, 0) == 0xFFFFFFFFu, "memory.grow fails when the pager reserves no more");
    g_reserve_limit = 1 << 30;
    check(call1(rt, inst, "sum", 4, 0, 0) != 0xDEADBEEFu, "the memory still works");

    /* Freeing a memory lets its RAM, slots and reservation go. */
    wm_instance_free(other);
    wm_instance_free(inst);
    check(g_reserved == 0 && wm_pager_resident() == 0 && slots_used() == 0 && !wm_memory_next(NULL),
          "freed memories leave no frames, slots or reservations");
    printf("  %lu frames taken out, %lu written, %lu brought back\n", g_taken, g_written, g_back);
    wm_module_free(module);
    wm_runtime_free(rt);
    wm_set_pager(NULL);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
