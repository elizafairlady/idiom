#include "test_util.h"

static void test_source_defmacro(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<macro-expand-test>", "defmacro answer stx -> %'(add 40 2)\nanswer\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(app (prim add) 40 2)");
    ish_buf_destroy(&dump);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<macro-expand-test>", "defmacro id stx -> syntax-nth stx 2\nid 42\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<macro-expand-test>", "defmacro plus2 stx -> %`(add %,(syntax-nth stx 2) 2)\nplus2 40\n", &core, &err));
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(app (prim add) 40 2)");
    ish_buf_destroy(&dump);
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_syntax_case(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *source =
        "defmacro plus2 stx ->\n"
        "  syntax-case stx do\n"
        "    (_ x) -> %`(add %,x 2)\n"
        "  end\n"
        "plus2 40\n";

    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<syntax-case-test>", source, &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(app (prim add) 40 2)");
    ish_buf_destroy(&dump);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *guarded_source =
        "defmacro word-only stx ->\n"
        "  syntax-case stx do\n"
        "    (_ x) when (syntax-word? x) -> %`(42)\n"
        "    (_ x) -> %`(0)\n"
        "  end\n"
        "word-only hello\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<syntax-case-guard-test>", guarded_source, &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *guard_fallback_source =
        "defmacro word-only stx ->\n"
        "  syntax-case stx do\n"
        "    (_ x) when (syntax-word? x) -> %`(0)\n"
        "    (_ x) -> %`(42)\n"
        "  end\n"
        "word-only 10\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<syntax-case-guard-fallback-test>", guard_fallback_source, &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *ellipsis_source =
        "defmacro do-it stx ->\n"
        "  syntax-case stx do\n"
        "    (_ body ...) -> %`(do %,@body end)\n"
        "  end\n"
        "do-it 1 42\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<syntax-case-ellipsis-test>", ellipsis_source, &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_standard_if(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<if-macro-test>", "if :false do 0 else do 42 end\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(cond :false 0 42)");
    ish_buf_destroy(&dump);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<if-macro-test>", "if :true do 42 end\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<if-macro-test>", "if :false do 42 end\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_NIL);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<if-macro-test>", "if :false do 0 else if :false do 1 else do 42 end\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(!ish_expand_string(&rt, "<if-macro-test>", "if :false 0 42\n", &core, &err));
    CHECK(err.present);
    ish_error_clear(&err);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_standard_control_macros(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *programs[] = {
        "unless :false do 42 end\n",
        "case [1 2] do [a b] -> add a b end\n",
        "and :true 42\n",
        "and :false (add 1 \"bad\")\n",
        "or 42 (add 1 \"bad\")\n",
        "or :false 42\n",
    };
    int64_t expected[] = {42, 3, 42, 0, 42, 42};
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        IshCore *core = NULL;
        CHECK(ish_expand_string(&rt, "<control-macro-test>", programs[i], &core, &err));
        IshBytecodeModule module;
        ish_bc_init(&module);
        uint32_t main_fn = 0;
        CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
        IshValue out = ish_nil();
        CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
        if (i == 3) CHECK(out.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(out.as.symbol), "false") == 0);
        else CHECK(out.tag == ISH_VAL_INT && out.as.i == expected[i]);
        CHECK(!err.present);
        ish_bc_destroy(&module);
        ish_core_free(core);
    }
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_macro_hygienic_introduction(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *source =
        "defmacro twice stx ->\n"
        "  syntax-case stx do\n"
        "    (_ expr) -> %`(do\n"
        "      tmp = %,expr\n"
        "      add tmp tmp\n"
        "    end)\n"
        "  end\n"
        "tmp = 100\n"
        "twice 21\n"
        "tmp\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<hygiene-test>", source, &core, &err));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 100);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_macro_bind_bang_and_depth(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);

    const char *capture_source =
        "defmacro with-it stx ->\n"
        "  syntax-case stx do\n"
        "    (_ value body) -> do\n"
        "      it = bind! stx \"it\"\n"
        "      %`(do\n"
        "        %,it = %,value\n"
        "        %,body\n"
        "      end)\n"
        "    end\n"
        "  end\n"
        "with-it 10 (add it 5)\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<bind-bang-test>", capture_source, &core, &err));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *use_site_source =
        "defmacro id stx -> %`(%,(syntax-nth stx 2))\n"
        "x = 42\n"
        "id x\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<use-site-test>", use_site_source, &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *loop_source =
        "defmacro loop stx -> %`(loop)\n"
        "loop\n";
    core = NULL;
    CHECK(!ish_expand_string(&rt, "<macro-depth-test>", loop_source, &core, &err));
    CHECK(err.present);
    ish_error_clear(&err);

    const char *local_expand_source =
        "defmacro plus2 stx -> %`(add %,(syntax-nth stx 2) 2)\n"
        "defmacro expand-plus2 stx -> local-expand %`(plus2 40)\n"
        "expand-plus2\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<local-expand-test>", local_expand_source, &core, &err));
    IshBytecodeModule module2;
    ish_bc_init(&module2);
    uint32_t main_fn2 = 0;
    CHECK(ish_core_compile_main(core, &module2, &main_fn2, &err));
    CHECK(ish_vm_run(&rt, &module2, main_fn2, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module2);
    ish_core_free(core);

    ish_runtime_destroy(&rt);
}

