/*
 * A kept memory the module imports (decision 24): a program that defines a
 * memory and a library that imports it, the library's data changed, the
 * memory kept apart from the program, both instances freed, then both built
 * again on that memory. Nothing is written over what the program left: the
 * library's data stays as it was changed, and the program's too. The memory
 * is bound to the library's import and stays the caller's.
 */
#include <stdio.h>
#include <stdlib.h>
#include "test.h"

static WmMemory* g_memory;
static int g_asked;

static uint8_t* load(const char* path, uint32_t* size)
{
    FILE* file = fopen(path, "rb");
    uint8_t* bytes = (uint8_t*)malloc(4096);
    *size = (uint32_t)fread(bytes, 1, 4096, file);
    fclose(file);
    return bytes;
}

/* The program's memory, for the library; counts when it is asked for. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    (void)context, (void)module, (void)module_length, (void)name, (void)name_length;
    ++g_asked;
    out->kind = kind;
    out->as.memory = g_memory;
    return NULL;
}

static uint32_t call(WmRuntime* rt, WmInstance* inst, const char* name)
{
    uint32_t result = 0;
    return wm_call(rt, export_func(inst, name), NULL, &result) ? 0xFFFFFFFFu : result;
}

int main(void)
{
    uint32_t prog_size, lib_size, big_size;
    uint8_t* prog_bytes = load("test/kept_prog.wasm", &prog_size);
    uint8_t* lib_bytes = load("test/kept_lib.wasm", &lib_size);
    uint8_t* big_bytes = load("test/kept_big.wasm", &big_size);
    WmModule *prog = NULL, *lib = NULL, *big = NULL;
    WmInstance *p = NULL, *l = NULL, *b = NULL;
    WmRuntime* rt = wm_runtime_new(16 * 1024, 64);
    WmMemory* kept;
    WmExtern e;

    check(!wm_module_load(&prog, prog_bytes, prog_size) && !wm_module_load(&lib, lib_bytes, lib_size) &&
              !wm_module_load(&big, big_bytes, big_size),
          "a program that defines a memory, libraries that import it");
    check(!wm_instantiate(&p, prog, rt, NULL, NULL) && !wm_instance_export(p, "memory", 6, &e) &&
              (g_memory = e.as.memory) != NULL && !wm_instantiate(&l, lib, rt, resolve, NULL) && g_asked == 1,
          "both instantiated, the library on the program's memory");
    check(call(rt, l, "bump") == 'M' && call(rt, l, "bump") == 'N', "the library's data changed: L, then M, then N");

    kept = wm_instance_detach_memory(p);
    check(kept == g_memory && wm_instance_detach_memory(l) == NULL, "the memory kept apart from the program; the library owns none");
    wm_instance_free(l);
    wm_instance_free(p);
    l = p = NULL;
    g_asked = 0;
    g_memory = NULL;

    check(!wm_instantiate_with_memory(&p, prog, kept, NULL, NULL) && wm_instance_memory(p) == kept,
          "the program built again takes the kept memory, as before");
    check(!wm_instantiate_with_memory(&l, lib, kept, resolve, NULL) && wm_instance_memory(l) == kept && g_asked == 0,
          "the library built again on it: bound to its import, the resolver not asked");
    check(call(rt, l, "bump") == 'O' && call(rt, p, "first") == 'p',
          "nothing written over: the library's data goes on from N, the program's is there");
    check(wm_instance_detach_memory(l) == NULL, "the library still owns no memory");
    check(wm_instantiate_with_memory(&b, big, kept, resolve, NULL) != NULL && g_asked == 0,
          "a library whose import wants more pages than the kept memory has is refused");
    wm_instance_free(b);

    wm_instance_free(l);
    wm_instance_free(p);
    wm_module_free(big);
    wm_module_free(lib);
    wm_module_free(prog);
    wm_runtime_free(rt);
    free(prog_bytes);
    free(lib_bytes);
    free(big_bytes);
    return summary();
}
