#include "ish/common.h"
#include "ish/core.h"
#include "ish/expand.h"
#include "ish/reader.h"
#include "ish/scope.h"
#include "ish/value.h"
#include "ish/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define CHECK_STR(actual, expected) do { if (strcmp((actual), (expected)) != 0) { fprintf(stderr, "CHECK_STR failed at %s:%d:\nactual:   %s\nexpected: %s\n", __FILE__, __LINE__, (actual), (expected)); failures++; } } while (0)

static char *dump_reader(const char *src) {
    IshError err;
    ish_error_init(&err);
    IshSyntax *program = NULL;
    if (!ish_reader_read_string("<test>", src, &program, &err)) {
        ish_error_fprint(stderr, &err);
        ish_error_clear(&err);
        return NULL;
    }
    IshBuffer buf;
    ish_buf_init(&buf);
    CHECK(ish_syn_dump(&buf, program));
    ish_syn_free(program);
    return ish_buf_take(&buf);
}

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
}

static void test_reader_shell_protocols(void) {
    char *s = dump_reader("grep -c *.c > out.txt\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr grep (%-word \"-c\") (%-word \"*.c\") (%-redirect > (%-word \"out.txt\"))))");
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
    CHECK(ish_binding_table_add(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &empty, 10, &root));
    CHECK(ish_binding_table_add(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &one, 20, &inner));
    const IshBinding *binding = NULL;
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, &empty, &binding) == ISH_RESOLVE_OK);
    CHECK(binding && binding->id == root && binding->payload == 10);
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_DEFAULT, &one, &binding) == ISH_RESOLVE_OK);
    CHECK(binding && binding->id == inner && binding->payload == 20);
    CHECK(ish_binding_resolve(&table, "x", 1, ISH_BIND_SPACE_DEFAULT, &one, &binding) == ISH_RESOLVE_UNBOUND);
    CHECK(ish_binding_resolve(&table, "x", 0, ISH_BIND_SPACE_PACKAGE, &one, &binding) == ISH_RESOLVE_UNBOUND);

    CHECK(ish_binding_table_add(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &one, 1, NULL));
    CHECK(ish_binding_table_add(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &two, 2, NULL));
    CHECK(ish_binding_resolve(&table, "amb", 0, ISH_BIND_SPACE_DEFAULT, &both, &binding) == ISH_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);

    CHECK(ish_binding_table_add(&table, "op", 0, ISH_BIND_SPACE_OPERATOR, ISH_BIND_OPERATOR, &empty, 99, NULL));
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
    CHECK(ish_core_fn_add_capture(fn, 0));
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

static void test_minimal_expander_subset(void) {
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
    CHECK(core == NULL);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_operator_subset(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<operator-expand-test>", "1 + 2 * 3\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(app (prim add) 1 (app (prim mul) 2 3))");
    ish_buf_destroy(&dump);
    IshBytecodeModule module;
    ish_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    IshValue out = ish_nil();
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 7);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<operator-expand-test>", "x = 2\nx * 20 + 2\n", &core, &err));
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

static void test_source_fn_subset(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<fn-expand-test>", "inc = fn x -> add x 1\ninc 41\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(bind-local 0 (fn <lambda>/1 (app (prim add) (arg 0) 1)) (app (local 0) 41))");
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
    CHECK(ish_expand_string(&rt, "<fn-expand-test>", "inc = fn x do add x 1 end\ninc 41\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<fn-expand-test>", "x = 1\nf = fn y -> add x y\nf 2\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 3);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    CHECK(!ish_expand_string(&rt, "<fn-expand-test>", "bad = fn x x -> x\nbad 1\n", &core, &err));
    CHECK(err.present);
    ish_error_clear(&err);
    ish_runtime_destroy(&rt);
}

static void test_source_defn_letrec(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(!ish_expand_string(&rt, "<definition-expand-test>", "def inc x -> add x 1\ninc 41\n", &core, &err));
    CHECK(err.present);
    CHECK(core == NULL);
    ish_error_clear(&err);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<definition-expand-test>", "defn inc x -> x + 1\ninc 41\n", &core, &err));
    IshBuffer dump;
    ish_buf_init(&dump);
    CHECK(ish_core_dump(&dump, core));
    CHECK_STR(dump.data, "(letrec ((inc #0 (fn inc/1 (app (prim add) (arg 0) 1)))) (app (local 0) 41))");
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
    CHECK(ish_expand_string(&rt, "<definition-expand-test>", "defn f x -> g x\ndefn g x -> x + 1\nf 41\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<definition-expand-test>", "defn f 0 -> 40\ndefn f n -> n\nadd (f 0) 2\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<definition-expand-test>", "defn f 0 -> 0\ndefn f n -> n\nf 7\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 7);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<definition-expand-test>", "defn sumdown n -> cond (n < 1) 0 (n + sumdown (n - 1))\nsumdown 5\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_runtime_destroy(&rt);
}

static void test_source_match_subset(void) {
    IshRuntime rt;
    ish_runtime_init(&rt);
    IshError err;
    ish_error_init(&err);
    IshCore *core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match 0 do\n  0 -> 42\n  n -> n\nend\n", &core, &err));
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
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match 7 do\n  0 -> 0\n  n -> n\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 7);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "x = 40\nmatch 2 do\n  n -> x + n\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match [1 2] do\n  [1 2] -> 42\n  _ -> 0\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match %[1 2] do\n  %[1 2] -> 42\n  _ -> 0\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match %[1 2 3] do\n  %[h . t] -> t\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    IshBuffer vec_buf;
    ish_buf_init(&vec_buf);
    CHECK(ish_value_write(&vec_buf, out));
    CHECK_STR(vec_buf.data, "%[2 3]");
    ish_buf_destroy(&vec_buf);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match {:ok 42} do\n  {:ok 42} -> 42\n  _ -> 0\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match {1 2 3} do\n  {h . t} -> t\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    IshBuffer tuple_buf;
    ish_buf_init(&tuple_buf);
    CHECK(ish_value_write(&tuple_buf, out));
    CHECK_STR(tuple_buf.data, "{2 3}");
    ish_buf_destroy(&tuple_buf);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match [1 2] do\n  [h t] -> h\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 1);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match [1 2 3] do\n  [h . t] -> h\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 1);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match [1 2 3] do\n  [h x . t] -> x\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 2);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "defn head [h . t] -> h\nhead [42 0]\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match %{:a 42 :b 0} do\n  %{:a 42} -> 42\n  _ -> 0\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match %{:a 42} do\n  %{:a x} -> x\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match {:ok \"no\"} do\n  {:ok x} when x < 0 -> 1\n  {:ok x} -> 42\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "defn f n when n < 0 -> 0\ndefn f n -> n\nf 42\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "defn f n when n == 42 -> 42\ndefn f n -> 0\nf 42\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "f = fn x when x == 42 -> x\nf 42\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == ISH_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    ish_bc_destroy(&module);
    ish_core_free(core);

    core = NULL;
    CHECK(ish_expand_string(&rt, "<match-expand-test>", "match 1 do\n  0 -> 0\nend\n", &core, &err));
    ish_bc_init(&module);
    CHECK(ish_core_compile_main(core, &module, &main_fn, &err));
    CHECK(!ish_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(err.present);
    ish_error_clear(&err);
    ish_bc_destroy(&module);
    ish_core_free(core);
    ish_runtime_destroy(&rt);
}

int main(void) {
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
    test_minimal_expander_subset();
    test_source_operator_subset();
    test_source_fn_subset();
    test_source_defn_letrec();
    test_source_match_subset();
    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("unit tests passed");
    return 0;
}
