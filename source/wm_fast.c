/*
 * Predecoding: each function written once more as cells, one record per
 * WebAssembly instruction, for the interpreter's fast loop. A record's first
 * cell is its handler (an offset from the first handler) and a 16-bit
 * operand; what does not fit follows in cells of its own. Branches hold
 * their target as a distance in cells and the slots to keep and drop. Rare
 * instructions keep their bytes and run through the same helpers as in
 * place. A module whose operands do not fit is not predecoded.
 *
 * Records follow instructions one to one: positions convert both ways, so
 * fuel and snapshots stay those of the module's instructions.
 */
#include "wm_internal.h"

#include <stdlib.h>

#if WM_PREDECODE

static uint32_t leb_u32(const uint8_t** p)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        if (shift < 32u)
            value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    return value;
}

static uint64_t leb_s64(const uint8_t** p)
{
    uint64_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        byte = WM_RD8((*p)++);
        if (shift < 64u)
            value |= (uint64_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    if (shift < 64u && (byte & 0x40u))
        value |= ~(uint64_t)0 << shift;
    return value;
}

static void skip_block_type(const uint8_t** p)
{
    if (WM_RD8(*p) >= 0x40u && WM_RD8(*p) < 0x80u)
        ++*p;
    else
        leb_s64(p);
}

/* The i32 instructions with handlers of their own (wm_ops.def), by opcode; 0 for the shared numeric one. */
static uint8_t fast_numeric(uint8_t op)
{
    switch (op)
    {
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR)                                                                    \
    case OP: return WM_F_##NAME;
#include "wm_ops.def"
        default: return 0;
    }
}

/* The i32 memory accesses with handlers of their own (wm_ops.def), the offset as operand, by opcode; 0 for the others. */
static uint8_t fast_access(uint8_t op)
{
    switch (op)
    {
#define WM_RECORD_ACCESS(NAME, name, OP, TYPE, KIND)                                                                  \
    case OP: return WM_F_##NAME;
#include "wm_ops.def"
        default: return 0;
    }
}

/* Whether a constant fits a record's signed 16-bit operand. */
static int fits16(int64_t value)
{
    return value >= -32768 && value <= 32767;
}

/* Slots of local index: 1 unless the function has locals that take more. */
static uint32_t local_width(const WmCode* code, uint32_t index)
{
    return code->local_offsets ? code->local_offsets[index + 1u] - code->local_offsets[index] : 1u;
}

/*
 * One instruction at p: where the next one starts, the cells its record
 * takes and the side table entries it owns. The cells follow from the
 * bytes alone, so positions convert both ways.
 */
static const uint8_t* step(const WmCode* code, const uint8_t* p, uint32_t* cells, uint32_t* sides)
{
    uint8_t op = WM_RD8(p++);
    *sides = 0;
    *cells = 1;
    if (op >= 0x45u && op <= 0xC4u)
        return p;
    switch (op)
    {
        case 0x02:
        case 0x03: skip_block_type(&p); break;
        case 0x04:
            skip_block_type(&p);
            *sides = 1;
            break;
        case 0x05: *sides = 1; break;
        case 0x0C:
        case 0x0D:
            leb_u32(&p);
            *cells = 2, *sides = 1;
            break;
        case 0x0E:
        {
            uint32_t count = leb_u32(&p);
            for (uint32_t i = 0; i <= count; ++i)
                leb_u32(&p);
            *cells = 1u + 2u * (count + 1u), *sides = count + 1u;
            break;
        }
        case 0x10:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0xD2: leb_u32(&p); break;
        case 0x11:
            leb_u32(&p);
            leb_u32(&p);
            *cells = 2;
            break;
        case 0x1A:
        case 0x1B: *sides = 1; break;
        case 0x1C:
        {
            uint32_t count = leb_u32(&p);
            p += count;
            break;
        }
        case 0x20:
        case 0x21:
        case 0x22:
            *cells = local_width(code, leb_u32(&p)) == 1u ? 1u : 2u;
            break;
        case 0x3F:
        case 0x40:
        case 0xD0: ++p; break;
        case 0x41:
            *cells = fits16((int64_t)leb_s64(&p)) ? 1u : 2u;
            break;
        case 0x42:
            leb_s64(&p);
            *cells = 3;
            break;
        case 0x43:
        {
            uint32_t bits = 0;
            for (uint32_t i = 0; i < 4u; ++i)
                bits |= (uint32_t)WM_RD8(p + i) << (8u * i);
            p += 4;
            *cells = fits16((int32_t)bits) ? 1u : 2u;
            break;
        }
        case 0x44:
            p += 8;
            *cells = 3;
            break;
        case 0xFC:
        {
            uint32_t sub = leb_u32(&p);
            if (sub <= 7u)
                break;
            /* The rest keep their immediates in the module: a record of the opcode and where they are. */
            *cells = 2;
            switch (sub)
            {
                case 8: leb_u32(&p); ++p; break;
                case 10: p += 2; break;
                case 11: ++p; break;
                case 12:
                case 14: leb_u32(&p); leb_u32(&p); break;
                default: leb_u32(&p);
            }
            break;
        }
        default:
            if (op >= 0x28u && op <= 0x3Eu)
            {
                uint32_t offset;
                leb_u32(&p);
                offset = leb_u32(&p);
                *cells = fast_access(op) && offset <= 0xFFFFu ? 1u : 2u;
            }
    }
    return p;
}

/* Where a branch lands: the module offset of its target and, once found, the target's record. */
typedef struct
{
    uint32_t pc;
    uint32_t cell;
} WmTarget;

static int target_order(const void* a, const void* b)
{
    uint32_t x = ((const WmTarget*)a)->pc, y = ((const WmTarget*)b)->pc;
    return x < y ? -1 : x > y;
}

static int is_branch(uint8_t op)
{
    return op == 0x04u || op == 0x05u || op == 0x0Cu || op == 0x0Du || op == 0x0Eu;
}

/* The record a branch to the module offset pc lands on, as a cell index; UINT32_MAX if no instruction starts there. */
static uint32_t record_at(const WmTarget* targets, uint32_t count, uint32_t pc)
{
    uint32_t low = 0, high = count;
    while (low < high)
    {
        uint32_t mid = low + (high - low) / 2u;
        if (targets[mid].pc < pc)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low == count || targets[low].pc != pc)
        return UINT32_MAX;
    return targets[low].cell;
}

static int is_nop(const WmCode* code, const uint8_t* p)
{
    uint8_t op = WM_RD8(p);
    return op == 0x01u || op == 0x02u || op == 0x03u || (op == 0x0Bu && p != code->end);
}

/* Instructions that do nothing, in a row from p: one handler skips them, with their fuel. */
static uint32_t nop_run(const WmCode* code, const uint8_t* p)
{
    uint32_t n = 0;
    while (p <= code->end && is_nop(code, p) && n < 0xFFFFu)
    {
        uint32_t cells, sides;
        p = step(code, p, &cells, &sides);
        ++n;
    }
    return n;
}

/* A record's first cell: the handler of kind and a 16-bit operand. */
static WmCell head(const uint16_t* handlers, uint8_t kind, uint32_t operand)
{
    WmCell c;
    c.u32 = (uint32_t)handlers[kind] | (operand << 16);
    return c;
}

