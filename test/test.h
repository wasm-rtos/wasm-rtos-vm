/*
 * What every test here shares: a check that prints PASS or FAIL and counts
 * failures, an export by name, and the last line with the exit code.
 * Included once, by the test's own .c file.
 */
#ifndef WM_TEST_H
#define WM_TEST_H

#include <stdio.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;

static inline void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/*
 * The store a test's objects are made in when it needs no store of its own
 * (decision 25): made on first use, freed when the program exits.
 */
static inline WmStore* test_store(void)
{
    static WmStore* store;
    if (!store)
        store = wm_store_new(NULL);
    return store;
}

/* An instance and the memory it made, for a test that counts what is freed: an instance's memory is its store's. */
static inline void test_instance_free(WmInstance* instance)
{
    WmMemory* memory = instance ? wm_instance_memory(instance) : NULL;
    wm_instance_free(instance);
    wm_memory_free(memory);
}

/* The function an instance exports by that name, or NULL. */
static inline WmFunc* export_func(WmInstance* inst, const char* name)
{
    WmExtern f;
    return wm_instance_export(inst, name, (uint32_t)strlen(name), &f) ? NULL : f.as.func;
}

/* The last line, and main's return value. */
static inline int summary(void)
{
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}

#endif
