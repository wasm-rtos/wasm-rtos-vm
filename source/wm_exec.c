/*
 * The interpreter: one loop over the module's own bytes. Branches take
 * their target from the side table, which advances with the code: every
 * instruction that owns an entry either jumps by it or steps past it.
 *
 * Numeric instructions share one path: wm_numeric says what each takes and
 * gives, so taking operands and pushing results is written once, and
 * small functions per operand type only compute.
 */
#include "wm_internal.h"

#include <math.h>
#include <stdlib.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "wasm-rtos-vm needs a little-endian target"
#endif

WmResult wm_table_init(WmInstance* inst, uint32_t segment, WmTable* table, uint32_t dst, uint32_t src, uint32_t count);

#define T_OOB_MEMORY WM_ERR_TRAP("trap: out of bounds memory access")
#define T_OOB_TABLE WM_ERR_TRAP("trap: out of bounds table access")
#define T_DIVIDE WM_ERR_TRAP("trap: integer divide by zero")
#define T_OVERFLOW WM_ERR_TRAP("trap: integer overflow")
#define T_STACK WM_ERR_EXHAUSTION("exhaustion: call stack exhausted")

/* Memory accesses 0x28-0x3E: log2 of the size, 4 sign-extends, 8 is a 64-bit value. */
static const uint8_t WM_ROM ACCESS[] = {
    0x02, 0x0B, 0x02, 0x0B, 0x04, 0x00, 0x05, 0x01, 0x0C, 0x08, 0x0D, 0x09, 0x0E, 0x0A, /* loads */
    0x02, 0x0B, 0x02, 0x0B, 0x00, 0x01, 0x08, 0x09, 0x0A,                               /* stores */
};

/* Immediates of a validated module: no checks needed. Most fit in one byte, so that case comes first. */
static WM_NOINLINE uint32_t imm_u32_long(const uint8_t** p)
{
    uint32_t value = 0;
    uint8_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    return value;
}

static WM_IMM uint32_t imm_u32(const uint8_t** p)
{
    uint8_t byte = WM_RD8(*p);
    if (byte < 0x80u)
    {
        ++*p;
        return byte;
    }
    return imm_u32_long(p);
}

static uint64_t imm_signed(const uint8_t** p)
{
    uint64_t value = 0;
    uint8_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    if (shift < 64u && (byte & 0x40u))
        value |= ~(uint64_t)0 << shift;
    return value;
}

/* i32.const in 32-bit arithmetic: no 64-bit work on 32- and 8-bit chips. */
static WM_NOINLINE uint32_t imm_s32_long(const uint8_t** p)
{
    uint32_t value = 0;
    uint8_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        if (shift < 32u)
            value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    if (shift < 32u && (byte & 0x40u))
        value |= ~(uint32_t)0 << shift;
    return value;
}

static WM_IMM uint32_t imm_s32(const uint8_t** p)
{
    uint8_t byte = WM_RD8(*p);
    if (byte < 0x80u)
    {
        ++*p;
        return (byte & 0x40u) ? (uint32_t)byte | 0xFFFFFF80u : byte;
    }
    return imm_s32_long(p);
}

static WM_IMM void skip_block_type(const uint8_t** p)
{
    /* 0x40 and value types are one byte from 0x40 up; a type index is a LEB number. */
    if (WM_RD8(*p) >= 0x40u && WM_RD8(*p) < 0x80u)
        ++*p;
    else
        imm_signed(p);
}

static WM_IMM uint64_t get64(const uint32_t* p)
{
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static WM_IMM void put64(uint32_t* p, uint64_t v) { __builtin_memcpy(p, &v, 8); }

#if WM_HAS_REF_TYPES
static WM_IMM uintptr_t get_ref(const uint32_t* p)
{
    uintptr_t r;
    __builtin_memcpy(&r, p, sizeof(r));
    return r;
}

static WM_IMM void put_ref(uint32_t* p, uintptr_t r)
{
    for (uint32_t i = 0; i < WM_REF_SLOTS; ++i)
        p[i] = 0;
    __builtin_memcpy(p, &r, sizeof(r));
}

#endif

/*
 * Slot copies are a few words: plain loops, not library calls, which some
 * ports cannot inline (ESP32 builds with -fno-builtin-memcpy). Moves only
 * ever go down the stack, so copying forward is safe.
 */
static WM_IMM void copy_slots(uint32_t* dst, const uint32_t* src, uint32_t n)
{
    while (n-- > 0)
        *dst++ = *src++;
}

static WM_IMM void zero_slots(uint32_t* dst, uint32_t n)
{
    while (n-- > 0)
        *dst++ = 0;
}

/* Little-endian memory of any access size, aligned or not. */
static WM_IMM uint64_t load_bytes(const uint8_t* p, uint8_t size)
{
    uint64_t v = 0;
    switch (size)
    {
        case 1: v = *p; break;
        case 2: { uint16_t x; __builtin_memcpy(&x, p, 2); v = x; break; }
        case 4: { uint32_t x; __builtin_memcpy(&x, p, 4); v = x; break; }
        default: __builtin_memcpy(&v, p, 8);
    }
    return v;
}

static WM_IMM void store_bytes(uint8_t* p, uint64_t v, uint8_t size)
{
    switch (size)
    {
        case 1: *p = (uint8_t)v; break;
        case 2: { uint16_t x = (uint16_t)v; __builtin_memcpy(p, &x, 2); break; }
        case 4: { uint32_t x = (uint32_t)v; __builtin_memcpy(p, &x, 4); break; }
        default: __builtin_memcpy(p, &v, 8);
    }
}

#if WM_HAS_MMU
/*
 * A memory access through the memory functions, for paged memory off the
 * fast path (unaligned, across frames, or no TLB to cache it). sp is before
 * the access; the result is sp after it, or NULL with *trap set.
 */
static uint32_t* paged_access(WmMemory* memory, uint8_t op, uint32_t offset, uint32_t* sp, WmResult* trap)
{
    uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    uint8_t bytes[8];
    uint64_t value = 0;
    uint64_t ea;
    if (op >= 0x36u)
    {
        if (d & 8u)
        {
            sp -= 2;
            value = get64(sp);
        }
        else
            value = *--sp;
    }
    ea = (uint64_t)*--sp + offset;
    if (ea > 0xFFFFFFFFu)
    {
        *trap = T_OOB_MEMORY;
        return NULL;
    }
    if (op >= 0x36u)
    {
        for (uint32_t i = 0; i < size; ++i)
            bytes[i] = (uint8_t)(value >> (8u * i));
        *trap = wm_memory_write(memory, (uint32_t)ea, bytes, size);
        return *trap ? NULL : sp;
    }
    if ((*trap = wm_memory_read(memory, (uint32_t)ea, bytes, size)))
        return NULL;
    for (uint32_t i = 0; i < size; ++i)
        value |= (uint64_t)bytes[i] << (8u * i);
    if (d & 4u)
    {
        uint8_t shift = (uint8_t)(64u - 8u * size);
        value = (uint64_t)((int64_t)(value << shift) >> shift);
    }
    if (d & 8u)
    {
        put64(sp, value);
        sp += 2;
    }
    else
        *sp++ = (uint32_t)value;
    return sp;
}
#endif

static uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clzl(x) - (uint32_t)(sizeof(long) * 8u - 32u) : 32u; }
static uint32_t ctz32(uint32_t x) { return x ? (uint32_t)__builtin_ctzl(x) : 32u; }

/* Ops whose operands are i32: results by wm_numeric. */
WM_OP uint32_t* op_i32(uint8_t op, uint8_t d, uint32_t* sp, WmResult* trap)
{
    uint32_t b = (d & WM_NUM_BINARY) ? *--sp : 0;
    uint32_t a = *--sp;
    uint64_t r;
    uint8_t n = (uint8_t)(b & 31u);
    switch (op)
    {
        case 0x45: r = a == 0; break;
        case 0x46: r = a == b; break;
        case 0x47: r = a != b; break;
        case 0x48: r = (int32_t)a < (int32_t)b; break;
        case 0x49: r = a < b; break;
        case 0x4A: r = (int32_t)a > (int32_t)b; break;
        case 0x4B: r = a > b; break;
        case 0x4C: r = (int32_t)a <= (int32_t)b; break;
        case 0x4D: r = a <= b; break;
        case 0x4E: r = (int32_t)a >= (int32_t)b; break;
        case 0x4F: r = a >= b; break;
        case 0x67: r = clz32(a); break;
        case 0x68: r = ctz32(a); break;
        case 0x69: r = (uint32_t)__builtin_popcountl(a); break;
        case 0x6A: r = a + b; break;
        case 0x6B: r = a - b; break;
        case 0x6C: r = a * b; break;
        case 0x6D:
        case 0x6E:
        case 0x6F:
        case 0x70:
            if (b == 0)
            {
                *trap = T_DIVIDE;
                return NULL;
            }
            if (op == 0x6Du && a == 0x80000000u && b == 0xFFFFFFFFu)
            {
                *trap = T_OVERFLOW;
                return NULL;
            }
            if (op == 0x6Du)
                r = (uint32_t)((int32_t)a / (int32_t)b);
            else if (op == 0x6Eu)
                r = a / b;
            else if (op == 0x6Fu)
                r = b == 0xFFFFFFFFu ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
            else
                r = a % b;
            break;
        case 0x71: r = a & b; break;
        case 0x72: r = a | b; break;
        case 0x73: r = a ^ b; break;
        case 0x74: r = a << n; break;
        case 0x75: r = (uint32_t)((int32_t)a >> n); break;
        case 0x76: r = a >> n; break;
        case 0x77: r = (uint32_t)(a << n) | (n ? a >> (32u - n) : 0); break;
        case 0x78: r = (uint32_t)(a >> n) | (n ? a << (32u - n) : 0); break;
        case 0xAC: r = (uint64_t)(int64_t)(int32_t)a; break;
        case 0xC0: r = (uint32_t)(int32_t)(int8_t)a; break;
        case 0xC1: r = (uint32_t)(int32_t)(int16_t)a; break;
#if WM_HAS_FLOAT
        case 0xB2:
        case 0xB3:
        {
            float f = op == 0xB2u ? (float)(int32_t)a : (float)a;
            uint32_t bits;
            __builtin_memcpy(&bits, &f, 4);
            r = bits;
            break;
        }
        case 0xB7:
        case 0xB8:
        {
            double f = op == 0xB7u ? (double)(int32_t)a : (double)a;
            __builtin_memcpy(&r, &f, 8);
            break;
        }
#endif
        default: r = a; /* i64.extend_i32_u, f32.reinterpret_i32 */
    }
    if (d & WM_NUM_WIDE)
    {
        put64(sp, r);
        return sp + 2;
    }
    *sp = (uint32_t)r;
    return sp + 1;
}

