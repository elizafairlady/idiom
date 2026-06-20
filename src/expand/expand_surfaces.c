#include "internal.h"

static bool expand_macro_syntax_only(ExpandContext *ctx, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err), bind_macro_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *name, uint32_t payload, IdmSpan span, IdmError *err), phase_syntax_fn_compile(ExpandContext *ctx, PhaseSyntaxFn *out, IdmCore *fn, const char *debug_name, IdmSpan span, IdmError *err);
static const char *operator_decl_token_text(const IdmSyntax *syn);
static MacroDef *macro_slot(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err);
static void phase_syntax_fn_import(ExpandContext *ctx, PhaseSyntaxFn *out, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env);
static bool phase_syntax_callable_arity_one(const IdmCore *fn);

bool run_phase_core(ExpandContext *ctx, IdmCore *core, IdmError *err) {
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_bc_init(module);
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, module, &main_fn, err)) {
        idm_bc_destroy(module); free(module); return false;
    }
    IdmNamespace *old_main_ns = ctx->rt->main_ns;
    int old_trait_phase = ctx->rt->trait_phase;
    if (ctx->phase_ns) ctx->rt->main_ns = ctx->phase_ns;
    ctx->rt->trait_phase = 1;
    IdmValue ignored = idm_nil();
    bool ok = idm_vm_run(ctx->rt, module, main_fn, &ignored, err);
    ctx->rt->main_ns = old_main_ns;
    ctx->rt->trait_phase = old_trait_phase;
    if (ok) {
        if (!idm_phase_env_add_module(ctx->phase_env, module, main_fn, err)) {
            idm_bc_destroy(module); free(module); return idm_error_oom(err, idm_span_unknown(NULL));
        }
    } else {
        idm_bc_destroy(module); free(module);
    }
    return ok;
}

bool resolve_transformer(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_payload, IdmError *err) {
    if (!head || head->kind != IDM_SYN_WORD) return false;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, head, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, head->span, "ambiguous transformer '%s'", head->as.text); return false;
    }
    if (status != IDM_RESOLVE_OK || !binding || binding->kind != IDM_BIND_TRANSFORMER) return false;
    *out_payload = binding->payload;
    return true;
}

bool resolve_head_grammar(ExpandContext *ctx, const IdmSyntax *head, uint32_t *out_grammar_index, IdmError *err) {
    if (!head || head->kind != IDM_SYN_WORD) return false;
    const IdmScopeSet *scopes = idm_syn_scope_set(head, 0);
    if (scopes) {
        const GrammarDef *best = NULL;
        for (size_t i = 0; i < ctx->grammar_count; i++) {
            const GrammarDef *candidate = &ctx->grammars[i];
            if (!idm_scope_set_subset(&candidate->scopes, scopes)) continue;
            if (!best || idm_scope_set_subset(&best->scopes, &candidate->scopes)) best = candidate;
        }
        if (!best) return false;
        for (size_t i = 0; i < ctx->grammar_count; i++) {
            const GrammarDef *candidate = &ctx->grammars[i];
            if (!idm_scope_set_subset(&candidate->scopes, scopes)) continue;
            if (!idm_scope_set_subset(&candidate->scopes, &best->scopes)) {
                expand_error(err, head->span, "ambiguous grammar for '%s'", head->as.text); return false;
            }
        }
        *out_grammar_index = (uint32_t)(best - ctx->grammars);
        return true;
    }
    for (size_t i = ctx->grammar_count; i > 0; i--) {
        const GrammarDef *candidate = &ctx->grammars[i - 1u];
        if (candidate->scopes.count == 0) { *out_grammar_index = (uint32_t)(i - 1u); return true; }
    }
    return false;
}

