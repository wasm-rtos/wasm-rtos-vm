/*
 * Code images (decision 15): a module's records written once and mapped.
 * Calls give what they give with records made in RAM, whether the cache
 * copies records in from the image or gets no memory and runs every
 * function from it, in slices of fuel through snapshots that another
 * instance, without the image, goes on from. An image of another module
 * or build, cut, misaligned or changed anywhere is refused: by its CRC,
 * or, with the CRC made again, by the records made again and compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint32_t g_code[1 << 12];
static uint32_t g_code_size;
static uint8_t g_snapshot[1 << 16];
static uint32_t g_length, g_at;
static WmInstance* g_inst;
static WmFunc* g_back;
static int g_code_refused;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

/* square(n), called back in, with every record that can go gone first. */
static WmResult host_back(WmRuntime* rt, void* user, uint32_t* slots, WmMemory* memory)
{
    uint32_t result = 0;
    WmResult r;
    (void)user, (void)memory;
    wm_code_cache_trim(0);
    r = wm_call(rt, export_func(g_inst, "square"), slots, &result);
    wm_code_cache_trim(0);
    slots[0] = result;
    return r;
}

static WmResult resolve(void* c, const char* mo, uint32_t ml, const char* name, uint32_t nl, uint8_t k, WmExtern* out)
{
    (void)c, (void)mo, (void)ml, (void)name, (void)nl, (void)k;
    out->kind = WM_EXTERN_FUNC;
    out->as.func = g_back;
    return NULL;
}

/* The code cache's memory, none while g_code_refused. */
static void* code_allocator(void* ptr, size_t size)
{
    if (!size)
    {
        free(ptr);
        return NULL;
    }
    return g_code_refused ? NULL : realloc(ptr, size);
}

static WmResult put_code(void* context, const void* data, uint32_t size)
{
    (void)context;
    if (g_code_size + size > sizeof(g_code))
        return "exhaustion: image too large";
    memcpy((uint8_t*)g_code + g_code_size, data, size);
    g_code_size += size;
    return NULL;
}

static WmResult put(void* context, const void* data, uint32_t size)
{
    (void)context;
    if (g_length + size > sizeof(g_snapshot))
        return "exhaustion: snapshot too large";
    memcpy(g_snapshot + g_length, data, size);
    g_length += size;
    return NULL;
}

static WmResult get(void* context, void* data, uint32_t size)
{
    (void)context;
    if (g_at + size > g_length)
        return "malformed: snapshot too short";
    memcpy(data, g_snapshot + g_at, size);
    g_at += size;
    return NULL;
}

