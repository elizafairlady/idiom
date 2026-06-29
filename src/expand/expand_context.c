#include "internal.h"

#include "idiom/reader.h"

static void capture_bindings_destroy(CaptureBinding *captures, size_t count);
static void method_surface_def_destroy(MethodSurfaceDef *method);
static void typed_entity_destroy(TypedEntity *entity);
static void core_syntax_def_destroy(CoreSyntaxDef *core_syntax);
static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap);
static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity);
static int saved_materialize(SavedFunctionContext *g, const IdmSyntax *word, const IdmBinding *b);
static const IdmOperatorDef *op_lookup_capture(const ExpandContext *ctx, const IdmSyntax *syn, const char *capture);
static const IdmBinding *resolve_surface_binding(const ExpandContext *ctx, const IdmSyntax *word, IdmBindingSpace space, IdmBindingKind kind, IdmResolveStatus *out_status);
static void core_form_fn_free(void *data);

void ctx_init(ExpandContext *ctx, IdmRuntime *rt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rt = rt;
    idm_binding_table_init(&ctx->bindings);
    idm_binding_table_set_data_free(&ctx->bindings, core_form_fn_free);
    idm_scope_set_init(&ctx->empty_scopes);
    ctx->frame = 1u;
    ctx->frame_seq = 1u;
    ctx->local_runner.user = ctx;
    ctx->local_runner.invoke = local_macro_invoke;
    idm_scope_store_init(&ctx->scope_store);
    ctx->surface_phase = -1;
    ctx->unit = "<unit>";
    memcpy(ctx->unit_key, "0000000000000000", sizeof ctx->unit_key);
}

void ctx_set_unit(ExpandContext *ctx, const char *name, const unsigned char hash[32]) {
    ctx->unit = name;
    artifact_provider_key(hash, ctx->unit_key);
}

void hooks_install(IdmRuntime *rt, ExpandContext *ctx, SavedHooks *saved) {
    if (saved) {
        saved->local_expand_user = rt->local_expand_user;
        saved->local_expand = rt->local_expand;
        saved->free_identifier_eq_user = rt->free_identifier_eq_user;
        saved->free_identifier_eq = rt->free_identifier_eq;
        saved->register_macro_user = rt->register_macro_user;
        saved->register_macro = rt->register_macro;
    }
    rt->local_expand_user = ctx;
    rt->local_expand = local_expand_callback;
    rt->free_identifier_eq_user = ctx;
    rt->free_identifier_eq = free_identifier_eq_callback;
    rt->register_macro_user = ctx;
    rt->register_macro = register_macro_callback;
}

void hooks_restore(IdmRuntime *rt, const SavedHooks *saved) {
    rt->local_expand_user = saved->local_expand_user;
    rt->local_expand = saved->local_expand;
    rt->free_identifier_eq_user = saved->free_identifier_eq_user;
    rt->free_identifier_eq = saved->free_identifier_eq;
    rt->register_macro_user = saved->register_macro_user;
    rt->register_macro = saved->register_macro;
}

static void capture_bindings_destroy(CaptureBinding *captures, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(captures[i].name);
        idm_scope_set_destroy(&captures[i].scopes);
    }
    free(captures);
}

void macro_def_destroy(MacroDef *macro) {
    if (!macro) return;
    phase_syntax_fn_destroy(&macro->fn);
    free(macro->name);
    memset(macro, 0, sizeof(*macro));
}

void phase_syntax_fn_destroy(PhaseSyntaxFn *fn) {
    if (!fn) return;
    idm_module_ref_release(fn->module);
    idm_phase_env_release(fn->phase_env);
    memset(fn, 0, sizeof(*fn));
}

static void core_form_fn_free(void *data) {
    if (!data) return;
    phase_syntax_fn_destroy((PhaseSyntaxFn *)data);
    free(data);
}

static void core_syntax_def_destroy(CoreSyntaxDef *core_syntax) {
    if (!core_syntax) return;
    free(core_syntax->name);
    idm_scope_set_destroy(&core_syntax->scopes);
    phase_syntax_fn_destroy(&core_syntax->fn);
    memset(core_syntax, 0, sizeof(*core_syntax));
}

void grammar_def_destroy(GrammarDef *grammar) {
    if (!grammar) return;
    idm_pkg_grammar_destroy(&grammar->artifact);
    idm_scope_set_destroy(&grammar->binding_scopes);
    free(grammar->provider);
    free(grammar->provider_key);
    memset(grammar, 0, sizeof(*grammar));
}

static void method_surface_def_destroy(MethodSurfaceDef *method) {
    if (!method) return;
    free(method->provider);
    free(method->provider_key);
    free(method->dispatch_env_key);
    idm_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

static void method_impl_def_destroy(MethodImplDef *impl) {
    if (!impl) return;
    free(impl->impl_env_key);
    memset(impl, 0, sizeof(*impl));
}

void protocol_def_destroy(ProtocolDef *protocol) {
    if (!protocol) return;
    free(protocol->name);
    if (protocol->art) {
        idm_artifact_destroy(protocol->art);
        free(protocol->art);
    }
    memset(protocol, 0, sizeof(*protocol));
}

void trait_def_destroy(TraitDef *trait) {
    if (!trait) return;
    free(trait->dispatch_env_key);
    for (size_t i = 0; i < trait->requirement_count; i++) idm_trait_requirement_def_destroy(&trait->requirements[i]);
    free(trait->requirements);
    for (size_t i = 0; i < trait->method_count; i++) trait_method_def_destroy(&trait->methods[i]);
    free(trait->methods);
    memset(trait, 0, sizeof(*trait));
}

void type_def_destroy(TypeDef *type) {
    if (!type) return;
    free(type->name);
    idm_scope_set_destroy(&type->scopes);
    free(type->fields);
    memset(type, 0, sizeof(*type));
}

static void typed_entity_destroy(TypedEntity *entity) {
    if (!entity) return;
    switch (entity->kind) {
        case IDM_TYPED_ENTITY_PROTOCOL:
            protocol_def_destroy(&entity->as.protocol);
            break;
        case IDM_TYPED_ENTITY_TRAIT:
            trait_def_destroy(&entity->as.trait);
            break;
        case IDM_TYPED_ENTITY_TYPE:
            type_def_destroy(&entity->as.type);
            break;
    }
    memset(entity, 0, sizeof(*entity));
}

bool record_activation(ExpandContext *ctx, const char *name, const char *provider, const char *provider_key, IdmSpan span, IdmError *err) {
    const char *record_provider = provider && *provider ? provider : name;
    const char *record_key = provider_key ? provider_key : "";
    for (size_t i = 0; i < ctx->activation_count; i++) {
        if (strcmp(ctx->activations[i].provider ? ctx->activations[i].provider : "", record_provider) == 0 &&
            strcmp(ctx->activations[i].provider_key ? ctx->activations[i].provider_key : "", record_key) == 0) {
            return true;
        }
    }
    if (ctx->activation_count == ctx->activation_cap) {
        if (!idm_grow((void **)&ctx->activations, &ctx->activation_cap, sizeof(*ctx->activations), 4u, ctx->activation_count + 1u)) return idm_error_oom(err, span);
    }
    ctx->activations[ctx->activation_count].name = idm_strdup(name);
    ctx->activations[ctx->activation_count].provider = idm_strdup(record_provider);
    ctx->activations[ctx->activation_count].provider_key = idm_strdup(record_key);
    if (!ctx->activations[ctx->activation_count].name ||
        !ctx->activations[ctx->activation_count].provider ||
        !ctx->activations[ctx->activation_count].provider_key) {
        free(ctx->activations[ctx->activation_count].name);
        free(ctx->activations[ctx->activation_count].provider);
        free(ctx->activations[ctx->activation_count].provider_key);
        memset(&ctx->activations[ctx->activation_count], 0, sizeof(ctx->activations[ctx->activation_count]));
        return idm_error_oom(err, span);
    }
    ctx->activations[ctx->activation_count].span = span;
    ctx->activation_count++;
    return true;
}

static void surface_install_destroy(SurfaceInstall *install) {
    free(install->name);
    free(install->provider);
    free(install->provider_key);
    idm_scope_set_destroy(&install->scopes);
}

int surface_install_guard(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, const IdmScopeSet *scopes, IdmError *err) {
    bool local = strcmp(provider_key, ctx->unit_key) == 0;
    int phase = ctx->surface_phase >= 0 ? ctx->surface_phase : ctx->phase;
    for (size_t i = 0; i < ctx->surface_install_count; i++) {
        SurfaceInstall *e = &ctx->surface_installs[i];
        if (e->space != space || e->phase != phase || strcmp(e->name, key) != 0 || !idm_scope_set_equal(&e->scopes, scopes)) continue;
        if (local || strcmp(e->provider_key, provider_key) == 0) return local ? 1 : 0;
        idm_error_set(err, idm_span_unknown(NULL), "surface '%s' from '%s' is already active in this context; activating '%s' would conflict", display, e->provider, provider);
        return -1;
    }
    if (ctx->surface_install_count == ctx->surface_install_cap) {
        if (!idm_grow((void **)&ctx->surface_installs, &ctx->surface_install_cap, sizeof(*ctx->surface_installs), 8u, ctx->surface_install_count + 1u)) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return -1;
        }
    }
    SurfaceInstall *e = &ctx->surface_installs[ctx->surface_install_count];
    memset(e, 0, sizeof(*e));
    e->name = idm_strdup(key);
    e->provider = idm_strdup(provider);
    e->provider_key = idm_strdup(provider_key);
    e->space = space;
    e->phase = phase;
    if (!e->name || !e->provider || !e->provider_key || !idm_scope_set_copy(&e->scopes, scopes)) {
        surface_install_destroy(e);
        idm_error_oom(err, idm_span_unknown(NULL));
        return -1;
    }
    ctx->surface_install_count++;
    return 1;
}

