#include "idiom/scope.h"

#include <stdlib.h>
#include <string.h>

void idm_scope_store_init(IdmScopeStore *store) {
    store->next_scope = 1u;
}

IdmScopeId idm_scope_fresh(IdmScopeStore *store) {
    return store->next_scope++;
}

void idm_scope_set_init(IdmScopeSet *set) {
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

void idm_scope_set_destroy(IdmScopeSet *set) {
    if (!set) return;
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

bool idm_scope_set_copy(IdmScopeSet *dst, const IdmScopeSet *src) {
    idm_scope_set_init(dst);
    if (src->count == 0) return true;
    dst->items = malloc(src->count * sizeof(*dst->items));
    if (!dst->items) return false;
    memcpy(dst->items, src->items, src->count * sizeof(*dst->items));
    dst->count = src->count;
    dst->cap = src->count;
    return true;
}

static size_t lower_bound(const IdmScopeSet *set, IdmScopeId scope, bool *found) {
    size_t lo = 0;
    size_t hi = set->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (set->items[mid] < scope) lo = mid + 1u;
        else hi = mid;
    }
    *found = lo < set->count && set->items[lo] == scope;
    return lo;
}

bool idm_scope_set_add(IdmScopeSet *set, IdmScopeId scope) {
    bool found = false;
    size_t index = lower_bound(set, scope, &found);
    if (found) return true;
    if (set->count == set->cap) {
        size_t cap = set->cap ? set->cap * 2u : 4u;
        IdmScopeId *items = realloc(set->items, cap * sizeof(*items));
        if (!items) return false;
        set->items = items;
        set->cap = cap;
    }
    memmove(set->items + index + 1u, set->items + index, (set->count - index) * sizeof(*set->items));
    set->items[index] = scope;
    set->count++;
    return true;
}

bool idm_scope_set_remove(IdmScopeSet *set, IdmScopeId scope) {
    bool found = false;
    size_t index = lower_bound(set, scope, &found);
    if (!found) return false;
    memmove(set->items + index, set->items + index + 1u, (set->count - index - 1u) * sizeof(*set->items));
    set->count--;
    return true;
}

bool idm_scope_set_flip(IdmScopeSet *set, IdmScopeId scope) {
    if (idm_scope_set_contains(set, scope)) return idm_scope_set_remove(set, scope);
    return idm_scope_set_add(set, scope);
}

bool idm_scope_set_contains(const IdmScopeSet *set, IdmScopeId scope) {
    bool found = false;
    (void)lower_bound(set, scope, &found);
    return found;
}

bool idm_scope_set_subset(const IdmScopeSet *a, const IdmScopeSet *b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a->count && j < b->count) {
        if (a->items[i] == b->items[j]) {
            i++;
            j++;
        } else if (a->items[i] > b->items[j]) {
            j++;
        } else {
            return false;
        }
    }
    return i == a->count;
}

bool idm_scope_set_equal(const IdmScopeSet *a, const IdmScopeSet *b) {
    return a->count == b->count && (a->count == 0 || memcmp(a->items, b->items, a->count * sizeof(*a->items)) == 0);
}

bool idm_scope_set_write(IdmBuffer *buf, const IdmScopeSet *set) {
    if (!idm_buf_append_char(buf, '{')) return false;
    for (size_t i = 0; i < set->count; i++) {
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!idm_buf_appendf(buf, "%u", set->items[i])) return false;
    }
    return idm_buf_append_char(buf, '}');
}

void idm_scope_set_relocate(IdmScopeSet *set, IdmScopeId min_id, int64_t delta) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i] >= min_id) set->items[i] = (IdmScopeId)((int64_t)set->items[i] + delta);
    }
    for (size_t i = 1; i < set->count; i++) {
        IdmScopeId key = set->items[i];
        size_t j = i;
        while (j > 0 && set->items[j - 1u] > key) {
            set->items[j] = set->items[j - 1u];
            j--;
        }
        set->items[j] = key;
    }
}

void idm_binding_table_init(IdmBindingTable *table) {
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
}

