#include "idiom/scope.h"
#include "idiom/value.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define IDM_TYPE_TERM_MAX_DEPTH 128u
#define IDM_TYPE_TERM_MIN_WIRE 11u

typedef struct {
    atomic_size_t refs;
    IdmScopeId items[];
} ScopeSetData;

static ScopeSetData *scope_set_data(const IdmScopeSet *set) {
    return set && set->items ? (ScopeSetData *)((char *)set->items - offsetof(ScopeSetData, items)) : NULL;
}

static void scope_set_release(IdmScopeSet *set) {
    ScopeSetData *data = scope_set_data(set);
    if (data && atomic_fetch_sub_explicit(&data->refs, 1u, memory_order_acq_rel) == 1u) free(data);
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

static bool scope_set_unique(IdmScopeSet *set, size_t needed) {
    ScopeSetData *data = scope_set_data(set);
    if (data && atomic_load_explicit(&data->refs, memory_order_acquire) == 1u && set->cap >= needed) return true;
    size_t count = set->count;
    size_t cap = set->cap ? set->cap : 4u;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u) { cap = needed; break; }
        cap *= 2u;
    }
    if (cap > (SIZE_MAX - offsetof(ScopeSetData, items)) / sizeof(IdmScopeId)) return false;
    ScopeSetData *next = malloc(offsetof(ScopeSetData, items) + cap * sizeof(IdmScopeId));
    if (!next) return false;
    atomic_init(&next->refs, 1u);
    if (count != 0) memcpy(next->items, set->items, count * sizeof(*set->items));
    scope_set_release(set);
    set->items = next->items;
    set->count = count;
    set->cap = cap;
    return true;
}

void idm_scope_store_init(IdmScopeStore *store) {
    store->next_scope = 1u;
    store->shared = NULL;
}

void idm_scope_store_init_shared(IdmScopeStore *store, IdmScopeId *shared) {
    store->next_scope = 0u;
    store->shared = shared;
}

IdmScopeId idm_scope_fresh(IdmScopeStore *store) {
    if (store->shared) return (*store->shared)++;
    return store->next_scope++;
}

IdmScopeId idm_scope_store_next(const IdmScopeStore *store) {
    return store->shared ? *store->shared : store->next_scope;
}

IdmScopeId idm_scope_reserve(IdmScopeStore *store, IdmScopeId count) {
    if (store->shared) {
        IdmScopeId base = *store->shared;
        *store->shared += count;
        return base;
    }
    IdmScopeId base = store->next_scope;
    store->next_scope += count;
    return base;
}

void idm_scope_store_bump_to(IdmScopeStore *store, IdmScopeId floor) {
    if (store->shared) {
        if (*store->shared < floor) *store->shared = floor;
        return;
    }
    if (store->next_scope < floor) store->next_scope = floor;
}

void idm_scope_set_init(IdmScopeSet *set) {
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

void idm_scope_set_destroy(IdmScopeSet *set) {
    if (!set) return;
    scope_set_release(set);
}

bool idm_scope_set_copy(IdmScopeSet *dst, const IdmScopeSet *src) {
    idm_scope_set_init(dst);
    if (src->count == 0) return true;
    ScopeSetData *data = scope_set_data(src);
    if (!data) return false;
    atomic_fetch_add_explicit(&data->refs, 1u, memory_order_relaxed);
    dst->items = src->items;
    dst->count = src->count;
    dst->cap = src->cap;
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
    size_t count = set->count;
    if (!scope_set_unique(set, count + 1u)) return false;
    memmove(set->items + index + 1u, set->items + index, (set->count - index) * sizeof(*set->items));
    set->items[index] = scope;
    set->count++;
    return true;
}

bool idm_scope_set_remove(IdmScopeSet *set, IdmScopeId scope) {
    bool found = false;
    size_t index = lower_bound(set, scope, &found);
    if (!found) return false;
    size_t count = set->count;
    if (!scope_set_unique(set, count)) return false;
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
    if (a == b || (a->count == b->count && a->items == b->items)) return true;
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
    return a == b || (a->count == b->count && (a->items == b->items || a->count == 0 || memcmp(a->items, b->items, a->count * sizeof(*a->items)) == 0));
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

bool idm_scope_set_relocate(IdmScopeSet *set, IdmScopeId min_id, int64_t delta) {
    if (!set || set->count == 0 || delta == 0) return true;
    bool changes = false;
    for (size_t i = 0; i < set->count; i++) if (set->items[i] >= min_id) { changes = true; break; }
    if (!changes) return true;
    size_t count = set->count;
    if (!scope_set_unique(set, count)) return false;
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
    return true;
}

void idm_binding_table_init(IdmBindingTable *table) {
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
    table->data_free = NULL;
    table->index_heads = NULL;
    table->index_next = NULL;
    table->index_bucket_count = 0;
}

static uint32_t binding_name_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static bool binding_index_rebuild(IdmBindingTable *table, size_t bucket_count) {
    uint32_t *heads = calloc(bucket_count, sizeof(*heads));
    uint32_t *next = table->cap ? malloc(table->cap * sizeof(*next)) : NULL;
    if (!heads || (table->cap && !next)) {
        free(heads);
        free(next);
        return false;
    }
    free(table->index_heads);
    free(table->index_next);
    table->index_heads = heads;
    table->index_next = next;
    table->index_bucket_count = bucket_count;
    for (size_t i = 0; i < table->count; i++) {
        size_t b = binding_name_hash(table->items[i].name) & (bucket_count - 1u);
        next[i] = heads[b];
        heads[b] = (uint32_t)(i + 1u);
    }
    return true;
}

void idm_binding_table_set_data_free(IdmBindingTable *table, void (*data_free)(IdmBindingKind kind, void *)) {
    table->data_free = data_free;
}

void idm_binding_table_destroy(IdmBindingTable *table) {
    if (!table) return;
    for (size_t i = 0; i < table->count; i++) {
        free(table->items[i].name);
        idm_scope_set_destroy(&table->items[i].scopes);
        if (table->items[i].has_contract) idm_callable_contract_destroy(&table->items[i].contract);
        if (table->data_free && table->items[i].data && table->items[i].owns_data) table->data_free(table->items[i].kind, table->items[i].data);
    }
    free(table->items);
    free(table->index_heads);
    free(table->index_next);
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
    table->next_id = 1u;
    table->index_heads = NULL;
    table->index_next = NULL;
    table->index_bucket_count = 0;
}

IdmArity idm_arity_unknown(void) {
    IdmArity arity;
    arity.kind = IDM_ARITY_UNKNOWN;
    arity.min = 0;
    arity.max = 0;
    return arity;
}

IdmArity idm_arity_range(uint32_t min, uint32_t max) {
    IdmArity arity;
    arity.kind = IDM_ARITY_RANGE;
    arity.min = min;
    arity.max = max;
    return arity;
}

IdmArity idm_arity_exact(uint32_t exact) {
    return idm_arity_range(exact, exact);
}

bool idm_arity_add_exact(IdmArity *arity, uint32_t exact) {
    if (!arity) return false;
    if (arity->kind == IDM_ARITY_UNKNOWN) {
        *arity = idm_arity_exact(exact);
        return true;
    }
    if (exact < arity->min) arity->min = exact;
    if (exact > arity->max) arity->max = exact;
    return true;
}

bool idm_arity_merge(IdmArity *dst, const IdmArity *src) {
    if (!dst) return false;
    if (!src || src->kind == IDM_ARITY_UNKNOWN) return true;
    if (dst->kind == IDM_ARITY_UNKNOWN) {
        *dst = *src;
        return true;
    }
    if (src->min < dst->min) dst->min = src->min;
    if (src->max > dst->max) dst->max = src->max;
    return true;
}

bool idm_arity_accepts(const IdmArity *arity, uint32_t argc) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN) return false;
    if (argc < arity->min || argc > arity->max) return false;
    return true;
}

