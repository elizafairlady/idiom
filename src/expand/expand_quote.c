#include "internal.h"

static IdmCore *string_literal(ExpandContext *ctx, const char *text, IdmSpan span, IdmError *err);
static IdmCore *syntax_context_literal(ExpandContext *ctx, const IdmSyntax *template_ctx, IdmError *err);
static IdmCore *syntax_build_core(ExpandContext *ctx, IdmSyntaxBuildKind kind, IdmCore *payload, const IdmSyntax *template_ctx, IdmSpan span, IdmError *err);
typedef bool (*TemplateSpliceFn)(const IdmSyntax *item, const IdmSyntax **out_splice);
typedef IdmCore *(*TemplateItemFn)(ExpandContext *ctx, const IdmSyntax *item, const void *user, IdmError *err);
typedef struct {
    TemplateSpliceFn splice;
    TemplateItemFn item;
    const void *user;
    const char *splice_name;
} TemplateListSpec;
typedef enum { TEMPLATE_QUOTE, TEMPLATE_QUASIQUOTE, TEMPLATE_QUASISYNTAX } TemplateMode;
typedef struct {
    TemplateMode mode;
    const IdmSyntax *syntax_ctx;
} TemplateContext;
static IdmCore *template_items_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmSpan span, const TemplateListSpec *spec, IdmError *err);
static bool template_walk_value(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValue *out, IdmError *err);
static IdmCore *template_walk_core(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmError *err);
static const char *quote_symbol_for_head(const char *head);
static bool quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err);
static IdmCore *datum_constant(ExpandContext *ctx, const IdmSyntax *t, IdmError *err);
static IdmCore *quasiquote_template(ExpandContext *ctx, const IdmSyntax *t, IdmError *err);
static IdmCore *quasisyntax_template(ExpandContext *ctx, const IdmSyntax *template, const IdmSyntax *template_ctx, IdmError *err);

static IdmCore *string_literal(ExpandContext *ctx, const char *text, IdmSpan span, IdmError *err) {
    IdmValue value = idm_string(ctx->rt, text, err);
    if (err && err->present) return NULL;
    return idm_core_literal(value, span);
}

static IdmCore *syntax_context_literal(ExpandContext *ctx, const IdmSyntax *template_ctx, IdmError *err) {
    IdmValue value = idm_syntax_value(ctx->rt, template_ctx, err);
    if (err && err->present) return NULL;
    return idm_core_literal(value, template_ctx->span);
}

static IdmCore *syntax_build_core(ExpandContext *ctx, IdmSyntaxBuildKind kind, IdmCore *payload, const IdmSyntax *template_ctx, IdmSpan span, IdmError *err) {
    if (kind != IDM_SYNTAX_BUILD_NIL && !payload) {
        if (!(err && err->present)) idm_error_oom(err, span);
        return NULL;
    }
    IdmCore *context = syntax_context_literal(ctx, template_ctx, err);
    if (!context) { idm_core_free(payload); return NULL; }
    IdmCore *core = idm_core_syntax_build(kind, context, payload, span);
    if (!core) {
        idm_core_free(context);
        idm_core_free(payload);
        idm_error_oom(err, span);
        return NULL;
    }
    return core;
}

static IdmCore *template_items_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmSpan span, const TemplateListSpec *spec, IdmError *err) {
    IdmCore *tail = idm_core_literal(idm_empty_list(), span);
    if (!tail) { idm_error_oom(err, span); return NULL; }
    for (size_t i = count; i > start; i--) {
        const IdmSyntax *item = items[i - 1u];
        const IdmSyntax *splice_item = NULL;
        if (spec->splice(item, &splice_item)) {
            if (splice_item->as.seq.count != 2) {
                idm_core_free(tail);
                return expand_error(err, splice_item->span, "%s expects one expression", spec->splice_name);
            }
            IdmCore *spliced = expand_syntax(ctx, splice_item->as.seq.items[1], err);
            if (!spliced) { idm_core_free(tail); return NULL; }
            IdmCore *append = idm_core_list_append(spliced, tail, splice_item->span);
            if (!append) { idm_core_free(spliced); idm_core_free(tail); idm_error_oom(err, splice_item->span); return NULL; }
            tail = append;
        } else {
            IdmCore *value = spec->item(ctx, item, spec->user, err);
            if (!value) { idm_core_free(tail); return NULL; }
            IdmCore *cons = idm_core_list_cons(value, tail, item->span);
            if (!cons) { idm_core_free(value); idm_core_free(tail); idm_error_oom(err, item->span); return NULL; }
            tail = cons;
        }
    }
    return tail;
}

