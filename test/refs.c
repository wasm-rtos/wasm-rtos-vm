/*
 * Snapshots and what they hold: function references on the stack go into
 * a fresh instance as references (its own functions, the old instance
 * freed), and a snapshot changed anywhere is refused, by its CRC or, with
 * the CRC made again, by the checks against the code (decision 11).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint8_t g_image[1 << 16];
static uint32_t g_length, g_at;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static WmResult put(void* context, const void* data, uint32_t size)
{
    (void)context;
    if (g_length + size > sizeof(g_image))
        return "test: snapshot too large";
    memcpy(g_image + g_length, data, size);
    g_length += size;
    return NULL;
}

static WmResult get(void* context, void* data, uint32_t size)
{
    (void)context;
    if (size > g_length - g_at)
        return "test: snapshot ends early";
    memcpy(data, g_image + g_at, size);
    g_at += size;
    return NULL;
}

static uint32_t crc32(const uint8_t* p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* The word at byte at of the image changed by delta, the CRC made again to match. */
static void change(uint32_t at, uint32_t delta)
{
    uint32_t word, crc;
    memcpy(&word, g_image + at, 4);
    word += delta;
    memcpy(g_image + at, &word, 4);
    crc = crc32(g_image, g_length - 4u);
    memcpy(g_image + g_length - 4u, &crc, 4);
}

/* The image read into a fresh instance and runtime, run to the end: the result, or ~0 when refused. */
static uint32_t load(WmModule* module)
{
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmInstance* inst = NULL;
    uint32_t result = ~0u;
    g_at = 0;
    if (!wm_instantiate(&inst, module, rt, NULL, NULL) && !wm_snapshot_read(rt, inst, get, NULL))
    {
        wm_set_fuel(rt, 1000000u);
        if (wm_resume(rt, &result))
            result = ~1u;
    }
    if (inst)
        wm_instance_free(inst);
    wm_runtime_free(rt);
    return result;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    static uint8_t image[sizeof(g_image)];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/refs.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    WmModule* module = NULL;
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 100u);
    WmInstance* inst = NULL;
    WmExtern run;
    uint32_t result = 0;
    if (f)
        fclose(f);
    if (!size || wm_module_load(&module, bytes, size) || wm_instantiate(&inst, module, rt, NULL, NULL) ||
        wm_instance_export(inst, "run", 3, &run))
    {
        printf("FAIL cannot load test/refs.wasm\n");
        return 1;
    }
    wm_set_fuel(rt, 500);
    check(wm_call(rt, run.as.func, NULL, &result) == wm_fuel_exhausted, "stopped in the loop, two references alive");
    check(!wm_snapshot_write(rt, inst, put, NULL), "the snapshot is written");
    /* The instance the references were made in goes: the snapshot must not point into it. */
    wm_instance_free(inst);
    wm_runtime_free(rt);
    memcpy(image, g_image, g_length);
    check(load(module) == 12, "a fresh instance goes on with its own functions: 7 + 5");

    g_image[g_length / 2u] ^= 1u;
    check(load(module) == ~0u, "a byte changed: refused by the CRC");
    memcpy(g_image, image, g_length);

    /* The header is ten words, then the suspended call's function, offset, side index and frame. */
    change(44, 1);
    check(load(module) == ~0u, "a position inside an instruction: refused");
    memcpy(g_image, image, g_length);
    change(48, 1);
    check(load(module) == ~0u, "another side table entry: refused");
    memcpy(g_image, image, g_length);
    change(52, 1);
    check(load(module) == ~0u, "a frame one slot higher: refused");
    memcpy(g_image, image, g_length);
    check(load(module) == 12, "the snapshot as written still loads");

    wm_module_free(module);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
