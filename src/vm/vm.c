#include "ish/vm.h"

#include "ish/actor.h"
#include "ish/prims.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const IshBytecodeModule *module;
    uint32_t function_index;
    size_t ip;
    size_t base;
    size_t result_base;
    IshValue *locals;
    uint32_t local_count;
    IshValue closure;
    IshNamespace *ns;
} Frame;

typedef struct {
    size_t catch_ip;
    size_t frame_count;
    size_t sp;
} Handler;

struct IshExec {
    IshRuntime *rt;
    const IshBytecodeModule *module;
    IshScheduler *sched;
    IshActor *self;
    IshValue *stack;
    size_t sp;
    size_t stack_cap;
    Frame *frames;
    size_t frame_count;
    size_t frame_cap;
    IshVmLimits limits;
    bool has_port_request;
    IshValue port_request;
    bool has_await;
    IshValue await_port;
    Handler *handlers;
    size_t handler_count;
    size_t handler_cap;
    IshValue raised;
    bool has_raised;
    IshNamespace **ns_save;
    size_t ns_save_count;
    size_t ns_save_cap;
    char *cwd;
    char **env_names;
    char **env_values;
    size_t env_count;
    size_t env_cap;
};

typedef struct IshExec Vm;

IshVmLimits ish_vm_default_limits(void) {
    IshVmLimits limits;
    limits.max_stack = 1024u * 64u;
    limits.max_frames = 1024u;
    return limits;
}

bool ish_vm_truthy(IshValue value) {
    if (value.tag == ISH_VAL_NIL) return false;
    if (value.tag == ISH_VAL_ATOM) {
        const char *text = ish_symbol_text(value.as.symbol);
        if (strcmp(text, "false") == 0 || strcmp(text, "nil") == 0) return false;
    }
    if (value.tag == ISH_VAL_TUPLE && ish_sequence_count(value) > 0) {
        IshError err;
        ish_error_init(&err);
        IshValue tag = ish_sequence_item(value, 0, &err);
        if (err.present) {
            ish_error_clear(&err);
            return true;
        }
        if (tag.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(tag.as.symbol), "error") == 0) return false;
    }
    return true;
}

static void vm_reset(Vm *vm) {
    for (size_t i = 0; i < vm->frame_count; i++) free(vm->frames[i].locals);
    free(vm->frames);
    free(vm->stack);
    free(vm->handlers);
    vm->frames = NULL;
    vm->frame_count = 0;
    vm->frame_cap = 0;
    vm->stack = NULL;
    vm->sp = 0;
    vm->stack_cap = 0;
    vm->handlers = NULL;
    vm->handler_count = 0;
    vm->handler_cap = 0;
    free(vm->ns_save);
    vm->ns_save = NULL;
    vm->ns_save_count = 0;
    vm->ns_save_cap = 0;
}

static bool stack_reserve(Vm *vm, size_t needed, IshError *err) {
    if (needed > vm->limits.max_stack) return ish_error_set(err, ish_span_unknown(NULL), "VM stack limit exceeded");
    if (needed <= vm->stack_cap) return true;
    size_t cap = vm->stack_cap ? vm->stack_cap * 2u : 64u;
    while (cap < needed) cap *= 2u;
    IshValue *next = realloc(vm->stack, cap * sizeof(*next));
    if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
    vm->stack = next;
    vm->stack_cap = cap;
    return true;
}

static bool push(Vm *vm, IshValue value, IshError *err) {
    if (!stack_reserve(vm, vm->sp + 1u, err)) return false;
    vm->stack[vm->sp++] = value;
    return true;
}

static bool pop(Vm *vm, IshValue *out, IshError *err) {
    if (vm->sp == 0) return ish_error_set(err, ish_span_unknown(NULL), "VM stack underflow");
    *out = vm->stack[--vm->sp];
    return true;
}

static Frame *current_frame(Vm *vm) {
    return vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1u];
}

static const IshBytecodeModule *closure_module_or_current(Vm *vm, IshValue closure) {
    const IshBytecodeModule *module = ish_closure_module(closure);
    return module ? module : vm->module;
}

static bool push_frame(Vm *vm, const IshBytecodeModule *module, uint32_t function_index, IshValue closure, size_t base, size_t result_base, IshError *err) {
    if (!module) module = vm->module;
    if (function_index >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (vm->frame_count >= vm->limits.max_frames) return ish_error_set(err, ish_span_unknown(NULL), "VM frame limit exceeded");
    if (vm->frame_count == vm->frame_cap) {
        size_t cap = vm->frame_cap ? vm->frame_cap * 2u : 32u;
        Frame *next = realloc(vm->frames, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        vm->frames = next;
        vm->frame_cap = cap;
    }
    const IshBcFunction *fn = &module->functions[function_index];
    Frame *caller = current_frame(vm);
    IshNamespace *ns = ish_is_closure(closure) ? ish_closure_namespace(closure) : (caller ? caller->ns : vm->rt->main_ns);
    if (!ns) ns = vm->rt->main_ns;
    Frame frame;
    frame.module = module;
    frame.function_index = function_index;
    frame.ip = fn->entry;
    frame.base = base;
    frame.result_base = result_base;
    frame.local_count = fn->local_count;
    frame.closure = closure;
    frame.ns = ns;
    frame.locals = fn->local_count == 0 ? NULL : calloc(fn->local_count, sizeof(*frame.locals));
    if (fn->local_count != 0 && !frame.locals) return ish_error_oom(err, ish_span_unknown(NULL));
    for (uint32_t i = 0; i < fn->local_count; i++) frame.locals[i] = ish_nil();
    vm->frames[vm->frame_count++] = frame;
    return true;
}

static void pop_frame(Vm *vm, Frame *out) {
    *out = vm->frames[--vm->frame_count];
}

static bool checked_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool checked_sub(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
}

static bool checked_mul(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return true; }
    if (a == -1 && b == INT64_MIN) return false;
    if (b == -1 && a == INT64_MIN) return false;
    int64_t r = a * b;
    if (r / b != a) return false;
    *out = r;
    return true;
}

static bool binary_int(Vm *vm, IshOpcode op, IshError *err) {
    IshValue b, a;
    if (!pop(vm, &b, err) || !pop(vm, &a, err)) return false;
    bool a_num = a.tag == ISH_VAL_INT || a.tag == ISH_VAL_FLOAT;
    bool b_num = b.tag == ISH_VAL_INT || b.tag == ISH_VAL_FLOAT;
    if (!a_num || !b_num) return ish_error_set(err, ish_span_unknown(NULL), "%s expects numeric operands", ish_opcode_name(op));
    if (a.tag == ISH_VAL_FLOAT || b.tag == ISH_VAL_FLOAT) {
        double x = a.tag == ISH_VAL_FLOAT ? a.as.f : (double)a.as.i;
        double y = b.tag == ISH_VAL_FLOAT ? b.as.f : (double)b.as.i;
        double r = 0.0;
        switch (op) {
            case ISH_OP_ADD: r = x + y; break;
            case ISH_OP_SUB: r = x - y; break;
            case ISH_OP_MUL: r = x * y; break;
            default: return ish_error_set(err, ish_span_unknown(NULL), "invalid numeric primitive");
        }
        return push(vm, ish_float(r), err);
    }
    int64_t out = 0;
    bool ok = false;
    switch (op) {
        case ISH_OP_ADD: ok = checked_add(a.as.i, b.as.i, &out); break;
        case ISH_OP_SUB: ok = checked_sub(a.as.i, b.as.i, &out); break;
        case ISH_OP_MUL: ok = checked_mul(a.as.i, b.as.i, &out); break;
        default: return ish_error_set(err, ish_span_unknown(NULL), "invalid integer primitive");
    }
    if (!ok) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in %s", ish_opcode_name(op));
    return push(vm, ish_int(out), err);
}

static bool generic_prim_call(Vm *vm, uint32_t primitive, uint32_t argc, IshError *err) {
    if (vm->sp < argc) return ish_error_set(err, ish_span_unknown(NULL), "PRIM_CALL stack underflow");
    IshValue *args = argc == 0 ? NULL : malloc((size_t)argc * sizeof(*args));
    if (argc != 0 && !args) return ish_error_oom(err, ish_span_unknown(NULL));
    for (uint32_t i = 0; i < argc; i++) args[argc - i - 1u] = vm->stack[--vm->sp];
    IshValue out = ish_nil();
    bool ok = ish_prim_invoke(vm->rt, (IshPrimitive)primitive, args, argc, &out, err);
    free(args);
    if (!ok) return false;
    return push(vm, out, err);
}

static bool vm_run_loop(Vm *vm, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err);
static bool vm_run_loop_inner(Vm *vm, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err);

static IshValue make_error_reason(Vm *vm, IshError *err) {
    IshError ignore;
    ish_error_init(&ignore);
    IshValue items[2];
    items[0] = ish_atom(vm->rt, "error");
    items[1] = ish_string(vm->rt, (err->present && err->message) ? err->message : "error", &ignore);
    IshValue reason = ish_tuple(vm->rt, items, 2u, &ignore);
    ish_error_clear(&ignore);
    return reason;
}

static bool init_pattern_locals(Vm *vm, Frame *frame, const IshBcFunction *fn, const IshPatternBindings *bindings, IshError *err) {
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) {
        if (fn->pattern_locals[i].slot >= frame->local_count) return ish_error_set(err, ish_span_unknown(NULL), "pattern local slot out of bounds");
        const IshValue *bound = bindings ? ish_pattern_bindings_get(bindings, fn->pattern_locals[i].name) : NULL;
        if (!bound) return ish_error_set(err, ish_span_unknown(NULL), "pattern binding '%s' missing", fn->pattern_locals[i].name);
        frame->locals[fn->pattern_locals[i].slot] = ish_cell(vm->rt, *bound, err);
        if (err && err->present) return false;
    }
    return true;
}

