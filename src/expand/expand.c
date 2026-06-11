#include "internal.h"

static IshPrimitive primitive_from_binding(const IshBinding *binding);
static IshCore *expand_word_ref_mode(ExpandContext *ctx, const IshSyntax *word, bool callee_position, IshError *err);
static IshCore *expand_word_ref(ExpandContext *ctx, const IshSyntax *word, IshError *err);
static IshCore *expand_word_callee(ExpandContext *ctx, const IshSyntax *word, IshError *err);
static bool head_is_bound(ExpandContext *ctx, const IshSyntax *word);
static IshCore *expand_raise_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
static const IshSyntax *try_section_head(const IshSyntax *stmt);
static IshCore *expand_try_handler(ExpandContext *ctx, const IshSyntax *stmt, IshError *err);
static IshCore *expand_try_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
static bool syn_is_dot(const IshSyntax *s);
static IshSyntax *make_qualified_word(IshSyntax *const *items, size_t start, size_t *inout_end, IshError *err);
static bool qualified_word_resolves(ExpandContext *ctx, const IshSyntax *word);
static IshCore *expand_method_surface_call(ExpandContext *ctx, const MethodSurfaceDef *method, IshCore *receiver, IshSyntax *const *items, size_t arg_start, size_t end, IshError *err);
static IshCore *expand_parts_inner(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
static bool enforest_at_end_or_operator(EnforestParser *parser);
static IshCore *parse_enforest_primary(EnforestParser *parser);
static bool core_app_prepend_arg(IshCore *app, IshCore *arg);
static IshCore *operator_callee(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshSpan span, IshError *err);
static IshCore *make_operator_app(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshCore *left, IshCore *right, IshSpan span, IshError *err);
static IshCore *make_operator_unary(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshCore *operand, IshSpan span, IshError *err);
static IshCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec);
static IshCore *expand_protocol_expr(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_protocol_body(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_protocol_group(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_protocol_expression(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_protocol_syntax_quote(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_program(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_container(ExpandContext *ctx, const IshSyntax *syn, IshError *err);

static IshPrimitive primitive_from_binding(const IshBinding *binding) {
    return (IshPrimitive)binding->payload;
}

static void note_unbound_context(ExpandContext *ctx, const IshSyntax *word, IshError *err) {
    if (!err || !err->present) return;
    for (size_t i = 0; i < word->origins.count; i++) {
        ish_error_note(err, "in expansion of '%s'", word->origins.items[i]);
    }
    size_t hidden = 0;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        if (ctx->bindings.items[i].phase == 0 && ctx->bindings.items[i].space == ISH_BIND_SPACE_DEFAULT &&
            strcmp(ctx->bindings.items[i].name, word->as.text) == 0) {
            hidden++;
        }
    }
    if (hidden != 0) {
        ish_error_note(err, "%zu binding%s named '%s' exist%s but %s not visible here (hygiene scopes differ)",
                       hidden, hidden == 1 ? "" : "s", word->as.text, hidden == 1 ? "s" : "", hidden == 1 ? "is" : "are");
    }
}

static IshCore *expand_word_ref_mode(ExpandContext *ctx, const IshSyntax *word, bool callee_position, IshError *err) {
    uint32_t cap = 0;
    if (capture_lookup_existing(ctx->captures, ctx->capture_count, word, &cap)) return ish_core_capture_ref(cap, word->span);
    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    const IshBinding *binding = resolve_default(ctx, word, &status);
    if (status == ISH_RESOLVE_OK) {
        if (binding->kind == ISH_BIND_LOCAL) {
            if (binding->frame_id == ctx->frame) return ish_core_local_ref(binding->payload, word->span);
            if (materialize_capture(ctx, word, binding, &cap)) return ish_core_capture_ref(cap, word->span);
            return (IshCore *)(uintptr_t)ish_error_oom(err, word->span);
        }
        if (binding->kind == ISH_BIND_ARG) {
            if (binding->frame_id == ctx->frame) return ish_core_arg_ref(binding->payload, word->span);
            if (materialize_capture(ctx, word, binding, &cap)) return ish_core_capture_ref(cap, word->span);
            return (IshCore *)(uintptr_t)ish_error_oom(err, word->span);
        }
        if (binding->kind == ISH_BIND_GLOBAL) return ish_core_global_ref(binding->payload, word->span);
        if (binding->kind == ISH_BIND_METHOD) return expand_error(err, word->span, "method '%s' requires a receiver", word->as.text);
        if (binding->kind == ISH_BIND_VALUE) {
            IshPrimitive prim = primitive_from_binding(binding);
            const IshPrimitiveInfo *info = ish_primitive_info(prim);
            if (!callee_position && info && info->min_arity == 0) {
                IshCore *callee = ish_core_primitive(prim, word->span);
                if (!callee) return (IshCore *)(uintptr_t)ish_error_oom(err, word->span);
                IshCore *app = ish_core_app(callee, word->span);
                if (!app) {
                    ish_core_free(callee);
                    return (IshCore *)(uintptr_t)ish_error_oom(err, word->span);
                }
                return app;
            }
            return ish_core_primitive(prim, word->span);
        }
    }
    if (status == ISH_RESOLVE_AMBIGUOUS) {
        expand_error(err, word->span, "ambiguous identifier '%s'", word->as.text);
        size_t candidates = 0;
        for (size_t i = 0; i < ctx->bindings.count; i++) {
            if (ctx->bindings.items[i].phase == 0 && ctx->bindings.items[i].space == ISH_BIND_SPACE_DEFAULT &&
                strcmp(ctx->bindings.items[i].name, word->as.text) == 0 &&
                scopes_subset_for_ref(&ctx->bindings.items[i].scopes, word)) {
                candidates++;
            }
        }
        if (candidates != 0) ish_error_note(err, "%zu candidate bindings named '%s' are visible here with incomparable scopes", candidates, word->as.text);
        for (size_t i = 0; i < word->origins.count; i++) ish_error_note(err, "in expansion of '%s'", word->origins.items[i]);
        return NULL;
    }
    expand_error(err, word->span, "unbound identifier '%s'", word->as.text);
    note_unbound_context(ctx, word, err);
    return NULL;
}

static IshCore *expand_word_ref(ExpandContext *ctx, const IshSyntax *word, IshError *err) {
    return expand_word_ref_mode(ctx, word, false, err);
}

static IshCore *expand_word_callee(ExpandContext *ctx, const IshSyntax *word, IshError *err) {
    return expand_word_ref_mode(ctx, word, true, err);
}

static bool head_is_bound(ExpandContext *ctx, const IshSyntax *word) {
    uint32_t cap = 0;
    if (capture_lookup_existing(ctx->captures, ctx->capture_count, word, &cap)) return true;
    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    (void)resolve_default(ctx, word, &status);
    return status == ISH_RESOLVE_OK;
}

static IshCore *expand_raise_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (start + 1u >= end) return expand_error(err, items[start]->span, "raise requires a value");
    IshCore *value = expand_parts(ctx, items, start + 1u, end, err);
    if (!value) return NULL;
    IshCore *raise = ish_core_raise(value, items[start]->span);
    if (!raise) { ish_core_free(value); return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span); }
    return raise;
}

static const IshSyntax *try_section_head(const IshSyntax *stmt) {
    if (!syn_is_protocol(stmt, "%-expr") || stmt->as.seq.count < 2) return NULL;
    const IshSyntax *head = stmt->as.seq.items[1];
    return head && head->kind == ISH_SYN_WORD ? head : NULL;
}

static IshCore *expand_try_handler(ExpandContext *ctx, const IshSyntax *stmt, IshError *err) {
    IshSyntax *const *ritems = stmt->as.seq.items;
    size_t rcount = stmt->as.seq.count;
    if (rcount < 5) return expand_error(err, stmt->span, "rescue requires the form: rescue NAME -> HANDLER");
    const IshSyntax *binder = ritems[2];
    if (binder->kind != ISH_SYN_WORD) return expand_error(err, binder->span, "rescue binder must be an identifier");
    size_t arrow = 0;
    for (size_t i = 3; i < rcount; i++) {
        if (syn_is_word(ritems[i], "->")) { arrow = i; break; }
    }
    if (arrow == 0 || arrow + 1u >= rcount) return expand_error(err, stmt->span, "rescue requires '-> HANDLER'");
    size_t saved_count = ctx->bindings.count;
    uint32_t r_slot = 0;
    if (!local_push_scoped(ctx, binder->as.text, binder, &r_slot)) return (IshCore *)(uintptr_t)ish_error_oom(err, stmt->span);
    IshCore *handler_body = expand_parts(ctx, ritems, arrow + 1u, rcount, err);
    local_pop_to(ctx, saved_count, ctx->next_slot);
    if (!handler_body) return NULL;
    IshCore *raised = ish_core_raised(stmt->span);
    if (!raised) { ish_core_free(handler_body); return (IshCore *)(uintptr_t)ish_error_oom(err, stmt->span); }
    IshCore *bind = ish_core_bind_local(r_slot, raised, handler_body, stmt->span);
    if (!bind) { ish_core_free(raised); ish_core_free(handler_body); return (IshCore *)(uintptr_t)ish_error_oom(err, stmt->span); }
    return bind;
}

static IshCore *expand_try_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (end - start != 2u || !syn_is_protocol(items[start + 1u], "%-body")) return expand_error(err, items[start]->span, "try requires a do/end body");
    const IshSyntax *body_syn = items[start + 1u];
    IshSyntax *const *stmts = body_syn->as.seq.items;
    size_t scount = body_syn->as.seq.count;
    size_t rescue_pos = 0;
    size_t ensure_pos = 0;
    for (size_t i = 1; i < scount; i++) {
        const IshSyntax *head = try_section_head(stmts[i]);
        if (!head) continue;
        if (strcmp(head->as.text, "rescue") == 0 && rescue_pos == 0) rescue_pos = i;
        else if (strcmp(head->as.text, "ensure") == 0 && ensure_pos == 0) ensure_pos = i;
    }
    bool has_rescue = rescue_pos != 0;
    bool has_ensure = ensure_pos != 0;
    if (!has_rescue && !has_ensure) return expand_error(err, items[start]->span, "try requires a rescue or ensure clause");
    if (has_rescue && has_ensure && rescue_pos > ensure_pos) return expand_error(err, stmts[ensure_pos]->span, "ensure must follow rescue in a try");
    size_t boundary = has_rescue ? rescue_pos : ensure_pos;
    size_t expected_sections = (has_rescue ? 1u : 0u) + (has_ensure ? 1u : 0u);
    if (scount - boundary != expected_sections) return expand_error(err, stmts[boundary]->span, "try sections (rescue/ensure) must be the final statements of the body");

    IshCore *body_core = expand_body_items(ctx, stmts, 1, boundary, err);
    if (!body_core) return NULL;

    IshCore *guarded = body_core;
    if (has_rescue) {
        IshCore *handler = expand_try_handler(ctx, stmts[rescue_pos], err);
        if (!handler) { ish_core_free(body_core); return NULL; }
        guarded = ish_core_rescue(body_core, handler, items[start]->span);
        if (!guarded) { ish_core_free(body_core); ish_core_free(handler); return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span); }
    }
    if (has_ensure) {
        const IshSyntax *estmt = stmts[ensure_pos];
        if (estmt->as.seq.count < 3) { ish_core_free(guarded); return expand_error(err, estmt->span, "ensure requires a cleanup expression"); }
        IshCore *cleanup = expand_parts(ctx, estmt->as.seq.items, 2, estmt->as.seq.count, err);
        if (!cleanup) { ish_core_free(guarded); return NULL; }
        uint32_t tmp_slot = ctx->next_slot++;
        IshCore *ensure = ish_core_ensure(guarded, cleanup, tmp_slot, items[start]->span);
        if (!ensure) { ish_core_free(guarded); ish_core_free(cleanup); return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span); }
        guarded = ensure;
    }
    return guarded;
}