static void test_macro_origin_and_properties(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *origin_source =
        "defmacro answer stx -> %`(42)\n"
        "defmacro origin-name stx -> datum->syntax stx (first (syntax-origin (local-expand %`(answer))) )\n"
        "origin-name\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<origin-test>", origin_source, &core, &err));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, out));
    CHECK_STR(buf.data, "\"answer\"");
    ish_buf_destroy(&buf);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    const char *property_source =
        "defmacro prop stx -> datum->syntax stx (syntax-property (syntax-set-property %`(x) \"k\" \"v\") \"k\")\n"
        "prop\n";
    core = NULL;
    CHECK(ish_expand_string(&rt, "<property-test>", property_source, &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, out));
    CHECK_STR(buf.data, "\"v\"");
    ish_buf_destroy(&buf);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_for_syntax_macro_registration(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *source =
        "for-syntax do\n"
        "  defmacro answer stx -> %`(42)\n"
        "end\n"
        "answer\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<for-syntax-test>", source, &core, &err));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_compile_time_surface_scoping(void) {
    expect_expand_result("<surface-scope-ok-macro>",
        "do\n"
        "  defmacro answer s -> (make-syntax-int s 42)\n"
        "  answer now\n"
        "end\n",
        true);
    expect_expand_result("<surface-scope-leak-macro>",
        "do\n"
        "  defmacro answer s -> (make-syntax-int s 42)\n"
        "end\n"
        "answer now\n",
        false);
    expect_expand_result("<surface-scope-ok-operator>",
        "do\n"
        "  operator <+> precedence: 6 assoc: left fixity: infix -> add\n"
        "  3 <+> 4\n"
        "end\n",
        true);
    expect_expand_result("<surface-scope-leak-operator>",
        "do\n"
        "  operator <+> precedence: 6 assoc: left fixity: infix -> add\n"
        "end\n"
        "3 <+> 4\n",
        false);
    expect_expand_result("<surface-scope-ok-shell>",
        "do\n"
        "  implements std/shell\n"
        "  echo inner\n"
        "end\n",
        true);
    expect_expand_result("<surface-scope-leak-shell>",
        "do\n"
        "  implements std/shell\n"
        "  echo inner\n"
        "end\n"
        "echo outer\n",
        false);
}

static void test_import_compile_time_surface_boundaries(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt, "use tests/pkg/exporter\nanswer anything\n", "99");
    check_value_written(&rt, "use tests/pkg/exporter\n3 <+> 4\n", "7");
    check_value_written(&rt, "import tests/pkg/exporter as E\nE.answer anything\n", "99");
    ish_runtime_destroy(&rt);

    expect_expand_result("<import-macro-leak>",
        "import tests/pkg/exporter as E\n"
        "answer anything\n",
        false);
    expect_expand_result("<import-operator-leak>",
        "import tests/pkg/exporter as E\n"
        "3 <+> 4\n",
        false);
}

