#include "internal.h"

static IdmPattern *pattern_from_param_depth(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, bool allow_bind, IdmError *err);
static IdmPattern *pattern_from_param(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, IdmError *err);
static bool copy_pattern_locals(ExpandContext *ctx, size_t table_base, IdmPatternLocal **out_locals, uint32_t *out_count);
static bool bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_rhs_start);
static bool definition_like_form(const IdmSyntax *form, const char **out_head);
static bool for_syntax_form(const IdmSyntax *form, const IdmSyntax **out_body);
static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export);
static void defn_groups_destroy(DefnGroup *groups, size_t count);
static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const IdmSyntax *name_syntax);
static bool group_add_index(DefnGroup *group, size_t index);
static void body_recs_destroy(BodyRec *recs, size_t count);
static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err);
static IdmCore *expand_defn_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body);
static IdmCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmError *err);
static IdmCore *expand_match_clause(ExpandContext *ctx, const IdmSyntax *clause, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool body_is_clauses(const IdmSyntax *body);
static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, IdmError *err);
static IdmCore *build_clause_fn(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, IdmError *err);

IdmCore *literal_from_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    switch (syn->kind) {
        case IDM_SYN_NIL:
            return idm_core_literal(idm_nil(), syn->span);
        case IDM_SYN_INT:
            return idm_core_literal(idm_int(syn->as.integer), syn->span);
        case IDM_SYN_FLOAT:
            return idm_core_literal(idm_float(syn->as.real), syn->span);
        case IDM_SYN_ATOM:
            if (strcmp(syn->as.text, "nil") == 0) return idm_core_literal(idm_nil(), syn->span);
            return idm_core_literal(idm_atom(ctx->rt, syn->as.text), syn->span);
        case IDM_SYN_STRING: {
            IdmValue value = idm_string(ctx->rt, syn->as.text, err);
            if (err && err->present) return NULL;
            return idm_core_literal(value, syn->span);
        }
        default:
            return NULL;
    }
}

bool value_from_literal_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmValue *out, IdmError *err) {
    switch (syn->kind) {
        case IDM_SYN_NIL:
            *out = idm_nil();
            return true;
        case IDM_SYN_INT:
            *out = idm_int(syn->as.integer);
            return true;
        case IDM_SYN_FLOAT:
            *out = idm_float(syn->as.real);
            return true;
        case IDM_SYN_ATOM:
            if (strcmp(syn->as.text, "nil") == 0) {
                *out = idm_nil();
                return true;
            }
            *out = idm_atom(ctx->rt, syn->as.text);
            return true;
        case IDM_SYN_STRING:
            *out = idm_string(ctx->rt, syn->as.text, err);
            return !(err && err->present);
        case IDM_SYN_LIST: {
            IdmValue list = idm_empty_list();
            for (size_t i = syn->as.seq.count; i > 0; i--) {
                IdmValue item = idm_nil();
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i - 1u], &item, err)) return false;
                list = idm_cons(ctx->rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            return true;
        }
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE: {
            IdmValue *items = NULL;
            if (syn->as.seq.count != 0) {
                items = calloc(syn->as.seq.count, sizeof(*items));
                if (!items) {
                    idm_error_oom(err, syn->span);
                    return false;
                }
            }
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i], &items[i], err)) {
                    free(items);
                    return false;
                }
            }
            *out = syn->kind == IDM_SYN_VECTOR ? idm_vector(ctx->rt, items, syn->as.seq.count, err) : idm_tuple(ctx->rt, items, syn->as.seq.count, err);
            free(items);
            return !(err && err->present);
        }
        case IDM_SYN_DICT: {
            if (syn->as.seq.count % 2u != 0) {
                idm_error_set(err, syn->span, "dict literal requires key/value pairs");
                return false;
            }
            size_t count = syn->as.seq.count / 2u;
            IdmDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) {
                idm_error_oom(err, syn->span);
                return false;
            }
            for (size_t i = 0; i < count; i++) {
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err) ||
                    !value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u + 1u], &entries[i].value, err)) {
                    free(entries);
                    return false;
                }
            }
            *out = idm_dict(ctx->rt, entries, count, err);
            free(entries);
            return !(err && err->present);
        }
        default:
            return false;
    }
}

static bool pattern_binder_note(ExpandContext *ctx, const IdmSyntax *syn) {
    if (!ctx->pat_binder_collect) return true;
    if (ctx->pat_binder_count == ctx->pat_binder_cap) {
        size_t cap = ctx->pat_binder_cap ? ctx->pat_binder_cap * 2u : 4u;
        const IdmSyntax **grown = realloc(ctx->pat_binders, cap * sizeof(*grown));
        if (!grown) return false;
        ctx->pat_binders = grown;
        ctx->pat_binder_cap = cap;
    }
    ctx->pat_binders[ctx->pat_binder_count++] = syn;
    return true;
}

static bool list_pattern_group_items(const IdmSyntax *syn, IdmSyntax *const **out_items, size_t *out_count) {
    if (!syn_is_protocol(syn, "%-group") || syn->as.seq.count != 2) return false;
    const IdmSyntax *expr = syn->as.seq.items[1];
    if (!syn_is_protocol(expr, "%-expr") || expr->as.seq.count < 2) return false;
    if (!syn_is_word(expr->as.seq.items[1], "list")) return false;
    *out_items = expr->as.seq.items + 2;
    *out_count = expr->as.seq.count - 2u;
    return true;
}

static IdmPattern *pattern_from_list_items(ExpandContext *ctx, IdmSyntax *const *items, size_t count, IdmSpan span, IdmError *err) {
    size_t dot_index = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (items[i]->kind == IDM_SYN_WORD && strcmp(items[i]->as.text, ".") == 0) {
            if (dot_index != SIZE_MAX) {
                idm_error_set(err, items[i]->span, "list rest pattern may contain only one dot");
                return NULL;
            }
            dot_index = i;
        }
    }
    if (dot_index != SIZE_MAX) {
        if (dot_index == 0 || dot_index + 2u != count) {
            idm_error_set(err, span, "list rest pattern must have form (list head ... . rest)");
            return NULL;
        }
        IdmPattern *tail = pattern_from_param_depth(ctx, items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
        if (!tail) return NULL;
        for (size_t i = dot_index; i > 0; i--) {
            IdmPattern *head = pattern_from_param_depth(ctx, items[i - 1u], (uint32_t)(i - 1u), false, err);
            if (!head) {
                idm_pat_free(tail);
                return NULL;
            }
            tail = idm_pat_pair(head, tail, span);
            if (!tail) {
                idm_pat_free(head);
                return (IdmPattern *)(uintptr_t)idm_error_oom(err, span);
            }
        }
        return tail;
    }
    IdmPattern **pats = NULL;
    if (count != 0) {
        pats = calloc(count, sizeof(*pats));
        if (!pats) {
            idm_error_oom(err, span);
            return NULL;
        }
    }
    for (size_t i = 0; i < count; i++) {
        pats[i] = pattern_from_param_depth(ctx, items[i], (uint32_t)i, false, err);
        if (!pats[i]) {
            for (size_t j = 0; j < i; j++) idm_pat_free(pats[j]);
            free(pats);
            return NULL;
        }
    }
    IdmPattern *pat = idm_pat_sequence(IDM_PAT_LIST, pats, count, span);
    if (!pat) {
        for (size_t i = 0; i < count; i++) idm_pat_free(pats[i]);
        free(pats);
        idm_error_oom(err, span);
        return NULL;
    }
    return pat;
}

