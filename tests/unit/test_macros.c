#include "test_util.h"

static void test_source_defmacro(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-expand-test>", "defmacro answer stx -> %'(add 40 2)\nanswer\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "((fn-multi add (/2..2 primitive add)") != NULL);
    CHECK(strstr(dump.data, "(prim ") == NULL);
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-expand-test>", "defmacro id %`(id %,x) -> x\nid 42\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-expand-test>", "defmacro plus2 %`(plus2 %,x) -> %`(add %,x 2)\nplus2 40\n", &core, &err));
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "((fn-multi add (/2..2 primitive add)") != NULL);
    CHECK(strstr(dump.data, "(prim ") == NULL);
    idm_buf_destroy(&dump);
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_macro_clauses(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *source =
        "defmacro plus2 do\n"
        "  %`(plus2 %,x) -> %`(add %,x 2)\n"
        "end\n"
        "plus2 40\n";

    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-clause-test>", source, &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "((fn-multi add (/2..2 primitive add)") != NULL);
    CHECK(strstr(dump.data, "(prim ") == NULL);
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *guarded_source =
        "defmacro word-only do\n"
        "  %`(word-only %,x) when (syntax-word? x) -> %`(42)\n"
        "  %`(word-only %,x) -> %`(0)\n"
        "end\n"
        "word-only hello\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-clause-guard-test>", guarded_source, &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *guard_fallback_source =
        "defmacro word-only do\n"
        "  %`(word-only %,x) when (syntax-word? x) -> %`(0)\n"
        "  %`(word-only %,x) -> %`(42)\n"
        "end\n"
        "word-only 10\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-clause-guard-fallback-test>", guard_fallback_source, &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *ellipsis_source =
        "defmacro do-it do\n"
        "  %`(do-it %,@body) -> %`(do %,@body end)\n"
        "end\n"
        "do-it 1 42\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<macro-clause-splice-test>", ellipsis_source, &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_standard_if(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<if-macro-test>", "if :false do 0 else do 42 end\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, " 42)") != NULL || strcmp(dump.data, "42") == 0);
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<if-macro-test>", "if :true do 42 end\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<if-macro-test>", "if :false do 42 end\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_NIL);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<if-macro-test>", "if :false do 0 else if :false do 1 else do 42 end\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(!idm_expand_source_string(&rt, "<if-macro-test>", "if :false 0 42\n", &core, &err));
    CHECK(err.present);
    idm_error_clear(&err);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_standard_control_macros(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
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
        IdmCore *core = NULL;
        CHECK(idm_expand_source_string(&rt, "<control-macro-test>", programs[i], &core, &err));
        IdmBytecodeModule module;
        idm_bc_init(&module);
        uint32_t main_fn = 0;
        CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
        IdmValue out = idm_nil();
        CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
        if (i == 3) CHECK(out.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(out.as.symbol), "false") == 0);
        else CHECK(out.tag == IDM_VAL_INT && out.as.i == expected[i]);
        CHECK(!err.present);
        idm_bc_destroy(&module);
        idm_core_free(core);
    }
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_macro_hygienic_introduction(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *source =
        "defmacro twice do\n"
        "  %`(twice %,expr) -> %`(do\n"
        "      tmp = %,expr\n"
        "      add tmp tmp\n"
        "    end)\n"
        "end\n"
        "tmp = 100\n"
        "twice 21\n"
        "tmp\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<hygiene-test>", source, &core, &err));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 100);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_macro_bind_bang_and_depth(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    const char *capture_source =
        "defmacro with-it do\n"
        "  %`(with-it %,value %,body) -> do\n"
        "      it = bind! value \"it\"\n"
        "      %`(do\n"
        "        %,it = %,value\n"
        "        %,body\n"
        "      end)\n"
        "  end\n"
        "end\n"
        "with-it 10 (add it 5)\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<bind-bang-test>", capture_source, &core, &err));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *use_site_source =
        "defmacro id %`(id %,x) -> %`(%,x)\n"
        "x = 42\n"
        "id x\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<use-site-test>", use_site_source, &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *loop_source =
        "defmacro loop stx -> %`(loop)\n"
        "loop\n";
    core = NULL;
    CHECK(!idm_expand_source_string(&rt, "<macro-depth-test>", loop_source, &core, &err));
    CHECK(err.present);
    idm_error_clear(&err);

    const char *local_expand_source =
        "defmacro plus2 %`(plus2 %,x) -> %`(add %,x 2)\n"
        "defmacro expand-plus2 stx -> local-expand %`(plus2 40)\n"
        "expand-plus2\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<local-expand-test>", local_expand_source, &core, &err));
    IdmBytecodeModule module2;
    idm_bc_init(&module2);
    uint32_t main_fn2 = 0;
    CHECK(idm_core_compile_main(core, &module2, &main_fn2, &err));
    CHECK(idm_bc_intern_literals(&rt, &module2, &err));
    CHECK(idm_vm_run(&rt, &module2, main_fn2, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module2);
    idm_core_free(core);

    idm_runtime_destroy(&rt);
}

