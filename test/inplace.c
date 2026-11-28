/*
 * Functions in place by themselves (decision 21): a runtime that makes no
 * records, and records that find no room, give the same results and the
 * same fuel as records do, to the instruction: whole calls, calls in
 * slices, calls through a host function that calls back in, and slices
 * resumed in a fresh runtime from a snapshot with the other setting.
 * A runtime that makes no records uses those others made and adds none.
 * A function stopped in place moves to its records when the runtime may
 * make them again, in the middle of a loop that never calls (test/inplace.wat).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

#define FUEL 1000000u
#define SLICE 37u

static uint8_t g_image[1 << 16];
static uint32_t g_length, g_at;
static WmInstance* g_inst;
static WmFunc* g_back;
static int g_refusing;

/* square(n), called back in. */
static WmResult host_back(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    uint32_t result = 0;
    WmResult r;
    (void)user, (void)memory;
    r = wm_call(rt, export_func(g_inst, "square"), slots, &result);
    slots[0] = result;
    return r;
}

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* name, uint32_t nl, uint8_t k, WmExtern* out)
{
    (void)c, (void)mo, (void)ml, (void)name, (void)nl, (void)k;
    out->kind = WM_EXTERN_FUNC;
    out->as.func = g_back;
    return NULL;
}

/* Every block the engine holds, counted, so the test sees a leak on any platform. */
static long g_blocks;

static void* counted(void* ptr, size_t size)
{
    void* moved;
    if (!size)
    {
        g_blocks -= ptr != NULL;
        free(ptr);
        return NULL;
    }
    moved = realloc(ptr, size);
    g_blocks += moved && !ptr;
    return moved;
}

/* Records counted too, or none at all while refusing. */
static void* code_allocator(void* ptr, size_t size)
{
    return size && g_refusing ? NULL : counted(ptr, size);
}

static WmResult put(void* context, const void* data, uint32_t size)
{
    (void)context;
    if (g_length + size > sizeof(g_image))
        return "exhaustion: image too large";
    memcpy(g_image + g_length, data, size);
    g_length += size;
    return NULL;
}

static WmResult get(void* context, void* data, uint32_t size)
{
    (void)context;
    if (g_at + size > g_length)
        return "malformed: image too short";
    memcpy(data, g_image + g_at, size);
    g_at += size;
    return NULL;
}

typedef struct
{
    uint32_t result;
    uint32_t fuel;   /* fuel the call took */
    uint32_t slices; /* slices of SLICE it stopped in */
} Run;

/* How a run makes its records: as usual, none, none with others' kept, or refused. */
enum
{
    USUAL,
    MAKES_NONE,
    REFUSED,
    COUNT
};
static const char* const g_names[COUNT] = {"records", "making none", "refused"};

static void set_mode(WmRuntime* rt, int mode)
{
    wm_set_makes_code(rt, mode != MAKES_NONE);
    g_refusing = mode == REFUSED;
}

/* f(arg) whole. */
static WmResult run_whole(WmRuntime* rt, int mode, WmFunc* f, uint32_t arg, Run* out)
{
    WmResult r;
    set_mode(rt, mode);
    wm_set_fuel(rt, FUEL);
    r = wm_call(rt, f, &arg, &out->result);
    out->fuel = FUEL - wm_get_fuel(rt);
    out->slices = 0;
    g_refusing = 0;
    return r;
}

/*
 * f(arg) in slices; with snapshots, each slice resumed in a new runtime
 * from a snapshot of the last, the modes taking turns from mode on.
 */
static WmResult run_sliced(WmRuntime** rt, int mode, int snapshots, WmFunc* f, uint32_t arg, Run* out)
{
    WmResult r;
    set_mode(*rt, mode);
    out->slices = 0;
    wm_set_fuel(*rt, SLICE);
    r = wm_call(*rt, f, &arg, &out->result);
    while (r == wm_fuel_exhausted)
    {
        ++out->slices;
        if (snapshots)
        {
            mode = (mode + 1) % COUNT;
            g_refusing = 0;
            g_length = g_at = 0;
            if ((r = wm_snapshot_write(*rt, g_inst, put, NULL)))
                return r;
            wm_runtime_free(*rt);
            *rt = wm_runtime_new(test_store(), 64u * 1024u, 100u);
            set_mode(*rt, mode);
            if ((r = wm_snapshot_read(*rt, g_inst, get, NULL)))
                return r;
        }
        wm_set_fuel(*rt, SLICE);
        r = wm_resume(*rt, &out->result);
    }
    out->fuel = out->slices * SLICE + (SLICE - wm_get_fuel(*rt));
    g_refusing = 0;
    return r;
}

/* f(arg) in slices on one runtime, making no records in the first slice and as usual from then on. */
static WmResult run_turned(WmRuntime* rt, WmFunc* f, uint32_t arg, Run* out, size_t* records_after_first)
{
    WmResult r;
    set_mode(rt, MAKES_NONE);
    out->slices = 0;
    wm_set_fuel(rt, SLICE);
    r = wm_call(rt, f, &arg, &out->result);
    *records_after_first = wm_code_cache_bytes();
    while (r == wm_fuel_exhausted)
    {
        ++out->slices;
        set_mode(rt, USUAL);
        wm_set_fuel(rt, SLICE);
        r = wm_resume(rt, &out->result);
    }
    out->fuel = out->slices * SLICE + (SLICE - wm_get_fuel(rt));
    return r;
}