static IdmPattern *pattern_from_param_depth(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, bool allow_bind, IdmError *err) {
    if (syn->kind == IDM_SYN_WORD) {
        if (strcmp(syn->as.text, "_") == 0) return idm_pat_wildcard(syn->span);
        if (!allow_bind) {
            const IdmBinding *existing = resolve_default(ctx, syn, NULL);
            bool have = existing && existing->kind == IDM_BIND_LOCAL && existing->frame_id == ctx->frame;
            if (!have && (!local_push_scoped(ctx, syn->as.text, syn, NULL) || !pattern_binder_note(ctx, syn))) {
                idm_error_oom(err, syn->span);
                return NULL;
            }
            return idm_pat_bind(syn->as.text, syn->span);
        }
        const IdmBinding *existing = resolve_default(ctx, syn, NULL);
        bool have = existing && existing->kind == IDM_BIND_ARG && existing->frame_id == ctx->frame;
        if (!have && (!arg_push_slot(ctx, syn, arg_index) || !pattern_binder_note(ctx, syn))) {
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return idm_pat_bind(syn->as.text, syn->span);
    }
    if (syn_is_protocol(syn, "%-quote") && syn->as.seq.count == 2) {
        IdmValue datum = idm_nil();
        if (!expand_quote_datum(ctx, syn->as.seq.items[1], &datum, err)) return NULL;
        return idm_pat_literal(datum, syn->span);
    }
    IdmSyntax *const *list_items = NULL;
    size_t list_count = 0;
    if (list_pattern_group_items(syn, &list_items, &list_count)) return pattern_from_list_items(ctx, list_items, list_count, syn->span, err);
    if (syn->kind == IDM_SYN_LIST) {
        idm_error_set(err, syn->span, "unsupported pattern");
        return NULL;
    }
    IdmValue literal = idm_nil();
    if (syn->kind != IDM_SYN_DICT && value_from_literal_syntax(ctx, syn, &literal, err)) return idm_pat_literal(literal, syn->span);
    if (syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE) {
        {
            size_t dot_index = SIZE_MAX;
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (syn->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                    if (dot_index != SIZE_MAX) {
                        idm_error_set(err, syn->as.seq.items[i]->span, "sequence rest pattern may contain only one dot");
                        return NULL;
                    }
                    dot_index = i;
                }
            }
            if (dot_index != SIZE_MAX) {
                if (dot_index == 0 || dot_index + 2u != syn->as.seq.count) {
                    idm_error_set(err, syn->span, "sequence rest pattern must have form [head ... . rest] or {head ... . rest}");
                    return NULL;
                }
                IdmPattern **items = calloc(dot_index, sizeof(*items));
                if (!items) {
                    idm_error_oom(err, syn->span);
                    return NULL;
                }
                for (size_t i = 0; i < dot_index; i++) {
                    items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
                IdmPattern *rest = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
                if (!rest) {
                    for (size_t i = 0; i < dot_index; i++) idm_pat_free(items[i]);
                    free(items);
                    return NULL;
                }
                IdmPatternKind kind = syn->kind == IDM_SYN_VECTOR ? IDM_PAT_VECTOR_REST : IDM_PAT_TUPLE_REST;
                IdmPattern *pat = idm_pat_sequence_rest(kind, items, dot_index, rest, syn->span);
                if (!pat) {
                    for (size_t i = 0; i < dot_index; i++) idm_pat_free(items[i]);
                    free(items);
                    idm_pat_free(rest);
                    idm_error_oom(err, syn->span);
                    return NULL;
                }
                return pat;
            }
        }
        IdmPattern **items = NULL;
        if (syn->as.seq.count != 0) {
            items = calloc(syn->as.seq.count, sizeof(*items));
            if (!items) {
                idm_error_oom(err, syn->span);
                return NULL;
            }
        }
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
            if (!items[i]) {
                for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                free(items);
                return NULL;
            }
        }
        IdmPatternKind kind = syn->kind == IDM_SYN_VECTOR ? IDM_PAT_VECTOR : IDM_PAT_TUPLE;
        IdmPattern *pat = idm_pat_sequence(kind, items, syn->as.seq.count, syn->span);
        if (!pat) {
            for (size_t i = 0; i < syn->as.seq.count; i++) idm_pat_free(items[i]);
            free(items);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    if (syn->kind == IDM_SYN_DICT) {
        if (syn->as.seq.count % 2u != 0) {
            idm_error_set(err, syn->span, "dict pattern requires key/value pairs");
            return NULL;
        }
        size_t count = syn->as.seq.count / 2u;
        IdmDictPatternEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
        if (count != 0 && !entries) {
            idm_error_oom(err, syn->span);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err)) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                if (!err->present) idm_error_set(err, syn->as.seq.items[i * 2u]->span, "dict pattern key must be literal until expression-key pattern expansion is implemented");
                return NULL;
            }
            entries[i].pattern = pattern_from_param_depth(ctx, syn->as.seq.items[i * 2u + 1u], (uint32_t)i, false, err);
            if (!entries[i].pattern) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                return NULL;
            }
        }
        IdmPattern *pat = idm_pat_dict(entries, count, syn->span);
        if (!pat) {
            for (size_t i = 0; i < count; i++) idm_pat_free(entries[i].pattern);
            free(entries);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    idm_error_set(err, syn->span, "unsupported pattern");
    return NULL;
}

static IdmPattern *pattern_from_param(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, IdmError *err) {
    return pattern_from_param_depth(ctx, syn, arg_index, true, err);
}

static bool copy_pattern_locals(ExpandContext *ctx, size_t table_base, IdmPatternLocal **out_locals, uint32_t *out_count) {
    const IdmBindingTable *table = &ctx->bindings;
    size_t total = 0;
    for (size_t i = table_base; i < table->count; i++) {
        if (table->items[i].kind == IDM_BIND_LOCAL && table->items[i].frame_id == ctx->frame) total++;
    }
    if (total == 0) {
        *out_locals = NULL;
        *out_count = 0;
        return true;
    }
    IdmPatternLocal *locals = calloc(total, sizeof(*locals));
    if (!locals) return false;
    size_t n = 0;
    for (size_t i = table_base; i < table->count; i++) {
        if (table->items[i].kind != IDM_BIND_LOCAL || table->items[i].frame_id != ctx->frame) continue;
        locals[n].name = idm_strdup(table->items[i].name);
        if (!locals[n].name) {
            for (size_t j = 0; j < n; j++) free(locals[j].name);
            free(locals);
            return false;
        }
        locals[n].slot = table->items[i].payload;
        n++;
    }
    *out_locals = locals;
    *out_count = (uint32_t)total;
    return true;
}

static bool bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_rhs_start) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    if (form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_name = form->as.seq.items[1];
    *out_rhs_start = 3;
    return true;
}

static bool pattern_bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_pattern, size_t *out_rhs_start) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    const IdmSyntax *lhs = form->as.seq.items[1];
    if (lhs->kind == IDM_SYN_WORD) return false;
    if (lhs->kind == IDM_SYN_LIST && !syn_is_protocol(lhs, "%-quote")) {
        IdmSyntax *const *items = NULL;
        size_t count = 0;
        if (!list_pattern_group_items(lhs, &items, &count)) return false;
    }
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_pattern = lhs;
    *out_rhs_start = 3;
    return true;
}

static bool scan_pattern_bind(ExpandContext *ctx, BodyRec *rec, const IdmSyntax *pattern_syntax, size_t rhs_start, IdmError *err) {
    SavedFunctionContext probe;
    begin_function_context(ctx, &probe);
    ctx->pat_binder_collect = true;
    ctx->pat_binder_count = 0;
    IdmPattern *pattern = pattern_from_param_depth(ctx, pattern_syntax, 0, false, err);
    ctx->pat_binder_collect = false;
    size_t count = ctx->pat_binder_count;
    const IdmSyntax **names = NULL;
    uint32_t *slots = NULL;
    bool ok = pattern != NULL;
    if (ok && count != 0) {
        names = malloc(count * sizeof(*names));
        slots = malloc(count * sizeof(*slots));
        ok = names != NULL && slots != NULL;
        if (ok) memcpy(names, ctx->pat_binders, count * sizeof(*names));
    }
    end_function_context(ctx, &probe);
    if (!ok) {
        free(names);
        free(slots);
        idm_pat_free(pattern);
        if (err && !err->present) idm_error_oom(err, pattern_syntax->span);
        return false;
    }
    bool repl_global = ctx->repl_global_binds && ctx->frame == IDM_FRAME_TOP;
    for (size_t i = 0; i < count; i++) slots[i] = repl_global ? ctx->global_seq++ : ctx->next_slot++;
    rec->kind = BODY_REC_BIND_PATTERN;
    rec->rhs_start = rhs_start;
    rec->pattern_syntax = pattern_syntax;
    rec->pattern = pattern;
    rec->pattern_names = names;
    rec->pattern_name_count = count;
    rec->pattern_slots = slots;
    rec->pattern_tmp_slot = ctx->next_slot++;
    return true;
}

static bool definition_like_form(const IdmSyntax *form, const char **out_head) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 2 || form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    size_t base = 1;
    if (syn_is_word(form->as.seq.items[1], "export") && form->as.seq.count >= 3 && (syn_is_word(form->as.seq.items[2], "defn") || syn_is_word(form->as.seq.items[2], "defmacro"))) base = 2;
    const char *head = form->as.seq.items[base]->as.text;
    if (strcmp(head, "def") != 0 && strcmp(head, "defn") != 0 && strcmp(head, "defmacro") != 0) return false;
    *out_head = head;
    return true;
}

static bool for_syntax_form(const IdmSyntax *form, const IdmSyntax **out_body) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count != 3) return false;
    if (!syn_is_word(form->as.seq.items[1], "for-syntax")) return false;
    if (!syn_is_protocol(form->as.seq.items[2], "%-body")) return false;
    *out_body = form->as.seq.items[2];
    return true;
}

static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    size_t base = 1;
    bool exported = false;
    if (form->as.seq.count > 1 && syn_is_word(form->as.seq.items[1], "export")) { exported = true; base = 2; }
    if (form->as.seq.count < base + 3u) return false;
    if (!syn_is_word(form->as.seq.items[base], "defn")) return false;
    if (form->as.seq.items[base + 1u]->kind != IDM_SYN_WORD) return false;
    *out_name = form->as.seq.items[base + 1u];
    *out_param_start = base + 2u;
    if (out_export) *out_export = exported;
    return true;
}

