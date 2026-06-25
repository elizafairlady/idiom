#include "idiom/vm.h"

#include "idiom/actor.h"
#include "idiom/prims.h"
#include "idiom/regex.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const IdmBytecodeModule *module;
    uint32_t function_index;
    size_t ip;
    size_t base;
    size_t result_base;
    size_t locals_base;
    size_t capture_base;
    uint32_t capture_count;
    uint32_t local_count;
    IdmValue closure;
    IdmEnv *env;
} Frame;

typedef struct {
    size_t catch_ip;
    size_t frame_count;
    size_t sp;
} Handler;

struct IdmExec {
    IdmRuntime *rt;
    const IdmBytecodeModule *module;
    IdmScheduler *sched;
    IdmActor *self;
    IdmValue *stack;
    size_t sp;
    size_t stack_cap;
    bool stack_borrowed;
    Frame *frames;
    size_t frame_count;
    size_t frame_cap;
    bool frames_borrowed;
    IdmVmLimits limits;
    bool has_port_request;
    IdmValue port_request;
    bool has_await;
    IdmValue await_port;
    bool has_port_read_request;
    IdmValue port_read_port;
    const char *port_read_stream;
    size_t port_read_max;
    bool has_port_write_request;
    IdmValue port_write_port;
    IdmValue port_write_data;
    bool has_tty_request;
    bool tty_line_mode;
    bool tty_has_timeout;
    int64_t tty_timeout_ms;
    Handler *handlers;
    size_t handler_count;
    size_t handler_cap;
    bool handlers_borrowed;
    IdmValue raised;
    bool has_raised;
    IdmValue pending_raise;
    bool has_pending_raise;
    IdmEnv **env_save;
    size_t env_save_count;
    size_t env_save_cap;
    IdmEnv *root_env;
    char *cwd;
    char **env_names;
    char **env_values;
    size_t env_count;
    size_t env_cap;
};

typedef struct IdmExec Vm;

IdmVmLimits idm_vm_default_limits(void) {
    IdmVmLimits limits;
    limits.max_stack = 1024u * 64u;
    limits.max_frames = 1024u;
    return limits;
}

static void vm_reset(Vm *vm) {
    if (!vm->frames_borrowed) free(vm->frames);
    if (!vm->stack_borrowed) free(vm->stack);
    if (!vm->handlers_borrowed) free(vm->handlers);
    vm->frames = NULL;
    vm->frame_count = 0;
    vm->frame_cap = 0;
    vm->frames_borrowed = false;
    vm->stack = NULL;
    vm->sp = 0;
    vm->stack_cap = 0;
    vm->stack_borrowed = false;
    vm->handlers = NULL;
    vm->handler_count = 0;
    vm->handler_cap = 0;
    vm->handlers_borrowed = false;
    free(vm->env_save);
    vm->env_save = NULL;
    vm->env_save_count = 0;
    vm->env_save_cap = 0;
    vm->root_env = NULL;
}

static void vm_borrow_storage(Vm *vm, IdmValue *stack, size_t stack_cap, Frame *frames, size_t frame_cap, Handler *handlers, size_t handler_cap) {
    vm->stack = stack;
    vm->stack_cap = stack_cap;
    vm->stack_borrowed = true;
    vm->frames = frames;
    vm->frame_cap = frame_cap;
    vm->frames_borrowed = true;
    vm->handlers = handlers;
    vm->handler_cap = handler_cap;
    vm->handlers_borrowed = true;
}

static bool stack_reserve_slow(Vm *vm, size_t needed, IdmError *err) {
    if (needed > vm->limits.max_stack) return idm_error_set(err, idm_span_unknown(NULL), "VM stack limit exceeded");
    if (needed <= vm->stack_cap) return true;
    size_t cap = vm->stack_cap ? vm->stack_cap * 2u : 64u;
    while (cap < needed) cap *= 2u;
    IdmValue *next = vm->stack_borrowed ? malloc(cap * sizeof(*next)) : realloc(vm->stack, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    if (vm->stack_borrowed && vm->sp != 0) memcpy(next, vm->stack, vm->sp * sizeof(*next));
    vm->stack = next;
    vm->stack_cap = cap;
    vm->stack_borrowed = false;
    return true;
}

static inline bool stack_reserve(Vm *vm, size_t needed, IdmError *err) {
    return needed <= vm->stack_cap ? true : stack_reserve_slow(vm, needed, err);
}

static inline bool push(Vm *vm, IdmValue value, IdmError *err) {
    size_t sp = vm->sp;
    if (sp >= vm->stack_cap && !stack_reserve_slow(vm, sp + 1u, err)) return false;
    vm->stack[sp] = value;
    vm->sp = sp + 1u;
    return true;
}

static bool pop(Vm *vm, IdmValue *out, IdmError *err) {
    if (vm->sp == 0) return idm_error_set(err, idm_span_unknown(NULL), "VM stack underflow");
    *out = vm->stack[--vm->sp];
    return true;
}

static Frame *current_frame(Vm *vm) {
    return vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1u];
}

static const IdmBytecodeModule *closure_module_or_current(Vm *vm, IdmValue closure) {
    (void)vm;
    const IdmBytecodeModule *module = idm_closure_module(closure);
    return module;
}

static bool closure_selector_ready(Vm *vm, IdmValue closure, IdmError *err) {
    const IdmBytecodeModule *module = closure_module_or_current(vm, closure);
    if (!idm_bc_is_finalized(module)) return idm_error_set(err, idm_span_unknown(NULL), "closure module is not finalized");
    if (idm_closure_selector_generation(closure) != module->selector_generation) return idm_error_set(err, idm_span_unknown(NULL), "closure selector is stale");
    return true;
}

static bool frame_reserve(Vm *vm, size_t needed, IdmError *err) {
    if (needed <= vm->frame_cap) return true;
    size_t cap = vm->frame_cap ? vm->frame_cap * 2u : 32u;
    while (cap < needed) cap *= 2u;
    Frame *next = vm->frames_borrowed ? malloc(cap * sizeof(*next)) : realloc(vm->frames, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    if (vm->frames_borrowed && vm->frame_count != 0) memcpy(next, vm->frames, vm->frame_count * sizeof(*next));
    vm->frames = next;
    vm->frame_cap = cap;
    vm->frames_borrowed = false;
    return true;
}

static bool handler_reserve(Vm *vm, size_t needed, IdmError *err) {
    if (needed <= vm->handler_cap) return true;
    size_t cap = vm->handler_cap ? vm->handler_cap * 2u : 8u;
    while (cap < needed) cap *= 2u;
    Handler *next = vm->handlers_borrowed ? malloc(cap * sizeof(*next)) : realloc(vm->handlers, cap * sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    if (vm->handlers_borrowed && vm->handler_count != 0) memcpy(next, vm->handlers, vm->handler_count * sizeof(*next));
    vm->handlers = next;
    vm->handler_cap = cap;
    vm->handlers_borrowed = false;
    return true;
}

static bool generated_clause_name(const char *name) {
    if (!name || !name[0]) return true;
    if (name[0] == '<' || name[0] == '{' || name[0] == '[' || name[0] == '(') return true;
    return strchr(name, ' ') != NULL || strchr(name, '\t') != NULL;
}

static bool push_frame(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, IdmValue closure, size_t base, size_t result_base, IdmError *err) {
    if (!module) module = vm->module;
    if (function_index >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (vm->frame_count >= vm->limits.max_frames) return idm_error_set(err, idm_span_unknown(NULL), "VM frame limit exceeded");
    if (!frame_reserve(vm, vm->frame_count + 1u, err)) return false;
    const IdmBcFunction *fn = &module->functions[function_index];
    Frame *caller = current_frame(vm);
    IdmEnv *root_env = vm->root_env ? vm->root_env : vm->rt->main_env;
    IdmEnv *env = idm_is_closure(closure) ? idm_closure_env(closure) : (caller ? caller->env : root_env);
    if (!env) return idm_error_set(err, idm_span_unknown(NULL), "frame entry requires an explicit runtime environment");
    Frame *frame = &vm->frames[vm->frame_count];
    frame->module = module;
    frame->function_index = function_index;
    frame->ip = fn->entry;
    frame->base = base;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->locals_base = base + fn->arity;
    frame->capture_base = 0;
    frame->capture_count = 0;
    frame->closure = closure;
    frame->env = env;
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    vm->frame_count++;
    return true;
}

static void pop_frame(Vm *vm, Frame *out) {
    *out = vm->frames[--vm->frame_count];
}

static bool raise_value(Vm *vm, IdmValue value, IdmError *err) {
    vm->raised = value;
    vm->has_raised = true;
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (idm_value_write(&buf, value) && buf.data) idm_error_set(err, idm_span_unknown(NULL), "%s", buf.data);
    else idm_error_set(err, idm_span_unknown(NULL), "error raised");
    idm_buf_destroy(&buf);
    return false;
}

static bool raise_arity_error(Vm *vm, uint32_t argc, IdmError *err) {
    const IdmPrimitiveInfo *info = idm_primitive_info(IDM_PRIM_RAISE);
    idm_error_set(err, idm_span_unknown(NULL), "primitive 'raise' arity mismatch: got %u, want 1..1", argc);
    return idm_error_reason(vm->rt, err, "arity", 4,
                            idm_atom(vm->rt, info ? info->name : "raise"),
                            idm_int((int64_t)argc), idm_int(1), idm_int(1));
}

static bool generic_primitive_clause(Vm *vm, uint32_t primitive, uint32_t argc, IdmError *err) {
    if (vm->sp < argc) return idm_error_set(err, idm_span_unknown(NULL), "primitive clause stack underflow");
    IdmValue *args = argc == 0 ? NULL : &vm->stack[vm->sp - argc];
    if ((IdmPrimitive)primitive == IDM_PRIM_RAISE) {
        if (argc != 1u) {
            vm->sp -= argc;
            return raise_arity_error(vm, argc, err);
        }
        IdmValue value = args[0];
        vm->sp -= argc;
        return raise_value(vm, value, err);
    }
    IdmValue out = idm_nil();
    bool ok = idm_prim_invoke(vm->rt, (IdmPrimitive)primitive, args, argc, &out, err);
    vm->sp -= argc;
    if (!ok) return false;
    return push(vm, out, err);
}

static bool op_tty_block(Vm *vm, IdmPrimitive prim, uint32_t argc, IdmError *err) {
    uint32_t want = prim == IDM_PRIM_TTY_READ ? 1u : 0u;
    if (argc != want) {
        idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' arity mismatch: got %u, want %u..%u", idm_primitive_name(prim), argc, want, want);
        return idm_error_reason(vm->rt, err, "arity", 4, idm_atom(vm->rt, idm_primitive_name(prim)), idm_int((int64_t)argc), idm_int((int64_t)want), idm_int((int64_t)want));
    }
    vm->tty_has_timeout = false;
    vm->tty_timeout_ms = 0;
    if (prim == IDM_PRIM_TTY_READ) {
        IdmValue timeout;
        if (!pop(vm, &timeout, err)) return false;
        if (timeout.tag == IDM_VAL_INT && timeout.as.i >= 0) {
            vm->tty_has_timeout = true;
            vm->tty_timeout_ms = timeout.as.i;
        } else if (timeout.tag != IDM_VAL_NIL) {
            idm_error_set(err, idm_span_unknown(NULL), "tty-read timeout must be a non-negative integer or :nil");
            return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "tty-read"), timeout);
        }
    }
    vm->tty_line_mode = prim == IDM_PRIM_TTY_READ_LINE;
    vm->has_tty_request = true;
    return true;
}

static bool port_stream_value(IdmRuntime *rt, IdmValue value, const char **out, IdmError *err) {
    if (value.tag == IDM_VAL_ATOM) {
        const char *text = idm_symbol_text(value.as.symbol);
        if (strcmp(text, "stdout") == 0 || strcmp(text, "stderr") == 0) {
            *out = text;
            return true;
        }
    }
    idm_error_set(err, idm_span_unknown(NULL), "port-read expects :stdout or :stderr");
    return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "port-read"), value);
}

