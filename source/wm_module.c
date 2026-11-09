/*
 * Decoding and validation: everything a module needs before it can run.
 *
 * Errors are sticky: the reader keeps the first one, reads after it give
 * zeros, and the code checks once per section or instruction instead of
 * after every read. Indices are still checked before they are used.
 */
#include "wm_internal.h"

#include <stdlib.h>

#define UNKNOWN 0x00
/* Locals beyond this make a module too big for this interpreter. */
#define MAX_LOCALS 50000u

#define E_EOF WM_ERR_MALFORMED("malformed: unexpected end")
#define E_LONG WM_ERR_MALFORMED("malformed: integer representation too long")
#define E_LARGE WM_ERR_MALFORMED("malformed: integer too large")
#define E_MEMORY WM_ERR_EXHAUSTION("exhaustion: out of memory")
#define E_MISMATCH WM_ERR_INVALID("invalid: type mismatch")
#define E_FLOAT WM_ERR_UNSUPPORTED("unsupported: floating point (built without WM_HAS_FLOAT)")
#define E_I64 WM_ERR_UNSUPPORTED("unsupported: i64 (built without WM_HAS_I64)")
#define E_REFS WM_ERR_UNSUPPORTED("unsupported: reference types and table instructions (built without WM_HAS_REF_TYPES)")
#define E_UNKNOWN_FUNC WM_ERR_INVALID("invalid: unknown function")
#define E_UNKNOWN_TYPE WM_ERR_INVALID("invalid: unknown type")
#define E_UNKNOWN_TABLE WM_ERR_INVALID("invalid: unknown table")
#define E_UNKNOWN_MEMORY WM_ERR_INVALID("invalid: unknown memory")
#define E_UNKNOWN_GLOBAL WM_ERR_INVALID("invalid: unknown global")
#define E_MEMORY_SIZE WM_ERR_INVALID("invalid: memory size must be at most 65536 pages (4GiB)")

typedef struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
    WmResult error;
} Reader;

static void fail(Reader* r, WmResult error)
{
    if (!r->error)
        r->error = error;
}

/* An invalid program: refused with validation, trusted without it. */
#if WM_HAS_VALIDATION
#define REJECT(r, error) fail((r), (error))
#else
#define REJECT(r, error) ((void)0)
#endif

static uint8_t read_u8(Reader* r)
{
    if (r->p >= r->end)
    {
        fail(r, E_EOF);
        return 0;
    }
    return WM_RD8(r->p++);
}

/*
 * A LEB128 number of the given bits, signed or not: its low 32 bits, sign
 * extended. The encoding is checked in full, also for 33 and 64 bits.
 */
static uint32_t read_leb(Reader* r, uint8_t bits, uint8_t is_signed)
{
    uint32_t value = 0;
    uint8_t max = (uint8_t)((bits + 6u) / 7u);
    for (uint8_t i = 0; i < max; ++i)
    {
        uint8_t byte = read_u8(r);
        uint8_t shift = (uint8_t)(7u * i);
        if (r->error)
            return 0;
        if (shift < 32u)
            value |= (uint32_t)(byte & 0x7Fu) << shift;
        if (byte & 0x80u)
            continue;
        if (i == max - 1u)
        {
            /* The bits past the number must be zero, or copy the sign. */
            uint8_t used = (uint8_t)(bits - shift);
            uint8_t mask = (uint8_t)(0x7Fu & ~((1u << used) - 1u));
            uint8_t want = (is_signed && ((byte >> (used - 1u)) & 1u)) ? mask : 0;
            if ((byte & mask) != want)
                REJECT(r, E_LARGE);
        }
        if (is_signed && (byte & 0x40u) && shift + 7u < 32u)
            value |= ~(uint32_t)0 << (shift + 7u);
        return value;
    }
    fail(r, E_LONG);
    return 0;
}

static uint32_t read_u32(Reader* r) { return read_leb(r, 32u, 0); }

static const uint8_t* skip(Reader* r, uint32_t count)
{
    const uint8_t* start = r->p;
    if ((uint32_t)(r->end - r->p) < count)
    {
        fail(r, E_EOF);
        r->p = r->end;
        return start;
    }
    r->p += count;
    return start;
}

static int valid_utf8(const uint8_t* s, uint32_t length)
{
    uint32_t i = 0;
    while (i < length)
    {
        uint8_t c = WM_RD8(s + i);
        uint8_t extra;
        uint32_t code;
        if (c < 0x80u)
        {
            ++i;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu)
            extra = 1, code = c & 0x1Fu;
        else if (c >= 0xE0u && c <= 0xEFu)
            extra = 2, code = c & 0x0Fu;
        else if (c >= 0xF0u && c <= 0xF4u)
            extra = 3, code = c & 0x07u;
        else
            return 0;
        if (length - i <= extra)
            return 0;
        for (uint8_t k = 1; k <= extra; ++k)
        {
            uint8_t next = WM_RD8(s + i + k);
            if ((next & 0xC0u) != 0x80u)
                return 0;
            code = (code << 6) | (next & 0x3Fu);
        }
        /* Overlong forms, surrogates and code points past U+10FFFF. */
        if ((extra == 2u && code < 0x800u) || (extra == 3u && (code < 0x10000u || code > 0x10FFFFu)) ||
            (code >= 0xD800u && code <= 0xDFFFu))
            return 0;
        i += extra + 1u;
    }
    return 1;
}

static const uint8_t* read_name(Reader* r, uint32_t* length)
{
    const uint8_t* name;
    *length = read_u32(r);
    name = skip(r, *length);
    if (WM_HAS_VALIDATION && !r->error && !valid_utf8(name, *length))
        REJECT(r, WM_ERR_MALFORMED("malformed: malformed UTF-8 encoding"));
    return name;
}

static int is_ref(uint8_t t) { return t == WM_FUNCREF || t == WM_EXTERNREF; }
static int is_float(uint8_t t) { return t == WM_F32 || t == WM_F64; }

static int is_value_type(uint8_t t) { return (t >= WM_F64 && t <= WM_I32) || is_ref(t); }

/* Why a value type cannot be used in this build, or NULL. */
static WmResult unsupported(uint8_t t)
{
    if (!WM_HAS_FLOAT && is_float(t))
        return E_FLOAT;
    if (!WM_HAS_I64 && t == WM_I64)
        return E_I64;
    if (!WM_HAS_REF_TYPES && is_ref(t))
        return E_REFS;
    return NULL;
}

uint32_t wm_type_slots(uint8_t type)
{
    if (type == WM_I64 || type == WM_F64)
        return 2u;
    return is_ref(type) ? WM_REF_SLOTS : 1u;
}

