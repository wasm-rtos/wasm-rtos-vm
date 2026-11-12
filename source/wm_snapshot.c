/*
 * Snapshots: the state of a suspended call, written so that any build of
 * the engine can go on from it, on any chip. Positions are offsets in the
 * module's bytes and side table indices, which depend on the module alone;
 * functions are indices; everything is little-endian and holds no host
 * address.
 *
 *   header   magic "WMS1", version, reference slots, module hash, memory
 *            page size and pages, stack slots, frames
 *   call     the suspended call: entry function, base, depth, and where it
 *            stands (function, offset, side index, frame and stack slots)
 *   frames   the calls below it, outermost first
 *   stack    the used slots
 *   memory   runs of (zero bytes, data bytes, data); or, for a state
 *            snapshot that leaves the memory where it is, one run of
 *            (0xFFFFFFFF, 0xFFFFFFFF)
 *   globals  the module's own mutable globals
 *   tables   the module's own tables: size, then each element
 *   dropped  one byte per element segment, then per data segment
 *
 * A reference on the value stack is a host address, and references can
 * change the stack's layout: a module that can have them there loads only
 * into a build with references as wide. C programs have none.
 */
#include "wm_internal.h"

#include <stdlib.h>

#define WM_SNAPSHOT_MAGIC 0x31534D57u /* "WMS1" */
#define WM_SNAPSHOT_VERSION 1u
/* Zero runs shorter than this are written as data. */
#define MIN_ZERO_RUN 32u

const char wm_snapshot_invalid[] = "snapshot does not fit this module or runtime";

typedef struct Stream
{
    WmSnapshotWrite write;
    WmSnapshotRead read;
    void* context;
    WmResult error;
} Stream;

static void put(Stream* s, const void* data, uint32_t size)
{
    if (!s->error && size)
        s->error = s->write(s->context, data, size);
}

static void put32(Stream* s, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    put(s, bytes, 4);
}

static void put64(Stream* s, uint64_t value)
{
    put32(s, (uint32_t)value);
    put32(s, (uint32_t)(value >> 32));
}

static void get(Stream* s, void* data, uint32_t size)
{
    if (!s->error && size)
        s->error = s->read(s->context, data, size);
    if (s->error)
        memset(data, 0, size);
}

static uint32_t get32(Stream* s)
{
    uint8_t b[4];
    get(s, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t get64(Stream* s)
{
    uint64_t low = get32(s);
    return low | ((uint64_t)get32(s) << 32);
}

/* FNV-1a of the module's bytes: a snapshot loads only into the module it came from. */
static uint32_t module_hash(const WmModule* m)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < m->size; ++i)
        hash = (hash ^ WM_RD8(m->bytes + i)) * 16777619u;
    return hash;
}

/* A function of this instance by index, or WM_NONE. */
static uint32_t func_index(const WmInstance* inst, const WmFunc* f)
{
    if (f >= inst->own_funcs && f < inst->own_funcs + inst->module->func_count)
        return (uint32_t)(f - inst->own_funcs);
    return WM_NONE;
}

static WmFunc* func_at(const WmInstance* inst, uint32_t index)
{
    if (index >= inst->module->func_count || !inst->own_funcs[index].code)
        return NULL;
    return &inst->own_funcs[index];
}

static void put_position(Stream* s, const WmInstance* inst, const WmRuntime* rt, const WmFunc* func,
                         const uint8_t* pc, const WmSide* st, const uint32_t* fp)
{
    uint32_t offset = 0;
    uint32_t side = 0;
#if WM_PREDECODE
    /* A predecoded position is a record (its function on a stack keeps them): written as the instruction it stands for. */
    if (WM_RUNS_FAST(func->instance->module))
    {
        if (!wm_fast_position(func->instance->module, func->code, (const WmCell*)(const void*)pc, &offset, &side))
            s->error = wm_snapshot_invalid;
    }
    else
#endif
    {
        offset = (uint32_t)(pc - inst->module->bytes);
        side = (uint32_t)(st - inst->module->side);
    }
    put32(s, func_index(inst, func));
    put32(s, offset);
    put32(s, side);
    put32(s, (uint32_t)(fp - rt->stack));
}