int surface_bind_payload(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, IdmSpan span, IdmError *err) {
    int guard = surface_install_guard(ctx, provider, provider_key, key, display, space, scopes, err);
    if (guard <= 0) return guard;
    if (!idm_binding_table_add(&ctx->bindings, key, IDM_PHASE_ANY, space, kind, scopes, payload, ctx->frame, NULL)) {
        idm_error_oom(err, span);
        return -1;
    }
    return 1;
}

int surface_bind_data(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, void *data, IdmSpan span, IdmError *err) {
    int guard = surface_install_guard(ctx, provider, provider_key, key, display, space, scopes, err);
    if (guard <= 0) return guard;
    if (!idm_binding_table_add_data(&ctx->bindings, key, IDM_PHASE_ANY, space, kind, scopes, data, ctx->frame, NULL)) {
        idm_error_oom(err, span);
        return -1;
    }
    return 1;
}

void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint) {
    checkpoint->next_scope = ctx->scope_store.next_scope;
    checkpoint->binding_count = ctx->bindings.count;
    checkpoint->macro_count = ctx->macro_count;
    checkpoint->grammar_count = ctx->grammar_count;
    checkpoint->decl_grammar_count = ctx->decl_grammar_count;
    checkpoint->operator_count = ctx->operator_count;
    checkpoint->typed.entity_count = ctx->typed.entity_count;
    checkpoint->typed.method_surface_count = ctx->typed.method_surface_count;
    checkpoint->decl_method_count = ctx->decl_method_count;
    checkpoint->typed.method_impl_count = ctx->typed.method_impl_count;
    checkpoint->decl_core_syntax_count = ctx->decl_core_syntax_count;
    checkpoint->decl_source_reader_count = ctx->decl_source_reader_count;
    checkpoint->activation_count = ctx->activation_count;
    checkpoint->surface_install_count = ctx->surface_install_count;
    checkpoint->artifact_base_count = ctx->artifact_base_count;
    checkpoint->init_emit_mark_count = ctx->init_emit_mark_count;
    checkpoint->runtime_init_mark_count = ctx->runtime_init_mark_count;
    checkpoint->phase_runtime_init_mark_count = ctx->phase_runtime_init_mark_count;
    checkpoint->package_slot_ref_count = ctx->package_slot_ref_count;
}

void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint) {
    while (ctx->activation_count > checkpoint->activation_count) {
        ctx->activation_count--;
        free(ctx->activations[ctx->activation_count].name);
        free(ctx->activations[ctx->activation_count].provider);
        free(ctx->activations[ctx->activation_count].provider_key);
    }
    while (ctx->surface_install_count > checkpoint->surface_install_count) surface_install_destroy(&ctx->surface_installs[--ctx->surface_install_count]);
    for (size_t i = 0; i < ctx->artifact_base_count && i < checkpoint->artifact_base_count; i++) {
        for (size_t phase = 0; phase < 2u; phase++) {
            if (ctx->artifact_bases[i].init_emit_mark[phase] > checkpoint->init_emit_mark_count) {
                ctx->artifact_bases[i].init_emit_mark[phase] = 0;
            }
            if (ctx->artifact_bases[i].runtime_init_mark[phase] > checkpoint->runtime_init_mark_count) {
                ctx->artifact_bases[i].runtime_init_mark[phase] = 0;
            }
        }
    }
    if (ctx->artifact_base_count > checkpoint->artifact_base_count) ctx->artifact_base_count = checkpoint->artifact_base_count;
    if (ctx->init_emit_mark_count > checkpoint->init_emit_mark_count) ctx->init_emit_mark_count = checkpoint->init_emit_mark_count;
    if (ctx->runtime_init_mark_count > checkpoint->runtime_init_mark_count) ctx->runtime_init_mark_count = checkpoint->runtime_init_mark_count;
    ctx->phase_runtime_init_mark_count = checkpoint->phase_runtime_init_mark_count;
    while (ctx->package_slot_ref_count > checkpoint->package_slot_ref_count) free(ctx->package_slot_refs[--ctx->package_slot_ref_count].env_key);
    while (ctx->grammar_count > checkpoint->grammar_count) grammar_def_destroy(&ctx->grammars[--ctx->grammar_count]);
    while (ctx->macro_count > checkpoint->macro_count) macro_def_destroy(&ctx->macros[--ctx->macro_count]);
    while (ctx->operator_count > checkpoint->operator_count) idm_operator_def_destroy(&ctx->operators[--ctx->operator_count]);
    while (ctx->typed.entity_count > checkpoint->typed.entity_count) typed_entity_destroy(&ctx->typed.entities[--ctx->typed.entity_count]);
    while (ctx->typed.method_surface_count > checkpoint->typed.method_surface_count) method_surface_def_destroy(&ctx->typed.method_surfaces[--ctx->typed.method_surface_count]);
    while (ctx->decl_method_count > checkpoint->decl_method_count) trait_method_def_destroy(&ctx->decl_methods[--ctx->decl_method_count]);
    while (ctx->typed.method_impl_count > checkpoint->typed.method_impl_count) method_impl_def_destroy(&ctx->typed.method_impls[--ctx->typed.method_impl_count]);
    while (ctx->decl_core_syntax_count > checkpoint->decl_core_syntax_count) core_syntax_def_destroy(&ctx->decl_core_syntax[--ctx->decl_core_syntax_count]);
    while (ctx->decl_source_reader_count > checkpoint->decl_source_reader_count) free(ctx->decl_source_readers[--ctx->decl_source_reader_count].name);
    while (ctx->decl_grammar_count > checkpoint->decl_grammar_count) idm_pkg_grammar_destroy(&ctx->decl_grammars[--ctx->decl_grammar_count]);
    idm_binding_table_truncate(&ctx->bindings, checkpoint->binding_count);
}

bool ctx_validate_source_reader_decls(ExpandContext *ctx, IdmError *err) {
    if (ctx->decl_source_reader_count == 0) return true;
    SourceReaderDecl *decl = &ctx->decl_source_readers[ctx->decl_source_reader_count - 1u];
    for (size_t i = 0; i < ctx->decl_grammar_count; i++) {
        if (ctx->decl_grammars[i].name && strcmp(ctx->decl_grammars[i].name, decl->name) == 0) {
            IdmReaderArtifact *reader = NULL;
            bool ok = idm_reader_artifact_from_grammars(decl->name, ctx->decl_grammars, ctx->decl_grammar_count, &reader, err);
            idm_reader_artifact_destroy(reader);
            return ok;
        }
    }
    return idm_error_set(err, decl->span, "core-reader source reader '%s' has no core-grammar declaration", decl->name);
}

void ctx_destroy(ExpandContext *ctx) {
    for (size_t i = 0; i < ctx->activation_count; i++) {
        free(ctx->activations[i].name);
        free(ctx->activations[i].provider);
        free(ctx->activations[i].provider_key);
    }
    free(ctx->activations);
    for (size_t i = 0; i < ctx->surface_install_count; i++) surface_install_destroy(&ctx->surface_installs[i]);
    free(ctx->surface_installs);
    idm_binding_table_destroy(&ctx->bindings);
    idm_scope_set_destroy(&ctx->empty_scopes);
    capture_bindings_destroy(ctx->captures, ctx->capture_count);
    for (size_t i = 0; i < ctx->package_slot_count; i++) idm_pkg_slot_destroy(&ctx->package_slots[i]);
    free(ctx->package_slots);
    for (size_t i = 0; i < ctx->macro_count; i++) macro_def_destroy(&ctx->macros[i]);
    free(ctx->macros);
    for (size_t i = 0; i < ctx->grammar_count; i++) grammar_def_destroy(&ctx->grammars[i]);
    free(ctx->grammars);
    for (size_t i = 0; i < ctx->operator_count; i++) idm_operator_def_destroy(&ctx->operators[i]);
    free(ctx->operators);
    for (size_t i = 0; i < ctx->typed.entity_count; i++) typed_entity_destroy(&ctx->typed.entities[i]);
    free(ctx->typed.entities);
    for (size_t i = 0; i < ctx->typed.method_surface_count; i++) method_surface_def_destroy(&ctx->typed.method_surfaces[i]);
    free(ctx->typed.method_surfaces);
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) method_impl_def_destroy(&ctx->typed.method_impls[i]);
    free(ctx->typed.method_impls);
    for (size_t i = 0; i < ctx->decl_method_count; i++) trait_method_def_destroy(&ctx->decl_methods[i]);
    free(ctx->decl_methods);
    for (size_t i = 0; i < ctx->decl_core_syntax_count; i++) core_syntax_def_destroy(&ctx->decl_core_syntax[i]);
    free(ctx->decl_core_syntax);
    for (size_t i = 0; i < ctx->decl_source_reader_count; i++) free(ctx->decl_source_readers[i].name);
    free(ctx->decl_source_readers);
    for (size_t i = 0; i < ctx->decl_grammar_count; i++) idm_pkg_grammar_destroy(&ctx->decl_grammars[i]);
    free(ctx->decl_grammars);
    idm_phase_env_release(ctx->phase_env);
    free(ctx->artifact_bases);
    for (size_t i = 0; i < ctx->package_slot_ref_count; i++) free(ctx->package_slot_refs[i].env_key);
    free(ctx->package_slot_refs);
    for (size_t i = 0; i < ctx->dep_count; i++) free(ctx->deps[i].path);
    free(ctx->deps);
    free(ctx->pat_binders);
}