static bool quasisyntax_splice_item(const IdmSyntax *item, const IdmSyntax **out_splice) {
    const IdmSyntax *splice_item = item;
    if (syn_is_form(item, "%-expr") && item->as.seq.count == 2 && syn_is_form(item->as.seq.items[1], "%-unsyntax-splicing")) splice_item = item->as.seq.items[1];
    if (!syn_is_form(splice_item, "%-unsyntax-splicing")) return false;
    *out_splice = splice_item;
    return true;
}

static bool quasiquote_splice_item(const IdmSyntax *item, const IdmSyntax **out_splice) {
    if (!syn_is_form(item, "%-unquote-splicing")) return false;
    *out_splice = item;
    return true;
}

static IdmCore *template_item_core(ExpandContext *ctx, const IdmSyntax *item, const void *user, IdmError *err) {
    return template_walk_core(ctx, item, user, err);
}

static IdmCore *template_splice_list_core(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmSpan span, const TemplateContext *list_ctx, TemplateSpliceFn splice, const char *splice_name, IdmError *err) {
    TemplateListSpec spec = { splice, template_item_core, list_ctx, splice_name };
    return template_items_list(ctx, items, start, count, span, &spec, err);
}

static bool template_quote_datum_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, const TemplateContext *spec, IdmValue *out, IdmError *err) {
    IdmValue list = idm_empty_list();
    for (size_t i = count; i > start; i--) {
        IdmValue item = idm_nil();
        if (!template_walk_value(ctx, items[i - 1u], spec, &item, err)) return false;
        list = idm_cons(ctx->rt, item, list, err);
        if (err && err->present) return false;
    }
    *out = list;
    return true;
}

static bool template_quote_sequence(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValue *out, IdmError *err) {
    size_t n = t->as.seq.count;
    IdmValue *vals = n ? calloc(n, sizeof(*vals)) : NULL;
    if (n && !vals) return idm_error_oom(err, t->span);
    for (size_t i = 0; i < n; i++) {
        if (!template_walk_value(ctx, t->as.seq.items[i], spec, &vals[i], err)) { free(vals); return false; }
    }
    *out = t->kind == IDM_SYN_VECTOR ? idm_vector(ctx->rt, vals, n, err) : idm_tuple(ctx->rt, vals, n, err);
    free(vals);
    return !(err && err->present);
}

static bool template_quote_dict(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValue *out, IdmError *err) {
    if (t->as.seq.count % 2u != 0) return idm_error_set(err, t->span, "dict literal requires key/value pairs");
    size_t pairs = t->as.seq.count / 2u;
    IdmDictEntry *entries = pairs ? calloc(pairs, sizeof(*entries)) : NULL;
    if (pairs && !entries) return idm_error_oom(err, t->span);
    for (size_t i = 0; i < pairs; i++) {
        if (!template_walk_value(ctx, t->as.seq.items[i * 2u], spec, &entries[i].key, err) ||
            !template_walk_value(ctx, t->as.seq.items[i * 2u + 1u], spec, &entries[i].value, err)) { free(entries); return false; }
    }
    *out = idm_dict(ctx->rt, entries, pairs, err);
    free(entries);
    return !(err && err->present);
}

static IdmCore *template_value_sequence_core(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValueSequenceKind kind, IdmError *err) {
    IdmCore *seq = idm_core_value_sequence(kind, t->span);
    if (!seq) { idm_error_oom(err, t->span); return NULL; }
    for (size_t i = 0; i < t->as.seq.count; i++) {
        IdmCore *elem = template_walk_core(ctx, t->as.seq.items[i], spec, err);
        if (!elem) { idm_core_free(seq); return NULL; }
        if (!idm_core_value_sequence_add(seq, elem)) { idm_core_free(elem); idm_core_free(seq); idm_error_oom(err, t->span); return NULL; }
    }
    return seq;
}

static IdmSyntaxBuildKind template_syntax_sequence_kind(IdmSyntaxKind kind) {
    return kind == IDM_SYN_LIST ? IDM_SYNTAX_BUILD_LIST :
           kind == IDM_SYN_VECTOR ? IDM_SYNTAX_BUILD_VECTOR :
           kind == IDM_SYN_TUPLE ? IDM_SYNTAX_BUILD_TUPLE :
           IDM_SYNTAX_BUILD_DICT;
}

