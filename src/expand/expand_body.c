#include "internal.h"
#include "idiom/regex.h"

static IdmPattern *pattern_from_param_depth(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, bool allow_bind, IdmError *err);
static IdmPattern *pattern_from_param(ExpandContext *ctx, const IdmSyntax *syn, uint32_t arg_index, IdmError *err);
static bool copy_pattern_locals(ExpandContext *ctx, size_t table_base, IdmPatternLocal **out_locals, uint32_t *out_count);
typedef struct {
    size_t param_start;
    size_t param_end;
    bool has_guard;
    size_t guard_start;
    size_t guard_end;
    bool has_arrow;
    size_t body_start;
    size_t body_end;
    size_t body_index;
    uint32_t arity;
} FunctionClauseShape;
static bool bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_rhs_start);
static bool definition_like_form(const IdmSyntax *form, const char **out_head);
static bool for_syntax_form(const IdmSyntax *form, const IdmSyntax **out_body);
static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export);
static bool expand_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
static bool expand_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
static bool expand_core_reader_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err);
static void defn_groups_destroy(DefnGroup *groups, size_t count);
static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const IdmSyntax *name_syntax);
static bool group_add_index(DefnGroup *group, size_t index);
static void body_recs_destroy(BodyRec *recs, size_t count);
static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err);
static bool owned_syntax_push(IdmSyntax ***owned, size_t *count, size_t *cap, IdmSyntax *syn, IdmError *err);
static bool body_install_expanded_syntax(IdmSyntax ***work, size_t *work_count, size_t *work_cap, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmScopeId extra_scope, size_t index, IdmSyntax *expanded, IdmError *err);
static bool body_scope_add_range(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err);
static bool body_definition_scope_add(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err);
static bool use_selection_parse(const IdmSyntax *form, size_t start, size_t end, size_t *out_pos, UseSelection *selection, IdmError *err);
static void use_selection_destroy(UseSelection *selection);
static bool parse_function_clause_shape(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, bool allow_bodyless, FunctionClauseShape *out, IdmError *err);
static bool function_parts_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err);
static bool defn_group_arity(const DefnGroup *group, IdmSyntax *const *items, IdmArity *out_arity, IdmError *err);
static IdmCore *expand_function_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body);
static IdmCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IdmSyntax *const *items, IdmError *err);
static IdmCore *expand_match_clause(ExpandContext *ctx, const IdmSyntax *clause, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err);
static bool body_is_clauses(const IdmSyntax *body, bool allow_empty_pattern);
static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, IdmError *err);
static IdmCore *build_clause_fn(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, IdmError *err);

IdmCore *literal_from_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmValue value = idm_nil();
    if (!value_from_literal_syntax(ctx, syn, &value, err)) return NULL;
    return idm_core_literal(value, syn->span);
}

bool value_from_literal_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmValue *out, IdmError *err) {
    switch (syn->kind) {
        case IDM_SYN_NIL:
            *out = idm_nil();
            return true;
        case IDM_SYN_INT:
            *out = idm_int_promote(ctx->rt, syn->as.integer, err);
            return !(err && err->present);
        case IDM_SYN_BIGINT: {
            bool ok = false;
            *out = idm_int_from_decimal(ctx->rt, syn->as.text, strlen(syn->as.text), &ok, err);
            if (err && err->present) return false;
            if (!ok) return idm_error_set(err, syn->span, "invalid integer literal");
            return true;
        }
        case IDM_SYN_FLOAT:
            *out = idm_float(ctx->rt, syn->as.real, err);
            return !(err && err->present);
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
            size_t elem_count = syn->as.seq.count;
            size_t tail_index = SIZE_MAX;
            if (!dict_rest_index(syn, &tail_index, err)) return false;
            IdmValue tail = idm_nil();
            if (tail_index != SIZE_MAX) {
                if (tail_index + 2u != syn->as.seq.count) return false;
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[tail_index + 1u], &tail, err)) return false;
                if (!idm_is_dict(tail)) return false;
                elem_count = tail_index;
            }
            if (elem_count % 2u != 0) {
                idm_error_set(err, syn->span, "dict literal requires key/value pairs");
                return false;
            }
            size_t pair_count = elem_count / 2u;
            size_t tail_count = tail_index == SIZE_MAX ? 0u : idm_dict_count(tail);
            size_t count = tail_count + pair_count;
            IdmDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) {
                idm_error_oom(err, syn->span);
                return false;
            }
            for (size_t i = 0; i < tail_count; i++) {
                if (!idm_dict_entry(tail, i, &entries[i].key, &entries[i].value)) {
                    free(entries);
                    return idm_error_set(err, syn->span, "dict rest literal conversion failed");
                }
            }
            for (size_t i = 0; i < pair_count; i++) {
                size_t out_i = tail_count + i;
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[out_i].key, err) ||
                    !value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u + 1u], &entries[out_i].value, err)) {
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
        if (!idm_grow((void **)&ctx->pat_binders, &ctx->pat_binder_cap, sizeof(*ctx->pat_binders), 4u, ctx->pat_binder_count + 1u)) return false;
    }
    ctx->pat_binders[ctx->pat_binder_count++] = syn;
    return true;
}

static bool syntax_pattern_note_local(ExpandContext *ctx, const IdmSyntax *name, IdmError *err) {
    const IdmBinding *existing = resolve_default(ctx, name, NULL);
    bool have = existing && existing->kind == IDM_BIND_LOCAL && existing->frame_id == ctx->frame;
    if (!have && (!local_push_scoped(ctx, name->as.text, name, NULL) || !pattern_binder_note(ctx, name))) {
        idm_error_oom(err, name->span);
        return false;
    }
    return true;
}

static IdmSyntaxPattern *syntax_pattern_hole(ExpandContext *ctx, const IdmSyntax *hole, IdmError *err) {
    if (!syn_is_form(hole, "%-unsyntax") || hole->as.seq.count != 2) {
        idm_error_set(err, hole->span, "syntax pattern hole expects %,name");
        return NULL;
    }
    const IdmSyntax *name = hole->as.seq.items[1];
    if (name->kind != IDM_SYN_WORD) {
        idm_error_set(err, hole->span, "syntax pattern hole must bind an identifier");
        return NULL;
    }
    if (strcmp(name->as.text, "_") == 0) return idm_syn_pat_wildcard(hole->span);
    if (!syntax_pattern_note_local(ctx, name, err)) return NULL;
    IdmSyntaxPattern *pat = idm_syn_pat_bind(name->as.text, hole->span);
    if (!pat) idm_error_oom(err, hole->span);
    return pat;
}

static const IdmSyntax *syntax_pattern_splice_form(const IdmSyntax *item) {
    if (syn_is_form(item, "%-expr") && item->as.seq.count == 2 && syn_is_form(item->as.seq.items[1], "%-unsyntax-splicing")) {
        return item->as.seq.items[1];
    }
    return item;
}

static bool syntax_pattern_rest_name(ExpandContext *ctx, const IdmSyntax *splice, const char **out_name, IdmError *err) {
    *out_name = NULL;
    if (!syn_is_form(splice, "%-unsyntax-splicing") || splice->as.seq.count != 2) {
        idm_error_set(err, splice->span, "syntax rest pattern expects %,@name");
        return false;
    }
    const IdmSyntax *name = splice->as.seq.items[1];
    if (name->kind != IDM_SYN_WORD) {
        idm_error_set(err, splice->span, "syntax rest pattern must bind an identifier");
        return false;
    }
    if (strcmp(name->as.text, "_") == 0) return true;
    if (!syntax_pattern_note_local(ctx, name, err)) return false;
    *out_name = name->as.text;
    return true;
}

static IdmSyntaxPattern *syntax_pattern_from_template(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);

static IdmSyntaxPattern *syntax_pattern_sequence(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmSyntaxPattern **items = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t rest_index = IDM_SYN_PAT_NO_REST;
    const char *rest_name = NULL;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        const IdmSyntax *splice = syntax_pattern_splice_form(syn->as.seq.items[i]);
        if (syn_is_form(splice, "%-unsyntax-splicing")) {
            if (rest_index != IDM_SYN_PAT_NO_REST) {
                idm_error_set(err, splice->span, "syntax pattern may contain only one rest hole");
                goto fail;
            }
            rest_index = count;
            if (!syntax_pattern_rest_name(ctx, splice, &rest_name, err)) goto fail;
            continue;
        }
        IdmSyntaxPattern *item = syntax_pattern_from_template(ctx, syn->as.seq.items[i], err);
        if (!item) goto fail;
        if (count == cap) {
            if (!idm_grow((void **)&items, &cap, sizeof(*items), 8u, count + 1u)) {
                idm_syn_pat_free(item);
                idm_error_oom(err, syn->span);
                goto fail;
            }
        }
        items[count++] = item;
    }
    IdmSyntaxPattern *pat = idm_syn_pat_sequence(syn->kind, items, count, rest_index, rest_name, syn->span);
    if (!pat) {
        idm_error_oom(err, syn->span);
        goto fail;
    }
    return pat;

fail:
    for (size_t i = 0; i < count; i++) idm_syn_pat_free(items[i]);
    free(items);
    return NULL;
}

static IdmSyntaxPattern *syntax_pattern_from_template(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if ((syn_is_form(syn, "%-group") || syn_is_form(syn, "%-layout-group")) && syn->as.seq.count == 2) return syntax_pattern_from_template(ctx, syn->as.seq.items[1], err);
    if (syn_is_form(syn, "%-unsyntax")) return syntax_pattern_hole(ctx, syn, err);
    if (syn_is_form(syn, "%-unsyntax-splicing")) {
        idm_error_set(err, syn->span, "syntax rest pattern is only valid inside sequence patterns");
        return NULL;
    }
    if (syn->kind == IDM_SYN_WORD && strcmp(syn->as.text, "_") == 0) return idm_syn_pat_wildcard(syn->span);
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        return syntax_pattern_sequence(ctx, syn, err);
    }
    IdmSyntax *literal = idm_syn_clone(syn);
    if (!literal) {
        idm_error_oom(err, syn->span);
        return NULL;
    }
    IdmSyntaxPattern *pat = idm_syn_pat_literal_take(literal, syn->span);
    if (!pat) {
        idm_syn_free(literal);
        idm_error_oom(err, syn->span);
    }
    return pat;
}

static IdmPattern *pattern_from_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (!syn_is_form(syn, "%-quasisyntax") || syn->as.seq.count != 2) {
        idm_error_set(err, syn->span, "%-quasisyntax pattern expects one child");
        return NULL;
    }
    IdmSyntaxPattern *syntax = syntax_pattern_from_template(ctx, syn->as.seq.items[1], err);
    if (!syntax) return NULL;
    IdmPattern *pat = idm_pat_syntax_take(syntax, syn->span);
    if (!pat) {
        idm_syn_pat_free(syntax);
        idm_error_oom(err, syn->span);
    }
    return pat;
}

