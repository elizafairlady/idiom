#include "internal.h"

#include <stddef.h>

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err);
static IdmSyntax *require_form_trait_word(const IdmSyntax *form, IdmError *err);
static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, const char *name);
static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, const char *name, IdmError *err, IdmSpan span);
static void trait_requirements_destroy(IdmTraitRequirementDef *requirements, size_t count);
static bool trait_method_impl_seen(ExpandContext *ctx, const char *trait, const char *type, const TraitMethodDef *method);
static bool check_trait_requirements_for_type(ExpandContext *ctx, const TraitDef *trait, const char *type, IdmSpan span, IdmError *err);
static bool install_required_trait_methods(ExpandContext *ctx, const TraitDef *required, const IdmSyntax *name_syntax, IdmError *err);
static bool method_signature_arity(const IdmSyntax *form, IdmArity *out_arity, IdmError *err);
static bool method_arity_mismatch(IdmError *err, IdmSpan span, const char *trait, const char *name, const IdmArity *expected, const IdmArity *got);
static TraitMethodDef *find_decl_method(ExpandContext *ctx, const char *name);
static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmArity arity, IdmError *err);
static void trait_impls_destroy(TraitMethodImpl *impls, size_t count);
static const TraitMethodDef *find_trait_method_def(ExpandContext *ctx, const TraitMethodDef *methods, size_t count, const char *name);
static bool trait_has_impl(ExpandContext *ctx, const TraitMethodImpl *impls, size_t count, const char *name);
static bool trait_impls_push(ExpandContext *ctx, TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, IdmArity arity, IdmCore *fn, IdmError *err, IdmSpan span);
static IdmCore *build_trait_implement_core(ExpandContext *ctx, const TraitDef *trait, const char *trait_key, const char *type, const IdmScopeSet *method_scopes, const IdmSyntax *body, IdmSpan span, IdmError *err);
static IdmCore *sequence_two(IdmCore *first, IdmCore *second, IdmSpan span, IdmError *err);
static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported);
typedef struct {
    char *name;
    IdmSymbol *name_symbol;
    IdmSyntax *contract_syntax;
    IdmSymbol *contract_symbol;
} RecordFieldDecl;
static void record_fields_destroy(RecordFieldDecl *fields, size_t count);
static bool record_field_seen(const RecordFieldDecl *fields, size_t count, const char *name);
static bool parse_record_fields(const IdmSyntax *body, RecordFieldDecl **out_fields, size_t *out_count, IdmError *err);
static bool resolve_record_field_contracts(ExpandContext *ctx, RecordFieldDecl *fields, size_t field_count, const char *record_name, const char *identity, IdmError *err);
static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const TypeDef *type, IdmSpan span, IdmError *err);
static IdmCore *record_predicate_fn(ExpandContext *ctx, const TypeDef *type, const char *predicate_name, IdmSpan span, IdmError *err);
static char *record_predicate_name(const char *record_name);
static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, bool exported, const RecordFieldDecl *fields, size_t field_count, TypeDef **out_type, IdmSpan span, IdmError *err);
static IdmCore *trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);

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
    if (!syn_is_form(form, "%-expr")) return false;
    if (form->as.seq.count < 3 || !syn_is_word(form->as.seq.items[1], "method") || form->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    bool has_body = false;
    for (size_t i = 3; i < form->as.seq.count; i++) {
        const IdmSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || syn_is_form(item, "%-body")) {
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

static IdmSyntax *require_form_trait_word(const IdmSyntax *form, IdmError *err) {
    if (!syn_is_form(form, "%-expr")) return NULL;
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

static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, const char *name) {
    return find_named((void *)requirements, count, sizeof(*requirements), offsetof(IdmTraitRequirementDef, name), name) != NULL;
}

static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, const char *name, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)requirements, cap, sizeof(**requirements), 4u, *count + 1u)) return idm_error_oom(err, span);
    }
    IdmTraitRequirementDef *requirement = &(*requirements)[(*count)++];
    memset(requirement, 0, sizeof(*requirement));
    requirement->name = idm_strdup(name);
    if (!requirement->name) {
        (*count)--;
        return idm_error_oom(err, span);
    }
    return true;
}

static void trait_requirements_destroy(IdmTraitRequirementDef *requirements, size_t count) {
    for (size_t i = 0; i < count; i++) idm_trait_requirement_def_destroy(&requirements[i]);
    free(requirements);
}

static bool trait_method_impl_seen(ExpandContext *ctx, const char *trait, const char *type, const TraitMethodDef *method) {
    const char *method_name = trait_method_name_text(method);
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        const MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (!method_impl_matches_identity(ctx, impl, trait, method_name, type)) continue;
        if (idm_arity_equal(&impl->arity, &method->arity)) return true;
    }
    return false;
}