static void test_scoped_surface_shadowing(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt,
        "defmacro answer y do %`1 end\n"
        "inner = do\n"
        "  defmacro answer y do %`2 end\n"
        "  answer foo\n"
        "end\n"
        "outer = answer foo\n"
        "{inner outer}\n",
        "{2 1}");
    check_value_written(&rt,
        "operator <+> precedence: 60 -> add\n"
        "inner = do\n"
        "  operator <+> precedence: 60 -> mul\n"
        "  3 <+> 4\n"
        "end\n"
        "outer = 3 <+> 4\n"
        "{inner outer}\n",
        "{12 7}");
    check_value_written(&rt,
        "protocol Outer do\n"
        "  export defmacro go y do %`5 end\n"
        "end\n"
        "protocol Inner do\n"
        "  export defmacro go y do %`6 end\n"
        "end\n"
        "implements Outer\n"
        "a = do\n"
        "  implements Inner\n"
        "  go x\n"
        "end\n"
        "{a (go x)}\n",
        "{6 5}");
    ish_runtime_destroy(&rt);
}

static void test_scoped_surface_protocol_nesting(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_result_rt(&rt, "<inline-protocol-body-ok>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`42 end\n"
        "end\n"
        "do\n"
        "  implements Nest\n"
        "  answer foo\n"
        "end\n",
        true);
    expect_expand_error_rt(&rt, "<inline-protocol-activation-leak>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`42 end\n"
        "end\n"
        "do\n"
        "  implements Nest\n"
        "  answer foo\n"
        "end\n"
        "answer foo\n",
        "unbound identifier 'answer'");
    expect_expand_error_rt(&rt, "<inline-protocol-decl-scoped>",
        "do\n"
        "  protocol BodyLocal do\n"
        "    export defmacro answer y do %`9 end\n"
        "  end\n"
        "  0\n"
        "end\n"
        "implements BodyLocal\n"
        "answer foo\n",
        "open failed");
    expect_expand_error_rt(&rt, "<fn-body-implements-scoped>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`7 end\n"
        "end\n"
        "f = fn z do\n"
        "  implements Nest\n"
        "  add z (answer foo)\n"
        "end\n"
        "answer foo\n",
        "unbound identifier 'answer'");
    ish_runtime_destroy(&rt);
}

static void test_scoped_surface_rollback_on_failure(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<rollback-install-then-fail>",
        "do\n"
        "  defmacro answer y do %`42 end\n"
        "  operator <+> precedence: 60 -> add\n"
        "  protocol RollP do\n"
        "    export defmacro roll y do %`9 end\n"
        "  end\n"
        "  implements RollP\n"
        "  roll check\n"
        "  this-name-is-unbound 1\n"
        "end\n",
        "unbound identifier 'this-name-is-unbound'");
    expect_expand_error_rt(&rt, "<rollback-macro-gone>", "answer now\n", "unbound identifier 'answer'");
    expect_expand_error_rt(&rt, "<rollback-operator-gone>", "3 <+> 4\n", "unbound identifier '<+>'");
    expect_expand_error_rt(&rt, "<rollback-protocol-gone>",
        "implements RollP\n"
        "roll now\n",
        "open failed");
    expect_expand_result_rt(&rt, "<body-surface-ok>",
        "do\n"
        "  defmacro answer y do %`42 end\n"
        "  answer now\n"
        "end\n",
        true);
    expect_expand_error_rt(&rt, "<body-surface-not-persisted>", "answer now\n", "unbound identifier 'answer'");
    ish_runtime_destroy(&rt);
}

