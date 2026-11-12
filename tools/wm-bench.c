/*
 * CoreMark (or any program exporting run, importing env.clock_ms) on the
 * engine: in slices of fuel as an operating system runs it, or in one go.
 *
 *   wm-bench program.wasm [slice]      slice 0: no fuel
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../source/wm.h"

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static WmResult clock_ms(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)rt, (void)user, (void)memory;
    slots[0] = now_ms();
    return NULL;
}

static WmFunc* g_clock;

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* n, uint32_t nl, uint8_t k, WmExtern* out)
{
    (void)c, (void)mo, (void)ml, (void)n, (void)nl, (void)k;
    out->kind = WM_EXTERN_FUNC;
    out->as.func = g_clock;
    return NULL;
}

int main(int argc, char** argv)
{
    static const uint8_t i32[] = {WM_I32};
    uint32_t slice = argc > 2 ? (uint32_t)atoi(argv[2]) : 10000u;
    FILE* f = fopen(argc > 1 ? argv[1] : "", "rb");
    static uint8_t bytes[1 << 20];
    size_t size;
    WmModule* m;
    WmInstance* in;
    WmExtern run;
    WmResult r;
    WmRuntime* rt = wm_runtime_new(64 * 1024, 1000);
    uint32_t result[2];
    uint64_t slices = 1;
    (void)slices;
    uint32_t start;
    float score;
    if (!f)
    {
        fprintf(stderr, "usage: %s program.wasm [slice] [code cache bytes]\n", argv[0]);
        return 2;
    }
    if (argc > 3)
        wm_code_cache_limit((size_t)strtoul(argv[3], NULL, 0));
    size = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    g_clock = wm_host_function_new(NULL, 0, i32, 1, clock_ms, NULL);
    if ((r = wm_module_load(&m, bytes, (uint32_t)size)) || (r = wm_instantiate(&in, m, rt, resolve, NULL)) ||
        (r = wm_instance_export(in, "run", 3, &run)))
    {
        printf("error %s\n", r);
        return 1;
    }
    start = now_ms();
    if (slice)
        wm_set_fuel(rt, slice);
    r = wm_call(rt, run.as.func, NULL, result);
    while (r == wm_fuel_exhausted)
    {
        wm_set_fuel(rt, slice);
        r = wm_resume(rt, result);
        ++slices;
    }
    if (r)
    {
        printf("trap %s\n", r);
        return 1;
    }
    memcpy(&score, result, 4);
    printf("CoreMark %.1f in %u ms (%s)\n", score, now_ms() - start, slice ? "slices of fuel" : "no fuel");
    /* Side table, records and local offsets: the records only of the functions that ran. */
    printf("code %u bytes besides the module\n", wm_module_code_bytes(m));
    return 0;
}
