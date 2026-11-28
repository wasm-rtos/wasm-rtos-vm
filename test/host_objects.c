/*
 * Globals and tables a host makes for a module to import: what they hold,
 * how a table grows up to its maximum, and that freeing them gives every
 * byte back (run under AddressSanitizer).
 */
#include <stdio.h>
#include "test.h"


int main(void)
{
    WmGlobal* global = wm_global_new(test_store(), WM_I32, 1, 42u);
    WmTable* table = wm_table_new(test_store(), WM_FUNCREF, 2u, 4u);

    check(global != NULL && wm_global_type(global) == WM_I32 && wm_global_get(global) == 42u,
          "a mutable i32 global holds what it was made with");
    check(table != NULL, "a funcref table of 2 entries, at most 4");
    check(wm_table_grow(table, 1u) == 2u, "growing by 1 gives the old size, 2");
    check(wm_table_grow(table, 2u) == 0xFFFFFFFFu, "growing past the maximum fails");
    check(wm_table_grow(table, 1u) == 3u, "growing to the maximum works");
    check(wm_table_grow(table, 0u) == 4u, "growing by nothing gives the size");
    wm_table_free(table);
    wm_global_free(global);
    wm_table_free(NULL);
    wm_global_free(NULL);
    check(1, "freed, NULL too");
    return summary();
}
