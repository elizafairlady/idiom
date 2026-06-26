#ifndef IDM_BYTECODE_H
#define IDM_BYTECODE_H

#include "idiom/pattern.h"

typedef enum {
    IDM_OP_HALT,
    IDM_OP_MOVE,
    IDM_OP_LOAD_CONST,
    IDM_OP_LOAD_CAPTURE,
    IDM_OP_MAKE_CELL,
    IDM_OP_LOAD_CELL,
    IDM_OP_STORE_CELL,
    IDM_OP_MAKE_CLOSURE,
    IDM_OP_MAKE_CLOSURE_CAPTURES,
    IDM_OP_MAKE_MULTI_CLOSURE,
    IDM_OP_CALL,
    IDM_OP_RETURN,
    IDM_OP_JUMP,
    IDM_OP_JUMP_IF_FALSE,
    IDM_OP_RECV,
    IDM_OP_RESCUE_PUSH,
    IDM_OP_RESCUE_POP,
    IDM_OP_RAISE,
    IDM_OP_LOAD_RAISED,
    IDM_OP_LOAD_ENV,
    IDM_OP_STORE_ENV,
    IDM_OP_LOAD_PACKAGE_SLOT,
    IDM_OP_STORE_PACKAGE_SLOT,
    IDM_OP_PUSH_PACKAGE_ENV,
    IDM_OP_POP_PACKAGE_ENV,
    IDM_OP_MAKE_RECORD,
    IDM_OP_RECORD_FIELD,
    IDM_OP_RECORD_IS,
    IDM_OP_LIST_CONS,
    IDM_OP_LIST_APPEND,
    IDM_OP_MAKE_VALUE_SEQUENCE,
    IDM_OP_MAKE_SYNTAX,
    IDM_OP_STRING_CONCAT,
    IDM_OP_CALL_PRIMITIVE,
    IDM_OP_MATCH,
} IdmOpcode;

typedef struct {
    uint32_t offset;
    char *name;
} IdmBcNameNote;

typedef struct {
    uint32_t offset;
    IdmPatternSelector *selector;
} IdmBcSelectorSite;

typedef struct {
    char *name;
    uint32_t arity;
    IdmArity call_arity;
    uint32_t local_count;
    uint32_t register_count;
    size_t entry;
    bool has_guard;
    uint32_t guard_function;
    bool primitive_backed;
    uint32_t primitive;
    IdmPattern **param_patterns;
    uint32_t pattern_count;
    IdmPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
    bool trivial_match;
    IdmPatternSelector *selector;
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
    char **span_files;
    size_t span_file_count;
    size_t span_file_cap;
    struct { uint32_t offset; uint32_t file; uint32_t line; uint32_t column; } *spans;
    size_t span_count;
    size_t span_cap;
    IdmBcNameNote *name_notes;
    size_t name_note_count;
    size_t name_note_cap;
    IdmBcSelectorSite *selector_sites;
    size_t selector_site_count;
    size_t selector_site_cap;
    IdmPatternSelector **selector_by_offset;
    size_t selector_by_offset_count;
    bool selectors_prepared;
    bool literals_interned;
    bool verified;
    uint64_t selector_generation;
} IdmBytecodeModule;

void idm_bc_init(IdmBytecodeModule *module);
void idm_bc_destroy(IdmBytecodeModule *module);
bool idm_bc_add_const(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index);
bool idm_bc_intern_literals(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err);
bool idm_bc_is_finalized(const IdmBytecodeModule *module);
bool idm_bc_add_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index);
bool idm_bc_add_primitive_function(IdmBytecodeModule *module, const char *name, IdmArity arity, uint32_t primitive, uint32_t *out_index);
bool idm_bc_set_function_entry(IdmBytecodeModule *module, uint32_t function_index, size_t entry);
bool idm_bc_set_function_register_count(IdmBytecodeModule *module, uint32_t function_index, uint32_t register_count);
bool idm_bc_note_span(IdmBytecodeModule *module, IdmSpan span);
bool idm_bc_note_name(IdmBytecodeModule *module, size_t offset, const char *name);
IdmSpan idm_bc_span_at(const IdmBytecodeModule *module, size_t ip);
bool idm_bc_prepare_selectors(IdmBytecodeModule *module, IdmError *err);
IdmPatternSelector *idm_bc_selector_at(const IdmBytecodeModule *module, size_t offset);
bool idm_bc_build_selector_for_entries(const IdmBytecodeModule *module, const uint32_t *entries, size_t entry_count, IdmPatternSelector **out, IdmError *err);
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
