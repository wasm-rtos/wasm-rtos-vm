/*
 * WASI preview 1 without files (decision 19, wm_wasi.h). Calls read and
 * write the program's memory through wm_memory_read and wm_memory_write,
 * one block or paged; a range outside it is EFAULT or a trap, as each call
 * has always answered.
 */
#include "wm_wasi.h"

#include <stdlib.h>
#include <string.h>

enum
{
    E_SUCCESS = 0,
    E_BADF = 8,
    E_FAULT = 21,
    E_INVAL = 28,
    E_NOSYS = 52,
    E_SPIPE = 70,
    FILETYPE_CHARACTER_DEVICE = 2,
    EVENTTYPE_CLOCK = 0,
    SUBCLOCKFLAGS_ABSTIME = 1,
    CLOCK_LAST = 3
};

static const char g_out_of_bounds[] = "trap: out of bounds memory access";
static const char g_exit[] = "trap: exit";

typedef struct Call
{
    const WmWasiHost* host;
    WmRuntime* runtime;
    WmMemory* memory;
} Call;

static const WmWasiHost* host_of(void* user) { return (const WmWasiHost*)user; }

static uint32_t arg32(const uint32_t* slots, uint32_t at) { return slots[at]; }

static int in_bounds(const WmMemory* memory, uint32_t address, uint64_t length)
{
    uint32_t size = 0;
    if (!memory)
        return 0;
    (void)wm_memory_data(memory, &size);
    return (uint64_t)address + length <= size;
}

/* EFAULT outside the memory; an access that fails inside (a frame that could not come back) traps after the call. */
static uint32_t put(const Call* c, uint32_t address, const void* data, uint32_t length)
{
    WmResult r;
    if (!in_bounds(c->memory, address, length))
        return E_FAULT;
    if ((r = wm_memory_write(c->memory, address, data, length)))
    {
        (void)wm_trap(c->runtime, r);
        return E_FAULT;
    }
    return E_SUCCESS;
}

static uint32_t get(const Call* c, uint32_t address, void* out, uint32_t length)
{
    WmResult r;
    if (!in_bounds(c->memory, address, length))
        return E_FAULT;
    if ((r = wm_memory_read(c->memory, address, out, length)))
    {
        (void)wm_trap(c->runtime, r);
        return E_FAULT;
    }
    return E_SUCCESS;
}

static uint32_t read32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t read64(const uint8_t* p) { return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32; }

#define CALL() Call c = {host_of(user), runtime, memory}
#define RETURN(errno_value) \
    do \
    { \
        slots[0] = (errno_value); \
        return NULL; \
    } while (0)
/* A range outside the memory traps (these calls have always trapped). */
#define CHECK(address, length) \
    do \
    { \
        if (!in_bounds(memory, (address), (length))) \
            return g_out_of_bounds; \
    } while (0)

static uint64_t now_ns(const WmWasiHost* host, uint32_t clock)
{
    return host->now_ns ? host->now_ns(host->context, clock) : 0;
}

static WmResult clock_res_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint64_t resolution = c.host->resolution_ns ? c.host->resolution_ns : 1000000u;
    if (arg32(slots, 0) > CLOCK_LAST)
        RETURN(E_INVAL);
    RETURN(put(&c, arg32(slots, 1), &resolution, sizeof(resolution)));
}

static WmResult clock_time_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint32_t clock = arg32(slots, 0);
    uint64_t time_ns;
    if (clock > CLOCK_LAST)
        RETURN(E_INVAL);
    time_ns = now_ns(c.host, clock);
    RETURN(put(&c, arg32(slots, 3), &time_ns, sizeof(time_ns)));
}

static WmResult sched_yield(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)memory;
    slots[0] = E_SUCCESS;
    if (host_of(user)->yield)
        host_of(user)->yield(host_of(user)->context, runtime);
    return NULL;
}

