#include "ish/core.h"

#include <stdlib.h>

static IshCore *core_alloc(IshCoreKind kind, IshSpan span) {
    IshCore *core = calloc(1u, sizeof(*core));
    if (!core) return NULL;
    core->kind = kind;
    core->span = span;
    return core;
}

IshCore *ish_core_literal(IshValue value, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_LITERAL, span);
    if (!core) return NULL;
    core->as.literal = value;
    return core;
}

IshCore *ish_core_arg_ref(uint32_t slot, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_ARG_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IshCore *ish_core_local_ref(uint32_t slot, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_LOCAL_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IshCore *ish_core_capture_ref(uint32_t slot, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_CAPTURE_REF, span);
    if (!core) return NULL;
    core->as.local_slot = slot;
    return core;
}

IshCore *ish_core_primitive(IshPrimitive primitive, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_PRIMITIVE, span);
    if (!core) return NULL;
    core->as.primitive = primitive;
    return core;
}

IshCore *ish_core_app(IshCore *callee, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_APP, span);
    if (!core) return NULL;
    core->as.app.callee = callee;
    return core;
}

bool ish_core_app_add_arg(IshCore *app, IshCore *arg) {
    if (!app || app->kind != ISH_CORE_APP || !arg) return false;
    if (app->as.app.arg_count == app->as.app.arg_cap) {
        size_t cap = app->as.app.arg_cap ? app->as.app.arg_cap * 2u : 4u;
        IshCore **args = realloc(app->as.app.args, cap * sizeof(*args));
        if (!args) return false;
        app->as.app.args = args;
        app->as.app.arg_cap = cap;
    }
    app->as.app.args[app->as.app.arg_count++] = arg;
    return true;
}

IshCore *ish_core_cond(IshCore *cond, IshCore *then_branch, IshCore *else_branch, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_COND, span);
    if (!core) return NULL;
    core->as.cond_expr.cond = cond;
    core->as.cond_expr.then_branch = then_branch;
    core->as.cond_expr.else_branch = else_branch;
    return core;
}

IshCore *ish_core_do(IshSpan span) {
    return core_alloc(ISH_CORE_DO, span);
}

bool ish_core_do_add(IshCore *do_expr, IshCore *item) {
    if (!do_expr || do_expr->kind != ISH_CORE_DO || !item) return false;
    if (do_expr->as.do_expr.count == do_expr->as.do_expr.cap) {
        size_t cap = do_expr->as.do_expr.cap ? do_expr->as.do_expr.cap * 2u : 4u;
        IshCore **items = realloc(do_expr->as.do_expr.items, cap * sizeof(*items));
        if (!items) return false;
        do_expr->as.do_expr.items = items;
        do_expr->as.do_expr.cap = cap;
    }
    do_expr->as.do_expr.items[do_expr->as.do_expr.count++] = item;
    return true;
}

IshCore *ish_core_bind_local(uint32_t slot, IshCore *value, IshCore *body, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_BIND_LOCAL, span);
    if (!core) return NULL;
    core->as.bind_local.slot = slot;
    core->as.bind_local.value = value;
    core->as.bind_local.body = body;
    return core;
}

IshCore *ish_core_fn(const char *name, uint32_t arity, IshCore *body, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_FN, span);
    if (!core) return NULL;
    core->as.fn.name = ish_strdup(name ? name : "<lambda>");
    if (!core->as.fn.name) { free(core); return NULL; }
    core->as.fn.arity = arity;
    core->as.fn.body = body;
    return core;
}

bool ish_core_fn_add_capture(IshCore *fn, uint32_t local_slot) {
    if (!fn || fn->kind != ISH_CORE_FN) return false;
    if (fn->as.fn.capture_count == fn->as.fn.capture_cap) {
        size_t cap = fn->as.fn.capture_cap ? fn->as.fn.capture_cap * 2u : 4u;
        uint32_t *slots = realloc(fn->as.fn.capture_slots, cap * sizeof(*slots));
        if (!slots) return false;
        fn->as.fn.capture_slots = slots;
        fn->as.fn.capture_cap = cap;
    }
    fn->as.fn.capture_slots[fn->as.fn.capture_count++] = local_slot;
    return true;
}

bool ish_core_fn_set_param_patterns_take(IshCore *fn, IshPattern **patterns, uint32_t pattern_count) {
    if (!fn || fn->kind != ISH_CORE_FN) return false;
    for (uint32_t i = 0; i < fn->as.fn.pattern_count; i++) ish_pat_free(fn->as.fn.param_patterns[i]);
    free(fn->as.fn.param_patterns);
    fn->as.fn.param_patterns = patterns;
    fn->as.fn.pattern_count = pattern_count;
    return true;
}