bool idm_arity_equal(const IdmArity *a, const IdmArity *b) {
    if (!a || !b) return false;
    return a->kind == b->kind && a->min == b->min && a->max == b->max;
}

bool idm_arity_max_accepting_at_least(const IdmArity *arity, uint32_t min, uint32_t *out) {
    if (!arity || arity->kind == IDM_ARITY_UNKNOWN || arity->max < min) return false;
    if (out) *out = arity->max;
    return true;
}

bool idm_arity_describe(IdmBuffer *buf, const IdmArity *arity) {
    if (!arity) return idm_buf_append(buf, "<null>");
    if (arity->kind == IDM_ARITY_UNKNOWN) return idm_buf_append(buf, "?");
    if (arity->min == arity->max) return idm_buf_appendf(buf, "%u", arity->min);
    return idm_buf_appendf(buf, "%u..%u", arity->min, arity->max);
}

static bool arity_valid(IdmArity arity) {
    if (arity.kind == IDM_ARITY_UNKNOWN) return arity.min == 0 && arity.max == 0;
    return arity.kind == IDM_ARITY_RANGE && arity.min <= arity.max;
}

bool idm_arity_serialize(IdmBuffer *out, IdmArity arity, IdmError *err) {
    if (!arity_valid(arity)) return idm_error_set(err, idm_span_unknown(NULL), "invalid arity");
    if (!idm_buf_put_u8(out, (uint8_t)arity.kind) ||
        !idm_buf_put_u32(out, arity.min) ||
        !idm_buf_put_u32(out, arity.max)) {
        return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    return true;
}

bool idm_arity_deserialize(IdmByteReader *r, IdmArity *out, IdmError *err) {
    uint8_t kind = idm_rd_u8(r);
    uint32_t min = idm_rd_u32(r);
    uint32_t max = idm_rd_u32(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated arity");
    if (kind > (uint8_t)IDM_ARITY_RANGE) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid arity kind");
    }
    IdmArity arity = {(IdmArityKind)kind, min, max};
    if (!arity_valid(arity)) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid arity");
    }
    *out = arity;
    return true;
}

static bool binding_table_reserve(IdmBindingTable *table, size_t needed) {
    size_t old_cap = table->cap;
    if (needed > table->cap) {
        if (!idm_grow((void **)&table->items, &table->cap, sizeof(*table->items), 16u, needed)) return false;
    }
    if (table->cap != old_cap || table->index_bucket_count == 0 ||
        needed > table->index_bucket_count - (table->index_bucket_count >> 2)) {
        size_t bucket_count = table->index_bucket_count ? table->index_bucket_count : 64u;
        while (needed > bucket_count - (bucket_count >> 2)) bucket_count <<= 1;
        if (!binding_index_rebuild(table, bucket_count)) return false;
    }
    return true;
}

bool idm_binding_table_add_with_arity(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, IdmBindingId *out_id) {
    if (!binding_table_reserve(table, table->count + 1u)) return false;
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
    binding->has_contract = false;
    binding->owns_data = false;
    memset(&binding->contract, 0, sizeof(binding->contract));
    binding->provider = NULL;
    binding->referenced = false;
    size_t bucket = binding_name_hash(binding->name) & (table->index_bucket_count - 1u);
    table->index_next[table->count] = table->index_heads[bucket];
    table->index_heads[bucket] = (uint32_t)(table->count + 1u);
    table->count++;
    if (out_id) *out_id = binding->id;
    return true;
}

bool idm_binding_table_add(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmBindingId *out_id) {
    return idm_binding_table_add_with_arity(table, name, phase, space, kind, scopes, payload, frame_id, idm_arity_unknown(), out_id);
}

bool idm_binding_table_add_biphase_with_arity(IdmBindingTable *table, const char *name, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, uint32_t payload, uint32_t frame_id, IdmArity arity, IdmBindingId out_ids[2]) {
    size_t rollback = table->count;
    IdmBindingId ids[2];
    if (!idm_binding_table_add_with_arity(table, name, 0, space, kind, scopes, payload, frame_id, arity, &ids[0]) ||
        !idm_binding_table_add_with_arity(table, name, 1, space, kind, scopes, payload, frame_id, arity, &ids[1])) {
        idm_binding_table_truncate(table, rollback);
        return false;
    }
    if (out_ids) {
        out_ids[0] = ids[0];
        out_ids[1] = ids[1];
    }
    return true;
}