static void defn_groups_destroy(DefnGroup *groups, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(groups[i].indices);
        idm_scope_set_destroy(&groups[i].scopes);
    }
    free(groups);
}

static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const IdmSyntax *name_syntax) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*groups)[i].name, name_syntax->as.text) == 0) {
            IdmScopeSet temp;
            if (!syntax_scopes_copy(&temp, name_syntax)) return NULL;
            bool same = idm_scope_set_equal(&(*groups)[i].scopes, &temp);
            idm_scope_set_destroy(&temp);
            if (same) return &(*groups)[i];
        }
    }
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        DefnGroup *next = realloc(*groups, next_cap * sizeof(*next));
        if (!next) return NULL;
        *groups = next;
        *cap = next_cap;
    }
    DefnGroup *group = &(*groups)[(*count)++];
    group->name = name_syntax->as.text;
    group->name_syntax = name_syntax;
    if (!syntax_scopes_copy(&group->scopes, name_syntax)) return NULL;
    group->slot = UINT32_MAX;
    group->indices = NULL;
    group->count = 0;
    group->cap = 0;
    group->exported = false;
    return group;
}

static bool group_add_index(DefnGroup *group, size_t index) {
    if (group->count == group->cap) {
        size_t cap = group->cap ? group->cap * 2u : 4u;
        size_t *indices = realloc(group->indices, cap * sizeof(*indices));
        if (!indices) return false;
        group->indices = indices;
        group->cap = cap;
    }
    group->indices[group->count++] = index;
    return true;
}

bool record_export(ExpandContext *ctx, const char *name, uint32_t global_id) {
    if (ctx->export_count == ctx->export_cap) {
        size_t cap = ctx->export_cap ? ctx->export_cap * 2u : 8u;
        void *next = realloc(ctx->exports, cap * sizeof(*ctx->exports));
        if (!next) return false;
        ctx->exports = next;
        ctx->export_cap = cap;
    }
    char *copy = idm_strdup(name);
    if (!copy) return false;
    ctx->exports[ctx->export_count].name = copy;
    ctx->exports[ctx->export_count].global_id = global_id;
    ctx->export_count++;
    return true;
}

bool record_package_global(ExpandContext *ctx, const char *name, uint32_t global_id, const IdmScopeSet *scopes) {
    if (!ctx->in_package) return true;
    if (ctx->package_global_count == ctx->package_global_cap) {
        size_t cap = ctx->package_global_cap ? ctx->package_global_cap * 2u : 8u;
        IdmPkgGlobal *next = realloc(ctx->package_globals, cap * sizeof(*next));
        if (!next) return false;
        ctx->package_globals = next;
        ctx->package_global_cap = cap;
    }
    IdmPkgGlobal *global = &ctx->package_globals[ctx->package_global_count];
    global->name = idm_strdup(name);
    if (!global->name) return false;
    global->slot = global_id;
    if (!idm_scope_set_copy(&global->scopes, scopes)) {
        free(global->name);
        global->name = NULL;
        return false;
    }
    ctx->package_global_count++;
    return true;
}

static void body_recs_destroy(BodyRec *recs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (recs[i].kind == BODY_REC_GROUPS) defn_groups_destroy(recs[i].groups, recs[i].group_count);
        if (recs[i].kind == BODY_REC_BIND_PATTERN) {
            idm_pat_free(recs[i].pattern);
            free(recs[i].pattern_names);
            free(recs[i].pattern_slots);
        }
        idm_core_free(recs[i].core);
    }
    free(recs);
}

static bool push_bind_binder(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t slot) {
    bool repl_global = ctx->repl_global_binds && ctx->frame == IDM_FRAME_TOP;
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    bool ok = idm_binding_table_add(&ctx->bindings, name_syntax->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, repl_global ? IDM_BIND_GLOBAL : IDM_BIND_LOCAL, &scopes, slot, repl_global ? IDM_FRAME_GLOBAL : ctx->frame, NULL);
    idm_scope_set_destroy(&scopes);
    return ok;
}

