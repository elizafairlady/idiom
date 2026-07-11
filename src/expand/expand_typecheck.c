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
    const IdmCore *core;
    IdmTypeTerm term;
} HarvestEntry;

typedef struct {
    ExpandContext *ctx;
    IdmSubst subst;
    IdmConstraintSet wanted;
    IdmConstraintSet given;
    const char *owner;
    bool may;
    const SiblingContract *siblings;
    size_t sibling_count;
    uint32_t widen_lo;
    uint32_t widen_hi;
    HarvestEntry *harvest;
    size_t harvest_count;
    size_t harvest_cap;
    bool harvest_on;
    bool total;
} GenCtx;

static size_t solved_type_slot(const SolvedNodeType *items, size_t cap, const IdmCore *core) {
    size_t h = ((size_t)(uintptr_t)core >> 4) * 2654435761u;
    size_t i = h & (cap - 1u);
    while (items[i].core && items[i].core != core) i = (i + 1u) & (cap - 1u);
    return i;
}

bool expand_solved_type_set(ExpandContext *ctx, const IdmCore *core, IdmTypeTerm *term) {
    if (!core) { idm_type_term_destroy(term); return true; }
    if (ctx->solved_node_count * 4u >= ctx->solved_node_cap * 3u) {
        size_t next = ctx->solved_node_cap ? ctx->solved_node_cap * 2u : 256u;
        SolvedNodeType *grown = calloc(next, sizeof(*grown));
        if (!grown) { idm_type_term_destroy(term); return false; }
        for (size_t i = 0; i < ctx->solved_node_cap; i++) {
            if (!ctx->solved_nodes[i].core) continue;
            grown[solved_type_slot(grown, next, ctx->solved_nodes[i].core)] = ctx->solved_nodes[i];
        }
        free(ctx->solved_nodes);
        ctx->solved_nodes = grown;
        ctx->solved_node_cap = next;
    }
    size_t i = solved_type_slot(ctx->solved_nodes, ctx->solved_node_cap, core);
    if (ctx->solved_nodes[i].core) {
        idm_type_term_destroy(&ctx->solved_nodes[i].term);
    } else {
        ctx->solved_nodes[i].core = core;
        ctx->solved_node_count++;
    }
    ctx->solved_nodes[i].term = *term;
    memset(term, 0, sizeof(*term));
    return true;
}

const IdmTypeTerm *expand_solved_type_lookup(const ExpandContext *ctx, const IdmCore *core) {
    if (!core || ctx->solved_node_count == 0) return NULL;
    size_t i = solved_type_slot(ctx->solved_nodes, ctx->solved_node_cap, core);
    return ctx->solved_nodes[i].core ? &ctx->solved_nodes[i].term : NULL;
}

void expand_solved_types_clear(ExpandContext *ctx) {
    for (size_t i = 0; i < ctx->solved_node_cap; i++) {
        if (!ctx->solved_nodes[i].core) continue;
        idm_type_term_destroy(&ctx->solved_nodes[i].term);
        ctx->solved_nodes[i].core = NULL;
    }
    ctx->solved_node_count = 0;
}

static bool gen_harvest_flush(GenCtx *g, IdmError *err) {
    bool ok = true;
    for (size_t i = 0; i < g->harvest_count; i++) {
        HarvestEntry *e = &g->harvest[i];
        if (ok) {
            IdmTypeTerm applied;
            if (!idm_subst_apply(&g->subst, &e->term, &applied)) ok = idm_error_oom(err, idm_span_unknown(NULL));
            else if (!expand_solved_type_set(g->ctx, e->core, &applied)) ok = idm_error_oom(err, idm_span_unknown(NULL));
        }
        idm_type_term_destroy(&e->term);
    }
    g->harvest_count = 0;
    return ok;
}

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
static bool cmp_given_matches(void *user, const IdmConstraint *given, IdmSymbol *trait, const IdmTypeTerm *lhs) {
    return typeclass_given_matches((ExpandContext *)user, given, idm_symbol_text(trait), lhs);
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
    return idm_type_var(g->ctx->rt, out, "t", 0x80000000u | g->ctx->type_var_seq, false);
}

static bool type_con_term(GenCtx *g, const char *name, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    return idm_type_con(g->ctx->rt, out, name) || idm_error_oom(err, span);
}

static bool type_con_symbol_term(IdmSymbol *symbol, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    return idm_type_con_symbol(out, symbol) || idm_error_oom(err, span);
}

static bool type_from_literal(GenCtx *g, IdmValue value, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    (void)g;
    if (idm_is_nil(value)) return type_con_term(g, "empty-list", out, err, span);
    IdmValueTag tag = idm_value_tag(value);
    const char *name = tag == IDM_VAL_BIGNUM ? "int" : idm_value_type_name(tag);
    return type_con_term(g, name, out, err, span);
}

static IdmInstanceResult ctx_instance_oracle(void *user, const IdmConstraint *constraint, const IdmTypeTerm *ty) {
    ExpandContext *ctx = user;
    const char *type = idm_type_term_text(ty);
    if (ty && ty->kind == IDM_TYPE_CON && type && strcmp(type, "Any") == 0) return IDM_INST_YES;
    if (constraint->kind == IDM_CONSTR_STRUCTURAL) {
        if (!ty || ty->kind != IDM_TYPE_CON || !ty->symbol) return IDM_INST_UNKNOWN;
        return type_satisfies_structural(ctx, &constraint->structural, ty->symbol) ? IDM_INST_YES : IDM_INST_NO;
    }
    const char *trait = idm_symbol_text(constraint->trait);
    const TraitDef *td = trait_by_constraint_name(ctx, trait);
    if (!td) return IDM_INST_UNKNOWN;
    return trait_impl_satisfies_term(ctx, td, ty) ? IDM_INST_YES : IDM_INST_NO;
}

static IdmSymbol *canonical_trait(GenCtx *g, IdmSymbol *trait_symbol) {
    const char *trait = idm_symbol_text(trait_symbol);
    const TraitDef *td = trait_by_constraint_name(g->ctx, trait);
    const char *id = td ? trait_def_identity_text(td) : NULL;
    return id ? idm_intern(&g->ctx->rt->intern, IDM_SYMBOL_ATOM, id) : trait_symbol;
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
    const char *name = idm_type_term_text(v);
    if (!c || !v || v->kind != IDM_TYPE_VAR || !name) return false;
    for (size_t i = 0; i < c->quantified_count; i++) {
        if (c->quantified[i] && strcmp(c->quantified[i], name) == 0) return true;
    }
    return v->var_id != 0u && v->var_id <= c->quantified_count;
}

typedef bool (*TermVarMap)(void *user, const IdmTypeTerm *var, IdmTypeTerm *out);