bool idm_binding_table_clone_phase_range(IdmBindingTable *table, size_t start, size_t end, int source_phase, int target_phase) {
    if (!table || start > end || end > table->count) return false;
    size_t rollback = table->count;
    size_t clone_count = 0;
    for (size_t i = start; i < end; i++) if (table->items[i].phase == source_phase) clone_count++;
    if (!binding_table_reserve(table, rollback + clone_count)) return false;
    for (size_t i = start; i < end; i++) {
        const IdmBinding *source = &table->items[i];
        if (source->phase != source_phase) continue;
        IdmBindingId id = 0;
        if (!idm_binding_table_add_with_arity(table, source->name, target_phase, source->space, source->kind, &source->scopes,
                                              source->payload, source->frame_id, source->arity, &id)) {
            idm_binding_table_truncate(table, rollback);
            return false;
        }
        IdmBinding *target = &table->items[table->count - 1u];
        target->data = source->data;
        target->provider = source->provider;
        if (source->has_contract && !idm_binding_table_set_contract(table, id, &source->contract)) {
            idm_binding_table_truncate(table, rollback);
            return false;
        }
    }
    return true;
}

void idm_binding_mark_referenced(const IdmBinding *binding) {
    ((IdmBinding *)binding)->referenced = true;
}

void idm_binding_table_truncate(IdmBindingTable *table, size_t count) {
    while (table->count > count) {
        table->count--;
        if (table->index_bucket_count) {
            size_t bucket = binding_name_hash(table->items[table->count].name) & (table->index_bucket_count - 1u);
            table->index_heads[bucket] = table->index_next[table->count];
        }
        free(table->items[table->count].name);
        idm_scope_set_destroy(&table->items[table->count].scopes);
        if (table->items[table->count].has_contract) idm_callable_contract_destroy(&table->items[table->count].contract);
        if (table->data_free && table->items[table->count].data && table->items[table->count].owns_data) table->data_free(table->items[table->count].kind, table->items[table->count].data);
    }
}

bool idm_binding_table_set_contract(IdmBindingTable *table, IdmBindingId id, const IdmCallableContract *contract) {
    if (!table) return false;
    size_t lo = 0, hi = table->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (table->items[mid].id < id) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= table->count || table->items[lo].id != id) return false;
    IdmBinding *binding = &table->items[lo];
    if (binding->has_contract) {
        idm_callable_contract_destroy(&binding->contract);
        binding->has_contract = false;
    }
    if (contract) {
        if (!idm_callable_contract_copy(&binding->contract, contract)) return false;
        binding->has_contract = true;
    }
    return true;
}

bool idm_binding_table_set_arity(IdmBindingTable *table, IdmBindingId id, IdmArity arity) {
    if (!table) return false;
    size_t lo = 0, hi = table->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (table->items[mid].id < id) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= table->count || table->items[lo].id != id) return false;
    table->items[lo].arity = arity;
    return true;
}

bool idm_binding_table_add_data(IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, IdmBindingKind kind, const IdmScopeSet *scopes, void *data, uint32_t frame_id, IdmBindingId *out_id) {
    if (!idm_binding_table_add_with_arity(table, name, phase, space, kind, scopes, 0u, frame_id, idm_arity_unknown(), out_id)) return false;
    table->items[table->count - 1u].data = data;
    table->items[table->count - 1u].owns_data = true;
    return true;
}

static bool binding_candidate(const IdmBinding *candidate, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes) {
    if (candidate->phase != phase || candidate->space != space || strcmp(candidate->name, name) != 0) return false;
    return idm_scope_set_subset(&candidate->scopes, reference_scopes);
}

static bool binding_prefer_candidate_newest_first(const IdmBinding *candidate, const IdmBinding *best) {
    if (!best) return true;
    if (!idm_scope_set_subset(&best->scopes, &candidate->scopes)) return false;
    return !idm_scope_set_equal(&best->scopes, &candidate->scopes);
}

static IdmResolveStatus binding_resolve_best(const IdmBindingTable *table, const char *name, int phase, IdmBindingSpace space, const IdmScopeSet *reference_scopes, const IdmBinding **out_binding) {
    const IdmBinding *best = NULL;
    if (table->index_bucket_count == 0) {
        if (out_binding) *out_binding = NULL;
        return IDM_RESOLVE_UNBOUND;
    }
    size_t bucket = binding_name_hash(name) & (table->index_bucket_count - 1u);
    for (uint32_t cur = table->index_heads[bucket]; cur; cur = table->index_next[cur - 1u]) {
        const IdmBinding *candidate = &table->items[cur - 1u];
        if (!binding_candidate(candidate, name, phase, space, reference_scopes)) continue;
        if (binding_prefer_candidate_newest_first(candidate, best)) best = candidate;
    }
    if (!best) {
        if (out_binding) *out_binding = NULL;
        return IDM_RESOLVE_UNBOUND;
    }
    for (uint32_t cur = table->index_heads[bucket]; cur; cur = table->index_next[cur - 1u]) {
        const IdmBinding *candidate = &table->items[cur - 1u];
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
        case IDM_BIND_SPACE_CORE_FORM: return "core-form";
        case IDM_BIND_SPACE_READER_FORM: return "reader-form";
        case IDM_BIND_SPACE_GRAMMAR: return "grammar";
        case IDM_BIND_SPACE_METHOD: return "method";
    }
    return "<bad-space>";
}