static IdmCore *pattern_extract_value(BodyRec *rec, size_t index, IdmError *err) {
    IdmSpan span = rec->pattern_names[index]->span;
    IdmCore *prim = idm_core_primitive(IDM_PRIM_TUPLE_GET, span);
    IdmCore *app = prim ? idm_core_app(prim, span) : NULL;
    if (!app) {
        idm_core_free(prim);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *ref = idm_core_local_ref(rec->pattern_tmp_slot, span);
    if (!ref || !idm_core_app_add_arg(app, ref)) {
        idm_core_free(ref);
        idm_core_free(app);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *idx = idm_core_literal(idm_int((int64_t)index), span);
    if (!idx || !idm_core_app_add_arg(app, idx)) {
        idm_core_free(idx);
        idm_core_free(app);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return app;
}

static IdmCore *pattern_bind_app(BodyRec *rec, IdmCore *rhs, IdmError *err) {
    IdmSpan span = rec->pattern_syntax->span;
    IdmCore *prim = idm_core_primitive(IDM_PRIM_TUPLE, span);
    IdmCore *body = prim ? idm_core_app(prim, span) : NULL;
    if (!body) {
        idm_core_free(prim);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < rec->pattern_name_count; i++) {
        IdmCore *ref = idm_core_local_ref((uint32_t)i, rec->pattern_names[i]->span);
        if (!ref || !idm_core_app_add_arg(body, ref)) {
            idm_core_free(ref);
            idm_core_free(body);
            idm_core_free(rhs);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    IdmBuffer name_buf;
    idm_buf_init(&name_buf);
    char *name = idm_syn_dump(&name_buf, rec->pattern_syntax) ? idm_buf_take(&name_buf) : NULL;
    idm_buf_destroy(&name_buf);
    IdmCore *fn = idm_core_fn(name ? name : "=", 1, body, span);
    free(name);
    if (!fn) {
        idm_core_free(body);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmPattern **patterns = malloc(sizeof(*patterns));
    if (!patterns) {
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    patterns[0] = rec->pattern;
    if (!idm_core_fn_set_param_patterns_take(fn, patterns, 1)) {
        free(patterns);
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    rec->pattern = NULL;
    IdmPatternLocal *locals = NULL;
    if (rec->pattern_name_count != 0) {
        locals = calloc(rec->pattern_name_count, sizeof(*locals));
        if (!locals) {
            idm_core_free(fn);
            idm_core_free(rhs);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        for (size_t i = 0; i < rec->pattern_name_count; i++) {
            locals[i].name = idm_strdup(rec->pattern_names[i]->as.text);
            if (!locals[i].name) {
                for (size_t j = 0; j < i; j++) free(locals[j].name);
                free(locals);
                idm_core_free(fn);
                idm_core_free(rhs);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
            locals[i].slot = (uint32_t)i;
        }
    }
    if (!idm_core_fn_set_pattern_locals_take(fn, locals, (uint32_t)rec->pattern_name_count)) {
        for (size_t i = 0; i < rec->pattern_name_count; i++) free(locals[i].name);
        free(locals);
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *app = idm_core_app(fn, rec->form->span);
    if (!app) {
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    if (!idm_core_app_add_arg(app, rhs)) {
        idm_core_free(app);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    return app;
}

static IdmCore *build_macro_registration(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, IdmError *err) {
    IdmValue name_value = idm_syntax_value(ctx->rt, name_syntax, err);
    if (err && err->present) {
        idm_core_free(fn);
        return NULL;
    }
    IdmCore *callee = idm_core_primitive(IDM_PRIM_EXPANDER_REGISTER_MACRO, span);
    IdmCore *app = callee ? idm_core_app(callee, span) : NULL;
    if (!app) {
        idm_core_free(callee);
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    IdmCore *name_lit = idm_core_literal(name_value, span);
    if (!name_lit || !idm_core_app_add_arg(app, name_lit)) {
        idm_core_free(name_lit);
        idm_core_free(app);
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (!idm_core_app_add_arg(app, fn)) {
        idm_core_free(app);
        idm_core_free(fn);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return app;
}

static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err) {
    size_t add = body->as.seq.count - 1u;
    size_t needed = *work_count - 1u + add;
    if (needed > *work_cap) {
        size_t cap = *work_cap ? *work_cap : 8u;
        while (cap < needed) cap *= 2u;
        IdmSyntax **grown = realloc(*work, cap * sizeof(*grown));
        if (!grown) return idm_error_oom(err, body->span);
        *work = grown;
        *work_cap = cap;
    }
    memmove(*work + at + add, *work + at + 1u, (*work_count - at - 1u) * sizeof(**work));
    for (size_t k = 0; k < add; k++) (*work)[at + k] = body->as.seq.items[1u + k];
    *work_count = needed;
    return true;
}

IdmCore *expand_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, bool def_scope, IdmError *err) {
    if (index >= count) return idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    size_t work_count = count - index;
    size_t work_cap = work_count;
    IdmSyntax **work = malloc(work_cap * sizeof(*work));
    if (!work) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    for (size_t i = 0; i < work_count; i++) work[i] = items[index + i];
    if (def_scope) {
        IdmScopeId body_scope = idm_scope_fresh(&ctx->scope_store);
        for (size_t i = 0; i < work_count; i++) {
            if (!idm_syn_scope_add_tree(work[i], 0, body_scope)) {
                free(work);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
            }
        }
    }
    IdmScopeSet body_ctx_scopes;
    if (!syntax_scopes_copy(&body_ctx_scopes, work[0])) {
        free(work);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    }
    const IdmScopeSet *saved_op_fallback = ctx->op_fallback;
    ctx->op_fallback = &body_ctx_scopes;
    IdmSyntax **owned = NULL;
    size_t owned_count = 0;
    size_t owned_cap = 0;
    BodyRec *recs = NULL;
    size_t rec_count = 0;
    size_t rec_cap = 0;
    BodyDefCtx def_ctx;
    idm_scope_set_init(&def_ctx.use_site);
    def_ctx.prev = ctx->def_ctx;
    ctx->def_ctx = &def_ctx;
    bool failed = false;
    bool boundary = false;
    size_t boundary_index = 0;
    size_t i = 0;

    while (i < work_count && !failed && !boundary) {
        const IdmSyntax *form = work[i];
        if (syn_is_protocol(form, "%-expr") && form->as.seq.count == 3 &&
            syn_is_word(form->as.seq.items[1], "package") && form->as.seq.items[2]->kind == IDM_SYN_WORD) {
            const char *declared = form->as.seq.items[2]->as.text;
            if (ctx->package_name && strcmp(ctx->package_name, declared) != 0) {
                expand_error(err, form->span, "conflicting package declarations '%s' and '%s' in one package", ctx->package_name, declared);
                failed = true;
                break;
            }
            const char *interned = idm_symbol_text(idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, declared));
            if (!interned) { idm_error_oom(err, form->span); failed = true; break; }
            ctx->package_name = interned;
            i++;
            continue;
        }
        if (syn_is_protocol(form, "%-expr") && form->as.seq.count == 4 &&
            syn_is_word(form->as.seq.items[1], "resolver") && form->as.seq.items[2]->kind == IDM_SYN_WORD &&
            syn_is_protocol(form->as.seq.items[3], "%-body")) {
            int saved_phase = ctx->phase;
            ctx->phase = 1;
            IdmCore *fn = expand_function_literal(ctx, "%resolver", form->as.seq.items[1], form->as.seq.items, 2u, form->as.seq.count, err);
            ctx->phase = saved_phase;
            if (!fn) { failed = true; break; }
            bool ok = register_resolver(ctx, fn, form->span, err);
            idm_core_free(fn);
            if (!ok) { failed = true; break; }
            i++;
            continue;
        }
        const IdmSyntax *for_syntax_body = NULL;
        if (for_syntax_form(form, &for_syntax_body)) {
            int saved_phase = ctx->phase;
            ctx->phase = 1;
            IdmCore *ignored = expand_body_items(ctx, for_syntax_body->as.seq.items, 1, for_syntax_body->as.seq.count, false, err);
            bool ran = ignored != NULL && run_phase_core(ctx, ignored, err);
            ctx->phase = saved_phase;
            idm_core_free(ignored);
            if (!ran) {
                if (err && err->present) idm_error_note(err, "during for-syntax evaluation (%s:%u:%u)", form->span.file ? form->span.file : "<unknown>", form->span.line, form->span.column);
                failed = true;
                break;
            }
            i++;
            continue;
        }
        const char *definition_head = NULL;
        if (definition_like_form(form, &definition_head)) {
            if (strcmp(definition_head, "def") == 0) {
                expand_error(err, form->as.seq.items[1]->span, "source 'def' is not a function declaration; use 'defn'");
                failed = true;
                break;
            }
            if (strcmp(definition_head, "defmacro") == 0) {
                size_t mbase = (form->as.seq.count > 1 && syn_is_word(form->as.seq.items[1], "export")) ? 2u : 1u;
                if (form->as.seq.count < mbase + 3u || form->as.seq.items[mbase + 1u]->kind != IDM_SYN_WORD) {
                    expand_error(err, form->span, "defmacro expects a name, one syntax parameter, and a body");
                    failed = true;
                    break;
                }
                const char *macro_name = form->as.seq.items[mbase + 1u]->as.text;
                size_t temp_base = ctx->bindings.count;
                for (size_t r = 0; r < rec_count && !failed; r++) {
                    if (recs[r].kind == BODY_REC_BIND && !push_bind_binder(ctx, recs[r].bind_name, recs[r].bind_slot)) {
                        idm_error_oom(err, form->span);
                        failed = true;
                    } else if (recs[r].kind == BODY_REC_BIND_PATTERN) {
                        for (size_t n = 0; n < recs[r].pattern_name_count && !failed; n++) {
                            if (!push_bind_binder(ctx, recs[r].pattern_names[n], recs[r].pattern_slots[n])) {
                                idm_error_oom(err, form->span);
                                failed = true;
                            }
                        }
                    }
                }
                if (failed) break;
                int saved_phase = ctx->phase;
                ctx->phase = 1;
                IdmCore *fn = expand_function_literal(ctx, macro_name, form->as.seq.items[mbase], form->as.seq.items, mbase + 2u, form->as.seq.count, err);
                ctx->phase = saved_phase;
                idm_binding_table_truncate(&ctx->bindings, temp_base);
                if (!fn) { failed = true; break; }
                if (fn->kind == IDM_CORE_FN && fn->as.fn.capture_count != 0) {
                    if (mbase == 2u) {
                        idm_core_free(fn);
                        expand_error(err, form->span, "exported defmacro '%s' cannot capture for-syntax locals", macro_name);
                        idm_error_note(err, "captured phase-1 values cannot be serialized into a package artifact; use an exported for-syntax defn helper instead");
                        failed = true;
                        break;
                    }
                    IdmCore *reg = build_macro_registration(ctx, form->as.seq.items[mbase + 1u], fn, form->span, err);
                    if (!reg) { failed = true; break; }
                    if (rec_count == rec_cap) {
                        size_t cap = rec_cap ? rec_cap * 2u : 8u;
                        BodyRec *grown = realloc(recs, cap * sizeof(*grown));
                        if (!grown) { idm_core_free(reg); idm_error_oom(err, form->span); failed = true; break; }
                        recs = grown;
                        rec_cap = cap;
                    }
                    memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
                    recs[rec_count].kind = BODY_REC_EXPR;
                    recs[rec_count].form = form;
                    recs[rec_count].core = reg;
                    rec_count++;
                    i++;
                    continue;
                }
                bool ok = register_macro(ctx, form->as.seq.items[mbase + 1u], fn, form->span, mbase == 2u, err);
                idm_core_free(fn);
                if (!ok) { failed = true; break; }
                i++;
                continue;
            }
            size_t j = i;
            DefnGroup *groups = NULL;
            size_t group_count = 0;
            size_t group_cap = 0;
            while (j < work_count) {
                const IdmSyntax *def_name = NULL;
                size_t ignored_start = 0;
                bool form_export = false;
                if (!defn_form_parts(work[j], &def_name, &ignored_start, &form_export)) break;
                DefnGroup *group = find_or_add_group(&groups, &group_count, &group_cap, def_name);
                if (!group || !group_add_index(group, j)) {
                    defn_groups_destroy(groups, group_count);
                    idm_error_oom(err, work[j]->span);
                    failed = true;
                    break;
                }
                if (form_export) group->exported = true;
                j++;
            }
            if (failed) break;
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            for (size_t k = 0; k < group_count && !failed; k++) {
                bool ok = top_level ? global_push_def_binder(ctx, groups[k].name, groups[k].name_syntax, &groups[k].slot)
                                    : local_push_def_binder(ctx, groups[k].name, groups[k].name_syntax, &groups[k].slot);
                if (!ok) { idm_error_oom(err, form->span); failed = true; break; }
                if (top_level && ctx->in_package && !record_package_global(ctx, groups[k].name, groups[k].slot, &groups[k].scopes)) {
                    idm_error_oom(err, form->span);
                    failed = true;
                    break;
                }
                if (top_level && ctx->in_package && groups[k].exported && !record_export(ctx, groups[k].name, groups[k].slot)) {
                    idm_error_oom(err, form->span);
                    failed = true;
                    break;
                }
            }
            if (failed) { defn_groups_destroy(groups, group_count); break; }
            if (rec_count == rec_cap) {
                size_t cap = rec_cap ? rec_cap * 2u : 8u;
                BodyRec *grown = realloc(recs, cap * sizeof(*grown));
                if (!grown) { defn_groups_destroy(groups, group_count); idm_error_oom(err, form->span); failed = true; break; }
                recs = grown;
                rec_cap = cap;
            }
            memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
            recs[rec_count].kind = BODY_REC_GROUPS;
            recs[rec_count].form = form;
            recs[rec_count].groups = groups;
            recs[rec_count].group_count = group_count;
            rec_count++;
            i = j;
            continue;
        }
        if (syn_is_protocol(form, "%-expr") && form->as.seq.count >= 2 && form->as.seq.items[1]->kind == IDM_SYN_WORD &&
            !bind_form_parts(form, &(const IdmSyntax *){NULL}, &(size_t){0})) {
            bool is_boundary_word =
                syn_is_word(form->as.seq.items[1], "implement") || syn_is_word(form->as.seq.items[1], "extend") ||
                syn_is_word(form->as.seq.items[1], "record") || syn_is_word(form->as.seq.items[1], "use") ||
                syn_is_word(form->as.seq.items[1], "import") || syn_is_word(form->as.seq.items[1], "method") ||
                syn_is_word(form->as.seq.items[1], "protocol") ||
                (syn_is_word(form->as.seq.items[1], "export") && form->as.seq.count >= 3 && syn_is_word(form->as.seq.items[2], "record"));
            if (!is_boundary_word) {
                uint32_t payload = 0;
                const IdmSyntax *head_word = form->as.seq.items[1];
                if (syn_is_word(head_word, "export") && form->as.seq.count >= 3 && form->as.seq.items[2]->kind == IDM_SYN_WORD &&
                    resolve_transformer(ctx, form->as.seq.items[2], &payload, err)) {
                    head_word = form->as.seq.items[2];
                } else if (err && err->present) {
                    failed = true;
                    break;
                } else if (!resolve_transformer(ctx, head_word, &payload, err)) {
                    if (err && err->present) { failed = true; break; }
                    head_word = NULL;
                }
                if (head_word != NULL) {
                    IdmSyntax *use = idm_syn_clone(form);
                    bool ok = use != NULL;
                    ok = ok && idm_syn_property_set(use, "value-context", ctx->value_context ? "true" : "false");
                    ok = ok && idm_syn_property_set(use, "command-sub", ctx->command_sub_context ? "true" : "false");
                    IdmScopeId use_site = idm_scope_fresh(&ctx->scope_store);
                    ok = ok && idm_syn_scope_add_tree(use, 0, use_site);
                    ok = ok && idm_scope_set_add(&def_ctx.use_site, use_site);
                    IdmSyntax *expanded = NULL;
                    if (ok) ok = invoke_macro_to_syntax(ctx, use, head_word, payload, &expanded, err);
                    idm_syn_free(use);
                    if (!ok) { failed = true; break; }
                    if (owned_count == owned_cap) {
                        size_t cap = owned_cap ? owned_cap * 2u : 8u;
                        IdmSyntax **grown = realloc(owned, cap * sizeof(*grown));
                        if (!grown) { idm_syn_free(expanded); idm_error_oom(err, form->span); failed = true; break; }
                        owned = grown;
                        owned_cap = cap;
                    }
                    owned[owned_count++] = expanded;
                    const IdmSyntax *splice_body = NULL;
                    if (syn_is_protocol(expanded, "%-body")) {
                        splice_body = expanded;
                    } else if (syn_is_protocol(expanded, "%-group") && expanded->as.seq.count == 2 &&
                               syn_is_protocol(expanded->as.seq.items[1], "%-expr") && expanded->as.seq.items[1]->as.seq.count == 2 &&
                               syn_is_protocol(expanded->as.seq.items[1]->as.seq.items[1], "%-body")) {
                        splice_body = expanded->as.seq.items[1]->as.seq.items[1];
                    }
                    if (splice_body) {
                        if (!body_work_splice(&work, &work_count, &work_cap, i, splice_body, err)) { failed = true; break; }
                    } else {
                        work[i] = expanded;
                    }
                    continue;
                }
                if (err && err->present) { failed = true; break; }
            } else {
                boundary = true;
                boundary_index = i;
                break;
            }
        }
        if (rec_count == rec_cap) {
            size_t cap = rec_cap ? rec_cap * 2u : 8u;
            BodyRec *grown = realloc(recs, cap * sizeof(*grown));
            if (!grown) { idm_error_oom(err, form->span); failed = true; break; }
            recs = grown;
            rec_cap = cap;
        }
        memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
        const IdmSyntax *bind_name = NULL;
        const IdmSyntax *bind_pattern = NULL;
        size_t rhs_start = 0;
        if (bind_form_parts(form, &bind_name, &rhs_start)) {
            recs[rec_count].kind = BODY_REC_BIND;
            recs[rec_count].bind_name = bind_name;
            recs[rec_count].rhs_start = rhs_start;
            recs[rec_count].bind_slot = (ctx->repl_global_binds && ctx->frame == IDM_FRAME_TOP) ? ctx->global_seq++ : ctx->next_slot++;
        } else if (pattern_bind_form_parts(form, &bind_pattern, &rhs_start)) {
            if (!scan_pattern_bind(ctx, &recs[rec_count], bind_pattern, rhs_start, err)) {
                failed = true;
                break;
            }
        } else {
            recs[rec_count].kind = BODY_REC_EXPR;
        }
        recs[rec_count].form = form;
        rec_count++;
        i++;
    }

    bool prealloc_cells = ctx->frame != IDM_FRAME_TOP;
    if (prealloc_cells) {
        size_t group_recs = 0;
        for (size_t r = 0; r < rec_count; r++) if (recs[r].kind == BODY_REC_GROUPS) group_recs++;
        prealloc_cells = group_recs >= 2u;
    }

    for (size_t r = 0; r < rec_count && !failed; r++) {
        BodyRec *rec = &recs[r];
        if (rec->kind == BODY_REC_BIND) {
            bool saved_vc = ctx->value_context;
            ctx->value_context = true;
            rec->core = expand_parts(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, err);
            ctx->value_context = saved_vc;
            if (!rec->core) { failed = true; break; }
            if (!push_bind_binder(ctx, rec->bind_name, rec->bind_slot)) {
                idm_error_oom(err, rec->form->span);
                failed = true;
                break;
            }
        } else if (rec->kind == BODY_REC_BIND_PATTERN) {
            bool saved_vc = ctx->value_context;
            ctx->value_context = true;
            IdmCore *rhs = expand_parts(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, err);
            ctx->value_context = saved_vc;
            if (!rhs) { failed = true; break; }
            rec->core = pattern_bind_app(rec, rhs, err);
            if (!rec->core) { failed = true; break; }
            for (size_t n = 0; n < rec->pattern_name_count; n++) {
                if (!push_bind_binder(ctx, rec->pattern_names[n], rec->pattern_slots[n])) {
                    idm_error_oom(err, rec->form->span);
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        } else if (rec->kind == BODY_REC_GROUPS) {
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            IdmCore *letrec = idm_core_letrec(rec->form->span);
            if (!letrec) { idm_error_oom(err, rec->form->span); failed = true; break; }
            if (top_level) idm_core_letrec_set_global(letrec);
            else if (prealloc_cells) idm_core_letrec_set_fill_only(letrec);
            for (size_t k = 0; k < rec->group_count; k++) {
                IdmCore *value = expand_defn_group(ctx, &rec->groups[k], work, err);
                if (!value || !idm_core_letrec_add(letrec, rec->groups[k].name, rec->groups[k].slot, value)) {
                    if (value) idm_core_free(value);
                    idm_core_free(letrec);
                    letrec = NULL;
                    if (err && !err->present) idm_error_oom(err, rec->form->span);
                    failed = true;
                    break;
                }
            }
            rec->core = letrec;
        } else if (!rec->core) {
            rec->core = syn_is_protocol(rec->form, "%-expr")
                ? expand_parts(ctx, rec->form->as.seq.items, 1, rec->form->as.seq.count, err)
                : expand_syntax(ctx, rec->form, err);
            if (!rec->core) { failed = true; break; }
        }
    }

    IdmCore *core = NULL;
    if (!failed) {
        if (boundary) {
            const IdmSyntax *bform = work[boundary_index];
            if (syn_is_word(bform->as.seq.items[1], "protocol") && bform->as.seq.count == 4 &&
                bform->as.seq.items[2]->kind == IDM_SYN_WORD && syn_is_protocol(bform->as.seq.items[3], "%-body")) {
                core = expand_protocol_decl(ctx, bform, work, boundary_index, work_count, err);
            } else if (syn_is_word(bform->as.seq.items[1], "implement") && bform->as.seq.count == 3 && bform->as.seq.items[2]->kind == IDM_SYN_WORD) {
                core = expand_implement(ctx, bform->as.seq.items[2], work, boundary_index, work_count, bform->span, err);
            } else if (syn_is_word(bform->as.seq.items[1], "implement") && bform->as.seq.count == 5 && syn_is_word(bform->as.seq.items[2], "protocol") &&
                       bform->as.seq.items[3]->kind == IDM_SYN_WORD && syn_is_protocol(bform->as.seq.items[4], "%-body")) {
                core = expand_implement_protocol(ctx, bform, work, boundary_index, work_count, err);
            } else if (syn_is_word(bform->as.seq.items[1], "extend")) {
                core = expand_extend_decl(ctx, bform, work, boundary_index, work_count, err);
            } else if (syn_is_word(bform->as.seq.items[1], "record") ||
                       (syn_is_word(bform->as.seq.items[1], "export") && bform->as.seq.count >= 3 && syn_is_word(bform->as.seq.items[2], "record"))) {
                core = expand_record_decl(ctx, bform, work, boundary_index, work_count, err);
            } else if (syn_is_word(bform->as.seq.items[1], "method")) {
                core = expand_method_decl(ctx, bform, work, boundary_index, work_count, err);
            } else if (syn_is_word(bform->as.seq.items[1], "use") && bform->as.seq.count >= 3 && package_path_text(bform->as.seq.items[2]) != NULL) {
                core = expand_use(ctx, package_path_text(bform->as.seq.items[2]), NULL, work, boundary_index + 1u, work_count, bform->span, err);
            } else if (syn_is_word(bform->as.seq.items[1], "import") && bform->as.seq.count >= 3 && package_path_text(bform->as.seq.items[2]) != NULL) {
                const char *path = package_path_text(bform->as.seq.items[2]);
                const char *alias = NULL;
                if (bform->as.seq.count == 3) {
                    const char *slash = strrchr(path, '/');
                    alias = slash ? slash + 1u : path;
                } else if (bform->as.seq.count == 5 && syn_is_word(bform->as.seq.items[3], "as") && bform->as.seq.items[4]->kind == IDM_SYN_WORD) {
                    alias = bform->as.seq.items[4]->as.text;
                } else {
                    core = expand_error(err, bform->span, "import expects 'import path' or 'import path as alias'");
                    failed = true;
                }
                if (!failed) core = expand_use(ctx, path, alias, work, boundary_index + 1u, work_count, bform->span, err);
            } else {
                BodyRec tail;
                memset(&tail, 0, sizeof(tail));
                tail.kind = BODY_REC_EXPR;
                tail.form = bform;
                core = syn_is_protocol(bform, "%-expr")
                    ? expand_parts(ctx, bform->as.seq.items, 1, bform->as.seq.count, err)
                    : expand_syntax(ctx, bform, err);
                if (core && boundary_index + 1u < work_count) {
                    IdmCore *rest = expand_body_items(ctx, work, boundary_index + 1u, work_count, false, err);
                    if (!rest) { idm_core_free(core); core = NULL; }
                    else {
                        IdmCore *do_expr = idm_core_do(bform->span);
                        if (!do_expr || !idm_core_do_add(do_expr, core) || !idm_core_do_add(do_expr, rest)) {
                            idm_core_free(core);
                            idm_core_free(rest);
                            idm_core_free(do_expr);
                            idm_error_oom(err, bform->span);
                            core = NULL;
                        } else {
                            core = do_expr;
                        }
                    }
                }
            }
            if (!core) failed = true;
        } else if (rec_count != 0 && recs[rec_count - 1u].kind == BODY_REC_EXPR) {
            core = recs[rec_count - 1u].core;
            recs[rec_count - 1u].core = NULL;
            rec_count--;
        } else {
            core = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
            if (!core) { idm_error_oom(err, idm_span_unknown(NULL)); failed = true; }
        }
    }

    if (!failed) {
        for (size_t r = rec_count; r > 0 && !failed; r--) {
            BodyRec *rec = &recs[r - 1u];
            IdmCore *inner = rec->core;
            rec->core = NULL;
            if (rec->kind == BODY_REC_BIND) {
                if (ctx->repl_global_binds && ctx->frame == IDM_FRAME_TOP) {
                    IdmCore *letrec = idm_core_letrec(rec->form->span);
                    if (!letrec || !idm_core_letrec_add(letrec, rec->bind_name->as.text, rec->bind_slot, inner) || !idm_core_letrec_set_body(letrec, core)) {
                        idm_core_free(letrec);
                        idm_core_free(inner);
                        idm_core_free(core);
                        core = NULL;
                        idm_error_oom(err, rec->form->span);
                        failed = true;
                    } else {
                        idm_core_letrec_set_global(letrec);
                        core = letrec;
                    }
                } else {
                    core = idm_core_bind_local(rec->bind_slot, inner, core, rec->form->span);
                    if (!core) { idm_core_free(inner); idm_error_oom(err, rec->form->span); failed = true; }
                }
            } else if (rec->kind == BODY_REC_BIND_PATTERN) {
                bool repl_global = ctx->repl_global_binds && ctx->frame == IDM_FRAME_TOP;
                for (size_t n = rec->pattern_name_count; n > 0 && !failed; n--) {
                    IdmCore *value = pattern_extract_value(rec, n - 1u, err);
                    if (!value) {
                        idm_core_free(core);
                        core = NULL;
                        failed = true;
                        break;
                    }
                    IdmSpan nspan = rec->pattern_names[n - 1u]->span;
                    if (repl_global) {
                        IdmCore *letrec = idm_core_letrec(nspan);
                        bool ok = letrec != NULL && idm_core_letrec_add(letrec, rec->pattern_names[n - 1u]->as.text, rec->pattern_slots[n - 1u], value);
                        if (ok) ok = idm_core_letrec_set_body(letrec, core);
                        else idm_core_free(value);
                        if (!ok) {
                            idm_core_free(letrec);
                            idm_core_free(core);
                            core = NULL;
                            idm_error_oom(err, rec->form->span);
                            failed = true;
                        } else {
                            idm_core_letrec_set_global(letrec);
                            core = letrec;
                        }
                    } else {
                        core = idm_core_bind_local(rec->pattern_slots[n - 1u], value, core, nspan);
                        if (!core) {
                            idm_core_free(value);
                            idm_error_oom(err, rec->form->span);
                            failed = true;
                        }
                    }
                }
                if (!failed) {
                    core = idm_core_bind_local(rec->pattern_tmp_slot, inner, core, rec->form->span);
                    if (!core) { idm_core_free(inner); idm_error_oom(err, rec->form->span); failed = true; }
                } else {
                    idm_core_free(inner);
                }
            } else if (rec->kind == BODY_REC_GROUPS) {
                if (!idm_core_letrec_set_body(inner, core)) {
                    idm_core_free(inner);
                    idm_core_free(core);
                    core = NULL;
                    idm_error_oom(err, rec->form->span);
                    failed = true;
                } else {
                    core = inner;
                }
            } else {
                IdmCore *do_expr = idm_core_do(rec->form->span);
                if (!do_expr || !idm_core_do_add(do_expr, inner) || !idm_core_do_add(do_expr, core)) {
                    idm_core_free(inner);
                    idm_core_free(core);
                    idm_core_free(do_expr);
                    core = NULL;
                    idm_error_oom(err, rec->form->span);
                    failed = true;
                } else {
                    core = do_expr;
                }
            }
        }
    }

    if (prealloc_cells && !failed) {
        IdmCore *prelude = idm_core_letrec(idm_span_unknown(NULL));
        bool ok = prelude != NULL;
        for (size_t r = 0; ok && r < rec_count; r++) {
            if (recs[r].kind != BODY_REC_GROUPS) continue;
            for (size_t k = 0; ok && k < recs[r].group_count; k++) {
                IdmCore *nil = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
                ok = nil != NULL && idm_core_letrec_add(prelude, recs[r].groups[k].name, recs[r].groups[k].slot, nil);
                if (!ok && nil) idm_core_free(nil);
            }
        }
        ok = ok && idm_core_letrec_set_body(prelude, core);
        if (!ok) {
            idm_core_free(prelude);
            idm_core_free(core);
            core = NULL;
            idm_error_oom(err, idm_span_unknown(NULL));
            failed = true;
        } else {
            core = prelude;
        }
    }

    ctx->def_ctx = def_ctx.prev;
    ctx->op_fallback = saved_op_fallback;
    idm_scope_set_destroy(&body_ctx_scopes);
    idm_scope_set_destroy(&def_ctx.use_site);
    body_recs_destroy(recs, rec_count);
    for (size_t o = 0; o < owned_count; o++) idm_syn_free(owned[o]);
    free(owned);
    free(work);
    if (failed) {
        idm_core_free(core);
        return NULL;
    }
    return core;
}

IdmCore *expand_fn_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end || items[start]->kind != IDM_SYN_WORD || strcmp(items[start]->as.text, "fn") != 0) {
        return expand_error(err, items[start]->span, "expected fn literal");
    }
    if (end - start == 2u && body_is_clauses(items[start + 1u])) {
        return build_clause_fn_styled(ctx, items[start + 1u], items[start + 1u]->as.seq.count, "<lambda>", true, err);
    }
    return expand_function_literal(ctx, "<lambda>", items[start], items, start + 1u, end, err);
}

IdmCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmError *err) {
    size_t cursor = param_start;
    size_t arrow = SIZE_MAX;
    size_t body_index = SIZE_MAX;
    IdmCore *guard = NULL;

    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    bool params_ok = true;
    while (cursor < end) {
        if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
            arrow = cursor;
            cursor++;
            break;
        }
        if (syn_is_protocol(items[cursor], "%-body")) {
            body_index = cursor;
            break;
        }
        if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "when") == 0) {
            if (guard) {
                expand_error(err, items[cursor]->span, "fn clause may have only one guard");
                params_ok = false;
                break;
            }
            size_t guard_start = cursor + 1u;
            cursor++;
            while (cursor < end) {
                if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
                    arrow = cursor;
                    break;
                }
                if (syn_is_protocol(items[cursor], "%-body")) {
                    body_index = cursor;
                    break;
                }
                cursor++;
            }
            if (guard_start == cursor) {
                expand_error(err, items[guard_start - 1u]->span, "fn guard requires an expression before the body");
                params_ok = false;
                break;
            }
            guard = expand_parts(ctx, items, guard_start, cursor, err);
            if (!guard) {
                params_ok = false;
                break;
            }
            if (arrow != SIZE_MAX) cursor++;
            break;
        }
        if (items[cursor]->kind != IDM_SYN_WORD) {
            expand_error(err, items[cursor]->span, "fn parameter must be an identifier");
            params_ok = false;
            break;
        }
        if (!arg_push(ctx, items[cursor], NULL)) {
            expand_error(err, items[cursor]->span, "duplicate fn parameter or out of memory: %s", items[cursor]->as.text);
            params_ok = false;
            break;
        }
        cursor++;
    }
    if (!params_ok) {
        idm_core_free(guard);
        end_function_context(ctx, &saved);
        return NULL;
    }

    IdmCore *body = NULL;
    if (arrow != SIZE_MAX) {
        if (cursor >= end) {
            expand_error(err, head->span, "fn arrow requires a body");
        } else {
            body = expand_parts(ctx, items, cursor, end, err);
        }
    } else if (body_index != SIZE_MAX) {
        if (body_index + 1u != end) {
            expand_error(err, items[body_index]->span, "fn do/end body must be the final fn component");
        } else {
            body = expand_syntax(ctx, items[body_index], err);
        }
    } else {
        expand_error(err, head->span, "fn literal requires -> or do/end body");
    }

    uint32_t arity = (uint32_t)ctx->arg_slots;
    if (!body) {
        idm_core_free(guard);
        end_function_context(ctx, &saved);
        return NULL;
    }
    IdmCore *fn = idm_core_fn(debug_name, arity, body, head->span);
    if (!fn || (guard && !idm_core_fn_set_guard_take(fn, guard))) {
        idm_core_free(body);
        idm_core_free(guard);
        end_function_context(ctx, &saved);
        idm_error_oom(err, head->span);
        return NULL;
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!idm_core_fn_add_capture(fn, ctx->captures[i].kind, ctx->captures[i].source_index)) {
            idm_core_free(fn);
            end_function_context(ctx, &saved);
            idm_error_oom(err, head->span);
            return NULL;
        }
    }
    end_function_context(ctx, &saved);
    return fn;
}

static IdmCore *expand_defn_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err) {
    size_t cursor = param_start;
    size_t arrow = SIZE_MAX;
    size_t body_index = SIZE_MAX;
    IdmCore *guard = NULL;
    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);

    IdmPattern **patterns = NULL;
    size_t pattern_count = 0;
    size_t pattern_cap = 0;
    while (cursor < end) {
        if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
            arrow = cursor;
            cursor++;
            break;
        }
        if (syn_is_protocol(items[cursor], "%-body")) {
            body_index = cursor;
            break;
        }
        if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "when") == 0) {
            if (guard) {
                expand_error(err, items[cursor]->span, "defn clause may have only one guard");
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            size_t guard_start = cursor + 1u;
            cursor++;
            while (cursor < end) {
                if (items[cursor]->kind == IDM_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
                    arrow = cursor;
                    break;
                }
                if (syn_is_protocol(items[cursor], "%-body")) {
                    body_index = cursor;
                    break;
                }
                cursor++;
            }
            if (guard_start == cursor) {
                expand_error(err, items[guard_start - 1u]->span, "defn guard requires an expression before the body");
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            guard = expand_parts(ctx, items, guard_start, cursor, err);
            if (!guard) {
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            if (arrow != SIZE_MAX) cursor++;
            break;
        }
        if (pattern_count == pattern_cap) {
            size_t cap = pattern_cap ? pattern_cap * 2u : 4u;
            IdmPattern **next = realloc(patterns, cap * sizeof(*next));
            if (!next) {
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, items[cursor]->span);
            }
            patterns = next;
            pattern_cap = cap;
        }
        IdmPattern *pat = pattern_from_param(ctx, items[cursor], (uint32_t)pattern_count, err);
        if (!pat) {
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
        patterns[pattern_count++] = pat;
        cursor++;
    }

    IdmCore *body = NULL;
    if (arrow != SIZE_MAX) {
        if (cursor >= end) expand_error(err, head->span, "defn arrow requires a body");
        else body = expand_parts(ctx, items, cursor, end, err);
    } else if (body_index != SIZE_MAX) {
        if (body_index + 1u != end) expand_error(err, items[body_index]->span, "defn do/end body must be final");
        else body = expand_syntax(ctx, items[body_index], err);
    } else {
        expand_error(err, head->span, "defn requires -> or do/end body");
    }
    if (!body) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, saved.table_base, out_locals, out_local_count)) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
        free(patterns);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
    }
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = (uint32_t)pattern_count;
    *out_guard = guard;
    *out_arity = (uint32_t)pattern_count;
    (void)debug_name;
    return body;
}

static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body) {
    if (param_start + 1u != def_form->as.seq.count) return false;
    const IdmSyntax *body = def_form->as.seq.items[param_start];
    if (!body_is_clauses(body)) return false;
    *out_body = body;
    return true;
}

static IdmCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmError *err) {
    size_t total_clauses = 0;
    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        const IdmSyntax *clause_body = NULL;
        total_clauses += defn_form_clause_block(def_form, param_start, &clause_body) ? clause_body->as.seq.count - 1u : 1u;
    }
    bool single = total_clauses == 1;
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IdmCore *result = single ? NULL : idm_core_fn_multi(group->name, items[group->indices[0]]->span);
    if (!single && !result) {
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
    }
    IdmCore *single_body = NULL;
    IdmPattern **single_patterns = NULL;
    uint32_t single_pattern_count = 0;
    IdmPatternLocal *single_pattern_locals = NULL;
    uint32_t single_pattern_local_count = 0;
    IdmCore *single_guard = NULL;
    uint32_t single_arity = 0;

    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        const IdmSyntax *clause_block = NULL;
        if (defn_form_clause_block(def_form, param_start, &clause_block)) {
            bool block_ok = true;
            for (size_t c = 1; c < clause_block->as.seq.count && block_ok; c++) {
                const IdmSyntax *clause = clause_block->as.seq.items[c];
                IdmPattern **patterns = NULL;
                uint32_t pattern_count = 0;
                IdmPatternLocal *pattern_locals = NULL;
                uint32_t pattern_local_count = 0;
                IdmCore *guard = NULL;
                uint32_t arity = 0;
                IdmCore *body = expand_defn_clause(ctx, def_name ? def_name->as.text : group->name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
                if (!body) { block_ok = false; break; }
                if (single) {
                    single_body = body;
                    single_patterns = patterns;
                    single_pattern_count = pattern_count;
                    single_pattern_locals = pattern_locals;
                    single_pattern_local_count = pattern_local_count;
                    single_guard = guard;
                    single_arity = arity;
                } else if (!idm_core_fn_multi_add_clause_take(result, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
                    for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
                    free(patterns);
                    for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
                    free(pattern_locals);
                    idm_core_free(guard);
                    idm_core_free(body);
                    block_ok = false;
                    if (err && !err->present) idm_error_oom(err, clause->span);
                }
            }
            if (!block_ok) {
                idm_core_free(result);
                end_function_context(ctx, &saved);
                return NULL;
            }
            continue;
        }
        IdmPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IdmPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IdmCore *guard = NULL;
        uint32_t arity = 0;
        IdmCore *body = expand_defn_clause(ctx, def_name ? def_name->as.text : group->name, def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!body) {
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return NULL;
        }
        if (single) {
            single_body = body;
            single_patterns = patterns;
            single_pattern_count = pattern_count;
            single_pattern_locals = pattern_locals;
            single_pattern_local_count = pattern_local_count;
            single_guard = guard;
            single_arity = arity;
        } else if (!idm_core_fn_multi_add_clause_take(result, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
            for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
            free(patterns);
            for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
            free(pattern_locals);
            idm_core_free(guard);
            idm_core_free(body);
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, def_form->span);
        }
    }

    if (single) {
        result = idm_core_fn(group->name, single_arity, single_body, items[group->indices[0]]->span);
        if (!result || !idm_core_fn_set_param_patterns_take(result, single_patterns, single_pattern_count) || !idm_core_fn_set_pattern_locals_take(result, single_pattern_locals, single_pattern_local_count) || (single_guard && !idm_core_fn_set_guard_take(result, single_guard))) {
            for (uint32_t p = 0; p < single_pattern_count; p++) idm_pat_free(single_patterns[p]);
            free(single_patterns);
            for (uint32_t p = 0; p < single_pattern_local_count; p++) free(single_pattern_locals[p].name);
            free(single_pattern_locals);
            idm_core_free(single_guard);
            idm_core_free(single_body);
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
        }
    }

    for (size_t i = 0; i < ctx->capture_count; i++) {
        bool ok = single ? idm_core_fn_add_capture(result, ctx->captures[i].kind, ctx->captures[i].source_index) : idm_core_fn_multi_add_capture(result, ctx->captures[i].kind, ctx->captures[i].source_index);
        if (!ok) {
            idm_core_free(result);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[group->indices[0]]->span);
        }
    }
    end_function_context(ctx, &saved);
    return result;
}

