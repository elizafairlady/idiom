#include "idiom/bytecode.h"

#include "idiom/core.h"
#include "idiom/regex.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stdlib.h>
#include <string.h>

static IdmIntern g_span_files;

static bool verify_operand(const IdmBytecodeModule *module, size_t *ip, IdmOpcode op, uint32_t *operand, IdmError *err);

static bool verify_arity_operands(const IdmBytecodeModule *module, size_t *ip, IdmOpcode op, IdmError *err) {
    uint32_t ignored = 0;
    for (size_t i = 0; i < 5u; i++) {
        if (!verify_operand(module, ip, op, &ignored, err)) return false;
    }
    return true;
}

static bool append_arity_operands(IdmBuffer *buf, const IdmBytecodeModule *module, size_t *ip) {
    if (*ip + 5u > module->code_count) return idm_buf_append(buf, " <missing>\n");
    for (size_t i = 0; i < 5u; i++) {
        if (!idm_buf_appendf(buf, " %u", module->code[(*ip)++])) return false;
    }
    return true;
}

static bool copy_arity_operands(const IdmBytecodeModule *src, IdmBytecodeModule *dst, size_t *ip, IdmError *err) {
    for (size_t i = 0; i < 5u; i++) {
        if (!idm_bc_emit(dst, src->code[(*ip)++], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool span_file_index(IdmBytecodeModule *module, const char *file, uint32_t *out_index) {
    const IdmSymbol *sym = idm_intern(&g_span_files, IDM_SYMBOL_WORD, file);
    if (!sym) return false;
    const char *name = idm_symbol_text(sym);
    for (size_t i = 0; i < module->span_file_count; i++) {
        if (module->span_files[i] == name) {
            *out_index = (uint32_t)i;
            return true;
        }
    }
    if (module->span_file_count == module->span_file_cap) {
        size_t cap = module->span_file_cap ? module->span_file_cap * 2u : 4u;
        const char **grown = realloc(module->span_files, cap * sizeof(*grown));
        if (!grown) return false;
        module->span_files = grown;
        module->span_file_cap = cap;
    }
    module->span_files[module->span_file_count] = name;
    *out_index = (uint32_t)module->span_file_count;
    module->span_file_count++;
    return true;
}

bool idm_bc_note_span(IdmBytecodeModule *module, IdmSpan span) {
    if (!span.file || span.line == 0) return true;
    uint32_t file_index = 0;
    if (!span_file_index(module, span.file, &file_index)) return false;
    if (module->span_count != 0) {
        size_t last = module->span_count - 1u;
        if (module->spans[last].offset == (uint32_t)module->code_count) {
            module->spans[last].file = file_index;
            module->spans[last].line = span.line;
            module->spans[last].column = span.column;
            return true;
        }
        if (module->spans[last].file == file_index && module->spans[last].line == span.line && module->spans[last].column == span.column) return true;
    }
    if (module->span_count == module->span_cap) {
        size_t cap = module->span_cap ? module->span_cap * 2u : 32u;
        void *grown = realloc(module->spans, cap * sizeof(*module->spans));
        if (!grown) return false;
        module->spans = grown;
        module->span_cap = cap;
    }
    module->spans[module->span_count].offset = (uint32_t)module->code_count;
    module->spans[module->span_count].file = file_index;
    module->spans[module->span_count].line = span.line;
    module->spans[module->span_count].column = span.column;
    module->span_count++;
    return true;
}

IdmSpan idm_bc_span_at(const IdmBytecodeModule *module, size_t ip) {
    IdmSpan none = idm_span_unknown(NULL);
    if (module->span_count == 0) return none;
    size_t lo = 0;
    size_t hi = module->span_count;
    while (lo + 1u < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if ((size_t)module->spans[mid].offset <= ip) lo = mid; else hi = mid;
    }
    if ((size_t)module->spans[lo].offset > ip) return none;
    IdmSpan out = idm_span_unknown(module->span_files[module->spans[lo].file]);
    out.line = module->spans[lo].line;
    out.column = module->spans[lo].column;
    return out;
}

bool idm_bc_note_name(IdmBytecodeModule *module, size_t offset, const char *name) {
    if (!name) return true;
    if (offset > UINT32_MAX) return false;
    uint32_t off = (uint32_t)offset;
    for (size_t i = 0; i < module->name_note_count; i++) {
        if (module->name_notes[i].offset == off) {
            char *copy = idm_strdup(name);
            if (!copy) return false;
            free(module->name_notes[i].name);
            module->name_notes[i].name = copy;
            return true;
        }
    }
    if (module->name_note_count == module->name_note_cap) {
        size_t cap = module->name_note_cap ? module->name_note_cap * 2u : 32u;
        IdmBcNameNote *grown = realloc(module->name_notes, cap * sizeof(*grown));
        if (!grown) return false;
        module->name_notes = grown;
        module->name_note_cap = cap;
    }
    char *copy = idm_strdup(name);
    if (!copy) return false;
    size_t at = module->name_note_count;
    while (at > 0 && module->name_notes[at - 1u].offset > off) {
        module->name_notes[at] = module->name_notes[at - 1u];
        at--;
    }
    module->name_notes[at].offset = off;
    module->name_notes[at].name = copy;
    module->name_note_count++;
    return true;
}

void idm_bc_init(IdmBytecodeModule *module) {
    module->code = NULL;
    module->code_count = 0;
    module->code_cap = 0;
    module->constants = NULL;
    module->const_count = 0;
    module->const_cap = 0;
    module->functions = NULL;
    module->function_count = 0;
    module->function_cap = 0;
    module->span_files = NULL;
    module->span_file_count = 0;
    module->span_file_cap = 0;
    module->spans = NULL;
    module->span_count = 0;
    module->span_cap = 0;
    module->name_notes = NULL;
    module->name_note_count = 0;
    module->name_note_cap = 0;
}

void idm_bc_destroy(IdmBytecodeModule *module) {
    if (!module) return;
    free(module->code);
    free(module->constants);
    free(module->span_files);
    free(module->spans);
    for (size_t i = 0; i < module->name_note_count; i++) free(module->name_notes[i].name);
    free(module->name_notes);
    for (size_t i = 0; i < module->function_count; i++) {
        free(module->functions[i].name);
        for (uint32_t p = 0; p < module->functions[i].pattern_count; p++) idm_pat_free(module->functions[i].param_patterns[p]);
        free(module->functions[i].param_patterns);
        for (uint32_t p = 0; p < module->functions[i].pattern_local_count; p++) free(module->functions[i].pattern_locals[p].name);
        free(module->functions[i].pattern_locals);
    }
    free(module->functions);
    idm_bc_init(module);
}

bool idm_bc_add_const(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index) {
    if (module->const_count == module->const_cap) {
        size_t cap = module->const_cap ? module->const_cap * 2u : 16u;
        IdmValue *next = realloc(module->constants, cap * sizeof(*next));
        if (!next) return false;
        module->constants = next;
        module->const_cap = cap;
    }
    if (module->const_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->const_count;
    module->constants[module->const_count++] = value;
    if (out_index) *out_index = index;
    return true;
}

static bool intern_pattern_literals(IdmRuntime *rt, IdmPattern *pat, IdmError *err) {
    if (!pat) return true;
    switch (pat->kind) {
        case IDM_PAT_LITERAL:
            pat->as.literal = idm_value_copy(rt, &rt->immortal, pat->as.literal, err);
            return !err->present;
        case IDM_PAT_PAIR:
            return intern_pattern_literals(rt, pat->as.pair.left, err)
                && intern_pattern_literals(rt, pat->as.pair.right, err);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++)
                if (!intern_pattern_literals(rt, pat->as.seq.items[i], err)) return false;
            return true;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++)
                if (!intern_pattern_literals(rt, pat->as.seq_rest.items[i], err)) return false;
            return intern_pattern_literals(rt, pat->as.seq_rest.rest, err);
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                pat->as.dict.entries[i].key = idm_value_copy(rt, &rt->immortal, pat->as.dict.entries[i].key, err);
                if (err->present) return false;
                if (!intern_pattern_literals(rt, pat->as.dict.entries[i].pattern, err)) return false;
            }
            return intern_pattern_literals(rt, pat->as.dict.rest, err);
        default:
            return true;
    }
}

bool idm_bc_intern_literals(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err) {
    if (!module) return true;
    for (size_t i = 0; i < module->const_count; i++) {
        module->constants[i] = idm_value_copy(rt, &rt->immortal, module->constants[i], err);
        if (err->present) return false;
    }
    for (size_t i = 0; i < module->function_count; i++) {
        IdmBcFunction *fn = &module->functions[i];
        for (uint32_t p = 0; p < fn->pattern_count; p++)
            if (!intern_pattern_literals(rt, fn->param_patterns[p], err)) return false;
    }
    return true;
}

bool idm_bc_add_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index) {
    if (module->function_count == module->function_cap) {
        size_t cap = module->function_cap ? module->function_cap * 2u : 8u;
        IdmBcFunction *next = realloc(module->functions, cap * sizeof(*next));
        if (!next) return false;
        module->functions = next;
        module->function_cap = cap;
    }
    if (module->function_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->function_count;
    module->functions[index].name = idm_strdup(name ? name : "<anonymous>");
    if (!module->functions[index].name) return false;
    module->functions[index].arity = arity;
    module->functions[index].local_count = local_count;
    module->functions[index].entry = entry;
    module->functions[index].has_guard = false;
    module->functions[index].guard_function = UINT32_MAX;
    module->functions[index].param_patterns = NULL;
    module->functions[index].pattern_count = 0;
    module->functions[index].pattern_locals = NULL;
    module->functions[index].pattern_local_count = 0;
    module->functions[index].trivial_match = false;
    module->function_count++;
    if (out_index) *out_index = index;
    return true;
}

bool idm_bc_set_function_entry(IdmBytecodeModule *module, uint32_t function_index, size_t entry) {
    if (function_index >= module->function_count) return false;
    module->functions[function_index].entry = entry;
    return true;
}

bool idm_bc_set_function_patterns_take(IdmBytecodeModule *module, uint32_t function_index, IdmPattern **patterns, uint32_t pattern_count) {
    if (function_index >= module->function_count) return false;
    IdmBcFunction *fn = &module->functions[function_index];
    for (uint32_t i = 0; i < fn->pattern_count; i++) idm_pat_free(fn->param_patterns[i]);
    free(fn->param_patterns);
    fn->param_patterns = patterns;
    fn->pattern_count = pattern_count;
    fn->trivial_match = true;
    for (uint32_t i = 0; i < pattern_count && fn->trivial_match; i++) {
        IdmPatternKind k = patterns[i]->kind;
        if (k != IDM_PAT_WILDCARD && k != IDM_PAT_BIND) { fn->trivial_match = false; break; }
        if (k == IDM_PAT_BIND) {
            for (uint32_t j = 0; j < i; j++) {
                if (patterns[j]->kind == IDM_PAT_BIND && strcmp(patterns[j]->as.name, patterns[i]->as.name) == 0) {
                    fn->trivial_match = false;
                    break;
                }
            }
        }
    }
    return true;
}

bool idm_bc_set_function_pattern_locals_take(IdmBytecodeModule *module, uint32_t function_index, IdmPatternLocal *locals, uint32_t local_count) {
    if (function_index >= module->function_count) return false;
    IdmBcFunction *fn = &module->functions[function_index];
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) free(fn->pattern_locals[i].name);
    free(fn->pattern_locals);
    fn->pattern_locals = locals;
    fn->pattern_local_count = local_count;
    return true;
}

bool idm_bc_set_function_guard(IdmBytecodeModule *module, uint32_t function_index, uint32_t guard_function) {
    if (function_index >= module->function_count || guard_function >= module->function_count) return false;
    module->functions[function_index].has_guard = true;
    module->functions[function_index].guard_function = guard_function;
    return true;
}

bool idm_bc_emit(IdmBytecodeModule *module, uint32_t word, size_t *out_offset) {
    if (module->code_count == module->code_cap) {
        size_t cap = module->code_cap ? module->code_cap * 2u : 64u;
        uint32_t *next = realloc(module->code, cap * sizeof(*next));
        if (!next) return false;
        module->code = next;
        module->code_cap = cap;
    }
    if (out_offset) *out_offset = module->code_count;
    module->code[module->code_count++] = word;
    return true;
}

bool idm_bc_emit_op(IdmBytecodeModule *module, IdmOpcode op, size_t *out_offset) {
    return idm_bc_emit(module, (uint32_t)op, out_offset);
}

bool idm_bc_emit_u32(IdmBytecodeModule *module, IdmOpcode op, uint32_t operand, size_t *out_offset) {
    size_t op_offset = 0;
    if (!idm_bc_emit(module, (uint32_t)op, &op_offset)) return false;
    if (!idm_bc_emit(module, operand, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

bool idm_bc_patch_u32(IdmBytecodeModule *module, size_t operand_offset, uint32_t operand) {
    if (operand_offset >= module->code_count) return false;
    module->code[operand_offset] = operand;
    return true;
}

const char *idm_opcode_name(IdmOpcode op) {
    switch (op) {
        case IDM_OP_HALT: return "HALT";
        case IDM_OP_LOAD_CONST: return "LOAD_CONST";
        case IDM_OP_LOAD_ARG: return "LOAD_ARG";
        case IDM_OP_LOAD_LOCAL: return "LOAD_LOCAL";
        case IDM_OP_STORE_LOCAL: return "STORE_LOCAL";
        case IDM_OP_LOAD_CAPTURE: return "LOAD_CAPTURE";
        case IDM_OP_MAKE_CELL: return "MAKE_CELL";
        case IDM_OP_LOAD_CELL: return "LOAD_CELL";
        case IDM_OP_STORE_CELL: return "STORE_CELL";
        case IDM_OP_MAKE_CLOSURE: return "MAKE_CLOSURE";
        case IDM_OP_MAKE_CLOSURE_CAPTURES: return "MAKE_CLOSURE_CAPTURES";
        case IDM_OP_MAKE_MULTI_CLOSURE: return "MAKE_MULTI_CLOSURE";
        case IDM_OP_CALL: return "CALL";
        case IDM_OP_TAIL_CALL: return "TAIL_CALL";
        case IDM_OP_RETURN: return "RETURN";
        case IDM_OP_POP: return "POP";
        case IDM_OP_JUMP: return "JUMP";
        case IDM_OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case IDM_OP_PRIM_CALL: return "PRIM_CALL";
        case IDM_OP_SELF: return "SELF";
        case IDM_OP_SPAWN: return "SPAWN";
        case IDM_OP_SPAWN_LINK: return "SPAWN_LINK";
        case IDM_OP_SPAWN_MONITOR: return "SPAWN_MONITOR";
        case IDM_OP_SEND: return "SEND";
        case IDM_OP_EXIT: return "EXIT";
        case IDM_OP_EXIT_SIGNAL: return "EXIT_SIGNAL";
        case IDM_OP_LINK: return "LINK";
        case IDM_OP_UNLINK: return "UNLINK";
        case IDM_OP_MONITOR: return "MONITOR";
        case IDM_OP_DEMONITOR: return "DEMONITOR";
        case IDM_OP_TRAP_EXIT: return "TRAP_EXIT";
        case IDM_OP_RECV: return "RECV";
        case IDM_OP_TAIL_RECV: return "TAIL_RECV";
        case IDM_OP_EXEC: return "EXEC";
        case IDM_OP_RESCUE_PUSH: return "RESCUE_PUSH";
        case IDM_OP_RESCUE_POP: return "RESCUE_POP";
        case IDM_OP_RAISE: return "RAISE";
        case IDM_OP_LOAD_RAISED: return "LOAD_RAISED";
        case IDM_OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case IDM_OP_STORE_GLOBAL: return "STORE_GLOBAL";
        case IDM_OP_AWAIT: return "AWAIT";
        case IDM_OP_APPLY: return "APPLY";
        case IDM_OP_ENTER_NAMESPACE: return "ENTER_NAMESPACE";
        case IDM_OP_IMPORT_GLOBAL: return "IMPORT_GLOBAL";
        case IDM_OP_LEAVE_NAMESPACE: return "LEAVE_NAMESPACE";
        case IDM_OP_DEFINE_TRAIT: return "DEFINE_TRAIT";
        case IDM_OP_IMPLEMENT_TRAIT: return "IMPLEMENT_TRAIT";
        case IDM_OP_CALL_METHOD: return "CALL_METHOD";
        case IDM_OP_TAIL_CALL_METHOD: return "TAIL_CALL_METHOD";
    }
    return "<bad-op>";
}

static bool verify_operand(const IdmBytecodeModule *module, size_t *ip, IdmOpcode op, uint32_t *operand, IdmError *err) {
    if (*ip >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s missing operand", idm_opcode_name(op));
    *operand = module->code[(*ip)++];
    return true;
}

static bool verify_scan(const IdmBytecodeModule *module, unsigned char *is_start, size_t *targets, size_t *target_count, IdmError *err) {
    size_t ip = 0;
    while (ip < module->code_count) {
        is_start[ip] = 1u;
        IdmOpcode op = (IdmOpcode)module->code[ip++];
        uint32_t operand = 0;
        switch (op) {
            case IDM_OP_HALT:
            case IDM_OP_RETURN:
            case IDM_OP_POP:
            case IDM_OP_MAKE_CELL:
            case IDM_OP_LOAD_CELL:
            case IDM_OP_STORE_CELL:
            case IDM_OP_SELF:
            case IDM_OP_SPAWN:
            case IDM_OP_SPAWN_LINK:
            case IDM_OP_SPAWN_MONITOR:
            case IDM_OP_SEND:
            case IDM_OP_EXIT:
            case IDM_OP_EXIT_SIGNAL:
            case IDM_OP_LINK:
            case IDM_OP_UNLINK:
            case IDM_OP_MONITOR:
            case IDM_OP_DEMONITOR:
            case IDM_OP_TRAP_EXIT:
            case IDM_OP_EXEC:
            case IDM_OP_AWAIT:
            case IDM_OP_APPLY:
            case IDM_OP_LEAVE_NAMESPACE:
            case IDM_OP_RESCUE_POP:
            case IDM_OP_RAISE:
            case IDM_OP_LOAD_RAISED:
                break;
            case IDM_OP_DEFINE_TRAIT: {
                uint32_t trait_const = 0;
                uint32_t requirement_count = 0;
                uint32_t method_count = 0;
                if (!verify_operand(module, &ip, op, &trait_const, err)) return false;
                if (trait_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "DEFINE_TRAIT trait constant %u out of bounds", trait_const);
                if (!verify_operand(module, &ip, op, &requirement_count, err)) return false;
                if (!verify_operand(module, &ip, op, &method_count, err)) return false;
                for (uint32_t i = 0; i < requirement_count; i++) {
                    uint32_t requirement_const = 0;
                    if (!verify_operand(module, &ip, op, &requirement_const, err)) return false;
                    if (requirement_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "DEFINE_TRAIT requirement constant %u out of bounds", requirement_const);
                }
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = 0;
                    uint32_t ignored = 0;
                    if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                    if (method_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "DEFINE_TRAIT method constant %u out of bounds", method_const);
                    if (!verify_arity_operands(module, &ip, op, err)) return false;
                    if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                    if (ignored > 1u) return idm_error_set(err, idm_span_unknown(NULL), "DEFINE_TRAIT default flag must be 0 or 1");
                }
                break;
            }
            case IDM_OP_IMPLEMENT_TRAIT: {
                uint32_t trait_const = 0;
                uint32_t type_const = 0;
                uint32_t provider_const = 0;
                uint32_t provider_key_const = 0;
                uint32_t method_count = 0;
                if (!verify_operand(module, &ip, op, &trait_const, err)) return false;
                if (trait_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT trait constant %u out of bounds", trait_const);
                if (!verify_operand(module, &ip, op, &type_const, err)) return false;
                if (type_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT type constant %u out of bounds", type_const);
                if (!verify_operand(module, &ip, op, &provider_const, err)) return false;
                if (provider_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT provider constant %u out of bounds", provider_const);
                if (!verify_operand(module, &ip, op, &provider_key_const, err)) return false;
                if (provider_key_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT provider key constant %u out of bounds", provider_key_const);
                if (!verify_operand(module, &ip, op, &method_count, err)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = 0;
                    if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                    if (method_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "IMPLEMENT_TRAIT method constant %u out of bounds", method_const);
                    if (!verify_arity_operands(module, &ip, op, err)) return false;
                }
                break;
            }
            case IDM_OP_CALL_METHOD:
            case IDM_OP_TAIL_CALL_METHOD: {
                uint32_t trait_const = 0;
                uint32_t method_const = 0;
                uint32_t ignored = 0;
                if (!verify_operand(module, &ip, op, &trait_const, err)) return false;
                if (trait_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD trait constant %u out of bounds", trait_const);
                if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                if (method_const >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "CALL_METHOD method constant %u out of bounds", method_const);
                if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                break;
            }
            case IDM_OP_PRIM_CALL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= idm_primitive_count()) return idm_error_set(err, idm_span_unknown(NULL), "PRIM_CALL primitive %u out of bounds", operand);
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case IDM_OP_LOAD_CONST:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "LOAD_CONST index %u out of bounds", operand);
                break;
            case IDM_OP_ENTER_NAMESPACE:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->const_count) return idm_error_set(err, idm_span_unknown(NULL), "ENTER_NAMESPACE index %u out of bounds", operand);
                break;
            case IDM_OP_MAKE_CLOSURE:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE function %u out of bounds", operand);
                break;
            case IDM_OP_LOAD_ARG:
            case IDM_OP_LOAD_LOCAL:
            case IDM_OP_STORE_LOCAL:
            case IDM_OP_LOAD_CAPTURE:
            case IDM_OP_LOAD_GLOBAL:
            case IDM_OP_STORE_GLOBAL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case IDM_OP_CALL:
            case IDM_OP_TAIL_CALL: {
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if ((operand & IDM_CALL_DIRECT_FLAG) == 0) break;
                uint32_t entry_count = 0;
                uint32_t capture_count = 0;
                if (!verify_operand(module, &ip, op, &entry_count, err)) return false;
                if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "%s direct call requires at least one entry", idm_opcode_name(op));
                if (!verify_operand(module, &ip, op, &capture_count, err)) return false;
                (void)capture_count;
                for (uint32_t i = 0; i < entry_count; i++) {
                    uint32_t entry = 0;
                    if (!verify_operand(module, &ip, op, &entry, err)) return false;
                    if (entry >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "%s direct function %u out of bounds", idm_opcode_name(op), entry);
                }
                break;
            }
            case IDM_OP_IMPORT_GLOBAL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case IDM_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = 0;
                uint32_t capture_count = 0;
                if (!verify_operand(module, &ip, op, &entry_count, err)) return false;
                if (entry_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE requires at least one entry");
                if (!verify_operand(module, &ip, op, &capture_count, err)) return false;
                (void)capture_count;
                for (uint32_t i = 0; i < entry_count; i++) {
                    uint32_t entry = 0;
                    if (!verify_operand(module, &ip, op, &entry, err)) return false;
                    if (entry >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_MULTI_CLOSURE function %u out of bounds", entry);
                }
                break;
            }
            case IDM_OP_MAKE_CLOSURE_CAPTURES:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES function %u out of bounds", operand);
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case IDM_OP_JUMP:
            case IDM_OP_JUMP_IF_FALSE:
            case IDM_OP_RECV:
            case IDM_OP_TAIL_RECV:
            case IDM_OP_RESCUE_PUSH:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s target %u out of bounds", idm_opcode_name(op), operand);
                targets[(*target_count)++] = operand;
                break;
            default:
                return idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u at offset %zu", (unsigned)op, ip - 1u);
        }
    }
    return true;
}

bool idm_bc_verify(const IdmBytecodeModule *module, IdmError *err) {
    if (module->function_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "bytecode module has no functions");
    for (size_t i = 0; i < module->function_count; i++) {
        if (module->functions[i].entry >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "function %zu entry is out of bounds", i);
        if (module->functions[i].has_guard && module->functions[i].guard_function >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function %zu guard function is out of bounds", i);
        if (module->functions[i].has_guard && module->functions[module->functions[i].guard_function].arity != module->functions[i].arity) return idm_error_set(err, idm_span_unknown(NULL), "function %zu guard arity mismatch", i);
    }
    size_t slots = module->code_count ? module->code_count : 1u;
    unsigned char *is_start = calloc(slots, 1u);
    size_t *targets = malloc(slots * sizeof(*targets));
    if (!is_start || !targets) { free(is_start); free(targets); return idm_error_oom(err, idm_span_unknown(NULL)); }
    size_t target_count = 0;
    bool ok = verify_scan(module, is_start, targets, &target_count, err);
    for (size_t i = 0; ok && i < module->function_count; i++) {
        if (!is_start[module->functions[i].entry]) ok = idm_error_set(err, idm_span_unknown(NULL), "function %zu entry %zu is not an instruction boundary", i, module->functions[i].entry);
    }
    for (size_t i = 0; ok && i < target_count; i++) {
        if (!is_start[targets[i]]) ok = idm_error_set(err, idm_span_unknown(NULL), "branch target %zu is not an instruction boundary", targets[i]);
    }
    free(is_start);
    free(targets);
    return ok;
}

static const char *name_note_at(const IdmBytecodeModule *module, size_t offset) {
    uint32_t off = (uint32_t)offset;
    size_t lo = 0;
    size_t hi = module->name_note_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (module->name_notes[mid].offset < off) lo = mid + 1u;
        else hi = mid;
    }
    if (lo < module->name_note_count && module->name_notes[lo].offset == off) return module->name_notes[lo].name;
    return NULL;
}

static bool append_function_ref(IdmBuffer *buf, const IdmBytecodeModule *module, uint32_t function_index) {
    if (function_index < module->function_count) {
        return idm_buf_appendf(buf, " %u#%s", function_index, module->functions[function_index].name);
    }
    return idm_buf_appendf(buf, " %u", function_index);
}

static bool append_name_note(IdmBuffer *buf, const IdmBytecodeModule *module, size_t offset) {
    const char *name = name_note_at(module, offset);
    return name ? idm_buf_appendf(buf, " #+name: %s", name) : true;
}

bool idm_bc_disassemble(IdmBuffer *buf, const IdmBytecodeModule *module) {
    for (size_t i = 0; i < module->function_count; i++) {
        if (!idm_buf_appendf(buf, "fn %zu %s/%u entry=%zu locals=%u", i, module->functions[i].name, module->functions[i].arity, module->functions[i].entry, module->functions[i].local_count)) return false;
        if (module->functions[i].has_guard) {
            uint32_t guard = module->functions[i].guard_function;
            if (guard < module->function_count) {
                if (!idm_buf_appendf(buf, " guard=%u#%s", guard, module->functions[guard].name)) return false;
            } else if (!idm_buf_appendf(buf, " guard=%u", guard)) return false;
        }
        if (!idm_buf_append_char(buf, '\n')) return false;
    }
    size_t ip = 0;
    while (ip < module->code_count) {
        size_t offset = ip;
        IdmOpcode op = (IdmOpcode)module->code[ip++];
        if (!idm_buf_appendf(buf, "%04zu %-16s", offset, idm_opcode_name(op))) return false;
        switch (op) {
            case IDM_OP_LOAD_CONST:
            case IDM_OP_LOAD_ARG:
            case IDM_OP_LOAD_LOCAL:
            case IDM_OP_STORE_LOCAL:
            case IDM_OP_LOAD_CAPTURE:
            case IDM_OP_MAKE_CLOSURE:
            case IDM_OP_MAKE_CLOSURE_CAPTURES:
            case IDM_OP_JUMP:
            case IDM_OP_JUMP_IF_FALSE:
            case IDM_OP_PRIM_CALL:
            case IDM_OP_RECV:
            case IDM_OP_TAIL_RECV:
            case IDM_OP_RESCUE_PUSH:
            case IDM_OP_LOAD_GLOBAL:
            case IDM_OP_STORE_GLOBAL:
            case IDM_OP_ENTER_NAMESPACE:
            case IDM_OP_IMPORT_GLOBAL:
                if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t operand = module->code[ip++];
                if (op == IDM_OP_MAKE_CLOSURE || op == IDM_OP_MAKE_CLOSURE_CAPTURES) {
                    if (!append_function_ref(buf, module, operand)) return false;
                } else {
                    if (!idm_buf_appendf(buf, " %u", operand)) return false;
                }
                if (op == IDM_OP_IMPORT_GLOBAL) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                } else if (op == IDM_OP_MAKE_CLOSURE_CAPTURES) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                } else if (op == IDM_OP_PRIM_CALL) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                }
                break;
            case IDM_OP_CALL:
            case IDM_OP_TAIL_CALL: {
                if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t operand = module->code[ip++];
                if ((operand & IDM_CALL_DIRECT_FLAG) == 0) {
                    if (!idm_buf_appendf(buf, " %u", operand)) return false;
                    break;
                }
                if (ip + 2u > module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t entry_count = module->code[ip++];
                uint32_t capture_count = module->code[ip++];
                if (!idm_buf_appendf(buf, " direct argc=%u captures=%u entries=%u", operand & IDM_CALL_ARGC_MASK, capture_count, entry_count)) return false;
                for (uint32_t i = 0; i < entry_count; i++) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!append_function_ref(buf, module, module->code[ip++])) return false;
                }
                break;
            }
            case IDM_OP_MAKE_MULTI_CLOSURE: {
                if (ip + 2u > module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t entry_count = module->code[ip++];
                uint32_t capture_count = module->code[ip++];
                if (!idm_buf_appendf(buf, " %u %u", entry_count, capture_count)) return false;
                for (uint32_t i = 0; i < entry_count; i++) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!append_function_ref(buf, module, module->code[ip++])) return false;
                }
                break;
            }
            case IDM_OP_DEFINE_TRAIT: {
                if (ip + 3u > module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t trait_const = module->code[ip++];
                uint32_t requirement_count = module->code[ip++];
                uint32_t method_count = module->code[ip++];
                if (!idm_buf_appendf(buf, " %u %u %u", trait_const, requirement_count, method_count)) return false;
                for (uint32_t i = 0; i < requirement_count; i++) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                }
                for (uint32_t i = 0; i < method_count; i++) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    if (!append_arity_operands(buf, module, &ip)) return false;
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                }
                break;
            }
            case IDM_OP_IMPLEMENT_TRAIT: {
                if (ip + 5u > module->code_count) return idm_buf_append(buf, " <missing>\n");
                uint32_t trait_const = module->code[ip++];
                uint32_t type_const = module->code[ip++];
                uint32_t provider_const = module->code[ip++];
                uint32_t provider_key_const = module->code[ip++];
                uint32_t method_count = module->code[ip++];
                if (!idm_buf_appendf(buf, " %u %u %u %u %u", trait_const, type_const, provider_const, provider_key_const, method_count)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    if (ip >= module->code_count) return idm_buf_append(buf, " <missing>\n");
                    if (!idm_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    if (!append_arity_operands(buf, module, &ip)) return false;
                }
                break;
            }
            case IDM_OP_CALL_METHOD:
            case IDM_OP_TAIL_CALL_METHOD:
                if (ip + 3u > module->code_count) return idm_buf_append(buf, " <missing>\n");
                if (!idm_buf_appendf(buf, " %u %u %u", module->code[ip], module->code[ip + 1u], module->code[ip + 2u])) return false;
                ip += 3u;
                break;
            default:
                break;
        }
        if (!append_name_note(buf, module, offset)) return false;
        if (!idm_buf_append_char(buf, '\n')) return false;
    }
    return true;
}

static bool serialize_value(IdmBuffer *out, IdmValue v, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "value nested too deeply for .ic");
    switch (v.tag) {
        case IDM_VAL_NIL: return idm_buf_put_u8(out, 0u);
        case IDM_VAL_EMPTY_LIST: return idm_buf_put_u8(out, 13u);
        case IDM_VAL_INT: return idm_buf_put_u8(out, 1u) && idm_buf_put_u64(out, (uint64_t)v.as.i);
        case IDM_VAL_FLOAT: { uint64_t bits; double d = v.as.f; memcpy(&bits, &d, 8u); return idm_buf_put_u8(out, 2u) && idm_buf_put_u64(out, bits); }
        case IDM_VAL_STRING: return idm_buf_put_u8(out, 3u) && idm_buf_put_str(out, idm_string_bytes(v), idm_string_length(v));
        case IDM_VAL_ATOM: { const char *s = idm_symbol_text(v.as.symbol); return idm_buf_put_u8(out, 4u) && idm_buf_put_str(out, s, strlen(s)); }
        case IDM_VAL_WORD: { const char *s = idm_symbol_text(v.as.symbol); return idm_buf_put_u8(out, 5u) && idm_buf_put_str(out, s, strlen(s)); }
        case IDM_VAL_PAIR: {
            IdmError ignore; idm_error_init(&ignore);
            IdmValue car = idm_car(v, &ignore), cdr = idm_cdr(v, &ignore);
            idm_error_clear(&ignore);
            return idm_buf_put_u8(out, 6u) && serialize_value(out, car, depth + 1u, err) && serialize_value(out, cdr, depth + 1u, err);
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            size_t n = idm_sequence_count(v);
            if (!idm_buf_put_u8(out, v.tag == IDM_VAL_TUPLE ? 7u : 8u) || !idm_buf_put_u32(out, (uint32_t)n)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < n; i++) {
                IdmError ignore; idm_error_init(&ignore);
                IdmValue item = idm_sequence_item(v, i, &ignore);
                idm_error_clear(&ignore);
                if (!serialize_value(out, item, depth + 1u, err)) return false;
            }
            return true;
        }
        case IDM_VAL_DICT: {
            size_t n = idm_dict_count(v);
            if (!idm_buf_put_u8(out, 9u) || !idm_buf_put_u32(out, (uint32_t)n)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < n; i++) {
                IdmValue key, val;
                if (!idm_dict_entry(v, i, &key, &val)) return idm_error_set(err, idm_span_unknown(NULL), "dict entry serialization failed");
                if (!serialize_value(out, key, depth + 1u, err) || !serialize_value(out, val, depth + 1u, err)) return false;
            }
            return true;
        }
        case IDM_VAL_PRIMITIVE:
            return idm_buf_put_u8(out, 10u) && idm_buf_put_u32(out, (uint32_t)v.as.id);
        case IDM_VAL_RECORD: {
            IdmError ignore;
            idm_error_init(&ignore);
            const char *type = idm_record_type(v, &ignore);
            IdmValue fields = idm_record_fields(v, &ignore);
            idm_error_clear(&ignore);
            return idm_buf_put_u8(out, 11u) && idm_buf_put_str(out, type ? type : "", type ? strlen(type) : 0u) && serialize_value(out, fields, depth + 1u, err);
        }
        case IDM_VAL_SYNTAX: {
            const IdmSyntax *syn = idm_syntax_get(v, err);
            if (!syn) return false;
            if (!idm_buf_put_u8(out, 12u)) return idm_error_oom(err, idm_span_unknown(NULL));
            return idm_syn_serialize(out, syn, err);
        }
        case IDM_VAL_REGEX: {
            IdmRegex *rx = idm_regex_value_get(v, err);
            if (!rx) return false;
            size_t len = 0;
            const char *source = idm_regex_source(rx, &len);
            return idm_buf_put_u8(out, 14u) &&
                   idm_buf_put_str(out, source, len) &&
                   idm_buf_put_u32(out, idm_regex_flags(rx));
        }
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "value kind cannot be serialized to .ic");
    }
}