const char *idm_binding_kind_name(IdmBindingKind kind) {
    switch (kind) {
        case IDM_BIND_VALUE: return "value";
        case IDM_BIND_CORE_FORM: return "core-form";
        case IDM_BIND_READER_FORM: return "reader-form";
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

bool idm_type_var_symbol(IdmTypeTerm *out, IdmSymbol *symbol, uint32_t var_id, bool rigid) {
    memset(out, 0, sizeof(*out));
    out->kind = IDM_TYPE_VAR;
    out->var_id = var_id;
    out->rigid = rigid;
    out->symbol = symbol;
    return symbol != NULL;
}

bool idm_type_var(IdmRuntime *rt, IdmTypeTerm *out, const char *name, uint32_t var_id, bool rigid) {
    return rt && idm_type_var_symbol(out, idm_intern(&rt->intern, IDM_SYMBOL_WORD, name ? name : "_"), var_id, rigid);
}

bool idm_type_con_symbol(IdmTypeTerm *out, IdmSymbol *symbol) {
    memset(out, 0, sizeof(*out));
    out->kind = IDM_TYPE_CON;
    out->symbol = symbol;
    return symbol != NULL;
}

bool idm_type_con(IdmRuntime *rt, IdmTypeTerm *out, const char *name) {
    if (name && strcmp(name, "nil") == 0) name = "empty-list";
    return rt && idm_type_con_symbol(out, idm_intern(&rt->intern, IDM_SYMBOL_ATOM, name ? name : ""));
}

bool idm_type_con_take_symbol(IdmTypeTerm *out, IdmSymbol *symbol, IdmTypeTerm *args, size_t arg_count) {
    if (!idm_type_con_symbol(out, symbol)) return false;
    out->args = args;
    out->arg_count = arg_count;
    return true;
}

bool idm_type_con_take(IdmRuntime *rt, IdmTypeTerm *out, const char *name, IdmTypeTerm *args, size_t arg_count) {
    if (!idm_type_con(rt, out, name)) return false;
    out->args = args;
    out->arg_count = arg_count;
    return true;
}

bool idm_type_compound(IdmTypeTerm *out, IdmTypeKind kind, IdmTypeTerm *args, size_t arg_count) {
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->args = args;
    out->arg_count = arg_count;
    return true;
}

void idm_type_term_destroy(IdmTypeTerm *term) {
    if (!term) return;
    for (size_t i = 0; i < term->arg_count; i++) idm_type_term_destroy(&term->args[i]);
    free(term->args);
    memset(term, 0, sizeof(*term));
}

bool idm_type_term_copy(IdmTypeTerm *dst, const IdmTypeTerm *src) {
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    dst->kind = src->kind;
    dst->symbol = src->symbol;
    dst->var_id = src->var_id;
    dst->rigid = src->rigid;
    if (src->arg_count != 0) {
        dst->args = calloc(src->arg_count, sizeof(*dst->args));
        if (!dst->args) { idm_type_term_destroy(dst); return false; }
        dst->arg_count = src->arg_count;
        for (size_t i = 0; i < src->arg_count; i++) {
            if (!idm_type_term_copy(&dst->args[i], &src->args[i])) { idm_type_term_destroy(dst); return false; }
        }
    }
    return true;
}

bool idm_type_term_equal(const IdmTypeTerm *a, const IdmTypeTerm *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind || a->arg_count != b->arg_count) return false;
    if (a->kind == IDM_TYPE_VAR) return a->var_id == b->var_id;
    if (a->symbol != b->symbol) return false;
    for (size_t i = 0; i < a->arg_count; i++) {
        if (!idm_type_term_equal(&a->args[i], &b->args[i])) return false;
    }
    return true;
}

bool idm_type_term_mentions(const IdmTypeTerm *term, const char *var_name) {
    if (!term || !var_name) return false;
    if (term->kind == IDM_TYPE_VAR && strcmp(idm_type_term_text(term), var_name) == 0) return true;
    for (size_t i = 0; i < term->arg_count; i++) {
        if (idm_type_term_mentions(&term->args[i], var_name)) return true;
    }
    return false;
}

const char *idm_type_term_text(const IdmTypeTerm *term) {
    return term && term->symbol ? idm_symbol_text(term->symbol) : NULL;
}

static bool type_write_args(IdmBuffer *buf, const IdmTypeTerm *term, char open, char close, const char *sep) {
    if (!idm_buf_append_char(buf, open)) return false;
    for (size_t i = 0; i < term->arg_count; i++) {
        if (i != 0 && !idm_buf_append(buf, sep)) return false;
        if (!idm_type_term_write(buf, &term->args[i])) return false;
    }
    return idm_buf_append_char(buf, close);
}

bool idm_type_term_write(IdmBuffer *buf, const IdmTypeTerm *term) {
    if (!term) return idm_buf_append(buf, "_");
    switch (term->kind) {
        case IDM_TYPE_VAR:
            return idm_buf_append(buf, idm_type_term_text(term) ? idm_type_term_text(term) : "_");
        case IDM_TYPE_CON:
            if (!idm_buf_append(buf, idm_type_term_text(term) ? idm_type_term_text(term) : "_")) return false;
            if (term->arg_count == 0) return true;
            return type_write_args(buf, term, '<', '>', " ");
        case IDM_TYPE_TUPLE:
            return type_write_args(buf, term, '{', '}', " ");
        case IDM_TYPE_VECTOR:
            return type_write_args(buf, term, '[', ']', " ");
        case IDM_TYPE_UNION:
            return type_write_args(buf, term, '(', ')', " | ");
    }
    return false;
}

static bool type_term_kind_valid(uint8_t kind) {
    return kind <= (uint8_t)IDM_TYPE_UNION;
}

static bool type_term_serialize_depth(IdmBuffer *out, const IdmTypeTerm *term, unsigned depth, IdmError *err) {
    if (!term) return idm_error_set(err, idm_span_unknown(NULL), "cannot serialize null type term");
    if (depth > IDM_TYPE_TERM_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "type term nested too deeply to serialize");
    if (!type_term_kind_valid((uint8_t)term->kind)) return idm_error_set(err, idm_span_unknown(NULL), "invalid type term kind");
    if (term->arg_count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "type term has too many arguments");
    const char *text = idm_type_term_text(term);
    if (!idm_buf_put_u8(out, (uint8_t)term->kind) ||
        !idm_buf_put_opt_str(out, text) ||
        (text && !idm_buf_put_u8(out, (uint8_t)idm_symbol_kind(term->symbol))) ||
        (text && idm_symbol_kind(term->symbol) == IDM_SYMBOL_IDENTITY &&
         !idm_buf_append_n(out, (const char *)idm_symbol_identity_hash(term->symbol), 32u)) ||
        !idm_buf_put_u32(out, term->var_id) ||
        !idm_buf_put_u8(out, term->rigid ? 1u : 0u) ||
        !idm_buf_put_u32(out, (uint32_t)term->arg_count)) {
        return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    for (size_t i = 0; i < term->arg_count; i++) {
        if (!type_term_serialize_depth(out, &term->args[i], depth + 1u, err)) return false;
    }
    return true;
}

bool idm_type_term_serialize(IdmBuffer *out, const IdmTypeTerm *term, IdmError *err) {
    return type_term_serialize_depth(out, term, 0u, err);
}

static bool type_term_deserialize_depth(IdmRuntime *rt, IdmByteReader *r, IdmTypeTerm *out, unsigned depth, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (depth > IDM_TYPE_TERM_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "serialized type term nested too deeply");
    uint8_t kind = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated type term");
    if (!type_term_kind_valid(kind)) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid type term kind");
    }
    char *name = NULL;
    if (!idm_rd_opt_str(r, &name, err)) return false;
    uint8_t symbol_kind = name ? idm_rd_u8(r) : 0u;
    unsigned char identity_hash[32];
    if (name && symbol_kind == (uint8_t)IDM_SYMBOL_IDENTITY) {
        if (r->ok && sizeof(identity_hash) <= r->len - r->pos) {
            memcpy(identity_hash, r->data + r->pos, sizeof(identity_hash));
            r->pos += sizeof(identity_hash);
        } else {
            r->ok = false;
        }
    }
    uint32_t var_id = idm_rd_u32(r);
    uint8_t rigid = idm_rd_u8(r);
    uint32_t arg_count = idm_rd_u32(r);
    if (!r->ok) {
        free(name);
        return idm_error_set(err, idm_span_unknown(NULL), "truncated type term");
    }
    if (name && symbol_kind > (uint8_t)IDM_SYMBOL_IDENTITY) {
        free(name);
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid type term symbol kind");
    }
    if (name && ((kind != (uint8_t)IDM_TYPE_VAR && kind != (uint8_t)IDM_TYPE_CON) ||
                 (kind == (uint8_t)IDM_TYPE_VAR && symbol_kind != (uint8_t)IDM_SYMBOL_WORD) ||
                 (kind == (uint8_t)IDM_TYPE_CON && symbol_kind != (uint8_t)IDM_SYMBOL_ATOM && symbol_kind != (uint8_t)IDM_SYMBOL_IDENTITY))) {
        free(name);
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "type term symbol kind does not match term kind");
    }
    if (rigid > 1u) {
        free(name);
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid type term rigid flag");
    }
    if (arg_count != 0 && (size_t)arg_count > (r->len - r->pos) / IDM_TYPE_TERM_MIN_WIRE) {
        free(name);
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "type term exceeds payload");
    }
    out->kind = (IdmTypeKind)kind;
    if (name) {
        const char *canonical = out->kind == IDM_TYPE_CON && strcmp(name, "nil") == 0 ? "empty-list" : name;
        out->symbol = symbol_kind == (uint8_t)IDM_SYMBOL_IDENTITY
            ? idm_intern_identity(&rt->intern, canonical, identity_hash)
            : idm_intern(&rt->intern, (IdmSymbolKind)symbol_kind, canonical);
        free(name);
        if (!out->symbol) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    out->var_id = var_id;
    out->rigid = rigid != 0;
    if (arg_count != 0) {
        out->args = calloc(arg_count, sizeof(*out->args));
        if (!out->args) {
            idm_type_term_destroy(out);
            return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        }
        out->arg_count = arg_count;
        for (uint32_t i = 0; i < arg_count; i++) {
            if (!type_term_deserialize_depth(rt, r, &out->args[i], depth + 1u, err)) {
                idm_type_term_destroy(out);
                return false;
            }
        }
    }
    return true;
}

