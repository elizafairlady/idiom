#include "internal.h"

static IdmPrimitive primitive_from_binding(const IdmBinding *binding);
static IdmCore *expand_word_ref_mode(ExpandContext *ctx, const IdmSyntax *word, bool callee_position, IdmError *err);
static IdmCore *expand_word_ref(ExpandContext *ctx, const IdmSyntax *word, IdmError *err);
static IdmCore *expand_word_callee(ExpandContext *ctx, const IdmSyntax *word, IdmError *err);
static bool head_is_bound(ExpandContext *ctx, const IdmSyntax *word);
static IdmCore *expand_raise_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static const IdmSyntax *try_section_head(const IdmSyntax *stmt);
static IdmCore *expand_try_handler(ExpandContext *ctx, const IdmSyntax *stmt, IdmError *err);
static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static bool syn_is_dot(const IdmSyntax *s);
static bool qualified_word_resolves(ExpandContext *ctx, const IdmSyntax *word);
static IdmCore *expand_method_surface_call(ExpandContext *ctx, const MethodSurfaceDef *method, IdmCore *receiver, IdmSyntax *const *items, size_t arg_start, size_t end, IdmError *err);
static IdmCore *expand_parts_inner(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static bool enforest_at_end_or_operator(EnforestParser *parser);
static IdmCore *parse_enforest_primary(EnforestParser *parser);
static bool core_app_prepend_arg(IdmCore *app, IdmCore *arg);
static IdmCore *operator_callee(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err);
static IdmCore *make_operator_app(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmCore *right, IdmSpan span, IdmError *err);
static IdmCore *make_operator_unary(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *operand, IdmSpan span, IdmError *err);
static IdmCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec);
static IdmCore *expand_protocol_expr(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_protocol_body(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_protocol_group(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_protocol_expression(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_protocol_syntax_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_program(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_container(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);

static IdmPrimitive primitive_from_binding(const IdmBinding *binding) {
    return (IdmPrimitive)binding->payload;
}

static bool binding_phase_visible(const IdmBinding *binding, int phase) {
    return binding->phase == phase || binding->phase == IDM_PHASE_ANY;
}

static void note_unbound_context(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    if (!err || !err->present) return;
    for (size_t i = 0; i < word->origins.count; i++) {
        idm_error_note(err, "in expansion of '%s'", word->origins.items[i]);
    }
    int other = ctx->phase == 0 ? 1 : 0;
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    if (idm_binding_resolve(&ctx->bindings, word->as.text, other, IDM_BIND_SPACE_DEFAULT, scopes ? scopes : &ctx->empty_scopes, NULL) == IDM_RESOLVE_OK) {
        if (ctx->phase == 0) idm_error_note(err, "'%s' is bound for-syntax (phase 1) but referenced at runtime (phase 0)", word->as.text);
        else idm_error_note(err, "'%s' is bound at runtime (phase 0) but referenced for-syntax (phase 1)", word->as.text);
        idm_error_note(err, "idiom has exactly two phases; for-syntax inside for-syntax is still for-syntax");
        return;
    }
    size_t hidden = 0;
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        if (binding_phase_visible(&ctx->bindings.items[i], ctx->phase) && ctx->bindings.items[i].space == IDM_BIND_SPACE_DEFAULT &&
            strcmp(ctx->bindings.items[i].name, word->as.text) == 0) {
            hidden++;
        }
    }
    if (hidden != 0) {
        idm_error_note(err, "%zu binding%s named '%s' exist%s but %s not visible here (hygiene scopes differ)",
                       hidden, hidden == 1 ? "" : "s", word->as.text, hidden == 1 ? "s" : "", hidden == 1 ? "is" : "are");
    }
}

static IdmCore *expand_word_ref_mode(ExpandContext *ctx, const IdmSyntax *word, bool callee_position, IdmError *err) {
    uint32_t cap = 0;
    if (capture_lookup_existing(ctx->captures, ctx->capture_count, word, &cap)) return idm_core_capture_ref(cap, word->span);
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK) {
        if (binding->kind == IDM_BIND_LOCAL) {
            if (binding->frame_id == ctx->frame) return idm_core_local_ref(binding->payload, word->span);
            if (materialize_capture(ctx, word, binding, &cap)) return idm_core_capture_ref(cap, word->span);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
        }
        if (binding->kind == IDM_BIND_ARG) {
            if (binding->frame_id == ctx->frame) return idm_core_arg_ref(binding->payload, word->span);
            if (materialize_capture(ctx, word, binding, &cap)) return idm_core_capture_ref(cap, word->span);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
        }
        if (binding->kind == IDM_BIND_GLOBAL) return idm_core_global_ref(binding->payload, word->span);
        if (binding->kind == IDM_BIND_METHOD) return expand_error(err, word->span, "method '%s' requires a receiver", word->as.text);
        if (binding->kind == IDM_BIND_VALUE) {
            IdmPrimitive prim = primitive_from_binding(binding);
            const IdmPrimitiveInfo *info = idm_primitive_info(prim);
            if (!callee_position && info && info->min_arity == 0) {
                IdmCore *callee = idm_core_primitive(prim, word->span);
                if (!callee) return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
                IdmCore *app = idm_core_app(callee, word->span);
                if (!app) {
                    idm_core_free(callee);
                    return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
                }
                return app;
            }
            return idm_core_primitive(prim, word->span);
        }
    }
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, word->span, "ambiguous identifier '%s'", word->as.text);
        size_t candidates = 0;
        for (size_t i = 0; i < ctx->bindings.count; i++) {
            if (binding_phase_visible(&ctx->bindings.items[i], ctx->phase) && ctx->bindings.items[i].space == IDM_BIND_SPACE_DEFAULT &&
                strcmp(ctx->bindings.items[i].name, word->as.text) == 0 &&
                scopes_subset_for_ref(&ctx->bindings.items[i].scopes, word)) {
                candidates++;
            }
        }
        if (candidates != 0) idm_error_note(err, "%zu candidate bindings named '%s' are visible here with incomparable scopes", candidates, word->as.text);
        for (size_t i = 0; i < word->origins.count; i++) idm_error_note(err, "in expansion of '%s'", word->origins.items[i]);
        return NULL;
    }
    expand_error(err, word->span, "unbound identifier '%s'", word->as.text);
    note_unbound_context(ctx, word, err);
    return NULL;
}

static IdmCore *expand_word_ref(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    return expand_word_ref_mode(ctx, word, false, err);
}

static IdmCore *expand_word_callee(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    return expand_word_ref_mode(ctx, word, true, err);
}

static bool head_is_bound(ExpandContext *ctx, const IdmSyntax *word) {
    uint32_t cap = 0;
    if (capture_lookup_existing(ctx->captures, ctx->capture_count, word, &cap)) return true;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    (void)resolve_default(ctx, word, &status);
    return status == IDM_RESOLVE_OK;
}

static IdmCore *expand_raise_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start + 1u >= end) return expand_error(err, items[start]->span, "raise requires a value");
    IdmCore *value = expand_parts(ctx, items, start + 1u, end, err);
    if (!value) return NULL;
    IdmCore *raise = idm_core_raise(value, items[start]->span);
    if (!raise) { idm_core_free(value); return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span); }
    return raise;
}

