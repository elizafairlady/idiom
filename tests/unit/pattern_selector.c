#include "idiom/pattern.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *name) {
    fprintf(stderr, "pattern_selector: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static void check_node_limit(size_t got, size_t limit, const char *name) {
    if (got > limit) {
        fprintf(stderr, "pattern_selector: %s: %zu > %zu\n", name, got, limit);
        exit(1);
    }
}

static IdmPattern *tuple2_pattern(IdmValue a, IdmValue b) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmPattern **items = calloc(2u, sizeof(*items));
    if (!items) return NULL;
    items[0] = idm_pat_literal(a, span);
    items[1] = idm_pat_literal(b, span);
    if (!items[0] || !items[1]) {
        idm_pat_free(items[0]);
        idm_pat_free(items[1]);
        free(items);
        return NULL;
    }
    IdmPattern *pat = idm_pat_sequence(IDM_PAT_TUPLE, items, 2u, span);
    if (!pat) {
        idm_pat_free(items[0]);
        idm_pat_free(items[1]);
        free(items);
    }
    return pat;
}

static IdmPattern *dict_rest_pattern(IdmRuntime *rt) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmDictPatternEntry *entries = calloc(1u, sizeof(*entries));
    if (!entries) return NULL;
    entries[0].key = idm_atom(rt, "a");
    entries[0].pattern = idm_pat_bind("a", span);
    IdmPattern *rest = idm_pat_bind("rest", span);
    if (!entries[0].pattern || !rest) {
        idm_pat_free(entries[0].pattern);
        idm_pat_free(rest);
        free(entries);
        return NULL;
    }
    IdmPattern *pat = idm_pat_dict(entries, 1u, rest, span);
    if (!pat) {
        idm_pat_free(entries[0].pattern);
        idm_pat_free(rest);
        free(entries);
    }
    return pat;
}

static IdmPattern *vector_rest_pattern(size_t fixed_count) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmPattern **items = fixed_count == 0 ? NULL : calloc(fixed_count, sizeof(*items));
    if (fixed_count != 0 && !items) return NULL;
    for (size_t i = 0; i < fixed_count; i++) {
        items[i] = idm_pat_literal(idm_int((int64_t)i + 1), span);
        if (!items[i]) {
            for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
            free(items);
            return NULL;
        }
    }
    IdmPattern *rest = idm_pat_bind("rest", span);
    if (!rest) {
        for (size_t i = 0; i < fixed_count; i++) idm_pat_free(items[i]);
        free(items);
        return NULL;
    }
    IdmPattern *pat = idm_pat_sequence_rest(IDM_PAT_VECTOR_REST, items, fixed_count, rest, span);
    if (!pat) {
        for (size_t i = 0; i < fixed_count; i++) idm_pat_free(items[i]);
        idm_pat_free(rest);
        free(items);
    }
    return pat;
}

static IdmPattern *pair_bind_pattern(void) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmPattern *left = idm_pat_bind("head", span);
    IdmPattern *right = idm_pat_bind("tail", span);
    if (!left || !right) {
        idm_pat_free(left);
        idm_pat_free(right);
        return NULL;
    }
    IdmPattern *pat = idm_pat_pair(left, right, span);
    if (!pat) {
        idm_pat_free(left);
        idm_pat_free(right);
    }
    return pat;
}

static IdmPattern *syntax_list_pattern(void) {
    IdmSpan span = idm_span_unknown(NULL);
    IdmSyntaxPattern **items = calloc(1u, sizeof(*items));
    if (!items) return NULL;
    items[0] = idm_syn_pat_bind("head", span);
    if (!items[0]) {
        free(items);
        return NULL;
    }
    IdmSyntaxPattern *syn = idm_syn_pat_sequence(IDM_SYN_LIST, items, 1u, IDM_SYN_PAT_NO_REST, NULL, span);
    if (!syn) {
        idm_syn_pat_free(items[0]);
        free(items);
        return NULL;
    }
    IdmPattern *pat = idm_pat_syntax_take(syn, span);
    if (!pat) idm_syn_pat_free(syn);
    return pat;
}