static void pattern_locals_free(IshPatternLocal *locals, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free(locals[i].name);
    free(locals);
}

bool ish_core_fn_set_pattern_locals_take(IshCore *fn, IshPatternLocal *locals, uint32_t local_count) {
    if (!fn || fn->kind != ISH_CORE_FN) return false;
    pattern_locals_free(fn->as.fn.pattern_locals, fn->as.fn.pattern_local_count);
    fn->as.fn.pattern_locals = locals;
    fn->as.fn.pattern_local_count = local_count;
    return true;
}

bool ish_core_fn_set_guard_take(IshCore *fn, IshCore *guard) {
    if (!fn || fn->kind != ISH_CORE_FN) return false;
    ish_core_free(fn->as.fn.guard);
    fn->as.fn.guard = guard;
    return true;
}

IshCore *ish_core_fn_multi(const char *name, IshSpan span) {
    IshCore *core = core_alloc(ISH_CORE_FN_MULTI, span);
    if (!core) return NULL;
    core->as.fn_multi.name = ish_strdup(name ? name : "<multi>");
    if (!core->as.fn_multi.name) { free(core); return NULL; }
    return core;
}

bool ish_core_fn_multi_add_capture(IshCore *multi, uint32_t local_slot) {
    if (!multi || multi->kind != ISH_CORE_FN_MULTI) return false;
    for (size_t i = 0; i < multi->as.fn_multi.capture_count; i++) {
        if (multi->as.fn_multi.capture_slots[i] == local_slot) return true;
    }
    if (multi->as.fn_multi.capture_count == multi->as.fn_multi.capture_cap) {
        size_t cap = multi->as.fn_multi.capture_cap ? multi->as.fn_multi.capture_cap * 2u : 4u;
        uint32_t *slots = realloc(multi->as.fn_multi.capture_slots, cap * sizeof(*slots));
        if (!slots) return false;
        multi->as.fn_multi.capture_slots = slots;
        multi->as.fn_multi.capture_cap = cap;
    }
    multi->as.fn_multi.capture_slots[multi->as.fn_multi.capture_count++] = local_slot;
    return true;
}

bool ish_core_fn_multi_add_clause_take(IshCore *multi, uint32_t arity, IshPattern **patterns, uint32_t pattern_count, IshPatternLocal *locals, uint32_t local_count, IshCore *guard, IshCore *body) {
    if (!multi || multi->kind != ISH_CORE_FN_MULTI || !body) return false;
    if (multi->as.fn_multi.count == multi->as.fn_multi.cap) {
        size_t cap = multi->as.fn_multi.cap ? multi->as.fn_multi.cap * 2u : 4u;
        IshFnClause *clauses = realloc(multi->as.fn_multi.clauses, cap * sizeof(*clauses));
        if (!clauses) return false;
        multi->as.fn_multi.clauses = clauses;
        multi->as.fn_multi.cap = cap;
    }
    IshFnClause *clause = &multi->as.fn_multi.clauses[multi->as.fn_multi.count++];
    clause->arity = arity;
    clause->param_patterns = patterns;
    clause->pattern_count = pattern_count;
    clause->pattern_locals = locals;
    clause->pattern_local_count = local_count;
    clause->guard = guard;
    clause->body = body;
    return true;
}

IshCore *ish_core_letrec(IshSpan span) {
    return core_alloc(ISH_CORE_LETREC, span);
}

bool ish_core_letrec_add(IshCore *letrec, const char *name, uint32_t slot, IshCore *value) {
    if (!letrec || letrec->kind != ISH_CORE_LETREC || !value) return false;
    if (letrec->as.letrec.count == letrec->as.letrec.cap) {
        size_t cap = letrec->as.letrec.cap ? letrec->as.letrec.cap * 2u : 4u;
        IshLetRecBinding *bindings = realloc(letrec->as.letrec.bindings, cap * sizeof(*bindings));
        if (!bindings) return false;
        letrec->as.letrec.bindings = bindings;
        letrec->as.letrec.cap = cap;
    }
    IshLetRecBinding *binding = &letrec->as.letrec.bindings[letrec->as.letrec.count];
    binding->name = ish_strdup(name);
    if (!binding->name) return false;
    binding->slot = slot;
    binding->value = value;
    letrec->as.letrec.count++;
    return true;
}

bool ish_core_letrec_set_body(IshCore *letrec, IshCore *body) {
    if (!letrec || letrec->kind != ISH_CORE_LETREC || !body) return false;
    letrec->as.letrec.body = body;
    return true;
}