static const IdmSyntax *try_section_head(const IdmSyntax *stmt) {
    if (!syn_is_protocol(stmt, "%-expr") || stmt->as.seq.count < 2) return NULL;
    const IdmSyntax *head = stmt->as.seq.items[1];
    return head && head->kind == IDM_SYN_WORD ? head : NULL;
}

static IdmCore *expand_try_handler(ExpandContext *ctx, const IdmSyntax *stmt, IdmError *err) {
    IdmSyntax *const *ritems = stmt->as.seq.items;
    size_t rcount = stmt->as.seq.count;
    if (rcount < 5) return expand_error(err, stmt->span, "rescue requires the form: rescue NAME -> HANDLER");
    const IdmSyntax *binder = ritems[2];
    if (binder->kind != IDM_SYN_WORD) return expand_error(err, binder->span, "rescue binder must be an identifier");
    size_t arrow = 0;
    for (size_t i = 3; i < rcount; i++) {
        if (syn_is_word(ritems[i], "->")) { arrow = i; break; }
    }
    if (arrow == 0 || arrow + 1u >= rcount) return expand_error(err, stmt->span, "rescue requires '-> HANDLER'");
    size_t saved_count = ctx->bindings.count;
    uint32_t r_slot = 0;
    if (!local_push_scoped(ctx, binder->as.text, binder, &r_slot)) return (IdmCore *)(uintptr_t)idm_error_oom(err, stmt->span);
    IdmCore *handler_body = expand_parts(ctx, ritems, arrow + 1u, rcount, err);
    local_pop_to(ctx, saved_count, ctx->next_slot);
    if (!handler_body) return NULL;
    IdmCore *raised = idm_core_raised(stmt->span);
    if (!raised) { idm_core_free(handler_body); return (IdmCore *)(uintptr_t)idm_error_oom(err, stmt->span); }
    IdmCore *bind = idm_core_bind_local(r_slot, raised, handler_body, stmt->span);
    if (!bind) { idm_core_free(raised); idm_core_free(handler_body); return (IdmCore *)(uintptr_t)idm_error_oom(err, stmt->span); }
    return bind;
}

