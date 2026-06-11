#include "internal.h"

static void capture_bindings_destroy(CaptureBinding *captures, size_t count);
static void method_surface_def_destroy(MethodSurfaceDef *method);
static bool seed_primitive(ExpandContext *ctx, const char *name, IshPrimitive primitive);
static const char *operator_fixity_name(IshOpFixity fixity);
static char *operator_binding_key(const char *name, IshOpFixity fixity);
static bool binder_scopes_pruned(ExpandContext *ctx, const IshSyntax *name_syntax, IshScopeSet *out);
static bool capture_array_grow(CaptureBinding **arr, size_t *count, size_t *cap);
static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IshSyntax *word, const IshScopeSet *scopes, IshCaptureKind kind, uint32_t source_index);
static int saved_materialize(SavedFunctionContext *g, const IshSyntax *word, const IshBinding *b);
static const IshOperatorDef *op_lookup_fixity(const ExpandContext *ctx, const IshSyntax *syn, IshOpFixity fixity);

void ctx_init(ExpandContext *ctx, IshRuntime *rt) {
    ctx->rt = rt;
    ish_binding_table_init(&ctx->bindings);
    ish_scope_set_init(&ctx->empty_scopes);
    ctx->next_slot = 0;
    ctx->arg_slots = 0;
    ctx->captures = NULL;
    ctx->capture_count = 0;
    ctx->capture_cap = 0;
    ctx->frame = 1u;
    ctx->frame_seq = 1u;
    ctx->global_seq = 0u;
    ctx->in_package = false;
    ctx->package_name = NULL;
    ctx->exports = NULL;
    ctx->export_count = 0;
    ctx->export_cap = 0;
    ctx->package_globals = NULL;
    ctx->package_global_count = 0;
    ctx->package_global_cap = 0;
    ctx->macros = NULL;
    ctx->macro_count = 0;
    ctx->macro_cap = 0;
    ctx->operators = NULL;
    ctx->operator_count = 0;
    ctx->operator_cap = 0;
    ctx->protocols = NULL;
    ctx->protocol_count = 0;
    ctx->protocol_cap = 0;
    ctx->method_surfaces = NULL;
    ctx->method_surface_count = 0;
    ctx->method_surface_cap = 0;
    ctx->decl_methods = NULL;
    ctx->decl_method_count = 0;
    ctx->decl_method_cap = 0;
    ctx->runner = NULL;
    ctx->local_runner.user = ctx;
    ctx->local_runner.invoke = local_macro_invoke;
    ish_scope_store_init(&ctx->scope_store);
    ctx->macro_depth = 0;
    ctx->value_context = false;
    ctx->command_sub_context = false;
    ctx->def_ctx = NULL;
    ctx->activations = NULL;
    ctx->activation_count = 0;
    ctx->activation_cap = 0;
    ctx->decl_resolver = false;
    ctx->decl_resolver_module = NULL;
    ctx->decl_resolver_fn = 0;
    ctx->decl_resolver_phase_ns = NULL;
    ctx->decl_resolver_phase_env = NULL;
    ctx->enclosing = NULL;
    ctx->phase_ns = rt->main_ns;
    ctx->phase_env = NULL;
    ctx->protocol_name = NULL;
    ctx->kernel_import_src = NULL;
    ctx->kernel_import_dst = NULL;
    ctx->kernel_import_count = 0;
    ctx->kernel_wrap = false;
    ctx->artifact_bases = NULL;
    ctx->artifact_base_count = 0;
    ctx->artifact_base_cap = 0;
    ctx->deps = NULL;
    ctx->dep_count = 0;
    ctx->dep_cap = 0;
}

static void capture_bindings_destroy(CaptureBinding *captures, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(captures[i].name);
        ish_scope_set_destroy(&captures[i].scopes);
    }
    free(captures);
}

