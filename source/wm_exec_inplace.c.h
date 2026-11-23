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
        /*
         * In a function that runs in place by itself: its side table stayed
         * with it, pinned on the stack. When the runtime may make records and
         * they fit, it moves to them on the way (decision 21, as engines with
         * two tiers replace a running function on its stack): the loop in
         * place goes on as part of the register loop and hands over at the
         * next segment, with the side table it has.
         */
        if (resume && rt->saved.inplace == 2u)
        {
            WmFunc* f = rt->saved.func;
            if (rt->makes_code && !wm_code_load(f) && !wm_reg_tail(f->instance->module, f->code, &tail))
            {
                rt->saved.inplace = 1;
                mixed = 1;
            }
            goto in_place;
        }
        if (resume && rt->saved.inplace)
        {
            if ((result = wm_reg_tail(rt->saved.func->instance->module, rt->saved.func->code, &tail)))
                return result;
            mixed = 1;
            goto in_place;
        }
#endif
        result = resume ? NULL : code_or_alone(rt, func);
        if (result == wm_code_alone)
            goto in_place;
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
            if (result == wm_code_back)
            {
                /* A function of the register loop returned to a caller running in place by itself. */
                mixed = 0;
                resume = 1;
                goto in_place;
            }
            else if (result == wm_code_stop)
            {
                if ((result = code_or_alone(rt, rt->saved.func)) == wm_code_alone)
                {
                    /* Its frame is made, locals zeroed: it runs in place by itself, from its start. */
                    rt->saved.pc = rt->saved.func->code->body;
                    rt->saved.st = rt->saved.func->code->tail->side;
                    rt->saved.inplace = 2;
                    mixed = 0;
                    resume = 1;
                    goto in_place;
                }
                if (result)
                {
                    rt->suspended = 0;
#if WM_HAS_TRACE
                    rt->trap_func = rt->saved.func;
                    rt->trap_depth = rt->depth;
#endif
                    return result;
                }
                rt->saved.pc = (const uint8_t*)(const void*)rt->saved.func->code->records;
            }