static void test_macro_phase_environment(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt,
        "for-syntax do\n"
        "  defn lit stx -> make-syntax-int stx 42\n"
        "  defmacro answer stx -> lit stx\n"
        "end\n"
        "answer anything\n",
        "42");
    check_value_written(&rt,
        "protocol p do\n"
        "  for-syntax do\n"
        "    defn lit stx -> make-syntax-int stx 42\n"
        "    export defmacro answer stx -> lit stx\n"
        "  end\n"
        "end\n"
        "implements p\n"
        "answer anything\n",
        "42");
    check_value_written(&rt,
        "use tests/pkg/macropriv\n"
        "phase-answer anything\n",
        "77");
    check_value_written(&rt,
        "import tests/pkg/macropriv as M\n"
        "M.phase-answer anything\n",
        "77");
    check_value_written(&rt,
        "use tests/pkg/macropriv\n"
        "inc-private 41\n",
        "42");
    expect_expand_result("<private-runtime-helper-hidden>",
        "use tests/pkg/macropriv\n"
        "hidden 41\n",
        false);
    ish_runtime_destroy(&rt);
}

static void test_macro_privacy_boundaries(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<phase-helper-hidden-runtime>",
        "use tests/pkg/macropriv\n"
        "lit 41\n",
        "unbound identifier 'lit'");
    expect_expand_error_rt(&rt, "<phase-helper-hidden-phase>",
        "use tests/pkg/macropriv\n"
        "defmacro probe stx -> lit stx\n"
        "probe x\n",
        "unbound identifier 'lit'");
    expect_expand_error_rt(&rt, "<import-phase-macro-not-unqualified>",
        "import tests/pkg/macropriv as M\n"
        "phase-answer anything\n",
        "unbound identifier 'phase-answer'");
    ish_runtime_destroy(&rt);
}

static void test_free_identifier_eq_uses_bindings(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    check_value_written(&rt,
        "defmacro same-use-site stx ->\n"
        "  cond (free-identifier=? (syntax-nth stx 2) (syntax-nth stx 3)) %`(1) %`(0)\n"
        "x = 1\n"
        "same-use-site x x\n",
        "1");
    check_value_written(&rt,
        "defmacro intro-diff stx -> do\n"
        "  intro = make-syntax-word stx \"definitely-free-id\"\n"
        "  caller = bind! stx \"definitely-free-id\"\n"
        "  cond (free-identifier=? intro caller) %`(1) %`(0)\n"
        "end\n"
        "intro-diff\n",
        "0");
    ish_runtime_destroy(&rt);
}

static void test_splice_hygiene_negatives(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<splice-helper-hidden>",
        "defmacro m stx ->\n"
        "  %`(do\n"
        "      defn helper99 -> 1\n"
        "      defn %,(syntax-nth stx 2) v -> v\n"
        "    end)\n"
        "m pub\n"
        "helper99\n",
        "unbound identifier 'helper99'");
    expect_expand_error_rt(&rt, "<splice-template-defn-hidden>",
        "defmacro m stx ->\n"
        "  %`(do\n"
        "      defn tprobe v -> v\n"
        "    end)\n"
        "m anything\n"
        "tprobe 1\n",
        "unbound identifier 'tprobe'");
    expect_expand_error_rt(&rt, "<register-macro-locality>",
        "answer x\n",
        "unbound identifier 'answer'");
    ish_runtime_destroy(&rt);
}

void run_macro_suite(void) {
    test_source_defmacro();
    test_source_syntax_case();
    test_source_standard_if();
    test_source_standard_control_macros();
    test_macro_hygienic_introduction();
    test_macro_bind_bang_and_depth();
    test_macro_origin_and_properties();
    test_for_syntax_macro_registration();
    test_compile_time_surface_scoping();
    test_import_compile_time_surface_boundaries();
    test_scoped_surface_shadowing();
    test_scoped_surface_protocol_nesting();
    test_scoped_surface_rollback_on_failure();
    test_macro_phase_environment();
    test_macro_privacy_boundaries();
    test_free_identifier_eq_uses_bindings();
    test_splice_hygiene_negatives();
}
