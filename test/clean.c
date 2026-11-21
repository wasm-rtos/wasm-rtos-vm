/*
 * Frames written ahead of need (wm_memory_clean): a memory in one block
 * copied frame by frame between runs leaves RAM with no write while
 * nothing changed it; any run of its code, its code called from another
 * instance, a host write or a run resumed after a stop makes the copies
 * stale, and they are written again. A paged memory's dirty frames come
 * out clean and leave RAM with no write. Every memory reads back as it
 * was.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint8_t* g_slots[4096];
static uint32_t g_next_slot;
static unsigned long g_written;
static uint32_t g_ram = 1000;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static int reserve(void* context, int32_t frames)
{
    (void)context, (void)frames;
    return 1;
}

static WmMemory* g_paged;

/* Keep RAM to g_ram frames: take out the first frame of the paged memory that may go. */
static int room(void* context, int again)
{
    (void)context;
    if (again)
        return 0;
    for (uint32_t i = 0; g_paged && wm_pager_resident() >= g_ram && i < wm_memory_frames(g_paged); ++i)
        if (wm_memory_frame_look(g_paged, i) >= 0)
            wm_memory_frame_take(g_paged, i);
    return 1;
}

static WmResult write_frame(void* context, uint32_t* slot, const uint8_t* frame)
{
    (void)context;
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
    return NULL;
}

static void drop(void* context, uint32_t slot) { (void)context, (void)slot; }

static int g_blocks = 1;
static int block(void* context, uint32_t frames)
{
    (void)context, (void)frames;
    return g_blocks;
}

static const WmPager g_pager = {NULL, reserve, room, write_frame, read_frame, drop, NULL, NULL, NULL, block};

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

static WmInstance* g_paged_inst;
static WmResult resolve(void* user, const char* module, uint32_t module_len, const char* name, uint32_t name_len,
                        uint8_t kind, WmExtern* out)
{
    (void)user, (void)module, (void)module_len, (void)kind;
    return wm_instance_export(g_paged_inst, name, name_len, out);
}

/* Write frames ahead until every one has its copy: how many were written. */
static unsigned long clean_all(WmMemory* memory)
{
    unsigned long before = g_written;
    uint32_t written = 1;
    while (written && !wm_memory_clean(memory, &written))
    {
    }
    return g_written - before;
}

/* The memory's bytes, through the memory functions. */
static uint8_t* contents(WmMemory* memory, uint32_t bytes)
{
    uint8_t* out = (uint8_t*)malloc(bytes);
    wm_memory_read(memory, 0, out, bytes);
    return out;
}

static uint8_t* load_file(const char* path, uint32_t* size)
{
    static uint8_t bytes[2][4096];
    static int n;
    FILE* f = fopen(path, "rb");
    uint8_t* b = bytes[n++ & 1];
    *size = f ? (uint32_t)fread(b, 1, 4096, f) : 0;
    if (f)
        fclose(f);
    return b;
}

/* A fresh instance of the paged module, its memory in one block, filled: its memory. */
static WmMemory* fresh(WmModule* module, WmRuntime* rt, WmInstance** inst)
{
    uint32_t pages = 2, grown, result;
    if (*inst)
        wm_instance_free(*inst);
    *inst = NULL;
    if (wm_instantiate(inst, module, rt, NULL, NULL))
        return NULL;
    wm_call(rt, export_func(*inst, "grow"), &(uint32_t){1}, &grown);
    wm_call(rt, export_func(*inst, "fill"), &pages, &result);
    return wm_instance_memory(*inst);
}