static uint32_t crc32(const void* data, uint32_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Whether a fresh module of bytes maps image (size bytes at offset bytes into a copy). */
static WmResult map_copy(const uint8_t* bytes, uint32_t size, const uint32_t* image, uint32_t image_size,
                         uint32_t offset)
{
    static uint32_t copy[(1 << 12) + 1];
    WmModule* module;
    WmResult r;
    if (wm_module_load(&module, bytes, size))
        return "test: cannot load";
    memcpy((uint8_t*)copy + offset, image, image_size);
    r = wm_code_image_map(module, (const uint8_t*)copy + offset, image_size);
    wm_module_free(module);
    return r;
}

/* The image with word at changed by delta, its CRC made again to match unless keep_crc. */
static WmResult map_changed(const uint8_t* bytes, uint32_t size, uint32_t at, uint32_t delta, int keep_crc)
{
    static uint32_t changed[1 << 12];
    uint32_t words = g_code_size / 4u;
    memcpy(changed, g_code, g_code_size);
    changed[at] += delta;
    if (!keep_crc)
        changed[words - 1u] = crc32(changed, g_code_size - 4u);
    return map_copy(bytes, size, changed, g_code_size, 0);
}

/* f(arg) in slices of fuel, each slice resumed from a snapshot in the instance next. */
static WmResult run_sliced(WmRuntime** rt, WmFunc* f, uint32_t arg, WmInstance* next, uint32_t* result)
{
    WmResult r;
    wm_set_fuel(*rt, 37);
    r = wm_call(*rt, f, &arg, result);
    while (r == wm_fuel_exhausted)
    {
        g_length = g_at = 0;
        if ((r = wm_snapshot_write(*rt, g_inst, put, NULL)))
            return r;
        wm_runtime_free(*rt);
        *rt = wm_runtime_new(64u * 1024u, 100u);
        if ((r = wm_snapshot_read(*rt, next, get, NULL)))
            return r;
        wm_set_fuel(*rt, 37);
        r = wm_resume(*rt, result);
    }
    return r;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096], other[4096];
    static const uint8_t i32[1] = {0x7F};
    char what[96];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/code.wasm", "rb");
    FILE* g = fopen("test/refs.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    uint32_t other_size = g ? (uint32_t)fread(other, 1, sizeof(other), g) : 0;
    WmModule *module = NULL, *plain = NULL;
    WmInstance* in_place = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    uint32_t result = 0, arg, words;
    WmResult r;
    if (f)
        fclose(f);
    if (g)
        fclose(g);
    g_back = wm_host_function_new(i32, 1, i32, 1, host_back, NULL);
    wm_set_code_allocator(code_allocator);

    check(size && !wm_module_load(&module, bytes, size), "load");
    if (!module)
        return 1;
    r = wm_code_image_write(module, put_code, NULL);
    if (r && wm_module_predecoded(module))
    {
        /* As with UBSan, which makes the interpreter too large: nothing to test. */
        printf("SKIP the module runs in place: %s\n", wm_module_predecoded(module));
        return 0;
    }
    check(!r && g_code_size > 32u && g_code_size % 4u == 0, "an image written before the first instance");
    words = g_code_size / 4u;

    check(!map_copy(bytes, size, g_code, g_code_size, 0), "the image maps into another module of the same bytes");
    check(map_copy(bytes, size, g_code, g_code_size, 1) == wm_code_image_invalid, "misaligned: refused");
    check(map_copy(bytes, size, g_code, g_code_size - 4u, 0) == wm_code_image_invalid, "cut: refused");
    check(map_copy(other, other_size, g_code, g_code_size, 0) == wm_code_image_invalid, "another module: refused");
    check(map_changed(bytes, size, words - 6u, 1, 0) == wm_code_image_invalid, "another build's tag: refused");
    check(map_changed(bytes, size, 0, 1, 1) == wm_code_image_invalid, "a record changed: refused by the CRC");
#if !defined(WM_HAS_VALIDATION) || WM_HAS_VALIDATION
    /* Every word but the CRC itself, which is made again. Without validation an image with a good CRC is trusted. */
    for (uint32_t at = 0; at + 1u < words; ++at)
        if (map_changed(bytes, size, at, 0x10000u, 0) != wm_code_image_invalid ||
            map_changed(bytes, size, at, 1, 0) != wm_code_image_invalid)
        {
            snprintf(what, sizeof(what), "word %u changed, the CRC made again: refused", at);
            check(0, what);
        }
#endif

    check(!wm_code_image_map(module, g_code, g_code_size), "the image maps before the first instance");
    check(wm_code_image_map(module, g_code, g_code_size) != NULL, "and only once");
    check(!wm_instantiate(&g_inst, module, rt, resolve, NULL), "instantiate");
    check(!wm_module_load(&plain, bytes, size) && !wm_instantiate(&in_place, plain, rt, resolve, NULL),
          "a second instance, without the image");
    if (!g_inst || !in_place)
        return 1;
    check(wm_code_cache_bytes() == 0, "no records in RAM before the first call");

    for (int refused = 0; refused < 2; ++refused)
    {
        const char* how = refused ? "from the image" : "copied in";
        g_code_refused = refused;
        wm_code_cache_limit(refused ? 0 : 1);
        wm_code_cache_trim(0);
        arg = 3;
        snprintf(what, sizeof(what), "%s: square(3) = 9", how);
        check(!wm_call(rt, export_func(g_inst, "square"), &arg, &result) && result == 9, what);
        arg = 15;
        snprintf(what, sizeof(what), "%s: fib(15) = 610", how);
        check(!wm_call(rt, export_func(g_inst, "fib"), &arg, &result) && result == 610, what);
        if (refused)
            check(wm_code_cache_bytes() == 0, "from the image: no records in RAM");
        arg = 7;
        snprintf(what, sizeof(what), "%s: a host call back in, the cache emptied around it", how);
        check(!wm_call(rt, export_func(g_inst, "outer"), &arg, &result) && result == 49 + 55 + 1, what);
        arg = 12;
        snprintf(what, sizeof(what), "%s: fib(12) in slices, through snapshots", how);
        check(!run_sliced(&rt, export_func(g_inst, "fib"), arg, g_inst, &result) && result == 144, what);
        g_code_refused = 0;
        arg = 5;
        snprintf(what, sizeof(what), "%s: outer(5) stopped, gone on in an instance without the image", how);
        wm_set_fuel(rt, 40);
        g_code_refused = refused;
        r = wm_call(rt, export_func(g_inst, "outer"), &arg, &result);
        g_code_refused = 0;
        g_length = g_at = 0;
        if (r == wm_fuel_exhausted && !(r = wm_snapshot_write(rt, g_inst, put, NULL)))
        {
            wm_runtime_free(rt);
            rt = wm_runtime_new(64u * 1024u, 100u);
            if (!(r = wm_snapshot_read(rt, in_place, get, NULL)))
                r = wm_resume(rt, &result);
        }
        check(!r && result == 25 + 55 + 1, what);
        wm_disable_fuel(rt);
    }

    g_code_refused = 0;
    wm_instance_free(in_place);
    wm_module_free(plain);
    wm_instance_free(g_inst);
    wm_module_free(module);
    check(wm_code_cache_bytes() == 0, "modules freed take their records along");
    wm_runtime_free(rt);
    wm_host_function_free(g_back);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