static bool check_trait_requirements_for_type(ExpandContext *ctx, const TraitDef *trait, const char *type, IdmSpan span, IdmError *err) {
    const char *trait_name = trait_def_identity_text(trait);
    for (size_t i = 0; i < trait->requirement_count; i++) {
        const char *required_name = trait->requirements[i].name;
        const TraitDef *required = typed_trait_by_identity(ctx, required_name);
        if (!required) return expand_error(err, span, "required trait '%s' is not available while implementing trait '%s'", required_name, trait_name);
        for (size_t m = 0; m < required->method_count; m++) {
            if (!trait_method_impl_seen(ctx, required_name, type, &required->methods[m])) {
                return expand_error(err, span, "required trait '%s' is not implemented on type '%s' for trait '%s'", required_name, type, trait_name);
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
        ok = install_method_surface(ctx, trait_def_identity_text(required), name_syntax->as.text, trait_method_name_text(&required->methods[i]), required->methods[i].arity, &method_scopes, ctx->unit, ctx->unit_key, required->methods[i].has_dispatch, required->dispatch_env, required->dispatch_env_key, required->methods[i].dispatch_slot, err);
    }
    idm_scope_set_destroy(&method_scopes);
    return ok;
}

static bool method_signature_arity(const IdmSyntax *form, IdmArity *out_arity, IdmError *err) {
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    if (!method_form_parts(form, &name, &param_start, NULL, err)) return false;
    return function_literal_arity(name, form->as.seq.items, param_start, form->as.seq.count, out_arity, err);
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
    method->dispatch_slot = ctx->frame == IDM_FRAME_TOP ? ctx->env_slot_seq++ : ctx->next_slot++;
    if (!binder_scopes_pruned(ctx, name_syntax, &method->scopes)) {
        trait_method_def_destroy(method);
        return idm_error_oom(err, name_syntax->span);
    }
    ctx->decl_method_count++;
    return install_method_surface(ctx, ctx->trait_name ? ctx->trait_name : "<anonymous>", ctx->trait_key ? ctx->trait_key : (ctx->trait_name ? ctx->trait_name : "<anonymous>"), trait_method_name_text(method), arity, &method->scopes, ctx->unit, ctx->unit_key, true, ctx->frame == IDM_FRAME_TOP, NULL, method->dispatch_slot, err);
}

bool predeclare_trait_methods(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmError *err) {
    if (!ctx->trait_name) return true;
    for (size_t i = start; i < count; i++) {
        const IdmSyntax *name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(items[i], &name, &param_start, &has_body, NULL)) continue;
        (void)param_start;
        (void)has_body;
        IdmArity arity = idm_arity_unknown();
        if (!method_signature_arity(items[i], &arity, err)) return false;
        if (!add_decl_method(ctx, name, arity, err)) return false;
    }
    return true;
}

IdmCore *expand_method_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    if (!ctx->trait_name) return expand_error(err, form->span, "method declarations are only valid inside trait bodies");
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return NULL;
    IdmArity arity = idm_arity_unknown();
    if (!method_signature_arity(form, &arity, err)) return NULL;
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
    }
    return expand_body_items(ctx, items, index + 1u, count, false, err);
}

static void trait_impls_destroy(TraitMethodImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) {
        idm_core_free(impls[i].fn);
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

static bool trait_impls_push(ExpandContext *ctx, TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, IdmArity arity, IdmCore *fn, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)impls, cap, sizeof(**impls), 4u, *count + 1u)) return idm_error_oom(err, span);
    }
    TraitMethodImpl *impl = &(*impls)[(*count)++];
    impl->arity = arity;
    impl->fn = fn;
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
    if (!idm_core_fn_multi_add_capture(multi, IDM_CAP_LOCAL, method_surface_name_text(surface), impl->impl_slot)) return false;
    for (size_t i = 0; i < multi->as.fn_multi.capture_count; i++) {
        const IdmCapture *capture = &multi->as.fn_multi.captures[i];
        if (capture->kind == IDM_CAP_LOCAL && capture->index == impl->impl_slot) {
            *out = idm_core_capture_ref(method_surface_name_text(surface), (uint32_t)i, multi->span);
            return *out != NULL;
        }
    }
    return false;
}

