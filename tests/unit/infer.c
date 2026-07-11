#include "idiom/infer.h"
#include "idiom/common.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static IdmRuntime *test_rt;
static void check(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "infer test failed: %s\n", msg); failures++; }
}

static IdmTypeTerm con(const char *name) {
    IdmTypeTerm t;
    idm_type_con(test_rt, &t, name);
    return t;
}
static IdmTypeTerm flex(uint32_t id, const char *name) {
    IdmTypeTerm t;
    idm_type_var(test_rt, &t, name, id, false);
    return t;
}
static IdmTypeTerm rigid(uint32_t id, const char *name) {
    IdmTypeTerm t;
    idm_type_var(test_rt, &t, name, id, true);
    return t;
}
static IdmTypeTerm app1(const char *name, IdmTypeTerm a) {
    IdmTypeTerm *args = calloc(1, sizeof(*args));
    args[0] = a;
    IdmTypeTerm t;
    idm_type_con_take(test_rt, &t, name, args, 1u);
    return t;
}
static IdmTypeTerm tup2(IdmTypeTerm a, IdmTypeTerm b) {
    IdmTypeTerm *args = calloc(2, sizeof(*args));
    args[0] = a;
    args[1] = b;
    IdmTypeTerm t;
    idm_type_compound(&t, IDM_TYPE_TUPLE, args, 2u);
    return t;
}

static bool applies_to_con(IdmSubst *s, IdmTypeTerm v, const char *name) {
    IdmTypeTerm out;
    idm_subst_apply(s, &v, &out);
    bool ok = out.kind == IDM_TYPE_CON && out.symbol && strcmp(idm_type_term_text(&out), name) == 0;
    idm_type_term_destroy(&out);
    return ok;
}

static bool try_unify(IdmTypeTerm a, IdmTypeTerm b) {
    IdmSubst s;
    idm_subst_init(&s);
    IdmError err;
    idm_error_init(&err);
    bool ok = idm_unify(&s, &a, &b, &err, idm_span_unknown(NULL));
    idm_error_clear(&err);
    idm_subst_destroy(&s);
    idm_type_term_destroy(&a);
    idm_type_term_destroy(&b);
    return ok;
}

static IdmInstanceResult oracle(void *user, IdmSymbol *trait_symbol, const IdmTypeTerm *ty) {
    (void)user;
    const char *trait = idm_symbol_text(trait_symbol);
    if (strcmp(trait, "Show") == 0) {
        if (ty->kind == IDM_TYPE_CON && strcmp(idm_type_term_text(ty), "int") == 0) return IDM_INST_YES;
        if (ty->kind == IDM_TYPE_CON && strcmp(idm_type_term_text(ty), "string") == 0) return IDM_INST_YES;
        return IDM_INST_NO;
    }
    return IDM_INST_UNKNOWN;
}