bool idm_type_term_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmTypeTerm *out, IdmError *err) {
    return type_term_deserialize_depth(rt, r, out, 0u, err);
}

void idm_constraint_destroy(IdmConstraint *c) {
    if (!c) return;
    idm_type_term_destroy(&c->lhs);
    idm_type_term_destroy(&c->rhs);
    idm_structural_head_destroy(&c->structural);
    memset(c, 0, sizeof(*c));
}

void idm_structural_head_destroy(IdmStructuralHead *head) {
    if (!head) return;
    idm_type_term_destroy(&head->type);
    memset(head, 0, sizeof(*head));
}

bool idm_structural_head_copy(IdmStructuralHead *dst, const IdmStructuralHead *src) {
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    dst->field = src->field;
    dst->has_type = src->has_type;
    return !src->has_type || idm_type_term_copy(&dst->type, &src->type);
}

bool idm_structural_head_equal(const IdmStructuralHead *a, const IdmStructuralHead *b) {
    return a && b && a->field == b->field && a->has_type == b->has_type &&
           (!a->has_type || idm_type_term_equal(&a->type, &b->type));
}

bool idm_structural_head_write(IdmBuffer *out, const IdmStructuralHead *head) {
    if (!out || !head || !head->field || !idm_buf_append(out, "_.") || !idm_buf_append(out, idm_symbol_text(head->field))) return false;
    return !head->has_type || (idm_buf_append(out, "::") && idm_type_term_write(out, &head->type));
}

bool idm_structural_head_serialize(IdmBuffer *out, const IdmStructuralHead *head, IdmError *err) {
    if (!head || !head->field || idm_symbol_kind(head->field) != IDM_SYMBOL_ATOM || idm_symbol_text(head->field)[0] == '\0') return idm_error_set(err, idm_span_unknown(NULL), "structural constraint requires a field");
    if (head->has_type && (head->type.kind != IDM_TYPE_CON || !head->type.symbol)) return idm_error_set(err, idm_span_unknown(NULL), "structural constraint requires a field type name");
    if (!idm_buf_put_str(out, idm_symbol_text(head->field), strlen(idm_symbol_text(head->field))) ||
        !idm_buf_put_u8(out, head->has_type ? 1u : 0u)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    return !head->has_type || idm_type_term_serialize(out, &head->type, err);
}

bool idm_structural_head_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmStructuralHead *head, IdmError *err) {
    memset(head, 0, sizeof(*head));
    char *field = idm_rd_string(r, NULL);
    if (!field) return r->ok ? (err ? idm_error_oom(err, idm_span_unknown(NULL)) : false)
                              : idm_error_set(err, idm_span_unknown(NULL), "truncated structural constraint field");
    head->field = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, field);
    free(field);
    uint8_t has_type = idm_rd_u8(r);
    if (!head->field || idm_symbol_text(head->field)[0] == '\0' || !r->ok || has_type > 1u) {
        idm_structural_head_destroy(head);
        return idm_error_set(err, idm_span_unknown(NULL), "invalid structural constraint");
    }
    head->has_type = has_type != 0u;
    if (head->has_type && !idm_type_term_deserialize(rt, r, &head->type, err)) {
        idm_structural_head_destroy(head);
        return false;
    }
    if (head->has_type && (head->type.kind != IDM_TYPE_CON || !head->type.symbol)) {
        idm_structural_head_destroy(head);
        return idm_error_set(err, idm_span_unknown(NULL), "invalid structural constraint field type");
    }
    return true;
}