#if WM_PREDECODE
            else if (result == wm_fuel_short)
            {
                /* Stopped at a segment's SEG, its frame the stack machine's: the loop in place counts what fuel is left. */
                WmModule* m = rt->saved.func->instance->module;
                WmCode* c = rt->saved.func->code;
                const WmTailEntry* e;
                if (rt->fuel == 0)
                    return wm_fuel_exhausted;
                if ((result = wm_reg_tail(m, c, &tail)))
                {
                    rt->suspended = 0;
                    return result;
                }
                e = wm_reg_at_cell(tail, (uint32_t)((const WmCell*)(const void*)rt->saved.pc - c->records));
                rt->saved.pc = m->bytes + e->offset;
                rt->saved.sp = rt->saved.fp + e->top;
                rt->saved.st = tail->side + e->sides;
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
/* In a module that runs predecoded each function has its own side table, from 0, in its tail. */
#define INPLACE_SIDE (WM_RUNS_FAST(inst->module) ? (code->tail ? code->tail->side : NULL) : inst->module->side)
#define SIDE_BASE (WM_RUNS_FAST(inst->module) ? 0u : code->side)
#else
#define INPLACE_SIDE (inst->module->side)
#define SIDE_BASE (code->side)
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
        st = side + SIDE_BASE;
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
                rt->saved.pc = (const uint8_t*)(const void*)(code->records + e->cell);
                rt->saved.st = NULL;
                rt->saved.fp = fp;
                rt->saved.sp = sp;
                rt->saved.inplace = 0;
                if (fuel_on)
                    rt->fuel = fuel;
                rt->suspended = 1;
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
                [0 ... 255] = &&l_default, [WM_OP_I32_EQZ ... WM_OP_I64_EXTEND32_S] = &&l_numeric, [WM_OP_UNREACHABLE] = &&l_WM_OP_UNREACHABLE, [WM_OP_NOP] = &&l_WM_OP_NOP, [WM_OP_BLOCK] = &&l_WM_OP_BLOCK, [WM_OP_LOOP] = &&l_WM_OP_LOOP, [WM_OP_IF] = &&l_WM_OP_IF, [WM_OP_BR_IF] = &&l_WM_OP_BR_IF, [WM_OP_ELSE] = &&l_WM_OP_ELSE, [WM_OP_BR] = &&l_WM_OP_BR, [WM_OP_BR_TABLE] = &&l_WM_OP_BR_TABLE, [WM_OP_END] = &&l_WM_OP_END, [WM_OP_RETURN] = &&l_WM_OP_RETURN, [WM_OP_CALL] = &&l_WM_OP_CALL, [WM_OP_CALL_INDIRECT] = &&l_WM_OP_CALL_INDIRECT, [WM_OP_DROP] = &&l_WM_OP_DROP, [WM_OP_SELECT] = &&l_WM_OP_SELECT, [WM_OP_SELECT_T] = &&l_WM_OP_SELECT_T, [WM_OP_LOCAL_GET] = &&l_WM_OP_LOCAL_GET, [WM_OP_LOCAL_SET] = &&l_WM_OP_LOCAL_SET, [WM_OP_LOCAL_TEE] = &&l_WM_OP_LOCAL_TEE, [WM_OP_GLOBAL_GET] = &&l_WM_OP_GLOBAL_GET, [WM_OP_GLOBAL_SET] = &&l_WM_OP_GLOBAL_SET, [WM_OP_I32_LOAD] = &&l_WM_OP_I32_LOAD, [WM_OP_I64_LOAD] = &&l_WM_OP_I64_LOAD, [WM_OP_F32_LOAD] = &&l_WM_OP_F32_LOAD, [WM_OP_F64_LOAD] = &&l_WM_OP_F64_LOAD, [WM_OP_I32_LOAD8_S] = &&l_WM_OP_I32_LOAD8_S, [WM_OP_I32_LOAD8_U] = &&l_WM_OP_I32_LOAD8_U, [WM_OP_I32_LOAD16_S] = &&l_WM_OP_I32_LOAD16_S, [WM_OP_I32_LOAD16_U] = &&l_WM_OP_I32_LOAD16_U, [WM_OP_I64_LOAD8_S] = &&l_WM_OP_I64_LOAD8_S, [WM_OP_I64_LOAD8_U] = &&l_WM_OP_I64_LOAD8_U, [WM_OP_I64_LOAD16_S] = &&l_WM_OP_I64_LOAD16_S, [WM_OP_I64_LOAD16_U] = &&l_WM_OP_I64_LOAD16_U, [WM_OP_I64_LOAD32_S] = &&l_WM_OP_I64_LOAD32_S, [WM_OP_I64_LOAD32_U] = &&l_WM_OP_I64_LOAD32_U, [WM_OP_I32_STORE] = &&l_WM_OP_I32_STORE, [WM_OP_I64_STORE] = &&l_WM_OP_I64_STORE, [WM_OP_F32_STORE] = &&l_WM_OP_F32_STORE, [WM_OP_F64_STORE] = &&l_WM_OP_F64_STORE, [WM_OP_I32_STORE8] = &&l_WM_OP_I32_STORE8, [WM_OP_I32_STORE16] = &&l_WM_OP_I32_STORE16, [WM_OP_I64_STORE8] = &&l_WM_OP_I64_STORE8, [WM_OP_I64_STORE16] = &&l_WM_OP_I64_STORE16, [WM_OP_I64_STORE32] = &&l_WM_OP_I64_STORE32, [WM_OP_MEMORY_SIZE] = &&l_WM_OP_MEMORY_SIZE, [WM_OP_MEMORY_GROW] = &&l_WM_OP_MEMORY_GROW, [WM_OP_I32_CONST] = &&l_WM_OP_I32_CONST, [WM_OP_F32_CONST] = &&l_WM_OP_F32_CONST, [WM_OP_F64_CONST] = &&l_WM_OP_F64_CONST, [WM_OP_PREFIX_FC] = &&l_WM_OP_PREFIX_FC,
#if WM_HAS_I64
                [WM_OP_I64_CONST] = &&l_WM_OP_I64_CONST,
#endif
#if WM_HAS_REF_TYPES
                [WM_OP_TABLE_GET] = &&l_WM_OP_TABLE_GET, [WM_OP_TABLE_SET] = &&l_WM_OP_TABLE_SET, [WM_OP_REF_NULL] = &&l_WM_OP_REF_NULL, [WM_OP_REF_IS_NULL] = &&l_WM_OP_REF_IS_NULL, [WM_OP_REF_FUNC] = &&l_WM_OP_REF_FUNC,
#endif
                /* The short paths last, so they win over the general code of the same opcodes. */
#define WM_RECORD_I32(NAME, name, OP, ARITY, EXPR) [OP] = &&l_i32_##name,
#include "wm_defs.def"
                [WM_OP_I32_LOAD] = &&f_WM_OP_I32_LOAD, [WM_OP_I32_STORE] = &&f_WM_OP_I32_STORE, [WM_OP_LOCAL_GET] = &&f_WM_OP_LOCAL_GET, [WM_OP_LOCAL_SET] = &&f_WM_OP_LOCAL_SET, [WM_OP_I32_CONST] = &&f_WM_OP_I32_CONST,};
