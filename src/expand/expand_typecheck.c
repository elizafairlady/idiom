#include "internal.h"
#include "idiom/infer.h"

typedef struct {
    uint32_t slot;
    IdmTypeTerm term;
} SlotType;

typedef struct TypeEnv TypeEnv;
struct TypeEnv {
    const TypeEnv *parent;
    const IdmCallableContract *givens;
    SlotType *args;
    size_t arg_count;
    size_t arg_cap;
    SlotType *locals;
    size_t local_count;
    size_t local_cap;
};

typedef struct {
    const char *name;
    uint32_t slot;
    const IdmCallableContract *contract;
} SiblingContract;

typedef struct {
    ExpandContext *ctx;
    IdmSubst subst;
    IdmConstraintSet wanted;
    IdmConstraintSet given;
    IdmTypeCmp cmp;
    const char *owner;
    const SiblingContract *siblings;
    size_t sibling_count;
    uint32_t widen_lo;
    uint32_t widen_hi;
} GenCtx;

static const IdmCallableContract *sibling_contract(const GenCtx *g, const IdmCore *ref) {
    if (!g->siblings || !ref) return NULL;
    const char *name = NULL;
    uint32_t slot = 0;
    switch (ref->kind) {
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_ENV_REF:
            name = ref->as.slot_ref.name;
            slot = ref->as.slot_ref.slot;
            break;
        case IDM_CORE_PACKAGE_REF:
            name = ref->as.package_ref.name;
            slot = ref->as.package_ref.slot;
            break;
        default:
            return NULL;
    }
    if (!name) return NULL;
    for (size_t i = 0; i < g->sibling_count; i++) {
        if (g->siblings[i].slot == slot && g->siblings[i].name && strcmp(g->siblings[i].name, name) == 0) return g->siblings[i].contract;
    }
    return NULL;
}

static bool cmp_name_eq(void *user, const char *a, const char *b) {
    return type_name_same((ExpandContext *)user, a, b);
}
static bool cmp_trait_eq(void *user, const char *a, const char *b) {
    return typeclass_same_trait((ExpandContext *)user, a, b);
}
static bool cmp_given_matches(void *user, const IdmConstraint *given, const char *trait, const IdmTypeTerm *lhs) {
    return typeclass_given_matches((ExpandContext *)user, given, trait, lhs);
}

static void type_env_destroy(TypeEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->arg_count; i++) idm_type_term_destroy(&env->args[i].term);
    for (size_t i = 0; i < env->local_count; i++) idm_type_term_destroy(&env->locals[i].term);
    free(env->args);
    free(env->locals);
    memset(env, 0, sizeof(*env));
}

static bool slot_types_set(SlotType **items, size_t *count, size_t *cap, uint32_t slot, const IdmTypeTerm *term) {
    for (size_t i = 0; i < *count; i++) {
        if ((*items)[i].slot != slot) continue;
        IdmTypeTerm copy;
        memset(&copy, 0, sizeof(copy));
        if (!idm_type_term_copy(&copy, term)) return false;
        idm_type_term_destroy(&(*items)[i].term);
        (*items)[i].term = copy;
        return true;
    }
    if (*count == *cap && !idm_grow((void **)items, cap, sizeof(**items), 4u, *count + 1u)) return false;
    (*items)[*count].slot = slot;
    memset(&(*items)[*count].term, 0, sizeof((*items)[*count].term));
    if (!idm_type_term_copy(&(*items)[*count].term, term)) return false;
    (*count)++;
    return true;
}

static bool type_env_set_arg(TypeEnv *env, uint32_t slot, const IdmTypeTerm *term) {
    return slot_types_set(&env->args, &env->arg_count, &env->arg_cap, slot, term);
}
static bool type_env_set_local(TypeEnv *env, uint32_t slot, const IdmTypeTerm *term) {
    return slot_types_set(&env->locals, &env->local_count, &env->local_cap, slot, term);
}
static bool type_env_lookup(const SlotType *items, size_t count, uint32_t slot, IdmTypeTerm *out) {
    for (size_t i = count; i > 0; i--) {
        if (items[i - 1u].slot == slot) return idm_type_term_copy(out, &items[i - 1u].term);
    }
    return false;
}
static bool type_env_lookup_arg(const TypeEnv *env, uint32_t slot, IdmTypeTerm *out) {
    for (const TypeEnv *e = env; e; e = e->parent) {
        if (type_env_lookup(e->args, e->arg_count, slot, out)) return true;
    }
    return false;
}
static bool type_env_lookup_local(const TypeEnv *env, uint32_t slot, IdmTypeTerm *out) {
    for (const TypeEnv *e = env; e; e = e->parent) {
        if (type_env_lookup(e->locals, e->local_count, slot, out)) return true;
    }
    return false;
}

static bool fresh_var(GenCtx *g, IdmTypeTerm *out) {
    if (g->ctx->type_var_seq >= 0x7fffffffu) { memset(out, 0, sizeof(*out)); return false; }
    g->ctx->type_var_seq++;
    return idm_type_var(out, "t", 0x80000000u | g->ctx->type_var_seq, false);
}

static bool type_con_term(const char *name, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    return idm_type_con(out, name) || idm_error_oom(err, span);
}

static bool type_from_literal(GenCtx *g, IdmValue value, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    (void)g;
    if (idm_is_nil(value)) return type_con_term("empty-list", out, err, span);
    IdmValueTag tag = idm_value_tag(value);
    const char *name = tag == IDM_VAL_BIGNUM ? "int" : idm_value_type_name(tag);
    return type_con_term(name, out, err, span);
}

static IdmInstanceResult ctx_instance_oracle(void *user, const char *trait, const IdmTypeTerm *ty) {
    ExpandContext *ctx = user;
    if (ty && ty->kind == IDM_TYPE_CON && ty->name && strcmp(ty->name, "Any") == 0) return IDM_INST_YES;
    const TraitDef *td = trait_by_constraint_name(ctx, trait);
    if (!td) return IDM_INST_UNKNOWN;
    return trait_impl_satisfies_term(ctx, td, ty) ? IDM_INST_YES : IDM_INST_NO;
}

static const char *canonical_trait(GenCtx *g, const char *trait) {
    const TraitDef *td = trait_by_constraint_name(g->ctx, trait);
    const char *id = td ? trait_def_identity_text(td) : NULL;
    return id ? id : trait;
}

typedef struct {
    uint32_t *items;
    size_t count;
    size_t cap;
} FreshMap;

static bool fresh_map_get(GenCtx *g, FreshMap *seen, FreshMap *repl, uint32_t src_id, uint32_t *out) {
    for (size_t i = 0; i < seen->count; i++) {
        if (seen->items[i] == src_id) { *out = repl->items[i]; return true; }
    }
    if (g->ctx->type_var_seq >= 0x7fffffffu) return false;
    g->ctx->type_var_seq++;
    uint32_t nv = 0x80000000u | g->ctx->type_var_seq;
    if (seen->count == seen->cap && !idm_grow((void **)&seen->items, &seen->cap, sizeof(*seen->items), 8u, seen->count + 1u)) return false;
    if (repl->count == repl->cap && !idm_grow((void **)&repl->items, &repl->cap, sizeof(*repl->items), 8u, repl->count + 1u)) return false;
    seen->items[seen->count++] = src_id;
    repl->items[repl->count++] = nv;
    *out = nv;
    return true;
}

static bool contract_quantifies(const IdmCallableContract *c, const IdmTypeTerm *v) {
    if (!c || !v || v->kind != IDM_TYPE_VAR || !v->name) return false;
    for (size_t i = 0; i < c->quantified_count; i++) {
        if (c->quantified[i] && strcmp(c->quantified[i], v->name) == 0) return true;
    }
    return v->var_id != 0u && v->var_id <= c->quantified_count;
}

static bool inst_term(GenCtx *g, const IdmCallableContract *owner, FreshMap *seen, FreshMap *repl, const IdmTypeTerm *src, IdmTypeTerm *out) {
    memset(out, 0, sizeof(*out));
    if (!src) return true;
    if (src->kind == IDM_TYPE_VAR) {
        if (contract_quantifies(owner, src)) {
            uint32_t nv = 0;
            if (!fresh_map_get(g, seen, repl, src->var_id ? src->var_id : (uint32_t)(uintptr_t)src->name, &nv)) return false;
            return idm_type_var(out, src->name ? src->name : "t", nv, false);
        }
        return idm_type_term_copy(out, src);
    }
    IdmTypeTerm *args = src->arg_count ? calloc(src->arg_count, sizeof(*args)) : NULL;
    if (src->arg_count && !args) return false;
    for (size_t i = 0; i < src->arg_count; i++) {
        if (!inst_term(g, owner, seen, repl, &src->args[i], &args[i])) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
    }
    if (src->kind == IDM_TYPE_CON) {
        if (src->arg_count == 0) { free(args); return idm_type_con(out, src->name); }
        if (idm_type_con_take(out, src->name, args, src->arg_count)) return true;
    } else if (idm_type_compound(out, src->kind, args, src->arg_count)) {
        return true;
    }
    for (size_t i = 0; i < src->arg_count; i++) idm_type_term_destroy(&args[i]);
    free(args);
    return false;
}

static bool rigidify_term(const IdmCallableContract *c, const IdmTypeTerm *src, IdmTypeTerm *out) {
    (void)c;
    memset(out, 0, sizeof(*out));
    if (!src) return true;
    if (src->kind == IDM_TYPE_VAR) {
        if (src->name && strcmp(src->name, "_") == 0) return idm_type_term_copy(out, src);
        return idm_type_var(out, src->name ? src->name : "t", src->var_id, true);
    }
    IdmTypeTerm *args = src->arg_count ? calloc(src->arg_count, sizeof(*args)) : NULL;
    if (src->arg_count && !args) return false;
    for (size_t i = 0; i < src->arg_count; i++) {
        if (!rigidify_term(c, &src->args[i], &args[i])) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
    }
    if (src->kind == IDM_TYPE_CON) {
        if (src->arg_count == 0) { free(args); return idm_type_con(out, src->name); }
        if (idm_type_con_take(out, src->name, args, src->arg_count)) return true;
    } else if (idm_type_compound(out, src->kind, args, src->arg_count)) {
        return true;
    }
    for (size_t i = 0; i < src->arg_count; i++) idm_type_term_destroy(&args[i]);
    free(args);
    return false;
}

static bool inst_contract(GenCtx *g, const IdmCallableContract *c, size_t argc_want, IdmTypeTerm **out_args, size_t *out_argc, IdmTypeTerm *out_result, bool *out_has_result, IdmError *err, IdmSpan span) {
    FreshMap seen = {0};
    FreshMap repl = {0};
    bool ok = true;
    const IdmContractSig *sig = idm_contract_sig_for(c, argc_want);
    memset(out_result, 0, sizeof(*out_result));
    if (!sig) {
        *out_args = NULL;
        *out_argc = 0;
        *out_has_result = false;
        return true;
    }
    IdmTypeTerm *args = sig->arg_count ? calloc(sig->arg_count, sizeof(*args)) : NULL;
    if (sig->arg_count && !args) ok = idm_error_oom(err, span);
    for (size_t i = 0; ok && i < sig->arg_count; i++) ok = inst_term(g, c, &seen, &repl, &sig->args[i], &args[i]);
    if (ok && sig->has_result) ok = inst_term(g, c, &seen, &repl, &sig->result, out_result);
    for (size_t i = 0; ok && i < c->context_count; i++) {
        const IdmConstraint *ctr = &c->context[i];
        if (ctr->kind != IDM_CONSTR_CLASS) continue;
        IdmTypeTerm lhs;
        ok = inst_term(g, c, &seen, &repl, &ctr->lhs, &lhs);
        if (ok) { ok = idm_constraint_set_add_class(&g->wanted, canonical_trait(g, ctr->trait), &lhs); idm_type_term_destroy(&lhs); }
    }
    free(seen.items);
    free(repl.items);
    if (ok) {
        *out_args = args;
        *out_argc = sig->arg_count;
        *out_has_result = sig->has_result;
        return true;
    }
    for (size_t i = 0; i < sig->arg_count; i++) idm_type_term_destroy(&args[i]);
    free(args);
    idm_type_term_destroy(out_result);
    if (!err->present) idm_error_oom(err, span);
    return false;
}

