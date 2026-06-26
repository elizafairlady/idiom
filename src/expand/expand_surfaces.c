#include "internal.h"

static bool expand_macro_syntax_only(ExpandContext *ctx, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err), bind_macro_surface(ExpandContext *ctx, const IdmSyntax *name_syntax, const char *name, uint32_t payload, IdmSpan span, IdmError *err), phase_syntax_fn_compile(ExpandContext *ctx, PhaseSyntaxFn *out, IdmCore *fn, const char *debug_name, IdmSpan span, IdmError *err);
static MacroDef *macro_slot(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err);
static void phase_syntax_fn_import(ExpandContext *ctx, PhaseSyntaxFn *out, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env);
static bool phase_syntax_callable_arity_one(const IdmCore *fn);

bool run_phase_core(ExpandContext *ctx, IdmCore *core, IdmError *err) {
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_bc_init(module);
    uint32_t main_fn = 0;
    if (!idm_core_compile_main(core, module, &main_fn, err) ||
        !idm_bc_intern_literals(ctx->rt, module, err)) {
        idm_bc_destroy(module); free(module); return false;
    }
    IdmValue ignored = idm_nil();
    IdmEnv *phase_runtime_env = ctx->phase_env ? ctx->phase_env->env : ctx->rt->main_env;
    bool ok = idm_vm_run_in_env(ctx->rt, module, main_fn, phase_runtime_env, &ignored, err);
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

bool resolve_head_core_syntax(ExpandContext *ctx, const IdmSyntax *head, const PhaseSyntaxFn **out_fn, IdmError *err) {
    if (!head || head->kind != IDM_SYN_WORD) return false;
    const IdmScopeSet *scopes = idm_syn_scope_set(head, 0);
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, head->as.text, IDM_BIND_SPACE_CORE_SYNTAX, scopes, NULL, &binding);
    if (status == IDM_RESOLVE_UNBOUND) status = resolve_scoped(ctx, "_", IDM_BIND_SPACE_CORE_SYNTAX, scopes, NULL, &binding);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, head->span, "ambiguous core syntax for '%s'", head->as.text);
        return false;
    }
    if (status != IDM_RESOLVE_OK || !binding || !binding->data) return false;
    *out_fn = (const PhaseSyntaxFn *)binding->data;
    return true;
}

static void core_syntax_def_clear_local(CoreSyntaxDef *def) {
    if (!def) return;
    free(def->name);
    idm_scope_set_destroy(&def->scopes);
    phase_syntax_fn_destroy(&def->fn);
    memset(def, 0, sizeof(*def));
}

static bool core_syntax_def_append(ExpandContext *ctx, CoreSyntaxDef **items, size_t *count, size_t *cap, const char *name, const IdmScopeSet *scopes, const PhaseSyntaxFn *fn, IdmError *err) {
    (void)ctx;
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        CoreSyntaxDef *next = realloc(*items, next_cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        *items = next;
        *cap = next_cap;
    }
    CoreSyntaxDef *def = &(*items)[*count];
    memset(def, 0, sizeof(*def));
    def->name = idm_strdup(name);
    if (!def->name) return idm_error_oom(err, idm_span_unknown(NULL));
    if (scopes && !idm_scope_set_copy(&def->scopes, scopes)) {
        core_syntax_def_clear_local(def);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (fn) {
        def->fn.module = idm_module_ref_retain(fn->module);
        def->fn.function_index = fn->function_index;
        def->fn.phase_env = idm_phase_env_retain(fn->phase_env);
        def->fn.closure_backed = fn->closure_backed;
        def->fn.closure = fn->closure;
    }
    (*count)++;
    return true;
}

static bool phase_syntax_call(IdmRuntime *rt, const IdmSyntax *use_syntax, const PhaseSyntaxFn *fn, IdmSyntax **out_syntax, IdmError *err) {
    if (!fn->closure_backed && !fn->module) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "phase syntax function module is not available");
    IdmValue arg = idm_syntax_value(rt, use_syntax, err);
    if (err && err->present) return false;
    IdmValue result = idm_nil();
    IdmEnv *call_env = fn->phase_env ? fn->phase_env->env : rt->main_env;
    IdmValue callee = fn->closure_backed
        ? fn->closure
        : idm_closure_in_module(rt, &fn->module->module, fn->function_index, NULL, 0, call_env, err);
    if (err && err->present) return false;
    bool ok = idm_vm_call_closure(rt, callee, &arg, 1, &result, err);
    if (!ok) return false;
    const IdmSyntax *result_syntax = idm_syntax_get(result, err);
    if (!result_syntax) return false;
    *out_syntax = idm_syn_clone(result_syntax);
    return *out_syntax ? true : idm_error_oom(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL));
}

