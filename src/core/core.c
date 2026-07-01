#include "idiom/core.h"

#include <stdlib.h>
#include <string.h>

static IdmCore *core_alloc(IdmCoreKind kind, IdmSpan span) {
    IdmCore *core = calloc(1u, sizeof(*core));
    if (!core) return NULL;
    core->kind = kind;
    core->span = span;
    core->local_celled = true;
    return core;
}

static bool append_arity(IdmBuffer *buf, const IdmArity *arity) {
    return idm_buf_append_char(buf, '/') && idm_arity_describe(buf, arity);
}

static const char *core_symbol_text(const IdmSymbol *symbol) {
    return symbol ? idm_symbol_text(symbol) : NULL;
}

IdmCore *idm_core_literal(IdmValue value, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LITERAL, span);
    if (!core) return NULL;
    core->as.literal = value;
    return core;
}

static bool core_slot_init(IdmCoreSlot *slot_ref, const char *name, uint32_t slot) {
    slot_ref->name = idm_strdup(name ? name : "<unnamed>");
    if (!slot_ref->name) return false;
    slot_ref->slot = slot;
    return true;
}

IdmCore *idm_core_arg_ref(const char *name, uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_ARG_REF, span);
    if (!core) return NULL;
    if (!core_slot_init(&core->as.slot_ref, name, slot)) { free(core); return NULL; }
    return core;
}

IdmCore *idm_core_local_ref(const char *name, uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LOCAL_REF, span);
    if (!core) return NULL;
    if (!core_slot_init(&core->as.slot_ref, name, slot)) { free(core); return NULL; }
    return core;
}

IdmCore *idm_core_capture_ref(const char *name, uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_CAPTURE_REF, span);
    if (!core) return NULL;
    if (!core_slot_init(&core->as.slot_ref, name, slot)) { free(core); return NULL; }
    return core;
}

bool idm_core_ref_set_contract(IdmCore *core, const IdmCallableContract *contract) {
    if (!core || !contract) return true;
    switch (core->kind) {
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
            if (core->as.slot_ref.has_contract) {
                idm_callable_contract_destroy(&core->as.slot_ref.contract);
                core->as.slot_ref.has_contract = false;
            }
            if (!idm_callable_contract_copy(&core->as.slot_ref.contract, contract)) return false;
            core->as.slot_ref.has_contract = true;
            return true;
        case IDM_CORE_PACKAGE_REF:
            if (core->as.package_ref.has_contract) {
                idm_callable_contract_destroy(&core->as.package_ref.contract);
                core->as.package_ref.has_contract = false;
            }
            if (!idm_callable_contract_copy(&core->as.package_ref.contract, contract)) return false;
            core->as.package_ref.has_contract = true;
            return true;
        default:
            return true;
    }
}

IdmCore *idm_core_call(IdmCore *callee, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_CALL, span);
    if (!core) return NULL;
    core->as.call.callee = callee;
    return core;
}

bool idm_core_call_add_arg(IdmCore *call, IdmCore *arg) {
    if (!call || call->kind != IDM_CORE_CALL || !arg) return false;
    if (call->as.call.arg_count == call->as.call.arg_cap) {
        if (!idm_grow((void **)&call->as.call.args, &call->as.call.arg_cap, sizeof(*call->as.call.args), 4u, call->as.call.arg_count + 1u)) return false;
    }
    call->as.call.args[call->as.call.arg_count++] = arg;
    return true;
}

IdmCore *idm_core_list_cons(IdmCore *head, IdmCore *tail, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LIST_CONS, span);
    if (!core) return NULL;
    core->as.list_pair.head = head;
    core->as.list_pair.tail = tail;
    return core;
}

IdmCore *idm_core_list_append(IdmCore *head, IdmCore *tail, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LIST_APPEND, span);
    if (!core) return NULL;
    core->as.list_pair.head = head;
    core->as.list_pair.tail = tail;
    return core;
}

IdmCore *idm_core_value_sequence(IdmValueSequenceKind kind, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_VALUE_SEQUENCE, span);
    if (!core) return NULL;
    core->as.value_sequence.kind = kind;
    return core;
}

bool idm_core_value_sequence_add(IdmCore *core, IdmCore *item) {
    if (!core || core->kind != IDM_CORE_VALUE_SEQUENCE || !item) return false;
    if (core->as.value_sequence.count == core->as.value_sequence.cap) {
        if (!idm_grow((void **)&core->as.value_sequence.items, &core->as.value_sequence.cap, sizeof(*core->as.value_sequence.items), 4u, core->as.value_sequence.count + 1u)) return false;
    }
    core->as.value_sequence.items[core->as.value_sequence.count++] = item;
    return true;
}

IdmCore *idm_core_syntax_build(IdmSyntaxBuildKind kind, IdmCore *ctx, IdmCore *payload, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_SYNTAX_BUILD, span);
    if (!core) return NULL;
    core->as.syntax_build.kind = kind;
    core->as.syntax_build.ctx = ctx;
    core->as.syntax_build.payload = payload;
    return core;
}

IdmCore *idm_core_string_concat(IdmSpan span) {
    return core_alloc(IDM_CORE_STRING_CONCAT, span);
}

bool idm_core_string_concat_add(IdmCore *core, IdmCore *item) {
    if (!core || core->kind != IDM_CORE_STRING_CONCAT || !item) return false;
    if (core->as.string_concat.count == core->as.string_concat.cap) {
        if (!idm_grow((void **)&core->as.string_concat.items, &core->as.string_concat.cap, sizeof(*core->as.string_concat.items), 4u, core->as.string_concat.count + 1u)) return false;
    }
    core->as.string_concat.items[core->as.string_concat.count++] = item;
    return true;
}

IdmCore *idm_core_cond(IdmCore *cond, IdmCore *then_branch, IdmCore *else_branch, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_COND, span);
    if (!core) return NULL;
    core->as.cond_expr.cond = cond;
    core->as.cond_expr.then_branch = then_branch;
    core->as.cond_expr.else_branch = else_branch;
    return core;
}

IdmCore *idm_core_match(IdmSpan span) {
    return core_alloc(IDM_CORE_MATCH, span);
}

bool idm_core_match_add_scrutinee(IdmCore *match, IdmCore *scrutinee) {
    if (!match || match->kind != IDM_CORE_MATCH || !scrutinee) return false;
    if (match->as.match_expr.scrutinee_count == match->as.match_expr.scrutinee_cap) {
        if (!idm_grow((void **)&match->as.match_expr.scrutinees, &match->as.match_expr.scrutinee_cap, sizeof(*match->as.match_expr.scrutinees), 2u, match->as.match_expr.scrutinee_count + 1u)) return false;
    }
    match->as.match_expr.scrutinees[match->as.match_expr.scrutinee_count++] = scrutinee;
    return true;
}

bool idm_core_match_add_capture(IdmCore *match, IdmCaptureKind kind, const char *name, uint32_t index) {
    if (!match || match->kind != IDM_CORE_MATCH) return false;
    for (size_t i = 0; i < match->as.match_expr.capture_count; i++) {
        if (match->as.match_expr.captures[i].kind == kind && match->as.match_expr.captures[i].index == index) return true;
    }
    if (match->as.match_expr.capture_count == match->as.match_expr.capture_cap) {
        if (!idm_grow((void **)&match->as.match_expr.captures, &match->as.match_expr.capture_cap, sizeof(*match->as.match_expr.captures), 4u, match->as.match_expr.capture_count + 1u)) return false;
    }
    IdmCapture *capture = &match->as.match_expr.captures[match->as.match_expr.capture_count];
    capture->name = idm_strdup(name ? name : "<capture>");
    if (!capture->name) return false;
    capture->kind = kind;
    capture->index = index;
    capture->celled = false;
    match->as.match_expr.capture_count++;
    return true;
}

bool idm_core_match_add_clause_take(IdmCore *match, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body) {
    if (!match || match->kind != IDM_CORE_MATCH || !body) return false;
    if (match->as.match_expr.count == match->as.match_expr.cap) {
        if (!idm_grow((void **)&match->as.match_expr.clauses, &match->as.match_expr.cap, sizeof(*match->as.match_expr.clauses), 4u, match->as.match_expr.count + 1u)) return false;
    }
    IdmFnClause *clause = &match->as.match_expr.clauses[match->as.match_expr.count++];
    memset(clause, 0, sizeof(*clause));
    clause->arity = arity;
    clause->call_arity = idm_arity_exact(arity);
    clause->param_patterns = patterns;
    clause->pattern_count = pattern_count;
    clause->pattern_locals = locals;
    clause->pattern_local_count = local_count;
    clause->guard = guard;
    clause->body = body;
    return true;
}

IdmCore *idm_core_do(IdmSpan span) {
    return core_alloc(IDM_CORE_DO, span);
}

bool idm_core_do_add(IdmCore *do_expr, IdmCore *item) {
    if (!do_expr || do_expr->kind != IDM_CORE_DO || !item) return false;
    if (do_expr->as.do_expr.count == do_expr->as.do_expr.cap) {
        if (!idm_grow((void **)&do_expr->as.do_expr.items, &do_expr->as.do_expr.cap, sizeof(*do_expr->as.do_expr.items), 4u, do_expr->as.do_expr.count + 1u)) return false;
    }
    do_expr->as.do_expr.items[do_expr->as.do_expr.count++] = item;
    return true;
}

IdmCore *idm_core_bind_local(const char *name, uint32_t slot, IdmCore *value, IdmCore *body, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_BIND_LOCAL, span);
    if (!core) return NULL;
    core->as.bind_local.name = idm_strdup(name ? name : "<local>");
    if (!core->as.bind_local.name) { free(core); return NULL; }
    core->as.bind_local.slot = slot;
    core->as.bind_local.value = value;
    core->as.bind_local.body = body;
    return core;
}

IdmCore *idm_core_fn(const char *name, uint32_t arity, IdmCore *body, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_FN, span);
    if (!core) return NULL;
    core->as.fn.name = idm_strdup(name ? name : "<lambda>");
    if (!core->as.fn.name) { free(core); return NULL; }
    core->as.fn.arity = arity;
    core->as.fn.body = body;
    return core;
}

bool idm_core_fn_add_capture(IdmCore *fn, IdmCaptureKind kind, const char *name, uint32_t index) {
    if (!fn || fn->kind != IDM_CORE_FN) return false;
    if (fn->as.fn.capture_count == fn->as.fn.capture_cap) {
        if (!idm_grow((void **)&fn->as.fn.captures, &fn->as.fn.capture_cap, sizeof(*fn->as.fn.captures), 4u, fn->as.fn.capture_count + 1u)) return false;
    }
    IdmCapture *capture = &fn->as.fn.captures[fn->as.fn.capture_count];
    capture->name = idm_strdup(name ? name : "<capture>");
    if (!capture->name) return false;
    capture->kind = kind;
    capture->index = index;
    capture->celled = false;
    fn->as.fn.capture_count++;
    return true;
}

bool idm_core_fn_set_param_patterns_take(IdmCore *fn, IdmPattern **patterns, uint32_t pattern_count) {
    if (!fn || fn->kind != IDM_CORE_FN) return false;
    for (uint32_t i = 0; i < fn->as.fn.pattern_count; i++) idm_pat_free(fn->as.fn.param_patterns[i]);
    free(fn->as.fn.param_patterns);
    fn->as.fn.param_patterns = patterns;
    fn->as.fn.pattern_count = pattern_count;
    return true;
}

static void pattern_locals_free(IdmPatternLocal *locals, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free(locals[i].name);
    free(locals);
}

bool idm_core_fn_set_pattern_locals_take(IdmCore *fn, IdmPatternLocal *locals, uint32_t local_count) {
    if (!fn || fn->kind != IDM_CORE_FN) return false;
    pattern_locals_free(fn->as.fn.pattern_locals, fn->as.fn.pattern_local_count);
    fn->as.fn.pattern_locals = locals;
    fn->as.fn.pattern_local_count = local_count;
    return true;
}

bool idm_core_fn_set_guard_take(IdmCore *fn, IdmCore *guard) {
    if (!fn || fn->kind != IDM_CORE_FN) return false;
    idm_core_free(fn->as.fn.guard);
    fn->as.fn.guard = guard;
    return true;
}

IdmCore *idm_core_fn_multi(const char *name, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_FN_MULTI, span);
    if (!core) return NULL;
    core->as.fn_multi.name = idm_strdup(name ? name : "<multi>");
    if (!core->as.fn_multi.name) { free(core); return NULL; }
    return core;
}

static IdmFnClause *fn_multi_append_clause(IdmCore *multi) {
    if (!multi || multi->kind != IDM_CORE_FN_MULTI) return NULL;
    if (multi->as.fn_multi.count == multi->as.fn_multi.cap) {
        if (!idm_grow((void **)&multi->as.fn_multi.clauses, &multi->as.fn_multi.cap, sizeof(*multi->as.fn_multi.clauses), 4u, multi->as.fn_multi.count + 1u)) return NULL;
    }
    IdmFnClause *clause = &multi->as.fn_multi.clauses[multi->as.fn_multi.count++];
    memset(clause, 0, sizeof(*clause));
    return clause;
}

bool idm_core_fn_multi_add_capture(IdmCore *multi, IdmCaptureKind kind, const char *name, uint32_t index) {
    if (!multi || multi->kind != IDM_CORE_FN_MULTI) return false;
    for (size_t i = 0; i < multi->as.fn_multi.capture_count; i++) {
        if (multi->as.fn_multi.captures[i].kind == kind && multi->as.fn_multi.captures[i].index == index) return true;
    }
    if (multi->as.fn_multi.capture_count == multi->as.fn_multi.capture_cap) {
        if (!idm_grow((void **)&multi->as.fn_multi.captures, &multi->as.fn_multi.capture_cap, sizeof(*multi->as.fn_multi.captures), 4u, multi->as.fn_multi.capture_count + 1u)) return false;
    }
    IdmCapture *capture = &multi->as.fn_multi.captures[multi->as.fn_multi.capture_count];
    capture->name = idm_strdup(name ? name : "<capture>");
    if (!capture->name) return false;
    capture->kind = kind;
    capture->index = index;
    capture->celled = false;
    multi->as.fn_multi.capture_count++;
    return true;
}

bool idm_core_fn_multi_add_clause_take(IdmCore *multi, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body) {
    if (!multi || multi->kind != IDM_CORE_FN_MULTI || !body) return false;
    IdmFnClause *clause = fn_multi_append_clause(multi);
    if (!clause) return false;
    clause->arity = arity;
    clause->call_arity = idm_arity_exact(arity);
    clause->param_patterns = patterns;
    clause->pattern_count = pattern_count;
    clause->pattern_locals = locals;
    clause->pattern_local_count = local_count;
    clause->guard = guard;
    clause->body = body;
    return true;
}

IdmCore *idm_core_primitive_backed_fn(const char *name, IdmPrimitive primitive, IdmArity arity, IdmSpan span) {
    IdmCore *fn = idm_core_fn_multi(name ? name : idm_primitive_name(primitive), span);
    if (!fn) return NULL;
    IdmFnClause *clause = fn_multi_append_clause(fn);
    if (!clause) {
        idm_core_free(fn);
        return NULL;
    }
    clause->arity = arity.min;
    clause->call_arity = arity;
    clause->primitive_backed = true;
    clause->primitive = primitive;
    return fn;
}

IdmCore *idm_core_letrec(IdmSpan span) {
    return core_alloc(IDM_CORE_LETREC, span);
}

bool idm_core_letrec_add_env_fill(IdmCore *letrec, const char *name, IdmValue env_key, uint32_t slot, IdmCore *value, bool fill_existing) {
    if (!letrec || letrec->kind != IDM_CORE_LETREC || !value) return false;
    if (letrec->as.letrec.count == letrec->as.letrec.cap) {
        if (!idm_grow((void **)&letrec->as.letrec.bindings, &letrec->as.letrec.cap, sizeof(*letrec->as.letrec.bindings), 4u, letrec->as.letrec.count + 1u)) return false;
    }
    IdmLetRecBinding *binding = &letrec->as.letrec.bindings[letrec->as.letrec.count];
    binding->name = idm_strdup(name);
    if (!binding->name) {
        free(binding->name);
        return false;
    }
    binding->env_key = env_key;
    binding->has_env_key = true;
    binding->slot = slot;
    binding->value = value;
    binding->fill_existing = fill_existing;
    letrec->as.letrec.count++;
    return true;
}

bool idm_core_letrec_add_fill(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value, bool fill_existing) {
    if (!letrec || letrec->kind != IDM_CORE_LETREC || !value) return false;
    if (letrec->as.letrec.count == letrec->as.letrec.cap) {
        if (!idm_grow((void **)&letrec->as.letrec.bindings, &letrec->as.letrec.cap, sizeof(*letrec->as.letrec.bindings), 4u, letrec->as.letrec.count + 1u)) return false;
    }
    IdmLetRecBinding *binding = &letrec->as.letrec.bindings[letrec->as.letrec.count];
    binding->name = idm_strdup(name);
    if (!binding->name) return false;
    binding->env_key = idm_nil();
    binding->has_env_key = false;
    binding->slot = slot;
    binding->value = value;
    binding->fill_existing = fill_existing;
    letrec->as.letrec.count++;
    return true;
}

bool idm_core_letrec_add(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value) {
    return idm_core_letrec_add_fill(letrec, name, slot, value, false);
}

bool idm_core_letrec_set_body(IdmCore *letrec, IdmCore *body) {
    if (!letrec || letrec->kind != IDM_CORE_LETREC || !body) return false;
    letrec->as.letrec.body = body;
    return true;
}

void idm_core_letrec_set_env(IdmCore *letrec) {
    if (letrec && letrec->kind == IDM_CORE_LETREC) letrec->as.letrec.env = true;
}

void idm_core_letrec_set_fill_only(IdmCore *letrec) {
    if (letrec && letrec->kind == IDM_CORE_LETREC) letrec->as.letrec.fill_only = true;
}

IdmCore *idm_core_env_ref(const char *name, uint32_t id, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_ENV_REF, span);
    if (!core) return NULL;
    if (!core_slot_init(&core->as.slot_ref, name, id)) { free(core); return NULL; }
    return core;
}

IdmCore *idm_core_package_ref(const char *name, IdmValue env_key, uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_PACKAGE_REF, span);
    if (!core) return NULL;
    core->as.package_ref.name = idm_strdup(name ? name : "<unnamed>");
    if (!core->as.package_ref.name) { free(core); return NULL; }
    core->as.package_ref.env_key = env_key;
    core->as.package_ref.slot = slot;
    return core;
}

IdmCore *idm_core_receive(IdmCore *receiver, IdmCore *timeout, IdmCore *timeout_body, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_RECEIVE, span);
    if (!core) return NULL;
    core->as.receive.receiver = receiver;
    core->as.receive.timeout = timeout;
    core->as.receive.timeout_body = timeout_body;
    return core;
}

IdmCore *idm_core_guard(IdmCore *body, IdmCore *handler, uint32_t rescue_slot, IdmCore *cleanup, uint32_t ensure_slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_GUARD, span);
    if (!core) return NULL;
    core->as.guard.body = body;
    core->as.guard.handler = handler;
    core->as.guard.cleanup = cleanup;
    core->as.guard.rescue_slot = rescue_slot;
    core->as.guard.ensure_slot = ensure_slot;
    return core;
}

IdmCore *idm_core_use_package(IdmValue env_key, IdmBytecodeModule *module, bool module_owned, uint32_t init_fn, IdmCore *cont, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_USE_PACKAGE, span);
    if (!core) return NULL;
    core->as.use_package.env_key = env_key;
    core->as.use_package.module = module;
    core->as.use_package.module_owned = module_owned;
    core->as.use_package.init_fn = init_fn;
    core->as.use_package.cont = cont;
    return core;
}