#pragma GCC diagnostic pop
            goto *dispatch[op];
        }
    l_numeric:
#else
        if (op >= WM_OP_I32_EQZ && op <= WM_OP_I64_EXTEND32_S)
#endif
        {
            uint8_t d = WM_ROM8(wm_numeric + (op - WM_FIRST_NUMERIC));
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
         * numeric code: the i32 ones of wm_defs.def, then memory accesses.
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
#include "wm_defs.def"
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
    f_WM_OP_LOCAL_GET:
        if (code->local_offsets)
            goto l_WM_OP_LOCAL_GET;
        *sp++ = fp[imm_u32(&pc)];
        if (TAKE_NEXT(WM_OP_LOCAL_GET))
            *sp++ = fp[imm_u32(&pc)];
        else if (TAKE_NEXT(WM_OP_I32_CONST))
            *sp++ = imm_s32(&pc);
        continue;
    f_WM_OP_LOCAL_SET:
        if (code->local_offsets)
            goto l_WM_OP_LOCAL_SET;
        fp[imm_u32(&pc)] = *--sp;
        if (TAKE_NEXT(WM_OP_LOCAL_GET))
            *sp++ = fp[imm_u32(&pc)];
        continue;
    f_WM_OP_I32_CONST:
    {
        uint32_t c = imm_s32(&pc);
        /* A constant is usually an operand: add, and, subtract and compare it at once. */
        if (TAKE_NEXT(WM_OP_I32_ADD))
            sp[-1] += c;
        else if (TAKE_NEXT(WM_OP_I32_AND))
            sp[-1] &= c;
        else if (TAKE_NEXT(WM_OP_I32_SUB))
            sp[-1] -= c;
        else if (TAKE_NEXT(WM_OP_I32_EQ))
            sp[-1] = sp[-1] == c;
        else if (TAKE_NEXT(WM_OP_I32_SHR_U))
            sp[-1] >>= c & 31u;
        else if (TAKE_NEXT(WM_OP_I32_SHL))
            sp[-1] <<= c & 31u;
        else
            *sp++ = c;
        continue;
    }
#undef TAKE_NEXT
    f_WM_OP_I32_LOAD:
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
                if (!(sp = paged_access(inst->memory, WM_OP_I32_LOAD, offset, sp, &trap)))
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
    f_WM_OP_I32_STORE:
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
                if (!(sp = paged_access(inst->memory, WM_OP_I32_STORE, offset, sp, &trap)))
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
            CASE(WM_OP_UNREACHABLE) TRAP(WM_ERR_TRAP("trap: unreachable"));
            CASE(WM_OP_NOP) NEXT;
            CASE(WM_OP_BLOCK)
            CASE(WM_OP_LOOP) skip_block_type(&pc); NEXT;
            CASE(WM_OP_IF)
                skip_block_type(&pc);
                if (*--sp)
                {
                    ++st;
                    NEXT;
                }
                taken = st;
                goto take;
            CASE(WM_OP_BR_IF)
                imm_u32(&pc);
                if (!*--sp)
                {
                    ++st;
                    NEXT;
                }
                FALLTHROUGH;
            CASE(WM_OP_ELSE)
            CASE(WM_OP_BR)
                taken = st;
                goto take;
            CASE(WM_OP_BR_TABLE)
            {
                uint32_t count = imm_u32(&pc);
                uint32_t index = *--sp;
                taken = st + (index < count ? index : count);
                goto take;
            }
            CASE(WM_OP_END)
                if (pc - 1 != code->end)
                    NEXT;
                /* The function's own end returns. */
                FALLTHROUGH;
            CASE(WM_OP_RETURN)
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
#if WM_PREDECODE
                if (!frame->st && WM_RUNS_FAST(frame->func->instance->module))
                {
                    /* Back to a caller of the register loop (decision 21): its records stayed, pinned on the stack. */
                    rt->saved.entry_depth = entry_depth;
                    rt->saved.func = frame->func;
                    rt->saved.pc = frame->pc;
                    rt->saved.st = NULL;
                    rt->saved.fp = frame->fp;
                    rt->saved.sp = sp;
                    rt->saved.inplace = 0;
                    if (fuel_on)
                        rt->fuel = fuel;
                    rt->suspended = 1;
                    func = frame->func;
                    result = run_fast(rt, func, 1);
                    goto fast_checked;
                }
#endif
                pc = frame->pc;
                st = frame->st;
                fp = frame->fp;
                func = frame->func;
                REFRESH();
                NEXT;
            }
            CASE(WM_OP_CALL) callee = inst->funcs[imm_u32(&pc)]; goto call;
            CASE(WM_OP_CALL_INDIRECT)
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
            CASE(WM_OP_DROP)
                sp -= st->keep;
                ++st;
                NEXT;
            CASE(WM_OP_SELECT)
            CASE(WM_OP_SELECT_T)
            {
                uint32_t n;
                if (op == WM_OP_SELECT)
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
            CASE(WM_OP_LOCAL_GET)
            CASE(WM_OP_LOCAL_SET)
            CASE(WM_OP_LOCAL_TEE)
            {
                uint32_t i = imm_u32(&pc);
                uint32_t n;
                /* Most functions have only one-slot locals: a plain word each. */
                if (!code->local_offsets)
                {
                    if (op == WM_OP_LOCAL_GET)
                        *sp++ = fp[i];
                    else
                    {
                        fp[i] = sp[-1];
                        if (op == WM_OP_LOCAL_SET)
                            --sp;
                    }
                    NEXT;
                }
                n = LOCAL_SLOTS(i);
                if (op == WM_OP_LOCAL_GET)
                {
                    copy_slots(sp, LOCAL(i), n);
                    sp += n;
                }
                else
                {
                    copy_slots(LOCAL(i), sp - n, n);
                    if (op == WM_OP_LOCAL_SET)
                        sp -= n;
                }
                NEXT;
            }
            CASE(WM_OP_GLOBAL_GET)
            CASE(WM_OP_GLOBAL_SET)
            {
                WmGlobal* g = inst->globals[imm_u32(&pc)];
                uint32_t n = wm_type_slots(g->type);
                /* The value as one or two slots, the way it lies in memory. */
                if (op == WM_OP_GLOBAL_GET)
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
            CASE(WM_OP_TABLE_GET)
            CASE(WM_OP_TABLE_SET)
            {
                WmTable* t = inst->tables[imm_u32(&pc)];
                uintptr_t r = 0;
                uint32_t i;
                if (op == WM_OP_TABLE_SET)
                {
                    sp -= WM_REF_SLOTS;
                    r = get_ref(sp);
                }
                i = *--sp;
                if (i >= t->size)
                    TRAP(T_OOB_TABLE);
                if (op == WM_OP_TABLE_SET)
                    t->elems[i] = r;
                else
                {
                    put_ref(sp, t->elems[i]);
                    sp += WM_REF_SLOTS;
                }
                NEXT;
            }
#endif
            CASE(WM_OP_I32_LOAD) CASE(WM_OP_I64_LOAD) CASE(WM_OP_F32_LOAD) CASE(WM_OP_F64_LOAD) CASE(WM_OP_I32_LOAD8_S) CASE(WM_OP_I32_LOAD8_U) CASE(WM_OP_I32_LOAD16_S) CASE(WM_OP_I32_LOAD16_U)
            CASE(WM_OP_I64_LOAD8_S) CASE(WM_OP_I64_LOAD8_U) CASE(WM_OP_I64_LOAD16_S) CASE(WM_OP_I64_LOAD16_U) CASE(WM_OP_I64_LOAD32_S) CASE(WM_OP_I64_LOAD32_U)
            CASE(WM_OP_I32_STORE) CASE(WM_OP_I64_STORE) CASE(WM_OP_F32_STORE) CASE(WM_OP_F64_STORE) CASE(WM_OP_I32_STORE8) CASE(WM_OP_I32_STORE16) CASE(WM_OP_I64_STORE8) CASE(WM_OP_I64_STORE16) CASE(WM_OP_I64_STORE32)
            {
                uint8_t d = WM_ROM8(ACCESS + (op - WM_FIRST_ACCESS));
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
                imm_u32(&pc);
                offset = imm_u32(&pc);
                address = *--sp;
                if (offset > mem_size || wm_out_of_bounds(address, offset + size, mem_size))
                    TRAP(T_OOB_MEMORY);
                address += offset;
                if (op >= WM_OP_I32_STORE)
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
            CASE(WM_OP_MEMORY_SIZE)
                ++pc;
                *sp++ = inst->memory->pages;
                NEXT;
            CASE(WM_OP_MEMORY_GROW)
                ++pc;
#if WM_TRACKS_CALLERS
                rt->caller = func;
#endif
                sp[-1] = wm_memory_grow(inst->memory, sp[-1]);
                REFRESH();
                NEXT;
            CASE(WM_OP_I32_CONST) *sp++ = imm_s32(&pc); NEXT;
#if WM_HAS_I64
            CASE(WM_OP_I64_CONST)
                put64(sp, imm_signed(&pc));
                sp += 2;
                NEXT;
#endif
            CASE(WM_OP_F32_CONST)
            CASE(WM_OP_F64_CONST)
            {
                uint8_t n = op == WM_OP_F32_CONST ? 4u : 8u;
                for (uint8_t i = 0; i < n; ++i)
                    ((uint8_t*)sp)[i] = WM_RD8(pc + i);
                pc += n;
                sp += n / 4u;
                NEXT;
            }
#if WM_HAS_REF_TYPES
            CASE(WM_OP_REF_NULL)
            CASE(WM_OP_REF_FUNC)
                put_ref(sp, op == WM_OP_REF_NULL ? (++pc, (uintptr_t)0) : (uintptr_t)inst->funcs[imm_u32(&pc)]);
                sp += WM_REF_SLOTS;
                NEXT;
            CASE(WM_OP_REF_IS_NULL)
                sp -= WM_REF_SLOTS;
                *sp = get_ref(sp) == 0;
                ++sp;
                NEXT;
#endif
            CASE(WM_OP_PREFIX_FC)
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
#if WM_TRACKS_CALLERS
                rt->caller = func;
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
            /* Into another instance: its memory may change too (wm_memory_clean). */
            if (callee->instance != func->instance)
                WM_MEMORY_CHANGES(callee->instance->memory);
            func = callee;
            REFRESH();
            fp = sp - func->param_slots;
#if WM_PREDECODE
            if (WM_RUNS_FAST(inst->module))
            {
                /*
                 * A call from a function running in place by itself (decision
                 * 21): the callee runs on its records when it has them, or
                 * where making them is safe, outside this loop, it gets them
                 * or runs in place too (code_or_alone).
                 */
                if (sp + code->local_slots + code->max_stack > rt->stack_end)
                    TRAP(T_STACK);
                zero_slots(sp, code->local_slots);
                rt->saved.entry_depth = entry_depth;
                rt->saved.func = func;
                rt->saved.st = NULL;
                rt->saved.fp = fp;
                rt->saved.sp = sp + code->local_slots;
                rt->saved.inplace = 0;
                if (fuel_on)
                    rt->fuel = fuel;
                rt->suspended = 1;
                if (code->records)
                {
                    func->code->used = 1;
                    rt->saved.pc = (const uint8_t*)(const void*)code->records;
                    result = run_fast(rt, func, 1);
                }
                else
                {
                    rt->saved.pc = NULL;
                    result = wm_code_stop;
                }
                goto fast_checked;
            }
#endif
            pc = code->body;
            st = side + SIDE_BASE;
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
        rt->saved.inplace = (uint8_t)(mixed ? 1u : WM_RUNS_FAST(inst->module) ? 2u : 0u);
#endif
        if (fuel_on)
            rt->fuel = fuel;
        rt->suspended = 1;
        return stop;
    }
}