static void test_macro_origin_and_properties(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *origin_source =
        "defmacro answer stx -> %`(42)\n"
        "defmacro origin-name stx -> make-syntax-string stx (first (syntax-origin (local-expand %`(answer))) )\n"
        "origin-name\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<origin-test>", origin_source, &core, &err));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data, "\"answer\"");
    idm_buf_destroy(&buf);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    const char *property_source =
        "defmacro prop stx -> make-syntax-string stx (syntax-property (syntax-set-property %`(x) \"k\" \"v\") \"k\")\n"
        "prop\n";
    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<property-test>", property_source, &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, out));
    CHECK_STR(buf.data, "\"v\"");
    idm_buf_destroy(&buf);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_for_syntax_macro_registration(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *source =
        "for-syntax do\n"
        "  defmacro answer stx -> %`(42)\n"
        "end\n"
        "answer\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<for-syntax-test>", source, &core, &err));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
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
        "  operator <+> precedence: 6 assoc: left capture: :infix -> add\n"
        "  3 <+> 4\n"
        "end\n",
        true);
    expect_expand_result("<surface-scope-leak-operator>",
        "do\n"
        "  operator <+> precedence: 6 assoc: left capture: :infix -> add\n"
        "end\n"
        "3 <+> 4\n",
        false);
    expect_expand_result("<surface-scope-ok-shell>",
        "do\n"
        "  use app/ish\n"
        "  activate Shell\n"
        "  echo inner\n"
        "end\n",
        true);
    expect_expand_result("<surface-scope-leak-shell>",
        "do\n"
        "  use app/ish\n"
        "  activate Shell\n"
        "  echo inner\n"
        "end\n"
        "echo outer\n",
        false);
}

static void test_import_compile_time_surface_boundaries(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "use tests/pkg/exporter\nactivate Exporter\nanswer anything\n", "99");
    check_value_written(&rt, "use tests/pkg/exporter\nactivate Exporter\n3 <+> 4\n", "7");
    check_value_written(&rt, "import tests/pkg/exporter as E\nactivate E.Exporter\nanswer anything\n", "99");
    idm_runtime_destroy(&rt);

    expect_expand_result("<import-macro-leak>",
        "import tests/pkg/exporter as E\n"
        "answer anything\n",
        false);
    expect_expand_result("<import-operator-leak>",
        "import tests/pkg/exporter as E\n"
        "3 <+> 4\n",
        false);
    expect_expand_result("<use-no-macro-surface>",
        "use tests/pkg/exporter\n"
        "answer anything\n",
        false);
    expect_expand_result("<use-no-operator-surface>",
        "use tests/pkg/exporter\n"
        "3 <+> 4\n",
        false);
    expect_expand_result("<use-installs-trait-method-surface>",
        "use tests/pkg/protomethod\n"
        "describe 1\n",
        true);
}

