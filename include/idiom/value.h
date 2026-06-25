#ifndef IDM_VALUE_H
#define IDM_VALUE_H

#include <pthread.h>
#include "idiom/common.h"
#include "idiom/scope.h"

typedef struct IdmObject IdmObject;
typedef struct IdmSymbol IdmSymbol;
typedef struct IdmSyntax IdmSyntax;
typedef struct IdmRuntime IdmRuntime;
typedef struct IdmBytecodeModule IdmBytecodeModule;
typedef struct IdmPatternSelector IdmPatternSelector;
typedef struct IdmExec IdmExec;
typedef struct IdmRepl IdmRepl;
typedef struct IdmRegex IdmRegex;
typedef struct IdmRegexResult IdmRegexResult;

typedef bool (*IdmLocalExpandFn)(void *user, IdmRuntime *rt, const IdmSyntax *syntax, IdmSyntax **out_syntax, IdmError *err);
typedef bool (*IdmFreeIdentifierEqFn)(void *user, IdmRuntime *rt, const IdmSyntax *a, const IdmSyntax *b, bool *out_equal, IdmError *err);

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
    IDM_VAL_EMPTY_LIST,
    IDM_VAL_TUPLE,
    IDM_VAL_VECTOR,
    IDM_VAL_DICT,
    IDM_VAL_SYNTAX,
    IDM_VAL_CELL,
    IDM_VAL_CLOSURE,
    IDM_VAL_PID,
    IDM_VAL_REF,
    IDM_VAL_PORT,
    IDM_VAL_RECORD,
    IDM_VAL_REGEX,
    IDM_VAL_REGEX_RESULT
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

typedef enum {
    IDM_VALUE_SEQ_VECTOR,
    IDM_VALUE_SEQ_TUPLE,
    IDM_VALUE_SEQ_DICT
} IdmValueSequenceKind;

typedef enum {
    IDM_SYNTAX_BUILD_NIL,
    IDM_SYNTAX_BUILD_WORD,
    IDM_SYNTAX_BUILD_ATOM,
    IDM_SYNTAX_BUILD_INT,
    IDM_SYNTAX_BUILD_FLOAT,
    IDM_SYNTAX_BUILD_STRING,
    IDM_SYNTAX_BUILD_LIST,
    IDM_SYNTAX_BUILD_VECTOR,
    IDM_SYNTAX_BUILD_TUPLE,
    IDM_SYNTAX_BUILD_DICT,
    IDM_SYNTAX_BUILD_EXPR,
    IDM_SYNTAX_BUILD_BODY,
    IDM_SYNTAX_BUILD_GROUP
} IdmSyntaxBuildKind;

typedef bool (*IdmRegisterMacroFn)(void *user, IdmRuntime *rt, const IdmSyntax *name, IdmValue transformer, IdmError *err);

typedef struct {
    const char *name;
} IdmTraitRequirementSpec;

typedef struct {
    const char *name;
    IdmArity arity;
    bool has_default;
    IdmValue default_impl;
} IdmTraitMethodSpec;

typedef struct {
    const char *name;
    IdmArity arity;
    IdmValue impl;
} IdmTraitImplSpec;

typedef struct {
    char *name;
    char **requirements;
    size_t requirement_count;
} IdmRuntimeTraitContract;

typedef struct {
    char *trait;
    char *method;
    IdmArity arity;
    bool has_default;
    IdmValue default_impl;
} IdmRuntimeTraitMethod;

typedef struct {
    char *trait;
    char *method;
    char *type;
    IdmValue impl;
} IdmRuntimeTraitImpl;

typedef struct {
    char *trait;
    char *type;
    char *provider;
    char *provider_key;
} IdmRuntimeTraitConformance;

typedef struct {
    uint64_t version;
    IdmRuntimeTraitContract *contracts;
    size_t contract_count;
    size_t contract_cap;
    IdmRuntimeTraitMethod *methods;
    size_t method_count;
    size_t method_cap;
    IdmRuntimeTraitImpl *impls;
    size_t impl_count;
    size_t impl_cap;
    IdmRuntimeTraitConformance *conformances;
    size_t conformance_count;
    size_t conformance_cap;
} IdmTraitWorld;

typedef struct {
    IdmSymbol **symbols;
    size_t count;
    size_t cap;
    uint32_t next_id;
    IdmSymbol **buckets;
    size_t bucket_count;
} IdmIntern;

typedef struct {
    IdmObject *objects;
    IdmObject *grey;
    size_t object_count;
    size_t bytes_allocated;
    pthread_mutex_t lock;
} IdmHeap;

typedef struct IdmEnv {
    char *package_key;
    IdmValue *slots;
    size_t slot_count;
    size_t slot_cap;
} IdmEnv;

typedef struct IdmPhaseReads IdmPhaseReads;