IdmCore *expand_primitive_clause_call(IdmPrimitive primitive, IdmSpan span, IdmError *err) {
    IdmCore *callee = idm_core_primitive_backed_fn(idm_primitive_name(primitive), primitive, idm_primitive_arity(primitive), span);
    if (!callee) {
        idm_error_oom(err, span);
        return NULL;
    }
    IdmCore *call = idm_core_call(callee, span);
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, span);
        return NULL;
    }
    return call;
}

IdmCore *expand_raise_message_body(ExpandContext *ctx, const char *message, IdmSpan span, IdmError *err) {
    IdmValue text = idm_string(ctx->rt, message ? message : "", err);
    if (err && err->present) return NULL;
    IdmCore *text_core = idm_core_literal(text, span);
    IdmCore *make_error = text_core ? expand_primitive_clause_call(IDM_PRIM_MAKE_ERROR, span, err) : NULL;
    if (!make_error || !core_call_add_arg_or_free(make_error, text_core, err, span)) {
        idm_core_free(make_error);
        return NULL;
    }
    IdmCore *raise = expand_primitive_clause_call(IDM_PRIM_RAISE, span, err);
    if (!raise || !core_call_add_arg_or_free(raise, make_error, err, span)) {
        idm_core_free(raise);
        return NULL;
    }
    return raise;
}

bool expand_multi_add_dispatch_clause(ExpandContext *ctx, IdmCore *multi, uint32_t argc, const char *arg0_type, ExpandMultiClauseBodyFn body_fn, const void *body_user, IdmSpan span, IdmError *err) {
    IdmPattern **patterns = argc == 0 ? NULL : calloc(argc, sizeof(*patterns));
    if (argc != 0 && !patterns) return idm_error_oom(err, span);
    bool ok = true;
    for (uint32_t i = 0; i < argc && ok; i++) {
        patterns[i] = i == 0 && arg0_type ? idm_pat_type(arg0_type, span) : idm_pat_wildcard(span);
        if (!patterns[i]) ok = false;
    }
    IdmCore *body = ok ? body_fn(ctx, multi, body_user, argc, span, err) : NULL;
    if (!body) ok = false;
    if (ok) ok = idm_core_fn_multi_add_clause_take(multi, argc, patterns, argc, NULL, 0, NULL, body);
    if (!ok) {
        for (uint32_t i = 0; i < argc; i++) idm_pat_free(patterns ? patterns[i] : NULL);
        free(patterns);
        idm_core_free(body);
        if (!err->present) idm_error_oom(err, span);
        return false;
    }
    return true;
}

bool core_call_add_arg_or_free(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span) {
    if (arg) {
        if (idm_core_call_add_arg(call, arg)) return true;
    }
    idm_core_free(arg);
    idm_core_free(call);
    idm_error_oom(err, span);
    return false;
}

char *operator_binding_key(const char *name, const char *capture) {
    IdmBuffer key;
    idm_buf_init(&key);
    if (!idm_buf_append(&key, capture) || !idm_buf_append_char(&key, ':') || !idm_buf_append(&key, name)) {
        idm_buf_destroy(&key);
        return NULL;
    }
    return idm_buf_take(&key);
}

static TypedEntity *typed_entity_by_index(ExpandContext *ctx, uint32_t index, IdmTypedEntityKind kind) {
    if (!ctx || index >= ctx->typed.entity_count) return NULL;
    TypedEntity *entity = &ctx->typed.entities[index];
    return entity->kind == kind ? entity : NULL;
}

static void *typed_registry_add_entity(ExpandContext *ctx, IdmTypedEntityKind kind, uint32_t *out_index, IdmError *err, IdmSpan span) {
    if (ctx->typed.entity_count > UINT32_MAX) {
        idm_error_set(err, span, "too many typed definitions");
        return NULL;
    }
    if (ctx->typed.entity_count == ctx->typed.entity_cap) {
        if (!idm_grow((void **)&ctx->typed.entities, &ctx->typed.entity_cap, sizeof(*ctx->typed.entities), 8u, ctx->typed.entity_count + 1u)) {
            idm_error_oom(err, span);
            return NULL;
        }
    }
    uint32_t index = (uint32_t)ctx->typed.entity_count;
    TypedEntity *entity = &ctx->typed.entities[ctx->typed.entity_count++];
    memset(entity, 0, sizeof(*entity));
    entity->kind = kind;
    if (out_index) *out_index = index;
    switch (kind) {
        case IDM_TYPED_ENTITY_PROTOCOL:
            return &entity->as.protocol;
        case IDM_TYPED_ENTITY_TRAIT:
            return &entity->as.trait;
        case IDM_TYPED_ENTITY_TYPE:
            return &entity->as.type;
    }
    return NULL;
}

ProtocolDef *typed_registry_add_protocol(ExpandContext *ctx, uint32_t *out_index, IdmError *err, IdmSpan span) {
    return typed_registry_add_entity(ctx, IDM_TYPED_ENTITY_PROTOCOL, out_index, err, span);
}

TraitDef *typed_registry_add_trait(ExpandContext *ctx, uint32_t *out_index, IdmError *err, IdmSpan span) {
    return typed_registry_add_entity(ctx, IDM_TYPED_ENTITY_TRAIT, out_index, err, span);
}

TypeDef *typed_registry_add_type(ExpandContext *ctx, uint32_t *out_index, IdmError *err, IdmSpan span) {
    return typed_registry_add_entity(ctx, IDM_TYPED_ENTITY_TYPE, out_index, err, span);
}

ProtocolDef *typed_protocol_by_index(ExpandContext *ctx, uint32_t index) {
    TypedEntity *entity = typed_entity_by_index(ctx, index, IDM_TYPED_ENTITY_PROTOCOL);
    return entity ? &entity->as.protocol : NULL;
}

TraitDef *typed_trait_by_index(ExpandContext *ctx, uint32_t index) {
    TypedEntity *entity = typed_entity_by_index(ctx, index, IDM_TYPED_ENTITY_TRAIT);
    return entity ? &entity->as.trait : NULL;
}

TypeDef *typed_type_by_index(ExpandContext *ctx, uint32_t index) {
    TypedEntity *entity = typed_entity_by_index(ctx, index, IDM_TYPED_ENTITY_TYPE);
    return entity ? &entity->as.type : NULL;
}

static IdmSymbol *typed_identity_lookup(const ExpandContext *ctx, const char *identity) {
    return ctx && ctx->rt && identity ? idm_intern_lookup(&ctx->rt->intern, IDM_SYMBOL_ATOM, identity) : NULL;
}

static IdmSymbol *typed_atom_intern(ExpandContext *ctx, const char *text) {
    return ctx && ctx->rt && text ? idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, text) : NULL;
}

const char *protocol_def_identity_text(const ProtocolDef *protocol) {
    return protocol && protocol->identity ? idm_symbol_text(protocol->identity) : NULL;
}

bool protocol_def_set_identity(ExpandContext *ctx, ProtocolDef *protocol, const char *identity, IdmError *err, IdmSpan span) {
    if (!ctx || !protocol || !identity) return idm_error_set(err, span, "protocol requires an identity");
    IdmSymbol *sym = typed_atom_intern(ctx, identity);
    if (!sym) return idm_error_oom(err, span);
    protocol->identity = sym;
    return true;
}

bool typed_trait_matches_identity(const ExpandContext *ctx, const TraitDef *trait, const char *identity) {
    IdmSymbol *sym = typed_identity_lookup(ctx, identity);
    return trait && trait->name && sym && trait->name == sym;
}

const TraitDef *typed_trait_by_identity(ExpandContext *ctx, const char *identity) {
    if (!ctx || !identity) return NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_TRAIT && typed_trait_matches_identity(ctx, &entity->as.trait, identity)) return &entity->as.trait;
    }
    return NULL;
}

bool typed_type_same_identity(const TypeDef *a, const TypeDef *b) {
    return a && b && a->identity && a->identity == b->identity;
}

const char *trait_def_identity_text(const TraitDef *trait) {
    return trait && trait->name ? idm_symbol_text(trait->name) : NULL;
}

bool trait_def_set_identity(ExpandContext *ctx, TraitDef *trait, const char *identity, IdmError *err, IdmSpan span) {
    if (!ctx || !trait || !identity) return idm_error_set(err, span, "trait requires an identity");
    IdmSymbol *sym = typed_atom_intern(ctx, identity);
    if (!sym) return idm_error_oom(err, span);
    trait->name = sym;
    return true;
}

