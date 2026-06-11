#include "test_util.h"

static IshValue lit_part(IshRuntime *rt, const char *text, IshError *err) {
    IshValue items[2];
    items[0] = ish_atom(rt, "lit");
    items[1] = ish_string(rt, text, err);
    return ish_tuple(rt, items, 2u, err);
}

static IshValue build_capture_graph(IshRuntime *rt, const char *arg0, const char *arg1, IshError *err) {
    IshValue argv = ish_cons(rt, lit_part(rt, arg0, err), ish_cons(rt, lit_part(rt, arg1, err), ish_nil(), err), err);
    IshValue stage_items[4];
    stage_items[0] = ish_atom(rt, "stage");
    stage_items[1] = argv;
    stage_items[2] = ish_nil();
    stage_items[3] = ish_nil();
    IshValue stage = ish_tuple(rt, stage_items, 4u, err);
    IshValue graph_items[3];
    graph_items[0] = ish_atom(rt, "exec");
    graph_items[1] = stage;
    graph_items[2] = ish_atom(rt, "true");
    return ish_tuple(rt, graph_items, 3u, err);
}

static void run_port_expect(IshRuntime *rt, IshValue graph, const char *expect_written) {
    IshError err;
    ish_error_init(&err);
    IshPort *port = ish_port_launch(rt, graph, NULL, &err);
    CHECK(port != NULL);
    CHECK(!err.present);
    if (!port) { ish_error_clear(&err); return; }
    int spins = 0;
    while (!ish_port_try_complete(port) && spins < 1000000) spins++;
    IshValue result = ish_port_result(port, rt, &err);
    CHECK(!err.present);
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, result));
    CHECK_STR(buf.data, expect_written);
    ish_buf_destroy(&buf);
    ish_port_free(port);
    ish_error_clear(&err);
}

static void test_gc_stress(void) {
    setenv("ISH_GC_THRESHOLD", "1500", 1);
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *src =
        "use std/enum\n"
        "keep = [10 20 30 40]\n"
        "defn loop n -> cond (eq? n 0) (sum keep) (do (map (fn x -> mul x x) [1 2 3 4 5]); (loop (sub n 1)) end)\n"
        "loop 200\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<gc>", src, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(!err.present);
    IshScheduler *sched = ish_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IshValue out = ish_nil();
    CHECK(ish_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 100);
    ish_sched_destroy(sched);
    ish_bc_destroy(&module);
    ish_core_free(core);
    unsetenv("ISH_GC_THRESHOLD");
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_heap_accounting_sweep(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    size_t baseline = ish_heap_bytes(&rt.heap);
    char *payload = malloc(10000u);
    CHECK(payload != NULL);
    memset(payload, 'x', 10000u);
    IshValue s = ish_string_n(&rt, payload, 10000u, &err);
    free(payload);
    CHECK(!err.present && s.tag == ISH_VAL_STRING);
    CHECK(ish_heap_bytes(&rt.heap) >= baseline + 10000u);
    ish_heap_sweep(&rt.heap);
    CHECK(ish_heap_bytes(&rt.heap) == baseline);
    CHECK(ish_heap_object_count(&rt.heap) == 0);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_gc_recurring_collection(void) {
    setenv("ISH_GC_THRESHOLD", "20000", 1);
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    const char *src =
        "defn big n s -> cond (eq? n 0) s (big (sub n 1) (str s s))\n"
        "defn loop k -> cond (eq? k 0) 0 (do (big 12 \"aaaaaaaaaaaaaaaa\"); (loop (sub k 1)) end)\n"
        "loop 80\n";
    CHECK(ish_expand_string(&rt, "<gc-recurring>", src, &core, &err));
    CHECK(!err.present);
    size_t baseline = ish_heap_bytes(&rt.heap);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshScheduler *sched = ish_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IshValue out = ish_nil();
    CHECK(ish_sched_run_main(sched, main_fn, &out, &err));
    CHECK(!err.present);
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 0);
    CHECK(ish_heap_bytes(&rt.heap) < baseline + 5u * 1024u * 1024u);
    ish_sched_destroy(sched);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
    unsetenv("ISH_GC_THRESHOLD");
}

static void test_port_result_gc_rooted(void) {
    setenv("ISH_GC_THRESHOLD", "1500", 1);
    IshRuntime rt;
    ish_runtime_init(&rt);
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
    ish_runtime_destroy(&rt);
    unsetenv("ISH_GC_THRESHOLD");
}

static void test_procsub_temp_cleanup(void) {
    size_t before = count_procsub_temps();
    IshRuntime rt;
    ish_runtime_init(&rt);
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
        "echo data > >(cat > /tmp/ish_d9_psw)\n"
        "y = cat /tmp/ish_d9_psw\n"
        "y\n",
        "{:ok 0 \"data\\n\" \"\"}");
    remove("/tmp/ish_d9_psw");
    ish_runtime_destroy(&rt);
    CHECK(count_procsub_temps() == before);
}

static void test_heredoc_leaves_no_files(void) {
    size_t before = count_glob_matches("/tmp/ish_heredoc_*");
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_sched_value_written(&rt,
        "implements std/shell\n"
        "x = cat <<E\n"
        "hi\n"
        "E\n"
        "x\n",
        "{:ok 0 \"hi\\n\" \"\"}");
    ish_runtime_destroy(&rt);
    CHECK(count_glob_matches("/tmp/ish_heredoc_*") == before);
}

static void test_temp_registry_backstop(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<backstop>", "make-procsub-temp\n", &core, &err));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshScheduler *sched = ish_sched_create(&rt, &module, &err);
    CHECK(sched != NULL);
    IshValue out = ish_nil();
    CHECK(ish_sched_run_main(sched, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_STRING);
    char path[256];
    size_t len = ish_string_length(out) < sizeof(path) - 1u ? ish_string_length(out) : sizeof(path) - 1u;
    memcpy(path, ish_string_bytes(out), len);
    path[len] = '\0';
    FILE *probe = fopen(path, "rb");
    CHECK(probe != NULL);
    if (probe) fclose(probe);
    ish_sched_destroy(sched);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
    probe = fopen(path, "rb");
    CHECK(probe == NULL);
    if (probe) fclose(probe);
}

static void test_actor_cwd_env_scoping(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
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
        "env-set \"ISH_F5_UNIT\" \"scoped\"\n"
        "env-get \"ISH_F5_UNIT\"\n",
        "\"scoped\"");
    CHECK(getenv("ISH_F5_UNIT") == NULL);
    ish_runtime_destroy(&rt);
}

static void test_port_capture_limit(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    setenv("ISH_CAPTURE_LIMIT", "1024", 1);
    run_port_expect(&rt, build_capture_graph(&rt, "printf", "hello", &err), "{:ok 0 \"hello\" \"\"}");
    setenv("ISH_CAPTURE_LIMIT", "8", 1);
    run_port_expect(&rt, build_capture_graph(&rt, "printf", "aaaaaaaaaaaaaaaaaaaa", &err), "{:error :capture-overflow \"aaaaaaaa\" \"\"}");
    unsetenv("ISH_CAPTURE_LIMIT");
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
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