static uint32_t slots_of(const uint8_t* types, uint32_t count)
{
    uint32_t slots = 0;
    for (uint32_t i = 0; i < count; ++i)
        slots += wm_type_slots(WM_RD8(types + i));
    return slots;
}

int wm_types_equal(const uint8_t* a, uint32_t a_count, const uint8_t* b, uint32_t b_count)
{
    if (a_count != b_count)
        return 0;
    for (uint32_t i = 0; i < a_count; ++i)
        if (WM_RD8(a + i) != WM_RD8(b + i))
            return 0;
    return 1;
}

int wm_bytes_equal(const uint8_t* program, uint32_t program_count, const uint8_t* ram, uint32_t ram_count)
{
    if (program_count != ram_count)
        return 0;
    for (uint32_t i = 0; i < program_count; ++i)
        if (WM_RD8(program + i) != ram[i])
            return 0;
    return 1;
}

int wm_func_has_type(const WmFunc* f, const WmType* t)
{
    /* A host function's types are in RAM, a program's where its bytes are. */
    if (!f->instance)
        return wm_bytes_equal(t->params, t->param_count, f->params, f->param_count) &&
               wm_bytes_equal(t->results, t->result_count, f->results, f->result_count);
    return wm_types_equal(f->params, f->param_count, t->params, t->param_count) &&
           wm_types_equal(f->results, f->result_count, t->results, t->result_count);
}

/* A value type; float types are refused when floating point is not built in. */
static uint8_t check_value_type(Reader* r, uint8_t t)
{
    if (!is_value_type(t))
        REJECT(r, WM_ERR_MALFORMED("malformed: malformed value type"));
    else
        fail(r, unsupported(t));
    return t;
}

static uint8_t read_value_type(Reader* r) { return check_value_type(r, read_u8(r)); }

static uint8_t read_ref_type(Reader* r)
{
    uint8_t t = read_u8(r);
    if (!is_ref(t))
        REJECT(r, WM_ERR_MALFORMED("malformed: malformed reference type"));
    return t;
}

static void read_limits(Reader* r, WmLimits* limits, uint32_t bound, WmResult too_big)
{
    uint8_t flags = read_u8(r);
    /* Bit 3: a page size follows (memories only, custom page sizes). */
    uint8_t paged = too_big != NULL && (flags & 8u);
    if ((flags & (uint8_t)~(paged ? 9u : 1u)) != 0)
        REJECT(r, E_LARGE);
    limits->has_max = flags & 1u;
    limits->min = read_u32(r);
    limits->max = limits->has_max ? read_u32(r) : 0;
    limits->page_log2 = 16;
    if (paged)
    {
        uint32_t log2 = read_u32(r);
        if (log2 != 0 && log2 != 16u)
            fail(r, WM_ERR_INVALID("invalid: invalid custom page size"));
        limits->page_log2 = (uint8_t)log2;
        if (log2 == 0)
            bound = 0xFFFFFFFFu;
    }
    if (limits->min > bound || limits->max > bound)
        REJECT(r, too_big);
    if (limits->has_max && limits->min > limits->max)
        REJECT(r, WM_ERR_INVALID("invalid: size minimum must not be greater than maximum"));
}

/* A new zeroed element at the end of *array, or NULL with the error set. */
static void* append(Reader* r, void* array, uint32_t* count, size_t size)
{
    void** at = (void**)array;
    uint32_t n = *count;
    uint8_t* grown = (uint8_t*)*at;
    if (r->error)
        return NULL;
    /* Room for 4, then twice as much each time it is full: few moves, few holes in a small heap. */
    if (n == 0 || (n >= 4u && (n & (n - 1u)) == 0))
    {
        grown = (uint8_t*)wm_realloc(*at, (size_t)(n ? n * 2u : 4u) * size);
        if (!grown)
        {
            fail(r, E_MEMORY);
            return NULL;
        }
        *at = grown;
    }
    grown += (size_t)(*count)++ * size;
    memset(grown, 0, size);
    return grown;
}

/* Loader state beyond the module itself. */
typedef struct Loader
{
    WmModule* m;
    /* Functions that ref.func may name: those in exports, elements and globals. */
    uint8_t* declared;
    uint32_t declared_count;
    uint32_t defined_funcs;
    uint8_t has_data_count;
    uint32_t data_count;
} Loader;

static void declare(Loader* l, Reader* r, uint32_t func)
{
    if (!WM_HAS_VALIDATION)
        return;
    if (!l->declared)
    {
        l->declared_count = l->m->func_count;
        l->declared = (uint8_t*)wm_calloc(l->declared_count / 8u + 1u, 1);
        if (!l->declared)
        {
            fail(r, E_MEMORY);
            return;
        }
    }
    if (func < l->declared_count)
        l->declared[func / 8u] |= (uint8_t)(1u << (func % 8u));
}

static int is_declared(const Loader* l, uint32_t func)
{
    return l->declared && func < l->declared_count && (l->declared[func / 8u] & (1u << (func % 8u)));
}

static uint32_t read_func_index(Loader* l, Reader* r)
{
    uint32_t func = read_u32(r);
    if (func >= l->m->func_count)
        fail(r, E_UNKNOWN_FUNC);
    return func;
}

/*
 * A constant expression of one value of type expected: constants,
 * ref.null, ref.func and global.get of an immutable import.
 */
static const uint8_t* read_const(Loader* l, Reader* r, uint8_t expected)
{
    WmModule* m = l->m;
    const uint8_t* start = r->p;
    uint8_t type = UNKNOWN;
    uint8_t count = 0;
#if !WM_HAS_VALIDATION
    /* Trusted: one instruction and its end. */
    switch (read_u8(r))
    {
        case 0x43: skip(r, 4u); break;
        case 0x44: skip(r, 8u); break;
        case 0xD0: read_u8(r); break;
        default: read_leb(r, 64u, 1);
    }
    read_u8(r);
    (void)m, (void)type, (void)count, (void)expected;
    return start;
#endif
    for (;;)
    {
        uint8_t op = read_u8(r);
        if (r->error || op == 0x0Bu)
            break;
        ++count;
        switch (op)
        {
            case 0x41: read_leb(r, 32u, 1); type = WM_I32; break;
            case 0x42: read_leb(r, 64u, 1); type = WM_I64; break;
            case 0x43: skip(r, 4u); type = WM_F32; break;
            case 0x44: skip(r, 8u); type = WM_F64; break;
            case 0xD0: type = read_ref_type(r); break;
            case 0xD2: declare(l, r, read_func_index(l, r)); type = WM_FUNCREF; break;
            case 0x23:
            {
                uint32_t global = read_u32(r);
                if (global >= m->global_imports)
                    fail(r, E_UNKNOWN_GLOBAL);
                else if (m->globals[global].mutable_)
                    fail(r, WM_ERR_INVALID("invalid: constant expression required"));
                else
                    type = m->globals[global].type;
                break;
            }
            default: fail(r, WM_ERR_INVALID("invalid: constant expression required"));
        }
    }
    if (count != 1u || type != expected)
        fail(r, E_MISMATCH);
    return start;
}

