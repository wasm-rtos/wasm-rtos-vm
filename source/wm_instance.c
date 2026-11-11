/* Instances and the objects they share: functions, globals, tables, memories. */
#include "wm_internal.h"

#include <stdlib.h>

#if WM_TERSE_ERRORS
const char wm_error_malformed[] = "malformed:";
const char wm_error_invalid[] = "invalid:";
const char wm_error_unlinkable[] = "unlinkable:";
const char wm_error_trap[] = "trap:";
const char wm_error_exhaustion[] = "exhaustion:";
const char wm_error_unsupported[] = "unsupported:";
#endif

#define E_MEMORY WM_ERR_EXHAUSTION("exhaustion: out of memory")

static uint32_t read_u32(const uint8_t** p)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    return value;
}

static int64_t read_signed(const uint8_t** p)
{
    uint64_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    if (shift < 64u && (byte & 0x40u))
        value |= ~(uint64_t)0 << shift;
    return (int64_t)value;
}

WmResult wm_eval_const(WmInstance* instance, const uint8_t* expr, uint64_t* out)
{
    const uint8_t* p = expr;
    uint64_t value = 0;
    for (;;)
    {
        uint8_t op = WM_RD8(p++);
        switch (op)
        {
            case 0x0B: *out = value; return NULL;
            case 0x41: value = (uint32_t)(int32_t)read_signed(&p); break;
            case 0x42: value = (uint64_t)read_signed(&p); break;
            case 0x43:
            {
                uint32_t bits = 0;
                for (uint32_t i = 0; i < 4u; ++i)
                    bits |= (uint32_t)WM_RD8(p++) << (8u * i);
                value = bits;
                break;
            }
            case 0x44:
                value = 0;
                for (uint32_t i = 0; i < 8u; ++i)
                    value |= (uint64_t)WM_RD8(p++) << (8u * i);
                break;
            case 0xD0:
                ++p;
                value = 0;
                break;
            case 0xD2: value = (uintptr_t)instance->funcs[read_u32(&p)]; break;
            case 0x23: value = instance->globals[read_u32(&p)]->value; break;
            default: return WM_ERR_INVALID("invalid: constant expression required");
        }
    }
}

static void set_signature(WmFunc* f, const WmType* t)
{
    f->params = t->params;
    f->param_count = t->param_count;
    f->results = t->results;
    f->result_count = t->result_count;
    f->param_slots = t->param_slots;
    f->result_slots = t->result_slots;
}

static int limits_match(uint32_t size, uint32_t max, int has_max, const WmLimits* wanted)
{
    if (size < wanted->min)
        return 0;
    if (wanted->has_max && (!has_max || max > wanted->max))
        return 0;
    return 1;
}

int wm_module_import_type(const WmModule* m, const char* name, const uint8_t** params, uint32_t* param_count,
                          const uint8_t** results, uint32_t* result_count)
{
    for (uint32_t i = 0; i < m->import_count; ++i)
    {
        const WmImport* imp = &m->imports[i];
        if (imp->kind == WM_EXTERN_FUNC && (const char*)imp->name == name)
        {
            const WmType* t = &m->types[m->func_types[imp->index]];
            *params = t->params;
            *param_count = t->param_count;
            *results = t->results;
            *result_count = t->result_count;
            return 1;
        }
    }
    return 0;
}

#define E_INCOMPATIBLE WM_ERR_UNLINKABLE("unlinkable: incompatible import type")