static const IdmCallableContract *core_ref_contract(const IdmCore *core) {
    if (!core) return NULL;
    switch (core->kind) {
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
            return core->as.slot_ref.has_contract ? &core->as.slot_ref.contract : NULL;
        case IDM_CORE_PACKAGE_REF:
            return core->as.package_ref.has_contract ? &core->as.package_ref.contract : NULL;
        default:
            return NULL;
    }
}

static const IdmBinding *binding_by_env_slot(const ExpandContext *ctx, const char *name, uint32_t slot) {
    for (size_t i = ctx->bindings.count; i > 0; i--) {
        const IdmBinding *b = &ctx->bindings.items[i - 1u];
        if (b->kind == IDM_BIND_ENV && b->space == IDM_BIND_SPACE_DEFAULT && b->payload == slot && strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

static const TypeDef *type_by_identity(const ExpandContext *ctx, IdmSymbol *identity) {
    if (!identity) return NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *e = &ctx->typed.entities[i];
        if (e->kind == IDM_TYPED_ENTITY_TYPE && e->as.type.identity == identity) return &e->as.type;
    }
    return NULL;
}

static const TypeDef *type_def_by_name(const ExpandContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *e = &ctx->typed.entities[i];
        if (e->kind != IDM_TYPED_ENTITY_TYPE) continue;
        const char *id = type_def_identity_text(&e->as.type);
        if ((id && strcmp(id, name) == 0) || (e->as.type.name && strcmp(e->as.type.name, name) == 0)) return &e->as.type;
    }
    return NULL;
}

static bool subsume(GenCtx *g, const IdmTypeTerm *expected, const IdmTypeTerm *actual, IdmError *err, IdmSpan span, unsigned depth);
static bool gen_union(GenCtx *g, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmTypeTerm *out, IdmError *err, IdmSpan span);
static bool term_all_concrete(const IdmTypeTerm *t);

static bool subsume_mismatch(GenCtx *g, IdmError *err, IdmSpan span, const IdmTypeTerm *e, const IdmTypeTerm *a) {
    IdmBuffer eb;
    IdmBuffer ab;
    idm_buf_init(&eb);
    idm_buf_init(&ab);
    bool we = idm_type_term_write(&eb, e);
    bool wa = idm_type_term_write(&ab, a);
    bool ok = idm_error_set(err, span, "type mismatch for '%s': expected %s, got %s", g && g->owner ? g->owner : "<expr>", we && eb.data ? eb.data : "?", wa && ab.data ? ab.data : "?");
    idm_buf_destroy(&eb);
    idm_buf_destroy(&ab);
    return ok;
}

static bool subsume_overtype(GenCtx *g, const IdmTypeTerm *expected, const IdmTypeTerm *actual, IdmError *err, IdmSpan span, unsigned depth) {
    if (!expected->name) return subsume_mismatch(g, err, span, expected, actual);
    const TraitDef *trait = trait_by_constraint_name(g->ctx, expected->name);
    if (trait && trait_impl_satisfies_term(g->ctx, trait, actual)) return true;
    const TypeDef *td = type_def_by_name(g->ctx, expected->name);
    if (td && td->member_count != 0) {
        for (size_t i = 0; i < td->member_count; i++) {
            if (idm_type_term_equal(&td->members[i].term, expected)) continue;
            if (subsume(g, &td->members[i].term, actual, err, span, depth + 1u)) return true;
            idm_error_clear(err);
        }
    }
    if (actual->kind == IDM_TYPE_CON && actual->name) {
        const TypeDef *ta = type_def_by_name(g->ctx, actual->name);
        if (ta && ta->member_count != 0) {
            bool all = true;
            for (size_t i = 0; all && i < ta->member_count; i++) {
                if (idm_type_term_equal(&ta->members[i].term, actual)) continue;
                all = subsume(g, expected, &ta->members[i].term, err, span, depth + 1u);
                if (!all) idm_error_clear(err);
            }
            if (all) return true;
        }
    }
    return subsume_mismatch(g, err, span, expected, actual);
}

static bool subsume(GenCtx *g, const IdmTypeTerm *expected, const IdmTypeTerm *actual, IdmError *err, IdmSpan span, unsigned depth) {
    if (depth > 64u) return subsume_mismatch(g, err, span, expected, actual);
    IdmTypeTerm e;
    IdmTypeTerm a;
    if (!idm_subst_apply(&g->subst, expected, &e)) return idm_error_oom(err, span);
    if (!idm_subst_apply(&g->subst, actual, &a)) { idm_type_term_destroy(&e); return idm_error_oom(err, span); }
    bool ok;
    if (e.kind == IDM_TYPE_VAR || a.kind == IDM_TYPE_VAR) {
        ok = idm_unify(&g->subst, &g->cmp, &e, &a, err, span);
        if (!ok) { idm_error_clear(err); ok = subsume_mismatch(g, err, span, &e, &a); }
    } else if (e.kind == IDM_TYPE_CON && e.name && strcmp(e.name, "Any") == 0) {
        ok = true;
    } else if (a.kind == IDM_TYPE_CON && a.name && strcmp(a.name, "Any") == 0) {
        ok = true;
    } else if (a.kind == IDM_TYPE_UNION) {
        ok = true;
        for (size_t i = 0; i < a.arg_count && ok; i++) ok = subsume(g, &e, &a.args[i], err, span, depth + 1u);
    } else if (e.kind == IDM_TYPE_UNION) {
        ok = false;
        for (size_t i = 0; i < e.arg_count && !ok; i++) {
            ok = subsume(g, &e.args[i], &a, err, span, depth + 1u);
            if (!ok) idm_error_clear(err);
        }
        if (!ok && a.kind == IDM_TYPE_CON && a.name) {
            const TypeDef *ta = type_def_by_name(g->ctx, a.name);
            if (ta && ta->member_count != 0) {
                ok = true;
                for (size_t i = 0; ok && i < ta->member_count; i++) {
                    if (idm_type_term_equal(&ta->members[i].term, &a)) continue;
                    ok = subsume(g, &e, &ta->members[i].term, err, span, depth + 1u);
                    if (!ok) idm_error_clear(err);
                }
            }
        }
        if (!ok) subsume_mismatch(g, err, span, &e, &a);
    } else if (e.kind == IDM_TYPE_CON && a.kind == IDM_TYPE_CON && type_name_same(g->ctx, e.name, a.name)) {
        ok = true;
        if (e.arg_count == a.arg_count) {
            for (size_t i = 0; i < e.arg_count && ok; i++) ok = subsume(g, &e.args[i], &a.args[i], err, span, depth + 1u);
        }
    } else if (e.kind == IDM_TYPE_CON) {
        ok = subsume_overtype(g, &e, &a, err, span, depth);
    } else if (e.kind == a.kind && e.arg_count == a.arg_count) {
        ok = true;
        for (size_t i = 0; i < e.arg_count && ok; i++) ok = subsume(g, &e.args[i], &a.args[i], err, span, depth + 1u);
    } else {
        ok = subsume_mismatch(g, err, span, &e, &a);
    }
    if (ok && g->widen_hi != 0u && actual->kind == IDM_TYPE_VAR && !actual->rigid && actual->var_id >= g->widen_lo && actual->var_id <= g->widen_hi && e.kind == IDM_TYPE_UNION && a.kind != IDM_TYPE_VAR && term_all_concrete(&e)) {
        if (!idm_subst_widen(&g->subst, actual, &e)) ok = idm_error_oom(err, span);
    }
    if (!ok && g->widen_hi != 0u && expected->kind == IDM_TYPE_VAR && !expected->rigid && expected->var_id >= g->widen_lo && expected->var_id <= g->widen_hi && e.kind != IDM_TYPE_VAR && term_all_concrete(&a)) {
        idm_error_clear(err);
        IdmTypeTerm joined;
        ok = gen_union(g, &e, &a, &joined, err, span);
        if (ok) {
            ok = idm_subst_widen(&g->subst, expected, &joined) || idm_error_oom(err, span);
            idm_type_term_destroy(&joined);
        }
    }
    idm_type_term_destroy(&e);
    idm_type_term_destroy(&a);
    return ok;
}

static bool gen_core(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err);
static bool seed_match_clause(GenCtx *g, TypeEnv *nested, const IdmFnClause *cl, const IdmTypeTerm *scrut_types, size_t sc, IdmError *err);

static bool overtype_as_union(GenCtx *g, const IdmTypeTerm *t, IdmTypeTerm *out, bool *expanded, IdmError *err, IdmSpan span) {
    *expanded = false;
    if (t->kind != IDM_TYPE_CON || !t->name) return true;
    const TypeDef *td = type_def_by_name(g->ctx, t->name);
    if (!td || td->member_count == 0) return true;
    IdmTypeTerm *mem = calloc(td->member_count, sizeof(*mem));
    if (!mem) return idm_error_oom(err, span);
    for (size_t i = 0; i < td->member_count; i++) {
        if (!idm_type_term_copy(&mem[i], &td->members[i].term)) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&mem[j]);
            free(mem);
            return idm_error_oom(err, span);
        }
    }
    if (!idm_type_compound(out, IDM_TYPE_UNION, mem, td->member_count)) {
        for (size_t i = 0; i < td->member_count; i++) idm_type_term_destroy(&mem[i]);
        free(mem);
        return idm_error_oom(err, span);
    }
    *expanded = true;
    return true;
}

static bool type_subtract_member(GenCtx *g, const IdmTypeTerm *t, const char *member, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    if (member && t->kind == IDM_TYPE_CON && !(t->name && type_name_same(g->ctx, t->name, member))) {
        IdmTypeTerm expanded;
        bool did = false;
        if (!overtype_as_union(g, t, &expanded, &did, err, span)) return false;
        if (did) {
            bool ok = type_subtract_member(g, &expanded, member, out, err, span);
            idm_type_term_destroy(&expanded);
            return ok;
        }
    }
    if (member && t->kind == IDM_TYPE_UNION) {
        IdmTypeTerm *kept = t->arg_count ? calloc(t->arg_count, sizeof(*kept)) : NULL;
        if (t->arg_count && !kept) return idm_error_oom(err, span);
        size_t kc = 0;
        bool removed = false;
        for (size_t i = 0; i < t->arg_count; i++) {
            const IdmTypeTerm *m = &t->args[i];
            if (m->kind == IDM_TYPE_CON && m->name && type_name_same(g->ctx, m->name, member)) { removed = true; continue; }
            if (!idm_type_term_copy(&kept[kc], m)) { for (size_t j = 0; j < kc; j++) idm_type_term_destroy(&kept[j]); free(kept); return idm_error_oom(err, span); }
            kc++;
        }
        if (!removed) { for (size_t j = 0; j < kc; j++) idm_type_term_destroy(&kept[j]); free(kept); return idm_type_term_copy(out, t) || idm_error_oom(err, span); }
        if (kc == 0) { free(kept); return fresh_var(g, out) || idm_error_oom(err, span); }
        if (kc == 1) { *out = kept[0]; free(kept); return true; }
        if (!idm_type_compound(out, IDM_TYPE_UNION, kept, kc)) { for (size_t j = 0; j < kc; j++) idm_type_term_destroy(&kept[j]); free(kept); return idm_error_oom(err, span); }
        return true;
    }
    if (member && t->kind == IDM_TYPE_CON && t->name && type_name_same(g->ctx, t->name, member)) return fresh_var(g, out) || idm_error_oom(err, span);
    return idm_type_term_copy(out, t) || idm_error_oom(err, span);
}

static bool pattern_irrefutable(const IdmPattern *p) {
    return !p || p->kind == IDM_PAT_BIND || p->kind == IDM_PAT_WILDCARD;
}

