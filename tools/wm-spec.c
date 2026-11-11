/*
 * A line protocol over stdin and stdout for the spec test runner.
 *
 *   module <file> <name|-> <keep 0|1>   load and instantiate; kept as the current module when keep is 1
 *   check <file>                        load (decode and validate) only
 *   register <as> <name|->              make an instance importable under a module name
 *   invoke <name|-> <hex field> [type:value ...]
 *   get <name|-> <hex field>
 *   reset                               start a new script
 *
 * Answers are "ok [type:value ...]" or "error <message>". Values are bit
 * patterns in decimal; references are "null", or a number for externref.
 */
#include "../source/wm.h"
#include "../source/wm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INSTANCES 4096

typedef struct Named
{
    char name[128];
    WmInstance* instance;
} Named;

static WmRuntime* g_runtime;
static Named g_named[MAX_INSTANCES];
static int g_named_count;
static Named g_registered[MAX_INSTANCES];
static int g_registered_count;
static WmInstance* g_current;
/* Every module and instance stays alive: tables may hold functions of failed instantiations. */
static WmModule* g_modules[MAX_INSTANCES];
static int g_module_count;
static uint8_t* g_bytes[MAX_INSTANCES];

static WmFunc* g_print[8];
static WmGlobal* g_globals[4];
static WmTable* g_table;
static WmMemory* g_memory;

static WmResult print_host(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime;
    (void)user;
    (void)slots;
    (void)memory;
    return NULL;
}

static void make_spectest(void)
{
    static const uint8_t i32[] = {WM_I32}, i64[] = {WM_I64}, f32[] = {WM_F32}, f64[] = {WM_F64};
    static const uint8_t i32_f32[] = {WM_I32, WM_F32}, f64_f64[] = {WM_F64, WM_F64};
    float f = 666.6f;
    double d = 666.6;
    uint32_t fb;
    uint64_t db;
    g_print[0] = wm_host_function_new(NULL, 0, NULL, 0, print_host, NULL);
    g_print[1] = wm_host_function_new(i32, 1, NULL, 0, print_host, NULL);
    g_print[2] = wm_host_function_new(i64, 1, NULL, 0, print_host, NULL);
    g_print[3] = wm_host_function_new(f32, 1, NULL, 0, print_host, NULL);
    g_print[4] = wm_host_function_new(f64, 1, NULL, 0, print_host, NULL);
    g_print[5] = wm_host_function_new(i32_f32, 2, NULL, 0, print_host, NULL);
    g_print[6] = wm_host_function_new(f64_f64, 2, NULL, 0, print_host, NULL);
    memcpy(&fb, &f, 4);
    memcpy(&db, &d, 8);
    g_globals[0] = wm_global_new(WM_I32, 0, 666);
    g_globals[1] = wm_global_new(WM_I64, 0, 666);
    g_globals[2] = wm_global_new(WM_F32, 0, fb);
    g_globals[3] = wm_global_new(WM_F64, 0, db);
    g_table = wm_table_new(WM_FUNCREF, 10, 20);
    g_memory = wm_memory_new(1, 2);
}

static int text_is(const char* s, uint32_t length, const char* text)
{
    return strlen(text) == length && memcmp(s, text, length) == 0;
}

static WmInstance* find(Named* list, int count, const char* name, uint32_t length)
{
    for (int i = count - 1; i >= 0; --i)
        if (text_is(name, length, list[i].name))
            return list[i].instance;
    return NULL;
}

static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    (void)context;
    (void)kind;
    if (text_is(module, module_length, "spectest"))
    {
        static const char* const prints[] = {"print", "print_i32", "print_i64", "print_f32", "print_f64",
                                             "print_i32_f32", "print_f64_f64"};
        static const char* const globals[] = {"global_i32", "global_i64", "global_f32", "global_f64"};
        for (int i = 0; i < 7; ++i)
            if (text_is(name, name_length, prints[i]))
            {
                out->kind = WM_EXTERN_FUNC;
                out->as.func = g_print[i];
                return NULL;
            }
        for (int i = 0; i < 4; ++i)
            if (text_is(name, name_length, globals[i]))
            {
                out->kind = WM_EXTERN_GLOBAL;
                out->as.global = g_globals[i];
                return NULL;
            }
        if (text_is(name, name_length, "table"))
        {
            out->kind = WM_EXTERN_TABLE;
            out->as.table = g_table;
            return NULL;
        }
        if (text_is(name, name_length, "memory"))
        {
            out->kind = WM_EXTERN_MEMORY;
            out->as.memory = g_memory;
            return NULL;
        }
        return "unlinkable: unknown import";
    }
    {
        WmInstance* instance = find(g_registered, g_registered_count, module, module_length);
        if (!instance)
            return "unlinkable: unknown import";
        return wm_instance_export(instance, name, name_length, out) ? "unlinkable: unknown import" : NULL;
    }
}