#if WM_HAS_I64
/* Ops whose operands are i64. */
WM_OP uint32_t* op_i64(uint8_t op, uint8_t d, uint32_t* sp, WmResult* trap)
{
    uint64_t b = 0;
    uint64_t a;
    uint64_t r;
    uint8_t n;
    if (d & WM_NUM_BINARY)
    {
        sp -= 2;
        b = get64(sp);
    }
    sp -= 2;
    a = get64(sp);
    n = (uint8_t)(b & 63u);
    switch (op)
    {
        case 0x50: r = a == 0; break;
        case 0x51: r = a == b; break;
        case 0x52: r = a != b; break;
        case 0x53: r = (int64_t)a < (int64_t)b; break;
        case 0x54: r = a < b; break;
        case 0x55: r = (int64_t)a > (int64_t)b; break;
        case 0x56: r = a > b; break;
        case 0x57: r = (int64_t)a <= (int64_t)b; break;
        case 0x58: r = a <= b; break;
        case 0x59: r = (int64_t)a >= (int64_t)b; break;
        case 0x5A: r = a >= b; break;
        case 0x79: r = a ? (uint64_t)__builtin_clzll(a) : 64u; break;
        case 0x7A: r = a ? (uint64_t)__builtin_ctzll(a) : 64u; break;
        case 0x7B: r = (uint64_t)__builtin_popcountll(a); break;
        case 0x7C: r = a + b; break;
        case 0x7D: r = a - b; break;
        case 0x7E: r = a * b; break;
        case 0x7F:
        case 0x80:
        case 0x81:
        case 0x82:
            if (b == 0)
            {
                *trap = T_DIVIDE;
                return NULL;
            }
            if (op == 0x7Fu && a == 0x8000000000000000u && b == UINT64_MAX)
            {
                *trap = T_OVERFLOW;
                return NULL;
            }
            if (op == 0x7Fu)
                r = (uint64_t)((int64_t)a / (int64_t)b);
            else if (op == 0x80u)
                r = a / b;
            else if (op == 0x81u)
                r = b == UINT64_MAX ? 0 : (uint64_t)((int64_t)a % (int64_t)b);
            else
                r = a % b;
            break;
        case 0x83: r = a & b; break;
        case 0x84: r = a | b; break;
        case 0x85: r = a ^ b; break;
        case 0x86: r = a << n; break;
        case 0x87: r = (uint64_t)((int64_t)a >> n); break;
        case 0x88: r = a >> n; break;
        case 0x89: r = (a << n) | (n ? a >> (64u - n) : 0); break;
        case 0x8A: r = (a >> n) | (n ? a << (64u - n) : 0); break;
        case 0xC2: r = (uint64_t)(int64_t)(int8_t)a; break;
        case 0xC3: r = (uint64_t)(int64_t)(int16_t)a; break;
        case 0xC4: r = (uint64_t)(int64_t)(int32_t)a; break;
#if WM_HAS_FLOAT
        case 0xB4:
        case 0xB5:
        {
            float f = op == 0xB4u ? (float)(int64_t)a : (float)a;
            uint32_t bits;
            __builtin_memcpy(&bits, &f, 4);
            r = bits;
            break;
        }
        case 0xB9:
        case 0xBA:
        {
            double f = op == 0xB9u ? (double)(int64_t)a : (double)a;
            __builtin_memcpy(&r, &f, 8);
            break;
        }
#endif
        default: r = a; /* i32.wrap_i64, f64.reinterpret_i64 */
    }
    if (d & WM_NUM_WIDE)
    {
        put64(sp, r);
        return sp + 2;
    }
    *sp = (uint32_t)r;
    return sp + 1;
}

#endif
#if WM_HAS_FLOAT
/* Float to integer by kind: 0 i32, 1 u32, 2 i64, 3 u64. NaN and values out of range trap or saturate. */
static WmResult to_integer(double x, uint8_t kind, int saturate, uint64_t* out)
{
    static const double low[] = {-2147483649.0, -1.0, 0.0, -1.0};
    static const double high[] = {2147483648.0, 4294967296.0, 9223372036854775808.0, 18446744073709551616.0};
    static const uint64_t min[] = {0x80000000u, 0, 0x8000000000000000u, 0};
    static const uint64_t max[] = {0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFFFFFFFFFu, UINT64_MAX};
    if (x != x)
    {
        if (!saturate)
            return WM_ERR_TRAP("trap: invalid conversion to integer");
        *out = 0;
        return NULL;
    }
    /* i64's lower bound is itself representable, so it is the one compared with <. */
    if (kind == 2u ? x < -9223372036854775808.0 : x <= low[kind])
    {
        if (!saturate)
            return T_OVERFLOW;
        *out = min[kind];
        return NULL;
    }
    if (x >= high[kind])
    {
        if (!saturate)
            return T_OVERFLOW;
        *out = max[kind];
        return NULL;
    }
    if (kind == 0u)
        *out = (uint32_t)(int32_t)x;
    else if (kind == 1u)
        *out = (uint32_t)x;
    else if (kind == 2u)
        *out = (uint64_t)(int64_t)x;
    else
        *out = (uint64_t)x;
    return NULL;
}

/* min and max: a NaN in gives a NaN out, and -0 is less than +0. */
static double min_max(double a, double b, int max)
{
    if (a != a || b != b)
        return a + b;
    if (a == 0.0 && b == 0.0)
        return (signbit(a) != 0) == max ? b : a;
    return (a < b) != max ? a : b;
}

/*
 * Ops whose operands are f32 or f64. f32 values are widened to double,
 * which is exact, and every f32 result is computed in float. Saturating
 * conversions come as 0xE0 plus their 0xFC sub-opcode.
 */
WM_OP uint32_t* op_float(uint8_t op, uint8_t d, uint32_t* sp, WmResult* trap)
{
    int wide = (d & 3u) == WM_NUM_F64;
    uint64_t ab = 0;
    uint64_t aa;
    double a;
    double b = 0;
    uint64_t r = 0;
    double f = 0;
    int is_float = 0;
    uint8_t kind = 0;
    int saturate = 0;
    if (d & WM_NUM_BINARY)
    {
        sp -= wide ? 2 : 1;
        ab = wide ? get64(sp) : *sp;
    }
    sp -= wide ? 2 : 1;
    aa = wide ? get64(sp) : *sp;
    if (wide)
    {
        __builtin_memcpy(&a, &aa, 8);
        __builtin_memcpy(&b, &ab, 8);
    }
    else
    {
        float fa, fb;
        uint32_t ua = (uint32_t)aa, ub = (uint32_t)ab;
        __builtin_memcpy(&fa, &ua, 4);
        __builtin_memcpy(&fb, &ub, 4);
        a = fa;
        b = fb;
    }
    /* f64 compares and arithmetic sit a fixed distance after f32's. */
    switch (wide && op >= 0x61u && op <= 0x66u ? op - 6u : wide && op >= 0x99u && op <= 0xA6u ? op - 14u : op)
    {
        case 0x5B: r = a == b; break;
        case 0x5C: r = a != b; break;
        case 0x5D: r = a < b; break;
        case 0x5E: r = a > b; break;
        case 0x5F: r = a <= b; break;
        case 0x60: r = a >= b; break;
        case 0x8B: r = aa & (wide ? 0x7FFFFFFFFFFFFFFFu : 0x7FFFFFFFu); break;
        case 0x8C: r = aa ^ (wide ? 0x8000000000000000u : 0x80000000u); break;
        case 0x98:
        {
            uint64_t sign = wide ? 0x8000000000000000u : 0x80000000u;
            r = (aa & ~sign) | (ab & sign);
            break;
        }
        case 0x8D: f = wide ? ceil(a) : ceilf((float)a), is_float = 1; break;
        case 0x8E: f = wide ? floor(a) : floorf((float)a), is_float = 1; break;
        case 0x8F: f = wide ? trunc(a) : truncf((float)a), is_float = 1; break;
        case 0x90: f = wide ? nearbyint(a) : nearbyintf((float)a), is_float = 1; break;
        case 0x91: f = wide ? sqrt(a) : sqrtf((float)a), is_float = 1; break;
        case 0x92: f = wide ? a + b : (double)((float)a + (float)b), is_float = 1; break;
        case 0x93: f = wide ? a - b : (double)((float)a - (float)b), is_float = 1; break;
        case 0x94: f = wide ? a * b : (double)((float)a * (float)b), is_float = 1; break;
        case 0x95: f = wide ? a / b : (double)((float)a / (float)b), is_float = 1; break;
        case 0x96: f = min_max(a, b, 0), is_float = 1; break;
        case 0x97: f = min_max(a, b, 1), is_float = 1; break;
        case 0xA8: case 0xAA: kind = 0; goto convert;
        case 0xA9: case 0xAB: kind = 1; goto convert;
        case 0xAE: case 0xB0: kind = 2; goto convert;
        case 0xAF: case 0xB1: kind = 3; goto convert;
        case 0xE0: case 0xE2: kind = 0, saturate = 1; goto convert;
        case 0xE1: case 0xE3: kind = 1, saturate = 1; goto convert;
        case 0xE4: case 0xE6: kind = 2, saturate = 1; goto convert;
        case 0xE5: case 0xE7: kind = 3, saturate = 1; goto convert;
        case 0xB6: /* f32.demote_f64 */
        case 0xBB: /* f64.promote_f32 */
            f = a, is_float = 1;
            break;
        default: r = aa; /* reinterpret */
    }
    if (0)
    {
    convert:
        if ((*trap = to_integer(a, kind, saturate, &r)))
            return NULL;
    }
    if (is_float)
    {
        /* The result type decides the width. */
        if (d & WM_NUM_WIDE)
            __builtin_memcpy(&r, &f, 8);
        else
        {
            float g = (float)f;
            uint32_t bits;
            __builtin_memcpy(&bits, &g, 4);
            r = bits;
        }
    }
    if (d & WM_NUM_WIDE)
    {
        put64(sp, r);
        return sp + 2;
    }
    *sp = (uint32_t)r;
    return sp + 1;
}
#endif

