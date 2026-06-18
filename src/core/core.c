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

IdmCore *idm_core_primitive(IdmPrimitive primitive, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_PRIMITIVE, span);
    if (!core) return NULL;
    core->as.primitive = primitive;
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

void idm_core_letrec_set_fill_only(IdmCore *letrec) {
    if (letrec && letrec->kind == IDM_CORE_LETREC) letrec->as.letrec.fill_only = true;
}

IdmCore *idm_core_global_ref(const char *name, uint32_t id, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_GLOBAL_REF, span);
    if (!core) return NULL;
    if (!core_slot_init(&core->as.slot_ref, name, id)) { free(core); return NULL; }
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

IdmCore *idm_core_use_package(IdmValue name, IdmBytecodeModule *module, uint32_t init_fn, uint32_t *export_src, uint32_t *export_dst, char **export_names, size_t export_count, IdmCore *cont, IdmSpan span) {
    IdmCore *core = core_alloc(IDM_CORE_USE_PACKAGE, span);
    if (!core) return NULL;
    core->as.use_package.name = name;
    core->as.use_package.module = module;
    core->as.use_package.init_fn = init_fn;
    core->as.use_package.export_src = export_src;
    core->as.use_package.export_dst = export_dst;
    core->as.use_package.export_names = export_names;
    core->as.use_package.export_count = export_count;
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

bool idm_core_define_trait_add_method(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *default_fn) {
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

bool idm_core_implement_trait_add_impl(IdmCore *core, IdmValue method, uint32_t arity, IdmCore *impl_fn) {
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
            free(core->as.use_package.export_src);
            free(core->as.use_package.export_dst);
            for (size_t i = 0; i < core->as.use_package.export_count; i++) free(core->as.use_package.export_names[i]);
            free(core->as.use_package.export_names);
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
        case IDM_CORE_GLOBAL_REF:
        case IDM_CORE_ARG_REF:
        case IDM_CORE_LOCAL_REF:
        case IDM_CORE_CAPTURE_REF:
            free(core->as.slot_ref.name);
            break;
        case IDM_CORE_LITERAL:
        case IDM_CORE_PRIMITIVE:
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

static bool literal_container_call(IdmRuntime *rt, const IdmCore *call, IdmValue *out, bool *out_folded, IdmError *err) {
    *out_folded = false;
    if (!call->as.call.callee || call->as.call.callee->kind != IDM_CORE_PRIMITIVE) return true;
    IdmPrimitive prim = call->as.call.callee->as.primitive;
    if (prim != IDM_PRIM_LIST && prim != IDM_PRIM_TUPLE && prim != IDM_PRIM_VECTOR && prim != IDM_PRIM_DICT) return true;
    if (prim == IDM_PRIM_DICT && call->as.call.arg_count % 2u != 0) return true;
    for (size_t i = 0; i < call->as.call.arg_count; i++) {
        if (!call->as.call.args[i] || call->as.call.args[i]->kind != IDM_CORE_LITERAL) return true;
    }
    switch (prim) {
        case IDM_PRIM_LIST: {
            IdmValue list = idm_empty_list();
            for (size_t i = call->as.call.arg_count; i > 0; i--) {
                list = idm_cons(rt, call->as.call.args[i - 1u]->as.literal, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            *out_folded = true;
            return true;
        }
        case IDM_PRIM_TUPLE:
        case IDM_PRIM_VECTOR: {
            IdmValue *items = call->as.call.arg_count == 0 ? NULL : malloc(call->as.call.arg_count * sizeof(*items));
            if (call->as.call.arg_count != 0 && !items) return idm_error_oom(err, call->span);
            for (size_t i = 0; i < call->as.call.arg_count; i++) items[i] = call->as.call.args[i]->as.literal;
            *out = prim == IDM_PRIM_TUPLE
                ? idm_tuple(rt, items, call->as.call.arg_count, err)
                : idm_vector(rt, items, call->as.call.arg_count, err);
            free(items);
            if (err && err->present) return false;
            *out_folded = true;
            return true;
        }
        case IDM_PRIM_DICT: {
            size_t count = call->as.call.arg_count / 2u;
            IdmDictEntry *entries = count == 0 ? NULL : malloc(count * sizeof(*entries));
            if (count != 0 && !entries) return idm_error_oom(err, call->span);
            for (size_t i = 0; i < count; i++) {
                entries[i].key = call->as.call.args[i * 2u]->as.literal;
                entries[i].value = call->as.call.args[i * 2u + 1u]->as.literal;
            }
            *out = idm_dict(rt, entries, count, err);
            free(entries);
            if (err && err->present) return false;
            *out_folded = true;
            return true;
        }
        default:
            return true;
    }
}

static bool normalize_call(IdmRuntime *rt, IdmCore **slot, IdmError *err) {
    IdmCore *core = *slot;
    if (!normalize_core(rt, &core->as.call.callee, err)) return false;
    for (size_t i = 0; i < core->as.call.arg_count; i++) {
        if (!normalize_core(rt, &core->as.call.args[i], err)) return false;
    }
    IdmValue value = idm_nil();
    bool folded = false;
    if (!literal_container_call(rt, core, &value, &folded, err)) return false;
    if (folded) {
        IdmCore *literal = idm_core_literal(value, core->span);
        if (!literal) return idm_error_oom(err, core->span);
        return replace_core(slot, literal, err, core->span);
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
        case IDM_CORE_PRIMITIVE:
        case IDM_CORE_GLOBAL_REF:
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
    uint32_t global_slot;
    const CompiledCallable *callable;
} KnownCallable;

typedef struct CompileContext {
    const struct CompileContext *parent;
    const KnownCallable *known;
    size_t known_count;
} CompileContext;

static bool compile_expr(IdmCore *core, IdmBytecodeModule *module, bool tail, const CompileContext *ctx, IdmError *err);

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
        case IDM_CORE_PRIMITIVE:
        case IDM_CORE_GLOBAL_REF:
            return 0;
    }
    return 0;
}

static bool actor_primitive_opcode(IdmPrimitive primitive, size_t argc, IdmOpcode *out_op, uint32_t *out_arity) {
    switch (primitive) {
        case IDM_PRIM_SELF: *out_op = IDM_OP_SELF; *out_arity = 0; return true;
        case IDM_PRIM_SPAWN: *out_op = IDM_OP_SPAWN; *out_arity = 1; return true;
        case IDM_PRIM_SPAWN_LINK: *out_op = IDM_OP_SPAWN_LINK; *out_arity = 1; return true;
        case IDM_PRIM_SPAWN_MONITOR: *out_op = IDM_OP_SPAWN_MONITOR; *out_arity = 1; return true;
        case IDM_PRIM_SEND: *out_op = IDM_OP_SEND; *out_arity = 2; return true;
        case IDM_PRIM_EXIT:
            if (argc >= 2) { *out_op = IDM_OP_EXIT_SIGNAL; *out_arity = 2; return true; }
            *out_op = IDM_OP_EXIT;
            *out_arity = 1;
            return true;
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
                if (!core->as.letrec.global && core->as.letrec.bindings[i].slot < lc) celled[core->as.letrec.bindings[i].slot] = true;
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
        case IDM_CORE_GLOBAL_REF:
        case IDM_CORE_LITERAL:
        case IDM_CORE_PRIMITIVE:
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
        case IDM_CORE_GLOBAL_REF:
        case IDM_CORE_LITERAL:
        case IDM_CORE_PRIMITIVE:
            return;
    }
}

static bool compile_function_code(IdmBytecodeModule *module, uint32_t function_index, IdmCore *body, const CompileContext *ctx, IdmError *err, IdmSpan span, const IdmCapture *captures, size_t capture_count) {
    if (!idm_bc_set_function_entry(module, function_index, module->code_count)) return idm_error_set(err, span, "failed to set function entry");
    uint32_t lc = module->functions[function_index].local_count;
    bool *celled = NULL;
    if (lc > 0) {
        celled = calloc(lc, sizeof(*celled));
        if (!celled) return idm_error_oom(err, span);
        collect_celled_slots(body, celled, lc);
    }
    mark_celled(body, celled, lc, captures, capture_count);
    free(celled);
    if (!compile_expr(body, module, true, ctx, err)) return false;
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
    [IDM_PRIM_FILE_OPEN] = {"file-open", 2, 2},
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
    [IDM_PRIM_MAKE_SYNTAX_LIST] = {"make-syntax-list", 2, 2},
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
    [IDM_PRIM_REGEX_COMPILE] = {"regex-raw-compile", 2, 2},
    [IDM_PRIM_REGEX_PRED] = {"regex-raw-regex?", 1, 1},
    [IDM_PRIM_REGEX_SOURCE] = {"regex-raw-source", 1, 1},
    [IDM_PRIM_REGEX_OPTIONS] = {"regex-raw-options", 1, 1},
    [IDM_PRIM_REGEX_GROUP_COUNT] = {"regex-raw-group-count", 1, 1},
    [IDM_PRIM_REGEX_GROUP_NAMES] = {"regex-raw-group-names", 1, 1},
    [IDM_PRIM_REGEX_RESULT_PRED] = {"regex-raw-result?", 1, 1},
    [IDM_PRIM_REGEX_SCAN_AT] = {"regex-raw-scan-at", 3, 3},
    [IDM_PRIM_REGEX_SCAN_FROM] = {"regex-raw-scan-from", 3, 3},
    [IDM_PRIM_REGEX_SCAN_FULL] = {"regex-raw-scan-full", 2, 2},
    [IDM_PRIM_REGEX_TEST] = {"regex-raw-test?", 2, 2},
    [IDM_PRIM_REGEX_RESULT_START] = {"regex-raw-result-start", 1, 1},
    [IDM_PRIM_REGEX_RESULT_END] = {"regex-raw-result-end", 1, 1},
    [IDM_PRIM_REGEX_RESULT_TEXT] = {"regex-raw-result-text", 1, 1},
    [IDM_PRIM_REGEX_CAPTURE] = {"regex-raw-capture", 2, 2},
    [IDM_PRIM_REGEX_CAPTURE_RANGE] = {"regex-raw-capture-range", 2, 2},
    [IDM_PRIM_REGEX_CAPTURE_NAMED] = {"regex-raw-capture-named", 2, 2},
    [IDM_PRIM_REGEX_CAPTURES] = {"regex-raw-captures", 1, 1},
    [IDM_PRIM_REGEX_SCAN_ALL] = {"regex-raw-scan-all", 2, 2},
    [IDM_PRIM_REGEX_REPLACE] = {"regex-raw-replace", 3, 3},
    [IDM_PRIM_REGEX_REPLACE_ALL] = {"regex-raw-replace-all", 3, 3},
    [IDM_PRIM_REGEX_SPLIT_ON] = {"regex-raw-split-on", 2, 2},
    [IDM_PRIM_REGEX_ESCAPE] = {"regex-raw-escape", 1, 1},
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
    [IDM_PRIM_ABS] = {"abs", 1, 1},
    [IDM_PRIM_FLOOR] = {"floor", 1, 1},
    [IDM_PRIM_ROUND] = {"round", 1, 1},
    [IDM_PRIM_SQRT] = {"sqrt", 1, 1},
    [IDM_PRIM_FLOOR_DIV] = {"floor-div", 2, 2},
    [IDM_PRIM_FLOOR_MOD] = {"floor-mod", 2, 2},
    [IDM_PRIM_PARSE_INT] = {"parse-int", 1, 1},
    [IDM_PRIM_PARSE_FLOAT] = {"parse-float", 1, 1},
    [IDM_PRIM_FILE_MKDIR] = {"file-mkdir", 1, 1},
    [IDM_PRIM_FILE_APPEND] = {"file-append", 2, 2},
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
    [IDM_PRIM_PRIMITIVE_P] = {"primitive?", 1, 1},
    [IDM_PRIM_REGEX_P] = {"regex?", 1, 1},
    [IDM_PRIM_REGEX_RESULT_P] = {"regex-result?", 1, 1},
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

static bool emit_named_u32(IdmBytecodeModule *module, IdmOpcode op, uint32_t operand, const char *name, IdmError *err, IdmSpan span) {
    size_t op_offset = 0;
    if (!idm_bc_emit_u32(module, op, operand, &op_offset)) return idm_error_oom(err, span);
    if (!idm_bc_note_name(module, op_offset, name)) return idm_error_oom(err, span);
    return true;
}

static bool emit_capture_load(IdmBytecodeModule *module, IdmCapture cap, IdmError *err, IdmSpan span) {
    switch (cap.kind) {
        case IDM_CAP_LOCAL:
            return emit_named_u32(module, IDM_OP_LOAD_LOCAL, cap.index, cap.name, err, span);
        case IDM_CAP_ARG:
            if (!emit_named_u32(module, IDM_OP_LOAD_ARG, cap.index, cap.name, err, span)) return false;
            if (cap.celled && !idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, span);
            return true;
        case IDM_CAP_UPVALUE:
            return emit_named_u32(module, IDM_OP_LOAD_CAPTURE, cap.index, cap.name, err, span);
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

static bool compile_callable_bodies(IdmCore *fn, const CompiledCallable *callable, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err, IdmSpan span) {
    if (fn->kind == IDM_CORE_FN) {
        if (fn->as.fn.guard && !compile_function_code(module, callable->guards[0], fn->as.fn.guard, ctx, err, span, fn->as.fn.captures, fn->as.fn.capture_count)) return false;
        return compile_function_code(module, callable->entries[0], fn->as.fn.body, ctx, err, span, fn->as.fn.captures, fn->as.fn.capture_count);
    }
    for (size_t i = 0; i < fn->as.fn_multi.count; i++) {
        IdmFnClause *clause = &fn->as.fn_multi.clauses[i];
        if (clause->guard && !compile_function_code(module, callable->guards[i], clause->guard, ctx, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
        if (!compile_function_code(module, callable->entries[i], clause->body, ctx, err, span, fn->as.fn_multi.captures, fn->as.fn_multi.capture_count)) return false;
    }
    return true;
}

static bool emit_callable_captures(IdmBytecodeModule *module, const CompiledCallable *callable, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < callable->capture_count; i++) {
        if (!emit_capture_load(module, callable->captures[i], err, span)) return false;
    }
    return true;
}

static bool emit_callable_closure(IdmBytecodeModule *module, const CompiledCallable *callable, IdmError *err, IdmSpan span) {
    if (!emit_callable_captures(module, callable, err, span)) return false;
    if (callable->count == 1u) {
        if (callable->capture_count == 0) {
            if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE, callable->entries[0], NULL)) return idm_error_oom(err, span);
        } else {
            if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE_CAPTURES, callable->entries[0], NULL)) return idm_error_oom(err, span);
            if (!idm_bc_emit(module, (uint32_t)callable->capture_count, NULL)) return idm_error_oom(err, span);
        }
        return true;
    }
    if (!idm_bc_emit_u32(module, IDM_OP_MAKE_MULTI_CLOSURE, (uint32_t)callable->count, NULL)) return idm_error_oom(err, span);
    if (!idm_bc_emit(module, (uint32_t)callable->capture_count, NULL)) return idm_error_oom(err, span);
    for (size_t i = 0; i < callable->count; i++) {
        if (!idm_bc_emit(module, callable->entries[i], NULL)) return idm_error_oom(err, span);
    }
    return true;
}

static bool compile_callable_literal(IdmCore *fn, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err) {
    CompiledCallable callable;
    if (!compile_callable_entries(fn, module, &callable, err, fn->span)) return false;
    bool ok = emit_callable_closure(module, &callable, err, fn->span);
    size_t jump_over_op = 0;
    if (ok && !idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, fn->span);
    if (ok) ok = compile_callable_bodies(fn, &callable, module, ctx, err, fn->span);
    if (ok) ok = patch_here(module, jump_over_op + 1u, err, fn->span, fn->kind == IDM_CORE_FN ? "function literal jump" : "multi function jump");
    compiled_callable_destroy(&callable);
    return ok;
}

static bool emit_direct_callable_call(const CompiledCallable *callable, IdmCore **args, size_t arg_count, IdmBytecodeModule *module, bool tail, const CompileContext *ctx, IdmError *err, IdmSpan span) {
    if (arg_count > IDM_CALL_ARGC_MASK) return idm_error_set(err, span, "too many call arguments");
    bool ok = emit_callable_captures(module, callable, err, span);
    for (size_t i = 0; ok && i < arg_count; i++) ok = compile_expr(args[i], module, false, ctx, err);
    if (ok) {
        uint32_t operand = IDM_CALL_DIRECT_FLAG | (uint32_t)arg_count;
        ok = idm_bc_emit_u32(module, tail ? IDM_OP_TAIL_CALL : IDM_OP_CALL, operand, NULL) &&
             idm_bc_emit(module, (uint32_t)callable->count, NULL) &&
             idm_bc_emit(module, (uint32_t)callable->capture_count, NULL);
        if (!ok) ok = idm_error_oom(err, span);
    }
    for (size_t i = 0; ok && i < callable->count; i++) {
        if (!idm_bc_emit(module, callable->entries[i], NULL)) ok = idm_error_oom(err, span);
    }
    return ok;
}

static bool compile_direct_callable_call(IdmCore *fn, IdmCore **args, size_t arg_count, IdmBytecodeModule *module, bool tail, const CompileContext *ctx, IdmError *err, IdmSpan span) {
    CompiledCallable callable;
    if (!compile_callable_entries(fn, module, &callable, err, span)) return false;
    bool ok = emit_direct_callable_call(&callable, args, arg_count, module, tail, ctx, err, span);
    size_t jump_over_op = 0;
    if (ok && !idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, span);
    if (ok) ok = compile_callable_bodies(fn, &callable, module, ctx, err, span);
    if (ok) ok = patch_here(module, jump_over_op + 1u, err, span, "direct function call body jump");
    compiled_callable_destroy(&callable);
    return ok;
}

static const CompiledCallable *known_callable_for_ref(const CompileContext *ctx, const IdmCore *ref) {
    if (!ref || ref->kind != IDM_CORE_GLOBAL_REF) return NULL;
    for (const CompileContext *c = ctx; c; c = c->parent) {
        for (size_t i = 0; i < c->known_count; i++) {
            const KnownCallable *known = &c->known[i];
            if (known->global_slot == ref->as.slot_ref.slot) return known->callable;
        }
    }
    return NULL;
}

static bool core_is_callable(const IdmCore *core) {
    return core && (core->kind == IDM_CORE_FN || core->kind == IDM_CORE_FN_MULTI);
}

static void destroy_compiled_callables(CompiledCallable *callables, const bool *compiled, size_t count) {
    if (!callables || !compiled) return;
    for (size_t i = 0; i < count; i++) {
        if (compiled[i]) compiled_callable_destroy(&callables[i]);
    }
}

static bool compile_letrec(IdmCore *core, IdmBytecodeModule *module, bool tail, const CompileContext *ctx, IdmError *err) {
    size_t count = core->as.letrec.count;
    CompiledCallable *callables = count == 0 ? NULL : calloc(count, sizeof(*callables));
    bool *compiled = count == 0 ? NULL : calloc(count, sizeof(*compiled));
    KnownCallable *known = count == 0 ? NULL : calloc(count, sizeof(*known));
    if (count != 0 && (!callables || !compiled || !known)) {
        free(callables);
        free(compiled);
        free(known);
        return idm_error_oom(err, core->span);
    }

    bool ok = true;
    size_t known_count = 0;
    size_t compiled_count = 0;
    if (core->as.letrec.global) {
        for (size_t i = 0; ok && i < count; i++) {
            IdmLetRecBinding *binding = &core->as.letrec.bindings[i];
            if (!core_is_callable(binding->value)) continue;
            ok = compile_callable_entries(binding->value, module, &callables[i], err, binding->value->span);
            if (!ok) break;
            compiled[i] = true;
            compiled_count++;
            if (callables[i].capture_count == 0) {
                known[known_count].global_slot = binding->slot;
                known[known_count].callable = &callables[i];
                known_count++;
            }
        }
    }

    CompileContext child = { ctx, known, known_count };
    const CompileContext *body_ctx = known_count == 0 ? ctx : &child;

    if (ok && core->as.letrec.global) {
        for (size_t i = 0; ok && i < count; i++) {
            IdmLetRecBinding *binding = &core->as.letrec.bindings[i];
            if (compiled[i]) ok = emit_callable_closure(module, &callables[i], err, binding->value->span);
            else ok = compile_expr(binding->value, module, false, body_ctx, err);
            if (ok) ok = emit_named_u32(module, IDM_OP_STORE_GLOBAL, binding->slot, binding->name, err, core->span);
        }
    } else if (ok) {
        if (!core->as.letrec.fill_only) {
            uint32_t nil_const = 0;
            if (!idm_bc_add_const(module, idm_nil(), &nil_const)) ok = idm_error_oom(err, core->span);
            for (size_t i = 0; ok && i < count; i++) {
                if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, nil_const, NULL) ||
                    !idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) ok = idm_error_oom(err, core->span);
                else ok = emit_named_u32(module, IDM_OP_STORE_LOCAL, core->as.letrec.bindings[i].slot, core->as.letrec.bindings[i].name, err, core->span);
            }
        }
        for (size_t i = 0; ok && i < count; i++) {
            if (!emit_named_u32(module, IDM_OP_LOAD_LOCAL, core->as.letrec.bindings[i].slot, core->as.letrec.bindings[i].name, err, core->span)) ok = false;
            else if (!compile_expr(core->as.letrec.bindings[i].value, module, false, ctx, err)) ok = false;
            else if (!idm_bc_emit_op(module, IDM_OP_STORE_CELL, NULL)) ok = idm_error_oom(err, core->span);
        }
    }

    size_t jump_over_op = 0;
    if (ok && compiled_count != 0 && !idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_over_op)) ok = idm_error_oom(err, core->span);
    for (size_t i = 0; ok && i < count; i++) {
        if (compiled[i]) ok = compile_callable_bodies(core->as.letrec.bindings[i].value, &callables[i], module, body_ctx, err, core->as.letrec.bindings[i].value->span);
    }
    if (ok && compiled_count != 0) ok = patch_here(module, jump_over_op + 1u, err, core->span, "letrec callable body jump");
    if (ok) ok = compile_expr(core->as.letrec.body, module, tail, body_ctx, err);

    destroy_compiled_callables(callables, compiled, count);
    free(callables);
    free(compiled);
    free(known);
    return ok;
}

static bool emit_store_raised_cell(IdmBytecodeModule *module, const char *name, uint32_t slot, IdmError *err, IdmSpan span) {
    if (!idm_bc_emit_op(module, IDM_OP_LOAD_RAISED, NULL)) return idm_error_oom(err, span);
    if (!idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, span);
    return emit_named_u32(module, IDM_OP_STORE_LOCAL, slot, name, err, span);
}

static bool emit_load_celled_local(IdmBytecodeModule *module, const char *name, uint32_t slot, IdmError *err, IdmSpan span) {
    if (!emit_named_u32(module, IDM_OP_LOAD_LOCAL, slot, name, err, span)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, span);
    return true;
}

static bool compile_guard_rescue_only(IdmCore *core, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err) {
    size_t push_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_done_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(module, push_op + 1u, err, core->span, "guard rescue catch target")) return false;
    if (!emit_store_raised_cell(module, "_rescue", core->as.guard.rescue_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.handler, module, false, ctx, err)) return false;
    return patch_here(module, jump_done_op + 1u, err, core->span, "guard rescue done jump");
}