void ish_core_free(IshCore *core) {
    if (!core) return;
    switch (core->kind) {
        case ISH_CORE_APP:
            ish_core_free(core->as.app.callee);
            for (size_t i = 0; i < core->as.app.arg_count; i++) ish_core_free(core->as.app.args[i]);
            free(core->as.app.args);
            break;
        case ISH_CORE_COND:
            ish_core_free(core->as.cond_expr.cond);
            ish_core_free(core->as.cond_expr.then_branch);
            ish_core_free(core->as.cond_expr.else_branch);
            break;
        case ISH_CORE_DO:
            for (size_t i = 0; i < core->as.do_expr.count; i++) ish_core_free(core->as.do_expr.items[i]);
            free(core->as.do_expr.items);
            break;
        case ISH_CORE_BIND_LOCAL:
            ish_core_free(core->as.bind_local.value);
            ish_core_free(core->as.bind_local.body);
            break;
        case ISH_CORE_FN:
            free(core->as.fn.name);
            free(core->as.fn.capture_slots);
            for (uint32_t i = 0; i < core->as.fn.pattern_count; i++) ish_pat_free(core->as.fn.param_patterns[i]);
            free(core->as.fn.param_patterns);
            pattern_locals_free(core->as.fn.pattern_locals, core->as.fn.pattern_local_count);
            ish_core_free(core->as.fn.guard);
            ish_core_free(core->as.fn.body);
            break;
        case ISH_CORE_FN_MULTI:
            free(core->as.fn_multi.name);
            free(core->as.fn_multi.capture_slots);
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                for (uint32_t p = 0; p < core->as.fn_multi.clauses[i].pattern_count; p++) ish_pat_free(core->as.fn_multi.clauses[i].param_patterns[p]);
                free(core->as.fn_multi.clauses[i].param_patterns);
                pattern_locals_free(core->as.fn_multi.clauses[i].pattern_locals, core->as.fn_multi.clauses[i].pattern_local_count);
                ish_core_free(core->as.fn_multi.clauses[i].guard);
                ish_core_free(core->as.fn_multi.clauses[i].body);
            }
            free(core->as.fn_multi.clauses);
            break;
        case ISH_CORE_LETREC:
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                free(core->as.letrec.bindings[i].name);
                ish_core_free(core->as.letrec.bindings[i].value);
            }
            free(core->as.letrec.bindings);
            ish_core_free(core->as.letrec.body);
            break;
        case ISH_CORE_LITERAL:
        case ISH_CORE_ARG_REF:
        case ISH_CORE_LOCAL_REF:
        case ISH_CORE_CAPTURE_REF:
        case ISH_CORE_PRIMITIVE:
            break;
    }
    free(core);
}

static bool compile_expr(IshCore *core, IshBytecodeModule *module, IshError *err);

static uint32_t core_max_local_plus_one(IshCore *core) {
    if (!core) return 0;
    switch (core->kind) {
        case ISH_CORE_LOCAL_REF:
            return core->as.local_slot + 1u;
        case ISH_CORE_ARG_REF:
            return 0;
        case ISH_CORE_CAPTURE_REF:
            return 0;
        case ISH_CORE_APP: {
            uint32_t max = core_max_local_plus_one(core->as.app.callee);
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.app.args[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case ISH_CORE_COND: {
            uint32_t max = core_max_local_plus_one(core->as.cond_expr.cond);
            uint32_t then_max = core_max_local_plus_one(core->as.cond_expr.then_branch);
            uint32_t else_max = core_max_local_plus_one(core->as.cond_expr.else_branch);
            if (then_max > max) max = then_max;
            if (else_max > max) max = else_max;
            return max;
        }
        case ISH_CORE_DO: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                uint32_t child = core_max_local_plus_one(core->as.do_expr.items[i]);
                if (child > max) max = child;
            }
            return max;
        }
        case ISH_CORE_BIND_LOCAL: {
            uint32_t max = core->as.bind_local.slot + 1u;
            uint32_t value_max = core_max_local_plus_one(core->as.bind_local.value);
            uint32_t body_max = core_max_local_plus_one(core->as.bind_local.body);
            if (value_max > max) max = value_max;
            if (body_max > max) max = body_max;
            return max;
        }
        case ISH_CORE_FN: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                uint32_t child = core->as.fn.capture_slots[i] + 1u;
                if (child > max) max = child;
            }
            return max;
        }
        case ISH_CORE_FN_MULTI: {
            uint32_t max = 0;
            for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                uint32_t child = core->as.fn_multi.capture_slots[i] + 1u;
                if (child > max) max = child;
            }
            return max;
        }
        case ISH_CORE_LETREC: {
            uint32_t max = core_max_local_plus_one(core->as.letrec.body);
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                uint32_t slot = core->as.letrec.bindings[i].slot + 1u;
                uint32_t value_max = core_max_local_plus_one(core->as.letrec.bindings[i].value);
                if (slot > max) max = slot;
                if (value_max > max) max = value_max;
            }
            return max;
        }
        case ISH_CORE_LITERAL:
        case ISH_CORE_PRIMITIVE:
            return 0;
    }
    return 0;
}

