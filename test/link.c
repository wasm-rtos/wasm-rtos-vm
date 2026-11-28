/*
 * What a host that links modules itself reads and does through wm.h
 * (decision 23): a module's imports, exports and custom sections, function
 * imports bound after instantiation, and funcref tables read and written.
 */
#include <stdio.h>
#include <stdlib.h>
#include "test.h"

static WmGlobal* g_base;

static uint8_t* load(const char* path, uint32_t extra, uint32_t* size)
{
    FILE* file = fopen(path, "rb");
    uint8_t* bytes = (uint8_t*)malloc(4096);
    *size = (uint32_t)fread(bytes, 1, 4096 - extra, file);
    fclose(file);
    return bytes;
}

/* A custom section at the end of the module, as a tool after the build adds one: id 0, size, name, contents. */
static void add_section(uint8_t* bytes, uint32_t* size, const char* name, const char* contents)
{
    uint32_t n = (uint32_t)strlen(name), c = (uint32_t)strlen(contents);
    bytes[(*size)++] = 0;
    bytes[(*size)++] = (uint8_t)(1u + n + c);
    bytes[(*size)++] = (uint8_t)n;
    memcpy(bytes + *size, name, n);
    *size += n;
    memcpy(bytes + *size, contents, c);
    *size += c;
}

static int is(const char* s, uint32_t length, const char* want)
{
    return s && length == strlen(want) && !memcmp(s, want, length);
}

