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

static bool emit_arity(IdmBytecodeModule *module, IdmArity arity) {
    return idm_bc_emit(module, (uint32_t)arity.kind, NULL) &&
           idm_bc_emit(module, arity.min, NULL) &&
           idm_bc_emit(module, arity.max, NULL) &&
           idm_bc_emit(module, (uint32_t)(arity.mask & UINT32_MAX), NULL) &&
           idm_bc_emit(module, (uint32_t)(arity.mask >> 32), NULL);
}

static bool append_arity(IdmBuffer *buf, const IdmArity *arity) {
    return idm_buf_append_char(buf, '/') && idm_arity_describe(buf, arity);
}

static const char *value_sequence_kind_name(IdmValueSequenceKind kind) {
    switch (kind) {
        case IDM_VALUE_SEQ_VECTOR: return "vector";
        case IDM_VALUE_SEQ_TUPLE: return "tuple";
        case IDM_VALUE_SEQ_DICT: return "dict";
    }
    return "unknown";
}

static const char *syntax_build_kind_name(IdmSyntaxBuildKind kind) {
    switch (kind) {
        case IDM_SYNTAX_BUILD_NIL: return "nil";
        case IDM_SYNTAX_BUILD_WORD: return "word";
        case IDM_SYNTAX_BUILD_ATOM: return "atom";
        case IDM_SYNTAX_BUILD_INT: return "int";
        case IDM_SYNTAX_BUILD_FLOAT: return "float";
        case IDM_SYNTAX_BUILD_STRING: return "string";
        case IDM_SYNTAX_BUILD_LIST: return "list";
        case IDM_SYNTAX_BUILD_VECTOR: return "vector";
        case IDM_SYNTAX_BUILD_TUPLE: return "tuple";
        case IDM_SYNTAX_BUILD_DICT: return "dict";
        case IDM_SYNTAX_BUILD_EXPR: return "expr";
        case IDM_SYNTAX_BUILD_BODY: return "body";
        case IDM_SYNTAX_BUILD_GROUP: return "group";
    }
    return "unknown";
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

IdmCore *idm_core_call(IdmCore *callee, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_CALL, span);
    if (!core) return NULL;
    core->as.call.callee = callee;
    return core;
}