static bool term_map_vars(void *user, TermVarMap map_var, const IdmTypeTerm *src, IdmTypeTerm *out) {
    memset(out, 0, sizeof(*out));
    if (!src) return true;
    if (src->kind == IDM_TYPE_VAR) return map_var(user, src, out);
    IdmTypeTerm *args = src->arg_count ? calloc(src->arg_count, sizeof(*args)) : NULL;
    if (src->arg_count && !args) return false;
    for (size_t i = 0; i < src->arg_count; i++) {
        if (!term_map_vars(user, map_var, &src->args[i], &args[i])) {
            for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
    }
    if (src->kind == IDM_TYPE_CON) {
        if (src->arg_count == 0) { free(args); return idm_type_con_symbol(out, src->symbol); }
        if (idm_type_con_take_symbol(out, src->symbol, args, src->arg_count)) return true;
    } else if (idm_type_compound(out, src->kind, args, src->arg_count)) {
        return true;
    }
    for (size_t i = 0; i < src->arg_count; i++) idm_type_term_destroy(&args[i]);
    free(args);
    return false;
}

typedef struct {
    GenCtx *g;
    const IdmCallableContract *owner;
    FreshMap *seen;
    FreshMap *repl;
} InstVars;

static bool inst_map_var(void *user, const IdmTypeTerm *var, IdmTypeTerm *out) {
    InstVars *iv = user;
    if (contract_quantifies(iv->owner, var)) {
        uint32_t nv = 0;
        if (!fresh_map_get(iv->g, iv->seen, iv->repl, var->var_id ? var->var_id : idm_symbol_id(var->symbol), &nv)) return false;
        return idm_type_var_symbol(out, var->symbol, nv, false);
    }
    return idm_type_term_copy(out, var);
}

static bool inst_term(GenCtx *g, const IdmCallableContract *owner, FreshMap *seen, FreshMap *repl, const IdmTypeTerm *src, IdmTypeTerm *out) {
    InstVars iv = { g, owner, seen, repl };
    return term_map_vars(&iv, inst_map_var, src, out);
}

static bool rigid_map_var(void *user, const IdmTypeTerm *var, IdmTypeTerm *out) {
    (void)user;
    if (strcmp(idm_type_term_text(var), "_") == 0) return idm_type_term_copy(out, var);
    return idm_type_var_symbol(out, var->symbol, var->var_id, true);
}

static bool rigidify_term(const IdmCallableContract *c, const IdmTypeTerm *src, IdmTypeTerm *out) {
    (void)c;
    return term_map_vars(NULL, rigid_map_var, src, out);
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
        if (ctr->kind != IDM_CONSTR_CLASS && ctr->kind != IDM_CONSTR_STRUCTURAL) continue;
        IdmTypeTerm lhs;
        ok = inst_term(g, c, &seen, &repl, &ctr->lhs, &lhs);
        if (ok) {
            ok = ctr->kind == IDM_CONSTR_CLASS
                ? idm_constraint_set_add_class(&g->wanted, canonical_trait(g, ctr->trait), &lhs)
                : idm_constraint_set_add_structural(&g->wanted, &ctr->structural, &lhs);
            idm_type_term_destroy(&lhs);
        }
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
    const char *expected_name = idm_type_term_text(expected);
    if (!expected_name) return subsume_mismatch(g, err, span, expected, actual);
    const TraitDef *trait = trait_by_constraint_name(g->ctx, expected_name);
    if (trait && trait_impl_satisfies_term(g->ctx, trait, actual)) return true;
    const TypeDef *td = type_def_by_name(g->ctx, expected_name);
    if (td && td->member_count != 0) {
        for (size_t i = 0; i < td->member_count; i++) {
            if (idm_type_term_equal(&td->members[i].term, expected)) continue;
            if (subsume(g, &td->members[i].term, actual, err, span, depth + 1u)) return true;
            idm_error_clear(err);
        }
    }
    if (actual->kind == IDM_TYPE_CON && actual->symbol) {
        const TypeDef *ta = type_def_by_name(g->ctx, idm_type_term_text(actual));
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

static bool numeric_con_pair(const IdmTypeTerm *e, const IdmTypeTerm *a) {
    if (e->kind != IDM_TYPE_CON || a->kind != IDM_TYPE_CON || e->arg_count != 0 || a->arg_count != 0 || !e->symbol || !a->symbol) return false;
    const char *en = idm_type_term_text(e);
    const char *an = idm_type_term_text(a);
    return (strcmp(en, "int") == 0 && strcmp(an, "float") == 0) ||
           (strcmp(en, "float") == 0 && strcmp(an, "int") == 0);
}

static bool number_classed_var(GenCtx *g, const IdmTypeTerm *v) {
    if (!v || v->kind != IDM_TYPE_VAR || v->rigid) return false;
    for (size_t i = 0; i < g->wanted.count; i++) {
        const IdmConstraint *c = &g->wanted.items[i];
        if (c->kind != IDM_CONSTR_CLASS && c->kind != IDM_CONSTR_STRUCTURAL) continue;
        if (c->lhs.kind != IDM_TYPE_VAR || c->lhs.var_id != v->var_id) continue;
        if (typeclass_same_trait(g->ctx, idm_symbol_text(c->trait), "Number")) return true;
    }
    return false;
}

static bool subsume(GenCtx *g, const IdmTypeTerm *expected, const IdmTypeTerm *actual, IdmError *err, IdmSpan span, unsigned depth) {
    if (depth > 64u) return subsume_mismatch(g, err, span, expected, actual);
    IdmTypeTerm e;
    IdmTypeTerm a;
    if (!idm_subst_apply(&g->subst, expected, &e)) return idm_error_oom(err, span);
    if (!idm_subst_apply(&g->subst, actual, &a)) { idm_type_term_destroy(&e); return idm_error_oom(err, span); }
    bool ok;
    bool member_identity = false;
    const IdmTypeTerm *var_member = NULL;
    if (a.kind == IDM_TYPE_VAR && !a.rigid && e.kind == IDM_TYPE_UNION) {
        for (size_t i = 0; !member_identity && i < e.arg_count; i++) {
            if (e.args[i].kind != IDM_TYPE_VAR || e.args[i].rigid) continue;
            if (e.args[i].var_id == a.var_id) member_identity = true;
            else if (!var_member) var_member = &e.args[i];
        }
    }
    if (member_identity) {
        ok = true;
    } else if (var_member) {
        ok = idm_unify(&g->subst, var_member, &a, err, span);
        if (!ok) { idm_error_clear(err); ok = subsume_mismatch(g, err, span, &e, &a); }
    } else if (e.kind == IDM_TYPE_CON && e.symbol && strcmp(idm_type_term_text(&e), "Any") == 0) {
        ok = true;
    } else if (e.kind == IDM_TYPE_VAR && e.rigid && a.kind == IDM_TYPE_UNION) {
        ok = true;
        for (size_t i = 0; i < a.arg_count && ok; i++) ok = subsume(g, &e, &a.args[i], err, span, depth + 1u);
    } else if (e.kind == IDM_TYPE_VAR || a.kind == IDM_TYPE_VAR) {
        ok = idm_unify(&g->subst, &e, &a, err, span);
        if (!ok) { idm_error_clear(err); ok = subsume_mismatch(g, err, span, &e, &a); }
    } else if (a.kind == IDM_TYPE_CON && a.symbol && strcmp(idm_type_term_text(&a), "Any") == 0) {
        ok = true;
    } else if (a.kind == IDM_TYPE_UNION && g->may) {
        ok = false;
        for (size_t i = 0; i < a.arg_count && !ok; i++) {
            ok = subsume(g, &e, &a.args[i], err, span, depth + 1u);
            if (!ok) idm_error_clear(err);
        }
        if (!ok) subsume_mismatch(g, err, span, &e, &a);
    } else if (a.kind == IDM_TYPE_UNION) {
        ok = true;
        for (size_t i = 0; i < a.arg_count && ok; i++) ok = subsume(g, &e, &a.args[i], err, span, depth + 1u);
    } else if (e.kind == IDM_TYPE_UNION) {
        ok = false;
        for (size_t i = 0; i < e.arg_count && !ok; i++) {
            if (e.args[i].kind == IDM_TYPE_VAR && !e.args[i].rigid) continue;
            ok = subsume(g, &e.args[i], &a, err, span, depth + 1u);
            if (!ok) idm_error_clear(err);
        }
        for (size_t i = 0; i < e.arg_count && !ok; i++) {
            if (e.args[i].kind != IDM_TYPE_VAR || e.args[i].rigid) continue;
            ok = subsume(g, &e.args[i], &a, err, span, depth + 1u);
            if (!ok) idm_error_clear(err);
        }
        if (!ok && a.kind == IDM_TYPE_CON && a.symbol) {
            const TypeDef *ta = type_def_by_name(g->ctx, idm_type_term_text(&a));
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
    } else if (e.kind == IDM_TYPE_CON && a.kind == IDM_TYPE_CON && e.symbol == a.symbol) {
        ok = true;
        if (e.arg_count == a.arg_count) {
            for (size_t i = 0; i < e.arg_count && ok; i++) ok = subsume(g, &e.args[i], &a.args[i], err, span, depth + 1u);
        }
    } else if (numeric_con_pair(&e, &a) && number_classed_var(g, expected)) {
        IdmTypeTerm widened;
        ok = idm_type_con(g->ctx->rt, &widened, "float") || idm_error_oom(err, span);
        if (ok) {
            ok = idm_subst_widen(&g->subst, expected, &widened) || idm_error_oom(err, span);
            idm_type_term_destroy(&widened);
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
static bool gen_harvest_record(GenCtx *g, const IdmCore *core, const IdmTypeTerm *term, IdmError *err);
static bool seed_match_clause(GenCtx *g, TypeEnv *nested, const IdmFnClause *cl, const IdmTypeTerm *scrut_types, size_t sc, IdmError *err);
static bool seed_pattern_type(GenCtx *g, TypeEnv *env, const IdmFnClause *clause, size_t i, const IdmTypeTerm *ptype, IdmError *err);

static bool gen_discard(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmError *err) {
    if (!core) return true;
    IdmTypeTerm t;
    if (!gen_core(g, env, core, &t, err)) return false;
    idm_type_term_destroy(&t);
    return true;
}

static bool gen_fn_clause(GenCtx *g, const IdmFnClause *cl, IdmError *err) {
    IdmSpan span = cl->body ? cl->body->span : idm_span_unknown(NULL);
    TypeEnv env;
    memset(&env, 0, sizeof(env));
    bool ok = true;
    for (uint32_t i = 0; ok && i < cl->arity; i++) {
        IdmTypeTerm av;
        ok = fresh_var(g, &av) || idm_error_oom(err, span);
        if (!ok) break;
        ok = type_env_set_arg(&env, i, &av) || idm_error_oom(err, span);
        if (ok) ok = seed_pattern_type(g, &env, cl, i, &av, err);
        idm_type_term_destroy(&av);
    }
    if (ok && cl->guard) ok = gen_discard(g, &env, cl->guard, err);
    if (ok) ok = gen_discard(g, &env, cl->body, err);
    type_env_destroy(&env);
    return ok;
}



static bool pattern_irrefutable(const IdmPattern *p) {
    return !p || p->kind == IDM_PAT_BIND || p->kind == IDM_PAT_WILDCARD;
}


static bool guard_check_pins_slot(const IdmCore *check, uint32_t slot) {
    if (!check || check->kind != IDM_CORE_CALL || check->as.call.arg_count != 1) return false;
    const IdmCore *callee = check->as.call.callee;
    const IdmCore *a0 = check->as.call.args[0];
    return callee && callee->kind == IDM_CORE_FN_MULTI && callee->as.fn_multi.count == 1u &&
           callee->as.fn_multi.clauses[0].primitive_backed && callee->as.fn_multi.clauses[0].primitive == IDM_PRIM_BITS_P &&
           a0 && a0->kind == IDM_CORE_ARG_REF && a0->as.slot_ref.slot == slot;
}

static bool clause_slot_pin(GenCtx *g, const IdmFnClause *cl, size_t i, IdmTypeTerm *out, bool *pinned, IdmError *err) {
    memset(out, 0, sizeof(*out));
    *pinned = false;
    const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
    if (!p || p->kind == IDM_PAT_PIN) return true;
    if (!pattern_irrefutable(p)) {
        IdmPatternProbe probe;
        if (!idm_pattern_probe(g->ctx->rt, p, &probe)) return idm_error_oom(err, p->span);
        if (probe.litset) {
            if (!type_from_literal(g, probe.literal, out, err, p->span)) return false;
            *pinned = true;
            return true;
        }
        if (!probe.has_head) return true;
        if (!type_con_term(g, probe.head_name, out, err, p->span)) return false;
        *pinned = true;
        return true;
    }
    for (const IdmCore *n = cl->guard; n;) {
        if (guard_check_pins_slot(n->kind == IDM_CORE_COND ? n->as.cond_expr.cond : n, (uint32_t)i)) {
            if (!type_con_term(g, "bitstring", out, err, p->span)) return false;
            *pinned = true;
            return true;
        }
        if (n->kind == IDM_CORE_BIND_LOCAL) { n = n->as.bind_local.body; continue; }
        if (n->kind != IDM_CORE_COND) break;
        n = n->as.cond_expr.then_branch;
    }
    return true;
}

static bool term_overlaps_head(GenCtx *g, const IdmTypeTerm *t, const IdmPatternProbe *probe, unsigned depth) {
    if (depth > 16u || !probe->has_head) return true;
    if (t->kind == IDM_TYPE_VAR) return true;
    if (t->kind == IDM_TYPE_UNION) {
        for (size_t i = 0; i < t->arg_count; i++) {
            if (term_overlaps_head(g, &t->args[i], probe, depth + 1u)) return true;
        }
        return false;
    }
    const char *name = idm_type_term_text(t);
    if (t->kind != IDM_TYPE_CON || !name) return true;
    if (strcmp(name, "Any") == 0) return true;
    if (type_name_same(g->ctx, name, probe->head_name)) return true;
    const TypeDef *td = type_def_by_name(g->ctx, name);
    if (td && td->member_count != 0) {
        for (size_t i = 0; i < td->member_count; i++) {
            if (idm_type_term_equal(&td->members[i].term, t)) continue;
            if (term_overlaps_head(g, &td->members[i].term, probe, depth + 1u)) return true;
        }
        return false;
    }
    return idm_pattern_probe_overlaps_type(g->ctx->rt, probe, name);
}


static bool term_all_concrete(const IdmTypeTerm *t) {
    if (t->kind == IDM_TYPE_VAR) return false;
    if (t->kind == IDM_TYPE_CON && t->symbol && strcmp(idm_type_term_text(t), "Any") == 0) return false;
    for (size_t i = 0; i < t->arg_count; i++) {
        if (!term_all_concrete(&t->args[i])) return false;
    }
    return true;
}

static bool union_collect(GenCtx *g, IdmTypeTerm **members, size_t *count, size_t *cap, const IdmTypeTerm *t);

static bool match_closed_members(GenCtx *g, const IdmTypeTerm *s0, IdmTypeTerm **out_members, size_t *out_count, IdmError *err, IdmSpan span) {
    *out_members = NULL;
    *out_count = 0;
    IdmTypeTerm *members = NULL;
    size_t count = 0;
    size_t cap = 0;
    bool ok = true;
    if (s0->kind == IDM_TYPE_UNION) {
        ok = union_collect(g, &members, &count, &cap, s0) || idm_error_oom(err, span);
    } else if (s0->kind == IDM_TYPE_CON && s0->symbol) {
        const TypeDef *td = type_def_by_name(g->ctx, idm_type_term_text(s0));
        for (size_t i = 0; ok && td && i < td->member_count; i++) {
            ok = union_collect(g, &members, &count, &cap, &td->members[i].term) || idm_error_oom(err, span);
        }
    }
    if (!ok) {
        for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i]);
        free(members);
        return false;
    }
    *out_members = members;
    *out_count = count;
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
        if (!idm_primitive_contract(g->ctx->rt, callee->as.fn_multi.clauses[0].primitive, core->as.call.arg_count, tmp, &has, err, core->span)) return NULL;
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

static bool callee_is_ref(const IdmCore *callee) {
    if (!callee) return true;
    switch (callee->kind) {
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
        default:
            return false;
    }
}

static bool callee_has_callable_shape(GenCtx *g, const IdmCore *callee) {
    if (!callee) return false;
    if (sibling_contract(g, callee)) return true;
    if (callee->kind == IDM_CORE_PACKAGE_REF) return true;
    if (callee->kind != IDM_CORE_ENV_REF) return false;
    const IdmBinding *binding = binding_by_env_slot(g->ctx, callee->as.slot_ref.name, callee->as.slot_ref.slot);
    return binding && binding->arity.kind != IDM_ARITY_UNKNOWN;
}

static bool gen_call(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    if (!callee_is_ref(core->as.call.callee) && !gen_discard(g, env, core->as.call.callee, err)) return false;
    IdmCallableContract tmp;
    bool tmp_owned = false;
    memset(&tmp, 0, sizeof(tmp));
    const IdmCallableContract *c = call_callee_contract(g, core, &tmp, &tmp_owned, err);
    if (err->present) { if (tmp_owned) idm_callable_contract_destroy(&tmp); return false; }
    if (c && callee_has_callable_shape(g, core->as.call.callee) && c->sig_count != 0 && !idm_contract_sig_for(c, core->as.call.arg_count)) {
        const char *name = core_callee_name(core->as.call.callee);
        if (tmp_owned) idm_callable_contract_destroy(&tmp);
        return idm_error_set(err, core->span, "call to '%s' has no contract signature for %zu arguments", name ? name : "<callable>", core->as.call.arg_count);
    }
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
            bool saved_may = g->may;
            g->may = true;
            ok = subsume(g, &iargs[i], &at, err, core->as.call.args[i]->span, 0u);
            g->may = saved_may;
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

static void gen_ctx_init(GenCtx *g, ExpandContext *ctx, const char *owner);
static void gen_ctx_destroy(GenCtx *g);

static bool dispatch_receiver_term(ExpandContext *ctx, const IdmCore *core, IdmTypeTerm *out, bool *has_type) {
    if (has_type) *has_type = false;
    memset(out, 0, sizeof(*out));
    if (!core) return true;
    GenCtx g;
    gen_ctx_init(&g, ctx, NULL);
    IdmError probe;
    idm_error_init(&probe);
    IdmTypeTerm t;
    bool ok = gen_core(&g, NULL, core, &t, &probe);
    if (ok) ok = idm_solve(&g.subst, &g.given, &g.wanted, cmp_given_matches, ctx_instance_oracle, ctx, NULL, &probe, core->span);
    if (ok) {
        IdmTypeTerm applied;
        if (idm_subst_apply(&g.subst, &t, &applied)) {
            if (applied.kind == IDM_TYPE_CON && applied.symbol && strcmp(idm_type_term_text(&applied), "->") != 0) {
                *out = applied;
                if (has_type) *has_type = true;
            } else {
                idm_type_term_destroy(&applied);
            }
        }
        idm_type_term_destroy(&t);
    }
    idm_error_clear(&probe);
    gen_ctx_destroy(&g);
    return true;
}

static const IdmCallableContract *env_givens(const TypeEnv *env) {
    for (const TypeEnv *e = env; e; e = e->parent) {
        if (e->givens) return e->givens;
    }
    return NULL;
}

static bool gen_dispatch(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    IdmCore *node = (IdmCore *)core;
    if (core->as.dispatch.kind == IDM_DISPATCH_IMPLEMENTS) {
        if (core->as.dispatch.arg_count != 0 && !gen_discard(g, env, core->as.dispatch.args[0], err)) return false;
        IdmTypeTerm applied;
        bool has_applied = false;
        if (core->as.dispatch.arg_count != 0 && !dispatch_receiver_term(g->ctx, core->as.dispatch.args[0], &applied, &has_applied)) return idm_error_oom(err, core->span);
        uint8_t route = IDM_DISPATCH_ROUTE_DYNAMIC;
        const TraitDef *trait = typed_trait_by_symbol(g->ctx, core->as.dispatch.identity);
        if (trait && has_applied && applied.kind != IDM_TYPE_VAR) {
            route = trait_impl_satisfies_term(g->ctx, trait, &applied) ? IDM_DISPATCH_ROUTE_FOLD_TRUE : IDM_DISPATCH_ROUTE_FOLD_FALSE;
        } else if (trait && has_applied) {
            const IdmCallableContract *givens = env_givens(env);
            for (size_t i = 0; givens && i < givens->context_count; i++) {
                if (typeclass_given_matches(g->ctx, &givens->context[i], core->as.dispatch.name, &applied)) {
                    route = IDM_DISPATCH_ROUTE_FOLD_TRUE;
                    break;
                }
            }
        }
        if (has_applied) idm_type_term_destroy(&applied);
        node->as.dispatch.route = route;
        return type_con_term(g, "atom", out, err, core->span);
    }
    if (core->as.dispatch.kind == IDM_DISPATCH_FIELD) {
        if (core->as.dispatch.arg_count == 0) return fresh_var(g, out) || idm_error_oom(err, core->span);
        if (!gen_discard(g, env, core->as.dispatch.args[0], err)) return false;
        IdmTypeTerm applied;
        bool got = false;
        if (!dispatch_receiver_term(g->ctx, core->as.dispatch.args[0], &applied, &got)) return idm_error_oom(err, core->span);
        IdmSymbol *rt_symbol = got && applied.kind == IDM_TYPE_CON ? applied.symbol : NULL;
        bool routed = expand_dispatch_route_field(g->ctx, node, rt_symbol, err);
        if (got) idm_type_term_destroy(&applied);
        if (!routed) return false;
        if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_FIELD_STATIC) {
            const IdmDispatchFieldDef *chosen = &node->as.dispatch.fields[node->as.dispatch.route_index];
            if (chosen->has_contract) return idm_type_term_copy(out, &chosen->contract) || idm_error_oom(err, core->span);
        }
        return fresh_var(g, out) || idm_error_oom(err, core->span);
    }
    IdmTypeTerm receiver_type;
    bool has_receiver_type = false;
    memset(&receiver_type, 0, sizeof(receiver_type));
    if (core->as.dispatch.arg_count != 0) {
        if (!gen_core(g, env, core->as.dispatch.args[0], &receiver_type, err)) return false;
        has_receiver_type = true;
    }
    IdmTypeTerm routed_type;
    bool has_routed_type = false;
    if (core->as.dispatch.arg_count != 0 && !dispatch_receiver_term(g->ctx, core->as.dispatch.args[0], &routed_type, &has_routed_type)) {
        idm_type_term_destroy(&receiver_type);
        return idm_error_oom(err, core->span);
    }
    IdmSymbol *rt_symbol = has_routed_type && routed_type.kind == IDM_TYPE_CON ? routed_type.symbol : NULL;
    bool routed = expand_dispatch_route_method(g->ctx, node, rt_symbol, err);
    if (has_routed_type) idm_type_term_destroy(&routed_type);
    if (!routed) {
        idm_type_term_destroy(&receiver_type);
        return false;
    }
    const IdmCallableContract *c = NULL;
    IdmCallableContract tmp;
    bool tmp_owned = false;
    memset(&tmp, 0, sizeof(tmp));
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_DEVIRT) {
        const IdmDispatchImplDef *cand = &node->as.dispatch.impls[node->as.dispatch.route_index];
        if (cand->passthrough && cand->primitive < IDM_PRIM_COUNT) {
            bool has = false;
            if (!idm_primitive_contract(g->ctx->rt, (IdmPrimitive)cand->primitive, core->as.dispatch.arg_count, &tmp, &has, err, core->span)) {
                idm_type_term_destroy(&receiver_type);
                return false;
            }
            if (has) { tmp_owned = true; c = &tmp; }
        } else {
            c = core_ref_contract(cand->ref);
        }
    } else if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_EVIDENCE) {
        c = core_ref_contract(node->as.dispatch.methods[node->as.dispatch.route_index].evidence);
    }
    bool ok = true;
    IdmTypeTerm *iargs = NULL;
    size_t iargc = 0;
    IdmTypeTerm iresult;
    bool ihas = false;
    memset(&iresult, 0, sizeof(iresult));
    if (c && c->sig_count != 0 && !idm_contract_sig_for(c, core->as.dispatch.arg_count)) {
        if (tmp_owned) idm_callable_contract_destroy(&tmp);
        idm_type_term_destroy(&receiver_type);
        return idm_error_set(err, core->span, "dispatch to '%s' has no contract signature for %zu arguments", core->as.dispatch.name ? core->as.dispatch.name : "<method>", core->as.dispatch.arg_count);
    }
    if (c) ok = inst_contract(g, c, core->as.dispatch.arg_count, &iargs, &iargc, &iresult, &ihas, err, core->span);
    if (tmp_owned) idm_callable_contract_destroy(&tmp);
    if (!ok) {
        idm_type_term_destroy(&receiver_type);
        return false;
    }
    const char *saved_owner = g->owner;
    for (size_t i = 0; ok && i < core->as.dispatch.arg_count; i++) {
        IdmTypeTerm at;
        memset(&at, 0, sizeof(at));
        if (i == 0) {
            if (!idm_type_term_copy(&at, &receiver_type)) { ok = idm_error_oom(err, core->span); break; }
        } else if (!gen_core(g, env, core->as.dispatch.args[i], &at, err)) {
            ok = false;
            break;
        }
        if (c && iargc == core->as.dispatch.arg_count) {
            if (core->as.dispatch.name) g->owner = core->as.dispatch.name;
            bool saved_may = g->may;
            g->may = true;
            ok = subsume(g, &iargs[i], &at, err, core->as.dispatch.args[i]->span, 0u);
            g->may = saved_may;
            g->owner = saved_owner;
        }
        idm_type_term_destroy(&at);
    }
    if (has_receiver_type) idm_type_term_destroy(&receiver_type);
    if (ok) {
        if (c && ihas && iargc == core->as.dispatch.arg_count) {
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

static bool union_member_push(IdmTypeTerm **members, size_t *count, size_t *cap, const IdmTypeTerm *m) {
    for (size_t i = 0; i < *count; i++) {
        const IdmTypeTerm *seen = &(*members)[i];
        if (idm_type_term_equal(seen, m)) return true;
        if (seen->kind == IDM_TYPE_CON && m->kind == IDM_TYPE_CON && seen->arg_count == 0 && m->arg_count == 0 && seen->symbol == m->symbol) return true;
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
    return union_member_push(members, count, cap, t);
}

static bool gen_union_alt(GenCtx *g, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    IdmTypeTerm ra;
    IdmTypeTerm rb;
    if (!idm_subst_apply(&g->subst, a, &ra)) return idm_error_oom(err, span);
    if (!idm_subst_apply(&g->subst, b, &rb)) {
        idm_type_term_destroy(&ra);
        return idm_error_oom(err, span);
    }
    if (idm_type_term_equal(&ra, &rb)) {
        idm_type_term_destroy(&rb);
        *out = ra;
        return true;
    }
    IdmTypeTerm *members = NULL;
    size_t count = 0;
    size_t cap = 0;
    bool collected = union_collect(g, &members, &count, &cap, &ra) && union_collect(g, &members, &count, &cap, &rb);
    idm_type_term_destroy(&ra);
    idm_type_term_destroy(&rb);
    if (!collected) {
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

static bool subsume_alternatives(GenCtx *g, const IdmTypeTerm *expected, const IdmTypeTerm *acc, IdmError *err, IdmSpan span) {
    IdmTypeTerm e;
    IdmTypeTerm a;
    if (!idm_subst_apply(&g->subst, expected, &e)) return idm_error_oom(err, span);
    if (!idm_subst_apply(&g->subst, acc, &a)) {
        idm_type_term_destroy(&e);
        return idm_error_oom(err, span);
    }
    bool ok = true;
    if (e.kind == IDM_TYPE_VAR && a.kind == IDM_TYPE_UNION) {
        IdmTypeTerm kept;
        memset(&kept, 0, sizeof(kept));
        IdmTypeTerm *members = NULL;
        size_t count = 0;
        size_t cap = 0;
        bool dropped = false;
        for (size_t i = 0; ok && i < a.arg_count; i++) {
            const IdmTypeTerm *m = &a.args[i];
            if (m->kind == IDM_TYPE_VAR && !m->rigid && m->var_id == e.var_id) {
                dropped = true;
                continue;
            }
            ok = union_member_push(&members, &count, &cap, m) || idm_error_oom(err, span);
        }
        if (ok && dropped) {
            if (count == 0) {
                free(members);
            } else if (count == 1) {
                ok = subsume(g, expected, &members[0], err, span, 0u);
                idm_type_term_destroy(&members[0]);
                free(members);
            } else if (!idm_type_compound(&kept, IDM_TYPE_UNION, members, count)) {
                for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i]);
                free(members);
                ok = idm_error_oom(err, span);
            } else {
                ok = subsume(g, expected, &kept, err, span, 0u);
                idm_type_term_destroy(&kept);
            }
            idm_type_term_destroy(&e);
            idm_type_term_destroy(&a);
            return ok;
        }
        for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i]);
        free(members);
    }
    idm_type_term_destroy(&e);
    idm_type_term_destroy(&a);
    return ok && subsume(g, expected, acc, err, span, 0u);
}

static bool gen_union(GenCtx *g, const IdmTypeTerm *a, const IdmTypeTerm *b, IdmTypeTerm *out, IdmError *err, IdmSpan span) {
    if (idm_type_term_equal(a, b)) return idm_type_term_copy(out, a) || idm_error_oom(err, span);
    if (a->kind == IDM_TYPE_VAR || b->kind == IDM_TYPE_VAR) {
        IdmError probe;
        idm_error_init(&probe);
        if (idm_unify(&g->subst, a, b, &probe, span)) return idm_subst_apply(&g->subst, a, out) || idm_error_oom(err, span);
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

static bool gen_core_node(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
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
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!gen_discard(g, env, core->as.string_concat.items[i], err)) return false;
            }
            return type_con_term(g, "string", out, err, core->span);
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!gen_discard(g, env, core->as.value_sequence.items[i], err)) return false;
            }
            return type_con_term(g, idm_value_sequence_kind_name(core->as.value_sequence.kind), out, err, core->span);
        case IDM_CORE_FORM_BUILD:
            if (!gen_discard(g, env, core->as.syntax_build.ctx, err)) return false;
            if (!gen_discard(g, env, core->as.syntax_build.payload, err)) return false;
            return type_con_term(g, "syntax", out, err, core->span);
        case IDM_CORE_RECORD_IS:
            if (!gen_discard(g, env, core->as.record_is.value, err)) return false;
            return type_con_term(g, "atom", out, err, core->span);
        case IDM_CORE_RECORD_CONSTRUCT: {
            const char *tname = idm_symbol_text(core->as.record_construct.type);
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                IdmTypeTerm vt;
                if (!gen_core(g, env, core->as.record_construct.field_values[i], &vt, err)) return false;
                bool ok = true;
                if (core->as.record_construct.field_has_contracts[i]) {
                    const char *saved = g->owner;
                    bool saved_may = g->may;
                    g->owner = tname;
                    g->may = true;
                    ok = subsume(g, &core->as.record_construct.field_contracts[i], &vt, err, core->as.record_construct.field_values[i]->span, 0u);
                    g->may = saved_may;
                    g->owner = saved;
                }
                idm_type_term_destroy(&vt);
                if (!ok) return false;
            }
            return type_con_symbol_term(core->as.record_construct.type, out, err, core->span);
        }
        case IDM_CORE_RECORD_FIELD: {
            if (!gen_discard(g, env, core->as.record_field.receiver, err)) return false;
            const TypeDef *td = type_by_identity(g->ctx, core->as.record_field.type);
            if (td && core->as.record_field.field_index < td->field_count && td->fields[core->as.record_field.field_index].has_contract)
                return idm_type_term_copy(out, &td->fields[core->as.record_field.field_index].contract) || idm_error_oom(err, core->span);
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        }
        case IDM_CORE_DISPATCH:
            return gen_dispatch(g, env, core, out, err);
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
            IdmTypeTerm *members = NULL;
            size_t member_count = 0;
            if (ok && sc == 1) {
                IdmTypeTerm s0;
                if (!idm_subst_apply(&g->subst, &st[0], &s0)) ok = idm_error_oom(err, core->span);
                else {
                    if (term_all_concrete(&s0)) ok = match_closed_members(g, &s0, &members, &member_count, err, core->span);
                    idm_type_term_destroy(&s0);
                }
            }
            size_t cc = core->as.match_expr.count;
            bool *useful = NULL;
            bool *covered = NULL;
            bool *clause_covers = NULL;
            bool residual = false;
            bool any_guard = false;
            if (ok && cc != 0) {
                bool uniform = true;
                for (size_t ci = 1; uniform && ci < cc; ci++) uniform = core->as.match_expr.clauses[ci].arity == core->as.match_expr.clauses[0].arity;
                IdmPatternSelectorClause *scl = uniform ? calloc(cc, sizeof(*scl)) : NULL;
                const char **names = member_count ? calloc(member_count, sizeof(*names)) : NULL;
                useful = calloc(cc, sizeof(*useful));
                covered = member_count ? calloc(member_count, sizeof(*covered)) : NULL;
                if (!useful || (uniform && !scl) || (member_count && (!names || !covered))) ok = idm_error_oom(err, core->span);
                for (size_t ci = 0; ok && ci < cc; ci++) {
                    const IdmFnClause *cl = &core->as.match_expr.clauses[ci];
                    if (cl->guard) any_guard = true;
                    useful[ci] = true;
                    if (!uniform) continue;
                    scl[ci].function_index = (uint32_t)ci;
                    scl[ci].arity = idm_arity_exact(cl->arity);
                    scl[ci].patterns = cl->param_patterns;
                    scl[ci].pattern_count = cl->pattern_count;
                    scl[ci].pattern_locals = cl->pattern_locals;
                    scl[ci].pattern_local_count = cl->pattern_local_count;
                    scl[ci].has_guard = cl->guard != NULL;
                }
                for (size_t m = 0; ok && m < member_count; m++) names[m] = members[m].kind == IDM_TYPE_CON ? idm_type_term_text(&members[m]) : NULL;
                clause_covers = member_count && cc ? calloc(cc * member_count, sizeof(*clause_covers)) : NULL;
                if (member_count && cc && !clause_covers) ok = idm_error_oom(err, core->span);
                if (ok && uniform) ok = idm_pattern_selector_usefulness(g->ctx->rt, scl, cc, names, member_count, cmp_name_eq, g->ctx, useful, covered, clause_covers, &residual, err);
                if (!uniform) residual = true;
                free(scl);
                free(names);
            }
            bool have = false;
            IdmTypeTerm acc;
            memset(&acc, 0, sizeof(acc));
            for (size_t ci = 0; ok && ci < cc; ci++) {
                const IdmFnClause *cl = &core->as.match_expr.clauses[ci];
                IdmSpan cspan = cl->body ? cl->body->span : core->span;
                for (size_t i = 0; ok && i < sc; i++) {
                    IdmTypeTerm refreshed;
                    if (!idm_subst_apply(&g->subst, &st[i], &refreshed)) { ok = idm_error_oom(err, cspan); break; }
                    idm_type_term_destroy(&st[i]);
                    st[i] = refreshed;
                }
                if (!ok) break;
                if (useful && !useful[ci]) {
                    idm_error_set(err, cspan, "match clause is unreachable");
                    ok = false;
                    break;
                }
                for (uint32_t i = 0; ok && i < cl->arity && i < sc && i < cl->pattern_count; i++) {
                    const IdmPattern *p = cl->param_patterns[i];
                    if (!p) continue;
                    IdmPatternProbe probe;
                    if (!idm_pattern_probe(g->ctx->rt, p, &probe)) { ok = idm_error_oom(err, cspan); continue; }
                    if (probe.has_head) {
                        IdmTypeTerm si;
                        if (!idm_subst_apply(&g->subst, &st[i], &si)) ok = idm_error_oom(err, cspan);
                        else {
                            if (!term_overlaps_head(g, &si, &probe, 0u)) {
                                IdmBuffer sb;
                                idm_buf_init(&sb);
                                bool w = idm_type_term_write(&sb, &si);
                                idm_error_set(err, p->span, "match clause on '%s' is unreachable for scrutinee type %s", probe.head_name, w && sb.data ? sb.data : "?");
                                idm_buf_destroy(&sb);
                                ok = false;
                            }
                            idm_type_term_destroy(&si);
                        }
                    }
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
                        ok = gen_union_alt(g, &acc, &bt, &u, err, core->span);
                        idm_type_term_destroy(&acc);
                        idm_type_term_destroy(&bt);
                        if (ok) acc = u; else memset(&acc, 0, sizeof(acc));
                    }
                } else {
                    idm_type_term_destroy(&bt);
                }
                if (ok && !cl->guard && sc == 1 && (!clause_covers || !member_count)) {
                    const IdmPattern *p0 = cl->pattern_count ? cl->param_patterns[0] : NULL;
                    IdmPatternProbe probe;
                    if (p0 && idm_pattern_probe(g->ctx->rt, p0, &probe) && probe.has_head && probe.full) {
                        IdmTypeTerm s0;
                        if (!idm_subst_apply(&g->subst, &st[0], &s0)) ok = idm_error_oom(err, core->span);
                        else {
                            if (s0.kind == IDM_TYPE_CON && s0.symbol && type_name_same(g->ctx, idm_type_term_text(&s0), probe.head_name)) {
                                IdmTypeTerm nr;
                                if (fresh_var(g, &nr)) {
                                    idm_type_term_destroy(&st[0]);
                                    st[0] = nr;
                                } else ok = idm_error_oom(err, core->span);
                            }
                            idm_type_term_destroy(&s0);
                        }
                    }
                }
                if (ok && !cl->guard && sc == 1 && clause_covers && member_count) {
                    IdmTypeTerm *kept = calloc(member_count, sizeof(*kept));
                    if (!kept) ok = idm_error_oom(err, core->span);
                    size_t kc = 0;
                    bool removed = false;
                    for (size_t m = 0; ok && m < member_count; m++) {
                        if (clause_covers[ci * member_count + m]) { removed = true; continue; }
                        ok = idm_type_term_copy(&kept[kc], &members[m]) || idm_error_oom(err, core->span);
                        if (ok) kc++;
                    }
                    if (ok && removed) {
                        IdmTypeTerm nr;
                        memset(&nr, 0, sizeof(nr));
                        if (kc == 0) ok = fresh_var(g, &nr) || idm_error_oom(err, core->span);
                        else if (kc == 1) { nr = kept[0]; kc = 0; }
                        else if (!idm_type_compound(&nr, IDM_TYPE_UNION, kept, kc)) ok = idm_error_oom(err, core->span);
                        else { kept = NULL; kc = 0; }
                        if (ok) {
                            idm_type_term_destroy(&st[0]);
                            st[0] = nr;
                        }
                    }
                    if (kept) {
                        for (size_t j = 0; j < kc; j++) idm_type_term_destroy(&kept[j]);
                        free(kept);
                    }
                }
                type_env_destroy(&nested);
            }
            if (ok && covered && !any_guard && !residual && g->total) {
                IdmTypeTerm *uncovered = calloc(member_count, sizeof(*uncovered));
                if (!uncovered) ok = idm_error_oom(err, core->span);
                size_t uc = 0;
                for (size_t m = 0; ok && m < member_count; m++) {
                    if (covered[m]) continue;
                    ok = idm_type_term_copy(&uncovered[uc], &members[m]) || idm_error_oom(err, core->span);
                    if (ok) uc++;
                }
                if (ok && uc != 0) {
                    IdmTypeTerm rem;
                    memset(&rem, 0, sizeof(rem));
                    if (uc == 1) {
                        rem = uncovered[0];
                        uc = 0;
                    } else if (!idm_type_compound(&rem, IDM_TYPE_UNION, uncovered, uc)) {
                        ok = idm_error_oom(err, core->span);
                    } else {
                        uc = 0;
                        uncovered = NULL;
                    }
                    if (ok) {
                        IdmBuffer rb;
                        idm_buf_init(&rb);
                        bool w = idm_type_term_write(&rb, &rem);
                        idm_error_set(err, core->span, "non-exhaustive match: %s is not covered", w && rb.data ? rb.data : "?");
                        idm_buf_destroy(&rb);
                        idm_type_term_destroy(&rem);
                        ok = false;
                    }
                }
                for (size_t m = 0; m < uc; m++) idm_type_term_destroy(&uncovered[m]);
                free(uncovered);
            }
            for (size_t m = 0; m < member_count; m++) idm_type_term_destroy(&members[m]);
            free(members);
            free(useful);
            free(covered);
            free(clause_covers);
            for (size_t i = 0; i < sc; i++) idm_type_term_destroy(&st[i]);
            free(st);
            if (!ok) { if (have) idm_type_term_destroy(&acc); return false; }
            if (have) { *out = acc; return true; }
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        }
        case IDM_CORE_COND: {
            IdmTypeTerm t;
            IdmTypeTerm e;
            if (!gen_discard(g, env, core->as.cond_expr.cond, err)) return false;
            if (!gen_core(g, env, core->as.cond_expr.then_branch, &t, err)) return false;
            if (!gen_core(g, env, core->as.cond_expr.else_branch, &e, err)) { idm_type_term_destroy(&t); return false; }
            bool ok = gen_union_alt(g, &t, &e, out, err, core->span);
            idm_type_term_destroy(&t);
            idm_type_term_destroy(&e);
            return ok;
        }
        case IDM_CORE_MATCH_BIND: {
            IdmTypeTerm vt;
            if (!gen_core(g, env, core->as.match_bind.value, &vt, err)) return false;
            const IdmCore *pf = core->as.match_bind.pattern_fn;
            TypeEnv seeded;
            memset(&seeded, 0, sizeof(seeded));
            bool ok = true;
            if (pf && pf->kind == IDM_CORE_FN) {
                IdmFnClause cl;
                memset(&cl, 0, sizeof(cl));
                cl.arity = pf->as.fn.arity;
                cl.param_patterns = pf->as.fn.param_patterns;
                cl.pattern_count = pf->as.fn.pattern_count;
                cl.pattern_locals = pf->as.fn.pattern_locals;
                cl.pattern_local_count = pf->as.fn.pattern_local_count;
                cl.guard = pf->as.fn.guard;
                cl.body = pf->as.fn.body;
                ok = seed_match_clause(g, &seeded, &cl, &vt, 1u, err);
            }
            idm_type_term_destroy(&vt);
            TypeEnv nested;
            memset(&nested, 0, sizeof(nested));
            nested.parent = env;
            for (size_t k = 0; ok && k < seeded.local_count; k++) {
                ok = type_env_set_local(&nested, seeded.locals[k].slot + core->as.match_bind.first_slot, &seeded.locals[k].term) ||
                     idm_error_oom(err, core->span);
            }
            type_env_destroy(&seeded);
            if (ok) ok = gen_core(g, &nested, core->as.match_bind.body, out, err);
            type_env_destroy(&nested);
            return ok;
        }
        case IDM_CORE_LIST_CONS:
            if (!gen_discard(g, env, core->as.list_pair.head, err)) return false;
            if (!gen_discard(g, env, core->as.list_pair.tail, err)) return false;
            return type_con_term(g, "pair", out, err, core->span);
        case IDM_CORE_LIST_APPEND:
            if (!gen_discard(g, env, core->as.list_pair.head, err)) return false;
            if (!gen_discard(g, env, core->as.list_pair.tail, err)) return false;
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        case IDM_CORE_FN: {
            IdmFnClause cl;
            memset(&cl, 0, sizeof(cl));
            cl.arity = core->as.fn.arity;
            cl.param_patterns = core->as.fn.param_patterns;
            cl.pattern_count = core->as.fn.pattern_count;
            cl.pattern_locals = core->as.fn.pattern_locals;
            cl.pattern_local_count = core->as.fn.pattern_local_count;
            cl.guard = core->as.fn.guard;
            cl.body = core->as.fn.body;
            if (!gen_fn_clause(g, &cl, err)) return false;
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        }
        case IDM_CORE_FN_MULTI:
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (core->as.fn_multi.clauses[i].primitive_backed) continue;
                if (!gen_fn_clause(g, &core->as.fn_multi.clauses[i], err)) return false;
            }
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        case IDM_CORE_LETREC: {
            TypeEnv nested;
            memset(&nested, 0, sizeof(nested));
            nested.parent = env;
            bool ok = true;
            for (size_t i = 0; ok && i < core->as.letrec.count; i++) {
                const IdmLetRecBinding *b = &core->as.letrec.bindings[i];
                IdmTypeTerm vt;
                ok = gen_core(g, &nested, b->value, &vt, err);
                if (!ok) break;
                if (!core->as.letrec.env) ok = type_env_set_local(&nested, b->slot, &vt) || idm_error_oom(err, core->span);
                idm_type_term_destroy(&vt);
            }
            if (ok) ok = gen_core(g, &nested, core->as.letrec.body, out, err);
            type_env_destroy(&nested);
            return ok;
        }
        case IDM_CORE_RECEIVE:
            if (!gen_discard(g, env, core->as.receive.receiver, err)) return false;
            if (!gen_discard(g, env, core->as.receive.timeout, err)) return false;
            if (!gen_discard(g, env, core->as.receive.timeout_body, err)) return false;
            return fresh_var(g, out) || idm_error_oom(err, core->span);
        case IDM_CORE_GUARD: {
            IdmTypeTerm bt;
            if (!gen_core(g, env, core->as.guard.body, &bt, err)) return false;
            bool ok = true;
            if (core->as.guard.handler) {
                TypeEnv nested;
                memset(&nested, 0, sizeof(nested));
                nested.parent = env;
                IdmTypeTerm ev;
                ok = fresh_var(g, &ev) || idm_error_oom(err, core->span);
                if (ok) {
                    ok = type_env_set_local(&nested, core->as.guard.rescue_slot, &ev) || idm_error_oom(err, core->span);
                    idm_type_term_destroy(&ev);
                }
                IdmTypeTerm ht;
                if (ok) ok = gen_core(g, &nested, core->as.guard.handler, &ht, err);
                type_env_destroy(&nested);
                if (ok) {
                    ok = gen_union_alt(g, &bt, &ht, out, err, core->span);
                    idm_type_term_destroy(&ht);
                }
                idm_type_term_destroy(&bt);
            } else {
                *out = bt;
            }
            if (ok && core->as.guard.cleanup) {
                TypeEnv nested;
                memset(&nested, 0, sizeof(nested));
                nested.parent = env;
                IdmTypeTerm ev;
                ok = fresh_var(g, &ev) || idm_error_oom(err, core->span);
                if (ok) {
                    ok = type_env_set_local(&nested, core->as.guard.ensure_slot, &ev) || idm_error_oom(err, core->span);
                    idm_type_term_destroy(&ev);
                }
                if (ok) ok = gen_discard(g, &nested, core->as.guard.cleanup, err);
                type_env_destroy(&nested);
                if (!ok) idm_type_term_destroy(out);
            }
            return ok;
        }
        case IDM_CORE_USE_PACKAGE:
            return gen_core(g, env, core->as.use_package.cont, out, err);
        default:
            return fresh_var(g, out) || idm_error_oom(err, core->span);
    }
}

