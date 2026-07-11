#include "internal.h"

#include <stdarg.h>
#include <string.h>

#define IDM_FOLD_BUDGET 512

typedef struct {
    ExpandContext *ctx;
} LowerCtx;

typedef struct {
    uint32_t next_mint;
} LowerFrame;

static bool lower_core(LowerCtx *lc, LowerFrame *fr, IdmCore **slot, IdmError *err);

static const char *lower_type_name(const LowerCtx *lc, const IdmCore *core) {
    if (!core) return NULL;
    const IdmTypeTerm *term = expand_solved_type_lookup(lc->ctx, core);
    const char *name = idm_type_term_text(term);
    if (term && term->kind == IDM_TYPE_CON && name && strcmp(name, "->") != 0) return name;
    return NULL;
}

static IdmCore *lower_error(IdmError *err, IdmSpan span, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    idm_error_setv(err, span, fmt, args);
    va_end(args);
    return NULL;
}

static bool lower_replace(LowerCtx *lc, IdmCore **slot, IdmCore *replacement, const char *alias) {
    IdmCore *old = *slot;
    *slot = replacement;
    idm_core_free(old);
    if (!alias) return true;
    IdmTypeTerm term;
    if (!idm_type_con(lc->ctx->rt, &term, alias)) return false;
    return expand_solved_type_set(lc->ctx, replacement, &term);
}

static IdmCore *dispatch_take_args_call(IdmCore *node, IdmCore *callee, IdmError *err) {
    IdmCore *call = idm_core_call(callee, node->span);
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, node->span);
        return NULL;
    }
    for (size_t i = 0; i < node->as.dispatch.arg_count; i++) {
        if (!idm_core_call_add_arg(call, node->as.dispatch.args[i])) {
            idm_core_free(call);
            idm_error_oom(err, node->span);
            return NULL;
        }
        node->as.dispatch.args[i] = NULL;
    }
    return call;
}

static bool dispatch_impl_matches_type(const ExpandContext *ctx, const IdmDispatchImplDef *cand, const char *type) {
    if (!cand->type_identity || !type) return false;
    if (cand->type_identity[0] == '_' && cand->type_identity[1] == '.') return type_satisfies_structural_head(ctx, cand->type_identity, type);
    return type_name_same(ctx, cand->type_identity, type);
}

int expand_dispatch_select_impl(ExpandContext *ctx, const IdmCore *node, const char *receiver_type, uint32_t argc, size_t *out) {
    size_t chosen = SIZE_MAX;
    for (size_t c = 0; c < node->as.dispatch.impl_count; c++) {
        const IdmDispatchImplDef *cand = &node->as.dispatch.impls[c];
        if (!dispatch_impl_matches_type(ctx, cand, receiver_type)) continue;
        if (!idm_arity_accepts(&cand->arity, argc)) continue;
        if (chosen != SIZE_MAX) return -1;
        chosen = c;
    }
    if (chosen == SIZE_MAX) return 0;
    *out = chosen;
    return 1;
}

static bool dispatch_route_evidence(ExpandContext *ctx, IdmCore *node, IdmError *err) {
    (void)ctx;
    const char *method_name = node->as.dispatch.name ? node->as.dispatch.name : "?";
    IdmDispatchMethodDef *m = &node->as.dispatch.methods[0];
    if (m->evidence_state == IDM_DISPATCH_REF_NONE || !m->evidence) {
        return idm_error_set(err, node->span, "method '%s' has no dispatch selector", method_name);
    }
    if (m->evidence_state == IDM_DISPATCH_REF_INVISIBLE) {
        return idm_error_set(err, node->span, "method evidence '%s.%s' is not visible in this function", m->trait ? m->trait : "?", method_name);
    }
    node->as.dispatch.route = IDM_DISPATCH_ROUTE_EVIDENCE;
    node->as.dispatch.route_index = 0;
    return true;
}

bool expand_dispatch_route_method(ExpandContext *ctx, IdmCore *node, const char *receiver_type, IdmError *err) {
    const char *method_name = node->as.dispatch.name ? node->as.dispatch.name : "?";
    uint32_t argc = (uint32_t)node->as.dispatch.arg_count;
    IdmSpan span = node->span;
    if (receiver_type && strcmp(receiver_type, "Any") == 0) receiver_type = NULL;
    if (receiver_type) {
        size_t chosen = SIZE_MAX;
        int sel = expand_dispatch_select_impl(ctx, node, receiver_type, argc, &chosen);
        if (sel < 0) return idm_error_set(err, span, "ambiguous method '%s' on type '%s'", method_name, receiver_type);
        if (sel > 0) {
            const IdmDispatchImplDef *cand = &node->as.dispatch.impls[chosen];
            if (!cand->passthrough && cand->ref_state == IDM_DISPATCH_REF_INVISIBLE) {
                const char *trait = node->as.dispatch.methods[cand->method].trait;
                return idm_error_set(err, span, "method implementation '%s.%s' is not visible in this function", trait ? trait : "?", method_name);
            }
            node->as.dispatch.route = IDM_DISPATCH_ROUTE_DEVIRT;
            node->as.dispatch.route_index = (uint32_t)chosen;
            return true;
        }
        const TypeDef *td = type_def_lookup_name(ctx, receiver_type);
        if (td && td->member_count != 0) {
            const char *missing = NULL;
            bool covered = true;
            for (size_t m = 0; m < td->member_count && covered; m++) {
                const char *member = td->members[m].term.kind == IDM_TYPE_CON ? idm_type_term_text(&td->members[m].term) : NULL;
                if (!member) { covered = false; break; }
                size_t member_chosen = SIZE_MAX;
                if (expand_dispatch_select_impl(ctx, node, member, argc, &member_chosen) == 0) { covered = false; missing = member; }
            }
            if (covered && node->as.dispatch.method_count == 1u) return dispatch_route_evidence(ctx, node, err);
            if (missing) {
                return idm_error_set(err, span, "method '%s' is not implemented on member '%s' of receiver type '%s'", method_name, missing, receiver_type);
            }
        }
        return idm_error_set(err, span, "method '%s' is not implemented on receiver type '%s'", method_name, receiver_type);
    }
    if (node->as.dispatch.method_count != 1u) {
        return idm_error_set(err, span, "ambiguous method '%s' for a dynamic receiver", method_name);
    }
    return dispatch_route_evidence(ctx, node, err);
}