static bool reject_first_guard(void *user, uint32_t function_index, const IdmPatternBindings *bindings, bool *out_pass, IdmError *err) {
    (void)user;
    (void)bindings;
    (void)err;
    *out_pass = function_index != 1u;
    return true;
}

static void select_one(IdmRuntime *rt, IdmPatternSelector *selector, IdmValue arg, IdmPatternGuardFn guard, uint32_t *out_fn, IdmPatternBindings *bindings, bool *out_has, IdmError *err) {
    bool matched = false;
    check(idm_pattern_selector_select(rt, selector, &arg, 1u, guard, NULL, out_fn, bindings, out_has, &matched, err), "select");
    check_error(err, "select error");
    check(matched, "matched");
}

static void test_tuple_dag(IdmRuntime *rt, IdmError *err) {
    IdmPattern *patterns[5] = {0};
    IdmPattern *args[5][1] = {{0}};
    IdmPatternSelectorClause clauses[5] = {0};
    IdmArity one = idm_arity_exact(1u);
    for (uint32_t i = 0; i < 4u; i++) {
        patterns[i] = tuple2_pattern(idm_int((int64_t)i + 1), idm_int(9));
        check(patterns[i] != NULL, "tuple pattern");
        args[i][0] = patterns[i];
        clauses[i].function_index = 10u + i;
        clauses[i].arity = one;
        clauses[i].patterns = args[i];
        clauses[i].pattern_count = 1u;
    }
    patterns[4] = idm_pat_wildcard(idm_span_unknown(NULL));
    check(patterns[4] != NULL, "fallback pattern");
    args[4][0] = patterns[4];
    clauses[4].function_index = 99u;
    clauses[4].arity = one;
    clauses[4].patterns = args[4];
    clauses[4].pattern_count = 1u;
    clauses[4].trivial_match = true;

    IdmPatternSelector *selector = NULL;
    check(idm_pattern_selector_build(rt, clauses, 5u, &selector, err), "build tuple selector");
    check_error(err, "build tuple selector error");
    for (size_t i = 0; i < 5u; i++) idm_pat_free(patterns[i]);

    check_node_limit(idm_pattern_selector_node_count(selector), 15u, "tuple DAG node count");

    IdmValue tuple_items[2] = { idm_int(3), idm_int(9) };
    IdmValue arg = idm_tuple(rt, tuple_items, 2u, err);
    check_error(err, "tuple value error");
    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    uint32_t fn = 0;
    bool has = true;
    select_one(rt, selector, arg, NULL, &fn, &bindings, &has, err);
    check(fn == 12u && !has, "tuple branch");
    idm_pattern_bindings_destroy(&bindings);

    tuple_items[0] = idm_int(7);
    arg = idm_tuple(rt, tuple_items, 2u, err);
    check_error(err, "fallback tuple value error");
    idm_pattern_bindings_init(&bindings);
    has = true;
    select_one(rt, selector, arg, NULL, &fn, &bindings, &has, err);
    check(fn == 99u && !has, "tuple fallback");
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
}

