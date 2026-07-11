#include "internal.h"

#include "idiom/reader.h"

static void capture_bindings_destroy(CaptureBinding *captures, size_t count);
static void method_surface_def_destroy(MethodSurfaceDef *method);
static void typed_entity_destroy(TypedEntity *entity);
static void core_form_def_destroy(CoreFormDef *core_form);
static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap);
static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmBindingId binding_id, IdmArity arity, const IdmCallableContract *contract);
static int saved_materialize(SavedFunctionContext *g, const IdmSyntax *word, const IdmBinding *b);
static const IdmOperatorDef *op_lookup_capture(const ExpandContext *ctx, const IdmSyntax *syn, const char *capture);
static const IdmBinding *resolve_surface_binding(const ExpandContext *ctx, const IdmSyntax *word, IdmBindingSpace space, IdmBindingKind kind, IdmResolveStatus *out_status);
static void core_form_fn_free(IdmBindingKind kind, void *data);

static bool ctx_seed_builtin_types(ExpandContext *ctx) {
    static const char *const parents[] = { "list", "int", "proc" };
    IdmError err;
    idm_error_init(&err);
    for (size_t p = 0; p < sizeof(parents) / sizeof(parents[0]); p++) {
        const char *const *member_names = NULL;
        size_t member_count = idm_builtin_overtype_members(parents[p], &member_names);
        if (member_count == 0) continue;
        uint32_t index = 0;
        TypeDef *type = typed_registry_add_type(ctx, &index, &err, idm_span_unknown(NULL));
        if (!type) return false;
        type->name = idm_strdup(parents[p]);
        if (!type->name) return false;
        if (!type_def_set_identity(ctx, type, parents[p], &err, idm_span_unknown(NULL))) return false;
        type->exported = false;
        type->members = calloc(member_count, sizeof(*type->members));
        if (!type->members) return false;
        for (size_t m = 0; m < member_count; m++) {
            if (!idm_type_con(&type->members[m].term, member_names[m])) return false;
            type->member_count++;
        }
    }
    return true;
}

static bool bind_native_core_form(ExpandContext *ctx, const char *name, NativeSurfaceFn handler, IdmError *err) {
    PhaseSyntaxFn *fn = calloc(1u, sizeof(*fn));
    if (!fn) return idm_error_oom(err, idm_span_unknown(NULL));
    fn->native = handler;
    int bound = surface_bind_data(ctx, "kernel", ctx->unit_key, name, name, IDM_BIND_SPACE_CORE_FORM, IDM_BIND_CORE_FORM, &ctx->empty_scopes, fn, idm_span_unknown(NULL), err);
    if (bound <= 0) {
        free(fn);
        return bound == 0;
    }
    return true;
}

bool ctx_init(ExpandContext *ctx, IdmRuntime *rt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rt = rt;
    idm_binding_table_init(&ctx->bindings);
    idm_binding_table_set_data_free(&ctx->bindings, core_form_fn_free);
    idm_scope_set_init(&ctx->empty_scopes);
    ctx->frame = 1u;
    ctx->frame_seq = 1u;
    ctx->local_runner.user = ctx;
    ctx->local_runner.invoke = local_macro_invoke;
    idm_scope_store_init_shared(&ctx->scope_store, &rt->scope_next);
    ctx->surface_phase = -1;
    ctx->unit = "<unit>";
    memcpy(ctx->unit_key, "0000000000000000", sizeof ctx->unit_key);
    if (!ctx_seed_builtin_types(ctx)) return false;
    IdmError floor_err;
    idm_error_init(&floor_err);
    bool floors = bind_native_core_form(ctx, "fn", expand_fn_parts, &floor_err) &&
                  bind_native_core_form(ctx, "explain", expand_explain_parts, &floor_err);
    idm_error_clear(&floor_err);
    return floors;
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
        saved->identifier_bound_user = rt->identifier_bound_user;
        saved->identifier_bound = rt->identifier_bound;
    }
    rt->local_expand_user = ctx;
    rt->local_expand = local_expand_callback;
    rt->free_identifier_eq_user = ctx;
    rt->free_identifier_eq = free_identifier_eq_callback;
    rt->identifier_bound_user = ctx;
    rt->identifier_bound = identifier_bound_callback;
}

void hooks_restore(IdmRuntime *rt, const SavedHooks *saved) {
    rt->local_expand_user = saved->local_expand_user;
    rt->local_expand = saved->local_expand;
    rt->free_identifier_eq_user = saved->free_identifier_eq_user;
    rt->free_identifier_eq = saved->free_identifier_eq;
    rt->identifier_bound_user = saved->identifier_bound_user;
    rt->identifier_bound = saved->identifier_bound;
}

static void capture_bindings_destroy(CaptureBinding *captures, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (captures[i].has_contract) idm_callable_contract_destroy(&captures[i].contract);
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

static void core_form_fn_free(IdmBindingKind kind, void *data) {
    if (!data) return;
    if (kind == IDM_BIND_READER_FORM) {
        reader_form_binding_free(data);
        return;
    }
    phase_syntax_fn_destroy((PhaseSyntaxFn *)data);
    free(data);
}

void reader_form_binding_free(void *data) {
    if (!data) return;
    ReaderFormBinding *binding = data;
    idm_enforest_free(binding->node);
    phase_syntax_fn_destroy(&binding->fn);
    free(binding);
}

static void reader_form_def_clear(ReaderFormDef *def) {
    free(def->name);
    idm_scope_set_destroy(&def->scopes);
    idm_enforest_free(def->node);
    phase_syntax_fn_destroy(&def->fn);
    memset(def, 0, sizeof(*def));
}

static void core_form_def_destroy(CoreFormDef *core_form) {
    if (!core_form) return;
    free(core_form->name);
    idm_scope_set_destroy(&core_form->scopes);
    phase_syntax_fn_destroy(&core_form->fn);
    memset(core_form, 0, sizeof(*core_form));
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
    if (impl->has_contract) idm_callable_contract_destroy(&impl->contract);
    memset(impl, 0, sizeof(*impl));
}

void protocol_def_destroy(ProtocolDef *protocol) {
    if (!protocol) return;
    free(protocol->name);
    if (protocol->art && !protocol->art->rt_owned) {
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
    for (size_t i = 0; i < type->member_count; i++) idm_type_term_destroy(&type->members[i].term);
    free(type->members);
    for (size_t i = 0; i < type->field_count; i++) {
        if (type->fields[i].has_contract) idm_type_term_destroy(&type->fields[i].contract);
    }
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

static char *edge_pretty(const IdmSyntax *syn) {
    if (!syn) return NULL;
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_syn_dump_pretty(&buf, syn)) {
        idm_buf_destroy(&buf);
        return NULL;
    }
    return idm_buf_take(&buf);
}

static void syn_span_extent(const IdmSyntax *syn, const char *file, IdmSpan *acc) {
    if (!syn) return;
    if (syn->span.file == file && syn->span.end > syn->span.start) {
        if (acc->end <= acc->start) {
            acc->start = syn->span.start;
            acc->end = syn->span.end;
        } else {
            if (syn->span.start < acc->start) acc->start = syn->span.start;
            if (syn->span.end > acc->end) acc->end = syn->span.end;
        }
    }
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) syn_span_extent(syn->as.seq.items[i], file, acc);
    }
}

void expand_edge_record(ExpandContext *ctx, const char *kind, const char *name, const char *provider, const char *doc, const IdmSyntax *before, const IdmSyntax *after, IdmSpan span) {
    if (!ctx->record_edges) return;
    if (before && span.file) {
        IdmSpan extent = span;
        syn_span_extent(before, span.file, &extent);
        span.start = extent.start;
        span.end = extent.end;
    }
    if (ctx->edge_count == ctx->edge_cap &&
        !idm_grow((void **)&ctx->edges, &ctx->edge_cap, sizeof(*ctx->edges), 16u, ctx->edge_count + 1u)) return;
    ExpandEdge *edge = &ctx->edges[ctx->edge_count];
    memset(edge, 0, sizeof(*edge));
    edge->kind = kind;
    edge->name = idm_strdup(name ? name : "");
    edge->provider = provider ? idm_strdup(provider) : NULL;
    edge->phase = ctx->phase;
    edge->value_context = ctx->value_context;
    edge->command_sub = ctx->command_sub_context;
    edge->span = span;
    edge->before = edge_pretty(before);
    edge->after = edge_pretty(after);
    edge->doc = doc ? idm_strdup(doc) : NULL;
    ctx->edge_count++;
}

void expand_edges_truncate(ExpandContext *ctx, size_t base) {
    while (ctx->edge_count > base) {
        ExpandEdge *edge = &ctx->edges[--ctx->edge_count];
        free(edge->name);
        free(edge->provider);
        free(edge->before);
        free(edge->after);
        free(edge->doc);
    }
}

