/* Fuel, yield and resume: sliced runs must match whole ones, instruction for instruction. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint32_t* g_wait_slot;
static uint32_t g_waits;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/* Yields; the test writes the result into the slot before resuming. */
static WmResult host_wait(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)user, (void)memory;
    g_wait_slot = slots;
    ++g_waits;
    return wm_yield(rt);
}

static WmResult host_stop(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)user, (void)slots, (void)memory;
    wm_set_fuel(rt, 0);
    return NULL;
}

static WmFunc* g_wait;
static WmFunc* g_stop;

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* name, uint32_t nl, uint8_t k, WmExtern* out)
{
    (void)c, (void)mo, (void)ml, (void)k;
    out->kind = WM_EXTERN_FUNC;
    out->as.func = nl == 4 && memcmp(name, "wait", 4) == 0 ? g_wait : g_stop;
    return NULL;
}

/* Runs func(arg) in slices of fuel `slice` (0: one go); the fuel it used and the result. */
static WmResult run(WmRuntime* rt, WmFunc* f, uint32_t arg, uint32_t slice, uint32_t* result, uint64_t* used, uint32_t* slices)
{
    const uint32_t whole = 0xFFFFFFFFu;
    WmResult r;
    *used = 0;
    *slices = 1;
    wm_set_fuel(rt, slice ? slice : whole);
    r = wm_call(rt, f, &arg, result);
    while (r == wm_fuel_exhausted)
    {
        *used += slice;
        ++*slices;
        wm_set_fuel(rt, slice);
        r = wm_resume(rt, result);
    }
    *used += (slice ? slice : whole) - wm_get_fuel(rt);
    return r;
}

int main(void)
{
    static const uint8_t i32[] = {WM_I32};
    FILE* file = fopen("test/fuel.wasm", "rb");
    static uint8_t bytes[4096];
    size_t size = fread(bytes, 1, sizeof bytes, file);
    WmModule* m;
    WmInstance* in = NULL;
    WmRuntime* rt = wm_runtime_new(64 * 1024, 1000);
    WmExtern count, fib, waits, stops, traps;
    uint32_t result, slices;
    uint64_t whole_used, used;
    WmResult r;
    fclose(file);
    g_wait = wm_host_function_new(NULL, 0, i32, 1, host_wait, NULL);
    g_stop = wm_host_function_new(NULL, 0, NULL, 0, host_stop, NULL);
    check(!wm_module_load(&m, bytes, (uint32_t)size) && !wm_instantiate(&in, m, rt, resolve, NULL) &&
              !wm_instance_export(in, "count", 5, &count) && !wm_instance_export(in, "fib", 3, &fib) &&
              !wm_instance_export(in, "waits", 5, &waits) && !wm_instance_export(in, "stops", 5, &stops) &&
              !wm_instance_export(in, "traps", 5, &traps),
          "load the test module");

    /*
     * count(n): 9 instructions a pass (local.get, local.get, i32.ge_u, br_if,
     * local.get, i32.const, i32.add, local.set, br), and 9 more: block, loop,
     * the last check's 4, the block's end, local.get and the function's end.
     */
    r = run(rt, count.as.func, 1000, 0, &result, &whole_used, &slices);
    printf("     count(1000): %u in one go, fuel %llu\n", result, (unsigned long long)whole_used);
    check(!r && result == 1000 && whole_used == 1000ull * 9u + 9u, "count(1000) takes exactly 9 instructions a pass plus 9");
    r = run(rt, count.as.func, 1000, 7, &result, &used, &slices);
    printf("     in slices of 7: %u, fuel %llu, %u slices\n", result, (unsigned long long)used, slices);
    check(!r && result == 1000 && used == whole_used, "in slices of 7 the same result and the same fuel");

    r = run(rt, fib.as.func, 20, 0, &result, &whole_used, &slices);
    check(!r && result == 6765, "fib(20) in one go");
    r = run(rt, fib.as.func, 20, 100, &result, &used, &slices);
    printf("     fib(20): fuel %llu whole, %llu in %u slices of 100\n", (unsigned long long)whole_used,
           (unsigned long long)used, slices);
    check(!r && result == 6765 && used == whole_used, "fib(20) cut into slices of 100, resumed inside calls");

    wm_disable_fuel(rt);
    g_waits = 0;
    r = wm_call(rt, waits.as.func, NULL, &result);
    check(r == wm_yielded && wm_is_suspended(rt) && g_waits == 1, "a host function yields: the program stops after it");
    *g_wait_slot = 4;
    r = wm_resume(rt, &result);
    check(r == wm_yielded && g_waits == 2, "it goes on with the value the host left, and yields again");
    *g_wait_slot = 2;
    r = wm_resume(rt, &result);
    check(!r && result == 42 && !wm_is_suspended(rt), "the second value too: 4 * 10 + 2");

    wm_set_fuel(rt, 1000);
    r = wm_call(rt, stops.as.func, NULL, &result);
    check(r == wm_fuel_exhausted, "a host function sets the fuel to 0: the program stops right after it");
    wm_set_fuel(rt, 1000);
    r = wm_resume(rt, &result);
    check(!r && result == 7, "and finishes when given fuel again");

    r = run(rt, traps.as.func, 15, 50, &result, &used, &slices);
    check(r && strstr(r, "trap") && !wm_is_suspended(rt) && slices > 10, "a trap after many resumes ends the call cleanly");
    wm_disable_fuel(rt);
    r = wm_call(rt, count.as.func, (uint32_t[]){5}, &result);
    check(!r && result == 5, "the runtime works after it");

    wm_instance_free(in);
    wm_module_free(m);
    wm_runtime_free(rt);
    wm_host_function_free(g_wait);
    wm_host_function_free(g_stop);
    printf("%s: %d failed\n", g_failures ? "FAIL" : "ok", g_failures);
    return g_failures != 0;
}