static void test_rest_cache(IdmRuntime *rt, IdmError *err) {
    IdmPattern *patterns[2] = { dict_rest_pattern(rt), dict_rest_pattern(rt) };
    check(patterns[0] && patterns[1], "dict rest pattern");
    IdmPattern *args[2][1] = {{ patterns[0] }, { patterns[1] }};
    IdmPatternSelectorClause clauses[2] = {0};
    clauses[0].function_index = 1u;
    clauses[0].arity = idm_arity_exact(1u);
    clauses[0].patterns = args[0];
    clauses[0].pattern_count = 1u;
    clauses[0].has_guard = true;
    clauses[1].function_index = 2u;
    clauses[1].arity = idm_arity_exact(1u);
    clauses[1].patterns = args[1];
    clauses[1].pattern_count = 1u;

    IdmPatternSelector *selector = NULL;
    check(idm_pattern_selector_build(rt, clauses, 2u, &selector, err), "build rest selector");
    check_error(err, "build rest selector error");
    idm_pat_free(patterns[0]);
    idm_pat_free(patterns[1]);

    IdmDictEntry entries[2] = {
        { idm_atom(rt, "a"), idm_int(11) },
        { idm_atom(rt, "b"), idm_int(22) }
    };
    IdmValue arg = idm_dict(rt, entries, 2u, err);
    check_error(err, "dict value error");

    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    size_t before = idm_heap_object_count(&rt->immortal);
    uint32_t fn = 0;
    bool has = false;
    select_one(rt, selector, arg, reject_first_guard, &fn, &bindings, &has, err);
    size_t after = idm_heap_object_count(&rt->immortal);
    check(fn == 2u && has, "guard retry result");
    check(after > before, "rest access allocated once");
    size_t growth = after - before;
    IdmPatternBindings again;
    idm_pattern_bindings_init(&again);
    uint32_t fn2 = 0;
    bool has2 = false;
    select_one(rt, selector, arg, reject_first_guard, &fn2, &again, &has2, err);
    size_t after2 = idm_heap_object_count(&rt->immortal);
    check(after2 - after <= growth, "rest access cached");
    idm_pattern_bindings_destroy(&again);

    const IdmValue *a = idm_pattern_bindings_get(&bindings, "a");
    const IdmValue *rest = idm_pattern_bindings_get(&bindings, "rest");
    check(a && idm_value_tag(*a) == IDM_VAL_INT && idm_int_value(*a) == 11, "dict bind");
    check(rest && idm_is_dict(*rest) && idm_dict_count(*rest) == 1u, "dict rest");
    IdmValue b = idm_nil();
    check(idm_dict_get(*rest, idm_atom(rt, "b"), &b), "dict rest key");
    check(idm_value_tag(b) == IDM_VAL_INT && idm_int_value(b) == 22, "dict rest value");

    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
}

static void test_vector_rest_overlap(IdmRuntime *rt, IdmError *err) {
    IdmPattern *patterns[2] = { vector_rest_pattern(2u), vector_rest_pattern(1u) };
    check(patterns[0] && patterns[1], "vector rest patterns");
    IdmPattern *args[2][1] = {{ patterns[0] }, { patterns[1] }};
    IdmPatternSelectorClause clauses[2] = {0};
    clauses[0].function_index = 1u;
    clauses[0].arity = idm_arity_exact(1u);
    clauses[0].patterns = args[0];
    clauses[0].pattern_count = 1u;
    clauses[0].has_guard = true;
    clauses[1].function_index = 2u;
    clauses[1].arity = idm_arity_exact(1u);
    clauses[1].patterns = args[1];
    clauses[1].pattern_count = 1u;

    IdmPatternSelector *selector = NULL;
    check(idm_pattern_selector_build(rt, clauses, 2u, &selector, err), "build vector rest selector");
    check_error(err, "build vector rest selector error");
    idm_pat_free(patterns[0]);
    idm_pat_free(patterns[1]);

    IdmValue items[3] = { idm_int(1), idm_int(2), idm_int(3) };
    IdmValue arg = idm_vector(rt, items, 3u, err);
    check_error(err, "vector value error");

    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    uint32_t fn = 0;
    bool has = false;
    select_one(rt, selector, arg, reject_first_guard, &fn, &bindings, &has, err);
    check(fn == 2u && has, "vector rest overlap retry");
    const IdmValue *rest = idm_pattern_bindings_get(&bindings, "rest");
    check(rest && idm_is_vector(*rest) && idm_sequence_count(*rest) == 2u, "vector rest bind");
    IdmValue first = idm_sequence_item(*rest, 0u, err);
    check_error(err, "vector rest item error");
    check(idm_value_tag(first) == IDM_VAL_INT && idm_int_value(first) == 2, "vector rest first");
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
}

