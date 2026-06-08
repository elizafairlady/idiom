#include "ish/expand.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    uint32_t slot;
} LocalBinding;

typedef struct {
    char *name;
    uint32_t slot;
} ArgBinding;

typedef struct {
    char *name;
    uint32_t outer_slot;
    uint32_t capture_index;
} CaptureBinding;

typedef struct {
    size_t local_count;
    size_t local_cap;
    uint32_t next_slot;
    ArgBinding *args;
    size_t arg_count;
    size_t arg_cap;
} SavedClauseContext;

typedef struct {
    const char *name;
    uint32_t slot;
    size_t *indices;
    size_t count;
    size_t cap;
} DefnGroup;

typedef struct {
    LocalBinding *locals;
    size_t local_count;
    size_t local_cap;
    uint32_t next_slot;
    ArgBinding *args;
    size_t arg_count;
    size_t arg_cap;
    LocalBinding *outer_locals;
    size_t outer_count;
    CaptureBinding *captures;
    size_t capture_count;
    size_t capture_cap;
} SavedFunctionContext;

typedef struct {
    const char *name;
    uint8_t precedence;
    IshPrimitive primitive;
} OperatorInfo;

static const OperatorInfo OPERATORS[] = {
    {"==", 30, ISH_PRIM_EQ},
    {"<", 30, ISH_PRIM_LT},
    {"+", 50, ISH_PRIM_ADD},
    {"-", 50, ISH_PRIM_SUB},
    {"*", 60, ISH_PRIM_MUL},
};

typedef struct {
    IshRuntime *rt;
    IshBindingTable bindings;
    IshScopeSet empty_scopes;
    LocalBinding *locals;
    size_t local_count;
    size_t local_cap;
    uint32_t next_slot;
    ArgBinding *args;
    size_t arg_count;
    size_t arg_cap;
    LocalBinding *outer_locals;
    size_t outer_count;
    CaptureBinding *captures;
    size_t capture_count;
    size_t capture_cap;
} ExpandContext;

static void ctx_init(ExpandContext *ctx, IshRuntime *rt) {
    ctx->rt = rt;
    ish_binding_table_init(&ctx->bindings);
    ish_scope_set_init(&ctx->empty_scopes);
    ctx->locals = NULL;
    ctx->local_count = 0;
    ctx->local_cap = 0;
    ctx->next_slot = 0;
    ctx->args = NULL;
    ctx->arg_count = 0;
    ctx->arg_cap = 0;
    ctx->outer_locals = NULL;
    ctx->outer_count = 0;
    ctx->captures = NULL;
    ctx->capture_count = 0;
    ctx->capture_cap = 0;
}

static void local_bindings_destroy(LocalBinding *locals, size_t count) {
    for (size_t i = 0; i < count; i++) free(locals[i].name);
    free(locals);
}

static void arg_bindings_destroy(ArgBinding *args, size_t count) {
    for (size_t i = 0; i < count; i++) free(args[i].name);
    free(args);
}

static void capture_bindings_destroy(CaptureBinding *captures, size_t count) {
    for (size_t i = 0; i < count; i++) free(captures[i].name);
    free(captures);
}

static void ctx_destroy(ExpandContext *ctx) {
    ish_binding_table_destroy(&ctx->bindings);
    ish_scope_set_destroy(&ctx->empty_scopes);
    local_bindings_destroy(ctx->locals, ctx->local_count);
    arg_bindings_destroy(ctx->args, ctx->arg_count);
    capture_bindings_destroy(ctx->captures, ctx->capture_count);
}

static bool seed_primitive(ExpandContext *ctx, const char *name, IshPrimitive primitive) {
    return ish_binding_table_add(&ctx->bindings, name, 0, ISH_BIND_SPACE_DEFAULT, ISH_BIND_VALUE, &ctx->empty_scopes, (uint32_t)primitive, NULL);
}

