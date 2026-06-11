#include "ish/bytecode.h"

#include "ish/core.h"
#include "ish/syntax.h"

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
        case ISH_OP_PRIM_CALL: return "PRIM_CALL";
        case ISH_OP_SELF: return "SELF";
        case ISH_OP_SPAWN: return "SPAWN";
        case ISH_OP_SEND: return "SEND";
        case ISH_OP_EXIT: return "EXIT";
        case ISH_OP_LINK: return "LINK";
        case ISH_OP_UNLINK: return "UNLINK";
        case ISH_OP_MONITOR: return "MONITOR";
        case ISH_OP_DEMONITOR: return "DEMONITOR";
        case ISH_OP_TRAP_EXIT: return "TRAP_EXIT";
        case ISH_OP_RECV: return "RECV";
        case ISH_OP_EXEC: return "EXEC";
        case ISH_OP_RESCUE_PUSH: return "RESCUE_PUSH";
        case ISH_OP_RESCUE_POP: return "RESCUE_POP";
        case ISH_OP_RAISE: return "RAISE";
        case ISH_OP_LOAD_RAISED: return "LOAD_RAISED";
        case ISH_OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case ISH_OP_STORE_GLOBAL: return "STORE_GLOBAL";
        case ISH_OP_AWAIT: return "AWAIT";
        case ISH_OP_APPLY: return "APPLY";
        case ISH_OP_ENTER_NAMESPACE: return "ENTER_NAMESPACE";
        case ISH_OP_IMPORT_GLOBAL: return "IMPORT_GLOBAL";
        case ISH_OP_LEAVE_NAMESPACE: return "LEAVE_NAMESPACE";
        case ISH_OP_DEFINE_PROTOCOL: return "DEFINE_PROTOCOL";
        case ISH_OP_EXTEND_PROTOCOL: return "EXTEND_PROTOCOL";
        case ISH_OP_CALL_METHOD: return "CALL_METHOD";
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
            case ISH_OP_SELF:
            case ISH_OP_SPAWN:
            case ISH_OP_SEND:
            case ISH_OP_EXIT:
            case ISH_OP_LINK:
            case ISH_OP_UNLINK:
            case ISH_OP_MONITOR:
            case ISH_OP_DEMONITOR:
            case ISH_OP_TRAP_EXIT:
            case ISH_OP_EXEC:
            case ISH_OP_AWAIT:
            case ISH_OP_APPLY:
            case ISH_OP_LEAVE_NAMESPACE:
            case ISH_OP_RESCUE_POP:
            case ISH_OP_RAISE:
            case ISH_OP_LOAD_RAISED:
                break;
            case ISH_OP_DEFINE_PROTOCOL: {
                uint32_t protocol_const = 0;
                uint32_t method_count = 0;
                if (!verify_operand(module, &ip, op, &protocol_const, err)) return false;
                if (protocol_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "DEFINE_PROTOCOL protocol constant %u out of bounds", protocol_const);
                if (!verify_operand(module, &ip, op, &method_count, err)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = 0;
                    uint32_t ignored = 0;
                    if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                    if (method_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "DEFINE_PROTOCOL method constant %u out of bounds", method_const);
                    if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                    if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                    if (ignored > 1u) return ish_error_set(err, ish_span_unknown(NULL), "DEFINE_PROTOCOL default flag must be 0 or 1");
                }
                break;
            }
            case ISH_OP_EXTEND_PROTOCOL: {
                uint32_t protocol_const = 0;
                uint32_t type_const = 0;
                uint32_t method_count = 0;
                if (!verify_operand(module, &ip, op, &protocol_const, err)) return false;
                if (protocol_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "EXTEND_PROTOCOL protocol constant %u out of bounds", protocol_const);
                if (!verify_operand(module, &ip, op, &type_const, err)) return false;
                if (type_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "EXTEND_PROTOCOL type constant %u out of bounds", type_const);
                if (!verify_operand(module, &ip, op, &method_count, err)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = 0;
                    uint32_t ignored = 0;
                    if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                    if (method_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "EXTEND_PROTOCOL method constant %u out of bounds", method_const);
                    if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                }
                break;
            }
            case ISH_OP_CALL_METHOD: {
                uint32_t protocol_const = 0;
                uint32_t method_const = 0;
                uint32_t ignored = 0;
                if (!verify_operand(module, &ip, op, &protocol_const, err)) return false;
                if (protocol_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "CALL_METHOD protocol constant %u out of bounds", protocol_const);
                if (!verify_operand(module, &ip, op, &method_const, err)) return false;
                if (method_const >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "CALL_METHOD method constant %u out of bounds", method_const);
                if (!verify_operand(module, &ip, op, &ignored, err)) return false;
                break;
            }
            case ISH_OP_PRIM_CALL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= ish_primitive_count()) return ish_error_set(err, ish_span_unknown(NULL), "PRIM_CALL primitive %u out of bounds", operand);
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case ISH_OP_LOAD_CONST:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "LOAD_CONST index %u out of bounds", operand);
                break;
            case ISH_OP_ENTER_NAMESPACE:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                if (operand >= module->const_count) return ish_error_set(err, ish_span_unknown(NULL), "ENTER_NAMESPACE index %u out of bounds", operand);
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
            case ISH_OP_LOAD_GLOBAL:
            case ISH_OP_STORE_GLOBAL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
                break;
            case ISH_OP_IMPORT_GLOBAL:
                if (!verify_operand(module, &ip, op, &operand, err)) return false;
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
            case ISH_OP_RECV:
            case ISH_OP_RESCUE_PUSH:
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
            case ISH_OP_PRIM_CALL:
            case ISH_OP_RECV:
            case ISH_OP_RESCUE_PUSH:
            case ISH_OP_LOAD_GLOBAL:
            case ISH_OP_STORE_GLOBAL:
            case ISH_OP_IMPORT_GLOBAL:
                if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                if (op == ISH_OP_IMPORT_GLOBAL) {
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                } else if (op == ISH_OP_MAKE_CLOSURE_CAPTURES) {
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                } else if (op == ISH_OP_MAKE_MULTI_CLOSURE) {
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    uint32_t entry_count = module->code[ip];
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    for (uint32_t i = 0; i < entry_count; i++) {
                        if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                        if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                    }
                } else if (op == ISH_OP_PRIM_CALL) {
                    if (ip >= module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u", module->code[ip++])) return false;
                }
                break;
            case ISH_OP_DEFINE_PROTOCOL: {
                if (ip + 2u > module->code_count) return ish_buf_append(buf, " <missing>\n");
                uint32_t protocol_const = module->code[ip++];
                uint32_t method_count = module->code[ip++];
                if (!ish_buf_appendf(buf, " %u %u", protocol_const, method_count)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    if (ip + 3u > module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u %u %u", module->code[ip], module->code[ip + 1u], module->code[ip + 2u])) return false;
                    ip += 3u;
                }
                break;
            }
            case ISH_OP_EXTEND_PROTOCOL: {
                if (ip + 3u > module->code_count) return ish_buf_append(buf, " <missing>\n");
                uint32_t protocol_const = module->code[ip++];
                uint32_t type_const = module->code[ip++];
                uint32_t method_count = module->code[ip++];
                if (!ish_buf_appendf(buf, " %u %u %u", protocol_const, type_const, method_count)) return false;
                for (uint32_t i = 0; i < method_count; i++) {
                    if (ip + 2u > module->code_count) return ish_buf_append(buf, " <missing>\n");
                    if (!ish_buf_appendf(buf, " %u %u", module->code[ip], module->code[ip + 1u])) return false;
                    ip += 2u;
                }
                break;
            }
            case ISH_OP_CALL_METHOD:
                if (ip + 3u > module->code_count) return ish_buf_append(buf, " <missing>\n");
                if (!ish_buf_appendf(buf, " %u %u %u", module->code[ip], module->code[ip + 1u], module->code[ip + 2u])) return false;
                ip += 3u;
                break;
            default:
                break;
        }
        if (!ish_buf_append_char(buf, '\n')) return false;
    }
    return true;
}

