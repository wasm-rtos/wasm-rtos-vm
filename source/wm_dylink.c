/*
 * dylink.0. The program and its libraries are instances in one runtime: the
 * program's memory and table (or ones made here, when it imports them) are
 * every library's, and the globals the convention names (__memory_base,
 * __stack_pointer, GOT.mem, GOT.func...) are made here and given through
 * the imports. A function import of another module's code becomes a copy of
 * that function, so calls go straight into it: fuel and stops work across
 * modules as within one.
 */
#include "wm_dylink.h"
#include "wm_internal.h"

#include <string.h>

#define E_MALFORMED WM_ERR_MALFORMED("malformed: dylink.0 section")
#define E_MISSING WM_ERR_UNLINKABLE("unlinkable: dylink.0 dependency is missing")
#define E_DUPLICATE WM_ERR_UNLINKABLE("unlinkable: duplicate dylink.0 symbol")
#define E_UNRESOLVED WM_ERR_UNLINKABLE("unlinkable: unresolved dylink.0 symbol")
#define E_TYPE WM_ERR_UNLINKABLE("unlinkable: dylink.0 symbol type mismatch")
#define E_UNSUPPORTED WM_ERR_UNSUPPORTED("unsupported: dylink.0 feature")
#define E_MEMORY WM_ERR_EXHAUSTION("exhaustion: out of memory")
#define E_MEMORY_SIZE WM_ERR_EXHAUSTION("exhaustion: dylink.0 memory too large")

enum
{
    MEM_INFO = 1,
    NEEDED = 2,
    EXPORT_INFO = 3,
    IMPORT_INFO = 4
};

enum
{
    BINDING_WEAK = 1,
    BINDING_LOCAL = 2,
    VISIBILITY_HIDDEN = 4,
    TLS = 0x100
};

#define DEFAULT_STACK_SIZE (64u * 1024u)

typedef struct Info
{
    char* module;
    char* name;
    uint32_t flags;
} Info;

typedef struct Metadata
{
    uint32_t memory_size;
    uint32_t memory_align;
    uint32_t table_size;
    uint32_t table_align;
    char** needed;
    uint32_t needed_count;
    Info* exports;
    uint32_t export_count;
    Info* imports;
    uint32_t import_count;
} Metadata;

/* A module of the program: where its data and table entries go. */
typedef struct Object
{
    char* name;
    const WmModule* module;
    WmInstance* instance;
    Metadata meta;
    uint32_t memory_base;
    uint32_t table_base;
    uint8_t is_main;
    uint8_t visit;
} Object;

/* A GOT entry, filled once every module is in: a function's table index or a data address. */
typedef struct Got
{
    Object* object;
    WmGlobal* global;
    const uint8_t* name;
    uint32_t name_length;
    uint8_t func;
} Got;

struct WmDylink
{
    WmRuntime* runtime;
    const WmDylinkOptions* options;
    Object* objects;
    uint32_t count;
    uint32_t capacity;
    WmMemory* memory;
    uint8_t owns_memory;
    WmTable* table;
    uint8_t owns_table;
    /* Every global made here, freed with the process. */
    WmGlobal** globals;
    uint32_t global_count;
    uint32_t global_capacity;
    Got* got;
    uint32_t got_count;
    uint32_t got_capacity;
    WmGlobal* stack_pointer;
    WmGlobal* stack_low;
    WmGlobal* stack_high;
    uint32_t stack_low_value;
    uint32_t stack_high_value;
    uint32_t heap_base;
    uint32_t heap_end;
    /* The module being instantiated, for the resolver. */
    Object* current;
};

typedef WmDylink State;

/* ---- The dylink.0 section ---- */

typedef struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
} Reader;

static int read_u32(Reader* r, uint32_t* out)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        if (r->p >= r->end || shift > 28u)
            return 0;
        byte = WM_RD8(r->p++);
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    *out = value;
    return 1;
}

static char* copy_name(const uint8_t* name, uint32_t length)
{
    char* copy = (char*)wm_malloc((size_t)length + 1u);
    if (!copy)
        return NULL;
    for (uint32_t i = 0; i < length; ++i)
        copy[i] = (char)WM_RD8(name + i);
    copy[length] = '\0';
    return copy;
}

