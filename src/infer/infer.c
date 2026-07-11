#include "idiom/infer.h"
#include "idiom/common.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same_symbol(const IdmSymbol *a, const IdmSymbol *b) {
    return a && a == b;
}

static bool term_is_wildcard(const IdmTypeTerm *t) {
    return t && t->kind == IDM_TYPE_VAR && strcmp(idm_type_term_text(t), "_") == 0;
}

static bool term_is_flex(const IdmTypeTerm *t) {
    return t && t->kind == IDM_TYPE_VAR && !t->rigid && t->var_id != 0u && strcmp(idm_type_term_text(t), "_") != 0;
}

void idm_subst_init(IdmSubst *s) {
    memset(s, 0, sizeof(*s));
}

void idm_subst_destroy(IdmSubst *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) idm_type_term_destroy(&s->items[i].term);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static const IdmTypeTerm *subst_lookup(const IdmSubst *s, uint32_t var_id) {
    for (size_t i = 0; i < s->count; i++) {
        if (s->items[i].var_id == var_id) return &s->items[i].term;
    }
    return NULL;
}

static const IdmTypeTerm *resolve_head(const IdmSubst *s, const IdmTypeTerm *t) {
    while (term_is_flex(t)) {
        const IdmTypeTerm *b = subst_lookup(s, t->var_id);
        if (!b) return t;
        t = b;
    }
    return t;
}

bool idm_subst_apply(const IdmSubst *s, const IdmTypeTerm *in, IdmTypeTerm *out) {
    memset(out, 0, sizeof(*out));
    if (!in) return true;
    const IdmTypeTerm *h = resolve_head(s, in);
    if (h->kind == IDM_TYPE_VAR) return idm_type_term_copy(out, h);
    IdmTypeTerm *args = h->arg_count == 0 ? NULL : calloc(h->arg_count, sizeof(*args));
    if (h->arg_count != 0 && !args) return false;
    for (size_t i = 0; i < h->arg_count; i++) {
        if (!idm_subst_apply(s, &h->args[i], &args[i])) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
    }
    if (h->kind == IDM_TYPE_CON) {
        if (h->arg_count == 0) {
            free(args);
            return idm_type_con_symbol(out, h->symbol);
        }
        if (idm_type_con_take_symbol(out, h->symbol, args, h->arg_count)) return true;
    } else if (idm_type_compound(out, h->kind, args, h->arg_count)) {
        return true;
    }
    for (size_t i = 0; i < h->arg_count; i++) idm_type_term_destroy(&args[i]);
    free(args);
    return false;
}

bool idm_subst_occurs(const IdmSubst *s, uint32_t var_id, const IdmTypeTerm *term) {
    const IdmTypeTerm *h = resolve_head(s, term);
    if (term_is_flex(h)) return h->var_id == var_id;
    for (size_t i = 0; i < h->arg_count; i++) {
        if (idm_subst_occurs(s, var_id, &h->args[i])) return true;
    }
    return false;
}

static bool subst_bind(IdmSubst *s, uint32_t var_id, const IdmTypeTerm *term, IdmError *err, IdmSpan span) {
    IdmTypeTerm copy;
    if (!idm_type_term_copy(&copy, term)) return idm_error_oom(err, span);
    if (s->count == s->cap && !idm_grow((void **)&s->items, &s->cap, sizeof(*s->items), 8u, s->count + 1u)) {
        idm_type_term_destroy(&copy);
        return idm_error_oom(err, span);
    }
    IdmSubstEntry *e = &s->items[s->count];
    e->var_id = var_id;
    e->term = copy;
    s->count++;
    return true;
}

bool idm_subst_widen(IdmSubst *s, const IdmTypeTerm *var, const IdmTypeTerm *widened) {
    if (!term_is_flex(var)) return false;
    uint32_t id = var->var_id;
    for (;;) {
        IdmSubstEntry *entry = NULL;
        for (size_t i = 0; i < s->count; i++) {
            if (s->items[i].var_id == id) { entry = &s->items[i]; break; }
        }
        if (!entry) break;
        if (term_is_flex(&entry->term)) { id = entry->term.var_id; continue; }
        IdmTypeTerm copy;
        if (!idm_type_term_copy(&copy, widened)) return false;
        idm_type_term_destroy(&entry->term);
        entry->term = copy;
        return true;
    }
    IdmTypeTerm copy;
    if (!idm_type_term_copy(&copy, widened)) return false;
    if (s->count == s->cap && !idm_grow((void **)&s->items, &s->cap, sizeof(*s->items), 8u, s->count + 1u)) {
        idm_type_term_destroy(&copy);
        return false;
    }
    IdmSubstEntry *e = &s->items[s->count];
    e->var_id = id;
    e->term = copy;
    s->count++;
    return true;
}