static bool ctx_seed(ExpandContext *ctx, IshError *err) {
    if (!seed_primitive(ctx, "add", ISH_PRIM_ADD) ||
        !seed_primitive(ctx, "sub", ISH_PRIM_SUB) ||
        !seed_primitive(ctx, "mul", ISH_PRIM_MUL) ||
        !seed_primitive(ctx, "eq", ISH_PRIM_EQ) ||
        !seed_primitive(ctx, "lt", ISH_PRIM_LT)) {
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    for (size_t i = 0; i < sizeof(OPERATORS) / sizeof(OPERATORS[0]); i++) {
        if (!ish_binding_table_add(&ctx->bindings, OPERATORS[i].name, 0, ISH_BIND_SPACE_OPERATOR, ISH_BIND_OPERATOR, &ctx->empty_scopes, (uint32_t)i, NULL)) {
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
    }
    return true;
}

static bool local_push(ExpandContext *ctx, const char *name, uint32_t *out_slot) {
    if (ctx->local_count == ctx->local_cap) {
        size_t cap = ctx->local_cap ? ctx->local_cap * 2u : 8u;
        LocalBinding *locals = realloc(ctx->locals, cap * sizeof(*locals));
        if (!locals) return false;
        ctx->locals = locals;
        ctx->local_cap = cap;
    }
    char *copy = ish_strdup(name);
    if (!copy) return false;
    uint32_t slot = ctx->next_slot++;
    ctx->locals[ctx->local_count].name = copy;
    ctx->locals[ctx->local_count].slot = slot;
    ctx->local_count++;
    if (out_slot) *out_slot = slot;
    return true;
}

static void local_pop_to(ExpandContext *ctx, size_t count, uint32_t next_slot) {
    while (ctx->local_count > count) free(ctx->locals[--ctx->local_count].name);
    ctx->next_slot = next_slot;
}

static bool local_lookup(const ExpandContext *ctx, const char *name, uint32_t *out_slot) {
    for (size_t i = ctx->local_count; i > 0; i--) {
        const LocalBinding *binding = &ctx->locals[i - 1u];
        if (strcmp(binding->name, name) == 0) {
            *out_slot = binding->slot;
            return true;
        }
    }
    return false;
}

static bool arg_lookup(const ExpandContext *ctx, const char *name, uint32_t *out_slot) {
    for (size_t i = ctx->arg_count; i > 0; i--) {
        const ArgBinding *binding = &ctx->args[i - 1u];
        if (strcmp(binding->name, name) == 0) {
            *out_slot = binding->slot;
            return true;
        }
    }
    return false;
}

static bool arg_push(ExpandContext *ctx, const char *name, uint32_t *out_slot) {
    uint32_t ignored = 0;
    if (arg_lookup(ctx, name, &ignored)) return false;
    if (ctx->arg_count == ctx->arg_cap) {
        size_t cap = ctx->arg_cap ? ctx->arg_cap * 2u : 4u;
        ArgBinding *args = realloc(ctx->args, cap * sizeof(*args));
        if (!args) return false;
        ctx->args = args;
        ctx->arg_cap = cap;
    }
    char *copy = ish_strdup(name);
    if (!copy) return false;
    uint32_t slot = (uint32_t)ctx->arg_count;
    ctx->args[ctx->arg_count].name = copy;
    ctx->args[ctx->arg_count].slot = slot;
    ctx->arg_count++;
    if (out_slot) *out_slot = slot;
    return true;
}

static bool arg_push_slot(ExpandContext *ctx, const char *name, uint32_t slot) {
    uint32_t ignored = 0;
    if (arg_lookup(ctx, name, &ignored)) return true;
    if (ctx->arg_count == ctx->arg_cap) {
        size_t cap = ctx->arg_cap ? ctx->arg_cap * 2u : 4u;
        ArgBinding *args = realloc(ctx->args, cap * sizeof(*args));
        if (!args) return false;
        ctx->args = args;
        ctx->arg_cap = cap;
    }
    char *copy = ish_strdup(name);
    if (!copy) return false;
    ctx->args[ctx->arg_count].name = copy;
    ctx->args[ctx->arg_count].slot = slot;
    ctx->arg_count++;
    return true;
}

static bool capture_lookup(ExpandContext *ctx, const char *name, uint32_t *out_capture) {
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (strcmp(ctx->captures[i].name, name) == 0) {
            *out_capture = ctx->captures[i].capture_index;
            return true;
        }
    }
    for (size_t i = ctx->outer_count; i > 0; i--) {
        LocalBinding *outer = &ctx->outer_locals[i - 1u];
        if (strcmp(outer->name, name) != 0) continue;
        if (ctx->capture_count == ctx->capture_cap) {
            size_t cap = ctx->capture_cap ? ctx->capture_cap * 2u : 4u;
            CaptureBinding *captures = realloc(ctx->captures, cap * sizeof(*captures));
            if (!captures) return false;
            ctx->captures = captures;
            ctx->capture_cap = cap;
        }
        CaptureBinding *capture = &ctx->captures[ctx->capture_count];
        capture->name = ish_strdup(name);
        if (!capture->name) return false;
        capture->outer_slot = outer->slot;
        capture->capture_index = (uint32_t)ctx->capture_count;
        ctx->capture_count++;
        *out_capture = capture->capture_index;
        return true;
    }
    return false;
}

static void begin_function_context(ExpandContext *ctx, SavedFunctionContext *saved) {
    saved->locals = ctx->locals;
    saved->local_count = ctx->local_count;
    saved->local_cap = ctx->local_cap;
    saved->next_slot = ctx->next_slot;
    saved->args = ctx->args;
    saved->arg_count = ctx->arg_count;
    saved->arg_cap = ctx->arg_cap;
    saved->outer_locals = ctx->outer_locals;
    saved->outer_count = ctx->outer_count;
    saved->captures = ctx->captures;
    saved->capture_count = ctx->capture_count;
    saved->capture_cap = ctx->capture_cap;
    ctx->locals = NULL;
    ctx->local_count = 0;
    ctx->local_cap = 0;
    ctx->next_slot = 0;
    ctx->args = NULL;
    ctx->arg_count = 0;
    ctx->arg_cap = 0;
    ctx->outer_locals = saved->locals;
    ctx->outer_count = saved->local_count;
    ctx->captures = NULL;
    ctx->capture_count = 0;
    ctx->capture_cap = 0;
}

static void end_function_context(ExpandContext *ctx, SavedFunctionContext *saved) {
    local_bindings_destroy(ctx->locals, ctx->local_count);
    arg_bindings_destroy(ctx->args, ctx->arg_count);
    ctx->locals = saved->locals;
    ctx->local_count = saved->local_count;
    ctx->local_cap = saved->local_cap;
    ctx->next_slot = saved->next_slot;
    ctx->args = saved->args;
    ctx->arg_count = saved->arg_count;
    ctx->arg_cap = saved->arg_cap;
    ctx->outer_locals = saved->outer_locals;
    ctx->outer_count = saved->outer_count;
    ctx->captures = saved->captures;
    ctx->capture_count = saved->capture_count;
    ctx->capture_cap = saved->capture_cap;
}

static void begin_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    saved->local_count = ctx->local_count;
    saved->local_cap = ctx->local_cap;
    saved->next_slot = ctx->next_slot;
    saved->args = ctx->args;
    saved->arg_count = ctx->arg_count;
    saved->arg_cap = ctx->arg_cap;
    ctx->args = NULL;
    ctx->arg_count = 0;
    ctx->arg_cap = 0;
    ctx->local_count = 0;
    ctx->local_cap = 0;
    ctx->next_slot = 0;
}

static void end_clause_context(ExpandContext *ctx, SavedClauseContext *saved) {
    arg_bindings_destroy(ctx->args, ctx->arg_count);
    ctx->args = saved->args;
    ctx->arg_count = saved->arg_count;
    ctx->arg_cap = saved->arg_cap;
    ctx->local_count = saved->local_count;
    ctx->local_cap = saved->local_cap;
    ctx->next_slot = saved->next_slot;
}

static bool syn_is_word(const IshSyntax *syn, const char *word) {
    return syn && syn->kind == ISH_SYN_WORD && strcmp(syn->as.text, word) == 0;
}

static bool syn_is_protocol(const IshSyntax *syn, const char *head) {
    return syn && syn->kind == ISH_SYN_LIST && syn->as.seq.shape == ISH_SEQ_PAREN && syn->as.seq.count > 0 && syn_is_word(syn->as.seq.items[0], head);
}

static const OperatorInfo *lookup_operator(const ExpandContext *ctx, const IshSyntax *syn) {
    if (!syn || syn->kind != ISH_SYN_WORD) return NULL;
    const IshScopeSet *scopes = ish_syn_scope_set(syn, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup_scopes = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, syn->as.text, 0, ISH_BIND_SPACE_OPERATOR, lookup_scopes, &binding);
    ish_scope_set_destroy(&empty);
    if (status != ISH_RESOLVE_OK || !binding || binding->kind != ISH_BIND_OPERATOR) return NULL;
    if (binding->payload >= sizeof(OPERATORS) / sizeof(OPERATORS[0])) return NULL;
    return &OPERATORS[binding->payload];
}

static IshCore *expand_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err);
static IshCore *expand_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
static IshCore *expand_fn_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);
static IshCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IshSyntax *head, IshSyntax *const *items, size_t param_start, size_t end, IshError *err);