static bool compile_guard_ensure_only(IdmCore *core, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err) {
    size_t push_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.cleanup, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_done_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(module, push_op + 1u, err, core->span, "guard ensure catch target")) return false;
    if (!emit_store_raised_cell(module, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.cleanup, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
    if (!emit_load_celled_local(module, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RAISE, NULL)) return idm_error_oom(err, core->span);
    return patch_here(module, jump_done_op + 1u, err, core->span, "guard ensure done jump");
}

static bool compile_guard_rescue_ensure(IdmCore *core, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err) {
    size_t outer_push_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &outer_push_op)) return idm_error_oom(err, core->span);
    size_t inner_push_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_RESCUE_PUSH, 0, &inner_push_op)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.body, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_after_rescue_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_after_rescue_op)) return idm_error_oom(err, core->span);
    if (!patch_here(module, inner_push_op + 1u, err, core->span, "guard rescue catch target")) return false;
    if (!emit_store_raised_cell(module, "_rescue", core->as.guard.rescue_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.handler, module, false, ctx, err)) return false;
    if (!patch_here(module, jump_after_rescue_op + 1u, err, core->span, "guard rescue done jump")) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RESCUE_POP, NULL)) return idm_error_oom(err, core->span);
    if (!compile_expr(core->as.guard.cleanup, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
    size_t jump_done_op = 0;
    if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
    if (!patch_here(module, outer_push_op + 1u, err, core->span, "guard ensure catch target")) return false;
    if (!emit_store_raised_cell(module, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!compile_expr(core->as.guard.cleanup, module, false, ctx, err)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
    if (!emit_load_celled_local(module, "_ensure", core->as.guard.ensure_slot, err, core->span)) return false;
    if (!idm_bc_emit_op(module, IDM_OP_RAISE, NULL)) return idm_error_oom(err, core->span);
    return patch_here(module, jump_done_op + 1u, err, core->span, "guard ensure done jump");
}

static bool compile_guard(IdmCore *core, IdmBytecodeModule *module, const CompileContext *ctx, IdmError *err) {
    bool has_handler = core->as.guard.handler != NULL;
    bool has_cleanup = core->as.guard.cleanup != NULL;
    if (!core->as.guard.body || (!has_handler && !has_cleanup)) return idm_error_set(err, core->span, "guard requires a body and handler or cleanup");
    if (has_handler && has_cleanup) return compile_guard_rescue_ensure(core, module, ctx, err);
    if (has_handler) return compile_guard_rescue_only(core, module, ctx, err);
    return compile_guard_ensure_only(core, module, ctx, err);
}

static bool compile_expr(IdmCore *core, IdmBytecodeModule *module, bool tail, const CompileContext *ctx, IdmError *err) {
    if (!idm_bc_note_span(module, core->span)) return idm_error_oom(err, core->span);
    if (!core) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null core expression");
    switch (core->kind) {
        case IDM_CORE_LITERAL: {
            return emit_load_value(module, core->as.literal, err, core->span);
        }
        case IDM_CORE_LOCAL_REF:
            if (!emit_named_u32(module, IDM_OP_LOAD_LOCAL, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span)) return false;
            if (core->local_celled && !idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_ARG_REF:
            return emit_named_u32(module, IDM_OP_LOAD_ARG, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span);
        case IDM_CORE_CAPTURE_REF:
            if (!emit_named_u32(module, IDM_OP_LOAD_CAPTURE, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span)) return false;
            if (core->local_celled && !idm_bc_emit_op(module, IDM_OP_LOAD_CELL, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_GLOBAL_REF:
            return emit_named_u32(module, IDM_OP_LOAD_GLOBAL, core->as.slot_ref.slot, core->as.slot_ref.name, err, core->span);
        case IDM_CORE_PRIMITIVE: {
            uint32_t index = 0;
            if (!idm_bc_add_const(module, idm_primitive_value(core->as.primitive), &index)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, index, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
        case IDM_CORE_CALL:
            if (core->as.call.callee && core->as.call.callee->kind == IDM_CORE_PRIMITIVE) {
                for (size_t i = 0; i < core->as.call.arg_count; i++) {
                    if (!compile_expr(core->as.call.args[i], module, false, ctx, err)) return false;
                }
                if (core->as.call.callee->as.primitive == IDM_PRIM_EXIT && core->as.call.arg_count == 0) {
                    uint32_t zero_const = 0;
                    if (!add_const_value(module, idm_int(0), &zero_const, err, core->span)) return false;
                    if (!idm_bc_emit_u32(module, IDM_OP_LOAD_CONST, zero_const, NULL)) return idm_error_oom(err, core->span);
                    if (!idm_bc_emit_op(module, IDM_OP_EXIT, NULL)) return idm_error_oom(err, core->span);
                    return true;
                }
                if (core->as.call.callee->as.primitive == IDM_PRIM_RAISE) {
                    if (core->as.call.arg_count != 1u) return idm_error_set(err, core->span, "primitive 'raise' expects 1 argument(s)");
                    if (!idm_bc_emit_op(module, IDM_OP_RAISE, NULL)) return idm_error_oom(err, core->span);
                    return true;
                }
                IdmOpcode actor_op = IDM_OP_HALT;
                uint32_t actor_arity = 0;
                if (actor_primitive_opcode(core->as.call.callee->as.primitive, core->as.call.arg_count, &actor_op, &actor_arity)) {
                    if (core->as.call.arg_count != actor_arity) return idm_error_set(err, core->span, "primitive '%s' expects %u argument(s)", idm_primitive_name(core->as.call.callee->as.primitive), actor_arity);
                    if (!idm_bc_emit_op(module, actor_op, NULL)) return idm_error_oom(err, core->span);
                    return true;
                }
                if (core->as.call.arg_count > UINT32_MAX) return idm_error_set(err, core->span, "too many primitive arguments");
                if (!idm_bc_emit_u32(module, IDM_OP_PRIM_CALL, (uint32_t)core->as.call.callee->as.primitive, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, (uint32_t)core->as.call.arg_count, NULL)) return idm_error_oom(err, core->span);
                return true;
            }
            if (core->as.call.callee && (core->as.call.callee->kind == IDM_CORE_FN || core->as.call.callee->kind == IDM_CORE_FN_MULTI)) {
                return compile_direct_callable_call(core->as.call.callee, core->as.call.args, core->as.call.arg_count, module, tail, ctx, err, core->span);
            }
            const CompiledCallable *known = known_callable_for_ref(ctx, core->as.call.callee);
            if (known) return emit_direct_callable_call(known, core->as.call.args, core->as.call.arg_count, module, tail, ctx, err, core->span);
            if (!compile_expr(core->as.call.callee, module, false, ctx, err)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                if (!compile_expr(core->as.call.args[i], module, false, ctx, err)) return false;
            }
            if (core->as.call.arg_count > IDM_CALL_ARGC_MASK) return idm_error_set(err, core->span, "too many call arguments");
            if (!idm_bc_emit_u32(module, tail ? IDM_OP_TAIL_CALL : IDM_OP_CALL, (uint32_t)core->as.call.arg_count, NULL)) return idm_error_oom(err, core->span);
            return true;
        case IDM_CORE_COND: {
            if (!compile_expr(core->as.cond_expr.cond, module, false, ctx, err)) return false;
            size_t jump_false_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP_IF_FALSE, 0, &jump_false_op)) return idm_error_oom(err, core->span);
            if (!compile_expr(core->as.cond_expr.then_branch, module, tail, ctx, err)) return false;
            size_t jump_end_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_end_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_false_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch cond false jump");
            if (!compile_expr(core->as.cond_expr.else_branch, module, tail, ctx, err)) return false;
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
                if (!compile_expr(core->as.do_expr.items[i], module, i + 1u == core->as.do_expr.count ? tail : false, ctx, err)) return false;
                if (i + 1u < core->as.do_expr.count && !idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        case IDM_CORE_BIND_LOCAL:
            if (!compile_expr(core->as.bind_local.value, module, false, ctx, err)) return false;
            if (core->local_celled && !idm_bc_emit_op(module, IDM_OP_MAKE_CELL, NULL)) return idm_error_oom(err, core->span);
            if (!emit_named_u32(module, IDM_OP_STORE_LOCAL, core->as.bind_local.slot, core->as.bind_local.name, err, core->span)) return false;
            return compile_expr(core->as.bind_local.body, module, tail, ctx, err);
        case IDM_CORE_FN: {
            return compile_callable_literal(core, module, ctx, err);
        }
        case IDM_CORE_FN_MULTI: {
            return compile_callable_literal(core, module, ctx, err);
        }
        case IDM_CORE_LETREC: {
            return compile_letrec(core, module, tail, ctx, err);
        }
        case IDM_CORE_RECEIVE: {
            if (!core->as.receive.receiver || !core->as.receive.timeout || !core->as.receive.timeout_body) return idm_error_set(err, core->span, "receive is missing a required component");
            if (!compile_expr(core->as.receive.timeout, module, false, ctx, err)) return false;
            if (!compile_expr(core->as.receive.receiver, module, false, ctx, err)) return false;
            size_t recv_op = 0;
            if (!idm_bc_emit_u32(module, tail ? IDM_OP_TAIL_RECV : IDM_OP_RECV, 0, &recv_op)) return idm_error_oom(err, core->span);
            size_t jump_done_op = 0;
            if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &jump_done_op)) return idm_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, recv_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch receive timeout target");
            if (!compile_expr(core->as.receive.timeout_body, module, tail, ctx, err)) return false;
            if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
            if (!idm_bc_patch_u32(module, jump_done_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch receive done jump");
            return true;
        }
        case IDM_CORE_GUARD:
            return compile_guard(core, module, ctx, err);
        case IDM_CORE_USE_PACKAGE: {
            uint32_t fn_off = 0;
            if (core->as.use_package.module) {
                size_t skip_op = 0;
                if (!idm_bc_emit_u32(module, IDM_OP_JUMP, 0, &skip_op)) return idm_error_oom(err, core->span);
                uint32_t const_off = 0;
                uint32_t code_off = 0;
                if (!idm_bc_link(module, core->as.use_package.module, &const_off, &fn_off, &code_off, err)) return false;
                if (module->code_count > UINT32_MAX) return idm_error_set(err, core->span, "bytecode too large");
                if (!idm_bc_patch_u32(module, skip_op + 1u, (uint32_t)module->code_count)) return idm_error_set(err, core->span, "failed to patch package skip jump");
            }
            uint32_t name_index = 0;
            if (!idm_bc_add_const(module, core->as.use_package.name, &name_index)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit_u32(module, IDM_OP_ENTER_NAMESPACE, name_index, NULL)) return idm_error_oom(err, core->span);
            if (core->as.use_package.module) {
                if (!idm_bc_emit_u32(module, IDM_OP_MAKE_CLOSURE, fn_off + core->as.use_package.init_fn, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit_u32(module, IDM_OP_CALL, 0u, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit_op(module, IDM_OP_POP, NULL)) return idm_error_oom(err, core->span);
            }
            for (size_t i = 0; i < core->as.use_package.export_count; i++) {
                if (!emit_named_u32(module, IDM_OP_IMPORT_GLOBAL, core->as.use_package.export_src[i], core->as.use_package.export_names[i], err, core->span)) return false;
                if (!idm_bc_emit(module, core->as.use_package.export_dst[i], NULL)) return idm_error_oom(err, core->span);
            }
            if (!idm_bc_emit_op(module, IDM_OP_LEAVE_NAMESPACE, NULL)) return idm_error_oom(err, core->span);
            return compile_expr(core->as.use_package.cont, module, tail, ctx, err);
        }
        case IDM_CORE_DEFINE_TRAIT: {
            if (core->as.define_trait.requirement_count > UINT32_MAX) return idm_error_set(err, core->span, "trait has too many requirements");
            if (core->as.define_trait.count > UINT32_MAX) return idm_error_set(err, core->span, "trait has too many methods");
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                if (method->has_default) {
                    if (!compile_expr(method->default_fn, module, false, ctx, err)) return false;
                } else {
                    if (!emit_load_value(module, idm_nil(), err, core->span)) return false;
                }
            }
            uint32_t trait_const = 0;
            if (!add_const_value(module, core->as.define_trait.name, &trait_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, IDM_OP_DEFINE_TRAIT, trait_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.define_trait.requirement_count, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.define_trait.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.define_trait.requirement_count; i++) {
                uint32_t requirement_const = 0;
                if (!add_const_value(module, core->as.define_trait.requirements[i].name, &requirement_const, err, core->span)) return false;
                if (!idm_bc_emit(module, requirement_const, NULL)) return idm_error_oom(err, core->span);
            }
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                uint32_t method_const = 0;
                if (!add_const_value(module, method->name, &method_const, err, core->span)) return false;
                if (!idm_bc_emit(module, method_const, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, method->arity, NULL)) return idm_error_oom(err, core->span);
                if (!idm_bc_emit(module, method->has_default ? 1u : 0u, NULL)) return idm_error_oom(err, core->span);
            }
            return true;
        }
        case IDM_CORE_IMPLEMENT_TRAIT: {
            if (core->as.implement_trait.count > UINT32_MAX) return idm_error_set(err, core->span, "implement has too many methods");
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                if (!compile_expr(core->as.implement_trait.impls[i].impl_fn, module, false, ctx, err)) return false;
            }
            uint32_t trait_const = 0;
            uint32_t type_const = 0;
            uint32_t provider_const = 0;
            uint32_t provider_key_const = 0;
            if (!add_const_value(module, core->as.implement_trait.trait, &trait_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.implement_trait.type, &type_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.implement_trait.provider, &provider_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.implement_trait.provider_key, &provider_key_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, IDM_OP_IMPLEMENT_TRAIT, trait_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, type_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, provider_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, provider_key_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.implement_trait.count, NULL)) return idm_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.implement_trait.count; i++) {
                IdmCoreTraitImpl *impl = &core->as.implement_trait.impls[i];
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
                if (!compile_expr(core->as.method_call.args[i], module, false, ctx, err)) return false;
            }
            uint32_t trait_const = 0;
            uint32_t method_const = 0;
            if (!add_const_value(module, core->as.method_call.trait, &trait_const, err, core->span)) return false;
            if (!add_const_value(module, core->as.method_call.method, &method_const, err, core->span)) return false;
            if (!idm_bc_emit_u32(module, tail ? IDM_OP_TAIL_CALL_METHOD : IDM_OP_CALL_METHOD, trait_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, method_const, NULL)) return idm_error_oom(err, core->span);
            if (!idm_bc_emit(module, (uint32_t)core->as.method_call.arg_count, NULL)) return idm_error_oom(err, core->span);
            return true;
        }
    }
    return idm_error_set(err, core->span, "unknown core node");
}

bool idm_core_compile_expression(IdmCore *core, IdmBytecodeModule *module, IdmError *err) {
    return compile_expr(core, module, false, NULL, err);
}

bool idm_core_compile_function_body(IdmCore *body, const char *name, uint32_t arity, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    if (!body) return idm_error_set(err, idm_span_unknown(NULL), "cannot compile null function body");
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(body);
    if (!idm_bc_add_function(module, name ? name : "<function>", arity, locals, 0, &fn)) return idm_error_oom(err, body->span);
    if (!compile_function_code(module, fn, body, NULL, err, body->span, NULL, 0)) return false;
    if (out_function) *out_function = fn;
    return true;
}

bool idm_core_compile_main(IdmCore *core, IdmBytecodeModule *module, uint32_t *out_function, IdmError *err) {
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!idm_bc_add_function(module, "main", 0, locals, 0, &fn)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!compile_function_code(module, fn, core, NULL, err, idm_span_unknown(NULL), NULL, 0)) return false;
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
            return idm_buf_appendf(buf, "(arg %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_LOCAL_REF:
            return idm_buf_appendf(buf, "(local %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_CAPTURE_REF:
            return idm_buf_appendf(buf, "(capture %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_PRIMITIVE:
            return idm_buf_appendf(buf, "(prim %s)", idm_primitive_name(core->as.primitive));
        case IDM_CORE_CALL:
            if (!idm_buf_append_char(buf, '(')) return false;
            if (!idm_core_dump(buf, core->as.call.callee)) return false;
            for (size_t i = 0; i < core->as.call.arg_count; i++) {
                if (!idm_buf_append_char(buf, ' ')) return false;
                if (!idm_core_dump(buf, core->as.call.args[i])) return false;
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
        case IDM_CORE_GLOBAL_REF:
            return idm_buf_appendf(buf, "(global %s#%u)", core->as.slot_ref.name, core->as.slot_ref.slot);
        case IDM_CORE_USE_PACKAGE:
            if (!idm_buf_append(buf, "(use-package ") || !dump_value(buf, core->as.use_package.name) ||
                !idm_buf_appendf(buf, " init=%u exports=[", core->as.use_package.init_fn)) {
                return false;
            }
            for (size_t i = 0; i < core->as.use_package.export_count; i++) {
                if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
                if (!idm_buf_appendf(buf, "%s#%u->%s#%u", core->as.use_package.export_names[i], core->as.use_package.export_src[i], core->as.use_package.export_names[i], core->as.use_package.export_dst[i])) return false;
            }
            return idm_buf_append(buf, "] ") &&
                   idm_core_dump(buf, core->as.use_package.cont) &&
                   idm_buf_append_char(buf, ')');
        case IDM_CORE_DEFINE_TRAIT:
            if (!idm_buf_append(buf, "(define-trait ") || !dump_value(buf, core->as.define_trait.name)) return false;
            for (size_t i = 0; i < core->as.define_trait.requirement_count; i++) {
                if (!idm_buf_append(buf, " (require ") || !dump_value(buf, core->as.define_trait.requirements[i].name) || !idm_buf_append_char(buf, ')')) return false;
            }
            for (size_t i = 0; i < core->as.define_trait.count; i++) {
                const IdmCoreTraitMethod *method = &core->as.define_trait.methods[i];
                if (!idm_buf_append(buf, " (method ") || !dump_value(buf, method->name) || !idm_buf_appendf(buf, "/%u", method->arity)) return false;
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
                if (!idm_buf_append(buf, " (impl ") || !dump_value(buf, impl->name) || !idm_buf_appendf(buf, "/%u ", impl->arity) || !idm_core_dump(buf, impl->impl_fn) || !idm_buf_append_char(buf, ')')) return false;
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