bool expand_edge_value(ExpandContext *ctx, IdmRuntime *rt, const ExpandEdge *edge, IdmValue *out, IdmError *err) {
    (void)ctx;
    IdmDictEntry entries[11];
    size_t count = 0;
    entries[count].key = idm_atom(rt, "kind");
    entries[count++].value = idm_atom(rt, edge->kind);
    entries[count].key = idm_atom(rt, "name");
    entries[count++].value = idm_string(rt, edge->name ? edge->name : "", err);
    if (err && err->present) return false;
    entries[count].key = idm_atom(rt, "provider");
    entries[count++].value = edge->provider ? idm_string(rt, edge->provider, err) : idm_atom(rt, "local");
    if (err && err->present) return false;
    entries[count].key = idm_atom(rt, "phase");
    entries[count++].value = idm_int(edge->phase);
    entries[count].key = idm_atom(rt, "value-context");
    entries[count++].value = idm_bool(rt, edge->value_context);
    entries[count].key = idm_atom(rt, "command-sub");
    entries[count++].value = idm_bool(rt, edge->command_sub);
    if (edge->span.file) {
        entries[count].key = idm_atom(rt, "file");
        entries[count++].value = idm_string(rt, edge->span.file, err);
        if (err && err->present) return false;
    }
    entries[count].key = idm_atom(rt, "line");
    entries[count++].value = idm_int((int64_t)edge->span.line);
    if (edge->before) {
        entries[count].key = idm_atom(rt, "before");
        entries[count++].value = idm_string(rt, edge->before, err);
        if (err && err->present) return false;
    }
    if (edge->after) {
        entries[count].key = idm_atom(rt, "after");
        entries[count++].value = idm_string(rt, edge->after, err);
        if (err && err->present) return false;
    }
    if (edge->doc) {
        entries[count].key = idm_atom(rt, "doc");
        entries[count++].value = idm_string(rt, edge->doc, err);
        if (err && err->present) return false;
    }
    *out = idm_dict(rt, entries, count, err);
    return !(err && err->present);
}

static void surface_binding_set_provider(ExpandContext *ctx, const char *provider) {
    if (!provider || !provider[0]) return;
    IdmSymbol *sym = idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, provider);
    if (sym) ctx->bindings.items[ctx->bindings.count - 1u].provider = idm_symbol_text(sym);
}

int surface_bind_payload(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, IdmSpan span, IdmError *err) {
    int guard = surface_install_guard(ctx, provider, provider_key, key, display, space, scopes, err);
    if (guard <= 0) return guard;
    if (!idm_binding_table_add(&ctx->bindings, key, IDM_PHASE_ANY, space, kind, scopes, payload, ctx->frame, NULL)) {
        idm_error_oom(err, span);
        return -1;
    }
    surface_binding_set_provider(ctx, provider);
    return 1;
}

int surface_bind_data(ExpandContext *ctx, const char *provider, const char *provider_key, const char *key, const char *display, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, void *data, IdmSpan span, IdmError *err) {
    int guard = surface_install_guard(ctx, provider, provider_key, key, display, space, scopes, err);
    if (guard <= 0) return guard;
    if (!idm_binding_table_add_data(&ctx->bindings, key, IDM_PHASE_ANY, space, kind, scopes, data, ctx->frame, NULL)) {
        idm_error_oom(err, span);
        return -1;
    }
    surface_binding_set_provider(ctx, provider);
    return 1;
}

void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint) {
    checkpoint->next_scope = idm_scope_store_next(&ctx->scope_store);
    checkpoint->env_slot_seq = ctx->env_slot_seq;
    checkpoint->next_slot = ctx->next_slot;
    checkpoint->binding_count = ctx->bindings.count;
    checkpoint->macro_count = ctx->macro_count;
    checkpoint->grammar_count = ctx->grammar_count;
    checkpoint->decl_grammar_count = ctx->decl_grammar_count;
    checkpoint->operator_count = ctx->operator_count;
    checkpoint->typed.entity_count = ctx->typed.entity_count;
    checkpoint->typed.method_surface_count = ctx->typed.method_surface_count;
    checkpoint->decl_method_count = ctx->decl_method_count;
    checkpoint->typed.method_impl_count = ctx->typed.method_impl_count;
    checkpoint->decl_core_form_count = ctx->decl_core_form_count;
    checkpoint->decl_reader_forms_count = ctx->decl_reader_forms_count;
    checkpoint->activation_count = ctx->activation_count;
    checkpoint->surface_install_count = ctx->surface_install_count;
    checkpoint->artifact_base_count = ctx->artifact_base_count;
    checkpoint->init_emit_mark_count = ctx->init_emit_mark_count;
    checkpoint->runtime_init_mark_count = ctx->runtime_init_mark_count;
    checkpoint->phase_runtime_init_mark_count = ctx->phase_runtime_init_mark_count;
    checkpoint->package_slot_ref_count = ctx->package_slot_ref_count;
    checkpoint->package_slot_count = ctx->package_slot_count;
    checkpoint->field_selector_count = ctx->field_selector_count;
    checkpoint->phase_candidate_count = ctx->phase_candidate_count;
    checkpoint->reader_generation = ctx->reader_generation;
}

static void phase_candidate_destroy(PhaseCandidate *cand) {
    free(cand->name);
    idm_scope_set_destroy(&cand->scopes);
    for (size_t k = 0; k < cand->count; k++) idm_syn_free(cand->items[k]);
    free(cand->items);
    if (cand->has_contract) idm_callable_contract_destroy(&cand->contract);
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
    while (ctx->package_slot_count > checkpoint->package_slot_count) idm_pkg_slot_destroy(&ctx->package_slots[--ctx->package_slot_count]);
    while (ctx->grammar_count > checkpoint->grammar_count) grammar_def_destroy(&ctx->grammars[--ctx->grammar_count]);
    if (ctx->reader_generation != checkpoint->reader_generation) ctx_reader_invalidate(ctx);
    while (ctx->macro_count > checkpoint->macro_count) macro_def_destroy(&ctx->macros[--ctx->macro_count]);
    while (ctx->operator_count > checkpoint->operator_count) idm_operator_def_destroy(&ctx->operators[--ctx->operator_count]);
    while (ctx->typed.entity_count > checkpoint->typed.entity_count) typed_entity_destroy(&ctx->typed.entities[--ctx->typed.entity_count]);
    while (ctx->typed.method_surface_count > checkpoint->typed.method_surface_count) method_surface_def_destroy(&ctx->typed.method_surfaces[--ctx->typed.method_surface_count]);
    while (ctx->decl_method_count > checkpoint->decl_method_count) trait_method_def_destroy(&ctx->decl_methods[--ctx->decl_method_count]);
    while (ctx->typed.method_impl_count > checkpoint->typed.method_impl_count) method_impl_def_destroy(&ctx->typed.method_impls[--ctx->typed.method_impl_count]);
    while (ctx->decl_core_form_count > checkpoint->decl_core_form_count) core_form_def_destroy(&ctx->decl_core_form[--ctx->decl_core_form_count]);
    while (ctx->decl_reader_forms_count > checkpoint->decl_reader_forms_count) reader_form_def_clear(&ctx->decl_reader_forms[--ctx->decl_reader_forms_count]);
    while (ctx->decl_grammar_count > checkpoint->decl_grammar_count) idm_pkg_grammar_destroy(&ctx->decl_grammars[--ctx->decl_grammar_count]);
    while (ctx->field_selector_count > checkpoint->field_selector_count) {
        FieldSelectorDef *sel = &ctx->field_selectors[--ctx->field_selector_count];
        free(sel->name);
        free(sel->env_key);
        memset(sel, 0, sizeof(*sel));
    }
    while (ctx->phase_candidate_count > checkpoint->phase_candidate_count) phase_candidate_destroy(&ctx->phase_candidates[--ctx->phase_candidate_count]);
    idm_binding_table_truncate(&ctx->bindings, checkpoint->binding_count);
}

void surface_discard(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint) {
    surface_rollback(ctx, checkpoint);
    if (!ctx->scope_store.shared) ctx->scope_store.next_scope = checkpoint->next_scope;
    ctx->env_slot_seq = checkpoint->env_slot_seq;
    ctx->next_slot = checkpoint->next_slot;
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
    expand_edges_truncate(ctx, 0);
    free(ctx->edges);
    bits_binders_truncate(ctx, 0);
    free(ctx->bits_binder_names);
    free(ctx->bits_binder_slots);
    free(ctx->match_size_outer);
    for (size_t i = 0; i < ctx->match_size_count; i++) free(ctx->match_size_fresh[i]);
    free(ctx->match_size_fresh);
    free(ctx->match_size_slots);
    free(ctx->match_size_clause);
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
    for (size_t i = 0; i < ctx->field_selector_count; i++) { free(ctx->field_selectors[i].name); free(ctx->field_selectors[i].env_key); }
    free(ctx->field_selectors);
    for (size_t i = 0; i < ctx->typed.entity_count; i++) typed_entity_destroy(&ctx->typed.entities[i]);
    free(ctx->typed.entities);
    for (size_t i = 0; i < ctx->typed.method_surface_count; i++) method_surface_def_destroy(&ctx->typed.method_surfaces[i]);
    free(ctx->typed.method_surfaces);
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) method_impl_def_destroy(&ctx->typed.method_impls[i]);
    free(ctx->typed.method_impls);
    for (size_t i = 0; i < ctx->decl_method_count; i++) trait_method_def_destroy(&ctx->decl_methods[i]);
    free(ctx->decl_methods);
    for (size_t i = 0; i < ctx->decl_core_form_count; i++) core_form_def_destroy(&ctx->decl_core_form[i]);
    free(ctx->decl_core_form);
    for (size_t i = 0; i < ctx->decl_reader_forms_count; i++) reader_form_def_clear(&ctx->decl_reader_forms[i]);
    free(ctx->decl_reader_forms);
    for (size_t i = 0; i < ctx->phase_candidate_count; i++) phase_candidate_destroy(&ctx->phase_candidates[i]);
    free(ctx->phase_candidates);
    free(ctx->phase_demand_diag);
    if (ctx->active_reader) idm_reader_artifact_destroy(ctx->active_reader);
    for (size_t i = 0; i < ctx->decl_grammar_count; i++) idm_pkg_grammar_destroy(&ctx->decl_grammars[i]);
    free(ctx->decl_grammars);
    idm_phase_env_release(ctx->phase_env);
    free(ctx->artifact_bases);
    for (size_t i = 0; i < ctx->package_slot_ref_count; i++) free(ctx->package_slot_refs[i].env_key);
    free(ctx->package_slot_refs);
    for (size_t i = 0; i < ctx->dep_count; i++) free(ctx->deps[i].path);
    free(ctx->deps);
    free(ctx->pat_binders);
    expand_solved_types_clear(ctx);
    free(ctx->solved_nodes);
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
        bool structural_head = arg0_type && arg0_type[0] == '_' && arg0_type[1] == '.';
        patterns[i] = i == 0 && arg0_type && !structural_head ? idm_pat_type(arg0_type, span) : idm_pat_wildcard(span);
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

