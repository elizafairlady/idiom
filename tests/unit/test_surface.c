#include "test_util.h"

static void test_source_operator_surface(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_operator_eval(&rt, "84 / 2\n", NULL, 42);
    check_operator_eval(&rt, "142 % 100\n", NULL, 42);
    check_operator_eval(&rt, "2 ** 5 + 10\n", NULL, 42);
    check_operator_eval(&rt, "2 ** 3 ** 2\n", NULL, 512);
    check_operator_eval(&rt, "50 + -8\n", NULL, 42);
    check_operator_eval(&rt, "50 + - 8\n", NULL, 42);
    check_operator_eval(&rt, "x = -5\nx + 47\n", NULL, 42);
    check_operator_eval(&rt, "if (3 != 4) do 42 else do 0 end\n", NULL, 42);
    check_operator_eval(&rt, "if (3 != 3) do 0 else do 42 end\n", NULL, 42);
    check_operator_eval(&rt, "defn combine a b -> add (mul a 10) b\noperator glue precedence: 50 assoc: left capture: :infix -> combine\n4 glue 2\n", NULL, 42);
    check_operator_eval(&rt, "operator negof precedence: 80 capture: :prefix do\n  x -> sub 0 x\nend\nadd 50 (negof 8)\n", NULL, 42);
    check_value_written(&rt, "operator <++> precedence: 50 assoc: left capture: :infix do\n  a b -> add (mul a 10) b\nend\n4 <++> 2\n", "42");
    check_value_written(&rt, "operator @ precedence: 50 assoc: left capture: :infix -> add\n40 @ 2\n", "42");
    check_value_written(&rt, "operator grab capture: {:prefix :count 2} do\n  xs -> syntax-length xs\nend\ngrab alpha beta\n", "2");
    check_value_written(&rt, "operator blocky capture: :indented do\n  b -> syntax-length b\nend\nblocky do\n  alpha\n  beta\nend\n", "3");
    check_value_written(&rt, "operator hered capture: :sentinel do\n  sent xs -> syntax-length xs\nend\nhered END alpha beta END\n", "2");
    check_value_written(&rt, "operator << precedence: 20 assoc: left capture: {:infix :sentinel} do\n  left sent xs -> add left (syntax-length xs)\nend\n38 + 2 << END alpha beta END\n", "42");
    check_value_written(&rt, "operator take precedence: 20 assoc: left capture: {:infix :count 2} do\n  left xs -> add left (syntax-length xs)\nend\n40 take alpha beta\n", "42");
    check_value_written(&rt, "operator mop precedence: 20 assoc: left capture: {:infix :expression} macro do\n  %`(_ %,left %,right) -> %`(add %,left 2)\nend\n40 mop ignored\n", "42");
    check_value_written(&rt, "operator imop precedence: 20 assoc: left capture: {:infix :expression} -> macro do\n  %`(_ %,left %,right) -> %`(add %,left 2)\nend\n40 imop ignored\n", "42");
    check_value_written(&rt, "operator ibop precedence: 20 assoc: left capture: {:infix :expression} macro %`(_ %,left %,right) -> %`(add %,left 2)\n40 ibop ignored\n", "42");
    check_operator_eval(&rt, "(fn x -> add x 1) 41\n", NULL, 42);
    check_operator_eval(&rt, "f = fn a -> (fn b -> (fn c -> add a (add b c)))\n((f 100) 20) 3\n", NULL, 123);
    check_operator_eval(&rt, "operator <+> precedence: 50 assoc: left capture: :infix do\n  a b -> add (mul a 10) b\nend\n1 <+> 2 <+> 3\n", NULL, 123);
    check_operator_eval(&rt, "defn ev n -> cond (n == 0) 1 (od (sub n 1))\ndefn od n -> cond (n == 0) 0 (ev (sub n 1))\nadd (ev 10) 41\n", NULL, 42);
    idm_runtime_destroy(&rt);
}