static int read_string(Reader* r, char** out)
{
    uint32_t length;
    if (!read_u32(r, &length) || length > (uint32_t)(r->end - r->p))
        return 0;
    *out = copy_name(r->p, length);
    r->p += length;
    return *out != NULL;
}

static void free_infos(Info* infos, uint32_t count)
{
    for (uint32_t i = 0; infos && i < count; ++i)
    {
        wm_free(infos[i].module);
        wm_free(infos[i].name);
    }
    wm_free(infos);
}

static void free_metadata(Metadata* m)
{
    for (uint32_t i = 0; m->needed && i < m->needed_count; ++i)
        wm_free(m->needed[i]);
    wm_free(m->needed);
    free_infos(m->exports, m->export_count);
    free_infos(m->imports, m->import_count);
    memset(m, 0, sizeof(*m));
}

static int read_infos(Reader* r, int has_module, Info** out, uint32_t* count)
{
    if (!read_u32(r, count) || *count > (uint32_t)(r->end - r->p))
        return 0;
    *out = (Info*)wm_calloc(*count + 1u, sizeof(Info));
    if (!*out)
        return 0;
    for (uint32_t i = 0; i < *count; ++i)
        if ((has_module && !read_string(r, &(*out)[i].module)) || !read_string(r, &(*out)[i].name) ||
            !read_u32(r, &(*out)[i].flags))
            return 0;
    return 1;
}

static WmResult parse_section(Metadata* m, Reader* r)
{
    uint32_t seen = 0;
    while (r->p < r->end)
    {
        uint32_t type, size;
        Reader sub;
        int ok = 1;
        if (!read_u32(r, &type) || !read_u32(r, &size) || size > (uint32_t)(r->end - r->p) ||
            (type < 32u && (seen & (1u << type))))
            return E_MALFORMED;
        if (type < 32u)
            seen |= 1u << type;
        sub.p = r->p;
        sub.end = r->p + size;
        r->p = sub.end;
        switch (type)
        {
            case MEM_INFO:
                ok = read_u32(&sub, &m->memory_size) && read_u32(&sub, &m->memory_align) &&
                     read_u32(&sub, &m->table_size) && read_u32(&sub, &m->table_align);
                break;
            case NEEDED:
                ok = read_u32(&sub, &m->needed_count) && m->needed_count <= (uint32_t)(sub.end - sub.p) &&
                     (m->needed = (char**)wm_calloc(m->needed_count + 1u, sizeof(char*))) != NULL;
                for (uint32_t i = 0; ok && i < m->needed_count; ++i)
                    ok = read_string(&sub, &m->needed[i]);
                break;
            case EXPORT_INFO: ok = read_infos(&sub, 0, &m->exports, &m->export_count); break;
            case IMPORT_INFO: ok = read_infos(&sub, 1, &m->imports, &m->import_count); break;
            /* Runtime paths and the rest mean nothing here. */
            default: sub.p = sub.end;
        }
        if (!ok || sub.p != sub.end)
            return E_MALFORMED;
    }
    return seen & (1u << MEM_INFO) ? NULL : E_MALFORMED;
}

/* The dylink.0 custom section's contents (it comes first, but any place is accepted). */
static int find_section(const WmModule* module, Reader* out)
{
    Reader r;
    r.p = module->bytes + 8;
    r.end = module->bytes + module->size;
    while (r.p < r.end)
    {
        uint8_t id = WM_RD8(r.p++);
        uint32_t size, length;
        Reader section;
        if (!read_u32(&r, &size) || size > (uint32_t)(r.end - r.p))
            return 0;
        section.p = r.p;
        section.end = r.p + size;
        r.p = section.end;
        if (id != 0u || !read_u32(&section, &length) || length > (uint32_t)(section.end - section.p))
            continue;
        if (wm_bytes_equal(section.p, length, (const uint8_t*)"dylink.0", 8u))
        {
            out->p = section.p + length;
            out->end = section.end;
            return 1;
        }
    }
    return 0;
}

int wm_module_is_dylink(const WmModule* module)
{
    Reader r;
    return module && find_section(module, &r);
}

/* ---- Symbols ---- */

static int is(const uint8_t* name, uint32_t length, const char* text)
{
    return wm_bytes_equal(name, length, (const uint8_t*)text, (uint32_t)strlen(text));
}

