/*
 * Memory before code: when the heap has no room for a program's memory,
 * predecoded code goes, being made again when called; never the code of a
 * function that runs. The heap is counted and capped here, and the module
 * is made in place: two functions with long records and one that grows the
 * memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static size_t g_used;
static size_t g_limit = (size_t)-1;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/* realloc with the size in front, refusing what would pass g_limit. */
static void* counted(void* ptr, size_t size)
{
    size_t* block = ptr ? (size_t*)ptr - 2 : NULL;
    size_t old = block ? block[0] : 0;
    size_t* moved;
    if (!size)
    {
        g_used -= old;
        free(block);
        return NULL;
    }
    if (size > old && g_used + (size - old) > g_limit)
        return NULL;
    if (!(moved = (size_t*)realloc(block, size + 2 * sizeof(size_t))))
        return NULL;
    g_used = g_used - old + size;
    moved[0] = size;
    return moved + 2;
}

static uint8_t* put_leb(uint8_t* p, uint32_t v)
{
    do
    {
        uint8_t b = v & 0x7Fu;
        v >>= 7;
        *p++ = (uint8_t)(b | (v ? 0x80u : 0));
    } while (v);
    return p;
}

/* A section: id, size, contents. */
static uint8_t* put_section(uint8_t* p, uint8_t id, const uint8_t* body, uint32_t size)
{
    *p++ = id;
    p = put_leb(p, size);
    memcpy(p, body, size);
    return p + size;
}

/* pairs of i32.const 1, drop: two cells each once predecoded. */
static uint8_t* put_padding(uint8_t* p, uint32_t pairs)
{
    for (uint32_t i = 0; i < pairs; ++i)
        *p++ = 0x41, *p++ = 0x01, *p++ = 0x1A;
    return p;
}

/*
 * func 0 "big" () -> (): 10000 pairs of padding, 80 KB of records.
 * func 1 "grow" (i32) -> i32: memory.grow.
 * func 2 "padded" (i32) -> i32: 5000 pairs of padding, then memory.grow.
 * One memory of one page.
 */
static uint32_t make_module(uint8_t* out)
{
    static uint8_t code[200000];
    static const uint8_t types[] = {2, 0x60, 0, 0, 0x60, 1, 0x7F, 1, 0x7F};
    static const uint8_t funcs[] = {3, 0, 1, 1};
    static const uint8_t memory[] = {1, 0, 1};
    static const uint8_t exports[] = {3, 3, 'b', 'i', 'g', 0, 0, 4, 'g', 'r', 'o', 'w', 0, 1,
                                      6, 'p', 'a', 'd', 'd', 'e', 'd', 0, 2};
    static const uint8_t magic[] = {0, 'a', 's', 'm', 1, 0, 0, 0};
    uint8_t body[60000];
    uint8_t* c = code;
    uint8_t* b;
    uint8_t* p = out;
    *c++ = 3;
    /* big */
    b = body;
    *b++ = 0;
    b = put_padding(b, 10000);
    *b++ = 0x0B;
    c = put_leb(c, (uint32_t)(b - body));
    memcpy(c, body, (size_t)(b - body)), c += b - body;
    /* grow */
    b = body;
    *b++ = 0, *b++ = 0x20, *b++ = 0, *b++ = 0x40, *b++ = 0, *b++ = 0x0B;
    c = put_leb(c, (uint32_t)(b - body));
    memcpy(c, body, (size_t)(b - body)), c += b - body;
    /* padded */
    b = body;
    *b++ = 0;
    b = put_padding(b, 5000);
    *b++ = 0x20, *b++ = 0, *b++ = 0x40, *b++ = 0, *b++ = 0x0B;
    c = put_leb(c, (uint32_t)(b - body));
    memcpy(c, body, (size_t)(b - body)), c += b - body;

    memcpy(p, magic, sizeof(magic)), p += sizeof(magic);
    p = put_section(p, 1, types, sizeof(types));
    p = put_section(p, 3, funcs, sizeof(funcs));
    p = put_section(p, 5, memory, sizeof(memory));
    p = put_section(p, 7, exports, sizeof(exports));
    p = put_section(p, 10, code, (uint32_t)(c - code));
    return (uint32_t)(p - out);
}

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

int main(void)
{
    static uint8_t bytes[200000];
    uint32_t size, arg = 1, result = 0;
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmRuntime* rt;

    wm_set_allocator(counted);
    size = make_module(bytes);
    rt = wm_runtime_new(16u * 1024u, 64u);
    check(!wm_module_load(&module, bytes, size) && !wm_instantiate(&inst, module, rt, NULL, NULL), "instantiate");
    if (!inst)
        return 1;
    if (wm_module_predecoded(module))
    {
        printf("SKIP the module runs in place: %s\n", wm_module_predecoded(module));
        return 0;
    }

    /* A: the records of big are all that can give the room a new page needs. */
    check(!wm_call(rt, export_func(inst, "big"), NULL, NULL) && wm_code_cache_bytes() >= 80000u,
          "big ran: its records are in the cache");
    g_limit = g_used + 16u * 1024u;
    check(!wm_call(rt, export_func(inst, "grow"), &arg, &result) && result == 1,
          "memory.grow by 64 KiB in a heap with 16 KiB to spare: done, the cache gave the rest");
    check(wm_code_cache_bytes() < 80000u, "big's records went for it");
    g_limit = (size_t)-1;
    check(!wm_call(rt, export_func(inst, "big"), NULL, NULL), "big runs again: its records are made again");

    /* B: the only records are those of the function that grows: they stay, and the growth fails. */
    wm_code_cache_trim(0);
    arg = 0;
    check(!wm_call(rt, export_func(inst, "padded"), &arg, &result) && result == 2 && wm_code_cache_bytes() >= 40000u,
          "padded ran (growing by nothing): its records are the cache's only ones");
    arg = 1;
    g_limit = g_used + 16u * 1024u;
    check(!wm_call(rt, export_func(inst, "padded"), &arg, &result) && result == 0xFFFFFFFFu,
          "padded grows with no room and no other code to drop: memory.grow says -1");
    check(wm_code_cache_bytes() >= 40000u, "padded kept its own records while it ran");
    g_limit = (size_t)-1;

    wm_instance_free(inst);
    wm_module_free(module);
    wm_runtime_free(rt);
    check(g_used == 0, "everything given back");
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