static IdmCore *expand_match_clause(ExpandContext *ctx, const IdmSyntax *clause, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err) {
    if (!syn_is_protocol(clause, "%-expr")) return expand_error(err, clause->span, "match clause must be an expression");
    size_t arrow = SIZE_MAX;
    for (size_t i = 1; i < clause->as.seq.count; i++) {
        if (clause->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(clause->as.seq.items[i]->as.text, "->") == 0) {
            arrow = i;
            break;
        }
    }
    if (arrow == SIZE_MAX || arrow == 1 || arrow + 1u >= clause->as.seq.count) return expand_error(err, clause->span, "match clause must have form pattern -> body");
    bool has_guard = arrow > 2;
    if (has_guard && (arrow <= 3 || !syn_is_word(clause->as.seq.items[2], "when"))) return expand_error(err, clause->span, "match guards must have form pattern when guard -> body");
    if (!has_guard && arrow != 2) return expand_error(err, clause->span, "match currently expects one pattern per clause");

    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);
    IdmPattern **patterns = calloc(1u, sizeof(*patterns));
    if (!patterns) {
        end_clause_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, clause->span);
    }
    patterns[0] = pattern_from_param(ctx, clause->as.seq.items[1], 0, err);
    if (!patterns[0]) {
        free(patterns);
        end_clause_context(ctx, &saved);
        return NULL;
    }
    IdmCore *guard = NULL;
    if (has_guard) {
        guard = expand_parts(ctx, clause->as.seq.items, 3, arrow, err);
        if (!guard) {
            idm_pat_free(patterns[0]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
    }
    IdmCore *body = expand_parts(ctx, clause->as.seq.items, arrow + 1u, clause->as.seq.count, err);
    if (!body) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        idm_pat_free(patterns[0]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, saved.table_base, out_locals, out_local_count)) {
        idm_core_free(guard);
        end_clause_context(ctx, &saved);
        idm_pat_free(patterns[0]);
        free(patterns);
        idm_core_free(body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, clause->span);
    }
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = 1;
    *out_guard = guard;
    *out_arity = 1;
    return body;
}