/* A position read back, checked against the module and the stack. */
static WmResult get_position(Stream* s, WmInstance* inst, WmRuntime* rt, WmFunc** func, const uint8_t** pc,
                             const WmSide** st, uint32_t** fp, uint32_t stack_slots)
{
    const WmModule* m = inst->module;
    uint32_t index = get32(s);
    uint32_t offset = get32(s);
    uint32_t side = get32(s);
    uint32_t slot = get32(s);
    *func = func_at(inst, index);
    if (s->error)
        return s->error;
    if (!*func || offset >= m->size || (*func)->code->body > m->bytes + offset ||
        (*func)->code->end < m->bytes + offset || side > m->side_count || slot > stack_slots)
        return wm_snapshot_invalid;
    *fp = rt->stack + slot;
    *pc = m->bytes + offset;
    /* A predecoded module keeps only the count of its side table: to_record makes the position a record. */
    *st = m->side ? m->side + side : NULL;
    return NULL;
}

#if WM_PREDECODE
/*
 * A position in a predecoded function, read as its instruction: now its
 * record. The records are made here, once every frame is on the runtime,
 * so that making some cannot drop others the snapshot stopped in.
 */
static WmResult to_record(WmFunc* func, const uint8_t** pc, const WmSide** st)
{
    const WmModule* m = func->instance->module;
    const WmCell* ip;
    WmResult result;
    if (!WM_RUNS_FAST(m))
        return NULL;
    if ((result = wm_code_load(func)))
        return result;
    if (!(ip = wm_fast_ip(m, func->code, (uint32_t)(*pc - m->bytes))))
        return wm_snapshot_invalid;
    *pc = (const uint8_t*)(const void*)ip;
    *st = NULL;
    return NULL;
}
#endif

static uintptr_t put_ref_index(const WmInstance* inst, uint8_t type, uintptr_t ref)
{
    /* funcref: the function's index plus one, 0 for null; externref as it is. */
    if (type != WM_FUNCREF || ref == 0)
        return ref;
    return func_index(inst, (const WmFunc*)ref) + 1u;
}

static WmResult get_ref_index(const WmInstance* inst, uint8_t type, uint64_t value, uintptr_t* out)
{
    if (type != WM_FUNCREF || value == 0)
    {
        *out = (uintptr_t)value;
        return NULL;
    }
    if (value > inst->module->func_count)
        return wm_snapshot_invalid;
    *out = (uintptr_t)inst->funcs[value - 1u];
    return NULL;
}

/* The memory section of a state snapshot: the memory stays where it is. */
#define KEPT_MEMORY 0xFFFFFFFFu

