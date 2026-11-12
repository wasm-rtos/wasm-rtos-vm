/*
 * The code cache: records are made at a function's first call and dropped
 * when room is needed, never those of a function on a call stack. Run with
 * caches of every size down to nothing, the calls give what they give with
 * all the records kept: through recursion, a host function that calls back
 * in and empties the cache meanwhile, calls stopped by fuel, and a snapshot
 * loaded into a fresh runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint8_t g_image[1 << 16];
static uint32_t g_length, g_at;
static WmInstance* g_inst;
static WmFunc* g_back;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

/* square(n), called back in, with every record that can go gone first. */
static WmResult host_back(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    uint32_t result = 0;
    WmResult r;
    (void)user, (void)memory;
    wm_code_cache_trim(0);
    r = wm_call(rt, export_func(g_inst, "square"), slots, &result);
    wm_code_cache_trim(0);
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

/* f(arg) in slices of fuel, each slice resumed in a new runtime from a snapshot of the last. */
static WmResult run_sliced(WmRuntime** rt, WmFunc* f, uint32_t arg, uint32_t* result)
{
    WmResult r;
    wm_set_fuel(*rt, 37);
    r = wm_call(*rt, f, &arg, result);
    while (r == wm_fuel_exhausted)
    {
        g_length = g_at = 0;
        if ((r = wm_snapshot_write(*rt, g_inst, put, NULL)))
            return r;
        wm_runtime_free(*rt);
        *rt = wm_runtime_new(64u * 1024u, 100u);
        wm_code_cache_trim(0);
        if ((r = wm_snapshot_read(*rt, g_inst, get, NULL)))
            return r;
        wm_set_fuel(*rt, 37);
        r = wm_resume(*rt, result);
    }
    return r;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    static const uint8_t i32[1] = {0x7F};
    static const size_t limits[] = {0, 100000, 200, 64, 1};
    char what[96];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/code.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    WmModule* module = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    uint32_t result = 0, arg;
    if (f)
        fclose(f);
    g_back = wm_host_function_new(i32, 1, i32, 1, host_back, NULL);

    check(size && !wm_module_load(&module, bytes, size) && !wm_instantiate(&g_inst, module, rt, resolve, NULL),
          "instantiate");
    if (!g_inst)
        return 1;
    if (wm_module_predecoded(module))
    {
        /* As with UBSan, which makes the interpreter too large: nothing to test. */
        printf("SKIP the module runs in place: %s\n", wm_module_predecoded(module));
        return 0;
    }
    check(wm_code_cache_bytes() == 0, "no records before the first call");
    arg = 3;
    check(!wm_call(rt, export_func(g_inst, "square"), &arg, &result) && result == 9, "square(3) = 9");
    check(wm_code_cache_bytes() > 0, "the call made records");
    check(wm_code_cache_trim(0) == 0, "with nothing running, all of them can go");

    for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i)
    {
        wm_code_cache_limit(limits[i]);
        arg = 15;
        snprintf(what, sizeof(what), "cache %zu: fib(15) = 610", limits[i]);
        check(!wm_call(rt, export_func(g_inst, "fib"), &arg, &result) && result == 610, what);
        /* The last function made keeps its records past a limit smaller than they are. */
        if (limits[i] >= 200u)
        {
            snprintf(what, sizeof(what), "cache %zu: no more records than that once the calls end", limits[i]);
            check(wm_code_cache_bytes() <= limits[i], what);
        }
        arg = 7;
        snprintf(what, sizeof(what), "cache %zu: a host call back in, the cache emptied around it", limits[i]);
        check(!wm_call(rt, export_func(g_inst, "outer"), &arg, &result) && result == 49 + 55 + 1, what);
        arg = 12;
        snprintf(what, sizeof(what), "cache %zu: fib(12) in slices, through snapshots", limits[i]);
        check(!run_sliced(&rt, export_func(g_inst, "fib"), arg, &result) && result == 144, what);
        arg = 5;
        snprintf(what, sizeof(what), "cache %zu: outer(5) in slices, through snapshots", limits[i]);
        check(!run_sliced(&rt, export_func(g_inst, "outer"), arg, &result) && result == 25 + 55 + 1, what);
        wm_disable_fuel(rt);
    }

    wm_instance_free(g_inst);
    wm_module_free(module);
    check(wm_code_cache_bytes() == 0, "a module freed takes its records along");
    wm_runtime_free(rt);
    wm_host_function_free(g_back);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