void macro_def_destroy(MacroDef *macro) {
    if (!macro) return;
    if (macro->closure_backed) {
        ish_runtime_unregister_gc_value(macro->rt, macro->transformer);
        macro->closure_backed = false;
    }
    ish_module_ref_release(macro->module);
    macro->module = NULL;
    free(macro->name);
    macro->name = NULL;
    macro->function_index = 0;
    macro->phase_ns = NULL;
    ish_phase_env_release(macro->phase_env);
    macro->phase_env = NULL;
    macro->exported = false;
}

static void method_surface_def_destroy(MethodSurfaceDef *method) {
    if (!method) return;
    free(method->protocol);
    free(method->name);
    ish_scope_set_destroy(&method->scopes);
    memset(method, 0, sizeof(*method));
}

void protocol_def_destroy(ProtocolDef *protocol) {
    if (!protocol) return;
    free(protocol->name);
    for (size_t j = 0; j < protocol->operator_count; j++) ish_operator_def_destroy(&protocol->operators[j]);
    free(protocol->operators);
    for (size_t j = 0; j < protocol->macro_count; j++) ish_pkg_macro_destroy(&protocol->macros[j]);
    free(protocol->macros);
    ish_module_ref_release(protocol->resolver_module);
    protocol->resolver_phase_ns = NULL;
    ish_phase_env_release(protocol->resolver_phase_env);
    protocol->resolver_phase_env = NULL;
    for (size_t j = 0; j < protocol->method_count; j++) ish_protocol_method_def_destroy(&protocol->methods[j]);
    free(protocol->methods);
    memset(protocol, 0, sizeof(*protocol));
}

bool record_activation(ExpandContext *ctx, const char *name, IshSpan span, IshError *err) {
    if (ctx->activation_count == ctx->activation_cap) {
        size_t cap = ctx->activation_cap ? ctx->activation_cap * 2u : 4u;
        void *grown = realloc(ctx->activations, cap * sizeof(*ctx->activations));
        if (!grown) return ish_error_oom(err, span);
        ctx->activations = grown;
        ctx->activation_cap = cap;
    }
    ctx->activations[ctx->activation_count].name = ish_strdup(name);
    if (!ctx->activations[ctx->activation_count].name) return ish_error_oom(err, span);
    ctx->activations[ctx->activation_count].span = span;
    ctx->activation_count++;
    return true;
}

void surface_checkpoint(ExpandContext *ctx, SurfaceCheckpoint *checkpoint) {
    checkpoint->binding_count = ctx->bindings.count;
    checkpoint->macro_count = ctx->macro_count;
    checkpoint->operator_count = ctx->operator_count;
    checkpoint->protocol_count = ctx->protocol_count;
    checkpoint->method_surface_count = ctx->method_surface_count;
    checkpoint->decl_method_count = ctx->decl_method_count;
    checkpoint->decl_resolver = ctx->decl_resolver;
    checkpoint->activation_count = ctx->activation_count;
}

void surface_rollback(ExpandContext *ctx, const SurfaceCheckpoint *checkpoint) {
    while (ctx->activation_count > checkpoint->activation_count) free(ctx->activations[--ctx->activation_count].name);
    while (ctx->macro_count > checkpoint->macro_count) macro_def_destroy(&ctx->macros[--ctx->macro_count]);
    while (ctx->operator_count > checkpoint->operator_count) ish_operator_def_destroy(&ctx->operators[--ctx->operator_count]);
    while (ctx->protocol_count > checkpoint->protocol_count) protocol_def_destroy(&ctx->protocols[--ctx->protocol_count]);
    while (ctx->method_surface_count > checkpoint->method_surface_count) method_surface_def_destroy(&ctx->method_surfaces[--ctx->method_surface_count]);
    while (ctx->decl_method_count > checkpoint->decl_method_count) ish_protocol_method_def_destroy(&ctx->decl_methods[--ctx->decl_method_count]);
    if (!checkpoint->decl_resolver && ctx->decl_resolver) {
        ish_module_ref_release(ctx->decl_resolver_module);
        ctx->decl_resolver_module = NULL;
        ctx->decl_resolver = false;
        ctx->decl_resolver_fn = 0;
        ctx->decl_resolver_phase_ns = NULL;
        ish_phase_env_release(ctx->decl_resolver_phase_env);
        ctx->decl_resolver_phase_env = NULL;
    }
    ish_binding_table_truncate(&ctx->bindings, checkpoint->binding_count);
}