bool local_macro_invoke(void *user, IdmRuntime *rt, uint32_t payload, const IdmSyntax *use_syntax, IdmSyntax **out_syntax, IdmError *err) {
    ExpandContext *ctx = user; if (payload >= ctx->macro_count) return idm_error_set(err, use_syntax ? use_syntax->span : idm_span_unknown(NULL), "macro transformer payload %u is out of bounds", payload);
    return phase_syntax_call(rt, use_syntax, &ctx->macros[payload].fn, out_syntax, err); }

typedef bool (*SyntaxSurfaceInvokeFn)(ExpandContext *ctx, const void *payload, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err);

static bool invoke_macro_payload(ExpandContext *ctx, const void *payload, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err) {
    uint32_t idx = (uint32_t)(uintptr_t)payload;
    IdmMacroRunner *runner = idx < ctx->macro_count ? &ctx->local_runner : ctx->runner;
    if (!runner || !runner->invoke) return idm_error_set(err, head->span, "macro runner is not available for transformer '%s'", head->as.text);
    return runner->invoke(runner->user, ctx->rt, idx, use_copy, out_syntax, err);
}

static bool invoke_core_syntax_payload(ExpandContext *ctx, const void *payload, const IdmSyntax *use_copy, const IdmSyntax *head, IdmSyntax **out_syntax, IdmError *err) { (void)head; return phase_syntax_call(ctx->rt, use_copy, (const PhaseSyntaxFn *)payload, out_syntax, err); }

static bool invoke_scoped_syntax_surface_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, const void *payload, const char *depth_action, const char *note_action, SyntaxSurfaceInvokeFn invoke, IdmSyntax **out_syntax, IdmError *err) {
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

bool invoke_macro_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, uint32_t payload, IdmSyntax **out_syntax, IdmError *err) { return invoke_scoped_syntax_surface_to_syntax(ctx, use_syntax, head, (const void *)(uintptr_t)payload, "macro expansion depth exceeded while expanding", "in expansion of", invoke_macro_payload, out_syntax, err); }

static bool invoke_core_syntax_to_syntax(ExpandContext *ctx, const IdmSyntax *use_syntax, const IdmSyntax *head, const PhaseSyntaxFn *fn, IdmSyntax **out_syntax, IdmError *err) { return invoke_scoped_syntax_surface_to_syntax(ctx, use_syntax, head, fn, "core syntax expansion depth exceeded while expanding", "in core syntax expansion of", invoke_core_syntax_payload, out_syntax, err); }

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
        const IdmFnClause *clause = &fn->as.fn_multi.clauses[i];
        if (clause->primitive_backed) {
            IdmArity one = idm_arity_exact(1u);
            if (!idm_arity_equal(&clause->call_arity, &one)) return false;
        } else if (clause->arity != 1) {
            return false;
        }
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

