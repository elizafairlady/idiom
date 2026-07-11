#ifndef IDM_SCOPE_H
#define IDM_SCOPE_H

#include "idiom/common.h"

typedef uint32_t IdmScopeId;
typedef uint32_t IdmBindingId;
typedef struct IdmSymbol IdmSymbol;

#define IDM_PHASE_ANY (-1)

typedef struct {
    IdmScopeId next_scope;
    IdmScopeId *shared;
} IdmScopeStore;

typedef struct {
    IdmScopeId *items;
    size_t count;
    size_t cap;
} IdmScopeSet;

typedef enum {
    IDM_BIND_SPACE_DEFAULT,
    IDM_BIND_SPACE_OPERATOR,
    IDM_BIND_SPACE_SHELL,
    IDM_BIND_SPACE_LABEL,
    IDM_BIND_SPACE_PROTOCOL,
    IDM_BIND_SPACE_TRAIT,
    IDM_BIND_SPACE_TYPE,
    IDM_BIND_SPACE_FIELD,
    IDM_BIND_SPACE_CORE_FORM,
    IDM_BIND_SPACE_READER_FORM,
    IDM_BIND_SPACE_GRAMMAR,
    IDM_BIND_SPACE_METHOD
} IdmBindingSpace;

typedef enum {
    IDM_BIND_VALUE,
    IDM_BIND_CORE_FORM,
    IDM_BIND_READER_FORM,
    IDM_BIND_TRANSFORMER,
    IDM_BIND_OPERATOR,
    IDM_BIND_SHELL_FORM,
    IDM_BIND_LOCAL,
    IDM_BIND_ARG,
    IDM_BIND_ENV,
    IDM_BIND_PACKAGE_SLOT,
    IDM_BIND_PROTOCOL,
    IDM_BIND_TRAIT,
    IDM_BIND_TYPE,
    IDM_BIND_FIELD,
    IDM_BIND_GRAMMAR,
    IDM_BIND_METHOD
} IdmBindingKind;

typedef enum {
    IDM_ARITY_UNKNOWN,
    IDM_ARITY_RANGE
} IdmArityKind;

typedef struct {
    IdmArityKind kind;
    uint32_t min;
    uint32_t max;
} IdmArity;

typedef enum {
    IDM_TYPE_VAR,
    IDM_TYPE_CON,
    IDM_TYPE_TUPLE,
    IDM_TYPE_VECTOR,
    IDM_TYPE_UNION
} IdmTypeKind;

typedef struct IdmTypeTerm {
    IdmTypeKind kind;
    char *name;
    IdmSymbol *symbol;
    uint32_t var_id;
    bool rigid;
    struct IdmTypeTerm *args;
    size_t arg_count;
} IdmTypeTerm;

typedef enum { IDM_CONSTR_EQ, IDM_CONSTR_CLASS } IdmConstraintKind;

typedef struct {
    IdmConstraintKind kind;
    IdmTypeTerm lhs;
    IdmTypeTerm rhs;
    char *trait;
} IdmConstraint;

typedef struct {
    uint64_t first;
    uint64_t *rest;
    size_t rest_count;
    size_t rest_cap;
} IdmArgMask;

typedef struct {
    IdmTypeTerm *args;
    size_t arg_count;
    size_t arg_cap;
    IdmTypeTerm result;
    bool has_result;
    IdmArgMask invoked;
} IdmContractSig;

typedef struct {
    char **quantified;
    size_t quantified_count;
    size_t quantified_cap;
    IdmConstraint *context;
    size_t context_count;
    size_t context_cap;
    IdmContractSig *sigs;
    size_t sig_count;
    size_t sig_cap;
    uint8_t purity;
    bool passthrough;
    uint32_t primitive;
    char *doc;
} IdmCallableContract;

#define IDM_PURITY_IMPURE 0u
#define IDM_PURITY_ARGS 1u
#define IDM_PURITY_PURE 2u
#define IDM_PURITY_CONST 3u

bool idm_type_var(IdmTypeTerm *out, const char *name, uint32_t var_id, bool rigid);
bool idm_type_con(IdmTypeTerm *out, const char *name);
bool idm_type_con_take(IdmTypeTerm *out, const char *name, IdmTypeTerm *args, size_t arg_count);
bool idm_type_compound(IdmTypeTerm *out, IdmTypeKind kind, IdmTypeTerm *args, size_t arg_count);
void idm_type_term_destroy(IdmTypeTerm *term);
bool idm_type_term_copy(IdmTypeTerm *dst, const IdmTypeTerm *src);
bool idm_type_term_equal(const IdmTypeTerm *a, const IdmTypeTerm *b);
bool idm_type_term_write(IdmBuffer *buf, const IdmTypeTerm *term);
bool idm_type_term_mentions(const IdmTypeTerm *term, const char *var_name);
bool idm_type_term_serialize(IdmBuffer *out, const IdmTypeTerm *term, IdmError *err);
bool idm_type_term_deserialize(IdmByteReader *r, IdmTypeTerm *out, IdmError *err);

void idm_constraint_destroy(IdmConstraint *c);
bool idm_constraint_copy(IdmConstraint *dst, const IdmConstraint *src);

void idm_arg_mask_destroy(IdmArgMask *mask);
bool idm_arg_mask_copy(IdmArgMask *dst, const IdmArgMask *src);
bool idm_arg_mask_set(IdmArgMask *mask, size_t index);
bool idm_arg_mask_test(const IdmArgMask *mask, size_t index);
size_t idm_arg_mask_count(const IdmArgMask *mask);