static bool serialize_pattern(IdmBuffer *out, const IdmPattern *pat, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "pattern nested too deeply for .ic");
    if (!idm_buf_put_u8(out, (uint8_t)pat->kind)) return idm_error_oom(err, idm_span_unknown(NULL));
    switch (pat->kind) {
        case IDM_PAT_WILDCARD: return true;
        case IDM_PAT_BIND:
        case IDM_PAT_PIN: return idm_buf_put_str(out, pat->as.name, strlen(pat->as.name)) ? true : idm_error_oom(err, idm_span_unknown(NULL));
        case IDM_PAT_LITERAL: return serialize_value(out, pat->as.literal, depth + 1u, err);
        case IDM_PAT_PAIR: return serialize_pattern(out, pat->as.pair.left, depth + 1u, err) && serialize_pattern(out, pat->as.pair.right, depth + 1u, err);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            if (!idm_buf_put_u32(out, (uint32_t)pat->as.seq.count)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.seq.count; i++) if (!serialize_pattern(out, pat->as.seq.items[i], depth + 1u, err)) return false;
            return true;
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            if (!idm_buf_put_u32(out, (uint32_t)pat->as.seq_rest.count)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) if (!serialize_pattern(out, pat->as.seq_rest.items[i], depth + 1u, err)) return false;
            return serialize_pattern(out, pat->as.seq_rest.rest, depth + 1u, err);
        }
        case IDM_PAT_DICT: {
            if (!idm_buf_put_u32(out, (uint32_t)pat->as.dict.count)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (!serialize_value(out, pat->as.dict.entries[i].key, depth + 1u, err)) return false;
                if (!serialize_pattern(out, pat->as.dict.entries[i].pattern, depth + 1u, err)) return false;
            }
            if (!idm_buf_put_u8(out, pat->as.dict.rest ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
            if (pat->as.dict.rest && !serialize_pattern(out, pat->as.dict.rest, depth + 1u, err)) return false;
            return true;
        }
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "pattern kind cannot be serialized to .ic");
    }
}