static bool serialize_value(IshBuffer *out, IshValue v, IshError *err) {
    switch (v.tag) {
        case ISH_VAL_NIL: return ish_buf_put_u8(out, 0u);
        case ISH_VAL_INT: return ish_buf_put_u8(out, 1u) && ish_buf_put_u64(out, (uint64_t)v.as.i);
        case ISH_VAL_FLOAT: { uint64_t bits; double d = v.as.f; memcpy(&bits, &d, 8u); return ish_buf_put_u8(out, 2u) && ish_buf_put_u64(out, bits); }
        case ISH_VAL_STRING: return ish_buf_put_u8(out, 3u) && ish_buf_put_str(out, ish_string_bytes(v), ish_string_length(v));
        case ISH_VAL_ATOM: { const char *s = ish_symbol_text(v.as.symbol); return ish_buf_put_u8(out, 4u) && ish_buf_put_str(out, s, strlen(s)); }
        case ISH_VAL_WORD: { const char *s = ish_symbol_text(v.as.symbol); return ish_buf_put_u8(out, 5u) && ish_buf_put_str(out, s, strlen(s)); }
        case ISH_VAL_PAIR: {
            IshError ignore; ish_error_init(&ignore);
            IshValue car = ish_car(v, &ignore), cdr = ish_cdr(v, &ignore);
            ish_error_clear(&ignore);
            return ish_buf_put_u8(out, 6u) && serialize_value(out, car, err) && serialize_value(out, cdr, err);
        }
        case ISH_VAL_TUPLE:
        case ISH_VAL_VECTOR: {
            size_t n = ish_sequence_count(v);
            if (!ish_buf_put_u8(out, v.tag == ISH_VAL_TUPLE ? 7u : 8u) || !ish_buf_put_u32(out, (uint32_t)n)) return ish_error_oom(err, ish_span_unknown(NULL));
            for (size_t i = 0; i < n; i++) {
                IshError ignore; ish_error_init(&ignore);
                IshValue item = ish_sequence_item(v, i, &ignore);
                ish_error_clear(&ignore);
                if (!serialize_value(out, item, err)) return false;
            }
            return true;
        }
        case ISH_VAL_DICT: {
            size_t n = ish_dict_count(v);
            if (!ish_buf_put_u8(out, 9u) || !ish_buf_put_u32(out, (uint32_t)n)) return ish_error_oom(err, ish_span_unknown(NULL));
            for (size_t i = 0; i < n; i++) {
                IshValue key, val;
                if (!ish_dict_entry(v, i, &key, &val)) return ish_error_set(err, ish_span_unknown(NULL), "dict entry serialization failed");
                if (!serialize_value(out, key, err) || !serialize_value(out, val, err)) return false;
            }
            return true;
        }
        case ISH_VAL_PRIMITIVE:
            return ish_buf_put_u8(out, 10u) && ish_buf_put_u32(out, (uint32_t)v.as.id);
        case ISH_VAL_RECORD: {
            IshError ignore;
            ish_error_init(&ignore);
            const char *type = ish_record_type(v, &ignore);
            IshValue fields = ish_record_fields(v, &ignore);
            ish_error_clear(&ignore);
            return ish_buf_put_u8(out, 11u) && ish_buf_put_str(out, type ? type : "", type ? strlen(type) : 0u) && serialize_value(out, fields, err);
        }
        case ISH_VAL_SYNTAX: {
            const IshSyntax *syn = ish_syntax_get(v, err);
            if (!syn) return false;
            if (!ish_buf_put_u8(out, 12u)) return ish_error_oom(err, ish_span_unknown(NULL));
            return ish_syn_serialize(out, syn, err);
        }
        default:
            return ish_error_set(err, ish_span_unknown(NULL), "value kind cannot be serialized to .ishc");
    }
}