static WmResult link_imports(WmInstance* inst, WmResolver resolver, void* context)
{
    const WmModule* m = inst->module;
    for (uint32_t i = 0; i < m->import_count; ++i)
    {
        const WmImport* imp = &m->imports[i];
        WmExtern ext;
        WmResult result;
        memset(&ext, 0, sizeof(ext));
        result = resolver ? resolver(context, (const char*)imp->module, imp->module_length, (const char*)imp->name,
                                     imp->name_length, imp->kind, &ext)
                          : WM_ERR_UNLINKABLE("unlinkable: unknown import");
        if (result)
            return result;
        if (ext.kind != imp->kind)
            return E_INCOMPATIBLE;
        switch (imp->kind)
        {
            case WM_EXTERN_FUNC:
            {
                WmFunc* f = ext.as.func;
                const WmType* t = &m->types[m->func_types[imp->index]];
                /* Unresolved: a stub of this instance that traps until linked. */
                if (!f)
                    break;
                if (!wm_func_has_type(f, t))
                    return E_INCOMPATIBLE;
                inst->funcs[imp->index] = f;
                break;
            }
            case WM_EXTERN_TABLE:
            {
                WmTable* t = ext.as.table;
                const WmTableType* wanted = &m->tables[imp->index];
                if (!t || t->type != wanted->type ||
                    !limits_match(t->size, t->max, t->max != WM_NONE, &wanted->limits))
                    return E_INCOMPATIBLE;
                inst->tables[imp->index] = t;
                break;
            }
            case WM_EXTERN_MEMORY:
            {
                WmMemory* mem = ext.as.memory;
                if (!mem || mem->page_log2 != m->memory.page_log2 ||
                    !limits_match(mem->pages, mem->max_pages, mem->max_pages != WM_NONE, &m->memory))
                    return E_INCOMPATIBLE;
                inst->memory = mem;
                break;
            }
            case WM_EXTERN_GLOBAL:
            {
                WmGlobal* g = ext.as.global;
                const WmGlobalType* wanted = &m->globals[imp->index];
                if (!g || g->type != wanted->type || g->mutable_ != wanted->mutable_)
                    return E_INCOMPATIBLE;
                inst->globals[imp->index] = g;
                break;
            }
        }
    }
    return NULL;
}

static uintptr_t elem_item(WmInstance* inst, const WmElemSegment* s, const uint8_t** p)
{
    if (s->expressions)
    {
        uint64_t value = 0;
        const uint8_t* start = *p;
        wm_eval_const(inst, start, &value);
        /* Skip the expression: one instruction and its end. */
        while (WM_RD8((*p)++) != 0x0Bu)
        {
            uint8_t op = WM_RD8(*p - 1);
            if (op == 0xD0u)
                ++*p;
            else
                read_u32(p);
        }
        return (uintptr_t)value;
    }
    return (uintptr_t)inst->funcs[read_u32(p)];
}

/* table.init's work, also for active segments while instantiating. */
WmResult wm_table_init(WmInstance* inst, uint32_t segment, WmTable* table, uint32_t dst, uint32_t src, uint32_t count);

WmResult wm_table_init(WmInstance* inst, uint32_t segment, WmTable* table, uint32_t dst, uint32_t src, uint32_t count)
{
    const WmElemSegment* s = &inst->module->elems[segment];
    uint32_t size = inst->elem_dropped[segment] ? 0 : s->count;
    const uint8_t* p = s->items;
    if (wm_out_of_bounds(src, count, size) || wm_out_of_bounds(dst, count, table->size))
        return WM_ERR_TRAP("trap: out of bounds table access");
    for (uint32_t i = 0; i < src; ++i)
        elem_item(inst, s, &p);
    for (uint32_t i = 0; i < count; ++i)
        table->elems[dst + i] = elem_item(inst, s, &p);
    return NULL;
}

static WmResult initialize(WmInstance* inst, int kept_memory)
{
    const WmModule* m = inst->module;
    for (uint32_t i = 0; i < m->elem_count; ++i)
    {
        const WmElemSegment* s = &m->elems[i];
        if (s->mode == WM_SEGMENT_ACTIVE)
        {
            uint64_t offset;
            WmResult result;
            wm_eval_const(inst, s->offset, &offset);
            if ((result = wm_table_init(inst, i, inst->tables[s->table], (uint32_t)offset, 0, s->count)))
                return result;
        }
        if (s->mode != WM_SEGMENT_PASSIVE)
            inst->elem_dropped[i] = 1;
    }
    for (uint32_t i = 0; i < m->data_count; ++i)
    {
        const WmDataSegment* s = &m->datas[i];
        if (s->mode == WM_SEGMENT_ACTIVE)
        {
            uint64_t value;
            uint32_t offset;
            wm_eval_const(inst, s->offset, &value);
            offset = (uint32_t)value;
            if (wm_out_of_bounds(offset, s->size, WM_MEMORY_SIZE(inst->memory)))
                return WM_ERR_TRAP("trap: out of bounds memory access");
            /* A memory kept from before has its bytes already. */
            if (kept_memory)
            {
                inst->data_dropped[i] = 1;
                continue;
            }
#if WM_HAS_MMU
            if (wm_memory_write(inst->memory, offset, s->data, s->size))
                return WM_ERR_EXHAUSTION("exhaustion: out of memory");
#else
            for (uint32_t k = 0; k < s->size; ++k)
                inst->memory->data[offset + k] = WM_RD8(s->data + k);
#endif
            inst->data_dropped[i] = 1;
        }
    }
    return NULL;
}

