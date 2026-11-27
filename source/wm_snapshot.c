/*
 * Snapshots: the state of a suspended call, written so that any build of
 * the engine can go on from it, on any chip. Positions are offsets in the
 * module's bytes and side table indices, which depend on the module alone;
 * functions are indices; everything is little-endian and holds no host
 * address.
 *
 * A snapshot is of the instance given and every instance linked to it
 * (decision 23), its group: those whose functions its function imports
 * bind to or its tables and globals hold, and theirs, breadth first in
 * import order. The memories, tables and mutable globals they use, their
 * own and imported, are written once each, in the order the group meets
 * them. Functions are numbered across the group, instance by instance, as
 * each numbers its own (imports first).
 *
 *   header   magic "WMS1", version, reference slots, the instances and the
 *            hash of each one's module, the memories and each one's page
 *            size and pages, stack slots, frames
 *   call     the suspended call: entry function, base, depth, and where it
 *            stands (function, offset, side index, frame and stack slots)
 *   frames   the calls below it, outermost first
 *   stack    the used slots
 *   memories each: runs of (zero bytes, data bytes, data); or, for a state
 *            snapshot that leaves the memory where it is, one run of
 *            (0xFFFFFFFF, 0xFFFFFFFF)
 *   globals  the mutable globals
 *   tables   the tables: size, then each element
 *   dropped  each instance's: one byte per element segment, then per data
 *            segment
 *   crc      CRC-32 of all of the above
 *
 * A function reference on the value stack is written as its number plus
 * one (0 for null), in the first of its slots, as in globals and tables; a
 * host function by the first function import bound to it; an externref as
 * the host's value. References change the stack's layout: a
 * module that can have them there loads only into a build with references
 * as wide. C programs have none.
 *
 * Where the module is validated, so is a snapshot (decision 11): each frame
 * against what its function's code holds at its position.
 */
#include "wm_internal.h"

#include <stdlib.h>

#define WM_SNAPSHOT_MAGIC 0x31534D57u /* "WMS1" */
#define WM_SNAPSHOT_VERSION 3u
/* Zero runs shorter than this are written as data. */
#define MIN_ZERO_RUN 32u

const char wm_snapshot_invalid[] = "snapshot does not fit this module or runtime";

typedef struct Stream
{
    WmSnapshotWrite write;
    WmSnapshotRead read;
    void* context;
    WmResult error;
    uint32_t crc;
} Stream;

