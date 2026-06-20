#include "internal.h"

static void capture_bindings_destroy(CaptureBinding *captures, size_t count);
static void method_surface_def_destroy(MethodSurfaceDef *method);
static void grammar_def_destroy(GrammarDef *grammar);
static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap);
static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity);
static int saved_materialize(SavedFunctionContext *g, const IdmSyntax *word, const IdmBinding *b);
static const IdmOperatorDef *op_lookup_capture(const ExpandContext *ctx, const IdmSyntax *syn, const char *capture);
static const IdmBinding *resolve_surface_binding(const ExpandContext *ctx, const IdmSyntax *word, IdmBindingSpace space, IdmBindingKind kind, IdmResolveStatus *out_status);

void ctx_init(ExpandContext *ctx, IdmRuntime *rt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rt = rt;
    idm_binding_table_init(&ctx->bindings);
    idm_scope_set_init(&ctx->empty_scopes);
    ctx->frame = 1u;
    ctx->frame_seq = 1u;
    ctx->local_runner.user = ctx;
    ctx->local_runner.invoke = local_macro_invoke;
    idm_scope_store_init(&ctx->scope_store);
    ctx->surface_phase = -1;
    ctx->phase_ns = rt->main_ns;
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
        saved->register_operator_user = rt->register_operator_user;
        saved->register_operator = rt->register_operator;
        saved->register_macro_user = rt->register_macro_user;
        saved->register_macro = rt->register_macro;
        saved->expander_surface_user = rt->expander_surface_user;
        saved->expander_surface = rt->expander_surface;
    }
    rt->local_expand_user = ctx;
    rt->local_expand = local_expand_callback;
    rt->free_identifier_eq_user = ctx;
    rt->free_identifier_eq = free_identifier_eq_callback;
    rt->register_operator_user = ctx;
    rt->register_operator = register_operator_callback;
    rt->register_macro_user = ctx;
    rt->register_macro = register_macro_callback;
    rt->expander_surface_user = ctx;
    rt->expander_surface = expander_surface_callback;
}