/* An instance of m; with kept a memory from an earlier instance, which it takes as its own. */
static WmResult instantiate(WmInstance** out, const WmModule* m, WmRuntime* runtime, WmResolver resolver,
                            void* context, WmMemory* kept)
{
    WmInstance* inst;
    WmResult result;
    uint32_t own_tables = m->table_count - m->table_imports;
    uint32_t own_globals = m->global_count - m->global_imports;

    *out = NULL;
    inst = (WmInstance*)wm_calloc(1, sizeof(WmInstance));
    if (!inst)
        return E_MEMORY;
    inst->module = m;
    inst->funcs = (WmFunc**)wm_calloc(m->func_count + 1u, sizeof(WmFunc*));
    inst->own_funcs = (WmFunc*)wm_calloc(m->func_count + 1u, sizeof(WmFunc));
    inst->globals = (WmGlobal**)wm_calloc(m->global_count + 1u, sizeof(WmGlobal*));
    inst->own_globals = (WmGlobal*)wm_calloc(own_globals + 1u, sizeof(WmGlobal));
    inst->tables = (WmTable**)wm_calloc(m->table_count + 1u, sizeof(WmTable*));
    inst->own_tables = (WmTable*)wm_calloc(own_tables + 1u, sizeof(WmTable));
    inst->elem_dropped = (uint8_t*)wm_calloc(m->elem_count + 1u, 1);
    inst->data_dropped = (uint8_t*)wm_calloc(m->data_count + 1u, 1);
    if (!inst->funcs || !inst->own_funcs || !inst->globals || !inst->own_globals || !inst->tables ||
        !inst->own_tables || !inst->elem_dropped || !inst->data_dropped)
    {
        wm_instance_free(inst);
        return E_MEMORY;
    }

    /* Every function gets an object of this instance; imports use theirs only until linked. */
    for (uint32_t i = 0; i < m->func_count; ++i)
    {
        WmFunc* f = &inst->own_funcs[i];
        f->instance = inst;
        set_signature(f, &m->types[m->func_types[i]]);
        if (i >= m->func_imports)
            f->code = &m->codes[i - m->func_imports];
        inst->funcs[i] = f;
    }
    if ((result = link_imports(inst, resolver, context)))
    {
        wm_instance_free(inst);
        return result;
    }

    for (uint32_t i = m->global_imports; i < m->global_count; ++i)
    {
        WmGlobal* g = &inst->own_globals[i - m->global_imports];
        g->type = m->globals[i].type;
        g->mutable_ = m->globals[i].mutable_;
        inst->globals[i] = g;
        wm_eval_const(inst, m->globals[i].init, &g->value);
    }
    for (uint32_t i = m->table_imports; i < m->table_count; ++i)
    {
        WmTable* t = &inst->own_tables[i - m->table_imports];
        const WmTableType* type = &m->tables[i];
        t->type = type->type;
        t->size = type->limits.min;
        t->max = type->limits.has_max ? type->limits.max : WM_NONE;
        t->elems = (uintptr_t*)wm_calloc(t->size + 1u, sizeof(uintptr_t));
        inst->tables[i] = t;
        if (!t->elems)
        {
            wm_instance_free(inst);
            return E_MEMORY;
        }
    }
    if (kept)
    {
        if (m->memory_count <= m->memory_imports || kept->page_log2 != m->memory.page_log2 ||
            !limits_match(kept->pages, kept->max_pages, kept->max_pages != WM_NONE, &m->memory))
        {
            wm_instance_free(inst);
            return E_INCOMPATIBLE;
        }
        inst->memory = kept;
        inst->owns_memory = 1;
    }
    else if (m->memory_count > m->memory_imports)
    {
        inst->memory = wm_memory_new_paged(m->memory.min, m->memory.has_max ? m->memory.max : WM_NONE, m->memory.page_log2);
        if (!inst->memory)
        {
            wm_instance_free(inst);
            return E_MEMORY;
        }
        inst->owns_memory = 1;
    }

#if WM_PREDECODE
    /* Once the instance has its memory, which matters more than speed. */
    wm_module_predecode((WmModule*)m);
#endif
    /* From here on effects can be seen through shared imports, so a failure keeps the instance. */
    *out = inst;
    if ((result = initialize(inst, kept != NULL)))
        return result;
    /* Without a runtime the start function waits for wm_instance_start, after late linking. */
    if (runtime)
        return wm_instance_start(inst, runtime);
    return NULL;
}

