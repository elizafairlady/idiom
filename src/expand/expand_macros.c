#include "internal.h"

static bool expand_macro_syntax_only(ExpandContext *ctx, const IshSyntax *syntax, IshSyntax **out_syntax, IshError *err);
static const char *operator_decl_token_text(const IshSyntax *syn);

bool run_phase_core(ExpandContext *ctx, IshCore *core, IshError *err) {
    IshBytecodeModule *module = malloc(sizeof(*module));
    if (!module) return ish_error_oom(err, ish_span_unknown(NULL));
    ish_bc_init(module);
    uint32_t main_fn = 0;
    if (!ish_core_compile_main(core, module, &main_fn, err)) {
        ish_bc_destroy(module);
        free(module);
        return false;
    }
    IshNamespace *old_main_ns = ctx->rt->main_ns;
    if (ctx->phase_ns) ctx->rt->main_ns = ctx->phase_ns;
    IshValue ignored = ish_nil();
    bool ok = ish_vm_run(ctx->rt, module, main_fn, &ignored, err);
    ctx->rt->main_ns = old_main_ns;
    if (ok) {
        if (!ish_phase_env_add_module(ctx->phase_env, module, main_fn)) {
            ish_bc_destroy(module);
            free(module);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
    } else {
        ish_bc_destroy(module);
        free(module);
    }
    return ok;
}

bool resolve_transformer(ExpandContext *ctx, const IshSyntax *head, uint32_t *out_payload, IshError *err) {
    if (!head || head->kind != ISH_SYN_WORD) return false;
    const IshScopeSet *scopes = ish_syn_scope_set(head, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup_scopes = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, head->as.text, 0, ISH_BIND_SPACE_DEFAULT, lookup_scopes, &binding);
    ish_scope_set_destroy(&empty);
    if (status == ISH_RESOLVE_AMBIGUOUS) {
        expand_error(err, head->span, "ambiguous transformer '%s'", head->as.text);
        return false;
    }
    if (status != ISH_RESOLVE_OK || !binding || binding->kind != ISH_BIND_TRANSFORMER) return false;
    *out_payload = binding->payload;
    return true;
}

bool resolve_head_resolver(ExpandContext *ctx, const IshSyntax *head, uint32_t *out_payload, IshError *err) {
    if (!head || head->kind != ISH_SYN_WORD) return false;
    IshSyntax *resolver = ish_syn_word("%resolver", head->span);
    if (!resolver) {
        ish_error_oom(err, head->span);
        return false;
    }
    const IshScopeSet *scopes = ish_syn_scope_set(head, 0);
    if (scopes) {
        for (size_t i = 0; i < scopes->count; i++) {
            if (!ish_syn_scope_add(resolver, 0, scopes->items[i])) {
                ish_syn_free(resolver);
                ish_error_oom(err, head->span);
                return false;
            }
        }
    }
    bool ok = resolve_transformer(ctx, resolver, out_payload, err);
    ish_syn_free(resolver);
    return ok;
}

bool local_macro_invoke(void *user, IshRuntime *rt, uint32_t payload, const IshSyntax *use_syntax, IshSyntax **out_syntax, IshError *err) {
    ExpandContext *ctx = user;
    if (payload >= ctx->macro_count) return ish_error_set(err, use_syntax ? use_syntax->span : ish_span_unknown(NULL), "macro transformer payload %u is out of bounds", payload);
    IshValue arg = ish_syntax_value(rt, use_syntax, err);
    if (err && err->present) return false;
    IshValue result = ish_nil();
    IshNamespace *old_main_ns = rt->main_ns;
    IshNamespace *macro_ns = ctx->macros[payload].phase_env ? ctx->macros[payload].phase_env->ns : ctx->macros[payload].phase_ns;
    if (macro_ns) rt->main_ns = macro_ns;
    bool ok = ctx->macros[payload].closure_backed
        ? ish_vm_call_closure(rt, ctx->macros[payload].transformer, &arg, 1, &result, err)
        : ish_vm_call_function(rt, &ctx->macros[payload].module->module, ctx->macros[payload].function_index, &arg, 1, &result, err);
    rt->main_ns = old_main_ns;
    if (!ok) return false;
    const IshSyntax *result_syntax = ish_syntax_get(result, err);
    if (!result_syntax) return false;
    *out_syntax = ish_syn_clone(result_syntax);
    if (!*out_syntax) return ish_error_oom(err, use_syntax ? use_syntax->span : ish_span_unknown(NULL));
    return true;
}

bool invoke_macro_to_syntax(ExpandContext *ctx, const IshSyntax *use_syntax, const IshSyntax *head, uint32_t payload, IshSyntax **out_syntax, IshError *err) {
    if (ctx->macro_depth >= 128) return expand_error(err, head->span, "macro expansion depth exceeded while expanding '%s'", head->as.text);
    IshSyntax *use_copy = ish_syn_clone(use_syntax);
    if (!use_copy) return ish_error_oom(err, use_syntax->span);
    IshScopeId use_scope = ish_scope_fresh(&ctx->scope_store);
    if (!ish_syn_scope_add_tree(use_copy, 0, use_scope)) {
        ish_syn_free(use_copy);
        return ish_error_oom(err, use_syntax->span);
    }
    IshSyntax *expanded_syntax = NULL;
    IshMacroRunner *runner = payload < ctx->macro_count ? &ctx->local_runner : ctx->runner;
    if (!runner || !runner->invoke) {
        ish_syn_free(use_copy);
        return ish_error_set(err, head->span, "macro runner is not available for transformer '%s'", head->as.text);
    }
    bool old_intro_active = ctx->rt->macro_intro_active;
    IshScopeId old_intro_scope = ctx->rt->macro_intro_scope;
    ctx->rt->macro_intro_active = true;
    ctx->rt->macro_intro_scope = use_scope;
    bool invoked = runner->invoke(runner->user, ctx->rt, payload, use_copy, &expanded_syntax, err);
    ctx->rt->macro_intro_active = old_intro_active;
    ctx->rt->macro_intro_scope = old_intro_scope;
    if (!invoked) {
        if (err && err->present) {
            const char *what = payload < ctx->macro_count ? "in expansion of" : "while resolving";
            if (use_syntax->span.line != 0) {
                ish_error_note(err, "%s '%s' (%s:%u:%u)", what, head->as.text, use_syntax->span.file ? use_syntax->span.file : "<unknown>", use_syntax->span.line, use_syntax->span.column);
            } else {
                ish_error_note(err, "%s '%s'", what, head->as.text);
            }
        }
        ish_syn_free(use_copy);
        return false;
    }
    ish_syn_free(use_copy);
    if (!ish_syn_scope_flip_tree(expanded_syntax, 0, use_scope)) {
        ish_syn_free(expanded_syntax);
        return ish_error_oom(err, use_syntax->span);
    }
    if (!ish_syn_origin_push_tree(expanded_syntax, head->as.text)) {
        ish_syn_free(expanded_syntax);
        return ish_error_oom(err, use_syntax->span);
    }
    for (size_t i = 0; i < use_syntax->origins.count; i++) {
        if (!ish_syn_origin_push_tree(expanded_syntax, use_syntax->origins.items[i])) {
            ish_syn_free(expanded_syntax);
            return ish_error_oom(err, use_syntax->span);
        }
    }
    *out_syntax = expanded_syntax;
    return true;
}

IshCore *expand_macro_use(ExpandContext *ctx, const IshSyntax *use_syntax, const IshSyntax *head, uint32_t payload, IshError *err) {
    IshSyntax *expanded_syntax = NULL;
    if (!invoke_macro_to_syntax(ctx, use_syntax, head, payload, &expanded_syntax, err)) return NULL;
    ctx->macro_depth++;
    IshCore *expanded = expand_syntax(ctx, expanded_syntax, err);
    ctx->macro_depth--;
    ish_syn_free(expanded_syntax);
    return expanded;
}

static bool expand_macro_syntax_only(ExpandContext *ctx, const IshSyntax *syntax, IshSyntax **out_syntax, IshError *err) {
    if (syn_is_protocol(syntax, "%-group")) {
        if (syntax->as.seq.count != 2) return ish_error_set(err, syntax->span, "%-group expects one child");
        return expand_macro_syntax_only(ctx, syntax->as.seq.items[1], out_syntax, err);
    }
    if (syn_is_protocol(syntax, "%-expr") && syntax->as.seq.count >= 2 && syntax->as.seq.items[1]->kind == ISH_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, syntax->as.seq.items[1], &payload, err)) {
            IshSyntax *expanded = NULL;
            if (!invoke_macro_to_syntax(ctx, syntax, syntax->as.seq.items[1], payload, &expanded, err)) return false;
            ctx->macro_depth++;
            bool ok = expand_macro_syntax_only(ctx, expanded, out_syntax, err);
            ctx->macro_depth--;
            ish_syn_free(expanded);
            return ok;
        }
        if (err && err->present) return false;
    }
    *out_syntax = ish_syn_clone(syntax);
    if (!*out_syntax) return ish_error_oom(err, syntax ? syntax->span : ish_span_unknown(NULL));
    return true;
}