typedef struct {
    ExpandContext *ctx;
    IshSyntax *const *items;
    size_t end;
    size_t pos;
    IshError *err;
} EnforestParser;

static IshCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec);

static IshCore *expand_error(IshError *err, IshSpan span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ish_error_setv(err, span, fmt, ap);
    va_end(ap);
    return NULL;
}

static IshCore *literal_from_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    switch (syn->kind) {
        case ISH_SYN_NIL:
            return ish_core_literal(ish_nil(), syn->span);
        case ISH_SYN_INT:
            return ish_core_literal(ish_int(syn->as.integer), syn->span);
        case ISH_SYN_ATOM:
            return ish_core_literal(ish_atom(ctx->rt, syn->as.text), syn->span);
        case ISH_SYN_STRING: {
            IshValue value = ish_string(ctx->rt, syn->as.text, err);
            if (err && err->present) return NULL;
            return ish_core_literal(value, syn->span);
        }
        default:
            return NULL;
    }
}

static bool value_from_literal_syntax(ExpandContext *ctx, const IshSyntax *syn, IshValue *out, IshError *err) {
    switch (syn->kind) {
        case ISH_SYN_NIL:
            *out = ish_nil();
            return true;
        case ISH_SYN_INT:
            *out = ish_int(syn->as.integer);
            return true;
        case ISH_SYN_ATOM:
            *out = ish_atom(ctx->rt, syn->as.text);
            return true;
        case ISH_SYN_STRING:
            *out = ish_string(ctx->rt, syn->as.text, err);
            return !(err && err->present);
        case ISH_SYN_LIST: {
            IshValue list = ish_nil();
            for (size_t i = syn->as.seq.count; i > 0; i--) {
                IshValue item = ish_nil();
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i - 1u], &item, err)) return false;
                list = ish_cons(ctx->rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            return true;
        }
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE: {
            IshValue *items = NULL;
            if (syn->as.seq.count != 0) {
                items = calloc(syn->as.seq.count, sizeof(*items));
                if (!items) {
                    ish_error_oom(err, syn->span);
                    return false;
                }
            }
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i], &items[i], err)) {
                    free(items);
                    return false;
                }
            }
            *out = syn->kind == ISH_SYN_VECTOR ? ish_vector(ctx->rt, items, syn->as.seq.count, err) : ish_tuple(ctx->rt, items, syn->as.seq.count, err);
            free(items);
            return !(err && err->present);
        }
        case ISH_SYN_DICT: {
            if (syn->as.seq.count % 2u != 0) {
                ish_error_set(err, syn->span, "dict literal requires key/value pairs");
                return false;
            }
            size_t count = syn->as.seq.count / 2u;
            IshDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) {
                ish_error_oom(err, syn->span);
                return false;
            }
            for (size_t i = 0; i < count; i++) {
                if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err) ||
                    !value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u + 1u], &entries[i].value, err)) {
                    free(entries);
                    return false;
                }
            }
            *out = ish_dict(ctx->rt, entries, count, err);
            free(entries);
            return !(err && err->present);
        }
        default:
            return false;
    }
}

static IshPattern *pattern_from_param_depth(ExpandContext *ctx, const IshSyntax *syn, uint32_t arg_index, bool allow_bind, IshError *err) {
    if (syn->kind == ISH_SYN_WORD) {
        if (strcmp(syn->as.text, "_") == 0) return ish_pat_wildcard(syn->span);
        if (!allow_bind) {
            uint32_t ignored = 0;
            if (!local_lookup(ctx, syn->as.text, &ignored) && !local_push(ctx, syn->as.text, NULL)) {
                ish_error_oom(err, syn->span);
                return NULL;
            }
            return ish_pat_bind(syn->as.text, syn->span);
        }
        uint32_t ignored = 0;
        if (!arg_lookup(ctx, syn->as.text, &ignored) && !arg_push_slot(ctx, syn->as.text, arg_index)) {
            ish_error_oom(err, syn->span);
            return NULL;
        }
        return ish_pat_bind(syn->as.text, syn->span);
    }
    IshValue literal = ish_nil();
    if (syn->kind != ISH_SYN_DICT && value_from_literal_syntax(ctx, syn, &literal, err)) return ish_pat_literal(literal, syn->span);
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE) {
        if (syn->kind == ISH_SYN_LIST) {
            size_t dot_index = SIZE_MAX;
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (syn->as.seq.items[i]->kind == ISH_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                    if (dot_index != SIZE_MAX) {
                        ish_error_set(err, syn->as.seq.items[i]->span, "list rest pattern may contain only one dot");
                        return NULL;
                    }
                    dot_index = i;
                }
            }
            if (dot_index != SIZE_MAX) {
                if (dot_index == 0 || dot_index + 2u != syn->as.seq.count) {
                    ish_error_set(err, syn->span, "list rest pattern must have form [head ... . rest]");
                    return NULL;
                }
                IshPattern *tail = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
                if (!tail) return NULL;
                for (size_t i = dot_index; i > 0; i--) {
                    IshPattern *head = pattern_from_param_depth(ctx, syn->as.seq.items[i - 1u], (uint32_t)(i - 1u), false, err);
                    if (!head) {
                        ish_pat_free(tail);
                        return NULL;
                    }
                    tail = ish_pat_pair(head, tail, syn->span);
                    if (!tail) {
                        ish_pat_free(head);
                        return (IshPattern *)(uintptr_t)ish_error_oom(err, syn->span);
                    }
                }
                return tail;
            }
        } else {
            size_t dot_index = SIZE_MAX;
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (syn->as.seq.items[i]->kind == ISH_SYN_WORD && strcmp(syn->as.seq.items[i]->as.text, ".") == 0) {
                    if (dot_index != SIZE_MAX) {
                        ish_error_set(err, syn->as.seq.items[i]->span, "sequence rest pattern may contain only one dot");
                        return NULL;
                    }
                    dot_index = i;
                }
            }
            if (dot_index != SIZE_MAX) {
                if (dot_index == 0 || dot_index + 2u != syn->as.seq.count) {
                    ish_error_set(err, syn->span, "sequence rest pattern must have form %[head ... . rest] or {head ... . rest}");
                    return NULL;
                }
                IshPattern **items = calloc(dot_index, sizeof(*items));
                if (!items) {
                    ish_error_oom(err, syn->span);
                    return NULL;
                }
                for (size_t i = 0; i < dot_index; i++) {
                    items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) ish_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
                IshPattern *rest = pattern_from_param_depth(ctx, syn->as.seq.items[dot_index + 1u], (uint32_t)(dot_index + 1u), false, err);
                if (!rest) {
                    for (size_t i = 0; i < dot_index; i++) ish_pat_free(items[i]);
                    free(items);
                    return NULL;
                }
                IshPatternKind kind = syn->kind == ISH_SYN_VECTOR ? ISH_PAT_VECTOR_REST : ISH_PAT_TUPLE_REST;
                IshPattern *pat = ish_pat_sequence_rest(kind, items, dot_index, rest, syn->span);
                if (!pat) {
                    for (size_t i = 0; i < dot_index; i++) ish_pat_free(items[i]);
                    free(items);
                    ish_pat_free(rest);
                    ish_error_oom(err, syn->span);
                    return NULL;
                }
                return pat;
            }
        }
        IshPattern **items = NULL;
        if (syn->as.seq.count != 0) {
            items = calloc(syn->as.seq.count, sizeof(*items));
            if (!items) {
                ish_error_oom(err, syn->span);
                return NULL;
            }
        }
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            items[i] = pattern_from_param_depth(ctx, syn->as.seq.items[i], (uint32_t)i, false, err);
            if (!items[i]) {
                for (size_t j = 0; j < i; j++) ish_pat_free(items[j]);
                free(items);
                return NULL;
            }
        }
        IshPatternKind kind = syn->kind == ISH_SYN_LIST ? ISH_PAT_LIST : (syn->kind == ISH_SYN_VECTOR ? ISH_PAT_VECTOR : ISH_PAT_TUPLE);
        IshPattern *pat = ish_pat_sequence(kind, items, syn->as.seq.count, syn->span);
        if (!pat) {
            for (size_t i = 0; i < syn->as.seq.count; i++) ish_pat_free(items[i]);
            free(items);
            ish_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    if (syn->kind == ISH_SYN_DICT) {
        if (syn->as.seq.count % 2u != 0) {
            ish_error_set(err, syn->span, "dict pattern requires key/value pairs");
            return NULL;
        }
        size_t count = syn->as.seq.count / 2u;
        IshDictPatternEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
        if (count != 0 && !entries) {
            ish_error_oom(err, syn->span);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            if (!value_from_literal_syntax(ctx, syn->as.seq.items[i * 2u], &entries[i].key, err)) {
                for (size_t j = 0; j < i; j++) ish_pat_free(entries[j].pattern);
                free(entries);
                if (!err->present) ish_error_set(err, syn->as.seq.items[i * 2u]->span, "dict pattern key must be literal in current subset");
                return NULL;
            }
            entries[i].pattern = pattern_from_param_depth(ctx, syn->as.seq.items[i * 2u + 1u], (uint32_t)i, false, err);
            if (!entries[i].pattern) {
                for (size_t j = 0; j < i; j++) ish_pat_free(entries[j].pattern);
                free(entries);
                return NULL;
            }
        }
        IshPattern *pat = ish_pat_dict(entries, count, syn->span);
        if (!pat) {
            for (size_t i = 0; i < count; i++) ish_pat_free(entries[i].pattern);
            free(entries);
            ish_error_oom(err, syn->span);
            return NULL;
        }
        return pat;
    }
    ish_error_set(err, syn->span, "unsupported defn parameter pattern in current subset");
    return NULL;
}

