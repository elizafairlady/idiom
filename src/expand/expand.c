#include "internal.h"

#include "idiom/regex.h"

#include <ctype.h>
#include <pthread.h>
#include <string.h>

static IdmCore *zero_arity_call_if_known(IdmCore *callee, const IdmArity *arity, bool callee_position, IdmSpan span, IdmError *err);
static IdmCore *expand_unbound_identifier(ExpandContext *ctx, const IdmSyntax *word, IdmError *err);
static IdmCore *expand_word_ref_mode(ExpandContext *ctx, const IdmSyntax *word, bool callee_position, IdmError *err);
static IdmCore *expand_word_ref(ExpandContext *ctx, const IdmSyntax *word, IdmError *err);
static IdmCore *expand_word_callee(ExpandContext *ctx, const IdmSyntax *word, IdmError *err);
static bool head_is_bound(ExpandContext *ctx, const IdmSyntax *word);
static const IdmSyntax *try_section_head(const IdmSyntax *stmt);
static IdmCore *expand_try_handler(ExpandContext *ctx, const IdmSyntax *stmt, uint32_t *out_slot, IdmError *err);
static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static IdmCore *expand_implements_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static bool syn_is_dot(const IdmSyntax *s);
static bool qualified_word_resolves(ExpandContext *ctx, const IdmSyntax *word);
typedef struct {
    const MethodSurfaceDef **items;
    size_t count;
    size_t cap;
} MethodSurfaceGroup;
typedef struct {
    const TypeDef *type;
    uint32_t field_index;
} FieldSurfaceRef;
typedef struct {
    FieldSurfaceRef *items;
    size_t count;
    size_t cap;
    const char *name;
} FieldSurfaceGroup;
static void method_surface_group_destroy(MethodSurfaceGroup *group);
static void field_surface_group_destroy(FieldSurfaceGroup *group);
static bool resolve_method_surface_group(ExpandContext *ctx, const IdmSyntax *word, MethodSurfaceGroup *out, IdmResolveStatus *out_status, IdmError *err);
static bool resolve_field_surface_group(ExpandContext *ctx, const IdmSyntax *word, FieldSurfaceGroup *out, IdmResolveStatus *out_status, IdmError *err);
static IdmCore *expand_field_surface_call_cores(ExpandContext *ctx, const FieldSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err);
static IdmCore *expand_method_surface_call_cores(ExpandContext *ctx, const MethodSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err);
static IdmCore *parse_postfix_expr(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, IdmError *err);
static IdmCore *parse_dot_tail(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, IdmCore *receiver, IdmError *err);
static IdmCore *parse_application_expr(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, bool allow_unknown_call, IdmError *err);
static bool arg_parse_at_stop(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator);
static bool syntax_call_arity(ExpandContext *ctx, const IdmSyntax *syn, IdmArity *out, IdmError *err);
static uint32_t application_available_args(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator);
static size_t application_postfix_arg_end(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator, IdmError *err);
static IdmCore *expand_parts_inner(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static bool surface_operator_boundary(ExpandContext *ctx, const IdmSyntax *syn, const IdmOperatorDef **out_syntax_capture, IdmError *err);
static size_t surface_primary_end(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static bool scan_operator_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, size_t *out_syntax_op_index, const IdmOperatorDef **out_syntax_infix, bool *out_has_operator, IdmError *err);
typedef struct { IdmSyntax *items[2]; size_t count; } SyntaxCapturePayload;
static IdmCore *expand_syntax_capture_tail(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmSyntax *const *items, size_t cursor, size_t end, IdmError *err);
static IdmCore *expand_syntax_capture_macro(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSyntax *target_word, uint32_t payload, IdmSyntax *const *items, size_t left_start, size_t left_end, size_t cursor, size_t end, IdmError *err);
static IdmCore *expand_syntax_capture_dispatch(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSyntax *const *items, size_t left_start, size_t left_end, size_t cursor, size_t end, IdmError *err);
static IdmCore *parse_enforest_primary(EnforestParser *parser);
static void core_args_free(IdmCore **args, size_t arg_count);
static IdmSyntax *operator_target_word(const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err);
static IdmCore *operator_callee(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err);
static IdmCore *make_operator_call(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmCore *right, IdmSpan span, IdmError *err);
static IdmCore *make_operator_unary(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *operand, IdmSpan span, IdmError *err);
static IdmCore *make_operator_call_args(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err);
static IdmCore *parse_enforest_expr(EnforestParser *parser, uint8_t min_prec);
static IdmCore *expand_form_expr(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_body(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_group(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_expression(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_adjacent(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_syntax_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_regex(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_match(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_program(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_container(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *empty_container_literal(IdmRuntime *rt, IdmPrimitive prim, IdmSpan span, IdmError *err);
static bool word_has_subtraction_shape(const char *text);
static bool syntax_is_literal_value(const IdmSyntax *syn);
static bool syntax_is_negative_number(const IdmSyntax *syn);
static bool adjacent_items_present(IdmSyntax *const *items, size_t start, size_t end);
static bool flatten_adjacent_items(IdmSyntax *const *items, size_t start, size_t end, IdmSyntax ***out_items, size_t *out_count);

typedef IdmCore *(*SurfaceKeywordHandler)(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
typedef struct {
    const char *name;
    SurfaceKeywordHandler handler;
    bool primary_takes_rest;
    bool operand_takes_rest;
} SurfaceKeywordDef;

static const SurfaceKeywordDef SURFACE_KEYWORDS[] = {
    {"fn", expand_fn_parts, true, true},
    {"receive", expand_receive_parts, false, false},
    {"try", expand_try_parts, false, false},
    {"implements?", expand_implements_parts, true, false},
};

static const SurfaceKeywordDef *surface_keyword_for_syntax(const IdmSyntax *syn) {
    if (!syn || syn->kind != IDM_SYN_WORD) return NULL;
    for (size_t i = 0; i < sizeof(SURFACE_KEYWORDS) / sizeof(SURFACE_KEYWORDS[0]); i++) {
        if (strcmp(syn->as.text, SURFACE_KEYWORDS[i].name) == 0) return &SURFACE_KEYWORDS[i];
    }
    return NULL;
}

static bool binding_phase_visible(const IdmBinding *binding, int phase) {
    return binding->phase == phase || binding->phase == IDM_PHASE_ANY;
}

bool expand_arity_auto_calls_zero(const IdmArity *arity) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return false;
    if (arity->kind == IDM_ARITY_RANGE && arity->max == UINT32_MAX) return false;
    return idm_arity_accepts(arity, 0u);
}

static IdmCore *zero_arity_call_if_known(IdmCore *callee, const IdmArity *arity, bool callee_position, IdmSpan span, IdmError *err) {
    if (!callee) {
        idm_error_oom(err, span);
        return NULL;
    }
    if (callee_position) return callee;
    if (!expand_arity_auto_calls_zero(arity)) return callee;
    IdmCore *call = idm_core_call(callee, span);
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, span);
        return NULL;
    }
    return call;
}

static IdmCore *method_arity_error(IdmError *err, IdmSpan span, const MethodSurfaceDef *method, size_t got) {
    IdmBuffer expected;
    idm_buf_init(&expected);
    bool ok = idm_arity_describe(&expected, &method->arity);
    expand_error(err, span, "method '%s.%s' expects %s argument(s), got %zu", method_surface_trait_text(method), method_surface_name_text(method), ok ? expected.data : "?", got);
    idm_buf_destroy(&expected);
    return NULL;
}

static void method_surface_group_destroy(MethodSurfaceGroup *group) {
    free(group->items);
    group->items = NULL;
    group->count = 0;
    group->cap = 0;
}

static void field_surface_group_destroy(FieldSurfaceGroup *group) {
    free(group->items);
    group->items = NULL;
    group->count = 0;
    group->cap = 0;
    group->name = NULL;
}

static bool method_surface_group_add(MethodSurfaceGroup *group, const MethodSurfaceDef *method, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < group->count; i++) {
        if (method_surfaces_share_trait_identity(group->items[i], method)) {
            group->items[i] = method;
            return true;
        }
    }
    if (group->count == group->cap) {
        if (!idm_grow((void **)&group->items, &group->cap, sizeof(*group->items), 4u, group->count + 1u)) return idm_error_oom(err, span);
    }
    group->items[group->count++] = method;
    return true;
}

static int type_field_index(const ExpandContext *ctx, const TypeDef *type, const char *name) {
    for (size_t i = 0; i < type->field_count; i++) {
        if (type_field_matches_name(ctx, &type->fields[i], name)) return (int)i;
    }
    return -1;
}

static bool field_surface_group_add(FieldSurfaceGroup *group, const TypeDef *type, uint32_t field_index, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < group->count; i++) {
        if (typed_type_same_identity(group->items[i].type, type)) {
            group->items[i].type = type;
            group->items[i].field_index = field_index;
            return true;
        }
    }
    if (group->count == group->cap) {
        if (!idm_grow((void **)&group->items, &group->cap, sizeof(*group->items), 4u, group->count + 1u)) return idm_error_oom(err, span);
    }
    group->items[group->count].type = type;
    group->items[group->count].field_index = field_index;
    group->count++;
    return true;
}

static bool resolve_method_surface_group(ExpandContext *ctx, const IdmSyntax *word, MethodSurfaceGroup *out, IdmResolveStatus *out_status, IdmError *err) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    if (!word || word->kind != IDM_SYN_WORD) {
        if (out_status) *out_status = IDM_RESOLVE_UNBOUND;
        return true;
    }
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = idm_syn_scope_set(word, 0);
    if (!lookup) lookup = &empty;
    const IdmScopeSet *best = NULL;
    IdmResolveStatus status = idm_binding_resolve_scopes(&ctx->bindings, word->as.text, ctx->phase, IDM_BIND_SPACE_METHOD, lookup, &best);
    idm_scope_set_destroy(&empty);
    if (status != IDM_RESOLVE_OK || !best) {
        if (out_status) *out_status = status;
        return true;
    }
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *binding = &ctx->bindings.items[i];
        if (!binding_phase_visible(binding, ctx->phase) ||
            binding->space != IDM_BIND_SPACE_METHOD ||
            binding->kind != IDM_BIND_METHOD ||
            strcmp(binding->name, word->as.text) != 0 ||
            !idm_scope_set_equal(&binding->scopes, best) ||
            binding->payload >= ctx->typed.method_surface_count) continue;
        const MethodSurfaceDef *method = &ctx->typed.method_surfaces[binding->payload];
        if (method_surface_matches_name(ctx, method, word->as.text) && idm_scope_set_equal(&method->scopes, best)) {
            if (!method_surface_group_add(out, method, err, word->span)) {
                method_surface_group_destroy(out);
                return false;
            }
        }
    }
    if (out_status) *out_status = out->count == 0 ? IDM_RESOLVE_UNBOUND : IDM_RESOLVE_OK;
    return true;
}

static bool resolve_field_surface_group(ExpandContext *ctx, const IdmSyntax *word, FieldSurfaceGroup *out, IdmResolveStatus *out_status, IdmError *err) {
    out->items = NULL;
    out->count = 0;
    out->cap = 0;
    out->name = word && word->kind == IDM_SYN_WORD ? word->as.text : NULL;
    if (!word || word->kind != IDM_SYN_WORD) {
        if (out_status) *out_status = IDM_RESOLVE_UNBOUND;
        return true;
    }
    IdmScopeSet empty;
    idm_scope_set_init(&empty);
    const IdmScopeSet *lookup = idm_syn_scope_set(word, 0);
    if (!lookup) lookup = &empty;
    const IdmScopeSet *best = NULL;
    IdmResolveStatus status = idm_binding_resolve_scopes(&ctx->bindings, word->as.text, ctx->phase, IDM_BIND_SPACE_FIELD, lookup, &best);
    idm_scope_set_destroy(&empty);
    if (status != IDM_RESOLVE_OK || !best) {
        if (out_status) *out_status = status;
        return true;
    }
    for (size_t i = 0; i < ctx->bindings.count; i++) {
        const IdmBinding *binding = &ctx->bindings.items[i];
        if (!binding_phase_visible(binding, ctx->phase) ||
            binding->space != IDM_BIND_SPACE_FIELD ||
            binding->kind != IDM_BIND_FIELD ||
            strcmp(binding->name, word->as.text) != 0 ||
            !idm_scope_set_equal(&binding->scopes, best)) continue;
        const TypeDef *type = typed_type_by_index(ctx, binding->payload);
        if (!type) continue;
        int field_index = type_field_index(ctx, type, word->as.text);
        if (field_index < 0) continue;
        if (!field_surface_group_add(out, type, (uint32_t)field_index, err, word->span)) {
            field_surface_group_destroy(out);
            return false;
        }
    }
    if (out_status) *out_status = out->count == 0 ? IDM_RESOLVE_UNBOUND : IDM_RESOLVE_OK;
    return true;
}

static bool method_group_accepts(const MethodSurfaceGroup *group, uint32_t argc) {
    for (size_t i = 0; i < group->count; i++) {
        if (idm_arity_accepts(&group->items[i]->arity, argc)) return true;
    }
    return false;
}

static bool method_group_arity(const MethodSurfaceGroup *group, IdmArity *out) {
    IdmArity arity = idm_arity_unknown();
    for (size_t i = 0; i < group->count; i++) {
        if (group->items[i]->arity.kind == IDM_ARITY_UNKNOWN) return false;
        if (arity.kind == IDM_ARITY_UNKNOWN) {
            arity = group->items[i]->arity;
        } else if (!idm_arity_merge(&arity, &group->items[i]->arity)) {
            return false;
        }
    }
    if (arity.kind == IDM_ARITY_UNKNOWN) return false;
    *out = arity;
    return true;
}

static bool method_group_max_accepting_at_least(const MethodSurfaceGroup *group, uint32_t min, uint32_t *out) {
    bool found = false;
    uint32_t best = 0;
    for (size_t i = 0; i < group->count; i++) {
        uint32_t arity = 0;
        if (!idm_arity_max_accepting_at_least(&group->items[i]->arity, min, &arity)) continue;
        if (!found || arity > best) best = arity;
        found = true;
    }
    if (found && out) *out = best;
    return found;
}

static size_t method_group_target_extra_count(const MethodSurfaceGroup *group, uint32_t receiver_count, uint32_t available, uint32_t max_extra) {
    uint32_t limit = available < max_extra ? available : max_extra;
    for (uint32_t extra = limit + 1u; extra > 0u; extra--) {
        uint32_t candidate = extra - 1u;
        if (method_group_accepts(group, receiver_count + candidate)) return candidate;
    }
    return limit;
}

static IdmCore *method_group_arity_error(IdmError *err, IdmSpan span, const MethodSurfaceGroup *group, size_t got) {
    if (group->count == 1u) return method_arity_error(err, span, group->items[0], got);
    IdmBuffer expected;
    idm_buf_init(&expected);
    for (size_t i = 0; i < group->count; i++) {
        IdmBuffer arity;
        idm_buf_init(&arity);
        bool described = idm_arity_describe(&arity, &group->items[i]->arity);
        if (i != 0 && !idm_buf_append(&expected, ", ")) break;
        if (!idm_buf_appendf(&expected, "%s.%s=%s", method_surface_trait_text(group->items[i]), method_surface_name_text(group->items[i]), described ? arity.data : "?")) {
            idm_buf_destroy(&arity);
            break;
        }
        idm_buf_destroy(&arity);
    }
    expand_error(err, span, "method '%s' has no candidate accepting %zu argument(s); candidates: %s",
                 method_surface_name_text(group->items[0]), got, expected.data ? expected.data : "?");
    idm_buf_destroy(&expected);
    return NULL;
}

static bool word_has_subtraction_shape(const char *text) {
    if (!text) return false;
    for (size_t i = 1; text[i] != '\0' && text[i + 1u] != '\0'; i++) {
        if (text[i] != '-') continue;
        unsigned char before = (unsigned char)text[i - 1u];
        unsigned char after = (unsigned char)text[i + 1u];
        bool left = isalnum(before) || before == '_' || before == '?' || before == '!';
        bool right = isdigit(after);
        if (left && right) return true;
    }
    return false;
}

static bool word_is_subtraction_of_bindings(ExpandContext *ctx, const IdmSyntax *word) {
    const char *t = word->as.text;
    if (!t || t[0] == '-') return false;
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    if (!scopes) scopes = &ctx->empty_scopes;
    size_t start = 0, segs = 0, idents = 0;
    bool saw_dash = false;
    for (size_t i = 0;; i++) {
        if (t[i] != '-' && t[i] != '\0') continue;
        size_t len = i - start;
        if (len == 0) return false;
        bool all_digits = true;
        for (size_t j = start; j < i; j++) {
            if (!isdigit((unsigned char)t[j])) { all_digits = false; break; }
        }
        if (!all_digits) {
            char *seg = idm_strndup(t + start, len);
            if (!seg) return false;
            bool bound = idm_binding_resolve(&ctx->bindings, seg, ctx->phase, IDM_BIND_SPACE_DEFAULT, scopes, NULL) == IDM_RESOLVE_OK;
            free(seg);
            if (!bound) return false;
            idents++;
        }
        segs++;
        if (t[i] == '\0') break;
        saw_dash = true;
        start = i + 1u;
    }
    return saw_dash && segs >= 2 && idents >= 1;
}

static bool syntax_is_literal_value(const IdmSyntax *syn) {
    if (!syn) return false;
    switch (syn->kind) {
        case IDM_SYN_NIL:
        case IDM_SYN_ATOM:
        case IDM_SYN_INT:
        case IDM_SYN_BIGINT:
        case IDM_SYN_FLOAT:
        case IDM_SYN_STRING:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            return true;
        default:
            return false;
    }
}

static bool syntax_is_negative_number(const IdmSyntax *syn) {
    if (!syn) return false;
    return (syn->kind == IDM_SYN_INT && syn->as.integer < 0) || (syn->kind == IDM_SYN_BIGINT && syn->as.text[0] == '-') || (syn->kind == IDM_SYN_FLOAT && syn->as.real < 0.0);
}

static bool operator_shaped_text(const char *text) {
    if (!text || !*text) return false;
    unsigned char c = (unsigned char)text[0];
    return !isalpha(c) && !isdigit(c) && c != '_';
}

static void note_unbound_context(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    if (!err || !err->present) return;
    for (size_t i = 0; i < word->origins.count; i++) {
        idm_error_note(err, "in expansion of '%s'", word->origins.items[i]);
    }
    if (word_has_subtraction_shape(word->as.text) || word_is_subtraction_of_bindings(ctx, word)) {
        idm_error_note(err, "identifier '%s' was read as one word; use spaces around '-' for subtraction", word->as.text);
    }
    int other = ctx->phase == 0 ? 1 : 0;
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    if (idm_binding_resolve(&ctx->bindings, word->as.text, other, IDM_BIND_SPACE_DEFAULT, scopes ? scopes : &ctx->empty_scopes, NULL) == IDM_RESOLVE_OK) {
        if (ctx->phase == 0) idm_error_note(err, "'%s' is bound for-syntax (phase 1) but referenced at runtime (phase 0)", word->as.text);
        else {
            idm_error_note(err, "'%s' is bound at runtime (phase 0) but referenced for-syntax (phase 1)", word->as.text);
            idm_error_note(err, "use for-syntax for transformer helpers: define or import '%s' inside for-syntax", word->as.text);
        }
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

static IdmCore *expand_unbound_identifier(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    expand_error(err, word->span, "unbound identifier '%s'", word->as.text);
    if (err && err->present) {
        IdmValue name = idm_string(ctx->rt, word->as.text, NULL);
        (void)idm_error_reason(ctx->rt, err, "unbound-identifier", 1, name);
    }
    note_unbound_context(ctx, word, err);
    return NULL;
}

static IdmCore *expand_frame_binding_ref(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *binding, bool callee_position, IdmError *err) {
    uint32_t cap = 0;
    IdmCore *ref = NULL;
    if (binding->frame_id == ctx->frame) {
        ref = binding->kind == IDM_BIND_LOCAL ?
              idm_core_local_ref(word->as.text, binding->payload, word->span) :
              idm_core_arg_ref(word->as.text, binding->payload, word->span);
        return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
    }
    if (materialize_capture(ctx, word, binding, &cap)) {
        ref = idm_core_capture_ref(word->as.text, cap, word->span);
        return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
    }
    return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
}

static IdmCore *expand_word_ref_mode(ExpandContext *ctx, const IdmSyntax *word, bool callee_position, IdmError *err) {
    const CaptureBinding *capture = capture_lookup_existing(ctx->captures, ctx->capture_count, word);
    if (capture) {
        IdmCore *ref = idm_core_capture_ref(capture->name, capture->capture_index, word->span);
        return zero_arity_call_if_known(ref, &capture->arity, callee_position, word->span, err);
    }
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK) {
        if (binding->kind == IDM_BIND_LOCAL) {
            return expand_frame_binding_ref(ctx, word, binding, callee_position, err);
        }
        if (binding->kind == IDM_BIND_ARG) {
            return expand_frame_binding_ref(ctx, word, binding, callee_position, err);
        }
        if (binding->kind == IDM_BIND_ENV) {
            IdmCore *ref = idm_core_env_ref(word->as.text, binding->payload, word->span);
            return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
        }
        if (binding->kind == IDM_BIND_PACKAGE_SLOT) {
            if (binding->primitive_backed) {
                IdmPrimitive primitive = (IdmPrimitive)binding->primitive;
                IdmCore *ref = idm_core_primitive_backed_fn(idm_primitive_name(primitive), primitive, binding->arity, word->span);
                return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
            }
            const PackageSlotRef *slot_ref = package_slot_ref_get(ctx, binding->payload);
            if (!slot_ref) return (IdmCore *)(uintptr_t)idm_error_set(err, word->span, "package slot payload %u is out of bounds", binding->payload);
            IdmCore *ref = idm_core_package_ref(word->as.text, idm_atom(ctx->rt, slot_ref->env_key), slot_ref->slot, word->span);
            return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
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
    IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
    FieldSurfaceGroup field_group = {0};
    if (!resolve_field_surface_group(ctx, word, &field_group, &field_status, err)) return NULL;
    if (field_status == IDM_RESOLVE_AMBIGUOUS) {
        field_surface_group_destroy(&field_group);
        return expand_error(err, word->span, "ambiguous field '%s'", word->as.text);
    }
    IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
    MethodSurfaceGroup method_group = {0};
    if (!resolve_method_surface_group(ctx, word, &method_group, &method_status, err)) {
        field_surface_group_destroy(&field_group);
        return NULL;
    }
    if (method_status == IDM_RESOLVE_AMBIGUOUS) {
        field_surface_group_destroy(&field_group);
        method_surface_group_destroy(&method_group);
        return expand_error(err, word->span, "ambiguous method '%s'", word->as.text);
    }
    if (field_group.count != 0 && method_group.count != 0) {
        field_surface_group_destroy(&field_group);
        method_surface_group_destroy(&method_group);
        return expand_error(err, word->span, "ambiguous member '%s' is both a record field and a trait method", word->as.text);
    }
    if (field_group.count != 0) {
        field_surface_group_destroy(&field_group);
        method_surface_group_destroy(&method_group);
        return expand_error(err, word->span, "field '%s' requires a receiver", word->as.text);
    }
    if (method_group.count != 0) {
        IdmArity arity = idm_arity_unknown();
        bool auto_call = !callee_position && method_group_arity(&method_group, &arity) && expand_arity_auto_calls_zero(&arity);
        if (auto_call) {
            IdmCore *call = expand_method_surface_call_cores(ctx, &method_group, NULL, NULL, 0u, word->span, err);
            field_surface_group_destroy(&field_group);
            method_surface_group_destroy(&method_group);
            return call;
        }
        field_surface_group_destroy(&field_group);
        method_surface_group_destroy(&method_group);
        return expand_error(err, word->span, "method '%s' requires a receiver", word->as.text);
    }
    field_surface_group_destroy(&field_group);
    method_surface_group_destroy(&method_group);
    return expand_unbound_identifier(ctx, word, err);
}

static IdmCore *expand_word_ref(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    return expand_word_ref_mode(ctx, word, false, err);
}

static IdmCore *expand_word_callee(ExpandContext *ctx, const IdmSyntax *word, IdmError *err) {
    return expand_word_ref_mode(ctx, word, true, err);
}

static bool head_is_bound(ExpandContext *ctx, const IdmSyntax *word) {
    if (capture_lookup_existing(ctx->captures, ctx->capture_count, word)) return true;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    (void)resolve_default(ctx, word, &status);
    return status == IDM_RESOLVE_OK;
}

static const IdmSyntax *try_section_head(const IdmSyntax *stmt) {
    if (!syn_is_form(stmt, "%-expr") || stmt->as.seq.count < 2) return NULL;
    const IdmSyntax *head = stmt->as.seq.items[1];
    return head && head->kind == IDM_SYN_WORD ? head : NULL;
}

static IdmCore *expand_try_handler(ExpandContext *ctx, const IdmSyntax *stmt, uint32_t *out_slot, IdmError *err) {
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
    if (out_slot) *out_slot = r_slot;
    return handler_body;
}

static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (end - start != 2u || !syn_is_form(items[start + 1u], "%-body")) return expand_error(err, items[start]->span, "try requires a do/end body");
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

    IdmCore *handler = NULL;
    uint32_t rescue_slot = 0;
    if (has_rescue) {
        handler = expand_try_handler(ctx, stmts[rescue_pos], &rescue_slot, err);
        if (!handler) { idm_core_free(body_core); return NULL; }
    }
    IdmCore *cleanup = NULL;
    uint32_t ensure_slot = 0;
    if (has_ensure) {
        const IdmSyntax *estmt = stmts[ensure_pos];
        if (estmt->as.seq.count < 3) {
            idm_core_free(body_core);
            idm_core_free(handler);
            return expand_error(err, estmt->span, "ensure requires a cleanup expression");
        }
        cleanup = expand_parts(ctx, estmt->as.seq.items, 2, estmt->as.seq.count, err);
        if (!cleanup) {
            idm_core_free(body_core);
            idm_core_free(handler);
            return NULL;
        }
        ensure_slot = ctx->next_slot++;
    }
    IdmCore *guarded = idm_core_guard(body_core, handler, rescue_slot, cleanup, ensure_slot, items[start]->span);
    if (!guarded) {
        idm_core_free(body_core);
        idm_core_free(handler);
        idm_core_free(cleanup);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    return guarded;
}

static IdmCore *implements_bool_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    bool value = *(const bool *)user;
    IdmCore *body = idm_core_literal(idm_bool(ctx->rt, value), span);
    (void)multi;
    (void)argc;
    if (!body) idm_error_oom(err, span);
    return body;
}

static bool implements_add_bool_clause(ExpandContext *ctx, IdmCore *multi, const char *arg0_type, bool value, IdmSpan span, IdmError *err) {
    return expand_multi_add_dispatch_clause(ctx, multi, 1u, arg0_type, implements_bool_body, &value, span, err);
}

static IdmCore *expand_implements_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start + 2u >= end) return expand_error(err, items[start]->span, "implements? expects 'implements? TRAIT value'");
    size_t trait_end = end;
    IdmSyntax *trait_name = expect_qualified_word_at(items, start + 1u, &trait_end, "implements? expects a trait name", err);
    if (!trait_name) return NULL;
    if (trait_end >= end) {
        IdmSpan span = trait_name->span;
        idm_syn_free(trait_name);
        return expand_error(err, span, "implements? expects a value after the trait name");
    }

    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    TraitDef *trait = resolve_trait_def(ctx, trait_name, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        IdmSpan span = trait_name->span;
        const char *name = trait_name->as.text;
        expand_error(err, span, "ambiguous trait '%s'", name);
        idm_syn_free(trait_name);
        return NULL;
    }
    if (!trait) {
        IdmSpan span = trait_name->span;
        const char *name = trait_name->as.text;
        expand_error(err, span, "unbound trait '%s'", name);
        idm_syn_free(trait_name);
        return NULL;
    }

    IdmCore *value = expand_parts(ctx, items, trait_end, end, err);
    if (!value) {
        idm_syn_free(trait_name);
        return NULL;
    }
    const MethodImplDef **types = NULL;
    size_t type_count = 0;
    size_t type_cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < ctx->typed.method_impl_count; i++) {
        MethodImplDef *impl = &ctx->typed.method_impls[i];
        const MethodSurfaceDef *surface = method_surface_by_index(ctx, impl->method_surface);
        if (!method_surface_matches_trait(ctx, surface, trait_def_identity_text(trait))) continue;
        bool seen = false;
        for (size_t j = 0; j < type_count; j++) {
            if (method_impl_same_type(impl, types[j])) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if (type_count == type_cap) {
            if (!idm_grow((void **)&types, &type_cap, sizeof(*types), 8u, type_count + 1u)) {
                ok = idm_error_oom(err, trait_name->span);
                break;
            }
        }
        types[type_count++] = impl;
    }
    IdmCore *multi = ok ? idm_core_fn_multi("implements?", items[start]->span) : NULL;
    ok = ok && multi;
    if (!ok && !(err && err->present)) idm_error_oom(err, items[start]->span);
    for (size_t i = 0; ok && i < type_count; i++) {
        const char *type = method_impl_type_text(types[i]);
        ok = type && implements_add_bool_clause(ctx, multi, type, true, items[start]->span, err);
    }
    if (ok) ok = implements_add_bool_clause(ctx, multi, NULL, false, items[start]->span, err);
    idm_syn_free(trait_name);
    free(types);
    if (!ok) {
        idm_core_free(multi);
        idm_core_free(value);
        return NULL;
    }
    IdmCore *call = idm_core_call(multi, items[start]->span);
    if (!call) {
        idm_core_free(multi);
        idm_core_free(value);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    if (!core_call_add_arg_or_free(call, value, err, items[start]->span)) return NULL;
    return call;
}

static bool syn_is_dot(const IdmSyntax *s) {
    return s->kind == IDM_SYN_WORD && strcmp(s->as.text, ".") == 0;
}

static IdmSyntax *make_qualified_word(IdmSyntax *const *items, size_t start, size_t *inout_end, IdmError *err) {
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

bool try_qualified_word_at(IdmSyntax *const *items, size_t start, size_t end, IdmSyntax **out_word, size_t *out_end, IdmError *err) {
    *out_word = NULL;
    if (out_end) *out_end = start;
    if (start >= end) return true;
    if (items[start]->kind == IDM_SYN_WORD) {
        size_t pos = end;
        IdmSyntax *word = make_qualified_word(items, start, &pos, err);
        if (!word) return false;
        *out_word = word;
        if (out_end) *out_end = pos;
        return true;
    }
    if (syn_is_form(items[start], "%-adjacent")) {
        const IdmSyntax *run = items[start];
        if (run->as.seq.count < 2u || run->as.seq.items[1]->kind != IDM_SYN_WORD) return true;
        size_t run_end = run->as.seq.count;
        IdmSyntax *word = make_qualified_word(run->as.seq.items, 1u, &run_end, err);
        if (!word) return false;
        if (run_end != run->as.seq.count) {
            idm_syn_free(word);
            return true;
        }
        *out_word = word;
        if (out_end) *out_end = start + 1u;
        return true;
    }
    return true;
}

IdmSyntax *expect_qualified_word_at(IdmSyntax *const *items, size_t start, size_t *inout_end, const char *message, IdmError *err) {
    if (start >= *inout_end) {
        idm_error_set(err, idm_span_unknown(NULL), "%s", message);
        return NULL;
    }
    IdmSyntax *word = NULL;
    size_t end = start;
    if (!try_qualified_word_at(items, start, *inout_end, &word, &end, err)) return NULL;
    if (word) {
        *inout_end = end;
        return word;
    }
    idm_error_set(err, items[start]->span, "%s", message);
    return NULL;
}

static bool qualified_word_resolves(ExpandContext *ctx, const IdmSyntax *word) {
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK && binding) return true;
    IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
    FieldSurfaceGroup fields = {0};
    if (!resolve_field_surface_group(ctx, word, &fields, &field_status, NULL)) return false;
    bool field_ok = fields.count != 0 || field_status == IDM_RESOLVE_AMBIGUOUS;
    field_surface_group_destroy(&fields);
    if (field_ok) return true;
    IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
    MethodSurfaceGroup group = {0};
    if (!resolve_method_surface_group(ctx, word, &group, &method_status, NULL)) return false;
    bool ok = group.count != 0 || method_status == IDM_RESOLVE_AMBIGUOUS;
    method_surface_group_destroy(&group);
    return ok;
}

static bool syntax_call_arity(ExpandContext *ctx, const IdmSyntax *syn, IdmArity *out, IdmError *err) {
    if (!out) return false;
    *out = idm_arity_unknown();
    if (!syn) return false;
    if (syn_is_form(syn, "%-expression") && syn->as.seq.count == 2u) {
        const IdmSyntax *child = syn->as.seq.items[1];
        if (child->kind == IDM_SYN_WORD && child->as.text[0] == '&' && child->as.text[1] != '\0') {
            IdmSyntax *word = idm_syn_word(child->as.text + 1, child->span);
            if (!word) return idm_error_oom(err, child->span);
            bool ok = true;
            for (int phase = 0; phase < 2 && ok; phase++) {
                const IdmScopeSet *scopes = idm_syn_scope_set(child, phase);
                if (scopes) for (size_t si = 0; si < scopes->count && ok; si++) ok = idm_syn_scope_add(word, phase, scopes->items[si]);
            }
            bool resolved = ok && syntax_call_arity(ctx, word, out, err);
            if (!ok) idm_error_oom(err, child->span);
            idm_syn_free(word);
            return resolved;
        }
        return false;
    }
    if (syn_is_form(syn, "%-layout-group") && syn->as.seq.count == 2u) {
        const IdmSyntax *inner = syn->as.seq.items[1];
        if (syn_is_form(inner, "%-expr") && inner->as.seq.count >= 2u) {
            return syntax_call_arity(ctx, inner->as.seq.items[1], out, err);
        }
        return false;
    }
    if (syn_is_form(syn, "%-adjacent")) {
        size_t end = syn->as.seq.count;
        IdmSyntax *word = NULL;
        if (!try_qualified_word_at((IdmSyntax *const *)syn->as.seq.items, 1u, end, &word, &end, err)) return false;
        if (end != syn->as.seq.count) {
            idm_syn_free(word);
            return false;
        }
        bool ok = syntax_call_arity(ctx, word, out, err);
        idm_syn_free(word);
        return ok;
    }
    if (syn->kind != IDM_SYN_WORD) return false;
    const CaptureBinding *capture = capture_lookup_existing(ctx->captures, ctx->capture_count, syn);
    if (capture) {
        if (capture->arity.kind != IDM_ARITY_UNKNOWN) {
            *out = capture->arity;
            return true;
        }
        return false;
    }
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, syn, &status);
    if (status == IDM_RESOLVE_OK && binding) {
        if (binding->arity.kind != IDM_ARITY_UNKNOWN) {
            *out = binding->arity;
            return true;
        }
        return false;
    }
    if (status != IDM_RESOLVE_UNBOUND) return false;
    IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
    FieldSurfaceGroup fields = {0};
    if (!resolve_field_surface_group(ctx, syn, &fields, &field_status, NULL)) return false;
    bool field_ok = fields.count != 0 || field_status == IDM_RESOLVE_AMBIGUOUS;
    field_surface_group_destroy(&fields);
    if (field_ok) {
        *out = idm_arity_exact(1u);
        return true;
    }
    IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
    MethodSurfaceGroup group = {0};
    if (!resolve_method_surface_group(ctx, syn, &group, &method_status, NULL)) return false;
    if (method_status == IDM_RESOLVE_AMBIGUOUS || group.count == 0) {
        method_surface_group_destroy(&group);
        return false;
    }
    bool ok = method_group_arity(&group, out);
    method_surface_group_destroy(&group);
    return ok;
}

bool expand_syntax_call_arity(ExpandContext *ctx, const IdmSyntax *syn, IdmArity *out, IdmError *err) {
    return syntax_call_arity(ctx, syn, out, err);
}

static void member_surface_groups_destroy(FieldSurfaceGroup *fields, MethodSurfaceGroup *methods) {
    field_surface_group_destroy(fields);
    method_surface_group_destroy(methods);
}

static bool resolve_unbound_member_surface(ExpandContext *ctx, const IdmSyntax *word, FieldSurfaceGroup *fields, MethodSurfaceGroup *methods, bool *out_has, IdmError *err) {
    memset(fields, 0, sizeof(*fields));
    memset(methods, 0, sizeof(*methods));
    *out_has = false;
    if (!word || word->kind != IDM_SYN_WORD || head_is_bound(ctx, word)) return true;
    IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
    if (!resolve_field_surface_group(ctx, word, fields, &field_status, err)) return false;
    if (field_status == IDM_RESOLVE_AMBIGUOUS) {
        member_surface_groups_destroy(fields, methods);
        return expand_error(err, word->span, "ambiguous field '%s'", word->as.text) != NULL;
    }
    IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
    if (!resolve_method_surface_group(ctx, word, methods, &method_status, err)) {
        member_surface_groups_destroy(fields, methods);
        return false;
    }
    if (method_status == IDM_RESOLVE_AMBIGUOUS) {
        member_surface_groups_destroy(fields, methods);
        return expand_error(err, word->span, "ambiguous method '%s'", word->as.text) != NULL;
    }
    if (fields->count != 0 && methods->count != 0) {
        member_surface_groups_destroy(fields, methods);
        return expand_error(err, word->span, "ambiguous member '%s' is both a record field and a trait method", word->as.text) != NULL;
    }
    *out_has = fields->count != 0 || methods->count != 0;
    return true;
}

typedef enum { METHOD_DISPATCH_PACKAGE, METHOD_DISPATCH_ENV, METHOD_DISPATCH_CAPTURE, METHOD_DISPATCH_LOCAL } MethodDispatchKind;

typedef struct {
    MethodDispatchKind kind;
    uint32_t index;
    const char *env_key;
} MethodDispatchLoc;

static bool method_dispatch_loc(ExpandContext *ctx, const MethodSurfaceDef *method, MethodDispatchLoc *out, IdmSpan span, IdmError *err) {
    if (method->dispatch_env) {
        out->kind = (method->dispatch_env_key && method->dispatch_env_key[0]) ? METHOD_DISPATCH_PACKAGE : METHOD_DISPATCH_ENV;
        out->index = method->dispatch_slot;
        out->env_key = method->dispatch_env_key;
        return true;
    }
    if (method->dispatch_frame != ctx->frame) {
        uint32_t capture = 0;
        if (!materialize_slot_capture(ctx, method_surface_name_text(method), &method->scopes, method->dispatch_frame, method->dispatch_slot, method->arity, &capture)) {
            idm_error_set(err, span, "method dispatcher '%s.%s' is not visible in this function", method_surface_trait_text(method), method_surface_name_text(method));
            return false;
        }
        out->kind = METHOD_DISPATCH_CAPTURE;
        out->index = capture;
        out->env_key = NULL;
        return true;
    }
    out->kind = METHOD_DISPATCH_LOCAL;
    out->index = method->dispatch_slot;
    out->env_key = NULL;
    return true;
}

static IdmCore *method_dispatch_ref(ExpandContext *ctx, const MethodSurfaceDef *method, IdmSpan span, IdmError *err) {
    MethodDispatchLoc loc;
    if (!method_dispatch_loc(ctx, method, &loc, span, err)) return NULL;
    switch (loc.kind) {
        case METHOD_DISPATCH_PACKAGE: return idm_core_package_ref(method_surface_name_text(method), idm_atom(ctx->rt, loc.env_key), loc.index, span);
        case METHOD_DISPATCH_ENV: return idm_core_env_ref(method_surface_name_text(method), loc.index, span);
        case METHOD_DISPATCH_CAPTURE: return idm_core_capture_ref(method_surface_name_text(method), loc.index, span);
        case METHOD_DISPATCH_LOCAL: return idm_core_local_ref(method_surface_name_text(method), loc.index, span);
    }
    return NULL;
}

static bool method_impl_accepts_surface_call(const ExpandContext *ctx, const MethodImplDef *impl, const MethodSurfaceDef *method, uint32_t argc) {
    return method_impl_matches_identity(ctx, impl, method_surface_trait_text(method), method_surface_name_text(method), NULL) && idm_arity_accepts(&impl->arity, argc);
}

typedef struct {
    const MethodSurfaceDef *method;
    const MethodImplDef *impl;
    bool duplicate;
} CandidateMethodType;

static bool candidate_type_index(CandidateMethodType *items, size_t count, const MethodImplDef *impl, size_t *out) {
    for (size_t i = 0; i < count; i++) {
        if (method_impl_same_type(impl, items[i].impl)) {
            if (out) *out = i;
            return true;
        }
    }
    return false;
}

static bool candidate_type_push(CandidateMethodType **items, size_t *count, size_t *cap, const MethodImplDef *impl, const MethodSurfaceDef *method, IdmError *err, IdmSpan span) {
    size_t existing = 0;
    if (candidate_type_index(*items, *count, impl, &existing)) {
        (*items)[existing].duplicate = true;
        return true;
    }
    if (*count == *cap) {
        if (!idm_grow((void **)items, cap, sizeof(**items), 8u, *count + 1u)) return idm_error_oom(err, span);
    }
    (*items)[*count].method = method;
    (*items)[*count].impl = impl;
    (*items)[*count].duplicate = false;
    (*count)++;
    return true;
}

static bool method_dispatch_ref_for_multi(ExpandContext *ctx, IdmCore *multi, const MethodSurfaceDef *method, IdmCore **out, IdmSpan span, IdmError *err) {
    *out = NULL;
    MethodDispatchLoc loc;
    if (!method_dispatch_loc(ctx, method, &loc, span, err)) return false;
    if (loc.kind == METHOD_DISPATCH_PACKAGE || loc.kind == METHOD_DISPATCH_ENV) {
        *out = method_dispatch_ref(ctx, method, span, err);
        return *out != NULL;
    }
    IdmCaptureKind kind = loc.kind == METHOD_DISPATCH_CAPTURE ? IDM_CAP_UPVALUE : IDM_CAP_LOCAL;
    uint32_t index = loc.index;
    if (!idm_core_fn_multi_add_capture(multi, kind, method_surface_name_text(method), index)) return idm_error_oom(err, span);
    for (size_t i = 0; i < multi->as.fn_multi.capture_count; i++) {
        const IdmCapture *capture = &multi->as.fn_multi.captures[i];
        if (capture->kind == kind && capture->index == index) {
            *out = idm_core_capture_ref(method_surface_name_text(method), (uint32_t)i, span);
            return *out != NULL || idm_error_oom(err, span);
        }
    }
    return idm_error_oom(err, span);
}

static IdmCore *method_dispatch_call_body(ExpandContext *ctx, IdmCore *multi, const MethodSurfaceDef *method, uint32_t argc, IdmSpan span, IdmError *err) {
    IdmCore *callee = NULL;
    if (!method_dispatch_ref_for_multi(ctx, multi, method, &callee, span, err)) return NULL;
    IdmCore *call = callee ? idm_core_call(callee, span) : NULL;
    if (!call) {
        idm_core_free(callee);
        idm_error_oom(err, span);
        return NULL;
    }
    for (uint32_t i = 0; i < argc; i++) {
        IdmCore *arg = idm_core_arg_ref("arg", i, span);
        if (!arg || !idm_core_call_add_arg(call, arg)) {
            idm_core_free(arg);
            idm_core_free(call);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    return call;
}

static IdmCore *method_ambiguity_body(ExpandContext *ctx, const char *method, const char *type, IdmSpan span, IdmError *err) {
    IdmBuffer msg;
    idm_buf_init(&msg);
    bool msg_ok = idm_buf_appendf(&msg, "ambiguous method '%s' on type '%s'", method, type);
    IdmCore *body = msg_ok ? expand_raise_message_body(ctx, msg.data ? msg.data : "", span, err) : NULL;
    idm_buf_destroy(&msg);
    if (!msg_ok) idm_error_oom(err, span);
    return body;
}

static IdmCore *method_no_candidate_body(ExpandContext *ctx, const char *method, IdmSpan span, IdmError *err) {
    IdmBuffer msg;
    idm_buf_init(&msg);
    bool msg_ok = idm_buf_appendf(&msg, "method '%s' is available via candidate traits but not implemented on receiver", method);
    IdmCore *body = msg_ok ? expand_raise_message_body(ctx, msg.data ? msg.data : "", span, err) : NULL;
    idm_buf_destroy(&msg);
    if (!msg_ok) idm_error_oom(err, span);
    return body;
}

static IdmCore *field_no_candidate_body(ExpandContext *ctx, const char *field, IdmSpan span, IdmError *err) {
    IdmBuffer msg;
    idm_buf_init(&msg);
    bool msg_ok = idm_buf_appendf(&msg, "field '%s' is available via candidate record types but not on receiver", field);
    IdmCore *body = msg_ok ? expand_raise_message_body(ctx, msg.data ? msg.data : "", span, err) : NULL;
    idm_buf_destroy(&msg);
    if (!msg_ok) idm_error_oom(err, span);
    return body;
}

typedef IdmCore *(*NoCandidateBodyFn)(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err);

typedef struct {
    const char *name;
    NoCandidateBodyFn body_fn;
} WildcardFallbackBody;

static IdmCore *wildcard_fallback_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const WildcardFallbackBody *body = user;
    (void)multi;
    (void)argc;
    return body->body_fn(ctx, body->name, span, err);
}

static bool multi_add_wildcard_fallback_clause(ExpandContext *ctx, IdmCore *multi, const char *name, uint32_t argc, NoCandidateBodyFn body_fn, IdmSpan span, IdmError *err) {
    WildcardFallbackBody body = {name, body_fn};
    return expand_multi_add_dispatch_clause(ctx, multi, argc, NULL, wildcard_fallback_body, &body, span, err);
}

static IdmCore *field_type_clause_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const FieldSurfaceRef *field = user;
    IdmCore *arg = argc != 0 ? idm_core_arg_ref("arg", 0u, span) : NULL;
    (void)multi;
    return arg ? expand_record_field_core(ctx, arg, field->type, field->field_index, span, err) : NULL;
}

static bool field_multi_add_type_clause(ExpandContext *ctx, IdmCore *multi, const FieldSurfaceRef *field, IdmSpan span, IdmError *err) {
    return expand_multi_add_dispatch_clause(ctx, multi, 1u, type_def_identity_text(field->type), field_type_clause_body, field, span, err);
}

static bool field_multi_add_fallback_clause(ExpandContext *ctx, IdmCore *multi, const char *field, IdmSpan span, IdmError *err) {
    return multi_add_wildcard_fallback_clause(ctx, multi, field, 1u, field_no_candidate_body, span, err);
}

typedef struct {
    const CandidateMethodType *candidate;
    const char *method_name;
} MethodTypeBody;

static IdmCore *method_type_clause_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    const MethodTypeBody *body = user;
    const CandidateMethodType *candidate = body->candidate;
    const char *type = method_impl_type_text(candidate->impl);
    if (!type) return (IdmCore *)(uintptr_t)idm_error_set(err, span, "method implementation requires a receiver type");
    return candidate->duplicate
        ? method_ambiguity_body(ctx, body->method_name, type, span, err)
        : method_dispatch_call_body(ctx, multi, candidate->method, argc, span, err);
}

static IdmCore *expand_field_surface_call_cores(ExpandContext *ctx, const FieldSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    size_t argc = (receiver ? 1u : 0u) + arg_count;
    if (argc != 1u) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return expand_error(err, span, "field '%s' expects a receiver", group->name ? group->name : "<field>");
    }
    if (!receiver) {
        receiver = args[0];
        args[0] = NULL;
    }
    core_args_free(args, arg_count);
    if (group->count == 1u) {
        const FieldSurfaceRef *field = &group->items[0];
        return expand_record_field_core(ctx, receiver, field->type, field->field_index, span, err);
    }
    IdmCore *multi = idm_core_fn_multi(group->name ? group->name : "<field>", span);
    if (!multi) {
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < group->count; i++) {
        if (!field_multi_add_type_clause(ctx, multi, &group->items[i], span, err)) {
            idm_core_free(multi);
            idm_core_free(receiver);
            return NULL;
        }
    }
    if (!field_multi_add_fallback_clause(ctx, multi, group->name ? group->name : "<field>", span, err)) {
        idm_core_free(multi);
        idm_core_free(receiver);
        return NULL;
    }
    IdmCore *call = idm_core_call(multi, receiver ? receiver->span : span);
    if (!call) {
        idm_core_free(multi);
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (!idm_core_call_add_arg(call, receiver)) {
        idm_core_free(receiver);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    return call;
}

static bool method_multi_add_type_clause(ExpandContext *ctx, IdmCore *multi, const CandidateMethodType *candidate, const char *method_name, uint32_t argc, IdmSpan span, IdmError *err) {
    MethodTypeBody body = {candidate, method_name};
    const char *type = method_impl_type_text(candidate->impl);
    if (!type) return idm_error_set(err, span, "method implementation requires a receiver type");
    return expand_multi_add_dispatch_clause(ctx, multi, argc, type, method_type_clause_body, &body, span, err);
}

static bool method_multi_add_fallback_clause(ExpandContext *ctx, IdmCore *multi, const char *method_name, uint32_t argc, IdmSpan span, IdmError *err) {
    return multi_add_wildcard_fallback_clause(ctx, multi, method_name, argc, method_no_candidate_body, span, err);
}

static IdmCore *expand_composite_method_surface_call(ExpandContext *ctx, const MethodSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, uint32_t argc, IdmSpan span, IdmError *err) {
    CandidateMethodType *types = NULL;
    size_t type_count = 0;
    size_t type_cap = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < group->count; i++) {
        const MethodSurfaceDef *method = group->items[i];
        if (!idm_arity_accepts(&method->arity, argc)) continue;
        if (!method->has_dispatch) {
            ok = expand_error(err, span, "method '%s.%s' has no compiled dispatcher", method_surface_trait_text(method), method_surface_name_text(method)) != NULL;
            break;
        }
        for (size_t j = 0; ok && j < ctx->typed.method_impl_count; j++) {
            const MethodImplDef *impl = &ctx->typed.method_impls[j];
            if (!method_impl_accepts_surface_call(ctx, impl, method, argc)) continue;
            ok = candidate_type_push(&types, &type_count, &type_cap, impl, method, err, span);
        }
    }
    if (!ok) {
        free(types);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return NULL;
    }
    const char *method_name = method_surface_name_text(group->items[0]);
    IdmCore *multi = idm_core_fn_multi(method_name, span);
    if (!multi) {
        free(types);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < type_count; i++) {
        if (!method_multi_add_type_clause(ctx, multi, &types[i], method_name, argc, span, err)) {
            free(types);
            idm_core_free(multi);
            idm_core_free(receiver);
            core_args_free(args, arg_count);
            return NULL;
        }
    }
    if (!method_multi_add_fallback_clause(ctx, multi, method_name, argc, span, err)) {
        free(types);
        idm_core_free(multi);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return NULL;
    }
    free(types);
    IdmCore *call = idm_core_call(multi, receiver ? receiver->span : span);
    if (!call) {
        idm_core_free(multi);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (receiver && !idm_core_call_add_arg(call, receiver)) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (!idm_core_call_add_arg(call, args[i])) {
            core_args_free(args + i, arg_count - i);
            idm_core_free(call);
            if (!err->present) idm_error_oom(err, span);
            return NULL;
        }
        args[i] = NULL;
    }
    return call;
}

static IdmCore *expand_method_surface_call_cores(ExpandContext *ctx, const MethodSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    size_t argc = (receiver ? 1u : 0u) + arg_count;
    if (argc > UINT32_MAX || !method_group_accepts(group, (uint32_t)argc)) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return method_group_arity_error(err, span, group, argc);
    }
    const MethodSurfaceDef *method = NULL;
    size_t accepted_count = 0;
    for (size_t i = 0; i < group->count; i++) {
        if (!idm_arity_accepts(&group->items[i]->arity, (uint32_t)argc)) continue;
        method = group->items[i];
        accepted_count++;
    }
    if (accepted_count == 0 || !method) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return expand_error(err, span, "ambiguous method '%s'", method_surface_name_text(group->items[0]));
    }
    if (accepted_count > 1u) return expand_composite_method_surface_call(ctx, group, receiver, args, arg_count, (uint32_t)argc, span, err);
    if (!method->has_dispatch) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return expand_error(err, span, "method '%s.%s' has no compiled dispatcher", method_surface_trait_text(method), method_surface_name_text(method));
    }
    IdmCore *callee = method_dispatch_ref(ctx, method, span, err);
    IdmCore *call = callee ? idm_core_call(callee, receiver ? receiver->span : span) : NULL;
    if (!call) {
        idm_core_free(callee);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (receiver && !idm_core_call_add_arg(call, receiver)) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, receiver->span);
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (!idm_core_call_add_arg(call, args[i])) {
            core_args_free(args + i, arg_count - i);
            idm_core_free(call);
            if (!err->present) idm_error_oom(err, span);
            return NULL;
        }
        args[i] = NULL;
    }
    return call;
}

