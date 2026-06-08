#include "ish/bytecode.h"

#include <stdlib.h>
#include <string.h>

void ish_bc_init(IshBytecodeModule *module) {
    module->code = NULL;
    module->code_count = 0;
    module->code_cap = 0;
    module->constants = NULL;
    module->const_count = 0;
    module->const_cap = 0;
    module->functions = NULL;
    module->function_count = 0;
    module->function_cap = 0;
}

void ish_bc_destroy(IshBytecodeModule *module) {
    if (!module) return;
    free(module->code);
    free(module->constants);
    for (size_t i = 0; i < module->function_count; i++) {
        free(module->functions[i].name);
        for (uint32_t p = 0; p < module->functions[i].pattern_count; p++) ish_pat_free(module->functions[i].param_patterns[p]);
        free(module->functions[i].param_patterns);
        for (uint32_t p = 0; p < module->functions[i].pattern_local_count; p++) free(module->functions[i].pattern_locals[p].name);
        free(module->functions[i].pattern_locals);
    }
    free(module->functions);
    ish_bc_init(module);
}

bool ish_bc_add_const(IshBytecodeModule *module, IshValue value, uint32_t *out_index) {
    if (module->const_count == module->const_cap) {
        size_t cap = module->const_cap ? module->const_cap * 2u : 16u;
        IshValue *next = realloc(module->constants, cap * sizeof(*next));
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

bool ish_bc_add_function(IshBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index) {
    if (module->function_count == module->function_cap) {
        size_t cap = module->function_cap ? module->function_cap * 2u : 8u;
        IshBcFunction *next = realloc(module->functions, cap * sizeof(*next));
        if (!next) return false;
        module->functions = next;
        module->function_cap = cap;
    }
    if (module->function_count > UINT32_MAX) return false;
    uint32_t index = (uint32_t)module->function_count;
    module->functions[index].name = ish_strdup(name ? name : "<anonymous>");
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
    module->function_count++;
    if (out_index) *out_index = index;
    return true;
}

bool ish_bc_set_function_entry(IshBytecodeModule *module, uint32_t function_index, size_t entry) {
    if (function_index >= module->function_count) return false;
    module->functions[function_index].entry = entry;
    return true;
}

bool ish_bc_set_function_patterns_take(IshBytecodeModule *module, uint32_t function_index, IshPattern **patterns, uint32_t pattern_count) {
    if (function_index >= module->function_count) return false;
    IshBcFunction *fn = &module->functions[function_index];
    for (uint32_t i = 0; i < fn->pattern_count; i++) ish_pat_free(fn->param_patterns[i]);
    free(fn->param_patterns);
    fn->param_patterns = patterns;
    fn->pattern_count = pattern_count;
    return true;
}

bool ish_bc_set_function_pattern_locals_take(IshBytecodeModule *module, uint32_t function_index, IshPatternLocal *locals, uint32_t local_count) {
    if (function_index >= module->function_count) return false;
    IshBcFunction *fn = &module->functions[function_index];
    for (uint32_t i = 0; i < fn->pattern_local_count; i++) free(fn->pattern_locals[i].name);
    free(fn->pattern_locals);
    fn->pattern_locals = locals;
    fn->pattern_local_count = local_count;
    return true;
}

bool ish_bc_set_function_guard(IshBytecodeModule *module, uint32_t function_index, uint32_t guard_function) {
    if (function_index >= module->function_count || guard_function >= module->function_count) return false;
    module->functions[function_index].has_guard = true;
    module->functions[function_index].guard_function = guard_function;
    return true;
}

bool ish_bc_emit(IshBytecodeModule *module, uint32_t word, size_t *out_offset) {
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

bool ish_bc_emit_op(IshBytecodeModule *module, IshOpcode op, size_t *out_offset) {
    return ish_bc_emit(module, (uint32_t)op, out_offset);
}

bool ish_bc_emit_u32(IshBytecodeModule *module, IshOpcode op, uint32_t operand, size_t *out_offset) {
    size_t op_offset = 0;
    if (!ish_bc_emit(module, (uint32_t)op, &op_offset)) return false;
    if (!ish_bc_emit(module, operand, NULL)) return false;
    if (out_offset) *out_offset = op_offset;
    return true;
}

bool ish_bc_patch_u32(IshBytecodeModule *module, size_t operand_offset, uint32_t operand) {
    if (operand_offset >= module->code_count) return false;
    module->code[operand_offset] = operand;
    return true;
}

const char *ish_opcode_name(IshOpcode op) {
    switch (op) {
        case ISH_OP_HALT: return "HALT";
        case ISH_OP_LOAD_CONST: return "LOAD_CONST";
        case ISH_OP_LOAD_ARG: return "LOAD_ARG";
        case ISH_OP_LOAD_LOCAL: return "LOAD_LOCAL";
        case ISH_OP_STORE_LOCAL: return "STORE_LOCAL";
        case ISH_OP_LOAD_CAPTURE: return "LOAD_CAPTURE";
        case ISH_OP_MAKE_CELL: return "MAKE_CELL";
        case ISH_OP_LOAD_CELL: return "LOAD_CELL";
        case ISH_OP_STORE_CELL: return "STORE_CELL";
        case ISH_OP_MAKE_CLOSURE: return "MAKE_CLOSURE";
        case ISH_OP_MAKE_CLOSURE_CAPTURES: return "MAKE_CLOSURE_CAPTURES";
        case ISH_OP_MAKE_MULTI_CLOSURE: return "MAKE_MULTI_CLOSURE";
        case ISH_OP_CALL: return "CALL";
        case ISH_OP_TAIL_CALL: return "TAIL_CALL";
        case ISH_OP_RETURN: return "RETURN";
        case ISH_OP_POP: return "POP";
        case ISH_OP_JUMP: return "JUMP";
        case ISH_OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case ISH_OP_ADD: return "ADD";
        case ISH_OP_SUB: return "SUB";
        case ISH_OP_MUL: return "MUL";
        case ISH_OP_EQ: return "EQ";
        case ISH_OP_LT: return "LT";
    }
    return "<bad-op>";
}

static bool verify_operand(const IshBytecodeModule *module, size_t *ip, IshOpcode op, uint32_t *operand, IshError *err) {
    if (*ip >= module->code_count) return ish_error_set(err, ish_span_unknown(NULL), "%s missing operand", ish_opcode_name(op));
    *operand = module->code[(*ip)++];
    return true;
}

bool ish_bc_verify(const IshBytecodeModule *module, IshError *err) {
    if (module->function_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "bytecode module has no functions");
    for (size_t i = 0; i < module->function_count; i++) {
        if (module->functions[i].entry >= module->code_count) return ish_error_set(err, ish_span_unknown(NULL), "function %zu entry is out of bounds", i);
        if (module->functions[i].has_guard && module->functions[i].guard_function >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "function %zu guard function is out of bounds", i);
        if (module->functions[i].has_guard && module->functions[module->functions[i].guard_function].arity != module->functions[i].arity) return ish_error_set(err, ish_span_unknown(NULL), "function %zu guard arity mismatch", i);
    }
    size_t ip = 0;
    while (ip < module->code_count) {
        IshOpcode op = (IshOpcode)module->code[ip++];
        uint32_t operand = 0;
        switch (op) {
            case ISH_OP_HALT:
            case ISH_OP_RETURN:
            case ISH_OP_POP:
            case ISH_OP_ADD:
            case ISH_OP_SUB:
            case ISH_OP_MUL:
            case ISH_OP_EQ:
            case ISH_OP_LT:
            case ISH_OP_MAKE_CELL:
            case ISH_OP_LOAD_CELL:
            case ISH_OP_STORE_CELL:
                break;
            case ISH_OP_LOAD_CONST:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "LOAD_CONST index %u out of bounds", operand);
                break;
            case ISH_OP_MAKE_CLOSURE:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_CLOSURE function %u out of bounds", operand);
                break;
            case ISH_OP_LOAD_ARG:
            case ISH_OP_LOAD_LOCAL:
            case ISH_OP_STORE_LOCAL:
            case ISH_OP_LOAD_CAPTURE:
            case ISH_OP_CALL:
            case ISH_OP_TAIL_CALL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case ISH_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = 0;
                uint32_t capture_count = 0;
                if (!verify_operand(module, &ip, op, &entry_count, err)) return false;
                if (entry_count == 0) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_MULTI_CLOSURE requires at least one entry");
                if (!verify_operand(module, &ip, op, &capture_count, err)) return false;
                (void)capture_count;
                for (uint32_t i = 0; i < entry_count; i++) {
                    uint32_t entry = 0;
                    if (!verify_operand(module, &ip, op, &entry, err)) return false;
                    if (entry >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_MULTI_CLOSURE function %u out of bounds", entry);
                }
                break;
            }
            case ISH_OP_MAKE_CLOSURE_CAPTURES:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->function_count) return ish_error_set(err, ish_span_unknown(NULL), "MAKE_CLOSURE_CAPTURES function %u out of bounds", operand);
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case ISH_OP_JUMP:
            case ISH_OP_JUMP_IF_FALSE:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->code_count) return ish_error_set(err, ish_span_unknown(NULL), "%s target %u out of bounds", ish_opcode_name(op), operand);
                break;
            default:
                return ish_error_set(err, ish_span_unknown(NULL), "invalid opcode %u at offset %zu", (unsigned)op, ip - 1u);
        }
    }
    return true;
}

