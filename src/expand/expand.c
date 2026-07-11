#include "internal.h"

static bool idm_expand_source_string_with_runner(IdmRuntime *rt, const char *label, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err);
static bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err);

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
static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static IdmCore *expand_implements_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static IdmCore *expand_protocol_info_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err);
static IdmCore *rewrite_is_a_call(ExpandContext *ctx, IdmCore *call, const IdmScopeSet *scopes, IdmError *err);
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
static IdmCore *rewrite_help_call(ExpandContext *ctx, IdmCore *call, IdmError *err);
static IdmCore *application_call_from_receiver(ExpandContext *ctx, IdmCore *callee, IdmCore *receiver, IdmSyntax *const *items, size_t start, size_t *pos, size_t end, bool stop_at_operator, bool known, IdmArity arity, IdmError *err);
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
static void record_operator_edge(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token);
static IdmCore *make_operator_call(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *left, IdmCore *right, IdmSpan span, IdmError *err);
static IdmCore *make_operator_unary(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore *operand, IdmSpan span, IdmError *err);
static IdmCore *make_operator_call_args(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err);
static IdmCore *parse_enforest_expr(EnforestParser *parser, uint16_t min_prec);
static IdmCore *expand_form_regex(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_form_match(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *expand_container(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err);
static IdmCore *empty_container_literal(IdmRuntime *rt, IdmPrimitive prim, IdmSpan span, IdmError *err);
static bool word_has_subtraction_shape(const char *text);
static bool syntax_is_literal_value(const IdmSyntax *syn);
static bool syntax_is_negative_number(const IdmSyntax *syn);
static bool adjacent_items_present(IdmSyntax *const *items, size_t start, size_t end);
static bool flatten_adjacent_items(IdmSyntax *const *items, size_t start, size_t end, IdmSyntax ***out_items, size_t *out_count);

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

static IdmCore *attach_ref_contract(IdmCore *ref, const IdmCallableContract *contract, IdmSpan span, IdmError *err) {
    if (!ref) return NULL;
    if (contract && !idm_core_ref_set_contract(ref, contract)) {
        idm_core_free(ref);
        idm_error_oom(err, span);
        return NULL;
    }
    return ref;
}

static IdmCore *zero_arity_binding_ref_if_known(IdmCore *ref, const IdmBinding *binding, bool callee_position, IdmSpan span, IdmError *err) {
    ref = attach_ref_contract(ref, binding && binding->has_contract ? &binding->contract : NULL, span, err);
    return ref ? zero_arity_call_if_known(ref, binding ? &binding->arity : NULL, callee_position, span, err) : NULL;
}

static IdmCore *zero_arity_capture_ref_if_known(IdmCore *ref, const CaptureBinding *capture, bool callee_position, IdmSpan span, IdmError *err) {
    ref = attach_ref_contract(ref, capture && capture->has_contract ? &capture->contract : NULL, span, err);
    return ref ? zero_arity_call_if_known(ref, capture ? &capture->arity : NULL, callee_position, span, err) : NULL;
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
    const IdmBinding *found = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, word->as.text, IDM_BIND_SPACE_METHOD, idm_syn_scope_set(word, 0), NULL, &found);
    if (status != IDM_RESOLVE_OK || !found) {
        if (out_status) *out_status = status;
        return true;
    }
    const IdmScopeSet *best = &found->scopes;
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
    const IdmBinding *found = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, word->as.text, IDM_BIND_SPACE_FIELD, idm_syn_scope_set(word, 0), NULL, &found);
    if (status != IDM_RESOLVE_OK || !found) {
        if (out_status) *out_status = status;
        return true;
    }
    const IdmScopeSet *best = &found->scopes;
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
    if (ctx->phase == 1 && ctx->phase_demand_diag) {
        idm_error_note(err, "phase-1 compilation of '%s' failed: %s", word->as.text, ctx->phase_demand_diag);
        free(ctx->phase_demand_diag);
        ctx->phase_demand_diag = NULL;
    }
    for (size_t i = 0; i < word->origins.count; i++) {
        idm_error_note(err, "in expansion of '%s'", word->origins.items[i]);
    }
    if (word_has_subtraction_shape(word->as.text) || word_is_subtraction_of_bindings(ctx, word)) {
        idm_error_note(err, "identifier '%s' was read as one word; use spaces around '-' for subtraction", word->as.text);
    }
    int other = ctx->phase == 0 ? 1 : 0;
    const IdmScopeSet *scopes = idm_syn_scope_set(word, 0);
    if (idm_binding_resolve(&ctx->bindings, word->as.text, other, IDM_BIND_SPACE_DEFAULT, scopes ? scopes : &ctx->empty_scopes, NULL) == IDM_RESOLVE_OK) {
        if (ctx->phase == 0) idm_error_note(err, "'%s' exists only at expansion time (phase 1) and has no runtime value", word->as.text);
        else idm_error_note(err, "'%s' is bound at runtime (phase 0) and could not be compiled for expansion time; only top-level defns and package slots cross phases", word->as.text);
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

static bool binding_passthrough(const IdmBinding *binding, IdmPrimitive *out_primitive) {
    if (!binding->has_contract || !binding->contract.passthrough) return false;
    if (out_primitive) *out_primitive = (IdmPrimitive)binding->contract.primitive;
    return true;
}

static const IdmBinding *resolve_default_demand(ExpandContext *ctx, const IdmSyntax *word, IdmResolveStatus *out_status, IdmError *err) {
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default(ctx, word, &status);
    if (ctx->phase == 1 && status == IDM_RESOLVE_OK && binding->kind == IDM_BIND_ENV && binding_passthrough(binding, NULL)) {
        bool compiled = false;
        if (!phase_demand_local(ctx, word, &compiled, err)) {
            if (out_status) *out_status = IDM_RESOLVE_UNBOUND;
            return NULL;
        }
        binding = resolve_default(ctx, word, &status);
    }
    if (out_status) *out_status = status;
    return binding;
}

static IdmCore *expand_frame_binding_ref(ExpandContext *ctx, const IdmSyntax *word, const IdmBinding *binding, bool callee_position, IdmError *err) {
    uint32_t cap = 0;
    IdmCore *ref = NULL;
    idm_binding_mark_referenced(binding);
    if (binding->frame_id == ctx->frame) {
        ref = binding->kind == IDM_BIND_LOCAL ?
              idm_core_local_ref(word->as.text, binding->payload, word->span) :
              idm_core_arg_ref(word->as.text, binding->payload, word->span);
        return zero_arity_binding_ref_if_known(ref, binding, callee_position, word->span, err);
    }
    if (materialize_capture(ctx, word, binding, &cap)) {
        ref = idm_core_capture_ref(word->as.text, cap, word->span);
        return zero_arity_binding_ref_if_known(ref, binding, callee_position, word->span, err);
    }
    return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
}

static IdmCore *expand_word_ref_mode(ExpandContext *ctx, const IdmSyntax *word, bool callee_position, IdmError *err) {
    if (word->as.text[0] == '&' && word->as.text[1] != '\0') {
        IdmSyntax *stripped = idm_syn_word(word->as.text + 1, word->span);
        if (!stripped) return (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
        bool ok = true;
        for (int phase = 0; phase < 2 && ok; phase++) {
            const IdmScopeSet *scopes = idm_syn_scope_set(word, phase);
            if (scopes) for (size_t si = 0; si < scopes->count && ok; si++) ok = idm_syn_scope_add(stripped, phase, scopes->items[si]);
        }
        IdmCore *core = ok ? expand_word_ref_mode(ctx, stripped, true, err) : (IdmCore *)(uintptr_t)idm_error_oom(err, word->span);
        idm_syn_free(stripped);
        return core;
    }
    const CaptureBinding *capture = capture_lookup_existing(ctx->captures, ctx->capture_count, word);
    if (capture) {
        IdmCore *ref = idm_core_capture_ref(capture->name, capture->capture_index, word->span);
        return zero_arity_capture_ref_if_known(ref, capture, callee_position, word->span, err);
    }
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    const IdmBinding *binding = resolve_default_demand(ctx, word, &status, err);
    if (err && err->present) return NULL;
    if (status == IDM_RESOLVE_UNBOUND && ctx->phase == 1) {
        bool compiled = false;
        if (!phase_demand_local(ctx, word, &compiled, err)) return NULL;
        if (compiled) binding = resolve_default(ctx, word, &status);
    }
    if (status == IDM_RESOLVE_OK) {
        if (ctx->record_edges && binding->has_contract && binding->contract.doc) {
            expand_edge_record(ctx, "binding", word->as.text, binding->provider, binding->contract.doc, NULL, NULL, word->span);
        }
        if (binding->kind == IDM_BIND_LOCAL) {
            return expand_frame_binding_ref(ctx, word, binding, callee_position, err);
        }
        if (binding->kind == IDM_BIND_ARG) {
            return expand_frame_binding_ref(ctx, word, binding, callee_position, err);
        }
        if (binding->kind == IDM_BIND_ENV) {
            IdmPrimitive primitive = IDM_PRIM_COUNT;
            if (binding_passthrough(binding, &primitive)) {
                IdmCore *ref = idm_core_primitive_backed_fn(idm_primitive_name(primitive), primitive, binding->arity, word->span);
                return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
            }
            IdmCore *ref = idm_core_env_ref(word->as.text, binding->payload, word->span);
            return zero_arity_binding_ref_if_known(ref, binding, callee_position, word->span, err);
        }
        if (binding->kind == IDM_BIND_PACKAGE_SLOT) {
            IdmPrimitive primitive = IDM_PRIM_COUNT;
            if (binding_passthrough(binding, &primitive)) {
                IdmCore *ref = idm_core_primitive_backed_fn(idm_primitive_name(primitive), primitive, binding->arity, word->span);
                return zero_arity_call_if_known(ref, &binding->arity, callee_position, word->span, err);
            }
            const PackageSlotRef *slot_ref = package_slot_ref_get(ctx, binding->payload);
            if (!slot_ref) return (IdmCore *)(uintptr_t)idm_error_set(err, word->span, "package slot payload %u is out of bounds", binding->payload);
            IdmCore *ref = idm_core_package_ref(word->as.text, idm_atom(ctx->rt, slot_ref->env_key), slot_ref->slot, word->span);
            return zero_arity_binding_ref_if_known(ref, binding, callee_position, word->span, err);
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
    if (status == IDM_RESOLVE_UNBOUND && ctx->phase == 1) {
        IdmError demand_err;
        idm_error_init(&demand_err);
        bool compiled = false;
        if (phase_demand_local(ctx, word, &compiled, &demand_err) && compiled) {
            (void)resolve_default(ctx, word, &status);
        } else if (demand_err.present && demand_err.message) {
            free(ctx->phase_demand_diag);
            ctx->phase_demand_diag = idm_strdup(demand_err.message);
        }
        idm_error_clear(&demand_err);
    }
    return status == IDM_RESOLVE_OK;
}

static bool try_clause_arrow_index(const IdmSyntax *stmt, size_t *out) {
    if (!idm_syn_is_form_id(stmt, IDM_FORM_EXPR)) return false;
    for (size_t i = 1; i < stmt->as.seq.count; i++) {
        if (syn_is_word(stmt->as.seq.items[i], "->")) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool try_rescue_catches_all(const IdmSyntax *body) {
    if (body->as.seq.count < 2u) return false;
    const IdmSyntax *last = body->as.seq.items[body->as.seq.count - 1u];
    size_t arrow = 0;
    if (!try_clause_arrow_index(last, &arrow)) return false;
    return arrow == 2u && last->as.seq.items[1]->kind == IDM_SYN_WORD;
}

static IdmSyntax *try_reraise_clause(IdmSpan span) {
    IdmSyntax *clause = idm_syn_list(span);
    if (!clause) return NULL;
    const char *words[5] = {"%-expr", "%try-reraise", "->", "raise", "%try-reraise"};
    for (size_t i = 0; i < 5u; i++) {
        IdmSyntax *word = idm_syn_word(words[i], span);
        if (!word || !idm_syn_append(clause, word)) {
            idm_syn_free(word);
            idm_syn_free(clause);
            return NULL;
        }
    }
    return clause;
}

static IdmCore *expand_try_rescue(ExpandContext *ctx, const IdmSyntax *body, uint32_t *out_slot, IdmError *err) {
    for (size_t i = 1; i < body->as.seq.count; i++) {
        size_t arrow = 0;
        if (!try_clause_arrow_index(body->as.seq.items[i], &arrow)) {
            return expand_error(err, body->as.seq.items[i]->span, "rescue clauses take the form: PATTERN [when GUARD] -> BODY");
        }
    }
    IdmSyntax *match_form = idm_syn_list(body->span);
    IdmSyntax *head = idm_syn_word("%-match", body->span);
    IdmSyntax *scrutinee = idm_syn_word("%try-reason", body->span);
    IdmSyntax *clauses = idm_syn_clone(body);
    bool built = match_form && head && idm_syn_append(match_form, head) &&
                 scrutinee && idm_syn_append(match_form, scrutinee) &&
                 clauses != NULL;
    if (built && !try_rescue_catches_all(clauses)) {
        IdmSyntax *reraise = try_reraise_clause(body->span);
        built = reraise && idm_syn_append(clauses, reraise);
        if (!built) idm_syn_free(reraise);
    }
    if (built) built = idm_syn_append(match_form, clauses);
    if (!built) {
        if (match_form && match_form->as.seq.count < 3u) idm_syn_free(clauses);
        idm_syn_free(match_form);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
    }
    size_t saved_count = ctx->bindings.count;
    uint32_t r_slot = 0;
    if (!local_push_scoped(ctx, "%try-reason", match_form->as.seq.items[1], &r_slot)) {
        idm_syn_free(match_form);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, body->span);
    }
    IdmCore *handler = expand_match_form(ctx, match_form, err);
    local_pop_to(ctx, saved_count, ctx->next_slot);
    idm_syn_free(match_form);
    if (!handler) return NULL;
    if (out_slot) *out_slot = r_slot;
    return handler;
}

static IdmCore *expand_try_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    size_t cursor = start + 1u;
    if (cursor >= end || !idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) return expand_error(err, items[start]->span, "try requires a do/end body");
    const IdmSyntax *body_syn = items[cursor++];
    const IdmSyntax *rescue_body = NULL;
    const IdmSyntax *ensure_body = NULL;
    if (cursor < end && syn_is_word(items[cursor], "rescue")) {
        cursor++;
        if (cursor >= end || !idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) {
            return expand_error(err, items[cursor - 1u]->span, "rescue takes a do/end clause body: try do BODY rescue do PATTERN -> HANDLER ... end");
        }
        rescue_body = items[cursor++];
    }
    if (cursor < end && syn_is_word(items[cursor], "ensure")) {
        cursor++;
        if (cursor >= end || !idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) {
            return expand_error(err, items[cursor - 1u]->span, "ensure takes a do/end body: try do BODY ensure do CLEANUP ... end");
        }
        ensure_body = items[cursor++];
    }
    if (cursor != end) {
        if (syn_is_word(items[cursor], "rescue")) return expand_error(err, items[cursor]->span, "rescue must come before ensure in a try");
        return expand_error(err, items[cursor]->span, "unexpected syntax after try sections");
    }
    if (!rescue_body && !ensure_body) return expand_error(err, items[start]->span, "try requires a rescue or ensure section");

    IdmCore *body_core = expand_body_items(ctx, body_syn->as.seq.items, 1, body_syn->as.seq.count, true, err);
    if (!body_core) return NULL;

    IdmCore *handler = NULL;
    uint32_t rescue_slot = 0;
    if (rescue_body) {
        handler = expand_try_rescue(ctx, rescue_body, &rescue_slot, err);
        if (!handler) { idm_core_free(body_core); return NULL; }
    }
    IdmCore *cleanup = NULL;
    uint32_t ensure_slot = 0;
    if (ensure_body) {
        cleanup = expand_body_items(ctx, ensure_body->as.seq.items, 1, ensure_body->as.seq.count, true, err);
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
    IdmSpan call_span = items[start]->span;
    idm_syn_free(trait_name);
    IdmCore *node = idm_core_dispatch(IDM_DISPATCH_IMPLEMENTS, trait_def_identity_text(trait), trait->name, call_span);
    if (!node || !idm_core_dispatch_add_arg(node, value)) {
        idm_core_free(node);
        idm_core_free(value);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, call_span);
    }
    return node;
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
    if (idm_syn_is_form_id(items[start], IDM_FORM_ADJACENT)) {
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
    if (idm_syn_is_form_id(syn, IDM_FORM_EXPRESSION) && syn->as.seq.count == 2u) {
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
    if (idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP) && syn->as.seq.count == 2u) {
        const IdmSyntax *inner = syn->as.seq.items[1];
        if (idm_syn_is_form_id(inner, IDM_FORM_EXPR) && inner->as.seq.count >= 2u) {
            return syntax_call_arity(ctx, inner->as.seq.items[1], out, err);
        }
        return false;
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_ADJACENT)) {
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
    const IdmBinding *binding = resolve_default_demand(ctx, syn, &status, err);
    if (err && err->present) return false;
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

static const IdmCallableContract *method_surface_contract(ExpandContext *ctx, const MethodSurfaceDef *surface) {
    const char *name = method_surface_name_text(surface);
    if (!ctx || !surface->trait || !name) return NULL;
    if (ctx->trait_identity == surface->trait) {
        for (size_t i = 0; i < ctx->decl_method_count; i++) {
            const TraitMethodDef *method = &ctx->decl_methods[i];
            if (method->has_contract && method->contract.sig_count != 0 && trait_method_matches_name(ctx, method, name)) return &method->contract;
        }
    }
    const TraitDef *def = typed_trait_by_symbol(ctx, surface->trait);
    if (!def) return NULL;
    for (size_t i = 0; i < def->method_count; i++) {
        const TraitMethodDef *method = &def->methods[i];
        if (method->has_contract && method->contract.sig_count != 0 && trait_method_matches_name(ctx, method, name)) return &method->contract;
    }
    return NULL;
}

static bool method_ref_attach_contract(const IdmCallableContract *contract, IdmCore *ref, IdmSpan span, IdmError *err) {
    if (!contract) return true;
    if (idm_core_ref_set_contract(ref, contract)) return true;
    return idm_error_oom(err, span);
}

static IdmCore *method_dispatch_ref(ExpandContext *ctx, const MethodSurfaceDef *method, IdmSpan span, IdmError *err) {
    IdmCore *ref = NULL;
    if (method->dispatch_env) {
        ref = method->dispatch_env_key && method->dispatch_env_key[0]
            ? idm_core_package_ref(method_surface_name_text(method), idm_atom(ctx->rt, method->dispatch_env_key), method->dispatch_slot, span)
            : idm_core_env_ref(method_surface_name_text(method), method->dispatch_slot, span);
    } else if (method->dispatch_frame == ctx->frame) {
        ref = idm_core_local_ref(method_surface_name_text(method), method->dispatch_slot, span);
    } else {
        uint32_t capture = 0;
        if (!materialize_slot_capture(ctx, method_surface_name_text(method), &method->scopes, method->dispatch_frame, method->dispatch_slot, method->arity, &capture)) {
            idm_error_set(err, span, "method evidence '%s.%s' is not visible in this function", method_surface_trait_text(method), method_surface_name_text(method));
            return NULL;
        }
        ref = idm_core_capture_ref(method_surface_name_text(method), capture, span);
    }
    if (!ref) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    if (!method_ref_attach_contract(method_surface_contract(ctx, method), ref, span, err)) {
        idm_core_free(ref);
        return NULL;
    }
    return ref;
}

static IdmCore *ref_build_quiet(ExpandContext *ctx, IdmCore *(*build)(ExpandContext *, const MethodSurfaceDef *, const MethodImplDef *, IdmSpan, IdmError *), const MethodSurfaceDef *method, const MethodImplDef *impl, IdmSpan span, uint8_t *state, IdmError *err) {
    IdmError probe;
    idm_error_init(&probe);
    IdmCore *ref = build(ctx, method, impl, span, &probe);
    if (ref) {
        idm_error_clear(&probe);
        *state = IDM_DISPATCH_REF_OK;
        return ref;
    }
    bool oom = probe.present && probe.message && strcmp(probe.message, "out of memory") == 0;
    idm_error_clear(&probe);
    if (oom) {
        idm_error_oom(err, span);
        *state = IDM_DISPATCH_REF_NONE;
        return NULL;
    }
    *state = IDM_DISPATCH_REF_INVISIBLE;
    return NULL;
}

static IdmCore *dispatch_ref_build(ExpandContext *ctx, const MethodSurfaceDef *method, const MethodImplDef *impl, IdmSpan span, IdmError *err) {
    (void)impl;
    return method_dispatch_ref(ctx, method, span, err);
}


static IdmCore *field_dispatch_clause_body(ExpandContext *ctx, IdmCore *multi, const void *user, uint32_t argc, IdmSpan span, IdmError *err) {
    (void)multi;
    (void)argc;
    const FieldSurfaceRef *field = user;
    IdmCore *recv = idm_core_arg_ref("arg", 0u, span);
    if (!recv) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    return expand_record_field_core(ctx, recv, field->type, field->field_index, span, err);
}

IdmCore *build_field_selector_core(ExpandContext *ctx, const char *name, IdmSpan span, IdmError *err) {
    IdmCore *multi = idm_core_fn_multi(name, span);
    if (!multi) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *e = &ctx->typed.entities[i];
        if (e->kind != IDM_TYPED_ENTITY_TYPE || e->as.type.field_count == 0) continue;
        IdmSymbol *identity = e->as.type.identity;
        if (!identity) continue;
        for (size_t f = 0; f < e->as.type.field_count; f++) {
            const char *fname = type_field_name_text(&e->as.type.fields[f]);
            if (!fname || strcmp(fname, name) != 0) continue;
            FieldSurfaceRef ref = { &e->as.type, (uint32_t)f };
            if (!expand_multi_add_dispatch_clause(ctx, multi, 1u, identity, field_dispatch_clause_body, &ref, span, err)) {
                idm_core_free(multi);
                return NULL;
            }
            break;
        }
    }
    if (multi->as.fn_multi.count == 0) {
        idm_core_free(multi);
        return expand_error(err, span, "field '%s' has no visible record types", name);
    }
    return multi;
}

static IdmCore *build_field_selector_callee(ExpandContext *ctx, const char *name, IdmSpan span, uint8_t *state, IdmError *err) {
    FieldSelectorDef *sel = field_selector_lookup(ctx, name);
    if (!sel) {
        *state = IDM_DISPATCH_REF_NONE;
        return NULL;
    }
    IdmCore *callee = NULL;
    if (sel->env) {
        const char *key = sel->env_key && sel->env_key[0] ? sel->env_key : (ctx->unit_key[0] && ctx->in_package ? ctx->unit_key : NULL);
        callee = key
            ? idm_core_package_ref(name, idm_atom(ctx->rt, key), sel->slot, span)
            : idm_core_env_ref(name, sel->slot, span);
    } else {
        callee = idm_core_local_ref(name, sel->slot, span);
    }
    if (!callee) {
        *state = IDM_DISPATCH_REF_NONE;
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    *state = IDM_DISPATCH_REF_OK;
    return callee;
}

static IdmCore *expand_field_surface_call_cores(ExpandContext *ctx, const FieldSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    size_t argc = (receiver ? 1u : 0u) + arg_count;
    const char *field_name = group->name ? group->name : "<field>";
    if (argc != 1u) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return expand_error(err, span, "field '%s' expects a receiver", field_name);
    }
    if (!receiver) {
        receiver = args[0];
        args[0] = NULL;
    }
    core_args_free(args, arg_count);
    IdmCore *node = idm_core_dispatch(IDM_DISPATCH_FIELD, field_name, NULL, span);
    if (!node || !idm_core_dispatch_add_arg(node, receiver)) {
        idm_core_free(node);
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < group->count; i++) {
        const TypeDef *type = group->items[i].type;
        uint32_t index = group->items[i].field_index;
        if (!type_def_identity_symbol(type) || index >= type->field_count || !type->fields[index].name) continue;
        const IdmTypeTerm *contract = type->fields[index].has_contract ? &type->fields[index].contract : NULL;
        if (!idm_core_dispatch_add_field(node, type_def_identity_symbol(type), type->fields[index].name, index, contract)) {
            idm_core_free(node);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
    }
    uint8_t fallback_state = IDM_DISPATCH_REF_NONE;
    IdmCore *fallback = build_field_selector_callee(ctx, field_name, span, &fallback_state, err);
    if (!fallback && err->present) {
        idm_core_free(node);
        return NULL;
    }
    idm_core_dispatch_set_fallback(node, fallback, fallback_state);
    return node;
}

static IdmCore *method_impl_ref(ExpandContext *ctx, const MethodSurfaceDef *method, const MethodImplDef *impl, IdmSpan span, IdmError *err) {
    const char *name = method_surface_name_text(method);
    IdmCore *ref = NULL;
    if (impl->impl_env) {
        ref = impl->impl_env_key && impl->impl_env_key[0]
            ? idm_core_package_ref(name, idm_atom(ctx->rt, impl->impl_env_key), impl->impl_slot, span)
            : idm_core_env_ref(name, impl->impl_slot, span);
    } else if (impl->impl_frame == ctx->frame) {
        ref = idm_core_local_ref(name, impl->impl_slot, span);
    } else {
        uint32_t capture = 0;
        if (!materialize_slot_capture(ctx, name, &method->scopes, impl->impl_frame, impl->impl_slot, method->arity, &capture)) {
            idm_error_set(err, span, "method implementation '%s.%s' is not visible in this function", method_surface_trait_text(method), name);
            return NULL;
        }
        ref = idm_core_capture_ref(name, capture, span);
    }
    if (!ref) return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    const IdmCallableContract *contract = impl->has_contract ? &impl->contract : method_surface_contract(ctx, method);
    if (!method_ref_attach_contract(contract, ref, span, err)) {
        idm_core_free(ref);
        return NULL;
    }
    return ref;
}

static IdmCore *impl_ref_build(ExpandContext *ctx, const MethodSurfaceDef *method, const MethodImplDef *impl, IdmSpan span, IdmError *err) {
    return method_impl_ref(ctx, method, impl, span, err);
}

static IdmCore *expand_method_surface_call_cores(ExpandContext *ctx, const MethodSurfaceGroup *group, IdmCore *receiver, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    size_t argc = (receiver ? 1u : 0u) + arg_count;
    if (argc > UINT32_MAX || !method_group_accepts(group, (uint32_t)argc)) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return method_group_arity_error(err, span, group, argc);
    }
    size_t accepted_count = 0;
    for (size_t i = 0; i < group->count; i++) {
        if (idm_arity_accepts(&group->items[i]->arity, (uint32_t)argc)) accepted_count++;
    }
    if (accepted_count == 0) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return expand_error(err, span, "ambiguous method '%s'", method_surface_name_text(group->items[0]));
    }
    IdmCore *node = idm_core_dispatch(IDM_DISPATCH_METHOD, method_surface_name_text(group->items[0]), NULL, span);
    if (!node) {
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    if (receiver && !idm_core_dispatch_add_arg(node, receiver)) {
        idm_core_free(node);
        idm_core_free(receiver);
        core_args_free(args, arg_count);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    for (size_t i = 0; i < arg_count; i++) {
        if (!idm_core_dispatch_add_arg(node, args[i])) {
            idm_core_free(node);
            core_args_free(args + i, arg_count - i);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        args[i] = NULL;
    }
    for (size_t i = 0; i < group->count; i++) {
        const MethodSurfaceDef *method = group->items[i];
        if (!idm_arity_accepts(&method->arity, (uint32_t)argc)) continue;
        uint8_t evidence_state = IDM_DISPATCH_REF_NONE;
        IdmCore *evidence = NULL;
        if (method->has_dispatch) {
            evidence = ref_build_quiet(ctx, dispatch_ref_build, method, NULL, span, &evidence_state, err);
            if (!evidence && err->present) {
                idm_core_free(node);
                return NULL;
            }
        }
        if (!idm_core_dispatch_add_method(node, method->trait, evidence, evidence_state)) {
            idm_core_free(evidence);
            idm_core_free(node);
            return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
        }
        uint32_t method_pos = (uint32_t)(node->as.dispatch.method_count - 1u);
        for (size_t j = 0; j < ctx->typed.method_impl_count; j++) {
            const MethodImplDef *impl = &ctx->typed.method_impls[j];
            if (!method_impl_matches_identity(ctx, impl, method_surface_trait_text(method), method_surface_name_text(method), NULL)) continue;
            if (!impl->type && !impl->structural) continue;
            uint8_t ref_state = IDM_DISPATCH_REF_NONE;
            IdmCore *ref = NULL;
            bool passthrough = impl->has_contract && impl->contract.passthrough && impl->contract.primitive < IDM_PRIM_COUNT;
            if (!passthrough) {
                ref = ref_build_quiet(ctx, impl_ref_build, method, impl, span, &ref_state, err);
                if (!ref && err->present) {
                    idm_core_free(node);
                    return NULL;
                }
            }
            if (!idm_core_dispatch_add_impl(node, method_pos, impl->type, impl->structural ? &impl->structural_head : NULL, impl->arity, passthrough, passthrough ? impl->contract.primitive : 0u, ref, ref_state)) {
                idm_core_free(ref);
                idm_core_free(node);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
            }
        }
    }
    return node;
}

static bool arg_parse_at_stop(ExpandContext *ctx, IdmSyntax *const *items, size_t pos, size_t end, bool stop_at_operator) {
    return pos >= end || (stop_at_operator && op_lookup(ctx, items[pos], false) != NULL);
}

static bool adjacent_items_present(IdmSyntax *const *items, size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
        if (idm_syn_is_form_id(items[i], IDM_FORM_ADJACENT)) return true;
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
    if (idm_syn_is_form_id(item, IDM_FORM_ADJACENT) && item->as.seq.count >= 2u) {
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
            field_surface_group_destroy(&fields);
            method_surface_group_destroy(&group);
            IdmResolveStatus bind_status = IDM_RESOLVE_UNBOUND;
            (void)resolve_default(ctx, items[dot + 1u], &bind_status);
            if (bind_status != IDM_RESOLVE_OK) {
                idm_core_free(receiver);
                return expand_error(err, items[dot + 1u]->span, "unbound method '%s'", items[dot + 1u]->as.text);
            }
            IdmCore *callee = expand_word_callee(ctx, items[dot + 1u], err);
            if (!callee) {
                idm_core_free(receiver);
                return NULL;
            }
            IdmArity bind_arity = idm_arity_unknown();
            bool bind_known = syntax_call_arity(ctx, items[dot + 1u], &bind_arity, err);
            if (err && err->present) {
                idm_core_free(callee);
                idm_core_free(receiver);
                return NULL;
            }
            *pos = dot + 2u;
            return application_call_from_receiver(ctx, callee, receiver, items, dot, pos, end, stop_at_operator, bind_known, bind_arity, err);
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
    call = rewrite_is_a_call(ctx, call, idm_syn_scope_set(items[start], 0), err);
    if (!call) return NULL;
    call = rewrite_help_call(ctx, call, err);
    if (!call) return NULL;
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, call, err);
}

static bool is_a_target_push(IdmSymbol *resolved, TypeNameList *out, IdmError *err) {
    for (size_t i = 0; i < out->count; i++) {
        if (out->items[i] == resolved) return true;
    }
    if (out->count == out->cap && !idm_grow((void **)&out->items, &out->cap, sizeof(*out->items), 8u, out->count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    out->items[out->count++] = resolved;
    return true;
}

bool is_a_target_symbols(ExpandContext *ctx, IdmSymbol *symbol, TypeNameList *out, IdmError *err) {
    if (!symbol) return false;
    const TypeDef *td = NULL;
    for (size_t i = 0; i < ctx->typed.entity_count; i++) {
        const TypedEntity *entity = &ctx->typed.entities[i];
        if (entity->kind == IDM_TYPED_ENTITY_TYPE && entity->as.type.identity == symbol) {
            td = &entity->as.type;
            break;
        }
    }
    if (!is_a_target_push(symbol, out, err)) return false;
    if (td && td->member_count != 0) {
        for (size_t i = 0; i < td->member_count; i++) {
            const IdmTypeTerm *m = &td->members[i].term;
            if (m->kind != IDM_TYPE_CON || !m->symbol) return false;
            if (!is_a_target_symbols(ctx, m->symbol, out, err)) return false;
        }
    }
    return true;
}

static IdmCore *rewrite_is_a_call(ExpandContext *ctx, IdmCore *call, const IdmScopeSet *scopes, IdmError *err) {
    if (!call || call->kind != IDM_CORE_CALL || call->as.call.arg_count != 2u) return call;
    const IdmCore *callee = call->as.call.callee;
    if (!callee || callee->kind != IDM_CORE_FN_MULTI || callee->as.fn_multi.count != 1u) return call;
    if (!callee->as.fn_multi.clauses[0].primitive_backed || callee->as.fn_multi.clauses[0].primitive != IDM_PRIM_IS_A_P) return call;
    const IdmCore *targ = call->as.call.args[1];
    if (!targ || targ->kind != IDM_CORE_LITERAL) return call;
    const char *name = NULL;
    IdmValue tv = targ->as.literal;
    if (idm_value_tag(tv) == IDM_VAL_ATOM || idm_value_tag(tv) == IDM_VAL_WORD) name = idm_symbol_text(idm_value_symbol(tv));
    else if (idm_value_tag(tv) == IDM_VAL_STRING) name = idm_string_bytes(tv);
    if (!name || !name[0]) return call;
    const IdmBinding *binding = NULL;
    if (resolve_scoped(ctx, name, IDM_BIND_SPACE_TYPE, scopes, NULL, &binding) == IDM_RESOLVE_OK && binding && binding->kind == IDM_BIND_TYPE) {
        const TypeDef *td = typed_type_by_index(ctx, binding->payload);
        IdmSymbol *identity = td ? td->identity : NULL;
        if (identity) {
            IdmCore *lit = idm_core_literal(idm_atom_symbol(identity), targ->span);
            if (!lit) {
                idm_core_free(call);
                return (IdmCore *)(uintptr_t)idm_error_oom(err, call->span);
            }
            idm_core_free(call->as.call.args[1]);
            call->as.call.args[1] = lit;
        }
    }
    return call;
}


static IdmCore *rewrite_help_call(ExpandContext *ctx, IdmCore *call, IdmError *err) {
    (void)ctx;
    if (!expand_core_is_primitive_call(call, IDM_PRIM_HELP) || call->as.call.arg_count != 1u) return call;
    const IdmCore *arg = call->as.call.args[0];
    const IdmCallableContract *contract = NULL;
    switch (arg->kind) {
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
            if (arg->as.slot_ref.has_contract) contract = &arg->as.slot_ref.contract;
            break;
        case IDM_CORE_PACKAGE_REF:
            if (arg->as.package_ref.has_contract) contract = &arg->as.package_ref.contract;
            break;
        default:
            return call;
    }
    if (!contract || !contract->doc) return call;
    IdmSpan span = call->span;
    IdmValue v = idm_string(ctx->rt, contract->doc, err);
    if (err && err->present) {
        idm_core_free(call);
        return NULL;
    }
    IdmCore *lit = idm_core_literal(v, span);
    if (!lit) {
        idm_core_free(call);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, span);
    }
    idm_core_free(call);
    return lit;
}

static IdmCore *application_call_from_receiver(ExpandContext *ctx, IdmCore *callee, IdmCore *receiver, IdmSyntax *const *items, size_t start, size_t *pos, size_t end, bool stop_at_operator, bool known, IdmArity arity, IdmError *err) {
    IdmCore *call = idm_core_call(callee, items[start]->span);
    if (!call) {
        idm_core_free(callee);
        idm_core_free(receiver);
        return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
    }
    if (receiver && !core_call_add_arg_or_free(call, receiver, err, items[start]->span)) return NULL;
    uint32_t available = application_available_args(ctx, items, *pos, end, stop_at_operator);
    uint32_t max = UINT32_MAX;
    if (known) {
        uint32_t supplied = receiver ? 1u : 0u;
        uint32_t target = application_target_argc(&arity, available + supplied);
        max = target > supplied ? target - supplied : 0u;
    }
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
    call = rewrite_is_a_call(ctx, call, idm_syn_scope_set(items[start], 0), err);
    if (!call) return NULL;
    call = rewrite_help_call(ctx, call, err);
    if (!call) return NULL;
    return parse_dot_tail(ctx, items, pos, end, stop_at_operator, call, err);
}

static IdmCore *application_call_from(ExpandContext *ctx, IdmCore *callee, IdmSyntax *const *items, size_t start, size_t *pos, size_t end, bool stop_at_operator, bool known, IdmArity arity, IdmError *err) {
    return application_call_from_receiver(ctx, callee, NULL, items, start, pos, end, stop_at_operator, known, arity, err);
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

static bool layout_group_operator_items(ExpandContext *ctx, const IdmSyntax *syn, IdmSyntax ***out_items, size_t *out_count) {
    if (!idm_syn_is_form_id(syn, IDM_FORM_LAYOUT_GROUP) || syn->as.seq.count != 2u) return false;
    const IdmSyntax *inner = syn->as.seq.items[1];
    if (!idm_syn_is_form_id(inner, IDM_FORM_EXPR) || inner->as.seq.count < 2u) return false;
    const IdmSyntax *head = inner->as.seq.items[1];
    if (head->kind != IDM_SYN_WORD) return false;
    if (!op_lookup(ctx, head, false)) {
        IdmError probe;
        idm_error_init(&probe);
        const IdmOperatorDef *cap = op_lookup_syntax_capture(ctx, head, true, &probe);
        idm_error_clear(&probe);
        if (!cap) return false;
    }
    *out_items = inner->as.seq.items;
    *out_count = inner->as.seq.count;
    return true;
}

IdmCore *expand_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (start >= end) return expand_error(err, idm_span_unknown(NULL), "empty expression");
    bool spliced = false;
    size_t total = 0;
    for (size_t j = start; j < end; j++) {
        IdmSyntax **inner_items = NULL;
        size_t inner_count = 0;
        if (layout_group_operator_items(ctx, items[j], &inner_items, &inner_count)) {
            spliced = true;
            total += inner_count - 1u;
        } else {
            total++;
        }
    }
    if (spliced) {
        IdmSyntax **flat = malloc(total * sizeof(*flat));
        if (!flat) return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span);
        size_t at = 0;
        for (size_t j = start; j < end; j++) {
            IdmSyntax **inner_items = NULL;
            size_t inner_count = 0;
            if (layout_group_operator_items(ctx, items[j], &inner_items, &inner_count)) {
                for (size_t k = 1; k < inner_count; k++) flat[at++] = inner_items[k];
            } else {
                flat[at++] = items[j];
            }
        }
        IdmCore *result = expand_parts(ctx, flat, 0, total, err);
        free(flat);
        return result;
    }
    bool folded = false;
    IdmSyntax **collapsed = malloc((end - start) * sizeof(*collapsed));
    IdmSyntax **synthetic = collapsed ? malloc((end - start) * sizeof(*synthetic)) : NULL;
    if (!collapsed || !synthetic) { free(collapsed); free(synthetic); return (IdmCore *)(uintptr_t)idm_error_oom(err, items[start]->span); }
    size_t out_count = 0;
    size_t syn_count = 0;
    for (size_t j = start; j < end;) {
        if (items[j]->kind == IDM_SYN_WORD && j + 2u < end && syn_is_dot(items[j + 1u]) && items[j + 2u]->kind == IDM_SYN_WORD) {
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
    return expand_parts_inner(ctx, items, start, end, err);
}

static bool reserved_prefix_string(IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    for (size_t i = start; i + 1u < end; i++) {
        const IdmSyntax *word = items[i];
        const IdmSyntax *str = items[i + 1u];
        if (word->kind != IDM_SYN_WORD || !word->token_raw || str->kind != IDM_SYN_STRING) continue;
        if (!str->token_adjacent_previous || str->token_leading_space) continue;
        idm_error_set(err, word->span, "reserved prefixed string literal '%s\"…\"' (bitstrings are written %%<…>; bytestrings arrive post-1.0)", word->as.text);
        return true;
    }
    return false;
}

static bool binding_is_primitive_backed(const IdmBinding *binding, IdmPrimitive primitive) {
    IdmPrimitive bound = IDM_PRIM_COUNT;
    return binding &&
           (binding->kind == IDM_BIND_ENV || binding->kind == IDM_BIND_PACKAGE_SLOT) &&
           binding_passthrough(binding, &bound) &&
           bound == primitive;
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
        if (idm_syn_is_form_id(items[i], IDM_FORM_BODY)) last_body = i;
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

    if (items[start]->kind == IDM_SYN_WORD) {
        const PhaseSyntaxFn *cf_fn = NULL;
        if (resolve_head_core_form_exact(ctx, items[start], &cf_fn, err) && cf_fn && cf_fn->native) return end;
        if (err && err->present) return SIZE_MAX;
    }

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

static bool qualified_head_is_bound(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end) {
    if (start + 2u >= end || items[start]->kind != IDM_SYN_WORD || !syn_is_dot(items[start + 1u])) return false;
    size_t pos = end;
    IdmError qerr;
    idm_error_init(&qerr);
    IdmSyntax *word = make_qualified_word(items, start, &pos, &qerr);
    idm_error_clear(&qerr);
    if (!word) return false;
    bool bound = pos > start + 1u && head_is_bound(ctx, word);
    idm_syn_free(word);
    return bound;
}

IdmCore *expand_explain_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (end - start < 2u) return expand_error(err, items[start]->span, "explain expects an expression");
    size_t base = ctx->edge_count;
    bool saved = ctx->record_edges;
    ctx->record_edges = true;
    IdmCore *probed = expand_parts(ctx, items, start + 1u, end, err);
    ctx->record_edges = saved;
    if (!probed) {
        expand_edges_truncate(ctx, base);
        return NULL;
    }
    idm_core_free(probed);
    IdmValue acc = idm_empty_list();
    for (size_t i = ctx->edge_count; i > base; i--) {
        IdmValue entry = idm_nil();
        if (!expand_edge_value(ctx, ctx->rt, &ctx->edges[i - 1u], &entry, err)) {
            expand_edges_truncate(ctx, base);
            return NULL;
        }
        acc = idm_cons(ctx->rt, entry, acc, err);
        if (err && err->present) {
            expand_edges_truncate(ctx, base);
            return NULL;
        }
    }
    expand_edges_truncate(ctx, base);
    return idm_core_literal(acc, items[start]->span);
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
    if (items[start]->kind == IDM_SYN_WORD) {
        const IdmOperatorDef *syntax_capture = op_lookup_syntax_capture(ctx, items[start], false, err);
        if (err && err->present) return NULL;
        if (syntax_capture) return expand_syntax_capture_dispatch(ctx, syntax_capture, items[start], items, start, start, start + 1u, end, err);
    }
    if (items[start]->kind == IDM_SYN_WORD) {
        IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
        const IdmBinding *binding = resolve_default_demand(ctx, items[start], &status, err);
        if (err && err->present) return NULL;
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
    if (items[start]->kind == IDM_SYN_WORD && !head_is_bound(ctx, items[start]) &&
        !has_member_surface && !op_lookup(ctx, items[start], true) && !op_lookup(ctx, items[start], false) &&
        !qualified_head_is_bound(ctx, items, start, end)) {
        const PhaseSyntaxFn *core_form_fn = NULL;
        if (resolve_head_core_form(ctx, items[start], &core_form_fn, err)) return expand_core_form_use_from_parts(ctx, items, start, end, core_form_fn, err);
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
    if (parser->pos >= parser->end) {
        expand_error(parser->err, idm_span_unknown(NULL), "expected operand");
        if (parser->pos > 0 && expand_unit_open_at(parser->ctx, parser->items[parser->pos - 1u]->span)) idm_error_reason(parser->ctx->rt, parser->err, "incomplete", 0);
        return NULL;
    }
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

    if (head->kind == IDM_SYN_WORD) {
        const PhaseSyntaxFn *cf_fn = NULL;
        if (resolve_head_core_form_exact(parser->ctx, head, &cf_fn, parser->err) && cf_fn && cf_fn->native) {
            parser->pos = parser->end;
            return expand_core_form_use_from_parts(parser->ctx, parser->items, start, parser->end, cf_fn, parser->err);
        }
        if (parser->err && parser->err->present) return NULL;
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
    if (status == IDM_RESOLVE_OK && binding && binding->kind == IDM_BIND_TRANSFORMER) {
        record_operator_edge(ctx, op, op_token);
        return expand_syntax_capture_macro(ctx, op, op_token, target_word, binding->payload, items, left_start, left_end, cursor, end, err);
    }
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

static void record_operator_edge(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token) {
    if (!ctx->record_edges) return;
    char *key = operator_binding_key(op->name, op->capture ? op->capture : "value");
    const IdmBinding *binding = NULL;
    if (key) {
        const IdmScopeSet *scopes = op_token ? idm_syn_scope_set(op_token, 0) : NULL;
        IdmResolveStatus status = resolve_scoped(ctx, key, IDM_BIND_SPACE_OPERATOR, scopes, ctx->op_fallback, &binding);
        if (status != IDM_RESOLVE_OK) binding = NULL;
    }
    expand_edge_record(ctx, "operator", op->name, binding ? binding->provider : NULL, NULL, NULL, NULL, op_token ? op_token->span : idm_span_unknown(NULL));
    free(key);
}

static IdmSyntax *operator_method_word(const IdmOperatorDef *op, const IdmSyntax *op_token, IdmSpan span, IdmError *err) {
    IdmSyntax *word = operator_target_word(op, op_token, span, err);
    if (!word) return NULL;
    const IdmScopeSet *use_scopes = op_token ? idm_syn_scope_set(op_token, 0) : NULL;
    for (size_t i = 0; use_scopes && i < use_scopes->count; i++) {
        if (!idm_syn_scope_add(word, 0, use_scopes->items[i])) {
            idm_syn_free(word);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    return word;
}

static IdmCore *make_operator_method_call(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    IdmSyntax *mword = operator_method_word(op, op_token, span, err);
    if (!mword) {
        core_args_free(args, arg_count);
        return NULL;
    }
    MethodSurfaceGroup group;
    IdmResolveStatus mstatus = IDM_RESOLVE_UNBOUND;
    if (!resolve_method_surface_group(ctx, mword, &group, &mstatus, err)) {
        idm_syn_free(mword);
        core_args_free(args, arg_count);
        return NULL;
    }
    idm_syn_free(mword);
    if (group.count == 0) {
        method_surface_group_destroy(&group);
        core_args_free(args, arg_count);
        return expand_error(err, span, "operator '%s' has no visible method '%s'", op->name, op->target_name);
    }
    IdmCore *call = expand_method_surface_call_cores(ctx, &group, NULL, args, arg_count, span, err);
    method_surface_group_destroy(&group);
    return call;
}

static IdmCore *make_operator_call_args(ExpandContext *ctx, const IdmOperatorDef *op, const IdmSyntax *op_token, IdmCore **args, size_t arg_count, IdmSpan span, IdmError *err) {
    record_operator_edge(ctx, op, op_token);
    if (op->action == IDM_OPERATOR_ACTION_METHOD) return make_operator_method_call(ctx, op, op_token, args, arg_count, span, err);
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
    if (idm_syn_is_form_id(syn, IDM_FORM_WORD) && syn->as.seq.count == 2 && syn->as.seq.items[1]->kind == IDM_SYN_STRING) return syn->as.seq.items[1]->as.text;
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
        if (cursor + 1u != end || !idm_syn_is_form_id(items[cursor], IDM_FORM_BODY)) return idm_error_set(err, op_token->span, "operator '%s' expects an indented do/end body", op->name);
        return syntax_payload_add(out, idm_syn_clone(items[cursor]), items[cursor]->span, err);
    }
    if (kind == IDM_OPERATOR_CAPTURE_COUNT) {
        size_t close = cursor + (size_t)count;
        if (close > end) return idm_error_set(err, op_token->span, "operator '%s' expected %u captured syntax item(s)", op->name, count);
        if (close != end) return idm_error_set(err, items[close]->span, "unexpected trailing syntax after operator '%s' count capture", op->name);
        return syntax_payload_add(out, syntax_list_from_range(items, cursor, close, op_token->span, err), op_token->span, err);
    }
    if (kind == IDM_OPERATOR_CAPTURE_EXPRESSION) {
        if (cursor >= end) {
            idm_error_set(err, op_token->span, "operator '%s' expression capture requires an expression", op->name);
            if (expand_unit_open_at(ctx, op_token->span)) idm_error_reason(ctx->rt, err, "incomplete", 0);
            return false;
        }
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

static IdmCore *parse_enforest_expr(EnforestParser *parser, uint16_t min_prec) {
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
        uint16_t next_min = op->assoc == IDM_OP_ASSOC_RIGHT ? op->precedence : (uint16_t)(op->precedence + 1u);
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

static IdmCore *expand_program_root(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    if (!idm_syn_is_form_id(syn, IDM_FORM_PACKAGE_BEGIN)) return expand_error(err, syn->span, "expected %%-package-begin syntax");
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        const IdmSyntax *item = syn->as.seq.items[i];
        if (idm_syn_is_form_id(item, IDM_FORM_EXPR) && item->as.seq.count == 3 &&
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

static IdmCore *expand_protocol_info_parts(ExpandContext *ctx, IdmSyntax *const *items, size_t start, size_t end, IdmError *err) {
    if (end - start != 2u || items[start + 1u]->kind != IDM_SYN_WORD) {
        return expand_error(err, items[start]->span, "protocol-info expects a protocol name");
    }
    const IdmSyntax *name_syntax = items[start + 1u];
    IdmResolveStatus status = IDM_RESOLVE_UNBOUND;
    ProtocolDef *p = resolve_protocol_def(ctx, name_syntax, &status);
    if (status == IDM_RESOLVE_AMBIGUOUS) return expand_error(err, name_syntax->span, "ambiguous protocol '%s'", name_syntax->as.text);
    if (!p) return expand_error(err, name_syntax->span, "protocol-info expects a protocol; '%s' is unbound", name_syntax->as.text);
    const IdmArtifact *art = p->art;
    char provider_key[65];
    artifact_provider_key(art->src_hash, provider_key);
    IdmValue env_key = idm_atom(ctx->rt, provider_key);
    IdmSpan span = items[start]->span;
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_DICT, span, err);
    if (!call) return NULL;
    for (size_t i = 0; i < art->slot_count; i++) {
        const IdmPkgSlot *slot = &art->slots[i];
        if (!slot->name || strncmp(slot->name, "info:", 5u) != 0) continue;
        IdmCore *key = idm_core_literal(idm_atom(ctx->rt, slot->name + 5u), span);
        if (!key || !core_call_add_arg_or_free(call, key, err, span)) {
            if (!key) idm_core_free(call);
            return NULL;
        }
        IdmCore *ref = idm_core_package_ref(slot->name, env_key, slot->slot, span);
        if (!ref || !core_call_add_arg_or_free(call, ref, err, span)) {
            if (!ref) idm_core_free(call);
            return NULL;
        }
    }
    return call;
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

static bool bitseg_has_opt(const IdmSyntax *seg, size_t opt_start, const char *name) {
    for (size_t i = opt_start; i < seg->as.seq.count; i++) {
        const IdmSyntax *o = seg->as.seq.items[i];
        if (o->kind == IDM_SYN_ATOM && o->as.text && strcmp(o->as.text, name) == 0) return true;
    }
    return false;
}

static IdmCore *bits_opts_list_core(ExpandContext *ctx, const IdmSyntax *seg, size_t opt_start, IdmError *err) {
    IdmCore *call = expand_primitive_clause_call(IDM_PRIM_LIST, seg->span, err);
    if (!call) return NULL;
    for (size_t i = opt_start; i < seg->as.seq.count; i++) {
        const IdmSyntax *o = seg->as.seq.items[i];
        if (o->kind != IDM_SYN_ATOM) continue;
        if (strcmp(o->as.text, "float") == 0 || strcmp(o->as.text, "bits") == 0) continue;
        IdmValue av = idm_nil();
        if (!value_from_literal_syntax(ctx, o, &av, err)) { idm_core_free(call); return NULL; }
        IdmCore *lit = idm_core_literal(av, o->span);
        if (!lit || !core_call_add_arg_or_free(call, lit, err, o->span)) { idm_core_free(lit); return NULL; }
    }
    return call;
}

static IdmCore *bits_call_take(IdmPrimitive prim, IdmSpan span, IdmError *err, IdmCore *a, IdmCore *b, IdmCore *c) {
    if (!a || (b == NULL && c != NULL)) {
        idm_core_free(a);
        idm_core_free(b);
        idm_core_free(c);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    IdmCore *call = expand_primitive_clause_call(prim, span, err);
    if (!call) { idm_core_free(a); idm_core_free(b); idm_core_free(c); return NULL; }
    if (!core_call_add_arg_or_free(call, a, err, span)) { idm_core_free(b); idm_core_free(c); return NULL; }
    if (b && !core_call_add_arg_or_free(call, b, err, span)) { idm_core_free(c); return NULL; }
    if (c && !core_call_add_arg_or_free(call, c, err, span)) return NULL;
    return call;
}

static IdmCore *bits_splice_checked(ExpandContext *ctx, IdmCore *value, IdmSpan span, IdmError *err) {
    if (!value) return NULL;
    uint32_t slot = ctx->next_slot++;
    IdmCore *lref = idm_core_local_ref("bits-splice", slot, span);
    IdmCore *len = bits_call_take(IDM_PRIM_BITS_LEN, span, err, lref, NULL, NULL);
    IdmCore *sref = len ? idm_core_local_ref("bits-splice", slot, span) : NULL;
    IdmCore *zero = sref ? idm_core_literal(idm_int(0), span) : NULL;
    if (!zero) idm_core_free(sref);
    IdmCore *slice = zero ? bits_call_take(IDM_PRIM_BITS_SLICE, span, err, sref, zero, len) : NULL;
    if (!slice) {
        if (zero == NULL) idm_core_free(len);
        idm_core_free(value);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    IdmCore *bound = idm_core_bind_local("bits-splice", slot, value, slice, span);
    if (!bound) {
        idm_core_free(value);
        idm_core_free(slice);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    return bound;
}

static IdmCore *bitstring_segment_piece(ExpandContext *ctx, const IdmSyntax *seg, IdmError *err) {
    if (idm_syn_is_form_id(seg, IDM_FORM_BITREST)) {
        if (seg->as.seq.count != 2u) return expand_error(err, seg->span, "malformed bitstring rest segment");
        return bits_splice_checked(ctx, expand_syntax(ctx, seg->as.seq.items[1], err), seg->span, err);
    }
    if (idm_syn_is_form_id(seg, IDM_FORM_BITSEG_BARE)) {
        const IdmSyntax *value = seg->as.seq.items[1];
        if (bitseg_has_opt(seg, 2u, "float")) return expand_error(err, seg->span, "bitstring float segment requires a size");
        if (bitseg_has_opt(seg, 2u, "bits")) return bits_splice_checked(ctx, expand_syntax(ctx, value, err), seg->span, err);
        if (value->kind == IDM_SYN_STRING || idm_syn_is_form_id(value, IDM_FORM_STRING)) {
            IdmCore *sv = expand_syntax(ctx, value, err);
            return bits_call_take(IDM_PRIM_STRING_BITS, seg->span, err, sv, NULL, NULL);
        }
        IdmCore *vc = expand_syntax(ctx, value, err);
        if (!vc) return NULL;
        IdmCore *sc = idm_core_literal(idm_int(8), seg->span);
        IdmCore *opts = sc ? bits_opts_list_core(ctx, seg, 2u, err) : NULL;
        if (!opts) {
            idm_core_free(vc);
            idm_core_free(sc);
            if (err && !err->present) idm_error_oom(err, seg->span);
            return NULL;
        }
        return bits_call_take(IDM_PRIM_BITS_OF_INT, seg->span, err, vc, sc, opts);
    }
    const IdmSyntax *value = seg->as.seq.items[1];
    const IdmSyntax *size = seg->as.seq.items[2];
    bool is_float = bitseg_has_opt(seg, 3u, "float");
    bool is_bits = bitseg_has_opt(seg, 3u, "bits");
    if (is_float && is_bits) return expand_error(err, seg->span, "bitstring segment cannot be both float and bits");
    IdmCore *vc = expand_syntax(ctx, value, err);
    if (!vc) return NULL;
    IdmCore *sc = expand_syntax(ctx, size, err);
    if (!sc) { idm_core_free(vc); return NULL; }
    if (is_bits) {
        IdmCore *zero = idm_core_literal(idm_int(0), seg->span);
        return bits_call_take(IDM_PRIM_BITS_SLICE, seg->span, err, vc, zero, sc);
    }
    IdmCore *opts = bits_opts_list_core(ctx, seg, 3u, err);
    if (!opts) { idm_core_free(vc); idm_core_free(sc); return NULL; }
    return bits_call_take(is_float ? IDM_PRIM_BITS_OF_FLOAT : IDM_PRIM_BITS_OF_INT, seg->span, err, vc, sc, opts);
}

static IdmCore *expand_form_bitstring(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmCore *acc = NULL;
    for (size_t i = 1; i < syn->as.seq.count; i++) {
        IdmCore *piece = bitstring_segment_piece(ctx, syn->as.seq.items[i], err);
        if (!piece) { idm_core_free(acc); return NULL; }
        if (!acc) { acc = piece; continue; }
        acc = bits_call_take(IDM_PRIM_BITS_APPEND, syn->span, err, acc, piece, NULL);
        if (!acc) return NULL;
    }
    if (acc) return acc;
    IdmCore *opts = expand_primitive_clause_call(IDM_PRIM_LIST, syn->span, err);
    if (!opts) return NULL;
    return bits_call_take(IDM_PRIM_BITS_OF_INT, syn->span, err,
                          idm_core_literal(idm_int(0), syn->span), idm_core_literal(idm_int(0), syn->span), opts);
}

static IdmEnforestNode *enforest_node_new(IdmEnforestOp op) {
    IdmEnforestNode *node = calloc(1u, sizeof(*node));
    if (node) node->op = (uint8_t)op;
    return node;
}

IdmEnforestNode *idm_enforest_parse(const IdmSyntax *term, IdmError *err) {
    if (!term || (term->kind != IDM_SYN_TUPLE && term->kind != IDM_SYN_LIST) || term->as.seq.count < 1u ||
        term->as.seq.items[0]->kind != IDM_SYN_ATOM) {
        idm_error_set(err, term ? term->span : idm_span_unknown(NULL), "enforest instruction expects a tagged iridium node");
        return NULL;
    }
    const char *tag = term->as.seq.items[0]->as.text;
    size_t count = term->as.seq.count;
    IdmEnforestNode *node = NULL;
    if (strcmp(tag, "child") == 0) {
        if (count != 2u && count != 3u) { idm_error_set(err, term->span, ":child expects an index and an optional instruction"); return NULL; }
        if (term->as.seq.items[1]->kind != IDM_SYN_INT || term->as.seq.items[1]->as.integer < 1) { idm_error_set(err, term->span, ":child index must be a positive integer"); return NULL; }
        node = enforest_node_new(IDM_ENFOREST_CHILD);
        if (!node) { idm_error_oom(err, term->span); return NULL; }
        node->index = (uint32_t)term->as.seq.items[1]->as.integer;
        if (count == 3u) {
            node->child = idm_enforest_parse(term->as.seq.items[2], err);
            if (!node->child) { idm_enforest_free(node); return NULL; }
        }
        return node;
    }
    if (strcmp(tag, "each") == 0) {
        if (count != 1u && count != 2u) { idm_error_set(err, term->span, ":each expects at most one instruction"); return NULL; }
        node = enforest_node_new(IDM_ENFOREST_EACH);
        if (!node) { idm_error_oom(err, term->span); return NULL; }
        if (count == 2u) {
            node->child = idm_enforest_parse(term->as.seq.items[1], err);
            if (!node->child) { idm_enforest_free(node); return NULL; }
        }
        return node;
    }
    if (strcmp(tag, "value") == 0 || strcmp(tag, "scope") == 0 || strcmp(tag, "concat") == 0 || strcmp(tag, "syntax") == 0) {
        IdmEnforestOp op = strcmp(tag, "value") == 0 ? IDM_ENFOREST_VALUE
                         : strcmp(tag, "scope") == 0 ? IDM_ENFOREST_SCOPE
                         : strcmp(tag, "concat") == 0 ? IDM_ENFOREST_CONCAT
                         : IDM_ENFOREST_SYNTAX;
        if (count != 2u) { idm_error_set(err, term->span, ":%s expects one instruction", tag); return NULL; }
        node = enforest_node_new(op);
        if (!node) { idm_error_oom(err, term->span); return NULL; }
        node->child = idm_enforest_parse(term->as.seq.items[1], err);
        if (!node->child) { idm_enforest_free(node); return NULL; }
        if (op == IDM_ENFOREST_CONCAT && node->child->op != IDM_ENFOREST_EACH) {
            idm_error_set(err, term->span, ":concat expects an :each instruction");
            idm_enforest_free(node);
            return NULL;
        }
        if (op == IDM_ENFOREST_SYNTAX && node->child->op != IDM_ENFOREST_CHILD) {
            idm_error_set(err, term->span, ":syntax expects a :child selector");
            idm_enforest_free(node);
            return NULL;
        }
        return node;
    }
    if (strcmp(tag, "template") == 0) {
        if (count != 3u || term->as.seq.items[1]->kind != IDM_SYN_ATOM) { idm_error_set(err, term->span, ":template expects a mode atom and a :child selector"); return NULL; }
        const char *mode = term->as.seq.items[1]->as.text;
        node = enforest_node_new(IDM_ENFOREST_TEMPLATE);
        if (!node) { idm_error_oom(err, term->span); return NULL; }
        if (strcmp(mode, "quote") == 0) node->mode = IDM_ENFOREST_TEMPLATE_QUOTE;
        else if (strcmp(mode, "quasiquote") == 0) node->mode = IDM_ENFOREST_TEMPLATE_QUASIQUOTE;
        else if (strcmp(mode, "quasisyntax") == 0) node->mode = IDM_ENFOREST_TEMPLATE_QUASISYNTAX;
        else { idm_error_set(err, term->span, "template mode must be :quote, :quasiquote, or :quasisyntax"); idm_enforest_free(node); return NULL; }
        node->child = idm_enforest_parse(term->as.seq.items[2], err);
        if (!node->child) { idm_enforest_free(node); return NULL; }
        if (node->child->op != IDM_ENFOREST_CHILD) {
            idm_error_set(err, term->span, ":template expects a :child selector");
            idm_enforest_free(node);
            return NULL;
        }
        return node;
    }
    if (strcmp(tag, "parts") == 0 || strcmp(tag, "body") == 0 || strcmp(tag, "match") == 0 || strcmp(tag, "regex") == 0 || strcmp(tag, "bitstring") == 0 || strcmp(tag, "try") == 0 || strcmp(tag, "receive") == 0 || strcmp(tag, "implements") == 0 || strcmp(tag, "protocol-info") == 0) {
        if (count != 1u) { idm_error_set(err, term->span, ":%s takes no children", tag); return NULL; }
        IdmEnforestOp op = strcmp(tag, "parts") == 0 ? IDM_ENFOREST_PARTS
                         : strcmp(tag, "body") == 0 ? IDM_ENFOREST_BODY
                         : strcmp(tag, "match") == 0 ? IDM_ENFOREST_MATCH
                         : strcmp(tag, "regex") == 0 ? IDM_ENFOREST_REGEX
                         : strcmp(tag, "try") == 0 ? IDM_ENFOREST_TRY
                         : strcmp(tag, "receive") == 0 ? IDM_ENFOREST_RECEIVE
                         : strcmp(tag, "implements") == 0 ? IDM_ENFOREST_IMPLEMENTS
                         : strcmp(tag, "protocol-info") == 0 ? IDM_ENFOREST_PROTOCOL_INFO
                         : IDM_ENFOREST_BITSTRING;
        node = enforest_node_new(op);
        if (!node) idm_error_oom(err, term->span);
        return node;
    }
    idm_error_set(err, term->span, "unknown enforest instruction ':%s'", tag);
    return NULL;
}

static const IdmSyntax *enforest_select(const IdmSyntax *subject, const IdmEnforestNode *node, IdmError *err) {
    if (subject->kind != IDM_SYN_LIST || node->index >= subject->as.seq.count) {
        expand_error(err, subject->span, "enforest child %u is out of bounds", node->index);
        return NULL;
    }
    return subject->as.seq.items[node->index];
}

static IdmCore *enforest_eval(ExpandContext *ctx, const IdmSyntax *subject, const IdmEnforestNode *node, IdmError *err) {
    switch ((IdmEnforestOp)node->op) {
        case IDM_ENFOREST_CHILD: {
            const IdmSyntax *child = enforest_select(subject, node, err);
            if (!child) return NULL;
            return node->child ? enforest_eval(ctx, child, node->child, err) : expand_syntax(ctx, child, err);
        }
        case IDM_ENFOREST_VALUE: {
            bool saved = ctx->value_context;
            ctx->value_context = true;
            IdmCore *core = enforest_eval(ctx, subject, node->child, err);
            ctx->value_context = saved;
            return core;
        }
        case IDM_ENFOREST_SCOPE: {
            SurfaceCheckpoint checkpoint;
            surface_checkpoint(ctx, &checkpoint);
            IdmCore *core = enforest_eval(ctx, subject, node->child, err);
            surface_rollback(ctx, &checkpoint);
            return core;
        }
        case IDM_ENFOREST_CONCAT: {
            if (subject->kind != IDM_SYN_LIST) return expand_error(err, subject->span, ":concat expects a form");
            if (subject->as.seq.count <= 1u) return expand_string_literal(ctx, "", subject->span, err);
            IdmCore *concat = idm_core_string_concat(subject->span);
            if (!concat) { idm_error_oom(err, subject->span); return NULL; }
            const IdmEnforestNode *item = node->child->child;
            for (size_t i = 1; i < subject->as.seq.count; i++) {
                const IdmSyntax *part = subject->as.seq.items[i];
                IdmCore *pc = item ? enforest_eval(ctx, part, item, err) : expand_syntax(ctx, part, err);
                if (!pc || !idm_core_string_concat_add(concat, pc)) {
                    if (pc) { idm_core_free(pc); idm_error_oom(err, subject->span); }
                    idm_core_free(concat);
                    return NULL;
                }
            }
            return concat;
        }
        case IDM_ENFOREST_SYNTAX: {
            const IdmSyntax *child = enforest_select(subject, node->child, err);
            if (!child) return NULL;
            IdmValue value = idm_syntax_value(ctx->rt, child, err);
            if (err && err->present) return NULL;
            return idm_core_literal(value, subject->span);
        }
        case IDM_ENFOREST_TEMPLATE: {
            const IdmSyntax *child = enforest_select(subject, node->child, err);
            if (!child) return NULL;
            switch ((IdmEnforestTemplateMode)node->mode) {
                case IDM_ENFOREST_TEMPLATE_QUOTE: {
                    IdmValue value = idm_nil();
                    if (!expand_quote_datum(ctx, child, &value, err)) return NULL;
                    return idm_core_literal(value, subject->span);
                }
                case IDM_ENFOREST_TEMPLATE_QUASIQUOTE:
                    return expand_template_quasiquote(ctx, child, err);
                case IDM_ENFOREST_TEMPLATE_QUASISYNTAX:
                    return expand_template_quasisyntax(ctx, child, child, err);
            }
            return expand_error(err, subject->span, "unsupported template mode");
        }
        case IDM_ENFOREST_PARTS:
            return expand_parts(ctx, subject->as.seq.items, 1, subject->as.seq.count, err);
        case IDM_ENFOREST_BODY:
            return expand_body_items(ctx, subject->as.seq.items, 1, subject->as.seq.count, true, err);
        case IDM_ENFOREST_REGEX:
            return expand_form_regex(ctx, subject, err);
        case IDM_ENFOREST_MATCH:
            return expand_form_match(ctx, subject, err);
        case IDM_ENFOREST_BITSTRING:
            return expand_form_bitstring(ctx, subject, err);
        case IDM_ENFOREST_TRY:
            return expand_try_parts(ctx, subject->as.seq.items, 0, subject->as.seq.count, err);
        case IDM_ENFOREST_RECEIVE:
            return expand_receive_parts(ctx, subject->as.seq.items, 0, subject->as.seq.count, err);
        case IDM_ENFOREST_IMPLEMENTS:
            return expand_implements_parts(ctx, subject->as.seq.items, 0, subject->as.seq.count, err);
        case IDM_ENFOREST_PROTOCOL_INFO:
            return expand_protocol_info_parts(ctx, subject->as.seq.items, 0, subject->as.seq.count, err);
        case IDM_ENFOREST_EACH:
            break;
    }
    return expand_error(err, subject->span, "unsupported enforest instruction");
}

static bool reader_form_stage(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmEnforestNode *node, const PhaseSyntaxFn *fn, IdmError *err) {
    if (ctx->decl_reader_forms_count == ctx->decl_reader_forms_cap) {
        if (!idm_grow((void **)&ctx->decl_reader_forms, &ctx->decl_reader_forms_cap, sizeof(*ctx->decl_reader_forms), 4u, ctx->decl_reader_forms_count + 1u))
            return idm_error_oom(err, idm_span_unknown(NULL));
    }
    ReaderFormDef *def = &ctx->decl_reader_forms[ctx->decl_reader_forms_count];
    memset(def, 0, sizeof(*def));
    def->name = idm_strdup(name);
    if (!def->name) return idm_error_oom(err, idm_span_unknown(NULL));
    if (scopes && !idm_scope_set_copy(&def->scopes, scopes)) { free(def->name); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (node) {
        def->node = idm_enforest_clone(node);
        if (!def->node) { free(def->name); idm_scope_set_destroy(&def->scopes); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (fn) {
        def->transformer = true;
        def->fn.module = idm_module_ref_retain(fn->module);
        def->fn.function_index = fn->function_index;
        def->fn.phase_env = idm_phase_env_retain(fn->phase_env);
    }
    ctx->decl_reader_forms_count++;
    return true;
}

static bool reader_form_bind(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmEnforestNode *node, const PhaseSyntaxFn *fn, IdmError *err) {
    ReaderFormBinding *binding = calloc(1u, sizeof(*binding));
    if (!binding) return idm_error_oom(err, idm_span_unknown(NULL));
    if (node) {
        binding->node = idm_enforest_clone(node);
        if (!binding->node) { free(binding); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (fn) {
        binding->transformer = true;
        binding->fn.module = idm_module_ref_retain(fn->module);
        binding->fn.function_index = fn->function_index;
        binding->fn.phase_env = idm_phase_env_retain(fn->phase_env);
    }
    IdmBindingId binding_id = 0;
    if (!idm_binding_table_add_data(&ctx->bindings, name, IDM_PHASE_ANY, IDM_BIND_SPACE_READER_FORM, IDM_BIND_READER_FORM, scopes ? scopes : &ctx->empty_scopes, binding, IDM_FRAME_ENV, &binding_id)) {
        reader_form_binding_free(binding);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

bool register_reader_form(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmEnforestNode *node, IdmError *err) {
    if (!name || !*name) return idm_error_set(err, idm_span_unknown(NULL), "reader form name must be non-empty");
    return reader_form_stage(ctx, name, scopes, node, NULL, err) && reader_form_bind(ctx, name, scopes, node, NULL, err);
}

bool register_reader_form_fn(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const PhaseSyntaxFn *fn, IdmError *err) {
    return reader_form_stage(ctx, name, scopes, NULL, fn, err) && reader_form_bind(ctx, name, scopes, NULL, fn, err);
}

bool bind_reader_form(ExpandContext *ctx, const char *name, const IdmScopeSet *scopes, const IdmEnforestNode *node, const PhaseSyntaxFn *fn, IdmError *err) {
    if (!name || !*name) return idm_error_set(err, idm_span_unknown(NULL), "reader form name must be non-empty");
    return reader_form_bind(ctx, name, scopes, node, fn, err);
}

static const ReaderFormBinding *resolve_reader_form(ExpandContext *ctx, const IdmSyntax *syn) {
    if (syn->kind != IDM_SYN_LIST || syn->as.seq.count == 0 || syn->as.seq.items[0]->kind != IDM_SYN_WORD) return NULL;
    const IdmSyntax *head = syn->as.seq.items[0];
    if (strncmp(head->as.text, "%-", 2u) != 0) return NULL;
    const IdmScopeSet *scopes = idm_syn_scope_set(head, 0);
    const IdmBinding *binding = NULL;
    IdmResolveStatus status = resolve_scoped(ctx, head->as.text, IDM_BIND_SPACE_READER_FORM, scopes, NULL, &binding);
    if (status != IDM_RESOLVE_OK || !binding || !binding->data) return NULL;
    return (const ReaderFormBinding *)binding->data;
}

IdmCore *expand_syntax(ExpandContext *ctx, const IdmSyntax *syn, IdmError *err) {
    IdmCore *lit = literal_from_syntax(ctx, syn, err);
    if (lit || (err && err->present)) return lit;

    if (syn->kind == IDM_SYN_WORD) return expand_word_ref(ctx, syn, err);
    const ReaderFormBinding *reader_form = resolve_reader_form(ctx, syn);
    if (reader_form) {
        if (reader_form->transformer) return expand_reader_form_transform(ctx, syn, &reader_form->fn, err);
        return enforest_eval(ctx, syn, reader_form->node, err);
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_WORD)) {
        return expand_error(err, syn->span, "word syntax requires a core form");
    }
    if (idm_syn_is_form_id(syn, IDM_FORM_PIN)) {
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
    SavedHooks saved_hooks;
    bool hooks_saved;
    bool sched_borrowed;
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

IdmRepl *idm_repl_create_with(IdmRuntime *rt, IdmScheduler *sched, IdmError *err) {
    IdmRepl *repl = calloc(1u, sizeof(*repl));
    if (!repl) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    repl->rt = rt;
    if (!ctx_init(&repl->ctx, rt)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&repl->ctx);
        free(repl);
        return NULL;
    }
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
    if (!ctx_activate_kernel(&repl->ctx, err)) {
        idm_repl_destroy(repl);
        return NULL;
    }
    if (sched) {
        repl->sched = sched;
        repl->sched_borrowed = true;
    } else {
        repl->sched = idm_sched_create(rt, NULL, err);
        if (!repl->sched) {
            idm_repl_destroy(repl);
            return NULL;
        }
    }
    rt->repl = repl;
    return repl;
}

IdmRepl *idm_repl_create(IdmRuntime *rt, IdmError *err) {
    return idm_repl_create_with(rt, NULL, err);
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
    expand_solved_types_clear(&repl->ctx);
    surface_discard(&repl->ctx, &repl->checkpoint);
    pthread_mutex_unlock(&repl->mu);
    return IDM_REPL_ERROR;
}

static IdmReplStatus repl_compile_incomplete(IdmRepl *repl, IdmError *err) {
    idm_error_clear(err);
    expand_solved_types_clear(&repl->ctx);
    surface_discard(&repl->ctx, &repl->checkpoint);
    pthread_mutex_unlock(&repl->mu);
    return IDM_REPL_INCOMPLETE;
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

static IdmReplStatus repl_compile_source(IdmRepl *repl, const char *file, const char *source, bool make_thunk, bool accommodate, IdmValue *out_thunk, uint64_t *out_token, IdmError *err) {
    if (out_thunk) *out_thunk = idm_nil();
    if (out_token) *out_token = 0;
    pthread_mutex_lock(&repl->mu);
    repl_install_hooks(repl);
    IdmSyntax *program = NULL;
    if (!expand_unit_read(&repl->ctx, file, source, 0, &program, err)) {
        pthread_mutex_unlock(&repl->mu);
        if (accommodate && idm_error_reason_is(err, "incomplete")) {
            idm_error_clear(err);
            return IDM_REPL_INCOMPLETE;
        }
        return IDM_REPL_ERROR;
    }
    repl->ctx.unit_source = source;
    repl->ctx.unit_label = file;
    repl->ctx.unit_reader_generation = repl->ctx.reader_generation;
    if (!idm_syn_scope_add_tree(program, 0, repl->session_scope)) {
        idm_syn_free(program);
        idm_error_oom(err, idm_span_unknown(NULL));
        pthread_mutex_unlock(&repl->mu);
        return IDM_REPL_ERROR;
    }
    surface_checkpoint(&repl->ctx, &repl->checkpoint);
    repl->token = 0;
    IdmCore *core = program->as.seq.count < 2
        ? idm_core_literal(idm_nil(), program->span)
        : expand_body_items(&repl->ctx, program->as.seq.items, 1, program->as.seq.count, false, err);
    idm_syn_free(program);
    if (!core && accommodate && (repl->ctx.unit_read_incomplete || idm_error_reason_is(err, "incomplete"))) return repl_compile_incomplete(repl, err);
    if (!core) return repl_compile_fail(repl, err);
    if (!make_thunk) {
        idm_core_free(core);
        expand_solved_types_clear(&repl->ctx);
        repl->token = 0;
        pthread_mutex_unlock(&repl->mu);
        return IDM_REPL_OK;
    }
    core = wrap_kernel_use(&repl->ctx, core, err);
    if (!core) return repl_compile_fail(repl, err);
    IdmBytecodeModule *module = malloc(sizeof(*module));
    if (!module) {
        idm_core_free(core);
        return repl_compile_fail(repl, err);
    }
    idm_bc_init(module);
    uint32_t main_fn = 0;
    bool compiled = idm_core_compile_main(repl->rt, core, module, &main_fn, err);
    idm_core_free(core);
    expand_solved_types_clear(&repl->ctx);
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
    IdmReplStatus status = repl_compile_source(repl, "<repl>", source, true, true, out_thunk, out_token, err);
    if (status != IDM_REPL_INCOMPLETE) return status;
    char *flat = repl_flatten_lines(source);
    if (!flat) return status;
    status = repl_compile_source(repl, "<repl>", flat, true, true, out_thunk, out_token, err);
    free(flat);
    return status;
}


void idm_repl_abort(IdmRepl *repl, uint64_t token) {
    pthread_mutex_lock(&repl->mu);
    if (token != 0 && token == repl->token) {
        surface_discard(&repl->ctx, &repl->checkpoint);
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

bool idm_repl_loop_thunk(IdmRepl *repl, const char *source, IdmValue *out_thunk, IdmError *err) {
    IdmReplStatus status = repl_compile_source(repl, "<repl>", source, true, true, out_thunk, NULL, err);
    if (status == IDM_REPL_INCOMPLETE) return idm_error_set(err, idm_span_unknown("<repl>"), "incomplete REPL startup source");
    return status == IDM_REPL_OK;
}

void idm_repl_destroy(IdmRepl *repl) {
    if (!repl) return;
    if (repl->rt && repl->rt->repl == repl) repl->rt->repl = NULL;
    repl_restore_hooks(repl);
    if (!repl->sched_borrowed) idm_sched_destroy(repl->sched);
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

static bool idm_expand_syntax_with_runner(IdmRuntime *rt, const IdmSyntax *syntax, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.syntax");
    ExpandContext ctx;
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
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
    if (ctx_activate_kernel(&ctx, err)) {
        core = expand_program_root(&ctx, syntax, err);
        if (core && !(err && err->present)) core = wrap_kernel_use(&ctx, core, err);
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

static bool expand_source_core(ExpandContext *ctx, const char *file, const char *source, IdmCore **out, IdmError *err) {
    IdmCore *core = NULL;
    if (ctx_activate_kernel(ctx, err)) {
        ctx->unit_source = source;
        ctx->unit_label = file;
        ctx->unit_reader_generation = ctx->reader_generation;
        IdmSyntax *program = NULL;
        if (expand_unit_read(ctx, file, source, 0, &program, err)) {
            core = expand_program_root(ctx, program, err);
            idm_syn_free(program);
        }
        if (core && !(err && err->present)) core = wrap_kernel_use(ctx, core, err);
    }
    if (!core || (err && err->present)) {
        idm_core_free(core);
        return false;
    }
    *out = core;
    return true;
}

bool idm_expand_source_string(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmError *err) {
    return idm_expand_source_string_with_runner(rt, file, source, NULL, out, err);
}

void idm_expand_trace_destroy(IdmExpandTrace *trace) {
    if (!trace) return;
    for (size_t i = 0; i < trace->dep_count; i++) free(trace->deps[i].path);
    free(trace->deps);
    memset(trace, 0, sizeof(*trace));
}

static bool expand_trace_add_dep(IdmExpandTrace *trace, const IdmArtifactDep *dep, IdmError *err) {
    for (size_t i = 0; i < trace->dep_count; i++) {
        if (trace->deps[i].kind == dep->kind && strcmp(trace->deps[i].path, dep->path) == 0) return true;
    }
    IdmArtifactDep *next = realloc(trace->deps, (trace->dep_count + 1u) * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(dep->path));
    trace->deps = next;
    IdmArtifactDep *copy = &trace->deps[trace->dep_count];
    memset(copy, 0, sizeof(*copy));
    copy->path = idm_strdup(dep->path);
    if (!copy->path) return idm_error_oom(err, idm_span_unknown(dep->path));
    memcpy(copy->hash, dep->hash, 32u);
    copy->kind = dep->kind;
    trace->dep_count++;
    return true;
}

bool idm_expand_trace_add_reads(IdmExpandTrace *trace, const IdmPhaseReads *reads, IdmError *err) {
    if (!trace) return true;
    if (reads) {
        if (reads->failed) return idm_error_oom(err, idm_span_unknown(NULL));
        if (reads->uncacheable) trace->cacheable = false;
        for (size_t i = 0; i < reads->count; i++) {
            if (!expand_trace_add_dep(trace, &reads->items[i], err)) return false;
        }
    }
    IdmArtifact action;
    memset(&action, 0, sizeof(action));
    memcpy(action.src_hash, trace->source_hash, 32u);
    action.deps = trace->deps;
    action.dep_count = trace->dep_count;
    if (!idm_artifact_compute_action_hash(&action, err)) return false;
    memcpy(trace->action_hash, action.action_hash, 32u);
    return true;
}

static bool expand_trace_finish(IdmExpandTrace *trace, const char *file, const char *source, const ExpandContext *ctx, const IdmPhaseReads *reads, IdmError *err) {
    IdmBuffer input;
    idm_buf_init(&input);
    const char *label = file ? file : "<program>";
    const char *text = source ? source : "";
    bool ok = idm_buf_append(&input, "IDM-TOP-SOURCE-v1") &&
              idm_buf_put_str(&input, label, strlen(label)) &&
              idm_buf_put_u64(&input, (uint64_t)strlen(text)) &&
              idm_buf_append(&input, text);
    if (!ok) {
        idm_buf_destroy(&input);
        return idm_error_oom(err, idm_span_unknown(file));
    }
    idm_sha256(input.data ? input.data : "", input.len, trace->source_hash);
    idm_buf_destroy(&input);
    trace->cacheable = true;
    for (size_t i = 0; i < ctx->dep_count; i++) {
        if (!expand_trace_add_dep(trace, &ctx->deps[i], err)) return false;
    }
    return idm_expand_trace_add_reads(trace, reads, err);
}

static bool source_line_range(const char *source, unsigned line, size_t *out_start, size_t *out_end) {
    size_t len = strlen(source);
    size_t pos = 0;
    unsigned at = 1;
    while (at < line && pos < len) {
        if (source[pos] == '\n') at++;
        pos++;
    }
    if (at != line) return false;
    size_t end = pos;
    while (end < len && source[end] != '\n') end++;
    *out_start = pos;
    *out_end = end;
    return true;
}

bool idm_expand_explain_source(IdmRuntime *rt, const char *file, const char *source, unsigned line, unsigned column, bool json, IdmBuffer *out, IdmError *err) {
    ExpandContext ctx;
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, idm_span_unknown(file));
        ctx_destroy(&ctx);
        return false;
    }
    unsigned char unit_hash[32];
    idm_sha256(source ? source : "", source ? strlen(source) : 0u, unit_hash);
    ctx_set_unit(&ctx, file ? file : "<program>", unit_hash);
    IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(rt, err);
    if (!phase_runtime_env) {
        ctx_destroy(&ctx);
        return false;
    }
    ctx.phase_env = idm_phase_env_create(rt, phase_runtime_env);
    if (!ctx.phase_env) {
        idm_error_oom(err, idm_span_unknown(NULL));
        ctx_destroy(&ctx);
        return false;
    }
    ctx.runner = &ctx.local_runner;
    ctx.record_edges = true;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    IdmCore *core = NULL;
    bool ok = expand_source_core(&ctx, file, source, &core, err);
    hooks_restore(rt, &saved);
    idm_core_free(core);
    size_t line_start = 0;
    size_t line_end = 0;
    bool have_line = false;
    if (ok && line != 0) {
        have_line = source_line_range(source, line, &line_start, &line_end);
        if (!have_line) ok = idm_error_set(err, idm_span_unknown(file), "line %u is past the end of '%s'", line, file ? file : "<program>");
    }
    for (size_t i = 0; ok && i < ctx.edge_count; i++) {
        const ExpandEdge *edge = &ctx.edges[i];
        if (have_line) {
            if (edge->span.end <= edge->span.start) {
                if (edge->span.line != line) continue;
                if (column != 0 && edge->span.column != column) continue;
            } else if (column != 0) {
                size_t qoff = line_start + column - 1u;
                if (qoff < edge->span.start || qoff >= edge->span.end) continue;
            } else if (edge->span.end <= line_start || edge->span.start > line_end) {
                continue;
            }
        }
        IdmValue entry = idm_nil();
        if (!expand_edge_value(&ctx, rt, edge, &entry, err)) {
            ok = false;
            break;
        }
        bool wrote = json ? idm_value_write_json(out, entry) : idm_value_write(out, entry);
        if (!wrote || !idm_buf_append_char(out, '\n')) ok = idm_error_oom(err, idm_span_unknown(file));
    }
    ctx_destroy(&ctx);
    return ok;
}

bool idm_expand_reader_artifact_string(IdmRuntime *rt, const IdmReaderArtifact *artifact, const char *file, const char *source, IdmCore **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.reader_artifact_string");
    IdmSyntax *syntax = NULL;
    if (!idm_reader_read_artifact_string(artifact, file, source, 0, &syntax, NULL, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    bool ok = idm_expand_syntax(rt, syntax, out, err);
    idm_syn_free(syntax);
    idm_profile_scope_end(&prof);
    return ok;
}

static bool expand_source_string_traced_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmExpandTrace *trace, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "expand.source_string");
    if (trace) memset(trace, 0, sizeof(*trace));
    ExpandContext ctx;
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, idm_span_unknown(file));
        ctx_destroy(&ctx);
        idm_profile_scope_end(&prof);
        return false;
    }
    unsigned char unit_hash[32];
    idm_sha256(source ? source : "", source ? strlen(source) : 0u, unit_hash);
    ctx_set_unit(&ctx, file ? file : "<program>", unit_hash);
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
    IdmPhaseReads reads;
    memset(&reads, 0, sizeof(reads));
    IdmPhaseReads *old_reads = rt->phase_reads;
    if (trace) rt->phase_reads = &reads;
    SavedHooks saved;
    hooks_install(rt, &ctx, &saved);
    IdmCore *core = NULL;
    bool expanded = expand_source_core(&ctx, file, source, &core, err);
    hooks_restore(rt, &saved);
    if (trace) rt->phase_reads = old_reads;
    if (expanded && trace && reads.failed) {
        idm_error_oom(err, idm_span_unknown(file));
        expanded = false;
    }
    if (expanded && trace && !expand_trace_finish(trace, file, source, &ctx, &reads, err)) expanded = false;
    idm_phase_reads_destroy(&reads);
    ctx_destroy(&ctx);
    if (!expanded) {
        idm_core_free(core);
        idm_expand_trace_destroy(trace);
        idm_profile_scope_end(&prof);
        return false;
    }
    *out = core;
    idm_profile_scope_end(&prof);
    return true;
}

static bool idm_expand_source_string_with_runner(IdmRuntime *rt, const char *file, const char *source, IdmMacroRunner *runner, IdmCore **out, IdmError *err) {
    return expand_source_string_traced_with_runner(rt, file, source, runner, out, NULL, err);
}

bool idm_expand_source_string_traced(IdmRuntime *rt, const char *file, const char *source, IdmCore **out, IdmExpandTrace *trace, IdmError *err) {
    return expand_source_string_traced_with_runner(rt, file, source, NULL, out, trace, err);
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
    if (!ctx_init(&ctx, rt)) {
        idm_error_oom(err, idm_span_unknown("<dump-surface>"));
        idm_syn_free(syntax);
        ctx_destroy(&ctx);
        return false;
    }
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
    bool ok = ctx_activate_kernel(&ctx, err);
    if (ok) {
        core = expand_program_root(&ctx, syntax, err);
        ok = core && !(err && err->present);
    }
    if (ok) {
        ok = append_surface_dump_line(&ctx, "operators", out, err) &&
             append_surface_dump_line(&ctx, "macros", out, err) &&
             append_surface_dump_line(&ctx, "protocols", out, err) &&
             append_surface_dump_line(&ctx, "core-form", out, err) &&
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
