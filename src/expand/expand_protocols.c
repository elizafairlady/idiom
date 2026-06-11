#include "internal.h"

static bool method_form_parts(const IshSyntax *form, const IshSyntax **out_name, size_t *out_param_start, bool *out_has_body, IshError *err);
static bool method_signature_arity(const IshSyntax *form, uint32_t *out_arity, IshError *err);
static IshProtocolMethodDef *find_decl_method(ExpandContext *ctx, const char *name);
static bool add_decl_method(ExpandContext *ctx, const IshSyntax *name_syntax, uint32_t arity, IshError *err);
static void extend_impls_destroy(ExtendMethodImpl *impls, size_t count);
static const IshProtocolMethodDef *find_protocol_method_def(const IshProtocolMethodDef *methods, size_t count, const char *name);
static bool extend_has_impl(const ExtendMethodImpl *impls, size_t count, const char *name);
static bool extend_impls_push(ExtendMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IshCore *fn, IshError *err, IshSpan span);
static IshCore *build_extend_core(ExpandContext *ctx, const char *protocol, const char *type, const IshProtocolMethodDef *methods, size_t method_count, const IshSyntax *body, IshError *err);
static bool activate_surface(ExpandContext *ctx, const char *protocol_name, const IshOperatorDef *operators, size_t op_count, const IshPkgMacro *macros, size_t macro_count, const IshProtocolMethodDef *methods, size_t method_count, IshModuleRef *resolver_module, uint32_t resolver_fn, IshNamespace *resolver_phase_ns, IshPhaseEnv *resolver_phase_env, IshSpan span, IshError *err);
static IshCore *sequence_two(IshCore *first, IshCore *second, IshSpan span, IshError *err);
static bool record_form_parts(const IshSyntax *form, const IshSyntax **out_name, const IshSyntax **out_body, bool *out_exported);
static void record_field_names_destroy(char **fields, size_t count);
static bool record_field_seen(char **fields, size_t count, const char *name);
static bool parse_record_fields(const IshSyntax *body, char ***out_fields, size_t *out_count, IshError *err);
static IshCore *make_prim_app(IshPrimitive primitive, IshSpan span, IshError *err);
static bool core_app_add_or_oom(IshCore *app, IshCore *arg, IshError *err, IshSpan span);
static IshCore *record_field_default_fn(ExpandContext *ctx, const char *field, IshSpan span, IshError *err);
static IshCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, char **fields, size_t field_count, IshSpan span, IshError *err);
static IshCore *record_predicate_fn(ExpandContext *ctx, const char *record_name, const char *predicate_name, IshSpan span, IshError *err);
static char *record_predicate_name(const char *record_name);
static bool register_record_protocol_surface(ExpandContext *ctx, const IshSyntax *name_syntax, const char *record_name, char **fields, size_t field_count, IshSpan span, IshCore **out_define, IshCore **out_extend, IshError *err);

static bool method_form_parts(const IshSyntax *form, const IshSyntax **out_name, size_t *out_param_start, bool *out_has_body, IshError *err) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 3 || !syn_is_word(form->as.seq.items[1], "method") || form->as.seq.items[2]->kind != ISH_SYN_WORD) return false;
    bool has_body = false;
    for (size_t i = 3; i < form->as.seq.count; i++) {
        const IshSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || syn_is_protocol(item, "%-body")) {
            has_body = true;
            break;
        }
        if (item->kind != ISH_SYN_WORD) {
            if (err) ish_error_set(err, item->span, "method parameter must be an identifier");
            return false;
        }
    }
    *out_name = form->as.seq.items[2];
    *out_param_start = 3u;
    if (out_has_body) *out_has_body = has_body;
    return true;
}

static bool method_signature_arity(const IshSyntax *form, uint32_t *out_arity, IshError *err) {
    const IshSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return false;
    (void)name;
    size_t arity = 0;
    for (size_t i = param_start; i < form->as.seq.count; i++) {
        const IshSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || syn_is_protocol(item, "%-body")) break;
        arity++;
    }
    if (arity > UINT32_MAX) return ish_error_set(err, form->span, "method arity is too large");
    *out_arity = (uint32_t)arity;
    return true;
}

static IshProtocolMethodDef *find_decl_method(ExpandContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->decl_method_count; i++) if (strcmp(ctx->decl_methods[i].name, name) == 0) return &ctx->decl_methods[i];
    return NULL;
}