bool type_var_is_wildcard(const IdmTypeTerm *term) {
    return term && term->kind == IDM_TYPE_VAR && (term->var_id == 0u || (term->name && strcmp(term->name, "_") == 0));
}

bool type_var_same(const IdmTypeTerm *a, const IdmTypeTerm *b) {
    if (!a || !b || a->kind != IDM_TYPE_VAR || b->kind != IDM_TYPE_VAR) return false;
    if (type_var_is_wildcard(a) || type_var_is_wildcard(b)) return true;
    if (a->var_id != 0u || b->var_id != 0u) return a->var_id == b->var_id;
    return a->name && b->name && strcmp(a->name, b->name) == 0;
}

static const TypeDef *typed_type_by_identity_or_name(const ExpandContext *ctx, const char *name);

static bool nil_empty_list_same(const char *a, const char *b) {
    return a && b &&
           ((strcmp(a, "nil") == 0 && strcmp(b, "empty-list") == 0) ||
            (strcmp(a, "empty-list") == 0 && strcmp(b, "nil") == 0));
}

bool type_name_same(const ExpandContext *ctx, const char *a, const char *b) {
    if (!a || !b) return false;
    if (strcmp(a, b) == 0) return true;
    if (nil_empty_list_same(a, b)) return true;
    IdmSymbol *as = typed_identity_lookup(ctx, a);
    IdmSymbol *bs = typed_identity_lookup(ctx, b);
    if (as && bs && as == bs) return true;
    const TypeDef *ta = typed_type_by_identity_or_name(ctx, a);
    const TypeDef *tb = typed_type_by_identity_or_name(ctx, b);
    return ta && tb && ta == tb;
}

const TraitDef *trait_by_constraint_name(ExpandContext *ctx, const char *trait) {
    const TraitDef *direct = typed_trait_by_identity(ctx, trait);
    if (direct) return direct;
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, trait, IDM_BIND_SPACE_TRAIT, &ctx->empty_scopes, NULL, &binding);
    if (status == IDM_RESOLVE_OK && binding && binding->kind == IDM_BIND_TRAIT) return typed_trait_by_index(ctx, binding->payload);
    const TraitDef *found = NULL;
    size_t len = trait ? strlen(trait) : 0u;
    for (size_t i = 0; trait && i < ctx->typed.entity_count; i++) {
        const TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_TRAIT) continue;
        const char *identity = trait_def_identity_text(&entity->as.trait);
        if (!identity || strncmp(identity, trait, len) != 0 || identity[len] != '#') continue;
        if (found) return NULL;
        found = &entity->as.trait;
    }
    return found;
}

const char *typeclass_display_name(ExpandContext *ctx, const char *trait) {
    const TraitDef *def = trait_by_constraint_name(ctx, trait);
    if (!def) return trait;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *binding = &ctx->bindings.items[i];
        if (binding->space == IDM_BIND_SPACE_TRAIT && binding->kind == IDM_BIND_TRAIT) {
            const TraitDef *candidate = typed_trait_by_index(ctx, binding->payload);
            if (candidate == def) return binding->name;
        }
    }
    return trait;
}

bool typeclass_same_trait(ExpandContext *ctx, const char *a, const char *b) {
    if (!a || !b) return false;
    if (strcmp(a, b) == 0) return true;
    const TraitDef *ta = trait_by_constraint_name(ctx, a);
    const TraitDef *tb = trait_by_constraint_name(ctx, b);
    return ta && tb && ta == tb;
}

static bool trait_requires_trait(ExpandContext *ctx, const TraitDef *source, const TraitDef *target, unsigned depth) {
    if (!source || !target || depth > 64u) return false;
    for (size_t i = 0; i < source->requirement_count; i++) {
        const TraitDef *required = trait_by_constraint_name(ctx, source->requirements[i].name);
        if (!required) continue;
        if (required == target) return true;
        if (trait_requires_trait(ctx, required, target, depth + 1u)) return true;
    }
    return false;
}

static bool typeclass_same_lhs(ExpandContext *ctx, const IdmTypeTerm *given, const IdmTypeTerm *wanted) {
    if (!given || !wanted) return false;
    if (given->kind == IDM_TYPE_VAR && wanted->kind == IDM_TYPE_VAR) return type_var_same(given, wanted);
    if (given->kind == IDM_TYPE_CON && wanted->kind == IDM_TYPE_CON && type_name_same(ctx, given->name, wanted->name)) return true;
    return idm_type_term_equal(given, wanted);
}

static bool trait_name_is_decl(const ExpandContext *ctx, const char *name) {
    if (!ctx->trait_name || !name) return false;
    if (strcmp(name, ctx->trait_name) == 0) return true;
    return ctx->trait_key && strcmp(name, ctx->trait_key) == 0;
}

bool typeclass_given_matches(ExpandContext *ctx, const IdmConstraint *given, const char *trait, const IdmTypeTerm *lhs) {
    if (!given || given->kind != IDM_CONSTR_CLASS || !trait || !given->trait || !typeclass_same_lhs(ctx, &given->lhs, lhs)) return false;
    if (strcmp(given->trait, trait) == 0) return true;
    const TraitDef *given_trait = trait_by_constraint_name(ctx, given->trait);
    const TraitDef *wanted_trait = trait_by_constraint_name(ctx, trait);
    if (!given_trait || !wanted_trait) return trait_name_is_decl(ctx, given->trait) && trait_name_is_decl(ctx, trait);
    return given_trait == wanted_trait || trait_requires_trait(ctx, given_trait, wanted_trait, 0u);
}

bool trait_impl_satisfies_type(const ExpandContext *ctx, const TraitDef *trait, const char *type) {
    if (!ctx || !trait || !type) return false;
    const char *trait_identity = trait_def_identity_text(trait);
    if (!trait_identity) return false;
    for (size_t m = 0; m < trait->method_count; m++) {
        bool found = false;
        const char *method = trait_method_name_text(&trait->methods[m]);
        for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
            if (method_impl_matches_identity(ctx, &ctx->typed.method_impls[i], trait_identity, method, type)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return trait->method_count != 0;
}

const TypeDef *typed_type_by_identity(const ExpandContext *ctx, const char *identity) {
    if (!ctx || !identity) return NULL;
    IdmSymbol *sym = typed_identity_lookup(ctx, identity);
    if (!sym) return NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_TYPE && entity->as.type.identity == sym) return &entity->as.type;
    }
    return NULL;
}

static const TypeDef *typed_type_by_identity_or_name(const ExpandContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind != IDM_TYPED_ENTITY_TYPE) continue;
        const TypeDef *type = &entity->as.type;
        const char *identity = type_def_identity_text(type);
        if ((identity && strcmp(identity, name) == 0) || (type->name && strcmp(type->name, name) == 0)) return type;
    }
    return NULL;
}

const TypeDef *type_def_lookup_name(const ExpandContext *ctx, const char *name) {
    return typed_type_by_identity_or_name(ctx, name);
}

bool trait_impl_satisfies_term(const ExpandContext *ctx, const TraitDef *trait, const IdmTypeTerm *lhs) {
    if (!ctx || !trait || !lhs) return false;
    if (lhs->kind == IDM_TYPE_UNION) {
        for (size_t i = 0; i < lhs->arg_count; i++) {
            if (!trait_impl_satisfies_term(ctx, trait, &lhs->args[i])) return false;
        }
        return true;
    }
    if (lhs->kind != IDM_TYPE_CON || !lhs->name) return false;
    if (trait_impl_satisfies_type(ctx, trait, lhs->name)) return true;
    const TypeDef *type = typed_type_by_identity_or_name(ctx, lhs->name);
    if (type && type->member_count != 0) {
        bool checked = false;
        for (size_t i = 0; i < type->member_count; i++) {
            if (idm_type_term_equal(&type->members[i].term, lhs)) continue;
            checked = true;
            if (!trait_impl_satisfies_term(ctx, trait, &type->members[i].term)) return false;
        }
        if (checked) return true;
    }
    return false;
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
    if (method->has_contract) idm_callable_contract_destroy(&method->contract);
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
        methods[i].has_contract = src[i].has_contract;
        if (src[i].has_contract && !idm_callable_contract_copy(&methods[i].contract, &src[i].contract)) {
            for (size_t j = 0; j <= i; j++) trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].dispatch_slot = src[i].dispatch_slot;
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
        methods[i].has_contract = src[i].has_contract;
        if (src[i].has_contract && !idm_callable_contract_copy(&methods[i].contract, &src[i].contract)) {
            for (size_t j = 0; j <= i; j++) trait_method_def_destroy(&methods[j]);
            free(methods);
            return idm_error_oom(err, span);
        }
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].dispatch_slot = src[i].dispatch_slot;
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
        methods[i].has_contract = src[i].has_contract;
        if (src[i].has_contract && !idm_callable_contract_copy(&methods[i].contract, &src[i].contract)) {
            for (size_t j = 0; j <= i; j++) idm_trait_method_def_destroy(&methods[j]);
            free(methods);
            return false;
        }
        methods[i].has_default = src[i].has_default;
        methods[i].seen_decl = src[i].seen_decl;
        methods[i].exported = src[i].exported;
        methods[i].default_slot = src[i].default_slot;
        methods[i].has_default_slot = src[i].has_default_slot;
        methods[i].has_dispatch = src[i].has_dispatch;
        methods[i].dispatch_slot = src[i].dispatch_slot;
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