const uint8_t WM_ROM wm_numeric[128] = {
    0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x01, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x00, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x09, 0x09, 0x09, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D,
    0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x16, 0x16, 0x16,
    0x16, 0x16, 0x16, 0x16, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
    0x1F, 0x1F, 0x01, 0x02, 0x02, 0x03, 0x03, 0x08, 0x08, 0x0A, 0x0A, 0x0B, 0x0B, 0x10, 0x10, 0x11,
    0x11, 0x13, 0x18, 0x18, 0x19, 0x19, 0x1A, 0x02, 0x0B, 0x10, 0x19, 0x00, 0x00, 0x09, 0x09, 0x09,
};

/* ---- Function bodies ---- */

typedef struct Control
{
    uint8_t kind;
    uint8_t unreachable;
    uint32_t height;
    uint32_t slots;
    const uint8_t* params;
    const uint8_t* results;
    uint32_t param_count;
    uint32_t result_count;
    /* Side entries waiting for this block's end, linked through their pc. */
    uint32_t pending;
    uint32_t if_entry;
    uint32_t loop_pc;
    uint32_t loop_st;
} Control;

typedef struct Validator
{
    Loader* l;
    WmModule* m;
    Reader* r;
    uint8_t* vals;
    uint32_t val_count;
    uint32_t val_capacity;
    Control* ctls;
    uint32_t ctl_count;
    uint32_t ctl_capacity;
    uint32_t slots;
    uint32_t max_slots;
    uint8_t* locals;
    uint32_t local_count;
} Validator;

static void push(Validator* v, uint8_t type)
{
    fail(v->r, unsupported(type));
    if (v->r->error)
        return;
    if (v->val_count == v->val_capacity)
    {
        uint32_t capacity = v->val_capacity ? v->val_capacity * 2u : 8u;
        uint8_t* grown = (uint8_t*)wm_realloc(v->vals, capacity);
        if (!grown)
        {
            fail(v->r, E_MEMORY);
            return;
        }
        v->vals = grown;
        v->val_capacity = capacity;
    }
    if (is_ref(type))
        v->m->stack_refs = 1;
    v->vals[v->val_count++] = type;
    v->slots += wm_type_slots(type);
    if (v->slots > v->max_slots)
        v->max_slots = v->slots;
}

static uint8_t pop(Validator* v)
{
    Control* c = &v->ctls[v->ctl_count - 1u];
    uint8_t type;
    if (v->val_count == c->height)
    {
        if (!c->unreachable)
            REJECT(v->r, E_MISMATCH);
        return UNKNOWN;
    }
    type = v->vals[--v->val_count];
    v->slots -= wm_type_slots(type);
    return type;
}

static uint8_t pop_expect(Validator* v, uint8_t expected)
{
    uint8_t actual;
    fail(v->r, unsupported(expected));
    actual = pop(v);
    if (actual != expected && actual != UNKNOWN && expected != UNKNOWN)
        REJECT(v->r, E_MISMATCH);
    return actual;
}

static void pop_types(Validator* v, const uint8_t* types, uint32_t count)
{
    while (count-- > 0)
        pop_expect(v, WM_RD8(types + count));
}

static void push_types(Validator* v, const uint8_t* types, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        push(v, WM_RD8(types + i));
}

static void set_unreachable(Validator* v)
{
    Control* c = &v->ctls[v->ctl_count - 1u];
    v->val_count = c->height;
    v->slots = c->slots;
    c->unreachable = 1;
}

static Control* push_control(Validator* v, uint8_t kind, const uint8_t* params, uint32_t param_count,
                             const uint8_t* results, uint32_t result_count)
{
    Control* c;
    pop_types(v, params, param_count);
    if (v->r->error)
        return NULL;
    if (v->ctl_count == v->ctl_capacity)
    {
        uint32_t capacity = v->ctl_capacity ? v->ctl_capacity * 2u : 4u;
        Control* grown = (Control*)wm_realloc(v->ctls, capacity * sizeof(Control));
        if (!grown)
        {
            fail(v->r, E_MEMORY);
            return NULL;
        }
        v->ctls = grown;
        v->ctl_capacity = capacity;
    }
    c = &v->ctls[v->ctl_count++];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->height = v->val_count;
    c->slots = v->slots;
    c->params = params;
    c->param_count = param_count;
    c->results = results;
    c->result_count = result_count;
    c->pending = WM_NONE;
    c->if_entry = WM_NONE;
    push_types(v, params, param_count);
    return c;
}

/* A new side entry: its index, or WM_NONE with the error set. */
static uint32_t new_side(Validator* v, uint32_t keep)
{
    WmSide* e;
    /* Entries, offsets and stack sizes must fit the side table's fields. */
    if (v->m->side_count >= WM_SIDE_MAX - 1u || keep >= WM_SIDE_MAX)
        fail(v->r, WM_ERR_UNSUPPORTED("unsupported: program too big for the side table"));
    e = (WmSide*)append(v->r, &v->m->side, &v->m->side_count, sizeof(WmSide));
    if (!e)
        return WM_NONE;
    e->keep = keep;
    return v->m->side_count - 1u;
}

static void patch(WmModule* m, uint32_t head, uint32_t pc, uint32_t st)
{
    while (head != WM_NONE)
    {
        uint32_t next = m->side[head].pc;
        m->side[head].pc = (wm_side_t)pc;
        m->side[head].st = (wm_side_t)st;
        head = next == WM_SIDE_MAX ? WM_NONE : next;
    }
}

static const uint8_t* label_types(const Control* c, uint32_t* count)
{
    if (c->kind == WM_LOOP)
    {
        *count = c->param_count;
        return c->params;
    }
    *count = c->result_count;
    return c->results;
}

/* The block a branch of this depth leaves, or NULL with the error set. */
static Control* label(Validator* v, uint32_t depth)
{
    if (depth >= v->ctl_count)
    {
        fail(v->r, WM_ERR_INVALID("invalid: unknown label"));
        return NULL;
    }
    return &v->ctls[v->ctl_count - 1u - depth];
}