/* The distance in cells from the record at cell to a branch's target, and its slots, from a side entry. */
static int branch_to(const WmSide* e, const WmTarget* targets, uint32_t count, uint32_t cell, int32_t* distance,
                     uint32_t* slots)
{
    uint32_t to = record_at(targets, count, e->pc);
    if (to == UINT32_MAX || e->keep > 0xFFFFu || e->drop > 0xFFFFu)
        return 0;
    *distance = (int32_t)(to - cell);
    *slots = e->keep | (e->drop << 16);
    return 1;
}

/*
 * The first record of a common run gets a fused handler: the one of the
 * longest pattern that matches the kinds from it on. The others keep
 * theirs, so a branch, a stop or a snapshot that lands inside the run finds
 * ordinary records. Kinds after the end of the function, and the end of a
 * pattern shorter than four, are WM_F_COUNT.
 */
#define FUSE_WINDOW 4u

#define RUN2(A, B) {A, B, WM_F_COUNT, WM_F_COUNT}
#define RUN3(A, B, C) {A, B, C, WM_F_COUNT}
/* The patterns of each shape of form (decision 14). */
#define PATTERN_CONST(OP) RUN2(WM_F_CONST16, WM_F_##OP)
#define PATTERN_GET(OP) RUN2(WM_F_LOCAL_GET1, WM_F_##OP)
#define PATTERN_SET(OP) RUN2(WM_F_##OP, WM_F_LOCAL_SET1)
#define PATTERN_GET_GET(OP) RUN3(WM_F_LOCAL_GET1, WM_F_LOCAL_GET1, WM_F_##OP)
#define PATTERN_GET_CONST(OP) RUN3(WM_F_LOCAL_GET1, WM_F_CONST16, WM_F_##OP)
#define PATTERN_GET_GET_SET(OP) {WM_F_LOCAL_GET1, WM_F_LOCAL_GET1, WM_F_##OP, WM_F_LOCAL_SET1}
#define PATTERN_GET_CONST_SET(OP) {WM_F_LOCAL_GET1, WM_F_CONST16, WM_F_##OP, WM_F_LOCAL_SET1}

static void fuse(const uint16_t* handlers, WmCell* record, const uint8_t* kinds)
{
    static const struct
    {
        uint8_t kinds[FUSE_WINDOW];
        uint8_t fused;
    } runs[] = {
#define WM_RECORD_FORM(NAME, name, SHAPE, OP, op) {PATTERN_##SHAPE(OP), WM_F_##NAME},
#define WM_RECORD_FUSED_GET_LOAD(NAME, name, LOAD, TYPE) {RUN2(WM_F_LOCAL_GET1, WM_F_##LOAD), WM_F_##NAME},
#include "wm_ops.def"
        {RUN2(WM_F_LOCAL_GET1, WM_F_CONST16), WM_F_GET_CONST},
        {RUN2(WM_F_LOCAL_GET1, WM_F_LOCAL_GET1), WM_F_GET_GET},
        {RUN2(WM_F_LOCAL_SET1, WM_F_LOCAL_GET1), WM_F_SET_GET},
        {RUN2(WM_F_LOCAL_TEE1, WM_F_CONST16), WM_F_TEE_CONST},
        {{WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_CONST16, WM_F_ADD}, WM_F_SET_GET_CONST_ADD},
        {RUN3(WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_LOCAL_GET1), WM_F_SET_GET_GET},
        {RUN3(WM_F_LOCAL_TEE1, WM_F_CONST16, WM_F_SHR_U), WM_F_TEE_CONST_SHR_U},
        {RUN3(WM_F_LOCAL_TEE1, WM_F_CONST16, WM_F_XOR), WM_F_TEE_CONST_XOR},
        {RUN2(WM_F_LOCAL_TEE1, WM_F_I32_LOAD), WM_F_TEE_LOAD},
        {RUN2(WM_F_LOCAL_GET1, WM_F_LOCAL_TEE1), WM_F_GET_TEE},
        {RUN2(WM_F_LOCAL_GET1, WM_F_I32_STORE), WM_F_GET_STORE},
        {RUN2(WM_F_LOCAL_GET1, WM_F_BR_TABLE), WM_F_GET_BR_TABLE},
#if WM_FUSE
        /* Branches with what decides them: a loss where branch prediction is good. */
        {RUN3(WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_BR_IF), WM_F_SET_GET_BR_IF},
        {RUN2(WM_F_LOCAL_TEE1, WM_F_BR_IF), WM_F_TEE_BR_IF},
        {RUN2(WM_F_EQ, WM_F_BR_IF), WM_F_EQ_BR_IF},
        {RUN2(WM_F_NE, WM_F_BR_IF), WM_F_NE_BR_IF},
        {RUN2(WM_F_EQZ, WM_F_BR_IF), WM_F_EQZ_BR_IF},
        {RUN2(WM_F_LE_U, WM_F_BR_IF), WM_F_LE_U_BR_IF},
        {RUN2(WM_F_LT_U, WM_F_BR_IF), WM_F_LT_U_BR_IF},
        {RUN2(WM_F_EQ, WM_F_IF), WM_F_EQ_IF},
        {RUN2(WM_F_NE, WM_F_IF), WM_F_NE_IF},
        {RUN2(WM_F_EQZ, WM_F_IF), WM_F_EQZ_IF},
        {{WM_F_LOCAL_GET1, WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_BR_IF}, WM_F_GET_SET_GET_BR_IF},
        {{WM_F_LOCAL_GET1, WM_F_CONST16, WM_F_EQ, WM_F_BR_IF}, WM_F_GET_CONST_EQ_BR_IF},
        {{WM_F_LOCAL_GET1, WM_F_CONST16, WM_F_NE, WM_F_IF}, WM_F_GET_CONST_NE_IF},
#endif
    };
    uint32_t best = 0;
    size_t found = 0;
    if (!record)
        return;
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); ++r)
    {
        uint32_t length = 0;
        while (length < FUSE_WINDOW && runs[r].kinds[length] != WM_F_COUNT && runs[r].kinds[length] == kinds[length])
            ++length;
        if ((length == FUSE_WINDOW || runs[r].kinds[length] == WM_F_COUNT) && length > best)
            best = length, found = r;
    }
    if (best)
        record->u32 = (record->u32 & 0xFFFF0000u) | handlers[runs[found].fused];
}

#define E_DOES_NOT_FIT WM_ERR_UNSUPPORTED("unsupported: an operand too large for predecoding")

/*
 * Besides the records themselves it needs RAM only for the branch targets,
 * 8 bytes each, sorted by offset so the records can be matched in one walk.
 */
#if WM_REGISTER
static WmResult reg_translate(WmModule* m, WmCode* code, const WmSide* side, WmTail* tail);
#endif