static bool syn_is_dot(const IshSyntax *s) {
    return s->kind == ISH_SYN_WORD && strcmp(s->as.text, ".") == 0;
}

static IshSyntax *make_qualified_word(IshSyntax *const *items, size_t start, size_t *inout_end, IshError *err) {
    IshBuffer name;
    ish_buf_init(&name);
    bool ok = ish_buf_append(&name, items[start]->as.text);
    size_t k = start + 1u;
    while (ok && k + 1u < *inout_end && syn_is_dot(items[k]) && items[k + 1u]->kind == ISH_SYN_WORD) {
        ok = ish_buf_append_char(&name, '.') && ish_buf_append(&name, items[k + 1u]->as.text);
        k += 2u;
    }
    IshSyntax *word = ok ? ish_syn_word(name.data ? name.data : "", items[start]->span) : NULL;
    ish_buf_destroy(&name);
    if (!word) {
        ish_error_oom(err, items[start]->span);
        return NULL;
    }
    for (int phase = 0; phase < 2; phase++) {
        const IshScopeSet *scopes = ish_syn_scope_set(items[start], phase);
        if (scopes) {
            for (size_t si = 0; si < scopes->count; si++) {
                if (!ish_syn_scope_add(word, phase, scopes->items[si])) {
                    ish_syn_free(word);
                    ish_error_oom(err, items[start]->span);
                    return NULL;
                }
            }
        }
    }
    *inout_end = k;
    return word;
}