static bool serialize_pattern(IshBuffer *out, const IshPattern *pat, IshError *err) {
    if (!ish_buf_put_u8(out, (uint8_t)pat->kind)) return ish_error_oom(err, ish_span_unknown(NULL));
    switch (pat->kind) {
        case ISH_PAT_WILDCARD: return true;
        case ISH_PAT_BIND:
        case ISH_PAT_PIN: return ish_buf_put_str(out, pat->as.name, strlen(pat->as.name)) ? true : ish_error_oom(err, ish_span_unknown(NULL));
        case ISH_PAT_LITERAL: return serialize_value(out, pat->as.literal, err);
        case ISH_PAT_PAIR: return serialize_pattern(out, pat->as.pair.left, err) && serialize_pattern(out, pat->as.pair.right, err);
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE: {
            if (!ish_buf_put_u32(out, (uint32_t)pat->as.seq.count)) return ish_error_oom(err, ish_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.seq.count; i++) if (!serialize_pattern(out, pat->as.seq.items[i], err)) return false;
            return true;
        }
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST: {
            if (!ish_buf_put_u32(out, (uint32_t)pat->as.seq_rest.count)) return ish_error_oom(err, ish_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) if (!serialize_pattern(out, pat->as.seq_rest.items[i], err)) return false;
            return serialize_pattern(out, pat->as.seq_rest.rest, err);
        }
        case ISH_PAT_DICT: {
            if (!ish_buf_put_u32(out, (uint32_t)pat->as.dict.count)) return ish_error_oom(err, ish_span_unknown(NULL));
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (!serialize_value(out, pat->as.dict.entries[i].key, err)) return false;
                if (!serialize_pattern(out, pat->as.dict.entries[i].pattern, err)) return false;
            }
            return true;
        }
        default:
            return ish_error_set(err, ish_span_unknown(NULL), "pattern kind cannot be serialized to .ishc");
    }
}

bool ish_ishc_serialize(const IshBytecodeModule *module, IshBuffer *out, IshError *err) {
    if (!ish_buf_append_n(out, "ISHC", 4u) || !ish_buf_put_u32(out, 1u)) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!ish_buf_put_u32(out, (uint32_t)module->const_count)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < module->const_count; i++) if (!serialize_value(out, module->constants[i], err)) return false;
    if (!ish_buf_put_u32(out, (uint32_t)module->function_count)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < module->function_count; i++) {
        const IshBcFunction *f = &module->functions[i];
        if (!ish_buf_put_str(out, f->name ? f->name : "", f->name ? strlen(f->name) : 0u)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!ish_buf_put_u32(out, f->arity) || !ish_buf_put_u32(out, f->local_count) || !ish_buf_put_u64(out, (uint64_t)f->entry)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!ish_buf_put_u8(out, f->has_guard ? 1u : 0u) || !ish_buf_put_u32(out, f->guard_function)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (!ish_buf_put_u32(out, f->pattern_count)) return ish_error_oom(err, ish_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_count; p++) if (!serialize_pattern(out, f->param_patterns[p], err)) return false;
        if (!ish_buf_put_u32(out, f->pattern_local_count)) return ish_error_oom(err, ish_span_unknown(NULL));
        for (uint32_t p = 0; p < f->pattern_local_count; p++) {
            if (!ish_buf_put_str(out, f->pattern_locals[p].name, strlen(f->pattern_locals[p].name)) || !ish_buf_put_u32(out, f->pattern_locals[p].slot)) return ish_error_oom(err, ish_span_unknown(NULL));
        }
    }
    if (!ish_buf_put_u64(out, (uint64_t)module->code_count)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < module->code_count; i++) if (!ish_buf_put_u32(out, module->code[i])) return ish_error_oom(err, ish_span_unknown(NULL));
    return true;
}

