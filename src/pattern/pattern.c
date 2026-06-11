#include "idiom/pattern.h"

#include <stdlib.h>
#include <string.h>

static IdmPattern *pat_alloc(IdmPatternKind kind, IdmSpan span) {
    IdmPattern *pat = calloc(1u, sizeof(*pat));
    if (!pat) return NULL;
    pat->kind = kind;
    pat->span = span;
    return pat;
}

IdmPattern *idm_pat_wildcard(IdmSpan span) {
    return pat_alloc(IDM_PAT_WILDCARD, span);
}

IdmPattern *idm_pat_bind(const char *name, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_BIND, span);
    if (!pat) return NULL;
    pat->as.name = idm_strdup(name);
    if (!pat->as.name) { free(pat); return NULL; }
    return pat;
}

IdmPattern *idm_pat_pin(const char *name, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_PIN, span);
    if (!pat) return NULL;
    pat->as.name = idm_strdup(name);
    if (!pat->as.name) { free(pat); return NULL; }
    return pat;
}

IdmPattern *idm_pat_literal(IdmValue value, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_LITERAL, span);
    if (!pat) return NULL;
    pat->as.literal = value;
    return pat;
}

IdmPattern *idm_pat_pair(IdmPattern *left, IdmPattern *right, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_PAIR, span);
    if (!pat) return NULL;
    pat->as.pair.left = left;
    pat->as.pair.right = right;
    return pat;
}

IdmPattern *idm_pat_sequence(IdmPatternKind kind, IdmPattern **items, size_t count, IdmSpan span) {
    if (kind != IDM_PAT_LIST && kind != IDM_PAT_VECTOR && kind != IDM_PAT_TUPLE) return NULL;
    IdmPattern *pat = pat_alloc(kind, span);
    if (!pat) return NULL;
    pat->as.seq.items = items;
    pat->as.seq.count = count;
    return pat;
}

IdmPattern *idm_pat_sequence_rest(IdmPatternKind kind, IdmPattern **items, size_t count, IdmPattern *rest, IdmSpan span) {
    if (kind != IDM_PAT_VECTOR_REST && kind != IDM_PAT_TUPLE_REST) return NULL;
    IdmPattern *pat = pat_alloc(kind, span);
    if (!pat) return NULL;
    pat->as.seq_rest.items = items;
    pat->as.seq_rest.count = count;
    pat->as.seq_rest.rest = rest;
    return pat;
}

IdmPattern *idm_pat_dict(IdmDictPatternEntry *entries, size_t count, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_DICT, span);
    if (!pat) return NULL;
    pat->as.dict.entries = entries;
    pat->as.dict.count = count;
    return pat;
}

void idm_pat_free(IdmPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case IDM_PAT_BIND:
        case IDM_PAT_PIN:
            free(pat->as.name);
            break;
        case IDM_PAT_PAIR:
            idm_pat_free(pat->as.pair.left);
            idm_pat_free(pat->as.pair.right);
            break;
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) idm_pat_free(pat->as.seq.items[i]);
            free(pat->as.seq.items);
            break;
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) idm_pat_free(pat->as.seq_rest.items[i]);
            free(pat->as.seq_rest.items);
            idm_pat_free(pat->as.seq_rest.rest);
            break;
        case IDM_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) idm_pat_free(pat->as.dict.entries[i].pattern);
            free(pat->as.dict.entries);
            break;
        default:
            break;
    }
    free(pat);
}

