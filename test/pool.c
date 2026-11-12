/*
 * The frame pool (decision 6): frames of memories with a pager come from a
 * region the host gives, and a memory in one block is its frames in a row
 * there. Frames of three memories cut the pool into pieces and one of them
 * still goes into one block, the frames moved and every byte where it was;
 * pinned frames and the memory of a call in progress stay; a program goes
 * into one block between two slices and ends as it would have; a block
 * grows in place, or goes to storage and grows paged; freed, the pool is
 * whole again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

#define POOL 24u

static int g_failures;
static uint8_t* g_slots[1024];
static uint32_t g_next_slot;
static int g_allow_blocks;
static WmMemory* g_other;
static WmMemory* g_program_memory;
static WmResult g_between_result;
static int g_pointer_kept;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static int reserve(void* context, int32_t frames) { (void)context, (void)frames; return 1; }
static int room(void* context, int again) { (void)context; return !again; }
static void drop(void* context, uint32_t slot) { (void)context, (void)slot; }

static WmResult write_frame(void* context, uint32_t* slot, const uint8_t* frame)
{
    (void)context;
    if (*slot == 0xFFFFFFFFu)
        *slot = g_next_slot++;
    if (!g_slots[*slot])
        g_slots[*slot] = (uint8_t*)malloc(wm_frame_size());
    memcpy(g_slots[*slot], frame, wm_frame_size());
    return NULL;
}

static WmResult read_frame(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], wm_frame_size());
    return NULL;
}

static int block(void* context, uint32_t frames) { (void)context; return g_allow_blocks && wm_pager_resident() + frames <= POOL; }

static const WmPager g_pager = {NULL, reserve, room, write_frame, read_frame, drop, NULL, NULL, NULL, block};

/* A memory of so many frames, its byte pages the size of a frame. */
static WmMemory* frames_memory(uint32_t frames)
{
    return wm_memory_new_paged(frames * wm_frame_size(), 0xFFFFFFFFu, 0);
}

/* Frame i of memory m filled with its mark: tag, then i. */
static void mark(WmMemory* m, uint32_t i, uint8_t tag)
{
    uint8_t bytes[2] = {tag, (uint8_t)i};
    wm_memory_write(m, i * wm_frame_size(), bytes, 2);
    wm_memory_write(m, i * wm_frame_size() + wm_frame_size() - 2u, bytes, 2);
}

static int marked(WmMemory* m, uint32_t frames, uint8_t tag)
{
    for (uint32_t i = 0; i < frames; ++i)
    {
        uint8_t a[2], b[2];
        wm_memory_read(m, i * wm_frame_size(), a, 2);
        wm_memory_read(m, i * wm_frame_size() + wm_frame_size() - 2u, b, 2);
        if (a[0] != tag || a[1] != (uint8_t)i || b[0] != tag || b[1] != (uint8_t)i)
            return 0;
    }
    return 1;
}

/* From the program: a pointer into its memory, another memory into one block, the pointer again. */
static WmResult between(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    uint8_t* before = NULL;
    uint8_t* after = NULL;
    (void)rt, (void)user, (void)slots;
    wm_memory_pointer(memory, 0, 4, &before);
    g_between_result = wm_memory_block_in(g_other);
    wm_memory_pointer(memory, 0, 4, &after);
    g_pointer_kept = before && before == after;
    return NULL;
}