static uint32_t grow_table(WmTable* t, uint32_t delta, uintptr_t init)
{
    uint32_t old = t->size;
    uint32_t size = old + delta;
    uintptr_t* elems;
    if (delta > 10000000u || size > 10000000u || (t->max != WM_NONE && size > t->max))
        return WM_NONE;
    elems = (uintptr_t*)wm_realloc(t->elems, ((size_t)size + 1u) * sizeof(uintptr_t));
    if (!elems)
        return WM_NONE;
    for (uint32_t i = old; i < size; ++i)
        elems[i] = init;
    t->elems = elems;
    t->size = size;
    return old;
}

uint32_t wm_table_grow(WmTable* table, uint32_t delta) { return grow_table(table, delta, 0); }

/* The bulk memory and table instructions after 0xFC: all but the saturating conversions. */
static WmResult misc(WmInstance* inst, uint32_t sub, const uint8_t** pc, uint32_t** stack)
{
    uint32_t* sp = *stack;
    WmMemory* memory = inst->memory;
    wm_size mem_size = memory ? WM_MEMORY_SIZE(memory) : 0;
    uint32_t a = 0;
    WmResult result = NULL;
#if WM_HAS_REF_TYPES
    uint32_t b;
    WmTable* t = NULL;
#endif
    if (sub == 8u || sub == 9u || sub == 12u || sub == 13u)
        a = imm_u32(pc);
#if WM_HAS_REF_TYPES
    if (sub >= 12u && sub != 13u)
        t = inst->tables[imm_u32(pc)];
#endif
    switch (sub)
    {
        case 8: /* memory.init */
        {
            const WmDataSegment* s = &inst->module->datas[a];
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            uint32_t size = inst->data_dropped[a] ? 0 : s->size;
            ++*pc;
            sp -= 3;
            if (wm_out_of_bounds(src, n, size) || wm_out_of_bounds(dst, n, mem_size))
                return T_OOB_MEMORY;
#if WM_HAS_MMU
            if ((result = wm_memory_write(memory, dst, s->data + src, n)))
                return result;
#else
            for (uint32_t i = 0; i < n; ++i)
                memory->data[dst + i] = WM_RD8(s->data + src + i);
#endif
            break;
        }
        case 9: inst->data_dropped[a] = 1; break;
        case 10: /* memory.copy */
        case 11: /* memory.fill */
        {
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            *pc += sub == 10u ? 2 : 1;
            sp -= 3;
            if (wm_out_of_bounds(dst, n, mem_size) || (sub == 10u && wm_out_of_bounds(src, n, mem_size)))
                return T_OOB_MEMORY;
#if WM_HAS_MMU
            if ((result = sub == 10u ? wm_memory_copy(memory, dst, src, n) : wm_memory_fill(memory, dst, (uint8_t)src, n)))
                return result;
#else
            if (sub == 10u)
                memmove(memory->data + dst, memory->data + src, n);
            else
                memset(memory->data + dst, (int)(uint8_t)src, n);
#endif
            break;
        }
#if WM_HAS_REF_TYPES
        case 12: /* table.init */
            sp -= 3;
            result = wm_table_init(inst, a, t, sp[0], sp[1], sp[2]);
            break;
        case 13: inst->elem_dropped[a] = 1; break;
        case 14: /* table.copy */
        {
            WmTable* src_table = inst->tables[imm_u32(pc)];
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            sp -= 3;
            if (wm_out_of_bounds(src, n, src_table->size) || wm_out_of_bounds(dst, n, t->size))
                return T_OOB_TABLE;
            memmove(t->elems + dst, src_table->elems + src, (size_t)n * sizeof(uintptr_t));
            break;
        }
        case 15: /* table.grow */
            b = *--sp;
            sp -= WM_REF_SLOTS;
            *sp = grow_table(t, b, get_ref(sp));
            ++sp;
            break;
        case 16: *sp++ = t->size; break;
        case 17: /* table.fill */
        {
            uintptr_t value;
            b = *--sp;
            sp -= WM_REF_SLOTS;
            value = get_ref(sp);
            a = *--sp;
            if (wm_out_of_bounds(a, b, t->size))
                return T_OOB_TABLE;
            for (uint32_t k = 0; k < b; ++k)
                t->elems[a + k] = value;
            break;
        }
#endif
    }
    *stack = sp;
    return result;
}

/*
 * A trap's exit: with WM_HAS_TRACE it notes the function and the calls it was
 * in. After a host function, a trap in a call it made is already noted.
 */
#if WM_HAS_TRACE
#define TRAP(e)                                                                                                \
    do                                                                                                         \
    {                                                                                                          \
        rt->trap_func = func;                                                                                  \
        rt->trap_depth = rt->depth;                                                                            \
        return (e);                                                                                            \
    } while (0)
#define TRACE_CLEAR() (rt->trap_func = NULL)
#define TRAP_AFTER_HOST(e)                                                                                     \
    do                                                                                                         \
    {                                                                                                          \
        if (!rt->trap_func)                                                                                    \
            TRAP(e);                                                                                           \
        return (e);                                                                                            \
    } while (0)
#else
#define TRAP(e) return (e)
#define TRACE_CLEAR() ((void)0)
#define TRAP_AFTER_HOST(e) return (e)
#endif

#if WM_PREDECODE
/*
 * The fast loop: predecoded records (wm_fast.c), one per instruction, each
 * starting with its handler's address. Called with rt NULL it only hands
 * out the handlers' addresses for the predecoder.
 */
static const uint16_t* g_fast_handlers;
static uint16_t g_fast_offsets[WM_F_COUNT];
/* From the first handler's label to the lowest one: records count handlers from there. */
static ptrdiff_t g_fast_shift;

#if WM_HAS_MMU
/* The fast loop stopped for a TLB miss: not an end of the call, only of its slice. */
static const char wm_page_stop[] = "page fault";

/*
 * The miss the fast loop stopped at: make the frame and fill the TLB, so the
 * record runs again; or make the access here, through the memory functions,
 * and step past the record. NULL to go on, or a trap.
 */
static WmResult page_fault(WmRuntime* rt)
{
    WmMemory* memory = rt->saved.func->instance->memory;
    uint8_t size = (uint8_t)(1u << (WM_ROM8(ACCESS + (rt->fault_op - 0x28u)) & 3u));
    WmResult result = wm_page_fault(rt->tlb, memory, rt->fault_address, rt->fault_offset, size, rt->fault_op >= 0x36u);
    if (result == wm_page_emulate)
    {
        WmResult trap = NULL;
        uint32_t* sp = paged_access(memory, rt->fault_op, rt->fault_offset, rt->saved.sp, &trap);
        if (!sp)
            result = trap;
        else
        {
            rt->saved.sp = sp;
            rt->saved.pc += rt->fault_cells * sizeof(WmCell);
            /* The record ran: its fuel is spent. */
            if (rt->fuel_on && rt->nesting == 1u)
                --rt->fuel;
            return NULL;
        }
    }
#if WM_HAS_TRACE
    if (result && result != wm_page_wait)
    {
        rt->trap_func = rt->saved.func;
        rt->trap_depth = rt->depth;
    }
#endif
    return result;
}
#endif

