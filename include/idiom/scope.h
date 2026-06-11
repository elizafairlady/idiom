#ifndef IDM_SCOPE_H
#define IDM_SCOPE_H

#include "idiom/common.h"

typedef uint32_t IdmScopeId;
typedef uint32_t IdmBindingId;

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
    IDM_BIND_SPACE_PROTOCOL
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
    IDM_BIND_METHOD
} IdmBindingKind;

typedef struct {
    char *name;
    int phase;
    IdmBindingSpace space;
    IdmBindingKind kind;
    IdmScopeSet scopes;
    IdmBindingId id;
    uint32_t payload;
    uint32_t frame_id;
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
void idm_binding_table_truncate(IdmBindingTable *table, size_t count);
IdmResolveStatus idm_binding_resolve(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding);

const char *idm_binding_space_name(IdmBindingSpace space);
const char *idm_binding_kind_name(IdmBindingKind kind);
const char *idm_resolve_status_name(IdmResolveStatus status);

#endif