bool expand_dispatch_route_field(ExpandContext *ctx, IdmCore *node, const char *receiver_type, IdmError *err) {
    const char *field_name = node->as.dispatch.name ? node->as.dispatch.name : "<field>";
    IdmSpan span = node->span;
    if (receiver_type) {
        size_t chosen = SIZE_MAX;
        for (size_t i = 0; i < node->as.dispatch.field_count; i++) {
            if (!type_name_same(ctx, node->as.dispatch.fields[i].type_identity, receiver_type)) continue;
            if (chosen != SIZE_MAX) return idm_error_set(err, span, "ambiguous field '%s' on type '%s'", field_name, receiver_type);
            chosen = i;
        }
        if (chosen == SIZE_MAX) {
            return idm_error_set(err, span, "field '%s' is not available on receiver type '%s'", field_name, receiver_type);
        }
        node->as.dispatch.route = IDM_DISPATCH_ROUTE_FIELD_STATIC;
        node->as.dispatch.route_index = (uint32_t)chosen;
        return true;
    }
    if (node->as.dispatch.fallback_state != IDM_DISPATCH_REF_OK || !node->as.dispatch.fallback) {
        return idm_error_set(err, span, "field '%s' has no selector in this context", field_name);
    }
    node->as.dispatch.route = IDM_DISPATCH_ROUTE_FALLBACK;
    return true;
}

static const char *contract_result_alias(LowerCtx *lc, const IdmCallableContract *contract, uint32_t argc, IdmCore *const *args) {
    if (!contract) return NULL;
    const IdmContractSig *sig = idm_contract_sig_for(contract, argc);
    if (!sig || !sig->has_result) return NULL;
    if (sig->result.kind == IDM_TYPE_CON && sig->result.symbol && sig->result.arg_count == 0) return idm_type_term_text(&sig->result);
    if (sig->result.kind != IDM_TYPE_VAR || sig->result.rigid) return NULL;
    const char *name = NULL;
    bool saw_float = false;
    for (size_t i = 0; i < sig->arg_count && i < argc; i++) {
        if (sig->args[i].kind != IDM_TYPE_VAR || sig->args[i].var_id != sig->result.var_id) continue;
        const char *an = lower_type_name(lc, args[i]);
        if (!an) return NULL;
        bool an_num = strcmp(an, "int") == 0 || strcmp(an, "float") == 0;
        if (!name) name = an;
        else if (strcmp(name, an) != 0) {
            bool name_num = strcmp(name, "int") == 0 || strcmp(name, "float") == 0;
            if (!name_num || !an_num) return NULL;
        }
        if (an_num && strcmp(an, "float") == 0) saw_float = true;
    }
    if (name && saw_float) return "float";
    return name;
}

static IdmCore *lower_method_dispatch(LowerCtx *lc, IdmCore *node, const char **out_alias, IdmError *err) {
    IdmSpan span = node->span;
    uint32_t argc = (uint32_t)node->as.dispatch.arg_count;
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_DEVIRT) {
        IdmDispatchImplDef *cand = &node->as.dispatch.impls[node->as.dispatch.route_index];
        if (cand->passthrough && cand->primitive < IDM_PRIM_COUNT) {
            IdmCallableContract prim_contract;
            bool prim_has = false;
            IdmError probe;
            idm_error_init(&probe);
            if (idm_primitive_contract(lc->ctx->rt, (IdmPrimitive)cand->primitive, argc, &prim_contract, &prim_has, &probe, span) && prim_has) {
                *out_alias = contract_result_alias(lc, &prim_contract, argc, node->as.dispatch.args);
                idm_callable_contract_destroy(&prim_contract);
            }
            idm_error_clear(&probe);
            IdmCore *callee_call = expand_primitive_clause_call((IdmPrimitive)cand->primitive, span, err);
            if (!callee_call) return NULL;
            IdmCore *callee = callee_call->as.call.callee;
            callee_call->as.call.callee = NULL;
            idm_core_free(callee_call);
            return dispatch_take_args_call(node, callee, err);
        }
        if (!cand->ref) return lower_error(err, span, "internal: dispatch impl candidate has no reference");
        if (cand->ref->kind == IDM_CORE_PACKAGE_REF && cand->ref->as.package_ref.has_contract) {
            *out_alias = contract_result_alias(lc, &cand->ref->as.package_ref.contract, argc, node->as.dispatch.args);
        } else if (cand->ref->kind != IDM_CORE_PACKAGE_REF && cand->ref->as.slot_ref.has_contract) {
            *out_alias = contract_result_alias(lc, &cand->ref->as.slot_ref.contract, argc, node->as.dispatch.args);
        }
        IdmCore *callee = cand->ref;
        cand->ref = NULL;
        return dispatch_take_args_call(node, callee, err);
    }
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_EVIDENCE) {
        IdmDispatchMethodDef *m = &node->as.dispatch.methods[node->as.dispatch.route_index];
        if (!m->evidence) return lower_error(err, span, "internal: dispatch evidence candidate has no reference");
        IdmCore *callee = m->evidence;
        m->evidence = NULL;
        return dispatch_take_args_call(node, callee, err);
    }
    return lower_error(err, span, "internal: unrouted method dispatch reached lower");
}