static bool quoted_list_pattern_items(const IdmSyntax *syn, IdmSyntax *const **out_items, size_t *out_count, IdmSpan *out_span) {
    if (!syn_is_form(syn, "%-quote") || syn->as.seq.count != 2) return false;
    const IdmSyntax *quoted = syn->as.seq.items[1];
    if (quoted->kind == IDM_SYN_LIST && quoted->as.seq.count == 0) {
        *out_items = NULL;
        *out_count = 0;
        *out_span = quoted->span;
        return true;
    }
    if ((syn_is_form(quoted, "%-group") || syn_is_form(quoted, "%-layout-group")) && quoted->as.seq.count == 2) {
        const IdmSyntax *expr = quoted->as.seq.items[1];
        if (syn_is_form(expr, "%-expr")) {
            *out_items = expr->as.seq.items + 1;
            *out_count = expr->as.seq.count - 1u;
            *out_span = quoted->span;
            return true;
        }
    }
    return false;
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
            idm_error_set(err, span, "list rest pattern must have form '(head ... . rest)");
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
    IdmSyntax *const *quoted_items = NULL;
    size_t quoted_count = 0;
    IdmSpan quoted_span = syn->span;
    if (quoted_list_pattern_items(syn, &quoted_items, &quoted_count, &quoted_span)) {
        return pattern_from_list_items(ctx, quoted_items, quoted_count, quoted_span, err);
    }
    if (syn_is_form(syn, "%-quote") && syn->as.seq.count == 2) {
        IdmValue datum = idm_nil();
        if (!expand_quote_datum(ctx, syn->as.seq.items[1], &datum, err)) return NULL;
        return idm_pat_literal(datum, syn->span);
    }
    if (syn_is_form(syn, "%-pin") && syn->as.seq.count == 2) {
        const IdmSyntax *target = syn->as.seq.items[1];
        if (target->kind != IDM_SYN_WORD) {
            idm_error_set(err, syn->span, "pin pattern currently supports only a variable: ^name");
            return NULL;
        }
        return idm_pat_pin(target->as.text, syn->span);
    }
    if (syn_is_form(syn, "%-quasisyntax")) return pattern_from_quasisyntax(ctx, syn, err);
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
        size_t elem_count = syn->as.seq.count;
        size_t dot_index = SIZE_MAX;
        for (size_t i = 0; i < elem_count; i++) {
            if (syn->as.seq.items[i]->kind == IDM_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                if (dot_index != SIZE_MAX) {
                    idm_error_set(err, syn->as.seq.items[i]->span, "dict rest pattern may contain only one dot");
                    return NULL;
                }
                dot_index = i;
            }
        }
        IdmPattern *rest = NULL;
        if (dot_index != SIZE_MAX) {
            if ((dot_index % 2u) != 0 || dot_index + 2u != elem_count) {
                idm_error_set(err, syn->span, "dict rest pattern must have form %{key value ... . rest}");
                return NULL;
            }
            rest = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index / 2u), false, err);
            if (!rest) return NULL;
            elem_count = dot_index;
        }
        if (elem_count % 2u != 0) {
            idm_pat_free(rest);
            idm_error_set(err, syn->span, "dict pattern requires key/value pairs");
            return NULL;
        }
        size_t count = elem_count / 2u;
        IdmDictPatternEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
        if (count != 0 && !entries) {
            idm_pat_free(rest);
            idm_error_oom(err, syn->span);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err)) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                idm_pat_free(rest);
                if (!err->present) idm_error_set(err, syn->as.seq.items[i * 2u]->span, "dict pattern key must be literal until expression-key pattern expansion is implemented");
                return NULL;
            }
            entries[i].pattern = pattern_from_param_depth(ctx, syn->as.seq.items[i * 2u + 1u], (uint32_t)i, false, err);
            if (!entries[i].pattern) {
                for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                free(entries);
                idm_pat_free(rest);
                return NULL;
            }
        }
        IdmPattern *pat = idm_pat_dict(entries, count, rest, syn->span);
        if (!pat) {
            for (size_t i = 0; i < count; i++) idm_pat_free(entries[i].pattern);
            free(entries);
            idm_pat_free(rest);
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
    if (!syn_is_form(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    if (form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_name = form->as.seq.items[1];
    *out_rhs_start = 3;
    return true;
}

static bool pattern_bind_form_parts(const IdmSyntax *form, const IdmSyntax **out_pattern, size_t *out_rhs_start) {
    if (!syn_is_form(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    const IdmSyntax *lhs = form->as.seq.items[1];
    if (lhs->kind == IDM_SYN_WORD) return false;
    if (lhs->kind == IDM_SYN_LIST && !syn_is_form(lhs, "%-quote") && !syn_is_form(lhs, "%-quasisyntax")) {
        return false;
    }
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    if (form->as.seq.count > 4 && form->as.seq.items[2]->token_adjacent_previous && form->as.seq.items[3]->token_adjacent_previous) return false;
    *out_pattern = lhs;
    *out_rhs_start = 3;
    return true;
}

static bool body_bind_is_env(ExpandContext *ctx) {
    return ctx->frame == IDM_FRAME_TOP && (ctx->repl_env_binds || ctx->in_package);
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
    CallableBindingInfo *bindings = NULL;
    bool ok = pattern != NULL;
    if (ok && count != 0) {
        names = malloc(count * sizeof(*names));
        slots = malloc(count * sizeof(*slots));
        bindings = malloc(count * sizeof(*bindings));
        ok = names != NULL && slots != NULL && bindings != NULL;
        if (ok) {
            memcpy(names, ctx->pat_binders, count * sizeof(*names));
            for (size_t i = 0; i < count; i++) {
                bindings[i].arity = idm_arity_unknown();
            }
        }
    }
    end_function_context(ctx, &probe);
    if (!ok) {
        free(names);
        free(slots);
        free(bindings);
        idm_pat_free(pattern);
        if (err && !err->present) idm_error_oom(err, pattern_syntax->span);
        return false;
    }
    bool env_bind = body_bind_is_env(ctx);
    for (size_t i = 0; i < count; i++) slots[i] = env_bind ? ctx->env_slot_seq++ : ctx->next_slot++;
    rec->kind = BODY_REC_BIND_PATTERN;
    rec->rhs_start = rhs_start;
    rec->pattern_syntax = pattern_syntax;
    rec->pattern = pattern;
    rec->pattern_names = names;
    rec->pattern_name_count = count;
    rec->pattern_slots = slots;
    rec->pattern_bindings = bindings;
    rec->pattern_tmp_slot = ctx->next_slot++;
    return true;
}

static bool definition_like_form(const IdmSyntax *form, const char **out_head) {
    if (!syn_is_form(form, "%-expr")) return false;
    if (form->as.seq.count < 2 || form->as.seq.items[1]->kind != IDM_SYN_WORD) return false;
    size_t base = 1;
    if (syn_is_word(form->as.seq.items[1], "export") && form->as.seq.count >= 3 && (syn_is_word(form->as.seq.items[2], "defn") || syn_is_word(form->as.seq.items[2], "defmacro"))) base = 2;
    const char *head = form->as.seq.items[base]->as.text;
    if (strcmp(head, "def") != 0 && strcmp(head, "defn") != 0 && strcmp(head, "defmacro") != 0) return false;
    *out_head = head;
    return true;
}

static const char *core_operator_text_arg(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD || syn->kind == IDM_SYN_ATOM || syn->kind == IDM_SYN_STRING) return syn->as.text;
    return NULL;
}

static bool core_operator_parts(const IdmSyntax *form, IdmSyntax *const **out_items, size_t *out_base, size_t *out_count) {
    if (!form || form->kind != IDM_SYN_LIST || form->as.seq.count == 0) return false;
    if ((syn_is_form(form, "%-group") || syn_is_form(form, "%-layout-group")) && form->as.seq.count == 2u) {
        return core_operator_parts(form->as.seq.items[1], out_items, out_base, out_count);
    }
    if (syn_is_form(form, "%-expr") && form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "core-operator")) {
        if (out_items) *out_items = form->as.seq.items;
        if (out_base) *out_base = 2u;
        if (out_count) *out_count = form->as.seq.count;
        return true;
    }
    if (form->as.seq.items[0]->kind == IDM_SYN_WORD && syn_is_word(form->as.seq.items[0], "core-operator")) {
        if (out_items) *out_items = form->as.seq.items;
        if (out_base) *out_base = 1u;
        if (out_count) *out_count = form->as.seq.count;
        return true;
    }
    return false;
}

static bool expand_core_operator_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    IdmSyntax *const *items = NULL;
    size_t base = 0;
    size_t count = 0;
    if (!core_operator_parts(form, &items, &base, &count)) return true;
    if (count != base + 5u) {
        return idm_error_set(err, form->span, "core-operator expects name, precedence, assoc, capture, and target");
    }
    const IdmSyntax *name_syntax = items[base];
    const IdmSyntax *precedence_syntax = items[base + 1u];
    const IdmSyntax *assoc_syntax = items[base + 2u];
    const IdmSyntax *capture_syntax = items[base + 3u];
    const IdmSyntax *target_syntax = items[base + 4u];
    char *name = surface_token_text_dup(name_syntax);
    if (!name) return idm_error_set(err, name_syntax->span, "operator name must be a symbol");
    if (precedence_syntax->kind != IDM_SYN_INT || precedence_syntax->as.integer < 0 || precedence_syntax->as.integer > 255) {
        free(name);
        return idm_error_set(err, precedence_syntax->span, "operator precedence must be an integer 0..255");
    }
    const char *assoc_text = core_operator_text_arg(assoc_syntax);
    IdmOpAssoc assoc;
    if (assoc_text && strcmp(assoc_text, "left") == 0) assoc = IDM_OP_ASSOC_LEFT;
    else if (assoc_text && strcmp(assoc_text, "right") == 0) assoc = IDM_OP_ASSOC_RIGHT;
    else if (assoc_text && strcmp(assoc_text, "none") == 0) assoc = IDM_OP_ASSOC_NONE;
    else {
        free(name);
        return idm_error_set(err, assoc_syntax->span, "operator assoc must be left, right, or none");
    }
    const char *capture = core_operator_text_arg(capture_syntax);
    if (!capture || !*capture) {
        free(name);
        return idm_error_set(err, capture_syntax->span, "operator capture must be non-empty");
    }
    char *target = surface_token_text_dup(target_syntax);
    if (!target) {
        free(name);
        return idm_error_set(err, target_syntax->span, "operator target must be an identifier");
    }

    IdmScopeSet decl_scopes;
    if (!surface_decl_scopes_pruned(ctx, name_syntax, &decl_scopes)) {
        free(name);
        free(target);
        return idm_error_oom(err, name_syntax->span);
    }
    IdmScopeSet target_scopes;
    if (!syntax_scopes_copy(&target_scopes, target_syntax)) {
        idm_scope_set_destroy(&decl_scopes);
        free(name);
        free(target);
        return idm_error_oom(err, target_syntax->span);
    }
    if (ctx->rt->macro_intro_active && !idm_scope_set_flip(&target_scopes, ctx->rt->macro_intro_scope)) {
        idm_scope_set_destroy(&decl_scopes);
        idm_scope_set_destroy(&target_scopes);
        free(name);
        free(target);
        return idm_error_oom(err, target_syntax->span);
    }
    bool ok = register_operator(ctx, name, capture, (uint8_t)precedence_syntax->as.integer, assoc, target, &target_scopes, &decl_scopes, ctx->unit, ctx->unit_key, true, err);
    idm_scope_set_destroy(&decl_scopes);
    idm_scope_set_destroy(&target_scopes);
    free(name);
    free(target);
    return ok;
}

static bool for_syntax_form(const IdmSyntax *form, const IdmSyntax **out_body) {
    if (!syn_is_form(form, "%-expr")) return false;
    if (form->as.seq.count != 3) return false;
    if (!syn_is_word(form->as.seq.items[1], "for-syntax")) return false;
    if (!syn_is_form(form->as.seq.items[2], "%-body")) return false;
    *out_body = form->as.seq.items[2];
    return true;
}

static bool core_grammar_args(const IdmSyntax *form, size_t *out_head) {
    if (syn_is_form(form, "%-expr") && form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "core-grammar")) {
        if (out_head) *out_head = 1u;
        return true;
    }
    if (form && (form->kind == IDM_SYN_LIST || form->kind == IDM_SYN_VECTOR || form->kind == IDM_SYN_TUPLE) && form->as.seq.count >= 1u && syn_is_word(form->as.seq.items[0], "core-grammar")) {
        if (out_head) *out_head = 0u;
        return true;
    }
    return false;
}

static bool surface_grammar_args(const IdmSyntax *form, size_t *out_head) {
    if (!syn_is_form(form, "%-expr") || form->as.seq.count < 2u) return false;
    if (syn_is_word(form->as.seq.items[1], "grammar")) {
        if (out_head) *out_head = 1u;
        return true;
    }
    if (form->as.seq.count >= 3u && syn_is_word(form->as.seq.items[1], "export") && syn_is_word(form->as.seq.items[2], "grammar")) {
        if (out_head) *out_head = 2u;
        return true;
    }
    return false;
}

static bool core_reader_args(const IdmSyntax *form, size_t *out_head) {
    if (syn_is_form(form, "%-expr") && form->as.seq.count >= 2u && syn_is_word(form->as.seq.items[1], "core-reader")) {
        if (out_head) *out_head = 1u;
        return true;
    }
    if (form && (form->kind == IDM_SYN_LIST || form->kind == IDM_SYN_VECTOR || form->kind == IDM_SYN_TUPLE) && form->as.seq.count >= 1u && syn_is_word(form->as.seq.items[0], "core-reader")) {
        if (out_head) *out_head = 0u;
        return true;
    }
    return false;
}

static bool grammar_append_take(IdmSyntax *seq, IdmSyntax *item, IdmSpan span, IdmError *err) {
    if (item && idm_syn_append(seq, item)) return true;
    idm_syn_free(item);
    return idm_error_oom(err, span);
}

static bool surface_grammar_mode_atom(const IdmSyntax *mode, IdmSyntax **out, IdmError *err) {
    const char *text = NULL;
    if (mode && mode->kind == IDM_SYN_WORD) text = mode->as.text;
    if (!text || (strcmp(text, "extend") != 0 && strcmp(text, "exclusive") != 0)) {
        return idm_error_set(err, mode ? mode->span : idm_span_unknown(NULL), "grammar mode must be extend or exclusive");
    }
    *out = idm_syn_atom(text, mode->span);
    return *out != NULL || idm_error_oom(err, mode->span);
}

static bool surface_grammar_arrow(const IdmSyntax *syn) {
    return syn_is_word(syn, "->");
}

static bool surface_grammar_tuple_rule(const char *kind, const IdmSyntax *name, const IdmSyntax *pattern, const IdmSyntax *constructor, IdmSpan span, IdmSyntax **out, IdmError *err) {
    IdmSyntax *rule = idm_syn_tuple(span);
    if (!rule) return idm_error_oom(err, span);
    if (!grammar_append_take(rule, idm_syn_atom(kind, span), span, err) ||
        !grammar_append_take(rule, idm_syn_clone(name), name ? name->span : span, err) ||
        !grammar_append_take(rule, idm_syn_clone(pattern), pattern ? pattern->span : span, err) ||
        (constructor && !grammar_append_take(rule, idm_syn_clone(constructor), constructor->span, err))) {
        idm_syn_free(rule);
        return false;
    }
    *out = rule;
    return true;
}

static bool surface_grammar_rule_from_body_item(const IdmSyntax *item, IdmSyntax **out, IdmError *err) {
    *out = NULL;
    if (!item) return idm_error_set(err, idm_span_unknown(NULL), "grammar body expects rule tuples or skip/token/form rule lines");
    if (item->kind == IDM_SYN_TUPLE) {
        *out = idm_syn_clone(item);
        return *out != NULL || idm_error_oom(err, item->span);
    }
    if (syn_is_form(item, "%-expr") && item->as.seq.count == 2u && item->as.seq.items[1]->kind == IDM_SYN_TUPLE) {
        *out = idm_syn_clone(item->as.seq.items[1]);
        return *out != NULL || idm_error_oom(err, item->as.seq.items[1]->span);
    }
    if (!syn_is_form(item, "%-expr") || item->as.seq.count < 2u) {
        return idm_error_set(err, item->span, "grammar body expects rule tuples or skip/token/form rule lines");
    }
    const IdmSyntax *kind = item->as.seq.items[1];
    if (syn_is_word(kind, "skip")) {
        if (item->as.seq.count != 4u) {
            return idm_error_set(err, item->span, "grammar skip rule expects: skip NAME PATTERN");
        }
        return surface_grammar_tuple_rule("skip", item->as.seq.items[2], item->as.seq.items[3], NULL, item->span, out, err);
    }
    if (syn_is_word(kind, "token") || syn_is_word(kind, "form")) {
        if (item->as.seq.count != 6u || !surface_grammar_arrow(item->as.seq.items[4])) {
            return idm_error_set(err, item->span, syn_is_word(kind, "token") ? "grammar token rule expects: token NAME PATTERN -> CONSTRUCTOR" : "grammar form rule expects: form NAME PATTERN -> CONSTRUCTOR");
        }
        return surface_grammar_tuple_rule(kind->as.text, item->as.seq.items[2], item->as.seq.items[3], item->as.seq.items[5], item->span, out, err);
    }
    return idm_error_set(err, item->span, "grammar body expects rule tuples or skip/token/form rule lines");
}

static bool expand_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    size_t head = 0;
    if (!core_grammar_args(form, &head)) {
        return idm_error_set(err, form->span, "core-grammar expects: core-grammar NAME :extend|:exclusive [RULE ...]");
    }
    if (ctx->phase != 0) {
        return idm_error_set(err, form->span, "core-grammar declarations are phase-0 reader artifact declarations");
    }
    IdmSyntax shifted;
    const IdmSyntax *ir = form;
    if (head != 0) {
        shifted = *form;
        shifted.as.seq.items = form->as.seq.items + head;
        shifted.as.seq.count = form->as.seq.count - head;
        ir = &shifted;
    }
    IdmPkgGrammar grammar;
    if (!idm_pkg_grammar_from_ir(ir, &grammar, err)) return false;
    const IdmSyntax *name = ir->as.seq.items[1];
    IdmGrammarRule *rules = grammar.rules;
    size_t rule_count = grammar.rule_count;
    uint8_t mode = grammar.mode;
    grammar.rules = NULL;
    grammar.rule_count = 0;
    bool ok = register_grammar(ctx, name, mode, rules, rule_count, true, err);
    idm_pkg_grammar_destroy(&grammar);
    return ok;
}