WM_HOT static WmResult run_fast(WmRuntime* rt, WmFunc* func, int resume)
{
    static const void* const labels[WM_F_COUNT] = {
        [WM_F_UNREACHABLE] = &&f_unreachable, [WM_F_NOP] = &&f_nop, [WM_F_IF] = &&f_if, [WM_F_JUMP] = &&f_jump,
        [WM_F_BR] = &&f_br, [WM_F_BR_IF] = &&f_br_if, [WM_F_BR_TABLE] = &&f_br_table, [WM_F_RETURN] = &&f_return,
        [WM_F_CALL] = &&f_call, [WM_F_CALL_INDIRECT] = &&f_call_indirect, [WM_F_DROP] = &&f_drop,
        [WM_F_SELECT] = &&f_select, [WM_F_LOCAL_GET1] = &&f_local_get1, [WM_F_LOCAL_SET1] = &&f_local_set1,
        [WM_F_LOCAL_TEE1] = &&f_local_tee1, [WM_F_LOCAL_GET] = &&f_local_get, [WM_F_LOCAL_SET] = &&f_local_set,
        [WM_F_LOCAL_TEE] = &&f_local_tee, [WM_F_GLOBAL_GET] = &&f_global_get, [WM_F_GLOBAL_SET] = &&f_global_set,
        [WM_F_TABLE_GET] = &&f_table_get, [WM_F_TABLE_SET] = &&f_table_set, [WM_F_ACCESS] = &&f_access,
        [WM_F_I32_LOAD] = &&f_i32_load, [WM_F_I32_STORE] = &&f_i32_store, [WM_F_MEMORY_SIZE] = &&f_memory_size,
        [WM_F_MEMORY_GROW] = &&f_memory_grow, [WM_F_CONST16] = &&f_const16, [WM_F_CONST32] = &&f_const32,
        [WM_F_CONST64] = &&f_const64,
        [WM_F_NUMERIC] = &&f_numeric, [WM_F_EQZ] = &&f_eqz, [WM_F_EQ] = &&f_eq, [WM_F_NE] = &&f_ne,
        [WM_F_LT_S] = &&f_lt_s, [WM_F_LT_U] = &&f_lt_u, [WM_F_GT_S] = &&f_gt_s, [WM_F_GT_U] = &&f_gt_u,
        [WM_F_LE_S] = &&f_le_s, [WM_F_LE_U] = &&f_le_u, [WM_F_GE_S] = &&f_ge_s, [WM_F_GE_U] = &&f_ge_u,
        [WM_F_ADD] = &&f_add, [WM_F_SUB] = &&f_sub, [WM_F_MUL] = &&f_mul, [WM_F_AND] = &&f_and, [WM_F_OR] = &&f_or,
        [WM_F_XOR] = &&f_xor, [WM_F_SHL] = &&f_shl, [WM_F_SHR_S] = &&f_shr_s, [WM_F_SHR_U] = &&f_shr_u,
        [WM_F_REF_NULL] = &&f_ref_null, [WM_F_REF_IS_NULL] = &&f_ref_is_null, [WM_F_REF_FUNC] = &&f_ref_func,
        [WM_F_SAT] = &&f_sat, [WM_F_MISC] = &&f_misc, [WM_F_GET_CONST_ADD] = &&f_get_const_add,
        [WM_F_GET_GET_ADD] = &&f_get_get_add, [WM_F_GET_CONST] = &&f_get_const, [WM_F_GET_GET] = &&f_get_get,
        [WM_F_SET_GET] = &&f_set_get, [WM_F_CONST_ADD] = &&f_const_add, [WM_F_CONST_AND] = &&f_const_and,
        [WM_F_CONST_SUB] = &&f_const_sub, [WM_F_ADD_SET] = &&f_add_set, [WM_F_GET_LOAD] = &&f_get_load,
#if WM_FUSE
        [WM_F_TEE_BR_IF] = &&f_tee_br_if, [WM_F_SET_GET_BR_IF] = &&f_set_get_br_if, [WM_F_EQ_BR_IF] = &&f_eq_br_if, [WM_F_NE_BR_IF] = &&f_ne_br_if, [WM_F_EQZ_BR_IF] = &&f_eqz_br_if, [WM_F_LE_U_BR_IF] = &&f_le_u_br_if, [WM_F_LT_U_BR_IF] = &&f_lt_u_br_if, [WM_F_EQ_IF] = &&f_eq_if, [WM_F_NE_IF] = &&f_ne_if, [WM_F_EQZ_IF] = &&f_eqz_if,
#endif
    };
    uint32_t entry_depth;
    const WmCode* code;
    WmInstance* inst;
    const uint8_t* bytes;
    const WmCell* ip;
    uint32_t* fp;
    uint32_t* sp;
    /* The top of the stack, also in sp[-1]: values are read back from here, not from memory. */
    uint32_t tos = 0;
    uint8_t* mem;
    wm_size mem_size;
    WmFunc* callee;
    WmResult trap = NULL;
    WmResult stop;
    uint8_t fuel_on;
    uint32_t fuel;
    const WmCell* target;
    uint32_t keep;
    uint32_t drop;
    /* The record's first cell; ip is past it, at the record's other cells. */
    uint32_t cell = 0;
    const char* base;

    if (!rt)
    {
        const char* lowest = NULL;
        const char* highest = NULL;
        for (uint32_t k = 0; k < WM_F_COUNT; ++k)
            if (labels[k])
            {
                if (!lowest || (const char*)labels[k] < lowest)
                    lowest = (const char*)labels[k];
                if (!highest || (const char*)labels[k] > highest)
                    highest = (const char*)labels[k];
            }
        if (!lowest || highest - lowest > 0xFFFF)
            return NULL;
        for (uint32_t k = 0; k < WM_F_COUNT; ++k)
            g_fast_offsets[k] = labels[k] ? (uint16_t)((const char*)labels[k] - lowest) : 0u;
        g_fast_shift = lowest - (const char*)&&f_unreachable;
        g_fast_handlers = g_fast_offsets;
        return NULL;
    }
    base = (const char*)&&f_unreachable + g_fast_shift;
    entry_depth = rt->depth;
    fuel_on = rt->fuel_on && rt->nesting == 1u;
    fuel = fuel_on ? rt->fuel : WM_NONE;

#define FAST_REFRESH()                                                                                         \
    do                                                                                                         \
    {                                                                                                          \
        code = func->code;                                                                                     \
        inst = func->instance;                                                                                 \
        bytes = inst->module->bytes;                                                                           \
        mem = inst->memory ? inst->memory->data : NULL;                                                        \
        mem_size = mem ? WM_MEMORY_SIZE(inst->memory) : 0;                                                     \
    } while (0)
/* Fuel for the record at ip, then on to its handler; a stop leaves ip at the record. */
#define FAST_NEXT()                                                                                            \
    do                                                                                                         \
    {                                                                                                          \
        if (fuel-- == 0)                                                                                       \
        {                                                                                                      \
            if (fuel_on)                                                                                       \
            {                                                                                                  \
                fuel = 0;                                                                                      \
                stop = wm_fuel_exhausted;                                                                      \
                goto suspend;                                                                                  \
            }                                                                                                  \
            fuel = WM_NONE - 1u;                                                                               \
        }                                                                                                      \
        cell = (ip++)->u32;                                                                                    \
        goto *(void*)(base + (cell & 0xFFFFu));                                                          \
    } while (0)
/*
 * HOST = the place of SIZE bytes at ADDRESS + OFFSET. A memory in one block
 * directly, as ever; a paged one through the TLB. A TLB miss is an
 * exception: back to the record at REC (CELLS long, an access OP), the
 * loop stops as for fuel and wm_execute makes the frame; the record then
 * runs again. Nothing is called from inside the loop for it: on Xtensa a
 * call anywhere in the handlers costs the whole loop registers.
 */
#if WM_HAS_MMU
#define FAST_ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, REC, CELLS, TAG)                                            \
    do                                                                                                         \
    {                                                                                                          \
        if (__builtin_expect(!((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size)), \
                             1))                                                                               \
            HOST = mem + (ADDRESS) + (OFFSET);                                                                 \
        else if (!mem && inst->memory)                                                                         \
        {                                                                                                      \
            uint32_t ea_ = (ADDRESS) + (OFFSET);                                                               \
            const WmTlbEntry* e_ = &rt->tlb->entries[(ea_ >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];          \
            if ((TAG) == ((ea_ & (WM_FRAME_MASK | ((SIZE) - 1u))) | ((uint32_t)inst->memory->space << 3)) &&     \
                ea_ >= (OFFSET))                                                                               \
                HOST = e_->frame + (ea_ & (WM_FRAME - 1u));                                                    \
            else                                                                                               \
            {                                                                                                  \
                rt->fault_address = (ADDRESS);                                                                 \
                rt->fault_offset = (OFFSET);                                                                   \
                rt->fault_op = (OP);                                                                           \
                rt->fault_cells = (CELLS);                                                                     \
                ip = (REC);                                                                                    \
                goto mmu_fault;                                                                                \
            }                                                                                                  \
        }                                                                                                      \
        else                                                                                                   \
            TRAP(T_OOB_MEMORY);                                                                                \
    } while (0)
#else
#define FAST_ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, REC, CELLS, TAG)                                            \
    do                                                                                                         \
    {                                                                                                          \
        if ((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size))                   \
            TRAP(T_OOB_MEMORY);                                                                                \
        HOST = mem + (ADDRESS) + (OFFSET);                                                                     \
    } while (0)
#endif
/* The record's 16-bit operand, unsigned and signed; a later cell's. */
#define IMM (cell >> 16)
#define SIMM ((int32_t)cell >> 16)
#define IMM_OF(C) ((C).u32 >> 16)
#define SIMM_OF(C) ((C).s32 >> 16)
/* After code that changed the stack in memory only. */
#define FAST_RELOAD() (tos = sp[-1])
#define FAST_BINARY(NAME, EXPR)                                                                                \
    NAME:                                                                                                      \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = sp[-2];                                                                                   \
        --sp;                                                                                                  \
        sp[-1] = tos = (uint32_t)(EXPR);                                                                       \
        FAST_NEXT();                                                                                           \
    }

    if (resume)
    {
        const WmSuspended* saved = &rt->saved;
        entry_depth = saved->entry_depth;
        func = saved->func;
        ip = (const WmCell*)(const void*)saved->pc;
        fp = saved->fp;
        sp = saved->sp;
        rt->suspended = 0;
        FAST_REFRESH();
        FAST_RELOAD();
        FAST_NEXT();
    }
    fp = rt->sp;
    sp = fp + func->param_slots;
    FAST_REFRESH();
    if (sp + code->local_slots + code->max_stack > rt->stack_end)
        TRAP(T_STACK);
    zero_slots(sp, code->local_slots);
    sp += code->local_slots;
    ip = code->fast;
    FAST_NEXT();

f_unreachable:
    TRAP(WM_ERR_TRAP("trap: unreachable"));
f_nop:
{
    /* The rest of the run at once when there is fuel for it, else the next one. */
#if WM_NOP_RUNS
    uint32_t rest = IMM - 1u;
    if (fuel >= rest)
    {
        fuel -= rest;
        ip += rest;
    }
#endif
    FAST_NEXT();
}
f_if:
{
    uint32_t c = tos;
    --sp;
    FAST_RELOAD();
    ip = c ? ip : ip - 1 + SIMM;
    FAST_NEXT();
}
f_jump:
    ip = ip - 1 + SIMM;
    FAST_NEXT();
f_br_if:
{
    uint32_t c = tos;
    --sp;
    FAST_RELOAD();
    if (!c)
    {
        ++ip;
        FAST_NEXT();
    }
}
    /* fall through */
f_br:
    target = ip - 1 + SIMM;
    keep = ip[0].u32 & 0xFFFFu;
    drop = ip[0].u32 >> 16;
take:
    if (drop)
    {
        copy_slots(sp - keep - drop, sp - keep, keep);
        sp -= drop;
        FAST_RELOAD();
    }
    ip = target;
    FAST_NEXT();
f_br_table:
{
    uint32_t count = IMM;
    uint32_t index = tos;
    const WmCell* e = ip + 2u * (index < count ? index : count);
    --sp;
    FAST_RELOAD();
    target = ip - 1 + e[0].s32;
    keep = e[1].u32 & 0xFFFFu;
    drop = e[1].u32 >> 16;
    goto take;
}
f_return:
{
    uint32_t n = func->result_slots;
    WmFrame* frame;
    copy_slots(fp, sp - n, n);
    sp = fp + n;
    if (rt->depth == entry_depth)
    {
        if (fuel_on)
            rt->fuel = fuel;
        return NULL;
    }
    frame = &rt->frames[--rt->depth];
    ip = (const WmCell*)(const void*)frame->pc;
    fp = frame->fp;
    func = frame->func;
    FAST_REFRESH();
    FAST_RELOAD();
    FAST_NEXT();
}
f_call:
    callee = inst->funcs[IMM];
    goto call;
f_call_indirect:
{
    const WmType* type = &inst->module->types[IMM];
    WmTable* table = inst->tables[ip[0].u32];
    uint32_t index = tos;
    --sp;
    ++ip;
    if (index >= table->size)
        TRAP(WM_ERR_TRAP("trap: undefined element"));
    callee = (WmFunc*)table->elems[index];
    if (!callee)
        TRAP(WM_ERR_TRAP("trap: uninitialized element"));
    if (!wm_func_has_type(callee, type))
        TRAP(WM_ERR_TRAP("trap: indirect call type mismatch"));
    goto call;
}
f_drop:
    sp -= IMM;
    FAST_RELOAD();
    FAST_NEXT();
f_select:
{
    uint32_t n = IMM;
    if (!*--sp)
        copy_slots(sp - 2u * n, sp - n, n);
    sp -= n;
    FAST_RELOAD();
    FAST_NEXT();
}
f_local_get1:
    *sp++ = tos = fp[IMM];
    FAST_NEXT();
f_local_set1:
    fp[IMM] = tos;
    --sp;
    FAST_RELOAD();
    FAST_NEXT();
f_local_tee1:
    fp[IMM] = tos;
    FAST_NEXT();
f_local_get:
    copy_slots(sp, fp + IMM, ip[0].u32);
    sp += ip[0].u32;
    ++ip;
    FAST_RELOAD();
    FAST_NEXT();
f_local_set:
    sp -= ip[0].u32;
    copy_slots(fp + IMM, sp, ip[0].u32);
    ++ip;
    FAST_RELOAD();
    FAST_NEXT();
f_local_tee:
    copy_slots(fp + IMM, sp - ip[0].u32, ip[0].u32);
    ++ip;
    FAST_NEXT();
f_global_get:
    keep = 1;
    goto global;
f_global_set:
    keep = 0;
global:
{
    WmGlobal* g = inst->globals[IMM];
    uint32_t n = wm_type_slots(g->type);
    if (keep)
    {
        if (n == 1u)
            *sp = (uint32_t)g->value;
        else
            __builtin_memcpy(sp, &g->value, 8);
        sp += n;
    }
    else
    {
        sp -= n;
        if (n == 1u)
            g->value = *sp;
        else
            __builtin_memcpy(&g->value, sp, 8);
    }
    FAST_RELOAD();
    FAST_NEXT();
}
f_table_get:
    keep = 0;
    goto table;
f_table_set:
    keep = 1;
table:
#if WM_HAS_REF_TYPES
{
    WmTable* t = inst->tables[IMM];
    int set = keep != 0;
    uintptr_t r = 0;
    uint32_t i;
    if (set)
    {
        sp -= WM_REF_SLOTS;
        r = get_ref(sp);
    }
    i = *--sp;
    if (i >= t->size)
        TRAP(T_OOB_TABLE);
    if (set)
        t->elems[i] = r;
    else
    {
        put_ref(sp, t->elems[i]);
        sp += WM_REF_SLOTS;
    }
    FAST_RELOAD();
    FAST_NEXT();
}
#else
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
f_access:
{
    uint8_t op = (uint8_t)IMM;
    uint32_t offset = ip[0].u32;
    uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    /* A store's value is above the address: slots of it. */
    uint32_t above = op >= 0x36u ? ((d & 8u) ? 2u : 1u) : 0u;
    uint64_t value = 0;
    uint8_t* host;
    /* Loads by the TLB's load tag, stores by its store tag. */
    FAST_ADDR(sp[-1 - (int32_t)above], offset, size, host, op, ip - 1, 2u, (above ? e_->wtag : e_->tag));
    ++ip;
    if (op >= 0x36u)
    {
        value = above == 2u ? get64(sp - 2) : sp[-1];
        sp -= above + 1u;
        store_bytes(host, value, size);
        FAST_RELOAD();
        FAST_NEXT();
    }
    --sp;
    value = load_bytes(host, size);
    if (d & 4u)
    {
        uint8_t shift = (uint8_t)(64u - 8u * size);
        value = (uint64_t)((int64_t)(value << shift) >> shift);
    }
    if (d & 8u)
    {
        put64(sp, value);
        sp += 2;
    }
    else
        *sp++ = (uint32_t)value;
    FAST_RELOAD();
    FAST_NEXT();
}
f_i32_load:
{
    uint32_t offset = IMM;
    uint32_t address = tos;
    uint32_t value;
    uint8_t* host;
    FAST_ADDR(address, offset, 4u, host, 0x28u, ip - 1, 1u, e_->tag);
    __builtin_memcpy(&value, host, 4);
    sp[-1] = tos = value;
    FAST_NEXT();
}
f_i32_store:
{
    uint32_t offset = IMM;
    uint32_t address = sp[-2];
    uint8_t* host;
    FAST_ADDR(address, offset, 4u, host, 0x36u, ip - 1, 1u, e_->wtag);
    __builtin_memcpy(host, &tos, 4);
    sp -= 2;
    FAST_RELOAD();
    FAST_NEXT();
}
f_memory_size:
    *sp++ = tos = inst->memory->pages;
    FAST_NEXT();
f_memory_grow:
    /* Making frames may drop predecoded code: not this function's. */
    rt->caller = func;
    sp[-1] = tos = wm_memory_grow(inst->memory, tos);
    FAST_REFRESH();
    FAST_NEXT();
f_const16:
    *sp++ = tos = (uint32_t)SIMM;
    FAST_NEXT();
f_const32:
    *sp++ = tos = (ip++)->u32;
    FAST_NEXT();
f_const64:
    sp[0] = ip[0].u32;
    sp[1] = tos = ip[1].u32;
    sp += 2;
    ip += 2;
    FAST_NEXT();
f_numeric:
{
    uint8_t op = (uint8_t)IMM;
    uint8_t d = WM_ROM8(wm_numeric + (op - 0x45u));
    switch (d & 3u)
    {
        case WM_NUM_I32: sp = op_i32(op, d, sp, &trap); break;
#if WM_HAS_I64
        case WM_NUM_I64: sp = op_i64(op, d, sp, &trap); break;
#endif
#if WM_HAS_FLOAT
        default: sp = op_float(op, d, sp, &trap); break;
#endif
    }
    if (!sp)
        TRAP(trap);
    FAST_RELOAD();
    FAST_NEXT();
}
f_eqz:
    sp[-1] = tos = tos == 0;
    FAST_NEXT();
    FAST_BINARY(f_eq, a == b)
    FAST_BINARY(f_ne, a != b)
    FAST_BINARY(f_lt_s, (int32_t)a < (int32_t)b)
    FAST_BINARY(f_lt_u, a < b)
    FAST_BINARY(f_gt_s, (int32_t)a > (int32_t)b)
    FAST_BINARY(f_gt_u, a > b)
    FAST_BINARY(f_le_s, (int32_t)a <= (int32_t)b)
    FAST_BINARY(f_le_u, a <= b)
    FAST_BINARY(f_ge_s, (int32_t)a >= (int32_t)b)
    FAST_BINARY(f_ge_u, a >= b)
    FAST_BINARY(f_add, a + b)
    FAST_BINARY(f_sub, a - b)
    FAST_BINARY(f_mul, a * b)
    FAST_BINARY(f_and, a & b)
    FAST_BINARY(f_or, a | b)
    FAST_BINARY(f_xor, a ^ b)
    FAST_BINARY(f_shl, a << (b & 31u))
    FAST_BINARY(f_shr_s, (uint32_t)((int32_t)a >> (b & 31u)))
    FAST_BINARY(f_shr_u, a >> (b & 31u))
#if WM_HAS_REF_TYPES
f_ref_null:
    put_ref(sp, 0);
    sp += WM_REF_SLOTS;
    FAST_RELOAD();
    FAST_NEXT();
f_ref_is_null:
    sp -= WM_REF_SLOTS;
    *sp = get_ref(sp) == 0;
    ++sp;
    FAST_RELOAD();
    FAST_NEXT();
f_ref_func:
    put_ref(sp, (uintptr_t)inst->funcs[IMM]);
    sp += WM_REF_SLOTS;
    FAST_RELOAD();
    FAST_NEXT();
#else
f_ref_null:
f_ref_is_null:
f_ref_func:
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
f_sat:
#if WM_HAS_FLOAT
{
    uint32_t sub = IMM;
    uint8_t d = (uint8_t)(((sub & 2u) ? WM_NUM_F64 : WM_NUM_F32) | (sub >= 4u ? WM_NUM_WIDE : 0));
    sp = op_float((uint8_t)(0xE0u + sub), d, sp, &trap);
    if (!sp)
        TRAP(trap);
    FAST_RELOAD();
    FAST_NEXT();
}
#else
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
    /*
     * Fused runs. cell is the first record's and ip at the record after it;
     * the records after it follow as usual, one cell each here (local.get,
     * local.set, i32.const that fits, i32.load with a small offset, add, and,
     * sub; br_if is two). Without fuel for all, only the first runs.
     */
#define FUSED(EXTRA, FIRST)                                                                                    \
    if (fuel < (EXTRA))                                                                                        \
        goto FIRST;                                                                                            \
    fuel -= (EXTRA)
f_get_const_add:
    FUSED(2u, f_local_get1);
    *sp++ = tos = fp[IMM] + (uint32_t)SIMM_OF(ip[0]);
    ip += 2;
    FAST_NEXT();
f_get_get_add:
    FUSED(2u, f_local_get1);
    *sp++ = tos = fp[IMM] + fp[IMM_OF(ip[0])];
    ip += 2;
    FAST_NEXT();
f_get_const:
    FUSED(1u, f_local_get1);
    sp[0] = fp[IMM];
    sp[1] = tos = (uint32_t)SIMM_OF(ip[0]);
    sp += 2;
    ++ip;
    FAST_NEXT();
f_get_get:
    FUSED(1u, f_local_get1);
    sp[0] = fp[IMM];
    sp[1] = tos = fp[IMM_OF(ip[0])];
    sp += 2;
    ++ip;
    FAST_NEXT();
f_set_get:
    FUSED(1u, f_local_set1);
    fp[IMM] = tos;
    sp[-1] = tos = fp[IMM_OF(ip[0])];
    ++ip;
    FAST_NEXT();
f_const_add:
    FUSED(1u, f_const16);
    sp[-1] = tos = tos + (uint32_t)SIMM;
    ++ip;
    FAST_NEXT();
f_const_and:
    FUSED(1u, f_const16);
    sp[-1] = tos = tos & (uint32_t)SIMM;
    ++ip;
    FAST_NEXT();
f_const_sub:
    FUSED(1u, f_const16);
    sp[-1] = tos = tos - (uint32_t)SIMM;
    ++ip;
    FAST_NEXT();
f_add_set:
    /* i32.add has no operand: ip is at the local.set record. */
    FUSED(1u, f_add);
    fp[IMM_OF(ip[0])] = sp[-2] + tos;
    sp -= 2;
    ++ip;
    FAST_RELOAD();
    FAST_NEXT();
f_get_load:
{
    uint32_t address;
    uint32_t offset;
    uint32_t value;
    FUSED(1u, f_local_get1);
    address = fp[IMM];
    offset = IMM_OF(ip[0]);
    if (offset > mem_size || wm_out_of_bounds(address, offset + 4u, mem_size))
    {
        /* Not in one block (a trap, or paged memory): the two records run on their own. */
        fuel += 1u;
        goto f_local_get1;
    }
    __builtin_memcpy(&value, mem + address + offset, 4);
    *sp++ = tos = value;
    ++ip;
    FAST_NEXT();
}
#if WM_FUSE
    /* Branches fused with what decides them: the br_if record, at ip + AT, is its head and its slots. */
#define FAST_TAKE_BR_IF(AT)                                                                                    \
    target = ip + (AT) + SIMM_OF(ip[(AT)]);                                                                    \
    keep = ip[(AT) + 1].u32 & 0xFFFFu;                                                                         \
    drop = ip[(AT) + 1].u32 >> 16;                                                                             \
    goto take
f_tee_br_if:
{
    uint32_t c = tos;
    FUSED(1u, f_local_tee1);
    fp[IMM] = c;
    --sp;
    FAST_RELOAD();
    if (!c)
    {
        ip += 2;
        FAST_NEXT();
    }
    FAST_TAKE_BR_IF(0);
}
f_set_get_br_if:
    FUSED(2u, f_local_set1);
    fp[IMM] = tos;
    --sp;
    FAST_RELOAD();
    if (!fp[IMM_OF(ip[0])])
    {
        ip += 3;
        FAST_NEXT();
    }
    FAST_TAKE_BR_IF(1);
#define FAST_CMP_BR_IF(NAME, FIRST, POP, A, EXPR)                                                               \
    NAME:                                                                                                      \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = (A);                                                                                      \
        FUSED(1u, FIRST);                                                                                      \
        sp -= (POP);                                                                                           \
        FAST_RELOAD();                                                                                         \
        if (!(EXPR))                                                                                           \
        {                                                                                                      \
            ip += 2;                                                                                           \
            FAST_NEXT();                                                                                       \
        }                                                                                                      \
        FAST_TAKE_BR_IF(0);                                                                                    \
    }