static bool add_decl_method(ExpandContext *ctx, const IshSyntax *name_syntax, uint32_t arity, IshError *err) {
    if (find_decl_method(ctx, name_syntax->as.text)) return ish_error_set(err, name_syntax->span, "protocol '%s' declares method '%s' more than once", ctx->protocol_name ? ctx->protocol_name : "<anonymous>", name_syntax->as.text);
    if (ctx->decl_method_count == ctx->decl_method_cap) {
        size_t cap = ctx->decl_method_cap ? ctx->decl_method_cap * 2u : 4u;
        IshProtocolMethodDef *next = realloc(ctx->decl_methods, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, name_syntax->span);
        ctx->decl_methods = next;
        ctx->decl_method_cap = cap;
    }
    IshProtocolMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
    memset(method, 0, sizeof(*method));
    method->name = ish_strdup(name_syntax->as.text);
    method->arity = arity;
    method->exported = true;
    if (!method->name || !syntax_scopes_copy(&method->scopes, name_syntax)) {
        ish_protocol_method_def_destroy(method);
        return ish_error_oom(err, name_syntax->span);
    }
    ctx->decl_method_count++;
    return install_method_surface(ctx, ctx->protocol_name ? ctx->protocol_name : "<anonymous>", method->name, arity, &method->scopes, err);
}

bool predeclare_protocol_methods(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshError *err) {
    if (!ctx->protocol_name) return true;
    for (size_t i = start; i < count; i++) {
        const IshSyntax *name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(items[i], &name, &param_start, &has_body, NULL)) continue;
        (void)param_start;
        (void)has_body;
        uint32_t arity = 0;
        if (!method_signature_arity(items[i], &arity, err)) return false;
        if (!add_decl_method(ctx, name, arity, err)) return false;
    }
    return true;
}

IshCore *expand_method_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err) {
    if (!ctx->protocol_name) return expand_error(err, form->span, "method declarations are only valid inside protocol bodies");
    const IshSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return NULL;
    uint32_t arity = 0;
    if (!method_signature_arity(form, &arity, err)) return NULL;
    IshProtocolMethodDef *method = find_decl_method(ctx, name->as.text);
    if (!method) {
        if (!add_decl_method(ctx, name, arity, err)) return NULL;
        method = find_decl_method(ctx, name->as.text);
    }
    if (!method) return (IshCore *)(uintptr_t)ish_error_oom(err, name->span);
    if (method->seen_decl) return expand_error(err, name->span, "protocol '%s' declares method '%s' more than once", ctx->protocol_name, name->as.text);
    if (method->arity != arity) return expand_error(err, name->span, "method '%s.%s' arity changed during declaration", ctx->protocol_name, name->as.text);
    method->seen_decl = true;
    if (has_body) {
        IshCore *fn = expand_function_literal(ctx, name->as.text, form->as.seq.items[1], form->as.seq.items, param_start, form->as.seq.count, err);
        if (!fn) return NULL;
        method->default_fn = fn;
        method->has_default = true;
    }
    return expand_body_items(ctx, items, index + 1u, count, err);
}

static void extend_impls_destroy(ExtendMethodImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(impls[i].name);
        ish_core_free(impls[i].fn);
    }
    free(impls);
}

static const IshProtocolMethodDef *find_protocol_method_def(const IshProtocolMethodDef *methods, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(methods[i].name, name) == 0) return &methods[i];
    return NULL;
}

static bool extend_has_impl(const ExtendMethodImpl *impls, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(impls[i].name, name) == 0) return true;
    return false;
}

static bool extend_impls_push(ExtendMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IshCore *fn, IshError *err, IshSpan span) {
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        ExtendMethodImpl *next = realloc(*impls, next_cap * sizeof(*next));
        if (!next) return ish_error_oom(err, span);
        *impls = next;
        *cap = next_cap;
    }
    ExtendMethodImpl *impl = &(*impls)[(*count)++];
    impl->name = ish_strdup(name);
    impl->arity = arity;
    impl->fn = fn;
    if (!impl->name) {
        (*count)--;
        return ish_error_oom(err, span);
    }
    return true;
}