#define ISH_ISHC_MAX_DEPTH 1024u

static bool deserialize_value(IshRuntime *rt, IshByteReader *r, IshValue *out, unsigned depth, IshError *err) {
    if (depth > ISH_ISHC_MAX_DEPTH) return ish_error_set(err, ish_span_unknown(NULL), ".ishc value nested too deeply");
    uint8_t tag = ish_rd_u8(r);
    if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated .ishc value");
    switch (tag) {
        case 0u: *out = ish_nil(); return true;
        case 1u: { uint64_t bits = ish_rd_u64(r); if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated int"); *out = ish_int((int64_t)bits); return true; }
        case 2u: { uint64_t bits = ish_rd_u64(r); if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated float"); double d; memcpy(&d, &bits, 8u); *out = ish_float(d); return true; }
        case 3u: { char *s = ish_rd_string(r); if (!s) return ish_error_set(err, ish_span_unknown(NULL), "truncated string"); *out = ish_string(rt, s, err); free(s); return !(err && err->present); }
        case 4u: { char *s = ish_rd_string(r); if (!s) return ish_error_set(err, ish_span_unknown(NULL), "truncated atom"); *out = ish_atom(rt, s); free(s); return true; }
        case 5u: { char *s = ish_rd_string(r); if (!s) return ish_error_set(err, ish_span_unknown(NULL), "truncated word"); *out = ish_word(rt, s); free(s); return true; }
        case 6u: {
            IshValue car, cdr;
            if (!deserialize_value(rt, r, &car, depth + 1u, err) || !deserialize_value(rt, r, &cdr, depth + 1u, err)) return false;
            *out = ish_cons(rt, car, cdr, err); return !(err && err->present);
        }
        case 7u:
        case 8u: {
            uint32_t n = ish_rd_u32(r);
            if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated sequence");
            IshValue *items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) return ish_error_oom(err, ish_span_unknown(NULL));
            for (uint32_t i = 0; i < n; i++) if (!deserialize_value(rt, r, &items[i], depth + 1u, err)) { free(items); return false; }
            *out = tag == 7u ? ish_tuple(rt, items, n, err) : ish_vector(rt, items, n, err);
            free(items);
            return !(err && err->present);
        }
        case 9u: {
            uint32_t n = ish_rd_u32(r);
            if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated dict");
            IshDictEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) return ish_error_oom(err, ish_span_unknown(NULL));
            for (uint32_t i = 0; i < n; i++) if (!deserialize_value(rt, r, &entries[i].key, depth + 1u, err) || !deserialize_value(rt, r, &entries[i].value, depth + 1u, err)) { free(entries); return false; }
            *out = ish_dict(rt, entries, n, err);
            free(entries);
            return !(err && err->present);
        }
        case 10u: { uint32_t p = ish_rd_u32(r); if (!r->ok) return ish_error_set(err, ish_span_unknown(NULL), "truncated primitive"); *out = ish_primitive_value(p); return true; }
        case 11u: {
            char *type = ish_rd_string(r);
            if (!type) return ish_error_set(err, ish_span_unknown(NULL), "truncated record type");
            IshValue fields = ish_nil();
            bool ok = deserialize_value(rt, r, &fields, depth + 1u, err);
            if (ok) {
                *out = ish_record(rt, type, fields, err);
                ok = !(err && err->present);
            }
            free(type);
            return ok;
        }
        case 12u: {
            IshSyntax *syn = ish_syn_deserialize(rt, r, err);
            if (!syn) return false;
            *out = ish_syntax_value(rt, syn, err);
            ish_syn_free(syn);
            return !(err && err->present);
        }
        default:
            return ish_error_set(err, ish_span_unknown(NULL), "unknown .ishc value tag %u", tag);
    }
}

