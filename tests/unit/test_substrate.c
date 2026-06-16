#include "test_util.h"

static bool emit_prim_call(IdmBytecodeModule *module, IdmPrimitive prim, uint32_t argc) {
    return idm_bc_emit_u32(module, IDM_OP_PRIM_CALL, (uint32_t)prim, NULL) &&
           idm_bc_emit(module, argc, NULL);
}

static void test_values(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue a1 = idm_atom(&rt, "ok");
    IdmValue a2 = idm_atom(&rt, "ok");
    IdmValue w1 = idm_word(&rt, "ok");
    CHECK(idm_value_equal(a1, a2));
    CHECK(!idm_value_equal(a1, w1));
    IdmValue s1 = idm_string(&rt, "hello", &err);
    IdmValue s2 = idm_string(&rt, "hello", &err);
    CHECK(idm_value_equal(s1, s2));
    IdmValue list = idm_cons(&rt, idm_int(1), idm_cons(&rt, idm_int(2), idm_empty_list(), &err), &err);
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, list));
    CHECK_STR(buf.data, "(1 2)");
    idm_buf_destroy(&buf);
    CHECK(idm_heap_object_count(&rt.heap) == 4u);
    IdmValue roots[1] = { list };
    idm_heap_collect(&rt.heap, roots, 1u);
    CHECK(idm_heap_object_count(&rt.heap) == 2u);
    IdmValue car = idm_car(list, &err);
    CHECK(car.tag == IDM_VAL_INT && car.as.i == 1);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_gc_deep_pair_chain(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue list = idm_empty_list();
    for (size_t i = 0; i < 100000u; i++) {
        list = idm_cons(&rt, idm_int((int64_t)i), list, &err);
        CHECK(!err.present);
    }
    IdmValue roots[1] = { list };
    idm_heap_collect(&rt.heap, roots, 1u);
    CHECK(idm_heap_object_count(&rt.heap) == 100000u);
    idm_heap_collect(&rt.heap, NULL, 0u);
    CHECK(idm_heap_object_count(&rt.heap) == 0u);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
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

    s = dump_reader("[1 2 :ok] (list a b) {:ok 1}\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr [1 2 :ok] (%-group (%-expr list a b)) {:ok 1}))");
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
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmError err;
    idm_error_init(&err);
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, 99, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(!idm_bc_verify(&module, &err));
    CHECK(err.present);
    idm_error_clear(&err);
    idm_bc_destroy(&module);
}

