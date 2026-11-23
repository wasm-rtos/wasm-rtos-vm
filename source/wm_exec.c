/*
 * Running code. What both interpreter loops share is here: traps, reading
 * immediates, stack slots, memory accesses, the numeric instructions and
 * the bulk memory and table ones. The loops are in files of their own,
 * included at the end so that they and these helpers are one unit, as
 * CPython's ceval.c includes its cases: the compiler inlines the helpers
 * where it should (WM_INLINE_OPS) and keeps one copy where it should not.
 *
 * - wm_exec_fast.c.h: the fast loop over predecoded records (wm_fast.c).
 * - wm_exec_inplace.c.h: the loop over the module's own bytes.
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

/* Memory accesses 0x28-0x3E: log2 of the size, 4 sign-extends, 8 is a 64-bit value (i64 and f64 are odd). */
static const uint8_t WM_ROM ACCESS[WM_OP_I64_STORE32 - WM_OP_I32_LOAD + 1] = {
#define WM_WASM_ACCESS(OP, NAME, name, VALUE, SIZE, STORE, SIGNED)                                            \
    [(OP) - WM_OP_I32_LOAD] = (SIZE) | ((SIGNED) << 2) | ((WM_NUM_##VALUE & 1u) << 3),
#include "wm_defs.def"
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

/* A signed load's value from its SIZE low bytes (a 64-bit load needs nothing). */
static WM_IMM uint64_t sign_extend(uint64_t value, uint8_t size)
{
    switch (size)
    {
        case 1: return (uint64_t)(int64_t)(int8_t)value;
        case 2: return (uint64_t)(int64_t)(int16_t)value;
        case 4: return (uint64_t)(int64_t)(int32_t)value;
        default: return value;
    }
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
    uint8_t d = WM_ROM8(ACCESS + (op - WM_FIRST_ACCESS));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    uint8_t bytes[8];
    uint64_t value = 0;
    uint64_t ea;
    if (op >= WM_OP_I32_STORE)
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
    if (op >= WM_OP_I32_STORE)
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
        value = sign_extend(value, size);
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

/* The i32 instructions with records of their own (wm_defs.def): the one definition both loops run. */
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR)                                                             \
    static WM_ALWAYS_INLINE uint32_t i32_##name(uint32_t a, uint32_t b)                                        \
    {                                                                                                          \
        (void)b;                                                                                               \
        return (uint32_t)(EXPR);                                                                               \
    }
#include "wm_defs.def"

/* Ops whose operands are i32: results by wm_numeric. */
WM_OP uint32_t* op_i32(uint8_t op, uint8_t d, uint32_t* sp, WmResult* trap)
{
    uint32_t b = (d & WM_NUM_BINARY) ? *--sp : 0;
    uint32_t a = *--sp;
    uint64_t r;
    uint8_t n = (uint8_t)(b & 31u);
    switch (op)
    {
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR)                                                             \
    case OP: r = i32_##name(a, b); break;
#include "wm_defs.def"
        case WM_OP_I32_CLZ: r = clz32(a); break;
        case WM_OP_I32_CTZ: r = ctz32(a); break;
        case WM_OP_I32_POPCNT: r = (uint32_t)__builtin_popcountl(a); break;
        case WM_OP_I32_DIV_S:
        case WM_OP_I32_DIV_U:
        case WM_OP_I32_REM_S:
        case WM_OP_I32_REM_U:
            if (b == 0)
            {
                *trap = T_DIVIDE;
                return NULL;
            }
            if (op == WM_OP_I32_DIV_S && a == 0x80000000u && b == 0xFFFFFFFFu)
            {
                *trap = T_OVERFLOW;
                return NULL;
            }
            if (op == WM_OP_I32_DIV_S)
                r = (uint32_t)((int32_t)a / (int32_t)b);
            else if (op == WM_OP_I32_DIV_U)
                r = a / b;
            else if (op == WM_OP_I32_REM_S)
                r = b == 0xFFFFFFFFu ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
            else
                r = a % b;
            break;
        case WM_OP_I32_ROTL: r = (uint32_t)(a << n) | (n ? a >> (32u - n) : 0); break;
        case WM_OP_I32_ROTR: r = (uint32_t)(a >> n) | (n ? a << (32u - n) : 0); break;
        case WM_OP_I64_EXTEND_I32_S: r = (uint64_t)(int64_t)(int32_t)a; break;
        case WM_OP_I32_EXTEND8_S: r = (uint32_t)(int32_t)(int8_t)a; break;
        case WM_OP_I32_EXTEND16_S: r = (uint32_t)(int32_t)(int16_t)a; break;
#if WM_HAS_FLOAT
        case WM_OP_F32_CONVERT_I32_S:
        case WM_OP_F32_CONVERT_I32_U:
        {
            float f = op == WM_OP_F32_CONVERT_I32_S ? (float)(int32_t)a : (float)a;
            uint32_t bits;
            __builtin_memcpy(&bits, &f, 4);
            r = bits;
            break;
        }
        case WM_OP_F64_CONVERT_I32_S:
        case WM_OP_F64_CONVERT_I32_U:
        {
            double f = op == WM_OP_F64_CONVERT_I32_S ? (double)(int32_t)a : (double)a;
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
        case WM_OP_I64_EQZ: r = a == 0; break;
        case WM_OP_I64_EQ: r = a == b; break;
        case WM_OP_I64_NE: r = a != b; break;
        case WM_OP_I64_LT_S: r = (int64_t)a < (int64_t)b; break;
        case WM_OP_I64_LT_U: r = a < b; break;
        case WM_OP_I64_GT_S: r = (int64_t)a > (int64_t)b; break;
        case WM_OP_I64_GT_U: r = a > b; break;
        case WM_OP_I64_LE_S: r = (int64_t)a <= (int64_t)b; break;
        case WM_OP_I64_LE_U: r = a <= b; break;
        case WM_OP_I64_GE_S: r = (int64_t)a >= (int64_t)b; break;
        case WM_OP_I64_GE_U: r = a >= b; break;
        case WM_OP_I64_CLZ: r = a ? (uint64_t)__builtin_clzll(a) : 64u; break;
        case WM_OP_I64_CTZ: r = a ? (uint64_t)__builtin_ctzll(a) : 64u; break;
        case WM_OP_I64_POPCNT: r = (uint64_t)__builtin_popcountll(a); break;
        case WM_OP_I64_ADD: r = a + b; break;
        case WM_OP_I64_SUB: r = a - b; break;
        case WM_OP_I64_MUL: r = a * b; break;
        case WM_OP_I64_DIV_S:
        case WM_OP_I64_DIV_U:
        case WM_OP_I64_REM_S:
        case WM_OP_I64_REM_U:
            if (b == 0)
            {
                *trap = T_DIVIDE;
                return NULL;
            }
            if (op == WM_OP_I64_DIV_S && a == 0x8000000000000000u && b == UINT64_MAX)
            {
                *trap = T_OVERFLOW;
                return NULL;
            }
            if (op == WM_OP_I64_DIV_S)
                r = (uint64_t)((int64_t)a / (int64_t)b);
            else if (op == WM_OP_I64_DIV_U)
                r = a / b;
            else if (op == WM_OP_I64_REM_S)
                r = b == UINT64_MAX ? 0 : (uint64_t)((int64_t)a % (int64_t)b);
            else
                r = a % b;
            break;
        case WM_OP_I64_AND: r = a & b; break;
        case WM_OP_I64_OR: r = a | b; break;
        case WM_OP_I64_XOR: r = a ^ b; break;
        case WM_OP_I64_SHL: r = a << n; break;
        case WM_OP_I64_SHR_S: r = (uint64_t)((int64_t)a >> n); break;
        case WM_OP_I64_SHR_U: r = a >> n; break;
        case WM_OP_I64_ROTL: r = (a << n) | (n ? a >> (64u - n) : 0); break;
        case WM_OP_I64_ROTR: r = (a >> n) | (n ? a << (64u - n) : 0); break;
        case WM_OP_I64_EXTEND8_S: r = (uint64_t)(int64_t)(int8_t)a; break;
        case WM_OP_I64_EXTEND16_S: r = (uint64_t)(int64_t)(int16_t)a; break;
        case WM_OP_I64_EXTEND32_S: r = (uint64_t)(int64_t)(int32_t)a; break;
#if WM_HAS_FLOAT
        case WM_OP_F32_CONVERT_I64_S:
        case WM_OP_F32_CONVERT_I64_U:
        {
            float f = op == WM_OP_F32_CONVERT_I64_S ? (float)(int64_t)a : (float)a;
            uint32_t bits;
            __builtin_memcpy(&bits, &f, 4);
            r = bits;
            break;
        }
        case WM_OP_F64_CONVERT_I64_S:
        case WM_OP_F64_CONVERT_I64_U:
        {
            double f = op == WM_OP_F64_CONVERT_I64_S ? (double)(int64_t)a : (double)a;
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
    switch (wide && op >= WM_OP_F64_EQ && op <= WM_OP_F64_GE ? op - 6u : wide && op >= WM_OP_F64_ABS && op <= WM_OP_F64_COPYSIGN ? op - 14u : op)
    {
        case WM_OP_F32_EQ: r = a == b; break;
        case WM_OP_F32_NE: r = a != b; break;
        case WM_OP_F32_LT: r = a < b; break;
        case WM_OP_F32_GT: r = a > b; break;
        case WM_OP_F32_LE: r = a <= b; break;
        case WM_OP_F32_GE: r = a >= b; break;
        case WM_OP_F32_ABS: r = aa & (wide ? 0x7FFFFFFFFFFFFFFFu : 0x7FFFFFFFu); break;
        case WM_OP_F32_NEG: r = aa ^ (wide ? 0x8000000000000000u : 0x80000000u); break;
        case WM_OP_F32_COPYSIGN:
        {
            uint64_t sign = wide ? 0x8000000000000000u : 0x80000000u;
            r = (aa & ~sign) | (ab & sign);
            break;
        }
        case WM_OP_F32_CEIL: f = wide ? ceil(a) : ceilf((float)a), is_float = 1; break;
        case WM_OP_F32_FLOOR: f = wide ? floor(a) : floorf((float)a), is_float = 1; break;
        case WM_OP_F32_TRUNC: f = wide ? trunc(a) : truncf((float)a), is_float = 1; break;
        case WM_OP_F32_NEAREST: f = wide ? nearbyint(a) : nearbyintf((float)a), is_float = 1; break;
        case WM_OP_F32_SQRT: f = wide ? sqrt(a) : sqrtf((float)a), is_float = 1; break;
        case WM_OP_F32_ADD: f = wide ? a + b : (double)((float)a + (float)b), is_float = 1; break;
        case WM_OP_F32_SUB: f = wide ? a - b : (double)((float)a - (float)b), is_float = 1; break;
        case WM_OP_F32_MUL: f = wide ? a * b : (double)((float)a * (float)b), is_float = 1; break;
        case WM_OP_F32_DIV: f = wide ? a / b : (double)((float)a / (float)b), is_float = 1; break;
        case WM_OP_F32_MIN: f = min_max(a, b, 0), is_float = 1; break;
        case WM_OP_F32_MAX: f = min_max(a, b, 1), is_float = 1; break;
        case WM_OP_I32_TRUNC_F32_S: case WM_OP_I32_TRUNC_F64_S: kind = 0; goto convert;
        case WM_OP_I32_TRUNC_F32_U: case WM_OP_I32_TRUNC_F64_U: kind = 1; goto convert;
        case WM_OP_I64_TRUNC_F32_S: case WM_OP_I64_TRUNC_F64_S: kind = 2; goto convert;
        case WM_OP_I64_TRUNC_F32_U: case WM_OP_I64_TRUNC_F64_U: kind = 3; goto convert;
        case 0xE0: case 0xE2: kind = 0, saturate = 1; goto convert;
        case 0xE1: case 0xE3: kind = 1, saturate = 1; goto convert;
        case 0xE4: case 0xE6: kind = 2, saturate = 1; goto convert;
        case 0xE5: case 0xE7: kind = 3, saturate = 1; goto convert;
        case WM_OP_F32_DEMOTE_F64: /* f32.demote_f64 */
        case WM_OP_F64_PROMOTE_F32: /* f64.promote_f32 */
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

/* A helper's trap: noted in *trap, and NULL for the stack. */
static uint32_t* trapped(WmResult* trap, WmResult e)
{
    *trap = e;
    return NULL;
}

/*
 * The bulk memory and table instructions after 0xFC: all but the saturating
 * conversions. The stack after it, or NULL and *trap, as the numeric helpers.
 */
static uint32_t* misc(WmInstance* inst, uint32_t sub, const uint8_t** pc, uint32_t* sp, WmResult* trap)
{
    WmMemory* memory = inst->memory;
    wm_size mem_size = memory ? WM_MEMORY_SIZE(memory) : 0;
    uint32_t a = 0;
    WmResult result = NULL;
#if WM_HAS_REF_TYPES
    uint32_t b;
    WmTable* t = NULL;
#endif
    if (sub == WM_FC_MEMORY_INIT || sub == WM_FC_DATA_DROP || sub == WM_FC_TABLE_INIT || sub == WM_FC_ELEM_DROP)
        a = imm_u32(pc);
#if WM_HAS_REF_TYPES
    if (sub >= WM_FC_TABLE_INIT && sub != WM_FC_ELEM_DROP)
        t = inst->tables[imm_u32(pc)];
#endif
    switch (sub)
    {
        case WM_FC_MEMORY_INIT:
        {
            const WmDataSegment* s = &inst->module->datas[a];
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            uint32_t size = inst->data_dropped[a] ? 0 : s->size;
            ++*pc;
            sp -= 3;
            if (wm_out_of_bounds(src, n, size) || wm_out_of_bounds(dst, n, mem_size))
                return trapped(trap, T_OOB_MEMORY);
#if WM_HAS_MMU
            if ((result = wm_memory_write(memory, dst, s->data + src, n)))
                return trapped(trap, result);
#else
            for (uint32_t i = 0; i < n; ++i)
                memory->data[dst + i] = WM_RD8(s->data + src + i);
#endif
            break;
        }
        case WM_FC_DATA_DROP: inst->data_dropped[a] = 1; break;
        case WM_FC_MEMORY_COPY:
        case WM_FC_MEMORY_FILL:
        {
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            *pc += sub == WM_FC_MEMORY_COPY ? 2 : 1;
            sp -= 3;
            if (wm_out_of_bounds(dst, n, mem_size) || (sub == WM_FC_MEMORY_COPY && wm_out_of_bounds(src, n, mem_size)))
                return trapped(trap, T_OOB_MEMORY);
#if WM_HAS_MMU
            if ((result = sub == WM_FC_MEMORY_COPY ? wm_memory_copy(memory, dst, src, n) : wm_memory_fill(memory, dst, (uint8_t)src, n)))
                return trapped(trap, result);
#else
            if (sub == WM_FC_MEMORY_COPY)
                memmove(memory->data + dst, memory->data + src, n);
            else
                memset(memory->data + dst, (int)(uint8_t)src, n);
#endif
            break;
        }
#if WM_HAS_REF_TYPES
        case WM_FC_TABLE_INIT:
            sp -= 3;
            result = wm_table_init(inst, a, t, sp[0], sp[1], sp[2]);
            break;
        case WM_FC_ELEM_DROP: inst->elem_dropped[a] = 1; break;
        case WM_FC_TABLE_COPY:
        {
            WmTable* src_table = inst->tables[imm_u32(pc)];
            uint32_t n = sp[-1], src = sp[-2], dst = sp[-3];
            sp -= 3;
            if (wm_out_of_bounds(src, n, src_table->size) || wm_out_of_bounds(dst, n, t->size))
                return trapped(trap, T_OOB_TABLE);
            memmove(t->elems + dst, src_table->elems + src, (size_t)n * sizeof(uintptr_t));
            break;
        }
        case WM_FC_TABLE_GROW:
            b = *--sp;
            sp -= WM_REF_SLOTS;
            *sp = grow_table(t, b, get_ref(sp));
            ++sp;
            break;
        case WM_FC_TABLE_SIZE: *sp++ = t->size; break;
        case WM_FC_TABLE_FILL:
        {
            uintptr_t value;
            b = *--sp;
            sp -= WM_REF_SLOTS;
            value = get_ref(sp);
            a = *--sp;
            if (wm_out_of_bounds(a, b, t->size))
                return trapped(trap, T_OOB_TABLE);
            for (uint32_t k = 0; k < b; ++k)
                t->elems[a + k] = value;
            break;
        }
#endif
    }
    return result ? trapped(trap, result) : sp;
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

#if WM_PROFILE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if WM_PREDECODE
static const char* const g_record_names[WM_R_COUNT] = {
#define WM_REC(NAME, name) #name,
#include "wm_defs.def"
};
#endif
#if WM_PREDECODE
static uint64_t g_profile_records[WM_R_COUNT];
/* The previous record's kind first, WM_R_COUNT for none yet. */
static uint64_t g_profile_pairs[WM_R_COUNT + 1u][WM_R_COUNT];
#endif
static uint64_t g_profile_ops[256];
/* Opcodes run by the fast loop's shared numeric and access records. */
static uint64_t g_profile_shared[256];
#if WM_PREDECODE
/* Kinds by their handler's offset (wm_exec_fast.c.h). */
static uint8_t g_profile_kinds[65536];
static uint8_t g_profile_previous = WM_R_COUNT;
#endif

static void profile_write(void)
{
    const char* name = getenv("WM_PROFILE_FILE");
    char path[512];
    FILE* f;
    if (!name)
        return;
    snprintf(path, sizeof(path), "%s.%ld", name, (long)getpid());
    if (!(f = fopen(path, "w")))
        return;
#if WM_PREDECODE
    for (uint32_t k = 0; k < WM_R_COUNT; ++k)
        if (g_profile_records[k])
            fprintf(f, "record %s %llu\n", g_record_names[k], (unsigned long long)g_profile_records[k]);
    for (uint32_t a = 0; a < WM_R_COUNT; ++a)
        for (uint32_t b = 0; b < WM_R_COUNT; ++b)
            if (g_profile_pairs[a][b])
                fprintf(f, "pair %s %s %llu\n", g_record_names[a], g_record_names[b],
                        (unsigned long long)g_profile_pairs[a][b]);
#endif
    for (uint32_t op = 0; op < 256; ++op)
        if (g_profile_ops[op])
            fprintf(f, "op 0x%02X %llu\n", (unsigned)op, (unsigned long long)g_profile_ops[op]);
    for (uint32_t op = 0; op < 256; ++op)
        if (g_profile_shared[op])
            fprintf(f, "shared 0x%02X %llu\n", (unsigned)op, (unsigned long long)g_profile_shared[op]);
    fclose(f);
}

static void profile_start(void)
{
    static int started;
    if (!started)
    {
        started = 1;
        atexit(profile_write);
    }
}

#define WM_PROFILE_OP(op) (++g_profile_ops[op])
#define WM_PROFILE_SHARED(op) (++g_profile_shared[op])
#define WM_PROFILE_RECORD(offset)                                                                              \
    do                                                                                                         \
    {                                                                                                          \
        uint8_t kind_ = g_profile_kinds[offset];                                                               \
        ++g_profile_records[kind_];                                                                            \
        ++g_profile_pairs[g_profile_previous][kind_];                                                          \
        g_profile_previous = kind_;                                                                            \
    } while (0)
#else
#define WM_PROFILE_OP(op) ((void)0)
#define WM_PROFILE_SHARED(op) ((void)0)
#define WM_PROFILE_RECORD(offset) ((void)0)
#endif

#if WM_PREDECODE
/*
 * Records for a function about to run, in a module that runs predecoded
 * (decision 21): NULL when it has them; wm_code_alone when it runs in place
 * by itself, its side table made, because the runtime makes no code or its
 * records found no room after the cache let go what it could. Any other
 * error ends the call.
 */
static WmResult code_or_alone(WmRuntime* rt, WmFunc* func)
{
    WmTail* tail;
    WmResult result;
    if (func->code->fast || rt->makes_code)
    {
        result = wm_code_load(func);
        if (!result || strncmp(result, "exhaustion", 10) != 0)
            return result;
    }
    if ((result = wm_reg_side(func->instance->module, func->code, &tail)))
        return result;
    return wm_code_alone;
}
#endif

#include "wm_exec_fast.c.h"
#include "wm_exec_inplace.c.h"
