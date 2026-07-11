#include "internal.h"

#include <stddef.h>

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err);
static IdmSyntax *require_form_trait_word(const IdmSyntax *form, IdmError *err);
static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, IdmSymbol *identity);
static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, IdmSymbol *identity, IdmError *err, IdmSpan span);
static void trait_requirements_destroy(IdmTraitRequirementDef *requirements, size_t count);
static bool trait_method_impl_seen(ExpandContext *ctx, const char *trait, const IdmStructuralHead *structural, const char *type_display, const TraitMethodDef *method);
static bool check_trait_requirements_for_type(ExpandContext *ctx, const TraitDef *trait, const IdmStructuralHead *structural, const char *type_display, IdmSpan span, IdmError *err);
static bool install_required_trait_methods(ExpandContext *ctx, const TraitDef *required, const IdmSyntax *name_syntax, IdmError *err);
static bool method_signature_arity(ExpandContext *ctx, const IdmSyntax *form, IdmArity *out_arity, IdmError *err);
static void method_contract_stamp_default(TraitMethodDef *method, const IdmCore *fn);
static bool method_arity_mismatch(IdmError *err, IdmSpan span, const char *trait, const char *name, const IdmArity *expected, const IdmArity *got);
static bool method_implicit_trait_contract(ExpandContext *ctx, IdmSymbol *trait, const IdmArity *arity, IdmCallableContract *out, bool *out_has, IdmError *err, IdmSpan span);
static bool method_contract_ensure_trait_context(ExpandContext *ctx, IdmCallableContract *contract, IdmSymbol *trait, IdmError *err, IdmSpan span);
static bool method_contract_ensure_requirement_contexts(ExpandContext *ctx, IdmCallableContract *contract, const IdmTraitRequirementDef *requirements, size_t requirement_count, IdmError *err, IdmSpan span);
static TraitMethodDef *find_decl_method(ExpandContext *ctx, const char *name);
static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmArity arity, IdmError *err);
static void trait_impls_destroy(TraitMethodImpl *impls, size_t count);
static const TraitMethodDef *find_trait_method_def(ExpandContext *ctx, const TraitMethodDef *methods, size_t count, const char *name);
static bool trait_has_impl(ExpandContext *ctx, const TraitMethodImpl *impls, size_t count, const char *name);
static bool trait_impls_push(ExpandContext *ctx, TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, IdmArity arity, IdmCore *fn, IdmCallableContract *contract, bool has_contract, IdmError *err, IdmSpan span);
static IdmCore *build_trait_implement_core(ExpandContext *ctx, TraitDef *trait, const char *trait_key, IdmSymbol *type, const IdmStructuralHead *structural, const char *type_display, const IdmScopeSet *method_scopes, const IdmSyntax *body, IdmSpan span, IdmError *err);
static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported);
static bool type_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_member_start, bool *out_exported);
typedef struct {
    char *name;
    IdmSymbol *name_symbol;
    bool has_contract;
    IdmTypeTerm contract;
} RecordFieldDecl;
typedef struct {
    const IdmSyntax *name;
    IdmCallableContract contract;
    bool used;
} MethodSignature;
static void record_fields_destroy(RecordFieldDecl *fields, size_t count);
static void type_members_destroy(TypeMemberDef *members, size_t count);
static bool record_field_seen(const RecordFieldDecl *fields, size_t count, const char *name);
static bool parse_record_fields(ExpandContext *ctx, const IdmSyntax *body, RecordFieldDecl **out_fields, size_t *out_count, IdmError *err);
static bool resolve_record_field_contracts(ExpandContext *ctx, RecordFieldDecl *fields, size_t field_count, const char *record_name, IdmSymbol *identity, IdmError *err);
static bool resolve_type_members(ExpandContext *ctx, TypeMemberDef *members, size_t member_count, const char *self_name, IdmSymbol *identity, IdmError *err);
static bool resolve_pending_type_members(ExpandContext *ctx, const char *surface_name, IdmSymbol *identity, IdmSpan span, IdmError *err);
static bool parse_type_members(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, TypeMemberDef **out_members, size_t *out_count, IdmError *err);
static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const TypeDef *type, IdmSpan span, IdmError *err);
static IdmCore *record_predicate_fn(ExpandContext *ctx, const TypeDef *type, const char *predicate_name, IdmSpan span, IdmError *err);
static char *record_predicate_name(const char *record_name);
static bool register_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *type_name, IdmSymbol *identity, bool exported, const TypeMemberDef *members, size_t member_count, TypeDef **out_type, IdmSpan span, IdmError *err);
static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, IdmSymbol *identity, bool exported, const RecordFieldDecl *fields, size_t field_count, TypeDef **out_type, IdmSpan span, IdmError *err);
static IdmCore *trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmError *err);

static void *find_named(void *base, size_t count, size_t stride, size_t name_offset, const char *name) {
    char *items = base;
    for (size_t i = 0; i < count; i++) {
        void *item = items + i * stride;
        char *const *item_name = (char *const *)((char *)item + name_offset);
        if (*item_name && strcmp(*item_name, name) == 0) return item;
    }
    return NULL;
}

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 3 || !syn_is_word(form->as.seq.items[1], "method") || form->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    bool has_body = false;
    for (size_t i = 3; i < form->as.seq.count; i++) {
        const IdmSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || idm_syn_is_form_id(item, IDM_FORM_BODY)) {
            has_body = true;
            break;
        }
        if (item->kind != IDM_SYN_WORD) {
            if (err) idm_error_set(err, item->span, "method parameter must be an identifier");
            return false;
        }
    }
    *out_name = form->as.seq.items[2];
    *out_param_start = 3u;
    if (out_has_body) *out_has_body = has_body;
    return true;
}

typedef struct {
    const IdmSyntax *symbol;
    const IdmSyntax *target;
    size_t param_start;
    bool has_params;
    uint8_t precedence;
    IdmOpAssoc assoc;
    const char *capture;
} TraitOperatorForm;

static bool trait_operator_form_parts(const IdmSyntax *form, TraitOperatorForm *out, bool *out_is_operator, IdmError *err) {
    *out_is_operator = false;
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return true;
    if (form->as.seq.count < 3 || !syn_is_word(form->as.seq.items[1], "operator")) return true;
    *out_is_operator = true;
    memset(out, 0, sizeof(*out));
    out->assoc = IDM_OP_ASSOC_LEFT;
    out->capture = "infix";
    const IdmSyntax *symbol = form->as.seq.items[2];
    if (symbol->kind != IDM_SYN_STRING) return idm_error_set(err, symbol->span, "trait operator expects a string symbol");
    out->symbol = symbol;
    if (form->as.seq.count < 4 || form->as.seq.items[3]->kind != IDM_SYN_DICT) {
        return idm_error_set(err, form->span, "trait operator expects options: precedence, assoc, capture");
    }
    const IdmSyntax *opts = form->as.seq.items[3];
    bool has_precedence = false;
    for (size_t i = 0; i + 1u < opts->as.seq.count; i += 2u) {
        const IdmSyntax *key = opts->as.seq.items[i];
        const IdmSyntax *value = opts->as.seq.items[i + 1u];
        const char *key_text = key->kind == IDM_SYN_ATOM ? key->as.text : NULL;
        if (key_text && strcmp(key_text, "precedence") == 0) {
            if (value->kind != IDM_SYN_INT || value->as.integer < 0 || value->as.integer > 255) {
                return idm_error_set(err, value->span, "operator precedence must be an integer 0..255");
            }
            out->precedence = (uint8_t)value->as.integer;
            has_precedence = true;
        } else if (key_text && strcmp(key_text, "assoc") == 0) {
            const char *assoc_text = value->kind == IDM_SYN_WORD ? value->as.text : NULL;
            if (assoc_text && strcmp(assoc_text, "left") == 0) out->assoc = IDM_OP_ASSOC_LEFT;
            else if (assoc_text && strcmp(assoc_text, "right") == 0) out->assoc = IDM_OP_ASSOC_RIGHT;
            else if (assoc_text && strcmp(assoc_text, "none") == 0) out->assoc = IDM_OP_ASSOC_NONE;
            else return idm_error_set(err, value->span, "operator assoc must be left, right, or none");
        } else if (key_text && strcmp(key_text, "capture") == 0) {
            const char *capture_text = value->kind == IDM_SYN_ATOM ? value->as.text : NULL;
            if (capture_text && strcmp(capture_text, "infix") == 0) out->capture = "infix";
            else if (capture_text && strcmp(capture_text, "prefix") == 0) out->capture = "prefix";
            else if (capture_text && strcmp(capture_text, "postfix") == 0) out->capture = "postfix";
            else return idm_error_set(err, value->span, "trait operator capture must be :infix, :prefix, or :postfix");
        } else {
            return idm_error_set(err, key->span, "trait operator options are precedence, assoc, and capture");
        }
    }
    if (!has_precedence) return idm_error_set(err, form->span, "trait operator expects a precedence");
    if (form->as.seq.count > 4 && syn_is_word(form->as.seq.items[4], "->")) {
        if (form->as.seq.count != 6 || form->as.seq.items[5]->kind != IDM_SYN_WORD) {
            return idm_error_set(err, form->span, "trait operator target expects '-> METHOD'");
        }
        out->target = form->as.seq.items[5];
        return true;
    }
    bool has_body = false;
    for (size_t i = 4; i < form->as.seq.count; i++) {
        const IdmSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || idm_syn_is_form_id(item, IDM_FORM_BODY)) {
            has_body = true;
            break;
        }
        if (item->kind != IDM_SYN_WORD) {
            return idm_error_set(err, item->span, "trait operator parameter must be an identifier");
        }
    }
    if (!has_body) return idm_error_set(err, form->span, "trait operator expects parameters and a body, or '-> METHOD'");
    out->has_params = true;
    out->param_start = 4u;
    return true;
}

static IdmSyntax *require_form_trait_word(const IdmSyntax *form, IdmError *err) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return NULL;
    if (form->as.seq.count < 2u || !syn_is_word(form->as.seq.items[1], "require")) return NULL;
    if (form->as.seq.count < 3u) {
        if (err) idm_error_set(err, form->span, "trait requirement expects 'require TRAIT'");
        return NULL;
    }
    size_t chain_end = form->as.seq.count;
    IdmSyntax *word = expect_qualified_word_at(form->as.seq.items, 2u, &chain_end, "trait requirement expects a trait name", err);
    if (!word) return NULL;
    if (chain_end != form->as.seq.count) {
        idm_syn_free(word);
        if (err) idm_error_set(err, form->span, "trait requirement expects one trait name");
        return NULL;
    }
    return word;
}

static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, IdmSymbol *identity) {
    for (size_t i = 0; i < count; i++) if (requirements[i].identity == identity) return true;
    return false;
}

static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, IdmSymbol *identity, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)requirements, cap, sizeof(**requirements), 4u, *count + 1u)) return idm_error_oom(err, span);
    }
    IdmTraitRequirementDef *requirement = &(*requirements)[(*count)++];
    memset(requirement, 0, sizeof(*requirement));
    requirement->identity = identity;
    if (!requirement->identity) {
        (*count)--;
        return idm_error_oom(err, span);
    }
    return true;
}

static void trait_requirements_destroy(IdmTraitRequirementDef *requirements, size_t count) {
    for (size_t i = 0; i < count; i++) idm_trait_requirement_def_destroy(&requirements[i]);
    free(requirements);
}

static bool trait_method_impl_seen(ExpandContext *ctx, const char *trait, const IdmStructuralHead *structural, const char *type_display, const TraitMethodDef *method) {
    const char *method_name = trait_method_name_text(method);
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        const MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (!method_impl_matches_identity(ctx, impl, trait, method_name, NULL)) continue;
        if (structural ? !impl->structural || !idm_structural_head_equal(&impl->structural_head, structural)
                       : !method_impl_matches_type(ctx, impl, type_display)) continue;
        if (idm_arity_equal(&impl->arity, &method->arity)) return true;
    }
    return false;
}

static bool check_trait_requirements_for_type(ExpandContext *ctx, const TraitDef *trait, const IdmStructuralHead *structural, const char *type_display, IdmSpan span, IdmError *err) {
    const char *trait_name = trait_def_identity_text(trait);
    for (size_t i = 0; i < trait->requirement_count; i++) {
        IdmSymbol *required_identity = trait->requirements[i].identity;
        const char *required_name = idm_symbol_text(required_identity);
        const TraitDef *required = NULL;
        for (size_t j = 0; j < ctx->typed.entity_count; j++) {
            const TypedEntity *entity = &ctx->typed.entities[j];
            if (entity->kind == IDM_TYPED_ENTITY_TRAIT && entity->as.trait.name == required_identity) {
                required = &entity->as.trait;
                break;
            }
        }
        if (!required) return expand_error(err, span, "required trait '%s' is not available while implementing trait '%s'", required_name, trait_name);
        for (size_t m = 0; m < required->method_count; m++) {
            if (!trait_method_impl_seen(ctx, required_name, structural, type_display, &required->methods[m])) {
                return expand_error(err, span, "required trait '%s' is not implemented on type '%s' for trait '%s'", required_name, type_display, trait_name);
            }
        }
    }
    return true;
}

static bool install_required_trait_methods(ExpandContext *ctx, const TraitDef *required, const IdmSyntax *name_syntax, IdmError *err) {
    IdmScopeSet method_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &method_scopes)) return idm_error_oom(err, name_syntax->span);
    bool ok = true;
    for (size_t i = 0; i < required->method_count && ok; i++) {
        ok = install_method_surface(ctx, required->name, name_syntax->as.text, trait_method_name_text(&required->methods[i]), required->methods[i].arity, &method_scopes, ctx->unit, ctx->unit_key, required->methods[i].has_dispatch, required->dispatch_env, required->dispatch_env_key, required->methods[i].dispatch_slot, err);
    }
    idm_scope_set_destroy(&method_scopes);
    return ok;
}

static bool method_signature_arity(ExpandContext *ctx, const IdmSyntax *form, IdmArity *out_arity, IdmError *err) {
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    if (!method_form_parts(form, &name, &param_start, NULL, err)) return false;
    return function_literal_arity(ctx, name, form->as.seq.items, param_start, form->as.seq.count, out_arity, err);
}

static bool method_arity_mismatch(IdmError *err, IdmSpan span, const char *trait, const char *name, const IdmArity *expected, const IdmArity *got) {
    IdmBuffer expected_buf;
    IdmBuffer got_buf;
    idm_buf_init(&expected_buf);
    idm_buf_init(&got_buf);
    bool described = idm_arity_describe(&expected_buf, expected) && idm_arity_describe(&got_buf, got);
    bool ok = idm_error_set(err, span, "method '%s.%s' arity mismatch: expected %s got %s", trait, name, described ? expected_buf.data : "?", described ? got_buf.data : "?");
    idm_buf_destroy(&expected_buf);
    idm_buf_destroy(&got_buf);
    return ok;
}

static bool method_spec_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_contract_start) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    if (form->as.seq.count < 5u) return false;
    if (!syn_is_word(form->as.seq.items[1], "spec")) return false;
    if (form->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    const char *op = surface_token_text(form->as.seq.items[3]);
    if (!op || strcmp(op, "::") != 0) return false;
    if (out_name) *out_name = form->as.seq.items[2];
    if (out_contract_start) *out_contract_start = 4u;
    return true;
}

