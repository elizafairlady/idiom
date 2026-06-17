#include "internal.h"

static bool method_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_has_body, IdmError *err);
static IdmSyntax *require_form_trait_word(const IdmSyntax *form, IdmError *err);
static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, const char *name);
static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, const char *name, IdmError *err, IdmSpan span);
static void trait_requirements_destroy(IdmTraitRequirementDef *requirements, size_t count);
static bool install_required_trait_methods(ExpandContext *ctx, const TraitDef *required, const IdmSyntax *name_syntax, IdmError *err);
static bool method_signature_arity(const IdmSyntax *form, uint32_t *out_arity, IdmError *err);
static IdmTraitMethodDef *find_decl_method(ExpandContext *ctx, const char *name);
static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t arity, IdmError *err);
static void trait_impls_destroy(TraitMethodImpl *impls, size_t count);
static const IdmTraitMethodDef *find_trait_method_def(const IdmTraitMethodDef *methods, size_t count, const char *name);
static bool trait_has_impl(const TraitMethodImpl *impls, size_t count, const char *name);
static bool trait_impls_push(TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IdmCore *fn, IdmError *err, IdmSpan span);
static IdmCore *build_trait_implement_core(ExpandContext *ctx, const char *trait, const char *type, const IdmTraitMethodDef *methods, size_t method_count, const IdmSyntax *body, IdmSpan span, IdmError *err);
static IdmCore *sequence_two(IdmCore *first, IdmCore *second, IdmSpan span, IdmError *err);
static bool record_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body, bool *out_exported);
static void record_field_names_destroy(char **fields, size_t count);
static bool record_field_seen(char **fields, size_t count, const char *name);
static bool parse_record_fields(const IdmSyntax *body, char ***out_fields, size_t *out_count, IdmError *err);
static IdmCore *make_prim_call(IdmPrimitive primitive, IdmSpan span, IdmError *err);
static bool core_call_add_or_oom(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span);
static IdmCore *record_field_default_fn(ExpandContext *ctx, const char *field, IdmSpan span, IdmError *err);
static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const char *identity, char **fields, size_t field_count, IdmSpan span, IdmError *err);
static IdmCore *record_predicate_fn(ExpandContext *ctx, const char *identity, const char *predicate_name, IdmSpan span, IdmError *err);
static char *record_predicate_name(const char *record_name);
static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, bool exported, char **fields, size_t field_count, IdmSpan span, IdmCore **out_define, IdmCore **out_implement, IdmError *err);
static IdmCore *trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool exported, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);

static void import_names_free(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
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
    if (form->as.seq.items[2]->kind != IDM_SYN_WORD) {
        if (err) idm_error_set(err, form->as.seq.items[2]->span, "trait requirement expects a trait name");
        return NULL;
    }
    if (form->as.seq.count == 3u) return idm_syn_clone(form->as.seq.items[2]);
    size_t chain_end = form->as.seq.count;
    IdmSyntax *word = make_qualified_word(form->as.seq.items, 2u, &chain_end, err);
    if (!word) return NULL;
    if (chain_end != form->as.seq.count) {
        idm_syn_free(word);
        if (err) idm_error_set(err, form->span, "trait requirement expects one trait name");
        return NULL;
    }
    return word;
}

static bool trait_requirement_seen(const IdmTraitRequirementDef *requirements, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(requirements[i].name, name) == 0) return true;
    return false;
}

static bool trait_requirements_push(IdmTraitRequirementDef **requirements, size_t *count, size_t *cap, const char *name, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        IdmTraitRequirementDef *next = realloc(*requirements, next_cap * sizeof(*next));
        if (!next) return idm_error_oom(err, span);
        *requirements = next;
        *cap = next_cap;
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

static bool install_required_trait_methods(ExpandContext *ctx, const TraitDef *required, const IdmSyntax *name_syntax, IdmError *err) {
    IdmScopeSet method_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &method_scopes)) return idm_error_oom(err, name_syntax->span);
    bool ok = true;
    for (size_t i = 0; i < required->method_count && ok; i++) {
        ok = install_method_surface(ctx, required->name, required->methods[i].name, required->methods[i].arity, &method_scopes, ctx->unit, ctx->unit_key, err);
    }
    idm_scope_set_destroy(&method_scopes);
    return ok;
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
        if (syn_is_word(item, "->") || syn_is_form(item, "%-body")) break;
        arity++;
    }
    if (arity > UINT32_MAX) return idm_error_set(err, form->span, "method arity is too large");
    *out_arity = (uint32_t)arity;
    return true;
}