bool ish_bc_disassemble(IshBuffer *buf, const IshBytecodeModule *module) {
    for (size_t i = 0; i < module->function_count; i++) {
        if (!ish_buf_appendf(buf, "fn %zu %s/%u entry=%zu locals=%u", i, module->functions[i].name, module->functions[i].arity, module->functions[i].entry, module->functions[i].local_count)) return false;
        if (module->functions[i].has_guard && !ish_buf_appendf(buf, " guard=%u", module->functions[i].guard_function)) return false;
        if (!ish_buf_append_char(buf, '\n')) return false;
    }
    size_t ip = 0;
    while (ip < module->code_count) {
        size_t offset = ip;
        IshOpcode op = (IshOpcode)module->code[ip++];
        if (!ish_buf_appendf(buf, "%04zu %-16s", offset, ish_opcode_name(op))) return false;
        switch (op) {
            case ISH_OP_LOAD_CONST:
            case ISH_OP_LOAD_ARG:
            case ISH_OP_LOAD_LOCAL:
            case ISH_OP_STORE_LOCAL:
            case ISH_OP_LOAD_CAPTURE:
            case ISH_OP_MAKE_CLOSURE:
            case ISH_OP_MAKE_CLOSURE_CAPTURES:
            case ISH_OP_MAKE_MULTI_CLOSURE:
            case ISH_OP_CALL:
            case ISH_OP_TAIL_CALL:
            case ISH_OP_JUMP:
            case ISH_OP_JUMP_IF_FALSE:
                if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                if (op == ISH_OP_MAKE_CLOSURE_CAPTURES) {
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                } else if (op == ISH_OP_MAKE_MULTI_CLOSURE) {
                    uint32_t entry_count = module->code[ip - 1u];
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    for (uint32_t i = 0; i < entry_count; i++) {
                        if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                        if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    }
                }
                break;
            default:
                break;
        }
        if (!ish_buf_append_char(buf, '\n')) return false;
    }
    return true;
}