static IshPattern *pattern_from_param(ExpandContext *ctx, const IshSyntax *syn, uint32_t arg_index, IshError *err) {
    return pattern_from_param_depth(ctx, syn, arg_index, true, err);
}

static bool copy_pattern_locals(ExpandContext *ctx, IshPatternLocal **out_locals, uint32_t *out_count) {
    if (ctx->local_count == 0) {
        *out_locals = NULL;
        *out_count = 0;
        return true;
    }
    IshPatternLocal *locals = calloc(ctx->local_count, sizeof(*locals));
    if (!locals) return false;
    for (size_t i = 0; i < ctx->local_count; i++) {
        locals[i].name = ish_strdup(ctx->locals[i].name);
        if (!locals[i].name) {
            for (size_t j = 0; j < i; j++) free(locals[j].name);
            free(locals);
            return false;
        }
        locals[i].slot = ctx->locals[i].slot;
    }
    *out_locals = locals;
    *out_count = (uint32_t)ctx->local_count;
    return true;
}

static IshCore *expand_body_items(ExpandContext *ctx, IshSyntax *const *items, size_t index, size_t count, IshError *err);

static bool bind_form_parts(const IshSyntax *form, const char **out_name, size_t *out_rhs_start) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    if (form->as.seq.items[1]->kind != ISH_SYN_WORD) return false;
    if (!syn_is_word(form->as.seq.items[2], "=")) return false;
    *out_name = form->as.seq.items[1]->as.text;
    *out_rhs_start = 3;
    return true;
}

static bool definition_like_form(const IshSyntax *form, const char **out_head) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 2 || form->as.seq.items[1]->kind != ISH_SYN_WORD) return false;
    const char *head = form->as.seq.items[1]->as.text;
    if (strcmp(head, "def") != 0 && strcmp(head, "defn") != 0) return false;
    *out_head = head;
    return true;
}

static bool defn_form_parts(const IshSyntax *form, const char **out_name, size_t *out_param_start) {
    if (!syn_is_protocol(form, "%-expr")) return false;
    if (form->as.seq.count < 4) return false;
    if (!syn_is_word(form->as.seq.items[1], "defn")) return false;
    if (form->as.seq.items[2]->kind != ISH_SYN_WORD) return false;
    *out_name = form->as.seq.items[2]->as.text;
    *out_param_start = 3;
    return true;
}

static void defn_groups_destroy(DefnGroup *groups, size_t count) {
    for (size_t i = 0; i < count; i++) free(groups[i].indices);
    free(groups);
}

static DefnGroup *find_or_add_group(DefnGroup **groups, size_t *count, size_t *cap, const char *name) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*groups)[i].name, name) == 0) return &(*groups)[i];
    }
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 4u;
        DefnGroup *next = realloc(*groups, next_cap * sizeof(*next));
        if (!next) return NULL;
        *groups = next;
        *cap = next_cap;
    }
    DefnGroup *group = &(*groups)[(*count)++];
    group->name = name;
    group->slot = UINT32_MAX;
    group->indices = NULL;
    group->count = 0;
    group->cap = 0;
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

static IshCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IshSyntax *const *items, IshError *err);
static IshCore *expand_match_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err);

