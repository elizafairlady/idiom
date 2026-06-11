#include "test_util.h"

int failures = 0;

char *dump_reader(const char *src) {
    IshError err;
    ish_error_init(&err);
    IshSyntax *program = NULL;
    if (!ish_reader_read_string("<test>", src, &program, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        return NULL;
    }
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_syn_dump(&buf, program));
    ish_syn_free(program);
    return ish_buf_take(&buf);
}

void check_operator_eval(IshRuntime *rt, const char *source, const char *expect_dump, int64_t expect_value) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(rt, "<operator-surface-test>", source, &core, &err));
    CHECK(!err.present);
    if (expect_dump) {
        IshBuffer dump;
        ish_buf_init(&dump);
        CHECK(ish_core_dump(&dump, core));
        CHECK_STR(dump.data, expect_dump);
        ish_buf_destroy(&dump);
    }
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == ISH_VAL_INT && out.as.i == expect_value);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
}

void check_value_written(IshRuntime *rt, const char *source, const char *expect_written) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(rt, "<quote-test>", source, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(!err.present);
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, out));
    CHECK_STR(buf.data, expect_written);
    ish_buf_destroy(&buf);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_expand_error_note_rt(IshRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(!ish_expand_string(rt, label, source, &core, &err));
    CHECK(err.present);
    if (err.message && !strstr(err.message, expect_message)) {
        fprintf(stderr, "FAIL %s: expected error containing \"%s\", got \"%s\"\n", label, expect_message, err.message);
        failures++;
    }
    if (!err.notes || !strstr(err.notes, expect_note)) {
        fprintf(stderr, "FAIL %s: expected note containing \"%s\", got \"%s\"\n", label, expect_note, err.notes ? err.notes : "(none)");
        failures++;
    }
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_runtime_error_note(IshRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(rt, label, source, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(!ish_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    if (err.message && !strstr(err.message, expect_message)) {
        fprintf(stderr, "FAIL %s: expected runtime error containing \"%s\", got \"%s\"\n", label, expect_message, err.message);
        failures++;
    }
    if (!err.notes || !strstr(err.notes, expect_note)) {
        fprintf(stderr, "FAIL %s: expected runtime note containing \"%s\", got \"%s\"\n", label, expect_note, err.notes ? err.notes : "(none)");
        failures++;
    }
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_runtime_error_contains(IshRuntime *rt, const char *label, const char *source, const char *expect_substring) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(rt, label, source, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(!err.present);
    IshValue out = ish_nil();
    CHECK(!ish_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, expect_substring)) {
        fprintf(stderr, "FAIL %s: expected runtime error containing \"%s\", got \"%s\"\n", label, expect_substring, err.message);
        failures++;
    }
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_expand_result_rt(IshRuntime *rt, const char *label, const char *source, bool should_succeed) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    bool ok = ish_expand_string(rt, label, source, &core, &err);
    CHECK(ok == should_succeed);
    if (should_succeed) CHECK(!err.present && core != NULL);
    else CHECK(err.present && core == NULL);
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_expand_error_rt(IshRuntime *rt, const char *label, const char *source, const char *expect_substring) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    bool ok = ish_expand_string(rt, label, source, &core, &err);
    CHECK(!ok && err.present && core == NULL);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, expect_substring)) {
        fprintf(stderr, "FAIL %s: expected error containing \"%s\", got \"%s\"\n", label, expect_substring, err.message);
        failures++;
    }
    ish_core_free(core);
    ish_error_clear(&err);
}

void expect_expand_result(const char *label, const char *source, bool should_succeed) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_result_rt(&rt, label, source, should_succeed);
    ish_runtime_destroy(&rt);
}

void check_sched_value_written(IshRuntime *rt, const char *source, const char *expect_written) {
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(rt, "<sched-test>", source, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshScheduler *sched = ish_sched_create(rt, &module, &err);
    CHECK(sched != NULL);
    IshValue out = ish_nil();
    CHECK(ish_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, out));
    CHECK_STR(buf.data, expect_written);
    ish_buf_destroy(&buf);
    ish_sched_destroy(sched);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
}

size_t count_procsub_temps(void) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    size_t n = 0;
    if (glob("/tmp/ish_procsub_*", 0, NULL, &g) == 0) n = g.gl_pathc;
    globfree(&g);
    return n;
}

size_t count_glob_matches(const char *pattern) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    size_t n = 0;
    if (glob(pattern, 0, NULL, &g) == 0) n = g.gl_pathc;
    globfree(&g);
    return n;
}

bool write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(content);
    bool ok = fwrite(content, 1u, len, f) == len;
    return fclose(f) == 0 && ok;
}

void check_pkg_value(const char *source, const char *expect) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt, source, expect);
    ish_runtime_destroy(&rt);
}
