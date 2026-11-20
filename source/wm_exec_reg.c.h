/*
 * Experiment (decision 16): the register loop, in place of the fast loop
 * when built with WM_REGISTER. Records name frame slots (wm_fast.c,
 * reg_translate); there is no stack pointer while it runs: a record that
 * works on the stack as the fast loop does gets its top as an operand.
 * Fuel is one a record. Memory in one block only; a paged one traps.
 */
#if WM_PREDECODE
static const uint16_t* g_fast_handlers;
static uint16_t g_fast_offsets[WM_F_COUNT + WM_R_COUNT];
static ptrdiff_t g_fast_shift;

static const char wm_page_stop[] = "page fault";

static WmResult page_fault(WmRuntime* rt)
{
    (void)rt;
    return T_OOB_MEMORY;
}

WM_HOT static WmResult run_fast(WmRuntime* rt, WmFunc* func, int resume)
{
    static const void* const labels[WM_R_COUNT] = {
#define REG(NAME, name) [WM_R_##NAME] = &&r_##name,
#include "wm_reg_ops.def"
    };
    uint32_t entry_depth;
    WmCode* code;
    WmInstance* inst;
    const uint8_t* bytes;
    const WmCell* ip;
    uint32_t* fp;
    uint32_t* sp;
    uint8_t* mem;
    wm_size mem_size;
    WmFunc* callee;
    WmResult trap = NULL;
    WmResult stop;
    uint8_t fuel_on;
    uint32_t fuel;
    const WmCell* target;
    uint32_t keep, drop, top;
    uint32_t cell = 0;
    char* base;

    if (!rt)
    {
        const char* lowest = NULL;
        const char* highest = NULL;
        for (uint32_t k = 0; k < WM_R_COUNT; ++k)
        {
            if (!lowest || (const char*)labels[k] < lowest)
                lowest = (const char*)labels[k];
            if (!highest || (const char*)labels[k] > highest)
                highest = (const char*)labels[k];
        }
        if (!lowest || highest - lowest > 0xFFFF)
            return NULL;
        for (uint32_t k = 0; k < WM_R_COUNT; ++k)
            g_fast_offsets[k] = (uint16_t)((const char*)labels[k] - lowest);
        g_fast_shift = lowest - (const char*)&&r_unreachable;
        g_fast_handlers = g_fast_offsets;
        return NULL;
    }
    base = (char*)&&r_unreachable + g_fast_shift;
    entry_depth = rt->depth;
    fuel_on = rt->fuel_on && rt->nesting == 1u;
    fuel = fuel_on ? rt->fuel : WM_NONE;

#define REFRESH()                                                                                              \
    do                                                                                                         \
    {                                                                                                          \
        code = func->code;                                                                                     \
        inst = func->instance;                                                                                 \
        bytes = inst->module->bytes;                                                                           \
        mem = inst->memory ? inst->memory->data : NULL;                                                        \
        mem_size = mem ? WM_MEMORY_SIZE(inst->memory) : 0;                                                     \
    } while (0)
/* On to the next record; fuel is charged by segments (r_seg). */
#define NEXT()                                                                                                 \
    do                                                                                                         \
    {                                                                                                          \
        cell = (ip++)->u32;                                                                                    \
        goto *(void*)(base + (cell & 0xFFFFu));                                                                \
    } while (0)
/*
 * On into the segment whose SEG record is at T, charging its fuel there
 * instead of dispatching the SEG: what branches do. Short of fuel, stop at
 * the SEG, where the frame is the stack machine's.
 */
#define ENTER(T)                                                                                               \
    do                                                                                                         \
    {                                                                                                          \
        const WmCell* t_ = (T);                                                                                \
        uint32_t n_ = t_->u32 >> 16;                                                                           \
        if (__builtin_expect(fuel < n_, 0))                                                                    \
        {                                                                                                      \
            if (fuel_on)                                                                                       \
            {                                                                                                  \
                ip = t_;                                                                                       \
                stop = wm_fuel_exhausted;                                                                      \
                goto suspend;                                                                                  \
            }                                                                                                  \
            fuel = WM_NONE;                                                                                    \
        }                                                                                                      \
        fuel -= n_;                                                                                            \
        ip = t_ + 3;                                                                                           \
        NEXT();                                                                                                \
    } while (0)
#define IMM (cell >> 16)
#define LO(C) ((C) & 0xFFFFu)
#define HI(C) ((C) >> 16)
#define ADDR(ADDRESS, OFFSET, SIZE, HOST)                                                                      \
    do                                                                                                         \
    {                                                                                                          \
        if ((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size))                   \
            TRAP(mem || !inst->memory ? T_OOB_MEMORY                                                           \
                                      : WM_ERR_UNSUPPORTED("unsupported: paged memory in the register loop")); \
        HOST = mem + (ADDRESS) + (OFFSET);                                                                     \
    } while (0)

    if (resume)
    {
        const WmSuspended* saved = &rt->saved;
        entry_depth = saved->entry_depth;
        func = saved->func;
        ip = (const WmCell*)(const void*)saved->pc;
        fp = saved->fp;
        rt->suspended = 0;
        REFRESH();
        NEXT();
    }
    fp = rt->sp;
    sp = fp + func->param_slots;
    REFRESH();
    if (sp + code->local_slots + code->max_stack > rt->stack_end)
        TRAP(T_STACK);
    zero_slots(sp, code->local_slots);
    ip = code->fast;
    NEXT();

r_unreachable:
    TRAP(WM_ERR_TRAP("trap: unreachable"));
r_seg:
    if (__builtin_expect(fuel < IMM, 0))
    {
        if (fuel_on)
        {
            --ip;
            stop = wm_fuel_exhausted;
            goto suspend;
        }
        fuel = WM_NONE;
    }
    fuel -= IMM;
    ip += 2;
    NEXT();
r_mov:
    fp[IMM] = fp[(ip++)->u32];
    NEXT();
r_movc:
    fp[IMM] = (ip++)->u32;
    NEXT();
r_jump:
    ENTER(ip - 1 + ip[0].s32);
r_br_unless:
    ENTER(fp[IMM] ? ip + 1 : ip - 1 + ip[0].s32);
r_br_if:
    if (!fp[IMM])
        ENTER(ip + 3);
    target = ip - 1 + ip[0].s32;
    keep = LO(ip[1].u32);
    drop = HI(ip[1].u32);
    top = ip[2].u32;
    goto take;
r_br:
    target = ip - 1 + ip[0].s32;
    keep = LO(ip[1].u32);
    drop = HI(ip[1].u32);
    top = IMM;
take:
    if (drop && keep)
        copy_slots(fp + top - keep - drop, fp + top - keep, keep);
    ENTER(target);
r_br_table:
{
    uint32_t count = ip[0].u32;
    uint32_t index = fp[IMM];
    const WmCell* e = ip + 2 + 2u * (index < count ? index : count);
    top = ip[1].u32;
    target = ip - 1 + e[0].s32;
    keep = LO(e[1].u32);
    drop = HI(e[1].u32);
    goto take;
}
r_return:
{
    uint32_t n = func->result_slots;
    WmFrame* frame;
    copy_slots(fp, fp + IMM - n, n);
    if (rt->depth == entry_depth)
    {
        if (fuel_on)
            rt->fuel = fuel;
        return NULL;
    }
    frame = &rt->frames[--rt->depth];
    fp = frame->fp;
    func = frame->func;
    REFRESH();
    /* The caller's segment after the call. */
    ENTER((const WmCell*)(const void*)frame->pc);
}
r_call:
    callee = inst->funcs[IMM];
    sp = fp + (ip++)->u32;
    goto call;
r_call_indirect:
{
    const WmType* type = &inst->module->types[IMM];
    WmTable* table = inst->tables[ip[0].u32];
    uint32_t index;
    sp = fp + ip[1].u32;
    index = *--sp;
    ip += 2;
    if (index >= table->size)
        TRAP(WM_ERR_TRAP("trap: undefined element"));
    callee = (WmFunc*)table->elems[index];
    if (!callee)
        TRAP(WM_ERR_TRAP("trap: uninitialized element"));
    if (callee->type != type && !wm_func_has_type(callee, type))
        TRAP(WM_ERR_TRAP("trap: indirect call type mismatch"));
    goto call;
}
r_select1:
{
    uint32_t c1 = ip[0].u32;
    fp[IMM] = fp[ip[1].u32] ? fp[LO(c1)] : fp[HI(c1)];
    ip += 2;
    NEXT();
}
r_select:
{
    uint32_t n = IMM;
    sp = fp + (ip++)->u32;
    if (!*--sp)
        copy_slots(sp - 2u * n, sp - n, n);
    NEXT();
}
r_local_get_w:
    copy_slots(fp + ip[1].u32, fp + IMM, ip[0].u32);
    ip += 2;
    NEXT();
r_local_set_w:
    copy_slots(fp + IMM, fp + ip[1].u32 - ip[0].u32, ip[0].u32);
    ip += 2;
    NEXT();
r_global_get1:
    fp[IMM] = (uint32_t)inst->globals[(ip++)->u32]->value;
    NEXT();
r_global_set1:
    inst->globals[IMM]->value = fp[(ip++)->u32];
    NEXT();
r_global_get:
    keep = 1;
    goto global;
r_global_set:
    keep = 0;
global:
{
    WmGlobal* g = inst->globals[IMM];
    uint32_t n = wm_type_slots(g->type);
    sp = fp + (ip++)->u32;
    if (keep)
        __builtin_memcpy(sp, &g->value, n * 4u);
    else
        __builtin_memcpy(&g->value, sp - n, n * 4u);
    NEXT();
}
r_table_get:
    keep = 0;
    goto table;
r_table_set:
    keep = 1;
table:
#if WM_HAS_REF_TYPES
{
    WmTable* t = inst->tables[IMM];
    int set = keep != 0;
    uintptr_t r = 0;
    uint32_t i;
    sp = fp + (ip++)->u32;
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
        put_ref(sp, t->elems[i]);
    NEXT();
}
#else
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
r_access:
{
    uint8_t op = (uint8_t)IMM;
    sp = fp + ip[0].u32;
    if (!paged_access(inst->memory, op, ip[1].u32, sp, &trap))
        TRAP(trap);
    ip += 2;
    NEXT();
}
#define REG_LOAD(NAME, name, TYPE)                                                                             \
    r_##name:                                                                                                  \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        TYPE value;                                                                                            \
        uint8_t* host;                                                                                         \
        ADDR(fp[LO(c1)], HI(c1), sizeof(TYPE), host);                                                          \
        __builtin_memcpy(&value, host, sizeof(TYPE));                                                          \
        fp[IMM] = (uint32_t)(int32_t)value;                                                                    \
        NEXT();                                                                                                \
    }