void idm_arg_mask_destroy(IdmArgMask *mask) {
    if (!mask) return;
    free(mask->rest);
    memset(mask, 0, sizeof(*mask));
}

bool idm_arg_mask_copy(IdmArgMask *dst, const IdmArgMask *src) {
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    dst->first = src->first;
    if (src->rest_count == 0) return true;
    dst->rest = calloc(src->rest_count, sizeof(*dst->rest));
    if (!dst->rest) return false;
    memcpy(dst->rest, src->rest, src->rest_count * sizeof(*dst->rest));
    dst->rest_count = src->rest_count;
    dst->rest_cap = src->rest_count;
    return true;
}

bool idm_arg_mask_set(IdmArgMask *mask, size_t index) {
    if (index < 64u) {
        mask->first |= UINT64_C(1) << index;
        return true;
    }
    size_t word = (index >> 6) - 1u;
    if (word == SIZE_MAX) return false;
    size_t needed = word + 1u;
    size_t old_count = mask->rest_count;
    if (needed > mask->rest_cap && !idm_grow((void **)&mask->rest, &mask->rest_cap, sizeof(*mask->rest), 1u, needed)) return false;
    if (needed > old_count) memset(mask->rest + old_count, 0, (needed - old_count) * sizeof(*mask->rest));
    if (needed > mask->rest_count) mask->rest_count = needed;
    mask->rest[word] |= UINT64_C(1) << (index & 63u);
    return true;
}

bool idm_arg_mask_test(const IdmArgMask *mask, size_t index) {
    if (!mask) return false;
    if (index < 64u) return (mask->first & (UINT64_C(1) << index)) != 0;
    size_t word = (index >> 6) - 1u;
    return word < mask->rest_count && (mask->rest[word] & (UINT64_C(1) << (index & 63u))) != 0;
}

static size_t arg_mask_word_count(uint64_t word) {
    size_t count = 0;
    while (word != 0) {
        word &= word - 1u;
        count++;
    }
    return count;
}

size_t idm_arg_mask_count(const IdmArgMask *mask) {
    if (!mask) return 0;
    size_t count = arg_mask_word_count(mask->first);
    for (size_t i = 0; i < mask->rest_count; i++) count += arg_mask_word_count(mask->rest[i]);
    return count;
}

static bool arg_mask_fits(const IdmArgMask *mask, size_t arg_count) {
    if (!mask) return true;
    if (arg_count < 64u) {
        uint64_t allowed = arg_count == 0 ? 0 : (UINT64_C(1) << arg_count) - 1u;
        return (mask->first & ~allowed) == 0 && mask->rest_count == 0;
    }
    size_t words = (arg_count - 1u) >> 6;
    if (mask->rest_count > words) return false;
    size_t tail = arg_count & 63u;
    if (tail == 0 || mask->rest_count < words) return true;
    uint64_t allowed = (UINT64_C(1) << tail) - 1u;
    return (mask->rest[words - 1u] & ~allowed) == 0;
}

bool idm_constraint_copy(IdmConstraint *dst, const IdmConstraint *src) {
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    if (!idm_type_term_copy(&dst->lhs, &src->lhs)) return false;
    if (!idm_type_term_copy(&dst->rhs, &src->rhs)) { idm_constraint_destroy(dst); return false; }
    dst->trait = src->trait;
    if (!idm_structural_head_copy(&dst->structural, &src->structural)) { idm_constraint_destroy(dst); return false; }
    return true;
}

bool idm_constraint_serialize(IdmBuffer *out, const IdmConstraint *constraint, IdmError *err) {
    if (!constraint) return idm_error_set(err, idm_span_unknown(NULL), "cannot serialize null constraint");
    if (constraint->kind > IDM_CONSTR_STRUCTURAL) return idm_error_set(err, idm_span_unknown(NULL), "invalid constraint kind");
    const char *trait = constraint->trait ? idm_symbol_text(constraint->trait) : NULL;
    if (!idm_buf_put_u8(out, (uint8_t)constraint->kind) ||
        !idm_buf_put_opt_str(out, trait) ||
        (trait && !idm_buf_put_u8(out, (uint8_t)idm_symbol_kind(constraint->trait))) ||
        (trait && idm_symbol_kind(constraint->trait) == IDM_SYMBOL_IDENTITY &&
         !idm_buf_append_n(out, (const char *)idm_symbol_identity_hash(constraint->trait), 32u))) {
        return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    if (!idm_type_term_serialize(out, &constraint->lhs, err) ||
        !idm_type_term_serialize(out, &constraint->rhs, err)) return false;
    return constraint->kind != IDM_CONSTR_STRUCTURAL || idm_structural_head_serialize(out, &constraint->structural, err);
}

bool idm_constraint_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmConstraint *constraint, IdmError *err) {
    memset(constraint, 0, sizeof(*constraint));
    uint8_t kind = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated constraint");
    if (kind > (uint8_t)IDM_CONSTR_STRUCTURAL) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid constraint kind");
    }
    constraint->kind = (IdmConstraintKind)kind;
    char *trait = NULL;
    if (!idm_rd_opt_str(r, &trait, err)) return false;
    if (trait) {
        uint8_t symbol_kind = idm_rd_u8(r);
        unsigned char identity_hash[32];
        if (symbol_kind != (uint8_t)IDM_SYMBOL_ATOM && symbol_kind != (uint8_t)IDM_SYMBOL_IDENTITY) r->ok = false;
        if (r->ok && symbol_kind == (uint8_t)IDM_SYMBOL_IDENTITY) {
            if (r->len - r->pos < sizeof(identity_hash)) r->ok = false;
            else {
                memcpy(identity_hash, r->data + r->pos, sizeof(identity_hash));
                r->pos += sizeof(identity_hash);
            }
        }
        constraint->trait = !r->ok ? NULL
            : symbol_kind == (uint8_t)IDM_SYMBOL_IDENTITY
                ? idm_intern_identity(&rt->intern, trait, identity_hash)
                : idm_intern(&rt->intern, (IdmSymbolKind)symbol_kind, trait);
        free(trait);
        if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "invalid constraint trait symbol");
        if (!constraint->trait) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    if (!idm_type_term_deserialize(rt, r, &constraint->lhs, err) ||
        !idm_type_term_deserialize(rt, r, &constraint->rhs, err)) {
        idm_constraint_destroy(constraint);
        return false;
    }
    if (constraint->kind == IDM_CONSTR_STRUCTURAL &&
        !idm_structural_head_deserialize(rt, r, &constraint->structural, err)) {
            idm_constraint_destroy(constraint);
            return false;
    }
    return true;
}