static bool run_guard_function(Vm *caller, const IshBytecodeModule *callee_module, IshValue callee, const IshBcFunction *candidate, const IshPatternBindings *bindings, size_t arg_base, uint32_t argc, bool *out_pass, IshError *err) {
    *out_pass = true;
    if (!candidate->has_guard) return true;
    if (candidate->guard_function >= callee_module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    const IshBcFunction *guard_fn = &callee_module->functions[candidate->guard_function];
    if (guard_fn->arity != argc) return ish_error_set(err, ish_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    guard_vm.rt = caller->rt;
    guard_vm.module = callee_module;
    guard_vm.limits = caller->limits;
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&guard_vm, caller->stack[arg_base + i], err)) {
            vm_reset(&guard_vm);
            return false;
        }
    }
    if (!push_frame(&guard_vm, callee_module, candidate->guard_function, callee, 0, 0, err)) {
        vm_reset(&guard_vm);
        return false;
    }
    Frame *guard_frame = current_frame(&guard_vm);
    if (!init_pattern_locals(&guard_vm, guard_frame, candidate, bindings, err)) {
        vm_reset(&guard_vm);
        return false;
    }

    IshError guard_err;
    ish_error_init(&guard_err);
    IshValue result = ish_nil();
    IshExecStatus guard_status = ISH_EXEC_DONE;
    bool ok = vm_run_loop(&guard_vm, -1, &guard_status, &result, NULL, &guard_err);
    vm_reset(&guard_vm);
    if (!ok || guard_err.present || guard_status != ISH_EXEC_DONE) {
        ish_error_clear(&guard_err);
        *out_pass = false;
        return true;
    }
    *out_pass = ish_vm_truthy(result);
    return true;
}

static bool vm_select_clause(Vm *vm, IshValue callee, size_t arg_base, uint32_t argc, uint32_t *out_index, IshPatternBindings *out_bindings, bool *out_has_bindings, bool *out_matched, IshError *err) {
    *out_matched = false;
    *out_has_bindings = false;
    *out_index = UINT32_MAX;
    const IshBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    for (size_t i = 0; i < ish_closure_entry_count(callee); i++) {
        uint32_t candidate_index = ish_closure_entry(callee, i, err);
        if (err && err->present) return false;
        if (candidate_index >= callee_module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "closure references invalid function %u", candidate_index);
        const IshBcFunction *candidate = &callee_module->functions[candidate_index];
        if (candidate->arity != argc) continue;
        bool patterns_ok = true;
        IshPatternBindings bindings;
        ish_pattern_bindings_init(&bindings);
        if (candidate->pattern_count != 0) {
            if (candidate->pattern_count != argc) return ish_error_set(err, ish_span_unknown(NULL), "function pattern metadata arity mismatch");
            for (uint32_t p = 0; p < argc; p++) {
                if (!ish_pattern_match(vm->rt, candidate->param_patterns[p], vm->stack[arg_base + p], &bindings, err)) {
                    patterns_ok = false;
                    break;
                }
                if (err && err->present) break;
            }
            if (err && err->present) {
                ish_pattern_bindings_destroy(&bindings);
                return false;
            }
        }
        if (!patterns_ok) {
            ish_pattern_bindings_destroy(&bindings);
            continue;
        }
        bool guard_ok = true;
        if (!run_guard_function(vm, callee_module, callee, candidate, &bindings, arg_base, argc, &guard_ok, err)) {
            ish_pattern_bindings_destroy(&bindings);
            return false;
        }
        if (!guard_ok) {
            ish_pattern_bindings_destroy(&bindings);
            continue;
        }
        *out_bindings = bindings;
        *out_has_bindings = true;
        *out_index = candidate_index;
        *out_matched = true;
        return true;
    }
    return true;
}

static bool vm_enter_clause(Vm *vm, IshValue callee, uint32_t function_index, size_t closure_index, const IshPatternBindings *bindings, IshError *err) {
    const IshBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    const IshBcFunction *fn = &callee_module->functions[function_index];
    if (!push_frame(vm, callee_module, function_index, callee, closure_index + 1u, closure_index, err)) return false;
    Frame *new_frame = current_frame(vm);
    return init_pattern_locals(vm, new_frame, fn, bindings, err);
}