static const char *pattern_head_type(GenCtx *g, const IdmPattern *p, IdmTypeTerm *lit, bool *litset, bool *full, bool *okp, IdmError *err) {
    *litset = false;
    *full = false;
    *okp = true;
    switch (p->kind) {
        case IDM_PAT_TYPE:
            *full = true;
            return p->as.name;
        case IDM_PAT_LITERAL:
            if (!type_from_literal(g, p->as.literal, lit, err, p->span)) { *okp = false; return NULL; }
            *litset = true;
            *full = idm_is_nil(p->as.literal);
            return lit->kind == IDM_TYPE_CON ? lit->name : NULL;
        case IDM_PAT_PAIR:
            *full = pattern_irrefutable(p->as.pair.left) && pattern_irrefutable(p->as.pair.right);
            return "pair";
        case IDM_PAT_LIST:
            if (p->as.seq.count == 0) { *full = true; return "empty-list"; }
            return "pair";
        case IDM_PAT_VECTOR:
            return "vector";
        case IDM_PAT_VECTOR_REST:
            *full = p->as.seq_rest.count == 0 && pattern_irrefutable(p->as.seq_rest.rest);
            return "vector";
        case IDM_PAT_TUPLE:
            return "tuple";
        case IDM_PAT_TUPLE_REST:
            *full = p->as.seq_rest.count == 0 && pattern_irrefutable(p->as.seq_rest.rest);
            return "tuple";
        case IDM_PAT_DICT:
            *full = p->as.dict.count == 0 && p->as.dict.rest && pattern_irrefutable(p->as.dict.rest);
            return "dict";
        case IDM_PAT_SYNTAX:
            return "syntax";
        default:
            return NULL;
    }
}

typedef enum { MEMBER_ABSENT, MEMBER_PRESENT, MEMBER_UNKNOWN } MemberPresence;

static MemberPresence type_member_presence(GenCtx *g, const IdmTypeTerm *t, const char *member) {
    if (!member) return MEMBER_UNKNOWN;
    if (t->kind == IDM_TYPE_VAR) return MEMBER_UNKNOWN;
    if (t->kind == IDM_TYPE_UNION) {
        MemberPresence acc = MEMBER_ABSENT;
        for (size_t i = 0; i < t->arg_count; i++) {
            MemberPresence p = type_member_presence(g, &t->args[i], member);
            if (p == MEMBER_PRESENT) return MEMBER_PRESENT;
            if (p == MEMBER_UNKNOWN) acc = MEMBER_UNKNOWN;
        }
        return acc;
    }
    if (t->kind != IDM_TYPE_CON || !t->name) return MEMBER_UNKNOWN;
    if (strcmp(t->name, "Any") == 0) return MEMBER_UNKNOWN;
    if (type_name_same(g->ctx, t->name, member)) return MEMBER_PRESENT;
    const TypeDef *td = type_def_by_name(g->ctx, t->name);
    if (td && td->member_count != 0) {
        MemberPresence acc = MEMBER_ABSENT;
        for (size_t i = 0; i < td->member_count; i++) {
            if (idm_type_term_equal(&td->members[i].term, t)) continue;
            MemberPresence p = type_member_presence(g, &td->members[i].term, member);
            if (p == MEMBER_PRESENT) return MEMBER_PRESENT;
            if (p == MEMBER_UNKNOWN) acc = MEMBER_UNKNOWN;
        }
        return acc;
    }
    return MEMBER_ABSENT;
}

static bool term_all_concrete(const IdmTypeTerm *t) {
    if (t->kind == IDM_TYPE_VAR) return false;
    if (t->kind == IDM_TYPE_CON && t->name && strcmp(t->name, "Any") == 0) return false;
    for (size_t i = 0; i < t->arg_count; i++) {
        if (!term_all_concrete(&t->args[i])) return false;
    }
    return true;
}

static bool gen_ref(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    if (core->kind == IDM_CORE_ARG_REF && type_env_lookup_arg(env, core->as.slot_ref.slot, out)) return true;
    if (core->kind == IDM_CORE_LOCAL_REF && type_env_lookup_local(env, core->as.slot_ref.slot, out)) return true;
    const IdmCallableContract *c = core_ref_contract(core);
    if (core->kind == IDM_CORE_ENV_REF && !c) {
        const IdmBinding *b = binding_by_env_slot(g->ctx, core->as.slot_ref.name, core->as.slot_ref.slot);
        if (b && b->has_contract) c = &b->contract;
    }
    const IdmContractSig *vsig = c ? idm_contract_sig_for(c, 0u) : NULL;
    if (vsig && vsig->has_result) {
        IdmTypeTerm *args = NULL;
        size_t argc = 0;
        bool has_result = false;
        return inst_contract(g, c, 0u, &args, &argc, out, &has_result, err, core->span) && (free(args), true);
    }
    return fresh_var(g, out) || idm_error_oom(err, core->span);
}

static const IdmCallableContract *call_callee_contract(GenCtx *g, const IdmCore *core, IdmCallableContract *tmp, bool *tmp_owned, IdmError *err) {
    *tmp_owned = false;
    const IdmCore *callee = core->as.call.callee;
    if (!callee) return NULL;
    if (callee->kind == IDM_CORE_FN_MULTI && callee->as.fn_multi.count == 1u && callee->as.fn_multi.clauses[0].primitive_backed) {
        bool has = false;
        if (!idm_primitive_contract(callee->as.fn_multi.clauses[0].primitive, core->as.call.arg_count, tmp, &has, err, core->span)) return NULL;
        if (has) { *tmp_owned = true; return tmp; }
        return NULL;
    }
    const IdmCallableContract *sib = sibling_contract(g, callee);
    if (sib) return sib;
    const IdmCallableContract *c = core_ref_contract(callee);
    if (c) return c;
    if (callee->kind == IDM_CORE_ENV_REF) {
        const IdmBinding *b = binding_by_env_slot(g->ctx, callee->as.slot_ref.name, callee->as.slot_ref.slot);
        if (b && b->has_contract) return &b->contract;
    }
    return NULL;
}

static const char *core_callee_name(const IdmCore *callee) {
    if (!callee) return NULL;
    switch (callee->kind) {
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
            return callee->as.slot_ref.name;
        case IDM_CORE_PACKAGE_REF:
            return callee->as.package_ref.name;
        case IDM_CORE_FN_MULTI:
            return callee->as.fn_multi.name;
        case IDM_CORE_FN:
            return callee->as.fn.name;
        default:
            return NULL;
    }
}

static bool gen_call(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    IdmCallableContract tmp;
    bool tmp_owned = false;
    memset(&tmp, 0, sizeof(tmp));
    const IdmCallableContract *c = call_callee_contract(g, core, &tmp, &tmp_owned, err);
    if (err->present) { if (tmp_owned) idm_callable_contract_destroy(&tmp); return false; }
    bool ok = true;
    IdmTypeTerm *iargs = NULL;
    size_t iargc = 0;
    IdmTypeTerm iresult;
    bool ihas = false;
    memset(&iresult, 0, sizeof(iresult));
    if (c) ok = inst_contract(g, c, core->as.call.arg_count, &iargs, &iargc, &iresult, &ihas, err, core->span);
    if (tmp_owned) idm_callable_contract_destroy(&tmp);
    if (!ok) return false;
    const char *saved_owner = g->owner;
    const char *callee_name = core_callee_name(core->as.call.callee);
    for (size_t i = 0; ok && i < core->as.call.arg_count; i++) {
        IdmTypeTerm at;
        if (!gen_core(g, env, core->as.call.args[i], &at, err)) { ok = false; break; }
        if (c && iargc == core->as.call.arg_count) {
            if (callee_name) g->owner = callee_name;
            ok = subsume(g, &iargs[i], &at, err, core->as.call.args[i]->span, 0u);
            g->owner = saved_owner;
        }
        idm_type_term_destroy(&at);
    }
    if (ok) {
        if (c && ihas && iargc == core->as.call.arg_count) {
            *out = iresult;
            ihas = false;
        } else {
            ok = fresh_var(g, out) || idm_error_oom(err, core->span);
        }
    }
    if (ihas) idm_type_term_destroy(&iresult);
    if (iargs) {
        for (size_t i = 0; i < iargc; i++) idm_type_term_destroy(&iargs[i]);
        free(iargs);
    }
    return ok;
}

static bool union_member_push(GenCtx *g, IdmTypeTerm **members, size_t *count, size_t *cap, const IdmTypeTerm *m) {
    for (size_t i = 0; i < *count; i++) {
        const IdmTypeTerm *seen = &(*members)[i];
        if (idm_type_term_equal(seen, m)) return true;
        if (seen->kind == IDM_TYPE_CON && m->kind == IDM_TYPE_CON && seen->arg_count == 0 && m->arg_count == 0 && type_name_same(g->ctx, seen->name, m->name)) return true;
    }
    if (*count == *cap && !idm_grow((void **)members, cap, sizeof(**members), 4u, *count + 1u)) return false;
    if (!idm_type_term_copy(&(*members)[*count], m)) return false;
    (*count)++;
    return true;
}

static bool union_collect(GenCtx *g, IdmTypeTerm **members, size_t *count, size_t *cap, const IdmTypeTerm *t) {
    if (t->kind == IDM_TYPE_UNION) {
        for (size_t i = 0; i < t->arg_count; i++) {
            if (!union_collect(g, members, count, cap, &t->args[i])) return false;
        }
        return true;
    }
    return union_member_push(g, members, count, cap, t);
}

static bool gen_union(GenCtx *g, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    if (idm_type_term_equal(a, b)) return idm_type_term_copy(out, a) || idm_error_oom(err, span);
    if (a->kind == IDM_TYPE_VAR || b->kind == IDM_TYPE_VAR) {
        IdmError probe;
        idm_error_init(&probe);
        if (idm_unify(&g->subst, &g->cmp, a, b, &probe, span)) return idm_subst_apply(&g->subst, a, out) || idm_error_oom(err, span);
        idm_error_clear(&probe);
    }
    IdmTypeTerm *members = NULL;
    size_t count = 0;
    size_t cap = 0;
    if (!union_collect(g, &members, &count, &cap, a) || !union_collect(g, &members, &count, &cap, b)) {
        for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i]);
        free(members);
        return idm_error_oom(err, span);
    }
    if (count == 1) {
        *out = members[0];
        free(members);
        return true;
    }
    if (!idm_type_compound(out, IDM_TYPE_UNION, members, count)) {
        for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i]);
        free(members);
        return idm_error_oom(err, span);
    }
    return true;
}

