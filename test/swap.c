/*
 * Swapping: with a pager every memory is paged, frames come into RAM when
 * touched, and the pager takes them out into its storage and brings them
 * back. Here RAM holds 3 frames, so nearly every access swaps.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"
#include "../source/wm_internal.h"

/* Frames RAM holds: 3, or 1 when the memory is only a few large frames. */
#define BUDGET (FRAMES >= 16u ? 3u : 1u)
/* The 4 pages the test grows to, in frames. */
#define FRAMES (4u * 65536u / wm_frame_size())
/* The frames fill and sum touch: they go 4 KiB at a time. */
#define TOUCHED (wm_frame_size() < 4096u ? 64u : FRAMES)

static int g_failures;
/* Frames promised to memories, and how many the pager will promise. */
static int32_t g_reserved, g_reserve_limit = 1 << 30;
static uint8_t* g_slots[4096];
static unsigned long g_taken, g_back, g_written;
/* Dirty frames being written in the background: at most MAX_WRITES, ended by end_write. */
#define MAX_WRITES 4u
static struct
{
    uint32_t slot;
    const uint8_t* frame;
    WmMemory* memory;
    uint32_t index;
} g_writes[MAX_WRITES];
static uint32_t g_writing;
static unsigned long g_write_starts;
static uint32_t g_most_resident;
static void end_write(uint32_t i, int tell);

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
    WmMemory* m;
    uint32_t at;
    for (uint32_t step = 0; step < 3u * wm_pager_resident() && wm_pager_hand(&m, &at); ++step)
        if (wm_memory_frame_look(m, at) == 0)
        {
            WmResult r = wm_memory_frame_take(m, at);
            if (!r || r == wm_page_wait)
                return (int)++g_taken;
        }
    return 0;
}

static int room(void* context, int again)
{
    (void)context;
    if (again)
    {
        /* No heap left: a write in flight ends, and its frame's RAM goes. */
        if (g_writing)
        {
            end_write(0, 1);
            return 1;
        }
        return take_one();
    }
    /* Frames being written are on their way out: they do not count against the budget. */
    while (wm_pager_resident() - g_writing >= BUDGET)
    {
        if (g_writing == MAX_WRITES)
            end_write(0, 1);
        else if (!take_one())
            break;
    }
    if (wm_pager_resident() + 1u > g_most_resident)
        g_most_resident = wm_pager_resident() + 1u;
    return 1;
}

static WmResult store(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t i = 0;
    (void)context;
    ++g_written;
    if (*slot != 0xFFFFFFFFu)
    {
        memcpy(g_slots[*slot], frame, wm_frame_size());
        return NULL;
    }
    while (i < 4096u && g_slots[i])
        ++i;
    if (i == 4096u || !(g_slots[i] = (uint8_t*)malloc(wm_frame_size())))
        return "exhaustion: storage full";
    memcpy(g_slots[i], frame, wm_frame_size());
    *slot = i;
    return NULL;
}

static WmResult load(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], wm_frame_size());
    ++g_back;
    return NULL;
}

static void drop(void* context, uint32_t slot)
{
    (void)context;
    free(g_slots[slot]);
    g_slots[slot] = NULL;
}

/*
 * Reading in the background: read_start only notes the read, and the test
 * ends it later (as storage would, with DMA or another thread).
 */
static struct
{
    int on;
    uint32_t slot;
    uint8_t* frame;
    WmMemory* memory;
    uint32_t index;
} g_loading;
static unsigned long g_waits;

static WmResult read_start(void* context, uint32_t slot, uint8_t* frame, WmMemory* memory, uint32_t index)
{
    (void)context;
    /* One read at a time in the background; another is made at once. */
    if (g_loading.on)
        return load(context, slot, frame);
    g_loading.on = 1;
    g_loading.slot = slot;
    g_loading.frame = frame;
    g_loading.memory = memory;
    g_loading.index = index;
    return wm_page_wait;
}

/* The read in flight ends: its bytes arrive. */
static void end_read(void)
{
    memcpy(g_loading.frame, g_slots[g_loading.slot], wm_frame_size());
    ++g_back;
    g_loading.on = 0;
}