static void method_signatures_destroy(MethodSignature *signatures, size_t count) {
    for (size_t i = 0; i < count; i++) idm_callable_contract_destroy(&signatures[i].contract);
    free(signatures);
}

static bool method_signature_add(ExpandContext *ctx, MethodSignature **signatures, size_t *count, size_t *cap, const IdmSyntax *form, IdmError *err) {
    const IdmSyntax *name = NULL;
    size_t contract_start = 0;
    if (!method_spec_form_parts(form, &name, &contract_start)) return true;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*signatures)[i].name->as.text, name->as.text) == 0) return idm_error_set(err, name->span, "duplicate signature for method '%s'", name->as.text);
    }
    if (*count == *cap && !idm_grow((void **)signatures, cap, sizeof(**signatures), 4u, *count + 1u)) return idm_error_oom(err, name->span);
    MethodSignature *sig = &(*signatures)[(*count)++];
    memset(sig, 0, sizeof(*sig));
    sig->name = name;
    if (!parse_callable_contract_parts(ctx, (IdmSyntax *const *)form->as.seq.items, contract_start, form->as.seq.count, &sig->contract, err)) {
        (*count)--;
        return false;
    }
    return true;
}

static bool method_signatures_collect(ExpandContext *ctx, const IdmSyntax *body, MethodSignature **out, size_t *out_count, IdmError *err) {
    *out = NULL;
    *out_count = 0;
    size_t cap = 0;
    size_t body_count = body ? body->as.seq.count : 1u;
    for (size_t i = 1; i < body_count; i++) {
        if (!method_signature_add(ctx, out, out_count, &cap, body->as.seq.items[i], err)) {
            method_signatures_destroy(*out, *out_count);
            *out = NULL;
            *out_count = 0;
            return false;
        }
    }
    return true;
}

static bool method_signature_take(const IdmSyntax *name, MethodSignature *signatures, size_t count, IdmCallableContract *out, bool *out_has, IdmError *err) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < count; i++) {
        MethodSignature *sig = &signatures[i];
        if (strcmp(sig->name->as.text, name->as.text) != 0) continue;
        if (sig->used) return idm_error_set(err, name->span, "duplicate signature use for method '%s'", name->as.text);
        if (!idm_callable_contract_copy(out, &sig->contract)) return idm_error_oom(err, name->span);
        sig->used = true;
        *out_has = true;
        return true;
    }
    return true;
}

static bool method_signatures_all_used(const MethodSignature *signatures, size_t count, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        if (!signatures[i].used) return idm_error_set(err, signatures[i].name->span, "signature for method '%s' has no definition", signatures[i].name->as.text);
    }
    return true;
}

static bool method_contract_apply_arity(const IdmSyntax *name, const IdmCallableContract *contract, const IdmArity *arity, IdmError *err) {
    if (!contract) return true;
    if (contract->sigs[0].arg_count > UINT32_MAX) return idm_error_set(err, name->span, "signature for method '%s' has too many arguments", name->as.text);
    IdmArity expected = idm_arity_exact((uint32_t)contract->sigs[0].arg_count);
    if (idm_arity_equal(arity, &expected)) return true;
    IdmBuffer want;
    IdmBuffer got;
    idm_buf_init(&want);
    idm_buf_init(&got);
    bool ok = idm_arity_describe(&want, &expected) && idm_arity_describe(&got, arity);
    if (!ok) {
        idm_buf_destroy(&want);
        idm_buf_destroy(&got);
        return idm_error_oom(err, name->span);
    }
    bool set = idm_error_set(err, name->span, "signature for method '%s' expects arity %s, method has %s", name->as.text, want.data ? want.data : "?", got.data ? got.data : "?");
    idm_buf_destroy(&want);
    idm_buf_destroy(&got);
    return set;
}

static bool method_arity_single(const IdmArity *arity, uint32_t *out) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return false;
    if (arity->min != arity->max) return false;
    if (!idm_arity_accepts(arity, arity->min)) return false;
    if (out) *out = arity->min;
    return true;
}

static bool method_contract_add_trait_context(ExpandContext *ctx, IdmCallableContract *contract, IdmSymbol *trait, const IdmTypeTerm *receiver, IdmError *err, IdmSpan span) {
    (void)ctx;
    if (!idm_grow((void **)&contract->context, &contract->context_cap, sizeof(*contract->context), 4u, contract->context_count + 1u)) return idm_error_oom(err, span);
    IdmConstraint *constraint = &contract->context[contract->context_count];
    memset(constraint, 0, sizeof(*constraint));
    constraint->kind = IDM_CONSTR_CLASS;
    constraint->trait = trait;
    if (!constraint->trait || !idm_type_term_copy(&constraint->lhs, receiver)) {
        idm_constraint_destroy(constraint);
        return idm_error_oom(err, span);
    }
    contract->context_count++;
    return true;
}

static bool method_contract_ensure_trait_context(ExpandContext *ctx, IdmCallableContract *contract, IdmSymbol *trait, IdmError *err, IdmSpan span) {
    if (!contract || !trait || contract->sigs[0].arg_count == 0u) return true;
    const IdmTypeTerm *receiver = &contract->sigs[0].args[0];
    if (receiver->kind != IDM_TYPE_VAR) return true;
    for (size_t i = 0; i < contract->context_count; i++) {
        if (contract->context[i].trait == trait && typeclass_given_matches(ctx, &contract->context[i], idm_symbol_text(trait), receiver)) return true;
    }
    return method_contract_add_trait_context(ctx, contract, trait, receiver, err, span);
}

static bool method_contract_ensure_requirement_contexts(ExpandContext *ctx, IdmCallableContract *contract, const IdmTraitRequirementDef *requirements, size_t requirement_count, IdmError *err, IdmSpan span) {
    if (!contract || contract->sigs[0].arg_count == 0u) return true;
    const IdmTypeTerm *receiver = &contract->sigs[0].args[0];
    if (receiver->kind != IDM_TYPE_VAR) return true;
    for (size_t i = 0; i < requirement_count; i++) {
        IdmSymbol *trait = requirements[i].identity;
        bool present = false;
        for (size_t j = 0; j < contract->context_count; j++) {
            if (contract->context[j].trait == trait && typeclass_given_matches(ctx, &contract->context[j], idm_symbol_text(trait), receiver)) {
                present = true;
                break;
            }
        }
        if (!present && !method_contract_add_trait_context(ctx, contract, trait, receiver, err, span)) return false;
    }
    return true;
}

static bool method_implicit_trait_contract(ExpandContext *ctx, IdmSymbol *trait, const IdmArity *arity, IdmCallableContract *out, bool *out_has, IdmError *err, IdmSpan span) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    uint32_t argc = 0;
    if (!method_arity_single(arity, &argc) || argc == 0u) return true;
    out->quantified = calloc(1u, sizeof(*out->quantified));
    out->context = calloc(1u, sizeof(*out->context));
    IdmContractSig *osig = idm_contract_add_sig(out);
    if (osig) osig->args = calloc(argc, sizeof(*osig->args));
    if (!out->quantified || !out->context || !osig || !osig->args) {
        idm_callable_contract_destroy(out);
        return idm_error_oom(err, span);
    }
    out->quantified[0] = idm_strdup("a");
    if (!out->quantified[0]) {
        idm_callable_contract_destroy(out);
        return idm_error_oom(err, span);
    }
    out->quantified_count = 1u;
    osig->arg_count = argc;
    if (!idm_type_var(ctx->rt, &osig->args[0], "a", 1u, false)) {
        idm_callable_contract_destroy(out);
        return idm_error_oom(err, span);
    }
    for (uint32_t i = 1u; i < argc; i++) {
        if (!idm_type_var(ctx->rt, &osig->args[i], "_", 0u, false)) {
            idm_callable_contract_destroy(out);
            return idm_error_oom(err, span);
        }
    }
    out->context[0].kind = IDM_CONSTR_CLASS;
    out->context[0].trait = trait;
    if (!out->context[0].trait || !idm_type_term_copy(&out->context[0].lhs, &osig->args[0])) {
        idm_callable_contract_destroy(out);
        return idm_error_oom(err, span);
    }
    out->context_count = 1u;
    *out_has = true;
    return true;
}

static bool method_type_var_same(const IdmTypeTerm *a, const IdmTypeTerm *b) {
    if (!a || !b || a->kind != IDM_TYPE_VAR || b->kind != IDM_TYPE_VAR) return false;
    if (a->var_id != 0u || b->var_id != 0u) return a->var_id == b->var_id;
    return a->symbol == b->symbol;
}

static bool method_contract_replace_type(IdmTypeTerm *term, const IdmTypeTerm *var, const IdmTypeTerm *replacement, IdmError *err, IdmSpan span) {
    if (!term) return true;
    if (term->kind == IDM_TYPE_VAR && method_type_var_same(term, var)) {
        IdmTypeTerm copy;
        memset(&copy, 0, sizeof(copy));
        if (!idm_type_term_copy(&copy, replacement)) return idm_error_oom(err, span);
        idm_type_term_destroy(term);
        *term = copy;
        return true;
    }
    for (size_t i = 0; i < term->arg_count; i++) {
        if (!method_contract_replace_type(&term->args[i], var, replacement, err, span)) return false;
    }
    return true;
}

static bool method_contract_replace_receiver_var(ExpandContext *ctx, IdmCallableContract *contract, const char *type, IdmError *err, IdmSpan span) {
    if (!contract || contract->sig_count == 0 || contract->sigs[0].arg_count == 0 || contract->sigs[0].args[0].kind != IDM_TYPE_VAR) return true;
    IdmTypeTerm var;
    IdmTypeTerm replacement;
    memset(&var, 0, sizeof(var));
    memset(&replacement, 0, sizeof(replacement));
    if (!idm_type_term_copy(&var, &contract->sigs[0].args[0])) return idm_error_oom(err, span);
    const TypeDef *type_def = type_def_lookup_name(ctx, type);
    bool made_replacement = type_def ? idm_type_con_symbol(&replacement, type_def->identity)
                                     : idm_type_con(ctx->rt, &replacement, type);
    if (!made_replacement) {
        idm_type_term_destroy(&var);
        return idm_error_oom(err, span);
    }
    bool ok = true;
    for (size_t i = 0; ok && i < contract->sigs[0].arg_count; i++) ok = method_contract_replace_type(&contract->sigs[0].args[i], &var, &replacement, err, span);
    for (size_t i = 0; ok && i < contract->context_count; i++) {
        ok = method_contract_replace_type(&contract->context[i].lhs, &var, &replacement, err, span) &&
             method_contract_replace_type(&contract->context[i].rhs, &var, &replacement, err, span);
    }
    if (ok && contract->sigs[0].has_result) ok = method_contract_replace_type(&contract->sigs[0].result, &var, &replacement, err, span);
    idm_type_term_destroy(&replacement);
    idm_type_term_destroy(&var);
    return ok;
}

static bool method_contract_for_type(ExpandContext *ctx, const TraitMethodDef *method, const char *type, IdmCallableContract *out, bool *out_has, IdmError *err, IdmSpan span) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    if (!method || !method->has_contract || method->contract.sig_count == 0) return true;
    if (!idm_callable_contract_copy(out, &method->contract)) return idm_error_oom(err, span);
    *out_has = true;
    if (!method_contract_replace_receiver_var(ctx, out, type, err, span)) {
        idm_callable_contract_destroy(out);
        *out_has = false;
        return false;
    }
    return true;
}

static bool receiver_only_contract(const ExpandContext *ctx, const char *type, bool structural, const IdmArity *arity, IdmCallableContract *out, bool *out_has, IdmError *err, IdmSpan span) {
    *out_has = false;
    memset(out, 0, sizeof(*out));
    if (!type || structural) return true;
    uint32_t argc = 0;
    if (!method_arity_single(arity, &argc) || argc == 0u) return true;
    const TypeDef *td = type_def_lookup_name(ctx, type);
    const char *identity = td ? type_def_identity_text(td) : type;
    IdmContractSig *sig = idm_contract_add_sig(out);
    if (!sig) goto oom;
    sig->args = calloc(argc, sizeof(*sig->args));
    if (!sig->args) goto oom;
    sig->arg_count = argc;
    if (!idm_type_con(ctx->rt, &sig->args[0], identity)) goto oom;
    for (uint32_t i = 1u; i < argc; i++) {
        if (!idm_type_var(ctx->rt, &sig->args[i], "_", 0u, false)) goto oom;
    }
    *out_has = true;
    return true;
oom:
    idm_callable_contract_destroy(out);
    return idm_error_oom(err, span);
}

static bool trait_dispatch_env_storage(const ExpandContext *ctx) {
    return ctx->frame == IDM_FRAME_TOP && (ctx->repl_env_binds || ctx->in_package);
}

static TraitMethodDef *find_decl_method(ExpandContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->decl_method_count; i++) {
        if (trait_method_matches_name(ctx, &ctx->decl_methods[i], name)) return &ctx->decl_methods[i];
    }
    return NULL;
}

static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmArity arity, IdmError *err) {
    if (find_decl_method(ctx, name_syntax->as.text)) return idm_error_set(err, name_syntax->span, "trait '%s' declares method '%s' more than once", ctx->trait_name ? ctx->trait_name : "<anonymous>", name_syntax->as.text);
    if (ctx->decl_method_count == ctx->decl_method_cap) {
        if (!idm_grow((void **)&ctx->decl_methods, &ctx->decl_method_cap, sizeof(*ctx->decl_methods), 4u, ctx->decl_method_count + 1u)) return idm_error_oom(err, name_syntax->span);
    }
    TraitMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
    memset(method, 0, sizeof(*method));
    if (!trait_method_set_name(ctx, method, name_syntax->as.text, err, name_syntax->span)) return false;
    method->arity = arity;
    method->exported = true;
    method->has_dispatch = true;
    bool dispatch_env = trait_dispatch_env_storage(ctx);
    method->dispatch_slot = dispatch_env ? ctx->env_slot_seq++ : ctx->next_slot++;
    if (!binder_scopes_pruned(ctx, name_syntax, &method->scopes)) {
        trait_method_def_destroy(method);
        return idm_error_oom(err, name_syntax->span);
    }
    ctx->decl_method_count++;
    return install_method_surface(ctx, ctx->trait_identity, ctx->trait_key ? ctx->trait_key : (ctx->trait_name ? ctx->trait_name : "<anonymous>"), trait_method_name_text(method), arity, &method->scopes, ctx->unit, ctx->unit_key, true, dispatch_env, dispatch_env ? ctx->unit_key : NULL, method->dispatch_slot, err);
}

bool predeclare_trait_methods(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmError *err) {
    if (!ctx->trait_name) return true;
    for (size_t i = start; i < count; i++) {
        const IdmSyntax *name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(items[i], &name, &param_start, &has_body, NULL)) {
            TraitOperatorForm op;
            bool is_operator = false;
            if (!trait_operator_form_parts(items[i], &op, &is_operator, err)) return false;
            if (!is_operator || !op.has_params) continue;
            IdmArity op_arity = idm_arity_unknown();
            if (!function_literal_arity(ctx, op.symbol, items[i]->as.seq.items, op.param_start, items[i]->as.seq.count, &op_arity, err)) return false;
            if (!add_decl_method(ctx, op.symbol, op_arity, err)) return false;
            continue;
        }
        (void)param_start;
        (void)has_body;
        IdmArity arity = idm_arity_unknown();
        if (!method_signature_arity(ctx, items[i], &arity, err)) return false;
        if (!add_decl_method(ctx, name, arity, err)) return false;
    }
    return true;
}