void ctx_destroy_activations(ExpandContext *ctx) {
    for (size_t i = 0; i < ctx->activation_count; i++) free(ctx->activations[i].name);
    free(ctx->activations);
    ctx->activations = NULL;
    ctx->activation_count = 0;
    ctx->activation_cap = 0;
}

void ctx_destroy(ExpandContext *ctx) {
    ctx_destroy_activations(ctx);
    ish_binding_table_destroy(&ctx->bindings);
    ish_scope_set_destroy(&ctx->empty_scopes);
    capture_bindings_destroy(ctx->captures, ctx->capture_count);
    for (size_t i = 0; i < ctx->export_count; i++) free(ctx->exports[i].name);
    free(ctx->exports);
    for (size_t i = 0; i < ctx->package_global_count; i++) ish_pkg_global_destroy(&ctx->package_globals[i]);
    free(ctx->package_globals);
    for (size_t i = 0; i < ctx->macro_count; i++) macro_def_destroy(&ctx->macros[i]);
    free(ctx->macros);
    for (size_t i = 0; i < ctx->operator_count; i++) ish_operator_def_destroy(&ctx->operators[i]);
    free(ctx->operators);
    for (size_t i = 0; i < ctx->protocol_count; i++) protocol_def_destroy(&ctx->protocols[i]);
    free(ctx->protocols);
    for (size_t i = 0; i < ctx->method_surface_count; i++) method_surface_def_destroy(&ctx->method_surfaces[i]);
    free(ctx->method_surfaces);
    for (size_t i = 0; i < ctx->decl_method_count; i++) ish_protocol_method_def_destroy(&ctx->decl_methods[i]);
    free(ctx->decl_methods);
    ish_module_ref_release(ctx->decl_resolver_module);
    ish_phase_env_release(ctx->decl_resolver_phase_env);
    ish_phase_env_release(ctx->phase_env);
    free(ctx->kernel_import_src);
    free(ctx->kernel_import_dst);
    free(ctx->artifact_bases);
    for (size_t i = 0; i < ctx->dep_count; i++) free(ctx->deps[i].path);
    free(ctx->deps);
}

static bool seed_primitive(ExpandContext *ctx, const char *name, IshPrimitive primitive) {
    return ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &ctx->empty_scopes, (uint32_t)primitive, ISH_FRAME_GLOBAL, NULL);
}

static const char *operator_fixity_name(IshOpFixity fixity) {
    switch (fixity) {
        case ISH_OP_FIX_INFIX: return "infix";
        case ISH_OP_FIX_PREFIX: return "prefix";
        case ISH_OP_FIX_POSTFIX: return "postfix";
    }
    return "operator";
}

static char *operator_binding_key(const char *name, IshOpFixity fixity) {
    IshBuffer key;
    ish_buf_init(&key);
    if (!ish_buf_append(&key, operator_fixity_name(fixity)) || !ish_buf_append_char(&key, ':') || !ish_buf_append(&key, name)) {
        ish_buf_destroy(&key);
        return NULL;
    }
    return ish_buf_take(&key);
}