static IshCore *expand_body_items(ExpandContext *ctx, IshSyntax *const *items, size_t index, size_t count, IshError *err) {
    if (index >= count) return ish_core_literal(ish_nil(), ish_span_unknown(NULL));

    const char *name = NULL;
    size_t rhs_start = 0;
    const char *definition_head = NULL;
    if (definition_like_form(items[index], &definition_head)) {
        const IshSyntax *form = items[index];
        if (strcmp(definition_head, "def") == 0) {
            return expand_error(err, form->as.seq.items[1]->span, "source 'def' is not a function declaration; use 'defn' once definition-region collection is implemented");
        }
        size_t j = index;
        DefnGroup *groups = NULL;
        size_t group_count = 0;
        size_t group_cap = 0;
        while (j < count) {
            const char *def_name = NULL;
            size_t ignored = 0;
            if (!defn_form_parts(items[j], &def_name, &ignored)) break;
            DefnGroup *group = find_or_add_group(&groups, &group_count, &group_cap, def_name);
            if (!group || !group_add_index(group, j)) {
                defn_groups_destroy(groups, group_count);
                return (IshCore *)(uintptr_t)ish_error_oom(err, items[j]->span);
            }
            j++;
        }
        if (group_count == 0) return expand_error(err, form->span, "invalid defn form");

        IshCore *letrec = ish_core_letrec(form->span);
        if (!letrec) { defn_groups_destroy(groups, group_count); return (IshCore *)(uintptr_t)ish_error_oom(err, form->span); }
        size_t saved_count = ctx->local_count;
        uint32_t saved_next = ctx->next_slot;
        for (size_t k = 0; k < group_count; k++) {
            if (!local_push(ctx, groups[k].name, &groups[k].slot)) {
                defn_groups_destroy(groups, group_count);
                ish_core_free(letrec);
                local_pop_to(ctx, saved_count, saved_next);
                return (IshCore *)(uintptr_t)ish_error_oom(err, form->span);
            }
        }
        for (size_t k = 0; k < group_count; k++) {
            IshCore *value = expand_defn_group(ctx, &groups[k], items, err);
            if (!value || !ish_core_letrec_add(letrec, groups[k].name, groups[k].slot, value)) {
                ish_core_free(value);
                defn_groups_destroy(groups, group_count);
                ish_core_free(letrec);
                local_pop_to(ctx, saved_count, saved_next);
                if (!err->present) ish_error_oom(err, form->span);
                return NULL;
            }
        }
        defn_groups_destroy(groups, group_count);
        IshCore *body = expand_body_items(ctx, items, j, count, err);
        local_pop_to(ctx, saved_count, saved_next);
        if (!body || !ish_core_letrec_set_body(letrec, body)) {
            ish_core_free(body);
            ish_core_free(letrec);
            if (!err->present) ish_error_oom(err, form->span);
            return NULL;
        }
        return letrec;
    }

    if (bind_form_parts(items[index], &name, &rhs_start)) {
        const IshSyntax *form = items[index];
        IshCore *value = expand_parts(ctx, form->as.seq.items, rhs_start, form->as.seq.count, err);
        if (!value) return NULL;
        size_t saved_count = ctx->local_count;
        uint32_t saved_next = ctx->next_slot;
        uint32_t slot = 0;
        if (!local_push(ctx, name, &slot)) {
            ish_core_free(value);
            ish_error_oom(err, form->span);
            return NULL;
        }
        IshCore *body = expand_body_items(ctx, items, index + 1u, count, err);
        local_pop_to(ctx, saved_count, saved_next);
        if (!body) {
            ish_core_free(value);
            return NULL;
        }
        return ish_core_bind_local(slot, value, body, form->span);
    }

    IshCore *first = expand_syntax(ctx, items[index], err);
    if (!first) return NULL;
    if (index + 1u >= count) return first;
    IshCore *rest = expand_body_items(ctx, items, index + 1u, count, err);
    if (!rest) {
        ish_core_free(first);
        return NULL;
    }
    IshCore *do_expr = ish_core_do(items[index]->span);
    if (!do_expr || !ish_core_do_add(do_expr, first) || !ish_core_do_add(do_expr, rest)) {
        ish_core_free(first);
        ish_core_free(rest);
        ish_core_free(do_expr);
        ish_error_oom(err, items[index]->span);
        return NULL;
    }
    return do_expr;
}

static IshPrimitive primitive_from_binding(const IshBinding *binding) {
    return (IshPrimitive)binding->payload;
}

static IshCore *expand_word_ref(ExpandContext *ctx, const IshSyntax *word, IshError *err) {
    uint32_t slot = 0;
    if (local_lookup(ctx, word->as.text, &slot)) return ish_core_local_ref(slot, word->span);
    if (arg_lookup(ctx, word->as.text, &slot)) return ish_core_arg_ref(slot, word->span);
    if (ctx->outer_count != 0 && capture_lookup(ctx, word->as.text, &slot)) return ish_core_capture_ref(slot, word->span);

    const IshScopeSet *scopes = ish_syn_scope_set(word, 0);
    IshScopeSet empty;
    ish_scope_set_init(&empty);
    const IshScopeSet *lookup_scopes = scopes ? scopes : &empty;
    const IshBinding *binding = NULL;
    IshResolveStatus status = ish_binding_resolve(&ctx->bindings, word->as.text, 0, ISH_BIND_SPACE_DEFAULT, lookup_scopes, &binding);
    ish_scope_set_destroy(&empty);
    if (status == ISH_RESOLVE_OK && binding->kind == ISH_BIND_VALUE) return ish_core_primitive(primitive_from_binding(binding), word->span);
    if (status == ISH_RESOLVE_AMBIGUOUS) return expand_error(err, word->span, "ambiguous identifier '%s'", word->as.text);
    return expand_error(err, word->span, "unbound identifier '%s'", word->as.text);
}