static bool expand_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    size_t head = 0;
    if (!surface_grammar_args(form, &head) || form->as.seq.count != head + 4u || form->as.seq.items[head + 1u]->kind != IDM_SYN_WORD ||
        !syn_is_form(form->as.seq.items[head + 3u], "%-body")) {
        return idm_error_set(err, form ? form->span : idm_span_unknown(NULL), "grammar expects: grammar NAME extend|exclusive do RULE ... end");
    }
    if (ctx->phase != 0) {
        return idm_error_set(err, form->span, "core-grammar declarations are phase-0 reader artifact declarations");
    }
    IdmSyntax *ir = idm_syn_list(form->span);
    IdmSyntax *rules = idm_syn_vector(form->as.seq.items[head + 3u]->span);
    if (!ir || !rules) {
        idm_syn_free(ir);
        idm_syn_free(rules);
        return idm_error_oom(err, form->span);
    }
    IdmSyntax *mode = NULL;
    bool ok = grammar_append_take(ir, idm_syn_word("core-grammar", form->as.seq.items[head]->span), form->span, err) &&
              grammar_append_take(ir, idm_syn_clone(form->as.seq.items[head + 1u]), form->as.seq.items[head + 1u]->span, err) &&
              surface_grammar_mode_atom(form->as.seq.items[head + 2u], &mode, err);
    if (ok) {
        ok = grammar_append_take(ir, mode, form->as.seq.items[head + 2u]->span, err);
        mode = NULL;
    }
    if (!ok) {
        idm_syn_free(mode);
        idm_syn_free(rules);
        idm_syn_free(ir);
        return false;
    }
    const IdmSyntax *body = form->as.seq.items[head + 3u];
    for (size_t i = 1u; i < body->as.seq.count; i++) {
        IdmSyntax *rule = NULL;
        if (!surface_grammar_rule_from_body_item(body->as.seq.items[i], &rule, err) ||
            !grammar_append_take(rules, rule, body->as.seq.items[i]->span, err)) {
            idm_syn_free(rules);
            idm_syn_free(ir);
            return false;
        }
    }
    if (!grammar_append_take(ir, rules, rules->span, err)) {
        idm_syn_free(ir);
        return false;
    }
    ok = expand_core_grammar_decl(ctx, ir, err);
    idm_syn_free(ir);
    return ok;
}

static bool register_source_reader(ExpandContext *ctx, char *name, IdmSpan span, IdmError *err) {
    if (ctx->phase != 0) {
        free(name);
        return idm_error_set(err, span, "core-reader declarations are phase-0 reader artifact declarations");
    }
    if (ctx->decl_source_reader_count == ctx->decl_source_reader_cap) {
        if (!idm_grow((void **)&ctx->decl_source_readers, &ctx->decl_source_reader_cap, sizeof(*ctx->decl_source_readers), 4u, ctx->decl_source_reader_count + 1u)) {
            free(name);
            return idm_error_oom(err, span);
        }
    }
    SourceReaderDecl *decl = &ctx->decl_source_readers[ctx->decl_source_reader_count];
    decl->name = name;
    decl->span = span;
    ctx->decl_source_reader_count++;
    return true;
}

static bool expand_core_reader_decl(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    size_t head = 0;
    if (!core_reader_args(form, &head)) {
        return idm_error_set(err, form->span, "core-reader expects: core-reader :source NAME");
    }
    if (ctx->phase != 0) {
        return idm_error_set(err, form->span, "core-reader declarations are phase-0 reader artifact declarations");
    }
    IdmSyntax shifted;
    const IdmSyntax *ir = form;
    if (head != 0) {
        shifted = *form;
        shifted.as.seq.items = form->as.seq.items + head;
        shifted.as.seq.count = form->as.seq.count - head;
        ir = &shifted;
    }
    char *name = NULL;
    if (!idm_pkg_source_reader_from_ir(ir, &name, err)) return false;
    return register_source_reader(ctx, name, ir->as.seq.items[2]->span, err);
}

static bool defn_form_parts(const IdmSyntax *form, const IdmSyntax **out_name, size_t *out_param_start, bool *out_export) {
    if (!syn_is_form(form, "%-expr")) return false;
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
        if (!idm_grow((void **)groups, cap, sizeof(**groups), 4u, *count + 1u)) return NULL;
    }
    DefnGroup *group = &(*groups)[(*count)++];
    group->name = name_syntax->as.text;
    group->name_syntax = name_syntax;
    if (!syntax_scopes_copy(&group->scopes, name_syntax)) return NULL;
    group->slot = UINT32_MAX;
    group->arity = idm_arity_unknown();
    group->indices = NULL;
    group->count = 0;
    group->cap = 0;
    group->exported = false;
    return group;
}

static bool group_add_index(DefnGroup *group, size_t index) {
    if (group->count == group->cap) {
        if (!idm_grow((void **)&group->indices, &group->cap, sizeof(*group->indices), 4u, group->count + 1u)) return false;
    }
    group->indices[group->count++] = index;
    return true;
}

bool record_package_slot(ExpandContext *ctx, const char *name, uint32_t slot_id, const IdmScopeSet *scopes, IdmArity arity, bool exported) {
    if (!ctx->in_package) return true;
    for (size_t i = 0; i < ctx->package_slot_count; i++) {
        if (ctx->package_slots[i].slot == slot_id && strcmp(ctx->package_slots[i].name, name) == 0) {
            if (arity.kind != IDM_ARITY_UNKNOWN) ctx->package_slots[i].arity = arity;
            if (exported) ctx->package_slots[i].exported = true;
            return true;
        }
    }
    if (ctx->package_slot_count == ctx->package_slot_cap) {
        if (!idm_grow((void **)&ctx->package_slots, &ctx->package_slot_cap, sizeof(*ctx->package_slots), 8u, ctx->package_slot_count + 1u)) return false;
    }
    IdmPkgSlot *entry = &ctx->package_slots[ctx->package_slot_count];
    entry->name = idm_strdup(name);
    if (!entry->name) return false;
    entry->slot = slot_id;
    entry->arity = arity;
    entry->exported = exported;
    if (!idm_scope_set_copy(&entry->scopes, scopes)) {
        free(entry->name);
        entry->name = NULL;
        return false;
    }
    ctx->package_slot_count++;
    return true;
}

static bool body_existing_env_binding(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmBinding **out_binding) {
    for (size_t i = ctx->bindings.count; i > 0; i--) {
        const IdmBinding *binding = &ctx->bindings.items[i - 1u];
        if ((binding->phase != ctx->phase && binding->phase != IDM_PHASE_ANY) || binding->space != IDM_BIND_SPACE_DEFAULT ||
            binding->kind != IDM_BIND_ENV || strcmp(binding->name, name) != 0) {
            continue;
        }
        if (!idm_scope_set_equal(&binding->scopes, scopes)) continue;
        if (out_binding) *out_binding = binding;
        return true;
    }
    return false;
}

static bool body_env_def_binder_with_arity(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, IdmArity arity, bool reuse_existing, uint32_t *out_id, bool *out_created, IdmError *err) {
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    const IdmBinding *existing = NULL;
    if (reuse_existing && body_existing_env_binding(ctx, name, &scopes, &existing)) {
        if (out_id) *out_id = existing->payload;
        if (out_created) *out_created = false;
        idm_scope_set_destroy(&scopes);
        return true;
    }
    uint32_t id = ctx->env_slot_seq++;
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name, ctx->phase, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, id, IDM_FRAME_ENV, arity, NULL);
    idm_scope_set_destroy(&scopes);
    if (!ok) return idm_error_oom(err, name_syntax->span);
    if (out_id) *out_id = id;
    if (out_created) *out_created = true;
    return true;
}

static void body_recs_destroy(BodyRec *recs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (recs[i].kind == BODY_REC_GROUPS) defn_groups_destroy(recs[i].groups, recs[i].group_count);
        if (recs[i].kind == BODY_REC_BIND_PATTERN) {
            idm_pat_free(recs[i].pattern);
            free(recs[i].pattern_names);
            free(recs[i].pattern_slots);
            free(recs[i].pattern_bindings);
        }
        idm_core_free(recs[i].core);
    }
    free(recs);
}

static IdmArity core_callable_arity(const IdmCore *core) {
    if (!core) return idm_arity_unknown();
    if (core->kind == IDM_CORE_FN) return idm_arity_exact(core->as.fn.arity);
    if (core->kind != IDM_CORE_FN_MULTI) return idm_arity_unknown();
    IdmArity arity = idm_arity_unknown();
    for (size_t i = 0; i < core->as.fn_multi.count; i++) {
        const IdmFnClause *clause = &core->as.fn_multi.clauses[i];
        const IdmArity *clause_arity = clause->primitive_backed ? &clause->call_arity : NULL;
        if (clause_arity && clause_arity->kind != IDM_ARITY_UNKNOWN) {
            if (arity.kind == IDM_ARITY_UNKNOWN) {
                arity = *clause_arity;
            } else if (arity.kind == IDM_ARITY_RANGE || clause_arity->kind == IDM_ARITY_RANGE) {
                uint32_t min = arity.min < clause_arity->min ? arity.min : clause_arity->min;
                uint32_t max = arity.max > clause_arity->max ? arity.max : clause_arity->max;
                arity = idm_arity_range(min, max);
            } else {
                for (uint32_t argc = clause_arity->min; argc <= clause_arity->max; argc++) {
                    if (idm_arity_accepts(clause_arity, argc) && !idm_arity_add_exact(&arity, argc)) return idm_arity_unknown();
                    if (argc == UINT32_MAX) break;
                }
            }
            continue;
        }
        if (!idm_arity_add_exact(&arity, clause->arity)) return idm_arity_unknown();
    }
    return arity;
}

static bool syntax_callable_value_binding_info(ExpandContext *ctx, const IdmSyntax *syn, IdmArity *out, IdmError *err) {
    (void)ctx;
    if (syn_is_form(syn, "%-expr") && syn->as.seq.count >= 2u && syn_is_word(syn->as.seq.items[1], "fn")) {
        return function_literal_arity(syn->as.seq.items[1], syn->as.seq.items, 2u, syn->as.seq.count, out, err);
    }
    return false;
}

static bool rhs_callable_value_binding_info(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmArity *out, IdmError *err) {
    if (start < end && syn_is_word(items[start], "fn")) {
        return function_literal_arity(items[start], items, start + 1u, end, out, err);
    }
    if (start + 1u != end) return false;
    return syntax_callable_value_binding_info(ctx, items[start], out, err);
}

static bool pattern_bind_infos_walk(ExpandContext *ctx, const IdmSyntax *pattern, const IdmSyntax *rhs, CallableBindingInfo *bindings, size_t count, size_t *index, IdmError *err) {
    if (pattern->kind == IDM_SYN_WORD) {
        if (strcmp(pattern->as.text, "_") != 0 && *index < count) {
            IdmArity arity = idm_arity_unknown();
            if (syntax_callable_value_binding_info(ctx, rhs, &arity, err)) {
                bindings[*index].arity = arity;
            }
            *index += 1u;
        }
        return true;
    }
    if ((pattern->kind == IDM_SYN_TUPLE || pattern->kind == IDM_SYN_VECTOR || pattern->kind == IDM_SYN_LIST) && pattern->kind == rhs->kind && pattern->as.seq.count == rhs->as.seq.count) {
        for (size_t i = 0; i < pattern->as.seq.count; i++) {
            if (!pattern_bind_infos_walk(ctx, pattern->as.seq.items[i], rhs->as.seq.items[i], bindings, count, index, err)) return false;
        }
    }
    return true;
}

static bool infer_pattern_bind_infos(ExpandContext *ctx, BodyRec *rec, IdmError *err) {
    if (!rec->pattern_bindings || rec->rhs_start + 1u != rec->form->as.seq.count) return true;
    size_t index = 0;
    return pattern_bind_infos_walk(ctx, rec->pattern_syntax, rec->form->as.seq.items[rec->rhs_start], rec->pattern_bindings, rec->pattern_name_count, &index, err);
}

static size_t core_callable_capture_count(const IdmCore *core) {
    if (!core) return 0;
    if (core->kind == IDM_CORE_FN) return core->as.fn.capture_count;
    if (core->kind == IDM_CORE_FN_MULTI) return core->as.fn_multi.capture_count;
    return 0;
}

static bool update_existing_env_binding(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, uint32_t slot, IdmArity arity) {
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        IdmBinding *binding = &ctx->bindings.items[i];
        if (binding->kind != IDM_BIND_ENV || binding->space != IDM_BIND_SPACE_DEFAULT || binding->payload != slot) continue;
        if (strcmp(binding->name, name) != 0 || !idm_scope_set_equal(&binding->scopes, scopes)) continue;
        binding->arity = arity;
        return true;
    }
    return false;
}

static bool push_bind_binder(ExpandContext *ctx, const IdmSyntax *name_syntax, uint32_t slot, IdmArity arity, IdmError *err) {
    bool env_bind = body_bind_is_env(ctx);
    IdmScopeSet scopes;
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return false;
    const IdmBinding *existing = NULL;
    if (env_bind && body_existing_env_binding(ctx, name_syntax->as.text, &scopes, &existing)) {
        bool ok = existing->payload == slot;
        if (ok) ok = update_existing_env_binding(ctx, name_syntax->as.text, &scopes, slot, arity);
        if (ok && ctx->in_package) ok = record_package_slot(ctx, name_syntax->as.text, slot, &scopes, arity, false);
        idm_scope_set_destroy(&scopes);
        return ok || idm_error_oom(err, name_syntax->span);
    }
    bool ok = idm_binding_table_add_with_arity(&ctx->bindings, name_syntax->as.text, ctx->phase, IDM_BIND_SPACE_DEFAULT, env_bind ? IDM_BIND_ENV : IDM_BIND_LOCAL, &scopes, slot, env_bind ? IDM_FRAME_ENV : ctx->frame, arity, NULL);
    if (ok && env_bind && ctx->in_package) ok = record_package_slot(ctx, name_syntax->as.text, slot, &scopes, arity, false);
    idm_scope_set_destroy(&scopes);
    return ok || idm_error_oom(err, name_syntax->span);
}