static IshPattern *deserialize_pattern(IshRuntime *rt, IshByteReader *r, unsigned depth, IshError *err) {
    if (depth > ISH_ISHC_MAX_DEPTH) { ish_error_set(err, ish_span_unknown(NULL), ".ishc pattern nested too deeply"); return NULL; }
    uint8_t kind = ish_rd_u8(r);
    if (!r->ok) { ish_error_set(err, ish_span_unknown(NULL), "truncated .ishc pattern"); return NULL; }
    IshSpan span = ish_span_unknown(NULL);
    switch (kind) {
        case ISH_PAT_WILDCARD: return ish_pat_wildcard(span);
        case ISH_PAT_BIND: { char *s = ish_rd_string(r); if (!s) { ish_error_set(err, span, "truncated pattern name"); return NULL; } IshPattern *p = ish_pat_bind(s, span); free(s); return p; }
        case ISH_PAT_PIN: { char *s = ish_rd_string(r); if (!s) { ish_error_set(err, span, "truncated pattern name"); return NULL; } IshPattern *p = ish_pat_pin(s, span); free(s); return p; }
        case ISH_PAT_LITERAL: { IshValue v; if (!deserialize_value(rt, r, &v, depth + 1u, err)) return NULL; return ish_pat_literal(v, span); }
        case ISH_PAT_PAIR: {
            IshPattern *left = deserialize_pattern(rt, r, depth + 1u, err); if (!left) return NULL;
            IshPattern *right = deserialize_pattern(rt, r, depth + 1u, err); if (!right) { ish_pat_free(left); return NULL; }
            IshPattern *p = ish_pat_pair(left, right, span);
            if (!p) { ish_pat_free(left); ish_pat_free(right); ish_error_oom(err, span); }
            return p;
        }
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE: {
            uint32_t n = ish_rd_u32(r); if (!r->ok) { ish_error_set(err, span, "truncated pattern sequence"); return NULL; }
            IshPattern **items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) { ish_error_oom(err, span); return NULL; }
            for (uint32_t i = 0; i < n; i++) { items[i] = deserialize_pattern(rt, r, depth + 1u, err); if (!items[i]) { for (uint32_t j = 0; j < i; j++) ish_pat_free(items[j]); free(items); return NULL; } }
            IshPattern *p = ish_pat_sequence((IshPatternKind)kind, items, n, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) ish_pat_free(items[i]); free(items); ish_error_oom(err, span); }
            return p;
        }
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST: {
            uint32_t n = ish_rd_u32(r); if (!r->ok) { ish_error_set(err, span, "truncated pattern rest"); return NULL; }
            IshPattern **items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) { ish_error_oom(err, span); return NULL; }
            for (uint32_t i = 0; i < n; i++) { items[i] = deserialize_pattern(rt, r, depth + 1u, err); if (!items[i]) { for (uint32_t j = 0; j < i; j++) ish_pat_free(items[j]); free(items); return NULL; } }
            IshPattern *rest = deserialize_pattern(rt, r, depth + 1u, err);
            if (!rest) { for (uint32_t i = 0; i < n; i++) ish_pat_free(items[i]); free(items); return NULL; }
            IshPattern *p = ish_pat_sequence_rest((IshPatternKind)kind, items, n, rest, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) ish_pat_free(items[i]); ish_pat_free(rest); free(items); ish_error_oom(err, span); }
            return p;
        }
        case ISH_PAT_DICT: {
            uint32_t n = ish_rd_u32(r); if (!r->ok) { ish_error_set(err, span, "truncated pattern dict"); return NULL; }
            IshDictPatternEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) { ish_error_oom(err, span); return NULL; }
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; i++) {
                if (!deserialize_value(rt, r, &entries[i].key, depth + 1u, err)) ok = false;
                else { entries[i].pattern = deserialize_pattern(rt, r, depth + 1u, err); if (!entries[i].pattern) ok = false; }
            }
            if (!ok) { for (uint32_t i = 0; i < n; i++) if (entries[i].pattern) ish_pat_free(entries[i].pattern); free(entries); return NULL; }
            IshPattern *p = ish_pat_dict(entries, n, span);
            if (!p) { for (uint32_t i = 0; i < n; i++) ish_pat_free(entries[i].pattern); free(entries); ish_error_oom(err, span); }
            return p;
        }
        default:
            ish_error_set(err, span, "unknown .ishc pattern kind %u", kind);
            return NULL;
    }
}