static void test_vm_call_and_locals(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t add2 = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "add2", 2, 0, 0, &add2));
    CHECK(idm_bc_add_function(&module, "main", 0, 1, 0, &main_fn));
    CHECK(idm_bc_set_function_entry(&module, add2, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_ARG, 0, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_ARG, 1, NULL));
    CHECK(emit_prim_call(&module, IDM_PRIM_ADD, 2));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c20 = 0;
    uint32_t c22 = 0;
    uint32_t c1 = 0;
    CHECK(idm_bc_add_const(&module, idm_int(20), &c20));
    CHECK(idm_bc_add_const(&module, idm_int(22), &c22));
    CHECK(idm_bc_add_const(&module, idm_int(1), &c1));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_MAKE_CLOSURE, add2, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c20, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c22, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_CALL, 2, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_STORE_LOCAL, 0, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_LOCAL, 0, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c1, NULL));
    CHECK(emit_prim_call(&module, IDM_PRIM_SUB, 2));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 41);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_vm_tail_call_reuses_frame(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t id_fn = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "id", 1, 0, 0, &id_fn));
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_set_function_entry(&module, id_fn, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_ARG, 0, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c99 = 0;
    CHECK(idm_bc_add_const(&module, idm_int(99), &c99));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_MAKE_CLOSURE, id_fn, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c99, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_TAIL_CALL, 1, NULL));
    IdmVmLimits limits = idm_vm_default_limits();
    limits.max_frames = 1;
    IdmValue out = idm_nil();
    CHECK(idm_vm_run_limited(&rt, &module, main_fn, limits, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 99);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_vm_guard_error_is_clause_failure(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t guarded = 0;
    uint32_t fallback = 0;
    uint32_t guard = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "guarded", 1, 0, 0, &guarded));
    CHECK(idm_bc_add_function(&module, "fallback", 1, 0, 0, &fallback));
    CHECK(idm_bc_add_function(&module, "guard", 1, 0, 0, &guard));
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_set_function_guard(&module, guarded, guard));
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    uint32_t c42 = 0;
    uint32_t c_bad = 0;
    CHECK(idm_bc_add_const(&module, idm_int(0), &c0));
    CHECK(idm_bc_add_const(&module, idm_int(1), &c1));
    CHECK(idm_bc_add_const(&module, idm_int(42), &c42));
    CHECK(idm_bc_add_const(&module, idm_string(&rt, "bad", &err), &c_bad));
    CHECK(!err.present);

    CHECK(idm_bc_set_function_entry(&module, guard, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_ARG, 0, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c0, NULL));
    CHECK(emit_prim_call(&module, IDM_PRIM_LT, 2));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_set_function_entry(&module, guarded, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c1, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_set_function_entry(&module, fallback, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c42, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_MAKE_MULTI_CLOSURE, 2, NULL));
    CHECK(idm_bc_emit(&module, 0, NULL));
    CHECK(idm_bc_emit(&module, guarded, NULL));
    CHECK(idm_bc_emit(&module, fallback, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_LOAD_CONST, c_bad, NULL));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_CALL, 1, NULL));
    CHECK(idm_bc_emit_op(&module, IDM_OP_RETURN, NULL));

    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_compile_and_vm(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<core-test>");
    IdmCore *add = idm_core_app(idm_core_primitive(IDM_PRIM_ADD, span), span);
    CHECK(idm_core_app_add_arg(add, idm_core_literal(idm_int(20), span)));
    CHECK(idm_core_app_add_arg(add, idm_core_literal(idm_int(22), span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(add, &module, &main_fn, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    idm_bc_destroy(&module);
    idm_core_free(add);

    IdmCore *cond_expr = idm_core_cond(
        idm_core_literal(idm_atom(&rt, "false"), span),
        idm_core_literal(idm_int(1), span),
        idm_core_literal(idm_int(2), span),
        span);
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(cond_expr, &module, &main_fn, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 2);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(cond_expr);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_scope_sets(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId a = idm_scope_fresh(&store);
    IdmScopeId b = idm_scope_fresh(&store);
    IdmScopeSet set;
    IdmScopeSet sup;
    idm_scope_set_init(&set);
    idm_scope_set_init(&sup);
    CHECK(idm_scope_set_add(&set, b));
    CHECK(idm_scope_set_add(&set, a));
    CHECK(idm_scope_set_add(&set, a));
    CHECK(set.count == 2u);
    CHECK(set.items[0] == a && set.items[1] == b);
    CHECK(idm_scope_set_add(&sup, a));
    CHECK(idm_scope_set_add(&sup, b));
    CHECK(idm_scope_set_subset(&set, &sup));
    CHECK(idm_scope_set_equal(&set, &sup));
    CHECK(idm_scope_set_flip(&sup, b));
    CHECK(!idm_scope_set_contains(&sup, b));
    CHECK(idm_scope_set_subset(&sup, &set));
    CHECK(!idm_scope_set_subset(&set, &sup));
    CHECK(idm_scope_set_flip(&sup, b));
    CHECK(idm_scope_set_equal(&set, &sup));
    idm_scope_set_destroy(&set);
    idm_scope_set_destroy(&sup);
}

static void test_binding_resolution(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId s1 = idm_scope_fresh(&store);
    IdmScopeId s2 = idm_scope_fresh(&store);
    IdmScopeSet empty;
    IdmScopeSet one;
    IdmScopeSet two;
    IdmScopeSet both;
    idm_scope_set_init(&empty);
    idm_scope_set_init(&one);
    idm_scope_set_init(&two);
    idm_scope_set_init(&both);
    CHECK(idm_scope_set_add(&one, s1));
    CHECK(idm_scope_set_add(&two, s2));
    CHECK(idm_scope_set_add(&both, s1));
    CHECK(idm_scope_set_add(&both, s2));

    IdmBindingTable table;
    idm_binding_table_init(&table);
    IdmBindingId root = 0;
    IdmBindingId inner = 0;
    CHECK(idm_binding_table_add(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &empty, 10, 0u, &root));
    CHECK(idm_binding_table_add(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one, 20, 0u, &inner));
    const IdmBinding *binding = NULL;
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, &empty, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->id == root && binding->payload == 10);
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, &one, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->id == inner && binding->payload == 20);
    CHECK(idm_binding_resolve(&table, "x", 1, IDM_BIND_SPACE_DEFAULT, &one, &binding) == IDM_RESOLVE_UNBOUND);
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_PACKAGE, &one, &binding) == IDM_RESOLVE_UNBOUND);

    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one, 1, 0u, NULL));
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &two, 2, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &both, &binding) == IDM_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);

    IdmScopeId s3 = idm_scope_fresh(&store);
    IdmScopeSet one_three;
    IdmScopeSet all;
    idm_scope_set_init(&one_three);
    idm_scope_set_init(&all);
    CHECK(idm_scope_set_add(&one_three, s1));
    CHECK(idm_scope_set_add(&one_three, s3));
    CHECK(idm_scope_set_add(&all, s1));
    CHECK(idm_scope_set_add(&all, s2));
    CHECK(idm_scope_set_add(&all, s3));
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one_three, 3, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &all, &binding) == IDM_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &both, 4, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &both, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->payload == 4);
    idm_scope_set_destroy(&one_three);
    idm_scope_set_destroy(&all);

    CHECK(idm_binding_table_add(&table, "op", 0, IDM_BIND_SPACE_OPERATOR, IDM_BIND_OPERATOR, &empty, 99, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "op", 0, IDM_BIND_SPACE_OPERATOR, &empty, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->kind == IDM_BIND_OPERATOR && binding->payload == 99);

    idm_binding_table_destroy(&table);
    idm_scope_set_destroy(&empty);
    idm_scope_set_destroy(&one);
    idm_scope_set_destroy(&two);
    idm_scope_set_destroy(&both);
}