static void test_scoped_surface_shadowing(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
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
        "activate Outer\n"
        "a = do\n"
        "  activate Inner\n"
        "  go x\n"
        "end\n"
        "{a (go x)}\n",
        "{6 5}");
    idm_runtime_destroy(&rt);
}

static void test_scoped_surface_protocol_nesting(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_result_rt(&rt, "<inline-protocol-body-ok>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`42 end\n"
        "end\n"
        "do\n"
        "  activate Nest\n"
        "  answer foo\n"
        "end\n",
        true);
    expect_expand_error_rt(&rt, "<inline-protocol-activation-leak>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`42 end\n"
        "end\n"
        "do\n"
        "  activate Nest\n"
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
        "activate BodyLocal\n"
        "answer foo\n",
        "activate expects a protocol");
    expect_expand_error_rt(&rt, "<fn-body-implement-scoped>",
        "protocol Nest do\n"
        "  export defmacro answer y do %`7 end\n"
        "end\n"
        "f = fn z do\n"
        "  activate Nest\n"
        "  add z (answer foo)\n"
        "end\n"
        "answer foo\n",
        "unbound identifier 'answer'");
    idm_runtime_destroy(&rt);
}

static void test_scoped_surface_rollback_on_failure(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<rollback-install-then-fail>",
        "do\n"
        "  defmacro answer y do %`42 end\n"
        "  operator <+> precedence: 60 -> add\n"
        "  protocol RollP do\n"
        "    export defmacro roll y do %`9 end\n"
        "  end\n"
        "  activate RollP\n"
        "  roll check\n"
        "  this-name-is-unbound 1\n"
        "end\n",
        "unbound identifier 'this-name-is-unbound'");
    expect_expand_error_rt(&rt, "<rollback-macro-gone>", "answer now\n", "unbound identifier 'answer'");
    expect_expand_error_rt(&rt, "<rollback-operator-gone>", "3 <+> 4\n", "unbound identifier '<+>'");
    expect_expand_error_rt(&rt, "<rollback-protocol-gone>",
        "activate RollP\n"
        "roll now\n",
        "activate expects a protocol");
    expect_expand_result_rt(&rt, "<body-surface-ok>",
        "do\n"
        "  defmacro answer y do %`42 end\n"
        "  answer now\n"
        "end\n",
        true);
    expect_expand_error_rt(&rt, "<body-surface-not-persisted>", "answer now\n", "unbound identifier 'answer'");
    idm_runtime_destroy(&rt);
}

static void test_macro_phase_environment(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
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
        "activate p\n"
        "answer anything\n",
        "42");
    check_value_written(&rt,
        "use tests/pkg/macropriv\n"
        "activate MacroPriv\n"
        "phase-answer anything\n",
        "77");
    check_value_written(&rt,
        "import tests/pkg/macropriv as M\n"
        "activate M.MacroPriv\n"
        "phase-answer anything\n",
        "77");
    check_value_written(&rt,
        "use tests/pkg/macropriv\n"
        "activate MacroPriv\n"
        "inc-private 41\n",
        "42");
    expect_expand_result("<private-runtime-helper-hidden>",
        "use tests/pkg/macropriv\n"
        "hidden 41\n",
        false);
    idm_runtime_destroy(&rt);
}

static void test_macro_privacy_boundaries(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
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
    idm_runtime_destroy(&rt);
}

static void test_free_identifier_eq_uses_bindings(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt,
        "defmacro same-use-site %`(same-use-site %,a %,b) ->\n"
        "  cond (free-identifier=? a b) %`(1) %`(0)\n"
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
    idm_runtime_destroy(&rt);
}

static void test_splice_hygiene_negatives(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<splice-helper-hidden>",
        "defmacro m %`(m %,name) ->\n"
        "  %`(do\n"
        "      defn helper99 -> 1\n"
        "      defn %,name v -> v\n"
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
    expect_expand_error_rt(&rt, "<macro-surface-locality>",
        "answer x\n",
        "unbound identifier 'answer'");
    idm_runtime_destroy(&rt);
}

void run_macro_suite(void) {
    test_source_defmacro();
    test_source_macro_clauses();
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
