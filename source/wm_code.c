/*
 * The code cache: the records of predecoded functions (wm_fast.c), made when
 * a function is first called and dropped when room is needed. The hand of a
 * CLOCK goes round the functions of every module that runs predecoded; a
 * call marks its function used, the hand clears the mark and drops the
 * records of a function it finds unmarked. Functions on a call stack are
 * never dropped: the frames of every runtime, where a suspended one
 * stopped, and the callers of host functions that called back in.
 *
 * Room is made only outside the fast loop (at a call that stopped for
 * records, or a host function), where all of that is written down.
 */
#include "wm_internal.h"

#include <string.h>

#if WM_PREDECODE

const char wm_code_stop[] = "code miss";

static WmModule* g_modules;
static WmRuntime* g_runtimes;
static size_t g_bytes;
static size_t g_limit;
static WmAllocator g_allocator;
/* The CLOCK hand: a function of a module in g_modules. */
static WmModule* g_hand_module;
static uint32_t g_hand;

static uint32_t defined(const WmModule* m) { return m->func_count - m->func_imports; }

void wm_set_code_allocator(WmAllocator allocator) { g_allocator = allocator; }
void wm_code_cache_limit(size_t bytes) { g_limit = bytes; }
size_t wm_code_cache_bytes(void) { return g_bytes; }

void* wm_code_malloc(size_t size) { return g_allocator ? g_allocator(NULL, size ? size : 1u) : wm_malloc(size); }

void wm_code_free(void* ptr)
{
    if (!ptr)
        return;
    if (g_allocator)
        g_allocator(ptr, 0);
    else
        wm_free(ptr);
}

void wm_code_drop(WmCode* code)
{
    if (!code->fast)
        return;
    wm_code_free(code->fast);
    g_bytes -= (size_t)code->fast_cells * sizeof(WmCell);
    code->fast = NULL;
    code->fast_cells = 0;
    code->used = 0;
}

void wm_code_add_module(WmModule* m)
{
    m->cached_next = g_modules;
    g_modules = m;
}

void wm_code_remove_module(WmModule* m)
{
    for (WmModule** at = &g_modules; *at; at = &(*at)->cached_next)
        if (*at == m)
        {
            *at = m->cached_next;
            break;
        }
    if (g_hand_module == m)
    {
        g_hand_module = NULL;
        g_hand = 0;
    }
    for (uint32_t i = 0; i < defined(m); ++i)
        wm_code_drop(&m->codes[i]);
}

void wm_code_add_runtime(WmRuntime* rt)
{
    rt->next = g_runtimes;
    g_runtimes = rt;
}

void wm_code_remove_runtime(WmRuntime* rt)
{
    for (WmRuntime** at = &g_runtimes; *at; at = &(*at)->next)
        if (*at == rt)
        {
            *at = rt->next;
            break;
        }
}

static void pin_func(const WmFunc* f, uint8_t on)
{
    if (f && f->code)
        ((WmCode*)f->code)->pinned = on;
}

/* Mark (or unmark) every function on a call stack. */
static void pin_stacks(uint8_t on)
{
    for (WmRuntime* rt = g_runtimes; rt; rt = rt->next)
    {
        for (uint32_t i = 0; i < rt->depth; ++i)
            pin_func(rt->frames[i].func, on);
        if (rt->suspended)
            pin_func(rt->saved.func, on);
        if (rt->nesting)
        {
            pin_func(rt->caller, on);
            for (const WmNest* n = rt->nest; n; n = n->up)
                pin_func(n->caller, on);
        }
    }
}

/* The function at the hand, the hand then one on; only when some module has functions. */
static WmCode* hand_next(void)
{
    for (;;)
    {
        if (!g_hand_module)
        {
            g_hand_module = g_modules;
            g_hand = 0;
        }
        if (g_hand < defined(g_hand_module))
            return &g_hand_module->codes[g_hand++];
        g_hand_module = g_hand_module->cached_next;
    }
}

/* Drop the records of one function, by CLOCK, with the stacks pinned; 0 when none can go. */
static int evict_one(void)
{
    uint32_t total = 0;
    for (const WmModule* m = g_modules; m; m = m->cached_next)
        total += defined(m);
    /* Twice round: the first pass may only clear marks. */
    for (uint32_t i = 0; i < 2u * total; ++i)
    {
        WmCode* code = hand_next();
        if (!code->fast || code->pinned)
            continue;
        if (code->used)
        {
            code->used = 0;
            continue;
        }
        wm_code_drop(code);
        return 1;
    }
    return 0;
}

static int is_exhaustion(WmResult result) { return result && !strncmp(result, "exhaustion", 10); }

/*
 * A function's records, dropping others while there is no memory for them.
 * side: its side table entries, NULL to make them again. Not counted in
 * the cache yet.
 */
WmResult wm_code_make(WmModule* m, WmCode* code, const WmSide* side)
{
    WmResult result;
    pin_stacks(1);
    code->pinned = 1;
    do
    {
        WmSide* made = NULL;
        if (side || !(result = wm_code_side(m, (uint32_t)(code - m->codes), &made)))
            result = wm_predecode(m, code, side ? side : made);
        wm_free(made);
    } while (is_exhaustion(result) && evict_one());
    code->pinned = 0;
    pin_stacks(0);
    return result;
}

WmResult wm_code_load(WmFunc* func)
{
    WmModule* m = (WmModule*)func->instance->module;
    WmCode* code = (WmCode*)func->code;
    WmResult result;
    if (code->fast)
        return NULL;
    if (!WM_RUNS_FAST(m))
        return WM_ERR_TRAP("trap: a call into code that is not predecoded");
    if ((result = wm_code_make(m, code, NULL)))
        return result;
    g_bytes += (size_t)code->fast_cells * sizeof(WmCell);
    code->used = 1;
    if (g_limit && g_bytes > g_limit)
    {
        pin_stacks(1);
        code->pinned = 1;
        while (g_bytes > g_limit && evict_one())
            ;
        code->pinned = 0;
        pin_stacks(0);
    }
    return NULL;
}

size_t wm_code_cache_trim(size_t keep)
{
    pin_stacks(1);
    while (g_bytes > keep && evict_one())
        ;
    pin_stacks(0);
    return g_bytes;
}

#else

void wm_set_code_allocator(WmAllocator allocator) { (void)allocator; }
void wm_code_cache_limit(size_t bytes) { (void)bytes; }
size_t wm_code_cache_bytes(void) { return 0; }
size_t wm_code_cache_trim(size_t keep)
{
    (void)keep;
    return 0;
}

#endif
