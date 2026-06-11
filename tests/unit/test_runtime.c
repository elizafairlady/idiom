#include "test_util.h"

static IdmValue lit_part(IdmRuntime *rt, const char *text, IdmError *err) {
    IdmValue items[2];
    items[0] = idm_atom(rt, "lit");
    items[1] = idm_string(rt, text, err);
    return idm_tuple(rt, items, 2u, err);
}

static IdmValue build_capture_graph(IdmRuntime *rt, const char *arg0, const char *arg1, IdmError *err) {
    IdmValue argv = idm_cons(rt, lit_part(rt, arg0, err), idm_cons(rt, lit_part(rt, arg1, err), idm_nil(), err), err);
    IdmValue stage_items[4];
    stage_items[0] = idm_atom(rt, "stage");
    stage_items[1] = argv;
    stage_items[2] = idm_nil();
    stage_items[3] = idm_nil();
    IdmValue stage = idm_tuple(rt, stage_items, 4u, err);
    IdmValue graph_items[3];
    graph_items[0] = idm_atom(rt, "exec");
    graph_items[1] = stage;
    graph_items[2] = idm_atom(rt, "true");
    return idm_tuple(rt, graph_items, 3u, err);
}

static void run_port_expect(IdmRuntime *rt, IdmValue graph, const char *expect_written) {
    IdmError err;
    idm_error_init(&err);
    IdmPort *port = idm_port_launch(rt, graph, NULL, &err);
    CHECK(port != NULL);
    CHECK(!err.present);
    if (!port) { idm_error_clear(&err); return; }
    int spins = 0;
    while (!idm_port_try_complete(port) && spins < 1000000) spins++;
    IdmValue result = idm_port_result(port, rt, &err);
    CHECK(!err.present);
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, result));
    CHECK_STR(buf.data, expect_written);
    idm_buf_destroy(&buf);
    idm_port_free(port);
    idm_error_clear(&err);
}

static void test_gc_stress(void) {
    setenv("IDIOMGC", "1500", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *src =
        "use std/enum\n"
        "keep = [10 20 30 40]\n"
        "defn loop n -> cond (eq? n 0) (sum keep) (do (map [1 2 3 4 5] (fn x -> mul x x)); (loop (sub n 1)) end)\n"
        "loop 200\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt, "<gc>", src, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(!err.present);
    IdmScheduler *sched = idm_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IdmValue out = idm_nil();
    CHECK(idm_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 100);
    idm_sched_destroy(sched);
    idm_bc_destroy(&module);
    idm_core_free(core);
    unsetenv("IDIOMGC");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_heap_accounting_sweep(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    size_t baseline = idm_heap_bytes(&rt.heap);
    char *payload = malloc(10000u);
    CHECK(payload != NULL);
    memset(payload, 'x', 10000u);
    IdmValue s = idm_string_n(&rt, payload, 10000u, &err);
    free(payload);
    CHECK(!err.present && s.tag == IDM_VAL_STRING);
    CHECK(idm_heap_bytes(&rt.heap) >= baseline + 10000u);
    idm_heap_sweep(&rt.heap);
    CHECK(idm_heap_bytes(&rt.heap) == baseline);
    CHECK(idm_heap_object_count(&rt.heap) == 0);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_gc_recurring_collection(void) {
    setenv("IDIOMGC", "20000", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    const char *src =
        "defn big n s -> cond (eq? n 0) s (big (sub n 1) (str s s))\n"
        "defn loop k -> cond (eq? k 0) 0 (do (big 12 \"aaaaaaaaaaaaaaaa\"); (loop (sub k 1)) end)\n"
        "loop 80\n";
    CHECK(idm_expand_string(&rt, "<gc-recurring>", src, &core, &err));
    CHECK(!err.present);
    size_t baseline = idm_heap_bytes(&rt.heap);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    IdmScheduler *sched = idm_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IdmValue out = idm_nil();
    CHECK(idm_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 0);
    CHECK(idm_heap_bytes(&rt.heap) < baseline + 5u * 1024u * 1024u);
    idm_sched_destroy(sched);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    unsetenv("IDIOMGC");
}

static void test_port_result_gc_rooted(void) {
    setenv("IDIOMGC", "1500", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "p = echo hi &\n"
        "a = await p\n"
        "defn churn n do\n"
        "  cond (eq? n 0) 0 (do\n"
        "    s = str \"aaaaaaaaaaaaaaaaaaaa\" n\n"
        "    churn (sub n 1)\n"
        "  end)\n"
        "end\n"
        "churn 400\n"
        "b = await p\n"
        "b\n",
        "{:ok 0 \"hi\\n\" \"\"}");
    idm_runtime_destroy(&rt);
    unsetenv("IDIOMGC");
}

static void test_procsub_temp_cleanup(void) {
    size_t before = count_procsub_temps();
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "x = cat <(echo hello)\n"
        "x\n",
        "{:ok 0 \"hello\\n\" \"\"}");
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "x = cat <(echo a) <(echo b)\n"
        "x\n",
        "{:ok 0 \"a\\nb\\n\" \"\"}");
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "echo data > >(cat > /tmp/idm_d9_psw)\n"
        "y = cat /tmp/idm_d9_psw\n"
        "y\n",
        "{:ok 0 \"data\\n\" \"\"}");
    remove("/tmp/idm_d9_psw");
    idm_runtime_destroy(&rt);
    CHECK(count_procsub_temps() == before);
}

static void test_heredoc_leaves_no_files(void) {
    size_t before = count_glob_matches("/tmp/idm_heredoc_*");
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "x = cat <<E\n"
        "hi\n"
        "E\n"
        "x\n",
        "{:ok 0 \"hi\\n\" \"\"}");
    idm_runtime_destroy(&rt);
    CHECK(count_glob_matches("/tmp/idm_heredoc_*") == before);
}

