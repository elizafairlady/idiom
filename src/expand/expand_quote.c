#include "internal.h"

static IdmCore *primitive_call(IdmPrimitive primitive, IdmSpan span);
static bool primitive_call_add(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span);
static IdmCore *string_literal(ExpandContext *ctx, const char *text, IdmSpan span, IdmError *err);
static IdmCore *syntax_context_literal(ExpandContext *ctx, const IdmSyntax *template_ctx, IdmError *err);
static IdmCore *syntax_constructor_call(ExpandContext *ctx, IdmPrimitive primitive, const IdmSyntax *template_ctx, IdmError *err);
static IdmCore *quasisyntax_items_list(ExpandContext *ctx, IdmSyntax *const *items, size_t count, const IdmSyntax *template_ctx, IdmError *err);
static IdmCore *quasisyntax_template(ExpandContext *ctx, const IdmSyntax *template, const IdmSyntax *template_ctx, IdmError *err);
static bool quote_datum_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmValue *out, IdmError *err);
static const char *quote_symbol_for_head(const char *head);
static bool quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err);
static IdmCore *datum_constant(ExpandContext *ctx, const IdmSyntax *t, IdmError *err);
static IdmCore *quasiquote_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmSpan span, IdmError *err);
static IdmCore *quasiquote_container(ExpandContext *ctx, const IdmSyntax *t, IdmPrimitive prim, IdmError *err);
static IdmCore *quasiquote_template(ExpandContext *ctx, const IdmSyntax *t, IdmError *err);

static IdmCore *primitive_call(IdmPrimitive primitive, IdmSpan span) {
    return idm_core_call(idm_core_primitive(primitive, span), span);
}

static bool primitive_call_add(IdmCore *call, IdmCore *arg, IdmError *err, IdmSpan span) {
    if (!arg || !idm_core_call_add_arg(call, arg)) {
        idm_core_free(arg);
        idm_core_free(call);
        idm_error_oom(err, span);
        return false;
    }
    return true;
}

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

static IdmCore *syntax_constructor_call(ExpandContext *ctx, IdmPrimitive primitive, const IdmSyntax *template_ctx, IdmError *err) {
    IdmCore *call = primitive_call(primitive, template_ctx->span);
    if (!call) { idm_error_oom(err, template_ctx->span); return NULL; }
    if (!primitive_call_add(call, syntax_context_literal(ctx, template_ctx, err), err, template_ctx->span)) return NULL;
    return call;
}

static IdmCore *quasisyntax_items_list(ExpandContext *ctx, IdmSyntax *const *items, size_t count, const IdmSyntax *template_ctx, IdmError *err) {
    IdmCore *tail = idm_core_literal(idm_empty_list(), template_ctx->span);
    if (!tail) { idm_error_oom(err, template_ctx->span); return NULL; }
    for (size_t i = count; i > 0; i--) {
        const IdmSyntax *item = items[i - 1u];
        const IdmSyntax *splice_item = item;
        if (syn_is_form(item, "%-expr") && item->as.seq.count == 2 && syn_is_form(item->as.seq.items[1], "%-unsyntax-splicing")) splice_item = item->as.seq.items[1];
        bool splice = syn_is_form(splice_item, "%-unsyntax-splicing");
        if (splice) {
            if (splice_item->as.seq.count != 2) {
                idm_core_free(tail);
                return expand_error(err, splice_item->span, "unsyntax-splicing expects one expression");
            }
            IdmCore *spliced = expand_syntax(ctx, splice_item->as.seq.items[1], err);
            if (!spliced) { idm_core_free(tail); return NULL; }
            IdmCore *call = primitive_call(IDM_PRIM_APPEND, splice_item->span);
            if (!call || !primitive_call_add(call, spliced, err, splice_item->span) || !primitive_call_add(call, tail, err, splice_item->span)) return NULL;
            tail = call;
        } else {
            IdmCore *value = quasisyntax_template(ctx, item, template_ctx, err);
            if (!value) { idm_core_free(tail); return NULL; }
            IdmCore *call = primitive_call(IDM_PRIM_CONS, item->span);
            if (!call || !primitive_call_add(call, value, err, item->span) || !primitive_call_add(call, tail, err, item->span)) return NULL;
            tail = call;
        }
    }
    return tail;
}

