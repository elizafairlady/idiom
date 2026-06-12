#include "internal.h"

static bool expand_macro_syntax_only(ExpandContext *ctx, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
static const char *operator_decl_token_text(const IdmSyntax *syn);

bool run_phase_core(ExpandContext *ctx, IdmCore *core, IdmError *err) {
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_bc_init(module);
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, module, &main_fn, err)) {
        idm_bc_destroy(module);
        free(module);
        return false;
    }
    IdmNamespace *old_main_ns = ctx->rt->main_ns;
    int old_protocol_phase = ctx->rt->protocol_phase;
    if (ctx->phase_ns) ctx->rt->main_ns = ctx->phase_ns;
    ctx->rt->protocol_phase = 1;
    IdmValue ignored = idm_nil();
    bool ok = idm_vm_run(ctx->rt, module, main_fn, &ignored, err);
    ctx->rt->main_ns = old_main_ns;
    ctx->rt->protocol_phase = old_protocol_phase;
    if (ok) {
        if (!idm_phase_env_add_module(ctx->phase_env, module, main_fn)) {
            idm_bc_destroy(module);
            free(module);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    } else {
        idm_bc_destroy(module);
        free(module);
    }
    return ok;
}

bool resolve_transformer(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err) {
    if (!head || head->kind != IDM_SYN_WORD) return false;
    const IdmScopeSet *scopes = idm_syn_scope_set(head, 0);
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup_scopes = scopes ? scopes : &empty;
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = idm_binding_resolve(&ctx->bindings, head->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, lookup_scopes, &binding);
    idm_scope_set_destroy(&empty);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, head->span, "ambiguous transformer '%s'", head->as.text);
        return false;
    }
    if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_TRANSFORMER) return false;
    *out_payload = binding->payload;
    return true;
}

bool resolve_head_resolver(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err) {
    if (!head || head->kind != IDM_SYN_WORD) return false;
    IdmSyntax *resolver = idm_syn_word("%resolver", head->span);
    if (!resolver) {
        idm_error_oom(err, head->span);
        return false;
    }
    const IdmScopeSet *scopes = idm_syn_scope_set(head, 0);
    if (scopes) {
        for (size_t i = 0; i < scopes->count; i++) {
            if (!idm_syn_scope_add(resolver, 0, scopes->items[i])) {
                idm_syn_free(resolver);
                idm_error_oom(err, head->span);
                return false;
            }
        }
    }
    bool ok = resolve_transformer(ctx, resolver, out_payload, err);
    idm_syn_free(resolver);
    return ok;
}

bool local_macro_invoke(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err) {
    ExpandContext *ctx = user;
    if (payload >= ctx->macro_count) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "macro transformer payload %u is out of bounds", payload);
    IdmValue arg = idm_syntax_value(rt, use_syntax, err);
    if (err && err->present) return false;
    IdmValue result = idm_nil();
    IdmNamespace *old_main_ns = rt->main_ns;
    int old_protocol_phase = rt->protocol_phase;
    IdmNamespace *macro_ns = ctx->macros[payload].phase_env ? ctx->macros[payload].phase_env->ns : ctx->macros[payload].phase_ns;
    if (macro_ns) rt->main_ns = macro_ns;
    rt->protocol_phase = 1;
    bool ok = ctx->macros[payload].closure_backed
        ? idm_vm_call_closure(rt, ctx->macros[payload].transformer, &arg, 1, &result, err)
        : idm_vm_call_function(rt, &ctx->macros[payload].module->module, ctx->macros[payload].function_index, &arg, 1, &result, err);
    rt->main_ns = old_main_ns;
    rt->protocol_phase = old_protocol_phase;
    if (!ok) return false;
    const IdmSyntax *result_syntax = idm_syntax_get(result, err);
    if (!result_syntax) return false;
    *out_syntax = idm_syn_clone(result_syntax);
    if (!*out_syntax) return idm_error_oom(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL));
    return true;
}