WmResult wm_instantiate(WmInstance** out, const WmModule* m, WmRuntime* runtime, WmResolver resolver,
                        void* context)
{
    return instantiate(out, m, runtime, resolver, context, NULL);
}

WmResult wm_instantiate_with_memory(WmInstance** out, const WmModule* m, WmMemory* memory, WmResolver resolver,
                                    void* context)
{
    WmResult result = instantiate(out, m, NULL, resolver, context, memory);
    /* Not taken: it stays the caller's. */
    if (!*out || (*out)->memory != memory)
        return result ? result : E_INCOMPATIBLE;
    return result;
}

WmMemory* wm_instance_detach_memory(WmInstance* inst)
{
    WmMemory* memory = inst->owns_memory ? inst->memory : NULL;
    inst->owns_memory = 0;
    return memory;
}

WmResult wm_instance_start(WmInstance* inst, WmRuntime* runtime)
{
    if (inst->module->start == WM_NONE || inst->started)
        return NULL;
    inst->started = 1;
    return wm_call(runtime, inst->funcs[inst->module->start], NULL, NULL);
}

void wm_instance_free(WmInstance* inst)
{
    if (!inst)
        return;
    if (inst->own_tables)
        for (uint32_t i = 0; i < inst->module->table_count - inst->module->table_imports; ++i)
            wm_free(inst->own_tables[i].elems);
    if (inst->owns_memory)
        wm_memory_free(inst->memory);
    wm_free(inst->funcs);
    wm_free(inst->own_funcs);
    wm_free(inst->globals);
    wm_free(inst->own_globals);
    wm_free(inst->tables);
    wm_free(inst->own_tables);
    wm_free(inst->elem_dropped);
    wm_free(inst->data_dropped);
    wm_free(inst);
}

WmResult wm_instance_export(const WmInstance* inst, const char* name, uint32_t length, WmExtern* out)
{
    const WmModule* m = inst->module;
    for (uint32_t i = 0; i < m->export_count; ++i)
    {
        const WmExport* e = &m->exports[i];
        if (!wm_bytes_equal(e->name, e->name_length, (const uint8_t*)name, length))
            continue;
        out->kind = e->kind;
        switch (e->kind)
        {
            case WM_EXTERN_FUNC: out->as.func = inst->funcs[e->index]; break;
            case WM_EXTERN_TABLE: out->as.table = inst->tables[e->index]; break;
            case WM_EXTERN_MEMORY: out->as.memory = inst->memory; break;
            case WM_EXTERN_GLOBAL: out->as.global = inst->globals[e->index]; break;
        }
        return NULL;
    }
    return WM_ERR_UNLINKABLE("unlinkable: unknown export");
}

WmMemory* wm_instance_memory(const WmInstance* inst) { return inst->memory; }

WmFunc* wm_host_function_new(const uint8_t* params, uint32_t param_count, const uint8_t* results,
                             uint32_t result_count, WmHostFunction function, void* user)
{
    WmFunc* f = (WmFunc*)wm_calloc(1, sizeof(WmFunc) + param_count + result_count);
    uint8_t* types;
    if (!f)
        return NULL;
    types = (uint8_t*)(f + 1);
    memcpy(types, params, param_count);
    memcpy(types + param_count, results, result_count);
    f->params = types;
    f->param_count = param_count;
    f->results = types + param_count;
    f->result_count = result_count;
    f->param_slots = 0;
    for (uint32_t i = 0; i < param_count; ++i)
        f->param_slots += wm_type_slots(params[i]);
    for (uint32_t i = 0; i < result_count; ++i)
        f->result_slots += wm_type_slots(results[i]);
    f->host = function;
    f->user = user;
    return f;
}

void wm_host_function_free(WmFunc* f) { wm_free(f); }

void wm_func_type(const WmFunc* f, const uint8_t** params, uint32_t* param_count, const uint8_t** results,
                  uint32_t* result_count)
{
    *params = f->params;
    *param_count = f->param_count;
    *results = f->results;
    *result_count = f->result_count;
}

WmGlobal* wm_global_new(uint8_t type, uint8_t mutable_, uint64_t value)
{
    WmGlobal* g = (WmGlobal*)wm_calloc(1, sizeof(WmGlobal));
    if (g)
    {
        g->type = type;
        g->mutable_ = mutable_;
        g->value = value;
    }
    return g;
}

