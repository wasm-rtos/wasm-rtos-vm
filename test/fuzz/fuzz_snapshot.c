/*
 * Any bytes through wm_snapshot_read into test/fuzz/snapshot.wasm stopped
 * deep in a recursion; a snapshot that reads goes on with a budget of fuel.
 *
 * With FUZZ_SEEDS set to a directory, it first writes there real snapshots
 * of the program stopped after different amounts of fuel, for the fuzzer
 * to start from.
 */
#include <stdio.h>

#define FUZZ_OWN_INITIALIZE
#include "fuzz.h"
#include "snapshot_wasm.h"

#define DEPTH 120u

typedef struct Buffer
{
    const uint8_t* data;
    size_t size;
    size_t at;
    FILE* out;
} Buffer;

static WmResult buffer_read(void* context, void* data, uint32_t size)
{
    Buffer* b = (Buffer*)context;
    if (size > b->size - b->at)
        return "fuzz: the snapshot ends early";
    memcpy(data, b->data + b->at, size);
    b->at += size;
    return NULL;
}

static WmResult file_write(void* context, const void* data, uint32_t size)
{
    return fwrite(data, 1, size, ((Buffer*)context)->out) == size ? NULL : "fuzz: cannot write a seed";
}

static WmModule* g_module;

int LLVMFuzzerInitialize(int* argc, char*** argv);

/* An instance of the program, stopped after fuel instructions of run(DEPTH) on rt. */
static WmInstance* stopped(WmRuntime* rt, uint32_t fuel)
{
    WmInstance* inst;
    WmExtern run;
    uint32_t arg = DEPTH, result;
    if (wm_instantiate(&inst, g_module, rt, NULL, NULL))
        return NULL;
    if (wm_instance_export(inst, "run", 3, &run))
    {
        wm_instance_free(inst);
        return NULL;
    }
    wm_set_fuel(rt, fuel);
    (void)wm_call(rt, run.as.func, &arg, &result);
    return inst;
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    const char* seeds = getenv("FUZZ_SEEDS");
    (void)argc, (void)argv;
    fuzz_init();
    if (wm_module_load(&g_module, test_fuzz_snapshot_wasm, test_fuzz_snapshot_wasm_len))
        abort();
    for (uint32_t fuel = 50; seeds && fuel <= 3200; fuel *= 2u)
    {
        char path[512];
        WmRuntime* rt = wm_runtime_new(64u * 1024u, 1000u);
        WmInstance* inst = rt ? stopped(rt, fuel) : NULL;
        Buffer b = {NULL, 0, 0, NULL};
        snprintf(path, sizeof(path), "%s/stopped-%u", seeds, fuel);
        if (inst && (b.out = fopen(path, "wb")))
        {
            if (wm_snapshot_write(rt, inst, file_write, &b))
                abort();
            fclose(b.out);
        }
        if (inst)
            wm_instance_free(inst);
        if (rt)
            wm_runtime_free(rt);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* The instance stopped on one runtime; the snapshot read into a fresh one, as hosts do. */
    WmRuntime* first = wm_runtime_new(64u * 1024u, 1000u);
    WmRuntime* rt = wm_runtime_new(64u * 1024u, 1000u);
    WmInstance* inst = first && rt ? stopped(first, 300) : NULL;
    Buffer b = {data, size, 0, NULL};
    uint32_t result;
    if (inst && !wm_snapshot_read(rt, inst, buffer_read, &b))
    {
        wm_set_fuel(rt, FUZZ_FUEL);
        (void)wm_resume(rt, &result);
    }
    if (inst)
        wm_instance_free(inst);
    if (rt)
        wm_runtime_free(rt);
    if (first)
        wm_runtime_free(first);
    return 0;
}