const char *trait_method_name_text(const TraitMethodDef *method) {
    return method && method->name ? idm_symbol_text(method->name) : NULL;
}

bool trait_method_set_name(ExpandContext *ctx, TraitMethodDef *method, const char *name, IdmError *err, IdmSpan span) {
    if (!ctx || !method || !name) return idm_error_set(err, span, "method requires a name");
    IdmSymbol *sym = typed_atom_intern(ctx, name);
    if (!sym) return idm_error_oom(err, span);
    method->name = sym;
    return true;
}

bool trait_method_matches_name(const ExpandContext *ctx, const TraitMethodDef *method, const char *name) {
    IdmSymbol *sym = typed_identity_lookup(ctx, name);
    return method && method->name && sym && method->name == sym;
}

void trait_method_def_destroy(TraitMethodDef *method) {
    if (!method) return;
    idm_core_free(method->default_fn);
    idm_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

const char *trait_method_impl_name_text(const TraitMethodImpl *impl) {
    return impl && impl->name ? idm_symbol_text(impl->name) : NULL;
}

bool trait_method_impl_set_name(ExpandContext *ctx, TraitMethodImpl *impl, const char *name, IdmError *err, IdmSpan span) {
    if (!ctx || !impl || !name) return idm_error_set(err, span, "method implementation requires a name");
    IdmSymbol *sym = typed_atom_intern(ctx, name);
    if (!sym) return idm_error_oom(err, span);
    impl->name = sym;
    return true;
}

bool trait_method_impl_matches_name(const ExpandContext *ctx, const TraitMethodImpl *impl, const char *name) {
    IdmSymbol *sym = typed_identity_lookup(ctx, name);
    return impl && impl->name && sym && impl->name == sym;
}

bool trait_method_defs_copy(const TraitMethodDef *src, size_t count, TraitMethodDef **out) {
    *out = NULL;
    if (count == 0) return true;
    TraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return false;
    for (size_t i = 0; i < count; i++) {
        methods[i].name = src[i].name;
        methods[i].arity = src[i].arity;
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].dispatch_slot = src[i].dispatch_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
        if (!methods[i].name || !idm_scope_set_copy(&methods[i].scopes, &src[i].scopes)) {
            for (size_t j = 0; j <= i; j++) trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
    }
    *out = methods;
    return true;
}

bool trait_method_defs_import(ExpandContext *ctx, const IdmTraitMethodDef *src, size_t count, TraitMethodDef **out, IdmError *err, IdmSpan span) {
    *out = NULL;
    if (count == 0) return true;
    TraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return idm_error_oom(err, span);
    for (size_t i = 0; i < count; i++) {
        if (!trait_method_set_name(ctx, &methods[i], src[i].name, err, span)) {
            for (size_t j = 0; j <= i; j++) trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
        methods[i].arity = src[i].arity;
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].dispatch_slot = src[i].dispatch_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
        if (!idm_scope_set_copy(&methods[i].scopes, &src[i].scopes)) {
            for (size_t j = 0; j <= i; j++) trait_method_def_destroy(&methods[j]);
            free(methods);
            return idm_error_oom(err, span);
        }
    }
    *out = methods;
    return true;
}

bool trait_method_defs_export(const TraitMethodDef *src, size_t count, IdmTraitMethodDef **out) {
    *out = NULL;
    if (count == 0) return true;
    IdmTraitMethodDef *methods = calloc(count, sizeof(*methods));
    if (!methods) return false;
    for (size_t i = 0; i < count; i++) {
        const char *name = trait_method_name_text(&src[i]);
        methods[i].name = idm_strdup(name);
        methods[i].arity = src[i].arity;
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].dispatch_slot = src[i].dispatch_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
        if (!methods[i].name || !idm_scope_set_copy(&methods[i].scopes, &src[i].scopes)) {
            for (size_t j = 0; j <= i; j++) idm_trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
    }
    *out = methods;
    return true;
}

const char *type_def_identity_text(const TypeDef *type) {
    return type && type->identity ? idm_symbol_text(type->identity) : NULL;
}

IdmSymbol *type_def_identity_symbol(const TypeDef *type) {
    return type ? type->identity : NULL;
}

bool type_def_set_identity(ExpandContext *ctx, TypeDef *type, const char *identity, IdmError *err, IdmSpan span) {
    if (!ctx || !type || !identity) return idm_error_set(err, span, "type requires an identity");
    IdmSymbol *sym = typed_atom_intern(ctx, identity);
    if (!sym) return idm_error_oom(err, span);
    type->identity = sym;
    return true;
}

const char *type_field_name_text(const TypeFieldDef *field) {
    return field && field->name ? idm_symbol_text(field->name) : NULL;
}

const char *type_field_contract_text(const TypeFieldDef *field) {
    return field && field->contract ? idm_symbol_text(field->contract) : NULL;
}

bool type_field_set(ExpandContext *ctx, TypeFieldDef *field, const char *name, const char *contract, IdmError *err, IdmSpan span) {
    if (!ctx || !field || !name || !*name) return idm_error_set(err, span, "record field must be a non-empty name");
    IdmSymbol *name_sym = typed_atom_intern(ctx, name);
    IdmSymbol *contract_sym = contract ? typed_atom_intern(ctx, contract) : NULL;
    if (!name_sym || (contract && !contract_sym)) return idm_error_oom(err, span);
    field->name = name_sym;
    field->contract = contract_sym;
    return true;
}

bool type_field_matches_name(const ExpandContext *ctx, const TypeFieldDef *field, const char *name) {
    IdmSymbol *sym = typed_identity_lookup(ctx, name);
    return field && field->name && sym && field->name == sym;
}

bool register_operator(ExpandContext *ctx, const char *name, const char *capture, uint8_t precedence, IdmOpAssoc assoc, const char *target_name, const IdmScopeSet *scopes, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, bool exported, IdmError *err) {
    if (!capture || !*capture) return idm_error_set(err, idm_span_unknown(NULL), "operator capture must be non-empty");
    if (!target_name || !*target_name) return idm_error_set(err, idm_span_unknown(NULL), "operator target must be non-empty");
    uint8_t capture_kind = IDM_OPERATOR_CAPTURE_INVALID;
    bool capture_left = false;
    uint32_t capture_count = 0;
    if (!operator_capture_compile(capture, &capture_kind, &capture_left, &capture_count)) {
        return idm_error_set(err, idm_span_unknown(NULL), "unsupported operator capture '%s'", capture);
    }
    if (!binding_scopes) binding_scopes = scopes ? scopes : &ctx->empty_scopes;
    char *key = operator_binding_key(name, capture);
    if (!key) return idm_error_oom(err, idm_span_unknown(NULL));
    if (ctx->operator_count == ctx->operator_cap) {
        if (!idm_grow((void **)&ctx->operators, &ctx->operator_cap, sizeof(*ctx->operators), 16u, ctx->operator_count + 1u)) {
            free(key);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    IdmOperatorDef *op = &ctx->operators[ctx->operator_count];
    memset(op, 0, sizeof(*op));
    op->name = idm_strdup(name);
    if (!op->name) {
        free(key);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    op->precedence = precedence;
    op->assoc = assoc;
    op->capture_kind = capture_kind;
    op->capture_left = capture_left;
    op->capture_count = capture_count;
    op->capture = idm_strdup(capture);
    if (!op->capture) {
        free(op->name);
        free(key);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    op->target_name = idm_strdup(target_name);
    if (!op->target_name) {
        free(op->name);
        free(op->capture);
        free(key);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_scope_set_copy(&op->scopes, scopes ? scopes : &ctx->empty_scopes)) {
        free(op->name);
        free(op->capture);
        free(op->target_name);
        free(key);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    uint32_t payload = (uint32_t)ctx->operator_count;
    int bound = surface_bind_payload(ctx, provider, provider_key, key, name, IDM_BIND_SPACE_OPERATOR, IDM_BIND_OPERATOR, binding_scopes, payload, idm_span_unknown(NULL), err);
    if (bound <= 0) {
        free(key);
        idm_operator_def_destroy(op);
        return bound == 0;
    }
    free(key);
    op->exported = exported;
    ctx->operator_count++;
    return true;
}

const MethodSurfaceDef *method_surface_by_index(const ExpandContext *ctx, uint32_t index) {
    if (!ctx || index >= ctx->typed.method_surface_count) return NULL;
    return &ctx->typed.method_surfaces[index];
}

const char *method_surface_trait_text(const MethodSurfaceDef *surface) {
    return surface && surface->trait ? idm_symbol_text(surface->trait) : NULL;
}

const char *method_surface_trait_key_text(const MethodSurfaceDef *surface) {
    return surface && surface->trait_key ? idm_symbol_text(surface->trait_key) : NULL;
}

const char *method_surface_name_text(const MethodSurfaceDef *surface) {
    return surface && surface->name ? idm_symbol_text(surface->name) : NULL;
}

bool method_surface_matches_identity(const ExpandContext *ctx, const MethodSurfaceDef *surface, const char *trait, const char *name) {
    return method_surface_matches_trait(ctx, surface, trait) && method_surface_matches_name(ctx, surface, name);
}

bool method_surface_matches_trait(const ExpandContext *ctx, const MethodSurfaceDef *surface, const char *trait) {
    IdmSymbol *sym = typed_identity_lookup(ctx, trait);
    return surface && surface->trait && sym && surface->trait == sym;
}

bool method_surface_matches_name(const ExpandContext *ctx, const MethodSurfaceDef *surface, const char *name) {
    IdmSymbol *sym = typed_identity_lookup(ctx, name);
    return surface && surface->name && sym && surface->name == sym;
}

bool method_surface_index_by_identity(const ExpandContext *ctx, const char *trait, const char *name, uint32_t *out) {
    if (!ctx || !trait || !name) return false;
    for (size_t i = 0; i < ctx->typed.method_surface_count; i++) {
        const MethodSurfaceDef *surface = &ctx->typed.method_surfaces[i];
        if (method_surface_matches_identity(ctx, surface, trait, name)) {
            if (out) *out = (uint32_t)i;
            return true;
        }
    }
    return false;
}

bool method_surfaces_share_trait_identity(const MethodSurfaceDef *a, const MethodSurfaceDef *b) {
    if (!a || !b) return false;
    if (a->trait && a->trait == b->trait) return true;
    return a->trait_key && a->trait_key == b->trait_key;
}

const char *method_impl_type_text(const MethodImplDef *impl) {
    return impl && impl->type ? idm_symbol_text(impl->type) : NULL;
}

bool method_impl_set_type(ExpandContext *ctx, MethodImplDef *impl, const char *type, IdmError *err, IdmSpan span) {
    if (!ctx || !impl || !type) return idm_error_set(err, span, "method implementation requires a receiver type");
    IdmSymbol *sym = typed_atom_intern(ctx, type);
    if (!sym) return idm_error_oom(err, span);
    impl->type = sym;
    return true;
}

bool method_impl_matches_type(const ExpandContext *ctx, const MethodImplDef *impl, const char *type) {
    IdmSymbol *sym = typed_identity_lookup(ctx, type);
    return impl && impl->type && sym && impl->type == sym;
}

bool method_impl_same_type(const MethodImplDef *a, const MethodImplDef *b) {
    return a && b && a->type && a->type == b->type;
}

bool method_impl_matches_identity(const ExpandContext *ctx, const MethodImplDef *impl, const char *trait, const char *name, const char *type) {
    if (!impl || (type && !method_impl_matches_type(ctx, impl, type))) return false;
    const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl->method_surface);
    return method_surface_matches_identity(ctx, surface, trait, name);
}

bool install_method_surface(ExpandContext *ctx, const char *trait, const char *trait_key, const char *name, IdmArity arity, const IdmScopeSet *scopes, const char *provider, const char *provider_key, bool has_dispatch, bool dispatch_env, const char *dispatch_env_key, uint32_t dispatch_slot, IdmError *err) {
    const IdmScopeSet *check_scopes = scopes ? scopes : &ctx->empty_scopes;
    const char *check_key = trait_key ? trait_key : trait;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *binding = &ctx->bindings.items[i];
        if (binding->space == IDM_BIND_SPACE_FIELD &&
            binding->kind == IDM_BIND_FIELD &&
            strcmp(binding->name, name) == 0 &&
            idm_scope_set_equal(&binding->scopes, check_scopes)) {
            return idm_error_set(err, idm_span_unknown(NULL), "'%s' is declared as both a record field and a trait method in the same scope; dot access would be ambiguous — rename one", name);
        }
    }
    for (size_t i = 0; i < ctx->typed.method_surface_count; i++) {
        const MethodSurfaceDef *e = &ctx->typed.method_surfaces[i];
        if (!method_surface_matches_identity(ctx, e, trait, name) || !idm_scope_set_equal(&e->scopes, check_scopes)) continue;
        {
            if (!idm_arity_equal(&e->arity, &arity)) {
                return idm_error_set(err, idm_span_unknown(NULL), "method surface '%s.%s' arity changed while activating this scope", trait, name);
            }
            const char *e_provider = e->provider ? e->provider : "";
            const char *e_provider_key = e->provider_key ? e->provider_key : "";
            const char *new_provider = provider ? provider : "";
            const char *new_provider_key = provider_key ? provider_key : "";
            if (strcmp(e_provider, new_provider) == 0 && strcmp(e_provider_key, new_provider_key) == 0) return true;
        }
    }
    if (ctx->typed.method_surface_count == ctx->typed.method_surface_cap) {
        if (!idm_grow((void **)&ctx->typed.method_surfaces, &ctx->typed.method_surface_cap, sizeof(*ctx->typed.method_surfaces), 8u, ctx->typed.method_surface_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    MethodSurfaceDef *method = &ctx->typed.method_surfaces[ctx->typed.method_surface_count];
    memset(method, 0, sizeof(*method));
    method->trait = typed_atom_intern(ctx, trait);
    method->trait_key = typed_atom_intern(ctx, check_key);
    method->name = typed_atom_intern(ctx, name);
    method->provider = idm_strdup(provider ? provider : "");
    method->provider_key = idm_strdup(provider_key ? provider_key : "");
    method->dispatch_env_key = idm_strdup(dispatch_env_key ? dispatch_env_key : "");
    method->arity = arity;
    method->has_dispatch = has_dispatch;
    method->dispatch_env = dispatch_env;
    method->dispatch_frame = ctx->frame;
    method->dispatch_slot = dispatch_slot;
    if (!method->trait || !method->trait_key || !method->name || !method->provider || !method->provider_key || !method->dispatch_env_key || !idm_scope_set_copy(&method->scopes, scopes ? scopes : &ctx->empty_scopes)) {
        method_surface_def_destroy(method);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_METHOD, IDM_BIND_METHOD, &method->scopes, (uint32_t)ctx->typed.method_surface_count, ctx->frame, NULL)) {
        method_surface_def_destroy(method);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ctx->typed.method_surface_count++;
    return true;
}

bool install_field_surfaces(ExpandContext *ctx, const TypeDef *type, uint32_t type_index, const IdmScopeSet *scopes, IdmError *err) {
    const IdmScopeSet *check_scopes = scopes ? scopes : &ctx->empty_scopes;
    for (size_t i = 0; i < type->field_count; i++) {
        const char *name = type_field_name_text(&type->fields[i]);
        for (size_t j = 0; j < ctx->typed.method_surface_count; j++) {
            const MethodSurfaceDef *method = &ctx->typed.method_surfaces[j];
            if (method_surface_matches_name(ctx, method, name) && idm_scope_set_equal(&method->scopes, check_scopes)) {
                return idm_error_set(err, idm_span_unknown(NULL), "'%s' is declared as both a record field and a trait method in the same scope; dot access would be ambiguous — rename one", name);
            }
        }
        bool duplicate = false;
        for (size_t j = 0; j < ctx->bindings.count; j++) {
            const IdmBinding *binding = &ctx->bindings.items[j];
            if (binding->space == IDM_BIND_SPACE_FIELD &&
                binding->kind == IDM_BIND_FIELD &&
                binding->payload == type_index &&
                strcmp(binding->name, name) == 0 &&
                idm_scope_set_equal(&binding->scopes, check_scopes)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_FIELD, IDM_BIND_FIELD, check_scopes, type_index, ctx->frame, NULL)) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    return true;
}

IdmResolveStatus resolve_scoped(const ExpandContext *ctx, const char *name, IdmBindingSpace space, const IdmScopeSet *base, const IdmScopeSet *fallback, const IdmBinding **out_binding) {
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = base ? base : &empty;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, name, ctx->phase, space, lookup, out_binding);
    if (status == IDM_RESOLVE_UNBOUND && fallback && fallback->count != 0) {
        IdmScopeSet merged;
        if (idm_scope_set_copy(&merged, lookup)) {
            bool ok = true;
            for (size_t i = 0; ok && i < fallback->count; i++) ok = idm_scope_set_add(&merged, fallback->items[i]);
            if (ok) status = idm_binding_resolve(&ctx->bindings, name, ctx->phase, space, &merged, out_binding);
            idm_scope_set_destroy(&merged);
        }
    }
    idm_scope_set_destroy(&empty);
    return status;
}

ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, out_status);
    return binding ? typed_protocol_by_index(ctx, binding->payload) : NULL;
}

TraitDef *resolve_trait_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, out_status);
    return binding ? typed_trait_by_index(ctx, binding->payload) : NULL;
}

TypeDef *resolve_type_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, out_status);
    return binding ? typed_type_by_index(ctx, binding->payload) : NULL;
}

static const IdmBinding *resolve_surface_binding(const ExpandContext *ctx, const IdmSyntax *word, IdmBindingSpace space, IdmBindingKind kind, IdmResolveStatus *out_status) {
    if (!word || word->kind != IDM_SYN_WORD) {
        if (out_status) *out_status = IDM_RESOLVE_UNBOUND;
        return NULL;
    }
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, word->as.text, space, idm_syn_scope_set(word, 0), NULL, &binding);
    if (out_status) *out_status = status;
    return status == IDM_RESOLVE_OK && binding && binding->kind == kind ? binding : NULL;
}