static IdmTraitMethodDef *find_decl_method(ExpandContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->decl_method_count; i++) if (strcmp(ctx->decl_methods[i].name, name) == 0) return &ctx->decl_methods[i];
    return NULL;
}

static bool add_decl_method(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t arity, IdmError *err) {
    if (find_decl_method(ctx, name_syntax->as.text)) return idm_error_set(err, name_syntax->span, "trait '%s' declares method '%s' more than once", ctx->trait_name ? ctx->trait_name : "<anonymous>", name_syntax->as.text);
    if (ctx->decl_method_count == ctx->decl_method_cap) {
        size_t cap = ctx->decl_method_cap ? ctx->decl_method_cap * 2u : 4u;
        IdmTraitMethodDef *next = realloc(ctx->decl_methods, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, name_syntax->span);
        ctx->decl_methods = next;
        ctx->decl_method_cap = cap;
    }
    IdmTraitMethodDef *method = &ctx->decl_methods[ctx->decl_method_count];
    memset(method, 0, sizeof(*method));
    method->name = idm_strdup(name_syntax->as.text);
    method->arity = arity;
    method->exported = true;
    if (!method->name || !binder_scopes_pruned(ctx, name_syntax, &method->scopes)) {
        idm_trait_method_def_destroy(method);
        return idm_error_oom(err, name_syntax->span);
    }
    ctx->decl_method_count++;
    return install_method_surface(ctx, ctx->trait_name ? ctx->trait_name : "<anonymous>", method->name, arity, &method->scopes, ctx->unit, ctx->unit_key, err);
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
        uint32_t arity = 0;
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
    uint32_t arity = 0;
    if (!method_signature_arity(form, &arity, err)) return NULL;
    IdmTraitMethodDef *method = find_decl_method(ctx, name->as.text);
    if (!method) {
        if (!add_decl_method(ctx, name, arity, err)) return NULL;
        method = find_decl_method(ctx, name->as.text);
    }
    if (!method) return (IdmCore *)(uintptr_t)idm_error_oom(err, name->span);
    if (method->seen_decl) return expand_error(err, name->span, "trait '%s' declares method '%s' more than once", ctx->trait_name, name->as.text);
    if (method->arity != arity) return expand_error(err, name->span, "method '%s.%s' arity changed during declaration", ctx->trait_name, name->as.text);
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
        free(impls[i].name);
        idm_core_free(impls[i].fn);
    }
    free(impls);
}

static const IdmTraitMethodDef *find_trait_method_def(const IdmTraitMethodDef *methods, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(methods[i].name, name) == 0) return &methods[i];
    return NULL;
}

static bool trait_has_impl(const TraitMethodImpl *impls, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) if (strcmp(impls[i].name, name) == 0) return true;
    return false;
}

static bool trait_impls_push(TraitMethodImpl **impls, size_t *count, size_t *cap, const char *name, uint32_t arity, IdmCore *fn, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        TraitMethodImpl *next = realloc(*impls, next_cap * sizeof(*next));
        if (!next) return idm_error_oom(err, span);
        *impls = next;
        *cap = next_cap;
    }
    TraitMethodImpl *impl = &(*impls)[(*count)++];
    impl->name = idm_strdup(name);
    impl->arity = arity;
    impl->fn = fn;
    if (!impl->name) {
        (*count)--;
        return idm_error_oom(err, span);
    }
    return true;
}