static WmFunc* g_between;

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* n, uint32_t nl, uint8_t k, WmExtern* out)
{
    (void)c, (void)mo, (void)ml, (void)n, (void)nl;
    out->kind = k;
    out->as.func = g_between;
    return NULL;
}

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    FILE* f;
    uint32_t size, result = 0, whole = 0, slices = 0;
    uint8_t* pool;
    WmMemory *a, *b, *c;
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmRuntime* rt;
    WmResult r;
    if (wm_frame_size() != 4096u)
    {
        printf("SKIP the test is laid out for frames of 4 KiB\n");
        return 0;
    }
    pool = (uint8_t*)malloc((size_t)POOL * wm_frame_size());
    wm_set_pager(&g_pager);
    wm_set_frame_pool(pool, POOL);

    /* 1. Three memories, their frames in turn: no run of six frames of one of them. */
    a = frames_memory(6);
    b = frames_memory(6);
    c = frames_memory(6);
    for (uint32_t i = 0; i < 6; ++i)
    {
        mark(a, i, 'A');
        mark(b, i, 'B');
        mark(c, i, 'C');
    }
    check(!wm_memory_data(a, NULL) && wm_pager_resident() == 18, "three paged memories, their frames in turn in the pool");
    g_allow_blocks = 1;
    check(wm_memory_block_in(a) == NULL && wm_memory_data(a, NULL) && wm_memory_block_frames(a) == 6,
          "the first goes into one block all the same: frames moved");
    check(marked(a, 6, 'A') && marked(b, 6, 'B') && marked(c, 6, 'C'), "every frame of the three holds what it held");
    check(wm_pager_resident() == 18, "and RAM holds as many frames as before");

    /* 2. Pinned frames in the way of every window: later. */
    for (uint32_t i = 0; i < 6; ++i)
        wm_memory_pin(c, i * wm_frame_size(), 1, 1);
    check(wm_memory_block_in(b) == wm_page_wait && !wm_memory_data(b, NULL) && marked(b, 6, 'B'),
          "with C pinned all over the pool, B stays paged for now");
    for (uint32_t i = 0; i < 6; ++i)
        wm_memory_pin(c, i * wm_frame_size(), 1, -1);
    check(wm_memory_block_in(b) == NULL && wm_memory_data(b, NULL) && marked(b, 6, 'B') && marked(c, 6, 'C'),
          "unpinned, B goes into one block");
    wm_memory_free(a);
    wm_memory_free(b);
    wm_memory_free(c);
    check(wm_pager_resident() == 0, "freed: no frame in RAM");

    /* 3. The memory of a call in progress does not move under the host's pointer. */
    f = fopen(argc > 1 ? argv[1] : "test/pool.wasm", "rb");
    size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    if (f)
        fclose(f);
    g_between = wm_host_function_new(NULL, 0, NULL, 0, between, NULL);
    rt = wm_runtime_new(16u * 1024u, 64u);
    g_allow_blocks = 0;
    check(size && !wm_module_load(&module, bytes, size) && !wm_instantiate(&inst, module, rt, resolve, NULL),
          "instantiate the program, its memory paged");
    g_program_memory = wm_instance_memory(inst);
    g_other = frames_memory(8);
    /* The program's 16 frames and the other's 8 in turn fill the pool: no window is free of the program's frames. */
    for (uint32_t i = 0; i < 16; ++i)
    {
        uint32_t word = i;
        wm_memory_write(g_program_memory, i * 4096u, &word, 4);
        if (i < 8)
            mark(g_other, i, 'O');
    }
    g_allow_blocks = 1;
    check(!wm_call(rt, export_func(inst, "call_host"), NULL, &result) && result == 0,
          "the program calls the host, which asks for the other memory in one block");
    check(g_between_result == wm_page_wait && !wm_memory_data(g_other, NULL),
          "every window holds the program's frames, which may not move during its call: later");
    check(g_pointer_kept, "the host's pointer into the program's memory still points at it");
    check(wm_memory_block_in(g_other) == NULL && wm_memory_data(g_other, NULL) && marked(g_other, 8, 'O'),
          "between calls the program's frames may move: the other memory goes into one block");
    {
        int same = 1;
        for (uint32_t i = 0; i < 16; ++i)
        {
            uint32_t word = 0;
            wm_memory_read(g_program_memory, i * 4096u, &word, 4);
            same &= word == i;
        }
        check(same, "and every frame of the program holds what it held");
    }

    /* 4. Between two slices the program's memory goes into one block and the sum ends as it would have. */
    wm_memory_free(g_other);
    g_other = NULL;
    wm_call(rt, export_func(inst, "fill"), NULL, NULL);
    wm_call(rt, export_func(inst, "sum"), NULL, &whole);
    wm_set_fuel(rt, 40);
    r = wm_call(rt, export_func(inst, "sum"), NULL, &result);
    check(r == wm_fuel_exhausted && !wm_memory_data(g_program_memory, NULL), "the sum stops by fuel, the memory paged");
    check(wm_memory_block_in(g_program_memory) == NULL && wm_memory_data(g_program_memory, NULL),
          "between two slices it goes into one block");
    while (r == wm_fuel_exhausted)
    {
        ++slices;
        wm_set_fuel(rt, 40);
        r = wm_resume(rt, &result);
    }
    check(r == NULL && result == whole && slices > 0, "and the sum ends as it would have");
    wm_disable_fuel(rt);
    wm_instance_free(inst);
    wm_module_free(module);

    /* 5. A block of the pool grows in place, or goes out and grows paged; freed, the pool is whole. */
    a = frames_memory(4);
    mark(a, 0, 'G');
    check(wm_memory_data(a, NULL) && wm_memory_grow(a, 2u * wm_frame_size()) == 4u * wm_frame_size() &&
              wm_memory_block_frames(a) == 6 && marked(a, 1, 'G'),
          "a block grows in place into the free frames after it");
    b = frames_memory(4);
    check(wm_memory_data(b, NULL) != NULL, "a second block right after it");
    check(wm_memory_grow(a, 2u * wm_frame_size()) == 6u * wm_frame_size() && !wm_memory_data(a, NULL) &&
              marked(a, 1, 'G'),
          "with no free frame after it, the first goes to storage and grows paged");
    wm_memory_free(a);
    wm_memory_free(b);
    a = frames_memory(POOL);
    check(wm_pager_resident() == POOL && wm_memory_block_frames(a) == POOL, "freed, the whole pool is one block again");
    wm_memory_free(a);

    wm_runtime_free(rt);
    wm_host_function_free(g_between);
    for (uint32_t i = 0; i < g_next_slot; ++i)
        free(g_slots[i]);
    free(pool);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
