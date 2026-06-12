#include "internal.h"

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err);
static bool method_signature_arity(const IdmSyntax *form, uint32_t *out_arity, IdmError *err);
static IdmProtocolMethodDef *find_decl_method(ExpandContext *ctx, const char *name);
static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t arity, IdmError *err);
static void extend_impls_destroy(ExtendMethodImpl *impls, size_t count);
static const IdmProtocolMethodDef *find_protocol_method_def(const IdmProtocolMethodDef *methods, size_t count, const char *name);
static bool extend_has_impl(const ExtendMethodImpl *impls, size_t count, const char *name);
static bool extend_impls_push(ExtendMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IdmCore *fn, IdmError *err, IdmSpan span);
static IdmCore *build_extend_core(ExpandContext *ctx, const char *protocol, const char *type, const IdmProtocolMethodDef *methods, size_t method_count, const IdmSyntax *body, IdmError *err);
static IdmCore *sequence_two(IdmCore *first, IdmCore *second, IdmSpan span, IdmError *err);
static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported);
static void record_field_names_destroy(char **fields, size_t count);
static bool record_field_seen(char **fields, size_t count, const char *name);
static bool parse_record_fields(const IdmSyntax *body, char ***out_fields, size_t *out_count, IdmError *err);
static IdmCore *make_prim_app(IdmPrimitive primitive, IdmSpan span, IdmError *err);
static bool core_app_add_or_oom(IdmCore *app, IdmCore *arg, IdmError *err, IdmSpan span);
static IdmCore *record_field_default_fn(ExpandContext *ctx, const char *field, IdmSpan span, IdmError *err);
static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const char *identity, char **fields, size_t field_count, IdmSpan span, IdmError *err);
static IdmCore *record_predicate_fn(ExpandContext *ctx, const char *identity, const char *predicate_name, IdmSpan span, IdmError *err);
static char *record_predicate_name(const char *record_name);
static bool register_record_protocol_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, const unsigned char hash[32], bool exported, char **fields, size_t field_count, IdmSpan span, IdmCore **out_define, IdmCore **out_extend, IdmError *err);

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 3 || !syn_is_word(form->as.seq.items[1], "method") || form->as.seq.items[2]->kind != IDM_SYN_WORD) return false;
    bool has_body = false;
    for (size_t i = 3; i < form->as.seq.count; i++) {
        const IdmSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || syn_is_protocol(item, "%-body")) {
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

static bool method_signature_arity(const IdmSyntax *form, uint32_t *out_arity, IdmError *err) {
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return false;
    (void)name;
    size_t arity = 0;
    for (size_t i = param_start; i < form->as.seq.count; i++) {
        const IdmSyntax *item = form->as.seq.items[i];
        if (syn_is_word(item, "->") || syn_is_protocol(item, "%-body")) break;
        arity++;
    }
    if (arity > UINT32_MAX) return idm_error_set(err, form->span, "method arity is too large");
    *out_arity = (uint32_t)arity;
    return true;
}

static IdmProtocolMethodDef *find_decl_method(ExpandContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->decl_method_count; i++) if (strcmp(ctx->decl_methods[i].name, name) == 0) return &ctx->decl_methods[i];
    return NULL;
}

static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t arity, IdmError *err) {
    if (find_decl_method(ctx, name_syntax->as.text)) return idm_error_set(err, name_syntax->span, "protocol '%s' declares method '%s' more than once", ctx->protocol_name ? ctx->protocol_name : "<anonymous>", name_syntax->as.text);
    if (ctx->decl_method_count == ctx->decl_method_cap) {
        size_t cap = ctx->decl_method_cap ? ctx->decl_method_cap * 2u : 4u;
        IdmProtocolMethodDef *next = realloc(ctx->decl_methods, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, name_syntax->span);
        ctx->decl_methods = next;
        ctx->decl_method_cap = cap;
    }
    IdmProtocolMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
    memset(method, 0, sizeof(*method));
    method->name = idm_strdup(name_syntax->as.text);
    method->arity = arity;
    method->exported = true;
    if (!method->name || !binder_scopes_pruned(ctx, name_syntax, &method->scopes)) {
        idm_protocol_method_def_destroy(method);
        return idm_error_oom(err, name_syntax->span);
    }
    ctx->decl_method_count++;
    return install_method_surface(ctx, ctx->protocol_name ? ctx->protocol_name : "<anonymous>", method->name, arity, &method->scopes, ctx->unit, ctx->unit_key, err);
}