bool register_operator(ExpandContext *ctx, const char *name, uint8_t precedence, IshOpAssoc assoc, IshOpFixity fixity, IshOpTargetKind target_kind, IshPrimitive primitive, const char *target_name, const IshScopeSet *scopes, IshError *err) {
    if (ctx->operator_count == ctx->operator_cap) {
        size_t cap = ctx->operator_cap ? ctx->operator_cap * 2u : 16u;
        IshOperatorDef *next = realloc(ctx->operators, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        ctx->operators = next;
        ctx->operator_cap = cap;
    }
    IshOperatorDef *op = &ctx->operators[ctx->operator_count];
    memset(op, 0, sizeof(*op));
    op->name = ish_strdup(name);
    if (!op->name) return ish_error_oom(err, ish_span_unknown(NULL));
    op->precedence = precedence;
    op->assoc = assoc;
    op->fixity = fixity;
    op->target_kind = target_kind;
    op->primitive = primitive;
    op->target_name = NULL;
    if (target_name) {
        op->target_name = ish_strdup(target_name);
        if (!op->target_name) { free(op->name); return ish_error_oom(err, ish_span_unknown(NULL)); }
    }
    if (!ish_scope_set_copy(&op->scopes, scopes ? scopes : &ctx->empty_scopes)) {
        free(op->name);
        free(op->target_name);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    char *key = operator_binding_key(name, fixity);
    if (!key) {
        ish_operator_def_destroy(op);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    uint32_t payload = (uint32_t)ctx->operator_count;
    if (!ish_binding_table_add(&ctx->bindings, key, 0, ISH_BIND_SPACE_OPERATOR, ISH_BIND_OPERATOR, &op->scopes, payload, ctx->frame, NULL)) {
        free(key);
        ish_operator_def_destroy(op);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    free(key);
    op->exported = true;
    ctx->operator_count++;
    return true;
}

bool install_method_surface(ExpandContext *ctx, const char *protocol, const char *name, uint32_t arity, const IshScopeSet *scopes, IshError *err) {
    if (ctx->method_surface_count == ctx->method_surface_cap) {
        size_t cap = ctx->method_surface_cap ? ctx->method_surface_cap * 2u : 8u;
        MethodSurfaceDef *next = realloc(ctx->method_surfaces, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        ctx->method_surfaces = next;
        ctx->method_surface_cap = cap;
    }
    MethodSurfaceDef *method = &ctx->method_surfaces[ctx->method_surface_count];
    memset(method, 0, sizeof(*method));
    method->protocol = ish_strdup(protocol);
    method->name = ish_strdup(name);
    method->arity = arity;
    if (!method->protocol || !method->name || !ish_scope_set_copy(&method->scopes, scopes ? scopes : &ctx->empty_scopes)) {
        method_surface_def_destroy(method);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    uint32_t payload = (uint32_t)ctx->method_surface_count;
    if (!ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_METHOD, &method->scopes, payload, ctx->frame, NULL)) {
        method_surface_def_destroy(method);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    ctx->method_surface_count++;
    return true;
}

const MethodSurfaceDef *resolve_method_surface(ExpandContext *ctx, const IshSyntax *word, IshResolveStatus *out_status) {
    if (!word || word->kind != ISH_SYN_WORD) {
        if (out_status) *out_status = ISH_RESOLVE_UNBOUND;
        return NULL;
    }
    const IshScopeSet *scopes = ish_syn_scope_set(word, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, word->as.text, 0, ISH_BIND_SPACE_DEFAULT, lookup, &binding);
    ish_scope_set_destroy(&empty);
    if (out_status) *out_status = status;
    if (status != ISH_RESOLVE_OK || !binding || binding->kind != ISH_BIND_METHOD || binding->payload >= ctx->method_surface_count) return NULL;
    return &ctx->method_surfaces[binding->payload];
}

ProtocolDef *resolve_protocol_def(ExpandContext *ctx, const IshSyntax *name_syntax, IshResolveStatus *out_status) {
    if (!name_syntax || name_syntax->kind != ISH_SYN_WORD) {
        if (out_status) *out_status = ISH_RESOLVE_UNBOUND;
        return NULL;
    }
    const IshScopeSet *scopes = ish_syn_scope_set(name_syntax, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, name_syntax->as.text, 0, ISH_BIND_SPACE_PROTOCOL, lookup, &binding);
    ish_scope_set_destroy(&empty);
    if (out_status) *out_status = status;
    if (status != ISH_RESOLVE_OK || !binding || binding->kind != ISH_BIND_PROTOCOL || binding->payload >= ctx->protocol_count) return NULL;
    return &ctx->protocols[binding->payload];
}

bool ctx_seed(ExpandContext *ctx, IshError *err) {
    for (size_t i = 0; i < ish_primitive_count(); i++) {
        const IshPrimitiveInfo *info = ish_primitive_info((IshPrimitive)i);
        if (info && !seed_primitive(ctx, info->name, (IshPrimitive)i)) return ish_error_oom(err, ish_span_unknown(NULL));
    }
    return true;
}

bool syntax_scopes_copy(IshScopeSet *dst, const IshSyntax *syn) {
    const IshScopeSet *src = ish_syn_scope_set(syn, 0);
    if (src) return ish_scope_set_copy(dst, src);
    ish_scope_set_init(dst);
    return true;
}

bool scopes_subset_for_ref(const IshScopeSet *binding_scopes, const IshSyntax *ref) {
    const IshScopeSet *ref_scopes = ish_syn_scope_set(ref, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup = ref_scopes ? ref_scopes : &empty;
    bool ok = ish_scope_set_subset(binding_scopes, lookup);
    ish_scope_set_destroy(&empty);
    return ok;
}

const IshBinding *resolve_default(const ExpandContext *ctx, const IshSyntax *word, IshResolveStatus *out_status) {
    const IshScopeSet *scopes = ish_syn_scope_set(word, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, word->as.text, 0, ISH_BIND_SPACE_DEFAULT, lookup, &binding);
    ish_scope_set_destroy(&empty);
    if (out_status) *out_status = status;
    return status == ISH_RESOLVE_OK ? binding : NULL;
}

static bool binder_scopes_pruned(ExpandContext *ctx, const IshSyntax *name_syntax, IshScopeSet *out) {
    if (!syntax_scopes_copy(out, name_syntax)) return false;
    if (ctx->def_ctx) {
        for (size_t i = 0; i < ctx->def_ctx->use_site.count; i++) ish_scope_set_remove(out, ctx->def_ctx->use_site.items[i]);
    }
    return true;
}

bool local_push_def_binder(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_slot) {
    IshScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t slot = ctx->next_slot++;
    bool ok = ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_LOCAL, &scopes, slot, ctx->frame, NULL);
    ish_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

bool global_push_def_binder(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_id) {
    IshScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    uint32_t id = ctx->global_seq++;
    bool ok = ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &scopes, id, ISH_FRAME_GLOBAL, NULL);
    ish_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_id) *out_id = id;
    return true;
}

bool local_push_scoped(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_slot) {
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, name_syntax)) return false;
    uint32_t slot = ctx->next_slot++;
    bool ok = ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_LOCAL, &scopes, slot, ctx->frame, NULL);
    ish_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

void local_pop_to(ExpandContext *ctx, size_t table_base, uint32_t next_slot) {
    ish_binding_table_truncate(&ctx->bindings, table_base);
    ctx->next_slot = next_slot;
}

bool global_push(ExpandContext *ctx, const char *name, const IshSyntax *name_syntax, uint32_t *out_id) {
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, name_syntax)) return false;
    uint32_t id = ctx->global_seq++;
    bool ok = ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_GLOBAL, &scopes, id, ISH_FRAME_GLOBAL, NULL);
    ish_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_id) *out_id = id;
    return true;
}