bool ish_ishc_deserialize(IshRuntime *rt, const unsigned char *data, size_t len, IshBytecodeModule *module, IshError *err) {
    ish_bc_init(module);
    IshByteReader r = { data, len, 0u, true };
    if (len < 8u || memcmp(data, "ISHC", 4u) != 0) { ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), "not an .ishc module"); }
    r.pos = 4u;
    uint32_t version = ish_rd_u32(&r);
    if (version != 1u) { ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), ".ishc version %u unsupported", version); }
    uint32_t const_count = ish_rd_u32(&r);
    for (uint32_t i = 0; i < const_count && r.ok; i++) {
        IshValue v;
        if (!deserialize_value(rt, &r, &v, 0u, err)) { ish_bc_destroy(module); return false; }
        if (!ish_bc_add_const(module, v, NULL)) { ish_bc_destroy(module); return ish_error_oom(err, ish_span_unknown(NULL)); }
    }
    uint32_t function_count = ish_rd_u32(&r);
    uint32_t *deferred_guards = NULL;
    if (r.ok && function_count != 0) {
        deferred_guards = malloc((size_t)function_count * sizeof(*deferred_guards));
        if (!deferred_guards) { ish_bc_destroy(module); return ish_error_oom(err, ish_span_unknown(NULL)); }
        for (uint32_t i = 0; i < function_count; i++) deferred_guards[i] = UINT32_MAX;
    }
    for (uint32_t i = 0; i < function_count && r.ok; i++) {
        char *name = ish_rd_string(&r);
        if (!name) { free(deferred_guards); ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), "truncated function name"); }
        uint32_t arity = ish_rd_u32(&r);
        uint32_t local_count = ish_rd_u32(&r);
        uint64_t entry = ish_rd_u64(&r);
        uint8_t has_guard = ish_rd_u8(&r);
        uint32_t guard_function = ish_rd_u32(&r);
        uint32_t idx = 0;
        if (!r.ok || !ish_bc_add_function(module, name, arity, local_count, (size_t)entry, &idx)) { free(name); free(deferred_guards); ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), "corrupt .ishc function"); }
        free(name);
        if (has_guard) deferred_guards[idx] = guard_function;
        uint32_t pattern_count = ish_rd_u32(&r);
        if (pattern_count != 0) {
            IshPattern **patterns = calloc(pattern_count, sizeof(*patterns));
            if (!patterns) { free(deferred_guards); ish_bc_destroy(module); return ish_error_oom(err, ish_span_unknown(NULL)); }
            bool ok = true;
            for (uint32_t p = 0; p < pattern_count && ok; p++) { patterns[p] = deserialize_pattern(rt, &r, 0u, err); if (!patterns[p]) ok = false; }
            if (!ok || !ish_bc_set_function_patterns_take(module, idx, patterns, pattern_count)) {
                for (uint32_t p = 0; p < pattern_count; p++) ish_pat_free(patterns[p]);
                free(patterns);
                free(deferred_guards);
                ish_bc_destroy(module);
                return false;
            }
        }
        uint32_t pattern_local_count = ish_rd_u32(&r);
        if (pattern_local_count != 0) {
            IshPatternLocal *locals = calloc(pattern_local_count, sizeof(*locals));
            if (!locals) { free(deferred_guards); ish_bc_destroy(module); return ish_error_oom(err, ish_span_unknown(NULL)); }
            bool ok = true;
            for (uint32_t p = 0; p < pattern_local_count && ok; p++) {
                char *lname = ish_rd_string(&r);
                uint32_t slot = ish_rd_u32(&r);
                if (!lname || !r.ok) { free(lname); ok = false; break; }
                locals[p].name = lname;
                locals[p].slot = slot;
            }
            if (!ok || !ish_bc_set_function_pattern_locals_take(module, idx, locals, pattern_local_count)) {
                for (uint32_t p = 0; p < pattern_local_count; p++) free(locals[p].name);
                free(locals);
                free(deferred_guards);
                ish_bc_destroy(module);
                return r.ok ? ish_error_oom(err, ish_span_unknown(NULL)) : ish_error_set(err, ish_span_unknown(NULL), "truncated pattern local");
            }
        }
    }
    for (uint32_t i = 0; i < function_count && r.ok; i++) {
        if (deferred_guards[i] != UINT32_MAX && !ish_bc_set_function_guard(module, i, deferred_guards[i])) {
            free(deferred_guards);
            ish_bc_destroy(module);
            return ish_error_set(err, ish_span_unknown(NULL), ".ishc guard function index is out of bounds");
        }
    }
    free(deferred_guards);
    uint64_t code_count = ish_rd_u64(&r);
    for (uint64_t i = 0; i < code_count && r.ok; i++) {
        uint32_t word = ish_rd_u32(&r);
        if (!r.ok || !ish_bc_emit(module, word, NULL)) { ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), "corrupt .ishc code"); }
    }
    if (!r.ok) { ish_bc_destroy(module); return ish_error_set(err, ish_span_unknown(NULL), "truncated .ishc module"); }
    if (!ish_bc_verify(module, err)) { ish_bc_destroy(module); return false; }
    return true;
}

