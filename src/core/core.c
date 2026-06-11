#include "idiom/core.h"

#include <stdlib.h>

static IdmCore *core_alloc(IdmCoreKind kind, IdmSpan span) {
    IdmCore *core = calloc(1u, sizeof(*core));
    if (!core) return NULL;
    core->kind = kind;
    core->span = span;
    return core;
}

IdmCore *idm_core_literal(IdmValue value, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LITERAL, span);
    if (!core) return NULL;
    core->as.literal = value;
    return core;
}

IdmCore *idm_core_arg_ref(uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_ARG_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IdmCore *idm_core_local_ref(uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_LOCAL_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IdmCore *idm_core_capture_ref(uint32_t slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_CAPTURE_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IdmCore *idm_core_primitive(IdmPrimitive primitive, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_PRIMITIVE, span);
    if (!core) return NULL;
    core->as.primitive = primitive;
    return core;
}

IdmCore *idm_core_app(IdmCore *callee, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_APP, span);
    if (!core) return NULL;
    core->as.app.callee = callee;
    return core;
}

bool idm_core_app_add_arg(IdmCore *app, IdmCore *arg) {
    if (!app || app->kind != IDM_CORE_APP || !arg) return false;
    if (app->as.app.arg_count == app->as.app.arg_cap) {
        size_t cap = app->as.app.arg_cap ? app->as.app.arg_cap * 2u : 4u;
        IdmCore **args = realloc(app->as.app.args, cap * sizeof(*args));
        if (!args) return false;
        app->as.app.args = args;
        app->as.app.arg_cap = cap;
    }
    app->as.app.args[app->as.app.arg_count++] = arg;
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

IdmCore *idm_core_bind_local(uint32_t slot, IdmCore *value, IdmCore *body, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_BIND_LOCAL, span);
    if (!core) return NULL;
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

bool idm_core_fn_add_capture(IdmCore *fn, IdmCaptureKind kind, uint32_t index) {
    if (!fn || fn->kind != IDM_CORE_FN) return false;
    if (fn->as.fn.capture_count == fn->as.fn.capture_cap) {
        size_t cap = fn->as.fn.capture_cap ? fn->as.fn.capture_cap * 2u : 4u;
        IdmCapture *slots = realloc(fn->as.fn.captures, cap * sizeof(*slots));
        if (!slots) return false;
        fn->as.fn.captures = slots;
        fn->as.fn.capture_cap = cap;
    }
    fn->as.fn.captures[fn->as.fn.capture_count].kind = kind;
    fn->as.fn.captures[fn->as.fn.capture_count].index = index;
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

bool idm_core_fn_multi_add_capture(IdmCore *multi, IdmCaptureKind kind, uint32_t index) {
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
    multi->as.fn_multi.captures[multi->as.fn_multi.capture_count].kind = kind;
    multi->as.fn_multi.captures[multi->as.fn_multi.capture_count].index = index;
    multi->as.fn_multi.capture_count++;
    return true;
}

bool idm_core_fn_multi_add_clause_take(IdmCore *multi, uint32_t arity, IdmPattern **patterns, uint32_t pattern_count, IdmPatternLocal *locals, uint32_t local_count, IdmCore *guard, IdmCore *body) {
    if (!multi || multi->kind != IDM_CORE_FN_MULTI || !body) return false;
    if (multi->as.fn_multi.count == multi->as.fn_multi.cap) {
        size_t cap = multi->as.fn_multi.cap ? multi->as.fn_multi.cap * 2u : 4u;
        IdmFnClause *clauses = realloc(multi->as.fn_multi.clauses, cap * sizeof(*clauses));
        if (!clauses) return false;
        multi->as.fn_multi.clauses = clauses;
        multi->as.fn_multi.cap = cap;
    }
    IdmFnClause *clause = &multi->as.fn_multi.clauses[multi->as.fn_multi.count++];
    clause->arity = arity;
    clause->param_patterns = patterns;
    clause->pattern_count = pattern_count;
    clause->pattern_locals = locals;
    clause->pattern_local_count = local_count;
    clause->guard = guard;
    clause->body = body;
    return true;
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

void idm_core_letrec_set_global(IdmCore *letrec) {
    if (letrec && letrec->kind == IDM_CORE_LETREC) letrec->as.letrec.global = true;
}

IdmCore *idm_core_global_ref(uint32_t id, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_GLOBAL_REF, span);
    if (!core) return NULL;
    core->as.local_slot = id;
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

IdmCore *idm_core_raise(IdmCore *value, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_RAISE, span);
    if (!core) return NULL;
    core->as.raise.value = value;
    return core;
}

IdmCore *idm_core_raised(IdmSpan span) {
    return core_alloc(IDM_CORE_RAISED, span);
}

IdmCore *idm_core_rescue(IdmCore *body, IdmCore *handler, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_RESCUE, span);
    if (!core) return NULL;
    core->as.rescue.body = body;
    core->as.rescue.handler = handler;
    return core;
}

IdmCore *idm_core_ensure(IdmCore *body, IdmCore *cleanup, uint32_t tmp_slot, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_ENSURE, span);
    if (!core) return NULL;
    core->as.ensure.body = body;
    core->as.ensure.cleanup = cleanup;
    core->as.ensure.tmp_slot = tmp_slot;
    return core;
}

IdmCore *idm_core_use_package(IdmValue name, IdmBytecodeModule *module, uint32_t init_fn, uint32_t *export_src, uint32_t *export_dst, size_t export_count, IdmCore *cont, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_USE_PACKAGE, span);
    if (!core) return NULL;
    core->as.use_package.name = name;
    core->as.use_package.module = module;
    core->as.use_package.init_fn = init_fn;
    core->as.use_package.export_src = export_src;
    core->as.use_package.export_dst = export_dst;
    core->as.use_package.export_count = export_count;
    core->as.use_package.cont = cont;
    return core;
}