void wm_global_free(WmGlobal* g) { wm_free(g); }
uint8_t wm_global_type(const WmGlobal* g) { return g->type; }
uint64_t wm_global_get(const WmGlobal* g) { return g->value; }

WmTable* wm_table_new(uint8_t type, uint32_t size, uint32_t max)
{
    WmTable* t = (WmTable*)wm_calloc(1, sizeof(WmTable));
    if (!t)
        return NULL;
    t->type = type;
    t->size = size;
    t->max = max;
    t->elems = (uintptr_t*)wm_calloc(size + 1u, sizeof(uintptr_t));
    if (!t->elems)
    {
        wm_free(t);
        return NULL;
    }
    return t;
}

void wm_table_free(WmTable* t)
{
    if (t)
        wm_free(t->elems);
    wm_free(t);
}

#if WM_HAS_ALLOCATOR
static WmAllocator allocator;

void wm_set_allocator(WmAllocator a) { allocator = a; }

void* wm_malloc(size_t size) { return allocator ? allocator(NULL, size ? size : 1u) : malloc(size); }

void* wm_calloc(size_t count, size_t size)
{
    void* p;
    if (!allocator)
        return calloc(count, size);
    if (size && count > (size_t)-1 / size)
        return NULL;
    p = allocator(NULL, count * size ? count * size : 1u);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void* wm_realloc(void* ptr, size_t size) { return allocator ? allocator(ptr, size ? size : 1u) : realloc(ptr, size); }

void wm_free(void* ptr)
{
    if (!ptr)
        return;
    if (allocator)
        allocator(ptr, 0);
    else
        free(ptr);
}

#else
void wm_set_allocator(WmAllocator a) { (void)a; }
#endif

WmRuntime* wm_runtime_new(uint32_t stack_bytes, uint32_t max_frames)
{
    WmRuntime* rt = (WmRuntime*)wm_calloc(1, sizeof(WmRuntime));
    uint32_t slots = stack_bytes / 4u;
    if (!rt)
        return NULL;
#if WM_HAS_MMU
    rt->tlb = &wm_tlb_default;
#endif
    /* One slot below the stack: the fast loop reads the top of an empty stack there. */
    rt->stack = (uint32_t*)wm_calloc(slots + 2u, sizeof(uint32_t));
    if (rt->stack)
        ++rt->stack;
    rt->frames = (WmFrame*)wm_calloc(max_frames + 1u, sizeof(WmFrame));
    if (!rt->stack || !rt->frames)
    {
        wm_runtime_free(rt);
        return NULL;
    }
    rt->stack_end = rt->stack + slots;
    rt->sp = rt->stack;
    rt->max_frames = max_frames;
    return rt;
}

void wm_runtime_free(WmRuntime* rt)
{
    if (!rt)
        return;
    if (rt->stack)
        wm_free(rt->stack - 1);
    wm_free(rt->frames);
    wm_free(rt);
}

uint8_t* wm_runtime_stack(const WmRuntime* rt, uint32_t* bytes)
{
    if (bytes)
        *bytes = (uint32_t)((uint8_t*)rt->stack_end - (uint8_t*)rt->stack);
    return (uint8_t*)rt->stack;
}

const char wm_fuel_exhausted[] = "fuel exhausted";
const char wm_yielded[] = "yielded";
const char wm_page_wait[] = "waiting for a page";

void wm_set_fuel(WmRuntime* rt, uint32_t fuel)
{
    rt->fuel = fuel;
    rt->fuel_on = 1;
}

void wm_disable_fuel(WmRuntime* rt) { rt->fuel_on = 0; }
uint32_t wm_get_fuel(const WmRuntime* rt) { return rt->fuel; }
int wm_is_fuel_enabled(const WmRuntime* rt) { return rt->fuel_on; }
int wm_is_suspended(const WmRuntime* rt) { return rt->suspended; }
uint32_t wm_call_depth(const WmRuntime* rt) { return rt->depth; }

WmResult wm_yield(WmRuntime* rt)
{
    rt->yielding = 1;
    return NULL;
}

WmResult wm_trap(WmRuntime* rt, WmResult trap)
{
    if (!rt->host_trap)
        rt->host_trap = trap;
    return trap;
}

/* After the outermost call ends: results out, stack and frames back as they were. */
static WmResult finish(WmRuntime* rt, WmFunc* f, uint32_t* base, uint32_t depth, WmResult result, uint32_t* results)
{
    --rt->nesting;
    if (result == wm_fuel_exhausted || result == wm_yielded || result == wm_page_wait)
    {
        rt->saved.entry = f;
        rt->saved.base = base;
        rt->sp = rt->saved.sp;
        return result;
    }
    /* A trap leaves the frames of the calls it cut short. */
    rt->depth = depth;
    rt->sp = base;
    rt->suspended = 0;
    if (!result && f->result_slots && results)
        memcpy(results, base, f->result_slots * 4u);
    return result;
}

WmResult wm_call(WmRuntime* rt, WmFunc* f, const uint32_t* args, uint32_t* results)
{
    uint32_t* base = rt->sp;
    uint32_t depth = rt->depth;
    WmResult result;
    uint32_t room = f->param_slots > f->result_slots ? f->param_slots : f->result_slots;
    if ((uint32_t)(rt->stack_end - base) < room)
        return WM_ERR_EXHAUSTION("exhaustion: call stack exhausted");
    if (f->param_slots)
        memcpy(base, args, f->param_slots * 4u);
#if WM_HAS_TRACE
    rt->trap_func = NULL;
#endif
    ++rt->nesting;
    rt->yielding = 0;
    if (f->host)
    {
        rt->sp = base + room;
        rt->host_trap = NULL;
        result = f->host(rt, f->user, base, f->instance ? f->instance->memory : NULL);
        if (!result)
            result = rt->host_trap;
        rt->yielding = 0;
    }
    else if (f->code)
        result = wm_execute(rt, f, 0);
    else
        result = WM_ERR_TRAP("trap: unlinked import");
    return finish(rt, f, base, depth, result, results);
}

WmResult wm_resume(WmRuntime* rt, uint32_t* results)
{
    if (!rt->suspended)
        return WM_ERR_TRAP("trap: nothing to resume");
#if WM_HAS_TRACE
    rt->trap_func = NULL;
#endif
    ++rt->nesting;
    return finish(rt, rt->saved.entry, rt->saved.base, rt->saved.entry_depth, wm_execute(rt, NULL, 1), results);
}

#if WM_HAS_TRACE
/* An unsigned LEB128 number of at most 32 bits that ends before end. */
static int name_leb(const uint8_t** p, const uint8_t* end, uint32_t* out)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        if (*p >= end || shift > 28u)
            return 0;
        byte = WM_RD8((*p)++);
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    *out = value;
    return 1;
}
#endif

