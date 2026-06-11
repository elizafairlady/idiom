#ifndef IDM_VALUE_H
#define IDM_VALUE_H

#include "idiom/common.h"
#include "idiom/scope.h"

typedef struct IdmObject IdmObject;
typedef struct IdmSymbol IdmSymbol;
typedef struct IdmSyntax IdmSyntax;
typedef struct IdmRuntime IdmRuntime;
typedef struct IdmBytecodeModule IdmBytecodeModule;
typedef struct IdmExec IdmExec;

typedef bool (*IdmLocalExpandFn)(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
typedef bool (*IdmFreeIdentifierEqFn)(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err);
typedef bool (*IdmRegisterOperatorFn)(void *user, IdmRuntime *rt, const IdmSyntax *name, int64_t precedence, const char *assoc, const char *fixity, const IdmSyntax *target, IdmError *err);

typedef enum {
    IDM_SYMBOL_WORD,
    IDM_SYMBOL_ATOM
} IdmSymbolKind;

typedef enum {
    IDM_VAL_NIL,
    IDM_VAL_ATOM,
    IDM_VAL_WORD,
    IDM_VAL_INT,
    IDM_VAL_FLOAT,
    IDM_VAL_STRING,
    IDM_VAL_PAIR,
    IDM_VAL_TUPLE,
    IDM_VAL_VECTOR,
    IDM_VAL_DICT,
    IDM_VAL_SYNTAX,
    IDM_VAL_CELL,
    IDM_VAL_CLOSURE,
    IDM_VAL_PID,
    IDM_VAL_REF,
    IDM_VAL_PORT,
    IDM_VAL_PRIMITIVE,
    IDM_VAL_RECORD
} IdmValueTag;

typedef struct {
    IdmValueTag tag;
    union {
        int64_t i;
        double f;
        IdmObject *obj;
        IdmSymbol *symbol;
        uint64_t id;
    } as;
} IdmValue;

typedef struct {
    IdmValue key;
    IdmValue value;
} IdmDictEntry;

typedef bool (*IdmRegisterMacroFn)(void *user, IdmRuntime *rt, const IdmSyntax *name, IdmValue transformer, IdmError *err);
typedef bool (*IdmExpanderSurfaceFn)(void *user, IdmRuntime *rt, const char *kind, IdmValue *out, IdmError *err);

typedef struct {
    const char *name;
    uint32_t arity;
    bool has_default;
    IdmValue default_impl;
} IdmProtocolMethodSpec;

typedef struct {
    const char *name;
    uint32_t arity;
    IdmValue impl;
} IdmProtocolImplSpec;

typedef struct {
    char *protocol;
    char *method;
    uint32_t arity;
    bool has_default;
    IdmValue default_impl;
} IdmRuntimeProtocolMethod;

typedef struct {
    char *protocol;
    char *method;
    char *type;
    IdmValue impl;
} IdmRuntimeProtocolImpl;

typedef struct {
    char *protocol;
    char *type;
} IdmRuntimeProtocolConformance;

typedef struct {
    IdmSymbol **symbols;
    size_t count;
    size_t cap;
    uint32_t next_id;
} IdmIntern;

typedef struct {
    IdmObject *objects;
    size_t object_count;
    size_t bytes_allocated;
} IdmHeap;

typedef struct IdmNamespace {
    char *name;
    IdmValue *slots;
    size_t slot_count;
    size_t slot_cap;
} IdmNamespace;

struct IdmRuntime {
    IdmIntern intern;
    IdmHeap heap;
    bool macro_intro_active;
    IdmScopeId macro_intro_scope;
    void *local_expand_user;
    IdmLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IdmFreeIdentifierEqFn free_identifier_eq;
    void *register_operator_user;
    IdmRegisterOperatorFn register_operator;
    void *register_macro_user;
    IdmRegisterMacroFn register_macro;
    void *expander_surface_user;
    IdmExpanderSurfaceFn expander_surface;
    char **cli_args;
    size_t cli_arg_count;
    IdmNamespace **namespaces;
    size_t ns_count;
    size_t ns_cap;
    IdmNamespace *main_ns;
    IdmRuntimeProtocolMethod *protocol_methods;
    size_t protocol_method_count;
    size_t protocol_method_cap;
    IdmRuntimeProtocolImpl *protocol_impls;
    size_t protocol_impl_count;
    size_t protocol_impl_cap;
    IdmRuntimeProtocolConformance *protocol_conformances;
    size_t protocol_conformance_count;
    size_t protocol_conformance_cap;
    const IdmBytecodeModule **gc_modules;
    size_t gc_module_count;
    size_t gc_module_cap;
    IdmValue *gc_values;
    size_t gc_value_count;
    size_t gc_value_cap;
    void *expand_cache;
    void (*expand_cache_free)(void *cache);
    IdmExec *current_exec;
    char **owned_temps;
    size_t owned_temp_count;
    size_t owned_temp_cap;
};

IdmNamespace *idm_namespace_get_or_create(IdmRuntime *rt, const char *name);
bool idm_ns_slot_ensure(IdmNamespace *ns, uint32_t id, IdmError *err);
void idm_ns_slot_set(IdmNamespace *ns, uint32_t id, IdmValue value);
IdmValue idm_ns_slot_get(const IdmNamespace *ns, uint32_t id);

void idm_runtime_init(IdmRuntime *rt);
void idm_runtime_destroy(IdmRuntime *rt);
bool idm_runtime_own_temp(IdmRuntime *rt, const char *path);
bool idm_runtime_register_gc_module(IdmRuntime *rt, const IdmBytecodeModule *module);
void idm_runtime_unregister_gc_module(IdmRuntime *rt, const IdmBytecodeModule *module);
bool idm_runtime_register_gc_value(IdmRuntime *rt, IdmValue value);
void idm_runtime_unregister_gc_value(IdmRuntime *rt, IdmValue value);

void idm_intern_init(IdmIntern *intern);
void idm_intern_destroy(IdmIntern *intern);
IdmSymbol *idm_intern(IdmIntern *intern, IdmSymbolKind kind, const char *text);
const char *idm_symbol_text(const IdmSymbol *sym);
uint32_t idm_symbol_id(const IdmSymbol *sym);
IdmSymbolKind idm_symbol_kind(const IdmSymbol *sym);

void idm_heap_init(IdmHeap *heap);
void idm_heap_destroy(IdmHeap *heap);
void idm_heap_collect(IdmHeap *heap, const IdmValue *roots, size_t root_count);
void idm_gc_mark_value(IdmValue value);
void idm_heap_sweep(IdmHeap *heap);
size_t idm_heap_object_count(const IdmHeap *heap);
size_t idm_heap_bytes(const IdmHeap *heap);

IdmValue idm_nil(void);
IdmValue idm_int(int64_t value);
IdmValue idm_float(double value);
IdmValue idm_word(IdmRuntime *rt, const char *text);
IdmValue idm_atom(IdmRuntime *rt, const char *text);
IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err);
IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err);
IdmValue idm_cons(IdmRuntime *rt, IdmValue car, IdmValue cdr, IdmError *err);
IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err);
IdmValue idm_syntax_value(IdmRuntime *rt, const IdmSyntax *syntax, IdmError *err);
IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err);
IdmValue idm_closure(IdmRuntime *rt, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err);
IdmValue idm_closure_multi(IdmRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err);
IdmValue idm_closure_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err);
IdmValue idm_closure_multi_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, const IdmValue *captures, size_t capture_count, IdmNamespace *ns, IdmError *err);
IdmValue idm_record(IdmRuntime *rt, const char *type, IdmValue fields, IdmError *err);
IdmValue idm_pid(uint64_t id);
IdmValue idm_ref(uint64_t id);
IdmValue idm_port(uint64_t id);
IdmValue idm_primitive_value(uint32_t primitive);
bool idm_is_primitive(IdmValue value);
const char *idm_value_type_name(IdmValueTag tag);
const char *idm_value_dispatch_type_name(IdmValue value);
bool idm_value_type_from_name(const char *name, IdmValueTag *out);
bool idm_protocol_define(IdmRuntime *rt, const char *protocol, const IdmProtocolMethodSpec *methods, size_t method_count, IdmError *err);
bool idm_protocol_extend(IdmRuntime *rt, const char *protocol, const char *type, const IdmProtocolImplSpec *impls, size_t impl_count, IdmError *err);
bool idm_protocol_lookup(IdmRuntime *rt, const char *protocol, const char *method, const char *type, uint32_t argc, IdmValue *out_impl, IdmError *err);
void idm_runtime_mark_protocol_roots(IdmRuntime *rt);