static bool reloc_emit(IshBytecodeModule *dst, const IshBytecodeModule *src, uint32_t const_off, uint32_t fn_off, uint32_t code_off, IshError *err) {
    size_t ip = 0;
    while (ip < src->code_count) {
        IshOpcode op = (IshOpcode)src->code[ip++];
        if (!ish_bc_emit(dst, (uint32_t)op, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
        switch (op) {
            case ISH_OP_HALT: case ISH_OP_RETURN: case ISH_OP_POP: case ISH_OP_ADD: case ISH_OP_SUB:
            case ISH_OP_MUL: case ISH_OP_EQ: case ISH_OP_LT: case ISH_OP_MAKE_CELL: case ISH_OP_LOAD_CELL:
            case ISH_OP_STORE_CELL: case ISH_OP_SELF: case ISH_OP_SPAWN: case ISH_OP_SEND: case ISH_OP_EXIT:
            case ISH_OP_LINK: case ISH_OP_UNLINK: case ISH_OP_MONITOR: case ISH_OP_DEMONITOR: case ISH_OP_TRAP_EXIT:
            case ISH_OP_EXEC: case ISH_OP_AWAIT: case ISH_OP_APPLY: case ISH_OP_LEAVE_NAMESPACE:
            case ISH_OP_RESCUE_POP: case ISH_OP_RAISE: case ISH_OP_LOAD_RAISED:
                break;
            case ISH_OP_DEFINE_PROTOCOL: {
                uint32_t protocol_const = src->code[ip++];
                uint32_t method_count = src->code[ip++];
                if (!ish_bc_emit(dst, protocol_const + const_off, NULL) || !ish_bc_emit(dst, method_count, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = src->code[ip++];
                    uint32_t arity = src->code[ip++];
                    uint32_t has_default = src->code[ip++];
                    if (!ish_bc_emit(dst, method_const + const_off, NULL) || !ish_bc_emit(dst, arity, NULL) || !ish_bc_emit(dst, has_default, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                }
                break;
            }
            case ISH_OP_EXTEND_PROTOCOL: {
                uint32_t protocol_const = src->code[ip++];
                uint32_t type_const = src->code[ip++];
                uint32_t method_count = src->code[ip++];
                if (!ish_bc_emit(dst, protocol_const + const_off, NULL) || !ish_bc_emit(dst, type_const + const_off, NULL) || !ish_bc_emit(dst, method_count, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                for (uint32_t i = 0; i < method_count; i++) {
                    uint32_t method_const = src->code[ip++];
                    uint32_t arity = src->code[ip++];
                    if (!ish_bc_emit(dst, method_const + const_off, NULL) || !ish_bc_emit(dst, arity, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                }
                break;
            }
            case ISH_OP_CALL_METHOD:
                if (!ish_bc_emit(dst, src->code[ip++] + const_off, NULL) || !ish_bc_emit(dst, src->code[ip++] + const_off, NULL) || !ish_bc_emit(dst, src->code[ip++], NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_LOAD_CONST:
            case ISH_OP_ENTER_NAMESPACE:
                if (!ish_bc_emit(dst, src->code[ip++] + const_off, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_MAKE_CLOSURE:
                if (!ish_bc_emit(dst, src->code[ip++] + fn_off, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_LOAD_ARG: case ISH_OP_LOAD_LOCAL: case ISH_OP_STORE_LOCAL: case ISH_OP_LOAD_CAPTURE:
            case ISH_OP_CALL: case ISH_OP_TAIL_CALL: case ISH_OP_LOAD_GLOBAL: case ISH_OP_STORE_GLOBAL:
                if (!ish_bc_emit(dst, src->code[ip++], NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_IMPORT_GLOBAL:
            case ISH_OP_PRIM_CALL:
                if (!ish_bc_emit(dst, src->code[ip++], NULL) || !ish_bc_emit(dst, src->code[ip++], NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_MAKE_CLOSURE_CAPTURES:
                if (!ish_bc_emit(dst, src->code[ip++] + fn_off, NULL) || !ish_bc_emit(dst, src->code[ip++], NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            case ISH_OP_MAKE_MULTI_CLOSURE: {
                uint32_t entry_count = src->code[ip++];
                uint32_t capture_count = src->code[ip++];
                if (!ish_bc_emit(dst, entry_count, NULL) || !ish_bc_emit(dst, capture_count, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                for (uint32_t i = 0; i < entry_count; i++) if (!ish_bc_emit(dst, src->code[ip++] + fn_off, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            }
            case ISH_OP_JUMP: case ISH_OP_JUMP_IF_FALSE: case ISH_OP_RECV: case ISH_OP_RESCUE_PUSH:
                if (!ish_bc_emit(dst, src->code[ip++] + code_off, NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
                break;
            default:
                return ish_error_set(err, ish_span_unknown(NULL), "relocation: invalid opcode %u", (unsigned)op);
        }
    }
    return true;
}

bool ish_bc_link(IshBytecodeModule *dst, const IshBytecodeModule *src, uint32_t *out_const_offset, uint32_t *out_fn_offset, uint32_t *out_code_offset, IshError *err) {
    uint32_t const_off = (uint32_t)dst->const_count;
    uint32_t fn_off = (uint32_t)dst->function_count;
    uint32_t code_off = (uint32_t)dst->code_count;
    for (size_t i = 0; i < src->const_count; i++) if (!ish_bc_add_const(dst, src->constants[i], NULL)) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < src->function_count; i++) {
        const IshBcFunction *sf = &src->functions[i];
        uint32_t nf = 0;
        if (!ish_bc_add_function(dst, sf->name, sf->arity, sf->local_count, sf->entry + code_off, &nf)) return ish_error_oom(err, ish_span_unknown(NULL));
        if (sf->pattern_count != 0) {
            IshPattern **clones = malloc((size_t)sf->pattern_count * sizeof(*clones));
            if (!clones) return ish_error_oom(err, ish_span_unknown(NULL));
            bool ok = true;
            for (uint32_t p = 0; p < sf->pattern_count && ok; p++) { clones[p] = ish_pat_clone(sf->param_patterns[p]); if (!clones[p]) ok = false; }
            if (!ok || !ish_bc_set_function_patterns_take(dst, nf, clones, sf->pattern_count)) {
                for (uint32_t p = 0; p < sf->pattern_count; p++) ish_pat_free(clones[p]);
                free(clones);
                return ish_error_oom(err, ish_span_unknown(NULL));
            }
        }
        if (sf->pattern_local_count != 0) {
            IshPatternLocal *locals = malloc((size_t)sf->pattern_local_count * sizeof(*locals));
            if (!locals) return ish_error_oom(err, ish_span_unknown(NULL));
            bool ok = true;
            for (uint32_t p = 0; p < sf->pattern_local_count && ok; p++) { locals[p].name = ish_strdup(sf->pattern_locals[p].name); locals[p].slot = sf->pattern_locals[p].slot; if (!locals[p].name) ok = false; }
            if (!ok || !ish_bc_set_function_pattern_locals_take(dst, nf, locals, sf->pattern_local_count)) {
                for (uint32_t p = 0; p < sf->pattern_local_count; p++) free(locals[p].name);
                free(locals);
                return ish_error_oom(err, ish_span_unknown(NULL));
            }
        }
    }
    for (size_t i = 0; i < src->function_count; i++) {
        const IshBcFunction *sf = &src->functions[i];
        if (sf->has_guard && !ish_bc_set_function_guard(dst, fn_off + (uint32_t)i, sf->guard_function + fn_off)) {
            return ish_error_set(err, ish_span_unknown(NULL), "linked guard function index is out of bounds");
        }
    }
    if (!reloc_emit(dst, src, const_off, fn_off, code_off, err)) return false;
    if (out_const_offset) *out_const_offset = const_off;
    if (out_fn_offset) *out_fn_offset = fn_off;
    if (out_code_offset) *out_code_offset = code_off;
    return true;
}

static bool value_relocate_syntax(IshRuntime *rt, IshValue v, IshScopeId min_id, int64_t delta, IshValue *out, bool *changed, IshError *err) {
    *out = v;
    switch (v.tag) {
        case ISH_VAL_SYNTAX: {
            const IshSyntax *syn = ish_syntax_get(v, err);
            if (!syn) return false;
            IshSyntax *copy = ish_syn_clone(syn);
            if (!copy) return ish_error_oom(err, ish_span_unknown(NULL));
            ish_syn_scope_relocate_tree(copy, min_id, delta);
            *out = ish_syntax_value(rt, copy, err);
            ish_syn_free(copy);
            if (err && err->present) return false;
            *changed = true;
            return true;
        }
        case ISH_VAL_PAIR: {
            IshValue car = ish_car(v, err);
            IshValue cdr = ish_cdr(v, err);
            if (err && err->present) return false;
            IshValue new_car, new_cdr;
            bool child_changed = false;
            if (!value_relocate_syntax(rt, car, min_id, delta, &new_car, &child_changed, err)) return false;
            if (!value_relocate_syntax(rt, cdr, min_id, delta, &new_cdr, &child_changed, err)) return false;
            if (!child_changed) return true;
            *out = ish_cons(rt, new_car, new_cdr, err);
            if (err && err->present) return false;
            *changed = true;
            return true;
        }
        case ISH_VAL_TUPLE:
        case ISH_VAL_VECTOR: {
            size_t n = ish_sequence_count(v);
            IshValue *items = n == 0 ? NULL : calloc(n, sizeof(*items));
            if (n != 0 && !items) return ish_error_oom(err, ish_span_unknown(NULL));
            bool child_changed = false;
            for (size_t i = 0; i < n; i++) {
                IshValue item = ish_sequence_item(v, i, err);
                if ((err && err->present) || !value_relocate_syntax(rt, item, min_id, delta, &items[i], &child_changed, err)) {
                    free(items);
                    return false;
                }
            }
            if (child_changed) {
                *out = v.tag == ISH_VAL_TUPLE ? ish_tuple(rt, items, n, err) : ish_vector(rt, items, n, err);
                if (err && err->present) {
                    free(items);
                    return false;
                }
                *changed = true;
            }
            free(items);
            return true;
        }
        case ISH_VAL_DICT: {
            size_t n = ish_dict_count(v);
            IshDictEntry *entries = n == 0 ? NULL : calloc(n, sizeof(*entries));
            if (n != 0 && !entries) return ish_error_oom(err, ish_span_unknown(NULL));
            bool child_changed = false;
            for (size_t i = 0; i < n; i++) {
                IshValue key, val;
                if (!ish_dict_entry(v, i, &key, &val)) {
                    free(entries);
                    return ish_error_set(err, ish_span_unknown(NULL), "dict entry walk failed");
                }
                if (!value_relocate_syntax(rt, key, min_id, delta, &entries[i].key, &child_changed, err) ||
                    !value_relocate_syntax(rt, val, min_id, delta, &entries[i].value, &child_changed, err)) {
                    free(entries);
                    return false;
                }
            }
            if (child_changed) {
                *out = ish_dict(rt, entries, n, err);
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

static bool pattern_relocate_syntax(IshRuntime *rt, IshPattern *pat, IshScopeId min_id, int64_t delta, IshError *err) {
    if (!pat) return true;
    bool changed = false;
    switch (pat->kind) {
        case ISH_PAT_LITERAL:
            return value_relocate_syntax(rt, pat->as.literal, min_id, delta, &pat->as.literal, &changed, err);
        case ISH_PAT_PAIR:
            return pattern_relocate_syntax(rt, pat->as.pair.left, min_id, delta, err) &&
                   pattern_relocate_syntax(rt, pat->as.pair.right, min_id, delta, err);
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (!pattern_relocate_syntax(rt, pat->as.seq.items[i], min_id, delta, err)) return false;
            }
            return true;
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                if (!pattern_relocate_syntax(rt, pat->as.seq_rest.items[i], min_id, delta, err)) return false;
            }
            return pattern_relocate_syntax(rt, pat->as.seq_rest.rest, min_id, delta, err);
        case ISH_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                if (!value_relocate_syntax(rt, pat->as.dict.entries[i].key, min_id, delta, &pat->as.dict.entries[i].key, &changed, err)) return false;
                if (!pattern_relocate_syntax(rt, pat->as.dict.entries[i].pattern, min_id, delta, err)) return false;
            }
            return true;
        default:
            return true;
    }
}

bool ish_bc_relocate_syntax_scopes(IshRuntime *rt, IshBytecodeModule *module, IshScopeId min_id, int64_t delta, IshError *err) {
    if (delta == 0) return true;
    for (size_t i = 0; i < module->const_count; i++) {
        bool changed = false;
        IshValue replaced;
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