static bool phase_syntax_call(IdmRuntime *rt, const IdmSyntax *use_syntax, const PhaseSyntaxFn *fn, IdmSyntax **out_syntax, IdmError *err) {
    if (!fn->closure_backed && !fn->module) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "phase syntax function module is not available");
    IdmValue arg = idm_syntax_value(rt, use_syntax, err);
    if (err && err->present) return false;
    IdmValue result = idm_nil();
    IdmNamespace *old_main_ns = rt->main_ns;
    int old_trait_phase = rt->trait_phase;
    IdmNamespace *call_ns = fn->phase_env ? fn->phase_env->ns : fn->phase_ns;
    if (call_ns) rt->main_ns = call_ns;
    rt->trait_phase = 1;
    IdmValue callee = fn->closure_backed
        ? fn->closure
        : idm_closure_in_module(rt, &fn->module->module, fn->function_index, NULL, 0, call_ns, err);
    if (err && err->present) {
        rt->main_ns = old_main_ns;
        rt->trait_phase = old_trait_phase;
        return false;
    }
    bool ok = idm_vm_call_closure(rt, callee, &arg, 1, &result, err);
    rt->main_ns = old_main_ns;
    rt->trait_phase = old_trait_phase;
    if (!ok) return false;
    const IdmSyntax *result_syntax = idm_syntax_get(result, err);
    if (!result_syntax) return false;
    *out_syntax = idm_syn_clone(result_syntax);
    return *out_syntax ? true : idm_error_oom(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL));
}

bool local_macro_invoke(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err) {
    ExpandContext *ctx = user; if (payload >= ctx->macro_count) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "macro transformer payload %u is out of bounds", payload);
    return phase_syntax_call(rt, use_syntax, &ctx->macros[payload].fn, out_syntax, err); }

static bool local_grammar_invoke(ExpandContext *ctx, IdmRuntime *rt, uint32_t grammar_index, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err) {
    if (grammar_index >= ctx->grammar_count) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "grammar index %u is out of bounds", grammar_index);
    return phase_syntax_call(rt, use_syntax, &ctx->grammars[grammar_index].fn, out_syntax, err); }

typedef bool (*SyntaxSurfaceInvokeFn)(ExpandContext *ctx, uint32_t payload, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err);

static bool invoke_macro_payload(ExpandContext *ctx, uint32_t payload, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err) {
    IdmMacroRunner *runner = payload < ctx->macro_count ? &ctx->local_runner : ctx->runner;
    if (!runner || !runner->invoke) return idm_error_set(err, head->span, "macro runner is not available for transformer '%s'", head->as.text);
    return runner->invoke(runner->user, ctx->rt, payload, use_copy, out_syntax, err);
}

static bool invoke_grammar_payload(ExpandContext *ctx, uint32_t grammar_index, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err) { (void)head; return local_grammar_invoke(ctx, ctx->rt, grammar_index, use_copy, out_syntax, err); }

static bool invoke_scoped_syntax_surface_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, const char *depth_action, const char *note_action, SyntaxSurfaceInvokeFn invoke, IdmSyntax **out_syntax, IdmError *err) {
    if (ctx->surface_depth >= 128) return expand_error(err, head->span, "%s '%s'", depth_action, head->as.text);
    IdmSyntax *use_copy = idm_syn_clone(use_syntax);
    if (!use_copy) return idm_error_oom(err, use_syntax->span);
    IdmScopeId use_scope = idm_scope_fresh(&ctx->scope_store);
    if (!idm_syn_scope_add_tree(use_copy, 0, use_scope)) {
        idm_syn_free(use_copy); return idm_error_oom(err, use_syntax->span);
    }
    IdmSyntax *expanded_syntax = NULL;
    bool old_intro_active = ctx->rt->macro_intro_active;
    IdmScopeId old_intro_scope = ctx->rt->macro_intro_scope;
    ctx->rt->macro_intro_active = true;
    ctx->rt->macro_intro_scope = use_scope;
    bool invoked = invoke(ctx, payload, use_copy, head, &expanded_syntax, err);
    ctx->rt->macro_intro_active = old_intro_active;
    ctx->rt->macro_intro_scope = old_intro_scope;
    if (!invoked) {
        if (err && err->present) {
            if (use_syntax->span.line != 0)
                idm_error_note(err, "%s '%s' (%s:%u:%u)", note_action, head->as.text, use_syntax->span.file ? use_syntax->span.file : "<unknown>", use_syntax->span.line, use_syntax->span.column);
            else
                idm_error_note(err, "%s '%s'", note_action, head->as.text);
        }
        idm_syn_free(use_copy);
        return false;
    }
    idm_syn_free(use_copy);
    if (!idm_syn_scope_flip_tree(expanded_syntax, 0, use_scope)) {
        idm_syn_free(expanded_syntax); return idm_error_oom(err, use_syntax->span);
    }
    if (!idm_syn_origin_push_tree(expanded_syntax, head->as.text)) {
        idm_syn_free(expanded_syntax); return idm_error_oom(err, use_syntax->span);
    }
    for (size_t i = 0; i < use_syntax->origins.count; i++) {
        if (!idm_syn_origin_push_tree(expanded_syntax, use_syntax->origins.items[i])) {
            idm_syn_free(expanded_syntax); return idm_error_oom(err, use_syntax->span);
        }
    }
    *out_syntax = expanded_syntax;
    return true;
}

