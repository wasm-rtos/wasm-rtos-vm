/* Any bytes through the loader: decoding and validation. */
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    WmModule* m = NULL;
    fuzz_init();
    if (size <= 0xFFFFFFFFu && !wm_module_load(&m, data, (uint32_t)size))
        wm_module_free(m);
    return 0;
}