void hooks_restore(IdmRuntime *rt, const SavedHooks *saved) {
    rt->local_expand_user = saved->local_expand_user;
    rt->local_expand = saved->local_expand;
    rt->free_identifier_eq_user = saved->free_identifier_eq_user;
    rt->free_identifier_eq = saved->free_identifier_eq;
    rt->register_operator_user = saved->register_operator_user;
    rt->register_operator = saved->register_operator;
    rt->register_macro_user = saved->register_macro_user;
    rt->register_macro = saved->register_macro;
    rt->expander_surface_user = saved->expander_surface_user;
    rt->expander_surface = saved->expander_surface;
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

static void grammar_def_destroy(GrammarDef *grammar) {
    if (!grammar) return;
    idm_scope_set_destroy(&grammar->scopes);
    phase_syntax_fn_destroy(&grammar->fn);
    memset(grammar, 0, sizeof(*grammar));
}

static void method_surface_def_destroy(MethodSurfaceDef *method) {
    if (!method) return;
    free(method->trait);
    free(method->name);
    free(method->provider);
    free(method->provider_key);
    idm_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

void protocol_def_destroy(ProtocolDef *protocol) {
    if (!protocol) return;
    free(protocol->name);
    free(protocol->public_name);
    idm_artifact_destroy(&protocol->art);
    memset(protocol, 0, sizeof(*protocol));
}

void trait_def_destroy(TraitDef *trait) {
    if (!trait) return;
    free(trait->name);
    for (size_t i = 0; i < trait->requirement_count; i++) idm_trait_requirement_def_destroy(&trait->requirements[i]);
    free(trait->requirements);
    for (size_t i = 0; i < trait->method_count; i++) idm_trait_method_def_destroy(&trait->methods[i]);
    free(trait->methods);
    memset(trait, 0, sizeof(*trait));
}

void type_def_destroy(TypeDef *type) {
    if (!type) return;
    free(type->name);
    free(type->identity);
    idm_scope_set_destroy(&type->scopes);
    for (size_t i = 0; i < type->field_count; i++) free(type->fields[i]);
    free(type->fields);
    memset(type, 0, sizeof(*type));
}

void use_package_transfer_init(UsePackageTransfer *transfer) {
    memset(transfer, 0, sizeof(*transfer));
}

void use_package_transfer_destroy(UsePackageTransfer *transfer) {
    if (!transfer) return;
    free(transfer->src);
    free(transfer->dst);
    for (size_t i = 0; i < transfer->count; i++) free(transfer->names[i]);
    free(transfer->names);
    memset(transfer, 0, sizeof(*transfer));
}

bool use_package_transfer_add(UsePackageTransfer *transfer, uint32_t src, uint32_t dst, const char *name, IdmSpan span, IdmError *err) {
    if (transfer->count == transfer->cap) {
        size_t cap = transfer->cap ? transfer->cap * 2u : 4u;
        uint32_t *next_src = realloc(transfer->src, cap * sizeof(*next_src));
        if (!next_src) return idm_error_oom(err, span);
        transfer->src = next_src;
        uint32_t *next_dst = realloc(transfer->dst, cap * sizeof(*next_dst));
        if (!next_dst) return idm_error_oom(err, span);
        transfer->dst = next_dst;
        char **next_names = realloc(transfer->names, cap * sizeof(*next_names));
        if (!next_names) return idm_error_oom(err, span);
        transfer->names = next_names;
        transfer->cap = cap;
    }
    transfer->src[transfer->count] = src;
    transfer->dst[transfer->count] = dst;
    transfer->names[transfer->count] = idm_strdup(name);
    if (!transfer->names[transfer->count]) return idm_error_oom(err, span);
    transfer->count++;
    return true;
}

bool record_activation(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err) {
    if (ctx->activation_count == ctx->activation_cap) {
        size_t cap = ctx->activation_cap ? ctx->activation_cap * 2u : 4u;
        void *grown = realloc(ctx->activations, cap * sizeof(*ctx->activations));
        if (!grown) return idm_error_oom(err, span);
        ctx->activations = grown;
        ctx->activation_cap = cap;
    }
    ctx->activations[ctx->activation_count].name = idm_strdup(name);
    if (!ctx->activations[ctx->activation_count].name) return idm_error_oom(err, span);
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
        size_t cap = ctx->surface_install_cap ? ctx->surface_install_cap * 2u : 8u;
        SurfaceInstall *next = realloc(ctx->surface_installs, cap * sizeof(*next));
        if (!next) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return -1;
        }
        ctx->surface_installs = next;
        ctx->surface_install_cap = cap;
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

void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint) {
    checkpoint->binding_count = ctx->bindings.count;
    checkpoint->macro_count = ctx->macro_count;
    checkpoint->grammar_count = ctx->grammar_count;
    checkpoint->operator_count = ctx->operator_count;
    checkpoint->protocol_count = ctx->protocol_count;
    checkpoint->trait_count = ctx->trait_count;
    checkpoint->type_count = ctx->type_count;
    checkpoint->method_surface_count = ctx->method_surface_count;
    checkpoint->decl_method_count = ctx->decl_method_count;
    checkpoint->decl_grammar = ctx->decl_grammar;
    checkpoint->activation_count = ctx->activation_count;
    checkpoint->surface_install_count = ctx->surface_install_count;
    checkpoint->artifact_base_count = ctx->artifact_base_count;
    checkpoint->runtime_import_count = ctx->runtime_import_count;
}

void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint) {
    while (ctx->activation_count > checkpoint->activation_count) free(ctx->activations[--ctx->activation_count].name);
    while (ctx->surface_install_count > checkpoint->surface_install_count) surface_install_destroy(&ctx->surface_installs[--ctx->surface_install_count]);
    if (ctx->artifact_base_count > checkpoint->artifact_base_count) ctx->artifact_base_count = checkpoint->artifact_base_count;
    if (ctx->runtime_import_count > checkpoint->runtime_import_count) ctx->runtime_import_count = checkpoint->runtime_import_count;
    while (ctx->grammar_count > checkpoint->grammar_count) grammar_def_destroy(&ctx->grammars[--ctx->grammar_count]);
    while (ctx->macro_count > checkpoint->macro_count) macro_def_destroy(&ctx->macros[--ctx->macro_count]);
    while (ctx->operator_count > checkpoint->operator_count) idm_operator_def_destroy(&ctx->operators[--ctx->operator_count]);
    while (ctx->protocol_count > checkpoint->protocol_count) protocol_def_destroy(&ctx->protocols[--ctx->protocol_count]);
    while (ctx->trait_count > checkpoint->trait_count) trait_def_destroy(&ctx->traits[--ctx->trait_count]);
    while (ctx->type_count > checkpoint->type_count) type_def_destroy(&ctx->types[--ctx->type_count]);
    while (ctx->method_surface_count > checkpoint->method_surface_count) method_surface_def_destroy(&ctx->method_surfaces[--ctx->method_surface_count]);
    while (ctx->decl_method_count > checkpoint->decl_method_count) idm_trait_method_def_destroy(&ctx->decl_methods[--ctx->decl_method_count]);
    if (!checkpoint->decl_grammar && ctx->decl_grammar) {
        phase_syntax_fn_destroy(&ctx->decl_grammar_impl);
        ctx->decl_grammar = false;
    }
    idm_binding_table_truncate(&ctx->bindings, checkpoint->binding_count);
}

