/*
 * What the fuzz targets share: no allocation over FUZZ_ALLOC_MAX, imports
 * made for the module that asks for them, and running a module's start
 * function and exports with a budget of fuel. Included once by each target.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/wm.h"
#include "../../source/wm_internal.h"

#ifdef __APPLE__
#include <malloc/malloc.h>
#define fuzz_block_size malloc_size
#else
#include <malloc.h>
#define fuzz_block_size malloc_usable_size
#endif

/* Fuel for each call: enough for real work, little enough for many runs a second. */
#define FUZZ_FUEL 20000u
#define FUZZ_RESULT_SLOTS 64u
#define FUZZ_MADE 64u
/*
 * The engine's memory, records aside: no more than FUZZ_LIVE_MAX at once,
 * no block over FUZZ_ALLOC_MAX. Allocations fail past them, as on a board,
 * instead of reaching the fuzzer's limit (a 4 GiB memory paged in frames
 * takes them one by one).
 */
#define FUZZ_ALLOC_MAX ((size_t)16 << 20)
#define FUZZ_LIVE_MAX ((size_t)256 << 20)

static size_t g_fuzz_live;

static inline void* fuzz_alloc(void* ptr, size_t size)
{
    size_t old = ptr ? fuzz_block_size(ptr) : 0;
    void* p;
    if (!size)
    {
        g_fuzz_live -= old;
        free(ptr);
        return NULL;
    }
    if (size > FUZZ_ALLOC_MAX || g_fuzz_live - old + size > FUZZ_LIVE_MAX)
        return NULL;
    if (!(p = realloc(ptr, size)))
        return NULL;
    g_fuzz_live = g_fuzz_live - old + fuzz_block_size(p);
    return p;
}

/* Records, counted apart: the loops that run in place make none. */
static inline void* fuzz_code_alloc(void* ptr, size_t size)
{
    if (!size)
    {
        free(ptr);
        return NULL;
    }
    return size > FUZZ_ALLOC_MAX ? NULL : realloc(ptr, size);
}

static inline void fuzz_init(void)
{
    static int done;
    if (!done)
    {
        wm_set_allocator(fuzz_alloc);
        wm_set_code_allocator(fuzz_code_alloc);
        done = 1;
    }
}

#ifndef FUZZ_OWN_INITIALIZE
int LLVMFuzzerInitialize(int* argc, char*** argv);
int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc, (void)argv;
    fuzz_init();
    return 0;
}
#endif

/* What the resolver made for one instance, freed after it. */
typedef struct FuzzImports
{
    WmModule* module;
    /* Where the instance and its imports are made: freed with them, one store a run. */
    WmStore* store;
    WmExtern made[FUZZ_MADE];
    uint32_t count;
} FuzzImports;

/*
 * Every import made for it: a function to be linked to a stub that returns
 * zeros (fuzz_link), a memory, table or global of the type it declares.
 */
static inline WmResult fuzz_resolve(void* context, const char* module, uint32_t module_length, const char* name,
                             uint32_t name_length, uint8_t kind, WmExtern* out)
{
    FuzzImports* imports = (FuzzImports*)context;
    WmModule* m = imports->module;
    const WmImport* import = NULL;
    (void)module, (void)module_length, (void)name_length;
    for (uint32_t i = 0; i < m->import_count && !import; ++i)
        if (m->imports[i].name == (const uint8_t*)name)
            import = &m->imports[i];
    out->kind = kind;
    if (kind == WM_EXTERN_FUNC)
    {
        out->as.func = NULL;
        return NULL;
    }
    if (!import || imports->count == FUZZ_MADE)
        return "fuzz: an import it cannot make";
    if (kind == WM_EXTERN_MEMORY)
    {
        const WmLimits* l = &m->memory;
        uint32_t max = l->has_max ? l->max : l->min;
        out->as.memory = l->page_log2 == 16 ? wm_memory_new(imports->store, l->min, max, 16) : wm_memory_new(imports->store, l->min, max, l->page_log2);
    }
    else if (kind == WM_EXTERN_TABLE)
    {
        const WmTableType* t = &m->tables[import->index];
        out->as.table = wm_table_new(imports->store, t->type, t->limits.min, t->limits.has_max ? t->limits.max : t->limits.min);
    }
    else
    {
        const WmGlobalType* g = &m->globals[import->index];
        out->as.global = wm_global_new(imports->store, g->type, g->mutable_, 0);
    }
    if (!out->as.memory)
        return "fuzz: no memory for an import";
    imports->made[imports->count++] = *out;
    return NULL;
}