static bool arg_parse_at_stop(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator) {
    return pos >= end || (stop_at_operator && op_lookup(ctx, items[pos], false) != NULL);
}

static bool adjacent_items_present(IdmSyntax *const *items, size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
        if (syn_is_form(items[i], "%-adjacent")) return true;
    }
    return false;
}

static bool flatten_adjacent_push(IdmSyntax ***out_items, size_t *out_count, size_t *out_cap, IdmSyntax *item) {
    if (*out_count == *out_cap) {
        if (!idm_grow((void **)out_items, out_cap, sizeof(**out_items), 8u, *out_count + 1u)) return false;
    }
    (*out_items)[(*out_count)++] = item;
    return true;
}

static bool flatten_adjacent_walk(IdmSyntax ***out_items, size_t *out_count, size_t *out_cap, IdmSyntax *item) {
    if (syn_is_form(item, "%-adjacent") && item->as.seq.count >= 2u) {
        for (size_t i = 1; i < item->as.seq.count; i++) {
            if (!flatten_adjacent_walk(out_items, out_count, out_cap, item->as.seq.items[i])) return false;
        }
        return true;
    }
    return flatten_adjacent_push(out_items, out_count, out_cap, item);
}

static bool flatten_adjacent_items(IdmSyntax *const *items, size_t start, size_t end, IdmSyntax ***out_items, size_t *out_count) {
    *out_items = NULL;
    *out_count = 0;
    size_t cap = 0;
    for (size_t i = start; i < end; i++) {
        if (!flatten_adjacent_walk(out_items, out_count, &cap, items[i])) {
            free(*out_items);
            *out_items = NULL;
            *out_count = 0;
            return false;
        }
    }
    return true;
}