bool predeclare_protocol_methods(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmError *err) {
    if (!ctx->protocol_name) return true;
    for (size_t i = start; i < count; i++) {
        const IdmSyntax *name = NULL;
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

IdmCore *expand_method_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    if (!ctx->protocol_name) return expand_error(err, form->span, "method declarations are only valid inside protocol bodies");
    const IdmSyntax *name = NULL;
    size_t param_start = 0;
    bool has_body = false;
    if (!method_form_parts(form, &name, &param_start, &has_body, err)) return NULL;
    uint32_t arity = 0;
    if (!method_signature_arity(form, &arity, err)) return NULL;
    IdmProtocolMethodDef *method = find_decl_method(ctx, name->as.text);
    if (!method) {
        if (!add_decl_method(ctx, name, arity, err)) return NULL;
        method = find_decl_method(ctx, name->as.text);
    }
    if (!method) return (IdmCore *)(uintptr_t)idm_error_oom(err, name->span);
    if (method->seen_decl) return expand_error(err, name->span, "protocol '%s' declares method '%s' more than once", ctx->protocol_name, name->as.text);
    if (method->arity != arity) return expand_error(err, name->span, "method '%s.%s' arity changed during declaration", ctx->protocol_name, name->as.text);
    method->seen_decl = true;
    if (has_body) {
        IdmCore *fn = expand_function_literal(ctx, name->as.text, form->as.seq.items[1], form->as.seq.items, param_start, form->as.seq.count, err);
        if (!fn) return NULL;
        method->default_fn = fn;
        method->has_default = true;
    }
    return expand_body_items(ctx, items, index + 1u, count, false, err);
}

static void extend_impls_destroy(ExtendMethodImpl *impls, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(impls[i].name);
        idm_core_free(impls[i].fn);
    }
    free(impls);
}

static const IdmProtocolMethodDef *find_protocol_method_def(const IdmProtocolMethodDef *methods, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(methods[i].name, name) == 0) return &methods[i];
    return NULL;
}

static bool extend_has_impl(const ExtendMethodImpl *impls, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(impls[i].name, name) == 0) return true;
    return false;
}

static bool extend_impls_push(ExtendMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IdmCore *fn, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        ExtendMethodImpl *next = realloc(*impls, next_cap * sizeof(*next));
        if (!next) return idm_error_oom(err, span);
        *impls = next;
        *cap = next_cap;
    }
    ExtendMethodImpl *impl = &(*impls)[(*count)++];
    impl->name = idm_strdup(name);
    impl->arity = arity;
    impl->fn = fn;
    if (!impl->name) {
        (*count)--;
        return idm_error_oom(err, span);
    }
    return true;
}