bool arg_push(ExpandContext *ctx, const IshSyntax *word, uint32_t *out_slot) {
    const IshBinding *existing = resolve_default(ctx, word, NULL);
    if (existing && existing->kind == ISH_BIND_ARG && existing->frame_id == ctx->frame) return false;
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, word)) return false;
    uint32_t slot = ctx->arg_slots++;
    bool ok = ish_binding_table_add(&ctx->bindings, word->as.text, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_ARG, &scopes, slot, ctx->frame, NULL);
    ish_scope_set_destroy(&scopes);
    if (!ok) return false;
    if (out_slot) *out_slot = slot;
    return true;
}

bool arg_push_slot(ExpandContext *ctx, const IshSyntax *word, uint32_t slot) {
    const IshBinding *existing = resolve_default(ctx, word, NULL);
    if (existing && existing->kind == ISH_BIND_ARG && existing->frame_id == ctx->frame) return true;
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, word)) return false;
    bool ok = ish_binding_table_add(&ctx->bindings, word->as.text, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_ARG, &scopes, slot, ctx->frame, NULL);
    ish_scope_set_destroy(&scopes);
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

static int capture_append(CaptureBinding **arr, size_t *count, size_t *cap, const IshSyntax *word, const IshScopeSet *scopes, IshCaptureKind kind, uint32_t source_index) {
    if (!capture_array_grow(arr, count, cap)) return -1;
    CaptureBinding *c = &(*arr)[*count];
    c->name = ish_strdup(word->as.text);
    if (!c->name) return -1;
    if (!ish_scope_set_copy(&c->scopes, scopes)) {
        free(c->name);
        return -1;
    }
    c->kind = kind;
    c->source_index = source_index;
    c->capture_index = (uint32_t)*count;
    (*count)++;
    return (int)c->capture_index;
}