bool ctx_seed(ExpandContext *ctx, IdmError *err) {
    (void)ctx;
    (void)err;
    return true;
}

char *scoped_identity(ExpandContext *ctx, const char *name, const IdmSyntax *form, unsigned char out_hash[32], IdmError *err) {
    IdmBuffer ser;
    idm_buf_init(&ser);
    if (!idm_syn_serialize(&ser, form, err)) {
        idm_buf_destroy(&ser);
        return NULL;
    }
    idm_sha256(ser.data ? ser.data : "", ser.len, out_hash);
    idm_buf_destroy(&ser);
    char key[17];
    artifact_provider_key(out_hash, key);
    IdmBuffer buf;
    idm_buf_init(&buf);
    bool ok = !ctx->trait_name || (idm_buf_append(&buf, ctx->trait_name) && idm_buf_append_char(&buf, '/'));
    if (!ok || !idm_buf_append(&buf, name) || !idm_buf_append_char(&buf, '#') || !idm_buf_append(&buf, key)) {
        idm_buf_destroy(&buf);
        idm_error_oom(err, form->span);
        return NULL;
    }
    char *identity = idm_buf_take(&buf);
    if (!identity) idm_error_oom(err, form->span);
    return identity;
}

bool syntax_scopes_copy(IdmScopeSet *dst, const IdmSyntax *syn) {
    const IdmScopeSet *src = idm_syn_scope_set(syn, 0);
    if (src) return idm_scope_set_copy(dst, src);
    idm_scope_set_init(dst);
    return true;
}