IdmCore *idm_core_define_protocol(IdmValue name, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_DEFINE_PROTOCOL, span);
    if (!core) return NULL;
    core->as.define_protocol.name = name;
    return core;
}

bool idm_core_define_protocol_add_method(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *default_fn) {
    if (!core || core->kind != IDM_CORE_DEFINE_PROTOCOL) return false;
    if (core->as.define_protocol.count == core->as.define_protocol.cap) {
        size_t cap = core->as.define_protocol.cap ? core->as.define_protocol.cap * 2u : 4u;
        IdmCoreProtocolMethod *methods = realloc(core->as.define_protocol.methods, cap * sizeof(*methods));
        if (!methods) return false;
        core->as.define_protocol.methods = methods;
        core->as.define_protocol.cap = cap;
    }
    IdmCoreProtocolMethod *m = &core->as.define_protocol.methods[core->as.define_protocol.count++];
    m->name = method;
    m->arity = arity;
    m->has_default = default_fn != NULL;
    m->default_fn = default_fn;
    return true;
}

IdmCore *idm_core_extend_protocol(IdmValue protocol, IdmValue type, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_EXTEND_PROTOCOL, span);
    if (!core) return NULL;
    core->as.extend_protocol.protocol = protocol;
    core->as.extend_protocol.type = type;
    return core;
}

bool idm_core_extend_protocol_add_impl(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *impl_fn) {
    if (!core || core->kind != IDM_CORE_EXTEND_PROTOCOL || !impl_fn) return false;
    if (core->as.extend_protocol.count == core->as.extend_protocol.cap) {
        size_t cap = core->as.extend_protocol.cap ? core->as.extend_protocol.cap * 2u : 4u;
        IdmCoreProtocolImpl *impls = realloc(core->as.extend_protocol.impls, cap * sizeof(*impls));
        if (!impls) return false;
        core->as.extend_protocol.impls = impls;
        core->as.extend_protocol.cap = cap;
    }
    IdmCoreProtocolImpl *impl = &core->as.extend_protocol.impls[core->as.extend_protocol.count++];
    impl->name = method;
    impl->arity = arity;
    impl->impl_fn = impl_fn;
    return true;
}

IdmCore *idm_core_method_call(IdmValue protocol, IdmValue method, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_METHOD_CALL, span);
    if (!core) return NULL;
    core->as.method_call.protocol = protocol;
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
        case IDM_CORE_RAISE:
            idm_core_free(core->as.raise.value);
            break;
        case IDM_CORE_RAISED:
            break;
        case IDM_CORE_RESCUE:
            idm_core_free(core->as.rescue.body);
            idm_core_free(core->as.rescue.handler);
            break;
        case IDM_CORE_ENSURE:
            idm_core_free(core->as.ensure.body);
            idm_core_free(core->as.ensure.cleanup);
            break;
        case IDM_CORE_USE_PACKAGE:
            idm_core_free(core->as.use_package.cont);
            free(core->as.use_package.export_src);
            free(core->as.use_package.export_dst);
            if (core->as.use_package.module) { idm_bc_destroy(core->as.use_package.module); free(core->as.use_package.module); }
            break;
        case IDM_CORE_DEFINE_PROTOCOL:
            for (size_t i = 0; i < core->as.define_protocol.count; i++) idm_core_free(core->as.define_protocol.methods[i].default_fn);
            free(core->as.define_protocol.methods);
            break;
        case IDM_CORE_EXTEND_PROTOCOL:
            for (size_t i = 0; i < core->as.extend_protocol.count; i++) idm_core_free(core->as.extend_protocol.impls[i].impl_fn);
            free(core->as.extend_protocol.impls);
            break;
        case IDM_CORE_METHOD_CALL:
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) idm_core_free(core->as.method_call.args[i]);
            free(core->as.method_call.args);
            break;
        case IDM_CORE_APP:
            idm_core_free(core->as.app.callee);
            for (size_t i = 0; i < core->as.app.arg_count; i++) idm_core_free(core->as.app.args[i]);
            free(core->as.app.args);
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
            idm_core_free(core->as.bind_local.value);
            idm_core_free(core->as.bind_local.body);
            break;
        case IDM_CORE_FN:
            free(core->as.fn.name);
            free(core->as.fn.captures);
            for (uint32_t i = 0; i < core->as.fn.pattern_count; i++) idm_pat_free(core->as.fn.param_patterns[i]);
            free(core->as.fn.param_patterns);
            pattern_locals_free(core->as.fn.pattern_locals, core->as.fn.pattern_local_count);
            idm_core_free(core->as.fn.guard);
            idm_core_free(core->as.fn.body);
            break;
        case IDM_CORE_FN_MULTI:
            free(core->as.fn_multi.name);
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
        case IDM_CORE_LITERAL:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
        case IDM_CORE_PRIMITIVE:
        case IDM_CORE_GLOBAL_REF:
            break;
    }
    free(core);
}