static IdmCore *parse_dot_tail(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, IdmCore *receiver, IdmError *err) {
    while (!arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator) && syn_is_dot(items[*pos])) {
        size_t dot = *pos;
        if (dot + 1u >= end || items[dot + 1u]->kind != IDM_SYN_WORD) {
            idm_core_free(receiver);
            return expand_error(err, items[dot]->span, "dot dispatch expects a method name");
        }
        IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
        FieldSurfaceGroup fields = {0};
        if (!resolve_field_surface_group(ctx, items[dot + 1u], &fields, &field_status, err)) {
            idm_core_free(receiver);
            return NULL;
        }
        if (field_status == IDM_RESOLVE_AMBIGUOUS) {
            idm_core_free(receiver);
            field_surface_group_destroy(&fields);
            return expand_error(err, items[dot + 1u]->span, "ambiguous field '%s'", items[dot + 1u]->as.text);
        }
        IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
        MethodSurfaceGroup group = {0};
        if (!resolve_method_surface_group(ctx, items[dot + 1u], &group, &method_status, err)) {
            idm_core_free(receiver);
            field_surface_group_destroy(&fields);
            return NULL;
        }
        if (method_status == IDM_RESOLVE_AMBIGUOUS) {
            idm_core_free(receiver);
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            return expand_error(err, items[dot + 1u]->span, "ambiguous method '%s'", items[dot + 1u]->as.text);
        }
        if (fields.count != 0 && group.count != 0) {
            idm_core_free(receiver);
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            return expand_error(err, items[dot + 1u]->span, "ambiguous member '%s' is both a record field and a trait method", items[dot + 1u]->as.text);
        }
        if (fields.count == 0 && group.count == 0) {
            idm_core_free(receiver);
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            return expand_error(err, items[dot + 1u]->span, "unbound method '%s'", items[dot + 1u]->as.text);
        }
        if (fields.count != 0) {
            *pos = dot + 2u;
            receiver = expand_field_surface_call_cores(ctx, &fields, receiver, NULL, 0u, items[dot + 1u]->span, err);
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            if (!receiver) return NULL;
            continue;
        }
        field_surface_group_destroy(&fields);
        const MethodSurfaceDef *method = group.items[0];
        uint32_t dot_arity = 0;
        if (!method_group_max_accepting_at_least(&group, 1u, &dot_arity)) {
            idm_core_free(receiver);
            method_surface_group_destroy(&group);
            return expand_error(err, items[dot + 1u]->span, "method '%s.%s' cannot be used with dot dispatch because it takes no receiver", method_surface_trait_text(method), method_surface_name_text(method));
        }
        uint32_t max_extra_count = dot_arity - 1u;
        uint32_t available = application_available_args(ctx, items, dot + 2u, end, stop_at_operator);
        size_t target_extra_count = method_group_target_extra_count(&group, 1u, available, max_extra_count);
        IdmCore **args = target_extra_count ? calloc(target_extra_count, sizeof(*args)) : NULL;
        if (target_extra_count && !args) {
            idm_core_free(receiver);
            method_surface_group_destroy(&group);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, items[dot + 1u]->span);
        }
        *pos = dot + 2u;
        size_t extra_count = 0;
        for (; extra_count < target_extra_count; extra_count++) {
            if (arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator)) {
                break;
            }
            size_t arg_end = end;
            uint32_t reserve = (uint32_t)(target_extra_count - extra_count - 1u);
            size_t remaining = end - *pos;
            if ((size_t)reserve < remaining) arg_end = end - (size_t)reserve;
            if (reserve == 0u && target_extra_count < available && *pos + 1u < arg_end) {
                IdmArity arg_arity = idm_arity_unknown();
                bool arg_known = syntax_call_arity(ctx, items[*pos], &arg_arity, err);
                if (err && err->present) {
                    core_args_free(args, extra_count);
                    free(args);
                    idm_core_free(receiver);
                    method_surface_group_destroy(&group);
                    return NULL;
                }
                if (!arg_known || arg_arity.max == 0u) arg_end = application_postfix_arg_end(ctx, items, *pos, arg_end, stop_at_operator, err);
            }
            args[extra_count] = parse_application_expr(ctx, items, pos, arg_end, stop_at_operator, false, err);
            if (!args[extra_count]) {
                core_args_free(args, extra_count);
                free(args);
                idm_core_free(receiver);
                method_surface_group_destroy(&group);
                return NULL;
            }
        }
        receiver = expand_method_surface_call_cores(ctx, &group, receiver, args, extra_count, items[dot + 1u]->span, err);
        free(args);
        method_surface_group_destroy(&group);
        if (!receiver) return NULL;
    }
    return receiver;
}