bool invoke_macro_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmSyntax **out_syntax, IdmError *err) {
    if (ctx->macro_depth >= 128) return expand_error(err, head->span, "macro expansion depth exceeded while expanding '%s'", head->as.text);
    IdmSyntax *use_copy = idm_syn_clone(use_syntax);
    if (!use_copy) return idm_error_oom(err, use_syntax->span);
    IdmScopeId use_scope = idm_scope_fresh(&ctx->scope_store);
    if (!idm_syn_scope_add_tree(use_copy, 0, use_scope)) {
        idm_syn_free(use_copy);
        return idm_error_oom(err, use_syntax->span);
    }
    IdmSyntax *expanded_syntax = NULL;
    IdmMacroRunner *runner = payload < ctx->macro_count ? &ctx->local_runner : ctx->runner;
    if (!runner || !runner->invoke) {
        idm_syn_free(use_copy);
        return idm_error_set(err, head->span, "macro runner is not available for transformer '%s'", head->as.text);
    }
    bool old_intro_active = ctx->rt->macro_intro_active;
    IdmScopeId old_intro_scope = ctx->rt->macro_intro_scope;
    ctx->rt->macro_intro_active = true;
    ctx->rt->macro_intro_scope = use_scope;
    bool invoked = runner->invoke(runner->user, ctx->rt, payload, use_copy, &expanded_syntax, err);
    ctx->rt->macro_intro_active = old_intro_active;
    ctx->rt->macro_intro_scope = old_intro_scope;
    if (!invoked) {
        if (err && err->present) {
            const char *what = payload < ctx->macro_count ? "in expansion of" : "while resolving";
            if (use_syntax->span.line != 0) {
                idm_error_note(err, "%s '%s' (%s:%u:%u)", what, head->as.text, use_syntax->span.file ? use_syntax->span.file : "<unknown>", use_syntax->span.line, use_syntax->span.column);
            } else {
                idm_error_note(err, "%s '%s'", what, head->as.text);
            }
        }
        idm_syn_free(use_copy);
        return false;
    }
    idm_syn_free(use_copy);
    if (!idm_syn_scope_flip_tree(expanded_syntax, 0, use_scope)) {
        idm_syn_free(expanded_syntax);
        return idm_error_oom(err, use_syntax->span);
    }
    if (!idm_syn_origin_push_tree(expanded_syntax, head->as.text)) {
        idm_syn_free(expanded_syntax);
        return idm_error_oom(err, use_syntax->span);
    }
    for (size_t i = 0; i < use_syntax->origins.count; i++) {
        if (!idm_syn_origin_push_tree(expanded_syntax, use_syntax->origins.items[i])) {
            idm_syn_free(expanded_syntax);
            return idm_error_oom(err, use_syntax->span);
        }
    }
    *out_syntax = expanded_syntax;
    return true;
}

IdmCore *expand_macro_use(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmError *err) {
    IdmSyntax *expanded_syntax = NULL;
    if (!invoke_macro_to_syntax(ctx, use_syntax, head, payload, &expanded_syntax, err)) return NULL;
    ctx->macro_depth++;
    IdmCore *expanded = expand_syntax(ctx, expanded_syntax, err);
    ctx->macro_depth--;
    idm_syn_free(expanded_syntax);
    return expanded;
}

static bool expand_macro_syntax_only(ExpandContext *ctx, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err) {
    if (syn_is_protocol(syntax, "%-group")) {
        if (syntax->as.seq.count != 2) return idm_error_set(err, syntax->span, "%-group expects one child");
        return expand_macro_syntax_only(ctx, syntax->as.seq.items[1], out_syntax, err);
    }
    if (syn_is_protocol(syntax, "%-expr") && syntax->as.seq.count >= 2 && syntax->as.seq.items[1]->kind == IDM_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, syntax->as.seq.items[1], &payload, err)) {
            IdmSyntax *expanded = NULL;
            if (!invoke_macro_to_syntax(ctx, syntax, syntax->as.seq.items[1], payload, &expanded, err)) return false;
            ctx->macro_depth++;
            bool ok = expand_macro_syntax_only(ctx, expanded, out_syntax, err);
            ctx->macro_depth--;
            idm_syn_free(expanded);
            return ok;
        }
        if (err && err->present) return false;
    }
    *out_syntax = idm_syn_clone(syntax);
    if (!*out_syntax) return idm_error_oom(err, syntax ? syntax->span : idm_span_unknown(NULL));
    return true;
}

bool local_expand_callback(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err) {
    (void)rt;
    return expand_macro_syntax_only((ExpandContext *)user, syntax, out_syntax, err);
}

