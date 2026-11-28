/*
 * Any bytes through wm_code_image_map as the code image of
 * test/fuzz/snapshot.wasm (decision 15); an image that maps runs the
 * program with a budget of fuel. The CRC is not checked in fuzzing builds,
 * so what refuses a changed image is the rest: its bounds, its index and,
 * with validation, the records made again and compared.
 *
 * With FUZZ_SEEDS set to a directory, it first writes there the real image
 * for the fuzzer to start from.
 */
#include <stdio.h>

#define FUZZ_OWN_INITIALIZE
#include "fuzz.h"
#include "snapshot_wasm.h"

static WmResult file_write(void* context, const void* data, uint32_t size)
{
    return fwrite(data, 1, size, (FILE*)context) == size ? NULL : "fuzz: cannot write a seed";
}

int LLVMFuzzerInitialize(int* argc, char*** argv);

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    const char* seeds = getenv("FUZZ_SEEDS");
    (void)argc, (void)argv;
    fuzz_init();
    if (seeds)
    {
        char path[512];
        WmModule* module;
        FILE* out;
        snprintf(path, sizeof(path), "%s/image", seeds);
        if (wm_module_load(&module, NULL, test_fuzz_snapshot_wasm, test_fuzz_snapshot_wasm_len) || !(out = fopen(path, "wb")))
            abort();
        if (wm_code_image_write(module, file_write, out))
            abort();
        fclose(out);
        wm_module_free(module);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* The image where a host maps it: 4-byte aligned, its own memory. */
    uint32_t* image = (uint32_t*)malloc(size ? size : 1u);
    WmModule* module = NULL;
    WmRuntime* rt = NULL;
    WmInstance* inst = NULL;
    WmExtern run;
    uint32_t arg = 40, result;
    if (!image)
        return 0;
    memcpy(image, data, size);
    if (!wm_module_load(&module, NULL, test_fuzz_snapshot_wasm, test_fuzz_snapshot_wasm_len) &&
        !wm_code_image_map(module, image, (uint32_t)size) && (rt = wm_runtime_new(test_store(), 64u * 1024u, 1000u)) &&
        !wm_instance_new(&inst, test_store(), module, rt, NULL, NULL) && !wm_instance_export(inst, "run", 3, &run))
    {
        /* From the image, and copied in from it. */
        wm_set_fuel(rt, FUZZ_FUEL);
        (void)wm_call(rt, run.as.func, &arg, &result);
        wm_code_cache_trim(0);
        wm_set_fuel(rt, FUZZ_FUEL);
        (void)wm_call(rt, run.as.func, &arg, &result);
    }
    if (inst)
        fuzz_instance_free(inst);
    if (rt)
        wm_runtime_free(rt);
    wm_module_free(module);
    free(image);
    return 0;
}