WmResult wm_predecode(WmModule* m, WmCode* code, const WmSide* side)
{
#if WM_REGISTER
    if (1)
        return reg_translate(m, code, side, NULL);
#endif
    {
    const uint16_t* handlers = wm_fast_handlers();
    uint32_t cells = 0, count = 0, k = 0;
    const uint8_t* p;
    const WmSide* st = side;
    WmTarget* targets = NULL;
    WmCell* out;
    /* The last records, waiting for the kinds after them to see whether they start a run. */
    WmCell* last[FUSE_WINDOW - 1u] = {NULL, NULL, NULL};
    uint8_t kinds[FUSE_WINDOW] = {WM_F_COUNT, WM_F_COUNT, WM_F_COUNT, WM_F_COUNT};
    if (!handlers)
        return WM_ERR_UNSUPPORTED("unsupported: handlers too far apart for predecoding");
    for (p = code->body; p <= code->end;)
    {
        uint32_t n, sides;
        uint8_t op = WM_RD8(p);
        p = step(code, p, &n, &sides);
        cells += n;
        if (is_branch(op))
            count += sides;
    }
    /* Branch distances within a record's 16-bit operand. */
    if (cells > 0x7FFFu)
        return E_DOES_NOT_FIT;
    if (count)
    {
        targets = (WmTarget*)wm_malloc((size_t)count * sizeof(WmTarget));
        if (!targets)
            return WM_ERR_EXHAUSTION("exhaustion: out of memory");
        for (p = code->body; p <= code->end;)
        {
            uint32_t n, sides;
            uint8_t op = WM_RD8(p);
            p = step(code, p, &n, &sides);
            for (uint32_t i = 0; is_branch(op) && i < sides; ++i, ++k)
                targets[k].pc = st[i].pc, targets[k].cell = UINT32_MAX;
            st += sides;
        }
        qsort(targets, count, sizeof(WmTarget), target_order);
        /* The record of every target, so that branches can point forward. */
        k = 0;
        p = code->body;
        for (uint32_t cell = 0; p <= code->end && k < count;)
        {
            uint32_t n, sides;
            uint32_t pc = (uint32_t)(p - m->bytes);
            while (k < count && targets[k].pc < pc)
                ++k;
            for (; k < count && targets[k].pc == pc; ++k)
                targets[k].cell = cell;
            p = step(code, p, &n, &sides);
            cell += n;
        }
        st = side;
    }
    code->own = (WmCell*)wm_code_malloc((size_t)cells * sizeof(WmCell));
    if (!code->own)
    {
        wm_free(targets);
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }
    code->fast_cells = (uint16_t)cells;
    code->mapped = 0;
    out = code->own;
    for (p = code->body; p <= code->end;)
    {
        const uint8_t* start = p;
        uint8_t op = WM_RD8(p++);
        uint8_t kind;
        uint32_t operand = 0;
        uint32_t cell = (uint32_t)(out - code->fast);
        int fits = 1;
        uint32_t n, sides;
        step(code, start, &n, &sides);
        if (op >= 0x45u && op <= 0xC4u)
        {
            kind = fast_numeric(op);
            if (!kind)
            {
                kind = WM_F_NUMERIC;
                operand = op;
            }
        }
        else
            switch (op)
            {
                case 0x00: kind = WM_F_UNREACHABLE; break;
                case 0x01:
                case 0x02:
                case 0x03:
                    kind = WM_F_NOP;
                    operand = nop_run(code, start);
                    break;
                case 0x04:
                case 0x05:
                {
                    uint32_t to = record_at(targets, count, st->pc);
                    kind = op == 0x04u ? WM_F_IF : WM_F_JUMP;
                    fits = to != UINT32_MAX;
                    operand = (uint32_t)(int32_t)(to - cell) & 0xFFFFu;
                    break;
                }
                case 0x0B:
                    kind = start == code->end ? WM_F_RETURN : WM_F_NOP;
                    operand = start == code->end ? 0u : nop_run(code, start);
                    break;
                case 0x0C:
                case 0x0D:
                {
                    int32_t distance = 0;
                    kind = op == 0x0Cu ? WM_F_BR : WM_F_BR_IF;
                    fits = branch_to(st, targets, count, cell, &distance, &out[1].u32);
                    operand = (uint32_t)distance & 0xFFFFu;
                    break;
                }
                case 0x0E:
                {
                    uint32_t table = sides - 1u;
                    kind = WM_F_BR_TABLE;
                    operand = table;
                    fits = table <= 0xFFFFu;
                    for (uint32_t i = 0; fits && i <= table; ++i)
                        fits = branch_to(st + i, targets, count, cell, &out[1u + 2u * i].s32, &out[2u + 2u * i].u32);
                    break;
                }
                case 0x0F: kind = WM_F_RETURN; break;
                case 0x10:
                    kind = WM_F_CALL;
                    operand = leb_u32(&p);
                    break;
                case 0x11:
                    kind = WM_F_CALL_INDIRECT;
                    operand = m->types[leb_u32(&p)].canon;
                    out[1].u32 = leb_u32(&p);
                    break;
                case 0x1A:
                    kind = WM_F_DROP;
                    operand = st->keep;
                    break;
                case 0x1B:
                    kind = WM_F_SELECT;
                    operand = st->keep;
                    break;
                case 0x1C:
                    leb_u32(&p);
                    kind = WM_F_SELECT;
                    operand = wm_type_slots(WM_RD8(p));
                    break;
                case 0x20:
                case 0x21:
                case 0x22:
                {
                    uint32_t index = leb_u32(&p);
                    uint32_t width = local_width(code, index);
                    operand = code->local_offsets ? code->local_offsets[index] : index;
                    if (width == 1u)
                        kind = (uint8_t)(WM_F_LOCAL_GET1 + (op - 0x20u));
                    else
                    {
                        kind = (uint8_t)(WM_F_LOCAL_GET + (op - 0x20u));
                        out[1].u32 = width;
                    }
                    break;
                }
                case 0x23:
                case 0x24:
                    /* A global of one slot (i32, f32: the stack pointer of C programs) has handlers of its own. */
                    operand = leb_u32(&p);
                    if (wm_type_slots(m->globals[operand].type) == 1u)
                        kind = op == 0x23u ? WM_F_GLOBAL_GET1 : WM_F_GLOBAL_SET1;
                    else
                        kind = op == 0x23u ? WM_F_GLOBAL_GET : WM_F_GLOBAL_SET;
                    break;
                case 0x25:
                case 0x26:
                    kind = op == 0x25u ? WM_F_TABLE_GET : WM_F_TABLE_SET;
                    operand = leb_u32(&p);
                    break;
                case 0x3F: kind = WM_F_MEMORY_SIZE; break;
                case 0x40: kind = WM_F_MEMORY_GROW; break;
                case 0x41:
                case 0x43:
                {
                    uint32_t value = 0;
                    if (op == 0x41u)
                        value = (uint32_t)leb_s64(&p);
                    else
                        for (uint32_t i = 0; i < 4u; ++i)
                            value |= (uint32_t)WM_RD8(p + i) << (8u * i);
                    if (n == 1u)
                    {
                        kind = WM_F_CONST16;
                        operand = value & 0xFFFFu;
                    }
                    else
                    {
                        kind = WM_F_CONST32;
                        out[1].u32 = value;
                    }
                    break;
                }
                case 0x42:
                case 0x44:
                {
                    uint64_t value = 0;
                    if (op == 0x42u)
                        value = leb_s64(&p);
                    else
                        for (uint32_t i = 0; i < 8u; ++i)
                            value |= (uint64_t)WM_RD8(p + i) << (8u * i);
                    kind = WM_F_CONST64;
                    out[1].u32 = (uint32_t)value;
                    out[2].u32 = (uint32_t)(value >> 32);
                    break;
                }
                case 0xD0: kind = WM_F_REF_NULL; break;
                case 0xD1: kind = WM_F_REF_IS_NULL; break;
                case 0xD2:
                    kind = WM_F_REF_FUNC;
                    operand = leb_u32(&p);
                    break;
                case 0xFC:
                {
                    uint32_t sub = leb_u32(&p);
                    kind = sub <= 7u ? WM_F_SAT : WM_F_MISC;
                    operand = sub;
                    if (sub > 7u)
                        out[1].u32 = (uint32_t)(p - m->bytes);
                    break;
                }
                default:
                {
                    /* Memory accesses: the common i32 ones with their own handlers. */
                    uint32_t offset;
                    leb_u32(&p);
                    offset = leb_u32(&p);
                    if (n == 1u)
                    {
                        kind = fast_access(op);
                        operand = offset;
                    }
                    else
                    {
                        kind = WM_F_ACCESS;
                        operand = op;
                        out[1].u32 = offset;
                    }
                }
            }
        if (!fits || operand > 0xFFFFu)
        {
            wm_free(targets);
            return E_DOES_NOT_FIT;
        }
        out[0] = head(handlers, kind, operand);
        kinds[FUSE_WINDOW - 1u] = kind;
        fuse(handlers, last[0], kinds);
        for (uint32_t i = 0; i + 1u < FUSE_WINDOW; ++i)
            kinds[i] = kinds[i + 1u];
        last[0] = last[1], last[1] = last[2], last[2] = out;
        st += sides;
        out += n;
        p = step(code, start, &n, &sides);
    }
    /* The last records, with nothing after them. */
    for (uint32_t i = 0; i + 1u < FUSE_WINDOW; ++i)
    {
        kinds[FUSE_WINDOW - 1u] = WM_F_COUNT;
        fuse(handlers, last[0], kinds);
        for (uint32_t k = 0; k + 1u < FUSE_WINDOW; ++k)
            kinds[k] = kinds[k + 1u];
        last[0] = last[1], last[1] = last[2], last[2] = NULL;
    }
    wm_free(targets);
    return NULL;
    }
}