bool invoke_macro_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmSyntax **out_syntax, IdmError *err) { return invoke_scoped_syntax_surface_to_syntax(ctx, use_syntax, head, payload, "macro expansion depth exceeded while expanding", "in expansion of", invoke_macro_payload, out_syntax, err); }

static bool invoke_grammar_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t grammar_index, IdmSyntax **out_syntax, IdmError *err) { return invoke_scoped_syntax_surface_to_syntax(ctx, use_syntax, head, grammar_index, "grammar expansion depth exceeded while expanding", "in grammar expansion of", invoke_grammar_payload, out_syntax, err); }

IdmCore *expand_macro_use(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmError *err) {
    IdmSyntax *expanded_syntax = NULL;
    if (!invoke_macro_to_syntax(ctx, use_syntax, head, payload, &expanded_syntax, err)) return NULL;
    ctx->surface_depth++;
    IdmCore *expanded = expand_syntax(ctx, expanded_syntax, err);
    ctx->surface_depth--; idm_syn_free(expanded_syntax);
    return expanded;
}

static bool expand_macro_syntax_only(ExpandContext *ctx, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err) {
    if (syn_is_form(syntax, "%-group")) {
        if (syntax->as.seq.count != 2) return idm_error_set(err, syntax->span, "%-group expects one child");
        return expand_macro_syntax_only(ctx, syntax->as.seq.items[1], out_syntax, err);
    }
    if (syn_is_form(syntax, "%-expr") && syntax->as.seq.count >= 2 && syntax->as.seq.items[1]->kind == IDM_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, syntax->as.seq.items[1], &payload, err)) {
            IdmSyntax *expanded = NULL;
            if (!invoke_macro_to_syntax(ctx, syntax, syntax->as.seq.items[1], payload, &expanded, err)) return false;
            ctx->surface_depth++;
            bool ok = expand_macro_syntax_only(ctx, expanded, out_syntax, err);
            ctx->surface_depth--;
            idm_syn_free(expanded);
            return ok;
        }
        if (err && err->present) return false;
    }
    *out_syntax = idm_syn_clone(syntax);
    return *out_syntax ? true : idm_error_oom(err, syntax ? syntax->span : idm_span_unknown(NULL));
}

bool local_expand_callback(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err) {
    (void)rt; return expand_macro_syntax_only((ExpandContext *)user, syntax, out_syntax, err);
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

static bool phase_syntax_callable_arity_one(const IdmCore *fn) {
    if (!fn) return false;
    if (fn->kind == IDM_CORE_FN) return fn->as.fn.arity == 1;
    if (fn->kind != IDM_CORE_FN_MULTI || fn->as.fn_multi.count == 0) return false;
    for (size_t i = 0; i < fn->as.fn_multi.count; i++) {
        if (fn->as.fn_multi.clauses[i].arity != 1) return false;
    }
    return true;
}

IdmSyntax *syntax_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    IdmSyntax *use = idm_syn_list(items[start]->span);
    if (!use || !idm_syn_append(use, idm_syn_word("%-expr", items[start]->span))) {
        idm_syn_free(use); idm_error_oom(err, items[start]->span); return NULL;
    }
    for (int phase = 0; phase < 2; phase++) {
        const IdmScopeSet *scopes = idm_syn_scope_set(items[start], phase);
        for (size_t i = 0; scopes && i < scopes->count; i++) {
            if (!idm_syn_scope_add(use, phase, scopes->items[i])) {
                idm_syn_free(use); idm_error_oom(err, items[start]->span); return NULL;
            }
        }
    }
    for (size_t i = start; i < end; i++) {
        IdmSyntax *item = idm_syn_clone(items[i]);
        if (!item || !idm_syn_append(use, item)) {
            idm_syn_free(item); idm_syn_free(use); idm_error_oom(err, items[i]->span); return NULL;
        }
    }
    if (!idm_syn_property_set(use, "value-context", ctx->value_context ? "true" : "false") ||
        !idm_syn_property_set(use, "command-sub", ctx->command_sub_context ? "true" : "false")) {
        idm_syn_free(use); idm_error_oom(err, items[start]->span); return NULL;
    }
    return use;
}

