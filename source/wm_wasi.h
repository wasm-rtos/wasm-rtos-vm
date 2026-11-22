#ifndef WM_WASI_H
#define WM_WASI_H

/*
 * WASI preview 1, the calls that need no file system (decision 19): clocks,
 * sleeping and yielding, the standard streams, arguments and environment,
 * random bytes, exit; and every other call linking with ENOSYS, so a
 * program that only might use one still loads. What the standard says
 * about memory, layouts and error numbers is here; what the calls mean on
 * a device is the host's (WmWasiHost). A host with files answers the file
 * calls itself before asking this module.
 *
 * Both namespaces link: wasi_snapshot_preview1 and the older wasi_unstable.
 */

#include "wm.h"

typedef struct WmWasiHost
{
    void* context;
    /* Nanoseconds now on a WASI clock (0 realtime, 1 monotonic, 2 and 3 CPU time). */
    uint64_t (*now_ns)(void* context, uint32_t clock);
    /* The clocks' resolution in nanoseconds; 0: 1 ms. */
    uint64_t resolution_ns;
    /*
     * The calling program sleeps ms (more than 0): the host may stop the
     * call there and go on with it later (wm_yield), or wait. The call's
     * result is set before.
     */
    void (*sleep_ms)(void* context, WmRuntime* runtime, uint32_t ms);
    /* The calling program gives the processor up; may be NULL. */
    void (*yield)(void* context, WmRuntime* runtime);
    /*
     * Bytes the program writes to fd 1 or 2 (in pieces); with data NULL,
     * the WASI errno for writing to another fd. NULL: output is dropped,
     * other fds are EBADF.
     */
    uint32_t (*write)(void* context, WmRuntime* runtime, uint32_t fd, const void* data, uint32_t length);
    /* The program's arguments, argv[0] its name; NULL: none. */
    uint32_t (*args)(void* context, WmRuntime* runtime, const char* const** out_argv);
    /* Random bytes; NULL: a generator of the module's own (not for secrets). */
    void (*random)(void* context, void* out, uint32_t length);
    /* proc_exit: the trap that ends the program with the code. */
    WmResult (*exit)(void* context, WmRuntime* runtime, uint32_t code);
} WmWasiHost;

/*
 * The call an import names, in either WASI namespace, made with host as its
 * user (kept until wm_wasi_free): *out NULL when this module does not
 * answer it (another module, or a file call).
 */
WmResult wm_wasi_resolve(const WmWasiHost* host, const char* module, uint32_t module_length, const char* name,
                         uint32_t name_length, WmFunc** out);
/*
 * A WASI call nothing answered: a function of the type program declares for
 * it that returns ENOSYS (52), other results 0; name is the pointer the
 * resolver was given (wm_module_import_type). *out NULL for any other
 * module, or a type it cannot make.
 */
WmResult wm_wasi_resolve_absent(const WmModule* program, const char* module, uint32_t module_length,
                                const char* name, WmFunc** out);
/* The functions both made, given back. */
void wm_wasi_free(void);

#endif