static bool gen_core(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!core) return fresh_var(g, out) || idm_error_oom(err, idm_span_unknown(NULL));
    switch (core->kind) {
        case IDM_CORE_LITERAL:
            return type_from_literal(g, core->as.literal, out, err, core->span);
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return gen_ref(g, env, core, out, err);
        case IDM_CORE_CALL:
            return gen_call(g, env, core, out, err);
        case IDM_CORE_STRING_CONCAT:
            return type_con_term("string", out, err, core->span);
        case IDM_CORE_VALUE_SEQUENCE:
            return type_con_term(idm_value_sequence_kind_name(core->as.value_sequence.kind), out, err, core->span);
        case IDM_CORE_SYNTAX_BUILD:
            return type_con_term("syntax", out, err, core->span);
        case IDM_CORE_RECORD_IS:
            return type_con_term("atom", out, err, core->span);
        case IDM_CORE_RECORD_CONSTRUCT: {
            const char *tname = idm_symbol_text(core->as.record_construct.type);
            char disp[96];
            size_t dn = tname ? strcspn(tname, "#") : 0u;
            if (dn >= sizeof(disp)) dn = sizeof(disp) - 1u;
            if (tname) memcpy(disp, tname, dn);
            disp[dn] = '\0';
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (!core->as.record_construct.field_has_contracts[i]) continue;
                IdmTypeTerm vt;
                if (!gen_core(g, env, core->as.record_construct.field_values[i], &vt, err)) return false;
                const char *saved = g->owner;
                g->owner = disp;
                bool ok = subsume(g, &core->as.record_construct.field_contracts[i], &vt, err, core->as.record_construct.field_values[i]->span, 0u);
                g->owner = saved;
                idm_type_term_destroy(&vt);
                if (!ok) return false;
            }
            return type_con_term(tname, out, err, core->span);
        }
        case IDM_CORE_RECORD_FIELD: {
            const TypeDef *td = type_by_identity(g->ctx, core->as.record_field.type);
            if (td && core->as.record_field.field_index < td->field_count && td->fields[core->as.record_field.field_index].has_contract)
                return idm_type_term_copy(out, &td->fields[core->as.record_field.field_index].contract) || idm_error_oom(err, core->span);
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        }
        case IDM_CORE_DO: {
            bool ok = fresh_var(g, out) || idm_error_oom(err, core->span);
            for (size_t i = 0; ok && i < core->as.do_expr.count; i++) {
                idm_type_term_destroy(out);
                ok = gen_core(g, env, core->as.do_expr.items[i], out, err);
            }
            return ok;
        }
        case IDM_CORE_BIND_LOCAL: {
            IdmTypeTerm vt;
            if (!gen_core(g, env, core->as.bind_local.value, &vt, err)) return false;
            TypeEnv nested;
            memset(&nested, 0, sizeof(nested));
            nested.parent = env;
            bool ok = type_env_set_local(&nested, core->as.bind_local.slot, &vt);
            idm_type_term_destroy(&vt);
            if (ok) ok = gen_core(g, &nested, core->as.bind_local.body, out, err);
            type_env_destroy(&nested);
            return ok;
        }
        case IDM_CORE_MATCH: {
            size_t sc = core->as.match_expr.scrutinee_count;
            IdmTypeTerm *st = sc ? calloc(sc, sizeof(*st)) : NULL;
            if (sc && !st) return idm_error_oom(err, core->span);
            bool ok = true;
            for (size_t i = 0; ok && i < sc; i++) ok = gen_core(g, env, core->as.match_expr.scrutinees[i], &st[i], err);
            bool closed = false;
            if (ok && sc == 1) {
                IdmTypeTerm s0;
                if (!idm_subst_apply(&g->subst, &st[0], &s0)) ok = idm_error_oom(err, core->span);
                else {
                    if (term_all_concrete(&s0)) {
                        if (s0.kind == IDM_TYPE_UNION) closed = true;
                        else if (s0.kind == IDM_TYPE_CON && s0.name) {
                            const TypeDef *td = type_def_by_name(g->ctx, s0.name);
                            closed = td && td->member_count != 0;
                        }
                    }
                    idm_type_term_destroy(&s0);
                }
            }
            bool saw_catchall = false;
            bool type_case = true;
            bool have = false;
            IdmTypeTerm acc;
            memset(&acc, 0, sizeof(acc));
            for (size_t ci = 0; ok && ci < core->as.match_expr.count; ci++) {
                const IdmFnClause *cl = &core->as.match_expr.clauses[ci];
                IdmSpan cspan = cl->body ? cl->body->span : core->span;
                for (size_t i = 0; ok && i < sc; i++) {
                    IdmTypeTerm refreshed;
                    if (!idm_subst_apply(&g->subst, &st[i], &refreshed)) { ok = idm_error_oom(err, cspan); break; }
                    idm_type_term_destroy(&st[i]);
                    st[i] = refreshed;
                }
                if (!ok) break;
                if (saw_catchall) {
                    idm_error_set(err, cspan, "match clause is unreachable");
                    ok = false;
                    break;
                }
                for (uint32_t i = 0; ok && i < cl->arity && i < sc && i < cl->pattern_count; i++) {
                    const IdmPattern *p = cl->param_patterns[i];
                    if (!p || pattern_irrefutable(p) || p->kind == IDM_PAT_PIN) continue;
                    IdmTypeTerm lit;
                    bool litset = false;
                    bool full = false;
                    const char *member = pattern_head_type(g, p, &lit, &litset, &full, &ok, err);
                    if (ok && member) {
                        IdmTypeTerm si;
                        if (!idm_subst_apply(&g->subst, &st[i], &si)) ok = idm_error_oom(err, cspan);
                        else {
                            if (type_member_presence(g, &si, member) == MEMBER_ABSENT) {
                                IdmBuffer sb;
                                idm_buf_init(&sb);
                                bool w = idm_type_term_write(&sb, &si);
                                idm_error_set(err, p->span, "match clause on '%.*s' is unreachable for scrutinee type %s", (int)strcspn(member, "#"), member, w && sb.data ? sb.data : "?");
                                idm_buf_destroy(&sb);
                                ok = false;
                            }
                            idm_type_term_destroy(&si);
                        }
                    }
                    if (litset) idm_type_term_destroy(&lit);
                }
                TypeEnv nested;
                memset(&nested, 0, sizeof(nested));
                nested.parent = env;
                if (ok) ok = seed_match_clause(g, &nested, cl, st, sc, err);
                if (ok && cl->guard) {
                    IdmTypeTerm gt;
                    ok = gen_core(g, &nested, cl->guard, &gt, err);
                    if (ok) idm_type_term_destroy(&gt);
                }
                IdmTypeTerm bt;
                memset(&bt, 0, sizeof(bt));
                if (ok) ok = gen_core(g, &nested, cl->body, &bt, err);
                if (ok) {
                    if (!have) { acc = bt; have = true; }
                    else {
                        IdmTypeTerm u;
                        ok = gen_union(g, &acc, &bt, &u, err, core->span);
                        idm_type_term_destroy(&acc);
                        idm_type_term_destroy(&bt);
                        if (ok) acc = u; else memset(&acc, 0, sizeof(acc));
                    }
                } else {
                    idm_type_term_destroy(&bt);
                }
                if (ok && cl->guard) type_case = false;
                if (ok && !cl->guard) {
                    bool catchall = true;
                    for (uint32_t i = 0; i < cl->arity; i++) {
                        const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
                        if (!pattern_irrefutable(p)) { catchall = false; break; }
                    }
                    if (catchall) saw_catchall = true;
                    for (size_t i = 0; ok && !catchall && i < sc; i++) {
                        if (i >= cl->pattern_count || pattern_irrefutable(cl->param_patterns[i])) continue;
                        const IdmPattern *p = cl->param_patterns[i];
                        IdmTypeTerm lit;
                        bool litset = false;
                        bool full = false;
                        const char *member = pattern_head_type(g, p, &lit, &litset, &full, &ok, err);
                        if (ok && member && full) {
                            IdmTypeTerm nr;
                            if (type_subtract_member(g, &st[i], member, &nr, err, core->span)) {
                                idm_type_term_destroy(&st[i]);
                                st[i] = nr;
                            }
                            else ok = false;
                        } else if (ok) {
                            type_case = false;
                        }
                        if (litset) idm_type_term_destroy(&lit);
                    }
                }
                type_env_destroy(&nested);
            }
            if (ok && closed && type_case && !saw_catchall) {
                IdmTypeTerm rem;
                if (!idm_subst_apply(&g->subst, &st[0], &rem)) ok = idm_error_oom(err, core->span);
                else {
                    if (term_all_concrete(&rem)) {
                        IdmBuffer rb;
                        idm_buf_init(&rb);
                        bool w = idm_type_term_write(&rb, &rem);
                        idm_error_set(err, core->span, "non-exhaustive match: %s is not covered", w && rb.data ? rb.data : "?");
                        idm_buf_destroy(&rb);
                        ok = false;
                    }
                    idm_type_term_destroy(&rem);
                }
            }
            for (size_t i = 0; i < sc; i++) idm_type_term_destroy(&st[i]);
            free(st);
            if (!ok) { if (have) idm_type_term_destroy(&acc); return false; }
            if (have) { *out = acc; return true; }
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        }
        case IDM_CORE_COND: {
            IdmTypeTerm t;
            IdmTypeTerm e;
            if (!gen_core(g, env, core->as.cond_expr.then_branch, &t, err)) return false;
            if (!gen_core(g, env, core->as.cond_expr.else_branch, &e, err)) { idm_type_term_destroy(&t); return false; }
            bool ok = gen_union(g, &t, &e, out, err, core->span);
            idm_type_term_destroy(&t);
            idm_type_term_destroy(&e);
            return ok;
        }
        default:
            return fresh_var(g, out) || idm_error_oom(err, core->span);
    }
}

static bool seed_match_clause(GenCtx *g, TypeEnv *nested, const IdmFnClause *cl, const IdmTypeTerm *scrut_types, size_t sc, IdmError *err) {
    IdmSpan span = cl->body ? cl->body->span : idm_span_unknown(NULL);
    for (uint32_t i = 0; i < cl->arity; i++) {
        const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
        const IdmTypeTerm *base = i < sc ? &scrut_types[i] : NULL;
        IdmTypeTerm refined;
        bool have_refined = false;
        if (p && !pattern_irrefutable(p) && p->kind != IDM_PAT_PIN) {
            IdmTypeTerm lit;
            bool litset = false;
            bool full = false;
            bool okp = true;
            const char *member = pattern_head_type(g, p, &lit, &litset, &full, &okp, err);
            if (!okp) return false;
            if (litset) {
                refined = lit;
                have_refined = true;
            } else if (member) {
                if (!type_con_term(member, &refined, err, p->span)) return false;
                have_refined = true;
            }
        }
        if (!have_refined && !base) {
            if (!fresh_var(g, &refined)) return idm_error_oom(err, span);
            have_refined = true;
        }
        const IdmTypeTerm *rt = have_refined ? &refined : base;
        bool ok = type_env_set_arg(nested, i, rt);
        if (ok && p && p->kind == IDM_PAT_BIND && p->as.name) {
            for (uint32_t k = 0; k < cl->pattern_local_count; k++) {
                if (cl->pattern_locals[k].name && strcmp(cl->pattern_locals[k].name, p->as.name) == 0) {
                    ok = type_env_set_local(nested, cl->pattern_locals[k].slot, rt);
                    break;
                }
            }
        }
        if (have_refined) idm_type_term_destroy(&refined);
        if (!ok) return idm_error_oom(err, span);
    }
    return true;
}

static void gen_ctx_init(GenCtx *g, ExpandContext *ctx, const char *owner) {
    g->ctx = ctx;
    g->owner = owner;
    g->cmp.name_eq = cmp_name_eq;
    g->cmp.trait_eq = cmp_trait_eq;
    g->cmp.given_matches = cmp_given_matches;
    g->cmp.user = ctx;
    idm_subst_init(&g->subst);
    idm_constraint_set_init(&g->wanted);
    idm_constraint_set_init(&g->given);
    g->siblings = NULL;
    g->sibling_count = 0;
    g->widen_lo = 0;
    g->widen_hi = 0;
}
static void gen_ctx_destroy(GenCtx *g) {
    idm_subst_destroy(&g->subst);
    idm_constraint_set_destroy(&g->wanted);
    idm_constraint_set_destroy(&g->given);
}

static bool reject_residual(GenCtx *g, const IdmConstraintSet *residual, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < residual->count; i++) {
        const IdmConstraint *c = &residual->items[i];
        if (c->kind != IDM_CONSTR_CLASS) continue;
        IdmTypeTerm ty;
        if (!idm_subst_apply(&g->subst, &c->lhs, &ty)) return idm_error_oom(err, span);
        if (!(ty.kind == IDM_TYPE_VAR && ty.rigid)) { idm_type_term_destroy(&ty); continue; }
        IdmBuffer tb;
        idm_buf_init(&tb);
        bool w = idm_type_term_write(&tb, &ty);
        const char *tr = c->trait ? c->trait : "?";
        int trn = (int)strcspn(tr, "#");
        idm_error_set(err, span, "unsolved typeclass '%.*s' for %s", trn, tr, w && tb.data ? tb.data : "?");
        idm_buf_destroy(&tb);
        idm_type_term_destroy(&ty);
        return false;
    }
    return true;
}