static bool body_is_clauses(const IdmSyntax *body) {
    if (!syn_is_protocol(body, "%-body") || body->as.seq.count < 2) return false;
    const IdmSyntax *first = body->as.seq.items[1];
    if (!syn_is_protocol(first, "%-expr")) return false;
    for (size_t i = 1; i < first->as.seq.count; i++) {
        const IdmSyntax *tok = first->as.seq.items[i];
        if (syn_is_word(tok, "->")) return i > 1u;
        if (syn_is_word(tok, "=") || syn_is_word(tok, "fn") || syn_is_word(tok, "defn") ||
            syn_is_word(tok, "def") || syn_is_word(tok, "defmacro")) return false;
    }
    return false;
}

static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, IdmError *err) {
    if (!syn_is_protocol(body, "%-body") || clause_end < 2u || body->as.seq.count < clause_end) return expand_error(err, body->span, "clause body requires at least one clause");
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IdmCore *multi = idm_core_fn_multi(debug_name, body->span);
    if (!multi) {
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
    }
    for (size_t i = 1; i < clause_end; i++) {
        IdmPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IdmPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IdmCore *guard = NULL;
        uint32_t arity = 0;
        const IdmSyntax *clause = body->as.seq.items[i];
        IdmCore *clause_body = defn_style
            ? expand_defn_clause(ctx, debug_name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err)
            : expand_match_clause(ctx, clause, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!clause_body || !idm_core_fn_multi_add_clause_take(multi, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, clause_body)) {
            if (clause_body) idm_core_free(clause_body);
            if (guard) idm_core_free(guard);
            if (patterns) {
                for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
                free(patterns);
            }
            if (pattern_locals) {
                for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
                free(pattern_locals);
            }
            idm_core_free(multi);
            end_function_context(ctx, &saved);
            if (!err->present) idm_error_oom(err, body->as.seq.items[i]->span);
            return NULL;
        }
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!idm_core_fn_multi_add_capture(multi, ctx->captures[i].kind, ctx->captures[i].source_index)) {
            idm_core_free(multi);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
        }
    }
    end_function_context(ctx, &saved);
    return multi;
}

