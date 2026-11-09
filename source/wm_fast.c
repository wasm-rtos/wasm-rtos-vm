/*
 * Predecoding: each function written once more as cells, one record per
 * WebAssembly instruction, for the interpreter's fast loop. A record is the
 * address of its handler, then its immediates decoded; branches hold their
 * target record and the slots to keep and drop. Rare instructions keep
 * their bytes and run through the same helpers as in place.
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

/* The i32 instructions with handlers of their own, by opcode; 0 for the shared numeric one. */
static uint8_t fast_numeric(uint8_t op)
{
    if (op >= 0x45u && op <= 0x4Fu)
        return (uint8_t)(WM_F_EQZ + (op - 0x45u));
    switch (op)
    {
        case 0x6A: return WM_F_ADD;
        case 0x6B: return WM_F_SUB;
        case 0x6C: return WM_F_MUL;
        case 0x71: return WM_F_AND;
        case 0x72: return WM_F_OR;
        case 0x73: return WM_F_XOR;
        case 0x74: return WM_F_SHL;
        case 0x75: return WM_F_SHR_S;
        case 0x76: return WM_F_SHR_U;
        default: return 0;
    }
}

/*
 * One instruction at p: where the next one starts, the cells its record
 * takes and the side table entries it owns.
 */
static const uint8_t* step(const WmCode* code, const uint8_t* p, uint32_t* cells, uint32_t* sides)
{
    uint8_t op = WM_RD8(p++);
    *sides = 0;
    *cells = 1;
    if (op >= 0x45u && op <= 0xC4u)
    {
        *cells = fast_numeric(op) ? 1u : 2u;
        return p;
    }
    switch (op)
    {
        /* nop, block, loop and end: the count of them in a row, from this one on. */
        case 0x01:
        case 0x0B: *cells = 2; break;
        case 0x02:
        case 0x03:
            skip_block_type(&p);
            *cells = 2;
            break;
        case 0x04:
            skip_block_type(&p);
            *cells = 2, *sides = 1;
            break;
        case 0x05: *cells = 2, *sides = 1; break;
        case 0x0C:
        case 0x0D:
            leb_u32(&p);
            *cells = 4, *sides = 1;
            break;
        case 0x0E:
        {
            uint32_t count = leb_u32(&p);
            for (uint32_t i = 0; i <= count; ++i)
                leb_u32(&p);
            *cells = 2u + 3u * (count + 1u), *sides = count + 1u;
            break;
        }
        case 0x10:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0xD2:
            leb_u32(&p);
            *cells = 2;
            break;
        case 0x11:
            leb_u32(&p);
            leb_u32(&p);
            *cells = 3;
            break;
        case 0x1A:
        case 0x1B: *cells = 2, *sides = 1; break;
        case 0x1C:
        {
            uint32_t count = leb_u32(&p);
            p += count;
            *cells = 2;
            break;
        }
        case 0x20:
        case 0x21:
        case 0x22:
            leb_u32(&p);
            *cells = code->local_offsets ? 3u : 2u;
            break;
        case 0x3F:
        case 0x40:
        case 0xD0: ++p; break;
        case 0x41:
            leb_s64(&p);
            *cells = 2;
            break;
        case 0x42:
            leb_s64(&p);
            *cells = 3;
            break;
        case 0x43:
            p += 4;
            *cells = 2;
            break;
        case 0x44:
            p += 8;
            *cells = 3;
            break;
        case 0xFC:
        {
            uint32_t sub = leb_u32(&p);
            if (sub <= 7u)
            {
                *cells = 2;
                break;
            }
            /* The rest keep their immediates in the module: a record of the opcode and where they are. */
            *cells = 3;
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
                leb_u32(&p);
                leb_u32(&p);
                *cells = op == 0x28u || op == 0x36u ? 2u : 3u;
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

/* The record a branch to the module offset pc lands on; NULL if no instruction starts there. */
static WmCell* record_at(const WmCode* code, const WmTarget* targets, uint32_t count, uint32_t pc)
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
    if (low == count || targets[low].pc != pc || targets[low].cell == UINT32_MAX)
        return NULL;
    return code->fast + targets[low].cell;
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
    while (p <= code->end && is_nop(code, p))
    {
        uint32_t cells, sides;
        p = step(code, p, &cells, &sides);
        ++n;
    }
    return n;
}

/* A branch's target record and slot counts, from its side entry. */
static int put_branch(WmCell* out, const WmCode* code, const WmSide* e, const WmTarget* targets, uint32_t count)
{
    out[0].target = record_at(code, targets, count, e->pc);
    out[1].u32 = e->keep;
    out[2].u32 = e->drop;
    return out[0].target != NULL;
}

/*
 * The first record of a common run gets a fused handler. The others keep
 * theirs, so a branch, a stop or a snapshot that lands inside the run finds
 * ordinary records. Kinds after the end of the function are WM_F_COUNT.
 */
static void fuse(const void* const* labels, WmCell* record, uint8_t first, uint8_t second, uint8_t third)
{
    static const struct
    {
        uint8_t first, second, third, fused;
    } runs[] = {
        {WM_F_LOCAL_GET1, WM_F_CONST32, WM_F_ADD, WM_F_GET_CONST_ADD},
        {WM_F_LOCAL_GET1, WM_F_LOCAL_GET1, WM_F_ADD, WM_F_GET_GET_ADD},
        {WM_F_LOCAL_GET1, WM_F_CONST32, WM_F_COUNT, WM_F_GET_CONST},
        {WM_F_LOCAL_GET1, WM_F_LOCAL_GET1, WM_F_COUNT, WM_F_GET_GET},
#if WM_FUSE
        /* Branches with what decides them: a loss where branch prediction is good. */
        {WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_BR_IF, WM_F_SET_GET_BR_IF},
#endif
        {WM_F_LOCAL_SET1, WM_F_LOCAL_GET1, WM_F_COUNT, WM_F_SET_GET},
#if WM_FUSE
        {WM_F_LOCAL_TEE1, WM_F_BR_IF, WM_F_COUNT, WM_F_TEE_BR_IF},
        {WM_F_EQ, WM_F_BR_IF, WM_F_COUNT, WM_F_EQ_BR_IF},
        {WM_F_NE, WM_F_BR_IF, WM_F_COUNT, WM_F_NE_BR_IF},
        {WM_F_EQZ, WM_F_BR_IF, WM_F_COUNT, WM_F_EQZ_BR_IF},
        {WM_F_LE_U, WM_F_BR_IF, WM_F_COUNT, WM_F_LE_U_BR_IF},
        {WM_F_LT_U, WM_F_BR_IF, WM_F_COUNT, WM_F_LT_U_BR_IF},
        {WM_F_EQ, WM_F_IF, WM_F_COUNT, WM_F_EQ_IF},
        {WM_F_NE, WM_F_IF, WM_F_COUNT, WM_F_NE_IF},
        {WM_F_EQZ, WM_F_IF, WM_F_COUNT, WM_F_EQZ_IF},
#endif
        {WM_F_CONST32, WM_F_ADD, WM_F_COUNT, WM_F_CONST_ADD},
        {WM_F_CONST32, WM_F_AND, WM_F_COUNT, WM_F_CONST_AND},
        {WM_F_CONST32, WM_F_SUB, WM_F_COUNT, WM_F_CONST_SUB},
        {WM_F_ADD, WM_F_LOCAL_SET1, WM_F_COUNT, WM_F_ADD_SET},
        {WM_F_LOCAL_GET1, WM_F_I32_LOAD, WM_F_COUNT, WM_F_GET_LOAD},
    };
    if (!record)
        return;
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); ++r)
        if (runs[r].first == first && runs[r].second == second &&
            (runs[r].third == WM_F_COUNT || runs[r].third == third))
        {
            record->label = labels[runs[r].fused];
            return;
        }
}