static bool call_value(Vm *vm, uint32_t argc, bool tail, IshError *err) {
    if (vm->sp < (size_t)argc + 1u) return ish_error_set(err, ish_span_unknown(NULL), "CALL stack underflow");
    size_t closure_index = vm->sp - (size_t)argc - 1u;
    IshValue callee = vm->stack[closure_index];
    if (callee.tag == ISH_VAL_PRIMITIVE) {
        IshValue *args = argc == 0 ? NULL : &vm->stack[closure_index + 1u];
        IshValue out = ish_nil();
        if (!ish_prim_invoke(vm->rt, (IshPrimitive)callee.as.id, args, argc, &out, err)) return false;
        vm->sp = closure_index;
        return push(vm, out, err);
    }
    if (!ish_is_closure(callee)) return ish_error_set(err, ish_span_unknown(NULL), "attempted to call a non-closure");
    uint32_t function_index = UINT32_MAX;
    IshPatternBindings selected_bindings;
    ish_pattern_bindings_init(&selected_bindings);
    bool has_selected_bindings = false;
    bool matched = false;
    if (!vm_select_clause(vm, callee, closure_index + 1u, argc, &function_index, &selected_bindings, &has_selected_bindings, &matched, err)) return false;
    if (!matched) {
        const IshBytecodeModule *cm = closure_module_or_current(vm, callee);
        const char *cname = NULL;
        uint32_t first_entry = ish_closure_entry_count(callee) != 0 ? ish_closure_entry(callee, 0, NULL) : ish_closure_function_index(callee);
        if (cm && first_entry < cm->function_count) cname = cm->functions[first_entry].name;
        IshBuffer args_buf;
        ish_buf_init(&args_buf);
        bool rendered = true;
        for (uint32_t i = 0; i < argc && rendered && args_buf.len < 160u; i++) {
            if (i != 0) rendered = ish_buf_append(&args_buf, " ");
            if (rendered) rendered = ish_value_write(&args_buf, vm->stack[closure_index + 1u + i]);
        }
        ish_error_set(err, ish_span_unknown(NULL), "no clause of '%s' matches (%.*s%s)",
                      cname && cname[0] ? cname : "<fn>",
                      (int)(args_buf.len < 160u ? args_buf.len : 160u), args_buf.data ? args_buf.data : "",
                      args_buf.len > 160u ? " ..." : "");
        ish_buf_destroy(&args_buf);
        return false;
    }
    const IshBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    const IshBcFunction *fn = &callee_module->functions[function_index];
    if (!tail) {
        if (!vm_enter_clause(vm, callee, function_index, closure_index, has_selected_bindings ? &selected_bindings : NULL, err)) {
            if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
            return false;
        }
        if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
        return true;
    }

    Frame *frame = current_frame(vm);
    if (!frame) return ish_error_set(err, ish_span_unknown(NULL), "TAIL_CALL without frame");
    IshValue *args = argc == 0 ? NULL : malloc((size_t)argc * sizeof(*args));
    if (argc != 0 && !args) { if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings); return ish_error_oom(err, ish_span_unknown(NULL)); }
    for (uint32_t i = 0; i < argc; i++) args[i] = vm->stack[closure_index + 1u + i];
    size_t result_base = frame->result_base;
    free(frame->locals);
    frame->function_index = function_index;
    frame->module = callee_module;
    frame->ip = fn->entry;
    frame->base = result_base;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->closure = callee;
    frame->ns = ish_is_closure(callee) ? ish_closure_namespace(callee) : frame->ns;
    if (!frame->ns) frame->ns = vm->rt->main_ns;
    frame->locals = fn->local_count == 0 ? NULL : calloc(fn->local_count, sizeof(*frame->locals));
    if (fn->local_count != 0 && !frame->locals) { free(args); if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings); return ish_error_oom(err, ish_span_unknown(NULL)); }
    for (uint32_t i = 0; i < fn->local_count; i++) frame->locals[i] = ish_nil();
    if (!init_pattern_locals(vm, frame, fn, has_selected_bindings ? &selected_bindings : NULL, err)) {
        free(args);
        if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
        return false;
    }
    vm->sp = result_base;
    if (!stack_reserve(vm, vm->sp + argc, err)) { free(args); if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings); return false; }
    for (uint32_t i = 0; i < argc; i++) vm->stack[vm->sp++] = args[i];
    free(args);
    if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
    return true;
}

static bool require_actor(Vm *vm, IshError *err) {
    if (!vm->self || !vm->sched) return ish_error_set(err, ish_span_unknown(NULL), "actor operation requires a running actor");
    return true;
}

static bool op_recv(Vm *vm, Frame *frame, size_t instr_ip, IshExecStatus *status, IshError *err) {
    const IshBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t timeout_label = module->code[frame->ip++];
    if (!require_actor(vm, err)) return false;
    if (vm->sp < 2u) return ish_error_set(err, ish_span_unknown(NULL), "RECV stack underflow");
    IshValue closure = vm->stack[vm->sp - 1u];
    IshValue timeout = vm->stack[vm->sp - 2u];
    if (!ish_is_closure(closure)) return ish_error_set(err, ish_span_unknown(NULL), "receive requires clause closure");

    bool matched = false;
    size_t matched_index = 0;
    uint32_t matched_fn = 0;
    IshPatternBindings matched_bindings;
    ish_pattern_bindings_init(&matched_bindings);
    bool matched_has = false;
    size_t count = ish_actor_mailbox_count(vm->self);
    for (size_t i = ish_actor_recv_cursor(vm->self); i < count; i++) {
        IshValue msg;
        if (!ish_actor_mailbox_peek(vm->self, i, &msg)) break;
        if (!push(vm, msg, err)) { if (matched_has) ish_pattern_bindings_destroy(&matched_bindings); return false; }
        uint32_t idx = UINT32_MAX;
        IshPatternBindings b;
        ish_pattern_bindings_init(&b);
        bool has = false;
        bool m = false;
        bool ok = vm_select_clause(vm, closure, vm->sp - 1u, 1u, &idx, &b, &has, &m, err);
        vm->sp--;
        if (!ok) { if (has) ish_pattern_bindings_destroy(&b); if (matched_has) ish_pattern_bindings_destroy(&matched_bindings); return false; }
        if (m) {
            matched = true;
            matched_index = i;
            matched_fn = idx;
            matched_bindings = b;
            matched_has = has;
            break;
        }
        if (has) ish_pattern_bindings_destroy(&b);
    }

    if (matched) {
        IshValue removed;
        if (!ish_actor_mailbox_remove(vm->self, matched_index, &removed)) {
            if (matched_has) ish_pattern_bindings_destroy(&matched_bindings);
            return ish_error_set(err, ish_span_unknown(NULL), "mailbox message vanished during receive");
        }
        ish_actor_recv_reset(vm->self);
        vm->sp -= 2u;
        if (!push(vm, closure, err) || !push(vm, removed, err)) { if (matched_has) ish_pattern_bindings_destroy(&matched_bindings); return false; }
        size_t closure_index = vm->sp - 2u;
        bool ok = vm_enter_clause(vm, closure, matched_fn, closure_index, matched_has ? &matched_bindings : NULL, err);
        if (matched_has) ish_pattern_bindings_destroy(&matched_bindings);
        return ok;
    }

    ish_actor_recv_set_cursor(vm->self, count);
    IshRecvDecision decision = ISH_RECV_BLOCK;
    if (!ish_actor_recv_no_match(vm->self, timeout, &decision, err)) return false;
    if (decision == ISH_RECV_TIMEOUT) {
        vm->sp -= 2u;
        frame->ip = timeout_label;
        return true;
    }
    frame->ip = instr_ip;
    *status = ISH_EXEC_BLOCK_RECEIVE;
    return true;
}