static uint32_t info_flags(const Info* infos, uint32_t count, const uint8_t* module, uint32_t module_length,
                           const uint8_t* name, uint32_t name_length)
{
    for (uint32_t i = 0; i < count; ++i)
        if ((!module || (infos[i].module && is(module, module_length, infos[i].module))) &&
            is(name, name_length, infos[i].name))
            return infos[i].flags;
    return 0;
}

static uint32_t import_flags(const Object* o, const WmImport* imp)
{
    return info_flags(o->meta.imports, o->meta.import_count, imp->module, imp->module_length, imp->name,
                      imp->name_length);
}

/*
 * The definition of a symbol among the modules in the process: a strong one
 * wins over weak ones, two strong ones are an error. Local and hidden
 * exports are not seen from other modules.
 */
static WmResult find_symbol(State* s, uint8_t kind, const uint8_t* name, uint32_t length, WmExtern* found,
                            Object** owner)
{
    int found_weak = 0;
    found->kind = kind;
    found->as.func = NULL;
    *owner = NULL;
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        Object* o = &s->objects[oi];
        const WmModule* m = o->module;
        WmInstance* inst = o->instance;
        uint32_t flags;
        if (!inst)
            continue;
        flags = info_flags(o->meta.exports, o->meta.export_count, NULL, 0, name, length);
        if (flags & (BINDING_LOCAL | VISIBILITY_HIDDEN))
            continue;
        for (uint32_t e = 0; e < m->export_count; ++e)
        {
            const WmExport* x = &m->exports[e];
            void* candidate;
            int weak = (flags & BINDING_WEAK) != 0;
            if (x->kind != kind || !wm_bytes_equal(x->name, x->name_length, name, length))
                continue;
            candidate = kind == WM_EXTERN_FUNC ? (void*)inst->funcs[x->index] : (void*)inst->globals[x->index];
            if (found->as.func && !found_weak && !weak && (void*)found->as.func != candidate)
                return E_DUPLICATE;
            if (!found->as.func || (found_weak && !weak))
            {
                found->as.func = (WmFunc*)candidate;
                *owner = o;
                found_weak = weak;
            }
        }
    }
    return NULL;
}

static WmGlobal* new_global(State* s, uint8_t type, uint8_t mutable_, uint64_t value)
{
    WmGlobal* g;
    if (s->global_count == s->global_capacity)
    {
        uint32_t capacity = s->global_capacity ? s->global_capacity * 2u : 16u;
        WmGlobal** globals = (WmGlobal**)wm_realloc(s->globals, capacity * sizeof(WmGlobal*));
        if (!globals)
            return NULL;
        s->globals = globals;
        s->global_capacity = capacity;
    }
    g = wm_global_new(type, mutable_, value);
    if (g)
        s->globals[s->global_count++] = g;
    return g;
}

static const WmImport* find_import(const WmModule* m, uint8_t kind, const uint8_t* module, uint32_t module_length,
                                   const uint8_t* name, uint32_t name_length)
{
    for (uint32_t i = 0; i < m->import_count; ++i)
    {
        const WmImport* imp = &m->imports[i];
        if (imp->kind == kind && imp->module_length == module_length && imp->name_length == name_length &&
            wm_bytes_equal(imp->module, module_length, module, module_length) &&
            wm_bytes_equal(imp->name, name_length, name, name_length))
            return imp;
    }
    return NULL;
}

/* A global shared by the modules that import it, made like the first import. */
static WmResult shared_global(State* s, WmGlobal** shared, const WmGlobalType* t, uint32_t value, WmGlobal** out)
{
    if (!*shared)
        *shared = new_global(s, t->type, t->mutable_, value);
    if (!*shared)
        return E_MEMORY;
    if ((*shared)->type != t->type || (*shared)->mutable_ != t->mutable_)
        return E_TYPE;
    *out = *shared;
    return NULL;
}