const IdmTypeTerm *type_field_contract_term(const TypeFieldDef *field) {
    return field && field->has_contract ? &field->contract : NULL;
}

bool type_field_set(ExpandContext *ctx, TypeFieldDef *field, const char *name, const IdmTypeTerm *contract, IdmError *err, IdmSpan span) {
    if (!ctx || !field || !name || !*name) return idm_error_set(err, span, "record field must be a non-empty name");
    IdmSymbol *name_sym = typed_atom_intern(ctx, name);
    if (!name_sym) return idm_error_oom(err, span);
    field->name = name_sym;
    field->has_contract = false;
    memset(&field->contract, 0, sizeof(field->contract));
    if (contract) {
        if (!idm_type_term_copy(&field->contract, contract)) return idm_error_oom(err, span);
        field->has_contract = true;
    }
    return true;
}

bool type_field_matches_name(const ExpandContext *ctx, const TypeFieldDef *field, const char *name) {
    IdmSymbol *sym = typed_identity_lookup(ctx, name);
    return field && field->name && sym && field->name == sym;
}

static bool syntax_is_comma(const IdmSyntax *syn) {
    return syn && syn->kind == IDM_SYN_ATOM && strcmp(syn->as.text, "comma") == 0;
}

static bool syntax_text_is(const IdmSyntax *syn, const char *want) {
    const char *got = surface_token_text(syn);
    return got && strcmp(got, want) == 0;
}

static bool type_name_is_var(ExpandContext *ctx, const IdmSyntax *word) {
    if (!word || word->kind != IDM_SYN_WORD || !word->as.text || !word->as.text[0]) return false;
    if (strcmp(word->as.text, "_") == 0) return true;
    IdmSymbol *sym = idm_intern(&ctx->rt->intern, IDM_SYMBOL_ATOM, word->as.text);
    if (sym && idm_value_builtin_type_symbol(sym)) return false;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    if (resolve_type_def(ctx, word, &status)) return false;
    char ch = word->as.text[0];
    return ch >= 'a' && ch <= 'z';
}

static bool contract_var_id(IdmCallableContract *contract, const char *name, uint32_t *out_id, IdmError *err, IdmSpan span) {
    if (out_id) *out_id = 0u;
    if (!name || !*name) return true;
    if (strcmp(name, "_") == 0) return true;
    for (size_t i = 0; i < contract->quantified_count; i++) {
        if (strcmp(contract->quantified[i], name) == 0) {
            if (i + 1u > UINT32_MAX) return idm_error_set(err, span, "too many quantified type variables");
            if (out_id) *out_id = (uint32_t)(i + 1u);
            return true;
        }
    }
    if (contract->quantified_count >= UINT32_MAX) return idm_error_set(err, span, "too many quantified type variables");
    if (!idm_grow((void **)&contract->quantified, &contract->quantified_cap, sizeof(*contract->quantified), 4u, contract->quantified_count + 1u)) return idm_error_oom(err, span);
    contract->quantified[contract->quantified_count] = idm_strdup(name);
    if (!contract->quantified[contract->quantified_count]) return idm_error_oom(err, span);
    contract->quantified_count++;
    if (out_id) *out_id = (uint32_t)contract->quantified_count;
    return true;
}

static bool parse_type_term_range(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *owner, IdmTypeTerm *out, IdmError *err);

static bool parse_type_atom(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, size_t *out_end, IdmCallableContract *owner, IdmTypeTerm *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (start >= end) return idm_error_set(err, idm_span_unknown(NULL), "expected type");
    IdmSyntax *syn = items[start];
    if (idm_syn_is_form_id(syn, IDM_FORM_GROUP) || idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP)) {
        if (syn->as.seq.count != 2u || !idm_syn_is_form_id(syn->as.seq.items[1], IDM_FORM_EXPR)) {
            return idm_error_set(err, syn->span, "type group expects one expression");
        }
        IdmSyntax *inner = syn->as.seq.items[1];
        if (!parse_type_term_range(ctx, inner->as.seq.items, 1u, inner->as.seq.count, owner, out, err)) return false;
        *out_end = start + 1u;
        return true;
    }
    if (syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE) {
        IdmTypeTerm *args = syn->as.seq.count == 0 ? NULL : calloc(syn->as.seq.count, sizeof(*args));
        if (syn->as.seq.count != 0 && !args) return idm_error_oom(err, syn->span);
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            IdmSyntax *item = syn->as.seq.items[i];
            IdmSyntax *one[] = { item };
            if (!parse_type_term_range(ctx, one, 0u, 1u, owner, &args[i], err)) {
                for (size_t j = 0; j < i; j++) idm_type_term_destroy(&args[j]);
                free(args);
                return false;
            }
        }
        if (!idm_type_compound(out, syn->kind == IDM_SYN_VECTOR ? IDM_TYPE_VECTOR : IDM_TYPE_TUPLE, args, syn->as.seq.count)) {
            for (size_t i = 0; i < syn->as.seq.count; i++) idm_type_term_destroy(&args[i]);
            free(args);
            return idm_error_oom(err, syn->span);
        }
        *out_end = start + 1u;
        return true;
    }
    IdmSyntax *word = NULL;
    size_t pos = start;
    if (!try_qualified_word_at(items, start, end, &word, &pos, err)) return false;
    if (!word) return idm_error_set(err, syn->span, "expected type name");
    bool is_var = type_name_is_var(ctx, word);
    uint32_t var_id = 0u;
    bool ok = true;
    if (is_var && owner) ok = contract_var_id(owner, word->as.text, &var_id, err, word->span);
    const char *type_name = word->as.text;
    if (ok && !is_var) {
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        TypeDef *type = resolve_type_def(ctx, word, &status);
        if (status == IDM_RESOLVE_AMBIGUOUS) {
            IdmSpan span = word->span;
            idm_syn_free(word);
            return idm_error_set(err, span, "ambiguous type '%s'", type_name);
        }
        if (type) type_name = type_def_identity_text(type);
    }
    if (ok) ok = is_var ? idm_type_var(out, word->as.text, var_id, false) : idm_type_con(out, type_name);
    IdmSpan span = word->span;
    idm_syn_free(word);
    if (!ok) return idm_error_oom(err, span);
    *out_end = pos;
    return true;
}

static bool parse_type_app(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *owner, IdmTypeTerm *out, IdmError *err) {
    size_t pos = start;
    if (!parse_type_atom(ctx, items, pos, end, &pos, owner, out, err)) return false;
    IdmTypeTerm *args = NULL;
    size_t arg_count = 0;
    size_t arg_cap = 0;
    while (pos < end) {
        if (syntax_text_is(items[pos], "|") || syntax_text_is(items[pos], "->") || syntax_text_is(items[pos], "=>") || syntax_is_comma(items[pos])) break;
        if (out->kind != IDM_TYPE_CON) {
            idm_type_term_destroy(out);
            return idm_error_set(err, items[pos]->span, "type application head must be a constructor");
        }
        if (arg_count == arg_cap) {
            if (!idm_grow((void **)&args, &arg_cap, sizeof(*args), 2u, arg_count + 1u)) {
                idm_type_term_destroy(out);
                return idm_error_oom(err, items[pos]->span);
            }
        }
        memset(&args[arg_count], 0, sizeof(args[arg_count]));
        if (!parse_type_atom(ctx, items, pos, end, &pos, owner, &args[arg_count], err)) {
            for (size_t i = 0; i < arg_count; i++) idm_type_term_destroy(&args[i]);
            free(args);
            idm_type_term_destroy(out);
            return false;
        }
        arg_count++;
    }
    if (pos != end) {
        for (size_t i = 0; i < arg_count; i++) idm_type_term_destroy(&args[i]);
        free(args);
        idm_type_term_destroy(out);
        return idm_error_set(err, items[pos]->span, "unexpected token in type");
    }
    if (arg_count != 0) {
        out->args = args;
        out->arg_count = arg_count;
    } else {
        free(args);
    }
    return true;
}

