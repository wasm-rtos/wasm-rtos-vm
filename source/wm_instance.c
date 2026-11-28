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

/* A constant expression's value; 0 in *out, and an error, for one that is not (a module not validated). */
static WmResult wm_eval_const(WmInstance* instance, const uint8_t* expr, uint64_t* out)
{
    const uint8_t* p = expr;
    uint64_t value = 0;
    *out = 0;
    for (;;)
    {
        uint8_t op = WM_RD8(p++);
        switch (op)
        {
            case WM_OP_END: *out = value; return NULL;
            case WM_OP_I32_CONST: value = (uint32_t)(int32_t)read_signed(&p); break;
            case WM_OP_I64_CONST: value = (uint64_t)read_signed(&p); break;
            case WM_OP_F32_CONST:
            {
                uint32_t bits = 0;
                for (uint32_t i = 0; i < 4u; ++i)
                    bits |= (uint32_t)WM_RD8(p++) << (8u * i);
                value = bits;
                break;
            }
            case WM_OP_F64_CONST:
                value = 0;
                for (uint32_t i = 0; i < 8u; ++i)
                    value |= (uint64_t)WM_RD8(p++) << (8u * i);
                break;
            case WM_OP_REF_NULL:
                ++p;
                value = 0;
                break;
            case WM_OP_REF_FUNC: value = (uintptr_t)instance->funcs[read_u32(&p)]; break;
            case WM_OP_GLOBAL_GET: value = instance->globals[read_u32(&p)]->value; break;
            default: return WM_ERR_INVALID("invalid: constant expression required");
        }
    }
}