static IdmCore *pattern_extract_value(BodyRec *rec, size_t index, IdmError *err) {
    IdmSpan span = rec->pattern_names[index]->span;
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_TUPLE_GET, span, err);
    if (!call) return NULL;
    if (!core_call_add_arg_or_free(call, idm_core_local_ref("_match", rec->pattern_tmp_slot, span), err, span)) return NULL;
    if (!core_call_add_arg_or_free(call, idm_core_literal(idm_int((int64_t)index), span), err, span)) return NULL;
    return call;
}

static IdmCore *pattern_bind_call(BodyRec *rec, IdmCore *rhs, IdmError *err) {
    IdmSpan span = rec->pattern_syntax->span;
    IdmCore *body = expand_primitive_clause_call(IDM_PRIM_TUPLE, span, err);
    if (!body) {
        idm_core_free(rhs);
        return NULL;
    }
    for (size_t i = 0; i < rec->pattern_name_count; i++) {
        IdmCore *ref = idm_core_local_ref(rec->pattern_names[i]->as.text, (uint32_t)i, rec->pattern_names[i]->span);
        if (!core_call_add_arg_or_free(body, ref, err, span)) {
            idm_core_free(rhs);
            return NULL;
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
    IdmCore *call = idm_core_call(fn, rec->form->span);
    if (!call) {
        idm_core_free(fn);
        idm_core_free(rhs);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, rec->form->span);
    }
    if (!core_call_add_arg_or_free(call, rhs, err, rec->form->span)) return NULL;
    return call;
}

static IdmCore *build_macro_registration(ExpandContext *ctx, const IdmSyntax *name_syntax, IdmCore *fn, IdmSpan span, IdmError *err) {
    IdmValue name_value = idm_syntax_value(ctx->rt, name_syntax, err);
    if (err && err->present) {
        idm_core_free(fn);
        return NULL;
    }
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_INTERNAL_REGISTER_MACRO, span, err);
    if (!call) {
        idm_core_free(fn);
        return NULL;
    }
    if (!core_call_add_arg_or_free(call, idm_core_literal(name_value, span), err, span)) { idm_core_free(fn); return NULL; }
    if (!core_call_add_arg_or_free(call, fn, err, span)) return NULL;
    return call;
}

static bool body_work_splice(IdmSyntax ***work, size_t *work_count, size_t *work_cap, size_t at, const IdmSyntax *body, IdmError *err) {
    size_t add = body->as.seq.count - 1u;
    size_t needed = *work_count - 1u + add;
    if (needed > *work_cap) {
        if (!idm_grow((void **)work, work_cap, sizeof(**work), 8u, needed)) return idm_error_oom(err, body->span);
    }
    memmove(*work + at + add, *work + at + 1u, (*work_count - at - 1u) * sizeof(**work));
    for (size_t k = 0; k < add; k++) (*work)[at + k] = body->as.seq.items[1u + k];
    *work_count = needed;
    return true;
}

static bool body_scope_add_range(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err) {
    (void)ctx;
    for (size_t i = start; i < count; i++) {
        if (idm_syn_scope_contains(work[i], 0, scope)) continue;
        if (!idm_syn_scope_add_tree(work[i], 0, scope)) return idm_error_oom(err, span);
    }
    return true;
}

static bool body_definition_scope_add(ExpandContext *ctx, IdmSyntax **work, size_t start, size_t count, IdmScopeId scope, IdmSpan span, IdmError *err) {
    if (!body_scope_add_range(ctx, work, start, count, scope, span, err)) return false;
    for (ScopePropagation *p = ctx->scope_propagation; p != NULL; p = p->prev) {
        if (!body_scope_add_range(ctx, p->work, p->start, p->count, scope, span, err)) return false;
    }
    return true;
}

static void use_selection_destroy(UseSelection *selection) {
    if (!selection) return;
    free(selection->names);
    free(selection->spans);
    free(selection->matched);
    selection->names = NULL;
    selection->spans = NULL;
    selection->matched = NULL;
    selection->count = 0;
}

static bool use_selection_parse(const IdmSyntax *form, size_t start, size_t end, size_t *out_pos, UseSelection *selection, IdmError *err) {
    selection->names = NULL;
    selection->spans = NULL;
    selection->matched = NULL;
    selection->count = 0;
    *out_pos = start;
    if (start >= end || !syn_is_word(form->as.seq.items[start], "with")) return true;
    if (start + 2u > end || form->as.seq.items[start + 1u]->kind != IDM_SYN_VECTOR) {
        return idm_error_set(err, form->span, "use selection expects: with [name ...]");
    }
    const IdmSyntax *items = form->as.seq.items[start + 1u];
    if (items->as.seq.count == 0) {
        return idm_error_set(err, items->span, "use selection requires at least one name");
    }
    const char **names = calloc(items->as.seq.count, sizeof(*names));
    IdmSpan *spans = calloc(items->as.seq.count, sizeof(*spans));
    bool *matched = calloc(items->as.seq.count, sizeof(*matched));
    if (!names || !spans || !matched) {
        free(names);
        free(spans);
        free(matched);
        return idm_error_oom(err, items->span);
    }
    for (size_t i = 0; i < items->as.seq.count; i++) {
        const IdmSyntax *name = items->as.seq.items[i];
        if (name->kind != IDM_SYN_WORD) {
            free(names);
            free(spans);
            free(matched);
            return idm_error_set(err, name->span, "use selection entries must be names");
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(names[j], name->as.text) == 0) {
                free(names);
                free(spans);
                free(matched);
                return idm_error_set(err, name->span, "use selection repeats name '%s'", name->as.text);
            }
        }
        names[i] = name->as.text;
        spans[i] = name->span;
    }
    selection->names = names;
    selection->spans = spans;
    selection->matched = matched;
    selection->count = items->as.seq.count;
    *out_pos = start + 2u;
    return true;
}

typedef enum {
    BODY_DECL_NONE,
    BODY_DECL_PACKAGE,
    BODY_DECL_USE,
    BODY_DECL_IMPORT,
    BODY_DECL_PROTOCOL,
    BODY_DECL_ACTIVATE,
    BODY_DECL_FOR_SYNTAX,
    BODY_DECL_CORE_SYNTAX,
    BODY_DECL_CORE_OPERATOR,
    BODY_DECL_CORE_GRAMMAR,
    BODY_DECL_GRAMMAR,
    BODY_DECL_CORE_READER,
    BODY_DECL_RECORD,
    BODY_DECL_TRAIT,
    BODY_DECL_IMPLEMENT,
    BODY_DECL_METHOD
} BodyDeclKind;

enum {
    BODY_DECL_BOUNDARY = 1u << 0,
    BODY_DECL_EXPORT_BOUNDARY = 1u << 1,
    BODY_DECL_EXPORT_RANK = 1u << 2
};

typedef struct {
    const char *name;
    BodyDeclKind kind;
    int rank;
    unsigned flags;
    IdmCore *(*handler)(ExpandContext *ctx, const IdmSyntax *form, IdmSyntax *const *items, size_t index, size_t count, IdmError *err);
} BodyDeclDesc;

static IdmCore *boundary_activate_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_for_syntax_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_core_syntax_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_core_operator_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_core_reader_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_use_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);
static IdmCore *boundary_import_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err);

static const BodyDeclDesc BODY_DECLS[] = {
    { "package", BODY_DECL_PACKAGE, 0, 0, NULL },
    { "use", BODY_DECL_USE, 1, BODY_DECL_BOUNDARY, boundary_use_decl },
    { "import", BODY_DECL_IMPORT, 1, BODY_DECL_BOUNDARY, boundary_import_decl },
    { "protocol", BODY_DECL_PROTOCOL, 2, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_BOUNDARY | BODY_DECL_EXPORT_RANK, expand_protocol_decl },
    { "activate", BODY_DECL_ACTIVATE, 3, BODY_DECL_BOUNDARY, boundary_activate_decl },
    { "for-syntax", BODY_DECL_FOR_SYNTAX, 4, BODY_DECL_BOUNDARY, boundary_for_syntax_decl },
    { "core-syntax", BODY_DECL_CORE_SYNTAX, 4, BODY_DECL_BOUNDARY, boundary_core_syntax_decl },
    { "core-operator", BODY_DECL_CORE_OPERATOR, 4, BODY_DECL_BOUNDARY, boundary_core_operator_decl },
    { "core-grammar", BODY_DECL_CORE_GRAMMAR, 4, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_RANK, boundary_core_grammar_decl },
    { "grammar", BODY_DECL_GRAMMAR, 4, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_BOUNDARY | BODY_DECL_EXPORT_RANK, boundary_surface_grammar_decl },
    { "core-reader", BODY_DECL_CORE_READER, 4, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_RANK, boundary_core_reader_decl },
    { "record", BODY_DECL_RECORD, 5, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_BOUNDARY | BODY_DECL_EXPORT_RANK, expand_record_decl },
    { "trait", BODY_DECL_TRAIT, 6, BODY_DECL_BOUNDARY | BODY_DECL_EXPORT_BOUNDARY | BODY_DECL_EXPORT_RANK, expand_trait_decl },
    { "implement", BODY_DECL_IMPLEMENT, 7, BODY_DECL_BOUNDARY, expand_implement_trait_decl },
    { "method", BODY_DECL_METHOD, 10, BODY_DECL_BOUNDARY, expand_method_decl },
};

static const BodyDeclDesc *body_decl_find(const char *name) {
    for (size_t i = 0; i < sizeof(BODY_DECLS) / sizeof(BODY_DECLS[0]); i++) {
        if (strcmp(BODY_DECLS[i].name, name) == 0) return &BODY_DECLS[i];
    }
    return NULL;
}

static const BodyDeclDesc *package_expr_head_index(const IdmSyntax *form, unsigned export_flag, size_t *out_head_index) {
    if (!syn_is_form(form, "%-expr") || form->as.seq.count < 2u || form->as.seq.items[1]->kind != IDM_SYN_WORD) return NULL;
    size_t head_index = 1u;
    const IdmSyntax *head = form->as.seq.items[1];
    bool exported = false;
    if (syn_is_word(head, "export")) {
        if (form->as.seq.count < 3u || form->as.seq.items[2]->kind != IDM_SYN_WORD) return NULL;
        head_index = 2u;
        head = form->as.seq.items[2];
        exported = true;
    }
    const BodyDeclDesc *desc = body_decl_find(head->as.text);
    if (!desc) return NULL;
    if (exported && (desc->flags & export_flag) == 0) return NULL;
    if (out_head_index) *out_head_index = head_index;
    return desc;
}

static const BodyDeclDesc *body_decl_form(const IdmSyntax *form, unsigned export_flag) {
    const BodyDeclDesc *desc = package_expr_head_index(form, export_flag, NULL);
    if (desc) return desc;
    if (core_operator_parts(form, NULL, NULL, NULL)) return body_decl_find("core-operator");
    return NULL;
}

static bool body_boundary_form(const IdmSyntax *form) {
    const BodyDeclDesc *desc = body_decl_form(form, BODY_DECL_EXPORT_BOUNDARY);
    return desc && (desc->flags & BODY_DECL_BOUNDARY) != 0;
}

static bool body_eval_for_syntax(ExpandContext *ctx, const IdmSyntax *form, const IdmSyntax *for_syntax_body, ScopePropagation *propagation, IdmError *err) {
    int saved_phase = ctx->phase;
    int saved_surface_phase = ctx->surface_phase;
    ctx->phase = 1;
    if (ctx->surface_phase < 0) ctx->surface_phase = saved_phase;
    if (propagation) {
        propagation->prev = ctx->scope_propagation;
        ctx->scope_propagation = propagation;
    }
    IdmCore *ignored = expand_body_items(ctx, for_syntax_body->as.seq.items, 1, for_syntax_body->as.seq.count, false, err);
    if (ignored) ignored = wrap_kernel_use(ctx, ignored, err);
    if (ignored) ignored = wrap_phase_runtime_inits(ctx, ignored, err);
    if (propagation) ctx->scope_propagation = propagation->prev;
    bool ran = ignored != NULL && run_phase_core(ctx, ignored, err);
    if (ran) ctx->phase_runtime_init_mark_count = ctx->runtime_init_mark_count;
    ctx->phase = saved_phase;
    ctx->surface_phase = saved_surface_phase;
    idm_core_free(ignored);
    if (!ran && err && err->present) {
        idm_error_note(err, "during for-syntax evaluation (%s:%u:%u)", form->span.file ? form->span.file : "<unknown>", form->span.line, form->span.column);
    }
    return ran;
}

static bool package_note_value_slot(ExpandContext *ctx, const char *name, const IdmSyntax *name_syntax, uint32_t slot, IdmArity arity, bool exported, IdmError *err) {
    if (!ctx->in_package || ctx->frame != IDM_FRAME_TOP) return true;
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    if (!binder_scopes_pruned(ctx, name_syntax, &scopes)) return idm_error_oom(err, name_syntax->span);
    bool ok = record_package_slot(ctx, name, slot, &scopes, arity, exported);
    idm_scope_set_destroy(&scopes);
    return ok || idm_error_oom(err, name_syntax->span);
}