bool free_identifier_eq_callback(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err) {
    (void)rt;
    ExpandContext *ctx = user;
    if (!a || !b || a->kind != IDM_SYN_WORD || b->kind != IDM_SYN_WORD) return idm_error_set(err, a ? a->span : idm_span_unknown(NULL), "free-identifier=? expects identifier syntax");
    if (strcmp(a->as.text, b->as.text) != 0) {
        *out_equal = false;
        return true;
    }
    IdmResolveStatus a_status = IDM_RESOLVE_UNBOUND;
    IdmResolveStatus b_status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *a_binding = resolve_default(ctx, a, &a_status);
    const IdmBinding *b_binding = resolve_default(ctx, b, &b_status);
    if (a_status == IDM_RESOLVE_AMBIGUOUS || b_status == IDM_RESOLVE_AMBIGUOUS) return idm_error_set(err, a->span, "free-identifier=? saw an ambiguous identifier");
    if (a_status == IDM_RESOLVE_OK || b_status == IDM_RESOLVE_OK) {
        *out_equal = a_status == IDM_RESOLVE_OK && b_status == IDM_RESOLVE_OK && a_binding && b_binding && a_binding->id == b_binding->id;
        return true;
    }
    const IdmScopeSet *as = idm_syn_scope_set(a, 0);
    const IdmScopeSet *bs = idm_syn_scope_set(b, 0);
    IdmScopeSet empty_a;
    IdmScopeSet empty_b;
    idm_scope_set_init(&empty_a);
    idm_scope_set_init(&empty_b);
    *out_equal = idm_scope_set_equal(as ? as : &empty_a, bs ? bs : &empty_b);
    idm_scope_set_destroy(&empty_a);
    idm_scope_set_destroy(&empty_b);
    return true;
}

IdmCore *expand_macro_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t payload, IdmError *err) {
    IdmSyntax *use = idm_syn_list(items[start]->span);
    if (!use || !idm_syn_append(use, idm_syn_word("%-expr", items[start]->span))) {
        idm_syn_free(use);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    for (int phase = 0; phase < 2; phase++) {
        const IdmScopeSet *scopes = idm_syn_scope_set(items[start], phase);
        for (size_t i = 0; scopes && i < scopes->count; i++) {
            if (!idm_syn_scope_add(use, phase, scopes->items[i])) {
                idm_syn_free(use);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
            }
        }
    }
    for (size_t i = start; i < end; i++) {
        IdmSyntax *item = idm_syn_clone(items[i]);
        if (!item || !idm_syn_append(use, item)) {
            idm_syn_free(item);
            idm_syn_free(use);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[i]->span);
        }
    }
    if (!idm_syn_property_set(use, "value-context", ctx->value_context ? "true" : "false") ||
        !idm_syn_property_set(use, "command-sub", ctx->command_sub_context ? "true" : "false")) {
        idm_syn_free(use);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    IdmCore *core = expand_macro_use(ctx, use, items[start], payload, err);
    idm_syn_free(use);
    return core;
}

bool register_macro(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, bool exported, IdmError *err) {
    const char *name = name_syntax && name_syntax->kind == IDM_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return idm_error_set(err, span, "defmacro name must be an identifier");
    if (!fn || fn->kind != IDM_CORE_FN) return idm_error_set(err, span, "defmacro must compile to a transformer function");
    if (fn->as.fn.arity != 1) return idm_error_set(err, span, "defmacro transformer must accept exactly one syntax argument");
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return idm_error_oom(err, span);
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = idm_strdup(name);
    if (!macro->name) return idm_error_oom(err, span);
    macro->exported = exported;
    macro->phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    macro->phase_env = idm_phase_env_retain(ctx->phase_env);
    macro->module = idm_module_ref_create(ctx->rt);
    if (!macro->module) {
        macro_def_destroy(macro);
        return idm_error_oom(err, span);
    }
    if (!idm_core_compile_function_body(fn->as.fn.body, name, fn->as.fn.arity, &macro->module->module, &macro->function_index, err)) {
        macro_def_destroy(macro);
        return false;
    }
    uint32_t payload = (uint32_t)ctx->macro_count;
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) {
        macro_def_destroy(macro);
        return idm_error_oom(err, span);
    }
    if (surface_install_guard(ctx, ctx->unit, ctx->unit_key, name, name, IDM_BIND_SPACE_DEFAULT, &scopes, err) < 0 ||
        !idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, &scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&scopes);
        macro_def_destroy(macro);
        return (err && err->present) ? false : idm_error_oom(err, span);
    }
    idm_scope_set_destroy(&scopes);
    ctx->macro_count++;
    return true;
}

