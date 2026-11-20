/*
 * The fast loop over predecoded records, and how it stops for a TLB miss.
 * Part of wm_exec.c, which includes it after the helpers both loops share.
 */
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

#if WM_HAS_MMU
/*
 * The host address of size bytes at ea of a paged memory, through the TLB:
 * NULL when the frame is not there, the access crosses it or is not
 * aligned to its size, or (for a store) the frame is not dirty yet.
 */
static WM_ALWAYS_INLINE uint8_t* tlb_host(const WmTlb* tlb, uint32_t ea, uint32_t size, uint32_t space, int store)
{
    const WmTlbEntry* e = &tlb->entries[(ea >> WM_FRAME_LOG2) & (WM_TLB_ENTRIES - 1u)];
    uint32_t want = (ea & (WM_FRAME_MASK | (size - 1u))) | (space << 3);
    return (store ? e->wtag : e->tag) == want ? (uint8_t*)((uintptr_t)ea + e->addend) : NULL;
}

#endif

WM_HOT static WmResult run_fast(WmRuntime* rt, WmFunc* func, int resume)
{
    static const void* const labels[WM_F_COUNT] = {
#define WM_RECORD(NAME, name) [WM_F_##NAME] = &&f_##name,
#if !WM_FUSE
#define WM_RECORD_FUSED_BRANCH(NAME, name)
#endif
#include "wm_ops.def"
    };
    uint32_t entry_depth;
    WmCode* code;
    WmInstance* inst;
    const uint8_t* bytes;
    const WmCell* ip;
    uint32_t* fp;
    uint32_t* sp;
    /*
     * The top of the stack. Its slot, sp[-1], is written only when the stack
     * goes to memory (FAST_SPILL): before a call, a helper, a branch that
     * moves values, a stop. With nothing on the stack tos holds what sp[-1]
     * holds, so writing it there changes nothing.
     */
    uint32_t tos = 0;
    uint8_t* mem;
    wm_size mem_size;
    WmFunc* callee;
    WmResult trap = NULL;
    WmResult stop;
    uint8_t fuel_on;
    uint32_t fuel;
    const WmCell* target;
    /*
     * Slots a branch keeps. Reused as a flag by the records that share code:
     * global get (1) or set (0), table set (1) or get (0).
     */
    uint32_t keep;
    uint32_t drop;
    /* The record's first cell; ip is past it, at the record's other cells. */
    uint32_t cell = 0;
    char* base;

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
#if WM_PROFILE
        for (uint32_t k = 0; k < WM_F_COUNT; ++k)
            if (labels[k])
                g_profile_kinds[g_fast_offsets[k]] = (uint8_t)k;
#endif
        return NULL;
    }
    base = (char*)&&f_unreachable + g_fast_shift;
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
        WM_PROFILE_RECORD(cell & 0xFFFFu);                                                                     \
        goto *(void*)(base + (cell & 0xFFFFu));                                                          \
    } while (0)
#if WM_HAS_MMU
/*
 * HOST = the place of SIZE bytes at ADDRESS + OFFSET: a memory in one block
 * directly, as ever, and first, so its path is the short one; a paged one
 * through the TLB. Not there, a record that can do without, as a fused
 * one, runs MISSING: it leaves the work to the records it stands for.
 */
#define FAST_ADDR_TRY(ADDRESS, OFFSET, SIZE, HOST, STORE, MISSING)                                             \
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
                MISSING;                                                                                       \
        }                                                                                                      \
    } while (0)
/*
 * The same for a record that must make the access. Out of bounds traps;
 * not in the TLB is an exception: back to the record at REC (CELLS long, an
 * access OP), the loop stops as for fuel and wm_execute makes the frame;
 * the record then runs again. Nothing is called from inside the loop for
 * it: on Xtensa a call anywhere in the handlers costs the whole loop
 * registers.
 */
#define FAST_ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, REC, CELLS, STORE)                                          \
    FAST_ADDR_TRY(ADDRESS, OFFSET, SIZE, HOST, STORE, {                                                        \
        if (mem || !inst->memory)                                                                              \
            TRAP(T_OOB_MEMORY);                                                                                \
        rt->fault_address = (ADDRESS);                                                                         \
        rt->fault_offset = (OFFSET);                                                                           \
        rt->fault_op = (OP);                                                                                   \
        rt->fault_cells = (CELLS);                                                                             \
        ip = (REC);                                                                                            \
        goto mmu_fault;                                                                                        \
    })