static WmResult snapshot_write(const WmRuntime* rt, const WmInstance* inst, WmSnapshotWrite write, void* context,
                               int with_memory)
{
    const WmModule* m = inst->module;
    const WmSuspended* saved = &rt->saved;
    WmMemory* memory = inst->memory;
    uint32_t stack_slots;
    Stream s = {write, NULL, context, NULL};

    if (!rt->suspended || saved->entry_depth != 0 || func_index(inst, saved->entry) == WM_NONE)
        return WM_ERR_UNSUPPORTED("unsupported: snapshots are of a suspended call of one instance");
    stack_slots = (uint32_t)(saved->sp - rt->stack);

    put32(&s, WM_SNAPSHOT_MAGIC);
    put32(&s, WM_SNAPSHOT_VERSION);
    /* The stack's layout depends on the width of references only where they can be on it. */
    put32(&s, m->stack_refs ? WM_REF_SLOTS : 0u);
    put32(&s, module_hash(m));
    put32(&s, memory ? memory->page_log2 : 0);
    put32(&s, memory ? memory->pages : 0);
    put32(&s, stack_slots);
    put32(&s, rt->depth);

    put32(&s, func_index(inst, saved->entry));
    put32(&s, (uint32_t)(saved->base - rt->stack));
    put_position(&s, inst, rt, saved->func, saved->pc, saved->st, saved->fp);
    for (uint32_t i = 0; i < rt->depth; ++i)
    {
        const WmFrame* f = &rt->frames[i];
        if (func_index(inst, f->func) == WM_NONE)
            return WM_ERR_UNSUPPORTED("unsupported: a call of another instance is in progress");
        put_position(&s, inst, rt, f->func, f->pc, f->st, f->fp);
    }
    for (uint32_t i = 0; i < stack_slots; ++i)
        put32(&s, rt->stack[i]);

    if (memory && !with_memory && WM_MEMORY_SIZE(memory))
    {
        put32(&s, KEPT_MEMORY);
        put32(&s, KEPT_MEMORY);
    }
    else if (memory)
    {
#if WM_HAS_MMU
#define MEMORY_BYTE(AT) (data ? data[AT] : wm_memory_byte(memory, (uint32_t)(AT)))
#else
#define MEMORY_BYTE(AT) (data[AT])
#endif
        const uint8_t* data = memory->data;
        wm_size size = WM_MEMORY_SIZE(memory);
        wm_size at = 0;
        while (at < size)
        {
            wm_size zeros = at;
            wm_size end;
            while (zeros < size && MEMORY_BYTE(zeros) == 0)
                ++zeros;
            zeros -= at;
            if (zeros < MIN_ZERO_RUN && at + zeros < size)
                zeros = 0;
            at += zeros;
            /* Data up to the next long run of zeros. */
            end = at;
            while (end < size)
            {
                wm_size run = end;
                while (run < size && MEMORY_BYTE(run) == 0 && run - end < MIN_ZERO_RUN)
                    ++run;
                if (run - end >= MIN_ZERO_RUN || run == size)
                    break;
                end = run + 1u;
            }
            put32(&s, (uint32_t)zeros);
            put32(&s, (uint32_t)(end - at));
#if WM_HAS_MMU
            if (!data)
            {
                /* Paged: through a buffer, the same bytes as from one block. */
                uint8_t chunk[64];
                for (wm_size k = at; k < end;)
                {
                    uint32_t n = end - k < sizeof(chunk) ? (uint32_t)(end - k) : (uint32_t)sizeof(chunk);
                    wm_memory_read(memory, (uint32_t)k, chunk, n);
                    put(&s, chunk, n);
                    k += n;
                }
            }
            else
#endif
                put(&s, data + at, (uint32_t)(end - at));
            at = end;
        }
#undef MEMORY_BYTE
    }

    for (uint32_t i = m->global_imports; i < m->global_count; ++i)
    {
        const WmGlobal* g = inst->globals[i];
        if (!g->mutable_)
            continue;
        if (g->type == WM_FUNCREF || g->type == WM_EXTERNREF)
            put64(&s, (uint64_t)put_ref_index(inst, g->type, (uintptr_t)g->value));
        else
            put64(&s, g->value);
    }
    for (uint32_t i = m->table_imports; i < m->table_count; ++i)
    {
        const WmTable* t = inst->tables[i];
        put32(&s, t->size);
        for (uint32_t k = 0; k < t->size; ++k)
            put64(&s, (uint64_t)put_ref_index(inst, t->type, t->elems[k]));
    }
    put(&s, inst->elem_dropped, m->elem_count);
    put(&s, inst->data_dropped, m->data_count);
    return s.error;
}

WmResult wm_snapshot_write(const WmRuntime* rt, const WmInstance* inst, WmSnapshotWrite write, void* context)
{
    return snapshot_write(rt, inst, write, context, 1);
}

WmResult wm_snapshot_write_state(const WmRuntime* rt, const WmInstance* inst, WmSnapshotWrite write, void* context)
{
    return snapshot_write(rt, inst, write, context, 0);
}