static bool unify_fail(IdmError *err, IdmSpan span, const IdmTypeTerm *a, const IdmTypeTerm *b) {
    IdmBuffer ab;
    IdmBuffer bb;
    idm_buf_init(&ab);
    idm_buf_init(&bb);
    bool wa = idm_type_term_write(&ab, a);
    bool wb = idm_type_term_write(&bb, b);
    bool ok = idm_error_set(err, span, "cannot unify %s with %s", wa && ab.data ? ab.data : "?", wb && bb.data ? bb.data : "?");
    idm_buf_destroy(&ab);
    idm_buf_destroy(&bb);
    return ok;
}

static bool rigid_same(const IdmTypeTerm *a, const IdmTypeTerm *b) {
    if (a->var_id != 0u || b->var_id != 0u) return a->var_id == b->var_id;
    return same_symbol(a->symbol, b->symbol);
}

bool idm_unify(IdmSubst *s, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmError *err, IdmSpan span) {
    const IdmTypeTerm *ra = resolve_head(s, a);
    const IdmTypeTerm *rb = resolve_head(s, b);
    if (term_is_wildcard(ra) || term_is_wildcard(rb)) return true;
    if (term_is_flex(ra)) {
        if (term_is_flex(rb) && ra->var_id == rb->var_id) return true;
        if (idm_subst_occurs(s, ra->var_id, rb)) return unify_fail(err, span, ra, rb);
        return subst_bind(s, ra->var_id, rb, err, span);
    }
    if (term_is_flex(rb)) {
        if (idm_subst_occurs(s, rb->var_id, ra)) return unify_fail(err, span, ra, rb);
        return subst_bind(s, rb->var_id, ra, err, span);
    }
    if (ra->kind == IDM_TYPE_VAR || rb->kind == IDM_TYPE_VAR) {
        if (ra->kind == IDM_TYPE_VAR && rb->kind == IDM_TYPE_VAR && rigid_same(ra, rb)) return true;
        return unify_fail(err, span, ra, rb);
    }
    if (ra->kind != rb->kind) return unify_fail(err, span, ra, rb);
    if (ra->kind == IDM_TYPE_UNION) {
        if (idm_type_term_equal(ra, rb)) return true;
        return unify_fail(err, span, ra, rb);
    }
    if (ra->kind == IDM_TYPE_CON) {
        if (!same_symbol(ra->symbol, rb->symbol)) return unify_fail(err, span, ra, rb);
    }
    if (ra->arg_count != rb->arg_count) return unify_fail(err, span, ra, rb);
    for (size_t i = 0; i < ra->arg_count; i++) {
        if (!idm_unify(s, &ra->args[i], &rb->args[i], err, span)) return false;
    }
    return true;
}

void idm_constraint_set_init(IdmConstraintSet *cs) {
    memset(cs, 0, sizeof(*cs));
}

void idm_constraint_set_destroy(IdmConstraintSet *cs) {
    if (!cs) return;
    for (size_t i = 0; i < cs->count; i++) idm_constraint_destroy(&cs->items[i]);
    free(cs->items);
    memset(cs, 0, sizeof(*cs));
}

bool idm_constraint_set_add(IdmConstraintSet *cs, const IdmConstraint *c) {
    if (cs->count == cs->cap && !idm_grow((void **)&cs->items, &cs->cap, sizeof(*cs->items), 8u, cs->count + 1u)) return false;
    if (!idm_constraint_copy(&cs->items[cs->count], c)) return false;
    cs->count++;
    return true;
}

bool idm_constraint_set_add_eq(IdmConstraintSet *cs, const IdmTypeTerm *a, const IdmTypeTerm *b) {
    IdmConstraint c;
    memset(&c, 0, sizeof(c));
    c.kind = IDM_CONSTR_EQ;
    if (!idm_type_term_copy(&c.lhs, a) || !idm_type_term_copy(&c.rhs, b)) {
        idm_constraint_destroy(&c);
        return false;
    }
    bool ok = idm_constraint_set_add(cs, &c);
    idm_constraint_destroy(&c);
    return ok;
}

bool idm_constraint_set_add_class(IdmConstraintSet *cs, IdmSymbol *trait, const IdmTypeTerm *ty) {
    IdmConstraint c;
    memset(&c, 0, sizeof(c));
    c.kind = IDM_CONSTR_CLASS;
    c.trait = trait;
    if (!c.trait || !idm_type_term_copy(&c.lhs, ty)) {
        idm_constraint_destroy(&c);
        return false;
    }
    bool ok = idm_constraint_set_add(cs, &c);
    idm_constraint_destroy(&c);
    return ok;
}

