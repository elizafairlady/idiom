#ifndef IDM_BYTECODE_H
#define IDM_BYTECODE_H

#include "idiom/pattern.h"

#define IDM_OPCODE_NO_COUNT 255u

typedef enum {
    IDM_OPCODE_FLOW_NEXT,
    IDM_OPCODE_FLOW_TERMINAL,
    IDM_OPCODE_FLOW_JUMP,
    IDM_OPCODE_FLOW_BRANCH,
    IDM_OPCODE_FLOW_BRANCH_TAIL,
    IDM_OPCODE_FLOW_TAIL_FALLTHROUGH
} IdmOpcodeFlow;

#define IDM_OPCODE_LIST(X) \
    X(HALT, 0, IDM_OPCODE_NO_COUNT, 0, "", "", "", "", IDM_OPCODE_FLOW_TERMINAL, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MOVE, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "ds", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_CONST, 2, IDM_OPCODE_NO_COUNT, 0, "rc", "", "dr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_CAPTURE, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "dr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MAKE_CELL, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "ds", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_CELL, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "ds", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(STORE_CELL, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "ss", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MAKE_CLOSURE, 2, IDM_OPCODE_NO_COUNT, 0, "rf", "", "dr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 1) \
    X(MAKE_CLOSURE_CAPTURES, 4, IDM_OPCODE_NO_COUNT, 0, "rfrr", "", "drrr", "23", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 1) \
    X(MAKE_MULTI_CLOSURE, 4, 1, 1, "rmrr", "f", "drrr", "32", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 1) \
    X(CALL, 5, IDM_OPCODE_NO_COUNT, 0, "rrrrt", "", "dsrrr", "23", IDM_OPCODE_FLOW_TAIL_FALLTHROUGH, IDM_OPCODE_NO_COUNT, 4, 0) \
    X(RETURN, 1, IDM_OPCODE_NO_COUNT, 0, "r", "", "s", "", IDM_OPCODE_FLOW_TERMINAL, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(JUMP, 1, IDM_OPCODE_NO_COUNT, 0, "j", "", "r", "", IDM_OPCODE_FLOW_JUMP, 0, IDM_OPCODE_NO_COUNT, 0) \
    X(JUMP_IF_FALSE, 2, IDM_OPCODE_NO_COUNT, 0, "rj", "", "sr", "", IDM_OPCODE_FLOW_BRANCH, 1, IDM_OPCODE_NO_COUNT, 0) \
    X(RECV, 5, IDM_OPCODE_NO_COUNT, 0, "rrrjt", "", "dssrr", "", IDM_OPCODE_FLOW_BRANCH_TAIL, 3, 4, 0) \
    X(RESCUE_PUSH, 1, IDM_OPCODE_NO_COUNT, 0, "j", "", "r", "", IDM_OPCODE_FLOW_BRANCH, 0, IDM_OPCODE_NO_COUNT, 0) \
    X(RESCUE_POP, 0, IDM_OPCODE_NO_COUNT, 0, "", "", "", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(RAISE, 1, IDM_OPCODE_NO_COUNT, 0, "r", "", "s", "", IDM_OPCODE_FLOW_TERMINAL, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_RAISED, 1, IDM_OPCODE_NO_COUNT, 0, "r", "", "d", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_ENV, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "dr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(STORE_ENV, 2, IDM_OPCODE_NO_COUNT, 0, "rr", "", "rs", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LOAD_PACKAGE_SLOT, 3, IDM_OPCODE_NO_COUNT, 0, "rcr", "", "drr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(STORE_PACKAGE_SLOT, 3, IDM_OPCODE_NO_COUNT, 0, "crr", "", "rrs", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(PUSH_PACKAGE_ENV, 1, IDM_OPCODE_NO_COUNT, 0, "c", "", "r", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(POP_PACKAGE_ENV, 0, IDM_OPCODE_NO_COUNT, 0, "", "", "", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MAKE_RECORD, 4, 3, 2, "rTrr", "cT", "drrr", "23", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(RECORD_FIELD, 5, IDM_OPCODE_NO_COUNT, 0, "rrTcr", "", "dsrrr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(RECORD_IS, 3, IDM_OPCODE_NO_COUNT, 0, "rrT", "", "dsr", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LIST_CONS, 3, IDM_OPCODE_NO_COUNT, 0, "rrr", "", "dss", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(LIST_APPEND, 3, IDM_OPCODE_NO_COUNT, 0, "rrr", "", "dss", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MAKE_VALUE_SEQUENCE, 4, IDM_OPCODE_NO_COUNT, 0, "rvrr", "", "drrr", "23", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MAKE_SYNTAX, 4, IDM_OPCODE_NO_COUNT, 0, "ryrr", "", "drss", "", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(STRING_CONCAT, 3, IDM_OPCODE_NO_COUNT, 0, "rrr", "", "drr", "12", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(CALL_PRIMITIVE, 4, IDM_OPCODE_NO_COUNT, 0, "rprr", "", "drrr", "23", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0) \
    X(MATCH, 7, 3, 1, "rrrmrrt", "f", "drrrrrr", "1254", IDM_OPCODE_FLOW_TAIL_FALLTHROUGH, IDM_OPCODE_NO_COUNT, 6, 1) \
    X(MATCH_BIND, 4, IDM_OPCODE_NO_COUNT, 0, "frrr", "", "rsdr", "23", IDM_OPCODE_FLOW_NEXT, IDM_OPCODE_NO_COUNT, IDM_OPCODE_NO_COUNT, 0)

typedef enum {
#define IDM_OPCODE_ENUM(name, fixed, count_index, repeat_width, roles, repeat_roles, reg_roles, reg_ranges, flow, branch_operand, tail_operand, selector_site) IDM_OP_##name,
    IDM_OPCODE_LIST(IDM_OPCODE_ENUM)
#undef IDM_OPCODE_ENUM
    IDM_OP_COUNT
} IdmOpcode;

typedef struct {
    const char *name;
    uint8_t fixed_operands;
    uint8_t count_operand;
    uint8_t repeat_width;
    const char *operand_roles;
    const char *repeat_roles;
    const char *register_roles;
    const char *register_ranges;
    IdmOpcodeFlow flow;
    uint8_t branch_operand;
    uint8_t tail_operand;
    uint8_t selector_site;
} IdmOpcodeInfo;

typedef struct {
    uint32_t offset;
    char *name;
} IdmBcNameNote;

typedef struct {
    IdmRecordShape *shape;
    const IdmTypeTerm **contracts;
    uint32_t field_count;
} IdmBcRecordSite;

typedef struct {
    IdmPatternSelector *selector;
    IdmBcRecordSite record;
    _Atomic(IdmEnv *) env;
    _Atomic uint64_t closure;
} IdmBcCodeSite;

typedef struct {
    char *name;
    char *doc;
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

typedef struct {
    IdmSymbol *env_key;
    uint32_t slot;
    uint32_t *entries;
    uint32_t entry_count;
} IdmBcEnvClosure;

typedef struct IdmBytecodeModule {
    uint32_t *code;
    size_t code_count;
    size_t code_cap;
    IdmValue *constants;
    size_t const_count;
    size_t const_cap;
    IdmTypeTerm *types;
    size_t type_count;
    size_t type_cap;
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
    IdmBcCodeSite *code_sites;
    size_t code_site_count;
    IdmBcEnvClosure *env_closures;
    size_t env_closure_count;
    size_t env_closure_cap;
    bool selectors_prepared;
    bool literals_interned;
    bool verified;
    uint64_t selector_generation;
} IdmBytecodeModule;

void idm_bc_init(IdmBytecodeModule *module);
void idm_bc_destroy(IdmBytecodeModule *module);
bool idm_bc_add_const(IdmBytecodeModule *module, IdmValue value, uint32_t *out_index);
bool idm_bc_add_type_term(IdmBytecodeModule *module, const IdmTypeTerm *term, uint32_t *out_index);
bool idm_bc_intern_literals(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err);
bool idm_bc_is_finalized(const IdmBytecodeModule *module);
bool idm_bc_add_function(IdmBytecodeModule *module, const char *name, uint32_t arity, uint32_t local_count, size_t entry, uint32_t *out_index);
bool idm_bc_add_primitive_function(IdmBytecodeModule *module, const char *name, IdmArity arity, uint32_t primitive, uint32_t *out_index);
bool idm_bc_set_function_entry(IdmBytecodeModule *module, uint32_t function_index, size_t entry);
bool idm_bc_set_function_register_count(IdmBytecodeModule *module, uint32_t function_index, uint32_t register_count);
bool idm_bc_set_function_doc(IdmBytecodeModule *module, uint32_t function_index, const char *doc);
bool idm_bc_note_span(IdmBytecodeModule *module, IdmSpan span);
bool idm_bc_note_name(IdmBytecodeModule *module, size_t offset, const char *name);
IdmSpan idm_bc_span_at(const IdmBytecodeModule *module, size_t ip);
bool idm_bc_prepare_selectors(IdmRuntime *rt, IdmBytecodeModule *module, IdmError *err);
IdmPatternSelector *idm_bc_selector_at(const IdmBytecodeModule *module, size_t offset);
IdmPatternSelector *idm_bc_function_selector_at(const IdmBytecodeModule *module, uint32_t fn);
bool idm_bc_build_selector_for_entries(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *entries, size_t entry_count, IdmPatternSelector **out, IdmError *err);
bool idm_bc_note_env_closure(IdmBytecodeModule *module, IdmSymbol *env_key, uint32_t slot, const uint32_t *entries, uint32_t entry_count);
const IdmBcEnvClosure *idm_bc_env_closure_for_slot(const IdmBytecodeModule *module, IdmSymbol *env_key, uint32_t slot);
bool idm_bc_set_function_patterns_take(IdmBytecodeModule *module, uint32_t function_index, IdmPattern **patterns, uint32_t pattern_count);
bool idm_bc_set_function_pattern_locals_take(IdmBytecodeModule *module, uint32_t function_index, IdmPatternLocal *locals, uint32_t local_count);
bool idm_bc_set_function_guard(IdmBytecodeModule *module, uint32_t function_index, uint32_t guard_function);
bool idm_bc_emit(IdmBytecodeModule *module, uint32_t word, size_t *out_offset);
bool idm_bc_emit_op(IdmBytecodeModule *module, IdmOpcode op, size_t *out_offset);
bool idm_bc_emit_u32(IdmBytecodeModule *module, IdmOpcode op, uint32_t operand, size_t *out_offset);
bool idm_bc_patch_u32(IdmBytecodeModule *module, size_t operand_offset, uint32_t operand);
bool idm_bc_verify(const IdmBytecodeModule *module, IdmError *err);
extern const IdmOpcodeInfo idm_opcode_infos[IDM_OP_COUNT];

static inline const IdmOpcodeInfo *idm_opcode_info(IdmOpcode op) {
    return (size_t)op < (size_t)IDM_OP_COUNT ? &idm_opcode_infos[(size_t)op] : NULL;
}
const char *idm_opcode_name(IdmOpcode op);
bool idm_bc_instruction_width(const IdmBytecodeModule *module, size_t offset, size_t *out_width, IdmError *err);
bool idm_bc_disassemble(IdmBuffer *buf, const IdmBytecodeModule *module);
bool idm_ic_serialize(const IdmBytecodeModule *module, IdmBuffer *out, IdmError *err);
bool idm_ic_deserialize(IdmRuntime *rt, const unsigned char *data, size_t len, IdmBytecodeModule *module, IdmError *err);
bool idm_value_serialize(IdmBuffer *out, IdmValue value, IdmError *err);
bool idm_value_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmValue *out, IdmError *err);
bool idm_bc_link(IdmBytecodeModule *dst, const IdmBytecodeModule *src, uint32_t *out_const_offset, uint32_t *out_fn_offset, uint32_t *out_code_offset, IdmError *err);
bool idm_bc_tree_shake(IdmBytecodeModule *module, uint32_t *main_fn, IdmError *err);
bool idm_bc_relocate_syntax_scopes(IdmRuntime *rt, IdmBytecodeModule *module, IdmScopeId min_id, int64_t delta, IdmError *err);

#endif