static void set_signature(WmFunc* f, const WmType* t)
{
    f->type = t;
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

/* The imports, through the resolver; a kept memory (decision 24) is the memory import's, not asked for. */
static WmResult link_imports(WmInstance* inst, WmResolver resolver, void* context, WmMemory* kept)
{
    const WmModule* m = inst->module;
    for (uint32_t i = 0; i < m->import_count; ++i)
    {
        const WmImport* imp = &m->imports[i];
        WmExtern ext;
        WmResult result = NULL;
        memset(&ext, 0, sizeof(ext));
        if (kept && imp->kind == WM_EXTERN_MEMORY)
        {
            ext.kind = WM_EXTERN_MEMORY;
            ext.as.memory = kept;
        }
        else
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
                const WmType* t = WM_TYPE(m, m->func_types[imp->index]);
                /* Unresolved: it gets a stub of this instance that traps until linked. */
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
        while (WM_RD8((*p)++) != WM_OP_END)
        {
            uint8_t op = WM_RD8(*p - 1);
            if (op == WM_OP_REF_NULL)
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
            if ((result = wm_eval_const(inst, s->offset, &offset)) ||
                (result = wm_table_init(inst, i, inst->tables[s->table], (uint32_t)offset, 0, s->count)))
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
            WmResult result;
            if ((result = wm_eval_const(inst, s->offset, &value)))
                return result;
            offset = (uint32_t)value;
            /* No memory for a data segment only in a module not validated. */
            if (!inst->memory || wm_out_of_bounds(offset, s->size, WM_MEMORY_SIZE(inst->memory)))
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

/*
 * The functions of this instance: the module's own, then a stub for each
 * function import still unlinked, which traps until something links it
 * (wm_instance_link_function). A linked import uses its target and takes no room here.
 */
static WmResult make_own_funcs(WmInstance* inst)
{
    const WmModule* m = inst->module;
    uint32_t own = m->func_count - m->func_imports;
    uint32_t count = own;
    for (uint32_t i = 0; i < m->func_imports; ++i)
        count += inst->funcs[i] == NULL;
    inst->own_funcs = (WmFunc*)wm_store_calloc(inst->store, count + 1u, sizeof(WmFunc));
    if (!inst->own_funcs)
        return E_MEMORY;
    inst->own_func_count = count;
    count = 0;
    for (uint32_t k = 0; k < m->func_count; ++k)
    {
        /* The own functions are k = 0 .. own - 1, the imports after them. */
        uint32_t i = k < own ? m->func_imports + k : k - own;
        WmFunc* f;
        if (inst->funcs[i])
            continue;
        f = &inst->own_funcs[count++];
        f->instance = inst;
        set_signature(f, WM_TYPE(m, m->func_types[i]));
        if (i >= m->func_imports)
            f->code = &m->codes[i - m->func_imports];
        inst->funcs[i] = f;
    }
    return NULL;
}

/*
 * An instance of m, in store or none; with kept a memory from an earlier
 * instance: its own, which it takes (the store keeps it, when in one), or
 * the one it imports.
 */
static WmResult instantiate(WmInstance** out, WmStore* store, WmModule* m, WmRuntime* runtime, WmResolver resolver,
                            void* context, WmMemory* kept)
{
    WmInstance* inst;
    WmResult result;
    uint32_t own_tables = m->table_count - m->table_imports;
    uint32_t own_globals = m->global_count - m->global_imports;

    *out = NULL;
    inst = (WmInstance*)wm_store_calloc(store, 1, sizeof(WmInstance));
    if (!inst)
        return E_MEMORY;
    inst->module = m;
#if WM_HAS_STORES
    ++m->instance_count;
#endif
    wm_store_link_instances(store, inst);
    inst->funcs = (WmFunc**)wm_store_calloc(store, m->func_count + 1u, sizeof(WmFunc*));
    inst->globals = (WmGlobal**)wm_store_calloc(store, m->global_count + 1u, sizeof(WmGlobal*));
    inst->own_globals = (WmGlobal*)wm_store_calloc(store, own_globals + 1u, sizeof(WmGlobal));
    inst->tables = (WmTable**)wm_store_calloc(store, m->table_count + 1u, sizeof(WmTable*));
    inst->own_tables = (WmTable*)wm_store_calloc(store, own_tables + 1u, sizeof(WmTable));
    inst->elem_dropped = (uint8_t*)wm_store_calloc(store, m->elem_count + 1u, 1);
    inst->data_dropped = (uint8_t*)wm_store_calloc(store, m->data_count + 1u, 1);
    if (!inst->funcs || !inst->globals || !inst->own_globals || !inst->tables ||
        !inst->own_tables || !inst->elem_dropped || !inst->data_dropped)
    {
        wm_instance_free(inst);
        return E_MEMORY;
    }

    if ((result = link_imports(inst, resolver, context, kept)) || (result = make_own_funcs(inst)))
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
        if ((result = wm_eval_const(inst, m->globals[i].init, &g->value)))
        {
            wm_instance_free(inst);
            return result;
        }
    }
    for (uint32_t i = m->table_imports; i < m->table_count; ++i)
    {
        WmTable* t = &inst->own_tables[i - m->table_imports];
        const WmTableType* type = &m->tables[i];
        t->type = type->type;
        t->store = store;
        t->max = type->limits.has_max ? type->limits.max : WM_NONE;
        /* Asked before its size counts in the store's. */
        if (!wm_store_may_grow(store, WM_EXTERN_TABLE, (size_t)type->limits.min * sizeof(uintptr_t)))
        {
            wm_instance_free(inst);
            return E_MEMORY;
        }
        t->size = type->limits.min;
        t->elems = (uintptr_t*)wm_store_calloc(store, t->size + 1u, sizeof(uintptr_t));
        inst->tables[i] = t;
        if (!t->elems)
        {
            wm_instance_free(inst);
            return E_MEMORY;
        }
    }
    /* A kept memory the module imports was bound with the imports, and stays the caller's. */
    if (kept && m->memory_imports)
    {
        if (inst->memory != kept)
        {
            wm_instance_free(inst);
            return E_INCOMPATIBLE;
        }
    }
    else if (kept)
    {
        if (m->memory_count <= m->memory_imports || kept->page_log2 != m->memory.page_log2 ||
            !limits_match(kept->pages, kept->max_pages, kept->max_pages != WM_NONE, &m->memory))
        {
            wm_instance_free(inst);
            return E_INCOMPATIBLE;
        }
        inst->memory = kept;
        /* In a store the memory is the store's, kept when the instance goes. */
        inst->owns_memory = !store;
    }
    else if (m->memory_count > m->memory_imports)
    {
        inst->memory = wm_memory_new_in(store, m->memory.min, m->memory.has_max ? m->memory.max : WM_NONE,
                                        m->memory.page_log2);
        if (!inst->memory)
        {
            wm_instance_free(inst);
            return E_MEMORY;
        }
        inst->owns_memory = !store;
    }

#if WM_PREDECODE
    /* Once the instance has its memory, which matters more than speed. */
    wm_module_predecode(m);
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

WmResult wm_instantiate(WmInstance** out, WmModule* m, WmRuntime* runtime, WmResolver resolver,
                        void* context)
{
    return instantiate(out, NULL, m, runtime, resolver, context, NULL);
}

WmResult wm_instance_new(WmInstance** out, WmStore* store, WmModule* m, WmRuntime* runtime, WmResolver resolver,
                         void* context)
{
    return instantiate(out, store, m, runtime, resolver, context, NULL);
}

static WmResult instantiate_with_memory(WmInstance** out, WmStore* store, WmModule* m, WmMemory* memory,
                                        WmResolver resolver, void* context)
{
    WmResult result;
    *out = NULL;
    /* A store builds only on a memory of its own. */
    if (!memory || memory->store != store)
        return E_INCOMPATIBLE;
    result = instantiate(out, store, m, NULL, resolver, context, memory);
    /* Not taken: it stays the caller's (the store's). */
    if (!*out || (*out)->memory != memory)
        return result ? result : E_INCOMPATIBLE;
    return result;
}

WmResult wm_instantiate_with_memory(WmInstance** out, WmModule* m, WmMemory* memory, WmResolver resolver,
                                    void* context)
{
    return instantiate_with_memory(out, NULL, m, memory, resolver, context);
}

WmResult wm_instance_new_with_memory(WmInstance** out, WmStore* store, WmModule* m, WmMemory* memory,
                                     WmResolver resolver, void* context)
{
    return instantiate_with_memory(out, store, m, memory, resolver, context);
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
    WmModule* module;
    if (!inst)
        return;
    module = inst->module;
    wm_store_unlink_instances(inst);
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
#if WM_HAS_STORES
    /* Last: the module's owner may free it now. */
    if (module && --module->instance_count == 0)
        wm_module_instance_gone(module);
#else
    (void)module;
#endif
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
    /* The function, its type and the type's bytes in one block. */
    WmFunc* f = (WmFunc*)wm_calloc(1, sizeof(WmFunc) + sizeof(WmType) + param_count + result_count);
    WmType* t;
    uint8_t* types;
    if (!f)
        return NULL;
    t = (WmType*)(f + 1);
    types = (uint8_t*)(t + 1);
    /* NULL types for none: memcpy may not be given NULL, even for no bytes. */
    if (param_count)
        memcpy(types, params, param_count);
    if (result_count)
        memcpy(types + param_count, results, result_count);
    t->params = types;
    t->param_count = param_count;
    t->results = types + param_count;
    t->result_count = result_count;
    for (uint32_t i = 0; i < param_count; ++i)
        t->param_slots += wm_type_slots(params[i]);
    for (uint32_t i = 0; i < result_count; ++i)
        t->result_slots += wm_type_slots(results[i]);
    set_signature(f, t);
    f->host = function;
    f->user = user;
    return f;
}

void wm_host_function_free(WmFunc* f) { wm_free(f); }

void wm_func_type(const WmFunc* f, const uint8_t** params, uint32_t* param_count, const uint8_t** results,
                  uint32_t* result_count)
{
    *params = f->type->params;
    *param_count = f->type->param_count;
    *results = f->type->results;
    *result_count = f->type->result_count;
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

WmGlobal* wm_global_new_in(WmStore* store, uint8_t type, uint8_t mutable_, uint64_t value)
{
    WmStoreGlobal* node;
    if (!store)
        return wm_global_new(type, mutable_, value);
    if (!(node = (WmStoreGlobal*)wm_store_calloc(store, 1, sizeof(WmStoreGlobal))))
        return NULL;
    wm_store_link_globals(store, node);
    node->global.type = type;
    node->global.mutable_ = mutable_;
    node->global.in_store = 1;
    node->global.value = value;
    return &node->global;
}

void wm_global_free(WmGlobal* g)
{
    WmStoreGlobal* node;
    if (!g || !g->in_store)
    {
        wm_free(g);
        return;
    }
    node = (WmStoreGlobal*)((uint8_t*)g - offsetof(WmStoreGlobal, global));
    wm_store_unlink_globals(node);
    wm_free(node);
}
uint8_t wm_global_type(const WmGlobal* g) { return g->type; }
uint64_t wm_global_get(const WmGlobal* g) { return g->value; }
void wm_global_set(WmGlobal* g, uint64_t value) { g->value = value; }

int wm_func_equal(const WmFunc* a, const WmFunc* b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    /* A copy of a host function has its function and user; of a module's, its code and instance. */
    return a->host ? a->host == b->host && a->user == b->user && !b->code
                   : a->code && a->code == b->code && a->instance == b->instance;
}

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

WmTable* wm_table_new_in(WmStore* store, uint8_t type, uint32_t size, uint32_t max)
{
    WmStoreTable* node;
    if (!store)
        return wm_table_new(type, size, max);
    if (!wm_store_may_grow(store, WM_EXTERN_TABLE, (size_t)size * sizeof(uintptr_t)) ||
        !(node = (WmStoreTable*)wm_store_calloc(store, 1, sizeof(WmStoreTable))))
        return NULL;
    node->table.type = type;
    node->table.in_store = 1;
    node->table.store = store;
    node->table.size = size;
    node->table.max = max;
    if (!(node->table.elems = (uintptr_t*)wm_store_calloc(store, size + 1u, sizeof(uintptr_t))))
    {
        wm_free(node);
        return NULL;
    }
    wm_store_link_tables(store, node);
    return &node->table;
}

void wm_table_free(WmTable* t)
{
    WmStoreTable* node;
    if (!t)
        return;
    wm_free(t->elems);
    if (!t->in_store)
    {
        wm_free(t);
        return;
    }
    node = (WmStoreTable*)((uint8_t*)t - offsetof(WmStoreTable, table));
    wm_store_unlink_tables(node);
    wm_free(node);
}

uint32_t wm_table_size(const WmTable* t) { return t->size; }

WmFunc* wm_table_get(const WmTable* t, uint32_t index)
{
    return t->type == WM_FUNCREF && index < t->size ? (WmFunc*)t->elems[index] : NULL;
}

WmResult wm_table_set(WmTable* t, uint32_t index, WmFunc* func)
{
    if (t->type != WM_FUNCREF)
        return E_INCOMPATIBLE;
    if (index >= t->size)
        return WM_ERR_TRAP("trap: out of bounds table access");
    t->elems[index] = (uintptr_t)func;
    return NULL;
}

/* An import's own stub, which nothing has bound: no code, not a host's. */
static int unlinked(const WmInstance* inst, const WmFunc* f) { return f->instance == inst && !f->code && !f->host; }

WmResult wm_instance_link_function(WmInstance* inst, uint32_t index, WmFunc* func)
{
    WmFunc* stub;
    if (index >= inst->module->func_imports || !func)
        return WM_ERR_UNLINKABLE("unlinkable: unknown function import");
    stub = inst->funcs[index];
    if (!unlinked(inst, stub))
        return WM_ERR_UNLINKABLE("unlinkable: function import already linked");
    if (func->instance && unlinked(func->instance, func))
        return WM_ERR_UNLINKABLE("unlinkable: function import not linked yet");
    if (!wm_func_has_type(func, stub->type))
        return E_INCOMPATIBLE;
    *stub = *func;
    return NULL;
}

#if WM_HAS_ALLOCATOR
static WmAllocator allocator;

void wm_set_allocator(WmAllocator a) { allocator = a; }

#if WM_STORE_BYTES
static void* raw_realloc(void* ptr, size_t size)
{
    if (allocator)
        return allocator(ptr, size ? size : 1u);
    return ptr ? realloc(ptr, size) : malloc(size);
}

static void raw_free(void* ptr)
{
    if (allocator)
        allocator(ptr, 0);
    else
        free(ptr);
}

/* Before every allocation: the store charged and the size, padded to keep the block aligned. */
typedef union WmAllocHeader
{
    struct
    {
        WmStore* store;
        size_t size;
    } info;
    long double align;
    void* pointer;
} WmAllocHeader;

WmStore* wm_charge;

static void charge(WmStore* store, size_t add, size_t take)
{
    if (!store)
        return;
    store->bytes = store->bytes + add - take;
    if (store->bytes > store->peak)
        store->peak = store->bytes;
}

void* wm_store_malloc(WmStore* store, size_t size)
{
    WmAllocHeader* h;
    if (size > (size_t)-1 - sizeof(WmAllocHeader))
        return NULL;
    if (!(h = (WmAllocHeader*)raw_realloc(NULL, sizeof(WmAllocHeader) + size)))
        return NULL;
    h->info.store = store;
    h->info.size = size;
    charge(store, size, 0);
    return h + 1;
}

void* wm_store_calloc(WmStore* store, size_t count, size_t size)
{
    void* p;
    if (size && count > (size_t)-1 / size)
        return NULL;
    if ((p = wm_store_malloc(store, count * size)))
        memset(p, 0, count * size);
    return p;
}

void* wm_malloc(size_t size) { return wm_store_malloc(wm_charge, size); }

void* wm_calloc(size_t count, size_t size) { return wm_store_calloc(wm_charge, count, size); }

/* Grown or shrunk, an allocation stays charged to the store it was made for. */
void* wm_realloc(void* ptr, size_t size)
{
    WmAllocHeader* h;
    WmStore* store;
    size_t old;
    if (!ptr)
        return wm_malloc(size);
    h = (WmAllocHeader*)ptr - 1;
    store = h->info.store;
    old = h->info.size;
    if (size > (size_t)-1 - sizeof(WmAllocHeader))
        return NULL;
    if (!(h = (WmAllocHeader*)raw_realloc(h, sizeof(WmAllocHeader) + size)))
        return NULL;
    h->info.size = size;
    charge(store, size, old);
    return h + 1;
}

void wm_free(void* ptr)
{
    WmAllocHeader* h;
    WmStore* store;
    if (!ptr)
        return;
    h = (WmAllocHeader*)ptr - 1;
    store = h->info.store;
    charge(store, 0, h->info.size);
    raw_free(h);
    /* A store freed with bytes still charged to it goes with the last of them. */
    if (store && store->freed && store->bytes == 0)
        wm_free(store);
}
#else
void* wm_malloc(size_t size) { return allocator ? allocator(NULL, size ? size : 1u) : malloc(size); }

void* wm_calloc(size_t count, size_t size)
{
    void* p;
    if (!allocator)
        return calloc(count, size);
    if (size && count > (size_t)-1 / size)
        return NULL;
    size_t bytes = count * size;
    p = allocator(NULL, bytes ? bytes : 1u);
    if (p)
        memset(p, 0, bytes);
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
#endif

#else
void wm_set_allocator(WmAllocator a) { (void)a; }
#endif

void* wm_allocate(void* ptr, size_t size)
{
    if (size)
        return wm_realloc(ptr, size);
    wm_free(ptr);
    return NULL;
}

#if WM_TRACKS_CALLERS
static WmRuntime* g_runtimes;

WmRuntime* wm_runtimes(void) { return g_runtimes; }
#endif

void wm_set_makes_code(WmRuntime* runtime, int makes)
{
#if WM_PREDECODE
    runtime->makes_code = makes != 0;
#else
    /* Everything runs in place here. */
    (void)runtime;
    (void)makes;
#endif
}

WmRuntime* wm_runtime_new(uint32_t stack_bytes, uint32_t max_frames)
{
    return wm_runtime_new_in(NULL, stack_bytes, max_frames);
}

WmRuntime* wm_runtime_new_in(WmStore* store, uint32_t stack_bytes, uint32_t max_frames)
{
    WmRuntime* rt = (WmRuntime*)wm_store_calloc(store, 1, sizeof(WmRuntime));
    uint32_t slots = stack_bytes / 4u;
    if (!rt)
        return NULL;
#if WM_HAS_MMU
    rt->tlb = &wm_tlb_default;
#endif
    /* One slot below the stack: the fast loop reads the top of an empty stack there. */
    rt->stack = (uint32_t*)wm_store_calloc(store, slots + 2u, sizeof(uint32_t));
    if (rt->stack)
        ++rt->stack;
    rt->frames = (WmFrame*)wm_store_calloc(store, max_frames + 1u, sizeof(WmFrame));
    if (!rt->stack || !rt->frames)
    {
        wm_runtime_free(rt);
        return NULL;
    }
    rt->stack_end = rt->stack + slots;
    rt->sp = rt->stack;
    rt->max_frames = max_frames;
#if WM_PREDECODE
    rt->makes_code = 1;
#endif
#if WM_TRACKS_CALLERS
    rt->next = g_runtimes;
    g_runtimes = rt;
#endif
    wm_store_link_runtimes(store, rt);
    return rt;
}

void wm_runtime_free(WmRuntime* rt)
{
    if (!rt)
        return;
    wm_store_unlink_runtimes(rt);
#if WM_TRACKS_CALLERS
    for (WmRuntime** at = &g_runtimes; *at; at = &(*at)->next)
        if (*at == rt)
        {
            *at = rt->next;
            break;
        }
#endif
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
#if WM_TRACKS_CALLERS
    /* Back in the host function that called in, if one did: its caller is the running one again. */
    if (rt->nesting && rt->nest)
    {
        rt->caller = rt->nest->caller;
        rt->nest = rt->nest->up;
    }
#endif
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
#if WM_TRACKS_CALLERS
    /* A host function calling in: its caller keeps its records while this call runs. */
    WmNest nest;
#endif
    uint32_t room = f->param_slots > f->result_slots ? f->param_slots : f->result_slots;
    if ((uint32_t)(rt->stack_end - base) < room)
        return WM_ERR_EXHAUSTION("exhaustion: call stack exhausted");
    if (f->param_slots)
    {
        if (!args)
            return WM_ERR_INVALID("invalid: arguments missing");
        memcpy(base, args, f->param_slots * 4u);
    }
#if WM_HAS_TRACE
    rt->trap_func = NULL;
#endif
#if WM_TRACKS_CALLERS
    if (rt->nesting)
    {
        nest.caller = rt->caller;
        nest.up = rt->nest;
        rt->nest = &nest;
    }
    else
        rt->caller = NULL;
#endif
    ++rt->nesting;
    rt->yielding = 0;
    /* The memory the call reaches may change: copies of it ahead of need may not hold. */
    if (f->instance)
        WM_MEMORY_CHANGES(f->instance->memory);
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
    /* Every instance with a call on the stack may run again, and change its memory. */
    WM_MEMORY_CHANGES(rt->saved.func->instance->memory);
    for (uint32_t i = 0; i < rt->depth; ++i)
        if (rt->frames[i].func)
            WM_MEMORY_CHANGES(rt->frames[i].func->instance->memory);
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