static bool given_discharges(const IdmSubst *s, const IdmConstraintSet *given, IdmSymbol *trait, const IdmTypeTerm *ty, IdmGivenOracle oracle, void *oracle_user) {
    for (size_t i = 0; i < given->count; i++) {
        const IdmConstraint *g = &given->items[i];
        if (g->kind != IDM_CONSTR_CLASS) continue;
        if (oracle && oracle(oracle_user, g, trait, ty)) return true;
        if (g->trait != trait) continue;
        IdmTypeTerm gt;
        if (!idm_subst_apply(s, &g->lhs, &gt)) return false;
        bool same = idm_type_term_equal(&gt, ty);
        idm_type_term_destroy(&gt);
        if (same) return true;
    }
    return false;
}

static bool solve_class(IdmSubst *s, const IdmConstraintSet *given,
                        IdmSymbol *trait, const IdmTypeTerm *ty,
                        IdmGivenOracle given_oracle,
                        IdmInstanceOracle oracle, void *oracle_user,
                        IdmConstraintSet *residual, IdmError *err, IdmSpan span) {
    if (given && given_discharges(s, given, trait, ty, given_oracle, oracle_user)) return true;
    if (ty->kind == IDM_TYPE_VAR) {
        if (residual && !idm_constraint_set_add_class(residual, trait, ty)) return idm_error_oom(err, span);
        return true;
    }
    if (ty->kind == IDM_TYPE_UNION) {
        for (size_t i = 0; i < ty->arg_count; i++) {
            IdmError probe;
            idm_error_init(&probe);
            if (solve_class(s, given, trait, &ty->args[i], given_oracle, oracle, oracle_user, residual, &probe, span)) {
                idm_error_clear(&probe);
                return true;
            }
            idm_error_clear(&probe);
        }
    }
    IdmInstanceResult r = ty->kind == IDM_TYPE_UNION ? IDM_INST_NO
                        : oracle ? oracle(oracle_user, trait, ty) : IDM_INST_UNKNOWN;
    if (r == IDM_INST_NO) {
        IdmBuffer tb;
        idm_buf_init(&tb);
        bool w = idm_type_term_write(&tb, ty);
        const char *tr = trait ? idm_symbol_text(trait) : "?";
        int trn = (int)strcspn(tr, "#");
        bool set = tr[0] == '_' && tr[1] == '.'
            ? idm_error_set(err, span, "structural constraint '%s' is not satisfied by %s", tr, w && tb.data ? tb.data : "?")
            : idm_error_set(err, span, "typeclass '%.*s' is not implemented for %s", trn, tr, w && tb.data ? tb.data : "?");
        idm_buf_destroy(&tb);
        return set;
    }
    return true;
}

bool idm_solve(IdmSubst *s,
               const IdmConstraintSet *given,
               const IdmConstraintSet *wanted,
               IdmGivenOracle given_oracle,
               IdmInstanceOracle oracle, void *oracle_user,
               IdmConstraintSet *residual,
               IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < wanted->count; i++) {
        const IdmConstraint *c = &wanted->items[i];
        if (c->kind != IDM_CONSTR_EQ) continue;
        if (!idm_unify(s, &c->lhs, &c->rhs, err, span)) return false;
    }
    for (size_t i = 0; i < wanted->count; i++) {
        const IdmConstraint *c = &wanted->items[i];
        if (c->kind != IDM_CONSTR_CLASS) continue;
        IdmTypeTerm ty;
        if (!idm_subst_apply(s, &c->lhs, &ty)) return idm_error_oom(err, span);
        bool ok = solve_class(s, given, c->trait, &ty, given_oracle, oracle, oracle_user, residual, err, span);
        idm_type_term_destroy(&ty);
        if (!ok) return false;
    }
    return true;
}

static bool push_id(uint32_t **ids, size_t *count, size_t *cap, uint32_t id) {
    for (size_t i = 0; i < *count; i++) {
        if ((*ids)[i] == id) return true;
    }
    if (*count == *cap && !idm_grow((void **)ids, cap, sizeof(**ids), 8u, *count + 1u)) return false;
    (*ids)[(*count)++] = id;
    return true;
}

static bool id_in(const uint32_t *ids, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

static bool collect_flex(const IdmSubst *s, const IdmTypeTerm *term, const uint32_t *bound, size_t bound_count, uint32_t **out_ids, size_t *out_count, size_t *out_cap) {
    const IdmTypeTerm *h = resolve_head(s, term);
    if (term_is_flex(h)) {
        if (id_in(bound, bound_count, h->var_id)) return true;
        return push_id(out_ids, out_count, out_cap, h->var_id);
    }
    for (size_t i = 0; i < h->arg_count; i++) {
        if (!collect_flex(s, &h->args[i], bound, bound_count, out_ids, out_count, out_cap)) return false;
    }
    return true;
}

bool idm_free_flex_vars(const IdmSubst *s, const IdmTypeTerm *term,
                        const uint32_t *bound, size_t bound_count,
                        uint32_t **out_ids, size_t *out_count, size_t *out_cap) {
    return collect_flex(s, term, bound, bound_count, out_ids, out_count, out_cap);
}
