/*
 * A module instantiated with every import made for it, its start function
 * and each exported function run with zeros for arguments and a budget of
 * fuel. Traps are answers; only the sanitizers' findings are bugs.
 */
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    FuzzImports imports;
    WmInstance* inst;
    WmRuntime* rt;
    WmResult r;
    fuzz_init();
    if (fuzz_instantiate(data, size, 0, &imports, &inst))
        return 0;
    rt = wm_runtime_new(test_store(), 64u * 1024u, 1000u);
    if (!rt)
    {
        fuzz_free(inst, &imports);
        return 0;
    }
    wm_set_fuel(rt, FUZZ_FUEL);
    r = wm_instance_start(inst, rt);
    /* A start function that did not end leaves the instance half made: nothing more to run. */
    for (uint32_t e = 0; !r && e < inst->module->export_count; ++e)
    {
        WmFunc* f = fuzz_export_func(inst, e);
        uint32_t args[FUZZ_RESULT_SLOTS] = {0};
        uint32_t results[FUZZ_RESULT_SLOTS];
        if (!f || f->param_slots > FUZZ_RESULT_SLOTS || f->result_slots > FUZZ_RESULT_SLOTS)
            continue;
        wm_runtime_free(rt);
        if (!(rt = wm_runtime_new(test_store(), 64u * 1024u, 1000u)))
            break;
        wm_set_fuel(rt, FUZZ_FUEL);
        (void)wm_call(rt, f, args, results);
    }
    if (rt)
        wm_runtime_free(rt);
    fuzz_free(inst, &imports);
    return 0;
}