IdmCore *expand_method_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    if (!ctx->trait_name) return expand_error(err, form->span, "method declarations are only valid inside trait bodies");
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return NULL;
    IdmArity arity = idm_arity_unknown();
    if (!method_signature_arity(ctx, form, &arity, err)) return NULL;
    TraitMethodDef *method = find_decl_method(ctx, name->as.text);
    if (!method) {
        if (!add_decl_method(ctx, name, arity, err)) return NULL;
        method = find_decl_method(ctx, name->as.text);
    }
    if (!method) return (IdmCore *)(uintptr_t)idm_error_oom(err, name->span);
    if (method->seen_decl) return expand_error(err, name->span, "trait '%s' declares method '%s' more than once", ctx->trait_name, name->as.text);
    if (!idm_arity_equal(&method->arity, &arity)) return expand_error(err, name->span, "method '%s.%s' arity changed during declaration", ctx->trait_name, name->as.text);
    method->seen_decl = true;
    if (has_body) {
        IdmCore *fn = expand_function_literal(ctx, name->as.text, form->as.seq.items[1], form->as.seq.items, param_start, form->as.seq.count, err);
        if (!fn) return NULL;
        method->default_fn = fn;
        method->has_default = true;
        method_contract_stamp_default(method, fn);
    }
    return decl_done(err);
}

static void trait_impls_destroy(TraitMethodImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) {
        idm_core_free(impls[i].fn);
        if (impls[i].has_contract) idm_callable_contract_destroy(&impls[i].contract);
    }
    free(impls);
}

static const TraitMethodDef *find_trait_method_def(ExpandContext *ctx, const TraitMethodDef *methods, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (trait_method_matches_name(ctx, &methods[i], name)) return &methods[i];
    }
    return NULL;
}

static bool trait_has_impl(ExpandContext *ctx, const TraitMethodImpl *impls, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (trait_method_impl_matches_name(ctx, &impls[i], name)) return true;
    }
    return false;
}

static bool trait_impls_push(ExpandContext *ctx, TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, IdmArity arity, IdmCore *fn, IdmCallableContract *contract, bool has_contract, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)impls, cap, sizeof(**impls), 4u, *count + 1u)) return idm_error_oom(err, span);
    }
    TraitMethodImpl *impl = &(*impls)[(*count)++];
    impl->arity = arity;
    impl->fn = fn;
    impl->has_contract = has_contract;
    memset(&impl->contract, 0, sizeof(impl->contract));
    if (has_contract) {
        impl->contract = *contract;
        memset(contract, 0, sizeof(*contract));
    }
    if (!trait_method_impl_set_name(ctx, impl, name, err, span)) {
        (*count)--;
        return false;
    }
    return true;
}

static IdmCore *method_storage_ref(ExpandContext *ctx, const char *name, bool env, const char *env_key, uint32_t slot, IdmSpan span) {
    if (env) {
        if (env_key && env_key[0]) return idm_core_package_ref(name, idm_atom(ctx->rt, env_key), slot, span);
        return idm_core_env_ref(name, slot, span);
    }
    return idm_core_local_ref(name, slot, span);
}

static bool method_capture_ref(IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, IdmCore **out) {
    *out = NULL;
    if (!idm_core_fn_multi_add_capture(multi, IDM_CAP_LOCAL, method_surface_name_text(surface), impl->impl_slot, 0)) return false;
    for (size_t i = 0; i < multi->as.fn_multi.capture_count; i++) {
        const IdmCapture *capture = &multi->as.fn_multi.captures[i];
        if (capture->kind == IDM_CAP_LOCAL && capture->index == impl->impl_slot) {
            *out = idm_core_capture_ref(method_surface_name_text(surface), (uint32_t)i, multi->span);
            return *out != NULL;
        }
    }
    return false;
}

static void method_contract_stamp_default(TraitMethodDef *method, const IdmCore *fn) {
    uint32_t primitive = 0;
    bool passthrough = fn_primitive_passthrough(fn, &primitive);
    if (!method->has_contract) {
        if (!passthrough) return;
        memset(&method->contract, 0, sizeof(method->contract));
        method->has_contract = true;
    }
    method->contract.passthrough = passthrough;
    method->contract.primitive = passthrough ? primitive : 0u;
}

static bool method_impl_record(ExpandContext *ctx, uint32_t method_surface, IdmSymbol *type, const IdmStructuralHead *structural, IdmArity arity, bool impl_env, const char *impl_env_key, uint32_t impl_slot, const IdmCallableContract *contract, IdmError *err, IdmSpan span) {
    const MethodSurfaceDef *surface = method_surface_by_index(ctx, method_surface);
    if (!surface) return idm_error_set(err, span, "method implementation references missing surface");
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (impl->trait != surface->trait || impl->name != surface->name || impl->structural != (structural != NULL)) continue;
        if (structural ? !idm_structural_head_equal(&impl->structural_head, structural) : impl->type != type) continue;
        impl->arity = arity;
        impl->impl_env = impl_env;
        impl->impl_frame = ctx->frame;
        impl->impl_slot = impl_slot;
        IdmCallableContract next_contract;
        memset(&next_contract, 0, sizeof(next_contract));
        if (contract && !idm_callable_contract_copy(&next_contract, contract)) return idm_error_oom(err, span);
        char *key = idm_strdup(impl_env_key ? impl_env_key : "");
        if (!key) {
            if (contract) idm_callable_contract_destroy(&next_contract);
            return idm_error_oom(err, span);
        }
        if (impl->has_contract) idm_callable_contract_destroy(&impl->contract);
        impl->contract = next_contract;
        impl->has_contract = contract != NULL;
        free(impl->impl_env_key);
        impl->impl_env_key = key;
        return true;
    }
    if (ctx->typed.method_impl_count == ctx->typed.method_impl_cap) {
        if (!idm_grow((void **)&ctx->typed.method_impls, &ctx->typed.method_impl_cap, sizeof(*ctx->typed.method_impls), 8u, ctx->typed.method_impl_count + 1u)) return idm_error_oom(err, span);
    }
    MethodImplDef *impl = &ctx->typed.method_impls[ctx->typed.method_impl_count];
    memset(impl, 0, sizeof(*impl));
    impl->method_surface = method_surface;
    impl->impl_env_key = idm_strdup(impl_env_key ? impl_env_key : "");
    impl->arity = arity;
    impl->impl_env = impl_env;
    impl->impl_frame = ctx->frame;
    impl->impl_slot = impl_slot;
    if (contract && idm_callable_contract_copy(&impl->contract, contract)) impl->has_contract = true;
    else if (contract) {
        free(impl->impl_env_key);
        memset(impl, 0, sizeof(*impl));
        return idm_error_oom(err, span);
    }
    if (!impl->impl_env_key || !method_impl_set_identity(impl, surface->trait, surface->name, type, structural, err, span)) {
        free(impl->impl_env_key);
        idm_structural_head_destroy(&impl->structural_head);
        if (impl->has_contract) idm_callable_contract_destroy(&impl->contract);
        memset(impl, 0, sizeof(*impl));
        return false;
    }
    ctx->typed.method_impl_count++;
    return true;
}

typedef struct {
    const MethodImplDef *impl;
    const MethodSurfaceDef *surface;
} ImplDispatchBody;

static IdmCore *impl_dispatch_clause_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const ImplDispatchBody *body = user;
    const MethodImplDef *impl = body->impl;
    const MethodSurfaceDef *surface = body->surface;
    IdmCore *callee = NULL;
    bool ok = true;
    if (impl->impl_env) callee = method_storage_ref(ctx, method_surface_name_text(surface), true, impl->impl_env_key, impl->impl_slot, span);
    else ok = method_capture_ref(multi, impl, surface, &callee);
    IdmCore *call = callee ? idm_core_call(callee, span) : NULL;
    if (!call) {
        idm_core_free(callee);
        ok = false;
    }
    for (uint32_t i = 0; ok && i < argc; i++) {
        IdmCore *arg = idm_core_arg_ref("arg", i, span);
        if (!arg || !idm_core_call_add_arg(call, arg)) {
            idm_core_free(arg);
            ok = false;
        }
    }
    if (!ok) {
        idm_core_free(call);
        if (!err->present) idm_error_oom(err, span);
        return NULL;
    }
    (void)ctx;
    return call;
}

static bool dispatch_add_clause_for_type(ExpandContext *ctx, IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, IdmSymbol *type, uint32_t argc, IdmSpan span, IdmError *err) {
    ImplDispatchBody body = {impl, surface};
    if (!type) return idm_error_set(err, span, "method implementation requires a receiver type");
    return expand_multi_add_dispatch_clause(ctx, multi, argc, type, impl_dispatch_clause_body, &body, span, err);
}

static bool dispatch_add_clause(ExpandContext *ctx, IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, uint32_t argc, IdmSpan span, IdmError *err) {
    if (impl->structural) {
        for (size_t i = 0; i < ctx->typed.entity_count; i++) {
            const TypedEntity *e = &ctx->typed.entities[i];
            if (e->kind != IDM_TYPED_ENTITY_TYPE || e->as.type.field_count == 0) continue;
            IdmSymbol *identity = e->as.type.identity;
            if (!identity || !type_satisfies_structural(ctx, &impl->structural_head, identity)) continue;
            if (!dispatch_add_clause_for_type(ctx, multi, impl, surface, identity, argc, span, err)) return false;
        }
        return true;
    }
    return dispatch_add_clause_for_type(ctx, multi, impl, surface, impl->type, argc, span, err);
}

static bool dispatch_add_impl_clauses(ExpandContext *ctx, IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, IdmError *err, IdmSpan span) {
    ImplDispatchBody body = {impl, surface};
    if (impl->has_contract && impl->contract.sig_count != 0) {
        for (size_t i = 0; i < impl->contract.sig_count; i++) {
            size_t arity = impl->contract.sigs[i].arg_count;
            if (arity > UINT32_MAX) return idm_error_set(err, span, "method '%s.%s' has too many arguments", method_surface_trait_text(surface), method_surface_name_text(surface));
            bool duplicate = false;
            for (size_t j = 0; j < i; j++) {
                if (impl->contract.sigs[j].arg_count == arity) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && !dispatch_add_clause(ctx, multi, body.impl, body.surface, (uint32_t)arity, span, err)) return false;
        }
        return true;
    }
    uint32_t arity = 0;
    if (!method_arity_single(&impl->arity, &arity)) return idm_error_set(err, span, "method '%s.%s' has no exact callable contract", method_surface_trait_text(surface), method_surface_name_text(surface));
    return dispatch_add_clause(ctx, multi, body.impl, body.surface, arity, span, err);
}

IdmCore *build_method_dispatcher_core(ExpandContext *ctx, const char *trait, const char *method_name, IdmSpan span, IdmError *err) {
    IdmCore *multi = idm_core_fn_multi(method_name, span);
    if (!multi) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        const MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (!method_impl_matches_identity(ctx, impl, trait, method_name, NULL)) continue;
        const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl->method_surface);
        if (!surface) {
            idm_core_free(multi);
            expand_error(err, span, "method implementation references missing surface");
            return NULL;
        }
        if (!dispatch_add_impl_clauses(ctx, multi, impl, surface, err, span)) {
            idm_core_free(multi);
            return NULL;
        }
    }
    if (multi->as.fn_multi.count == 0) {
        bool structural = false;
        for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
            const MethodImplDef *impl = &ctx->typed.method_impls[i];
            if (!method_impl_matches_identity(ctx, impl, trait, method_name, NULL)) continue;
            if (impl->structural) { structural = true; break; }
        }
        if (!structural) {
            idm_core_free(multi);
            return expand_error(err, span, "method '%s.%s' has no compiled dispatch clauses", trait, method_name);
        }
        IdmCore *raise_fn = expand_primitive_clause_call(IDM_PRIM_RAISE, span, err);
        IdmCore *raise_call = raise_fn ? idm_core_call(raise_fn, span) : NULL;
        IdmCore *reason = raise_call ? idm_core_literal(idm_atom(ctx->rt, "no-method-implementation"), span) : NULL;
        if (!reason || !idm_core_call_add_arg(raise_call, reason)) {
            idm_core_free(reason);
            idm_core_free(raise_call);
            idm_core_free(multi);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        IdmPattern *pat = idm_pat_wildcard(span);
        IdmPattern **pats = pat ? calloc(1u, sizeof(*pats)) : NULL;
        if (pats) pats[0] = pat;
        if (!pats || !idm_core_fn_multi_add_clause_take(multi, 1u, pats, 1u, NULL, 0u, NULL, raise_call)) {
            if (!pats) idm_pat_free(pat);
            free(pats);
            idm_core_free(raise_call);
            idm_core_free(multi);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    return multi;
}

typedef struct {
    const char *env_key;
    uint32_t slot;
} RefreshSlot;

static bool refresh_slot_seen(const RefreshSlot *slots, size_t count, const char *env_key, uint32_t slot) {
    for (size_t i = 0; i < count; i++) {
        if (slots[i].slot == slot && strcmp(slots[i].env_key, env_key) == 0) return true;
    }
    return false;
}

static bool refresh_slot_push(RefreshSlot **slots, size_t *count, size_t *cap, const char *env_key, uint32_t slot, IdmError *err, IdmSpan span) {
    if (refresh_slot_seen(*slots, *count, env_key, slot)) return true;
    if (*count == *cap) {
        if (!idm_grow((void **)slots, cap, sizeof(**slots), 16u, *count + 1u)) return idm_error_oom(err, span);
    }
    (*slots)[*count].env_key = env_key;
    (*slots)[*count].slot = slot;
    (*count)++;
    return true;
}

static bool method_dispatch_has_impl_clause(const ExpandContext *ctx, const char *trait, const char *method) {
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        if (method_impl_matches_identity(ctx, &ctx->typed.method_impls[i], trait, method, NULL)) return true;
    }
    return false;
}

static bool refresh_add_field_selectors(ExpandContext *ctx, IdmCore **core, IdmSpan span, IdmError *err) {
    for (size_t i = 0; i < ctx->field_selector_count; i++) {
        FieldSelectorDef *sel = &ctx->field_selectors[i];
        if (!sel->env) continue;
        bool any = false;
        for (size_t e = 0; !any && e < ctx->typed.entity_count; e++) {
            const TypedEntity *ent = &ctx->typed.entities[e];
            if (ent->kind != IDM_TYPED_ENTITY_TYPE) continue;
            for (size_t f = 0; f < ent->as.type.field_count; f++) {
                const char *fname = type_field_name_text(&ent->as.type.fields[f]);
                if (fname && strcmp(fname, sel->name) == 0) { any = true; break; }
            }
        }
        if (!any) continue;
        IdmCore *sel_core = build_field_selector_core(ctx, sel->name, span, err);
        if (!sel_core) return false;
        if (!*core) {
            *core = idm_core_letrec(span);
            if (!*core) { idm_core_free(sel_core); return idm_error_oom(err, span); }
            idm_core_letrec_set_env(*core);
        }
        const char *sel_key = sel->env_key ? sel->env_key : (ctx->unit_key[0] && ctx->in_package ? ctx->unit_key : NULL);
        bool added = sel_key
            ? idm_core_letrec_add_env_fill(*core, sel->name, idm_atom(ctx->rt, sel_key), sel->slot, sel_core, sel->emitted)
            : idm_core_letrec_add_fill(*core, sel->name, sel->slot, sel_core, sel->emitted);
        if (!added) { idm_core_free(sel_core); return idm_error_oom(err, span); }
        sel->emitted = true;
    }
    return true;
}