int wm_fast_position(const WmModule* m, const WmCode* code, const WmCell* ip, uint32_t* offset, uint32_t* side)
{
    uint32_t cell = 0;
    uint32_t target = (uint32_t)(ip - code->fast);
#if WM_REGISTER
    /* Records do not follow instructions there: no positions. */
    (void)m, (void)target, (void)cell, (void)offset;
    *side = code->side;
    return 0;
#endif
    *side = code->side;
    for (const uint8_t* p = code->body; p <= code->end;)
    {
        uint32_t n, sides;
        if (cell == target)
        {
            *offset = (uint32_t)(p - m->bytes);
            return 1;
        }
        p = step(code, p, &n, &sides);
        cell += n;
        *side += sides;
    }
    return 0;
}

const WmCell* wm_fast_ip(const WmModule* m, const WmCode* code, uint32_t offset)
{
#if WM_REGISTER
    (void)m, (void)code, (void)offset;
    return NULL;
#endif
    uint32_t cell = 0;
    for (const uint8_t* p = code->body; p <= code->end;)
    {
        uint32_t n, sides;
        if ((uint32_t)(p - m->bytes) == offset)
            return code->fast + cell;
        p = step(code, p, &n, &sides);
        cell += n;
    }
    return NULL;
}

#endif

#if WM_REGISTER
/*
 * Experiment (decision 16): a function translated for the register loop.
 * Operands are frame slots (fp-relative: parameters and locals, then the
 * stack at B + depth); a value that is only a local or a constant stays
 * that until something needs it in its slot (copy propagation within a
 * block). Every instruction not done here runs as on the stack, on the
 * slots below the record's top, so the frame at a label, a call or a
 * branch is the stack machine's. Fuel is one a record, and snapshots are
 * not made: an experiment of what the loop could gain, not the engine.
 */
enum
{
    P_SLOT,
    P_LOCAL,
    P_CONST
};

typedef struct
{
    uint8_t kind;
    uint32_t value;
    /* Where the stack machine keeps it. */
    uint32_t slot;
} RegValue;

typedef struct
{
    WmCell* cells;
    uint32_t count, capacity;
    RegValue pend[256];
    uint32_t pending;
    /* The cell of the last record written and the slot its result went to, or UINT32_MAX. */
    uint32_t last;
    uint32_t last_slot;
    uint8_t last_kind;
    uint8_t kind;
    uint32_t base;
    const uint16_t* handlers;
    WmResult error;
} RegOut;

typedef struct
{
    uint32_t cell;
    uint32_t head;
    uint32_t pc;
} RegFixup;

static void reg_put(RegOut* o, uint32_t value)
{
    if (o->error)
        return;
    if (o->count == o->capacity)
    {
        uint32_t capacity = o->capacity ? o->capacity * 2u : 256u;
        WmCell* grown = (WmCell*)wm_realloc(o->cells, capacity * sizeof(WmCell));
        if (!grown)
        {
            o->error = WM_ERR_EXHAUSTION("exhaustion: out of memory");
            return;
        }
        o->cells = grown;
        o->capacity = capacity;
    }
    o->cells[o->count++].u32 = value;
}

static void reg_push(RegOut* o, uint8_t kind, uint32_t value, uint32_t slot);

static void reg_head(RegOut* o, uint8_t kind, uint32_t operand)
{
    if (operand > 0xFFFFu)
        o->error = WM_ERR_UNSUPPORTED("unsupported: an operand too large for the register loop");
    reg_put(o, (uint32_t)o->handlers[kind] | (operand << 16));
    o->last = UINT32_MAX;
    o->kind = kind;
}

/* The record just written made the value now on top, in its slot: a local.set after it can retarget it. */
static void reg_made(RegOut* o, uint32_t head, uint32_t slot)
{
    reg_push(o, P_SLOT, slot, slot);
    o->last = head;
    o->last_slot = slot;
    o->last_kind = o->kind;
}

/*
 * When the condition on top is what the last record compared, that record
 * goes and the branch compares instead: its kind, taken when the compare
 * holds (negate: when it does not), and the compare's operands; 0 if not.
 */