static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (end - start != 2u || !syn_is_protocol(items[start + 1u], "%-body")) return expand_error(err, items[start]->span, "try requires a do/end body");
    const IdmSyntax *body_syn = items[start + 1u];
    IdmSyntax *const *stmts = body_syn->as.seq.items;
    size_t scount = body_syn->as.seq.count;
    size_t rescue_pos = 0;
    size_t ensure_pos = 0;
    for (size_t i = 1; i < scount; i++) {
        const IdmSyntax *head = try_section_head(stmts[i]);
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

    IdmCore *body_core = expand_body_items(ctx, stmts, 1, boundary, true, err);
    if (!body_core) return NULL;

    IdmCore *guarded = body_core;
    if (has_rescue) {
        IdmCore *handler = expand_try_handler(ctx, stmts[rescue_pos], err);
        if (!handler) { idm_core_free(body_core); return NULL; }
        guarded = idm_core_rescue(body_core, handler, items[start]->span);
        if (!guarded) { idm_core_free(body_core); idm_core_free(handler); return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span); }
    }
    if (has_ensure) {
        const IdmSyntax *estmt = stmts[ensure_pos];
        if (estmt->as.seq.count < 3) { idm_core_free(guarded); return expand_error(err, estmt->span, "ensure requires a cleanup expression"); }
        IdmCore *cleanup = expand_parts(ctx, estmt->as.seq.items, 2, estmt->as.seq.count, err);
        if (!cleanup) { idm_core_free(guarded); return NULL; }
        uint32_t tmp_slot = ctx->next_slot++;
        IdmCore *ensure = idm_core_ensure(guarded, cleanup, tmp_slot, items[start]->span);
        if (!ensure) { idm_core_free(guarded); idm_core_free(cleanup); return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span); }
        guarded = ensure;
    }
    return guarded;
}

static bool syn_is_dot(const IdmSyntax *s) {
    return s->kind == IDM_SYN_WORD && strcmp(s->as.text, ".") == 0;
}

IdmSyntax *make_qualified_word(IdmSyntax *const *items, size_t start, size_t *inout_end, IdmError *err) {
    IdmBuffer name;
    idm_buf_init(&name);
    bool ok = idm_buf_append(&name, items[start]->as.text);
    size_t k = start + 1u;
    while (ok && k + 1u < *inout_end && syn_is_dot(items[k]) && items[k + 1u]->kind == IDM_SYN_WORD) {
        ok = idm_buf_append_char(&name, '.') && idm_buf_append(&name, items[k + 1u]->as.text);
        k += 2u;
    }
    IdmSyntax *word = ok ? idm_syn_word(name.data ? name.data : "", items[start]->span) : NULL;
    idm_buf_destroy(&name);
    if (!word) {
        idm_error_oom(err, items[start]->span);
        return NULL;
    }
    for (int phase = 0; phase < 2; phase++) {
        const IdmScopeSet *scopes = idm_syn_scope_set(items[start], phase);
        if (scopes) {
            for (size_t si = 0; si < scopes->count; si++) {
                if (!idm_syn_scope_add(word, phase, scopes->items[si])) {
                    idm_syn_free(word);
                    idm_error_oom(err, items[start]->span);
                    return NULL;
                }
            }
        }
    }
    *inout_end = k;
    return word;
}

static bool qualified_word_resolves(ExpandContext *ctx, const IdmSyntax *word) {
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK && binding) return true;
    IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
    return resolve_method_surface(ctx, word, &method_status) != NULL || method_status == IDM_RESOLVE_AMBIGUOUS;
}

static IdmCore *expand_method_surface_call(ExpandContext *ctx, const MethodSurfaceDef *method, IdmCore *receiver, IdmSyntax *const *items, size_t arg_start, size_t end, IdmError *err) {
    size_t argc = (receiver ? 1u : 0u) + (end - arg_start);
    if (argc != method->arity) {
        idm_core_free(receiver);
        return expand_error(err, (arg_start < end ? items[arg_start]->span : idm_span_unknown(NULL)), "method '%s.%s' expects %u argument(s), got %zu", method->protocol, method->name, method->arity, argc);
    }
    IdmCore *call = idm_core_method_call(idm_atom(ctx->rt, method->protocol), idm_atom(ctx->rt, method->name), receiver ? receiver->span : (arg_start < end ? items[arg_start]->span : idm_span_unknown(NULL)));
    if (!call) {
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, arg_start < end ? items[arg_start]->span : idm_span_unknown(NULL));
    }
    if (receiver && !idm_core_method_call_add_arg(call, receiver)) {
        idm_core_free(receiver);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, receiver->span);
    }
    for (size_t i = arg_start; i < end; i++) {
        IdmCore *arg = expand_syntax(ctx, items[i], err);
        if (!arg || !idm_core_method_call_add_arg(call, arg)) {
            idm_core_free(arg);
            idm_core_free(call);
            if (!err->present) idm_error_oom(err, items[i]->span);
            return NULL;
        }
    }
    return call;
}