/* A branch's side entry, from the operand stack as it is before the branch's own pops. */
static void branch_entry(Validator* v, Control* c, uint32_t slots_now)
{
    uint32_t count;
    const uint8_t* types = label_types(c, &count);
    uint32_t keep = slots_of(types, count);
    uint32_t index = new_side(v, keep);
    WmSide* e;
    if (index == WM_NONE)
        return;
    e = &v->m->side[index];
    e->drop = slots_now >= keep + c->slots ? slots_now - keep - c->slots : 0;
    if (c->kind == WM_LOOP)
    {
        e->pc = c->loop_pc;
        e->st = c->loop_st;
    }
    else
    {
        e->pc = c->pending;
        c->pending = index;
    }
}

/* A block type: no values, one value type, or a function type by index. */
static void read_block_type(Validator* v, Control* out)
{
    Reader* r = v->r;
    uint8_t first;
    uint32_t index;
    out->params = out->results = NULL;
    out->param_count = out->result_count = 0;
    if (r->p >= r->end)
    {
        fail(r, E_EOF);
        return;
    }
    first = WM_RD8(r->p);
    if (first == 0x40u)
    {
        ++r->p;
        return;
    }
    if (first >= 0x40u && first < 0x80u)
    {
        out->results = r->p;
        out->result_count = 1;
        check_value_type(r, read_u8(r));
        return;
    }
    index = read_leb(r, 33u, 1);
    if (index >= v->m->type_count)
    {
        fail(r, E_UNKNOWN_TYPE);
        return;
    }
    out->params = v->m->types[index].params;
    out->param_count = v->m->types[index].param_count;
    out->results = v->m->types[index].results;
    out->result_count = v->m->types[index].result_count;
}

static void need_memory(Validator* v)
{
    if (v->m->memory_count == 0)
        fail(v->r, E_UNKNOWN_MEMORY);
}

static void read_zero(Reader* r)
{
    if (read_u8(r) != 0)
        REJECT(r, WM_ERR_MALFORMED("malformed: zero byte expected"));
}


static const uint8_t WM_ROM VALUE_TYPES[4] = {WM_I32, WM_I64, WM_F32, WM_F64};

/*
 * Memory accesses 0x28-0x3E: bits 0-1 the natural alignment, bits 2-3 the
 * value type (as in wm_numeric), bit 4 a store.
 */
static const uint8_t WM_ROM ACCESS_TYPES[] = {
    0x02, 0x07, 0x0A, 0x0F, 0x00, 0x00, 0x01, 0x01, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06,
    0x12, 0x17, 0x1A, 0x1F, 0x10, 0x11, 0x14, 0x15, 0x16,
};

/* A numeric instruction by its wm_numeric byte: pop its operands, push its result. */
static void apply_numeric(Validator* v, uint8_t d)
{
    uint8_t operand = WM_ROM8(VALUE_TYPES + (d & 3u));
    if (d & WM_NUM_BINARY)
        pop_expect(v, operand);
    pop_expect(v, operand);
    push(v, WM_ROM8(VALUE_TYPES + ((d >> 3) & 3u)));
}

static uint32_t read_table_index(Validator* v)
{
    uint32_t table = read_u32(v->r);
    if (table >= v->m->table_count)
    {
        fail(v->r, E_UNKNOWN_TABLE);
        return WM_NONE;
    }
    return table;
}

static uint8_t table_type(Validator* v, uint32_t table) { return table == WM_NONE ? UNKNOWN : v->m->tables[table].type; }

static void validate_misc(Validator* v)
{
    WmModule* m = v->m;
    Loader* l = v->l;
    Reader* r = v->r;
    uint32_t op = read_u32(r);
    uint32_t a;
    uint32_t b;
    uint8_t three = 1;
    if (!WM_HAS_REF_TYPES && op >= 12u)
        fail(r, E_REFS);
    if (op <= 7u)
    {
        apply_numeric(v, (uint8_t)(((op & 2u) ? WM_NUM_F64 : WM_NUM_F32) | (op >= 4u ? WM_NUM_WIDE : 0)));
        return;
    }
    switch (op)
    {
        case 8: /* memory.init */
        case 9: /* data.drop */
            a = read_u32(r);
            if (op == 8u)
            {
                read_zero(r);
                need_memory(v);
            }
            if (!l->has_data_count)
                REJECT(r, WM_ERR_MALFORMED("malformed: data count section required"));
            if (a >= l->data_count)
                fail(r, WM_ERR_INVALID("invalid: unknown data segment"));
            three = op == 8u;
            break;
        case 10: /* memory.copy */
            read_zero(r);
            /* fall through */
        case 11: /* memory.fill */
            read_zero(r);
            need_memory(v);
            break;
        case 12: /* table.init */
        case 13: /* elem.drop */
            a = read_u32(r);
            b = op == 12u ? read_table_index(v) : WM_NONE;
            if (a >= m->elem_count)
                fail(r, WM_ERR_INVALID("invalid: unknown elem segment"));
            else if (op == 12u && b != WM_NONE && m->elems[a].type != m->tables[b].type)
                REJECT(r, E_MISMATCH);
            three = op == 12u;
            break;
        case 14: /* table.copy */
            a = read_table_index(v);
            b = read_table_index(v);
            if (table_type(v, a) != table_type(v, b))
                REJECT(r, E_MISMATCH);
            break;
        case 15: /* table.grow */
            a = read_table_index(v);
            pop_expect(v, WM_I32);
            pop_expect(v, table_type(v, a));
            push(v, WM_I32);
            return;
        case 16: /* table.size */
            read_table_index(v);
            push(v, WM_I32);
            return;
        case 17: /* table.fill */
            a = read_table_index(v);
            pop_expect(v, WM_I32);
            pop_expect(v, table_type(v, a));
            pop_expect(v, WM_I32);
            return;
        default: fail(r, WM_ERR_MALFORMED("malformed: illegal opcode")); return;
    }
    /* The copy, fill and init forms take three i32s. */
    if (three)
    {
        pop_expect(v, WM_I32);
        pop_expect(v, WM_I32);
        pop_expect(v, WM_I32);
    }
}

