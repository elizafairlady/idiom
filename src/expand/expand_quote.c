#include "internal.h"

static IshCore *primitive_app(IshPrimitive primitive, IshSpan span);
static bool primitive_app_add(IshCore *app, IshCore *arg, IshError *err, IshSpan span);
static IshCore *string_literal(ExpandContext *ctx, const char *text, IshSpan span, IshError *err);
static IshCore *syntax_context_literal(ExpandContext *ctx, const IshSyntax *template_ctx, IshError *err);
static IshCore *syntax_constructor_call(ExpandContext *ctx, IshPrimitive primitive, const IshSyntax *template_ctx, IshError *err);
static IshCore *quasisyntax_items_list(ExpandContext *ctx, IshSyntax *const *items, size_t count, const IshSyntax *template_ctx, IshError *err);
static IshCore *quasisyntax_template(ExpandContext *ctx, const IshSyntax *template, const IshSyntax *template_ctx, IshError *err);
static bool quote_datum_list(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshValue *out, IshError *err);
static const char *quote_symbol_for_head(const char *head);
static bool quote_datum(ExpandContext *ctx, const IshSyntax *t, IshValue *out, IshError *err);
static IshCore *datum_constant(ExpandContext *ctx, const IshSyntax *t, IshError *err);
static IshCore *quasiquote_list(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshSpan span, IshError *err);
static IshCore *quasiquote_container(ExpandContext *ctx, const IshSyntax *t, IshPrimitive prim, IshError *err);
static IshCore *quasiquote_template(ExpandContext *ctx, const IshSyntax *t, IshError *err);
static IshCore *expand_command_sub(ExpandContext *ctx, const IshSyntax *part, IshError *err);

static IshCore *primitive_app(IshPrimitive primitive, IshSpan span) {
    return ish_core_app(ish_core_primitive(primitive, span), span);
}

static bool primitive_app_add(IshCore *app, IshCore *arg, IshError *err, IshSpan span) {
    if (!arg || !ish_core_app_add_arg(app, arg)) {
        ish_core_free(arg);
        ish_core_free(app);
        ish_error_oom(err, span);
        return false;
    }
    return true;
}

static IshCore *string_literal(ExpandContext *ctx, const char *text, IshSpan span, IshError *err) {
    IshValue value = ish_string(ctx->rt, text, err);
    if (err && err->present) return NULL;
    return ish_core_literal(value, span);
}

static IshCore *syntax_context_literal(ExpandContext *ctx, const IshSyntax *template_ctx, IshError *err) {
    IshValue value = ish_syntax_value(ctx->rt, template_ctx, err);
    if (err && err->present) return NULL;
    return ish_core_literal(value, template_ctx->span);
}

static IshCore *syntax_constructor_call(ExpandContext *ctx, IshPrimitive primitive, const IshSyntax *template_ctx, IshError *err) {
    IshCore *app = primitive_app(primitive, template_ctx->span);
    if (!app) { ish_error_oom(err, template_ctx->span); return NULL; }
    if (!primitive_app_add(app, syntax_context_literal(ctx, template_ctx, err), err, template_ctx->span)) return NULL;
    return app;
}

static IshCore *quasisyntax_items_list(ExpandContext *ctx, IshSyntax *const *items, size_t count, const IshSyntax *template_ctx, IshError *err) {
    IshCore *tail = ish_core_literal(ish_nil(), template_ctx->span);
    if (!tail) { ish_error_oom(err, template_ctx->span); return NULL; }
    for (size_t i = count; i > 0; i--) {
        const IshSyntax *item = items[i - 1u];
        const IshSyntax *splice_item = item;
        if (syn_is_protocol(item, "%-expr") && item->as.seq.count == 2 && syn_is_protocol(item->as.seq.items[1], "%-unsyntax-splicing")) splice_item = item->as.seq.items[1];
        bool splice = syn_is_protocol(splice_item, "%-unsyntax-splicing");
        if (splice) {
            if (splice_item->as.seq.count != 2) {
                ish_core_free(tail);
                return expand_error(err, splice_item->span, "unsyntax-splicing expects one expression");
            }
            IshCore *spliced = expand_syntax(ctx, splice_item->as.seq.items[1], err);
            if (!spliced) { ish_core_free(tail); return NULL; }
            IshCore *app = primitive_app(ISH_PRIM_APPEND, splice_item->span);
            if (!app || !primitive_app_add(app, spliced, err, splice_item->span) || !primitive_app_add(app, tail, err, splice_item->span)) return NULL;
            tail = app;
        } else {
            IshCore *value = quasisyntax_template(ctx, item, template_ctx, err);
            if (!value) { ish_core_free(tail); return NULL; }
            IshCore *app = primitive_app(ISH_PRIM_CONS, item->span);
            if (!app || !primitive_app_add(app, value, err, item->span) || !primitive_app_add(app, tail, err, item->span)) return NULL;
            tail = app;
        }
    }
    return tail;
}