static IdmCore *parse_postfix_expr(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, IdmError *err) {
    if (arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator)) return expand_error(err, idm_span_unknown(NULL), "expected expression");
    IdmSyntax *head = items[(*pos)++];
    if (syn_is_dot(head)) return expand_error(err, head->span, "dot dispatch requires a receiver");
    IdmCore *core = expand_syntax(ctx, head, err);
    if (!core) return NULL;
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, core, err);
}

static IdmCore *parse_callee_primary(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, IdmError *err) {
    if (arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator)) return expand_error(err, idm_span_unknown(NULL), "expected expression");
    IdmSyntax *head = items[(*pos)++];
    if (syn_is_dot(head)) return expand_error(err, head->span, "dot dispatch requires a receiver");
    IdmCore *core = head->kind == IDM_SYN_WORD ? expand_word_callee(ctx, head, err) : expand_syntax(ctx, head, err);
    if (!core) return NULL;
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, core, err);
}

static bool syntax_forbids_call(const IdmSyntax *syn) {
    return syntax_is_literal_value(syn);
}

static uint32_t application_available_args(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator) {
    uint32_t available = 0;
    while (!arg_parse_at_stop(ctx, items, pos, end, stop_at_operator) && available < UINT32_MAX) {
        available++;
        pos++;
    }
    return available;
}

static uint32_t application_target_argc(const IdmArity *arity, uint32_t available) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return available;
    uint32_t max = arity->max < available ? arity->max : available;
    for (uint32_t argc = max + 1u; argc > 0u; argc--) {
        uint32_t candidate = argc - 1u;
        if (idm_arity_accepts(arity, candidate)) return candidate;
    }
    return max;
}

