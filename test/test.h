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