static IshCore *build_extend_core(ExpandContext *ctx, const char *protocol, const char *type, const IshProtocolMethodDef *methods, size_t method_count, const IshSyntax *body, IshError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool surface_ok = true;
    for (size_t i = 0; i < method_count && surface_ok; i++) surface_ok = install_method_surface(ctx, protocol, methods[i].name, methods[i].arity, &ctx->empty_scopes, err);
    if (!surface_ok) {
        surface_rollback(ctx, &checkpoint);
        return NULL;
    }

    ExtendMethodImpl *impls = NULL;
    size_t impl_count = 0;
    size_t impl_cap = 0;
    bool ok = true;
    for (size_t i = 1; i < body->as.seq.count && ok; i++) {
        const IshSyntax *stmt = body->as.seq.items[i];
        const IshSyntax *name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(stmt, &name, &param_start, &has_body, err)) {
            if (!(err && err->present)) expand_error(err, stmt->span, "extend bodies may contain only method implementations");
            ok = false;
            break;
        }
        if (!has_body) { expand_error(err, stmt->span, "extend method '%s' requires an implementation body", name->as.text); ok = false; break; }
        uint32_t arity = 0;
        if (!method_signature_arity(stmt, &arity, err)) { ok = false; break; }
        const IshProtocolMethodDef *contract = find_protocol_method_def(methods, method_count, name->as.text);
        if (!contract) { expand_error(err, name->span, "protocol '%s' has no method '%s'", protocol, name->as.text); ok = false; break; }
        if (contract->arity != arity) { expand_error(err, name->span, "method '%s.%s' expects %u argument(s), got %u", protocol, name->as.text, contract->arity, arity); ok = false; break; }
        if (extend_has_impl(impls, impl_count, name->as.text)) { expand_error(err, name->span, "extend for '%s' implements method '%s' more than once", protocol, name->as.text); ok = false; break; }
        IshCore *fn = expand_function_literal(ctx, name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
        if (!fn) { ok = false; break; }
        if (!extend_impls_push(&impls, &impl_count, &impl_cap, name->as.text, arity, fn, err, name->span)) {
            ish_core_free(fn);
            ok = false;
            break;
        }
    }
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        extend_impls_destroy(impls, impl_count);
        return NULL;
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!methods[i].has_default && !extend_has_impl(impls, impl_count, methods[i].name)) {
            expand_error(err, body->span, "extend for '%s' on type '%s' is missing required method '%s'", protocol, type, methods[i].name);
            extend_impls_destroy(impls, impl_count);
            return NULL;
        }
    }
    IshCore *core = ish_core_extend_protocol(ish_atom(ctx->rt, protocol), ish_atom(ctx->rt, type), body->span);
    if (!core) {
        extend_impls_destroy(impls, impl_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, body->span);
    }
    for (size_t i = 0; i < impl_count; i++) {
        IshCore *fn = impls[i].fn;
        impls[i].fn = NULL;
        if (!ish_core_extend_protocol_add_impl(core, ish_atom(ctx->rt, impls[i].name), impls[i].arity, fn)) {
            ish_core_free(fn);
            ish_core_free(core);
            extend_impls_destroy(impls, impl_count);
            return (IshCore *)(uintptr_t)ish_error_oom(err, body->span);
        }
    }
    extend_impls_destroy(impls, impl_count);
    return core;
}

IshCore *expand_protocol_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err) {
    IshSyntax *const *fitems = form->as.seq.items;
    if (form->as.seq.count != 4 || fitems[2]->kind != ISH_SYN_WORD || !syn_is_protocol(fitems[3], "%-body"))
        return expand_error(err, form->span, "protocol expects 'protocol NAME do ... end'");
    const char *name = fitems[2]->as.text;
    IshArtifact art;
    memset(&art, 0, sizeof(art));
    if (!compile_package_module(ctx, fitems[3], name, &art, err)) return NULL;
    if (ctx->protocol_count == ctx->protocol_cap) {
        size_t cap = ctx->protocol_cap ? ctx->protocol_cap * 2u : 4u;
        ProtocolDef *next = realloc(ctx->protocols, cap * sizeof(*next));
        if (!next) { ish_artifact_destroy(&art); return (IshCore *)(uintptr_t)ish_error_oom(err, form->span); }
        ctx->protocols = next;
        ctx->protocol_cap = cap;
    }
    ProtocolDef *p = &ctx->protocols[ctx->protocol_count];
    memset(p, 0, sizeof(*p));
    p->name = ish_strdup(name);
    if (!p->name) { ish_artifact_destroy(&art); return (IshCore *)(uintptr_t)ish_error_oom(err, form->span); }
    p->operators = art.operators;
    p->operator_count = art.operator_count;
    p->macros = art.macros;
    p->macro_count = art.macro_count;
    p->methods = art.methods;
    p->method_count = art.method_count;
    if (art.resolver_module) {
        p->resolver_module = art.resolver_module;
        p->resolver_fn = art.resolver_fn;
        p->resolver_phase_ns = art.resolver_phase_ns;
        p->resolver_phase_env = ish_phase_env_retain(art.resolver_phase_env);
        art.resolver_module = NULL;
        art.resolver_phase_ns = NULL;
        ish_phase_env_release(art.resolver_phase_env);
        art.resolver_phase_env = NULL;
    }
    art.operators = NULL;
    art.operator_count = 0;
    art.macros = NULL;
    art.macro_count = 0;
    art.methods = NULL;
    art.method_count = 0;
    IshScopeSet protocol_scopes;
    if (!syntax_scopes_copy(&protocol_scopes, fitems[2])) {
        protocol_def_destroy(p);
        ish_artifact_destroy(&art);
        return (IshCore *)(uintptr_t)ish_error_oom(err, fitems[2]->span);
    }
    uint32_t payload = (uint32_t)ctx->protocol_count;
    if (!ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_PROTOCOL, ISH_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        ish_scope_set_destroy(&protocol_scopes);
        protocol_def_destroy(p);
        ish_artifact_destroy(&art);
        return (IshCore *)(uintptr_t)ish_error_oom(err, fitems[2]->span);
    }
    ish_scope_set_destroy(&protocol_scopes);
    ctx->protocol_count++;
    IshBytecodeModule *module = art.module;
    uint32_t init_fn = art.init_fn;
    art.module = NULL;
    IshCore *cont = expand_body_items(ctx, items, index + 1u, count, err);
    if (!cont) {
        if (module) { ish_bc_destroy(module); free(module); }
        ish_artifact_destroy(&art);
        return NULL;
    }
    IshCore *runtime = ish_core_use_package(ish_atom(ctx->rt, name), module, init_fn, NULL, NULL, 0u, cont, form->span);
    if (!runtime) {
        if (module) { ish_bc_destroy(module); free(module); }
        ish_core_free(cont);
        ish_artifact_destroy(&art);
        return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
    }
    ish_artifact_destroy(&art);
    return runtime;
}