IdmCore *build_method_dispatch_refresh_core(ExpandContext *ctx, IdmSpan span, IdmError *err) {
    IdmCore *core = NULL;
    RefreshSlot *slots = NULL;
    size_t slot_count = 0;
    size_t slot_cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < ctx->typed.method_surface_count; i++) {
        const MethodSurfaceDef *surface = &ctx->typed.method_surfaces[i];
        if (!surface->has_dispatch || !surface->dispatch_env || !surface->dispatch_env_key || !surface->dispatch_env_key[0]) continue;
        const TraitDef *trait = typed_trait_by_symbol(ctx, surface->trait);
        const TraitMethodDef *method = trait ? find_trait_method_def(ctx, trait->methods, trait->method_count, method_surface_name_text(surface)) : NULL;
        if (!method) continue;
        if (!method_dispatch_has_impl_clause(ctx, method_surface_trait_text(surface), method_surface_name_text(surface))) continue;
        if (refresh_slot_seen(slots, slot_count, surface->dispatch_env_key, surface->dispatch_slot)) continue;
        if (!refresh_slot_push(&slots, &slot_count, &slot_cap, surface->dispatch_env_key, surface->dispatch_slot, err, span)) {
            ok = false;
            break;
        }
        if (!core) {
            core = idm_core_letrec(span);
            if (!core) {
                ok = idm_error_oom(err, span);
                break;
            }
            idm_core_letrec_set_env(core);
        }
        IdmCore *dispatcher = build_method_dispatcher_core(ctx, method_surface_trait_text(surface), method_surface_name_text(surface), span, err);
        if (!dispatcher || !idm_core_letrec_add_env_fill(core, method_surface_name_text(surface), idm_atom(ctx->rt, surface->dispatch_env_key), surface->dispatch_slot, dispatcher, false)) {
            idm_core_free(dispatcher);
            ok = err && err->present ? false : idm_error_oom(err, span);
            break;
        }
    }
    free(slots);
    if (ok) ok = refresh_add_field_selectors(ctx, &core, span, err);
    if (!ok) {
        idm_core_free(core);
        return NULL;
    }
    if (!core) return NULL;
    IdmCore *body = idm_core_literal(idm_nil(), span);
    if (!body || !idm_core_letrec_set_body(core, body)) {
        idm_core_free(body);
        idm_core_free(core);
        idm_error_oom(err, span);
        return NULL;
    }
    return core;
}

typedef struct {
    const TraitMethodDef *method;
    uint32_t method_surface;
    uint32_t slot;
    bool env;
    bool fill_existing;
    char *env_key;
} ImplementDispatchBinding;

static void implement_dispatch_bindings_destroy(ImplementDispatchBinding *bindings, size_t count) {
    if (!bindings) return;
    for (size_t i = 0; i < count; i++) free(bindings[i].env_key);
    free(bindings);
}

static TraitMethodImpl *trait_impl_find(ExpandContext *ctx, TraitMethodImpl *impls, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (trait_method_impl_matches_name(ctx, &impls[i], name)) return &impls[i];
    }
    return NULL;
}

static const MethodSurfaceDef *current_dispatch_surface_index(ExpandContext *ctx, IdmSymbol *trait, const char *method, const IdmScopeSet *scopes, uint32_t *out_index, IdmError *err, IdmSpan span) {
    const IdmBinding *found = NULL;
    if (out_index) *out_index = UINT32_MAX;
    IdmResolveStatus status = resolve_scoped(ctx, method, IDM_BIND_SPACE_METHOD, scopes, NULL, &found);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, span, "ambiguous method '%s'", method);
        return NULL;
    }
    if (status != IDM_RESOLVE_OK || !found) return NULL;
    const IdmScopeSet *best = &found->scopes;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *binding = &ctx->bindings.items[i];
        if (binding->phase != IDM_PHASE_ANY && binding->phase != ctx->phase) continue;
        if (binding->space != IDM_BIND_SPACE_METHOD || binding->kind != IDM_BIND_METHOD) continue;
        if (strcmp(binding->name, method) != 0 || !idm_scope_set_equal(&binding->scopes, best) || binding->payload >= ctx->typed.method_surface_count) continue;
        const MethodSurfaceDef *surface = &ctx->typed.method_surfaces[binding->payload];
        if (surface->has_dispatch && method_surface_matches_identity(ctx, surface, trait, method)) {
            if (out_index) *out_index = binding->payload;
            return surface;
        }
    }
    return NULL;
}

static bool implement_dispatch_bindings_prepare(ExpandContext *ctx, const TraitDef *trait, const IdmScopeSet *method_scopes, ImplementDispatchBinding **out, IdmError *err, IdmSpan span) {
    *out = NULL;
    if (trait->method_count == 0) return true;
    ImplementDispatchBinding *bindings = calloc(trait->method_count, sizeof(*bindings));
    if (!bindings) return idm_error_oom(err, span);
    for (size_t i = 0; i < trait->method_count; i++) {
        const TraitMethodDef *method = &trait->methods[i];
        const char *method_name = trait_method_name_text(method);
        bindings[i].method = method;
        uint32_t method_surface = UINT32_MAX;
        const MethodSurfaceDef *surface = current_dispatch_surface_index(ctx, trait->name, method_name, method_scopes, &method_surface, err, span);
        if (err && err->present) {
            implement_dispatch_bindings_destroy(bindings, trait->method_count);
            return false;
        }
        bindings[i].method_surface = method_surface;
        if (surface) {
            bindings[i].slot = surface->dispatch_slot;
            bindings[i].env = surface->dispatch_env;
            bindings[i].fill_existing = !surface->dispatch_env;
            bindings[i].env_key = idm_strdup(surface->dispatch_env_key ? surface->dispatch_env_key : "");
        } else {
            bool dispatch_env = trait_dispatch_env_storage(ctx);
            bindings[i].slot = dispatch_env ? ctx->env_slot_seq++ : ctx->next_slot++;
            bindings[i].env = dispatch_env;
            bindings[i].fill_existing = false;
            bindings[i].env_key = idm_strdup("");
        }
        if (!bindings[i].env_key) {
            implement_dispatch_bindings_destroy(bindings, trait->method_count);
            return idm_error_oom(err, span);
        }
    }
    *out = bindings;
    return true;
}