struct IdmRuntime {
    IdmIntern intern;
    IdmValue cached_true;
    IdmValue cached_false;
    IdmHeap heap;
    IdmHeap immortal;
    bool macro_intro_active;
    IdmScopeId macro_intro_scope;
    void *local_expand_user;
    IdmLocalExpandFn local_expand;
    void *free_identifier_eq_user;
    IdmFreeIdentifierEqFn free_identifier_eq;
    void *register_macro_user;
    IdmRegisterMacroFn register_macro;
    char **cli_args;
    size_t cli_arg_count;
    IdmEnv **envs;
    size_t env_count;
    size_t env_cap;
    IdmEnv *main_env;
    IdmTraitWorld trait_worlds[2];
    int trait_phase;
    void *expand_cache;
    void (*expand_cache_free)(void *cache);
    IdmPhaseReads *phase_reads;
    char **owned_temps;
    size_t owned_temp_count;
    size_t owned_temp_cap;
    IdmRepl *repl;
    bool interactive;
};

IdmEnv *idm_package_env_get_or_create(IdmRuntime *rt, const char *key);
IdmEnv *idm_env_fresh(IdmRuntime *rt);
bool idm_env_slot_ensure(IdmEnv *env, uint32_t id, IdmError *err);
void idm_env_slot_set(IdmEnv *env, uint32_t id, IdmValue value);
IdmValue idm_env_slot_get(const IdmEnv *env, uint32_t id);

void idm_runtime_init(IdmRuntime *rt);
void idm_runtime_destroy(IdmRuntime *rt);
void idm_phase_io_record(IdmRuntime *rt, const char *path);
bool idm_runtime_own_temp(IdmRuntime *rt, const char *path);

void idm_intern_init(IdmIntern *intern);
void idm_intern_destroy(IdmIntern *intern);
IdmSymbol *idm_intern(IdmIntern *intern, IdmSymbolKind kind, const char *text);
const char *idm_symbol_text(const IdmSymbol *sym);
uint32_t idm_symbol_id(const IdmSymbol *sym);
IdmSymbolKind idm_symbol_kind(const IdmSymbol *sym);

void idm_heap_init(IdmHeap *heap);
void idm_heap_destroy(IdmHeap *heap);
IdmHeap *idm_active_heap(IdmRuntime *rt);
void idm_set_active_heap(IdmHeap *heap);
void idm_heap_collect(IdmHeap *heap, const IdmValue *roots, size_t root_count);
void idm_gc_mark_value(IdmHeap *heap, IdmValue value);
void idm_heap_sweep(IdmHeap *heap);
IdmValue idm_value_copy(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err);
IdmValue idm_value_copy_locked(IdmRuntime *rt, IdmHeap *target, IdmValue value, IdmError *err);
size_t idm_heap_object_count(const IdmHeap *heap);
size_t idm_heap_bytes(const IdmHeap *heap);

IdmValue idm_nil(void);
IdmValue idm_empty_list(void);
IdmValue idm_int(int64_t value);
IdmValue idm_float(double value);
IdmValue idm_word(IdmRuntime *rt, const char *text);
IdmValue idm_atom(IdmRuntime *rt, const char *text);
IdmValue idm_bool(IdmRuntime *rt, bool value);
IdmValue idm_string(IdmRuntime *rt, const char *text, IdmError *err);
IdmValue idm_string_n(IdmRuntime *rt, const char *text, size_t len, IdmError *err);
bool idm_string_concat_display(IdmRuntime *rt, const IdmValue *items, size_t count, IdmValue *out, IdmError *err);
IdmValue idm_cons(IdmRuntime *rt, IdmValue car, IdmValue cdr, IdmError *err);
bool idm_list_append(IdmRuntime *rt, IdmValue head, IdmValue tail, IdmValue *out, IdmError *err);
IdmValue idm_tuple(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_vector(IdmRuntime *rt, const IdmValue *items, size_t count, IdmError *err);
IdmValue idm_dict(IdmRuntime *rt, const IdmDictEntry *entries, size_t count, IdmError *err);
IdmValue idm_syntax_value(IdmRuntime *rt, const IdmSyntax *syntax, IdmError *err);
bool idm_syntax_build(IdmRuntime *rt, IdmSyntaxBuildKind kind, IdmValue ctx_value, IdmValue payload, IdmValue *out, IdmError *err);
IdmValue idm_cell(IdmRuntime *rt, IdmValue initial, IdmError *err);
IdmValue idm_closure_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, uint32_t function_index, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err);
IdmValue idm_closure_multi_selectable_in_module(IdmRuntime *rt, const IdmBytecodeModule *module, const uint32_t *function_indexes, size_t function_count, IdmPatternSelector *selector, const IdmValue *captures, size_t capture_count, IdmEnv *env, IdmError *err);
IdmValue idm_record(IdmRuntime *rt, const char *type, IdmValue fields, IdmError *err);
IdmValue idm_regex_value(IdmRuntime *rt, IdmRegex *regex, IdmError *err);
IdmValue idm_regex_result_value(IdmRuntime *rt, IdmRegexResult *result, IdmError *err);
IdmValue idm_pid(uint64_t id);
IdmValue idm_ref(uint64_t id);
IdmValue idm_port(uint64_t id);
const char *idm_value_type_name(IdmValueTag tag);
const char *idm_value_dispatch_type_name(IdmValue value);
bool idm_value_type_from_name(const char *name, IdmValueTag *out);
bool idm_trait_define(IdmRuntime *rt, const char *trait, const IdmTraitRequirementSpec *requirements, size_t requirement_count, const IdmTraitMethodSpec *methods, size_t method_count, IdmError *err);
bool idm_trait_implement(IdmRuntime *rt, const char *trait, const char *type, const char *provider, const char *provider_key, const IdmTraitImplSpec *impls, size_t impl_count, IdmError *err);
bool idm_trait_implements(IdmRuntime *rt, const char *trait, const char *type);
bool idm_trait_lookup(IdmRuntime *rt, const char *trait, const char *method, const char *type, uint32_t argc, IdmValue *out_impl, IdmError *err);