int main(void)
{
    uint32_t size = 0, other_size = 0;
    const uint8_t* bytes = load_file("test/paged.wasm", &size);
    const uint8_t* other_bytes = load_file("test/clean.wasm", &other_size);
    uint32_t bytes_in = 2u * 65536u, nonzero = 0;
    WmModule *module = NULL, *other_module = NULL;
    WmInstance *inst = NULL, *other = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmMemory* memory;
    uint8_t *before, *after;
    unsigned long written;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!wm_frame_size())
    {
        printf("SKIP built without paging\n");
        return 0;
    }
    wm_set_pager(&g_pager);
    check(size && !wm_module_load(&module, bytes, size), "load the module");
    memory = fresh(module, rt, &inst);
    check(memory && wm_memory_block_frames(memory) == bytes_in / wm_frame_size(), "its memory of two pages is one block");
    if (!memory)
        return 1;
    before = contents(memory, bytes_in);
    for (uint32_t i = 0; i < bytes_in / wm_frame_size(); ++i)
        for (uint32_t k = 0; k < wm_frame_size(); ++k)
            if (before[i * wm_frame_size() + k])
            {
                ++nonzero;
                break;
            }

    /* 1. Copied ahead, unchanged: it leaves RAM with no write. */
    check(clean_all(memory) == nonzero, "frames are written ahead one at a time, the zero ones not");
    check(clean_all(memory) == 0, "and once each: then nothing is left to write");
    written = g_written;
    check(!wm_memory_block_out(memory) && wm_memory_block_frames(memory) == 0 && g_written == written,
          "the block leaves RAM with no write");
    after = contents(memory, bytes_in);
    check(!memcmp(before, after, bytes_in), "and reads back as it was");
    free(after);

    /* 2. Half copied: only the rest is written when it leaves. */
    memory = fresh(module, rt, &inst);
    {
        uint32_t done = 0, w = 1;
        for (uint32_t i = 0; i < bytes_in / wm_frame_size() / 2u && w; ++i)
            if (!wm_memory_clean(memory, &w) && w)
                ++done;
        written = g_written;
        wm_memory_block_out(memory);
        check(g_written - written < nonzero && g_written - written > 0, "half copied ahead: the block writes only the rest");
        after = contents(memory, bytes_in);
        check(!memcmp(before, after, bytes_in), "and reads back as it was");
        free(after);
    }

    /* 3. What makes the copies stale: each time all are written again. */
    {
        static const char* what[] = {"a run of its code", "a host write", "its code called from another instance",
                                     "growing it", "a host pointer into it"};
        check(other_size && !wm_module_load(&other_module, other_bytes, other_size), "load a module that calls into it");
        for (int k = 0; k < 5; ++k)
        {
            char line[160];
            uint32_t value = 0x5EEDF00Du, grown;
            uint8_t* p;
            memory = fresh(module, rt, &inst);
            clean_all(memory);
            if (k == 0)
                wm_call(rt, export_func(inst, "add"), &(uint32_t){2}, NULL);
            else if (k == 1)
                wm_memory_write(memory, 4096 + 16, &value, 4);
            else if (k == 2)
            {
                g_paged_inst = inst;
                if (other)
                    wm_instance_free(other);
                other = NULL;
                if (!wm_instantiate(&other, other_module, rt, resolve, NULL))
                    wm_call(rt, export_func(other, "run"), &(uint32_t){2}, NULL);
            }
            else if (k == 3)
                wm_call(rt, export_func(inst, "grow"), &(uint32_t){1}, &grown);
            else
            {
                wm_memory_pointer(memory, 4096 + 32, 4, &p);
                if (p)
                    memcpy(p, &value, 4);
            }
            free(before);
            {
                uint32_t now;
                wm_memory_data(memory, &now);
                bytes_in = now;
            }
            before = contents(memory, bytes_in);
            written = g_written;
            wm_memory_block_out(memory);
            after = contents(memory, bytes_in);
            snprintf(line, sizeof(line), "after %s, the block is written again and reads back as changed", what[k]);
            check(g_written > written && !memcmp(before, after, bytes_in), line);
            free(after);
            bytes_in = 2u * 65536u;
        }
    }

    /* 4. A run stopped by fuel and resumed after its memory was copied: the copies do not hold. */
    {
        uint32_t pages = 2;
        WmResult r;
        memory = fresh(module, rt, &inst);
        free(before);
        before = contents(memory, bytes_in);
        wm_set_fuel(rt, 20);
        r = wm_call(rt, export_func(inst, "add"), &pages, NULL);
        wm_disable_fuel(rt);
        clean_all(memory);
        while (r == wm_fuel_exhausted)
            r = wm_resume(rt, NULL);
        for (uint32_t at = 0; at < bytes_in; at += 4096)
        {
            uint32_t word;
            memcpy(&word, before + at, 4);
            ++word;
            memcpy(before + at, &word, 4);
        }
        written = g_written;
        wm_memory_block_out(memory);
        after = contents(memory, bytes_in);
        check(r == NULL && g_written > written && !memcmp(before, after, bytes_in),
              "a run resumed after the copies were made changes the memory, and it is written again");
        free(after);
    }

    /* 5. A paged memory: dirty frames come out clean and leave RAM with no write. */
    {
        unsigned long cleaned;
        uint32_t pages = 2, result;
        g_blocks = 0;
        wm_instance_free(inst);
        inst = NULL;
        check(!wm_instantiate(&inst, module, rt, NULL, NULL), "a paged memory");
        memory = wm_instance_memory(inst);
        g_paged = memory;
        wm_call(rt, export_func(inst, "grow"), &(uint32_t){1}, &result);
        wm_call(rt, export_func(inst, "fill"), &pages, &result);
        check(wm_memory_data(memory, NULL) == NULL, "it is paged");
        free(before);
        before = contents(memory, bytes_in);
        cleaned = clean_all(memory);
        check(cleaned > 0 && clean_all(memory) == 0, "its dirty frames are written once each");
        written = g_written;
        for (uint32_t i = 0; i < wm_memory_frames(memory); ++i)
            if (wm_memory_frame_look(memory, i) >= 0)
                wm_memory_frame_take(memory, i);
        check(wm_memory_resident(memory) == 0 && g_written == written, "then they leave RAM with no write");
        wm_call(rt, export_func(inst, "copy"), (uint32_t[]){8, 4096, 4}, NULL);
        check(clean_all(memory) > 0, "a store marks a cleaned frame dirty again");
        memcpy(before + 8, before + 4096, 4);
        after = contents(memory, bytes_in);
        check(!memcmp(before, after, bytes_in), "and the memory reads back as written");
        free(after);
    }

    free(before);
    if (other)
        wm_instance_free(other);
    wm_instance_free(inst);
    wm_module_free(other_module);
    wm_module_free(module);
    wm_runtime_free(rt);
    for (uint32_t i = 0; i < g_next_slot; ++i)
        free(g_slots[i]);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