/*
 * Besides the records themselves it needs RAM only for the branch targets,
 * 8 bytes each, sorted by offset so the records can be matched in one walk.
 */
WmResult wm_predecode(WmModule* m, WmCode* code)
{
    const void* const* labels = wm_fast_labels();
    uint32_t cells = 0, count = 0, k = 0;
    const uint8_t* p;
    const WmSide* st = m->side + code->side;
    WmTarget* targets = NULL;
    WmCell* out;
    /* The last two records, waiting for the kinds after them to see whether they start a run. */
    WmCell* last[2] = {NULL, NULL};
    uint8_t last_kind[2] = {WM_F_COUNT, WM_F_COUNT};
    for (p = code->body; p <= code->end;)
    {
        uint32_t n, sides;
        uint8_t op = WM_RD8(p);
        p = step(code, p, &n, &sides);
        cells += n;
        if (is_branch(op))
            count += sides;
    }
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
        st = m->side + code->side;
    }
    code->fast = (WmCell*)wm_malloc((size_t)cells * sizeof(WmCell));
    if (!code->fast)
    {
        wm_free(targets);
        return WM_ERR_EXHAUSTION("exhaustion: out of memory");
    }
    code->fast_cells = cells;
    out = code->fast;
    for (p = code->body; p <= code->end;)
    {
        const uint8_t* start = p;
        uint8_t op = WM_RD8(p++);
        uint8_t kind;
        int resolved = 1;
        uint32_t n, sides;
        step(code, start, &n, &sides);
        if (op >= 0x45u && op <= 0xC4u)
        {
            kind = fast_numeric(op);
            if (!kind)
            {
                kind = WM_F_NUMERIC;
                out[1].u32 = op;
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
                    out[1].u32 = nop_run(code, start);
                    break;
                case 0x04:
                case 0x05:
                    kind = op == 0x04u ? WM_F_IF : WM_F_JUMP;
                    out[1].target = record_at(code, targets, count, st->pc);
                    resolved = out[1].target != NULL;
                    break;
                case 0x0B:
                    kind = start == code->end ? WM_F_RETURN : WM_F_NOP;
                    out[1].u32 = nop_run(code, start);
                    break;
                case 0x0C:
                case 0x0D:
                    kind = op == 0x0Cu ? WM_F_BR : WM_F_BR_IF;
                    resolved = put_branch(out + 1, code, st, targets, count);
                    break;
                case 0x0E:
                {
                    uint32_t table = sides - 1u;
                    kind = WM_F_BR_TABLE;
                    out[1].u32 = table;
                    for (uint32_t i = 0; i <= table; ++i)
                        resolved &= put_branch(out + 2 + 3u * i, code, st + i, targets, count);
                    break;
                }
                case 0x0F: kind = WM_F_RETURN; break;
                case 0x10:
                    kind = WM_F_CALL;
                    out[1].u32 = leb_u32(&p);
                    break;
                case 0x11:
                    kind = WM_F_CALL_INDIRECT;
                    out[1].u32 = leb_u32(&p);
                    out[2].u32 = leb_u32(&p);
                    break;
                case 0x1A:
                    kind = WM_F_DROP;
                    out[1].u32 = st->keep;
                    break;
                case 0x1B:
                    kind = WM_F_SELECT;
                    out[1].u32 = st->keep;
                    break;
                case 0x1C:
                    leb_u32(&p);
                    kind = WM_F_SELECT;
                    out[1].u32 = wm_type_slots(WM_RD8(p));
                    break;
                case 0x20:
                case 0x21:
                case 0x22:
                {
                    uint32_t index = leb_u32(&p);
                    if (!code->local_offsets)
                    {
                        kind = (uint8_t)(WM_F_LOCAL_GET1 + (op - 0x20u));
                        out[1].u32 = index;
                    }
                    else
                    {
                        kind = (uint8_t)(WM_F_LOCAL_GET + (op - 0x20u));
                        out[1].u32 = code->local_offsets[index];
                        out[2].u32 = code->local_offsets[index + 1u] - code->local_offsets[index];
                    }
                    break;
                }
                case 0x23:
                case 0x24:
                    kind = op == 0x23u ? WM_F_GLOBAL_GET : WM_F_GLOBAL_SET;
                    out[1].u32 = leb_u32(&p);
                    break;
                case 0x25:
                case 0x26:
                    kind = op == 0x25u ? WM_F_TABLE_GET : WM_F_TABLE_SET;
                    out[1].u32 = leb_u32(&p);
                    break;
                case 0x3F: kind = WM_F_MEMORY_SIZE; break;
                case 0x40: kind = WM_F_MEMORY_GROW; break;
                case 0x41:
                    kind = WM_F_CONST32;
                    out[1].u32 = (uint32_t)leb_s64(&p);
                    break;
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
                case 0x43:
                {
                    uint32_t value = 0;
                    for (uint32_t i = 0; i < 4u; ++i)
                        value |= (uint32_t)WM_RD8(p + i) << (8u * i);
                    kind = WM_F_CONST32;
                    out[1].u32 = value;
                    break;
                }
                case 0xD0: kind = WM_F_REF_NULL; break;
                case 0xD1: kind = WM_F_REF_IS_NULL; break;
                case 0xD2:
                    kind = WM_F_REF_FUNC;
                    out[1].u32 = leb_u32(&p);
                    break;
                case 0xFC:
                {
                    uint32_t sub = leb_u32(&p);
                    kind = sub <= 7u ? WM_F_SAT : WM_F_MISC;
                    out[1].u32 = sub;
                    if (sub > 7u)
                        out[2].u32 = (uint32_t)(p - m->bytes);
                    break;
                }
                default:
                    /* Memory accesses: the common i32 ones with their own handlers. */
                    leb_u32(&p);
                    if (op == 0x28u || op == 0x36u)
                    {
                        kind = op == 0x28u ? WM_F_I32_LOAD : WM_F_I32_STORE;
                        out[1].u32 = leb_u32(&p);
                    }
                    else
                    {
                        kind = WM_F_ACCESS;
                        out[1].u32 = op;
                        out[2].u32 = leb_u32(&p);
                    }
            }
        if (!resolved)
        {
            wm_free(targets);
            return WM_ERR_MALFORMED("malformed: branch target");
        }
        out[0].label = labels[kind];
        fuse(labels, last[0], last_kind[0], last_kind[1], kind);
        last[0] = last[1], last_kind[0] = last_kind[1];
        last[1] = out, last_kind[1] = kind;
        st += sides;
        out += n;
        p = step(code, start, &n, &sides);
    }
    fuse(labels, last[0], last_kind[0], last_kind[1], WM_F_COUNT);
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