static bool op_actor_unary_pid(Vm *vm, IshOpcode op, IshError *err) {
    IshValue target;
    if (!pop(vm, &target, err)) return false;
    if (target.tag != ISH_VAL_PID) return ish_error_set(err, ish_span_unknown(NULL), "%s expects a pid", ish_opcode_name(op));
    switch (op) {
        case ISH_OP_UNLINK:
            ish_sched_unlink(vm->sched, vm->self, target.as.id);
            return push(vm, ish_atom(vm->rt, "ok"), err);
        case ISH_OP_MONITOR: {
            IshValue ref = ish_nil();
            if (!ish_sched_monitor(vm->sched, vm->self, target.as.id, &ref, err)) return false;
            return push(vm, ref, err);
        }
        default:
            return ish_error_set(err, ish_span_unknown(NULL), "invalid actor pid op");
    }
}

static const char *module_const_text(const IshBytecodeModule *module, uint32_t index, const char *what, IshError *err) {
    if (index >= module->const_count) {
        ish_error_set(err, ish_span_unknown(NULL), "%s constant %u out of bounds", what, index);
        return NULL;
    }
    IshValue value = module->constants[index];
    if (value.tag == ISH_VAL_ATOM || value.tag == ISH_VAL_WORD) return ish_symbol_text(value.as.symbol);
    if (value.tag == ISH_VAL_STRING) return ish_string_bytes(value);
    ish_error_set(err, ish_span_unknown(NULL), "%s constant must be a name", what);
    return NULL;
}

static bool op_define_protocol(Vm *vm, Frame *frame, IshError *err) {
    const IshBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t protocol_const = module->code[frame->ip++];
    uint32_t method_count = module->code[frame->ip++];
    const char *protocol = module_const_text(module, protocol_const, "DEFINE_PROTOCOL protocol", err);
    if (!protocol) return false;
    if (vm->sp < method_count) return ish_error_set(err, ish_span_unknown(NULL), "DEFINE_PROTOCOL stack underflow");
    IshProtocolMethodSpec *specs = method_count == 0 ? NULL : calloc(method_count, sizeof(*specs));
    if (method_count != 0 && !specs) return ish_error_oom(err, ish_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; i < method_count && ok; i++) {
        uint32_t method_const = module->code[frame->ip++];
        uint32_t arity = module->code[frame->ip++];
        uint32_t has_default = module->code[frame->ip++];
        const char *method = module_const_text(module, method_const, "DEFINE_PROTOCOL method", err);
        if (!method) { ok = false; break; }
        specs[i].name = method;
        specs[i].arity = arity;
        specs[i].has_default = has_default != 0;
    }
    if (!ok) { free(specs); return false; }
    for (uint32_t i = method_count; i > 0; i--) {
        IshValue default_impl = ish_nil();
        if (!pop(vm, &default_impl, err)) { free(specs); return false; }
        specs[i - 1u].default_impl = default_impl;
    }
    ok = ish_protocol_define(vm->rt, protocol, specs, method_count, err);
    free(specs);
    if (!ok) return false;
    return push(vm, ish_atom(vm->rt, "ok"), err);
}

static bool op_extend_protocol(Vm *vm, Frame *frame, IshError *err) {
    const IshBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t protocol_const = module->code[frame->ip++];
    uint32_t type_const = module->code[frame->ip++];
    uint32_t impl_count = module->code[frame->ip++];
    const char *protocol = module_const_text(module, protocol_const, "EXTEND_PROTOCOL protocol", err);
    if (!protocol) return false;
    const char *type = module_const_text(module, type_const, "EXTEND_PROTOCOL type", err);
    if (!type) return false;
    if (vm->sp < impl_count) return ish_error_set(err, ish_span_unknown(NULL), "EXTEND_PROTOCOL stack underflow");
    IshProtocolImplSpec *specs = impl_count == 0 ? NULL : calloc(impl_count, sizeof(*specs));
    if (impl_count != 0 && !specs) return ish_error_oom(err, ish_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; i < impl_count && ok; i++) {
        uint32_t method_const = module->code[frame->ip++];
        uint32_t arity = module->code[frame->ip++];
        const char *method = module_const_text(module, method_const, "EXTEND_PROTOCOL method", err);
        if (!method) { ok = false; break; }
        specs[i].name = method;
        specs[i].arity = arity;
    }
    if (!ok) { free(specs); return false; }
    for (uint32_t i = impl_count; i > 0; i--) {
        IshValue impl = ish_nil();
        if (!pop(vm, &impl, err)) { free(specs); return false; }
        specs[i - 1u].impl = impl;
    }
    ok = ish_protocol_extend(vm->rt, protocol, type, specs, impl_count, err);
    free(specs);
    if (!ok) return false;
    return push(vm, ish_atom(vm->rt, "ok"), err);
}

static bool op_call_method(Vm *vm, Frame *frame, IshError *err) {
    const IshBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t protocol_const = module->code[frame->ip++];
    uint32_t method_const = module->code[frame->ip++];
    uint32_t argc = module->code[frame->ip++];
    if (argc == 0) return ish_error_set(err, ish_span_unknown(NULL), "CALL_METHOD requires a receiver");
    if (vm->sp < argc) return ish_error_set(err, ish_span_unknown(NULL), "CALL_METHOD stack underflow");
    const char *protocol = module_const_text(module, protocol_const, "CALL_METHOD protocol", err);
    const char *method = protocol ? module_const_text(module, method_const, "CALL_METHOD method", err) : NULL;
    if (!protocol || !method) return false;
    size_t arg_base = vm->sp - argc;
    IshValue receiver = vm->stack[arg_base];
    const char *type = ish_value_dispatch_type_name(receiver);
    IshValue impl = ish_nil();
    if (!ish_protocol_lookup(vm->rt, protocol, method, type, argc, &impl, err)) return false;
    if (!stack_reserve(vm, vm->sp + 1u, err)) return false;
    memmove(&vm->stack[arg_base + 1u], &vm->stack[arg_base], argc * sizeof(*vm->stack));
    vm->stack[arg_base] = impl;
    vm->sp++;
    return call_value(vm, argc, false, err);
}

