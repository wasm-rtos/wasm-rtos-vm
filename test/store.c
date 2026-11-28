/*
 * Stores (decision 25): a program and a library that imports its memory,
 * made in a store with a table, a global and a runtime of it, freed in
 * one call whatever the order they were made in; the heap is as before.
 * A store counts what is its own and not what is the engine's; a failed
 * instance stays in it; a module of the engine tells its owner when its
 * last instance goes, in any store; the host refuses a store's memory or
 * table growth as a full heap would, without dropping code; releasing a store keeps its memory, and
 * instances made again on it go on from what it holds; a store freed with
 * a linker's bytes still charged goes with the last of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include "test.h"

/* The host's heap, counted: what the engine holds now. */
static size_t g_live;

static void* counted(void* pointer, size_t size)
{
    size_t* header = pointer ? (size_t*)pointer - 2 : NULL;
    size_t* moved;
    if (header)
        g_live -= header[0];
    if (size == 0)
    {
        free(header);
        return NULL;
    }
    if (!(moved = (size_t*)realloc(header, size + 2 * sizeof(size_t))))
    {
        if (header)
            g_live += header[0];
        return NULL;
    }
    moved[0] = size;
    g_live += size;
    return moved + 2;
}

/* The heap without the code cache, which keeps records of the engine's modules between calls. */
static size_t heap(void)
{
    wm_code_cache_trim(0);
    return g_live;
}

static uint8_t* load(const char* path, uint32_t* size)
{
    FILE* file = fopen(path, "rb");
    uint8_t* bytes = (uint8_t*)malloc(4096);
    *size = file ? (uint32_t)fread(bytes, 1, 4096, file) : 0;
    if (file)
        fclose(file);
    return bytes;
}

/* The program's memory, for the library. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    (void)module, (void)module_length, (void)name, (void)name_length;
    out->kind = kind;
    out->as.memory = (WmMemory*)context;
    return NULL;
}

static uint32_t call(WmRuntime* rt, WmInstance* inst, const char* name, uint32_t argument)
{
    uint32_t result = 0;
    return wm_call(rt, export_func(inst, name), &argument, &result) ? 0xFFFFFFFFu : result;
}

static WmMemory* memory_of(WmInstance* inst)
{
    WmExtern e;
    return wm_instance_export(inst, "memory", 6, &e) ? NULL : e.as.memory;
}

/* Whether this build counts a store's bytes (not where WM_STORE_BYTES is 0, as on AVR): byte checks only then. */
static int g_counts;
#define BYTES(test) (!g_counts || (test))

/* The host of test's stores: the capped one may hold g_cap bytes of memories and tables; what was asked, kept. */
static WmStore* g_capped;
static size_t g_cap;
static int g_asked;
static uint8_t g_kind;
static size_t g_bytes;
static void* g_host;

static int growing(void* host, WmStore* store, uint8_t kind, size_t bytes)
{
    ++g_asked;
    if (store != g_capped)
        return 1;
    g_host = host;
    g_kind = kind;
    g_bytes = bytes;
    return bytes <= g_cap;
}

/* A module's owner, told when its last instance goes: it frees it then. */
static int g_unused;

static void free_unused(void* context, WmModule* module)
{
    ++g_unused;
    *(WmModule**)context = NULL;
    wm_module_free(module);
}