static IdmCore *template_quasisyntax_leaf_core(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmError *err) {
    switch (t->kind) {
        case IDM_SYN_WORD:
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_WORD, string_literal(ctx, t->as.text, t->span, err), spec->syntax_ctx, t->span, err);
        case IDM_SYN_ATOM:
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_ATOM, string_literal(ctx, t->as.text, t->span, err), spec->syntax_ctx, t->span, err);
        case IDM_SYN_INT: {
            IdmValue lit = idm_int_promote(ctx->rt, t->as.integer, err);
            if (err && err->present) return NULL;
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_INT, idm_core_literal(lit, t->span), spec->syntax_ctx, t->span, err);
        }
        case IDM_SYN_BIGINT: {
            bool ok = false;
            IdmValue lit = idm_int_from_decimal(ctx->rt, t->as.text, strlen(t->as.text), &ok, err);
            if (err && err->present) return NULL;
            if (!ok) { idm_error_set(err, t->span, "invalid integer literal"); return NULL; }
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_INT, idm_core_literal(lit, t->span), spec->syntax_ctx, t->span, err);
        }
        case IDM_SYN_FLOAT:
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_FLOAT, idm_core_literal(idm_float(ctx->rt, t->as.real, err), t->span), spec->syntax_ctx, t->span, err);
        case IDM_SYN_STRING:
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_STRING, string_literal(ctx, t->as.text, t->span, err), spec->syntax_ctx, t->span, err);
        case IDM_SYN_NIL:
            return syntax_build_core(ctx, IDM_SYNTAX_BUILD_NIL, NULL, spec->syntax_ctx, t->span, err);
        default:
            return expand_error(err, t->span, "unsupported quasisyntax template node");
    }
}

static const char *quote_symbol_for_head(const char *head) {
    if (strcmp(head, "%-quote") == 0) return "quote";
    if (strcmp(head, "%-quasiquote") == 0) return "quasiquote";
    if (strcmp(head, "%-unquote") == 0) return "unquote";
    if (strcmp(head, "%-unquote-splicing") == 0) return "unquote-splicing";
    return NULL;
}