static void test_list_overtype_overlap(IdmRuntime *rt, IdmError *err) {
    IdmPattern *patterns[2] = { idm_pat_type("list", idm_span_unknown(NULL)), pair_bind_pattern() };
    check(patterns[0] && patterns[1], "list overtype patterns");
    IdmPattern *args[2][1] = {{ patterns[0] }, { patterns[1] }};
    IdmPatternSelectorClause clauses[2] = {0};
    clauses[0].function_index = 1u;
    clauses[0].arity = idm_arity_exact(1u);
    clauses[0].patterns = args[0];
    clauses[0].pattern_count = 1u;
    clauses[0].has_guard = true;
    clauses[1].function_index = 2u;
    clauses[1].arity = idm_arity_exact(1u);
    clauses[1].patterns = args[1];
    clauses[1].pattern_count = 1u;

    IdmPatternSelector *selector = NULL;
    check(idm_pattern_selector_build(rt, clauses, 2u, &selector, err), "build list overtype selector");
    check_error(err, "build list overtype selector error");
    idm_pat_free(patterns[0]);
    idm_pat_free(patterns[1]);

    IdmValue arg = idm_cons(rt, idm_int(5), idm_empty_list(), err);
    check_error(err, "pair value error");

    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    uint32_t fn = 0;
    bool has = false;
    select_one(rt, selector, arg, reject_first_guard, &fn, &bindings, &has, err);
    check(fn == 2u && has, "list overtype overlap retry");
    const IdmValue *head = idm_pattern_bindings_get(&bindings, "head");
    check(head && idm_value_tag(*head) == IDM_VAL_INT && idm_int_value(*head) == 5, "pair head bind");
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
}

static void test_syntax_mixed_switch(IdmRuntime *rt, IdmError *err) {
    IdmPattern *patterns[2] = { syntax_list_pattern(), idm_pat_type("syntax", idm_span_unknown(NULL)) };
    check(patterns[0] && patterns[1], "syntax mixed patterns");
    IdmPattern *args[2][1] = {{ patterns[0] }, { patterns[1] }};
    IdmPatternSelectorClause clauses[2] = {0};
    clauses[0].function_index = 1u;
    clauses[0].arity = idm_arity_exact(1u);
    clauses[0].patterns = args[0];
    clauses[0].pattern_count = 1u;
    clauses[1].function_index = 2u;
    clauses[1].arity = idm_arity_exact(1u);
    clauses[1].patterns = args[1];
    clauses[1].pattern_count = 1u;

    IdmPatternSelector *selector = NULL;
    check(idm_pattern_selector_build(rt, clauses, 2u, &selector, err), "build syntax mixed selector");
    check_error(err, "build syntax mixed selector error");
    idm_pat_free(patterns[0]);
    idm_pat_free(patterns[1]);

    IdmSyntax *syn = idm_syn_atom("x", idm_span_unknown(NULL));
    check(syn != NULL, "syntax atom");
    IdmValue arg = idm_syntax_value(rt, syn, err);
    idm_syn_free(syn);
    check_error(err, "syntax value error");

    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    uint32_t fn = 0;
    bool has = true;
    select_one(rt, selector, arg, NULL, &fn, &bindings, &has, err);
    check(fn == 2u && !has, "syntax mixed type fallback");
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
}

int idm_unit_pattern_selector(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    test_tuple_dag(&rt, &err);
    test_rest_cache(&rt, &err);
    test_vector_rest_overlap(&rt, &err);
    test_list_overtype_overlap(&rt, &err);
    test_syntax_mixed_switch(&rt, &err);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    return 0;
}