static IdmCore *build_clause_fn(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, IdmError *err) {
    return build_clause_fn_styled(ctx, body, clause_end, debug_name, false, err);
}

IdmCore *expand_receive_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    size_t body_index = SIZE_MAX;
    for (size_t i = start + 1u; i < end; i++) {
        if (syn_is_protocol(items[i], "%-body")) {
            body_index = i;
            break;
        }
    }
    if (body_index == SIZE_MAX || body_index + 1u != end) return expand_error(err, items[start]->span, "receive requires a final do/end clause body");
    const IdmSyntax *body = items[body_index];
    if (body->as.seq.count < 2) return expand_error(err, body->span, "receive requires at least one clause");

    size_t clause_total = body->as.seq.count;
    const IdmSyntax *after_clause = NULL;
    size_t message_end = clause_total;
    for (size_t i = 1; i < clause_total; i++) {
        const IdmSyntax *clause = body->as.seq.items[i];
        if (syn_is_protocol(clause, "%-expr") && clause->as.seq.count >= 2 && syn_is_word(clause->as.seq.items[1], "after")) {
            if (i + 1u != clause_total) return expand_error(err, clause->span, "receive 'after' clause must be last");
            after_clause = clause;
            message_end = i;
            break;
        }
    }
    if (message_end < 2) return expand_error(err, body->span, "receive requires at least one message clause");

    IdmCore *timeout = NULL;
    IdmCore *timeout_body = NULL;
    if (after_clause) {
        size_t arrow = SIZE_MAX;
        for (size_t i = 2; i < after_clause->as.seq.count; i++) {
            if (syn_is_word(after_clause->as.seq.items[i], "->")) {
                arrow = i;
                break;
            }
        }
        if (arrow == SIZE_MAX || arrow == 2 || arrow + 1u >= after_clause->as.seq.count) return expand_error(err, after_clause->span, "receive 'after' clause must have form after timeout -> body");
        timeout = expand_parts(ctx, after_clause->as.seq.items, 2, arrow, err);
        if (!timeout) return NULL;
        timeout_body = expand_parts(ctx, after_clause->as.seq.items, arrow + 1u, after_clause->as.seq.count, err);
        if (!timeout_body) {
            idm_core_free(timeout);
            return NULL;
        }
    } else {
        timeout = idm_core_literal(idm_atom(ctx->rt, "infinity"), items[start]->span);
        timeout_body = idm_core_literal(idm_nil(), items[start]->span);
        if (!timeout || !timeout_body) {
            idm_core_free(timeout);
            idm_core_free(timeout_body);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
        }
    }

    IdmCore *multi = build_clause_fn(ctx, body, message_end, "<receive>", err);
    if (!multi) {
        idm_core_free(timeout);
        idm_core_free(timeout_body);
        return NULL;
    }

    IdmCore *recv = idm_core_receive(multi, timeout, timeout_body, items[start]->span);
    if (!recv) {
        idm_core_free(multi);
        idm_core_free(timeout);
        idm_core_free(timeout_body);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    return recv;
}