static bool seed_pattern_type(GenCtx *g, TypeEnv *env, const IdmFnClause *clause, size_t i, const IdmTypeTerm *ptype, IdmError *err) {
    if (i >= clause->pattern_count) return true;
    const IdmPattern *p = clause->param_patterns[i];
    if (!p) return true;
    if (p->kind == IDM_PAT_BIND) {
        for (uint32_t k = 0; k < clause->pattern_local_count; k++) {
            if (clause->pattern_locals[k].name && strcmp(clause->pattern_locals[k].name, p->as.name) == 0)
                return type_env_set_local(env, clause->pattern_locals[k].slot, ptype);
        }
        return true;
    }
    if (pattern_irrefutable(p) || p->kind == IDM_PAT_PIN) return true;
    IdmTypeTerm lit;
    bool litset = false;
    bool full = false;
    bool okp = true;
    const char *member = pattern_head_type(g, p, &lit, &litset, &full, &okp, err);
    if (!okp) return false;
    bool ok = true;
    if (litset) ok = type_env_set_arg(env, (uint32_t)i, &lit);
    else if (member) {
        IdmTypeTerm con;
        if (!type_con_term(member, &con, err, p->span)) return false;
        ok = type_env_set_arg(env, (uint32_t)i, &con);
        idm_type_term_destroy(&con);
    }
    if (litset) idm_type_term_destroy(&lit);
    return ok || idm_error_oom(err, p->span);
}

static bool term_is_flex_var(const IdmTypeTerm *t) {
    return t && t->kind == IDM_TYPE_VAR && !t->rigid && t->var_id != 0u && !(t->name && strcmp(t->name, "_") == 0);
}

typedef struct {
    uint32_t *ids;
    char **names;
    size_t count;
    size_t cap;
} QuantMap;

static void quant_map_destroy(QuantMap *qm) {
    for (size_t i = 0; i < qm->count; i++) free(qm->names[i]);
    free(qm->ids);
    free(qm->names);
    memset(qm, 0, sizeof(*qm));
}

static char *quant_name_make(size_t i) {
    char buf[16];
    size_t n = 0;
    buf[n++] = (char)('a' + (i % 26u));
    size_t tier = i / 26u;
    if (tier) {
        char num[12];
        size_t m = 0;
        while (tier) { num[m++] = (char)('0' + (tier % 10u)); tier /= 10u; }
        while (m) buf[n++] = num[--m];
    }
    buf[n] = '\0';
    return idm_strdup(buf);
}

static bool quant_map_find(const QuantMap *qm, uint32_t id, size_t *out_index) {
    for (size_t i = 0; i < qm->count; i++) {
        if (qm->ids[i] == id) { *out_index = i; return true; }
    }
    return false;
}

static bool quant_map_intern(QuantMap *qm, uint32_t id, size_t *out_index) {
    if (quant_map_find(qm, id, out_index)) return true;
    if (qm->count == qm->cap) {
        if (!idm_grow((void **)&qm->ids, &qm->cap, sizeof(*qm->ids), 4u, qm->count + 1u)) return false;
    }
    size_t nc = qm->count + 1u;
    char **nn = realloc(qm->names, nc * sizeof(*nn));
    if (!nn) return false;
    qm->names = nn;
    qm->names[qm->count] = quant_name_make(qm->count);
    if (!qm->names[qm->count]) return false;
    qm->ids[qm->count] = id;
    *out_index = qm->count;
    qm->count = nc;
    return true;
}

static bool quantify_applied(QuantMap *qm, const IdmTypeTerm *t, IdmTypeTerm *out) {
    memset(out, 0, sizeof(*out));
    if (term_is_flex_var(t)) {
        size_t idx = 0;
        if (!quant_map_intern(qm, t->var_id, &idx)) return false;
        return idm_type_var(out, qm->names[idx], (uint32_t)(idx + 1u), false);
    }
    if (t->kind == IDM_TYPE_VAR) return idm_type_term_copy(out, t);
    if (t->arg_count == 0) {
        if (t->kind == IDM_TYPE_CON) return idm_type_con(out, t->name);
        return idm_type_compound(out, t->kind, NULL, 0u);
    }
    IdmTypeTerm *args = calloc(t->arg_count, sizeof(*args));
    if (!args) return false;
    for (size_t i = 0; i < t->arg_count; i++) {
        if (!quantify_applied(qm, &t->args[i], &args[i])) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
    }
    bool ok = t->kind == IDM_TYPE_CON ? idm_type_con_take(out, t->name, args, t->arg_count)
                                      : idm_type_compound(out, t->kind, args, t->arg_count);
    if (!ok) {
        for (size_t i = 0; i < t->arg_count; i++) idm_type_term_destroy(&args[i]);
        free(args);
    }
    return ok;
}

static bool quantify_term(const IdmSubst *s, QuantMap *qm, const IdmTypeTerm *in, IdmTypeTerm *out) {
    IdmTypeTerm applied;
    if (!idm_subst_apply(s, in, &applied)) return false;
    bool ok = quantify_applied(qm, &applied, out);
    idm_type_term_destroy(&applied);
    return ok;
}

typedef struct {
    size_t argc;
    const IdmTypeTerm *args;
    const IdmTypeTerm *result;
    bool has_result;
} GenSigInput;

