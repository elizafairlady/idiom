#include "idiom/bytecode.h"

#include "idiom/core.h"
#include "idiom/prims.h"
#include "idiom/regex.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stdlib.h>
#include <string.h>

#define IDM_IC_VERSION 47u
#define IDM_IC_MIN_READ_VERSION 47u

static const IdmOpcodeInfo opcode_infos[IDM_OP_COUNT] = {
#define IDM_OPCODE_INFO(name, fixed, count_index, repeat, roles, repeat_roles, reg_roles, reg_ranges, flow_kind, branch, tail) \
    [IDM_OP_##name] = {#name, fixed, count_index, repeat, roles, repeat_roles, reg_roles, reg_ranges, flow_kind, branch, tail},
    IDM_OPCODE_LIST(IDM_OPCODE_INFO)
#undef IDM_OPCODE_INFO
};

static bool valid_value_sequence_kind(uint32_t kind) {
    return kind <= (uint32_t)IDM_VALUE_SEQ_DICT;
}

static bool valid_syntax_build_kind(uint32_t kind) {
    return kind <= (uint32_t)IDM_SYNTAX_BUILD_GROUP;
}

static bool put_primitive_ref(IdmBuffer *out, uint32_t primitive, IdmError *err) {
    const IdmPrimitiveInfo *info = idm_primitive_info((IdmPrimitive)primitive);
    if (!info) return idm_error_set(err, idm_span_unknown(NULL), "unknown primitive %u", primitive);
    const char *home = idm_primitive_home((IdmPrimitive)primitive);
    if (!idm_buf_put_str(out, home, strlen(home)) ||
        !idm_buf_put_str(out, info->name, strlen(info->name))) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool read_primitive_ref(IdmByteReader *r, IdmPrimitive *out, IdmError *err) {
    char *home = idm_rd_string(r, NULL);
    char *name = idm_rd_string(r, NULL);
    if (!home || !name) {
        free(home);
        free(name);
        return idm_error_set(err, idm_span_unknown(NULL), "truncated primitive reference");
    }
    bool ok = idm_primitive_lookup(home, name, out);
    if (!ok) idm_error_set(err, idm_span_unknown(NULL), "unknown primitive '%s/%s'", home, name);
    free(home);
    free(name);
    return ok;
}

static void selector_sites_clear(IdmBytecodeModule *module) {
    for (size_t i = 0; i < module->selector_site_count; i++) idm_pattern_selector_free(module->selector_sites[i].selector);
    free(module->selector_sites);
    module->selector_sites = NULL;
    module->selector_site_count = 0;
    module->selector_site_cap = 0;
    free(module->selector_by_offset);
    module->selector_by_offset = NULL;
    module->selector_by_offset_count = 0;
    module->selectors_prepared = false;
}

static void function_selectors_clear(IdmBytecodeModule *module) {
    for (size_t i = 0; i < module->function_count; i++) {
        idm_pattern_selector_free(module->functions[i].selector);
        module->functions[i].selector = NULL;
    }
}

static void selectors_clear(IdmBytecodeModule *module) {
    selector_sites_clear(module);
    function_selectors_clear(module);
    module->selectors_prepared = false;
    module->selector_generation++;
}

static void selectors_invalidate(IdmBytecodeModule *module) {
    if (module->selectors_prepared || module->selector_site_count != 0 || module->selector_by_offset || module->selector_by_offset_count != 0) {
        selectors_clear(module);
        return;
    }
    module->selectors_prepared = false;
}

static void module_mutated(IdmBytecodeModule *module) {
    module->verified = false;
    module->literals_interned = false;
    selectors_invalidate(module);
}

static bool span_file_index(IdmBytecodeModule *module, const char *file, uint32_t *out_index) {
    for (size_t i = 0; i < module->span_file_count; i++) {
        if (strcmp(module->span_files[i], file) == 0) {
            *out_index = (uint32_t)i;
            return true;
        }
    }
    if (module->span_file_count == module->span_file_cap) {
        if (!idm_grow((void **)&module->span_files, &module->span_file_cap, sizeof(*module->span_files), 4u, module->span_file_count + 1u)) return false;
    }
    char *name = idm_strdup(file);
    if (!name) return false;
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
        if (!idm_grow((void **)&module->spans, &module->span_cap, sizeof(*module->spans), 32u, module->span_count + 1u)) return false;
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
        if (!idm_grow((void **)&module->name_notes, &module->name_note_cap, sizeof(*module->name_notes), 32u, module->name_note_count + 1u)) return false;
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
    module->types = NULL;
    module->type_count = 0;
    module->type_cap = 0;
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
    module->selector_sites = NULL;
    module->selector_site_count = 0;
    module->selector_site_cap = 0;
    module->selector_by_offset = NULL;
    module->selector_by_offset_count = 0;
    module->selectors_prepared = false;
    module->literals_interned = false;
    module->verified = false;
    module->selector_generation = 0;
}

void idm_bc_destroy(IdmBytecodeModule *module) {
    if (!module) return;
    free(module->code);
    free(module->constants);
    for (size_t i = 0; i < module->type_count; i++) idm_type_term_destroy(&module->types[i]);
    free(module->types);
    for (size_t i = 0; i < module->span_file_count; i++) free(module->span_files[i]);
    free(module->span_files);
    free(module->spans);
    for (size_t i = 0; i < module->name_note_count; i++) free(module->name_notes[i].name);
    free(module->name_notes);
    for (size_t i = 0; i < module->function_count; i++) {
        idm_pattern_selector_free(module->functions[i].selector);
        free(module->functions[i].name);
        for (uint32_t p = 0; p < module->functions[i].pattern_count; p++) idm_pat_free(module->functions[i].param_patterns[p]);
        free(module->functions[i].param_patterns);
        for (uint32_t p = 0; p < module->functions[i].pattern_local_count; p++) free(module->functions[i].pattern_locals[p].name);
        free(module->functions[i].pattern_locals);
    }
    selector_sites_clear(module);
    free(module->functions);
    idm_bc_init(module);
}

bool idm_bc_add_const(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index) {
    if (module->const_count == module->const_cap) {
        if (!idm_grow((void **)&module->constants, &module->const_cap, sizeof(*module->constants), 16u, module->const_count + 1u)) return false;
    }
    if (module->const_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->const_count;
    module->constants[module->const_count++] = value;
    module_mutated(module);
    if (out_index) *out_index = index;
    return true;
}

bool idm_bc_add_type_term(IdmBytecodeModule *module, const IdmTypeTerm *term, uint32_t *out_index) {
    if (!module || !term) return false;
    if (module->type_count == module->type_cap) {
        if (!idm_grow((void **)&module->types, &module->type_cap, sizeof(*module->types), 16u, module->type_count + 1u)) return false;
    }
    if (module->type_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->type_count;
    memset(&module->types[index], 0, sizeof(module->types[index]));
    if (!idm_type_term_copy(&module->types[index], term)) return false;
    module->type_count++;
    module_mutated(module);
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
        case IDM_PAT_SYNTAX:
            return true;
        default:
            return true;
    }
}

bool idm_bc_intern_literals(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err) {
    if (!module) return true;
    if (module->literals_interned && module->selectors_prepared) {
        if (!module->verified && !idm_bc_verify(module, err)) return false;
        module->verified = true;
        for (size_t i = 0; i < module->const_count; i++) {
            module->constants[i] = idm_value_copy(rt, &rt->immortal, module->constants[i], err);
            if (err->present) return false;
        }
        return true;
    }
    if (!module->verified && !idm_bc_verify(module, err)) return false;
    module->verified = true;
    selectors_clear(module);
    module->literals_interned = false;
    for (size_t i = 0; i < module->const_count; i++) {
        module->constants[i] = idm_value_copy(rt, &rt->immortal, module->constants[i], err);
        if (err->present) return false;
    }
    for (size_t i = 0; i < module->function_count; i++) {
        IdmBcFunction *fn = &module->functions[i];
        for (uint32_t p = 0; p < fn->pattern_count; p++)
            if (!intern_pattern_literals(rt, fn->param_patterns[p], err)) return false;
    }
    module->literals_interned = true;
    return idm_bc_prepare_selectors(rt, module, err);
}

bool idm_bc_is_finalized(const IdmBytecodeModule *module) {
    return module && module->literals_interned && module->selectors_prepared;
}

bool idm_bc_add_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index) {
    if (module->function_count == module->function_cap) {
        if (!idm_grow((void **)&module->functions, &module->function_cap, sizeof(*module->functions), 8u, module->function_count + 1u)) return false;
    }
    if (module->function_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->function_count;
    module->functions[index].name = idm_strdup(name ? name : "<anonymous>");
    if (!module->functions[index].name) return false;
    module->functions[index].arity = arity;
    module->functions[index].call_arity = idm_arity_exact(arity);
    module->functions[index].local_count = local_count;
    module->functions[index].register_count = arity + local_count;
    module->functions[index].entry = entry;
    module->functions[index].has_guard = false;
    module->functions[index].guard_function = UINT32_MAX;
    module->functions[index].primitive_backed = false;
    module->functions[index].primitive = 0;
    module->functions[index].param_patterns = NULL;
    module->functions[index].pattern_count = 0;
    module->functions[index].pattern_locals = NULL;
    module->functions[index].pattern_local_count = 0;
    module->functions[index].trivial_match = false;
    module->functions[index].selector = NULL;
    module->function_count++;
    module_mutated(module);
    if (out_index) *out_index = index;
    return true;
}

bool idm_bc_add_primitive_function(IdmBytecodeModule *module, const char *name, IdmArity arity, uint32_t primitive, uint32_t *out_index) {
    if (module->function_count == module->function_cap) {
        if (!idm_grow((void **)&module->functions, &module->function_cap, sizeof(*module->functions), 8u, module->function_count + 1u)) return false;
    }
    if (module->function_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->function_count;
    module->functions[index].name = idm_strdup(name ? name : "<primitive>");
    if (!module->functions[index].name) return false;
    module->functions[index].arity = arity.min;
    module->functions[index].call_arity = arity;
    module->functions[index].local_count = 0;
    module->functions[index].register_count = arity.min;
    module->functions[index].entry = 0;
    module->functions[index].has_guard = false;
    module->functions[index].guard_function = UINT32_MAX;
    module->functions[index].primitive_backed = true;
    module->functions[index].primitive = primitive;
    module->functions[index].param_patterns = NULL;
    module->functions[index].pattern_count = 0;
    module->functions[index].pattern_locals = NULL;
    module->functions[index].pattern_local_count = 0;
    module->functions[index].trivial_match = true;
    module->functions[index].selector = NULL;
    module->function_count++;
    module_mutated(module);
    if (out_index) *out_index = index;
    return true;
}

bool idm_bc_set_function_entry(IdmBytecodeModule *module, uint32_t function_index, size_t entry) {
    if (function_index >= module->function_count) return false;
    module->functions[function_index].entry = entry;
    module_mutated(module);
    return true;
}

bool idm_bc_set_function_register_count(IdmBytecodeModule *module, uint32_t function_index, uint32_t register_count) {
    if (function_index >= module->function_count) return false;
    IdmBcFunction *fn = &module->functions[function_index];
    uint32_t minimum = fn->arity + fn->local_count;
    if (register_count < minimum) register_count = minimum;
    fn->register_count = register_count;
    module_mutated(module);
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
    module_mutated(module);
    return true;
}

bool idm_bc_set_function_pattern_locals_take(IdmBytecodeModule *module, uint32_t function_index, IdmPatternLocal *locals, uint32_t local_count) {
    if (function_index >= module->function_count) return false;
    IdmBcFunction *fn = &module->functions[function_index];
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) free(fn->pattern_locals[i].name);
    free(fn->pattern_locals);
    fn->pattern_locals = locals;
    fn->pattern_local_count = local_count;
    module_mutated(module);
    return true;
}

bool idm_bc_set_function_guard(IdmBytecodeModule *module, uint32_t function_index, uint32_t guard_function) {
    if (function_index >= module->function_count || guard_function >= module->function_count) return false;
    module->functions[function_index].has_guard = true;
    module->functions[function_index].guard_function = guard_function;
    module_mutated(module);
    return true;
}

bool idm_bc_emit(IdmBytecodeModule *module, uint32_t word, size_t *out_offset) {
    if (module->code_count == module->code_cap) {
        if (!idm_grow((void **)&module->code, &module->code_cap, sizeof(*module->code), 64u, module->code_count + 1u)) return false;
    }
    if (out_offset) *out_offset = module->code_count;
    module->code[module->code_count++] = word;
    module_mutated(module);
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
    module_mutated(module);
    return true;
}

const IdmOpcodeInfo *idm_opcode_info(IdmOpcode op) {
    size_t index = (size_t)op;
    if (index >= (size_t)IDM_OP_COUNT) return NULL;
    return &opcode_infos[index];
}

bool idm_opcode_valid(IdmOpcode op) {
    return idm_opcode_info(op) != NULL;
}

size_t idm_opcode_count(void) {
    return (size_t)IDM_OP_COUNT;
}

const char *idm_opcode_name(IdmOpcode op) {
    const IdmOpcodeInfo *info = idm_opcode_info(op);
    return info ? info->name : "<bad-op>";
}

bool idm_bc_instruction_width(const IdmBytecodeModule *module, size_t offset, size_t *out_width, IdmError *err) {
    if (offset >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "instruction offset %zu out of bounds", offset);
    IdmOpcode op = (IdmOpcode)module->code[offset];
    const IdmOpcodeInfo *info = idm_opcode_info(op);
    if (!info) return idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u at offset %zu", (unsigned)op, offset);
    size_t width = 1u + (size_t)info->fixed_operands;
    if (width > module->code_count - offset) return idm_error_set(err, idm_span_unknown(NULL), "%s truncated at offset %zu", info->name, offset);
    if (info->count_operand != IDM_OPCODE_NO_COUNT) {
        if (info->count_operand >= info->fixed_operands || info->repeat_width == 0) {
            return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid opcode metadata", info->name);
        }
        uint32_t count = module->code[offset + 1u + (size_t)info->count_operand];
        if (count > (SIZE_MAX - width) / (size_t)info->repeat_width) return idm_error_set(err, idm_span_unknown(NULL), "%s payload length overflow", info->name);
        width += (size_t)count * (size_t)info->repeat_width;
        if (width > module->code_count - offset) return idm_error_set(err, idm_span_unknown(NULL), "%s payload out of bounds at offset %zu", info->name, offset);
    }
    if (out_width) *out_width = width;
    return true;
}

typedef struct {
    IdmOpcode op;
    const IdmOpcodeInfo *info;
    size_t offset;
    size_t width;
    const uint32_t *operands;
    uint32_t repeat_count;
    const uint32_t *repeat_operands;
} IdmBcInstr;

static char opcode_role_at(const char *roles, size_t index) {
    char role = roles ? roles[index] : '\0';
    return role ? role : 'r';
}

static bool decode_instr(const IdmBytecodeModule *module, size_t offset, IdmBcInstr *out, IdmError *err) {
    size_t width = 0;
    if (!idm_bc_instruction_width(module, offset, &width, err)) return false;
    IdmOpcode op = (IdmOpcode)module->code[offset];
    const IdmOpcodeInfo *info = idm_opcode_info(op);
    if (!info) return idm_error_set(err, idm_span_unknown(NULL), "invalid opcode %u at offset %zu", (unsigned)op, offset);
    out->op = op;
    out->info = info;
    out->offset = offset;
    out->width = width;
    out->operands = &module->code[offset + 1u];
    out->repeat_count = info->count_operand == IDM_OPCODE_NO_COUNT ? 0u : out->operands[info->count_operand];
    out->repeat_operands = &module->code[offset + 1u + (size_t)info->fixed_operands];
    return true;
}

bool idm_bc_build_selector_for_entries(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *entries, size_t entry_count, IdmPatternSelector **out, IdmError *err) {
    *out = NULL;
    if (!rt) return idm_error_set(err, idm_span_unknown(NULL), "selector requires a runtime");
    if (!module) return idm_error_set(err, idm_span_unknown(NULL), "closure requires a module");
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
        clauses[i].arity = fn->call_arity;
        clauses[i].patterns = fn->param_patterns;
        clauses[i].pattern_count = fn->pattern_count;
        clauses[i].pattern_locals = fn->pattern_locals;
        clauses[i].pattern_local_count = fn->pattern_local_count;
        clauses[i].trivial_match = fn->trivial_match;
        clauses[i].has_guard = fn->has_guard;
    }
    bool ok = idm_pattern_selector_build(rt, clauses, entry_count, out, err);
    free(clauses);
    return ok;
}

static bool selector_site_add_take(IdmBytecodeModule *module, size_t offset, IdmPatternSelector *selector, IdmError *err) {
    if (offset > UINT32_MAX) {
        idm_pattern_selector_free(selector);
        return idm_error_set(err, idm_span_unknown(NULL), "selector site offset is out of bounds");
    }
    if (offset >= module->selector_by_offset_count) {
        idm_pattern_selector_free(selector);
        return idm_error_set(err, idm_span_unknown(NULL), "selector site offset is out of bounds");
    }
    if (module->selector_site_count == module->selector_site_cap) {
        if (!idm_grow((void **)&module->selector_sites, &module->selector_site_cap, sizeof(*module->selector_sites), 16u, module->selector_site_count + 1u)) {
            idm_pattern_selector_free(selector);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    module->selector_sites[module->selector_site_count].offset = (uint32_t)offset;
    module->selector_sites[module->selector_site_count].selector = selector;
    module->selector_site_count++;
    module->selector_by_offset[offset] = selector;
    return true;
}

static bool selector_site_for_entries(IdmRuntime *rt, IdmBytecodeModule *module, size_t offset, const uint32_t *entries, size_t entry_count, IdmError *err) {
    IdmPatternSelector *selector = NULL;
    if (!idm_bc_build_selector_for_entries(rt, module, entries, entry_count, &selector, err)) return false;
    return selector_site_add_take(module, offset, selector, err);
}

static bool selector_site_for_instr(IdmRuntime *rt, IdmBytecodeModule *module, const IdmBcInstr *instr, IdmError *err) {
    uint32_t fixed_entries[8];
    size_t fixed_count = 0;
    for (uint8_t i = 0; i < instr->info->fixed_operands; i++) {
        if (opcode_role_at(instr->info->operand_roles, i) == 'f') fixed_entries[fixed_count++] = instr->operands[i];
    }
    size_t repeat_functions = 0;
    for (uint8_t part = 0; part < instr->info->repeat_width; part++) {
        if (opcode_role_at(instr->info->repeat_roles, part) == 'f') repeat_functions++;
    }
    if (fixed_count == 0 && repeat_functions == 0) return true;
    if (repeat_functions == 0) return selector_site_for_entries(rt, module, instr->offset, fixed_entries, fixed_count, err);
    if (fixed_count == 0 && repeat_functions == 1u && instr->info->repeat_width == 1u) {
        return selector_site_for_entries(rt, module, instr->offset, instr->repeat_operands, instr->repeat_count, err);
    }
    size_t repeat_count = (size_t)instr->repeat_count * repeat_functions;
    if (repeat_functions != 0 && repeat_count / repeat_functions != instr->repeat_count) {
        return idm_error_set(err, idm_span_unknown(NULL), "%s selector entry count overflow", instr->info->name);
    }
    size_t entry_count = fixed_count + repeat_count;
    uint32_t *entries = entry_count == 0 ? NULL : malloc(entry_count * sizeof(*entries));
    if (entry_count != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t at = 0;
    for (size_t i = 0; i < fixed_count; i++) entries[at++] = fixed_entries[i];
    for (uint32_t item = 0; item < instr->repeat_count; item++) {
        for (uint8_t part = 0; part < instr->info->repeat_width; part++) {
            if (opcode_role_at(instr->info->repeat_roles, part) == 'f') {
                entries[at++] = instr->repeat_operands[(size_t)item * (size_t)instr->info->repeat_width + (size_t)part];
            }
        }
    }
    bool ok = selector_site_for_entries(rt, module, instr->offset, entries, entry_count, err);
    free(entries);
    return ok;
}

IdmPatternSelector *idm_bc_selector_at(const IdmBytecodeModule *module, size_t offset) {
    if (!module || offset >= module->selector_by_offset_count) return NULL;
    return module->selector_by_offset[offset];
}

bool idm_bc_prepare_selectors(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "bytecode.prepare_selectors");
    if (module->selectors_prepared) {
        idm_profile_count("bytecode.prepare_selectors.cached", 1u);
        idm_profile_scope_end(&prof);
        return true;
    }
    selectors_clear(module);
    module->selector_by_offset_count = module->code_count;
    module->selector_by_offset = module->code_count == 0 ? NULL : calloc(module->code_count, sizeof(*module->selector_by_offset));
    if (module->code_count != 0 && !module->selector_by_offset) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->function_count; i++) {
        uint32_t entry = (uint32_t)i;
        if (!idm_bc_build_selector_for_entries(rt, module, &entry, 1u, &module->functions[i].selector, err)) {
            selectors_clear(module);
            return false;
        }
    }
    size_t ip = 0;
    while (ip < module->code_count) {
        IdmBcInstr instr;
        if (!decode_instr(module, ip, &instr, err)) { selectors_clear(module); return false; }
        if (!selector_site_for_instr(rt, module, &instr, err)) { selectors_clear(module); return false; }
        ip += instr.width;
    }
    module->selectors_prepared = true;
    idm_profile_count("bytecode.prepare_selectors.functions", (uint64_t)module->function_count);
    idm_profile_count("bytecode.prepare_selectors.code_words", (uint64_t)module->code_count);
    idm_profile_scope_end(&prof);
    return true;
}

static bool verify_reference_role(const IdmBytecodeModule *module, const IdmBcInstr *instr, char role, uint32_t word, size_t *targets, size_t *target_count, IdmError *err) {
    switch (role) {
        case 'r':
            return true;
        case 'c':
            if (word < module->const_count) return true;
            return idm_error_set(err, idm_span_unknown(NULL), "%s constant %u out of bounds", instr->info->name, word);
        case 'T':
            if (word == UINT32_MAX || word < module->type_count) return true;
            return idm_error_set(err, idm_span_unknown(NULL), "%s type %u out of bounds", instr->info->name, word);
        case 'f':
            if (word < module->function_count) return true;
            return idm_error_set(err, idm_span_unknown(NULL), "%s function %u out of bounds", instr->info->name, word);
        case 'j':
            if (word >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s target %u out of bounds", instr->info->name, word);
            targets[(*target_count)++] = word;
            return true;
        case 'm':
            return word != 0 ? true : idm_error_set(err, idm_span_unknown(NULL), "%s requires at least one entry", instr->info->name);
        case 't':
            return word <= 1u ? true : idm_error_set(err, idm_span_unknown(NULL), "%s tail flag must be 0 or 1", instr->info->name);
        case 'p':
            return idm_primitive_info((IdmPrimitive)word) != NULL ? true : idm_error_set(err, idm_span_unknown(NULL), "%s primitive %u out of bounds", instr->info->name, word);
        case 'v':
            return valid_value_sequence_kind(word) ? true : idm_error_set(err, idm_span_unknown(NULL), "%s kind %u is invalid", instr->info->name, word);
        case 'y':
            return valid_syntax_build_kind(word) ? true : idm_error_set(err, idm_span_unknown(NULL), "%s kind %u is invalid", instr->info->name, word);
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid operand role '%c'", instr->info->name, role);
    }
}

static bool verify_instr_references(const IdmBytecodeModule *module, const IdmBcInstr *instr, size_t *targets, size_t *target_count, IdmError *err) {
    for (uint8_t i = 0; i < instr->info->fixed_operands; i++) {
        if (!verify_reference_role(module, instr, opcode_role_at(instr->info->operand_roles, i), instr->operands[i], targets, target_count, err)) return false;
    }
    for (uint32_t item = 0; item < instr->repeat_count; item++) {
        for (uint8_t part = 0; part < instr->info->repeat_width; part++) {
            size_t index = (size_t)item * (size_t)instr->info->repeat_width + (size_t)part;
            if (!verify_reference_role(module, instr, opcode_role_at(instr->info->repeat_roles, part), instr->repeat_operands[index], targets, target_count, err)) return false;
        }
    }
    return true;
}

static bool verify_scan(const IdmBytecodeModule *module, unsigned char *is_start, size_t *targets, size_t *target_count, IdmError *err) {
    size_t ip = 0;
    while (ip < module->code_count) {
        IdmBcInstr instr;
        if (!decode_instr(module, ip, &instr, err)) return false;
        is_start[ip] = 1u;
        if (!verify_instr_references(module, &instr, targets, target_count, err)) return false;
        ip += instr.width;
    }
    return true;
}

static bool verify_reg(uint32_t register_count, IdmOpcode op, const char *role, uint32_t reg, IdmError *err) {
    if (reg < register_count) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "%s %s register r%u out of bounds for %u registers", idm_opcode_name(op), role, reg, register_count);
}

static bool verify_reg_range(uint32_t register_count, IdmOpcode op, const char *role, uint32_t first, uint32_t count, IdmError *err) {
    if (count == 0) return true;
    if (first < register_count && count <= register_count - first) return true;
    unsigned long long last = (unsigned long long)first + (unsigned long long)count - 1u;
    return idm_error_set(err, idm_span_unknown(NULL), "%s %s register range r%u..r%llu out of bounds for %u registers", idm_opcode_name(op), role, first, last, register_count);
}

static const char *register_role_label(char role) {
    switch (role) {
        case 'd': return "destination";
        case 's': return "source";
        default: return "operand";
    }
}

static bool verify_instr_registers(const IdmBcFunction *fn, const IdmBcInstr *instr, IdmError *err) {
    for (uint8_t i = 0; i < instr->info->fixed_operands; i++) {
        char role = opcode_role_at(instr->info->register_roles, i);
        switch (role) {
            case 'r':
                break;
            case 'd':
            case 's':
                if (!verify_reg(fn->register_count, instr->op, register_role_label(role), instr->operands[i], err)) return false;
                break;
            default:
                return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid register role '%c'", instr->info->name, role);
        }
    }
    const char *ranges = instr->info->register_ranges ? instr->info->register_ranges : "";
    size_t len = strlen(ranges);
    if ((len % 2u) != 0) return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid register range metadata", instr->info->name);
    for (size_t i = 0; i < len; i += 2u) {
        char base_ch = ranges[i];
        char count_ch = ranges[i + 1u];
        if (base_ch < '0' || base_ch > '9' || count_ch < '0' || count_ch > '9') {
            return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid register range metadata", instr->info->name);
        }
        uint8_t base_index = (uint8_t)(base_ch - '0');
        uint8_t count_index = (uint8_t)(count_ch - '0');
        if (base_index >= instr->info->fixed_operands || count_index >= instr->info->fixed_operands) {
            return idm_error_set(err, idm_span_unknown(NULL), "%s has out-of-bounds register range metadata", instr->info->name);
        }
        if (!verify_reg_range(fn->register_count, instr->op, "range", instr->operands[base_index], instr->operands[count_index], err)) return false;
    }
    return true;
}

static bool verify_enqueue_target(const IdmBytecodeModule *module, const unsigned char *is_start, unsigned char *seen, size_t *work, size_t *work_count, uint32_t target, IdmOpcode op, IdmError *err) {
    if (target >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s target %u out of bounds", idm_opcode_name(op), target);
    if (!is_start[target]) return idm_error_set(err, idm_span_unknown(NULL), "%s target %u is not an instruction boundary", idm_opcode_name(op), target);
    if (!seen[target]) {
        seen[target] = 1u;
        work[(*work_count)++] = target;
    }
    return true;
}

static bool verify_enqueue_next(const IdmBytecodeModule *module, const unsigned char *is_start, unsigned char *seen, size_t *work, size_t *work_count, size_t next, IdmOpcode op, IdmError *err) {
    if (next >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "%s fallthrough is out of bounds", idm_opcode_name(op));
    if (!is_start[next]) return idm_error_set(err, idm_span_unknown(NULL), "%s fallthrough %zu is not an instruction boundary", idm_opcode_name(op), next);
    if (!seen[next]) {
        seen[next] = 1u;
        work[(*work_count)++] = next;
    }
    return true;
}

static bool verify_enqueue_tail_next(const IdmBytecodeModule *module, const unsigned char *is_start, unsigned char *seen, size_t *work, size_t *work_count, size_t next, IdmOpcode op, IdmError *err) {
    if (next >= module->code_count) return true;
    if (!is_start[next]) return idm_error_set(err, idm_span_unknown(NULL), "%s fallthrough %zu is not an instruction boundary", idm_opcode_name(op), next);
    if (!seen[next]) {
        seen[next] = 1u;
        work[(*work_count)++] = next;
    }
    return true;
}

static bool instr_fixed_operand(const IdmBcInstr *instr, uint8_t index, const char *role, uint32_t *out, IdmError *err) {
    if (index < instr->info->fixed_operands) {
        *out = instr->operands[index];
        return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid %s metadata", instr->info->name, role);
}

static bool verify_instr_flow(const IdmBytecodeModule *module, const unsigned char *is_start, unsigned char *seen, size_t *work, size_t *work_count, const IdmBcInstr *instr, IdmError *err) {
    size_t next = instr->offset + instr->width;
    uint32_t target = 0;
    uint32_t tail = 0;
    switch (instr->info->flow) {
        case IDM_OPCODE_FLOW_NEXT:
            return verify_enqueue_next(module, is_start, seen, work, work_count, next, instr->op, err);
        case IDM_OPCODE_FLOW_TERMINAL:
            return true;
        case IDM_OPCODE_FLOW_JUMP:
            return instr_fixed_operand(instr, instr->info->branch_operand, "branch", &target, err) &&
                   verify_enqueue_target(module, is_start, seen, work, work_count, target, instr->op, err);
        case IDM_OPCODE_FLOW_BRANCH:
            return instr_fixed_operand(instr, instr->info->branch_operand, "branch", &target, err) &&
                   verify_enqueue_target(module, is_start, seen, work, work_count, target, instr->op, err) &&
                   verify_enqueue_next(module, is_start, seen, work, work_count, next, instr->op, err);
        case IDM_OPCODE_FLOW_BRANCH_TAIL:
            return instr_fixed_operand(instr, instr->info->branch_operand, "branch", &target, err) &&
                   instr_fixed_operand(instr, instr->info->tail_operand, "tail", &tail, err) &&
                   verify_enqueue_target(module, is_start, seen, work, work_count, target, instr->op, err) &&
                   (tail ? verify_enqueue_tail_next(module, is_start, seen, work, work_count, next, instr->op, err) : verify_enqueue_next(module, is_start, seen, work, work_count, next, instr->op, err));
        case IDM_OPCODE_FLOW_TAIL_FALLTHROUGH:
            return instr_fixed_operand(instr, instr->info->tail_operand, "tail", &tail, err) &&
                   (tail ? verify_enqueue_tail_next(module, is_start, seen, work, work_count, next, instr->op, err) : verify_enqueue_next(module, is_start, seen, work, work_count, next, instr->op, err));
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid flow metadata", instr->info->name);
    }
}

static bool verify_function_registers(const IdmBytecodeModule *module, uint32_t function_index, const unsigned char *is_start, IdmError *err) {
    const IdmBcFunction *fn = &module->functions[function_index];
    if (fn->primitive_backed) return true;
    unsigned char *seen = calloc(module->code_count ? module->code_count : 1u, 1u);
    size_t *work = malloc((module->code_count ? module->code_count : 1u) * sizeof(*work));
    if (!seen || !work) {
        free(seen);
        free(work);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    size_t work_count = 0;
    seen[fn->entry] = 1u;
    work[work_count++] = fn->entry;
    bool ok = true;
    while (ok && work_count != 0) {
        size_t instr_ip = work[--work_count];
        IdmBcInstr instr;
        ok = decode_instr(module, instr_ip, &instr, err);
        if (!ok) break;
        ok = verify_instr_registers(fn, &instr, err) &&
             verify_instr_flow(module, is_start, seen, work, &work_count, &instr, err);
    }
    free(seen);
    free(work);
    return ok;
}

bool idm_bc_verify(const IdmBytecodeModule *module, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "bytecode.verify");
    if (module->function_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "bytecode module has no functions");
    for (size_t i = 0; i < module->function_count; i++) {
        if (!module->functions[i].primitive_backed && module->functions[i].entry >= module->code_count) return idm_error_set(err, idm_span_unknown(NULL), "function %zu entry is out of bounds", i);
        if (module->functions[i].register_count < module->functions[i].arity + module->functions[i].local_count) return idm_error_set(err, idm_span_unknown(NULL), "function %zu register count is smaller than args plus locals", i);
        if (module->functions[i].has_guard && module->functions[i].guard_function >= module->function_count) return idm_error_set(err, idm_span_unknown(NULL), "function %zu guard function is out of bounds", i);
        if (module->functions[i].has_guard && !idm_arity_equal(&module->functions[module->functions[i].guard_function].call_arity, &module->functions[i].call_arity)) return idm_error_set(err, idm_span_unknown(NULL), "function %zu guard arity mismatch", i);
    }
    size_t slots = module->code_count ? module->code_count : 1u;
    unsigned char *is_start = calloc(slots, 1u);
    size_t *targets = malloc(slots * sizeof(*targets));
    if (!is_start || !targets) { free(is_start); free(targets); return idm_error_oom(err, idm_span_unknown(NULL)); }
    size_t target_count = 0;
    bool ok = verify_scan(module, is_start, targets, &target_count, err);
    for (size_t i = 0; ok && i < module->function_count; i++) {
        if (module->functions[i].primitive_backed) continue;
        if (!is_start[module->functions[i].entry]) ok = idm_error_set(err, idm_span_unknown(NULL), "function %zu entry %zu is not an instruction boundary", i, module->functions[i].entry);
    }
    for (size_t i = 0; ok && i < target_count; i++) {
        if (!is_start[targets[i]]) ok = idm_error_set(err, idm_span_unknown(NULL), "branch target %zu is not an instruction boundary", targets[i]);
    }
    for (size_t i = 0; ok && i < module->function_count; i++) {
        ok = verify_function_registers(module, (uint32_t)i, is_start, err);
    }
    free(is_start);
    free(targets);
    if (ok) {
        idm_profile_count("bytecode.verify.functions", (uint64_t)module->function_count);
        idm_profile_count("bytecode.verify.code_words", (uint64_t)module->code_count);
        idm_profile_scope_end(&prof);
    }
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

static bool append_disasm_operand(IdmBuffer *buf, const IdmBytecodeModule *module, char role, uint32_t word) {
    switch (role) {
        case 'c':
            return idm_buf_appendf(buf, " const=%u", word);
        case 'T':
            return word == UINT32_MAX ? idm_buf_append(buf, " type=_") : idm_buf_appendf(buf, " type=%u", word);
        case 'f':
            return append_function_ref(buf, module, word);
        case 'j':
            return idm_buf_appendf(buf, " target=%u", word);
        case 'm':
            return idm_buf_appendf(buf, " entries=%u", word);
        case 't':
            return idm_buf_appendf(buf, " tail=%u", word);
        case 'p': {
            const char *name = idm_primitive_info((IdmPrimitive)word) ? idm_primitive_name((IdmPrimitive)word) : "?";
            return idm_buf_appendf(buf, " primitive=%s#%u", name, word);
        }
        case 'v':
        case 'y':
            return idm_buf_appendf(buf, " kind=%u", word);
        case 'r':
            return idm_buf_appendf(buf, " %u", word);
        default:
            return idm_buf_appendf(buf, " %u", word);
    }
}

static bool append_disasm_operands(IdmBuffer *buf, const IdmBytecodeModule *module, const IdmBcInstr *instr) {
    for (uint8_t i = 0; i < instr->info->fixed_operands; i++) {
        if (!append_disasm_operand(buf, module, opcode_role_at(instr->info->operand_roles, i), instr->operands[i])) return false;
    }
    for (uint32_t item = 0; item < instr->repeat_count; item++) {
        for (uint8_t part = 0; part < instr->info->repeat_width; part++) {
            size_t index = (size_t)item * (size_t)instr->info->repeat_width + (size_t)part;
            if (!append_disasm_operand(buf, module, opcode_role_at(instr->info->repeat_roles, part), instr->repeat_operands[index])) return false;
        }
    }
    return true;
}

static bool append_name_note(IdmBuffer *buf, const IdmBytecodeModule *module, size_t offset) {
    const char *name = name_note_at(module, offset);
    return name ? idm_buf_appendf(buf, " #+name: %s", name) : true;
}

bool idm_bc_disassemble(IdmBuffer *buf, const IdmBytecodeModule *module) {
    for (size_t i = 0; i < module->function_count; i++) {
        if (!idm_buf_appendf(buf, "fn %zu %s/", i, module->functions[i].name) ||
            !idm_arity_describe(buf, &module->functions[i].call_arity) ||
            !idm_buf_appendf(buf, " entry=%zu locals=%u registers=%u", module->functions[i].entry, module->functions[i].local_count, module->functions[i].register_count)) return false;
        if (module->functions[i].primitive_backed && !idm_buf_appendf(buf, " primitive=%u", module->functions[i].primitive)) return false;
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
        IdmBcInstr instr;
        IdmError decode_error;
        idm_error_init(&decode_error);
        bool ok = decode_instr(module, ip, &instr, &decode_error);
        idm_error_clear(&decode_error);
        if (!ok) {
            IdmOpcode op = (IdmOpcode)module->code[ip];
            return idm_buf_appendf(buf, "%04zu %-16s <missing>\n", offset, idm_opcode_name(op));
        }
        if (!idm_buf_appendf(buf, "%04zu %-16s", offset, instr.info->name)) return false;
        if (!append_disasm_operands(buf, module, &instr)) return false;
        if (!append_name_note(buf, module, offset)) return false;
        if (!idm_buf_append_char(buf, '\n')) return false;
        ip += instr.width;
    }
    return true;
}

static bool serialize_value(IdmBuffer *out, IdmValue v, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "value nested too deeply for .ic");
    switch (idm_value_tag(v)) {
        case IDM_VAL_NIL: return idm_buf_put_u8(out, 0u);
        case IDM_VAL_INT: return idm_buf_put_u8(out, 1u) && idm_buf_put_u64(out, (uint64_t)idm_int_value(v));
        case IDM_VAL_BIGNUM: {
            const uint32_t *limbs = NULL;
            size_t count = 0u;
            int sign = 0;
            if (!idm_bignum_view(v, &limbs, &count, &sign)) return idm_error_set(err, idm_span_unknown(NULL), "invalid bignum constant");
            if (count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "bignum constant is too large");
            uint8_t sign_tag = sign < 0 ? 1u : (sign > 0 ? 2u : 0u);
            if (!idm_buf_put_u8(out, 16u) || !idm_buf_put_u8(out, sign_tag) || !idm_buf_put_u32(out, (uint32_t)count)) {
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            for (size_t i = 0; i < count; i++) {
                if (!idm_buf_put_u32(out, limbs[i])) return idm_error_oom(err, idm_span_unknown(NULL));
            }
            return true;
        }
        case IDM_VAL_FLOAT: { uint64_t bits; double d = idm_float_value(v); memcpy(&bits, &d, 8u); return idm_buf_put_u8(out, 2u) && idm_buf_put_u64(out, bits); }
        case IDM_VAL_STRING: return idm_buf_put_u8(out, 3u) && idm_buf_put_str(out, idm_string_bytes(v), idm_string_length(v));
        case IDM_VAL_ATOM: { const char *s = idm_symbol_text(idm_value_symbol(v)); return idm_buf_put_u8(out, 4u) && idm_buf_put_str(out, s, strlen(s)); }
        case IDM_VAL_WORD: { const char *s = idm_symbol_text(idm_value_symbol(v)); return idm_buf_put_u8(out, 5u) && idm_buf_put_str(out, s, strlen(s)); }
        case IDM_VAL_PAIR: {
            IdmError ignore; idm_error_init(&ignore);
            IdmValue car = idm_car(v, &ignore), cdr = idm_cdr(v, &ignore);
            idm_error_clear(&ignore);
            return idm_buf_put_u8(out, 6u) && serialize_value(out, car, depth + 1u, err) && serialize_value(out, cdr, depth + 1u, err);
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR: {
            size_t n = idm_sequence_count(v);
            if (!idm_buf_put_u8(out, idm_value_tag(v) == IDM_VAL_TUPLE ? 7u : 8u) || !idm_buf_put_u32(out, (uint32_t)n)) return idm_error_oom(err, idm_span_unknown(NULL));
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
            IdmDictIter it;
            IdmValue key, val;
            idm_dict_iter_init(v, &it);
            while (idm_dict_iter_next(&it, &key, &val)) {
                if (!serialize_value(out, key, depth + 1u, err) || !serialize_value(out, val, depth + 1u, err)) return false;
            }
            (void)n;
            return true;
        }
        case IDM_VAL_RECORD: {
            IdmError ignore;
            idm_error_init(&ignore);
            IdmSymbol *type_symbol = idm_record_type_symbol(v, &ignore);
            const char *type = type_symbol ? idm_symbol_text(type_symbol) : NULL;
            size_t n = idm_record_field_count(v, &ignore);
            idm_error_clear(&ignore);
            if (!idm_buf_put_u8(out, 11u) ||
                !idm_buf_put_str(out, type ? type : "", type ? strlen(type) : 0u) ||
                !idm_buf_put_u32(out, (uint32_t)n)) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < n; i++) {
                IdmError field_err;
                idm_error_init(&field_err);
                IdmSymbol *name_symbol = idm_record_field_name_symbol(v, i, &field_err);
                const char *name = name_symbol ? idm_symbol_text(name_symbol) : NULL;
                IdmValue value = idm_record_field_value(v, i, &field_err);
                bool ok = !(field_err.present);
                idm_error_clear(&field_err);
                if (!ok) return idm_error_set(err, idm_span_unknown(NULL), "record field serialization failed");
                if (!idm_buf_put_str(out, name ? name : "", name ? strlen(name) : 0u) ||
                    !serialize_value(out, value, depth + 1u, err)) return false;
            }
            return true;
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

static bool serialize_syntax_pattern(IdmBuffer *out, const IdmSyntaxPattern *pat, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "syntax pattern nested too deeply for .ic");
    if (!idm_buf_put_u8(out, (uint8_t)pat->kind)) return idm_error_oom(err, idm_span_unknown(NULL));
    switch (pat->kind) {
        case IDM_SYN_PAT_WILDCARD:
            return true;
        case IDM_SYN_PAT_BIND:
            return idm_buf_put_str(out, pat->as.bind.name, strlen(pat->as.bind.name)) ? true : idm_error_oom(err, idm_span_unknown(NULL));
        case IDM_SYN_PAT_LITERAL:
            return idm_syn_serialize(out, pat->as.literal, err);
        case IDM_SYN_PAT_SEQUENCE: {
            bool has_rest = pat->as.seq.rest_index != IDM_SYN_PAT_NO_REST;
            if (!idm_buf_put_u8(out, (uint8_t)pat->as.seq.kind) ||
                !idm_buf_put_u32(out, (uint32_t)pat->as.seq.count) ||
                !idm_buf_put_u8(out, has_rest ? 1u : 0u)) {
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            if (has_rest) {
                if (!idm_buf_put_u32(out, (uint32_t)pat->as.seq.rest_index) ||
                    !idm_buf_put_opt_str(out, pat->as.seq.rest_name)) {
                    return idm_error_oom(err, idm_span_unknown(NULL));
                }
            }
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (!serialize_syntax_pattern(out, pat->as.seq.items[i], depth + 1u, err)) return false;
            }
            return true;
        }
    }
    return idm_error_set(err, idm_span_unknown(NULL), "syntax pattern kind cannot be serialized to .ic");
}

static bool serialize_pattern(IdmBuffer *out, const IdmPattern *pat, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "pattern nested too deeply for .ic");
    if (!idm_buf_put_u8(out, (uint8_t)pat->kind)) return idm_error_oom(err, idm_span_unknown(NULL));
    switch (pat->kind) {
        case IDM_PAT_WILDCARD: return true;
        case IDM_PAT_BIND:
        case IDM_PAT_PIN:
        case IDM_PAT_TYPE: return idm_buf_put_str(out, pat->as.name, strlen(pat->as.name)) ? true : idm_error_oom(err, idm_span_unknown(NULL));
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
        case IDM_PAT_SYNTAX:
            return serialize_syntax_pattern(out, pat->as.syntax, depth + 1u, err);
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "pattern kind cannot be serialized to .ic");
    }
}

static bool serialize_code(IdmBuffer *out, const IdmBytecodeModule *module, IdmError *err) {
    if (!idm_buf_put_u64(out, (uint64_t)module->code_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t ip = 0;
    while (ip < module->code_count) {
        size_t width = 0;
        if (!idm_bc_instruction_width(module, ip, &width, err)) return false;
        for (size_t i = 0; i < width; i++) {
            if (!idm_buf_put_u32(out, module->code[ip + i])) return idm_error_oom(err, idm_span_unknown(NULL));
        }
        ip += width;
    }
    return true;
}

bool idm_value_serialize(IdmBuffer *out, IdmValue value, IdmError *err) {
    return serialize_value(out, value, 0u, err);
}

bool idm_ic_serialize(const IdmBytecodeModule *module, IdmBuffer *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "bytecode.serialize");
    if (!idm_buf_append_n(out, "IDMC", 4u) || !idm_buf_put_u32(out, IDM_IC_VERSION)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!idm_buf_put_u32(out, (uint32_t)module->const_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->const_count; i++) if (!serialize_value(out, module->constants[i], 0u, err)) return false;
    if (!idm_buf_put_u32(out, (uint32_t)module->type_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->type_count; i++) if (!idm_type_term_serialize(out, &module->types[i], err)) return false;
    if (!idm_buf_put_u32(out, (uint32_t)module->function_count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < module->function_count; i++) {
        const IdmBcFunction *f = &module->functions[i];
        if (!idm_buf_put_str(out, f->name ? f->name : "", f->name ? strlen(f->name) : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u32(out, f->arity) ||
            !idm_buf_put_u32(out, f->local_count) ||
            !idm_buf_put_u32(out, f->register_count) ||
            !idm_buf_put_u64(out, (uint64_t)f->entry)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u8(out, f->has_guard ? 1u : 0u) || !idm_buf_put_u32(out, f->guard_function)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_arity_serialize(out, f->call_arity, err)) return false;
        if (!idm_buf_put_u8(out, f->primitive_backed ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (f->primitive_backed) {
            if (!put_primitive_ref(out, f->primitive, err)) return false;
        } else if (!idm_buf_put_str(out, "", 0u) || !idm_buf_put_str(out, "", 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!idm_buf_put_u32(out, f->pattern_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_count; p++) if (!serialize_pattern(out, f->param_patterns[p], 0u, err)) return false;
        if (!idm_buf_put_u32(out, f->pattern_local_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_local_count; p++) {
            if (!idm_buf_put_str(out, f->pattern_locals[p].name, strlen(f->pattern_locals[p].name)) || !idm_buf_put_u32(out, f->pattern_locals[p].slot)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    if (!serialize_code(out, module, err)) return false;
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
    idm_profile_count("bytecode.serialize.constants", (uint64_t)module->const_count);
    idm_profile_count("bytecode.serialize.types", (uint64_t)module->type_count);
    idm_profile_count("bytecode.serialize.functions", (uint64_t)module->function_count);
    idm_profile_count("bytecode.serialize.code_words", (uint64_t)module->code_count);
    idm_profile_scope_end(&prof);
    return true;
}

static bool deserialize_value(IdmRuntime *rt, IdmByteReader *r, IdmValue *out, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), ".ic value nested too deeply");
    uint8_t tag = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic value");
    switch (tag) {
        case 0u: *out = idm_nil(); return true;
        case 1u: { uint64_t bits = idm_rd_u64(r); if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated int"); *out = idm_int((int64_t)bits); return true; }
        case 16u: {
            uint8_t sign_tag = idm_rd_u8(r);
            uint32_t count = idm_rd_u32(r);
            if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated bignum");
            if (sign_tag > 2u) return idm_error_set(err, idm_span_unknown(NULL), "invalid bignum sign");
            int sign = sign_tag == 1u ? -1 : (sign_tag == 2u ? 1 : 0);
            if ((count == 0u) != (sign == 0)) return idm_error_set(err, idm_span_unknown(NULL), "invalid bignum zero encoding");
            uint32_t *limbs = NULL;
            if (count != 0u) {
                limbs = malloc((size_t)count * sizeof(*limbs));
                if (!limbs) return idm_error_oom(err, idm_span_unknown(NULL));
            }
            for (uint32_t i = 0; i < count; i++) limbs[i] = idm_rd_u32(r);
            if (!r->ok) {
                free(limbs);
                return idm_error_set(err, idm_span_unknown(NULL), "truncated bignum");
            }
            *out = idm_int_from_limbs(rt, limbs, count, sign, err);
            free(limbs);
            return !(err && err->present);
        }
        case 2u: { uint64_t bits = idm_rd_u64(r); if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated float"); double d; memcpy(&d, &bits, 8u); *out = idm_float(rt, d, err); return !(err && err->present); }
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
        case 11u: {
            char *type = idm_rd_string(r, NULL);
            if (!type) return idm_error_set(err, idm_span_unknown(NULL), "truncated record type");
            if (!*type) {
                free(type);
                return idm_error_set(err, idm_span_unknown(NULL), "record type must be a non-empty name");
            }
            IdmSymbol *type_symbol = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, type);
            uint32_t n = idm_rd_u32(r);
            if (!type_symbol || !r->ok) {
                free(type);
                return type_symbol ? idm_error_set(err, idm_span_unknown(NULL), "truncated record fields") : idm_error_oom(err, idm_span_unknown(NULL));
            }
            IdmSymbol **field_names = n == 0 ? NULL : calloc(n, sizeof(*field_names));
            IdmValue *field_values = n == 0 ? NULL : calloc(n, sizeof(*field_values));
            if ((n != 0 && !field_names) || (n != 0 && !field_values)) {
                free(field_names);
                free(field_values);
                free(type);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            bool ok = true;
            for (uint32_t i = 0; ok && i < n; i++) {
                char *field_name = idm_rd_string(r, NULL);
                if (!field_name) {
                    idm_error_set(err, idm_span_unknown(NULL), "truncated record field name");
                    ok = false;
                } else if (!*field_name) {
                    idm_error_set(err, idm_span_unknown(NULL), "record field must be a non-empty name");
                    ok = false;
                } else if (!deserialize_value(rt, r, &field_values[i], depth + 1u, err)) {
                    ok = false;
                } else {
                    field_names[i] = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, field_name);
                    if (!field_names[i]) ok = idm_error_oom(err, idm_span_unknown(NULL));
                }
                free(field_name);
            }
            if (ok) {
                IdmRecordShape *shape = idm_record_shape_intern_symbols(rt, type_symbol, field_names, n, err);
                if (shape) {
                    *out = idm_record_from_shape(rt, shape, field_values, err);
                    ok = !(err && err->present);
                } else {
                    ok = false;
                }
            }
            free(field_names);
            free(field_values);
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

static IdmSyntaxPattern *deserialize_syntax_pattern(IdmRuntime *rt, IdmByteReader *r, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) {
        idm_error_set(err, idm_span_unknown(NULL), ".ic syntax pattern nested too deeply");
        return NULL;
    }
    uint8_t kind = idm_rd_u8(r);
    if (!r->ok) {
        idm_error_set(err, idm_span_unknown(NULL), "truncated .ic syntax pattern");
        return NULL;
    }
    IdmSpan span = idm_span_unknown(NULL);
    switch (kind) {
        case IDM_SYN_PAT_WILDCARD:
            return idm_syn_pat_wildcard(span);
        case IDM_SYN_PAT_BIND: {
            char *name = idm_rd_string(r, NULL);
            if (!name) { idm_error_set(err, span, "truncated syntax pattern name"); return NULL; }
            IdmSyntaxPattern *pat = idm_syn_pat_bind(name, span);
            free(name);
            if (!pat) idm_error_oom(err, span);
            return pat;
        }
        case IDM_SYN_PAT_LITERAL: {
            IdmSyntax *literal = idm_syn_deserialize(rt, r, err);
            if (!literal) return NULL;
            IdmSyntaxPattern *pat = idm_syn_pat_literal_take(literal, span);
            if (!pat) { idm_syn_free(literal); idm_error_oom(err, span); }
            return pat;
        }
        case IDM_SYN_PAT_SEQUENCE: {
            uint8_t seq_kind = idm_rd_u8(r);
            uint32_t count = idm_rd_u32(r);
            uint8_t has_rest = idm_rd_u8(r);
            if (!r->ok || has_rest > 1u || seq_kind > (uint8_t)IDM_SYN_DICT) {
                idm_error_set(err, span, "truncated or invalid syntax sequence pattern");
                return NULL;
            }
            size_t rest_index = IDM_SYN_PAT_NO_REST;
            char *rest_name = NULL;
            if (has_rest) {
                rest_index = idm_rd_u32(r);
                if (!idm_rd_opt_str(r, &rest_name, err)) return NULL;
                if (rest_index > count) {
                    free(rest_name);
                    idm_error_set(err, span, "invalid syntax rest pattern index");
                    return NULL;
                }
            }
            IdmSyntaxPattern **items = count == 0 ? NULL : calloc(count, sizeof(*items));
            if (count != 0 && !items) {
                free(rest_name);
                idm_error_oom(err, span);
                return NULL;
            }
            for (uint32_t i = 0; i < count; i++) {
                items[i] = deserialize_syntax_pattern(rt, r, depth + 1u, err);
                if (!items[i]) {
                    for (uint32_t j = 0; j < i; j++) idm_syn_pat_free(items[j]);
                    free(items);
                    free(rest_name);
                    return NULL;
                }
            }
            IdmSyntaxPattern *pat = idm_syn_pat_sequence((IdmSyntaxKind)seq_kind, items, count, rest_index, rest_name, span);
            free(rest_name);
            if (!pat) {
                for (uint32_t i = 0; i < count; i++) idm_syn_pat_free(items[i]);
                free(items);
                idm_error_oom(err, span);
            }
            return pat;
        }
        default:
            idm_error_set(err, span, "unknown .ic syntax pattern kind %u", kind);
            return NULL;
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
        case IDM_PAT_TYPE: { char *s = idm_rd_string(r, NULL); if (!s) { idm_error_set(err, span, "truncated pattern type"); return NULL; } IdmPattern *p = idm_pat_type(s, span); free(s); return p; }
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
        case IDM_PAT_SYNTAX: {
            IdmSyntaxPattern *syntax = deserialize_syntax_pattern(rt, r, depth + 1u, err);
            if (!syntax) return NULL;
            IdmPattern *p = idm_pat_syntax_take(syntax, span);
            if (!p) { idm_syn_pat_free(syntax); idm_error_oom(err, span); }
            return p;
        }
        default:
            idm_error_set(err, span, "unknown .ic pattern kind %u", kind);
            return NULL;
    }
}

static bool deserialize_code_word(IdmBytecodeModule *module, IdmByteReader *r, IdmError *err) {
    uint32_t word = idm_rd_u32(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic code");
    if (!idm_bc_emit(module, word, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool deserialize_code(IdmBytecodeModule *module, IdmByteReader *r, uint64_t code_count, IdmError *err) {
    if (code_count > (uint64_t)SIZE_MAX) return idm_error_set(err, idm_span_unknown(NULL), ".ic code is too large");
    while (module->code_count < code_count && r->ok) {
        size_t start = module->code_count;
        uint32_t raw_op = idm_rd_u32(r);
        if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic opcode");
        IdmOpcode op = (IdmOpcode)raw_op;
        const IdmOpcodeInfo *info = idm_opcode_info(op);
        if (!info) return idm_error_set(err, idm_span_unknown(NULL), "invalid .ic opcode %u", raw_op);
        if (!idm_bc_emit(module, raw_op, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
        if ((uint64_t)module->code_count + info->fixed_operands > code_count) {
            return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic %s", info->name);
        }
        for (uint8_t i = 0; i < info->fixed_operands; i++) {
            if (!deserialize_code_word(module, r, err)) return false;
        }
        if (info->count_operand != IDM_OPCODE_NO_COUNT) {
            if (info->count_operand >= info->fixed_operands || info->repeat_width == 0) {
                return idm_error_set(err, idm_span_unknown(NULL), "%s has invalid opcode metadata", info->name);
            }
            uint32_t count = module->code[start + 1u + (size_t)info->count_operand];
            uint64_t extra = (uint64_t)count * (uint64_t)info->repeat_width;
            if ((uint64_t)module->code_count + extra > code_count) {
                return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic %s payload", info->name);
            }
            for (uint64_t i = 0; i < extra; i++) {
                if (!deserialize_code_word(module, r, err)) return false;
            }
        }
    }
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic code");
    if (module->code_count != code_count) return idm_error_set(err, idm_span_unknown(NULL), "corrupt .ic code length");
    return true;
}

bool idm_value_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmValue *out, IdmError *err) {
    return deserialize_value(rt, r, out, 0u, err);
}

bool idm_ic_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmBytecodeModule *module, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "bytecode.deserialize");
    idm_bc_init(module);
    IdmByteReader r = { data, len, 0u, true };
    if (len < 8u || memcmp(data, "IDMC", 4u) != 0) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "not an .ic module"); }
    r.pos = 4u;
    uint32_t version = idm_rd_u32(&r);
    if (version < IDM_IC_MIN_READ_VERSION || version > IDM_IC_VERSION) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), ".ic version %u unsupported", version); }
    uint32_t const_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < const_count && r.ok; i++) {
        IdmValue v;
        if (!deserialize_value(rt, &r, &v, 0u, err)) { idm_bc_destroy(module); return false; }
        if (!idm_bc_add_const(module, v, NULL)) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    uint32_t type_count = idm_rd_u32(&r);
    for (uint32_t i = 0; i < type_count && r.ok; i++) {
        IdmTypeTerm term;
        if (!idm_type_term_deserialize(&r, &term, err)) { idm_bc_destroy(module); return false; }
        bool added = idm_bc_add_type_term(module, &term, NULL);
        idm_type_term_destroy(&term);
        if (!added) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
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
        uint32_t register_count = idm_rd_u32(&r);
        uint64_t entry = idm_rd_u64(&r);
        uint8_t has_guard = idm_rd_u8(&r);
        uint32_t guard_function = idm_rd_u32(&r);
        IdmArity call_arity = idm_arity_unknown();
        if (!idm_arity_deserialize(&r, &call_arity, err)) {
            free(name);
            free(deferred_guards);
            idm_bc_destroy(module);
            return false;
        }
        uint8_t primitive_backed = idm_rd_u8(&r);
        IdmPrimitive primitive = 0;
        if (primitive_backed) {
            if (!read_primitive_ref(&r, &primitive, err)) {
                free(name);
                free(deferred_guards);
                idm_bc_destroy(module);
                return false;
            }
        } else {
            char *primitive_home = idm_rd_string(&r, NULL);
            char *primitive_name = idm_rd_string(&r, NULL);
            if (!primitive_home || !primitive_name) {
                free(primitive_home);
                free(primitive_name);
                free(name);
                free(deferred_guards);
                idm_bc_destroy(module);
                return idm_error_set(err, idm_span_unknown(NULL), "truncated primitive function reference");
            }
            free(primitive_home);
            free(primitive_name);
        }
        uint32_t idx = 0;
        bool added = primitive_backed
            ? idm_bc_add_primitive_function(module, name, call_arity, (uint32_t)primitive, &idx)
            : idm_bc_add_function(module, name, arity, local_count, (size_t)entry, &idx);
        if (!r.ok || !added) { free(name); free(deferred_guards); idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "corrupt .ic function"); }
        module->functions[idx].call_arity = call_arity;
        module->functions[idx].register_count = register_count;
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
    if (!deserialize_code(module, &r, code_count, err)) { idm_bc_destroy(module); return false; }
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
            if (!idm_grow((void **)&module->spans, &module->span_cap, sizeof(*module->spans), 32u, module->span_count + 1u)) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
        }
        module->spans[module->span_count].offset = offset;
        module->spans[module->span_count].file = file;
        module->spans[module->span_count].line = line;
        module->spans[module->span_count].column = column;
        module->span_count++;
    }
    uint32_t name_note_count = idm_rd_u32(&r);
    if (name_note_count != 0) {
        module->name_notes = calloc(name_note_count, sizeof(*module->name_notes));
        if (!module->name_notes) { idm_bc_destroy(module); return idm_error_oom(err, idm_span_unknown(NULL)); }
        module->name_note_cap = name_note_count;
    }
    uint32_t prev_offset = 0;
    for (uint32_t i = 0; i < name_note_count && r.ok; i++) {
        uint32_t offset = idm_rd_u32(&r);
        char *name = idm_rd_string(&r, NULL);
        if (!name || !r.ok || offset >= module->code_count || (i != 0 && offset < prev_offset)) {
            free(name);
            idm_bc_destroy(module);
            return idm_error_set(err, idm_span_unknown(NULL), "corrupt bytecode name note");
        }
        module->name_notes[module->name_note_count].offset = offset;
        module->name_notes[module->name_note_count].name = name;
        module->name_note_count++;
        prev_offset = offset;
    }
    if (!r.ok) { idm_bc_destroy(module); return idm_error_set(err, idm_span_unknown(NULL), "truncated .ic module"); }
    if (!idm_bc_intern_literals(rt, module, err)) { idm_bc_destroy(module); return false; }
    idm_profile_count("bytecode.deserialize.bytes", (uint64_t)len);
    idm_profile_count("bytecode.deserialize.types", (uint64_t)module->type_count);
    idm_profile_count("bytecode.deserialize.functions", (uint64_t)module->function_count);
    idm_profile_count("bytecode.deserialize.code_words", (uint64_t)module->code_count);
    idm_profile_scope_end(&prof);
    return true;
}

static bool reloc_add(uint32_t value, uint32_t delta, const char *what, uint32_t *out, IdmError *err) {
    if (value > UINT32_MAX - delta) return idm_error_set(err, idm_span_unknown(NULL), "%s relocation overflow", what);
    *out = value + delta;
    return true;
}

static bool reloc_word(uint32_t word, char role, uint32_t const_off, uint32_t type_off, uint32_t fn_off, uint32_t code_off, uint32_t *out, IdmError *err) {
    switch (role) {
        case 'r':
        case 'm':
        case 't':
        case 'p':
        case 'v':
        case 'y':
            *out = word;
            return true;
        case 'c':
            return reloc_add(word, const_off, "constant", out, err);
        case 'T':
            if (word == UINT32_MAX) {
                *out = word;
                return true;
            }
            return reloc_add(word, type_off, "type", out, err);
        case 'f':
            return reloc_add(word, fn_off, "function", out, err);
        case 'j':
            return reloc_add(word, code_off, "code target", out, err);
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "invalid operand relocation role '%c'", role);
    }
}

static bool reloc_emit_word(IdmBytecodeModule *dst, uint32_t word, char role, uint32_t const_off, uint32_t type_off, uint32_t fn_off, uint32_t code_off, IdmError *err) {
    uint32_t relocated = 0;
    if (!reloc_word(word, role, const_off, type_off, fn_off, code_off, &relocated, err)) return false;
    if (!idm_bc_emit(dst, relocated, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool reloc_emit(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t const_off, uint32_t type_off, uint32_t fn_off, uint32_t code_off, IdmError *err) {
    size_t ip = 0;
    while (ip < src->code_count) {
        size_t offset = ip;
        size_t width = 0;
        if (!idm_bc_instruction_width(src, offset, &width, err)) return false;
        IdmOpcode op = (IdmOpcode)src->code[ip++];
        const IdmOpcodeInfo *info = idm_opcode_info(op);
        if (!info) return idm_error_set(err, idm_span_unknown(NULL), "relocation: invalid opcode %u", (unsigned)op);
        if (!idm_bc_emit(dst, (uint32_t)op, NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
        uint32_t repeat_count = 0;
        for (uint8_t i = 0; i < info->fixed_operands; i++) {
            uint32_t word = src->code[ip++];
            if (info->count_operand == i) repeat_count = word;
            if (!reloc_emit_word(dst, word, opcode_role_at(info->operand_roles, i), const_off, type_off, fn_off, code_off, err)) return false;
        }
        if (info->count_operand != IDM_OPCODE_NO_COUNT) {
            for (uint32_t item = 0; item < repeat_count; item++) {
                for (uint8_t part = 0; part < info->repeat_width; part++) {
                    if (!reloc_emit_word(dst, src->code[ip++], opcode_role_at(info->repeat_roles, part), const_off, type_off, fn_off, code_off, err)) return false;
                }
            }
        }
        if (ip != offset + width) return idm_error_set(err, idm_span_unknown(NULL), "%s relocation width mismatch", info->name);
    }
    return true;
}

static bool link_spans(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t code_off, IdmError *err) {
    for (size_t i = 0; i < src->span_count; i++) {
        uint32_t file_index = 0;
        if (!span_file_index(dst, src->span_files[src->spans[i].file], &file_index)) return idm_error_oom(err, idm_span_unknown(NULL));
        if (dst->span_count == dst->span_cap) {
            if (!idm_grow((void **)&dst->spans, &dst->span_cap, sizeof(*dst->spans), 32u, dst->span_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
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
    if (src->name_note_count == 0) return true;
    size_t needed = dst->name_note_count + src->name_note_count;
    if (needed > dst->name_note_cap) {
        if (!idm_grow((void **)&dst->name_notes, &dst->name_note_cap, sizeof(*dst->name_notes), 32u, needed)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < src->name_note_count; i++) {
        if (src->name_notes[i].offset > UINT32_MAX - code_off) return idm_error_set(err, idm_span_unknown(NULL), "linked name note offset overflow");
        char *name = idm_strdup(src->name_notes[i].name);
        if (!name) return idm_error_oom(err, idm_span_unknown(NULL));
        dst->name_notes[dst->name_note_count].offset = src->name_notes[i].offset + code_off;
        dst->name_notes[dst->name_note_count].name = name;
        dst->name_note_count++;
    }
    return true;
}

bool idm_bc_link(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t *out_const_offset, uint32_t *out_fn_offset, uint32_t *out_code_offset, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "bytecode.link");
    uint32_t const_off = (uint32_t)dst->const_count;
    uint32_t type_off = (uint32_t)dst->type_count;
    uint32_t fn_off = (uint32_t)dst->function_count;
    uint32_t code_off = (uint32_t)dst->code_count;
    IdmProfileScope spans_prof;
    idm_profile_scope_begin(&spans_prof, "bytecode.link.spans");
    if (!link_spans(dst, src, code_off, err)) return false;
    idm_profile_scope_end(&spans_prof);
    IdmProfileScope consts_prof;
    idm_profile_scope_begin(&consts_prof, "bytecode.link.constants_phase");
    for (size_t i = 0; i < src->const_count; i++) if (!idm_bc_add_const(dst, src->constants[i], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_profile_scope_end(&consts_prof);
    IdmProfileScope types_prof;
    idm_profile_scope_begin(&types_prof, "bytecode.link.types_phase");
    for (size_t i = 0; i < src->type_count; i++) if (!idm_bc_add_type_term(dst, &src->types[i], NULL)) return idm_error_oom(err, idm_span_unknown(NULL));
    idm_profile_scope_end(&types_prof);
    IdmProfileScope functions_prof;
    idm_profile_scope_begin(&functions_prof, "bytecode.link.functions_phase");
    for (size_t i = 0; i < src->function_count; i++) {
        const IdmBcFunction *sf = &src->functions[i];
        uint32_t nf = 0;
        bool added = sf->primitive_backed
            ? idm_bc_add_primitive_function(dst, sf->name, sf->call_arity, sf->primitive, &nf)
            : idm_bc_add_function(dst, sf->name, sf->arity, sf->local_count, sf->entry + code_off, &nf);
        if (!added) return idm_error_oom(err, idm_span_unknown(NULL));
        dst->functions[nf].call_arity = sf->call_arity;
        dst->functions[nf].register_count = sf->register_count;
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
    idm_profile_scope_end(&functions_prof);
    IdmProfileScope guards_prof;
    idm_profile_scope_begin(&guards_prof, "bytecode.link.guards");
    for (size_t i = 0; i < src->function_count; i++) {
        const IdmBcFunction *sf = &src->functions[i];
        if (sf->has_guard && !idm_bc_set_function_guard(dst, fn_off + (uint32_t)i, sf->guard_function + fn_off)) {
            return idm_error_set(err, idm_span_unknown(NULL), "linked guard function index is out of bounds");
        }
    }
    idm_profile_scope_end(&guards_prof);
    IdmProfileScope reloc_prof;
    idm_profile_scope_begin(&reloc_prof, "bytecode.link.reloc_emit");
    if (!reloc_emit(dst, src, const_off, type_off, fn_off, code_off, err)) return false;
    idm_profile_scope_end(&reloc_prof);
    IdmProfileScope names_prof;
    idm_profile_scope_begin(&names_prof, "bytecode.link.name_notes");
    if (!link_name_notes(dst, src, code_off, err)) return false;
    idm_profile_scope_end(&names_prof);
    if (out_const_offset) *out_const_offset = const_off;
    if (out_fn_offset) *out_fn_offset = fn_off;
    if (out_code_offset) *out_code_offset = code_off;
    idm_profile_count("bytecode.link.constants", (uint64_t)src->const_count);
    idm_profile_count("bytecode.link.types", (uint64_t)src->type_count);
    idm_profile_count("bytecode.link.functions", (uint64_t)src->function_count);
    idm_profile_count("bytecode.link.code_words", (uint64_t)src->code_count);
    idm_profile_scope_end(&prof);
    return true;
}

static bool value_relocate_syntax(IdmRuntime *rt, IdmValue v, IdmScopeId min_id, int64_t delta, IdmValue *out, bool *changed, IdmError *err) {
    *out = v;
    switch (idm_value_tag(v)) {
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
                *out = idm_value_tag(v) == IDM_VAL_TUPLE ? idm_tuple(rt, items, n, err) : idm_vector(rt, items, n, err);
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
            IdmDictIter it;
            IdmValue key, val;
            idm_dict_iter_init(v, &it);
            for (size_t i = 0; i < n && idm_dict_iter_next(&it, &key, &val); i++) {
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
        case IDM_PAT_SYNTAX: {
            IdmSyntaxPattern *sp = pat->as.syntax;
            if (!sp) return true;
            switch (sp->kind) {
                case IDM_SYN_PAT_LITERAL:
                    idm_syn_scope_relocate_tree(sp->as.literal, min_id, delta);
                    return true;
                case IDM_SYN_PAT_SEQUENCE:
                    for (size_t i = 0; i < sp->as.seq.count; i++) {
                        IdmPattern wrapper;
                        memset(&wrapper, 0, sizeof(wrapper));
                        wrapper.kind = IDM_PAT_SYNTAX;
                        wrapper.as.syntax = sp->as.seq.items[i];
                        if (!pattern_relocate_syntax(rt, &wrapper, min_id, delta, err)) return false;
                    }
                    return true;
                case IDM_SYN_PAT_WILDCARD:
                case IDM_SYN_PAT_BIND:
                    return true;
            }
            return true;
        }
        default:
            return true;
    }
}

bool idm_bc_relocate_syntax_scopes(IdmRuntime *rt, IdmBytecodeModule *module, IdmScopeId min_id, int64_t delta, IdmError *err) {
    if (delta == 0) return true;
    selectors_clear(module);
    module->literals_interned = false;
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