bool scopes_subset_for_ref(const IdmScopeSet *binding_scopes, const IdmSyntax *ref) {
    const IdmScopeSet *ref_scopes = idm_syn_scope_set(ref, 0);
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = ref_scopes ? ref_scopes : &empty;
    bool ok = idm_scope_set_subset(binding_scopes, lookup);
    idm_scope_set_destroy(&empty);
    return ok;
}

const IdmBinding *resolve_default(const ExpandContext *ctx, const IdmSyntax *word, IdmResolveStatus *out_status) {
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, word->as.text, IDM_BIND_SPACE_DEFAULT, idm_syn_scope_set(word, 0), NULL, &binding);
    if (out_status) *out_status = status;
    return status == IDM_RESOLVE_OK ? binding : NULL;
}

static bool scopes_pruned_for_binding(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out) {
    if (!syntax_scopes_copy(out, name_syntax)) return false;
    if (ctx->rt->macro_intro_active) idm_scope_set_remove(out, ctx->rt->macro_intro_scope);
    return true;
}

bool binder_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out) {
    return scopes_pruned_for_binding(ctx, name_syntax, out);
}

bool surface_decl_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out) {
    return scopes_pruned_for_binding(ctx, name_syntax, out);
}

bool local_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, uint32_t *out_slot) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t slot = ctx->next_slot++;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_LOCAL, &scopes, slot, ctx->frame, arity, NULL);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

bool local_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot) {
    return local_push_def_binder_with_arity(ctx, name, name_syntax, idm_arity_unknown(), out_slot);
}

bool env_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, uint32_t *out_id) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t id = ctx->env_slot_seq++;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, id, IDM_FRAME_ENV, arity, NULL);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_id) *out_id = id;
    return true;
}

bool env_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_id) {
    return env_push_def_binder_with_arity(ctx, name, name_syntax, idm_arity_unknown(), out_id);
}

bool package_slot_ref_add(ExpandContext *ctx, const char *env_key, uint32_t slot, uint32_t *out_id) {
    if (ctx->package_slot_ref_count == ctx->package_slot_ref_cap) {
        if (!idm_grow((void **)&ctx->package_slot_refs, &ctx->package_slot_ref_cap, sizeof(*ctx->package_slot_refs), 8u, ctx->package_slot_ref_count + 1u)) return false;
    }
    PackageSlotRef *ref = &ctx->package_slot_refs[ctx->package_slot_ref_count];
    ref->env_key = idm_strdup(env_key);
    if (!ref->env_key) return false;
    ref->slot = slot;
    if (out_id) *out_id = (uint32_t)ctx->package_slot_ref_count;
    ctx->package_slot_ref_count++;
    return true;
}

const PackageSlotRef *package_slot_ref_get(const ExpandContext *ctx, uint32_t id) {
    return id < ctx->package_slot_ref_count ? &ctx->package_slot_refs[id] : NULL;
}

bool local_push_scoped(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot) {
    IdmScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, name_syntax)) return false;
    uint32_t slot = ctx->next_slot++;
    bool ok = idm_binding_table_add(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_LOCAL, &scopes, slot, ctx->frame, NULL);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

void local_pop_to(ExpandContext *ctx, size_t table_base, uint32_t next_slot) {
    idm_binding_table_truncate(&ctx->bindings, table_base);
    ctx->next_slot = next_slot;
}

bool arg_push_slot(ExpandContext *ctx, const IdmSyntax *word, uint32_t slot) {
    const IdmBinding *existing = resolve_default(ctx, word, NULL);
    if (existing && existing->kind == IDM_BIND_ARG && existing->frame_id == ctx->frame) return true;
    IdmScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, word)) return false;
    bool ok = idm_binding_table_add(&ctx->bindings, word->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ARG, &scopes, slot, ctx->frame, NULL);
    idm_scope_set_destroy(&scopes);
    return ok;
}

static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap) {
    if (*count != *cap) return true;
    return idm_grow((void **)arr, cap, sizeof(**arr), 4u, *count + 1u);
}

static int capture_append_name(CaptureBinding **arr, size_t *count, size_t *cap, const char *name, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity) {
    if (!capture_array_grow(arr, count, cap)) return -1;
    CaptureBinding *c = &(*arr)[*count];
    c->name = idm_strdup(name);
    if (!c->name) return -1;
    if (!idm_scope_set_copy(&c->scopes, scopes)) {
        free(c->name);
        return -1;
    }
    c->kind = kind;
    c->source_index = source_index;
    c->capture_index = (uint32_t)*count;
    c->arity = arity;
    (*count)++;
    return (int)c->capture_index;
}

static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity) {
    return capture_append_name(arr, count, cap, word->as.text, scopes, kind, source_index, arity);
}

static const CaptureBinding *capture_lookup_existing_name(const CaptureBinding *captures, size_t count, const char *name, const IdmScopeSet *scopes) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(captures[i].name, name) == 0 && idm_scope_set_subset(&captures[i].scopes, scopes)) return &captures[i];
    }
    return NULL;
}

const CaptureBinding *capture_lookup_existing(const CaptureBinding *captures, size_t count, const IdmSyntax *word) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(captures[i].name, word->as.text) == 0 && scopes_subset_for_ref(&captures[i].scopes, word)) {
            return &captures[i];
        }
    }
    return NULL;
}

static int saved_materialize(SavedFunctionContext *g, const IdmSyntax *word, const IdmBinding *b) {
    const CaptureBinding *existing = capture_lookup_existing(g->captures, g->capture_count, word);
    if (existing) return (int)existing->capture_index;
    if (!g->prev) return -1;
    if (g->prev->frame == b->frame_id) {
        IdmCaptureKind kind = b->kind == IDM_BIND_ARG ? IDM_CAP_ARG : IDM_CAP_LOCAL;
        return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, kind, b->payload, b->arity);
    }
    int parent = saved_materialize(g->prev, word, b);
    if (parent < 0) return -1;
    return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, IDM_CAP_UPVALUE, (uint32_t)parent, b->arity);
}

static int saved_materialize_slot(SavedFunctionContext *g, const char *name, const IdmScopeSet *scopes, uint32_t frame_id, uint32_t slot, IdmArity arity) {
    const CaptureBinding *existing = capture_lookup_existing_name(g->captures, g->capture_count, name, scopes);
    if (existing) return (int)existing->capture_index;
    if (!g->prev) return -1;
    if (g->prev->frame == frame_id) return capture_append_name(&g->captures, &g->capture_count, &g->capture_cap, name, scopes, IDM_CAP_LOCAL, slot, arity);
    int parent = saved_materialize_slot(g->prev, name, scopes, frame_id, slot, arity);
    if (parent < 0) return -1;
    return capture_append_name(&g->captures, &g->capture_count, &g->capture_cap, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent, arity);
}

bool materialize_capture(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *b, uint32_t *out) {
    if (!ctx->enclosing) return false;
    int idx;
    if (ctx->enclosing->frame == b->frame_id) {
        IdmCaptureKind kind = b->kind == IDM_BIND_ARG ? IDM_CAP_ARG : IDM_CAP_LOCAL;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, kind, b->payload, b->arity);
    } else {
        int parent = saved_materialize(ctx->enclosing, word, b);
        if (parent < 0) return false;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, IDM_CAP_UPVALUE, (uint32_t)parent, b->arity);
    }
    if (idx < 0) return false;
    *out = (uint32_t)idx;
    return true;
}

bool materialize_slot_capture(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, uint32_t frame_id, uint32_t slot, IdmArity arity, uint32_t *out) {
    const CaptureBinding *existing = capture_lookup_existing_name(ctx->captures, ctx->capture_count, name, scopes);
    if (existing) {
        *out = existing->capture_index;
        return true;
    }
    if (!ctx->enclosing) return false;
    int idx;
    if (ctx->enclosing->frame == frame_id) {
        idx = capture_append_name(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, name, scopes, IDM_CAP_LOCAL, slot, arity);
    } else {
        int parent = saved_materialize_slot(ctx->enclosing, name, scopes, frame_id, slot, arity);
        if (parent < 0) return false;
        idx = capture_append_name(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent, arity);
    }
    if (idx < 0) return false;
    *out = (uint32_t)idx;
    return true;
}