void ctx_destroy(ExpandContext *ctx) {
    for (size_t i = 0; i < ctx->activation_count; i++) free(ctx->activations[i].name);
    free(ctx->activations);
    for (size_t i = 0; i < ctx->surface_install_count; i++) surface_install_destroy(&ctx->surface_installs[i]);
    free(ctx->surface_installs);
    idm_binding_table_destroy(&ctx->bindings);
    idm_scope_set_destroy(&ctx->empty_scopes);
    capture_bindings_destroy(ctx->captures, ctx->capture_count);
    for (size_t i = 0; i < ctx->export_count; i++) free(ctx->exports[i].name);
    free(ctx->exports);
    for (size_t i = 0; i < ctx->package_global_count; i++) idm_pkg_global_destroy(&ctx->package_globals[i]);
    free(ctx->package_globals);
    for (size_t i = 0; i < ctx->macro_count; i++) macro_def_destroy(&ctx->macros[i]);
    free(ctx->macros);
    for (size_t i = 0; i < ctx->grammar_count; i++) grammar_def_destroy(&ctx->grammars[i]);
    free(ctx->grammars);
    for (size_t i = 0; i < ctx->operator_count; i++) idm_operator_def_destroy(&ctx->operators[i]);
    free(ctx->operators);
    for (size_t i = 0; i < ctx->protocol_count; i++) protocol_def_destroy(&ctx->protocols[i]);
    free(ctx->protocols);
    for (size_t i = 0; i < ctx->trait_count; i++) trait_def_destroy(&ctx->traits[i]);
    free(ctx->traits);
    for (size_t i = 0; i < ctx->type_count; i++) type_def_destroy(&ctx->types[i]);
    free(ctx->types);
    for (size_t i = 0; i < ctx->method_surface_count; i++) method_surface_def_destroy(&ctx->method_surfaces[i]);
    free(ctx->method_surfaces);
    for (size_t i = 0; i < ctx->decl_method_count; i++) idm_trait_method_def_destroy(&ctx->decl_methods[i]);
    free(ctx->decl_methods);
    phase_syntax_fn_destroy(&ctx->decl_grammar_impl);
    idm_phase_env_release(ctx->phase_env);
    free(ctx->kernel_import_src);
    free(ctx->kernel_import_dst);
    if (ctx->kernel_import_names) {
        for (size_t i = 0; i < ctx->kernel_import_count; i++) free(ctx->kernel_import_names[i]);
    }
    free(ctx->kernel_import_names);
    free(ctx->artifact_bases);
    free(ctx->runtime_imports);
    for (size_t i = 0; i < ctx->dep_count; i++) free(ctx->deps[i].path);
    free(ctx->deps);
    free(ctx->pat_binders);
}