/* CRC-32 (the polynomial of zlib and Ethernet), half a byte at a time: a table of 64 bytes. */
static const uint32_t WM_ROM crc_nibbles[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t wm_crc32(uint32_t crc, const void* data, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    for (uint32_t i = 0; i < size; ++i)
    {
        crc = WM_ROM32(&crc_nibbles[(crc ^ p[i]) & 15u]) ^ (crc >> 4);
        crc = WM_ROM32(&crc_nibbles[(crc ^ (p[i] >> 4)) & 15u]) ^ (crc >> 4);
    }
    return crc;
}

static void crc_add(Stream* s, const void* data, uint32_t size) { s->crc = wm_crc32(s->crc, data, size); }

static void put(Stream* s, const void* data, uint32_t size)
{
    if (!s->error && size)
    {
        crc_add(s, data, size);
        s->error = s->write(s->context, data, size);
    }
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
    else
        crc_add(s, data, size);
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

/* A function object of this instance by its index, or WM_NONE: its own functions come first, then import stubs. */
static uint32_t func_index(const WmInstance* inst, const WmFunc* f)
{
    const WmModule* m = inst->module;
    uint32_t own = m->func_count - m->func_imports;
    if (f < inst->own_funcs || f >= inst->own_funcs + inst->own_func_count)
        return WM_NONE;
    if (f < inst->own_funcs + own)
        return m->func_imports + (uint32_t)(f - inst->own_funcs);
    for (uint32_t i = 0; i < m->func_imports; ++i)
        if (inst->funcs[i] == f)
            return i;
    return WM_NONE;
}

/* The instance a snapshot is of and those linked to it, and what they use (once each). */
typedef struct Group
{
    WmInstance** insts;
    uint32_t count;
    /* The number of each instance's first function across the group. */
    uint32_t* bases;
    WmMemory** memories;
    uint32_t memory_count;
    WmTable** tables;
    uint32_t table_count;
    /* Mutable only. */
    WmGlobal** globals;
    uint32_t global_count;
    uint8_t stack_refs;
} Group;

/* Adds item to the list unless it is there: 0 when out of memory. */
static int add(void*** list, uint32_t* count, void* item)
{
    void** more;
    for (uint32_t i = 0; i < *count; ++i)
        if ((*list)[i] == item)
            return 1;
    if (!(more = (void**)wm_realloc(*list, ((size_t)*count + 1u) * sizeof(void*))))
        return 0;
    more[(*count)++] = item;
    *list = more;
    return 1;
}

static void group_free(Group* g)
{
    wm_free(g->insts), wm_free(g->bases), wm_free(g->memories), wm_free(g->tables), wm_free(g->globals);
}

/* The instance of a function reference, to the group. */
static int add_ref(Group* g, uint8_t type, uintptr_t ref)
{
    const WmFunc* f = (const WmFunc*)ref;
    return type != WM_FUNCREF || !f || !f->instance || add((void***)&g->insts, &g->count, f->instance);
}

/* The group of inst, the same for the same links: what a snapshot is written of and read into. */
static WmResult group_make(Group* g, WmInstance* inst)
{
    int ok = add((void***)&g->insts, &g->count, inst);
    for (uint32_t i = 0; ok && i < g->count; ++i)
    {
        WmInstance* in = g->insts[i];
        const WmModule* m = in->module;
        for (uint32_t k = 0; ok && k < m->func_imports; ++k)
            ok = add_ref(g, WM_FUNCREF, (uintptr_t)in->funcs[k]);
        for (uint32_t k = 0; ok && k < m->table_count; ++k)
        {
            WmTable* t = in->tables[k];
            ok = add((void***)&g->tables, &g->table_count, t);
            for (uint32_t e = 0; ok && e < t->size; ++e)
                ok = add_ref(g, t->type, t->elems[e]);
        }
        for (uint32_t k = 0; ok && k < m->global_count; ++k)
        {
            WmGlobal* gl = in->globals[k];
            if (gl->mutable_)
                ok = add((void***)&g->globals, &g->global_count, gl) && add_ref(g, gl->type, (uintptr_t)gl->value);
        }
        if (ok && in->memory)
            ok = add((void***)&g->memories, &g->memory_count, in->memory);
        g->stack_refs |= m->stack_refs;
    }
    if (ok && (g->bases = (uint32_t*)wm_malloc((size_t)g->count * sizeof(uint32_t))) != NULL)
        for (uint32_t i = 0, base = 0; i < g->count; base += g->insts[i++]->module->func_count)
            g->bases[i] = base;
    if (!ok || !g->bases)
    {
        group_free(g);
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }
    return NULL;
}

/* A function's number across the group, or WM_NONE: the instance that holds its object, else (a host's) an import bound to it. */
static uint32_t group_index(const Group* g, const WmFunc* f)
{
    for (uint32_t i = 0; i < g->count; ++i)
    {
        uint32_t index = func_index(g->insts[i], f);
        if (index != WM_NONE)
            return g->bases[i] + index;
    }
    for (uint32_t i = 0; f && !f->instance && i < g->count; ++i)
        for (uint32_t k = 0; k < g->insts[i]->module->func_imports; ++k)
            if (g->insts[i]->funcs[k] == f)
                return g->bases[i] + k;
    return WM_NONE;
}

/* The function of that number: the object, as group_index found it; NULL past the group. */
static WmFunc* group_func(const Group* g, uint32_t number)
{
    for (uint32_t i = 0; i < g->count; ++i)
        if (number - g->bases[i] < g->insts[i]->module->func_count)
            return g->insts[i]->funcs[number - g->bases[i]];
    return NULL;
}

/* A function with code a frame may stand in, by its number. */
static WmFunc* group_code_at(const Group* g, uint32_t number)
{
    WmFunc* f = group_func(g, number);
    return f && f->code && group_index(g, f) == number ? f : NULL;
}

/* A frame as a snapshot has it: its function, the offset and side table index it stands at, its first slot. */
typedef struct Place
{
    WmFunc* func;
    uint32_t offset;
    uint32_t side;
    uint32_t fp;
} Place;

static WmResult place_of(const WmRuntime* rt, WmFunc* func, const uint8_t* pc, const WmSide* st, const uint32_t* fp,
                         int inplace, Place* out)
{
    /* Its code's module: a function bound to an import of another runs that one's code. */
    const WmModule* m = func->instance->module;
    out->func = func;
    out->fp = (uint32_t)(fp - rt->stack);
#if WM_PREDECODE
    /*
     * Stopped in the loop in place in a function of the register loop, or a
     * frame of a function running in place by itself (decision 21, st not
     * NULL): its own side table, from 0.
     */
    if (inplace || (st && WM_RUNS_FAST(m)))
    {
        out->offset = (uint32_t)(pc - m->bytes);
        out->side = func->code->side + (uint32_t)(st - func->code->tail->side);
        return NULL;
    }
#else
    (void)inplace;
#endif
#if WM_PREDECODE
    /* A predecoded position is a record (its function on a stack keeps them): written as the instruction it stands for. */
    if (WM_RUNS_FAST(m))
        return wm_fast_position(m, func->code, (const WmCell*)(const void*)pc, &out->offset,
                                &out->side)
                   ? NULL
                   : wm_snapshot_invalid;
#endif
    out->offset = (uint32_t)(pc - m->bytes);
    out->side = (uint32_t)(st - m->side);
    return NULL;
}

static void put_place(Stream* s, const Group* g, const Place* p)
{
    put32(s, group_index(g, p->func));
    put32(s, p->offset);
    put32(s, p->side);
    put32(s, p->fp);
}

static uint32_t leb_at(const uint8_t* p)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8(p++);
        if (shift < 32u)
            value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    return value;
}

static int same_type(const WmType* a, const WmType* b)
{
    return wm_types_equal(a->params, a->param_count, b->params, b->param_count) &&
           wm_types_equal(a->results, a->result_count, b->results, b->result_count);
}

/* Marks the function references among count values of types from slot on; 0, or WM_NONE past the end. */
static uint32_t mark_refs(const uint8_t* types, uint32_t count, uint32_t slot, uint32_t end, uint8_t* refs)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t type = types[i];
        uint32_t width = wm_type_slots(type);
        if (slot + width > end)
            return WM_NONE;
        if (type == WM_FUNCREF)
            refs[slot] = 1;
        slot += width;
    }
    return slot;
}