static bool parse_type_term_range(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *owner, IdmTypeTerm *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (start >= end) return idm_error_set(err, idm_span_unknown(NULL), "expected type");
    size_t parts = 1u;
    for (size_t i = start; i < end; i++) if (syntax_text_is(items[i], "|")) parts++;
    if (parts == 1u) return parse_type_app(ctx, items, start, end, owner, out, err);
    IdmTypeTerm *args = calloc(parts, sizeof(*args));
    if (!args) return idm_error_oom(err, items[start]->span);
    size_t arg = 0;
    size_t part_start = start;
    for (size_t i = start; i <= end; i++) {
        if (i != end && !syntax_text_is(items[i], "|")) continue;
        if (i == part_start) {
            for (size_t j = 0; j < arg; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return idm_error_set(err, i < end ? items[i]->span : items[end - 1u]->span, "empty type union member");
        }
        if (!parse_type_app(ctx, items, part_start, i, owner, &args[arg], err)) {
            for (size_t j = 0; j < arg; j++) idm_type_term_destroy(&args[j]);
            free(args);
            return false;
        }
        arg++;
        part_start = i + 1u;
    }
    return idm_type_compound(out, IDM_TYPE_UNION, args, arg);
}

bool parse_type_term_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmTypeTerm *out, IdmError *err) {
    IdmCallableContract owner;
    memset(&owner, 0, sizeof(owner));
    bool ok = parse_type_term_range(ctx, items, start, end, &owner, out, err);
    idm_callable_contract_destroy(&owner);
    return ok;
}

static bool callable_contract_add_arg(IdmCallableContract *contract, const IdmTypeTerm *arg, IdmError *err, IdmSpan span) {
    if (contract->sig_count == 0 && !idm_contract_add_sig(contract)) return idm_error_oom(err, span);
    IdmContractSig *sig = &contract->sigs[contract->sig_count - 1u];
    if (!idm_grow((void **)&sig->args, &sig->arg_cap, sizeof(*sig->args), 4u, sig->arg_count + 1u)) return idm_error_oom(err, span);
    memset(&sig->args[sig->arg_count], 0, sizeof(sig->args[sig->arg_count]));
    if (!idm_type_term_copy(&sig->args[sig->arg_count], arg)) return idm_error_oom(err, span);
    sig->arg_count++;
    return true;
}

static bool callable_contract_add_constraint(IdmCallableContract *contract, IdmConstraint *constraint, IdmError *err, IdmSpan span) {
    if (!idm_grow((void **)&contract->context, &contract->context_cap, sizeof(*contract->context), 4u, contract->context_count + 1u)) return idm_error_oom(err, span);
    memset(&contract->context[contract->context_count], 0, sizeof(contract->context[contract->context_count]));
    if (!idm_constraint_copy(&contract->context[contract->context_count], constraint)) return idm_error_oom(err, span);
    contract->context_count++;
    return true;
}

static size_t contract_arg_field_end(IdmSyntax *const *items, size_t start, size_t end) {
    size_t pos = start + 1u;
    while (pos + 1u < end && syntax_text_is(items[pos], ".") && items[pos + 1u]->kind == IDM_SYN_WORD) pos += 2u;
    return pos;
}

static bool parse_contract_arg_fields(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *out, IdmError *err) {
    size_t pos = start;
    while (pos < end) {
        if (syntax_text_is(items[pos], "->")) return idm_error_set(err, items[pos]->span, "callable contract uses one '->' before the result type");
        if (syntax_is_comma(items[pos])) return idm_error_set(err, items[pos]->span, "callable contract fields use spaces, not comma");
        size_t field_end = contract_arg_field_end(items, pos, end);
        IdmTypeTerm term;
        if (!parse_type_term_range(ctx, items, pos, field_end, out, &term, err)) return false;
        if (!callable_contract_add_arg(out, &term, err, items[pos]->span)) {
            idm_type_term_destroy(&term);
            return false;
        }
        idm_type_term_destroy(&term);
        pos = field_end;
    }
    return true;
}

static bool parse_contract_context(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *out, IdmError *err) {
    size_t pos = start;
    while (pos < end) {
        if (syntax_is_comma(items[pos])) return idm_error_set(err, items[pos]->span, "typeclass constraints use spaces, not comma");
        IdmSyntax *trait = NULL;
        size_t trait_end = pos;
        if (!try_qualified_word_at(items, pos, end, &trait, &trait_end, err)) return false;
        if (!trait) return idm_error_set(err, items[pos]->span, "typeclass constraint expects a trait name");
        if (trait->as.text[0] == '_' && trait->as.text[1] == '.') {
            IdmSpan span = trait->span;
            char *head_text = structural_head_join(trait->as.text, items, end, &trait_end);
            idm_syn_free(trait);
            if (!head_text) return idm_error_oom(err, span);
            size_t arg_end = trait_end < end ? contract_arg_field_end(items, trait_end, end) : trait_end;
            if (arg_end == trait_end) {
                free(head_text);
                return idm_error_set(err, span, "structural constraint expects an argument type");
            }
            IdmConstraint constraint;
            memset(&constraint, 0, sizeof(constraint));
            constraint.kind = IDM_CONSTR_CLASS;
            constraint.trait = head_text;
            bool sok = parse_type_term_range(ctx, items, trait_end, arg_end, out, &constraint.lhs, err) &&
                       callable_contract_add_constraint(out, &constraint, err, span);
            idm_constraint_destroy(&constraint);
            if (!sok) return false;
            pos = arg_end;
            continue;
        }
        size_t arg_end = trait_end < end ? contract_arg_field_end(items, trait_end, end) : trait_end;
        if (arg_end == trait_end) {
            IdmSpan span = trait->span;
            idm_syn_free(trait);
            return idm_error_set(err, span, "typeclass constraint expects an argument type");
        }
        IdmConstraint constraint;
        memset(&constraint, 0, sizeof(constraint));
        constraint.kind = IDM_CONSTR_CLASS;
        const char *trait_name = trait->as.text;
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        TraitDef *trait_def = resolve_trait_def(ctx, trait, &status);
        if (status == IDM_RESOLVE_AMBIGUOUS) {
            IdmSpan span = trait->span;
            idm_syn_free(trait);
            return idm_error_set(err, span, "ambiguous trait '%s'", trait_name);
        }
        if (trait_def) trait_name = trait_def_identity_text(trait_def);
        else if (ctx->trait_name && ctx->trait_key &&
                 (strcmp(trait_name, ctx->trait_key) == 0 || strcmp(trait_name, ctx->trait_name) == 0)) {
            trait_name = ctx->trait_name;
        }
        constraint.trait = idm_strdup(trait_name);
        IdmSpan span = trait->span;
        idm_syn_free(trait);
        if (!constraint.trait) return idm_error_oom(err, span);
        bool ok = parse_type_term_range(ctx, items, trait_end, arg_end, out, &constraint.lhs, err) &&
                  callable_contract_add_constraint(out, &constraint, err, span);
        idm_constraint_destroy(&constraint);
        if (!ok) return false;
        pos = arg_end;
    }
    return true;
}

static bool contract_terms_resolved(ExpandContext *ctx, const IdmTypeTerm *term, IdmSpan span, IdmError *err) {
    if (!term) return true;
    if (term->kind == IDM_TYPE_CON && term->name && strcmp(term->name, "_") != 0 &&
        strcmp(term->name, "Any") != 0 &&
        !strchr(term->name, '#') && !idm_type_name_is_builtin(term->name)) {
        size_t n = strlen(term->name);
        for (size_t i = 0; i < ctx->typed.entity_count; i++) {
            const TypedEntity *e = &ctx->typed.entities[i];
            const char *id = NULL;
            if (e->kind == IDM_TYPED_ENTITY_TYPE) id = type_def_identity_text(&e->as.type);
            else if (e->kind == IDM_TYPED_ENTITY_TRAIT) id = trait_def_identity_text(&e->as.trait);
            if (!id || strncmp(id, term->name, n) != 0 || id[n] != '#') continue;
            if (e->kind == IDM_TYPED_ENTITY_TRAIT) {
                return expand_error(err, span, "unbound type '%s' in contract; '%s' is a trait — constrain with it before '=>' instead", term->name, term->name);
            }
            return expand_error(err, span, "unbound type '%s' in contract; a type named '%s' is declared but not visible here — reference it through its import alias", term->name, term->name);
        }
        return expand_error(err, span, "unbound type '%s' in contract", term->name);
    }
    for (size_t i = 0; i < term->arg_count; i++) {
        if (!contract_terms_resolved(ctx, &term->args[i], span, err)) return false;
    }
    return true;
}

static bool contract_sigs_resolved(ExpandContext *ctx, const IdmCallableContract *out, IdmSpan span, IdmError *err) {
    for (size_t s = 0; s < out->sig_count; s++) {
        for (size_t a = 0; a < out->sigs[s].arg_count; a++) {
            if (!contract_terms_resolved(ctx, &out->sigs[s].args[a], span, err)) return false;
        }
        if (out->sigs[s].has_result && !contract_terms_resolved(ctx, &out->sigs[s].result, span, err)) return false;
    }
    return true;
}