IdmCore *expand_core_syntax_use_from_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, const PhaseSyntaxFn *fn, IdmError *err) {
    IdmSyntax *use = syntax_use_from_parts(ctx, items, start, end, err);
    if (!use) return NULL;
    IdmSyntax *expanded_syntax = NULL;
    if (!invoke_core_syntax_to_syntax(ctx, use, items[start], fn, &expanded_syntax, err)) {
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

static bool materialize_phase_runtime_inits(ExpandContext *ctx, IdmSpan span, IdmError *err) {
    if (!ctx->phase_env || ctx->phase_runtime_init_mark_count >= ctx->runtime_init_mark_count) return true;
    size_t first_mark = ctx->phase_runtime_init_mark_count;
    IdmCore *body = idm_core_literal(idm_nil(), span);
    if (!body) return idm_error_oom(err, span);
    int saved_phase = ctx->phase;
    ctx->phase = 1;
    IdmCore *wrapped = wrap_phase_runtime_inits_since(ctx, body, first_mark, err);
    bool ok = wrapped != NULL && run_phase_core(ctx, wrapped, err);
    ctx->phase = saved_phase;
    idm_core_free(wrapped);
    if (ok) ctx->phase_runtime_init_mark_count = ctx->runtime_init_mark_count;
    return ok;
}

static bool phase_syntax_fn_compile(ExpandContext *ctx, PhaseSyntaxFn *out, IdmCore *fn, const char *debug_name, IdmSpan span, IdmError *err) {
    if (!materialize_phase_runtime_inits(ctx, span, err)) return false;
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

static void phase_syntax_fn_import(ExpandContext *ctx, PhaseSyntaxFn *out, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env) {
    out->module = idm_module_ref_retain(module);
    out->function_index = function_index;
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

static bool register_core_syntax_named(ExpandContext *ctx, const char *name, IdmCore *fn, IdmSpan span, IdmError *err) {
    if (!name || !*name) return idm_error_set(err, span, "core-syntax name must be non-empty");
    if (!fn || (fn->kind != IDM_CORE_FN && fn->kind != IDM_CORE_FN_MULTI)) return idm_error_set(err, span, "core-syntax %s must compile to a transformer function", name);
    if (!phase_syntax_callable_arity_one(fn)) return idm_error_set(err, span, "core-syntax %s transformer must accept exactly one syntax argument", name);
    size_t capture_count = fn->kind == IDM_CORE_FN ? fn->as.fn.capture_count : fn->as.fn_multi.capture_count;
    if (capture_count != 0) return idm_error_set(err, span, "core-syntax %s transformer captures require phase environment support", name);
    for (size_t i = 0; i < ctx->decl_core_syntax_count; i++) {
        if (ctx->decl_core_syntax[i].name && strcmp(ctx->decl_core_syntax[i].name, name) == 0) {
            return idm_error_set(err, span, "core-syntax %s is already declared in this protocol", name);
        }
    }
    PhaseSyntaxFn compiled;
    memset(&compiled, 0, sizeof(compiled));
    if (!phase_syntax_fn_compile(ctx, &compiled, fn, name, span, err)) return false;
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    bool ok = core_syntax_def_append(ctx, &ctx->decl_core_syntax, &ctx->decl_core_syntax_count, &ctx->decl_core_syntax_cap, name, &scopes, &compiled, err);
    idm_scope_set_destroy(&scopes);
    phase_syntax_fn_destroy(&compiled);
    return ok;
}

bool register_core_syntax(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, IdmError *err) {
    if (!name_syntax || name_syntax->kind != IDM_SYN_WORD) return idm_error_set(err, span, "core-syntax expects a head name");
    return register_core_syntax_named(ctx, name_syntax->as.text, fn, span, err);
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
    if (!idm_bc_is_finalized(closure_module)) { macro_def_destroy(macro); return idm_error_set(err, name_syntax->span, "macro transformer module is not finalized"); }
    transformer = idm_value_copy(rt, &rt->immortal, transformer, err);
    if (err->present) {
        macro_def_destroy(macro);
        return false;
    }
    macro->fn.closure_backed = true;
    macro->fn.closure = transformer;
    macro->exported = false;
    macro->fn.phase_env = idm_phase_env_retain(ctx->phase_env);
    uint32_t payload = (uint32_t)ctx->macro_count;
    if (!bind_macro_surface(ctx, name_syntax, name, payload, name_syntax->span, err)) {
        macro_def_destroy(macro);
        return false;
    }
    ctx->macro_count++;
    return true;
}

static bool install_imported_macro_impl(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, bool hidden, IdmError *err) {
    MacroDef *macro = macro_slot(ctx, name, idm_span_unknown(NULL), err);
    if (!macro) return false;
    phase_syntax_fn_import(ctx, &macro->fn, module, function_index, phase_env);
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

bool install_imported_macro(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err) {
    return install_imported_macro_impl(ctx, name, scopes, module, function_index, phase_env, provider, provider_key, false, err);
}

bool install_imported_core_syntax(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, IdmModuleRef *module, uint32_t function_index, IdmPhaseEnv *phase_env, const char *provider, const char *provider_key, IdmError *err) {
    if (!name || !*name) return idm_error_set(err, idm_span_unknown(NULL), "core-syntax import requires a name");
    const IdmScopeSet *binding_scopes = scopes ? scopes : &ctx->empty_scopes;
    PhaseSyntaxFn *imported = calloc(1u, sizeof(*imported));
    if (!imported) return idm_error_oom(err, idm_span_unknown(NULL));
    phase_syntax_fn_import(ctx, imported, module, function_index, phase_env);
    int bound = surface_bind_data(ctx, provider, provider_key, name, name, IDM_BIND_SPACE_CORE_SYNTAX, IDM_BIND_CORE_FORM, binding_scopes, imported, idm_span_unknown(NULL), err);
    if (bound <= 0) {
        phase_syntax_fn_destroy(imported);
        free(imported);
        return bound == 0;
    }
    return true;
}

static bool grammar_terminal_copy(IdmGrammarTerminal *dst, const IdmGrammarTerminal *src, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->flags = src->flags;
    if (src->text) {
        dst->text = idm_strdup(src->text);
        if (!dst->text) {
            idm_grammar_terminal_destroy(dst);
            return idm_error_oom(err, span);
        }
    }
    return true;
}

static bool grammar_rule_copy_relocated(IdmGrammarRule *dst, const IdmGrammarRule *src, IdmScopeId min_id, int64_t delta, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->name = idm_strdup(src->name);
    dst->kind = src->kind;
    if (!grammar_terminal_copy(&dst->terminal, &src->terminal, err, span)) {
        idm_grammar_rule_destroy(dst);
        return false;
    }
    if (!idm_reader_pattern_program_copy(&dst->pattern, &src->pattern, err, span) ||
        !idm_reader_ctor_program_copy(&dst->constructor, &src->constructor, err, span)) {
        idm_grammar_rule_destroy(dst);
        return false;
    }
    if (!dst->name) {
        idm_grammar_rule_destroy(dst);
        return idm_error_oom(err, span);
    }
    idm_reader_pattern_program_relocate(&dst->pattern, min_id, delta);
    idm_reader_ctor_program_relocate(&dst->constructor, min_id, delta);
    return true;
}

static bool grammar_rules_copy_relocated(IdmGrammarRule **out, const IdmGrammarRule *src, size_t count, IdmScopeId min_id, int64_t delta, IdmError *err, IdmSpan span) {
    *out = NULL;
    if (count == 0) return true;
    IdmGrammarRule *rules = calloc(count, sizeof(*rules));
    if (!rules) return idm_error_oom(err, span);
    for (size_t i = 0; i < count; i++) {
        if (!grammar_rule_copy_relocated(&rules[i], &src[i], min_id, delta, err, span)) {
            for (size_t j = 0; j <= i; j++) idm_grammar_rule_destroy(&rules[j]);
            free(rules);
            return false;
        }
    }
    *out = rules;
    return true;
}

static int grammar_install_check(ExpandContext *ctx, const char *name, const IdmPkgGrammar *incoming, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err) {
    for (size_t i = 0; i < ctx->grammar_count; i++) {
        GrammarDef *existing = &ctx->grammars[i];
        if (!existing->artifact.name || strcmp(existing->artifact.name, name) != 0 || !idm_scope_set_equal(&existing->binding_scopes, binding_scopes)) continue;
        bool same_provider = existing->provider && provider && strcmp(existing->provider, provider) == 0 &&
                             existing->provider_key && provider_key && strcmp(existing->provider_key, provider_key) == 0;
        if (same_provider) return 0;
        if (existing->artifact.mode == IDM_GRAMMAR_MODE_EXTEND && incoming->mode == IDM_GRAMMAR_MODE_EXTEND) {
            for (size_t r = 0; r < incoming->rule_count; r++) {
                for (size_t e = 0; e < existing->artifact.rule_count; e++) {
                    if (strcmp(incoming->rules[r].name, existing->artifact.rules[e].name) == 0) {
                        idm_error_set(err, idm_span_unknown(NULL), "grammar surface '%s' rule '%s' from '%s' conflicts with active provider '%s'", name, incoming->rules[r].name, provider ? provider : "<unknown>", existing->provider ? existing->provider : "<unknown>");
                        return -1;
                    }
                }
            }
            continue;
        }
        idm_error_set(err, idm_span_unknown(NULL), "grammar surface '%s' from '%s' is already active in this context; activating '%s' would conflict", name, existing->provider ? existing->provider : "<unknown>", provider ? provider : "<unknown>");
        return -1;
    }
    return 1;
}

static GrammarDef *grammar_slot(ExpandContext *ctx, IdmSpan span, IdmError *err) {
    if (ctx->grammar_count == ctx->grammar_cap) {
        size_t cap = ctx->grammar_cap ? ctx->grammar_cap * 2u : 4u;
        GrammarDef *grammars = realloc(ctx->grammars, cap * sizeof(*grammars));
        if (!grammars) {
            idm_error_oom(err, span);
            return NULL;
        }
        ctx->grammars = grammars;
        ctx->grammar_cap = cap;
    }
    GrammarDef *grammar = &ctx->grammars[ctx->grammar_count];
    memset(grammar, 0, sizeof(*grammar));
    return grammar;
}

static bool grammar_bind(ExpandContext *ctx, const char *name, const IdmScopeSet *binding_scopes, uint32_t payload, IdmSpan span, IdmError *err) {
    if (!idm_binding_table_add(&ctx->bindings, name, 0, IDM_BIND_SPACE_GRAMMAR, IDM_BIND_GRAMMAR, binding_scopes, payload, ctx->frame, NULL)) {
        return idm_error_oom(err, span);
    }
    return true;
}

bool register_grammar(ExpandContext *ctx, const IdmSyntax *name_syntax, uint8_t mode, IdmGrammarRule *rules, size_t rule_count, bool exported, IdmError *err) {
    if (!name_syntax || name_syntax->kind != IDM_SYN_WORD) return idm_error_set(err, name_syntax ? name_syntax->span : idm_span_unknown(NULL), "grammar expects a name");
    if (mode > (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) return idm_error_set(err, name_syntax->span, "invalid grammar mode");
    if (rule_count == 0) return idm_error_set(err, name_syntax->span, "grammar '%s' must contain at least one rule", name_syntax->as.text);
    if (ctx->phase != 0) {
        for (size_t i = 0; i < rule_count; i++) idm_grammar_rule_destroy(&rules[i]);
        free(rules);
        return idm_error_set(err, name_syntax->span, "core-grammar declarations are phase-0 reader artifact declarations");
    }
    for (size_t i = 0; i < rule_count; i++) {
        if (!idm_grammar_rule_validate(&rules[i], err, name_syntax->span)) {
            for (size_t j = 0; j < rule_count; j++) idm_grammar_rule_destroy(&rules[j]);
            free(rules);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(rules[i].name, rules[j].name) == 0) {
                bool reported = idm_error_set(err, name_syntax->span, "grammar '%s' declares rule '%s' more than once", name_syntax->as.text, rules[i].name);
                for (size_t k = 0; k < rule_count; k++) idm_grammar_rule_destroy(&rules[k]);
                free(rules);
                return reported;
            }
        }
    }
    IdmScopeSet decl_scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &decl_scopes)) {
        for (size_t i = 0; i < rule_count; i++) idm_grammar_rule_destroy(&rules[i]);
        free(rules);
        return idm_error_oom(err, name_syntax->span);
    }
    for (size_t i = 0; i < ctx->decl_grammar_count; i++) {
        IdmPkgGrammar *existing = &ctx->decl_grammars[i];
        if (!existing->name || strcmp(existing->name, name_syntax->as.text) != 0) continue;
        if (!idm_scope_set_equal(&existing->scopes, &decl_scopes)) {
            idm_scope_set_destroy(&decl_scopes);
            for (size_t k = 0; k < rule_count; k++) idm_grammar_rule_destroy(&rules[k]);
            free(rules);
            return idm_error_set(err, name_syntax->span, "grammar surface '%s' is already declared with different scopes in this artifact", name_syntax->as.text);
        }
        if (existing->mode != IDM_GRAMMAR_MODE_EXTEND || mode != IDM_GRAMMAR_MODE_EXTEND) {
            idm_scope_set_destroy(&decl_scopes);
            for (size_t k = 0; k < rule_count; k++) idm_grammar_rule_destroy(&rules[k]);
            free(rules);
            return idm_error_set(err, name_syntax->span, "grammar surface '%s' is already declared in this artifact", name_syntax->as.text);
        }
        for (size_t r = 0; r < rule_count; r++) {
            for (size_t e = 0; e < existing->rule_count; e++) {
                if (strcmp(rules[r].name, existing->rules[e].name) == 0) {
                    bool reported = idm_error_set(err, name_syntax->span, "grammar '%s' declares rule '%s' more than once", name_syntax->as.text, rules[r].name);
                    idm_scope_set_destroy(&decl_scopes);
                    for (size_t k = 0; k < rule_count; k++) idm_grammar_rule_destroy(&rules[k]);
                    free(rules);
                    return reported;
                }
            }
        }
        IdmGrammarRule *merged = realloc(existing->rules, (existing->rule_count + rule_count) * sizeof(*merged));
        if (!merged) {
            idm_scope_set_destroy(&decl_scopes);
            for (size_t k = 0; k < rule_count; k++) idm_grammar_rule_destroy(&rules[k]);
            free(rules);
            return idm_error_oom(err, name_syntax->span);
        }
        existing->rules = merged;
        memcpy(existing->rules + existing->rule_count, rules, rule_count * sizeof(*rules));
        existing->rule_count += rule_count;
        existing->exported = existing->exported || exported;
        idm_scope_set_destroy(&decl_scopes);
        free(rules);
        return true;
    }
    if (ctx->decl_grammar_count == ctx->decl_grammar_cap) {
        size_t cap = ctx->decl_grammar_cap ? ctx->decl_grammar_cap * 2u : 4u;
        IdmPkgGrammar *grammars = realloc(ctx->decl_grammars, cap * sizeof(*grammars));
        if (!grammars) {
            idm_scope_set_destroy(&decl_scopes);
            for (size_t i = 0; i < rule_count; i++) idm_grammar_rule_destroy(&rules[i]);
            free(rules);
            return idm_error_oom(err, name_syntax->span);
        }
        ctx->decl_grammars = grammars;
        ctx->decl_grammar_cap = cap;
    }
    IdmPkgGrammar *dst = &ctx->decl_grammars[ctx->decl_grammar_count];
    memset(dst, 0, sizeof(*dst));
    dst->name = idm_strdup(name_syntax->as.text);
    dst->mode = mode;
    dst->rules = rules;
    dst->rule_count = rule_count;
    dst->exported = exported;
    bool ok = dst->name && idm_scope_set_copy(&dst->scopes, &decl_scopes);
    idm_scope_set_destroy(&decl_scopes);
    if (!ok) {
        idm_pkg_grammar_destroy(dst);
        return idm_error_oom(err, name_syntax->span);
    }
    ctx->decl_grammar_count++;
    return true;
}

bool install_imported_grammar(ExpandContext *ctx, const IdmPkgGrammar *grammar, const IdmScopeSet *scopes, const char *name, const char *provider, const char *provider_key, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (!grammar || !grammar->name || !*grammar->name) return idm_error_set(err, idm_span_unknown(NULL), "grammar import requires a name");
    if (ctx->phase != 0) return idm_error_set(err, idm_span_unknown(NULL), "grammar artifacts can only be activated at phase 0");
    if (!name || !*name) name = grammar->name;
    const IdmScopeSet *binding_scopes = scopes ? scopes : &ctx->empty_scopes;
    int check = grammar_install_check(ctx, name, grammar, binding_scopes, provider, provider_key, err);
    if (check <= 0) return check == 0;
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    GrammarDef *dst = grammar_slot(ctx, idm_span_unknown(NULL), err);
    if (!dst) return false;
    dst->artifact.name = idm_strdup(name);
    dst->artifact.mode = grammar->mode;
    dst->artifact.exported = false;
    dst->provider = idm_strdup(provider ? provider : "");
    dst->provider_key = idm_strdup(provider_key ? provider_key : "");
    bool ok = dst->artifact.name && dst->provider && dst->provider_key &&
              idm_scope_set_copy(&dst->artifact.scopes, &grammar->scopes) &&
              idm_scope_set_copy(&dst->binding_scopes, binding_scopes);
    if (ok) idm_scope_set_relocate(&dst->artifact.scopes, min_id, delta);
    if (ok) ok = grammar_rules_copy_relocated(&dst->artifact.rules, grammar->rules, grammar->rule_count, min_id, delta, err, idm_span_unknown(NULL));
    if (ok) dst->artifact.rule_count = grammar->rule_count;
    uint32_t payload = (uint32_t)ctx->grammar_count;
    ok = ok && grammar_bind(ctx, dst->artifact.name, binding_scopes, payload, idm_span_unknown(NULL), err);
    if (!ok) {
        surface_rollback(ctx, &checkpoint);
        if (err && err->present) return false;
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ctx->grammar_count++;
    return true;
}

bool install_artifact_grammars(ExpandContext *ctx, const IdmPkgGrammar *grammars, size_t grammar_count, const IdmScopeSet *scopes, const char *qualifier, IdmScopeId min_id, int64_t delta, const char *provider, const char *provider_key, IdmError *err) {
    for (size_t i = 0; i < grammar_count; i++) {
        const IdmPkgGrammar *entry = &grammars[i];
        char *qualified = NULL;
        const char *bind_name = entry->name;
        if (qualifier) {
            IdmBuffer qb;
            idm_buf_init(&qb);
            if (!idm_buf_append(&qb, qualifier) || !idm_buf_append_char(&qb, '.') || !idm_buf_append(&qb, entry->name)) {
                idm_buf_destroy(&qb);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            qualified = idm_buf_take(&qb);
            bind_name = qualified;
        }
        bool ok = install_imported_grammar(ctx, entry, scopes, bind_name, provider, provider_key, min_id, delta, err);
        free(qualified);
        if (!ok) return false;
    }
    return true;
}

bool ctx_reader_artifact_from_active_grammars(ExpandContext *ctx, const char *surface, IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    if (!surface || surface[0] == '\0') return idm_error_set(err, idm_span_unknown(NULL), "reader artifact requires a surface name");
    const IdmScopeSet *surface_scopes = NULL;
    size_t count = 0;
    for (size_t i = 0; i < ctx->grammar_count; i++) {
        const GrammarDef *entry = &ctx->grammars[i];
        if (!entry->artifact.name || strcmp(entry->artifact.name, surface) != 0) continue;
        if (!surface_scopes) surface_scopes = &entry->binding_scopes;
        else if (!idm_scope_set_equal(surface_scopes, &entry->binding_scopes)) {
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' is active in multiple binding scopes", surface);
        }
        count++;
    }
    if (count == 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no active grammar contributors", surface);
    IdmReaderGrammarSource *sources = calloc(count, sizeof(*sources));
    if (!sources) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t index = 0;
    for (size_t i = 0; i < ctx->grammar_count; i++) {
        const GrammarDef *entry = &ctx->grammars[i];
        if (!entry->artifact.name || strcmp(entry->artifact.name, surface) != 0) continue;
        sources[index].grammar = &entry->artifact;
        sources[index].provider = entry->provider;
        sources[index].provider_key = entry->provider_key;
        sources[index].binding_scopes = &entry->binding_scopes;
        index++;
    }
    bool ok = idm_reader_artifact_from_sources(surface, sources, count, out, err);
    free(sources);
    return ok;
}

bool install_imported_operator(ExpandContext *ctx, const IdmOperatorDef *op, const IdmScopeSet *binding_scopes, const char *provider, const char *provider_key, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    bool ok = true;
    if (op->target_module) {
        ok = install_imported_macro_impl(ctx, op->target_name, &op->scopes, op->target_module, op->target_function_index, op->target_phase_env, provider, provider_key, true, err);
    }
    ok = ok && register_operator(ctx, op->name, op->capture ? op->capture : "infix", op->precedence, op->assoc, op->target_name, &op->scopes, binding_scopes ? binding_scopes : &ctx->empty_scopes, provider, provider_key, false, err);
    if (!ok) surface_rollback(ctx, &checkpoint);
    return ok;
}