static WmResult global_import(State* s, const WmImport* imp, WmGlobal** out)
{
    Object* o = s->current;
    const WmGlobalType* t = &o->module->globals[imp->index];
    const uint8_t* name = imp->name;
    uint32_t n = imp->name_length;
    int env = is(imp->module, imp->module_length, "env");
    int got = is(imp->module, imp->module_length, "GOT.func") || is(imp->module, imp->module_length, "GOT.mem");
    static const char* const special[] = {"__memory_base", "__table_base", "__stack_pointer", "__stack_low",
                                          "__stack_high",  "__heap_base",  "__heap_end"};
    int is_special = got;
    for (size_t i = 0; env && i < sizeof(special) / sizeof(special[0]); ++i)
        is_special |= is(name, n, special[i]);
    if (is_special && t->type != WM_I32)
        return E_TYPE;
    *out = NULL;
    if (env && is(name, n, "__memory_base"))
        *out = new_global(s, t->type, t->mutable_, o->memory_base);
    else if (env && is(name, n, "__table_base"))
        *out = new_global(s, t->type, t->mutable_, o->table_base);
    else if (env && is(name, n, "__heap_base"))
        *out = new_global(s, t->type, t->mutable_, s->heap_base);
    else if (env && is(name, n, "__heap_end"))
        *out = new_global(s, t->type, t->mutable_, s->heap_end);
    else if (env && is(name, n, "__stack_pointer"))
    {
        if (!t->mutable_)
            return E_TYPE;
        *out = s->stack_pointer;
    }
    else if (env && is(name, n, "__stack_low"))
        return shared_global(s, &s->stack_low, t, s->stack_low_value, out);
    else if (env && is(name, n, "__stack_high"))
        return shared_global(s, &s->stack_high, t, s->stack_high_value, out);
    else if (got)
    {
        if (s->got_count == s->got_capacity)
        {
            uint32_t capacity = s->got_capacity ? s->got_capacity * 2u : 16u;
            Got* entries = (Got*)wm_realloc(s->got, capacity * sizeof(Got));
            if (!entries)
                return E_MEMORY;
            s->got = entries;
            s->got_capacity = capacity;
        }
        *out = new_global(s, t->type, t->mutable_, 0);
        if (*out)
        {
            Got* g = &s->got[s->got_count++];
            g->object = o;
            g->global = *out;
            g->name = name;
            g->name_length = n;
            g->func = is(imp->module, imp->module_length, "GOT.func");
        }
    }
    else
    {
        /* A global another module exports: that module must be in already. */
        WmExtern found;
        Object* owner;
        WmResult result = find_symbol(s, WM_EXTERN_GLOBAL, name, n, &found, &owner);
        if (result)
            return result;
        if (!found.as.global)
        {
            if (!(import_flags(o, imp) & BINDING_WEAK))
                return E_UNRESOLVED;
            *out = new_global(s, t->type, t->mutable_, 0);
        }
        else if (found.as.global->type != t->type || found.as.global->mutable_ != t->mutable_)
            return E_TYPE;
        else
            *out = found.as.global;
    }
    return *out ? NULL : E_MEMORY;
}

/* Imports while a module is instantiated: host functions now, other modules' functions after, the rest now. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    State* s = (State*)context;
    const uint8_t* mod = (const uint8_t*)module;
    const uint8_t* field = (const uint8_t*)name;
    out->kind = kind;
    switch (kind)
    {
        case WM_EXTERN_FUNC:
            out->as.func = NULL;
            if (s->options && s->options->host)
                return s->options->host(s->options->context, module, module_length, name, name_length, kind, out);
            return NULL;
        case WM_EXTERN_MEMORY:
            if (!is(mod, module_length, "env") || !is(field, name_length, "memory") || !s->memory)
                return E_UNSUPPORTED;
            out->as.memory = s->memory;
            return NULL;
        case WM_EXTERN_TABLE:
            if (!is(mod, module_length, "env") || !is(field, name_length, "__indirect_function_table") || !s->table)
                return E_UNSUPPORTED;
            out->as.table = s->table;
            return NULL;
        default:
        {
            const WmImport* imp = find_import(s->current->module, WM_EXTERN_GLOBAL, mod, module_length, field,
                                              name_length);
            if (!imp)
                return E_UNRESOLVED;
            return global_import(s, imp, &out->as.global);
        }
    }
}

/* ---- Loading ---- */

static int find_dependency(State* s, const char* name)
{
    for (uint32_t i = 0; i < s->count; ++i)
        if (!s->objects[i].is_main && strcmp(s->objects[i].name, name) == 0)
            return (int)i;
    return -1;
}