bool local_expand_callback(void *user, IshRuntime *rt, const IshSyntax *syntax, IshSyntax **out_syntax, IshError *err) {
    (void)rt;
    return expand_macro_syntax_only((ExpandContext *)user, syntax, out_syntax, err);
}

bool free_identifier_eq_callback(void *user, IshRuntime *rt, const IshSyntax *a, const IshSyntax *b, bool *out_equal, IshError *err) {
    (void)rt;
    ExpandContext *ctx = user;
    if (!a || !b || a->kind != ISH_SYN_WORD || b->kind != ISH_SYN_WORD) return ish_error_set(err, a ? a->span : ish_span_unknown(NULL), "free-identifier=? expects identifier syntax");
    if (strcmp(a->as.text, b->as.text) != 0) {
        *out_equal = false;
        return true;
    }
    IshResolveStatus a_status = ISH_RESOLVE_UNBOUND;
    IshResolveStatus b_status = ISH_RESOLVE_UNBOUND;
    const IshBinding *a_binding = resolve_default(ctx, a, &a_status);
    const IshBinding *b_binding = resolve_default(ctx, b, &b_status);
    if (a_status == ISH_RESOLVE_AMBIGUOUS || b_status == ISH_RESOLVE_AMBIGUOUS) return ish_error_set(err, a->span, "free-identifier=? saw an ambiguous identifier");
    if (a_status == ISH_RESOLVE_OK || b_status == ISH_RESOLVE_OK) {
        *out_equal = a_status == ISH_RESOLVE_OK && b_status == ISH_RESOLVE_OK && a_binding && b_binding && a_binding->id == b_binding->id;
        return true;
    }
    const IshScopeSet *as = ish_syn_scope_set(a, 0);
    const IshScopeSet *bs = ish_syn_scope_set(b, 0);
    IshScopeSet empty_a;
    IshScopeSet empty_b;
    ish_scope_set_init(&empty_a);
    ish_scope_set_init(&empty_b);
    *out_equal = ish_scope_set_equal(as ? as : &empty_a, bs ? bs : &empty_b);
    ish_scope_set_destroy(&empty_a);
    ish_scope_set_destroy(&empty_b);
    return true;
}

