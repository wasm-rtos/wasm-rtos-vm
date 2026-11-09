/*
 * The dylink.0 convention: a position-independent program and the libraries
 * it needs share one linear memory and one function table in one runtime,
 * like an ELF process. Optional: build wm_dylink.c to have it.
 */
#ifndef WM_DYLINK_H
#define WM_DYLINK_H

#include "wm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WmDylink WmDylink;

/* Finds a library the program needs (a dylink.0 "needed" name), loaded; the process then owns it. */
typedef WmResult (*WmDylinkFind)(void* context, const char* name, WmModule** out);

typedef struct WmDylinkOptions
{
    void* context;
    WmDylinkFind find;
    /*
     * Asked first for every function import of every module: a host
     * function, or out->as.func left NULL for the program's modules to
     * define it. Other kinds of imports are the convention's.
     */
    WmResolver host;
    /* The stack in linear memory, after every module's data; 0 for 64 KiB. */
    uint32_t stack_size;
} WmDylinkOptions;

/* Whether the module has a dylink.0 section, so it loads with wm_dylink_load. */
int wm_module_is_dylink(const WmModule* module);

/*
 * Loads the program and the libraries it needs, instantiates them on
 * runtime and runs their start functions, relocations and constructors.
 * The program stays the caller's; the libraries belong to the process,
 * which is kept on failure too, so that wm_dylink_free releases it.
 */
WmResult wm_dylink_load(WmDylink** out, const WmModule* program, const char* name, WmRuntime* runtime,
                        const WmDylinkOptions* options);
/* The program's instance. */
WmInstance* wm_dylink_instance(const WmDylink* process);
void wm_dylink_free(WmDylink* process);

#ifdef __cplusplus
}
#endif

#endif