/* The if record, at ip, is one cell: on to the record after it, or to its target. */
#define FAST_CMP_IF(NAME, FIRST, POP, A, EXPR)                                                                  \
    NAME:                                                                                                      \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = (A);                                                                                      \
        FUSED(1u, FIRST);                                                                                      \
        sp -= (POP);                                                                                           \
        FAST_RELOAD();                                                                                         \
        ip = (EXPR) ? ip + 1 : ip + SIMM_OF(ip[0]);                                                            \
        FAST_NEXT();                                                                                           \
    }
    FAST_CMP_BR_IF(f_eq_br_if, f_eq, 2, sp[-2], a == b)
    FAST_CMP_BR_IF(f_ne_br_if, f_ne, 2, sp[-2], a != b)
    FAST_CMP_BR_IF(f_eqz_br_if, f_eqz, 1, 0u, ((void)a, b == 0))
    FAST_CMP_BR_IF(f_le_u_br_if, f_le_u, 2, sp[-2], a <= b)
    FAST_CMP_BR_IF(f_lt_u_br_if, f_lt_u, 2, sp[-2], a < b)
    FAST_CMP_IF(f_eq_if, f_eq, 2, sp[-2], a == b)
    FAST_CMP_IF(f_ne_if, f_ne, 2, sp[-2], a != b)
    FAST_CMP_IF(f_eqz_if, f_eqz, 1, 0u, ((void)a, b == 0))