static void test_temp_registry_backstop(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt, "<backstop>", "make-procsub-temp\n", &core, &err));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    IdmScheduler *sched = idm_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IdmValue out = idm_nil();
    CHECK(idm_sched_run_main(sched, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_STRING);
    char path[256];
    size_t len = idm_string_length(out) < sizeof(path) - 1u ? idm_string_length(out) : sizeof(path) - 1u;
    memcpy(path, idm_string_bytes(out), len);
    path[len] = '\0';
    FILE *probe = fopen(path, "rb");
    CHECK(probe != NULL);
    if (probe) fclose(probe);
    idm_sched_destroy(sched);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    probe = fopen(path, "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);
}

static void test_actor_cwd_env_scoping(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    char before[4096];
    CHECK(getcwd(before, sizeof(before)) != NULL);
    check_sched_value_written(&rt,
        "cd \"/tmp\"\n"
        "pwd\n",
        "\"/tmp\"");
    char after[4096];
    CHECK(getcwd(after, sizeof(after)) != NULL);
    CHECK_STR(after, before);
    check_sched_value_written(&rt,
        "env-set \"IDM_F5_UNIT\" \"scoped\"\n"
        "env-get \"IDM_F5_UNIT\"\n",
        "\"scoped\"");
    CHECK(getenv("IDM_F5_UNIT") == NULL);
    idm_runtime_destroy(&rt);
}

static void test_port_capture_limit(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    setenv("IDM_CAPTURE_LIMIT", "1024", 1);
    run_port_expect(&rt, build_capture_graph(&rt, "printf", "hello", &err), "{:ok 0 \"hello\" \"\"}");
    setenv("IDM_CAPTURE_LIMIT", "8", 1);
    run_port_expect(&rt, build_capture_graph(&rt, "printf", "aaaaaaaaaaaaaaaaaaaa", &err), "{:error :capture-overflow \"aaaaaaaa\" \"\"}");
    unsetenv("IDM_CAPTURE_LIMIT");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

void run_runtime_suite(void) {
    test_gc_stress();
    test_heap_accounting_sweep();
    test_gc_recurring_collection();
    test_port_result_gc_rooted();
    test_procsub_temp_cleanup();
    test_heredoc_leaves_no_files();
    test_temp_registry_backstop();
    test_actor_cwd_env_scoping();
    test_port_capture_limit();
}