static IshCore *expand_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (start >= end) return expand_error(err, ish_span_unknown(NULL), "empty expression");
    size_t len = end - start;
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "fn") == 0) return expand_fn_parts(ctx, items, start, end, err);
    if (items[start]->kind == ISH_SYN_WORD && strcmp(items[start]->as.text, "match") == 0) return expand_match_parts(ctx, items, start, end, err);
    if (len == 1) return expand_syntax(ctx, items[start], err);
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

    bool has_operator = false;
    for (size_t i = start; i < end; i++) {
        if (lookup_operator(ctx, items[i])) {
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

    if (items[start]->kind != ISH_SYN_WORD) {
        return expand_error(err, items[start]->span, "call head must be an identifier in the supported subset");
    }
    IshCore *callee = expand_word_ref(ctx, items[start], err);
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
    return parser->pos >= parser->end || lookup_operator(parser->ctx, parser->items[parser->pos]) != NULL;
}

static IshCore *parse_enforest_primary(EnforestParser *parser) {
    if (parser->pos >= parser->end) return expand_error(parser->err, ish_span_unknown(NULL), "expected operand");
    size_t start = parser->pos;
    IshSyntax *head = parser->items[parser->pos++];
    if (lookup_operator(parser->ctx, head)) return expand_error(parser->err, head->span, "operator cannot appear where an operand is required");

    if (head->kind == ISH_SYN_WORD && strcmp(head->as.text, "fn") == 0) {
        parser->pos = parser->end;
        return expand_fn_parts(parser->ctx, parser->items, start, parser->end, parser->err);
    }

    if (head->kind == ISH_SYN_WORD && parser->pos < parser->end && !lookup_operator(parser->ctx, parser->items[parser->pos])) {
        IshCore *callee = expand_word_ref(parser->ctx, head, parser->err);
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

static IshCore *make_operator_app(const OperatorInfo *op, IshCore *left, IshCore *right, IshSpan span, IshError *err) {
    IshCore *app = ish_core_app(ish_core_primitive(op->primitive, span), span);
    if (!app || !ish_core_app_add_arg(app, left) || !ish_core_app_add_arg(app, right)) {
        ish_core_free(app);
        ish_core_free(left);
        ish_core_free(right);
        ish_error_oom(err, span);
        return NULL;
    }
    return app;
}

static IshCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec) {
    IshCore *left = parse_enforest_primary(parser);
    if (!left) return NULL;

    while (parser->pos < parser->end) {
        const OperatorInfo *op = lookup_operator(parser->ctx, parser->items[parser->pos]);
        if (!op || op->precedence < min_prec) break;
        IshSpan op_span = parser->items[parser->pos]->span;
        parser->pos++;
        IshCore *right = parse_enforest_expr(parser, (uint8_t)(op->precedence + 1u));
        if (!right) {
            ish_core_free(left);
            return NULL;
        }
        left = make_operator_app(op, left, right, op_span, parser->err);
        if (!left) return NULL;
    }
    return left;
}

static IshCore *expand_fn_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (start >= end || items[start]->kind != ISH_SYN_WORD || strcmp(items[start]->as.text, "fn") != 0) {
        return expand_error(err, items[start]->span, "expected fn literal");
    }
    return expand_function_literal(ctx, "<lambda>", items[start], items, start + 1u, end, err);
}

static IshCore *expand_function_literal(ExpandContext *ctx, const char *debug_name, const IshSyntax *head, IshSyntax *const *items, size_t param_start, size_t end, IshError *err) {
    size_t cursor = param_start;
    size_t arrow = SIZE_MAX;
    size_t body_index = SIZE_MAX;
    IshCore *guard = NULL;

    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    bool params_ok = true;
    while (cursor < end) {
        if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
            arrow = cursor;
            cursor++;
            break;
        }
        if (syn_is_protocol(items[cursor], "%-body")) {
            body_index = cursor;
            break;
        }
        if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "when") == 0) {
            if (guard) {
                expand_error(err, items[cursor]->span, "fn clause may have only one guard");
                params_ok = false;
                break;
            }
            size_t guard_start = cursor + 1u;
            cursor++;
            while (cursor < end) {
                if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
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
        if (items[cursor]->kind != ISH_SYN_WORD) {
            expand_error(err, items[cursor]->span, "fn parameter must be an identifier");
            params_ok = false;
            break;
        }
        if (!arg_push(ctx, items[cursor]->as.text, NULL)) {
            expand_error(err, items[cursor]->span, "duplicate fn parameter or out of memory: %s", items[cursor]->as.text);
            params_ok = false;
            break;
        }
        cursor++;
    }
    if (!params_ok) {
        ish_core_free(guard);
        end_function_context(ctx, &saved);
        return NULL;
    }

    IshCore *body = NULL;
    if (arrow != SIZE_MAX) {
        if (cursor >= end) {
            expand_error(err, head->span, "fn arrow requires a body");
        } else {
            body = expand_parts(ctx, items, cursor, end, err);
        }
    } else if (body_index != SIZE_MAX) {
        if (body_index + 1u != end) {
            expand_error(err, items[body_index]->span, "fn do/end body must be the final fn component in the supported subset");
        } else {
            body = expand_syntax(ctx, items[body_index], err);
        }
    } else {
        expand_error(err, head->span, "fn literal requires -> or do/end body");
    }

    uint32_t arity = (uint32_t)ctx->arg_count;
    if (!body) {
        ish_core_free(guard);
        end_function_context(ctx, &saved);
        return NULL;
    }
    IshCore *fn = ish_core_fn(debug_name, arity, body, head->span);
    if (!fn || (guard && !ish_core_fn_set_guard_take(fn, guard))) {
        ish_core_free(body);
        ish_core_free(guard);
        end_function_context(ctx, &saved);
        ish_error_oom(err, head->span);
        return NULL;
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!ish_core_fn_add_capture(fn, ctx->captures[i].outer_slot)) {
            ish_core_free(fn);
            end_function_context(ctx, &saved);
            ish_error_oom(err, head->span);
            return NULL;
        }
    }
    end_function_context(ctx, &saved);
    return fn;
}