static void validate_body(Validator* v, const WmType* type, WmCode* code)
{
    WmModule* m = v->m;
    Reader* r = v->r;
    const uint8_t* base = m->bytes;

    push_control(v, WM_FUNCTION, NULL, 0, type->results, type->result_count);
    while (v->ctl_count > 0 && !r->error)
    {
        const uint8_t* at = r->p;
        uint8_t op = read_u8(r);
        Control* c = &v->ctls[v->ctl_count - 1u];
        uint32_t index;
        if (op >= 0x45u && op <= 0xC4u)
        {
            apply_numeric(v, WM_ROM8(wm_numeric + (op - 0x45u)));
            continue;
        }
        if (op >= 0x28u && op <= 0x3Eu)
        {
            uint8_t d = WM_ROM8(ACCESS_TYPES + (op - 0x28u));
            uint8_t value = WM_ROM8(VALUE_TYPES + ((d >> 2) & 3u));
            if (read_u32(r) > (d & 3u))
                REJECT(r, WM_ERR_INVALID("invalid: alignment must not be larger than natural"));
            read_u32(r);
            need_memory(v);
            if (d & 0x10u)
                pop_expect(v, value);
            pop_expect(v, WM_I32);
            if (!(d & 0x10u))
                push(v, value);
            continue;
        }
        if (!WM_HAS_REF_TYPES && (op == 0x25u || op == 0x26u || (op >= 0xD0u && op <= 0xD2u)))
            fail(r, E_REFS);
        switch (op)
        {
            case 0x00: set_unreachable(v); break;
            case 0x01: break;
            case 0x02:
            case 0x03:
            case 0x04:
            {
                Control block;
                read_block_type(v, &block);
                if (op == WM_IF)
                    pop_expect(v, WM_I32);
                c = push_control(v, op, block.params, block.param_count, block.results, block.result_count);
                if (!c)
                    break;
                c->loop_pc = (uint32_t)(r->p - base);
                c->loop_st = m->side_count;
                if (op == WM_IF)
                    c->if_entry = new_side(v, 0);
                break;
            }
            case 0x05:
                if (c->kind != WM_IF)
                {
                    fail(r, WM_ERR_MALFORMED("malformed: else without if"));
                    break;
                }
                pop_types(v, c->results, c->result_count);
                if (v->val_count != c->height)
                    REJECT(r, E_MISMATCH);
                index = new_side(v, 0);
                if (index == WM_NONE)
                    break;
                /* The false way in starts after the else and its own entry. */
                m->side[c->if_entry].pc = (uint32_t)(r->p - base);
                m->side[c->if_entry].st = m->side_count;
                m->side[index].pc = c->pending;
                c->pending = index;
                c->if_entry = WM_NONE;
                c->kind = WM_ELSE;
                c->unreachable = 0;
                v->val_count = c->height;
                v->slots = c->slots;
                push_types(v, c->params, c->param_count);
                break;
            case 0x0B:
            {
                Control done = *c;
                uint32_t target = (uint32_t)(at - base);
                if (done.kind == WM_IF && !wm_types_equal(done.params, done.param_count, done.results, done.result_count))
                    REJECT(r, E_MISMATCH);
                pop_types(v, done.results, done.result_count);
                if (v->val_count != done.height)
                    REJECT(r, E_MISMATCH);
                if (done.if_entry != WM_NONE)
                {
                    m->side[done.if_entry].pc = target;
                    m->side[done.if_entry].st = m->side_count;
                }
                patch(m, done.pending, target, m->side_count);
                --v->ctl_count;
                if (v->ctl_count > 0)
                    push_types(v, done.results, done.result_count);
                else
                    code->end = at;
                break;
            }
            case 0x0C:
            case 0x0D:
            {
                uint32_t count;
                const uint8_t* types;
                Control* target = label(v, read_u32(r));
                if (op == 0x0Du)
                    pop_expect(v, WM_I32);
                if (!target)
                    break;
                branch_entry(v, target, v->slots);
                types = label_types(target, &count);
                pop_types(v, types, count);
                if (op == 0x0Cu)
                    set_unreachable(v);
                else
                    push_types(v, types, count);
                break;
            }
            case 0x0E:
            {
                uint32_t count = read_u32(r);
                uint32_t arity = WM_NONE;
                uint32_t slots_now;
                pop_expect(v, WM_I32);
                slots_now = v->slots;
                /* One side entry per label, the default last. */
                for (uint32_t i = 0; i <= count && !r->error; ++i)
                {
                    Control* target = label(v, read_u32(r));
                    uint32_t n;
                    const uint8_t* types;
                    if (!target)
                        break;
                    branch_entry(v, target, slots_now);
                    types = label_types(target, &n);
#if WM_HAS_VALIDATION
                    {
                        uint8_t popped[64];
                        if (arity != WM_NONE && n != arity)
                            REJECT(r, E_MISMATCH);
                        arity = n;
                        if (n > sizeof(popped))
                        {
                            REJECT(r, WM_ERR_INVALID("invalid: too many branch values"));
                            break;
                        }
                        /* Check against the stack, then put back what was there. */
                        for (uint32_t k = n; k-- > 0;)
                        {
                            popped[k] = pop(v);
                            if (popped[k] != WM_RD8(types + k) && popped[k] != UNKNOWN)
                                REJECT(r, E_MISMATCH);
                        }
                        for (uint32_t k = 0; k < n; ++k)
                            push(v, popped[k]);
                    }
#else
                    (void)types, (void)arity;
#endif
                }
                set_unreachable(v);
                break;
            }
            case 0x0F:
                pop_types(v, type->results, type->result_count);
                set_unreachable(v);
                break;
            case 0x10:
            case 0x11:
            {
                const WmType* callee;
                if (op == 0x10u)
                {
                    index = read_func_index(v->l, r);
                    if (r->error)
                        break;
                    index = m->func_types[index];
                }
                else
                {
                    uint32_t table;
                    index = read_u32(r);
                    table = read_table_index(v);
                    if (index >= m->type_count)
                        fail(r, E_UNKNOWN_TYPE);
                    if (table_type(v, table) != WM_FUNCREF)
                        REJECT(r, E_MISMATCH);
                    if (r->error)
                        break;
                    pop_expect(v, WM_I32);
                }
                callee = &m->types[index];
                pop_types(v, callee->params, callee->param_count);
                push_types(v, callee->results, callee->result_count);
                break;
            }
            case 0x1A:
                new_side(v, wm_type_slots(pop(v)));
                break;
            case 0x1B:
            case 0x1C:
            {
                uint8_t t1, t2;
                if (op == 0x1Cu)
                {
                    if (read_u32(r) != 1u)
                        REJECT(r, WM_ERR_INVALID("invalid: invalid result arity"));
                    t1 = read_value_type(r);
                    pop_expect(v, WM_I32);
                    pop_expect(v, t1);
                    pop_expect(v, t1);
                    push(v, t1);
                    break;
                }
                pop_expect(v, WM_I32);
                t1 = pop(v);
                t2 = pop(v);
                if (is_ref(t1) || is_ref(t2) || (t1 != t2 && t1 != UNKNOWN && t2 != UNKNOWN))
                    REJECT(r, E_MISMATCH);
                t1 = t1 == UNKNOWN ? t2 : t1;
                new_side(v, wm_type_slots(t1));
                push(v, t1);
                break;
            }
            case 0x20:
            case 0x21:
            case 0x22:
            {
                uint8_t t;
                index = read_u32(r);
                if (index >= v->local_count)
                {
                    fail(r, WM_ERR_INVALID("invalid: unknown local"));
                    break;
                }
                t = v->locals[index];
                if (op != 0x20u)
                    pop_expect(v, t);
                if (op != 0x21u)
                    push(v, t);
                break;
            }
            case 0x23:
            case 0x24:
                index = read_u32(r);
                if (index >= m->global_count)
                {
                    fail(r, E_UNKNOWN_GLOBAL);
                    break;
                }
                if (op == 0x23u)
                    push(v, m->globals[index].type);
                else if (!m->globals[index].mutable_)
                    REJECT(r, WM_ERR_INVALID("invalid: global is immutable"));
                else
                    pop_expect(v, m->globals[index].type);
                break;
            case 0x25:
            case 0x26:
            {
                uint8_t t = table_type(v, read_table_index(v));
                if (op == 0x26u)
                    pop_expect(v, t);
                pop_expect(v, WM_I32);
                if (op == 0x25u)
                    push(v, t);
                break;
            }
            case 0x3F:
            case 0x40:
                read_zero(r);
                need_memory(v);
                if (op == 0x40u)
                    pop_expect(v, WM_I32);
                push(v, WM_I32);
                break;
            case 0x41: read_leb(r, 32u, 1); push(v, WM_I32); break;
            case 0x42: read_leb(r, 64u, 1); push(v, WM_I64); break;
            case 0x43: skip(r, 4u); push(v, WM_F32); break;
            case 0x44: skip(r, 8u); push(v, WM_F64); break;
            case 0xD0: push(v, read_ref_type(r)); break;
            case 0xD1:
            {
                uint8_t t = pop(v);
                if (t != UNKNOWN && !is_ref(t))
                    REJECT(r, E_MISMATCH);
                push(v, WM_I32);
                break;
            }
            case 0xD2:
                index = read_func_index(v->l, r);
                if (!r->error && !is_declared(v->l, index))
                    REJECT(r, WM_ERR_INVALID("invalid: undeclared function reference"));
                push(v, WM_FUNCREF);
                break;
            case 0xFC: validate_misc(v); break;
            default: fail(r, WM_ERR_MALFORMED("malformed: illegal opcode"));
        }
    }
    if (r->p != r->end)
        REJECT(r, WM_ERR_MALFORMED("malformed: section size mismatch"));
    code->max_stack = v->max_slots;
}