WmResult wm_snapshot_read(WmRuntime* rt, WmInstance* inst, WmSnapshotRead read, void* context)
{
    const WmModule* m = inst->module;
    WmMemory* memory = inst->memory;
    WmSuspended* saved = &rt->saved;
    uint32_t capacity = (uint32_t)(rt->stack_end - rt->stack);
    uint32_t page_log2, pages, stack_slots, depth, entry, base;
    Stream s = {NULL, read, context, NULL};
    WmResult result;

    if (rt->suspended || rt->depth || rt->nesting)
        return WM_ERR_UNSUPPORTED("unsupported: a snapshot loads into a runtime that is not running");
    if (get32(&s) != WM_SNAPSHOT_MAGIC || get32(&s) != WM_SNAPSHOT_VERSION || get32(&s) != (m->stack_refs ? WM_REF_SLOTS : 0u) ||
        get32(&s) != module_hash(m))
        return s.error ? s.error : wm_snapshot_invalid;
    page_log2 = get32(&s);
    pages = get32(&s);
    stack_slots = get32(&s);
    depth = get32(&s);
    entry = get32(&s);
    base = get32(&s);
    if (s.error)
        return s.error;
    if ((memory ? memory->page_log2 : 0u) != page_log2 || (!memory && pages) || stack_slots > capacity ||
        depth > rt->max_frames || base > stack_slots || !func_at(inst, entry))
        return wm_snapshot_invalid;
    if (memory && pages != memory->pages)
    {
        if (pages < memory->pages || (memory->max_pages != WM_NONE && pages > memory->max_pages))
            return wm_snapshot_invalid;
        if (wm_memory_grow(memory, pages - memory->pages) == WM_NONE)
            return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }

    saved->entry = func_at(inst, entry);
    saved->base = rt->stack + base;
    saved->entry_depth = 0;
    if ((result = get_position(&s, inst, rt, &saved->func, &saved->pc, &saved->st, &saved->fp, stack_slots)))
        return result;
    saved->sp = rt->stack + stack_slots;
    for (uint32_t i = 0; i < depth; ++i)
    {
        WmFrame* f = &rt->frames[i];
        if ((result = get_position(&s, inst, rt, &f->func, &f->pc, &f->st, &f->fp, stack_slots)))
            return result;
    }
    for (uint32_t i = 0; i < stack_slots; ++i)
        rt->stack[i] = get32(&s);

    if (memory && WM_MEMORY_SIZE(memory))
    {
        wm_size size = WM_MEMORY_SIZE(memory);
        wm_size at = 0;
        uint32_t zeros = get32(&s);
        uint32_t length = get32(&s);
        /* A state snapshot: the memory is the one it was taken with, kept where it is. */
        int kept = zeros == KEPT_MEMORY && length == KEPT_MEMORY;
        if (!kept)
        {
#if WM_HAS_MMU
            if (!memory->data)
                wm_memory_fill(memory, 0, 0, (uint32_t)size);
            else
#endif
                memset(memory->data, 0, (size_t)size);
        }
        while (!kept && at < size && !s.error)
        {
            if (at)
            {
                zeros = get32(&s);
                length = get32(&s);
            }
            if (s.error || zeros > size - at || length > size - at - zeros)
                return s.error ? s.error : wm_snapshot_invalid;
            at += zeros;
#if WM_HAS_MMU
            if (!memory->data)
            {
                uint8_t chunk[64];
                for (uint32_t k = 0; k < length;)
                {
                    uint32_t n = length - k < sizeof(chunk) ? length - k : (uint32_t)sizeof(chunk);
                    get(&s, chunk, n);
                    if (wm_memory_write(memory, (uint32_t)at + k, chunk, n))
                        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
                    k += n;
                }
            }
            else
#endif
                get(&s, memory->data + at, length);
            at += length;
            if (zeros == 0 && length == 0)
                return wm_snapshot_invalid;
        }
    }

    for (uint32_t i = m->global_imports; i < m->global_count; ++i)
    {
        WmGlobal* g = inst->globals[i];
        uint64_t value;
        if (!g->mutable_)
            continue;
        value = get64(&s);
        if (g->type == WM_FUNCREF || g->type == WM_EXTERNREF)
        {
            uintptr_t ref;
            if ((result = get_ref_index(inst, g->type, value, &ref)))
                return result;
            value = (uint64_t)ref;
        }
        g->value = value;
    }
    for (uint32_t i = m->table_imports; i < m->table_count; ++i)
    {
        WmTable* t = inst->tables[i];
        uint32_t size = get32(&s);
        if (s.error)
            return s.error;
        if (size != t->size)
        {
            uintptr_t* elems;
            if (size < t->size || (t->max != WM_NONE && size > t->max) || size > 10000000u)
                return wm_snapshot_invalid;
            elems = (uintptr_t*)wm_realloc(t->elems, ((size_t)size + 1u) * sizeof(uintptr_t));
            if (!elems)
                return WM_ERR_EXHAUSTION("exhaustion: out of memory");
            t->elems = elems;
            t->size = size;
        }
        for (uint32_t k = 0; k < size; ++k)
            if ((result = get_ref_index(inst, t->type, get64(&s), &t->elems[k])))
                return result;
    }
    get(&s, inst->elem_dropped, m->elem_count);
    get(&s, inst->data_dropped, m->data_count);
    if (s.error)
        return s.error;
    inst->started = 1;
    rt->depth = depth;
    rt->sp = saved->sp;
    rt->suspended = 1;
#if WM_PREDECODE
    result = to_record(saved->func, &saved->pc, &saved->st);
    for (uint32_t i = 0; !result && i < depth; ++i)
        result = to_record(rt->frames[i].func, &rt->frames[i].pc, &rt->frames[i].st);
    if (result)
    {
        rt->depth = 0;
        rt->sp = rt->stack;
        rt->suspended = 0;
        return result;
    }
#endif
    return NULL;
}