static bool generalize_contract_sigs(GenCtx *g, const GenSigInput *inputs, size_t input_count, const IdmConstraintSet *residual, IdmCallableContract *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    QuantMap qm;
    memset(&qm, 0, sizeof(qm));
    bool ok = true;
    for (size_t si = 0; ok && si < input_count; si++) {
        const GenSigInput *in = &inputs[si];
        IdmContractSig *osig = idm_contract_add_sig(out);
        if (!osig) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
        if (in->argc) {
            osig->args = calloc(in->argc, sizeof(*osig->args));
            if (!osig->args) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            osig->arg_count = in->argc;
        }
        for (size_t i = 0; ok && i < in->argc; i++) ok = quantify_term(&g->subst, &qm, &in->args[i], &osig->args[i]) || idm_error_oom(err, idm_span_unknown(NULL));
        if (ok && in->has_result && in->result) {
            ok = quantify_term(&g->subst, &qm, in->result, &osig->result) || idm_error_oom(err, idm_span_unknown(NULL));
            osig->has_result = true;
        }
    }
    if (ok && residual && residual->count) {
        IdmConstraint *ctxc = calloc(residual->count, sizeof(*ctxc));
        if (!ctxc) { ok = idm_error_oom(err, idm_span_unknown(NULL)); goto done; }
        size_t cc = 0;
        for (size_t i = 0; ok && i < residual->count; i++) {
            const IdmConstraint *c = &residual->items[i];
            if (c->kind != IDM_CONSTR_CLASS) continue;
            IdmTypeTerm applied;
            if (!idm_subst_apply(&g->subst, &c->lhs, &applied)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            size_t idx = 0;
            bool member_var = term_is_flex_var(&applied) && quant_map_find(&qm, applied.var_id, &idx);
            idm_type_term_destroy(&applied);
            if (!member_var) continue;
            bool dup = false;
            for (size_t j = 0; j < cc; j++) {
                if (ctxc[j].lhs.var_id == (uint32_t)(idx + 1u) && ctxc[j].trait && c->trait && strcmp(ctxc[j].trait, c->trait) == 0) { dup = true; break; }
            }
            if (dup) continue;
            memset(&ctxc[cc], 0, sizeof(ctxc[cc]));
            ctxc[cc].kind = IDM_CONSTR_CLASS;
            if (!idm_type_var(&ctxc[cc].lhs, qm.names[idx], (uint32_t)(idx + 1u), false)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            ctxc[cc].trait = c->trait ? idm_strdup(c->trait) : NULL;
            if (c->trait && !ctxc[cc].trait) { idm_type_term_destroy(&ctxc[cc].lhs); ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            cc++;
        }
        if (ok && cc) { out->context = ctxc; out->context_count = cc; }
        else free(ctxc);
    }
    if (ok && qm.count) {
        out->quantified = calloc(qm.count, sizeof(*out->quantified));
        if (!out->quantified) { ok = idm_error_oom(err, idm_span_unknown(NULL)); goto done; }
        for (size_t i = 0; i < qm.count; i++) { out->quantified[i] = qm.names[i]; qm.names[i] = NULL; }
        out->quantified_count = qm.count;
    }
done:
    quant_map_destroy(&qm);
    if (!ok) idm_callable_contract_destroy(out);
    return ok;
}


static bool check_clause(ExpandContext *ctx, const IdmFnClause *clause, const IdmCallableContract *contract, const char *name, IdmError *err) {
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    TypeEnv env;
    memset(&env, 0, sizeof(env));
    env.givens = contract;
    bool ok = true;
    const IdmContractSig *sig = contract ? idm_contract_sig_for(contract, clause->arity) : NULL;
    if (contract && !sig) {
        gen_ctx_destroy(&g);
        return idm_error_set(err, idm_span_unknown(NULL), "signature for '%s' expects arity %zu, got %u", name, contract->sig_count ? contract->sigs[0].arg_count : 0u, clause->arity);
    }
    for (size_t i = 0; ok && contract && i < contract->context_count; i++) {
        const IdmConstraint *c = &contract->context[i];
        if (c->kind != IDM_CONSTR_CLASS) continue;
        IdmTypeTerm rl;
        ok = rigidify_term(contract, &c->lhs, &rl);
        if (ok) { ok = idm_constraint_set_add_class(&g.given, canonical_trait(&g, c->trait), &rl); idm_type_term_destroy(&rl); }
    }
    IdmTypeTerm *ptypes = clause->arity ? calloc(clause->arity, sizeof(*ptypes)) : NULL;
    if (clause->arity && !ptypes) ok = idm_error_oom(err, idm_span_unknown(NULL));
    for (uint32_t i = 0; ok && i < clause->arity; i++) {
        if (sig && i < sig->arg_count) ok = rigidify_term(contract, &sig->args[i], &ptypes[i]);
        else ok = fresh_var(&g, &ptypes[i]);
        if (ok) ok = type_env_set_arg(&env, i, &ptypes[i]);
        if (ok) ok = seed_pattern_type(&g, &env, clause, i, &ptypes[i], err);
    }
    IdmTypeTerm body;
    memset(&body, 0, sizeof(body));
    IdmSpan bspan = clause->body ? clause->body->span : idm_span_unknown(NULL);
    if (ok) ok = gen_core(&g, &env, clause->body, &body, err);
    if (ok && sig && sig->has_result) {
        IdmTypeTerm rr;
        ok = rigidify_term(contract, &sig->result, &rr);
        if (ok) { ok = subsume(&g, &rr, &body, err, bspan, 0u); idm_type_term_destroy(&rr); }
    }
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.cmp, &g.given, &g.wanted, ctx_instance_oracle, ctx, &residual, err, bspan);
    if (ok && contract) ok = reject_residual(&g, &residual, err, bspan);
    idm_constraint_set_destroy(&residual);
    if (ptypes) {
        for (uint32_t i = 0; i < clause->arity; i++) idm_type_term_destroy(&ptypes[i]);
        free(ptypes);
    }
    idm_type_term_destroy(&body);
    type_env_destroy(&env);
    gen_ctx_destroy(&g);
    return ok;
}

static bool clause_infer_into(GenCtx *g, const IdmFnClause *cl, const IdmTypeTerm *shared, IdmTypeTerm *argacc, size_t argc, bool *arg_started, bool *arg_open, IdmTypeTerm *resacc, bool *res_started, IdmError *err) {
    TypeEnv env;
    memset(&env, 0, sizeof(env));
    IdmTypeTerm *cargs = argc ? calloc(argc, sizeof(*cargs)) : NULL;
    if (argc && !cargs) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; ok && i < argc; i++) {
        ok = shared ? idm_type_term_copy(&cargs[i], &shared[i]) : fresh_var(g, &cargs[i]);
        if (ok) ok = type_env_set_arg(&env, i, &cargs[i]);
        if (ok) ok = seed_pattern_type(g, &env, cl, i, &cargs[i], err);
    }
    IdmTypeTerm body;
    memset(&body, 0, sizeof(body));
    if (ok) ok = gen_core(g, &env, cl->body, &body, err);
    for (size_t i = 0; ok && i < argc; i++) {
        IdmTypeTerm applied;
        memset(&applied, 0, sizeof(applied));
        bool have_applied = false;
        const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
        if (arg_open && (pattern_irrefutable(p) || (p && p->kind == IDM_PAT_PIN))) arg_open[i] = true;
        if (arg_open && arg_open[i]) continue;
        if (p && !pattern_irrefutable(p) && p->kind != IDM_PAT_PIN) {
            IdmTypeTerm lit;
            bool litset = false;
            bool full = false;
            const char *member = pattern_head_type(g, p, &lit, &litset, &full, &ok, err);
            if (ok && litset) { applied = lit; have_applied = true; }
            else if (ok && member) {
                have_applied = type_con_term(member, &applied, err, idm_span_unknown(NULL));
                if (!have_applied) ok = false;
            }
        }
        if (ok && !have_applied) {
            if (!idm_subst_apply(&g->subst, &cargs[i], &applied)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            have_applied = true;
        }
        if (!ok) { if (have_applied) idm_type_term_destroy(&applied); break; }
        if (!arg_started[i]) { argacc[i] = applied; arg_started[i] = true; }
        else {
            IdmTypeTerm u;
            ok = gen_union(g, &argacc[i], &applied, &u, err, idm_span_unknown(NULL));
            idm_type_term_destroy(&applied);
            if (ok) { idm_type_term_destroy(&argacc[i]); argacc[i] = u; }
        }
    }
    if (ok) {
        IdmTypeTerm rapplied;
        if (!idm_subst_apply(&g->subst, &body, &rapplied)) ok = idm_error_oom(err, idm_span_unknown(NULL));
        else if (!*res_started) { *resacc = rapplied; *res_started = true; }
        else {
            IdmTypeTerm u;
            ok = gen_union(g, resacc, &rapplied, &u, err, idm_span_unknown(NULL));
            idm_type_term_destroy(&rapplied);
            if (ok) { idm_type_term_destroy(resacc); *resacc = u; }
        }
    }
    idm_type_term_destroy(&body);
    if (cargs) { for (size_t i = 0; i < argc; i++) idm_type_term_destroy(&cargs[i]); free(cargs); }
    type_env_destroy(&env);
    return ok;
}


static bool value_clauses_infer(GenCtx *g, const IdmCore *value, const IdmTypeTerm *shared, size_t argc, IdmTypeTerm *argacc, bool *arg_started, bool *arg_open, IdmTypeTerm *resacc, bool *res_started, IdmError *err) {
    if (value->kind == IDM_CORE_FN) {
        IdmFnClause cl;
        memset(&cl, 0, sizeof(cl));
        cl.arity = value->as.fn.arity;
        cl.param_patterns = value->as.fn.param_patterns;
        cl.pattern_count = value->as.fn.pattern_count;
        cl.pattern_locals = value->as.fn.pattern_locals;
        cl.pattern_local_count = value->as.fn.pattern_local_count;
        cl.body = value->as.fn.body;
        return clause_infer_into(g, &cl, shared, argacc, argc, arg_started, arg_open, resacc, res_started, err);
    }
    if (value->kind == IDM_CORE_FN_MULTI) {
        for (size_t i = 0; i < value->as.fn_multi.count; i++) {
            if (value->as.fn_multi.clauses[i].arity != argc) continue;
            if (!clause_infer_into(g, &value->as.fn_multi.clauses[i], shared, argacc, argc, arg_started, arg_open, resacc, res_started, err)) return false;
        }
        return true;
    }
    bool ok = gen_core(g, NULL, value, resacc, err);
    *res_started = ok;
    return ok;
}

static bool infer_scheme(ExpandContext *ctx, const IdmCore *value, const char *name, IdmCallableContract *out, bool *out_has, IdmError *err) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    if (!value) return true;
    size_t arities[16];
    size_t arity_count = 0;
    if (value->kind == IDM_CORE_FN) {
        arities[arity_count++] = value->as.fn.arity;
    } else if (value->kind == IDM_CORE_FN_MULTI) {
        if (value->as.fn_multi.count == 0) return true;
        for (size_t ci = 0; ci < value->as.fn_multi.count; ci++) {
            size_t a = value->as.fn_multi.clauses[ci].arity;
            bool seen = false;
            for (size_t k = 0; k < arity_count; k++) {
                if (arities[k] == a) { seen = true; break; }
            }
            if (!seen) {
                if (arity_count >= 16u) return true;
                arities[arity_count++] = a;
            }
        }
    } else {
        arities[arity_count++] = 0;
    }
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    bool ok = true;
    GenSigInput inputs[16];
    IdmTypeTerm *accs[16] = {0};
    bool *starts[16] = {0};
    IdmTypeTerm results[16];
    bool result_started[16] = {0};
    memset(results, 0, sizeof(results));
    size_t built = 0;
    for (size_t k = 0; ok && k < arity_count; k++) {
        size_t argc = arities[k];
        accs[k] = argc ? calloc(argc, sizeof(*accs[k])) : NULL;
        starts[k] = argc ? calloc(argc, sizeof(*starts[k])) : NULL;
        bool *open_flags = argc ? calloc(argc, sizeof(*open_flags)) : NULL;
        if (argc && (!accs[k] || !starts[k] || !open_flags)) ok = idm_error_oom(err, value->span);
        if (ok && (value->kind == IDM_CORE_FN || value->kind == IDM_CORE_FN_MULTI)) {
            ok = value_clauses_infer(&g, value, NULL, argc, accs[k], starts[k], open_flags, &results[k], &result_started[k], err);
        } else if (ok) {
            ok = gen_core(&g, NULL, value, &results[k], err);
            result_started[k] = ok;
        }
        if (ok) {
            for (size_t j2 = 0; j2 < argc; j2++) {
                if (open_flags[j2] && starts[k][j2]) {
                    idm_type_term_destroy(&accs[k][j2]);
                    if (!fresh_var(&g, &accs[k][j2])) { ok = idm_error_oom(err, value->span); break; }
                } else if (!starts[k][j2]) {
                    if (!fresh_var(&g, &accs[k][j2])) { ok = idm_error_oom(err, value->span); break; }
                    starts[k][j2] = true;
                }
            }
        }
        free(open_flags);
        if (ok) {
            inputs[k].argc = argc;
            inputs[k].args = accs[k];
            inputs[k].result = &results[k];
            inputs[k].has_result = result_started[k];
            built = k + 1u;
        }
    }
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.cmp, &g.given, &g.wanted, ctx_instance_oracle, ctx, &residual, err, value->span);
    if (ok) ok = generalize_contract_sigs(&g, inputs, built, &residual, out, err);
    if (ok) *out_has = true;
    idm_constraint_set_destroy(&residual);
    for (size_t k = 0; k < arity_count; k++) {
        if (accs[k]) {
            for (size_t j2 = 0; j2 < arities[k]; j2++) if (starts[k] && starts[k][j2]) idm_type_term_destroy(&accs[k][j2]);
            free(accs[k]);
        }
        free(starts[k]);
        if (result_started[k]) idm_type_term_destroy(&results[k]);
    }
    gen_ctx_destroy(&g);
    if (!ok) { idm_callable_contract_destroy(out); memset(out, 0, sizeof(*out)); *out_has = false; }
    return ok;
}

bool expand_typecheck_infer_scheme(ExpandContext *ctx, const IdmCore *value, const char *name, IdmCallableContract *out, bool *out_has, IdmError *err) {
    return infer_scheme(ctx, value, name, out, out_has, err);
}

static bool check_fn(ExpandContext *ctx, const IdmCore *value, const IdmCallableContract *contract, const char *name, IdmError *err) {
    if (!value) return true;
    if (value->kind == IDM_CORE_FN) {
        IdmFnClause clause;
        memset(&clause, 0, sizeof(clause));
        clause.arity = value->as.fn.arity;
        clause.param_patterns = value->as.fn.param_patterns;
        clause.pattern_count = value->as.fn.pattern_count;
        clause.pattern_locals = value->as.fn.pattern_locals;
        clause.pattern_local_count = value->as.fn.pattern_local_count;
        clause.guard = value->as.fn.guard;
        clause.body = value->as.fn.body;
        return check_clause(ctx, &clause, contract, name, err);
    }
    if (value->kind == IDM_CORE_FN_MULTI) {
        for (size_t i = 0; i < value->as.fn_multi.count; i++) {
            if (!check_clause(ctx, &value->as.fn_multi.clauses[i], contract, name, err)) return false;
        }
        return true;
    }
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    IdmTypeTerm t;
    bool ok = gen_core(&g, NULL, value, &t, err);
    const IdmContractSig *vsig = contract ? idm_contract_sig_for(contract, 0u) : NULL;
    if (ok && vsig && vsig->has_result) ok = subsume(&g, &vsig->result, &t, err, value->span, 0u);
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.cmp, &g.given, &g.wanted, ctx_instance_oracle, ctx, &residual, err, value->span);
    if (ok && contract) ok = reject_residual(&g, &residual, err, value->span);
    idm_constraint_set_destroy(&residual);
    idm_type_term_destroy(&t);
    gen_ctx_destroy(&g);
    return ok;
}

bool expand_core_static_type_term(ExpandContext *ctx, const IdmCore *core, IdmTypeTerm *out, bool *has_type, IdmError *err) {
    if (has_type) *has_type = false;
    memset(out, 0, sizeof(*out));
    GenCtx g;
    gen_ctx_init(&g, ctx, NULL);
    IdmTypeTerm t;
    bool ok = gen_core(&g, NULL, core, &t, err);
    if (ok) ok = idm_solve(&g.subst, &g.cmp, &g.given, &g.wanted, ctx_instance_oracle, ctx, NULL, err, core ? core->span : idm_span_unknown(NULL));
    if (ok) {
        IdmTypeTerm applied;
        if (idm_subst_apply(&g.subst, &t, &applied)) {
            if (applied.kind == IDM_TYPE_CON && applied.name && strcmp(applied.name, "->") != 0) {
                *out = applied;
                if (has_type) *has_type = true;
            } else if (applied.kind == IDM_TYPE_CON) {
                idm_type_term_destroy(&applied);
            } else {
                idm_type_term_destroy(&applied);
            }
        }
    }
    idm_type_term_destroy(&t);
    gen_ctx_destroy(&g);
    if (!ok) { idm_type_term_destroy(out); memset(out, 0, sizeof(*out)); if (has_type) *has_type = false; }
    return ok || !err->present;
}

bool expand_typecheck_value(ExpandContext *ctx, const char *name, IdmCore *value, IdmCallableContract *contract, bool *has_contract, IdmError *err) {
    if (!value || !has_contract) return true;
    if (*has_contract) {
        if (!check_fn(ctx, value, contract, name, err)) return false;
        contract->purity = expand_typecheck_core_purity(ctx, value);
        return true;
    }
    if (!infer_scheme(ctx, value, name, contract, has_contract, err)) return false;
    if (!*has_contract) return check_fn(ctx, value, NULL, name, err);
    contract->purity = expand_typecheck_core_purity(ctx, value);
    return true;
}