static bool method_impl_record(ExpandContext *ctx, uint32_t method_surface, const char *type, IdmArity arity, bool impl_env, const char *impl_env_key, uint32_t impl_slot, IdmError *err, IdmSpan span) {
    const MethodSurfaceDef *surface = method_surface_by_index(ctx, method_surface);
    if (!surface) return idm_error_set(err, span, "method implementation references missing surface");
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (!method_impl_matches_identity(ctx, impl, method_surface_trait_text(surface), method_surface_name_text(surface), type)) continue;
        impl->arity = arity;
        impl->impl_env = impl_env;
        impl->impl_slot = impl_slot;
        char *key = idm_strdup(impl_env_key ? impl_env_key : "");
        if (!key) return idm_error_oom(err, span);
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
    impl->impl_slot = impl_slot;
    if (!impl->impl_env_key || !method_impl_set_type(ctx, impl, type, err, span)) {
        free(impl->impl_env_key);
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

static bool dispatch_add_clause(ExpandContext *ctx, IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, uint32_t argc, IdmSpan span, IdmError *err) {
    ImplDispatchBody body = {impl, surface};
    const char *type = method_impl_type_text(impl);
    if (!type) return idm_error_set(err, span, "method implementation requires a receiver type");
    return expand_multi_add_dispatch_clause(ctx, multi, argc, type, impl_dispatch_clause_body, &body, span, err);
}

typedef bool (*DispatchArityClauseFn)(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err);

static IdmCore *method_unimplemented_body(ExpandContext *ctx, const char *trait, const char *method, IdmSpan span, IdmError *err) {
    IdmBuffer msg;
    idm_buf_init(&msg);
    bool ok = idm_buf_appendf(&msg, "method '%s' does not implement trait '%s'", method, trait);
    IdmCore *body = ok ? expand_raise_message_body(ctx, msg.data ? msg.data : "", span, err) : NULL;
    idm_buf_destroy(&msg);
    if (!ok) idm_error_oom(err, span);
    return body;
}

static bool dispatch_add_arity_clauses(ExpandContext *ctx, IdmCore *multi, const char *trait, const char *method, const IdmArity *arity, DispatchArityClauseFn clause_fn, const void *user, IdmError *err, IdmSpan span) {
    if (arity->kind == IDM_ARITY_UNKNOWN) return idm_error_set(err, span, "method '%s.%s' has unknown arity", trait, method);
    if (arity->max < arity->min || arity->max - arity->min > 64u) return idm_error_set(err, span, "method '%s.%s' arity range is too wide for dispatch generation", trait, method);
    for (uint32_t argc = arity->min; argc <= arity->max; argc++) {
        if (idm_arity_accepts(arity, argc) && !clause_fn(ctx, multi, user, argc, span, err)) return false;
        if (argc == UINT32_MAX) break;
    }
    return true;
}

static bool dispatch_add_impl_clause_at_arity(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const ImplDispatchBody *body = user;
    return dispatch_add_clause(ctx, multi, body->impl, body->surface, argc, span, err);
}

static bool dispatch_add_impl_clauses(ExpandContext *ctx, IdmCore *multi, const MethodImplDef *impl, const MethodSurfaceDef *surface, IdmError *err, IdmSpan span) {
    ImplDispatchBody body = {impl, surface};
    return dispatch_add_arity_clauses(ctx, multi, method_surface_trait_text(surface), method_surface_name_text(surface), &impl->arity, dispatch_add_impl_clause_at_arity, &body, err, span);
}

typedef struct {
    const char *trait;
    const TraitMethodDef *method;
} UnimplementedClauseBody;

static IdmCore *unimplemented_dispatch_clause_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const UnimplementedClauseBody *body = user;
    (void)multi;
    (void)argc;
    return method_unimplemented_body(ctx, body->trait, trait_method_name_text(body->method), span, err);
}

static bool dispatch_add_unimplemented_clause(ExpandContext *ctx, IdmCore *multi, const char *trait, const TraitMethodDef *method, uint32_t argc, IdmSpan span, IdmError *err) {
    UnimplementedClauseBody body = {trait, method};
    return expand_multi_add_dispatch_clause(ctx, multi, argc, NULL, unimplemented_dispatch_clause_body, &body, span, err);
}

static bool dispatch_add_unimplemented_clause_at_arity(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const UnimplementedClauseBody *body = user;
    return dispatch_add_unimplemented_clause(ctx, multi, body->trait, body->method, argc, span, err);
}

static bool dispatch_add_unimplemented_clauses(ExpandContext *ctx, IdmCore *multi, const char *trait, const TraitMethodDef *method, IdmError *err, IdmSpan span) {
    UnimplementedClauseBody body = {trait, method};
    return dispatch_add_arity_clauses(ctx, multi, trait, trait_method_name_text(method), &method->arity, dispatch_add_unimplemented_clause_at_arity, &body, err, span);
}

static IdmCore *method_empty_dispatcher_fn(ExpandContext *ctx, const char *trait, const TraitMethodDef *method, IdmSpan span, IdmError *err) {
    IdmCore *multi = idm_core_fn_multi(trait_method_name_text(method), span);
    if (!multi) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    if (!dispatch_add_unimplemented_clauses(ctx, multi, trait, method, err, span)) {
        idm_core_free(multi);
        return NULL;
    }
    return multi;
}

static IdmCore *method_dispatcher_fn(ExpandContext *ctx, const char *trait, const TraitMethodDef *method, IdmSpan span, IdmError *err) {
    const char *method_name = trait_method_name_text(method);
    IdmCore *multi = idm_core_fn_multi(method_name, span);
    if (!multi) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    size_t clauses = 0;
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        const MethodImplDef *impl = &ctx->typed.method_impls[i];
        const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl->method_surface);
        if (!surface) {
            idm_error_set(err, span, "method implementation references missing surface");
            idm_core_free(multi);
            return NULL;
        }
        if (!method_impl_matches_identity(ctx, impl, trait, method_name, NULL)) continue;
        if (!dispatch_add_impl_clauses(ctx, multi, impl, surface, err, span)) {
            idm_core_free(multi);
            return NULL;
        }
        clauses++;
    }
    (void)clauses;
    if (!dispatch_add_unimplemented_clauses(ctx, multi, trait, method, err, span)) {
        idm_core_free(multi);
        return NULL;
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

IdmCore *build_method_dispatch_refresh_core(ExpandContext *ctx, IdmSpan span, IdmError *err) {
    IdmCore *core = NULL;
    RefreshSlot *slots = NULL;
    size_t slot_count = 0;
    size_t slot_cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < ctx->typed.method_surface_count; i++) {
        const MethodSurfaceDef *surface = &ctx->typed.method_surfaces[i];
        if (!surface->has_dispatch || !surface->dispatch_env || !surface->dispatch_env_key || !surface->dispatch_env_key[0]) continue;
        if (refresh_slot_seen(slots, slot_count, surface->dispatch_env_key, surface->dispatch_slot)) continue;
        if (!refresh_slot_push(&slots, &slot_count, &slot_cap, surface->dispatch_env_key, surface->dispatch_slot, err, span)) {
            ok = false;
            break;
        }
        const TraitDef *trait = typed_trait_by_identity(ctx, method_surface_trait_text(surface));
        const TraitMethodDef *method = trait ? find_trait_method_def(ctx, trait->methods, trait->method_count, method_surface_name_text(surface)) : NULL;
        if (!method) continue;
        if (!core) {
            core = idm_core_letrec(span);
            if (!core) {
                ok = idm_error_oom(err, span);
                break;
            }
            idm_core_letrec_set_env(core);
        }
        IdmCore *dispatcher = method_dispatcher_fn(ctx, method_surface_trait_text(surface), method, span, err);
        if (!dispatcher || !idm_core_letrec_add_env_fill(core, method_surface_name_text(surface), idm_atom(ctx->rt, surface->dispatch_env_key), surface->dispatch_slot, dispatcher, false)) {
            idm_core_free(dispatcher);
            ok = err && err->present ? false : idm_error_oom(err, span);
            break;
        }
    }
    free(slots);
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

static const MethodSurfaceDef *current_dispatch_surface_index(ExpandContext *ctx, const char *trait, const char *method, const IdmScopeSet *scopes, uint32_t *out_index, IdmError *err, IdmSpan span) {
    const IdmScopeSet *check_scopes = scopes ? scopes : &ctx->empty_scopes;
    const IdmScopeSet *best = NULL;
    if (out_index) *out_index = UINT32_MAX;
    IdmResolveStatus status = idm_binding_resolve_scopes(&ctx->bindings, method, ctx->phase, IDM_BIND_SPACE_METHOD, check_scopes, &best);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, span, "ambiguous method '%s'", method);
        return NULL;
    }
    if (status != IDM_RESOLVE_OK || !best) return NULL;
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
        const MethodSurfaceDef *surface = current_dispatch_surface_index(ctx, trait_def_identity_text(trait), method_name, method_scopes, &method_surface, err, span);
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
            bindings[i].slot = ctx->frame == IDM_FRAME_TOP ? ctx->env_slot_seq++ : ctx->next_slot++;
            bindings[i].env = ctx->frame == IDM_FRAME_TOP;
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

