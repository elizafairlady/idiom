#include "idiom/vm.h"

#include "idiom/actor.h"
#include "idiom/ports.h"
#include "idiom/prims.h"
#include "idiom/regex.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDM_FRAME_INLINE_CAPTURES 4u
#define IDM_FRAME_INLINE_REGS 16u

typedef struct {
    const IdmBytecodeModule *module;
    uint32_t function_index;
    size_t ip;
    IdmValue *regs;
    uint32_t reg_count;
    uint32_t reg_cap;
    bool regs_owned;
    uint32_t return_reg;
    bool has_return;
    IdmValue closure;
    IdmEnv *env;
    IdmValue *captures;
    uint32_t capture_count;
    bool captures_owned;
    IdmValue inline_regs[IDM_FRAME_INLINE_REGS];
    IdmValue inline_captures[IDM_FRAME_INLINE_CAPTURES];
} Frame;

typedef struct {
    size_t catch_ip;
    size_t frame_count;
} Handler;

struct IdmExec {
    IdmRuntime *rt;
    const IdmBytecodeModule *module;
    IdmScheduler *sched;
    IdmActor *self;
    Frame *frames;
    size_t frame_count;
    size_t frame_cap;
    bool frames_borrowed;
    IdmVmLimits limits;
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
    bool has_block_result_dst;
    size_t block_frame;
    uint32_t block_reg;
    bool has_direct_result;
    IdmValue direct_result;
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

#define VM_PROFILE_OPCODE_COUNT ((size_t)IDM_OP_COUNT)

static atomic_bool vm_profile_registered = false;
static atomic_uint_fast64_t vm_profile_opcode_counts[VM_PROFILE_OPCODE_COUNT];

static void vm_profile_flush_opcodes(void) {
    char name[64];
    for (size_t i = 0; i < VM_PROFILE_OPCODE_COUNT; i++) {
        uint64_t count = atomic_exchange_explicit(&vm_profile_opcode_counts[i], 0, memory_order_relaxed);
        if (count == 0) continue;
        snprintf(name, sizeof(name), "vm.op.%s", idm_opcode_name((IdmOpcode)i));
        idm_profile_count(name, count);
    }
}

static bool vm_profile_active(void) {
    if (!idm_profile_enabled()) return false;
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&vm_profile_registered, &expected, true, memory_order_relaxed, memory_order_relaxed)) {
        atexit(vm_profile_flush_opcodes);
    }
    return true;
}

static void vm_profile_opcode(IdmOpcode op) {
    size_t index = (size_t)op;
    if (index >= VM_PROFILE_OPCODE_COUNT) return;
    atomic_fetch_add_explicit(&vm_profile_opcode_counts[index], 1, memory_order_relaxed);
}

IdmVmLimits idm_vm_default_limits(void) {
    IdmVmLimits limits;
    limits.max_registers = 1024u * 64u;
    limits.max_frames = 1024u;
    return limits;
}

static void frame_clear_captures(Frame *frame) {
    if (frame->captures_owned) free(frame->captures);
    frame->captures = NULL;
    frame->capture_count = 0;
    frame->captures_owned = false;
}

static void frame_release(Frame *frame) {
    if (!frame) return;
    if (frame->regs_owned) free(frame->regs);
    frame->regs = NULL;
    frame->reg_count = 0;
    frame->reg_cap = 0;
    frame->regs_owned = false;
    frame_clear_captures(frame);
}

static bool frame_set_captures(Frame *frame, const IdmValue *captures, uint32_t capture_count, IdmError *err) {
    frame_clear_captures(frame);
    if (capture_count == 0) return true;
    if (capture_count <= IDM_FRAME_INLINE_CAPTURES) {
        for (uint32_t i = 0; i < capture_count; i++) frame->inline_captures[i] = captures ? captures[i] : idm_nil();
        frame->captures = frame->inline_captures;
        frame->capture_count = capture_count;
        frame->captures_owned = false;
        return true;
    }
    IdmValue *copy = malloc((size_t)capture_count * sizeof(*copy));
    if (!copy) return idm_error_oom(err, idm_span_unknown(NULL));
    for (uint32_t i = 0; i < capture_count; i++) copy[i] = captures ? captures[i] : idm_nil();
    frame->captures = copy;
    frame->capture_count = capture_count;
    frame->captures_owned = true;
    return true;
}

static bool frame_install_registers(Frame *frame, uint32_t register_count, const IdmValue *args, uint32_t argc, IdmError *err) {
    if (register_count > frame->reg_cap) {
        IdmValue *regs = NULL;
        uint32_t reg_cap = register_count;
        bool regs_owned = false;
        if (register_count <= IDM_FRAME_INLINE_REGS) {
            regs = frame->inline_regs;
            reg_cap = IDM_FRAME_INLINE_REGS;
        } else {
            regs = malloc((size_t)register_count * sizeof(*regs));
            if (!regs) return idm_error_oom(err, idm_span_unknown(NULL));
            regs_owned = true;
        }
        for (uint32_t i = 0; i < argc; i++) regs[i] = args ? args[i] : idm_nil();
        for (uint32_t i = argc; i < register_count; i++) regs[i] = idm_nil();
        if (frame->regs_owned) free(frame->regs);
        frame->regs = regs;
        frame->reg_cap = reg_cap;
        frame->regs_owned = regs_owned;
    } else {
        if (argc != 0) {
            if (args) memmove(frame->regs, args, (size_t)argc * sizeof(*frame->regs));
            else for (uint32_t i = 0; i < argc; i++) frame->regs[i] = idm_nil();
        }
        for (uint32_t i = argc; i < register_count; i++) frame->regs[i] = idm_nil();
    }
    frame->reg_count = register_count;
    return true;
}

static void frame_rebase_inline_storage(Frame *frames, uintptr_t old_base, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uintptr_t old_regs = old_base + i * sizeof(Frame) + offsetof(Frame, inline_regs);
        if ((uintptr_t)frames[i].regs == old_regs) frames[i].regs = frames[i].inline_regs;
        uintptr_t old_inline = old_base + i * sizeof(Frame) + offsetof(Frame, inline_captures);
        if ((uintptr_t)frames[i].captures == old_inline) frames[i].captures = frames[i].inline_captures;
    }
}

static void vm_reset(Vm *vm) {
    for (size_t i = 0; i < vm->frame_count; i++) frame_release(&vm->frames[i]);
    if (!vm->frames_borrowed) free(vm->frames);
    if (!vm->handlers_borrowed) free(vm->handlers);
    vm->frames = NULL;
    vm->frame_count = 0;
    vm->frame_cap = 0;
    vm->frames_borrowed = false;
    vm->handlers = NULL;
    vm->handler_count = 0;
    vm->handler_cap = 0;
    vm->handlers_borrowed = false;
    free(vm->env_save);
    vm->env_save = NULL;
    vm->env_save_count = 0;
    vm->env_save_cap = 0;
    vm->root_env = NULL;
    vm->has_direct_result = false;
    vm->direct_result = idm_nil();
    vm->has_raised = false;
    vm->raised = idm_nil();
    vm->has_pending_raise = false;
    vm->pending_raise = idm_nil();
    vm->has_port_read_request = false;
    vm->port_read_port = idm_nil();
    vm->port_read_stream = NULL;
    vm->port_read_max = 0;
    vm->has_port_write_request = false;
    vm->port_write_port = idm_nil();
    vm->port_write_data = idm_nil();
}