static IdmCore *build_trait_implement_core(ExpandContext *ctx, TraitDef *trait_def, const char *trait_key, IdmSymbol *type, const IdmStructuralHead *structural, const char *type_display, const IdmScopeSet *method_scopes, const IdmSyntax *body, IdmSpan span, IdmError *err) {
    const char *trait = trait_def_identity_text(trait_def);
    TraitMethodDef *methods = trait_def->methods;
    size_t method_count = trait_def->method_count;
    if (!check_trait_requirements_for_type(ctx, trait_def, structural, type_display, span, err)) return NULL;
    ImplementDispatchBinding *dispatch = NULL;
    if (!implement_dispatch_bindings_prepare(ctx, trait_def, method_scopes, &dispatch, err, span)) return NULL;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool surface_ok = true;
    bool saved_suppress_refresh = ctx->suppress_method_impl_surface_refresh;
    ctx->suppress_method_impl_surface_refresh = true;
    for (size_t i = 0; i < method_count && surface_ok; i++) surface_ok = install_method_surface(ctx, trait_def->name, trait_key, trait_method_name_text(&methods[i]), methods[i].arity, method_scopes, ctx->unit, ctx->unit_key, true, dispatch[i].env, dispatch[i].env_key, dispatch[i].slot, err);
    ctx->suppress_method_impl_surface_refresh = saved_suppress_refresh;
    if (!surface_ok) {
        surface_rollback(ctx, &checkpoint);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return NULL;
    }

    MethodSignature *method_signatures = NULL;
    size_t method_signature_count = 0;
    if (!method_signatures_collect(ctx, body, &method_signatures, &method_signature_count, err)) {
        surface_rollback(ctx, &checkpoint);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return NULL;
    }
    TraitMethodImpl *impls = NULL;
    size_t impl_count = 0;
    size_t impl_cap = 0;
    bool ok = true;
    size_t body_count = body ? body->as.seq.count : 1u;
    for (size_t i = 1; i < body_count && ok; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (method_spec_form_parts(stmt, NULL, NULL)) continue;
        const IdmSyntax *name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(stmt, &name, &param_start, &has_body, err)) {
            if (!(err && err->present)) expand_error(err, stmt->span, "implement bodies may contain only method implementations");
            ok = false;
            break;
        }
        if (!has_body) { expand_error(err, stmt->span, "implement method '%s' requires an implementation body", name->as.text); ok = false; break; }
        IdmArity arity = idm_arity_unknown();
        if (!method_signature_arity(ctx, stmt, &arity, err)) { ok = false; break; }
        const TraitMethodDef *contract = find_trait_method_def(ctx, methods, method_count, name->as.text);
        if (!contract) { expand_error(err, name->span, "trait '%s' has no method '%s'", trait, name->as.text); ok = false; break; }
        if (!idm_arity_equal(&contract->arity, &arity)) { method_arity_mismatch(err, name->span, trait, name->as.text, &contract->arity, &arity); ok = false; break; }
        if (trait_has_impl(ctx, impls, impl_count, name->as.text)) { expand_error(err, name->span, "implement for '%s' provides method '%s' more than once", trait, name->as.text); ok = false; break; }
        IdmCallableContract provided_contract;
        bool provided_has_contract = false;
        if (!method_signature_take(name, method_signatures, method_signature_count, &provided_contract, &provided_has_contract, err)) { ok = false; break; }
        if (provided_has_contract && !method_contract_apply_arity(name, &provided_contract, &arity, err)) {
            idm_callable_contract_destroy(&provided_contract);
            ok = false;
            break;
        }
        IdmCallableContract trait_contract;
        bool trait_has_contract = false;
        if (!method_contract_for_type(ctx, contract, type_display, &trait_contract, &trait_has_contract, err, name->span)) {
            if (provided_has_contract) idm_callable_contract_destroy(&provided_contract);
            ok = false;
            break;
        }
        IdmCallableContract receiver_contract;
        bool receiver_has_contract = false;
        if (!trait_has_contract && !provided_has_contract) {
            if (!receiver_only_contract(ctx, type_display, structural != NULL, &arity, &receiver_contract, &receiver_has_contract, err, name->span)) {
                ok = false;
                break;
            }
        }
        const IdmCallableContract *active_contract = trait_has_contract ? &trait_contract : (provided_has_contract ? &provided_contract : (receiver_has_contract ? &receiver_contract : NULL));
        IdmCore *fn = expand_function_literal_with_contract(ctx, name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, active_contract, err);
        if (receiver_has_contract) idm_callable_contract_destroy(&receiver_contract);
        if (!fn) {
            if (provided_has_contract) idm_callable_contract_destroy(&provided_contract);
            if (trait_has_contract) idm_callable_contract_destroy(&trait_contract);
            ok = false;
            break;
        }
        if (provided_has_contract) {
            bool has_contract = true;
            if (!expand_typecheck_value(ctx, name->as.text, &fn, &provided_contract, &has_contract, true, err)) ok = false;
        }
        if (ok && trait_has_contract) {
            bool has_contract = true;
            if (!expand_typecheck_value(ctx, name->as.text, &fn, &trait_contract, &has_contract, true, err)) ok = false;
        }
        IdmCallableContract inferred;
        memset(&inferred, 0, sizeof(inferred));
        bool inferred_has = false;
        if (ok && !provided_has_contract && !trait_has_contract) {
            if (!expand_typecheck_value(ctx, name->as.text, &fn, &inferred, &inferred_has, false, err)) ok = false;
        }
        IdmCallableContract *impl_contract = provided_has_contract ? &provided_contract
                                           : trait_has_contract ? &trait_contract
                                           : inferred_has ? &inferred : NULL;
        if (!ok) {
            if (provided_has_contract) idm_callable_contract_destroy(&provided_contract);
            if (trait_has_contract) idm_callable_contract_destroy(&trait_contract);
            if (inferred_has) idm_callable_contract_destroy(&inferred);
            idm_core_free(fn);
            break;
        }
        bool pushed = trait_impls_push(ctx, &impls, &impl_count, &impl_cap, name->as.text, arity, fn, impl_contract, impl_contract != NULL, err, name->span);
        if (provided_has_contract) idm_callable_contract_destroy(&provided_contract);
        if (trait_has_contract) idm_callable_contract_destroy(&trait_contract);
        if (inferred_has) idm_callable_contract_destroy(&inferred);
        if (!pushed) {
            idm_core_free(fn);
            ok = false;
            break;
        }
    }
    if (ok) ok = method_signatures_all_used(method_signatures, method_signature_count, err);
    surface_rollback(ctx, &checkpoint);
    method_signatures_destroy(method_signatures, method_signature_count);
    if (!ok) {
        trait_impls_destroy(impls, impl_count);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return NULL;
    }
    for (size_t i = 0; i < method_count; i++) {
        const char *method_name = trait_method_name_text(&methods[i]);
        if (!methods[i].has_default && !trait_has_impl(ctx, impls, impl_count, method_name)) {
            expand_error(err, body ? body->span : span, "implement for '%s' on type '%s' is missing required method '%s'", trait, type_display, method_name);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            return NULL;
        }
    }
    bool letrec_env = method_count != 0 && dispatch[0].env;
    for (size_t i = 1; i < method_count; i++) {
        if (dispatch[i].env != letrec_env) {
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            expand_error(err, span, "trait '%s' has mixed method dispatcher storage", trait);
            return NULL;
        }
    }
    IdmCore *core = idm_core_letrec(span);
    if (!core) {
        trait_impls_destroy(impls, impl_count);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (letrec_env) idm_core_letrec_set_env(core);
    for (size_t i = 0; i < method_count; i++) {
        const char *method_name = trait_method_name_text(&methods[i]);
        TraitMethodImpl *provided = trait_impl_find(ctx, impls, impl_count, method_name);
        uint32_t method_surface = dispatch[i].method_surface;
        if (method_surface == UINT32_MAX && !method_surface_index_by_identity(ctx, trait_def->name, method_name, &method_surface)) {
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            expand_error(err, span, "method implementation references missing surface");
            return NULL;
        }
        if (provided) {
            bool impl_pkg = letrec_env && ctx->unit_key[0];
            uint32_t impl_slot = letrec_env ? ctx->env_slot_seq++ : ctx->next_slot++;
            IdmCore *fn = provided->fn;
            provided->fn = NULL;
            bool impl_added = impl_pkg
                ? idm_core_letrec_add_env_fill(core, trait_method_impl_name_text(provided), idm_atom(ctx->rt, ctx->unit_key), impl_slot, fn, false)
                : idm_core_letrec_add(core, trait_method_impl_name_text(provided), impl_slot, fn);
            if (!impl_added) {
                idm_core_free(fn);
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
            if (!method_impl_record(ctx, method_surface, type, structural, methods[i].arity, letrec_env, impl_pkg ? ctx->unit_key : NULL, impl_slot, provided->has_contract ? &provided->contract : NULL, err, span)) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return NULL;
            }
            if (impl_pkg && !record_package_slot(ctx, trait_method_impl_name_text(provided), impl_slot, method_scopes, methods[i].arity, provided->has_contract ? &provided->contract : NULL, true)) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
        } else if (methods[i].has_default) {
            if (!methods[i].has_default_slot) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                expand_error(err, span, "trait '%s.%s' has a default method without storage", trait, method_name);
                return NULL;
            }
            if (!method_impl_record(ctx, method_surface, type, structural, methods[i].arity, trait_def->dispatch_env, trait_def->dispatch_env_key, methods[i].default_slot, methods[i].has_contract ? &methods[i].contract : NULL, err, span)) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return NULL;
            }
        }
    }
    for (size_t i = 0; i < method_count; i++) {
        IdmCore *dispatcher = build_method_dispatcher_core(ctx, trait, trait_method_name_text(&methods[i]), span, err);
        bool added = dispatcher && (dispatch[i].env && dispatch[i].env_key[0]
            ? idm_core_letrec_add_env_fill(core, trait_method_name_text(&methods[i]), idm_atom(ctx->rt, dispatch[i].env_key), dispatch[i].slot, dispatcher, dispatch[i].fill_existing)
            : idm_core_letrec_add_fill(core, trait_method_name_text(&methods[i]), dispatch[i].slot, dispatcher, dispatch[i].fill_existing));
        if (!added) {
            idm_core_free(dispatcher);
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        methods[i].has_dispatch = true;
        methods[i].dispatch_slot = dispatch[i].slot;
        if (ctx->in_package && dispatch[i].env && dispatch[i].env_key[0] && strcmp(dispatch[i].env_key, ctx->unit_key) == 0 &&
            !record_package_slot(ctx, trait_method_name_text(&methods[i]), dispatch[i].slot, method_scopes, methods[i].arity, NULL, true)) {
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    IdmCore *nil_body = idm_core_literal(idm_nil(), span);
    if (!nil_body || !idm_core_letrec_set_body(core, nil_body)) {
        idm_core_free(nil_body);
        idm_core_free(core);
        trait_impls_destroy(impls, impl_count);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!install_method_surface(ctx, trait_def->name, trait_key, trait_method_name_text(&methods[i]), methods[i].arity, method_scopes, ctx->unit, ctx->unit_key, true, dispatch[i].env, dispatch[i].env_key, dispatch[i].slot, err)) {
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            return NULL;
        }
    }
    trait_impls_destroy(impls, impl_count);
    implement_dispatch_bindings_destroy(dispatch, method_count);
    return core;
}

static ProtocolDef *protocol_def_by_identity(ExpandContext *ctx, const char *identity);

static bool protocol_activate_stmt(const IdmSyntax *item, const IdmSyntax **out_name) {
    if (!idm_syn_is_form_id(item, IDM_FORM_EXPR) || item->as.seq.count != 3u) return false;
    if (!syn_is_word(item->as.seq.items[1], "activate")) return false;
    if (item->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    *out_name = item->as.seq.items[2];
    return true;
}

static bool protocol_info_stmt(const IdmSyntax *item) {
    return idm_syn_is_form_id(item, IDM_FORM_EXPR) && item->as.seq.count >= 4u &&
           syn_is_word(item->as.seq.items[1], "info") && item->as.seq.items[2]->kind == IDM_SYN_ATOM;
}

static IdmSyntax *protocol_info_bind(const IdmSyntax *item, IdmError *err) {
    const IdmSyntax *key = item->as.seq.items[2];
    IdmSpan span = item->span;
    IdmBuffer name;
    idm_buf_init(&name);
    if (!idm_buf_append(&name, "info:") || !idm_buf_append(&name, key->as.text)) {
        idm_buf_destroy(&name);
        idm_error_oom(err, span);
        return NULL;
    }
    IdmSyntax *bind = idm_syn_list(span);
    IdmSyntax *head = idm_syn_word("%-expr", span);
    IdmSyntax *binder = idm_syn_word(name.data, span);
    IdmSyntax *eq = idm_syn_word("=", span);
    idm_buf_destroy(&name);
    bool ok = bind && head && binder && eq;
    for (int phase = 0; ok && phase < 2; phase++) {
        const IdmScopeSet *scopes = idm_syn_scope_set(item->as.seq.items[1], phase);
        if (scopes) for (size_t si = 0; si < scopes->count && ok; si++) ok = idm_syn_scope_add(binder, phase, scopes->items[si]);
    }
    ok = ok && idm_syn_append(bind, head) && idm_syn_append(bind, binder) && idm_syn_append(bind, eq);
    if (!ok) {
        idm_syn_free(bind);
        if (!bind) { idm_syn_free(head); idm_syn_free(binder); idm_syn_free(eq); }
        idm_error_oom(err, span);
        return NULL;
    }
    for (size_t i = 3; i < item->as.seq.count; i++) {
        IdmSyntax *part = idm_syn_clone(item->as.seq.items[i]);
        if (!part || !idm_syn_append(bind, part)) {
            idm_syn_free(part);
            idm_syn_free(bind);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    return bind;
}

static IdmCore *protocol_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmError *err) {
    const char *name = name_syntax->as.text;
    unsigned char hash[32];
    IdmSymbol *identity_symbol = scoped_identity(ctx, name, form, hash, err);
    if (!identity_symbol) return NULL;
    const char *identity = idm_symbol_text(identity_symbol);
    ProtocolPreActivation *preacts = NULL;
    size_t preact_count = 0;
    size_t preact_cap = 0;
    IdmSyntax *stripped = NULL;
    const IdmSyntax *compile_body = body;
    for (size_t i = 0; i < ctx->activation_count; i++) {
        const char *provider = ctx->activations[i].provider;
        if (!provider) continue;
        ProtocolDef *ambient = protocol_def_by_identity(ctx, provider);
        if (!ambient || !ambient->art) continue;
        if (preact_count == preact_cap && !idm_grow((void **)&preacts, &preact_cap, sizeof(*preacts), 4u, preact_count + 1u)) {
            free(preacts);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
        preacts[preact_count].identity = ambient->identity;
        preacts[preact_count].art = ambient->art;
        preact_count++;
    }
    size_t ambient_preacts = preact_count;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        const IdmSyntax *req_name = NULL;
        if (!protocol_activate_stmt(body->as.seq.items[i], &req_name)) continue;
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        ProtocolDef *req = resolve_protocol_def(ctx, req_name, &status);
        if (status == IDM_RESOLVE_AMBIGUOUS) {
            free(preacts);
            return expand_error(err, req_name->span, "ambiguous protocol '%s'", req_name->as.text);
        }
        if (!req) {
            free(preacts);
            return expand_error(err, req_name->span, "protocol '%s' activates '%s', which is not a visible protocol", name, req_name->as.text);
        }
        if (preact_count == preact_cap && !idm_grow((void **)&preacts, &preact_cap, sizeof(*preacts), 4u, preact_count + 1u)) {
            free(preacts);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, req_name->span);
        }
        preacts[preact_count].identity = req->identity;
        preacts[preact_count].art = req->art;
        preact_count++;
    }
    bool rewrite = preact_count != 0;
    for (size_t i = 1; !rewrite && i < body->as.seq.count; i++) rewrite = protocol_info_stmt(body->as.seq.items[i]);
    if (rewrite) {
        stripped = idm_syn_list(body->span);
        IdmSyntax *head = idm_syn_word("%-body", body->span);
        bool ok = stripped && head && idm_syn_append(stripped, head);
        if (!ok && !stripped) idm_syn_free(head);
        for (size_t i = 1; ok && i < body->as.seq.count; i++) {
            const IdmSyntax *req_name = NULL;
            if (protocol_activate_stmt(body->as.seq.items[i], &req_name)) continue;
            IdmSyntax *item = NULL;
            if (protocol_info_stmt(body->as.seq.items[i])) {
                item = protocol_info_bind(body->as.seq.items[i], err);
                if (!item) {
                    idm_syn_free(stripped);
                    free(preacts);
                    return NULL;
                }
            } else {
                item = idm_syn_clone(body->as.seq.items[i]);
            }
            ok = item != NULL && idm_syn_append(stripped, item);
            if (!ok) idm_syn_free(item);
        }
        if (!ok) {
            idm_syn_free(stripped);
            free(preacts);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
        }
        compile_body = stripped;
    }
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    IdmScopeId scope_base = idm_scope_store_next(&ctx->scope_store);
    bool compiled = compile_package_module(ctx, compile_body, identity, hash, ctx->package_slots, ctx->package_slot_count, preacts, preact_count, &art, err);
    idm_syn_free(stripped);
    if (compiled && preact_count > ambient_preacts) {
        size_t require_count = preact_count - ambient_preacts;
        art.protocol_requires = calloc(require_count, sizeof(*art.protocol_requires));
        if (!art.protocol_requires) compiled = false;
        for (size_t i = 0; compiled && i < require_count; i++) {
            art.protocol_requires[i] = preacts[ambient_preacts + i].identity;
            art.protocol_require_count = i + 1u;
        }
        if (!compiled) idm_error_oom(err, body->span);
    }
    free(preacts);
    if (!compiled) {
        idm_artifact_destroy(&art);
        return NULL;
    }
    if (!artifact_record_nested_deps(ctx, &art, err)) {
        idm_artifact_destroy(&art);
        return NULL;
    }
    art.scope_base = scope_base;
    art.scope_end = idm_scope_store_next(&ctx->scope_store);
    memcpy(art.src_hash, hash, 32u);
    SurfaceCheckpoint install_checkpoint;
    surface_checkpoint(ctx, &install_checkpoint);
    uint32_t payload = 0;
    ProtocolDef *p = typed_registry_add_protocol(ctx, &payload, err, form->span);
    if (!p) {
        idm_artifact_destroy(&art);
        return NULL;
    }
    p->name = idm_strdup(name);
    if (!protocol_def_set_identity(p, identity_symbol, err, form->span)) {
        surface_rollback(ctx, &install_checkpoint);
        idm_artifact_destroy(&art);
        return NULL;
    }
    p->exported = exported && ctx->in_package;
    IdmArtifact *moved = malloc(sizeof(*moved));
    if (!moved) {
        surface_rollback(ctx, &install_checkpoint);
        idm_artifact_destroy(&art);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    *moved = art;
    p->art = expand_cache_intern_artifact(ctx->rt, moved);
    if (!p->art) {
        idm_artifact_destroy(moved);
        free(moved);
        surface_rollback(ctx, &install_checkpoint);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (!p->name) {
        surface_rollback(ctx, &install_checkpoint);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    IdmScopeSet protocol_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &protocol_scopes)) {
        surface_rollback(ctx, &install_checkpoint);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&protocol_scopes);
        surface_rollback(ctx, &install_checkpoint);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    idm_scope_set_destroy(&protocol_scopes);
    return decl_done(err);
}

IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    bool exported = false;
    size_t base = 1u;
    if (form->as.seq.count >= 2 && syn_is_word(fitems[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(fitems[base], "protocol") || fitems[base + 1u]->kind != IDM_SYN_WORD || !idm_syn_is_form_id(fitems[base + 2u], IDM_FORM_BODY))
        return expand_error(err, form->span, "protocol expects 'protocol NAME do ... end'");
    return protocol_decl_core(ctx, form, fitems[base + 1u], fitems[base + 2u], exported, err);
}

IdmCore *expand_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    bool exported = false;
    size_t base = 1u;
    if (form->as.seq.count >= 2 && syn_is_word(fitems[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(fitems[base], "trait") || fitems[base + 1u]->kind != IDM_SYN_WORD || !idm_syn_is_form_id(fitems[base + 2u], IDM_FORM_BODY))
        return expand_error(err, form->span, "trait expects 'trait NAME do ... end'");
    return trait_decl_core(ctx, form, fitems[base + 1u], fitems[base + 2u], exported, err);
}

static IdmCore *trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmError *err) {
    unsigned char hash[32];
    IdmSymbol *identity_symbol = scoped_identity(ctx, name_syntax->as.text, form, hash, err);
    if (!identity_symbol) return NULL;
    const char *identity = idm_symbol_text(identity_symbol);

    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool saved_suppress_refresh = ctx->suppress_method_impl_surface_refresh;
    ctx->suppress_method_impl_surface_refresh = true;
    const char *saved_trait_name = ctx->trait_name;
    const char *saved_trait_key = ctx->trait_key;
    IdmSymbol *saved_trait_identity = ctx->trait_identity;
    ctx->trait_name = identity;
    ctx->trait_key = name_syntax->as.text;
    ctx->trait_identity = identity_symbol;
    IdmTraitRequirementDef *requirements = NULL;
    size_t requirement_count = 0;
    size_t requirement_cap = 0;
    TraitOperatorForm *op_decls = NULL;
    size_t op_decl_count = 0;
    size_t op_decl_cap = 0;
    MethodSignature *method_signatures = NULL;
    size_t method_signature_count = 0;
    bool ok = true;
    if (!method_signatures_collect(ctx, body, &method_signatures, &method_signature_count, err)) ok = false;
    for (size_t i = 1; ok && i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        IdmSyntax *required_name = require_form_trait_word(stmt, err);
        if (!required_name) {
            if (err && err->present && idm_syn_is_form_id(stmt, IDM_FORM_EXPR) && stmt->as.seq.count >= 2u && syn_is_word(stmt->as.seq.items[1], "require")) ok = false;
            continue;
        }
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        TraitDef *required = resolve_trait_def(ctx, required_name, &status);
        if (status == IDM_RESOLVE_AMBIGUOUS) {
            expand_error(err, required_name->span, "ambiguous required trait '%s'", required_name->as.text);
            ok = false;
        } else if (!required) {
            expand_error(err, required_name->span, "unbound required trait '%s'", required_name->as.text);
            ok = false;
        } else if (trait_requirement_seen(requirements, requirement_count, required->name)) {
            expand_error(err, required_name->span, "trait '%s' requires trait '%s' more than once", identity, trait_def_identity_text(required));
            ok = false;
        } else {
            ok = trait_requirements_push(&requirements, &requirement_count, &requirement_cap, required->name, err, required_name->span) &&
                 install_required_trait_methods(ctx, required, required_name, err);
        }
        idm_syn_free(required_name);
    }
    if (ok) ok = predeclare_trait_methods(ctx, body->as.seq.items, 1u, body->as.seq.count, err);
    for (size_t i = 1; ok && i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (idm_syn_is_form_id(stmt, IDM_FORM_EXPR) && stmt->as.seq.count >= 2u && syn_is_word(stmt->as.seq.items[1], "require")) continue;
        if (method_spec_form_parts(stmt, NULL, NULL)) continue;
        const IdmSyntax *method_name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        TraitOperatorForm opf;
        bool is_operator = false;
        if (!trait_operator_form_parts(stmt, &opf, &is_operator, err)) { ok = false; break; }
        if (is_operator) {
            if (op_decl_count == op_decl_cap && !idm_grow((void **)&op_decls, &op_decl_cap, sizeof(*op_decls), 4u, op_decl_count + 1u)) {
                ok = false;
                idm_error_oom(err, stmt->span);
                break;
            }
            op_decls[op_decl_count++] = opf;
            if (!opf.has_params) continue;
            method_name = opf.symbol;
            param_start = opf.param_start;
            has_body = true;
        } else if (!method_form_parts(stmt, &method_name, &param_start, &has_body, err)) {
            if (!(err && err->present)) expand_error(err, stmt->span, "trait bodies may contain only method declarations");
            ok = false;
            break;
        }
        IdmArity arity = idm_arity_unknown();
        bool arity_ok = is_operator
            ? function_literal_arity(ctx, method_name, stmt->as.seq.items, param_start, stmt->as.seq.count, &arity, err)
            : method_signature_arity(ctx, stmt, &arity, err);
        if (!arity_ok) { ok = false; break; }
        TraitMethodDef *method = find_decl_method(ctx, method_name->as.text);
        if (!method) { ok = false; idm_error_oom(err, method_name->span); break; }
        if (method->seen_decl) { expand_error(err, method_name->span, "trait '%s' declares method '%s' more than once", identity, method_name->as.text); ok = false; break; }
        if (!idm_arity_equal(&method->arity, &arity)) { expand_error(err, method_name->span, "method '%s.%s' arity changed during declaration", identity, method_name->as.text); ok = false; break; }
        IdmCallableContract method_contract;
        bool method_has_contract = false;
        if (!method_signature_take(method_name, method_signatures, method_signature_count, &method_contract, &method_has_contract, err)) { ok = false; break; }
        if (method_has_contract && !method_contract_apply_arity(method_name, &method_contract, &arity, err)) {
            idm_callable_contract_destroy(&method_contract);
            ok = false;
            break;
        }
        if (!method_has_contract && !method_implicit_trait_contract(ctx, identity_symbol, &arity, &method_contract, &method_has_contract, err, method_name->span)) { ok = false; break; }
        if (method_has_contract && !method_contract_ensure_trait_context(ctx, &method_contract, identity_symbol, err, method_name->span)) {
            idm_callable_contract_destroy(&method_contract);
            ok = false;
            break;
        }
        if (method_has_contract && !method_contract_ensure_requirement_contexts(ctx, &method_contract, requirements, requirement_count, err, method_name->span)) {
            idm_callable_contract_destroy(&method_contract);
            ok = false;
            break;
        }
        if (method_has_contract) {
            method->contract = method_contract;
            method->has_contract = true;
        }
        method->seen_decl = true;
        if (has_body) {
            IdmCore *fn = expand_function_literal_with_contract(ctx, method_name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, method->has_contract ? &method->contract : NULL, err);
            if (!fn) { ok = false; break; }
            if (method->has_contract) {
                bool has_contract = true;
                if (!expand_typecheck_value(ctx, method_name->as.text, &fn, &method->contract, &has_contract, true, err)) {
                    idm_core_free(fn);
                    ok = false;
                    break;
                }
            } else {
                IdmCallableContract inferred;
                bool inferred_has = false;
                memset(&inferred, 0, sizeof(inferred));
                if (!expand_typecheck_value(ctx, method_name->as.text, &fn, &inferred, &inferred_has, false, err)) {
                    idm_core_free(fn);
                    ok = false;
                    break;
                }
                if (inferred_has) {
                    method->contract = inferred;
                    method->has_contract = true;
                    if (!method_contract_ensure_trait_context(ctx, &method->contract, identity_symbol, err, method_name->span)) {
                        idm_core_free(fn);
                        ok = false;
                        break;
                    }
                }
            }
            method->default_fn = fn;
            method->has_default = true;
            method_contract_stamp_default(method, fn);
            method->has_default_slot = true;
            bool default_env = trait_dispatch_env_storage(ctx);
            method->default_slot = default_env ? ctx->env_slot_seq++ : ctx->next_slot++;
        }
    }
    if (ok) ok = method_signatures_all_used(method_signatures, method_signature_count, err);

    size_t method_count = ok ? ctx->decl_method_count - checkpoint.decl_method_count : 0u;
    TraitMethodDef *methods = NULL;
    if (ok && !trait_method_defs_copy(&ctx->decl_methods[checkpoint.decl_method_count], method_count, &methods)) {
        ok = false;
        idm_error_oom(err, form->span);
    }
    method_signatures_destroy(method_signatures, method_signature_count);
    method_signatures = NULL;
    method_signature_count = 0;

    IdmCore *init = NULL;
    if (ok) {
        init = idm_core_letrec(form->span);
        if (!init) {
            ok = false;
            idm_error_oom(err, form->span);
        } else if (trait_dispatch_env_storage(ctx)) {
            idm_core_letrec_set_env(init);
        }
    }
    for (size_t i = 0; ok && i < method_count; i++) {
        TraitMethodDef *method = &ctx->decl_methods[checkpoint.decl_method_count + i];
        IdmCore *default_fn = method->default_fn;
        method->default_fn = NULL;
        if (default_fn) {
            bool def_pkg = trait_dispatch_env_storage(ctx) && ctx->unit_key[0];
            bool def_added = def_pkg
                ? idm_core_letrec_add_env_fill(init, trait_method_name_text(method), idm_atom(ctx->rt, ctx->unit_key), method->default_slot, default_fn, false)
                : idm_core_letrec_add(init, trait_method_name_text(method), method->default_slot, default_fn);
            if (!def_added) {
                idm_core_free(default_fn);
                ok = false;
                idm_error_oom(err, form->span);
                break;
            }
        }
    }
    for (size_t i = 0; ok && !trait_dispatch_env_storage(ctx) && i < method_count; i++) {
        TraitMethodDef *method = &ctx->decl_methods[checkpoint.decl_method_count + i];
        IdmCore *placeholder = idm_core_literal(idm_nil(), form->span);
        if (!placeholder || !idm_core_letrec_add(init, trait_method_name_text(method), method->dispatch_slot, placeholder)) {
            idm_core_free(placeholder);
            ok = false;
            idm_error_oom(err, form->span);
        }
    }
    if (ok) {
        IdmCore *nil_body = idm_core_literal(idm_nil(), form->span);
        if (!nil_body || !idm_core_letrec_set_body(init, nil_body)) {
            idm_core_free(nil_body);
            ok = false;
            idm_error_oom(err, form->span);
        }
    }

    ctx->trait_name = saved_trait_name;
    ctx->trait_key = saved_trait_key;
    ctx->trait_identity = saved_trait_identity;
    ctx->suppress_method_impl_surface_refresh = saved_suppress_refresh;
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        free(op_decls);
        method_signatures_destroy(method_signatures, method_signature_count);
        idm_core_free(init);
        if (methods) { for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]); free(methods); }
        trait_requirements_destroy(requirements, requirement_count);
        return NULL;
    }

    SurfaceCheckpoint install_checkpoint;
    surface_checkpoint(ctx, &install_checkpoint);
    uint32_t payload = 0;
    TraitDef *trait = typed_registry_add_trait(ctx, &payload, err, form->span);
    if (!trait) {
        free(op_decls);
        idm_core_free(init);
        for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]);
        free(methods);
        trait_requirements_destroy(requirements, requirement_count);
        return NULL;
    }
    if (!trait_def_set_identity(trait, identity_symbol, err, form->span)) {
        free(op_decls);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]);
        free(methods);
        trait_requirements_destroy(requirements, requirement_count);
        return NULL;
    }
    trait->exported = exported && ctx->in_package;
    (void)hash;
    trait->requirements = requirements;
    trait->requirement_count = requirement_count;
    trait->methods = methods;
    trait->method_count = method_count;
    trait->dispatch_env = trait_dispatch_env_storage(ctx);
    IdmScopeSet trait_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &trait_scopes)) {
        free(op_decls);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (!idm_binding_table_add(&ctx->bindings, name_syntax->as.text, IDM_PHASE_ANY, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, &trait_scopes, payload, ctx->frame, NULL)) {
        free(op_decls);
        idm_scope_set_destroy(&trait_scopes);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    trait->dispatch_env_key = idm_strdup(ctx->unit_key);
    if (!trait->dispatch_env_key) {
        free(op_decls);
        idm_scope_set_destroy(&trait_scopes);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!install_method_surface(ctx, trait->name, name_syntax->as.text, trait_method_name_text(&methods[i]), methods[i].arity, &trait_scopes, ctx->unit, ctx->unit_key, methods[i].has_dispatch, trait->dispatch_env, trait->dispatch_env_key, methods[i].dispatch_slot, err)) {
            free(op_decls);
            idm_scope_set_destroy(&trait_scopes);
            surface_rollback(ctx, &install_checkpoint);
            idm_core_free(init);
            return NULL;
        }
    }
    for (size_t i = 0; i < op_decl_count; i++) {
        const TraitOperatorForm *opd = &op_decls[i];
        const char *target_text = opd->target ? opd->target->as.text : opd->symbol->as.text;
        if (!register_operator(ctx, opd->symbol->as.text, opd->capture, opd->precedence, opd->assoc, IDM_OPERATOR_ACTION_METHOD, target_text, &trait_scopes, &trait_scopes, ctx->unit, ctx->unit_key, trait->exported, opd->symbol->span, err)) {
            free(op_decls);
            idm_scope_set_destroy(&trait_scopes);
            surface_rollback(ctx, &install_checkpoint);
            idm_core_free(init);
            return NULL;
        }
    }
    free(op_decls);
    idm_scope_set_destroy(&trait_scopes);
    return init;
}

