/*
 * Snapshots of linked instances (decision 23): a program and a library
 * that call each other, linked late, sharing a memory and a global of the
 * host's, with a host function in a table. A call stopped by fuel with
 * frames in both modules is saved from the program's instance, the whole
 * group is built again the same way, read back, and the call ends as it
 * would have. A group that is not the same is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include "test.h"

#define N 300u
/* What run(N) gives: the sum of 2i for i below N, nine, and the last i tick stored. */
#define WHOLE (N * (N - 1u) + 9u + (N - 1u))

static uint8_t g_image[1 << 17];
static uint32_t g_length, g_at;

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

static WmResult host_nine(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    slots[0] = 9u;
    return NULL;
}

/* What a host gives the two modules: a memory, a global, a function; functions of the modules are linked after. */
typedef struct Host
{
    WmMemory* mem;
    WmGlobal* acc;
    WmFunc* nine;
    WmInstance* lib;
    WmInstance* prog;
} Host;

static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    Host* h = (Host*)context;
    (void)module, (void)module_length;
    out->kind = kind;
    if (kind == WM_EXTERN_MEMORY)
        out->as.memory = h->mem;
    else if (kind == WM_EXTERN_GLOBAL)
        out->as.global = h->acc;
    else if (name_length == 4 && !memcmp(name, "nine", 4))
        out->as.func = h->nine;
    return NULL;
}

static uint32_t import_index(const WmModule* m, const char* name)
{
    WmExternType t;
    for (uint32_t i = 0; i < wm_module_imports(m, i, &t); ++i)
        if (t.kind == WM_EXTERN_FUNC && t.name_length == strlen(name) && !memcmp(t.name, name, t.name_length))
            return t.index;
    return 0xFFFFFFFFu;
}

/* The two instances, linked to each other: the same group every time. */
static int build(Host* h, WmModule* lib, WmModule* prog, int link)
{
    memset(h, 0, sizeof(*h));
    h->mem = wm_memory_new(1, 1);
    h->acc = wm_global_new(WM_I32, 1, 0);
    h->nine = wm_host_function_new(NULL, 0, (const uint8_t[]){WM_I32}, 1, host_nine, NULL);
    if (wm_instantiate(&h->lib, lib, NULL, resolve, h) || wm_instantiate(&h->prog, prog, NULL, resolve, h))
        return 0;
    return !link || (!wm_instance_link_function(h->prog, import_index(prog, "work"), export_func(h->lib, "work")) &&
                     !wm_instance_link_function(h->lib, import_index(lib, "tick"), export_func(h->prog, "tick")));
}

static void unbuild(Host* h)
{
    wm_instance_free(h->prog);
    wm_instance_free(h->lib);
    wm_host_function_free(h->nine);
    wm_global_free(h->acc);
    wm_memory_free(h->mem);
}

static uint8_t* load(const char* path, uint32_t* size)
{
    FILE* file = fopen(path, "rb");
    uint8_t* bytes = (uint8_t*)malloc(4096);
    *size = (uint32_t)fread(bytes, 1, 4096 - 16, file);
    fclose(file);
    return bytes;
}

int main(void)
{
    static const uint32_t fuels[] = {7, 50, 333, 1001, 2500};
    uint32_t lib_size, prog_size, result = 0;
    uint8_t* lib_bytes = load("test/group_lib.wasm", &lib_size);
    uint8_t* prog_bytes = load("test/group_prog.wasm", &prog_size);
    WmModule *lib = NULL, *prog = NULL, *other = NULL;
    WmRuntime* rt = wm_runtime_new(64 * 1024, 100);
    Host h;
    int ok = 1;
    char what[96];

    check(!wm_module_load(&lib, lib_bytes, lib_size) && !wm_module_load(&prog, prog_bytes, prog_size),
          "a library and a program");
    check(build(&h, lib, prog, 1) && !wm_call(rt, export_func(h.prog, "run"), (uint32_t[]){N}, &result) &&
              result == WHOLE,
          "linked to each other, run gives what it should without stopping");
    unbuild(&h);

    for (uint32_t k = 0; k < sizeof(fuels) / sizeof(fuels[0]); ++k)
    {
        WmRuntime* again = wm_runtime_new(64 * 1024, 100);
        WmResult r;
        g_length = g_at = 0;
        ok = build(&h, lib, prog, 1);
        wm_set_fuel(rt, fuels[k]);
        r = wm_call(rt, export_func(h.prog, "run"), (uint32_t[]){N}, &result);
        snprintf(what, sizeof what, "fuel %u: stopped with frames in both modules, saved from the program", fuels[k]);
        check(ok && r == wm_fuel_exhausted && !wm_snapshot_write(rt, h.prog, put, NULL), what);
        unbuild(&h);
        wm_runtime_free(rt);
        rt = again;
        ok = build(&h, lib, prog, 1) && !wm_snapshot_read(rt, h.prog, get, NULL);
        wm_set_fuel(rt, 0xFFFFFFFFu);
        snprintf(what, sizeof what, "fuel %u: the group built again, read, and the call ends as it would have", fuels[k]);
        check(ok && wm_resume(rt, &result) == NULL && result == WHOLE && g_at == g_length, what);
        unbuild(&h);
    }

    /* Not the same group. */
    g_length = g_at = 0;
    build(&h, lib, prog, 1);
    wm_set_fuel(rt, 333);
    wm_call(rt, export_func(h.prog, "run"), (uint32_t[]){N}, &result);
    check(!wm_snapshot_write(rt, h.prog, put, NULL), "saved once more, for the groups that differ");
    wm_set_fuel(rt, 0xFFFFFFFFu);
    check(!wm_resume(rt, &result) && result == WHOLE, "and goes on after it was saved");
    unbuild(&h);
    check(build(&h, lib, prog, 0) && wm_snapshot_read(rt, h.prog, get, NULL) == wm_snapshot_invalid,
          "the program alone, its library not linked: refused");
    unbuild(&h);
    /* The library with one byte more (a custom section at its end): another module, another hash. */
    lib_bytes[lib_size] = 0, lib_bytes[lib_size + 1] = 1, lib_bytes[lib_size + 2] = 0;
    g_at = 0;
    check(!wm_module_load(&other, lib_bytes, lib_size + 3) && build(&h, other, prog, 1) &&
              wm_snapshot_read(rt, h.prog, get, NULL) == wm_snapshot_invalid,
          "a library that is not the one saved: refused");
    unbuild(&h);
    g_at = 0;
    check(build(&h, lib, prog, 1) && !wm_snapshot_read(rt, h.prog, get, NULL) && !wm_resume(rt, &result) &&
              result == WHOLE,
          "and the right group still reads it after the refusals");
    unbuild(&h);

    wm_module_free(other);
    wm_module_free(prog);
    wm_module_free(lib);
    wm_runtime_free(rt);
    free(lib_bytes);
    free(prog_bytes);
    return summary();
}