static bool gen_harvest_record(GenCtx *g, const IdmCore *core, const IdmTypeTerm *term, IdmError *err) {
    if (!g->harvest_on || !core) return true;
    if (g->harvest_count == g->harvest_cap &&
        !idm_grow((void **)&g->harvest, &g->harvest_cap, sizeof(*g->harvest), 64u, g->harvest_count + 1u)) {
        return idm_error_oom(err, core->span);
    }
    if (!idm_type_term_copy(&g->harvest[g->harvest_count].term, term)) return idm_error_oom(err, core->span);
    g->harvest[g->harvest_count].core = core;
    g->harvest_count++;
    return true;
}

static bool gen_core(GenCtx *g, const TypeEnv *env, const IdmCore *core, IdmTypeTerm *out, IdmError *err) {
    if (!gen_core_node(g, env, core, out, err)) return false;
    return gen_harvest_record(g, core, out, err);
}

static bool seed_pattern_local_fresh(GenCtx *g, TypeEnv *env, const IdmFnClause *cl, const char *name, IdmError *err) {
    if (!name) return true;
    for (uint32_t k = 0; k < cl->pattern_local_count; k++) {
        if (!cl->pattern_locals[k].name || strcmp(cl->pattern_locals[k].name, name) != 0) continue;
        IdmTypeTerm fresh;
        if (!fresh_var(g, &fresh)) return idm_error_oom(err, idm_span_unknown(NULL));
        bool ok = type_env_set_local(env, cl->pattern_locals[k].slot, &fresh);
        idm_type_term_destroy(&fresh);
        return ok || idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool seed_pattern_local_typed(GenCtx *g, TypeEnv *env, const IdmFnClause *cl, const char *name, const char *type_name, IdmSymbol *type_symbol, IdmError *err) {
    if (!name) return true;
    for (uint32_t k = 0; k < cl->pattern_local_count; k++) {
        if (!cl->pattern_locals[k].name || strcmp(cl->pattern_locals[k].name, name) != 0) continue;
        IdmTypeTerm t;
        bool made = type_symbol ? type_con_symbol_term(type_symbol, &t, err, idm_span_unknown(NULL))
                                : type_con_term(g, type_name, &t, err, idm_span_unknown(NULL));
        if (!made) return false;
        bool ok = type_env_set_local(env, cl->pattern_locals[k].slot, &t);
        idm_type_term_destroy(&t);
        return ok || idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool seed_pattern_interior(GenCtx *g, TypeEnv *env, const IdmFnClause *cl, const IdmPattern *p, bool top, IdmError *err) {
    if (!p) return true;
    switch (p->kind) {
        case IDM_PAT_BIND:
            return top || seed_pattern_local_fresh(g, env, cl, p->as.name, err);
        case IDM_PAT_PAIR:
            return seed_pattern_interior(g, env, cl, p->as.pair.left, false, err) &&
                   seed_pattern_interior(g, env, cl, p->as.pair.right, false, err);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < p->as.seq.count; i++) {
                if (!seed_pattern_interior(g, env, cl, p->as.seq.items[i], false, err)) return false;
            }
            return true;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < p->as.seq_rest.count; i++) {
                if (!seed_pattern_interior(g, env, cl, p->as.seq_rest.items[i], false, err)) return false;
            }
            return seed_pattern_interior(g, env, cl, p->as.seq_rest.rest, false, err);
        case IDM_PAT_DICT:
            for (size_t i = 0; i < p->as.dict.count; i++) {
                if (!seed_pattern_interior(g, env, cl, p->as.dict.entries[i].pattern, false, err)) return false;
            }
            return seed_pattern_interior(g, env, cl, p->as.dict.rest, false, err);
        case IDM_PAT_BITS:
            for (size_t i = 0; i < p->as.bits.count; i++) {
                const IdmBitSeg *seg = &p->as.bits.segs[i];
                if (!seg->sub || seg->sub->kind != IDM_PAT_BIND) continue;
                const char *tn = seg->kind == IDM_BITSEG_INT ? "int"
                               : seg->kind == IDM_BITSEG_FLOAT ? "float"
                               : "bitstring";
                if (!seed_pattern_local_typed(g, env, cl, seg->sub->as.name, tn, NULL, err)) return false;
            }
            return true;
        case IDM_PAT_TYPE:
            if (p->as.type_test.sub && p->as.type_test.sub->kind == IDM_PAT_BIND) {
                return seed_pattern_local_typed(g, env, cl, p->as.type_test.sub->as.name, p->as.type_test.name, p->as.type_test.symbol, err);
            }
            return seed_pattern_interior(g, env, cl, p->as.type_test.sub, false, err);
        default:
            return true;
    }
}

static bool seed_match_clause(GenCtx *g, TypeEnv *nested, const IdmFnClause *cl, const IdmTypeTerm *scrut_types, size_t sc, IdmError *err) {
    IdmSpan span = cl->body ? cl->body->span : idm_span_unknown(NULL);
    for (uint32_t i = 0; i < cl->arity; i++) {
        const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
        const IdmTypeTerm *base = i < sc ? &scrut_types[i] : NULL;
        IdmTypeTerm refined;
        bool have_refined = false;
        if (!clause_slot_pin(g, cl, i, &refined, &have_refined, err)) return false;
        if (have_refined && base && base->kind == IDM_TYPE_VAR && base->rigid) {
            idm_type_term_destroy(&refined);
            have_refined = false;
            const IdmPattern *bp = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
            if (bp && bp->kind == IDM_PAT_BITS) {
                return idm_error_set(err, bp->span, "type mismatch for '%s': expected bitstring, got %s",
                                     g && g->owner ? g->owner : "<expr>", idm_type_term_text(base) ? idm_type_term_text(base) : "?");
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
        if (ok) ok = seed_pattern_interior(g, nested, cl, p, true, err);
        if (!ok) return idm_error_oom(err, span);
    }
    return true;
}

static void gen_ctx_init(GenCtx *g, ExpandContext *ctx, const char *owner) {
    g->ctx = ctx;
    g->owner = owner;
    g->may = false;
    idm_subst_init(&g->subst);
    idm_constraint_set_init(&g->wanted);
    idm_constraint_set_init(&g->given);
    g->siblings = NULL;
    g->sibling_count = 0;
    g->widen_lo = 0;
    g->widen_hi = 0;
    g->harvest = NULL;
    g->harvest_count = 0;
    g->harvest_cap = 0;
    g->harvest_on = false;
    g->total = false;
}
static void gen_ctx_destroy(GenCtx *g) {
    for (size_t i = 0; i < g->harvest_count; i++) idm_type_term_destroy(&g->harvest[i].term);
    free(g->harvest);
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
        IdmBuffer hb;
        idm_buf_init(&hb);
        bool hw = c->kind == IDM_CONSTR_STRUCTURAL && idm_structural_head_write(&hb, &c->structural);
        const char *tr = c->trait ? idm_symbol_text(c->trait) : "?";
        if (c->kind == IDM_CONSTR_STRUCTURAL) idm_error_set(err, span, "unsolved structural constraint '%s' for %s", hw && hb.data ? hb.data : "?", w && tb.data ? tb.data : "?");
        else idm_error_set(err, span, "unsolved typeclass '%s' for %s", tr, w && tb.data ? tb.data : "?");
        idm_buf_destroy(&hb);
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
    IdmTypeTerm pin;
    bool pinned = false;
    if (!clause_slot_pin(g, clause, i, &pin, &pinned, err)) return false;
    if (pinned && ptype && ptype->kind == IDM_TYPE_VAR && ptype->rigid) {
        idm_type_term_destroy(&pin);
        pinned = false;
        if (p->kind == IDM_PAT_BITS) {
            return idm_error_set(err, p->span, "type mismatch for '%s': expected bitstring, got %s",
                                 g && g->owner ? g->owner : "<expr>", idm_type_term_text(ptype) ? idm_type_term_text(ptype) : "?");
        }
    }
    const IdmTypeTerm *t = pinned ? &pin : ptype;
    bool ok = true;
    if (pinned) ok = type_env_set_arg(env, (uint32_t)i, &pin);
    if (ok && p->kind == IDM_PAT_BIND && p->as.name) {
        for (uint32_t k = 0; k < clause->pattern_local_count; k++) {
            if (clause->pattern_locals[k].name && strcmp(clause->pattern_locals[k].name, p->as.name) == 0) {
                ok = type_env_set_local(env, clause->pattern_locals[k].slot, t);
                break;
            }
        }
    }
    if (ok) ok = seed_pattern_interior(g, env, clause, p, true, err);
    if (pinned) idm_type_term_destroy(&pin);
    return ok || idm_error_oom(err, p->span);
}

static bool term_is_flex_var(const IdmTypeTerm *t) {
    return t && t->kind == IDM_TYPE_VAR && !t->rigid && t->var_id != 0u && strcmp(idm_type_term_text(t), "_") != 0;
}

typedef struct {
    IdmRuntime *rt;
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
    IdmGrowItem items[2] = {
        { .base = (void **)&qm->ids, .elem_size = sizeof(*qm->ids) },
        { .base = (void **)&qm->names, .elem_size = sizeof(*qm->names) },
    };
    if (!idm_growv(items, 2u, &qm->cap, 4u, qm->count + 1u)) return false;
    qm->names[qm->count] = quant_name_make(qm->count);
    if (!qm->names[qm->count]) return false;
    qm->ids[qm->count] = id;
    *out_index = qm->count;
    qm->count++;
    return true;
}

static bool quantify_applied(QuantMap *qm, const IdmTypeTerm *t, IdmTypeTerm *out) {
    memset(out, 0, sizeof(*out));
    if (term_is_flex_var(t)) {
        size_t idx = 0;
        if (!quant_map_intern(qm, t->var_id, &idx)) return false;
        return idm_type_var(qm->rt, out, qm->names[idx], (uint32_t)(idx + 1u), false);
    }
    if (t->kind == IDM_TYPE_VAR) return idm_type_term_copy(out, t);
    if (t->arg_count == 0) {
        if (t->kind == IDM_TYPE_CON) return idm_type_con_symbol(out, t->symbol);
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
    bool ok = t->kind == IDM_TYPE_CON ? idm_type_con_take_symbol(out, t->symbol, args, t->arg_count)
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
    qm.rt = g->ctx->rt;
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
            if (c->kind != IDM_CONSTR_CLASS && c->kind != IDM_CONSTR_STRUCTURAL) continue;
            IdmTypeTerm applied;
            if (!idm_subst_apply(&g->subst, &c->lhs, &applied)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            size_t idx = 0;
            bool member_var = term_is_flex_var(&applied) && quant_map_find(&qm, applied.var_id, &idx);
            idm_type_term_destroy(&applied);
            if (!member_var) continue;
            bool dup = false;
            for (size_t j = 0; j < cc; j++) {
                if (ctxc[j].lhs.var_id != (uint32_t)(idx + 1u) || ctxc[j].kind != c->kind) continue;
                if ((c->kind == IDM_CONSTR_CLASS && ctxc[j].trait == c->trait) ||
                    (c->kind == IDM_CONSTR_STRUCTURAL && idm_structural_head_equal(&ctxc[j].structural, &c->structural))) { dup = true; break; }
            }
            if (dup) continue;
            memset(&ctxc[cc], 0, sizeof(ctxc[cc]));
            ctxc[cc].kind = c->kind;
            if (!idm_type_var(g->ctx->rt, &ctxc[cc].lhs, qm.names[idx], (uint32_t)(idx + 1u), false)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            if (c->kind == IDM_CONSTR_CLASS) ctxc[cc].trait = c->trait;
            else if (!idm_structural_head_copy(&ctxc[cc].structural, &c->structural)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
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


static bool check_clause(ExpandContext *ctx, const IdmFnClause *clause, const IdmCallableContract *contract, const char *name, bool total, IdmError *err) {
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    g.harvest_on = true;
    g.total = total;
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
        if (c->kind != IDM_CONSTR_CLASS && c->kind != IDM_CONSTR_STRUCTURAL) continue;
        IdmTypeTerm rl;
        ok = rigidify_term(contract, &c->lhs, &rl);
        if (ok) {
            ok = c->kind == IDM_CONSTR_CLASS
                ? idm_constraint_set_add_class(&g.given, canonical_trait(&g, c->trait), &rl)
                : idm_constraint_set_add_structural(&g.given, &c->structural, &rl);
            idm_type_term_destroy(&rl);
        }
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
    if (ok && clause->guard) ok = gen_discard(&g, &env, clause->guard, err);
    if (ok) ok = gen_core(&g, &env, clause->body, &body, err);
    if (ok && sig && sig->has_result) {
        IdmTypeTerm rr;
        ok = rigidify_term(contract, &sig->result, &rr);
        if (ok) { ok = subsume(&g, &rr, &body, err, bspan, 0u); idm_type_term_destroy(&rr); }
    }
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.given, &g.wanted, cmp_given_matches, ctx_instance_oracle, ctx, &residual, err, bspan);
    if (ok && contract) ok = reject_residual(&g, &residual, err, bspan);
    if (ok) ok = gen_harvest_flush(&g, err);
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

static bool fn_body_match_pin(GenCtx *g, const IdmFnClause *cl, size_t i, IdmTypeTerm *out, bool *pinned, IdmError *err) {
    memset(out, 0, sizeof(*out));
    *pinned = false;
    const IdmCore *m = cl->body;
    if (!m || m->kind != IDM_CORE_MATCH || m->as.match_expr.count == 0) return true;
    size_t sc = m->as.match_expr.scrutinee_count;
    size_t j = sc;
    for (size_t k = 0; k < sc; k++) {
        const IdmCore *s = m->as.match_expr.scrutinees[k];
        if (s && s->kind == IDM_CORE_ARG_REF && s->as.slot_ref.slot == (uint32_t)i) { j = k; break; }
    }
    if (j == sc) return true;
    IdmTypeTerm acc;
    bool have = false;
    for (size_t ci = 0; ci < m->as.match_expr.count; ci++) {
        const IdmFnClause *mc = &m->as.match_expr.clauses[ci];
        IdmTypeTerm pin;
        bool cp = false;
        bool ok = (size_t)mc->arity == sc && clause_slot_pin(g, mc, j, &pin, &cp, err);
        if (ok && !cp) ok = false;
        if (!ok) {
            if (have) idm_type_term_destroy(&acc);
            return !err->present;
        }
        if (!have) { acc = pin; have = true; continue; }
        IdmTypeTerm u;
        ok = gen_union_alt(g, &acc, &pin, &u, err, m->span);
        idm_type_term_destroy(&pin);
        idm_type_term_destroy(&acc);
        have = false;
        if (!ok) return false;
        acc = u;
        have = true;
    }
    *out = acc;
    *pinned = have;
    return true;
}

static bool clause_infer_into(GenCtx *g, const IdmFnClause *cl, IdmTypeTerm *argacc, size_t argc, bool *arg_started, bool *arg_open, IdmTypeTerm *resacc, bool *res_started, IdmError *err) {
    TypeEnv env;
    memset(&env, 0, sizeof(env));
    IdmTypeTerm *cargs = argc ? calloc(argc, sizeof(*cargs)) : NULL;
    if (argc && !cargs) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; ok && i < argc; i++) {
        ok = fresh_var(g, &cargs[i]);
        if (ok) ok = type_env_set_arg(&env, i, &cargs[i]);
        if (ok) ok = seed_pattern_type(g, &env, cl, i, &cargs[i], err);
    }
    IdmTypeTerm body;
    memset(&body, 0, sizeof(body));
    if (ok && cl->guard) ok = gen_discard(g, &env, cl->guard, err);
    if (ok) ok = gen_core(g, &env, cl->body, &body, err);
    IdmTypeTerm result_now;
    memset(&result_now, 0, sizeof(result_now));
    bool have_result_now = ok && idm_subst_apply(&g->subst, &body, &result_now);
    if (ok && !have_result_now) ok = idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; ok && i < argc; i++) {
        IdmTypeTerm applied;
        memset(&applied, 0, sizeof(applied));
        bool have_applied = false;
        const IdmPattern *p = i < cl->pattern_count ? cl->param_patterns[i] : NULL;
        ok = clause_slot_pin(g, cl, i, &applied, &have_applied, err);
        if (ok && !have_applied) ok = fn_body_match_pin(g, cl, i, &applied, &have_applied, err);
        if (!ok) break;
        if (arg_open && !have_applied && (pattern_irrefutable(p) || (p && p->kind == IDM_PAT_PIN))) {
            IdmTypeTerm solved;
            if (!idm_subst_apply(&g->subst, &cargs[i], &solved)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            bool escapes = solved.kind != IDM_TYPE_VAR || solved.rigid;
            if (!escapes && have_result_now) escapes = idm_subst_occurs(&g->subst, solved.var_id, &result_now);
            for (size_t j = 0; !escapes && j < argc; j++) {
                if (j == i) continue;
                IdmTypeTerm other;
                if (!idm_subst_apply(&g->subst, &cargs[j], &other)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
                escapes = idm_subst_occurs(&g->subst, solved.var_id, &other);
                idm_type_term_destroy(&other);
            }
            idm_type_term_destroy(&solved);
            if (!ok) break;
            if (!escapes) arg_open[i] = true;
        }
        if (arg_open && arg_open[i]) {
            if (have_applied) idm_type_term_destroy(&applied);
            continue;
        }
        if (ok && !have_applied) {
            if (!idm_subst_apply(&g->subst, &cargs[i], &applied)) { ok = idm_error_oom(err, idm_span_unknown(NULL)); break; }
            have_applied = true;
        }
        if (!ok) { if (have_applied) idm_type_term_destroy(&applied); break; }
        if (!arg_started[i]) { argacc[i] = applied; arg_started[i] = true; }
        else {
            IdmTypeTerm u;
            ok = gen_union_alt(g, &argacc[i], &applied, &u, err, idm_span_unknown(NULL));
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
            ok = gen_union_alt(g, resacc, &rapplied, &u, err, idm_span_unknown(NULL));
            idm_type_term_destroy(&rapplied);
            if (ok) { idm_type_term_destroy(resacc); *resacc = u; }
        }
    }
    if (have_result_now) idm_type_term_destroy(&result_now);
    idm_type_term_destroy(&body);
    if (cargs) { for (size_t i = 0; i < argc; i++) idm_type_term_destroy(&cargs[i]); free(cargs); }
    type_env_destroy(&env);
    return ok;
}


static bool value_clauses_infer(GenCtx *g, const IdmCore *value, size_t argc, IdmTypeTerm *argacc, bool *arg_started, bool *arg_open, IdmTypeTerm *resacc, bool *res_started, IdmError *err) {
    if (value->kind == IDM_CORE_FN) {
        IdmFnClause cl;
        memset(&cl, 0, sizeof(cl));
        cl.arity = value->as.fn.arity;
        cl.param_patterns = value->as.fn.param_patterns;
        cl.pattern_count = value->as.fn.pattern_count;
        cl.pattern_locals = value->as.fn.pattern_locals;
        cl.pattern_local_count = value->as.fn.pattern_local_count;
        cl.guard = value->as.fn.guard;
        cl.body = value->as.fn.body;
        return clause_infer_into(g, &cl, argacc, argc, arg_started, arg_open, resacc, res_started, err);
    }
    if (value->kind == IDM_CORE_FN_MULTI) {
        for (size_t i = 0; i < value->as.fn_multi.count; i++) {
            if (value->as.fn_multi.clauses[i].arity != argc) continue;
            if (!clause_infer_into(g, &value->as.fn_multi.clauses[i], argacc, argc, arg_started, arg_open, resacc, res_started, err)) return false;
        }
        return true;
    }
    bool ok = gen_core(g, NULL, value, resacc, err);
    *res_started = ok;
    return ok;
}

static bool value_distinct_arities(const IdmCore *value, size_t **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    size_t cap = 0;
    size_t one = 0;
    const size_t *src = &one;
    size_t src_count = 1;
    if (value->kind == IDM_CORE_FN) {
        one = value->as.fn.arity;
    } else if (value->kind == IDM_CORE_FN_MULTI) {
        src = NULL;
        src_count = value->as.fn_multi.count;
    }
    for (size_t ci = 0; ci < src_count; ci++) {
        size_t a = src ? src[ci] : value->as.fn_multi.clauses[ci].arity;
        bool seen = false;
        for (size_t k = 0; k < *out_count; k++) {
            if ((*out)[k] == a) { seen = true; break; }
        }
        if (seen) continue;
        if (*out_count == cap && !idm_grow((void **)out, &cap, sizeof(**out), 4u, *out_count + 1u)) {
            free(*out);
            *out = NULL;
            return false;
        }
        (*out)[(*out_count)++] = a;
    }
    return true;
}

static bool infer_scheme(ExpandContext *ctx, const IdmCore *value, const char *name, IdmCallableContract *out, bool *out_has, IdmError *err) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    if (!value) return true;
    if (value->kind == IDM_CORE_FN_MULTI && value->as.fn_multi.count == 0) return true;
    size_t *arities = NULL;
    size_t arity_count = 0;
    if (!value_distinct_arities(value, &arities, &arity_count)) return idm_error_oom(err, value->span);
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    g.harvest_on = true;
    bool ok = true;
    GenSigInput *inputs = calloc(arity_count, sizeof(*inputs));
    IdmTypeTerm **accs = calloc(arity_count, sizeof(*accs));
    bool **starts = calloc(arity_count, sizeof(*starts));
    IdmTypeTerm *results = calloc(arity_count, sizeof(*results));
    bool *result_started = calloc(arity_count, sizeof(*result_started));
    if (!inputs || !accs || !starts || !results || !result_started) ok = idm_error_oom(err, value->span);
    size_t allocated = 0;
    for (size_t k = 0; ok && k < arity_count; k++) {
        size_t argc = arities[k];
        accs[k] = argc ? calloc(argc, sizeof(*accs[k])) : NULL;
        starts[k] = argc ? calloc(argc, sizeof(*starts[k])) : NULL;
        bool *open_flags = argc ? calloc(argc, sizeof(*open_flags)) : NULL;
        allocated = k + 1u;
        if (argc && (!accs[k] || !starts[k] || !open_flags)) ok = idm_error_oom(err, value->span);
        if (ok && (value->kind == IDM_CORE_FN || value->kind == IDM_CORE_FN_MULTI)) {
            ok = value_clauses_infer(&g, value, argc, accs[k], starts[k], open_flags, &results[k], &result_started[k], err);
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
        }
    }
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.given, &g.wanted, cmp_given_matches, ctx_instance_oracle, ctx, &residual, err, value->span);
    if (ok) ok = gen_harvest_flush(&g, err);
    if (ok) ok = generalize_contract_sigs(&g, inputs, arity_count, &residual, out, err);
    if (ok) *out_has = true;
    idm_constraint_set_destroy(&residual);
    for (size_t k = 0; k < allocated; k++) {
        if (accs[k]) {
            for (size_t j2 = 0; j2 < arities[k]; j2++) if (starts[k] && starts[k][j2]) idm_type_term_destroy(&accs[k][j2]);
            free(accs[k]);
        }
        free(starts[k]);
        if (result_started[k]) idm_type_term_destroy(&results[k]);
    }
    gen_ctx_destroy(&g);
    free(inputs);
    free(accs);
    free(starts);
    free(results);
    free(result_started);
    free(arities);
    if (!ok) { idm_callable_contract_destroy(out); memset(out, 0, sizeof(*out)); *out_has = false; }
    return ok;
}

bool expand_typecheck_infer_scheme(ExpandContext *ctx, const IdmCore *value, const char *name, IdmCallableContract *out, bool *out_has, IdmError *err) {
    return infer_scheme(ctx, value, name, out, out_has, err);
}

static bool check_fn(ExpandContext *ctx, const IdmCore *value, const IdmCallableContract *contract, const char *name, bool total, IdmError *err) {
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
        return check_clause(ctx, &clause, contract, name, total, err);
    }
    if (value->kind == IDM_CORE_FN_MULTI) {
        for (size_t i = 0; i < value->as.fn_multi.count; i++) {
            if (!check_clause(ctx, &value->as.fn_multi.clauses[i], contract, name, total, err)) return false;
        }
        return true;
    }
    GenCtx g;
    gen_ctx_init(&g, ctx, name);
    g.harvest_on = true;
    g.total = total;
    IdmTypeTerm t;
    bool ok = gen_core(&g, NULL, value, &t, err);
    const IdmContractSig *vsig = contract ? idm_contract_sig_for(contract, 0u) : NULL;
    if (ok && vsig && vsig->has_result) ok = subsume(&g, &vsig->result, &t, err, value->span, 0u);
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.given, &g.wanted, cmp_given_matches, ctx_instance_oracle, ctx, &residual, err, value->span);
    if (ok && contract) ok = reject_residual(&g, &residual, err, value->span);
    if (ok) ok = gen_harvest_flush(&g, err);
    idm_constraint_set_destroy(&residual);
    idm_type_term_destroy(&t);
    gen_ctx_destroy(&g);
    return ok;
}

bool expand_typecheck_statement(ExpandContext *ctx, IdmCore **core, IdmError *err) {
    if (!core || !*core) return true;
    if (!check_fn(ctx, *core, NULL, NULL, false, err)) return false;
    return expand_lower_root(ctx, core, err);
}

bool fn_primitive_passthrough(const IdmCore *fn, uint32_t *out_primitive) {
    if (!fn || fn->kind != IDM_CORE_FN || fn->as.fn.guard || fn->as.fn.capture_count != 0) return false;
    for (uint32_t i = 0; i < fn->as.fn.pattern_count; i++) {
        const IdmPattern *p = fn->as.fn.param_patterns[i];
        if (p && p->kind != IDM_PAT_BIND) return false;
    }
    const IdmCore *body = fn->as.fn.body;
    if (!body || body->kind != IDM_CORE_CALL) return false;
    const IdmCore *callee = body->as.call.callee;
    if (!callee || callee->kind != IDM_CORE_FN_MULTI || callee->as.fn_multi.count != 1u || !callee->as.fn_multi.clauses[0].primitive_backed) return false;
    if (body->as.call.arg_count != fn->as.fn.arity) return false;
    for (size_t i = 0; i < body->as.call.arg_count; i++) {
        const IdmCore *arg = body->as.call.args[i];
        if (!arg || arg->kind != IDM_CORE_ARG_REF || arg->as.slot_ref.slot != (uint32_t)i) return false;
    }
    *out_primitive = (uint32_t)callee->as.fn_multi.clauses[0].primitive;
    return true;
}

static void contract_stamp_passthrough(IdmCallableContract *contract, const IdmCore *value) {
    uint32_t primitive = 0;
    contract->passthrough = fn_primitive_passthrough(value, &primitive);
    contract->primitive = contract->passthrough ? primitive : 0u;
}

static bool contract_set_invoked(IdmCallableContract *contract, const IdmArgMask *invoked) {
    for (size_t i = 0; i < contract->sig_count; i++) {
        IdmArgMask copy;
        if (!idm_arg_mask_copy(&copy, invoked)) return false;
        idm_arg_mask_destroy(&contract->sigs[i].invoked);
        contract->sigs[i].invoked = copy;
    }
    return true;
}

bool expand_typecheck_value(ExpandContext *ctx, const char *name, IdmCore **value, IdmCallableContract *contract, bool *has_contract, bool declared, IdmError *err) {
    if (!value || !*value || !has_contract) return true;
    if (*has_contract) {
        if (!check_fn(ctx, *value, contract, name, declared, err)) return false;
        if (!expand_lower_root(ctx, value, err)) return false;
        IdmArgMask invoked;
        uint8_t purity;
        if (!expand_typecheck_core_purity(ctx, *value, &purity, &invoked, err)) return false;
        contract->purity = purity;
        bool invoked_ok = contract_set_invoked(contract, &invoked);
        idm_arg_mask_destroy(&invoked);
        if (!invoked_ok) return idm_error_oom(err, (*value)->span);
        contract_stamp_passthrough(contract, *value);
        return true;
    }
    if (!infer_scheme(ctx, *value, name, contract, has_contract, err)) return false;
    if (!*has_contract) {
        if (!check_fn(ctx, *value, NULL, name, false, err)) return false;
        return expand_lower_root(ctx, value, err);
    }
    if (!expand_lower_root(ctx, value, err)) return false;
    IdmArgMask invoked;
    uint8_t purity;
    if (!expand_typecheck_core_purity(ctx, *value, &purity, &invoked, err)) return false;
    contract->purity = purity;
    bool invoked_ok = contract_set_invoked(contract, &invoked);
    idm_arg_mask_destroy(&invoked);
    if (!invoked_ok) return idm_error_oom(err, (*value)->span);
    contract_stamp_passthrough(contract, *value);
    return true;
}

static bool publish_inferred_contract(ExpandContext *ctx, const DefnGroup *group, const IdmCallableContract *contract, IdmError *err) {
    if (group->doc && !((IdmCallableContract *)contract)->doc) {
        ((IdmCallableContract *)contract)->doc = idm_strdup(group->doc);
        if (!contract->doc) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    const IdmBinding *b = binding_by_env_slot(ctx, group->name, group->slot);
    if (b && !idm_binding_table_set_contract(&ctx->bindings, b->id, contract)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!record_package_slot(ctx, group->name, group->slot, &group->scopes, group->arity, contract, group->exported)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

typedef struct {
    size_t argc;
    IdmTypeTerm *sargs;
    bool *open;
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
        free(m->msigs[k].open);
        idm_type_term_destroy(&m->msigs[k].sresult);
    }
    free(m->msigs);
    idm_callable_contract_destroy(&m->skel);
    memset(m, 0, sizeof(*m));
}

static bool group_member_skeleton(GenCtx *g, GroupMember *m, const IdmCore *value, IdmError *err) {
    m->shaped = false;
    if (!value) return true;
    size_t *arities = NULL;
    size_t arity_count = 0;
    if (value->kind == IDM_CORE_FN || value->kind == IDM_CORE_FN_MULTI) {
        if (!value_distinct_arities(value, &arities, &arity_count)) return (free(arities), idm_error_oom(err, value->span));
    }
    if (arity_count == 0) { free(arities); return true; }
    m->msigs = calloc(arity_count, sizeof(*m->msigs));
    if (!m->msigs) { free(arities); return idm_error_oom(err, value->span); }
    memset(&m->skel, 0, sizeof(m->skel));
    m->shaped = true;
    m->msig_count = arity_count;
    for (size_t k = 0; k < arity_count; k++) {
        MemberSig *ms = &m->msigs[k];
        ms->argc = arities[k];
        ms->sargs = ms->argc ? calloc(ms->argc, sizeof(*ms->sargs)) : NULL;
        if (ms->argc && !ms->sargs) return (free(arities), idm_error_oom(err, value->span));
        ms->open = ms->argc ? calloc(ms->argc, sizeof(*ms->open)) : NULL;
        if (ms->argc && !ms->open) return (free(arities), idm_error_oom(err, value->span));
        IdmContractSig *ssig = idm_contract_add_sig(&m->skel);
        if (!ssig) return (free(arities), idm_error_oom(err, value->span));
        if (ms->argc) {
            ssig->args = calloc(ms->argc, sizeof(*ssig->args));
            if (!ssig->args) return (free(arities), idm_error_oom(err, value->span));
            ssig->arg_count = ms->argc;
        }
        for (size_t j = 0; j < ms->argc; j++) {
            if (!fresh_var(g, &ms->sargs[j]) || !idm_type_term_copy(&ssig->args[j], &ms->sargs[j])) return (free(arities), idm_error_oom(err, value->span));
        }
        if (!fresh_var(g, &ms->sresult) || !idm_type_term_copy(&ssig->result, &ms->sresult)) return (free(arities), idm_error_oom(err, value->span));
        ssig->has_result = true;
    }
    free(arities);
    return true;
}

static bool overtype_member_covers(GenCtx *g, const TypeDef *td, const IdmTypeTerm *t) {
    const char *name = idm_type_term_text(t);
    if (t->kind != IDM_TYPE_CON || !name) return false;
    const char *id = type_def_identity_text(td);
    if (id && type_name_same(g->ctx, id, name)) return false;
    for (size_t i = 0; i < td->member_count; i++) {
        const IdmTypeTerm *m = &td->members[i].term;
        const char *member = idm_type_term_text(m);
        if (m->kind == IDM_TYPE_CON && member && type_name_same(g->ctx, member, name)) return true;
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
            if (!idm_type_con(g->ctx->rt, &wide, id)) ok = idm_error_oom(err, span);
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
        case IDM_CORE_FORM_BUILD:
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
        case IDM_CORE_MATCH_BIND:
            core_mark_sibling_refs(core->as.match_bind.pattern_fn, groups, count, deps);
            core_mark_sibling_refs(core->as.match_bind.value, groups, count, deps);
            core_mark_sibling_refs(core->as.match_bind.body, groups, count, deps);
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
        case IDM_CORE_DISPATCH:
            for (size_t i = 0; i < core->as.dispatch.arg_count; i++) core_mark_sibling_refs(core->as.dispatch.args[i], groups, count, deps);
            for (size_t i = 0; i < core->as.dispatch.method_count; i++) core_mark_sibling_refs(core->as.dispatch.methods[i].evidence, groups, count, deps);
            for (size_t i = 0; i < core->as.dispatch.impl_count; i++) core_mark_sibling_refs(core->as.dispatch.impls[i].ref, groups, count, deps);
            core_mark_sibling_refs(core->as.dispatch.fallback, groups, count, deps);
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

static uint8_t core_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, IdmError *err);
static uint8_t callable_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, IdmError *err);

static uint8_t purity_join(uint8_t a, uint8_t b) {
    return a < b ? a : b;
}

typedef struct {
    bool known;
    const IdmArgMask *mask;
    const IdmCallableContract *contract;
    size_t argc;
    IdmArgMask owned;
} InvokedSource;

static void invoked_source_destroy(InvokedSource *source) {
    idm_arg_mask_destroy(&source->owned);
}

static bool invoked_source_test(const InvokedSource *source, size_t index) {
    if (!source->known) return true;
    if (source->mask) return idm_arg_mask_test(source->mask, index);
    if (!source->contract) return false;
    if (source->argc != SIZE_MAX) {
        const IdmContractSig *sig = idm_contract_sig_for(source->contract, source->argc);
        return !sig || idm_arg_mask_test(&sig->invoked, index);
    }
    for (size_t i = 0; i < source->contract->sig_count; i++) {
        if (idm_arg_mask_test(&source->contract->sigs[i].invoked, index)) return true;
    }
    return false;
}

static bool purity_mark_invoked(IdmArgMask *mask, size_t index, IdmError *err, IdmSpan span) {
    if (!mask || idm_arg_mask_test(mask, index)) return true;
    return idm_arg_mask_set(mask, index) || idm_error_oom(err, span);
}

static uint8_t callee_purity_level(const ExpandContext *ctx, const IdmCore *callee, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, size_t argc, InvokedSource *out_invoked, IdmError *err) {
    memset(out_invoked, 0, sizeof(*out_invoked));
    out_invoked->argc = argc;
    if (!callee) return IDM_PURITY_IMPURE;
    switch (callee->kind) {
        case IDM_CORE_FN_MULTI:
            if (callee->as.fn_multi.count == 1u && callee->as.fn_multi.clauses[0].primitive_backed) {
                out_invoked->known = true;
                return idm_primitive_pure(callee->as.fn_multi.clauses[0].primitive) ? IDM_PURITY_CONST : IDM_PURITY_IMPURE;
            }
            out_invoked->known = true;
            out_invoked->mask = &out_invoked->owned;
            return callable_purity_level(ctx, callee, groups, fx, count, &out_invoked->owned, err);
        case IDM_CORE_FN:
            out_invoked->known = true;
            out_invoked->mask = &out_invoked->owned;
            return callable_purity_level(ctx, callee, groups, fx, count, &out_invoked->owned, err);
        case IDM_CORE_ARG_REF:
            if (!purity_mark_invoked(self_invoked, callee->as.slot_ref.slot, err, callee->span)) return IDM_PURITY_IMPURE;
            return IDM_PURITY_ARGS;
        case IDM_CORE_CAPTURE_REF:
            return IDM_PURITY_ARGS;
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF: {
            const char *name = callee->kind == IDM_CORE_PACKAGE_REF ? callee->as.package_ref.name : callee->as.slot_ref.name;
            uint32_t slot = callee->kind == IDM_CORE_PACKAGE_REF ? callee->as.package_ref.slot : callee->as.slot_ref.slot;
            for (size_t i = 0; i < count; i++) {
                if (groups[i].slot == slot && groups[i].name && name && strcmp(groups[i].name, name) == 0) {
                    out_invoked->known = true;
                    out_invoked->mask = &fx[i].invoked;
                    return fx[i].level;
                }
            }
            const IdmCallableContract *c = core_ref_contract(callee);
            if (!c && callee->kind == IDM_CORE_ENV_REF) {
                const IdmBinding *b = binding_by_env_slot(ctx, callee->as.slot_ref.name, callee->as.slot_ref.slot);
                if (b && b->has_contract) c = &b->contract;
            }
            if (!c) return IDM_PURITY_IMPURE;
            out_invoked->known = c->sig_count != 0;
            out_invoked->contract = c;
            return c->purity;
        }
        default:
            return IDM_PURITY_IMPURE;
    }
}

static uint8_t call_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, IdmError *err) {
    InvokedSource callee_invoked;
    uint8_t callee_level = callee_purity_level(ctx, core->as.call.callee, groups, fx, count, self_invoked, core->as.call.arg_count, &callee_invoked, err);
    if (callee_level == IDM_PURITY_IMPURE) {
        invoked_source_destroy(&callee_invoked);
        return IDM_PURITY_IMPURE;
    }
    bool invokes_args = callee_level == IDM_PURITY_ARGS;
    uint8_t level = callee_level == IDM_PURITY_CONST ? IDM_PURITY_CONST : IDM_PURITY_PURE;
    for (size_t i = 0; i < core->as.call.arg_count; i++) {
        const IdmCore *arg = core->as.call.args[i];
        if (!arg) continue;
        bool invoked_here = invokes_args && invoked_source_test(&callee_invoked, i);
        if (arg->kind == IDM_CORE_ARG_REF) {
            if (invoked_here) {
                if (!purity_mark_invoked(self_invoked, arg->as.slot_ref.slot, err, arg->span)) {
                    invoked_source_destroy(&callee_invoked);
                    return IDM_PURITY_IMPURE;
                }
                level = purity_join(level, IDM_PURITY_ARGS);
            }
            continue;
        }
        if (arg->kind == IDM_CORE_CAPTURE_REF || arg->kind == IDM_CORE_LOCAL_REF) {
            if (invoked_here) level = purity_join(level, IDM_PURITY_ARGS);
            continue;
        }
        if (arg->kind == IDM_CORE_ENV_REF || arg->kind == IDM_CORE_PACKAGE_REF) {
            if (invoked_here) {
                InvokedSource ignored;
                level = purity_join(level, callee_purity_level(ctx, arg, groups, fx, count, self_invoked, SIZE_MAX, &ignored, err));
            } else {
                level = purity_join(level, IDM_PURITY_PURE);
            }
            continue;
        }
        level = purity_join(level, core_purity_level(ctx, arg, groups, fx, count, self_invoked, err));
    }
    invoked_source_destroy(&callee_invoked);
    return level;
}

static uint8_t callable_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, IdmError *err) {
    if (core->kind == IDM_CORE_FN) {
        return purity_join(core_purity_level(ctx, core->as.fn.guard, groups, fx, count, self_invoked, err),
                           core_purity_level(ctx, core->as.fn.body, groups, fx, count, self_invoked, err));
    }
    if (core->kind == IDM_CORE_FN_MULTI) {
        if (core->as.fn_multi.count == 1u && core->as.fn_multi.clauses[0].primitive_backed) {
            return idm_primitive_pure(core->as.fn_multi.clauses[0].primitive) ? IDM_PURITY_CONST : IDM_PURITY_IMPURE;
        }
        uint8_t level = IDM_PURITY_CONST;
        for (size_t i = 0; i < core->as.fn_multi.count; i++) {
            level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].guard, groups, fx, count, self_invoked, err));
            level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].body, groups, fx, count, self_invoked, err));
        }
        return level;
    }
    return core_purity_level(ctx, core, groups, fx, count, self_invoked, err);
}