IshCore *expand_macro_use_from_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, uint32_t payload, IshError *err) {
    IshSyntax *use = ish_syn_list(ISH_SEQ_PAREN, items[start]->span);
    if (!use || !ish_syn_append(use, ish_syn_word("%-expr", items[start]->span))) {
        ish_syn_free(use);
        return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
    }
    for (size_t i = start; i < end; i++) {
        IshSyntax *item = ish_syn_clone(items[i]);
        if (!item || !ish_syn_append(use, item)) {
            ish_syn_free(item);
            ish_syn_free(use);
            return (IshCore *)(uintptr_t)ish_error_oom(err, items[i]->span);
        }
    }
    if (!ish_syn_property_set(use, "value-context", ctx->value_context ? "true" : "false") ||
        !ish_syn_property_set(use, "command-sub", ctx->command_sub_context ? "true" : "false")) {
        ish_syn_free(use);
        return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
    }
    IshCore *core = expand_macro_use(ctx, use, items[start], payload, err);
    ish_syn_free(use);
    return core;
}

bool register_macro(ExpandContext *ctx, const IshSyntax *name_syntax, IshCore *fn, IshSpan span, bool exported, IshError *err) {
    const char *name = name_syntax && name_syntax->kind == ISH_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return ish_error_set(err, span, "defmacro name must be an identifier");
    if (!fn || fn->kind != ISH_CORE_FN) return ish_error_set(err, span, "defmacro must compile to a transformer function");
    if (fn->as.fn.arity != 1) return ish_error_set(err, span, "defmacro transformer must accept exactly one syntax argument");
    if (fn->as.fn.capture_count != 0) return ish_error_set(err, span, "defmacro transformer captures require phase environment support");
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return ish_error_oom(err, span);
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = ish_strdup(name);
    if (!macro->name) return ish_error_oom(err, span);
    macro->exported = exported;
    macro->phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    macro->phase_env = ish_phase_env_retain(ctx->phase_env);
    macro->module = ish_module_ref_create(ctx->rt);
    if (!macro->module) {
        macro_def_destroy(macro);
        return ish_error_oom(err, span);
    }
    if (!ish_core_compile_function_body(fn->as.fn.body, name, fn->as.fn.arity, &macro->module->module, &macro->function_index, err)) {
        macro_def_destroy(macro);
        return false;
    }
    uint32_t payload = (uint32_t)ctx->macro_count;
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, name_syntax)) {
        macro_def_destroy(macro);
        return ish_error_oom(err, span);
    }
    if (!ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_TRANSFORMER, &scopes, payload, ctx->frame, NULL)) {
        ish_scope_set_destroy(&scopes);
        macro_def_destroy(macro);
        return ish_error_oom(err, span);
    }
    ish_scope_set_destroy(&scopes);
    ctx->macro_count++;
    return true;
}