bool seed_home_primitives(ExpandContext *ctx, const char *home, const char *qualifier, IdmError *err) {
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        const IdmPrimitiveInfo *info = idm_primitive_info((IdmPrimitive)i);
        if (!info || strcmp(idm_primitive_home((IdmPrimitive)i), home) != 0) continue;
        IdmArity arity = idm_arity_range(info->min_arity, info->max_arity);
        const char *name = info->name;
        char *qualified = NULL;
        if (qualifier) {
            IdmBuffer qb;
            idm_buf_init(&qb);
            if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, info->name)) {
                idm_buf_destroy(&qb);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            qualified = idm_buf_take(&qb);
            name = qualified;
        }
        bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &ctx->empty_scopes, (uint32_t)i, IDM_FRAME_GLOBAL, arity, NULL);
        free(qualified);
        if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

IdmCore *expand_primitive_call(IdmPrimitive primitive, IdmSpan span, IdmError *err) {
    IdmCore *callee = idm_core_primitive(primitive, span);
    IdmCore *call = callee ? idm_core_call(callee, span) : NULL;
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, span);
    }
    return call;
}

bool core_call_add_arg_or_free(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span) {
    if (arg && idm_core_call_add_arg(call, arg)) return true;
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

bool register_operator(ExpandContext *ctx, const char *name, const char *capture, uint8_t precedence, IdmOpAssoc assoc, const char *target_name, const IdmScopeSet *scopes, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, bool exported, IdmError *err) {
    if (!capture || !*capture) return idm_error_set(err, idm_span_unknown(NULL), "operator capture must be non-empty");
    if (!target_name || !*target_name) return idm_error_set(err, idm_span_unknown(NULL), "operator target must be non-empty");
    if (!binding_scopes) binding_scopes = scopes ? scopes : &ctx->empty_scopes;
    char *key = operator_binding_key(name, capture);
    if (!key) return idm_error_oom(err, idm_span_unknown(NULL));
    if (ctx->operator_count == ctx->operator_cap) {
        size_t cap = ctx->operator_cap ? ctx->operator_cap * 2u : 16u;
        IdmOperatorDef *next = realloc(ctx->operators, cap * sizeof(*next));
        if (!next) {
            free(key);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        ctx->operators = next;
        ctx->operator_cap = cap;
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

bool install_method_surface(ExpandContext *ctx, const char *trait, const char *name, IdmArity arity, bool is_field, const IdmScopeSet *scopes, const char *provider, const char *provider_key, IdmError *err) {
    const IdmScopeSet *check_scopes = scopes ? scopes : &ctx->empty_scopes;
    for (size_t i = 0; i < ctx->method_surface_count; i++) {
        const MethodSurfaceDef *e = &ctx->method_surfaces[i];
        if (strcmp(e->name, name) != 0 || !idm_scope_set_equal(&e->scopes, check_scopes)) continue;
        if (e->is_field != is_field) {
            return idm_error_set(err, idm_span_unknown(NULL), "'%s' is declared as both a record field and a trait method in the same scope; dot access would be ambiguous — rename one", name);
        }
        if (is_field) return true;
        if (strcmp(e->trait, trait) == 0) {
            if (!idm_arity_equal(&e->arity, &arity)) {
                return idm_error_set(err, idm_span_unknown(NULL), "method surface '%s.%s' arity changed while activating this scope", trait, name);
            }
            return true;
        }
    }
    if (ctx->method_surface_count == ctx->method_surface_cap) {
        size_t cap = ctx->method_surface_cap ? ctx->method_surface_cap * 2u : 8u;
        MethodSurfaceDef *next = realloc(ctx->method_surfaces, cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        ctx->method_surfaces = next;
        ctx->method_surface_cap = cap;
    }
    MethodSurfaceDef *method = &ctx->method_surfaces[ctx->method_surface_count];
    memset(method, 0, sizeof(*method));
    method->trait = idm_strdup(trait);
    method->name = idm_strdup(name);
    method->provider = idm_strdup(provider ? provider : "");
    method->provider_key = idm_strdup(provider_key ? provider_key : "");
    method->arity = arity;
    method->is_field = is_field;
    if (!method->trait || !method->name || !method->provider || !method->provider_key || !idm_scope_set_copy(&method->scopes, scopes ? scopes : &ctx->empty_scopes)) {
        method_surface_def_destroy(method);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ctx->method_surface_count++;
    return true;
}

ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_PROTOCOL, IDM_BIND_PROTOCOL, out_status);
    if (!binding || binding->payload >= ctx->protocol_count) return NULL;
    return &ctx->protocols[binding->payload];
}

TraitDef *resolve_trait_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_TRAIT, IDM_BIND_TRAIT, out_status);
    if (!binding || binding->payload >= ctx->trait_count) return NULL;
    return &ctx->traits[binding->payload];
}

TypeDef *resolve_type_def(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmResolveStatus *out_status) {
    const IdmBinding *binding = resolve_surface_binding(ctx, name_syntax, IDM_BIND_SPACE_TYPE, IDM_BIND_TYPE, out_status);
    if (!binding || binding->payload >= ctx->type_count) return NULL;
    return &ctx->types[binding->payload];
}

static const IdmBinding *resolve_surface_binding(const ExpandContext *ctx, const IdmSyntax *word, IdmBindingSpace space, IdmBindingKind kind, IdmResolveStatus *out_status) {
    if (!word || word->kind != IDM_SYN_WORD) {
        if (out_status) *out_status = IDM_RESOLVE_UNBOUND;
        return NULL;
    }
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, word->as.text, ctx->phase, space, scopes ? scopes : &empty, &binding);
    idm_scope_set_destroy(&empty);
    if (out_status) *out_status = status;
    return status == IDM_RESOLVE_OK && binding && binding->kind == kind ? binding : NULL;
}

bool ctx_seed(ExpandContext *ctx, IdmError *err) {
    return seed_home_primitives(ctx, "kernel", NULL, err);
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
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = scopes ? scopes : &empty;
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, word->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, lookup, &binding);
    idm_scope_set_destroy(&empty);
    if (out_status) *out_status = status;
    return status == IDM_RESOLVE_OK ? binding : NULL;
}