static IdmCore *build_extend_core(ExpandContext *ctx, const char *protocol, const char *type, const IdmProtocolMethodDef *methods, size_t method_count, const IdmSyntax *body, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool surface_ok = true;
    for (size_t i = 0; i < method_count && surface_ok; i++) surface_ok = install_method_surface(ctx, protocol, methods[i].name, methods[i].arity, &ctx->empty_scopes, ctx->unit, ctx->unit_key, err);
    if (!surface_ok) {
        surface_rollback(ctx, &checkpoint);
        return NULL;
    }

    ExtendMethodImpl *impls = NULL;
    size_t impl_count = 0;
    size_t impl_cap = 0;
    bool ok = true;
    for (size_t i = 1; i < body->as.seq.count && ok; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        const IdmSyntax *name = NULL;
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
        const IdmProtocolMethodDef *contract = find_protocol_method_def(methods, method_count, name->as.text);
        if (!contract) { expand_error(err, name->span, "protocol '%s' has no method '%s'", protocol, name->as.text); ok = false; break; }
        if (contract->arity != arity) { expand_error(err, name->span, "method '%s.%s' expects %u argument(s), got %u", protocol, name->as.text, contract->arity, arity); ok = false; break; }
        if (extend_has_impl(impls, impl_count, name->as.text)) { expand_error(err, name->span, "extend for '%s' provides method '%s' more than once", protocol, name->as.text); ok = false; break; }
        IdmCore *fn = expand_function_literal(ctx, name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
        if (!fn) { ok = false; break; }
        if (!extend_impls_push(&impls, &impl_count, &impl_cap, name->as.text, arity, fn, err, name->span)) {
            idm_core_free(fn);
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
    IdmCore *core = idm_core_extend_protocol(idm_atom(ctx->rt, protocol), idm_atom(ctx->rt, type), body->span);
    if (!core) {
        extend_impls_destroy(impls, impl_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
    }
    for (size_t i = 0; i < impl_count; i++) {
        IdmCore *fn = impls[i].fn;
        impls[i].fn = NULL;
        if (!idm_core_extend_protocol_add_impl(core, idm_atom(ctx->rt, impls[i].name), impls[i].arity, fn)) {
            idm_core_free(fn);
            idm_core_free(core);
            extend_impls_destroy(impls, impl_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
        }
    }
    extend_impls_destroy(impls, impl_count);
    return core;
}

static IdmCore *protocol_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool activate, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const char *name = name_syntax->as.text;
    unsigned char hash[32];
    char *identity = protocol_identity(ctx, name, form, hash, err);
    if (!identity) return NULL;
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    if (!compile_package_module(ctx, body, identity, hash, &art, err)) { free(identity); return NULL; }
    memcpy(art.src_hash, hash, 32u);
    if (ctx->protocol_count == ctx->protocol_cap) {
        size_t cap = ctx->protocol_cap ? ctx->protocol_cap * 2u : 4u;
        ProtocolDef *next = realloc(ctx->protocols, cap * sizeof(*next));
        if (!next) { free(identity); idm_artifact_destroy(&art); return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span); }
        ctx->protocols = next;
        ctx->protocol_cap = cap;
    }
    ProtocolDef *p = &ctx->protocols[ctx->protocol_count];
    memset(p, 0, sizeof(*p));
    p->name = identity;
    p->art = art;
    IdmScopeSet protocol_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &protocol_scopes)) {
        protocol_def_destroy(p);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    uint32_t payload = (uint32_t)ctx->protocol_count;
    if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&protocol_scopes);
        protocol_def_destroy(p);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    idm_scope_set_destroy(&protocol_scopes);
    ctx->protocol_count++;
    const char *runtime_name = p->name;
    IdmBytecodeModule *module = p->art.module;
    uint32_t init_fn = p->art.init_fn;
    p->art.module = NULL;
    size_t global_count = p->art.global_count;
    const IdmPkgGlobal *globals = p->art.globals;
    uint32_t *global_src = NULL;
    uint32_t *global_dst = NULL;
    if (global_count != 0) {
        global_src = malloc(global_count * sizeof(*global_src));
        global_dst = malloc(global_count * sizeof(*global_dst));
        if (!global_src || !global_dst) {
            free(global_src);
            free(global_dst);
            if (module) { idm_bc_destroy(module); free(module); }
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
    }
    for (size_t i = 0; i < global_count; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        global_src[i] = globals[i].slot;
        global_dst[i] = consumer_slot;
        if (!idm_binding_table_add(&ctx->bindings, globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &globals[i].scopes, consumer_slot, IDM_FRAME_GLOBAL, NULL)) {
            free(global_src);
            free(global_dst);
            if (module) { idm_bc_destroy(module); free(module); }
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
    }
    if (activate) {
        IdmScopeSet act_scopes;
        bool activated = binder_scopes_pruned(ctx, name_syntax, &act_scopes);
        if (!activated) {
            idm_error_oom(err, form->span);
        } else {
            IdmArtifact art_view = ctx->protocols[payload].art;
            activated = activate_artifact(ctx, runtime_name, &art_view, art_view.scope_base, &act_scopes, form->span, err) && record_activation(ctx, name, form->span, err);
            idm_scope_set_destroy(&act_scopes);
        }
        if (!activated) {
            free(global_src);
            free(global_dst);
            if (module) { idm_bc_destroy(module); free(module); }
            if (err && err->present && err->span.line == 0) err->span = form->span;
            if (err && err->present && form->span.line != 0) idm_error_note(err, "while activating '%s' (%s:%u:%u)", name, form->span.file ? form->span.file : "<unknown>", form->span.line, form->span.column);
            return NULL;
        }
    }
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        free(global_src);
        free(global_dst);
        if (module) { idm_bc_destroy(module); free(module); }
        return NULL;
    }
    IdmCore *runtime = idm_core_use_package(idm_atom(ctx->rt, runtime_name), module, init_fn, global_src, global_dst, global_count, cont, form->span);
    if (!runtime) {
        free(global_src);
        free(global_dst);
        if (module) { idm_bc_destroy(module); free(module); }
        idm_core_free(cont);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    return runtime;
}

IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    if (form->as.seq.count != 4 || fitems[2]->kind != IDM_SYN_WORD || !syn_is_protocol(fitems[3], "%-body"))
        return expand_error(err, form->span, "protocol expects 'protocol NAME do ... end'");
    return protocol_decl_core(ctx, form, fitems[2], fitems[3], false, items, index, count, err);
}

IdmCore *expand_implement_protocol(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    if (form->as.seq.count != 5 || fitems[3]->kind != IDM_SYN_WORD || !syn_is_protocol(fitems[4], "%-body"))
        return expand_error(err, form->span, "implement protocol expects 'implement protocol NAME do ... end'");
    return protocol_decl_core(ctx, form, fitems[3], fitems[4], true, items, index, count, err);
}

IdmCore *expand_implement(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err) {
    const char *name = name_syntax->as.text;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *p = resolve_protocol_def(ctx, name_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, name_syntax->span, "ambiguous protocol '%s'", name);
    IdmScopeSet act_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &act_scopes)) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    if (p) {
        const char *identity = p->name;
        IdmArtifact art_view = p->art;
        bool activated = activate_artifact(ctx, identity, &art_view, art_view.scope_base, &act_scopes, span, err) && record_activation(ctx, name, span, err);
        idm_scope_set_destroy(&act_scopes);
        if (!activated) {
            if (err && err->present && err->span.line == 0) err->span = span;
            if (err && err->present && span.line != 0) idm_error_note(err, "while activating '%s' (%s:%u:%u)", name, span.file ? span.file : "<unknown>", span.line, span.column);
            return NULL;
        }
        return expand_body_items(ctx, items, index + 1u, count, false, err);
    }
    const IdmArtifact *art = artifact_get(ctx, name, span, err);
    if (!art) {
        idm_scope_set_destroy(&act_scopes);
        if (err && err->present && err->message && strcmp(err->message, "package source must be a program") == 0) {
            idm_error_set(err, span, "implement: '%s' is not a protocol", name);
        }
        return NULL;
    }
    IdmScopeId base = 0;
    bool activated = artifact_base(ctx, art, &base, err) &&
                     activate_artifact(ctx, name, art, base, &act_scopes, span, err) &&
                     record_activation(ctx, name, span, err);
    idm_scope_set_destroy(&act_scopes);
    if (!activated) {
        if (err && err->present && err->span.line == 0) err->span = span;
        if (err && err->present && span.line != 0) idm_error_note(err, "while activating '%s' (%s:%u:%u)", name, span.file ? span.file : "<unknown>", span.line, span.column);
        return NULL;
    }
    IdmScopeId min_id = art->scope_base;
    int64_t delta = (int64_t)base - (int64_t)art->scope_base;
    uint32_t *global_src = NULL;
    uint32_t *global_dst = NULL;
    if (art->global_count != 0) {
        global_src = malloc(art->global_count * sizeof(*global_src));
        global_dst = malloc(art->global_count * sizeof(*global_dst));
        if (!global_src || !global_dst) {
            free(global_src);
            free(global_dst);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    for (size_t i = 0; i < art->global_count; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        global_src[i] = art->globals[i].slot;
        global_dst[i] = consumer_slot;
        IdmScopeSet gscopes;
        idm_scope_set_init(&gscopes);
        bool ok = idm_scope_set_copy(&gscopes, &art->globals[i].scopes);
        if (ok) {
            idm_scope_set_relocate(&gscopes, min_id, delta);
            ok = idm_binding_table_add(&ctx->bindings, art->globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &gscopes, consumer_slot, IDM_FRAME_GLOBAL, NULL);
        }
        idm_scope_set_destroy(&gscopes);
        if (!ok) {
            free(global_src);
            free(global_dst);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    bool init_pending = artifact_init_pending(ctx, art);
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        free(global_src);
        free(global_dst);
        return NULL;
    }
    uint32_t fn_off = 0;
    IdmBytecodeModule *module = NULL;
    if (init_pending) {
        module = relocated_module_copy(ctx, art->module, min_id, delta, &fn_off, err);
        if (!module) {
            free(global_src);
            free(global_dst);
            idm_core_free(cont);
            return NULL;
        }
    }
    IdmCore *runtime = idm_core_use_package(idm_atom(ctx->rt, art->name ? art->name : name), module, art->init_fn + fn_off, global_src, global_dst, art->global_count, cont, span);
    if (!runtime) {
        free(global_src);
        free(global_dst);
        idm_bc_destroy(module);
        free(module);
        idm_core_free(cont);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return runtime;
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

static IdmCore *extend_decl_core(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    if (form->as.seq.count != 6 || form->as.seq.items[2]->kind != IDM_SYN_WORD || !syn_is_word(form->as.seq.items[3], "with") || form->as.seq.items[4]->kind != IDM_SYN_WORD || !syn_is_protocol(form->as.seq.items[5], "%-body")) {
        return expand_error(err, form->span, "extend expects 'extend TYPE with PROTOCOL do ... end'");
    }
    const char *type_name = form->as.seq.items[2]->as.text;
    IdmResolveStatus type_status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *type_def = resolve_protocol_def(ctx, form->as.seq.items[2], &type_status);
    if (type_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, form->as.seq.items[2]->span, "ambiguous type '%s'", type_name);
    if (type_def) type_name = type_def->name;
    const char *protocol_name = form->as.seq.items[4]->as.text;
    const IdmProtocolMethodDef *methods = NULL;
    size_t method_count = 0;
    const IdmArtifact *ext = NULL;
    IdmScopeId ext_base = 0;

    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *local = resolve_protocol_def(ctx, form->as.seq.items[4], &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, form->as.seq.items[4]->span, "ambiguous protocol '%s'", protocol_name);
    if (local) {
        protocol_name = local->name;
        methods = local->art.methods;
        method_count = local->art.method_count;
    } else {
        ext = artifact_get(ctx, protocol_name, form->as.seq.items[4]->span, err);
        if (!ext) {
            if (err && err->present && err->message && strcmp(err->message, "package source must be a program") == 0) {
                idm_error_set(err, form->as.seq.items[4]->span, "extend: '%s' is not a protocol", protocol_name);
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

    bool ext_init_pending = ext ? artifact_init_pending(ctx, ext) : false;
    IdmCore *extend_core = build_extend_core(ctx, protocol_name, type_name, methods, method_count, form->as.seq.items[5], err);
    if (!extend_core) return NULL;
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) { idm_core_free(extend_core); return NULL; }
    IdmCore *seq = sequence_two(extend_core, cont, form->span, err);
    if (!seq) return NULL;
    if (ext && ext_init_pending) {
        uint32_t fn_off = 0;
        IdmBytecodeModule *module = relocated_module_copy(ctx, ext->module, ext->scope_base, (int64_t)ext_base - (int64_t)ext->scope_base, &fn_off, err);
        if (!module) {
            idm_core_free(seq);
            return NULL;
        }
        IdmCore *runtime = idm_core_use_package(idm_atom(ctx->rt, ext->name ? ext->name : protocol_name), module, ext->init_fn + fn_off, NULL, NULL, 0u, seq, form->span);
        if (!runtime) {
            idm_bc_destroy(module);
            free(module);
            idm_core_free(seq);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
        return runtime;
    }
    return seq;
}

IdmCore *expand_extend_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    bool dotted = false;
    for (size_t i = 2; i + 2u < form->as.seq.count && !dotted; i++) {
        dotted = form->as.seq.items[i]->kind == IDM_SYN_WORD && syn_is_word(form->as.seq.items[i + 1u], ".") && form->as.seq.items[i + 2u]->kind == IDM_SYN_WORD;
    }
    if (!dotted) return extend_decl_core(ctx, form, items, index, count, err);
    IdmSyntax *folded = idm_syn_list(form->span);
    bool ok = folded != NULL;
    for (size_t i = 0; ok && i < form->as.seq.count;) {
        IdmSyntax *part = NULL;
        if (i >= 2u && form->as.seq.items[i]->kind == IDM_SYN_WORD && i + 2u < form->as.seq.count &&
            syn_is_word(form->as.seq.items[i + 1u], ".") && form->as.seq.items[i + 2u]->kind == IDM_SYN_WORD) {
            size_t chain_end = form->as.seq.count;
            part = make_qualified_word(form->as.seq.items, i, &chain_end, err);
            i = chain_end;
        } else {
            part = idm_syn_clone(form->as.seq.items[i]);
            i++;
        }
        ok = part != NULL && idm_syn_append(folded, part);
        if (!ok) idm_syn_free(part);
    }
    if (!ok) {
        idm_syn_free(folded);
        if (err && !err->present) idm_error_oom(err, form->span);
        return NULL;
    }
    IdmCore *core = extend_decl_core(ctx, folded, items, index, count, err);
    idm_syn_free(folded);
    return core;
}

static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    size_t base = 1u;
    bool exported = false;
    if (form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "export")) {
        exported = true;
        base = 2u;
    }
    if (form->as.seq.count != base + 3u || !syn_is_word(form->as.seq.items[base], "record") || form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD || !syn_is_protocol(form->as.seq.items[base + 2u], "%-body")) return false;
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

static bool parse_record_fields(const IdmSyntax *body, char ***out_fields, size_t *out_count, IdmError *err) {
    char **fields = NULL;
    size_t count = 0;
    size_t cap = 0;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        const IdmSyntax *stmt = body->as.seq.items[i];
        if (!syn_is_protocol(stmt, "%-expr") || stmt->as.seq.count != 3u || !syn_is_word(stmt->as.seq.items[1], "field") || stmt->as.seq.items[2]->kind != IDM_SYN_WORD) {
            record_field_names_destroy(fields, count);
            return idm_error_set(err, stmt->span, "record body expects 'field NAME' declarations");
        }
        const char *name = stmt->as.seq.items[2]->as.text;
        if (record_field_seen(fields, count, name)) {
            record_field_names_destroy(fields, count);
            return idm_error_set(err, stmt->as.seq.items[2]->span, "record field '%s' is declared more than once", name);
        }
        if (count == cap) {
            size_t next_cap = cap ? cap * 2u : 4u;
            char **next = realloc(fields, next_cap * sizeof(*next));
            if (!next) {
                record_field_names_destroy(fields, count);
                return idm_error_oom(err, stmt->span);
            }
            fields = next;
            cap = next_cap;
        }
        fields[count] = idm_strdup(name);
        if (!fields[count]) {
            record_field_names_destroy(fields, count);
            return idm_error_oom(err, stmt->as.seq.items[2]->span);
        }
        count++;
    }
    *out_fields = fields;
    *out_count = count;
    return true;
}

static IdmCore *make_prim_app(IdmPrimitive primitive, IdmSpan span, IdmError *err) {
    IdmCore *callee = idm_core_primitive(primitive, span);
    IdmCore *app = callee ? idm_core_app(callee, span) : NULL;
    if (!app) {
        idm_core_free(callee);
        idm_error_oom(err, span);
        return NULL;
    }
    return app;
}

static bool core_app_add_or_oom(IdmCore *app, IdmCore *arg, IdmError *err, IdmSpan span) {
    if (!arg || !idm_core_app_add_arg(app, arg)) {
        idm_core_free(arg);
        idm_error_oom(err, span);
        return false;
    }
    return true;
}

static IdmCore *record_field_default_fn(ExpandContext *ctx, const char *field, IdmSpan span, IdmError *err) {
    IdmCore *app = make_prim_app(IDM_PRIM_RECORD_FIELD, span, err);
    if (!app) return NULL;
    if (!core_app_add_or_oom(app, idm_core_arg_ref(0u, span), err, span) ||
        !core_app_add_or_oom(app, idm_core_literal(idm_atom(ctx->rt, field), span), err, span)) {
        idm_core_free(app);
        return NULL;
    }
    return idm_core_fn(field, 1u, app, span);
}

static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const char *identity, char **fields, size_t field_count, IdmSpan span, IdmError *err) {
    IdmCore *dict = make_prim_app(IDM_PRIM_DICT, span, err);
    if (!dict) return NULL;
    for (size_t i = 0; i < field_count; i++) {
        if (!core_app_add_or_oom(dict, idm_core_literal(idm_atom(ctx->rt, fields[i]), span), err, span) ||
            !core_app_add_or_oom(dict, idm_core_arg_ref((uint32_t)i, span), err, span)) {
            idm_core_free(dict);
            return NULL;
        }
    }
    IdmCore *make = make_prim_app(IDM_PRIM_RECORD_NEW, span, err);
    if (!make) { idm_core_free(dict); return NULL; }
    if (!core_app_add_or_oom(make, idm_core_literal(idm_atom(ctx->rt, identity), span), err, span) ||
        !core_app_add_or_oom(make, dict, err, span)) {
        idm_core_free(make);
        return NULL;
    }
    if (field_count > UINT32_MAX) {
        idm_core_free(make);
        idm_error_set(err, span, "record '%s' has too many fields", record_name);
        return NULL;
    }
    return idm_core_fn(record_name, (uint32_t)field_count, make, span);
}

static IdmCore *record_predicate_fn(ExpandContext *ctx, const char *identity, const char *predicate_name, IdmSpan span, IdmError *err) {
    IdmCore *pred = make_prim_app(IDM_PRIM_RECORD_PRED, span, err);
    if (!pred) return NULL;
    if (!core_app_add_or_oom(pred, idm_core_arg_ref(0u, span), err, span)) { idm_core_free(pred); return NULL; }
    IdmCore *type = make_prim_app(IDM_PRIM_RECORD_TYPE, span, err);
    if (!type) { idm_core_free(pred); return NULL; }
    if (!core_app_add_or_oom(type, idm_core_arg_ref(0u, span), err, span)) { idm_core_free(pred); idm_core_free(type); return NULL; }
    IdmCore *eq = make_prim_app(IDM_PRIM_EQ, span, err);
    if (!eq) { idm_core_free(pred); idm_core_free(type); return NULL; }
    if (!core_app_add_or_oom(eq, type, err, span) || !core_app_add_or_oom(eq, idm_core_literal(idm_atom(ctx->rt, identity), span), err, span)) {
        idm_core_free(pred);
        idm_core_free(eq);
        return NULL;
    }
    IdmCore *cond = idm_core_cond(pred, eq, idm_core_literal(idm_atom(ctx->rt, "false"), span), span);
    if (!cond) {
        idm_core_free(pred);
        idm_core_free(eq);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return idm_core_fn(predicate_name, 1u, cond, span);
}

static char *record_predicate_name(const char *record_name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_buf_append(&buf, record_name) || !idm_buf_append_char(&buf, '?')) { idm_buf_destroy(&buf); return NULL; }
    return idm_buf_take(&buf);
}

static bool register_record_protocol_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, const unsigned char hash[32], bool exported, char **fields, size_t field_count, IdmSpan span, IdmCore **out_define, IdmCore **out_extend, IdmError *err) {
    if (ctx->protocol_count == ctx->protocol_cap) {
        size_t cap = ctx->protocol_cap ? ctx->protocol_cap * 2u : 4u;
        ProtocolDef *next = realloc(ctx->protocols, cap * sizeof(*next));
        if (!next) { free(identity); return idm_error_oom(err, span); }
        ctx->protocols = next;
        ctx->protocol_cap = cap;
    }
    ProtocolDef *p = &ctx->protocols[ctx->protocol_count];
    memset(p, 0, sizeof(*p));
    p->name = identity;
    p->exported = exported;
    memcpy(p->art.src_hash, hash, 32u);
    p->art.method_count = field_count;
    p->art.methods = field_count == 0 ? NULL : calloc(field_count, sizeof(*p->art.methods));
    if (field_count != 0 && !p->art.methods) { protocol_def_destroy(p); return idm_error_oom(err, span); }

    IdmCore *define = idm_core_define_protocol(idm_atom(ctx->rt, identity), span);
    IdmCore *extend = idm_core_extend_protocol(idm_atom(ctx->rt, identity), idm_atom(ctx->rt, identity), span);
    if (!define || !extend) { idm_core_free(define); idm_core_free(extend); protocol_def_destroy(p); return idm_error_oom(err, span); }
    for (size_t i = 0; i < field_count; i++) {
        IdmProtocolMethodDef *method = &p->art.methods[i];
        method->name = idm_strdup(fields[i]);
        method->arity = 1u;
        method->has_default = true;
        method->seen_decl = true;
        method->exported = true;
        if (!method->name || !binder_scopes_pruned(ctx, name_syntax, &method->scopes)) {
            idm_core_free(define);
            idm_core_free(extend);
            protocol_def_destroy(p);
            return idm_error_oom(err, span);
        }
        IdmCore *default_fn = record_field_default_fn(ctx, fields[i], span, err);
        if (!default_fn || !idm_core_define_protocol_add_method(define, idm_atom(ctx->rt, fields[i]), 1u, default_fn)) {
            idm_core_free(default_fn);
            idm_core_free(define);
            idm_core_free(extend);
            protocol_def_destroy(p);
            return false;
        }
        if (!install_method_surface(ctx, identity, fields[i], 1u, &method->scopes, ctx->unit, ctx->unit_key, err)) {
            idm_core_free(define);
            idm_core_free(extend);
            protocol_def_destroy(p);
            return false;
        }
    }
    IdmScopeSet protocol_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &protocol_scopes)) {
        idm_core_free(define);
        idm_core_free(extend);
        protocol_def_destroy(p);
        return idm_error_oom(err, span);
    }
    uint32_t payload = (uint32_t)ctx->protocol_count;
    if (!idm_binding_table_add(&ctx->bindings, record_name, IDM_PHASE_ANY, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, &protocol_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&protocol_scopes);
        idm_core_free(define);
        idm_core_free(extend);
        protocol_def_destroy(p);
        return idm_error_oom(err, span);
    }
    idm_scope_set_destroy(&protocol_scopes);
    ctx->protocol_count++;
    *out_define = define;
    *out_extend = extend;
    return true;
}

IdmCore *expand_record_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const IdmSyntax *name_syntax = NULL;
    const IdmSyntax *body = NULL;
    bool exported = false;
    if (!record_form_parts(form, &name_syntax, &body, &exported)) return expand_error(err, form->span, "record expects 'record NAME do field ... end'");
    const char *record_name = name_syntax->as.text;
    char **fields = NULL;
    size_t field_count = 0;
    if (!parse_record_fields(body, &fields, &field_count, err)) return NULL;
    unsigned char hash[32];
    char *predicate_name = record_predicate_name(record_name);
    char *owned_identity = predicate_name ? protocol_identity(ctx, record_name, form, hash, err) : NULL;
    if (!predicate_name || !owned_identity) {
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        if (err && !err->present) idm_error_oom(err, name_syntax->span);
        return NULL;
    }
    const char *identity = owned_identity;

    IdmCore *define_protocol = NULL;
    IdmCore *extend_protocol = NULL;
    if (!register_record_protocol_surface(ctx, name_syntax, record_name, owned_identity, hash, exported && ctx->in_package, fields, field_count, form->span, &define_protocol, &extend_protocol, err)) {
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }

    bool top_level = ctx->frame == IDM_FRAME_TOP;
    size_t saved_count = ctx->bindings.count;
    uint32_t saved_next = ctx->next_slot;
    uint32_t constructor_slot = 0;
    uint32_t predicate_slot = 0;
    bool ok = top_level ? global_push_def_binder(ctx, record_name, name_syntax, &constructor_slot) : local_push_def_binder(ctx, record_name, name_syntax, &constructor_slot);
    IdmSyntax *predicate_syntax = NULL;
    if (ok) {
        predicate_syntax = idm_syn_word(predicate_name, name_syntax->span);
        if (!predicate_syntax) ok = false;
    }
    if (ok) {
        const IdmScopeSet *scopes = idm_syn_scope_set(name_syntax, 0);
        if (scopes) for (size_t i = 0; i < scopes->count && ok; i++) ok = idm_syn_scope_add(predicate_syntax, 0, scopes->items[i]);
    }
    if (ok) ok = top_level ? global_push_def_binder(ctx, predicate_name, predicate_syntax, &predicate_slot) : local_push_def_binder(ctx, predicate_name, predicate_syntax, &predicate_slot);
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (top_level && ctx->in_package) {
        IdmScopeSet record_scopes;
        idm_scope_set_init(&record_scopes);
        if (!syntax_scopes_copy(&record_scopes, name_syntax)) ok = false;
        if (ok && !record_package_global(ctx, record_name, constructor_slot, &record_scopes)) ok = false;
        if (ok && !record_package_global(ctx, predicate_name, predicate_slot, &record_scopes)) ok = false;
        if (ok && exported && !record_export(ctx, record_name, constructor_slot)) ok = false;
        if (ok && exported && !record_export(ctx, predicate_name, predicate_slot)) ok = false;
        if (ok && exported) {
            for (size_t i = 0; i < field_count && ok; i++) {
                if (ctx->decl_method_count == ctx->decl_method_cap) {
                    size_t cap = ctx->decl_method_cap ? ctx->decl_method_cap * 2u : 4u;
                    IdmProtocolMethodDef *next = realloc(ctx->decl_methods, cap * sizeof(*next));
                    if (!next) { ok = false; break; }
                    ctx->decl_methods = next;
                    ctx->decl_method_cap = cap;
                }
                IdmProtocolMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
                memset(method, 0, sizeof(*method));
                method->name = idm_strdup(fields[i]);
                method->arity = 1u;
                method->has_default = true;
                method->seen_decl = true;
                method->exported = true;
                method->default_fn = record_field_default_fn(ctx, fields[i], form->span, err);
                if (!method->name || !method->default_fn || !idm_scope_set_copy(&method->scopes, &record_scopes)) { idm_protocol_method_def_destroy(method); ok = false; break; }
                ctx->decl_method_count++;
            }
        }
        idm_scope_set_destroy(&record_scopes);
    }
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }

    IdmCore *constructor = record_constructor_fn(ctx, record_name, identity, fields, field_count, form->span, err);
    IdmCore *predicate = constructor ? record_predicate_fn(ctx, identity, predicate_name, form->span, err) : NULL;
    if (!constructor || !predicate) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *letrec = idm_core_letrec(form->span);
    if (!letrec) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        idm_core_free(cont);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    if (top_level) idm_core_letrec_set_global(letrec);
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
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    IdmCore *export_extend = NULL;
    if (ctx->in_package && exported && ctx->protocol_name) {
        export_extend = idm_core_extend_protocol(idm_atom(ctx->rt, ctx->protocol_name), idm_atom(ctx->rt, identity), form->span);
        if (!export_extend) {
            idm_syn_free(predicate_syntax);
            idm_core_free(define_protocol);
            idm_core_free(extend_protocol);
            idm_core_free(letrec);
            free(predicate_name);
            record_field_names_destroy(fields, field_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
    }
    IdmCore *seq = idm_core_do(form->span);
    bool seq_ok = seq != NULL;
    if (seq_ok) { seq_ok = idm_core_do_add(seq, define_protocol); if (seq_ok) define_protocol = NULL; }
    if (seq_ok) { seq_ok = idm_core_do_add(seq, extend_protocol); if (seq_ok) extend_protocol = NULL; }
    if (seq_ok && export_extend) { seq_ok = idm_core_do_add(seq, export_extend); if (seq_ok) export_extend = NULL; }
    if (seq_ok) { seq_ok = idm_core_do_add(seq, letrec); if (seq_ok) letrec = NULL; }
    if (!seq_ok) {
        idm_syn_free(predicate_syntax);
        idm_core_free(seq);
        idm_core_free(define_protocol);
        idm_core_free(extend_protocol);
        idm_core_free(export_extend);
        idm_core_free(letrec);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    idm_syn_free(predicate_syntax);
    free(predicate_name);
    record_field_names_destroy(fields, field_count);
    return seq;
}
