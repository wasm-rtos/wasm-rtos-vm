/*
 * A memory in one block with a pager: reached as fast as one without, its
 * frames all counted in RAM and reserved in storage, until it leaves RAM
 * whole (wm_memory_block_out) and goes on paged. Here a program is stopped
 * by fuel, its block goes out between two slices, and it ends as it would
 * have; frames still all zero are not written; a pinned range or a failed
 * write keeps the block; memory.grow stays in one block while the pager
 * lets it, and goes on paged when it does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static int32_t g_reserved;
static uint8_t* g_slots[1024];
static uint32_t g_next_slot;
static unsigned long g_written, g_read;
/* Frames the pager lets stay in RAM in one block; a write it fails, for the test. */
static uint32_t g_block_budget = 1000;
static int g_fail_writes;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static int reserve(void* context, int32_t frames)
{
    (void)context;
    g_reserved += frames;
    return 1;
}

static int room(void* context, int again)
{
    (void)context, (void)again;
    return !again;
}

static WmResult write_frame(void* context, uint32_t* slot, const uint8_t* frame)
{
    (void)context;
    if (g_fail_writes)
        return "exhaustion: the test's store says no";
    if (*slot == 0xFFFFFFFFu)
        *slot = g_next_slot++;
    if (!g_slots[*slot])
        g_slots[*slot] = (uint8_t*)malloc(wm_frame_size());
    memcpy(g_slots[*slot], frame, wm_frame_size());
    ++g_written;
    return NULL;
}

static WmResult read_frame(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], wm_frame_size());
    ++g_read;
    return NULL;
}

static void drop(void* context, uint32_t slot) { (void)context, (void)slot; }

static int block(void* context, uint32_t frames)
{
    (void)context;
    return wm_pager_resident() + frames <= g_block_budget;
}

static const WmPager g_pager = {NULL, reserve, room, write_frame, read_frame, drop, NULL, NULL, NULL, block};

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/paged.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    uint32_t frames_per_page = 65536u / wm_frame_size();
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmMemory* memory;
    uint32_t pages = 1, whole = 0, result = 0, slices = 0, value = 0;
    unsigned long written;
    WmResult r;
    if (f)
        fclose(f);
    if (!wm_frame_size())
    {
        printf("SKIP built without paging\n");
        return 0;
    }
    wm_set_pager(&g_pager);

    check(size && !wm_module_load(&module, bytes, size) && !wm_instantiate(&inst, module, rt, NULL, NULL), "instantiate");
    if (!inst)
        return 1;
    memory = wm_instance_memory(inst);
    check(wm_memory_data(memory, NULL) != NULL && wm_memory_block_frames(memory) == frames_per_page,
          "with room in the budget, the memory is one block");
    check(wm_pager_resident() == frames_per_page && g_reserved == (int32_t)frames_per_page,
          "its frames count in RAM and are reserved in storage");

    /* The whole sum, then the same sum stopped by fuel with the block sent out between two slices. */
    wm_call(rt, export_func(inst, "fill"), &pages, &result);
    check(!wm_call(rt, export_func(inst, "sum"), &pages, &whole), "fill and sum it whole");
    wm_set_fuel(rt, 30);
    r = wm_call(rt, export_func(inst, "sum"), &pages, &result);
    check(r == wm_fuel_exhausted, "the same sum stops by fuel");
    written = g_written;
    {
        /* The frames fill wrote: those, and only those, need a copy. */
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < frames_per_page; ++i)
        {
            uint8_t frame[65536];
            wm_memory_read(memory, i * wm_frame_size(), frame, wm_frame_size());
            for (uint32_t k = 0; k < wm_frame_size(); ++k)
                if (frame[k])
                {
                    ++nonzero;
                    break;
                }
        }
        check(wm_memory_block_out(memory) == NULL && wm_memory_data(memory, NULL) == NULL &&
                  wm_memory_block_frames(memory) == 0,
              "between two slices the block goes out: the memory is paged");
        check(wm_pager_resident() == 0, "no frame of it is in RAM");
        check(g_written - written == nonzero && nonzero > 0, "the frames fill wrote were stored, the zero ones not");
    }
    while (r == wm_fuel_exhausted)
    {
        ++slices;
        wm_set_fuel(rt, 30);
        r = wm_resume(rt, &result);
    }
    check(r == NULL && result == whole && slices > 1, "the sum goes on paged and ends as it would have");
    wm_disable_fuel(rt);
    wm_instance_free(inst);
    wm_module_free(module);
    check(wm_pager_resident() == 0 && g_reserved == 0, "freed: nothing in RAM, nothing reserved");

    /* Frames never written need no copy. */
    memory = wm_memory_new(2, 4);
    check(wm_memory_block_frames(memory) == 2u * frames_per_page, "a new memory of 2 pages is one block");
    value = 0x5A5A5A5Au;
    wm_memory_write(memory, 65536u + 12u, &value, 4);
    written = g_written;
    check(wm_memory_block_out(memory) == NULL && g_written - written == 1,
          "only the one frame written goes to storage");
    value = 0;
    wm_memory_read(memory, 65536u + 12u, &value, 4);
    check(value == 0x5A5A5A5Au, "it reads back after the block is gone");
    wm_memory_read(memory, 100u, &value, 4);
    check(value == 0, "an untouched frame reads as zero");
    wm_memory_free(memory);

    /* A pinned range and a failed write keep the block. */
    memory = wm_memory_new(1, 4);
    value = 7;
    wm_memory_write(memory, 0, &value, 4);
    check(wm_memory_pin(memory, 0, 16, 1) == NULL && wm_memory_block_out(memory) != NULL && wm_memory_data(memory, NULL),
          "a pinned range keeps the block in RAM");
    wm_memory_pin(memory, 0, 16, -1);
    g_fail_writes = 1;
    check(wm_memory_block_out(memory) != NULL && wm_memory_data(memory, NULL) && wm_memory_block_frames(memory),
          "a write that fails keeps the block");
    g_fail_writes = 0;
    value = 0;
    wm_memory_read(memory, 0, &value, 4);
    check(value == 7, "and what it holds");

    /* memory.grow: in one block while the pager lets it, then paged. */
    check(wm_memory_grow(memory, 1) == 1 && wm_memory_block_frames(memory) == 2u * frames_per_page &&
              wm_pager_resident() == 2u * frames_per_page,
          "it grows in one block while the budget has room");
    g_block_budget = wm_pager_resident();
    check(wm_memory_grow(memory, 1) == 2 && wm_memory_data(memory, NULL) == NULL && wm_memory_block_frames(memory) == 0,
          "with no more room in one block it grows paged, the block in storage");
    value = 0;
    wm_memory_read(memory, 0, &value, 4);
    check(value == 7, "what it held reads back");
    wm_memory_read(memory, 2u * 65536u + 40u, &value, 4);
    check(value == 0, "the new page reads as zero");
    wm_memory_free(memory);
    check(wm_pager_resident() == 0 && g_reserved == 0, "freed: nothing in RAM, nothing reserved");

    /* Without room in the budget a memory is paged from the start. */
    g_block_budget = 0;
    memory = wm_memory_new(1, 1);
    check(wm_memory_data(memory, NULL) == NULL && wm_memory_block_frames(memory) == 0,
          "without room in the budget a memory is paged at once");
    wm_memory_free(memory);

    wm_runtime_free(rt);
    for (uint32_t i = 0; i < g_next_slot; ++i)
        free(g_slots[i]);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