static bool vm_run_loop_inner(Vm *vm, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err) {
        IshRuntime *rt = vm->rt;
    int64_t left = budget;
    IshValue result = ish_nil();
    *status = ISH_EXEC_DONE;
    for (;;) {
        Frame *frame = current_frame(vm);
        if (!frame) break;
        const IshBytecodeModule *module = frame->module ? frame->module : vm->module;
        if (budget >= 0) {
            if (left <= 0) { *status = ISH_EXEC_YIELD; return true; }
            left--;
        }
        size_t instr_ip = frame->ip;
        if (frame->ip >= module->code_count) {
            ish_error_set(err, ish_span_unknown(NULL), "instruction pointer out of bounds");
            return false;
        }
        IshOpcode op = (IshOpcode)module->code[frame->ip++];
        uint32_t operand = 0;
        switch (op) {
            case ISH_OP_HALT:
                if (vm->sp != 0 && !pop(vm, &result, err)) return false;
                *status = ISH_EXEC_DONE;
                *out_result = result;
                return true;
            case ISH_OP_LOAD_CONST:
                operand = module->code[frame->ip++];
                if (!push(vm, module->constants[operand], err)) return false;
                break;
            case ISH_OP_LOAD_ARG:
                operand = module->code[frame->ip++];
                if (operand >= module->functions[frame->function_index].arity) return ish_error_set(err, ish_span_unknown(NULL), "argument index %u out of bounds", operand);
                if (!push(vm, vm->stack[frame->base + operand], err)) return false;
                break;
            case ISH_OP_LOAD_LOCAL:
                operand = module->code[frame->ip++];
                if (operand >= frame->local_count) return ish_error_set(err, ish_span_unknown(NULL), "local index %u out of bounds", operand);
                if (!push(vm, frame->locals[operand], err)) return false;
                break;
            case ISH_OP_STORE_LOCAL: {
                operand = module->code[frame->ip++];
                if (operand >= frame->local_count) return ish_error_set(err, ish_span_unknown(NULL), "local index %u out of bounds", operand);
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                frame->locals[operand] = value;
                break;
            }
            case ISH_OP_LOAD_CAPTURE:
                operand = module->code[frame->ip++];
                if (frame->closure.tag != ISH_VAL_CLOSURE || operand >= ish_closure_capture_count(frame->closure)) return ish_error_set(err, ish_span_unknown(NULL), "capture index %u out of bounds", operand);
                if (!push(vm, ish_closure_capture(frame->closure, operand, err), err)) return false;
                if (err && err->present) return false;
                break;
            case ISH_OP_MAKE_CELL: {
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                if (!push(vm, ish_cell(rt, value, err), err)) return false;
                if (err && err->present) return false;
                break;
            }
            case ISH_OP_LOAD_CELL: {
                IshValue cell;
                if (!pop(vm, &cell, err)) return false;
                if (!push(vm, ish_cell_get(cell, err), err)) return false;
                if (err && err->present) return false;
                break;
            }
            case ISH_OP_STORE_CELL: {
                IshValue value, cell;
                if (!pop(vm, &value, err) || !pop(vm, &cell, err)) return false;
                if (!ish_cell_set(cell, value, err)) return false;
                break;
            }
            case ISH_OP_MAKE_CLOSURE:
                operand = module->code[frame->ip++];
                if (!push(vm, ish_closure_in_module(rt, module, operand, NULL, 0, frame->ns, err), err)) return false;
                if (err && err->present) return false;
                break;
            case ISH_OP_MAKE_CLOSURE_CAPTURES: {
                uint32_t fn_index = module->code[frame->ip++];
                uint32_t capture_count = module->code[frame->ip++];
                if (vm->sp < capture_count) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES stack underflow");
                IshValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) return ish_error_oom(err, ish_span_unknown(NULL));
                size_t start = vm->sp - capture_count;
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = vm->stack[start + i];
                vm->sp = start;
                IshValue closure = ish_closure_in_module(rt, module, fn_index, captures, capture_count, frame->ns, err);
                free(captures);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case ISH_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = module->code[frame->ip++];
                uint32_t capture_count = module->code[frame->ip++];
                if (entry_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_MULTI_CLOSURE requires entries");
                uint32_t *entries = malloc((size_t)entry_count * sizeof(*entries));
                if (!entries) return ish_error_oom(err, ish_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) entries[i] = module->code[frame->ip++];
                if (vm->sp < capture_count) { free(entries); return ish_error_set(err, ish_span_unknown(NULL), "MAKE_MULTI_CLOSURE stack underflow"); }
                IshValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) { free(entries); return ish_error_oom(err, ish_span_unknown(NULL)); }
                size_t start = vm->sp - capture_count;
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = vm->stack[start + i];
                vm->sp = start;
                IshValue closure = ish_closure_multi_in_module(rt, module, entries, entry_count, captures, capture_count, frame->ns, err);
                free(entries);
                free(captures);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case ISH_OP_CALL:
                operand = module->code[frame->ip++];
                if (!call_value(vm, operand, false, err)) return false;
                break;
            case ISH_OP_TAIL_CALL:
                operand = module->code[frame->ip++];
                if (!call_value(vm, operand, true, err)) return false;
                break;
            case ISH_OP_RETURN: {
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                Frame returning;
                pop_frame(vm, &returning);
                size_t result_base = returning.result_base;
                free(returning.locals);
                if (vm->frame_count == 0) {
                    *status = ISH_EXEC_DONE;
                    *out_result = value;
                    return true;
                }
                vm->sp = result_base;
                if (!push(vm, value, err)) return false;
                break;
            }
            case ISH_OP_POP: {
                IshValue ignored;
                if (!pop(vm, &ignored, err)) return false;
                break;
            }
            case ISH_OP_JUMP:
                operand = module->code[frame->ip++];
                frame->ip = operand;
                break;
            case ISH_OP_JUMP_IF_FALSE: {
                operand = module->code[frame->ip++];
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                if (!ish_vm_truthy(value)) frame->ip = operand;
                break;
            }
            case ISH_OP_ADD:
            case ISH_OP_SUB:
            case ISH_OP_MUL:
                if (!binary_int(vm, op, err)) return false;
                break;
            case ISH_OP_EQ: {
                IshValue b, a;
                if (!pop(vm, &b, err) || !pop(vm, &a, err)) return false;
                if (!push(vm, ish_atom(rt, ish_value_equal(a, b) ? "true" : "false"), err)) return false;
                break;
            }
            case ISH_OP_LT: {
                IshValue b, a;
                if (!pop(vm, &b, err) || !pop(vm, &a, err)) return false;
                bool an = a.tag == ISH_VAL_INT || a.tag == ISH_VAL_FLOAT;
                bool bn = b.tag == ISH_VAL_INT || b.tag == ISH_VAL_FLOAT;
                if (!an || !bn) return ish_error_set(err, ish_span_unknown(NULL), "LT expects numeric operands");
                bool lt = (a.tag == ISH_VAL_FLOAT || b.tag == ISH_VAL_FLOAT)
                    ? ((a.tag == ISH_VAL_FLOAT ? a.as.f : (double)a.as.i) < (b.tag == ISH_VAL_FLOAT ? b.as.f : (double)b.as.i))
                    : (a.as.i < b.as.i);
                if (!push(vm, ish_atom(rt, lt ? "true" : "false"), err)) return false;
                break;
            }
            case ISH_OP_PRIM_CALL: {
                uint32_t prim = module->code[frame->ip++];
                uint32_t argc = module->code[frame->ip++];
                if (!generic_prim_call(vm, prim, argc, err)) return false;
                break;
            }
            case ISH_OP_SELF:
                if (!require_actor(vm, err)) return false;
                if (!push(vm, ish_pid(ish_actor_pid(vm->self)), err)) return false;
                break;
            case ISH_OP_SPAWN: {
                if (!require_actor(vm, err)) return false;
                IshValue thunk;
                if (!pop(vm, &thunk, err)) return false;
                if (!ish_is_closure(thunk)) return ish_error_set(err, ish_span_unknown(NULL), "spawn expects a function value");
                IshValue pid = ish_nil();
                if (!ish_sched_spawn(vm->sched, thunk, vm, &pid, err)) return false;
                if (!push(vm, pid, err)) return false;
                break;
            }
            case ISH_OP_SEND: {
                if (!require_actor(vm, err)) return false;
                IshValue msg, target;
                if (!pop(vm, &msg, err) || !pop(vm, &target, err)) return false;
                if (target.tag != ISH_VAL_PID) return ish_error_set(err, ish_span_unknown(NULL), "send expects a pid target");
                ish_sched_send(vm->sched, target.as.id, msg);
                if (!push(vm, msg, err)) return false;
                break;
            }
            case ISH_OP_EXIT: {
                IshValue reason;
                if (!pop(vm, &reason, err)) return false;
                *status = ISH_EXEC_EXIT;
                if (out_reason) *out_reason = reason;
                return true;
            }
            case ISH_OP_LINK: {
                if (!require_actor(vm, err)) return false;
                IshValue target;
                if (!pop(vm, &target, err)) return false;
                if (target.tag != ISH_VAL_PID) return ish_error_set(err, ish_span_unknown(NULL), "link expects a pid");
                bool self_exits = false;
                IshValue exit_reason = ish_nil();
                if (!ish_sched_link(vm->sched, vm->self, target.as.id, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    *status = ISH_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                if (!push(vm, ish_atom(rt, "ok"), err)) return false;
                break;
            }
            case ISH_OP_UNLINK:
            case ISH_OP_MONITOR:
                if (!require_actor(vm, err)) return false;
                if (!op_actor_unary_pid(vm, op, err)) return false;
                break;
            case ISH_OP_DEMONITOR: {
                if (!require_actor(vm, err)) return false;
                IshValue ref;
                if (!pop(vm, &ref, err)) return false;
                if (ref.tag != ISH_VAL_REF) return ish_error_set(err, ish_span_unknown(NULL), "demonitor expects a reference");
                ish_sched_demonitor(vm->sched, vm->self, ref);
                if (!push(vm, ish_atom(rt, "ok"), err)) return false;
                break;
            }
            case ISH_OP_TRAP_EXIT: {
                if (!require_actor(vm, err)) return false;
                IshValue flag;
                if (!pop(vm, &flag, err)) return false;
                bool previous = ish_actor_trap_exit_get(vm->self);
                ish_actor_trap_exit_set(vm->self, ish_vm_truthy(flag));
                if (!push(vm, ish_atom(rt, previous ? "true" : "false"), err)) return false;
                break;
            }
            case ISH_OP_RECV:
                if (!op_recv(vm, frame, instr_ip, status, err)) return false;
                if (*status == ISH_EXEC_BLOCK_RECEIVE) return true;
                break;
            case ISH_OP_EXEC: {
                if (!require_actor(vm, err)) return false;
                IshValue graph;
                if (!pop(vm, &graph, err)) return false;
                vm->port_request = graph;
                vm->has_port_request = true;
                *status = ISH_EXEC_LAUNCH_PORT;
                return true;
            }
            case ISH_OP_AWAIT: {
                if (!require_actor(vm, err)) return false;
                IshValue port;
                if (!pop(vm, &port, err)) return false;
                if (port.tag != ISH_VAL_PORT) return ish_error_set(err, ish_span_unknown(NULL), "await expects a port");
                vm->await_port = port;
                vm->has_await = true;
                *status = ISH_EXEC_BLOCK_AWAIT;
                return true;
            }
            case ISH_OP_APPLY: {
                IshValue arglist;
                IshValue callee;
                if (!pop(vm, &arglist, err)) return false;
                if (!pop(vm, &callee, err)) return false;
                size_t argc = 0;
                IshValue cursor = arglist;
                while (ish_is_pair(cursor)) {
                    argc++;
                    cursor = ish_cdr(cursor, err);
                    if (err && err->present) return false;
                }
                if (!ish_is_nil(cursor)) return ish_error_set(err, ish_span_unknown(NULL), "apply expects a proper list of arguments");
                if (argc > UINT32_MAX) return ish_error_set(err, ish_span_unknown(NULL), "apply argument list too long");
                if (!push(vm, callee, err)) return false;
                cursor = arglist;
                for (size_t i = 0; i < argc; i++) {
                    IshValue item = ish_car(cursor, err);
                    if (err && err->present) return false;
                    if (!push(vm, item, err)) return false;
                    cursor = ish_cdr(cursor, err);
                    if (err && err->present) return false;
                }
                if (!call_value(vm, (uint32_t)argc, false, err)) return false;
                break;
            }
            case ISH_OP_ENTER_NAMESPACE: {
                operand = module->code[frame->ip++];
                if (operand >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "ENTER_NAMESPACE constant out of bounds");
                IshValue name_value = module->constants[operand];
                if (name_value.tag != ISH_VAL_ATOM && name_value.tag != ISH_VAL_WORD) return ish_error_set(err, ish_span_unknown(NULL), "ENTER_NAMESPACE expects a name constant");
                IshNamespace *target = ish_namespace_get_or_create(rt, ish_symbol_text(name_value.as.symbol));
                if (!target) return ish_error_oom(err, ish_span_unknown(NULL));
                if (vm->ns_save_count == vm->ns_save_cap) {
                    size_t cap = vm->ns_save_cap ? vm->ns_save_cap * 2u : 8u;
                    IshNamespace **grown = realloc(vm->ns_save, cap * sizeof(*grown));
                    if (!grown) return ish_error_oom(err, ish_span_unknown(NULL));
                    vm->ns_save = grown;
                    vm->ns_save_cap = cap;
                }
                vm->ns_save[vm->ns_save_count++] = frame->ns;
                frame->ns = target;
                break;
            }
            case ISH_OP_IMPORT_GLOBAL: {
                uint32_t import_src = module->code[frame->ip++];
                uint32_t import_dst = module->code[frame->ip++];
                if (vm->ns_save_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "IMPORT_GLOBAL with no active namespace");
                IshNamespace *outer = vm->ns_save[vm->ns_save_count - 1u];
                if (!ish_ns_slot_ensure(outer, import_dst, err)) return false;
                ish_ns_slot_set(outer, import_dst, ish_ns_slot_get(frame->ns, import_src));
                break;
            }
            case ISH_OP_LEAVE_NAMESPACE: {
                if (vm->ns_save_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "LEAVE_NAMESPACE with no active namespace");
                frame->ns = vm->ns_save[--vm->ns_save_count];
                break;
            }
            case ISH_OP_DEFINE_PROTOCOL:
                if (!op_define_protocol(vm, frame, err)) return false;
                break;
            case ISH_OP_EXTEND_PROTOCOL:
                if (!op_extend_protocol(vm, frame, err)) return false;
                break;
            case ISH_OP_CALL_METHOD:
                if (!op_call_method(vm, frame, err)) return false;
                break;
            case ISH_OP_RESCUE_PUSH: {
                operand = module->code[frame->ip++];
                if (vm->handler_count == vm->handler_cap) {
                    size_t cap = vm->handler_cap ? vm->handler_cap * 2u : 8u;
                    Handler *next = realloc(vm->handlers, cap * sizeof(*next));
                    if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
                    vm->handlers = next;
                    vm->handler_cap = cap;
                }
                vm->handlers[vm->handler_count].catch_ip = operand;
                vm->handlers[vm->handler_count].frame_count = vm->frame_count;
                vm->handlers[vm->handler_count].sp = vm->sp;
                vm->handler_count++;
                break;
            }
            case ISH_OP_RESCUE_POP:
                if (vm->handler_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "RESCUE_POP with no active handler");
                vm->handler_count--;
                break;
            case ISH_OP_RAISE: {
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                vm->raised = value;
                vm->has_raised = true;
                IshBuffer buf;
                ish_buf_init(&buf);
                if (ish_value_write(&buf, value) && buf.data) ish_error_set(err, ish_span_unknown(NULL), "%s", buf.data);
                else ish_error_set(err, ish_span_unknown(NULL), "error raised");
                ish_buf_destroy(&buf);
                return false;
            }
            case ISH_OP_LOAD_RAISED:
                if (!push(vm, vm->raised, err)) return false;
                break;
            case ISH_OP_LOAD_GLOBAL:
                operand = module->code[frame->ip++];
                if (!push(vm, ish_ns_slot_get(frame->ns, operand), err)) return false;
                break;
            case ISH_OP_STORE_GLOBAL: {
                operand = module->code[frame->ip++];
                IshValue value;
                if (!pop(vm, &value, err)) return false;
                if (!ish_ns_slot_ensure(frame->ns, operand, err)) return false;
                ish_ns_slot_set(frame->ns, operand, value);
                break;
            }
            default:
                ish_error_set(err, ish_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
                return false;
        }
    }
    *status = ISH_EXEC_DONE;
    *out_result = result;
    return true;
}

static bool vm_run_loop(Vm *vm, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err) {
    for (;;) {
        bool ok = vm_run_loop_inner(vm, budget, status, out_result, out_reason, err);
        if (ok) return true;
        if (vm->handler_count == 0) {
            if (vm->has_raised) {
                vm->has_raised = false;
                ish_error_clear(err);
                *status = ISH_EXEC_EXIT;
                if (out_reason) *out_reason = vm->raised;
                return true;
            }
            size_t depth = vm->frame_count;
            size_t shown = depth < 12u ? depth : 12u;
            for (size_t i = 0; i < shown; i++) {
                const Frame *f = &vm->frames[depth - 1u - i];
                const char *fname = f->module && f->function_index < f->module->function_count ? f->module->functions[f->function_index].name : NULL;
                ish_error_note(err, "in %s", fname && fname[0] ? fname : "<fn>");
            }
            if (depth > shown) ish_error_note(err, "... (%zu more frames)", depth - shown);
            return false;
        }
        Handler handler = vm->handlers[--vm->handler_count];
        while (vm->frame_count > handler.frame_count) {
            Frame discarded;
            pop_frame(vm, &discarded);
            free(discarded.locals);
        }
        Frame *frame = current_frame(vm);
        if (!frame) return false;
        if (handler.sp <= vm->sp) vm->sp = handler.sp;
        if (vm->has_raised) {
            vm->has_raised = false;
        } else {
            vm->raised = make_error_reason(vm, err);
        }
        ish_error_clear(err);
        frame->ip = handler.catch_ip;
        if (budget >= 0) {
            *status = ISH_EXEC_YIELD;
            return true;
        }
    }
}

bool ish_vm_run_limited(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshVmLimits limits, IshValue *out, IshError *err) {
    if (!ish_bc_verify(module, err)) return false;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = limits;
    if (!push_frame(&vm, module, function_index, ish_nil(), 0, 0, err)) { vm_reset(&vm); return false; }
    IshExecStatus status = ISH_EXEC_DONE;
    IshValue result = ish_nil();
    IshValue reason = ish_nil();
    bool ok = vm_run_loop(&vm, -1, &status, &result, &reason, err);
    vm_reset(&vm);
    if (!ok) return false;
    if (status == ISH_EXEC_EXIT) {
        IshBuffer buf;
        ish_buf_init(&buf);
        if (ish_value_write(&buf, reason) && buf.data) {
            ish_error_set(err, ish_span_unknown(NULL), "exited with reason %s", buf.data);
        } else {
            ish_error_set(err, ish_span_unknown(NULL), "program exited outside the scheduler");
        }
        ish_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

bool ish_vm_run(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshValue *out, IshError *err) {
    return ish_vm_run_limited(rt, module, function_index, ish_vm_default_limits(), out, err);
}

bool ish_vm_call_function(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, const IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    if (!ish_bc_verify(module, err)) return false;
    if (function_index >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (module->functions[function_index].arity != argc) return ish_error_set(err, ish_span_unknown(NULL), "function arity mismatch: expected %u got %u", module->functions[function_index].arity, argc);
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = ish_vm_default_limits();
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&vm, args[i], err)) {
            vm_reset(&vm);
            return false;
        }
    }
    if (!push_frame(&vm, module, function_index, ish_nil(), 0, 0, err)) {
        vm_reset(&vm);
        return false;
    }
    IshExecStatus status = ISH_EXEC_DONE;
    IshValue result = ish_nil();
    IshValue reason = ish_nil();
    bool ok = vm_run_loop(&vm, -1, &status, &result, &reason, err);
    vm_reset(&vm);
    if (!ok) return false;
    if (status == ISH_EXEC_EXIT) {
        IshBuffer buf;
        ish_buf_init(&buf);
        if (ish_value_write(&buf, reason) && buf.data) {
            ish_error_set(err, ish_span_unknown(NULL), "exited with reason %s", buf.data);
        } else {
            ish_error_set(err, ish_span_unknown(NULL), "exited outside the scheduler");
        }
        ish_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

bool ish_vm_call_closure(IshRuntime *rt, IshValue closure, const IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    if (!ish_is_closure(closure)) return ish_error_set(err, ish_span_unknown(NULL), "call target must be a function value");
    const IshBytecodeModule *module = ish_closure_module(closure);
    if (!module) return ish_error_set(err, ish_span_unknown(NULL), "function value has no backing module");
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = ish_vm_default_limits();
    if (!push(&vm, closure, err)) {
        vm_reset(&vm);
        return false;
    }
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&vm, args[i], err)) {
            vm_reset(&vm);
            return false;
        }
    }
    if (!call_value(&vm, argc, false, err)) {
        vm_reset(&vm);
        return false;
    }
    IshExecStatus status = ISH_EXEC_DONE;
    IshValue result = ish_nil();
    IshValue reason = ish_nil();
    bool ok = vm_run_loop(&vm, -1, &status, &result, &reason, err);
    vm_reset(&vm);
    if (!ok) return false;
    if (status == ISH_EXEC_EXIT) {
        IshBuffer buf;
        ish_buf_init(&buf);
        if (ish_value_write(&buf, reason) && buf.data) {
            ish_error_set(err, ish_span_unknown(NULL), "exited with reason %s", buf.data);
        } else {
            ish_error_set(err, ish_span_unknown(NULL), "exited outside the scheduler");
        }
        ish_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

IshExec *ish_exec_create(IshRuntime *rt, const IshBytecodeModule *module, IshScheduler *sched, IshActor *self, IshVmLimits limits, IshError *err) {
    IshExec *exec = calloc(1u, sizeof(*exec));
    if (!exec) {
        ish_error_oom(err, ish_span_unknown(NULL));
        return NULL;
    }
    exec->rt = rt;
    exec->module = module;
    exec->sched = sched;
    exec->self = self;
    exec->limits = limits;
    return exec;
}

void ish_exec_destroy(IshExec *exec) {
    if (!exec) return;
    vm_reset(exec);
    free(exec->cwd);
    for (size_t i = 0; i < exec->env_count; i++) {
        free(exec->env_names[i]);
        free(exec->env_values[i]);
    }
    free(exec->env_names);
    free(exec->env_values);
    free(exec);
}

const char *ish_exec_cwd(const IshExec *exec) {
    return exec ? exec->cwd : NULL;
}

bool ish_exec_set_cwd(IshExec *exec, const char *cwd) {
    char *copy = ish_strdup(cwd);
    if (!copy) return false;
    free(exec->cwd);
    exec->cwd = copy;
    return true;
}

const char *ish_exec_env_get(const IshExec *exec, const char *name) {
    if (!exec) return NULL;
    for (size_t i = 0; i < exec->env_count; i++) {
        if (strcmp(exec->env_names[i], name) == 0) return exec->env_values[i];
    }
    return NULL;
}

bool ish_exec_env_set(IshExec *exec, const char *name, const char *value) {
    for (size_t i = 0; i < exec->env_count; i++) {
        if (strcmp(exec->env_names[i], name) == 0) {
            char *copy = ish_strdup(value);
            if (!copy) return false;
            free(exec->env_values[i]);
            exec->env_values[i] = copy;
            return true;
        }
    }
    if (exec->env_count == exec->env_cap) {
        size_t cap = exec->env_cap ? exec->env_cap * 2u : 8u;
        char **names = realloc(exec->env_names, cap * sizeof(*names));
        if (!names) return false;
        exec->env_names = names;
        char **values = realloc(exec->env_values, cap * sizeof(*values));
        if (!values) return false;
        exec->env_values = values;
        exec->env_cap = cap;
    }
    exec->env_names[exec->env_count] = ish_strdup(name);
    exec->env_values[exec->env_count] = ish_strdup(value);
    if (!exec->env_names[exec->env_count] || !exec->env_values[exec->env_count]) {
        free(exec->env_names[exec->env_count]);
        free(exec->env_values[exec->env_count]);
        return false;
    }
    exec->env_count++;
    return true;
}

size_t ish_exec_env_count(const IshExec *exec) {
    return exec ? exec->env_count : 0;
}

bool ish_exec_env_entry(const IshExec *exec, size_t index, const char **out_name, const char **out_value) {
    if (!exec || index >= exec->env_count) return false;
    *out_name = exec->env_names[index];
    *out_value = exec->env_values[index];
    return true;
}

bool ish_exec_copy_context(IshExec *dst, const IshExec *src) {
    if (!src) return true;
    if (src->cwd && !ish_exec_set_cwd(dst, src->cwd)) return false;
    for (size_t i = 0; i < src->env_count; i++) {
        if (!ish_exec_env_set(dst, src->env_names[i], src->env_values[i])) return false;
    }
    return true;
}

bool ish_exec_setup_function(IshExec *exec, uint32_t function_index, IshError *err) {
    return push_frame(exec, exec->module, function_index, ish_nil(), 0, 0, err);
}

bool ish_exec_setup_thunk(IshExec *exec, IshValue closure, IshError *err) {
    if (!ish_is_closure(closure)) return ish_error_set(err, ish_span_unknown(NULL), "spawn expects a function value");
    if (!push(exec, closure, err)) return false;
    return call_value(exec, 0, false, err);
}

bool ish_exec_step(IshExec *exec, int64_t budget, IshExecStatus *status, IshValue *out_result, IshValue *out_reason, IshError *err) {
    if (exec->frame_count == 0) {
        *status = ISH_EXEC_DONE;
        *out_result = ish_nil();
        return true;
    }
    IshExec *prev = exec->rt->current_exec;
    exec->rt->current_exec = exec;
    bool ok = vm_run_loop(exec, budget, status, out_result, out_reason, err);
    exec->rt->current_exec = prev;
    return ok;
}

bool ish_exec_take_port_request(IshExec *exec, IshValue *out_graph) {
    if (!exec->has_port_request) return false;
    *out_graph = exec->port_request;
    exec->has_port_request = false;
    exec->port_request = ish_nil();
    return true;
}

bool ish_exec_take_await(IshExec *exec, IshValue *out_port) {
    if (!exec->has_await) return false;
    *out_port = exec->await_port;
    exec->has_await = false;
    exec->await_port = ish_nil();
    return true;
}

bool ish_exec_push_result(IshExec *exec, IshValue value, IshError *err) {
    return push(exec, value, err);
}

void ish_exec_visit_roots(const IshExec *exec, IshRootVisitor visit, void *user) {
    if (!exec) return;
    for (size_t i = 0; i < exec->sp; i++) visit(user, exec->stack[i]);
    for (size_t f = 0; f < exec->frame_count; f++) {
        const Frame *frame = &exec->frames[f];
        visit(user, frame->closure);
        for (uint32_t l = 0; l < frame->local_count; l++) visit(user, frame->locals[l]);
    }
    if (exec->has_raised) visit(user, exec->raised);
    if (exec->has_await) visit(user, exec->await_port);
    if (exec->has_port_request) visit(user, exec->port_request);
}
