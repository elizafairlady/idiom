#include "idiom/core.h"
#include "idiom/expand.h"
#include "idiom/value.h"
#include "idiom/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *name) {
    fprintf(stderr, "string_case: %s\n", name);
    exit(1);
}

static void fail_error(IdmError *err, const char *name) {
    if (err->present) idm_error_fprint(stderr, err);
    fail(name);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_ok(bool ok, IdmError *err, const char *name) {
    if (!ok || err->present) fail_error(err, name);
}

static bool contains_text(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

static IdmValue chunk_string(IdmRuntime *rt, char byte, IdmError *err) {
    char text[2049];
    memset(text, byte, 2048u);
    text[2048] = '\0';
    IdmValue v = idm_string(rt, text, err);
    check_ok(!err->present, err, "chunk string");
    return v;
}

static IdmValue rope_of_chunks(IdmRuntime *rt, IdmValue chunk, size_t count, IdmError *err) {
    IdmValue acc = chunk;
    for (size_t i = 1; i < count; i++) {
        check_ok(idm_string_concat2(rt, acc, chunk, &acc, err), err, "rope concat");
    }
    return acc;
}

static void test_home_prim_rebind(void) {
    IdmError err;
    idm_error_init(&err);
    const char *root_env = getenv("IDIOMROOT");
    check(root_env != NULL && root_env[0] != '\0', "rebind needs IDIOMROOT");
    char *real_root = idm_strdup(root_env);
    check(real_root != NULL, "rebind root copy");
    const char *cache_env = getenv("IDIOMCACHE");
    char *old_cache = cache_env ? idm_strdup(cache_env) : NULL;
    char tmpl[] = "/tmp/idiom_string_rebind_XXXXXX";
    char *root = mkdtemp(tmpl);
    check(root != NULL, "rebind mkdtemp");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/std", root);
    check(mkdir(dir, 0700) == 0, "rebind mkdir std");
    char kernel_link[512];
    snprintf(kernel_link, sizeof(kernel_link), "%s/std/kernel", root);
    char kernel_real[512];
    snprintf(kernel_real, sizeof(kernel_real), "%s/std/kernel", real_root);
    check(symlink(kernel_real, kernel_link) == 0, "rebind kernel symlink");
    snprintf(dir, sizeof(dir), "%s/std/string", root);
    check(mkdir(dir, 0700) == 0, "rebind mkdir string");
    char path[1024];
    snprintf(path, sizeof(path), "%s/pkg.id", dir);
    FILE *f = fopen(path, "wb");
    check(f != NULL, "rebind open");
    const char *src =
        "package string\n"
        "\n"
        "export defn chomp _s -> \"rebound\"\n"
        "export defn probe -> chomp \"hi\\n\"\n"
        "export defmacro chomped %`(chomped %,s) -> make-syntax-string s (chomp \"x\\n\")\n";
    check(fwrite(src, 1u, strlen(src), f) == strlen(src), "rebind write");
    check(fclose(f) == 0, "rebind close");
    check(setenv("IDIOMROOT", root, 1) == 0, "rebind setenv");
    check(setenv("IDIOMCACHE", "0", 1) == 0, "rebind cache off");

    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmCore *core = NULL;
    check_ok(idm_expand_source_string(&rt, "<rebind>", "use std/string\n{probe (chomp \"hi\\n\") (chomped :x)}\n", &core, &err), &err, "rebind expand");
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check_ok(idm_core_compile_main(&rt, core, &module, &main_fn, &err), &err, "rebind compile");
    idm_core_free(core);
    check_ok(idm_bc_intern_literals(&rt, &module, &err), &err, "rebind intern");
    IdmValue out = idm_nil();
    check_ok(idm_vm_run(&rt, &module, main_fn, &out, &err), &err, "rebind run");
    check(idm_value_tag(out) == IDM_VAL_TUPLE, "rebind tuple");
    IdmValue in_pkg = idm_sequence_item(out, 0u, &err);
    IdmValue client = idm_sequence_item(out, 1u, &err);
    IdmValue macro_time = idm_sequence_item(out, 2u, &err);
    check_ok(!err.present, &err, "rebind items");
    check(idm_value_tag(in_pkg) == IDM_VAL_STRING && strcmp(idm_string_bytes(in_pkg), "rebound") == 0,
          "home-package internal reference sees the defn, not the seeded primitive");
    check(idm_value_tag(client) == IDM_VAL_STRING && strcmp(idm_string_bytes(client), "rebound") == 0,
          "client call through use sees the defn, not a name-matched primitive");
    check(idm_value_tag(macro_time) == IDM_VAL_STRING && strcmp(idm_string_bytes(macro_time), "rebound") == 0,
          "phase-1 macro body sees the defn, not the phase-ANY seeded primitive");

    check(setenv("IDIOMROOT", real_root, 1) == 0, "rebind restore root");
    if (old_cache) {
        check(setenv("IDIOMCACHE", old_cache, 1) == 0, "rebind restore cache");
        free(old_cache);
    } else {
        unsetenv("IDIOMCACHE");
    }
    free(real_root);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

static void test_bind_compiles_to_match_bind(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmCore *core = NULL;
    check_ok(idm_expand_source_string(&rt, "<bind>", "defn split-pair p -> do\n  {a b} = p\n  add a b\nend\nsplit-pair {1 2}\n", &core, &err), &err, "bind expand");
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check_ok(idm_core_compile_main(&rt, core, &module, &main_fn, &err), &err, "bind compile");
    idm_core_free(core);
    check_ok(idm_bc_intern_literals(&rt, &module, &err), &err, "bind intern");
    IdmBuffer dis;
    idm_buf_init(&dis);
    check(idm_bc_disassemble(&dis, &module), "bind disassemble");
    check(dis.data && strstr(dis.data, "MATCH_BIND") != NULL, "a destructuring bind compiles to MATCH_BIND");
    idm_buf_destroy(&dis);
    IdmValue out = idm_nil();
    check_ok(idm_vm_run(&rt, &module, main_fn, &out, &err), &err, "bind run");
    check(idm_value_tag(out) == IDM_VAL_INT && idm_int_value(out) == 3, "bind result");
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

static void test_package_home_prim_direct_call(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmCore *core = NULL;
    const char *src =
        "use std/string\n"
        "defn byte-probe s i -> byte s i\n"
        "byte-probe \"A\" 0\n";
    check_ok(idm_expand_source_string(&rt, "<direct-prim>", src, &core, &err), &err, "direct primitive expand");
    IdmBuffer core_dump;
    idm_buf_init(&core_dump);
    check(idm_core_dump(&core_dump, core), "direct primitive core dump");
    check(contains_text(core_dump.data, "fn-multi byte (/2 primitive byte)"), "package-home primitive survives artifact identity");
    idm_buf_destroy(&core_dump);

    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check_ok(idm_core_compile_main(&rt, core, &module, &main_fn, &err), &err, "direct primitive compile");
    idm_core_free(core);
    check_ok(idm_bc_intern_literals(&rt, &module, &err), &err, "direct primitive intern");
    IdmBuffer dis;
    idm_buf_init(&dis);
    check(idm_bc_disassemble(&dis, &module), "direct primitive disassemble");
    check(contains_text(dis.data, "CALL_PRIMITIVE") && contains_text(dis.data, "primitive=byte#"), "package-home primitive emits CALL_PRIMITIVE");
    idm_buf_destroy(&dis);
    IdmValue out = idm_nil();
    check_ok(idm_vm_run(&rt, &module, main_fn, &out, &err), &err, "direct primitive run");
    check(idm_value_tag(out) == IDM_VAL_INT && idm_int_value(out) == 65, "direct primitive result");
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

static void test_home_prim_folds_through_use(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmCore *core = NULL;
    check_ok(idm_expand_source_string(&rt, "<fold>", "use std/string\nchomp \"hi\\n\"\n", &core, &err), &err, "fold expand");
    IdmBuffer dump;
    idm_buf_init(&dump);
    check(idm_core_dump(&dump, core), "fold dump");
    check(dump.data && strstr(dump.data, "chomp") == NULL, "home primitive call through use is folded away");
    check(dump.data && strstr(dump.data, "hi") != NULL, "the folded literal survives in core");
    idm_buf_destroy(&dump);
    idm_core_free(core);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
}

int idm_unit_string_case(void) {
    test_home_prim_rebind();
    test_home_prim_folds_through_use();
    test_package_home_prim_direct_call();
    test_bind_compiles_to_match_bind();
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);

    IdmCore *core = NULL;
    check_ok(idm_expand_source_string(&rt, "<string_case>", "use std/string\n{&upcase &downcase}\n", &core, &err), &err, "expand std/string");
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = UINT32_MAX;
    check_ok(idm_core_compile_main(&rt, core, &module, &main_fn, &err), &err, "compile");
    idm_core_free(core);
    check_ok(idm_bc_intern_literals(&rt, &module, &err), &err, "intern literals");
    IdmValue closures = idm_nil();
    check_ok(idm_vm_run(&rt, &module, main_fn, &closures, &err), &err, "run main");
    check(idm_value_tag(closures) == IDM_VAL_TUPLE, "closure tuple");
    IdmValue upcase = idm_sequence_item(closures, 0u, &err);
    IdmValue downcase = idm_sequence_item(closures, 1u, &err);
    check_ok(!err.present, &err, "closure items");

    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);

    IdmValue upper_chunk = chunk_string(&rt, 'A', &err);
    IdmValue lower_chunk = chunk_string(&rt, 'a', &err);
    IdmValue clean_upper = rope_of_chunks(&rt, upper_chunk, 32u, &err);
    IdmValue clean_lower = rope_of_chunks(&rt, lower_chunk, 32u, &err);

    size_t before = idm_heap_bytes(&heap);
    IdmValue out = idm_nil();
    check_ok(idm_vm_call_closure(&rt, upcase, &clean_upper, 1u, &out, &err), &err, "upcase clean call");
    size_t after = idm_heap_bytes(&heap);
    check(out.bits == clean_upper.bits, "upcase of cased input returns its input");
    check(after == before, "upcase of cased input allocates zero bytes");

    before = idm_heap_bytes(&heap);
    out = idm_nil();
    check_ok(idm_vm_call_closure(&rt, downcase, &clean_lower, 1u, &out, &err), &err, "downcase clean call");
    after = idm_heap_bytes(&heap);
    check(out.bits == clean_lower.bits, "downcase of cased input returns its input");
    check(after == before, "downcase of cased input allocates zero bytes");

    char raw_bytes[2049];
    memset(raw_bytes, 0xFF, 2048u);
    raw_bytes[2048] = '\0';
    IdmValue raw_chunk = idm_string_n(&rt, raw_bytes, 2048u, &err);
    check_ok(!err.present, &err, "raw chunk");
    IdmValue raw_rope = rope_of_chunks(&rt, raw_chunk, 32u, &err);
    before = idm_heap_bytes(&heap);
    out = idm_nil();
    check_ok(idm_vm_call_closure(&rt, upcase, &raw_rope, 1u, &out, &err), &err, "upcase raw call");
    after = idm_heap_bytes(&heap);
    check(out.bits == raw_rope.bits, "upcase of raw bytes returns its input");
    check(after == before, "upcase of raw bytes allocates zero bytes");

    IdmValue prefix = idm_string(&rt, "AAAAB", &err);
    check_ok(!err.present, &err, "prefix");
    IdmValue half = rope_of_chunks(&rt, upper_chunk, 16u, &err);
    IdmValue island = idm_string(&rt, "abcdefgh", &err);
    check_ok(!err.present, &err, "island");
    IdmValue sparse = prefix;
    check_ok(idm_string_concat2(&rt, sparse, half, &sparse, &err), &err, "sparse concat head");
    check_ok(idm_string_concat2(&rt, sparse, island, &sparse, &err), &err, "sparse concat island");
    check_ok(idm_string_concat2(&rt, sparse, half, &sparse, &err), &err, "sparse concat tail");

    before = idm_heap_bytes(&heap);
    out = idm_nil();
    check_ok(idm_vm_call_closure(&rt, upcase, &sparse, 1u, &out, &err), &err, "upcase sparse call");
    after = idm_heap_bytes(&heap);
    check(after > before, "sparse upcase allocates");
    check(after - before < idm_string_length(sparse) / 4u, "sparse upcase allocates O(runs), not O(bytes)");

    IdmValue island_upper = idm_string(&rt, "ABCDEFGH", &err);
    check_ok(!err.present, &err, "island upper");
    IdmValue expected = prefix;
    check_ok(idm_string_concat2(&rt, expected, half, &expected, &err), &err, "expected concat head");
    check_ok(idm_string_concat2(&rt, expected, island_upper, &expected, &err), &err, "expected concat island");
    check_ok(idm_string_concat2(&rt, expected, half, &expected, &err), &err, "expected concat tail");
    check(idm_value_equal(out, expected), "sparse upcase result content");

    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    return 0;
}