#else
#define FAST_ADDR_TRY(ADDRESS, OFFSET, SIZE, HOST, STORE, MISSING)                                             \
    do                                                                                                         \
    {                                                                                                          \
        if ((OFFSET) > mem_size || wm_out_of_bounds((ADDRESS), (OFFSET) + (SIZE), mem_size))                   \
            MISSING;                                                                                           \
        HOST = mem + (ADDRESS) + (OFFSET);                                                                     \
    } while (0)
#define FAST_ADDR(ADDRESS, OFFSET, SIZE, HOST, OP, REC, CELLS, STORE)                                          \
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
/*
 * A local written by a record that leaves the stack as it is. With
 * nothing on the stack tos holds the slot below it, the last local: that
 * one is written in tos too, or the next push would write the old value
 * back.
 */
#define FAST_SET_LOCAL(INDEX, VALUE)                                                                           \
    do                                                                                                         \
    {                                                                                                          \
        uint32_t* slot_ = &fp[(INDEX)];                                                                        \
        *slot_ = (VALUE);                                                                                      \
        if (slot_ == sp - 1)                                                                                   \
            tos = *slot_;                                                                                      \
    } while (0)
/* N values off the top: the one below them is the top now. */
#define FAST_POP(N)                                                                                            \
    do                                                                                                         \
    {                                                                                                          \
        sp -= (N);                                                                                             \
        FAST_RELOAD();                                                                                         \
    } while (0)
/* The whole stack in memory, for code that works on it there. */
#define FAST_SPILL() (sp[-1] = tos)
/* A value on top: the one below goes to its slot. */
#define FAST_PUSH(VALUE)                                                                                       \
    do                                                                                                         \
    {                                                                                                          \
        sp[-1] = tos;                                                                                          \
        tos = (VALUE);                                                                                         \
        ++sp;                                                                                                  \
    } while (0)
/* An i32 instruction of wm_ops.def: one operand, tos, or two, the one below and tos. */
#define FAST_I32(name, ARITY)                                                                                  \
    f_##name:                                                                                                  \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = (ARITY) == 2 ? sp[-2] : b;                                                                \
        sp -= (ARITY) - 1;                                                                                     \
        tos = i32_##name(a, b);                                                                                \
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
    FAST_RELOAD();
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
    FAST_POP(1);
    ip = c ? ip : ip - 1 + SIMM;
    FAST_NEXT();
}
f_jump:
    ip = ip - 1 + SIMM;
    FAST_NEXT();
f_br_if:
{
    uint32_t c = tos;
    FAST_POP(1);
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
        FAST_SPILL();
        copy_slots(sp - keep - drop, sp - keep, keep);
        FAST_POP(drop);
    }
    ip = target;
    FAST_NEXT();
f_br_table:
{
    uint32_t count = IMM;
    uint32_t index = tos;
    const WmCell* e = ip + 2u * (index < count ? index : count);
    FAST_POP(1);
    target = ip - 1 + e[0].s32;
    keep = e[1].u32 & 0xFFFFu;
    drop = e[1].u32 >> 16;
    goto take;
}
f_return:
{
    uint32_t n = func->result_slots;
    WmFrame* frame;
    FAST_SPILL();
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
    /* The record has the type as functions point to it (WM_TYPE). */
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
    /* The same type is the same pointer; only another module's or a host's needs comparing. */
    if (callee->type != type && !wm_func_has_type(callee, type))
        TRAP(WM_ERR_TRAP("trap: indirect call type mismatch"));
    /* Below the index the stack is in memory already; tos becomes the last argument. */
    FAST_RELOAD();
    goto call_spilled;
}
f_drop:
    FAST_POP(IMM);
    FAST_NEXT();
f_select:
{
    uint32_t n = IMM;
    if (n == 1u)
    {
        uint32_t c = tos;
        sp -= 2;
        tos = c ? sp[-1] : sp[0];
        FAST_NEXT();
    }
    FAST_SPILL();
    if (!*--sp)
        copy_slots(sp - 2u * n, sp - n, n);
    FAST_POP(n);
    FAST_NEXT();
}
f_local_get1:
    FAST_PUSH(fp[IMM]);
    FAST_NEXT();
f_local_set1:
    fp[IMM] = tos;
    FAST_POP(1);
    FAST_NEXT();
f_local_tee1:
    fp[IMM] = tos;
    FAST_NEXT();