bool register_resolver(ExpandContext *ctx, IdmCore *fn, IdmSpan span, IdmError *err) {
    if (!fn || fn->kind != IDM_CORE_FN) return idm_error_set(err, span, "resolver must compile to a transformer function");
    if (fn->as.fn.arity != 1) return idm_error_set(err, span, "resolver transformer must accept exactly one syntax argument");
    if (fn->as.fn.capture_count != 0) return idm_error_set(err, span, "resolver transformer captures require phase environment support");
    if (ctx->decl_resolver) return idm_error_set(err, span, "only one resolver may be declared per protocol");
    ctx->decl_resolver_module = idm_module_ref_create(ctx->rt);
    if (!ctx->decl_resolver_module) return idm_error_oom(err, span);
    if (!idm_core_compile_function_body(fn->as.fn.body, "%resolver", fn->as.fn.arity, &ctx->decl_resolver_module->module, &ctx->decl_resolver_fn, err)) {
        idm_module_ref_release(ctx->decl_resolver_module);
        ctx->decl_resolver_module = NULL;
        return false;
    }
    ctx->decl_resolver = true;
    ctx->decl_resolver_phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    ctx->decl_resolver_phase_env = idm_phase_env_retain(ctx->phase_env);
    return true;
}

bool register_macro_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, IdmValue transformer, IdmError *err) {
    ExpandContext *ctx = user;
    const char *name = name_syntax && name_syntax->kind == IDM_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return idm_error_set(err, name_syntax ? name_syntax->span : idm_span_unknown(NULL), "macro name must be an identifier");
    const IdmBytecodeModule *closure_module = idm_closure_module(transformer);
    if (!closure_module) return idm_error_set(err, name_syntax->span, "macro transformer must be a module-backed function value");
    uint32_t fn_index = idm_closure_function_index(transformer);
    if (fn_index >= closure_module->function_count || closure_module->functions[fn_index].arity != 1) {
        return idm_error_set(err, name_syntax->span, "macro transformer must accept exactly one syntax argument");
    }
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return idm_error_oom(err, name_syntax->span);
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = idm_strdup(name);
    if (!macro->name) return idm_error_oom(err, name_syntax->span);
    if (!idm_runtime_register_gc_value(rt, transformer)) {
        free(macro->name);
        macro->name = NULL;
        return idm_error_oom(err, name_syntax->span);
    }
    macro->closure_backed = true;
    macro->transformer = transformer;
    macro->rt = rt;
    macro->exported = false;
    macro->phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    macro->phase_env = idm_phase_env_retain(ctx->phase_env);
    uint32_t payload = (uint32_t)ctx->macro_count;
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) {
        macro_def_destroy(macro);
        return idm_error_oom(err, name_syntax->span);
    }
    if (surface_install_guard(ctx, ctx->unit, ctx->unit_key, name, name, IDM_BIND_SPACE_DEFAULT, &scopes, err) < 0 ||
        !idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, &scopes, payload, ctx->frame, NULL)) {
        idm_scope_set_destroy(&scopes);
        macro_def_destroy(macro);
        return (err && err->present) ? false : idm_error_oom(err, name_syntax->span);
    }
    idm_scope_set_destroy(&scopes);
    ctx->macro_count++;
    return true;
}