bool register_resolver(ExpandContext *ctx, IshCore *fn, IshSpan span, IshError *err) {
    if (!fn || fn->kind != ISH_CORE_FN) return ish_error_set(err, span, "resolver must compile to a transformer function");
    if (fn->as.fn.arity != 1) return ish_error_set(err, span, "resolver transformer must accept exactly one syntax argument");
    if (fn->as.fn.capture_count != 0) return ish_error_set(err, span, "resolver transformer captures require phase environment support");
    if (ctx->decl_resolver) return ish_error_set(err, span, "only one resolver may be declared per protocol");
    ctx->decl_resolver_module = ish_module_ref_create(ctx->rt);
    if (!ctx->decl_resolver_module) return ish_error_oom(err, span);
    if (!ish_core_compile_function_body(fn->as.fn.body, "%resolver", fn->as.fn.arity, &ctx->decl_resolver_module->module, &ctx->decl_resolver_fn, err)) {
        ish_module_ref_release(ctx->decl_resolver_module);
        ctx->decl_resolver_module = NULL;
        return false;
    }
    ctx->decl_resolver = true;
    ctx->decl_resolver_phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    ctx->decl_resolver_phase_env = ish_phase_env_retain(ctx->phase_env);
    return true;
}

bool register_macro_callback(void *user, IshRuntime *rt, const IshSyntax *name_syntax, IshValue transformer, IshError *err) {
    ExpandContext *ctx = user;
    const char *name = name_syntax && name_syntax->kind == ISH_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return ish_error_set(err, name_syntax ? name_syntax->span : ish_span_unknown(NULL), "macro name must be an identifier");
    const IshBytecodeModule *closure_module = ish_closure_module(transformer);
    if (!closure_module) return ish_error_set(err, name_syntax->span, "macro transformer must be a module-backed function value");
    uint32_t fn_index = ish_closure_function_index(transformer);
    if (fn_index >= closure_module->function_count || closure_module->functions[fn_index].arity != 1) {
        return ish_error_set(err, name_syntax->span, "macro transformer must accept exactly one syntax argument");
    }
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return ish_error_oom(err, name_syntax->span);
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = ish_strdup(name);
    if (!macro->name) return ish_error_oom(err, name_syntax->span);
    if (!ish_runtime_register_gc_value(rt, transformer)) {
        free(macro->name);
        macro->name = NULL;
        return ish_error_oom(err, name_syntax->span);
    }
    macro->closure_backed = true;
    macro->transformer = transformer;
    macro->rt = rt;
    macro->exported = false;
    macro->phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    macro->phase_env = ish_phase_env_retain(ctx->phase_env);
    uint32_t payload = (uint32_t)ctx->macro_count;
    IshScopeSet scopes;
    if (!syntax_scopes_copy(&scopes, name_syntax)) {
        macro_def_destroy(macro);
        return ish_error_oom(err, name_syntax->span);
    }
    if (!ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_TRANSFORMER, &scopes, payload, ctx->frame, NULL)) {
        ish_scope_set_destroy(&scopes);
        macro_def_destroy(macro);
        return ish_error_oom(err, name_syntax->span);
    }
    ish_scope_set_destroy(&scopes);
    ctx->macro_count++;
    return true;
}