/* The instance and everything made for it, in its store. */
static inline void fuzz_imports_free(FuzzImports* imports)
{
    wm_store_free(imports->store);
    imports->store = NULL;
    imports->count = 0;
}

/* The store a fuzzer's runtimes are made in: one for the whole run (they are freed one by one). */
static inline WmStore* test_store(void)
{
    static WmStore* store;
    if (!store)
        store = wm_store_new(NULL);
    return store;
}

/* An instance of test_store() and the memory it made there: an instance's memory is its store's. */
static inline void fuzz_instance_free(WmInstance* inst)
{
    WmMemory* memory;
    wm_instance_free(inst);
    while ((memory = wm_store_memory(test_store(), 0)) != NULL)
        wm_memory_free(memory);
}

/* A function import nothing was linked to: returns zeros. */
static inline WmResult fuzz_stub(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)rt, (void)memory;
    memset(slots, 0, ((const WmFunc*)user)->result_slots * sizeof(uint32_t));
    return NULL;
}

static inline void fuzz_link(WmInstance* inst)
{
    /* The resolver left every function import unresolved: each is a stub of the engine's. */
    for (uint32_t i = 0; i < inst->module->func_imports; ++i)
    {
        WmFunc* f = inst->funcs[i];
        f->host = fuzz_stub;
        f->user = f;
    }
}

/* Loads and instantiates, the start function not yet run; NULL, or why not. */
static inline WmResult fuzz_instantiate(const uint8_t* data, size_t size, int in_place, FuzzImports* imports,
                                 WmInstance** out)
{
    WmResult r;
    imports->count = 0;
    imports->module = NULL;
    imports->store = NULL;
    *out = NULL;
    if (size > 0xFFFFFFFFu)
        return "fuzz: too large";
    if (!(imports->store = wm_store_new(NULL)))
        return "fuzz: no store";
    if ((r = wm_module_load(&imports->module, NULL, data, (uint32_t)size)))
        return r;
    /* The module runs in place: it is never predecoded. */
    if (in_place)
    {
        imports->module->predecode_tried = 1;
        imports->module->predecode_result = "fuzz: in place";
    }
    if ((r = wm_instance_new(out, imports->store, imports->module, NULL, fuzz_resolve, imports)))
    {
        /* A segment out of bounds leaves the instance in the store (wm.h): it goes with its imports. */
        *out = NULL;
        fuzz_imports_free(imports);
        wm_module_free(imports->module);
        imports->module = NULL;
        return r;
    }
    fuzz_link(*out);
    return NULL;
}

static inline void fuzz_free(WmInstance* inst, FuzzImports* imports)
{
    (void)inst;
    fuzz_imports_free(imports);
    if (imports->module)
        wm_module_free(imports->module);
    imports->module = NULL;
}

/* The exported function at export index e, or NULL. */
static inline WmFunc* fuzz_export_func(const WmInstance* inst, uint32_t e)
{
    const WmExport* x = &inst->module->exports[e];
    WmExtern ext;
    if (x->kind != WM_EXTERN_FUNC ||
        wm_instance_export(inst, (const char*)x->name, x->name_length, &ext) || ext.kind != WM_EXTERN_FUNC)
        return NULL;
    return ext.as.func;
}