bool install_imported_macro(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err) {
    int guard = surface_install_guard(ctx, provider, provider_key, name, name, IDM_BIND_SPACE_DEFAULT, scopes ? scopes : &ctx->empty_scopes, err);
    if (guard <= 0) return guard == 0;
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return idm_error_oom(err, idm_span_unknown(NULL));
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = idm_strdup(name);
    if (!macro->name) return idm_error_oom(err, idm_span_unknown(NULL));
    macro->module = idm_module_ref_retain(module);
    macro->function_index = function_index;
    macro->phase_ns = phase_ns ? phase_ns : ctx->phase_ns;
    macro->phase_env = idm_phase_env_retain(phase_env ? phase_env : ctx->phase_env);
    macro->exported = false;
    uint32_t payload = (uint32_t)ctx->macro_count;
    if (!idm_binding_table_add(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, scopes ? scopes : &ctx->empty_scopes, payload, ctx->frame, NULL)) {
        macro_def_destroy(macro);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ctx->macro_count++;
    return true;
}

bool install_imported_operator(ExpandContext *ctx, const IdmOperatorDef *op, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err) {
    return register_operator(ctx, op->name, op->precedence, op->assoc, op->fixity, op->target_kind, op->primitive, op->target_name, &op->scopes, binding_scopes ? binding_scopes : &ctx->empty_scopes, provider, provider_key, false, err);
}

static const char *operator_decl_token_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD) return syn->as.text;
    if (syn_is_protocol(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

bool register_operator_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, int64_t precedence, const char *assoc_text, const char *fixity_text, const IdmSyntax *target_syntax, IdmError *err) {
    (void)rt;
    ExpandContext *ctx = user;
    const char *name = operator_decl_token_text(name_syntax);
    if (!name) return idm_error_set(err, name_syntax->span, "operator name must be a symbol");
    if (precedence < 0 || precedence > 255) return idm_error_set(err, name_syntax->span, "operator precedence must be an integer 0..255");
    IdmOpAssoc assoc;
    if (strcmp(assoc_text, "left") == 0) assoc = IDM_OP_ASSOC_LEFT;
    else if (strcmp(assoc_text, "right") == 0) assoc = IDM_OP_ASSOC_RIGHT;
    else if (strcmp(assoc_text, "none") == 0) assoc = IDM_OP_ASSOC_NONE;
    else return idm_error_set(err, name_syntax->span, "operator assoc must be left, right, or none");
    IdmOpFixity fixity;
    if (strcmp(fixity_text, "infix") == 0) fixity = IDM_OP_FIX_INFIX;
    else if (strcmp(fixity_text, "prefix") == 0) fixity = IDM_OP_FIX_PREFIX;
    else if (strcmp(fixity_text, "postfix") == 0) fixity = IDM_OP_FIX_POSTFIX;
    else return idm_error_set(err, name_syntax->span, "operator fixity must be infix, prefix, or postfix");
    const char *target = operator_decl_token_text(target_syntax);
    if (!target) return idm_error_set(err, target_syntax->span, "operator target must be an identifier");
    IdmOpTargetKind tk;
    IdmPrimitive prim = IDM_PRIM_ADD;
    const char *target_name = NULL;
    if (strcmp(target, "thread-first") == 0) {
        tk = IDM_OP_TGT_THREAD_FIRST;
        if (fixity != IDM_OP_FIX_INFIX) return idm_error_set(err, name_syntax->span, "thread operators must be infix");
    } else if (strcmp(target, "thread-last") == 0) {
        tk = IDM_OP_TGT_THREAD_LAST;
        if (fixity != IDM_OP_FIX_INFIX) return idm_error_set(err, name_syntax->span, "thread operators must be infix");
    } else if (idm_prim_lookup_by_name(target, &prim)) {
        tk = IDM_OP_TGT_PRIMITIVE;
    } else {
        tk = IDM_OP_TGT_NAMED;
        target_name = target;
    }
    IdmScopeSet decl_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &decl_scopes)) return idm_error_oom(err, name_syntax->span);
    IdmScopeSet target_scopes;
    if (!syntax_scopes_copy(&target_scopes, target_syntax)) {
        idm_scope_set_destroy(&decl_scopes);
        return idm_error_oom(err, target_syntax->span);
    }
    if (ctx->rt->macro_intro_active && !idm_scope_set_flip(&target_scopes, ctx->rt->macro_intro_scope)) {
        idm_scope_set_destroy(&decl_scopes);
        idm_scope_set_destroy(&target_scopes);
        return idm_error_oom(err, target_syntax->span);
    }
    bool ok = register_operator(ctx, name, (uint8_t)precedence, assoc, fixity, tk, prim, target_name, tk == IDM_OP_TGT_NAMED ? &target_scopes : &decl_scopes, &decl_scopes, ctx->unit, ctx->unit_key, true, err);
    idm_scope_set_destroy(&decl_scopes);
    idm_scope_set_destroy(&target_scopes);
    return ok;
}