static uint8_t* read_file(const char* path, uint32_t* size)
{
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    data = (uint8_t*)malloc((size_t)n + 1u);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return data;
}

static WmInstance* instance_named(const char* name)
{
    if (strcmp(name, "-") == 0)
        return g_current;
    return find(g_named, g_named_count, name, (uint32_t)strlen(name));
}

static uint32_t unhex(const char* hex, char* out)
{
    uint32_t n = 0;
    if (strcmp(hex, "-") == 0)
        return 0;
    while (hex[0] && hex[1])
    {
        unsigned value;
        sscanf(hex, "%2x", &value);
        out[n++] = (char)value;
        hex += 2;
    }
    return n;
}

static void print_value(uint8_t type, const uint32_t* slots)
{
    switch (type)
    {
        case WM_I32: printf(" i32:%u", slots[0]); break;
        case WM_F32: printf(" f32:%u", slots[0]); break;
        case WM_I64:
        case WM_F64:
        {
            uint64_t v;
            memcpy(&v, slots, 8);
            printf(" %s:%llu", type == WM_I64 ? "i64" : "f64", (unsigned long long)v);
            break;
        }
        default:
        {
            uintptr_t r = 0;
            memcpy(&r, slots, sizeof(r));
            if (r == 0)
                printf(" %s:null", type == WM_FUNCREF ? "funcref" : "externref");
            else if (type == WM_EXTERNREF)
                printf(" externref:%llu", (unsigned long long)(r - 1u));
            else
                printf(" funcref:func");
        }
    }
}

static void command(char* line)
{
    char* words[64];
    int count = 0;
    for (char* w = strtok(line, " \n"); w && count < 64; w = strtok(NULL, " \n"))
        words[count++] = w;
    if (count == 0)
        return;
    if (strcmp(words[0], "module") == 0 || strcmp(words[0], "check") == 0)
    {
        uint32_t size;
        uint8_t* bytes = read_file(words[1], &size);
        WmModule* module;
        WmInstance* instance;
        WmResult result;
        if (!bytes)
        {
            printf("error cannot read %s\n", words[1]);
            return;
        }
        result = wm_module_load(&module, bytes, size);
        if (result || words[0][0] == 'c')
        {
            printf(result ? "error %s\n" : "ok\n", result);
            if (!result)
                wm_module_free(module);
            free(bytes);
            return;
        }
        g_modules[g_module_count] = module;
        g_bytes[g_module_count++] = bytes;
        result = wm_instantiate(&instance, module, g_runtime, resolve, NULL);
        if (result)
        {
            printf("error %s\n", result);
            return;
        }
        if (strcmp(words[3], "1") == 0)
        {
            g_current = instance;
            if (strcmp(words[2], "-") != 0)
            {
                snprintf(g_named[g_named_count].name, sizeof(g_named[0].name), "%s", words[2]);
                g_named[g_named_count++].instance = instance;
            }
        }
        printf("ok\n");
    }
    else if (strcmp(words[0], "reset") == 0)
    {
        /* A new script: fresh spectest objects and no names. The old ones stay alive. */
        make_spectest();
        g_named_count = 0;
        g_registered_count = 0;
        g_current = NULL;
        printf("ok\n");
    }
    else if (strcmp(words[0], "register") == 0)
    {
        WmInstance* instance = instance_named(words[2]);
        char name[128];
        uint32_t length = unhex(words[1], name);
        name[length] = 0;
        snprintf(g_registered[g_registered_count].name, sizeof(g_registered[0].name), "%s", name);
        g_registered[g_registered_count++].instance = instance;
        printf("ok\n");
    }
    else if (strcmp(words[0], "invoke") == 0 || strcmp(words[0], "get") == 0)
    {
        WmInstance* instance = instance_named(words[1]);
        char field[1024];
        uint32_t length = unhex(words[2], field);
        WmExtern ext;
        if (!instance || wm_instance_export(instance, field, length, &ext))
        {
            printf("error unknown export\n");
            return;
        }
        if (words[0][0] == 'g')
        {
            uint32_t slots[4] = {0};
            memcpy(slots, &ext.as.global->value, 8);
            printf("ok");
            print_value(ext.as.global->type, slots);
            printf("\n");
            return;
        }
        {
            uint32_t args[256] = {0};
            uint32_t results[256] = {0};
            uint32_t at = 0;
            WmFunc* f = ext.as.func;
            WmResult result;
            for (int i = 3; i < count; ++i)
            {
                char* colon = strchr(words[i], ':');
                const char* value = colon + 1;
                *colon = 0;
                if (strcmp(words[i], "i32") == 0 || strcmp(words[i], "f32") == 0)
                    args[at++] = (uint32_t)strtoull(value, NULL, 10);
                else if (strcmp(words[i], "i64") == 0 || strcmp(words[i], "f64") == 0)
                {
                    uint64_t v = strtoull(value, NULL, 10);
                    memcpy(args + at, &v, 8);
                    at += 2;
                }
                else
                {
                    uintptr_t r = strcmp(value, "null") == 0 ? 0 : (uintptr_t)strtoull(value, NULL, 10) + 1u;
                    memcpy(args + at, &r, sizeof(r));
                    at += WM_REF_SLOTS;
                }
            }
            result = wm_call(g_runtime, f, args, results);
            if (result)
            {
                printf("error %s\n", result);
                return;
            }
            printf("ok");
            at = 0;
            for (uint32_t i = 0; i < f->result_count; ++i)
            {
                print_value(f->results[i], results + at);
                at += wm_type_slots(f->results[i]);
            }
            printf("\n");
        }
    }
    else
        printf("error unknown command\n");
}