static ProtocolDef *protocol_def_by_identity_symbol(ExpandContext *ctx, IdmSymbol *identity) {
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
        if (entity->as.protocol.identity == identity) return &entity->as.protocol;
    }
    return NULL;
}

static ProtocolDef *protocol_def_by_identity(ExpandContext *ctx, const char *identity) {
    ProtocolDef *found = NULL;
    for (size_t i = 0; identity && i < ctx->typed.entity_count; i++) {
        TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL || strcmp(protocol_def_identity_text(&entity->as.protocol), identity) != 0) continue;
        if (found) return NULL;
        found = &entity->as.protocol;
    }
    return found;
}

typedef struct {
    const IdmArtifact *art;
    const char *identity;
    const char *name;
    IdmScopeId base;
    IdmScopeId min_id;
    int64_t delta;
    bool init_pending;
} ProtocolActivation;

static bool protocol_activation_collect(ExpandContext *ctx, ProtocolDef *p, ProtocolActivation **list, size_t *count, size_t *cap, IdmSpan span, IdmError *err) {
    const char *identity = protocol_def_identity_text(p);
    for (size_t i = 0; i < *count; i++) {
        const char *seen = (*list)[i].identity;
        if (seen && identity && strcmp(seen, identity) == 0) return true;
    }
    const IdmArtifact *art = p->art;
    const char *name = p->name;
    for (size_t i = 0; art && i < art->protocol_require_count; i++) {
        ProtocolDef *req = protocol_def_by_identity_symbol(ctx, art->protocol_requires[i]);
        if (!req) return idm_error_set(err, span, "activating '%s' requires protocol '%s', which is not visible here", name ? name : identity, idm_symbol_text(art->protocol_requires[i]));
        if (!protocol_activation_collect(ctx, req, list, count, cap, span, err)) return false;
    }
    if (*count == *cap && !idm_grow((void **)list, cap, sizeof(**list), 4u, *count + 1u)) return idm_error_oom(err, span);
    memset(&(*list)[*count], 0, sizeof(**list));
    (*list)[*count].art = art;
    (*list)[*count].identity = identity;
    (*list)[*count].name = name;
    (*count)++;
    return true;
}

bool ctx_activate_protocol_closure(ExpandContext *ctx, ProtocolDef *p, IdmError *err) {
    ProtocolActivation *acts = NULL;
    size_t act_count = 0;
    size_t act_cap = 0;
    if (!protocol_activation_collect(ctx, p, &acts, &act_count, &act_cap, idm_span_unknown(NULL), err)) {
        free(acts);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < act_count; i++) {
        ProtocolActivation *act = &acts[i];
        char runtime_key[65];
        artifact_provider_key(act->art->src_hash, runtime_key);
        ok = artifact_base(ctx, act->art, &act->base, err) &&
             activate_artifact(ctx, act->identity, act->art, act->base, &ctx->empty_scopes, idm_span_unknown(NULL), err) &&
             record_activation(ctx, act->name ? act->name : act->identity, act->identity, runtime_key, idm_span_unknown(NULL), err) &&
             install_artifact_runtime_slots_mode(ctx, act->art, act->art->scope_base, (int64_t)act->base - (int64_t)act->art->scope_base, true, true, idm_span_unknown(NULL), err) &&
             bind_artifact_exported_slots(ctx, act->art, &ctx->empty_scopes, idm_span_unknown(NULL), err);
    }
    free(acts);
    return ok;
}

IdmCore *expand_activate(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSpan span, IdmError *err) {
    const char *name = name_syntax->as.text;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *p = resolve_protocol_def(ctx, name_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, name_syntax->span, "ambiguous protocol '%s'", name);
    if (!p) return expand_error(err, name_syntax->span, "activate expects a protocol; '%s' is unbound", name);
    ProtocolActivation *acts = NULL;
    size_t act_count = 0;
    size_t act_cap = 0;
    if (!protocol_activation_collect(ctx, p, &acts, &act_count, &act_cap, span, err)) {
        free(acts);
        return NULL;
    }
    IdmScopeSet act_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &act_scopes)) {
        free(acts);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    SurfaceCheckpoint activation_checkpoint;
    surface_checkpoint(ctx, &activation_checkpoint);
    bool activated = true;
    for (size_t i = 0; activated && i < act_count; i++) {
        ProtocolActivation *act = &acts[i];
        char runtime_key[65];
        artifact_provider_key(act->art->src_hash, runtime_key);
        activated = artifact_base(ctx, act->art, &act->base, err) &&
                    activate_artifact(ctx, act->identity, act->art, act->base, &act_scopes, span, err) &&
                    record_activation(ctx, i + 1u == act_count ? name : act->name, act->identity, runtime_key, span, err);
    }
    if (!activated) {
        surface_discard(ctx, &activation_checkpoint);
    }
    idm_scope_set_destroy(&act_scopes);
    if (!activated) {
        free(acts);
        if (err && err->present && err->span.line == 0) idm_error_set_span(err, span);
        if (err && err->present && span.line != 0) idm_error_note_at(err, span, "while activating '%s'", name);
        return NULL;
    }
    for (size_t i = 0; i < act_count; i++) {
        ProtocolActivation *act = &acts[i];
        act->min_id = act->art->scope_base;
        act->delta = (int64_t)act->base - (int64_t)act->art->scope_base;
        if (!install_artifact_runtime_slots_mode(ctx, act->art, act->min_id, act->delta, true, true, span, err) ||
            !bind_artifact_exported_slots(ctx, act->art, &ctx->empty_scopes, span, err)) {
            free(acts);
            return NULL;
        }
        act->init_pending = artifact_init_pending(ctx, act->art);
    }
    IdmCore *cont = decl_done(err);
    if (!cont) {
        free(acts);
        return NULL;
    }
    for (size_t i = act_count; i > 0 && cont; i--) {
        ProtocolActivation *act = &acts[i - 1u];
        if (!act->init_pending) continue;
        cont = wrap_artifact_runtime_init(ctx, act->art, act->min_id, act->delta, false, false, cont, span, err);
    }
    free(acts);
    return cont;
}