static WmResult add_object(State* s, const char* name, const WmModule* module, int is_main, uint32_t* index)
{
    Object* o;
    Reader section;
    WmResult result;
    if (!name || !module)
        return E_MISSING;
    if (!find_section(module, &section))
        return WM_ERR_UNLINKABLE("unlinkable: module has no dylink.0 section");
    for (uint32_t i = 0; i < s->count; ++i)
        if (s->objects[i].module == module)
            return E_DUPLICATE;
    if (s->count == s->capacity)
    {
        uint32_t capacity = s->capacity ? s->capacity * 2u : 4u;
        Object* objects = (Object*)wm_realloc(s->objects, capacity * sizeof(Object));
        if (!objects)
            return E_MEMORY;
        s->objects = objects;
        s->capacity = capacity;
    }
    o = &s->objects[s->count];
    memset(o, 0, sizeof(*o));
    o->module = module;
    o->is_main = (uint8_t)is_main;
    /* From here on the object, with a library's module, is the process's to free. */
    *index = s->count++;
    o->name = copy_name((const uint8_t*)name, (uint32_t)strlen(name));
    if (!o->name)
        return E_MEMORY;
    if ((result = parse_section(&o->meta, &section)))
        return result;
    return NULL;
}

/* Every library the program needs, through the embedder's finder. A cycle is fine: symbols come later. */
static WmResult collect(State* s, uint32_t index)
{
    if (s->objects[index].visit)
        return NULL;
    s->objects[index].visit = 1;
    for (uint32_t i = 0; i < s->objects[index].meta.needed_count; ++i)
    {
        const char* needed = s->objects[index].meta.needed[i];
        int found = find_dependency(s, needed);
        uint32_t dependency;
        WmResult result;
        if (found < 0)
        {
            WmModule* module = NULL;
            if (!s->options || !s->options->find)
                return E_MISSING;
            if ((result = s->options->find(s->options->context, needed, &module)))
                return result;
            if (!module)
                return E_MISSING;
            if ((result = add_object(s, needed, module, 0, &dependency)))
            {
                /* Not the process's yet unless it was added. */
                if (s->count == 0 || s->objects[s->count - 1u].module != module)
                    wm_module_free(module);
                return result;
            }
        }
        else
            dependency = (uint32_t)found;
        if ((result = collect(s, dependency)))
            return result;
    }
    return NULL;
}

/* Dependencies before the modules that need them. */
static void order_visit(State* s, uint32_t index, uint32_t* order, uint32_t* count)
{
    if (s->objects[index].visit)
        return;
    s->objects[index].visit = 1;
    for (uint32_t i = 0; i < s->objects[index].meta.needed_count; ++i)
    {
        int dependency = find_dependency(s, s->objects[index].meta.needed[i]);
        if (dependency >= 0)
            order_visit(s, (uint32_t)dependency, order, count);
    }
    order[(*count)++] = index;
}

static int allocate(uint32_t* cursor, uint32_t align_log2, uint32_t size, uint32_t* base)
{
    uint32_t mask;
    if (align_log2 >= 32u)
        return 0;
    mask = align_log2 ? (1u << align_log2) - 1u : 0;
    if (*cursor > UINT32_MAX - mask)
        return 0;
    *base = (*cursor + mask) & ~mask;
    if (size > UINT32_MAX - *base)
        return 0;
    *cursor = *base + size;
    return 1;
}

/* The program may define the memory and the table; the libraries import them from env. */
static int imports_kind(const WmModule* m, uint8_t kind, const char* name)
{
    for (uint32_t i = 0; i < m->import_count; ++i)
        if (m->imports[i].kind == kind)
            return is(m->imports[i].module, m->imports[i].module_length, "env") &&
                   is(m->imports[i].name, m->imports[i].name_length, name);
    return 0;
}

static WmResult check_object(const Object* o)
{
    const WmModule* m = o->module;
    const Metadata* d = &o->meta;
    if (m->memory_count && (m->memory_imports ? !imports_kind(m, WM_EXTERN_MEMORY, "memory") : !o->is_main))
        return E_UNSUPPORTED;
    if (m->table_count &&
        (m->table_imports ? !imports_kind(m, WM_EXTERN_TABLE, "__indirect_function_table") : !o->is_main))
        return E_UNSUPPORTED;
    if (m->table_count > 1u || d->memory_align >= 32u || d->table_align >= 32u)
        return E_UNSUPPORTED;
    for (uint32_t i = 0; i < d->export_count; ++i)
        if (d->exports[i].flags & TLS)
            return E_UNSUPPORTED;
    for (uint32_t i = 0; i < d->import_count; ++i)
        if (d->imports[i].flags & TLS)
            return E_UNSUPPORTED;
    return NULL;
}