static IdmCore *quasisyntax_template(ExpandContext *ctx, const IdmSyntax *template, const IdmSyntax *template_ctx, IdmError *err) {
    if (syn_is_form(template, "%-unsyntax")) {
        if (template->as.seq.count != 2) return expand_error(err, template->span, "unsyntax expects one expression");
        return expand_syntax(ctx, template->as.seq.items[1], err);
    }
    if (syn_is_form(template, "%-unsyntax-splicing")) return expand_error(err, template->span, "unsyntax-splicing is only valid inside sequence templates");
    if (syn_is_form(template, "%-group") && template->as.seq.count == 2 && syn_is_form(template->as.seq.items[1], "%-expr") && template->as.seq.items[1]->as.seq.count == 2 && syn_is_form(template->as.seq.items[1]->as.seq.items[1], "%-body")) {
        return quasisyntax_template(ctx, template->as.seq.items[1]->as.seq.items[1], template_ctx, err);
    }
    if (syn_is_form(template, "%-expr") || syn_is_form(template, "%-body")) {
        IdmCore *items = quasisyntax_items_list(ctx, template->as.seq.items + 1, template->as.seq.count - 1u, template_ctx, err);
        if (!items) return NULL;
        IdmPrimitive prim = syn_is_form(template, "%-expr") ? IDM_PRIM_MAKE_SYNTAX_EXPR : IDM_PRIM_MAKE_SYNTAX_BODY;
        IdmCore *call = syntax_constructor_call(ctx, prim, template_ctx, err);
        if (!call) { idm_core_free(items); return NULL; }
        if (!primitive_call_add(call, items, err, template->span)) return NULL;
        return call;
    }
    if (syn_is_form(template, "%-group")) {
        if (template->as.seq.count != 2) return expand_error(err, template->span, "%-group expects one child");
        IdmCore *child = quasisyntax_template(ctx, template->as.seq.items[1], template_ctx, err);
        if (!child) return NULL;
        IdmCore *call = syntax_constructor_call(ctx, IDM_PRIM_MAKE_SYNTAX_GROUP, template_ctx, err);
        if (!call) { idm_core_free(child); return NULL; }
        if (!primitive_call_add(call, child, err, template->span)) return NULL;
        return call;
    }

    if (template->kind == IDM_SYN_WORD) {
        IdmCore *call = syntax_constructor_call(ctx, IDM_PRIM_MAKE_SYNTAX_WORD, template_ctx, err);
        if (!call) return NULL;
        if (!primitive_call_add(call, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return call;
    }
    if (template->kind == IDM_SYN_ATOM) {
        IdmCore *call = syntax_constructor_call(ctx, IDM_PRIM_MAKE_SYNTAX_ATOM, template_ctx, err);
        if (!call) return NULL;
        if (!primitive_call_add(call, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return call;
    }
    if (template->kind == IDM_SYN_INT) {
        IdmCore *call = syntax_constructor_call(ctx, IDM_PRIM_MAKE_SYNTAX_INT, template_ctx, err);
        if (!call) return NULL;
        if (!primitive_call_add(call, idm_core_literal(idm_int(template->as.integer), template->span), err, template->span)) return NULL;
        return call;
    }
    if (template->kind == IDM_SYN_STRING) {
        IdmCore *call = syntax_constructor_call(ctx, IDM_PRIM_MAKE_SYNTAX_STRING, template_ctx, err);
        if (!call) return NULL;
        if (!primitive_call_add(call, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return call;
    }
    if (template->kind == IDM_SYN_NIL) {
        IdmCore *call = primitive_call(IDM_PRIM_DATUM_TO_SYNTAX, template->span);
        if (!call) { idm_error_oom(err, template->span); return NULL; }
        if (!primitive_call_add(call, syntax_context_literal(ctx, template_ctx, err), err, template->span)) return NULL;
        if (!primitive_call_add(call, idm_core_literal(idm_nil(), template->span), err, template->span)) return NULL;
        return call;
    }
    if (template->kind == IDM_SYN_LIST || template->kind == IDM_SYN_VECTOR || template->kind == IDM_SYN_TUPLE) {
        IdmCore *items = quasisyntax_items_list(ctx, template->as.seq.items, template->as.seq.count, template_ctx, err);
        if (!items) return NULL;
        IdmPrimitive prim = template->kind == IDM_SYN_LIST ? IDM_PRIM_MAKE_SYNTAX_LIST : (template->kind == IDM_SYN_VECTOR ? IDM_PRIM_MAKE_SYNTAX_VECTOR : IDM_PRIM_MAKE_SYNTAX_TUPLE);
        IdmCore *call = syntax_constructor_call(ctx, prim, template_ctx, err);
        if (!call) { idm_core_free(items); return NULL; }
        if (!primitive_call_add(call, items, err, template->span)) return NULL;
        return call;
    }
    return expand_error(err, template->span, "unsupported quasisyntax template node");
}

IdmCore *expand_form_quasisyntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasisyntax expects one child");
    return quasisyntax_template(ctx, syn->as.seq.items[1], syn->as.seq.items[1], err);
}

static bool quote_datum_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmValue *out, IdmError *err) {
    IdmValue list = idm_empty_list();
    for (size_t i = count; i > start; i--) {
        IdmValue item = idm_nil();
        if (!quote_datum(ctx, items[i - 1u], &item, err)) return false;
        list = idm_cons(ctx->rt, item, list, err);
        if (err && err->present) return false;
    }
    *out = list;
    return true;
}

static const char *quote_symbol_for_head(const char *head) {
    if (strcmp(head, "%-quote") == 0) return "quote";
    if (strcmp(head, "%-quasiquote") == 0) return "quasiquote";
    if (strcmp(head, "%-unquote") == 0) return "unquote";
    if (strcmp(head, "%-unquote-splicing") == 0) return "unquote-splicing";
    return NULL;
}

static bool quote_datum(ExpandContext *ctx, const IdmSyntax *t, IdmValue *out, IdmError *err) {
    switch (t->kind) {
        case IDM_SYN_NIL: *out = idm_nil(); return true;
        case IDM_SYN_WORD: *out = idm_word(ctx->rt, t->as.text); return true;
        case IDM_SYN_ATOM: *out = strcmp(t->as.text, "nil") == 0 ? idm_nil() : idm_atom(ctx->rt, t->as.text); return true;
        case IDM_SYN_INT: *out = idm_int(t->as.integer); return true;
        case IDM_SYN_FLOAT: *out = idm_float(t->as.real); return true;
        case IDM_SYN_STRING: *out = idm_string(ctx->rt, t->as.text, err); return !(err && err->present);
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE: {
            size_t n = t->as.seq.count;
            IdmValue *vals = n ? calloc(n, sizeof(*vals)) : NULL;
            if (n && !vals) return idm_error_oom(err, t->span);
            for (size_t i = 0; i < n; i++) {
                if (!quote_datum(ctx, t->as.seq.items[i], &vals[i], err)) { free(vals); return false; }
            }
            *out = t->kind == IDM_SYN_VECTOR ? idm_vector(ctx->rt, vals, n, err) : idm_tuple(ctx->rt, vals, n, err);
            free(vals);
            return !(err && err->present);
        }
        case IDM_SYN_DICT: {
            if (t->as.seq.count % 2u != 0) return idm_error_set(err, t->span, "dict literal requires key/value pairs");
            size_t pairs = t->as.seq.count / 2u;
            IdmDictEntry *entries = pairs ? calloc(pairs, sizeof(*entries)) : NULL;
            if (pairs && !entries) return idm_error_oom(err, t->span);
            for (size_t i = 0; i < pairs; i++) {
                if (!quote_datum(ctx, t->as.seq.items[i * 2u], &entries[i].key, err) ||
                    !quote_datum(ctx, t->as.seq.items[i * 2u + 1u], &entries[i].value, err)) { free(entries); return false; }
            }
            *out = idm_dict(ctx->rt, entries, pairs, err);
            free(entries);
            return !(err && err->present);
        }
        case IDM_SYN_LIST: {
            if (t->as.seq.count == 0) { *out = idm_empty_list(); return true; }
            const char *head = t->as.seq.items[0]->kind == IDM_SYN_WORD ? t->as.seq.items[0]->as.text : "";
            if (strcmp(head, "%-expr") == 0) return quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, out, err);
            if (strcmp(head, "%-group") == 0) {
                if (t->as.seq.count != 2) return idm_error_set(err, t->span, "%-group expects one child");
                const IdmSyntax *child = t->as.seq.items[1];
                if (syn_is_form(child, "%-expr")) return quote_datum_list(ctx, child->as.seq.items, 1, child->as.seq.count, out, err);
                return quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, out, err);
            }
            const char *sym = quote_symbol_for_head(head);
            if (sym) {
                if (t->as.seq.count != 2) return idm_error_set(err, t->span, "%s expects one child", head);
                IdmValue inner = idm_nil();
                if (!quote_datum(ctx, t->as.seq.items[1], &inner, err)) return false;
                IdmValue tail = idm_cons(ctx->rt, inner, idm_empty_list(), err);
                if (err && err->present) return false;
                *out = idm_cons(ctx->rt, idm_word(ctx->rt, sym), tail, err);
                return !(err && err->present);
            }
            return idm_error_set(err, t->span, "unsupported quote template");
        }
    }
    return idm_error_set(err, t->span, "unsupported quote template");
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

static IdmCore *quasiquote_list(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t count, IdmSpan span, IdmError *err) {
    IdmCore *tail = idm_core_literal(idm_empty_list(), span);
    if (!tail) { idm_error_oom(err, span); return NULL; }
    for (size_t i = count; i > start; i--) {
        const IdmSyntax *item = items[i - 1u];
        if (syn_is_form(item, "%-unquote-splicing")) {
            if (item->as.seq.count != 2) { idm_core_free(tail); return expand_error(err, item->span, "unquote-splicing expects one expression"); }
            IdmCore *spliced = expand_syntax(ctx, item->as.seq.items[1], err);
            if (!spliced) { idm_core_free(tail); return NULL; }
            IdmCore *call = primitive_call(IDM_PRIM_APPEND, item->span);
            if (!call || !primitive_call_add(call, spliced, err, item->span) || !primitive_call_add(call, tail, err, item->span)) return NULL;
            tail = call;
        } else {
            IdmCore *value = quasiquote_template(ctx, item, err);
            if (!value) { idm_core_free(tail); return NULL; }
            IdmCore *call = primitive_call(IDM_PRIM_CONS, item->span);
            if (!call || !primitive_call_add(call, value, err, item->span) || !primitive_call_add(call, tail, err, item->span)) return NULL;
            tail = call;
        }
    }
    return tail;
}

static IdmCore *quasiquote_container(ExpandContext *ctx, const IdmSyntax *t, IdmPrimitive prim, IdmError *err) {
    IdmCore *call = primitive_call(prim, t->span);
    if (!call) { idm_error_oom(err, t->span); return NULL; }
    for (size_t i = 0; i < t->as.seq.count; i++) {
        IdmCore *elem = quasiquote_template(ctx, t->as.seq.items[i], err);
        if (!elem || !idm_core_call_add_arg(call, elem)) {
            idm_core_free(elem);
            idm_core_free(call);
            if (!err->present) idm_error_oom(err, t->span);
            return NULL;
        }
    }
    return call;
}

static IdmCore *quasiquote_template(ExpandContext *ctx, const IdmSyntax *t, IdmError *err) {
    if (syn_is_form(t, "%-unquote")) {
        if (t->as.seq.count != 2) return expand_error(err, t->span, "unquote expects one expression");
        return expand_syntax(ctx, t->as.seq.items[1], err);
    }
    if (syn_is_form(t, "%-unquote-splicing")) return expand_error(err, t->span, "unquote-splicing is only valid inside a sequence");
    switch (t->kind) {
        case IDM_SYN_NIL:
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_INT:
        case IDM_SYN_FLOAT:
        case IDM_SYN_STRING:
            return datum_constant(ctx, t, err);
        case IDM_SYN_VECTOR: return quasiquote_container(ctx, t, IDM_PRIM_VECTOR, err);
        case IDM_SYN_TUPLE: return quasiquote_container(ctx, t, IDM_PRIM_TUPLE, err);
        case IDM_SYN_DICT:
            if (t->as.seq.count % 2u != 0) return expand_error(err, t->span, "dict literal requires key/value pairs");
            return quasiquote_container(ctx, t, IDM_PRIM_DICT, err);
        case IDM_SYN_LIST: {
            if (t->as.seq.count == 0) return datum_constant(ctx, t, err);
            const char *head = t->as.seq.items[0]->kind == IDM_SYN_WORD ? t->as.seq.items[0]->as.text : "";
            if (strcmp(head, "%-expr") == 0) return quasiquote_list(ctx, t->as.seq.items, 1, t->as.seq.count, t->span, err);
            if (strcmp(head, "%-group") == 0) {
                if (t->as.seq.count != 2) return expand_error(err, t->span, "%-group expects one child");
                const IdmSyntax *child = t->as.seq.items[1];
                if (syn_is_form(child, "%-expr")) return quasiquote_list(ctx, child->as.seq.items, 1, child->as.seq.count, t->span, err);
                return quasiquote_template(ctx, child, err);
            }
            return datum_constant(ctx, t, err);
        }
    }
    return expand_error(err, t->span, "unsupported quasiquote template");
}

IdmCore *expand_form_quasiquote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasiquote expects one child");
    return quasiquote_template(ctx, syn->as.seq.items[1], err);
}

IdmCore *expand_form_string(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmCore *call = primitive_call(IDM_PRIM_STR, syn->span);
    if (!call) return (IdmCore *)(uintptr_t)idm_error_oom(err, syn->span);
    if (syn->as.seq.count <= 1) {
        IdmCore *empty = string_literal(ctx, "", syn->span, err);
        if (!empty) { idm_core_free(call); return NULL; }
        if (!idm_core_call_add_arg(call, empty)) {
            idm_core_free(empty);
            idm_core_free(call);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, syn->span);
        }
        return call;
    }
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IdmSyntax *part = syn->as.seq.items[i];
        IdmCore *pc = expand_syntax(ctx, part, err);
        if (!pc || !idm_core_call_add_arg(call, pc)) {
            idm_core_free(pc);
            idm_core_free(call);
            if (!err->present) idm_error_oom(err, syn->span);
            return NULL;
        }
    }
    return call;
}