#ifdef WM_SPEC_PAGER
/*
 * -DWM_SPEC_PAGER=n: every memory swapped, with only n frames in RAM, so
 * frames go to storage (here, the heap) and come back all the time.
 */
static uint8_t** g_slots;
static uint32_t g_slot_count;
static uint32_t g_hand;
/* Frames taken out and brought back: stderr shows them at the end. */
static unsigned long g_taken, g_back;
static void show_paging(void) { fprintf(stderr, "paging: %lu frames taken out, %lu brought back\n", g_taken, g_back); }

static int test_reserve(void* context, int32_t frames)
{
    (void)context, (void)frames;
    return 1;
}

/* CLOCK over every frame of every memory: the first one not touched since the hand last passed. */
static int take_one(void)
{
    uint32_t total = 0;
    for (WmMemory* m = wm_memory_next(NULL); m; m = wm_memory_next(m))
        total += wm_memory_frames(m);
    for (uint32_t step = 0; total && step < 3u * total; ++step)
    {
        uint32_t at = g_hand++ % total;
        for (WmMemory* m = wm_memory_next(NULL); m; m = wm_memory_next(m))
        {
            if (at < wm_memory_frames(m))
            {
                if (wm_memory_frame_look(m, at) == 0)
                    return wm_memory_frame_take(m, at) == NULL && ++g_taken;
                break;
            }
            at -= wm_memory_frames(m);
        }
    }
    return 0;
}

static int test_room(void* context, int again)
{
    (void)context;
    if (again)
        return take_one();
    while (wm_pager_resident() >= (uint32_t)(WM_SPEC_PAGER))
        if (!take_one())
            return 1; /* all pinned or in use: let it grow past the budget */
    return 1;
}

static WmResult test_write(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t i = 0;
    (void)context;
    if (*slot != 0xFFFFFFFFu)
    {
        memcpy(g_slots[*slot], frame, 4096);
        return NULL;
    }
    while (i < g_slot_count && g_slots[i])
        ++i;
    if (i == g_slot_count)
    {
        uint8_t** slots = (uint8_t**)realloc(g_slots, (g_slot_count * 2u + 8u) * sizeof(uint8_t*));
        if (!slots)
            return "exhaustion: no storage";
        memset(slots + g_slot_count, 0, (g_slot_count + 8u) * sizeof(uint8_t*));
        g_slots = slots;
        g_slot_count = g_slot_count * 2u + 8u;
    }
    if (!(g_slots[i] = (uint8_t*)malloc(4096)))
        return "exhaustion: no storage";
    memcpy(g_slots[i], frame, 4096);
    *slot = i;
    return NULL;
}

static WmResult test_read(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], 4096);
    ++g_back;
    return NULL;
}

static void test_drop(void* context, uint32_t slot)
{
    (void)context;
    free(g_slots[slot]);
    g_slots[slot] = NULL;
}

static const WmPager g_test_pager = {NULL, test_reserve, test_room, test_write, test_read, test_drop};
#endif

int main(void)
{
    static char line[1 << 20];
#ifdef WM_SPEC_PAGER
    wm_set_pager(&g_test_pager);
    atexit(show_paging);
#endif
    g_runtime = wm_runtime_new(64u * 1024u, 1000u);
    make_spectest();
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof(line), stdin))
    {
        command(line);
        fflush(stdout);
    }
    return 0;
}