static IdmCore *build_trait_implement_core(ExpandContext *ctx, const TraitDef *trait_def, const char *trait_key, const char *type, const IdmScopeSet *method_scopes, const IdmSyntax *body, IdmSpan span, IdmError *err) {
    const char *trait = trait_def_identity_text(trait_def);
    const TraitMethodDef *methods = trait_def->methods;
    size_t method_count = trait_def->method_count;
    if (!check_trait_requirements_for_type(ctx, trait_def, type, span, err)) return NULL;
    ImplementDispatchBinding *dispatch = NULL;
    if (!implement_dispatch_bindings_prepare(ctx, trait_def, method_scopes, &dispatch, err, span)) return NULL;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool surface_ok = true;
    for (size_t i = 0; i < method_count && surface_ok; i++) surface_ok = install_method_surface(ctx, trait, trait_key, trait_method_name_text(&methods[i]), methods[i].arity, method_scopes, ctx->unit, ctx->unit_key, true, dispatch[i].env, dispatch[i].env_key, dispatch[i].slot, err);
    if (!surface_ok) {
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
        if (!method_signature_arity(stmt, &arity, err)) { ok = false; break; }
        const TraitMethodDef *contract = find_trait_method_def(ctx, methods, method_count, name->as.text);
        if (!contract) { expand_error(err, name->span, "trait '%s' has no method '%s'", trait, name->as.text); ok = false; break; }
        if (!idm_arity_equal(&contract->arity, &arity)) { method_arity_mismatch(err, name->span, trait, name->as.text, &contract->arity, &arity); ok = false; break; }
        if (trait_has_impl(ctx, impls, impl_count, name->as.text)) { expand_error(err, name->span, "implement for '%s' provides method '%s' more than once", trait, name->as.text); ok = false; break; }
        IdmCore *fn = expand_function_literal(ctx, name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
        if (!fn) { ok = false; break; }
        if (!trait_impls_push(ctx, &impls, &impl_count, &impl_cap, name->as.text, arity, fn, err, name->span)) {
            idm_core_free(fn);
            ok = false;
            break;
        }
    }
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        trait_impls_destroy(impls, impl_count);
        implement_dispatch_bindings_destroy(dispatch, method_count);
        return NULL;
    }
    for (size_t i = 0; i < method_count; i++) {
        const char *method_name = trait_method_name_text(&methods[i]);
        if (!methods[i].has_default && !trait_has_impl(ctx, impls, impl_count, method_name)) {
            expand_error(err, body ? body->span : span, "implement for '%s' on type '%s' is missing required method '%s'", trait, type, method_name);
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
        if (method_surface == UINT32_MAX && !method_surface_index_by_identity(ctx, trait, method_name, &method_surface)) {
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            implement_dispatch_bindings_destroy(dispatch, method_count);
            expand_error(err, span, "method implementation references missing surface");
            return NULL;
        }
        if (provided) {
            uint32_t impl_slot = letrec_env ? ctx->env_slot_seq++ : ctx->next_slot++;
            IdmCore *fn = provided->fn;
            provided->fn = NULL;
            if (!idm_core_letrec_add(core, trait_method_impl_name_text(provided), impl_slot, fn)) {
                idm_core_free(fn);
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
            if (!method_impl_record(ctx, method_surface, type, methods[i].arity, letrec_env, NULL, impl_slot, err, span)) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return NULL;
            }
        } else if (methods[i].has_default) {
            if (!methods[i].has_default_slot) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                expand_error(err, span, "trait '%s.%s' has a default method without storage", trait, method_name);
                return NULL;
            }
            if (!method_impl_record(ctx, method_surface, type, methods[i].arity, trait_def->dispatch_env, trait_def->dispatch_env_key, methods[i].default_slot, err, span)) {
                idm_core_free(core);
                trait_impls_destroy(impls, impl_count);
                implement_dispatch_bindings_destroy(dispatch, method_count);
                return NULL;
            }
        }
    }
    for (size_t i = 0; i < method_count; i++) {
        IdmCore *dispatcher = method_dispatcher_fn(ctx, trait, &methods[i], span, err);
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
        if (!install_method_surface(ctx, trait, trait_key, trait_method_name_text(&methods[i]), methods[i].arity, method_scopes, ctx->unit, ctx->unit_key, true, dispatch[i].env, dispatch[i].env_key, dispatch[i].slot, err)) {
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

static IdmCore *protocol_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const char *name = name_syntax->as.text;
    unsigned char hash[32];
    char *identity = scoped_identity(ctx, name, form, hash, err);
    if (!identity) return NULL;
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    IdmScopeId scope_base = ctx->scope_store.next_scope;
    if (!compile_package_module(ctx, body, identity, hash, ctx->package_slots, ctx->package_slot_count, &art, err)) { free(identity); return NULL; }
    art.scope_base = scope_base;
    art.scope_end = ctx->scope_store.next_scope;
    memcpy(art.src_hash, hash, 32u);
    SurfaceCheckpoint install_checkpoint;
    surface_checkpoint(ctx, &install_checkpoint);
    uint32_t payload = 0;
    ProtocolDef *p = typed_registry_add_protocol(ctx, &payload, err, form->span);
    if (!p) {
        free(identity);
        idm_artifact_destroy(&art);
        return NULL;
    }
    p->name = idm_strdup(name);
    if (!protocol_def_set_identity(ctx, p, identity, err, form->span)) {
        surface_rollback(ctx, &install_checkpoint);
        idm_artifact_destroy(&art);
        free(identity);
        return NULL;
    }
    p->exported = exported && ctx->in_package;
    p->art = malloc(sizeof(*p->art));
    if (!p->art) {
        surface_rollback(ctx, &install_checkpoint);
        idm_artifact_destroy(&art);
        free(identity);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    *p->art = art;
    if (!p->name) {
        surface_rollback(ctx, &install_checkpoint);
        free(identity);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    IdmScopeSet protocol_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &protocol_scopes)) {
        surface_rollback(ctx, &install_checkpoint);
        free(identity);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    free(identity);
    if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&protocol_scopes);
        surface_rollback(ctx, &install_checkpoint);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    idm_scope_set_destroy(&protocol_scopes);
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        return NULL;
    }
    return cont;
}

IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    bool exported = false;
    size_t base = 1u;
    if (form->as.seq.count >= 2 && syn_is_word(fitems[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(fitems[base], "protocol") || fitems[base + 1u]->kind != IDM_SYN_WORD || !syn_is_form(fitems[base + 2u], "%-body"))
        return expand_error(err, form->span, "protocol expects 'protocol NAME do ... end'");
    return protocol_decl_core(ctx, form, fitems[base + 1u], fitems[base + 2u], exported, items, index, count, err);
}

IdmCore *expand_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    bool exported = false;
    size_t base = 1u;
    if (form->as.seq.count >= 2 && syn_is_word(fitems[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(fitems[base], "trait") || fitems[base + 1u]->kind != IDM_SYN_WORD || !syn_is_form(fitems[base + 2u], "%-body"))
        return expand_error(err, form->span, "trait expects 'trait NAME do ... end'");
    return trait_decl_core(ctx, form, fitems[base + 1u], fitems[base + 2u], exported, items, index, count, err);
}

static IdmCore *trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    unsigned char hash[32];
    char *identity = scoped_identity(ctx, name_syntax->as.text, form, hash, err);
    if (!identity) return NULL;

    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    const char *saved_trait_name = ctx->trait_name;
    const char *saved_trait_key = ctx->trait_key;
    ctx->trait_name = identity;
    ctx->trait_key = name_syntax->as.text;
    IdmTraitRequirementDef *requirements = NULL;
    size_t requirement_count = 0;
    size_t requirement_cap = 0;
    bool ok = true;
    for (size_t i = 1; ok && i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        IdmSyntax *required_name = require_form_trait_word(stmt, err);
        if (!required_name) {
            if (err && err->present && syn_is_form(stmt, "%-expr") && stmt->as.seq.count >= 2u && syn_is_word(stmt->as.seq.items[1], "require")) ok = false;
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
        } else if (trait_requirement_seen(requirements, requirement_count, trait_def_identity_text(required))) {
            expand_error(err, required_name->span, "trait '%s' requires trait '%s' more than once", identity, trait_def_identity_text(required));
            ok = false;
        } else {
            ok = trait_requirements_push(&requirements, &requirement_count, &requirement_cap, trait_def_identity_text(required), err, required_name->span) &&
                 install_required_trait_methods(ctx, required, required_name, err);
        }
        idm_syn_free(required_name);
    }
    if (ok) ok = predeclare_trait_methods(ctx, body->as.seq.items, 1u, body->as.seq.count, err);
    for (size_t i = 1; ok && i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (syn_is_form(stmt, "%-expr") && stmt->as.seq.count >= 2u && syn_is_word(stmt->as.seq.items[1], "require")) continue;
        const IdmSyntax *method_name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(stmt, &method_name, &param_start, &has_body, err)) {
            if (!(err && err->present)) expand_error(err, stmt->span, "trait bodies may contain only method declarations");
            ok = false;
            break;
        }
        IdmArity arity = idm_arity_unknown();
        if (!method_signature_arity(stmt, &arity, err)) { ok = false; break; }
        TraitMethodDef *method = find_decl_method(ctx, method_name->as.text);
        if (!method) { ok = false; idm_error_oom(err, method_name->span); break; }
        if (method->seen_decl) { expand_error(err, method_name->span, "trait '%s' declares method '%s' more than once", identity, method_name->as.text); ok = false; break; }
        if (!idm_arity_equal(&method->arity, &arity)) { expand_error(err, method_name->span, "method '%s.%s' arity changed during declaration", identity, method_name->as.text); ok = false; break; }
        method->seen_decl = true;
        if (has_body) {
            IdmCore *fn = expand_function_literal(ctx, method_name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
            if (!fn) { ok = false; break; }
            method->default_fn = fn;
            method->has_default = true;
            method->has_default_slot = true;
            method->default_slot = ctx->frame == IDM_FRAME_TOP ? ctx->env_slot_seq++ : ctx->next_slot++;
        }
    }

    size_t method_count = ok ? ctx->decl_method_count - checkpoint.decl_method_count : 0u;
    TraitMethodDef *methods = NULL;
    if (ok && !trait_method_defs_copy(&ctx->decl_methods[checkpoint.decl_method_count], method_count, &methods)) {
        ok = false;
        idm_error_oom(err, form->span);
    }

    IdmCore *init = NULL;
    if (ok) {
        init = idm_core_letrec(form->span);
        if (!init) {
            ok = false;
            idm_error_oom(err, form->span);
        } else if (ctx->frame == IDM_FRAME_TOP) {
            idm_core_letrec_set_env(init);
        }
    }
    for (size_t i = 0; ok && i < method_count; i++) {
        TraitMethodDef *method = &ctx->decl_methods[checkpoint.decl_method_count + i];
        IdmCore *empty_dispatch = method_empty_dispatcher_fn(ctx, identity, method, form->span, err);
        if (!empty_dispatch || !idm_core_letrec_add(init, trait_method_name_text(method), method->dispatch_slot, empty_dispatch)) {
            idm_core_free(empty_dispatch);
            ok = false;
            idm_error_oom(err, form->span);
            break;
        }
        IdmCore *default_fn = method->default_fn;
        method->default_fn = NULL;
        if (default_fn) {
            if (!idm_core_letrec_add(init, trait_method_name_text(method), method->default_slot, default_fn)) {
                idm_core_free(default_fn);
                ok = false;
                idm_error_oom(err, form->span);
                break;
            }
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
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        idm_core_free(init);
        if (methods) { for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]); free(methods); }
        trait_requirements_destroy(requirements, requirement_count);
        free(identity);
        return NULL;
    }

    SurfaceCheckpoint install_checkpoint;
    surface_checkpoint(ctx, &install_checkpoint);
    uint32_t payload = 0;
    TraitDef *trait = typed_registry_add_trait(ctx, &payload, err, form->span);
    if (!trait) {
        idm_core_free(init);
        for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]);
        free(methods);
        trait_requirements_destroy(requirements, requirement_count);
        free(identity);
        return NULL;
    }
    if (!trait_def_set_identity(ctx, trait, identity, err, form->span)) {
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        for (size_t i = 0; i < method_count; i++) trait_method_def_destroy(&methods[i]);
        free(methods);
        trait_requirements_destroy(requirements, requirement_count);
        free(identity);
        return NULL;
    }
    const char *trait_identity = trait_def_identity_text(trait);
    trait->exported = exported && ctx->in_package;
    (void)hash;
    trait->requirements = requirements;
    trait->requirement_count = requirement_count;
    trait->methods = methods;
    trait->method_count = method_count;
    trait->dispatch_env = ctx->frame == IDM_FRAME_TOP;
    IdmScopeSet trait_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &trait_scopes)) {
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        free(identity);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (!idm_binding_table_add(&ctx->bindings, name_syntax->as.text, IDM_PHASE_ANY, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, &trait_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&trait_scopes);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(init);
        free(identity);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!install_method_surface(ctx, trait_identity, name_syntax->as.text, trait_method_name_text(&methods[i]), methods[i].arity, &trait_scopes, ctx->unit, ctx->unit_key, methods[i].has_dispatch, ctx->frame == IDM_FRAME_TOP, NULL, methods[i].dispatch_slot, err)) {
            idm_scope_set_destroy(&trait_scopes);
            surface_rollback(ctx, &install_checkpoint);
            idm_core_free(init);
            free(identity);
            return NULL;
        }
    }
    idm_scope_set_destroy(&trait_scopes);
    free(identity);

    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        idm_core_free(init);
        return NULL;
    }
    return sequence_two(init, cont, form->span, err);
}