bool install_imported_macro(ExpandContext *ctx, const char *name, const IshScopeSet *scopes, IshModuleRef *module, uint32_t function_index, IshNamespace *phase_ns, IshPhaseEnv *phase_env, IshError *err) {
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) return ish_error_oom(err, ish_span_unknown(NULL));
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = ish_strdup(name);
    if (!macro->name) return ish_error_oom(err, ish_span_unknown(NULL));
    macro->module = ish_module_ref_retain(module);
    macro->function_index = function_index;
    macro->phase_ns = phase_ns ? phase_ns : ctx->phase_ns;
    macro->phase_env = ish_phase_env_retain(phase_env ? phase_env : ctx->phase_env);
    macro->exported = false;
    uint32_t payload = (uint32_t)ctx->macro_count;
    if (!ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_TRANSFORMER, scopes ? scopes : &ctx->empty_scopes, payload, ctx->frame, NULL)) {
        macro_def_destroy(macro);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    ctx->macro_count++;
    return true;
}

bool install_imported_operator(ExpandContext *ctx, const IshOperatorDef *op, IshError *err) {
    if (!register_operator(ctx, op->name, op->precedence, op->assoc, op->fixity, op->target_kind, op->primitive, op->target_name, &op->scopes, err)) return false;
    ctx->operators[ctx->operator_count - 1u].exported = false;
    return true;
}

static const char *operator_decl_token_text(const IshSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == ISH_SYN_WORD) return syn->as.text;
    if (syn_is_protocol(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == ISH_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

bool register_operator_callback(void *user, IshRuntime *rt, const IshSyntax *name_syntax, int64_t precedence, const char *assoc_text, const char *fixity_text, const IshSyntax *target_syntax, IshError *err) {
    (void)rt;
    ExpandContext *ctx = user;
    const char *name = operator_decl_token_text(name_syntax);
    if (!name) return ish_error_set(err, name_syntax->span, "operator name must be a symbol");
    if (precedence < 0 || precedence > 255) return ish_error_set(err, name_syntax->span, "operator precedence must be an integer 0..255");
    IshOpAssoc assoc;
    if (strcmp(assoc_text, "left") == 0) assoc = ISH_OP_ASSOC_LEFT;
    else if (strcmp(assoc_text, "right") == 0) assoc = ISH_OP_ASSOC_RIGHT;
    else if (strcmp(assoc_text, "none") == 0) assoc = ISH_OP_ASSOC_NONE;
    else return ish_error_set(err, name_syntax->span, "operator assoc must be left, right, or none");
    IshOpFixity fixity;
    if (strcmp(fixity_text, "infix") == 0) fixity = ISH_OP_FIX_INFIX;
    else if (strcmp(fixity_text, "prefix") == 0) fixity = ISH_OP_FIX_PREFIX;
    else if (strcmp(fixity_text, "postfix") == 0) fixity = ISH_OP_FIX_POSTFIX;
    else return ish_error_set(err, name_syntax->span, "operator fixity must be infix, prefix, or postfix");
    const char *target = operator_decl_token_text(target_syntax);
    if (!target) return ish_error_set(err, target_syntax->span, "operator target must be an identifier");
    IshOpTargetKind tk;
    IshPrimitive prim = ISH_PRIM_ADD;
    const char *target_name = NULL;
    if (strcmp(target, "thread-first") == 0) {
        tk = ISH_OP_TGT_THREAD_FIRST;
        if (fixity != ISH_OP_FIX_INFIX) return ish_error_set(err, name_syntax->span, "thread operators must be infix");
    } else if (strcmp(target, "thread-last") == 0) {
        tk = ISH_OP_TGT_THREAD_LAST;
        if (fixity != ISH_OP_FIX_INFIX) return ish_error_set(err, name_syntax->span, "thread operators must be infix");
    } else if (ish_prim_lookup_by_name(target, &prim)) {
        tk = ISH_OP_TGT_PRIMITIVE;
    } else {
        tk = ISH_OP_TGT_NAMED;
        target_name = target;
    }
    return register_operator(ctx, name, (uint8_t)precedence, assoc, fixity, tk, prim, target_name, NULL, err);
}