bool binder_scopes_pruned(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmScopeSet *out) {
    if (!syntax_scopes_copy(out, name_syntax)) return false;
    if (ctx->def_ctx) {
        for (size_t i = 0; i < ctx->def_ctx->use_site.count; i++) idm_scope_set_remove(out, ctx->def_ctx->use_site.items[i]);
    }
    if (ctx->rt->macro_intro_active) idm_scope_set_remove(out, ctx->rt->macro_intro_scope);
    return true;
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

bool global_push_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, uint32_t *out_id) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t id = ctx->global_seq++;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_GLOBAL, &scopes, id, IDM_FRAME_GLOBAL, arity, NULL);
    idm_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_id) *out_id = id;
    return true;
}

bool global_push_def_binder(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t *out_id) {
    return global_push_def_binder_with_arity(ctx, name, name_syntax, idm_arity_unknown(), out_id);
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
    size_t next = *cap ? *cap * 2u : 4u;
    CaptureBinding *grown = realloc(*arr, next * sizeof(*grown));
    if (!grown) return false;
    *arr = grown;
    *cap = next;
    return true;
}

static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IdmSyntax *word, const IdmScopeSet *scopes, IdmCaptureKind kind, uint32_t source_index, IdmArity arity) {
    if (!capture_array_grow(arr, count, cap)) return -1;
    CaptureBinding *c = &(*arr)[*count];
    c->name = idm_strdup(word->as.text);
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

const char *package_path_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD) return syn->as.text;
    if (syn_is_form(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

static const IdmOperatorDef *op_lookup_capture(const ExpandContext *ctx, const IdmSyntax *syn, const char *capture) {
    if (!syn || syn->kind != IDM_SYN_WORD) return NULL;
    char *key = operator_binding_key(syn->as.text, capture);
    if (!key) return NULL;
    const IdmScopeSet *scopes = idm_syn_scope_set(syn, 0);
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = scopes ? scopes : &empty;
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, key, ctx->phase, IDM_BIND_SPACE_OPERATOR, lookup, &binding);
    if (status == IDM_RESOLVE_UNBOUND && ctx->op_fallback) {
        IdmScopeSet merged;
        if (idm_scope_set_copy(&merged, lookup)) {
            bool ok = true;
            for (size_t i = 0; ok && i < ctx->op_fallback->count; i++) ok = idm_scope_set_add(&merged, ctx->op_fallback->items[i]);
            if (ok) status = idm_binding_resolve(&ctx->bindings, key, ctx->phase, IDM_BIND_SPACE_OPERATOR, &merged, &binding);
            idm_scope_set_destroy(&merged);
        }
    }
    idm_scope_set_destroy(&empty);
    free(key);
    if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_OPERATOR) return NULL;
    if (binding->payload >= ctx->operator_count) return NULL;
    const IdmOperatorDef *op = &ctx->operators[binding->payload];
    return op->capture && strcmp(op->capture, capture) == 0 ? op : NULL;
}