static IshCore *quasisyntax_template(ExpandContext *ctx, const IshSyntax *template, const IshSyntax *template_ctx, IshError *err) {
    if (syn_is_protocol(template, "%-unsyntax")) {
        if (template->as.seq.count != 2) return expand_error(err, template->span, "unsyntax expects one expression");
        return expand_syntax(ctx, template->as.seq.items[1], err);
    }
    if (syn_is_protocol(template, "%-unsyntax-splicing")) return expand_error(err, template->span, "unsyntax-splicing is only valid inside sequence templates");
    if (syn_is_protocol(template, "%-group") && template->as.seq.count == 2 && syn_is_protocol(template->as.seq.items[1], "%-expr") && template->as.seq.items[1]->as.seq.count == 2 && syn_is_protocol(template->as.seq.items[1]->as.seq.items[1], "%-body")) {
        return quasisyntax_template(ctx, template->as.seq.items[1]->as.seq.items[1], template_ctx, err);
    }
    if (syn_is_protocol(template, "%-expr") || syn_is_protocol(template, "%-body")) {
        IshCore *items = quasisyntax_items_list(ctx, template->as.seq.items + 1, template->as.seq.count - 1u, template_ctx, err);
        if (!items) return NULL;
        IshPrimitive prim = syn_is_protocol(template, "%-expr") ? ISH_PRIM_MAKE_SYNTAX_EXPR : ISH_PRIM_MAKE_SYNTAX_BODY;
        IshCore *app = syntax_constructor_call(ctx, prim, template_ctx, err);
        if (!app) { ish_core_free(items); return NULL; }
        if (!primitive_app_add(app, items, err, template->span)) return NULL;
        return app;
    }
    if (syn_is_protocol(template, "%-group")) {
        if (template->as.seq.count != 2) return expand_error(err, template->span, "%-group expects one child");
        IshCore *child = quasisyntax_template(ctx, template->as.seq.items[1], template_ctx, err);
        if (!child) return NULL;
        IshCore *app = syntax_constructor_call(ctx, ISH_PRIM_MAKE_SYNTAX_GROUP, template_ctx, err);
        if (!app) { ish_core_free(child); return NULL; }
        if (!primitive_app_add(app, child, err, template->span)) return NULL;
        return app;
    }

    if (template->kind == ISH_SYN_WORD) {
        IshCore *app = syntax_constructor_call(ctx, ISH_PRIM_MAKE_SYNTAX_WORD, template_ctx, err);
        if (!app) return NULL;
        if (!primitive_app_add(app, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return app;
    }
    if (template->kind == ISH_SYN_ATOM) {
        IshCore *app = syntax_constructor_call(ctx, ISH_PRIM_MAKE_SYNTAX_ATOM, template_ctx, err);
        if (!app) return NULL;
        if (!primitive_app_add(app, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return app;
    }
    if (template->kind == ISH_SYN_INT) {
        IshCore *app = syntax_constructor_call(ctx, ISH_PRIM_MAKE_SYNTAX_INT, template_ctx, err);
        if (!app) return NULL;
        if (!primitive_app_add(app, ish_core_literal(ish_int(template->as.integer), template->span), err, template->span)) return NULL;
        return app;
    }
    if (template->kind == ISH_SYN_STRING) {
        IshCore *app = syntax_constructor_call(ctx, ISH_PRIM_MAKE_SYNTAX_STRING, template_ctx, err);
        if (!app) return NULL;
        if (!primitive_app_add(app, string_literal(ctx, template->as.text, template->span, err), err, template->span)) return NULL;
        return app;
    }
    if (template->kind == ISH_SYN_NIL) {
        IshCore *app = primitive_app(ISH_PRIM_DATUM_TO_SYNTAX, template->span);
        if (!app) { ish_error_oom(err, template->span); return NULL; }
        if (!primitive_app_add(app, syntax_context_literal(ctx, template_ctx, err), err, template->span)) return NULL;
        if (!primitive_app_add(app, ish_core_literal(ish_nil(), template->span), err, template->span)) return NULL;
        return app;
    }
    if (template->kind == ISH_SYN_LIST || template->kind == ISH_SYN_VECTOR || template->kind == ISH_SYN_TUPLE) {
        IshCore *items = quasisyntax_items_list(ctx, template->as.seq.items, template->as.seq.count, template_ctx, err);
        if (!items) return NULL;
        IshPrimitive prim = template->kind == ISH_SYN_LIST ? ISH_PRIM_MAKE_SYNTAX_LIST : (template->kind == ISH_SYN_VECTOR ? ISH_PRIM_MAKE_SYNTAX_VECTOR : ISH_PRIM_MAKE_SYNTAX_TUPLE);
        IshCore *app = syntax_constructor_call(ctx, prim, template_ctx, err);
        if (!app) { ish_core_free(items); return NULL; }
        if (template->kind == ISH_SYN_LIST) {
            const char *shape = template->as.seq.shape == ISH_SEQ_BRACKET ? "bracket" : "paren";
            if (!primitive_app_add(app, ish_core_literal(ish_atom(ctx->rt, shape), template->span), err, template->span)) { ish_core_free(items); return NULL; }
        }
        if (!primitive_app_add(app, items, err, template->span)) return NULL;
        return app;
    }
    return expand_error(err, template->span, "unsupported quasisyntax template node");
}

IshCore *expand_protocol_quasisyntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasisyntax expects one child");
    return quasisyntax_template(ctx, syn->as.seq.items[1], syn->as.seq.items[1], err);
}

static bool quote_datum_list(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshValue *out, IshError *err) {
    IshValue list = ish_nil();
    for (size_t i = count; i > start; i--) {
        IshValue item = ish_nil();
        if (!quote_datum(ctx, items[i - 1u], &item, err)) return false;
        list = ish_cons(ctx->rt, item, list, err);
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

static bool quote_datum(ExpandContext *ctx, const IshSyntax *t, IshValue *out, IshError *err) {
    switch (t->kind) {
        case ISH_SYN_NIL: *out = ish_nil(); return true;
        case ISH_SYN_WORD: *out = ish_word(ctx->rt, t->as.text); return true;
        case ISH_SYN_ATOM: *out = strcmp(t->as.text, "nil") == 0 ? ish_nil() : ish_atom(ctx->rt, t->as.text); return true;
        case ISH_SYN_INT: *out = ish_int(t->as.integer); return true;
        case ISH_SYN_FLOAT: *out = ish_float(t->as.real); return true;
        case ISH_SYN_STRING: *out = ish_string(ctx->rt, t->as.text, err); return !(err && err->present);
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE: {
            size_t n = t->as.seq.count;
            IshValue *vals = n ? calloc(n, sizeof(*vals)) : NULL;
            if (n && !vals) return ish_error_oom(err, t->span);
            for (size_t i = 0; i < n; i++) {
                if (!quote_datum(ctx, t->as.seq.items[i], &vals[i], err)) { free(vals); return false; }
            }
            *out = t->kind == ISH_SYN_VECTOR ? ish_vector(ctx->rt, vals, n, err) : ish_tuple(ctx->rt, vals, n, err);
            free(vals);
            return !(err && err->present);
        }
        case ISH_SYN_DICT: {
            if (t->as.seq.count % 2u != 0) return ish_error_set(err, t->span, "dict literal requires key/value pairs");
            size_t pairs = t->as.seq.count / 2u;
            IshDictEntry *entries = pairs ? calloc(pairs, sizeof(*entries)) : NULL;
            if (pairs && !entries) return ish_error_oom(err, t->span);
            for (size_t i = 0; i < pairs; i++) {
                if (!quote_datum(ctx, t->as.seq.items[i * 2u], &entries[i].key, err) ||
                    !quote_datum(ctx, t->as.seq.items[i * 2u + 1u], &entries[i].value, err)) { free(entries); return false; }
            }
            *out = ish_dict(ctx->rt, entries, pairs, err);
            free(entries);
            return !(err && err->present);
        }
        case ISH_SYN_LIST: {
            if (t->as.seq.shape == ISH_SEQ_BRACKET) return quote_datum_list(ctx, t->as.seq.items, 0, t->as.seq.count, out, err);
            const char *head = (t->as.seq.count > 0 && t->as.seq.items[0]->kind == ISH_SYN_WORD) ? t->as.seq.items[0]->as.text : "";
            if (strcmp(head, "%-expr") == 0) return quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, out, err);
            if (strcmp(head, "%-group") == 0) {
                if (t->as.seq.count != 2) return ish_error_set(err, t->span, "%-group expects one child");
                const IshSyntax *child = t->as.seq.items[1];
                if (syn_is_protocol(child, "%-expr")) return quote_datum_list(ctx, child->as.seq.items, 1, child->as.seq.count, out, err);
                return quote_datum_list(ctx, t->as.seq.items, 1, t->as.seq.count, out, err);
            }
            const char *sym = quote_symbol_for_head(head);
            if (sym) {
                if (t->as.seq.count != 2) return ish_error_set(err, t->span, "%s expects one child", head);
                IshValue inner = ish_nil();
                if (!quote_datum(ctx, t->as.seq.items[1], &inner, err)) return false;
                IshValue tail = ish_cons(ctx->rt, inner, ish_nil(), err);
                if (err && err->present) return false;
                *out = ish_cons(ctx->rt, ish_word(ctx->rt, sym), tail, err);
                return !(err && err->present);
            }
            return ish_error_set(err, t->span, "unsupported quote template");
        }
    }
    return ish_error_set(err, t->span, "unsupported quote template");
}

IshCore *expand_protocol_quote(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quote expects one child");
    IshValue value = ish_nil();
    if (!quote_datum(ctx, syn->as.seq.items[1], &value, err)) return NULL;
    return ish_core_literal(value, syn->span);
}

static IshCore *datum_constant(ExpandContext *ctx, const IshSyntax *t, IshError *err) {
    IshValue value = ish_nil();
    if (!quote_datum(ctx, t, &value, err)) return NULL;
    return ish_core_literal(value, t->span);
}

static IshCore *quasiquote_list(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t count, IshSpan span, IshError *err) {
    IshCore *tail = ish_core_literal(ish_nil(), span);
    if (!tail) { ish_error_oom(err, span); return NULL; }
    for (size_t i = count; i > start; i--) {
        const IshSyntax *item = items[i - 1u];
        if (syn_is_protocol(item, "%-unquote-splicing")) {
            if (item->as.seq.count != 2) { ish_core_free(tail); return expand_error(err, item->span, "unquote-splicing expects one expression"); }
            IshCore *spliced = expand_syntax(ctx, item->as.seq.items[1], err);
            if (!spliced) { ish_core_free(tail); return NULL; }
            IshCore *app = primitive_app(ISH_PRIM_APPEND, item->span);
            if (!app || !primitive_app_add(app, spliced, err, item->span) || !primitive_app_add(app, tail, err, item->span)) return NULL;
            tail = app;
        } else {
            IshCore *value = quasiquote_template(ctx, item, err);
            if (!value) { ish_core_free(tail); return NULL; }
            IshCore *app = primitive_app(ISH_PRIM_CONS, item->span);
            if (!app || !primitive_app_add(app, value, err, item->span) || !primitive_app_add(app, tail, err, item->span)) return NULL;
            tail = app;
        }
    }
    return tail;
}

static IshCore *quasiquote_container(ExpandContext *ctx, const IshSyntax *t, IshPrimitive prim, IshError *err) {
    IshCore *app = primitive_app(prim, t->span);
    if (!app) { ish_error_oom(err, t->span); return NULL; }
    for (size_t i = 0; i < t->as.seq.count; i++) {
        IshCore *elem = quasiquote_template(ctx, t->as.seq.items[i], err);
        if (!elem || !ish_core_app_add_arg(app, elem)) {
            ish_core_free(elem);
            ish_core_free(app);
            if (!err->present) ish_error_oom(err, t->span);
            return NULL;
        }
    }
    return app;
}

static IshCore *quasiquote_template(ExpandContext *ctx, const IshSyntax *t, IshError *err) {
    if (syn_is_protocol(t, "%-unquote")) {
        if (t->as.seq.count != 2) return expand_error(err, t->span, "unquote expects one expression");
        return expand_syntax(ctx, t->as.seq.items[1], err);
    }
    if (syn_is_protocol(t, "%-unquote-splicing")) return expand_error(err, t->span, "unquote-splicing is only valid inside a sequence");
    switch (t->kind) {
        case ISH_SYN_NIL:
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_INT:
        case ISH_SYN_FLOAT:
        case ISH_SYN_STRING:
            return datum_constant(ctx, t, err);
        case ISH_SYN_VECTOR: return quasiquote_container(ctx, t, ISH_PRIM_VECTOR, err);
        case ISH_SYN_TUPLE: return quasiquote_container(ctx, t, ISH_PRIM_TUPLE, err);
        case ISH_SYN_DICT:
            if (t->as.seq.count % 2u != 0) return expand_error(err, t->span, "dict literal requires key/value pairs");
            return quasiquote_container(ctx, t, ISH_PRIM_DICT, err);
        case ISH_SYN_LIST: {
            if (t->as.seq.shape == ISH_SEQ_BRACKET) return quasiquote_list(ctx, t->as.seq.items, 0, t->as.seq.count, t->span, err);
            const char *head = (t->as.seq.count > 0 && t->as.seq.items[0]->kind == ISH_SYN_WORD) ? t->as.seq.items[0]->as.text : "";
            if (strcmp(head, "%-expr") == 0) return quasiquote_list(ctx, t->as.seq.items, 1, t->as.seq.count, t->span, err);
            if (strcmp(head, "%-group") == 0) {
                if (t->as.seq.count != 2) return expand_error(err, t->span, "%-group expects one child");
                const IshSyntax *child = t->as.seq.items[1];
                if (syn_is_protocol(child, "%-expr")) return quasiquote_list(ctx, child->as.seq.items, 1, child->as.seq.count, t->span, err);
                return quasiquote_template(ctx, child, err);
            }
            return datum_constant(ctx, t, err);
        }
    }
    return expand_error(err, t->span, "unsupported quasiquote template");
}

IshCore *expand_protocol_quasiquote(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-quasiquote expects one child");
    return quasiquote_template(ctx, syn->as.seq.items[1], err);
}

static IshCore *expand_command_sub(ExpandContext *ctx, const IshSyntax *part, IshError *err) {
    if (part->as.seq.count != 2) return expand_error(err, part->span, "%-command-sub expects one child");
    bool saved = ctx->value_context;
    bool saved_cs = ctx->command_sub_context;
    ctx->value_context = true;
    ctx->command_sub_context = true;
    IshCore *command = expand_syntax(ctx, part->as.seq.items[1], err);
    ctx->value_context = saved;
    ctx->command_sub_context = saved_cs;
    return command;
}

IshCore *expand_protocol_string(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    IshCore *app = primitive_app(ISH_PRIM_STR, syn->span);
    if (!app) return (IshCore *)(uintptr_t)ish_error_oom(err, syn->span);
    if (syn->as.seq.count <= 1) {
        IshCore *empty = string_literal(ctx, "", syn->span, err);
        if (!empty) { ish_core_free(app); return NULL; }
        if (!ish_core_app_add_arg(app, empty)) {
            ish_core_free(empty);
            ish_core_free(app);
            return (IshCore *)(uintptr_t)ish_error_oom(err, syn->span);
        }
        return app;
    }
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IshSyntax *part = syn->as.seq.items[i];
        IshCore *pc = syn_is_protocol(part, "%-command-sub") ? expand_command_sub(ctx, part, err) : expand_syntax(ctx, part, err);
        if (!pc || !ish_core_app_add_arg(app, pc)) {
            ish_core_free(pc);
            ish_core_free(app);
            if (!err->present) ish_error_oom(err, syn->span);
            return NULL;
        }
    }
    return app;
}