static IdmCore *lower_field_dispatch(LowerCtx *lc, IdmCore *node, const char **out_alias, IdmError *err) {
    (void)lc;
    IdmSpan span = node->span;
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_FIELD_STATIC) {
        const IdmDispatchFieldDef *chosen = &node->as.dispatch.fields[node->as.dispatch.route_index];
        if (chosen->has_contract && chosen->contract.kind == IDM_TYPE_CON && chosen->contract.symbol && chosen->contract.arg_count == 0) {
            *out_alias = idm_type_term_text(&chosen->contract);
        }
        IdmCore *receiver = node->as.dispatch.args[0];
        node->as.dispatch.args[0] = NULL;
        IdmCore *field = idm_core_record_field(receiver, chosen->type, chosen->field, chosen->field_index, span);
        if (!field) {
            idm_core_free(receiver);
            idm_error_oom(err, span);
        }
        return field;
    }
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_FALLBACK) {
        if (!node->as.dispatch.fallback) return lower_error(err, span, "internal: dispatch fallback selector missing");
        IdmCore *callee = node->as.dispatch.fallback;
        node->as.dispatch.fallback = NULL;
        return dispatch_take_args_call(node, callee, err);
    }
    return lower_error(err, span, "internal: unrouted field dispatch reached lower");
}

