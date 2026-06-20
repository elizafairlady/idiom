#include "test_util.h"

static void test_macro_origin_chain(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
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
    idm_runtime_destroy(&rt);
}

static void test_transformer_failure_context(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<transformer-raise>",
        "defmacro boomer stx -> error :kaboom\n"
        "boomer x\n",
        "exited with reason error: :kaboom",
        "in expansion of 'boomer'");
    idm_runtime_destroy(&rt);
}

static void test_hygiene_hint(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<hygiene-hint>",
        "defmacro m stx -> %`(do\n"
        "    defn hidden-fn v -> v\n"
        "  end)\n"
        "m x\n"
        "hidden-fn 1\n",
        "unbound identifier 'hidden-fn'",
        "hygiene scopes differ");
    idm_runtime_destroy(&rt);
}

static void test_package_load_context(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<package-chain>",
        "use tests/pkg/nope-not-real\n"
        "1\n",
        "not found",
        "while loading package 'tests/pkg/nope-not-real'");
    idm_runtime_destroy(&rt);
}

static void test_for_syntax_context(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<phase-chain>",
        "for-syntax do\n"
        "  error :phase-boom\n"
        "end\n"
        "1\n",
        "exited with reason error: :phase-boom",
        "during for-syntax evaluation");
    idm_runtime_destroy(&rt);
}

static void test_phase_boundary_hint(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<phase-transformer-helper>",
        "defn helper x -> x\n"
        "defmacro m stx -> helper stx\n"
        "m 1\n",
        "unbound identifier 'helper'",
        "use for-syntax for transformer helpers");
    expect_expand_error_note_rt(&rt, "<phase-runtime-helper>",
        "for-syntax do\n"
        "  defn helper x -> x\n"
        "end\n"
        "helper 1\n",
        "unbound identifier 'helper'",
        "bound for-syntax (phase 1) but referenced at runtime");
    idm_runtime_destroy(&rt);
}

static void test_clause_failure_names_function(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_runtime_error_contains(&rt,
        "<clause-name>",
        "defn pick do\n"
        "  0 acc -> acc\n"
        "  [h . t] acc -> pick t (add acc h)\n"
        "end\n"
        "pick :wat 0\n",
        "no clause of 'pick' matches (:wat 0)");
    idm_runtime_destroy(&rt);
}

static void test_runtime_call_trace(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_runtime_error_note(&rt, "<call-trace>",
        "defn level3 x -> add x :not-a-number\n"
        "defn level2 x -> level3 x\n"
        "defn level1 x -> level2 x\n"
        "level1 1\n",
        "add expects numeric operands",
        "in level3");
    expect_runtime_error_note(&rt, "<call-trace-outer>",
        "defn level3 x -> add x :not-a-number\n"
        "defn level2 x -> add (level3 x) 1\n"
        "defn level1 x -> add (level2 x) 1\n"
        "level1 1\n",
        "add expects numeric operands",
        "in level1");
    idm_runtime_destroy(&rt);
}

static void test_expander_surface_introspection(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "defmacro probe stx do\n"
        "  defn count do\n"
        "    '() n -> n\n"
        "    (list _ . t) n -> count t (add n 1)\n"
        "  end\n"
        "  make-syntax-int stx (count (expander-surface :operators) 0)\n"
        "end\n"
        "gt? (probe x) 8\n",
        ":true");
    check_value_written(&rt,
        "use app/ish\n"
        "activate Shell\n"
        "defmacro probe2 stx do\n"
        "  defn count do\n"
        "    '() n -> n\n"
        "    (list _ . t) n -> count t (add n 1)\n"
        "  end\n"
        "  make-syntax-int stx (count (expander-surface :grammars) 0)\n"
        "end\n"
        "probe2 x\n",
        "1");
    expect_expand_error_rt(&rt, "<surface-bad-kind>",
        "defmacro bad stx -> make-syntax-int stx (expander-surface :nonsense)\n"
        "bad x\n",
        "expander-surface kind must be");
    idm_runtime_destroy(&rt);
}

static void test_surface_outside_expansion(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_runtime_error_contains(&rt, "<surface-runtime>",
        "expander-surface :operators\n",
        "requires an active expansion");
    idm_runtime_destroy(&rt);
}