bool idm_core_call_add_arg(IdmCore *call, IdmCore *arg) {
    if (!call || call->kind != IDM_CORE_CALL || !arg) return false;
    if (call->as.call.arg_count == call->as.call.arg_cap) {
        size_t cap = call->as.call.arg_cap ? call->as.call.arg_cap * 2u : 4u;
        IdmCore **args = realloc(call->as.call.args, cap * sizeof(*args));
        if (!args) return false;
        call->as.call.args = args;
        call->as.call.arg_cap = cap;
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
        size_t cap = core->as.value_sequence.cap ? core->as.value_sequence.cap * 2u : 4u;
        IdmCore **items = realloc(core->as.value_sequence.items, cap * sizeof(*items));
        if (!items) return false;
        core->as.value_sequence.items = items;
        core->as.value_sequence.cap = cap;
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
        size_t cap = core->as.string_concat.cap ? core->as.string_concat.cap * 2u : 4u;
        IdmCore **items = realloc(core->as.string_concat.items, cap * sizeof(*items));
        if (!items) return false;
        core->as.string_concat.items = items;
        core->as.string_concat.cap = cap;
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

IdmCore *idm_core_do(IdmSpan span) {
    return core_alloc(IDM_CORE_DO, span);
}

bool idm_core_do_add(IdmCore *do_expr, IdmCore *item) {
    if (!do_expr || do_expr->kind != IDM_CORE_DO || !item) return false;
    if (do_expr->as.do_expr.count == do_expr->as.do_expr.cap) {
        size_t cap = do_expr->as.do_expr.cap ? do_expr->as.do_expr.cap * 2u : 4u;
        IdmCore **items = realloc(do_expr->as.do_expr.items, cap * sizeof(*items));
        if (!items) return false;
        do_expr->as.do_expr.items = items;
        do_expr->as.do_expr.cap = cap;
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
        size_t cap = fn->as.fn.capture_cap ? fn->as.fn.capture_cap * 2u : 4u;
        IdmCapture *slots = realloc(fn->as.fn.captures, cap * sizeof(*slots));
        if (!slots) return false;
        fn->as.fn.captures = slots;
        fn->as.fn.capture_cap = cap;
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
        size_t cap = multi->as.fn_multi.cap ? multi->as.fn_multi.cap * 2u : 4u;
        IdmFnClause *clauses = realloc(multi->as.fn_multi.clauses, cap * sizeof(*clauses));
        if (!clauses) return NULL;
        multi->as.fn_multi.clauses = clauses;
        multi->as.fn_multi.cap = cap;
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
        size_t cap = multi->as.fn_multi.capture_cap ? multi->as.fn_multi.capture_cap * 2u : 4u;
        IdmCapture *slots = realloc(multi->as.fn_multi.captures, cap * sizeof(*slots));
        if (!slots) return false;
        multi->as.fn_multi.captures = slots;
        multi->as.fn_multi.capture_cap = cap;
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

bool idm_core_letrec_add(IdmCore *letrec, const char *name, uint32_t slot, IdmCore *value) {
    if (!letrec || letrec->kind != IDM_CORE_LETREC || !value) return false;
    if (letrec->as.letrec.count == letrec->as.letrec.cap) {
        size_t cap = letrec->as.letrec.cap ? letrec->as.letrec.cap * 2u : 4u;
        IdmLetRecBinding *bindings = realloc(letrec->as.letrec.bindings, cap * sizeof(*bindings));
        if (!bindings) return false;
        letrec->as.letrec.bindings = bindings;
        letrec->as.letrec.cap = cap;
    }
    IdmLetRecBinding *binding = &letrec->as.letrec.bindings[letrec->as.letrec.count];
    binding->name = idm_strdup(name);
    if (!binding->name) return false;
    binding->slot = slot;
    binding->value = value;
    letrec->as.letrec.count++;
    return true;
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

IdmCore *idm_core_use_package(IdmValue env_key, IdmBytecodeModule *module, uint32_t init_fn, IdmCore *cont, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_USE_PACKAGE, span);
    if (!core) return NULL;
    core->as.use_package.env_key = env_key;
    core->as.use_package.module = module;
    core->as.use_package.init_fn = init_fn;
    core->as.use_package.cont = cont;
    return core;
}

IdmCore *idm_core_define_trait(IdmValue name, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_DEFINE_TRAIT, span);
    if (!core) return NULL;
    core->as.define_trait.name = name;
    return core;
}

bool idm_core_define_trait_add_requirement(IdmCore *core, IdmValue requirement) {
    if (!core || core->kind != IDM_CORE_DEFINE_TRAIT) return false;
    if (core->as.define_trait.requirement_count == core->as.define_trait.requirement_cap) {
        size_t cap = core->as.define_trait.requirement_cap ? core->as.define_trait.requirement_cap * 2u : 4u;
        IdmCoreTraitRequirement *requirements = realloc(core->as.define_trait.requirements, cap * sizeof(*requirements));
        if (!requirements) return false;
        core->as.define_trait.requirements = requirements;
        core->as.define_trait.requirement_cap = cap;
    }
    core->as.define_trait.requirements[core->as.define_trait.requirement_count++].name = requirement;
    return true;
}

bool idm_core_define_trait_add_method(IdmCore *core, IdmValue method, IdmArity arity, IdmCore *default_fn) {
    if (!core || core->kind != IDM_CORE_DEFINE_TRAIT) return false;
    if (core->as.define_trait.count == core->as.define_trait.cap) {
        size_t cap = core->as.define_trait.cap ? core->as.define_trait.cap * 2u : 4u;
        IdmCoreTraitMethod *methods = realloc(core->as.define_trait.methods, cap * sizeof(*methods));
        if (!methods) return false;
        core->as.define_trait.methods = methods;
        core->as.define_trait.cap = cap;
    }
    IdmCoreTraitMethod *m = &core->as.define_trait.methods[core->as.define_trait.count++];
    m->name = method;
    m->arity = arity;
    m->has_default = default_fn != NULL;
    m->default_fn = default_fn;
    return true;
}

IdmCore *idm_core_implement_trait(IdmValue trait, IdmValue type, IdmValue provider, IdmValue provider_key, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_IMPLEMENT_TRAIT, span);
    if (!core) return NULL;
    core->as.implement_trait.trait = trait;
    core->as.implement_trait.type = type;
    core->as.implement_trait.provider = provider;
    core->as.implement_trait.provider_key = provider_key;
    return core;
}

bool idm_core_implement_trait_add_impl(IdmCore *core, IdmValue method, IdmArity arity, IdmCore *impl_fn) {
    if (!core || core->kind != IDM_CORE_IMPLEMENT_TRAIT || !impl_fn) return false;
    if (core->as.implement_trait.count == core->as.implement_trait.cap) {
        size_t cap = core->as.implement_trait.cap ? core->as.implement_trait.cap * 2u : 4u;
        IdmCoreTraitImpl *impls = realloc(core->as.implement_trait.impls, cap * sizeof(*impls));
        if (!impls) return false;
        core->as.implement_trait.impls = impls;
        core->as.implement_trait.cap = cap;
    }
    IdmCoreTraitImpl *impl = &core->as.implement_trait.impls[core->as.implement_trait.count++];
    impl->name = method;
    impl->arity = arity;
    impl->impl_fn = impl_fn;
    return true;
}

IdmCore *idm_core_method_call(IdmValue trait, IdmValue method, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_METHOD_CALL, span);
    if (!core) return NULL;
    core->as.method_call.trait = trait;
    core->as.method_call.method = method;
    return core;
}

bool idm_core_method_call_add_arg(IdmCore *core, IdmCore *arg) {
    if (!core || core->kind != IDM_CORE_METHOD_CALL || !arg) return false;
    if (core->as.method_call.arg_count == core->as.method_call.arg_cap) {
        size_t cap = core->as.method_call.arg_cap ? core->as.method_call.arg_cap * 2u : 4u;
        IdmCore **args = realloc(core->as.method_call.args, cap * sizeof(*args));
        if (!args) return false;
        core->as.method_call.args = args;
        core->as.method_call.arg_cap = cap;
    }
    core->as.method_call.args[core->as.method_call.arg_count++] = arg;
    return true;
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
            if (core->as.use_package.module) { idm_bc_destroy(core->as.use_package.module); free(core->as.use_package.module); }
            break;
        case IDM_CORE_DEFINE_TRAIT:
            for (size_t i = 0; i < core->as.define_trait.count; i++) idm_core_free(core->as.define_trait.methods[i].default_fn);
            free(core->as.define_trait.requirements);
            free(core->as.define_trait.methods);
            break;
        case IDM_CORE_IMPLEMENT_TRAIT:
            for (size_t i = 0; i < core->as.implement_trait.count; i++) idm_core_free(core->as.implement_trait.impls[i].impl_fn);
            free(core->as.implement_trait.impls);
            break;
        case IDM_CORE_METHOD_CALL:
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) idm_core_free(core->as.method_call.args[i]);
            free(core->as.method_call.args);
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
            free(core->as.slot_ref.name);
            break;
        case IDM_CORE_PACKAGE_REF:
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

static bool normalize_do(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = *slot;
    for (size_t i = 0; i < core->as.do_expr.count; i++) {
        if (!normalize_core(rt, &core->as.do_expr.items[i], err)) return false;
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
        case IDM_CORE_DEFINE_TRAIT:
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                if (!normalize_core(rt, &core->as.define_trait.methods[i].default_fn, err)) return false;
            }
            return true;
        case IDM_CORE_IMPLEMENT_TRAIT:
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                if (!normalize_core(rt, &core->as.implement_trait.impls[i].impl_fn, err)) return false;
            }
            return true;
        case IDM_CORE_METHOD_CALL:
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                if (!normalize_core(rt, &core->as.method_call.args[i], err)) return false;
            }
            return true;
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
static bool emit_op6(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, size_t *out_offset);
static uint32_t ctx_alloc(CompileCtx *ctx);
static uint32_t ctx_alloc_range(CompileCtx *ctx, uint32_t count);
static uint32_t ctx_mark(const CompileCtx *ctx);
static void ctx_reset(CompileCtx *ctx, uint32_t mark);
static uint32_t ctx_local_reg(const CompileCtx *ctx, uint32_t slot);

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
        case IDM_CORE_DEFINE_TRAIT: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.define_trait.methods[i].default_fn);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_IMPLEMENT_TRAIT: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.implement_trait.impls[i].impl_fn);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_METHOD_CALL: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.method_call.args[i]);
                if (child > max) max = child;
            }
            return max;
        }
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
        case IDM_CORE_DEFINE_TRAIT:
            for (size_t i = 0; i < core->as.define_trait.count; i++) collect_celled_slots(core->as.define_trait.methods[i].default_fn, celled, lc);
            return;
        case IDM_CORE_IMPLEMENT_TRAIT:
            for (size_t i = 0; i < core->as.implement_trait.count; i++) collect_celled_slots(core->as.implement_trait.impls[i].impl_fn, celled, lc);
            return;
        case IDM_CORE_METHOD_CALL:
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) collect_celled_slots(core->as.method_call.args[i], celled, lc);
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
        case IDM_CORE_DEFINE_TRAIT:
            for (size_t i = 0; i < core->as.define_trait.count; i++) mark_celled(core->as.define_trait.methods[i].default_fn, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_IMPLEMENT_TRAIT:
            for (size_t i = 0; i < core->as.implement_trait.count; i++) mark_celled(core->as.implement_trait.impls[i].impl_fn, celled, lc, caps, ncaps);
            return;
        case IDM_CORE_METHOD_CALL:
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) mark_celled(core->as.method_call.args[i], celled, lc, caps, ncaps);
            return;
        case IDM_CORE_ARG_REF:
        case IDM_CORE_ENV_REF:
        case IDM_CORE_PACKAGE_REF:
        case IDM_CORE_LITERAL:
            return;
    }
}