static bool qualified_word_resolves(ExpandContext *ctx, const IshSyntax *word) {
    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    const IshBinding *binding = resolve_default(ctx, word, &status);
    if (status == ISH_RESOLVE_OK && binding) return true;
    IshResolveStatus method_status = ISH_RESOLVE_UNBOUND;
    return resolve_method_surface(ctx, word, &method_status) != NULL || method_status == ISH_RESOLVE_AMBIGUOUS;
}

static IshCore *expand_method_surface_call(ExpandContext *ctx, const MethodSurfaceDef *method, IshCore *receiver, IshSyntax *const *items, size_t arg_start, size_t end, IshError *err) {
    size_t argc = (receiver ? 1u : 0u) + (end - arg_start);
    if (argc != method->arity) {
        ish_core_free(receiver);
        return expand_error(err, (arg_start < end ? items[arg_start]->span : ish_span_unknown(NULL)), "method '%s.%s' expects %u argument(s), got %zu", method->protocol, method->name, method->arity, argc);
    }
    IshCore *call = ish_core_method_call(ish_atom(ctx->rt, method->protocol), ish_atom(ctx->rt, method->name), receiver ? receiver->span : (arg_start < end ? items[arg_start]->span : ish_span_unknown(NULL)));
    if (!call) {
        ish_core_free(receiver);
        return (IshCore *)(uintptr_t)ish_error_oom(err, arg_start < end ? items[arg_start]->span : ish_span_unknown(NULL));
    }
    if (receiver && !ish_core_method_call_add_arg(call, receiver)) {
        ish_core_free(receiver);
        ish_core_free(call);
        return (IshCore *)(uintptr_t)ish_error_oom(err, receiver->span);
    }
    for (size_t i = arg_start; i < end; i++) {
        IshCore *arg = expand_syntax(ctx, items[i], err);
        if (!arg || !ish_core_method_call_add_arg(call, arg)) {
            ish_core_free(arg);
            ish_core_free(call);
            if (!err->present) ish_error_oom(err, items[i]->span);
            return NULL;
        }
    }
    return call;
}

