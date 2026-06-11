#ifndef IDM_BYTECODE_H
#define IDM_BYTECODE_H

#include "idiom/pattern.h"

typedef enum {
    IDM_OP_HALT,
    IDM_OP_LOAD_CONST,
    IDM_OP_LOAD_ARG,
    IDM_OP_LOAD_LOCAL,
    IDM_OP_STORE_LOCAL,
    IDM_OP_LOAD_CAPTURE,
    IDM_OP_MAKE_CELL,
    IDM_OP_LOAD_CELL,
    IDM_OP_STORE_CELL,
    IDM_OP_MAKE_CLOSURE,
    IDM_OP_MAKE_CLOSURE_CAPTURES,
    IDM_OP_MAKE_MULTI_CLOSURE,
    IDM_OP_CALL,
    IDM_OP_TAIL_CALL,
    IDM_OP_RETURN,
    IDM_OP_POP,
    IDM_OP_JUMP,
    IDM_OP_JUMP_IF_FALSE,
    IDM_OP_ADD,
    IDM_OP_SUB,
    IDM_OP_MUL,
    IDM_OP_EQ,
    IDM_OP_LT,
    IDM_OP_PRIM_CALL,
    IDM_OP_SELF,
    IDM_OP_SPAWN,
    IDM_OP_SEND,
    IDM_OP_EXIT,
    IDM_OP_LINK,
    IDM_OP_UNLINK,
    IDM_OP_MONITOR,
    IDM_OP_DEMONITOR,
    IDM_OP_TRAP_EXIT,
    IDM_OP_RECV,
    IDM_OP_EXEC,
    IDM_OP_RESCUE_PUSH,
    IDM_OP_RESCUE_POP,
    IDM_OP_RAISE,
    IDM_OP_LOAD_RAISED,
    IDM_OP_LOAD_GLOBAL,
    IDM_OP_STORE_GLOBAL,
    IDM_OP_AWAIT,
    IDM_OP_APPLY,
    IDM_OP_ENTER_NAMESPACE,
    IDM_OP_IMPORT_GLOBAL,
    IDM_OP_LEAVE_NAMESPACE,
    IDM_OP_DEFINE_PROTOCOL,
    IDM_OP_EXTEND_PROTOCOL,
    IDM_OP_CALL_METHOD
} IdmOpcode;

typedef struct {
    char *name;
    uint32_t slot;
} IdmPatternLocal;

typedef struct {
    char *name;
    uint32_t arity;
    uint32_t local_count;
    size_t entry;
    bool has_guard;
    uint32_t guard_function;
    IdmPattern **param_patterns;
    uint32_t pattern_count;
    IdmPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
} IdmBcFunction;

typedef struct IdmBytecodeModule {
    uint32_t *code;
    size_t code_count;
    size_t code_cap;
    IdmValue *constants;
    size_t const_count;
    size_t const_cap;
    IdmBcFunction *functions;
    size_t function_count;
    size_t function_cap;
} IdmBytecodeModule;

void idm_bc_init(IdmBytecodeModule *module);
void idm_bc_destroy(IdmBytecodeModule *module);
bool idm_bc_add_const(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index);
bool idm_bc_add_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index);
bool idm_bc_set_function_entry(IdmBytecodeModule *module, uint32_t function_index, size_t entry);
bool idm_bc_set_function_patterns_take(IdmBytecodeModule *module, uint32_t function_index, IdmPattern **patterns, uint32_t pattern_count);
bool idm_bc_set_function_pattern_locals_take(IdmBytecodeModule *module, uint32_t function_index, IdmPatternLocal *locals, uint32_t local_count);
bool idm_bc_set_function_guard(IdmBytecodeModule *module, uint32_t function_index, uint32_t guard_function);
bool idm_bc_emit(IdmBytecodeModule *module, uint32_t word, size_t *out_offset);
bool idm_bc_emit_op(IdmBytecodeModule *module, IdmOpcode op, size_t *out_offset);
bool idm_bc_emit_u32(IdmBytecodeModule *module, IdmOpcode op, uint32_t operand, size_t *out_offset);
bool idm_bc_patch_u32(IdmBytecodeModule *module, size_t operand_offset, uint32_t operand);
bool idm_bc_verify(const IdmBytecodeModule *module, IdmError *err);
const char *idm_opcode_name(IdmOpcode op);
bool idm_bc_disassemble(IdmBuffer *buf, const IdmBytecodeModule *module);
bool idm_ic_serialize(const IdmBytecodeModule *module, IdmBuffer *out, IdmError *err);
bool idm_ic_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmBytecodeModule *module, IdmError *err);
bool idm_bc_link(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t *out_const_offset, uint32_t *out_fn_offset, uint32_t *out_code_offset, IdmError *err);
bool idm_bc_relocate_syntax_scopes(IdmRuntime *rt, IdmBytecodeModule *module, IdmScopeId min_id, int64_t delta, IdmError *err);

#endif