bool idm_is_nil(IdmValue value);
bool idm_is_pair(IdmValue value);
bool idm_is_tuple(IdmValue value);
bool idm_is_vector(IdmValue value);
bool idm_is_dict(IdmValue value);
bool idm_is_syntax(IdmValue value);
bool idm_is_string(IdmValue value);
bool idm_is_cell(IdmValue value);
bool idm_is_closure(IdmValue value);
bool idm_is_record(IdmValue value);
IdmValue idm_cell_get(IdmValue cell, IdmError *err);
bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err);
uint32_t idm_closure_function_index(IdmValue value);
const IdmBytecodeModule *idm_closure_module(IdmValue value);
size_t idm_closure_entry_count(IdmValue value);
uint32_t idm_closure_entry(IdmValue value, size_t index, IdmError *err);
size_t idm_closure_capture_count(IdmValue value);
IdmValue idm_closure_capture(IdmValue value, size_t index, IdmError *err);
IdmNamespace *idm_closure_namespace(IdmValue value);
const char *idm_record_type(IdmValue value, IdmError *err);
IdmValue idm_record_fields(IdmValue value, IdmError *err);
bool idm_record_field(IdmValue value, IdmValue field, IdmValue *out, IdmError *err);
IdmValue idm_car(IdmValue pair, IdmError *err);
IdmValue idm_cdr(IdmValue pair, IdmError *err);
size_t idm_sequence_count(IdmValue value);
IdmValue idm_sequence_item(IdmValue value, size_t index, IdmError *err);
size_t idm_dict_count(IdmValue value);
bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out);
bool idm_dict_entry(IdmValue dict, size_t index, IdmValue *out_key, IdmValue *out_value);
const IdmSyntax *idm_syntax_get(IdmValue value, IdmError *err);
IdmSyntax *idm_syntax_value_get(IdmValue value);
const char *idm_string_bytes(IdmValue value);
size_t idm_string_length(IdmValue value);
bool idm_value_equal(IdmValue a, IdmValue b);
bool idm_value_write(IdmBuffer *buf, IdmValue value);

#endif