static bool op_port_read_block(Vm *vm, uint32_t argc, IdmError *err) {
    if (argc != 3u) {
        idm_error_set(err, idm_span_unknown(NULL), "primitive 'port-read' arity mismatch: got %u, want 3..3", argc);
        return idm_error_reason(vm->rt, err, "arity", 4, idm_atom(vm->rt, "port-read"), idm_int((int64_t)argc), idm_int(3), idm_int(3));
    }
    IdmValue maxv;
    IdmValue streamv;
    IdmValue port;
    if (!pop(vm, &maxv, err) || !pop(vm, &streamv, err) || !pop(vm, &port, err)) return false;
    if (port.tag != IDM_VAL_PORT) {
        idm_error_set(err, idm_span_unknown(NULL), "port-read expects a port");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-read"), port);
    }
    const char *stream = NULL;
    if (!port_stream_value(vm->rt, streamv, &stream, err)) return false;
    if (maxv.tag != IDM_VAL_INT || maxv.as.i <= 0) {
        idm_error_set(err, idm_span_unknown(NULL), "port-read expects a positive byte count");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-read"), maxv);
    }
    vm->port_read_port = port;
    vm->port_read_stream = stream;
    vm->port_read_max = (size_t)maxv.as.i;
    vm->has_port_read_request = true;
    return true;
}

static bool op_port_write_block(Vm *vm, uint32_t argc, IdmError *err) {
    if (argc != 2u) {
        idm_error_set(err, idm_span_unknown(NULL), "primitive 'port-write' arity mismatch: got %u, want 2..2", argc);
        return idm_error_reason(vm->rt, err, "arity", 4, idm_atom(vm->rt, "port-write"), idm_int((int64_t)argc), idm_int(2), idm_int(2));
    }
    IdmValue data;
    IdmValue port;
    if (!pop(vm, &data, err) || !pop(vm, &port, err)) return false;
    if (port.tag != IDM_VAL_PORT) {
        idm_error_set(err, idm_span_unknown(NULL), "port-write expects a port");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-write"), port);
    }
    if (data.tag != IDM_VAL_STRING) {
        idm_error_set(err, idm_span_unknown(NULL), "port-write expects a string");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-write"), data);
    }
    vm->port_write_port = port;
    vm->port_write_data = data;
    vm->has_port_write_request = true;
    return true;
}

static bool call_value(Vm *vm, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err);

static bool primitive_actor_context(Vm *vm, IdmPrimitive primitive, IdmError *err) {
    if (vm->self && vm->sched) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", idm_primitive_name(primitive));
}

static bool primitive_exact_arity(IdmPrimitive primitive, uint32_t argc, uint32_t want, IdmError *err) {
    if (argc == want) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' arity mismatch: got %u, want %u..%u", idm_primitive_name(primitive), argc, want, want);
}

static bool execute_apply_primitive(Vm *vm, uint32_t argc, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!primitive_exact_arity(IDM_PRIM_APPLY, argc, 2u, err)) return false;
    IdmValue callee;
    IdmValue arglist;
    if (!pop(vm, &callee, err) || !pop(vm, &arglist, err)) return false;
    size_t apply_argc = 0;
    IdmValue cursor = arglist;
    while (idm_is_pair(cursor)) {
        apply_argc++;
        cursor = idm_cdr(cursor, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cursor)) return idm_error_set(err, idm_span_unknown(NULL), "apply expects a proper list of arguments");
    if (apply_argc > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "apply argument list too long");
    if (!push(vm, callee, err)) return false;
    cursor = arglist;
    for (size_t i = 0; i < apply_argc; i++) {
        IdmValue item = idm_car(cursor, err);
        if (err && err->present) return false;
        if (!push(vm, item, err)) return false;
        cursor = idm_cdr(cursor, err);
        if (err && err->present) return false;
    }
    return call_value(vm, (uint32_t)apply_argc, false, status, out_reason, err);
}

static bool execute_actor_primitive(Vm *vm, IdmPrimitive primitive, uint32_t argc, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!primitive_actor_context(vm, primitive, err)) return false;
    switch (primitive) {
        case IDM_PRIM_SELF:
            if (!primitive_exact_arity(primitive, argc, 0u, err)) return false;
            return push(vm, idm_pid(idm_actor_pid(vm->self)), err);
        case IDM_PRIM_SPAWN: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue thunk;
            if (!pop(vm, &thunk, err)) return false;
            if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a function value");
            IdmValue pid = idm_nil();
            if (!idm_sched_spawn(vm->sched, thunk, vm, &pid, err)) return false;
            return push(vm, pid, err);
        }
        case IDM_PRIM_SPAWN_LINK: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue thunk;
            if (!pop(vm, &thunk, err)) return false;
            if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-link expects a function value");
            IdmValue pid = idm_nil();
            bool self_exits = false;
            IdmValue exit_reason = idm_nil();
            if (!idm_sched_spawn_link(vm->sched, thunk, vm, vm->self, &pid, &self_exits, &exit_reason, err)) return false;
            if (self_exits) {
                if (!status) return idm_error_set(err, idm_span_unknown(NULL), "spawn-link exit requires a schedulable VM step");
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = exit_reason;
                return true;
            }
            return push(vm, pid, err);
        }
        case IDM_PRIM_SPAWN_MONITOR: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue thunk;
            if (!pop(vm, &thunk, err)) return false;
            if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-monitor expects a function value");
            IdmValue pid = idm_nil();
            IdmValue ref = idm_nil();
            if (!idm_sched_spawn_monitor(vm->sched, thunk, vm, vm->self, &pid, &ref, err)) return false;
            IdmValue items[2] = { pid, ref };
            IdmValue result = idm_tuple(vm->rt, items, 2u, err);
            if (result.tag != IDM_VAL_TUPLE) return false;
            return push(vm, result, err);
        }
        case IDM_PRIM_SEND: {
            if (!primitive_exact_arity(primitive, argc, 2u, err)) return false;
            IdmValue msg;
            IdmValue target;
            if (!pop(vm, &msg, err) || !pop(vm, &target, err)) return false;
            if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "send expects a pid target");
            idm_sched_send(vm->sched, target.as.id, msg);
            return push(vm, msg, err);
        }
        case IDM_PRIM_EXIT: {
            if (argc > 2u) return idm_error_set(err, idm_span_unknown(NULL), "primitive 'exit' arity mismatch: got %u, want 0..2", argc);
            if (argc == 2u) {
                IdmValue reason;
                IdmValue target;
                if (!pop(vm, &reason, err) || !pop(vm, &target, err)) return false;
                if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "exit signal expects a pid target");
                bool self_exits = false;
                IdmValue exit_reason = idm_nil();
                if (!idm_sched_exit_signal(vm->sched, vm->self, target.as.id, reason, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    if (!status) return idm_error_set(err, idm_span_unknown(NULL), "exit signal requires a schedulable VM step");
                    *status = IDM_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                return push(vm, idm_atom(vm->rt, "ok"), err);
            }
            IdmValue reason = idm_int(0);
            if (argc == 1u && !pop(vm, &reason, err)) return false;
            if (!status) return idm_error_set(err, idm_span_unknown(NULL), "exit requires a schedulable VM step");
            *status = IDM_EXEC_EXIT;
            if (out_reason) *out_reason = reason;
            return true;
        }
        case IDM_PRIM_LINK: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue target;
            if (!pop(vm, &target, err)) return false;
            if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "link expects a pid");
            bool self_exits = false;
            IdmValue exit_reason = idm_nil();
            if (!idm_sched_link(vm->sched, vm->self, target.as.id, &self_exits, &exit_reason, err)) return false;
            if (self_exits) {
                if (!status) return idm_error_set(err, idm_span_unknown(NULL), "link exit requires a schedulable VM step");
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = exit_reason;
                return true;
            }
            return push(vm, idm_atom(vm->rt, "ok"), err);
        }
        case IDM_PRIM_UNLINK: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue target;
            if (!pop(vm, &target, err)) return false;
            if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "unlink expects a pid");
            idm_sched_unlink(vm->sched, vm->self, target.as.id);
            return push(vm, idm_atom(vm->rt, "ok"), err);
        }
        case IDM_PRIM_MONITOR: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue target;
            if (!pop(vm, &target, err)) return false;
            if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "monitor expects a pid");
            IdmValue ref = idm_nil();
            if (!idm_sched_monitor(vm->sched, vm->self, target.as.id, &ref, err)) return false;
            return push(vm, ref, err);
        }
        case IDM_PRIM_DEMONITOR: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue ref;
            if (!pop(vm, &ref, err)) return false;
            if (ref.tag != IDM_VAL_REF) return idm_error_set(err, idm_span_unknown(NULL), "demonitor expects a reference");
            idm_sched_demonitor(vm->sched, vm->self, ref);
            return push(vm, idm_atom(vm->rt, "ok"), err);
        }
        case IDM_PRIM_TRAP_EXIT: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            IdmValue flag;
            if (!pop(vm, &flag, err)) return false;
            bool previous = idm_actor_trap_exit_get(vm->self);
            idm_actor_trap_exit_set(vm->self, idm_value_ok(flag));
            return push(vm, idm_atom(vm->rt, previous ? "true" : "false"), err);
        }
        case IDM_PRIM_EXEC: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            if (!status) return idm_error_set(err, idm_span_unknown(NULL), "exec requires a schedulable VM step");
            IdmValue graph;
            if (!pop(vm, &graph, err)) return false;
            vm->port_request = graph;
            vm->has_port_request = true;
            *status = IDM_EXEC_LAUNCH_PORT;
            return true;
        }
        case IDM_PRIM_AWAIT: {
            if (!primitive_exact_arity(primitive, argc, 1u, err)) return false;
            if (!status) return idm_error_set(err, idm_span_unknown(NULL), "await requires a schedulable VM step");
            IdmValue port;
            if (!pop(vm, &port, err)) return false;
            if (port.tag != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "await expects a port");
            vm->await_port = port;
            vm->has_await = true;
            *status = IDM_EXEC_BLOCK_AWAIT;
            return true;
        }
        default:
            return false;
    }
}