static WmResult finish(void* context, uint32_t slot)
{
    (void)context;
    /* A write in flight: it ends now; the memory frees or keeps the frame itself. */
    for (uint32_t i = 0; i < g_writing; ++i)
        if (g_writes[i].slot == slot)
        {
            end_write(i, 0);
            return NULL;
        }
    if (!g_loading.on || g_loading.slot != slot)
        return "trap: no such read";
    end_read();
    return NULL;
}

/* A write in the background: the slot is chosen now, the bytes go when it ends. */
static WmResult write_start(void* context, uint32_t* slot, const uint8_t* frame, WmMemory* memory, uint32_t index)
{
    uint32_t i = 0;
    (void)context;
    if (g_writing == MAX_WRITES)
        return store(context, slot, frame);
    if (*slot == 0xFFFFFFFFu)
    {
        while (i < 4096u && g_slots[i])
            ++i;
        if (i == 4096u || !(g_slots[i] = (uint8_t*)malloc(wm_frame_size())))
            return "exhaustion: storage full";
        *slot = i;
    }
    g_writes[g_writing].slot = *slot;
    g_writes[g_writing].frame = frame;
    g_writes[g_writing].memory = memory;
    g_writes[g_writing].index = index;
    ++g_writing;
    ++g_write_starts;
    return wm_page_wait;
}

/* Write i ends: its bytes reach the slot, and the memory hears of it if tell. */
static void end_write(uint32_t i, int tell)
{
    WmMemory* memory = g_writes[i].memory;
    uint32_t index = g_writes[i].index;
    memcpy(g_slots[g_writes[i].slot], g_writes[i].frame, wm_frame_size());
    ++g_written;
    for (--g_writing; i < g_writing; ++i)
        g_writes[i] = g_writes[i + 1];
    if (tell)
        wm_memory_frame_stored(memory, index, NULL);
}

static WmPager g_pager = {NULL, reserve, room, store, load, drop, NULL, NULL, NULL};

/* A call that waits for frames: each time, the read ends and the call goes on. */
static uint32_t call_waiting(WmRuntime* rt, WmInstance* inst, const char* name, uint32_t a)
{
    WmExtern f;
    uint32_t args[3] = {a, 0, 0};
    uint32_t result = 0xDEADBEEFu;
    WmResult r;
    if (wm_instance_export(inst, name, (uint32_t)strlen(name), &f))
        return 0xDEADBEEFu;
    for (r = wm_call(rt, f.as.func, args, &result); r == wm_page_wait; r = wm_resume(rt, &result))
    {
        WmMemory* memory = g_loading.memory;
        uint32_t index = g_loading.index;
        ++g_waits;
        if (!g_loading.on)
            return 0xDEADBEEFu;
        end_read();
        wm_memory_frame_loaded(memory, index, NULL);
    }
    if (r)
        printf("  %s: %s\n", name, r);
    return result;
}

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
    check(wm_memory_data(memory, NULL) == NULL && g_reserved == (int32_t)(65536u / wm_frame_size()),
          "the memory is paged, its frames reserved");
    check(wm_memory_resident(memory) == 0, "no frame is in RAM before it is touched");

    /* 4 pages, 64 frames, through 3 frames of RAM. */
    check(call1(rt, inst, "grow", 3, 0, 0) == 1 && g_reserved == (int32_t)FRAMES, "memory.grow reserves the new frames");
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
        check(g_taken - taken > TOUCHED / 2u && g_written - written <= BUDGET, "frames that were only read leave without a write");
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

    /*
     * Frames read in the background: the call waits for each and goes on.
     * Only the predecoded loop waits; code run in place (also when a build
     * is too large to predecode, as with UBSan) reads frames at once.
     */