static bool activate_surface(ExpandContext *ctx, const char *protocol_name, const IshOperatorDef *operators, size_t op_count, const IshPkgMacro *macros, size_t macro_count, const IshProtocolMethodDef *methods, size_t method_count, IshModuleRef *resolver_module, uint32_t resolver_fn, IshNamespace *resolver_phase_ns, IshPhaseEnv *resolver_phase_env, IshSpan span, IshError *err) {
    (void)span;
    for (size_t i = 0; i < op_count; i++) {
        if (!install_imported_operator(ctx, &operators[i], err)) return false;
    }
    for (size_t i = 0; i < macro_count; i++) {
        if (!install_imported_macro(ctx, macros[i].name, &ctx->empty_scopes, macros[i].module, macros[i].function_index, macros[i].phase_ns, macros[i].phase_env, err)) return false;
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!install_method_surface(ctx, protocol_name, methods[i].name, methods[i].arity, &ctx->empty_scopes, err)) return false;
    }
    if (resolver_module) {
        if (!install_imported_macro(ctx, "%resolver", &ctx->empty_scopes, resolver_module, resolver_fn, resolver_phase_ns ? resolver_phase_ns : ctx->phase_ns, resolver_phase_env ? resolver_phase_env : ctx->phase_env, err)) return false;
    }
    return true;
}

IshCore *expand_implements(ExpandContext *ctx, const IshSyntax *name_syntax, IshSyntax *const *items, size_t index, size_t count, IshSpan span, IshError *err) {
    const char *name = name_syntax->as.text;
    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    ProtocolDef *p = resolve_protocol_def(ctx, name_syntax, &status);
    if (status == ISH_RESOLVE_AMBIGUOUS) return expand_error(err, name_syntax->span, "ambiguous protocol '%s'", name);
    if (p) {
        if (!activate_surface(ctx, p->name, p->operators, p->operator_count, p->macros, p->macro_count, p->methods, p->method_count, p->resolver_module, p->resolver_fn, p->resolver_phase_ns, p->resolver_phase_env, span, err)) return NULL;
        if (!record_activation(ctx, name, span, err)) return NULL;
        return expand_body_items(ctx, items, index + 1u, count, err);
    }
    const IshArtifact *art = artifact_get(ctx, name, span, err);
    if (!art) {
        if (err && err->present && err->message && strcmp(err->message, "package source must be a program") == 0) {
            ish_error_set(err, span, "implements: '%s' is not a protocol", name);
        }
        return NULL;
    }
    IshScopeId base = 0;
    if (!artifact_base(ctx, art, &base, err)) return NULL;
    if (!activate_artifact(ctx, name, art, base, span, err)) return NULL;
    if (!record_activation(ctx, name, span, err)) return NULL;
    IshCore *cont = expand_body_items(ctx, items, index + 1u, count, err);
    if (!cont) return NULL;
    uint32_t fn_off = 0;
    IshBytecodeModule *module = relocated_module_copy(ctx, art->module, art->scope_base, (int64_t)base - (int64_t)art->scope_base, &fn_off, err);
    if (!module) {
        ish_core_free(cont);
        return NULL;
    }
    IshCore *runtime = ish_core_use_package(ish_atom(ctx->rt, art->name ? art->name : name), module, art->init_fn + fn_off, NULL, NULL, 0u, cont, span);
    if (!runtime) {
        ish_bc_destroy(module);
        free(module);
        ish_core_free(cont);
        return (IshCore *)(uintptr_t)ish_error_oom(err, span);
    }
    return runtime;
}

static IshCore *sequence_two(IshCore *first, IshCore *second, IshSpan span, IshError *err) {
    IshCore *seq = ish_core_do(span);
    if (!seq || !ish_core_do_add(seq, first) || !ish_core_do_add(seq, second)) {
        ish_core_free(seq);
        ish_core_free(first);
        ish_core_free(second);
        ish_error_oom(err, span);
        return NULL;
    }
    return seq;
}