static IdmCore *expand_protocol_runtime(ExpandContext *ctx, ProtocolDef *p, IdmScopeId base, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err) {
    const IdmArtifact *art = p->art;
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    if (!install_artifact_runtime_slots(ctx, art, NULL, min_id, delta, true, span, err)) {
        return NULL;
    }
    bool init_pending = artifact_init_pending(ctx, art);
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) return NULL;
    if (!init_pending) return cont;
    return wrap_artifact_runtime_init(ctx, art, min_id, delta, false, false, cont, span, err);
}

IdmCore *expand_activate(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err) {
    const char *name = name_syntax->as.text;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *p = resolve_protocol_def(ctx, name_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, name_syntax->span, "ambiguous protocol '%s'", name);
    if (!p) return expand_error(err, name_syntax->span, "activate expects a protocol; '%s' is unbound", name);
    IdmScopeSet act_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &act_scopes)) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    IdmScopeId base = 0;
    SurfaceCheckpoint activation_checkpoint;
    surface_checkpoint(ctx, &activation_checkpoint);
    bool activated = artifact_base(ctx, p->art, &base, err);
    char runtime_key[17];
    artifact_provider_key(p->art->src_hash, runtime_key);
    activated = activated &&
                activate_artifact(ctx, protocol_def_identity_text(p), p->art, base, &act_scopes, span, err) &&
                record_activation(ctx, name, protocol_def_identity_text(p), runtime_key, span, err);
    if (!activated) {
        surface_rollback(ctx, &activation_checkpoint);
        ctx->scope_store.next_scope = activation_checkpoint.next_scope;
    }
    idm_scope_set_destroy(&act_scopes);
    if (!activated) {
        if (err && err->present && err->span.line == 0) err->span = span;
        if (err && err->present && span.line != 0) idm_error_note(err, "while activating '%s' (%s:%u:%u)", name, span.file ? span.file : "<unknown>", span.line, span.column);
        return NULL;
    }
    return expand_protocol_runtime(ctx, p, base, items, index, count, span, err);
}

