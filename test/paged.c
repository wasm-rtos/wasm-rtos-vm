/* Paged memory: a memory that cannot grow in one block goes on paged, and every access still works. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static size_t g_block_limit = (size_t)-1;
/* How many more allocations succeed, like a heap that is running out; -1: no end. */
static long g_allocations_left = -1;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/* The heap with no block larger than g_block_limit, like a fragmented one. */
static void* limited(void* ptr, size_t size)
{
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }
    if (size > g_block_limit || g_allocations_left == 0)
        return NULL;
    if (g_allocations_left > 0)
        --g_allocations_left;
    return realloc(ptr, size);
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
    WmRuntime* rt;
    uint32_t flat_sum, paged_sum;
    if (f)
        fclose(f);
    wm_set_allocator(limited);
    rt = wm_runtime_new(16384, 64);
    check(size && !wm_module_load(&module, bytes, size), "load the module");
    check(module && !wm_instantiate(&inst, module, rt, NULL, NULL), "instantiate with one page in one block");
    if (!inst)
        return 1;
    check(wm_memory_data(wm_instance_memory(inst), NULL) != NULL, "the memory starts in one block");
    check(wm_memory_resident(wm_instance_memory(inst)) == 65536, "all of a one-block memory is in RAM");
    call1(rt, inst, "fill", 1, 0, 0);
    flat_sum = call1(rt, inst, "sum", 1, 0, 0);

    /* No block larger than 100 KiB from now on: growing to 4 pages must go paged. */
    g_block_limit = 100u * 1024u;
    check(call1(rt, inst, "grow", 3, 0, 0) == 1, "memory.grow to 4 pages succeeds");
    check(wm_memory_data(wm_instance_memory(inst), NULL) == NULL, "the memory is paged now");
    check(wm_memory_resident(wm_instance_memory(inst)) == 4 * 65536, "the pages it grew by are in RAM at once");
    check(call1(rt, inst, "sum", 1, 0, 0) == flat_sum, "what was written in one block reads back paged");
    call1(rt, inst, "fill", 4, 0, 0);
    paged_sum = call1(rt, inst, "sum", 4, 0, 0);
    {
        /* The same sums, in a memory that never was paged. */
        WmInstance* other = NULL;
        g_block_limit = (size_t)-1;
        if (!wm_instantiate(&other, module, rt, NULL, NULL))
        {
            call1(rt, other, "grow", 3, 0, 0);
            call1(rt, other, "fill", 4, 0, 0);
            check(call1(rt, other, "sum", 4, 0, 0) == paged_sum, "paged memory holds what one block holds");
            wm_instance_free(other);
        }
        g_block_limit = 100u * 1024u;
    }
    /* Across frames: 20 bytes from 8190 (frames 1 and 2) to 4090 (frames 0 and 1). */
    {
        uint32_t before[5], after[5];
        int same = 1;
        for (uint32_t i = 0; i < 5; ++i)
            before[i] = call1(rt, inst, "peek", 8190 + 4 * i, 0, 0);
        call1(rt, inst, "copy", 4090, 8190, 20);
        for (uint32_t i = 0; i < 5; ++i)
        {
            after[i] = call1(rt, inst, "peek", 4090 + 4 * i, 0, 0);
            same &= after[i] == before[i];
        }
        check(same, "memory.copy across frames");
        /* Overlapping, the destination after the source. */
        for (uint32_t i = 0; i < 5; ++i)
            before[i] = call1(rt, inst, "peek", 4090 + 4 * i, 0, 0);
        call1(rt, inst, "copy", 4094, 4090, 20);
        same = 1;
        for (uint32_t i = 0; i < 5; ++i)
            same &= call1(rt, inst, "peek", 4094 + 4 * i, 0, 0) == before[i];
        check(same, "overlapping memory.copy across frames");
    }
    check(call1(rt, inst, "peek", 4 * 65536 - 4, 0, 0) == 0, "an untouched page reads as zeros");
    check(call1(rt, inst, "peek", 4 * 65536 - 3, 0, 0) == 0xDEADBEEFu, "an access past the end traps");

    /* The host reaches the memory in place where it is in one piece, and copies where it is not. */
    {
        WmMemory* memory = wm_instance_memory(inst);
        uint8_t* p = NULL;
        uint8_t bytes[8];
        uint32_t value = 0;
        check(!wm_memory_pointer(memory, 65536 + 8, wm_frame_size() / 2u, &p) && p != NULL,
              "a range inside one frame has a pointer");
        check(!wm_memory_pointer(memory, 65536 + wm_frame_size() - 6u, 20, &p) && p == NULL, "a range across frames has none");
        check(!wm_memory_pointer(memory, 100, 3000, &p) && p != NULL,
              "a range in the frames of the former block has a pointer");
        check(wm_memory_pointer(memory, 4 * 65536 - 4, 8, &p) != NULL && p == NULL, "a range past the end is an error");
        check(!wm_memory_pointer(memory, 4 * 65536, 0, &p), "an empty range at the end is not");
        /* Writing through the pointer and reading back by address agree. */
        if (!wm_memory_pointer(memory, 3 * 65536 + 8, 4, &p) && p)
        {
            memcpy(p, "\x78\x56\x34\x12", 4);
            check(call1(rt, inst, "peek", 3 * 65536 + 8, 0, 0) == 0x12345678u, "the program sees what the host wrote in place");
        }
        value = 0xCAFEF00Du;
        check(!wm_memory_write(memory, 2 * 65536 + 4094, &value, 4) && !wm_memory_read(memory, 2 * 65536 + 4094, bytes, 4) &&
                  memcmp(bytes, &value, 4) == 0 && call1(rt, inst, "peek", 2 * 65536 + 4094, 0, 0) == value,
              "a write across frames reads back, by the host and by the program");
    }

    /* A heap that runs out during memory.grow: it says no, and the memory stays as it was. */
    {
        WmMemory* memory = wm_instance_memory(inst);
        uint32_t resident = wm_memory_resident(memory);
        uint32_t sum = call1(rt, inst, "sum", 4, 0, 0);
        /* About half the frames of a page: some are made before the heap runs out. */
        g_allocations_left = (long)(65536u / wm_frame_size() + 1u) / 2;
        check(call1(rt, inst, "grow", 1, 0, 0) == 0xFFFFFFFFu, "memory.grow fails when the heap runs out of frames");
        g_allocations_left = -1;
        check(wm_memory_resident(memory) == resident && call1(rt, inst, "sum", 4, 0, 0) == sum,
              "the memory keeps its size and contents");
        check(call1(rt, inst, "grow", 1, 0, 0) == 4 && wm_memory_resident(memory) == resident + 65536,
              "it grows once there is room again");
    }

    wm_instance_free(inst);
    wm_module_free(module);
    wm_runtime_free(rt);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