f_local_get:
    FAST_SPILL();
    copy_slots(sp, fp + IMM, ip[0].u32);
    sp += ip[0].u32;
    ++ip;
    FAST_RELOAD();
    FAST_NEXT();
f_local_set:
    FAST_SPILL();
    sp -= ip[0].u32;
    copy_slots(fp + IMM, sp, ip[0].u32);
    ++ip;
    FAST_RELOAD();
    FAST_NEXT();
f_local_tee:
    FAST_SPILL();
    copy_slots(fp + IMM, sp - ip[0].u32, ip[0].u32);
    ++ip;
    FAST_NEXT();
f_global_get1:
    FAST_PUSH((uint32_t)inst->globals[IMM]->value);
    FAST_NEXT();
f_global_set1:
    inst->globals[IMM]->value = tos;
    FAST_POP(1);
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
    FAST_SPILL();
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
    FAST_SPILL();
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
    WM_PROFILE_SHARED(op);
    uint8_t d = WM_ROM8(ACCESS + (op - 0x28u));
    uint8_t size = (uint8_t)(1u << (d & 3u));
    /* A store's value is above the address: slots of it. */
    uint32_t above = op >= 0x36u ? ((d & 8u) ? 2u : 1u) : 0u;
    uint64_t value = 0;
    uint8_t* host;
    /* tos is a load's address or a store's value; a 64-bit value is read from memory. */
    if (above == 2u)
        FAST_SPILL();
    /* Loads by the TLB's load tag, stores by its store tag. */
    FAST_ADDR(above ? sp[-1 - (int32_t)above] : tos, offset, size, host, op, ip - 1, 2u,
              above != 0);
    ++ip;
    if (op >= 0x36u)
    {
        value = above == 2u ? get64(sp - 2) : tos;
        sp -= above + 1u;
        store_bytes(host, value, size);
        FAST_RELOAD();
        FAST_NEXT();
    }
    value = load_bytes(host, size);
    if (d & 4u)
        value = sign_extend(value, size);
    if (d & 8u)
    {
        put64(sp - 1, value);
        ++sp;
        FAST_RELOAD();
    }
    else
        tos = (uint32_t)value;
    FAST_NEXT();
}
    /*
     * The common i32 accesses, the offset as operand. A load's address is
     * tos and its value goes there; a store's value is tos, its address
     * below. Memory is little-endian, as WebAssembly's.
     */
#define FAST_LOAD(NAME, OP, TYPE)                                                                              \
    NAME:                                                                                                      \
    {                                                                                                          \
        TYPE value;                                                                                            \
        uint8_t* host;                                                                                         \
        FAST_ADDR(tos, IMM, sizeof(TYPE), host, OP, ip - 1, 1u, 0);                                            \
        __builtin_memcpy(&value, host, sizeof(TYPE));                                                          \
        tos = (uint32_t)(int32_t)value;                                                                        \
        FAST_NEXT();                                                                                           \
    }
#define FAST_STORE(NAME, OP, TYPE)                                                                             \
    NAME:                                                                                                      \
    {                                                                                                          \
        TYPE value = (TYPE)tos;                                                                                \
        uint8_t* host;                                                                                         \
        FAST_ADDR(sp[-2], IMM, sizeof(TYPE), host, OP, ip - 1, 1u, 1);                                         \
        __builtin_memcpy(host, &value, sizeof(TYPE));                                                          \
        FAST_POP(2);                                                                                           \
        FAST_NEXT();                                                                                           \
    }
#define WM_RECORD_ACCESS(NAME, name, OP, TYPE, KIND) FAST_##KIND(f_##name, OP##u, TYPE)
#include "wm_ops.def"
#undef FAST_STORE
#undef FAST_LOAD
f_memory_size:
    FAST_PUSH(inst->memory->pages);
    FAST_NEXT();
f_memory_grow:
    /* Making frames may drop predecoded code: not this function's. */
    rt->caller = func;
    tos = wm_memory_grow(inst->memory, tos);
    FAST_REFRESH();
    FAST_NEXT();
f_const16:
    FAST_PUSH((uint32_t)SIMM);
    FAST_NEXT();
f_const32:
    FAST_PUSH((ip++)->u32);
    FAST_NEXT();
f_const64:
    FAST_SPILL();
    sp[0] = ip[0].u32;
    sp[1] = tos = ip[1].u32;
    sp += 2;
    ip += 2;
    FAST_NEXT();