static void read_code(Loader* l, Reader* r, uint32_t index, Validator* v)
{
    WmModule* m = l->m;
    WmCode* code = &m->codes[index];
    const WmType* type = &m->types[m->func_types[m->func_imports + index]];
    uint32_t size = read_u32(r);
    uint32_t groups;
    uint32_t total = type->param_count;
    uint32_t slots = 0;
    uint8_t identity = 1;
    Reader body;
    Reader scan;

    body.p = skip(r, size);
    body.end = r->p;
    body.error = r->error;

    /* Count the locals first, then fill their types. */
    groups = read_u32(&body);
    scan = body;
    for (uint32_t g = 0; g < groups && !scan.error; ++g)
    {
        uint32_t count = read_u32(&scan);
        read_value_type(&scan);
        if (count > MAX_LOCALS - total)
            fail(&scan, WM_ERR_MALFORMED("malformed: too many locals"));
        else
            total += count;
    }
    if (scan.error)
    {
        fail(r, scan.error);
        return;
    }
    v->locals = (uint8_t*)wm_realloc(v->locals, total + 1u);
    if (!v->locals)
    {
        fail(r, E_MEMORY);
        return;
    }
    v->local_count = total;
    for (uint32_t i = 0; i < type->param_count; ++i)
        v->locals[i] = WM_RD8(type->params + i);
    total = type->param_count;
    for (uint32_t g = 0; g < groups; ++g)
    {
        uint32_t count = read_u32(&body);
        memset(v->locals + total, read_u8(&body), count);
        total += count;
    }
    for (uint32_t i = 0; i < total; ++i)
    {
        if (is_ref(v->locals[i]))
            m->stack_refs = 1;
        slots += wm_type_slots(v->locals[i]);
        if (wm_type_slots(v->locals[i]) != 1u)
            identity = 0;
    }
    code->local_count = total;
    code->local_slots = slots - type->param_slots;
    code->body = body.p;
    code->side = m->side_count;
    if (!identity)
    {
        uint32_t offset = 0;
        code->local_offsets = (uint32_t*)wm_malloc(((size_t)total + 1u) * sizeof(uint32_t));
        if (!code->local_offsets)
        {
            fail(r, E_MEMORY);
            return;
        }
        for (uint32_t i = 0; i < total; ++i)
        {
            code->local_offsets[i] = offset;
            offset += wm_type_slots(v->locals[i]);
        }
        code->local_offsets[total] = offset;
    }
    v->r = &body;
    v->val_count = 0;
    v->ctl_count = 0;
    v->slots = 0;
    v->max_slots = 0;
    validate_body(v, type, code);
    fail(r, body.error);
}

/* ---- Sections ---- */

static void read_types(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmType* t;
        if (read_u8(r) != 0x60u)
            REJECT(r, WM_ERR_MALFORMED("malformed: integer representation too long"));
        t = (WmType*)append(r, &m->types, &m->type_count, sizeof(WmType));
        if (!t)
            return;
        for (uint8_t results = 0; results < 2u; ++results)
        {
            uint32_t n = read_u32(r);
            const uint8_t* types = r->p;
            for (uint32_t k = 0; k < n && !r->error; ++k)
                read_value_type(r);
            if (results)
                t->results = types, t->result_count = n, t->result_slots = slots_of(types, n);
            else
                t->params = types, t->param_count = n, t->param_slots = slots_of(types, n);
            for (uint32_t k = 0; k < n; ++k)
                if (is_ref(WM_RD8(types + k)))
                    m->stack_refs = 1;
        }
    }
}

static void read_table_type(Reader* r, WmTableType* t)
{
    t->type = read_ref_type(r);
    read_limits(r, &t->limits, 0xFFFFFFFFu, NULL);
}

static void read_global_type(Reader* r, WmGlobalType* g)
{
    g->type = read_value_type(r);
    g->mutable_ = read_u8(r);
    if (g->mutable_ > 1u)
        REJECT(r, WM_ERR_MALFORMED("malformed: malformed mutability"));
}

static void add_memory(WmModule* m, Reader* r)
{
    if (m->memory_count++ > 0)
        REJECT(r, WM_ERR_INVALID("invalid: multiple memories"));
    read_limits(r, &m->memory, 65536u, E_MEMORY_SIZE);
}