static void vm_borrow_storage(Vm *vm, Frame *frames, size_t frame_cap, Handler *handlers, size_t handler_cap) {
    vm->frames = frames;
    vm->frame_cap = frame_cap;
    vm->frames_borrowed = true;
    vm->handlers = handlers;
    vm->handler_cap = handler_cap;
    vm->handlers_borrowed = true;
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
    uintptr_t old_base = (uintptr_t)vm->frames;
    if (vm->frames_borrowed) {
        size_t cap = 0;
        if (!idm_next_capacity(vm->frame_cap, 32u, needed, &cap) || cap > SIZE_MAX / sizeof(*vm->frames)) return idm_error_oom(err, idm_span_unknown(NULL));
        Frame *next = malloc(cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        if (vm->frame_count != 0) memcpy(next, vm->frames, vm->frame_count * sizeof(*next));
        if (old_base != 0) frame_rebase_inline_storage(next, old_base, vm->frame_count);
        vm->frames = next;
        vm->frame_cap = cap;
        vm->frames_borrowed = false;
        return true;
    }
    if (!idm_grow((void **)&vm->frames, &vm->frame_cap, sizeof(*vm->frames), 32u, needed)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (old_base != 0 && old_base != (uintptr_t)vm->frames) frame_rebase_inline_storage(vm->frames, old_base, vm->frame_count);
    return true;
}

static bool values_overlap_frames(const Vm *vm, const IdmValue *values, uint32_t count) {
    if (!vm->frames || !values || count == 0 || vm->frame_cap == 0) return false;
    uintptr_t start = (uintptr_t)vm->frames;
    uintptr_t end = start + vm->frame_cap * sizeof(*vm->frames);
    uintptr_t ptr = (uintptr_t)values;
    uintptr_t ptr_end = ptr + (uintptr_t)count * sizeof(*values);
    return ptr < end && ptr_end > start && ptr_end >= ptr;
}

static bool protect_values_for_frame_push(Vm *vm, const IdmValue **values, uint32_t count, IdmValue **owned, IdmError *err) {
    *owned = NULL;
    if (!values || !*values || count == 0) return true;
    if (vm->frame_count + 1u <= vm->frame_cap || !values_overlap_frames(vm, *values, count)) return true;
    IdmValue *copy = malloc((size_t)count * sizeof(*copy));
    if (!copy) return idm_error_oom(err, idm_span_unknown(NULL));
    memcpy(copy, *values, (size_t)count * sizeof(*copy));
    *values = copy;
    *owned = copy;
    return true;
}

static bool handler_reserve(Vm *vm, size_t needed, IdmError *err) {
    if (needed <= vm->handler_cap) return true;
    if (vm->handlers_borrowed) {
        size_t cap = 0;
        if (!idm_next_capacity(vm->handler_cap, 8u, needed, &cap) || cap > SIZE_MAX / sizeof(*vm->handlers)) return idm_error_oom(err, idm_span_unknown(NULL));
        Handler *next = malloc(cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        if (vm->handler_count != 0) memcpy(next, vm->handlers, vm->handler_count * sizeof(*next));
        vm->handlers = next;
        vm->handler_cap = cap;
        vm->handlers_borrowed = false;
        return true;
    }
    if (!idm_grow((void **)&vm->handlers, &vm->handler_cap, sizeof(*vm->handlers), 8u, needed)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool generated_clause_name(const char *name) {
    if (!name || !name[0]) return true;
    if (name[0] == '<' || name[0] == '{' || name[0] == '[' || name[0] == '(') return true;
    return strchr(name, ' ') != NULL || strchr(name, '\t') != NULL;
}

static bool push_frame(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, IdmValue closure, const IdmValue *args, uint32_t argc, bool has_return, uint32_t return_reg, IdmError *err) {
    if (!module) module = vm->module;
    if (function_index >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (vm->frame_count >= vm->limits.max_frames) return idm_error_set(err, idm_span_unknown(NULL), "VM frame limit exceeded");
    const IdmBcFunction *fn = &module->functions[function_index];
    if (fn->arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "function arity mismatch: expected %u got %u", fn->arity, argc);
    if (fn->register_count > vm->limits.max_registers) return idm_error_set(err, idm_span_unknown(NULL), "VM register limit exceeded");
    Frame *caller = current_frame(vm);
    IdmEnv *caller_env = caller ? caller->env : NULL;
    IdmEnv *root_env = vm->root_env ? vm->root_env : vm->rt->main_env;
    IdmEnv *env = idm_is_closure(closure) ? idm_closure_env(closure) : (caller_env ? caller_env : root_env);
    if (!env) return idm_error_set(err, idm_span_unknown(NULL), "frame entry requires an explicit runtime environment");
    const IdmValue *safe_args = args;
    IdmValue *owned_args = NULL;
    if (!protect_values_for_frame_push(vm, &safe_args, argc, &owned_args, err)) return false;
    if (!frame_reserve(vm, vm->frame_count + 1u, err)) {
        free(owned_args);
        return false;
    }
    Frame *frame = &vm->frames[vm->frame_count];
    memset(frame, 0, sizeof(*frame));
    frame->module = module;
    frame->function_index = function_index;
    frame->ip = fn->entry;
    frame->return_reg = return_reg;
    frame->has_return = has_return;
    frame->closure = closure;
    frame->env = env;
    if (!frame_install_registers(frame, fn->register_count, safe_args, argc, err)) {
        frame_release(frame);
        free(owned_args);
        return false;
    }
    free(owned_args);
    vm->frame_count++;
    return true;
}

static void pop_frame(Vm *vm, Frame *out) {
    *out = vm->frames[--vm->frame_count];
    vm->frames[vm->frame_count].regs = NULL;
    vm->frames[vm->frame_count].reg_count = 0;
    vm->frames[vm->frame_count].reg_cap = 0;
    vm->frames[vm->frame_count].regs_owned = false;
    vm->frames[vm->frame_count].captures = NULL;
    vm->frames[vm->frame_count].capture_count = 0;
    vm->frames[vm->frame_count].captures_owned = false;
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

static bool save_block_result_dst(Vm *vm, uint32_t dst, IdmError *err) {
    if (vm->frame_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "blocking operation requires an active frame");
    Frame *frame = current_frame(vm);
    if (dst >= frame->reg_count) return idm_error_set(err, idm_span_unknown(NULL), "blocking result register r%u out of bounds", dst);
    vm->has_block_result_dst = true;
    vm->block_frame = vm->frame_count - 1u;
    vm->block_reg = dst;
    return true;
}

static bool generic_primitive_clause(Vm *vm, uint32_t primitive, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if ((IdmPrimitive)primitive == IDM_PRIM_RAISE) {
        if (!idm_primitive_require_arity(vm->rt, (IdmPrimitive)primitive, argc, err)) return false;
        return raise_value(vm, args[0], err);
    }
    *out = idm_nil();
    return idm_prim_invoke(vm->rt, (IdmPrimitive)primitive, args, argc, out, err);
}

static bool op_tty_block(Vm *vm, IdmPrimitive prim, const IdmValue *args, uint32_t argc, IdmError *err) {
    if (!idm_primitive_require_arity(vm->rt, prim, argc, err)) return false;
    vm->tty_has_timeout = false;
    vm->tty_timeout_ms = 0;
    if (prim == IDM_PRIM_TTY_READ) {
        IdmValue timeout = args[0];
        int64_t timeout_ms = 0;
        if (idm_value_is_int(timeout) && idm_int_to_i64(timeout, &timeout_ms) && timeout_ms >= 0) {
            vm->tty_has_timeout = true;
            vm->tty_timeout_ms = timeout_ms;
        } else if (idm_value_tag(timeout) != IDM_VAL_NIL) {
            idm_error_set(err, idm_span_unknown(NULL), "tty-read timeout must be a non-negative integer or :nil");
            return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "tty-read"), timeout);
        }
    }
    vm->tty_line_mode = prim == IDM_PRIM_TTY_READ_LINE;
    vm->has_tty_request = true;
    return true;
}

static bool port_stream_value(IdmRuntime *rt, IdmValue value, const char **out, IdmError *err) {
    if (idm_value_tag(value) == IDM_VAL_ATOM) {
        const char *text = idm_symbol_text(idm_value_symbol(value));
        if (strcmp(text, "stdout") == 0 || strcmp(text, "stderr") == 0) {
            *out = text;
            return true;
        }
    }
    idm_error_set(err, idm_span_unknown(NULL), "port-read expects :stdout or :stderr");
    return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "port-read"), value);
}

static bool op_port_read_block(Vm *vm, IdmPrimitive primitive, const IdmValue *args, uint32_t argc, IdmError *err) {
    (void)primitive;
    if (!idm_primitive_require_arity(vm->rt, IDM_PRIM_PORT_READ, argc, err)) return false;
    IdmValue port = args[0];
    IdmValue streamv = args[1];
    IdmValue maxv = args[2];
    if (idm_value_tag(port) != IDM_VAL_PORT) {
        idm_error_set(err, idm_span_unknown(NULL), "port-read expects a port");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-read"), port);
    }
    const char *stream = NULL;
    if (!port_stream_value(vm->rt, streamv, &stream, err)) return false;
    int64_t max = 0;
    if (!idm_value_is_int(maxv) || !idm_int_to_i64(maxv, &max) || max <= 0) {
        idm_error_set(err, idm_span_unknown(NULL), "port-read expects a positive byte count");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-read"), maxv);
    }
    vm->port_read_port = port;
    vm->port_read_stream = stream;
    vm->port_read_max = (size_t)max;
    vm->has_port_read_request = true;
    return true;
}

static bool op_port_write_block(Vm *vm, IdmPrimitive primitive, const IdmValue *args, uint32_t argc, IdmError *err) {
    (void)primitive;
    if (!idm_primitive_require_arity(vm->rt, IDM_PRIM_PORT_WRITE, argc, err)) return false;
    IdmValue port = args[0];
    IdmValue data = args[1];
    if (idm_value_tag(port) != IDM_VAL_PORT) {
        idm_error_set(err, idm_span_unknown(NULL), "port-write expects a port");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-write"), port);
    }
    if (idm_value_tag(data) != IDM_VAL_STRING) {
        idm_error_set(err, idm_span_unknown(NULL), "port-write expects a string");
        return idm_error_reason(vm->rt, err, "type-error", 2, idm_atom(vm->rt, "port-write"), data);
    }
    vm->port_write_port = port;
    vm->port_write_data = data;
    vm->has_port_write_request = true;
    return true;
}

typedef bool (*BlockingOp)(Vm *vm, IdmPrimitive primitive, const IdmValue *args, uint32_t argc, IdmError *err);

static bool blocking_dispatch(Vm *vm, IdmExecStatus *status, uint32_t dst, IdmPrimitive primitive, BlockingOp op, IdmExecStatus block_status, const IdmValue *args, uint32_t argc, IdmError *err) {
    if (!status) return idm_error_set(err, idm_span_unknown(NULL), "blocking primitive '%s' requires a schedulable VM step", idm_primitive_name(primitive));
    if (!op(vm, primitive, args, argc, err)) return false;
    if (!save_block_result_dst(vm, dst, err)) return false;
    *status = block_status;
    return true;
}

static bool call_value(Vm *vm, uint32_t dst, IdmValue callee, const IdmValue *args, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err);

static bool primitive_actor_context(Vm *vm, IdmPrimitive primitive, IdmError *err) {
    if (vm->self && vm->sched) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", idm_primitive_name(primitive));
}

static bool value_atom_named(IdmValue value, const char *name) {
    return idm_value_tag(value) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(value)), name) == 0;
}

static bool value_is_command_graph(IdmValue value) {
    if (!idm_is_tuple(value) || idm_sequence_count(value) < 3u) return false;
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(value, 0, &ignore);
    idm_error_clear(&ignore);
    return value_atom_named(tag, "exec") || value_atom_named(tag, "pipeline");
}

static bool spawn_port_process(Vm *vm, IdmValue graph, IdmValue *out, const char *name, IdmError *err) {
    if (!value_is_command_graph(graph)) return idm_error_set(err, idm_span_unknown(NULL), "%s expects a function or command graph", name);
    IdmPort *port = idm_port_launch(vm->rt, graph, vm, err);
    if (!port) return false;
    if (!idm_sched_register_port(vm->sched, port, out, err)) {
        idm_port_free(port);
        return false;
    }
    return true;
}

static bool spawn_port_link_process(Vm *vm, IdmValue graph, IdmValue *out, bool *self_exits, IdmValue *exit_reason, IdmError *err) {
    if (!value_is_command_graph(graph)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-link expects a function or command graph");
    IdmPort *port = idm_port_launch(vm->rt, graph, vm, err);
    if (!port) return false;
    return idm_sched_register_port_link(vm->sched, port, vm->self, out, self_exits, exit_reason, err);
}

static bool spawn_port_monitor_process(Vm *vm, IdmValue graph, IdmValue *out_port, IdmValue *out_ref, IdmError *err) {
    if (!value_is_command_graph(graph)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-monitor expects a function or command graph");
    IdmPort *port = idm_port_launch(vm->rt, graph, vm, err);
    if (!port) return false;
    return idm_sched_register_port_monitor(vm->sched, port, vm->self, out_port, out_ref, err);
}

static bool execute_apply_primitive(Vm *vm, uint32_t dst, const IdmValue *args, uint32_t argc, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!idm_primitive_require_arity(vm->rt, IDM_PRIM_APPLY, argc, err)) return false;
    IdmValue arglist = args[0];
    IdmValue callee = args[1];
    size_t apply_argc = 0;
    IdmValue cursor = arglist;
    while (idm_is_pair(cursor)) {
        apply_argc++;
        cursor = idm_cdr(cursor, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cursor)) return idm_error_set(err, idm_span_unknown(NULL), "apply expects a proper list of arguments");
    if (apply_argc > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "apply argument list too long");
    IdmValue *apply_args = apply_argc == 0 ? NULL : malloc(apply_argc * sizeof(*apply_args));
    if (apply_argc != 0 && !apply_args) return idm_error_oom(err, idm_span_unknown(NULL));
    cursor = arglist;
    for (size_t i = 0; i < apply_argc; i++) {
        IdmValue item = idm_car(cursor, err);
        if (err && err->present) { free(apply_args); return false; }
        apply_args[i] = item;
        cursor = idm_cdr(cursor, err);
        if (err && err->present) { free(apply_args); return false; }
    }
    bool ok = call_value(vm, dst, callee, apply_args, (uint32_t)apply_argc, false, status, out_reason, err);
    free(apply_args);
    return ok;
}

static bool execute_actor_primitive(Vm *vm, IdmPrimitive primitive, const IdmValue *args, uint32_t argc, IdmExecStatus *status, IdmValue *out, IdmValue *out_reason, IdmError *err) {
    if (!idm_primitive_require_arity(vm->rt, primitive, argc, err)) return false;
    if (!primitive_actor_context(vm, primitive, err)) return false;
    switch (primitive) {
        case IDM_PRIM_SELF:
            *out = idm_pid(idm_actor_pid(vm->self));
            return true;
        case IDM_PRIM_SPAWN: {
            IdmValue spawnable = args[0];
            if (idm_is_closure(spawnable)) {
                IdmValue pid = idm_nil();
                if (!idm_sched_spawn(vm->sched, spawnable, vm, &pid, err)) return false;
                *out = pid;
                return true;
            }
            if (!spawn_port_process(vm, spawnable, out, "spawn", err)) return false;
            return true;
        }
        case IDM_PRIM_SPAWN_LINK: {
            IdmValue spawnable = args[0];
            bool self_exits = false;
            IdmValue exit_reason = idm_nil();
            if (idm_is_closure(spawnable)) {
                IdmValue pid = idm_nil();
                if (!idm_sched_spawn_link(vm->sched, spawnable, vm, vm->self, &pid, &self_exits, &exit_reason, err)) return false;
                *out = pid;
            } else {
                if (!spawn_port_link_process(vm, spawnable, out, &self_exits, &exit_reason, err)) return false;
            }
            if (self_exits) {
                if (!status) return idm_error_set(err, idm_span_unknown(NULL), "spawn-link exit requires a schedulable VM step");
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = exit_reason;
                return true;
            }
            return true;
        }
        case IDM_PRIM_SPAWN_MONITOR: {
            IdmValue spawnable = args[0];
            IdmValue proc = idm_nil();
            IdmValue ref = idm_nil();
            if (idm_is_closure(spawnable)) {
                if (!idm_sched_spawn_monitor(vm->sched, spawnable, vm, vm->self, &proc, &ref, err)) return false;
            } else {
                if (!spawn_port_monitor_process(vm, spawnable, &proc, &ref, err)) return false;
            }
            IdmValue items[2] = { proc, ref };
            IdmValue result = idm_tuple(vm->rt, items, 2u, err);
            if (idm_value_tag(result) != IDM_VAL_TUPLE) return false;
            *out = result;
            return true;
        }
        case IDM_PRIM_SEND: {
            IdmValue target = args[0];
            IdmValue msg = args[1];
            if (idm_value_tag(target) != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "send expects a pid target");
            idm_sched_send(vm->sched, idm_value_id(target), msg);
            *out = msg;
            return true;
        }
        case IDM_PRIM_EXIT: {
            if (argc == 2u) {
                IdmValue target = args[0];
                IdmValue reason = args[1];
                if (idm_value_tag(target) != IDM_VAL_PID && idm_value_tag(target) != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "exit signal expects a process target");
                bool self_exits = false;
                IdmValue exit_reason = idm_nil();
                if (!idm_sched_exit_signal(vm->sched, vm->self, target, reason, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    if (!status) return idm_error_set(err, idm_span_unknown(NULL), "exit signal requires a schedulable VM step");
                    *status = IDM_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                *out = idm_atom(vm->rt, "ok");
                return true;
            }
            IdmValue reason = idm_int(0);
            if (argc == 1u) reason = args[0];
            if (!status) return idm_error_set(err, idm_span_unknown(NULL), "exit requires a schedulable VM step");
            *status = IDM_EXEC_EXIT;
            if (out_reason) *out_reason = reason;
            return true;
        }
        case IDM_PRIM_LINK: {
            IdmValue target = args[0];
            if (idm_value_tag(target) != IDM_VAL_PID && idm_value_tag(target) != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "link expects a process");
            bool self_exits = false;
            IdmValue exit_reason = idm_nil();
            if (!idm_sched_link(vm->sched, vm->self, target, &self_exits, &exit_reason, err)) return false;
            if (self_exits) {
                if (!status) return idm_error_set(err, idm_span_unknown(NULL), "link exit requires a schedulable VM step");
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = exit_reason;
                return true;
            }
            *out = idm_atom(vm->rt, "ok");
            return true;
        }
        case IDM_PRIM_UNLINK: {
            IdmValue target = args[0];
            if (idm_value_tag(target) != IDM_VAL_PID && idm_value_tag(target) != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "unlink expects a process");
            if (!idm_sched_unlink(vm->sched, vm->self, target, err)) return false;
            *out = idm_atom(vm->rt, "ok");
            return true;
        }
        case IDM_PRIM_MONITOR: {
            IdmValue target = args[0];
            if (idm_value_tag(target) != IDM_VAL_PID && idm_value_tag(target) != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "monitor expects a process");
            IdmValue ref = idm_nil();
            if (!idm_sched_monitor(vm->sched, vm->self, target, &ref, err)) return false;
            *out = ref;
            return true;
        }
        case IDM_PRIM_DEMONITOR: {
            IdmValue ref = args[0];
            if (idm_value_tag(ref) != IDM_VAL_REF) return idm_error_set(err, idm_span_unknown(NULL), "demonitor expects a reference");
            idm_sched_demonitor(vm->sched, vm->self, ref);
            *out = idm_atom(vm->rt, "ok");
            return true;
        }
        case IDM_PRIM_TRAP_EXIT: {
            IdmValue flag = args[0];
            bool previous = idm_actor_trap_exit_get(vm->self);
            idm_actor_trap_exit_set(vm->self, idm_value_ok(flag));
            *out = idm_atom(vm->rt, previous ? "true" : "false");
            return true;
        }
        default:
            return false;
    }
}

static bool frame_reg_value(const Frame *frame, uint32_t reg, IdmValue *out, IdmError *err) {
    if (reg >= frame->reg_count) return idm_error_set(err, idm_span_unknown(NULL), "register r%u out of bounds", reg);
    *out = frame->regs[reg];
    return true;
}

static bool frame_reg_write(Frame *frame, uint32_t reg, IdmValue value, IdmError *err) {
    if (reg >= frame->reg_count) return idm_error_set(err, idm_span_unknown(NULL), "register r%u out of bounds", reg);
    frame->regs[reg] = value;
    return true;
}

static bool frame_reg_slice(Frame *frame, uint32_t first, uint32_t count, const IdmValue **out, IdmError *err) {
    if (count == 0) {
        *out = NULL;
        return true;
    }
    if (first >= frame->reg_count || count > frame->reg_count - first) return idm_error_set(err, idm_span_unknown(NULL), "register range r%u..r%u out of bounds", first, first + count - 1u);
    *out = &frame->regs[first];
    return true;
}

static bool write_current_reg(Vm *vm, uint32_t reg, IdmValue value, IdmError *err) {
    Frame *frame = current_frame(vm);
    if (!frame) {
        vm->direct_result = value;
        vm->has_direct_result = true;
        (void)reg;
        return true;
    }
    return frame_reg_write(frame, reg, value, err);
}

static bool execute_primitive_clause(Vm *vm, uint32_t primitive, uint32_t dst, const IdmValue *args, uint32_t argc, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (vm->self && vm->sched && (primitive == IDM_PRIM_TTY_READ || primitive == IDM_PRIM_TTY_READ_LINE)) {
        return blocking_dispatch(vm, status, dst, (IdmPrimitive)primitive, op_tty_block, IDM_EXEC_BLOCK_TTY, args, argc, err);
    }
    if (vm->self && vm->sched && primitive == IDM_PRIM_PORT_READ) {
        return blocking_dispatch(vm, status, dst, (IdmPrimitive)primitive, op_port_read_block, IDM_EXEC_BLOCK_PORT_READ, args, argc, err);
    }
    if (vm->self && vm->sched && primitive == IDM_PRIM_PORT_WRITE) {
        return blocking_dispatch(vm, status, dst, (IdmPrimitive)primitive, op_port_write_block, IDM_EXEC_BLOCK_PORT_WRITE, args, argc, err);
    }
    IdmValue out = idm_nil();
    switch ((IdmPrimitive)primitive) {
        case IDM_PRIM_APPLY:
            return execute_apply_primitive(vm, dst, args, argc, status, out_reason, err);
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
            if (!execute_actor_primitive(vm, (IdmPrimitive)primitive, args, argc, status, &out, out_reason, err)) return false;
            if (status && *status == IDM_EXEC_EXIT) return true;
            return write_current_reg(vm, dst, out, err);
        default:
            break;
    }
    if (!generic_primitive_clause(vm, primitive, args, argc, &out, err)) return false;
    return write_current_reg(vm, dst, out, err);
}

#define IDM_GUARD_BUDGET (1 << 20)
#define IDM_GUARD_FRAME_SLOTS 4u
#define IDM_GUARD_HANDLER_SLOTS 2u

static bool vm_run_loop(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool vm_run_loop_inner(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool enter_clause(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, IdmValue closure, IdmEnv *env, const IdmValue *captures, uint32_t capture_count, uint32_t dst, const IdmValue *args, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err);

static bool vm_run_call_with_exit_message(Vm *vm, int64_t budget, IdmExecStatus *out_status, IdmValue *out, const char *outside_message, IdmError *err) {
    IdmRuntime *rt = vm->rt;
    IdmExecStatus status = IDM_EXEC_DONE;
    IdmValue result = idm_nil();
    IdmValue reason = idm_nil();
    bool ok = vm_run_loop(vm, &budget, &status, &result, &reason, err);
    vm_reset(vm);
    if (out_status) *out_status = status;
    if (!ok) return false;
    if (status == IDM_EXEC_EXIT) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        idm_error_describe(rt, reason, &buf);
        if (buf.data) idm_error_set(err, idm_span_unknown(NULL), "exited with reason %s", buf.data);
        else idm_error_set(err, idm_span_unknown(NULL), "%s", outside_message);
        idm_buf_destroy(&buf);
        return false;
    }
    if (out) *out = result;
    return true;
}

static bool vm_run_call(Vm *vm, int64_t budget, IdmExecStatus *out_status, IdmValue *out, IdmError *err) {
    return vm_run_call_with_exit_message(vm, budget, out_status, out, "exited outside the scheduler", err);
}

typedef bool (*VmBootstrapSetupFn)(Vm *vm, const void *user, IdmError *err);

typedef struct {
    uint32_t function_index;
    const IdmValue *args;
    uint32_t argc;
} VmFunctionSetup;

typedef struct {
    IdmValue closure;
    const IdmValue *args;
    uint32_t argc;
} VmClosureSetup;

static bool vm_setup_function(Vm *vm, const void *user, IdmError *err) {
    const VmFunctionSetup *setup = user;
    return push_frame(vm, vm->module, setup->function_index, idm_nil(), setup->args, setup->argc, false, 0, err);
}

static bool vm_setup_closure(Vm *vm, const void *user, IdmError *err) {
    const VmClosureSetup *setup = user;
    return call_value(vm, 0, setup->closure, setup->args, setup->argc, false, NULL, NULL, err);
}

static bool vm_bootstrap_run(IdmRuntime *rt, const IdmBytecodeModule *module, IdmVmLimits limits, IdmEnv *env, VmBootstrapSetupFn setup_fn, const void *setup, const char *outside_message, IdmValue *out, IdmError *err) {
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = limits;
    vm.root_env = env;
    if (!setup_fn(&vm, setup, err)) {
        vm_reset(&vm);
        return false;
    }
    return vm_run_call_with_exit_message(&vm, -1, NULL, out, outside_message, err);
}

static bool init_pattern_locals(Vm *vm, Frame *frame, const IdmBcFunction *fn, const IdmPatternBindings *bindings, IdmError *err) {
    (void)vm;
    if (fn->pattern_local_count == 0) return true;
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) {
        uint32_t slot = fn->pattern_locals[i].slot;
        if (slot >= fn->local_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern local slot out of bounds");
        const IdmValue *bound = bindings ? idm_pattern_bindings_get_slot(bindings, slot) : NULL;
        if (!bound && bindings) bound = idm_pattern_bindings_get(bindings, fn->pattern_locals[i].name);
        if (!bound) return idm_error_set(err, idm_span_unknown(NULL), "pattern binding '%s' missing", fn->pattern_locals[i].name);
        uint32_t reg = fn->arity + slot;
        if (reg >= frame->reg_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern local register out of bounds");
        if (!frame_reg_write(frame, reg, *bound, err)) return false;
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

typedef struct {
    Vm *vm;
    const IdmBytecodeModule *module;
    IdmValue closure;
    IdmEnv *env;
    const IdmValue *captures;
    uint32_t capture_count;
    ClauseArgs args;
    bool exhausted;
} ClauseGuardCtx;

static bool run_guard_function(Vm *caller, const IdmBytecodeModule *module, IdmValue closure, IdmEnv *env, const IdmValue *captures, uint32_t capture_count, const IdmBcFunction *candidate, const IdmPatternBindings *bindings, ClauseArgs args, bool *out_pass, bool *out_exhausted, IdmError *err) {
    *out_pass = true;
    if (!candidate->has_guard) return true;
    if (!module) return idm_error_set(err, idm_span_unknown(NULL), "function guard requires a module");
    if (candidate->guard_function >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    if (!idm_arity_accepts(&module->functions[candidate->guard_function].call_arity, args.count)) return idm_error_set(err, idm_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    Frame guard_frames[IDM_GUARD_FRAME_SLOTS];
    Handler guard_handlers[IDM_GUARD_HANDLER_SLOTS];
    vm_borrow_storage(&guard_vm, guard_frames, IDM_GUARD_FRAME_SLOTS, guard_handlers, IDM_GUARD_HANDLER_SLOTS);
    guard_vm.rt = caller->rt;
    guard_vm.module = module;
    guard_vm.limits = caller->limits;
    guard_vm.root_env = env ? env : (caller->root_env ? caller->root_env : caller->rt->main_env);
    if (!enter_clause(&guard_vm, module, candidate->guard_function, closure, env, captures, capture_count, 0u, args.values, args.count, bindings, false, NULL, NULL, err)) { vm_reset(&guard_vm); return false; }

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
    if (status != IDM_EXEC_DONE) {
        *out_pass = false;
        return true;
    }
    *out_pass = idm_value_ok(result);
    return true;
}

static bool selector_guard(void *user, uint32_t function_index, const IdmPatternBindings *bindings, bool *out_pass, IdmError *err) {
    ClauseGuardCtx *ctx = user;
    if (function_index >= ctx->module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "selector chose invalid function %u", function_index);
    const IdmBcFunction *candidate = &ctx->module->functions[function_index];
    return run_guard_function(ctx->vm, ctx->module, ctx->closure, ctx->env, ctx->captures, ctx->capture_count, candidate, bindings, ctx->args, out_pass, &ctx->exhausted, err);
}

static bool run_selector(Vm *vm, IdmPatternSelector *selector, ClauseGuardCtx *guard_ctx, ClauseSelection *selection, IdmError *err) {
    clause_selection_init(selection);
    bool sel_ok = idm_pattern_selector_select(vm->rt, selector, guard_ctx->args.values, guard_ctx->args.count, selector_guard, guard_ctx, &selection->function_index, &selection->bindings, &selection->has_bindings, &selection->matched, err);
    selection->guard_budget_exhausted = guard_ctx->exhausted;
    if (!sel_ok) clause_selection_destroy(selection);
    return sel_ok;
}

static bool select_ready_closure_clause(Vm *vm, IdmValue callee, ClauseArgs args, ClauseSelection *selection, IdmError *err) {
    clause_selection_init(selection);
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    IdmPatternSelector *selector = idm_closure_selector(callee);
    if (!selector) {
        clause_selection_destroy(selection);
        return idm_error_set(err, idm_span_unknown(NULL), "closure has no clause selector");
    }
    ClauseGuardCtx guard_ctx;
    guard_ctx.vm = vm;
    guard_ctx.module = callee_module;
    guard_ctx.closure = callee;
    guard_ctx.env = NULL;
    guard_ctx.captures = NULL;
    guard_ctx.capture_count = 0;
    guard_ctx.args = args;
    guard_ctx.exhausted = false;
    return run_selector(vm, selector, &guard_ctx, selection, err);
}

static bool select_closure_clause(Vm *vm, IdmValue callee, ClauseArgs args, ClauseSelection *selection, IdmError *err) {
    if (!closure_selector_ready(vm, callee, err)) return false;
    return select_ready_closure_clause(vm, callee, args, selection, err);
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

static bool replace_current_frame(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, IdmValue closure, IdmEnv *env, const IdmValue *captures, uint32_t capture_count, const IdmValue *args, uint32_t argc, const IdmPatternBindings *bindings, IdmError *err) {
    Frame *frame = current_frame(vm);
    if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry without frame");
    const IdmBcFunction *fn = &module->functions[function_index];
    if (fn->arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "function arity mismatch: expected %u got %u", fn->arity, argc);
    if (fn->register_count > vm->limits.max_registers) return idm_error_set(err, idm_span_unknown(NULL), "VM register limit exceeded");
    IdmEnv *entry_env = idm_is_closure(closure) ? idm_closure_env(closure) : (env ? env : frame->env);
    if (!entry_env) {
        return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry requires an explicit runtime environment");
    }
    bool has_return = frame->has_return;
    uint32_t return_reg = frame->return_reg;
    if (!frame_set_captures(frame, captures, capture_count, err)) return false;
    if (!frame_install_registers(frame, fn->register_count, args, argc, err)) return false;
    frame->module = module;
    frame->function_index = function_index;
    frame->ip = fn->entry;
    frame->return_reg = return_reg;
    frame->has_return = has_return;
    frame->closure = closure;
    frame->env = entry_env;
    return init_pattern_locals(vm, frame, fn, bindings, err);
}

static bool can_tail_replace(const Vm *vm) {
    if (vm->frame_count == 0) return false;
    size_t depth = vm->frame_count;
    for (size_t i = 0; i < vm->handler_count; i++) {
        if (vm->handlers[i].frame_count >= depth) return false;
    }
    return true;
}

static bool enter_clause(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, IdmValue closure, IdmEnv *env, const IdmValue *captures, uint32_t capture_count, uint32_t dst, const IdmValue *args, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    const bool has_closure = idm_is_closure(closure);
    const IdmBytecodeModule *entry_module = module ? module : (has_closure ? closure_module_or_current(vm, closure) : vm->module);
    if (!entry_module) return idm_error_set(err, idm_span_unknown(NULL), has_closure ? "closure has no bytecode module" : "direct clause entry requires a module");
    if (function_index >= entry_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "clause function index %u out of bounds", function_index);
    const IdmBcFunction *fn = &entry_module->functions[function_index];
    if (fn->primitive_backed) {
        if (bindings) return idm_error_set(err, idm_span_unknown(NULL), "primitive-backed clause cannot bind patterns");
        return execute_primitive_clause(vm, fn->primitive, dst, args, argc, status, out_reason, err);
    }
    if (!tail || !can_tail_replace(vm)) {
        bool has_return = current_frame(vm) != NULL;
        const IdmValue *safe_captures = captures;
        IdmValue *owned_captures = NULL;
        if (!protect_values_for_frame_push(vm, &safe_captures, capture_count, &owned_captures, err)) return false;
        if (!push_frame(vm, entry_module, function_index, closure, args, argc, has_return, dst, err)) {
            free(owned_captures);
            return false;
        }
        Frame *frame = current_frame(vm);
        if (!has_closure && env) frame->env = env;
        if (!has_closure && !frame->env) {
            free(owned_captures);
            return idm_error_set(err, idm_span_unknown(NULL), "direct clause entry requires an explicit runtime environment");
        }
        if (!frame_set_captures(frame, safe_captures, capture_count, err)) {
            free(owned_captures);
            return false;
        }
        free(owned_captures);
        return init_pattern_locals(vm, frame, fn, bindings, err);
    }
    return replace_current_frame(vm, entry_module, function_index, closure, env, captures, capture_count, args, argc, bindings, err);
}

static bool execute_match_op(Vm *vm, Frame *frame, const IdmBytecodeModule *module, size_t instr_ip, const uint32_t *operands, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    uint32_t dst = operands[0];
    uint32_t first_arg = operands[1];
    uint32_t argc = operands[2];
    uint32_t entry_count = operands[3];
    uint32_t capture_count = operands[4];
    uint32_t first_capture = operands[5];
    bool tail = operands[6] != 0;
    if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "MATCH requires entries");
    const IdmValue *args = NULL;
    const IdmValue *captures = NULL;
    if (!frame_reg_slice(frame, first_arg, argc, &args, err)) return false;
    if (!frame_reg_slice(frame, first_capture, capture_count, &captures, err)) return false;
    IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
    if (!selector) return idm_error_set(err, idm_span_unknown(NULL), "MATCH selector missing");
    ClauseSelection selected;
    ClauseArgs clause_args = clause_args_from(args, argc);
    ClauseGuardCtx guard_ctx;
    guard_ctx.vm = vm;
    guard_ctx.module = module;
    guard_ctx.closure = idm_nil();
    guard_ctx.env = frame->env;
    guard_ctx.captures = captures;
    guard_ctx.capture_count = capture_count;
    guard_ctx.args = clause_args;
    guard_ctx.exhausted = false;
    if (!run_selector(vm, selector, &guard_ctx, &selected, err)) return false;
    if (!selected.matched) {
        bool budget_exhausted = selected.guard_budget_exhausted;
        clause_selection_destroy(&selected);
        if (budget_exhausted) return raise_guard_budget("match", err);
        return raise_no_clause(vm, "match", clause_args, err);
    }
    bool entered = enter_clause(vm, module, selected.function_index, idm_nil(), frame->env, captures, capture_count, dst, args, argc, selected.has_bindings ? &selected.bindings : NULL, tail, status, out_reason, err);
    clause_selection_destroy(&selected);
    return entered;
}

static bool execute_call_op(Vm *vm, Frame *frame, const uint32_t *operands, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    uint32_t dst = operands[0];
    uint32_t callee_reg = operands[1];
    uint32_t first_arg = operands[2];
    uint32_t argc = operands[3];
    bool tail = operands[4] != 0;
    IdmValue callee = idm_nil();
    if (!frame_reg_value(frame, callee_reg, &callee, err)) return false;
    const IdmValue *args = NULL;
    if (!frame_reg_slice(frame, first_arg, argc, &args, err)) return false;
    return call_value(vm, dst, callee, args, argc, tail, status, out_reason, err);
}

static bool call_closure_clause(Vm *vm, uint32_t dst, IdmValue callee, const IdmValue *arg_values, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!idm_is_closure(callee)) {
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    if (idm_closure_entry_count(callee) == 1u) {
        if (!closure_selector_ready(vm, callee, err)) return false;
        const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
        if (!callee_module) return idm_error_set(err, idm_span_unknown(NULL), "closure has no bytecode module");
        uint32_t entry = idm_closure_function_index(callee);
        if (entry >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "closure references invalid function %u", entry);
        const IdmBcFunction *fn = &callee_module->functions[entry];
        bool trivial = fn->primitive_backed || fn->trivial_match || fn->pattern_count == 0;
        if (trivial && !fn->has_guard && fn->pattern_local_count == 0 && idm_arity_accepts(&fn->call_arity, argc)) {
            return enter_clause(vm, callee_module, entry, callee, NULL, NULL, 0, dst, arg_values, argc, NULL, tail, status, out_reason, err);
        }
    }
    ClauseSelection selected;
    ClauseArgs args = clause_args_from(arg_values, argc);
    bool selected_ok = idm_closure_entry_count(callee) == 1u ?
                       select_ready_closure_clause(vm, callee, args, &selected, err) :
                       select_closure_clause(vm, callee, args, &selected, err);
    if (!selected_ok) return false;
    if (!selected.matched) {
        const IdmBytecodeModule *cm = closure_module_or_current(vm, callee);
        const char *cname = NULL;
        uint32_t first_entry = idm_closure_entry_count(callee) != 0 ? idm_closure_entry(callee, 0, NULL) : idm_closure_function_index(callee);
        if (cm && first_entry < cm->function_count) cname = cm->functions[first_entry].name;
        IdmArity arity = idm_arity_unknown();
        (void)idm_closure_arity(callee, &arity);
        bool budget_exhausted = selected.guard_budget_exhausted;
        clause_selection_destroy(&selected);
        if (budget_exhausted) return raise_guard_budget(cname, err);
        if (arity.kind != IDM_ARITY_UNKNOWN && !idm_arity_accepts(&arity, argc)) return raise_call_arity(vm, cname, argc, arity.min, arity.max, err);
        return raise_no_clause(vm, cname, args, err);
    }
    bool entered = enter_clause(vm, NULL, selected.function_index, callee, NULL, NULL, 0, dst, arg_values, argc, selected.has_bindings ? &selected.bindings : NULL, tail, status, out_reason, err);
    clause_selection_destroy(&selected);
    return entered;
}

static bool call_value(Vm *vm, uint32_t dst, IdmValue callee, const IdmValue *args, uint32_t argc, bool tail, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    if (!idm_is_closure(callee)) {
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    return call_closure_clause(vm, dst, callee, args, argc, tail, status, out_reason, err);
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

static bool op_recv(Vm *vm, Frame *frame, size_t instr_ip, const uint32_t *operands, IdmExecStatus *status, IdmValue *out_reason, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t dst = operands[0];
    uint32_t timeout_reg = operands[1];
    uint32_t receiver_reg = operands[2];
    uint32_t timeout_label = operands[3];
    bool tail = operands[4] != 0;
    if (!require_actor(vm, err)) return false;
    IdmValue closure = idm_nil();
    IdmValue timeout = idm_nil();
    if (!frame_reg_value(frame, receiver_reg, &closure, err)) return false;
    if (!frame_reg_value(frame, timeout_reg, &timeout, err)) return false;
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
        IdmValue arg = removed;
        bool ok = enter_clause(vm, NULL, selection.clause.function_index, closure, NULL, NULL, 0, dst, &arg, 1u, selection.clause.has_bindings ? &selection.clause.bindings : NULL, tail, status, out_reason, err);
        receive_selection_destroy(&selection);
        return ok;
    }

    receive_cursor_no_match(&cursor);
    receive_selection_destroy(&selection);
    IdmRecvDecision decision = IDM_RECV_BLOCK;
    if (!idm_actor_recv_no_match(vm->self, timeout, &decision, err)) return false;
    if (decision == IDM_RECV_TIMEOUT) {
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
    if (idm_value_tag(value) == IDM_VAL_ATOM || idm_value_tag(value) == IDM_VAL_WORD) return idm_symbol_text(idm_value_symbol(value));
    if (idm_value_tag(value) == IDM_VAL_STRING) return idm_string_bytes(value);
    idm_error_set(err, idm_span_unknown(NULL), "%s constant must be a name", what);
    return NULL;
}

static IdmSymbol *module_const_symbol(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t index, const char *what, IdmError *err) {
    if (index >= module->const_count) {
        idm_error_set(err, idm_span_unknown(NULL), "%s constant %u out of bounds", what, index);
        return NULL;
    }
    IdmValue value = module->constants[index];
    if (idm_value_tag(value) == IDM_VAL_ATOM || idm_value_tag(value) == IDM_VAL_WORD) return idm_value_symbol(value);
    if (idm_value_tag(value) == IDM_VAL_STRING) {
        IdmSymbol *sym = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, idm_string_bytes(value));
        if (!sym) idm_error_oom(err, idm_span_unknown(NULL));
        return sym;
    }
    idm_error_set(err, idm_span_unknown(NULL), "%s constant must be a name", what);
    return NULL;
}

static const IdmTypeTerm *module_type_term(const IdmBytecodeModule *module, uint32_t index, const char *what, IdmError *err) {
    if (index == UINT32_MAX) return NULL;
    if (index >= module->type_count) {
        idm_error_set(err, idm_span_unknown(NULL), "%s type %u out of bounds", what, index);
        return NULL;
    }
    return &module->types[index];
}

static bool op_make_record(Vm *vm, Frame *frame, const uint32_t *operands, const uint32_t *field_refs, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t dst = operands[0];
    uint32_t type_const = operands[1];
    uint32_t first_field = operands[2];
    uint32_t field_count = operands[3];
    IdmSymbol *type = module_const_symbol(vm->rt, module, type_const, "MAKE_RECORD type", err);
    if (!type) return false;
    const IdmValue *field_values = NULL;
    if (!frame_reg_slice(frame, first_field, field_count, &field_values, err)) return false;
    IdmSymbol **field_names = field_count == 0 ? NULL : calloc(field_count, sizeof(*field_names));
    if (field_count != 0 && !field_names) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = true;
    for (uint32_t i = 0; i < field_count && ok; i++) {
        uint32_t field_const = field_refs[(size_t)i * 2u];
        uint32_t contract_index = field_refs[(size_t)i * 2u + 1u];
        field_names[i] = module_const_symbol(vm->rt, module, field_const, "MAKE_RECORD field", err);
        if (!field_names[i]) ok = false;
        const IdmTypeTerm *contract = ok ? module_type_term(module, contract_index, "MAKE_RECORD field contract", err) : NULL;
        if (ok && err && err->present) ok = false;
        if (ok && contract && !idm_value_matches_type_term(field_values[i], contract)) {
            IdmBuffer expected;
            idm_buf_init(&expected);
            if (!idm_type_term_write(&expected, contract)) {
                idm_buf_destroy(&expected);
                ok = idm_error_oom(err, idm_span_unknown(NULL));
            } else {
                ok = idm_error_set(err, idm_span_unknown(NULL), "record field '%s' expects %s, got %s", idm_symbol_text(field_names[i]), expected.data ? expected.data : "_", idm_value_dispatch_type_name(field_values[i]));
                idm_buf_destroy(&expected);
            }
        }
    }
    IdmValue record = idm_nil();
    if (ok) {
        IdmRecordShape *shape = idm_record_shape_intern_symbols(vm->rt, type, field_names, field_count, err);
        if (shape) record = idm_record_from_shape(vm->rt, shape, field_values, err);
        ok = !(err && err->present);
    }
    free(field_names);
    if (!ok) return false;
    return frame_reg_write(frame, dst, record, err);
}

static bool op_record_field(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t dst = operands[0];
    uint32_t receiver_reg = operands[1];
    uint32_t type_const = operands[2];
    uint32_t field_const = operands[3];
    uint32_t field_index = operands[4];
    IdmSymbol *type = module_const_symbol(vm->rt, module, type_const, "RECORD_FIELD type", err);
    if (!type) return false;
    IdmSymbol *field = module_const_symbol(vm->rt, module, field_const, "RECORD_FIELD field", err);
    if (!field) return false;
    IdmValue receiver = idm_nil();
    if (!frame_reg_value(frame, receiver_reg, &receiver, err)) return false;
    IdmValue value = idm_nil();
    if (!idm_record_field_project_symbols(receiver, type, field, field_index, &value, err)) return false;
    return frame_reg_write(frame, dst, value, err);
}

static bool op_record_is(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t dst = operands[0];
    uint32_t value_reg = operands[1];
    uint32_t type_const = operands[2];
    IdmSymbol *type = module_const_symbol(vm->rt, module, type_const, "RECORD_IS type", err);
    if (!type) return false;
    IdmValue value = idm_nil();
    if (!frame_reg_value(frame, value_reg, &value, err)) return false;
    return frame_reg_write(frame, dst, idm_bool(vm->rt, idm_record_is_symbol(value, type)), err);
}

static bool package_env_push(Vm *vm, Frame *frame, uint32_t key_const, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    if (key_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "PUSH_PACKAGE_ENV constant out of bounds");
    IdmValue key_value = module->constants[key_const];
    if (idm_value_tag(key_value) != IDM_VAL_ATOM && idm_value_tag(key_value) != IDM_VAL_WORD) return idm_error_set(err, idm_span_unknown(NULL), "PUSH_PACKAGE_ENV expects a key constant");
    IdmEnv *parent = frame->env ? frame->env : vm->rt->main_env;
    IdmEnv *child = idm_package_env_get_or_create(vm->rt, idm_symbol_text(idm_value_symbol(key_value)));
    if (!child) return idm_error_oom(err, idm_span_unknown(NULL));
    if (vm->env_save_count == vm->env_save_cap) {
        if (!idm_grow((void **)&vm->env_save, &vm->env_save_cap, sizeof(*vm->env_save), 8u, vm->env_save_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
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

static bool op_list_cons(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    uint32_t dst = operands[0];
    uint32_t head_reg = operands[1];
    uint32_t tail_reg = operands[2];
    IdmValue head = idm_nil();
    IdmValue tail = idm_nil();
    if (!frame_reg_value(frame, head_reg, &head, err) || !frame_reg_value(frame, tail_reg, &tail, err)) return false;
    IdmValue result = idm_cons(vm->rt, head, tail, err);
    if (err && err->present) return false;
    return frame_reg_write(frame, dst, result, err);
}

static bool op_list_append(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    uint32_t dst = operands[0];
    uint32_t head_reg = operands[1];
    uint32_t tail_reg = operands[2];
    IdmValue head = idm_nil();
    IdmValue tail = idm_nil();
    if (!frame_reg_value(frame, head_reg, &head, err) || !frame_reg_value(frame, tail_reg, &tail, err)) return false;
    IdmValue result = idm_nil();
    if (!idm_list_append(vm->rt, head, tail, &result, err)) return false;
    return frame_reg_write(frame, dst, result, err);
}

static bool op_make_value_sequence(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    uint32_t dst = operands[0];
    IdmValueSequenceKind kind = (IdmValueSequenceKind)operands[1];
    uint32_t first_item = operands[2];
    uint32_t count = operands[3];
    const IdmValue *items = NULL;
    if (!frame_reg_slice(frame, first_item, count, &items, err)) return false;
    IdmValue result = idm_nil();
    switch (kind) {
        case IDM_VALUE_SEQ_VECTOR:
            result = idm_vector(vm->rt, items, count, err);
            break;
        case IDM_VALUE_SEQ_TUPLE:
            result = idm_tuple(vm->rt, items, count, err);
            break;
        case IDM_VALUE_SEQ_DICT: {
            if (count % 2u != 0) return idm_error_set(err, idm_span_unknown(NULL), "dict construction requires key/value pairs");
            size_t pairs = count / 2u;
            IdmDictEntry *entries = pairs == 0 ? NULL : calloc(pairs, sizeof(*entries));
            if (pairs != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < pairs; i++) {
                entries[i].key = items[i * 2u];
                entries[i].value = items[i * 2u + 1u];
            }
            result = idm_dict(vm->rt, entries, pairs, err);
            free(entries);
            break;
        }
    }
    if (err && err->present) return false;
    return frame_reg_write(frame, dst, result, err);
}

static bool op_make_syntax(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    uint32_t dst = operands[0];
    IdmSyntaxBuildKind kind = (IdmSyntaxBuildKind)operands[1];
    uint32_t ctx_reg = operands[2];
    uint32_t payload_reg = operands[3];
    IdmValue payload = idm_nil();
    IdmValue ctx = idm_nil();
    if (!frame_reg_value(frame, ctx_reg, &ctx, err) || !frame_reg_value(frame, payload_reg, &payload, err)) return false;
    IdmValue result = idm_nil();
    if (!idm_syntax_build(vm->rt, kind, ctx, payload, &result, err)) return false;
    return frame_reg_write(frame, dst, result, err);
}

static bool op_string_concat(Vm *vm, Frame *frame, const uint32_t *operands, IdmError *err) {
    uint32_t dst = operands[0];
    uint32_t first_item = operands[1];
    uint32_t count = operands[2];
    const IdmValue *items = NULL;
    if (!frame_reg_slice(frame, first_item, count, &items, err)) return false;
    IdmValue result = idm_nil();
    if (!idm_string_concat_display(vm->rt, items, count, &result, err)) return false;
    return frame_reg_write(frame, dst, result, err);
}

static bool vm_run_loop_inner(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    IdmRuntime *rt = vm->rt;
    bool profile_ops = vm_profile_active();
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
        IdmOpcode op = (IdmOpcode)module->code[instr_ip];
        const IdmOpcodeInfo *info = idm_opcode_info(op);
        if (!info) {
            idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
            return false;
        }
        size_t width = 0;
        if (!idm_bc_instruction_width(module, instr_ip, &width, err)) return false;
        const uint32_t *operands = &module->code[instr_ip + 1u];
        const uint32_t *payload = operands + info->fixed_operands;
        frame->ip = instr_ip + width;
        if (profile_ops) vm_profile_opcode(op);
        switch (op) {
            case IDM_OP_HALT:
                *status = IDM_EXEC_DONE;
                *out_result = vm->has_direct_result ? vm->direct_result : idm_nil();
                return true;
            case IDM_OP_MOVE: {
                uint32_t dst = operands[0];
                uint32_t src = operands[1];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                if (!frame_reg_write(frame, dst, value, err)) return false;
                break;
            }
            case IDM_OP_LOAD_CONST: {
                uint32_t dst = operands[0];
                uint32_t operand = operands[1];
                if (!frame_reg_write(frame, dst, module->constants[operand], err)) return false;
                break;
            }
            case IDM_OP_LOAD_CAPTURE: {
                uint32_t dst = operands[0];
                uint32_t operand = operands[1];
                IdmValue value = idm_nil();
                if (frame->capture_count != 0 || frame->captures) {
                    if (operand >= frame->capture_count) return idm_error_set(err, idm_span_unknown(NULL), "capture index %u out of bounds", operand);
                    value = frame->captures[operand];
                } else {
                    if (idm_value_tag(frame->closure) != IDM_VAL_CLOSURE) return idm_error_set(err, idm_span_unknown(NULL), "LOAD_CAPTURE requires a closure frame");
                    if (operand >= idm_closure_capture_count(frame->closure)) return idm_error_set(err, idm_span_unknown(NULL), "capture index %u out of bounds", operand);
                    value = idm_closure_capture(frame->closure, operand, err);
                    if (err && err->present) return false;
                }
                if (!frame_reg_write(frame, dst, value, err)) return false;
                break;
            }
            case IDM_OP_MAKE_CELL: {
                uint32_t dst = operands[0];
                uint32_t src = operands[1];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                IdmValue cell = idm_cell(rt, value, err);
                if (err && err->present) return false;
                if (!frame_reg_write(frame, dst, cell, err)) return false;
                break;
            }
            case IDM_OP_LOAD_CELL: {
                uint32_t dst = operands[0];
                uint32_t cell_reg = operands[1];
                IdmValue cell = idm_nil();
                if (!frame_reg_value(frame, cell_reg, &cell, err)) return false;
                IdmValue value = idm_cell_get(cell, err);
                if (err && err->present) return false;
                if (!frame_reg_write(frame, dst, value, err)) return false;
                break;
            }
            case IDM_OP_STORE_CELL: {
                uint32_t cell_reg = operands[0];
                uint32_t value_reg = operands[1];
                IdmValue cell = idm_nil();
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, cell_reg, &cell, err) || !frame_reg_value(frame, value_reg, &value, err)) return false;
                if (!idm_cell_set(cell, value, err)) return false;
                break;
            }
            case IDM_OP_MAKE_CLOSURE: {
                uint32_t dst = operands[0];
                uint32_t operand = operands[1];
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE selector missing");
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &operand, 1u, selector, NULL, 0, frame->env, err);
                if (err && err->present) return false;
                if (!frame_reg_write(frame, dst, closure, err)) return false;
                break;
            }
            case IDM_OP_MAKE_CLOSURE_CAPTURES: {
                uint32_t dst = operands[0];
                uint32_t fn_index = operands[1];
                uint32_t first_capture = operands[2];
                uint32_t capture_count = operands[3];
                const IdmValue *capture_values = NULL;
                if (!frame_reg_slice(frame, first_capture, capture_count, &capture_values, err)) return false;
                IdmValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = capture_values[i];
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) { free(captures); return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES selector missing"); }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &fn_index, 1u, selector, captures, capture_count, frame->env, err);
                free(captures);
                if (err && err->present) return false;
                if (!frame_reg_write(frame, dst, closure, err)) return false;
                break;
            }
            case IDM_OP_MAKE_MULTI_CLOSURE: {
                uint32_t dst = operands[0];
                uint32_t entry_count = operands[1];
                uint32_t capture_count = operands[2];
                uint32_t first_capture = operands[3];
                if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE requires entries");
                uint32_t *entries = malloc((size_t)entry_count * sizeof(*entries));
                if (!entries) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) entries[i] = payload[i];
                const IdmValue *capture_values = NULL;
                if (!frame_reg_slice(frame, first_capture, capture_count, &capture_values, err)) { free(entries); return false; }
                IdmValue *captures = capture_count == 0 ? NULL : malloc((size_t)capture_count * sizeof(*captures));
                if (capture_count != 0 && !captures) { free(entries); return idm_error_oom(err, idm_span_unknown(NULL)); }
                for (uint32_t i = 0; i < capture_count; i++) captures[i] = capture_values[i];
                IdmPatternSelector *selector = idm_bc_selector_at(module, instr_ip);
                if (!selector) { free(entries); free(captures); return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE selector missing"); }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, entries, entry_count, selector, captures, capture_count, frame->env, err);
                free(entries);
                free(captures);
                if (err && err->present) return false;
                if (!frame_reg_write(frame, dst, closure, err)) return false;
                break;
            }
            case IDM_OP_CALL:
                if (!execute_call_op(vm, frame, operands, status, out_reason, err)) return false;
                break;
            case IDM_OP_CALL_PRIMITIVE: {
                uint32_t dst = operands[0];
                uint32_t primitive = operands[1];
                uint32_t first_arg = operands[2];
                uint32_t argc = operands[3];
                const IdmValue *args = NULL;
                if (!frame_reg_slice(frame, first_arg, argc, &args, err)) return false;
                if (!execute_primitive_clause(vm, primitive, dst, args, argc, status, out_reason, err)) return false;
                break;
            }
            case IDM_OP_MATCH:
                if (!execute_match_op(vm, frame, module, instr_ip, operands, status, out_reason, err)) return false;
                break;
            case IDM_OP_LIST_CONS:
                if (!op_list_cons(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_LIST_APPEND:
                if (!op_list_append(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_MAKE_VALUE_SEQUENCE:
                if (!op_make_value_sequence(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_MAKE_SYNTAX:
                if (!op_make_syntax(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_STRING_CONCAT:
                if (!op_string_concat(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_RETURN: {
                uint32_t src = operands[0];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                Frame returning;
                pop_frame(vm, &returning);
                if (vm->frame_count == 0) {
                    frame_release(&returning);
                    *status = IDM_EXEC_DONE;
                    *out_result = value;
                    return true;
                }
                Frame *caller = current_frame(vm);
                if (returning.has_return && !frame_reg_write(caller, returning.return_reg, value, err)) {
                    frame_release(&returning);
                    return false;
                }
                frame_release(&returning);
                break;
            }
            case IDM_OP_JUMP:
                frame->ip = operands[0];
                break;
            case IDM_OP_JUMP_IF_FALSE: {
                uint32_t cond = operands[0];
                uint32_t operand = operands[1];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, cond, &value, err)) return false;
                if (!idm_value_ok(value)) frame->ip = operand;
                break;
            }
            case IDM_OP_RECV:
                if (!op_recv(vm, frame, instr_ip, operands, status, out_reason, err)) return false;
                if (*status == IDM_EXEC_BLOCK_RECEIVE) return true;
                break;
            case IDM_OP_PUSH_PACKAGE_ENV:
                if (!package_env_push(vm, frame, operands[0], err)) return false;
                break;
            case IDM_OP_POP_PACKAGE_ENV:
                if (!package_env_pop(vm, frame, err)) return false;
                break;
            case IDM_OP_MAKE_RECORD:
                if (!op_make_record(vm, frame, operands, payload, err)) return false;
                break;
            case IDM_OP_RECORD_FIELD:
                if (!op_record_field(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_RECORD_IS:
                if (!op_record_is(vm, frame, operands, err)) return false;
                break;
            case IDM_OP_RESCUE_PUSH: {
                uint32_t operand = operands[0];
                if (!handler_reserve(vm, vm->handler_count + 1u, err)) return false;
                vm->handlers[vm->handler_count].catch_ip = operand;
                vm->handlers[vm->handler_count].frame_count = vm->frame_count;
                vm->handler_count++;
                break;
            }
            case IDM_OP_RESCUE_POP:
                if (vm->handler_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "RESCUE_POP with no active handler");
                vm->handler_count--;
                break;
            case IDM_OP_RAISE: {
                uint32_t src = operands[0];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                return raise_value(vm, value, err);
            }
            case IDM_OP_LOAD_RAISED: {
                uint32_t dst = operands[0];
                if (!frame_reg_write(frame, dst, vm->raised, err)) return false;
                break;
            }
            case IDM_OP_LOAD_ENV: {
                uint32_t dst = operands[0];
                uint32_t operand = operands[1];
                if (!frame_reg_write(frame, dst, idm_env_slot_get(frame->env, operand), err)) return false;
                break;
            }
            case IDM_OP_LOAD_PACKAGE_SLOT: {
                uint32_t dst = operands[0];
                uint32_t key_const = operands[1];
                uint32_t slot = operands[2];
                const char *key = module_const_text(module, key_const, "LOAD_PACKAGE_SLOT key", err);
                if (!key) return false;
                IdmEnv *env = idm_package_env_get_or_create(vm->rt, key);
                if (!env) return idm_error_oom(err, idm_span_unknown(NULL));
                if (!frame_reg_write(frame, dst, idm_env_slot_get(env, slot), err)) return false;
                break;
            }
            case IDM_OP_STORE_PACKAGE_SLOT: {
                uint32_t key_const = operands[0];
                uint32_t slot = operands[1];
                uint32_t src = operands[2];
                const char *key = module_const_text(module, key_const, "STORE_PACKAGE_SLOT key", err);
                if (!key) return false;
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                IdmEnv *env = idm_package_env_get_or_create(vm->rt, key);
                if (!env) return idm_error_oom(err, idm_span_unknown(NULL));
                if (!idm_env_slot_ensure(env, slot, err)) return false;
                if (!idm_env_slot_set(vm->rt, env, slot, value, err)) return false;
                break;
            }
            case IDM_OP_STORE_ENV: {
                uint32_t operand = operands[0];
                uint32_t src = operands[1];
                IdmValue value = idm_nil();
                if (!frame_reg_value(frame, src, &value, err)) return false;
                if (!idm_env_slot_ensure(frame->env, operand, err)) return false;
                if (!idm_env_slot_set(vm->rt, frame->env, operand, value, err)) return false;
                break;
            }
            default:
                idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
                return false;
        }
        if (*status != IDM_EXEC_DONE) return true;
    }
    *status = IDM_EXEC_DONE;
    *out_result = vm->has_direct_result ? vm->direct_result : idm_nil();
    return true;
}

static bool vm_run_loop(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    for (;;) {
        bool ok = vm_run_loop_inner(vm, budget, status, out_result, out_reason, err);
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
            frame_release(&discarded);
        }
        Frame *frame = current_frame(vm);
        if (!frame) return false;
        if (vm->has_raised) {
            vm->has_raised = false;
        } else {
            vm->raised = idm_error_reason_value(vm->rt, err);
        }
        idm_error_clear(err);
        frame->ip = handler.catch_ip;
        if (budget && *budget >= 0 && vm->sched) {
            *status = IDM_EXEC_YIELD;
            return true;
        }
    }
}

static bool ensure_module_ready(const IdmBytecodeModule *module, IdmError *err) {
    if (!idm_bc_is_finalized(module)) return idm_error_set(err, idm_span_unknown(NULL), "bytecode module is not finalized");
    if (!module->verified) return idm_error_set(err, idm_span_unknown(NULL), "bytecode module is not verified");
    return true;
}

static bool vm_run_limited_in_env(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmVmLimits limits, IdmEnv *env, IdmValue *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "vm.run");
    if (!ensure_module_ready(module, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    VmFunctionSetup setup = { function_index, NULL, 0u };
    bool ok = vm_bootstrap_run(rt, module, limits, env, vm_setup_function, &setup, "program exited outside the scheduler", out, err);
    idm_profile_count("vm.run.code_words", module ? (uint64_t)module->code_count : 0u);
    idm_profile_count("vm.run.functions", module ? (uint64_t)module->function_count : 0u);
    idm_profile_scope_end(&prof);
    return ok;
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
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "vm.call_function");
    if (!ensure_module_ready(module, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    if (function_index >= module->function_count) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "function index %u out of bounds", function_index);
    }
    VmFunctionSetup setup = { function_index, args, argc };
    bool ok = vm_bootstrap_run(rt, module, idm_vm_default_limits(), NULL, vm_setup_function, &setup, "exited outside the scheduler", out, err);
    idm_profile_scope_end(&prof);
    return ok;
}

bool idm_vm_call_closure(IdmRuntime *rt, IdmValue closure, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "vm.call_closure");
    if (!idm_is_closure(closure)) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "call target must be a function value");
    }
    const IdmBytecodeModule *module = idm_closure_module(closure);
    if (!module) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "function value has no backing module");
    }
    if (!ensure_module_ready(module, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    VmClosureSetup setup = { closure, args, argc };
    bool ok = vm_bootstrap_run(rt, module, idm_vm_default_limits(), NULL, vm_setup_closure, &setup, "exited outside the scheduler", out, err);
    idm_profile_scope_end(&prof);
    return ok;
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
        IdmGrowItem items[] = {
            { .base = (void **)&exec->env_names, .elem_size = sizeof(*exec->env_names) },
            { .base = (void **)&exec->env_values, .elem_size = sizeof(*exec->env_values) },
        };
        if (!idm_growv(items, 2u, &exec->env_cap, 8u, exec->env_count + 1u)) return false;
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
    return push_frame(exec, exec->module, function_index, idm_nil(), NULL, 0, false, 0, err);
}

bool idm_exec_setup_thunk(IdmExec *exec, IdmValue closure, IdmError *err) {
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a function value");
    if (!closure_selector_ready(exec, closure, err)) return false;
    return call_value(exec, 0, closure, NULL, 0, false, NULL, NULL, err);
}

static _Thread_local IdmExec *g_current_exec = NULL;

IdmExec *idm_current_exec(void) {
    return g_current_exec;
}

bool idm_exec_step(IdmExec *exec, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "vm.exec_step");
    if (exec->frame_count == 0) {
        *status = IDM_EXEC_DONE;
        *out_result = exec->has_direct_result ? exec->direct_result : idm_nil();
        idm_profile_scope_end(&prof);
        return true;
    }
    IdmExec *prev = g_current_exec;
    g_current_exec = exec;
    bool ok = vm_run_loop(exec, &budget, status, out_result, out_reason, err);
    g_current_exec = prev;
    idm_profile_scope_end(&prof);
    return ok;
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
    if (!exec->has_block_result_dst) return idm_error_set(err, idm_span_unknown(NULL), "no blocking result destination");
    if (exec->block_frame >= exec->frame_count) return idm_error_set(err, idm_span_unknown(NULL), "blocking result frame is no longer live");
    Frame *frame = &exec->frames[exec->block_frame];
    if (!frame_reg_write(frame, exec->block_reg, value, err)) return false;
    exec->has_block_result_dst = false;
    exec->block_frame = 0;
    exec->block_reg = 0;
    return true;
}

void idm_exec_inject_raise(IdmExec *exec, IdmValue reason) {
    if (exec->has_pending_raise) idm_gc_write_barrier(exec->pending_raise);
    exec->pending_raise = reason;
    exec->has_pending_raise = true;
}

IdmScheduler *idm_exec_scheduler(const IdmExec *exec) {
    return exec ? exec->sched : NULL;
}

void idm_exec_root_cursor_init(IdmExecRootCursor *cursor) {
    if (!cursor) return;
    cursor->frame = 0;
    cursor->section = 0;
    cursor->index = 0;
}

static bool exec_extra_root_at(const IdmExec *exec, uint32_t section, IdmValue *out) {
    switch (section) {
        case 1:
            if (!exec->has_direct_result) return false;
            *out = exec->direct_result;
            return true;
        case 2:
            *out = exec->raised;
            return true;
        case 3:
            if (!exec->has_pending_raise) return false;
            *out = exec->pending_raise;
            return true;
        case 4:
            if (!exec->has_port_read_request) return false;
            *out = exec->port_read_port;
            return true;
        case 5:
            if (!exec->has_port_write_request) return false;
            *out = exec->port_write_port;
            return true;
        case 6:
            if (!exec->has_port_write_request) return false;
            *out = exec->port_write_data;
            return true;
        default:
            return false;
    }
}

bool idm_exec_mark_roots_step(const IdmExec *exec, IdmExecRootCursor *cursor, int64_t *budget, IdmHeap *heap) {
    if (!exec || !cursor || !budget) return true;
    while (*budget > 0) {
        if (cursor->section == 0) {
            if (cursor->frame >= exec->frame_count) {
                cursor->section = 1;
                cursor->index = 0;
                continue;
            }
            const Frame *frame = &exec->frames[cursor->frame];
            uint32_t roots = frame->reg_count + frame->capture_count + 1u;
            if (cursor->index < frame->reg_count) {
                idm_gc_mark_value(heap, frame->regs[cursor->index++]);
                (*budget)--;
                continue;
            }
            if (cursor->index < frame->reg_count + frame->capture_count) {
                idm_gc_mark_value(heap, frame->captures[cursor->index++ - frame->reg_count]);
                (*budget)--;
                continue;
            }
            if (cursor->index < roots) {
                idm_gc_mark_value(heap, frame->closure);
                cursor->index++;
                (*budget)--;
                continue;
            }
            cursor->frame++;
            cursor->index = 0;
            continue;
        }
        if (cursor->section > 6u) return true;
        IdmValue value = idm_nil();
        bool present = exec_extra_root_at(exec, cursor->section, &value);
        cursor->section++;
        cursor->index = 0;
        if (!present) continue;
        idm_gc_mark_value(heap, value);
        (*budget)--;
    }
    return cursor->section > 6u;
}