/* Functions are left for wm_instance_link_function; the global is the host's. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    (void)context, (void)module, (void)module_length, (void)name, (void)name_length;
    out->kind = kind;
    if (kind == WM_EXTERN_GLOBAL)
        out->as.global = g_base;
    return NULL;
}

static WmResult host_twice(WmRuntime* runtime, void* user, uint32_t* slots, WmMemory* memory)
{
    (void)runtime, (void)user, (void)memory;
    slots[0] *= 2u;
    return NULL;
}

static uint32_t call(WmRuntime* rt, WmInstance* inst, const char* name, WmResult* error)
{
    uint32_t result = 0;
    *error = wm_call(rt, export_func(inst, name), NULL, &result);
    return result;
}

int main(void)
{
    static const uint8_t i32[] = {WM_I32};
    uint32_t lib_size, prog_size, size, count;
    uint8_t* lib_bytes = load("test/link_lib.wasm", 0, &lib_size);
    uint8_t* prog_bytes = load("test/link_prog.wasm", 64, &prog_size);
    WmModule *lib = NULL, *prog = NULL;
    WmInstance *lib_inst = NULL, *inst = NULL, *other = NULL;
    WmRuntime* rt = wm_runtime_new(test_store(), 64 * 1024, 100);
    WmFunc* twice = wm_host_function_new(i32, 1, i32, 1, host_twice, NULL);
    WmExternType t;
    WmExtern e;
    WmTable* table;
    const uint8_t* bytes;
    WmResult r;

    g_base = wm_global_new(test_store(), WM_I64, 0, 1u);
    add_section(prog_bytes, &prog_size, "wasm-rtos.test", "abc");
    add_section(prog_bytes, &prog_size, "other", "x");
    add_section(prog_bytes, &prog_size, "wasm-rtos.test", "de");
    check(!wm_module_load(&lib, NULL, lib_bytes, lib_size) && !wm_module_load(&prog, NULL, prog_bytes, prog_size),
          "both modules load, the program with custom sections added at its end");

    /* Imports. */
    count = wm_module_imports(prog, 0, &t);
    check(count == 4 && is(t.module, t.module_length, "lib") && is(t.name, t.name_length, "add") &&
              t.kind == WM_EXTERN_FUNC && t.index == 0 && t.param_count == 2 && t.result_count == 1 &&
              t.params[0] == WM_I32 && t.results[0] == WM_I32,
          "4 imports; the first: lib add, a function (i32 i32) -> i32 at index 0");
    wm_module_imports(prog, 2, &t);
    check(is(t.module, t.module_length, "env") && is(t.name, t.name_length, "twice") && t.index == 2,
          "the third: env twice, function index 2");
    wm_module_imports(prog, 3, &t);
    check(t.kind == WM_EXTERN_GLOBAL && t.type == WM_I64 && !t.mutable_ && t.index == 0,
          "the fourth: an immutable i64 global, global index 0");
    t.kind = 99;
    check(wm_module_imports(prog, 4, &t) == 4 && t.kind == 99 && wm_module_imports(prog, 0, NULL) == 4,
          "past the last, or with no out, only the count");

    /* Exports, of each kind. */
    count = wm_module_exports(lib, 0, &t);
    check(count == 5 && t.module == NULL && is(t.name, t.name_length, "add") && t.kind == WM_EXTERN_FUNC,
          "the library exports 5, the first the function add, with no module name");
    wm_module_exports(lib, 2, &t);
    check(is(t.name, t.name_length, "mem") && t.kind == WM_EXTERN_MEMORY && t.min == 1 && t.max == 2 && t.has_max &&
              t.page_log2 == 16,
          "mem: a memory of 1 to 2 pages of 64 KiB");
    wm_module_exports(lib, 3, &t);
    check(t.kind == WM_EXTERN_GLOBAL && t.type == WM_I32 && t.mutable_, "count: a mutable i32 global");
    wm_module_exports(lib, 4, &t);
    check(t.kind == WM_EXTERN_TABLE && t.type == WM_FUNCREF && t.min == 2 && !t.has_max,
          "table: funcref, 2 entries, no maximum");
    wm_module_exports(prog, 4, &t);
    check(is(t.name, t.name_length, "twice") && t.kind == WM_EXTERN_FUNC && t.index == 2 && t.param_count == 1,
          "an exported import: its index is the import's");

    /* Custom sections. */
    count = wm_module_custom_sections(prog, "wasm-rtos.test", 14, 1, &bytes, &size);
    check(count == 2 && size == 2 && !memcmp(bytes, "de", 2), "two sections of that name; the second holds de");
    wm_module_custom_sections(prog, "wasm-rtos.test", 14, 0, &bytes, &size);
    check(size == 3 && !memcmp(bytes, "abc", 3), "the first holds abc");
    check(wm_module_custom_sections(prog, "other", 5, 0, &bytes, &size) == 1 && size == 1 && bytes[0] == 'x',
          "another name, one section");
    check(wm_module_custom_sections(prog, "wasm-rtos", 9, 0, &bytes, &size) == 0 &&
              wm_module_custom_sections(lib, "wasm-rtos.test", 14, 0, &bytes, &size) == 0,
          "a prefix of a name is not the name; a module without them has none");

    /* Linking late. */
    check(!wm_instance_new(&lib_inst, test_store(), lib, rt, NULL, NULL) && !wm_instance_new(&inst, test_store(), prog, rt, resolve, NULL),
          "the library, then the program with its functions unresolved");
    call(rt, inst, "run", &r);
    check(r != NULL, "an unlinked import traps");
    check(wm_instance_link_function(inst, 0, export_func(lib_inst, "add")) == NULL &&
              call(rt, inst, "run", &r) == 5 && r == NULL,
          "add linked: run gives 2 + 3");
    check(wm_instance_link_function(inst, 0, export_func(lib_inst, "add")) != NULL,
          "linking it again is refused");
    check(wm_instance_link_function(inst, 1, export_func(lib_inst, "add")) != NULL,
          "a function of another type is refused");
    check(wm_instance_link_function(inst, 3, export_func(lib_inst, "seven")) != NULL &&
              wm_instance_link_function(inst, 1, NULL) != NULL,
          "past the function imports, or nothing, is refused");
    check(!wm_instance_new(&other, test_store(), prog, rt, resolve, NULL) &&
              wm_instance_link_function(inst, 2, export_func(other, "twice")) != NULL,
          "another instance's import, still unlinked, is refused as the target");
    check(wm_instance_link_function(inst, 1, export_func(lib_inst, "seven")) == NULL &&
              call(rt, inst, "indirect", &r) == 7 && r == NULL,
          "seven linked: the table entry made of the import calls it");
    check(wm_instance_link_function(inst, 2, twice) == NULL && call(rt, inst, "doubled", &r) == 42 && r == NULL,
          "a host function linked: doubled gives 42");
    check(wm_instance_link_function(other, 2, export_func(inst, "twice")) == NULL &&
              wm_instance_link_function(other, 0, export_func(lib_inst, "add")) == NULL &&
              call(rt, other, "doubled", &r) == 42 && call(rt, other, "run", &r) == 5,
          "once linked, an import is a target for another instance");

    /* What the modules define, imported or their own. */
    check(wm_module_memories(lib, 0, &t) == 1 && t.module == NULL && t.min == 1 && t.max == 2 && t.has_max &&
              wm_module_tables(lib, 0, &t) == 1 && t.module == NULL && t.type == WM_FUNCREF && t.min == 2,
          "the library's own memory and table, with their limits");
    check(wm_module_memories(prog, 0, &t) == 0 && wm_module_tables(prog, 0, &t) == 1 && t.min == 1 && !t.module,
          "the program: no memory, one table of its own");
    check(wm_func_equal(export_func(inst, "twice"), twice) && !wm_func_equal(twice, export_func(lib_inst, "seven")),
          "an import bound to a host function is that function, and no other");
    {
        WmExtern entry;
        check(!wm_instance_export(inst, "table", 5, &entry) &&
                  wm_func_equal(wm_table_get(entry.as.table, 0), export_func(lib_inst, "seven")) &&
                  wm_table_get(entry.as.table, 0) != export_func(lib_inst, "seven"),
              "a table entry made of an import bound late is the same function as its target, not the same object");
    }
    wm_global_set(g_base, 77u);
    check(wm_global_get(g_base) == 77u, "a global the host made, set by the host");

    /* Tables. */
    check(wm_instance_export(inst, "table", 5, &e) == NULL && wm_table_size(e.as.table) == 1 &&
              wm_table_get(e.as.table, 0) != NULL && wm_table_get(e.as.table, 1) == NULL,
          "the program's table: 1 entry, set by its element segment; past it, NULL");
    table = wm_table_new(test_store(), WM_FUNCREF, 2u, 2u);
    check(wm_table_get(table, 0) == NULL && wm_table_set(table, 1, export_func(lib_inst, "seven")) == NULL &&
              wm_table_get(table, 1) == export_func(lib_inst, "seven"),
          "a new table: null, then set and read back");
    check(wm_table_set(table, 2, NULL) != NULL && wm_table_set(table, 1, NULL) == NULL && wm_table_get(table, 1) == NULL,
          "past its end refused; set to null");
    wm_table_free(table);
    table = wm_table_new(test_store(), WM_EXTERNREF, 1u, 1u);
    check(wm_table_set(table, 0, twice) != NULL && wm_table_get(table, 0) == NULL, "an externref table: refused");
    wm_table_free(table);

    wm_instance_free(other);
    wm_instance_free(inst);
    wm_instance_free(lib_inst);
    wm_module_free(prog);
    wm_module_free(lib);
    wm_host_function_free(twice);
    wm_global_free(g_base);
    wm_runtime_free(rt);
    free(lib_bytes);
    free(prog_bytes);
    return summary();
}