uint32_t wm_trace(const WmRuntime* rt, WmFunc** out, uint32_t max)
{
#if WM_HAS_TRACE
    uint32_t count = 1;
    if (!rt->trap_func)
        return 0;
    if (max)
        out[0] = rt->trap_func;
    for (uint32_t i = rt->trap_depth; i-- > 0; ++count)
        if (count < max)
            out[count] = rt->frames[i].func;
    return count;
#else
    (void)rt, (void)out, (void)max;
    return 0;
#endif
}

/* Custom sections are not validated: the name section is read with every bound checked. */
const char* wm_func_name(const WmFunc* f, uint32_t* length)
{
#if WM_HAS_TRACE
    const WmModule* m;
    const uint8_t* p;
    const uint8_t* end;
    uint32_t index;
    if (!f || !f->code || !f->instance || !f->instance->module->names)
        return NULL;
    m = f->instance->module;
    index = (uint32_t)(f->code - m->codes) + m->func_imports;
    p = m->bytes + m->names;
    end = p + m->names_size;
    while (p < end)
    {
        uint8_t id = WM_RD8(p++);
        uint32_t size, count;
        const uint8_t* next;
        if (!name_leb(&p, end, &size) || size > (uint32_t)(end - p))
            return NULL;
        next = p + size;
        /* Subsection 1: the function names, by index. */
        if (id == 1u)
        {
            if (!name_leb(&p, next, &count))
                return NULL;
            while (count--)
            {
                uint32_t i, n;
                if (!name_leb(&p, next, &i) || !name_leb(&p, next, &n) || n > (uint32_t)(next - p))
                    return NULL;
                if (i == index)
                {
                    *length = n;
                    return (const char*)p;
                }
                p += n;
            }
            return NULL;
        }
        p = next;
    }
#else
    (void)f, (void)length;
#endif
    return NULL;
}
