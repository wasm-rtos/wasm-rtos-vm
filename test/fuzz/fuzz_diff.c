/*
 * The engine against itself: a module run three ways must agree on every
 * call's result or trap, on the fuel spent by calls that did not trap, and
 * at the end on its exported globals and every byte of its memory.
 *
 *   FAST     predecoded, each call in one slice of fuel
 *   IN_PLACE the loop over the module's bytes
 *   SLICED   predecoded, in small slices; at the first stop of each call a
 *            snapshot is written and read into a fresh runtime, which goes on
 *
 * A difference aborts: one of them is wrong, whichever is right.
 */
#include <stdio.h>

#include "fuzz.h"

enum
{
    FAST,
    IN_PLACE,
    SLICED,
    WAYS
};

#define FUZZ_CALLS 32u

typedef struct Call
{
    WmResult r;
    uint32_t used;
    uint32_t result_slots;
    uint32_t results[FUZZ_RESULT_SLOTS];
} Call;

typedef struct Outcome
{
    WmResult instantiated;
    Call calls[FUZZ_CALLS + 1u];
    uint32_t call_count;
    uint64_t memory;
    uint64_t globals;
} Outcome;

static Outcome g_outcomes[WAYS];

typedef struct Buffer
{
    uint8_t* data;
    uint32_t size;
    uint32_t at;
} Buffer;

static WmResult buffer_write(void* context, const void* data, uint32_t size)
{
    Buffer* b = (Buffer*)context;
    uint8_t* grown = (uint8_t*)realloc(b->data, b->size + size);
    if (!grown)
        return "fuzz: no memory for the snapshot";
    memcpy(grown + b->size, data, size);
    b->data = grown;
    b->size += size;
    return NULL;
}

static WmResult buffer_read(void* context, void* data, uint32_t size)
{
    Buffer* b = (Buffer*)context;
    if (size > b->size - b->at)
        return "fuzz: the snapshot ends early";
    memcpy(data, b->data + b->at, size);
    b->at += size;
    return NULL;
}

static uint64_t hash(uint64_t h, const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t memory_hash(WmInstance* inst)
{
    WmMemory* mem = wm_instance_memory(inst);
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t size;
    const uint8_t* data;
    if (!mem)
        return h;
    if ((data = wm_memory_data(mem, &size)))
        return hash(h, data, size);
    /* Paged: through the memory's own reads. */
    {
        uint64_t total = (uint64_t)mem->pages << mem->page_log2;
        uint8_t chunk[4096];
        for (uint64_t at = 0; at < total; at += sizeof(chunk))
        {
            uint32_t n = total - at < sizeof(chunk) ? (uint32_t)(total - at) : (uint32_t)sizeof(chunk);
            if (wm_memory_read(mem, (uint32_t)at, chunk, n))
                return 0;
            h = hash(h, chunk, n);
        }
    }
    return h;
}

static uint64_t globals_hash(const WmInstance* inst)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t e = 0; e < inst->module->export_count; ++e)
    {
        const WmExport* x = &inst->module->exports[e];
        WmExtern ext;
        /* References are pointers into each run's own instance: not compared. */
        if (x->kind == WM_EXTERN_GLOBAL && !wm_instance_export(inst, (const char*)x->name, x->name_length, &ext) &&
            wm_global_type(ext.as.global) != WM_FUNCREF && wm_global_type(ext.as.global) != WM_EXTERNREF)
        {
            uint64_t v = wm_global_get(ext.as.global);
            h = hash(h, &v, sizeof(v));
        }
    }
    return h;
}

/* One call with FUZZ_FUEL in slices of slice (0: one slice), a snapshot at its first stop if asked. */
static void call(WmRuntime** rt, WmInstance* inst, WmFunc* f, uint32_t slice, int snapshot, Call* out)
{
    uint32_t args[FUZZ_RESULT_SLOTS] = {0};
    uint32_t left = FUZZ_FUEL;
    uint32_t give = slice && slice < left ? slice : left;
    WmResult r;
    left -= give;
    wm_set_fuel(*rt, give);
    r = wm_call(*rt, f, args, out->results);
    while (r == wm_fuel_exhausted && left)
    {
        if (snapshot)
        {
            Buffer b = {NULL, 0, 0};
            WmRuntime* fresh;
            snapshot = 0;
            if (!wm_snapshot_write(*rt, inst, buffer_write, &b) && (fresh = wm_runtime_new(64u * 1024u, 1000u)))
            {
                WmResult read = wm_snapshot_read(fresh, inst, buffer_read, &b);
                if (read)
                {
                    fprintf(stderr, "fuzz_diff: a snapshot just written does not read back: %s\n", read);
                    abort();
                }
                wm_runtime_free(*rt);
                *rt = fresh;
            }
            free(b.data);
        }
        give = slice < left ? slice : left;
        left -= give;
        wm_set_fuel(*rt, give);
        r = wm_resume(*rt, out->results);
    }
    out->r = r;
    out->used = FUZZ_FUEL - left - wm_get_fuel(*rt);
    out->result_slots = f->result_slots;
    /* References are pointers into each run's own instance: not compared. */
    for (uint32_t i = 0; i < f->type->result_count; ++i)
        if (WM_RD8(f->type->results + i) == WM_FUNCREF || WM_RD8(f->type->results + i) == WM_EXTERNREF)
            out->result_slots = 0;
}