/*
 * The frames, outermost first, with what validation says each holds at
 * its position. With check set: each position the start of a reachable
 * instruction with its own side table entry; a caller's position just
 * after a call of the function above it, whose frame starts where the
 * call's arguments are; the outermost frame at base; the stack exactly
 * as high as the innermost frame's code says. With refs (one byte a
 * slot, zeroed), where function references start on the stack.
 */
static WmResult walk(const Place* places, uint32_t count, uint32_t base, uint32_t stack_slots, int check, uint8_t* refs)
{
    WmResult result = NULL;
    WmCodeStack at;
    WmCodeStack before;
    memset(&at, 0, sizeof(at));
    memset(&before, 0, sizeof(before));
    if (check && (!count || places[0].fp != base))
        return wm_snapshot_invalid;
    for (uint32_t k = 0; k < count && !result; ++k)
    {
        const Place* p = &places[k];
        const WmFunc* f = p->func;
        /* The code's module and its function there (decision 23: a frame may run another module's code). */
        const WmModule* m = f->instance->module;
        uint32_t index = (uint32_t)(f->code - m->codes);
        const uint8_t* operands;
        uint32_t operand_count;
        uint32_t operand_slots;
        uint32_t end = stack_slots;
        /* A recursion repeats its frames: what the last one found stays. */
        if (k == 0 || p->func != places[k - 1u].func || p->offset != places[k - 1u].offset)
        {
            wm_free(at.types), wm_free(at.locals), wm_free(before.types), wm_free(before.locals);
            memset(&before, 0, sizeof(before));
            if ((result = wm_code_stack(m, index, p->offset, &at)))
            {
                memset(&at, 0, sizeof(at));
                break;
            }
            if (k + 1u < count && (at.prev == WM_NONE || (result = wm_code_stack(m, index, at.prev, &before))))
            {
                memset(&before, 0, sizeof(before));
                result = wm_snapshot_invalid;
                break;
            }
        }
        if (check && at.side != p->side)
            result = wm_snapshot_invalid;
        operands = at.types;
        operand_count = at.count;
        operand_slots = at.slots;
        if (k + 1u < count)
        {
            /* A caller: what it held before its call, the call's arguments (and call_indirect's index) aside. */
            const Place* next = &places[k + 1u];
            const uint8_t* call = m->bytes + at.prev;
            uint8_t op = WM_RD8(call);
            uint32_t args = next->func->type->param_count + (op == WM_OP_CALL_INDIRECT);
            uint32_t arg_slots = next->func->param_slots + (op == WM_OP_CALL_INDIRECT);
            if (op != WM_OP_CALL && op != WM_OP_CALL_INDIRECT)
                result = wm_snapshot_invalid;
            else if (check && op == WM_OP_CALL && leb_at(call + 1) != func_index(f->instance, next->func))
                result = wm_snapshot_invalid;
            else if (check && op == WM_OP_CALL_INDIRECT && !same_type(&m->types[leb_at(call + 1)], next->func->type))
                result = wm_snapshot_invalid;
            else if (before.count < args || before.slots < arg_slots)
                result = wm_snapshot_invalid;
            operands = before.types;
            operand_count = before.count - args;
            operand_slots = before.slots - arg_slots;
            end = next->fp;
            if (check && next->fp != p->fp + f->param_slots + f->code->local_slots + operand_slots)
                result = wm_snapshot_invalid;
        }
        else if (check && stack_slots != p->fp + f->param_slots + f->code->local_slots + operand_slots)
            result = wm_snapshot_invalid;
        if (!result && refs)
        {
            uint32_t slot = mark_refs(at.locals, at.local_count, p->fp, end, refs);
            if (slot == WM_NONE || mark_refs(operands, operand_count, slot, end, refs) == WM_NONE)
                result = wm_snapshot_invalid;
        }
    }
    wm_free(at.types), wm_free(at.locals), wm_free(before.types), wm_free(before.locals);
    return result;
}