IdmCore *idm_core_record_construct(IdmSymbol *type, IdmSpan span) {
    if (!type) return NULL;
    IdmCore *core = core_alloc(IDM_CORE_RECORD_CONSTRUCT, span);
    if (!core) return NULL;
    core->as.record_construct.type = type;
    return core;
}

bool idm_core_record_construct_add(IdmCore *core, IdmSymbol *field, const IdmTypeTerm *contract, IdmCore *value) {
    if (!core || core->kind != IDM_CORE_RECORD_CONSTRUCT || !field || !value) return false;
    if (core->as.record_construct.count == core->as.record_construct.cap) {
        IdmGrowItem items[] = {
            { .base = (void **)&core->as.record_construct.field_names, .elem_size = sizeof(*core->as.record_construct.field_names) },
            { .base = (void **)&core->as.record_construct.field_contracts, .elem_size = sizeof(*core->as.record_construct.field_contracts) },
            { .base = (void **)&core->as.record_construct.field_has_contracts, .elem_size = sizeof(*core->as.record_construct.field_has_contracts) },
            { .base = (void **)&core->as.record_construct.field_values, .elem_size = sizeof(*core->as.record_construct.field_values) },
        };
        if (!idm_growv(items, 4u, &core->as.record_construct.cap, 4u, core->as.record_construct.count + 1u)) return false;
    }
    size_t index = core->as.record_construct.count;
    core->as.record_construct.field_names[index] = field;
    memset(&core->as.record_construct.field_contracts[index], 0, sizeof(core->as.record_construct.field_contracts[index]));
    core->as.record_construct.field_has_contracts[index] = false;
    if (contract) {
        if (!idm_type_term_copy(&core->as.record_construct.field_contracts[index], contract)) return false;
        core->as.record_construct.field_has_contracts[index] = true;
    }
    core->as.record_construct.field_values[index] = value;
    core->as.record_construct.count++;
    return true;
}

IdmCore *idm_core_record_field(IdmCore *receiver, IdmSymbol *type, IdmSymbol *field, uint32_t field_index, IdmSpan span) {
    if (!receiver || !type || !field) return NULL;
    IdmCore *core = core_alloc(IDM_CORE_RECORD_FIELD, span);
    if (!core) return NULL;
    core->as.record_field.type = type;
    core->as.record_field.field = field;
    core->as.record_field.receiver = receiver;
    core->as.record_field.field_index = field_index;
    return core;
}

IdmCore *idm_core_record_is(IdmCore *value, IdmSymbol *type, IdmSpan span) {
    if (!value || !type) return NULL;
    IdmCore *core = core_alloc(IDM_CORE_RECORD_IS, span);
    if (!core) return NULL;
    core->as.record_is.type = type;
    core->as.record_is.value = value;
    return core;
}

void idm_core_free(IdmCore *core) {
    if (!core) return;
    switch (core->kind) {
        case IDM_CORE_GUARD:
            idm_core_free(core->as.guard.body);
            idm_core_free(core->as.guard.handler);
            idm_core_free(core->as.guard.cleanup);
            break;
        case IDM_CORE_USE_PACKAGE:
            idm_core_free(core->as.use_package.cont);
            if (core->as.use_package.module_owned && core->as.use_package.module) { idm_bc_destroy(core->as.use_package.module); free(core->as.use_package.module); }
            break;
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (core->as.record_construct.field_has_contracts[i]) idm_type_term_destroy(&core->as.record_construct.field_contracts[i]);
                idm_core_free(core->as.record_construct.field_values[i]);
            }
            free(core->as.record_construct.field_names);
            free(core->as.record_construct.field_contracts);
            free(core->as.record_construct.field_has_contracts);
            free(core->as.record_construct.field_values);
            break;
        case IDM_CORE_RECORD_FIELD:
            idm_core_free(core->as.record_field.receiver);
            break;
        case IDM_CORE_RECORD_IS:
            idm_core_free(core->as.record_is.value);
            break;
        case IDM_CORE_CALL:
            idm_core_free(core->as.call.callee);
            for (size_t i = 0; i < core->as.call.arg_count; i++) idm_core_free(core->as.call.args[i]);
            free(core->as.call.args);
            break;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            idm_core_free(core->as.list_pair.head);
            idm_core_free(core->as.list_pair.tail);
            break;
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) idm_core_free(core->as.value_sequence.items[i]);
            free(core->as.value_sequence.items);
            break;
        case IDM_CORE_SYNTAX_BUILD:
            idm_core_free(core->as.syntax_build.ctx);
            idm_core_free(core->as.syntax_build.payload);
            break;
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) idm_core_free(core->as.string_concat.items[i]);
            free(core->as.string_concat.items);
            break;
        case IDM_CORE_COND:
            idm_core_free(core->as.cond_expr.cond);
            idm_core_free(core->as.cond_expr.then_branch);
            idm_core_free(core->as.cond_expr.else_branch);
            break;
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) idm_core_free(core->as.match_expr.scrutinees[i]);
            free(core->as.match_expr.scrutinees);
            for (size_t i = 0; i < core->as.match_expr.capture_count; i++) free(core->as.match_expr.captures[i].name);
            free(core->as.match_expr.captures);
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                for (uint32_t p = 0; p < core->as.match_expr.clauses[i].pattern_count; p++) idm_pat_free(core->as.match_expr.clauses[i].param_patterns[p]);
                free(core->as.match_expr.clauses[i].param_patterns);
                pattern_locals_free(core->as.match_expr.clauses[i].pattern_locals, core->as.match_expr.clauses[i].pattern_local_count);
                idm_core_free(core->as.match_expr.clauses[i].guard);
                idm_core_free(core->as.match_expr.clauses[i].body);
            }
            free(core->as.match_expr.clauses);
            break;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) idm_core_free(core->as.do_expr.items[i]);
            free(core->as.do_expr.items);
            break;
        case IDM_CORE_BIND_LOCAL:
            free(core->as.bind_local.name);
            idm_core_free(core->as.bind_local.value);
            idm_core_free(core->as.bind_local.body);
            break;
        case IDM_CORE_FN:
            free(core->as.fn.name);
            for (size_t i = 0; i < core->as.fn.capture_count; i++) free(core->as.fn.captures[i].name);
            free(core->as.fn.captures);
            for (uint32_t i = 0; i < core->as.fn.pattern_count; i++) idm_pat_free(core->as.fn.param_patterns[i]);
            free(core->as.fn.param_patterns);
            pattern_locals_free(core->as.fn.pattern_locals, core->as.fn.pattern_local_count);
            idm_core_free(core->as.fn.guard);
            idm_core_free(core->as.fn.body);
            break;
        case IDM_CORE_FN_MULTI:
            free(core->as.fn_multi.name);
            for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) free(core->as.fn_multi.captures[i].name);
            free(core->as.fn_multi.captures);
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                for (uint32_t p = 0; p < core->as.fn_multi.clauses[i].pattern_count; p++) idm_pat_free(core->as.fn_multi.clauses[i].param_patterns[p]);
                free(core->as.fn_multi.clauses[i].param_patterns);
                pattern_locals_free(core->as.fn_multi.clauses[i].pattern_locals, core->as.fn_multi.clauses[i].pattern_local_count);
                idm_core_free(core->as.fn_multi.clauses[i].guard);
                idm_core_free(core->as.fn_multi.clauses[i].body);
            }
            free(core->as.fn_multi.clauses);
            break;
        case IDM_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                free(core->as.letrec.bindings[i].name);
                idm_core_free(core->as.letrec.bindings[i].value);
            }
            free(core->as.letrec.bindings);
            idm_core_free(core->as.letrec.body);
            break;
        case IDM_CORE_RECEIVE:
            idm_core_free(core->as.receive.receiver);
            idm_core_free(core->as.receive.timeout);
            idm_core_free(core->as.receive.timeout_body);
            break;
        case IDM_CORE_ENV_REF:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
            if (core->as.slot_ref.has_contract) idm_callable_contract_destroy(&core->as.slot_ref.contract);
            free(core->as.slot_ref.name);
            break;
        case IDM_CORE_PACKAGE_REF:
            if (core->as.package_ref.has_contract) idm_callable_contract_destroy(&core->as.package_ref.contract);
            free(core->as.package_ref.name);
            break;
        case IDM_CORE_LITERAL:
            break;
    }
    free(core);
}

static bool normalize_core(IdmRuntime *rt, IdmCore **slot, IdmError *err);

static bool replace_core(IdmCore **slot, IdmCore *replacement, IdmError *err, IdmSpan span) {
    if (!replacement) return idm_error_oom(err, span);
    IdmCore *old = *slot;
    *slot = replacement;
    idm_core_free(old);
    return true;
}

static bool normalize_call(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = *slot;
    if (!normalize_core(rt, &core->as.call.callee, err)) return false;
    for (size_t i = 0; i < core->as.call.arg_count; i++) {
        if (!normalize_core(rt, &core->as.call.args[i], err)) return false;
    }
    return true;
}

static bool normalize_cond(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = *slot;
    if (!normalize_core(rt, &core->as.cond_expr.cond, err) ||
        !normalize_core(rt, &core->as.cond_expr.then_branch, err) ||
        !normalize_core(rt, &core->as.cond_expr.else_branch, err)) {
        return false;
    }
    if (core->as.cond_expr.cond && core->as.cond_expr.cond->kind == IDM_CORE_LITERAL) {
        bool take_then = idm_value_ok(core->as.cond_expr.cond->as.literal);
        IdmCore *replacement = take_then ? core->as.cond_expr.then_branch : core->as.cond_expr.else_branch;
        if (take_then) core->as.cond_expr.then_branch = NULL;
        else core->as.cond_expr.else_branch = NULL;
        return replace_core(slot, replacement, err, core->span);
    }
    return true;
}

static bool core_statement_discardable(const IdmCore *core) {
    if (!core) return true;
    switch (core->kind) {
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
        case IDM_CORE_RECORD_IS:
            return core_statement_discardable(core->as.record_is.value);
        case IDM_CORE_RECORD_FIELD:
            return core_statement_discardable(core->as.record_field.receiver);
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return core_statement_discardable(core->as.list_pair.head) && core_statement_discardable(core->as.list_pair.tail);
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!core_statement_discardable(core->as.value_sequence.items[i])) return false;
            }
            return true;
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!core_statement_discardable(core->as.string_concat.items[i])) return false;
            }
            return true;
        case IDM_CORE_COND:
            return core_statement_discardable(core->as.cond_expr.cond) &&
                   core_statement_discardable(core->as.cond_expr.then_branch) &&
                   core_statement_discardable(core->as.cond_expr.else_branch);
        default:
            return false;
    }
}

static bool normalize_do(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = *slot;
    for (size_t i = 0; i < core->as.do_expr.count; i++) {
        if (!normalize_core(rt, &core->as.do_expr.items[i], err)) return false;
    }
    if (core->as.do_expr.count > 1) {
        size_t keep = 0;
        for (size_t i = 0; i < core->as.do_expr.count; i++) {
            IdmCore *item = core->as.do_expr.items[i];
            if (i + 1u < core->as.do_expr.count && core_statement_discardable(item)) {
                idm_core_free(item);
                continue;
            }
            core->as.do_expr.items[keep++] = item;
        }
        core->as.do_expr.count = keep;
    }
    if (core->as.do_expr.count == 0) {
        IdmCore *nil_lit = idm_core_literal(idm_nil(), core->span);
        if (!nil_lit) return idm_error_oom(err, core->span);
        return replace_core(slot, nil_lit, err, core->span);
    }
    size_t total = 0;
    bool nested = false;
    for (size_t i = 0; i < core->as.do_expr.count; i++) {
        IdmCore *item = core->as.do_expr.items[i];
        if (item && item->kind == IDM_CORE_DO) {
            nested = true;
            total += item->as.do_expr.count;
        } else {
            total++;
        }
    }
    if (nested) {
        IdmCore **items = total == 0 ? NULL : malloc(total * sizeof(*items));
        if (total != 0 && !items) return idm_error_oom(err, core->span);
        size_t out = 0;
        for (size_t i = 0; i < core->as.do_expr.count; i++) {
            IdmCore *item = core->as.do_expr.items[i];
            if (item && item->kind == IDM_CORE_DO) {
                for (size_t j = 0; j < item->as.do_expr.count; j++) items[out++] = item->as.do_expr.items[j];
                free(item->as.do_expr.items);
                item->as.do_expr.items = NULL;
                item->as.do_expr.count = 0;
                item->as.do_expr.cap = 0;
                idm_core_free(item);
            } else {
                items[out++] = item;
            }
        }
        free(core->as.do_expr.items);
        core->as.do_expr.items = items;
        core->as.do_expr.count = total;
        core->as.do_expr.cap = total;
    }
    size_t write = 0;
    for (size_t read = 0; read < core->as.do_expr.count; read++) {
        IdmCore *item = core->as.do_expr.items[read];
        if (read + 1u < core->as.do_expr.count && item && item->kind == IDM_CORE_LITERAL) {
            idm_core_free(item);
            continue;
        }
        core->as.do_expr.items[write++] = item;
    }
    core->as.do_expr.count = write;
    if (core->as.do_expr.count == 1) {
        IdmCore *replacement = core->as.do_expr.items[0];
        free(core->as.do_expr.items);
        core->as.do_expr.items = NULL;
        core->as.do_expr.count = 0;
        core->as.do_expr.cap = 0;
        return replace_core(slot, replacement, err, core->span);
    }
    return true;
}

static bool normalize_core(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = slot ? *slot : NULL;
    if (!core) return true;
    switch (core->kind) {
        case IDM_CORE_CALL:
            return normalize_call(rt, slot, err);
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            return normalize_core(rt, &core->as.list_pair.head, err) &&
                   normalize_core(rt, &core->as.list_pair.tail, err);
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!normalize_core(rt, &core->as.value_sequence.items[i], err)) return false;
            }
            return true;
        case IDM_CORE_SYNTAX_BUILD:
            return normalize_core(rt, &core->as.syntax_build.ctx, err) &&
                   normalize_core(rt, &core->as.syntax_build.payload, err);
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!normalize_core(rt, &core->as.string_concat.items[i], err)) return false;
            }
            return true;
        case IDM_CORE_COND:
            return normalize_cond(rt, slot, err);
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                if (!normalize_core(rt, &core->as.match_expr.scrutinees[i], err)) return false;
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                if (!normalize_core(rt, &core->as.match_expr.clauses[i].guard, err) ||
                    !normalize_core(rt, &core->as.match_expr.clauses[i].body, err)) {
                    return false;
                }
            }
            return true;
        case IDM_CORE_DO:
            return normalize_do(rt, slot, err);
        case IDM_CORE_BIND_LOCAL:
            return normalize_core(rt, &core->as.bind_local.value, err) &&
                   normalize_core(rt, &core->as.bind_local.body, err);
        case IDM_CORE_FN:
            return normalize_core(rt, &core->as.fn.guard, err) &&
                   normalize_core(rt, &core->as.fn.body, err);
        case IDM_CORE_FN_MULTI:
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!normalize_core(rt, &core->as.fn_multi.clauses[i].guard, err) ||
                    !normalize_core(rt, &core->as.fn_multi.clauses[i].body, err)) {
                    return false;
                }
            }
            return true;
        case IDM_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!normalize_core(rt, &core->as.letrec.bindings[i].value, err)) return false;
            }
            return normalize_core(rt, &core->as.letrec.body, err);
        case IDM_CORE_RECEIVE:
            return normalize_core(rt, &core->as.receive.receiver, err) &&
                   normalize_core(rt, &core->as.receive.timeout, err) &&
                   normalize_core(rt, &core->as.receive.timeout_body, err);
        case IDM_CORE_GUARD:
            return normalize_core(rt, &core->as.guard.body, err) &&
                   normalize_core(rt, &core->as.guard.handler, err) &&
                   normalize_core(rt, &core->as.guard.cleanup, err);
        case IDM_CORE_USE_PACKAGE:
            return normalize_core(rt, &core->as.use_package.cont, err);
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (!normalize_core(rt, &core->as.record_construct.field_values[i], err)) return false;
            }
            return true;
        case IDM_CORE_RECORD_FIELD:
            return normalize_core(rt, &core->as.record_field.receiver, err);
        case IDM_CORE_RECORD_IS:
            return normalize_core(rt, &core->as.record_is.value, err);
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
    }
    return true;
}

bool idm_core_normalize(IdmRuntime *rt, IdmCore **core, IdmError *err) {
    return normalize_core(rt, core, err);
}

typedef struct {
    uint32_t *entries;
    uint32_t *guards;
    size_t count;
    const IdmCapture *captures;
    size_t capture_count;
} CompiledCallable;

typedef struct {
    IdmValue env_key;
    uint32_t fn_off;
} LinkedPackage;

typedef struct {
    IdmRuntime *rt;
    LinkedPackage *linked_packages;
    size_t linked_package_count;
    size_t linked_package_cap;
} CompileModuleCtx;

typedef struct {
    IdmRuntime *rt;
    CompileModuleCtx *module_ctx;
    IdmBytecodeModule *module;
    uint32_t function_index;
    uint32_t arity;
    uint32_t local_count;
    uint32_t next_reg;
    uint32_t max_reg;
} CompileCtx;

static bool compile_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err);
static bool emit_op1(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, size_t *out_offset);
static bool emit_op2(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, size_t *out_offset);
static bool emit_op3(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, size_t *out_offset);
static bool emit_op4(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, size_t *out_offset);
static bool emit_op5(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, size_t *out_offset);
static uint32_t ctx_alloc(CompileCtx *ctx);
static uint32_t ctx_alloc_range(CompileCtx *ctx, uint32_t count);
static uint32_t ctx_mark(const CompileCtx *ctx);
static void ctx_reset(CompileCtx *ctx, uint32_t mark);
static uint32_t ctx_local_reg(const CompileCtx *ctx, uint32_t slot);
static bool compile_function_code(CompileModuleCtx *module_ctx, IdmBytecodeModule *module, uint32_t function_index, IdmCore *body, IdmError *err, IdmSpan span, const IdmCapture *captures, size_t capture_count);

