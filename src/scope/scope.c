#include "ish/scope.h"

#include <stdlib.h>
#include <string.h>

void ish_scope_store_init(IshScopeStore *store) {
    store->next_scope = 1u;
}

IshScopeId ish_scope_fresh(IshScopeStore *store) {
    return store->next_scope++;
}

void ish_scope_set_init(IshScopeSet *set) {
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

void ish_scope_set_destroy(IshScopeSet *set) {
    if (!set) return;
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

bool ish_scope_set_copy(IshScopeSet *dst, const IshScopeSet *src) {
    ish_scope_set_init(dst);
    if (src->count == 0) return true;
    dst->items = malloc(src->count * sizeof(*dst->items));
    if (!dst->items) return false;
    memcpy(dst->items, src->items, src->count * sizeof(*dst->items));
    dst->count = src->count;
    dst->cap = src->count;
    return true;
}

static size_t lower_bound(const IshScopeSet *set, IshScopeId scope, bool *found) {
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

bool ish_scope_set_add(IshScopeSet *set, IshScopeId scope) {
    bool found = false;
    size_t index = lower_bound(set, scope, &found);
    if (found) return true;
    if (set->count == set->cap) {
        size_t cap = set->cap ? set->cap * 2u : 4u;
        IshScopeId *items = realloc(set->items, cap * sizeof(*items));
        if (!items) return false;
        set->items = items;
        set->cap = cap;
    }
    memmove(set->items + index + 1u, set->items + index, (set->count - index) * sizeof(*set->items));
    set->items[index] = scope;
    set->count++;
    return true;
}

bool ish_scope_set_remove(IshScopeSet *set, IshScopeId scope) {
    bool found = false;
    size_t index = lower_bound(set, scope, &found);
    if (!found) return false;
    memmove(set->items + index, set->items + index + 1u, (set->count - index - 1u) * sizeof(*set->items));
    set->count--;
    return true;
}

bool ish_scope_set_flip(IshScopeSet *set, IshScopeId scope) {
    if (ish_scope_set_contains(set, scope)) return ish_scope_set_remove(set, scope);
    return ish_scope_set_add(set, scope);
}

bool ish_scope_set_contains(const IshScopeSet *set, IshScopeId scope) {
    bool found = false;
    (void)lower_bound(set, scope, &found);
    return found;
}

bool ish_scope_set_subset(const IshScopeSet *a, const IshScopeSet *b) {
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

bool ish_scope_set_equal(const IshScopeSet *a, const IshScopeSet *b) {
    return a->count == b->count && (a->count == 0 || memcmp(a->items, b->items, a->count * sizeof(*a->items)) == 0);
}

bool ish_scope_set_write(IshBuffer *buf, const IshScopeSet *set) {
    if (!ish_buf_append_char(buf, '{')) return false;
    for (size_t i = 0; i < set->count; i++) {
        if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
        if (!ish_buf_appendf(buf, "%u", set->items[i])) return false;
    }
    return ish_buf_append_char(buf, '}');
}

void ish_binding_table_init(IshBindingTable *table) {
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
}

void ish_binding_table_destroy(IshBindingTable *table) {
    if (!table) return;
    for (size_t i = 0; i < table->count; i++) {
        free(table->items[i].name);
        ish_scope_set_destroy(&table->items[i].scopes);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
}

bool ish_binding_table_add(IshBindingTable *table, const char *name, int phase, IshBindingSpace space, IshBindingKind kind, const IshScopeSet *scopes, uint32_t payload, IshBindingId *out_id) {
    if (table->count == table->cap) {
        size_t cap = table->cap ? table->cap * 2u : 16u;
        IshBinding *items = realloc(table->items, cap * sizeof(*items));
        if (!items) return false;
        table->items = items;
        table->cap = cap;
    }
    IshBinding *binding = &table->items[table->count];
    binding->name = ish_strdup(name);
    if (!binding->name) return false;
    binding->phase = phase;
    binding->space = space;
    binding->kind = kind;
    if (!ish_scope_set_copy(&binding->scopes, scopes)) {
        free(binding->name);
        return false;
    }
    binding->id = table->next_id++;
    binding->payload = payload;
    table->count++;
    if (out_id) *out_id = binding->id;
    return true;
}

IshResolveStatus ish_binding_resolve(const IshBindingTable *table, const char *name, int phase, IshBindingSpace space, const IshScopeSet *reference_scopes, const IshBinding **out_binding) {
    const IshBinding *best = NULL;
    bool ambiguous = false;
    for (size_t i = 0; i < table->count; i++) {
        const IshBinding *candidate = &table->items[i];
        if (candidate->phase != phase || candidate->space != space || strcmp(candidate->name, name) != 0) continue;
        if (!ish_scope_set_subset(&candidate->scopes, reference_scopes)) continue;
        if (!best) {
            best = candidate;
            ambiguous = false;
            continue;
        }
        bool candidate_more_specific = ish_scope_set_subset(&best->scopes, &candidate->scopes);
        bool best_more_specific = ish_scope_set_subset(&candidate->scopes, &best->scopes);
        if (candidate_more_specific && !best_more_specific) {
            best = candidate;
            ambiguous = false;
        } else if (!candidate_more_specific && !best_more_specific) {
            ambiguous = true;
        }
    }
    if (!best) {
        if (out_binding) *out_binding = NULL;
        return ISH_RESOLVE_UNBOUND;
    }
    if (ambiguous) {
        if (out_binding) *out_binding = NULL;
        return ISH_RESOLVE_AMBIGUOUS;
    }
    if (out_binding) *out_binding = best;
    return ISH_RESOLVE_OK;
}

const char *ish_binding_space_name(IshBindingSpace space) {
    switch (space) {
        case ISH_BIND_SPACE_DEFAULT: return "default";
        case ISH_BIND_SPACE_PACKAGE: return "package";
        case ISH_BIND_SPACE_OPERATOR: return "operator";
        case ISH_BIND_SPACE_SHELL: return "shell";
        case ISH_BIND_SPACE_LABEL: return "label";
    }
    return "<bad-space>";
}

const char *ish_binding_kind_name(IshBindingKind kind) {
    switch (kind) {
        case ISH_BIND_VALUE: return "value";
        case ISH_BIND_CORE_FORM: return "core-form";
        case ISH_BIND_TRANSFORMER: return "transformer";
        case ISH_BIND_PACKAGE: return "package";
        case ISH_BIND_OPERATOR: return "operator";
        case ISH_BIND_SHELL_FORM: return "shell-form";
    }
    return "<bad-kind>";
}

const char *ish_resolve_status_name(IshResolveStatus status) {
    switch (status) {
        case ISH_RESOLVE_OK: return "ok";
        case ISH_RESOLVE_UNBOUND: return "unbound";
        case ISH_RESOLVE_AMBIGUOUS: return "ambiguous";
    }
    return "<bad-resolve-status>";
}