#undef FAST_CMP_IF
#undef FAST_CMP_BR_IF
#undef FAST_TAKE_BR_IF
#endif
#undef FUSED
f_misc:
{
    const uint8_t* immediates = bytes + ip[0].u32;
    uint32_t sub = IMM;
    ++ip;
    /* memory.init, copy and fill may make frames, which may drop predecoded code: not this function's. */
    rt->caller = func;
    if ((trap = misc(inst, sub, &immediates, &sp)))
        TRAP(trap);
    FAST_REFRESH();
    FAST_RELOAD();
    FAST_NEXT();
}

call:
    if (callee->code)
    {
        WmFrame* frame;
        if (rt->depth >= rt->max_frames)
            TRAP(T_STACK);
        frame = &rt->frames[rt->depth++];
        frame->pc = (const uint8_t*)(const void*)ip;
        frame->st = NULL;
        frame->fp = fp;
        frame->func = func;
        func = callee;
        FAST_REFRESH();
        fp = sp - func->param_slots;
        if (sp + code->local_slots + code->max_stack > rt->stack_end)
            TRAP(T_STACK);
        zero_slots(sp, code->local_slots);
        sp += code->local_slots;
        /* No records: the loop stops in the new frame, wm_execute has them made and goes on at the first. */
        if (__builtin_expect(!code->fast, 0))
        {
            ip = NULL;
            stop = wm_code_stop;
            goto suspend;
        }
        ((WmCode*)code)->used = 1;
        ip = code->fast;
        FAST_NEXT();
    }
    if (callee->host)
    {
        uint32_t* args = sp - callee->param_slots;
        uint32_t room = callee->param_slots > callee->result_slots ? callee->param_slots : callee->result_slots;
        if (args + room > rt->stack_end)
            TRAP(T_STACK);
        rt->sp = args + room;
        if (fuel_on)
            rt->fuel = fuel;
        TRACE_CLEAR();
        rt->host_trap = NULL;
        rt->caller = func;
        if ((trap = callee->host(rt, callee->user, args, inst->memory)) || (trap = rt->host_trap))
            TRAP_AFTER_HOST(trap);
        fuel_on = rt->fuel_on && rt->nesting == 1u;
        fuel = fuel_on ? rt->fuel : WM_NONE;
        sp = args + callee->result_slots;
        FAST_REFRESH();
        FAST_RELOAD();
        if (rt->yielding && rt->nesting == 1u)
        {
            rt->yielding = 0;
            stop = wm_yielded;
            goto suspend;
        }
        FAST_NEXT();
    }
    TRAP(WM_ERR_TRAP("trap: unlinked import"));

#if WM_HAS_MMU
mmu_fault:
    /* The record runs again: its fuel comes back. */
    ++fuel;
    stop = wm_page_stop;
    goto suspend;
#endif

suspend:
    rt->saved.entry_depth = entry_depth;
    rt->saved.func = func;
    rt->saved.pc = (const uint8_t*)(const void*)ip;
    rt->saved.st = NULL;
    rt->saved.fp = fp;
    rt->saved.sp = sp;
    if (fuel_on)
        rt->fuel = fuel;
    rt->suspended = 1;
    return stop;
#undef FAST_BINARY
#undef IMM
#undef SIMM
#undef IMM_OF
#undef SIMM_OF
#undef FAST_RELOAD
#undef FAST_NEXT
#undef FAST_REFRESH
}

const uint16_t* wm_fast_handlers(void)
{
    if (!g_fast_handlers)
        run_fast(NULL, NULL, 0);
    return g_fast_handlers;
}
#endif

/*
 * The dispatch: computed goto where the compiler has labels as values (GCC,
 * Clang), a switch elsewhere. Each instruction's code ends with NEXT.
 */
#if WM_COMPUTED_GOTO
#define SWITCH(op)
#define CASE(n) l_##n:
#define DEFAULT l_default:
#define NEXT continue
#else
#define SWITCH(op) switch (op)
#define CASE(n) case n:
#define DEFAULT default:
#define NEXT break
#endif