int idm_unit_infer(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    test_rt = &rt;
    IdmSymbol *show = idm_intern(&rt.intern, IDM_SYMBOL_ATOM, "Show");
    check(show != NULL, "intern Show trait");
    check(try_unify(con("int"), con("int")), "int~int");
    check(!try_unify(con("int"), con("string")), "int~string fails");
    check(try_unify(app1("Box", con("int")), app1("Box", flex(1, "a"))), "Box int ~ Box a");
    check(!try_unify(app1("Box", con("int")), app1("List", con("int"))), "Box~List fails");
    check(!try_unify(flex(1, "a"), app1("Box", flex(1, "a"))), "occurs check");
    check(try_unify(flex(0, "_"), con("int")), "wildcard unifies");
    check(!try_unify(rigid(1, "a"), con("int")), "rigid var ~ int fails");
    check(try_unify(rigid(1, "a"), rigid(1, "a")), "rigid var ~ same rigid ok");
    check(try_unify(rigid(1, "a"), flex(2, "b")), "flex binds to rigid");

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmTypeTerm a = flex(1, "a");
        IdmTypeTerm b = flex(2, "b");
        check(idm_unify(&s, &a, &b, &err, idm_span_unknown(NULL)), "a~b");
        IdmTypeTerm i = con("int");
        check(idm_unify(&s, &a, &i, &err, idm_span_unknown(NULL)), "a~int");
        check(applies_to_con(&s, b, "int"), "b resolves to int transitively");
        idm_type_term_destroy(&a);
        idm_type_term_destroy(&b);
        idm_type_term_destroy(&i);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmTypeTerm t1 = tup2(flex(1, "a"), con("int"));
        IdmTypeTerm t2 = tup2(con("bool"), flex(2, "b"));
        check(idm_unify(&s, &t1, &t2, &err, idm_span_unknown(NULL)), "tuple unify");
        IdmTypeTerm a = flex(1, "a");
        IdmTypeTerm b = flex(2, "b");
        check(applies_to_con(&s, a, "bool"), "tuple: a=bool");
        check(applies_to_con(&s, b, "int"), "tuple: b=int");
        idm_type_term_destroy(&a);
        idm_type_term_destroy(&b);
        idm_type_term_destroy(&t1);
        idm_type_term_destroy(&t2);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmConstraintSet want;
        idm_constraint_set_init(&want);
        IdmTypeTerm a = flex(1, "a");
        IdmTypeTerm i = con("int");
        idm_constraint_set_add_eq(&want, &a, &i);
        idm_constraint_set_add_class(&want, show, &a);
        IdmConstraintSet resid;
        idm_constraint_set_init(&resid);
        check(idm_solve(&s, NULL, &want, NULL, oracle, NULL, &resid, &err, idm_span_unknown(NULL)), "solve eq+class");
        check(resid.count == 0, "Show int discharged, no residual");
        idm_type_term_destroy(&a);
        idm_type_term_destroy(&i);
        idm_constraint_set_destroy(&want);
        idm_constraint_set_destroy(&resid);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmConstraintSet want;
        idm_constraint_set_init(&want);
        IdmTypeTerm b = con("bool");
        idm_constraint_set_add_class(&want, show, &b);
        check(!idm_solve(&s, NULL, &want, NULL, oracle, NULL, NULL, &err, idm_span_unknown(NULL)), "Show bool rejected");
        check(err.present, "Show bool sets error");
        idm_type_term_destroy(&b);
        idm_constraint_set_destroy(&want);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmConstraintSet want;
        idm_constraint_set_init(&want);
        IdmTypeTerm a = flex(1, "a");
        idm_constraint_set_add_class(&want, show, &a);
        IdmConstraintSet resid;
        idm_constraint_set_init(&resid);
        check(idm_solve(&s, NULL, &want, NULL, oracle, NULL, &resid, &err, idm_span_unknown(NULL)), "solve free class");
        check(resid.count == 1 && resid.items[0].kind == IDM_CONSTR_CLASS, "free class becomes residual");
        idm_type_term_destroy(&a);
        idm_constraint_set_destroy(&want);
        idm_constraint_set_destroy(&resid);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmConstraintSet given;
        idm_constraint_set_init(&given);
        IdmConstraintSet want;
        idm_constraint_set_init(&want);
        IdmTypeTerm r = rigid(5, "a");
        idm_constraint_set_add_class(&given, show, &r);
        idm_constraint_set_add_class(&want, show, &r);
        IdmConstraintSet resid;
        idm_constraint_set_init(&resid);
        check(idm_solve(&s, &given, &want, NULL, oracle, NULL, &resid, &err, idm_span_unknown(NULL)), "solve given class");
        check(resid.count == 0, "given discharges wanted over rigid var");
        idm_type_term_destroy(&r);
        idm_constraint_set_destroy(&given);
        idm_constraint_set_destroy(&want);
        idm_constraint_set_destroy(&resid);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        IdmError err;
        idm_error_init(&err);
        IdmTypeTerm boxa = app1("Box", flex(1, "a"));
        IdmTypeTerm boxint = app1("Box", con("int"));
        idm_unify(&s, &boxa, &boxint, &err, idm_span_unknown(NULL));
        uint32_t *ids = NULL;
        size_t n = 0;
        size_t cap = 0;
        IdmTypeTerm a = flex(1, "a");
        idm_free_flex_vars(&s, &a, NULL, 0, &ids, &n, &cap);
        check(n == 0, "a resolved to int has no free flex vars");
        free(ids);
        idm_type_term_destroy(&a);
        idm_type_term_destroy(&boxa);
        idm_type_term_destroy(&boxint);
        idm_error_clear(&err);
        idm_subst_destroy(&s);
    }

    {
        IdmSubst s;
        idm_subst_init(&s);
        uint32_t *ids = NULL;
        size_t n = 0;
        size_t cap = 0;
        IdmTypeTerm pair = tup2(flex(1, "a"), flex(2, "b"));
        uint32_t bound[1] = {1u};
        idm_free_flex_vars(&s, &pair, bound, 1u, &ids, &n, &cap);
        check(n == 1 && ids[0] == 2u, "free vars excludes bound env var");
        free(ids);
        idm_type_term_destroy(&pair);
        idm_subst_destroy(&s);
    }

    idm_runtime_destroy(&rt);
    test_rt = NULL;
    return failures;
}