bool idm_ic_serialize(const IdmBytecodeModule *module, IdmBuffer *out, IdmError *err) {
    if (!idm_buf_append_n(out, "IDMC", 4u) || !idm_buf_put_u32(out, 12u)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, (uint32_t)module->const_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->const_count; i++) if (!serialize_value(out, module->constants[i], 0u, err)) return false;
    if (!idm_buf_put_u32(out, (uint32_t)module->function_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->function_count; i++) {
        const IdmBcFunction *f = &module->functions[i];
        if (!idm_buf_put_str(out, f->name ? f->name : "", f->name ? strlen(f->name) : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u32(out, f->arity) || !idm_buf_put_u32(out, f->local_count) || !idm_buf_put_u64(out, (uint64_t)f->entry)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u8(out, f->has_guard ? 1u : 0u) || !idm_buf_put_u32(out, f->guard_function)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u32(out, f->pattern_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_count; p++) if (!serialize_pattern(out, f->param_patterns[p], 0u, err)) return false;
        if (!idm_buf_put_u32(out, f->pattern_local_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_local_count; p++) {
            if (!idm_buf_put_str(out, f->pattern_locals[p].name, strlen(f->pattern_locals[p].name)) || !idm_buf_put_u32(out, f->pattern_locals[p].slot)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    if (!idm_buf_put_u64(out, (uint64_t)module->code_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->code_count; i++) if (!idm_buf_put_u32(out, module->code[i])) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, (uint32_t)module->span_file_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->span_file_count; i++) {
        if (!idm_buf_put_str(out, module->span_files[i], strlen(module->span_files[i]))) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!idm_buf_put_u32(out, (uint32_t)module->span_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->span_count; i++) {
        if (!idm_buf_put_u32(out, module->spans[i].offset) || !idm_buf_put_u32(out, module->spans[i].file) ||
            !idm_buf_put_u32(out, module->spans[i].line) || !idm_buf_put_u32(out, module->spans[i].column)) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    if (!idm_buf_put_u32(out, (uint32_t)module->name_note_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->name_note_count; i++) {
        if (!idm_buf_put_u32(out, module->name_notes[i].offset) ||
            !idm_buf_put_str(out, module->name_notes[i].name, strlen(module->name_notes[i].name))) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    return true;
}

static bool deserialize_value(IdmRuntime *rt, IdmByteReader *r, IdmValue *out, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), ".ic value nested too deeply");
    uint8_t tag = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic value");
    switch (tag) {
        case 0u: *out = idm_nil(); return true;
        case 13u: *out = idm_empty_list(); return true;
        case 1u: { uint64_t bits = idm_rd_u64(r); if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated int"); *out = idm_int((int64_t)bits); return true; }
        case 2u: { uint64_t bits = idm_rd_u64(r); if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated float"); double d; memcpy(&d, &bits, 8u); *out = idm_float(d); return true; }
        case 3u: { size_t n = 0; char *s = idm_rd_string(r, &n); if (!s) return idm_error_set(err, idm_span_unknown(NULL), "truncated string"); *out = idm_string_n(rt, s, n, err); free(s); return !(err && err->present); }
        case 4u: { char *s = idm_rd_string(r, NULL); if (!s) return idm_error_set(err, idm_span_unknown(NULL), "truncated atom"); *out = idm_atom(rt, s); free(s); return true; }
        case 5u: { char *s = idm_rd_string(r, NULL); if (!s) return idm_error_set(err, idm_span_unknown(NULL), "truncated word"); *out = idm_word(rt, s); free(s); return true; }
        case 6u: {
            IdmValue car, cdr;
            if (!deserialize_value(rt, r, &car, depth + 1u, err) || !deserialize_value(rt, r, &cdr, depth + 1u, err)) return false;
            *out = idm_cons(rt, car, cdr, err); return !(err && err->present);
        }
        case 7u:
        case 8u: {
            uint32_t n = idm_rd_u32(r);
            if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated sequence");
            IdmValue *items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
            for (uint32_t i = 0; i < n; i++) if (!deserialize_value(rt, r, &items[i], depth + 1u, err)) { free(items); return false; }
            *out = tag == 7u ? idm_tuple(rt, items, n, err) : idm_vector(rt, items, n, err);
            free(items);
            return !(err && err->present);
        }
        case 9u: {
            uint32_t n = idm_rd_u32(r);
            if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated dict");
            IdmDictEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
            for (uint32_t i = 0; i < n; i++) if (!deserialize_value(rt, r, &entries[i].key, depth + 1u, err) || !deserialize_value(rt, r, &entries[i].value, depth + 1u, err)) { free(entries); return false; }
            *out = idm_dict(rt, entries, n, err);
            free(entries);
            return !(err && err->present);
        }
        case 10u: { uint32_t p = idm_rd_u32(r); if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated primitive"); *out = idm_primitive_value(p); return true; }
        case 11u: {
            char *type = idm_rd_string(r, NULL);
            if (!type) return idm_error_set(err, idm_span_unknown(NULL), "truncated record type");
            IdmValue fields = idm_nil();
            bool ok = deserialize_value(rt, r, &fields, depth + 1u, err);
            if (ok) {
                *out = idm_record(rt, type, fields, err);
                ok = !(err && err->present);
            }
            free(type);
            return ok;
        }
        case 12u: {
            IdmSyntax *syn = idm_syn_deserialize(rt, r, err);
            if (!syn) return false;
            *out = idm_syntax_value(rt, syn, err);
            idm_syn_free(syn);
            return !(err && err->present);
        }
        case 14u: {
            size_t len = 0;
            char *source = idm_rd_string(r, &len);
            uint32_t flags = source ? idm_rd_u32(r) : 0u;
            if (!source || !r->ok) {
                free(source);
                return idm_error_set(err, idm_span_unknown(NULL), "truncated regex");
            }
            IdmRegex *rx = NULL;
            bool ok = idm_regex_compile(source, len, flags, &rx, err);
            free(source);
            if (!ok) return false;
            *out = idm_regex_value(rt, rx, err);
            return !(err && err->present);
        }
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "unknown .ic value tag %u", tag);
    }
}

static IdmPattern *deserialize_pattern(IdmRuntime *rt, IdmByteReader *r, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) { idm_error_set(err, idm_span_unknown(NULL), ".ic pattern nested too deeply"); return NULL; }
    uint8_t kind = idm_rd_u8(r);
    if (!r->ok) { idm_error_set(err, idm_span_unknown(NULL), "truncated .ic pattern"); return NULL; }
    IdmSpan span = idm_span_unknown(NULL);
    switch (kind) {
        case IDM_PAT_WILDCARD: return idm_pat_wildcard(span);
        case IDM_PAT_BIND: { char *s = idm_rd_string(r, NULL); if (!s) { idm_error_set(err, span, "truncated pattern name"); return NULL; } IdmPattern *p = idm_pat_bind(s, span); free(s); return p; }
        case IDM_PAT_PIN: { char *s = idm_rd_string(r, NULL); if (!s) { idm_error_set(err, span, "truncated pattern name"); return NULL; } IdmPattern *p = idm_pat_pin(s, span); free(s); return p; }
        case IDM_PAT_LITERAL: { IdmValue v; if (!deserialize_value(rt, r, &v, depth + 1u, err)) return NULL; return idm_pat_literal(v, span); }
        case IDM_PAT_PAIR: {
            IdmPattern *left = deserialize_pattern(rt, r, depth + 1u, err); if (!left) return NULL;
            IdmPattern *right = deserialize_pattern(rt, r, depth + 1u, err); if (!right) { idm_pat_free(left); return NULL; }
            IdmPattern *p = idm_pat_pair(left, right, span);
            if (!p) { idm_pat_free(left); idm_pat_free(right); idm_error_oom(err, span); }
            return p;
        }
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            uint32_t n = idm_rd_u32(r); if (!r->ok) { idm_error_set(err, span, "truncated pattern sequence"); return NULL; }
            IdmPattern **items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) { idm_error_oom(err, span); return NULL; }
            for (uint32_t i = 0; i < n; i++) { items[i] = deserialize_pattern(rt, r, depth + 1u, err); if (!items[i]) { for (uint32_t j = 0; j < i; j++) idm_pat_free(items[j]); free(items); return NULL; } }
            IdmPattern *p = idm_pat_sequence((IdmPatternKind)kind, items, n, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) idm_pat_free(items[i]); free(items); idm_error_oom(err, span); }
            return p;
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            uint32_t n = idm_rd_u32(r); if (!r->ok) { idm_error_set(err, span, "truncated pattern rest"); return NULL; }
            IdmPattern **items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) { idm_error_oom(err, span); return NULL; }
            for (uint32_t i = 0; i < n; i++) { items[i] = deserialize_pattern(rt, r, depth + 1u, err); if (!items[i]) { for (uint32_t j = 0; j < i; j++) idm_pat_free(items[j]); free(items); return NULL; } }
            IdmPattern *rest = deserialize_pattern(rt, r, depth + 1u, err);
            if (!rest) { for (uint32_t i = 0; i < n; i++) idm_pat_free(items[i]); free(items); return NULL; }
            IdmPattern *p = idm_pat_sequence_rest((IdmPatternKind)kind, items, n, rest, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) idm_pat_free(items[i]); idm_pat_free(rest); free(items); idm_error_oom(err, span); }
            return p;
        }
        case IDM_PAT_DICT: {
            uint32_t n = idm_rd_u32(r); if (!r->ok) { idm_error_set(err, span, "truncated pattern dict"); return NULL; }
            IdmDictPatternEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) { idm_error_oom(err, span); return NULL; }
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; i++) {
                if (!deserialize_value(rt, r, &entries[i].key, depth + 1u, err)) ok = false;
                else { entries[i].pattern = deserialize_pattern(rt, r, depth + 1u, err); if (!entries[i].pattern) ok = false; }
            }
            uint8_t has_rest = ok ? idm_rd_u8(r) : 0u;
            if (ok && !r->ok) { idm_error_set(err, span, "truncated pattern dict rest"); ok = false; }
            IdmPattern *rest = NULL;
            if (ok && has_rest > 1u) { idm_error_set(err, span, "invalid pattern dict rest flag"); ok = false; }
            if (ok && has_rest) { rest = deserialize_pattern(rt, r, depth + 1u, err); if (!rest) ok = false; }
            if (!ok) { for (uint32_t i = 0; i < n; i++) if (entries[i].pattern) idm_pat_free(entries[i].pattern); free(entries); idm_pat_free(rest); return NULL; }
            IdmPattern *p = idm_pat_dict(entries, n, rest, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) idm_pat_free(entries[i].pattern); free(entries); idm_pat_free(rest); idm_error_oom(err, span); }
            return p;
        }
        default:
            idm_error_set(err, span, "unknown .ic pattern kind %u", kind);
            return NULL;
    }
}

bool idm_ic_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmBytecodeModule *module, IdmError *err) {
    idm_bc_init(module);
    IdmByteReader r = { data, len, 0u, true };
    if (len < 8u || memcmp(data, "IDMC", 4u) != 0) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "not an .ic module"); }
    r.pos = 4u;
    uint32_t version = idm_rd_u32(&r);
    if (version != 12u) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), ".ic version %u unsupported", version); }
    uint32_t const_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < const_count && r.ok; i++) {
        IdmValue v;
        if (!deserialize_value(rt, &r, &v, 0u, err)) { idm_bc_destroy(module); return false; }
        if (!idm_bc_add_const(module, v, NULL)) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    uint32_t function_count = idm_rd_u32(&r);
    uint32_t *deferred_guards = NULL;
    if (r.ok && function_count != 0) {
        deferred_guards = malloc((size_t)function_count * sizeof(*deferred_guards));
        if (!deferred_guards) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
        for (uint32_t i = 0; i < function_count; i++) deferred_guards[i] = UINT32_MAX;
    }
    for (uint32_t i = 0; i < function_count && r.ok; i++) {
        char *name = idm_rd_string(&r, NULL);
        if (!name) { free(deferred_guards); idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "truncated function name"); }
        uint32_t arity = idm_rd_u32(&r);
        uint32_t local_count = idm_rd_u32(&r);
        uint64_t entry = idm_rd_u64(&r);
        uint8_t has_guard = idm_rd_u8(&r);
        uint32_t guard_function = idm_rd_u32(&r);
        uint32_t idx = 0;
        if (!r.ok || !idm_bc_add_function(module, name, arity, local_count, (size_t)entry, &idx)) { free(name); free(deferred_guards); idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "corrupt .ic function"); }
        free(name);
        if (has_guard) deferred_guards[idx] = guard_function;
        uint32_t pattern_count = idm_rd_u32(&r);
        if (pattern_count != 0) {
            IdmPattern **patterns = calloc(pattern_count, sizeof(*patterns));
            if (!patterns) { free(deferred_guards); idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
            bool ok = true;
            for (uint32_t p = 0; p < pattern_count && ok; p++) { patterns[p] = deserialize_pattern(rt, &r, 0u, err); if (!patterns[p]) ok = false; }
            if (!ok || !idm_bc_set_function_patterns_take(module, idx, patterns, pattern_count)) {
                for (uint32_t p = 0; p < pattern_count; p++) idm_pat_free(patterns[p]);
                free(patterns);
                free(deferred_guards);
                idm_bc_destroy(module);
                return false;
            }
        }
        uint32_t pattern_local_count = idm_rd_u32(&r);
        if (pattern_local_count != 0) {
            IdmPatternLocal *locals = calloc(pattern_local_count, sizeof(*locals));
            if (!locals) { free(deferred_guards); idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
            bool ok = true;
            for (uint32_t p = 0; p < pattern_local_count && ok; p++) {
                char *lname = idm_rd_string(&r, NULL);
                uint32_t slot = idm_rd_u32(&r);
                if (!lname || !r.ok) { free(lname); ok = false; break; }
                locals[p].name = lname;
                locals[p].slot = slot;
            }
            if (!ok || !idm_bc_set_function_pattern_locals_take(module, idx, locals, pattern_local_count)) {
                for (uint32_t p = 0; p < pattern_local_count; p++) free(locals[p].name);
                free(locals);
                free(deferred_guards);
                idm_bc_destroy(module);
                return r.ok ? idm_error_oom(err, idm_span_unknown(NULL)) : idm_error_set(err, idm_span_unknown(NULL), "truncated pattern local");
            }
        }
    }
    for (uint32_t i = 0; i < function_count && r.ok; i++) {
        if (deferred_guards[i] != UINT32_MAX && !idm_bc_set_function_guard(module, i, deferred_guards[i])) {
            free(deferred_guards);
            idm_bc_destroy(module);
            return idm_error_set(err, idm_span_unknown(NULL), ".ic guard function index is out of bounds");
        }
    }
    free(deferred_guards);
    uint64_t code_count = idm_rd_u64(&r);
    for (uint64_t i = 0; i < code_count && r.ok; i++) {
        uint32_t word = idm_rd_u32(&r);
        if (!r.ok || !idm_bc_emit(module, word, NULL)) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "corrupt .ic code"); }
    }
    uint32_t span_file_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < span_file_count && r.ok; i++) {
        char *name = idm_rd_string(&r, NULL);
        if (!name) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "truncated span file"); }
        uint32_t idx = 0;
        bool ok = span_file_index(module, name, &idx);
        free(name);
        if (!ok) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    uint32_t span_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < span_count && r.ok; i++) {
        uint32_t offset = idm_rd_u32(&r);
        uint32_t file = idm_rd_u32(&r);
        uint32_t line = idm_rd_u32(&r);
        uint32_t column = idm_rd_u32(&r);
        if (!r.ok || file >= module->span_file_count) break;
        if (module->span_count == module->span_cap) {
            size_t cap = module->span_cap ? module->span_cap * 2u : 32u;
            void *grown = realloc(module->spans, cap * sizeof(*module->spans));
            if (!grown) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
            module->spans = grown;
            module->span_cap = cap;
        }
        module->spans[module->span_count].offset = offset;
        module->spans[module->span_count].file = file;
        module->spans[module->span_count].line = line;
        module->spans[module->span_count].column = column;
        module->span_count++;
    }
    uint32_t name_note_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < name_note_count && r.ok; i++) {
        uint32_t offset = idm_rd_u32(&r);
        char *name = idm_rd_string(&r, NULL);
        if (!name || !r.ok || offset >= module->code_count) {
            free(name);
            idm_bc_destroy(module);
            return idm_error_set(err, idm_span_unknown(NULL), "corrupt bytecode name note");
        }
        bool ok = idm_bc_note_name(module, offset, name);
        free(name);
        if (!ok) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (!r.ok) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic module"); }
    if (!idm_bc_verify(module, err)) { idm_bc_destroy(module); return false; }
    if (!idm_bc_intern_literals(rt, module, err)) { idm_bc_destroy(module); return false; }
    return true;
}

