/*
 * A state snapshot: a call stopped by fuel, its runtime, instance and
 * module freed but its memory kept, goes on in a new instance of the
 * module made with that memory, and ends as it would have.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint8_t g_image[1 << 16];
static uint32_t g_length, g_at;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
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
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmMemory* memory;
    uint32_t args[3] = {4, 0, 0}, whole = 0, result = 0, before, state_length;
    if (f)
        fclose(f);

    check(size && !wm_module_load(&module, bytes, size) && !wm_instantiate(&inst, module, rt, NULL, NULL), "instantiate");
    if (!inst)
        return 1;
    check(wm_call(rt, export_func(inst, "grow"), (uint32_t[]){3, 0, 0}, &result) == NULL && result == 1, "grow to 4 pages");
    wm_call(rt, export_func(inst, "fill"), args, &result);
    check(wm_call(rt, export_func(inst, "sum"), args, &whole) == NULL, "sum it whole");

    /* The same sum stopped half way, its state written without the memory. */
    wm_set_fuel(rt, 600);
    check(wm_call(rt, export_func(inst, "sum"), args, &result) == wm_fuel_exhausted, "a sum stopped by fuel");
    check(!wm_snapshot_write_state(rt, inst, put, NULL), "its state, without the 256 KiB memory");
    state_length = g_length;
    wm_memory_read(wm_instance_memory(inst), 65536, &before, 4);

    /* Everything goes but the memory. */
    memory = wm_instance_detach_memory(inst);
    check(memory != NULL, "the memory is kept apart from the instance");
    wm_instance_free(inst);
    wm_runtime_free(rt);
    wm_module_free(module);
    module = NULL;
    inst = NULL;

    /* Back in a new instance of the module, with that memory. */
    rt = wm_runtime_new(64u * 1024u, 100u);
    check(!wm_module_load(&module, bytes, size) && !wm_instantiate_with_memory(&inst, module, memory, NULL, NULL) &&
              wm_instance_memory(inst) == memory,
          "a new instance takes the memory");
    {
        uint32_t now = 0;
        wm_memory_read(memory, 65536, &now, 4);
        check(now == before, "its data segments are not written again");
    }
    check(!wm_snapshot_read(rt, inst, get, NULL), "the state loads");
    wm_disable_fuel(rt);
    check(wm_resume(rt, &result) == NULL && result == whole, "the sum goes on and ends as it would have");

    /* A full snapshot of it still has the memory, and a state one needs it. */
    g_length = g_at = 0;
    wm_set_fuel(rt, 600);
    wm_call(rt, export_func(inst, "sum"), args, &result);
    check(!wm_snapshot_write(rt, inst, put, NULL) && g_length > state_length + 64u * 8u,
          "a full snapshot holds the memory (64 runs of it), a state one does not");

    wm_instance_free(inst);
    wm_module_free(module);
    wm_runtime_free(rt);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
