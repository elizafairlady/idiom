#ifndef IDM_SCOPE_H
#define IDM_SCOPE_H

#include "idiom/common.h"

typedef uint32_t IdmScopeId;
typedef uint32_t IdmBindingId;

#define IDM_PHASE_ANY (-1)

typedef struct {
    IdmScopeId next_scope;
} IdmScopeStore;

typedef struct {
    IdmScopeId *items;
    size_t count;
    size_t cap;
} IdmScopeSet;

typedef enum {
    IDM_BIND_SPACE_DEFAULT,
    IDM_BIND_SPACE_PACKAGE,
    IDM_BIND_SPACE_OPERATOR,
    IDM_BIND_SPACE_SHELL,
    IDM_BIND_SPACE_LABEL,
    IDM_BIND_SPACE_PROTOCOL,
    IDM_BIND_SPACE_TRAIT,
    IDM_BIND_SPACE_TYPE,
    IDM_BIND_SPACE_GRAMMAR,
    IDM_BIND_SPACE_METHOD
} IdmBindingSpace;

typedef enum {
    IDM_BIND_VALUE,
    IDM_BIND_CORE_FORM,
    IDM_BIND_TRANSFORMER,
    IDM_BIND_PACKAGE,
    IDM_BIND_OPERATOR,
    IDM_BIND_SHELL_FORM,
    IDM_BIND_LOCAL,
    IDM_BIND_ARG,
    IDM_BIND_GLOBAL,
    IDM_BIND_PROTOCOL,
    IDM_BIND_TRAIT,
    IDM_BIND_TYPE,
    IDM_BIND_METHOD
} IdmBindingKind;

typedef enum {
    IDM_ARITY_UNKNOWN,
    IDM_ARITY_RANGE,
    IDM_ARITY_SET
} IdmArityKind;

typedef struct {
    IdmArityKind kind;
    uint32_t min;
    uint32_t max;
    uint64_t mask;
} IdmArity;

typedef struct {
    char *name;
    int phase;
    IdmBindingSpace space;
    IdmBindingKind kind;
    IdmScopeSet scopes;
    IdmBindingId id;
    uint32_t payload;
    uint32_t frame_id;
    IdmArity arity;
} IdmBinding;

typedef struct {
    IdmBinding *items;
    size_t count;
    size_t cap;
    IdmBindingId next_id;
} IdmBindingTable;

typedef enum {
    IDM_RESOLVE_OK,
    IDM_RESOLVE_UNBOUND,
    IDM_RESOLVE_AMBIGUOUS
} IdmResolveStatus;

void idm_scope_store_init(IdmScopeStore *store);
IdmScopeId idm_scope_fresh(IdmScopeStore *store);

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
void idm_scope_set_relocate(IdmScopeSet *set, IdmScopeId min_id, int64_t delta);

void idm_binding_table_init(IdmBindingTable *table);
void idm_binding_table_destroy(IdmBindingTable *table);
bool idm_binding_table_add(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmBindingId *out_id);
bool idm_binding_table_add_with_arity(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, IdmBindingId *out_id);
void idm_binding_table_truncate(IdmBindingTable *table, size_t count);
IdmResolveStatus idm_binding_resolve(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding);

IdmArity idm_arity_unknown(void);
IdmArity idm_arity_range(uint32_t min, uint32_t max);
IdmArity idm_arity_exact(uint32_t arity);
bool idm_arity_add_exact(IdmArity *arity, uint32_t exact);
bool idm_arity_accepts(const IdmArity *arity, uint32_t argc);
bool idm_arity_equal(const IdmArity *a, const IdmArity *b);
bool idm_arity_max_accepting_at_least(const IdmArity *arity, uint32_t min, uint32_t *out);
bool idm_arity_describe(IdmBuffer *buf, const IdmArity *arity);

const char *idm_binding_space_name(IdmBindingSpace space);
const char *idm_binding_kind_name(IdmBindingKind kind);
const char *idm_resolve_status_name(IdmResolveStatus status);

#endif
