/*
 * A plain optimized build must predecode: otherwise every program runs in
 * place, two to three times slower, and nothing else would say so.
 */
#include <stdio.h>
#include "../source/wm.h"

/* The store this program's objects are made in (decision 25): one for the whole run. */
static WmStore* test_store(void)
{
    static WmStore* store;
    if (!store)
        store = wm_store_new(NULL);
    return store;
}

int main(int argc, char** argv)
{
    static uint8_t bytes[4096];
    FILE* f = fopen(argc > 1 ? argv[1] : "test/paged.wasm", "rb");
    uint32_t size = f ? (uint32_t)fread(bytes, 1, sizeof(bytes), f) : 0;
    WmModule* module = NULL;
    WmInstance* inst = NULL;
    WmRuntime* rt = wm_runtime_new(test_store(), 64u * 1024u, 100u);
    WmResult why;
    if (f)
        fclose(f);
    if (!size || wm_module_load(&module, NULL, bytes, size) || wm_instance_new(&inst, test_store(), module, rt, NULL, NULL))
    {
        printf("FAIL load the module\n");
        return 1;
    }
    why = wm_module_predecoded(module);
    printf("%s predecoded%s%s\n", why ? "FAIL" : "PASS", why ? ": " : "", why ? why : "");
    wm_instance_free(inst);
    wm_module_free(module);
    wm_runtime_free(rt);
    return why ? 1 : 0;
}
