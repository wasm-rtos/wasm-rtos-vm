/*
 * Experiment (decision 16): the register loop, in place of the fast loop
 * when built with WM_REGISTER. Records name frame slots (wm_fast.c,
 * reg_translate); there is no stack pointer while it runs: a record that
 * works on the stack as the fast loop does gets its top as an operand.
 * Fuel is one a record. Memory in one block only; a paged one traps.
 */
#if WM_PREDECODE
static const uint16_t* g_fast_handlers;
static uint16_t g_fast_offsets[WM_R_COUNT];
static ptrdiff_t g_fast_shift;

#if WM_HAS_MMU
static const char wm_page_stop[] = "page fault";
#endif
const char wm_fuel_short[] = "fuel short of a segment";

#if WM_HAS_MMU
/* The host address of size bytes at ea of a paged memory, through the TLB, or NULL (as the fast loop's). */
static WM_ALWAYS_INLINE uint8_t* tlb_host(const WmTlb* tlb, uint32_t ea, uint32_t size, uint32_t space, int store)
{
    const WmTlbEntry* e = &tlb->entries[(ea >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];
    uint32_t want = (ea & (WM_FRAME_MASK | (size - 1u))) | (space << 3);
    return (store ? e->wtag : e->tag) == want ? (uint8_t*)((uintptr_t)ea + e->addend) : NULL;
}

/*
 * The register loop stopped at an access of a paged memory the TLB did not
 * have (rt->fault_*, the record at saved.pc): make the frame and fill the
 * TLB, the record runs again; or make the access here through the memory
 * functions, its operands and result in their slots, and step past it.
 * While a frame is read in the background, the stack is made the stack
 * machine's there and the loop in place goes on with the access.
 */
static WmResult page_fault(WmRuntime* rt)
{
    WmFunc* func = rt->saved.func;
    WmMemory* memory = func->instance->memory;
    const WmCell* rec = (const WmCell*)(const void*)rt->saved.pc;
    uint32_t* fp = rt->saved.fp;
    uint8_t op = rt->fault_op;
    uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    int store = op >= 0x36u;
    WmResult result = wm_page_fault(rt->tlb, memory, rt->fault_address, rt->fault_offset, size, store);
    if (result == wm_page_emulate)
    {
        uint64_t ea = (uint64_t)rt->fault_address + rt->fault_offset;
        uint32_t c1 = rec[1].u32;
        uint8_t bytes[4];
        uint32_t value = 0;
        if (ea > 0xFFFFFFFFu)
            return T_OOB_MEMORY;
        if (store)
        {
            value = fp[c1 >> 16];
            for (uint32_t i = 0; i < size; ++i)
                bytes[i] = (uint8_t)(value >> (8u * i));
            result = wm_memory_write(memory, (uint32_t)ea, bytes, size);
        }
        else if (!(result = wm_memory_read(memory, (uint32_t)ea, bytes, size)))
        {
            for (uint32_t i = 0; i < size; ++i)
                value |= (uint32_t)bytes[i] << (8u * i);
            fp[rec[0].u32 >> 16] = (uint32_t)((d & 4u) ? sign_extend(value, size) : value);
        }
        if (!result)
            rt->saved.pc = (const uint8_t*)(const void*)(rec + 2);
        return result;
    }
    if (result == wm_page_wait)
    {
        WmModule* m = func->instance->module;
        WmTail* tail;
        uint32_t offset, top;
        WmResult made;
        if ((made = wm_reg_tail(m, func->code, &tail)) ||
            (made = wm_reg_stack_at(m, func->code, (uint32_t)(rec - func->code->fast), fp, &offset, &top)))
            return made;
        rt->saved.pc = m->bytes + offset;
        rt->saved.sp = fp + top;
        rt->saved.st = tail->side + wm_reg_sides_before(m, func->code, offset);
        rt->saved.inplace = 1;
    }
    return result;
}
#endif

WM_HOT static WmResult run_fast(WmRuntime* rt, WmFunc* func, int resume)
{
    static const void* const labels[WM_R_COUNT] = {
#define WM_REC(NAME, name) [WM_R_##NAME] = &&r_##name,
#include "wm_records.def"
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
#if WM_PROFILE
        for (uint32_t k = 0; k < WM_R_COUNT; ++k)
            g_profile_kinds[g_fast_offsets[k]] = (uint8_t)k;
#endif
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
        WM_PROFILE_RECORD(cell & 0xFFFFu);                                                                     \
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
                stop = wm_fuel_short;                                                                          \
                goto suspend;                                                                                  \
            }                                                                                                  \
            fuel = WM_NONE;                                                                                    \
        }                                                                                                      \
        fuel -= n_;                                                                                            \
        ip = t_ + 1;                                                                                           \
        NEXT();                                                                                                \
    } while (0)