bool capture_lookup_existing(const CaptureBinding *captures, size_t count, const IshSyntax *word, uint32_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(captures[i].name, word->as.text) == 0 && scopes_subset_for_ref(&captures[i].scopes, word)) {
            *out = captures[i].capture_index;
            return true;
        }
    }
    return false;
}

static int saved_materialize(SavedFunctionContext *g, const IshSyntax *word, const IshBinding *b) {
    uint32_t existing = 0;
    if (capture_lookup_existing(g->captures, g->capture_count, word, &existing)) return (int)existing;
    if (!g->prev) return -1;
    if (g->prev->frame == b->frame_id) {
        IshCaptureKind kind = b->kind == ISH_BIND_ARG ? ISH_CAP_ARG : ISH_CAP_LOCAL;
        return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, kind, b->payload);
    }
    int parent = saved_materialize(g->prev, word, b);
    if (parent < 0) return -1;
    return capture_append(&g->captures, &g->capture_count, &g->capture_cap, word, &b->scopes, ISH_CAP_UPVALUE, (uint32_t)parent);
}

bool materialize_capture(ExpandContext *ctx, const IshSyntax *word, const IshBinding *b, uint32_t *out) {
    if (!ctx->enclosing) return false;
    int idx;
    if (ctx->enclosing->frame == b->frame_id) {
        IshCaptureKind kind = b->kind == ISH_BIND_ARG ? ISH_CAP_ARG : ISH_CAP_LOCAL;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, kind, b->payload);
    } else {
        int parent = saved_materialize(ctx->enclosing, word, b);
        if (parent < 0) return false;
        idx = capture_append(&ctx->captures, &ctx->capture_count, &ctx->capture_cap, word, &b->scopes, ISH_CAP_UPVALUE, (uint32_t)parent);
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
    ish_binding_table_truncate(&ctx->bindings, saved->table_base);
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
    ish_binding_table_truncate(&ctx->bindings, saved->table_base);
    ctx->next_slot = saved->next_slot;
    ctx->arg_slots = saved->arg_slots;
}

bool syn_is_word(const IshSyntax *syn, const char *word) {
    return syn && syn->kind == ISH_SYN_WORD && strcmp(syn->as.text, word) == 0;
}

bool syn_is_protocol(const IshSyntax *syn, const char *head) {
    return syn && syn->kind == ISH_SYN_LIST && syn->as.seq.shape == ISH_SEQ_PAREN && syn->as.seq.count > 0 && syn_is_word(syn->as.seq.items[0], head);
}