static size_t application_postfix_arg_end(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator, IdmError *err) {
    size_t cursor = pos + 1u;
    while (!arg_parse_at_stop(ctx, items, cursor, end, stop_at_operator) &&
           syn_is_dot(items[cursor]) &&
           cursor + 1u < end &&
           items[cursor + 1u]->kind == IDM_SYN_WORD) {
        IdmResolveStatus field_status = IDM_RESOLVE_UNBOUND;
        FieldSurfaceGroup fields = {0};
        if (!resolve_field_surface_group(ctx, items[cursor + 1u], &fields, &field_status, NULL)) {
            field_surface_group_destroy(&fields);
            break;
        }
        IdmResolveStatus method_status = IDM_RESOLVE_UNBOUND;
        MethodSurfaceGroup group = {0};
        if (!resolve_method_surface_group(ctx, items[cursor + 1u], &group, &method_status, NULL)) {
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            break;
        }
        if ((fields.count != 0 && group.count != 0) || field_status == IDM_RESOLVE_AMBIGUOUS || method_status == IDM_RESOLVE_AMBIGUOUS) {
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            break;
        }
        if (fields.count != 0 || group.count == 0) {
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            cursor += 2u;
            continue;
        }
        uint32_t dot_arity = 0;
        if (!method_group_max_accepting_at_least(&group, 1u, &dot_arity)) {
            method_surface_group_destroy(&group);
            field_surface_group_destroy(&fields);
            cursor += 2u;
            continue;
        }
        uint32_t max_extra = dot_arity - 1u;
        uint32_t available = application_available_args(ctx, items, cursor + 2u, end, stop_at_operator);
        size_t target_extra = method_group_target_extra_count(&group, 1u, available, max_extra);
        cursor += 2u;
        for (size_t i = 0; i < target_extra && !arg_parse_at_stop(ctx, items, cursor, end, stop_at_operator); i++) {
            size_t arg_end = end;
            uint32_t reserve = (uint32_t)(target_extra - i - 1u);
            size_t remaining = end - cursor;
            if ((size_t)reserve < remaining) arg_end = end - (size_t)reserve;
            if (reserve == 0u && target_extra < available && cursor + 1u < arg_end) {
                IdmArity arg_arity = idm_arity_unknown();
                bool arg_known = syntax_call_arity(ctx, items[cursor], &arg_arity, err);
                if (err && err->present) {
                    method_surface_group_destroy(&group);
                    field_surface_group_destroy(&fields);
                    return cursor;
                }
                if (!arg_known || arg_arity.max == 0u) arg_end = application_postfix_arg_end(ctx, items, cursor, arg_end, stop_at_operator, err);
            }
            cursor = arg_end;
        }
        method_surface_group_destroy(&group);
        field_surface_group_destroy(&fields);
    }
    return cursor;
}

static IdmCore *application_surface_call_from(ExpandContext *ctx, FieldSurfaceGroup *fields, MethodSurfaceGroup *methods, IdmSyntax *const *items, size_t start, size_t *pos, size_t end, bool stop_at_operator, bool known, IdmArity arity, IdmError *err) {
    uint32_t available = application_available_args(ctx, items, *pos, end, stop_at_operator);
    uint32_t max = known ? application_target_argc(&arity, available) : available;
    IdmCore **args = max ? calloc(max, sizeof(*args)) : NULL;
    if (max && !args) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    uint32_t argc = 0;
    while (!arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator) && argc < max) {
        size_t arg_end = end;
        if (known) {
            uint32_t reserve = max - argc - 1u;
            size_t remaining = end - *pos;
            if ((size_t)reserve < remaining) arg_end = end - (size_t)reserve;
            if (reserve == 0u && max < available && *pos + 1u < arg_end) {
                IdmArity arg_arity = idm_arity_unknown();
                bool arg_known = syntax_call_arity(ctx, items[*pos], &arg_arity, err);
                if (err && err->present) {
                    core_args_free(args, argc);
                    free(args);
                    return NULL;
                }
                if (!arg_known || arg_arity.max == 0u) arg_end = application_postfix_arg_end(ctx, items, *pos, arg_end, stop_at_operator, err);
            }
        }
        args[argc] = known ? parse_application_expr(ctx, items, pos, arg_end, stop_at_operator, false, err)
                           : parse_postfix_expr(ctx, items, pos, arg_end, stop_at_operator, err);
        if (!args[argc]) {
            core_args_free(args, argc);
            free(args);
            return NULL;
        }
        argc++;
    }
    IdmCore *call = fields->count != 0
        ? expand_field_surface_call_cores(ctx, fields, NULL, args, argc, items[start]->span, err)
        : expand_method_surface_call_cores(ctx, methods, NULL, args, argc, items[start]->span, err);
    free(args);
    if (!call) return NULL;
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, call, err);
}

static IdmCore *application_call_from(ExpandContext *ctx, IdmCore *callee, IdmSyntax *const *items, size_t start, size_t *pos, size_t end, bool stop_at_operator, bool known, IdmArity arity, IdmError *err) {
    IdmCore *call = idm_core_call(callee, items[start]->span);
    if (!call) {
        idm_core_free(callee);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    uint32_t available = application_available_args(ctx, items, *pos, end, stop_at_operator);
    uint32_t max = known ? application_target_argc(&arity, available) : UINT32_MAX;
    uint32_t argc = 0;
    while (!arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator) && argc < max) {
        size_t arg_end = end;
        if (known) {
            uint32_t reserve = max - argc - 1u;
            size_t remaining = end - *pos;
            if ((size_t)reserve < remaining) arg_end = end - (size_t)reserve;
            if (reserve == 0u && max < available && *pos + 1u < arg_end) {
                IdmArity arg_arity = idm_arity_unknown();
                bool arg_known = syntax_call_arity(ctx, items[*pos], &arg_arity, err);
                if (err && err->present) {
                    idm_core_free(call);
                    return NULL;
                }
                if (!arg_known || arg_arity.max == 0u) arg_end = application_postfix_arg_end(ctx, items, *pos, arg_end, stop_at_operator, err);
            }
        }
        IdmCore *arg = known ? parse_application_expr(ctx, items, pos, arg_end, stop_at_operator, false, err)
                             : parse_postfix_expr(ctx, items, pos, arg_end, stop_at_operator, err);
        if (!arg) {
            idm_core_free(call);
            return NULL;
        }
        if (!core_call_add_arg_or_free(call, arg, err, items[start]->span)) return NULL;
        argc++;
    }
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, call, err);
}

static IdmCore *parse_application_expr(ExpandContext *ctx, IdmSyntax *const *items, size_t *pos, size_t end, bool stop_at_operator, bool allow_unknown_call, IdmError *err) {
    if (arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator)) return expand_error(err, idm_span_unknown(NULL), "expected expression");
    size_t start = *pos;
    IdmArity arity = idm_arity_unknown();
    bool known = syntax_call_arity(ctx, items[start], &arity, err);
    if (err && err->present) return NULL;
    bool can_take_arg = !arg_parse_at_stop(ctx, items, start + 1u, end, stop_at_operator) && !syn_is_dot(items[start + 1u]);
    FieldSurfaceGroup fields = {0};
    MethodSurfaceGroup methods = {0};
    bool has_member_surface = false;
    if (!resolve_unbound_member_surface(ctx, items[start], &fields, &methods, &has_member_surface, err)) return NULL;
    if (has_member_surface && can_take_arg && known && arity.max != 0u) {
        *pos = start + 1u;
        IdmCore *call = application_surface_call_from(ctx, &fields, &methods, items, start, pos, end, stop_at_operator, known, arity, err);
        member_surface_groups_destroy(&fields, &methods);
        return call;
    }
    member_surface_groups_destroy(&fields, &methods);
    if (can_take_arg && ((known && arity.max != 0u) || (items[start]->kind == IDM_SYN_WORD && !known && allow_unknown_call))) {
        *pos = start;
        IdmCore *callee = parse_callee_primary(ctx, items, pos, end, stop_at_operator, err);
        if (!callee) return NULL;
        return application_call_from(ctx, callee, items, start, pos, end, stop_at_operator, known, arity, err);
    }
    IdmCore *core = parse_postfix_expr(ctx, items, pos, end, stop_at_operator, err);
    if (!core) return NULL;
    if (!allow_unknown_call || arg_parse_at_stop(ctx, items, *pos, end, stop_at_operator)) return core;
    if (syntax_forbids_call(items[start])) {
        idm_core_free(core);
        for (size_t i = start + 1u; i < end; i++) {
            const char *text = surface_token_text(items[i]);
            if (!operator_shaped_text(text) || op_lookup(ctx, items[i], true) || op_lookup(ctx, items[i], false)) continue;
            if (items[i]->kind == IDM_SYN_WORD) return expand_unbound_identifier(ctx, items[i], err);
            return expand_error(err, items[i]->span, "unbound identifier '%s'", text);
        }
        expand_error(err, items[start]->span, "literal cannot be used as a function");
        if (*pos < end && syntax_is_negative_number(items[*pos])) {
            idm_error_note(err, "whitespace before a negative literal makes this a call; use spaces around '-' for subtraction");
        }
        return NULL;
    }
    return application_call_from(ctx, core, items, start, pos, end, stop_at_operator, false, idm_arity_unknown(), err);
}

IdmCore *expand_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return expand_error(err, idm_span_unknown(NULL), "empty expression");
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
    (void)saw_dot;
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

static bool binding_is_primitive_backed(const IdmBinding *binding, IdmPrimitive primitive) {
    return binding &&
           (binding->kind == IDM_BIND_ENV || binding->kind == IDM_BIND_PACKAGE_SLOT) &&
           binding->primitive_backed &&
           binding->primitive == (uint32_t)primitive;
}

static IdmCore *expand_cond_primitive_clause_call(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    size_t len = end - start;
    if (len != 3u && len != 4u) return expand_error(err, items[start]->span, "cond expects two or three arguments: condition, then[, else]");
    IdmCore *condition = expand_syntax(ctx, items[start + 1u], err);
    IdmCore *then_branch = condition ? expand_syntax(ctx, items[start + 2u], err) : NULL;
    IdmCore *else_branch = NULL;
    if (then_branch) {
        else_branch = len == 4u ? expand_syntax(ctx, items[start + 3u], err)
                                : idm_core_literal(idm_nil(), items[start]->span);
    }
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

static bool surface_operator_boundary(ExpandContext *ctx, const IdmSyntax *syn, const IdmOperatorDef **out_syntax_capture, IdmError *err) {
    const IdmOperatorDef *syntax_capture = op_lookup_syntax_capture(ctx, syn, true, err);
    if (err && err->present) return false;
    if (out_syntax_capture) *out_syntax_capture = syntax_capture;
    return syntax_capture || op_lookup(ctx, syn, false) != NULL;
}

static size_t delimited_body_primary_end(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    size_t last_body = SIZE_MAX;
    for (size_t i = start + 1u; i < end; i++) {
        if (syn_is_form(items[i], "%-body")) last_body = i;
    }
    if (last_body == SIZE_MAX) return SIZE_MAX;
    size_t pos = last_body + 1u;
    while (pos < end) {
        const IdmOperatorDef *syntax_capture = NULL;
        if (surface_operator_boundary(ctx, items[pos], &syntax_capture, err) || (err && err->present)) break;
        pos++;
    }
    return pos;
}

static size_t surface_primary_end(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return start;
    size_t body_end = delimited_body_primary_end(ctx, items, start, end, err);
    if (body_end != SIZE_MAX || (err && err->present)) return body_end;

    const SurfaceKeywordDef *keyword = surface_keyword_for_syntax(items[start]);
    if (keyword && keyword->primary_takes_rest) return end;

    const char *head = surface_token_text(items[start]);

    size_t pos = start + 1u;
    if (!head) return pos;
    while (pos < end) {
        const IdmOperatorDef *syntax_capture = NULL;
        if (surface_operator_boundary(ctx, items[pos], &syntax_capture, err) || (err && err->present)) break;
        if (syn_is_dot(items[pos])) break;
        size_t arg_end = delimited_body_primary_end(ctx, items, pos, end, err);
        if (err && err->present) break;
        pos = arg_end == SIZE_MAX ? pos + 1u : arg_end;
    }
    return pos;
}

static bool scan_operator_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, size_t *out_syntax_op_index, const IdmOperatorDef **out_syntax_infix, bool *out_has_operator, IdmError *err) {
    size_t syntax_op_index = 0;
    const IdmOperatorDef *syntax_infix = NULL;
    bool has_operator = op_lookup(ctx, items[start], true) != NULL;
    bool expect_operand = true;
    size_t i = start;
    while (i < end) {
        if (expect_operand) {
            if (op_lookup(ctx, items[i], true)) {
                has_operator = true;
                i++;
                continue;
            }
            size_t next = surface_primary_end(ctx, items, i, end, err);
            if (err && err->present) return false;
            if (next <= i) next = i + 1u;
            i = next;
            expect_operand = false;
            continue;
        }
        const IdmOperatorDef *candidate = NULL;
        if (surface_operator_boundary(ctx, items[i], &candidate, err)) {
            if (candidate) {
                syntax_infix = candidate;
                syntax_op_index = i;
                break;
            }
            has_operator = true;
            i++;
            expect_operand = true;
            continue;
        }
        if (err && err->present) return false;
        size_t next = surface_primary_end(ctx, items, i, end, err);
        if (err && err->present) return false;
        if (next <= i) break;
        i = next;
    }
    if (out_syntax_op_index) *out_syntax_op_index = syntax_op_index;
    if (out_syntax_infix) *out_syntax_infix = syntax_infix;
    if (out_has_operator) *out_has_operator = has_operator;
    return true;
}

bool surface_parts_have_operator_boundary(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return false;
    size_t syntax_op_index = 0;
    const IdmOperatorDef *syntax_infix = NULL;
    bool has_operator = false;
    if (!scan_operator_parts(ctx, items, start, end, &syntax_op_index, &syntax_infix, &has_operator, err)) return false;
    (void)syntax_op_index;
    return syntax_infix || has_operator;
}

static IdmCore *expand_operator_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, bool *handled, IdmError *err) {
    *handled = false;
    size_t syntax_op_index = 0;
    const IdmOperatorDef *syntax_infix = NULL;
    bool has_operator = false;
    if (!scan_operator_parts(ctx, items, start, end, &syntax_op_index, &syntax_infix, &has_operator, err)) {
        *handled = true;
        return NULL;
    }
    if (syntax_infix) {
        *handled = true;
        return expand_syntax_capture_dispatch(ctx, syntax_infix, items[syntax_op_index], items, start, syntax_op_index, syntax_op_index + 1u, end, err);
    }
    if (has_operator) {
        *handled = true;
        EnforestParser parser = {ctx, items, end, start, err};
        IdmCore *expr = parse_enforest_expr(&parser, 0);
        if (!expr) return NULL;
        if (parser.pos != end) {
            idm_core_free(expr);
            return expand_error(err, items[parser.pos]->span, "unexpected trailing syntax after operator expression");
        }
        return expr;
    }
    return NULL;
}