static bool reloc_emit(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t const_off, uint32_t fn_off, uint32_t code_off, IdmError *err) {
    size_t ip = 0;
    while (ip < src->code_count) {
        IdmOpcode op = (IdmOpcode)src->code[ip++];
        if (!idm_bc_emit(dst, (uint32_t)op, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
        switch (op) {
            case IDM_OP_HALT: case IDM_OP_RETURN: case IDM_OP_POP: case IDM_OP_MAKE_CELL: case IDM_OP_LOAD_CELL:
            case IDM_OP_STORE_CELL: case IDM_OP_SELF: case IDM_OP_SPAWN: case IDM_OP_SEND: case IDM_OP_EXIT:
            case IDM_OP_SPAWN_LINK: case IDM_OP_SPAWN_MONITOR:
            case IDM_OP_EXIT_SIGNAL:
            case IDM_OP_LINK: case IDM_OP_UNLINK: case IDM_OP_MONITOR: case IDM_OP_DEMONITOR: case IDM_OP_TRAP_EXIT:
            case IDM_OP_EXEC: case IDM_OP_AWAIT: case IDM_OP_APPLY: case IDM_OP_LEAVE_NAMESPACE:
            case IDM_OP_RESCUE_POP: case IDM_OP_RAISE: case IDM_OP_LOAD_RAISED:
                break;
            case IDM_OP_DEFINE_TRAIT: {
                uint32_t trait_const = src->code[ip++];
                uint32_t requirement_count = src->code[ip++];
                uint32_t method_count = src->code[ip++];
                if (!idm_bc_emit(dst, trait_const + const_off, NULL) || !idm_bc_emit(dst, requirement_count, NULL) ||
                    !idm_bc_emit(dst, method_count, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < requirement_count; i++) {
                    uint32_t requirement_const = src->code[ip++];
                    if (!idm_bc_emit(dst, requirement_const + const_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                }
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = src->code[ip++];
                    if (!idm_bc_emit(dst, method_const + const_off, NULL) || !copy_arity_operands(src, dst, &ip, err)) return false;
                    uint32_t has_default = src->code[ip++];
                    if (!idm_bc_emit(dst, has_default, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                }
                break;
            }
            case IDM_OP_IMPLEMENT_TRAIT: {
                uint32_t trait_const = src->code[ip++];
                uint32_t type_const = src->code[ip++];
                uint32_t provider_const = src->code[ip++];
                uint32_t provider_key_const = src->code[ip++];
                uint32_t method_count = src->code[ip++];
                if (!idm_bc_emit(dst, trait_const + const_off, NULL) || !idm_bc_emit(dst, type_const + const_off, NULL) ||
                    !idm_bc_emit(dst, provider_const + const_off, NULL) || !idm_bc_emit(dst, provider_key_const + const_off, NULL) ||
                    !idm_bc_emit(dst, method_count, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = src->code[ip++];
                    if (!idm_bc_emit(dst, method_const + const_off, NULL) || !copy_arity_operands(src, dst, &ip, err)) return false;
                }
                break;
            }
            case IDM_OP_CALL_METHOD:
            case IDM_OP_TAIL_CALL_METHOD:
                if (!idm_bc_emit(dst, src->code[ip++] + const_off, NULL) || !idm_bc_emit(dst, src->code[ip++] + const_off, NULL) || !idm_bc_emit(dst, src->code[ip++], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_LOAD_CONST:
            case IDM_OP_ENTER_NAMESPACE:
                if (!idm_bc_emit(dst, src->code[ip++] + const_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_MAKE_CLOSURE:
                if (!idm_bc_emit(dst, src->code[ip++] + fn_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_LOAD_ARG: case IDM_OP_LOAD_LOCAL: case IDM_OP_STORE_LOCAL: case IDM_OP_LOAD_CAPTURE:
            case IDM_OP_LOAD_GLOBAL: case IDM_OP_STORE_GLOBAL:
                if (!idm_bc_emit(dst, src->code[ip++], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_CALL:
            case IDM_OP_TAIL_CALL: {
                uint32_t operand = src->code[ip++];
                if (!idm_bc_emit(dst, operand, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                if ((operand & IDM_CALL_DIRECT_FLAG) == 0) break;
                uint32_t entry_count = src->code[ip++];
                uint32_t capture_count = src->code[ip++];
                if (!idm_bc_emit(dst, entry_count, NULL) || !idm_bc_emit(dst, capture_count, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) {
                    if (!idm_bc_emit(dst, src->code[ip++] + fn_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                }
                break;
            }
            case IDM_OP_IMPORT_GLOBAL:
            case IDM_OP_PRIM_CALL:
                if (!idm_bc_emit(dst, src->code[ip++], NULL) || !idm_bc_emit(dst, src->code[ip++], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_MAKE_CLOSURE_CAPTURES:
                if (!idm_bc_emit(dst, src->code[ip++] + fn_off, NULL) || !idm_bc_emit(dst, src->code[ip++], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case IDM_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = src->code[ip++];
                uint32_t capture_count = src->code[ip++];
                if (!idm_bc_emit(dst, entry_count, NULL) || !idm_bc_emit(dst, capture_count, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) if (!idm_bc_emit(dst, src->code[ip++] + fn_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            }
            case IDM_OP_JUMP: case IDM_OP_JUMP_IF_FALSE: case IDM_OP_RECV: case IDM_OP_TAIL_RECV: case IDM_OP_RESCUE_PUSH:
                if (!idm_bc_emit(dst, src->code[ip++] + code_off, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            default:
                return idm_error_set(err, idm_span_unknown(NULL), "relocation: invalid opcode %u", (unsigned)op);
        }
    }
    return true;
}

static bool link_spans(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t code_off, IdmError *err) {
    for (size_t i = 0; i < src->span_count; i++) {
        uint32_t file_index = 0;
        if (!span_file_index(dst, src->span_files[src->spans[i].file], &file_index)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (dst->span_count == dst->span_cap) {
            size_t cap = dst->span_cap ? dst->span_cap * 2u : 32u;
            void *grown = realloc(dst->spans, cap * sizeof(*dst->spans));
            if (!grown) return idm_error_oom(err, idm_span_unknown(NULL));
            dst->spans = grown;
            dst->span_cap = cap;
        }
        dst->spans[dst->span_count].offset = src->spans[i].offset + code_off;
        dst->spans[dst->span_count].file = file_index;
        dst->spans[dst->span_count].line = src->spans[i].line;
        dst->spans[dst->span_count].column = src->spans[i].column;
        dst->span_count++;
    }
    return true;
}

static bool link_name_notes(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t code_off, IdmError *err) {
    for (size_t i = 0; i < src->name_note_count; i++) {
        if (!idm_bc_note_name(dst, src->name_notes[i].offset + code_off, src->name_notes[i].name)) {
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    return true;
}

bool idm_bc_link(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t *out_const_offset, uint32_t *out_fn_offset, uint32_t *out_code_offset, IdmError *err) {
    uint32_t const_off = (uint32_t)dst->const_count;
    uint32_t fn_off = (uint32_t)dst->function_count;
    uint32_t code_off = (uint32_t)dst->code_count;
    if (!link_spans(dst, src, code_off, err)) return false;
    for (size_t i = 0; i < src->const_count; i++) if (!idm_bc_add_const(dst, src->constants[i], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < src->function_count; i++) {
        const IdmBcFunction *sf = &src->functions[i];
        uint32_t nf = 0;
        if (!idm_bc_add_function(dst, sf->name, sf->arity, sf->local_count, sf->entry + code_off, &nf)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (sf->pattern_count != 0) {
            IdmPattern **clones = malloc((size_t)sf->pattern_count * sizeof(*clones));
            if (!clones) return idm_error_oom(err, idm_span_unknown(NULL));
            bool ok = true;
            for (uint32_t p = 0; p < sf->pattern_count && ok; p++) { clones[p] = idm_pat_clone(sf->param_patterns[p]); if (!clones[p]) ok = false; }
            if (!ok || !idm_bc_set_function_patterns_take(dst, nf, clones, sf->pattern_count)) {
                for (uint32_t p = 0; p < sf->pattern_count; p++) idm_pat_free(clones[p]);
                free(clones);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
        if (sf->pattern_local_count != 0) {
            IdmPatternLocal *locals = malloc((size_t)sf->pattern_local_count * sizeof(*locals));
            if (!locals) return idm_error_oom(err, idm_span_unknown(NULL));
            bool ok = true;
            for (uint32_t p = 0; p < sf->pattern_local_count && ok; p++) { locals[p].name = idm_strdup(sf->pattern_locals[p].name); locals[p].slot = sf->pattern_locals[p].slot; if (!locals[p].name) ok = false; }
            if (!ok || !idm_bc_set_function_pattern_locals_take(dst, nf, locals, sf->pattern_local_count)) {
                for (uint32_t p = 0; p < sf->pattern_local_count; p++) free(locals[p].name);
                free(locals);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
    }
    for (size_t i = 0; i < src->function_count; i++) {
        const IdmBcFunction *sf = &src->functions[i];
        if (sf->has_guard && !idm_bc_set_function_guard(dst, fn_off + (uint32_t)i, sf->guard_function + fn_off)) {
            return idm_error_set(err, idm_span_unknown(NULL), "linked guard function index is out of bounds");
        }
    }
    if (!reloc_emit(dst, src, const_off, fn_off, code_off, err)) return false;
    if (!link_name_notes(dst, src, code_off, err)) return false;
    if (out_const_offset) *out_const_offset = const_off;
    if (out_fn_offset) *out_fn_offset = fn_off;
    if (out_code_offset) *out_code_offset = code_off;
    return true;
}

static bool value_relocate_syntax(IdmRuntime *rt, IdmValue v, IdmScopeId min_id, int64_t delta, IdmValue *out, bool *changed, IdmError *err) {
    *out = v;
    switch (v.tag) {
        case IDM_VAL_SYNTAX: {
            const IdmSyntax *syn = idm_syntax_get(v, err);
            if (!syn) return false;
            IdmSyntax *copy = idm_syn_clone(syn);
            if (!copy) return idm_error_oom(err, idm_span_unknown(NULL));
            idm_syn_scope_relocate_tree(copy, min_id, delta);
            *out = idm_syntax_value(rt, copy, err);
            idm_syn_free(copy);
            if (err && err->present) return false;
            *changed = true;
            return true;
        }
        case IDM_VAL_PAIR: {
            IdmValue car = idm_car(v, err);
            IdmValue cdr = idm_cdr(v, err);
            if (err && err->present) return false;
            IdmValue new_car, new_cdr;
            bool child_changed = false;
            if (!value_relocate_syntax(rt, car, min_id, delta, &new_car, &child_changed, err)) return false;
            if (!value_relocate_syntax(rt, cdr, min_id, delta, &new_cdr, &child_changed, err)) return false;
            if (!child_changed) return true;
            *out = idm_cons(rt, new_car, new_cdr, err);
            if (err && err->present) return false;
            *changed = true;
            return true;
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            size_t n = idm_sequence_count(v);
            IdmValue *items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
            bool child_changed = false;
            for (size_t i = 0; i < n; i++) {
                IdmValue item = idm_sequence_item(v, i, err);
                if ((err && err->present) || !value_relocate_syntax(rt, item, min_id, delta, &items[i], &child_changed, err)) {
                    free(items);
                    return false;
                }
            }
            if (child_changed) {
                *out = v.tag == IDM_VAL_TUPLE ? idm_tuple(rt, items, n, err) : idm_vector(rt, items, n, err);
                if (err && err->present) {
                    free(items);
                    return false;
                }
                *changed = true;
            }
            free(items);
            return true;
        }
        case IDM_VAL_DICT: {
            size_t n = idm_dict_count(v);
            IdmDictEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
            bool child_changed = false;
            for (size_t i = 0; i < n; i++) {
                IdmValue key, val;
                if (!idm_dict_entry(v, i, &key, &val)) {
                    free(entries);
                    return idm_error_set(err, idm_span_unknown(NULL), "dict entry walk failed");
                }
                if (!value_relocate_syntax(rt, key, min_id, delta, &entries[i].key, &child_changed, err) ||
                    !value_relocate_syntax(rt, val, min_id, delta, &entries[i].value, &child_changed, err)) {
                    free(entries);
                    return false;
                }
            }
            if (child_changed) {
                *out = idm_dict(rt, entries, n, err);
                if (err && err->present) {
                    free(entries);
                    return false;
                }
                *changed = true;
            }
            free(entries);
            return true;
        }
        default:
            return true;
    }
}

static bool pattern_relocate_syntax(IdmRuntime *rt, IdmPattern *pat, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (!pat) return true;
    bool changed = false;
    switch (pat->kind) {
        case IDM_PAT_LITERAL:
            return value_relocate_syntax(rt, pat->as.literal, min_id, delta, &pat->as.literal, &changed, err);
        case IDM_PAT_PAIR:
            return pattern_relocate_syntax(rt, pat->as.pair.left, min_id, delta, err) &&
                   pattern_relocate_syntax(rt, pat->as.pair.right, min_id, delta, err);
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (!pattern_relocate_syntax(rt, pat->as.seq.items[i], min_id, delta, err)) return false;
            }
            return true;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                if (!pattern_relocate_syntax(rt, pat->as.seq_rest.items[i], min_id, delta, err)) return false;
            }
            return pattern_relocate_syntax(rt, pat->as.seq_rest.rest, min_id, delta, err);
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (!value_relocate_syntax(rt, pat->as.dict.entries[i].key, min_id, delta, &pat->as.dict.entries[i].key, &changed, err)) return false;
                if (!pattern_relocate_syntax(rt, pat->as.dict.entries[i].pattern, min_id, delta, err)) return false;
            }
            return pattern_relocate_syntax(rt, pat->as.dict.rest, min_id, delta, err);
        default:
            return true;
    }
}

bool idm_bc_relocate_syntax_scopes(IdmRuntime *rt, IdmBytecodeModule *module, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (delta == 0) return true;
    for (size_t i = 0; i < module->const_count; i++) {
        bool changed = false;
        IdmValue replaced;
        if (!value_relocate_syntax(rt, module->constants[i], min_id, delta, &replaced, &changed, err)) return false;
        if (changed) module->constants[i] = replaced;
    }
    for (size_t i = 0; i < module->function_count; i++) {
        for (uint32_t p = 0; p < module->functions[i].pattern_count; p++) {
            if (!pattern_relocate_syntax(rt, module->functions[i].param_patterns[p], min_id, delta, err)) return false;
        }
    }
    return true;
}