static IshOpcode primitive_opcode(IshPrimitive primitive) {
    switch (primitive) {
        case ISH_PRIM_ADD: return ISH_OP_ADD;
        case ISH_PRIM_SUB: return ISH_OP_SUB;
        case ISH_PRIM_MUL: return ISH_OP_MUL;
        case ISH_PRIM_EQ: return ISH_OP_EQ;
        case ISH_PRIM_LT: return ISH_OP_LT;
    }
    return ISH_OP_HALT;
}

static bool clone_patterns(IshPattern **patterns, uint32_t count, IshPattern ***out) {
    if (count == 0) {
        *out = NULL;
        return true;
    }
    IshPattern **clones = calloc(count, sizeof(*clones));
    if (!clones) return false;
    for (uint32_t i = 0; i < count; i++) {
        clones[i] = ish_pat_clone(patterns[i]);
        if (!clones[i]) {
            for (uint32_t j = 0; j < i; j++) ish_pat_free(clones[j]);
            free(clones);
            return false;
        }
    }
    *out = clones;
    return true;
}

static bool clone_pattern_locals(IshPatternLocal *locals, uint32_t count, IshPatternLocal **out) {
    if (count == 0) {
        *out = NULL;
        return true;
    }
    IshPatternLocal *clones = calloc(count, sizeof(*clones));
    if (!clones) return false;
    for (uint32_t i = 0; i < count; i++) {
        clones[i].name = ish_strdup(locals[i].name);
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

static uint32_t pattern_local_max_plus_one(IshPatternLocal *locals, uint32_t count) {
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
    IshBuffer buf;
    ish_buf_init(&buf);
    if (!ish_buf_appendf(&buf, "%s#guard", name ? name : "<anonymous>")) {
        ish_buf_destroy(&buf);
        return false;
    }
    *out_name = ish_buf_take(&buf);
    return *out_name != NULL;
}

static bool add_guard_function(IshBytecodeModule *module, const char *name, uint32_t arity, uint32_t locals, uint32_t *out_index, IshError *err, IshSpan span) {
    char *guard_name = NULL;
    if (!make_guard_function_name(name, &guard_name)) return ish_error_oom(err, span);
    bool ok = ish_bc_add_function(module, guard_name, arity, locals, 0, out_index);
    free(guard_name);
    if (!ok) return ish_error_oom(err, span);
    return true;
}

static bool add_compiled_function(IshBytecodeModule *module, const char *name, uint32_t arity, uint32_t locals, IshPattern **patterns, uint32_t pattern_count, IshPatternLocal *pattern_locals, uint32_t pattern_local_count, uint32_t *out_index, IshError *err, IshSpan span) {
    if (!ish_bc_add_function(module, name, arity, locals, 0, out_index)) return ish_error_oom(err, span);
    if (pattern_count != 0) {
        IshPattern **clones = NULL;
        if (!clone_patterns(patterns, pattern_count, &clones)) return ish_error_oom(err, span);
        if (!ish_bc_set_function_patterns_take(module, *out_index, clones, pattern_count)) {
            for (uint32_t i = 0; i < pattern_count; i++) ish_pat_free(clones[i]);
            free(clones);
            return ish_error_oom(err, span);
        }
    }
    if (pattern_local_count != 0) {
        IshPatternLocal *clones = NULL;
        if (!clone_pattern_locals(pattern_locals, pattern_local_count, &clones)) return ish_error_oom(err, span);
        if (!ish_bc_set_function_pattern_locals_take(module, *out_index, clones, pattern_local_count)) {
            for (uint32_t i = 0; i < pattern_local_count; i++) free(clones[i].name);
            free(clones);
            return ish_error_oom(err, span);
        }
    }
    return true;
}

static bool compile_function_code(IshBytecodeModule *module, uint32_t function_index, IshCore *body, IshError *err, IshSpan span) {
    if (!ish_bc_set_function_entry(module, function_index, module->code_count)) return ish_error_set(err, span, "failed to set function entry");
    if (!compile_expr(body, module, err)) return false;
    if (!ish_bc_emit_op(module, ISH_OP_RETURN, NULL)) return ish_error_oom(err, span);
    return true;
}

const char *ish_primitive_name(IshPrimitive primitive) {
    switch (primitive) {
        case ISH_PRIM_ADD: return "add";
        case ISH_PRIM_SUB: return "sub";
        case ISH_PRIM_MUL: return "mul";
        case ISH_PRIM_EQ: return "eq";
        case ISH_PRIM_LT: return "lt";
    }
    return "<bad-primitive>";
}

static bool compile_expr(IshCore *core, IshBytecodeModule *module, IshError *err) {
    if (!core) return ish_error_set(err, ish_span_unknown(NULL), "cannot compile null core expression");
    switch (core->kind) {
        case ISH_CORE_LITERAL: {
            uint32_t index = 0;
            if (!ish_bc_add_const(module, core->as.literal, &index)) return ish_error_oom(err, core->span);
            if (!ish_bc_emit_u32(module, ISH_OP_LOAD_CONST, index, NULL)) return ish_error_oom(err, core->span);
            return true;
        }
        case ISH_CORE_LOCAL_REF:
            if (!ish_bc_emit_u32(module, ISH_OP_LOAD_LOCAL, core->as.local_slot, NULL)) return ish_error_oom(err, core->span);
            if (!ish_bc_emit_op(module, ISH_OP_LOAD_CELL, NULL)) return ish_error_oom(err, core->span);
            return true;
        case ISH_CORE_ARG_REF:
            if (!ish_bc_emit_u32(module, ISH_OP_LOAD_ARG, core->as.local_slot, NULL)) return ish_error_oom(err, core->span);
            return true;
        case ISH_CORE_CAPTURE_REF:
            if (!ish_bc_emit_u32(module, ISH_OP_LOAD_CAPTURE, core->as.local_slot, NULL)) return ish_error_oom(err, core->span);
            if (!ish_bc_emit_op(module, ISH_OP_LOAD_CELL, NULL)) return ish_error_oom(err, core->span);
            return true;
        case ISH_CORE_PRIMITIVE:
            return ish_error_set(err, core->span, "primitive cannot be compiled as a standalone value yet");
        case ISH_CORE_APP:
            if (core->as.app.callee && core->as.app.callee->kind == ISH_CORE_PRIMITIVE) {
                if (core->as.app.arg_count != 2) return ish_error_set(err, core->span, "primitive applications currently require exactly two arguments");
                for (size_t i = 0; i < core->as.app.arg_count; i++) {
                    if (!compile_expr(core->as.app.args[i], module, err)) return false;
                }
                if (!ish_bc_emit_op(module, primitive_opcode(core->as.app.callee->as.primitive), NULL)) return ish_error_oom(err, core->span);
                return true;
            }
            if (!compile_expr(core->as.app.callee, module, err)) return false;
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                if (!compile_expr(core->as.app.args[i], module, err)) return false;
            }
            if (core->as.app.arg_count > UINT32_MAX) return ish_error_set(err, core->span, "too many call arguments");
            if (!ish_bc_emit_u32(module, ISH_OP_CALL, (uint32_t)core->as.app.arg_count, NULL)) return ish_error_oom(err, core->span);
            return true;
        case ISH_CORE_COND: {
            if (!compile_expr(core->as.cond_expr.cond, module, err)) return false;
            size_t jump_false_op = 0;
            if (!ish_bc_emit_u32(module, ISH_OP_JUMP_IF_FALSE, 0, &jump_false_op)) return ish_error_oom(err, core->span);
            if (!compile_expr(core->as.cond_expr.then_branch, module, err)) return false;
            size_t jump_end_op = 0;
            if (!ish_bc_emit_u32(module, ISH_OP_JUMP, 0, &jump_end_op)) return ish_error_oom(err, core->span);
            if (module->code_count > UINT32_MAX) return ish_error_set(err, core->span, "bytecode too large");
            if (!ish_bc_patch_u32(module, jump_false_op + 1u, (uint32_t)module->code_count)) return ish_error_set(err, core->span, "failed to patch cond false jump");
            if (!compile_expr(core->as.cond_expr.else_branch, module, err)) return false;
            if (module->code_count > UINT32_MAX) return ish_error_set(err, core->span, "bytecode too large");
            if (!ish_bc_patch_u32(module, jump_end_op + 1u, (uint32_t)module->code_count)) return ish_error_set(err, core->span, "failed to patch cond end jump");
            return true;
        }
        case ISH_CORE_DO:
            if (core->as.do_expr.count == 0) {
                uint32_t index = 0;
                if (!ish_bc_add_const(module, ish_nil(), &index)) return ish_error_oom(err, core->span);
                if (!ish_bc_emit_u32(module, ISH_OP_LOAD_CONST, index, NULL)) return ish_error_oom(err, core->span);
                return true;
            }
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!compile_expr(core->as.do_expr.items[i], module, err)) return false;
                if (i + 1u < core->as.do_expr.count && !ish_bc_emit_op(module, ISH_OP_POP, NULL)) return ish_error_oom(err, core->span);
            }
            return true;
        case ISH_CORE_BIND_LOCAL:
            if (!compile_expr(core->as.bind_local.value, module, err)) return false;
            if (!ish_bc_emit_op(module, ISH_OP_MAKE_CELL, NULL)) return ish_error_oom(err, core->span);
            if (!ish_bc_emit_u32(module, ISH_OP_STORE_LOCAL, core->as.bind_local.slot, NULL)) return ish_error_oom(err, core->span);
            return compile_expr(core->as.bind_local.body, module, err);
        case ISH_CORE_FN: {
            uint32_t fn_index = 0;
            uint32_t pattern_locals = pattern_local_max_plus_one(core->as.fn.pattern_locals, core->as.fn.pattern_local_count);
            uint32_t locals = max_u32(core_max_local_plus_one(core->as.fn.body), pattern_locals);
            if (!add_compiled_function(module, core->as.fn.name, core->as.fn.arity, locals, core->as.fn.param_patterns, core->as.fn.pattern_count, core->as.fn.pattern_locals, core->as.fn.pattern_local_count, &fn_index, err, core->span)) return false;
            uint32_t guard_index = UINT32_MAX;
            if (core->as.fn.guard) {
                uint32_t guard_locals = max_u32(core_max_local_plus_one(core->as.fn.guard), pattern_locals);
                if (!add_guard_function(module, core->as.fn.name, core->as.fn.arity, guard_locals, &guard_index, err, core->span)) return false;
                if (!ish_bc_set_function_guard(module, fn_index, guard_index)) return ish_error_set(err, core->span, "failed to attach function guard");
            }
            for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                if (!ish_bc_emit_u32(module, ISH_OP_LOAD_LOCAL, core->as.fn.capture_slots[i], NULL)) return ish_error_oom(err, core->span);
            }
            if (core->as.fn.capture_count == 0) {
                if (!ish_bc_emit_u32(module, ISH_OP_MAKE_CLOSURE, fn_index, NULL)) return ish_error_oom(err, core->span);
            } else {
                if (core->as.fn.capture_count > UINT32_MAX) return ish_error_set(err, core->span, "too many captures");
                if (!ish_bc_emit_u32(module, ISH_OP_MAKE_CLOSURE_CAPTURES, fn_index, NULL)) return ish_error_oom(err, core->span);
                if (!ish_bc_emit(module, (uint32_t)core->as.fn.capture_count, NULL)) return ish_error_oom(err, core->span);
            }
            size_t jump_over_op = 0;
            if (!ish_bc_emit_u32(module, ISH_OP_JUMP, 0, &jump_over_op)) return ish_error_oom(err, core->span);
            if (core->as.fn.guard && !compile_function_code(module, guard_index, core->as.fn.guard, err, core->span)) return false;
            if (!compile_function_code(module, fn_index, core->as.fn.body, err, core->span)) return false;
            if (module->code_count > UINT32_MAX) return ish_error_set(err, core->span, "bytecode too large");
            if (!ish_bc_patch_u32(module, jump_over_op + 1u, (uint32_t)module->code_count)) return ish_error_set(err, core->span, "failed to patch function literal jump");
            return true;
        }
        case ISH_CORE_FN_MULTI: {
            if (core->as.fn_multi.count == 0) return ish_error_set(err, core->span, "multi function has no clauses");
            if (core->as.fn_multi.count > UINT32_MAX || core->as.fn_multi.capture_count > UINT32_MAX) return ish_error_set(err, core->span, "multi function too large");
            uint32_t *entries = calloc(core->as.fn_multi.count, sizeof(*entries));
            if (!entries) return ish_error_oom(err, core->span);
            uint32_t *guard_entries = calloc(core->as.fn_multi.count, sizeof(*guard_entries));
            if (!guard_entries) { free(entries); return ish_error_oom(err, core->span); }
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
                    if (!ish_bc_set_function_guard(module, entries[i], guard_entries[i])) {
                        free(guard_entries);
                        free(entries);
                        return ish_error_set(err, core->span, "failed to attach multi function guard");
                    }
                }
            }
            for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                if (!ish_bc_emit_u32(module, ISH_OP_LOAD_LOCAL, core->as.fn_multi.capture_slots[i], NULL)) { free(guard_entries); free(entries); return ish_error_oom(err, core->span); }
            }
            if (!ish_bc_emit_u32(module, ISH_OP_MAKE_MULTI_CLOSURE, (uint32_t)core->as.fn_multi.count, NULL)) { free(guard_entries); free(entries); return ish_error_oom(err, core->span); }
            if (!ish_bc_emit(module, (uint32_t)core->as.fn_multi.capture_count, NULL)) { free(guard_entries); free(entries); return ish_error_oom(err, core->span); }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!ish_bc_emit(module, entries[i], NULL)) { free(guard_entries); free(entries); return ish_error_oom(err, core->span); }
            }
            size_t jump_over_op = 0;
            if (!ish_bc_emit_u32(module, ISH_OP_JUMP, 0, &jump_over_op)) { free(guard_entries); free(entries); return ish_error_oom(err, core->span); }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (core->as.fn_multi.clauses[i].guard && !compile_function_code(module, guard_entries[i], core->as.fn_multi.clauses[i].guard, err, core->span)) { free(guard_entries); free(entries); return false; }
                if (!compile_function_code(module, entries[i], core->as.fn_multi.clauses[i].body, err, core->span)) { free(guard_entries); free(entries); return false; }
            }
            free(guard_entries);
            free(entries);
            if (module->code_count > UINT32_MAX) return ish_error_set(err, core->span, "bytecode too large");
            if (!ish_bc_patch_u32(module, jump_over_op + 1u, (uint32_t)module->code_count)) return ish_error_set(err, core->span, "failed to patch multi function jump");
            return true;
        }
        case ISH_CORE_LETREC: {
            uint32_t nil_const = 0;
            if (!ish_bc_add_const(module, ish_nil(), &nil_const)) return ish_error_oom(err, core->span);
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!ish_bc_emit_u32(module, ISH_OP_LOAD_CONST, nil_const, NULL)) return ish_error_oom(err, core->span);
                if (!ish_bc_emit_op(module, ISH_OP_MAKE_CELL, NULL)) return ish_error_oom(err, core->span);
                if (!ish_bc_emit_u32(module, ISH_OP_STORE_LOCAL, core->as.letrec.bindings[i].slot, NULL)) return ish_error_oom(err, core->span);
            }
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (!ish_bc_emit_u32(module, ISH_OP_LOAD_LOCAL, core->as.letrec.bindings[i].slot, NULL)) return ish_error_oom(err, core->span);
                if (!compile_expr(core->as.letrec.bindings[i].value, module, err)) return false;
                if (!ish_bc_emit_op(module, ISH_OP_STORE_CELL, NULL)) return ish_error_oom(err, core->span);
            }
            return compile_expr(core->as.letrec.body, module, err);
        }
    }
    return ish_error_set(err, core->span, "unknown core node");
}