static bool template_walk(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValue *out_value, IdmCore **out_core, IdmError *err) {
    if (spec->mode == TEMPLATE_QUOTE) {
        switch (t->kind) {
            case IDM_SYN_WORD: *out_value = idm_word(ctx->rt, t->as.text); return true;
            case IDM_SYN_NIL:
            case IDM_SYN_ATOM:
            case IDM_SYN_INT:
            case IDM_SYN_BIGINT:
            case IDM_SYN_FLOAT:
            case IDM_SYN_STRING:
                return value_from_literal_syntax(ctx, t, out_value, err);
            case IDM_SYN_VECTOR:
            case IDM_SYN_TUPLE:
                return template_quote_sequence(ctx, t, spec, out_value, err);
            case IDM_SYN_DICT:
                return template_quote_dict(ctx, t, spec, out_value, err);
            case IDM_SYN_LIST: {
                if (t->as.seq.count == 0) { *out_value = idm_empty_list(); return true; }
                const char *head = t->as.seq.items[0]->kind == IDM_SYN_WORD ? t->as.seq.items[0]->as.text : "";
                if (strcmp(head, "%-expr") == 0) return template_quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, spec, out_value, err);
                if (strcmp(head, "%-group") == 0 || strcmp(head, "%-layout-group") == 0) {
                    if (t->as.seq.count != 2) return idm_error_set(err, t->span, "%-group expects one child");
                    const IdmSyntax *child = t->as.seq.items[1];
                    if (syn_is_form(child, "%-expr")) return template_quote_datum_list(ctx, child->as.seq.items, 1, child->as.seq.count, spec, out_value, err);
                    return template_quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, spec, out_value, err);
                }
                const char *sym = quote_symbol_for_head(head);
                if (sym) {
                    if (t->as.seq.count != 2) return idm_error_set(err, t->span, "%s expects one child", head);
                    IdmValue inner = idm_nil();
                    if (!template_walk_value(ctx, t->as.seq.items[1], spec, &inner, err)) return false;
                    IdmValue tail = idm_cons(ctx->rt, inner, idm_empty_list(), err);
                    if (err && err->present) return false;
                    *out_value = idm_cons(ctx->rt, idm_word(ctx->rt, sym), tail, err);
                    return !(err && err->present);
                }
                return idm_error_set(err, t->span, "unsupported quote template");
            }
        }
        return idm_error_set(err, t->span, "unsupported quote template");
    }

    if (spec->mode == TEMPLATE_QUASIQUOTE) {
        if (syn_is_form(t, "%-unquote")) {
            if (t->as.seq.count != 2) { expand_error(err, t->span, "unquote expects one expression"); return false; }
            *out_core = expand_syntax(ctx, t->as.seq.items[1], err);
            return *out_core != NULL;
        }
        if (syn_is_form(t, "%-unquote-splicing")) {
            expand_error(err, t->span, "unquote-splicing is only valid inside a sequence");
            return false;
        }
        switch (t->kind) {
            case IDM_SYN_NIL:
            case IDM_SYN_WORD:
            case IDM_SYN_ATOM:
            case IDM_SYN_INT:
            case IDM_SYN_BIGINT:
            case IDM_SYN_FLOAT:
            case IDM_SYN_STRING:
                *out_core = datum_constant(ctx, t, err);
                return *out_core != NULL;
            case IDM_SYN_VECTOR:
                *out_core = template_value_sequence_core(ctx, t, spec, IDM_VALUE_SEQ_VECTOR, err);
                return *out_core != NULL;
            case IDM_SYN_TUPLE:
                *out_core = template_value_sequence_core(ctx, t, spec, IDM_VALUE_SEQ_TUPLE, err);
                return *out_core != NULL;
            case IDM_SYN_DICT:
                if (t->as.seq.count % 2u != 0) { expand_error(err, t->span, "dict literal requires key/value pairs"); return false; }
                *out_core = template_value_sequence_core(ctx, t, spec, IDM_VALUE_SEQ_DICT, err);
                return *out_core != NULL;
            case IDM_SYN_LIST: {
                if (t->as.seq.count == 0) { *out_core = datum_constant(ctx, t, err); return *out_core != NULL; }
                const char *head = t->as.seq.items[0]->kind == IDM_SYN_WORD ? t->as.seq.items[0]->as.text : "";
                if (strcmp(head, "%-expr") == 0) {
                    *out_core = template_splice_list_core(ctx, t->as.seq.items, 1, t->as.seq.count, t->span, spec, quasiquote_splice_item, "unquote-splicing", err);
                    return *out_core != NULL;
                }
                if (strcmp(head, "%-group") == 0 || strcmp(head, "%-layout-group") == 0) {
                    if (t->as.seq.count != 2) { expand_error(err, t->span, "%-group expects one child"); return false; }
                    const IdmSyntax *child = t->as.seq.items[1];
                    if (syn_is_form(child, "%-expr")) {
                        *out_core = template_splice_list_core(ctx, child->as.seq.items, 1, child->as.seq.count, t->span, spec, quasiquote_splice_item, "unquote-splicing", err);
                        return *out_core != NULL;
                    }
                    *out_core = template_walk_core(ctx, child, spec, err);
                    return *out_core != NULL;
                }
                *out_core = datum_constant(ctx, t, err);
                return *out_core != NULL;
            }
        }
        expand_error(err, t->span, "unsupported quasiquote template");
        return false;
    }

    if (syn_is_form(t, "%-unsyntax")) {
        if (t->as.seq.count != 2) { expand_error(err, t->span, "unsyntax expects one expression"); return false; }
        *out_core = expand_syntax(ctx, t->as.seq.items[1], err);
        return *out_core != NULL;
    }
    if (syn_is_form(t, "%-unsyntax-splicing")) {
        expand_error(err, t->span, "unsyntax-splicing is only valid inside sequence templates");
        return false;
    }
    if ((syn_is_form(t, "%-group") || syn_is_form(t, "%-layout-group")) && t->as.seq.count == 2 && syn_is_form(t->as.seq.items[1], "%-expr") && t->as.seq.items[1]->as.seq.count == 2 && syn_is_form(t->as.seq.items[1]->as.seq.items[1], "%-body")) {
        *out_core = template_walk_core(ctx, t->as.seq.items[1]->as.seq.items[1], spec, err);
        return *out_core != NULL;
    }
    if (syn_is_form(t, "%-expr") || syn_is_form(t, "%-body")) {
        IdmCore *items = template_splice_list_core(ctx, t->as.seq.items + 1, 0u, t->as.seq.count - 1u, spec->syntax_ctx->span, spec, quasisyntax_splice_item, "unsyntax-splicing", err);
        if (!items) return false;
        IdmSyntaxBuildKind kind = syn_is_form(t, "%-expr") ? IDM_SYNTAX_BUILD_EXPR : IDM_SYNTAX_BUILD_BODY;
        *out_core = syntax_build_core(ctx, kind, items, spec->syntax_ctx, t->span, err);
        return *out_core != NULL;
    }
    if (syn_is_form(t, "%-group") || syn_is_form(t, "%-layout-group")) {
        if (t->as.seq.count != 2) { expand_error(err, t->span, "%-group expects one child"); return false; }
        IdmCore *child = template_walk_core(ctx, t->as.seq.items[1], spec, err);
        if (!child) return false;
        *out_core = syntax_build_core(ctx, IDM_SYNTAX_BUILD_GROUP, child, spec->syntax_ctx, t->span, err);
        return *out_core != NULL;
    }
    switch (t->kind) {
        case IDM_SYN_NIL:
        case IDM_SYN_ATOM:
        case IDM_SYN_INT:
        case IDM_SYN_BIGINT:
        case IDM_SYN_FLOAT:
        case IDM_SYN_WORD:
        case IDM_SYN_STRING:
            *out_core = template_quasisyntax_leaf_core(ctx, t, spec, err);
            return *out_core != NULL;
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
        case IDM_SYN_LIST: {
            IdmCore *items = template_splice_list_core(ctx, t->as.seq.items, 0u, t->as.seq.count, spec->syntax_ctx->span, spec, quasisyntax_splice_item, "unsyntax-splicing", err);
            if (!items) return false;
            *out_core = syntax_build_core(ctx, template_syntax_sequence_kind(t->kind), items, spec->syntax_ctx, t->span, err);
            return *out_core != NULL;
        }
    }
    expand_error(err, t->span, "unsupported quasisyntax template node");
    return false;
}