IdmCore *expand_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    bool saw_dot = false;
    bool folded = false;
    IdmSyntax **collapsed = malloc((end - start) * sizeof(*collapsed));
    IdmSyntax **synthetic = collapsed ? malloc((end - start) * sizeof(*synthetic)) : NULL;
    if (!collapsed || !synthetic) { free(collapsed); free(synthetic); return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span); }
    size_t out_count = 0;
    size_t syn_count = 0;
    for (size_t j = start; j < end;) {
        if (items[j]->kind == IDM_SYN_WORD && j + 2u < end && syn_is_dot(items[j + 1u]) && items[j + 2u]->kind == IDM_SYN_WORD) {
            saw_dot = true;
            size_t chain_end = end;
            IdmSyntax *word = make_qualified_word(items, j, &chain_end, err);
            if (!word) {
                for (size_t i = 0; i < syn_count; i++) idm_syn_free(synthetic[i]);
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
                idm_syn_free(word);
                collapsed[out_count++] = items[j++];
            }
        } else {
            if (syn_is_dot(items[j])) saw_dot = true;
            collapsed[out_count++] = items[j++];
        }
    }
    if (folded) {
        IdmCore *result = expand_parts_inner(ctx, collapsed, 0, out_count, err);
        for (size_t i = 0; i < syn_count; i++) idm_syn_free(synthetic[i]);
        free(synthetic);
        free(collapsed);
        return result;
    }
    for (size_t i = 0; i < syn_count; i++) idm_syn_free(synthetic[i]);
    free(synthetic);
    free(collapsed);
    if (!saw_dot) return expand_parts_inner(ctx, items, start, end, err);

    for (size_t i = start + 1u; i + 1u < end; i++) {
        if (!syn_is_dot(items[i]) || items[i + 1u]->kind != IDM_SYN_WORD) continue;
        IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
        const MethodSurfaceDef *method = resolve_method_surface(ctx, items[i + 1u], &method_status);
        if (method_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, items[i + 1u]->span, "ambiguous method '%s'", items[i + 1u]->as.text);
        if (!method) continue;
        IdmCore *receiver = expand_parts_inner(ctx, items, start, i, err);
        if (!receiver) return NULL;
        return expand_method_surface_call(ctx, method, receiver, items, i + 2u, end, err);
    }
    return expand_parts_inner(ctx, items, start, end, err);
}

static bool reserved_prefix_string(IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    for (size_t i = start; i + 1u < end; i++) {
        const IdmSyntax *word = items[i];
        const IdmSyntax *str = items[i + 1u];
        if (word->kind != IDM_SYN_WORD || !word->token_raw || str->kind != IDM_SYN_STRING) continue;
        if (!str->token_adjacent_previous || str->token_leading_space) continue;
        idm_error_set(err, word->span, "reserved prefixed string literal '%s\"…\"' (bitstrings and bytestrings arrive post-1.0)", word->as.text);
        return true;
    }
    return false;
}