static IdmCore *type_membership_chain(LowerCtx *lc, LowerFrame *fr, const char *bind_name, IdmCore *value, const TypeNameList *targets, IdmSpan span, IdmError *err) {
    ExpandContext *ctx = lc->ctx;
    uint32_t slot = fr->next_mint++;
    IdmCore *body = NULL;
    for (size_t i = targets->count; i > 0; i--) {
        IdmCore *test = expand_primitive_clause_call(IDM_PRIM_IS_A_P, span, err);
        IdmCore *ref = test ? idm_core_local_ref(bind_name, slot, span) : NULL;
        IdmCore *atom = ref ? idm_core_literal(idm_atom(ctx->rt, targets->items[i - 1u]), span) : NULL;
        if (atom && idm_core_call_add_arg(test, ref)) ref = NULL;
        if (!ref && atom && idm_core_call_add_arg(test, atom)) atom = NULL;
        if (ref || atom || !test) {
            idm_core_free(atom);
            idm_core_free(ref);
            idm_core_free(test);
            idm_core_free(body);
            idm_core_free(value);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        if (!body) {
            body = test;
        } else {
            IdmCore *truth = idm_core_literal(idm_bool(ctx->rt, true), span);
            IdmCore *branch = truth ? idm_core_cond(test, truth, body, span) : NULL;
            if (!branch) {
                idm_core_free(truth);
                idm_core_free(test);
                idm_core_free(body);
                idm_core_free(value);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
            body = branch;
        }
    }
    if (!body) body = idm_core_literal(idm_bool(ctx->rt, false), span);
    IdmCore *bound = body ? idm_core_bind_local(bind_name, slot, value, body, span) : NULL;
    if (!bound) {
        idm_core_free(value);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return bound;
}

static bool implements_target_add(LowerCtx *lc, const TraitDef *trait, const char *type, TypeNameList *out, IdmSpan span, IdmError *err) {
    if (!trait_impl_satisfies_type(lc->ctx, trait, type)) return true;
    if (is_a_target_names(lc->ctx, type, out, err)) return true;
    if (err && err->present) return false;
    return idm_error_set(err, span, "implements? cannot compile a runtime type test for type '%.*s'", (int)strcspn(type, "#"), type);
}

static IdmCore *lower_implements_dispatch(LowerCtx *lc, LowerFrame *fr, IdmCore *node, IdmError *err) {
    ExpandContext *ctx = lc->ctx;
    IdmSpan span = node->span;
    const char *trait_identity = node->as.dispatch.name;
    if (node->as.dispatch.route == IDM_DISPATCH_ROUTE_FOLD_TRUE || node->as.dispatch.route == IDM_DISPATCH_ROUTE_FOLD_FALSE) {
        IdmCore *literal = idm_core_literal(idm_bool(ctx->rt, node->as.dispatch.route == IDM_DISPATCH_ROUTE_FOLD_TRUE), span);
        if (!literal) idm_error_oom(err, span);
        return literal;
    }
    if (node->as.dispatch.route != IDM_DISPATCH_ROUTE_DYNAMIC) {
        return lower_error(err, span, "internal: unrouted implements dispatch reached lower");
    }
    const TraitDef *trait = typed_trait_by_identity(ctx, trait_identity);
    if (!trait || node->as.dispatch.arg_count == 0) {
        return lower_error(err, span, "unbound trait '%s'", trait_identity ? trait_identity : "?");
    }
    IdmCore *value = node->as.dispatch.args[0];
    TypeNameList targets = {0};
    bool ok = true;
    for (size_t i = 0; ok && trait_identity && i < ctx->typed.method_impl_count; i++) {
        const MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (!method_impl_matches_trait(ctx, impl, trait_identity)) continue;
        const char *type = method_impl_type_text(impl);
        if (!type) continue;
        if (type[0] == '_' && type[1] == '.') {
            for (size_t e = 0; ok && e < ctx->typed.entity_count; e++) {
                const TypedEntity *ent = &ctx->typed.entities[e];
                if (ent->kind != IDM_TYPED_ENTITY_TYPE || ent->as.type.field_count == 0) continue;
                const char *id = type_def_identity_text(&ent->as.type);
                if (!id || !type_satisfies_structural_head(ctx, type, id)) continue;
                ok = implements_target_add(lc, trait, id, &targets, span, err);
            }
        } else {
            ok = implements_target_add(lc, trait, type, &targets, span, err);
        }
    }
    if (!ok) {
        free(targets.items);
        return NULL;
    }
    node->as.dispatch.args[0] = NULL;
    IdmCore *chain = type_membership_chain(lc, fr, "implements-value", value, &targets, span, err);
    free(targets.items);
    return chain;
}

static bool is_a_static_member(ExpandContext *ctx, const char *type_name, const char *target) {
    if (strcmp(target, "Any") == 0) return true;
    const TypeDef *vt = typed_type_by_identity(ctx, type_name);
    const TypeDef *tt = typed_type_by_identity(ctx, target);
    if (vt || tt) {
        if (typed_type_same_identity(vt, tt)) return true;
        return strcmp(target, "record") == 0 && vt && vt->field_count != 0;
    }
    return type_name_same(ctx, target, type_name);
}

bool expand_core_is_primitive_call(const IdmCore *core, IdmPrimitive primitive) {
    if (!core || core->kind != IDM_CORE_CALL) return false;
    const IdmCore *callee = core->as.call.callee;
    return callee && callee->kind == IDM_CORE_FN_MULTI && callee->as.fn_multi.count == 1u &&
           callee->as.fn_multi.clauses[0].primitive_backed && callee->as.fn_multi.clauses[0].primitive == primitive;
}

static IdmCore *lower_is_a_call(LowerCtx *lc, LowerFrame *fr, IdmCore *call, IdmError *err) {
    ExpandContext *ctx = lc->ctx;
    if (!expand_core_is_primitive_call(call, IDM_PRIM_IS_A_P) || call->as.call.arg_count != 2u) return call;
    const IdmCore *targ = call->as.call.args[1];
    if (!targ || targ->kind != IDM_CORE_LITERAL) return call;
    const char *name = NULL;
    IdmValue tv = targ->as.literal;
    if (idm_value_tag(tv) == IDM_VAL_ATOM || idm_value_tag(tv) == IDM_VAL_WORD) name = idm_symbol_text(idm_value_symbol(tv));
    else if (idm_value_tag(tv) == IDM_VAL_STRING) name = idm_string_bytes(tv);
    if (!name || !name[0]) return call;
    TypeNameList targets = {0};
    if (!is_a_target_names(ctx, name, &targets, err)) {
        free(targets.items);
        if (err && !err->present) idm_error_set(err, call->span, "is-a? cannot compile a runtime type test for type '%.*s'", (int)strcspn(name, "#"), name);
        idm_core_free(call);
        return NULL;
    }
    IdmSpan span = call->span;
    const char *static_name = lower_type_name(lc, call->as.call.args[0]);
    if (static_name) {
        bool member = false;
        for (size_t i = 0; !member && i < targets.count; i++) member = is_a_static_member(ctx, static_name, targets.items[i]);
        free(targets.items);
        IdmCore *lit = idm_core_literal(idm_bool(ctx->rt, member), span);
        if (!lit) {
            idm_core_free(call);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        idm_core_free(call);
        return lit;
    }
    IdmCore *value = call->as.call.args[0];
    call->as.call.args[0] = NULL;
    idm_core_free(call);
    IdmCore *bound = type_membership_chain(lc, fr, "is-a-value", value, &targets, span, err);
    free(targets.items);
    return bound;
}

static IdmCore *lower_record_update_call(LowerCtx *lc, IdmCore *call, IdmError *err) {
    ExpandContext *ctx = lc->ctx;
    if (!expand_core_is_primitive_call(call, IDM_PRIM_RECORD_UPDATE) || call->as.call.arg_count != 2u) return call;
    IdmCore *dict = call->as.call.args[1];
    size_t pair_count = 0;
    bool dict_is_call = false;
    if (expand_core_is_primitive_call(dict, IDM_PRIM_DICT)) {
        size_t elems = dict->as.call.arg_count;
        if (elems == 0 || elems % 2u != 0) return call;
        pair_count = elems / 2u;
        for (size_t p = 0; p < pair_count; p++) {
            const IdmCore *key = dict->as.call.args[2u * p];
            if (!key || key->kind != IDM_CORE_LITERAL || idm_value_tag(key->as.literal) != IDM_VAL_ATOM) return call;
        }
        dict_is_call = true;
    } else if (dict && dict->kind == IDM_CORE_LITERAL && idm_value_tag(dict->as.literal) == IDM_VAL_DICT) {
        IdmDictIter it;
        if (!idm_dict_iter_init(dict->as.literal, &it)) return call;
        IdmValue k;
        IdmValue v;
        while (idm_dict_iter_next(&it, &k, &v)) {
            if (idm_value_tag(k) != IDM_VAL_ATOM) return call;
            pair_count++;
        }
        if (pair_count == 0) return call;
    } else {
        return call;
    }
    const char *tname = lower_type_name(lc, call->as.call.args[0]);
    if (!tname) return call;
    const TypeDef *type = NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *e = &ctx->typed.entities[i];
        if (e->kind != IDM_TYPED_ENTITY_TYPE || e->as.type.field_count == 0) continue;
        const char *id = type_def_identity_text(&e->as.type);
        if (!id || !type_name_same(ctx, id, tname)) continue;
        type = &e->as.type;
        break;
    }
    if (!type) return call;
    const char **keys = calloc(pair_count, sizeof(*keys));
    IdmCore **values = calloc(pair_count, sizeof(*values));
    if (!keys || !values) {
        free(keys);
        free(values);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, call->span);
    }
    IdmSpan span = call->span;
    bool ok = true;
    if (dict_is_call) {
        for (size_t p = 0; p < pair_count; p++) keys[p] = idm_symbol_text(idm_value_symbol(dict->as.call.args[2u * p]->as.literal));
    } else {
        IdmDictIter it;
        idm_dict_iter_init(dict->as.literal, &it);
        IdmValue k;
        IdmValue v;
        size_t p = 0;
        while (ok && idm_dict_iter_next(&it, &k, &v)) {
            keys[p] = idm_symbol_text(idm_value_symbol(k));
            values[p] = idm_core_literal(v, span);
            ok = values[p] != NULL;
            p++;
        }
    }
    for (size_t p = 0; ok && p < pair_count; p++) {
        bool found = false;
        for (size_t f = 0; !found && f < type->field_count; f++) {
            const char *name = type_field_name_text(&type->fields[f]);
            found = name && keys[p] && strcmp(name, keys[p]) == 0;
        }
        if (!found) {
            idm_error_set(err, span, "record '%s' has no field '%s'", type_def_identity_text(type), keys[p] ? keys[p] : "?");
            for (size_t q = 0; q < pair_count; q++) idm_core_free(values[q]);
            free(keys);
            free(values);
            idm_core_free(call);
            return NULL;
        }
    }
    IdmCore *construct = ok ? idm_core_record_construct(type_def_identity_symbol((TypeDef *)type), span) : NULL;
    ok = construct != NULL;
    for (size_t f = 0; ok && f < type->field_count; f++) {
        const TypeFieldDef *field = &type->fields[f];
        const char *name = type_field_name_text(field);
        size_t at = SIZE_MAX;
        for (size_t p = 0; p < pair_count; p++) {
            if (name && keys[p] && strcmp(name, keys[p]) == 0) at = p;
        }
        IdmCore *value = NULL;
        if (at != SIZE_MAX) {
            value = idm_core_arg_ref(name, (uint32_t)(1u + at), span);
        } else {
            IdmCore *recv = idm_core_arg_ref("record", 0u, span);
            value = recv ? expand_record_field_core(ctx, recv, type, (uint32_t)f, span, err) : NULL;
        }
        ok = value && idm_core_record_construct_add(construct, field->name, type_field_contract_term(field), value);
        if (!ok) idm_core_free(value);
    }
    IdmCore *fn = ok ? idm_core_fn("record-update", (uint32_t)(1u + pair_count), construct, span) : NULL;
    IdmCore *out = fn ? idm_core_call(fn, span) : NULL;
    if (!out) {
        if (construct && !fn) idm_core_free(construct);
        if (fn) idm_core_free(fn);
        for (size_t q = 0; q < pair_count; q++) idm_core_free(values[q]);
        free(keys);
        free(values);
        idm_core_free(call);
        if (!(err && err->present)) idm_error_oom(err, span);
        return NULL;
    }
    IdmCore *receiver = call->as.call.args[0];
    call->as.call.args[0] = NULL;
    if (!idm_core_call_add_arg(out, receiver)) {
        idm_core_free(receiver);
        idm_core_free(out);
        for (size_t q = 0; q < pair_count; q++) idm_core_free(values[q]);
        free(keys);
        free(values);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t p = 0; p < pair_count; p++) {
        IdmCore *value = values[p];
        values[p] = NULL;
        if (dict_is_call) {
            value = dict->as.call.args[2u * p + 1u];
            dict->as.call.args[2u * p + 1u] = NULL;
        }
        if (!idm_core_call_add_arg(out, value)) {
            idm_core_free(value);
            idm_core_free(out);
            for (size_t q = p + 1u; q < pair_count; q++) idm_core_free(values[q]);
            free(keys);
            free(values);
            idm_core_free(call);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    free(keys);
    free(values);
    idm_core_free(call);
    return out;
}

static bool fold_value_stack_push(IdmValue **items, size_t *count, size_t *cap, IdmValue *inline_items, IdmValue value) {
    if (*count == *cap) {
        size_t next = *cap > SIZE_MAX / 2u ? SIZE_MAX : *cap * 2u;
        if (next <= *cap || next > SIZE_MAX / sizeof(**items)) return false;
        IdmValue *grown = *items == inline_items ? malloc(next * sizeof(*grown)) : realloc(*items, next * sizeof(*grown));
        if (!grown) return false;
        if (*items == inline_items) memcpy(grown, inline_items, *count * sizeof(*grown));
        *items = grown;
        *cap = next;
    }
    (*items)[(*count)++] = value;
    return true;
}

static bool fold_value_embeddable(IdmValue root) {
    IdmValue inline_items[64];
    IdmValue *items = inline_items;
    size_t count = 1u;
    size_t cap = sizeof(inline_items) / sizeof(inline_items[0]);
    inline_items[0] = root;
    bool ok = true;
    while (count != 0 && ok) {
        IdmValue value = items[--count];
        switch (idm_value_tag(value)) {
            case IDM_VAL_NIL:
            case IDM_VAL_ATOM:
            case IDM_VAL_WORD:
            case IDM_VAL_INT:
            case IDM_VAL_FLOAT:
            case IDM_VAL_STRING:
            case IDM_VAL_BIGNUM:
            case IDM_VAL_BITSTRING:
                break;
            case IDM_VAL_PAIR: {
                IdmError probe;
                idm_error_init(&probe);
                IdmValue car = idm_car(value, &probe);
                IdmValue cdr = idm_cdr(value, &probe);
                ok = !probe.present &&
                     fold_value_stack_push(&items, &count, &cap, inline_items, car) &&
                     fold_value_stack_push(&items, &count, &cap, inline_items, cdr);
                idm_error_clear(&probe);
                break;
            }
            case IDM_VAL_TUPLE:
            case IDM_VAL_VECTOR: {
                size_t n = idm_sequence_count(value);
                for (size_t i = 0; i < n && ok; i++) {
                    IdmError probe;
                    idm_error_init(&probe);
                    IdmValue item = idm_sequence_item(value, i, &probe);
                    ok = !probe.present && fold_value_stack_push(&items, &count, &cap, inline_items, item);
                    idm_error_clear(&probe);
                }
                break;
            }
            case IDM_VAL_DICT: {
                IdmDictIter it;
                IdmValue key = idm_nil();
                IdmValue val = idm_nil();
                ok = idm_dict_iter_init(value, &it);
                while (ok && idm_dict_iter_next(&it, &key, &val)) {
                    ok = fold_value_stack_push(&items, &count, &cap, inline_items, key) &&
                         fold_value_stack_push(&items, &count, &cap, inline_items, val);
                }
                break;
            }
            default:
                ok = false;
                break;
        }
    }
    if (items != inline_items) free(items);
    return ok;
}


static IdmCore *lower_fold_primitive_call(LowerCtx *lc, IdmCore *call) {
    const IdmCore *callee = call->as.call.callee;
    if (!callee || callee->kind != IDM_CORE_FN_MULTI || callee->as.fn_multi.count != 1u) return call;
    if (!callee->as.fn_multi.clauses[0].primitive_backed) return call;
    IdmPrimitive prim = callee->as.fn_multi.clauses[0].primitive;
    if (!idm_primitive_pure(prim)) return call;
    const IdmPrimitiveInfo *info = idm_primitive_info(prim);
    if (!info || call->as.call.arg_count < info->min_arity || call->as.call.arg_count > info->max_arity) return call;
    IdmValue *args = call->as.call.arg_count ? malloc(call->as.call.arg_count * sizeof(*args)) : NULL;
    if (call->as.call.arg_count && !args) return call;
    for (size_t i = 0; i < call->as.call.arg_count; i++) {
        const IdmCore *arg = call->as.call.args[i];
        if (!arg || arg->kind != IDM_CORE_LITERAL) {
            free(args);
            return call;
        }
        args[i] = arg->as.literal;
    }
    IdmError probe;
    idm_error_init(&probe);
    IdmValue folded = idm_nil();
    bool ok = idm_prim_invoke(lc->ctx->rt, prim, args, (uint32_t)call->as.call.arg_count, &folded, &probe);
    free(args);
    if (!ok || probe.present) {
        idm_error_clear(&probe);
        return call;
    }
    IdmCore *lit = idm_core_literal(folded, call->span);
    if (!lit) return call;
    idm_core_free(call);
    return lit;
}

static IdmCore *lower_fold_pure_call(LowerCtx *lc, IdmCore *call) {
    ExpandContext *ctx = lc->ctx;
    if (!call || call->kind != IDM_CORE_CALL) return call;
    const IdmCore *callee = call->as.call.callee;
    if (!callee) return call;
    uint32_t slot = 0;
    const IdmCallableContract *contract = NULL;
    IdmEnv *env = NULL;
    if (callee->kind == IDM_CORE_ENV_REF) {
        slot = callee->as.slot_ref.slot;
        if (callee->as.slot_ref.has_contract) contract = &callee->as.slot_ref.contract;
        env = expand_unit_runtime_env(ctx);
    } else if (callee->kind == IDM_CORE_PACKAGE_REF) {
        slot = callee->as.package_ref.slot;
        IdmValue kv = callee->as.package_ref.env_key;
        IdmValueTag kt = idm_value_tag(kv);
        const char *key = kt == IDM_VAL_ATOM || kt == IDM_VAL_WORD ? idm_symbol_text(idm_value_symbol(kv)) : NULL;
        if (!key) return call;
        if (callee->as.package_ref.has_contract) contract = &callee->as.package_ref.contract;
        env = idm_package_env_get_or_create(ctx->rt, key);
    } else {
        return call;
    }
    if (!contract || !env) return call;
    if (contract->purity != IDM_PURITY_CONST || contract->context_count != 0) return call;
    IdmValue local_args[8];
    IdmValue *args = local_args;
    if (call->as.call.arg_count > 8u) {
        args = malloc(call->as.call.arg_count * sizeof(*args));
        if (!args) return call;
    }
    for (size_t i = 0; i < call->as.call.arg_count; i++) {
        const IdmCore *arg = call->as.call.args[i];
        if (!arg || arg->kind != IDM_CORE_LITERAL) {
            if (args != local_args) free(args);
            return call;
        }
        args[i] = arg->as.literal;
    }
    const IdmContractSig *sig = NULL;
    for (size_t k = 0; k < contract->sig_count; k++) {
        if (contract->sigs[k].arg_count == call->as.call.arg_count) { sig = &contract->sigs[k]; break; }
    }
    if (!sig) {
        if (args != local_args) free(args);
        return call;
    }
    for (size_t i = 0; i < sig->arg_count; i++) {
        if (!idm_value_matches_type_term(args[i], &sig->args[i])) {
            if (args != local_args) free(args);
            return call;
        }
    }
    IdmValue fn = idm_env_slot_get(env, slot);
    if (!idm_is_closure(fn)) {
        if (args != local_args) free(args);
        return call;
    }
    IdmError probe;
    idm_error_init(&probe);
    IdmValue out = idm_nil();
    bool ok = idm_vm_call_closure_budgeted(ctx->rt, fn, args, (uint32_t)call->as.call.arg_count, IDM_FOLD_BUDGET, &out, &probe);
    if (args != local_args) free(args);
    idm_error_clear(&probe);
    if (!ok || !fold_value_embeddable(out)) return call;
    IdmCore *lit = idm_core_literal(out, call->span);
    if (!lit) return call;
    idm_core_free(call);
    return lit;
}

static uint32_t clause_watermark(const IdmFnClause *clause) {
    uint32_t max = 0;
    for (uint32_t i = 0; i < clause->pattern_local_count; i++) {
        if (clause->pattern_locals[i].slot + 1u > max) max = clause->pattern_locals[i].slot + 1u;
    }
    uint32_t guard_max = idm_core_max_local_plus_one(clause->guard);
    uint32_t body_max = idm_core_max_local_plus_one(clause->body);
    if (guard_max > max) max = guard_max;
    if (body_max > max) max = body_max;
    return max;
}

static bool lower_clause(LowerCtx *lc, IdmFnClause *clause, IdmError *err) {
    LowerFrame fr;
    fr.next_mint = clause_watermark(clause);
    if (clause->guard && !lower_core(lc, &fr, &clause->guard, err)) return false;
    return lower_core(lc, &fr, &clause->body, err);
}

static bool lower_fn_value(LowerCtx *lc, IdmCore *core, IdmError *err) {
    if (core->kind == IDM_CORE_FN) {
        IdmFnClause clause;
        memset(&clause, 0, sizeof(clause));
        clause.pattern_locals = core->as.fn.pattern_locals;
        clause.pattern_local_count = core->as.fn.pattern_local_count;
        clause.guard = core->as.fn.guard;
        clause.body = core->as.fn.body;
        LowerFrame fr;
        fr.next_mint = clause_watermark(&clause);
        if (core->as.fn.guard && !lower_core(lc, &fr, &core->as.fn.guard, err)) return false;
        return lower_core(lc, &fr, &core->as.fn.body, err);
    }
    for (size_t i = 0; i < core->as.fn_multi.count; i++) {
        if (!lower_clause(lc, &core->as.fn_multi.clauses[i], err)) return false;
    }
    return true;
}

static bool lower_children(LowerCtx *lc, LowerFrame *fr, IdmCore *core, IdmError *err) {
    switch (core->kind) {
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
        case IDM_CORE_DISPATCH:
            for (size_t i = 0; i < core->as.dispatch.arg_count; i++) {
                if (!lower_core(lc, fr, &core->as.dispatch.args[i], err)) return false;
            }
            return true;
        case IDM_CORE_CALL:
            if (!lower_core(lc, fr, &core->as.call.callee, err)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                if (!lower_core(lc, fr, &core->as.call.args[i], err)) return false;
            }
            return true;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return lower_core(lc, fr, &core->as.list_pair.head, err) && lower_core(lc, fr, &core->as.list_pair.tail, err);
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!lower_core(lc, fr, &core->as.value_sequence.items[i], err)) return false;
            }
            return true;
        case IDM_CORE_FORM_BUILD:
            return lower_core(lc, fr, &core->as.syntax_build.ctx, err) && lower_core(lc, fr, &core->as.syntax_build.payload, err);
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!lower_core(lc, fr, &core->as.string_concat.items[i], err)) return false;
            }
            return true;
        case IDM_CORE_COND:
            return lower_core(lc, fr, &core->as.cond_expr.cond, err) &&
                   lower_core(lc, fr, &core->as.cond_expr.then_branch, err) &&
                   lower_core(lc, fr, &core->as.cond_expr.else_branch, err);
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                if (!lower_core(lc, fr, &core->as.match_expr.scrutinees[i], err)) return false;
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                IdmFnClause *clause = &core->as.match_expr.clauses[i];
                if (clause->guard && !lower_core(lc, fr, &clause->guard, err)) return false;
                if (!lower_core(lc, fr, &clause->body, err)) return false;
            }
            return true;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!lower_core(lc, fr, &core->as.do_expr.items[i], err)) return false;
            }
            return true;
        case IDM_CORE_BIND_LOCAL:
            return lower_core(lc, fr, &core->as.bind_local.value, err) && lower_core(lc, fr, &core->as.bind_local.body, err);
        case IDM_CORE_MATCH_BIND:
            return lower_core(lc, fr, &core->as.match_bind.pattern_fn, err) &&
                   lower_core(lc, fr, &core->as.match_bind.value, err) &&
                   lower_core(lc, fr, &core->as.match_bind.body, err);
        case IDM_CORE_FN:
        case IDM_CORE_FN_MULTI:
            return lower_fn_value(lc, core, err);
        case IDM_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!lower_core(lc, fr, &core->as.letrec.bindings[i].value, err)) return false;
            }
            return lower_core(lc, fr, &core->as.letrec.body, err);
        case IDM_CORE_RECEIVE:
            if (!lower_core(lc, fr, &core->as.receive.receiver, err)) return false;
            if (core->as.receive.timeout && !lower_core(lc, fr, &core->as.receive.timeout, err)) return false;
            if (core->as.receive.timeout_body && !lower_core(lc, fr, &core->as.receive.timeout_body, err)) return false;
            return true;
        case IDM_CORE_GUARD:
            if (!lower_core(lc, fr, &core->as.guard.body, err)) return false;
            if (core->as.guard.handler && !lower_core(lc, fr, &core->as.guard.handler, err)) return false;
            if (core->as.guard.cleanup && !lower_core(lc, fr, &core->as.guard.cleanup, err)) return false;
            return true;
        case IDM_CORE_USE_PACKAGE:
            return lower_core(lc, fr, &core->as.use_package.cont, err);
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (!lower_core(lc, fr, &core->as.record_construct.field_values[i], err)) return false;
            }
            return true;
        case IDM_CORE_RECORD_FIELD:
            return lower_core(lc, fr, &core->as.record_field.receiver, err);
        case IDM_CORE_RECORD_IS:
            return lower_core(lc, fr, &core->as.record_is.value, err);
    }
    return true;
}