bool idm_contract_sig_copy(IdmContractSig *dst, const IdmContractSig *src);
const IdmContractSig *idm_contract_sig_for(const IdmCallableContract *c, size_t argc);
IdmContractSig *idm_contract_add_sig(IdmCallableContract *c);
void idm_callable_contract_destroy(IdmCallableContract *c);
bool idm_callable_contract_copy(IdmCallableContract *dst, const IdmCallableContract *src);
bool idm_callable_contract_serialize(IdmBuffer *out, const IdmCallableContract *contract, IdmError *err);
bool idm_callable_contract_deserialize(IdmByteReader *r, IdmCallableContract *contract, IdmError *err);

typedef struct {
    char *name;
    int phase;
    IdmBindingSpace space;
    IdmBindingKind kind;
    IdmScopeSet scopes;
    IdmBindingId id;
    uint32_t payload;
    void *data;
    uint32_t frame_id;
    IdmArity arity;
    bool has_contract;
    IdmCallableContract contract;
    const char *provider;
    bool referenced;
} IdmBinding;

typedef struct {
    IdmBinding *items;
    size_t count;
    size_t cap;
    IdmBindingId next_id;
    void (*data_free)(IdmBindingKind kind, void *);
    uint32_t *index_heads;
    uint32_t *index_next;
    size_t index_bucket_count;
} IdmBindingTable;

typedef enum {
    IDM_RESOLVE_OK,
    IDM_RESOLVE_UNBOUND,
    IDM_RESOLVE_AMBIGUOUS
} IdmResolveStatus;

void idm_scope_store_init(IdmScopeStore *store);
void idm_scope_store_init_shared(IdmScopeStore *store, IdmScopeId *shared);
IdmScopeId idm_scope_fresh(IdmScopeStore *store);
IdmScopeId idm_scope_store_next(const IdmScopeStore *store);
IdmScopeId idm_scope_reserve(IdmScopeStore *store, IdmScopeId count);
void idm_scope_store_bump_to(IdmScopeStore *store, IdmScopeId floor);

void idm_scope_set_init(IdmScopeSet *set);
void idm_scope_set_destroy(IdmScopeSet *set);
bool idm_scope_set_copy(IdmScopeSet *dst, const IdmScopeSet *src);
bool idm_scope_set_add(IdmScopeSet *set, IdmScopeId scope);
bool idm_scope_set_remove(IdmScopeSet *set, IdmScopeId scope);
bool idm_scope_set_flip(IdmScopeSet *set, IdmScopeId scope);
bool idm_scope_set_contains(const IdmScopeSet *set, IdmScopeId scope);
bool idm_scope_set_subset(const IdmScopeSet *a, const IdmScopeSet *b);
bool idm_scope_set_equal(const IdmScopeSet *a, const IdmScopeSet *b);
bool idm_scope_set_write(IdmBuffer *buf, const IdmScopeSet *set);
bool idm_scope_set_serialize(IdmBuffer *out, const IdmScopeSet *set, IdmError *err);
bool idm_scope_set_deserialize(IdmByteReader *r, IdmScopeSet *set, IdmError *err);
void idm_scope_set_relocate(IdmScopeSet *set, IdmScopeId min_id, int64_t delta);

void idm_binding_table_init(IdmBindingTable *table);
void idm_binding_table_destroy(IdmBindingTable *table);
void idm_binding_table_set_data_free(IdmBindingTable *table, void (*data_free)(IdmBindingKind kind, void *));
bool idm_binding_table_add_data(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, void *data, uint32_t frame_id, IdmBindingId *out_id);
bool idm_binding_table_add(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmBindingId *out_id);
bool idm_binding_table_add_with_arity(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, IdmBindingId *out_id);
bool idm_binding_table_set_contract(IdmBindingTable *table, IdmBindingId id, const IdmCallableContract *contract);
bool idm_binding_table_set_arity(IdmBindingTable *table, IdmBindingId id, IdmArity arity);
void idm_binding_table_truncate(IdmBindingTable *table, size_t count);
void idm_binding_mark_referenced(const IdmBinding *binding);
IdmResolveStatus idm_binding_resolve(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding);

IdmArity idm_arity_unknown(void);
IdmArity idm_arity_range(uint32_t min, uint32_t max);
IdmArity idm_arity_exact(uint32_t arity);
bool idm_arity_add_exact(IdmArity *arity, uint32_t exact);
bool idm_arity_merge(IdmArity *dst, const IdmArity *src);
bool idm_arity_accepts(const IdmArity *arity, uint32_t argc);
bool idm_arity_equal(const IdmArity *a, const IdmArity *b);
bool idm_arity_max_accepting_at_least(const IdmArity *arity, uint32_t min, uint32_t *out);
bool idm_arity_describe(IdmBuffer *buf, const IdmArity *arity);
bool idm_arity_serialize(IdmBuffer *out, IdmArity arity, IdmError *err);
bool idm_arity_deserialize(IdmByteReader *r, IdmArity *out, IdmError *err);

const char *idm_binding_space_name(IdmBindingSpace space);
const char *idm_binding_kind_name(IdmBindingKind kind);
const char *idm_resolve_status_name(IdmResolveStatus status);

#endif