/* fd_write: the standard streams to the host; every buffer is checked first, so a bad one writes nothing. */
static WmResult fd_write(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint32_t fd = arg32(slots, 0), iovs = arg32(slots, 1), count = arg32(slots, 2), written = arg32(slots, 3);
    uint32_t total = 0;
    if (count > UINT32_MAX / 8u || !in_bounds(memory, iovs, (uint64_t)count * 8u) || !in_bounds(memory, written, 4))
        RETURN(E_FAULT);
    if (fd != 1u && fd != 2u)
        RETURN(c.host->write ? c.host->write(c.host->context, runtime, fd, NULL, 0) : E_BADF);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t iov[8];
        uint32_t length;
        if (get(&c, iovs + i * 8u, iov, sizeof(iov)) != E_SUCCESS)
            RETURN(E_FAULT);
        length = read32(iov + 4);
        if (!in_bounds(memory, read32(iov), length) || length > UINT32_MAX - total)
            RETURN(E_FAULT);
        total += length;
    }
    for (uint32_t i = 0; i < count && c.host->write; ++i)
    {
        uint8_t iov[8];
        uint32_t address, length;
        uint8_t* data = NULL;
        if (get(&c, iovs + i * 8u, iov, sizeof(iov)) != E_SUCCESS)
            RETURN(E_FAULT);
        address = read32(iov);
        length = read32(iov + 4);
        if (!wm_memory_pointer(memory, address, length, &data) && data)
            (void)c.host->write(c.host->context, runtime, fd, data, length);
        else
            while (length)
            {
                uint8_t piece[64];
                uint32_t n = length < sizeof(piece) ? length : (uint32_t)sizeof(piece);
                if (get(&c, address, piece, n) != E_SUCCESS)
                    break;
                (void)c.host->write(c.host->context, runtime, fd, piece, n);
                address += n;
                length -= n;
            }
    }
    (void)put(&c, written, &total, sizeof(total));
    RETURN(E_SUCCESS);
}

/* poll_oneoff: file subscriptions are ready at once; otherwise the program sleeps until the earliest clock. */
static WmResult poll_oneoff(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint32_t in = arg32(slots, 0), out = arg32(slots, 1), count = arg32(slots, 2), events = arg32(slots, 3);
    uint8_t subscription[48], event[32];
    uint64_t now = now_ns(c.host, 1), soonest_ns = UINT64_MAX;
    uint32_t soonest = 0, written = 0;
    if (count == 0)
        RETURN(E_INVAL);
    if (count > 0xFFFFFFFFu / 48u || !in_bounds(memory, in, (uint64_t)count * 48u) ||
        !in_bounds(memory, out, (uint64_t)count * 32u) || !in_bounds(memory, events, 4))
        RETURN(E_FAULT);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t tag;
        if (get(&c, in + i * 48u, subscription, sizeof(subscription)) != E_SUCCESS)
            RETURN(E_FAULT);
        tag = subscription[8];
        if (tag == EVENTTYPE_CLOCK)
        {
            uint64_t timeout = read64(subscription + 24);
            uint16_t flags = (uint16_t)(subscription[40] | subscription[41] << 8);
            uint64_t due = (flags & SUBCLOCKFLAGS_ABSTIME) ? timeout : now + timeout;
            if (due < soonest_ns)
            {
                soonest_ns = due;
                soonest = i;
            }
        }
        else
        {
            memset(event, 0, sizeof(event));
            memcpy(event, subscription, 8);
            event[10] = tag;
            (void)put(&c, out + written * 32u, event, sizeof(event));
            ++written;
        }
    }
    if (written == 0)
    {
        uint64_t wait_ns = soonest_ns > now ? soonest_ns - now : 0;
        uint64_t wait_ms = (wait_ns + 999999u) / 1000000u;
        if (get(&c, in + soonest * 48u, subscription, sizeof(subscription)) != E_SUCCESS)
            RETURN(E_FAULT);
        memset(event, 0, sizeof(event));
        memcpy(event, subscription, 8);
        event[10] = EVENTTYPE_CLOCK;
        (void)put(&c, out, event, sizeof(event));
        written = 1;
        (void)put(&c, events, &written, sizeof(written));
        slots[0] = E_SUCCESS;
        if (wait_ms > 0 && c.host->sleep_ms)
            c.host->sleep_ms(c.host->context, runtime, wait_ms > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)wait_ms);
        return NULL;
    }
    (void)put(&c, events, &written, sizeof(written));
    RETURN(E_SUCCESS);
}

static uint32_t args_of(const WmWasiHost* host, WmRuntime* runtime, const char* const** argv)
{
    *argv = NULL;
    return host->args ? host->args(host->context, runtime, argv) : 0;
}