static void test_syntax_scope_tree(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId scope = idm_scope_fresh(&store);
    IdmSpan span = idm_span_unknown("<syntax-scope-test>");
    IdmSyntax *root = idm_syn_list(span);
    IdmSyntax *word = idm_syn_word("root", span);
    IdmSyntax *nested = idm_syn_vector(span);
    IdmSyntax *inner = idm_syn_word("inner", span);
    CHECK(root && word && nested && inner);
    CHECK(idm_syn_append(nested, inner));
    CHECK(idm_syn_append(root, word));
    CHECK(idm_syn_append(root, nested));
    CHECK(idm_syn_scope_add_tree(root, 0, scope));
    CHECK(idm_syn_scope_contains(root, 0, scope));
    CHECK(idm_syn_scope_contains(word, 0, scope));
    CHECK(idm_syn_scope_contains(nested, 0, scope));
    CHECK(idm_syn_scope_contains(inner, 0, scope));
    CHECK(!idm_syn_scope_contains(inner, 1, scope));
    CHECK(idm_syn_scope_flip_tree(root, 0, scope));
    CHECK(!idm_syn_scope_contains(root, 0, scope));
    CHECK(!idm_syn_scope_contains(word, 0, scope));
    CHECK(!idm_syn_scope_contains(nested, 0, scope));
    CHECK(!idm_syn_scope_contains(inner, 0, scope));
    idm_syn_free(root);
}