static IdmCore *expand_parts_inner(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return expand_error(err, idm_span_unknown(NULL), "empty expression");
    size_t len = end - start;
    if (len > 1u) {
        bool handled = false;
        IdmCore *core = expand_operator_parts(ctx, items, start, end, &handled, err);
        if (handled) return core;
    }
    if (items[start]->kind == IDM_SYN_WORD) {
        uint32_t payload = 0;
        if (resolve_transformer(ctx, items[start], &payload, err)) return expand_macro_use_from_parts(ctx, items, start, end, payload, err);
        if (err && err->present) return NULL;
    }
    const SurfaceKeywordDef *keyword = surface_keyword_for_syntax(items[start]);
    if (keyword && keyword->handler) return keyword->handler(ctx, items, start, end, err);
    if (items[start]->kind == IDM_SYN_WORD) {
        const IdmOperatorDef *syntax_capture = op_lookup_syntax_capture(ctx, items[start], false, err);
        if (err && err->present) return NULL;
        if (syntax_capture) return expand_syntax_capture_dispatch(ctx, syntax_capture, items[start], items, start, start, start + 1u, end, err);
    }
    if (items[start]->kind == IDM_SYN_WORD) {
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        const IdmBinding *binding = resolve_default(ctx, items[start], &status);
        if (status == IDM_RESOLVE_OK && binding_is_primitive_backed(binding, IDM_PRIM_COND)) {
            return expand_cond_primitive_clause_call(ctx, items, start, end, err);
        }
    }
    bool has_member_surface = false;
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start])) {
        FieldSurfaceGroup fields = {0};
        MethodSurfaceGroup methods = {0};
        if (!resolve_unbound_member_surface(ctx, items[start], &fields, &methods, &has_member_surface, err)) return NULL;
        member_surface_groups_destroy(&fields, &methods);
    }
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start])) {
        const char *w = items[start]->as.text;
        if (strcmp(w, "defprotocol") == 0)
            return expand_error(err, items[start]->span, "'%s' (records/method dispatch) is not implemented", w);
        if (strcmp(w, "record") == 0)
            return expand_error(err, items[start]->span, "record expects 'record NAME do field ... end'");
    }
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start]) &&
        !has_member_surface && !op_lookup(ctx, items[start], true) && !op_lookup(ctx, items[start], false)) {
        const PhaseSyntaxFn *core_syntax_fn = NULL;
        if (resolve_head_core_syntax(ctx, items[start], &core_syntax_fn, err)) return expand_core_syntax_use_from_parts(ctx, items, start, end, core_syntax_fn, err);
        if (err && err->present) return NULL;
        if (reserved_prefix_string(items, start, end, err)) return NULL;
        return expand_unbound_identifier(ctx, items[start], err);
    }
    if (adjacent_items_present(items, start, end)) {
        IdmSyntax **flat = NULL;
        size_t flat_count = 0;
        if (!flatten_adjacent_items(items, start, end, &flat, &flat_count)) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
        IdmCore *core = expand_parts(ctx, flat, 0, flat_count, err);
        free(flat);
        return core;
    }
    if (reserved_prefix_string(items, start, end, err)) return NULL;
    size_t pos = start;
    IdmCore *expr = parse_application_expr(ctx, items, &pos, end, false, true, err);
    if (!expr) return NULL;
    if (pos != end) {
        idm_core_free(expr);
        return expand_error(err, items[pos]->span, "unexpected trailing syntax after expression");
    }
    return expr;
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
    const char *head_text = surface_token_text(head);
    if (head_text && op_lookup(parser->ctx, head, false)) {
        return expand_error(parser->err, head->span, "operator '%s' cannot appear where an operand is required", head_text);
    }

    size_t primary_end = delimited_body_primary_end(parser->ctx, parser->items, start, parser->end, parser->err);
    if (parser->err && parser->err->present) return NULL;
    if (primary_end != SIZE_MAX) {
        parser->pos = primary_end;
        IdmCore *core = expand_parts(parser->ctx, parser->items, start, primary_end, parser->err);
        if (!core) return NULL;
        return parse_dot_tail(parser->ctx, parser->items, &parser->pos, parser->end, true, core, parser->err);
    }

    const SurfaceKeywordDef *keyword = surface_keyword_for_syntax(head);
    if (keyword && keyword->operand_takes_rest && keyword->handler) {
        parser->pos = parser->end;
        return keyword->handler(parser->ctx, parser->items, start, parser->end, parser->err);
    }

    parser->pos = start;
    return parse_application_expr(parser->ctx, parser->items, &parser->pos, parser->end, true, true, parser->err);
}

static IdmSyntax *operator_target_word(const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err) {
    if (!op->target_name) { idm_error_set(err, span, "operator '%s' has no target", op->name); return NULL; }
    IdmSyntax *word = idm_syn_word(op->target_name, op_token ? op_token->span : span);
    if (!word) { idm_error_oom(err, span); return NULL; }
    for (size_t i = 0; i < op->scopes.count; i++) {
        if (!idm_syn_scope_add(word, 0, op->scopes.items[i])) {
            idm_syn_free(word);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    return word;
}

static IdmCore *expand_syntax_capture_dispatch(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSyntax *const *items, size_t left_start, size_t left_end, size_t cursor, size_t end, IdmError *err) {
    IdmSyntax *target_word = operator_target_word(op, op_token, op_token ? op_token->span : idm_span_unknown(NULL), err);
    if (!target_word) return NULL;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, target_word, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) {
        expand_error(err, target_word->span, "ambiguous operator target '%s'", op->target_name);
        idm_syn_free(target_word);
        return NULL;
    }
    if (status == IDM_RESOLVE_OK && binding && binding->kind == IDM_BIND_TRANSFORMER)
        return expand_syntax_capture_macro(ctx, op, op_token, target_word, binding->payload, items, left_start, left_end, cursor, end, err);
    idm_syn_free(target_word);
    IdmCore *left = left_start == left_end ? NULL : expand_parts(ctx, items, left_start, left_end, err);
    return (left_start == left_end || left) ? expand_syntax_capture_tail(ctx, op, op_token, left, items, cursor, end, err) : NULL;
}

static IdmCore *operator_callee(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err) {
    IdmSyntax *word = operator_target_word(op, op_token, span, err);
    if (!word) return NULL;
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (status == IDM_RESOLVE_OK && binding->kind == IDM_BIND_TRANSFORMER) {
        idm_syn_free(word);
        return expand_error(err, span, "operator target '%s' is phase syntax; use a syntax capture such as capture: {:infix :expression}", op->target_name);
    }
    IdmCore *callee = expand_word_callee(ctx, word, err);
    idm_syn_free(word);
    return callee;
}

static IdmCore *make_operator_call(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmCore *right, IdmSpan span, IdmError *err) {
    IdmCore *args[2] = {left, right};
    return make_operator_call_args(ctx, op, op_token, args, 2u, span, err);
}

static IdmCore *make_operator_unary(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *operand, IdmSpan span, IdmError *err) {
    IdmCore *args[1] = {operand};
    return make_operator_call_args(ctx, op, op_token, args, 1u, span, err);
}

static void core_args_free(IdmCore **args, size_t arg_count) {
    for (size_t i = 0; i < arg_count; i++) idm_core_free(args[i]);
}

static IdmCore *make_operator_call_args(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    IdmCore *callee = operator_callee(ctx, op, op_token, span, err);
    if (!callee) {
        core_args_free(args, arg_count);
        return NULL;
    }
    IdmCore *call = idm_core_call(callee, span);
    if (!call) {
        idm_core_free(callee);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (!core_call_add_arg_or_free(call, args[i], err, span)) {
            core_args_free(args + i + 1u, arg_count - i - 1u);
            return NULL;
        }
        args[i] = NULL;
    }
    return call;
}

static IdmCore *syntax_literal_core(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmValue value = idm_syntax_value(ctx->rt, syn, err);
    if (err && err->present) return NULL;
    IdmCore *core = idm_core_literal(value, syn ? syn->span : idm_span_unknown(NULL));
    if (!core) idm_error_oom(err, syn ? syn->span : idm_span_unknown(NULL));
    return core;
}

static IdmSyntax *syntax_list_from_range(IdmSyntax *const *items, size_t start, size_t end, IdmSpan span, IdmError *err) {
    IdmSyntax *captured = idm_syn_list(span);
    if (!captured) { idm_error_oom(err, span); return NULL; }
    for (size_t i = start; i < end; i++) {
        IdmSyntax *item = idm_syn_clone(items[i]);
        if (!item || !idm_syn_append(captured, item)) {
            idm_syn_free(item);
            idm_syn_free(captured);
            idm_error_oom(err, items[i]->span);
            return NULL;
        }
    }
    return captured;
}

static const char *sentinel_text(const IdmSyntax *syn) {
    if (!syn) return NULL;
    if (syn->kind == IDM_SYN_WORD || syn->kind == IDM_SYN_ATOM || syn->kind == IDM_SYN_STRING) return syn->as.text;
    if (syn_is_form(syn, "%-word") && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
    return NULL;
}

static bool copy_use_scopes(IdmSyntax *dst, const IdmSyntax *src, IdmError *err) {
    if (!dst || !src) return true;
    for (int phase = 0; phase < 2; phase++) {
        const IdmScopeSet *scopes = idm_syn_scope_set(src, phase);
        for (size_t i = 0; scopes && i < scopes->count; i++) if (!idm_syn_scope_add(dst, phase, scopes->items[i])) return idm_error_oom(err, src->span);
    }
    return true;
}

static bool append_syntax_or_oom(IdmSyntax *seq, IdmSyntax *item, IdmSpan span, IdmError *err) {
    if (item && idm_syn_append(seq, item)) return true;
    idm_syn_free(item);
    idm_error_oom(err, span);
    return false;
}

static void syntax_payload_free(SyntaxCapturePayload *payload) {
    for (size_t i = 0; i < payload->count; i++) idm_syn_free(payload->items[i]);
    memset(payload, 0, sizeof(*payload));
}

static bool syntax_payload_add(SyntaxCapturePayload *payload, IdmSyntax *item, IdmSpan span, IdmError *err) {
    if (!item) return err && err->present ? false : idm_error_oom(err, span);
    payload->items[payload->count++] = item;
    return true;
}

static bool build_syntax_capture_payload(ExpandContext *ctx, uint8_t kind, uint32_t count, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSyntax *const *items, size_t cursor, size_t end, SyntaxCapturePayload *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (kind == IDM_OPERATOR_CAPTURE_INDENTED) {
        if (cursor + 1u != end || !syn_is_form(items[cursor], "%-body")) return idm_error_set(err, op_token->span, "operator '%s' expects an indented do/end body", op->name);
        return syntax_payload_add(out, idm_syn_clone(items[cursor]), items[cursor]->span, err);
    }
    if (kind == IDM_OPERATOR_CAPTURE_COUNT) {
        size_t close = cursor + (size_t)count;
        if (close > end) return idm_error_set(err, op_token->span, "operator '%s' expected %u captured syntax item(s)", op->name, count);
        if (close != end) return idm_error_set(err, items[close]->span, "unexpected trailing syntax after operator '%s' count capture", op->name);
        return syntax_payload_add(out, syntax_list_from_range(items, cursor, close, op_token->span, err), op_token->span, err);
    }
    if (kind == IDM_OPERATOR_CAPTURE_EXPRESSION) {
        if (cursor >= end) return idm_error_set(err, op_token->span, "operator '%s' expression capture requires an expression", op->name);
        return syntax_payload_add(out, syntax_use_from_parts(ctx, items, cursor, end, err), op_token->span, err);
    }
    if (kind != IDM_OPERATOR_CAPTURE_SENTINEL) return idm_error_set(err, op_token->span, "operator '%s' has unsupported capture '%s'", op->name, op->capture ? op->capture : "");
    if (cursor >= end) return idm_error_set(err, op_token->span, "operator '%s' sentinel capture requires a sentinel token", op->name);
    const char *sentinel = sentinel_text(items[cursor]);
    if (!sentinel) return idm_error_set(err, items[cursor]->span, "operator '%s' sentinel must be a word, atom, or string", op->name);
    size_t close = SIZE_MAX;
    for (size_t i = cursor + 1u; i < end; i++) {
        const char *text = sentinel_text(items[i]);
        if (text && strcmp(text, sentinel) == 0) { close = i; break; }
    }
    if (close == SIZE_MAX) return idm_error_set(err, items[cursor]->span, "operator '%s' sentinel '%s' was not closed", op->name, sentinel);
    if (close + 1u != end) return idm_error_set(err, items[close + 1u]->span, "unexpected trailing syntax after operator '%s' sentinel capture", op->name);
    return syntax_payload_add(out, idm_syn_clone(items[cursor]), items[cursor]->span, err) &&
           syntax_payload_add(out, syntax_list_from_range(items, cursor + 1u, close, items[cursor]->span, err), items[cursor]->span, err);
}

static IdmSyntax *operator_macro_use_start(const IdmSyntax *op_token, IdmSyntax *target_word, IdmError *err) {
    IdmSpan span = op_token ? op_token->span : idm_span_unknown(NULL);
    IdmSyntax *use = idm_syn_list(span);
    if (!use) { idm_syn_free(target_word); idm_error_oom(err, span); return NULL; }
    if (!append_syntax_or_oom(use, idm_syn_word("%-expr", span), span, err) || !copy_use_scopes(use, op_token, err) || !append_syntax_or_oom(use, target_word, span, err)) {
        idm_syn_free(use);
        return NULL;
    }
    return use;
}

static IdmCore *expand_syntax_capture_macro(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSyntax *target_word, uint32_t payload, IdmSyntax *const *items, size_t left_start, size_t left_end, size_t cursor, size_t end, IdmError *err) {
    uint8_t kind = op->capture_kind;
    if (kind == IDM_OPERATOR_CAPTURE_INVALID || kind == IDM_OPERATOR_CAPTURE_PREFIX || kind == IDM_OPERATOR_CAPTURE_INFIX || kind == IDM_OPERATOR_CAPTURE_POSTFIX) {
        idm_syn_free(target_word);
        return expand_error(err, op_token->span, "operator '%s' has unsupported capture '%s'", op->name, op->capture ? op->capture : "");
    }
    IdmSyntax *use = operator_macro_use_start(op_token, target_word, err);
    if (!use) return NULL;
    SyntaxCapturePayload captured = {0};
    bool right_first = op->capture_left && kind == IDM_OPERATOR_CAPTURE_EXPRESSION;
#define MACRO_CAPTURE_FAIL(span, ...) do { expand_error(err, span, __VA_ARGS__); goto fail; } while (0)
    if (right_first && !build_syntax_capture_payload(ctx, kind, op->capture_count, op, op_token, items, cursor, end, &captured, err)) goto fail;
    if (right_first) {
        for (size_t i = 0; i < captured.count; i++) {
            IdmSyntax *arg = captured.items[i];
            captured.items[i] = NULL;
            if (!append_syntax_or_oom(use, arg, arg ? arg->span : op_token->span, err)) goto fail;
        }
    }
    if (op->capture_left) {
        if (left_start == left_end) MACRO_CAPTURE_FAIL(op_token->span, "operator '%s' requires a left operand", op->name);
        IdmSyntax *left = syntax_use_from_parts(ctx, items, left_start, left_end, err);
        if (!append_syntax_or_oom(use, left, op_token->span, err)) goto fail;
    } else if (left_start != left_end) {
        MACRO_CAPTURE_FAIL(op_token->span, "operator '%s' does not accept a left operand", op->name);
    }
    if (!right_first) {
        if (!build_syntax_capture_payload(ctx, kind, op->capture_count, op, op_token, items, cursor, end, &captured, err)) goto fail;
        for (size_t i = 0; i < captured.count; i++) {
            IdmSyntax *arg = captured.items[i];
            captured.items[i] = NULL;
            if (!append_syntax_or_oom(use, arg, arg ? arg->span : op_token->span, err)) goto fail;
        }
    }
#undef MACRO_CAPTURE_FAIL
    IdmCore *core = expand_macro_use(ctx, use, use->as.seq.items[1], payload, err);
    idm_syn_free(use);
    return core;
fail:
    idm_syn_free(use);
    syntax_payload_free(&captured);
    return NULL;
}

static IdmCore *expand_syntax_capture_tail(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmSyntax *const *items, size_t cursor, size_t end, IdmError *err) {
    uint8_t kind = op->capture_kind;
    if (kind == IDM_OPERATOR_CAPTURE_INVALID || kind == IDM_OPERATOR_CAPTURE_PREFIX || kind == IDM_OPERATOR_CAPTURE_INFIX || kind == IDM_OPERATOR_CAPTURE_POSTFIX) {
        idm_core_free(left);
        return expand_error(err, op_token->span, "operator '%s' has unsupported capture '%s'", op->name, op->capture ? op->capture : "");
    }
    IdmCore *args[3] = {0};
    size_t arg_count = 0;
#define CAPTURE_FAIL(span, ...) do { core_args_free(args, arg_count); return expand_error(err, span, __VA_ARGS__); } while (0)
    if (op->capture_left) {
        if (!left) return expand_error(err, op_token->span, "operator '%s' requires a left operand", op->name);
        args[arg_count++] = left;
        left = NULL;
    } else if (left) {
        idm_core_free(left);
        return expand_error(err, op_token->span, "operator '%s' does not accept a left operand", op->name);
    }
    SyntaxCapturePayload captured = {0};
    if (!build_syntax_capture_payload(ctx, kind, op->capture_count, op, op_token, items, cursor, end, &captured, err)) goto oom;
    for (size_t i = 0; i < captured.count; i++) {
        args[arg_count] = syntax_literal_core(ctx, captured.items[i], err);
        idm_syn_free(captured.items[i]);
        captured.items[i] = NULL;
        if (!args[arg_count]) goto oom;
        arg_count++;
    }
#undef CAPTURE_FAIL
    return make_operator_call_args(ctx, op, op_token, args, arg_count, op_token->span, err);
oom:
    syntax_payload_free(&captured);
    core_args_free(args, arg_count);
    return NULL;
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
        if (op->capture && strcmp(op->capture, "postfix") == 0) {
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
        left = make_operator_call(parser->ctx, op, op_token, left, right, op_span, parser->err);
        if (!left) return NULL;
    }
    return left;
}

static IdmCore *expand_form_expr(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count < 2) return expand_error(err, syn->span, "empty %%-expr");
    return expand_parts(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IdmCore *expand_form_body(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    SurfaceCheckpoint checkpoint;
    surface_checkpoint(ctx, &checkpoint);
    IdmCore *core = expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, true, err);
    surface_rollback(ctx, &checkpoint);
    return core;
}

static IdmCore *expand_form_group(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-group expects one child");
    bool saved = ctx->value_context;
    ctx->value_context = true;
    IdmCore *core = expand_syntax(ctx, syn->as.seq.items[1], err);
    ctx->value_context = saved;
    return core;
}

static IdmCore *expand_form_expression(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
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

static IdmCore *expand_form_adjacent(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count < 3) return expand_error(err, syn->span, "%-adjacent expects at least two children");
    return expand_parts(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
}

static IdmCore *expand_form_syntax_quote(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2) return expand_error(err, syn->span, "%-syntax expects one child");
    IdmValue value = idm_syntax_value(ctx->rt, syn->as.seq.items[1], err);
    if (err && err->present) return NULL;
    return idm_core_literal(value, syn->span);
}

static IdmCore *expand_form_regex(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (syn->as.seq.count != 2 || syn->as.seq.items[1]->kind != IDM_SYN_STRING) return expand_error(err, syn->span, "%-regex expects one string child");
    const IdmSyntax *body = syn->as.seq.items[1];
    IdmError inner;
    idm_error_init(&inner);
    IdmRegex *rx = NULL;
    bool ok = idm_regex_compile(body->as.text, strlen(body->as.text), 0, &rx, &inner);
    if (!ok) {
        expand_error(err, syn->span, "%s", inner.message ? inner.message : "invalid regex literal");
        idm_error_clear(&inner);
        return NULL;
    }
    idm_error_clear(&inner);
    IdmValue value = idm_regex_value(ctx->rt, rx, err);
    if (err && err->present) return NULL;
    return idm_core_literal(value, syn->span);
}

static IdmCore *expand_form_match(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    return expand_match_form(ctx, syn, err);
}

static IdmCore *expand_program(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (!syn_is_form(syn, "%-package-begin")) return expand_error(err, syn->span, "expected %%-package-begin syntax");
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IdmSyntax *item = syn->as.seq.items[i];
        if (syn_is_form(item, "%-expr") && item->as.seq.count == 3 &&
            syn_is_word(item->as.seq.items[1], "package") && item->as.seq.items[2]->kind == IDM_SYN_WORD) {
            bool saved_in_package = ctx->in_package;
            ctx->in_package = true;
            IdmCore *core = expand_package_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, err);
            ctx->in_package = saved_in_package;
            return core;
        }
    }
    return expand_body_items(ctx, syn->as.seq.items, 1, syn->as.seq.count, true, err);
}