static bool execute_primitive_clause(Vm *vm, uint32_t primitive, uint32_t argc, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (vm->self && vm->sched && (primitive == IDM_PRIM_TTY_READ || primitive == IDM_PRIM_TTY_READ_LINE)) {
        if (!status) return idm_error_set(err, idm_span_unknown(NULL), "blocking primitive '%s' requires a schedulable VM step", idm_primitive_name((IdmPrimitive)primitive));
        if (!op_tty_block(vm, (IdmPrimitive)primitive, argc, err)) return false;
        *status = IDM_EXEC_BLOCK_TTY;
        return true;
    }
    if (vm->self && vm->sched && primitive == IDM_PRIM_PORT_READ) {
        if (!status) return idm_error_set(err, idm_span_unknown(NULL), "blocking primitive 'port-read' requires a schedulable VM step");
        if (!op_port_read_block(vm, argc, err)) return false;
        *status = IDM_EXEC_BLOCK_PORT_READ;
        return true;
    }
    if (vm->self && vm->sched && primitive == IDM_PRIM_PORT_WRITE) {
        if (!status) return idm_error_set(err, idm_span_unknown(NULL), "blocking primitive 'port-write' requires a schedulable VM step");
        if (!op_port_write_block(vm, argc, err)) return false;
        *status = IDM_EXEC_BLOCK_PORT_WRITE;
        return true;
    }
    switch ((IdmPrimitive)primitive) {
        case IDM_PRIM_APPLY:
            return execute_apply_primitive(vm, argc, status, out_reason, err);
        case IDM_PRIM_SELF:
        case IDM_PRIM_SPAWN:
        case IDM_PRIM_SPAWN_LINK:
        case IDM_PRIM_SPAWN_MONITOR:
        case IDM_PRIM_SEND:
        case IDM_PRIM_EXIT:
        case IDM_PRIM_LINK:
        case IDM_PRIM_UNLINK:
        case IDM_PRIM_MONITOR:
        case IDM_PRIM_DEMONITOR:
        case IDM_PRIM_TRAP_EXIT:
        case IDM_PRIM_EXEC:
        case IDM_PRIM_AWAIT:
            return execute_actor_primitive(vm, (IdmPrimitive)primitive, argc, status, out_reason, err);
        default:
            break;
    }
    return generic_primitive_clause(vm, primitive, argc, err);
}

#define IDM_GUARD_BUDGET (1 << 20)
#define IDM_GUARD_STACK_SLOTS 32u
#define IDM_GUARD_FRAME_SLOTS 4u
#define IDM_GUARD_HANDLER_SLOTS 2u