static bool compile_function_code(IdmBytecodeModule *module, uint32_t function_index, IdmCore *body, IdmError *err, IdmSpan span, const IdmCapture *captures, size_t capture_count) {
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
    [IDM_PRIM_ADD] = {"add", 2, 2},
    [IDM_PRIM_SUB] = {"sub", 2, 2},
    [IDM_PRIM_MUL] = {"mul", 2, 2},
    [IDM_PRIM_DIV] = {"div", 2, 2},
    [IDM_PRIM_MOD] = {"mod", 2, 2},
    [IDM_PRIM_POW] = {"pow", 2, 2},
    [IDM_PRIM_NEG] = {"neg", 1, 1},
    [IDM_PRIM_EQ] = {"eq?", 2, 2},
    [IDM_PRIM_NEQ] = {"neq?", 2, 2},
    [IDM_PRIM_LT] = {"lt?", 2, 2},
    [IDM_PRIM_GT] = {"gt?", 2, 2},
    [IDM_PRIM_LTE] = {"lte?", 2, 2},
    [IDM_PRIM_GTE] = {"gte?", 2, 2},
    [IDM_PRIM_COND] = {"cond", 2, 3},
    [IDM_PRIM_COMPARE] = {"compare", 2, 2},
    [IDM_PRIM_CEIL] = {"ceil", 1, 1},
    [IDM_PRIM_TRUNCATE] = {"truncate", 1, 1},
    [IDM_PRIM_SIN] = {"sin", 1, 1},
    [IDM_PRIM_COS] = {"cos", 1, 1},
    [IDM_PRIM_TAN] = {"tan", 1, 1},
    [IDM_PRIM_ASIN] = {"asin", 1, 1},
    [IDM_PRIM_ACOS] = {"acos", 1, 1},
    [IDM_PRIM_ATAN] = {"atan", 1, 1},
    [IDM_PRIM_ATAN2] = {"atan2", 2, 2},
    [IDM_PRIM_EXP] = {"exp", 1, 1},
    [IDM_PRIM_LOG] = {"log", 1, 1},
    [IDM_PRIM_LOG2] = {"log2", 1, 1},
    [IDM_PRIM_LOG10] = {"log10", 1, 1},
    [IDM_PRIM_HYPOT] = {"hypot", 2, 2},
    [IDM_PRIM_NAN_P] = {"nan?", 1, 1},
    [IDM_PRIM_FINITE_P] = {"finite?", 1, 1},
    [IDM_PRIM_INFINITE_P] = {"infinite?", 1, 1},
    [IDM_PRIM_NAN] = {"nan", 0, 0},
    [IDM_PRIM_INF] = {"inf", 0, 0},
    [IDM_PRIM_DIVMOD] = {"divmod", 2, 2},
    [IDM_PRIM_BIT_AND] = {"bit-and", 2, 2},
    [IDM_PRIM_BIT_OR] = {"bit-or", 2, 2},
    [IDM_PRIM_BIT_XOR] = {"bit-xor", 2, 2},
    [IDM_PRIM_BIT_NOT] = {"bit-not", 1, 1},
    [IDM_PRIM_SHIFT_LEFT] = {"shift-left", 2, 2},
    [IDM_PRIM_SHIFT_RIGHT] = {"shift-right", 2, 2},
    [IDM_PRIM_BIT_COUNT] = {"bit-count", 1, 1},
    [IDM_PRIM_BIT_LENGTH] = {"bit-length", 1, 1},
    [IDM_PRIM_TO_INT] = {"to-int", 1, 1},
    [IDM_PRIM_TO_FLOAT] = {"to-float", 1, 1},
    [IDM_PRIM_FILE_OPEN] = {"open", 2, 2},
    [IDM_PRIM_OK] = {"ok?", 1, 1},
    [IDM_PRIM_CONS] = {"cons", 2, 2},
    [IDM_PRIM_FIRST] = {"first", 1, 1},
    [IDM_PRIM_REST] = {"rest", 1, 1},
    [IDM_PRIM_LIST] = {"list", 0, UINT32_MAX},
    [IDM_PRIM_TUPLE] = {"tuple", 0, UINT32_MAX},
    [IDM_PRIM_VECTOR] = {"vector", 0, UINT32_MAX},
    [IDM_PRIM_DICT] = {"dict", 0, UINT32_MAX},
    [IDM_PRIM_TUPLE_GET] = {"tuple-get", 2, 2},
    [IDM_PRIM_STR_TO_LIST] = {"str-to-list", 1, 1},
    [IDM_PRIM_DICT_TO_LIST] = {"dict-to-list", 1, 1},
    [IDM_PRIM_VECTOR_TO_LIST] = {"vector-to-list", 1, 1},
    [IDM_PRIM_TUPLE_TO_LIST] = {"tuple-to-list", 1, 1},
    [IDM_PRIM_APPLY] = {"apply", 2, 2},
    [IDM_PRIM_APPEND] = {"append", 2, 2},
    [IDM_PRIM_SYNTAX_KIND] = {"syntax-kind", 1, 1},
    [IDM_PRIM_SYNTAX_PROPERTY] = {"syntax-property", 2, 2},
    [IDM_PRIM_SYNTAX_SET_PROPERTY] = {"syntax-set-property", 3, 3},
    [IDM_PRIM_SYNTAX_ORIGIN] = {"syntax-origin", 1, 1},
    [IDM_PRIM_SYNTAX_LIST_PRED] = {"syntax-list?", 1, 1},
    [IDM_PRIM_SYNTAX_LENGTH] = {"syntax-length", 1, 1},
    [IDM_PRIM_SYNTAX_NTH] = {"syntax-nth", 2, 2},
    [IDM_PRIM_SYNTAX_SLICE] = {"syntax-slice", 3, 3},
    [IDM_PRIM_SYNTAX_WORD_PRED] = {"syntax-word?", 1, 1},
    [IDM_PRIM_SYNTAX_WORD_TEXT] = {"syntax-word-text", 1, 1},
    [IDM_PRIM_SYNTAX_ATOM_PRED] = {"syntax-atom?", 1, 1},
    [IDM_PRIM_SYNTAX_ATOM_TEXT] = {"syntax-atom-text", 1, 1},
    [IDM_PRIM_SYNTAX_INT_PRED] = {"syntax-int?", 1, 1},
    [IDM_PRIM_SYNTAX_INT_VALUE] = {"syntax-int-value", 1, 1},
    [IDM_PRIM_SYNTAX_FLOAT_VALUE] = {"syntax-float-value", 1, 1},
    [IDM_PRIM_MAKE_SYNTAX_NIL] = {"make-syntax-nil", 1, 1},
    [IDM_PRIM_MAKE_SYNTAX_WORD] = {"make-syntax-word", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_ATOM] = {"make-syntax-atom", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_INT] = {"make-syntax-int", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_STRING] = {"make-syntax-string", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_LIST] = {"make-syntax-list", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_VECTOR] = {"make-syntax-vector", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_TUPLE] = {"make-syntax-tuple", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_DICT] = {"make-syntax-dict", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_EXPR] = {"make-syntax-expr", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_BODY] = {"make-syntax-body", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_GROUP] = {"make-syntax-group", 2, 2},
    [IDM_PRIM_SYNTAX_ERROR] = {"syntax-error", 2, 2},
    [IDM_PRIM_LOCAL_EXPAND] = {"local-expand", 1, 1},
    [IDM_PRIM_FREE_IDENTIFIER_EQ] = {"free-identifier=?", 2, 2},
    [IDM_PRIM_BOUND_IDENTIFIER_EQ] = {"bound-identifier=?", 2, 2},
    [IDM_PRIM_BIND_BANG] = {"bind!", 2, 2},
    [IDM_PRIM_SELF] = {"self", 0, 0},
    [IDM_PRIM_SPAWN] = {"spawn", 1, 1},
    [IDM_PRIM_SEND] = {"send", 2, 2},
    [IDM_PRIM_EXIT] = {"exit", 0, 2},
    [IDM_PRIM_LINK] = {"link", 1, 1},
    [IDM_PRIM_UNLINK] = {"unlink", 1, 1},
    [IDM_PRIM_MONITOR] = {"monitor", 1, 1},
    [IDM_PRIM_DEMONITOR] = {"demonitor", 1, 1},
    [IDM_PRIM_TRAP_EXIT] = {"trap-exit", 1, 1},
    [IDM_PRIM_STR] = {"str", 1, UINT32_MAX},
    [IDM_PRIM_CHOMP] = {"chomp", 1, 1},
    [IDM_PRIM_CAPTURE_STDOUT] = {"capture-stdout", 1, 1},
    [IDM_PRIM_EXEC] = {"exec", 1, 1},
    [IDM_PRIM_AWAIT] = {"await", 1, 1},
    [IDM_PRIM_PRINT] = {"print", 0, UINT32_MAX},
    [IDM_PRIM_PRINTLN] = {"println", 0, UINT32_MAX},
    [IDM_PRIM_CD] = {"cd", 1, 1},
    [IDM_PRIM_CHDIR] = {"chdir", 1, 1},
    [IDM_PRIM_PWD] = {"pwd", 0, 0},
    [IDM_PRIM_WRITE_PROCSUB_TEMP] = {"write-procsub-temp", 1, 1},
    [IDM_PRIM_MAKE_PROCSUB_TEMP] = {"make-procsub-temp", 0, 0},
    [IDM_PRIM_MAKE_RECORD] = {"make-record", 2, 2},
    [IDM_PRIM_RECORD_PRED] = {"record?", 1, 1},
    [IDM_PRIM_RECORD_TYPE] = {"record-type", 1, 1},
    [IDM_PRIM_ENV_GET] = {"env-get", 1, 1},
    [IDM_PRIM_ENV_SET] = {"env-set", 2, 2},
    [IDM_PRIM_SYNTAX_ADJACENT_PRED] = {"syntax-adjacent?", 1, 1},
    [IDM_PRIM_SYNTAX_STRING_TEXT] = {"syntax-string-text", 1, 1},
    [IDM_PRIM_STR_CONTAINS] = {"contains?", 2, 2},
    [IDM_PRIM_INTERNAL_REGISTER_MACRO] = {"internal-register-macro", 2, 2},
    [IDM_PRIM_EXPAND_CHECK] = {"expand-check", 1, 1},
    [IDM_PRIM_INSPECT] = {"inspect", 1, 1},
    [IDM_PRIM_STR_LEN] = {"len", 1, 1},
    [IDM_PRIM_STR_SLICE] = {"slice", 3, 3},
    [IDM_PRIM_STR_FIND] = {"find", 3, 3},
    [IDM_PRIM_STR_BYTE] = {"byte", 2, 2},
    [IDM_PRIM_BYTE_STR] = {"byte-str", 1, 1},
    [IDM_PRIM_REGEX_COMPILE] = {"raw-compile", 2, 2},
    [IDM_PRIM_REGEX_PRED] = {"raw-regex?", 1, 1},
    [IDM_PRIM_REGEX_SOURCE] = {"raw-source", 1, 1},
    [IDM_PRIM_REGEX_OPTIONS] = {"raw-options", 1, 1},
    [IDM_PRIM_REGEX_GROUP_COUNT] = {"raw-group-count", 1, 1},
    [IDM_PRIM_REGEX_GROUP_NAMES] = {"raw-group-names", 1, 1},
    [IDM_PRIM_REGEX_RESULT_PRED] = {"raw-result?", 1, 1},
    [IDM_PRIM_REGEX_SCAN_AT] = {"raw-scan-at", 3, 3},
    [IDM_PRIM_REGEX_SCAN_FROM] = {"raw-scan-from", 3, 3},
    [IDM_PRIM_REGEX_SCAN_FULL] = {"raw-scan-full", 2, 2},
    [IDM_PRIM_REGEX_TEST] = {"raw-test?", 2, 2},
    [IDM_PRIM_REGEX_RESULT_START] = {"raw-result-start", 1, 1},
    [IDM_PRIM_REGEX_RESULT_END] = {"raw-result-end", 1, 1},
    [IDM_PRIM_REGEX_RESULT_TEXT] = {"raw-result-text", 1, 1},
    [IDM_PRIM_REGEX_CAPTURE] = {"raw-capture", 2, 2},
    [IDM_PRIM_REGEX_CAPTURE_RANGE] = {"raw-capture-range", 2, 2},
    [IDM_PRIM_REGEX_CAPTURE_NAMED] = {"raw-capture-named", 2, 2},
    [IDM_PRIM_REGEX_CAPTURES] = {"raw-captures", 1, 1},
    [IDM_PRIM_REGEX_SCAN_ALL] = {"raw-scan-all", 2, 2},
    [IDM_PRIM_REGEX_REPLACE] = {"raw-replace", 3, 3},
    [IDM_PRIM_REGEX_REPLACE_ALL] = {"raw-replace-all", 3, 3},
    [IDM_PRIM_REGEX_SPLIT_ON] = {"raw-split-on", 2, 2},
    [IDM_PRIM_REGEX_ESCAPE] = {"raw-escape", 1, 1},
    [IDM_PRIM_FILE_READ] = {"read", 1, 1},
    [IDM_PRIM_FILE_WRITE] = {"write", 2, 2},
    [IDM_PRIM_FILE_EXISTS] = {"exists?", 1, 1},
    [IDM_PRIM_FILE_STAT] = {"stat", 1, 1},
    [IDM_PRIM_FILE_LIST] = {"list", 1, 1},
    [IDM_PRIM_FILE_REMOVE] = {"remove", 1, 1},
    [IDM_PRIM_ARGS] = {"args", 0, 0},
    [IDM_PRIM_TIME_MS] = {"time-ms", 0, 0},
    [IDM_PRIM_RANDOM] = {"random", 1, 1},
    [IDM_PRIM_DICT_GET] = {"dict-get", 3, 3},
    [IDM_PRIM_DICT_PUT] = {"dict-put", 3, 3},
    [IDM_PRIM_DICT_DEL] = {"dict-del", 2, 2},
    [IDM_PRIM_DICT_KEYS] = {"dict-keys", 1, 1},
    [IDM_PRIM_DICT_VALS] = {"dict-vals", 1, 1},
    [IDM_PRIM_DICT_HAS] = {"dict-has?", 2, 2},
    [IDM_PRIM_DICT_SIZE] = {"dict-size", 1, 1},
    [IDM_PRIM_RECORD_FIELD] = {"record-field", 2, 2},
    [IDM_PRIM_ABS] = {"abs", 1, 1},
    [IDM_PRIM_FLOOR] = {"floor", 1, 1},
    [IDM_PRIM_ROUND] = {"round", 1, 1},
    [IDM_PRIM_SQRT] = {"sqrt", 1, 1},
    [IDM_PRIM_FLOOR_DIV] = {"floor-div", 2, 2},
    [IDM_PRIM_FLOOR_MOD] = {"floor-mod", 2, 2},
    [IDM_PRIM_PARSE_INT] = {"parse-int", 1, 1},
    [IDM_PRIM_PARSE_FLOAT] = {"parse-float", 1, 1},
    [IDM_PRIM_FILE_MKDIR] = {"mkdir", 1, 1},
    [IDM_PRIM_FILE_APPEND] = {"append", 2, 2},
    [IDM_PRIM_ORD_STR] = {"ord->str", 1, 1},
    [IDM_PRIM_STR_ORD] = {"str->ord", 1, 1},
    [IDM_PRIM_FROM_RUNES] = {"from-runes", 1, 1},
    [IDM_PRIM_REPL_COMPILE] = {"repl-compile", 1, 1},
    [IDM_PRIM_REPL_ABORT] = {"repl-abort", 1, 1},
    [IDM_PRIM_REPL_SPAWN] = {"repl-spawn", 1, 1},
    [IDM_PRIM_REPL_DIAGNOSTIC] = {"repl-diagnostic", 0, 0},
    [IDM_PRIM_ISH_SESSION] = {"ish-session", 0, 0},
    [IDM_PRIM_TTY_PRED] = {"tty?", 0, 0},
    [IDM_PRIM_TTY_RAW] = {"tty-raw!", 0, 0},
    [IDM_PRIM_TTY_RESTORE] = {"tty-restore!", 0, 0},
    [IDM_PRIM_TTY_READ] = {"tty-read", 1, 1},
    [IDM_PRIM_TTY_READ_LINE] = {"tty-read-line", 0, 0},
    [IDM_PRIM_TTY_WRITE] = {"tty-write", 1, 1},
    [IDM_PRIM_TTY_SIZE] = {"tty-size", 0, 0},
    [IDM_PRIM_EPRINTLN] = {"eprintln", 0, UINT32_MAX},
    [IDM_PRIM_PORT_STATUS] = {"port-status", 1, 1},
    [IDM_PRIM_JOB_RESUME] = {"job-resume", 2, 2},
    [IDM_PRIM_JOB_SIGNAL] = {"job-signal", 2, 2},
    [IDM_PRIM_ERROR_MESSAGE] = {"error-message", 1, 1},
    [IDM_PRIM_MAKE_ERROR] = {"make-error", 1, 1},
    [IDM_PRIM_SPAWN_LINK] = {"spawn-link", 1, 1},
    [IDM_PRIM_SPAWN_MONITOR] = {"spawn-monitor", 1, 1},
    [IDM_PRIM_PORT_READ] = {"port-read", 3, 3},
    [IDM_PRIM_PORT_WRITE] = {"port-write", 2, 2},
    [IDM_PRIM_PORT_CLOSE_INPUT] = {"port-close-input", 1, 1},
    [IDM_PRIM_RAISE] = {"raise", 1, 1},
    [IDM_PRIM_IS_A_P] = {"is-a?", 2, 2},
    [IDM_PRIM_NIL_P] = {"nil?", 1, 1},
    [IDM_PRIM_ATOM_P] = {"atom?", 1, 1},
    [IDM_PRIM_WORD_P] = {"word?", 1, 1},
    [IDM_PRIM_INT_P] = {"int?", 1, 1},
    [IDM_PRIM_FLOAT_P] = {"float?", 1, 1},
    [IDM_PRIM_STRING_P] = {"string?", 1, 1},
    [IDM_PRIM_PAIR_P] = {"pair?", 1, 1},
    [IDM_PRIM_EMPTY_LIST_P] = {"empty-list?", 1, 1},
    [IDM_PRIM_LIST_P] = {"list?", 1, 1},
    [IDM_PRIM_TUPLE_P] = {"tuple?", 1, 1},
    [IDM_PRIM_VECTOR_P] = {"vector?", 1, 1},
    [IDM_PRIM_DICT_P] = {"dict?", 1, 1},
    [IDM_PRIM_SYNTAX_P] = {"syntax?", 1, 1},
    [IDM_PRIM_CELL_P] = {"cell?", 1, 1},
    [IDM_PRIM_CLOSURE_P] = {"closure?", 1, 1},
    [IDM_PRIM_PID_P] = {"pid?", 1, 1},
    [IDM_PRIM_REF_P] = {"ref?", 1, 1},
    [IDM_PRIM_PORT_P] = {"port?", 1, 1},
    [IDM_PRIM_REGEX_P] = {"regex?", 1, 1},
    [IDM_PRIM_REGEX_RESULT_P] = {"regex-result?", 1, 1},
};