static int reg_fused_compare(RegOut* o, int negate, uint8_t* kind, uint32_t* operands)
{
    static const uint8_t compares[][3] = {
        /* compare as a slot pair, as slot and constant, its negation's index */
        {WM_R_SS_EQZ, WM_R_SC_EQZ, 0xFF}, {WM_R_SS_EQ, WM_R_SC_EQ, 2},     {WM_R_SS_NE, WM_R_SC_NE, 1},
        {WM_R_SS_LT_S, WM_R_SC_LT_S, 9},  {WM_R_SS_LT_U, WM_R_SC_LT_U, 10}, {WM_R_SS_GT_S, WM_R_SC_GT_S, 7},
        {WM_R_SS_GT_U, WM_R_SC_GT_U, 8},  {WM_R_SS_LE_S, WM_R_SC_LE_S, 5}, {WM_R_SS_LE_U, WM_R_SC_LE_U, 6},
        {WM_R_SS_GE_S, WM_R_SC_GE_S, 3},  {WM_R_SS_GE_U, WM_R_SC_GE_U, 4},
    };
    static const uint8_t branches[][2] = {
        {WM_R_BRIF_SS_EQZ, WM_R_BRIF_SC_EQZ},   {WM_R_BRIF_SS_EQ, WM_R_BRIF_SC_EQ},
        {WM_R_BRIF_SS_NE, WM_R_BRIF_SC_NE},     {WM_R_BRIF_SS_LT_S, WM_R_BRIF_SC_LT_S},
        {WM_R_BRIF_SS_LT_U, WM_R_BRIF_SC_LT_U}, {WM_R_BRIF_SS_GT_S, WM_R_BRIF_SC_GT_S},
        {WM_R_BRIF_SS_GT_U, WM_R_BRIF_SC_GT_U}, {WM_R_BRIF_SS_LE_S, WM_R_BRIF_SC_LE_S},
        {WM_R_BRIF_SS_LE_U, WM_R_BRIF_SC_LE_U}, {WM_R_BRIF_SS_GE_S, WM_R_BRIF_SC_GE_S},
        {WM_R_BRIF_SS_GE_U, WM_R_BRIF_SC_GE_U},
    };
    RegValue top;
    if (!o->pending || o->last == UINT32_MAX)
        return 0;
    top = o->pend[o->pending - 1u];
    if (top.kind != P_SLOT || top.slot != o->last_slot)
        return 0;
    for (uint32_t i = 0; i < sizeof(compares) / sizeof(compares[0]); ++i)
        for (uint32_t shape = 0; shape < 2u; ++shape)
            if (compares[i][shape] == o->last_kind)
            {
                uint32_t c = i;
                *operands = o->cells[o->last + 1u].u32;
                if (negate)
                {
                    if (i == 0)
                    {
                        /* eqz a fails: a is not 0. */
                        c = 2, shape = 1;
                        *operands &= 0xFFFFu;
                    }
                    else
                        c = compares[i][2];
                }
                *kind = branches[c][shape];
                /* The compare's record goes: the branch is written in its place. */
                o->count = o->last;
                o->last = UINT32_MAX;
                --o->pending;
                return 1;
            }
    return 0;
}

static void reg_mov(RegOut* o, uint32_t to, const RegValue* v)
{
    if (v->kind == P_CONST)
    {
        reg_head(o, WM_R_MOVC, to);
        reg_put(o, v->value);
    }
    else if (v->value != to)
    {
        reg_head(o, WM_R_MOV, to);
        reg_put(o, v->value);
    }
}

/* Every pending value into its slot: the frame is the stack machine's. */
static void reg_flush(RegOut* o)
{
    for (uint32_t i = 0; i < o->pending; ++i)
        reg_mov(o, o->pend[i].slot, &o->pend[i]);
    o->pending = 0;
    o->last = UINT32_MAX;
}

/* Pending reads of local slot, before it is written: into their slots. */
static void reg_before_write(RegOut* o, uint32_t slot)
{
    for (uint32_t i = 0; i < o->pending; ++i)
        if (o->pend[i].kind == P_LOCAL && o->pend[i].value == slot)
        {
            reg_mov(o, o->pend[i].slot, &o->pend[i]);
            o->pend[i].kind = P_SLOT;
            o->pend[i].value = o->pend[i].slot;
            o->last = UINT32_MAX;
        }
}

/* The value k from the top (0 the top) of a stack h slots high, all of them one slot. */
static RegValue reg_peek(const RegOut* o, uint32_t h, uint32_t k)
{
    RegValue v;
    if (k < o->pending)
        return o->pend[o->pending - 1u - k];
    v.kind = P_SLOT;
    v.slot = v.value = o->base + h - 1u - k;
    return v;
}

static void reg_push(RegOut* o, uint8_t kind, uint32_t value, uint32_t slot)
{
    if (o->pending == sizeof(o->pend) / sizeof(o->pend[0]))
        reg_flush(o);
    o->pend[o->pending].kind = kind;
    o->pend[o->pending].value = value;
    o->pend[o->pending].slot = slot;
    ++o->pending;
}

/* Drop n values (one slot each) from the top. */
static void reg_pop(RegOut* o, uint32_t n) { o->pending = n < o->pending ? o->pending - n : 0; }

/* An operand that must be in a slot: a constant goes into its own slot first. */
static uint32_t reg_slot(RegOut* o, RegValue v)
{
    if (v.kind == P_CONST)
    {
        reg_mov(o, v.slot, &v);
        o->last = UINT32_MAX;
        return v.slot;
    }
    return v.value;
}

static int reg_i32(uint8_t op, uint8_t* ss, uint8_t* sc, uint8_t* arity)
{
    switch (op)
    {
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR)                                                                    \
    case OP: *ss = WM_R_SS_##NAME, *sc = WM_R_SC_##NAME, *arity = (ARITY); return 1;
#include "wm_ops.def"
        default: return 0;
    }
}

static int reg_commutes(uint8_t op)
{
    return op == 0x46u || op == 0x47u || op == 0x6Au || op == 0x6Cu || op == 0x71u || op == 0x72u || op == 0x73u;
}

/* A place the register loop can be entered from the stack machine's frame: its offset in the module and its record. */
static void reg_entry(WmTail* tail, uint32_t offset, uint32_t cell, uint32_t charge, uint32_t sides, WmResult* error)
{
    if (!tail || *error)
        return;
    if (sides > 0xFFFFu)
    {
        *error = WM_ERR_UNSUPPORTED("unsupported: a function too large for the register loop");
        return;
    }
    if (tail->count == tail->capacity)
    {
        uint32_t capacity = tail->capacity ? tail->capacity * 2u : 32u;
        WmTailEntry* grown = (WmTailEntry*)wm_realloc(tail->entries, capacity * sizeof(WmTailEntry));
        if (!grown)
        {
            *error = WM_ERR_EXHAUSTION("exhaustion: out of memory");
            return;
        }
        tail->entries = grown;
        tail->capacity = capacity;
    }
    tail->entries[tail->count].offset = offset;
    tail->entries[tail->count].cell = cell;
    tail->entries[tail->count].sides = (uint16_t)sides;
    tail->entries[tail->count++].charge = (uint16_t)charge;
}

/*
 * With tail, the function is translated again only for the places the
 * register loop can be entered at (in order of offset): segment starts,
 * and calls and returns, which flush first. Its records stay as they are.
 */