static IdmCore *expand_parts_inner(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return expand_error(err, idm_span_unknown(NULL), "empty expression");
    size_t len = end - start;
    if (items[start]->kind == IDM_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, items[start], &payload, err)) return expand_macro_use_from_parts(ctx, items, start, end, payload, err);
        if (err && err->present) return NULL;
    }
    if (items[start]->kind == IDM_SYN_WORD) {
        IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
        const MethodSurfaceDef *method = resolve_method_surface(ctx, items[start], &method_status);
        if (method_status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, items[start]->span, "ambiguous method '%s'", items[start]->as.text);
        if (method) return expand_method_surface_call(ctx, method, NULL, items, start + 1u, end, err);
    }
    if (items[start]->kind == IDM_SYN_WORD && strcmp(items[start]->as.text, "fn") == 0) return expand_fn_parts(ctx, items, start, end, err);
    if (items[start]->kind == IDM_SYN_WORD && strcmp(items[start]->as.text, "receive") == 0) return expand_receive_parts(ctx, items, start, end, err);
    if (items[start]->kind == IDM_SYN_WORD && strcmp(items[start]->as.text, "raise") == 0) return expand_raise_parts(ctx, items, start, end, err);
    if (items[start]->kind == IDM_SYN_WORD && strcmp(items[start]->as.text, "try") == 0) return expand_try_parts(ctx, items, start, end, err);
    if (items[start]->kind == IDM_SYN_WORD && strcmp(items[start]->as.text, "cond") == 0) {
        if (len != 4) return expand_error(err, items[start]->span, "cond expects exactly three arguments: condition, then, else");
        IdmCore *condition = expand_syntax(ctx, items[start + 1u], err);
        IdmCore *then_branch = condition ? expand_syntax(ctx, items[start + 2u], err) : NULL;
        IdmCore *else_branch = then_branch ? expand_syntax(ctx, items[start + 3u], err) : NULL;
        if (!condition || !then_branch || !else_branch) {
            idm_core_free(condition);
            idm_core_free(then_branch);
            idm_core_free(else_branch);
            return NULL;
        }
        IdmCore *cond = idm_core_cond(condition, then_branch, else_branch, items[start]->span);
        if (!cond) {
            idm_core_free(condition);
            idm_core_free(then_branch);
            idm_core_free(else_branch);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
        }
        return cond;
    }
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start])) {
        const char *w = items[start]->as.text;
        if (strcmp(w, "defprotocol") == 0)
            return expand_error(err, items[start]->span, "'%s' (records/method dispatch) is not implemented", w);
        if (strcmp(w, "record") == 0)
            return expand_error(err, items[start]->span, "record expects 'record NAME do field ... end'");
        if (strcmp(w, "extend") == 0)
            return expand_error(err, items[start]->span, "extend expects 'extend TYPE with PROTOCOL do ... end'");
    }
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start]) &&
        !op_lookup(ctx, items[start], true) && !op_lookup(ctx, items[start], false)) {
        uint32_t resolver_payload = 0;
        if (resolve_head_resolver(ctx, items[start], &resolver_payload, err)) return expand_macro_use_from_parts(ctx, items, start, end, resolver_payload, err);
        if (err && err->present) return NULL;
        if (reserved_prefix_string(items, start, end, err)) return NULL;
        expand_error(err, items[start]->span, "unbound identifier '%s'", items[start]->as.text);
        note_unbound_context(ctx, items[start], err);
        return NULL;
    }
    if (reserved_prefix_string(items, start, end, err)) return NULL;
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
        IdmCore *expr = parse_enforest_expr(&parser, 0);
        if (!expr) return NULL;
        if (parser.pos != end) {
            idm_core_free(expr);
            return expand_error(err, items[parser.pos]->span, "unexpected trailing syntax after operator expression");
        }
        return expr;
    }

    IdmCore *callee = items[start]->kind == IDM_SYN_WORD ? expand_word_callee(ctx, items[start], err) : expand_syntax(ctx, items[start], err);
    if (!callee) return NULL;
    if (err && err->present) return NULL;
    IdmCore *app = idm_core_app(callee, items[start]->span);
    if (!app) {
        idm_core_free(callee);
        idm_error_oom(err, items[start]->span);
        return NULL;
    }
    for (size_t i = start + 1u; i < end; i++) {
        IdmCore *arg = expand_syntax(ctx, items[i], err);
        if (!arg || !idm_core_app_add_arg(app, arg)) {
            idm_core_free(arg);
            idm_core_free(app);
            if (!err->present) idm_error_oom(err, items[i]->span);
            return NULL;
        }
    }
    return app;
}

static bool enforest_at_end_or_operator(EnforestParser *parser) {
    return parser->pos >= parser->end || op_lookup(parser->ctx, parser->items[parser->pos], false) != NULL;
}

static IdmCore *parse_enforest_primary(EnforestParser *parser) {
    if (parser->pos >= parser->end) return expand_error(parser->err, idm_span_unknown(NULL), "expected operand");
    size_t start = parser->pos;
    IdmSyntax *head = parser->items[parser->pos++];
    const IdmOperatorDef *prefix_op = op_lookup(parser->ctx, head, true);
    if (prefix_op) {
        IdmCore *operand = parse_enforest_expr(parser, prefix_op->precedence);
        if (!operand) return NULL;
        return make_operator_unary(parser->ctx, prefix_op, head, operand, head->span, parser->err);
    }
    if (head->kind == IDM_SYN_WORD && op_lookup(parser->ctx, head, false)) {
        return expand_error(parser->err, head->span, "operator '%s' cannot appear where an operand is required", head->as.text);
    }

    if (head->kind == IDM_SYN_WORD && strcmp(head->as.text, "fn") == 0) {
        parser->pos = parser->end;
        return expand_fn_parts(parser->ctx, parser->items, start, parser->end, parser->err);
    }

    if (head->kind == IDM_SYN_WORD && parser->pos < parser->end && !op_lookup(parser->ctx, parser->items[parser->pos], false)) {
        IdmCore *callee = expand_word_callee(parser->ctx, head, parser->err);
        if (!callee || parser->err->present) return NULL;
        IdmCore *app = idm_core_app(callee, head->span);
        if (!app) {
            idm_core_free(callee);
            idm_error_oom(parser->err, head->span);
            return NULL;
        }
        while (!enforest_at_end_or_operator(parser)) {
            IdmCore *arg = expand_syntax(parser->ctx, parser->items[parser->pos++], parser->err);
            if (!arg || !idm_core_app_add_arg(app, arg)) {
                idm_core_free(arg);
                idm_core_free(app);
                if (!parser->err->present) idm_error_oom(parser->err, head->span);
                return NULL;
            }
        }
        return app;
    }

    return expand_syntax(parser->ctx, head, parser->err);
}