static const char *const PRIMITIVE_HOMES[] = {
    [IDM_PRIM_ADD] = "kernel", [IDM_PRIM_SUB] = "kernel", [IDM_PRIM_MUL] = "kernel",
    [IDM_PRIM_DIV] = "kernel", [IDM_PRIM_MOD] = "kernel", [IDM_PRIM_POW] = "kernel",
    [IDM_PRIM_NEG] = "kernel", [IDM_PRIM_EQ] = "kernel", [IDM_PRIM_NEQ] = "kernel",
    [IDM_PRIM_LT] = "kernel", [IDM_PRIM_GT] = "kernel", [IDM_PRIM_LTE] = "kernel",
    [IDM_PRIM_GTE] = "kernel", [IDM_PRIM_COND] = "kernel", [IDM_PRIM_COMPARE] = "kernel", [IDM_PRIM_APPLY] = "kernel",
    [IDM_PRIM_CEIL] = "math", [IDM_PRIM_TRUNCATE] = "math", [IDM_PRIM_SIN] = "math",
    [IDM_PRIM_COS] = "math", [IDM_PRIM_TAN] = "math", [IDM_PRIM_ASIN] = "math",
    [IDM_PRIM_ACOS] = "math", [IDM_PRIM_ATAN] = "math", [IDM_PRIM_ATAN2] = "math",
    [IDM_PRIM_EXP] = "math", [IDM_PRIM_LOG] = "math", [IDM_PRIM_LOG2] = "math",
    [IDM_PRIM_LOG10] = "math", [IDM_PRIM_HYPOT] = "math", [IDM_PRIM_NAN_P] = "math",
    [IDM_PRIM_FINITE_P] = "math", [IDM_PRIM_INFINITE_P] = "math", [IDM_PRIM_NAN] = "math",
    [IDM_PRIM_INF] = "math", [IDM_PRIM_DIVMOD] = "math", [IDM_PRIM_BIT_AND] = "math",
    [IDM_PRIM_BIT_OR] = "math", [IDM_PRIM_BIT_XOR] = "math", [IDM_PRIM_BIT_NOT] = "math",
    [IDM_PRIM_SHIFT_LEFT] = "math", [IDM_PRIM_SHIFT_RIGHT] = "math", [IDM_PRIM_BIT_COUNT] = "math",
    [IDM_PRIM_BIT_LENGTH] = "math", [IDM_PRIM_TO_INT] = "math", [IDM_PRIM_TO_FLOAT] = "math",
    [IDM_PRIM_ABS] = "math", [IDM_PRIM_FLOOR] = "math", [IDM_PRIM_ROUND] = "math",
    [IDM_PRIM_SQRT] = "math", [IDM_PRIM_FLOOR_DIV] = "kernel", [IDM_PRIM_FLOOR_MOD] = "kernel",
    [IDM_PRIM_CONS] = "kernel", [IDM_PRIM_FIRST] = "kernel", [IDM_PRIM_REST] = "kernel",
    [IDM_PRIM_LIST] = "kernel", [IDM_PRIM_APPEND] = "kernel",
    [IDM_PRIM_TUPLE] = "kernel", [IDM_PRIM_TUPLE_GET] = "kernel", [IDM_PRIM_TUPLE_TO_LIST] = "kernel",
    [IDM_PRIM_VECTOR] = "kernel", [IDM_PRIM_VECTOR_TO_LIST] = "kernel",
    [IDM_PRIM_DICT] = "kernel", [IDM_PRIM_DICT_TO_LIST] = "kernel", [IDM_PRIM_DICT_GET] = "kernel",
    [IDM_PRIM_DICT_PUT] = "kernel", [IDM_PRIM_DICT_DEL] = "kernel", [IDM_PRIM_DICT_KEYS] = "kernel",
    [IDM_PRIM_DICT_VALS] = "kernel", [IDM_PRIM_DICT_HAS] = "kernel", [IDM_PRIM_DICT_SIZE] = "kernel",
    [IDM_PRIM_STR_TO_LIST] = "kernel", [IDM_PRIM_STR_CONTAINS] = "string", [IDM_PRIM_STR_LEN] = "string",
    [IDM_PRIM_STR_SLICE] = "string", [IDM_PRIM_STR_FIND] = "string", [IDM_PRIM_STR_BYTE] = "string",
    [IDM_PRIM_BYTE_STR] = "string", [IDM_PRIM_CHOMP] = "string", [IDM_PRIM_PARSE_INT] = "string",
    [IDM_PRIM_PARSE_FLOAT] = "string", [IDM_PRIM_ORD_STR] = "string", [IDM_PRIM_STR_ORD] = "string",
    [IDM_PRIM_FROM_RUNES] = "string",
    [IDM_PRIM_REGEX_COMPILE] = "regex", [IDM_PRIM_REGEX_PRED] = "regex", [IDM_PRIM_REGEX_SOURCE] = "regex",
    [IDM_PRIM_REGEX_OPTIONS] = "regex", [IDM_PRIM_REGEX_GROUP_COUNT] = "regex", [IDM_PRIM_REGEX_GROUP_NAMES] = "regex",
    [IDM_PRIM_REGEX_RESULT_PRED] = "regex", [IDM_PRIM_REGEX_SCAN_AT] = "regex", [IDM_PRIM_REGEX_SCAN_FROM] = "regex",
    [IDM_PRIM_REGEX_SCAN_FULL] = "regex", [IDM_PRIM_REGEX_TEST] = "regex", [IDM_PRIM_REGEX_RESULT_START] = "regex",
    [IDM_PRIM_REGEX_RESULT_END] = "regex", [IDM_PRIM_REGEX_RESULT_TEXT] = "regex", [IDM_PRIM_REGEX_CAPTURE] = "regex",
    [IDM_PRIM_REGEX_CAPTURE_RANGE] = "regex", [IDM_PRIM_REGEX_CAPTURE_NAMED] = "regex", [IDM_PRIM_REGEX_CAPTURES] = "regex",
    [IDM_PRIM_REGEX_SCAN_ALL] = "regex", [IDM_PRIM_REGEX_REPLACE] = "regex", [IDM_PRIM_REGEX_REPLACE_ALL] = "regex",
    [IDM_PRIM_REGEX_SPLIT_ON] = "regex", [IDM_PRIM_REGEX_ESCAPE] = "regex",
    [IDM_PRIM_FILE_OPEN] = "file", [IDM_PRIM_FILE_READ] = "file", [IDM_PRIM_FILE_WRITE] = "file",
    [IDM_PRIM_FILE_EXISTS] = "file", [IDM_PRIM_FILE_STAT] = "file", [IDM_PRIM_FILE_LIST] = "file",
    [IDM_PRIM_FILE_REMOVE] = "file", [IDM_PRIM_FILE_MKDIR] = "file", [IDM_PRIM_FILE_APPEND] = "file",
    [IDM_PRIM_OK] = "result",
    [IDM_PRIM_SYNTAX_KIND] = "kernel",
    [IDM_PRIM_SYNTAX_PROPERTY] = "kernel", [IDM_PRIM_SYNTAX_SET_PROPERTY] = "kernel", [IDM_PRIM_SYNTAX_ORIGIN] = "kernel",
    [IDM_PRIM_SYNTAX_LIST_PRED] = "kernel", [IDM_PRIM_SYNTAX_LENGTH] = "kernel", [IDM_PRIM_SYNTAX_NTH] = "kernel",
    [IDM_PRIM_SYNTAX_SLICE] = "kernel", [IDM_PRIM_SYNTAX_WORD_PRED] = "kernel", [IDM_PRIM_SYNTAX_WORD_TEXT] = "kernel",
    [IDM_PRIM_SYNTAX_ATOM_PRED] = "kernel", [IDM_PRIM_SYNTAX_ATOM_TEXT] = "kernel", [IDM_PRIM_SYNTAX_INT_PRED] = "kernel",
    [IDM_PRIM_SYNTAX_INT_VALUE] = "kernel", [IDM_PRIM_MAKE_SYNTAX_WORD] = "kernel", [IDM_PRIM_MAKE_SYNTAX_ATOM] = "kernel",
    [IDM_PRIM_MAKE_SYNTAX_INT] = "kernel", [IDM_PRIM_MAKE_SYNTAX_STRING] = "kernel", [IDM_PRIM_MAKE_SYNTAX_LIST] = "kernel",
    [IDM_PRIM_MAKE_SYNTAX_VECTOR] = "kernel", [IDM_PRIM_MAKE_SYNTAX_TUPLE] = "kernel", [IDM_PRIM_MAKE_SYNTAX_DICT] = "kernel",
    [IDM_PRIM_MAKE_SYNTAX_EXPR] = "kernel", [IDM_PRIM_MAKE_SYNTAX_BODY] = "kernel", [IDM_PRIM_MAKE_SYNTAX_GROUP] = "kernel",
    [IDM_PRIM_SYNTAX_ERROR] = "kernel", [IDM_PRIM_LOCAL_EXPAND] = "kernel", [IDM_PRIM_FREE_IDENTIFIER_EQ] = "kernel",
    [IDM_PRIM_BOUND_IDENTIFIER_EQ] = "kernel", [IDM_PRIM_BIND_BANG] = "kernel", [IDM_PRIM_SYNTAX_ADJACENT_PRED] = "kernel",
    [IDM_PRIM_SYNTAX_STRING_TEXT] = "kernel", [IDM_PRIM_SYNTAX_FLOAT_VALUE] = "kernel",
    [IDM_PRIM_MAKE_SYNTAX_NIL] = "kernel",
    [IDM_PRIM_INTERNAL_REGISTER_MACRO] = "", [IDM_PRIM_EXPAND_CHECK] = "compile",
    [IDM_PRIM_SELF] = "kernel", [IDM_PRIM_SPAWN] = "kernel", [IDM_PRIM_SEND] = "kernel",
    [IDM_PRIM_EXIT] = "kernel", [IDM_PRIM_LINK] = "kernel", [IDM_PRIM_UNLINK] = "kernel",
    [IDM_PRIM_MONITOR] = "kernel", [IDM_PRIM_DEMONITOR] = "kernel", [IDM_PRIM_TRAP_EXIT] = "kernel",
    [IDM_PRIM_SPAWN_LINK] = "kernel", [IDM_PRIM_SPAWN_MONITOR] = "kernel", [IDM_PRIM_RAISE] = "kernel",
    [IDM_PRIM_STR] = "kernel", [IDM_PRIM_CAPTURE_STDOUT] = "kernel", [IDM_PRIM_EXEC] = "kernel",
    [IDM_PRIM_AWAIT] = "kernel", [IDM_PRIM_PRINT] = "kernel", [IDM_PRIM_PRINTLN] = "kernel",
    [IDM_PRIM_EPRINTLN] = "kernel", [IDM_PRIM_CD] = "system", [IDM_PRIM_CHDIR] = "system",
    [IDM_PRIM_PWD] = "system", [IDM_PRIM_WRITE_PROCSUB_TEMP] = "kernel", [IDM_PRIM_MAKE_PROCSUB_TEMP] = "kernel",
    [IDM_PRIM_MAKE_RECORD] = "kernel", [IDM_PRIM_RECORD_PRED] = "kernel", [IDM_PRIM_RECORD_TYPE] = "kernel",
    [IDM_PRIM_RECORD_FIELD] = "kernel", [IDM_PRIM_MAKE_ERROR] = "kernel", [IDM_PRIM_ERROR_MESSAGE] = "kernel",
    [IDM_PRIM_ENV_GET] = "system", [IDM_PRIM_ENV_SET] = "system", [IDM_PRIM_INSPECT] = "kernel",
    [IDM_PRIM_ARGS] = "system", [IDM_PRIM_TIME_MS] = "system", [IDM_PRIM_RANDOM] = "system",
    [IDM_PRIM_REPL_COMPILE] = "kernel", [IDM_PRIM_REPL_ABORT] = "kernel", [IDM_PRIM_REPL_SPAWN] = "kernel",
    [IDM_PRIM_REPL_DIAGNOSTIC] = "kernel", [IDM_PRIM_ISH_SESSION] = "kernel", [IDM_PRIM_TTY_PRED] = "term",
    [IDM_PRIM_TTY_RAW] = "term", [IDM_PRIM_TTY_RESTORE] = "term", [IDM_PRIM_TTY_READ] = "term",
    [IDM_PRIM_TTY_READ_LINE] = "term", [IDM_PRIM_TTY_WRITE] = "term", [IDM_PRIM_TTY_SIZE] = "term",
    [IDM_PRIM_PORT_STATUS] = "port", [IDM_PRIM_JOB_RESUME] = "kernel", [IDM_PRIM_JOB_SIGNAL] = "kernel",
    [IDM_PRIM_PORT_READ] = "port", [IDM_PRIM_PORT_WRITE] = "port", [IDM_PRIM_PORT_CLOSE_INPUT] = "port",
    [IDM_PRIM_IS_A_P] = "kernel", [IDM_PRIM_NIL_P] = "kernel", [IDM_PRIM_ATOM_P] = "kernel",
    [IDM_PRIM_WORD_P] = "kernel", [IDM_PRIM_INT_P] = "kernel", [IDM_PRIM_FLOAT_P] = "kernel",
    [IDM_PRIM_STRING_P] = "kernel", [IDM_PRIM_PAIR_P] = "kernel", [IDM_PRIM_EMPTY_LIST_P] = "kernel",
    [IDM_PRIM_LIST_P] = "kernel", [IDM_PRIM_TUPLE_P] = "kernel", [IDM_PRIM_VECTOR_P] = "kernel",
    [IDM_PRIM_DICT_P] = "kernel", [IDM_PRIM_SYNTAX_P] = "kernel", [IDM_PRIM_CELL_P] = "kernel",
    [IDM_PRIM_CLOSURE_P] = "kernel", [IDM_PRIM_PID_P] = "kernel", [IDM_PRIM_REF_P] = "kernel",
    [IDM_PRIM_PORT_P] = "kernel", [IDM_PRIM_REGEX_P] = "kernel",
    [IDM_PRIM_REGEX_RESULT_P] = "kernel",
};

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