IshCore *expand_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    bool saw_dot = false;
    bool folded = false;
    IshSyntax **collapsed = malloc((end - start) * sizeof(*collapsed));
    IshSyntax **synthetic = collapsed ? malloc((end - start) * sizeof(*synthetic)) : NULL;
    if (!collapsed || !synthetic) { free(collapsed); free(synthetic); return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span); }
    size_t out_count = 0;
    size_t syn_count = 0;
    for (size_t j = start; j < end;) {
        if (items[j]->kind == ISH_SYN_WORD && j + 2u < end && syn_is_dot(items[j + 1u]) && items[j + 2u]->kind == ISH_SYN_WORD) {
            saw_dot = true;
            size_t chain_end = end;
            IshSyntax *word = make_qualified_word(items, j, &chain_end, err);
            if (!word) {
                for (size_t i = 0; i < syn_count; i++) ish_syn_free(synthetic[i]);
                free(synthetic);
                free(collapsed);
                return NULL;
            }
            if (qualified_word_resolves(ctx, word)) {
                synthetic[syn_count++] = word;
                collapsed[out_count++] = word;
                folded = true;
                j = chain_end;
            } else {
                ish_syn_free(word);
                collapsed[out_count++] = items[j++];
            }
        } else {
            if (syn_is_dot(items[j])) saw_dot = true;
            collapsed[out_count++] = items[j++];
        }
    }
    if (folded) {
        IshCore *result = expand_parts_inner(ctx, collapsed, 0, out_count, err);
        for (size_t i = 0; i < syn_count; i++) ish_syn_free(synthetic[i]);
        free(synthetic);
        free(collapsed);
        return result;
    }
    for (size_t i = 0; i < syn_count; i++) ish_syn_free(synthetic[i]);
    free(synthetic);
    free(collapsed);
    if (!saw_dot) return expand_parts_inner(ctx, items, start, end, err);

    for (size_t i = start + 1u; i + 1u < end; i++) {
        if (!syn_is_dot(items[i]) || items[i + 1u]->kind != ISH_SYN_WORD) continue;
        IshResolveStatus method_status = ISH_RESOLVE_UNBOUND;
        const MethodSurfaceDef *method = resolve_method_surface(ctx, items[i + 1u], &method_status);
        if (method_status == ISH_RESOLVE_AMBIGUOUS) return expand_error(err, items[i + 1u]->span, "ambiguous method '%s'", items[i + 1u]->as.text);
        if (!method) continue;
        IshCore *receiver = expand_parts_inner(ctx, items, start, i, err);
        if (!receiver) return NULL;
        return expand_method_surface_call(ctx, method, receiver, items, i + 2u, end, err);
    }
    return expand_parts_inner(ctx, items, start, end, err);
}

