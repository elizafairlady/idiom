#include "ish/vm.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t function_index;
    size_t ip;
    size_t base;
    size_t result_base;
    IshValue *locals;
    uint32_t local_count;
    IshValue closure;
} Frame;

typedef struct {
    IshRuntime *rt;
    const IshBytecodeModule *module;
    IshValue *stack;
    size_t sp;
    size_t stack_cap;
    Frame *frames;
    size_t frame_count;
    size_t frame_cap;
    IshVmLimits limits;
} Vm;

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

static void vm_destroy(Vm *vm) {
    for (size_t i = 0; i < vm->frame_count; i++) free(vm->frames[i].locals);
    free(vm->frames);
    free(vm->stack);
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

static bool push_frame(Vm *vm, uint32_t function_index, IshValue closure, size_t base, size_t result_base, IshError *err) {
    if (function_index >= vm->module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (vm->frame_count >= vm->limits.max_frames) return ish_error_set(err, ish_span_unknown(NULL), "VM frame limit exceeded");
    if (vm->frame_count == vm->frame_cap) {
        size_t cap = vm->frame_cap ? vm->frame_cap * 2u : 32u;
        Frame *next = realloc(vm->frames, cap * sizeof(*next));
        if (!next) return ish_error_oom(err, ish_span_unknown(NULL));
        vm->frames = next;
        vm->frame_cap = cap;
    }
    const IshBcFunction *fn = &vm->module->functions[function_index];
    Frame frame;
    frame.function_index = function_index;
    frame.ip = fn->entry;
    frame.base = base;
    frame.result_base = result_base;
    frame.local_count = fn->local_count;
    frame.closure = closure;
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
    if (a.tag != ISH_VAL_INT || b.tag != ISH_VAL_INT) return ish_error_set(err, ish_span_unknown(NULL), "%s expects integer operands", ish_opcode_name(op));
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

static bool vm_run_loop(Vm *vm, IshValue *out, IshError *err);

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

static bool run_guard_function(Vm *caller, IshValue callee, const IshBcFunction *candidate, const IshPatternBindings *bindings, size_t arg_base, uint32_t argc, bool *out_pass, IshError *err) {
    *out_pass = true;
    if (!candidate->has_guard) return true;
    if (candidate->guard_function >= caller->module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    const IshBcFunction *guard_fn = &caller->module->functions[candidate->guard_function];
    if (guard_fn->arity != argc) return ish_error_set(err, ish_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    guard_vm.rt = caller->rt;
    guard_vm.module = caller->module;
    guard_vm.limits = caller->limits;
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&guard_vm, caller->stack[arg_base + i], err)) {
            vm_destroy(&guard_vm);
            return false;
        }
    }
    if (!push_frame(&guard_vm, candidate->guard_function, callee, 0, 0, err)) {
        vm_destroy(&guard_vm);
        return false;
    }
    Frame *guard_frame = current_frame(&guard_vm);
    if (!init_pattern_locals(&guard_vm, guard_frame, candidate, bindings, err)) {
        vm_destroy(&guard_vm);
        return false;
    }

    IshError guard_err;
    ish_error_init(&guard_err);
    IshValue result = ish_nil();
    bool ok = vm_run_loop(&guard_vm, &result, &guard_err);
    vm_destroy(&guard_vm);
    if (!ok || guard_err.present) {
        ish_error_clear(&guard_err);
        *out_pass = false;
        return true;
    }
    *out_pass = ish_vm_truthy(result);
    return true;
}

static bool call_value(Vm *vm, uint32_t argc, bool tail, IshError *err) {
    if (vm->sp < (size_t)argc + 1u) return ish_error_set(err, ish_span_unknown(NULL), "CALL stack underflow");
    size_t closure_index = vm->sp - (size_t)argc - 1u;
    IshValue callee = vm->stack[closure_index];
    if (!ish_is_closure(callee)) return ish_error_set(err, ish_span_unknown(NULL), "attempted to call a non-closure");
    uint32_t function_index = UINT32_MAX;
    const IshBcFunction *fn = NULL;
    IshPatternBindings selected_bindings;
    ish_pattern_bindings_init(&selected_bindings);
    bool has_selected_bindings = false;
    for (size_t i = 0; i < ish_closure_entry_count(callee); i++) {
        uint32_t candidate_index = ish_closure_entry(callee, i, err);
        if (err && err->present) return false;
        if (candidate_index >= vm->module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "closure references invalid function %u", candidate_index);
        const IshBcFunction *candidate = &vm->module->functions[candidate_index];
        if (candidate->arity != argc) continue;
        bool patterns_ok = true;
        IshPatternBindings bindings;
        ish_pattern_bindings_init(&bindings);
        if (candidate->pattern_count != 0) {
            if (candidate->pattern_count != argc) return ish_error_set(err, ish_span_unknown(NULL), "function pattern metadata arity mismatch");
            for (uint32_t p = 0; p < argc; p++) {
                if (!ish_pattern_match(vm->rt, candidate->param_patterns[p], vm->stack[closure_index + 1u + p], &bindings, err)) {
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
        if (!run_guard_function(vm, callee, candidate, &bindings, closure_index + 1u, argc, &guard_ok, err)) {
            ish_pattern_bindings_destroy(&bindings);
            return false;
        }
        if (!guard_ok) {
            ish_pattern_bindings_destroy(&bindings);
            continue;
        }
        selected_bindings = bindings;
        has_selected_bindings = true;
        function_index = candidate_index;
        fn = candidate;
        break;
    }
    if (!fn) return ish_error_set(err, ish_span_unknown(NULL), "no matching function clause for arity %u", argc);
    if (!tail) {
        if (!push_frame(vm, function_index, callee, closure_index + 1u, closure_index, err)) {
            if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
            return false;
        }
        Frame *new_frame = current_frame(vm);
        if (!init_pattern_locals(vm, new_frame, fn, &selected_bindings, err)) {
            if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
            return false;
        }
        if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
        return true;
    }

    Frame *frame = current_frame(vm);
    if (!frame) return ish_error_set(err, ish_span_unknown(NULL), "TAIL_CALL without frame");
    IshValue *args = argc == 0 ? NULL : malloc((size_t)argc * sizeof(*args));
    if (argc != 0 && !args) return ish_error_oom(err, ish_span_unknown(NULL));
    for (uint32_t i = 0; i < argc; i++) args[i] = vm->stack[closure_index + 1u + i];
    size_t result_base = frame->result_base;
    free(frame->locals);
    frame->function_index = function_index;
    frame->ip = fn->entry;
    frame->base = result_base;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->closure = callee;
    frame->locals = fn->local_count == 0 ? NULL : calloc(fn->local_count, sizeof(*frame->locals));
    if (fn->local_count != 0 && !frame->locals) { free(args); return ish_error_oom(err, ish_span_unknown(NULL)); }
    for (uint32_t i = 0; i < fn->local_count; i++) frame->locals[i] = ish_nil();
    if (!init_pattern_locals(vm, frame, fn, &selected_bindings, err)) {
        free(args);
        if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
        return false;
    }
    vm->sp = result_base;
    if (!stack_reserve(vm, vm->sp + argc, err)) { free(args); return false; }
    for (uint32_t i = 0; i < argc; i++) vm->stack[vm->sp++] = args[i];
    free(args);
    if (has_selected_bindings) ish_pattern_bindings_destroy(&selected_bindings);
    return true;
}

static bool vm_run_loop(Vm *vm, IshValue *out, IshError *err) {
    const IshBytecodeModule *module = vm->module;
    IshRuntime *rt = vm->rt;
    bool done = false;
    IshValue result = ish_nil();
    while (!done) {
        Frame *frame = current_frame(vm);
        if (!frame) break;
        if (frame->ip >= module->code_count) {
            ish_error_set(err, ish_span_unknown(NULL), "instruction pointer out of bounds");
            return false;
        }
        IshOpcode op = (IshOpcode)module->code[frame->ip++];
        uint32_t operand = 0;
        switch (op) {
            case ISH_OP_HALT:
                if (vm->sp != 0 && !pop(vm, &result, err)) return false;
                done = true;
                break;
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
                if (!push(vm, ish_closure(rt, operand, NULL, 0, err), err)) return false;
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
                IshValue closure = ish_closure(rt, fn_index, captures, capture_count, err);
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
                IshValue closure = ish_closure_multi(rt, entries, entry_count, captures, capture_count, err);
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
                    result = value;
                    done = true;
                } else {
                    vm->sp = result_base;
                    if (!push(vm, value, err)) return false;
                }
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
                if (a.tag != ISH_VAL_INT || b.tag != ISH_VAL_INT) return ish_error_set(err, ish_span_unknown(NULL), "LT expects integer operands");
                if (!push(vm, ish_atom(rt, a.as.i < b.as.i ? "true" : "false"), err)) return false;
                break;
            }
            default:
                ish_error_set(err, ish_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
                return false;
        }
    }
    if (out) *out = result;
    return true;
}

bool ish_vm_run_limited(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshVmLimits limits, IshValue *out, IshError *err) {
    if (!ish_bc_verify(module, err)) return false;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = limits;
    if (!push_frame(&vm, function_index, ish_nil(), 0, 0, err)) { vm_destroy(&vm); return false; }
    bool ok = vm_run_loop(&vm, out, err);
    vm_destroy(&vm);
    return ok;
}

bool ish_vm_run(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, IshValue *out, IshError *err) {
    return ish_vm_run_limited(rt, module, function_index, ish_vm_default_limits(), out, err);
}