static WmResult reg_translate(WmModule* m, WmCode* code, const WmSide* side, WmTail* tail)
{
    uint32_t index = (uint32_t)(code - m->codes);
    const WmType* type = &m->types[m->func_types[m->func_imports + index]];
    uint32_t length = (uint32_t)(code->end - code->body) + 1u;
    uint32_t B = type->param_slots + code->local_slots;
    uint16_t* heights = (uint16_t*)wm_malloc(length * sizeof(uint16_t));
    uint32_t* cell_at = (uint32_t*)wm_malloc(length * sizeof(uint32_t));
    uint8_t* target = (uint8_t*)wm_calloc(length, 1);
    RegFixup* fixups = NULL;
    uint32_t fixup_count = 0, fixup_capacity = 0;
    RegOut o;
    const WmSide* st = side;
    const uint8_t* p;
    int dead = 0;
    /* Segments: from a label, a call's return or a branch's way on to the next; fuel is charged by them. */
    uint32_t seg_cell = UINT32_MAX, seg_count = 0;
    int need_seg = 1;
    memset(&o, 0, sizeof(o));
    o.handlers = wm_fast_handlers();
    o.last = UINT32_MAX;
    o.base = B;
    if (!o.handlers)
        o.error = WM_ERR_UNSUPPORTED("unsupported: handlers too far apart for predecoding");
    if (!heights || !cell_at || !target)
        o.error = WM_ERR_EXHAUSTION("exhaustion: out of memory");
    if (!o.error)
        o.error = wm_code_heights(m, index, heights);
    if (!o.error && B + code->max_stack > 0xFFF0u)
        o.error = WM_ERR_UNSUPPORTED("unsupported: a frame too large for the register loop");
    /* Labels: where branches land, so the frame there must be the stack machine's. */
    for (p = code->body; !o.error && p <= code->end;)
    {
        uint32_t n, sides;
        uint8_t op = WM_RD8(p);
        p = step(code, p, &n, &sides);
        for (uint32_t i = 0; is_branch(op) && i < sides; ++i)
            if (st[i].pc >= (uint32_t)(code->body - m->bytes) && st[i].pc - (uint32_t)(code->body - m->bytes) < length)
                target[st[i].pc - (uint32_t)(code->body - m->bytes)] = 1;
        st += sides;
    }
    st = side;
#define FIXUP(PC)                                                                                              \
    do                                                                                                         \
    {                                                                                                          \
        if (fixup_count == fixup_capacity)                                                                     \
        {                                                                                                      \
            uint32_t c_ = fixup_capacity ? fixup_capacity * 2u : 16u;                                          \
            RegFixup* g_ = (RegFixup*)wm_realloc(fixups, c_ * sizeof(RegFixup));                               \
            if (!g_)                                                                                           \
            {                                                                                                  \
                o.error = WM_ERR_EXHAUSTION("exhaustion: out of memory");                                      \
                break;                                                                                         \
            }                                                                                                  \
            fixups = g_, fixup_capacity = c_;                                                                  \
        }                                                                                                      \
        fixups[fixup_count].cell = o.count;                                                                    \
        fixups[fixup_count].head = head_cell;                                                                  \
        fixups[fixup_count++].pc = (PC);                                                                       \
        reg_put(&o, 0);                                                                                        \
    } while (0)
    for (p = code->body; !o.error && p <= code->end;)
    {
        const uint8_t* start = p;
        uint32_t at = (uint32_t)(p - code->body);
        uint32_t h = heights[at];
        uint8_t op = WM_RD8(p++);
        uint32_t cells, sides, head_cell, hn;
        const uint8_t* next = step(code, start, &cells, &sides);
        const WmSide* mine = st;
        uint32_t top;
        st += sides;
        if (h == 0xFFFFu && op != 0x05u && op != 0x0Bu)
        {
            dead = 1;
            cell_at[at] = o.count;
            p = next;
            continue;
        }
        if (target[at] && !dead)
            reg_flush(&o);
        cell_at[at] = o.count;
        hn = next <= code->end ? heights[next - code->body] : 0u;
        /* An else or end after code that cannot be reached: branches arrive with the height after it. */
        if (h == 0xFFFFu)
            h = start == code->end ? type->result_slots : hn;
        top = B + h;
        if (target[at] || need_seg)
        {
            if (seg_cell != UINT32_MAX)
            {
                if (seg_count > 0xFFFFu)
                    o.error = WM_ERR_UNSUPPORTED("unsupported: a segment too long for the register loop");
                o.cells[seg_cell].u32 = (o.cells[seg_cell].u32 & 0xFFFFu) | (seg_count << 16);
            }
            reg_flush(&o);
            seg_cell = o.count;
            seg_count = 0;
            need_seg = 0;
            reg_entry(tail, (uint32_t)(start - m->bytes), seg_cell, 0, (uint32_t)(mine - side), &o.error);
            reg_head(&o, WM_R_SEG, 0);
            reg_put(&o, (uint32_t)(start - m->bytes));
            reg_put(&o, top);
        }
        ++seg_count;
        head_cell = o.count;
        if (op == 0x05u || op == 0x0Bu)
        {
            if (!dead || op == 0x05u)
                reg_flush(&o);
            if (op == 0x05u && !dead)
            {
                head_cell = o.count;
                reg_head(&o, WM_R_JUMP, 0);
                FIXUP(mine[0].pc);
            }
            if (start == code->end)
            {
                head_cell = o.count;
                reg_entry(tail, (uint32_t)(start - m->bytes), cell_at[at], 1, (uint32_t)(mine - side), &o.error);
                reg_head(&o, WM_R_RETURN, B + type->result_slots);
            }
            else if (op == 0x05u)
                cell_at[at] = o.count;
            dead = 0;
            o.pending = 0;
            o.last = UINT32_MAX;
            need_seg |= op == 0x05u;
            p = next;
            continue;
        }
        dead = 0;
        switch (op)
        {
            case 0x00:
                reg_flush(&o);
                reg_head(&o, WM_R_UNREACHABLE, 0);
                break;
            case 0x01:
            case 0x02:
            case 0x03: break;
            case 0x04:
            {
                RegValue c;
                uint32_t cs, operands;
                uint8_t kind;
                if (reg_fused_compare(&o, 1, &kind, &operands))
                {
                    reg_flush(&o);
                    head_cell = o.count;
                    reg_head(&o, kind, top - 1u);
                    reg_put(&o, operands);
                    FIXUP(mine[0].pc);
                    reg_put(&o, 0);
                    break;
                }
                c = reg_peek(&o, h, 0);
                reg_pop(&o, 1);
                cs = reg_slot(&o, c);
                reg_flush(&o);
                head_cell = o.count;
                reg_head(&o, WM_R_BR_UNLESS, cs);
                FIXUP(mine[0].pc);
                break;
            }
            case 0x0C:
                reg_flush(&o);
                head_cell = o.count;
                reg_head(&o, WM_R_BR, top);
                FIXUP(mine[0].pc);
                reg_put(&o, mine[0].keep | (mine[0].drop << 16));
                break;
            case 0x0D:
            {
                RegValue c;
                uint32_t cs, operands;
                uint8_t kind;
                if (reg_fused_compare(&o, 0, &kind, &operands))
                {
                    reg_flush(&o);
                    head_cell = o.count;
                    reg_head(&o, kind, top - 1u);
                    reg_put(&o, operands);
                    FIXUP(mine[0].pc);
                    reg_put(&o, mine[0].keep | (mine[0].drop << 16));
                    break;
                }
                c = reg_peek(&o, h, 0);
                reg_pop(&o, 1);
                cs = reg_slot(&o, c);
                reg_flush(&o);
                head_cell = o.count;
                reg_head(&o, WM_R_BR_IF, cs);
                FIXUP(mine[0].pc);
                reg_put(&o, mine[0].keep | (mine[0].drop << 16));
                reg_put(&o, top - 1u);
                break;
            }
            case 0x0E:
            {
                RegValue c = reg_peek(&o, h, 0);
                uint32_t cs;
                reg_pop(&o, 1);
                cs = reg_slot(&o, c);
                reg_flush(&o);
                head_cell = o.count;
                reg_head(&o, WM_R_BR_TABLE, cs);
                reg_put(&o, sides - 1u);
                reg_put(&o, top - 1u);
                for (uint32_t i = 0; i < sides; ++i)
                {
                    FIXUP(mine[i].pc);
                    reg_put(&o, mine[i].keep | (mine[i].drop << 16));
                }
                break;
            }
            case 0x0F:
                reg_entry(tail, (uint32_t)(start - m->bytes), cell_at[at], 1, (uint32_t)(mine - side), &o.error);
                reg_flush(&o);
                reg_head(&o, WM_R_RETURN, top);
                break;
            case 0x10:
                reg_entry(tail, (uint32_t)(start - m->bytes), cell_at[at], 1, (uint32_t)(mine - side), &o.error);
                reg_flush(&o);
                reg_head(&o, WM_R_CALL, leb_u32(&p));
                reg_put(&o, top);
                break;
            case 0x11:
            {
                uint32_t t = leb_u32(&p);
                reg_entry(tail, (uint32_t)(start - m->bytes), cell_at[at], 1, (uint32_t)(mine - side), &o.error);
                reg_flush(&o);
                reg_head(&o, WM_R_CALL_INDIRECT, m->types[t].canon);
                reg_put(&o, leb_u32(&p));
                reg_put(&o, top);
                break;
            }
            case 0x1A:
                if (o.pending && h - hn == 1u)
                    reg_pop(&o, 1);
                else
                    reg_flush(&o);
                break;
            case 0x1B:
            case 0x1C:
                if (h - hn == 2u)
                {
                    RegValue c = reg_peek(&o, h, 0), b = reg_peek(&o, h, 1), a = reg_peek(&o, h, 2);
                    uint32_t cs, bs, as;
                    reg_pop(&o, 3);
                    cs = reg_slot(&o, c), bs = reg_slot(&o, b), as = reg_slot(&o, a);
                    head_cell = o.count;
                    reg_head(&o, WM_R_SELECT1, B + hn - 1u);
                    reg_put(&o, as | (bs << 16));
                    reg_put(&o, cs);
                    reg_made(&o, head_cell, B + hn - 1u);
                }
                else
                {
                    reg_flush(&o);
                    reg_head(&o, WM_R_SELECT, h - hn - 1u);
                    reg_put(&o, top);
                }
                break;
            case 0x20:
            case 0x21:
            case 0x22:
            {
                uint32_t local = leb_u32(&p);
                uint32_t slot = code->local_offsets ? code->local_offsets[local] : local;
                uint32_t width = local_width(code, local);
                if (width != 1u)
                {
                    reg_flush(&o);
                    reg_head(&o, op == 0x20u ? WM_R_LOCAL_GET_W : WM_R_LOCAL_SET_W, slot);
                    reg_put(&o, width);
                    reg_put(&o, op == 0x21u ? top : op == 0x22u ? top + width : top);
                    if (op == 0x22u)
                    {
                        /* tee: set from the top, which stays. */
                        o.cells[o.count - 1u].u32 = top;
                    }
                    break;
                }
                if (op == 0x20u)
                {
                    reg_push(&o, P_LOCAL, slot, top);
                    o.last = UINT32_MAX;
                    break;
                }
                {
                    RegValue v = reg_peek(&o, h, 0);
                    int was_pending = o.pending > 0;
                    if (v.kind == P_LOCAL && v.value == slot)
                    {
                        if (op == 0x21u)
                            reg_pop(&o, 1);
                        break;
                    }
                    reg_pop(&o, 1);
                    /* The record that made the value writes the local instead, when nothing pending reads it. */
                    if (was_pending && v.kind == P_SLOT && o.last != UINT32_MAX && o.last_slot == v.slot)
                    {
                        int read = 0;
                        for (uint32_t i = 0; i < o.pending; ++i)
                            read |= o.pend[i].kind == P_LOCAL && o.pend[i].value == slot;
                        if (!read)
                        {
                            o.cells[o.last].u32 = (o.cells[o.last].u32 & 0xFFFFu) | (slot << 16);
                            o.last = UINT32_MAX;
                            if (op == 0x22u)
                                reg_push(&o, P_LOCAL, slot, v.slot);
                            break;
                        }
                    }
                    reg_before_write(&o, slot);
                    reg_mov(&o, slot, &v);
                    o.last = UINT32_MAX;
                    if (op == 0x22u)
                        reg_push(&o, P_LOCAL, slot, v.slot);
                }
                break;
            }
            case 0x23:
            case 0x24:
            {
                uint32_t g = leb_u32(&p);
                if (wm_type_slots(m->globals[g].type) == 1u)
                {
                    if (op == 0x23u)
                    {
                        reg_head(&o, WM_R_GLOBAL_GET1, top);
                        reg_put(&o, g);
                        reg_made(&o, head_cell, top);
                    }
                    else
                    {
                        RegValue v = reg_peek(&o, h, 0);
                        uint32_t vs;
                        reg_pop(&o, 1);
                        vs = reg_slot(&o, v);
                        head_cell = o.count;
                        reg_head(&o, WM_R_GLOBAL_SET1, g);
                        reg_put(&o, vs);
                        o.last = UINT32_MAX;
                    }
                    break;
                }
                reg_flush(&o);
                reg_head(&o, op == 0x23u ? WM_R_GLOBAL_GET : WM_R_GLOBAL_SET, g);
                reg_put(&o, top);
                break;
            }
            case 0x25:
            case 0x26:
                reg_flush(&o);
                reg_head(&o, op == 0x25u ? WM_R_TABLE_GET : WM_R_TABLE_SET, leb_u32(&p));
                reg_put(&o, top);
                break;
            case 0x3F:
                reg_flush(&o);
                ++p;
                reg_head(&o, WM_R_MEMORY_SIZE, top);
                break;
            case 0x40:
                reg_flush(&o);
                ++p;
                reg_head(&o, WM_R_MEMORY_GROW, top - 1u);
                break;
            case 0x41:
            {
                int64_t c = (int64_t)leb_s64(&p);
                reg_push(&o, P_CONST, (uint32_t)c, top);
                o.last = UINT32_MAX;
                break;
            }
            case 0x43:
            {
                uint32_t bits = 0;
                for (uint32_t i = 0; i < 4u; ++i)
                    bits |= (uint32_t)WM_RD8(p + i) << (8u * i);
                reg_push(&o, P_CONST, bits, top);
                o.last = UINT32_MAX;
                break;
            }
            case 0x42:
            case 0x44:
            {
                uint64_t bits = 0;
                RegValue lo, hi;
                if (op == 0x42u)
                    bits = leb_s64(&p);
                else
                    for (uint32_t i = 0; i < 8u; ++i)
                        bits |= (uint64_t)WM_RD8(p + i) << (8u * i);
                reg_flush(&o);
                lo.kind = hi.kind = P_CONST;
                lo.value = (uint32_t)bits, hi.value = (uint32_t)(bits >> 32);
                reg_mov(&o, top, &lo);
                reg_mov(&o, top + 1u, &hi);
                break;
            }
            case 0xD0:
            case 0xD1:
            case 0xD2:
                reg_flush(&o);
                reg_head(&o, op == 0xD0u ? WM_R_REF_NULL : op == 0xD1u ? WM_R_REF_IS_NULL : WM_R_REF_FUNC,
                         op == 0xD2u ? leb_u32(&p) : 0u);
                reg_put(&o, top);
                break;
            case 0xFC:
            {
                uint32_t sub = leb_u32(&p);
                reg_flush(&o);
                if (sub <= 7u)
                {
                    reg_head(&o, WM_R_SAT, sub);
                    reg_put(&o, top);
                }
                else
                {
                    reg_head(&o, WM_R_MISC, sub);
                    reg_put(&o, (uint32_t)(p - m->bytes));
                    reg_put(&o, top);
                }
                break;
            }
            default:
            {
                uint8_t ss, sc, arity;
                if (op >= 0x28u && op <= 0x3Eu)
                {
                    uint32_t offset;
                    uint8_t load = 0, store = 0;
                    leb_u32(&p);
                    offset = leb_u32(&p);
                    switch (op)
                    {
                        case 0x28: load = WM_R_LOAD32; break;
                        case 0x2C: load = WM_R_LOAD8_S; break;
                        case 0x2D: load = WM_R_LOAD8_U; break;
                        case 0x2E: load = WM_R_LOAD16_S; break;
                        case 0x2F: load = WM_R_LOAD16_U; break;
                        case 0x36: store = WM_R_STORE32; break;
                        case 0x3A: store = WM_R_STORE8; break;
                        case 0x3B: store = WM_R_STORE16; break;
                        default: break;
                    }
                    if (load && offset <= 0xFFFFu)
                    {
                        RegValue a = reg_peek(&o, h, 0);
                        uint32_t as;
                        reg_pop(&o, 1);
                        as = reg_slot(&o, a);
                        head_cell = o.count;
                        reg_head(&o, load, top - 1u);
                        reg_put(&o, as | (offset << 16));
                        reg_made(&o, head_cell, top - 1u);
                    }
                    else if (store && offset <= 0xFFFFu)
                    {
                        RegValue v = reg_peek(&o, h, 0), a = reg_peek(&o, h, 1);
                        uint32_t vs, as;
                        reg_pop(&o, 2);
                        vs = reg_slot(&o, v), as = reg_slot(&o, a);
                        head_cell = o.count;
                        reg_head(&o, store, offset);
                        reg_put(&o, as | (vs << 16));
                        o.last = UINT32_MAX;
                    }
                    else
                    {
                        reg_flush(&o);
                        reg_head(&o, WM_R_ACCESS, op);
                        reg_put(&o, top);
                        reg_put(&o, offset);
                    }
                }
                else if (op >= 0x45u && op <= 0xC4u && reg_i32(op, &ss, &sc, &arity))
                {
                    RegValue b = reg_peek(&o, h, 0), a = arity == 2u ? reg_peek(&o, h, 1) : b;
                    uint32_t d = B + hn - 1u;
                    reg_pop(&o, arity);
                    if (arity == 2u && a.kind == P_CONST && b.kind != P_CONST && reg_commutes(op))
                    {
                        RegValue t = a;
                        a = b, b = t;
                    }
                    if (arity == 2u && b.kind == P_CONST && fits16((int32_t)b.value))
                    {
                        uint32_t as = reg_slot(&o, a);
                        head_cell = o.count;
                        reg_head(&o, sc, d);
                        reg_put(&o, as | (b.value << 16));
                    }
                    else
                    {
                        uint32_t as = reg_slot(&o, a), bs = arity == 2u ? reg_slot(&o, b) : as;
                        head_cell = o.count;
                        reg_head(&o, ss, d);
                        reg_put(&o, as | (bs << 16));
                    }
                    reg_made(&o, head_cell, d);
                }
                else if (op >= 0x45u && op <= 0xC4u)
                {
                    reg_flush(&o);
                    reg_head(&o, WM_R_NUMERIC, op);
                    reg_put(&o, top);
                }
                else
                    o.error = WM_ERR_UNSUPPORTED("unsupported: an instruction the register loop does not know");
            }
        }
        /* What ends a segment: the next instruction starts one. */
        need_seg |= op == 0x00u || op == 0x04u || (op >= 0x0Cu && op <= 0x11u);
        p = next;
    }
#undef FIXUP
    if (seg_cell != UINT32_MAX && !o.error)
        o.cells[seg_cell].u32 = (o.cells[seg_cell].u32 & 0xFFFFu) | ((seg_count > 0xFFFFu ? 0xFFFFu : seg_count) << 16);
    for (uint32_t i = 0; !o.error && i < fixup_count; ++i)
    {
        uint32_t at = fixups[i].pc - (uint32_t)(code->body - m->bytes);
        if (at >= length)
            o.error = WM_ERR_UNSUPPORTED("unsupported: a branch the register loop cannot place");
        else
            o.cells[fixups[i].cell].s32 = (int32_t)cell_at[at] - (int32_t)fixups[i].head;
    }
    if (!o.error && o.count > 0x7FFFu)
        o.error = E_DOES_NOT_FIT;
    if (!o.error && !tail)
    {
        code->own = (WmCell*)wm_code_malloc((size_t)o.count * sizeof(WmCell));
        if (!code->own)
            o.error = WM_ERR_EXHAUSTION("exhaustion: out of memory");
        else
        {
            memcpy(code->own, o.cells, (size_t)o.count * sizeof(WmCell));
            code->fast_cells = (uint16_t)o.count;
            code->mapped = 0;
        }
    }
    wm_free(o.cells);
    wm_free(fixups);
    wm_free(heights);
    wm_free(cell_at);
    wm_free(target);
    return o.error;
}