static bool publish_inferred_contract(ExpandContext *ctx, const DefnGroup *group, const IdmCallableContract *contract, IdmError *err) {


    const IdmBinding *b = binding_by_env_slot(ctx, group->name, group->slot);
    if (b && !idm_binding_table_set_contract(&ctx->bindings, b->id, contract)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!record_package_slot(ctx, group->name, group->slot, &group->scopes, group->arity, contract, group->exported)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

typedef struct {
    size_t argc;
    IdmTypeTerm *sargs;
    IdmTypeTerm sresult;
} MemberSig;

typedef struct {
    MemberSig *msigs;
    size_t msig_count;
    IdmCallableContract skel;
    bool shaped;
} GroupMember;

static void group_member_destroy(GroupMember *m) {
    if (!m->shaped) return;
    for (size_t k = 0; k < m->msig_count; k++) {
        for (size_t j = 0; j < m->msigs[k].argc; j++) idm_type_term_destroy(&m->msigs[k].sargs[j]);
        free(m->msigs[k].sargs);
        idm_type_term_destroy(&m->msigs[k].sresult);
    }
    free(m->msigs);
    idm_callable_contract_destroy(&m->skel);
    memset(m, 0, sizeof(*m));
}

static bool group_member_skeleton(GenCtx *g, GroupMember *m, const IdmCore *value, IdmError *err) {
    m->shaped = false;
    if (!value) return true;
    size_t arities[16];
    size_t arity_count = 0;
    if (value->kind == IDM_CORE_FN) {
        arities[arity_count++] = value->as.fn.arity;
    } else if (value->kind == IDM_CORE_FN_MULTI) {
        for (size_t ci = 0; ci < value->as.fn_multi.count; ci++) {
            size_t a = value->as.fn_multi.clauses[ci].arity;
            bool seen = false;
            for (size_t k = 0; k < arity_count; k++) {
                if (arities[k] == a) { seen = true; break; }
            }
            if (!seen && arity_count < 16u) arities[arity_count++] = a;
        }
    }
    if (arity_count == 0) return true;
    m->msigs = calloc(arity_count, sizeof(*m->msigs));
    if (!m->msigs) return idm_error_oom(err, value->span);
    memset(&m->skel, 0, sizeof(m->skel));
    m->shaped = true;
    m->msig_count = arity_count;
    for (size_t k = 0; k < arity_count; k++) {
        MemberSig *ms = &m->msigs[k];
        ms->argc = arities[k];
        ms->sargs = ms->argc ? calloc(ms->argc, sizeof(*ms->sargs)) : NULL;
        if (ms->argc && !ms->sargs) return idm_error_oom(err, value->span);
        IdmContractSig *ssig = idm_contract_add_sig(&m->skel);
        if (!ssig) return idm_error_oom(err, value->span);
        if (ms->argc) {
            ssig->args = calloc(ms->argc, sizeof(*ssig->args));
            if (!ssig->args) return idm_error_oom(err, value->span);
            ssig->arg_count = ms->argc;
        }
        for (size_t j = 0; j < ms->argc; j++) {
            if (!fresh_var(g, &ms->sargs[j]) || !idm_type_term_copy(&ssig->args[j], &ms->sargs[j])) return idm_error_oom(err, value->span);
        }
        if (!fresh_var(g, &ms->sresult) || !idm_type_term_copy(&ssig->result, &ms->sresult)) return idm_error_oom(err, value->span);
        ssig->has_result = true;
    }
    return true;
}

static bool overtype_member_covers(GenCtx *g, const TypeDef *td, const IdmTypeTerm *t) {
    if (t->kind != IDM_TYPE_CON || !t->name) return false;
    const char *id = type_def_identity_text(td);
    if (id && type_name_same(g->ctx, id, t->name)) return false;
    for (size_t i = 0; i < td->member_count; i++) {
        const IdmTypeTerm *m = &td->members[i].term;
        if (m->kind == IDM_TYPE_CON && m->name && type_name_same(g->ctx, m->name, t->name)) return true;
    }
    return false;
}

static const TypeDef *overtype_covering(GenCtx *g, const IdmTypeTerm *t) {
    const TypeDef *found = NULL;
    for (size_t i = 0; i < g->ctx->typed.entity_count; i++) {
        const TypedEntity *e = &g->ctx->typed.entities[i];
        if (e->kind != IDM_TYPED_ENTITY_TYPE || e->as.type.member_count == 0) continue;
        const TypeDef *td = &e->as.type;
        bool covers = true;
        if (t->kind == IDM_TYPE_CON) covers = overtype_member_covers(g, td, t);
        else if (t->kind == IDM_TYPE_UNION) {
            for (size_t j = 0; covers && j < t->arg_count; j++) covers = overtype_member_covers(g, td, &t->args[j]);
        } else {
            covers = false;
        }
        if (!covers) continue;
        if (found) return NULL;
        found = td;
    }
    return found;
}

static bool promote_arg_binding(GenCtx *g, const IdmTypeTerm *var, IdmError *err, IdmSpan span) {
    if (!var || var->kind != IDM_TYPE_VAR) return true;
    IdmTypeTerm applied;
    if (!idm_subst_apply(&g->subst, var, &applied)) return idm_error_oom(err, span);
    bool ok = true;
    if (applied.kind != IDM_TYPE_VAR && term_all_concrete(&applied)) {
        const TypeDef *td = overtype_covering(g, &applied);
        const char *id = td ? type_def_identity_text(td) : NULL;
        if (id) {
            IdmTypeTerm wide;
            if (!idm_type_con(&wide, id)) ok = idm_error_oom(err, span);
            else {
                ok = idm_subst_widen(&g->subst, var, &wide) || idm_error_oom(err, span);
                idm_type_term_destroy(&wide);
            }
        }
    }
    idm_type_term_destroy(&applied);
    return ok;
}

static void core_mark_sibling_refs(const IdmCore *core, const DefnGroup *groups, size_t count, bool *deps) {
    if (!core) return;
    switch (core->kind) {
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_ENV_REF:
            for (size_t i = 0; i < count; i++) {
                if (groups[i].slot == core->as.slot_ref.slot && core->as.slot_ref.name && groups[i].name && strcmp(groups[i].name, core->as.slot_ref.name) == 0) deps[i] = true;
            }
            return;
        case IDM_CORE_PACKAGE_REF:
            for (size_t i = 0; i < count; i++) {
                if (groups[i].slot == core->as.package_ref.slot && core->as.package_ref.name && groups[i].name && strcmp(groups[i].name, core->as.package_ref.name) == 0) deps[i] = true;
            }
            return;
        case IDM_CORE_CALL:
            core_mark_sibling_refs(core->as.call.callee, groups, count, deps);
            for (size_t i = 0; i < core->as.call.arg_count; i++) core_mark_sibling_refs(core->as.call.args[i], groups, count, deps);
            return;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            core_mark_sibling_refs(core->as.list_pair.head, groups, count, deps);
            core_mark_sibling_refs(core->as.list_pair.tail, groups, count, deps);
            return;
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) core_mark_sibling_refs(core->as.value_sequence.items[i], groups, count, deps);
            return;
        case IDM_CORE_SYNTAX_BUILD:
            core_mark_sibling_refs(core->as.syntax_build.ctx, groups, count, deps);
            core_mark_sibling_refs(core->as.syntax_build.payload, groups, count, deps);
            return;
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) core_mark_sibling_refs(core->as.string_concat.items[i], groups, count, deps);
            return;
        case IDM_CORE_COND:
            core_mark_sibling_refs(core->as.cond_expr.cond, groups, count, deps);
            core_mark_sibling_refs(core->as.cond_expr.then_branch, groups, count, deps);
            core_mark_sibling_refs(core->as.cond_expr.else_branch, groups, count, deps);
            return;
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) core_mark_sibling_refs(core->as.match_expr.scrutinees[i], groups, count, deps);
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                core_mark_sibling_refs(core->as.match_expr.clauses[i].guard, groups, count, deps);
                core_mark_sibling_refs(core->as.match_expr.clauses[i].body, groups, count, deps);
            }
            return;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) core_mark_sibling_refs(core->as.do_expr.items[i], groups, count, deps);
            return;
        case IDM_CORE_BIND_LOCAL:
            core_mark_sibling_refs(core->as.bind_local.value, groups, count, deps);
            core_mark_sibling_refs(core->as.bind_local.body, groups, count, deps);
            return;
        case IDM_CORE_FN:
            core_mark_sibling_refs(core->as.fn.guard, groups, count, deps);
            core_mark_sibling_refs(core->as.fn.body, groups, count, deps);
            return;
        case IDM_CORE_FN_MULTI:
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                core_mark_sibling_refs(core->as.fn_multi.clauses[i].guard, groups, count, deps);
                core_mark_sibling_refs(core->as.fn_multi.clauses[i].body, groups, count, deps);
            }
            return;
        case IDM_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) core_mark_sibling_refs(core->as.letrec.bindings[i].value, groups, count, deps);
            core_mark_sibling_refs(core->as.letrec.body, groups, count, deps);
            return;
        case IDM_CORE_RECEIVE:
            core_mark_sibling_refs(core->as.receive.receiver, groups, count, deps);
            core_mark_sibling_refs(core->as.receive.timeout, groups, count, deps);
            core_mark_sibling_refs(core->as.receive.timeout_body, groups, count, deps);
            return;
        case IDM_CORE_GUARD:
            core_mark_sibling_refs(core->as.guard.body, groups, count, deps);
            core_mark_sibling_refs(core->as.guard.handler, groups, count, deps);
            core_mark_sibling_refs(core->as.guard.cleanup, groups, count, deps);
            return;
        case IDM_CORE_USE_PACKAGE:
            core_mark_sibling_refs(core->as.use_package.cont, groups, count, deps);
            return;
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) core_mark_sibling_refs(core->as.record_construct.field_values[i], groups, count, deps);
            return;
        case IDM_CORE_RECORD_FIELD:
            core_mark_sibling_refs(core->as.record_field.receiver, groups, count, deps);
            return;
        case IDM_CORE_RECORD_IS:
            core_mark_sibling_refs(core->as.record_is.value, groups, count, deps);
            return;
        default:
            return;
    }
}

static void group_topo_order(const DefnGroup *groups, IdmCore **values, size_t count, size_t *order) {
    bool *edges = count ? calloc(count * count, sizeof(*edges)) : NULL;
    size_t *indegree = count ? calloc(count, sizeof(*indegree)) : NULL;
    bool *placed = count ? calloc(count, sizeof(*placed)) : NULL;
    if (!edges || !indegree || !placed) {
        for (size_t i = 0; i < count; i++) order[i] = i;
        free(edges);
        free(indegree);
        free(placed);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        core_mark_sibling_refs(values[i], groups, count, edges + i * count);
        edges[i * count + i] = false;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            if (edges[i * count + j]) indegree[i]++;
        }
    }
    size_t placed_count = 0;
    while (placed_count < count) {
        size_t pick = count;
        for (size_t i = 0; i < count; i++) {
            if (!placed[i] && indegree[i] == 0) { pick = i; break; }
        }
        if (pick == count) {
            for (size_t i = 0; i < count; i++) {
                if (!placed[i]) { pick = i; break; }
            }
        }
        placed[pick] = true;
        order[placed_count++] = pick;
        for (size_t i = 0; i < count; i++) {
            if (!placed[i] && edges[i * count + pick] && indegree[i] > 0) indegree[i]--;
        }
    }
    free(edges);
    free(indegree);
    free(placed);
}

static uint8_t core_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const uint8_t *member_purity, size_t count);

static uint8_t purity_join(uint8_t a, uint8_t b) {
    return a < b ? a : b;
}

static uint8_t callee_purity_level(const ExpandContext *ctx, const IdmCore *callee, const DefnGroup *groups, const uint8_t *member_purity, size_t count, bool *out_prim) {
    *out_prim = false;
    if (!callee) return IDM_PURITY_IMPURE;
    switch (callee->kind) {
        case IDM_CORE_FN_MULTI:
            if (callee->as.fn_multi.count == 1u && callee->as.fn_multi.clauses[0].primitive_backed) {
                *out_prim = true;
                return idm_primitive_pure(callee->as.fn_multi.clauses[0].primitive) ? IDM_PURITY_PURE : IDM_PURITY_IMPURE;
            }
            return core_purity_level(ctx, callee, groups, member_purity, count);
        case IDM_CORE_FN:
            return core_purity_level(ctx, callee, groups, member_purity, count);
        case IDM_CORE_ARG_REF:
        case IDM_CORE_CAPTURE_REF:
            return IDM_PURITY_ARGS;
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF: {
            const char *name = callee->kind == IDM_CORE_PACKAGE_REF ? callee->as.package_ref.name : callee->as.slot_ref.name;
            uint32_t slot = callee->kind == IDM_CORE_PACKAGE_REF ? callee->as.package_ref.slot : callee->as.slot_ref.slot;
            for (size_t i = 0; i < count; i++) {
                if (groups[i].slot == slot && groups[i].name && name && strcmp(groups[i].name, name) == 0) return member_purity[i];
            }
            const IdmCallableContract *c = core_ref_contract(callee);
            if (!c && callee->kind == IDM_CORE_ENV_REF) {
                const IdmBinding *b = binding_by_env_slot(ctx, callee->as.slot_ref.name, callee->as.slot_ref.slot);
                if (b && b->has_contract) c = &b->contract;
            }
            return c ? c->purity : IDM_PURITY_IMPURE;
        }
        default:
            return IDM_PURITY_IMPURE;
    }
}