bool idm_is_nil(IdmValue value);
bool idm_is_empty_list(IdmValue value);
bool idm_is_pair(IdmValue value);
bool idm_is_tuple(IdmValue value);
bool idm_is_vector(IdmValue value);
bool idm_is_dict(IdmValue value);
bool idm_is_syntax(IdmValue value);
bool idm_is_string(IdmValue value);
bool idm_is_cell(IdmValue value);
bool idm_is_closure(IdmValue value);
bool idm_is_record(IdmValue value);
bool idm_is_regex(IdmValue value);
bool idm_is_regex_result(IdmValue value);
IdmValue idm_cell_get(IdmValue cell, IdmError *err);
bool idm_cell_set(IdmValue cell, IdmValue value, IdmError *err);
uint32_t idm_closure_function_index(IdmValue value);
const IdmBytecodeModule *idm_closure_module(IdmValue value);
size_t idm_closure_entry_count(IdmValue value);
uint32_t idm_closure_entry(IdmValue value, size_t index, IdmError *err);
size_t idm_closure_capture_count(IdmValue value);
IdmValue idm_closure_capture(IdmValue value, size_t index, IdmError *err);
IdmEnv *idm_closure_env(IdmValue value);
IdmPatternSelector *idm_closure_selector(IdmValue value);
uint64_t idm_closure_selector_generation(IdmValue value);
const char *idm_record_type(IdmValue value, IdmError *err);
IdmValue idm_record_fields(IdmValue value, IdmError *err);
bool idm_record_field(IdmValue value, IdmValue field, IdmValue *out, IdmError *err);
IdmRegex *idm_regex_value_get(IdmValue value, IdmError *err);
IdmRegexResult *idm_regex_result_value_get(IdmValue value, IdmError *err);
IdmValue idm_car(IdmValue pair, IdmError *err);
IdmValue idm_cdr(IdmValue pair, IdmError *err);
size_t idm_sequence_count(IdmValue value);
IdmValue idm_sequence_item(IdmValue value, size_t index, IdmError *err);
bool idm_value_is_error(IdmValue value);
bool idm_value_ok(IdmValue value);
size_t idm_dict_count(IdmValue value);
bool idm_dict_get(IdmValue dict, IdmValue key, IdmValue *out);
bool idm_dict_entry(IdmValue dict, size_t index, IdmValue *out_key, IdmValue *out_value);
IdmValue idm_dict_put(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmValue value, IdmError *err);
IdmValue idm_dict_del(IdmRuntime *rt, IdmValue dict, IdmValue key, IdmError *err);
const IdmSyntax *idm_syntax_get(IdmValue value, IdmError *err);
IdmSyntax *idm_syntax_value_get(IdmValue value);
const char *idm_string_bytes(IdmValue value);
size_t idm_string_length(IdmValue value);
bool idm_value_equal(IdmValue a, IdmValue b);
bool idm_value_write(IdmBuffer *buf, IdmValue value);
IdmValue idm_error_value(IdmRuntime *rt, IdmValue detail);
bool idm_error_reason(IdmRuntime *rt, IdmError *err, const char *kind, size_t count, ...);
bool idm_error_take_reason(IdmError *err, IdmValue *out);
IdmValue idm_error_reason_value(IdmRuntime *rt, IdmError *err);
void idm_error_describe(IdmRuntime *rt, IdmValue reason, IdmBuffer *out);

#endif