#define REG_STORE(NAME, name, TYPE)                                                                            \
    r_##name:                                                                                                  \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        TYPE value = (TYPE)fp[HI(c1)];                                                                         \
        uint8_t* host;                                                                                         \
        ADDR(fp[LO(c1)], IMM, sizeof(TYPE), host);                                                             \
        __builtin_memcpy(host, &value, sizeof(TYPE));                                                          \
        NEXT();                                                                                                \
    }
#include "wm_reg_ops.def"
r_memory_size:
    fp[IMM] = inst->memory->pages;
    NEXT();
r_memory_grow:
    rt->caller = func;
    fp[IMM] = wm_memory_grow(inst->memory, fp[IMM]);
    REFRESH();
    NEXT();
r_numeric:
{
    uint8_t op = (uint8_t)IMM;
    uint8_t d = WM_ROM8(wm_numeric + (op - 0x45u));
    sp = fp + (ip++)->u32;
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
    NEXT();
}
r_sat:
#if WM_HAS_FLOAT
{
    uint32_t sub = IMM;
    uint8_t d = (uint8_t)(((sub & 2u) ? WM_NUM_F64 : WM_NUM_F32) | (sub >= 4u ? WM_NUM_WIDE : 0));
    sp = fp + (ip++)->u32;
    if (!op_float((uint8_t)(0xE0u + sub), d, sp, &trap))
        TRAP(trap);
    NEXT();
}
#else
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
r_misc:
{
    const uint8_t* immediates = bytes + ip[0].u32;
    sp = fp + ip[1].u32;
    ip += 2;
    rt->caller = func;
    if (!misc(inst, IMM, &immediates, sp, &trap))
        TRAP(trap);
    REFRESH();
    NEXT();
}
#if WM_HAS_REF_TYPES
r_ref_null:
    put_ref(fp + (ip++)->u32, 0);
    NEXT();
