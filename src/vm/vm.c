#include "idiom/vm.h"

#include "idiom/actor.h"
#include "idiom/prims.h"

#include <stdint.h>
#include <limits.h>
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
    IdmNamespace *ns;
} Frame;

typedef struct {
    size_t catch_ip;
    size_t frame_count;
    size_t sp;
} Handler;

typedef struct {
    const IdmBytecodeModule *module;
    size_t site;
    IdmPatternSelector *selector;
} SelectorCacheEntry;

typedef struct {
    const IdmBytecodeModule *module;
    size_t site;
    uint32_t argc;
    int trait_phase;
    uint64_t trait_version;
    char *type;
    IdmValue impl;
    bool direct;
    uint32_t function_index;
} MethodCacheEntry;

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
    IdmNamespace **ns_save;
    size_t ns_save_count;
    size_t ns_save_cap;
    struct IdmExec *selector_cache_owner;
    SelectorCacheEntry *selector_cache;
    size_t selector_cache_count;
    size_t selector_cache_cap;
    char *cwd;
    char **env_names;
    char **env_values;
    size_t env_count;
    size_t env_cap;
    MethodCacheEntry *method_cache;
    size_t method_cache_count;
    size_t method_cache_cap;
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
    free(vm->ns_save);
    vm->ns_save = NULL;
    vm->ns_save_count = 0;
    vm->ns_save_cap = 0;
    vm->selector_cache_owner = NULL;
    for (size_t i = 0; i < vm->selector_cache_cap; i++) idm_pattern_selector_free(vm->selector_cache[i].selector);
    free(vm->selector_cache);
    vm->selector_cache = NULL;
    vm->selector_cache_count = 0;
    vm->selector_cache_cap = 0;
    for (size_t i = 0; i < vm->method_cache_count; i++) free(vm->method_cache[i].type);
    free(vm->method_cache);
    vm->method_cache = NULL;
    vm->method_cache_count = 0;
    vm->method_cache_cap = 0;
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

static inline bool vm_checked_add(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, out);
#else
    return idm_checked_add(a, b, out);
#endif
}

static inline bool vm_checked_sub(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, out);
#else
    return idm_checked_sub(a, b, out);
#endif
}

static inline bool vm_checked_mul(int64_t a, int64_t b, int64_t *out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, out);
#else
    return idm_checked_mul(a, b, out);
#endif
}

static Frame *current_frame(Vm *vm) {
    return vm->frame_count == 0 ? NULL : &vm->frames[vm->frame_count - 1u];
}

static const IdmBytecodeModule *closure_module_or_current(Vm *vm, IdmValue closure) {
    const IdmBytecodeModule *module = idm_closure_module(closure);
    return module ? module : vm->module;
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
    IdmNamespace *ns = idm_is_closure(closure) ? idm_closure_namespace(closure) : (caller ? caller->ns : vm->rt->main_ns);
    if (!ns) ns = vm->rt->main_ns;
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
    frame->ns = ns;
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    vm->frame_count++;
    return true;
}

