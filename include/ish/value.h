#ifndef ISH_VALUE_H
#define ISH_VALUE_H

#include "ish/common.h"
#include "ish/scope.h"

typedef struct IshObject IshObject;
typedef struct IshSymbol IshSymbol;
typedef struct IshSyntax IshSyntax;
typedef struct IshRuntime IshRuntime;
typedef struct IshBytecodeModule IshBytecodeModule;
typedef struct IshExec IshExec;

typedef bool (*IshLocalExpandFn)(void *user, IshRuntime *rt, const IshSyntax *syntax, IshSyntax **out_syntax, IshError *err);
typedef bool (*IshFreeIdentifierEqFn)(void *user, IshRuntime *rt, const IshSyntax *a, const IshSyntax *b, bool *out_equal, IshError *err);
typedef bool (*IshRegisterOperatorFn)(void *user, IshRuntime *rt, const IshSyntax *name, int64_t precedence, const char *assoc, const char *fixity, const IshSyntax *target, IshError *err);

typedef enum {
    ISH_SYMBOL_WORD,
    ISH_SYMBOL_ATOM
} IshSymbolKind;

typedef enum {
    ISH_VAL_NIL,
    ISH_VAL_ATOM,
    ISH_VAL_WORD,
    ISH_VAL_INT,
    ISH_VAL_FLOAT,
    ISH_VAL_STRING,
    ISH_VAL_PAIR,
    ISH_VAL_TUPLE,
    ISH_VAL_VECTOR,
    ISH_VAL_DICT,
    ISH_VAL_SYNTAX,
    ISH_VAL_CELL,
    ISH_VAL_CLOSURE,
    ISH_VAL_PID,
    ISH_VAL_REF,
    ISH_VAL_PORT,
    ISH_VAL_PRIMITIVE,
    ISH_VAL_RECORD
} IshValueTag;

typedef struct {
    IshValueTag tag;
    union {
        int64_t i;
        double f;
        IshObject *obj;
        IshSymbol *symbol;
        uint64_t id;
    } as;
} IshValue;

typedef struct {
    IshValue key;
    IshValue value;
} IshDictEntry;

typedef bool (*IshRegisterMacroFn)(void *user, IshRuntime *rt, const IshSyntax *name, IshValue transformer, IshError *err);
typedef bool (*IshExpanderSurfaceFn)(void *user, IshRuntime *rt, const char *kind, IshValue *out, IshError *err);

typedef struct {
    const char *name;
    uint32_t arity;
    bool has_default;
    IshValue default_impl;
} IshProtocolMethodSpec;

typedef struct {
    const char *name;
    uint32_t arity;
    IshValue impl;
} IshProtocolImplSpec;

typedef struct {
    char *protocol;
    char *method;
    uint32_t arity;
    bool has_default;
    IshValue default_impl;
} IshRuntimeProtocolMethod;

typedef struct {
    char *protocol;
    char *method;
    char *type;
    IshValue impl;
} IshRuntimeProtocolImpl;

typedef struct {
    char *protocol;
    char *type;
} IshRuntimeProtocolConformance;

typedef struct {
    IshSymbol **symbols;
    size_t count;
    size_t cap;
    uint32_t next_id;
} IshIntern;

typedef struct {
    IshObject *objects;
    size_t object_count;
    size_t bytes_allocated;
} IshHeap;

typedef struct IshNamespace {
    char *name;
    IshValue *slots;
    size_t slot_count;
    size_t slot_cap;
} IshNamespace;

struct IshRuntime {
    IshIntern intern;
    IshHeap heap;
    bool macro_intro_active;
    IshScopeId macro_intro_scope;
    void *local_expand_user;
    IshLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IshFreeIdentifierEqFn free_identifier_eq;
    void *register_operator_user;
    IshRegisterOperatorFn register_operator;
    void *register_macro_user;
    IshRegisterMacroFn register_macro;
    void *expander_surface_user;
    IshExpanderSurfaceFn expander_surface;
    IshNamespace **namespaces;
    size_t ns_count;
    size_t ns_cap;
    IshNamespace *main_ns;
    IshRuntimeProtocolMethod *protocol_methods;
    size_t protocol_method_count;
    size_t protocol_method_cap;
    IshRuntimeProtocolImpl *protocol_impls;
    size_t protocol_impl_count;
    size_t protocol_impl_cap;
    IshRuntimeProtocolConformance *protocol_conformances;
    size_t protocol_conformance_count;
    size_t protocol_conformance_cap;
    const IshBytecodeModule **gc_modules;
    size_t gc_module_count;
    size_t gc_module_cap;
    IshValue *gc_values;
    size_t gc_value_count;
    size_t gc_value_cap;
    void *expand_cache;
    void (*expand_cache_free)(void *cache);
    IshExec *current_exec;
    char **owned_temps;
    size_t owned_temp_count;
    size_t owned_temp_cap;
};

IshNamespace *ish_namespace_get_or_create(IshRuntime *rt, const char *name);
bool ish_ns_slot_ensure(IshNamespace *ns, uint32_t id, IshError *err);
void ish_ns_slot_set(IshNamespace *ns, uint32_t id, IshValue value);
IshValue ish_ns_slot_get(const IshNamespace *ns, uint32_t id);

void ish_runtime_init(IshRuntime *rt);
void ish_runtime_destroy(IshRuntime *rt);
bool ish_runtime_own_temp(IshRuntime *rt, const char *path);
bool ish_runtime_register_gc_module(IshRuntime *rt, const IshBytecodeModule *module);
void ish_runtime_unregister_gc_module(IshRuntime *rt, const IshBytecodeModule *module);
bool ish_runtime_register_gc_value(IshRuntime *rt, IshValue value);
void ish_runtime_unregister_gc_value(IshRuntime *rt, IshValue value);

