#include "test_util.h"

int failures = 0;

char *dump_reader(const char *src) {
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;
    IdmRuntime rt;
    idm_runtime_init(&rt);
    if (!idm_expand_read_source_string(&rt, "<test>", src, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        idm_runtime_destroy(&rt);
        return NULL;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_syn_dump(&buf, program));
    idm_syn_free(program);
    idm_runtime_destroy(&rt);
    return idm_buf_take(&buf);
}

void check_operator_eval(IdmRuntime *rt, const char *source, const char *expect_dump, int64_t expect_value) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(rt, "<operator-surface-test>", source, &core, &err));
    CHECK(!err.present);
    if (expect_dump) {
        IdmBuffer dump;
        idm_buf_init(&dump);
        CHECK(idm_core_dump(&dump, core));
        CHECK_STR(dump.data, expect_dump);
        idm_buf_destroy(&dump);
    }
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == IDM_VAL_INT && out.as.i == expect_value);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
}

void check_value_written(IdmRuntime *rt, const char *source, const char *expect_written) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(rt, "<quote-test>", source, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(!err.present);
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data, expect_written);
    idm_buf_destroy(&buf);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_expand_error_note_rt(IdmRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(!idm_expand_source_string(rt, label, source, &core, &err));
    CHECK(err.present);
    if (err.message && !strstr(err.message, expect_message)) {
        fprintf(stderr, "FAIL %s: expected error containing \"%s\", got \"%s\"\n", label, expect_message, err.message);
        failures++;
    }
    if (!err.notes || !strstr(err.notes, expect_note)) {
        fprintf(stderr, "FAIL %s: expected note containing \"%s\", got \"%s\"\n", label, expect_note, err.notes ? err.notes : "(none)");
        failures++;
    }
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_runtime_error_note(IdmRuntime *rt, const char *label, const char *source, const char *expect_message, const char *expect_note) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(rt, label, source, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(!idm_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    if (err.message && !strstr(err.message, expect_message)) {
        fprintf(stderr, "FAIL %s: expected runtime error containing \"%s\", got \"%s\"\n", label, expect_message, err.message);
        failures++;
    }
    if (!err.notes || !strstr(err.notes, expect_note)) {
        fprintf(stderr, "FAIL %s: expected runtime note containing \"%s\", got \"%s\"\n", label, expect_note, err.notes ? err.notes : "(none)");
        failures++;
    }
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_runtime_error_contains(IdmRuntime *rt, const char *label, const char *source, const char *expect_substring) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(rt, label, source, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(!err.present);
    CHECK(idm_bc_intern_literals(rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(!idm_vm_run(rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, expect_substring)) {
        fprintf(stderr, "FAIL %s: expected runtime error containing \"%s\", got \"%s\"\n", label, expect_substring, err.message);
        failures++;
    }
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_expand_result_rt(IdmRuntime *rt, const char *label, const char *source, bool should_succeed) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    bool ok = idm_expand_source_string(rt, label, source, &core, &err);
    CHECK(ok == should_succeed);
    if (should_succeed) CHECK(!err.present && core != NULL);
    else CHECK(err.present && core == NULL);
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_expand_error_rt(IdmRuntime *rt, const char *label, const char *source, const char *expect_substring) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    bool ok = idm_expand_source_string(rt, label, source, &core, &err);
    CHECK(!ok && err.present && core == NULL);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, expect_substring)) {
        fprintf(stderr, "FAIL %s: expected error containing \"%s\", got \"%s\"\n", label, expect_substring, err.message);
        failures++;
    }
    idm_core_free(core);
    idm_error_clear(&err);
}

void expect_expand_result(const char *label, const char *source, bool should_succeed) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_result_rt(&rt, label, source, should_succeed);
    idm_runtime_destroy(&rt);
}

void check_sched_value_written(IdmRuntime *rt, const char *source, const char *expect_written) {
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(rt, "<sched-test>", source, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(rt, &module, &err));
    IdmScheduler *sched = idm_sched_create(rt, &module, &err);
    CHECK(sched != NULL);
    IdmValue out = idm_nil();
    CHECK(idm_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data, expect_written);
    idm_buf_destroy(&buf);
    idm_sched_destroy(sched);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
}

size_t count_procsub_temps(void) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    size_t n = 0;
    if (glob("/tmp/idm_procsub_*", 0, NULL, &g) == 0) n = g.gl_pathc;
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
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, source, expect);
    idm_runtime_destroy(&rt);
}
