#include "test_util.h"

static void test_macro_origin_chain(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<origin-chain>",
        "defmacro outer stx -> %`(inner 1)\n"
        "defmacro inner stx -> %`(no-such-fn 1)\n"
        "outer x\n",
        "unbound identifier 'no-such-fn'",
        "in expansion of 'inner'");
    expect_expand_error_note_rt(&rt, "<origin-chain-outer>",
        "defmacro outer stx -> %`(inner 1)\n"
        "defmacro inner stx -> %`(no-such-fn 1)\n"
        "outer x\n",
        "unbound identifier 'no-such-fn'",
        "in expansion of 'outer'");
    ish_runtime_destroy(&rt);
}

static void test_transformer_failure_context(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<transformer-raise>",
        "defmacro boomer stx -> error :kaboom\n"
        "boomer x\n",
        "exited with reason {:error :kaboom}",
        "in expansion of 'boomer'");
    ish_runtime_destroy(&rt);
}

static void test_hygiene_hint(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<hygiene-hint>",
        "defmacro m stx -> %`(do\n"
        "    defn hidden-fn v -> v\n"
        "  end)\n"
        "m x\n"
        "hidden-fn 1\n",
        "unbound identifier 'hidden-fn'",
        "hygiene scopes differ");
    ish_runtime_destroy(&rt);
}

static void test_package_load_context(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<package-chain>",
        "use tests/pkg/nope-not-real\n"
        "1\n",
        "open failed",
        "while loading package 'tests/pkg/nope-not-real'");
    ish_runtime_destroy(&rt);
}

static void test_for_syntax_context(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<phase-chain>",
        "for-syntax do\n"
        "  error :phase-boom\n"
        "end\n"
        "1\n",
        "exited with reason {:error :phase-boom}",
        "during for-syntax evaluation");
    ish_runtime_destroy(&rt);
}

static void test_clause_failure_names_function(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_runtime_error_contains(&rt,
        "<clause-name>",
        "defn pick do\n"
        "  0 acc -> acc\n"
        "  [h . t] acc -> pick t (add acc h)\n"
        "end\n"
        "pick :wat 0\n",
        "no clause of 'pick' matches (:wat 0)");
    ish_runtime_destroy(&rt);
}

static void test_runtime_call_trace(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_runtime_error_note(&rt, "<call-trace>",
        "defn level3 x -> add x :not-a-number\n"
        "defn level2 x -> level3 x\n"
        "defn level1 x -> level2 x\n"
        "level1 1\n",
        "ADD expects numeric operands",
        "in level3");
    expect_runtime_error_note(&rt, "<call-trace-outer>",
        "defn level3 x -> add x :not-a-number\n"
        "defn level2 x -> level3 x\n"
        "defn level1 x -> level2 x\n"
        "level1 1\n",
        "ADD expects numeric operands",
        "in level1");
    ish_runtime_destroy(&rt);
}

static void test_expander_surface_introspection(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt,
        "defmacro probe stx do\n"
        "  defn count do\n"
        "    :nil n -> n\n"
        "    [_ . t] n -> count t (add n 1)\n"
        "  end\n"
        "  make-syntax-int stx (count (expander-surface :operators) 0)\n"
        "end\n"
        "gt? (probe x) 8\n",
        ":true");
    check_value_written(&rt,
        "implements std/shell\n"
        "defmacro probe2 stx do\n"
        "  defn count do\n"
        "    :nil n -> n\n"
        "    [_ . t] n -> count t (add n 1)\n"
        "  end\n"
        "  make-syntax-int stx (count (expander-surface :resolvers) 0)\n"
        "end\n"
        "probe2 x\n",
        "1");
    expect_expand_error_rt(&rt, "<surface-bad-kind>",
        "defmacro bad stx -> make-syntax-int stx (expander-surface :nonsense)\n"
        "bad x\n",
        "expander-surface kind must be");
    ish_runtime_destroy(&rt);
}

static void test_surface_outside_expansion(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_runtime_error_contains(&rt, "<surface-runtime>",
        "expander-surface :operators\n",
        "requires an active expansion");
    ish_runtime_destroy(&rt);
}

void run_diagnostics_suite(void) {
    test_macro_origin_chain();
    test_transformer_failure_context();
    test_hygiene_hint();
    test_package_load_context();
    test_for_syntax_context();
    test_clause_failure_names_function();
    test_runtime_call_trace();
    test_expander_surface_introspection();
    test_surface_outside_expansion();
}
