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
        if (!idm_grow((void **)&set->items, &set->cap, sizeof(*set->items), 4u, set->count + 1u)) return false;
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

bool idm_scope_set_serialize(IdmBuffer *out, const IdmScopeSet *set, IdmError *err) {
    static const IdmScopeSet empty = {0};
    const IdmScopeSet *src = set ? set : &empty;
    if (src->count > UINT32_MAX || !idm_buf_put_u32(out, (uint32_t)src->count)) {
        return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    for (size_t i = 0; i < src->count; i++) {
        if (!idm_buf_put_u32(out, src->items[i])) {
            return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        }
    }
    return true;
}

bool idm_scope_set_deserialize(IdmByteReader *r, IdmScopeSet *set, IdmError *err) {
    idm_scope_set_init(set);
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return false;
    if ((size_t)count > (r->len - r->pos) / sizeof(uint32_t)) {
        return err ? idm_error_set(err, idm_span_unknown(NULL), "scope set exceeds payload") : false;
    }
    for (uint32_t i = 0; i < count; i++) {
        IdmScopeId id = idm_rd_u32(r);
        if (!r->ok) return false;
        if (!idm_scope_set_add(set, id)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    return true;
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
    table->data_free = NULL;
}

void idm_binding_table_set_data_free(IdmBindingTable *table, void (*data_free)(void *)) {
    table->data_free = data_free;
}

void idm_binding_table_destroy(IdmBindingTable *table) {
    if (!table) return;
    for (size_t i = 0; i < table->count; i++) {
        free(table->items[i].name);
        idm_scope_set_destroy(&table->items[i].scopes);
        if (table->data_free && table->items[i].data) table->data_free(table->items[i].data);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
}

IdmArity idm_arity_unknown(void) {
    IdmArity arity;
    arity.kind = IDM_ARITY_UNKNOWN;
    arity.min = 0;
    arity.max = 0;
    arity.mask = 0;
    return arity;
}

IdmArity idm_arity_range(uint32_t min, uint32_t max) {
    IdmArity arity;
    arity.kind = IDM_ARITY_RANGE;
    arity.min = min;
    arity.max = max;
    arity.mask = 0;
    return arity;
}

IdmArity idm_arity_exact(uint32_t exact) {
    IdmArity arity;
    arity.kind = exact < 64u ? IDM_ARITY_SET : IDM_ARITY_RANGE;
    arity.min = exact;
    arity.max = exact;
    arity.mask = exact < 64u ? (UINT64_C(1) << exact) : 0;
    return arity;
}

bool idm_arity_add_exact(IdmArity *arity, uint32_t exact) {
    if (!arity) return false;
    if (arity->kind == IDM_ARITY_UNKNOWN) {
        *arity = idm_arity_exact(exact);
        return true;
    }
    if (arity->kind == IDM_ARITY_RANGE) {
        if (exact < arity->min) arity->min = exact;
        if (exact > arity->max) arity->max = exact;
        return true;
    }
    if (exact < arity->min) arity->min = exact;
    if (exact > arity->max) arity->max = exact;
    if (exact < 64u) arity->mask |= UINT64_C(1) << exact;
    else arity->kind = IDM_ARITY_RANGE;
    return true;
}

bool idm_arity_merge(IdmArity *dst, const IdmArity *src) {
    if (!dst) return false;
    if (!src || src->kind == IDM_ARITY_UNKNOWN) return true;
    if (dst->kind == IDM_ARITY_UNKNOWN) {
        *dst = *src;
        return true;
    }
    if (src->kind == IDM_ARITY_SET) {
        for (uint32_t argc = src->min; argc <= src->max; argc++) {
            if (idm_arity_accepts(src, argc) && !idm_arity_add_exact(dst, argc)) return false;
            if (argc == UINT32_MAX) break;
        }
        return true;
    }
    if (dst->kind == IDM_ARITY_SET && src->max < 64u) {
        for (uint32_t argc = src->min; argc <= src->max; argc++) {
            if (!idm_arity_add_exact(dst, argc)) return false;
        }
        return true;
    }
    if (dst->kind == IDM_ARITY_SET) dst->kind = IDM_ARITY_RANGE;
    if (src->min < dst->min) dst->min = src->min;
    if (src->max > dst->max) dst->max = src->max;
    dst->mask = 0;
    return true;
}

bool idm_arity_accepts(const IdmArity *arity, uint32_t argc) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return false;
    if (argc < arity->min || argc > arity->max) return false;
    if (arity->kind == IDM_ARITY_RANGE) return true;
    if (argc >= 64u) return false;
    return (arity->mask & (UINT64_C(1) << argc)) != 0;
}

bool idm_arity_equal(const IdmArity *a, const IdmArity *b) {
    if (!a || !b) return false;
    return a->kind == b->kind && a->min == b->min && a->max == b->max && a->mask == b->mask;
}

bool idm_arity_max_accepting_at_least(const IdmArity *arity, uint32_t min, uint32_t *out) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN || arity->max < min) return false;
    if (arity->kind == IDM_ARITY_RANGE) {
        if (out) *out = arity->max;
        return true;
    }
    for (uint32_t argc = arity->max;; argc--) {
        if (argc >= min && idm_arity_accepts(arity, argc)) {
            if (out) *out = argc;
            return true;
        }
        if (argc == 0) break;
    }
    return false;
}

bool idm_arity_describe(IdmBuffer *buf, const IdmArity *arity) {
    if (!arity) return idm_buf_append(buf, "<null>");
    if (arity->kind == IDM_ARITY_UNKNOWN) return idm_buf_append(buf, "?");
    if (arity->kind == IDM_ARITY_RANGE) return idm_buf_appendf(buf, "%u..%u", arity->min, arity->max);
    bool first = true;
    for (uint32_t argc = arity->min; argc <= arity->max; argc++) {
        if (!idm_arity_accepts(arity, argc)) continue;
        if (!first && !idm_buf_append_char(buf, '|')) return false;
        if (!idm_buf_appendf(buf, "%u", argc)) return false;
        first = false;
        if (argc == UINT32_MAX) break;
    }
    return first ? idm_buf_append(buf, "<empty>") : true;
}

bool idm_arity_serialize(IdmBuffer *out, IdmArity arity, IdmError *err) {
    if (arity.kind < IDM_ARITY_UNKNOWN || arity.kind > IDM_ARITY_SET) return idm_error_set(err, idm_span_unknown(NULL), "invalid arity kind");
    if (!idm_buf_put_u8(out, (uint8_t)arity.kind) ||
        !idm_buf_put_u32(out, arity.min) ||
        !idm_buf_put_u32(out, arity.max) ||
        !idm_buf_put_u64(out, arity.mask)) {
        return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    return true;
}

bool idm_arity_deserialize(IdmByteReader *r, IdmArity *out, IdmError *err) {
    uint8_t kind = idm_rd_u8(r);
    uint32_t min = idm_rd_u32(r);
    uint32_t max = idm_rd_u32(r);
    uint64_t mask = idm_rd_u64(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated arity");
    if (kind > (uint8_t)IDM_ARITY_SET) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid arity kind");
    }
    out->kind = (IdmArityKind)kind;
    out->min = min;
    out->max = max;
    out->mask = mask;
    return true;
}

bool idm_binding_table_add_primitive_with_arity(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, uint32_t primitive, IdmBindingId *out_id) {
    if (table->count == table->cap) {
        if (!idm_grow((void **)&table->items, &table->cap, sizeof(*table->items), 16u, table->count + 1u)) return false;
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
    binding->data = NULL;
    binding->frame_id = frame_id;
    binding->arity = arity;
    binding->primitive_backed = true;
    binding->primitive = primitive;
    table->count++;
    if (out_id) *out_id = binding->id;
    return true;
}

bool idm_binding_table_add_with_arity(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, IdmBindingId *out_id) {
    if (!idm_binding_table_add_primitive_with_arity(table, name, phase, space, kind, scopes, payload, frame_id, arity, 0u, out_id)) return false;
    table->items[table->count - 1u].primitive_backed = false;
    return true;
}

bool idm_binding_table_add(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmBindingId *out_id) {
    return idm_binding_table_add_with_arity(table, name, phase, space, kind, scopes, payload, frame_id, idm_arity_unknown(), out_id);
}

void idm_binding_table_truncate(IdmBindingTable *table, size_t count) {
    while (table->count > count) {
        table->count--;
        free(table->items[table->count].name);
        idm_scope_set_destroy(&table->items[table->count].scopes);
        if (table->data_free && table->items[table->count].data) table->data_free(table->items[table->count].data);
    }
}

bool idm_binding_table_add_data(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, void *data, uint32_t frame_id, IdmBindingId *out_id) {
    if (!idm_binding_table_add_primitive_with_arity(table, name, phase, space, kind, scopes, 0u, frame_id, idm_arity_unknown(), 0u, out_id)) return false;
    table->items[table->count - 1u].data = data;
    return true;
}

static bool binding_candidate(const IdmBinding *candidate, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes) {
    if ((candidate->phase != phase && candidate->phase != IDM_PHASE_ANY) || candidate->space != space || strcmp(candidate->name, name) != 0) return false;
    return idm_scope_set_subset(&candidate->scopes, reference_scopes);
}

static int binding_tie_priority(const IdmBinding *binding) {
    switch (binding->kind) {
        case IDM_BIND_VALUE:
        case IDM_BIND_LOCAL:
        case IDM_BIND_ARG:
        case IDM_BIND_ENV:
            return 1;
        default:
            return 0;
    }
}

static bool binding_prefer_candidate(const IdmBinding *candidate, const IdmBinding *best) {
    if (!best) return true;
    if (!idm_scope_set_subset(&best->scopes, &candidate->scopes)) return false;
    if (!idm_scope_set_equal(&best->scopes, &candidate->scopes)) return true;
    int candidate_priority = binding_tie_priority(candidate);
    int best_priority = binding_tie_priority(best);
    return candidate_priority >= best_priority;
}

static IdmResolveStatus binding_resolve_best(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding) {
    const IdmBinding *best = NULL;
    for (size_t i = 0; i < table->count; i++) {
        const IdmBinding *candidate = &table->items[i];
        if (!binding_candidate(candidate, name, phase, space, reference_scopes)) continue;
        if (binding_prefer_candidate(candidate, best)) best = candidate;
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

IdmResolveStatus idm_binding_resolve(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding) {
    return binding_resolve_best(table, name, phase, space, reference_scopes, out_binding);
}

IdmResolveStatus idm_binding_resolve_scopes(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmScopeSet **out_scopes) {
    const IdmBinding *best = NULL;
    IdmResolveStatus status = binding_resolve_best(table, name, phase, space, reference_scopes, &best);
    if (out_scopes) *out_scopes = status == IDM_RESOLVE_OK && best ? &best->scopes : NULL;
    return status;
}

const char *idm_binding_space_name(IdmBindingSpace space) {
    switch (space) {
        case IDM_BIND_SPACE_DEFAULT: return "default";
        case IDM_BIND_SPACE_OPERATOR: return "operator";
        case IDM_BIND_SPACE_SHELL: return "shell";
        case IDM_BIND_SPACE_LABEL: return "label";
        case IDM_BIND_SPACE_PROTOCOL: return "protocol";
        case IDM_BIND_SPACE_TRAIT: return "trait";
        case IDM_BIND_SPACE_TYPE: return "type";
        case IDM_BIND_SPACE_FIELD: return "field";
        case IDM_BIND_SPACE_CORE_SYNTAX: return "core-syntax";
        case IDM_BIND_SPACE_GRAMMAR: return "grammar";
        case IDM_BIND_SPACE_METHOD: return "method";
    }
    return "<bad-space>";
}

const char *idm_binding_kind_name(IdmBindingKind kind) {
    switch (kind) {
        case IDM_BIND_VALUE: return "value";
        case IDM_BIND_CORE_FORM: return "core-form";
        case IDM_BIND_TRANSFORMER: return "transformer";
        case IDM_BIND_OPERATOR: return "operator";
        case IDM_BIND_SHELL_FORM: return "shell-form";
        case IDM_BIND_LOCAL: return "local";
        case IDM_BIND_ARG: return "arg";
        case IDM_BIND_ENV: return "env";
        case IDM_BIND_PACKAGE_SLOT: return "package-slot";
        case IDM_BIND_PROTOCOL: return "protocol";
        case IDM_BIND_TRAIT: return "trait";
        case IDM_BIND_TYPE: return "type";
        case IDM_BIND_FIELD: return "field";
        case IDM_BIND_GRAMMAR: return "grammar";
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
