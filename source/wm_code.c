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

static void wm_code_drop(WmCode* code)
{
    if (!code->fast)
        return;
    if (!code->mapped)
    {
        wm_code_free(code->own);
        g_bytes -= (size_t)code->fast_cells * sizeof(WmCell);
    }
    code->fast = NULL;
    code->fast_cells = 0;
    code->used = 0;
    code->mapped = 0;
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

static void pin_func(const WmFunc* f, uint8_t on)
{
    if (f && f->code)
        f->code->pinned = on;
}

/* Mark (or unmark) every function on a call stack. */
static void pin_stacks(uint8_t on)
{
    for (WmRuntime* rt = wm_runtimes(); rt; rt = rt->next)
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
        g_hand = 0;
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
        /* Records in the image take no RAM: nothing to gain. */
        if (!code->fast || code->pinned || code->mapped)
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

/* A function's records copied from the image, dropping others for the room; with none, it runs from the image. */
static void from_image(WmModule* m, WmCode* code)
{
    uint32_t i = (uint32_t)(code - m->codes);
    const WmCell* cells = m->image + m->image_index[i];
    uint32_t count = m->image_index[i + 1u] - m->image_index[i];
    WmCell* copy;
    pin_stacks(1);
    code->pinned = 1;
    while (!(copy = (WmCell*)wm_code_malloc((size_t)count * sizeof(WmCell))) && evict_one())
        ;
    code->pinned = 0;
    pin_stacks(0);
    code->fast_cells = (uint16_t)count;
    code->mapped = !copy;
    if (copy)
    {
        memcpy(copy, cells, (size_t)count * sizeof(WmCell));
        code->own = copy;
    }
    else
        code->fast = cells;
}

/*
 * A function's records, dropping others while there is no memory for them.
 * side: its side table entries, NULL to make them again. Not counted in
 * the cache yet. With a code image they are copied from it, or read there.
 */
WmResult wm_code_make(WmModule* m, WmCode* code, const WmSide* side)
{
    WmResult result;
    if (m->image && !side)
    {
        from_image(m, code);
        return NULL;
    }
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
    WmModule* m = func->instance->module;
    WmCode* code = func->code;
    WmResult result;
    if (code->fast)
        return NULL;
    if (!WM_RUNS_FAST(m))
        return WM_ERR_TRAP("trap: a call into code that is not predecoded");
    if ((result = wm_code_make(m, code, NULL)))
        return result;
    code->used = 1;
    if (code->mapped)
        return NULL;
    g_bytes += (size_t)code->fast_cells * sizeof(WmCell);
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

int wm_code_drop_one(void)
{
    int dropped;
    pin_stacks(1);
    dropped = evict_one();
    pin_stacks(0);
    return dropped;
}

size_t wm_code_cache_trim(size_t keep)
{
    pin_stacks(1);
    while (g_bytes > keep && evict_one())
        ;
    /* Functions that found no room run from the image until here: their next call copies them in if there is room. */
    for (WmModule* m = g_modules; m; m = m->cached_next)
        for (uint32_t i = 0; i < defined(m); ++i)
            if (m->codes[i].mapped && !m->codes[i].pinned)
                wm_code_drop(&m->codes[i]);
    pin_stacks(0);
    return g_bytes;
}

/*
 * Code images (decision 15): every function's records, then where each
 * starts (in cells, and the end), then a trailer, so the image is written
 * in one pass, as a ZIP file's directory comes last. Words in the
 * processor's own order: an image is only for the build that wrote it.
 */
enum
{
    IMAGE_MAGIC = 0x49434D57u, /* "WMCI" */
    IMAGE_VERSION = 1u,
    IMAGE_TRAILER = 8u
};

/* What a record's meaning depends on besides the module: the handlers' offsets and the flags predecoding follows. */
static uint32_t build_tag(void)
{
    const uint16_t* handlers = wm_fast_handlers();
    const uint32_t flags[] = {IMAGE_VERSION, (uint32_t)sizeof(void*), WM_F_COUNT, WM_FUSE, WM_NOP_RUNS,
                              WM_REF_SLOTS, WM_FRAME_SIZE, WM_HAS_FLOAT, WM_HAS_I64, WM_HAS_REF_TYPES,
                              WM_PROFILE, WM_MAX_PAGES};
    uint32_t crc = wm_crc32(0xFFFFFFFFu, flags, sizeof(flags));
    return ~wm_crc32(crc, handlers, WM_F_COUNT * (uint32_t)sizeof(uint16_t));
}

/* Predecoding is never built where program bytes need WM_RD8 (AVR): they are read as memory. */
static uint32_t module_crc(const WmModule* m) { return ~wm_crc32(0xFFFFFFFFu, m->bytes, m->size); }

typedef struct ImageOut
{
    WmSnapshotWrite write;
    void* context;
    WmResult error;
    uint32_t crc;
} ImageOut;

static void image_put(ImageOut* o, const void* data, uint32_t size)
{
    if (!o->error && size)
    {
        o->crc = wm_crc32(o->crc, data, size);
        o->error = o->write(o->context, data, size);
    }
}

WmResult wm_code_image_write(WmModule* m, WmSnapshotWrite write, void* context)
{
    uint32_t defined_count = defined(m), at = 0;
    uint32_t* index;
    uint32_t trailer[IMAGE_TRAILER];
    ImageOut o = {write, context, NULL, 0xFFFFFFFFu};
    wm_module_predecode(m);
    if (!WM_RUNS_FAST(m) || m->image)
        return WM_ERR_UNSUPPORTED("unsupported: a code image of a module not predecoded, or already mapped");
    index = (uint32_t*)wm_malloc(((size_t)defined_count + 1u) * sizeof(uint32_t));
    if (!index)
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    /* Each function's records as the cache has them, or made for the image and let go. */
    for (uint32_t i = 0; !o.error && i < defined_count; ++i)
    {
        WmCode* code = &m->codes[i];
        int made = !code->fast;
        if (made && (o.error = wm_code_make(m, code, NULL)))
            break;
        index[i] = at;
        at += code->fast_cells;
        image_put(&o, code->fast, code->fast_cells * (uint32_t)sizeof(WmCell));
        if (made)
        {
            wm_code_free(code->own);
            code->fast = NULL;
            code->fast_cells = 0;
        }
    }
    index[defined_count] = at;
    image_put(&o, index, (defined_count + 1u) * (uint32_t)sizeof(uint32_t));
    wm_free(index);
    trailer[0] = IMAGE_MAGIC;
    trailer[1] = IMAGE_VERSION;
    trailer[2] = build_tag();
    trailer[3] = module_crc(m);
    trailer[4] = m->size;
    trailer[5] = defined_count;
    trailer[6] = (at + defined_count + 1u + IMAGE_TRAILER) * (uint32_t)sizeof(uint32_t);
    image_put(&o, trailer, (IMAGE_TRAILER - 1u) * (uint32_t)sizeof(uint32_t));
    trailer[7] = ~o.crc;
    image_put(&o, &trailer[7], (uint32_t)sizeof(uint32_t));
    return o.error;
}

const char wm_code_image_invalid[] = "code image does not fit this module or build";

WmResult wm_code_image_map(WmModule* m, const void* image, uint32_t size)
{
    const uint32_t* words = (const uint32_t*)image;
    const uint32_t* trailer;
    const uint32_t* index;
    uint32_t defined_count = defined(m), cells;
    if (m->image || (m->predecode_tried && m->predecode_result))
        return WM_ERR_UNSUPPORTED("unsupported: a code image of a module not predecoded, or already mapped");
    if (!wm_fast_handlers())
        return WM_ERR_UNSUPPORTED("unsupported: handlers too far apart for predecoding");
    /* The trailer, then the index, then the records: each in bounds before it is read. */
    if (((uintptr_t)image & 3u) || (size & 3u) || size < IMAGE_TRAILER * 4u)
        return wm_code_image_invalid;
    trailer = words + size / 4u - IMAGE_TRAILER;
    if (trailer[0] != IMAGE_MAGIC || trailer[1] != IMAGE_VERSION || trailer[2] != build_tag() ||
        trailer[4] != m->size || trailer[5] != defined_count || trailer[6] != size ||
        size / 4u - IMAGE_TRAILER < defined_count + 1u)
        return wm_code_image_invalid;
    index = trailer - (defined_count + 1u);
    cells = (uint32_t)(index - words);
    if (index[0] != 0 || index[defined_count] != cells)
        return wm_code_image_invalid;
    for (uint32_t i = 0; i < defined_count; ++i)
        if (index[i + 1u] <= index[i] || index[i + 1u] - index[i] > 0x7FFFu)
            return wm_code_image_invalid;
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (~wm_crc32(0xFFFFFFFFu, image, size - 4u) != trailer[7] || trailer[3] != module_crc(m))
        return wm_code_image_invalid;
#endif
#if WM_HAS_VALIDATION
    /* A record is a jump into the loop: each function's are made again and compared, one at a time. */
    for (uint32_t i = 0; i < defined_count; ++i)
    {
        WmCode* code = &m->codes[i];
        uint32_t count = index[i + 1u] - index[i];
        int made = !code->fast, same;
        WmResult result;
        if (made && (result = wm_code_make(m, code, m->side ? m->side + code->side : NULL)))
            return result;
        same = code->fast_cells == count && !memcmp(code->fast, words + index[i], (size_t)count * sizeof(WmCell));
        if (made)
        {
            wm_code_free(code->own);
            code->fast = NULL;
            code->fast_cells = 0;
        }
        if (!same)
            return wm_code_image_invalid;
    }
#endif
    m->image = (const WmCell*)image;
    m->image_index = index;
    return NULL;
}

#else

const char wm_code_image_invalid[] = "code image does not fit this module or build";

WmResult wm_code_image_write(WmModule* module, WmSnapshotWrite write, void* context)
{
    (void)module, (void)write, (void)context;
    return WM_ERR_UNSUPPORTED("unsupported: built without predecoding");
}

WmResult wm_code_image_map(WmModule* module, const void* image, uint32_t size)
{
    (void)module, (void)image, (void)size;
    return WM_ERR_UNSUPPORTED("unsupported: built without predecoding");
}

int wm_code_drop_one(void) { return 0; }
void wm_set_code_allocator(WmAllocator allocator) { (void)allocator; }
void wm_code_cache_limit(size_t bytes) { (void)bytes; }
size_t wm_code_cache_bytes(void) { return 0; }
size_t wm_code_cache_trim(size_t keep)
{
    (void)keep;
    return 0;
}

#endif