bool parse_callable_contract_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmCallableContract *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    size_t body_start = start;
    for (size_t i = start; i < end; i++) {
        if (!syntax_text_is(items[i], "=>")) continue;
        if (!parse_contract_context(ctx, items, start, i, out, err)) goto fail;
        body_start = i + 1u;
        break;
    }
    if (body_start >= end) {
        idm_error_set(err, start < end ? items[start]->span : idm_span_unknown(NULL), "callable contract expects a type");
        goto fail;
    }
    size_t result_arrow = SIZE_MAX;
    for (size_t i = body_start; i < end; i++) {
        if (syntax_text_is(items[i], "->")) result_arrow = i;
    }
    if (result_arrow == SIZE_MAX) {
        if (out->sig_count == 0 && !idm_contract_add_sig(out)) { idm_error_oom(err, idm_span_unknown(NULL)); goto fail; }
        if (!parse_type_term_range(ctx, items, body_start, end, out, &out->sigs[out->sig_count - 1u].result, err)) goto fail;
        out->sigs[out->sig_count - 1u].has_result = true;
        if (!contract_sigs_resolved(ctx, out, items[start]->span, err)) goto fail;
        return true;
    }
    if (result_arrow == body_start || result_arrow + 1u >= end) {
        idm_error_set(err, items[result_arrow]->span, "callable contract expects argument types before '->' and a result type after it");
        goto fail;
    }
    if (!parse_contract_arg_fields(ctx, items, body_start, result_arrow, out, err)) goto fail;
    if (out->sig_count == 0 && !idm_contract_add_sig(out)) { idm_error_oom(err, idm_span_unknown(NULL)); goto fail; }
    if (!parse_type_term_range(ctx, items, result_arrow + 1u, end, out, &out->sigs[out->sig_count - 1u].result, err)) goto fail;
    out->sigs[out->sig_count - 1u].has_result = true;
    if (!contract_sigs_resolved(ctx, out, items[start]->span, err)) goto fail;
    return true;

fail:
    idm_callable_contract_destroy(out);
    return false;
}

bool register_operator(ExpandContext *ctx, const char *name, const char *capture, uint8_t precedence, IdmOpAssoc assoc, uint8_t action, const char *target_name, const IdmScopeSet *scopes, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, bool exported, IdmSpan span, IdmError *err) {
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
    const IdmBinding *existing = NULL;
    IdmResolveStatus existing_status = resolve_scoped(ctx, key, IDM_BIND_SPACE_OPERATOR, binding_scopes, NULL, &existing);
    if (existing_status == IDM_RESOLVE_OK && existing && existing->kind == IDM_BIND_OPERATOR && existing->payload < ctx->operator_count) {
        const IdmOperatorDef *prev = &ctx->operators[existing->payload];
        if (prev->action == IDM_OPERATOR_ACTION_METHOD || action == IDM_OPERATOR_ACTION_METHOD) {
            if (prev->precedence != precedence || prev->assoc != assoc) {
                free(key);
                return idm_error_set(err, span, "operator '%s' already declared with different precedence or associativity", name);
            }
            if (prev->action == IDM_OPERATOR_ACTION_METHOD && action == IDM_OPERATOR_ACTION_METHOD &&
                prev->target_name && strcmp(prev->target_name, target_name) == 0) {
                free(key);
                return true;
            }
        }
    }
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
    op->action = action;
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

const char *method_impl_trait_text(const MethodImplDef *impl) {
    return impl && impl->trait ? idm_symbol_text(impl->trait) : NULL;
}

const char *method_impl_name_text(const MethodImplDef *impl) {
    return impl && impl->name ? idm_symbol_text(impl->name) : NULL;
}

bool method_impl_set_identity(ExpandContext *ctx, MethodImplDef *impl, const char *trait, const char *name, const char *type, IdmError *err, IdmSpan span) {
    if (!ctx || !impl || !trait || !name || !type) return idm_error_set(err, span, "method implementation requires trait, method, and receiver type");
    IdmSymbol *trait_sym = typed_atom_intern(ctx, trait);
    IdmSymbol *name_sym = typed_atom_intern(ctx, name);
    IdmSymbol *type_sym = typed_atom_intern(ctx, type);
    if (!trait_sym || !name_sym || !type_sym) return idm_error_oom(err, span);
    impl->trait = trait_sym;
    impl->name = name_sym;
    impl->type = type_sym;
    return true;
}

bool method_impl_set_type(ExpandContext *ctx, MethodImplDef *impl, const char *type, IdmError *err, IdmSpan span) {
    if (!ctx || !impl || !type) return idm_error_set(err, span, "method implementation requires a receiver type");
    IdmSymbol *sym = typed_atom_intern(ctx, type);
    if (!sym) return idm_error_oom(err, span);
    impl->type = sym;
    return true;
}

char *structural_head_join(const char *head, IdmSyntax *const *items, size_t count, size_t *inout_pos) {
    if (!head || head[0] != '_' || head[1] != '.') return NULL;
    size_t pos = *inout_pos;
    const char *field_type = pos + 1u < count && syntax_text_is(items[pos], "::") ? surface_token_text(items[pos + 1u]) : NULL;
    IdmBuffer joined;
    idm_buf_init(&joined);
    bool ok = idm_buf_append(&joined, head);
    if (ok && field_type) {
        ok = idm_buf_append(&joined, "::") && idm_buf_append(&joined, field_type);
        pos += 2u;
    }
    char *text = ok ? idm_strdup(joined.data) : NULL;
    idm_buf_destroy(&joined);
    if (!text) return NULL;
    *inout_pos = pos;
    return text;
}

bool structural_head_parse(const char *head, const char **out_field, size_t *out_field_len, const char **out_type) {
    if (!head || head[0] != '_' || head[1] != '.') return false;
    const char *f = head + 2;
    const char *sep = strstr(f, "::");
    *out_field = f;
    *out_field_len = sep ? (size_t)(sep - f) : strlen(f);
    *out_type = sep ? sep + 2 : NULL;
    return *out_field_len != 0;
}

static bool field_term_satisfies_name(const ExpandContext *ctx, const char *expected, const IdmTypeTerm *t) {
    if (!expected) return true;
    if (!t) return false;
    if (t->kind == IDM_TYPE_UNION) {
        for (size_t i = 0; i < t->arg_count; i++) {
            if (!field_term_satisfies_name(ctx, expected, &t->args[i])) return false;
        }
        return t->arg_count != 0;
    }
    if (t->kind != IDM_TYPE_CON || !t->name) return false;
    if (type_name_same(ctx, expected, t->name)) return true;
    const TypeDef *td = typed_type_by_identity_or_name(ctx, expected);
    if (td) {
        for (size_t i = 0; i < td->member_count; i++) {
            const IdmTypeTerm *m = &td->members[i].term;
            if (m->kind == IDM_TYPE_CON && m->name && type_name_same(ctx, m->name, t->name)) return true;
        }
    }
    return false;
}

bool type_satisfies_structural_head(const ExpandContext *ctx, const char *head, const char *type_name) {
    const char *field = NULL;
    size_t flen = 0;
    const char *ftype = NULL;
    if (!structural_head_parse(head, &field, &flen, &ftype)) return false;
    const TypeDef *td = typed_type_by_identity_or_name(ctx, type_name);
    if (!td) return false;
    if (td->field_count) {
        for (size_t i = 0; i < td->field_count; i++) {
            const char *fname = td->fields[i].name ? idm_symbol_text(td->fields[i].name) : NULL;
            if (!fname || strncmp(fname, field, flen) != 0 || fname[flen] != '\0') continue;
            if (!ftype) return true;
            return td->fields[i].has_contract && field_term_satisfies_name(ctx, ftype, &td->fields[i].contract);
        }
        return false;
    }
    if (td->member_count) {
        for (size_t i = 0; i < td->member_count; i++) {
            const IdmTypeTerm *m = &td->members[i].term;
            if (m->kind != IDM_TYPE_CON || !m->name) return false;
            if (!type_satisfies_structural_head(ctx, head, m->name)) return false;
        }
        return true;
    }
    return false;
}

FieldSelectorDef *field_selector_lookup(ExpandContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->field_selector_count; i++) {
        if (strcmp(ctx->field_selectors[i].name, name) == 0) return &ctx->field_selectors[i];
    }
    return NULL;
}

FieldSelectorDef *field_selector_ensure(ExpandContext *ctx, const char *name, IdmError *err) {
    FieldSelectorDef *found = field_selector_lookup(ctx, name);
    if (found) return found;
    if (ctx->field_selector_count == ctx->field_selector_cap &&
        !idm_grow((void **)&ctx->field_selectors, &ctx->field_selector_cap, sizeof(*ctx->field_selectors), 8u, ctx->field_selector_count + 1u)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    FieldSelectorDef *sel = &ctx->field_selectors[ctx->field_selector_count];
    memset(sel, 0, sizeof(*sel));
    sel->name = idm_strdup(name);
    if (!sel->name) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    sel->env = ctx->frame == IDM_FRAME_TOP;
    sel->slot = sel->env ? ctx->env_slot_seq++ : ctx->next_slot++;
    ctx->field_selector_count++;
    return &ctx->field_selectors[ctx->field_selector_count - 1u];
}

IdmEnv *expand_unit_runtime_env(ExpandContext *ctx) {
    if (ctx->in_package && ctx->unit_key[0]) return idm_package_env_get_or_create(ctx->rt, ctx->unit_key);
    if (ctx->phase != 0 && ctx->phase_env) return ctx->phase_env->env;
    return ctx->rt->main_env;
}

void expand_seed_const_defn(ExpandContext *ctx, uint32_t slot, IdmCore *value) {
    if (ctx->frame != IDM_FRAME_TOP) return;
    if (!value || (value->kind != IDM_CORE_FN && value->kind != IDM_CORE_FN_MULTI)) return;
    size_t capture_count = value->kind == IDM_CORE_FN ? value->as.fn.capture_count : value->as.fn_multi.capture_count;
    if (capture_count != 0) return;
    IdmEnv *env = expand_unit_runtime_env(ctx);
    if (!env) return;
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) return;
    idm_bc_init(module);
    uint32_t main_fn = 0;
    IdmError probe;
    idm_error_init(&probe);
    IdmValue closure = idm_nil();
    bool ok = idm_core_compile_main(ctx->rt, value, module, &main_fn, &probe) &&
              idm_bc_intern_literals(ctx->rt, module, &probe) &&
              idm_vm_run_in_env(ctx->rt, module, main_fn, env, &closure, &probe) &&
              idm_is_closure(closure) &&
              idm_env_slot_ensure(env, slot, &probe) &&
              idm_env_slot_set(ctx->rt, env, slot, closure, &probe);
    idm_error_clear(&probe);
    if (!ok) {
        idm_bc_destroy(module);
        free(module);
        return;
    }
    idm_runtime_retire_module(ctx->rt, module);
}

