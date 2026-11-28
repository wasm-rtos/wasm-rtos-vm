/* Stores (decision 25): the owner of what a process makes. */
#include "wm_internal.h"

WmStore* wm_store_new(void* host)
{
    /* The store itself is charged to none: it outlives what is charged to it. */
    WmStore* store = (WmStore*)wm_store_calloc(NULL, 1, sizeof(WmStore));
    if (store)
        store->host = host;
    return store;
}

void* wm_store_host(const WmStore* store) { return store->host; }

void wm_store_release(WmStore* store)
{
    if (!store)
        return;
    /*
     * Instances first: they use the store's tables, globals, memories and
     * modules. Each free takes the object off its list, and a module's owner
     * told of its last instance may free what is its own meanwhile.
     */
    while (store->instances)
        wm_instance_free(store->instances);
    while (store->runtimes)
        wm_runtime_free(store->runtimes);
    while (store->tables)
        wm_table_free(&store->tables->table);
    while (store->globals)
        wm_global_free(&store->globals->global);
    while (store->modules)
        wm_module_free(store->modules);
}

void wm_store_free(WmStore* store)
{
    if (!store)
        return;
    wm_store_release(store);
    while (store->memories)
        wm_memory_free(store->memories);
#if WM_STORE_BYTES
    /* Bytes still charged (a linker's records not freed yet): the store goes with the last of them. */
    if (store->bytes)
    {
        store->freed = 1;
        return;
    }
#endif
    wm_free(store);
}

WmMemory* wm_store_memory(const WmStore* store, uint32_t index)
{
    uint32_t count = 0;
    WmMemory* memory;
    for (memory = store->memories; memory; memory = memory->store_next)
        ++count;
    if (index >= count)
        return NULL;
    /* Newest first in the list: the oldest is the last. */
    memory = store->memories;
    for (uint32_t i = count - 1u; i > index; --i)
        memory = memory->store_next;
    return memory;
}

static WmStoreGrowing g_growing;
int (*wm_store_check)(WmStore* store, uint8_t kind, size_t more);
static int check_growth(WmStore* store, uint8_t kind, size_t more);

/* Only a build that calls this carries the walk below (as the kernel's drivers, decision 3 there). */
void wm_set_store_growing(WmStoreGrowing growing)
{
    g_growing = growing;
    wm_store_check = growing ? check_growth : NULL;
}

/* What the store's memories and tables hold: walked, a handful of objects, so nothing is kept that could drift. */
static size_t growable(const WmStore* store)
{
    size_t bytes = 0;
    for (const WmMemory* m = store->memories; m; m = m->store_next)
        bytes += (size_t)WM_MEMORY_SIZE(m);
    for (const WmStoreTable* t = store->tables; t; t = t->store_next)
        bytes += (size_t)t->table.size * sizeof(uintptr_t);
    for (const WmInstance* i = store->instances; i; i = i->store_next)
        for (uint32_t k = 0; i->own_tables && k < i->module->table_count - i->module->table_imports; ++k)
            bytes += (size_t)i->own_tables[k].size * sizeof(uintptr_t);
    return bytes;
}

static int check_growth(WmStore* store, uint8_t kind, size_t more)
{
    size_t now = growable(store);
    return more <= (size_t)-1 - now && g_growing(store->host, store, kind, now + more);
}

#if WM_STORE_BYTES
size_t wm_store_bytes(const WmStore* store) { return store->bytes; }
size_t wm_store_peak_bytes(const WmStore* store) { return store->peak; }

void* wm_store_allocate(WmStore* store, void* ptr, size_t size)
{
    if (!size)
    {
        wm_free(ptr);
        return NULL;
    }
    return ptr ? wm_realloc(ptr, size) : wm_store_malloc(store, size);
}
#else
size_t wm_store_bytes(const WmStore* store)
{
    (void)store;
    return 0;
}
size_t wm_store_peak_bytes(const WmStore* store)
{
    (void)store;
    return 0;
}
void* wm_store_allocate(WmStore* store, void* ptr, size_t size)
{
    (void)store;
    return wm_allocate(ptr, size);
}
#endif
