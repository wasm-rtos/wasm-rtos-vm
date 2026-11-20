/*
 * The interpreter loop over the module's own bytes. Branches take their
 * target from the side table, which advances with the code: every
 * instruction that owns an entry either jumps by it or steps past it. Part
 * of wm_exec.c, which includes it after the helpers both loops share.
 */
/*
 * The dispatch: computed goto where the compiler has labels as values (GCC,
 * Clang), a switch elsewhere. Each instruction's code ends with NEXT.
 */
#if WM_COMPUTED_GOTO
#define SWITCH(op)
#define CASE(n) l_##n:
#define DEFAULT l_default:
#define NEXT continue
#define FALLTHROUGH
#else
#define SWITCH(op) switch (op)
#define CASE(n) case n:
#define DEFAULT default:
#define NEXT break
/* Before a case its code runs into: a comment would not reach the compiler through CASE. */
#define FALLTHROUGH __attribute__((fallthrough))
#endif

WM_HOT WmResult wm_execute(WmRuntime* rt, WmFunc* func, int resume)
{
#if WM_PROFILE
    profile_start();
#endif
#if WM_PREDECODE
    /*
     * The loop in place runs part of a function of the register loop: where
     * fuel ends inside a segment, or a snapshot stopped. It keeps to that
     * function and hands back at the first place the register loop can
     * take over (tail->entries).
     */
    int mixed = 0;
    int first = 0;
    WmTail* tail = NULL;
    WmResult result = NULL;
#endif
#if WM_PREDECODE
    if (WM_RUNS_FAST((resume ? rt->saved.func : func)->instance->module))
    {
#if !WM_PREDECODE
        WmResult result = NULL;
#endif
#if WM_PREDECODE
        if (resume && rt->saved.inplace)
        {
            if ((result = wm_reg_tail(rt->saved.func->instance->module, rt->saved.func->code, &tail)))
                return result;
            mixed = 1;
            goto in_place;
        }
#endif
        result = resume ? NULL : wm_code_load(func);
        if (result)
            return result;
        result = run_fast(rt, func, resume);
#if WM_PREDECODE
    fast_checked:
#endif
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
#if WM_PREDECODE
            else if (result == wm_fuel_short)
            {
                /* Stopped at a segment's SEG, its frame the stack machine's: the loop in place counts what fuel is left. */
                WmModule* m = rt->saved.func->instance->module;
                WmCode* c = rt->saved.func->code;
                const WmCell* seg = (const WmCell*)(const void*)rt->saved.pc;
                if (rt->fuel == 0)
                    return wm_fuel_exhausted;
                if ((result = wm_reg_tail(m, c, &tail)))
                {
                    rt->suspended = 0;
                    return result;
                }
                rt->saved.pc = m->bytes + seg[1].u32;
                rt->saved.sp = rt->saved.fp + seg[2].u32;
                rt->saved.st = tail->side + wm_reg_entry(tail, seg[1].u32)->sides;
                rt->saved.inplace = 1;
                mixed = 1;
                first = 1;
                resume = 1;
                goto in_place;
            }
#endif
            else
                return result;
            result = run_fast(rt, func, 1);
        }
    }
#endif
#if WM_PREDECODE
in_place:;
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

#if WM_PREDECODE
#define INPLACE_SIDE (mixed ? tail->side : inst->module->side)
#else
#define INPLACE_SIDE (inst->module->side)
#endif
#define REFRESH()                                                                                              \
    do                                                                                                         \
    {                                                                                                          \
        code = func->code;                                                                                     \
        inst = func->instance;                                                                                 \
        bytes = inst->module->bytes;                                                                           \
        side = INPLACE_SIDE;                                                                                   \
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
        goto enter;
    }

    for (;;)
    {
        uint8_t op;
#if WM_PREDECODE
        if (mixed)
        {
            const WmTailEntry* e = wm_reg_entry(tail, (uint32_t)(pc - bytes));
            /*
             * Where a short segment was handed over, the loop in place starts
             * it, unless it starts with a call or return: never run here.
             */
            if (e && first && !e->charge)
                e = e + 1 < tail->entries + tail->count && e[1].offset == e->offset ? e + 1 : NULL;
            first = 0;
            if (e)
            {
                /* A call or a return: its fuel is charged here, the register loop runs it. */
                if (e->charge && fuel-- == 0)
                {
                    if (fuel_on)
                    {
                        fuel = 0;
                        stop = wm_fuel_exhausted;
                        goto suspend;
                    }
                    fuel = WM_NONE - 1u;
                }
                rt->saved.entry_depth = entry_depth;
                rt->saved.func = func;
                rt->saved.pc = (const uint8_t*)(const void*)(code->fast + e->cell);
                rt->saved.st = NULL;
                rt->saved.fp = fp;
                rt->saved.sp = sp;
                rt->saved.inplace = 0;
                if (fuel_on)
                    rt->fuel = fuel;
                rt->suspended = 1;
                mixed = 0;
                result = run_fast(rt, func, 1);
                goto fast_checked;
            }
        }
#endif
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
        WM_PROFILE_OP(op);
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
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR) [OP] = &&l_i32_##name,
#include "wm_ops.def"
                [0x28] = &&f_0x28, [0x36] = &&f_0x36, [0x20] = &&f_0x20, [0x21] = &&f_0x21, [0x41] = &&f_0x41,};
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
        /*
         * The most common instructions each with a short path, past the shared
         * numeric code: the i32 ones of wm_ops.def, then memory accesses.
         */
#define INPLACE_I32(name, ARITY)                                                                               \
    l_i32_##name:                                                                                              \
    {                                                                                                          \
        uint32_t b = sp[-1];                                                                                   \
        uint32_t a = (ARITY) == 2 ? sp[-2] : b;                                                                \
        sp -= (ARITY) - 1;                                                                                     \
        sp[-1] = i32_##name(a, b);                                                                             \
        continue;                                                                                              \
    }
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR) INPLACE_I32(name, ARITY)
#include "wm_ops.def"
#undef INPLACE_I32
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
                FALLTHROUGH;
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
                /* The function's own end returns. */
                FALLTHROUGH;
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
                const WmType* type = WM_TYPE(inst->module, imm_u32(&pc));
                WmTable* table = inst->tables[imm_u32(&pc)];
                uint32_t index = *--sp;
                if (index >= table->size)
                    TRAP(WM_ERR_TRAP("trap: undefined element"));
                callee = (WmFunc*)table->elems[index];
                if (!callee)
                    TRAP(WM_ERR_TRAP("trap: uninitialized element"));
                if (callee->type != type && !wm_func_has_type(callee, type))
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
                    value = sign_extend(value, size);
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
                if (!(sp = misc(inst, sub, &pc, sp, &trap)))
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
#if WM_PREDECODE
        rt->saved.inplace = (uint8_t)mixed;
#endif
        if (fuel_on)
            rt->fuel = fuel;
        rt->suspended = 1;
        return stop;
    }
}
