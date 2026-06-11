#include "test_util.h"

static void test_values(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshValue a1 = ish_atom(&rt, "ok");
    IshValue a2 = ish_atom(&rt, "ok");
    IshValue w1 = ish_word(&rt, "ok");
    CHECK(ish_value_equal(a1, a2));
    CHECK(!ish_value_equal(a1, w1));
    IshValue s1 = ish_string(&rt, "hello", &err);
    IshValue s2 = ish_string(&rt, "hello", &err);
    CHECK(ish_value_equal(s1, s2));
    IshValue list = ish_cons(&rt, ish_int(1), ish_cons(&rt, ish_int(2), ish_nil(), &err), &err);
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_value_write(&buf, list));
    CHECK_STR(buf.data, "(1 2)");
    ish_buf_destroy(&buf);
    CHECK(ish_heap_object_count(&rt.heap) == 4u);
    IshValue roots[1] = { list };
    ish_heap_collect(&rt.heap, roots, 1u);
    CHECK(ish_heap_object_count(&rt.heap) == 2u);
    IshValue car = ish_car(list, &err);
    CHECK(car.tag == ISH_VAL_INT && car.as.i == 1);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_reader_basic(void) {
    char *s = dump_reader("foo bar\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr foo bar))");
    free(s);

    s = dump_reader("foo (bar baz)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr foo (%-group (%-expr bar baz))))");
    free(s);

    s = dump_reader("[1 2 :ok] %[a b] {:ok 1}\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr [1 2 :ok] %[a b] {:ok 1}))");
    free(s);

    s = dump_reader("i > count\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr i > count))");
    free(s);
}

static void test_reader_shell_protocols(void) {
    char *s = dump_reader("grep -c *.c > out.txt\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr grep (%-word \"-c\") (%-word \"*.c\") > out . txt))");
    free(s);

    s = dump_reader("wc $n - 1\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr wc (%-shell-var n) - 1))");
    free(s);
}

static void test_reader_quote(void) {
    char *s = dump_reader("'(foo bar) %`(add %,x 1)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr (%-quote (%-group (%-expr foo bar))) (%-quasisyntax (%-group (%-expr add (%-unsyntax x) 1)))))");
    free(s);
}

static void test_reader_body_and_function_value(void) {
    char *s = dump_reader("match value do\n  {:ok x} -> send &handler x\nend\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr match value (%-body (%-expr {:ok x} -> send (%-expression &handler) x))))");
    free(s);
}