static bool package_prepare_values(ExpandContext *ctx, IdmSyntax **work, size_t work_count, bool *changed, IdmError *err) {
    DefnGroup *groups = NULL;
    size_t group_count = 0;
    size_t group_cap = 0;
    bool ok = true;
    for (size_t i = 0; i < work_count && ok; i++) {
        const IdmSyntax *def_name = NULL;
        size_t ignored_start = 0;
        bool exported = false;
        if (!defn_form_parts(work[i], &def_name, &ignored_start, &exported)) continue;
        DefnGroup *group = find_or_add_group(&groups, &group_count, &group_cap, def_name);
        if (!group || !group_add_index(group, i)) {
            ok = idm_error_oom(err, work[i]->span);
            break;
        }
        if (exported) group->exported = true;
    }
    for (size_t i = 0; i < group_count && ok; i++) {
        if (!defn_group_arity(&groups[i], work, &groups[i].arity, err)) {
            ok = false;
            break;
        }
        bool created = false;
        ok = body_env_def_binder_with_arity(ctx, groups[i].name, groups[i].name_syntax, groups[i].arity, true, &groups[i].slot, &created, err);
        if (!ok) break;
        if (created) {
            *changed = true;
        }
        ok = package_note_value_slot(ctx, groups[i].name, groups[i].name_syntax, groups[i].slot, groups[i].arity, groups[i].exported, err);
    }
    for (size_t i = 0; i < work_count && ok; i++) {
        const IdmSyntax *bind_name = NULL;
        size_t rhs_start = 0;
        if (!bind_form_parts(work[i], &bind_name, &rhs_start)) continue;
        (void)rhs_start;
        uint32_t slot = 0;
        bool created = false;
        ok = body_env_def_binder_with_arity(ctx, bind_name->as.text, bind_name, idm_arity_unknown(), true, &slot, &created, err);
        if (!ok) break;
        if (created) {
            *changed = true;
        }
        ok = package_note_value_slot(ctx, bind_name->as.text, bind_name, slot, idm_arity_unknown(), false, err);
    }
    defn_groups_destroy(groups, group_count);
    return ok;
}

static int package_work_rank(const IdmSyntax *form) {
    const BodyDeclDesc *desc = body_decl_form(form, BODY_DECL_EXPORT_RANK);
    if (desc) return desc->rank;
    if (bind_form_parts(form, &(const IdmSyntax *){NULL}, &(size_t){0})) return 8;
    const char *definition_head = NULL;
    if (definition_like_form(form, &definition_head) && strcmp(definition_head, "defmacro") != 0) return 9;
    return 10;
}

static bool package_trait_decl_parts(const IdmSyntax *form, const IdmSyntax **out_name, const IdmSyntax **out_body) {
    size_t head = 0;
    const BodyDeclDesc *desc = package_expr_head_index(form, BODY_DECL_EXPORT_BOUNDARY, &head);
    if (!desc || desc->kind != BODY_DECL_TRAIT) return false;
    if (form->as.seq.count != head + 3u ||
        form->as.seq.items[head + 1u]->kind != IDM_SYN_WORD || !syn_is_form(form->as.seq.items[head + 2u], "%-body")) {
        return false;
    }
    if (out_name) *out_name = form->as.seq.items[head + 1u];
    if (out_body) *out_body = form->as.seq.items[head + 2u];
    return true;
}

static bool package_syntax_identity_matches(const IdmSyntax *syntax, const char *identity) {
    return syntax && syntax->kind == IDM_SYN_WORD && identity && strcmp(syntax->as.text, identity) == 0;
}

static bool package_syntax_pair_matches(const IdmSyntax *a, const char *a_identity, const IdmSyntax *b, const char *b_identity) {
    return package_syntax_identity_matches(a, a_identity) && package_syntax_identity_matches(b, b_identity);
}

static bool package_local_trait_decl_seen(IdmSyntax **work, const bool *placed, size_t work_count, const char *name, bool require_placed) {
    for (size_t i = 0; i < work_count; i++) {
        if (require_placed && (!placed || !placed[i])) continue;
        const IdmSyntax *decl_name = NULL;
        if (package_trait_decl_parts(work[i], &decl_name, NULL) && package_syntax_identity_matches(decl_name, name)) return true;
    }
    return false;
}

static bool package_trait_require_name(const IdmSyntax *stmt, IdmSyntax **out_required, IdmError *err) {
    *out_required = NULL;
    if (!syn_is_form(stmt, "%-expr") || stmt->as.seq.count < 3u || !syn_is_word(stmt->as.seq.items[1], "require")) return true;
    size_t end = 0;
    if (!try_qualified_word_at(stmt->as.seq.items, 2u, stmt->as.seq.count, out_required, &end, err)) return false;
    if (*out_required && end != stmt->as.seq.count) {
        idm_syn_free(*out_required);
        *out_required = NULL;
    }
    return true;
}

static bool package_trait_ready(IdmSyntax **work, const bool *placed, size_t work_count, size_t index, bool *out_ready, IdmError *err) {
    *out_ready = true;
    const IdmSyntax *body = NULL;
    if (!package_trait_decl_parts(work[index], NULL, &body)) return true;
    for (size_t i = 1; i < body->as.seq.count; i++) {
        IdmSyntax *required = NULL;
        if (!package_trait_require_name(body->as.seq.items[i], &required, err)) return false;
        if (!required) continue;
        if (package_local_trait_decl_seen(work, placed, work_count, required->as.text, false) &&
            !package_local_trait_decl_seen(work, placed, work_count, required->as.text, true)) {
            idm_syn_free(required);
            *out_ready = false;
            return true;
        }
        idm_syn_free(required);
    }
    return true;
}

static bool package_implement_decl_parts(const IdmSyntax *form, IdmSyntax **out_trait, IdmSyntax **out_type, IdmError *err) {
    *out_trait = NULL;
    *out_type = NULL;
    size_t head = 0;
    const BodyDeclDesc *desc = package_expr_head_index(form, 0, &head);
    if (!desc || desc->kind != BODY_DECL_IMPLEMENT || form->as.seq.count < head + 4u) {
        return true;
    }
    size_t pos = 0;
    IdmSyntax *trait = NULL;
    if (!try_qualified_word_at(form->as.seq.items, head + 1u, form->as.seq.count, &trait, &pos, err)) return false;
    if (!trait) return true;
    if (pos >= form->as.seq.count || !syn_is_word(form->as.seq.items[pos], "on")) {
        idm_syn_free(trait);
        return true;
    }
    pos++;
    IdmSyntax *type = NULL;
    size_t type_end = 0;
    if (!try_qualified_word_at(form->as.seq.items, pos, form->as.seq.count, &type, &type_end, err)) {
        idm_syn_free(trait);
        return false;
    }
    bool valid = type && (type_end == form->as.seq.count ||
                          (type_end + 1u == form->as.seq.count && syn_is_form(form->as.seq.items[type_end], "%-body")));
    if (!valid) {
        idm_syn_free(type);
        idm_syn_free(trait);
        return true;
    }
    *out_trait = trait;
    *out_type = type;
    return true;
}

static bool package_local_implement_decl_seen(IdmSyntax **work, const bool *placed, size_t work_count, const char *trait_name, const char *type_name, bool require_placed, bool *out_seen, IdmError *err) {
    *out_seen = false;
    for (size_t i = 0; i < work_count; i++) {
        if (require_placed && (!placed || !placed[i])) continue;
        IdmSyntax *impl_trait = NULL;
        IdmSyntax *impl_type = NULL;
        if (!package_implement_decl_parts(work[i], &impl_trait, &impl_type, err)) return false;
        bool matched = package_syntax_pair_matches(impl_trait, trait_name, impl_type, type_name);
        idm_syn_free(impl_type);
        idm_syn_free(impl_trait);
        if (matched) {
            *out_seen = true;
            return true;
        }
    }
    return true;
}

static bool package_implement_ready(IdmSyntax **work, const bool *placed, size_t work_count, size_t index, bool *out_ready, IdmError *err) {
    *out_ready = true;
    IdmSyntax *trait = NULL;
    IdmSyntax *type = NULL;
    if (!package_implement_decl_parts(work[index], &trait, &type, err)) return false;
    if (!trait || !type) {
        idm_syn_free(type);
        idm_syn_free(trait);
        return true;
    }
    for (size_t i = 0; i < work_count; i++) {
        const IdmSyntax *decl_name = NULL;
        const IdmSyntax *body = NULL;
        if (!package_trait_decl_parts(work[i], &decl_name, &body) || !package_syntax_identity_matches(decl_name, trait->as.text)) continue;
        for (size_t j = 1; j < body->as.seq.count; j++) {
            IdmSyntax *required = NULL;
            if (!package_trait_require_name(body->as.seq.items[j], &required, err)) {
                idm_syn_free(type);
                idm_syn_free(trait);
                return false;
            }
            if (!required) continue;
            bool exists = false;
            bool placed_impl = false;
            bool ok = package_local_implement_decl_seen(work, placed, work_count, required->as.text, type->as.text, false, &exists, err) &&
                      package_local_implement_decl_seen(work, placed, work_count, required->as.text, type->as.text, true, &placed_impl, err);
            if (!ok) {
                idm_syn_free(required);
                idm_syn_free(type);
                idm_syn_free(trait);
                return false;
            }
            if (exists && !placed_impl) {
                idm_syn_free(required);
                idm_syn_free(type);
                idm_syn_free(trait);
                *out_ready = false;
                return true;
            }
            idm_syn_free(required);
        }
    }
    idm_syn_free(type);
    idm_syn_free(trait);
    return true;
}

static bool package_rank_ready(IdmSyntax **work, const bool *placed, size_t work_count, size_t index, int rank, bool *out_ready, IdmError *err) {
    *out_ready = true;
    if (rank == 6) return package_trait_ready(work, placed, work_count, index, out_ready, err);
    if (rank == 7) return package_implement_ready(work, placed, work_count, index, out_ready, err);
    return true;
}

static bool package_order_work(IdmSyntax **work, size_t work_count, IdmError *err) {
    if (work_count < 2u) return true;
    IdmSyntax **ordered = malloc(work_count * sizeof(*ordered));
    bool *placed = calloc(work_count, sizeof(*placed));
    if (!ordered || !placed) {
        free(ordered);
        free(placed);
        return idm_error_oom(err, work[0]->span);
    }
    size_t out = 0;
    for (int rank = 0; rank <= 10; rank++) {
        if (rank == 6 || rank == 7) {
            bool progress = true;
            while (progress) {
                progress = false;
                for (size_t i = 0; i < work_count; i++) {
                    if (placed[i] || package_work_rank(work[i]) != rank) continue;
                    bool ready = true;
                    if (!package_rank_ready(work, placed, work_count, i, rank, &ready, err)) {
                        free(ordered);
                        free(placed);
                        return false;
                    }
                    if (!ready) continue;
                    ordered[out++] = work[i];
                    placed[i] = true;
                    progress = true;
                }
            }
        }
        for (size_t i = 0; i < work_count; i++) {
            if (placed[i] || package_work_rank(work[i]) != rank) continue;
            ordered[out++] = work[i];
            placed[i] = true;
        }
    }
    memcpy(work, ordered, work_count * sizeof(*work));
    free(ordered);
    free(placed);
    return true;
}

static bool owned_syntax_push(IdmSyntax ***owned, size_t *count, size_t *cap, IdmSyntax *syn, IdmError *err) {
    if (*count == *cap) {
        if (!idm_grow((void **)owned, cap, sizeof(**owned), 8u, *count + 1u)) return idm_error_oom(err, syn ? syn->span : idm_span_unknown(NULL));
    }
    (*owned)[(*count)++] = syn;
    return true;
}

static const IdmSyntax *expanded_splice_body(const IdmSyntax *expanded) {
    if (syn_is_form(expanded, "%-body")) return expanded;
    return NULL;
}

static bool body_install_expanded_syntax(IdmSyntax ***work, size_t *work_count, size_t *work_cap, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmScopeId extra_scope, size_t index, IdmSyntax *expanded, IdmError *err) {
    if (extra_scope != 0 && !idm_syn_scope_add_tree(expanded, 0, extra_scope)) {
        IdmSpan span = expanded ? expanded->span : idm_span_unknown(NULL);
        idm_syn_free(expanded);
        return idm_error_oom(err, span);
    }
    if (!owned_syntax_push(owned, owned_count, owned_cap, expanded, err)) {
        idm_syn_free(expanded);
        return false;
    }
    const IdmSyntax *splice_body = expanded_splice_body(expanded);
    if (splice_body) return body_work_splice(work, work_count, work_cap, index, splice_body, err);
    (*work)[index] = expanded;
    return true;
}

static IdmSyntax *flatten_degenerate_adjacent_expr(const IdmSyntax *form, IdmError *err) {
    if (!syn_is_form(form, "%-expr") || form->as.seq.count != 2u ||
        !syn_is_form(form->as.seq.items[1], "%-adjacent")) {
        return NULL;
    }
    IdmSyntax *flat = idm_syn_clone(form);
    if (!flat) {
        idm_error_oom(err, form->span);
        return NULL;
    }
    IdmSyntax *run = flat->as.seq.items[1];
    if (run->as.seq.count < 3u) return flat;

    size_t new_count = run->as.seq.count;
    IdmSyntax **new_items = malloc(new_count * sizeof(*new_items));
    if (!new_items) {
        idm_syn_free(flat);
        idm_error_oom(err, form->span);
        return NULL;
    }

    IdmSyntax **old_items = flat->as.seq.items;
    IdmSyntax **run_items = run->as.seq.items;
    new_items[0] = old_items[0];
    for (size_t i = 1; i < run->as.seq.count; i++) new_items[i] = run_items[i];

    IdmSyntax *adjacent_head = run_items[0];
    run->as.seq.items = NULL;
    run->as.seq.count = 0;
    run->as.seq.cap = 0;
    idm_syn_free(adjacent_head);
    idm_syn_free(run);
    free(run_items);
    free(old_items);

    flat->as.seq.items = new_items;
    flat->as.seq.count = new_count;
    flat->as.seq.cap = new_count;
    return flat;
}

static bool normalize_degenerate_adjacent_exprs(IdmSyntax **work, size_t work_count, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmError *err) {
    for (size_t i = 0; i < work_count; i++) {
        IdmSyntax *flat = flatten_degenerate_adjacent_expr(work[i], err);
        if (!flat) {
            if (err && err->present) return false;
            continue;
        }
        if (!owned_syntax_push(owned, owned_count, owned_cap, flat, err)) {
            idm_syn_free(flat);
            return false;
        }
        work[i] = flat;
    }
    return true;
}

static void package_work_remove(IdmSyntax **work, size_t *work_count, size_t index) {
    if (index + 1u < *work_count) memmove(work + index, work + index + 1u, (*work_count - index - 1u) * sizeof(*work));
    (*work_count)--;
}

static bool package_deferrable_error(const IdmError *err) {
    return err && err->present && idm_error_reason_is(err, "unbound-identifier");
}