static IdmCore *sequence_two(IdmCore *first, IdmCore *second, IdmSpan span, IdmError *err) {
    IdmCore *seq = idm_core_do(span);
    if (!seq || !idm_core_do_add(seq, first) || !idm_core_do_add(seq, second)) {
        idm_core_free(seq);
        idm_core_free(first);
        idm_core_free(second);
        idm_error_oom(err, span);
        return NULL;
    }
    return seq;
}

static IdmCore *implement_trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *trait_syntax, const IdmSyntax *type_syntax, const IdmSyntax *body, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const char *trait_name = trait_syntax->as.text;
    const char *type_name = type_syntax->as.text;
    IdmResolveStatus type_status = IDM_RESOLVE_UNBOUND;
    TypeDef *type_def = resolve_type_def(ctx, type_syntax, &type_status);
    if (type_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, type_syntax->span, "ambiguous type '%s'", type_name);
    if (type_def) type_name = type_def_identity_text(type_def);

    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    TraitDef *trait = resolve_trait_def(ctx, trait_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, trait_syntax->span, "ambiguous trait '%s'", trait_name);
    if (!trait) return expand_error(err, trait_syntax->span, "unbound trait '%s'", trait_name);

    IdmScopeSet method_scopes;
    if (!binder_scopes_pruned(ctx, trait_syntax, &method_scopes)) {
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    IdmCore *implement_core = build_trait_implement_core(ctx, trait, trait_syntax->as.text, type_name, &method_scopes, body, form->span, err);
    idm_scope_set_destroy(&method_scopes);
    if (!implement_core) return NULL;
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) { idm_core_free(implement_core); return NULL; }
    return sequence_two(implement_core, cont, form->span, err);
}