int main(void)
{
    uint32_t prog_size, lib_size, trap_size;
    uint8_t* prog_bytes = load("test/store_prog.wasm", &prog_size);
    uint8_t* lib_bytes = load("test/store_lib.wasm", &lib_size);
    uint8_t* trap_bytes = load("test/store_trap.wasm", &trap_size);
    WmModule *prog = NULL, *lib = NULL, *trap = NULL, *own = NULL;
    WmInstance *p = NULL, *l = NULL, *t = NULL;
    WmStore *store, *other;
    WmRuntime* rt;
    size_t base, before;

    wm_set_allocator(counted);
    store = wm_store_new(NULL);
    {
        void* probe = wm_store_allocate(store, NULL, 8);
        g_counts = wm_store_bytes(store) == 8;
        wm_store_allocate(store, probe, 0);
    }
    wm_store_free(store);
    printf("%s\n", g_counts ? "a store's bytes are counted" : "a store's bytes are not counted in this build");
    check(prog_size && lib_size && trap_size && !wm_module_load(&prog, prog_bytes, prog_size) &&
              !wm_module_load(&lib, lib_bytes, lib_size) && !wm_module_load(&trap, trap_bytes, trap_size),
          "modules of the engine: a program, a library that imports its memory, one whose start traps");
    base = heap();

    /* Everything in one store, freed in one call: the heap as before, whatever the order. */
    store = wm_store_new(&base);
    rt = wm_runtime_new_in(store, 16 * 1024, 64);
    check(store && wm_store_host(store) == &base && rt && BYTES(wm_store_bytes(store) > 16 * 1024),
          "a store, its host, and a runtime in it: charged to it");
    before = wm_store_bytes(store);
    check(!wm_instance_new(&p, store, prog, rt, NULL, NULL) && BYTES(wm_store_bytes(store) > before + 64 * 1024) &&
              wm_store_memory(store, 0) == memory_of(p) && wm_store_memory(store, 1) == NULL,
          "the program's instance in it, and its memory, the store's");
    check(!wm_instance_new(&l, store, lib, rt, resolve, memory_of(p)) && call(rt, p, "bump", 0) == 2 &&
              call(rt, l, "peek", 0) == 2,
          "the library on the program's memory, in the same store");
    check(wm_table_new_in(store, WM_FUNCREF, 4, 8) && wm_global_new_in(store, WM_I32, 1, 7) &&
              wm_memory_new_in(store, 1, 1, 16) && wm_store_memory(store, 1) != NULL,
          "a table, a global and a second memory made alone in it");
    check(wm_module_instance_count(prog) == 1 && wm_module_instance_count(lib) == 1,
          "each module counts its instance");
    wm_store_free(store);
    check(heap() == base && wm_module_instance_count(prog) == 0 && wm_module_instance_count(lib) == 0,
          "freed in one call: the program before the library that uses its memory, the heap as before");

    /* What is the engine's is charged to no store; a module for the store is. */
    before = heap();
    store = wm_store_new(NULL);
    {
        WmModule* engine = NULL;
        check(!wm_module_load(&engine, prog_bytes, prog_size) && BYTES(wm_store_bytes(store) == 0),
              "a module of the engine is charged to no store");
        wm_module_free(engine);
    }
    check(!wm_module_load_in(&own, store, prog_bytes, prog_size) && BYTES(wm_store_bytes(store) > 0),
          "a module loaded for the store is charged to it");
    rt = wm_runtime_new_in(store, 4096, 16);
    check(rt && !wm_instance_new(&p, store, own, rt, NULL, NULL) && call(rt, p, "bump", 0) == 2,
          "an instance of the store's own module");
    wm_instance_free(p);
    wm_module_free(own);
    wm_runtime_free(rt);
    check(BYTES(wm_store_bytes(store) > 0) && wm_store_memory(store, 0) != NULL,
          "freed alone, they go; the memory the instance made stays the store's");
    wm_memory_free(wm_store_memory(store, 0));
    check(BYTES(wm_store_bytes(store) == 0 && wm_store_peak_bytes(store) > 64 * 1024),
          "with the memory freed too, nothing is charged; the peak stays");
    wm_store_free(store);
    check(heap() == before, "the store goes, the heap as before");

    /* A start function that traps: the instance stays in the store, and goes with it. */
    store = wm_store_new(NULL);
    rt = wm_runtime_new_in(store, 4096, 16);
    check(wm_instance_new(&t, store, trap, rt, NULL, NULL) != NULL && t != NULL &&
              wm_module_instance_count(trap) == 1,
          "a start function that traps: the instance is left in the store");
    wm_store_free(store);
    check(heap() == base && wm_module_instance_count(trap) == 0, "and goes with it");

    /* A module of the engine tells its owner when its last instance goes, in any store. */
    store = wm_store_new(NULL);
    other = wm_store_new(NULL);
    {
        WmModule* shared = NULL;
        WmRuntime* r1 = wm_runtime_new_in(store, 4096, 16);
        WmRuntime* r2 = wm_runtime_new_in(other, 4096, 16);
        WmInstance *a = NULL, *b = NULL;
        check(!wm_module_load(&shared, prog_bytes, prog_size) && !wm_instance_new(&a, store, shared, r1, NULL, NULL) &&
                  !wm_instance_new(&b, other, shared, r2, NULL, NULL) && wm_module_instance_count(shared) == 2,
              "one module of the engine, an instance in each of two stores");
        wm_module_on_unused(shared, free_unused, &shared);
        wm_store_free(store);
        check(g_unused == 0 && shared != NULL && call(r2, b, "bump", 0) == 2,
              "the first store freed: the module stays, the other's instance runs");
        wm_store_release(other);
        check(g_unused == 1 && shared == NULL, "the other released: its owner is told once, and frees it");
        wm_store_free(other);
    }
    check(heap() == base, "the heap as before");

    /* The host decides how much a store's memories and tables may hold: asked before they are made or grow. */
    store = wm_store_new(&g_cap);
    other = wm_store_new(NULL);
    wm_set_store_growing(growing);
    {
        WmRuntime* r1 = wm_runtime_new_in(store, 4096, 16);
        WmRuntime* r2 = wm_runtime_new_in(other, 4096, 16);
        WmInstance *a = NULL, *b = NULL, *plain = NULL;
        WmRuntime* loose = wm_runtime_new(4096, 16);
        size_t code;
        WmResult refused;
        g_capped = store;
        check(!wm_instance_new(&b, other, prog, r2, NULL, NULL) && call(r2, b, "bump", 0) == 2 && g_asked > 0,
              "a program in the other store runs, its code made; its host was asked");
        code = wm_code_cache_bytes();
        g_cap = 32 * 1024;
        refused = wm_instance_new(&a, store, prog, r1, NULL, NULL);
        check(refused && !strncmp(refused, "exhaustion", 10) && a == NULL && g_kind == WM_EXTERN_MEMORY &&
                  g_bytes == 64 * 1024 + 2 * sizeof(uintptr_t) && g_host == &g_cap && wm_code_cache_bytes() == code,
              "refused by the host: no instance, as when the heap is full; it was told the store's memory and table");
        g_cap = 64 * 1024 + 2 * sizeof(uintptr_t);
        check(!wm_instance_new(&a, store, prog, r1, NULL, NULL), "allowed exactly what it holds, it is made");
        check(call(r1, a, "grow", 1) == 0xFFFFFFFFu && g_bytes == 128 * 1024 + 2 * sizeof(uintptr_t) &&
                  call(r1, a, "grow", 0) == 1,
              "memory.grow the host refuses gives -1, the memory as it was");
        check(call(r1, a, "tgrow", 1) == 0xFFFFFFFFu && g_kind == WM_EXTERN_TABLE,
              "table.grow the host refuses gives -1");
        g_cap += sizeof(uintptr_t);
        check(call(r1, a, "tgrow", 1) == 2 && call(r1, a, "tgrow", 1) == 0xFFFFFFFFu,
              "one element more allowed: the table grows by it, and not again");
        check(call(r2, b, "bump", 0) == 3 && call(r2, b, "grow", 1) == 1, "the other store goes on and grows");
        g_asked = 0;
        check(!wm_instantiate(&plain, prog, loose, NULL, NULL) && call(loose, plain, "grow", 1) == 1 && g_asked == 0,
              "an instance of no store is not asked");
        wm_instance_free(plain);
        wm_runtime_free(loose);
        wm_set_store_growing(NULL);
        g_capped = NULL;
        wm_store_free(store);
        wm_store_free(other);
    }
    check(heap() == base, "the heap as before");

    /* Released, a store keeps its memory: instances made again on it go on from what it holds. */
    store = wm_store_new(NULL);
    other = wm_store_new(NULL);
    {
        WmMemory* kept;
        WmMemory* foreign = wm_memory_new_in(other, 1, 4, 16);
        rt = wm_runtime_new_in(store, 4096, 16);
        check(!wm_instance_new(&p, store, prog, rt, NULL, NULL) && !wm_instance_new(&l, store, lib, rt, resolve, memory_of(p)) &&
                  call(rt, p, "bump", 0) == 2 && call(rt, p, "bump", 0) == 3,
              "a program and its library, the program's byte counted to 3");
        kept = wm_store_memory(store, 0);
        wm_store_release(store);
        check(kept && wm_store_memory(store, 0) == kept && wm_store_memory(store, 1) == NULL &&
                  wm_module_instance_count(prog) == 0 && wm_module_instance_count(lib) == 0,
              "released: instances and runtime gone, the memory stays the store's");
        rt = wm_runtime_new_in(store, 4096, 16);
        check(wm_instance_new_with_memory(&p, store, prog, foreign, NULL, NULL) != NULL && p == NULL,
              "another store's memory is refused");
        check(!wm_instance_new_with_memory(&p, store, prog, kept, NULL, NULL) &&
                  !wm_instance_new_with_memory(&l, store, lib, kept, resolve, NULL) && memory_of(p) == kept &&
                  call(rt, l, "peek", 0) == 3 && call(rt, p, "bump", 0) == 4,
              "made again on it, program and library: no data written over it, the count goes on (4)");
        check(wm_instance_detach_memory(p) == NULL, "an instance of a store owns no memory to detach");
        wm_store_free(store);
        wm_store_free(other);
    }
    check(heap() == base, "the heap as before");

    /* A linker's records charged to a store, still there when it is freed: the store goes with the last. */
    store = wm_store_new(NULL);
    {
        void* record = wm_store_allocate(store, NULL, 100);
        check(record && BYTES(wm_store_bytes(store) == 100) && (record = wm_store_allocate(store, record, 200)) != NULL &&
                  BYTES(wm_store_bytes(store) == 200),
              "a linker's record charged to the store, grown");
        wm_store_free(store);
        check(BYTES(heap() > base), "the store freed first: it waits for the record");
        wm_store_allocate(NULL, record, 0);
        check(heap() == base, "the record freed: the store goes with it");
    }

    wm_module_free(prog);
    wm_module_free(lib);
    wm_module_free(trap);
    free(prog_bytes);
    free(lib_bytes);
    free(trap_bytes);
    return summary();
}