static bool push_frame_direct(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, size_t base, size_t result_base, size_t capture_base, uint32_t capture_count, IdmNamespace *ns, IdmError *err) {
    if (!module) module = vm->module;
    if (function_index >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function index %u out of bounds", function_index);
    if (vm->frame_count >= vm->limits.max_frames) return idm_error_set(err, idm_span_unknown(NULL), "VM frame limit exceeded");
    if (!frame_reserve(vm, vm->frame_count + 1u, err)) return false;
    const IdmBcFunction *fn = &module->functions[function_index];
    Frame *frame = &vm->frames[vm->frame_count];
    frame->module = module;
    frame->function_index = function_index;
    frame->ip = fn->entry;
    frame->base = base;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->locals_base = base + fn->arity;
    frame->capture_base = capture_base;
    frame->capture_count = capture_count;
    frame->closure = idm_nil();
    frame->ns = ns ? ns : vm->rt->main_ns;
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

static bool vm_value_is_proper_list(IdmValue value) {
    while (idm_is_pair(value)) value = idm_cdr(value, NULL);
    return idm_is_empty_list(value);
}

static bool vm_fast_type_predicate(IdmPrimitive prim, IdmValue value, bool *out) {
    switch (prim) {
        case IDM_PRIM_NIL_P: *out = value.tag == IDM_VAL_NIL; return true;
        case IDM_PRIM_ATOM_P: *out = value.tag == IDM_VAL_ATOM; return true;
        case IDM_PRIM_WORD_P: *out = value.tag == IDM_VAL_WORD; return true;
        case IDM_PRIM_INT_P: *out = value.tag == IDM_VAL_INT; return true;
        case IDM_PRIM_FLOAT_P: *out = value.tag == IDM_VAL_FLOAT; return true;
        case IDM_PRIM_STRING_P: *out = value.tag == IDM_VAL_STRING; return true;
        case IDM_PRIM_PAIR_P: *out = value.tag == IDM_VAL_PAIR; return true;
        case IDM_PRIM_EMPTY_LIST_P: *out = value.tag == IDM_VAL_EMPTY_LIST; return true;
        case IDM_PRIM_LIST_P: *out = vm_value_is_proper_list(value); return true;
        case IDM_PRIM_TUPLE_P: *out = value.tag == IDM_VAL_TUPLE; return true;
        case IDM_PRIM_VECTOR_P: *out = value.tag == IDM_VAL_VECTOR; return true;
        case IDM_PRIM_DICT_P: *out = value.tag == IDM_VAL_DICT; return true;
        case IDM_PRIM_SYNTAX_P: *out = value.tag == IDM_VAL_SYNTAX; return true;
        case IDM_PRIM_CELL_P: *out = value.tag == IDM_VAL_CELL; return true;
        case IDM_PRIM_CLOSURE_P: *out = value.tag == IDM_VAL_CLOSURE; return true;
        case IDM_PRIM_PID_P: *out = value.tag == IDM_VAL_PID; return true;
        case IDM_PRIM_REF_P: *out = value.tag == IDM_VAL_REF; return true;
        case IDM_PRIM_PORT_P: *out = value.tag == IDM_VAL_PORT; return true;
        case IDM_PRIM_PRIMITIVE_P: *out = value.tag == IDM_VAL_PRIMITIVE; return true;
        case IDM_PRIM_REGEX_P:
        case IDM_PRIM_REGEX_PRED: *out = value.tag == IDM_VAL_REGEX; return true;
        case IDM_PRIM_REGEX_RESULT_P:
        case IDM_PRIM_REGEX_RESULT_PRED: *out = value.tag == IDM_VAL_REGEX_RESULT; return true;
        case IDM_PRIM_RECORD_PRED: *out = value.tag == IDM_VAL_RECORD; return true;
        default: return false;
    }
}

static bool vm_fast_equal_known(IdmValue a, IdmValue b, bool *out_equal) {
    if (a.tag != b.tag) {
        *out_equal = false;
        return true;
    }
    switch (a.tag) {
        case IDM_VAL_NIL:
        case IDM_VAL_EMPTY_LIST:
            *out_equal = true;
            return true;
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
            *out_equal = a.as.symbol == b.as.symbol;
            return true;
        case IDM_VAL_INT:
            *out_equal = a.as.i == b.as.i;
            return true;
        case IDM_VAL_FLOAT:
            *out_equal = a.as.f == b.as.f;
            return true;
        case IDM_VAL_STRING:
            *out_equal = idm_string_length(a) == idm_string_length(b) &&
                         memcmp(idm_string_bytes(a), idm_string_bytes(b), idm_string_length(a)) == 0;
            return true;
        case IDM_VAL_SYNTAX:
        case IDM_VAL_CELL:
        case IDM_VAL_CLOSURE:
        case IDM_VAL_REGEX:
        case IDM_VAL_REGEX_RESULT:
            *out_equal = a.as.obj == b.as.obj;
            return true;
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
        case IDM_VAL_PRIMITIVE:
            *out_equal = a.as.id == b.as.id;
            return true;
        case IDM_VAL_PAIR:
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR:
        case IDM_VAL_DICT:
        case IDM_VAL_RECORD:
            if (a.as.obj == b.as.obj) {
                *out_equal = true;
                return true;
            }
            return false;
    }
    return false;
}

static inline bool vm_primitive_has_fast_path(IdmPrimitive prim) {
    static const bool fast[IDM_PRIM_FILE_OPEN + 1u] = {
        [IDM_PRIM_EQ] = true,
        [IDM_PRIM_NEQ] = true,
        [IDM_PRIM_OK] = true,
        [IDM_PRIM_TUPLE_GET] = true,
        [IDM_PRIM_RECORD_PRED] = true,
        [IDM_PRIM_STR_LEN] = true,
        [IDM_PRIM_STR_BYTE] = true,
        [IDM_PRIM_REGEX_PRED] = true,
        [IDM_PRIM_REGEX_RESULT_PRED] = true,
        [IDM_PRIM_DICT_GET] = true,
        [IDM_PRIM_DICT_PUT] = true,
        [IDM_PRIM_DICT_DEL] = true,
        [IDM_PRIM_DICT_HAS] = true,
        [IDM_PRIM_DICT_SIZE] = true,
        [IDM_PRIM_NIL_P] = true,
        [IDM_PRIM_ATOM_P] = true,
        [IDM_PRIM_WORD_P] = true,
        [IDM_PRIM_INT_P] = true,
        [IDM_PRIM_FLOAT_P] = true,
        [IDM_PRIM_STRING_P] = true,
        [IDM_PRIM_PAIR_P] = true,
        [IDM_PRIM_EMPTY_LIST_P] = true,
        [IDM_PRIM_LIST_P] = true,
        [IDM_PRIM_TUPLE_P] = true,
        [IDM_PRIM_VECTOR_P] = true,
        [IDM_PRIM_DICT_P] = true,
        [IDM_PRIM_SYNTAX_P] = true,
        [IDM_PRIM_CELL_P] = true,
        [IDM_PRIM_CLOSURE_P] = true,
        [IDM_PRIM_PID_P] = true,
        [IDM_PRIM_REF_P] = true,
        [IDM_PRIM_PORT_P] = true,
        [IDM_PRIM_PRIMITIVE_P] = true,
        [IDM_PRIM_REGEX_P] = true,
        [IDM_PRIM_REGEX_RESULT_P] = true,
    };
    uint32_t index = (uint32_t)prim;
    return index <= (uint32_t)IDM_PRIM_FILE_OPEN && fast[index];
}

static bool try_fast_prim_invoke(Vm *vm, IdmPrimitive prim, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err, bool *out_done) {
    *out_done = false;
    switch (prim) {
        case IDM_PRIM_OK:
            if (argc != 1u) return true;
            *out = idm_bool(vm->rt, idm_value_ok(args[0]));
            *out_done = true;
            return true;

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
        case IDM_PRIM_PRIMITIVE_P:
        case IDM_PRIM_REGEX_P:
        case IDM_PRIM_REGEX_PRED:
        case IDM_PRIM_REGEX_RESULT_P:
        case IDM_PRIM_REGEX_RESULT_PRED:
        case IDM_PRIM_RECORD_PRED: {
            if (argc != 1u) return true;
            bool pred = false;
            if (!vm_fast_type_predicate(prim, args[0], &pred)) return true;
            *out = idm_bool(vm->rt, pred);
            *out_done = true;
            return true;
        }

        case IDM_PRIM_STR_LEN:
            if (argc != 1u || args[0].tag != IDM_VAL_STRING) return true;
            *out = idm_int((int64_t)idm_string_length(args[0]));
            *out_done = true;
            return true;

        case IDM_PRIM_DICT_SIZE:
            if (argc != 1u || args[0].tag != IDM_VAL_DICT) return true;
            *out = idm_int((int64_t)idm_dict_count(args[0]));
            *out_done = true;
            return true;

        case IDM_PRIM_EQ:
        case IDM_PRIM_NEQ: {
            if (argc != 2u) return true;
            bool equal = false;
            if (vm_fast_equal_known(args[0], args[1], &equal)) {
                *out = idm_bool(vm->rt, prim == IDM_PRIM_EQ ? equal : !equal);
                *out_done = true;
                return true;
            }
            return true;
        }

        case IDM_PRIM_TUPLE_GET: {
            if (argc != 2u || args[0].tag != IDM_VAL_TUPLE || args[1].tag != IDM_VAL_INT || args[1].as.i < 0) return true;
            size_t index = (size_t)args[1].as.i;
            if (index < idm_sequence_count(args[0])) {
                *out = idm_sequence_item(args[0], index, err);
                *out_done = true;
                return !(err && err->present);
            }
            return true;
        }

        case IDM_PRIM_STR_BYTE: {
            if (argc != 2u || args[0].tag != IDM_VAL_STRING || args[1].tag != IDM_VAL_INT) return true;
            int64_t index = args[1].as.i;
            size_t len = idm_string_length(args[0]);
            if (index < 0 || (size_t)index >= len) {
                *out = idm_nil();
            } else {
                *out = idm_int((int64_t)(unsigned char)idm_string_bytes(args[0])[index]);
            }
            *out_done = true;
            return true;
        }

        case IDM_PRIM_DICT_HAS: {
            if (argc != 2u || args[0].tag != IDM_VAL_DICT) return true;
            IdmValue found;
            *out = idm_bool(vm->rt, idm_dict_get(args[0], args[1], &found));
            *out_done = true;
            return true;
        }

        case IDM_PRIM_DICT_DEL:
            if (argc != 2u || args[0].tag != IDM_VAL_DICT) return true;
            *out = idm_dict_del(vm->rt, args[0], args[1], err);
            *out_done = true;
            return !(err && err->present);

        case IDM_PRIM_DICT_GET: {
            if (argc != 3u || args[0].tag != IDM_VAL_DICT) return true;
            IdmValue found;
            *out = idm_dict_get(args[0], args[1], &found) ? found : args[2];
            *out_done = true;
            return true;
        }

        case IDM_PRIM_DICT_PUT:
            if (argc != 3u || args[0].tag != IDM_VAL_DICT) return true;
            *out = idm_dict_put(vm->rt, args[0], args[1], args[2], err);
            *out_done = true;
            return !(err && err->present);

        default:
            return true;
    }
}

static bool generic_prim_call(Vm *vm, uint32_t primitive, uint32_t argc, IdmError *err) {
    if (vm->sp < argc) return idm_error_set(err, idm_span_unknown(NULL), "PRIM_CALL stack underflow");
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

#define IDM_GUARD_BUDGET (1 << 20)
#define IDM_GUARD_STACK_SLOTS 32u
#define IDM_GUARD_FRAME_SLOTS 4u
#define IDM_GUARD_HANDLER_SLOTS 2u

static bool vm_run_loop(Vm *vm, int64_t budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool vm_run_loop_inner(Vm *vm, int64_t *budget, IdmExecStatus *status, IdmValue *out_result, IdmValue *out_reason, IdmError *err);
static bool enter_clause(Vm *vm, IdmValue callee, uint32_t function_index, size_t closure_index, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmError *err);
static bool call_value(Vm *vm, uint32_t argc, bool tail, IdmError *err);

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

static bool build_selector_for_entries(const IdmBytecodeModule *module, const uint32_t *entries, size_t entry_count, IdmPatternSelector **out, IdmError *err) {
    *out = NULL;
    if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "selector requires at least one entry");
    IdmPatternSelectorClause *clauses = calloc(entry_count, sizeof(*clauses));
    if (!clauses) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < entry_count; i++) {
        uint32_t index = entries[i];
        if (index >= module->function_count) {
            free(clauses);
            return idm_error_set(err, idm_span_unknown(NULL), "closure references invalid function %u", index);
        }
        const IdmBcFunction *fn = &module->functions[index];
        clauses[i].function_index = index;
        clauses[i].arity = fn->arity;
        clauses[i].patterns = fn->param_patterns;
        clauses[i].pattern_count = fn->pattern_count;
        clauses[i].pattern_locals = fn->pattern_locals;
        clauses[i].pattern_local_count = fn->pattern_local_count;
        clauses[i].trivial_match = fn->trivial_match;
        clauses[i].has_guard = fn->has_guard;
    }
    bool ok = idm_pattern_selector_build(clauses, entry_count, out, err);
    free(clauses);
    return ok;
}

static size_t selector_cache_hash(const IdmBytecodeModule *module, size_t site) {
    uintptr_t h = ((uintptr_t)module >> 4) ^ (uintptr_t)(site * 16777619u);
    h ^= h >> 16;
    h *= (uintptr_t)2246822519u;
    h ^= h >> 13;
    return (size_t)h;
}

static size_t selector_cache_slot(const SelectorCacheEntry *entries, size_t cap, const IdmBytecodeModule *module, size_t site, bool *out_found) {
    size_t mask = cap - 1u;
    size_t slot = selector_cache_hash(module, site) & mask;
    for (;;) {
        const SelectorCacheEntry *entry = &entries[slot];
        if (!entry->selector) {
            *out_found = false;
            return slot;
        }
        if (entry->module == module && entry->site == site) {
            *out_found = true;
            return slot;
        }
        slot = (slot + 1u) & mask;
    }
}

static bool selector_cache_grow(Vm *vm, IdmError *err) {
    size_t new_cap = vm->selector_cache_cap ? vm->selector_cache_cap * 2u : 32u;
    SelectorCacheEntry *next = calloc(new_cap, sizeof(*next));
    if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < vm->selector_cache_cap; i++) {
        SelectorCacheEntry entry = vm->selector_cache[i];
        if (!entry.selector) continue;
        bool found = false;
        size_t slot = selector_cache_slot(next, new_cap, entry.module, entry.site, &found);
        (void)found;
        next[slot] = entry;
    }
    free(vm->selector_cache);
    vm->selector_cache = next;
    vm->selector_cache_cap = new_cap;
    return true;
}

static bool selector_for_closure_site(Vm *vm, const IdmBytecodeModule *module, size_t site, const uint32_t *entries, size_t entry_count, IdmPatternSelector **out, IdmError *err) {
    Vm *cache_vm = vm->selector_cache_owner ? vm->selector_cache_owner : vm;
    if (cache_vm->selector_cache_cap != 0) {
        bool found = false;
        size_t slot = selector_cache_slot(cache_vm->selector_cache, cache_vm->selector_cache_cap, module, site, &found);
        if (found) {
            *out = cache_vm->selector_cache[slot].selector;
            return true;
        }
    }
    IdmPatternSelector *selector = NULL;
    if (!build_selector_for_entries(module, entries, entry_count, &selector, err)) return false;
    if (cache_vm->selector_cache_cap == 0 || (cache_vm->selector_cache_count + 1u) * 4u >= cache_vm->selector_cache_cap * 3u) {
        if (!selector_cache_grow(cache_vm, err)) {
            idm_pattern_selector_free(selector);
            return false;
        }
    }
    bool found = false;
    size_t slot = selector_cache_slot(cache_vm->selector_cache, cache_vm->selector_cache_cap, module, site, &found);
    if (found) {
        idm_pattern_selector_free(selector);
        *out = cache_vm->selector_cache[slot].selector;
        return true;
    }
    cache_vm->selector_cache[slot].module = module;
    cache_vm->selector_cache[slot].site = site;
    cache_vm->selector_cache[slot].selector = selector;
    cache_vm->selector_cache_count++;
    *out = selector;
    return true;
}

static bool method_cache_lookup(Vm *vm, const IdmBytecodeModule *module, size_t site, uint32_t argc, int trait_phase, uint64_t trait_version, const char *type, IdmValue *out_impl, bool *out_direct, uint32_t *out_function_index) {
    for (size_t i = 0; i < vm->method_cache_count; i++) {
        MethodCacheEntry *entry = &vm->method_cache[i];
        if (entry->module == module && entry->site == site && entry->argc == argc && entry->trait_phase == trait_phase && entry->trait_version == trait_version && strcmp(entry->type, type) == 0) {
            *out_impl = entry->impl;
            *out_direct = entry->direct;
            *out_function_index = entry->function_index;
            return true;
        }
    }
    return false;
}

static void method_cache_store(Vm *vm, const IdmBytecodeModule *module, size_t site, uint32_t argc, int trait_phase, uint64_t trait_version, const char *type, IdmValue impl, bool direct, uint32_t function_index) {
    for (size_t i = 0; i < vm->method_cache_count; i++) {
        MethodCacheEntry *entry = &vm->method_cache[i];
        if (entry->module == module && entry->site == site && entry->argc == argc && entry->trait_phase == trait_phase && strcmp(entry->type, type) == 0) {
            entry->trait_version = trait_version;
            entry->impl = impl;
            entry->direct = direct;
            entry->function_index = function_index;
            return;
        }
    }
    if (vm->method_cache_count == vm->method_cache_cap) {
        size_t cap = vm->method_cache_cap ? vm->method_cache_cap * 2u : 16u;
        MethodCacheEntry *grown = realloc(vm->method_cache, cap * sizeof(*grown));
        if (!grown) return;
        vm->method_cache = grown;
        vm->method_cache_cap = cap;
    }
    char *type_copy = idm_strdup(type);
    if (!type_copy) return;
    MethodCacheEntry *entry = &vm->method_cache[vm->method_cache_count++];
    entry->module = module;
    entry->site = site;
    entry->argc = argc;
    entry->trait_phase = trait_phase;
    entry->trait_version = trait_version;
    entry->type = type_copy;
    entry->impl = impl;
    entry->direct = direct;
    entry->function_index = function_index;
}

static bool run_guard_function(Vm *caller, const IdmBytecodeModule *callee_module, IdmValue callee, const IdmBcFunction *candidate, const IdmPatternBindings *bindings, size_t arg_base, uint32_t argc, bool *out_pass, bool *out_exhausted, IdmError *err) {
    *out_pass = true;
    if (!candidate->has_guard) return true;
    if (candidate->guard_function >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    if (callee_module->functions[candidate->guard_function].arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    IdmValue guard_stack[IDM_GUARD_STACK_SLOTS];
    Frame guard_frames[IDM_GUARD_FRAME_SLOTS];
    Handler guard_handlers[IDM_GUARD_HANDLER_SLOTS];
    vm_borrow_storage(&guard_vm, guard_stack, IDM_GUARD_STACK_SLOTS, guard_frames, IDM_GUARD_FRAME_SLOTS, guard_handlers, IDM_GUARD_HANDLER_SLOTS);
    guard_vm.rt = caller->rt;
    guard_vm.module = callee_module;
    guard_vm.limits = caller->limits;
    guard_vm.selector_cache_owner = caller->selector_cache_owner ? caller->selector_cache_owner : caller;
    if (!push(&guard_vm, callee, err)) { vm_reset(&guard_vm); return false; }
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&guard_vm, caller->stack[arg_base + i], err)) { vm_reset(&guard_vm); return false; }
    }
    if (!enter_clause(&guard_vm, callee, candidate->guard_function, 0, argc, bindings, false, err)) { vm_reset(&guard_vm); return false; }

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

static bool run_guard_function_direct(Vm *caller, const IdmBytecodeModule *module, uint32_t function_index, const IdmPatternBindings *bindings, size_t capture_base, uint32_t capture_count, size_t arg_base, uint32_t argc, IdmNamespace *ns, bool *out_pass, bool *out_exhausted, IdmError *err) {
    *out_pass = true;
    if (function_index >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "selector chose invalid function %u", function_index);
    const IdmBcFunction *candidate = &module->functions[function_index];
    if (!candidate->has_guard) return true;
    if (candidate->guard_function >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function guard index %u out of bounds", candidate->guard_function);
    if (module->functions[candidate->guard_function].arity != argc) return idm_error_set(err, idm_span_unknown(NULL), "function guard arity mismatch");

    Vm guard_vm;
    memset(&guard_vm, 0, sizeof(guard_vm));
    IdmValue guard_stack[IDM_GUARD_STACK_SLOTS];
    Frame guard_frames[IDM_GUARD_FRAME_SLOTS];
    Handler guard_handlers[IDM_GUARD_HANDLER_SLOTS];
    vm_borrow_storage(&guard_vm, guard_stack, IDM_GUARD_STACK_SLOTS, guard_frames, IDM_GUARD_FRAME_SLOTS, guard_handlers, IDM_GUARD_HANDLER_SLOTS);
    guard_vm.rt = caller->rt;
    guard_vm.module = module;
    guard_vm.limits = caller->limits;
    guard_vm.selector_cache_owner = caller->selector_cache_owner ? caller->selector_cache_owner : caller;
    for (uint32_t i = 0; i < capture_count; i++) {
        if (!push(&guard_vm, caller->stack[capture_base + i], err)) { vm_reset(&guard_vm); return false; }
    }
    for (uint32_t i = 0; i < argc; i++) {
        if (!push(&guard_vm, caller->stack[arg_base + i], err)) { vm_reset(&guard_vm); return false; }
    }
    if (!push_frame_direct(&guard_vm, module, candidate->guard_function, capture_count, 0, 0, capture_count, ns, err)) { vm_reset(&guard_vm); return false; }
    if (!init_pattern_locals(&guard_vm, current_frame(&guard_vm), &module->functions[candidate->guard_function], bindings, err)) { vm_reset(&guard_vm); return false; }

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
    size_t arg_base;
    uint32_t argc;
    bool direct;
    size_t capture_base;
    uint32_t capture_count;
    IdmNamespace *ns;
    bool exhausted;
} SelectorGuardCtx;

static bool selector_guard(void *user, uint32_t function_index, const IdmPatternBindings *bindings, bool *out_pass, IdmError *err) {
    SelectorGuardCtx *ctx = user;
    if (function_index >= ctx->callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "selector chose invalid function %u", function_index);
    const IdmBcFunction *candidate = &ctx->callee_module->functions[function_index];
    if (ctx->direct) {
        return run_guard_function_direct(ctx->vm, ctx->callee_module, function_index, bindings, ctx->capture_base, ctx->capture_count, ctx->arg_base, ctx->argc, ctx->ns, out_pass, &ctx->exhausted, err);
    }
    return run_guard_function(ctx->vm, ctx->callee_module, ctx->callee, candidate, bindings, ctx->arg_base, ctx->argc, out_pass, &ctx->exhausted, err);
}

static bool select_trivial_closure(Vm *vm, IdmValue callee, uint32_t argc, uint32_t *out_index, bool *out_selected, IdmError *err) {
    *out_selected = false;
    if (idm_closure_entry_count(callee) != 1u) return true;
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    uint32_t candidate_index = idm_closure_function_index(callee);
    if (candidate_index >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "closure references invalid function %u", candidate_index);
    const IdmBcFunction *candidate = &callee_module->functions[candidate_index];
    if (candidate->arity != argc || candidate->has_guard) return true;
    bool trivial = candidate->pattern_count == 0 || (candidate->trivial_match && candidate->pattern_local_count == 0);
    if (!trivial) return true;
    *out_index = candidate_index;
    *out_selected = true;
    return true;
}

static bool vm_select_clause(Vm *vm, IdmValue callee, size_t arg_base, uint32_t argc, uint32_t *out_index, IdmPatternBindings *out_bindings, bool *out_has_bindings, bool *out_matched, bool *out_exhausted, IdmError *err) {
    *out_matched = false;
    *out_has_bindings = false;
    *out_index = UINT32_MAX;
    if (out_exhausted) *out_exhausted = false;
    bool selected = false;
    if (!select_trivial_closure(vm, callee, argc, out_index, &selected, err)) return false;
    if (selected) {
        *out_matched = true;
        return true;
    }
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    IdmPatternSelector *selector = idm_closure_selector(callee);
    if (selector) {
        SelectorGuardCtx guard_ctx;
        guard_ctx.vm = vm;
        guard_ctx.callee_module = callee_module;
        guard_ctx.callee = callee;
        guard_ctx.arg_base = arg_base;
        guard_ctx.argc = argc;
        guard_ctx.direct = false;
        guard_ctx.capture_base = 0;
        guard_ctx.capture_count = 0;
        guard_ctx.ns = NULL;
        guard_ctx.exhausted = false;
        bool sel_ok = idm_pattern_selector_select(vm->rt, selector, &vm->stack[arg_base], argc, selector_guard, &guard_ctx, out_index, out_bindings, out_has_bindings, out_matched, err);
        if (out_exhausted) *out_exhausted = guard_ctx.exhausted;
        return sel_ok;
    }
    bool lin_ex = false;
    for (size_t i = 0; i < idm_closure_entry_count(callee); i++) {
        uint32_t candidate_index = idm_closure_entry(callee, i, err);
        if (err && err->present) return false;
        if (candidate_index >= callee_module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "closure references invalid function %u", candidate_index);
        const IdmBcFunction *candidate = &callee_module->functions[candidate_index];
        if (candidate->arity != argc) continue;
        bool trivial = candidate->trivial_match && candidate->pattern_local_count == 0;
        bool patterns_ok = true;
        IdmPatternBindings bindings;
        idm_pattern_bindings_init(&bindings);
        if (!trivial && candidate->pattern_count != 0) {
            if (candidate->pattern_count != argc) return idm_error_set(err, idm_span_unknown(NULL), "function pattern metadata arity mismatch");
            for (uint32_t p = 0; p < argc; p++) {
                if (!idm_pattern_match(vm->rt, candidate->param_patterns[p], vm->stack[arg_base + p], &bindings, err)) {
                    patterns_ok = false;
                    break;
                }
                if (err && err->present) break;
            }
            if (err && err->present) {
                idm_pattern_bindings_destroy(&bindings);
                return false;
            }
        }
        if (!patterns_ok) {
            idm_pattern_bindings_destroy(&bindings);
            continue;
        }
        bool guard_ok = true;
        if (!run_guard_function(vm, callee_module, callee, candidate, &bindings, arg_base, argc, &guard_ok, &lin_ex, err)) {
            idm_pattern_bindings_destroy(&bindings);
            return false;
        }
        if (!guard_ok) {
            idm_pattern_bindings_destroy(&bindings);
            continue;
        }
        idm_pattern_bindings_move(out_bindings, &bindings);
        *out_has_bindings = !trivial;
        *out_index = candidate_index;
        *out_matched = true;
        return true;
    }
    if (out_exhausted) *out_exhausted = lin_ex;
    return true;
}

static bool raise_no_clause(Vm *vm, const char *name, const IdmValue *args, uint32_t argc, IdmError *err) {
    bool generated_name = generated_clause_name(name);
    const char *display_name = generated_name ? "<fn>" : name;
    const char *reason_name = generated_name ? "fn" : name;
    IdmBuffer args_buf;
    idm_buf_init(&args_buf);
    bool rendered = true;
    for (uint32_t i = 0; i < argc && rendered && args_buf.len < 160u; i++) {
        if (i != 0) rendered = idm_buf_append(&args_buf, " ");
        if (rendered) rendered = idm_value_write(&args_buf, args[i]);
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
    IdmValue args_tuple = idm_tuple(vm->rt, argc == 0 ? NULL : args, argc, NULL);
    idm_buf_destroy(&message_buf);
    return idm_error_reason(vm->rt, err, "no-clause", 2, idm_atom(vm->rt, reason_name), args_tuple);
}

static bool raise_guard_budget(const char *name, IdmError *err) {
    const char *display = (name && name[0] && !generated_clause_name(name)) ? name : "<fn>";
    return idm_error_set(err, idm_span_unknown(NULL), "guard of '%s' exceeded its budget of %d reductions", display, IDM_GUARD_BUDGET);
}

static bool enter_clause(Vm *vm, IdmValue callee, uint32_t function_index, size_t closure_index, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmError *err) {
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    const IdmBcFunction *fn = &callee_module->functions[function_index];
    if (!tail) {
        if (!push_frame(vm, callee_module, function_index, callee, closure_index + 1u, closure_index, err)) return false;
        return init_pattern_locals(vm, current_frame(vm), fn, bindings, err);
    }
    Frame *frame = current_frame(vm);
    if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry without frame");
    size_t result_base = frame->result_base;
    if (callee_module == frame->module && function_index == frame->function_index && callee.tag == IDM_VAL_CLOSURE && frame->closure.tag == IDM_VAL_CLOSURE && callee.as.obj == frame->closure.as.obj && bindings == NULL && fn->pattern_local_count == 0 && fn->arity == argc) {
        for (uint32_t i = 0; i < argc; i++) vm->stack[result_base + i] = vm->stack[closure_index + 1u + i];
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
    frame->ns = idm_is_closure(callee) ? idm_closure_namespace(callee) : frame->ns;
    if (!frame->ns) frame->ns = vm->rt->main_ns;
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    for (uint32_t i = 0; i < argc; i++) vm->stack[result_base + i] = vm->stack[closure_index + 1u + i];
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    if (!init_pattern_locals(vm, frame, fn, bindings, err)) return false;
    return true;
}

static bool enter_closure_at_args(Vm *vm, IdmValue callee, uint32_t function_index, size_t arg_base, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmError *err) {
    const IdmBytecodeModule *callee_module = closure_module_or_current(vm, callee);
    const IdmBcFunction *fn = &callee_module->functions[function_index];
    if (!tail) {
        if (!push_frame(vm, callee_module, function_index, callee, arg_base, arg_base, err)) return false;
        return init_pattern_locals(vm, current_frame(vm), fn, bindings, err);
    }
    Frame *frame = current_frame(vm);
    if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail closure entry without frame");
    size_t result_base = frame->result_base;
    for (uint32_t i = 0; i < argc; i++) vm->stack[result_base + i] = vm->stack[arg_base + i];
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
    frame->ns = idm_is_closure(callee) ? idm_closure_namespace(callee) : frame->ns;
    if (!frame->ns) frame->ns = vm->rt->main_ns;
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    return init_pattern_locals(vm, frame, fn, bindings, err);
}

static bool enter_direct_clause(Vm *vm, const IdmBytecodeModule *module, uint32_t function_index, size_t capture_base, uint32_t capture_count, size_t arg_base, uint32_t argc, const IdmPatternBindings *bindings, bool tail, IdmNamespace *ns, IdmError *err) {
    const IdmBcFunction *fn = &module->functions[function_index];
    if (!tail) {
        if (!push_frame_direct(vm, module, function_index, arg_base, capture_base, capture_base, capture_count, ns, err)) return false;
        return init_pattern_locals(vm, current_frame(vm), fn, bindings, err);
    }
    Frame *frame = current_frame(vm);
    if (!frame) return idm_error_set(err, idm_span_unknown(NULL), "tail clause entry without frame");
    if (module == frame->module && function_index == frame->function_index && capture_count == 0 && frame->capture_count == 0 && bindings == NULL && fn->pattern_local_count == 0 && fn->arity == argc) {
        for (uint32_t i = 0; i < argc; i++) vm->stack[frame->base + i] = vm->stack[arg_base + i];
        frame->ip = fn->entry;
        if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
        vm->sp = frame->locals_base + fn->local_count;
        if (ns) frame->ns = ns;
        return true;
    }
    size_t result_base = frame->result_base;
    if (capture_count != 0) memmove(&vm->stack[result_base], &vm->stack[capture_base], capture_count * sizeof(*vm->stack));
    if (argc != 0) memmove(&vm->stack[result_base + capture_count], &vm->stack[arg_base], argc * sizeof(*vm->stack));
    frame->function_index = function_index;
    frame->module = module;
    frame->ip = fn->entry;
    frame->base = result_base + capture_count;
    frame->result_base = result_base;
    frame->local_count = fn->local_count;
    frame->locals_base = frame->base + fn->arity;
    frame->capture_base = result_base;
    frame->capture_count = capture_count;
    frame->closure = idm_nil();
    frame->ns = ns ? ns : vm->rt->main_ns;
    if (!stack_reserve(vm, frame->locals_base + fn->local_count, err)) return false;
    if (fn->local_count != 0) memset(&vm->stack[frame->locals_base], 0, fn->local_count * sizeof(IdmValue));
    vm->sp = frame->locals_base + fn->local_count;
    return init_pattern_locals(vm, frame, fn, bindings, err);
}

static bool call_direct(Vm *vm, const IdmBytecodeModule *module, size_t site, const uint32_t *entries, uint32_t entry_count, uint32_t capture_count, uint32_t argc, bool tail, IdmError *err) {
    if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "direct CALL requires entries");
    if (vm->sp < (size_t)capture_count + (size_t)argc) return idm_error_set(err, idm_span_unknown(NULL), "direct CALL stack underflow");
    size_t arg_base = vm->sp - (size_t)argc;
    size_t capture_base = arg_base - (size_t)capture_count;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (entries[i] >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "direct CALL function %u out of bounds", entries[i]);
    }
    if (entry_count == 1u) {
        const IdmBcFunction *fn = &module->functions[entries[0]];
        bool trivial = fn->pattern_count == 0 || (fn->trivial_match && fn->pattern_local_count == 0);
        if (fn->arity == argc && !fn->has_guard && trivial) {
            Frame *frame = current_frame(vm);
            IdmNamespace *ns = frame && frame->ns ? frame->ns : vm->rt->main_ns;
            return enter_direct_clause(vm, module, entries[0], capture_base, capture_count, arg_base, argc, NULL, tail, ns, err);
        }
    }
    IdmPatternSelector *selector = NULL;
    if (!selector_for_closure_site(vm, module, site, entries, entry_count, &selector, err)) return false;
    Frame *frame = current_frame(vm);
    IdmNamespace *ns = frame && frame->ns ? frame->ns : vm->rt->main_ns;
    SelectorGuardCtx guard_ctx;
    guard_ctx.vm = vm;
    guard_ctx.callee_module = module;
    guard_ctx.callee = idm_nil();
    guard_ctx.arg_base = arg_base;
    guard_ctx.argc = argc;
    guard_ctx.direct = true;
    guard_ctx.capture_base = capture_base;
    guard_ctx.capture_count = capture_count;
    guard_ctx.ns = ns;
    guard_ctx.exhausted = false;
    uint32_t function_index = UINT32_MAX;
    IdmPatternBindings selected_bindings;
    idm_pattern_bindings_init(&selected_bindings);
    bool has_selected_bindings = false;
    bool matched = false;
    if (!idm_pattern_selector_select(vm->rt, selector, &vm->stack[arg_base], argc, selector_guard, &guard_ctx, &function_index, &selected_bindings, &has_selected_bindings, &matched, err)) {
        idm_pattern_bindings_destroy(&selected_bindings);
        return false;
    }
    if (!matched) {
        const char *cname = entries[0] < module->function_count ? module->functions[entries[0]].name : NULL;
        idm_pattern_bindings_destroy(&selected_bindings);
        if (guard_ctx.exhausted) return raise_guard_budget(cname, err);
        return raise_no_clause(vm, cname, &vm->stack[arg_base], argc, err);
    }
    bool entered = enter_direct_clause(vm, module, function_index, capture_base, capture_count, arg_base, argc, has_selected_bindings ? &selected_bindings : NULL, tail, ns, err);
    idm_pattern_bindings_destroy(&selected_bindings);
    return entered;
}

static bool execute_call_op(Vm *vm, Frame *frame, const IdmBytecodeModule *module, size_t instr_ip, bool tail, IdmError *err) {
    uint32_t operand = module->code[frame->ip++];
    if ((operand & IDM_CALL_DIRECT_FLAG) == 0) return call_value(vm, operand, tail, err);

    uint32_t argc = operand & IDM_CALL_ARGC_MASK;
    uint32_t entry_count = module->code[frame->ip++];
    uint32_t capture_count = module->code[frame->ip++];
    const char *op_name = tail ? "TAIL_CALL" : "CALL";
    if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "%s direct requires entries", op_name);
    if (frame->ip + entry_count > module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s direct entries out of bounds", op_name);
    const uint32_t *entries = &module->code[frame->ip];
    frame->ip += entry_count;
    return call_direct(vm, module, instr_ip, entries, entry_count, capture_count, argc, tail, err);
}

static bool closure_accepts_argc(Vm *vm, IdmValue closure, uint32_t argc) {
    const IdmBytecodeModule *m = closure_module_or_current(vm, closure);
    size_t n = idm_closure_entry_count(closure);
    if (n == 0) {
        uint32_t idx = idm_closure_function_index(closure);
        return idx < m->function_count && m->functions[idx].arity == argc;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = idm_closure_entry(closure, i, NULL);
        if (idx < m->function_count && m->functions[idx].arity == argc) return true;
    }
    return false;
}

static bool call_value(Vm *vm, uint32_t argc, bool tail, IdmError *err) {
    if (vm->sp < (size_t)argc + 1u) return idm_error_set(err, idm_span_unknown(NULL), "CALL stack underflow");
    size_t closure_index = vm->sp - (size_t)argc - 1u;
    IdmValue callee = vm->stack[closure_index];
    if (callee.tag == IDM_VAL_PRIMITIVE) {
        if (argc == 0) {
            const IdmPrimitiveInfo *info = idm_primitive_info((IdmPrimitive)callee.as.id);
            if (!info || info->min_arity != 0) return true;
        }
        IdmValue *args = argc == 0 ? NULL : &vm->stack[closure_index + 1u];
        if ((IdmPrimitive)callee.as.id == IDM_PRIM_RAISE) {
            vm->sp = closure_index;
            return argc == 1u ? raise_value(vm, args[0], err) : raise_arity_error(vm, argc, err);
        }
        IdmValue out = idm_nil();
        bool fast_done = false;
        if (vm_primitive_has_fast_path((IdmPrimitive)callee.as.id) &&
            !try_fast_prim_invoke(vm, (IdmPrimitive)callee.as.id, args, argc, &out, err, &fast_done)) return false;
        if (fast_done) {
            vm->stack[closure_index] = out;
            vm->sp = closure_index + 1u;
            return true;
        }
        if (!idm_prim_invoke(vm->rt, (IdmPrimitive)callee.as.id, args, argc, &out, err)) return false;
        vm->sp = closure_index;
        return push(vm, out, err);
    }
    if (!idm_is_closure(callee)) {
        if (argc == 0) return true;
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    if (argc == 0 && !closure_accepts_argc(vm, callee, 0)) return true;
    bool selected = false;
    uint32_t function_index = UINT32_MAX;
    if (!select_trivial_closure(vm, callee, argc, &function_index, &selected, err)) return false;
    if (selected) return enter_clause(vm, callee, function_index, closure_index, argc, NULL, tail, err);
    IdmPatternBindings selected_bindings;
    idm_pattern_bindings_init(&selected_bindings);
    bool has_selected_bindings = false;
    bool matched = false;
    bool budget_exhausted = false;
    if (!vm_select_clause(vm, callee, closure_index + 1u, argc, &function_index, &selected_bindings, &has_selected_bindings, &matched, &budget_exhausted, err)) {
        idm_pattern_bindings_destroy(&selected_bindings);
        return false;
    }
    if (!matched) {
        const IdmBytecodeModule *cm = closure_module_or_current(vm, callee);
        const char *cname = NULL;
        uint32_t first_entry = idm_closure_entry_count(callee) != 0 ? idm_closure_entry(callee, 0, NULL) : idm_closure_function_index(callee);
        if (cm && first_entry < cm->function_count) cname = cm->functions[first_entry].name;
        idm_pattern_bindings_destroy(&selected_bindings);
        if (budget_exhausted) return raise_guard_budget(cname, err);
        return raise_no_clause(vm, cname, &vm->stack[closure_index + 1u], argc, err);
    }
    bool entered = enter_clause(vm, callee, function_index, closure_index, argc, has_selected_bindings ? &selected_bindings : NULL, tail, err);
    idm_pattern_bindings_destroy(&selected_bindings);
    return entered;
}

static bool call_closure_at_args(Vm *vm, IdmValue callee, size_t arg_base, uint32_t argc, bool tail, IdmError *err) {
    if (!idm_is_closure(callee)) {
        idm_error_set(err, idm_span_unknown(NULL), "attempted to call a non-closure");
        return idm_error_reason(vm->rt, err, "not-callable", 1, callee);
    }
    bool selected = false;
    uint32_t function_index = UINT32_MAX;
    if (!select_trivial_closure(vm, callee, argc, &function_index, &selected, err)) return false;
    if (selected) return enter_closure_at_args(vm, callee, function_index, arg_base, argc, NULL, tail, err);
    IdmPatternBindings selected_bindings;
    idm_pattern_bindings_init(&selected_bindings);
    bool has_selected_bindings = false;
    bool matched = false;
    bool budget_exhausted = false;
    if (!vm_select_clause(vm, callee, arg_base, argc, &function_index, &selected_bindings, &has_selected_bindings, &matched, &budget_exhausted, err)) {
        idm_pattern_bindings_destroy(&selected_bindings);
        return false;
    }
    if (!matched) {
        const IdmBytecodeModule *cm = closure_module_or_current(vm, callee);
        const char *cname = NULL;
        uint32_t first_entry = idm_closure_entry_count(callee) != 0 ? idm_closure_entry(callee, 0, NULL) : idm_closure_function_index(callee);
        if (cm && first_entry < cm->function_count) cname = cm->functions[first_entry].name;
        idm_pattern_bindings_destroy(&selected_bindings);
        if (budget_exhausted) return raise_guard_budget(cname, err);
        return raise_no_clause(vm, cname, &vm->stack[arg_base], argc, err);
    }
    bool entered = enter_closure_at_args(vm, callee, function_index, arg_base, argc, has_selected_bindings ? &selected_bindings : NULL, tail, err);
    idm_pattern_bindings_destroy(&selected_bindings);
    return entered;
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

static bool op_recv(Vm *vm, Frame *frame, size_t instr_ip, bool tail, IdmExecStatus *status, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t timeout_label = module->code[frame->ip++];
    if (!require_actor(vm, err)) return false;
    if (vm->sp < 2u) return idm_error_set(err, idm_span_unknown(NULL), "RECV stack underflow");
    IdmValue closure = vm->stack[vm->sp - 1u];
    IdmValue timeout = vm->stack[vm->sp - 2u];
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "receive requires clause closure");
    bool guard_free = closure_is_guard_free(vm, closure);

    bool matched = false;
    size_t matched_index = 0;
    uint32_t matched_fn = 0;
    IdmPatternBindings matched_bindings;
    idm_pattern_bindings_init(&matched_bindings);
    bool matched_has = false;
    size_t count = idm_actor_mailbox_count(vm->self);
    for (size_t i = idm_actor_recv_start(vm->self, module, instr_ip); i < count; i++) {
        IdmValue msg;
        if (!idm_actor_mailbox_peek(vm->self, i, &msg)) break;
        if (!push(vm, msg, err)) { if (matched_has) idm_pattern_bindings_destroy(&matched_bindings); return false; }
        uint32_t idx = UINT32_MAX;
        IdmPatternBindings b;
        idm_pattern_bindings_init(&b);
        bool has = false;
        bool m = false;
        bool ok = vm_select_clause(vm, closure, vm->sp - 1u, 1u, &idx, &b, &has, &m, NULL, err);
        vm->sp--;
        if (!ok) { if (has) idm_pattern_bindings_destroy(&b); if (matched_has) idm_pattern_bindings_destroy(&matched_bindings); return false; }
        if (m) {
            matched = true;
            matched_index = i;
            matched_fn = idx;
            idm_pattern_bindings_move(&matched_bindings, &b);
            matched_has = has;
            break;
        }
        if (has) idm_pattern_bindings_destroy(&b);
    }

    if (matched) {
        IdmValue removed;
        if (!idm_actor_mailbox_remove(vm->self, matched_index, &removed)) {
            if (matched_has) idm_pattern_bindings_destroy(&matched_bindings);
            return idm_error_set(err, idm_span_unknown(NULL), "mailbox message vanished during receive");
        }
        idm_actor_recv_matched(vm->self, guard_free ? matched_index : 0u);
        vm->sp -= 2u;
        if (!push(vm, closure, err) || !push(vm, removed, err)) { if (matched_has) idm_pattern_bindings_destroy(&matched_bindings); return false; }
        size_t closure_index = vm->sp - 2u;
        bool ok = enter_clause(vm, closure, matched_fn, closure_index, 1u, matched_has ? &matched_bindings : NULL, tail, err);
        if (matched_has) idm_pattern_bindings_destroy(&matched_bindings);
        return ok;
    }

    idm_actor_recv_set_cursor(vm->self, count);
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

static bool op_actor_unary_pid(Vm *vm, IdmOpcode op, IdmError *err) {
    IdmValue target;
    if (!pop(vm, &target, err)) return false;
    if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "%s expects a pid", idm_opcode_name(op));
    switch (op) {
        case IDM_OP_UNLINK:
            idm_sched_unlink(vm->sched, vm->self, target.as.id);
            return push(vm, idm_atom(vm->rt, "ok"), err);
        case IDM_OP_MONITOR: {
            IdmValue ref = idm_nil();
            if (!idm_sched_monitor(vm->sched, vm->self, target.as.id, &ref, err)) return false;
            return push(vm, ref, err);
        }
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "invalid actor pid op");
    }
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
    return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD trait candidate must be a name");
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

static bool lookup_method_candidates(Vm *vm, IdmValue traits, const char *method, const char *type, uint32_t argc, IdmValue *out_impl, IdmError *err) {
    size_t count = trait_candidate_count(traits);
    if (count == 0) return idm_error_set(err, idm_span_unknown(NULL), "method '%s' has no trait candidates", method);
    if (count == 1u) {
        const char *trait = NULL;
        if (!trait_candidate_at(traits, 0, &trait, err)) return false;
        return idm_trait_lookup(vm->rt, trait, method, type, argc, out_impl, err);
    }

    IdmValue match = idm_nil();
    size_t match_count = 0;
    IdmBuffer matching;
    idm_buf_init(&matching);
    for (size_t i = 0; i < count; i++) {
        const char *trait = NULL;
        if (!trait_candidate_at(traits, i, &trait, err)) {
            idm_buf_destroy(&matching);
            return false;
        }
        IdmError probe;
        idm_error_init(&probe);
        IdmValue impl = idm_nil();
        if (idm_trait_lookup(vm->rt, trait, method, type, argc, &impl, &probe)) {
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
        bool ok = idm_error_set(err, idm_span_unknown(NULL), "ambiguous method '%s' on type '%s'; matching traits: %s", method, type, matching.data ? matching.data : "?");
        idm_buf_destroy(&matching);
        return ok;
    }
    IdmBuffer candidates;
    idm_buf_init(&candidates);
    bool listed = append_trait_candidates(&candidates, traits, err);
    if (!listed && !(err && err->present)) idm_error_oom(err, idm_span_unknown(NULL));
    if (!listed) {
        idm_buf_destroy(&matching);
        idm_buf_destroy(&candidates);
        return false;
    }
    bool ok = idm_error_set(err, idm_span_unknown(NULL), "method '%s' is available via %s but is not implemented on type '%s'", method, candidates.data ? candidates.data : "?", type);
    idm_buf_destroy(&matching);
    idm_buf_destroy(&candidates);
    return ok;
}

static bool op_call_method(Vm *vm, Frame *frame, bool tail, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    size_t site = frame->ip;
    uint32_t trait_const = module->code[frame->ip++];
    uint32_t method_const = module->code[frame->ip++];
    uint32_t argc = module->code[frame->ip++];
    if (argc == 0) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD requires a receiver");
    if (vm->sp < argc) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD stack underflow");
    size_t arg_base = vm->sp - argc;
    IdmValue receiver = vm->stack[arg_base];
    const char *type = idm_value_dispatch_type_name(receiver);
    int trait_phase = vm->rt->trait_phase ? 1 : 0;
    uint64_t trait_version = idm_trait_world_version(vm->rt);
    IdmValue impl = idm_nil();
    bool direct = false;
    uint32_t function_index = UINT32_MAX;
    if (!method_cache_lookup(vm, module, site, argc, trait_phase, trait_version, type, &impl, &direct, &function_index)) {
        IdmValue traits = idm_nil();
        if (!module_const_value(module, trait_const, "CALL_METHOD trait", &traits, err)) return false;
        const char *method = module_const_text(module, method_const, "CALL_METHOD method", err);
        if (!method) return false;
        if (!lookup_method_candidates(vm, traits, method, type, argc, &impl, err)) return false;
        if (idm_is_closure(impl)) {
            bool selected = false;
            if (!select_trivial_closure(vm, impl, argc, &function_index, &selected, err)) return false;
            direct = selected;
        }
        method_cache_store(vm, module, site, argc, trait_phase, trait_version, type, impl, direct, function_index);
    }
    if (direct) return enter_closure_at_args(vm, impl, function_index, arg_base, argc, NULL, tail, err);
    return call_closure_at_args(vm, impl, arg_base, argc, tail, err);
}

static bool transfer_namespace_slots(Vm *vm, Frame *frame, IdmError *err) {
    const IdmBytecodeModule *module = frame->module ? frame->module : vm->module;
    uint32_t direction = module->code[frame->ip++];
    uint32_t count = module->code[frame->ip++];
    if (direction != (uint32_t)IDM_NS_TRANSFER_PARENT_TO_CHILD && direction != (uint32_t)IDM_NS_TRANSFER_CHILD_TO_PARENT) {
        return idm_error_set(err, idm_span_unknown(NULL), "TRANSFER_NAMESPACE direction %u is invalid", direction);
    }
    if (vm->ns_save_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "TRANSFER_NAMESPACE with no active namespace");
    IdmNamespace *outer = vm->ns_save[vm->ns_save_count - 1u];
    bool parent_to_child = direction == (uint32_t)IDM_NS_TRANSFER_PARENT_TO_CHILD;
    IdmNamespace *src = parent_to_child ? outer : frame->ns;
    IdmNamespace *dst = parent_to_child ? frame->ns : outer;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t src_slot = module->code[frame->ip++];
        uint32_t dst_slot = module->code[frame->ip++];
        if (!idm_ns_slot_ensure(dst, dst_slot, err)) return false;
        idm_ns_slot_set(dst, dst_slot, idm_ns_slot_get(src, src_slot));
    }
    return true;
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
                IdmPatternSelector *selector = NULL;
                if (!selector_for_closure_site(vm, module, instr_ip, &operand, 1u, &selector, err)) return false;
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &operand, 1u, selector, NULL, 0, frame->ns, err);
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
                IdmPatternSelector *selector = NULL;
                bool selector_ok = selector_for_closure_site(vm, module, instr_ip, &fn_index, 1u, &selector, err);
                if (!selector_ok) { free(captures); return false; }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, &fn_index, 1u, selector, captures, capture_count, frame->ns, err);
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
                IdmPatternSelector *selector = NULL;
                bool selector_ok = selector_for_closure_site(vm, module, instr_ip, entries, entry_count, &selector, err);
                if (!selector_ok) { free(entries); free(captures); return false; }
                IdmValue closure = idm_closure_multi_selectable_in_module(rt, module, entries, entry_count, selector, captures, capture_count, frame->ns, err);
                free(entries);
                free(captures);
                if (!push(vm, closure, err)) return false;
                if (err && err->present) return false;
                break;
            }
            case IDM_OP_CALL:
                if (!execute_call_op(vm, frame, module, instr_ip, false, err)) return false;
                break;
            case IDM_OP_TAIL_CALL:
                if (!execute_call_op(vm, frame, module, instr_ip, true, err)) return false;
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
            case IDM_OP_PRIM_CALL: {
                uint32_t prim = module->code[frame->ip++];
                uint32_t argc = module->code[frame->ip++];
                bool fast_done = false;
                if (argc == 2u && vm->sp >= 2u) {
                    IdmValue *lhs = &vm->stack[vm->sp - 2u];
                    IdmValue rhs = vm->stack[vm->sp - 1u];
                    if (lhs->tag == IDM_VAL_INT && rhs.tag == IDM_VAL_INT) {
                        int64_t a = lhs->as.i, b = rhs.as.i, r = 0;
                        switch (prim) {
                            case IDM_PRIM_ADD: if (vm_checked_add(a, b, &r)) { *lhs = idm_int(r); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_SUB: if (vm_checked_sub(a, b, &r)) { *lhs = idm_int(r); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_MUL: if (vm_checked_mul(a, b, &r)) { *lhs = idm_int(r); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_DIV: if (b != 0 && !(a == INT64_MIN && b == -1)) { *lhs = idm_int(a / b); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_MOD: if (b != 0 && !(a == INT64_MIN && b == -1)) { *lhs = idm_int(a % b); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_POW: if (b >= 0 && idm_checked_pow(a, b, &r)) { *lhs = idm_int(r); vm->sp--; fast_done = true; } break;
                            case IDM_PRIM_LT:  *lhs = idm_bool(rt, a < b);  vm->sp--; fast_done = true; break;
                            case IDM_PRIM_GT:  *lhs = idm_bool(rt, a > b);  vm->sp--; fast_done = true; break;
                            case IDM_PRIM_LTE: *lhs = idm_bool(rt, a <= b); vm->sp--; fast_done = true; break;
                            case IDM_PRIM_GTE: *lhs = idm_bool(rt, a >= b); vm->sp--; fast_done = true; break;
                            case IDM_PRIM_EQ:  *lhs = idm_bool(rt, a == b); vm->sp--; fast_done = true; break;
                            case IDM_PRIM_NEQ: *lhs = idm_bool(rt, a != b); vm->sp--; fast_done = true; break;
                            default: break;
                        }
                    }
                }
                if (!fast_done) {
                    if (vm->sp >= argc && vm_primitive_has_fast_path((IdmPrimitive)prim)) {
                        IdmValue out = idm_nil();
                        bool primitive_fast_done = false;
                        IdmValue *args = argc == 0 ? NULL : &vm->stack[vm->sp - argc];
                        if (!try_fast_prim_invoke(vm, (IdmPrimitive)prim, args, argc, &out, err, &primitive_fast_done)) return false;
                        if (primitive_fast_done) {
                            vm->stack[vm->sp - argc] = out;
                            vm->sp -= (size_t)argc - 1u;
                            fast_done = true;
                        }
                    }
                }
                if (!fast_done) {
                    if (vm->self && vm->sched && (prim == IDM_PRIM_TTY_READ || prim == IDM_PRIM_TTY_READ_LINE)) {
                        if (!op_tty_block(vm, (IdmPrimitive)prim, argc, err)) return false;
                        *status = IDM_EXEC_BLOCK_TTY;
                        return true;
                    }
                    if (vm->self && vm->sched && prim == IDM_PRIM_PORT_READ) {
                        if (!op_port_read_block(vm, argc, err)) return false;
                        *status = IDM_EXEC_BLOCK_PORT_READ;
                        return true;
                    }
                    if (vm->self && vm->sched && prim == IDM_PRIM_PORT_WRITE) {
                        if (!op_port_write_block(vm, argc, err)) return false;
                        *status = IDM_EXEC_BLOCK_PORT_WRITE;
                        return true;
                    }
                    if (!generic_prim_call(vm, prim, argc, err)) return false;
                }
                break;
            }
            case IDM_OP_SELF:
                if (!require_actor(vm, err)) return false;
                if (!push(vm, idm_pid(idm_actor_pid(vm->self)), err)) return false;
                break;
            case IDM_OP_SPAWN: {
                if (!require_actor(vm, err)) return false;
                IdmValue thunk;
                if (!pop(vm, &thunk, err)) return false;
                if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a function value");
                IdmValue pid = idm_nil();
                if (!idm_sched_spawn(vm->sched, thunk, vm, &pid, err)) return false;
                if (!push(vm, pid, err)) return false;
                break;
            }
            case IDM_OP_SPAWN_LINK: {
                if (!require_actor(vm, err)) return false;
                IdmValue thunk;
                if (!pop(vm, &thunk, err)) return false;
                if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-link expects a function value");
                IdmValue pid = idm_nil();
                bool self_exits = false;
                IdmValue exit_reason = idm_nil();
                if (!idm_sched_spawn_link(vm->sched, thunk, vm, vm->self, &pid, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    *status = IDM_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                if (!push(vm, pid, err)) return false;
                break;
            }
            case IDM_OP_SPAWN_MONITOR: {
                if (!require_actor(vm, err)) return false;
                IdmValue thunk;
                if (!pop(vm, &thunk, err)) return false;
                if (!idm_is_closure(thunk)) return idm_error_set(err, idm_span_unknown(NULL), "spawn-monitor expects a function value");
                IdmValue pid = idm_nil();
                IdmValue ref = idm_nil();
                if (!idm_sched_spawn_monitor(vm->sched, thunk, vm, vm->self, &pid, &ref, err)) return false;
                IdmValue items[2] = { pid, ref };
                IdmValue result = idm_tuple(rt, items, 2u, err);
                if (result.tag != IDM_VAL_TUPLE) return false;
                if (!push(vm, result, err)) return false;
                break;
            }
            case IDM_OP_SEND: {
                if (!require_actor(vm, err)) return false;
                IdmValue msg, target;
                if (!pop(vm, &msg, err) || !pop(vm, &target, err)) return false;
                if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "send expects a pid target");
                idm_sched_send(vm->sched, target.as.id, msg);
                if (!push(vm, msg, err)) return false;
                break;
            }
            case IDM_OP_EXIT: {
                IdmValue reason;
                if (!pop(vm, &reason, err)) return false;
                *status = IDM_EXEC_EXIT;
                if (out_reason) *out_reason = reason;
                return true;
            }
            case IDM_OP_EXIT_SIGNAL: {
                if (!require_actor(vm, err)) return false;
                IdmValue reason, target;
                if (!pop(vm, &reason, err) || !pop(vm, &target, err)) return false;
                if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "exit signal expects a pid target");
                bool self_exits = false;
                IdmValue exit_reason = idm_nil();
                if (!idm_sched_exit_signal(vm->sched, vm->self, target.as.id, reason, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    *status = IDM_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                if (!push(vm, idm_atom(rt, "ok"), err)) return false;
                break;
            }
            case IDM_OP_LINK: {
                if (!require_actor(vm, err)) return false;
                IdmValue target;
                if (!pop(vm, &target, err)) return false;
                if (target.tag != IDM_VAL_PID) return idm_error_set(err, idm_span_unknown(NULL), "link expects a pid");
                bool self_exits = false;
                IdmValue exit_reason = idm_nil();
                if (!idm_sched_link(vm->sched, vm->self, target.as.id, &self_exits, &exit_reason, err)) return false;
                if (self_exits) {
                    *status = IDM_EXEC_EXIT;
                    if (out_reason) *out_reason = exit_reason;
                    return true;
                }
                if (!push(vm, idm_atom(rt, "ok"), err)) return false;
                break;
            }
            case IDM_OP_UNLINK:
            case IDM_OP_MONITOR:
                if (!require_actor(vm, err)) return false;
                if (!op_actor_unary_pid(vm, op, err)) return false;
                break;
            case IDM_OP_DEMONITOR: {
                if (!require_actor(vm, err)) return false;
                IdmValue ref;
                if (!pop(vm, &ref, err)) return false;
                if (ref.tag != IDM_VAL_REF) return idm_error_set(err, idm_span_unknown(NULL), "demonitor expects a reference");
                idm_sched_demonitor(vm->sched, vm->self, ref);
                if (!push(vm, idm_atom(rt, "ok"), err)) return false;
                break;
            }
            case IDM_OP_TRAP_EXIT: {
                if (!require_actor(vm, err)) return false;
                IdmValue flag;
                if (!pop(vm, &flag, err)) return false;
                bool previous = idm_actor_trap_exit_get(vm->self);
                idm_actor_trap_exit_set(vm->self, idm_value_ok(flag));
                if (!push(vm, idm_atom(rt, previous ? "true" : "false"), err)) return false;
                break;
            }
            case IDM_OP_RECV:
                if (!op_recv(vm, frame, instr_ip, false, status, err)) return false;
                if (*status == IDM_EXEC_BLOCK_RECEIVE) return true;
                break;
            case IDM_OP_TAIL_RECV:
                if (!op_recv(vm, frame, instr_ip, true, status, err)) return false;
                if (*status == IDM_EXEC_BLOCK_RECEIVE) return true;
                break;
            case IDM_OP_EXEC: {
                if (!require_actor(vm, err)) return false;
                IdmValue graph;
                if (!pop(vm, &graph, err)) return false;
                vm->port_request = graph;
                vm->has_port_request = true;
                *status = IDM_EXEC_LAUNCH_PORT;
                return true;
            }
            case IDM_OP_AWAIT: {
                if (!require_actor(vm, err)) return false;
                IdmValue port;
                if (!pop(vm, &port, err)) return false;
                if (port.tag != IDM_VAL_PORT) return idm_error_set(err, idm_span_unknown(NULL), "await expects a port");
                vm->await_port = port;
                vm->has_await = true;
                *status = IDM_EXEC_BLOCK_AWAIT;
                return true;
            }
            case IDM_OP_APPLY: {
                IdmValue arglist;
                IdmValue callee;
                if (!pop(vm, &callee, err)) return false;
                if (!pop(vm, &arglist, err)) return false;
                size_t argc = 0;
                IdmValue cursor = arglist;
                while (idm_is_pair(cursor)) {
                    argc++;
                    cursor = idm_cdr(cursor, err);
                    if (err && err->present) return false;
                }
                if (!idm_is_empty_list(cursor)) return idm_error_set(err, idm_span_unknown(NULL), "apply expects a proper list of arguments");
                if (argc > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "apply argument list too long");
                if (!push(vm, callee, err)) return false;
                cursor = arglist;
                for (size_t i = 0; i < argc; i++) {
                    IdmValue item = idm_car(cursor, err);
                    if (err && err->present) return false;
                    if (!push(vm, item, err)) return false;
                    cursor = idm_cdr(cursor, err);
                    if (err && err->present) return false;
                }
                if (!call_value(vm, (uint32_t)argc, false, err)) return false;
                break;
            }
            case IDM_OP_ENTER_NAMESPACE: {
                operand = module->code[frame->ip++];
                if (operand >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "ENTER_NAMESPACE constant out of bounds");
                IdmValue name_value = module->constants[operand];
                if (name_value.tag != IDM_VAL_ATOM && name_value.tag != IDM_VAL_WORD) return idm_error_set(err, idm_span_unknown(NULL), "ENTER_NAMESPACE expects a name constant");
                IdmNamespace *target = idm_namespace_get_or_create(rt, idm_symbol_text(name_value.as.symbol));
                if (!target) return idm_error_oom(err, idm_span_unknown(NULL));
                if (vm->ns_save_count == vm->ns_save_cap) {
                    size_t cap = vm->ns_save_cap ? vm->ns_save_cap * 2u : 8u;
                    IdmNamespace **grown = realloc(vm->ns_save, cap * sizeof(*grown));
                    if (!grown) return idm_error_oom(err, idm_span_unknown(NULL));
                    vm->ns_save = grown;
                    vm->ns_save_cap = cap;
                }
                vm->ns_save[vm->ns_save_count++] = frame->ns;
                frame->ns = target;
                break;
            }
            case IDM_OP_TRANSFER_NAMESPACE: {
                if (!transfer_namespace_slots(vm, frame, err)) return false;
                break;
            }
            case IDM_OP_LEAVE_NAMESPACE: {
                if (vm->ns_save_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "LEAVE_NAMESPACE with no active namespace");
                frame->ns = vm->ns_save[--vm->ns_save_count];
                break;
            }
            case IDM_OP_DEFINE_TRAIT:
                if (!op_define_trait(vm, frame, err)) return false;
                break;
            case IDM_OP_IMPLEMENT_TRAIT:
                if (!op_implement_trait(vm, frame, err)) return false;
                break;
            case IDM_OP_CALL_METHOD:
                if (!op_call_method(vm, frame, false, err)) return false;
                break;
            case IDM_OP_TAIL_CALL_METHOD:
                if (!op_call_method(vm, frame, true, err)) return false;
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
            case IDM_OP_LOAD_GLOBAL:
                operand = module->code[frame->ip++];
                if (!push(vm, idm_ns_slot_get(frame->ns, operand), err)) return false;
                break;
            case IDM_OP_STORE_GLOBAL: {
                operand = module->code[frame->ip++];
                IdmValue value;
                if (!pop(vm, &value, err)) return false;
                if (!idm_ns_slot_ensure(frame->ns, operand, err)) return false;
                value = idm_value_copy(vm->rt, &vm->rt->immortal, value, err);
                if (err->present) return false;
                idm_ns_slot_set(frame->ns, operand, value);
                break;
            }
            default:
                idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u", (unsigned)op);
                return false;
        }
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

bool idm_vm_run_limited(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmVmLimits limits, IdmValue *out, IdmError *err) {
    if (!idm_bc_verify(module, err)) return false;
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.rt = rt;
    vm.module = module;
    vm.limits = limits;
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

bool idm_vm_run(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, IdmValue *out, IdmError *err) {
    return idm_vm_run_limited(rt, module, function_index, idm_vm_default_limits(), out, err);
}

bool idm_vm_call_function(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!idm_bc_verify(module, err)) return false;
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
    if (!call_value(&vm, argc, false, err)) {
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
    return push_frame(exec, exec->module, function_index, idm_nil(), 0, 0, err);
}

bool idm_exec_setup_thunk(IdmExec *exec, IdmValue closure, IdmError *err) {
    if (!idm_is_closure(closure)) return idm_error_set(err, idm_span_unknown(NULL), "spawn expects a function value");
    if (!push(exec, closure, err)) return false;
    return call_value(exec, 0, false, err);
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