void ish_intern_init(IshIntern *intern);
void ish_intern_destroy(IshIntern *intern);
IshSymbol *ish_intern(IshIntern *intern, IshSymbolKind kind, const char *text);
const char *ish_symbol_text(const IshSymbol *sym);
uint32_t ish_symbol_id(const IshSymbol *sym);
IshSymbolKind ish_symbol_kind(const IshSymbol *sym);

void ish_heap_init(IshHeap *heap);
void ish_heap_destroy(IshHeap *heap);
void ish_heap_collect(IshHeap *heap, const IshValue *roots, size_t root_count);
void ish_gc_mark_value(IshValue value);
void ish_heap_sweep(IshHeap *heap);
size_t ish_heap_object_count(const IshHeap *heap);
size_t ish_heap_bytes(const IshHeap *heap);

IshValue ish_nil(void);
IshValue ish_int(int64_t value);
IshValue ish_float(double value);
IshValue ish_word(IshRuntime *rt, const char *text);
IshValue ish_atom(IshRuntime *rt, const char *text);
IshValue ish_string(IshRuntime *rt, const char *text, IshError *err);
IshValue ish_string_n(IshRuntime *rt, const char *text, size_t len, IshError *err);
IshValue ish_cons(IshRuntime *rt, IshValue car, IshValue cdr, IshError *err);
IshValue ish_tuple(IshRuntime *rt, const IshValue *items, size_t count, IshError *err);
IshValue ish_vector(IshRuntime *rt, const IshValue *items, size_t count, IshError *err);
IshValue ish_dict(IshRuntime *rt, const IshDictEntry *entries, size_t count, IshError *err);
IshValue ish_syntax_value(IshRuntime *rt, const IshSyntax *syntax, IshError *err);
IshValue ish_cell(IshRuntime *rt, IshValue initial, IshError *err);
IshValue ish_closure(IshRuntime *rt, uint32_t function_index, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err);
IshValue ish_closure_multi(IshRuntime *rt, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err);
IshValue ish_closure_in_module(IshRuntime *rt, const IshBytecodeModule *module, uint32_t function_index, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err);
IshValue ish_closure_multi_in_module(IshRuntime *rt, const IshBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, const IshValue *captures, size_t capture_count, IshNamespace *ns, IshError *err);
IshValue ish_record(IshRuntime *rt, const char *type, IshValue fields, IshError *err);
IshValue ish_pid(uint64_t id);
IshValue ish_ref(uint64_t id);
IshValue ish_port(uint64_t id);
IshValue ish_primitive_value(uint32_t primitive);
bool ish_is_primitive(IshValue value);
const char *ish_value_type_name(IshValueTag tag);
const char *ish_value_dispatch_type_name(IshValue value);
bool ish_value_type_from_name(const char *name, IshValueTag *out);
bool ish_protocol_define(IshRuntime *rt, const char *protocol, const IshProtocolMethodSpec *methods, size_t method_count, IshError *err);
bool ish_protocol_extend(IshRuntime *rt, const char *protocol, const char *type, const IshProtocolImplSpec *impls, size_t impl_count, IshError *err);
bool ish_protocol_lookup(IshRuntime *rt, const char *protocol, const char *method, const char *type, uint32_t argc, IshValue *out_impl, IshError *err);
void ish_runtime_mark_protocol_roots(IshRuntime *rt);

bool ish_is_nil(IshValue value);
bool ish_is_pair(IshValue value);
bool ish_is_tuple(IshValue value);
bool ish_is_vector(IshValue value);
bool ish_is_dict(IshValue value);
bool ish_is_syntax(IshValue value);
bool ish_is_string(IshValue value);
bool ish_is_cell(IshValue value);
bool ish_is_closure(IshValue value);
bool ish_is_record(IshValue value);
IshValue ish_cell_get(IshValue cell, IshError *err);
bool ish_cell_set(IshValue cell, IshValue value, IshError *err);
uint32_t ish_closure_function_index(IshValue value);
const IshBytecodeModule *ish_closure_module(IshValue value);
size_t ish_closure_entry_count(IshValue value);
uint32_t ish_closure_entry(IshValue value, size_t index, IshError *err);
size_t ish_closure_capture_count(IshValue value);
IshValue ish_closure_capture(IshValue value, size_t index, IshError *err);
IshNamespace *ish_closure_namespace(IshValue value);
const char *ish_record_type(IshValue value, IshError *err);
IshValue ish_record_fields(IshValue value, IshError *err);
bool ish_record_field(IshValue value, IshValue field, IshValue *out, IshError *err);
IshValue ish_car(IshValue pair, IshError *err);
IshValue ish_cdr(IshValue pair, IshError *err);
size_t ish_sequence_count(IshValue value);
IshValue ish_sequence_item(IshValue value, size_t index, IshError *err);
size_t ish_dict_count(IshValue value);
bool ish_dict_get(IshValue dict, IshValue key, IshValue *out);
bool ish_dict_entry(IshValue dict, size_t index, IshValue *out_key, IshValue *out_value);
const IshSyntax *ish_syntax_get(IshValue value, IshError *err);
IshSyntax *ish_syntax_value_get(IshValue value);
const char *ish_string_bytes(IshValue value);
size_t ish_string_length(IshValue value);
bool ish_value_equal(IshValue a, IshValue b);
bool ish_value_write(IshBuffer *buf, IshValue value);

#endif