static IdmCore *implement_trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *trait_syntax, const IdmSyntax *type_syntax, const IdmStructuralHead *structural, const IdmSyntax *body, IdmError *err) {
    const char *trait_name = trait_syntax->as.text;
    const char *type_name = type_syntax->as.text;
    IdmResolveStatus type_status = IDM_RESOLVE_UNBOUND;
    TypeDef *type_def = structural ? NULL : resolve_type_def(ctx, type_syntax, &type_status);
    if (type_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, type_syntax->span, "ambiguous type '%s'", type_name);
    IdmSymbol *type_identity = type_def ? type_def->identity : structural ? NULL : idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, type_name);
    IdmBuffer type_buf;
    idm_buf_init(&type_buf);
    if (structural && !idm_structural_head_write(&type_buf, structural)) return (IdmCore *)(uintptr_t)idm_error_oom(err, type_syntax->span);
    if (type_def) type_name = type_def_identity_text(type_def);
    else if (structural) type_name = type_buf.data;

    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    TraitDef *trait = resolve_trait_def(ctx, trait_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) { idm_buf_destroy(&type_buf); return expand_error(err, trait_syntax->span, "ambiguous trait '%s'", trait_name); }
    if (!trait) { idm_buf_destroy(&type_buf); return expand_error(err, trait_syntax->span, "unbound trait '%s'", trait_name); }

    IdmScopeSet method_scopes;
    if (!binder_scopes_pruned(ctx, trait_syntax, &method_scopes)) {
        idm_buf_destroy(&type_buf);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    IdmCore *implement_core = build_trait_implement_core(ctx, trait, trait_syntax->as.text, type_identity, structural, type_name, &method_scopes, body, form->span, err);
    idm_scope_set_destroy(&method_scopes);
    idm_buf_destroy(&type_buf);
    return implement_core;
}

IdmCore *expand_implement_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR) || form->as.seq.count < 5u || !syn_is_word(form->as.seq.items[1], "implement")) {
        return expand_error(err, form->span, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'");
    }
    size_t pos = 2u;
    size_t parse_end = form->as.seq.count;
    IdmSyntax *trait = expect_qualified_word_at(form->as.seq.items, pos, &parse_end, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'", err);
    if (!trait) return NULL;
    pos = parse_end;
    if (pos >= form->as.seq.count || !syn_is_word(form->as.seq.items[pos], "on")) {
        idm_syn_free(trait);
        return expand_error(err, form->span, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'");
    }
    pos++;
    parse_end = form->as.seq.count;
    IdmSyntax *type = expect_qualified_word_at(form->as.seq.items, pos, &parse_end, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'", err);
    if (!type) {
        idm_syn_free(trait);
        return NULL;
    }
    pos = parse_end;
    IdmStructuralHead structural;
    memset(&structural, 0, sizeof(structural));
    bool has_structural = type->kind == IDM_SYN_WORD && type->as.text[0] == '_' && type->as.text[1] == '.';
    if (has_structural && !parse_structural_head_syntax(ctx, type->as.text, (IdmSyntax *const *)form->as.seq.items, form->as.seq.count, &pos, NULL, &structural, err, type->span)) {
        idm_syn_free(type);
        idm_syn_free(trait);
        return NULL;
    }
    const IdmSyntax *body = NULL;
    if (pos < form->as.seq.count) {
        if (pos + 1u != form->as.seq.count || !idm_syn_is_form_id(form->as.seq.items[pos], IDM_FORM_BODY)) {
            idm_syn_free(type);
            idm_syn_free(trait);
            idm_structural_head_destroy(&structural);
            return expand_error(err, form->span, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'");
        }
        body = form->as.seq.items[pos];
    }
    IdmCore *core = implement_trait_decl_core(ctx, form, trait, type, has_structural ? &structural : NULL, body, err);
    idm_structural_head_destroy(&structural);
    idm_syn_free(type);
    idm_syn_free(trait);
    return core;
}

static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    size_t base = 1u;
    bool exported = false;
    if (form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(form->as.seq.items[base], "record") || form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD || !idm_syn_is_form_id(form->as.seq.items[base + 2u], IDM_FORM_BODY)) return false;
    *out_name = form->as.seq.items[base + 1u];
    *out_body = form->as.seq.items[base + 2u];
    if (out_exported) *out_exported = exported;
    return true;
}

static bool type_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_member_start, bool *out_exported) {
    if (!idm_syn_is_form_id(form, IDM_FORM_EXPR)) return false;
    size_t base = 1u;
    bool exported = false;
    if (form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count < base + 4u || !syn_is_word(form->as.seq.items[base], "type") || form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD) return false;
    const char *sep = surface_token_text(form->as.seq.items[base + 2u]);
    if (!sep || strcmp(sep, "::") != 0) return false;
    if (out_name) *out_name = form->as.seq.items[base + 1u];
    if (out_member_start) *out_member_start = base + 3u;
    if (out_exported) *out_exported = exported;
    return true;
}

static void record_fields_destroy(RecordFieldDecl *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(fields[i].name);
        if (fields[i].has_contract) idm_type_term_destroy(&fields[i].contract);
    }
    free(fields);
}

static void type_members_destroy(TypeMemberDef *members, size_t count) {
    for (size_t i = 0; i < count; i++) idm_type_term_destroy(&members[i].term);
    free(members);
}

static bool record_field_seen(const RecordFieldDecl *fields, size_t count, const char *name) {
    return find_named((void *)fields, count, sizeof(*fields), offsetof(RecordFieldDecl, name), name) != NULL;
}

static bool record_field_contract_marker(const IdmSyntax *syn) {
    const char *text = surface_token_text(syn);
    return text && strcmp(text, "::") == 0;
}

static bool parse_record_fields(ExpandContext *ctx, const IdmSyntax *body, RecordFieldDecl **out_fields, size_t *out_count, IdmError *err) {
    RecordFieldDecl *fields = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (!idm_syn_is_form_id(stmt, IDM_FORM_EXPR) || stmt->as.seq.count < 3u || !syn_is_word(stmt->as.seq.items[1], "field") || stmt->as.seq.items[2]->kind != IDM_SYN_WORD) {
            record_fields_destroy(fields, count);
            return idm_error_set(err, stmt->span, "record body expects 'field NAME' or 'field NAME :: TYPE' declarations");
        }
        IdmTypeTerm contract;
        memset(&contract, 0, sizeof(contract));
        bool has_contract = false;
        if (stmt->as.seq.count != 3u) {
            if (stmt->as.seq.count < 5u || !record_field_contract_marker(stmt->as.seq.items[3])) {
                record_fields_destroy(fields, count);
                return idm_error_set(err, stmt->span, "record body expects 'field NAME' or 'field NAME :: TYPE' declarations");
            }
            if (!parse_type_term_parts(ctx, (IdmSyntax *const *)stmt->as.seq.items, 4u, stmt->as.seq.count, &contract, err)) {
                record_fields_destroy(fields, count);
                return false;
            }
            has_contract = true;
        }
        const char *name = stmt->as.seq.items[2]->as.text;
        if (record_field_seen(fields, count, name)) {
            if (has_contract) idm_type_term_destroy(&contract);
            record_fields_destroy(fields, count);
            return idm_error_set(err, stmt->as.seq.items[2]->span, "record field '%s' is declared more than once", name);
        }
        if (count == cap) {
            if (!idm_grow((void **)&fields, &cap, sizeof(*fields), 4u, count + 1u)) {
                if (has_contract) idm_type_term_destroy(&contract);
                record_fields_destroy(fields, count);
                return idm_error_oom(err, stmt->span);
            }
        }
        memset(&fields[count], 0, sizeof(fields[count]));
        fields[count].name = idm_strdup(name);
        fields[count].has_contract = has_contract;
        fields[count].contract = contract;
        if (!fields[count].name) {
            record_fields_destroy(fields, count + 1u);
            return idm_error_oom(err, stmt->as.seq.items[2]->span);
        }
        count++;
    }
    *out_fields = fields;
    *out_count = count;
    return true;
}

static bool replace_type_term_symbol(IdmTypeTerm *term, IdmSymbol *symbol, IdmError *err, IdmSpan span) {
    if (!symbol) return idm_error_set(err, span, "type identity is missing");
    term->symbol = symbol;
    return true;
}

static bool resolve_type_term_names(ExpandContext *ctx, IdmTypeTerm *term, const char *self_name, IdmSymbol *identity, bool allow_unbound, IdmSpan span, IdmError *err) {
    if (!term) return true;
    if (term->kind == IDM_TYPE_CON && term->symbol) {
        const char *name = idm_type_term_text(term);
        if (strcmp(name, "_") != 0) {
            IdmSymbol *resolved = NULL;
            IdmSymbol *contract_symbol = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, name);
            if (!contract_symbol) return idm_error_oom(err, span);
            if (idm_value_builtin_type_symbol(contract_symbol)) {
                resolved = contract_symbol;
            } else if (self_name && strcmp(name, self_name) == 0) {
                resolved = identity;
            } else {
                IdmSyntax probe;
                memset(&probe, 0, sizeof(probe));
                probe.kind = IDM_SYN_WORD;
                probe.as.text = (char *)name;
                probe.span = span;
                IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
                TypeDef *type = resolve_type_def(ctx, &probe, &status);
                if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, span, "ambiguous type '%s'", name);
                if (!type) {
                    bool ambiguous_name = false;
                    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
                        TypedEntity *entity = &ctx->typed.entities[i];
                        if (entity->kind != IDM_TYPED_ENTITY_TYPE || !entity->as.type.name || strcmp(entity->as.type.name, name) != 0) continue;
                        if (type && type->identity != entity->as.type.identity) {
                            ambiguous_name = true;
                            break;
                        }
                        type = &entity->as.type;
                    }
                    if (ambiguous_name) type = NULL;
                }
                if (!type) {
                    if (allow_unbound) resolved = contract_symbol;
                    else return expand_error(err, span, "unbound type '%s'", name);
                } else {
                    resolved = type->identity;
                }
            }
            if (!replace_type_term_symbol(term, resolved, err, span)) return false;
        }
    }
    for (size_t i = 0; i < term->arg_count; i++) {
        if (!resolve_type_term_names(ctx, &term->args[i], self_name, identity, allow_unbound, span, err)) return false;
    }
    return true;
}

static bool resolve_record_field_contracts(ExpandContext *ctx, RecordFieldDecl *fields, size_t field_count, const char *record_name, IdmSymbol *identity, IdmError *err) {
    for (size_t i = 0; i < field_count; i++) {
        fields[i].name_symbol = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, fields[i].name);
        if (!fields[i].name_symbol) return idm_error_oom(err, idm_span_unknown(NULL));
        if (fields[i].has_contract &&
            !resolve_type_term_names(ctx, &fields[i].contract, record_name, identity, false, idm_span_unknown(NULL), err)) {
            return false;
        }
    }
    return true;
}

static bool resolve_type_members(ExpandContext *ctx, TypeMemberDef *members, size_t member_count, const char *self_name, IdmSymbol *identity, IdmError *err) {
    for (size_t i = 0; i < member_count; i++) {
        if (!resolve_type_term_names(ctx, &members[i].term, self_name, identity, true, idm_span_unknown(NULL), err)) return false;
    }
    return true;
}

static bool resolve_pending_type_member_term(ExpandContext *ctx, IdmTypeTerm *term, const char *surface_name, IdmSymbol *identity, IdmSpan span, IdmError *err) {
    if (!term) return true;
    if (term->kind == IDM_TYPE_CON && term->symbol && strcmp(idm_type_term_text(term), surface_name) == 0) {
        if (!replace_type_term_symbol(term, identity, err, span)) return false;
    }
    for (size_t i = 0; i < term->arg_count; i++) {
        if (!resolve_pending_type_member_term(ctx, &term->args[i], surface_name, identity, span, err)) return false;
    }
    return true;
}

static bool resolve_pending_type_members(ExpandContext *ctx, const char *surface_name, IdmSymbol *identity, IdmSpan span, IdmError *err) {
    if (!surface_name || !identity) return true;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_TYPE) continue;
        TypeDef *type = &entity->as.type;
        if (type->member_count == 0) continue;
        const IdmBinding *binding = NULL;
        IdmResolveStatus status = resolve_scoped(ctx, surface_name, IDM_BIND_SPACE_TYPE, &type->scopes, NULL, &binding);
        if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_TYPE) continue;
        TypeDef *resolved = typed_type_by_index(ctx, binding->payload);
        if (!resolved || resolved->identity != identity) continue;
        for (size_t m = 0; m < type->member_count; m++) {
            if (!resolve_pending_type_member_term(ctx, &type->members[m].term, surface_name, identity, span, err)) return false;
        }
    }
    return true;
}

static bool parse_type_members(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, TypeMemberDef **out_members, size_t *out_count, IdmError *err) {
    *out_members = NULL;
    *out_count = 0;
    IdmTypeTerm term;
    memset(&term, 0, sizeof(term));
    if (!parse_type_term_parts(ctx, items, start, count, &term, err)) return false;
    size_t member_count = term.kind == IDM_TYPE_UNION ? term.arg_count : 1u;
    TypeMemberDef *members = calloc(member_count, sizeof(*members));
    if (!members) {
        idm_type_term_destroy(&term);
        return idm_error_oom(err, items[start]->span);
    }
    bool ok = true;
    if (term.kind == IDM_TYPE_UNION) {
        for (size_t i = 0; ok && i < term.arg_count; i++) {
            ok = idm_type_term_copy(&members[i].term, &term.args[i]);
        }
    } else {
        ok = idm_type_term_copy(&members[0].term, &term);
    }
    idm_type_term_destroy(&term);
    if (!ok) {
        type_members_destroy(members, member_count);
        return idm_error_oom(err, items[start]->span);
    }
    *out_members = members;
    *out_count = member_count;
    return true;
}

static bool register_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *type_name, IdmSymbol *identity, bool exported, const TypeMemberDef *members, size_t member_count, TypeDef **out_type, IdmSpan span, IdmError *err) {
    if (out_type) *out_type = NULL;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    uint32_t payload = 0;
    TypeDef *type = typed_registry_add_type(ctx, &payload, err, span);
    if (!type) return false;
    type->name = idm_strdup(type_name);
    if (!type_def_set_identity(type, identity, err, span)) { surface_rollback(ctx, &checkpoint); return false; }
    type->exported = exported;
    if (!type->name) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
    if (member_count != 0) {
        type->members = calloc(member_count, sizeof(*type->members));
        if (!type->members) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
        type->member_count = member_count;
        for (size_t i = 0; i < member_count; i++) {
            if (!idm_type_term_copy(&type->members[i].term, &members[i].term)) {
                surface_rollback(ctx, &checkpoint);
                return idm_error_oom(err, span);
            }
        }
    }
    if (!binder_scopes_pruned(ctx, name_syntax, &type->scopes)) {
        surface_rollback(ctx, &checkpoint);
        return idm_error_oom(err, span);
    }
    if (!idm_binding_table_add(&ctx->bindings, type_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, &type->scopes, payload, ctx->frame, NULL)) {
        surface_rollback(ctx, &checkpoint);
        return idm_error_oom(err, span);
    }
    if (!resolve_pending_type_members(ctx, type_name, type->identity, span, err)) {
        surface_rollback(ctx, &checkpoint);
        return false;
    }
    if (out_type) *out_type = type;
    return true;
}

IdmCore *expand_type_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    const IdmSyntax *name_syntax = NULL;
    size_t member_start = 0;
    bool exported = false;
    if (!type_form_parts(form, &name_syntax, &member_start, &exported)) return expand_error(err, form->span, "type expects 'type NAME :: TYPE' or 'type NAME :: TYPE | TYPE ...'");
    TypeMemberDef *members = NULL;
    size_t member_count = 0;
    if (!parse_type_members(ctx, form->as.seq.items, member_start, form->as.seq.count, &members, &member_count, err)) return NULL;
    unsigned char hash[32];
    IdmSymbol *owned_identity = scoped_identity(ctx, name_syntax->as.text, form, hash, err);
    if (!owned_identity) {
        type_members_destroy(members, member_count);
        if (err && !err->present) idm_error_oom(err, name_syntax->span);
        return NULL;
    }
    if (!resolve_type_members(ctx, members, member_count, name_syntax->as.text, owned_identity, err)) {
        type_members_destroy(members, member_count);
        return NULL;
    }
    if (!register_type_surface(ctx, name_syntax, name_syntax->as.text, owned_identity, exported && ctx->in_package, members, member_count, NULL, form->span, err)) {
        type_members_destroy(members, member_count);
        return NULL;
    }
    type_members_destroy(members, member_count);
    return decl_done(err);
}

