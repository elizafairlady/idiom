#ifndef ISH_SCOPE_H
#define ISH_SCOPE_H

#include "ish/common.h"

typedef uint32_t IshScopeId;
typedef uint32_t IshBindingId;

typedef struct {
    IshScopeId next_scope;
} IshScopeStore;

typedef struct {
    IshScopeId *items;
    size_t count;
    size_t cap;
} IshScopeSet;

typedef enum {
    ISH_BIND_SPACE_DEFAULT,
    ISH_BIND_SPACE_PACKAGE,
    ISH_BIND_SPACE_OPERATOR,
    ISH_BIND_SPACE_SHELL,
    ISH_BIND_SPACE_LABEL,
    ISH_BIND_SPACE_PROTOCOL
} IshBindingSpace;

typedef enum {
    ISH_BIND_VALUE,
    ISH_BIND_CORE_FORM,
    ISH_BIND_TRANSFORMER,
    ISH_BIND_PACKAGE,
    ISH_BIND_OPERATOR,
    ISH_BIND_SHELL_FORM,
    ISH_BIND_LOCAL,
    ISH_BIND_ARG,
    ISH_BIND_GLOBAL,
    ISH_BIND_PROTOCOL,
    ISH_BIND_METHOD
} IshBindingKind;

typedef struct {
    char *name;
    int phase;
    IshBindingSpace space;
    IshBindingKind kind;
    IshScopeSet scopes;
    IshBindingId id;
    uint32_t payload;
    uint32_t frame_id;
} IshBinding;

typedef struct {
    IshBinding *items;
    size_t count;
    size_t cap;
    IshBindingId next_id;
} IshBindingTable;

typedef enum {
    ISH_RESOLVE_OK,
    ISH_RESOLVE_UNBOUND,
    ISH_RESOLVE_AMBIGUOUS
} IshResolveStatus;

void ish_scope_store_init(IshScopeStore *store);
IshScopeId ish_scope_fresh(IshScopeStore *store);

void ish_scope_set_init(IshScopeSet *set);
void ish_scope_set_destroy(IshScopeSet *set);
bool ish_scope_set_copy(IshScopeSet *dst, const IshScopeSet *src);
bool ish_scope_set_add(IshScopeSet *set, IshScopeId scope);
bool ish_scope_set_remove(IshScopeSet *set, IshScopeId scope);
bool ish_scope_set_flip(IshScopeSet *set, IshScopeId scope);
bool ish_scope_set_contains(const IshScopeSet *set, IshScopeId scope);
bool ish_scope_set_subset(const IshScopeSet *a, const IshScopeSet *b);
bool ish_scope_set_equal(const IshScopeSet *a, const IshScopeSet *b);
bool ish_scope_set_write(IshBuffer *buf, const IshScopeSet *set);
void ish_scope_set_relocate(IshScopeSet *set, IshScopeId min_id, int64_t delta);

void ish_binding_table_init(IshBindingTable *table);
void ish_binding_table_destroy(IshBindingTable *table);
bool ish_binding_table_add(IshBindingTable *table, const char *name, int phase, IshBindingSpace space, IshBindingKind kind, const IshScopeSet *scopes, uint32_t payload, uint32_t frame_id, IshBindingId *out_id);
void ish_binding_table_truncate(IshBindingTable *table, size_t count);
IshResolveStatus ish_binding_resolve(const IshBindingTable *table, const char *name, int phase, IshBindingSpace space, const IshScopeSet *reference_scopes, const IshBinding **out_binding);

const char *ish_binding_space_name(IshBindingSpace space);
const char *ish_binding_kind_name(IshBindingKind kind);
const char *ish_resolve_status_name(IshResolveStatus status);

#endif