void idm_contract_sig_destroy(IdmContractSig *sig) {
    if (!sig) return;
    for (size_t i = 0; i < sig->arg_count; i++) idm_type_term_destroy(&sig->args[i]);
    free(sig->args);
    idm_type_term_destroy(&sig->result);
    idm_arg_mask_destroy(&sig->invoked);
    memset(sig, 0, sizeof(*sig));
}

bool idm_contract_sig_copy(IdmContractSig *dst, const IdmContractSig *src) {
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    if (src->arg_count) {
        dst->args = calloc(src->arg_count, sizeof(*dst->args));
        if (!dst->args) return false;
        dst->arg_count = src->arg_count;
        for (size_t i = 0; i < src->arg_count; i++) {
            if (!idm_type_term_copy(&dst->args[i], &src->args[i])) { idm_contract_sig_destroy(dst); return false; }
        }
    }
    dst->has_result = src->has_result;
    if (!idm_arg_mask_copy(&dst->invoked, &src->invoked)) { idm_contract_sig_destroy(dst); return false; }
    if (src->has_result && !idm_type_term_copy(&dst->result, &src->result)) { idm_contract_sig_destroy(dst); return false; }
    return true;
}

const IdmContractSig *idm_contract_sig_for(const IdmCallableContract *c, size_t argc) {
    if (!c) return NULL;
    for (size_t i = 0; i < c->sig_count; i++) {
        if (c->sigs[i].arg_count == argc) return &c->sigs[i];
    }
    return NULL;
}

IdmContractSig *idm_contract_add_sig(IdmCallableContract *c) {
    if (!idm_grow((void **)&c->sigs, &c->sig_cap, sizeof(*c->sigs), 2u, c->sig_count + 1u)) return NULL;
    IdmContractSig *sig = &c->sigs[c->sig_count];
    memset(sig, 0, sizeof(*sig));
    c->sig_count++;
    return sig;
}

void idm_callable_contract_destroy(IdmCallableContract *c) {
    if (!c) return;
    for (size_t i = 0; i < c->quantified_count; i++) free(c->quantified[i]);
    free(c->quantified);
    for (size_t i = 0; i < c->context_count; i++) idm_constraint_destroy(&c->context[i]);
    free(c->context);
    for (size_t i = 0; i < c->sig_count; i++) idm_contract_sig_destroy(&c->sigs[i]);
    free(c->sigs);
    free(c->doc);
    memset(c, 0, sizeof(*c));
}

bool idm_callable_contract_copy(IdmCallableContract *dst, const IdmCallableContract *src) {
    memset(dst, 0, sizeof(*dst));
    if (!src) return true;
    if (src->quantified_count) {
        dst->quantified = calloc(src->quantified_count, sizeof(*dst->quantified));
        if (!dst->quantified) return false;
        dst->quantified_count = src->quantified_count;
        for (size_t i = 0; i < src->quantified_count; i++) {
            dst->quantified[i] = idm_strdup(src->quantified[i] ? src->quantified[i] : "");
            if (!dst->quantified[i]) { idm_callable_contract_destroy(dst); return false; }
        }
    }
    if (src->context_count) {
        dst->context = calloc(src->context_count, sizeof(*dst->context));
        if (!dst->context) { idm_callable_contract_destroy(dst); return false; }
        dst->context_count = src->context_count;
        for (size_t i = 0; i < src->context_count; i++) {
            if (!idm_constraint_copy(&dst->context[i], &src->context[i])) { idm_callable_contract_destroy(dst); return false; }
        }
    }
    dst->purity = src->purity;
    dst->passthrough = src->passthrough;
    dst->primitive = src->primitive;
    if (src->doc) {
        dst->doc = idm_strdup(src->doc);
        if (!dst->doc) { idm_callable_contract_destroy(dst); return false; }
    }
    if (src->sig_count) {
        dst->sigs = calloc(src->sig_count, sizeof(*dst->sigs));
        if (!dst->sigs) { idm_callable_contract_destroy(dst); return false; }
        dst->sig_count = src->sig_count;
        for (size_t i = 0; i < src->sig_count; i++) {
            if (!idm_contract_sig_copy(&dst->sigs[i], &src->sigs[i])) { idm_callable_contract_destroy(dst); return false; }
        }
    }
    return true;
}