IdmCore *expand_macro_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t payload, IdmError *err) {
    IdmSyntax *use = syntax_use_from_parts(ctx, items, start, end, err);
    if (!use) return NULL;
    IdmCore *core = expand_macro_use(ctx, use, items[start], payload, err);
    idm_syn_free(use);
    return core;
}

IdmCore *expand_grammar_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, uint32_t grammar_index, IdmError *err) {
    IdmSyntax *use = syntax_use_from_parts(ctx, items, start, end, err);
    if (!use) return NULL;
    IdmSyntax *expanded_syntax = NULL;
    if (!invoke_grammar_to_syntax(ctx, use, items[start], grammar_index, &expanded_syntax, err)) {
        idm_syn_free(use);
        return NULL;
    }
    idm_syn_free(use);
    ctx->surface_depth++;
    IdmCore *expanded = expand_syntax(ctx, expanded_syntax, err);
    ctx->surface_depth--;
    idm_syn_free(expanded_syntax);
    return expanded;
}

static bool phase_syntax_fn_compile(ExpandContext *ctx, PhaseSyntaxFn *out, IdmCore *fn, const char *debug_name, IdmSpan span, IdmError *err) {
    out->phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    out->phase_env = idm_phase_env_retain(ctx->phase_env);
    out->module = idm_module_ref_create(ctx->rt);
    if (!out->module) return idm_error_oom(err, span);
    (void)debug_name;
    if (!idm_core_compile_function(fn, &out->module->module, &out->function_index, err) ||
        !idm_bc_intern_literals(ctx->rt, &out->module->module, err)) {
        phase_syntax_fn_destroy(out);
        return false;
    }
    return true;
}

static void phase_syntax_fn_import(ExpandContext *ctx, PhaseSyntaxFn *out, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env) {
    out->module = idm_module_ref_retain(module);
    out->function_index = function_index;
    out->phase_ns = phase_ns ? phase_ns : ctx->phase_ns;
    out->phase_env = idm_phase_env_retain(phase_env ? phase_env : ctx->phase_env);
}

static MacroDef *macro_slot(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err) {
    if (ctx->macro_count == ctx->macro_cap) {
        size_t cap = ctx->macro_cap ? ctx->macro_cap * 2u : 4u;
        MacroDef *macros = realloc(ctx->macros, cap * sizeof(*macros));
        if (!macros) { idm_error_oom(err, span); return NULL; }
        ctx->macros = macros;
        ctx->macro_cap = cap;
    }
    MacroDef *macro = &ctx->macros[ctx->macro_count];
    memset(macro, 0, sizeof(*macro));
    macro->name = idm_strdup(name);
    if (!macro->name) { idm_error_oom(err, span); return NULL; }
    return macro;
}

static bool bind_macro_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *name, uint32_t payload, IdmSpan span, IdmError *err) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return idm_error_oom(err, span);
    int bound = surface_bind_payload(ctx, ctx->unit, ctx->unit_key, name, name, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, &scopes, payload, span, err);
    idm_scope_set_destroy(&scopes);
    return bound >= 0;
}

bool register_macro(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, bool exported, IdmError *err) {
    const char *name = name_syntax && name_syntax->kind == IDM_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return idm_error_set(err, span, "defmacro name must be an identifier");
    if (!fn || (fn->kind != IDM_CORE_FN && fn->kind != IDM_CORE_FN_MULTI)) return idm_error_set(err, span, "defmacro must compile to a transformer function");
    if (!phase_syntax_callable_arity_one(fn)) return idm_error_set(err, span, "defmacro transformer must accept exactly one syntax argument");
    MacroDef *macro = macro_slot(ctx, name, span, err);
    if (!macro) return false;
    macro->exported = exported;
    if (!phase_syntax_fn_compile(ctx, &macro->fn, fn, name, span, err)) {
        macro_def_destroy(macro);
        return false;
    }
    uint32_t payload = (uint32_t)ctx->macro_count;
    if (!bind_macro_surface(ctx, name_syntax, name, payload, span, err)) {
        macro_def_destroy(macro);
        return false;
    }
    ctx->macro_count++;
    return true;
}