bool ish_core_compile_expression(IshCore *core, IshBytecodeModule *module, IshError *err) {
    return compile_expr(core, module, err);
}

bool ish_core_compile_main(IshCore *core, IshBytecodeModule *module, uint32_t *out_function, IshError *err) {
    uint32_t fn = 0;
    uint32_t locals = core_max_local_plus_one(core);
    if (!ish_bc_add_function(module, "main", 0, locals, 0, &fn)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_bc_set_function_entry(module, fn, module->code_count)) return ish_error_set(err, ish_span_unknown(NULL), "failed to set main entry");
    if (!compile_expr(core, module, err)) return false;
    if (!ish_bc_emit_op(module, ISH_OP_RETURN, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (out_function) *out_function = fn;
    return true;
}

static bool dump_value(IshBuffer *buf, IshValue value) {
    return ish_value_write(buf, value);
}

bool ish_core_dump(IshBuffer *buf, const IshCore *core) {
    if (!core) return ish_buf_append(buf, "#<null-core>");
    switch (core->kind) {
        case ISH_CORE_LITERAL:
            return dump_value(buf, core->as.literal);
        case ISH_CORE_ARG_REF:
            return ish_buf_appendf(buf, "(arg %u)", core->as.local_slot);
        case ISH_CORE_LOCAL_REF:
            return ish_buf_appendf(buf, "(local %u)", core->as.local_slot);
        case ISH_CORE_CAPTURE_REF:
            return ish_buf_appendf(buf, "(capture %u)", core->as.local_slot);
        case ISH_CORE_PRIMITIVE:
            return ish_buf_appendf(buf, "(prim %s)", ish_primitive_name(core->as.primitive));
        case ISH_CORE_APP:
            if (!ish_buf_append(buf, "(app ")) return false;
            if (!ish_core_dump(buf, core->as.app.callee)) return false;
            for (size_t i = 0; i < core->as.app.arg_count; i++) {
                if (!ish_buf_append_char(buf, ' ')) return false;
                if (!ish_core_dump(buf, core->as.app.args[i])) return false;
            }
            return ish_buf_append_char(buf, ')');
        case ISH_CORE_COND:
            return ish_buf_append(buf, "(cond ") &&
                   ish_core_dump(buf, core->as.cond_expr.cond) &&
                   ish_buf_append_char(buf, ' ') &&
                   ish_core_dump(buf, core->as.cond_expr.then_branch) &&
                   ish_buf_append_char(buf, ' ') &&
                   ish_core_dump(buf, core->as.cond_expr.else_branch) &&
                   ish_buf_append_char(buf, ')');
        case ISH_CORE_DO:
            if (!ish_buf_append(buf, "(do")) return false;
            for (size_t i = 0; i < core->as.do_expr.count; i++) {
                if (!ish_buf_append_char(buf, ' ')) return false;
                if (!ish_core_dump(buf, core->as.do_expr.items[i])) return false;
            }
            return ish_buf_append_char(buf, ')');
        case ISH_CORE_BIND_LOCAL:
            return ish_buf_appendf(buf, "(bind-local %u ", core->as.bind_local.slot) &&
                   ish_core_dump(buf, core->as.bind_local.value) &&
                   ish_buf_append_char(buf, ' ') &&
                   ish_core_dump(buf, core->as.bind_local.body) &&
                   ish_buf_append_char(buf, ')');
        case ISH_CORE_FN:
            if (!ish_buf_appendf(buf, "(fn %s/%u", core->as.fn.name, core->as.fn.arity)) return false;
            if (core->as.fn.capture_count != 0) {
                if (!ish_buf_append(buf, " captures=")) return false;
                if (!ish_buf_append_char(buf, '[')) return false;
                for (size_t i = 0; i < core->as.fn.capture_count; i++) {
                    if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
                    if (!ish_buf_appendf(buf, "%u", core->as.fn.capture_slots[i])) return false;
                }
                if (!ish_buf_append_char(buf, ']')) return false;
            }
            if (core->as.fn.guard) {
                if (!ish_buf_append(buf, " guard=")) return false;
                if (!ish_core_dump(buf, core->as.fn.guard)) return false;
            }
            return ish_buf_append_char(buf, ' ') && ish_core_dump(buf, core->as.fn.body) && ish_buf_append_char(buf, ')');
        case ISH_CORE_FN_MULTI:
            if (!ish_buf_appendf(buf, "(fn-multi %s", core->as.fn_multi.name)) return false;
            if (core->as.fn_multi.capture_count != 0) {
                if (!ish_buf_append(buf, " captures=")) return false;
                if (!ish_buf_append_char(buf, '[')) return false;
                for (size_t i = 0; i < core->as.fn_multi.capture_count; i++) {
                    if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
                    if (!ish_buf_appendf(buf, "%u", core->as.fn_multi.capture_slots[i])) return false;
                }
                if (!ish_buf_append_char(buf, ']')) return false;
            }
            for (size_t i = 0; i < core->as.fn_multi.count; i++) {
                if (!ish_buf_appendf(buf, " (/%u ", core->as.fn_multi.clauses[i].arity)) return false;
                if (core->as.fn_multi.clauses[i].guard) {
                    if (!ish_buf_append(buf, "guard ")) return false;
                    if (!ish_core_dump(buf, core->as.fn_multi.clauses[i].guard)) return false;
                    if (!ish_buf_append_char(buf, ' ')) return false;
                }
                if (!ish_core_dump(buf, core->as.fn_multi.clauses[i].body)) return false;
                if (!ish_buf_append_char(buf, ')')) return false;
            }
            return ish_buf_append_char(buf, ')');
        case ISH_CORE_LETREC:
            if (!ish_buf_append(buf, "(letrec (")) return false;
            for (size_t i = 0; i < core->as.letrec.count; i++) {
                if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
                if (!ish_buf_appendf(buf, "(%s #%u ", core->as.letrec.bindings[i].name, core->as.letrec.bindings[i].slot)) return false;
                if (!ish_core_dump(buf, core->as.letrec.bindings[i].value)) return false;
                if (!ish_buf_append_char(buf, ')')) return false;
            }
            return ish_buf_append(buf, ") ") && ish_core_dump(buf, core->as.letrec.body) && ish_buf_append_char(buf, ')');
    }
    return false;
}