static IshCore *expand_parts_inner(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (start >= end) return expand_error(err, ish_span_unknown(NULL), "empty expression");
    size_t len = end - start;
    if (items[start]->kind == ISH_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, items[start], &payload, err)) return expand_macro_use_from_parts(ctx, items, start, end, payload, err);
        if (err && err->present) return NULL;
    }
    if (items[start]->kind == ISH_SYN_WORD) {
        IshResolveStatus method_status = ISH_RESOLVE_UNBOUND;
        const MethodSurfaceDef *method = resolve_method_surface(ctx, items[start], &method_status);
        if (method_status == ISH_RESOLVE_AMBIGUOUS) return expand_error(err, items[start]->span, "ambiguous method '%s'", items[start]->as.text);
        if (method) return expand_method_surface_call(ctx, method, NULL, items, start + 1u, end, err);
    }
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "fn") == 0) return expand_fn_parts(ctx, items, start, end, err);
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "receive") == 0) return expand_receive_parts(ctx, items, start, end, err);
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "raise") == 0) return expand_raise_parts(ctx, items, start, end, err);
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "try") == 0) return expand_try_parts(ctx, items, start, end, err);
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "cond") == 0) {
        if (len != 4) return expand_error(err, items[start]->span, "cond expects exactly three arguments: condition, then, else");
        IshCore *condition = expand_syntax(ctx, items[start + 1u], err);
        IshCore *then_branch = condition ? expand_syntax(ctx, items[start + 2u], err) : NULL;
        IshCore *else_branch = then_branch ? expand_syntax(ctx, items[start + 3u], err) : NULL;
        if (!condition || !then_branch || !else_branch) {
            ish_core_free(condition);
            ish_core_free(then_branch);
            ish_core_free(else_branch);
            return NULL;
        }
        IshCore *cond = ish_core_cond(condition, then_branch, else_branch, items[start]->span);
        if (!cond) {
            ish_core_free(condition);
            ish_core_free(then_branch);
            ish_core_free(else_branch);
            return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
        }
        return cond;
    }
    if (items[start]->kind == ISH_SYN_WORD && !head_is_bound(ctx, items[start])) {
        const char *w = items[start]->as.text;
        if (strcmp(w, "defprotocol") == 0)
            return expand_error(err, items[start]->span, "'%s' (records/method dispatch) is not implemented", w);
        if (strcmp(w, "record") == 0)
            return expand_error(err, items[start]->span, "record expects 'record NAME do field ... end'");
        if (strcmp(w, "extend") == 0)
            return expand_error(err, items[start]->span, "extend expects 'extend TYPE with PROTOCOL do ... end'");
    }
    if (items[start]->kind == ISH_SYN_WORD && !head_is_bound(ctx, items[start]) &&
        !op_lookup(ctx, items[start], true) && !op_lookup(ctx, items[start], false)) {
        uint32_t resolver_payload = 0;
        if (resolve_head_resolver(ctx, items[start], &resolver_payload, err)) return expand_macro_use_from_parts(ctx, items, start, end, resolver_payload, err);
        if (err && err->present) return NULL;
        expand_error(err, items[start]->span, "unbound identifier '%s'", items[start]->as.text);
        note_unbound_context(ctx, items[start], err);
        return NULL;
    }
    if (len == 1) return expand_syntax(ctx, items[start], err);

    bool has_operator = false;
    for (size_t i = start; i < end; i++) {
        if (op_lookup(ctx, items[i], false) || op_lookup(ctx, items[i], true)) {
            has_operator = true;
            break;
        }
    }
    if (has_operator) {
        EnforestParser parser = {ctx, items, end, start, err};
        IshCore *expr = parse_enforest_expr(&parser, 0);
        if (!expr) return NULL;
        if (parser.pos != end) {
            ish_core_free(expr);
            return expand_error(err, items[parser.pos]->span, "unexpected trailing syntax after operator expression");
        }
        return expr;
    }

    IshCore *callee = items[start]->kind == ISH_SYN_WORD ? expand_word_callee(ctx, items[start], err) : expand_syntax(ctx, items[start], err);
    if (!callee) return NULL;
    if (err && err->present) return NULL;
    IshCore *app = ish_core_app(callee, items[start]->span);
    if (!app) {
        ish_core_free(callee);
        ish_error_oom(err, items[start]->span);
        return NULL;
    }
    for (size_t i = start + 1u; i < end; i++) {
        IshCore *arg = expand_syntax(ctx, items[i], err);
        if (!arg || !ish_core_app_add_arg(app, arg)) {
            ish_core_free(arg);
            ish_core_free(app);
            if (!err->present) ish_error_oom(err, items[i]->span);
            return NULL;
        }
    }
    return app;
}

static bool enforest_at_end_or_operator(EnforestParser *parser) {
    return parser->pos >= parser->end || op_lookup(parser->ctx, parser->items[parser->pos], false) != NULL;
}

static IshCore *parse_enforest_primary(EnforestParser *parser) {
    if (parser->pos >= parser->end) return expand_error(parser->err, ish_span_unknown(NULL), "expected operand");
    size_t start = parser->pos;
    IshSyntax *head = parser->items[parser->pos++];
    const IshOperatorDef *prefix_op = op_lookup(parser->ctx, head, true);
    if (prefix_op) {
        IshCore *operand = parse_enforest_expr(parser, prefix_op->precedence);
        if (!operand) return NULL;
        return make_operator_unary(parser->ctx, prefix_op, head, operand, head->span, parser->err);
    }
    if (head->kind == ISH_SYN_WORD && op_lookup(parser->ctx, head, false)) {
        return expand_error(parser->err, head->span, "operator '%s' cannot appear where an operand is required", head->as.text);
    }

    if (head->kind == ISH_SYN_WORD && strcmp(head->as.text, "fn") == 0) {
        parser->pos = parser->end;
        return expand_fn_parts(parser->ctx, parser->items, start, parser->end, parser->err);
    }

    if (head->kind == ISH_SYN_WORD && parser->pos < parser->end && !op_lookup(parser->ctx, parser->items[parser->pos], false)) {
        IshCore *callee = expand_word_callee(parser->ctx, head, parser->err);
        if (!callee || parser->err->present) return NULL;
        IshCore *app = ish_core_app(callee, head->span);
        if (!app) {
            ish_core_free(callee);
            ish_error_oom(parser->err, head->span);
            return NULL;
        }
        while (!enforest_at_end_or_operator(parser)) {
            IshCore *arg = expand_syntax(parser->ctx, parser->items[parser->pos++], parser->err);
            if (!arg || !ish_core_app_add_arg(app, arg)) {
                ish_core_free(arg);
                ish_core_free(app);
                if (!parser->err->present) ish_error_oom(parser->err, head->span);
                return NULL;
            }
        }
        return app;
    }

    return expand_syntax(parser->ctx, head, parser->err);
}