static bool template_walk_value(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmValue *out, IdmError *err) {
    IdmCore *ignored = NULL;
    return template_walk(ctx, t, spec, out, &ignored, err);
}

static IdmCore *template_walk_core(ExpandContext *ctx, const IdmSyntax *t, const TemplateContext *spec, IdmError *err) {
    IdmValue ignored = idm_nil();
    IdmCore *core = NULL;
    if (!template_walk(ctx, t, spec, &ignored, &core, err)) return NULL;
    return core;
}

static bool quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err) {
    TemplateContext spec = { TEMPLATE_QUOTE, NULL };
    return template_walk_value(ctx, t, &spec, out, err);
}

static IdmCore *quasisyntax_template(ExpandContext *ctx, const IdmSyntax *template, const IdmSyntax *template_ctx, IdmError *err) {
    TemplateContext spec = { TEMPLATE_QUASISYNTAX, template_ctx };
    return template_walk_core(ctx, template, &spec, err);
}

IdmCore *expand_form_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasisyntax expects one child");
    return quasisyntax_template(ctx, syn->as.seq.items[1], syn->as.seq.items[1], err);
}

bool expand_quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err) {
    return quote_datum(ctx, t, out, err);
}

IdmCore *expand_form_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quote expects one child");
    IdmValue value = idm_nil();
    if (!quote_datum(ctx, syn->as.seq.items[1], &value, err)) return NULL;
    return idm_core_literal(value, syn->span);
}

static IdmCore *datum_constant(ExpandContext *ctx, const IdmSyntax *t, IdmError *err) {
    IdmValue value = idm_nil();
    if (!quote_datum(ctx, t, &value, err)) return NULL;
    return idm_core_literal(value, t->span);
}

static IdmCore *quasiquote_template(ExpandContext *ctx, const IdmSyntax *t, IdmError *err) {
    TemplateContext spec = { TEMPLATE_QUASIQUOTE, NULL };
    return template_walk_core(ctx, t, &spec, err);
}

IdmCore *expand_form_quasiquote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasiquote expects one child");
    return quasiquote_template(ctx, syn->as.seq.items[1], err);
}

IdmCore *expand_form_string(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count <= 1) {
        return string_literal(ctx, "", syn->span, err);
    }
    IdmCore *concat = idm_core_string_concat(syn->span);
    if (!concat) { idm_error_oom(err, syn->span); return NULL; }
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IdmSyntax *part = syn->as.seq.items[i];
        IdmCore *pc = expand_syntax(ctx, part, err);
        if (!pc) {
            idm_core_free(concat);
            return NULL;
        }
        if (!idm_core_string_concat_add(concat, pc)) {
            idm_core_free(pc);
            idm_core_free(concat);
            idm_error_oom(err, syn->span);
            return NULL;
        }
    }
    return concat;
}