#define IMM (cell >> 16)
#define SIMM ((int32_t)cell >> 16)
#define LO(C) ((C) & 0xFFFFu)
#define HI(C) ((C) >> 16)
#if WM_HAS_MMU
/*
 * HOST = the place of SIZE bytes at ADDRESS + OFFSET: a memory in one block
 * first and directly; a paged one through the TLB, and when it is not
 * there the loop stops at the record (ip - 2) for page_fault.
 */
#define ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, STORE)                                                           \
    do                                                                                                         \
    {                                                                                                          \
        if (__builtin_expect(!((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size)), \
                             1))                                                                               \
            HOST = mem + (ADDRESS) + (OFFSET);                                                                 \
        else                                                                                                   \
        {                                                                                                      \
            uint32_t ea_ = (ADDRESS) + (OFFSET);                                                               \
            HOST = !mem && inst->memory && ea_ >= (OFFSET)                                                     \
                       ? tlb_host(rt->tlb, ea_, (SIZE), inst->memory->space, (STORE))                          \
                       : NULL;                                                                                 \
            if (!HOST)                                                                                         \
            {                                                                                                  \
                if (mem || !inst->memory)                                                                      \
                    TRAP(T_OOB_MEMORY);                                                                        \
                rt->fault_address = (ADDRESS);                                                                 \
                rt->fault_offset = (OFFSET);                                                                   \
                rt->fault_op = (OP);                                                                           \
                rt->fault_cells = 2u;                                                                          \
                ip -= 2;                                                                                       \
                stop = wm_page_stop;                                                                           \
                goto suspend;                                                                                  \
            }                                                                                                  \
        }                                                                                                      \
    } while (0)
#else
#define ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, STORE)                                                           \
    do                                                                                                         \
    {                                                                                                          \
        if ((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size))                   \
            TRAP(T_OOB_MEMORY);                                                                                \
        HOST = mem + (ADDRESS) + (OFFSET);                                                                     \
    } while (0)
#endif

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
            stop = wm_fuel_short;
            goto suspend;
        }
        fuel = WM_NONE;
    }
    fuel -= IMM;
    NEXT();
r_mov:
    fp[IMM] = fp[(ip++)->u32];
    NEXT();
r_movc:
    fp[IMM] = (ip++)->u32;
    NEXT();
r_movc64:
    fp[IMM] = ip[0].u32;
    fp[IMM + 1u] = ip[1].u32;
    ip += 2;
    NEXT();
r_jump:
r_br0:
    ENTER(ip - 1 + SIMM);
r_br_if0:
    if (!fp[IMM])
        ENTER(ip + 1);
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
r_access_far:
    keep = ip[1].u32;
    sp = fp + ip[0].u32;
    ip += 2;
    goto access;
r_access:
    keep = HI(ip[0].u32);
    sp = fp + LO(ip[0].u32);
    ++ip;