static uint8_t call_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const uint8_t *member_purity, size_t count) {
    bool prim = false;
    uint8_t callee_level = callee_purity_level(ctx, core->as.call.callee, groups, member_purity, count, &prim);
    if (callee_level == IDM_PURITY_IMPURE) return IDM_PURITY_IMPURE;
    bool invokes_args = callee_level == IDM_PURITY_ARGS;
    uint8_t level = IDM_PURITY_PURE;
    for (size_t i = 0; i < core->as.call.arg_count; i++) {
        const IdmCore *arg = core->as.call.args[i];
        if (!arg) continue;
        if (arg->kind == IDM_CORE_ARG_REF || arg->kind == IDM_CORE_CAPTURE_REF || arg->kind == IDM_CORE_LOCAL_REF) {
            if (invokes_args) level = purity_join(level, IDM_PURITY_ARGS);
            continue;
        }
        if (arg->kind == IDM_CORE_ENV_REF || arg->kind == IDM_CORE_PACKAGE_REF) {
            if (invokes_args) {
                bool aprim = false;
                level = purity_join(level, callee_purity_level(ctx, arg, groups, member_purity, count, &aprim));
            }
            continue;
        }
        level = purity_join(level, core_purity_level(ctx, arg, groups, member_purity, count));
    }
    return level;
}

static uint8_t core_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const uint8_t *member_purity, size_t count) {
    if (!core) return IDM_PURITY_PURE;
    switch (core->kind) {
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return IDM_PURITY_PURE;
        case IDM_CORE_RECEIVE:
        case IDM_CORE_GUARD:
        case IDM_CORE_USE_PACKAGE:
            return IDM_PURITY_IMPURE;
        case IDM_CORE_CALL:
            return call_purity_level(ctx, core, groups, member_purity, count);
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return purity_join(core_purity_level(ctx, core->as.list_pair.head, groups, member_purity, count),
                               core_purity_level(ctx, core->as.list_pair.tail, groups, member_purity, count));
        case IDM_CORE_VALUE_SEQUENCE: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.value_sequence.items[i], groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_SYNTAX_BUILD:
            return purity_join(core_purity_level(ctx, core->as.syntax_build.ctx, groups, member_purity, count),
                               core_purity_level(ctx, core->as.syntax_build.payload, groups, member_purity, count));
        case IDM_CORE_STRING_CONCAT: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.string_concat.items[i], groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_COND:
            return purity_join(core_purity_level(ctx, core->as.cond_expr.cond, groups, member_purity, count),
                   purity_join(core_purity_level(ctx, core->as.cond_expr.then_branch, groups, member_purity, count),
                               core_purity_level(ctx, core->as.cond_expr.else_branch, groups, member_purity, count)));
        case IDM_CORE_MATCH: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.scrutinees[i], groups, member_purity, count));
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.clauses[i].guard, groups, member_purity, count));
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.clauses[i].body, groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_DO: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.do_expr.items[i], groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_BIND_LOCAL:
            return purity_join(core_purity_level(ctx, core->as.bind_local.value, groups, member_purity, count),
                               core_purity_level(ctx, core->as.bind_local.body, groups, member_purity, count));
        case IDM_CORE_FN:
            return purity_join(core_purity_level(ctx, core->as.fn.guard, groups, member_purity, count),
                               core_purity_level(ctx, core->as.fn.body, groups, member_purity, count));
        case IDM_CORE_FN_MULTI: {
            if (core->as.fn_multi.count == 1u && core->as.fn_multi.clauses[0].primitive_backed) {
                return idm_primitive_pure(core->as.fn_multi.clauses[0].primitive) ? IDM_PURITY_PURE : IDM_PURITY_IMPURE;
            }
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].guard, groups, member_purity, count));
                level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].body, groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_LETREC: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.letrec.bindings[i].value, groups, member_purity, count));
            }
            return purity_join(level, core_purity_level(ctx, core->as.letrec.body, groups, member_purity, count));
        }
        case IDM_CORE_RECORD_CONSTRUCT: {
            uint8_t level = IDM_PURITY_PURE;
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.record_construct.field_values[i], groups, member_purity, count));
            }
            return level;
        }
        case IDM_CORE_RECORD_FIELD:
            return core_purity_level(ctx, core->as.record_field.receiver, groups, member_purity, count);
        case IDM_CORE_RECORD_IS:
            return core_purity_level(ctx, core->as.record_is.value, groups, member_purity, count);
        default:
            return IDM_PURITY_IMPURE;
    }
}

uint8_t expand_typecheck_core_purity(ExpandContext *ctx, const IdmCore *core) {
    return core_purity_level(ctx, core, NULL, NULL, 0u);
}

bool expand_typecheck_purity(ExpandContext *ctx, const DefnGroup *groups, IdmCore **values, size_t count, uint8_t *out_purity) {
    for (size_t i = 0; i < count; i++) out_purity[i] = IDM_PURITY_PURE;
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            uint8_t level = core_purity_level(ctx, values[i], groups, out_purity, count);
            if (level < out_purity[i]) {
                out_purity[i] = level;
                changed = true;
            }
        }
    }
    return true;
}

bool expand_typecheck_defn_groups(ExpandContext *ctx, const DefnGroup *groups, IdmCore **values, size_t count, IdmError *err) {
    uint8_t *purity = count ? calloc(count, sizeof(*purity)) : NULL;
    if (count && !purity) return idm_error_oom(err, idm_span_unknown(NULL));
    expand_typecheck_purity(ctx, groups, values, count, purity);
    for (size_t i = 0; i < count; i++) {
        if (!groups[i].has_contract) continue;
        if (!check_fn(ctx, values[i], &groups[i].contract, groups[i].name, err)) { free(purity); return false; }
        IdmCallableContract annotated;
        if (!idm_callable_contract_copy(&annotated, &groups[i].contract)) { free(purity); return idm_error_oom(err, idm_span_unknown(NULL)); }
        annotated.purity = purity[i];
        bool pok = publish_inferred_contract(ctx, &groups[i], &annotated, err);
        idm_callable_contract_destroy(&annotated);
        if (!pok) { free(purity); return false; }
    }
    size_t unannotated = 0;
    for (size_t i = 0; i < count; i++) {
        if (!groups[i].has_contract) unannotated++;
    }
    if (unannotated == 0) return true;
    GenCtx g;
    gen_ctx_init(&g, ctx, NULL);
    GroupMember *members = calloc(count, sizeof(*members));
    SiblingContract *sibs = calloc(count, sizeof(*sibs));
    size_t sib_count = 0;
    bool ok = members && sibs;
    if (!ok) idm_error_oom(err, idm_span_unknown(NULL));
    uint32_t widen_seq_base = ctx->type_var_seq;
    for (size_t i = 0; ok && i < count; i++) {
        if (groups[i].has_contract) {
            sibs[sib_count].name = groups[i].name;
            sibs[sib_count].slot = groups[i].slot;
            sibs[sib_count].contract = &groups[i].contract;
            sib_count++;
            continue;
        }
        ok = group_member_skeleton(&g, &members[i], values[i], err);
        if (ok && members[i].shaped) {
            sibs[sib_count].name = groups[i].name;
            sibs[sib_count].slot = groups[i].slot;
            sibs[sib_count].contract = &members[i].skel;
            sib_count++;
        }
    }
    if (ctx->type_var_seq > widen_seq_base) {
        g.widen_lo = 0x80000000u | (widen_seq_base + 1u);
        g.widen_hi = 0x80000000u | ctx->type_var_seq;
    }
    g.siblings = sibs;
    g.sibling_count = sib_count;
    IdmSpan gspan = count && values[0] ? values[0]->span : idm_span_unknown(NULL);
    size_t *order = count ? calloc(count, sizeof(*order)) : NULL;
    if (ok && count && !order) ok = idm_error_oom(err, gspan);
    if (ok && count) group_topo_order(groups, values, count, order);
    for (size_t oi = 0; ok && oi < count; oi++) {
        size_t i = order[oi];
        if (groups[i].has_contract || !members[i].shaped) continue;
        g.owner = groups[i].name;
        for (size_t k = 0; ok && k < members[i].msig_count; k++) {
            MemberSig *ms = &members[i].msigs[k];
            size_t argc = ms->argc;
            IdmTypeTerm *argacc = argc ? calloc(argc, sizeof(*argacc)) : NULL;
            bool *arg_started = argc ? calloc(argc, sizeof(*arg_started)) : NULL;
            bool *arg_open = argc ? calloc(argc, sizeof(*arg_open)) : NULL;
            if (argc && (!argacc || !arg_started || !arg_open)) ok = idm_error_oom(err, values[i]->span);
            IdmTypeTerm resacc;
            memset(&resacc, 0, sizeof(resacc));
            bool res_started = false;
            if (ok) ok = value_clauses_infer(&g, values[i], ms->sargs, argc, argacc, arg_started, arg_open, &resacc, &res_started, err);
            for (size_t j = 0; ok && j < argc; j++) {
                if (arg_started[j] && !arg_open[j]) ok = subsume(&g, &ms->sargs[j], &argacc[j], err, values[i]->span, 0u);
            }
            if (ok && res_started) ok = subsume(&g, &ms->sresult, &resacc, err, values[i]->span, 0u);
            if (argacc) { for (size_t j = 0; j < argc; j++) if (arg_started[j]) idm_type_term_destroy(&argacc[j]); free(argacc); }
            free(arg_started);
            free(arg_open);
            if (res_started) idm_type_term_destroy(&resacc);
        }
    }
    g.owner = NULL;
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.cmp, &g.given, &g.wanted, ctx_instance_oracle, ctx, &residual, err, gspan);
    for (size_t i = 0; ok && i < count; i++) {
        if (groups[i].has_contract || !members[i].shaped) continue;
        for (size_t k = 0; ok && k < members[i].msig_count; k++) {
            MemberSig *ms = &members[i].msigs[k];
            if (ok) ok = promote_arg_binding(&g, &ms->sresult, err, gspan);
            for (size_t j = 0; ok && j < ms->argc; j++) {
                ok = promote_arg_binding(&g, &ms->sargs[j], err, gspan);
            }
        }
    }
    for (size_t i = 0; ok && i < count; i++) {
        if (groups[i].has_contract || !members[i].shaped) continue;
        GenSigInput inputs[16];
        size_t input_count = members[i].msig_count < 16u ? members[i].msig_count : 16u;
        for (size_t k = 0; k < input_count; k++) {
            inputs[k].argc = members[i].msigs[k].argc;
            inputs[k].args = members[i].msigs[k].sargs;
            inputs[k].result = &members[i].msigs[k].sresult;
            inputs[k].has_result = true;
        }
        IdmCallableContract inferred;
        ok = generalize_contract_sigs(&g, inputs, input_count, &residual, &inferred, err);
        if (ok) inferred.purity = purity[i];
        if (ok) ok = publish_inferred_contract(ctx, &groups[i], &inferred, err);
        idm_callable_contract_destroy(&inferred);
    }
    idm_constraint_set_destroy(&residual);
    for (size_t i = 0; ok && i < count; i++) {
        if (!groups[i].has_contract && !members[i].shaped && !check_fn(ctx, values[i], NULL, groups[i].name, err)) ok = false;
    }
    if (members) {
        for (size_t i = 0; i < count; i++) group_member_destroy(&members[i]);
    }
    free(members);
    free(sibs);
    free(order);
    free(purity);
    gen_ctx_destroy(&g);
    return ok;
}