static IdmCore *empty_container_literal(IdmRuntime *rt, IdmPrimitive prim, IdmSpan span, IdmError *err) {
    IdmValue value = idm_nil();
    switch (prim) {
        case IDM_PRIM_LIST:
            value = idm_empty_list();
            break;
        case IDM_PRIM_TUPLE:
            value = idm_tuple(rt, NULL, 0, err);
            break;
        case IDM_PRIM_VECTOR:
            value = idm_vector(rt, NULL, 0, err);
            break;
        case IDM_PRIM_DICT:
            value = idm_dict(rt, NULL, 0, err);
            break;
        default:
            return NULL;
    }
    if (err && err->present) return NULL;
    IdmCore *literal = idm_core_literal(value, span);
    if (!literal) idm_error_oom(err, span);
    return literal;
}

bool dict_rest_index(const IdmSyntax *syn, size_t *out_index, IdmError *err) {
    *out_index = SIZE_MAX;
    if (!syn || syn->kind != IDM_SYN_DICT) return true;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        if (!syn_is_word(syn->as.seq.items[i], ".")) continue;
        if (syn->as.seq.items[i]->token_adjacent_previous) continue;
        if ((i % 2u) != 0) return idm_error_set(err, syn->as.seq.items[i]->span, "dict rest marker must follow key/value pairs");
        *out_index = i;
        return true;
    }
    return true;
}

static IdmCore *dict_put_core(IdmCore *dict, IdmCore *key, IdmCore *value, IdmSpan span, IdmError *err) {
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_DICT_PUT, span, err);
    if (!call) {
        idm_core_free(dict);
        idm_core_free(key);
        idm_core_free(value);
        return NULL;
    }
    if (!core_call_add_arg_or_free(call, dict, err, span)) {
        idm_core_free(key);
        idm_core_free(value);
        return NULL;
    }
    if (!core_call_add_arg_or_free(call, key, err, span)) {
        idm_core_free(value);
        return NULL;
    }
    if (!core_call_add_arg_or_free(call, value, err, span)) return NULL;
    return call;
}

static size_t container_layout_end(const IdmSyntax *const *items, size_t pos, size_t end) {
    if (pos + 1u >= end) return pos + 1u;
    if (syntax_forbids_call(items[pos])) return pos + 1u;
    IdmSpan head = items[pos]->span;
    IdmSpan next = items[pos + 1u]->span;
    if (head.line == 0 || head.column == 0 || next.line <= head.line || next.column <= head.column) return pos + 1u;
    size_t cursor = pos + 1u;
    while (cursor < end && items[cursor]->span.line > head.line && items[cursor]->span.column > head.column) cursor++;
    return cursor;
}

static IdmCore *parse_container_expr(ExpandContext *ctx, const IdmSyntax *syn, size_t *pos, size_t end, IdmError *err) {
    size_t group_end = container_layout_end((const IdmSyntax *const *)syn->as.seq.items, *pos, end);
    if (group_end > *pos + 1u) {
        IdmCore *core = expand_parts(ctx, syn->as.seq.items, *pos, group_end, err);
        if (core) *pos = group_end;
        return core;
    }
    return parse_postfix_expr(ctx, syn->as.seq.items, pos, end, false, err);
}

static IdmCore *expand_dict_tail_container(ExpandContext *ctx, const IdmSyntax *syn, size_t tail_index, IdmError *err) {
    if (tail_index + 1u >= syn->as.seq.count) return expand_error(err, syn->span, "dict rest marker requires a tail expression");
    IdmCore *dict = expand_parts(ctx, syn->as.seq.items, tail_index + 1u, syn->as.seq.count, err);
    if (!dict) return NULL;
    size_t pos = 0;
    while (pos < tail_index) {
        IdmCore *key = parse_container_expr(ctx, syn, &pos, tail_index, err);
        if (!key) {
            idm_core_free(dict);
            return NULL;
        }
        if (pos >= tail_index) {
            idm_core_free(dict);
            idm_core_free(key);
            return expand_error(err, syn->span, "dict literal requires key/value pairs before rest marker");
        }
        IdmCore *value = parse_container_expr(ctx, syn, &pos, tail_index, err);
        if (!value) {
            idm_core_free(dict);
            idm_core_free(key);
            return NULL;
        }
        dict = dict_put_core(dict, key, value, syn->span, err);
        if (!dict) return NULL;
    }
    return dict;
}

static IdmCore *expand_container(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmPrimitive prim;
    switch (syn->kind) {
        case IDM_SYN_LIST: prim = IDM_PRIM_LIST; break;
        case IDM_SYN_VECTOR: prim = IDM_PRIM_VECTOR; break;
        case IDM_SYN_TUPLE: prim = IDM_PRIM_TUPLE; break;
        case IDM_SYN_DICT:
            prim = IDM_PRIM_DICT;
            break;
        default:
            return expand_error(err, syn->span, "unsupported container syntax");
    }
    if (syn->as.seq.count == 0) return empty_container_literal(ctx->rt, prim, syn->span, err);
    if (syn->kind == IDM_SYN_DICT) {
        size_t tail_index = SIZE_MAX;
        if (!dict_rest_index(syn, &tail_index, err)) return NULL;
        if (tail_index != SIZE_MAX) return expand_dict_tail_container(ctx, syn, tail_index, err);
    }
    IdmCore *call = expand_primitive_clause_call(prim, syn->span, err);
    if (!call) return NULL;
    size_t pos = 0;
    size_t elem_count = 0;
    while (pos < syn->as.seq.count) {
        IdmCore *elem = parse_container_expr(ctx, syn, &pos, syn->as.seq.count, err);
        if (!elem) { idm_core_free(call); return NULL; }
        if (!core_call_add_arg_or_free(call, elem, err, syn->span)) return NULL;
        elem_count++;
    }
    if (syn->kind == IDM_SYN_DICT && elem_count % 2u != 0) {
        idm_core_free(call);
        return expand_error(err, syn->span, "dict literal requires key/value pairs");
    }
    return call;
}

IdmCore *expand_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmCore *lit = literal_from_syntax(ctx, syn, err);
    if (lit || (err && err->present)) return lit;

    if (syn->kind == IDM_SYN_WORD) return expand_word_ref(ctx, syn, err);
    if (syn_is_form(syn, "%-expr")) return expand_form_expr(ctx, syn, err);
    if (syn_is_form(syn, "%-body")) return expand_form_body(ctx, syn, err);
    if (syn_is_form(syn, "%-group") || syn_is_form(syn, "%-layout-group")) return expand_form_group(ctx, syn, err);
    if (syn_is_form(syn, "%-expression")) return expand_form_expression(ctx, syn, err);
    if (syn_is_form(syn, "%-adjacent")) return expand_form_adjacent(ctx, syn, err);
    if (syn_is_form(syn, "%-syntax")) return expand_form_syntax_quote(ctx, syn, err);
    if (syn_is_form(syn, "%-quasisyntax")) return expand_form_quasisyntax(ctx, syn, err);
    if (syn_is_form(syn, "%-quote")) return expand_form_quote(ctx, syn, err);
    if (syn_is_form(syn, "%-quasiquote")) return expand_form_quasiquote(ctx, syn, err);
    if (syn_is_form(syn, "%-string")) return expand_form_string(ctx, syn, err);
    if (syn_is_form(syn, "%-regex")) return expand_form_regex(ctx, syn, err);
    if (syn_is_form(syn, "%-match")) return expand_form_match(ctx, syn, err);
    if (syn_is_form(syn, "%-package-begin")) return expand_program(ctx, syn, err);
    if (syn_is_form(syn, "%-word")) {
        return expand_error(err, syn->span, "word syntax requires core syntax");
    }
    if (syn_is_form(syn, "%-pin")) {
        return expand_error(err, syn->span, "pin '^name' is only valid in pattern position");
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
    IdmBytecodeModule boot;
    IdmScheduler *sched;
    pthread_mutex_t mu;
    uint64_t next_token;
    uint64_t token;
    SurfaceCheckpoint checkpoint;
    uint32_t cp_env_slot_seq;
    uint32_t cp_next_slot;
    uint64_t session_pid;
    SavedHooks saved_hooks;
    bool hooks_saved;
};

static void repl_install_hooks(IdmRepl *repl) {
    if (!repl->hooks_saved) {
        hooks_install(repl->rt, &repl->ctx, &repl->saved_hooks);
        repl->hooks_saved = true;
        return;
    }
    hooks_install(repl->rt, &repl->ctx, NULL);
}

static void repl_restore_hooks(IdmRepl *repl) {
    if (!repl || !repl->hooks_saved) return;
    hooks_restore(repl->rt, &repl->saved_hooks);
    repl->hooks_saved = false;
}

IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err) {
    IdmRepl *repl = calloc(1u, sizeof(*repl));
    if (!repl) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    repl->rt = rt;
    ctx_init(&repl->ctx, rt);
    idm_bc_init(&repl->boot);
    pthread_mutex_init(&repl->mu, NULL);
    unsigned char session_hash[32];
    idm_sha256("<repl>", 6u, session_hash);
    ctx_set_unit(&repl->ctx, "<repl>", session_hash);
    repl->ctx.repl_env_binds = true;
    repl->ctx.phase_env = idm_phase_env_create(rt, rt->main_env);
    if (!repl->ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        idm_repl_destroy(repl);
        return NULL;
    }
    repl->ctx.runner = &repl->ctx.local_runner;
    repl->session_scope = idm_scope_fresh(&repl->ctx.scope_store);
    repl_install_hooks(repl);
    if (!ctx_seed(&repl->ctx, err) || !ctx_activate_kernel(&repl->ctx, err)) {
        idm_repl_destroy(repl);
        return NULL;
    }
    repl->sched = idm_sched_create(rt, NULL, err);
    if (!repl->sched) {
        idm_repl_destroy(repl);
        return NULL;
    }
    if (rt->interactive && !idm_signals_install(err)) {
        idm_repl_destroy(repl);
        return NULL;
    }
    rt->repl = repl;
    return repl;
}

static bool repl_track_module(IdmRepl *repl, IdmBytecodeModule *module) {
    if (repl->module_count == repl->module_cap) {
        if (!idm_grow((void **)&repl->modules, &repl->module_cap, sizeof(*repl->modules), 8u, repl->module_count + 1u)) return false;
    }
    repl->modules[repl->module_count++] = module;
    return true;
}

static bool repl_make_thunk(IdmRuntime *rt, IdmBytecodeModule *module, uint32_t main_fn, IdmEnv *env, IdmValue *out, IdmError *err) {
    if (main_fn >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "REPL main function index out of bounds");
    *out = idm_closure_in_module(rt, module, main_fn, NULL, 0, env, err);
    return !(err && err->present);
}