static uint32_t core_max_local_plus_one(IdmCore *core) {
    if (!core) return 0;
    switch (core->kind) {
        case IDM_CORE_LOCAL_REF:
            return core->as.slot_ref.slot + 1u;
        case IDM_CORE_ARG_REF:
            return 0;
        case IDM_CORE_CAPTURE_REF:
            return 0;
        case IDM_CORE_CALL: {
            uint32_t max = core_max_local_plus_one(core->as.call.callee);
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.call.args[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND: {
            uint32_t max = core_max_local_plus_one(core->as.list_pair.head);
            uint32_t tail_max = core_max_local_plus_one(core->as.list_pair.tail);
            if (tail_max > max) max = tail_max;
            return max;
        }
        case IDM_CORE_VALUE_SEQUENCE: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.value_sequence.items[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_SYNTAX_BUILD: {
            uint32_t max = core_max_local_plus_one(core->as.syntax_build.ctx);
            uint32_t payload_max = core_max_local_plus_one(core->as.syntax_build.payload);
            if (payload_max > max) max = payload_max;
            return max;
        }
        case IDM_CORE_STRING_CONCAT: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.string_concat.items[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_COND: {
            uint32_t max = core_max_local_plus_one(core->as.cond_expr.cond);
            uint32_t then_max = core_max_local_plus_one(core->as.cond_expr.then_branch);
            uint32_t else_max = core_max_local_plus_one(core->as.cond_expr.else_branch);
            if (then_max > max) max = then_max;
            if (else_max > max) max = else_max;
            return max;
        }
        case IDM_CORE_MATCH: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.match_expr.scrutinees[i]);
                if (child > max) max = child;
            }
            for (size_t i = 0; i < core->as.match_expr.capture_count; i++) {
                if (core->as.match_expr.captures[i].kind != IDM_CAP_LOCAL) continue;
                uint32_t child = core->as.match_expr.captures[i].index + 1u;
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_DO: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.do_expr.items[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_BIND_LOCAL: {
            uint32_t max = core->as.bind_local.slot + 1u;
            uint32_t value_max = core_max_local_plus_one(core->as.bind_local.value);
            uint32_t body_max = core_max_local_plus_one(core->as.bind_local.body);
            if (value_max > max) max = value_max;
            if (body_max > max) max = body_max;
            return max;
        }
        case IDM_CORE_FN: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                if (core->as.fn.captures[i].kind != IDM_CAP_LOCAL) continue;
                uint32_t child = core->as.fn.captures[i].index + 1u;
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_FN_MULTI: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                if (core->as.fn_multi.captures[i].kind != IDM_CAP_LOCAL) continue;
                uint32_t child = core->as.fn_multi.captures[i].index + 1u;
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_LETREC: {
            uint32_t max = core_max_local_plus_one(core->as.letrec.body);
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                uint32_t value_max = core_max_local_plus_one(core->as.letrec.bindings[i].value);
                if (value_max > max) max = value_max;
                if (!core->as.letrec.env) {
                    uint32_t slot = core->as.letrec.bindings[i].slot + 1u;
                    if (slot > max) max = slot;
                }
            }
            return max;
        }
        case IDM_CORE_RECEIVE: {
            uint32_t max = core_max_local_plus_one(core->as.receive.receiver);
            uint32_t t = core_max_local_plus_one(core->as.receive.timeout);
            uint32_t b = core_max_local_plus_one(core->as.receive.timeout_body);
            if (t > max) max = t;
            if (b > max) max = b;
            return max;
        }
        case IDM_CORE_GUARD: {
            uint32_t max = core_max_local_plus_one(core->as.guard.body);
            if (core->as.guard.handler) {
                uint32_t h = core_max_local_plus_one(core->as.guard.handler);
                uint32_t s = core->as.guard.rescue_slot + 1u;
                if (h > max) max = h;
                if (s > max) max = s;
            }
            if (core->as.guard.cleanup) {
                uint32_t c = core_max_local_plus_one(core->as.guard.cleanup);
                uint32_t s = core->as.guard.ensure_slot + 1u;
                if (c > max) max = c;
                if (s > max) max = s;
            }
            return max;
        }
        case IDM_CORE_USE_PACKAGE:
            return core_max_local_plus_one(core->as.use_package.cont);
        case IDM_CORE_RECORD_CONSTRUCT: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.record_construct.field_values[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_RECORD_FIELD:
            return core_max_local_plus_one(core->as.record_field.receiver);
        case IDM_CORE_RECORD_IS:
            return core_max_local_plus_one(core->as.record_is.value);
        case IDM_CORE_LITERAL:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return 0;
    }
    return 0;
}

static bool clone_patterns(IdmPattern **patterns, uint32_t count, IdmPattern ***out) {
    if (count == 0) {
        *out = NULL;
        return true;
    }
    IdmPattern **clones = calloc(count, sizeof(*clones));
    if (!clones) return false;
    for (uint32_t i = 0; i < count; i++) {
        clones[i] = idm_pat_clone(patterns[i]);
        if (!clones[i]) {
            for (uint32_t j = 0; j < i; j++) idm_pat_free(clones[j]);
            free(clones);
            return false;
        }
    }
    *out = clones;
    return true;
}

static bool clone_pattern_locals(IdmPatternLocal *locals, uint32_t count, IdmPatternLocal **out) {
    if (count == 0) {
        *out = NULL;
        return true;
    }
    IdmPatternLocal *clones = calloc(count, sizeof(*clones));
    if (!clones) return false;
    for (uint32_t i = 0; i < count; i++) {
        clones[i].name = idm_strdup(locals[i].name);
        if (!clones[i].name) {
            for (uint32_t j = 0; j < i; j++) free(clones[j].name);
            free(clones);
            return false;
        }
        clones[i].slot = locals[i].slot;
    }
    *out = clones;
    return true;
}

static uint32_t pattern_local_max_plus_one(IdmPatternLocal *locals, uint32_t count) {
    uint32_t max = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t child = locals[i].slot + 1u;
        if (child > max) max = child;
    }
    return max;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static bool make_guard_function_name(const char *name, char **out_name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_buf_appendf(&buf, "%s#guard", name ? name : "<anonymous>")) {
        idm_buf_destroy(&buf);
        return false;
    }
    *out_name = idm_buf_take(&buf);
    return *out_name != NULL;
}

static bool add_guard_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t locals, IdmPatternLocal *pattern_locals, uint32_t pattern_local_count, uint32_t *out_index, IdmError *err, IdmSpan span) {
    char *guard_name = NULL;
    if (!make_guard_function_name(name, &guard_name)) return idm_error_oom(err, span);
    bool ok = idm_bc_add_function(module, guard_name, arity, locals, 0, out_index);
    free(guard_name);
    if (!ok) return idm_error_oom(err, span);
    if (pattern_local_count != 0) {
        IdmPatternLocal *clones = NULL;
        if (!clone_pattern_locals(pattern_locals, pattern_local_count, &clones)) return idm_error_oom(err, span);
        if (!idm_bc_set_function_pattern_locals_take(module, *out_index, clones, pattern_local_count)) {
            pattern_locals_free(clones, pattern_local_count);
            return idm_error_oom(err, span);
        }
    }
    return true;
}

static bool add_compiled_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t locals, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *pattern_locals, uint32_t pattern_local_count, uint32_t *out_index, IdmError *err, IdmSpan span) {
    if (!idm_bc_add_function(module, name, arity, locals, 0, out_index)) return idm_error_oom(err, span);
    if (pattern_count != 0) {
        IdmPattern **clones = NULL;
        if (!clone_patterns(patterns, pattern_count, &clones)) return idm_error_oom(err, span);
        if (!idm_bc_set_function_patterns_take(module, *out_index, clones, pattern_count)) {
            for (uint32_t i = 0; i < pattern_count; i++) idm_pat_free(clones[i]);
            free(clones);
            return idm_error_oom(err, span);
        }
    }
    if (pattern_local_count != 0) {
        IdmPatternLocal *clones = NULL;
        if (!clone_pattern_locals(pattern_locals, pattern_local_count, &clones)) return idm_error_oom(err, span);
        if (!idm_bc_set_function_pattern_locals_take(module, *out_index, clones, pattern_local_count)) {
            for (uint32_t i = 0; i < pattern_local_count; i++) free(clones[i].name);
            free(clones);
            return idm_error_oom(err, span);
        }
    }
    return true;
}

static void collect_celled_slots(IdmCore *core, bool *celled, uint32_t lc) {
    if (!core) return;
    switch (core->kind) {
        case IDM_CORE_CALL:
            collect_celled_slots(core->as.call.callee, celled, lc);
            for (size_t i = 0; i < core->as.call.arg_count; i++) collect_celled_slots(core->as.call.args[i], celled, lc);
            return;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            collect_celled_slots(core->as.list_pair.head, celled, lc);
            collect_celled_slots(core->as.list_pair.tail, celled, lc);
            return;
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) collect_celled_slots(core->as.value_sequence.items[i], celled, lc);
            return;
        case IDM_CORE_SYNTAX_BUILD:
            collect_celled_slots(core->as.syntax_build.ctx, celled, lc);
            collect_celled_slots(core->as.syntax_build.payload, celled, lc);
            return;
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) collect_celled_slots(core->as.string_concat.items[i], celled, lc);
            return;
        case IDM_CORE_COND:
            collect_celled_slots(core->as.cond_expr.cond, celled, lc);
            collect_celled_slots(core->as.cond_expr.then_branch, celled, lc);
            collect_celled_slots(core->as.cond_expr.else_branch, celled, lc);
            return;
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) collect_celled_slots(core->as.match_expr.scrutinees[i], celled, lc);
            return;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) collect_celled_slots(core->as.do_expr.items[i], celled, lc);
            return;
        case IDM_CORE_BIND_LOCAL:
            collect_celled_slots(core->as.bind_local.value, celled, lc);
            collect_celled_slots(core->as.bind_local.body, celled, lc);
            return;
        case IDM_CORE_FN:
        case IDM_CORE_FN_MULTI:
            return;
        case IDM_CORE_LETREC:
            collect_celled_slots(core->as.letrec.body, celled, lc);
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                collect_celled_slots(core->as.letrec.bindings[i].value, celled, lc);
                if (!core->as.letrec.env && core->as.letrec.bindings[i].slot < lc) celled[core->as.letrec.bindings[i].slot] = true;
            }
            return;
        case IDM_CORE_RECEIVE:
            collect_celled_slots(core->as.receive.receiver, celled, lc);
            collect_celled_slots(core->as.receive.timeout, celled, lc);
            collect_celled_slots(core->as.receive.timeout_body, celled, lc);
            return;
        case IDM_CORE_GUARD:
            if (core->as.guard.handler && core->as.guard.rescue_slot < lc) celled[core->as.guard.rescue_slot] = true;
            if (core->as.guard.cleanup && core->as.guard.ensure_slot < lc) celled[core->as.guard.ensure_slot] = true;
            collect_celled_slots(core->as.guard.body, celled, lc);
            collect_celled_slots(core->as.guard.handler, celled, lc);
            collect_celled_slots(core->as.guard.cleanup, celled, lc);
            return;
        case IDM_CORE_USE_PACKAGE:
            collect_celled_slots(core->as.use_package.cont, celled, lc);
            return;
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) collect_celled_slots(core->as.record_construct.field_values[i], celled, lc);
            return;
        case IDM_CORE_RECORD_FIELD:
            collect_celled_slots(core->as.record_field.receiver, celled, lc);
            return;
        case IDM_CORE_RECORD_IS:
            collect_celled_slots(core->as.record_is.value, celled, lc);
            return;
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
        case IDM_CORE_LITERAL:
            return;
    }
}

static void mark_capture_cells(IdmCapture *child, size_t n, const bool *celled, uint32_t lc, const IdmCapture *outer, size_t outer_n) {
    for (size_t i = 0; i < n; i++) {
        switch (child[i].kind) {
            case IDM_CAP_LOCAL: child[i].celled = child[i].index < lc ? celled[child[i].index] : false; break;
            case IDM_CAP_ARG: child[i].celled = false; break;
            case IDM_CAP_UPVALUE: child[i].celled = child[i].index < outer_n ? outer[child[i].index].celled : false; break;
        }
    }
}