r_ref_is_null:
    sp = fp + (ip++)->u32 - WM_REF_SLOTS;
    *sp = get_ref(sp) == 0;
    NEXT();
r_ref_func:
    put_ref(fp + (ip++)->u32, (uintptr_t)inst->funcs[IMM]);
    NEXT();
#else
r_ref_null:
r_ref_is_null:
r_ref_func:
    TRAP(WM_ERR_TRAP("trap: illegal opcode"));
#endif
#define REG_I32(NAME, name)                                                                                    \
    r_ss_##name:                                                                                               \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        fp[IMM] = i32_##name(fp[LO(c1)], fp[HI(c1)]);                                                          \
        NEXT();                                                                                                \
    }                                                                                                          \
    r_sc_##name:                                                                                               \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        fp[IMM] = i32_##name(fp[LO(c1)], (uint32_t)((int32_t)c1 >> 16));                                       \
        NEXT();                                                                                                \
    }
#define REG(NAME, name)
#include "wm_reg_ops.def"
#define REG_CMP(NAME, name)                                                                                    \
    r_brif_ss_##name:                                                                                          \
    {                                                                                                          \
        uint32_t c1 = ip[0].u32;                                                                               \
        if (!i32_##name(fp[LO(c1)], fp[HI(c1)]))                                                               \
            ENTER(ip + 3);                                                                                     \
        goto brif_take;                                                                                        \
    }                                                                                                          \
    r_brif_sc_##name:                                                                                          \
    {                                                                                                          \
        uint32_t c1 = ip[0].u32;                                                                               \
        if (!i32_##name(fp[LO(c1)], (uint32_t)((int32_t)c1 >> 16)))                                            \
            ENTER(ip + 3);                                                                                     \
        goto brif_take;                                                                                        \
    }
#define REG(NAME, name)
#include "wm_reg_ops.def"
brif_take:
    target = ip - 1 + ip[1].s32;
    keep = LO(ip[2].u32);
    drop = HI(ip[2].u32);
    top = IMM;
    goto take;

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
        REFRESH();
        fp = sp - func->param_slots;
        if (sp + code->local_slots + code->max_stack > rt->stack_end)
            TRAP(T_STACK);
        zero_slots(sp, code->local_slots);
        if (__builtin_expect(!code->fast, 0))
        {
            ip = NULL;
            stop = wm_code_stop;
            goto suspend;
        }
        code->used = 1;
        ENTER(code->fast);
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
        REFRESH();
        if (rt->yielding && rt->nesting == 1u)
        {
            rt->yielding = 0;
            stop = wm_yielded;
            goto suspend;
        }
        NEXT();
    }
    TRAP(WM_ERR_TRAP("trap: unlinked import"));

suspend:
    rt->saved.entry_depth = entry_depth;
    rt->saved.func = func;
    rt->saved.pc = (const uint8_t*)(const void*)ip;
    rt->saved.st = NULL;
    rt->saved.fp = fp;
    rt->saved.sp = fp + func->param_slots + func->code->local_slots + func->code->max_stack;
    if (fuel_on)
        rt->fuel = fuel;
    rt->suspended = 1;
    return stop;
#undef IMM
#undef ENTER
#undef LO
#undef HI
#undef ADDR
#undef NEXT
#undef REFRESH
}

const uint16_t* wm_fast_handlers(void)
{
    if (!g_fast_handlers)
        run_fast(NULL, NULL, 0);
    return g_fast_handlers;
}
#endif