IshCore *expand_extend_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err) {
    if (form->as.seq.count != 6 || form->as.seq.items[2]->kind != ISH_SYN_WORD || !syn_is_word(form->as.seq.items[3], "with") || form->as.seq.items[4]->kind != ISH_SYN_WORD || !syn_is_protocol(form->as.seq.items[5], "%-body")) {
        return expand_error(err, form->span, "extend expects 'extend TYPE with PROTOCOL do ... end'");
    }
    const char *type_name = form->as.seq.items[2]->as.text;
    const char *protocol_name = form->as.seq.items[4]->as.text;
    const IshProtocolMethodDef *methods = NULL;
    size_t method_count = 0;
    const IshArtifact *ext = NULL;
    IshScopeId ext_base = 0;

    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    ProtocolDef *local = resolve_protocol_def(ctx, form->as.seq.items[4], &status);
    if (status == ISH_RESOLVE_AMBIGUOUS) return expand_error(err, form->as.seq.items[4]->span, "ambiguous protocol '%s'", protocol_name);
    if (local) {
        protocol_name = local->name;
        methods = local->methods;
        method_count = local->method_count;
    } else {
        ext = artifact_get(ctx, protocol_name, form->as.seq.items[4]->span, err);
        if (!ext) {
            if (err && err->present && err->message && strcmp(err->message, "package source must be a program") == 0) {
                ish_error_set(err, form->as.seq.items[4]->span, "extend: '%s' is not a protocol", protocol_name);
            }
            return NULL;
        }
        if (!artifact_base(ctx, ext, &ext_base, err)) return NULL;
        methods = ext->methods;
        method_count = ext->method_count;
    }
    if (method_count == 0) {
        return expand_error(err, form->as.seq.items[4]->span, "protocol '%s' has no methods to extend", protocol_name);
    }

    IshCore *extend_core = build_extend_core(ctx, protocol_name, type_name, methods, method_count, form->as.seq.items[5], err);
    if (!extend_core) return NULL;
    IshCore *cont = expand_body_items(ctx, items, index + 1u, count, err);
    if (!cont) { ish_core_free(extend_core); return NULL; }
    IshCore *seq = sequence_two(extend_core, cont, form->span, err);
    if (!seq) return NULL;
    if (ext) {
        uint32_t fn_off = 0;
        IshBytecodeModule *module = relocated_module_copy(ctx, ext->module, ext->scope_base, (int64_t)ext_base - (int64_t)ext->scope_base, &fn_off, err);
        if (!module) {
            ish_core_free(seq);
            return NULL;
        }
        IshCore *runtime = ish_core_use_package(ish_atom(ctx->rt, ext->name ? ext->name : protocol_name), module, ext->init_fn + fn_off, NULL, NULL, 0u, seq, form->span);
        if (!runtime) {
            ish_bc_destroy(module);
            free(module);
            ish_core_free(seq);
            return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
        }
        return runtime;
    }
    return seq;
}

static bool record_form_parts(const IshSyntax *form, const IshSyntax **out_name, const IshSyntax **out_body, bool *out_exported) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    size_t base = 1u;
    bool exported = false;
    if (form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(form->as.seq.items[base], "record") || form->as.seq.items[base + 1u]->kind != ISH_SYN_WORD || !syn_is_protocol(form->as.seq.items[base + 2u], "%-body")) return false;
    *out_name = form->as.seq.items[base + 1u];
    *out_body = form->as.seq.items[base + 2u];
    if (out_exported) *out_exported = exported;
    return true;
}

static void record_field_names_destroy(char **fields, size_t count) {
    for (size_t i = 0; i < count; i++) free(fields[i]);
    free(fields);
}

static bool record_field_seen(char **fields, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(fields[i], name) == 0) return true;
    return false;
}

static bool parse_record_fields(const IshSyntax *body, char ***out_fields, size_t *out_count, IshError *err) {
    char **fields = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        const IshSyntax *stmt = body->as.seq.items[i];
        if (!syn_is_protocol(stmt, "%-expr") || stmt->as.seq.count != 3u || !syn_is_word(stmt->as.seq.items[1], "field") || stmt->as.seq.items[2]->kind != ISH_SYN_WORD) {
            record_field_names_destroy(fields, count);
            return ish_error_set(err, stmt->span, "record body expects 'field NAME' declarations");
        }
        const char *name = stmt->as.seq.items[2]->as.text;
        if (record_field_seen(fields, count, name)) {
            record_field_names_destroy(fields, count);
            return ish_error_set(err, stmt->as.seq.items[2]->span, "record field '%s' is declared more than once", name);
        }
        if (count == cap) {
            size_t next_cap = cap ? cap * 2u : 4u;
            char **next = realloc(fields, next_cap * sizeof(*next));
            if (!next) {
                record_field_names_destroy(fields, count);
                return ish_error_oom(err, stmt->span);
            }
            fields = next;
            cap = next_cap;
        }
        fields[count] = ish_strdup(name);
        if (!fields[count]) {
            record_field_names_destroy(fields, count);
            return ish_error_oom(err, stmt->as.seq.items[2]->span);
        }
        count++;
    }
    *out_fields = fields;
    *out_count = count;
    return true;
}