const char *idm_primitive_name(IdmPrimitive primitive) {
    const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
    if (info) return info->name;
    return "<bad-primitive>";
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

static bool emit_op6(IdmBytecodeModule *module, IdmOpcode op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, size_t *out_offset) {
    size_t op_offset = 0;
    if (!emit_op5(module, op, a, b, c, d, e, &op_offset)) return false;
    if (!idm_bc_emit(module, f, NULL)) return false;
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

static bool patch_here(IdmBytecodeModule *module, size_t operand_index, IdmError *err, IdmSpan span, const char *what) {
    if (module->code_count > UINT32_MAX) return idm_error_set(err, span, "bytecode too large");
    if (!idm_bc_patch_u32(module, operand_index, (uint32_t)module->code_count)) return idm_error_set(err, span, "failed to patch %s", what);
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

static bool compile_callable_bodies(IdmCore *fn, const CompiledCallable *callable, IdmBytecodeModule *module, IdmError *err, IdmSpan span) {
    if (fn->kind == IDM_CORE_FN) {
        if (fn->as.fn.guard && !compile_function_code(module, callable->guards[0], fn->as.fn.guard, err, span, fn->as.fn.captures, fn->as.fn.capture_count)) return false;
        return compile_function_code(module, callable->entries[0], fn->as.fn.body, err, span, fn->as.fn.captures, fn->as.fn.capture_count);
    }
    for (size_t i = 0; i < fn->as.fn_multi.count; i++) {
        IdmFnClause *clause = &fn->as.fn_multi.clauses[i];
        if (clause->primitive_backed) continue;
        if (clause->guard && !compile_function_code(module, callable->guards[i], clause->guard, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
        if (!compile_function_code(module, callable->entries[i], clause->body, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
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
    if (ok) ok = compile_callable_bodies(fn, &callable, ctx->module, err, fn->span);
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
            if (ok) ok = emit_named_op2(ctx->module, IDM_OP_STORE_ENV, binding->slot, value_reg, binding->name, err, core->span);
        }
    } else if (ok) {
        if (!core->as.letrec.fill_only) {
            uint32_t nil_reg = ctx_alloc(ctx);
            for (size_t i = 0; ok && i < count; i++) {
                uint32_t local = ctx_local_reg(ctx, core->as.letrec.bindings[i].slot);
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
        if (compiled[i]) ok = compile_callable_bodies(core->as.letrec.bindings[i].value, &callables[i], ctx->module, err, core->as.letrec.bindings[i].value->span);
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
                size_t skip_op = 0;
                if (!emit_op1(ctx->module, IDM_OP_JUMP, 0, &skip_op)) return idm_error_oom(err, core->span);
                uint32_t const_off = 0;
                uint32_t code_off = 0;
                if (!idm_bc_link(ctx->module, core->as.use_package.module, &const_off, &fn_off, &code_off, err)) return false;
                if (ctx->module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
                if (!idm_bc_patch_u32(ctx->module, skip_op + 1u, (uint32_t)ctx->module->code_count)) return idm_error_set(err, core->span, "failed to patch package skip jump");
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
        case IDM_CORE_DEFINE_TRAIT: {
            if (core->as.define_trait.requirement_count > UINT32_MAX) return idm_error_set(err, core->span, "trait has too many requirements");
            if (core->as.define_trait.count > UINT32_MAX) return idm_error_set(err, core->span, "trait has too many methods");
            uint32_t first_default = ctx_alloc_range(ctx, (uint32_t)core->as.define_trait.count);
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                if (method->has_default) {
                    if (!compile_expr(method->default_fn, ctx, first_default + (uint32_t)i, false, err)) return false;
                } else {
                    if (!emit_load_value(ctx, first_default + (uint32_t)i, idm_nil(), err, core->span)) return false;
                }
            }
            uint32_t trait_const = 0;
            if (!add_const_value(ctx->module, core->as.define_trait.name, &trait_const, err, core->span)) return false;
            if (!emit_op5(ctx->module, IDM_OP_DEFINE_TRAIT, dst, first_default, trait_const, (uint32_t)core->as.define_trait.requirement_count, (uint32_t)core->as.define_trait.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.define_trait.requirement_count; i++) {
                uint32_t requirement_const = 0;
                if (!add_const_value(ctx->module, core->as.define_trait.requirements[i].name, &requirement_const, err, core->span)) return false;
                if (!idm_bc_emit(ctx->module, requirement_const, NULL)) return idm_error_oom(err, core->span);
            }
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                uint32_t method_const = 0;
                if (!add_const_value(ctx->module, method->name, &method_const, err, core->span)) return false;
                if (!idm_bc_emit(ctx->module, method_const, NULL)) return idm_error_oom(err, core->span);
                if (!emit_arity(ctx->module, method->arity)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(ctx->module, method->has_default ? 1u : 0u, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_IMPLEMENT_TRAIT: {
            if (core->as.implement_trait.count > UINT32_MAX) return idm_error_set(err, core->span, "implement has too many methods");
            uint32_t first_impl = ctx_alloc_range(ctx, (uint32_t)core->as.implement_trait.count);
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                if (!compile_expr(core->as.implement_trait.impls[i].impl_fn, ctx, first_impl + (uint32_t)i, false, err)) return false;
            }
            uint32_t trait_const = 0;
            uint32_t type_const = 0;
            uint32_t provider_const = 0;
            uint32_t provider_key_const = 0;
            if (!add_const_value(ctx->module, core->as.implement_trait.trait, &trait_const, err, core->span)) return false;
            if (!add_const_value(ctx->module, core->as.implement_trait.type, &type_const, err, core->span)) return false;
            if (!add_const_value(ctx->module, core->as.implement_trait.provider, &provider_const, err, core->span)) return false;
            if (!add_const_value(ctx->module, core->as.implement_trait.provider_key, &provider_key_const, err, core->span)) return false;
            if (!emit_op6(ctx->module, IDM_OP_IMPLEMENT_TRAIT, dst, first_impl, trait_const, type_const, provider_const, provider_key_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(ctx->module, (uint32_t)core->as.implement_trait.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                IdmCoreTraitImpl *impl = &core->as.implement_trait.impls[i];
                uint32_t method_const = 0;
                if (!add_const_value(ctx->module, impl->name, &method_const, err, core->span)) return false;
                if (!idm_bc_emit(ctx->module, method_const, NULL)) return idm_error_oom(err, core->span);
                if (!emit_arity(ctx->module, impl->arity)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_METHOD_CALL: {
            if (core->as.method_call.arg_count == 0) return idm_error_set(err, core->span, "method call requires a receiver");
            if (core->as.method_call.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "method call has too many arguments");
            uint32_t argc = (uint32_t)core->as.method_call.arg_count;
            uint32_t first_arg = ctx_alloc_range(ctx, argc);
            for (uint32_t i = 0; i < argc; i++) {
                if (!compile_expr(core->as.method_call.args[i], ctx, first_arg + i, false, err)) return false;
            }
            uint32_t trait_const = 0;
            uint32_t method_const = 0;
            if (!add_const_value(ctx->module, core->as.method_call.trait, &trait_const, err, core->span)) return false;
            if (!add_const_value(ctx->module, core->as.method_call.method, &method_const, err, core->span)) return false;
            if (!emit_op6(ctx->module, IDM_OP_CALL_METHOD, dst, trait_const, method_const, first_arg, argc, tail ? 1u : 0u, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
    }
    return idm_error_set(err, core->span, "unknown core node");
}

bool idm_core_compile_expression(IdmCore *core, IdmBytecodeModule *module, IdmError *err) {
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "<expression>", 0, locals, 0, &fn)) return idm_error_oom(err, core ? core->span : idm_span_unknown(NULL));
    return compile_function_code(module, fn, core, err, core ? core->span : idm_span_unknown(NULL), NULL, 0);
}

bool idm_core_compile_function_body(IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!body) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null function body");
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(body);
    if (!idm_bc_add_function(module, name ? name : "<function>", arity, locals, 0, &fn)) return idm_error_oom(err, body->span);
    if (!compile_function_code(module, fn, body, err, body->span, NULL, 0)) return false;
    if (out_function) *out_function = fn;
    return true;
}

static bool compile_function_entry(IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!fn || !core_is_callable(fn)) return idm_error_set(err, fn ? fn->span : idm_span_unknown(NULL), "expected a function");
    CompiledCallable callable;
    if (!compile_callable_entries(fn, module, &callable, err, fn->span)) return false;
    bool ok = compile_callable_bodies(fn, &callable, module, err, fn->span);
    if (ok && out_function) *out_function = callable.entries[0];
    compiled_callable_destroy(&callable);
    return ok;
}

bool idm_core_compile_function(IdmCore *fn, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!fn || !core_is_callable(fn)) return idm_error_set(err, fn ? fn->span : idm_span_unknown(NULL), "expected a function");
    if (core_is_primitive_only_callable(fn)) return compile_function_entry(fn, module, out_function, err);
    size_t capture_count = fn->kind == IDM_CORE_FN ? fn->as.fn.capture_count : fn->as.fn_multi.capture_count;
    if (capture_count != 0) return idm_error_set(err, fn->span, "captured function cannot be compiled as a bare module entry");
    if (fn->kind == IDM_CORE_FN) return compile_function_entry(fn, module, out_function, err);

    IdmCore *arg = idm_core_arg_ref("__syntax__", 0, fn->span);
    IdmCore *call = idm_core_call(fn, fn->span);
    const char *name = fn->as.fn_multi.name ? fn->as.fn_multi.name : "<transformer>";
    IdmCore *wrapper = call ? idm_core_fn(name, 1, call, fn->span) : NULL;
    bool arg_attached = false;
    bool ok = arg && call && wrapper;
    if (ok) {
        ok = idm_core_call_add_arg(call, arg);
        arg_attached = ok;
    }
    if (ok) ok = compile_function_entry(wrapper, module, out_function, err);
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
    return ok;
}

bool idm_core_compile_main(IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "main", 0, locals, 0, &fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!compile_function_code(module, fn, core, err, idm_span_unknown(NULL), NULL, 0)) return false;
    if (out_function) *out_function = fn;
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
            if (!idm_buf_appendf(buf, "(make-%s", value_sequence_kind_name(core->as.value_sequence.kind))) return false;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.value_sequence.items[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_SYNTAX_BUILD:
            if (!idm_buf_appendf(buf, "(make-syntax:%s ", syntax_build_kind_name(core->as.syntax_build.kind))) return false;
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
                if (!idm_buf_appendf(buf, "(%s#%u ", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
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
        case IDM_CORE_DEFINE_TRAIT:
            if (!idm_buf_append(buf, "(define-trait ") || !dump_value(buf, core->as.define_trait.name)) return false;
            for (size_t i = 0; i < core->as.define_trait.requirement_count; i++) {
                if (!idm_buf_append(buf, " (require ") || !dump_value(buf, core->as.define_trait.requirements[i].name) || !idm_buf_append_char(buf, ')')) return false;
            }
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                const IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                if (!idm_buf_append(buf, " (method ") || !dump_value(buf, method->name) || !append_arity(buf, &method->arity)) return false;
                if (method->has_default) {
                    if (!idm_buf_append(buf, " default=") || !idm_core_dump(buf, method->default_fn)) return false;
                }
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_IMPLEMENT_TRAIT:
            if (!idm_buf_append(buf, "(implement-trait ") || !dump_value(buf, core->as.implement_trait.trait) ||
                !idm_buf_append(buf, " type=") || !dump_value(buf, core->as.implement_trait.type)) return false;
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                const IdmCoreTraitImpl *impl = &core->as.implement_trait.impls[i];
                if (!idm_buf_append(buf, " (impl ") || !dump_value(buf, impl->name) || !append_arity(buf, &impl->arity) || !idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, impl->impl_fn) || !idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_METHOD_CALL:
            if (!idm_buf_append(buf, "(method-call ") || !dump_value(buf, core->as.method_call.trait) || !idm_buf_append_char(buf, ' ') || !dump_value(buf, core->as.method_call.method)) return false;
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.method_call.args[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
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
        if (!idm_buf_appendf(buf, "(%s#%u ", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
        if (!idm_core_dump(buf, core->as.letrec.bindings[i].value)) return false;
        if (!idm_buf_append_char(buf, ')')) return false;
    }
    return idm_buf_append_char(buf, ')');
}

static bool trait_method_compact(IdmBuffer *buf, const IdmCoreTraitMethod *method) {
    if (!idm_buf_append(buf, "(method ") || !dump_value(buf, method->name) || !append_arity(buf, &method->arity)) return false;
    if (method->has_default) {
        if (!idm_buf_append(buf, " default=") || !idm_core_dump(buf, method->default_fn)) return false;
    }
    return idm_buf_append_char(buf, ')');
}

static bool trait_impl_compact(IdmBuffer *buf, const IdmCoreTraitImpl *impl) {
    if (!idm_buf_append(buf, "(impl ") || !dump_value(buf, impl->name) || !append_arity(buf, &impl->arity) || !idm_buf_append_char(buf, ' ')) return false;
    if (!idm_core_dump(buf, impl->impl_fn)) return false;
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
            if (!idm_buf_appendf(buf, "(make-%s", value_sequence_kind_name(core->as.value_sequence.kind))) return false;
            for (size_t i = 0; i < core->as.value_sequence.count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.value_sequence.items[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_SYNTAX_BUILD:
            if (!idm_buf_appendf(buf, "(make-syntax:%s", syntax_build_kind_name(core->as.syntax_build.kind))) return false;
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
                        if (!idm_buf_appendf(&pfx, "(%s#%u ", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) { idm_buf_destroy(&pfx); return false; }
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
        case IDM_CORE_DEFINE_TRAIT:
            if (!idm_buf_append(buf, "(define-trait ") || !dump_value(buf, core->as.define_trait.name)) return false;
            for (size_t i = 0; i < core->as.define_trait.requirement_count; i++) {
                if (!pretty_newline(buf, ci)) return false;
                if (!idm_buf_append(buf, "(require ") || !dump_value(buf, core->as.define_trait.requirements[i].name) || !idm_buf_append_char(buf, ')')) return false;
            }
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                const IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                if (!pretty_newline(buf, ci)) return false;
                IdmBuffer mc;
                idm_buf_init(&mc);
                if (!trait_method_compact(&mc, method)) { idm_buf_destroy(&mc); return false; }
                if (ci + mc.len <= IDM_CORE_PRETTY_WIDTH) {
                    bool ok = idm_buf_append_n(buf, mc.data ? mc.data : "", mc.len);
                    idm_buf_destroy(&mc);
                    if (!ok) return false;
                    continue;
                }
                idm_buf_destroy(&mc);
                if (!idm_buf_append(buf, "(method ") || !dump_value(buf, method->name) || !append_arity(buf, &method->arity)) return false;
                if (method->has_default) {
                    if (!pretty_newline(buf, ci + 2) || !pretty_marker_child(buf, ci + 2, "default=", method->default_fn)) return false;
                }
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_IMPLEMENT_TRAIT:
            if (!idm_buf_append(buf, "(implement-trait ") || !dump_value(buf, core->as.implement_trait.trait) ||
                !idm_buf_append(buf, " type=") || !dump_value(buf, core->as.implement_trait.type)) return false;
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                const IdmCoreTraitImpl *impl = &core->as.implement_trait.impls[i];
                if (!pretty_newline(buf, ci)) return false;
                IdmBuffer ic;
                idm_buf_init(&ic);
                if (!trait_impl_compact(&ic, impl)) { idm_buf_destroy(&ic); return false; }
                if (ci + ic.len <= IDM_CORE_PRETTY_WIDTH) {
                    bool ok = idm_buf_append_n(buf, ic.data ? ic.data : "", ic.len);
                    idm_buf_destroy(&ic);
                    if (!ok) return false;
                    continue;
                }
                idm_buf_destroy(&ic);
                if (!idm_buf_append(buf, "(impl ") || !dump_value(buf, impl->name) || !append_arity(buf, &impl->arity)) return false;
                if (!pretty_newline(buf, ci + 2) || !core_pretty(buf, impl->impl_fn, ci + 2)) return false;
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_METHOD_CALL:
            if (!idm_buf_append(buf, "(method-call ") || !dump_value(buf, core->as.method_call.trait) ||
                !idm_buf_append_char(buf, ' ') || !dump_value(buf, core->as.method_call.method)) return false;
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                if (!pretty_newline(buf, ci) || !core_pretty(buf, core->as.method_call.args[i], ci)) return false;
            }
            return idm_buf_append_char(buf, ')');
        default:
            return idm_buf_append_n(buf, "", 0);
    }
}

bool idm_core_dump_pretty(IdmBuffer *buf, const IdmCore *core) {
    return core_pretty(buf, core, 0);
}