IdmPattern *idm_pat_clone(const IdmPattern *pat) {
    if (!pat) return NULL;
    switch (pat->kind) {
        case IDM_PAT_WILDCARD:
            return idm_pat_wildcard(pat->span);
        case IDM_PAT_BIND:
            return idm_pat_bind(pat->as.name, pat->span);
        case IDM_PAT_PIN:
            return idm_pat_pin(pat->as.name, pat->span);
        case IDM_PAT_LITERAL:
            return idm_pat_literal(pat->as.literal, pat->span);
        case IDM_PAT_PAIR: {
            IdmPattern *left = idm_pat_clone(pat->as.pair.left);
            IdmPattern *right = idm_pat_clone(pat->as.pair.right);
            if (!left || !right) {
                idm_pat_free(left);
                idm_pat_free(right);
                return NULL;
            }
            return idm_pat_pair(left, right, pat->span);
        }
        case IDM_PAT_LIST:
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            IdmPattern **items = NULL;
            if (pat->as.seq.count != 0) {
                items = calloc(pat->as.seq.count, sizeof(*items));
                if (!items) return NULL;
                for (size_t i = 0; i < pat->as.seq.count; i++) {
                    items[i] = idm_pat_clone(pat->as.seq.items[i]);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
            }
            return idm_pat_sequence(pat->kind, items, pat->as.seq.count, pat->span);
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            IdmPattern **items = NULL;
            if (pat->as.seq_rest.count != 0) {
                items = calloc(pat->as.seq_rest.count, sizeof(*items));
                if (!items) return NULL;
                for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                    items[i] = idm_pat_clone(pat->as.seq_rest.items[i]);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) idm_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
            }
            IdmPattern *rest = idm_pat_clone(pat->as.seq_rest.rest);
            if (!rest) {
                for (size_t i = 0; i < pat->as.seq_rest.count; i++) idm_pat_free(items[i]);
                free(items);
                return NULL;
            }
            return idm_pat_sequence_rest(pat->kind, items, pat->as.seq_rest.count, rest, pat->span);
        }
        case IDM_PAT_DICT: {
            IdmDictPatternEntry *entries = NULL;
            if (pat->as.dict.count != 0) {
                entries = calloc(pat->as.dict.count, sizeof(*entries));
                if (!entries) return NULL;
                for (size_t i = 0; i < pat->as.dict.count; i++) {
                    entries[i].key = pat->as.dict.entries[i].key;
                    entries[i].pattern = idm_pat_clone(pat->as.dict.entries[i].pattern);
                    if (!entries[i].pattern) {
                        for (size_t j = 0; j < i; j++) idm_pat_free(entries[j].pattern);
                        free(entries);
                        return NULL;
                    }
                }
            }
            return idm_pat_dict(entries, pat->as.dict.count, pat->span);
        }
    }
    return NULL;
}

void idm_pattern_bindings_init(IdmPatternBindings *bindings) {
    bindings->names = NULL;
    bindings->values = NULL;
    bindings->count = 0;
    bindings->cap = 0;
}

void idm_pattern_bindings_destroy(IdmPatternBindings *bindings) {
    if (!bindings) return;
    for (size_t i = 0; i < bindings->count; i++) free(bindings->names[i]);
    free(bindings->names);
    free(bindings->values);
    bindings->names = NULL;
    bindings->values = NULL;
    bindings->count = 0;
    bindings->cap = 0;
}

const IdmValue *idm_pattern_bindings_get(const IdmPatternBindings *bindings, const char *name) {
    for (size_t i = 0; i < bindings->count; i++) {
        if (strcmp(bindings->names[i], name) == 0) return &bindings->values[i];
    }
    return NULL;
}

bool idm_pattern_bindings_add(IdmPatternBindings *bindings, const char *name, IdmValue value) {
    const IdmValue *existing = idm_pattern_bindings_get(bindings, name);
    if (existing) return idm_value_equal(*existing, value);
    if (bindings->count == bindings->cap) {
        size_t cap = bindings->cap ? bindings->cap * 2u : 4u;
        char **names = realloc(bindings->names, cap * sizeof(*names));
        if (!names) return false;
        bindings->names = names;
        IdmValue *values = realloc(bindings->values, cap * sizeof(*values));
        if (!values) return false;
        bindings->values = values;
        bindings->cap = cap;
    }
    bindings->names[bindings->count] = idm_strdup(name);
    if (!bindings->names[bindings->count]) return false;
    bindings->values[bindings->count] = value;
    bindings->count++;
    return true;
}

static void bindings_truncate(IdmPatternBindings *bindings, size_t count) {
    while (bindings->count > count) free(bindings->names[--bindings->count]);
}