void begin_function_context(ExpandContext *ctx, SavedFunctionContext *saved) {
    saved->table_base = ctx->bindings.count;
    saved->frame = ctx->frame;
    saved->next_slot = ctx->next_slot;
    saved->arg_slots = ctx->arg_slots;
    saved->captures = ctx->captures;
    saved->capture_count = ctx->capture_count;
    saved->capture_cap = ctx->capture_cap;
    saved->prev = ctx->enclosing;
    ctx->enclosing = saved;
    ctx->frame = ++ctx->frame_seq;
    ctx->next_slot = 0;
    ctx->arg_slots = 0;
    ctx->captures = NULL;
    ctx->capture_count = 0;
    ctx->capture_cap = 0;
}

void end_function_context(ExpandContext *ctx, SavedFunctionContext *saved) {
    idm_binding_table_truncate(&ctx->bindings, saved->table_base);
    capture_bindings_destroy(ctx->captures, ctx->capture_count);
    ctx->enclosing = saved->prev;
    ctx->frame = saved->frame;
    ctx->next_slot = saved->next_slot;
    ctx->arg_slots = saved->arg_slots;
    ctx->captures = saved->captures;
    ctx->capture_count = saved->capture_count;
    ctx->capture_cap = saved->capture_cap;
}

void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    saved->table_base = ctx->bindings.count;
    saved->next_slot = ctx->next_slot;
    saved->arg_slots = ctx->arg_slots;
    ctx->next_slot = 0;
    ctx->arg_slots = 0;
}

void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    idm_binding_table_truncate(&ctx->bindings, saved->table_base);
    ctx->next_slot = saved->next_slot;
    ctx->arg_slots = saved->arg_slots;
}

bool syn_is_word(const IdmSyntax *syn, const char *word) {
    return syn && syn->kind == IDM_SYN_WORD && strcmp(syn->as.text, word) == 0;
}

bool syn_is_form(const IdmSyntax *syn, const char *head) {
    return syn && syn->kind == IDM_SYN_LIST && syn->as.seq.count > 0 && syn_is_word(syn->as.seq.items[0], head);
}

const char *surface_token_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD) return syn->as.text;
    if (syn_is_form(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

static bool surface_token_text_append(IdmBuffer *buf, const IdmSyntax *syn) {
    const char *direct = surface_token_text(syn);
    if (direct) return idm_buf_append(buf, direct);
    if (!syn_is_form(syn, "%-adjacent") || syn->as.seq.count < 2u) return false;
    for (size_t i = 1u; i < syn->as.seq.count; i++) {
        if (!surface_token_text_append(buf, syn->as.seq.items[i])) return false;
    }
    return true;
}

char *surface_token_text_dup(const IdmSyntax *syn) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!surface_token_text_append(&buf, syn)) {
        idm_buf_destroy(&buf);
        return NULL;
    }
    return idm_buf_take(&buf);
}

bool artifact_bind_name(const char *qualifier, const char *name, const char **out_name, char **owned, IdmError *err, IdmSpan span) {
    *owned = NULL;
    *out_name = name;
    if (!qualifier) return true;
    IdmBuffer qb;
    idm_buf_init(&qb);
    if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, name)) {
        idm_buf_destroy(&qb);
        return idm_error_oom(err, span);
    }
    *owned = idm_buf_take(&qb);
    *out_name = *owned;
    return true;
}

const char *package_path_text(const IdmSyntax *syn) {
    return surface_token_text(syn);
}

static const IdmOperatorDef *op_lookup_capture(const ExpandContext *ctx, const IdmSyntax *syn, const char *capture) {
    char *name = surface_token_text_dup(syn);
    if (!name) return NULL;
    char *key = operator_binding_key(name, capture);
    free(name);
    if (!key) return NULL;
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, key, IDM_BIND_SPACE_OPERATOR, idm_syn_scope_set(syn, 0), ctx->op_fallback, &binding);
    free(key);
    if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_OPERATOR) return NULL;
    if (binding->payload >= ctx->operator_count) return NULL;
    const IdmOperatorDef *op = &ctx->operators[binding->payload];
    return op->capture && strcmp(op->capture, capture) == 0 ? op : NULL;
}

static bool operator_capture_separator(char c) {
    return c == ' ' || c == ':';
}

static const char *operator_capture_role(const char *capture, const char *role) {
    size_t n = strlen(role);
    return capture && strncmp(capture, role, n) == 0 && operator_capture_separator(capture[n]) && capture[n + 1u] ? capture + n + 1u : NULL;
}

bool operator_capture_compile(const char *capture, uint8_t *out_kind, bool *out_left, uint32_t *out_count) {
    uint8_t kind = IDM_OPERATOR_CAPTURE_INVALID;
    bool left = false;
    uint32_t count = 0;
    if (capture) {
        if (strcmp(capture, "prefix") == 0) {
            kind = IDM_OPERATOR_CAPTURE_PREFIX;
        } else if (strcmp(capture, "infix") == 0) {
            kind = IDM_OPERATOR_CAPTURE_INFIX;
            left = true;
        } else if (strcmp(capture, "postfix") == 0) {
            kind = IDM_OPERATOR_CAPTURE_POSTFIX;
            left = true;
        } else {
            const char *shape = capture;
            const char *after = operator_capture_role(shape, "infix");
            if (after) {
                left = true;
                shape = after;
            } else if ((after = operator_capture_role(shape, "prefix")) != NULL) {
                shape = after;
            } else if (operator_capture_role(shape, "postfix")) {
                shape = NULL;
            }
            if (shape) {
                if (strcmp(shape, "indented") == 0) {
                    kind = IDM_OPERATOR_CAPTURE_INDENTED;
                } else if (strcmp(shape, "sentinel") == 0) {
                    kind = IDM_OPERATOR_CAPTURE_SENTINEL;
                } else if (strcmp(shape, "expression") == 0 || strcmp(shape, "expr") == 0) {
                    kind = IDM_OPERATOR_CAPTURE_EXPRESSION;
                } else if (strncmp(shape, "count", 5u) == 0 && operator_capture_separator(shape[5])) {
                    const char *ntext = shape + 5u;
                    while (operator_capture_separator(*ntext)) ntext++;
                    if (*ntext != '\0') {
                        char *tail = NULL;
                        unsigned long n = strtoul(ntext, &tail, 10);
                        if (*tail == '\0' && n <= UINT32_MAX) {
                            kind = IDM_OPERATOR_CAPTURE_COUNT;
                            count = (uint32_t)n;
                        }
                    }
                }
            }
        }
    }
    if (out_kind) *out_kind = kind;
    if (out_left) *out_left = left;
    if (out_count) *out_count = count;
    return kind != IDM_OPERATOR_CAPTURE_INVALID;
}

const IdmOperatorDef *op_lookup(const ExpandContext *ctx, const IdmSyntax *syn, bool want_prefix) {
    if (want_prefix) return op_lookup_capture(ctx, syn, "prefix");
    const IdmOperatorDef *infix = op_lookup_capture(ctx, syn, "infix");
    return infix ? infix : op_lookup_capture(ctx, syn, "postfix");
}

const IdmOperatorDef *op_lookup_syntax_capture(const ExpandContext *ctx, const IdmSyntax *syn, bool want_left, IdmError *err) {
    char *name = surface_token_text_dup(syn);
    if (!name) return NULL;
    const IdmOperatorDef *found = NULL;
    for (size_t i = 0; i < ctx->operator_count; i++) {
        const IdmOperatorDef *op = &ctx->operators[i];
        if (!op->name || strcmp(op->name, name) != 0) continue;
        if (op->capture_kind == IDM_OPERATOR_CAPTURE_PREFIX || op->capture_kind == IDM_OPERATOR_CAPTURE_INFIX || op->capture_kind == IDM_OPERATOR_CAPTURE_POSTFIX) continue;
        if (op->capture_left != want_left) continue;
        const IdmOperatorDef *visible = op_lookup_capture(ctx, syn, op->capture);
        if (visible != op) continue;
        if (found && found != op) {
            idm_error_set(err, syn->span, "ambiguous operator capture '%s'", name);
            free(name);
            return NULL;
        }
        found = op;
    }
    free(name);
    return found;
}

IdmCore *expand_error(IdmError *err, IdmSpan span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    idm_error_setv(err, span, fmt, ap);
    va_end(ap);
    return NULL;
}

static const char *assoc_atom_name(IdmOpAssoc assoc) {
    if (assoc == IDM_OP_ASSOC_LEFT) return "left";
    if (assoc == IDM_OP_ASSOC_RIGHT) return "right";
    return "none";
}

static const char *arity_kind_atom_name(IdmArityKind kind) {
    switch (kind) {
        case IDM_ARITY_UNKNOWN: return "unknown";
        case IDM_ARITY_RANGE: return "range";
        case IDM_ARITY_SET: return "set";
    }
    return "unknown";
}