static IdmReplStatus repl_compile_fail(IdmRepl *repl, IdmError *err) {
    if (err && !err->present) idm_error_oom(err, idm_span_unknown(NULL));
    surface_rollback(&repl->ctx, &repl->checkpoint);
    repl->ctx.scope_store.next_scope = repl->checkpoint.next_scope;
    repl->ctx.env_slot_seq = repl->cp_env_slot_seq;
    repl->ctx.next_slot = repl->cp_next_slot;
    pthread_mutex_unlock(&repl->mu);
    return IDM_REPL_ERROR;
}

static IdmReplStatus repl_compile_incomplete(IdmRepl *repl, IdmError *err) {
    idm_error_clear(err);
    surface_rollback(&repl->ctx, &repl->checkpoint);
    repl->ctx.scope_store.next_scope = repl->checkpoint.next_scope;
    repl->ctx.env_slot_seq = repl->cp_env_slot_seq;
    repl->ctx.next_slot = repl->cp_next_slot;
    pthread_mutex_unlock(&repl->mu);
    return IDM_REPL_INCOMPLETE;
}

static const char *repl_next_token(const char *p, char *token, size_t cap) {
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p != '"') break;
        p++;
        while (*p) {
            if (*p == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (*p++ == '"') break;
        }
    }
    if (!*p) return NULL;
    size_t len = 0;
    if (p[0] == '-' && p[1] == '>') {
        len = 2;
    } else if (isalpha((unsigned char)*p) || *p == '_') {
        const char *start = p;
        p++;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '?' || *p == '!' || *p == '/' || *p == '<' || *p == '>' || *p == '-') p++;
        if (*p == '=' && p[1] == '?') p += 2;
        len = (size_t)(p - start);
        p = start;
    } else {
        len = 1;
    }
    size_t copy = len + 1u < cap ? len : cap - 1u;
    memcpy(token, p, copy);
    token[copy] = '\0';
    return p + len;
}

static bool repl_source_ends_with_token(const char *source, const char *want) {
    char token[64];
    char last[64] = "";
    const char *p = source;
    while ((p = repl_next_token(p, token, sizeof(token))) != NULL) {
        memcpy(last, token, strlen(token) + 1u);
    }
    return strcmp(last, want) == 0;
}

static bool repl_source_has_unclosed_do(const char *source) {
    int depth = 0;
    char token[64];
    const char *p = source;
    while ((p = repl_next_token(p, token, sizeof(token))) != NULL) {
        if (strcmp(token, "do") == 0) depth++;
        else if (strcmp(token, "end") == 0 || strcmp(token, "else") == 0) {
            if (depth > 0) depth--;
        }
    }
    return depth > 0;
}

static bool repl_error_is_incomplete(const IdmError *err, const char *source) {
    if (!err || !err->present || !err->message) return false;
    if (strcmp(err->message, "unterminated command") == 0) return true;
    bool open_do = repl_source_has_unclosed_do(source);
    bool ends_arrow = repl_source_ends_with_token(source, "->");
    if (open_do && (strcmp(err->message, "try requires a do/end body") == 0 ||
                    strcmp(err->message, "receive requires a final do/end clause body") == 0 ||
                    strcmp(err->message, "function clause requires -> or do/end body") == 0 ||
                    strstr(err->message, "expects an indented do/end body") != NULL)) {
        return true;
    }
    return ends_arrow && (strcmp(err->message, "function clause arrow requires a body") == 0 ||
                          strcmp(err->message, "rescue requires '-> HANDLER'") == 0);
}

static char *repl_flatten_lines(const char *source) {
    if (!strchr(source, '\n')) return NULL;
    char *flat = idm_strdup(source);
    if (!flat) return NULL;
    for (char *p = flat; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
    return flat;
}

static IdmReplStatus repl_compile_source(IdmRepl *repl, const char *file, const char *source, bool make_thunk, IdmValue *out_thunk, uint64_t *out_token, IdmError *err) {
    if (out_thunk) *out_thunk = idm_nil();
    if (out_token) *out_token = 0;
    IdmSyntax *program = NULL;
    if (!idm_expand_read_source_string(repl->rt, file, source, &program, err)) {
        if (err->message && strstr(err->message, "unterminated")) {
            idm_error_clear(err);
            return IDM_REPL_INCOMPLETE;
        }
        return IDM_REPL_ERROR;
    }
    pthread_mutex_lock(&repl->mu);
    repl_install_hooks(repl);
    if (!idm_syn_scope_add_tree(program, 0, repl->session_scope)) {
        idm_syn_free(program);
        idm_error_oom(err, idm_span_unknown(NULL));
        pthread_mutex_unlock(&repl->mu);
        return IDM_REPL_ERROR;
    }
    surface_checkpoint(&repl->ctx, &repl->checkpoint);
    repl->cp_env_slot_seq = repl->ctx.env_slot_seq;
    repl->cp_next_slot = repl->ctx.next_slot;
    repl->token = 0;
    IdmCore *core = program->as.seq.count < 2
        ? idm_core_literal(idm_nil(), program->span)
        : expand_body_items(&repl->ctx, program->as.seq.items, 1, program->as.seq.count, false, err);
    idm_syn_free(program);
    if (!core && repl_error_is_incomplete(err, source)) return repl_compile_incomplete(repl, err);
    if (!core) return repl_compile_fail(repl, err);
    if (!ctx_validate_source_reader_decls(&repl->ctx, err)) {
        idm_core_free(core);
        return repl_compile_fail(repl, err);
    }
    if (!make_thunk) {
        idm_core_free(core);
        repl->token = 0;
        pthread_mutex_unlock(&repl->mu);
        return IDM_REPL_OK;
    }
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_core_free(core);
        return repl_compile_fail(repl, err);
    }
    idm_bc_init(module);
    uint32_t main_fn = 0;
    bool compiled = idm_core_compile_main(repl->rt, core, module, &main_fn, err);
    idm_core_free(core);
    if (!compiled) {
        idm_bc_destroy(module);
        free(module);
        return repl_compile_fail(repl, err);
    }
    if (!repl_track_module(repl, module)) {
        idm_bc_destroy(module);
        free(module);
        return repl_compile_fail(repl, err);
    }
    if (!idm_bc_intern_literals(repl->rt, module, err)) return repl_compile_fail(repl, err);
    IdmValue thunk = idm_nil();
    if (!repl_make_thunk(repl->rt, module, main_fn, repl->rt->main_env, &thunk, err)) return repl_compile_fail(repl, err);
    if (out_token) {
        repl->token = ++repl->next_token;
        *out_token = repl->token;
    } else {
        repl->token = 0;
    }
    if (out_thunk) *out_thunk = thunk;
    pthread_mutex_unlock(&repl->mu);
    return IDM_REPL_OK;
}

IdmReplStatus idm_repl_compile(IdmRepl *repl, const char *source, IdmValue *out_thunk, uint64_t *out_token, IdmError *err) {
    IdmReplStatus status = repl_compile_source(repl, "<repl>", source, true, out_thunk, out_token, err);
    if (status != IDM_REPL_INCOMPLETE) return status;
    char *flat = repl_flatten_lines(source);
    if (!flat) return status;
    status = repl_compile_source(repl, "<repl>", flat, true, out_thunk, out_token, err);
    free(flat);
    return status;
}

void idm_repl_abort(IdmRepl *repl, uint64_t token) {
    pthread_mutex_lock(&repl->mu);
    if (token != 0 && token == repl->token) {
        surface_rollback(&repl->ctx, &repl->checkpoint);
        repl->ctx.scope_store.next_scope = repl->checkpoint.next_scope;
        repl->ctx.env_slot_seq = repl->cp_env_slot_seq;
        repl->ctx.next_slot = repl->cp_next_slot;
        repl->token = 0;
    }
    pthread_mutex_unlock(&repl->mu);
}

bool idm_repl_run(IdmRepl *repl, IdmValue thunk, IdmValue *out_value, IdmError *err) {
    return idm_sched_eval(repl->sched, thunk, out_value, err);
}

IdmScheduler *idm_repl_scheduler(IdmRepl *repl) {
    return repl->sched;
}

uint64_t idm_repl_session_pid(const IdmRepl *repl) {
    return repl->session_pid;
}

void idm_repl_set_session_pid(IdmRepl *repl, uint64_t pid) {
    repl->session_pid = pid;
}

bool idm_repl_loop_thunk(IdmRepl *repl, const char *source, IdmValue *out_thunk, IdmError *err) {
    IdmReplStatus status = repl_compile_source(repl, "<repl>", source, true, out_thunk, NULL, err);
    if (status == IDM_REPL_INCOMPLETE) return idm_error_set(err, idm_span_unknown("<repl>"), "incomplete REPL startup source");
    return status == IDM_REPL_OK;
}

bool idm_repl_seed_source(IdmRepl *repl, const char *source, IdmError *err) {
    IdmReplStatus status = repl_compile_source(repl, "<repl-seed>", source, false, NULL, NULL, err);
    if (status == IDM_REPL_INCOMPLETE) return idm_error_set(err, idm_span_unknown("<repl-seed>"), "incomplete REPL seed source");
    return status == IDM_REPL_OK;
}

void idm_repl_destroy(IdmRepl *repl) {
    if (!repl) return;
    if (repl->rt && repl->rt->repl == repl) repl->rt->repl = NULL;
    repl_restore_hooks(repl);
    idm_sched_destroy(repl->sched);
    for (size_t i = 0; i < repl->module_count; i++) {
        idm_bc_destroy(repl->modules[i]);
        free(repl->modules[i]);
    }
    free(repl->modules);
    idm_bc_destroy(&repl->boot);
    pthread_mutex_destroy(&repl->mu);
    ctx_destroy(&repl->ctx);
    free(repl);
}

bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.syntax");
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    IdmBuffer ser;
    idm_buf_init(&ser);
    if (!idm_syn_serialize(&ser, syntax, err)) {
        idm_buf_destroy(&ser);
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    unsigned char unit_hash[32];
    idm_sha256(ser.data ? ser.data : "", ser.len, unit_hash);
    idm_buf_destroy(&ser);
    ctx_set_unit(&ctx, syntax->span.file ? syntax->span.file : "<program>", unit_hash);
    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(rt, err);
    if (!phase_runtime_env) {
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, phase_runtime_env);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    ctx.runner = runner ? runner : &ctx.local_runner;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    IdmCore *core = NULL;
    if (ctx_seed(&ctx, err) && ctx_activate_kernel(&ctx, err)) {
        core = expand_syntax(&ctx, syntax, err);
        if (core && !ctx_validate_source_reader_decls(&ctx, err)) {
            idm_core_free(core);
            core = NULL;
        }
        if (core && !(err && err->present)) core = wrap_kernel_use(&ctx, core, err);
        if (core && !(err && err->present) && !idm_core_normalize(rt, &core, err)) {
            idm_core_free(core);
            core = NULL;
        }
    }
    hooks_restore(rt, &saved);
    ctx_destroy(&ctx);
    if (!core || (err && err->present)) {
        idm_core_free(core);
        idm_profile_scope_end(&prof);
        return false;
    }
    *out = core;
    idm_profile_scope_end(&prof);
    return true;
}

bool idm_expand_source_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err) {
    return idm_expand_source_string_with_runner(rt, file, source, NULL, out, err);
}

bool idm_expand_reader_artifact_string(IdmRuntime *rt, const IdmReaderArtifact *artifact, const char *file, const char *source, IdmCore **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.reader_artifact_string");
    IdmSyntax *syntax = NULL;
    if (!idm_reader_read_artifact_string(artifact, file, source, &syntax, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    bool ok = idm_expand_syntax(rt, syntax, out, err);
    idm_syn_free(syntax);
    idm_profile_scope_end(&prof);
    return ok;
}

bool idm_expand_source_string_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.source_string");
    IdmSyntax *syntax = NULL;
    if (!idm_expand_read_source_string(rt, file, source, &syntax, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    bool ok = idm_expand_syntax_with_runner(rt, syntax, runner, out, err);
    idm_syn_free(syntax);
    idm_profile_scope_end(&prof);
    return ok;
}

static bool append_surface_dump_line(ExpandContext *ctx, const char *kind, IdmBuffer *out, IdmError *err) {
    IdmValue surface = idm_nil();
    if (!expand_surface_value(ctx, ctx->rt, kind, &surface, err)) return false;
    IdmValue items[2];
    items[0] = idm_atom(ctx->rt, kind);
    items[1] = surface;
    IdmValue line = idm_tuple(ctx->rt, items, 2u, err);
    if (err && err->present) return false;
    if (!idm_value_write(out, line) || !idm_buf_append_char(out, '\n')) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

bool idm_expand_surface_dump(IdmRuntime *rt, const char *prelude, IdmBuffer *out, IdmError *err) {
    IdmBuffer source;
    idm_buf_init(&source);
    bool source_ok = true;
    if (prelude && *prelude) {
        source_ok = idm_buf_append(&source, prelude) && idm_buf_append_char(&source, '\n');
    }
    source_ok = source_ok && idm_buf_append(&source, ":ok\n");
    if (!source_ok) {
        idm_buf_destroy(&source);
        return idm_error_oom(err, idm_span_unknown("<dump-surface>"));
    }
    IdmSyntax *syntax = NULL;
    if (!idm_expand_read_source_string(rt, "<dump-surface>", source.data ? source.data : "", &syntax, err)) {
        idm_buf_destroy(&source);
        return false;
    }
    idm_buf_destroy(&source);
    ExpandContext ctx;
    ctx_init(&ctx, rt);
    IdmBuffer ser;
    idm_buf_init(&ser);
    if (!idm_syn_serialize(&ser, syntax, err)) {
        idm_buf_destroy(&ser);
        idm_syn_free(syntax);
        ctx_destroy(&ctx);
        return false;
    }
    unsigned char unit_hash[32];
    idm_sha256(ser.data ? ser.data : "", ser.len, unit_hash);
    idm_buf_destroy(&ser);
    ctx_set_unit(&ctx, "<dump-surface>", unit_hash);
    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(rt, err);
    if (!phase_runtime_env) {
        idm_syn_free(syntax);
        ctx_destroy(&ctx);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, phase_runtime_env);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        idm_syn_free(syntax);
        ctx_destroy(&ctx);
        return false;
    }
    ctx.runner = &ctx.local_runner;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    IdmCore *core = NULL;
    bool ok = ctx_seed(&ctx, err) && ctx_activate_kernel(&ctx, err);
    if (ok) {
        core = expand_syntax(&ctx, syntax, err);
        ok = core && !(err && err->present);
        if (ok) ok = ctx_validate_source_reader_decls(&ctx, err);
    }
    if (ok) {
        ok = append_surface_dump_line(&ctx, "operators", out, err) &&
             append_surface_dump_line(&ctx, "macros", out, err) &&
             append_surface_dump_line(&ctx, "protocols", out, err) &&
             append_surface_dump_line(&ctx, "core-syntax", out, err) &&
             append_surface_dump_line(&ctx, "grammar-artifacts", out, err) &&
             append_surface_dump_line(&ctx, "methods", out, err) &&
             append_surface_dump_line(&ctx, "active", out, err);
    }
    hooks_restore(rt, &saved);
    idm_core_free(core);
    idm_syn_free(syntax);
    ctx_destroy(&ctx);
    return ok;
}