#if WM_PREDECODE
    if (module->codes[0].fast)
    {
        WmExtern f;
        uint32_t expected = call1(rt, inst, "sum", 4, 0, 0);
        unsigned long back = g_back;
        /* A word fill wrote: its frame goes to storage. */
        uint32_t args[3] = {3 * 65536, 0, 0};
        uint32_t result = 0;
        uint32_t direct = 0;
        g_pager.read_start = read_start;
        g_pager.finish = finish;
        check(call_waiting(rt, inst, "sum", 4) == expected && g_waits > TOUCHED / 2u && g_back - back > TOUCHED / 2u,
              "a call that waits for frames read in the background gets the same sum");
        /* The host needs a frame still being read: it waits for that read. */
        call_waiting(rt, inst, "sum", 1); /* the end of the memory goes to storage */
        wm_instance_export(inst, "peek", 4, &f);
        check(wm_call(rt, f.as.func, args, &result) == wm_page_wait && g_loading.on,
              "a touch of a stored frame starts a read and waits");
        check(!wm_memory_read(memory, 3 * 65536, &direct, 4) && !g_loading.on,
              "a host read of that frame finishes the read first");
        check(wm_resume(rt, &result) == NULL && result == direct, "the waiting call then goes on and reads the same");
        check(!g_loading.on, "no read is left in flight");
        g_pager.read_start = NULL;
        g_pager.finish = NULL;
    }
    else
        printf("  (not predecoded: no background reads)\n");
#endif

    /* Dirty frames written in the background: the program goes on while they are written. */
    {
        uint32_t expected;
        unsigned long starts;
        WmMemory* writing_memory;
        uint32_t writing_index, word = 0;
        call1(rt, inst, "fill", 4, 0, 0);
        expected = call1(rt, inst, "sum", 4, 0, 0);
        g_pager.write_start = write_start;
        g_pager.finish = finish;
        g_most_resident = 0;
        starts = g_write_starts;
        call1(rt, inst, "fill", 4, 0, 0);
        check(call1(rt, inst, "sum", 4, 0, 0) == expected && g_write_starts - starts > TOUCHED / 2u,
              "frames written in the background read back the same");
        check(g_most_resident <= BUDGET + MAX_WRITES, "RAM holds the budget plus the frames being written");
        /* A frame being written that is touched stays in RAM. */
        call1(rt, inst, "fill", 4, 0, 0);
        check(g_writing > 0, "some writes are in flight");
        if (!g_writing)
            return 1;
        writing_memory = g_writes[g_writing - 1].memory;
        writing_index = g_writes[g_writing - 1].index;
        check(!wm_memory_read(writing_memory, writing_index * wm_frame_size(), &word, 4), "a frame being written reads");
        while (g_writing)
            end_write(0, 1);
        {
            uint32_t again = 0;
            check(!wm_memory_read(writing_memory, writing_index * wm_frame_size(), &again, 4) && again == word,
                  "and keeps what it had once its write ends");
        }
        check(wm_memory_frame_look(writing_memory, writing_index) >= 0, "touched while written, it stayed in RAM");
        /* Writes still in flight when the memory goes: they end first (below). */
        call1(rt, inst, "fill", 4, 0, 0);
        /* With a few large frames all may have been written: take one more out. */
        while (!g_writing && take_one())
            ;
        check(g_writing > 0, "writes are in flight as the memory is freed");
    }

    /*
     * More paged memories than address space numbers: the last ones get no
     * number and are never in the TLB, and still read and write right, page
     * 0 too (an empty TLB entry once passed for page 0 of such a memory).
     */
    {
        uint32_t spaces = wm_frame_size() / 8u < 512u ? wm_frame_size() / 8u : 512u;
        uint32_t count = spaces + 4u;
        WmInstance** many = (WmInstance**)calloc(count, sizeof(WmInstance*));
        uint32_t made = 0, right = 0;
        for (; many && made < count && !wm_instantiate(&many[made], module, rt, NULL, NULL); ++made)
            ;
        for (uint32_t i = 0; i < made; ++i)
        {
            call1(rt, many[i], "fill", 1, 0, 0);
            right += call1(rt, many[i], "peek", 0, 0, 0) == 0 && call1(rt, many[i], "sum", 1, 0, 0) == call1(rt, many[0], "sum", 1, 0, 0);
        }
        check(made == count && right == count, "more memories than address space numbers all work, page 0 too");
        for (uint32_t i = 0; i < made; ++i)
            wm_instance_free(many[i]);
        free(many);
    }

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