static bool compile_expr(IdmCore *core, IdmBytecodeModule *module, IdmError *err);

static uint32_t core_max_local_plus_one(IdmCore *core) {
    if (!core) return 0;
    switch (core->kind) {
        case IDM_CORE_LOCAL_REF:
            return core->as.local_slot + 1u;
        case IDM_CORE_ARG_REF:
            return 0;
        case IDM_CORE_CAPTURE_REF:
            return 0;
        case IDM_CORE_APP: {
            uint32_t max = core_max_local_plus_one(core->as.app.callee);
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.app.args[i]);
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
                if (!core->as.letrec.global) {
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
        case IDM_CORE_RAISE:
            return core_max_local_plus_one(core->as.raise.value);
        case IDM_CORE_RAISED:
            return 0;
        case IDM_CORE_RESCUE: {
            uint32_t max = core_max_local_plus_one(core->as.rescue.body);
            uint32_t h = core_max_local_plus_one(core->as.rescue.handler);
            if (h > max) max = h;
            return max;
        }
        case IDM_CORE_ENSURE: {
            uint32_t max = core->as.ensure.tmp_slot + 1u;
            uint32_t b = core_max_local_plus_one(core->as.ensure.body);
            uint32_t c = core_max_local_plus_one(core->as.ensure.cleanup);
            if (b > max) max = b;
            if (c > max) max = c;
            return max;
        }
        case IDM_CORE_USE_PACKAGE:
            return core_max_local_plus_one(core->as.use_package.cont);
        case IDM_CORE_DEFINE_PROTOCOL: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.define_protocol.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.define_protocol.methods[i].default_fn);
                if (child > max) max = child;
            }
            return max;
        }
        case IDM_CORE_EXTEND_PROTOCOL: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.extend_protocol.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.extend_protocol.impls[i].impl_fn);
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
        case IDM_CORE_PRIMITIVE:
        case IDM_CORE_GLOBAL_REF:
            return 0;
    }
    return 0;
}

static IdmOpcode primitive_opcode(IdmPrimitive primitive) {
    switch (primitive) {
        case IDM_PRIM_ADD: return IDM_OP_ADD;
        case IDM_PRIM_SUB: return IDM_OP_SUB;
        case IDM_PRIM_MUL: return IDM_OP_MUL;
        case IDM_PRIM_EQ: return IDM_OP_EQ;
        case IDM_PRIM_LT: return IDM_OP_LT;
        default: return IDM_OP_HALT;
    }
}