IdmCore *expand_record_field_core(ExpandContext *ctx, IdmCore *receiver, const TypeDef *type, uint32_t field_index, IdmSpan span, IdmError *err) {
    (void)ctx;
    IdmSymbol *identity = type_def_identity_symbol(type);
    if (!type || !identity || field_index >= type->field_count || !type->fields[field_index].name) {
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_set(err, span, "invalid record field metadata");
    }
    IdmCore *core = idm_core_record_field(receiver, identity, type->fields[field_index].name, field_index, span);
    if (!core) idm_core_free(receiver);
    return core;
}

static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const TypeDef *type, IdmSpan span, IdmError *err) {
    (void)ctx;
    if (type->field_count > UINT32_MAX) {
        idm_error_set(err, span, "record '%s' has too many fields", record_name);
        return NULL;
    }
    IdmCore *record = idm_core_record_construct(type_def_identity_symbol(type), span);
    if (!record) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeFieldDef *field = &type->fields[i];
        const char *name = type_field_name_text(field);
        IdmCore *arg = idm_core_arg_ref(name, (uint32_t)i, span);
        const IdmTypeTerm *contract = type_field_contract_term(field);
        if (!arg || !idm_core_record_construct_add(record, field->name, contract, arg)) {
            idm_core_free(arg);
            idm_core_free(record);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    IdmCore *fn = idm_core_fn(record_name, (uint32_t)type->field_count, record, span);
    if (!fn) {
        idm_core_free(record);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return fn;
}

static IdmCore *record_predicate_fn(ExpandContext *ctx, const TypeDef *type, const char *predicate_name, IdmSpan span, IdmError *err) {
    (void)ctx;
    IdmCore *arg = idm_core_arg_ref("value", 0u, span);
    if (!arg) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    IdmCore *test = idm_core_record_is(arg, type_def_identity_symbol(type), span);
    if (!test) {
        idm_core_free(arg);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *fn = idm_core_fn(predicate_name, 1u, test, span);
    if (!fn) {
        idm_core_free(test);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return fn;
}

static bool record_contract_add_arg(IdmCallableContract *contract, const IdmTypeTerm *term, IdmError *err, IdmSpan span) {
    if (contract->sig_count == 0 && !idm_contract_add_sig(contract)) return idm_error_oom(err, span);
    IdmContractSig *sig = &contract->sigs[0];
    if (!idm_grow((void **)&sig->args, &sig->arg_cap, sizeof(*sig->args), 4u, sig->arg_count + 1u)) return idm_error_oom(err, span);
    memset(&sig->args[sig->arg_count], 0, sizeof(sig->args[sig->arg_count]));
    if (!idm_type_term_copy(&contract->sigs[0].args[contract->sigs[0].arg_count], term)) return idm_error_oom(err, span);
    contract->sigs[0].arg_count++;
    return true;
}

static bool record_constructor_contract(ExpandContext *ctx, const TypeDef *type, IdmCallableContract *contract, IdmError *err, IdmSpan span) {
    memset(contract, 0, sizeof(*contract));
    if (!idm_contract_add_sig(contract)) return idm_error_oom(err, span);
    for (size_t i = 0; i < type->field_count; i++) {
        IdmTypeTerm any;
        memset(&any, 0, sizeof(any));
        const IdmTypeTerm *arg = type->fields[i].has_contract ? &type->fields[i].contract : NULL;
        if (!arg) {
            if (!idm_type_var(ctx->rt, &any, "_", 0u, false)) return idm_error_oom(err, span);
            arg = &any;
        }
        bool ok = record_contract_add_arg(contract, arg, err, span);
        idm_type_term_destroy(&any);
        if (!ok) {
            idm_callable_contract_destroy(contract);
            return false;
        }
    }
    if (!idm_type_con_symbol(&contract->sigs[0].result, type_def_identity_symbol(type))) {
        idm_callable_contract_destroy(contract);
        return idm_error_oom(err, span);
    }
    contract->sigs[0].has_result = true;
    return true;
}

static bool record_predicate_contract(ExpandContext *ctx, IdmCallableContract *contract, IdmError *err, IdmSpan span) {
    memset(contract, 0, sizeof(*contract));
    if (!idm_contract_add_sig(contract)) return idm_error_oom(err, span);
    IdmTypeTerm arg;
    memset(&arg, 0, sizeof(arg));
    if (!idm_type_var(ctx->rt, &arg, "_", 0u, false)) return idm_error_oom(err, span);
    bool ok = record_contract_add_arg(contract, &arg, err, span);
    idm_type_term_destroy(&arg);
    if (!ok) {
        idm_callable_contract_destroy(contract);
        return false;
    }
    if (!idm_type_con(ctx->rt, &contract->sigs[0].result, "atom")) {
        idm_callable_contract_destroy(contract);
        return idm_error_oom(err, span);
    }
    contract->sigs[0].has_result = true;
    return true;
}

static char *record_predicate_name(const char *record_name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_buf_append(&buf, record_name) || !idm_buf_append_char(&buf, '?')) { idm_buf_destroy(&buf); return NULL; }
    return idm_buf_take(&buf);
}

static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, IdmSymbol *identity, bool exported, const RecordFieldDecl *fields, size_t field_count, TypeDef **out_type, IdmSpan span, IdmError *err) {
    if (out_type) *out_type = NULL;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    uint32_t payload = 0;
    TypeDef *type = typed_registry_add_type(ctx, &payload, err, span);
    if (!type) return false;
    type->name = idm_strdup(record_name);
    if (!type_def_set_identity(type, identity, err, span)) { surface_rollback(ctx, &checkpoint); return false; }
    type->exported = exported;
    if (!type->name) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
    if (field_count != 0) {
        type->fields = calloc(field_count, sizeof(*type->fields));
        if (!type->fields) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
        type->field_count = field_count;
        for (size_t i = 0; i < field_count; i++) {
            if (!fields[i].name_symbol) { surface_rollback(ctx, &checkpoint); return idm_error_set(err, span, "record field metadata missing symbol"); }
            type->fields[i].name = fields[i].name_symbol;
            if (fields[i].has_contract) {
                if (!idm_type_term_copy(&type->fields[i].contract, &fields[i].contract)) {
                    surface_rollback(ctx, &checkpoint);
                    return idm_error_oom(err, span);
                }
                type->fields[i].has_contract = true;
            }
        }
    }

    if (!binder_scopes_pruned(ctx, name_syntax, &type->scopes)) {
        surface_rollback(ctx, &checkpoint);
        return idm_error_oom(err, span);
    }
    if (!idm_binding_table_add(&ctx->bindings, record_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, &type->scopes, payload, ctx->frame, NULL)) {
        surface_rollback(ctx, &checkpoint);
        return idm_error_oom(err, span);
    }
    if (!install_field_surfaces(ctx, type, payload, &type->scopes, err)) {
        surface_rollback(ctx, &checkpoint);
        return false;
    }
    if (!resolve_pending_type_members(ctx, record_name, type->identity, span, err)) {
        surface_rollback(ctx, &checkpoint);
        return false;
    }
    if (out_type) *out_type = type;
    return true;
}

IdmCore *expand_record_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    const IdmSyntax *name_syntax = NULL;
    const IdmSyntax *body = NULL;
    bool exported = false;
    if (!record_form_parts(form, &name_syntax, &body, &exported)) return expand_error(err, form->span, "record expects 'record NAME do field ... end'");
    const char *record_name = name_syntax->as.text;
    RecordFieldDecl *fields = NULL;
    size_t field_count = 0;
    if (!parse_record_fields(ctx, body, &fields, &field_count, err)) return NULL;
    if (field_count > UINT32_MAX) {
        record_fields_destroy(fields, field_count);
        return expand_error(err, name_syntax->span, "record has too many fields");
    }
    unsigned char hash[32];
    char *predicate_name = record_predicate_name(record_name);
    IdmSymbol *owned_identity = predicate_name ? scoped_identity(ctx, record_name, form, hash, err) : NULL;
    if (!predicate_name || !owned_identity) {
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        if (err && !err->present) idm_error_oom(err, name_syntax->span);
        return NULL;
    }
    if (!resolve_record_field_contracts(ctx, fields, field_count, record_name, owned_identity, err)) {
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }

    TypeDef *record_type = NULL;
    IdmSymbol *record_identity = NULL;
    if (!register_record_type_surface(ctx, name_syntax, record_name, owned_identity, exported && ctx->in_package, fields, field_count, &record_type, form->span, err)) {
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }

    bool top_level = ctx->frame == IDM_FRAME_TOP;
    size_t saved_count = ctx->bindings.count;
    uint32_t saved_next = ctx->next_slot;
    uint32_t constructor_slot = 0;
    uint32_t predicate_slot = 0;
    IdmArity constructor_arity = idm_arity_exact((uint32_t)field_count);
    IdmArity predicate_arity = idm_arity_exact(1u);
    IdmCallableContract constructor_contract;
    IdmCallableContract predicate_contract;
    bool contracts_ready = false;
    record_identity = record_type ? type_def_identity_symbol(record_type) : NULL;
    if (!record_constructor_contract(ctx, record_type, &constructor_contract, err, form->span) ||
        !record_predicate_contract(ctx, &predicate_contract, err, form->span)) {
        idm_callable_contract_destroy(&constructor_contract);
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    contracts_ready = true;
    bool ok = top_level ? env_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_contract, &constructor_slot) : local_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_contract, &constructor_slot);
    IdmSyntax *predicate_syntax = NULL;
    if (ok) {
        predicate_syntax = idm_syn_word(predicate_name, name_syntax->span);
        if (!predicate_syntax) ok = false;
    }
    if (ok) {
        const IdmScopeSet *scopes = idm_syn_scope_set(name_syntax, 0);
        if (scopes) for (size_t i = 0; i < scopes->count && ok; i++) ok = idm_syn_scope_add(predicate_syntax, 0, scopes->items[i]);
    }
    if (ok) ok = top_level ? env_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_contract, &predicate_slot) : local_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_contract, &predicate_slot);
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (top_level && ctx->in_package) {
        IdmScopeSet record_scopes;
        idm_scope_set_init(&record_scopes);
        if (!syntax_scopes_copy(&record_scopes, name_syntax)) ok = false;
        if (ok && !record_package_slot(ctx, record_name, constructor_slot, &record_scopes, constructor_arity, &constructor_contract, exported)) ok = false;
        if (ok && !record_package_slot(ctx, predicate_name, predicate_slot, &record_scopes, predicate_arity, &predicate_contract, exported)) ok = false;
        idm_scope_set_destroy(&record_scopes);
    }
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }

    IdmCore *constructor = record_constructor_fn(ctx, record_name, record_type, form->span, err);
    IdmCore *predicate = constructor ? record_predicate_fn(ctx, record_type, predicate_name, form->span, err) : NULL;
    if (!constructor || !predicate) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    if (!expand_typecheck_value(ctx, record_name, &constructor, &constructor_contract, &contracts_ready, true, err) ||
        !expand_typecheck_value(ctx, predicate_name, &predicate, &predicate_contract, &contracts_ready, true, err)) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_callable_contract_destroy(&constructor_contract);
        idm_callable_contract_destroy(&predicate_contract);
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *cont = decl_done(err);
    if (!cont) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *letrec = idm_core_letrec(form->span);
    if (!letrec) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(cont);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    if (top_level) idm_core_letrec_set_env(letrec);
    bool letrec_ok = idm_core_letrec_add(letrec, record_name, constructor_slot, constructor);
    if (letrec_ok) constructor = NULL;
    if (letrec_ok) {
        letrec_ok = idm_core_letrec_add(letrec, predicate_name, predicate_slot, predicate);
        if (letrec_ok) predicate = NULL;
    }
    const TypeDef *sel_type = record_identity ? type_def_lookup_name(ctx, idm_symbol_text(record_identity)) : NULL;
    for (size_t si = 0; letrec_ok && sel_type && si < ctx->typed.method_surface_count; si++) {
        const MethodSurfaceDef *surface = &ctx->typed.method_surfaces[si];
        if (!surface->has_dispatch) continue;
        bool structural_member = false;
        for (size_t ii = 0; ii < ctx->typed.method_impl_count; ii++) {
            const MethodImplDef *impl = &ctx->typed.method_impls[ii];
            if (!impl->structural) continue;
            if (!method_impl_matches_identity(ctx, impl, method_surface_trait_text(surface), method_surface_name_text(surface), NULL)) continue;
            if (type_satisfies_structural(ctx, &impl->structural_head, record_identity)) { structural_member = true; break; }
        }
        if (!structural_member) continue;
        IdmCore *dispatcher = build_method_dispatcher_core(ctx, method_surface_trait_text(surface), method_surface_name_text(surface), form->span, err);
        if (!dispatcher) { letrec_ok = false; break; }
        bool added = surface->dispatch_env && surface->dispatch_env_key && surface->dispatch_env_key[0]
            ? idm_core_letrec_add_env_fill(letrec, method_surface_name_text(surface), idm_atom(ctx->rt, surface->dispatch_env_key), surface->dispatch_slot, dispatcher, true)
            : idm_core_letrec_add_fill(letrec, method_surface_name_text(surface), surface->dispatch_slot, dispatcher, true);
        if (!added) {
            idm_core_free(dispatcher);
            letrec_ok = false;
            break;
        }
    }
    for (size_t fi = 0; letrec_ok && sel_type && fi < sel_type->field_count; fi++) {
        const char *fname = type_field_name_text(&sel_type->fields[fi]);
        if (!fname) continue;
        FieldSelectorDef *sel = field_selector_ensure(ctx, fname, err);
        if (!sel) { letrec_ok = false; break; }
        IdmCore *sel_core = build_field_selector_core(ctx, fname, form->span, err);
        if (!sel_core) { letrec_ok = false; break; }
        if (!sel->env && sel->emitted) sel->slot = ctx->next_slot++;
        letrec_ok = sel->env_key && sel->env_key[0]
            ? idm_core_letrec_add_env_fill(letrec, fname, idm_atom(ctx->rt, sel->env_key), sel->slot, sel_core, true)
            : idm_core_letrec_add_fill(letrec, fname, sel->slot, sel_core, sel->env && sel->emitted);
        if (!letrec_ok) idm_core_free(sel_core);
        else sel->emitted = true;
    }
    if (letrec_ok) {
        letrec_ok = idm_core_letrec_set_body(letrec, cont);
        if (letrec_ok) cont = NULL;
    }
    if (!letrec_ok) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(cont);
        idm_core_free(letrec);
        if (contracts_ready) {
            idm_callable_contract_destroy(&constructor_contract);
            idm_callable_contract_destroy(&predicate_contract);
        }
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    idm_syn_free(predicate_syntax);
    if (contracts_ready) {
        idm_callable_contract_destroy(&constructor_contract);
        idm_callable_contract_destroy(&predicate_contract);
    }
    free(predicate_name);
    record_fields_destroy(fields, field_count);
    return letrec;
}