f_numeric:
{
    uint8_t op = (uint8_t)IMM;
    uint8_t d = WM_ROM8(wm_numeric + (op - 0x45u));
    WM_PROFILE_SHARED(op);
    FAST_SPILL();
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
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR) FAST_I32(name, ARITY)
#include "wm_ops.def"
#if WM_HAS_REF_TYPES
f_ref_null:
    FAST_SPILL();
    put_ref(sp, 0);
    sp += WM_REF_SLOTS;
    FAST_RELOAD();
    FAST_NEXT();
f_ref_is_null:
    FAST_SPILL();
    sp -= WM_REF_SLOTS;
    *sp = get_ref(sp) == 0;
    ++sp;
    FAST_RELOAD();
    FAST_NEXT();
f_ref_func:
    FAST_SPILL();
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
    FAST_SPILL();
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
    /*
     * Forms (decision 14): an i32 instruction of wm_ops.def with its
     * operands in the records before it, a or b a local's (fp[...]) or a
     * constant, and its result on the stack or into the local of a
     * local.set after it. Each record is one cell; ip is at the second.
     */
    /* i32.const c; op: tos op c */
#define FORM_CONST(name, op)                                                                                   \
    f_##name:                                                                                                  \
    FUSED(1u, f_const16);                                                                                      \
    tos = i32_##op(tos, (uint32_t)SIMM);                                                                       \
    ++ip;                                                                                                      \
    FAST_NEXT();
    /* local.get b; op: tos op b */
#define FORM_GET(name, op)                                                                                     \
    f_##name:                                                                                                  \
    FUSED(1u, f_local_get1);                                                                                   \
    tos = i32_##op(tos, fp[IMM]);                                                                              \
    ++ip;                                                                                                      \
    FAST_NEXT();
    /* op; local.set d: d = below op tos */