static IshCore *make_prim_app(IshPrimitive primitive, IshSpan span, IshError *err) {
    IshCore *callee = ish_core_primitive(primitive, span);
    IshCore *app = callee ? ish_core_app(callee, span) : NULL;
    if (!app) {
        ish_core_free(callee);
        ish_error_oom(err, span);
        return NULL;
    }
    return app;
}

static bool core_app_add_or_oom(IshCore *app, IshCore *arg, IshError *err, IshSpan span) {
    if (!arg || !ish_core_app_add_arg(app, arg)) {
        ish_core_free(arg);
        ish_error_oom(err, span);
        return false;
    }
    return true;
}

static IshCore *record_field_default_fn(ExpandContext *ctx, const char *field, IshSpan span, IshError *err) {
    IshCore *app = make_prim_app(ISH_PRIM_RECORD_FIELD, span, err);
    if (!app) return NULL;
    if (!core_app_add_or_oom(app, ish_core_arg_ref(0u, span), err, span) ||
        !core_app_add_or_oom(app, ish_core_literal(ish_atom(ctx->rt, field), span), err, span)) {
        ish_core_free(app);
        return NULL;
    }
    return ish_core_fn(field, 1u, app, span);
}

static IshCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, char **fields, size_t field_count, IshSpan span, IshError *err) {
    IshCore *dict = make_prim_app(ISH_PRIM_DICT, span, err);
    if (!dict) return NULL;
    for (size_t i = 0; i < field_count; i++) {
        if (!core_app_add_or_oom(dict, ish_core_literal(ish_atom(ctx->rt, fields[i]), span), err, span) ||
            !core_app_add_or_oom(dict, ish_core_arg_ref((uint32_t)i, span), err, span)) {
            ish_core_free(dict);
            return NULL;
        }
    }
    IshCore *make = make_prim_app(ISH_PRIM_MAKE_RECORD, span, err);
    if (!make) { ish_core_free(dict); return NULL; }
    if (!core_app_add_or_oom(make, ish_core_literal(ish_atom(ctx->rt, record_name), span), err, span) ||
        !core_app_add_or_oom(make, dict, err, span)) {
        ish_core_free(make);
        return NULL;
    }
    if (field_count > UINT32_MAX) {
        ish_core_free(make);
        ish_error_set(err, span, "record '%s' has too many fields", record_name);
        return NULL;
    }
    return ish_core_fn(record_name, (uint32_t)field_count, make, span);
}

static IshCore *record_predicate_fn(ExpandContext *ctx, const char *record_name, const char *predicate_name, IshSpan span, IshError *err) {
    IshCore *pred = make_prim_app(ISH_PRIM_RECORD_PRED, span, err);
    if (!pred) return NULL;
    if (!core_app_add_or_oom(pred, ish_core_arg_ref(0u, span), err, span)) { ish_core_free(pred); return NULL; }
    IshCore *type = make_prim_app(ISH_PRIM_RECORD_TYPE, span, err);
    if (!type) { ish_core_free(pred); return NULL; }
    if (!core_app_add_or_oom(type, ish_core_arg_ref(0u, span), err, span)) { ish_core_free(pred); ish_core_free(type); return NULL; }
    IshCore *eq = make_prim_app(ISH_PRIM_EQ, span, err);
    if (!eq) { ish_core_free(pred); ish_core_free(type); return NULL; }
    if (!core_app_add_or_oom(eq, type, err, span) || !core_app_add_or_oom(eq, ish_core_literal(ish_atom(ctx->rt, record_name), span), err, span)) {
        ish_core_free(pred);
        ish_core_free(eq);
        return NULL;
    }
    IshCore *cond = ish_core_cond(pred, eq, ish_core_literal(ish_atom(ctx->rt, "false"), span), span);
    if (!cond) {
        ish_core_free(pred);
        ish_core_free(eq);
        return (IshCore *)(uintptr_t)ish_error_oom(err, span);
    }
    return ish_core_fn(predicate_name, 1u, cond, span);
}

static char *record_predicate_name(const char *record_name) {
    IshBuffer buf;
    ish_buf_init(&buf);
    if (!ish_buf_append(&buf, record_name) || !ish_buf_append_char(&buf, '?')) { ish_buf_destroy(&buf); return NULL; }
    return ish_buf_take(&buf);
}