bool method_impl_matches_type(const ExpandContext *ctx, const MethodImplDef *impl, const char *type) {
    if (!impl || !impl->type) return false;
    const char *impl_type = method_impl_type_text(impl);
    if (impl_type && impl_type[0] == '_' && impl_type[1] == '.') return type && type_satisfies_structural_head(ctx, impl_type, type);
    return type_name_same(ctx, impl_type, type);
}

bool method_impl_matches_trait(const ExpandContext *ctx, const MethodImplDef *impl, const char *trait) {
    IdmSymbol *sym = typed_identity_lookup(ctx, trait);
    if (impl && impl->trait && sym) return impl->trait == sym;
    const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl ? impl->method_surface : UINT32_MAX);
    return method_surface_matches_trait(ctx, surface, trait);
}

bool method_impl_same_type(const MethodImplDef *a, const MethodImplDef *b) {
    return a && b && a->type && a->type == b->type;
}

bool method_impl_matches_identity(const ExpandContext *ctx, const MethodImplDef *impl, const char *trait, const char *name, const char *type) {
    if (!impl || (type && !method_impl_matches_type(ctx, impl, type))) return false;
    IdmSymbol *trait_sym = typed_identity_lookup(ctx, trait);
    IdmSymbol *name_sym = typed_identity_lookup(ctx, name);
    if (impl->trait && impl->name && trait_sym && name_sym) return impl->trait == trait_sym && impl->name == name_sym;
    const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl->method_surface);
    return method_surface_matches_identity(ctx, surface, trait, name);
}

static void refresh_method_impl_surface_cache(ExpandContext *ctx, const char *trait, const char *name, uint32_t method_surface) {
    if (!ctx || !trait || !name || method_surface == UINT32_MAX) return;
    if (ctx->suppress_method_impl_surface_refresh) return;
    for (size_t i = 0; i < ctx->typed.method_impl_count; i++) {
        MethodImplDef *impl = &ctx->typed.method_impls[i];
        if (method_impl_matches_identity(ctx, impl, trait, name, NULL)) impl->method_surface = method_surface;
    }
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
        MethodSurfaceDef *e = &ctx->typed.method_surfaces[i];
        if (!method_surface_matches_identity(ctx, e, trait, name) || !idm_scope_set_equal(&e->scopes, check_scopes)) continue;
        {
            if (!idm_arity_equal(&e->arity, &arity)) {
                return idm_error_set(err, idm_span_unknown(NULL), "method surface '%s.%s' arity changed while activating this scope", trait, name);
            }
            const char *e_provider = e->provider ? e->provider : "";
            const char *e_provider_key = e->provider_key ? e->provider_key : "";
            const char *new_provider = provider ? provider : "";
            const char *new_provider_key = provider_key ? provider_key : "";
            if (strcmp(e_provider, new_provider) == 0 && strcmp(e_provider_key, new_provider_key) == 0) {
                if (has_dispatch && !e->has_dispatch) {
                    char *next_key = idm_strdup(dispatch_env_key ? dispatch_env_key : "");
                    if (!next_key) return idm_error_oom(err, idm_span_unknown(NULL));
                    free(e->dispatch_env_key);
                    e->dispatch_env_key = next_key;
                    e->has_dispatch = true;
                    e->dispatch_env = dispatch_env;
                    e->dispatch_frame = ctx->frame;
                    e->dispatch_slot = dispatch_slot;
                }
                refresh_method_impl_surface_cache(ctx, trait, name, (uint32_t)i);
                return true;
            }
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
    refresh_method_impl_surface_cache(ctx, trait, name, (uint32_t)ctx->typed.method_surface_count);
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
        if (!field_selector_ensure(ctx, name, err)) return false;
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

bool binder_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out) {
    if (!syntax_scopes_copy(out, name_syntax)) return false;
    if (ctx->rt->macro_intro_active) idm_scope_set_remove(out, ctx->rt->macro_intro_scope);
    return true;
}

bool local_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, const IdmCallableContract *contract, uint32_t *out_slot) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t slot = ctx->next_slot++;
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_LOCAL, &scopes, slot, ctx->frame, arity, &binding_id);
    if (ok && contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, contract);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

bool local_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_slot) {
    return local_push_def_binder_with_arity(ctx, name, name_syntax, idm_arity_unknown(), NULL, out_slot);
}

bool env_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, const IdmCallableContract *contract, uint32_t *out_id) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t id = ctx->env_slot_seq++;
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, id, IDM_FRAME_ENV, arity, &binding_id);
    if (ok && contract) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, contract);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_id) *out_id = id;
    return true;
}

bool env_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_id) {
    return env_push_def_binder_with_arity(ctx, name, name_syntax, idm_arity_unknown(), NULL, out_id);
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

bool callable_contract_from_value_type(const IdmTypeTerm *type, IdmCallableContract *out) {
    memset(out, 0, sizeof(*out));
    if (!type) return true;
    IdmContractSig *sig = idm_contract_add_sig(out);
    if (!sig) return false;
    if (!idm_type_term_copy(&sig->result, type)) return false;
    sig->has_result = true;
    return true;
}

bool arg_push_slot_with_type(ExpandContext *ctx, const IdmSyntax *word, uint32_t slot, const IdmTypeTerm *type) {
    const IdmBinding *existing = resolve_default(ctx, word, NULL);
    if (existing && existing->kind == IDM_BIND_ARG && existing->frame_id == ctx->frame) {
        if (!type || existing->has_contract) return true;
        IdmCallableContract contract;
        if (!callable_contract_from_value_type(type, &contract)) return false;
        bool ok = idm_binding_table_set_contract(&ctx->bindings, existing->id, &contract);
        idm_callable_contract_destroy(&contract);
        return ok;
    }
    IdmScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, word)) return false;
    IdmBindingId binding_id = 0;
    bool ok = idm_binding_table_add(&ctx->bindings, word->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ARG, &scopes, slot, ctx->frame, &binding_id);
    if (ok && type) {
        IdmCallableContract contract;
        ok = callable_contract_from_value_type(type, &contract);
        if (ok) ok = idm_binding_table_set_contract(&ctx->bindings, binding_id, &contract);
        idm_callable_contract_destroy(&contract);
    }
    idm_scope_set_destroy(&scopes);
    return ok;
}

bool arg_push_slot(ExpandContext *ctx, const IdmSyntax *word, uint32_t slot) {
    return arg_push_slot_with_type(ctx, word, slot, NULL);
}

static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap) {
    if (*count != *cap) return true;
    return idm_grow((void **)arr, cap, sizeof(**arr), 4u, *count + 1u);
}

static int capture_append_name(CaptureBinding **arr, size_t *count, size_t *cap, const char *name, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmBindingId binding_id, IdmArity arity, const IdmCallableContract *contract) {
    if (!capture_array_grow(arr, count, cap)) return -1;
    CaptureBinding *c = &(*arr)[*count];
    memset(c, 0, sizeof(*c));
    c->name = idm_strdup(name);
    if (!c->name) return -1;
    if (!idm_scope_set_copy(&c->scopes, scopes)) {
        free(c->name);
        return -1;
    }
    c->kind = kind;
    c->source_index = source_index;
    c->capture_index = (uint32_t)*count;
    c->binding_id = binding_id;
    c->arity = arity;
    if (contract) {
        if (!idm_callable_contract_copy(&c->contract, contract)) {
            free(c->name);
            idm_scope_set_destroy(&c->scopes);
            return -1;
        }
        c->has_contract = true;
    }
    (*count)++;
    return (int)c->capture_index;
}

static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmBindingId binding_id, IdmArity arity, const IdmCallableContract *contract) {
    return capture_append_name(arr, count, cap, word->as.text, scopes, kind, source_index, binding_id, arity, contract);
}

static int capture_append_slot(CaptureBinding **arr, size_t *count, size_t *cap, const char *name, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity) {
    int idx = capture_append_name(arr, count, cap, name, scopes, kind, source_index, 0, arity, NULL);
    if (idx >= 0) (*arr)[idx].slot_synthetic = true;
    return idx;
}

static const CaptureBinding *capture_lookup_existing_slot(const CaptureBinding *captures, size_t count, const char *name, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index) {
    for (size_t i = 0; i < count; i++) {
        if (captures[i].kind == kind && captures[i].source_index == source_index &&
            strcmp(captures[i].name, name) == 0 && idm_scope_set_subset(&captures[i].scopes, scopes)) return &captures[i];
    }
    return NULL;
}