static bool register_grammar_named(ExpandContext *ctx, IdmCore *fn, IdmSpan span, const char *surface, IdmError *err) {
    const char *name = surface ? surface : "grammar";
    if (!fn || (fn->kind != IDM_CORE_FN && fn->kind != IDM_CORE_FN_MULTI)) return idm_error_set(err, span, "%s must compile to a transformer function", name);
    if (!phase_syntax_callable_arity_one(fn)) return idm_error_set(err, span, "%s transformer must accept exactly one syntax argument", name);
    size_t capture_count = fn->kind == IDM_CORE_FN ? fn->as.fn.capture_count : fn->as.fn_multi.capture_count;
    if (capture_count != 0) return idm_error_set(err, span, "%s transformer captures require phase environment support", name);
    if (ctx->decl_grammar) return idm_error_set(err, span, "only one %s may be declared per protocol", name);
    if (!phase_syntax_fn_compile(ctx, &ctx->decl_grammar_impl, fn, name, span, err)) return false;
    ctx->decl_grammar = true;
    return true;
}

bool register_grammar(ExpandContext *ctx, IdmCore *fn, IdmSpan span, IdmError *err) {
    return register_grammar_named(ctx, fn, span, "grammar", err);
}

bool register_macro_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, IdmValue transformer, IdmError *err) {
    ExpandContext *ctx = user;
    const char *name = name_syntax && name_syntax->kind == IDM_SYN_WORD ? name_syntax->as.text : NULL;
    if (!name) return idm_error_set(err, name_syntax ? name_syntax->span : idm_span_unknown(NULL), "macro name must be an identifier");
    const IdmBytecodeModule *closure_module = idm_closure_module(transformer);
    if (!closure_module) return idm_error_set(err, name_syntax->span, "macro transformer must be a module-backed function value");
    size_t entry_count = idm_closure_entry_count(transformer);
    if (entry_count == 0) {
        uint32_t fn_index = idm_closure_function_index(transformer);
        if (fn_index >= closure_module->function_count || closure_module->functions[fn_index].arity != 1) {
            return idm_error_set(err, name_syntax->span, "macro transformer must accept exactly one syntax argument");
        }
    } else {
        for (size_t i = 0; i < entry_count; i++) {
            uint32_t fn_index = idm_closure_entry(transformer, i, err);
            if (err && err->present) return false;
            if (fn_index >= closure_module->function_count || closure_module->functions[fn_index].arity != 1) {
                return idm_error_set(err, name_syntax->span, "macro transformer must accept exactly one syntax argument");
            }
        }
    }
    MacroDef *macro = macro_slot(ctx, name, name_syntax->span, err);
    if (!macro) return false;
    if (!idm_bc_intern_literals(rt, (IdmBytecodeModule *)closure_module, err)) {
        macro_def_destroy(macro);
        return false;
    }
    transformer = idm_value_copy(rt, &rt->immortal, transformer, err);
    if (err->present) {
        macro_def_destroy(macro);
        return false;
    }
    macro->fn.closure_backed = true;
    macro->fn.closure = transformer;
    macro->exported = false;
    macro->fn.phase_ns = ctx->phase_ns ? ctx->phase_ns : ctx->rt->main_ns;
    macro->fn.phase_env = idm_phase_env_retain(ctx->phase_env);
    uint32_t payload = (uint32_t)ctx->macro_count;
    if (!bind_macro_surface(ctx, name_syntax, name, payload, name_syntax->span, err)) {
        macro_def_destroy(macro);
        return false;
    }
    ctx->macro_count++;
    return true;
}

static bool install_imported_macro_impl(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, bool hidden, IdmError *err) {
    MacroDef *macro = macro_slot(ctx, name, idm_span_unknown(NULL), err);
    if (!macro) return false;
    phase_syntax_fn_import(ctx, &macro->fn, module, function_index, phase_ns, phase_env);
    macro->exported = false;
    macro->hidden = hidden;
    uint32_t payload = (uint32_t)ctx->macro_count;
    int bound = surface_bind_payload(ctx, provider, provider_key, name, name, IDM_BIND_SPACE_DEFAULT, IDM_BIND_TRANSFORMER, scopes ? scopes : &ctx->empty_scopes, payload, idm_span_unknown(NULL), err);
    if (bound <= 0) {
        macro_def_destroy(macro);
        return bound == 0;
    }
    ctx->macro_count++;
    return true;
}