/* Side table entries owned by the instructions of code before offset (in the module). */
uint32_t wm_reg_sides_before(const WmModule* m, const WmCode* code, uint32_t offset)
{
    uint32_t count = 0;
    for (const uint8_t* p = code->body; p <= code->end && (uint32_t)(p - m->bytes) < offset;)
    {
        uint32_t cells, sides;
        p = step(code, p, &cells, &sides);
        count += sides;
    }
    return count;
}

/* What the loop in place needs to run part of a function that runs predecoded: made once, kept with its records. */
WmResult wm_reg_tail(WmModule* m, WmCode* code, WmTail** out)
{
    uint32_t index = (uint32_t)(code - m->codes);
    WmTail* tail;
    WmResult result;
    if (code->tail)
    {
        *out = code->tail;
        return NULL;
    }
    tail = (WmTail*)wm_calloc(1, sizeof(WmTail));
    if (!tail)
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    if (!(result = wm_code_side(m, index, &tail->side)))
        result = reg_translate(m, code, tail->side, tail);
    if (result)
    {
        wm_reg_tail_free(tail);
        return result;
    }
    code->tail = tail;
    *out = tail;
    return NULL;
}

void wm_reg_tail_free(WmTail* tail)
{
    if (!tail)
        return;
    wm_free(tail->side);
    wm_free(tail->entries);
    wm_free(tail);
}

/* The entry at offset, or NULL. */
const WmTailEntry* wm_reg_entry(const WmTail* tail, uint32_t offset)
{
    uint32_t low = 0, high = tail->count;
    while (low < high)
    {
        uint32_t mid = low + (high - low) / 2u;
        if (tail->entries[mid].offset < offset)
            low = mid + 1u;
        else
            high = mid;
    }
    return low < tail->count && tail->entries[low].offset == offset ? &tail->entries[low] : NULL;
}
#endif