IdmCore *expand_implement_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    if (!syn_is_form(form, "%-expr") || form->as.seq.count < 5u || !syn_is_word(form->as.seq.items[1], "implement")) {
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
    const IdmSyntax *body = NULL;
    if (pos < form->as.seq.count) {
        if (pos + 1u != form->as.seq.count || !syn_is_form(form->as.seq.items[pos], "%-body")) {
            idm_syn_free(type);
            idm_syn_free(trait);
            return expand_error(err, form->span, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'");
        }
        body = form->as.seq.items[pos];
    }
    IdmCore *core = implement_trait_decl_core(ctx, form, trait, type, body, items, index, count, err);
    idm_syn_free(type);
    idm_syn_free(trait);
    return core;
}

static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported) {
    if (!syn_is_form(form, "%-expr")) return false;
    size_t base = 1u;
    bool exported = false;
    if (form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(form->as.seq.items[base], "record") || form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD || !syn_is_form(form->as.seq.items[base + 2u], "%-body")) return false;
    *out_name = form->as.seq.items[base + 1u];
    *out_body = form->as.seq.items[base + 2u];
    if (out_exported) *out_exported = exported;
    return true;
}

static void record_fields_destroy(RecordFieldDecl *fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(fields[i].name);
        idm_syn_free(fields[i].contract_syntax);
    }
    free(fields);
}

static bool record_field_seen(const RecordFieldDecl *fields, size_t count, const char *name) {
    return find_named((void *)fields, count, sizeof(*fields), offsetof(RecordFieldDecl, name), name) != NULL;
}

static bool parse_record_fields(const IdmSyntax *body, RecordFieldDecl **out_fields, size_t *out_count, IdmError *err) {
    RecordFieldDecl *fields = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (!syn_is_form(stmt, "%-expr") || stmt->as.seq.count < 3u || !syn_is_word(stmt->as.seq.items[1], "field") || stmt->as.seq.items[2]->kind != IDM_SYN_WORD) {
            record_fields_destroy(fields, count);
            return idm_error_set(err, stmt->span, "record body expects 'field NAME' or 'field NAME : TYPE' declarations");
        }
        IdmSyntax *contract = NULL;
        if (stmt->as.seq.count != 3u) {
            if (stmt->as.seq.count < 5u || !syn_is_word(stmt->as.seq.items[3], ":")) {
                record_fields_destroy(fields, count);
                return idm_error_set(err, stmt->span, "record body expects 'field NAME' or 'field NAME : TYPE' declarations");
            }
            size_t contract_end = stmt->as.seq.count;
            contract = expect_qualified_word_at(stmt->as.seq.items, 4u, &contract_end, "record field contract expects a type name", err);
            if (!contract) {
                record_fields_destroy(fields, count);
                return false;
            }
            if (contract_end != stmt->as.seq.count) {
                idm_syn_free(contract);
                record_fields_destroy(fields, count);
                return idm_error_set(err, stmt->as.seq.items[contract_end]->span, "record field contract expects one type name");
            }
        }
        const char *name = stmt->as.seq.items[2]->as.text;
        if (record_field_seen(fields, count, name)) {
            idm_syn_free(contract);
            record_fields_destroy(fields, count);
            return idm_error_set(err, stmt->as.seq.items[2]->span, "record field '%s' is declared more than once", name);
        }
        if (count == cap) {
            if (!idm_grow((void **)&fields, &cap, sizeof(*fields), 4u, count + 1u)) {
                idm_syn_free(contract);
                record_fields_destroy(fields, count);
                return idm_error_oom(err, stmt->span);
            }
        }
        memset(&fields[count], 0, sizeof(fields[count]));
        fields[count].name = idm_strdup(name);
        fields[count].contract_syntax = contract;
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

static bool resolve_record_field_contracts(ExpandContext *ctx, RecordFieldDecl *fields, size_t field_count, const char *record_name, const char *identity, IdmError *err) {
    for (size_t i = 0; i < field_count; i++) {
        fields[i].name_symbol = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, fields[i].name);
        if (!fields[i].name_symbol) return idm_error_oom(err, idm_span_unknown(NULL));
        IdmSyntax *contract = fields[i].contract_syntax;
        if (!contract) continue;
        const char *name = contract->as.text;
        if (strcmp(name, "_") == 0) continue;
        IdmSymbol *resolved = NULL;
        IdmSymbol *contract_symbol = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, name);
        if (!contract_symbol) return idm_error_oom(err, contract->span);
        if (idm_value_builtin_type_symbol(contract_symbol)) {
            resolved = contract_symbol;
        } else if (strcmp(name, record_name) == 0) {
            resolved = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, identity);
            if (!resolved) return idm_error_oom(err, contract->span);
        } else {
            IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
            TypeDef *type = resolve_type_def(ctx, contract, &status);
            if (status == IDM_RESOLVE_AMBIGUOUS) {
                expand_error(err, contract->span, "ambiguous type '%s'", name);
                return false;
            }
            if (!type) {
                expand_error(err, contract->span, "unbound type '%s'", name);
                return false;
            }
            resolved = type_def_identity_symbol(type);
        }
        fields[i].contract_symbol = resolved;
    }
    return true;
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
        if (!arg || !idm_core_record_construct_add(record, field->name, field->contract, arg)) {
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

static char *record_predicate_name(const char *record_name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_buf_append(&buf, record_name) || !idm_buf_append_char(&buf, '?')) { idm_buf_destroy(&buf); return NULL; }
    return idm_buf_take(&buf);
}