static int same_func(const WmFunc* a, const WmFunc* b)
{
    return a == b || (a && b && a->code && a->code == b->code && a->instance == b->instance);
}

static int table_index(const State* s, const WmFunc* f)
{
    for (uint32_t i = 1; i < s->table->size; ++i)
        if (same_func((const WmFunc*)s->table->elems[i], f))
            return (int)i;
    return -1;
}

/* A function import nothing has bound yet: still this instance's stub. */
static int unbound(const WmInstance* inst, uint32_t index)
{
    return inst->funcs[index] == &inst->own_funcs[index] && !inst->own_funcs[index].code &&
           !inst->own_funcs[index].host;
}

/* Function imports of other modules' code: deps first, so that a re-exported import is linked when copied. */
static WmResult link_functions(State* s, const uint32_t* order, uint32_t count)
{
    for (uint32_t k = 0; k < count; ++k)
    {
        Object* o = &s->objects[order[k]];
        const WmModule* m = o->module;
        WmInstance* inst = o->instance;
        for (uint32_t i = 0; i < m->import_count; ++i)
        {
            const WmImport* imp = &m->imports[i];
            WmFunc* stub;
            WmExtern found;
            Object* owner;
            WmResult result;
            if (imp->kind != WM_EXTERN_FUNC || !unbound(inst, imp->index))
                continue;
            stub = &inst->own_funcs[imp->index];
            if ((result = find_symbol(s, WM_EXTERN_FUNC, imp->name, imp->name_length, &found, &owner)))
                return result;
            if (!found.as.func || found.as.func == stub || !found.as.func->code)
                continue;
            if (!wm_types_equal(stub->params, stub->param_count, found.as.func->params, found.as.func->param_count) ||
                !wm_types_equal(stub->results, stub->result_count, found.as.func->results,
                                found.as.func->result_count))
                return E_TYPE;
            *stub = *found.as.func;
        }
    }
    return NULL;
}

static WmResult check_imports(State* s)
{
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        Object* o = &s->objects[oi];
        const WmModule* m = o->module;
        for (uint32_t i = 0; i < m->import_count; ++i)
        {
            const WmImport* imp = &m->imports[i];
            if (imp->kind == WM_EXTERN_FUNC && unbound(o->instance, imp->index) &&
                !(import_flags(o, imp) & BINDING_WEAK))
                return E_UNRESOLVED;
        }
    }
    return NULL;
}

/* Every exported function gets a table entry, unless it has one, so that GOT.func can name it. */
static WmResult fill_table(State* s, uint32_t cursor)
{
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        const WmModule* m = s->objects[oi].module;
        WmInstance* inst = s->objects[oi].instance;
        for (uint32_t e = 0; e < m->export_count; ++e)
        {
            WmFunc* f;
            if (m->exports[e].kind != WM_EXTERN_FUNC)
                continue;
            f = inst->funcs[m->exports[e].index];
            if (!f->code || table_index(s, f) >= 0)
                continue;
            if (cursor >= s->table->size)
                return E_UNSUPPORTED;
            s->table->elems[cursor++] = (uintptr_t)f;
        }
    }
    return NULL;
}

static WmResult fill_got(State* s)
{
    for (uint32_t i = 0; i < s->got_count; ++i)
    {
        Got* g = &s->got[i];
        WmExtern found;
        Object* owner;
        WmResult result = find_symbol(s, g->func ? WM_EXTERN_FUNC : WM_EXTERN_GLOBAL, g->name, g->name_length,
                                      &found, &owner);
        uint64_t value = 0;
        if (result)
            return result;
        if (g->func && found.as.func && table_index(s, found.as.func) >= 0)
            value = (uint32_t)table_index(s, found.as.func);
        else if (!g->func && found.as.global)
        {
            uint32_t relative;
            if (found.as.global->type != WM_I32)
                return E_TYPE;
            relative = (uint32_t)wm_global_get(found.as.global);
            if (relative > UINT32_MAX - owner->memory_base)
                return E_UNSUPPORTED;
            value = relative + owner->memory_base;
        }
        else
        {
            const Metadata* d = &g->object->meta;
            uint32_t flags = info_flags(d->imports, d->import_count, (const uint8_t*)(g->func ? "GOT.func" : "GOT.mem"),
                                        g->func ? 8u : 7u, g->name, g->name_length);
            if (!(flags & BINDING_WEAK))
                return E_UNRESOLVED;
        }
        g->global->value = value;
    }
    return NULL;
}