static void test_minus_reader_diagnostics(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_note_rt(&rt, "<glued-minus-word>",
        "x-1\n",
        "unbound identifier 'x-1'",
        "was read as one word");
    expect_expand_error_note_rt(&rt, "<negative-literal-application>",
        "3 -1\n",
        "literal cannot be used as a function",
        "use spaces around '-' for subtraction");
    expect_expand_error_note_rt(&rt, "<glued-minus-idents>",
        "a = 1\nb = 2\na-b\n",
        "unbound identifier 'a-b'",
        "use spaces around '-' for subtraction");
    idm_runtime_destroy(&rt);
}

static void expect_bytecode_verify_error(const char *label, const uint32_t *words, size_t word_count, const char *expect_substring) {
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmError err;
    idm_error_init(&err);
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    for (size_t i = 0; i < word_count; i++) CHECK(idm_bc_emit(&module, words[i], NULL));
    CHECK(!idm_bc_verify(&module, &err));
    CHECK(err.present);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, expect_substring)) {
        fprintf(stderr, "FAIL %s: expected bytecode error containing \"%s\", got \"%s\"\n", label, expect_substring, err.message);
        failures++;
    }
    idm_error_clear(&err);
    idm_bc_destroy(&module);
}

static void test_namespace_transfer_diagnostics(void) {
    const uint32_t bad_direction[] = {
        (uint32_t)IDM_OP_TRANSFER_NAMESPACE, 99u, 1u, 0u, 1u,
        (uint32_t)IDM_OP_RETURN,
    };
    expect_bytecode_verify_error("<transfer-bad-direction>", bad_direction, sizeof(bad_direction) / sizeof(bad_direction[0]), "TRANSFER_NAMESPACE direction 99 is invalid");

    const uint32_t empty_transfer[] = {
        (uint32_t)IDM_OP_TRANSFER_NAMESPACE, (uint32_t)IDM_NS_TRANSFER_PARENT_TO_CHILD, 0u,
        (uint32_t)IDM_OP_RETURN,
    };
    expect_bytecode_verify_error("<transfer-empty>", empty_transfer, sizeof(empty_transfer) / sizeof(empty_transfer[0]), "TRANSFER_NAMESPACE requires at least one transfer");

    const uint32_t truncated_transfer[] = {
        (uint32_t)IDM_OP_TRANSFER_NAMESPACE, (uint32_t)IDM_NS_TRANSFER_PARENT_TO_CHILD, 1u, 0u,
    };
    expect_bytecode_verify_error("<transfer-truncated>", truncated_transfer, sizeof(truncated_transfer) / sizeof(truncated_transfer[0]), "TRANSFER_NAMESPACE missing operand");

    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmError err;
    idm_error_init(&err);
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_TRANSFER_NAMESPACE, (uint32_t)IDM_NS_TRANSFER_PARENT_TO_CHILD, NULL));
    CHECK(idm_bc_emit(&module, 1u, NULL));
    CHECK(idm_bc_emit(&module, 0u, NULL));
    CHECK(idm_bc_emit(&module, 0u, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    IdmValue out = idm_nil();
    CHECK(!idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    CHECK(err.message != NULL);
    if (err.message && !strstr(err.message, "TRANSFER_NAMESPACE with no active namespace")) {
        fprintf(stderr, "FAIL <transfer-no-namespace>: expected runtime error containing \"TRANSFER_NAMESPACE with no active namespace\", got \"%s\"\n", err.message);
        failures++;
    }
    idm_error_clear(&err);
    idm_bc_destroy(&module);
    idm_runtime_destroy(&rt);
}

void run_diagnostics_suite(void) {
    test_macro_origin_chain();
    test_transformer_failure_context();
    test_hygiene_hint();
    test_package_load_context();
    test_for_syntax_context();
    test_phase_boundary_hint();
    test_clause_failure_names_function();
    test_runtime_call_trace();
    test_expander_surface_introspection();
    test_surface_outside_expansion();
    test_minus_reader_diagnostics();
    test_namespace_transfer_diagnostics();
}
