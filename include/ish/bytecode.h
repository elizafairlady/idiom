#ifndef ISH_BYTECODE_H
#define ISH_BYTECODE_H

#include "ish/pattern.h"

typedef enum {
    ISH_OP_HALT,
    ISH_OP_LOAD_CONST,
    ISH_OP_LOAD_ARG,
    ISH_OP_LOAD_LOCAL,
    ISH_OP_STORE_LOCAL,
    ISH_OP_LOAD_CAPTURE,
    ISH_OP_MAKE_CELL,
    ISH_OP_LOAD_CELL,
    ISH_OP_STORE_CELL,
    ISH_OP_MAKE_CLOSURE,
    ISH_OP_MAKE_CLOSURE_CAPTURES,
    ISH_OP_MAKE_MULTI_CLOSURE,
    ISH_OP_CALL,
    ISH_OP_TAIL_CALL,
    ISH_OP_RETURN,
    ISH_OP_POP,
    ISH_OP_JUMP,
    ISH_OP_JUMP_IF_FALSE,
    ISH_OP_ADD,
    ISH_OP_SUB,
    ISH_OP_MUL,
    ISH_OP_EQ,
    ISH_OP_LT
} IshOpcode;

typedef struct {
    char *name;
    uint32_t slot;
} IshPatternLocal;

typedef struct {
    char *name;
    uint32_t arity;
    uint32_t local_count;
    size_t entry;
    bool has_guard;
    uint32_t guard_function;
    IshPattern **param_patterns;
    uint32_t pattern_count;
    IshPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
} IshBcFunction;

typedef struct {
    uint32_t *code;
    size_t code_count;
    size_t code_cap;
    IshValue *constants;
    size_t const_count;
    size_t const_cap;
    IshBcFunction *functions;
    size_t function_count;
    size_t function_cap;
} IshBytecodeModule;

void ish_bc_init(IshBytecodeModule *module);
void ish_bc_destroy(IshBytecodeModule *module);
bool ish_bc_add_const(IshBytecodeModule *module, IshValue value, uint32_t *out_index);
bool ish_bc_add_function(IshBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index);
bool ish_bc_set_function_entry(IshBytecodeModule *module, uint32_t function_index, size_t entry);
bool ish_bc_set_function_patterns_take(IshBytecodeModule *module, uint32_t function_index, IshPattern **patterns, uint32_t pattern_count);
bool ish_bc_set_function_pattern_locals_take(IshBytecodeModule *module, uint32_t function_index, IshPatternLocal *locals, uint32_t local_count);
bool ish_bc_set_function_guard(IshBytecodeModule *module, uint32_t function_index, uint32_t guard_function);
bool ish_bc_emit(IshBytecodeModule *module, uint32_t word, size_t *out_offset);
bool ish_bc_emit_op(IshBytecodeModule *module, IshOpcode op, size_t *out_offset);
bool ish_bc_emit_u32(IshBytecodeModule *module, IshOpcode op, uint32_t operand, size_t *out_offset);
bool ish_bc_patch_u32(IshBytecodeModule *module, size_t operand_offset, uint32_t operand);
bool ish_bc_verify(const IshBytecodeModule *module, IshError *err);
const char *ish_opcode_name(IshOpcode op);
bool ish_bc_disassemble(IshBuffer *buf, const IshBytecodeModule *module);

#endif