static void test_pattern_matcher(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<pattern-test>");
    IdmValue pair = idm_cons(&rt, idm_int(1), idm_int(2), &err);
    IdmPattern *pat = idm_pat_pair(idm_pat_bind("h", span), idm_pat_bind("t", span), span);
    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_match(&rt, pat, pair, &bindings, &err));
    const IdmValue *h = idm_pattern_bindings_get(&bindings, "h");
    const IdmValue *t = idm_pattern_bindings_get(&bindings, "t");
    CHECK(h && h->tag == IDM_VAL_INT && h->as.i == 1);
    CHECK(t && t->tag == IDM_VAL_INT && t->as.i == 2);
    CHECK(!err.present);
    idm_pattern_bindings_destroy(&bindings);
    idm_pat_free(pat);

    IdmValue same = idm_cons(&rt, idm_int(7), idm_cons(&rt, idm_int(7), idm_nil(), &err), &err);
    IdmValue different = idm_cons(&rt, idm_int(7), idm_cons(&rt, idm_int(8), idm_nil(), &err), &err);
    pat = idm_pat_pair(idm_pat_bind("x", span), idm_pat_pair(idm_pat_bind("x", span), idm_pat_literal(idm_nil(), span), span), span);
    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_match(&rt, pat, same, &bindings, &err));
    CHECK(bindings.count == 1u);
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_bindings_init(&bindings);
    CHECK(!idm_pattern_match(&rt, pat, different, &bindings, &err));
    CHECK(bindings.count == 0u);
    CHECK(!err.present);
    idm_pattern_bindings_destroy(&bindings);
    idm_pat_free(pat);

    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_bindings_add(&bindings, "p", idm_int(5)));
    pat = idm_pat_pin("p", span);
    CHECK(idm_pattern_match(&rt, pat, idm_int(5), &bindings, &err));
    CHECK(!idm_pattern_match(&rt, pat, idm_int(6), &bindings, &err));
    idm_pat_free(pat);
    idm_pattern_bindings_destroy(&bindings);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_do_and_local_bind(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<local-core-test>");

    IdmCore *add = idm_core_app(idm_core_primitive(IDM_PRIM_ADD, span), span);
    CHECK(idm_core_app_add_arg(add, idm_core_local_ref(0, span)));
    CHECK(idm_core_app_add_arg(add, idm_core_literal(idm_int(1), span)));
    IdmCore *bind = idm_core_bind_local(0, idm_core_literal(idm_int(41), span), add, span);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(bind, &module, &main_fn, &err));
    CHECK(module.functions[main_fn].local_count == 1u);
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(bind);

    IdmCore *do_expr = idm_core_do(span);
    CHECK(idm_core_do_add(do_expr, idm_core_literal(idm_int(1), span)));
    CHECK(idm_core_do_add(do_expr, idm_core_literal(idm_int(2), span)));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(do_expr, &module, &main_fn, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 2);
    idm_bc_destroy(&module);
    idm_core_free(do_expr);

    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_fn_literal_and_call(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<fn-core-test>");

    IdmCore *body_add = idm_core_app(idm_core_primitive(IDM_PRIM_ADD, span), span);
    CHECK(idm_core_app_add_arg(body_add, idm_core_arg_ref(0, span)));
    CHECK(idm_core_app_add_arg(body_add, idm_core_literal(idm_int(1), span)));
    IdmCore *fn = idm_core_fn("inc", 1, body_add, span);
    IdmCore *call = idm_core_app(fn, span);
    CHECK(idm_core_app_add_arg(call, idm_core_literal(idm_int(41), span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(call, &module, &main_fn, &err));
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &module));
    CHECK(strstr(dis.data, "MAKE_CLOSURE") != NULL);
    CHECK(strstr(dis.data, "CALL") != NULL);
    idm_buf_destroy(&dis);
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(call);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static IdmCore *binary_prim(IdmPrimitive prim, IdmCore *a, IdmCore *b, IdmSpan span) {
    IdmCore *app = idm_core_app(idm_core_primitive(prim, span), span);
    CHECK(app != NULL);
    CHECK(idm_core_app_add_arg(app, a));
    CHECK(idm_core_app_add_arg(app, b));
    return app;
}

static void test_core_letrec_recursion(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<letrec-core-test>");

    IdmCore *cond = binary_prim(IDM_PRIM_LT, idm_core_arg_ref(0, span), idm_core_literal(idm_int(1), span), span);
    IdmCore *minus_one = binary_prim(IDM_PRIM_SUB, idm_core_arg_ref(0, span), idm_core_literal(idm_int(1), span), span);
    IdmCore *recursive_call = idm_core_app(idm_core_capture_ref(0, span), span);
    CHECK(idm_core_app_add_arg(recursive_call, minus_one));
    IdmCore *sum = binary_prim(IDM_PRIM_ADD, idm_core_arg_ref(0, span), recursive_call, span);
    IdmCore *body = idm_core_cond(cond, idm_core_literal(idm_int(0), span), sum, span);
    IdmCore *fn = idm_core_fn("sumdown", 1, body, span);
    CHECK(idm_core_fn_add_capture(fn, false, 0));
    IdmCore *letrec = idm_core_letrec(span);
    CHECK(idm_core_letrec_add(letrec, "sumdown", 0, fn));
    IdmCore *call = idm_core_app(idm_core_local_ref(0, span), span);
    CHECK(idm_core_app_add_arg(call, idm_core_literal(idm_int(5), span)));
    CHECK(idm_core_letrec_set_body(letrec, call));

    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(letrec, &module, &main_fn, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(letrec);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_expansion_capabilities(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt, "<expand-test>", "x = 40\nadd x 2\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK_STR(dump.data, "(bind-local 0 40 (app (prim add) (local 0) 2))");
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    idm_bc_destroy(&module);
    idm_core_free(core);
    CHECK(!err.present);

    core = NULL;
    CHECK(!idm_expand_string(&rt, "<expand-test>", "echo hello\n", &core, &err));
    CHECK(err.present);
    idm_error_clear(&err);

    core = NULL;
    CHECK(idm_expand_string(&rt, "<expand-test>", "activate std/shell\necho hello\n", &core, &err));
    CHECK(!err.present);
    CHECK(core != NULL);
    {
        IdmBytecodeModule cmd_module;
        idm_bc_init(&cmd_module);
        uint32_t cmd_fn = 0;
        CHECK(idm_core_compile_main(core, &cmd_module, &cmd_fn, &err));
        CHECK(!err.present);
        idm_bc_destroy(&cmd_module);
    }
    idm_core_free(core);
    idm_runtime_destroy(&rt);
}

static void test_sha256_vectors(void) {
    char hex[65];
    idm_sha256_hex("", 0u, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    idm_sha256_hex("abc", 3u, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    idm_sha256_hex(two_block, strlen(two_block), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char block63[64];
    memset(block63, 'a', 63u);
    block63[63] = '\0';
    idm_sha256_hex(block63, 63u, hex);
    CHECK_STR(hex, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
}

static void test_intern_stability(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue first = idm_atom(&rt, "first-symbol");
    char name[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "sym%d", i);
        (void)idm_atom(&rt, name);
    }
    IdmValue again = idm_atom(&rt, "first-symbol");
    CHECK(idm_value_equal(first, again));
    CHECK_STR(idm_symbol_text(first.as.symbol), "first-symbol");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_bytecode_serialize_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *src = "defn pick x do match x do [a b] -> add a b end end\npick [40 2]\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_string(&rt, "<ishc-test>", src, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &m1, &main_fn, &err));
    CHECK(!err.present);
    IdmValue out1 = idm_nil();
    CHECK(idm_vm_run(&rt, &m1, main_fn, &out1, &err));
    CHECK(!err.present);
    CHECK(out1.tag == IDM_VAL_INT && out1.as.i == 42);
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    IdmBuffer d1;
    IdmBuffer d2;
    idm_buf_init(&d1);
    idm_buf_init(&d2);
    CHECK(idm_bc_disassemble(&d1, &m1));
    CHECK(idm_bc_disassemble(&d2, &m2));
    CHECK_STR(d2.data, d1.data);
    IdmValue out2 = idm_nil();
    CHECK(idm_vm_run(&rt, &m2, main_fn, &out2, &err));
    CHECK(!err.present);
    CHECK(out2.tag == IDM_VAL_INT && out2.as.i == 42);
    IdmBytecodeModule m3;
    idm_bc_init(&m3);
    CHECK(!idm_ic_deserialize(&rt, (const unsigned char *)"XX", 2u, &m3, &err));
    idm_error_clear(&err);
    idm_bc_destroy(&m3);
    idm_buf_destroy(&d1);
    idm_buf_destroy(&d2);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_syntax_serialize_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("fixture.id");
    span.start = 3;
    span.end = 9;
    span.line = 2;
    span.column = 5;
    IdmSyntax *root = idm_syn_list(span);
    CHECK(root != NULL);
    IdmSyntax *word = idm_syn_word("hello", span);
    CHECK(word != NULL);
    CHECK(idm_syn_scope_add(word, 0, 7u));
    CHECK(idm_syn_scope_add(word, 0, 42u));
    CHECK(idm_syn_scope_add(word, 1, 9u));
    idm_syn_set_token(word, "hello", true, false);
    CHECK(idm_syn_property_set(word, "value-context", "true"));
    CHECK(idm_syn_origin_push(word, "macro-x"));
    CHECK(idm_syn_append(root, word));
    CHECK(idm_syn_append(root, idm_syn_atom("ok", span)));
    CHECK(idm_syn_append(root, idm_syn_int(-12, span)));
    CHECK(idm_syn_append(root, idm_syn_float(2.5, span)));
    CHECK(idm_syn_append(root, idm_syn_string("s\"x", span)));
    IdmSyntax *inner = idm_syn_tuple(span);
    CHECK(inner != NULL);
    CHECK(idm_syn_append(inner, idm_syn_word("deep", span)));
    CHECK(idm_syn_append(root, inner));

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_syn_serialize(&blob, root, &err));
    CHECK(!err.present);
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)blob.data, blob.len);
    IdmSyntax *back = idm_syn_deserialize(&rt, &r, &err);
    CHECK(back != NULL);
    CHECK(!err.present);
    CHECK(r.pos == blob.len);

    IdmBuffer d1, d2;
    idm_buf_init(&d1);
    idm_buf_init(&d2);
    CHECK(idm_syn_dump(&d1, root));
    CHECK(idm_syn_dump(&d2, back));
    CHECK_STR(d2.data, d1.data);
    IdmSyntax *back_word = back->as.seq.items[0];
    CHECK(back_word->kind == IDM_SYN_WORD);
    CHECK(idm_syn_scope_contains(back_word, 0, 7u));
    CHECK(idm_syn_scope_contains(back_word, 0, 42u));
    CHECK(idm_syn_scope_contains(back_word, 1, 9u));
    CHECK(!idm_syn_scope_contains(back_word, 0, 9u));
    CHECK(back_word->token_raw != NULL && strcmp(back_word->token_raw, "hello") == 0);
    CHECK(back_word->token_leading_space && !back_word->token_adjacent_previous);
    const char *prop = idm_syn_property_get(back_word, "value-context");
    CHECK(prop != NULL && strcmp(prop, "true") == 0);
    CHECK(back_word->origins.count == 1 && strcmp(back_word->origins.items[0], "macro-x") == 0);
    CHECK(back->as.seq.items[1]->kind == IDM_SYN_ATOM);
    CHECK(back->as.seq.items[2]->as.integer == -12);
    CHECK(back->as.seq.items[3]->as.real == 2.5);
    CHECK(back_word->span.file != NULL && strcmp(back_word->span.file, "fixture.id") == 0);
    CHECK(back_word->span.start == 3 && back_word->span.end == 9 && back_word->span.line == 2 && back_word->span.column == 5);

    idm_syn_scope_relocate_tree(back, 10u, 100u);
    CHECK(idm_syn_scope_contains(back_word, 0, 7u));
    CHECK(idm_syn_scope_contains(back_word, 0, 142u));
    CHECK(!idm_syn_scope_contains(back_word, 0, 42u));
    CHECK(idm_syn_scope_contains(back_word, 1, 9u));

    IdmByteReader bad;
    idm_byte_reader_init(&bad, (const unsigned char *)blob.data, blob.len > 8u ? blob.len - 8u : 1u);
    IdmSyntax *trunc = idm_syn_deserialize(&rt, &bad, &err);
    CHECK(trunc == NULL && err.present);
    idm_error_clear(&err);

    idm_buf_destroy(&d1);
    idm_buf_destroy(&d2);
    idm_buf_destroy(&blob);
    idm_syn_free(root);
    idm_syn_free(back);
    idm_runtime_destroy(&rt);
}

static void test_module_syntax_constant_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    IdmSpan span = idm_span_unknown("tmpl.id");
    IdmSyntax *template_syn = idm_syn_list(span);
    CHECK(template_syn != NULL);
    IdmSyntax *head = idm_syn_word("cond", span);
    CHECK(head != NULL);
    CHECK(idm_syn_scope_add(head, 0, 21u));
    CHECK(idm_syn_append(template_syn, head));
    CHECK(idm_syn_append(template_syn, idm_syn_int(1, span)));
    IdmValue template_value = idm_syntax_value(&rt, template_syn, &err);
    CHECK(!err.present);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m1, template_value, &const_index));
    uint32_t fn = 0;
    CHECK(idm_bc_add_function(&m1, "main", 0, 0, 0, &fn));
    CHECK(idm_bc_emit_u32(&m1, IDM_OP_LOAD_CONST, const_index, NULL));
    CHECK(idm_bc_emit_op(&m1, IDM_OP_RETURN, NULL));

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    CHECK(m2.const_count == 1 && m2.constants[0].tag == IDM_VAL_SYNTAX);
    const IdmSyntax *back = idm_syntax_get(m2.constants[0], &err);
    CHECK(back != NULL && !err.present);
    CHECK(idm_syn_scope_contains(back->as.seq.items[0], 0, 21u));

    const IdmSyntax *original = idm_syntax_get(m1.constants[0], &err);
    CHECK(original != NULL && !err.present);
    CHECK(idm_bc_relocate_syntax_scopes(&rt, &m2, 10u, 50u, &err));
    CHECK(!err.present);
    const IdmSyntax *moved = idm_syntax_get(m2.constants[0], &err);
    CHECK(moved != NULL && !err.present);
    CHECK(idm_syn_scope_contains(moved->as.seq.items[0], 0, 71u));
    CHECK(!idm_syn_scope_contains(moved->as.seq.items[0], 0, 21u));
    CHECK(idm_syn_scope_contains(original->as.seq.items[0], 0, 21u));

    IdmBytecodeModule linked;
    idm_bc_init(&linked);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    CHECK(idm_bc_link(&linked, &m1, &const_off, &fn_off, &code_off, &err));
    CHECK(idm_bc_relocate_syntax_scopes(&rt, &linked, 10u, 1000u, &err));
    const IdmSyntax *aliased = idm_syntax_get(m1.constants[0], &err);
    CHECK(aliased != NULL && idm_syn_scope_contains(aliased->as.seq.items[0], 0, 21u));
    const IdmSyntax *relinked = idm_syntax_get(linked.constants[0], &err);
    CHECK(relinked != NULL && idm_syn_scope_contains(relinked->as.seq.items[0], 0, 1021u));

    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_bc_destroy(&linked);
    idm_syn_free(template_syn);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_string_constant_nul_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    const char bytes[] = "nul\0byte";
    IdmValue s = idm_string_n(&rt, bytes, sizeof(bytes) - 1u, &err);
    CHECK(!err.present);
    CHECK(idm_string_length(s) == sizeof(bytes) - 1u);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m1, s, &const_index));
    uint32_t fn = 0;
    CHECK(idm_bc_add_function(&m1, "main", 0, 0, 0, &fn));
    CHECK(idm_bc_emit_u32(&m1, IDM_OP_LOAD_CONST, const_index, NULL));
    CHECK(idm_bc_emit_op(&m1, IDM_OP_RETURN, NULL));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    CHECK(m2.const_count == 1 && m2.constants[0].tag == IDM_VAL_STRING);
    CHECK(idm_string_length(m2.constants[0]) == sizeof(bytes) - 1u);
    CHECK(memcmp(idm_string_bytes(m2.constants[0]), bytes, sizeof(bytes) - 1u) == 0);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_depth_guard(void) {
    size_t n = 5000;
    IdmBuffer src;
    idm_buf_init(&src);
    for (size_t i = 0; i < n; i++) CHECK(idm_buf_append_char(&src, '['));
    for (size_t i = 0; i < n; i++) CHECK(idm_buf_append_char(&src, ']'));
    CHECK(idm_buf_append_char(&src, '\n'));
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;
    CHECK(!idm_reader_read_string("<deep>", src.data, &program, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    CHECK(err.span.file != NULL && strcmp(err.span.file, "<deep>") == 0);
    idm_error_clear(&err);
    idm_buf_destroy(&src);
}

static void test_serialize_depth_guard(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("deep.id");
    IdmSyntax *deep = idm_syn_int(1, span);
    CHECK(deep != NULL);
    for (size_t i = 0; i < 2000; i++) {
        IdmSyntax *wrap = idm_syn_list(span);
        CHECK(wrap != NULL && idm_syn_append(wrap, deep));
        deep = wrap;
    }
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(!idm_syn_serialize(&blob, deep, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    idm_error_clear(&err);
    idm_syn_free(deep);
    idm_buf_destroy(&blob);

    IdmValue list = idm_nil();
    for (size_t i = 0; i < 2000; i++) {
        list = idm_cons(&rt, idm_int((int64_t)i), list, &err);
        CHECK(!err.present);
    }
    IdmBytecodeModule m;
    idm_bc_init(&m);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m, list, &const_index));
    idm_buf_init(&blob);
    CHECK(!idm_ic_serialize(&m, &blob, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    idm_error_clear(&err);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m);
    idm_runtime_destroy(&rt);
}

static void test_value_copy(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    IdmValue s = idm_string(&rt, "hi", &err);
    IdmValue lst = idm_cons(&rt, idm_int(1), idm_cons(&rt, idm_int(2), idm_empty_list(), &err), &err);
    IdmValue items[2] = { s, lst };
    IdmValue orig = idm_tuple(&rt, items, 2u, &err);
    CHECK(!err.present);

    IdmValue copy = idm_value_copy(&rt, &rt.heap, orig, &err);
    CHECK(!err.present);
    CHECK(idm_value_equal(orig, copy));
    CHECK(orig.as.obj != copy.as.obj);
    IdmValue cs = idm_sequence_item(copy, 0u, &err);
    IdmValue os = idm_sequence_item(orig, 0u, &err);
    CHECK(cs.as.obj != os.as.obj);
    CHECK(idm_value_equal(cs, os));

    IdmValue k = idm_cell(&rt, idm_nil(), &err);
    IdmValue titems[2] = { idm_int(1), k };
    IdmValue t = idm_tuple(&rt, titems, 2u, &err);
    CHECK(idm_cell_set(k, t, &err));
    IdmValue tcopy = idm_value_copy(&rt, &rt.heap, t, &err);
    CHECK(!err.present);
    CHECK(tcopy.as.obj != t.as.obj);
    IdmValue kcopy = idm_sequence_item(tcopy, 1u, &err);
    CHECK(kcopy.tag == IDM_VAL_CELL);
    CHECK(kcopy.as.obj != k.as.obj);
    IdmValue inner = idm_cell_get(kcopy, &err);
    CHECK(inner.as.obj == tcopy.as.obj);

    idm_runtime_destroy(&rt);
}

void run_substrate_suite(void) {
    test_values();
    test_value_copy();
    test_gc_deep_pair_chain();
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
    test_string_constant_nul_roundtrip();
    test_reader_depth_guard();
    test_serialize_depth_guard();
}