static bool core_app_prepend_arg(IdmCore *app, IdmCore *arg) {
    if (!app || app->kind != IDM_CORE_APP || !arg) return false;
    if (app->as.app.arg_count == app->as.app.arg_cap) {
        size_t cap = app->as.app.arg_cap ? app->as.app.arg_cap * 2u : 4u;
        IdmCore **args = realloc(app->as.app.args, cap * sizeof(*args));
        if (!args) return false;
        app->as.app.args = args;
        app->as.app.arg_cap = cap;
    }
    for (size_t i = app->as.app.arg_count; i > 0; i--) app->as.app.args[i] = app->as.app.args[i - 1u];
    app->as.app.args[0] = arg;
    app->as.app.arg_count++;
    return true;
}

static IdmCore *operator_callee(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err) {
    if (op->target_kind != IDM_OP_TGT_NAMED) return idm_core_primitive(op->primitive, span);
    IdmSyntax *word = idm_syn_word(op->target_name, op_token ? op_token->span : span);
    if (!word) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    for (size_t i = 0; i < op->scopes.count; i++) {
        if (!idm_syn_scope_add(word, 0, op->scopes.items[i])) { idm_syn_free(word); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    }
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK && binding->kind == IDM_BIND_TRANSFORMER) {
        idm_syn_free(word);
        return expand_error(err, span, "operator target '%s' is a macro; macro operator targets are not yet supported", op->target_name);
    }
    IdmCore *callee = expand_word_callee(ctx, word, err);
    idm_syn_free(word);
    return callee;
}

static IdmCore *make_operator_app(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmCore *right, IdmSpan span, IdmError *err) {
    if (op->target_kind == IDM_OP_TGT_THREAD_FIRST || op->target_kind == IDM_OP_TGT_THREAD_LAST) {
        if (right && right->kind == IDM_CORE_APP) {
            bool ok = op->target_kind == IDM_OP_TGT_THREAD_FIRST ? core_app_prepend_arg(right, left) : idm_core_app_add_arg(right, left);
            if (!ok) {
                idm_core_free(left);
                idm_core_free(right);
                idm_error_oom(err, span);
                return NULL;
            }
            return right;
        }
        IdmCore *app = idm_core_app(right, span);
        if (!app || !idm_core_app_add_arg(app, left)) {
            idm_core_free(app);
            if (!app) idm_core_free(right);
            idm_core_free(left);
            idm_error_oom(err, span);
            return NULL;
        }
        return app;
    }
    IdmCore *callee = operator_callee(ctx, op, op_token, span, err);
    if (!callee) { idm_core_free(left); idm_core_free(right); return NULL; }
    IdmCore *app = idm_core_app(callee, span);
    if (!app) { idm_core_free(callee); idm_core_free(left); idm_core_free(right); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    if (!idm_core_app_add_arg(app, left)) { idm_core_free(left); idm_core_free(right); idm_core_free(app); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    if (!idm_core_app_add_arg(app, right)) { idm_core_free(right); idm_core_free(app); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    return app;
}

static IdmCore *make_operator_unary(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *operand, IdmSpan span, IdmError *err) {
    IdmCore *callee = operator_callee(ctx, op, op_token, span, err);
    if (!callee) { idm_core_free(operand); return NULL; }
    IdmCore *app = idm_core_app(callee, span);
    if (!app) { idm_core_free(callee); idm_core_free(operand); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    if (!idm_core_app_add_arg(app, operand)) { idm_core_free(operand); idm_core_free(app); return (IdmCore *)(uintptr_t)idm_error_oom(err, span); }
    return app;
}

static IdmCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec) {
    IdmCore *left = parse_enforest_primary(parser);
    if (!left) return NULL;

    while (parser->pos < parser->end) {
        IdmSyntax *op_token = parser->items[parser->pos];
        const IdmOperatorDef *op = op_lookup(parser->ctx, op_token, false);
        if (!op || op->precedence < min_prec) break;
        IdmSpan op_span = op_token->span;
        parser->pos++;
        if (op->fixity == IDM_OP_FIX_POSTFIX) {
            left = make_operator_unary(parser->ctx, op, op_token, left, op_span, parser->err);
            if (!left) return NULL;
            continue;
        }
        uint8_t next_min = op->assoc == IDM_OP_ASSOC_RIGHT ? op->precedence : (uint8_t)(op->precedence + 1u);
        IdmCore *right = parse_enforest_expr(parser, next_min);
        if (!right) {
            idm_core_free(left);
            return NULL;
        }
        left = make_operator_app(parser->ctx, op, op_token, left, right, op_span, parser->err);
        if (!left) return NULL;
    }
    return left;
}

static IdmCore *expand_protocol_expr(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count < 2) return expand_error(err, syn->span, "empty %%-expr");
    if (syn->as.seq.items[1]->kind == IDM_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, syn->as.seq.items[1], &payload, err)) return expand_macro_use(ctx, syn, syn->as.seq.items[1], payload, err);
        if (err && err->present) return NULL;
    }
    return expand_parts(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IdmCore *expand_protocol_body(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    IdmCore *core = expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, true, err);
    surface_rollback(ctx, &checkpoint);
    return core;
}

static IdmCore *expand_protocol_group(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-group expects one child");
    bool saved = ctx->value_context;
    ctx->value_context = true;
    IdmCore *core = expand_syntax(ctx, syn->as.seq.items[1], err);
    ctx->value_context = saved;
    return core;
}

static IdmCore *expand_protocol_expression(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-expression expects one child");
    const IdmSyntax *child = syn->as.seq.items[1];
    if (child->kind == IDM_SYN_WORD && child->as.text[0] == '&' && child->as.text[1] != '\0') {
        IdmSyntax *word = idm_syn_word(child->as.text + 1, child->span);
        if (!word) return (IdmCore *)(uintptr_t)idm_error_oom(err, child->span);
        bool ok = true;
        for (int phase = 0; phase < 2 && ok; phase++) {
            const IdmScopeSet *scopes = idm_syn_scope_set(child, phase);
            if (scopes) for (size_t si = 0; si < scopes->count && ok; si++) ok = idm_syn_scope_add(word, phase, scopes->items[si]);
        }
        IdmCore *core = ok ? expand_word_callee(ctx, word, err) : (IdmCore *)(uintptr_t)idm_error_oom(err, child->span);
        idm_syn_free(word);
        return core;
    }
    return expand_syntax(ctx, child, err);
}

static IdmCore *expand_protocol_syntax_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-syntax expects one child");
    IdmValue value = idm_syntax_value(ctx->rt, syn->as.seq.items[1], err);
    if (err && err->present) return NULL;
    return idm_core_literal(value, syn->span);
}