static bool vm_run_loop(Vm *vm, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool vm_run_loop_inner(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool enter_closure_clause(Vm *vm, IdmValue callee, uint32_t function_index, size_t arg_base, size_t result_base, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err);

static bool vm_run_call(Vm *vm, int64_t budget, IdmExecStatus *out_status, IdmValue *out, IdmError *err) {
    IdmRuntime *rt = vm->rt;
    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue result = idm_nil();
    IdmValue reason = idm_nil();
    bool ok = vm_run_loop(vm, budget, &status, &result, &reason, err);
    vm_reset(vm);
    if (out_status) *out_status = status;
    if (!ok) return false;
    if (status == IDM_EXEC_EXIT) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        idm_error_describe(rt, reason, &buf);
        if (buf.data) idm_error_set(err, idm_span_unknown(NULL), "exited with reason %s", buf.data);
        else idm_error_set(err, idm_span_unknown(NULL), "exited outside the scheduler");
        idm_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

static bool init_pattern_locals(Vm *vm, Frame *frame, const IdmBcFunction *fn, const IdmPatternBindings *bindings, IdmError *err) {
    if (fn->pattern_local_count == 0) return true;
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) {
        uint32_t slot = fn->pattern_locals[i].slot;
        if (slot >= frame->local_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern local slot out of bounds");
        const IdmValue *bound = bindings ? idm_pattern_bindings_get_slot(bindings, slot) : NULL;
        if (!bound && bindings) bound = idm_pattern_bindings_get(bindings, fn->pattern_locals[i].name);
        if (!bound) return idm_error_set(err, idm_span_unknown(NULL), "pattern binding '%s' missing", fn->pattern_locals[i].name);
        vm->stack[frame->locals_base + slot] = *bound;
    }
    return true;
}

typedef struct {
    const IdmValue *values;
    uint32_t count;
} ClauseArgs;

static ClauseArgs clause_args_from(const IdmValue *values, uint32_t count) {
    ClauseArgs args;
    args.values = values;
    args.count = count;
    return args;
}

static ClauseArgs clause_args_from_stack(Vm *vm, size_t base, uint32_t count) {
    return clause_args_from(count == 0 ? NULL : &vm->stack[base], count);
}

typedef struct {
    bool matched;
    uint32_t function_index;
    IdmPatternBindings bindings;
    bool has_bindings;
    bool guard_budget_exhausted;
} ClauseSelection;

static void clause_selection_init(ClauseSelection *selection) {
    selection->matched = false;
    selection->function_index = UINT32_MAX;
    idm_pattern_bindings_init(&selection->bindings);
    selection->has_bindings = false;
    selection->guard_budget_exhausted = false;
}

static void clause_selection_destroy(ClauseSelection *selection) {
    idm_pattern_bindings_destroy(&selection->bindings);
    selection->matched = false;
    selection->function_index = UINT32_MAX;
    selection->has_bindings = false;
    selection->guard_budget_exhausted = false;
}

static void clause_selection_move(ClauseSelection *dst, ClauseSelection *src) {
    clause_selection_destroy(dst);
    dst->matched = src->matched;
    dst->function_index = src->function_index;
    idm_pattern_bindings_move(&dst->bindings, &src->bindings);
    dst->has_bindings = src->has_bindings;
    dst->guard_budget_exhausted = src->guard_budget_exhausted;
    src->matched = false;
    src->function_index = UINT32_MAX;
    src->has_bindings = false;
    src->guard_budget_exhausted = false;
}

static bool run_guard_function(Vm *caller, const IdmBytecodeModule *callee_module, IdmValue callee, const IdmBcFunction *candidate, const IdmPatternBindings *bindings, ClauseArgs args, bool *out_pass, bool *out_exhausted, IdmError *err) {
    *out_pass = true;
    if (!candidate->has_guard) return true;
    if (candidate->guard_function >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    if (!idm_arity_accepts(&callee_module->functions[candidate->guard_function].call_arity, args.count)) return idm_error_set(err, idm_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    IdmValue guard_stack[IDM_GUARD_STACK_SLOTS];
    Frame guard_frames[IDM_GUARD_FRAME_SLOTS];
    Handler guard_handlers[IDM_GUARD_HANDLER_SLOTS];
    vm_borrow_storage(&guard_vm, guard_stack, IDM_GUARD_STACK_SLOTS, guard_frames, IDM_GUARD_FRAME_SLOTS, guard_handlers, IDM_GUARD_HANDLER_SLOTS);
    guard_vm.rt = caller->rt;
    guard_vm.module = callee_module;
    guard_vm.limits = caller->limits;
    if (!push(&guard_vm, callee, err)) { vm_reset(&guard_vm); return false; }
    for (uint32_t i = 0; i < args.count; i++) {
        if (!push(&guard_vm, args.values[i], err)) { vm_reset(&guard_vm); return false; }
    }
    if (!enter_closure_clause(&guard_vm, callee, candidate->guard_function, 1u, 0u, args.count, bindings, false, NULL, NULL, err)) { vm_reset(&guard_vm); return false; }

    IdmError guard_err;
    idm_error_init(&guard_err);
    IdmValue result = idm_nil();
    IdmExecStatus status = IDM_EXEC_DONE;
    if (!vm_run_call(&guard_vm, IDM_GUARD_BUDGET, &status, &result, &guard_err)) {
        idm_error_clear(&guard_err);
        *out_pass = false;
        return true;
    }
    idm_error_clear(&guard_err);
    if (status == IDM_EXEC_YIELD) {
        if (out_exhausted) *out_exhausted = true;
        *out_pass = false;
        return true;
    }
    *out_pass = idm_value_ok(result);
    return true;
}

typedef struct {
    Vm *vm;
    const IdmBytecodeModule *callee_module;
    IdmValue callee;
    ClauseArgs args;
    bool exhausted;
} SelectorGuardCtx;

static bool selector_guard(void *user, uint32_t function_index, const IdmPatternBindings *bindings, bool *out_pass, IdmError *err) {
    SelectorGuardCtx *ctx = user;
    if (function_index >= ctx->callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "selector chose invalid function %u", function_index);
    const IdmBcFunction *candidate = &ctx->callee_module->functions[function_index];
    return run_guard_function(ctx->vm, ctx->callee_module, ctx->callee, candidate, bindings, ctx->args, out_pass, &ctx->exhausted, err);
}

static bool select_closure_clause(Vm *vm, IdmValue callee, ClauseArgs args, ClauseSelection *selection, IdmError *err) {
    clause_selection_init(selection);
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    if (!closure_selector_ready(vm, callee, err)) {
        clause_selection_destroy(selection);
        return false;
    }
    IdmPatternSelector *selector = idm_closure_selector(callee);
    if (!selector) {
        clause_selection_destroy(selection);
        return idm_error_set(err, idm_span_unknown(NULL), "closure has no clause selector");
    }
    SelectorGuardCtx guard_ctx;
    guard_ctx.vm = vm;
    guard_ctx.callee_module = callee_module;
    guard_ctx.callee = callee;
    guard_ctx.args = args;
    guard_ctx.exhausted = false;
    bool sel_ok = idm_pattern_selector_select(vm->rt, selector, args.values, args.count, selector_guard, &guard_ctx, &selection->function_index, &selection->bindings, &selection->has_bindings, &selection->matched, err);
    selection->guard_budget_exhausted = guard_ctx.exhausted;
    if (!sel_ok) clause_selection_destroy(selection);
    return sel_ok;
}

static bool raise_no_clause(Vm *vm, const char *name, ClauseArgs args, IdmError *err) {
    bool generated_name = generated_clause_name(name);
    const char *display_name = generated_name ? "<fn>" : name;
    const char *reason_name = generated_name ? "fn" : name;
    IdmBuffer args_buf;
    idm_buf_init(&args_buf);
    bool rendered = true;
    for (uint32_t i = 0; i < args.count && rendered && args_buf.len < 160u; i++) {
        if (i != 0) rendered = idm_buf_append(&args_buf, " ");
        if (rendered) rendered = idm_value_write(&args_buf, args.values[i]);
    }
    IdmBuffer message_buf;
    idm_buf_init(&message_buf);
    rendered = rendered &&
               idm_buf_appendf(&message_buf, "no clause of '%s' matches (%.*s%s)",
                               display_name,
                               (int)(args_buf.len < 160u ? args_buf.len : 160u), args_buf.data ? args_buf.data : "",
                               args_buf.len > 160u ? " ..." : "");
    idm_error_set(err, idm_span_unknown(NULL), "%s", rendered && message_buf.data ? message_buf.data : "no matching function clause");
    idm_buf_destroy(&args_buf);
    IdmValue args_tuple = idm_tuple(vm->rt, args.count == 0 ? NULL : args.values, args.count, NULL);
    idm_buf_destroy(&message_buf);
    return idm_error_reason(vm->rt, err, "no-clause", 2, idm_atom(vm->rt, reason_name), args_tuple);
}

typedef struct {
    bool has_known;
    bool accepts;
    uint32_t min;
    uint32_t max;
} CallArityInfo;

static void call_arity_info_add(CallArityInfo *info, const IdmArity *arity, uint32_t argc) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return;
    if (!info->has_known) {
        info->min = arity->min;
        info->max = arity->max;
        info->has_known = true;
    } else {
        if (arity->min < info->min) info->min = arity->min;
        if (arity->max > info->max) info->max = arity->max;
    }
    if (idm_arity_accepts(arity, argc)) info->accepts = true;
}

static CallArityInfo closure_arity_info(Vm *vm, IdmValue closure, uint32_t argc) {
    CallArityInfo info = {0};
    const IdmBytecodeModule *m = closure_module_or_current(vm, closure);
    if (!m) return info;
    size_t n = idm_closure_entry_count(closure);
    if (n == 0) {
        uint32_t idx = idm_closure_function_index(closure);
        if (idx < m->function_count) call_arity_info_add(&info, &m->functions[idx].call_arity, argc);
        return info;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = idm_closure_entry(closure, i, NULL);
        if (idx < m->function_count) call_arity_info_add(&info, &m->functions[idx].call_arity, argc);
    }
    return info;
}

static bool raise_call_arity(Vm *vm, const char *name, uint32_t argc, uint32_t min, uint32_t max, IdmError *err) {
    bool generated_name = generated_clause_name(name);
    const char *display_name = generated_name ? "<fn>" : name;
    const char *reason_name = generated_name ? "fn" : name;
    idm_error_set(err, idm_span_unknown(NULL), "function '%s' arity mismatch: got %u, want %u..%u", display_name, argc, min, max);
    return idm_error_reason(vm->rt, err, "arity", 4,
                            idm_atom(vm->rt, reason_name),
                            idm_int((int64_t)argc), idm_int((int64_t)min), idm_int((int64_t)max));
}

static bool raise_guard_budget(const char *name, IdmError *err) {
    const char *display = (name && name[0] && !generated_clause_name(name)) ? name : "<fn>";
    return idm_error_set(err, idm_span_unknown(NULL), "guard of '%s' exceeded its budget of %d reductions", display, IDM_GUARD_BUDGET);
}

static bool enter_closure_clause(Vm *vm, IdmValue callee, uint32_t function_index, size_t arg_base, size_t result_base, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    if (!callee_module) return idm_error_set(err, idm_span_unknown(NULL), "closure has no bytecode module");
    if (function_index >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "clause function index %u out of bounds", function_index);
    const IdmBcFunction *fn = &callee_module->functions[function_index];
    if (fn->primitive_backed) {
        if (bindings) return idm_error_set(err, idm_span_unknown(NULL), "primitive-backed clause cannot bind patterns");
        size_t dst_base = result_base;
        if (tail) {
            Frame *frame = current_frame(vm);
            if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail primitive clause entry without frame");
            dst_base = frame->result_base;
        }
        if (argc != 0) memmove(&vm->stack[dst_base], &vm->stack[arg_base], argc * sizeof(*vm->stack));
        vm->sp = dst_base + argc;
        return execute_primitive_clause(vm, fn->primitive, argc, status, out_reason, err);
    }
    if (!tail) {
        if (!push_frame(vm, callee_module, function_index, callee, arg_base, result_base, err)) return false;
        return init_pattern_locals(vm, current_frame(vm), fn, bindings, err);
    }
    Frame *frame = current_frame(vm);
    if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry without frame");
    result_base = frame->result_base;
    if (callee_module == frame->module && function_index == frame->function_index && callee.tag == IDM_VAL_CLOSURE && frame->closure.tag == IDM_VAL_CLOSURE && callee.as.obj == frame->closure.as.obj && bindings == NULL && fn->pattern_local_count == 0 && fn->arity == argc) {
        if (argc != 0) memmove(&vm->stack[result_base], &vm->stack[arg_base], argc * sizeof(*vm->stack));
        frame->ip = fn->entry;
        frame->base = result_base;
        frame->locals_base = result_base + fn->arity;
        if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
        vm->sp = frame->locals_base + fn->local_count;
        return true;
    }
    frame->function_index = function_index;
    frame->module = callee_module;
    frame->ip = fn->entry;
    frame->base = result_base;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->locals_base = result_base + fn->arity;
    frame->capture_base = 0;
    frame->capture_count = 0;
    frame->closure = callee;
    frame->env = idm_is_closure(callee) ? idm_closure_env(callee) : frame->env;
    if (!frame->env) return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry requires an explicit runtime environment");
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    if (argc != 0) memmove(&vm->stack[result_base], &vm->stack[arg_base], argc * sizeof(*vm->stack));
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    return init_pattern_locals(vm, frame, fn, bindings, err);
}

static bool execute_call_op(Vm *vm, Frame *frame, const IdmBytecodeModule *module, size_t instr_ip, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    (void)instr_ip;
    uint32_t argc = module->code[frame->ip++];
    bool tail = module->code[frame->ip++] != 0;
    return call_value(vm, argc, tail, status, out_reason, err);
}

static bool closure_accepts_argc(Vm *vm, IdmValue closure, uint32_t argc) {
    return closure_arity_info(vm, closure, argc).accepts;
}

static bool call_closure_clause(Vm *vm, IdmValue callee, size_t arg_base, size_t result_base, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!idm_is_closure(callee)) {
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    ClauseSelection selected;
    ClauseArgs args = clause_args_from_stack(vm, arg_base, argc);
    if (!select_closure_clause(vm, callee, args, &selected, err)) return false;
    if (!selected.matched) {
        const IdmBytecodeModule *cm = closure_module_or_current(vm, callee);
        const char *cname = NULL;
        uint32_t first_entry = idm_closure_entry_count(callee) != 0 ? idm_closure_entry(callee, 0, NULL) : idm_closure_function_index(callee);
        if (cm && first_entry < cm->function_count) cname = cm->functions[first_entry].name;
        CallArityInfo arity = closure_arity_info(vm, callee, argc);
        bool budget_exhausted = selected.guard_budget_exhausted;
        clause_selection_destroy(&selected);
        if (budget_exhausted) return raise_guard_budget(cname, err);
        if (arity.has_known && !arity.accepts) return raise_call_arity(vm, cname, argc, arity.min, arity.max, err);
        return raise_no_clause(vm, cname, args, err);
    }
    bool entered = enter_closure_clause(vm, callee, selected.function_index, arg_base, result_base, argc, selected.has_bindings ? &selected.bindings : NULL, tail, status, out_reason, err);
    clause_selection_destroy(&selected);
    return entered;
}

static bool call_value(Vm *vm, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (vm->sp < (size_t)argc + 1u) return idm_error_set(err, idm_span_unknown(NULL), "CALL stack underflow");
    size_t closure_index = vm->sp - (size_t)argc - 1u;
    IdmValue callee = vm->stack[closure_index];
    if (!idm_is_closure(callee)) {
        if (argc == 0) return true;
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    if (argc == 0 && !closure_accepts_argc(vm, callee, 0)) {
        if (!closure_selector_ready(vm, callee, err)) return false;
        return true;
    }
    return call_closure_clause(vm, callee, closure_index + 1u, closure_index, argc, tail, status, out_reason, err);
}

static bool require_actor(Vm *vm, IdmError *err) {
    if (!vm->self || !vm->sched) return idm_error_set(err, idm_span_unknown(NULL), "actor operation requires a running actor");
    return true;
}

static bool closure_is_guard_free(Vm *vm, IdmValue closure) {
    const IdmBytecodeModule *m = closure_module_or_current(vm, closure);
    size_t n = idm_closure_entry_count(closure);
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = idm_closure_entry(closure, i, NULL);
        if (idx < m->function_count && m->functions[idx].has_guard) return false;
    }
    return n > 0;
}

typedef struct {
    bool matched;
    size_t matched_index;
    ClauseSelection clause;
    bool guard_free;
} ReceiveSelection;

static void receive_selection_init(ReceiveSelection *selection) {
    selection->matched = false;
    selection->matched_index = 0;
    clause_selection_init(&selection->clause);
    selection->guard_free = false;
}

static void receive_selection_destroy(ReceiveSelection *selection) {
    clause_selection_destroy(&selection->clause);
}

typedef struct {
    IdmActor *actor;
    size_t index;
    size_t limit;
} ReceiveCursor;

static ReceiveCursor receive_cursor_open(IdmActor *actor, const IdmBytecodeModule *module, size_t instr_ip) {
    ReceiveCursor cursor;
    cursor.actor = actor;
    cursor.index = idm_actor_recv_start(actor, module, instr_ip);
    cursor.limit = idm_actor_mailbox_count(actor);
    return cursor;
}

static bool receive_cursor_next(ReceiveCursor *cursor, size_t *out_index, IdmValue *out) {
    while (cursor->index < cursor->limit) {
        size_t index = cursor->index++;
        if (!idm_actor_mailbox_peek(cursor->actor, index, out)) break;
        *out_index = index;
        return true;
    }
    return false;
}

static void receive_cursor_no_match(ReceiveCursor *cursor) {
    idm_actor_recv_set_cursor(cursor->actor, cursor->limit);
}

static void receive_cursor_matched(ReceiveCursor *cursor, size_t matched_index, bool guard_free) {
    idm_actor_recv_matched(cursor->actor, guard_free ? matched_index : 0u);
}

static bool select_receive_message(Vm *vm, IdmValue closure, ReceiveCursor *cursor, ReceiveSelection *selection, IdmError *err) {
    receive_selection_init(selection);
    selection->guard_free = closure_is_guard_free(vm, closure);
    size_t index = 0;
    IdmValue msg = idm_nil();
    while (receive_cursor_next(cursor, &index, &msg)) {
        ClauseSelection candidate;
        ClauseArgs args = clause_args_from(&msg, 1u);
        bool ok = select_closure_clause(vm, closure, args, &candidate, err);
        if (!ok) {
            receive_selection_destroy(selection);
            return false;
        }
        if (candidate.matched) {
            selection->matched = true;
            selection->matched_index = index;
            clause_selection_move(&selection->clause, &candidate);
            clause_selection_destroy(&candidate);
            return true;
        }
        clause_selection_destroy(&candidate);
    }
    return true;
}

static bool op_recv(Vm *vm, Frame *frame, size_t instr_ip, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t timeout_label = module->code[frame->ip++];
    bool tail = module->code[frame->ip++] != 0;
    if (!require_actor(vm, err)) return false;
    if (vm->sp < 2u) return idm_error_set(err, idm_span_unknown(NULL), "RECV stack underflow");
    IdmValue closure = vm->stack[vm->sp - 1u];
    IdmValue timeout = vm->stack[vm->sp - 2u];
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "receive requires clause closure");

    ReceiveCursor cursor = receive_cursor_open(vm->self, module, instr_ip);
    ReceiveSelection selection;
    if (!select_receive_message(vm, closure, &cursor, &selection, err)) return false;

    if (selection.matched) {
        IdmValue removed;
        if (!idm_actor_mailbox_remove(vm->self, selection.matched_index, &removed)) {
            receive_selection_destroy(&selection);
            return idm_error_set(err, idm_span_unknown(NULL), "mailbox message vanished during receive");
        }
        receive_cursor_matched(&cursor, selection.matched_index, selection.guard_free);
        vm->sp -= 2u;
        if (!push(vm, closure, err) || !push(vm, removed, err)) {
            receive_selection_destroy(&selection);
            return false;
        }
        size_t closure_index = vm->sp - 2u;
        bool ok = enter_closure_clause(vm, closure, selection.clause.function_index, closure_index + 1u, closure_index, 1u, selection.clause.has_bindings ? &selection.clause.bindings : NULL, tail, status, out_reason, err);
        receive_selection_destroy(&selection);
        return ok;
    }

    receive_cursor_no_match(&cursor);
    receive_selection_destroy(&selection);
    IdmRecvDecision decision = IDM_RECV_BLOCK;
    if (!idm_actor_recv_no_match(vm->self, timeout, &decision, err)) return false;
    if (decision == IDM_RECV_TIMEOUT) {
        vm->sp -= 2u;
        frame->ip = timeout_label;
        return true;
    }
    frame->ip = instr_ip;
    *status = IDM_EXEC_BLOCK_RECEIVE;
    return true;
}

static const char *module_const_text(const IdmBytecodeModule *module, uint32_t index, const char *what, IdmError *err) {
    if (index >= module->const_count) {
        idm_error_set(err, idm_span_unknown(NULL), "%s constant %u out of bounds", what, index);
        return NULL;
    }
    IdmValue value = module->constants[index];
    if (value.tag == IDM_VAL_ATOM || value.tag == IDM_VAL_WORD) return idm_symbol_text(value.as.symbol);
    if (value.tag == IDM_VAL_STRING) return idm_string_bytes(value);
    idm_error_set(err, idm_span_unknown(NULL), "%s constant must be a name", what);
    return NULL;
}

static bool module_const_value(const IdmBytecodeModule *module, uint32_t index, const char *what, IdmValue *out, IdmError *err) {
    if (index >= module->const_count) {
        return idm_error_set(err, idm_span_unknown(NULL), "%s constant %u out of bounds", what, index);
    }
    *out = module->constants[index];
    return true;
}

static bool trait_name_value_text(IdmValue value, const char **out) {
    if (value.tag == IDM_VAL_ATOM || value.tag == IDM_VAL_WORD) {
        *out = idm_symbol_text(value.as.symbol);
        return true;
    }
    if (value.tag == IDM_VAL_STRING) {
        *out = idm_string_bytes(value);
        return true;
    }
    return false;
}

static size_t trait_candidate_count(IdmValue traits) {
    if (traits.tag == IDM_VAL_TUPLE || traits.tag == IDM_VAL_VECTOR) return idm_sequence_count(traits);
    return 1u;
}

static bool trait_candidate_at(IdmValue traits, size_t index, const char **out, IdmError *err) {
    IdmValue value = traits;
    if (traits.tag == IDM_VAL_TUPLE || traits.tag == IDM_VAL_VECTOR) value = idm_sequence_item(traits, index, err);
    if (err && err->present) return false;
    if (trait_name_value_text(value, out)) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "method call trait candidate must be a name");
}

static bool append_trait_candidates(IdmBuffer *buf, IdmValue traits, IdmError *err) {
    size_t count = trait_candidate_count(traits);
    for (size_t i = 0; i < count; i++) {
        const char *trait = NULL;
        if (!trait_candidate_at(traits, i, &trait, err)) return false;
        if (i != 0 && !idm_buf_append(buf, ", ")) return false;
        if (!idm_buf_append(buf, trait)) return false;
    }
    return true;
}

static IdmArity read_arity_operands(const IdmBytecodeModule *module, Frame *frame) {
    IdmArity arity;
    arity.kind = (IdmArityKind)module->code[frame->ip++];
    arity.min = module->code[frame->ip++];
    arity.max = module->code[frame->ip++];
    uint64_t lo = module->code[frame->ip++];
    uint64_t hi = module->code[frame->ip++];
    arity.mask = lo | (hi << 32);
    return arity;
}

static bool op_define_trait(Vm *vm, Frame *frame, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t trait_const = module->code[frame->ip++];
    uint32_t requirement_count = module->code[frame->ip++];
    uint32_t method_count = module->code[frame->ip++];
    const char *trait = module_const_text(module, trait_const, "DEFINE_TRAIT trait", err);
    if (!trait) return false;
    if (vm->sp < method_count) return idm_error_set(err, idm_span_unknown(NULL), "DEFINE_TRAIT stack underflow");
    IdmTraitRequirementSpec *requirements = requirement_count == 0 ? NULL : calloc(requirement_count, sizeof(*requirements));
    if (requirement_count != 0 && !requirements) return idm_error_oom(err, idm_span_unknown(NULL));
    IdmTraitMethodSpec *specs = method_count == 0 ? NULL : calloc(method_count, sizeof(*specs));
    if (method_count != 0 && !specs) {
        free(requirements);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    bool ok = true;
    for (uint32_t i = 0; i < requirement_count && ok; i++) {
        uint32_t requirement_const = module->code[frame->ip++];
        const char *requirement = module_const_text(module, requirement_const, "DEFINE_TRAIT requirement", err);
        if (!requirement) { ok = false; break; }
        requirements[i].name = requirement;
    }
    for (uint32_t i = 0; i < method_count && ok; i++) {
        uint32_t method_const = module->code[frame->ip++];
        IdmArity arity = read_arity_operands(module, frame);
        uint32_t has_default = module->code[frame->ip++];
        const char *method = module_const_text(module, method_const, "DEFINE_TRAIT method", err);
        if (!method) { ok = false; break; }
        specs[i].name = method;
        specs[i].arity = arity;
        specs[i].has_default = has_default != 0;
    }
    if (!ok) { free(requirements); free(specs); return false; }
    for (uint32_t i = method_count; i > 0; i--) {
        IdmValue default_impl = idm_nil();
        if (!pop(vm, &default_impl, err)) { free(requirements); free(specs); return false; }
        specs[i - 1u].default_impl = default_impl;
    }
    ok = idm_trait_define(vm->rt, trait, requirements, requirement_count, specs, method_count, err);
    free(requirements);
    free(specs);
    if (!ok) return false;
    return push(vm, idm_atom(vm->rt, "ok"), err);
}

static bool op_implement_trait(Vm *vm, Frame *frame, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t trait_const = module->code[frame->ip++];
    uint32_t type_const = module->code[frame->ip++];
    uint32_t provider_const = module->code[frame->ip++];
    uint32_t provider_key_const = module->code[frame->ip++];
    uint32_t impl_count = module->code[frame->ip++];
    const char *trait = module_const_text(module, trait_const, "IMPLEMENT_TRAIT trait", err);
    if (!trait) return false;
    const char *type = module_const_text(module, type_const, "IMPLEMENT_TRAIT type", err);
    if (!type) return false;
    const char *provider = module_const_text(module, provider_const, "IMPLEMENT_TRAIT provider", err);
    if (!provider) return false;
    const char *provider_key = module_const_text(module, provider_key_const, "IMPLEMENT_TRAIT provider key", err);
    if (!provider_key) return false;
    if (vm->sp < impl_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT stack underflow");
    IdmTraitImplSpec *specs = impl_count == 0 ? NULL : calloc(impl_count, sizeof(*specs));
    if (impl_count != 0 && !specs) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; i < impl_count && ok; i++) {
        uint32_t method_const = module->code[frame->ip++];
        IdmArity arity = read_arity_operands(module, frame);
        const char *method = module_const_text(module, method_const, "IMPLEMENT_TRAIT method", err);
        if (!method) { ok = false; break; }
        specs[i].name = method;
        specs[i].arity = arity;
    }
    if (!ok) { free(specs); return false; }
    for (uint32_t i = impl_count; i > 0; i--) {
        IdmValue impl = idm_nil();
        if (!pop(vm, &impl, err)) { free(specs); return false; }
        specs[i - 1u].impl = impl;
    }
    ok = idm_trait_implement(vm->rt, trait, type, provider, provider_key, specs, impl_count, err);
    free(specs);
    if (!ok) return false;
    return push(vm, idm_atom(vm->rt, "ok"), err);
}

typedef struct {
    IdmValue traits;
    const char *method;
    const char *receiver_type;
    uint32_t argc;
} MethodImplementationRequest;

static bool resolve_method_implementation(Vm *vm, const MethodImplementationRequest *request, IdmValue *out_impl, IdmError *err) {
    size_t count = trait_candidate_count(request->traits);
    if (count == 0) return idm_error_set(err, idm_span_unknown(NULL), "method '%s' has no trait candidates", request->method);
    if (count == 1u) {
        const char *trait = NULL;
        if (!trait_candidate_at(request->traits, 0, &trait, err)) return false;
        return idm_trait_lookup(vm->rt, trait, request->method, request->receiver_type, request->argc, out_impl, err);
    }

    IdmValue match = idm_nil();
    size_t match_count = 0;
    IdmBuffer matching;
    idm_buf_init(&matching);
    for (size_t i = 0; i < count; i++) {
        const char *trait = NULL;
        if (!trait_candidate_at(request->traits, i, &trait, err)) {
            idm_buf_destroy(&matching);
            return false;
        }
        IdmError probe;
        idm_error_init(&probe);
        IdmValue impl = idm_nil();
        if (idm_trait_lookup(vm->rt, trait, request->method, request->receiver_type, request->argc, &impl, &probe)) {
            if (match_count != 0 && !idm_buf_append(&matching, ", ")) {
                idm_error_clear(&probe);
                idm_buf_destroy(&matching);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (!idm_buf_append(&matching, trait)) {
                idm_error_clear(&probe);
                idm_buf_destroy(&matching);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (match_count == 0) match = impl;
            match_count++;
        }
        idm_error_clear(&probe);
    }

    if (match_count == 1u) {
        *out_impl = match;
        idm_buf_destroy(&matching);
        return true;
    }
    if (match_count > 1u) {
        bool ok = idm_error_set(err, idm_span_unknown(NULL), "ambiguous method '%s' on type '%s'; matching traits: %s", request->method, request->receiver_type, matching.data ? matching.data : "?");
        idm_buf_destroy(&matching);
        return ok;
    }
    IdmBuffer candidates;
    idm_buf_init(&candidates);
    bool listed = append_trait_candidates(&candidates, request->traits, err);
    if (!listed && !(err && err->present)) idm_error_oom(err, idm_span_unknown(NULL));
    if (!listed) {
        idm_buf_destroy(&matching);
        idm_buf_destroy(&candidates);
        return false;
    }
    bool ok = idm_error_set(err, idm_span_unknown(NULL), "method '%s' is available via %s but is not implemented on type '%s'", request->method, candidates.data ? candidates.data : "?", request->receiver_type);
    idm_buf_destroy(&matching);
    idm_buf_destroy(&candidates);
    return ok;
}

static bool op_call_method(Vm *vm, Frame *frame, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t trait_const = module->code[frame->ip++];
    uint32_t method_const = module->code[frame->ip++];
    uint32_t argc = module->code[frame->ip++];
    bool tail = module->code[frame->ip++] != 0;
    if (argc == 0) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD requires a receiver");
    if (vm->sp < argc) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD stack underflow");
    size_t arg_base = vm->sp - argc;
    IdmValue receiver = vm->stack[arg_base];
    const char *type = idm_value_dispatch_type_name(receiver);
    IdmValue impl = idm_nil();
    IdmValue traits = idm_nil();
    if (!module_const_value(module, trait_const, "CALL_METHOD trait", &traits, err)) return false;
    const char *method = module_const_text(module, method_const, "CALL_METHOD method", err);
    if (!method) return false;
    MethodImplementationRequest request;
    request.traits = traits;
    request.method = method;
    request.receiver_type = type;
    request.argc = argc;
    if (!resolve_method_implementation(vm, &request, &impl, err)) return false;
    return call_closure_clause(vm, impl, arg_base, arg_base, argc, tail, status, out_reason, err);
}

static bool package_env_push(Vm *vm, Frame *frame, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t key_const = module->code[frame->ip++];
    if (key_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "PUSH_PACKAGE_ENV constant out of bounds");
    IdmValue key_value = module->constants[key_const];
    if (key_value.tag != IDM_VAL_ATOM && key_value.tag != IDM_VAL_WORD) return idm_error_set(err, idm_span_unknown(NULL), "PUSH_PACKAGE_ENV expects a key constant");
    IdmEnv *parent = frame->env ? frame->env : vm->rt->main_env;
    IdmEnv *child = idm_package_env_get_or_create(vm->rt, idm_symbol_text(key_value.as.symbol));
    if (!child) return idm_error_oom(err, idm_span_unknown(NULL));
    if (vm->env_save_count == vm->env_save_cap) {
        size_t cap = vm->env_save_cap ? vm->env_save_cap * 2u : 8u;
        IdmEnv **grown = realloc(vm->env_save, cap * sizeof(*grown));
        if (!grown) return idm_error_oom(err, idm_span_unknown(NULL));
        vm->env_save = grown;
        vm->env_save_cap = cap;
    }
    vm->env_save[vm->env_save_count++] = parent;
    frame->env = child;
    return true;
}

static bool package_env_pop(Vm *vm, Frame *frame, IdmError *err) {
    if (vm->env_save_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "POP_PACKAGE_ENV with no active package env");
    frame->env = vm->env_save[--vm->env_save_count];
    return true;
}

static bool op_list_cons(Vm *vm, IdmError *err) {
    IdmValue tail = idm_nil();
    IdmValue head = idm_nil();
    if (!pop(vm, &tail, err) || !pop(vm, &head, err)) return false;
    IdmValue result = idm_cons(vm->rt, head, tail, err);
    if (err && err->present) return false;
    return push(vm, result, err);
}

static bool op_list_append(Vm *vm, IdmError *err) {
    IdmValue tail = idm_nil();
    IdmValue head = idm_nil();
    if (!pop(vm, &tail, err) || !pop(vm, &head, err)) return false;
    IdmValue result = idm_nil();
    if (!idm_list_append(vm->rt, head, tail, &result, err)) return false;
    return push(vm, result, err);
}

static bool op_make_value_sequence(Vm *vm, Frame *frame, IdmError *err) {
    IdmValueSequenceKind kind = (IdmValueSequenceKind)frame->module->code[frame->ip++];
    uint32_t count = frame->module->code[frame->ip++];
    if (vm->sp < count) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_VALUE_SEQUENCE stack underflow");
    size_t start = vm->sp - count;
    IdmValue result = idm_nil();
    switch (kind) {
        case IDM_VALUE_SEQ_VECTOR:
            result = idm_vector(vm->rt, vm->stack + start, count, err);
            break;
        case IDM_VALUE_SEQ_TUPLE:
            result = idm_tuple(vm->rt, vm->stack + start, count, err);
            break;
        case IDM_VALUE_SEQ_DICT: {
            if (count % 2u != 0) return idm_error_set(err, idm_span_unknown(NULL), "dict construction requires key/value pairs");
            size_t pairs = count / 2u;
            IdmDictEntry *entries = pairs == 0 ? NULL : calloc(pairs, sizeof(*entries));
            if (pairs != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < pairs; i++) {
                entries[i].key = vm->stack[start + i * 2u];
                entries[i].value = vm->stack[start + i * 2u + 1u];
            }
            result = idm_dict(vm->rt, entries, pairs, err);
            free(entries);
            break;
        }
    }
    if (err && err->present) return false;
    vm->sp = start;
    return push(vm, result, err);
}

static bool vm_syntax_build_needs_payload(IdmSyntaxBuildKind kind) {
    return kind != IDM_SYNTAX_BUILD_NIL;
}

static bool op_make_syntax(Vm *vm, Frame *frame, IdmError *err) {
    IdmSyntaxBuildKind kind = (IdmSyntaxBuildKind)frame->module->code[frame->ip++];
    IdmValue payload = idm_nil();
    IdmValue ctx = idm_nil();
    if (vm_syntax_build_needs_payload(kind) && !pop(vm, &payload, err)) return false;
    if (!pop(vm, &ctx, err)) return false;
    IdmValue result = idm_nil();
    if (!idm_syntax_build(vm->rt, kind, ctx, payload, &result, err)) return false;
    return push(vm, result, err);
}

static bool op_string_concat(Vm *vm, Frame *frame, IdmError *err) {
    uint32_t count = frame->module->code[frame->ip++];
    if (vm->sp < count) return idm_error_set(err, idm_span_unknown(NULL), "STRING_CONCAT stack underflow");
    size_t start = vm->sp - count;
    IdmValue result = idm_nil();
    if (!idm_string_concat_display(vm->rt, vm->stack + start, count, &result, err)) return false;
    vm->sp = start;
    return push(vm, result, err);
}

static bool vm_run_loop_inner(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    IdmRuntime *rt = vm->rt;
    IdmValue result = idm_nil();
    *status = IDM_EXEC_DONE;
    if (vm->has_pending_raise) {
        vm->has_pending_raise = false;
        vm->raised = vm->pending_raise;
        vm->pending_raise = idm_nil();
        vm->has_raised = true;
        IdmBuffer buf;
        idm_buf_init(&buf);
        if (idm_value_write(&buf, vm->raised) && buf.data) idm_error_set(err, idm_span_unknown(NULL), "%s", buf.data);
        else idm_error_set(err, idm_span_unknown(NULL), "error raised");
        idm_buf_destroy(&buf);
        return false;
    }
    for (;;) {
        Frame *frame = current_frame(vm);
        if (!frame) break;
        const IdmBytecodeModule *module = frame->module;
        if (*budget >= 0) {
            if (*budget == 0) { *status = IDM_EXEC_YIELD; return true; }
            (*budget)--;
        }
        size_t instr_ip = frame->ip;
        if (frame->ip >= module->code_count) {
            idm_error_set(err, idm_span_unknown(NULL), "instruction pointer out of bounds");
            return false;
        }
        IdmOpcode op = (IdmOpcode)module->code[frame->ip++];
        uint32_t operand = 0;
        switch (op) {
            case IDM_OP_HALT:
                if (vm->sp != 0 && !pop(vm, &result, err)) return false;
                *status = IDM_EXEC_DONE;
                *out_result = result;
                return true;
            case IDM_OP_LOAD_CONST:
                operand = module->code[frame->ip++];
                if (!push(vm, module->constants[operand], err)) return false;
                break;
            case IDM_OP_LOAD_ARG:
                operand = module->code[frame->ip++];
                if (operand >= module->functions[frame->function_index].arity) return idm_error_set(err, idm_span_unknown(NULL), "argument index %u out of bounds", operand);
                if (!push(vm, vm->stack[frame->base + operand], err)) return false;
                break;
            case IDM_OP_LOAD_LOCAL:
                operand = module->code[frame->ip++];
                if (operand >= frame->local_count) return idm_error_set(err, idm_span_unknown(NULL), "local index %u out of bounds", operand);
                if (!push(vm, vm->stack[frame->locals_base + operand], err)) return false;
                break;
            case IDM_OP_STORE_LOCAL: {
                operand = module->code[frame->ip++];
                if (operand >= frame->local_count) return idm_error_set(err, idm_span_unknown(NULL), "local index %u out of bounds", operand);
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                vm->stack[frame->locals_base + operand] = value;
                break;
            }
            case IDM_OP_LOAD_CAPTURE:
                operand = module->code[frame->ip++];
                if (frame->closure.tag == IDM_VAL_CLOSURE) {
                    if (operand >= idm_closure_capture_count(frame->closure)) return idm_error_set(err, idm_span_unknown(NULL), "capture index %u out of bounds", operand);
                    if (!push(vm, idm_closure_capture(frame->closure, operand, err), err)) return false;
                    if (err && err->present) return false;
                } else {
                    if (operand >= frame->capture_count || frame->capture_base + operand >= vm->sp) return idm_error_set(err, idm_span_unknown(NULL), "capture index %u out of bounds", operand);
                    if (!push(vm, vm->stack[frame->capture_base + operand], err)) return false;
                }
                break;
            case IDM_OP_MAKE_CELL: {
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                if (!push(vm, idm_cell(rt, value, err), err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_LOAD_CELL: {
                IdmValue cell;
                if (!pop(vm, &cell, err)) return false;
                if (!push(vm, idm_cell_get(cell, err), err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_STORE_CELL: {
                IdmValue value, cell;
                if (!pop(vm, &value, err) || !pop(vm, &cell, err)) return false;
                if (!idm_cell_set(cell, value, err)) return false;
                break;
            }
            case IDM_OP_MAKE_CLOSURE: {
                operand = module->code[frame->ip++];
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE selector missing");
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &operand, 1u, selector, NULL, 0, frame->env, err);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_MAKE_CLOSURE_CAPTURES: {
                uint32_t fn_index = module->code[frame->ip++];
                uint32_t capture_count = module->code[frame->ip++];
                if (vm->sp < capture_count) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES stack underflow");
                IdmValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) return idm_error_oom(err, idm_span_unknown(NULL));
                size_t start = vm->sp - capture_count;
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = vm->stack[start + i];
                vm->sp = start;
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) { free(captures); return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES selector missing"); }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &fn_index, 1u, selector, captures, capture_count, frame->env, err);
                free(captures);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = module->code[frame->ip++];
                uint32_t capture_count = module->code[frame->ip++];
                if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE requires entries");
                uint32_t *entries = malloc((size_t)entry_count * sizeof(*entries));
                if (!entries) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) entries[i] = module->code[frame->ip++];
                if (vm->sp < capture_count) { free(entries); return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE stack underflow"); }
                IdmValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) { free(entries); return idm_error_oom(err, idm_span_unknown(NULL)); }
                size_t start = vm->sp - capture_count;
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = vm->stack[start + i];
                vm->sp = start;
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) { free(entries); free(captures); return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE selector missing"); }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, entries, entry_count, selector, captures, capture_count, frame->env, err);
                free(entries);
                free(captures);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_CALL:
                if (!execute_call_op(vm, frame, module, instr_ip, status, out_reason, err)) return false;
                break;
            case IDM_OP_LIST_CONS:
                if (!op_list_cons(vm, err)) return false;
                break;
            case IDM_OP_LIST_APPEND:
                if (!op_list_append(vm, err)) return false;
                break;
            case IDM_OP_MAKE_VALUE_SEQUENCE:
                if (!op_make_value_sequence(vm, frame, err)) return false;
                break;
            case IDM_OP_MAKE_SYNTAX:
                if (!op_make_syntax(vm, frame, err)) return false;
                break;
            case IDM_OP_STRING_CONCAT:
                if (!op_string_concat(vm, frame, err)) return false;
                break;
            case IDM_OP_RETURN: {
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                Frame returning;
                pop_frame(vm, &returning);
                size_t result_base = returning.result_base;
                if (vm->frame_count == 0) {
                    *status = IDM_EXEC_DONE;
                    *out_result = value;
                    return true;
                }
                vm->sp = result_base;
                if (!push(vm, value, err)) return false;
                break;
            }
            case IDM_OP_POP: {
                IdmValue ignored;
                if (!pop(vm, &ignored, err)) return false;
                break;
            }
            case IDM_OP_JUMP:
                operand = module->code[frame->ip++];
                frame->ip = operand;
                break;
            case IDM_OP_JUMP_IF_FALSE: {
                operand = module->code[frame->ip++];
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                if (!idm_value_ok(value)) frame->ip = operand;
                break;
            }
            case IDM_OP_RECV:
                if (!op_recv(vm, frame, instr_ip, status, out_reason, err)) return false;
                if (*status == IDM_EXEC_BLOCK_RECEIVE) return true;
                break;
            case IDM_OP_PUSH_PACKAGE_ENV:
                if (!package_env_push(vm, frame, err)) return false;
                break;
            case IDM_OP_POP_PACKAGE_ENV:
                if (!package_env_pop(vm, frame, err)) return false;
                break;
            case IDM_OP_DEFINE_TRAIT:
                if (!op_define_trait(vm, frame, err)) return false;
                break;
            case IDM_OP_IMPLEMENT_TRAIT:
                if (!op_implement_trait(vm, frame, err)) return false;
                break;
            case IDM_OP_CALL_METHOD:
                if (!op_call_method(vm, frame, status, out_reason, err)) return false;
                break;
            case IDM_OP_RESCUE_PUSH: {
                operand = module->code[frame->ip++];
                if (!handler_reserve(vm, vm->handler_count + 1u, err)) return false;
                vm->handlers[vm->handler_count].catch_ip = operand;
                vm->handlers[vm->handler_count].frame_count = vm->frame_count;
                vm->handlers[vm->handler_count].sp = vm->sp;
                vm->handler_count++;
                break;
            }
            case IDM_OP_RESCUE_POP:
                if (vm->handler_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "RESCUE_POP with no active handler");
                vm->handler_count--;
                break;
            case IDM_OP_RAISE: {
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                vm->raised = value;
                vm->has_raised = true;
                IdmBuffer buf;
                idm_buf_init(&buf);
                if (idm_value_write(&buf, value) && buf.data) idm_error_set(err, idm_span_unknown(NULL), "%s", buf.data);
                else idm_error_set(err, idm_span_unknown(NULL), "error raised");
                idm_buf_destroy(&buf);
                return false;
            }
            case IDM_OP_LOAD_RAISED:
                if (!push(vm, vm->raised, err)) return false;
                break;
            case IDM_OP_LOAD_ENV:
                operand = module->code[frame->ip++];
                if (!push(vm, idm_env_slot_get(frame->env, operand), err)) return false;
                break;
            case IDM_OP_LOAD_PACKAGE_SLOT: {
                uint32_t key_const = module->code[frame->ip++];
                uint32_t slot = module->code[frame->ip++];
                const char *key = module_const_text(module, key_const, "LOAD_PACKAGE_SLOT key", err);
                if (!key) return false;
                IdmEnv *env = idm_package_env_get_or_create(vm->rt, key);
                if (!env) return idm_error_oom(err, idm_span_unknown(NULL));
                if (!push(vm, idm_env_slot_get(env, slot), err)) return false;
                break;
            }
            case IDM_OP_STORE_ENV: {
                operand = module->code[frame->ip++];
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                if (!idm_env_slot_ensure(frame->env, operand, err)) return false;
                value = idm_value_copy(vm->rt, &vm->rt->immortal, value, err);
                if (err->present) return false;
                idm_env_slot_set(frame->env, operand, value);
                break;
            }
            default:
                idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
                return false;
        }
        if (*status != IDM_EXEC_DONE) return true;
    }
    *status = IDM_EXEC_DONE;
    *out_result = result;
    return true;
}

static bool vm_run_loop(Vm *vm, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    for (;;) {
        bool ok = vm_run_loop_inner(vm, &budget, status, out_result, out_reason, err);
        if (ok) return true;
        if (vm->handler_count == 0) {
            if (vm->has_raised) {
                vm->has_raised = false;
                idm_error_clear(err);
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = vm->raised;
                return true;
            }
            if (err->present && err->span.line == 0) {
                const Frame *top = vm->frame_count != 0 ? &vm->frames[vm->frame_count - 1u] : NULL;
                if (top && top->module) {
                    IdmSpan where = idm_bc_span_at(top->module, top->ip > 0 ? top->ip - 1u : 0u);
                    if (where.line != 0) err->span = where;
                }
            }
            size_t depth = vm->frame_count;
            size_t shown = depth < 12u ? depth : 12u;
            for (size_t i = 0; i < shown; i++) {
                const Frame *f = &vm->frames[depth - 1u - i];
                const char *fname = f->module && f->function_index < f->module->function_count ? f->module->functions[f->function_index].name : NULL;
                IdmSpan where = f->module ? idm_bc_span_at(f->module, f->ip > 0 ? f->ip - 1u : 0u) : idm_span_unknown(NULL);
                if (where.line != 0) {
                    idm_error_note(err, "in %s (%s:%u)", fname && fname[0] ? fname : "<fn>", where.file, where.line);
                } else {
                    idm_error_note(err, "in %s", fname && fname[0] ? fname : "<fn>");
                }
            }
            if (depth > shown) idm_error_note(err, "... (%zu more frames)", depth - shown);
            return false;
        }
        Handler handler = vm->handlers[--vm->handler_count];
        while (vm->frame_count > handler.frame_count) {
            Frame discarded;
            pop_frame(vm, &discarded);
        }
        Frame *frame = current_frame(vm);
        if (!frame) return false;
        if (handler.sp <= vm->sp) vm->sp = handler.sp;
        if (vm->has_raised) {
            vm->has_raised = false;
        } else {
            vm->raised = idm_error_reason_value(vm->rt, err);
        }
        idm_error_clear(err);
        frame->ip = handler.catch_ip;
        if (budget >= 0 && vm->sched) {
            *status = IDM_EXEC_YIELD;
            return true;
        }
    }
}

static bool ensure_module_ready(const IdmBytecodeModule *module, IdmError *err) {
    if (!idm_bc_verify(module, err)) return false;
    if (!idm_bc_is_finalized(module)) return idm_error_set(err, idm_span_unknown(NULL), "bytecode module is not finalized");
    return true;
}

static bool vm_run_limited_in_env(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmVmLimits limits, IdmEnv *env, IdmValue *out, IdmError *err) {
    if (!ensure_module_ready(module, err)) return false;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = limits;
    vm.root_env = env;
    if (!push_frame(&vm, module, function_index, idm_nil(), 0, 0, err)) { vm_reset(&vm); return false; }
    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue result = idm_nil();
    IdmValue reason = idm_nil();
    bool ok = vm_run_loop(&vm, -1, &status, &result, &reason, err);
    vm_reset(&vm);
    if (!ok) return false;
    if (status == IDM_EXEC_EXIT) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        idm_error_describe(rt, reason, &buf);
        if (buf.data) {
            idm_error_set(err, idm_span_unknown(NULL), "exited with reason %s", buf.data);
        } else {
            idm_error_set(err, idm_span_unknown(NULL), "program exited outside the scheduler");
        }
        idm_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

bool idm_vm_run_limited(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmVmLimits limits, IdmValue *out, IdmError *err) {
    return vm_run_limited_in_env(rt, module, function_index, limits, NULL, out, err);
}

bool idm_vm_run(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmValue *out, IdmError *err) {
    return idm_vm_run_limited(rt, module, function_index, idm_vm_default_limits(), out, err);
}

bool idm_vm_run_in_env(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmEnv *env, IdmValue *out, IdmError *err) {
    return vm_run_limited_in_env(rt, module, function_index, idm_vm_default_limits(), env, out, err);
}

bool idm_vm_call_function(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!ensure_module_ready(module, err)) return false;
    if (function_index >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (module->functions[function_index].arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "function arity mismatch: expected %u got %u", module->functions[function_index].arity, argc);
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = idm_vm_default_limits();
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&vm, args[i], err)) {
            vm_reset(&vm);
            return false;
        }
    }
    if (!push_frame(&vm, module, function_index, idm_nil(), 0, 0, err)) {
        vm_reset(&vm);
        return false;
    }
    return vm_run_call(&vm, -1, NULL, out, err);
}

bool idm_vm_call_closure(IdmRuntime *rt, IdmValue closure, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "call target must be a function value");
    const IdmBytecodeModule *module = idm_closure_module(closure);
    if (!module) return idm_error_set(err, idm_span_unknown(NULL), "function value has no backing module");
    if (!ensure_module_ready(module, err)) return false;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = idm_vm_default_limits();
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
    if (!call_value(&vm, argc, false, NULL, NULL, err)) {
        vm_reset(&vm);
        return false;
    }
    return vm_run_call(&vm, -1, NULL, out, err);
}

IdmExec *idm_exec_create(IdmRuntime *rt, const IdmBytecodeModule *module, IdmScheduler *sched, IdmActor *self, IdmVmLimits limits, IdmError *err) {
    IdmExec *exec = calloc(1u, sizeof(*exec));
    if (!exec) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    exec->rt = rt;
    exec->module = module;
    exec->sched = sched;
    exec->self = self;
    exec->limits = limits;
    exec->root_env = rt->main_env;
    return exec;
}

void idm_exec_destroy(IdmExec *exec) {
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

const char *idm_exec_cwd(const IdmExec *exec) {
    return exec ? exec->cwd : NULL;
}

bool idm_exec_set_cwd(IdmExec *exec, const char *cwd) {
    char *copy = idm_strdup(cwd);
    if (!copy) return false;
    free(exec->cwd);
    exec->cwd = copy;
    return true;
}

const char *idm_exec_env_get(const IdmExec *exec, const char *name) {
    if (!exec) return NULL;
    for (size_t i = 0; i < exec->env_count; i++) {
        if (strcmp(exec->env_names[i], name) == 0) return exec->env_values[i];
    }
    return NULL;
}

bool idm_exec_env_set(IdmExec *exec, const char *name, const char *value) {
    for (size_t i = 0; i < exec->env_count; i++) {
        if (strcmp(exec->env_names[i], name) == 0) {
            char *copy = idm_strdup(value);
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
    exec->env_names[exec->env_count] = idm_strdup(name);
    exec->env_values[exec->env_count] = idm_strdup(value);
    if (!exec->env_names[exec->env_count] || !exec->env_values[exec->env_count]) {
        free(exec->env_names[exec->env_count]);
        free(exec->env_values[exec->env_count]);
        return false;
    }
    exec->env_count++;
    return true;
}

size_t idm_exec_env_count(const IdmExec *exec) {
    return exec ? exec->env_count : 0;
}

bool idm_exec_env_entry(const IdmExec *exec, size_t index, const char **out_name, const char **out_value) {
    if (!exec || index >= exec->env_count) return false;
    *out_name = exec->env_names[index];
    *out_value = exec->env_values[index];
    return true;
}

bool idm_exec_copy_context(IdmExec *dst, const IdmExec *src) {
    if (!src) return true;
    if (src->cwd && !idm_exec_set_cwd(dst, src->cwd)) return false;
    for (size_t i = 0; i < src->env_count; i++) {
        if (!idm_exec_env_set(dst, src->env_names[i], src->env_values[i])) return false;
    }
    return true;
}

bool idm_exec_setup_function(IdmExec *exec, uint32_t function_index, IdmError *err) {
    if (exec->module && !ensure_module_ready(exec->module, err)) return false;
    return push_frame(exec, exec->module, function_index, idm_nil(), 0, 0, err);
}

bool idm_exec_setup_thunk(IdmExec *exec, IdmValue closure, IdmError *err) {
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a function value");
    if (!closure_selector_ready(exec, closure, err)) return false;
    if (!closure_accepts_argc(exec, closure, 0)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a zero-arity function value");
    if (!push(exec, closure, err)) return false;
    return call_value(exec, 0, false, NULL, NULL, err);
}

static _Thread_local IdmExec *g_current_exec = NULL;

IdmExec *idm_current_exec(void) {
    return g_current_exec;
}

bool idm_exec_step(IdmExec *exec, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    if (exec->frame_count == 0) {
        *status = IDM_EXEC_DONE;
        *out_result = idm_nil();
        return true;
    }
    IdmExec *prev = g_current_exec;
    g_current_exec = exec;
    bool ok = vm_run_loop(exec, budget, status, out_result, out_reason, err);
    g_current_exec = prev;
    return ok;
}

bool idm_exec_take_port_request(IdmExec *exec, IdmValue *out_graph) {
    if (!exec->has_port_request) return false;
    *out_graph = exec->port_request;
    exec->has_port_request = false;
    exec->port_request = idm_nil();
    return true;
}

bool idm_exec_take_await(IdmExec *exec, IdmValue *out_port) {
    if (!exec->has_await) return false;
    *out_port = exec->await_port;
    exec->has_await = false;
    exec->await_port = idm_nil();
    return true;
}

bool idm_exec_take_port_read(IdmExec *exec, IdmValue *out_port, const char **out_stream, size_t *out_max) {
    if (!exec->has_port_read_request) return false;
    *out_port = exec->port_read_port;
    *out_stream = exec->port_read_stream;
    *out_max = exec->port_read_max;
    exec->has_port_read_request = false;
    exec->port_read_port = idm_nil();
    exec->port_read_stream = NULL;
    exec->port_read_max = 0;
    return true;
}

bool idm_exec_take_port_write(IdmExec *exec, IdmValue *out_port, IdmValue *out_data) {
    if (!exec->has_port_write_request) return false;
    *out_port = exec->port_write_port;
    *out_data = exec->port_write_data;
    exec->has_port_write_request = false;
    exec->port_write_port = idm_nil();
    exec->port_write_data = idm_nil();
    return true;
}

bool idm_exec_take_tty(IdmExec *exec, bool *out_line_mode, bool *out_has_timeout, int64_t *out_timeout_ms) {
    if (!exec->has_tty_request) return false;
    *out_line_mode = exec->tty_line_mode;
    *out_has_timeout = exec->tty_has_timeout;
    *out_timeout_ms = exec->tty_timeout_ms;
    exec->has_tty_request = false;
    return true;
}

bool idm_exec_push_result(IdmExec *exec, IdmValue value, IdmError *err) {
    return push(exec, value, err);
}

void idm_exec_inject_raise(IdmExec *exec, IdmValue reason) {
    exec->pending_raise = reason;
    exec->has_pending_raise = true;
}

IdmScheduler *idm_exec_scheduler(const IdmExec *exec) {
    return exec ? exec->sched : NULL;
}

void idm_exec_visit_roots(const IdmExec *exec, IdmRootVisitor visit, void *user) {
    if (!exec) return;
    for (size_t i = 0; i < exec->sp; i++) visit(user, exec->stack[i]);
    for (size_t f = 0; f < exec->frame_count; f++) {
        const Frame *frame = &exec->frames[f];
        visit(user, frame->closure);
    }
    visit(user, exec->raised);
    if (exec->has_pending_raise) visit(user, exec->pending_raise);
    if (exec->has_await) visit(user, exec->await_port);
    if (exec->has_port_request) visit(user, exec->port_request);
    if (exec->has_port_read_request) visit(user, exec->port_read_port);
    if (exec->has_port_write_request) {
        visit(user, exec->port_write_port);
        visit(user, exec->port_write_data);
    }
}