static IdmValue arity_surface_value(IdmRuntime *rt, IdmArity arity, IdmError *err) {
    IdmDictEntry entries[4];
    entries[0].key = idm_atom(rt, "kind");
    entries[0].value = idm_atom(rt, arity_kind_atom_name(arity.kind));
    entries[1].key = idm_atom(rt, "min");
    entries[1].value = idm_int((int64_t)arity.min);
    entries[2].key = idm_atom(rt, "max");
    entries[2].value = idm_int((int64_t)arity.max);
    entries[3].key = idm_atom(rt, "mask");
    entries[3].value = idm_int((int64_t)arity.mask);
    return idm_dict(rt, entries, 4u, err);
}

static bool grammar_surface_same(const GrammarDef *a, const GrammarDef *b) {
    if (!a->artifact.name || !b->artifact.name) return false;
    return strcmp(a->artifact.name, b->artifact.name) == 0 && idm_scope_set_equal(&a->binding_scopes, &b->binding_scopes);
}

static bool grammar_rule_surface_value(IdmRuntime *rt, const GrammarDef *entry, const IdmGrammarRule *rule, IdmValue *out, IdmError *err) {
    IdmValue rule_items[5];
    rule_items[0] = idm_string(rt, entry->provider ? entry->provider : "", err);
    if (err && err->present) return false;
    rule_items[1] = idm_string(rt, entry->provider_key ? entry->provider_key : "", err);
    if (err && err->present) return false;
    rule_items[2] = idm_atom(rt, idm_grammar_rule_kind_name(rule->kind));
    rule_items[3] = idm_atom(rt, rule->name);
    size_t rule_item_count = 4u;
    if (rule->kind == IDM_GRAMMAR_RULE_TOKEN || rule->kind == IDM_GRAMMAR_RULE_SKIP) {
        IdmValue term_items[3];
        term_items[0] = idm_atom(rt, idm_grammar_terminal_kind_name(rule->terminal.kind));
        term_items[1] = idm_string(rt, rule->terminal.text ? rule->terminal.text : "", err);
        if (err && err->present) return false;
        size_t term_item_count = 2u;
        if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_REGEX) {
            term_items[2] = idm_int((int64_t)rule->terminal.flags);
            term_item_count = 3u;
        }
        rule_items[4] = idm_tuple(rt, term_items, term_item_count, err);
        if (err && err->present) return false;
        rule_item_count = 5u;
    }
    *out = idm_tuple(rt, rule_items, rule_item_count, err);
    return !(err && err->present);
}

static IdmValue scope_set_surface_value(IdmRuntime *rt, const IdmScopeSet *set, IdmError *err) {
    IdmValue scopes = idm_empty_list();
    if (!set) return scopes;
    for (size_t i = set->count; i > 0; i--) {
        scopes = idm_cons(rt, idm_int((int64_t)set->items[i - 1u]), scopes, err);
        if (err && err->present) return scopes;
    }
    return scopes;
}

static bool grammar_contributor_surface_value(IdmRuntime *rt, const GrammarDef *entry, IdmValue *out, IdmError *err) {
    IdmValue items[5];
    items[0] = idm_string(rt, entry->provider ? entry->provider : "", err);
    if (err && err->present) return false;
    items[1] = idm_string(rt, entry->provider_key ? entry->provider_key : "", err);
    if (err && err->present) return false;
    items[2] = idm_atom(rt, idm_grammar_mode_name(entry->artifact.mode));
    items[3] = scope_set_surface_value(rt, &entry->binding_scopes, err);
    if (err && err->present) return false;
    items[4] = scope_set_surface_value(rt, &entry->artifact.scopes, err);
    if (err && err->present) return false;
    *out = idm_tuple(rt, items, 5u, err);
    return !(err && err->present);
}

static bool grammar_composed_rules_value(ExpandContext *ctx, IdmRuntime *rt, const GrammarDef *surface, IdmValue *out, IdmError *err) {
    IdmValue rules = idm_empty_list();
    for (size_t i = ctx->grammar_count; i > 0; i--) {
        const GrammarDef *entry = &ctx->grammars[i - 1u];
        if (!grammar_surface_same(surface, entry)) continue;
        const IdmPkgGrammar *grammar = &entry->artifact;
        for (size_t r = grammar->rule_count; r > 0; r--) {
            IdmValue rule_entry;
            if (!grammar_rule_surface_value(rt, entry, &grammar->rules[r - 1u], &rule_entry, err)) return false;
            rules = idm_cons(rt, rule_entry, rules, err);
            if (err && err->present) return false;
        }
    }
    *out = rules;
    return true;
}

static bool grammar_composed_contributors_value(ExpandContext *ctx, IdmRuntime *rt, const GrammarDef *surface, IdmValue *out, IdmError *err) {
    IdmValue contributors = idm_empty_list();
    for (size_t i = ctx->grammar_count; i > 0; i--) {
        const GrammarDef *entry = &ctx->grammars[i - 1u];
        if (!grammar_surface_same(surface, entry)) continue;
        IdmValue contributor;
        if (!grammar_contributor_surface_value(rt, entry, &contributor, err)) return false;
        contributors = idm_cons(rt, contributor, contributors, err);
        if (err && err->present) return false;
    }
    *out = contributors;
    return true;
}

bool expand_surface_value(ExpandContext *ctx, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err) {
    IdmValue acc = idm_empty_list();
    if (strcmp(kind, "operators") == 0) {
        for (size_t i = ctx->operator_count; i > 0; i--) {
            const IdmOperatorDef *op = &ctx->operators[i - 1u];
            IdmValue items[4];
            items[0] = idm_atom(rt, op->name);
            items[1] = idm_int((int64_t)op->precedence);
            items[2] = idm_atom(rt, assoc_atom_name(op->assoc));
            items[3] = idm_string(rt, op->capture ? op->capture : "value", err);
            if (err && err->present) return false;
            IdmValue entry = idm_tuple(rt, items, 4u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "macros") == 0) {
        for (size_t i = ctx->macro_count; i > 0; i--) {
            if (ctx->macros[i - 1u].hidden) continue;
            const char *name = ctx->macros[i - 1u].name;
            acc = idm_cons(rt, idm_atom(rt, name), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "core-syntax") == 0) {
        for (size_t i = ctx->bindings.count; i > 0; i--) {
            const IdmBinding *b = &ctx->bindings.items[i - 1u];
            if (b->space != IDM_BIND_SPACE_CORE_SYNTAX) continue;
            acc = idm_cons(rt, idm_atom(rt, b->name ? b->name : "_"), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "grammar-artifacts") == 0) {
        for (size_t i = ctx->grammar_count; i > 0; i--) {
            const GrammarDef *surface = &ctx->grammars[i - 1u];
            bool first = true;
            for (size_t prior = 0; prior + 1u < i; prior++) {
                if (grammar_surface_same(surface, &ctx->grammars[prior])) {
                    first = false;
                    break;
                }
            }
            if (!first) continue;
            IdmValue rules;
            if (!grammar_composed_rules_value(ctx, rt, surface, &rules, err)) return false;
            IdmValue contributors;
            if (!grammar_composed_contributors_value(ctx, rt, surface, &contributors, err)) return false;
            IdmValue items[5];
            items[0] = idm_atom(rt, surface->artifact.name);
            items[1] = idm_atom(rt, idm_grammar_mode_name(surface->artifact.mode));
            items[2] = idm_int(0);
            items[3] = rules;
            items[4] = contributors;
            IdmValue entry = idm_tuple(rt, items, 5u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "protocols") == 0) {
        for (size_t i = ctx->typed.entity_count; i > 0; i--) {
            const TypedEntity *entity = &ctx->typed.entities[i - 1u];
            if (entity->kind != IDM_TYPED_ENTITY_PROTOCOL) continue;
            const ProtocolDef *p = &entity->as.protocol;
            acc = idm_cons(rt, idm_atom(rt, p->name), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "active") == 0) {
        for (size_t i = ctx->activation_count; i > 0; i--) {
            IdmValue items[4];
            items[0] = idm_atom(rt, ctx->activations[i - 1u].name);
            items[1] = idm_string(rt, ctx->activations[i - 1u].span.file ? ctx->activations[i - 1u].span.file : "<unknown>", err);
            if (err && err->present) return false;
            items[2] = idm_int((int64_t)ctx->activations[i - 1u].span.line);
            items[3] = idm_int((int64_t)ctx->activations[i - 1u].span.column);
            IdmValue entry = idm_tuple(rt, items, 4u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "methods") == 0) {
        for (size_t i = ctx->typed.method_surface_count; i > 0; i--) {
            const MethodSurfaceDef *m = &ctx->typed.method_surfaces[i - 1u];
            IdmValue items[3];
            items[0] = idm_atom(rt, method_surface_trait_text(m));
            items[1] = idm_atom(rt, method_surface_name_text(m));
            items[2] = arity_surface_value(rt, m->arity, err);
            if (err && err->present) return false;
            IdmValue entry = idm_tuple(rt, items, 3u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else {
        return idm_error_set(err, idm_span_unknown(NULL), "surface kind must be :operators, :macros, :protocols, :core-syntax, :grammar-artifacts, :methods, or :active");
    }
    *out = acc;
    return true;
}