static IdmCore *expand_program(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (!syn_is_protocol(syn, "%-package-begin")) return expand_error(err, syn->span, "expected %%-package-begin syntax");
    return expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, true, err);
}

static IdmCore *expand_container(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmPrimitive prim;
    switch (syn->kind) {
        case IDM_SYN_LIST: prim = IDM_PRIM_LIST; break;
        case IDM_SYN_VECTOR: prim = IDM_PRIM_VECTOR; break;
        case IDM_SYN_TUPLE: prim = IDM_PRIM_TUPLE; break;
        case IDM_SYN_DICT:
            if (syn->as.seq.count % 2u != 0) return expand_error(err, syn->span, "dict literal requires key/value pairs");
            prim = IDM_PRIM_DICT;
            break;
        default:
            return expand_error(err, syn->span, "unsupported container syntax");
    }
    IdmCore *callee = idm_core_primitive(prim, syn->span);
    if (!callee) return (IdmCore *)(uintptr_t)idm_error_oom(err, syn->span);
    IdmCore *app = idm_core_app(callee, syn->span);
    if (!app) {
        idm_core_free(callee);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, syn->span);
    }
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        IdmCore *elem = expand_syntax(ctx, syn->as.seq.items[i], err);
        if (!elem || !idm_core_app_add_arg(app, elem)) {
            idm_core_free(elem);
            idm_core_free(app);
            if (!err->present) idm_error_oom(err, syn->span);
            return NULL;
        }
    }
    return app;
}

IdmCore *expand_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmCore *lit = literal_from_syntax(ctx, syn, err);
    if (lit || (err && err->present)) return lit;

    IdmValue literal = idm_nil();
    if (value_from_literal_syntax(ctx, syn, &literal, err)) return idm_core_literal(literal, syn->span);
    if (err && err->present) return NULL;

    if (syn->kind == IDM_SYN_WORD) return expand_word_ref(ctx, syn, err);
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
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT)
        return expand_container(ctx, syn, err);
    return expand_error(err, syn->span, "unsupported syntax for the current expansion phase");
}

bool idm_expand_syntax(IdmRuntime *rt, const IdmSyntax *syntax, IdmCore **out, IdmError *err) {
    return idm_expand_syntax_with_runner(rt, syntax, NULL, out, err);
}

struct IdmRepl {
    IdmRuntime *rt;
    ExpandContext ctx;
    IdmScopeId session_scope;
    IdmBytecodeModule **modules;
    size_t module_count;
    size_t module_cap;
};

static void repl_install_hooks(IdmRepl *repl) {
    hooks_install(repl->rt, &repl->ctx, NULL);
}

IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err) {
    IdmRepl *repl = calloc(1u, sizeof(*repl));
    if (!repl) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    repl->rt = rt;
    ctx_init(&repl->ctx, rt);
    unsigned char session_hash[32];
    idm_sha256("<repl>", 6u, session_hash);
    ctx_set_unit(&repl->ctx, "<repl>", session_hash);
    repl->ctx.repl_global_binds = true;
    repl->ctx.phase_env = idm_phase_env_create(rt, repl->ctx.phase_ns);
    if (!repl->ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&repl->ctx);
        free(repl);
        return NULL;
    }
    repl->ctx.runner = &repl->ctx.local_runner;
    repl->session_scope = idm_scope_fresh(&repl->ctx.scope_store);
    repl_install_hooks(repl);
    if (!ctx_seed(&repl->ctx, err) || !ctx_activate_kernel(&repl->ctx, err)) {
        ctx_destroy(&repl->ctx);
        free(repl);
        return NULL;
    }
    return repl;
}