static IdmCore *build_trait_implement_core(ExpandContext *ctx, const char *trait, const char *type, const IdmTraitMethodDef *methods, size_t method_count, const IdmSyntax *body, IdmSpan span, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool surface_ok = true;
    for (size_t i = 0; i < method_count && surface_ok; i++) surface_ok = install_method_surface(ctx, trait, methods[i].name, methods[i].arity, &ctx->empty_scopes, ctx->unit, ctx->unit_key, err);
    if (!surface_ok) {
        surface_rollback(ctx, &checkpoint);
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
        uint32_t arity = 0;
        if (!method_signature_arity(stmt, &arity, err)) { ok = false; break; }
        const IdmTraitMethodDef *contract = find_trait_method_def(methods, method_count, name->as.text);
        if (!contract) { expand_error(err, name->span, "trait '%s' has no method '%s'", trait, name->as.text); ok = false; break; }
        if (contract->arity != arity) { expand_error(err, name->span, "method '%s.%s' expects %u argument(s), got %u", trait, name->as.text, contract->arity, arity); ok = false; break; }
        if (trait_has_impl(impls, impl_count, name->as.text)) { expand_error(err, name->span, "implement for '%s' provides method '%s' more than once", trait, name->as.text); ok = false; break; }
        IdmCore *fn = expand_function_literal(ctx, name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
        if (!fn) { ok = false; break; }
        if (!trait_impls_push(&impls, &impl_count, &impl_cap, name->as.text, arity, fn, err, name->span)) {
            idm_core_free(fn);
            ok = false;
            break;
        }
    }
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        trait_impls_destroy(impls, impl_count);
        return NULL;
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!methods[i].has_default && !trait_has_impl(impls, impl_count, methods[i].name)) {
            expand_error(err, body ? body->span : span, "implement for '%s' on type '%s' is missing required method '%s'", trait, type, methods[i].name);
            trait_impls_destroy(impls, impl_count);
            return NULL;
        }
    }
    IdmCore *core = idm_core_implement_trait(idm_atom(ctx->rt, trait), idm_atom(ctx->rt, type), idm_atom(ctx->rt, ctx->unit), idm_atom(ctx->rt, ctx->unit_key), span);
    if (!core) {
        trait_impls_destroy(impls, impl_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < impl_count; i++) {
        IdmCore *fn = impls[i].fn;
        impls[i].fn = NULL;
        if (!idm_core_implement_trait_add_impl(core, idm_atom(ctx->rt, impls[i].name), impls[i].arity, fn)) {
            idm_core_free(fn);
            idm_core_free(core);
            trait_impls_destroy(impls, impl_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    trait_impls_destroy(impls, impl_count);
    return core;
}

static IdmCore *protocol_decl_core(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *name_syntax, const IdmSyntax *body, bool activate, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    const char *name = name_syntax->as.text;
    unsigned char hash[32];
    char *identity = scoped_identity(ctx, name, form, hash, err);
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
    char **global_names = NULL;
    if (global_count != 0) {
        global_src = malloc(global_count * sizeof(*global_src));
        global_dst = malloc(global_count * sizeof(*global_dst));
        global_names = calloc(global_count, sizeof(*global_names));
        if (!global_src || !global_dst || !global_names) {
            free(global_src);
            free(global_dst);
            free(global_names);
            if (module) { idm_bc_destroy(module); free(module); }
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
    }
    for (size_t i = 0; i < global_count; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        global_src[i] = globals[i].slot;
        global_dst[i] = consumer_slot;
        global_names[i] = idm_strdup(globals[i].name);
        if (!global_names[i]) {
            free(global_src);
            free(global_dst);
            import_names_free(global_names, global_count);
            if (module) { idm_bc_destroy(module); free(module); }
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
        if (!idm_binding_table_add_with_arity(&ctx->bindings, globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &globals[i].scopes, consumer_slot, IDM_FRAME_GLOBAL, globals[i].arity, NULL)) {
            free(global_src);
            free(global_dst);
            import_names_free(global_names, global_count);
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
            import_names_free(global_names, global_count);
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
        import_names_free(global_names, global_count);
        if (module) { idm_bc_destroy(module); free(module); }
        return NULL;
    }
    IdmCore *runtime = idm_core_use_package(idm_atom(ctx->rt, runtime_name), module, init_fn, global_src, global_dst, global_names, global_count, cont, form->span);
    if (!runtime) {
        free(global_src);
        free(global_dst);
        import_names_free(global_names, global_count);
        if (module) { idm_bc_destroy(module); free(module); }
        idm_core_free(cont);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    return runtime;
}

IdmCore *expand_protocol_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    IdmSyntax *const *fitems = form->as.seq.items;
    if (form->as.seq.count != 4 || fitems[2]->kind != IDM_SYN_WORD || !syn_is_form(fitems[3], "%-body"))
        return expand_error(err, form->span, "protocol expects 'protocol NAME do ... end'");
    return protocol_decl_core(ctx, form, fitems[2], fitems[3], false, items, index, count, err);
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
    ctx->trait_name = identity;
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
        } else if (trait_requirement_seen(requirements, requirement_count, required->name)) {
            expand_error(err, required_name->span, "trait '%s' requires trait '%s' more than once", identity, required->name);
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
        if (syn_is_form(stmt, "%-expr") && stmt->as.seq.count >= 2u && syn_is_word(stmt->as.seq.items[1], "require")) continue;
        const IdmSyntax *method_name = NULL;
        size_t param_start = 0;
        bool has_body = false;
        if (!method_form_parts(stmt, &method_name, &param_start, &has_body, err)) {
            if (!(err && err->present)) expand_error(err, stmt->span, "trait bodies may contain only method declarations");
            ok = false;
            break;
        }
        uint32_t arity = 0;
        if (!method_signature_arity(stmt, &arity, err)) { ok = false; break; }
        IdmTraitMethodDef *method = find_decl_method(ctx, method_name->as.text);
        if (!method) { ok = false; idm_error_oom(err, method_name->span); break; }
        if (method->seen_decl) { expand_error(err, method_name->span, "trait '%s' declares method '%s' more than once", identity, method_name->as.text); ok = false; break; }
        if (method->arity != arity) { expand_error(err, method_name->span, "method '%s.%s' arity changed during declaration", identity, method_name->as.text); ok = false; break; }
        method->seen_decl = true;
        if (has_body) {
            IdmCore *fn = expand_function_literal(ctx, method_name->as.text, stmt->as.seq.items[1], stmt->as.seq.items, param_start, stmt->as.seq.count, err);
            if (!fn) { ok = false; break; }
            method->default_fn = fn;
            method->has_default = true;
        }
    }

    size_t method_count = ok ? ctx->decl_method_count - checkpoint.decl_method_count : 0u;
    IdmTraitMethodDef *methods = NULL;
    if (ok && !idm_trait_method_defs_copy(&ctx->decl_methods[checkpoint.decl_method_count], method_count, &methods)) {
        ok = false;
        idm_error_oom(err, form->span);
    }

    IdmCore *define = NULL;
    if (ok) {
        define = idm_core_define_trait(idm_atom(ctx->rt, identity), form->span);
        if (!define) {
            ok = false;
            idm_error_oom(err, form->span);
        }
    }
    for (size_t i = 0; ok && i < requirement_count; i++) {
        if (!idm_core_define_trait_add_requirement(define, idm_atom(ctx->rt, requirements[i].name))) {
            ok = false;
            idm_error_oom(err, form->span);
            break;
        }
    }
    for (size_t i = 0; ok && i < method_count; i++) {
        IdmTraitMethodDef *method = &ctx->decl_methods[checkpoint.decl_method_count + i];
        IdmCore *default_fn = method->default_fn;
        method->default_fn = NULL;
        if (!idm_core_define_trait_add_method(define, idm_atom(ctx->rt, method->name), method->arity, default_fn)) {
            idm_core_free(default_fn);
            ok = false;
            idm_error_oom(err, form->span);
            break;
        }
    }

    ctx->trait_name = saved_trait_name;
    surface_rollback(ctx, &checkpoint);
    if (!ok) {
        idm_core_free(define);
        if (methods) { for (size_t i = 0; i < method_count; i++) idm_trait_method_def_destroy(&methods[i]); free(methods); }
        trait_requirements_destroy(requirements, requirement_count);
        free(identity);
        return NULL;
    }

    if (ctx->trait_count == ctx->trait_cap) {
        size_t cap = ctx->trait_cap ? ctx->trait_cap * 2u : 4u;
        TraitDef *next = realloc(ctx->traits, cap * sizeof(*next));
        if (!next) {
            idm_core_free(define);
            for (size_t i = 0; i < method_count; i++) idm_trait_method_def_destroy(&methods[i]);
            free(methods);
            trait_requirements_destroy(requirements, requirement_count);
            free(identity);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
        }
        ctx->traits = next;
        ctx->trait_cap = cap;
    }
    SurfaceCheckpoint install_checkpoint;
    surface_checkpoint(ctx, &install_checkpoint);
    TraitDef *trait = &ctx->traits[ctx->trait_count];
    memset(trait, 0, sizeof(*trait));
    trait->name = identity;
    trait->exported = exported && ctx->in_package;
    (void)hash;
    trait->requirements = requirements;
    trait->requirement_count = requirement_count;
    trait->methods = methods;
    trait->method_count = method_count;
    uint32_t payload = (uint32_t)ctx->trait_count;
    ctx->trait_count++;
    IdmScopeSet trait_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &trait_scopes)) {
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(define);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (!idm_binding_table_add(&ctx->bindings, name_syntax->as.text, IDM_PHASE_ANY, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, &trait_scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&trait_scopes);
        surface_rollback(ctx, &install_checkpoint);
        idm_core_free(define);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    for (size_t i = 0; i < method_count; i++) {
        if (!install_method_surface(ctx, identity, methods[i].name, methods[i].arity, &trait_scopes, ctx->unit, ctx->unit_key, err)) {
            idm_scope_set_destroy(&trait_scopes);
            surface_rollback(ctx, &install_checkpoint);
            idm_core_free(define);
            return NULL;
        }
    }
    idm_scope_set_destroy(&trait_scopes);

    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        idm_core_free(define);
        return NULL;
    }
    return sequence_two(define, cont, form->span, err);
}

IdmCore *expand_activate(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmSyntax *const *items, size_t index, size_t count, IdmSpan span, IdmError *err) {
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
            idm_error_set(err, span, "activate: '%s' is not a protocol or package", name);
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
    bool import_globals = art->global_count != 0 && !runtime_globals_imported(ctx, art);
    size_t import_count = import_globals ? art->global_count : 0u;
    uint32_t *global_src = NULL;
    uint32_t *global_dst = NULL;
    char **global_names = NULL;
    if (import_count != 0) {
        global_src = malloc(import_count * sizeof(*global_src));
        global_dst = malloc(import_count * sizeof(*global_dst));
        global_names = calloc(import_count, sizeof(*global_names));
        if (!global_src || !global_dst || !global_names) {
            free(global_src);
            free(global_dst);
            free(global_names);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    for (size_t i = 0; i < import_count; i++) {
        uint32_t consumer_slot = ctx->global_seq++;
        global_src[i] = art->globals[i].slot;
        global_dst[i] = consumer_slot;
        global_names[i] = idm_strdup(art->globals[i].name);
        if (!global_names[i]) {
            free(global_src);
            free(global_dst);
            import_names_free(global_names, import_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        IdmScopeSet gscopes;
        idm_scope_set_init(&gscopes);
        bool ok = idm_scope_set_copy(&gscopes, &art->globals[i].scopes);
        if (ok) {
            idm_scope_set_relocate(&gscopes, min_id, delta);
            ok = idm_binding_table_add_with_arity(&ctx->bindings, art->globals[i].name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &gscopes, consumer_slot, IDM_FRAME_GLOBAL, art->globals[i].arity, NULL);
        }
        idm_scope_set_destroy(&gscopes);
        if (!ok) {
            free(global_src);
            free(global_dst);
            import_names_free(global_names, import_count);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    if (import_globals && !record_runtime_globals_import(ctx, art, span, err)) {
        free(global_src);
        free(global_dst);
        import_names_free(global_names, import_count);
        return NULL;
    }
    bool init_pending = artifact_init_pending(ctx, art);
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        free(global_src);
        free(global_dst);
        import_names_free(global_names, import_count);
        return NULL;
    }
    if (!init_pending && import_count == 0) return cont;
    uint32_t fn_off = 0;
    IdmBytecodeModule *module = NULL;
    if (init_pending) {
        module = relocated_module_copy(ctx, art->module, min_id, delta, &fn_off, err);
        if (!module) {
            free(global_src);
            free(global_dst);
            import_names_free(global_names, import_count);
            idm_core_free(cont);
            return NULL;
        }
    }
    IdmCore *runtime = idm_core_use_package(idm_atom(ctx->rt, art->name ? art->name : name), module, art->init_fn + fn_off, global_src, global_dst, global_names, import_count, cont, span);
    if (!runtime) {
        free(global_src);
        free(global_dst);
        import_names_free(global_names, import_count);
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

static IdmCore *implement_trait_decl_core(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    bool has_body = form->as.seq.count == 6;
    if ((form->as.seq.count != 5 && form->as.seq.count != 6) ||
        form->as.seq.items[2]->kind != IDM_SYN_WORD ||
        !syn_is_word(form->as.seq.items[3], "on") ||
        form->as.seq.items[4]->kind != IDM_SYN_WORD ||
        (has_body && !syn_is_form(form->as.seq.items[5], "%-body"))) {
        return expand_error(err, form->span, "implement expects 'implement TRAIT on TYPE' or 'implement TRAIT on TYPE do ... end'");
    }
    const char *trait_name = form->as.seq.items[2]->as.text;
    const char *type_name = form->as.seq.items[4]->as.text;
    IdmResolveStatus type_status = IDM_RESOLVE_UNBOUND;
    TypeDef *type_def = resolve_type_def(ctx, form->as.seq.items[4], &type_status);
    if (type_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, form->as.seq.items[2]->span, "ambiguous type '%s'", type_name);
    if (type_def) type_name = type_def->identity;
    const IdmTraitMethodDef *methods = NULL;
    size_t method_count = 0;

    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    TraitDef *trait = resolve_trait_def(ctx, form->as.seq.items[2], &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, form->as.seq.items[2]->span, "ambiguous trait '%s'", trait_name);
    if (!trait) return expand_error(err, form->as.seq.items[2]->span, "unbound trait '%s'", trait_name);
    trait_name = trait->name;
    methods = trait->methods;
    method_count = trait->method_count;

    const IdmSyntax *body = has_body ? form->as.seq.items[5] : NULL;
    IdmCore *implement_core = build_trait_implement_core(ctx, trait_name, type_name, methods, method_count, body, form->span, err);
    if (!implement_core) return NULL;
    IdmScopeSet method_scopes;
    if (!binder_scopes_pruned(ctx, form->as.seq.items[2], &method_scopes)) {
        idm_core_free(implement_core);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    bool methods_active = true;
    for (size_t i = 0; i < method_count && methods_active; i++) {
        methods_active = install_method_surface(ctx, trait_name, methods[i].name, methods[i].arity, &method_scopes, ctx->unit, ctx->unit_key, err);
    }
    idm_scope_set_destroy(&method_scopes);
    if (!methods_active) {
        idm_core_free(implement_core);
        return NULL;
    }
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) { idm_core_free(implement_core); return NULL; }
    return sequence_two(implement_core, cont, form->span, err);
}

IdmCore *expand_implement_trait_decl(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    bool dotted = false;
    for (size_t i = 2; i + 2u < form->as.seq.count && !dotted; i++) {
        dotted = form->as.seq.items[i]->kind == IDM_SYN_WORD && syn_is_word(form->as.seq.items[i + 1u], ".") && form->as.seq.items[i + 2u]->kind == IDM_SYN_WORD;
    }
    if (!dotted) return implement_trait_decl_core(ctx, form, items, index, count, err);
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
    IdmCore *core = implement_trait_decl_core(ctx, folded, items, index, count, err);
    idm_syn_free(folded);
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
        if (!syn_is_form(stmt, "%-expr") || stmt->as.seq.count != 3u || !syn_is_word(stmt->as.seq.items[1], "field") || stmt->as.seq.items[2]->kind != IDM_SYN_WORD) {
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

static IdmCore *make_prim_call(IdmPrimitive primitive, IdmSpan span, IdmError *err) {
    IdmCore *callee = idm_core_primitive(primitive, span);
    IdmCore *call = callee ? idm_core_call(callee, span) : NULL;
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, span);
        return NULL;
    }
    return call;
}

static bool core_call_add_or_oom(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span) {
    if (!arg || !idm_core_call_add_arg(call, arg)) {
        idm_core_free(arg);
        idm_error_oom(err, span);
        return false;
    }
    return true;
}

static IdmCore *record_field_default_fn(ExpandContext *ctx, const char *field, IdmSpan span, IdmError *err) {
    IdmCore *call = make_prim_call(IDM_PRIM_RECORD_FIELD, span, err);
    if (!call) return NULL;
    if (!core_call_add_or_oom(call, idm_core_arg_ref("record", 0u, span), err, span) ||
        !core_call_add_or_oom(call, idm_core_literal(idm_atom(ctx->rt, field), span), err, span)) {
        idm_core_free(call);
        return NULL;
    }
    return idm_core_fn(field, 1u, call, span);
}

static IdmCore *record_constructor_fn(ExpandContext *ctx, const char *record_name, const char *identity, char **fields, size_t field_count, IdmSpan span, IdmError *err) {
    IdmCore *dict = make_prim_call(IDM_PRIM_DICT, span, err);
    if (!dict) return NULL;
    for (size_t i = 0; i < field_count; i++) {
        if (!core_call_add_or_oom(dict, idm_core_literal(idm_atom(ctx->rt, fields[i]), span), err, span) ||
            !core_call_add_or_oom(dict, idm_core_arg_ref(fields[i], (uint32_t)i, span), err, span)) {
            idm_core_free(dict);
            return NULL;
        }
    }
    IdmCore *make = make_prim_call(IDM_PRIM_RECORD_NEW, span, err);
    if (!make) { idm_core_free(dict); return NULL; }
    if (!core_call_add_or_oom(make, idm_core_literal(idm_atom(ctx->rt, identity), span), err, span) ||
        !core_call_add_or_oom(make, dict, err, span)) {
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
    IdmCore *call = make_prim_call(IDM_PRIM_IS_A_P, span, err);
    if (!call) return NULL;
    if (!core_call_add_or_oom(call, idm_core_arg_ref("value", 0u, span), err, span) ||
        !core_call_add_or_oom(call, idm_core_literal(idm_atom(ctx->rt, identity), span), err, span)) {
        idm_core_free(call);
        return NULL;
    }
    return idm_core_fn(predicate_name, 1u, call, span);
}

static char *record_predicate_name(const char *record_name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_buf_append(&buf, record_name) || !idm_buf_append_char(&buf, '?')) { idm_buf_destroy(&buf); return NULL; }
    return idm_buf_take(&buf);
}

static bool register_record_type_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *record_name, char *identity, bool exported, char **fields, size_t field_count, IdmSpan span, IdmCore **out_define, IdmCore **out_implement, IdmError *err) {
    if (ctx->type_count == ctx->type_cap) {
        size_t cap = ctx->type_cap ? ctx->type_cap * 2u : 4u;
        TypeDef *next = realloc(ctx->types, cap * sizeof(*next));
        if (!next) { free(identity); return idm_error_oom(err, span); }
        ctx->types = next;
        ctx->type_cap = cap;
    }
    TypeDef *type = &ctx->types[ctx->type_count];
    memset(type, 0, sizeof(*type));
    type->name = idm_strdup(record_name);
    type->identity = identity;
    type->exported = exported;
    if (!type->name) { type_def_destroy(type); return idm_error_oom(err, span); }
    if (field_count != 0) {
        type->fields = calloc(field_count, sizeof(*type->fields));
        if (!type->fields) { type_def_destroy(type); return idm_error_oom(err, span); }
        type->field_count = field_count;
        for (size_t i = 0; i < field_count; i++) {
            type->fields[i] = idm_strdup(fields[i]);
            if (!type->fields[i]) { type_def_destroy(type); return idm_error_oom(err, span); }
        }
    }

    IdmCore *define = idm_core_define_trait(idm_atom(ctx->rt, identity), span);
    IdmCore *implement = idm_core_implement_trait(idm_atom(ctx->rt, identity), idm_atom(ctx->rt, identity), idm_atom(ctx->rt, ctx->unit), idm_atom(ctx->rt, ctx->unit_key), span);
    if (!define || !implement) { idm_core_free(define); idm_core_free(implement); type_def_destroy(type); return idm_error_oom(err, span); }
    for (size_t i = 0; i < field_count; i++) {
        IdmScopeSet method_scopes;
        idm_scope_set_init(&method_scopes);
        if (!binder_scopes_pruned(ctx, name_syntax, &method_scopes)) {
            idm_scope_set_destroy(&method_scopes);
            idm_core_free(define);
            idm_core_free(implement);
            type_def_destroy(type);
            return idm_error_oom(err, span);
        }
        IdmCore *default_fn = record_field_default_fn(ctx, fields[i], span, err);
        if (!default_fn || !idm_core_define_trait_add_method(define, idm_atom(ctx->rt, fields[i]), 1u, default_fn)) {
            idm_core_free(default_fn);
            idm_core_free(define);
            idm_core_free(implement);
            idm_scope_set_destroy(&method_scopes);
            type_def_destroy(type);
            return false;
        }
        if (!install_method_surface(ctx, identity, fields[i], 1u, &method_scopes, ctx->unit, ctx->unit_key, err)) {
            idm_core_free(define);
            idm_core_free(implement);
            idm_scope_set_destroy(&method_scopes);
            type_def_destroy(type);
            return false;
        }
        idm_scope_set_destroy(&method_scopes);
    }
    if (!binder_scopes_pruned(ctx, name_syntax, &type->scopes)) {
        idm_core_free(define);
        idm_core_free(implement);
        type_def_destroy(type);
        return idm_error_oom(err, span);
    }
    uint32_t payload = (uint32_t)ctx->type_count;
    if (!idm_binding_table_add(&ctx->bindings, record_name, IDM_PHASE_ANY, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, &type->scopes, payload, ctx->frame, NULL)) {
        idm_core_free(define);
        idm_core_free(implement);
        type_def_destroy(type);
        return idm_error_oom(err, span);
    }
    ctx->type_count++;
    *out_define = define;
    *out_implement = implement;
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
    char *owned_identity = predicate_name ? scoped_identity(ctx, record_name, form, hash, err) : NULL;
    if (!predicate_name || !owned_identity) {
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        if (err && !err->present) idm_error_oom(err, name_syntax->span);
        return NULL;
    }
    const char *identity = owned_identity;

    IdmCore *define_trait = NULL;
    IdmCore *implement_trait = NULL;
    if (!register_record_type_surface(ctx, name_syntax, record_name, owned_identity, exported && ctx->in_package, fields, field_count, form->span, &define_trait, &implement_trait, err)) {
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }

    bool top_level = ctx->frame == IDM_FRAME_TOP;
    size_t saved_count = ctx->bindings.count;
    uint32_t saved_next = ctx->next_slot;
    uint32_t constructor_slot = 0;
    uint32_t predicate_slot = 0;
    if (field_count > UINT32_MAX) {
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return expand_error(err, name_syntax->span, "record has too many fields");
    }
    IdmArity constructor_arity = idm_arity_exact((uint32_t)field_count);
    IdmArity predicate_arity = idm_arity_exact(1u);
    bool ok = top_level ? global_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_slot) : local_push_def_binder_with_arity(ctx, record_name, name_syntax, constructor_arity, &constructor_slot);
    IdmSyntax *predicate_syntax = NULL;
    if (ok) {
        predicate_syntax = idm_syn_word(predicate_name, name_syntax->span);
        if (!predicate_syntax) ok = false;
    }
    if (ok) {
        const IdmScopeSet *scopes = idm_syn_scope_set(name_syntax, 0);
        if (scopes) for (size_t i = 0; i < scopes->count && ok; i++) ok = idm_syn_scope_add(predicate_syntax, 0, scopes->items[i]);
    }
    if (ok) ok = top_level ? global_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_slot) : local_push_def_binder_with_arity(ctx, predicate_name, predicate_syntax, predicate_arity, &predicate_slot);
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, name_syntax->span);
    }
    if (top_level && ctx->in_package) {
        IdmScopeSet record_scopes;
        idm_scope_set_init(&record_scopes);
        if (!syntax_scopes_copy(&record_scopes, name_syntax)) ok = false;
        if (ok && !record_package_global(ctx, record_name, constructor_slot, &record_scopes, constructor_arity)) ok = false;
        if (ok && !record_package_global(ctx, predicate_name, predicate_slot, &record_scopes, predicate_arity)) ok = false;
        if (ok && exported && !record_export(ctx, record_name, constructor_slot, constructor_arity)) ok = false;
        if (ok && exported && !record_export(ctx, predicate_name, predicate_slot, predicate_arity)) ok = false;
        idm_scope_set_destroy(&record_scopes);
    }
    if (!ok) {
        if (!top_level) local_pop_to(ctx, saved_count, saved_next);
        idm_syn_free(predicate_syntax);
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
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
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *cont = expand_body_items(ctx, items, index + 1u, count, false, err);
    if (!cont) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return NULL;
    }
    IdmCore *letrec = idm_core_letrec(form->span);
    if (!letrec) {
        idm_syn_free(predicate_syntax);
        idm_core_free(constructor);
        idm_core_free(predicate);
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
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
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
        free(predicate_name);
        record_field_names_destroy(fields, field_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    IdmCore *seq = idm_core_do(form->span);
    bool seq_ok = seq != NULL;
    if (seq_ok) { seq_ok = idm_core_do_add(seq, define_trait); if (seq_ok) define_trait = NULL; }
    if (seq_ok) { seq_ok = idm_core_do_add(seq, implement_trait); if (seq_ok) implement_trait = NULL; }
    if (seq_ok) { seq_ok = idm_core_do_add(seq, letrec); if (seq_ok) letrec = NULL; }
    if (!seq_ok) {
        idm_syn_free(predicate_syntax);
        idm_core_free(seq);
        idm_core_free(define_trait);
        idm_core_free(implement_trait);
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