static void mark_celled(IdmCore *core, const bool *celled, uint32_t lc, const IdmCapture *caps, size_t ncaps) {
    if (!core) return;
    switch (core->kind) {
        case IDM_CORE_LOCAL_REF:
            core->local_celled = core->as.slot_ref.slot < lc ? celled[core->as.slot_ref.slot] : true;
            return;
        case IDM_CORE_CAPTURE_REF:
            core->local_celled = core->as.slot_ref.slot < ncaps ? caps[core->as.slot_ref.slot].celled : true;
            return;
        case IDM_CORE_BIND_LOCAL:
            core->local_celled = core->as.bind_local.slot < lc ? celled[core->as.bind_local.slot] : true;
            mark_celled(core->as.bind_local.value, celled, lc, caps, ncaps);
            mark_celled(core->as.bind_local.body, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_FN:
            mark_capture_cells(core->as.fn.captures, core->as.fn.capture_count, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_FN_MULTI:
            mark_capture_cells(core->as.fn_multi.captures, core->as.fn_multi.capture_count, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_CALL:
            mark_celled(core->as.call.callee, celled, lc, caps, ncaps);
            for (size_t i = 0; i < core->as.call.arg_count; i++) mark_celled(core->as.call.args[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            mark_celled(core->as.list_pair.head, celled, lc, caps, ncaps);
            mark_celled(core->as.list_pair.tail, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_VALUE_SEQUENCE:
            for (size_t i = 0; i < core->as.value_sequence.count; i++) mark_celled(core->as.value_sequence.items[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_SYNTAX_BUILD:
            mark_celled(core->as.syntax_build.ctx, celled, lc, caps, ncaps);
            mark_celled(core->as.syntax_build.payload, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_STRING_CONCAT:
            for (size_t i = 0; i < core->as.string_concat.count; i++) mark_celled(core->as.string_concat.items[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_COND:
            mark_celled(core->as.cond_expr.cond, celled, lc, caps, ncaps);
            mark_celled(core->as.cond_expr.then_branch, celled, lc, caps, ncaps);
            mark_celled(core->as.cond_expr.else_branch, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_MATCH:
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) mark_celled(core->as.match_expr.scrutinees[i], celled, lc, caps, ncaps);
            mark_capture_cells(core->as.match_expr.captures, core->as.match_expr.capture_count, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) mark_celled(core->as.do_expr.items[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_LETREC:
            mark_celled(core->as.letrec.body, celled, lc, caps, ncaps);
            for (size_t i = 0; i < core->as.letrec.count; i++) mark_celled(core->as.letrec.bindings[i].value, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_RECEIVE:
            mark_celled(core->as.receive.receiver, celled, lc, caps, ncaps);
            mark_celled(core->as.receive.timeout, celled, lc, caps, ncaps);
            mark_celled(core->as.receive.timeout_body, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_GUARD:
            mark_celled(core->as.guard.body, celled, lc, caps, ncaps);
            mark_celled(core->as.guard.handler, celled, lc, caps, ncaps);
            mark_celled(core->as.guard.cleanup, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_USE_PACKAGE:
            mark_celled(core->as.use_package.cont, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_RECORD_CONSTRUCT:
            for (size_t i = 0; i < core->as.record_construct.count; i++) mark_celled(core->as.record_construct.field_values[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_RECORD_FIELD:
            mark_celled(core->as.record_field.receiver, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_RECORD_IS:
            mark_celled(core->as.record_is.value, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_ARG_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
        case IDM_CORE_LITERAL:
            return;
    }
}

static bool compile_function_code(CompileModuleCtx *module_ctx, IdmBytecodeModule *module, uint32_t function_index, IdmCore *body, IdmError *err, IdmSpan span, const IdmCapture *captures, size_t capture_count) {
    if (!idm_bc_set_function_entry(module, function_index, module->code_count)) return idm_error_set(err, span, "failed to set function entry");
    uint32_t arity = module->functions[function_index].arity;
    uint32_t lc = module->functions[function_index].local_count;
    bool *celled = NULL;
    if (lc > 0) {
        celled = calloc(lc, sizeof(*celled));
        if (!celled) return idm_error_oom(err, span);
        collect_celled_slots(body, celled, lc);
    }
    mark_celled(body, celled, lc, captures, capture_count);
    free(celled);
    CompileCtx ctx;
    ctx.rt = module_ctx->rt;
    ctx.module_ctx = module_ctx;
    ctx.module = module;
    ctx.function_index = function_index;
    ctx.arity = arity;
    ctx.local_count = lc;
    ctx.next_reg = arity + lc;
    ctx.max_reg = ctx.next_reg;
    uint32_t result = ctx_alloc(&ctx);
    if (!compile_expr(body, &ctx, result, true, err)) return false;
    if (!emit_op1(module, IDM_OP_RETURN, result, NULL)) return idm_error_oom(err, span);
    if (!idm_bc_set_function_register_count(module, function_index, ctx.max_reg)) return idm_error_set(err, span, "failed to set function register count");
    return true;
}

static const IdmPrimitiveInfo PRIMITIVES[] = {
#define IDM_PRIMITIVE_INFO(id, name, min_arity, max_arity, home, pure) [IDM_PRIM_##id] = {name, min_arity, max_arity, pure},
    IDM_PRIMITIVE_LIST(IDM_PRIMITIVE_INFO)
#undef IDM_PRIMITIVE_INFO
};

static const char *const PRIMITIVE_HOMES[] = {
#define IDM_PRIMITIVE_HOME(id, name, min_arity, max_arity, home, pure) [IDM_PRIM_##id] = home,
    IDM_PRIMITIVE_LIST(IDM_PRIMITIVE_HOME)
#undef IDM_PRIMITIVE_HOME
};

bool idm_primitive_pure(IdmPrimitive primitive) {
    if ((size_t)primitive >= sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0])) return false;
    return PRIMITIVES[primitive].pure;
}

size_t idm_primitive_count(void) {
    return sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]);
}

const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive) {
    size_t index = (size_t)primitive;
    if (index >= idm_primitive_count() || !PRIMITIVES[index].name) return NULL;
    return &PRIMITIVES[index];
}

IdmArity idm_primitive_arity(IdmPrimitive primitive) {
    if (primitive == IDM_PRIM_COND) {
        IdmArity arity = idm_arity_unknown();
        if (!idm_arity_add_exact(&arity, 2u) || !idm_arity_add_exact(&arity, 3u)) return idm_arity_unknown();
        return arity;
    }
    const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
    return info ? idm_arity_range(info->min_arity, info->max_arity) : idm_arity_unknown();
}

const char *idm_primitive_home(IdmPrimitive primitive) {
    size_t index = (size_t)primitive;
    if (index >= idm_primitive_count()) return "";
    if (index >= sizeof(PRIMITIVE_HOMES) / sizeof(PRIMITIVE_HOMES[0])) return "";
    const char *home = PRIMITIVE_HOMES[index];
    return home ? home : "";
}

bool idm_primitive_home_exists(const char *home) {
    if (!home || !home[0]) return false;
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        const char *candidate = idm_primitive_home((IdmPrimitive)i);
        if (candidate[0] && strcmp(candidate, home) == 0) return true;
    }
    return false;
}

bool idm_primitive_lookup(const char *home, const char *name, IdmPrimitive *out) {
    if (!home || !name) return false;
    for (size_t i = 0; i < idm_primitive_count(); i++) {
        IdmPrimitive primitive = (IdmPrimitive)i;
        const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
        if (info && strcmp(idm_primitive_home(primitive), home) == 0 && strcmp(info->name, name) == 0) {
            if (out) *out = primitive;
            return true;
        }
    }
    return false;
}

const char *idm_primitive_name(IdmPrimitive primitive) {
    const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
    if (info) return info->name;
    return "<bad-primitive>";
}

static bool primitive_contract_quantify(IdmCallableContract *contract, const char *name, IdmError *err, IdmSpan span) {
    if (!name || !name[0] || strcmp(name, "_") == 0) return true;
    for (size_t i = 0; i < contract->quantified_count; i++) {
        if (contract->quantified[i] && strcmp(contract->quantified[i], name) == 0) return true;
    }
    char **items = realloc(contract->quantified, sizeof(*items) * (contract->quantified_count + 1u));
    if (!items) return idm_error_oom(err, span);
    contract->quantified = items;
    contract->quantified[contract->quantified_count] = idm_strdup(name);
    if (!contract->quantified[contract->quantified_count]) return idm_error_oom(err, span);
    contract->quantified_count++;
    return true;
}

static bool primitive_contract_args(IdmCallableContract *contract, size_t argc, IdmError *err, IdmSpan span) {
    memset(contract, 0, sizeof(*contract));
    IdmContractSig *sig = idm_contract_add_sig(contract);
    if (!sig) return idm_error_oom(err, span);
    if (argc == 0u) return true;
    sig->args = calloc(argc, sizeof(*sig->args));
    if (!sig->args) return idm_error_oom(err, span);
    sig->arg_count = argc;
    for (size_t i = 0; i < argc; i++) {
        if (!idm_type_var(&sig->args[i], "_", 0u, false)) {
            idm_callable_contract_destroy(contract);
            return idm_error_oom(err, span);
        }
    }
    return true;
}

static bool primitive_contract_set_var(IdmCallableContract *contract, IdmTypeTerm *term, const char *name, uint32_t var_id, IdmError *err, IdmSpan span) {
    IdmTypeTerm next;
    if (!idm_type_var(&next, name, var_id, false)) return idm_error_oom(err, span);
    idm_type_term_destroy(term);
    *term = next;
    return primitive_contract_quantify(contract, name, err, span);
}

static bool primitive_contract_set_con(IdmTypeTerm *term, const char *name, IdmError *err, IdmSpan span) {
    IdmTypeTerm next;
    if (!idm_type_con(&next, name)) return idm_error_oom(err, span);
    idm_type_term_destroy(term);
    *term = next;
    return true;
}

static bool primitive_contract_arg_var(IdmCallableContract *contract, size_t index, const char *name, uint32_t var_id, IdmError *err, IdmSpan span) {
    if (index >= contract->sigs[0].arg_count) return idm_error_set(err, span, "primitive contract argument out of range");
    return primitive_contract_set_var(contract, &contract->sigs[0].args[index], name, var_id, err, span);
}

static bool primitive_contract_arg_con(IdmCallableContract *contract, size_t index, const char *name, IdmError *err, IdmSpan span) {
    if (index >= contract->sigs[0].arg_count) return idm_error_set(err, span, "primitive contract argument out of range");
    return primitive_contract_set_con(&contract->sigs[0].args[index], name, err, span);
}

static bool primitive_contract_arg_union_cons(IdmCallableContract *contract, size_t index, const char *a, const char *b, IdmError *err, IdmSpan span) {
    if (index >= contract->sigs[0].arg_count) return idm_error_set(err, span, "primitive contract argument out of range");
    IdmTypeTerm *items = calloc(2u, sizeof(*items));
    if (!items) return idm_error_oom(err, span);
    if (!idm_type_con(&items[0], a) || !idm_type_con(&items[1], b)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return idm_error_oom(err, span);
    }
    IdmTypeTerm next;
    if (!idm_type_compound(&next, IDM_TYPE_UNION, items, 2u)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return idm_error_oom(err, span);
    }
    idm_type_term_destroy(&contract->sigs[0].args[index]);
    contract->sigs[0].args[index] = next;
    return true;
}

static bool primitive_contract_result_var(IdmCallableContract *contract, const char *name, uint32_t var_id, IdmError *err, IdmSpan span) {
    if (!primitive_contract_set_var(contract, &contract->sigs[0].result, name, var_id, err, span)) return false;
    contract->sigs[0].has_result = true;
    return true;
}

static bool primitive_contract_result_con(IdmCallableContract *contract, const char *name, IdmError *err, IdmSpan span) {
    if (!primitive_contract_set_con(&contract->sigs[0].result, name, err, span)) return false;
    contract->sigs[0].has_result = true;
    return true;
}

static bool primitive_contract_result_union_take(IdmCallableContract *contract, IdmTypeTerm *items, size_t count, IdmError *err, IdmSpan span) {
    IdmTypeTerm next;
    if (!idm_type_compound(&next, IDM_TYPE_UNION, items, count)) {
        for (size_t i = 0; i < count; i++) idm_type_term_destroy(&items[i]);
        free(items);
        return idm_error_oom(err, span);
    }
    idm_type_term_destroy(&contract->sigs[0].result);
    contract->sigs[0].result = next;
    contract->sigs[0].has_result = true;
    return true;
}

static bool primitive_contract_result_union_cons(IdmCallableContract *contract, const char *a, const char *b, IdmError *err, IdmSpan span) {
    IdmTypeTerm *items = calloc(2u, sizeof(*items));
    if (!items) return idm_error_oom(err, span);
    if (!idm_type_con(&items[0], a) || !idm_type_con(&items[1], b)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return idm_error_oom(err, span);
    }
    return primitive_contract_result_union_take(contract, items, 2u, err, span);
}

static bool primitive_contract_result_union_var_con(IdmCallableContract *contract, const char *var_name, uint32_t var_id, const char *con, IdmError *err, IdmSpan span) {
    IdmTypeTerm *items = calloc(2u, sizeof(*items));
    if (!items) return idm_error_oom(err, span);
    if (!idm_type_var(&items[0], var_name, var_id, false) || !idm_type_con(&items[1], con)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return idm_error_oom(err, span);
    }
    if (!primitive_contract_quantify(contract, var_name, err, span)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return false;
    }
    return primitive_contract_result_union_take(contract, items, 2u, err, span);
}

static bool primitive_contract_result_union_vars(IdmCallableContract *contract, const char *a_name, uint32_t a_id, const char *b_name, uint32_t b_id, IdmError *err, IdmSpan span) {
    IdmTypeTerm *items = calloc(2u, sizeof(*items));
    if (!items) return idm_error_oom(err, span);
    if (!idm_type_var(&items[0], a_name, a_id, false) || !idm_type_var(&items[1], b_name, b_id, false)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return idm_error_oom(err, span);
    }
    if (!primitive_contract_quantify(contract, a_name, err, span) || !primitive_contract_quantify(contract, b_name, err, span)) {
        idm_type_term_destroy(&items[0]);
        idm_type_term_destroy(&items[1]);
        free(items);
        return false;
    }
    return primitive_contract_result_union_take(contract, items, 2u, err, span);
}

static bool primitive_contract_class(IdmCallableContract *contract, const char *trait, const char *var_name, uint32_t var_id, IdmError *err, IdmSpan span) {
    IdmConstraint *items = realloc(contract->context, sizeof(*items) * (contract->context_count + 1u));
    if (!items) return idm_error_oom(err, span);
    contract->context = items;
    IdmConstraint *constraint = &contract->context[contract->context_count];
    memset(constraint, 0, sizeof(*constraint));
    constraint->kind = IDM_CONSTR_CLASS;
    constraint->trait = idm_strdup(trait);
    if (!constraint->trait || !idm_type_var(&constraint->lhs, var_name, var_id, false)) {
        idm_constraint_destroy(constraint);
        return idm_error_oom(err, span);
    }
    contract->context_count++;
    return primitive_contract_quantify(contract, var_name, err, span);
}

static bool primitive_contract_numeric_same(IdmCallableContract *contract, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < contract->sigs[0].arg_count; i++) {
        if (!primitive_contract_arg_var(contract, i, "a", 1u, err, span)) return false;
    }
    return primitive_contract_result_var(contract, "a", 1u, err, span) &&
           primitive_contract_class(contract, "Number", "a", 1u, err, span);
}

static bool primitive_contract_numeric_to(IdmCallableContract *contract, const char *result, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < contract->sigs[0].arg_count; i++) {
        if (!primitive_contract_arg_var(contract, i, "a", 1u, err, span)) return false;
    }
    return primitive_contract_result_con(contract, result, err, span) &&
           primitive_contract_class(contract, "Number", "a", 1u, err, span);
}

static bool primitive_contract_all_args(IdmCallableContract *contract, const char *name, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < contract->sigs[0].arg_count; i++) {
        if (!primitive_contract_arg_con(contract, i, name, err, span)) return false;
    }
    return true;
}

bool idm_primitive_contract(IdmPrimitive primitive, size_t argc, IdmCallableContract *out, bool *has_contract, IdmError *err, IdmSpan span) {
    if (has_contract) *has_contract = false;
    if (!out) return true;
    memset(out, 0, sizeof(*out));
    IdmArity arity = idm_primitive_arity(primitive);
    if (!idm_arity_accepts(&arity, (uint32_t)argc)) return true;
    if (!primitive_contract_args(out, argc, err, span)) return false;
    out->purity = idm_primitive_pure(primitive) ? IDM_PURITY_PURE : IDM_PURITY_IMPURE;
    bool ok = true;
    switch (primitive) {
        case IDM_PRIM_ADD:
        case IDM_PRIM_SUB:
        case IDM_PRIM_MUL:
        case IDM_PRIM_DIV:
        case IDM_PRIM_MOD:
        case IDM_PRIM_POW:
        case IDM_PRIM_NEG:
        case IDM_PRIM_ABS:
        case IDM_PRIM_FLOOR_DIV:
        case IDM_PRIM_FLOOR_MOD:
            ok = primitive_contract_numeric_same(out, err, span);
            break;
        case IDM_PRIM_LT:
        case IDM_PRIM_GT:
        case IDM_PRIM_LTE:
        case IDM_PRIM_GTE:
        case IDM_PRIM_NAN_P:
        case IDM_PRIM_FINITE_P:
        case IDM_PRIM_INFINITE_P:
            ok = primitive_contract_numeric_to(out, "atom", err, span);
            break;
        case IDM_PRIM_FLOOR:
        case IDM_PRIM_ROUND:
        case IDM_PRIM_CEIL:
        case IDM_PRIM_TRUNCATE:
        case IDM_PRIM_SQRT:
        case IDM_PRIM_SIN:
        case IDM_PRIM_COS:
        case IDM_PRIM_TAN:
        case IDM_PRIM_ASIN:
        case IDM_PRIM_ACOS:
        case IDM_PRIM_ATAN:
        case IDM_PRIM_ATAN2:
        case IDM_PRIM_EXP:
        case IDM_PRIM_LOG:
        case IDM_PRIM_LOG2:
        case IDM_PRIM_LOG10:
        case IDM_PRIM_HYPOT:
        case IDM_PRIM_TO_FLOAT:
            ok = primitive_contract_numeric_to(out, "float", err, span);
            break;
        case IDM_PRIM_TO_INT:
            ok = primitive_contract_numeric_to(out, "int", err, span);
            break;
        case IDM_PRIM_DIVMOD:
            ok = primitive_contract_numeric_to(out, "tuple", err, span);
            break;
        case IDM_PRIM_BIT_AND:
        case IDM_PRIM_BIT_OR:
        case IDM_PRIM_BIT_XOR:
        case IDM_PRIM_BIT_NOT:
        case IDM_PRIM_SHIFT_LEFT:
        case IDM_PRIM_SHIFT_RIGHT:
        case IDM_PRIM_BIT_COUNT:
        case IDM_PRIM_BIT_LENGTH:
            ok = primitive_contract_all_args(out, "int", err, span) &&
                 primitive_contract_result_con(out, "int", err, span);
            break;
        case IDM_PRIM_EQ:
        case IDM_PRIM_NEQ:
        case IDM_PRIM_OK:
        case IDM_PRIM_SYNTAX_LIST_PRED:
        case IDM_PRIM_SYNTAX_WORD_PRED:
        case IDM_PRIM_SYNTAX_ATOM_PRED:
        case IDM_PRIM_SYNTAX_INT_PRED:
        case IDM_PRIM_SYNTAX_ADJACENT_PRED:
        case IDM_PRIM_STR_CONTAINS:
        case IDM_PRIM_REGEX_PRED:
        case IDM_PRIM_REGEX_RESULT_PRED:
        case IDM_PRIM_REGEX_TEST:
        case IDM_PRIM_DICT_HAS:
        case IDM_PRIM_FILE_EXISTS:
        case IDM_PRIM_TTY_PRED:
        case IDM_PRIM_IS_A_P:
        case IDM_PRIM_NIL_P:
        case IDM_PRIM_ATOM_P:
        case IDM_PRIM_WORD_P:
        case IDM_PRIM_INT_P:
        case IDM_PRIM_FLOAT_P:
        case IDM_PRIM_STRING_P:
        case IDM_PRIM_PAIR_P:
        case IDM_PRIM_EMPTY_LIST_P:
        case IDM_PRIM_LIST_P:
        case IDM_PRIM_TUPLE_P:
        case IDM_PRIM_VECTOR_P:
        case IDM_PRIM_DICT_P:
        case IDM_PRIM_SYNTAX_P:
        case IDM_PRIM_CELL_P:
        case IDM_PRIM_CLOSURE_P:
        case IDM_PRIM_PID_P:
        case IDM_PRIM_REF_P:
        case IDM_PRIM_PORT_P:
        case IDM_PRIM_REGEX_P:
        case IDM_PRIM_REGEX_RESULT_P:
        case IDM_PRIM_NAN:
        case IDM_PRIM_INF:
        case IDM_PRIM_CD:
        case IDM_PRIM_CHDIR:
        case IDM_PRIM_ENV_SET:
        case IDM_PRIM_PRINT:
        case IDM_PRIM_PRINTLN:
        case IDM_PRIM_EPRINTLN:
        case IDM_PRIM_REPL_ABORT:
        case IDM_PRIM_TTY_RAW:
        case IDM_PRIM_TTY_RESTORE:
        case IDM_PRIM_TTY_WRITE:
        case IDM_PRIM_PORT_CLOSE_INPUT:
        case IDM_PRIM_JOB_RESUME:
        case IDM_PRIM_JOB_SIGNAL:
        case IDM_PRIM_SEND:
        case IDM_PRIM_EXIT:
        case IDM_PRIM_LINK:
        case IDM_PRIM_UNLINK:
        case IDM_PRIM_DEMONITOR:
        case IDM_PRIM_TRAP_EXIT:
            ok = primitive_contract_result_con(out, "atom", err, span);
            break;
        case IDM_PRIM_INSPECT:
        case IDM_PRIM_STR:
        case IDM_PRIM_CHOMP:
        case IDM_PRIM_SYNTAX_WORD_TEXT:
        case IDM_PRIM_SYNTAX_ATOM_TEXT:
        case IDM_PRIM_SYNTAX_STRING_TEXT:
        case IDM_PRIM_STR_SLICE:
        case IDM_PRIM_BYTE_STR:
        case IDM_PRIM_REGEX_SOURCE:
        case IDM_PRIM_REGEX_RESULT_TEXT:
        case IDM_PRIM_REGEX_REPLACE:
        case IDM_PRIM_REGEX_REPLACE_ALL:
        case IDM_PRIM_REGEX_ESCAPE:
        case IDM_PRIM_PWD:
        case IDM_PRIM_ERROR_MESSAGE:
        case IDM_PRIM_ORD_STR:
            ok = primitive_contract_result_con(out, "string", err, span);
            break;
        case IDM_PRIM_REPL_DIAGNOSTIC:
            ok = primitive_contract_result_union_cons(out, "string", "nil", err, span);
            break;
        case IDM_PRIM_PORT_READ:
        case IDM_PRIM_PORT_WRITE:
            ok = primitive_contract_result_union_cons(out, "tuple", "atom", err, span);
            break;
        case IDM_PRIM_ENV_GET:
            ok = primitive_contract_arg_con(out, 0u, "string", err, span) &&
                 primitive_contract_result_union_cons(out, "string", "nil", err, span);
            break;
        case IDM_PRIM_STR_FIND:
        case IDM_PRIM_STR_BYTE:
        case IDM_PRIM_STR_ORD:
        case IDM_PRIM_REGEX_RESULT_START:
        case IDM_PRIM_REGEX_RESULT_END:
            ok = primitive_contract_result_union_cons(out, "int", "nil", err, span);
            break;
        case IDM_PRIM_STR_LEN:
        case IDM_PRIM_SYNTAX_LENGTH:
        case IDM_PRIM_SYNTAX_INT_VALUE:
        case IDM_PRIM_REGEX_GROUP_COUNT:
        case IDM_PRIM_TIME_MS:
        case IDM_PRIM_RANDOM:
        case IDM_PRIM_DICT_SIZE:
        case IDM_PRIM_COMPARE:
            ok = primitive_contract_result_con(out, "int", err, span);
            break;
        case IDM_PRIM_SYNTAX_FLOAT_VALUE:
            ok = primitive_contract_result_con(out, "float", err, span);
            break;
        case IDM_PRIM_LIST:
        case IDM_PRIM_STR_TO_LIST:
        case IDM_PRIM_DICT_TO_LIST:
        case IDM_PRIM_VECTOR_TO_LIST:
        case IDM_PRIM_TUPLE_TO_LIST:
        case IDM_PRIM_SYNTAX_ORIGIN:
        case IDM_PRIM_SYNTAX_SLICE:
        case IDM_PRIM_REGEX_OPTIONS:
        case IDM_PRIM_REGEX_GROUP_NAMES:
        case IDM_PRIM_REGEX_CAPTURES:
        case IDM_PRIM_REGEX_SCAN_ALL:
        case IDM_PRIM_REGEX_SPLIT_ON:
        case IDM_PRIM_FILE_LIST:
        case IDM_PRIM_ARGS:
        case IDM_PRIM_DICT_KEYS:
        case IDM_PRIM_DICT_VALS:
            ok = primitive_contract_result_con(out, "list", err, span);
            break;
        case IDM_PRIM_CONS:
        case IDM_PRIM_APPEND:
            ok = primitive_contract_result_con(out, "pair", err, span);
            break;
        case IDM_PRIM_TUPLE:
        case IDM_PRIM_MAKE_ERROR:
        case IDM_PRIM_FILE_READ:
        case IDM_PRIM_FILE_STAT:
        case IDM_PRIM_FILE_OPEN:
        case IDM_PRIM_PARSE_INT:
        case IDM_PRIM_PARSE_FLOAT:
        case IDM_PRIM_TTY_SIZE:
            ok = primitive_contract_result_con(out, "tuple", err, span);
            break;
        case IDM_PRIM_REPL_COMPILE:
        case IDM_PRIM_FILE_WRITE:
        case IDM_PRIM_FILE_REMOVE:
        case IDM_PRIM_FILE_MKDIR:
        case IDM_PRIM_FILE_APPEND:
            ok = primitive_contract_result_union_cons(out, "tuple", "atom", err, span);
            break;
        case IDM_PRIM_TTY_READ:
            ok = primitive_contract_arg_union_cons(out, 0u, "int", "nil", err, span) &&
                 primitive_contract_result_union_cons(out, "tuple", "atom", err, span);
            break;
        case IDM_PRIM_TTY_READ_LINE:
            ok = primitive_contract_result_union_cons(out, "tuple", "atom", err, span);
            break;
        case IDM_PRIM_VECTOR:
            ok = primitive_contract_result_con(out, "vector", err, span);
            break;
        case IDM_PRIM_DICT:
        case IDM_PRIM_DICT_PUT:
        case IDM_PRIM_DICT_DEL:
            ok = primitive_contract_result_con(out, "dict", err, span);
            break;
        case IDM_PRIM_TUPLE_GET:
            ok = primitive_contract_arg_con(out, 0u, "tuple", err, span) &&
                 primitive_contract_arg_con(out, 1u, "int", err, span) &&
                 primitive_contract_result_var(out, "a", 1u, err, span);
            break;
        case IDM_PRIM_DICT_GET:
            ok = primitive_contract_arg_con(out, 0u, "dict", err, span) &&
                 primitive_contract_arg_var(out, 2u, "a", 1u, err, span) &&
                 primitive_contract_result_var(out, "a", 1u, err, span);
            break;
        case IDM_PRIM_FIRST:
            ok = primitive_contract_result_var(out, "a", 1u, err, span);
            break;
        case IDM_PRIM_REST:
            ok = primitive_contract_result_con(out, "list", err, span);
            break;
        case IDM_PRIM_COND:
            if (argc == 2u) {
                ok = primitive_contract_arg_var(out, 1u, "a", 1u, err, span) &&
                     primitive_contract_result_union_var_con(out, "a", 1u, "nil", err, span);
            } else {
                ok = primitive_contract_arg_var(out, 1u, "a", 1u, err, span) &&
                     primitive_contract_arg_var(out, 2u, "b", 2u, err, span) &&
                     primitive_contract_result_union_vars(out, "a", 1u, "b", 2u, err, span);
            }
            break;
        case IDM_PRIM_REGEX_COMPILE:
            ok = primitive_contract_arg_con(out, 0u, "string", err, span) &&
                 primitive_contract_arg_union_cons(out, 1u, "list", "atom", err, span) &&
                 primitive_contract_result_con(out, "tuple", err, span);
            break;
        case IDM_PRIM_REGEX_SCAN_AT:
        case IDM_PRIM_REGEX_SCAN_FROM:
            ok = primitive_contract_arg_con(out, 0u, "regex", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_arg_con(out, 2u, "int", err, span) &&
                 primitive_contract_result_union_cons(out, "regex-result", "nil", err, span);
            break;
        case IDM_PRIM_REGEX_SCAN_FULL:
            ok = primitive_contract_arg_con(out, 0u, "regex", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_result_union_cons(out, "regex-result", "nil", err, span);
            break;
        case IDM_PRIM_REGEX_CAPTURE:
        case IDM_PRIM_REGEX_CAPTURE_NAMED:
            ok = primitive_contract_arg_con(out, 0u, "regex-result", err, span) &&
                 primitive_contract_result_union_cons(out, "string", "nil", err, span);
            break;
        case IDM_PRIM_REGEX_CAPTURE_RANGE:
            ok = primitive_contract_arg_con(out, 0u, "regex-result", err, span) &&
                 primitive_contract_result_union_cons(out, "tuple", "nil", err, span);
            break;
        case IDM_PRIM_SYNTAX_KIND:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_result_con(out, "atom", err, span);
            break;
        case IDM_PRIM_SYNTAX_PROPERTY:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_result_union_cons(out, "string", "nil", err, span);
            break;
        case IDM_PRIM_SYNTAX_SET_PROPERTY:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_arg_con(out, 2u, "string", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_SYNTAX_NTH:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "int", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_MAKE_SYNTAX_NIL:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_MAKE_SYNTAX_WORD:
        case IDM_PRIM_MAKE_SYNTAX_ATOM:
        case IDM_PRIM_MAKE_SYNTAX_STRING:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_MAKE_SYNTAX_INT:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "int", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_MAKE_SYNTAX_LIST:
        case IDM_PRIM_MAKE_SYNTAX_VECTOR:
        case IDM_PRIM_MAKE_SYNTAX_TUPLE:
        case IDM_PRIM_MAKE_SYNTAX_DICT:
        case IDM_PRIM_MAKE_SYNTAX_EXPR:
        case IDM_PRIM_MAKE_SYNTAX_BODY:
        case IDM_PRIM_MAKE_SYNTAX_GROUP:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "list", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_SYNTAX_ERROR:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "string", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_LOCAL_EXPAND:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_result_con(out, "syntax", err, span);
            break;
        case IDM_PRIM_FREE_IDENTIFIER_EQ:
        case IDM_PRIM_BOUND_IDENTIFIER_EQ:
            ok = primitive_contract_all_args(out, "syntax", err, span) &&
                 primitive_contract_result_con(out, "atom", err, span);
            break;
        case IDM_PRIM_BIND_BANG:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_result_con(out, "atom", err, span);
            break;
        case IDM_PRIM_INTERNAL_REGISTER_MACRO:
            ok = primitive_contract_arg_con(out, 0u, "syntax", err, span) &&
                 primitive_contract_arg_con(out, 1u, "closure", err, span) &&
                 primitive_contract_result_con(out, "atom", err, span);
            break;
        case IDM_PRIM_EXPAND_CHECK:
            ok = primitive_contract_arg_con(out, 0u, "string", err, span) &&
                 primitive_contract_result_union_cons(out, "atom", "tuple", err, span);
            break;
        case IDM_PRIM_SELF:
        case IDM_PRIM_SPAWN:
        case IDM_PRIM_SPAWN_LINK:
        case IDM_PRIM_SPAWN_MONITOR:
        case IDM_PRIM_REPL_SPAWN:
            ok = primitive_contract_result_con(out, "pid", err, span);
            break;
        case IDM_PRIM_MONITOR:
            ok = primitive_contract_result_con(out, "ref", err, span);
            break;
        case IDM_PRIM_APPLY:
        case IDM_PRIM_RAISE:
            ok = primitive_contract_result_var(out, "a", 1u, err, span);
            break;
        case IDM_PRIM_FROM_RUNES:
            ok = primitive_contract_arg_con(out, 0u, "list", err, span) &&
                 primitive_contract_result_con(out, "string", err, span);
            break;
        case IDM_PRIM_ISH_SESSION:
            ok = primitive_contract_result_union_cons(out, "pid", "nil", err, span);
            break;
        case IDM_PRIM_PORT_STATUS:
            ok = primitive_contract_arg_con(out, 0u, "port", err, span) &&
                 primitive_contract_result_con(out, "atom", err, span);
            break;
        default:
            idm_callable_contract_destroy(out);
            return idm_error_set(err, span, "primitive '%s' has no declared contract", idm_primitive_name(primitive));
    }
    if (!ok) {
        idm_callable_contract_destroy(out);
        if (err && err->present) return false;
        return true;
    }
    if (has_contract) *has_contract = true;
    return true;
}

static bool emit_op1(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, size_t *out_offset) {
    return idm_bc_emit_u32(module, op, a, out_offset);
}

static bool emit_op2(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, size_t *out_offset) {
    size_t op_offset = 0;
    if (!idm_bc_emit_u32(module, op, a, &op_offset)) return false;
    if (!idm_bc_emit(module, b, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

static bool emit_op3(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, size_t *out_offset) {
    size_t op_offset = 0;
    if (!emit_op2(module, op, a, b, &op_offset)) return false;
    if (!idm_bc_emit(module, c, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

static bool emit_op4(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, size_t *out_offset) {
    size_t op_offset = 0;
    if (!emit_op3(module, op, a, b, c, &op_offset)) return false;
    if (!idm_bc_emit(module, d, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

static bool emit_op5(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, size_t *out_offset) {
    size_t op_offset = 0;
    if (!emit_op4(module, op, a, b, c, d, &op_offset)) return false;
    if (!idm_bc_emit(module, e, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

static uint32_t ctx_alloc(CompileCtx *ctx) {
    uint32_t reg = ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg) ctx->max_reg = ctx->next_reg;
    return reg;
}

static uint32_t ctx_alloc_range(CompileCtx *ctx, uint32_t count) {
    if (count == 0) return 0;
    uint32_t first = ctx->next_reg;
    ctx->next_reg += count;
    if (ctx->next_reg > ctx->max_reg) ctx->max_reg = ctx->next_reg;
    return first;
}

static uint32_t ctx_mark(const CompileCtx *ctx) {
    return ctx->next_reg;
}

static void ctx_reset(CompileCtx *ctx, uint32_t mark) {
    if (mark >= ctx->arity + ctx->local_count && mark <= ctx->next_reg) ctx->next_reg = mark;
}

static uint32_t ctx_local_reg(const CompileCtx *ctx, uint32_t slot) {
    return ctx->arity + slot;
}

static bool emit_named_op2(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, const char *name, IdmError *err, IdmSpan span) {
    size_t op_offset = 0;
    if (!emit_op2(module, op, a, b, &op_offset)) return idm_error_oom(err, span);
    if (!idm_bc_note_name(module, op_offset, name)) return idm_error_oom(err, span);
    return true;
}

static bool emit_named_op3(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, const char *name, IdmError *err, IdmSpan span) {
    size_t op_offset = 0;
    if (!emit_op3(module, op, a, b, c, &op_offset)) return idm_error_oom(err, span);
    if (!idm_bc_note_name(module, op_offset, name)) return idm_error_oom(err, span);
    return true;
}

static bool add_const_value(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index, IdmError *err, IdmSpan span);

static bool emit_capture_load(CompileCtx *ctx, IdmCapture cap, uint32_t dst, IdmError *err, IdmSpan span) {
    switch (cap.kind) {
        case IDM_CAP_LOCAL:
            return emit_named_op2(ctx->module, IDM_OP_MOVE, dst, ctx_local_reg(ctx, cap.index), cap.name, err, span);
        case IDM_CAP_ARG:
            if (!emit_named_op2(ctx->module, IDM_OP_MOVE, dst, cap.index, cap.name, err, span)) return false;
            if (cap.celled && !emit_op2(ctx->module, IDM_OP_MAKE_CELL, dst, dst, NULL)) return idm_error_oom(err, span);
            return true;
        case IDM_CAP_UPVALUE:
            return emit_named_op2(ctx->module, IDM_OP_LOAD_CAPTURE, dst, cap.index, cap.name, err, span);
    }
    return idm_error_set(err, span, "invalid capture kind");
}

static bool emit_load_value(CompileCtx *ctx, uint32_t dst, IdmValue value, IdmError *err, IdmSpan span) {
    uint32_t index = 0;
    if (!idm_bc_add_const(ctx->module, value, &index)) return idm_error_oom(err, span);
    if (!emit_op2(ctx->module, IDM_OP_LOAD_CONST, dst, index, NULL)) return idm_error_oom(err, span);
    return true;
}

static bool add_const_value(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index, IdmError *err, IdmSpan span) {
    if (!idm_bc_add_const(module, value, out_index)) return idm_error_oom(err, span);
    return true;
}

static bool add_const_name(CompileCtx *ctx, const char *name, uint32_t *out_index, IdmError *err, IdmSpan span) {
    if (!name) return add_const_value(ctx->module, idm_nil(), out_index, err, span);
    if (!ctx->rt) return idm_error_set(err, span, "compiler requires a runtime to intern record metadata");
    return add_const_value(ctx->module, idm_atom(ctx->rt, name), out_index, err, span);
}

static bool add_const_symbol(CompileCtx *ctx, const IdmSymbol *symbol, uint32_t *out_index, IdmError *err, IdmSpan span) {
    return add_const_name(ctx, core_symbol_text(symbol), out_index, err, span);
}

static bool patch_here(IdmBytecodeModule *module, size_t operand_index, IdmError *err, IdmSpan span, const char *what) {
    if (module->code_count > UINT32_MAX) return idm_error_set(err, span, "bytecode too large");
    if (!idm_bc_patch_u32(module, operand_index, (uint32_t)module->code_count)) return idm_error_set(err, span, "failed to patch %s", what);
    return true;
}

static void compile_module_ctx_destroy(CompileModuleCtx *ctx) {
    if (!ctx) return;
    free(ctx->linked_packages);
    memset(ctx, 0, sizeof(*ctx));
}

static bool compile_module_ctx_add_linked_package(CompileModuleCtx *ctx, IdmValue env_key, uint32_t fn_off, IdmError *err, IdmSpan span) {
    if (ctx->linked_package_count == ctx->linked_package_cap) {
        if (!idm_grow((void **)&ctx->linked_packages, &ctx->linked_package_cap, sizeof(*ctx->linked_packages), 8u, ctx->linked_package_count + 1u)) return idm_error_oom(err, span);
    }
    ctx->linked_packages[ctx->linked_package_count].env_key = env_key;
    ctx->linked_packages[ctx->linked_package_count].fn_off = fn_off;
    ctx->linked_package_count++;
    return true;
}

static bool compile_link_package_once(CompileCtx *ctx, IdmCore *core, uint32_t *out_fn_off, IdmError *err) {
    CompileModuleCtx *module_ctx = ctx->module_ctx;
    if (!module_ctx) return idm_error_set(err, core->span, "missing compiler module context");
    for (size_t i = 0; i < module_ctx->linked_package_count; i++) {
        if (idm_value_equal(module_ctx->linked_packages[i].env_key, core->as.use_package.env_key)) {
            *out_fn_off = module_ctx->linked_packages[i].fn_off;
            return true;
        }
    }
    size_t skip_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &skip_op)) return idm_error_oom(err, core->span);
    uint32_t const_off = 0;
    uint32_t fn_off = 0;
    uint32_t code_off = 0;
    if (!idm_bc_link(ctx->module, core->as.use_package.module, &const_off, &fn_off, &code_off, err)) return false;
    (void)const_off;
    (void)code_off;
    if (!patch_here(ctx->module, skip_op + 1u, err, core->span, "package skip jump")) return false;
    if (!compile_module_ctx_add_linked_package(module_ctx, core->as.use_package.env_key, fn_off, err, core->span)) return false;
    *out_fn_off = fn_off;
    return true;
}

static void compiled_callable_destroy(CompiledCallable *callable) {
    if (!callable) return;
    free(callable->entries);
    free(callable->guards);
    memset(callable, 0, sizeof(*callable));
}

static bool compile_callable_entries(IdmCore *fn, IdmBytecodeModule *module, CompiledCallable *out, IdmError *err, IdmSpan span) {
    memset(out, 0, sizeof(*out));
    if (!fn || (fn->kind != IDM_CORE_FN && fn->kind != IDM_CORE_FN_MULTI)) return idm_error_set(err, span, "call head is not a function");
    out->count = fn->kind == IDM_CORE_FN ? 1u : fn->as.fn_multi.count;
    out->captures = fn->kind == IDM_CORE_FN ? fn->as.fn.captures : fn->as.fn_multi.captures;
    out->capture_count = fn->kind == IDM_CORE_FN ? fn->as.fn.capture_count : fn->as.fn_multi.capture_count;
    if (out->count == 0) return idm_error_set(err, span, "multi function has no clauses");
    if (out->count > UINT32_MAX || out->capture_count > UINT32_MAX) return idm_error_set(err, span, "function is too large");
    out->entries = calloc(out->count, sizeof(*out->entries));
    if (!out->entries) return idm_error_oom(err, span);
    out->guards = calloc(out->count, sizeof(*out->guards));
    if (!out->guards) {
        compiled_callable_destroy(out);
        return idm_error_oom(err, span);
    }
    for (size_t i = 0; i < out->count; i++) out->guards[i] = UINT32_MAX;

    if (fn->kind == IDM_CORE_FN) {
        uint32_t pattern_locals = pattern_local_max_plus_one(fn->as.fn.pattern_locals, fn->as.fn.pattern_local_count);
        uint32_t locals = max_u32(core_max_local_plus_one(fn->as.fn.body), pattern_locals);
        if (!add_compiled_function(module, fn->as.fn.name, fn->as.fn.arity, locals, fn->as.fn.param_patterns, fn->as.fn.pattern_count, fn->as.fn.pattern_locals, fn->as.fn.pattern_local_count, &out->entries[0], err, span)) {
            compiled_callable_destroy(out);
            return false;
        }
        if (fn->as.fn.guard) {
            uint32_t guard_locals = max_u32(core_max_local_plus_one(fn->as.fn.guard), pattern_locals);
            if (!add_guard_function(module, fn->as.fn.name, fn->as.fn.arity, guard_locals, fn->as.fn.pattern_locals, fn->as.fn.pattern_local_count, &out->guards[0], err, span)) {
                compiled_callable_destroy(out);
                return false;
            }
            if (!idm_bc_set_function_guard(module, out->entries[0], out->guards[0])) {
                compiled_callable_destroy(out);
                return idm_error_set(err, span, "failed to attach function guard");
            }
        }
        return true;
    }

    for (size_t i = 0; i < fn->as.fn_multi.count; i++) {
        IdmFnClause *clause = &fn->as.fn_multi.clauses[i];
        if (clause->primitive_backed) {
            if (fn->as.fn_multi.capture_count != 0) {
                compiled_callable_destroy(out);
                return idm_error_set(err, span, "primitive-backed clause cannot capture values");
            }
            if (clause->guard || clause->body || clause->pattern_count != 0 || clause->pattern_local_count != 0) {
                compiled_callable_destroy(out);
                return idm_error_set(err, span, "primitive-backed clause cannot carry source patterns, guards, or bodies");
            }
            if (!idm_bc_add_primitive_function(module, fn->as.fn_multi.name, clause->call_arity, (uint32_t)clause->primitive, &out->entries[i])) {
                compiled_callable_destroy(out);
                return idm_error_oom(err, span);
            }
            continue;
        }
        uint32_t pattern_locals = pattern_local_max_plus_one(clause->pattern_locals, clause->pattern_local_count);
        uint32_t locals = max_u32(core_max_local_plus_one(clause->body), pattern_locals);
        if (!add_compiled_function(module, fn->as.fn_multi.name, clause->arity, locals, clause->param_patterns, clause->pattern_count, clause->pattern_locals, clause->pattern_local_count, &out->entries[i], err, span)) {
            compiled_callable_destroy(out);
            return false;
        }
        if (clause->guard) {
            uint32_t guard_locals = max_u32(core_max_local_plus_one(clause->guard), pattern_locals);
            if (!add_guard_function(module, fn->as.fn_multi.name, clause->arity, guard_locals, clause->pattern_locals, clause->pattern_local_count, &out->guards[i], err, span)) {
                compiled_callable_destroy(out);
                return false;
            }
            if (!idm_bc_set_function_guard(module, out->entries[i], out->guards[i])) {
                compiled_callable_destroy(out);
                return idm_error_set(err, span, "failed to attach multi function guard");
            }
        }
    }
    return true;
}

static bool compile_callable_bodies(CompileModuleCtx *module_ctx, IdmCore *fn, const CompiledCallable *callable, IdmBytecodeModule *module, IdmError *err, IdmSpan span) {
    if (fn->kind == IDM_CORE_FN) {
        if (fn->as.fn.guard && !compile_function_code(module_ctx, module, callable->guards[0], fn->as.fn.guard, err, span, fn->as.fn.captures, fn->as.fn.capture_count)) return false;
        return compile_function_code(module_ctx, module, callable->entries[0], fn->as.fn.body, err, span, fn->as.fn.captures, fn->as.fn.capture_count);
    }
    for (size_t i = 0; i < fn->as.fn_multi.count; i++) {
        IdmFnClause *clause = &fn->as.fn_multi.clauses[i];
        if (clause->primitive_backed) continue;
        if (clause->guard && !compile_function_code(module_ctx, module, callable->guards[i], clause->guard, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
        if (!compile_function_code(module_ctx, module, callable->entries[i], clause->body, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
    }
    return true;
}

static bool compile_match_entries(IdmCore *match, IdmBytecodeModule *module, CompiledCallable *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!match || match->kind != IDM_CORE_MATCH) return idm_error_set(err, idm_span_unknown(NULL), "expected match core");
    out->count = match->as.match_expr.count;
    out->captures = match->as.match_expr.captures;
    out->capture_count = match->as.match_expr.capture_count;
    if (out->count == 0) return idm_error_set(err, match->span, "match has no clauses");
    if (out->count > UINT32_MAX || out->capture_count > UINT32_MAX) return idm_error_set(err, match->span, "match is too large");
    out->entries = calloc(out->count, sizeof(*out->entries));
    if (!out->entries) return idm_error_oom(err, match->span);
    out->guards = calloc(out->count, sizeof(*out->guards));
    if (!out->guards) {
        compiled_callable_destroy(out);
        return idm_error_oom(err, match->span);
    }
    for (size_t i = 0; i < out->count; i++) out->guards[i] = UINT32_MAX;
    for (size_t i = 0; i < match->as.match_expr.count; i++) {
        IdmFnClause *clause = &match->as.match_expr.clauses[i];
        uint32_t pattern_locals = pattern_local_max_plus_one(clause->pattern_locals, clause->pattern_local_count);
        uint32_t locals = max_u32(core_max_local_plus_one(clause->body), pattern_locals);
        if (!add_compiled_function(module, "<match>", clause->arity, locals, clause->param_patterns, clause->pattern_count, clause->pattern_locals, clause->pattern_local_count, &out->entries[i], err, match->span)) {
            compiled_callable_destroy(out);
            return false;
        }
        if (clause->guard) {
            uint32_t guard_locals = max_u32(core_max_local_plus_one(clause->guard), pattern_locals);
            if (!add_guard_function(module, "<match>", clause->arity, guard_locals, clause->pattern_locals, clause->pattern_local_count, &out->guards[i], err, match->span)) {
                compiled_callable_destroy(out);
                return false;
            }
            if (!idm_bc_set_function_guard(module, out->entries[i], out->guards[i])) {
                compiled_callable_destroy(out);
                return idm_error_set(err, match->span, "failed to attach match guard");
            }
        }
    }
    return true;
}

static bool compile_match_bodies(CompileModuleCtx *module_ctx, IdmCore *match, const CompiledCallable *callable, IdmBytecodeModule *module, IdmError *err) {
    for (size_t i = 0; i < match->as.match_expr.count; i++) {
        IdmFnClause *clause = &match->as.match_expr.clauses[i];
        if (clause->guard && !compile_function_code(module_ctx, module, callable->guards[i], clause->guard, err, match->span, match->as.match_expr.captures, match->as.match_expr.capture_count)) return false;
        if (!compile_function_code(module_ctx, module, callable->entries[i], clause->body, err, match->span, match->as.match_expr.captures, match->as.match_expr.capture_count)) return false;
    }
    return true;
}

static bool emit_callable_closure(CompileCtx *ctx, const CompiledCallable *callable, uint32_t dst, IdmError *err, IdmSpan span) {
    if (callable->capture_count > UINT32_MAX || callable->count > UINT32_MAX) return idm_error_set(err, span, "function literal is too large");
    uint32_t first_capture = ctx_alloc_range(ctx, (uint32_t)callable->capture_count);
    for (size_t i = 0; i < callable->capture_count; i++) {
        if (!emit_capture_load(ctx, callable->captures[i], first_capture + (uint32_t)i, err, span)) return false;
    }
    if (callable->count == 1u) {
        if (callable->capture_count == 0) {
            if (!emit_op2(ctx->module, IDM_OP_MAKE_CLOSURE, dst, callable->entries[0], NULL)) return idm_error_oom(err, span);
        } else if (!emit_op4(ctx->module, IDM_OP_MAKE_CLOSURE_CAPTURES, dst, callable->entries[0], first_capture, (uint32_t)callable->capture_count, NULL)) {
            return idm_error_oom(err, span);
        }
        return true;
    }
    size_t op_offset = 0;
    if (!emit_op4(ctx->module, IDM_OP_MAKE_MULTI_CLOSURE, dst, (uint32_t)callable->count, (uint32_t)callable->capture_count, first_capture, &op_offset)) return idm_error_oom(err, span);
    (void)op_offset;
    for (size_t i = 0; i < callable->count; i++) {
        if (!idm_bc_emit(ctx->module, callable->entries[i], NULL)) return idm_error_oom(err, span);
    }
    return true;
}

static bool compile_callable_literal(IdmCore *fn, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    CompiledCallable callable;
    if (!compile_callable_entries(fn, ctx->module, &callable, err, fn->span)) return false;
    bool ok = emit_callable_closure(ctx, &callable, dst, err, fn->span);
    size_t jump_over_op = 0;
    if (ok && !emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, fn->span);
    if (ok) ok = compile_callable_bodies(ctx->module_ctx, fn, &callable, ctx->module, err, fn->span);
    if (ok) ok = patch_here(ctx->module, jump_over_op + 1u, err, fn->span, fn->kind == IDM_CORE_FN ? "function literal jump" : "multi function jump");
    compiled_callable_destroy(&callable);
    return ok;
}

static bool core_is_callable(const IdmCore *core) {
    return core && (core->kind == IDM_CORE_FN || core->kind == IDM_CORE_FN_MULTI);
}

static bool core_is_primitive_only_callable(const IdmCore *core) {
    if (!core || core->kind != IDM_CORE_FN_MULTI || core->as.fn_multi.count == 0) return false;
    for (size_t i = 0; i < core->as.fn_multi.count; i++) {
        if (!core->as.fn_multi.clauses[i].primitive_backed) return false;
    }
    return true;
}

static bool core_direct_primitive_callable(const IdmCore *core, IdmPrimitive *out) {
    if (!core || core->kind != IDM_CORE_FN_MULTI || core->as.fn_multi.count != 1u || core->as.fn_multi.capture_count != 0) return false;
    const IdmFnClause *clause = &core->as.fn_multi.clauses[0];
    if (!clause->primitive_backed) return false;
    if (out) *out = clause->primitive;
    return true;
}

static void destroy_compiled_callables(CompiledCallable *callables, const bool *compiled, size_t count) {
    if (!callables || !compiled) return;
    for (size_t i = 0; i < count; i++) {
        if (compiled[i]) compiled_callable_destroy(&callables[i]);
    }
}

static bool compile_letrec(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    size_t count = core->as.letrec.count;
    CompiledCallable *callables = count == 0 ? NULL : calloc(count, sizeof(*callables));
    bool *compiled = count == 0 ? NULL : calloc(count, sizeof(*compiled));
    if (count != 0 && (!callables || !compiled)) {
        free(callables);
        free(compiled);
        return idm_error_oom(err, core->span);
    }

    bool ok = true;
    size_t compiled_count = 0;
    if (core->as.letrec.env) {
        for (size_t i = 0; ok && i < count; i++) {
            IdmLetRecBinding *binding = &core->as.letrec.bindings[i];
            if (!core_is_callable(binding->value)) continue;
            ok = compile_callable_entries(binding->value, ctx->module, &callables[i], err, binding->value->span);
            if (!ok) break;
            compiled[i] = true;
            compiled_count++;
        }
    }

    if (ok && core->as.letrec.env) {
        for (size_t i = 0; ok && i < count; i++) {
            IdmLetRecBinding *binding = &core->as.letrec.bindings[i];
            uint32_t value_reg = ctx_alloc(ctx);
            if (compiled[i]) ok = emit_callable_closure(ctx, &callables[i], value_reg, err, binding->value->span);
            else ok = compile_expr(binding->value, ctx, value_reg, false, err);
            if (ok && binding->has_env_key) {
                uint32_t key_const = 0;
                if (!idm_bc_add_const(ctx->module, binding->env_key, &key_const)) ok = idm_error_oom(err, core->span);
                else ok = emit_named_op3(ctx->module, IDM_OP_STORE_PACKAGE_SLOT, key_const, binding->slot, value_reg, binding->name, err, core->span);
            } else if (ok) {
                ok = emit_named_op2(ctx->module, IDM_OP_STORE_ENV, binding->slot, value_reg, binding->name, err, core->span);
            }
        }
    } else if (ok) {
        if (!core->as.letrec.fill_only) {
            uint32_t nil_reg = ctx_alloc(ctx);
            for (size_t i = 0; ok && i < count; i++) {
                IdmLetRecBinding *binding = &core->as.letrec.bindings[i];
                if (binding->fill_existing) continue;
                uint32_t local = ctx_local_reg(ctx, binding->slot);
                if (!emit_load_value(ctx, nil_reg, idm_nil(), err, core->span) ||
                    !emit_op2(ctx->module, IDM_OP_MAKE_CELL, local, nil_reg, NULL)) ok = idm_error_oom(err, core->span);
            }
        }
        for (size_t i = 0; ok && i < count; i++) {
            uint32_t value_reg = ctx_alloc(ctx);
            uint32_t local = ctx_local_reg(ctx, core->as.letrec.bindings[i].slot);
            if (!compile_expr(core->as.letrec.bindings[i].value, ctx, value_reg, false, err)) ok = false;
            else if (!emit_op2(ctx->module, IDM_OP_STORE_CELL, local, value_reg, NULL)) ok = idm_error_oom(err, core->span);
        }
    }

    size_t jump_over_op = 0;
    if (ok && compiled_count != 0 && !emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, core->span);
    for (size_t i = 0; ok && i < count; i++) {
        if (compiled[i]) ok = compile_callable_bodies(ctx->module_ctx, core->as.letrec.bindings[i].value, &callables[i], ctx->module, err, core->as.letrec.bindings[i].value->span);
    }
    if (ok && compiled_count != 0) ok = patch_here(ctx->module, jump_over_op + 1u, err, core->span, "letrec callable body jump");
    if (ok) ok = compile_expr(core->as.letrec.body, ctx, dst, tail, err);

    destroy_compiled_callables(callables, compiled, count);
    free(callables);
    free(compiled);
    return ok;
}

static bool emit_store_raised_cell(CompileCtx *ctx, const char *name, uint32_t slot, IdmError *err, IdmSpan span) {
    uint32_t local = ctx_local_reg(ctx, slot);
    if (!emit_op1(ctx->module, IDM_OP_LOAD_RAISED, local, NULL)) return idm_error_oom(err, span);
    if (!emit_op2(ctx->module, IDM_OP_MAKE_CELL, local, local, NULL)) return idm_error_oom(err, span);
    if (!idm_bc_note_name(ctx->module, ctx->module->code_count >= 2 ? ctx->module->code_count - 2u : 0u, name)) return idm_error_oom(err, span);
    return true;
}

static bool emit_load_celled_local(CompileCtx *ctx, const char *name, uint32_t slot, uint32_t dst, IdmError *err, IdmSpan span) {
    uint32_t local = ctx_local_reg(ctx, slot);
    if (!emit_named_op2(ctx->module, IDM_OP_LOAD_CELL, dst, local, name, err, span)) return false;
    return true;
}

static bool compile_guard_rescue_only(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    size_t push_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, ctx, dst, false, err)) return false;
    if (!idm_bc_emit_op(ctx->module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_done_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(ctx->module, push_op + 1u, err, core->span, "guard rescue catch target")) return false;
    if (!emit_store_raised_cell(ctx, "_rescue", core->as.guard.rescue_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.handler, ctx, dst, false, err)) return false;
    return patch_here(ctx->module, jump_done_op + 1u, err, core->span, "guard rescue done jump");
}

static bool compile_guard_ensure_only(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    uint32_t scratch = ctx_alloc(ctx);
    size_t push_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, ctx, dst, false, err)) return false;
    if (!idm_bc_emit_op(ctx->module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.cleanup, ctx, scratch, false, err)) return false;
    size_t jump_done_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(ctx->module, push_op + 1u, err, core->span, "guard ensure catch target")) return false;
    if (!emit_store_raised_cell(ctx, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.cleanup, ctx, scratch, false, err)) return false;
    if (!emit_load_celled_local(ctx, "_ensure", core->as.guard.ensure_slot, scratch, err, core->span)) return false;
    if (!emit_op1(ctx->module, IDM_OP_RAISE, scratch, NULL)) return idm_error_oom(err, core->span);
    return patch_here(ctx->module, jump_done_op + 1u, err, core->span, "guard ensure done jump");
}

static bool compile_guard_rescue_ensure(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    uint32_t scratch = ctx_alloc(ctx);
    size_t outer_push_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_RESCUE_PUSH, 0, &outer_push_op)) return idm_error_oom(err, core->span);
    size_t inner_push_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_RESCUE_PUSH, 0, &inner_push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, ctx, dst, false, err)) return false;
    if (!idm_bc_emit_op(ctx->module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_after_rescue_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_after_rescue_op)) return idm_error_oom(err, core->span);
    if (!patch_here(ctx->module, inner_push_op + 1u, err, core->span, "guard rescue catch target")) return false;
    if (!emit_store_raised_cell(ctx, "_rescue", core->as.guard.rescue_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.handler, ctx, dst, false, err)) return false;
    if (!patch_here(ctx->module, jump_after_rescue_op + 1u, err, core->span, "guard rescue done jump")) return false;
    if (!idm_bc_emit_op(ctx->module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.cleanup, ctx, scratch, false, err)) return false;
    size_t jump_done_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(ctx->module, outer_push_op + 1u, err, core->span, "guard ensure catch target")) return false;
    if (!emit_store_raised_cell(ctx, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.cleanup, ctx, scratch, false, err)) return false;
    if (!emit_load_celled_local(ctx, "_ensure", core->as.guard.ensure_slot, scratch, err, core->span)) return false;
    if (!emit_op1(ctx->module, IDM_OP_RAISE, scratch, NULL)) return idm_error_oom(err, core->span);
    return patch_here(ctx->module, jump_done_op + 1u, err, core->span, "guard ensure done jump");
}

static bool compile_guard(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    bool has_handler = core->as.guard.handler != NULL;
    bool has_cleanup = core->as.guard.cleanup != NULL;
    if (!core->as.guard.body || (!has_handler && !has_cleanup)) return idm_error_set(err, core->span, "guard requires a body and handler or cleanup");
    if (has_handler && has_cleanup) return compile_guard_rescue_ensure(core, ctx, dst, err);
    if (has_handler) return compile_guard_rescue_only(core, ctx, dst, err);
    return compile_guard_ensure_only(core, ctx, dst, err);
}

static bool compile_call_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    if (core->as.call.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "too many call arguments");
    IdmPrimitive primitive = 0;
    if (core_direct_primitive_callable(core->as.call.callee, &primitive)) {
        uint32_t mark = ctx_mark(ctx);
        uint32_t argc = (uint32_t)core->as.call.arg_count;
        uint32_t first_arg = ctx_alloc_range(ctx, argc);
        for (uint32_t i = 0; i < argc; i++) {
            if (!compile_expr(core->as.call.args[i], ctx, first_arg + i, false, err)) return false;
        }
        if (!emit_op4(ctx->module, IDM_OP_CALL_PRIMITIVE, dst, (uint32_t)primitive, first_arg, argc, NULL)) return idm_error_oom(err, core->span);
        ctx_reset(ctx, mark);
        return true;
    }
    uint32_t mark = ctx_mark(ctx);
    uint32_t callee = ctx_alloc(ctx);
    uint32_t argc = (uint32_t)core->as.call.arg_count;
    uint32_t first_arg = ctx_alloc_range(ctx, argc);
    if (!compile_expr(core->as.call.callee, ctx, callee, false, err)) return false;
    for (uint32_t i = 0; i < argc; i++) {
        if (!compile_expr(core->as.call.args[i], ctx, first_arg + i, false, err)) return false;
    }
    if (!emit_op5(ctx->module, IDM_OP_CALL, dst, callee, first_arg, argc, tail ? 1u : 0u, NULL)) return idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    return true;
}

static bool compile_match_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    if (core->as.match_expr.scrutinee_count == 0) return idm_error_set(err, core->span, "match requires at least one scrutinee");
    if (core->as.match_expr.scrutinee_count > UINT32_MAX || core->as.match_expr.capture_count > UINT32_MAX || core->as.match_expr.count > UINT32_MAX) {
        return idm_error_set(err, core->span, "match is too large");
    }
    CompiledCallable callable;
    if (!compile_match_entries(core, ctx->module, &callable, err)) return false;
    bool ok = true;
    uint32_t mark = ctx_mark(ctx);
    uint32_t argc = (uint32_t)core->as.match_expr.scrutinee_count;
    uint32_t capture_count = (uint32_t)core->as.match_expr.capture_count;
    uint32_t first_arg = ctx_alloc_range(ctx, argc);
    uint32_t first_capture = ctx_alloc_range(ctx, capture_count);
    for (uint32_t i = 0; ok && i < argc; i++) ok = compile_expr(core->as.match_expr.scrutinees[i], ctx, first_arg + i, false, err);
    for (uint32_t i = 0; ok && i < capture_count; i++) ok = emit_capture_load(ctx, core->as.match_expr.captures[i], first_capture + i, err, core->span);
    size_t op_offset = 0;
    if (ok && !idm_bc_emit_op(ctx->module, IDM_OP_MATCH, &op_offset)) ok = idm_error_oom(err, core->span);
    if (ok) {
        ok = idm_bc_emit(ctx->module, dst, NULL) &&
             idm_bc_emit(ctx->module, first_arg, NULL) &&
             idm_bc_emit(ctx->module, argc, NULL) &&
             idm_bc_emit(ctx->module, (uint32_t)callable.count, NULL) &&
             idm_bc_emit(ctx->module, capture_count, NULL) &&
             idm_bc_emit(ctx->module, first_capture, NULL) &&
             idm_bc_emit(ctx->module, tail ? 1u : 0u, NULL);
        if (!ok) ok = idm_error_oom(err, core->span);
    }
    for (size_t i = 0; ok && i < callable.count; i++) {
        if (!idm_bc_emit(ctx->module, callable.entries[i], NULL)) ok = idm_error_oom(err, core->span);
    }
    (void)op_offset;
    size_t jump_over_op = 0;
    if (ok && !emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    if (ok) ok = compile_match_bodies(ctx->module_ctx, core, &callable, ctx->module, err);
    if (ok) ok = patch_here(ctx->module, jump_over_op + 1u, err, core->span, "match body jump");
    compiled_callable_destroy(&callable);
    return ok;
}

static bool compile_list_pair_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmOpcode op, IdmError *err) {
    uint32_t mark = ctx_mark(ctx);
    uint32_t head = ctx_alloc(ctx);
    uint32_t tail = ctx_alloc(ctx);
    if (!compile_expr(core->as.list_pair.head, ctx, head, false, err)) return false;
    if (!compile_expr(core->as.list_pair.tail, ctx, tail, false, err)) return false;
    if (!emit_op3(ctx->module, op, dst, head, tail, NULL)) return idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    return true;
}

static bool compile_value_sequence_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    if (core->as.value_sequence.count > UINT32_MAX) return idm_error_set(err, core->span, "sequence has too many items");
    uint32_t mark = ctx_mark(ctx);
    uint32_t count = (uint32_t)core->as.value_sequence.count;
    uint32_t first = ctx_alloc_range(ctx, count);
    for (uint32_t i = 0; i < count; i++) {
        if (!compile_expr(core->as.value_sequence.items[i], ctx, first + i, false, err)) return false;
    }
    if (!emit_op4(ctx->module, IDM_OP_MAKE_VALUE_SEQUENCE, dst, (uint32_t)core->as.value_sequence.kind, first, count, NULL)) return idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    return true;
}

static bool syntax_build_needs_payload(IdmSyntaxBuildKind kind) {
    return kind != IDM_SYNTAX_BUILD_NIL;
}

static bool compile_syntax_build_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    uint32_t mark = ctx_mark(ctx);
    uint32_t ctx_reg = ctx_alloc(ctx);
    uint32_t payload = ctx_alloc(ctx);
    if (!compile_expr(core->as.syntax_build.ctx, ctx, ctx_reg, false, err)) return false;
    if (syntax_build_needs_payload(core->as.syntax_build.kind)) {
        if (!compile_expr(core->as.syntax_build.payload, ctx, payload, false, err)) return false;
    } else if (!emit_load_value(ctx, payload, idm_nil(), err, core->span)) return false;
    if (!emit_op4(ctx->module, IDM_OP_MAKE_SYNTAX, dst, (uint32_t)core->as.syntax_build.kind, ctx_reg, payload, NULL)) return idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    return true;
}

static bool compile_string_concat_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, IdmError *err) {
    if (core->as.string_concat.count > UINT32_MAX) return idm_error_set(err, core->span, "string interpolation has too many parts");
    uint32_t mark = ctx_mark(ctx);
    uint32_t count = (uint32_t)core->as.string_concat.count;
    uint32_t first = ctx_alloc_range(ctx, count);
    for (uint32_t i = 0; i < count; i++) {
        if (!compile_expr(core->as.string_concat.items[i], ctx, first + i, false, err)) return false;
    }
    if (!emit_op3(ctx->module, IDM_OP_STRING_CONCAT, dst, first, count, NULL)) return idm_error_oom(err, core->span);
    ctx_reset(ctx, mark);
    return true;
}

static bool compile_cond_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    uint32_t cond = ctx_alloc(ctx);
    if (!compile_expr(core->as.cond_expr.cond, ctx, cond, false, err)) return false;
    size_t jump_false_op = 0;
    if (!emit_op2(ctx->module, IDM_OP_JUMP_IF_FALSE, cond, 0, &jump_false_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.cond_expr.then_branch, ctx, dst, tail, err)) return false;
    size_t jump_end_op = 0;
    if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_end_op)) return idm_error_oom(err, core->span);
    if (ctx->module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
    if (!idm_bc_patch_u32(ctx->module, jump_false_op + 2u, (uint32_t)ctx->module->code_count)) return idm_error_set(err, core->span, "failed to patch cond false jump");
    if (!compile_expr(core->as.cond_expr.else_branch, ctx, dst, tail, err)) return false;
    if (ctx->module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
    if (!idm_bc_patch_u32(ctx->module, jump_end_op + 1u, (uint32_t)ctx->module->code_count)) return idm_error_set(err, core->span, "failed to patch cond end jump");
    return true;
}

static bool compile_do_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    if (core->as.do_expr.count == 0) return emit_load_value(ctx, dst, idm_nil(), err, core->span);
    uint32_t scratch = ctx_alloc(ctx);
    for (size_t i = 0; i < core->as.do_expr.count; i++) {
        bool last = i + 1u == core->as.do_expr.count;
        if (!compile_expr(core->as.do_expr.items[i], ctx, last ? dst : scratch, last ? tail : false, err)) return false;
    }
    return true;
}

static bool compile_expr(IdmCore *core, CompileCtx *ctx, uint32_t dst, bool tail, IdmError *err) {
    if (!core) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null core expression");
    if (!idm_bc_note_span(ctx->module, core->span)) return idm_error_oom(err, core->span);
    switch (core->kind) {
        case IDM_CORE_LITERAL: {
            return emit_load_value(ctx, dst, core->as.literal, err, core->span);
        }
        case IDM_CORE_LOCAL_REF:
            if (core->local_celled) return emit_named_op2(ctx->module, IDM_OP_LOAD_CELL, dst, ctx_local_reg(ctx, core->as.slot_ref.slot), core->as.slot_ref.name, err, core->span);
            if (!emit_named_op2(ctx->module, IDM_OP_MOVE, dst, ctx_local_reg(ctx, core->as.slot_ref.slot), core->as.slot_ref.name, err, core->span)) return false;
            return true;
        case IDM_CORE_ARG_REF:
            return emit_named_op2(ctx->module, IDM_OP_MOVE, dst, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span);
        case IDM_CORE_CAPTURE_REF:
            if (!emit_named_op2(ctx->module, IDM_OP_LOAD_CAPTURE, dst, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span)) return false;
            if (core->local_celled && !emit_op2(ctx->module, IDM_OP_LOAD_CELL, dst, dst, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_ENV_REF:
            return emit_named_op2(ctx->module, IDM_OP_LOAD_ENV, dst, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span);
        case IDM_CORE_PACKAGE_REF: {
            uint32_t key_const = 0;
            if (!idm_bc_add_const(ctx->module, core->as.package_ref.env_key, &key_const)) return idm_error_oom(err, core->span);
            if (!emit_op3(ctx->module, IDM_OP_LOAD_PACKAGE_SLOT, dst, key_const, core->as.package_ref.slot, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
        case IDM_CORE_CALL:
            return compile_call_expr(core, ctx, dst, tail, err);
        case IDM_CORE_LIST_CONS:
            return compile_list_pair_expr(core, ctx, dst, IDM_OP_LIST_CONS, err);
        case IDM_CORE_LIST_APPEND:
            return compile_list_pair_expr(core, ctx, dst, IDM_OP_LIST_APPEND, err);
        case IDM_CORE_VALUE_SEQUENCE:
            return compile_value_sequence_expr(core, ctx, dst, err);
        case IDM_CORE_SYNTAX_BUILD:
            return compile_syntax_build_expr(core, ctx, dst, err);
        case IDM_CORE_STRING_CONCAT:
            return compile_string_concat_expr(core, ctx, dst, err);
        case IDM_CORE_COND:
            return compile_cond_expr(core, ctx, dst, tail, err);
        case IDM_CORE_MATCH:
            return compile_match_expr(core, ctx, dst, tail, err);
        case IDM_CORE_DO:
            return compile_do_expr(core, ctx, dst, tail, err);
        case IDM_CORE_BIND_LOCAL:
            if (!compile_expr(core->as.bind_local.value, ctx, ctx_local_reg(ctx, core->as.bind_local.slot), false, err)) return false;
            if (core->local_celled && !emit_op2(ctx->module, IDM_OP_MAKE_CELL, ctx_local_reg(ctx, core->as.bind_local.slot), ctx_local_reg(ctx, core->as.bind_local.slot), NULL)) return idm_error_oom(err, core->span);
            return compile_expr(core->as.bind_local.body, ctx, dst, tail, err);
        case IDM_CORE_FN: {
            return compile_callable_literal(core, ctx, dst, err);
        }
        case IDM_CORE_FN_MULTI: {
            return compile_callable_literal(core, ctx, dst, err);
        }
        case IDM_CORE_LETREC: {
            return compile_letrec(core, ctx, dst, tail, err);
        }
        case IDM_CORE_RECEIVE: {
            if (!core->as.receive.receiver || !core->as.receive.timeout || !core->as.receive.timeout_body) return idm_error_set(err, core->span, "receive is missing a required component");
            uint32_t timeout = ctx_alloc(ctx);
            uint32_t receiver = ctx_alloc(ctx);
            if (!compile_expr(core->as.receive.timeout, ctx, timeout, false, err)) return false;
            if (!compile_expr(core->as.receive.receiver, ctx, receiver, false, err)) return false;
            size_t recv_op = 0;
            if (!emit_op5(ctx->module, IDM_OP_RECV, dst, timeout, receiver, 0, tail ? 1u : 0u, &recv_op)) return idm_error_oom(err, core->span);
            size_t jump_done_op = 0;
            if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
            if (ctx->module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(ctx->module, recv_op + 4u, (uint32_t)ctx->module->code_count)) return idm_error_set(err, core->span, "failed to patch receive timeout target");
            if (!compile_expr(core->as.receive.timeout_body, ctx, dst, tail, err)) return false;
            if (ctx->module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(ctx->module, jump_done_op + 1u, (uint32_t)ctx->module->code_count)) return idm_error_set(err, core->span, "failed to patch receive done jump");
            return true;
        }
        case IDM_CORE_GUARD:
            return compile_guard(core, ctx, dst, err);
        case IDM_CORE_USE_PACKAGE: {
            uint32_t fn_off = 0;
            if (core->as.use_package.module) {
                if (!compile_link_package_once(ctx, core, &fn_off, err)) return false;
            }
            uint32_t env_key_index = 0;
            if (!idm_bc_add_const(ctx->module, core->as.use_package.env_key, &env_key_index)) return idm_error_oom(err, core->span);
            if (!emit_op1(ctx->module, IDM_OP_PUSH_PACKAGE_ENV, env_key_index, NULL)) return idm_error_oom(err, core->span);
            if (core->as.use_package.module) {
                uint32_t tmp = ctx_alloc(ctx);
                uint32_t closure = ctx_alloc(ctx);
                if (!emit_op2(ctx->module, IDM_OP_MAKE_CLOSURE, closure, fn_off + core->as.use_package.init_fn, NULL)) return idm_error_oom(err, core->span);
                if (!emit_op5(ctx->module, IDM_OP_CALL, tmp, closure, 0u, 0u, 0u, NULL)) return idm_error_oom(err, core->span);
            }
            if (!idm_bc_emit_op(ctx->module, IDM_OP_POP_PACKAGE_ENV, NULL)) return idm_error_oom(err, core->span);
            return compile_expr(core->as.use_package.cont, ctx, dst, tail, err);
        }
        case IDM_CORE_RECORD_CONSTRUCT: {
            if (core->as.record_construct.count > UINT32_MAX) return idm_error_set(err, core->span, "record has too many fields");
            uint32_t count = (uint32_t)core->as.record_construct.count;
            uint32_t first = ctx_alloc_range(ctx, count);
            for (uint32_t i = 0; i < count; i++) {
                if (!compile_expr(core->as.record_construct.field_values[i], ctx, first + i, false, err)) return false;
            }
            uint32_t type_const = 0;
            if (!add_const_symbol(ctx, core->as.record_construct.type, &type_const, err, core->span)) return false;
            if (!emit_op4(ctx->module, IDM_OP_MAKE_RECORD, dst, type_const, first, count, NULL)) return idm_error_oom(err, core->span);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t field_const = 0;
                uint32_t contract_type = UINT32_MAX;
                if (!add_const_symbol(ctx, core->as.record_construct.field_names[i], &field_const, err, core->span)) return false;
                if (core->as.record_construct.field_has_contracts[i] &&
                    !idm_bc_add_type_term(ctx->module, &core->as.record_construct.field_contracts[i], &contract_type)) {
                    return idm_error_oom(err, core->span);
                }
                if (!idm_bc_emit(ctx->module, field_const, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(ctx->module, contract_type, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_RECORD_FIELD: {
            uint32_t receiver = ctx_alloc(ctx);
            if (!compile_expr(core->as.record_field.receiver, ctx, receiver, false, err)) return false;
            uint32_t type_const = 0;
            uint32_t field_const = 0;
            if (!add_const_symbol(ctx, core->as.record_field.type, &type_const, err, core->span)) return false;
            if (!add_const_symbol(ctx, core->as.record_field.field, &field_const, err, core->span)) return false;
            if (!emit_op5(ctx->module, IDM_OP_RECORD_FIELD, dst, receiver, type_const, field_const, core->as.record_field.field_index, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
        case IDM_CORE_RECORD_IS: {
            uint32_t value = ctx_alloc(ctx);
            if (!compile_expr(core->as.record_is.value, ctx, value, false, err)) return false;
            uint32_t type_const = 0;
            if (!add_const_symbol(ctx, core->as.record_is.type, &type_const, err, core->span)) return false;
            if (!emit_op3(ctx->module, IDM_OP_RECORD_IS, dst, value, type_const, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
    }
    return idm_error_set(err, core->span, "unknown core node");
}

static bool compiler_mark_module_verified(IdmBytecodeModule *module, IdmError *err) {
    if (!idm_bc_verify(module, err)) return false;
    module->verified = true;
    return true;
}

bool idm_core_compile_expression(IdmRuntime *rt, IdmCore *core, IdmBytecodeModule *module, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "core.compile_expression");
    size_t code_start = module->code_count;
    size_t fn_start = module->function_count;
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "<expression>", 0, locals, 0, &fn)) {
        idm_profile_scope_end(&prof);
        return idm_error_oom(err, core ? core->span : idm_span_unknown(NULL));
    }
    CompileModuleCtx module_ctx;
    memset(&module_ctx, 0, sizeof(module_ctx));
    module_ctx.rt = rt;
    bool ok = compile_function_code(&module_ctx, module, fn, core, err, core ? core->span : idm_span_unknown(NULL), NULL, 0);
    compile_module_ctx_destroy(&module_ctx);
    if (ok) ok = compiler_mark_module_verified(module, err);
    if (ok) {
        idm_profile_count("core.compile_expression.code_words", (uint64_t)(module->code_count - code_start));
        idm_profile_count("core.compile_expression.functions", (uint64_t)(module->function_count - fn_start));
    }
    idm_profile_scope_end(&prof);
    return ok;
}

bool idm_core_compile_function_body(IdmRuntime *rt, IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "core.compile_function_body");
    size_t code_start = module->code_count;
    size_t fn_start = module->function_count;
    if (!body) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null function body");
    }
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(body);
    if (!idm_bc_add_function(module, name ? name : "<function>", arity, locals, 0, &fn)) {
        idm_profile_scope_end(&prof);
        return idm_error_oom(err, body->span);
    }
    CompileModuleCtx module_ctx;
    memset(&module_ctx, 0, sizeof(module_ctx));
    module_ctx.rt = rt;
    bool ok = compile_function_code(&module_ctx, module, fn, body, err, body->span, NULL, 0);
    compile_module_ctx_destroy(&module_ctx);
    if (!ok) {
        idm_profile_scope_end(&prof);
        return false;
    }
    if (!compiler_mark_module_verified(module, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    if (out_function) *out_function = fn;
    idm_profile_count("core.compile_function_body.code_words", (uint64_t)(module->code_count - code_start));
    idm_profile_count("core.compile_function_body.functions", (uint64_t)(module->function_count - fn_start));
    idm_profile_scope_end(&prof);
    return true;
}

static bool compile_function_entry(CompileModuleCtx *module_ctx, IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!fn || !core_is_callable(fn)) return idm_error_set(err, fn ? fn->span : idm_span_unknown(NULL), "expected a function");
    CompiledCallable callable;
    if (!compile_callable_entries(fn, module, &callable, err, fn->span)) return false;
    bool ok = compile_callable_bodies(module_ctx, fn, &callable, module, err, fn->span);
    if (ok && out_function) *out_function = callable.entries[0];
    compiled_callable_destroy(&callable);
    return ok;
}

bool idm_core_compile_function(IdmRuntime *rt, IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "core.compile_function");
    size_t code_start = module->code_count;
    size_t fn_start = module->function_count;
    if (!fn || !core_is_callable(fn)) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, fn ? fn->span : idm_span_unknown(NULL), "expected a function");
    }
    CompileModuleCtx module_ctx;
    memset(&module_ctx, 0, sizeof(module_ctx));
    module_ctx.rt = rt;
    bool ok = true;
    if (core_is_primitive_only_callable(fn)) {
        ok = compile_function_entry(&module_ctx, fn, module, out_function, err);
        compile_module_ctx_destroy(&module_ctx);
        if (ok) ok = compiler_mark_module_verified(module, err);
        if (ok) {
            idm_profile_count("core.compile_function.code_words", (uint64_t)(module->code_count - code_start));
            idm_profile_count("core.compile_function.functions", (uint64_t)(module->function_count - fn_start));
        }
        idm_profile_scope_end(&prof);
        return ok;
    }
    size_t capture_count = fn->kind == IDM_CORE_FN ? fn->as.fn.capture_count : fn->as.fn_multi.capture_count;
    if (capture_count != 0) {
        compile_module_ctx_destroy(&module_ctx);
        idm_profile_scope_end(&prof);
        return idm_error_set(err, fn->span, "captured function cannot be compiled as a bare module entry");
    }
    if (fn->kind == IDM_CORE_FN) {
        ok = compile_function_entry(&module_ctx, fn, module, out_function, err);
        compile_module_ctx_destroy(&module_ctx);
        if (ok) ok = compiler_mark_module_verified(module, err);
        if (ok) {
            idm_profile_count("core.compile_function.code_words", (uint64_t)(module->code_count - code_start));
            idm_profile_count("core.compile_function.functions", (uint64_t)(module->function_count - fn_start));
        }
        idm_profile_scope_end(&prof);
        return ok;
    }

    IdmCore *arg = idm_core_arg_ref("__syntax__", 0, fn->span);
    IdmCore *call = idm_core_call(fn, fn->span);
    const char *name = fn->as.fn_multi.name ? fn->as.fn_multi.name : "<transformer>";
    IdmCore *wrapper = call ? idm_core_fn(name, 1, call, fn->span) : NULL;
    bool arg_attached = false;
    ok = arg && call && wrapper;
    if (ok) {
        ok = idm_core_call_add_arg(call, arg);
        arg_attached = ok;
    }
    if (ok) ok = compile_function_entry(&module_ctx, wrapper, module, out_function, err);
    if (call) call->as.call.callee = NULL;
    if (wrapper) idm_core_free(wrapper);
    else {
        if (call) {
            call->as.call.callee = NULL;
            idm_core_free(call);
        }
    }
    if (!arg_attached) idm_core_free(arg);
    if (!ok && err && !err->present) idm_error_oom(err, fn->span);
    compile_module_ctx_destroy(&module_ctx);
    if (ok) ok = compiler_mark_module_verified(module, err);
    if (ok) {
        idm_profile_count("core.compile_function.code_words", (uint64_t)(module->code_count - code_start));
        idm_profile_count("core.compile_function.functions", (uint64_t)(module->function_count - fn_start));
    }
    idm_profile_scope_end(&prof);
    return ok;
}

bool idm_core_compile_main(IdmRuntime *rt, IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "core.compile_main");
    size_t code_start = module->code_count;
    size_t fn_start = module->function_count;
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "main", 0, locals, 0, &fn)) {
        idm_profile_scope_end(&prof);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    CompileModuleCtx module_ctx;
    memset(&module_ctx, 0, sizeof(module_ctx));
    module_ctx.rt = rt;
    bool ok = compile_function_code(&module_ctx, module, fn, core, err, idm_span_unknown(NULL), NULL, 0);
    compile_module_ctx_destroy(&module_ctx);
    if (!ok) {
        idm_profile_scope_end(&prof);
        return false;
    }
    if (!compiler_mark_module_verified(module, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    if (out_function) *out_function = fn;
    idm_profile_count("core.compile_main.code_words", (uint64_t)(module->code_count - code_start));
    idm_profile_count("core.compile_main.functions", (uint64_t)(module->function_count - fn_start));
    idm_profile_scope_end(&prof);
    return true;
}

static bool dump_value(IdmBuffer *buf, IdmValue value) {
    return idm_value_write(buf, value);
}

static const char *capture_kind_tag(IdmCaptureKind kind) {
    switch (kind) {
        case IDM_CAP_LOCAL: return "";
        case IDM_CAP_ARG: return "@";
        case IDM_CAP_UPVALUE: return "^";
    }
    return "?";
}

static bool clause_compact(IdmBuffer *buf, const IdmFnClause *clause);

bool idm_core_dump(IdmBuffer *buf, const IdmCore *core) {
    if (!core) return idm_buf_append(buf, "#<null-core>");
    switch (core->kind) {
        case IDM_CORE_LITERAL:
            return dump_value(buf, core->as.literal);
        case IDM_CORE_ARG_REF:
            return idm_buf_appendf(buf, "(arg %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_LOCAL_REF:
            return idm_buf_appendf(buf, "(local %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_CAPTURE_REF:
            return idm_buf_appendf(buf, "(capture %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_CALL:
            if (!idm_buf_append_char(buf, '(')) return false;
            if (!idm_core_dump(buf, core->as.call.callee)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_core_dump(buf, core->as.call.args[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_LIST_CONS:
            return idm_buf_append(buf, "(list-cons ") &&
                   idm_core_dump(buf, core->as.list_pair.head) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.list_pair.tail) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_LIST_APPEND:
            return idm_buf_append(buf, "(list-append ") &&
                   idm_core_dump(buf, core->as.list_pair.head) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.list_pair.tail) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_VALUE_SEQUENCE:
            if (!idm_buf_appendf(buf, "(make-%s", idm_value_sequence_kind_name(core->as.value_sequence.kind))) return false;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.value_sequence.items[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_SYNTAX_BUILD:
            if (!idm_buf_appendf(buf, "(make-syntax:%s ", idm_syntax_build_kind_name(core->as.syntax_build.kind))) return false;
            if (!idm_core_dump(buf, core->as.syntax_build.ctx)) return false;
            if (syntax_build_needs_payload(core->as.syntax_build.kind)) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.syntax_build.payload)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_STRING_CONCAT:
            if (!idm_buf_append(buf, "(string-concat")) return false;
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.string_concat.items[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_COND:
            return idm_buf_append(buf, "(cond ") &&
                   idm_core_dump(buf, core->as.cond_expr.cond) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.cond_expr.then_branch) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.cond_expr.else_branch) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_MATCH:
            if (!idm_buf_append(buf, "(match")) return false;
            if (core->as.match_expr.capture_count != 0) {
                if (!idm_buf_append(buf, " captures=") || !idm_buf_append_char(buf, '[')) return false;
                for (size_t i = 0; i < core->as.match_expr.capture_count; i++) {
                    if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                    if (!idm_buf_appendf(buf, "%s%s#%u", capture_kind_tag(core->as.match_expr.captures[i].kind), core->as.match_expr.captures[i].name, core->as.match_expr.captures[i].index)) return false;
                }
                if (!idm_buf_append_char(buf, ']')) return false;
            }
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.match_expr.scrutinees[i])) return false;
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !clause_compact(buf, &core->as.match_expr.clauses[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_DO:
            if (!idm_buf_append(buf, "(do")) return false;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_core_dump(buf, core->as.do_expr.items[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_BIND_LOCAL:
            return idm_buf_appendf(buf, "(bind-local %s#%u ", core->as.bind_local.name, core->as.bind_local.slot) &&
                   idm_core_dump(buf, core->as.bind_local.value) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.bind_local.body) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_FN:
            if (!idm_buf_appendf(buf, "(fn %s/%u", core->as.fn.name, core->as.fn.arity)) return false;
            if (core->as.fn.capture_count != 0) {
                if (!idm_buf_append(buf, " captures=")) return false;
                if (!idm_buf_append_char(buf, '[')) return false;
                for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                    if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                    if (!idm_buf_appendf(buf, "%s%s#%u", capture_kind_tag(core->as.fn.captures[i].kind), core->as.fn.captures[i].name, core->as.fn.captures[i].index)) return false;
                }
                if (!idm_buf_append_char(buf, ']')) return false;
            }
            if (core->as.fn.guard) {
                if (!idm_buf_append(buf, " guard=")) return false;
                if (!idm_core_dump(buf, core->as.fn.guard)) return false;
            }
            return idm_buf_append_char(buf, ' ') && idm_core_dump(buf, core->as.fn.body) && idm_buf_append_char(buf, ')');
        case IDM_CORE_FN_MULTI:
            if (!idm_buf_appendf(buf, "(fn-multi %s", core->as.fn_multi.name)) return false;
            if (core->as.fn_multi.capture_count != 0) {
                if (!idm_buf_append(buf, " captures=")) return false;
                if (!idm_buf_append_char(buf, '[')) return false;
                for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                    if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                    if (!idm_buf_appendf(buf, "%s%s#%u", capture_kind_tag(core->as.fn_multi.captures[i].kind), core->as.fn_multi.captures[i].name, core->as.fn_multi.captures[i].index)) return false;
                }
                if (!idm_buf_append_char(buf, ']')) return false;
            }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !clause_compact(buf, &core->as.fn_multi.clauses[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_LETREC:
            if (!idm_buf_append(buf, "(letrec (")) return false;
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_buf_appendf(buf, "(%s#%u", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
                if (core->as.letrec.bindings[i].has_env_key && (!idm_buf_append_char(buf, '@') || !dump_value(buf, core->as.letrec.bindings[i].env_key))) return false;
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_core_dump(buf, core->as.letrec.bindings[i].value)) return false;
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append(buf, ") ") && idm_core_dump(buf, core->as.letrec.body) && idm_buf_append_char(buf, ')');
        case IDM_CORE_RECEIVE:
            return idm_buf_append(buf, "(receive ") &&
                   idm_core_dump(buf, core->as.receive.receiver) &&
                   idm_buf_append(buf, " timeout ") &&
                   idm_core_dump(buf, core->as.receive.timeout) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.receive.timeout_body) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_GUARD: {
            if (!idm_buf_append(buf, "(guard ") || !idm_core_dump(buf, core->as.guard.body)) return false;
            if (core->as.guard.handler) {
                if (!idm_buf_appendf(buf, " rescue _rescue#%u ", core->as.guard.rescue_slot) || !idm_core_dump(buf, core->as.guard.handler)) return false;
            }
            if (core->as.guard.cleanup) {
                if (!idm_buf_appendf(buf, " ensure _ensure#%u ", core->as.guard.ensure_slot) || !idm_core_dump(buf, core->as.guard.cleanup)) return false;
            }
            return idm_buf_append_char(buf, ')');
        }
        case IDM_CORE_ENV_REF:
            return idm_buf_appendf(buf, "(env %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_PACKAGE_REF:
            return idm_buf_append(buf, "(package-slot ") &&
                   dump_value(buf, core->as.package_ref.env_key) &&
                   idm_buf_appendf(buf, " %s#%u)", core->as.package_ref.name, core->as.package_ref.slot);
        case IDM_CORE_USE_PACKAGE:
            if (!idm_buf_append(buf, "(use-package ") || !dump_value(buf, core->as.use_package.env_key) ||
                !idm_buf_appendf(buf, " init=%u ", core->as.use_package.init_fn)) {
                return false;
            }
            return idm_core_dump(buf, core->as.use_package.cont) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_RECORD_CONSTRUCT:
            if (!idm_buf_append(buf, "(record ") || !idm_buf_append(buf, core_symbol_text(core->as.record_construct.type))) return false;
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (!idm_buf_append(buf, " (field ") ||
                    !idm_buf_append(buf, core_symbol_text(core->as.record_construct.field_names[i])) ||
                    !idm_buf_append(buf, " : ")) return false;
                if (core->as.record_construct.field_has_contracts[i]) {
                    if (!idm_type_term_write(buf, &core->as.record_construct.field_contracts[i])) return false;
                } else if (!idm_buf_append(buf, "_")) return false;
                if (!idm_buf_append_char(buf, ' ') ||
                    !idm_core_dump(buf, core->as.record_construct.field_values[i]) ||
                    !idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_RECORD_FIELD:
            return idm_buf_append(buf, "(record-field ") &&
                   idm_core_dump(buf, core->as.record_field.receiver) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_buf_append(buf, core_symbol_text(core->as.record_field.type)) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_buf_append(buf, core_symbol_text(core->as.record_field.field)) &&
                   idm_buf_appendf(buf, " %u", core->as.record_field.field_index) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_RECORD_IS:
            return idm_buf_append(buf, "(record-is ") &&
                   idm_core_dump(buf, core->as.record_is.value) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_buf_append(buf, core_symbol_text(core->as.record_is.type)) &&
                   idm_buf_append_char(buf, ')');
    }
    return false;
}

#define IDM_CORE_PRETTY_WIDTH 80

static bool pretty_newline(IdmBuffer *buf, size_t indent) {
    if (!idm_buf_append_char(buf, '\n')) return false;
    for (size_t i = 0; i < indent; i++) {
        if (!idm_buf_append_char(buf, ' ')) return false;
    }
    return true;
}

static bool core_atomic(const IdmCore *core) {
    switch (core->kind) {
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
            return true;
        default:
            return false;
    }
}

static bool dump_captures(IdmBuffer *buf, const IdmCapture *captures, size_t count) {
    if (!idm_buf_append(buf, " captures=[")) return false;
    for (size_t i = 0; i < count; i++) {
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!idm_buf_appendf(buf, "%s%s#%u", capture_kind_tag(captures[i].kind), captures[i].name, captures[i].index)) return false;
    }
    return idm_buf_append_char(buf, ']');
}

static bool clause_compact(IdmBuffer *buf, const IdmFnClause *clause) {
    if (clause->primitive_backed) {
        return idm_buf_append_char(buf, '(') &&
               append_arity(buf, &clause->call_arity) &&
               idm_buf_appendf(buf, " primitive %s)", idm_primitive_name(clause->primitive));
    }
    if (!idm_buf_appendf(buf, "(/%u ", clause->arity)) return false;
    if (clause->guard) {
        if (!idm_buf_append(buf, "guard ") || !idm_core_dump(buf, clause->guard) || !idm_buf_append_char(buf, ' ')) return false;
    }
    if (!idm_core_dump(buf, clause->body)) return false;
    return idm_buf_append_char(buf, ')');
}

static bool letrec_bindings_compact(IdmBuffer *buf, const IdmCore *core) {
    if (!idm_buf_append_char(buf, '(')) return false;
    for (size_t i = 0; i < core->as.letrec.count; i++) {
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!idm_buf_appendf(buf, "(%s#%u", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
        if (core->as.letrec.bindings[i].has_env_key && (!idm_buf_append_char(buf, '@') || !dump_value(buf, core->as.letrec.bindings[i].env_key))) return false;
        if (!idm_buf_append_char(buf, ' ')) return false;
        if (!idm_core_dump(buf, core->as.letrec.bindings[i].value)) return false;
        if (!idm_buf_append_char(buf, ')')) return false;
    }
    return idm_buf_append_char(buf, ')');
}

static bool core_pretty(IdmBuffer *buf, const IdmCore *core, size_t indent);

static bool pretty_marker_child(IdmBuffer *buf, size_t indent, const char *marker, const IdmCore *child) {
    if (!idm_buf_append(buf, marker)) return false;
    return core_pretty(buf, child, indent + strlen(marker));
}

static bool core_pretty(IdmBuffer *buf, const IdmCore *core, size_t indent) {
    if (!core) return idm_buf_append(buf, "#<null-core>");

    IdmBuffer compact;
    idm_buf_init(&compact);
    if (!idm_core_dump(&compact, core)) { idm_buf_destroy(&compact); return false; }
    if (core_atomic(core) || indent + compact.len <= IDM_CORE_PRETTY_WIDTH) {
        bool ok = idm_buf_append_n(buf, compact.data ? compact.data : "", compact.len);
        idm_buf_destroy(&compact);
        return ok;
    }
    idm_buf_destroy(&compact);

    size_t ci = indent + 2;
    switch (core->kind) {
        case IDM_CORE_CALL:
            if (!idm_buf_append_char(buf, '(')) return false;
            if (!core_pretty(buf, core->as.call.callee, indent + 1)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.call.args[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_LIST_CONS:
        case IDM_CORE_LIST_APPEND:
            if (!idm_buf_append(buf, core->kind == IDM_CORE_LIST_CONS ? "(list-cons" : "(list-append")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.list_pair.head, ci)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.list_pair.tail, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_VALUE_SEQUENCE:
            if (!idm_buf_appendf(buf, "(make-%s", idm_value_sequence_kind_name(core->as.value_sequence.kind))) return false;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.value_sequence.items[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_SYNTAX_BUILD:
            if (!idm_buf_appendf(buf, "(make-syntax:%s", idm_syntax_build_kind_name(core->as.syntax_build.kind))) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.syntax_build.ctx, ci)) return false;
            if (syntax_build_needs_payload(core->as.syntax_build.kind) &&
                (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.syntax_build.payload, ci))) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_STRING_CONCAT:
            if (!idm_buf_append(buf, "(string-concat")) return false;
            for (size_t i = 0; i < core->as.string_concat.count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.string_concat.items[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_COND:
            if (!idm_buf_append(buf, "(cond")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.cond_expr.cond, ci)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.cond_expr.then_branch, ci)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.cond_expr.else_branch, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_MATCH:
            if (!idm_buf_append(buf, "(match")) return false;
            if (core->as.match_expr.capture_count != 0 && !dump_captures(buf, core->as.match_expr.captures, core->as.match_expr.capture_count)) return false;
            for (size_t i = 0; i < core->as.match_expr.scrutinee_count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.match_expr.scrutinees[i], ci)) return false;
            }
            for (size_t i = 0; i < core->as.match_expr.count; i++) {
                const IdmFnClause *clause = &core->as.match_expr.clauses[i];
                if (!pretty_newline(buf, ci)) return false;
                IdmBuffer cc;
                idm_buf_init(&cc);
                if (!clause_compact(&cc, clause)) { idm_buf_destroy(&cc); return false; }
                if (ci + cc.len <= IDM_CORE_PRETTY_WIDTH) {
                    bool ok = idm_buf_append_n(buf, cc.data ? cc.data : "", cc.len);
                    idm_buf_destroy(&cc);
                    if (!ok) return false;
                    continue;
                }
                idm_buf_destroy(&cc);
                if (!idm_buf_appendf(buf, "(/%u", clause->arity)) return false;
                if (clause->guard) {
                    if (!idm_buf_append(buf, " guard ") || !idm_core_dump(buf, clause->guard)) return false;
                }
                if (!pretty_newline(buf, ci + 2) || !core_pretty(buf, clause->body, ci + 2)) return false;
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_DO:
            if (!idm_buf_append(buf, "(do")) return false;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.do_expr.items[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_BIND_LOCAL:
            if (!idm_buf_appendf(buf, "(bind-local %s#%u", core->as.bind_local.name, core->as.bind_local.slot)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.bind_local.value, ci)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.bind_local.body, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_FN:
            if (!idm_buf_appendf(buf, "(fn %s/%u", core->as.fn.name, core->as.fn.arity)) return false;
            if (core->as.fn.capture_count != 0 && !dump_captures(buf, core->as.fn.captures, core->as.fn.capture_count)) return false;
            if (core->as.fn.guard) {
                if (!idm_buf_append(buf, " guard=") || !idm_core_dump(buf, core->as.fn.guard)) return false;
            }
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.fn.body, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_FN_MULTI:
            if (!idm_buf_appendf(buf, "(fn-multi %s", core->as.fn_multi.name)) return false;
            if (core->as.fn_multi.capture_count != 0 && !dump_captures(buf, core->as.fn_multi.captures, core->as.fn_multi.capture_count)) return false;
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                const IdmFnClause *clause = &core->as.fn_multi.clauses[i];
                if (!pretty_newline(buf, ci)) return false;
                IdmBuffer cc;
                idm_buf_init(&cc);
                if (!clause_compact(&cc, clause)) { idm_buf_destroy(&cc); return false; }
                if (ci + cc.len <= IDM_CORE_PRETTY_WIDTH) {
                    bool ok = idm_buf_append_n(buf, cc.data ? cc.data : "", cc.len);
                    idm_buf_destroy(&cc);
                    if (!ok) return false;
                    continue;
                }
                idm_buf_destroy(&cc);
                if (clause->primitive_backed) {
                    if (!clause_compact(buf, clause)) return false;
                    continue;
                }
                if (!idm_buf_appendf(buf, "(/%u", clause->arity)) return false;
                if (clause->guard) {
                    if (!idm_buf_append(buf, " guard ") || !idm_core_dump(buf, clause->guard)) return false;
                }
                if (!pretty_newline(buf, ci + 2) || !core_pretty(buf, clause->body, ci + 2)) return false;
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_LETREC:
            if (!idm_buf_append(buf, "(letrec")) return false;
            if (!pretty_newline(buf, ci)) return false;
            {
                IdmBuffer bb;
                idm_buf_init(&bb);
                if (!letrec_bindings_compact(&bb, core)) { idm_buf_destroy(&bb); return false; }
                if (ci + bb.len <= IDM_CORE_PRETTY_WIDTH) {
                    bool ok = idm_buf_append_n(buf, bb.data ? bb.data : "", bb.len);
                    idm_buf_destroy(&bb);
                    if (!ok) return false;
                } else {
                    idm_buf_destroy(&bb);
                    if (!idm_buf_append_char(buf, '(')) return false;
                    for (size_t i = 0; i < core->as.letrec.count; i++) {
                        if (i != 0 && !pretty_newline(buf, ci + 1)) return false;
                        IdmBuffer pfx;
                        idm_buf_init(&pfx);
                        if (!idm_buf_appendf(&pfx, "(%s#%u", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) { idm_buf_destroy(&pfx); return false; }
                        if (core->as.letrec.bindings[i].has_env_key && (!idm_buf_append_char(&pfx, '@') || !dump_value(&pfx, core->as.letrec.bindings[i].env_key))) { idm_buf_destroy(&pfx); return false; }
                        if (!idm_buf_append_char(&pfx, ' ')) { idm_buf_destroy(&pfx); return false; }
                        size_t vcol = ci + 1 + pfx.len;
                        bool ok = idm_buf_append_n(buf, pfx.data ? pfx.data : "", pfx.len);
                        idm_buf_destroy(&pfx);
                        if (!ok || !core_pretty(buf, core->as.letrec.bindings[i].value, vcol) || !idm_buf_append_char(buf, ')')) return false;
                    }
                    if (!idm_buf_append_char(buf, ')')) return false;
                }
            }
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.letrec.body, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_RECEIVE:
            if (!idm_buf_append(buf, "(receive")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.receive.receiver, ci)) return false;
            if (!pretty_newline(buf, ci) || !pretty_marker_child(buf, ci, "timeout ", core->as.receive.timeout)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.receive.timeout_body, ci)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_GUARD:
            if (!idm_buf_append(buf, "(guard")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.guard.body, ci)) return false;
            if (core->as.guard.handler) {
                IdmBuffer m;
                idm_buf_init(&m);
                if (!idm_buf_appendf(&m, "rescue _rescue#%u ", core->as.guard.rescue_slot)) { idm_buf_destroy(&m); return false; }
                bool ok = pretty_newline(buf, ci) && idm_buf_append_n(buf, m.data ? m.data : "", m.len) && core_pretty(buf, core->as.guard.handler, ci + m.len);
                idm_buf_destroy(&m);
                if (!ok) return false;
            }
            if (core->as.guard.cleanup) {
                IdmBuffer m;
                idm_buf_init(&m);
                if (!idm_buf_appendf(&m, "ensure _ensure#%u ", core->as.guard.ensure_slot)) { idm_buf_destroy(&m); return false; }
                bool ok = pretty_newline(buf, ci) && idm_buf_append_n(buf, m.data ? m.data : "", m.len) && core_pretty(buf, core->as.guard.cleanup, ci + m.len);
                idm_buf_destroy(&m);
                if (!ok) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_USE_PACKAGE: {
            if (!idm_buf_append(buf, "(use-package ") || !dump_value(buf, core->as.use_package.env_key) ||
                !idm_buf_appendf(buf, " init=%u", core->as.use_package.init_fn)) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.use_package.cont, ci)) return false;
            return idm_buf_append_char(buf, ')');
        }
        case IDM_CORE_RECORD_CONSTRUCT:
            if (!idm_buf_append(buf, "(record ") || !idm_buf_append(buf, core_symbol_text(core->as.record_construct.type))) return false;
            for (size_t i = 0; i < core->as.record_construct.count; i++) {
                if (!pretty_newline(buf, ci) ||
                    !idm_buf_append(buf, "(field ") ||
                    !idm_buf_append(buf, core_symbol_text(core->as.record_construct.field_names[i])) ||
                    !idm_buf_append(buf, " : ")) return false;
                if (core->as.record_construct.field_has_contracts[i]) {
                    if (!idm_type_term_write(buf, &core->as.record_construct.field_contracts[i])) return false;
                } else if (!idm_buf_append(buf, "_")) return false;
                if (!pretty_newline(buf, ci + 2) ||
                    !core_pretty(buf, core->as.record_construct.field_values[i], ci + 2) ||
                    !idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_RECORD_FIELD:
            if (!idm_buf_append(buf, "(record-field")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.record_field.receiver, ci)) return false;
            if (!pretty_newline(buf, ci) || !idm_buf_append(buf, core_symbol_text(core->as.record_field.type))) return false;
            if (!pretty_newline(buf, ci) || !idm_buf_append(buf, core_symbol_text(core->as.record_field.field))) return false;
            if (!pretty_newline(buf, ci) || !idm_buf_appendf(buf, "%u", core->as.record_field.field_index)) return false;
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_RECORD_IS:
            if (!idm_buf_append(buf, "(record-is")) return false;
            if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.record_is.value, ci)) return false;
            if (!pretty_newline(buf, ci) || !idm_buf_append(buf, core_symbol_text(core->as.record_is.type))) return false;
            return idm_buf_append_char(buf, ')');
        default:
            return idm_buf_append_n(buf, "", 0);
    }
}

bool idm_core_dump_pretty(IdmBuffer *buf, const IdmCore *core) {
    return core_pretty(buf, core, 0);
}