/* A position read back, checked roughly against the module and the stack; walk checks the rest. */
static WmResult get_place(Stream* s, const Group* g, Place* out, uint32_t stack_slots)
{
    const WmModule* m;
    uint32_t index = get32(s);
    out->offset = get32(s);
    out->side = get32(s);
    out->fp = get32(s);
    out->func = group_code_at(g, index);
    if (s->error)
        return s->error;
    if (!out->func)
        return wm_snapshot_invalid;
    m = out->func->instance->module;
    if (out->offset >= m->size || out->func->code->body > m->bytes + out->offset ||
        out->func->code->end < m->bytes + out->offset || out->side > m->side_count || out->fp > stack_slots)
        return wm_snapshot_invalid;
    return NULL;
}

static void set_place(WmRuntime* rt, const Place* p, WmFunc** func, const uint8_t** pc, const WmSide** st,
                      uint32_t** fp)
{
    const WmModule* m = p->func->instance->module;
    *func = p->func;
    *fp = rt->stack + p->fp;
    *pc = m->bytes + p->offset;
    /* A predecoded module keeps only the count of its side table: to_record makes the position a record. */
    *st = m->side ? m->side + p->side : NULL;
}

#if WM_PREDECODE
/*
 * A position in a predecoded function, read as its instruction: now its
 * record. The records are made here, once every frame is on the runtime,
 * so that making some cannot drop others the snapshot stopped in.
 */
static WmResult to_record(WmFunc* func, const uint8_t** pc, const WmSide** st, uint8_t* inplace, int makes_code)
{
    const WmModule* m = func->instance->module;
    WmResult result = NULL;
    if (!WM_RUNS_FAST(m))
        return NULL;
    if (func->code->records || makes_code)
        result = wm_code_load(func);
    if (!(func->code->records || makes_code) || (result && !strncmp(result, "exhaustion", 10)))
    {
        /* Without records (decision 21): the function runs in place by itself, here too. */
        WmTail* tail;
        if ((result = wm_reg_side((WmModule*)(uintptr_t)m, func->code, &tail)))
            return result;
        *st = tail->side + wm_reg_sides_before(m, func->code, (uint32_t)(*pc - m->bytes));
        if (inplace)
            *inplace = 2;
        return NULL;
    }
    if (result)
        return result;
#if WM_PREDECODE
    {
        /* A segment's start is its SEG; anywhere else (the top frame only) the loop in place goes on. */
        WmTail* tail;
        const WmTailEntry* e;
        uint32_t offset = (uint32_t)(*pc - m->bytes);
        if ((result = wm_reg_tail((WmModule*)(uintptr_t)m, func->code, &tail)))
            return result;
        e = wm_reg_entry(tail, offset);
        if (e && !e->charge)
        {
            *pc = (const uint8_t*)(const void*)(func->code->records + e->cell);
            *st = NULL;
            return NULL;
        }
        if (!inplace)
            return wm_snapshot_invalid;
        *st = tail->side + wm_reg_sides_before(m, func->code, offset);
        *inplace = 1;
        return NULL;
    }
#endif
}
#endif