static bool repl_rollback(IdmRepl *repl, const SurfaceCheckpoint *checkpoint, uint32_t global_seq, uint32_t next_slot) {
    surface_rollback(&repl->ctx, checkpoint);
    repl->ctx.global_seq = global_seq;
    repl->ctx.next_slot = next_slot;
    return false;
}

bool idm_repl_eval(IdmRepl *repl, const char *source, IdmValue *out_value, bool *out_has_value, IdmError *err) {
    *out_has_value = false;
    IdmSyntax *program = NULL;
    if (!idm_reader_read_string("<repl>", source, &program, err)) return false;
    if (program->as.seq.count < 2) {
        idm_syn_free(program);
        return true;
    }
    repl_install_hooks(repl);
    if (!idm_syn_scope_add_tree(program, 0, repl->session_scope)) {
        idm_syn_free(program);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(&repl->ctx, &checkpoint);
    uint32_t global_seq = repl->ctx.global_seq;
    uint32_t next_slot = repl->ctx.next_slot;
    IdmCore *core = expand_body_items(&repl->ctx, program->as.seq.items, 1, program->as.seq.count, false, err);
    idm_syn_free(program);
    if (!core) return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_core_free(core);
        idm_error_oom(err, idm_span_unknown(NULL));
        return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    }
    idm_bc_init(module);
    uint32_t main_fn = 0;
    bool compiled = idm_core_compile_main(core, module, &main_fn, err);
    idm_core_free(core);
    if (!compiled) {
        idm_bc_destroy(module);
        free(module);
        return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    }
    if (repl->module_count == repl->module_cap) {
        size_t cap = repl->module_cap ? repl->module_cap * 2u : 8u;
        IdmBytecodeModule **grown = realloc(repl->modules, cap * sizeof(*grown));
        if (!grown) {
            idm_bc_destroy(module);
            free(module);
            idm_error_oom(err, idm_span_unknown(NULL));
            return repl_rollback(repl, &checkpoint, global_seq, next_slot);
        }
        repl->modules = grown;
        repl->module_cap = cap;
    }
    repl->modules[repl->module_count++] = module;
    if (!idm_runtime_register_gc_module(repl->rt, module)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    }
    IdmScheduler *sched = idm_sched_create(repl->rt, module, err);
    if (!sched) return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    IdmValue out = idm_nil();
    bool ran = idm_sched_run_main(sched, main_fn, &out, err);
    idm_sched_destroy(sched);
    if (!ran) return repl_rollback(repl, &checkpoint, global_seq, next_slot);
    *out_value = out;
    *out_has_value = true;
    return true;
}

void idm_repl_destroy(IdmRepl *repl) {
    if (!repl) return;
    for (size_t i = 0; i < repl->module_count; i++) {
        idm_runtime_unregister_gc_module(repl->rt, repl->modules[i]);
        idm_bc_destroy(repl->modules[i]);
        free(repl->modules[i]);
    }
    free(repl->modules);
    ctx_destroy(&repl->ctx);
    free(repl);
}

bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    IdmBuffer ser;
    idm_buf_init(&ser);
    if (!idm_syn_serialize(&ser, syntax, err)) {
        idm_buf_destroy(&ser);
        ctx_destroy(&ctx);
        return false;
    }
    unsigned char unit_hash[32];
    idm_sha256(ser.data ? ser.data : "", ser.len, unit_hash);
    idm_buf_destroy(&ser);
    ctx_set_unit(&ctx, syntax->span.file ? syntax->span.file : "<program>", unit_hash);
    ctx.phase_ns = idm_fresh_phase_namespace(rt, err);
    if (!ctx.phase_ns) {
        ctx_destroy(&ctx);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, ctx.phase_ns);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&ctx);
        return false;
    }
    ctx.runner = runner ? runner : &ctx.local_runner;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    IdmCore *core = NULL;
    if (ctx_seed(&ctx, err) && ctx_activate_kernel(&ctx, err)) {
        core = expand_syntax(&ctx, syntax, err);
        if (core && !(err && err->present)) core = wrap_kernel_use(&ctx, core, err);
    }
    hooks_restore(rt, &saved);
    ctx_destroy(&ctx);
    if (!core || (err && err->present)) {
        idm_core_free(core);
        return false;
    }
    *out = core;
    return true;
}

bool idm_expand_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err) {
    return idm_expand_string_with_runner(rt, file, source, NULL, out, err);
}

bool idm_expand_string_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    IdmSyntax *syntax = NULL;
    if (!idm_reader_read_string(file, source, &syntax, err)) return false;
    bool ok = idm_expand_syntax_with_runner(rt, syntax, runner, out, err);
    idm_syn_free(syntax);
    return ok;
}