static uint8_t core_purity_level(const ExpandContext *ctx, const IdmCore *core, const DefnGroup *groups, const PurityFx *fx, size_t count, IdmArgMask *self_invoked, IdmError *err) {
    if (!core) return IDM_PURITY_CONST;
    switch (core->kind) {
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
            return IDM_PURITY_CONST;
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return IDM_PURITY_PURE;
        case IDM_CORE_RECEIVE:
        case IDM_CORE_GUARD:
        case IDM_CORE_USE_PACKAGE:
            return IDM_PURITY_IMPURE;
        case IDM_CORE_CALL:
            return call_purity_level(ctx, core, groups, fx, count, self_invoked, err);
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return purity_join(core_purity_level(ctx, core->as.list_pair.head, groups, fx, count, self_invoked, err),
                               core_purity_level(ctx, core->as.list_pair.tail, groups, fx, count, self_invoked, err));
        case IDM_CORE_VALUE_SEQUENCE: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.value_sequence.items[i], groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_FORM_BUILD:
            return purity_join(core_purity_level(ctx, core->as.syntax_build.ctx, groups, fx, count, self_invoked, err),
                               core_purity_level(ctx, core->as.syntax_build.payload, groups, fx, count, self_invoked, err));
        case IDM_CORE_STRING_CONCAT: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.string_concat.items[i], groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_COND:
            return purity_join(core_purity_level(ctx, core->as.cond_expr.cond, groups, fx, count, self_invoked, err),
                   purity_join(core_purity_level(ctx, core->as.cond_expr.then_branch, groups, fx, count, self_invoked, err),
                               core_purity_level(ctx, core->as.cond_expr.else_branch, groups, fx, count, self_invoked, err)));
        case IDM_CORE_MATCH: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.scrutinees[i], groups, fx, count, self_invoked, err));
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.clauses[i].guard, groups, fx, count, self_invoked, err));
                level = purity_join(level, core_purity_level(ctx, core->as.match_expr.clauses[i].body, groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_DO: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.do_expr.items[i], groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_BIND_LOCAL:
            return purity_join(core_purity_level(ctx, core->as.bind_local.value, groups, fx, count, self_invoked, err),
                               core_purity_level(ctx, core->as.bind_local.body, groups, fx, count, self_invoked, err));
        case IDM_CORE_MATCH_BIND:
            return purity_join(core_purity_level(ctx, core->as.match_bind.pattern_fn, groups, fx, count, NULL, err),
                   purity_join(core_purity_level(ctx, core->as.match_bind.value, groups, fx, count, self_invoked, err),
                               core_purity_level(ctx, core->as.match_bind.body, groups, fx, count, self_invoked, err)));
        case IDM_CORE_FN:
            return purity_join(core_purity_level(ctx, core->as.fn.guard, groups, fx, count, NULL, err),
                               core_purity_level(ctx, core->as.fn.body, groups, fx, count, NULL, err));
        case IDM_CORE_FN_MULTI: {
            if (core->as.fn_multi.count == 1u && core->as.fn_multi.clauses[0].primitive_backed) {
                return idm_primitive_pure(core->as.fn_multi.clauses[0].primitive) ? IDM_PURITY_CONST : IDM_PURITY_IMPURE;
            }
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].guard, groups, fx, count, self_invoked, err));
                level = purity_join(level, core_purity_level(ctx, core->as.fn_multi.clauses[i].body, groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_LETREC: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.letrec.bindings[i].value, groups, fx, count, self_invoked, err));
            }
            return purity_join(level, core_purity_level(ctx, core->as.letrec.body, groups, fx, count, self_invoked, err));
        }
        case IDM_CORE_RECORD_CONSTRUCT: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.record_construct.field_values[i], groups, fx, count, self_invoked, err));
            }
            return level;
        }
        case IDM_CORE_RECORD_FIELD:
            return core_purity_level(ctx, core->as.record_field.receiver, groups, fx, count, self_invoked, err);
        case IDM_CORE_RECORD_IS:
            return core_purity_level(ctx, core->as.record_is.value, groups, fx, count, self_invoked, err);
        case IDM_CORE_DISPATCH: {
            uint8_t level = IDM_PURITY_CONST;
            for (size_t i = 0; i < core->as.dispatch.arg_count; i++) {
                level = purity_join(level, core_purity_level(ctx, core->as.dispatch.args[i], groups, fx, count, self_invoked, err));
            }
            if (core->as.dispatch.kind != IDM_DISPATCH_METHOD) return level;
            uint8_t outcome = IDM_PURITY_PURE;
            bool any = false;
            for (size_t i = 0; i < core->as.dispatch.method_count; i++) {
                const IdmCallableContract *c = core_ref_contract(core->as.dispatch.methods[i].evidence);
                outcome = purity_join(outcome, c ? c->purity : IDM_PURITY_IMPURE);
                any = true;
            }
            for (size_t i = 0; i < core->as.dispatch.impl_count; i++) {
                const IdmCallableContract *c = core_ref_contract(core->as.dispatch.impls[i].ref);
                outcome = purity_join(outcome, c ? c->purity : IDM_PURITY_IMPURE);
                any = true;
            }
            if (!any) outcome = IDM_PURITY_IMPURE;
            return purity_join(level, outcome);
        }
        default:
            return IDM_PURITY_IMPURE;
    }
}

