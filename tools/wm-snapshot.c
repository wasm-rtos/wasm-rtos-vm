/*
 * Snapshots between builds: stop a call by fuel and save it, or load a
 * saved one and go on. Build it twice (say 64-bit native and 32-bit) and
 * save in one, load in the other.
 *
 *   wm-snapshot save program.wasm function argument fuel snapshot
 *   wm-snapshot load program.wasm function snapshot
 *
 * The program's imports are stubs (functions that return nothing or 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/wm.h"

static uint8_t* slurp(const char* path, uint32_t* size)
{
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    data = (uint8_t*)malloc((size_t)n + 1u);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    fclose(f);
    *size = (uint32_t)n;
    return data;
}

static WmResult stub(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)rt, (void)memory;
    if (user)
        slots[0] = 0;
    return NULL;
}

/* Every import a stub of its own type. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    (void)context, (void)module, (void)module_length, (void)name, (void)name_length, (void)kind;
    out->kind = WM_EXTERN_FUNC;
    out->as.func = NULL;
    return NULL;
}

static WmResult file_write(void* context, const void* data, uint32_t size)
{
    return fwrite(data, 1, size, (FILE*)context) == size ? NULL : "cannot write the snapshot";
}

static WmResult file_read(void* context, void* data, uint32_t size)
{
    return fread(data, 1, size, (FILE*)context) == size ? NULL : "the snapshot ends early";
}

/* Runs to the end in slices; the fuel it took and the result. */
static WmResult finish(WmRuntime* rt, WmResult r, uint32_t* result, unsigned long long* used)
{
    while (r == wm_fuel_exhausted)
    {
        *used += 1000000u;
        wm_set_fuel(rt, 1000000u);
        r = wm_resume(rt, result);
    }
    *used += 1000000u - wm_get_fuel(rt);
    return r;
}

/* Binds the module's unresolved imports to stubs (results 0). */
static void link_stubs(WmInstance* inst, WmFunc** stubs);

int main(int argc, char** argv)
{
    uint32_t size;
    uint8_t* bytes;
    WmModule* m;
    WmInstance* in;
    WmRuntime* rt = wm_runtime_new(64 * 1024, 1000);
    WmExtern f;
    WmResult r;
    uint32_t result = 0;
    unsigned long long used = 0;
    int save = argc == 7 && strcmp(argv[1], "save") == 0;
    if (!save && !(argc == 5 && strcmp(argv[1], "load") == 0))
    {
        fprintf(stderr, "usage: %s save program.wasm function argument fuel snapshot\n"
                        "       %s load program.wasm function snapshot\n", argv[0], argv[0]);
        return 2;
    }
    printf("%u-bit pointers\n", (unsigned)(sizeof(void*) * 8u));
    bytes = slurp(argv[2], &size);
    if (!bytes || (r = wm_module_load(&m, bytes, size)) || (r = wm_instantiate(&in, m, rt, resolve, NULL)) ||
        (r = wm_instance_export(in, argv[3], (uint32_t)strlen(argv[3]), &f)))
    {
        printf("error %s\n", bytes ? r : "cannot read the program");
        return 1;
    }
    link_stubs(in, NULL);
    if (save)
    {
        uint32_t arg = (uint32_t)atoi(argv[4]);
        uint32_t fuel = (uint32_t)atoi(argv[5]);
        FILE* out = fopen(argv[6], "wb");
        wm_set_fuel(rt, fuel);
        r = wm_call(rt, f.as.func, &arg, &result);
        if (r != wm_fuel_exhausted)
        {
            printf("the call ended before its fuel ran out: %s\n", r ? r : "finished");
            return 1;
        }
        printf("stopped after %u instructions, %u calls deep\n", fuel, wm_call_depth(rt));
        {
            WmResult written = wm_snapshot_write(rt, in, file_write, out);
            if (written)
            {
                printf("snapshot: %s\n", written);
                return 1;
            }
        }
        printf("saved %ld bytes\n", ftell(out));
        fclose(out);
        used = fuel;
        wm_set_fuel(rt, 0);
        r = finish(rt, r, &result, &used);
        printf("going on here: result %u, %llu instructions in all%s%s\n", result, used, r ? ", " : "", r ? r : "");
    }
    else
    {
        FILE* in_file = fopen(argv[4], "rb");
        if (!in_file || (r = wm_snapshot_read(rt, in, file_read, in_file)))
        {
            printf("load: %s\n", in_file ? r : "cannot open the snapshot");
            return 1;
        }
        fclose(in_file);
        wm_set_fuel(rt, 0);
        r = finish(rt, wm_fuel_exhausted, &result, &used);
        printf("loaded and went on: result %u, %llu instructions after the snapshot%s%s\n", result, used, r ? ", " : "",
               r ? r : "");
    }
    return 0;
}

#include "../source/wm_internal.h"

static void link_stubs(WmInstance* inst, WmFunc** stubs)
{
    (void)stubs;
    for (uint32_t i = 0; i < inst->module->func_imports; ++i)
    {
        WmFunc* f = &inst->own_funcs[i];
        f->host = stub;
        f->user = f->result_count ? (void*)1 : NULL;
    }
}