static WmResult args_sizes_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    const char* const* argv;
    uint32_t argc = args_of(c.host, runtime, &argv), bytes = 0;
    CHECK(arg32(slots, 0), 4);
    CHECK(arg32(slots, 1), 4);
    for (uint32_t i = 0; i < argc; ++i)
        bytes += (uint32_t)strlen(argv[i]) + 1u;
    (void)put(&c, arg32(slots, 0), &argc, 4);
    (void)put(&c, arg32(slots, 1), &bytes, 4);
    RETURN(E_SUCCESS);
}

static WmResult args_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    const char* const* argv;
    uint32_t argc = args_of(c.host, runtime, &argv), pointers = arg32(slots, 0), buffer = arg32(slots, 1);
    for (uint32_t i = 0; i < argc; ++i)
    {
        uint32_t length = (uint32_t)strlen(argv[i]) + 1u;
        CHECK(pointers + i * 4u, 4);
        CHECK(buffer, length);
        (void)put(&c, pointers + i * 4u, &buffer, 4);
        (void)put(&c, buffer, argv[i], length);
        buffer += length;
    }
    RETURN(E_SUCCESS);
}

/* No environment variables. */
static WmResult environ_sizes_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint32_t zero = 0;
    CHECK(arg32(slots, 0), 4);
    CHECK(arg32(slots, 1), 4);
    (void)put(&c, arg32(slots, 0), &zero, 4);
    (void)put(&c, arg32(slots, 1), &zero, 4);
    RETURN(E_SUCCESS);
}

static WmResult environ_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    RETURN(E_SUCCESS);
}

static WmResult proc_exit(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)memory;
    return host_of(user)->exit ? host_of(user)->exit(host_of(user)->context, runtime, arg32(slots, 0)) : g_exit;
}

static WmResult random_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    static uint32_t state = 0x2545F491u;
    CALL();
    uint32_t buffer = arg32(slots, 0), length = arg32(slots, 1);
    uint8_t chunk[32];
    CHECK(buffer, length);
    while (length)
    {
        uint32_t n = length < sizeof(chunk) ? length : (uint32_t)sizeof(chunk);
        if (c.host->random)
            c.host->random(c.host->context, chunk, n);
        else
            for (uint32_t i = 0; i < n; ++i)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                chunk[i] = (uint8_t)state;
            }
        (void)put(&c, buffer, chunk, n);
        buffer += n;
        length -= n;
    }
    RETURN(E_SUCCESS);
}

/* The standard streams are character devices; nothing else is open here. */
static WmResult fd_fdstat_get(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint8_t stat[24];
    if (arg32(slots, 0) > 2u)
        RETURN(E_BADF);
    CHECK(arg32(slots, 1), sizeof(stat));
    memset(stat, 0, sizeof(stat));
    stat[0] = FILETYPE_CHARACTER_DEVICE;
    memset(stat + 8, 0xFF, 16);
    (void)put(&c, arg32(slots, 1), stat, sizeof(stat));
    RETURN(E_SUCCESS);
}

static WmResult fd_close(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    RETURN(arg32(slots, 0) <= 2u ? E_SUCCESS : E_BADF);
}

static WmResult fd_seek(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    RETURN(E_SPIPE);
}

static WmResult badf(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    RETURN(E_BADF);
}

/* Standard input has nothing to read. */
static WmResult fd_read(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    CALL();
    uint32_t zero = 0;
    if (arg32(slots, 0) != 0)
        RETURN(E_BADF);
    CHECK(arg32(slots, 3), 4);
    (void)put(&c, arg32(slots, 3), &zero, 4);
    RETURN(E_SUCCESS);
}

typedef struct Entry
{
    const char* name;
    const char* signature;
    WmHostFunction function;
} Entry;

static const Entry g_calls[] = {
    {"clock_res_get", "i(ii)", clock_res_get},     {"clock_time_get", "i(iIi)", clock_time_get},
    {"sched_yield", "i()", sched_yield},           {"poll_oneoff", "i(iiii)", poll_oneoff},
    {"fd_write", "i(iiii)", fd_write},             {"args_sizes_get", "i(ii)", args_sizes_get},
    {"args_get", "i(ii)", args_get},               {"environ_sizes_get", "i(ii)", environ_sizes_get},
    {"environ_get", "i(ii)", environ_get},         {"proc_exit", "v(i)", proc_exit},
    {"random_get", "i(ii)", random_get},           {"fd_fdstat_get", "i(ii)", fd_fdstat_get},
    {"fd_close", "i(i)", fd_close},                {"fd_seek", "i(iIii)", fd_seek},
    {"fd_read", "i(iiii)", fd_read},               {"fd_prestat_get", "i(ii)", badf},
    {"fd_prestat_dir_name", "i(iii)", badf},
};