bool expand_typecheck_core_purity(ExpandContext *ctx, const IdmCore *core, uint8_t *out_level, IdmArgMask *out_invoked, IdmError *err) {
    memset(out_invoked, 0, sizeof(*out_invoked));
    *out_level = callable_purity_level(ctx, core, NULL, NULL, 0u, out_invoked, err);
    if (err && err->present) {
        idm_arg_mask_destroy(out_invoked);
        return false;
    }
    return true;
}

bool expand_typecheck_purity(ExpandContext *ctx, const DefnGroup *groups, IdmCore **values, size_t count, PurityFx *out_fx, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        out_fx[i].level = IDM_PURITY_CONST;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < count; i++) {
            size_t invoked_before = idm_arg_mask_count(&out_fx[i].invoked);
            uint8_t level = callable_purity_level(ctx, values[i], groups, out_fx, count, &out_fx[i].invoked, err);
            if (err && err->present) return false;
            if (level < out_fx[i].level || idm_arg_mask_count(&out_fx[i].invoked) != invoked_before) {
                if (level < out_fx[i].level) out_fx[i].level = level;
                changed = true;
            }
        }
    }
    return true;
}

static bool defn_groups_finish(ExpandContext *ctx, const DefnGroup *groups, IdmCore **values, size_t count, PurityFx *purity, IdmCallableContract *inferred, bool *inferred_has, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        if (!expand_lower_root(ctx, &values[i], err)) return false;
    }
    if (!expand_typecheck_purity(ctx, groups, values, count, purity, err)) return false;
    for (size_t i = 0; i < count; i++) {
        const IdmCallableContract *base = groups[i].has_contract ? &groups[i].contract : (inferred_has && inferred_has[i] ? &inferred[i] : NULL);
        if (!base) continue;
        IdmCallableContract annotated;
        if (!idm_callable_contract_copy(&annotated, base)) return idm_error_oom(err, idm_span_unknown(NULL));
        annotated.purity = purity[i].level;
        if (!contract_set_invoked(&annotated, &purity[i].invoked)) {
            idm_callable_contract_destroy(&annotated);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        bool pok = publish_inferred_contract(ctx, &groups[i], &annotated, err);
        idm_callable_contract_destroy(&annotated);
        if (!pok) return false;
        if (purity[i].level == IDM_PURITY_CONST) expand_seed_const_defn(ctx, groups[i].slot, values[i]);
    }
    return true;
}