static bool actor_primitive_opcode(IdmPrimitive primitive, IdmOpcode *out_op, uint32_t *out_arity) {
    switch (primitive) {
        case IDM_PRIM_SELF: *out_op = IDM_OP_SELF; *out_arity = 0; return true;
        case IDM_PRIM_SPAWN: *out_op = IDM_OP_SPAWN; *out_arity = 1; return true;
        case IDM_PRIM_SEND: *out_op = IDM_OP_SEND; *out_arity = 2; return true;
        case IDM_PRIM_EXIT: *out_op = IDM_OP_EXIT; *out_arity = 1; return true;
        case IDM_PRIM_LINK: *out_op = IDM_OP_LINK; *out_arity = 1; return true;
        case IDM_PRIM_UNLINK: *out_op = IDM_OP_UNLINK; *out_arity = 1; return true;
        case IDM_PRIM_MONITOR: *out_op = IDM_OP_MONITOR; *out_arity = 1; return true;
        case IDM_PRIM_DEMONITOR: *out_op = IDM_OP_DEMONITOR; *out_arity = 1; return true;
        case IDM_PRIM_TRAP_EXIT: *out_op = IDM_OP_TRAP_EXIT; *out_arity = 1; return true;
        case IDM_PRIM_EXEC: *out_op = IDM_OP_EXEC; *out_arity = 1; return true;
        case IDM_PRIM_AWAIT: *out_op = IDM_OP_AWAIT; *out_arity = 1; return true;
        case IDM_PRIM_APPLY: *out_op = IDM_OP_APPLY; *out_arity = 2; return true;
        default: return false;
    }
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

static bool add_guard_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t locals, uint32_t *out_index, IdmError *err, IdmSpan span) {
    char *guard_name = NULL;
    if (!make_guard_function_name(name, &guard_name)) return idm_error_oom(err, span);
    bool ok = idm_bc_add_function(module, guard_name, arity, locals, 0, out_index);
    free(guard_name);
    if (!ok) return idm_error_oom(err, span);
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

static bool compile_function_code(IdmBytecodeModule *module, uint32_t function_index, IdmCore *body, IdmError *err, IdmSpan span) {
    if (!idm_bc_set_function_entry(module, function_index, module->code_count)) return idm_error_set(err, span, "failed to set function entry");
    if (!compile_expr(body, module, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RETURN, NULL)) return idm_error_oom(err, span);
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
    [IDM_PRIM_CONS] = {"cons", 2, 2},
    [IDM_PRIM_FIRST] = {"first", 1, 1},
    [IDM_PRIM_REST] = {"rest", 1, 1},
    [IDM_PRIM_LIST] = {"list", 0, UINT32_MAX},
    [IDM_PRIM_TUPLE] = {"tuple", 0, UINT32_MAX},
    [IDM_PRIM_VECTOR] = {"vector", 0, UINT32_MAX},
    [IDM_PRIM_DICT] = {"dict", 0, UINT32_MAX},
    [IDM_PRIM_TUPLE_GET] = {"tuple-get", 2, 2},
    [IDM_PRIM_TO_LIST] = {"to-list", 1, 1},
    [IDM_PRIM_APPLY] = {"apply", 2, 2},
    [IDM_PRIM_APPEND] = {"append", 2, 2},
    [IDM_PRIM_SYNTAX_KIND] = {"syntax-kind", 1, 1},
    [IDM_PRIM_SYNTAX_TO_DATUM] = {"syntax->datum", 1, 1},
    [IDM_PRIM_DATUM_TO_SYNTAX] = {"datum->syntax", 2, 2},
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
    [IDM_PRIM_MAKE_SYNTAX_WORD] = {"make-syntax-word", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_ATOM] = {"make-syntax-atom", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_INT] = {"make-syntax-int", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_STRING] = {"make-syntax-string", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_LIST] = {"make-syntax-list", 3, 3},
    [IDM_PRIM_MAKE_SYNTAX_VECTOR] = {"make-syntax-vector", 2, 2},
    [IDM_PRIM_MAKE_SYNTAX_TUPLE] = {"make-syntax-tuple", 2, 2},
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
    [IDM_PRIM_EXIT] = {"exit", 1, 1},
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
    [IDM_PRIM_STR_CONTAINS] = {"str-contains?", 2, 2},
    [IDM_PRIM_EXPANDER_REGISTER_OPERATOR] = {"expander-register-operator", 5, 5},
    [IDM_PRIM_EXPANDER_REGISTER_MACRO] = {"expander-register-macro", 2, 2},
    [IDM_PRIM_EXPANDER_SURFACE] = {"expander-surface", 1, 1},
    [IDM_PRIM_EXPAND_CHECK] = {"expand-check", 1, 1},
    [IDM_PRIM_INSPECT] = {"inspect", 1, 1},
    [IDM_PRIM_STR_LEN] = {"str-len", 1, 1},
    [IDM_PRIM_STR_SLICE] = {"str-slice", 3, 3},
    [IDM_PRIM_STR_FIND] = {"str-find", 3, 3},
    [IDM_PRIM_STR_BYTE] = {"str-byte", 2, 2},
    [IDM_PRIM_BYTE_STR] = {"byte-str", 1, 1},
    [IDM_PRIM_FILE_READ] = {"file-read", 1, 1},
    [IDM_PRIM_FILE_WRITE] = {"file-write", 2, 2},
    [IDM_PRIM_FILE_EXISTS] = {"file-exists?", 1, 1},
    [IDM_PRIM_FILE_STAT] = {"file-stat", 1, 1},
    [IDM_PRIM_FILE_LIST] = {"file-list", 1, 1},
    [IDM_PRIM_FILE_REMOVE] = {"file-remove", 1, 1},
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
};

size_t idm_primitive_count(void) {
    return sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]);
}

const IdmPrimitiveInfo *idm_primitive_info(IdmPrimitive primitive) {
    size_t index = (size_t)primitive;
    if (index >= idm_primitive_count() || !PRIMITIVES[index].name) return NULL;
    return &PRIMITIVES[index];
}

const char *idm_primitive_name(IdmPrimitive primitive) {
    const IdmPrimitiveInfo *info = idm_primitive_info(primitive);
    if (info) return info->name;
    return "<bad-primitive>";
}

static bool emit_capture_load(IdmBytecodeModule *module, IdmCapture cap, IdmError *err, IdmSpan span) {
    switch (cap.kind) {
        case IDM_CAP_LOCAL:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_LOCAL, cap.index, NULL)) return idm_error_oom(err, span);
            return true;
        case IDM_CAP_ARG:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_ARG, cap.index, NULL)) return idm_error_oom(err, span);
            if (!idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, span);
            return true;
        case IDM_CAP_UPVALUE:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CAPTURE, cap.index, NULL)) return idm_error_oom(err, span);
            return true;
    }
    return idm_error_set(err, span, "invalid capture kind");
}

static bool emit_load_value(IdmBytecodeModule *module, IdmValue value, IdmError *err, IdmSpan span) {
    uint32_t index = 0;
    if (!idm_bc_add_const(module, value, &index)) return idm_error_oom(err, span);
    if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, index, NULL)) return idm_error_oom(err, span);
    return true;
}

static bool add_const_value(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index, IdmError *err, IdmSpan span) {
    if (!idm_bc_add_const(module, value, out_index)) return idm_error_oom(err, span);
    return true;
}