access:
{
    /* Any other access, on the slots below its top (sp), the offset in keep: in one block here, a paged memory through its functions. */
    uint8_t op = (uint8_t)IMM;
    uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    uint32_t offset = keep;
    uint32_t above = op >= 0x36u ? ((d & 8u) ? 2u : 1u) : 0u;
    uint8_t* host;
    WM_PROFILE_SHARED(op);
#if WM_HAS_MMU
    if (!mem && inst->memory)
    {
        if (!paged_access(inst->memory, op, offset, sp, &trap))
            TRAP(trap);
        NEXT();
    }
#endif
    if (offset > mem_size || wm_out_of_bounds(sp[-1 - (int32_t)above], offset + size, mem_size))
        TRAP(T_OOB_MEMORY);
    host = mem + sp[-1 - (int32_t)above] + offset;
    if (above)
        store_bytes(host, above == 2u ? get64(sp - 2) : sp[-1], size);
    else
    {
        uint64_t value = load_bytes(host, size);
        if (d & 4u)
            value = sign_extend(value, size);
        if (d & 8u)
            put64(sp - 1, value);
        else
            sp[-1] = (uint32_t)value;
    }
    NEXT();
}
#define WM_REC_LOAD(NAME, name, TYPE, OP)                                                                         \
    r_##name:                                                                                                  \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        TYPE value;                                                                                            \
        uint8_t* host;                                                                                         \
        ADDR(fp[LO(c1)], HI(c1), sizeof(TYPE), host, (OP), 0);                                                \
        __builtin_memcpy(&value, host, sizeof(TYPE));                                                          \
        fp[IMM] = (uint32_t)(int32_t)value;                                                                    \
        NEXT();                                                                                                \
    }
#define WM_REC_STORE(NAME, name, TYPE, OP)                                                                        \
    r_##name:                                                                                                  \
    {                                                                                                          \
        uint32_t c1 = (ip++)->u32;                                                                             \
        TYPE value = (TYPE)fp[HI(c1)];                                                                         \
        uint8_t* host;                                                                                         \
        ADDR(fp[LO(c1)], IMM, sizeof(TYPE), host, (OP), 1);                                                    \
        __builtin_memcpy(host, &value, sizeof(TYPE));                                                          \
        NEXT();                                                                                                \
    }
#include "wm_records.def"
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
    WM_PROFILE_SHARED(op);
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
#define WM_REC_I32(NAME, name)                                                                                    \
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
#define WM_REC(NAME, name)
#include "wm_records.def"
#define WM_REC_CMP(NAME, name)                                                                                 \
    r_brif_ss_##name:                                                                                          \
    {                                                                                                          \
        uint32_t c1 = ip[0].u32;                                                                               \
        if (!i32_##name(fp[LO(c1)], fp[HI(c1)]))                                                               \
            ENTER(ip + 1);                                                                                     \
        ENTER(ip - 1 + SIMM);                                                                                  \
    }                                                                                                          \
    r_brif_sc_##name:                                                                                          \
    {                                                                                                          \
        uint32_t c1 = ip[0].u32;                                                                               \
        if (!i32_##name(fp[LO(c1)], (uint32_t)((int32_t)c1 >> 16)))                                            \
            ENTER(ip + 1);                                                                                     \
        ENTER(ip - 1 + SIMM);                                                                                  \
    }
#define WM_REC(NAME, name)
#include "wm_records.def"

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
        /* Into another instance: its memory may change too (wm_memory_clean). */
        if (callee->instance != func->instance)
            WM_MEMORY_CHANGES(callee->instance->memory);
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
    /*
     * Stops come at a segment's SEG or at a function's start. Where the
     * stack's top is at a SEG the function's tail says (wm_reg_segment),
     * when something needs it: the frame's locals end here.
     */
    rt->saved.sp = fp + func->param_slots + func->code->local_slots;
    rt->saved.inplace = 0;
    if (fuel_on)
        rt->fuel = fuel;
    rt->suspended = 1;
    return stop;
#undef IMM
#undef SIMM
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