static bool package_run_for_syntax_at(ExpandContext *ctx, IdmSyntax **work, size_t *work_count, size_t index, bool *deferred, IdmError *err) {
    if (deferred) *deferred = false;
    const IdmSyntax *for_syntax_body = NULL;
    if (!for_syntax_form(work[index], &for_syntax_body)) return true;
    ScopePropagation propagation = { work, 0, *work_count, NULL };
    if (!body_eval_for_syntax(ctx, work[index], for_syntax_body, &propagation, err)) {
        if (package_deferrable_error(err)) {
            idm_error_clear(err);
            if (deferred) *deferred = true;
            return true;
        }
        return false;
    }
    package_work_remove(work, work_count, index);
    return true;
}

static bool package_register_defmacro_at(ExpandContext *ctx, IdmSyntax **work, size_t *work_count, size_t index, bool *deferred, IdmError *err) {
    if (deferred) *deferred = false;
    const char *definition_head = NULL;
    if (!definition_like_form(work[index], &definition_head) || strcmp(definition_head, "defmacro") != 0) return true;
    const IdmSyntax *form = work[index];
    size_t mbase = (form->as.seq.count > 1 && syn_is_word(form->as.seq.items[1], "export")) ? 2u : 1u;
    if (form->as.seq.count < mbase + 3u || form->as.seq.items[mbase + 1u]->kind != IDM_SYN_WORD) {
        return idm_error_set(err, form->span, "defmacro expects a name, one syntax parameter, and a body");
    }
    const char *macro_name = form->as.seq.items[mbase + 1u]->as.text;
    int saved_phase = ctx->phase;
    ctx->phase = 1;
    IdmCore *fn = expand_function_literal(ctx, macro_name, form->as.seq.items[mbase], form->as.seq.items, mbase + 2u, form->as.seq.count, err);
    ctx->phase = saved_phase;
    if (!fn) {
        if (package_deferrable_error(err)) {
            idm_error_clear(err);
            if (deferred) *deferred = true;
            return true;
        }
        return false;
    }
    if (core_callable_capture_count(fn) != 0) {
        idm_core_free(fn);
        return idm_error_set(err, form->span, "package defmacro '%s' cannot capture for-syntax locals during package preparation", macro_name);
    }
    bool ok = register_macro(ctx, form->as.seq.items[mbase + 1u], fn, form->span, mbase == 2u, err);
    idm_core_free(fn);
    if (!ok) return false;
    package_work_remove(work, work_count, index);
    return true;
}

static bool package_expand_macro_at(ExpandContext *ctx, IdmSyntax ***work, size_t *work_count, size_t *work_cap, IdmSyntax ***owned, size_t *owned_count, size_t *owned_cap, IdmScopeId package_scope, size_t index, bool *expanded_any, IdmError *err) {
    IdmSyntax *form = (*work)[index];
    if (!syn_is_form(form, "%-expr") || form->as.seq.count < 2u || form->as.seq.items[1]->kind != IDM_SYN_WORD) return true;
    if (bind_form_parts(form, &(const IdmSyntax *){NULL}, &(size_t){0}) || definition_like_form(form, &(const char *){NULL}) || body_decl_form(form, BODY_DECL_EXPORT_BOUNDARY)) return true;
    IdmSyntax *expanded = NULL;
    if (!expand_body_macro_form_to_syntax(ctx, form, &expanded, err)) return false;
    if (!expanded) return true;
    if (!body_install_expanded_syntax(work, work_count, work_cap, owned, owned_count, owned_cap, package_scope, index, expanded, err)) return false;
    *expanded_any = true;
    return true;
}

typedef struct {
    const char *name;
    IdmPrimitive primitive;
    IdmArity arity;
    uint32_t slot;
} PrimitiveSeed;

static const char *declared_package_name(ExpandContext *ctx, IdmSyntax *const *work, size_t work_count, IdmError *err) {
    if (ctx->package_name) return ctx->package_name;
    for (size_t i = 0; i < work_count; i++) {
        const IdmSyntax *form = work[i];
        if (!syn_is_form(form, "%-expr") || form->as.seq.count != 3u ||
            !syn_is_word(form->as.seq.items[1], "package") ||
            form->as.seq.items[2]->kind != IDM_SYN_WORD) {
            continue;
        }
        const char *interned = idm_symbol_text(idm_intern(&ctx->rt->intern, IDM_SYMBOL_WORD, form->as.seq.items[2]->as.text));
        if (!interned) {
            idm_error_oom(err, form->span);
            return NULL;
        }
        ctx->package_name = interned;
        return interned;
    }
    if (ctx->primitive_home && strcmp(ctx->primitive_home, "kernel") == 0) return ctx->primitive_home;
    return NULL;
}

static bool primitive_seeds_push(PrimitiveSeed **seeds, size_t *count, size_t *cap, const char *name, IdmPrimitive primitive, IdmArity arity, uint32_t slot, IdmError *err, IdmSpan span) {
    if (*count == *cap) {
        if (!idm_grow((void **)seeds, cap, sizeof(**seeds), 16u, *count + 1u)) return idm_error_oom(err, span);
    }
    (*seeds)[*count].name = name;
    (*seeds)[*count].primitive = primitive;
    (*seeds)[*count].arity = arity;
    (*seeds)[*count].slot = slot;
    (*count)++;
    return true;
}

static bool primitive_seed_exports(const char *home) {
    return !home || strcmp(home, "regex") != 0;
}