static void run(const uint8_t* data, size_t size, int way, uint32_t slice, Outcome* o)
{
    FuzzImports imports;
    WmInstance* inst;
    WmRuntime* rt;
    memset(o, 0, sizeof(*o));
    if ((o->instantiated = fuzz_instantiate(data, size, way == IN_PLACE, &imports, &inst)))
        return;
    if ((rt = wm_runtime_new(64u * 1024u, 1000u)))
    {
        Call* start = &o->calls[o->call_count++];
        wm_set_fuel(rt, FUZZ_FUEL);
        start->r = wm_instance_start(inst, rt);
        start->used = FUZZ_FUEL - wm_get_fuel(rt);
        for (uint32_t e = 0; !start->r && e < inst->module->export_count && o->call_count <= FUZZ_CALLS; ++e)
        {
            WmFunc* f = fuzz_export_func(inst, e);
            if (!f || f->param_slots > FUZZ_RESULT_SLOTS || f->result_slots > FUZZ_RESULT_SLOTS)
                continue;
            wm_runtime_free(rt);
            if (!(rt = wm_runtime_new(64u * 1024u, 1000u)))
                break;
            call(&rt, inst, f, way == SLICED ? slice : 0, way == SLICED, &o->calls[o->call_count++]);
        }
        if (rt)
            wm_runtime_free(rt);
    }
    o->memory = memory_hash(inst);
    o->globals = globals_hash(inst);
    fuzz_free(inst, &imports);
}

static int same_text(WmResult a, WmResult b) { return a == b || (a && b && !strcmp(a, b)); }

static void differ(int way, const char* what, uint32_t at)
{
    static const char* const names[] = {"predecoded", "in place", "in slices with a snapshot"};
    fprintf(stderr, "fuzz_diff: %s and %s differ: %s (call %u)\n", names[FAST], names[way], what, at);
    for (int w = 0; w < WAYS; ++w)
    {
        const Outcome* o = &g_outcomes[w];
        const Call* c = at < o->call_count ? &o->calls[at] : NULL;
        fprintf(stderr, "  %-26s %s; fuel %u; %s\n", names[w], c && c->r ? c->r : "ended",
                c ? c->used : 0u, o->instantiated ? o->instantiated : "instantiated");
    }
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Slices of 7 to 103 instructions, chosen by the input. */
    uint32_t slice = 7u + (size ? data[size / 2u] % 97u : 0u);
    fuzz_init();
    for (int way = 0; way < WAYS; ++way)
        run(data, size, way, slice, &g_outcomes[way]);
    for (int way = 1; way < WAYS; ++way)
    {
        const Outcome* a = &g_outcomes[FAST];
        const Outcome* b = &g_outcomes[way];
        if (!same_text(a->instantiated, b->instantiated))
            differ(way, "instantiation", 0);
        if (a->call_count != b->call_count)
            differ(way, "number of calls", 0);
        for (uint32_t c = 0; c < a->call_count; ++c)
        {
            const Call* x = &a->calls[c];
            const Call* y = &b->calls[c];
            if (!same_text(x->r, y->r))
                differ(way, "result or trap", c);
            /* After a trap the fuel left is not defined (wm.h). */
            if ((!x->r || x->r == wm_fuel_exhausted) && x->used != y->used)
                differ(way, "fuel spent", c);
            if (!x->r && memcmp(x->results, y->results, x->result_slots * sizeof(uint32_t)))
                differ(way, "results", c);
        }
        if (a->globals != b->globals)
            differ(way, "exported globals", 0);
        if (a->memory != b->memory)
            differ(way, "memory", 0);
    }
    return 0;
}