static bool compile_expr(IdmCore *core, IdmBytecodeModule *module, IdmError *err) {
    if (!idm_bc_note_span(module, core->span)) return idm_error_oom(err, core->span);
    if (!core) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null core expression");
    switch (core->kind) {
        case IDM_CORE_LITERAL: {
            return emit_load_value(module, core->as.literal, err, core->span);
        }
        case IDM_CORE_LOCAL_REF:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_LOCAL, core->as.local_slot, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_ARG_REF:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_ARG, core->as.local_slot, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_CAPTURE_REF:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CAPTURE, core->as.local_slot, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_GLOBAL_REF:
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_GLOBAL, core->as.local_slot, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_PRIMITIVE: {
            uint32_t index = 0;
            if (!idm_bc_add_const(module, idm_primitive_value(core->as.primitive), &index)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, index, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
        case IDM_CORE_APP:
            if (core->as.app.callee && core->as.app.callee->kind == IDM_CORE_PRIMITIVE) {
                for (size_t i = 0; i < core->as.app.arg_count; i++) {
                    if (!compile_expr(core->as.app.args[i], module, err)) return false;
                }
                IdmOpcode actor_op = IDM_OP_HALT;
                uint32_t actor_arity = 0;
                if (actor_primitive_opcode(core->as.app.callee->as.primitive, &actor_op, &actor_arity)) {
                    if (core->as.app.arg_count != actor_arity) return idm_error_set(err, core->span, "primitive '%s' expects %u argument(s)", idm_primitive_name(core->as.app.callee->as.primitive), actor_arity);
                    if (!idm_bc_emit_op(module, actor_op, NULL)) return idm_error_oom(err, core->span);
                    return true;
                }
                IdmOpcode op = primitive_opcode(core->as.app.callee->as.primitive);
                if (op != IDM_OP_HALT) {
                    if (core->as.app.arg_count != 2) return idm_error_set(err, core->span, "primitive '%s' expects exactly two arguments", idm_primitive_name(core->as.app.callee->as.primitive));
                    if (!idm_bc_emit_op(module, op, NULL)) return idm_error_oom(err, core->span);
                    return true;
                }
                if (core->as.app.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "too many primitive arguments");
                if (!idm_bc_emit_u32(module, IDM_OP_PRIM_CALL, (uint32_t)core->as.app.callee->as.primitive, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, (uint32_t)core->as.app.arg_count, NULL)) return idm_error_oom(err, core->span);
                return true;
            }
            if (!compile_expr(core->as.app.callee, module, err)) return false;
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                if (!compile_expr(core->as.app.args[i], module, err)) return false;
            }
            if (core->as.app.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "too many call arguments");
            if (!idm_bc_emit_u32(module, IDM_OP_CALL, (uint32_t)core->as.app.arg_count, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_COND: {
            if (!compile_expr(core->as.cond_expr.cond, module, err)) return false;
            size_t jump_false_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP_IF_FALSE, 0, &jump_false_op)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.cond_expr.then_branch, module, err)) return false;
            size_t jump_end_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_end_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_false_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch cond false jump");
            if (!compile_expr(core->as.cond_expr.else_branch, module, err)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_end_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch cond end jump");
            return true;
        }
        case IDM_CORE_DO:
            if (core->as.do_expr.count == 0) {
                uint32_t index = 0;
                if (!idm_bc_add_const(module, idm_nil(), &index)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, index, NULL)) return idm_error_oom(err, core->span);
                return true;
            }
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!compile_expr(core->as.do_expr.items[i], module, err)) return false;
                if (i + 1u < core->as.do_expr.count && !idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        case IDM_CORE_BIND_LOCAL:
            if (!compile_expr(core->as.bind_local.value, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_STORE_LOCAL, core->as.bind_local.slot, NULL)) return idm_error_oom(err, core->span);
            return compile_expr(core->as.bind_local.body, module, err);
        case IDM_CORE_FN: {
            uint32_t fn_index = 0;
            uint32_t pattern_locals = pattern_local_max_plus_one(core->as.fn.pattern_locals, core->as.fn.pattern_local_count);
            uint32_t locals = max_u32(core_max_local_plus_one(core->as.fn.body), pattern_locals);
            if (!add_compiled_function(module, core->as.fn.name, core->as.fn.arity, locals, core->as.fn.param_patterns, core->as.fn.pattern_count, core->as.fn.pattern_locals, core->as.fn.pattern_local_count, &fn_index, err, core->span)) return false;
            uint32_t guard_index = UINT32_MAX;
            if (core->as.fn.guard) {
                uint32_t guard_locals = max_u32(core_max_local_plus_one(core->as.fn.guard), pattern_locals);
                if (!add_guard_function(module, core->as.fn.name, core->as.fn.arity, guard_locals, &guard_index, err, core->span)) return false;
                if (!idm_bc_set_function_guard(module, fn_index, guard_index)) return idm_error_set(err, core->span, "failed to attach function guard");
            }
            for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                if (!emit_capture_load(module, core->as.fn.captures[i], err, core->span)) return false;
            }
            if (core->as.fn.capture_count == 0) {
                if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE, fn_index, NULL)) return idm_error_oom(err, core->span);
            } else {
                if (core->as.fn.capture_count > UINT32_MAX) return idm_error_set(err, core->span, "too many captures");
                if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE_CAPTURES, fn_index, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, (uint32_t)core->as.fn.capture_count, NULL)) return idm_error_oom(err, core->span);
            }
            size_t jump_over_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_over_op)) return idm_error_oom(err, core->span);
            if (core->as.fn.guard && !compile_function_code(module, guard_index, core->as.fn.guard, err, core->span)) return false;
            if (!compile_function_code(module, fn_index, core->as.fn.body, err, core->span)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_over_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch function literal jump");
            return true;
        }
        case IDM_CORE_FN_MULTI: {
            if (core->as.fn_multi.count == 0) return idm_error_set(err, core->span, "multi function has no clauses");
            if (core->as.fn_multi.count > UINT32_MAX || core->as.fn_multi.capture_count > UINT32_MAX) return idm_error_set(err, core->span, "multi function too large");
            uint32_t *entries = calloc(core->as.fn_multi.count, sizeof(*entries));
            if (!entries) return idm_error_oom(err, core->span);
            uint32_t *guard_entries = calloc(core->as.fn_multi.count, sizeof(*guard_entries));
            if (!guard_entries) { free(entries); return idm_error_oom(err, core->span); }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) guard_entries[i] = UINT32_MAX;
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                uint32_t pattern_locals = pattern_local_max_plus_one(core->as.fn_multi.clauses[i].pattern_locals, core->as.fn_multi.clauses[i].pattern_local_count);
                uint32_t locals = max_u32(core_max_local_plus_one(core->as.fn_multi.clauses[i].body), pattern_locals);
                if (!add_compiled_function(module, core->as.fn_multi.name, core->as.fn_multi.clauses[i].arity, locals, core->as.fn_multi.clauses[i].param_patterns, core->as.fn_multi.clauses[i].pattern_count, core->as.fn_multi.clauses[i].pattern_locals, core->as.fn_multi.clauses[i].pattern_local_count, &entries[i], err, core->span)) {
                    free(guard_entries);
                    free(entries);
                    return false;
                }
                if (core->as.fn_multi.clauses[i].guard) {
                    uint32_t guard_locals = max_u32(core_max_local_plus_one(core->as.fn_multi.clauses[i].guard), pattern_locals);
                    if (!add_guard_function(module, core->as.fn_multi.name, core->as.fn_multi.clauses[i].arity, guard_locals, &guard_entries[i], err, core->span)) {
                        free(guard_entries);
                        free(entries);
                        return false;
                    }
                    if (!idm_bc_set_function_guard(module, entries[i], guard_entries[i])) {
                        free(guard_entries);
                        free(entries);
                        return idm_error_set(err, core->span, "failed to attach multi function guard");
                    }
                }
            }
            for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                if (!emit_capture_load(module, core->as.fn_multi.captures[i], err, core->span)) { free(guard_entries); free(entries); return false; }
            }
            if (!idm_bc_emit_u32(module, IDM_OP_MAKE_MULTI_CLOSURE, (uint32_t)core->as.fn_multi.count, NULL)) { free(guard_entries); free(entries); return idm_error_oom(err, core->span); }
            if (!idm_bc_emit(module, (uint32_t)core->as.fn_multi.capture_count, NULL)) { free(guard_entries); free(entries); return idm_error_oom(err, core->span); }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!idm_bc_emit(module, entries[i], NULL)) { free(guard_entries); free(entries); return idm_error_oom(err, core->span); }
            }
            size_t jump_over_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_over_op)) { free(guard_entries); free(entries); return idm_error_oom(err, core->span); }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (core->as.fn_multi.clauses[i].guard && !compile_function_code(module, guard_entries[i], core->as.fn_multi.clauses[i].guard, err, core->span)) { free(guard_entries); free(entries); return false; }
                if (!compile_function_code(module, entries[i], core->as.fn_multi.clauses[i].body, err, core->span)) { free(guard_entries); free(entries); return false; }
            }
            free(guard_entries);
            free(entries);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_over_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch multi function jump");
            return true;
        }
        case IDM_CORE_LETREC: {
            if (core->as.letrec.global) {
                for (size_t i = 0; i < core->as.letrec.count; i++) {
                    if (!compile_expr(core->as.letrec.bindings[i].value, module, err)) return false;
                    if (!idm_bc_emit_u32(module, IDM_OP_STORE_GLOBAL, core->as.letrec.bindings[i].slot, NULL)) return idm_error_oom(err, core->span);
                }
                return compile_expr(core->as.letrec.body, module, err);
            }
            uint32_t nil_const = 0;
            if (!idm_bc_add_const(module, idm_nil(), &nil_const)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, nil_const, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit_u32(module, IDM_OP_STORE_LOCAL, core->as.letrec.bindings[i].slot, NULL)) return idm_error_oom(err, core->span);
            }
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!idm_bc_emit_u32(module, IDM_OP_LOAD_LOCAL, core->as.letrec.bindings[i].slot, NULL)) return idm_error_oom(err, core->span);
                if (!compile_expr(core->as.letrec.bindings[i].value, module, err)) return false;
                if (!idm_bc_emit_op(module, IDM_OP_STORE_CELL, NULL)) return idm_error_oom(err, core->span);
            }
            return compile_expr(core->as.letrec.body, module, err);
        }
        case IDM_CORE_RECEIVE: {
            if (!core->as.receive.receiver || !core->as.receive.timeout || !core->as.receive.timeout_body) return idm_error_set(err, core->span, "receive is missing a required component");
            if (!compile_expr(core->as.receive.timeout, module, err)) return false;
            if (!compile_expr(core->as.receive.receiver, module, err)) return false;
            size_t recv_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_RECV, 0, &recv_op)) return idm_error_oom(err, core->span);
            size_t jump_done_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, recv_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch receive timeout target");
            if (!compile_expr(core->as.receive.timeout_body, module, err)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_done_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch receive done jump");
            return true;
        }
        case IDM_CORE_RAISE:
            if (!compile_expr(core->as.raise.value, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_RAISE, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_RAISED:
            if (!idm_bc_emit_op(module, IDM_OP_LOAD_RAISED, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_RESCUE: {
            size_t push_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.rescue.body, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
            size_t jump_done_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, push_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch rescue catch target");
            if (!compile_expr(core->as.rescue.handler, module, err)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_done_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch rescue done jump");
            return true;
        }
        case IDM_CORE_ENSURE: {
            size_t push_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.ensure.body, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.ensure.cleanup, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            size_t jump_done_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, push_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch ensure catch target");
            if (!idm_bc_emit_op(module, IDM_OP_LOAD_RAISED, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_STORE_LOCAL, core->as.ensure.tmp_slot, NULL)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.ensure.cleanup, module, err)) return false;
            if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_LOCAL, core->as.ensure.tmp_slot, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_RAISE, NULL)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_done_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch ensure done jump");
            return true;
        }
        case IDM_CORE_USE_PACKAGE: {
            size_t skip_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &skip_op)) return idm_error_oom(err, core->span);
            uint32_t const_off = 0;
            uint32_t fn_off = 0;
            uint32_t code_off = 0;
            if (!idm_bc_link(module, core->as.use_package.module, &const_off, &fn_off, &code_off, err)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, skip_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch package skip jump");
            uint32_t name_index = 0;
            if (!idm_bc_add_const(module, core->as.use_package.name, &name_index)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_ENTER_NAMESPACE, name_index, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE, fn_off + core->as.use_package.init_fn, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_CALL, 0u, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.use_package.export_count; i++) {
                if (!idm_bc_emit_u32(module, IDM_OP_IMPORT_GLOBAL, core->as.use_package.export_src[i], NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, core->as.use_package.export_dst[i], NULL)) return idm_error_oom(err, core->span);
            }
            if (!idm_bc_emit_op(module, IDM_OP_LEAVE_NAMESPACE, NULL)) return idm_error_oom(err, core->span);
            return compile_expr(core->as.use_package.cont, module, err);
        }
        case IDM_CORE_DEFINE_PROTOCOL: {
            if (core->as.define_protocol.count > UINT32_MAX) return idm_error_set(err, core->span, "protocol has too many methods");
            for (size_t i = 0; i < core->as.define_protocol.count; i++) {
                IdmCoreProtocolMethod *method = &core->as.define_protocol.methods[i];
                if (method->has_default) {
                    if (!compile_expr(method->default_fn, module, err)) return false;
                } else {
                    if (!emit_load_value(module, idm_nil(), err, core->span)) return false;
                }
            }
            uint32_t protocol_const = 0;
            if (!add_const_value(module, core->as.define_protocol.name, &protocol_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, IDM_OP_DEFINE_PROTOCOL, protocol_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.define_protocol.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.define_protocol.count; i++) {
                IdmCoreProtocolMethod *method = &core->as.define_protocol.methods[i];
                uint32_t method_const = 0;
                if (!add_const_value(module, method->name, &method_const, err, core->span)) return false;
                if (!idm_bc_emit(module, method_const, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, method->arity, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, method->has_default ? 1u : 0u, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_EXTEND_PROTOCOL: {
            if (core->as.extend_protocol.count > UINT32_MAX) return idm_error_set(err, core->span, "extend has too many methods");
            for (size_t i = 0; i < core->as.extend_protocol.count; i++) {
                if (!compile_expr(core->as.extend_protocol.impls[i].impl_fn, module, err)) return false;
            }
            uint32_t protocol_const = 0;
            uint32_t type_const = 0;
            if (!add_const_value(module, core->as.extend_protocol.protocol, &protocol_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.extend_protocol.type, &type_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, IDM_OP_EXTEND_PROTOCOL, protocol_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, type_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.extend_protocol.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.extend_protocol.count; i++) {
                IdmCoreProtocolImpl *impl = &core->as.extend_protocol.impls[i];
                uint32_t method_const = 0;
                if (!add_const_value(module, impl->name, &method_const, err, core->span)) return false;
                if (!idm_bc_emit(module, method_const, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, impl->arity, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_METHOD_CALL: {
            if (core->as.method_call.arg_count == 0) return idm_error_set(err, core->span, "method call requires a receiver");
            if (core->as.method_call.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "method call has too many arguments");
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                if (!compile_expr(core->as.method_call.args[i], module, err)) return false;
            }
            uint32_t protocol_const = 0;
            uint32_t method_const = 0;
            if (!add_const_value(module, core->as.method_call.protocol, &protocol_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.method_call.method, &method_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, IDM_OP_CALL_METHOD, protocol_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, method_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.method_call.arg_count, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
    }
    return idm_error_set(err, core->span, "unknown core node");
}

bool idm_core_compile_expression(IdmCore *core, IdmBytecodeModule *module, IdmError *err) {
    return compile_expr(core, module, err);
}

bool idm_core_compile_function_body(IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!body) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null function body");
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(body);
    if (!idm_bc_add_function(module, name ? name : "<function>", arity, locals, 0, &fn)) return idm_error_oom(err, body->span);
    if (!compile_function_code(module, fn, body, err, body->span)) return false;
    if (out_function) *out_function = fn;
    return true;
}

bool idm_core_compile_main(IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "main", 0, locals, 0, &fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_bc_set_function_entry(module, fn, module->code_count)) return idm_error_set(err, idm_span_unknown(NULL), "failed to set main entry");
    if (!compile_expr(core, module, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RETURN, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
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

bool idm_core_dump(IdmBuffer *buf, const IdmCore *core) {
    if (!core) return idm_buf_append(buf, "#<null-core>");
    switch (core->kind) {
        case IDM_CORE_LITERAL:
            return dump_value(buf, core->as.literal);
        case IDM_CORE_ARG_REF:
            return idm_buf_appendf(buf, "(arg %u)", core->as.local_slot);
        case IDM_CORE_LOCAL_REF:
            return idm_buf_appendf(buf, "(local %u)", core->as.local_slot);
        case IDM_CORE_CAPTURE_REF:
            return idm_buf_appendf(buf, "(capture %u)", core->as.local_slot);
        case IDM_CORE_PRIMITIVE:
            return idm_buf_appendf(buf, "(prim %s)", idm_primitive_name(core->as.primitive));
        case IDM_CORE_APP:
            if (!idm_buf_append(buf, "(app ")) return false;
            if (!idm_core_dump(buf, core->as.app.callee)) return false;
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_core_dump(buf, core->as.app.args[i])) return false;
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
            return idm_buf_appendf(buf, "(bind-local %u ", core->as.bind_local.slot) &&
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
                    if (!idm_buf_appendf(buf, "%s%u", capture_kind_tag(core->as.fn.captures[i].kind), core->as.fn.captures[i].index)) return false;
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
                    if (!idm_buf_appendf(buf, "%s%u", capture_kind_tag(core->as.fn_multi.captures[i].kind), core->as.fn_multi.captures[i].index)) return false;
                }
                if (!idm_buf_append_char(buf, ']')) return false;
            }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!idm_buf_appendf(buf, " (/%u ", core->as.fn_multi.clauses[i].arity)) return false;
                if (core->as.fn_multi.clauses[i].guard) {
                    if (!idm_buf_append(buf, "guard ")) return false;
                    if (!idm_core_dump(buf, core->as.fn_multi.clauses[i].guard)) return false;
                    if (!idm_buf_append_char(buf, ' ')) return false;
                }
                if (!idm_core_dump(buf, core->as.fn_multi.clauses[i].body)) return false;
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_LETREC:
            if (!idm_buf_append(buf, "(letrec (")) return false;
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_buf_appendf(buf, "(%s #%u ", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
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
        case IDM_CORE_RAISE:
            return idm_buf_append(buf, "(raise ") && idm_core_dump(buf, core->as.raise.value) && idm_buf_append_char(buf, ')');
        case IDM_CORE_RAISED:
            return idm_buf_append(buf, "raised");
        case IDM_CORE_GLOBAL_REF:
            return idm_buf_appendf(buf, "(global %u)", core->as.local_slot);
        case IDM_CORE_RESCUE:
            return idm_buf_append(buf, "(rescue ") &&
                   idm_core_dump(buf, core->as.rescue.body) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.rescue.handler) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_ENSURE:
            return idm_buf_append(buf, "(ensure ") &&
                   idm_core_dump(buf, core->as.ensure.body) &&
                   idm_buf_append_char(buf, ' ') &&
                   idm_core_dump(buf, core->as.ensure.cleanup) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_USE_PACKAGE:
            return idm_buf_append(buf, "(use-package ") &&
                   dump_value(buf, core->as.use_package.name) &&
                   idm_buf_appendf(buf, " init=%u exports=%zu ", core->as.use_package.init_fn, core->as.use_package.export_count) &&
                   idm_core_dump(buf, core->as.use_package.cont) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_DEFINE_PROTOCOL:
            if (!idm_buf_append(buf, "(define-protocol ") || !dump_value(buf, core->as.define_protocol.name)) return false;
            for (size_t i = 0; i < core->as.define_protocol.count; i++) {
                const IdmCoreProtocolMethod *method = &core->as.define_protocol.methods[i];
                if (!idm_buf_append(buf, " (method ") || !dump_value(buf, method->name) || !idm_buf_appendf(buf, "/%u", method->arity)) return false;
                if (method->has_default) {
                    if (!idm_buf_append(buf, " default=") || !idm_core_dump(buf, method->default_fn)) return false;
                }
                if (!idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_EXTEND_PROTOCOL:
            if (!idm_buf_append(buf, "(extend-protocol ") || !dump_value(buf, core->as.extend_protocol.protocol) ||
                !idm_buf_append(buf, " type=") || !dump_value(buf, core->as.extend_protocol.type)) return false;
            for (size_t i = 0; i < core->as.extend_protocol.count; i++) {
                const IdmCoreProtocolImpl *impl = &core->as.extend_protocol.impls[i];
                if (!idm_buf_append(buf, " (impl ") || !dump_value(buf, impl->name) || !idm_buf_appendf(buf, "/%u ", impl->arity) || !idm_core_dump(buf, impl->impl_fn) || !idm_buf_append_char(buf, ')')) return false;
            }
            return idm_buf_append_char(buf, ')');
        case IDM_CORE_METHOD_CALL:
            if (!idm_buf_append(buf, "(method-call ") || !dump_value(buf, core->as.method_call.protocol) || !idm_buf_append_char(buf, ' ') || !dump_value(buf, core->as.method_call.method)) return false;
            for (size_t i = 0; i < core->as.method_call.arg_count; i++) {
                if (!idm_buf_append_char(buf, ' ') || !idm_core_dump(buf, core->as.method_call.args[i])) return false;
            }
            return idm_buf_append_char(buf, ')');
    }
    return false;
}