WM_HOT WmResult wm_execute(WmRuntime* rt, WmFunc* func, int resume)
{
#if WM_PREDECODE
    if (WM_RUNS_FAST((resume ? rt->saved.func : func)->instance->module))
    {
        WmResult result = resume ? NULL : wm_code_load(func);
        if (result)
            return result;
        result = run_fast(rt, func, resume);
        for (;;)
        {
#if WM_HAS_MMU
            if (result == wm_page_stop)
            {
                if ((result = page_fault(rt)))
                {
                    /* Waiting for a frame: still suspended at the access, which runs again. */
                    if (result != wm_page_wait)
                        rt->suspended = 0;
                    return result;
                }
            }
            else
#endif
            if (result == wm_code_stop)
            {
                if ((result = wm_code_load(rt->saved.func)))
                {
                    rt->suspended = 0;
#if WM_HAS_TRACE
                    rt->trap_func = rt->saved.func;
                    rt->trap_depth = rt->depth;
#endif
                    return result;
                }
                rt->saved.pc = (const uint8_t*)(const void*)rt->saved.func->code->fast;
            }
            else
                return result;
            result = run_fast(rt, func, 1);
        }
    }
#endif
    uint32_t entry_depth = rt->depth;
    const WmCode* code;
    WmInstance* inst;
    const uint8_t* bytes;
    const uint8_t* pc;
    const WmSide* side;
    const WmSide* st;
    const WmSide* taken;
    uint32_t* fp;
    uint32_t* sp;
    uint8_t* mem;
    wm_size mem_size;
    WmFunc* callee;
    WmResult trap = NULL;
    WmResult stop;
    /*
     * Only the outermost call stops for fuel: a host function's own calls run
     * to the end. Without fuel the counter still runs, from its top, so each
     * instruction costs one decrement and one test either way.
     */
    uint8_t fuel_on = rt->fuel_on && rt->nesting == 1u;
    uint32_t fuel = fuel_on ? rt->fuel : WM_NONE;

#define REFRESH()                                                                                              \
    do                                                                                                         \
    {                                                                                                          \
        code = func->code;                                                                                     \
        inst = func->instance;                                                                                 \
        bytes = inst->module->bytes;                                                                           \
        side = inst->module->side;                                                                             \
        mem = inst->memory ? inst->memory->data : NULL;                                                        \
        mem_size = mem ? WM_MEMORY_SIZE(inst->memory) : 0;                                                     \
    } while (0)

#define LOCAL(i) (code->local_offsets ? fp + code->local_offsets[i] : fp + (i))
#define LOCAL_SLOTS(i) (code->local_offsets ? code->local_offsets[(i) + 1u] - code->local_offsets[i] : 1u)

    if (resume)
    {
        const WmSuspended* saved = &rt->saved;
        entry_depth = saved->entry_depth;
        func = saved->func;
        pc = saved->pc;
        st = saved->st;
        fp = saved->fp;
        sp = saved->sp;
        rt->suspended = 0;
        REFRESH();
    }
    else
    {
        fp = rt->sp;
        sp = fp + func->param_slots;
        REFRESH();
        pc = code->body;
        st = side + code->side;
        callee = func;
        goto enter;
    }

    for (;;)
    {
        uint8_t op;
        if (fuel-- == 0)
        {
            if (fuel_on)
            {
                fuel = 0;
                stop = wm_fuel_exhausted;
                goto suspend;
            }
            fuel = WM_NONE - 1u;
        }
        op = WM_RD8(pc++);
#if WM_COMPUTED_GOTO
        {
            /* One jump per instruction, straight to its code; the numeric ones share a path. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Winitializer-overrides"
            static const void* const dispatch[256] = {
                [0 ... 255] = &&l_default, [0x45 ... 0xC4] = &&l_numeric, [0x00] = &&l_0x00, [0x01] = &&l_0x01, [0x02] = &&l_0x02, [0x03] = &&l_0x03, [0x04] = &&l_0x04, [0x0D] = &&l_0x0D, [0x05] = &&l_0x05, [0x0C] = &&l_0x0C, [0x0E] = &&l_0x0E, [0x0B] = &&l_0x0B, [0x0F] = &&l_0x0F, [0x10] = &&l_0x10, [0x11] = &&l_0x11, [0x1A] = &&l_0x1A, [0x1B] = &&l_0x1B, [0x1C] = &&l_0x1C, [0x20] = &&l_0x20, [0x21] = &&l_0x21, [0x22] = &&l_0x22, [0x23] = &&l_0x23, [0x24] = &&l_0x24, [0x28] = &&l_0x28, [0x29] = &&l_0x29, [0x2A] = &&l_0x2A, [0x2B] = &&l_0x2B, [0x2C] = &&l_0x2C, [0x2D] = &&l_0x2D, [0x2E] = &&l_0x2E, [0x2F] = &&l_0x2F, [0x30] = &&l_0x30, [0x31] = &&l_0x31, [0x32] = &&l_0x32, [0x33] = &&l_0x33, [0x34] = &&l_0x34, [0x35] = &&l_0x35, [0x36] = &&l_0x36, [0x37] = &&l_0x37, [0x38] = &&l_0x38, [0x39] = &&l_0x39, [0x3A] = &&l_0x3A, [0x3B] = &&l_0x3B, [0x3C] = &&l_0x3C, [0x3D] = &&l_0x3D, [0x3E] = &&l_0x3E, [0x3F] = &&l_0x3F, [0x40] = &&l_0x40, [0x41] = &&l_0x41, [0x43] = &&l_0x43, [0x44] = &&l_0x44, [0xFC] = &&l_0xFC,
#if WM_HAS_I64
                [0x42] = &&l_0x42,
#endif
#if WM_HAS_REF_TYPES
                [0x25] = &&l_0x25, [0x26] = &&l_0x26, [0xD0] = &&l_0xD0, [0xD1] = &&l_0xD1, [0xD2] = &&l_0xD2,
#endif
                /* The short paths last, so they win over the general code of the same opcodes. */
                [0x45] = &&f_0x45, [0x46] = &&f_0x46, [0x47] = &&f_0x47, [0x48] = &&f_0x48, [0x49] = &&f_0x49, [0x4A] = &&f_0x4A, [0x4B] = &&f_0x4B, [0x4C] = &&f_0x4C, [0x4D] = &&f_0x4D, [0x4E] = &&f_0x4E, [0x4F] = &&f_0x4F, [0x6A] = &&f_0x6A, [0x6B] = &&f_0x6B, [0x6C] = &&f_0x6C, [0x71] = &&f_0x71, [0x72] = &&f_0x72, [0x73] = &&f_0x73, [0x74] = &&f_0x74, [0x75] = &&f_0x75, [0x76] = &&f_0x76, [0x28] = &&f_0x28, [0x36] = &&f_0x36, [0x20] = &&f_0x20, [0x21] = &&f_0x21, [0x41] = &&f_0x41,};
#pragma GCC diagnostic pop
            goto *dispatch[op];
        }
    l_numeric:
#else
        if (op >= 0x45u && op <= 0xC4u)
#endif
        {
            uint8_t d = WM_ROM8(wm_numeric + (op - 0x45u));
            switch (d & 3u)
            {
                case WM_NUM_I32: sp = op_i32(op, d, sp, &trap); break;
#if WM_HAS_I64
                case WM_NUM_I64: sp = op_i64(op, d, sp, &trap); break;
#endif
#if WM_HAS_FLOAT
                default: sp = op_float(op, d, sp, &trap); break;
#endif
            }
            if (!sp)
                TRAP(trap);
            continue;
        }
#if WM_COMPUTED_GOTO
        /* The most common i32 instructions, memory accesses among them, each with its own short path. */
#define FAST_BINARY(OP, EXPR)                                                                                  \
    f_##OP:                                                                                                    \
    {                                                                                                          \
        uint32_t b = sp[-1];                                                                                   \
        uint32_t a = sp[-2];                                                                                   \
        --sp;                                                                                                  \
        sp[-1] = (uint32_t)(EXPR);                                                                             \
        continue;                                                                                              \
    }
    f_0x45:
        sp[-1] = sp[-1] == 0;
        continue;
        FAST_BINARY(0x46, a == b)
        FAST_BINARY(0x47, a != b)
        FAST_BINARY(0x48, (int32_t)a < (int32_t)b)
        FAST_BINARY(0x49, a < b)
        FAST_BINARY(0x4A, (int32_t)a > (int32_t)b)
        FAST_BINARY(0x4B, a > b)
        FAST_BINARY(0x4C, (int32_t)a <= (int32_t)b)
        FAST_BINARY(0x4D, a <= b)
        FAST_BINARY(0x4E, (int32_t)a >= (int32_t)b)
        FAST_BINARY(0x4F, a >= b)
        FAST_BINARY(0x6A, a + b)
        FAST_BINARY(0x6B, a - b)
        FAST_BINARY(0x6C, a * b)
        FAST_BINARY(0x71, a & b)
        FAST_BINARY(0x72, a | b)
        FAST_BINARY(0x73, a ^ b)
        FAST_BINARY(0x74, a << (b & 31u))
        FAST_BINARY(0x75, (uint32_t)((int32_t)a >> (b & 31u)))
        FAST_BINARY(0x76, a >> (b & 31u))
#undef FAST_BINARY
        /*
         * The commonest pairs run without a dispatch in between: the next
         * instruction is looked at, and taken here if there is fuel for it, so
         * fuel still counts every instruction and a stop falls where it would.
         */
#if WM_FUSE
#define TAKE_NEXT(OPCODE) (WM_RD8(pc) == (OPCODE) && fuel != 0 && (--fuel, ++pc, 1))
#else
#define TAKE_NEXT(OPCODE) 0
#endif
    f_0x20:
        if (code->local_offsets)
            goto l_0x20;
        *sp++ = fp[imm_u32(&pc)];
        if (TAKE_NEXT(0x20u))
            *sp++ = fp[imm_u32(&pc)];
        else if (TAKE_NEXT(0x41u))
            *sp++ = imm_s32(&pc);
        continue;
    f_0x21:
        if (code->local_offsets)
            goto l_0x21;
        fp[imm_u32(&pc)] = *--sp;
        if (TAKE_NEXT(0x20u))
            *sp++ = fp[imm_u32(&pc)];
        continue;
    f_0x41:
    {
        uint32_t c = imm_s32(&pc);
        /* A constant is usually an operand: add, and, subtract and compare it at once. */
        if (TAKE_NEXT(0x6Au))
            sp[-1] += c;
        else if (TAKE_NEXT(0x71u))
            sp[-1] &= c;
        else if (TAKE_NEXT(0x6Bu))
            sp[-1] -= c;
        else if (TAKE_NEXT(0x46u))
            sp[-1] = sp[-1] == c;
        else if (TAKE_NEXT(0x76u))
            sp[-1] >>= c & 31u;
        else if (TAKE_NEXT(0x74u))
            sp[-1] <<= c & 31u;
        else
            *sp++ = c;
        continue;
    }
#undef TAKE_NEXT
    f_0x28:
    {
        uint32_t offset;
        uint32_t address = sp[-1];
        uint32_t value;
        imm_u32(&pc);
        offset = imm_u32(&pc);
        if (offset > mem_size || wm_out_of_bounds(address, offset + 4u, mem_size))
        {
#if WM_HAS_MMU
            if (!mem && inst->memory)
            {
                if (!(sp = paged_access(inst->memory, 0x28u, offset, sp, &trap)))
                    TRAP(trap);
                continue;
            }
#endif
            TRAP(T_OOB_MEMORY);
        }
        __builtin_memcpy(&value, mem + address + offset, 4);
        sp[-1] = value;
        continue;
    }
    f_0x36:
    {
        uint32_t offset;
        uint32_t address = sp[-2];
        imm_u32(&pc);
        offset = imm_u32(&pc);
        if (offset > mem_size || wm_out_of_bounds(address, offset + 4u, mem_size))
        {
#if WM_HAS_MMU
            if (!mem && inst->memory)
            {
                if (!(sp = paged_access(inst->memory, 0x36u, offset, sp, &trap)))
                    TRAP(trap);
                continue;
            }
#endif
            TRAP(T_OOB_MEMORY);
        }
        __builtin_memcpy(mem + address + offset, &sp[-1], 4);
        sp -= 2;
        continue;
    }
#endif
        SWITCH(op)
        {
            CASE(0x00) TRAP(WM_ERR_TRAP("trap: unreachable"));
            CASE(0x01) NEXT;
            CASE(0x02)
            CASE(0x03) skip_block_type(&pc); NEXT;
            CASE(0x04)
                skip_block_type(&pc);
                if (*--sp)
                {
                    ++st;
                    NEXT;
                }
                taken = st;
                goto take;
            CASE(0x0D)
                imm_u32(&pc);
                if (!*--sp)
                {
                    ++st;
                    NEXT;
                }
                /* fall through */
            CASE(0x05)
            CASE(0x0C)
                taken = st;
                goto take;
            CASE(0x0E)
            {
                uint32_t count = imm_u32(&pc);
                uint32_t index = *--sp;
                taken = st + (index < count ? index : count);
                goto take;
            }
            CASE(0x0B)
                if (pc - 1 != code->end)
                    NEXT;
                /* fall through: the function's own end returns */
            CASE(0x0F)
            {
                uint32_t n = func->result_slots;
                WmFrame* frame;
                copy_slots(fp, sp - n, n);
                sp = fp + n;
                if (rt->depth == entry_depth)
                {
                    if (fuel_on)
                        rt->fuel = fuel;
                    return NULL;
                }
                frame = &rt->frames[--rt->depth];
                pc = frame->pc;
                st = frame->st;
                fp = frame->fp;
                func = frame->func;
                REFRESH();
                NEXT;
            }
            CASE(0x10) callee = inst->funcs[imm_u32(&pc)]; goto call;
            CASE(0x11)
            {
                const WmType* type = &inst->module->types[imm_u32(&pc)];
                WmTable* table = inst->tables[imm_u32(&pc)];
                uint32_t index = *--sp;
                if (index >= table->size)
                    TRAP(WM_ERR_TRAP("trap: undefined element"));
                callee = (WmFunc*)table->elems[index];
                if (!callee)
                    TRAP(WM_ERR_TRAP("trap: uninitialized element"));
                if (!wm_func_has_type(callee, type))
                    TRAP(WM_ERR_TRAP("trap: indirect call type mismatch"));
                goto call;
            }
            CASE(0x1A)
                sp -= st->keep;
                ++st;
                NEXT;
            CASE(0x1B)
            CASE(0x1C)
            {
                uint32_t n;
                if (op == 0x1Bu)
                    n = (st++)->keep;
                else
                {
                    imm_u32(&pc);
                    n = wm_type_slots(WM_RD8(pc++));
                }
                if (!*--sp)
                    copy_slots(sp - 2u * n, sp - n, n);
                sp -= n;
                NEXT;
            }
            CASE(0x20)
            CASE(0x21)
            CASE(0x22)
            {
                uint32_t i = imm_u32(&pc);
                uint32_t n;
                /* Most functions have only one-slot locals: a plain word each. */
                if (!code->local_offsets)
                {
                    if (op == 0x20u)
                        *sp++ = fp[i];
                    else
                    {
                        fp[i] = sp[-1];
                        if (op == 0x21u)
                            --sp;
                    }
                    NEXT;
                }
                n = LOCAL_SLOTS(i);
                if (op == 0x20u)
                {
                    copy_slots(sp, LOCAL(i), n);
                    sp += n;
                }
                else
                {
                    copy_slots(LOCAL(i), sp - n, n);
                    if (op == 0x21u)
                        sp -= n;
                }
                NEXT;
            }
            CASE(0x23)
            CASE(0x24)
            {
                WmGlobal* g = inst->globals[imm_u32(&pc)];
                uint32_t n = wm_type_slots(g->type);
                /* The value as one or two slots, the way it lies in memory. */
                if (op == 0x23u)
                {
                    if (n == 1u)
                        *sp = (uint32_t)g->value;
                    else
                        __builtin_memcpy(sp, &g->value, 8);
                    sp += n;
                }
                else
                {
                    sp -= n;
                    if (n == 1u)
                        g->value = *sp;
                    else
                        __builtin_memcpy(&g->value, sp, 8);
                }
                NEXT;
            }
#if WM_HAS_REF_TYPES
            CASE(0x25)
            CASE(0x26)
            {
                WmTable* t = inst->tables[imm_u32(&pc)];
                uintptr_t r = 0;
                uint32_t i;
                if (op == 0x26u)
                {
                    sp -= WM_REF_SLOTS;
                    r = get_ref(sp);
                }
                i = *--sp;
                if (i >= t->size)
                    TRAP(T_OOB_TABLE);
                if (op == 0x26u)
                    t->elems[i] = r;
                else
                {
                    put_ref(sp, t->elems[i]);
                    sp += WM_REF_SLOTS;
                }
                NEXT;
            }
#endif
            CASE(0x28) CASE(0x29) CASE(0x2A) CASE(0x2B) CASE(0x2C) CASE(0x2D) CASE(0x2E) CASE(0x2F)
            CASE(0x30) CASE(0x31) CASE(0x32) CASE(0x33) CASE(0x34) CASE(0x35)
            CASE(0x36) CASE(0x37) CASE(0x38) CASE(0x39) CASE(0x3A) CASE(0x3B) CASE(0x3C) CASE(0x3D) CASE(0x3E)
            {
                uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
                uint8_t size = (uint8_t)(1u << (d & 3u));
                uint64_t value = 0;
                uint32_t address;
                uint32_t offset;
#if WM_HAS_MMU
                if (!mem && inst->memory)
                {
                    imm_u32(&pc);
                    offset = imm_u32(&pc);
                    if (!(sp = paged_access(inst->memory, op, offset, sp, &trap)))
                        TRAP(trap);
                    NEXT;
                }
#endif
                if (op >= 0x36u)
                {
                    if (d & 8u)
                    {
                        sp -= 2;
                        value = get64(sp);
                    }
                    else
                        value = *--sp;
                }
                imm_u32(&pc);
                offset = imm_u32(&pc);
                address = *--sp;
                if (offset > mem_size || wm_out_of_bounds(address, offset + size, mem_size))
                    TRAP(T_OOB_MEMORY);
                address += offset;
                if (op >= 0x36u)
                {
                    store_bytes(mem + address, value, size);
                    NEXT;
                }
                value = load_bytes(mem + address, size);
                if (d & 4u)
                {
                    uint8_t shift = (uint8_t)(64u - 8u * size);
                    value = (uint64_t)((int64_t)(value << shift) >> shift);
                }
                if (d & 8u)
                {
                    put64(sp, value);
                    sp += 2;
                }
                else
                    *sp++ = (uint32_t)value;
                NEXT;
            }
            CASE(0x3F)
                ++pc;
                *sp++ = inst->memory->pages;
                NEXT;
            CASE(0x40)
                ++pc;
                sp[-1] = wm_memory_grow(inst->memory, sp[-1]);
                REFRESH();
                NEXT;
            CASE(0x41) *sp++ = imm_s32(&pc); NEXT;
#if WM_HAS_I64
            CASE(0x42)
                put64(sp, imm_signed(&pc));
                sp += 2;
                NEXT;
#endif
            CASE(0x43)
            CASE(0x44)
            {
                uint8_t n = op == 0x43u ? 4u : 8u;
                for (uint8_t i = 0; i < n; ++i)
                    ((uint8_t*)sp)[i] = WM_RD8(pc + i);
                pc += n;
                sp += n / 4u;
                NEXT;
            }
#if WM_HAS_REF_TYPES
            CASE(0xD0)
            CASE(0xD2)
                put_ref(sp, op == 0xD0u ? (++pc, (uintptr_t)0) : (uintptr_t)inst->funcs[imm_u32(&pc)]);
                sp += WM_REF_SLOTS;
                NEXT;
            CASE(0xD1)
                sp -= WM_REF_SLOTS;
                *sp = get_ref(sp) == 0;
                ++sp;
                NEXT;
#endif
            CASE(0xFC)
            {
                uint32_t sub = imm_u32(&pc);
#if WM_HAS_FLOAT
                if (sub <= 7u)
                {
                    /* Saturating conversions: f32 or f64 to i32 (0-3) or i64 (4-7). */
                    uint8_t d = (uint8_t)(((sub & 2u) ? WM_NUM_F64 : WM_NUM_F32) | (sub >= 4u ? WM_NUM_WIDE : 0));
                    sp = op_float((uint8_t)(0xE0u + sub), d, sp, &trap);
                    NEXT;
                }
#endif
                if ((trap = misc(inst, sub, &pc, &sp)))
                    TRAP(trap);
                REFRESH();
                NEXT;
            }
            DEFAULT TRAP(WM_ERR_TRAP("trap: illegal opcode"));
        }
        continue;

    take:
        if (taken->drop)
        {
            copy_slots(sp - taken->keep - taken->drop, sp - taken->keep, taken->keep);
            sp -= taken->drop;
        }
        pc = bytes + taken->pc;
        st = side + taken->st;
        continue;

    call:
        if (callee->code)
        {
            WmFrame* frame;
            if (rt->depth >= rt->max_frames)
                TRAP(T_STACK);
            frame = &rt->frames[rt->depth++];
            frame->pc = pc;
            frame->st = st;
            frame->fp = fp;
            frame->func = func;
            func = callee;
            REFRESH();
            pc = code->body;
            st = side + code->side;
            fp = sp - func->param_slots;
        enter:
            if (sp + code->local_slots + code->max_stack > rt->stack_end)
                TRAP(T_STACK);
            zero_slots(sp, code->local_slots);
            sp += code->local_slots;
        }
        else if (callee->host)
        {
            uint32_t* args = sp - callee->param_slots;
            uint32_t room = callee->param_slots > callee->result_slots ? callee->param_slots : callee->result_slots;
            if (args + room > rt->stack_end)
                TRAP(T_STACK);
            rt->sp = args + room;
            /* The host may read the fuel, set it, or set it to 0 to stop the program. */
            if (fuel_on)
                rt->fuel = fuel;
            TRACE_CLEAR();
            rt->host_trap = NULL;
#if WM_TRACKS_CALLERS
            rt->caller = func;
#endif
            if ((trap = callee->host(rt, callee->user, args, inst->memory)) || (trap = rt->host_trap))
                TRAP_AFTER_HOST(trap);
            fuel_on = rt->fuel_on && rt->nesting == 1u;
            fuel = fuel_on ? rt->fuel : WM_NONE;
            sp = args + callee->result_slots;
            REFRESH();
            if (rt->yielding && rt->nesting == 1u)
            {
                rt->yielding = 0;
                stop = wm_yielded;
                goto suspend;
            }
        }
        else
            TRAP(WM_ERR_TRAP("trap: unlinked import"));
        continue;

    suspend:
        /* Everything the loop needs to go on lies in plain data. */
        rt->saved.entry_depth = entry_depth;
        rt->saved.func = func;
        rt->saved.pc = pc;
        rt->saved.st = st;
        rt->saved.fp = fp;
        rt->saved.sp = sp;
        if (fuel_on)
            rt->fuel = fuel;
        rt->suspended = 1;
        return stop;
    }
}