/* An initializer the module may export: no parameters, no results. */
static WmResult call_optional(State* s, WmInstance* inst, const char* name, int* found)
{
    WmExtern f;
    *found = wm_instance_export(inst, name, (uint32_t)strlen(name), &f) == NULL && f.kind == WM_EXTERN_FUNC;
    if (!*found)
        return NULL;
    if (f.as.func->param_count || f.as.func->result_count)
        return E_TYPE;
    return wm_call(s->runtime, f.as.func, NULL, NULL);
}

static WmResult initialize(State* s, const uint32_t* order, uint32_t count)
{
    WmResult result;
    int found;
    for (uint32_t i = 0; i < count; ++i)
        if ((result = wm_instance_start(s->objects[order[i]].instance, s->runtime)))
            return result;
    for (uint32_t i = 0; i < count; ++i)
        if ((result = call_optional(s, s->objects[order[i]].instance, "__wasm_apply_data_relocs", &found)))
            return result;
    for (uint32_t i = 0; i < count; ++i)
    {
        WmInstance* inst = s->objects[order[i]].instance;
        if ((result = call_optional(s, inst, "_initialize", &found)))
            return result;
        if (!found && (result = call_optional(s, inst, "__wasm_call_ctors", &found)))
            return result;
    }
    return NULL;
}

static WmResult load(State* s, const WmModule* program, const char* name, uint32_t** order_out)
{
    uint32_t* order;
    uint32_t main_index, count = 0;
    uint32_t memory_cursor = 0, table_cursor = 1;
    uint32_t min_pages = 0, max_pages = WM_NONE, min_table = 1, max_table = WM_NONE;
    uint32_t stack_size = s->options && s->options->stack_size ? s->options->stack_size : DEFAULT_STACK_SIZE;
    uint32_t export_count = 0, first_export, table_size, pages;
    uint64_t required;
    int page_log2 = -1;
    WmResult result;
    WmInstance* main_instance;

    if ((result = add_object(s, name ? name : "main", program, 1, &main_index)) || (result = collect(s, main_index)))
        return result;
    if (!(order = *order_out = (uint32_t*)wm_malloc(s->count * sizeof(uint32_t))))
        return E_MEMORY;
    for (uint32_t i = 0; i < s->count; ++i)
        s->objects[i].visit = 0;
    order_visit(s, main_index, order, &count);

    /* Data, then table entries, one module after another; the linear stack after all the data, then the heap. */
    for (uint32_t i = 0; i < s->count; ++i)
    {
        Object* o = &s->objects[i];
        const WmModule* m = o->module;
        if ((result = check_object(o)))
            return result;
        if (!allocate(&memory_cursor, o->meta.memory_align, o->meta.memory_size, &o->memory_base) ||
            !allocate(&table_cursor, o->meta.table_align, o->meta.table_size, &o->table_base))
            return E_UNSUPPORTED;
        if (m->memory_count)
        {
            if (page_log2 >= 0 && page_log2 != m->memory.page_log2)
                return E_UNSUPPORTED;
            page_log2 = m->memory.page_log2;
            if (m->memory.min > min_pages)
                min_pages = m->memory.min;
            if (m->memory.has_max && m->memory.max < max_pages)
                max_pages = m->memory.max;
        }
        if (m->table_count)
        {
            if (m->tables[0].limits.min > min_table)
                min_table = m->tables[0].limits.min;
            if (m->tables[0].limits.has_max && m->tables[0].limits.max < max_table)
                max_table = m->tables[0].limits.max;
        }
        for (uint32_t e = 0; e < m->export_count; ++e)
            export_count += m->exports[e].kind == WM_EXTERN_FUNC;
    }
    if (page_log2 < 0)
        page_log2 = 16;
    if (!allocate(&memory_cursor, 4, stack_size, &s->stack_low_value) ||
        !allocate(&memory_cursor, 4, 0, &s->stack_high_value))
        return E_UNSUPPORTED;
    s->heap_base = memory_cursor;
    required = memory_cursor;
    if ((uint64_t)min_pages << page_log2 > required)
        required = (uint64_t)min_pages << page_log2;
    if (((required + (1u << page_log2) - 1u) >> page_log2) > (max_pages == WM_NONE ? UINT32_MAX : max_pages) ||
        ((required + (1u << page_log2) - 1u) >> page_log2) << page_log2 > UINT32_MAX)
        return E_MEMORY_SIZE;
    pages = (uint32_t)((required + (1u << page_log2) - 1u) >> page_log2);
    s->heap_end = (uint32_t)((uint64_t)pages << page_log2);
    if (export_count > UINT32_MAX - table_cursor)
        return E_UNSUPPORTED;
    first_export = table_cursor;
    table_size = table_cursor + export_count > min_table ? table_cursor + export_count : min_table;
    if (table_size > max_table)
        return E_UNSUPPORTED;
    if (!(s->stack_pointer = new_global(s, WM_I32, 1, s->stack_high_value)))
        return E_MEMORY;

    /* The program first: its memory and table, grown to fit everything, are the libraries'. */
    if (program->memory_imports)
    {
        if (!(s->memory = wm_memory_new_paged(pages, max_pages, (uint8_t)page_log2)))
            return E_MEMORY;
        s->owns_memory = 1;
    }
    if (program->table_imports)
    {
        if (!(s->table = wm_table_new(WM_FUNCREF, table_size, max_table)))
            return E_MEMORY;
        s->owns_table = 1;
    }
    s->current = &s->objects[main_index];
    if ((result = wm_instantiate(&s->objects[main_index].instance, program, NULL, resolve, s)))
        return result;
    main_instance = s->objects[main_index].instance;
    if (!s->memory && (s->memory = wm_instance_memory(main_instance)) && s->memory->pages < pages &&
        wm_memory_grow(s->memory, pages - s->memory->pages) == WM_NONE)
        return E_MEMORY_SIZE;
    if (!s->table && program->table_count)
    {
        s->table = main_instance->tables[0];
        if (s->table->size < table_size && wm_table_grow(s->table, table_size - s->table->size) == WM_NONE)
            return E_UNSUPPORTED;
    }
    if (!s->table)
    {
        if (!(s->table = wm_table_new(WM_FUNCREF, table_size, max_table)))
            return E_MEMORY;
        s->owns_table = 1;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        Object* o = &s->objects[order[i]];
        if (o->is_main)
            continue;
        s->current = o;
        if ((result = wm_instantiate(&o->instance, o->module, NULL, resolve, s)))
            return result;
    }

    if ((result = link_functions(s, order, count)) || (result = check_imports(s)) ||
        (result = fill_table(s, first_export)) || (result = fill_got(s)))
        return result;
    return initialize(s, order, count);
}