static void add_function(WmModule* m, Reader* r, uint32_t type)
{
    uint32_t* slot;
    if (type >= m->type_count)
        fail(r, E_UNKNOWN_TYPE);
    slot = (uint32_t*)append(r, &m->func_types, &m->func_count, sizeof(uint32_t));
    if (slot)
        *slot = type;
}

static void read_imports(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmImport* imp = (WmImport*)append(r, &m->imports, &m->import_count, sizeof(WmImport));
        if (!imp)
            return;
        imp->module = read_name(r, &imp->module_length);
        imp->name = read_name(r, &imp->name_length);
        imp->kind = read_u8(r);
        switch (imp->kind)
        {
            case WM_EXTERN_FUNC:
                imp->index = m->func_count;
                add_function(m, r, read_u32(r));
                ++m->func_imports;
                break;
            case WM_EXTERN_TABLE:
            {
                WmTableType* t;
                imp->index = m->table_count;
                t = (WmTableType*)append(r, &m->tables, &m->table_count, sizeof(WmTableType));
                if (t)
                    read_table_type(r, t);
                ++m->table_imports;
                break;
            }
            case WM_EXTERN_MEMORY:
                imp->index = m->memory_count;
                add_memory(m, r);
                ++m->memory_imports;
                break;
            case WM_EXTERN_GLOBAL:
            {
                WmGlobalType* g;
                imp->index = m->global_count;
                g = (WmGlobalType*)append(r, &m->globals, &m->global_count, sizeof(WmGlobalType));
                if (g)
                    read_global_type(r, g);
                ++m->global_imports;
                break;
            }
            default: fail(r, WM_ERR_MALFORMED("malformed: malformed import kind"));
        }
    }
}

static void read_functions(Loader* l, Reader* r)
{
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
        add_function(l->m, r, read_u32(r));
    l->defined_funcs = count;
}

static void read_tables(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmTableType* t = (WmTableType*)append(r, &m->tables, &m->table_count, sizeof(WmTableType));
        if (t)
            read_table_type(r, t);
    }
}

static void read_memories(Loader* l, Reader* r)
{
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
        add_memory(l->m, r);
}

static void read_globals(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmGlobalType* g = (WmGlobalType*)append(r, &m->globals, &m->global_count, sizeof(WmGlobalType));
        if (!g)
            return;
        read_global_type(r, g);
        g->init = read_const(l, r, g->type);
    }
}

static void read_exports(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmExport* e = (WmExport*)append(r, &m->exports, &m->export_count, sizeof(WmExport));
        uint32_t limit;
        if (!e)
            return;
        e->name = read_name(r, &e->name_length);
        e->kind = read_u8(r);
        e->index = read_u32(r);
        switch (e->kind)
        {
            case WM_EXTERN_FUNC:
                limit = m->func_count;
                declare(l, r, e->index);
                break;
            case WM_EXTERN_TABLE: limit = m->table_count; break;
            case WM_EXTERN_MEMORY: limit = m->memory_count; break;
            case WM_EXTERN_GLOBAL: limit = m->global_count; break;
            default: fail(r, WM_ERR_MALFORMED("malformed: malformed export kind")); return;
        }
        if (e->index >= limit)
        {
            static const char* const unknown[] = {E_UNKNOWN_FUNC, E_UNKNOWN_TABLE, E_UNKNOWN_MEMORY, E_UNKNOWN_GLOBAL};
            fail(r, unknown[e->kind]);
        }
        for (uint32_t k = 0; WM_HAS_VALIDATION && k + 1u < m->export_count; ++k)
            if (wm_types_equal(m->exports[k].name, m->exports[k].name_length, e->name, e->name_length))
                REJECT(r, WM_ERR_INVALID("invalid: duplicate export name"));
    }
}

static void read_start(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    m->start = read_func_index(l, r);
    if (!r->error)
    {
        const WmType* t = &m->types[m->func_types[m->start]];
        if (t->param_count || t->result_count)
            REJECT(r, WM_ERR_INVALID("invalid: start function"));
    }
}

static void read_elems(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmElemSegment* s = (WmElemSegment*)append(r, &m->elems, &m->elem_count, sizeof(WmElemSegment));
        uint32_t flags;
        if (!s)
            return;
        flags = read_u32(r);
        if (flags > 7u)
        {
            fail(r, WM_ERR_MALFORMED("malformed: malformed elements segment kind"));
            return;
        }
        /* Bit 0: passive or declarative; bit 1: explicit table (active) or declarative; bit 2: expressions. */
        s->mode = (flags & 1u) ? ((flags & 2u) ? WM_SEGMENT_DECLARATIVE : WM_SEGMENT_PASSIVE) : WM_SEGMENT_ACTIVE;
        s->expressions = (uint8_t)((flags & 4u) != 0);
        s->type = WM_FUNCREF;
        if (s->mode == WM_SEGMENT_ACTIVE)
        {
            if (flags & 2u)
                s->table = read_u32(r);
            if (s->table >= m->table_count)
            {
                fail(r, E_UNKNOWN_TABLE);
                return;
            }
            s->offset = read_const(l, r, WM_I32);
        }
        /* An element kind (0: funcref) or a reference type. */
        if (flags & 3u)
        {
            if (s->expressions)
                s->type = read_ref_type(r);
            else if (read_u8(r) != 0)
                REJECT(r, WM_ERR_MALFORMED("malformed: malformed element kind"));
        }
        s->count = read_u32(r);
        s->items = r->p;
        for (uint32_t k = 0; k < s->count && !r->error; ++k)
        {
            if (s->expressions)
                read_const(l, r, s->type);
            else
                declare(l, r, read_func_index(l, r));
        }
        if (s->mode == WM_SEGMENT_ACTIVE && m->tables[s->table].type != s->type)
            REJECT(r, E_MISMATCH);
    }
}

static void read_codes(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    Validator v;
    if (count != l->defined_funcs)
    {
        fail(r, WM_ERR_MALFORMED("malformed: function and code section have inconsistent lengths"));
        return;
    }
    m->codes = (WmCode*)wm_calloc(count + 1u, sizeof(WmCode));
    if (!m->codes)
    {
        fail(r, E_MEMORY);
        return;
    }
    memset(&v, 0, sizeof(v));
    v.l = l;
    v.m = m;
    for (uint32_t i = 0; i < count && !r->error; ++i)
        read_code(l, r, i, &v);
    wm_free(v.vals);
    wm_free(v.ctls);
    wm_free(v.locals);
}

