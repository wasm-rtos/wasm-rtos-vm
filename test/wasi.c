/* WASI without files (wm_wasi.h), alone: no operating system, a host of a few functions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "../source/wm_wasi.h"

static char g_out[64];
static uint32_t g_out_length, g_slept;
static const char g_exited[] = "test: exited";
static uint32_t g_code;

static uint64_t now_ns(void* context, uint32_t clock)
{
    (void)context;
    return 1000000000ull * (clock + 1) + 5;
}

static void sleep_ms(void* context, WmRuntime* runtime, uint32_t ms)
{
    (void)context, (void)runtime;
    g_slept += ms;
}

static uint32_t write_out(void* context, WmRuntime* runtime, uint32_t fd, const void* data, uint32_t length)
{
    (void)context, (void)runtime;
    if (!data)
        return 8; /* EBADF */
    if (fd == 1 && g_out_length + length < sizeof(g_out))
    {
        memcpy(g_out + g_out_length, data, length);
        g_out_length += length;
    }
    return 0;
}

static uint32_t args(void* context, WmRuntime* runtime, const char* const** out)
{
    static const char* const argv[] = {"prog", ""};
    (void)context, (void)runtime;
    *out = argv;
    return 2;
}

static WmResult on_exit_(void* context, WmRuntime* runtime, uint32_t code)
{
    (void)context, (void)runtime;
    g_code = code;
    return g_exited;
}

static const WmWasiHost g_host = {NULL, now_ns, 0, sleep_ms, NULL, write_out, args, NULL, on_exit_};
static WmModule* g_module;

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* name, uint32_t nl, uint8_t k, WmExtern* out)
{
    WmResult r;
    (void)c, (void)k;
    out->kind = WM_EXTERN_FUNC;
    if ((r = wm_wasi_resolve(&g_host, mo, ml, name, nl, &out->as.func)) || out->as.func)
        return r;
    if ((r = wm_wasi_resolve_absent(g_module, mo, ml, name, &out->as.func)) || out->as.func)
        return r;
    return "unknown import";
}

static WmResult call(WmInstance* instance, WmRuntime* rt, const char* name, uint32_t* args_, void* result)
{
    WmExtern e;
    WmResult r = wm_instance_export(instance, name, (uint32_t)strlen(name), &e);
    if (r)
        return r;
    return wm_call(rt, e.as.func, args_, result);
}

int main(void)
{
    FILE* file = fopen("test/wasi.wasm", "rb");
    static uint8_t bytes[4096];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    WmRuntime* rt = wm_runtime_new(64 * 1024, 1000);
    WmInstance* instance = NULL;
    uint32_t r32 = 0, arg = 1;
    uint64_t r64 = 0;
    WmResult r;
    fclose(file);

    check(!wm_module_load(&g_module, bytes, (uint32_t)size), "loads");
    r = wm_instantiate(&instance, g_module, rt, resolve, NULL);
    check(!r, r ? r : "links, absent calls too");
    if (r)
        return 1;

    check(!call(instance, rt, "write", &arg, &r32) && r32 == 12 && g_out_length == 12 && !memcmp(g_out, "hello, wasi\n", 12),
          "fd_write: both buffers, the count");
    arg = 5;
    check(!call(instance, rt, "write", &arg, &r32) && r32 == (uint32_t)-8, "fd_write elsewhere: the host's errno");
    g_out_length = 0;
    check(!call(instance, rt, "write_bad", NULL, &r32) && r32 == 21 && g_out_length == 0, "fd_write, a bad buffer: EFAULT, nothing written");

    arg = 1;
    check(!call(instance, rt, "clock", &arg, &r64) && r64 == 2000000005ull, "clock_time_get (wasi_unstable): the host's clock");
    arg = 9;
    check(!call(instance, rt, "clock", &arg, &r64) && r64 == (uint64_t)-1, "clock_time_get, no such clock: EINVAL");
    check(!call(instance, rt, "clock_fault", NULL, &r32) && r32 == 21, "clock_time_get past the memory: EFAULT");

    check(!call(instance, rt, "args", NULL, &r32) && r32 == 2000 + 60 + 1, "args: two, 6 bytes, the second empty");
    check(!call(instance, rt, "random", NULL, &r32) && r32 != 0, "random_get: its own generator");
    r = call(instance, rt, "random_trap", NULL, &r32);
    check(r && strstr(r, "out of bounds"), "random_get past the memory: traps");

    check(!call(instance, rt, "sleep", NULL, &r32) && r32 == 177 && g_slept == 5, "poll_oneoff, one clock: the event, slept 5 ms");
    r = call(instance, rt, "exit", NULL, NULL);
    check(r == g_exited && g_code == 3, "proc_exit: the host's trap, the code");
    check(!call(instance, rt, "absent", NULL, &r32) && r32 == 52, "a file call nothing answers: ENOSYS");
    check(!call(instance, rt, "absent2", NULL, &r32) && r32 == 52, "another: ENOSYS");

    wm_instance_free(instance);
    wm_module_free(g_module);
    wm_runtime_free(rt);
    wm_wasi_free();
    return summary();
}