static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, bool exported, const RecordFieldDecl *fields, size_t field_count, TypeDef **out_type, IdmSpan span, IdmError *err) {
    if (out_type) *out_type = NULL;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    uint32_t payload = 0;
    TypeDef *type = typed_registry_add_type(ctx, &payload, err, span);
    if (!type) { free(identity); return false; }
    type->name = idm_strdup(record_name);
    if (!type_def_set_identity(ctx, type, identity, err, span)) { free(identity); surface_rollback(ctx, &checkpoint); return false; }
    free(identity);
    identity = NULL;
    type->exported = exported;
    if (!type->name) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
    if (field_count != 0) {
        type->fields = calloc(field_count, sizeof(*type->fields));
        if (!type->fields) { surface_rollback(ctx, &checkpoint); return idm_error_oom(err, span); }
        type->field_count = field_count;
        for (size_t i = 0; i < field_count; i++) {
            if (!fields[i].name_symbol) { surface_rollback(ctx, &checkpoint); return idm_error_set(err, span, "record field metadata missing symbol"); }
            type->fields[i].name = fields[i].name_symbol;
            type->fields[i].contract = fields[i].contract_symbol;
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
    if (out_type) *out_type = type;
    return true;
}

IdmCore *expand_record_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const IdmSyntax *name_syntax = NULL;
    const IdmSyntax *body = NULL;
    bool exported = false;
    if (!record_form_parts(form, &name_syntax, &body, &exported)) return expand_error(err, form->span, "record expects 'record NAME do field ... end'");
    const char *record_name = name_syntax->as.text;
    RecordFieldDecl *fields = NULL;
    size_t field_count = 0;
    if (!parse_record_fields(body, &fields, &field_count, err)) return NULL;
    if (field_count > UINT32_MAX) {
        record_fields_destroy(fields, field_count);
        return expand_error(err, name_syntax->span, "record has too many fields");
    }
    unsigned char hash[32];
    char *predicate_name = record_predicate_name(record_name);
    char *owned_identity = predicate_name ? scoped_identity(ctx, record_name, form, hash, err) : NULL;
    if (!predicate_name || !owned_identity) {
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        if (err && !err->present) idm_error_oom(err, name_syntax->span);
        return NULL;
    }
    const char *identity = owned_identity;

    if (!resolve_record_field_contracts(ctx, fields, field_count, record_name, identity, err)) {
        free(predicate_name);
        free(owned_identity);
        record_fields_destroy(fields, field_count);
        return NULL;
    }

    TypeDef *record_type = NULL;
    if (!register_record_type_surface(ctx, name_syntax, record_name, owned_identity, exported && ctx->in_package, fields, field_count, &record_type, form->span, err)) {
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    owned_identity = NULL;

    bool top_level = ctx->frame == IDM_FRAME_TOP;
    size_t saved_count = ctx->bindings.count;
    uint32_t saved_next = ctx->next_slot;
    uint32_t constructor_slot = 0;
    uint32_t predicate_slot = 0;
    IdmArity constructor_arity = idm_arity_exact((uint32_t)field_count);
    IdmArity predicate_arity = idm_arity_exact(1u);
    bool ok = top_level ? env_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_slot) : local_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_slot);
    IdmSyntax *predicate_syntax = NULL;
    if (ok) {
        predicate_syntax = idm_syn_word(predicate_name, name_syntax->span);
        if (!predicate_syntax) ok = false;
    }
    if (ok) {
        const IdmScopeSet *scopes = idm_syn_scope_set(name_syntax, 0);
        if (scopes) for (size_t i = 0; i < scopes->count && ok; i++) ok = idm_syn_scope_add(predicate_syntax, 0, scopes->items[i]);
    }
    if (ok) ok = top_level ? env_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_slot) : local_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_slot);
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (top_level && ctx->in_package) {
        IdmScopeSet record_scopes;
        idm_scope_set_init(&record_scopes);
        if (!syntax_scopes_copy(&record_scopes, name_syntax)) ok = false;
        if (ok && !record_package_slot(ctx, record_name, constructor_slot, &record_scopes, constructor_arity, exported)) ok = false;
        if (ok && !record_package_slot(ctx, predicate_name, predicate_slot, &record_scopes, predicate_arity, exported)) ok = false;
        idm_scope_set_destroy(&record_scopes);
    }
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
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
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
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
        free(predicate_name);
        record_fields_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    idm_syn_free(predicate_syntax);
    free(predicate_name);
    record_fields_destroy(fields, field_count);
    return letrec;
}