static void test_source_quote(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "'foo\n", "foo");
    check_value_written(&rt, "':ok\n", ":ok");
    check_value_written(&rt, "'(a b c)\n", "(a b c)");
    check_value_written(&rt, "'(a (b c))\n", "(a (b c))");
    check_value_written(&rt, "'()\n", "()");
    check_value_written(&rt, "'[1 2]\n", "[1 2]");
    check_value_written(&rt, "x = 5\n`(a ,x c)\n", "(a 5 c)");
    check_value_written(&rt, "xs = '(1 2 3)\n`(a ,@xs z)\n", "(a 1 2 3 z)");
    check_value_written(&rt, "x = 9\n`[1 ,x 3]\n", "[1 9 3]");
    idm_runtime_destroy(&rt);
}

static void test_source_interpolation(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "name = \"world\"\n\"hello ${name}\"\n", "\"hello world\"");
    check_value_written(&rt, "x = 20\n\"sum #{x + 22}\"\n", "\"sum 42\"");
    check_value_written(&rt, "a = 3\nb = 4\n\"#{a}+#{b}=#{a + b}\"\n", "\"3+4=7\"");
    check_value_written(&rt, "\"lit \\$ \\#\"\n", "\"lit $ #\"");
    check_value_written(&rt, "\"no interpolation\"\n", "\"no interpolation\"");
    idm_runtime_destroy(&rt);
}

static void test_source_rescue(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "try do raise :boom\n rescue r -> r end\n", ":boom");
    check_value_written(&rt, "try do add 40 2\n rescue r -> 0 end\n", "42");
    check_value_written(&rt, "try do raise 5\n rescue r -> add r 37 end\n", "42");
    check_value_written(&rt, "try do div 1 0\n rescue r -> 99 end\n", "99");
    check_value_written(&rt, "try do 42\n ensure 1 end\n", "42");
    check_value_written(&rt, "try do try do raise 9\n ensure 1 end\n rescue r -> r end\n", "9");
    check_value_written(&rt, "try do raise 7\n rescue r -> r\n ensure 1 end\n", "7");
    idm_runtime_destroy(&rt);
}

static void test_source_operator(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "1 + 2 * 3\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "((fn-multi add (/2..2 primitive add)") != NULL);
    CHECK(strstr(dump.data, "((fn-multi mul (/2..2 primitive mul)") != NULL);
    CHECK(strstr(dump.data, "(prim ") == NULL);
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 7);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "if (2 <= 2) do 42 else do 0 end\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "if (2 >= 2) do 42 else do 0 end\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "add (div 80 2) (mod 5 3)\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "add (pow 6 2) 6\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(!idm_expand_source_string(&rt, "<operator-expand-test>", "add 1 nope\n", &core, &err));
    CHECK(err.present);
    idm_error_clear(&err);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<operator-expand-test>", "x = 2\nx * 20 + 2\n", &core, &err));
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

static void test_source_fn(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<fn-expand-test>", "inc = fn x -> add x 1\ninc 41\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "(bind-local inc#0") != NULL);
    CHECK(strstr(dump.data, "(fn <lambda>/1") != NULL);
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
    CHECK(idm_expand_source_string(&rt, "<fn-expand-test>", "inc = fn x do add x 1 end\ninc 41\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<fn-expand-test>", "x = 1\nf = fn y -> add x y\nf 2\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 3);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<fn-expand-test>", "f = fn x x -> x\nf 3 3\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 3);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<fn-expand-test>", "f = fn x x -> x\nf 3 4\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(!idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_defn_letrec(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(!idm_expand_source_string(&rt, "<definition-expand-test>", "def inc x -> add x 1\ninc 41\n", &core, &err));
    CHECK(err.present);
    CHECK(core == NULL);
    idm_error_clear(&err);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<definition-expand-test>", "defn inc x -> x + 1\ninc 41\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "(letrec") != NULL);
    CHECK(strstr(dump.data, "(fn inc/1") != NULL);
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
    CHECK(idm_expand_source_string(&rt, "<definition-expand-test>", "defn f x -> g x\ndefn g x -> x + 1\nf 41\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<definition-expand-test>", "defn f 0 -> 40\ndefn f n -> n\nadd (f 0) 2\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<definition-expand-test>", "defn f 0 -> 0\ndefn f n -> n\nf 7\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 7);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<definition-expand-test>", "defn sumdown n -> cond (n < 1) 0 (n + sumdown (n - 1))\nsumdown 5\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_runtime_destroy(&rt);
}

static void test_source_match(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match 0 do\n  0 -> 42\n  n -> n\nend\n", &core, &err));
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
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match 7 do\n  0 -> 0\n  n -> n\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 7);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "x = 40\nmatch 2 do\n  n -> x + n\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2] do\n  [1 2] -> 42\n  _ -> 0\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2] do\n  [1 2] -> 42\n  _ -> 0\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2 3] do\n  [h . t] -> t\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    IdmBuffer vec_buf;
    idm_buf_init(&vec_buf);
    CHECK(idm_value_write(&vec_buf, out));
    CHECK_STR(vec_buf.data, "[2 3]");
    idm_buf_destroy(&vec_buf);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match {:ok 42} do\n  {:ok 42} -> 42\n  _ -> 0\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match {1 2 3} do\n  {h . t} -> t\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    IdmBuffer tuple_buf;
    idm_buf_init(&tuple_buf);
    CHECK(idm_value_write(&tuple_buf, out));
    CHECK_STR(tuple_buf.data, "{2 3}");
    idm_buf_destroy(&tuple_buf);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2] do\n  [h t] -> h\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 1);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2 3] do\n  [h . t] -> h\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 1);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match [1 2 3] do\n  [h x . t] -> x\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 2);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "defn head [h . t] -> h\nhead [42 0]\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match %{:a 42 :b 0} do\n  %{:a 42} -> 42\n  _ -> 0\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match %{:a 42} do\n  %{:a x} -> x\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match {:ok \"no\"} do\n  {:ok x} when x < 0 -> 1\n  {:ok x} -> 42\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "defn f n when n < 0 -> 0\ndefn f n -> n\nf 42\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "defn f n when n == 42 -> 42\ndefn f n -> 0\nf 42\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "f = fn x when x == 42 -> x\nf 42\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(core);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<match-expand-test>", "match 1 do\n  0 -> 0\nend\n", &core, &err));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(!idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    idm_error_clear(&err);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_runtime_destroy(&rt);
}