static IshCore *expand_defn_clause(ExpandContext *ctx, const char *debug_name, const IshSyntax *head, IshSyntax *const *items, size_t param_start, size_t end, IshPattern ***out_patterns, uint32_t *out_pattern_count, IshPatternLocal **out_locals, uint32_t *out_local_count, IshCore **out_guard, uint32_t *out_arity, IshError *err) {
    size_t cursor = param_start;
    size_t arrow = SIZE_MAX;
    size_t body_index = SIZE_MAX;
    IshCore *guard = NULL;
    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);

    IshPattern **patterns = NULL;
    size_t pattern_count = 0;
    size_t pattern_cap = 0;
    while (cursor < end) {
        if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
            arrow = cursor;
            cursor++;
            break;
        }
        if (syn_is_protocol(items[cursor], "%-body")) {
            body_index = cursor;
            break;
        }
        if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "when") == 0) {
            if (guard) {
                expand_error(err, items[cursor]->span, "defn clause may have only one guard");
                for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            size_t guard_start = cursor + 1u;
            cursor++;
            while (cursor < end) {
                if (items[cursor]->kind == ISH_SYN_WORD && strcmp(items[cursor]->as.text, "->") == 0) {
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
                for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            guard = expand_parts(ctx, items, guard_start, cursor, err);
            if (!guard) {
                for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return NULL;
            }
            if (arrow != SIZE_MAX) cursor++;
            break;
        }
        if (pattern_count == pattern_cap) {
            size_t cap = pattern_cap ? pattern_cap * 2u : 4u;
            IshPattern **next = realloc(patterns, cap * sizeof(*next));
            if (!next) {
                for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
                free(patterns);
                end_clause_context(ctx, &saved);
                return (IshCore *)(uintptr_t)ish_error_oom(err, items[cursor]->span);
            }
            patterns = next;
            pattern_cap = cap;
        }
        IshPattern *pat = pattern_from_param(ctx, items[cursor], (uint32_t)pattern_count, err);
        if (!pat) {
            for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
        patterns[pattern_count++] = pat;
        cursor++;
    }

    IshCore *body = NULL;
    if (arrow != SIZE_MAX) {
        if (cursor >= end) expand_error(err, head->span, "defn arrow requires a body");
        else body = expand_parts(ctx, items, cursor, end, err);
    } else if (body_index != SIZE_MAX) {
        if (body_index + 1u != end) expand_error(err, items[body_index]->span, "defn do/end body must be final in this subset");
        else body = expand_syntax(ctx, items[body_index], err);
    } else {
        expand_error(err, head->span, "defn requires -> or do/end body");
    }
    if (!body) {
        ish_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, out_locals, out_local_count)) {
        ish_core_free(guard);
        end_clause_context(ctx, &saved);
        for (size_t i = 0; i < pattern_count; i++) ish_pat_free(patterns[i]);
        free(patterns);
        ish_core_free(body);
        return (IshCore *)(uintptr_t)ish_error_oom(err, head->span);
    }
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = (uint32_t)pattern_count;
    *out_guard = guard;
    *out_arity = (uint32_t)pattern_count;
    (void)debug_name;
    return body;
}

static IshCore *expand_defn_group(ExpandContext *ctx, const DefnGroup *group, IshSyntax *const *items, IshError *err) {
    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IshCore *result = group->count == 1 ? NULL : ish_core_fn_multi(group->name, items[group->indices[0]]->span);
    if (group->count != 1 && !result) {
        end_function_context(ctx, &saved);
        return (IshCore *)(uintptr_t)ish_error_oom(err, items[group->indices[0]]->span);
    }
    IshCore *single_body = NULL;
    IshPattern **single_patterns = NULL;
    uint32_t single_pattern_count = 0;
    IshPatternLocal *single_pattern_locals = NULL;
    uint32_t single_pattern_local_count = 0;
    IshCore *single_guard = NULL;
    uint32_t single_arity = 0;

    for (size_t i = 0; i < group->count; i++) {
        const IshSyntax *def_form = items[group->indices[i]];
        const char *def_name = NULL;
        size_t param_start = 0;
        (void)defn_form_parts(def_form, &def_name, &param_start);
        IshPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IshPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IshCore *guard = NULL;
        uint32_t arity = 0;
        IshCore *body = expand_defn_clause(ctx, def_name, def_form->as.seq.items[1], def_form->as.seq.items, param_start, def_form->as.seq.count, &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!body) {
            ish_core_free(result);
            end_function_context(ctx, &saved);
            return NULL;
        }
        if (group->count == 1) {
            single_body = body;
            single_patterns = patterns;
            single_pattern_count = pattern_count;
            single_pattern_locals = pattern_locals;
            single_pattern_local_count = pattern_local_count;
            single_guard = guard;
            single_arity = arity;
        } else if (!ish_core_fn_multi_add_clause_take(result, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
            for (uint32_t p = 0; p < pattern_count; p++) ish_pat_free(patterns[p]);
            free(patterns);
            for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
            free(pattern_locals);
            ish_core_free(guard);
            ish_core_free(body);
            ish_core_free(result);
            end_function_context(ctx, &saved);
            return (IshCore *)(uintptr_t)ish_error_oom(err, def_form->span);
        }
    }

    if (group->count == 1) {
        result = ish_core_fn(group->name, single_arity, single_body, items[group->indices[0]]->span);
        if (!result || !ish_core_fn_set_param_patterns_take(result, single_patterns, single_pattern_count) || !ish_core_fn_set_pattern_locals_take(result, single_pattern_locals, single_pattern_local_count) || (single_guard && !ish_core_fn_set_guard_take(result, single_guard))) {
            for (uint32_t p = 0; p < single_pattern_count; p++) ish_pat_free(single_patterns[p]);
            free(single_patterns);
            for (uint32_t p = 0; p < single_pattern_local_count; p++) free(single_pattern_locals[p].name);
            free(single_pattern_locals);
            ish_core_free(single_guard);
            ish_core_free(single_body);
            ish_core_free(result);
            end_function_context(ctx, &saved);
            return (IshCore *)(uintptr_t)ish_error_oom(err, items[group->indices[0]]->span);
        }
    }

    for (size_t i = 0; i < ctx->capture_count; i++) {
        bool ok = group->count == 1 ? ish_core_fn_add_capture(result, ctx->captures[i].outer_slot) : ish_core_fn_multi_add_capture(result, ctx->captures[i].outer_slot);
        if (!ok) {
            ish_core_free(result);
            end_function_context(ctx, &saved);
            return (IshCore *)(uintptr_t)ish_error_oom(err, items[group->indices[0]]->span);
        }
    }
    end_function_context(ctx, &saved);
    return result;
}

static IshCore *expand_match_clause(ExpandContext *ctx, const IshSyntax *clause, IshPattern ***out_patterns, uint32_t *out_pattern_count, IshPatternLocal **out_locals, uint32_t *out_local_count, IshCore **out_guard, uint32_t *out_arity, IshError *err) {
    if (!syn_is_protocol(clause, "%-expr")) return expand_error(err, clause->span, "match clause must be an expression");
    size_t arrow = SIZE_MAX;
    for (size_t i = 1; i < clause->as.seq.count; i++) {
        if (clause->as.seq.items[i]->kind == ISH_SYN_WORD && strcmp(clause->as.seq.items[i]->as.text, "->") == 0) {
            arrow = i;
            break;
        }
    }
    if (arrow == SIZE_MAX || arrow == 1 || arrow + 1u >= clause->as.seq.count) return expand_error(err, clause->span, "match clause must have form pattern -> body");
    bool has_guard = arrow > 2;
    if (has_guard && (arrow <= 3 || !syn_is_word(clause->as.seq.items[2], "when"))) return expand_error(err, clause->span, "match guards must have form pattern when guard -> body");
    if (!has_guard && arrow != 2) return expand_error(err, clause->span, "current match subset supports exactly one pattern per clause");

    SavedClauseContext saved;
    begin_clause_context(ctx, &saved);
    IshPattern **patterns = calloc(1u, sizeof(*patterns));
    if (!patterns) {
        end_clause_context(ctx, &saved);
        return (IshCore *)(uintptr_t)ish_error_oom(err, clause->span);
    }
    patterns[0] = pattern_from_param(ctx, clause->as.seq.items[1], 0, err);
    if (!patterns[0]) {
        free(patterns);
        end_clause_context(ctx, &saved);
        return NULL;
    }
    IshCore *guard = NULL;
    if (has_guard) {
        guard = expand_parts(ctx, clause->as.seq.items, 3, arrow, err);
        if (!guard) {
            ish_pat_free(patterns[0]);
            free(patterns);
            end_clause_context(ctx, &saved);
            return NULL;
        }
    }
    IshCore *body = expand_parts(ctx, clause->as.seq.items, arrow + 1u, clause->as.seq.count, err);
    if (!body) {
        ish_core_free(guard);
        end_clause_context(ctx, &saved);
        ish_pat_free(patterns[0]);
        free(patterns);
        return NULL;
    }
    if (!copy_pattern_locals(ctx, out_locals, out_local_count)) {
        ish_core_free(guard);
        end_clause_context(ctx, &saved);
        ish_pat_free(patterns[0]);
        free(patterns);
        ish_core_free(body);
        return (IshCore *)(uintptr_t)ish_error_oom(err, clause->span);
    }
    end_clause_context(ctx, &saved);
    *out_patterns = patterns;
    *out_pattern_count = 1;
    *out_guard = guard;
    *out_arity = 1;
    return body;
}

static IshCore *expand_match_parts(ExpandContext *ctx, IshSyntax *const *items, size_t start, size_t end, IshError *err) {
    if (start + 2u >= end) return expand_error(err, items[start]->span, "match expects a scrutinee and do/end clause body");
    size_t body_index = SIZE_MAX;
    for (size_t i = start + 1u; i < end; i++) {
        if (syn_is_protocol(items[i], "%-body")) {
            body_index = i;
            break;
        }
    }
    if (body_index == SIZE_MAX || body_index + 1u != end) return expand_error(err, items[start]->span, "match requires final do/end clause body");
    if (items[body_index]->as.seq.count < 2) return expand_error(err, items[body_index]->span, "match requires at least one clause");

    IshCore *scrutinee = expand_parts(ctx, items, start + 1u, body_index, err);
    if (!scrutinee) return NULL;

    SavedFunctionContext saved;
    begin_function_context(ctx, &saved);
    IshCore *multi = ish_core_fn_multi("<match>", items[start]->span);
    if (!multi) {
        end_function_context(ctx, &saved);
        ish_core_free(scrutinee);
        return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
    }
    for (size_t i = 1; i < items[body_index]->as.seq.count; i++) {
        IshPattern **patterns = NULL;
        uint32_t pattern_count = 0;
        IshPatternLocal *pattern_locals = NULL;
        uint32_t pattern_local_count = 0;
        IshCore *guard = NULL;
        uint32_t arity = 0;
        IshCore *body = expand_match_clause(ctx, items[body_index]->as.seq.items[i], &patterns, &pattern_count, &pattern_locals, &pattern_local_count, &guard, &arity, err);
        if (!body || !ish_core_fn_multi_add_clause_take(multi, arity, patterns, pattern_count, pattern_locals, pattern_local_count, guard, body)) {
            if (body) ish_core_free(body);
            if (guard) ish_core_free(guard);
            if (patterns) {
                for (uint32_t p = 0; p < pattern_count; p++) ish_pat_free(patterns[p]);
                free(patterns);
            }
            if (pattern_locals) {
                for (uint32_t p = 0; p < pattern_local_count; p++) free(pattern_locals[p].name);
                free(pattern_locals);
            }
            ish_core_free(multi);
            end_function_context(ctx, &saved);
            ish_core_free(scrutinee);
            if (!err->present) ish_error_oom(err, items[body_index]->as.seq.items[i]->span);
            return NULL;
        }
    }
    for (size_t i = 0; i < ctx->capture_count; i++) {
        if (!ish_core_fn_multi_add_capture(multi, ctx->captures[i].outer_slot)) {
            ish_core_free(multi);
            end_function_context(ctx, &saved);
            ish_core_free(scrutinee);
            return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
        }
    }
    end_function_context(ctx, &saved);

    IshCore *app = ish_core_app(multi, items[start]->span);
    if (!app || !ish_core_app_add_arg(app, scrutinee)) {
        ish_core_free(app);
        ish_core_free(multi);
        ish_core_free(scrutinee);
        return (IshCore *)(uintptr_t)ish_error_oom(err, items[start]->span);
    }
    return app;
}

static IshCore *expand_protocol_expr(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count < 2) return expand_error(err, syn->span, "empty %%-expr");
    return expand_parts(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IshCore *expand_protocol_body(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    return expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IshCore *expand_protocol_group(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-group expects one child");
    return expand_syntax(ctx, syn->as.seq.items[1], err);
}

static IshCore *expand_program(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    if (!syn_is_protocol(syn, "%-package-begin")) return expand_error(err, syn->span, "expected %%-package-begin syntax");
    return expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IshCore *expand_syntax(ExpandContext *ctx, const IshSyntax *syn, IshError *err) {
    IshCore *lit = literal_from_syntax(ctx, syn, err);
    if (lit || (err && err->present)) return lit;

    IshValue literal = ish_nil();
    if (value_from_literal_syntax(ctx, syn, &literal, err)) return ish_core_literal(literal, syn->span);
    if (err && err->present) return NULL;

    if (syn->kind == ISH_SYN_WORD) return expand_word_ref(ctx, syn, err);
    if (syn_is_protocol(syn, "%-expr")) return expand_protocol_expr(ctx, syn, err);
    if (syn_is_protocol(syn, "%-body")) return expand_protocol_body(ctx, syn, err);
    if (syn_is_protocol(syn, "%-group")) return expand_protocol_group(ctx, syn, err);
    if (syn_is_protocol(syn, "%-package-begin")) return expand_program(ctx, syn, err);
    if (syn_is_protocol(syn, "%-word") || syn_is_protocol(syn, "%-shell-var") || syn_is_protocol(syn, "%-redirect")) {
        return expand_error(err, syn->span, "shell syntax is not supported by the minimal expander yet");
    }
    return expand_error(err, syn->span, "unsupported syntax in minimal expander");
}

bool ish_expand_syntax(IshRuntime *rt, const IshSyntax *syntax, IshCore **out, IshError *err) {
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    if (!ctx_seed(&ctx, err)) {
        ctx_destroy(&ctx);
        return false;
    }
    IshCore *core = expand_syntax(&ctx, syntax, err);
    ctx_destroy(&ctx);
    if (!core || (err && err->present)) {
        ish_core_free(core);
        return false;
    }
    *out = core;
    return true;
}

bool ish_expand_string(IshRuntime *rt, const char *file, const char *source, IshCore **out, IshError *err) {
    IshSyntax *syntax = NULL;
    if (!ish_reader_read_string(file, source, &syntax, err)) return false;
    bool ok = ish_expand_syntax(rt, syntax, out, err);
    ish_syn_free(syntax);
    return ok;
}