static bool register_record_protocol_surface(ExpandContext *ctx, const IshSyntax *name_syntax, const char *record_name, char **fields, size_t field_count, IshSpan span, IshCore **out_define, IshCore **out_extend, IshError *err) {
    if (ctx->protocol_count == ctx->protocol_cap) {
        size_t cap = ctx->protocol_cap ? ctx->protocol_cap * 2u : 4u;
        ProtocolDef *next = realloc(ctx->protocols, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, span);
        ctx->protocols = next;
        ctx->protocol_cap = cap;
    }
    ProtocolDef *p = &ctx->protocols[ctx->protocol_count];
    memset(p, 0, sizeof(*p));
    p->name = ish_strdup(record_name);
    p->method_count = field_count;
    p->methods = field_count == 0 ? NULL : calloc(field_count, sizeof(*p->methods));
    if (!p->name || (field_count != 0 && !p->methods)) { protocol_def_destroy(p); return ish_error_oom(err, span); }

    IshCore *define = ish_core_define_protocol(ish_atom(ctx->rt, record_name), span);
    IshCore *extend = ish_core_extend_protocol(ish_atom(ctx->rt, record_name), ish_atom(ctx->rt, record_name), span);
    if (!define || !extend) { ish_core_free(define); ish_core_free(extend); protocol_def_destroy(p); return ish_error_oom(err, span); }
    for (size_t i = 0; i < field_count; i++) {
        IshProtocolMethodDef *method = &p->methods[i];
        method->name = ish_strdup(fields[i]);
        method->arity = 1u;
        method->has_default = true;
        method->seen_decl = true;
        method->exported = true;
        if (!method->name || !syntax_scopes_copy(&method->scopes, name_syntax)) {
            ish_core_free(define);
            ish_core_free(extend);
            protocol_def_destroy(p);
            return ish_error_oom(err, span);
        }
        IshCore *default_fn = record_field_default_fn(ctx, fields[i], span, err);
        if (!default_fn || !ish_core_define_protocol_add_method(define, ish_atom(ctx->rt, fields[i]), 1u, default_fn)) {
            ish_core_free(default_fn);
            ish_core_free(define);
            ish_core_free(extend);
            protocol_def_destroy(p);
            return false;
        }
        if (!install_method_surface(ctx, record_name, fields[i], 1u, &method->scopes, err)) {
            ish_core_free(define);
            ish_core_free(extend);
            protocol_def_destroy(p);
            return false;
        }
    }
    IshScopeSet protocol_scopes;
    if (!syntax_scopes_copy(&protocol_scopes, name_syntax)) {
        ish_core_free(define);
        ish_core_free(extend);
        protocol_def_destroy(p);
        return ish_error_oom(err, span);
    }
    uint32_t payload = (uint32_t)ctx->protocol_count;
    if (!ish_binding_table_add(&ctx->bindings, record_name, 0, ISH_BIND_SPACE_PROTOCOL, ISH_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        ish_scope_set_destroy(&protocol_scopes);
        ish_core_free(define);
        ish_core_free(extend);
        protocol_def_destroy(p);
        return ish_error_oom(err, span);
    }
    ish_scope_set_destroy(&protocol_scopes);
    ctx->protocol_count++;
    *out_define = define;
    *out_extend = extend;
    return true;
}

IshCore *expand_record_decl(ExpandContext *ctx, const IshSyntax *form, IshSyntax *const *items, size_t index, size_t count, IshError *err) {
    const IshSyntax *name_syntax = NULL;
    const IshSyntax *body = NULL;
    bool exported = false;
    if (!record_form_parts(form, &name_syntax, &body, &exported)) return expand_error(err, form->span, "record expects 'record NAME do field ... end'");
    const char *record_name = name_syntax->as.text;
    char **fields = NULL;
    size_t field_count = 0;
    if (!parse_record_fields(body, &fields, &field_count, err)) return NULL;
    char *predicate_name = record_predicate_name(record_name);
    if (!predicate_name) { record_field_names_destroy(fields, field_count); return (IshCore *)(uintptr_t)ish_error_oom(err, name_syntax->span); }

    IshCore *define_protocol = NULL;
    IshCore *extend_protocol = NULL;
    if (!register_record_protocol_surface(ctx, name_syntax, record_name, fields, field_count, form->span, &define_protocol, &extend_protocol, err)) {
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }

    bool top_level = ctx->frame == ISH_FRAME_TOP;
    size_t saved_count = ctx->bindings.count;
    uint32_t saved_next = ctx->next_slot;
    uint32_t constructor_slot = 0;
    uint32_t predicate_slot = 0;
    bool ok = top_level ? global_push(ctx, record_name, name_syntax, &constructor_slot) : local_push_scoped(ctx, record_name, name_syntax, &constructor_slot);
    IshSyntax *predicate_syntax = NULL;
    if (ok) {
        predicate_syntax = ish_syn_word(predicate_name, name_syntax->span);
        if (!predicate_syntax) ok = false;
    }
    if (ok) {
        const IshScopeSet *scopes = ish_syn_scope_set(name_syntax, 0);
        if (scopes) for (size_t i = 0; i < scopes->count && ok; i++) ok = ish_syn_scope_add(predicate_syntax, 0, scopes->items[i]);
    }
    if (ok) ok = top_level ? global_push(ctx, predicate_name, predicate_syntax, &predicate_slot) : local_push_scoped(ctx, predicate_name, predicate_syntax, &predicate_slot);
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        ish_syn_free(predicate_syntax);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, name_syntax->span);
    }
    if (top_level && ctx->in_package) {
        IshScopeSet record_scopes;
        ish_scope_set_init(&record_scopes);
        if (!syntax_scopes_copy(&record_scopes, name_syntax)) ok = false;
        if (ok && !record_package_global(ctx, record_name, constructor_slot, &record_scopes)) ok = false;
        if (ok && !record_package_global(ctx, predicate_name, predicate_slot, &record_scopes)) ok = false;
        if (ok && exported && !record_export(ctx, record_name, constructor_slot)) ok = false;
        if (ok && exported && !record_export(ctx, predicate_name, predicate_slot)) ok = false;
        if (ok && exported) {
            for (size_t i = 0; i < field_count && ok; i++) {
                if (ctx->decl_method_count == ctx->decl_method_cap) {
                    size_t cap = ctx->decl_method_cap ? ctx->decl_method_cap * 2u : 4u;
                    IshProtocolMethodDef *next = realloc(ctx->decl_methods, cap * sizeof(*next));
                    if (!next) { ok = false; break; }
                    ctx->decl_methods = next;
                    ctx->decl_method_cap = cap;
                }
                IshProtocolMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
                memset(method, 0, sizeof(*method));
                method->name = ish_strdup(fields[i]);
                method->arity = 1u;
                method->has_default = true;
                method->seen_decl = true;
                method->exported = true;
                method->default_fn = record_field_default_fn(ctx, fields[i], form->span, err);
                if (!method->name || !method->default_fn || !ish_scope_set_copy(&method->scopes, &record_scopes)) { ish_protocol_method_def_destroy(method); ok = false; break; }
                ctx->decl_method_count++;
            }
        }
        ish_scope_set_destroy(&record_scopes);
    }
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        ish_syn_free(predicate_syntax);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, name_syntax->span);
    }

    IshCore *constructor = record_constructor_fn(ctx, record_name, fields, field_count, form->span, err);
    IshCore *predicate = constructor ? record_predicate_fn(ctx, record_name, predicate_name, form->span, err) : NULL;
    if (!constructor || !predicate) {
        ish_syn_free(predicate_syntax);
        ish_core_free(constructor);
        ish_core_free(predicate);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IshCore *cont = expand_body_items(ctx, items, index + 1u, count, err);
    if (!cont) {
        ish_syn_free(predicate_syntax);
        ish_core_free(constructor);
        ish_core_free(predicate);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IshCore *letrec = ish_core_letrec(form->span);
    if (!letrec) {
        ish_syn_free(predicate_syntax);
        ish_core_free(constructor);
        ish_core_free(predicate);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        ish_core_free(cont);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
    }
    if (top_level) ish_core_letrec_set_global(letrec);
    bool letrec_ok = ish_core_letrec_add(letrec, record_name, constructor_slot, constructor);
    if (letrec_ok) constructor = NULL;
    if (letrec_ok) {
        letrec_ok = ish_core_letrec_add(letrec, predicate_name, predicate_slot, predicate);
        if (letrec_ok) predicate = NULL;
    }
    if (letrec_ok) {
        letrec_ok = ish_core_letrec_set_body(letrec, cont);
        if (letrec_ok) cont = NULL;
    }
    if (!letrec_ok) {
        ish_syn_free(predicate_syntax);
        ish_core_free(constructor);
        ish_core_free(predicate);
        ish_core_free(cont);
        ish_core_free(letrec);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
    }
    IshCore *export_extend = NULL;
    if (ctx->in_package && exported && ctx->protocol_name) {
        export_extend = ish_core_extend_protocol(ish_atom(ctx->rt, ctx->protocol_name), ish_atom(ctx->rt, record_name), form->span);
        if (!export_extend) {
            ish_syn_free(predicate_syntax);
            ish_core_free(define_protocol);
            ish_core_free(extend_protocol);
            ish_core_free(letrec);
            free(predicate_name);
            record_field_names_destroy(fields, field_count);
            return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
        }
    }
    IshCore *seq = ish_core_do(form->span);
    bool seq_ok = seq != NULL;
    if (seq_ok) { seq_ok = ish_core_do_add(seq, define_protocol); if (seq_ok) define_protocol = NULL; }
    if (seq_ok) { seq_ok = ish_core_do_add(seq, extend_protocol); if (seq_ok) extend_protocol = NULL; }
    if (seq_ok && export_extend) { seq_ok = ish_core_do_add(seq, export_extend); if (seq_ok) export_extend = NULL; }
    if (seq_ok) { seq_ok = ish_core_do_add(seq, letrec); if (seq_ok) letrec = NULL; }
    if (!seq_ok) {
        ish_syn_free(predicate_syntax);
        ish_core_free(seq);
        ish_core_free(define_protocol);
        ish_core_free(extend_protocol);
        ish_core_free(export_extend);
        ish_core_free(letrec);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
    }
    ish_syn_free(predicate_syntax);
    free(predicate_name);
    record_field_names_destroy(fields, field_count);
    return seq;
}