static void same(const Run* a, const Run* b, const char* what)
{
    char line[160];
    snprintf(line, sizeof(line), "%s: result %u, fuel %u, slices %u (records: %u, %u, %u)", what, b->result, b->fuel,
             b->slices, a->result, a->fuel, a->slices);
    check(a->result == b->result && a->fuel == b->fuel && a->slices == b->slices, line);
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    static const uint8_t i32[1] = {0x7F};
    static const struct
    {
        const char* name;
        uint32_t arg;
    } calls[] = {{"fib", 15}, {"square", 9}, {"outer", 7}};
    char what[128];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/code.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    WmModule* module = NULL;
    WmRuntime* rt;
    (void)test_store(); /* made before the count starts: it lasts the whole test */
    wm_set_allocator(counted);
    rt = wm_runtime_new(test_store(), 64u * 1024u, 100u);
    if (f)
        fclose(f);
    g_back = wm_host_function_new(i32, 1, i32, 1, host_back, NULL);
    wm_set_code_allocator(code_allocator);

    check(size && !wm_module_load(&module, NULL, bytes, size) && !wm_instance_new(&g_inst, test_store(), module, rt, resolve, NULL),
          "instantiate");
    if (!g_inst)
        return 1;
    if (wm_module_predecoded(module))
    {
        /* As with UBSan, which makes the interpreter too large: nothing to test. */
        printf("SKIP the module runs in place: %s\n", wm_module_predecoded(module));
        return 0;
    }

    for (uint32_t c = 0; c < sizeof(calls) / sizeof(calls[0]); ++c)
    {
        WmFunc* func = export_func(g_inst, calls[c].name);
        Run usual, run;
        wm_code_cache_trim(0);
        check(!run_whole(rt, USUAL, func, calls[c].arg, &usual), calls[c].name);
        for (int mode = MAKES_NONE; mode < COUNT; ++mode)
        {
            WmResult r;
            wm_code_cache_trim(0);
            r = run_whole(rt, mode, func, calls[c].arg, &run);
            snprintf(what, sizeof(what), "%s(%u) whole, %s, no records before", calls[c].name, calls[c].arg,
                     g_names[mode]);
            if (r)
                printf("  %s\n", r);
            same(&usual, &run, what);
            snprintf(what, sizeof(what), "%s(%u) whole, %s: no records made", calls[c].name, calls[c].arg,
                     g_names[mode]);
            check(wm_code_cache_bytes() == 0, what);
        }

        /* Some records there from an earlier call (fib's): used, none added. */
        {
            size_t before;
            uint32_t ten = 10, ignored;
            wm_code_cache_trim(0);
            wm_set_makes_code(rt, 1);
            wm_call(rt, export_func(g_inst, "fib"), &ten, &ignored);
            before = wm_code_cache_bytes();
            run_whole(rt, MAKES_NONE, func, calls[c].arg, &run);
            snprintf(what, sizeof(what), "%s(%u) whole, others' records used", calls[c].name, calls[c].arg);
            same(&usual, &run, what);
            snprintf(what, sizeof(what), "%s(%u), making none: the records there stay, none added", calls[c].name,
                     calls[c].arg);
            check(wm_code_cache_bytes() == before, what);
        }

        check(!run_sliced(&rt, USUAL, 0, func, calls[c].arg, &usual), "sliced");
        for (int mode = MAKES_NONE; mode < COUNT; ++mode)
        {
            wm_code_cache_trim(0);
            run_sliced(&rt, mode, 0, func, calls[c].arg, &run);
            snprintf(what, sizeof(what), "%s(%u) in slices, %s", calls[c].name, calls[c].arg, g_names[mode]);
            same(&usual, &run, what);
        }
        /* outer calls the host, which calls back in: no snapshot of a call nested in another. */
        if (strcmp(calls[c].name, "outer") != 0)
            for (int mode = USUAL; mode < COUNT; ++mode)
            {
                WmResult r;
                wm_code_cache_trim(0);
                r = run_sliced(&rt, mode, 1, func, calls[c].arg, &run);
                if (r)
                    printf("  %s\n", r);
                snprintf(what, sizeof(what), "%s(%u) in slices through snapshots, modes in turn from %s",
                         calls[c].name, calls[c].arg, g_names[mode]);
                same(&usual, &run, what);
            }
    }

    test_instance_free(g_inst);
    wm_module_free(module);
    check(wm_code_cache_bytes() == 0, "a module freed takes its records along");

    /* A loop that never calls, stopped in place: on to its records when the runtime makes them again. */
    f = fopen(argc > 2 ? argv[2] : "test/inplace.wasm", "rb");
    size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    if (f)
        fclose(f);
    module = NULL;
    g_inst = NULL;
    check(size && !wm_module_load(&module, NULL, bytes, size) && !wm_instance_new(&g_inst, test_store(), module, rt, resolve, NULL),
          "instantiate the loop");
    if (g_inst)
    {
        WmFunc* sum = export_func(g_inst, "sum");
        Run usual, run;
        size_t first;
        check(!run_sliced(&rt, USUAL, 0, sum, 300, &usual), "sum(300) in slices");
        wm_code_cache_trim(0);
        check(!run_sliced(&rt, MAKES_NONE, 0, sum, 300, &run) && wm_code_cache_bytes() == 0,
              "sum(300) in slices, making none: in place to the end");
        same(&usual, &run, "sum(300), making none");
        wm_code_cache_trim(0);
        check(!run_turned(rt, sum, 300, &run, &first), "sum(300), making none, then records");
        same(&usual, &run, "sum(300), making none, then records");
        check(first == 0 && wm_code_cache_bytes() > 0, "the stopped loop moved to its records");
        test_instance_free(g_inst);
        wm_module_free(module);
    }
    wm_runtime_free(rt);
    wm_host_function_free(g_back);
    {
        char line[64];
        snprintf(line, sizeof(line), "everything given back (%ld blocks held)", g_blocks);
        check(g_blocks == 0, line);
    }
    return summary();
}