static void read_datas(Loader* l, Reader* r)
{
    WmModule* m = l->m;
    uint32_t count = read_u32(r);
    if (l->has_data_count && count != l->data_count)
        REJECT(r, WM_ERR_MALFORMED("malformed: data count and data section have inconsistent lengths"));
    for (uint32_t i = 0; i < count && !r->error; ++i)
    {
        WmDataSegment* s = (WmDataSegment*)append(r, &m->datas, &m->data_count, sizeof(WmDataSegment));
        uint32_t flags;
        if (!s)
            return;
        flags = read_u32(r);
        if (flags > 2u)
            fail(r, WM_ERR_MALFORMED("malformed: malformed data segment kind"));
        s->mode = flags == 1u ? WM_SEGMENT_PASSIVE : WM_SEGMENT_ACTIVE;
        if (s->mode == WM_SEGMENT_ACTIVE)
        {
            if ((flags == 2u ? read_u32(r) : 0) >= m->memory_count)
                fail(r, E_UNKNOWN_MEMORY);
            s->offset = read_const(l, r, WM_I32);
        }
        s->size = read_u32(r);
        s->data = skip(r, s->size);
    }
}

/* Where each known section may appear: 1 type ... 12 data count, between element and code. */
static const uint8_t WM_ROM SECTION_ORDER[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 10};

typedef void (*SectionReader)(Loader* l, Reader* r);

static void read_data_count(Loader* l, Reader* r)
{
    l->has_data_count = 1;
    l->data_count = read_u32(r);
}

static void read_custom(Loader* l, Reader* r)
{
    uint32_t length;
    const uint8_t* name = read_name(r, &length);
    (void)l, (void)name;
#if WM_HAS_TRACE
    if (!r->error && wm_bytes_equal(name, length, (const uint8_t*)"name", 4u))
    {
        l->m->names = (uint32_t)(r->p - l->m->bytes);
        l->m->names_size = (uint32_t)(r->end - r->p);
    }
#endif
    r->p = r->end;
}

static const SectionReader SECTIONS[] = {read_custom,  read_types,   read_imports, read_functions, read_tables,
                                         read_memories, read_globals, read_exports, read_start,     read_elems,
                                         read_codes,   read_datas,   read_data_count};

static WmResult load(Loader* l)
{
    WmModule* m = l->m;
    Reader r = {m->bytes, m->bytes + m->size, NULL};
    uint8_t last = 0;
    uint8_t has_codes = 0;
    static const uint8_t header[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    if (m->size < 4u || !wm_bytes_equal(m->bytes, 4u, header, 4u))
        return WM_ERR_MALFORMED("malformed: magic header not detected");
    if (m->size < 8u || !wm_bytes_equal(m->bytes + 4, 4u, header + 4, 4u))
        return WM_ERR_MALFORMED("malformed: unknown binary version");
    r.p += 8;
    while (r.p < r.end && !r.error)
    {
        uint8_t id = read_u8(&r);
        uint32_t size = read_u32(&r);
        Reader section;
        if (r.error)
            break;
        if ((uint32_t)(r.end - r.p) < size)
            return WM_ERR_MALFORMED("malformed: length out of bounds");
        section.p = r.p;
        section.end = r.p + size;
        section.error = NULL;
        r.p += size;
        if (id >= sizeof(SECTION_ORDER))
            return WM_ERR_MALFORMED("malformed: malformed section id");
        if (id != 0)
        {
            uint8_t order = WM_ROM8(SECTION_ORDER + id);
            if (order <= last)
                return WM_ERR_MALFORMED("malformed: unexpected content after last section");
            last = order;
        }
        has_codes |= id == 10u;
        SECTIONS[id](l, &section);
        if (!section.error && section.p != section.end)
            REJECT(&section, WM_ERR_MALFORMED("malformed: section size mismatch"));
        fail(&r, section.error);
    }
    if (!has_codes && l->defined_funcs != 0)
        fail(&r, WM_ERR_MALFORMED("malformed: function and code section have inconsistent lengths"));
    if (l->has_data_count && l->data_count != m->data_count)
        REJECT(&r, WM_ERR_MALFORMED("malformed: data count and data section have inconsistent lengths"));
    return r.error;
}

WmResult wm_module_load(WmModule** out, const uint8_t* bytes, uint32_t size)
{
    Loader l;
    WmResult result;
    *out = NULL;
    memset(&l, 0, sizeof(l));
    l.m = (WmModule*)wm_calloc(1, sizeof(WmModule));
    if (!l.m)
        return E_MEMORY;
    if (size >= WM_SIDE_MAX)
        return WM_ERR_UNSUPPORTED("unsupported: program too big for the side table");
    l.m->bytes = bytes;
    l.m->size = size;
    l.m->start = WM_NONE;
    result = load(&l);
    wm_free(l.declared);
    if (result)
    {
        wm_module_free(l.m);
        return result;
    }
    *out = l.m;
    return NULL;
}

void wm_module_free(WmModule* m)
{
    if (!m)
        return;
    if (m->codes)
        for (uint32_t i = 0; i < m->func_count - m->func_imports; ++i)
        {
            wm_free(m->codes[i].local_offsets);
            wm_free(m->codes[i].fast);
        }
    wm_free(m->types);
    wm_free(m->imports);
    wm_free(m->func_types);
    wm_free(m->codes);
    wm_free(m->tables);
    wm_free(m->globals);
    wm_free(m->exports);
    wm_free(m->elems);
    wm_free(m->datas);
    wm_free(m->side);
    wm_free(m);
}

uint32_t wm_module_code_bytes(const WmModule* m)
{
    size_t bytes;
    if (!m)
        return 0;
    bytes = m->side ? (size_t)m->side_count * sizeof(WmSide) : 0u;
    if (m->codes)
        for (uint32_t i = 0; i < m->func_count - m->func_imports; ++i)
        {
            const WmCode* c = &m->codes[i];
            if (c->local_offsets)
                bytes += ((size_t)c->local_count + 1u) * sizeof(uint32_t);
            if (c->fast)
                bytes += (size_t)c->fast_cells * sizeof(WmCell);
        }
    return (uint32_t)bytes;
}

#if WM_PREDECODE
/*
 * Predecoding only speeds the module up: it is done once, when the first
 * instance has its memory, and without the memory for the records the
 * module runs in place, with its side table.
 */
void wm_module_predecode(WmModule* m)
{
    WmResult predecoded = NULL;
    uint32_t defined = m->func_count - m->func_imports;
    if (m->predecode_tried)
        return;
    m->predecode_tried = 1;
    for (uint32_t i = 0; !predecoded && i < defined; ++i)
        predecoded = wm_predecode(m, &m->codes[i]);
    if (predecoded)
        for (uint32_t i = 0; i < defined; ++i)
        {
            wm_free(m->codes[i].fast);
            m->codes[i].fast = NULL;
            m->codes[i].fast_cells = 0;
        }
    else
    {
        /* The records hold every branch target: the side table's entries are no longer needed, only their count. */
        wm_free(m->side);
        m->side = NULL;
    }
}
#endif