const char *package_path_text(const IshSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == ISH_SYN_WORD) return syn->as.text;
    if (syn_is_protocol(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == ISH_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

static const IshOperatorDef *op_lookup_fixity(const ExpandContext *ctx, const IshSyntax *syn, IshOpFixity fixity) {
    if (!syn || syn->kind != ISH_SYN_WORD) return NULL;
    char *key = operator_binding_key(syn->as.text, fixity);
    if (!key) return NULL;
    const IshScopeSet *scopes = ish_syn_scope_set(syn, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, key, 0, ISH_BIND_SPACE_OPERATOR, lookup, &binding);
    ish_scope_set_destroy(&empty);
    free(key);
    if (status != ISH_RESOLVE_OK || !binding || binding->kind != ISH_BIND_OPERATOR) return NULL;
    if (binding->payload >= ctx->operator_count) return NULL;
    const IshOperatorDef *op = &ctx->operators[binding->payload];
    return op->fixity == fixity ? op : NULL;
}

const IshOperatorDef *op_lookup(const ExpandContext *ctx, const IshSyntax *syn, bool want_prefix) {
    if (want_prefix) return op_lookup_fixity(ctx, syn, ISH_OP_FIX_PREFIX);
    const IshOperatorDef *infix = op_lookup_fixity(ctx, syn, ISH_OP_FIX_INFIX);
    return infix ? infix : op_lookup_fixity(ctx, syn, ISH_OP_FIX_POSTFIX);
}

IshCore *expand_error(IshError *err, IshSpan span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ish_error_setv(err, span, fmt, ap);
    va_end(ap);
    return NULL;
}

static const char *assoc_atom_name(IshOpAssoc assoc) {
    if (assoc == ISH_OP_ASSOC_LEFT) return "left";
    if (assoc == ISH_OP_ASSOC_RIGHT) return "right";
    return "none";
}

bool expander_surface_callback(void *user, IshRuntime *rt, const char *kind, IshValue *out, IshError *err) {
    ExpandContext *ctx = user;
    IshValue acc = ish_nil();
    if (strcmp(kind, "operators") == 0) {
        for (size_t i = ctx->operator_count; i > 0; i--) {
            const IshOperatorDef *op = &ctx->operators[i - 1u];
            IshValue items[4];
            items[0] = ish_atom(rt, op->name);
            items[1] = ish_int((int64_t)op->precedence);
            items[2] = ish_atom(rt, assoc_atom_name(op->assoc));
            items[3] = ish_atom(rt, operator_fixity_name(op->fixity));
            IshValue entry = ish_tuple(rt, items, 4u, err);
            if (err && err->present) return false;
            acc = ish_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "macros") == 0 || strcmp(kind, "resolvers") == 0) {
        bool want_resolvers = kind[0] == 'r';
        for (size_t i = ctx->macro_count; i > 0; i--) {
            const char *name = ctx->macros[i - 1u].name;
            bool is_resolver = strcmp(name, "%resolver") == 0;
            if (is_resolver != want_resolvers) continue;
            acc = ish_cons(rt, ish_atom(rt, name), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "protocols") == 0) {
        for (size_t i = ctx->protocol_count; i > 0; i--) {
            acc = ish_cons(rt, ish_atom(rt, ctx->protocols[i - 1u].name), acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "active") == 0) {
        for (size_t i = ctx->activation_count; i > 0; i--) {
            IshValue items[4];
            items[0] = ish_atom(rt, ctx->activations[i - 1u].name);
            items[1] = ish_string(rt, ctx->activations[i - 1u].span.file ? ctx->activations[i - 1u].span.file : "<unknown>", err);
            if (err && err->present) return false;
            items[2] = ish_int((int64_t)ctx->activations[i - 1u].span.line);
            items[3] = ish_int((int64_t)ctx->activations[i - 1u].span.column);
            IshValue entry = ish_tuple(rt, items, 4u, err);
            if (err && err->present) return false;
            acc = ish_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else if (strcmp(kind, "methods") == 0) {
        for (size_t i = ctx->method_surface_count; i > 0; i--) {
            const MethodSurfaceDef *m = &ctx->method_surfaces[i - 1u];
            IshValue items[3];
            items[0] = ish_atom(rt, m->protocol);
            items[1] = ish_atom(rt, m->name);
            items[2] = ish_int((int64_t)m->arity);
            IshValue entry = ish_tuple(rt, items, 3u, err);
            if (err && err->present) return false;
            acc = ish_cons(rt, entry, acc, err);
            if (err && err->present) return false;
        }
    } else {
        return ish_error_set(err, ish_span_unknown(NULL), "expander-surface kind must be :operators, :macros, :protocols, :resolvers, :methods, or :active");
    }
    *out = acc;
    return true;
}