static void test_env_prefix_boundaries(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    check_value_written(&rt, "y=2\nadd y 40\n", "42");
    check_value_written(&rt, "FOO = add 1 2\nFOO\n", "3");
    expect_expand_error_rt(&rt, "<env-prefix-needs-shell>",
        "FOO=bar printenv FOO\n",
        "unbound identifier 'FOO'");
    idm_runtime_destroy(&rt);
}

static void test_operator_macro_negatives(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    expect_expand_error_rt(&rt, "<op-bad-precedence>", "operator bad precedence: 999 -> add\n", "operator precedence must be an integer 0..255");
    expect_expand_error_rt(&rt, "<op-bad-assoc>", "operator bad assoc: sideways -> add\n", "operator assoc must be left, right, or none");
    expect_expand_error_rt(&rt, "<op-bad-keyword>", "operator bad bogus: 1 -> add\n", "unknown operator keyword 'bogus'");
    expect_expand_error_rt(&rt, "<op-word-capture>", "operator bad capture: prefix -> add\n", "operator capture must use atoms");
    expect_expand_error_rt(&rt, "<op-macro-value-capture>", "operator bad capture: :prefix -> macro do\n  %`(_ %,x) -> %`1\nend\nbad 1\n", "use a syntax capture");
    expect_expand_error_rt(&rt, "<op-too-short>", "operator alone\n", "operator declaration requires a name and a target");
    expect_expand_error_rt(&rt, "<op-no-arrow>", "operator noarrow precedence: 9\n", "operator declaration requires '-> target'");
    check_value_written(&rt, "defn combine a b -> add (mul a 10) b\noperator <%> precedence: 50 assoc: left capture: :infix -> combine\n1 <%> 2 <%> 3\n", "123");
    idm_runtime_destroy(&rt);
}

void run_surface_suite(void) {
    test_source_operator_surface();
    test_source_quote();
    test_source_interpolation();
    test_source_rescue();
    test_source_operator();
    test_source_fn();
    test_source_defn_letrec();
    test_source_match();
    test_env_prefix_boundaries();
    test_operator_macro_negatives();
}