bool idm_callable_contract_serialize(IdmBuffer *out, const IdmCallableContract *contract, IdmError *err) {
    if (!contract) return idm_error_set(err, idm_span_unknown(NULL), "cannot serialize null callable contract");
    if (contract->quantified_count > UINT32_MAX || contract->context_count > UINT32_MAX || contract->sig_count > UINT32_MAX) {
        return idm_error_set(err, idm_span_unknown(NULL), "callable contract is too large");
    }
    if (!idm_buf_put_u32(out, (uint32_t)contract->quantified_count)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    for (size_t i = 0; i < contract->quantified_count; i++) {
        const char *name = contract->quantified[i] ? contract->quantified[i] : "";
        if (!idm_buf_put_str(out, name, strlen(name))) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    }
    if (!idm_buf_put_u32(out, (uint32_t)contract->context_count)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    for (size_t i = 0; i < contract->context_count; i++) {
        if (!idm_constraint_serialize(out, &contract->context[i], err)) return false;
    }
    if (!idm_buf_put_u32(out, (uint32_t)contract->sig_count)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    for (size_t i = 0; i < contract->sig_count; i++) {
        const IdmContractSig *sig = &contract->sigs[i];
        if (sig->arg_count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "callable contract is too large");
        if (!arg_mask_fits(&sig->invoked, sig->arg_count)) return idm_error_set(err, idm_span_unknown(NULL), "callable contract invocation mask exceeds signature arity");
        if (!idm_buf_put_u32(out, (uint32_t)sig->arg_count)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        for (size_t j = 0; j < sig->arg_count; j++) {
            if (!idm_type_term_serialize(out, &sig->args[j], err)) return false;
        }
        if (!idm_buf_put_u8(out, sig->has_result ? 1u : 0u)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        if (sig->has_result && !idm_type_term_serialize(out, &sig->result, err)) return false;
        if (!idm_buf_put_u64(out, sig->invoked.first)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        size_t rest_words = sig->arg_count > 64u ? (sig->arg_count - 1u) >> 6 : 0u;
        for (size_t j = 0; j < rest_words; j++) {
            uint64_t word = j < sig->invoked.rest_count ? sig->invoked.rest[j] : 0;
            if (!idm_buf_put_u64(out, word)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        }
    }
    if (!idm_buf_put_u8(out, contract->purity)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    if (!idm_buf_put_u8(out, contract->passthrough ? 1u : 0u)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    if (contract->passthrough && !idm_buf_put_u32(out, contract->primitive)) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
    return idm_buf_put_opt_str(out, contract->doc) || (err ? idm_error_oom(err, idm_span_unknown(NULL)) : false);
}

bool idm_callable_contract_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmCallableContract *contract, IdmError *err) {
    memset(contract, 0, sizeof(*contract));
    uint32_t quantified_count = idm_rd_u32(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract");
    if (quantified_count != 0) {
        contract->quantified = calloc(quantified_count, sizeof(*contract->quantified));
        if (!contract->quantified) return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
        contract->quantified_count = quantified_count;
        for (uint32_t i = 0; i < quantified_count; i++) {
            contract->quantified[i] = idm_rd_string(r, NULL);
            if (!contract->quantified[i]) {
                idm_callable_contract_destroy(contract);
                return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract quantifier");
            }
        }
    }
    uint32_t context_count = idm_rd_u32(r);
    if (!r->ok) { idm_callable_contract_destroy(contract); return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract"); }
    if (context_count != 0) {
        contract->context = calloc(context_count, sizeof(*contract->context));
        if (!contract->context) { idm_callable_contract_destroy(contract); return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false; }
        contract->context_count = context_count;
        for (uint32_t i = 0; i < context_count; i++) {
            if (!idm_constraint_deserialize(rt, r, &contract->context[i], err)) {
                idm_callable_contract_destroy(contract);
                return false;
            }
        }
    }
    uint32_t sig_count = idm_rd_u32(r);
    if (!r->ok) { idm_callable_contract_destroy(contract); return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract"); }
    if (sig_count != 0) {
        contract->sigs = calloc(sig_count, sizeof(*contract->sigs));
        if (!contract->sigs) { idm_callable_contract_destroy(contract); return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false; }
        contract->sig_count = sig_count;
        for (uint32_t i = 0; i < sig_count; i++) {
            IdmContractSig *sig = &contract->sigs[i];
            uint32_t arg_count = idm_rd_u32(r);
            if (!r->ok) { idm_callable_contract_destroy(contract); return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract signature"); }
            if (arg_count != 0) {
                sig->args = calloc(arg_count, sizeof(*sig->args));
                if (!sig->args) { idm_callable_contract_destroy(contract); return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false; }
                sig->arg_count = arg_count;
                for (uint32_t j = 0; j < arg_count; j++) {
                    if (!idm_type_term_deserialize(rt, r, &sig->args[j], err)) {
                        idm_callable_contract_destroy(contract);
                        return false;
                    }
                }
            }
            uint8_t has_result = idm_rd_u8(r);
            if (!r->ok || has_result > 1u) {
                r->ok = false;
                idm_callable_contract_destroy(contract);
                return idm_error_set(err, idm_span_unknown(NULL), "invalid callable contract result flag");
            }
            sig->has_result = has_result != 0;
            if (sig->has_result && !idm_type_term_deserialize(rt, r, &sig->result, err)) {
                idm_callable_contract_destroy(contract);
                return false;
            }
            sig->invoked.first = idm_rd_u64(r);
            if (!r->ok) {
                idm_callable_contract_destroy(contract);
                return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract invocation mask");
            }
            size_t rest_words = arg_count > 64u ? (arg_count - 1u) >> 6 : 0u;
            if (rest_words != 0) {
                sig->invoked.rest = calloc(rest_words, sizeof(*sig->invoked.rest));
                if (!sig->invoked.rest) {
                    idm_callable_contract_destroy(contract);
                    return err ? idm_error_oom(err, idm_span_unknown(NULL)) : false;
                }
                sig->invoked.rest_count = rest_words;
                sig->invoked.rest_cap = rest_words;
                for (size_t j = 0; j < rest_words; j++) sig->invoked.rest[j] = idm_rd_u64(r);
                if (!r->ok) {
                    idm_callable_contract_destroy(contract);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract invocation mask");
                }
            }
            if (!arg_mask_fits(&sig->invoked, sig->arg_count)) {
                idm_callable_contract_destroy(contract);
                return idm_error_set(err, idm_span_unknown(NULL), "callable contract invocation mask exceeds signature arity");
            }
        }
    }
    uint8_t purity = idm_rd_u8(r);
    if (!r->ok || purity > IDM_PURITY_CONST) {
        r->ok = false;
        idm_callable_contract_destroy(contract);
        return idm_error_set(err, idm_span_unknown(NULL), "invalid callable contract purity flag");
    }
    contract->purity = purity;
    uint8_t passthrough = idm_rd_u8(r);
    if (!r->ok || passthrough > 1u) {
        r->ok = false;
        idm_callable_contract_destroy(contract);
        return idm_error_set(err, idm_span_unknown(NULL), "invalid callable contract passthrough flag");
    }
    contract->passthrough = passthrough != 0;
    if (contract->passthrough) {
        contract->primitive = idm_rd_u32(r);
        if (!r->ok) {
            idm_callable_contract_destroy(contract);
            return idm_error_set(err, idm_span_unknown(NULL), "truncated callable contract primitive");
        }
    }
    if (!idm_rd_opt_str(r, &contract->doc, err)) {
        idm_callable_contract_destroy(contract);
        return false;
    }
    return true;
}