bool idm_pattern_match(IdmRuntime *rt, IdmPattern *pat, IdmValue value, IdmPatternBindings *bindings, IdmError *err) {
    if (!pat) return idm_error_set(err, idm_span_unknown(NULL), "cannot match null pattern");
    size_t checkpoint = bindings->count;
    bool ok = false;
    switch (pat->kind) {
        case IDM_PAT_WILDCARD:
            ok = true;
            break;
        case IDM_PAT_BIND:
        {
            const IdmValue *existing = idm_pattern_bindings_get(bindings, pat->as.name);
            if (existing) ok = idm_value_equal(*existing, value);
            else {
                ok = idm_pattern_bindings_add(bindings, pat->as.name, value);
                if (!ok) idm_error_oom(err, pat->span);
            }
            break;
        }
        case IDM_PAT_PIN: {
            const IdmValue *pinned = idm_pattern_bindings_get(bindings, pat->as.name);
            ok = pinned && idm_value_equal(*pinned, value);
            break;
        }
        case IDM_PAT_LITERAL:
            ok = idm_value_equal(pat->as.literal, value);
            break;
        case IDM_PAT_PAIR: {
            if (!idm_is_pair(value)) {
                ok = false;
                break;
            }
            IdmValue car = idm_car(value, err);
            IdmValue cdr = idm_cdr(value, err);
            if (err && err->present) {
                ok = false;
                break;
            }
            ok = idm_pattern_match(rt, pat->as.pair.left, car, bindings, err) &&
                 idm_pattern_match(rt, pat->as.pair.right, cdr, bindings, err);
            break;
        }
        case IDM_PAT_LIST: {
            IdmValue cur = value;
            ok = true;
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (!idm_is_pair(cur)) { ok = false; break; }
                IdmValue car = idm_car(cur, err);
                IdmValue cdr = idm_cdr(cur, err);
                if (err && err->present) { ok = false; break; }
                if (!idm_pattern_match(rt, pat->as.seq.items[i], car, bindings, err)) { ok = false; break; }
                cur = cdr;
            }
            ok = ok && idm_is_nil(cur);
            break;
        }
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            if ((pat->kind == IDM_PAT_VECTOR && !idm_is_vector(value)) || (pat->kind == IDM_PAT_TUPLE && !idm_is_tuple(value))) {
                ok = false;
                break;
            }
            if (idm_sequence_count(value) != pat->as.seq.count) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                IdmValue item = idm_sequence_item(value, i, err);
                if (err && err->present) { ok = false; break; }
                if (!idm_pattern_match(rt, pat->as.seq.items[i], item, bindings, err)) { ok = false; break; }
            }
            break;
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            bool want_vector = pat->kind == IDM_PAT_VECTOR_REST;
            if ((want_vector && !idm_is_vector(value)) || (!want_vector && !idm_is_tuple(value))) { ok = false; break; }
            size_t n = idm_sequence_count(value);
            if (n < pat->as.seq_rest.count) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                IdmValue item = idm_sequence_item(value, i, err);
                if (err && err->present) { ok = false; break; }
                if (!idm_pattern_match(rt, pat->as.seq_rest.items[i], item, bindings, err)) { ok = false; break; }
            }
            if (!ok) break;
            size_t rest_count = n - pat->as.seq_rest.count;
            IdmValue *rest_items = rest_count == 0 ? NULL : calloc(rest_count, sizeof(*rest_items));
            if (rest_count != 0 && !rest_items) return idm_error_oom(err, pat->span);
            for (size_t i = 0; i < rest_count; i++) rest_items[i] = idm_sequence_item(value, pat->as.seq_rest.count + i, err);
            if (!rt) {
                free(rest_items);
                return idm_error_set(err, pat->span, "runtime required for vector/tuple rest pattern");
            }
            IdmValue rest_value = want_vector ? idm_vector(rt, rest_items, rest_count, err) : idm_tuple(rt, rest_items, rest_count, err);
            free(rest_items);
            if (err && err->present) return false;
            ok = idm_pattern_match(rt, pat->as.seq_rest.rest, rest_value, bindings, err);
            break;
        }
        case IDM_PAT_DICT: {
            if (!idm_is_dict(value)) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                IdmValue item = idm_nil();
                if (!idm_dict_get(value, pat->as.dict.entries[i].key, &item)) { ok = false; break; }
                if (!idm_pattern_match(rt, pat->as.dict.entries[i].pattern, item, bindings, err)) { ok = false; break; }
            }
            break;
        }
    }
    if (!ok) bindings_truncate(bindings, checkpoint);
    return ok;
}