const CaptureBinding *capture_lookup_existing(const CaptureBinding *captures, size_t count, const IdmSyntax *word) {
    for (size_t i = 0; i < count; i++) {
        if (captures[i].slot_synthetic) continue;
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
        return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, kind, b->payload, b->id, b->arity, b->has_contract ? &b->contract : NULL);
    }
    int parent = saved_materialize(g->prev, word, b);
    if (parent < 0) return -1;
    return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, IDM_CAP_UPVALUE, (uint32_t)parent, b->id, b->arity, b->has_contract ? &b->contract : NULL);
}

static int saved_materialize_slot(SavedFunctionContext *g, const char *name, const IdmScopeSet *scopes, uint32_t frame_id, uint32_t slot, IdmArity arity) {
    if (!g->prev) return -1;
    if (g->prev->frame == frame_id) {
        const CaptureBinding *existing = capture_lookup_existing_slot(g->captures, g->capture_count, name, scopes, IDM_CAP_LOCAL, slot);
        if (existing) return (int)existing->capture_index;
        return capture_append_slot(&g->captures, &g->capture_count, &g->capture_cap, name, scopes, IDM_CAP_LOCAL, slot, arity);
    }
    int parent = saved_materialize_slot(g->prev, name, scopes, frame_id, slot, arity);
    if (parent < 0) return -1;
    const CaptureBinding *existing = capture_lookup_existing_slot(g->captures, g->capture_count, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent);
    if (existing) return (int)existing->capture_index;
    return capture_append_slot(&g->captures, &g->capture_count, &g->capture_cap, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent, arity);
}

bool materialize_capture(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *b, uint32_t *out) {
    if (!ctx->enclosing) return false;
    int idx;
    if (ctx->enclosing->frame == b->frame_id) {
        IdmCaptureKind kind = b->kind == IDM_BIND_ARG ? IDM_CAP_ARG : IDM_CAP_LOCAL;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, kind, b->payload, b->id, b->arity, b->has_contract ? &b->contract : NULL);
    } else {
        int parent = saved_materialize(ctx->enclosing, word, b);
        if (parent < 0) return false;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, IDM_CAP_UPVALUE, (uint32_t)parent, b->id, b->arity, b->has_contract ? &b->contract : NULL);
    }
    if (idx < 0) return false;
    *out = (uint32_t)idx;
    return true;
}

bool materialize_slot_capture(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, uint32_t frame_id, uint32_t slot, IdmArity arity, uint32_t *out) {
    if (!ctx->enclosing) return false;
    int idx;
    if (ctx->enclosing->frame == frame_id) {
        const CaptureBinding *existing = capture_lookup_existing_slot(ctx->captures, ctx->capture_count, name, scopes, IDM_CAP_LOCAL, slot);
        if (existing) {
            *out = existing->capture_index;
            return true;
        }
        idx = capture_append_slot(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, name, scopes, IDM_CAP_LOCAL, slot, arity);
    } else {
        int parent = saved_materialize_slot(ctx->enclosing, name, scopes, frame_id, slot, arity);
        if (parent < 0) return false;
        const CaptureBinding *existing = capture_lookup_existing_slot(ctx->captures, ctx->capture_count, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent);
        if (existing) {
            *out = existing->capture_index;
            return true;
        }
        idx = capture_append_slot(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, name, scopes, IDM_CAP_UPVALUE, (uint32_t)parent, arity);
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
    saved->function_contract = ctx->function_contract;
    saved->pin_collect = ctx->pin_collect;
    ctx->enclosing = saved;
    ctx->frame = ++ctx->frame_seq;
    ctx->next_slot = 0;
    ctx->arg_slots = 0;
    ctx->captures = NULL;
    ctx->capture_count = 0;
    ctx->capture_cap = 0;
    ctx->function_contract = NULL;
    ctx->pin_collect = NULL;
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
    ctx->function_contract = saved->function_contract;
    ctx->pin_collect = saved->pin_collect;
}

void bits_binders_truncate(ExpandContext *ctx, size_t base) {
    while (ctx->bits_binder_count > base) {
        free(ctx->bits_binder_names[--ctx->bits_binder_count]);
    }
}

void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    saved->table_base = ctx->bindings.count;
    saved->next_slot = ctx->next_slot;
    saved->arg_slots = ctx->arg_slots;
    saved->bits_binder_base = ctx->bits_binder_count;
    saved->clause_table_base = ctx->clause_table_base;
    ctx->clause_table_base = ctx->bindings.count;
    ctx->next_slot = 0;
    ctx->arg_slots = 0;
}

void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    bits_binders_truncate(ctx, saved->bits_binder_base);
    idm_binding_table_truncate(&ctx->bindings, saved->table_base);
    ctx->clause_table_base = saved->clause_table_base;
    ctx->next_slot = saved->next_slot;
    ctx->arg_slots = saved->arg_slots;
}

bool syn_is_word(const IdmSyntax *syn, const char *word) {
    return syn && syn->kind == IDM_SYN_WORD && strcmp(syn->as.text, word) == 0;
}

const char *surface_token_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD) return syn->as.text;
    if (idm_syn_is_form_id(syn, IDM_FORM_WORD) && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

static bool surface_token_text_append(IdmBuffer *buf, const IdmSyntax *syn) {
    const char *direct = surface_token_text(syn);
    if (direct) return idm_buf_append(buf, direct);
    if (!idm_syn_is_form_id(syn, IDM_FORM_ADJACENT) || syn->as.seq.count < 2u) return false;
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
    }
    return "unknown";
}

static IdmValue arity_surface_value(IdmRuntime *rt, IdmArity arity, IdmError *err) {
    IdmDictEntry entries[3];
    entries[0].key = idm_atom(rt, "kind");
    entries[0].value = idm_atom(rt, arity_kind_atom_name(arity.kind));
    entries[1].key = idm_atom(rt, "min");
    entries[1].value = idm_int((int64_t)arity.min);
    entries[2].key = idm_atom(rt, "max");
    entries[2].value = idm_int((int64_t)arity.max);
    return idm_dict(rt, entries, 3u, err);
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

static IdmValue binding_provider_atom(IdmRuntime *rt, const IdmBinding *binding) {
    return idm_atom(rt, binding && binding->provider ? binding->provider : "local");
}

static bool surface_binding_repeated(const ExpandContext *ctx, size_t index) {
    const IdmBinding *b = &ctx->bindings.items[index];
    for (size_t j = index + 1u; j < ctx->bindings.count; j++) {
        const IdmBinding *later = &ctx->bindings.items[j];
        if (later->space != b->space || later->kind != b->kind) continue;
        if (strcmp(later->name ? later->name : "", b->name ? b->name : "") != 0) continue;
        const char *bp = b->provider ? b->provider : "";
        const char *lp = later->provider ? later->provider : "";
        if (strcmp(bp, lp) == 0) return true;
    }
    return false;
}

bool expand_surface_value(ExpandContext *ctx, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err) {
    IdmValue acc = idm_empty_list();
    if (strcmp(kind, "operators") == 0) {
        for (size_t i = ctx->bindings.count; i > 0; i--) {
            const IdmBinding *b = &ctx->bindings.items[i - 1u];
            if (b->space != IDM_BIND_SPACE_OPERATOR || b->kind != IDM_BIND_OPERATOR || b->payload >= ctx->operator_count) continue;
            if (surface_binding_repeated(ctx, i - 1u)) continue;
            const IdmOperatorDef *op = &ctx->operators[b->payload];
            IdmValue items[5];
            items[0] = idm_atom(rt, op->name);
            items[1] = idm_int((int64_t)op->precedence);
            items[2] = idm_atom(rt, assoc_atom_name(op->assoc));
            items[3] = idm_string(rt, op->capture ? op->capture : "value", err);
            items[4] = binding_provider_atom(rt, b);
            if (err && err->present) return false;
            IdmValue entry = idm_tuple(rt, items, 5u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "macros") == 0) {
        for (size_t i = ctx->bindings.count; i > 0; i--) {
            const IdmBinding *b = &ctx->bindings.items[i - 1u];
            if (b->space != IDM_BIND_SPACE_DEFAULT || b->kind != IDM_BIND_TRANSFORMER) continue;
            if (surface_binding_repeated(ctx, i - 1u)) continue;
            if (b->payload < ctx->macro_count && ctx->macros[b->payload].hidden) continue;
            IdmValue items[2];
            items[0] = idm_atom(rt, b->name);
            items[1] = binding_provider_atom(rt, b);
            IdmValue entry = idm_tuple(rt, items, 2u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "core-form") == 0) {
        for (size_t i = ctx->bindings.count; i > 0; i--) {
            const IdmBinding *b = &ctx->bindings.items[i - 1u];
            if (b->space != IDM_BIND_SPACE_CORE_FORM) continue;
            if (surface_binding_repeated(ctx, i - 1u)) continue;
            IdmValue items[2];
            items[0] = idm_atom(rt, b->name ? b->name : "_");
            items[1] = binding_provider_atom(rt, b);
            IdmValue entry = idm_tuple(rt, items, 2u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
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
            const IdmBinding *binding = NULL;
            (void)resolve_scoped(ctx, p->name, IDM_BIND_SPACE_PROTOCOL, NULL, NULL, &binding);
            IdmValue items[2];
            items[0] = idm_atom(rt, p->name);
            items[1] = binding_provider_atom(rt, binding);
            IdmValue entry = idm_tuple(rt, items, 2u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
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
        return idm_error_set(err, idm_span_unknown(NULL), "surface kind must be :operators, :macros, :protocols, :core-form, :grammar-artifacts, :methods, or :active");
    }
    *out = acc;
    return true;
}