WmResult wm_dylink_load(WmDylink** out, const WmModule* program, const char* name, WmRuntime* runtime,
                        const WmDylinkOptions* options)
{
    State* s;
    uint32_t* order = NULL;
    WmResult result;
    *out = NULL;
    if (!program || !runtime)
        return E_MISSING;
    s = (State*)wm_calloc(1, sizeof(State));
    if (!s)
        return E_MEMORY;
    s->runtime = runtime;
    s->options = options;
    *out = s;
    result = load(s, program, name, &order);
    wm_free(order);
    /* The options need not outlive the call. */
    s->options = NULL;
    return result;
}

WmInstance* wm_dylink_instance(const WmDylink* s)
{
    for (uint32_t i = 0; s && i < s->count; ++i)
        if (s->objects[i].is_main)
            return s->objects[i].instance;
    return NULL;
}

void wm_dylink_free(WmDylink* s)
{
    if (!s)
        return;
    /* Instances first: they may hold the shared memory and table. */
    for (uint32_t i = 0; i < s->count; ++i)
        wm_instance_free(s->objects[i].instance);
    for (uint32_t i = 0; i < s->global_count; ++i)
        wm_global_free(s->globals[i]);
    if (s->owns_memory)
        wm_memory_free(s->memory);
    if (s->owns_table)
        wm_table_free(s->table);
    for (uint32_t i = 0; i < s->count; ++i)
    {
        Object* o = &s->objects[i];
        free_metadata(&o->meta);
        wm_free(o->name);
        if (!o->is_main)
            wm_module_free((WmModule*)o->module);
    }
    wm_free(s->objects);
    wm_free(s->globals);
    wm_free(s->got);
    wm_free(s);
}