/* funcref: the function's number plus one, 0 for null; externref as it is. WM_NONE: not the group's. */
static uint64_t put_ref_index(const Group* g, uint8_t type, uintptr_t ref)
{
    uint32_t index;
    if (type != WM_FUNCREF || ref == 0)
        return (uint64_t)ref;
    index = group_index(g, (const WmFunc*)ref);
    return index == WM_NONE ? WM_NONE : (uint64_t)index + 1u;
}

static WmResult get_ref_index(const Group* g, uint8_t type, uint64_t value, uintptr_t* out)
{
    WmFunc* f;
    if (type != WM_FUNCREF || value == 0)
    {
        *out = (uintptr_t)value;
        return NULL;
    }
    if (value > 0xFFFFFFFFu || !(f = group_func(g, (uint32_t)(value - 1u))))
        return wm_snapshot_invalid;
    *out = (uintptr_t)f;
    return NULL;
}

/* The memory section of a state snapshot: the memory stays where it is. */
#define KEPT_MEMORY 0xFFFFFFFFu

static WmResult snapshot_write(const WmRuntime* rt, const WmInstance* inst, WmSnapshotWrite write, void* context,
                               int with_memory)
{
    const WmSuspended* saved = &rt->saved;
    uint32_t stack_slots;
    Stream s = {write, NULL, context, NULL, 0xFFFFFFFFu};
    Group g;
    Place* places;
    uint8_t* refs = NULL;
    WmResult result = NULL;

    memset(&g, 0, sizeof(g));
    if ((result = group_make(&g, (WmInstance*)(uintptr_t)inst)))
        return result;
    if (!rt->suspended || saved->entry_depth != 0 || group_index(&g, saved->entry) == WM_NONE)
    {
        group_free(&g);
        return WM_ERR_UNSUPPORTED("unsupported: snapshots are of a suspended call of an instance and those linked to it");
    }
    stack_slots = (uint32_t)(saved->sp - rt->stack);
#if WM_PREDECODE
    /* Stopped at a segment of the register loop: where its stack's top is, the function's tail says. */
    if (!saved->inplace && saved->pc && WM_RUNS_FAST(saved->func->instance->module))
    {
        const WmTailEntry* e = wm_reg_segment(saved->func, (const WmCell*)(const void*)saved->pc);
        if (e)
            stack_slots = (uint32_t)(saved->fp + e->top - rt->stack);
    }
#endif
    places = (Place*)wm_malloc(((size_t)rt->depth + 1u) * sizeof(Place));
    if (!places)
        result = WM_ERR_EXHAUSTION("exhaustion: out of memory");
    for (uint32_t i = 0; !result && i <= rt->depth; ++i)
    {
        const WmFrame* f = &rt->frames[i];
        WmFunc* func = i == rt->depth ? saved->func : f->func;
        if (group_index(&g, func) == WM_NONE)
            result = WM_ERR_UNSUPPORTED("unsupported: a call of an instance not linked to it is in progress");
        else if (i == rt->depth)
            result = place_of(rt, saved->func, saved->pc, saved->st, saved->fp, saved->inplace, &places[i]);
        else
            result = place_of(rt, f->func, f->pc, f->st, f->fp, 0, &places[i]);
    }
    /* Function references on the stack become numbers: where they are, the code says. */
    if (!result && g.stack_refs)
    {
        if (!(refs = (uint8_t*)wm_calloc(stack_slots + 1u, 1)))
            result = WM_ERR_EXHAUSTION("exhaustion: out of memory");
        else
            result = walk(places, rt->depth + 1u, (uint32_t)(saved->base - rt->stack), stack_slots, 0, refs);
    }
    if (result)
    {
        wm_free(places);
        wm_free(refs);
        group_free(&g);
        return result;
    }

    put32(&s, WM_SNAPSHOT_MAGIC);
    put32(&s, WM_SNAPSHOT_VERSION);
    /* The stack's layout depends on the width of references only where they can be on it. */
    put32(&s, g.stack_refs ? WM_REF_SLOTS : 0u);
    put32(&s, g.count);
    for (uint32_t i = 0; i < g.count; ++i)
        put32(&s, module_hash(g.insts[i]->module));
    put32(&s, g.memory_count);
    for (uint32_t i = 0; i < g.memory_count; ++i)
    {
        put32(&s, g.memories[i]->page_log2);
        put32(&s, g.memories[i]->pages);
    }
    put32(&s, stack_slots);
    put32(&s, rt->depth);

    put32(&s, group_index(&g, saved->entry));
    put32(&s, (uint32_t)(saved->base - rt->stack));
    put_place(&s, &g, &places[rt->depth]);
    for (uint32_t i = 0; i < rt->depth; ++i)
        put_place(&s, &g, &places[i]);
    wm_free(places);
    for (uint32_t i = 0; i < stack_slots; ++i)
    {
        if (refs && refs[i])
        {
            uintptr_t ref = 0;
            uint64_t index;
            memcpy(&ref, &rt->stack[i], sizeof(ref) < WM_REF_SLOTS * 4u ? sizeof(ref) : WM_REF_SLOTS * 4u);
            index = put_ref_index(&g, WM_FUNCREF, ref);
            if (index == WM_NONE)
                s.error = WM_ERR_UNSUPPORTED("unsupported: a reference to a function of no linked instance on the stack");
            put32(&s, (uint32_t)index);
            for (uint32_t k = 1; k < WM_REF_SLOTS; ++k)
                put32(&s, 0);
            i += WM_REF_SLOTS - 1u;
            continue;
        }
        put32(&s, rt->stack[i]);
    }
    wm_free(refs);

    for (uint32_t i = 0; i < g.memory_count; ++i)
    {
        WmMemory* memory = g.memories[i];
        if (!with_memory && WM_MEMORY_SIZE(memory))
        {
            put32(&s, KEPT_MEMORY);
            put32(&s, KEPT_MEMORY);
            continue;
        }
#if WM_HAS_MMU
#define MEMORY_BYTE(AT) (data ? data[AT] : wm_memory_byte(memory, (uint32_t)(AT)))
#else
#define MEMORY_BYTE(AT) (data[AT])
#endif
        {
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
        }
#undef MEMORY_BYTE
    }

    for (uint32_t i = 0; i < g.global_count; ++i)
    {
        const WmGlobal* gl = g.globals[i];
        uint64_t value = gl->value;
        if (gl->type == WM_FUNCREF || gl->type == WM_EXTERNREF)
            value = put_ref_index(&g, gl->type, (uintptr_t)gl->value);
        if (value == WM_NONE && gl->type == WM_FUNCREF)
            s.error = WM_ERR_UNSUPPORTED("unsupported: a reference to a function of no linked instance in a global");
        put64(&s, value);
    }
    for (uint32_t i = 0; i < g.table_count; ++i)
    {
        const WmTable* t = g.tables[i];
        put32(&s, t->size);
        for (uint32_t k = 0; k < t->size; ++k)
        {
            uint64_t value = put_ref_index(&g, t->type, t->elems[k]);
            if (value == WM_NONE && t->type == WM_FUNCREF)
                s.error = WM_ERR_UNSUPPORTED("unsupported: a reference to a function of no linked instance in a table");
            put64(&s, value);
        }
    }
    for (uint32_t i = 0; i < g.count; ++i)
    {
        put(&s, g.insts[i]->elem_dropped, g.insts[i]->module->elem_count);
        put(&s, g.insts[i]->data_dropped, g.insts[i]->module->data_count);
    }
    put32(&s, ~s.crc);
    group_free(&g);
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

/*
 * The header, the frames and the stack, checked before anything of the
 * group changes; then the rest, read straight into it.
 */
static WmResult snapshot_read(WmRuntime* rt, Group* g, Stream* s)
{
    WmSuspended* saved = &rt->saved;
    uint32_t capacity = (uint32_t)(rt->stack_end - rt->stack);
    uint32_t* pages = NULL;
    uint32_t stack_slots, depth, entry, base;
    WmResult result = NULL;
    Place* places;
    uint8_t* refs = NULL;

    if (get32(s) != WM_SNAPSHOT_MAGIC || get32(s) != WM_SNAPSHOT_VERSION || get32(s) != (g->stack_refs ? WM_REF_SLOTS : 0u) ||
        get32(s) != g->count)
        return s->error ? s->error : wm_snapshot_invalid;
    for (uint32_t i = 0; i < g->count; ++i)
        if (get32(s) != module_hash(g->insts[i]->module))
            return s->error ? s->error : wm_snapshot_invalid;
    if (get32(s) != g->memory_count)
        return s->error ? s->error : wm_snapshot_invalid;
    if (!(pages = (uint32_t*)wm_malloc(((size_t)g->memory_count + 1u) * sizeof(uint32_t))))
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    for (uint32_t i = 0; !result && i < g->memory_count; ++i)
    {
        const WmMemory* memory = g->memories[i];
        uint32_t page_log2 = get32(s);
        pages[i] = get32(s);
        if (!s->error && (memory->page_log2 != page_log2 || pages[i] < memory->pages ||
                          (memory->max_pages != WM_NONE && pages[i] > memory->max_pages)))
            result = wm_snapshot_invalid;
    }
    stack_slots = get32(s);
    depth = get32(s);
    entry = get32(s);
    base = get32(s);
    if (!result)
        result = s->error;
    if (!result && (stack_slots > capacity || depth > rt->max_frames || base > stack_slots || !group_code_at(g, entry)))
        result = wm_snapshot_invalid;
    if (result)
    {
        wm_free(pages);
        return result;
    }

    /* The frames, outermost first, then the stack: checked before anything of the instances changes. */
    places = (Place*)wm_malloc(((size_t)depth + 1u) * sizeof(Place));
    if (!places)
    {
        wm_free(pages);
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }
    result = get_place(s, g, &places[depth], stack_slots);
    for (uint32_t i = 0; !result && i < depth; ++i)
        result = get_place(s, g, &places[i], stack_slots);
    for (uint32_t i = 0; !result && i < stack_slots; ++i)
        rt->stack[i] = get32(s);
    if (!result)
        result = s->error;
    if (!result && WM_HAS_VALIDATION && places[0].func != group_code_at(g, entry))
        result = wm_snapshot_invalid;
    if (!result && g->stack_refs && !(refs = (uint8_t*)wm_calloc(stack_slots + 1u, 1)))
        result = WM_ERR_EXHAUSTION("exhaustion: out of memory");
    if (!result && (WM_HAS_VALIDATION || refs))
        result = walk(places, depth + 1u, base, stack_slots, WM_HAS_VALIDATION, refs);
    /* Function references back from their numbers. */
    for (uint32_t i = 0; !result && refs && i < stack_slots; ++i)
        if (refs[i])
        {
            uintptr_t ref;
            for (uint32_t k = 1; k < WM_REF_SLOTS; ++k)
                if (rt->stack[i + k])
                    result = wm_snapshot_invalid;
            if (!result && !(result = get_ref_index(g, WM_FUNCREF, rt->stack[i], &ref)))
                memcpy(&rt->stack[i], &ref, sizeof(ref) < WM_REF_SLOTS * 4u ? sizeof(ref) : WM_REF_SLOTS * 4u);
            i += WM_REF_SLOTS - 1u;
        }
    if (!result)
    {
        saved->entry = group_code_at(g, entry);
        saved->base = rt->stack + base;
        saved->entry_depth = 0;
        saved->sp = rt->stack + stack_slots;
        set_place(rt, &places[depth], &saved->func, &saved->pc, &saved->st, &saved->fp);
        for (uint32_t i = 0; i < depth; ++i)
        {
            WmFrame* f = &rt->frames[i];
            set_place(rt, &places[i], &f->func, &f->pc, &f->st, &f->fp);
        }
    }
    wm_free(places);
    wm_free(refs);
    for (uint32_t i = 0; !result && i < g->memory_count; ++i)
    {
        WmMemory* memory = g->memories[i];
        if (pages[i] != memory->pages && wm_memory_grow(memory, pages[i] - memory->pages) == WM_NONE)
            result = WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }
    wm_free(pages);
    if (result)
        return result;

    for (uint32_t i = 0; i < g->memory_count; ++i)
    {
        WmMemory* memory = g->memories[i];
        wm_size size = WM_MEMORY_SIZE(memory);
        wm_size at = 0;
        uint32_t zeros, length;
        int kept;
        if (!size)
            continue;
        zeros = get32(s);
        length = get32(s);
        /* A state snapshot: the memory is the one it was taken with, kept where it is. */
        kept = zeros == KEPT_MEMORY && length == KEPT_MEMORY;
        if (!kept)
        {
            WM_MEMORY_CHANGES(memory);
#if WM_HAS_MMU
            if (!memory->data)
                wm_memory_fill(memory, 0, 0, (uint32_t)size);
            else
#endif
                memset(memory->data, 0, (size_t)size);
        }
        while (!kept && at < size && !s->error)
        {
            if (at)
            {
                zeros = get32(s);
                length = get32(s);
            }
            if (s->error || zeros > size - at || length > size - at - zeros)
                return s->error ? s->error : wm_snapshot_invalid;
            at += zeros;
#if WM_HAS_MMU
            if (!memory->data)
            {
                uint8_t chunk[64];
                for (uint32_t k = 0; k < length;)
                {
                    uint32_t n = length - k < sizeof(chunk) ? length - k : (uint32_t)sizeof(chunk);
                    get(s, chunk, n);
                    if (wm_memory_write(memory, (uint32_t)at + k, chunk, n))
                        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
                    k += n;
                }
            }
            else
#endif
                get(s, memory->data + at, length);
            at += length;
            if (zeros == 0 && length == 0)
                return wm_snapshot_invalid;
        }
    }

    for (uint32_t i = 0; i < g->global_count; ++i)
    {
        WmGlobal* gl = g->globals[i];
        uint64_t value = get64(s);
        if (gl->type == WM_FUNCREF || gl->type == WM_EXTERNREF)
        {
            uintptr_t ref;
            if ((result = get_ref_index(g, gl->type, value, &ref)))
                return result;
            value = (uint64_t)ref;
        }
        gl->value = value;
    }
    for (uint32_t i = 0; i < g->table_count; ++i)
    {
        WmTable* t = g->tables[i];
        uint32_t size = get32(s);
        if (s->error)
            return s->error;
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
            if ((result = get_ref_index(g, t->type, get64(s), &t->elems[k])))
                return result;
    }
    for (uint32_t i = 0; i < g->count; ++i)
    {
        get(s, g->insts[i]->elem_dropped, g->insts[i]->module->elem_count);
        get(s, g->insts[i]->data_dropped, g->insts[i]->module->data_count);
    }
    {
        uint32_t crc = ~s->crc;
        /* Fuzzing builds skip it, as is the custom, so mutated snapshots reach the checks above. */
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (get32(s) != crc && !s->error)
            return wm_snapshot_invalid;
#else
        (void)crc;
        (void)get32(s);
#endif
    }
    if (s->error)
        return s->error;
    for (uint32_t i = 0; i < g->count; ++i)
        g->insts[i]->started = 1;
    rt->depth = depth;
    rt->sp = saved->sp;
    rt->suspended = 1;
#if WM_PREDECODE
    saved->inplace = 0;
    result = to_record(saved->func, &saved->pc, &saved->st, &saved->inplace, rt->makes_code);
    for (uint32_t i = 0; !result && i < depth; ++i)
        result = to_record(rt->frames[i].func, &rt->frames[i].pc, &rt->frames[i].st, NULL, rt->makes_code);
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

WmResult wm_snapshot_read(WmRuntime* rt, WmInstance* inst, WmSnapshotRead read, void* context)
{
    Stream s = {NULL, read, context, NULL, 0xFFFFFFFFu};
    Group g;
    WmResult result;
    if (rt->suspended || rt->depth || rt->nesting)
        return WM_ERR_UNSUPPORTED("unsupported: a snapshot loads into a runtime that is not running");
    memset(&g, 0, sizeof(g));
    if ((result = group_make(&g, inst)))
        return result;
    result = snapshot_read(rt, &g, &s);
    group_free(&g);
    return result;
}
