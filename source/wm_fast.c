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
WmResult wm_predecode(WmModule* m, WmCode* code, const WmSide* side)
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

int wm_fast_position(const WmModule* m, const WmCode* code, const WmCell* ip, uint32_t* offset, uint32_t* side)
{
    uint32_t cell = 0;
    uint32_t target = (uint32_t)(ip - code->fast);
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