static bool lower_slot_swap(LowerCtx *lc, IdmCore **slot, IdmCore *out, const char *alias, IdmError *err) {
    *slot = out;
    if (!alias) return true;
    IdmTypeTerm term;
    if (!idm_type_con(lc->ctx->rt, &term, alias)) return idm_error_oom(err, out->span);
    return expand_solved_type_set(lc->ctx, out, &term) || idm_error_oom(err, out->span);
}

static bool lower_core(LowerCtx *lc, LowerFrame *fr, IdmCore **slot, IdmError *err) {
    IdmCore *core = slot ? *slot : NULL;
    if (!core || core->lowered) return true;
    if (!lower_children(lc, fr, core, err)) return false;
    if (core->kind == IDM_CORE_DISPATCH) {
        const char *alias = lower_type_name(lc, core);
        const char *sel_alias = NULL;
        IdmCore *replacement = NULL;
        if (core->as.dispatch.kind == IDM_DISPATCH_METHOD) replacement = lower_method_dispatch(lc, core, &sel_alias, err);
        else if (core->as.dispatch.kind == IDM_DISPATCH_FIELD) replacement = lower_field_dispatch(lc, core, &sel_alias, err);
        else replacement = lower_implements_dispatch(lc, fr, core, err);
        if (!alias) alias = sel_alias;
        if (!replacement) return false;
        if (!lower_replace(lc, slot, replacement, alias)) return idm_error_oom(err, replacement->span);
        return lower_core(lc, fr, slot, err);
    }
    if (core->kind == IDM_CORE_COND && core->as.cond_expr.cond && core->as.cond_expr.cond->kind == IDM_CORE_LITERAL) {
        bool take_then = idm_value_ok(core->as.cond_expr.cond->as.literal);
        IdmCore *branch = take_then ? core->as.cond_expr.then_branch : core->as.cond_expr.else_branch;
        if (take_then) core->as.cond_expr.then_branch = NULL;
        else core->as.cond_expr.else_branch = NULL;
        *slot = branch;
        idm_core_free(core);
        return true;
    }
    if (core->kind == IDM_CORE_DO) {
        if (core->as.do_expr.count > 1) {
            size_t keep = 0;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                IdmCore *item = core->as.do_expr.items[i];
                if (i + 1u < core->as.do_expr.count && idm_core_statement_discardable(item)) {
                    idm_core_free(item);
                    continue;
                }
                core->as.do_expr.items[keep++] = item;
            }
            core->as.do_expr.count = keep;
        }
        if (core->as.do_expr.count == 0) {
            IdmCore *nil_lit = idm_core_literal(idm_nil(), core->span);
            if (!nil_lit) return idm_error_oom(err, core->span);
            *slot = nil_lit;
            idm_core_free(core);
        }
        return true;
    }
    if (core->kind == IDM_CORE_CALL) {
        const char *alias = lower_type_name(lc, core);
        IdmCore *out = lower_is_a_call(lc, fr, core, err);
        if (!out) {
            *slot = NULL;
            return false;
        }
        if (out != core) return lower_slot_swap(lc, slot, out, alias, err);
        out = lower_record_update_call(lc, core, err);
        if (!out) {
            *slot = NULL;
            return false;
        }
        if (out != core) return lower_slot_swap(lc, slot, out, alias, err);
        out = lower_fold_primitive_call(lc, core);
        if (out == core) out = lower_fold_pure_call(lc, core);
        if (out != core) return lower_slot_swap(lc, slot, out, alias, err);
        return true;
    }
    return true;
}

bool expand_lower_root(ExpandContext *ctx, IdmCore **root, IdmError *err) {
    if (!root || !*root || (*root)->lowered) return true;
    LowerCtx lc;
    memset(&lc, 0, sizeof(lc));
    lc.ctx = ctx;
    bool ok;
    if ((*root)->kind == IDM_CORE_FN || (*root)->kind == IDM_CORE_FN_MULTI) {
        ok = lower_fn_value(&lc, *root, err);
    } else {
        LowerFrame fr;
        fr.next_mint = idm_core_max_local_plus_one(*root);
        if (ctx->next_slot > fr.next_mint) fr.next_mint = ctx->next_slot;
        ok = lower_core(&lc, &fr, root, err);
    }
    if (ok && *root) (*root)->lowered = true;
    return ok;
}