void idm_binding_table_destroy(IdmBindingTable *table) {
    if (!table) return;
    for (size_t i = 0; i < table->count; i++) {
        free(table->items[i].name);
        idm_scope_set_destroy(&table->items[i].scopes);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
}

bool idm_binding_table_add(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmBindingId *out_id) {
    if (table->count == table->cap) {
        size_t cap = table->cap ? table->cap * 2u : 16u;
        IdmBinding *items = realloc(table->items, cap * sizeof(*items));
        if (!items) return false;
        table->items = items;
        table->cap = cap;
    }
    IdmBinding *binding = &table->items[table->count];
    binding->name = idm_strdup(name);
    if (!binding->name) return false;
    binding->phase = phase;
    binding->space = space;
    binding->kind = kind;
    if (!idm_scope_set_copy(&binding->scopes, scopes)) {
        free(binding->name);
        return false;
    }
    binding->id = table->next_id++;
    binding->payload = payload;
    binding->frame_id = frame_id;
    table->count++;
    if (out_id) *out_id = binding->id;
    return true;
}

void idm_binding_table_truncate(IdmBindingTable *table, size_t count) {
    while (table->count > count) {
        table->count--;
        free(table->items[table->count].name);
        idm_scope_set_destroy(&table->items[table->count].scopes);
    }
}

static bool binding_candidate(const IdmBinding *candidate, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes) {
    if ((candidate->phase != phase && candidate->phase != IDM_PHASE_ANY) || candidate->space != space || strcmp(candidate->name, name) != 0) return false;
    return idm_scope_set_subset(&candidate->scopes, reference_scopes);
}

IdmResolveStatus idm_binding_resolve(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding) {
    const IdmBinding *best = NULL;
    for (size_t i = 0; i < table->count; i++) {
        const IdmBinding *candidate = &table->items[i];
        if (!binding_candidate(candidate, name, phase, space, reference_scopes)) continue;
        if (!best || idm_scope_set_subset(&best->scopes, &candidate->scopes)) best = candidate;
    }
    if (!best) {
        if (out_binding) *out_binding = NULL;
        return IDM_RESOLVE_UNBOUND;
    }
    for (size_t i = 0; i < table->count; i++) {
        const IdmBinding *candidate = &table->items[i];
        if (!binding_candidate(candidate, name, phase, space, reference_scopes)) continue;
        if (!idm_scope_set_subset(&candidate->scopes, &best->scopes)) {
            if (out_binding) *out_binding = NULL;
            return IDM_RESOLVE_AMBIGUOUS;
        }
    }
    if (out_binding) *out_binding = best;
    return IDM_RESOLVE_OK;
}

const char *idm_binding_space_name(IdmBindingSpace space) {
    switch (space) {
        case IDM_BIND_SPACE_DEFAULT: return "default";
        case IDM_BIND_SPACE_PACKAGE: return "package";
        case IDM_BIND_SPACE_OPERATOR: return "operator";
        case IDM_BIND_SPACE_SHELL: return "shell";
        case IDM_BIND_SPACE_LABEL: return "label";
        case IDM_BIND_SPACE_PROTOCOL: return "protocol";
    }
    return "<bad-space>";
}

const char *idm_binding_kind_name(IdmBindingKind kind) {
    switch (kind) {
        case IDM_BIND_VALUE: return "value";
        case IDM_BIND_CORE_FORM: return "core-form";
        case IDM_BIND_TRANSFORMER: return "transformer";
        case IDM_BIND_PACKAGE: return "package";
        case IDM_BIND_OPERATOR: return "operator";
        case IDM_BIND_SHELL_FORM: return "shell-form";
        case IDM_BIND_LOCAL: return "local";
        case IDM_BIND_ARG: return "arg";
        case IDM_BIND_GLOBAL: return "global";
        case IDM_BIND_PROTOCOL: return "protocol";
        case IDM_BIND_METHOD: return "method";
    }
    return "<bad-kind>";
}

const char *idm_resolve_status_name(IdmResolveStatus status) {
    switch (status) {
        case IDM_RESOLVE_OK: return "ok";
        case IDM_RESOLVE_UNBOUND: return "unbound";
        case IDM_RESOLVE_AMBIGUOUS: return "ambiguous";
    }
    return "<bad-resolve-status>";
}