static bool core_app_prepend_arg(IshCore *app, IshCore *arg) {
    if (!app || app->kind != ISH_CORE_APP || !arg) return false;
    if (app->as.app.arg_count == app->as.app.arg_cap) {
        size_t cap = app->as.app.arg_cap ? app->as.app.arg_cap * 2u : 4u;
        IshCore **args = realloc(app->as.app.args, cap * sizeof(*args));
        if (!args) return false;
        app->as.app.args = args;
        app->as.app.arg_cap = cap;
    }
    for (size_t i = app->as.app.arg_count; i > 0; i--) app->as.app.args[i] = app->as.app.args[i - 1u];
    app->as.app.args[0] = arg;
    app->as.app.arg_count++;
    return true;
}

static IshCore *operator_callee(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshSpan span, IshError *err) {
    if (op->target_kind != ISH_OP_TGT_NAMED) return ish_core_primitive(op->primitive, span);
    IshSyntax *word = ish_syn_word(op->target_name, op_token ? op_token->span : span);
    if (!word) return (IshCore *)(uintptr_t)ish_error_oom(err, span);
    if (op_token) {
        const IshScopeSet *scopes = ish_syn_scope_set(op_token, 0);
        if (scopes) {
            for (size_t i = 0; i < scopes->count; i++) {
                if (!ish_syn_scope_add(word, 0, scopes->items[i])) { ish_syn_free(word); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
            }
        }
    }
    IshResolveStatus status = ISH_RESOLVE_UNBOUND;
    const IshBinding *binding = resolve_default(ctx, word, &status);
    if (status == ISH_RESOLVE_OK && binding->kind == ISH_BIND_TRANSFORMER) {
        ish_syn_free(word);
        return expand_error(err, span, "operator target '%s' is a macro; macro operator targets are not yet supported", op->target_name);
    }
    IshCore *callee = expand_word_callee(ctx, word, err);
    ish_syn_free(word);
    return callee;
}

static IshCore *make_operator_app(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshCore *left, IshCore *right, IshSpan span, IshError *err) {
    if (op->target_kind == ISH_OP_TGT_THREAD_FIRST || op->target_kind == ISH_OP_TGT_THREAD_LAST) {
        if (right && right->kind == ISH_CORE_APP) {
            bool ok = op->target_kind == ISH_OP_TGT_THREAD_FIRST ? core_app_prepend_arg(right, left) : ish_core_app_add_arg(right, left);
            if (!ok) {
                ish_core_free(left);
                ish_core_free(right);
                ish_error_oom(err, span);
                return NULL;
            }
            return right;
        }
        IshCore *app = ish_core_app(right, span);
        if (!app || !ish_core_app_add_arg(app, left)) {
            ish_core_free(app);
            if (!app) ish_core_free(right);
            ish_core_free(left);
            ish_error_oom(err, span);
            return NULL;
        }
        return app;
    }
    IshCore *callee = operator_callee(ctx, op, op_token, span, err);
    if (!callee) { ish_core_free(left); ish_core_free(right); return NULL; }
    IshCore *app = ish_core_app(callee, span);
    if (!app) { ish_core_free(callee); ish_core_free(left); ish_core_free(right); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    if (!ish_core_app_add_arg(app, left)) { ish_core_free(left); ish_core_free(right); ish_core_free(app); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    if (!ish_core_app_add_arg(app, right)) { ish_core_free(right); ish_core_free(app); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    return app;
}

static IshCore *make_operator_unary(ExpandContext *ctx, const IshOperatorDef *op, const IshSyntax *op_token, IshCore *operand, IshSpan span, IshError *err) {
    IshCore *callee = operator_callee(ctx, op, op_token, span, err);
    if (!callee) { ish_core_free(operand); return NULL; }
    IshCore *app = ish_core_app(callee, span);
    if (!app) { ish_core_free(callee); ish_core_free(operand); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    if (!ish_core_app_add_arg(app, operand)) { ish_core_free(operand); ish_core_free(app); return (IshCore *)(uintptr_t)ish_error_oom(err, span); }
    return app;
}

static IshCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec) {
    IshCore *left = parse_enforest_primary(parser);
    if (!left) return NULL;

    while (parser->pos < parser->end) {
        IshSyntax *op_token = parser->items[parser->pos];
        const IshOperatorDef *op = op_lookup(parser->ctx, op_token, false);
        if (!op || op->precedence < min_prec) break;
        IshSpan op_span = op_token->span;
        parser->pos++;
        if (op->fixity == ISH_OP_FIX_POSTFIX) {
            left = make_operator_unary(parser->ctx, op, op_token, left, op_span, parser->err);
            if (!left) return NULL;
            continue;
        }
        uint8_t next_min = op->assoc == ISH_OP_ASSOC_RIGHT ? op->precedence : (uint8_t)(op->precedence + 1u);
        IshCore *right = parse_enforest_expr(parser, next_min);
        if (!right) {
            ish_core_free(left);
            return NULL;
        }
        left = make_operator_app(parser->ctx, op, op_token, left, right, op_span, parser->err);
        if (!left) return NULL;
    }
    return left;
}

static IshCore *expand_protocol_expr(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count < 2) return expand_error(err, syn->span, "empty %%-expr");
    if (syn->as.seq.items[1]->kind == ISH_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, syn->as.seq.items[1], &payload, err)) return expand_macro_use(ctx, syn, syn->as.seq.items[1], payload, err);
        if (err && err->present) return NULL;
    }
    return expand_parts(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IshCore *expand_protocol_body(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    IshCore *core = expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
    surface_rollback(ctx, &checkpoint);
    return core;
}

static IshCore *expand_protocol_group(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-group expects one child");
    return expand_syntax(ctx, syn->as.seq.items[1], err);
}

static IshCore *expand_protocol_expression(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-expression expects one child");
    const IshSyntax *child = syn->as.seq.items[1];
    if (child->kind == ISH_SYN_WORD && child->as.text[0] == '&' && child->as.text[1] != '\0') {
        IshSyntax *word = ish_syn_word(child->as.text + 1, child->span);
        if (!word) return (IshCore *)(uintptr_t)ish_error_oom(err, child->span);
        bool ok = true;
        for (int phase = 0; phase < 2 && ok; phase++) {
            const IshScopeSet *scopes = ish_syn_scope_set(child, phase);
            if (scopes) for (size_t si = 0; si < scopes->count && ok; si++) ok = ish_syn_scope_add(word, phase, scopes->items[si]);
        }
        IshCore *core = ok ? expand_word_callee(ctx, word, err) : (IshCore *)(uintptr_t)ish_error_oom(err, child->span);
        ish_syn_free(word);
        return core;
    }
    return expand_syntax(ctx, child, err);
}

static IshCore *expand_protocol_syntax_quote(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-syntax expects one child");
    IshValue value = ish_syntax_value(ctx->rt, syn->as.seq.items[1], err);
    if (err && err->present) return NULL;
    return ish_core_literal(value, syn->span);
}

static IshCore *expand_program(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (!syn_is_protocol(syn, "%-package-begin")) return expand_error(err, syn->span, "expected %%-package-begin syntax");
    return expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IshCore *expand_container(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    IshPrimitive prim;
    switch (syn->kind) {
        case ISH_SYN_LIST: prim = ISH_PRIM_LIST; break;
        case ISH_SYN_VECTOR: prim = ISH_PRIM_VECTOR; break;
        case ISH_SYN_TUPLE: prim = ISH_PRIM_TUPLE; break;
        case ISH_SYN_DICT:
            if (syn->as.seq.count % 2u != 0) return expand_error(err, syn->span, "dict literal requires key/value pairs");
            prim = ISH_PRIM_DICT;
            break;
        default:
            return expand_error(err, syn->span, "unsupported container syntax");
    }
    IshCore *callee = ish_core_primitive(prim, syn->span);
    if (!callee) return (IshCore *)(uintptr_t)ish_error_oom(err, syn->span);
    IshCore *app = ish_core_app(callee, syn->span);
    if (!app) {
        ish_core_free(callee);
        return (IshCore *)(uintptr_t)ish_error_oom(err, syn->span);
    }
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        IshCore *elem = expand_syntax(ctx, syn->as.seq.items[i], err);
        if (!elem || !ish_core_app_add_arg(app, elem)) {
            ish_core_free(elem);
            ish_core_free(app);
            if (!err->present) ish_error_oom(err, syn->span);
            return NULL;
        }
    }
    return app;
}

IshCore *expand_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    IshCore *lit = literal_from_syntax(ctx, syn, err);
    if (lit || (err && err->present)) return lit;

    IshValue literal = ish_nil();
    if (value_from_literal_syntax(ctx, syn, &literal, err)) return ish_core_literal(literal, syn->span);
    if (err && err->present) return NULL;

    if (syn->kind == ISH_SYN_WORD) return expand_word_ref(ctx, syn, err);
    if (syn_is_protocol(syn, "%-expr")) return expand_protocol_expr(ctx, syn, err);
    if (syn_is_protocol(syn, "%-body")) return expand_protocol_body(ctx, syn, err);
    if (syn_is_protocol(syn, "%-group")) return expand_protocol_group(ctx, syn, err);
    if (syn_is_protocol(syn, "%-expression")) return expand_protocol_expression(ctx, syn, err);
    if (syn_is_protocol(syn, "%-syntax")) return expand_protocol_syntax_quote(ctx, syn, err);
    if (syn_is_protocol(syn, "%-quasisyntax")) return expand_protocol_quasisyntax(ctx, syn, err);
    if (syn_is_protocol(syn, "%-quote")) return expand_protocol_quote(ctx, syn, err);
    if (syn_is_protocol(syn, "%-quasiquote")) return expand_protocol_quasiquote(ctx, syn, err);
    if (syn_is_protocol(syn, "%-string")) return expand_protocol_string(ctx, syn, err);
    if (syn_is_protocol(syn, "%-package-begin")) return expand_program(ctx, syn, err);
    if (syn_is_protocol(syn, "%-word") || syn_is_protocol(syn, "%-shell-var") || syn_is_protocol(syn, "%-redirect")) {
        return expand_error(err, syn->span, "shell syntax requires command graph expansion");
    }
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE || syn->kind == ISH_SYN_DICT)
        return expand_container(ctx, syn, err);
    return expand_error(err, syn->span, "unsupported syntax for the current expansion phase");
}

bool ish_expand_syntax(IshRuntime *rt, const IshSyntax *syntax, IshCore **out, IshError *err) {
    return ish_expand_syntax_with_runner(rt, syntax, NULL, out, err);
}

bool ish_expand_syntax_with_runner(IshRuntime *rt, const IshSyntax *syntax, IshMacroRunner *runner, IshCore **out, IshError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    ctx.phase_env = ish_phase_env_create(rt, ctx.phase_ns);
    if (!ctx.phase_env) {
        ish_error_oom(err, ish_span_unknown(NULL));
        ctx_destroy(&ctx);
        return false;
    }
    ctx.runner = runner ? runner : &ctx.local_runner;
    void *old_local_expand_user = rt->local_expand_user;
    IshLocalExpandFn old_local_expand = rt->local_expand;
    void *old_free_identifier_eq_user = rt->free_identifier_eq_user;
    IshFreeIdentifierEqFn old_free_identifier_eq = rt->free_identifier_eq;
    rt->local_expand_user = &ctx;
    rt->local_expand = local_expand_callback;
    rt->free_identifier_eq_user = &ctx;
    rt->free_identifier_eq = free_identifier_eq_callback;
    void *old_register_operator_user = rt->register_operator_user;
    IshRegisterOperatorFn old_register_operator = rt->register_operator;
    rt->register_operator_user = &ctx;
    rt->register_operator = register_operator_callback;
    void *old_register_macro_user = rt->register_macro_user;
    IshRegisterMacroFn old_register_macro = rt->register_macro;
    rt->register_macro_user = &ctx;
    rt->register_macro = register_macro_callback;
    void *old_expander_surface_user = rt->expander_surface_user;
    IshExpanderSurfaceFn old_expander_surface = rt->expander_surface;
    rt->expander_surface_user = &ctx;
    rt->expander_surface = expander_surface_callback;
    if (!ctx_seed(&ctx, err)) {
        rt->local_expand_user = old_local_expand_user;
        rt->local_expand = old_local_expand;
        rt->free_identifier_eq_user = old_free_identifier_eq_user;
        rt->free_identifier_eq = old_free_identifier_eq;
    rt->register_operator_user = old_register_operator_user;
    rt->register_operator = old_register_operator;
    rt->register_macro_user = old_register_macro_user;
    rt->register_macro = old_register_macro;
    rt->expander_surface_user = old_expander_surface_user;
    rt->expander_surface = old_expander_surface;
        rt->register_operator_user = old_register_operator_user;
        rt->register_operator = old_register_operator;
        rt->register_macro_user = old_register_macro_user;
        rt->register_macro = old_register_macro;
        rt->expander_surface_user = old_expander_surface_user;
        rt->expander_surface = old_expander_surface;
        ctx_destroy(&ctx);
        return false;
    }
    if (!ctx_activate_kernel(&ctx, err)) {
        rt->local_expand_user = old_local_expand_user;
        rt->local_expand = old_local_expand;
        rt->free_identifier_eq_user = old_free_identifier_eq_user;
        rt->free_identifier_eq = old_free_identifier_eq;
    rt->register_operator_user = old_register_operator_user;
    rt->register_operator = old_register_operator;
    rt->register_macro_user = old_register_macro_user;
    rt->register_macro = old_register_macro;
    rt->expander_surface_user = old_expander_surface_user;
    rt->expander_surface = old_expander_surface;
        rt->register_operator_user = old_register_operator_user;
        rt->register_operator = old_register_operator;
        rt->register_macro_user = old_register_macro_user;
        rt->register_macro = old_register_macro;
        rt->expander_surface_user = old_expander_surface_user;
        rt->expander_surface = old_expander_surface;
        ctx_destroy(&ctx);
        return false;
    }
    IshCore *core = expand_syntax(&ctx, syntax, err);
    if (core && !(err && err->present)) core = wrap_kernel_use(&ctx, core, err);
    rt->local_expand_user = old_local_expand_user;
    rt->local_expand = old_local_expand;
    rt->free_identifier_eq_user = old_free_identifier_eq_user;
    rt->free_identifier_eq = old_free_identifier_eq;
    rt->register_operator_user = old_register_operator_user;
    rt->register_operator = old_register_operator;
    rt->register_macro_user = old_register_macro_user;
    rt->register_macro = old_register_macro;
    rt->expander_surface_user = old_expander_surface_user;
    rt->expander_surface = old_expander_surface;
    ctx_destroy(&ctx);
    if (!core || (err && err->present)) {
        ish_core_free(core);
        return false;
    }
    *out = core;
    return true;
}

bool ish_expand_string(IshRuntime *rt, const char *file, const char *source, IshCore **out, IshError *err) {
    return ish_expand_string_with_runner(rt, file, source, NULL, out, err);
}

bool ish_expand_string_with_runner(IshRuntime *rt, const char *file, const char *source, IshMacroRunner *runner, IshCore **out, IshError *err) {
    IshSyntax *syntax = NULL;
    if (!ish_reader_read_string(file, source, &syntax, err)) return false;
    bool ok = ish_expand_syntax_with_runner(rt, syntax, runner, out, err);
    ish_syn_free(syntax);
    return ok;
}