/* Functions made, kept until wm_wasi_free: a call of a host, or an ENOSYS of a type. */
typedef struct Made
{
    struct Made* next;
    const void* key;
    char signature[24];
    WmFunc* func;
} Made;
static Made* g_made;

static int is_wasi(const char* module, uint32_t length)
{
    return (length == 22 && !memcmp(module, "wasi_snapshot_preview1", 22)) ||
           (length == 13 && !memcmp(module, "wasi_unstable", 13));
}

static uint8_t type_of(char letter)
{
    switch (letter)
    {
        case 'i': return WM_I32;
        case 'I': return WM_I64;
        case 'f': return WM_F32;
        case 'F': return WM_F64;
        default: return 0;
    }
}

/* The function for a signature ("i(iI)", v: no result) and a key (host and call, or a type), made once. */
static WmResult made(const void* key, const char* signature, WmHostFunction function, void* user, WmFunc** out)
{
    uint8_t params[16], result = 0;
    uint32_t param_count = 0, result_count = 0;
    const char* at = signature;
    Made* m;
    for (m = g_made; m; m = m->next)
        if (m->key == key && !strcmp(m->signature, signature))
        {
            *out = m->func;
            return NULL;
        }
    if (*at != 'v')
    {
        result = type_of(*at);
        result_count = 1;
    }
    for (at += 2; *at && *at != ')' && param_count < 16; ++at)
        params[param_count++] = type_of(*at);
    if (!(m = (Made*)calloc(1, sizeof(Made))))
        return "exhaustion: out of memory";
    if (!(m->func = wm_host_function_new(params, param_count, &result, result_count, function, user)))
    {
        free(m);
        return "exhaustion: out of memory";
    }
    m->key = key;
    strncpy(m->signature, signature, sizeof(m->signature) - 1);
    m->next = g_made;
    g_made = m;
    *out = m->func;
    return NULL;
}

WmResult wm_wasi_resolve(const WmWasiHost* host, const char* module, uint32_t module_length, const char* name,
                         uint32_t name_length, WmFunc** out)
{
    *out = NULL;
    if (!is_wasi(module, module_length))
        return NULL;
    for (uint32_t i = 0; i < sizeof(g_calls) / sizeof(g_calls[0]); ++i)
        if (strlen(g_calls[i].name) == name_length && !memcmp(name, g_calls[i].name, name_length))
            return made((const char*)host + i, g_calls[i].signature, g_calls[i].function, (void*)(uintptr_t)host, out);
    return NULL;
}

static WmResult absent(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)memory;
    slots[0] = user ? E_NOSYS : 0;
    return NULL;
}

WmResult wm_wasi_resolve_absent(const WmModule* program, const char* module, uint32_t module_length,
                                const char* name, WmFunc** out)
{
    static const char letters[] = {'i', 'I', 'f', 'F'};
    const uint8_t *params = NULL, *results = NULL;
    uint32_t param_count = 0, result_count = 0, length = 0;
    char signature[24];
    static char key_nosys, key_quiet;
    *out = NULL;
    if (!program || !is_wasi(module, module_length) ||
        !wm_module_import_type(program, name, &params, &param_count, &results, &result_count) ||
        param_count + 4u > sizeof(signature) || result_count > 1)
        return NULL;
    for (uint32_t i = 0; i < param_count + result_count; ++i)
    {
        uint8_t type = i < result_count ? results[i] : params[i - result_count];
        if (type != WM_I32 && type != WM_I64 && type != WM_F32 && type != WM_F64)
            return NULL;
    }
    signature[length++] = result_count ? letters[WM_I32 - results[0]] : 'v';
    signature[length++] = '(';
    for (uint32_t i = 0; i < param_count; ++i)
        signature[length++] = letters[WM_I32 - params[i]];
    signature[length++] = ')';
    signature[length] = '\0';
    /* An i32 result says ENOSYS; any other result stays 0. */
    if (result_count == 1 && results[0] == WM_I32)
        return made(&key_nosys, signature, absent, (void*)&key_nosys, out);
    return made(&key_quiet, signature, absent, NULL, out);
}

void wm_wasi_free(void)
{
    while (g_made)
    {
        Made* m = g_made;
        g_made = m->next;
        wm_host_function_free(m->func);
        free(m);
    }
}