bool install_imported_macro(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err) {
    return install_imported_macro_impl(ctx, name, scopes, module, function_index, phase_ns, phase_env, provider, provider_key, false, err);
}

bool install_imported_grammar(ExpandContext *ctx, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmNamespace *phase_ns, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err) {
    const IdmScopeSet *binding_scopes = scopes ? scopes : &ctx->empty_scopes;
    int guard = surface_install_guard(ctx, provider, provider_key, "grammar", "grammar", IDM_BIND_SPACE_GRAMMAR, binding_scopes, err);
    if (guard <= 0) return guard == 0;
    if (ctx->grammar_count == ctx->grammar_cap) {
        size_t cap = ctx->grammar_cap ? ctx->grammar_cap * 2u : 4u;
        GrammarDef *grammars = realloc(ctx->grammars, cap * sizeof(*grammars));
        if (!grammars) return idm_error_oom(err, idm_span_unknown(NULL));
        ctx->grammars = grammars;
        ctx->grammar_cap = cap;
    }
    GrammarDef *grammar = &ctx->grammars[ctx->grammar_count];
    memset(grammar, 0, sizeof(*grammar));
    if (!idm_scope_set_copy(&grammar->scopes, binding_scopes)) {
        idm_scope_set_destroy(&grammar->scopes);
        memset(grammar, 0, sizeof(*grammar));
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    phase_syntax_fn_import(ctx, &grammar->fn, module, function_index, phase_ns, phase_env);
    ctx->grammar_count++;
    return true;
}

bool install_imported_operator(ExpandContext *ctx, const IdmOperatorDef *op, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool ok = true;
    if (op->target_module) {
        ok = install_imported_macro_impl(ctx, op->target_name, &op->scopes, op->target_module, op->target_function_index, op->target_phase_ns, op->target_phase_env, provider, provider_key, true, err);
    }
    ok = ok && register_operator(ctx, op->name, op->capture ? op->capture : "infix", op->precedence, op->assoc, op->target_name, &op->scopes, binding_scopes ? binding_scopes : &ctx->empty_scopes, provider, provider_key, false, err);
    if (!ok) surface_rollback(ctx, &checkpoint);
    return ok;
}

static const char *operator_decl_token_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD) return syn->as.text;
    if (syn_is_form(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

bool register_operator_callback(void *user, IdmRuntime *rt, const IdmSyntax *name_syntax, int64_t precedence, const char *assoc_text, const char *capture_text, const IdmSyntax *target_syntax, IdmError *err) {
    (void)rt;
    ExpandContext *ctx = user;
    const char *name = operator_decl_token_text(name_syntax);
    if (!name) return idm_error_set(err, name_syntax->span, "operator name must be a symbol");
    if (!capture_text || !*capture_text) return idm_error_set(err, name_syntax->span, "operator capture must be non-empty");
    if (precedence < 0 || precedence > 255) return idm_error_set(err, name_syntax->span, "operator precedence must be an integer 0..255");
    IdmOpAssoc assoc;
    if (strcmp(assoc_text, "left") == 0) assoc = IDM_OP_ASSOC_LEFT;
    else if (strcmp(assoc_text, "right") == 0) assoc = IDM_OP_ASSOC_RIGHT;
    else if (strcmp(assoc_text, "none") == 0) assoc = IDM_OP_ASSOC_NONE;
    else return idm_error_set(err, name_syntax->span, "operator assoc must be left, right, or none");
    const char *target = operator_decl_token_text(target_syntax);
    if (!target) return idm_error_set(err, target_syntax->span, "operator target must be an identifier");
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
    bool ok = register_operator(ctx, name, capture_text, (uint8_t)precedence, assoc, target, &target_scopes, &decl_scopes, ctx->unit, ctx->unit_key, true, err);
    idm_scope_set_destroy(&decl_scopes);
    idm_scope_set_destroy(&target_scopes);
    return ok;
}