#define FORM_SET(name, op)                                                                                     \
    f_##name:                                                                                                  \
    FUSED(1u, f_##op);                                                                                         \
    fp[IMM_OF(ip[0])] = i32_##op(sp[-2], tos);                                                                 \
    FAST_POP(2);                                                                                               \
    ++ip;                                                                                                      \
    FAST_NEXT();
    /* local.get a; local.get b; op: push a op b */
#define FORM_GET_GET(name, op)                                                                                 \
    f_##name:                                                                                                  \
    FUSED(2u, f_local_get1);                                                                                   \
    FAST_PUSH(i32_##op(fp[IMM], fp[IMM_OF(ip[0])]));                                                           \
    ip += 2;                                                                                                   \
    FAST_NEXT();
    /* local.get a; i32.const c; op: push a op c */
#define FORM_GET_CONST(name, op)                                                                               \
    f_##name:                                                                                                  \
    FUSED(2u, f_local_get1);                                                                                   \
    FAST_PUSH(i32_##op(fp[IMM], (uint32_t)SIMM_OF(ip[0])));                                                    \
    ip += 2;                                                                                                   \
    FAST_NEXT();
    /* local.get a; local.get b; op; local.set d: d = a op b */
#define FORM_GET_GET_SET(name, op)                                                                             \
    f_##name:                                                                                                  \
    FUSED(3u, f_local_get1);                                                                                   \
    FAST_SET_LOCAL(IMM_OF(ip[2]), i32_##op(fp[IMM], fp[IMM_OF(ip[0])]));                                      \
    ip += 3;                                                                                                   \
    FAST_NEXT();
    /* local.get a; i32.const c; op; local.set d: d = a op c */
#define FORM_GET_CONST_SET(name, op)                                                                           \
    f_##name:                                                                                                  \
    FUSED(3u, f_local_get1);                                                                                   \
    FAST_SET_LOCAL(IMM_OF(ip[2]), i32_##op(fp[IMM], (uint32_t)SIMM_OF(ip[0])));                               \
    ip += 3;                                                                                                   \
    FAST_NEXT();
#define WM_RECORD_FORM(NAME, name, SHAPE, OP, op) FORM_##SHAPE(name, op)
#include "wm_ops.def"
#undef FORM_CONST
#undef FORM_GET
#undef FORM_SET
#undef FORM_GET_GET
#undef FORM_GET_CONST
#undef FORM_GET_GET_SET
#undef FORM_GET_CONST_SET
f_get_const:
    FUSED(1u, f_local_get1);
    sp[-1] = tos;
    sp[0] = fp[IMM];
    tos = (uint32_t)SIMM_OF(ip[0]);
    sp += 2;
    ++ip;
    FAST_NEXT();
f_get_get:
    FUSED(1u, f_local_get1);
    sp[-1] = tos;
    sp[0] = fp[IMM];
    tos = fp[IMM_OF(ip[0])];
    sp += 2;
    ++ip;
    FAST_NEXT();
f_set_get:
    FUSED(1u, f_local_set1);
    fp[IMM] = tos;
    tos = fp[IMM_OF(ip[0])];
    ++ip;
    FAST_NEXT();
f_tee_const:
    FUSED(1u, f_local_tee1);
    fp[IMM] = tos;
    FAST_PUSH((uint32_t)SIMM_OF(ip[0]));
    ++ip;
    FAST_NEXT();
/* local.set a; local.get b; i32.const c; i32.add: a = tos, b + c on top. */
f_set_get_const_add:
    FUSED(3u, f_local_set1);
    fp[IMM] = tos;
    tos = i32_add(fp[IMM_OF(ip[0])], (uint32_t)SIMM_OF(ip[1]));
    ip += 3;
    FAST_NEXT();
f_set_get_get:
    FUSED(2u, f_local_set1);
    fp[IMM] = tos;
    tos = fp[IMM_OF(ip[0])];
    FAST_PUSH(fp[IMM_OF(ip[1])]);
    ip += 2;
    FAST_NEXT();
/* local.tee a; i32.const c; op: a = tos, tos op c on top. */
#define FAST_TEE_CONST(name, op)                                                                               \
    f_##name:                                                                                                  \
    FUSED(2u, f_local_tee1);                                                                                   \
    fp[IMM] = tos;                                                                                             \
    tos = i32_##op(tos, (uint32_t)SIMM_OF(ip[0]));                                                             \
    ip += 2;                                                                                                   \
    FAST_NEXT();
    FAST_TEE_CONST(tee_const_shr_u, shr_u)
    FAST_TEE_CONST(tee_const_xor, xor)
#undef FAST_TEE_CONST
/* local.tee a; i32.load: a = tos, the load from it on top. */
f_tee_load:
{
    uint8_t* host;
    uint32_t value;
    FUSED(1u, f_local_tee1);
    FAST_ADDR_TRY(tos, IMM_OF(ip[0]), 4u, host, 0, {
        fuel += 1u;
        goto f_local_tee1;
    });
    __builtin_memcpy(&value, host, 4u);
    fp[IMM] = tos;
    tos = value;
    ++ip;
    FAST_NEXT();
}
/* local.get a; local.tee b: b = a, a on top. */
f_get_tee:
{
    uint32_t value = fp[IMM];
    FUSED(1u, f_local_get1);
    FAST_SET_LOCAL(IMM_OF(ip[0]), value);
    FAST_PUSH(value);
    ++ip;
    FAST_NEXT();
}
/* local.get v; i32.store: v to the address on top. */
f_get_store:
{
    uint8_t* host;
    uint32_t value;
    FUSED(1u, f_local_get1);
    FAST_ADDR_TRY(tos, IMM_OF(ip[0]), 4u, host, 1, {
        fuel += 1u;
        goto f_local_get1;
    });
    value = fp[IMM];
    __builtin_memcpy(host, &value, 4u);
    FAST_POP(1);
    ++ip;
    FAST_NEXT();
}
/* local.get i; br_table: by i; the br_table record, at ip, is its head. */
f_get_br_table:
{
    uint32_t count = IMM_OF(ip[0]);
    uint32_t index = fp[IMM];
    const WmCell* e;
    FUSED(1u, f_local_get1);
    e = ip + 1 + 2u * (index < count ? index : count);
    target = ip + e[0].s32;
    keep = e[1].u32 & 0xFFFFu;
    drop = e[1].u32 >> 16;
    goto take;
}
/*
 * local.get, then a load of wm_ops.def from the local's value, the load's
 * offset its record's operand. Out of bounds or not in the TLB: the two
 * records run on their own.
 */
#define FAST_GET_LOAD(name, TYPE)                                                                              \
    f_##name:                                                                                                  \
    {                                                                                                          \
        uint8_t* host;                                                                                         \
        TYPE value;                                                                                            \
        FUSED(1u, f_local_get1);                                                                               \
        FAST_ADDR_TRY(fp[IMM], IMM_OF(ip[0]), sizeof(TYPE), host, 0, {                                        \
            fuel += 1u;                                                                                        \
            goto f_local_get1;                                                                                 \
        });                                                                                                    \
        __builtin_memcpy(&value, host, sizeof(TYPE));                                                          \
        FAST_PUSH((uint32_t)(int32_t)value);                                                                   \
        ++ip;                                                                                                  \
        FAST_NEXT();                                                                                           \
    }
#define WM_RECORD_FUSED_GET_LOAD(NAME, name, LOAD, TYPE) FAST_GET_LOAD(name, TYPE)
#include "wm_ops.def"
#undef FAST_GET_LOAD
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
    FAST_POP(1);
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
    FAST_POP(1);
    if (!fp[IMM_OF(ip[0])])
    {
        ip += 3;
        FAST_NEXT();
    }
    FAST_TAKE_BR_IF(1);
/* An i32 compare of wm_ops.def, then br_if: operands as FAST_I32's, the br_if record after. */
#define FAST_CMP_BR_IF(name, ARITY)                                                                            \
    f_##name##_br_if:                                                                                          \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = (ARITY) == 2 ? sp[-2] : b;                                                                \
        FUSED(1u, f_##name);                                                                                   \
        FAST_POP(ARITY);                                                                                       \
        if (!i32_##name(a, b))                                                                                 \
        {                                                                                                      \
            ip += 2;                                                                                           \
            FAST_NEXT();                                                                                       \
        }                                                                                                      \
        FAST_TAKE_BR_IF(0);                                                                                    \
    }
/* The same, then if: the if record, at ip, is one cell; on to the record after it, or to its target. */
#define FAST_CMP_IF(name, ARITY)                                                                               \
    f_##name##_if:                                                                                             \
    {                                                                                                          \
        uint32_t b = tos;                                                                                      \
        uint32_t a = (ARITY) == 2 ? sp[-2] : b;                                                                \
        FUSED(1u, f_##name);                                                                                   \
        FAST_POP(ARITY);                                                                                       \
        ip = i32_##name(a, b) ? ip + 1 : ip + SIMM_OF(ip[0]);                                                  \
        FAST_NEXT();                                                                                           \
    }
    FAST_CMP_BR_IF(eq, 2)
    FAST_CMP_BR_IF(ne, 2)
    FAST_CMP_BR_IF(eqz, 1)
    FAST_CMP_BR_IF(le_u, 2)
    FAST_CMP_BR_IF(lt_u, 2)
    FAST_CMP_IF(eq, 2)
    FAST_CMP_IF(ne, 2)
    FAST_CMP_IF(eqz, 1)
/* local.get x; local.set a; local.get b; br_if: a = x, branch on b. */
f_get_set_get_br_if:
    FUSED(3u, f_local_get1);
    FAST_SET_LOCAL(IMM_OF(ip[0]), fp[IMM]);
    if (!fp[IMM_OF(ip[1])])
    {
        ip += 4;
        FAST_NEXT();
    }
    FAST_TAKE_BR_IF(2);
/* local.get a; i32.const c; i32.eq; br_if: branch when a is c. */
f_get_const_eq_br_if:
    FUSED(3u, f_local_get1);
    if (fp[IMM] != (uint32_t)SIMM_OF(ip[0]))
    {
        ip += 4;
        FAST_NEXT();
    }
    FAST_TAKE_BR_IF(2);
/* local.get a; i32.const c; i32.ne; if: on into it when a is not c, else to its target. */
f_get_const_ne_if:
    FUSED(3u, f_local_get1);
    ip = fp[IMM] != (uint32_t)SIMM_OF(ip[0]) ? ip + 3 : ip + 2 + SIMM_OF(ip[2]);
    FAST_NEXT();
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
    FAST_SPILL();
    if (!(sp = misc(inst, sub, &immediates, sp, &trap)))
        TRAP(trap);
    FAST_REFRESH();
    FAST_RELOAD();
    FAST_NEXT();
}

call:
    FAST_SPILL();
call_spilled:
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
        /* tos is what sp[-1] holds: a local, zeroed, or else the last argument, which it is already. */
        if (code->local_slots)
            tos = 0;
        /* No records: the loop stops in the new frame, wm_execute has them made and goes on at the first. */
        if (__builtin_expect(!code->fast, 0))
        {
            ip = NULL;
            stop = wm_code_stop;
            goto suspend;
        }
        code->used = 1;
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
    FAST_SPILL();
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
#undef FAST_I32
#undef IMM
#undef SIMM
#undef IMM_OF
#undef SIMM_OF
#undef FAST_RELOAD
#undef FAST_SPILL
#undef FAST_PUSH
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