static void test_bytecode_verifier(void) {
    IshBytecodeModule module;
    ish_bc_init(&module);
    IshError err;
    ish_error_init(&err);
    uint32_t main_fn = 0;
    CHECK(ish_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, 99, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(!ish_bc_verify(&module, &err));
    CHECK(err.present);
    ish_error_clear(&err);
    ish_bc_destroy(&module);
}

static void test_vm_call_and_locals(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t add2 = 0;
    uint32_t main_fn = 0;
    CHECK(ish_bc_add_function(&module, "add2", 2, 0, 0, &add2));
    CHECK(ish_bc_add_function(&module, "main", 0, 1, 0, &main_fn));
    CHECK(ish_bc_set_function_entry(&module, add2, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_ARG, 0, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_ARG, 1, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_ADD, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(ish_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c20 = 0;
    uint32_t c22 = 0;
    uint32_t c1 = 0;
    CHECK(ish_bc_add_const(&module, ish_int(20), &c20));
    CHECK(ish_bc_add_const(&module, ish_int(22), &c22));
    CHECK(ish_bc_add_const(&module, ish_int(1), &c1));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_MAKE_CLOSURE, add2, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c20, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c22, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_CALL, 2, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_STORE_LOCAL, 0, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_LOCAL, 0, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c1, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_SUB, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 41);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_vm_tail_call_reuses_frame(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t id_fn = 0;
    uint32_t main_fn = 0;
    CHECK(ish_bc_add_function(&module, "id", 1, 0, 0, &id_fn));
    CHECK(ish_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(ish_bc_set_function_entry(&module, id_fn, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_ARG, 0, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(ish_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c99 = 0;
    CHECK(ish_bc_add_const(&module, ish_int(99), &c99));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_MAKE_CLOSURE, id_fn, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c99, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_TAIL_CALL, 1, NULL));
    IshVmLimits limits = ish_vm_default_limits();
    limits.max_frames = 1;
    IshValue out = ish_nil();
    CHECK(ish_vm_run_limited(&rt, &module, main_fn, limits, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 99);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_vm_guard_error_is_clause_failure(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t guarded = 0;
    uint32_t fallback = 0;
    uint32_t guard = 0;
    uint32_t main_fn = 0;
    CHECK(ish_bc_add_function(&module, "guarded", 1, 0, 0, &guarded));
    CHECK(ish_bc_add_function(&module, "fallback", 1, 0, 0, &fallback));
    CHECK(ish_bc_add_function(&module, "guard", 1, 0, 0, &guard));
    CHECK(ish_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(ish_bc_set_function_guard(&module, guarded, guard));
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    uint32_t c42 = 0;
    uint32_t c_bad = 0;
    CHECK(ish_bc_add_const(&module, ish_int(0), &c0));
    CHECK(ish_bc_add_const(&module, ish_int(1), &c1));
    CHECK(ish_bc_add_const(&module, ish_int(42), &c42));
    CHECK(ish_bc_add_const(&module, ish_string(&rt, "bad", &err), &c_bad));
    CHECK(!err.present);

    CHECK(ish_bc_set_function_entry(&module, guard, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_ARG, 0, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c0, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_LT, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(ish_bc_set_function_entry(&module, guarded, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c1, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(ish_bc_set_function_entry(&module, fallback, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c42, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));
    CHECK(ish_bc_set_function_entry(&module, main_fn, module.code_count));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_MAKE_MULTI_CLOSURE, 2, NULL));
    CHECK(ish_bc_emit(&module, 0, NULL));
    CHECK(ish_bc_emit(&module, guarded, NULL));
    CHECK(ish_bc_emit(&module, fallback, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_LOAD_CONST, c_bad, NULL));
    CHECK(ish_bc_emit_u32(&module, ISH_OP_CALL, 1, NULL));
    CHECK(ish_bc_emit_op(&module, ISH_OP_RETURN, NULL));

    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_core_compile_and_vm(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("<core-test>");
    IshCore *add = ish_core_app(ish_core_primitive(ISH_PRIM_ADD, span), span);
    CHECK(ish_core_app_add_arg(add, ish_core_literal(ish_int(20), span)));
    CHECK(ish_core_app_add_arg(add, ish_core_literal(ish_int(22), span)));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(add, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    ish_bc_destroy(&module);
    ish_core_free(add);

    IshCore *cond_expr = ish_core_cond(
        ish_core_literal(ish_atom(&rt, "false"), span),
        ish_core_literal(ish_int(1), span),
        ish_core_literal(ish_int(2), span),
        span);
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(cond_expr, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 2);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(cond_expr);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_scope_sets(void) {
    IshScopeStore store;
    ish_scope_store_init(&store);
    IshScopeId a = ish_scope_fresh(&store);
    IshScopeId b = ish_scope_fresh(&store);
    IshScopeSet set;
    IshScopeSet sup;
    ish_scope_set_init(&set);
    ish_scope_set_init(&sup);
    CHECK(ish_scope_set_add(&set, b));
    CHECK(ish_scope_set_add(&set, a));
    CHECK(ish_scope_set_add(&set, a));
    CHECK(set.count == 2u);
    CHECK(set.items[0] == a && set.items[1] == b);
    CHECK(ish_scope_set_add(&sup, a));
    CHECK(ish_scope_set_add(&sup, b));
    CHECK(ish_scope_set_subset(&set, &sup));
    CHECK(ish_scope_set_equal(&set, &sup));
    CHECK(ish_scope_set_flip(&sup, b));
    CHECK(!ish_scope_set_contains(&sup, b));
    CHECK(ish_scope_set_subset(&sup, &set));
    CHECK(!ish_scope_set_subset(&set, &sup));
    CHECK(ish_scope_set_flip(&sup, b));
    CHECK(ish_scope_set_equal(&set, &sup));
    ish_scope_set_destroy(&set);
    ish_scope_set_destroy(&sup);
}

static void test_binding_resolution(void) {
    IshScopeStore store;
    ish_scope_store_init(&store);
    IshScopeId s1 = ish_scope_fresh(&store);
    IshScopeId s2 = ish_scope_fresh(&store);
    IshScopeSet empty;
    IshScopeSet one;
    IshScopeSet two;
    IshScopeSet both;
    ish_scope_set_init(&empty);
    ish_scope_set_init(&one);
    ish_scope_set_init(&two);
    ish_scope_set_init(&both);
    CHECK(ish_scope_set_add(&one, s1));
    CHECK(ish_scope_set_add(&two, s2));
    CHECK(ish_scope_set_add(&both, s1));
    CHECK(ish_scope_set_add(&both, s2));

    IshBindingTable table;
    ish_binding_table_init(&table);
    IshBindingId root = 0;
    IshBindingId inner = 0;
    CHECK(ish_binding_table_add(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &empty, 10, 0u, &root));
    CHECK(ish_binding_table_add(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &one, 20, 0u, &inner));
    const IshBinding *binding = NULL;
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, &empty, &binding) == ISH_RESOLVE_OK);
    CHECK(binding && binding->id == root && binding->payload == 10);
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, &one, &binding) == ISH_RESOLVE_OK);
    CHECK(binding && binding->id == inner && binding->payload == 20);
    CHECK(ish_binding_resolve(&table, "x", 1, ISH_BIND_SPACE_DEFAULT, &one, &binding) == ISH_RESOLVE_UNBOUND);
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_PACKAGE, &one, &binding) == ISH_RESOLVE_UNBOUND);

    CHECK(ish_binding_table_add(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &one, 1, 0u, NULL));
    CHECK(ish_binding_table_add(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &two, 2, 0u, NULL));
    CHECK(ish_binding_resolve(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, &both, &binding) == ISH_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);

    CHECK(ish_binding_table_add(&table, "op", 0, ISH_BIND_SPACE_OPERATOR, ISH_BIND_OPERATOR, &empty, 99, 0u, NULL));
    CHECK(ish_binding_resolve(&table, "op", 0, ISH_BIND_SPACE_OPERATOR, &empty, &binding) == ISH_RESOLVE_OK);
    CHECK(binding && binding->kind == ISH_BIND_OPERATOR && binding->payload == 99);

    ish_binding_table_destroy(&table);
    ish_scope_set_destroy(&empty);
    ish_scope_set_destroy(&one);
    ish_scope_set_destroy(&two);
    ish_scope_set_destroy(&both);
}

static void test_syntax_scope_tree(void) {
    IshScopeStore store;
    ish_scope_store_init(&store);
    IshScopeId scope = ish_scope_fresh(&store);
    IshSpan span = ish_span_unknown("<syntax-scope-test>");
    IshSyntax *root = ish_syn_list(ISH_SEQ_PAREN, span);
    IshSyntax *word = ish_syn_word("root", span);
    IshSyntax *nested = ish_syn_vector(span);
    IshSyntax *inner = ish_syn_word("inner", span);
    CHECK(root && word && nested && inner);
    CHECK(ish_syn_append(nested, inner));
    CHECK(ish_syn_append(root, word));
    CHECK(ish_syn_append(root, nested));
    CHECK(ish_syn_scope_add_tree(root, 0, scope));
    CHECK(ish_syn_scope_contains(root, 0, scope));
    CHECK(ish_syn_scope_contains(word, 0, scope));
    CHECK(ish_syn_scope_contains(nested, 0, scope));
    CHECK(ish_syn_scope_contains(inner, 0, scope));
    CHECK(!ish_syn_scope_contains(inner, 1, scope));
    CHECK(ish_syn_scope_flip_tree(root, 0, scope));
    CHECK(!ish_syn_scope_contains(root, 0, scope));
    CHECK(!ish_syn_scope_contains(word, 0, scope));
    CHECK(!ish_syn_scope_contains(nested, 0, scope));
    CHECK(!ish_syn_scope_contains(inner, 0, scope));
    ish_syn_free(root);
}

static void test_pattern_matcher(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("<pattern-test>");
    IshValue pair = ish_cons(&rt, ish_int(1), ish_int(2), &err);
    IshPattern *pat = ish_pat_pair(ish_pat_bind("h", span), ish_pat_bind("t", span), span);
    IshPatternBindings bindings;
    ish_pattern_bindings_init(&bindings);
    CHECK(ish_pattern_match(&rt, pat, pair, &bindings, &err));
    const IshValue *h = ish_pattern_bindings_get(&bindings, "h");
    const IshValue *t = ish_pattern_bindings_get(&bindings, "t");
    CHECK(h && h->tag == ISH_VAL_INT && h->as.i == 1);
    CHECK(t && t->tag == ISH_VAL_INT && t->as.i == 2);
    CHECK(!err.present);
    ish_pattern_bindings_destroy(&bindings);
    ish_pat_free(pat);

    IshValue same = ish_cons(&rt, ish_int(7), ish_cons(&rt, ish_int(7), ish_nil(), &err), &err);
    IshValue different = ish_cons(&rt, ish_int(7), ish_cons(&rt, ish_int(8), ish_nil(), &err), &err);
    pat = ish_pat_pair(ish_pat_bind("x", span), ish_pat_pair(ish_pat_bind("x", span), ish_pat_literal(ish_nil(), span), span), span);
    ish_pattern_bindings_init(&bindings);
    CHECK(ish_pattern_match(&rt, pat, same, &bindings, &err));
    CHECK(bindings.count == 1u);
    ish_pattern_bindings_destroy(&bindings);
    ish_pattern_bindings_init(&bindings);
    CHECK(!ish_pattern_match(&rt, pat, different, &bindings, &err));
    CHECK(bindings.count == 0u);
    CHECK(!err.present);
    ish_pattern_bindings_destroy(&bindings);
    ish_pat_free(pat);

    ish_pattern_bindings_init(&bindings);
    CHECK(ish_pattern_bindings_add(&bindings, "p", ish_int(5)));
    pat = ish_pat_pin("p", span);
    CHECK(ish_pattern_match(&rt, pat, ish_int(5), &bindings, &err));
    CHECK(!ish_pattern_match(&rt, pat, ish_int(6), &bindings, &err));
    ish_pat_free(pat);
    ish_pattern_bindings_destroy(&bindings);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_core_do_and_local_bind(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("<local-core-test>");

    IshCore *add = ish_core_app(ish_core_primitive(ISH_PRIM_ADD, span), span);
    CHECK(ish_core_app_add_arg(add, ish_core_local_ref(0, span)));
    CHECK(ish_core_app_add_arg(add, ish_core_literal(ish_int(1), span)));
    IshCore *bind = ish_core_bind_local(0, ish_core_literal(ish_int(41), span), add, span);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(bind, &module, &main_fn, &err));
    CHECK(module.functions[main_fn].local_count == 1u);
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(bind);

    IshCore *do_expr = ish_core_do(span);
    CHECK(ish_core_do_add(do_expr, ish_core_literal(ish_int(1), span)));
    CHECK(ish_core_do_add(do_expr, ish_core_literal(ish_int(2), span)));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(do_expr, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 2);
    ish_bc_destroy(&module);
    ish_core_free(do_expr);

    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_core_fn_literal_and_call(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("<fn-core-test>");

    IshCore *body_add = ish_core_app(ish_core_primitive(ISH_PRIM_ADD, span), span);
    CHECK(ish_core_app_add_arg(body_add, ish_core_arg_ref(0, span)));
    CHECK(ish_core_app_add_arg(body_add, ish_core_literal(ish_int(1), span)));
    IshCore *fn = ish_core_fn("inc", 1, body_add, span);
    IshCore *call = ish_core_app(fn, span);
    CHECK(ish_core_app_add_arg(call, ish_core_literal(ish_int(41), span)));
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(call, &module, &main_fn, &err));
    IshBuffer dis;
    ish_buf_init(&dis);
    CHECK(ish_bc_disassemble(&dis, &module));
    CHECK(strstr(dis.data, "MAKE_CLOSURE") != NULL);
    CHECK(strstr(dis.data, "CALL") != NULL);
    ish_buf_destroy(&dis);
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(call);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static IshCore *binary_prim(IshPrimitive prim, IshCore *a, IshCore *b, IshSpan span) {
    IshCore *app = ish_core_app(ish_core_primitive(prim, span), span);
    CHECK(app != NULL);
    CHECK(ish_core_app_add_arg(app, a));
    CHECK(ish_core_app_add_arg(app, b));
    return app;
}

static void test_core_letrec_recursion(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("<letrec-core-test>");

    IshCore *cond = binary_prim(ISH_PRIM_LT, ish_core_arg_ref(0, span), ish_core_literal(ish_int(1), span), span);
    IshCore *minus_one = binary_prim(ISH_PRIM_SUB, ish_core_arg_ref(0, span), ish_core_literal(ish_int(1), span), span);
    IshCore *recursive_call = ish_core_app(ish_core_capture_ref(0, span), span);
    CHECK(ish_core_app_add_arg(recursive_call, minus_one));
    IshCore *sum = binary_prim(ISH_PRIM_ADD, ish_core_arg_ref(0, span), recursive_call, span);
    IshCore *body = ish_core_cond(cond, ish_core_literal(ish_int(0), span), sum, span);
    IshCore *fn = ish_core_fn("sumdown", 1, body, span);
    CHECK(ish_core_fn_add_capture(fn, false, 0));
    IshCore *letrec = ish_core_letrec(span);
    CHECK(ish_core_letrec_add(letrec, "sumdown", 0, fn));
    IshCore *call = ish_core_app(ish_core_local_ref(0, span), span);
    CHECK(ish_core_app_add_arg(call, ish_core_literal(ish_int(5), span)));
    CHECK(ish_core_letrec_set_body(letrec, call));

    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(letrec, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(letrec);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_expansion_capabilities(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<expand-test>", "x = 40\nadd x 2\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(bind-local 0 40 (app (prim add) (local 0) 2))");
    ish_buf_destroy(&dump);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    ish_bc_destroy(&module);
    ish_core_free(core);
    CHECK(!err.present);

    core = NULL;
    CHECK(!ish_expand_string(&rt, "<expand-test>", "echo hello\n", &core, &err));
    CHECK(err.present);
    ish_error_clear(&err);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<expand-test>", "implements std/shell\necho hello\n", &core, &err));
    CHECK(!err.present);
    CHECK(core != NULL);
    {
        IshBytecodeModule cmd_module;
        ish_bc_init(&cmd_module);
        uint32_t cmd_fn = 0;
        CHECK(ish_core_compile_main(core, &cmd_module, &cmd_fn, &err));
        CHECK(!err.present);
        ish_bc_destroy(&cmd_module);
    }
    ish_core_free(core);
    ish_runtime_destroy(&rt);
}

static void test_sha256_vectors(void) {
    char hex[65];
    ish_sha256_hex("", 0u, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ish_sha256_hex("abc", 3u, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ish_sha256_hex(two_block, strlen(two_block), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char block63[64];
    memset(block63, 'a', 63u);
    block63[63] = '\0';
    ish_sha256_hex(block63, 63u, hex);
    CHECK_STR(hex, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
}

static void test_intern_stability(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshValue first = ish_atom(&rt, "first-symbol");
    char name[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "sym%d", i);
        (void)ish_atom(&rt, name);
    }
    IshValue again = ish_atom(&rt, "first-symbol");
    CHECK(ish_value_equal(first, again));
    CHECK_STR(ish_symbol_text(first.as.symbol), "first-symbol");
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_bytecode_serialize_roundtrip(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    const char *src = "defn pick x do match x do [a b] -> add a b end end\npick [40 2]\n";
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<ishc-test>", src, &core, &err));
    CHECK(!err.present);
    IshBytecodeModule m1;
    ish_bc_init(&m1);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &m1, &main_fn, &err));
    CHECK(!err.present);
    IshValue out1 = ish_nil();
    CHECK(ish_vm_run(&rt, &m1, main_fn, &out1, &err));
    CHECK(!err.present);
    CHECK(out1.tag == ISH_VAL_INT && out1.as.i == 42);
    IshBuffer blob;
    ish_buf_init(&blob);
    CHECK(ish_ishc_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IshBytecodeModule m2;
    ish_bc_init(&m2);
    CHECK(ish_ishc_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    IshBuffer d1;
    IshBuffer d2;
    ish_buf_init(&d1);
    ish_buf_init(&d2);
    CHECK(ish_bc_disassemble(&d1, &m1));
    CHECK(ish_bc_disassemble(&d2, &m2));
    CHECK_STR(d2.data, d1.data);
    IshValue out2 = ish_nil();
    CHECK(ish_vm_run(&rt, &m2, main_fn, &out2, &err));
    CHECK(!err.present);
    CHECK(out2.tag == ISH_VAL_INT && out2.as.i == 42);
    IshBytecodeModule m3;
    ish_bc_init(&m3);
    CHECK(!ish_ishc_deserialize(&rt, (const unsigned char *)"XX", 2u, &m3, &err));
    ish_error_clear(&err);
    ish_bc_destroy(&m3);
    ish_buf_destroy(&d1);
    ish_buf_destroy(&d2);
    ish_buf_destroy(&blob);
    ish_bc_destroy(&m1);
    ish_bc_destroy(&m2);
    ish_core_free(core);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_syntax_serialize_roundtrip(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshSpan span = ish_span_unknown("fixture.ish");
    span.start = 3;
    span.end = 9;
    span.line = 2;
    span.column = 5;
    IshSyntax *root = ish_syn_list(ISH_SEQ_PAREN, span);
    CHECK(root != NULL);
    IshSyntax *word = ish_syn_word("hello", span);
    CHECK(word != NULL);
    CHECK(ish_syn_scope_add(word, 0, 7u));
    CHECK(ish_syn_scope_add(word, 0, 42u));
    CHECK(ish_syn_scope_add(word, 1, 9u));
    ish_syn_set_token(word, "hello", true, false);
    CHECK(ish_syn_property_set(word, "value-context", "true"));
    CHECK(ish_syn_origin_push(word, "macro-x"));
    CHECK(ish_syn_append(root, word));
    CHECK(ish_syn_append(root, ish_syn_atom("ok", span)));
    CHECK(ish_syn_append(root, ish_syn_int(-12, span)));
    CHECK(ish_syn_append(root, ish_syn_float(2.5, span)));
    CHECK(ish_syn_append(root, ish_syn_string("s\"x", span)));
    IshSyntax *inner = ish_syn_tuple(span);
    CHECK(inner != NULL);
    CHECK(ish_syn_append(inner, ish_syn_word("deep", span)));
    CHECK(ish_syn_append(root, inner));

    IshBuffer blob;
    ish_buf_init(&blob);
    CHECK(ish_syn_serialize(&blob, root, &err));
    CHECK(!err.present);
    IshByteReader r;
    ish_byte_reader_init(&r, (const unsigned char *)blob.data, blob.len);
    IshSyntax *back = ish_syn_deserialize(&rt, &r, &err);
    CHECK(back != NULL);
    CHECK(!err.present);
    CHECK(r.pos == blob.len);

    IshBuffer d1, d2;
    ish_buf_init(&d1);
    ish_buf_init(&d2);
    CHECK(ish_syn_dump(&d1, root));
    CHECK(ish_syn_dump(&d2, back));
    CHECK_STR(d2.data, d1.data);
    IshSyntax *back_word = back->as.seq.items[0];
    CHECK(back_word->kind == ISH_SYN_WORD);
    CHECK(ish_syn_scope_contains(back_word, 0, 7u));
    CHECK(ish_syn_scope_contains(back_word, 0, 42u));
    CHECK(ish_syn_scope_contains(back_word, 1, 9u));
    CHECK(!ish_syn_scope_contains(back_word, 0, 9u));
    CHECK(back_word->token_raw != NULL && strcmp(back_word->token_raw, "hello") == 0);
    CHECK(back_word->token_leading_space && !back_word->token_adjacent_previous);
    const char *prop = ish_syn_property_get(back_word, "value-context");
    CHECK(prop != NULL && strcmp(prop, "true") == 0);
    CHECK(back_word->origins.count == 1 && strcmp(back_word->origins.items[0], "macro-x") == 0);
    CHECK(back->as.seq.items[1]->kind == ISH_SYN_ATOM);
    CHECK(back->as.seq.items[2]->as.integer == -12);
    CHECK(back->as.seq.items[3]->as.real == 2.5);
    CHECK(back_word->span.file != NULL && strcmp(back_word->span.file, "fixture.ish") == 0);
    CHECK(back_word->span.start == 3 && back_word->span.end == 9 && back_word->span.line == 2 && back_word->span.column == 5);

    ish_syn_scope_relocate_tree(back, 10u, 100u);
    CHECK(ish_syn_scope_contains(back_word, 0, 7u));
    CHECK(ish_syn_scope_contains(back_word, 0, 142u));
    CHECK(!ish_syn_scope_contains(back_word, 0, 42u));
    CHECK(ish_syn_scope_contains(back_word, 1, 9u));

    IshByteReader bad;
    ish_byte_reader_init(&bad, (const unsigned char *)blob.data, blob.len > 8u ? blob.len - 8u : 1u);
    IshSyntax *trunc = ish_syn_deserialize(&rt, &bad, &err);
    CHECK(trunc == NULL && err.present);
    ish_error_clear(&err);

    ish_buf_destroy(&d1);
    ish_buf_destroy(&d2);
    ish_buf_destroy(&blob);
    ish_syn_free(root);
    ish_syn_free(back);
    ish_runtime_destroy(&rt);
}

static void test_module_syntax_constant_roundtrip(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshBytecodeModule m1;
    ish_bc_init(&m1);
    IshSpan span = ish_span_unknown("tmpl.ish");
    IshSyntax *template_syn = ish_syn_list(ISH_SEQ_PAREN, span);
    CHECK(template_syn != NULL);
    IshSyntax *head = ish_syn_word("cond", span);
    CHECK(head != NULL);
    CHECK(ish_syn_scope_add(head, 0, 21u));
    CHECK(ish_syn_append(template_syn, head));
    CHECK(ish_syn_append(template_syn, ish_syn_int(1, span)));
    IshValue template_value = ish_syntax_value(&rt, template_syn, &err);
    CHECK(!err.present);
    uint32_t const_index = 0;
    CHECK(ish_bc_add_const(&m1, template_value, &const_index));
    uint32_t fn = 0;
    CHECK(ish_bc_add_function(&m1, "main", 0, 0, 0, &fn));
    CHECK(ish_bc_emit_u32(&m1, ISH_OP_LOAD_CONST, const_index, NULL));
    CHECK(ish_bc_emit_op(&m1, ISH_OP_RETURN, NULL));

    IshBuffer blob;
    ish_buf_init(&blob);
    CHECK(ish_ishc_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IshBytecodeModule m2;
    ish_bc_init(&m2);
    CHECK(ish_ishc_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    CHECK(m2.const_count == 1 && m2.constants[0].tag == ISH_VAL_SYNTAX);
    const IshSyntax *back = ish_syntax_get(m2.constants[0], &err);
    CHECK(back != NULL && !err.present);
    CHECK(ish_syn_scope_contains(back->as.seq.items[0], 0, 21u));

    const IshSyntax *original = ish_syntax_get(m1.constants[0], &err);
    CHECK(original != NULL && !err.present);
    CHECK(ish_bc_relocate_syntax_scopes(&rt, &m2, 10u, 50u, &err));
    CHECK(!err.present);
    const IshSyntax *moved = ish_syntax_get(m2.constants[0], &err);
    CHECK(moved != NULL && !err.present);
    CHECK(ish_syn_scope_contains(moved->as.seq.items[0], 0, 71u));
    CHECK(!ish_syn_scope_contains(moved->as.seq.items[0], 0, 21u));
    CHECK(ish_syn_scope_contains(original->as.seq.items[0], 0, 21u));

    IshBytecodeModule linked;
    ish_bc_init(&linked);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    CHECK(ish_bc_link(&linked, &m1, &const_off, &fn_off, &code_off, &err));
    CHECK(ish_bc_relocate_syntax_scopes(&rt, &linked, 10u, 1000u, &err));
    const IshSyntax *aliased = ish_syntax_get(m1.constants[0], &err);
    CHECK(aliased != NULL && ish_syn_scope_contains(aliased->as.seq.items[0], 0, 21u));
    const IshSyntax *relinked = ish_syntax_get(linked.constants[0], &err);
    CHECK(relinked != NULL && ish_syn_scope_contains(relinked->as.seq.items[0], 0, 1021u));

    ish_buf_destroy(&blob);
    ish_bc_destroy(&m1);
    ish_bc_destroy(&m2);
    ish_bc_destroy(&linked);
    ish_syn_free(template_syn);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

void run_substrate_suite(void) {
    test_values();
    test_reader_basic();
    test_reader_shell_protocols();
    test_reader_quote();
    test_reader_body_and_function_value();
    test_bytecode_verifier();
    test_vm_call_and_locals();
    test_vm_tail_call_reuses_frame();
    test_vm_guard_error_is_clause_failure();
    test_core_compile_and_vm();
    test_scope_sets();
    test_binding_resolution();
    test_syntax_scope_tree();
    test_pattern_matcher();
    test_core_do_and_local_bind();
    test_core_fn_literal_and_call();
    test_core_letrec_recursion();
    test_source_expansion_capabilities();
    test_sha256_vectors();
    test_intern_stability();
    test_bytecode_serialize_roundtrip();
    test_syntax_serialize_roundtrip();
    test_module_syntax_constant_roundtrip();
}
