/*
 * CoreMark (or any program exporting run, importing env.clock_ms) on the
 * engine: in slices of fuel as an operating system runs it, or in one go.
 *
 *   wm-bench program.wasm [slice]      slice 0: no fuel
 *   WM_BENCH_CLOCK_STEP=ms             a clock that moves ms each call: the same run each time
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../source/wm.h"

/* The store this program's objects are made in (decision 25): one for the whole run. */
static WmStore* test_store(void)
{
    static WmStore* store;
    if (!store)
        store = wm_store_new(NULL);
    return store;
}

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* With WM_BENCH_CLOCK_STEP set, a clock that moves that many ms a call: the same run every time, for counting instructions. */
static uint32_t g_clock_step;
static uint32_t g_fake_ms;

/* The clock, as an i32 or an i64 of milliseconds: user is 2 for an i64. */
static WmResult clock_ms(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)rt, (void)memory;
    slots[0] = g_clock_step ? (g_fake_ms += g_clock_step) : now_ms();
    if (user)
        slots[1] = 0;
    return NULL;
}

static WmFunc* g_clock;

/* The clock of the type the program declares for it. */
static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* n, uint32_t nl, uint8_t k, WmExtern* out)
{
    const uint8_t* params;
    const uint8_t* results;
    uint32_t param_count, result_count;
    (void)mo, (void)ml, (void)nl, (void)k;
    if (!wm_module_import_type((const WmModule*)c, n, &params, &param_count, &results, &result_count) ||
        param_count || result_count != 1 || (results[0] != WM_I32 && results[0] != WM_I64))
        return "unlinkable: the clock is clock_ms() -> i32 or i64";
    g_clock = wm_host_function_new(NULL, 0, results, 1, clock_ms, results[0] == WM_I64 ? (void*)2 : NULL);
    out->kind = WM_EXTERN_FUNC;
    out->as.func = g_clock;
    return NULL;
}

int main(int argc, char** argv)
{
    uint32_t slice = argc > 2 ? (uint32_t)atoi(argv[2]) : 10000u;
    FILE* f = fopen(argc > 1 ? argv[1] : "", "rb");
    static uint8_t bytes[1 << 20];
    size_t size;
    WmModule* m;
    WmInstance* in;
    WmExtern run;
    WmResult r;
    WmRuntime* rt = wm_runtime_new(test_store(), 64 * 1024, 1000);
    uint32_t result[2];
    uint64_t slices = 1;
    uint32_t start;
    uint32_t elapsed;
    float score;
    if (!f)
    {
        fprintf(stderr, "usage: %s program.wasm [slice] [code cache bytes]\n", argv[0]);
        return 2;
    }
    if (argc > 3)
        wm_code_cache_limit((size_t)strtoul(argv[3], NULL, 0));
    if (getenv("WM_BENCH_CLOCK_STEP"))
        g_clock_step = (uint32_t)strtoul(getenv("WM_BENCH_CLOCK_STEP"), NULL, 0);
    size = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    if ((r = wm_module_load(&m, NULL, bytes, (uint32_t)size)) || (r = wm_instance_new(&in, test_store(), m, rt, resolve, m)) ||
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
    elapsed = now_ms() - start;
    memcpy(&score, result, 4);
    printf("CoreMark %.1f in %u ms (%s)\n", score, elapsed, slice ? "slices of fuel" : "no fuel");
    /* Fuel counts WebAssembly instructions: the time of one, whatever compiled the engine. */
    if (slice)
    {
        uint64_t instructions = (slices - 1u) * slice + (slice - wm_get_fuel(rt));
        printf("%llu instructions, %.3f ns each\n", (unsigned long long)instructions,
               (double)elapsed * 1e6 / (double)instructions);
    }
    /* Side table, records and local offsets: the records only of the functions that ran. */
    printf("code %u bytes besides the module\n", wm_module_code_bytes(m));
    return 0;
}