bool operator_capture_is_expression(const char *capture) {
    return capture && (strcmp(capture, "prefix") == 0 || strcmp(capture, "infix") == 0 || strcmp(capture, "postfix") == 0);
}

static bool operator_capture_has_left_operand(const char *capture) {
    return capture && strncmp(capture, "infix", 5u) == 0 && (capture[5] == ' ' || capture[5] == ':') && capture[6] != '\0';
}

const IdmOperatorDef *op_lookup(const ExpandContext *ctx, const IdmSyntax *syn, bool want_prefix) {
    if (want_prefix) return op_lookup_capture(ctx, syn, "prefix");
    const IdmOperatorDef *infix = op_lookup_capture(ctx, syn, "infix");
    return infix ? infix : op_lookup_capture(ctx, syn, "postfix");
}

const IdmOperatorDef *op_lookup_syntax_capture(const ExpandContext *ctx, const IdmSyntax *syn, bool want_left, IdmError *err) {
    if (!syn || syn->kind != IDM_SYN_WORD) return NULL;
    const IdmOperatorDef *found = NULL;
    for (size_t i = 0; i < ctx->operator_count; i++) {
        const IdmOperatorDef *op = &ctx->operators[i];
        if (!op->name || strcmp(op->name, syn->as.text) != 0) continue;
        if (operator_capture_is_expression(op->capture)) continue;
        if (operator_capture_has_left_operand(op->capture) != want_left) continue;
        const IdmOperatorDef *visible = op_lookup_capture(ctx, syn, op->capture);
        if (visible != op) continue;
        if (found && found != op) {
            idm_error_set(err, syn->span, "ambiguous operator capture '%s'", syn->as.text);
            return NULL;
        }
        found = op;
    }
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

bool expander_surface_callback(void *user, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err) {
    ExpandContext *ctx = user;
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
    } else if (strcmp(kind, "grammars") == 0) {
        for (size_t i = ctx->grammar_count; i > 0; i--) {
            acc = idm_cons(rt, idm_atom(rt, "grammar"), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "protocols") == 0) {
        for (size_t i = ctx->protocol_count; i > 0; i--) {
            const ProtocolDef *p = &ctx->protocols[i - 1u];
            acc = idm_cons(rt, idm_atom(rt, p->public_name ? p->public_name : p->name), acc, err);
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
        for (size_t i = ctx->method_surface_count; i > 0; i--) {
            const MethodSurfaceDef *m = &ctx->method_surfaces[i - 1u];
            IdmValue items[3];
            items[0] = idm_atom(rt, m->trait);
            items[1] = idm_atom(rt, m->name);
            items[2] = arity_surface_value(rt, m->arity, err);
            if (err && err->present) return false;
            IdmValue entry = idm_tuple(rt, items, 3u, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else {
        return idm_error_set(err, idm_span_unknown(NULL), "expander-surface kind must be :operators, :macros, :protocols, :grammars, :methods, or :active");
    }
    *out = acc;
    return true;
}