static bool seed_virtual_package_primitives(ExpandContext *ctx, IdmSyntax *const *work, size_t work_count, IdmScopeId package_scope, PrimitiveSeed **out_seeds, size_t *out_count, IdmError *err) {
    *out_seeds = NULL;
    *out_count = 0;
    const char *declared = declared_package_name(ctx, work, work_count, err);
    if (!declared) return !(err && err->present);
    const char *home = ctx->primitive_home;
    if (!home) return true;
    if (strcmp(declared, home) != 0) {
        return idm_error_set(err, idm_span_unknown(NULL), "package '%s' is not the canonical home for '%s' primitives", declared, home);
    }
    PrimitiveSeed *seeds = NULL;
    size_t seed_count = 0;
    size_t seed_cap = 0;
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        IdmPrimitive primitive = (IdmPrimitive)i;
        const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
        if (!info || strcmp(idm_primitive_home(primitive), home) != 0) continue;
        IdmSyntax *name_syntax = idm_syn_word(info->name, idm_span_unknown(NULL));
        if (!name_syntax) {
            free(seeds);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        bool ok = package_scope == 0 || idm_syn_scope_add(name_syntax, 0, package_scope);
        IdmArity arity = idm_primitive_arity(primitive);
        IdmScopeSet scopes;
        idm_scope_set_init(&scopes);
        uint32_t slot = 0;
        if (ok) ok = binder_scopes_pruned(ctx, name_syntax, &scopes);
        if (ok) {
            const IdmBinding *existing = NULL;
            if (body_existing_env_binding(ctx, info->name, &scopes, &existing)) {
                slot = existing->payload;
            } else {
                slot = ctx->env_slot_seq++;
                ok = idm_binding_table_add_primitive_with_arity(&ctx->bindings, info->name, IDM_PHASE_ANY, IDM_BIND_SPACE_DEFAULT, IDM_BIND_ENV, &scopes, slot, IDM_FRAME_ENV, arity, (uint32_t)primitive, NULL);
                if (!ok) idm_error_oom(err, name_syntax->span);
            }
        }
        if (ok) ok = record_package_slot(ctx, info->name, slot, &scopes, arity, primitive_seed_exports(home));
        if (ok) ok = primitive_seeds_push(&seeds, &seed_count, &seed_cap, info->name, primitive, arity, slot, err, name_syntax->span);
        idm_scope_set_destroy(&scopes);
        idm_syn_free(name_syntax);
        if (!ok) {
            free(seeds);
            if (err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
            return false;
        }
    }
    *out_seeds = seeds;
    *out_count = seed_count;
    return true;
}

static IdmCore *wrap_virtual_primitive_seeds(IdmCore *body, const PrimitiveSeed *seeds, size_t seed_count, IdmError *err) {
    if (!body || seed_count == 0) return body;
    IdmCore *prelude = idm_core_letrec(idm_span_unknown(NULL));
    bool ok = prelude != NULL;
    if (ok) idm_core_letrec_set_env(prelude);
    for (size_t i = 0; ok && i < seed_count; i++) {
        IdmCore *value = idm_core_primitive_backed_fn(seeds[i].name, seeds[i].primitive, seeds[i].arity, idm_span_unknown(NULL));
        ok = value != NULL && idm_core_letrec_add(prelude, seeds[i].name, seeds[i].slot, value);
        if (!ok && value) idm_core_free(value);
    }
    if (ok) ok = idm_core_letrec_set_body(prelude, body);
    if (!ok) {
        idm_core_free(prelude);
        idm_core_free(body);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    return prelude;
}

static bool run_virtual_primitive_seed_phase(ExpandContext *ctx, const PrimitiveSeed *seeds, size_t seed_count, IdmError *err) {
    if (seed_count == 0) return true;
    IdmCore *body = idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    IdmCore *init = body ? wrap_virtual_primitive_seeds(body, seeds, seed_count, err) : NULL;
    if (!init) {
        if (!body && err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
        return false;
    }
    bool ok = run_phase_core(ctx, init, err);
    idm_core_free(init);
    return ok;
}

IdmCore *expand_package_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, IdmError *err) {
    size_t work_count = count > index ? count - index : 0u;
    size_t work_cap = work_count ? work_count : 1u;
    IdmSyntax **work = malloc(work_cap * sizeof(*work));
    if (!work) return (IdmCore *)(uintptr_t)idm_error_oom(err, index < count ? items[index]->span : idm_span_unknown(NULL));
    for (size_t i = 0; i < work_count; i++) work[i] = items[index + i];
    IdmSyntax **owned = NULL;
    size_t owned_count = 0;
    size_t owned_cap = 0;

    if (!normalize_degenerate_adjacent_exprs(work, work_count, &owned, &owned_count, &owned_cap, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    IdmScopeId package_scope = idm_scope_fresh(&ctx->scope_store);
    if (work_count != 0 && !body_scope_add_range(ctx, work, 0, work_count, package_scope, work[0]->span, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    PrimitiveSeed *primitive_seeds = NULL;
    size_t primitive_seed_count = 0;
    if (!seed_virtual_package_primitives(ctx, work, work_count, package_scope, &primitive_seeds, &primitive_seed_count, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }
    if (!run_virtual_primitive_seed_phase(ctx, primitive_seeds, primitive_seed_count, err)) {
        free(primitive_seeds);
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }

    BodyDefCtx def_ctx;
    def_ctx.prev = ctx->def_ctx;
    ctx->def_ctx = &def_ctx;
    bool ok = true;
    bool changed = true;
    while (ok && changed) {
        changed = false;
        for (size_t i = 0; ok && i < work_count;) {
            size_t before = work_count;
            bool deferred = false;
            ok = package_run_for_syntax_at(ctx, work, &work_count, i, &deferred, err);
            if (!ok) break;
            if (deferred) { i++; continue; }
            if (work_count != before) { changed = true; continue; }
            ok = package_register_defmacro_at(ctx, work, &work_count, i, &deferred, err);
            if (!ok) break;
            if (deferred) { i++; continue; }
            if (work_count != before) { changed = true; continue; }
            bool expanded = false;
            ok = package_expand_macro_at(ctx, &work, &work_count, &work_cap, &owned, &owned_count, &owned_cap, package_scope, i, &expanded, err);
            if (!ok) break;
            if (expanded) { changed = true; continue; }
            i++;
        }
    }
    bool values_changed = true;
    while (ok && values_changed) {
        values_changed = false;
        ok = package_prepare_values(ctx, work, work_count, &values_changed, err);
    }
    ok = ok && package_order_work(work, work_count, err);
    IdmCore *body = ok ? expand_body_items(ctx, work, 0, work_count, false, err) : NULL;
    ctx->def_ctx = def_ctx.prev;
    if (body) body = wrap_virtual_primitive_seeds(body, primitive_seeds, primitive_seed_count, err);
    free(primitive_seeds);
    for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
    free(owned);
    free(work);
    return body;
}

static IdmCore *expand_boundary_fallback(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    IdmCore *core = syn_is_form(bform, "%-expr")
        ? expand_parts(ctx, bform->as.seq.items, 1, bform->as.seq.count, err)
        : expand_syntax(ctx, bform, err);
    if (core && boundary_index + 1u < work_count) {
        IdmCore *rest = expand_body_items(ctx, work, boundary_index + 1u, work_count, false, err);
        if (!rest) {
            idm_core_free(core);
            return NULL;
        }
        IdmCore *do_expr = idm_core_do(bform->span);
        if (!do_expr || !idm_core_do_add(do_expr, core) || !idm_core_do_add(do_expr, rest)) {
            idm_core_free(core);
            idm_core_free(rest);
            idm_core_free(do_expr);
            idm_error_oom(err, bform->span);
            return NULL;
        }
        core = do_expr;
    }
    return core;
}

static IdmCore *boundary_rest(ExpandContext *ctx, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    return expand_body_items(ctx, work, boundary_index + 1u, work_count, false, err);
}

static IdmCore *boundary_activate_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    if (bform->as.seq.count < 3u) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    size_t name_end = bform->as.seq.count;
    IdmSyntax *name = expect_qualified_word_at(bform->as.seq.items, 2u, &name_end, "activate expects a single protocol name", err);
    if (!name) return NULL;
    if (name_end != bform->as.seq.count) {
        idm_syn_free(name);
        expand_error(err, bform->span, "activate expects a single protocol name");
        return NULL;
    }
    IdmCore *core = expand_activate(ctx, name, work, boundary_index, work_count, bform->span, err);
    idm_syn_free(name);
    return core;
}

static IdmCore *boundary_for_syntax_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    const IdmSyntax *for_syntax_body = NULL;
    if (!for_syntax_form(bform, &for_syntax_body)) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    ScopePropagation propagation = { (IdmSyntax **)work, boundary_index + 1u, work_count, NULL };
    return body_eval_for_syntax(ctx, bform, for_syntax_body, &propagation, err) ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_core_syntax_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    size_t head = 0;
    const BodyDeclDesc *desc = package_expr_head_index(bform, BODY_DECL_EXPORT_BOUNDARY, &head);
    if (!desc || desc->kind != BODY_DECL_CORE_SYNTAX) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    if (bform->as.seq.count != head + 3u || bform->as.seq.items[head + 1u]->kind != IDM_SYN_WORD ||
        !syn_is_form(bform->as.seq.items[head + 2u], "%-body")) {
        return expand_error(err, bform->span, "core-syntax expects a head name before do; use core-syntax _ do for unresolved-head fallback");
    }
    const IdmSyntax *name_syntax = bform->as.seq.items[head + 1u];
    const IdmSyntax *body = bform->as.seq.items[head + 2u];
    int saved_phase = ctx->phase;
    ctx->phase = 1;
    IdmCore *fn = build_clause_fn(ctx, body, body->as.seq.count, name_syntax->as.text, err);
    ctx->phase = saved_phase;
    if (!fn) return NULL;
    bool ok = register_core_syntax(ctx, name_syntax, fn, bform->span, err);
    idm_core_free(fn);
    return ok ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_core_operator_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    return expand_core_operator_decl(ctx, bform, err) ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_core_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    return expand_core_grammar_decl(ctx, bform, err) ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_surface_grammar_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    return expand_surface_grammar_decl(ctx, bform, err) ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_core_reader_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    return expand_core_reader_decl(ctx, bform, err) ? boundary_rest(ctx, work, boundary_index, work_count, err) : NULL;
}

static IdmCore *boundary_use_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    if (bform->as.seq.count < 3u || package_path_text(bform->as.seq.items[2]) == NULL) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    UseSelection selection;
    size_t pos = 3u;
    if (!use_selection_parse(bform, pos, bform->as.seq.count, &pos, &selection, err)) return NULL;
    if (pos != bform->as.seq.count) {
        use_selection_destroy(&selection);
        return expand_error(err, bform->span, "use expects 'use path' or 'use path with [name ...]'");
    }
    IdmCore *core = expand_use(ctx, package_path_text(bform->as.seq.items[2]), NULL, &selection, work, boundary_index + 1u, work_count, bform->span, err);
    use_selection_destroy(&selection);
    return core;
}

static IdmCore *boundary_import_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    if (bform->as.seq.count < 3u || package_path_text(bform->as.seq.items[2]) == NULL) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    const char *path = package_path_text(bform->as.seq.items[2]);
    const char *alias = NULL;
    UseSelection selection;
    size_t pos = 3u;
    if (!use_selection_parse(bform, pos, bform->as.seq.count, &pos, &selection, err)) return NULL;
    if (pos == bform->as.seq.count) {
        const char *slash = strrchr(path, '/');
        alias = slash ? slash + 1u : path;
    } else if (pos + 2u == bform->as.seq.count && syn_is_word(bform->as.seq.items[pos], "as") && bform->as.seq.items[pos + 1u]->kind == IDM_SYN_WORD) {
        alias = bform->as.seq.items[pos + 1u]->as.text;
    } else {
        use_selection_destroy(&selection);
        return expand_error(err, bform->span, "import expects 'import path', 'import path as alias', or 'import path with [name ...] as alias'");
    }
    IdmCore *core = expand_use(ctx, path, alias, &selection, work, boundary_index + 1u, work_count, bform->span, err);
    use_selection_destroy(&selection);
    return core;
}

static IdmCore *expand_boundary_decl(ExpandContext *ctx, const IdmSyntax *bform, IdmSyntax *const *work, size_t boundary_index, size_t work_count, IdmError *err) {
    const BodyDeclDesc *desc = body_decl_form(bform, BODY_DECL_EXPORT_BOUNDARY);
    if (!desc || !desc->handler) return expand_boundary_fallback(ctx, bform, work, boundary_index, work_count, err);
    return desc->handler(ctx, bform, work, boundary_index, work_count, err);
}

IdmCore *expand_body_items(ExpandContext *ctx, IdmSyntax *const *items, size_t index, size_t count, bool def_scope, IdmError *err) {
    if (index >= count) return idm_core_literal(idm_nil(), idm_span_unknown(NULL));
    size_t work_count = count - index;
    size_t work_cap = work_count;
    IdmSyntax **work = malloc(work_cap * sizeof(*work));
    if (!work) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    for (size_t i = 0; i < work_count; i++) work[i] = items[index + i];
    IdmSyntax **owned = NULL;
    size_t owned_count = 0;
    size_t owned_cap = 0;
    if (!normalize_degenerate_adjacent_exprs(work, work_count, &owned, &owned_count, &owned_cap, err)) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return NULL;
    }
    if (def_scope) {
        IdmScopeId body_scope = idm_scope_fresh(&ctx->scope_store);
        if (!body_scope_add_range(ctx, work, 0, work_count, body_scope, items[index]->span, err)) {
            for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
            free(owned);
            free(work);
            return NULL;
        }
    }
    IdmScopeSet body_ctx_scopes;
    if (!syntax_scopes_copy(&body_ctx_scopes, work[0])) {
        for (size_t i = 0; i < owned_count; i++) idm_syn_free(owned[i]);
        free(owned);
        free(work);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[index]->span);
    }
    const IdmScopeSet *saved_op_fallback = ctx->op_fallback;
    ctx->op_fallback = &body_ctx_scopes;
    BodyRec *recs = NULL;
    size_t rec_count = 0;
    size_t rec_cap = 0;
    BodyDefCtx def_ctx;
    def_ctx.prev = ctx->def_ctx;
    ctx->def_ctx = &def_ctx;
    bool failed = false;
    bool boundary = false;
    size_t boundary_index = 0;
    IdmScopeId definition_scope = 0;
    size_t i = 0;

    while (i < work_count && !failed && !boundary) {
        const IdmSyntax *form = work[i];
        if (syn_is_form(form, "%-expr") && form->as.seq.count == 3 &&
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
                    if (recs[r].kind == BODY_REC_BIND && !push_bind_binder(ctx, recs[r].bind_name, recs[r].bind_slot, recs[r].bind_arity, err)) {
                        failed = true;
                    } else if (recs[r].kind == BODY_REC_BIND_PATTERN) {
                        for (size_t n = 0; n < recs[r].pattern_name_count && !failed; n++) {
                            CallableBindingInfo info = recs[r].pattern_bindings ? recs[r].pattern_bindings[n] : (CallableBindingInfo){ idm_arity_unknown() };
                            if (!push_bind_binder(ctx, recs[r].pattern_names[n], recs[r].pattern_slots[n], info.arity, err)) {
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
                if (core_callable_capture_count(fn) != 0) {
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
                        if (!idm_grow((void **)&recs, &rec_cap, sizeof(*recs), 8u, rec_count + 1u)) { idm_core_free(reg); idm_error_oom(err, form->span); failed = true; break; }
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
            bool persistent_repl_env = ctx->repl_env_binds && ctx->frame == IDM_FRAME_TOP;
            bool package_top = ctx->in_package && ctx->frame == IDM_FRAME_TOP;
            if (!persistent_repl_env && !package_top) {
                if (definition_scope == 0) definition_scope = idm_scope_fresh(&ctx->scope_store);
                if (!body_definition_scope_add(ctx, work, i, work_count, definition_scope, form->span, err)) {
                    failed = true;
                    break;
                }
                form = work[i];
            }
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
            if (group_count == 0) {
                expand_error(err, form->span, "defn expects a name, parameters, and a body");
                failed = true;
                break;
            }
            if (failed) break;
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            for (size_t k = 0; k < group_count && !failed; k++) {
                if (!defn_group_arity(&groups[k], work, &groups[k].arity, err)) {
                    failed = true;
                    break;
                }
                bool ok = true;
                if (top_level && ctx->in_package) ok = body_env_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, true, &groups[k].slot, NULL, err);
                else ok = top_level ? env_push_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, &groups[k].slot)
                                    : local_push_def_binder_with_arity(ctx, groups[k].name, groups[k].name_syntax, groups[k].arity, &groups[k].slot);
                if (!ok) { idm_error_oom(err, form->span); failed = true; break; }
                if (top_level && ctx->in_package && !record_package_slot(ctx, groups[k].name, groups[k].slot, &groups[k].scopes, groups[k].arity, groups[k].exported)) {
                    idm_error_oom(err, form->span);
                    failed = true;
                    break;
                }
            }
            if (failed) { defn_groups_destroy(groups, group_count); break; }
            if (rec_count == rec_cap) {
                if (!idm_grow((void **)&recs, &rec_cap, sizeof(*recs), 8u, rec_count + 1u)) { defn_groups_destroy(groups, group_count); idm_error_oom(err, form->span); failed = true; break; }
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
        if (syn_is_form(form, "%-expr") && form->as.seq.count >= 2 && form->as.seq.items[1]->kind == IDM_SYN_WORD &&
            !bind_form_parts(form, &(const IdmSyntax *){NULL}, &(size_t){0})) {
            bool is_boundary_word = body_boundary_form(form);
            if (!is_boundary_word) {
                IdmSyntax *expanded = NULL;
                if (!expand_body_macro_form_to_syntax(ctx, form, &expanded, err)) {
                    failed = true;
                    break;
                }
                if (expanded) {
                    if (!body_install_expanded_syntax(&work, &work_count, &work_cap, &owned, &owned_count, &owned_cap, 0, i, expanded, err)) { failed = true; break; }
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
            if (!idm_grow((void **)&recs, &rec_cap, sizeof(*recs), 8u, rec_count + 1u)) { idm_error_oom(err, form->span); failed = true; break; }
        }
        memset(&recs[rec_count], 0, sizeof(recs[rec_count]));
        const IdmSyntax *bind_name = NULL;
        const IdmSyntax *bind_pattern = NULL;
        size_t rhs_start = 0;
        if (bind_form_parts(form, &bind_name, &rhs_start)) {
            IdmArity bind_arity = idm_arity_unknown();
            if (!rhs_callable_value_binding_info(ctx, form->as.seq.items, rhs_start, form->as.seq.count, &bind_arity, err) &&
                err && err->present) {
                failed = true;
                break;
            }
            recs[rec_count].kind = BODY_REC_BIND;
            recs[rec_count].bind_name = bind_name;
            recs[rec_count].rhs_start = rhs_start;
            if (ctx->in_package && ctx->frame == IDM_FRAME_TOP) {
                if (!body_env_def_binder_with_arity(ctx, bind_name->as.text, bind_name, bind_arity, true, &recs[rec_count].bind_slot, NULL, err)) {
                    failed = true;
                    break;
                }
            } else {
                recs[rec_count].bind_slot = (ctx->repl_env_binds && ctx->frame == IDM_FRAME_TOP) ? ctx->env_slot_seq++ : ctx->next_slot++;
            }
            recs[rec_count].bind_arity = bind_arity;
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
            rec->bind_arity = core_callable_arity(rec->core);
            if (rec->bind_arity.kind == IDM_ARITY_UNKNOWN) {
                IdmArity syntax_arity = idm_arity_unknown();
                if (rhs_callable_value_binding_info(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, &syntax_arity, err)) {
                    rec->bind_arity = syntax_arity;
                } else if (err && err->present) {
                    failed = true;
                    break;
                }
            }
            if (!push_bind_binder(ctx, rec->bind_name, rec->bind_slot, rec->bind_arity, err)) {
                failed = true;
                break;
            }
        } else if (rec->kind == BODY_REC_BIND_PATTERN) {
            bool saved_vc = ctx->value_context;
            ctx->value_context = true;
            IdmCore *rhs = expand_parts(ctx, rec->form->as.seq.items, rec->rhs_start, rec->form->as.seq.count, err);
            ctx->value_context = saved_vc;
            if (!rhs) { failed = true; break; }
            if (!infer_pattern_bind_infos(ctx, rec, err)) {
                failed = true;
                break;
            }
            rec->core = pattern_bind_call(rec, rhs, err);
            if (!rec->core) { failed = true; break; }
            for (size_t n = 0; n < rec->pattern_name_count; n++) {
                CallableBindingInfo info = rec->pattern_bindings ? rec->pattern_bindings[n] : (CallableBindingInfo){ idm_arity_unknown() };
                if (!push_bind_binder(ctx, rec->pattern_names[n], rec->pattern_slots[n], info.arity, err)) {
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        } else if (rec->kind == BODY_REC_GROUPS) {
            bool top_level = ctx->frame == IDM_FRAME_TOP;
            IdmCore *letrec = idm_core_letrec(rec->form->span);
            if (!letrec) { idm_error_oom(err, rec->form->span); failed = true; break; }
            if (top_level) idm_core_letrec_set_env(letrec);
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
            rec->core = syn_is_form(rec->form, "%-expr")
                ? expand_parts(ctx, rec->form->as.seq.items, 1, rec->form->as.seq.count, err)
                : expand_syntax(ctx, rec->form, err);
            if (!rec->core) { failed = true; break; }
        }
    }

    IdmCore *core = NULL;
    if (!failed) {
        if (boundary) {
            const IdmSyntax *bform = work[boundary_index];
            core = expand_boundary_decl(ctx, bform, work, boundary_index, work_count, err);
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
                if (body_bind_is_env(ctx)) {
                    IdmCore *letrec = idm_core_letrec(rec->form->span);
                    if (!letrec || !idm_core_letrec_add(letrec, rec->bind_name->as.text, rec->bind_slot, inner) || !idm_core_letrec_set_body(letrec, core)) {
                        idm_core_free(letrec);
                        idm_core_free(inner);
                        idm_core_free(core);
                        core = NULL;
                        idm_error_oom(err, rec->form->span);
                        failed = true;
                    } else {
                        idm_core_letrec_set_env(letrec);
                        core = letrec;
                    }
                } else {
                    core = idm_core_bind_local(rec->bind_name->as.text, rec->bind_slot, inner, core, rec->form->span);
                    if (!core) { idm_core_free(inner); idm_error_oom(err, rec->form->span); failed = true; }
                }
            } else if (rec->kind == BODY_REC_BIND_PATTERN) {
                bool env_bind = body_bind_is_env(ctx);
                for (size_t n = rec->pattern_name_count; n > 0 && !failed; n--) {
                    IdmCore *value = pattern_extract_value(rec, n - 1u, err);
                    if (!value) {
                        idm_core_free(core);
                        core = NULL;
                        failed = true;
                        break;
                    }
                    IdmSpan nspan = rec->pattern_names[n - 1u]->span;
                    if (env_bind) {
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
                            idm_core_letrec_set_env(letrec);
                            core = letrec;
                        }
                    } else {
                        core = idm_core_bind_local(rec->pattern_names[n - 1u]->as.text, rec->pattern_slots[n - 1u], value, core, nspan);
                        if (!core) {
                            idm_core_free(value);
                            idm_error_oom(err, rec->form->span);
                            failed = true;
                        }
                    }
                }
                if (!failed) {
                    core = idm_core_bind_local("_match", rec->pattern_tmp_slot, inner, core, rec->form->span);
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
    if (start >= end) {
        return expand_error(err, idm_span_unknown(NULL), "expected fn literal");
    }
    return expand_function_literal(ctx, "<lambda>", items[start], items, start + 1u, end, err);
}

IdmCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmError *err) {
    if (param_start + 1u == end && body_is_clauses(items[param_start], true)) {
        return build_clause_fn_styled(ctx, items[param_start], items[param_start]->as.seq.count, debug_name, true, err);
    }
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);

    IdmPattern **patterns = NULL;
    uint32_t pattern_count = 0;
    IdmPatternLocal *pattern_locals = NULL;
    uint32_t pattern_local_count = 0;
    IdmCore *guard = NULL;
    uint32_t arity = 0;
    IdmCore *body = expand_function_clause(ctx, debug_name, head, items, param_start, end, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
    if (!body) {
        end_function_context(ctx, &saved);
        return NULL;
    }

    IdmCore *fn = idm_core_fn(debug_name, arity, body, head->span);
    if (!fn
        || !idm_core_fn_set_param_patterns_take(fn, patterns, pattern_count)
        || !idm_core_fn_set_pattern_locals_take(fn, pattern_locals, pattern_local_count)
        || (guard && !idm_core_fn_set_guard_take(fn, guard))) {
        for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
        free(patterns);
        for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
        free(pattern_locals);
        idm_core_free(guard);
        idm_core_free(body);
        idm_core_free(fn);
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!idm_core_fn_add_capture(fn, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index)) {
            idm_core_free(fn);
            end_function_context(ctx, &saved);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, head->span);
        }
    }
    end_function_context(ctx, &saved);
    return fn;
}

static bool parse_function_clause_shape(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, bool allow_bodyless, FunctionClauseShape *out, IdmError *err) {
    FunctionClauseShape shape;
    memset(&shape, 0, sizeof(shape));
    shape.param_start = param_start;
    shape.param_end = param_start;
    shape.guard_start = SIZE_MAX;
    shape.guard_end = SIZE_MAX;
    shape.body_start = SIZE_MAX;
    shape.body_end = end;
    shape.body_index = SIZE_MAX;

    size_t cursor = param_start;
    size_t count = 0;
    while (cursor < end) {
        if (syn_is_word(items[cursor], "->")) {
            shape.has_arrow = true;
            shape.body_start = cursor + 1u;
            shape.param_end = cursor;
            cursor++;
            break;
        }
        if (syn_is_form(items[cursor], "%-body")) {
            shape.body_index = cursor;
            shape.param_end = cursor;
            break;
        }
        if (syn_is_word(items[cursor], "when")) {
            shape.has_guard = true;
            shape.guard_start = cursor + 1u;
            shape.param_end = cursor;
            cursor++;
            while (cursor < end) {
                if (syn_is_word(items[cursor], "->")) {
                    shape.has_arrow = true;
                    shape.guard_end = cursor;
                    shape.body_start = cursor + 1u;
                    cursor++;
                    break;
                }
                if (syn_is_form(items[cursor], "%-body")) {
                    shape.guard_end = cursor;
                    shape.body_index = cursor;
                    break;
                }
                cursor++;
            }
            if (shape.guard_end == SIZE_MAX) shape.guard_end = cursor;
            if (shape.guard_start == shape.guard_end) {
                expand_error(err, items[shape.guard_start - 1u]->span, "guard requires an expression before the body");
                return false;
            }
            break;
        }
        count++;
        cursor++;
    }
    if (!shape.has_guard && !shape.has_arrow && shape.body_index == SIZE_MAX) shape.param_end = cursor;
    if (count > UINT32_MAX) {
        expand_error(err, head->span, "function clause has too many parameters");
        return false;
    }
    shape.arity = (uint32_t)count;

    if (shape.has_arrow) {
        if (shape.body_start >= end) {
            expand_error(err, head->span, "function clause arrow requires a body");
            return false;
        }
        shape.body_end = end;
    } else if (shape.body_index != SIZE_MAX) {
        if (shape.body_index + 1u != end) {
            expand_error(err, items[shape.body_index]->span, "function clause do/end body must be final");
            return false;
        }
    } else if (allow_bodyless) {
        shape.body_start = end;
    } else {
        expand_error(err, head->span, "function clause requires -> or do/end body");
        return false;
    }
    *out = shape;
    return true;
}

static IdmCore *expand_function_clause(ExpandContext *ctx, const char *debug_name, const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmPattern ***out_patterns, uint32_t *out_pattern_count, IdmPatternLocal **out_locals, uint32_t *out_local_count, IdmCore **out_guard, uint32_t *out_arity, IdmError *err) {
    FunctionClauseShape shape;
    if (!parse_function_clause_shape(head, items, param_start, end, false, &shape, err)) return NULL;
    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);

    IdmPattern **patterns = NULL;
    size_t pattern_count = 0;
    size_t pattern_cap = 0;
    for (size_t cursor = shape.param_start; cursor < shape.param_end; cursor++) {
        if (pattern_count == pattern_cap) {
            if (!idm_grow((void **)&patterns, &pattern_cap, sizeof(*patterns), 4u, pattern_count + 1u)) {
                for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, items[cursor]->span);
            }
        }
        IdmPattern *pat = pattern_from_param(ctx, items[cursor], (uint32_t)pattern_count, err);
        if (!pat) {
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
        patterns[pattern_count++] = pat;
    }

    IdmCore *guard = NULL;
    if (shape.has_guard) {
        guard = expand_parts(ctx, items, shape.guard_start, shape.guard_end, err);
        if (!guard) {
            for (size_t i = 0; i < pattern_count; i++) idm_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
    }

    IdmCore *body = NULL;
    if (shape.has_arrow) {
        body = expand_parts(ctx, items, shape.body_start, shape.body_end, err);
    } else {
        body = expand_syntax(ctx, items[shape.body_index], err);
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
    *out_arity = shape.arity;
    (void)debug_name;
    return body;
}

static bool defn_form_clause_block(const IdmSyntax *def_form, size_t param_start, const IdmSyntax **out_body) {
    if (param_start + 1u != def_form->as.seq.count) return false;
    const IdmSyntax *body = def_form->as.seq.items[param_start];
    if (!body_is_clauses(body, true)) return false;
    *out_body = body;
    return true;
}

static const IdmSyntax *function_clause_head(const IdmSyntax *clause) {
    return clause->as.seq.count > 1u ? clause->as.seq.items[1] : clause;
}

static bool add_function_clause_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *arity, IdmError *err) {
    FunctionClauseShape shape;
    if (!parse_function_clause_shape(head, items, param_start, end, true, &shape, err)) return false;
    return idm_arity_add_exact(arity, shape.arity);
}

static bool function_clause_body_arity(const IdmSyntax *body, IdmArity *arity, IdmError *err) {
    for (size_t c = 1; c < body->as.seq.count; c++) {
        const IdmSyntax *clause = body->as.seq.items[c];
        if (!add_function_clause_arity(function_clause_head(clause), clause->as.seq.items, 1u, clause->as.seq.count, arity, err)) return false;
    }
    return true;
}

static bool add_function_parts_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *arity, IdmError *err) {
    if (param_start + 1u == end && body_is_clauses(items[param_start], true)) {
        return function_clause_body_arity(items[param_start], arity, err);
    }
    return add_function_clause_arity(head, items, param_start, end, arity, err);
}

static bool function_parts_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err) {
    IdmArity arity = idm_arity_unknown();
    if (!add_function_parts_arity(head, items, param_start, end, &arity, err)) return false;
    *out_arity = arity;
    return true;
}

static bool defn_group_arity(const DefnGroup *group, IdmSyntax *const *items, IdmArity *out_arity, IdmError *err) {
    IdmArity arity = idm_arity_unknown();
    for (size_t i = 0; i < group->count; i++) {
        const IdmSyntax *def_form = items[group->indices[i]];
        const IdmSyntax *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start, NULL);
        if (!add_function_parts_arity(def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &arity, err)) return false;
    }
    *out_arity = arity;
    return true;
}

bool function_literal_arity(const IdmSyntax *head, IdmSyntax *const *items, size_t param_start, size_t end, IdmArity *out_arity, IdmError *err) {
    return function_parts_arity(head, items, param_start, end, out_arity, err);
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
                IdmCore *body = expand_function_clause(ctx, def_name ? def_name->as.text : group->name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
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
        IdmCore *body = expand_function_clause(ctx, def_name ? def_name->as.text : group->name, def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
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
        bool ok = single ? idm_core_fn_add_capture(result, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index) : idm_core_fn_multi_add_capture(result, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index);
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
    if (!syn_is_form(clause, "%-expr")) {
        expand_error(err, clause->span, "match clause must be an expression");
        return NULL;
    }
    FunctionClauseShape shape;
    const IdmSyntax *head = clause->as.seq.count > 1u ? clause->as.seq.items[1] : clause;
    if (!parse_function_clause_shape(head, clause->as.seq.items, 1u, clause->as.seq.count, false, &shape, err)) return NULL;
    if (!shape.has_arrow || shape.body_index != SIZE_MAX) {
        expand_error(err, clause->span, "match clause must have form pattern -> body");
        return NULL;
    }
    if (shape.arity != 1u) {
        expand_error(err, clause->span, shape.arity == 0u ? "match clause must have form pattern -> body" : "match currently expects one pattern per clause");
        return NULL;
    }

    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);
    IdmPattern **patterns = calloc(1u, sizeof(*patterns));
    if (!patterns) {
        end_clause_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, clause->span);
    }
    patterns[0] = pattern_from_param(ctx, clause->as.seq.items[shape.param_start], 0, err);
    if (!patterns[0]) {
        free(patterns);
        end_clause_context(ctx, &saved);
        return NULL;
    }
    IdmCore *guard = NULL;
    if (shape.has_guard) {
        guard = expand_parts(ctx, clause->as.seq.items, shape.guard_start, shape.guard_end, err);
        if (!guard) {
            idm_pat_free(patterns[0]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
    }
    IdmCore *body = expand_parts(ctx, clause->as.seq.items, shape.body_start, shape.body_end, err);
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
    *out_arity = shape.arity;
    return body;
}

static bool body_is_clauses(const IdmSyntax *body, bool allow_empty_pattern) {
    if (!syn_is_form(body, "%-body") || body->as.seq.count < 2) return false;
    const IdmSyntax *first = body->as.seq.items[1];
    if (!syn_is_form(first, "%-expr")) return false;
    for (size_t i = 1; i < first->as.seq.count; i++) {
        const IdmSyntax *tok = first->as.seq.items[i];
        if (syn_is_word(tok, "->")) return allow_empty_pattern || i > 1u;
        if (syn_is_word(tok, "=") || syn_is_word(tok, "fn") || syn_is_word(tok, "defn") ||
            syn_is_word(tok, "def") || syn_is_word(tok, "defmacro")) return false;
    }
    return false;
}

IdmCore *expand_match_form(ExpandContext *ctx, const IdmSyntax *form, IdmError *err) {
    if (!syn_is_form(form, "%-match") || form->as.seq.count != 3u) return expand_error(err, form ? form->span : idm_span_unknown(NULL), "%-match expects a scrutinee and clause body");
    const IdmSyntax *body = form->as.seq.items[2];
    if (!body_is_clauses(body, false)) return expand_error(err, body ? body->span : form->span, "match requires a do/end clause body");
    IdmCore *scrutinee = expand_syntax(ctx, form->as.seq.items[1], err);
    if (!scrutinee) return NULL;
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IdmCore *match = idm_core_match(form->span);
    if (!match || !idm_core_match_add_scrutinee(match, scrutinee)) {
        idm_core_free(match);
        idm_core_free(scrutinee);
        end_function_context(ctx, &saved);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, form->span);
    }
    bool ok = true;
    for (size_t i = 1; ok && i < body->as.seq.count; i++) {
        IdmPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IdmPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IdmCore *guard = NULL;
        uint32_t arity = 0;
        IdmCore *clause_body = expand_match_clause(ctx, body->as.seq.items[i], &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!clause_body) {
            ok = false;
            break;
        }
        if (arity != match->as.match_expr.scrutinee_count) {
            expand_error(err, body->as.seq.items[i]->span, "match clause arity does not match scrutinee count");
            ok = false;
        } else if (!idm_core_match_add_clause_take(match, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, clause_body)) {
            if (!err->present) idm_error_oom(err, body->as.seq.items[i]->span);
            ok = false;
        }
        if (!ok) {
            idm_core_free(clause_body);
            idm_core_free(guard);
            for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
            free(patterns);
            for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
            free(pattern_locals);
            break;
        }
    }
    if (ok) {
        for (size_t i = 0; i < ctx->capture_count; i++) {
            if (!idm_core_match_add_capture(match, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index)) {
                ok = idm_error_oom(err, form->span);
                break;
            }
        }
    }
    end_function_context(ctx, &saved);
    if (!ok) {
        idm_core_free(match);
        return NULL;
    }
    return match;
}

static IdmCore *build_clause_fn_styled(ExpandContext *ctx, const IdmSyntax *body, size_t clause_end, const char *debug_name, bool defn_style, IdmError *err) {
    if (!syn_is_form(body, "%-body") || clause_end < 2u || body->as.seq.count < clause_end) return expand_error(err, body->span, "clause body requires at least one clause");
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
            ? expand_function_clause(ctx, debug_name, clause->as.seq.count > 1 ? clause->as.seq.items[1] : clause, clause->as.seq.items, 1, clause->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err)
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
        if (!idm_core_fn_multi_add_capture(multi, ctx->captures[i].kind, ctx->captures[i].name, ctx->captures[i].source_index)) {
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
        if (syn_is_form(items[i], "%-body")) {
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
        if (syn_is_form(clause, "%-expr") && clause->as.seq.count >= 2 && syn_is_word(clause->as.seq.items[1], "after")) {
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