static void purity_fx_destroy(PurityFx *purity, size_t count) {
    for (size_t i = 0; i < count; i++) idm_arg_mask_destroy(&purity[i].invoked);
    free(purity);
}

bool expand_typecheck_defn_groups(ExpandContext *ctx, const DefnGroup *groups, IdmCore **values, size_t count, IdmError *err) {
    PurityFx *purity = count ? calloc(count, sizeof(*purity)) : NULL;
    if (count && !purity) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < count; i++) {
        if (!groups[i].has_contract) continue;
        if (!check_fn(ctx, values[i], &groups[i].contract, groups[i].name, true, err)) { purity_fx_destroy(purity, count); return false; }
    }
    size_t unannotated = 0;
    for (size_t i = 0; i < count; i++) {
        if (!groups[i].has_contract) unannotated++;
    }
    if (unannotated == 0) {
        bool fok = defn_groups_finish(ctx, groups, values, count, purity, NULL, NULL, err);
        purity_fx_destroy(purity, count);
        return fok;
    }
    GenCtx g;
    gen_ctx_init(&g, ctx, NULL);
    g.harvest_on = true;
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
            if (argc && (!argacc || !arg_started)) ok = idm_error_oom(err, values[i]->span);
            IdmTypeTerm resacc;
            memset(&resacc, 0, sizeof(resacc));
            bool res_started = false;
            if (ok) ok = value_clauses_infer(&g, values[i], argc, argacc, arg_started, ms->open, &resacc, &res_started, err);
            for (size_t j = 0; ok && j < argc; j++) {
                if (arg_started[j] && !ms->open[j]) ok = subsume_alternatives(&g, &ms->sargs[j], &argacc[j], err, values[i]->span);
            }
            if (ok && res_started) ok = subsume_alternatives(&g, &ms->sresult, &resacc, err, values[i]->span);
            for (size_t j = 0; ok && j < argc; j++) {
                if (!ms->open[j]) continue;
                char oname[64];
                snprintf(oname, sizeof(oname), "_open%zu_%zu", k, j);
                uint32_t oid = 0;
                if (g.ctx->type_var_seq >= 0x7fffffffu) { ok = idm_error_oom(err, gspan); break; }
                g.ctx->type_var_seq++;
                oid = 0x80000000u | g.ctx->type_var_seq;
                idm_type_term_destroy(&ms->sargs[j]);
                if (!idm_type_var(g.ctx->rt, &ms->sargs[j], oname, oid, false)) { ok = idm_error_oom(err, gspan); break; }
                if (k < members[i].skel.sig_count && j < members[i].skel.sigs[k].arg_count) {
                    idm_type_term_destroy(&members[i].skel.sigs[k].args[j]);
                    ok = idm_type_term_copy(&members[i].skel.sigs[k].args[j], &ms->sargs[j]) || idm_error_oom(err, gspan);
                }
                if (ok) {
                    IdmCallableContract *skel = &members[i].skel;
                    if (!idm_grow((void **)&skel->quantified, &skel->quantified_cap, sizeof(*skel->quantified), 4u, skel->quantified_count + 1u)) { ok = idm_error_oom(err, gspan); break; }
                    skel->quantified[skel->quantified_count] = idm_strdup(oname);
                    if (!skel->quantified[skel->quantified_count]) { ok = idm_error_oom(err, gspan); break; }
                    skel->quantified_count++;
                }
            }
            if (argacc) { for (size_t j = 0; j < argc; j++) if (arg_started[j]) idm_type_term_destroy(&argacc[j]); free(argacc); }
            free(arg_started);
            if (res_started) idm_type_term_destroy(&resacc);
        }
    }
    g.owner = NULL;
    IdmConstraintSet residual;
    idm_constraint_set_init(&residual);
    if (ok) ok = idm_solve(&g.subst, &g.given, &g.wanted, cmp_given_matches, ctx_instance_oracle, ctx, &residual, err, gspan);
    if (ok) ok = gen_harvest_flush(&g, err);
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
    IdmCallableContract *inferred = count ? calloc(count, sizeof(*inferred)) : NULL;
    bool *inferred_has = count ? calloc(count, sizeof(*inferred_has)) : NULL;
    if (ok && count && (!inferred || !inferred_has)) ok = idm_error_oom(err, gspan);
    for (size_t i = 0; ok && i < count; i++) {
        if (groups[i].has_contract || !members[i].shaped) continue;
        GenSigInput *inputs = calloc(members[i].msig_count ? members[i].msig_count : 1u, sizeof(*inputs));
        if (!inputs) { ok = idm_error_oom(err, gspan); break; }
        size_t input_count = members[i].msig_count;
        for (size_t k = 0; k < input_count; k++) {
            inputs[k].argc = members[i].msigs[k].argc;
            inputs[k].args = members[i].msigs[k].sargs;
            inputs[k].result = &members[i].msigs[k].sresult;
            inputs[k].has_result = true;
        }
        ok = generalize_contract_sigs(&g, inputs, input_count, &residual, &inferred[i], err);
        free(inputs);
        if (ok) inferred_has[i] = true;
    }
    idm_constraint_set_destroy(&residual);
    for (size_t i = 0; ok && i < count; i++) {
        if (!groups[i].has_contract && !members[i].shaped && !check_fn(ctx, values[i], NULL, groups[i].name, false, err)) ok = false;
    }
    if (ok) ok = defn_groups_finish(ctx, groups, values, count, purity, inferred, inferred_has, err);
    if (inferred) {
        for (size_t i = 0; i < count; i++) {
            if (inferred_has && inferred_has[i]) idm_callable_contract_destroy(&inferred[i]);
        }
    }
    free(inferred);
    free(inferred_has);
    if (members) {
        for (size_t i = 0; i < count; i++) group_member_destroy(&members[i]);
    }
    free(members);
    free(sibs);
    free(order);
    purity_fx_destroy(purity, count);
    gen_ctx_destroy(&g);
    return ok;
}
