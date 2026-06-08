#include "ish/pattern.h"

#include <stdlib.h>
#include <string.h>

static IshPattern *pat_alloc(IshPatternKind kind, IshSpan span) {
    IshPattern *pat = calloc(1u, sizeof(*pat));
    if (!pat) return NULL;
    pat->kind = kind;
    pat->span = span;
    return pat;
}

IshPattern *ish_pat_wildcard(IshSpan span) {
    return pat_alloc(ISH_PAT_WILDCARD, span);
}

IshPattern *ish_pat_bind(const char *name, IshSpan span) {
    IshPattern *pat = pat_alloc(ISH_PAT_BIND, span);
    if (!pat) return NULL;
    pat->as.name = ish_strdup(name);
    if (!pat->as.name) { free(pat); return NULL; }
    return pat;
}

IshPattern *ish_pat_pin(const char *name, IshSpan span) {
    IshPattern *pat = pat_alloc(ISH_PAT_PIN, span);
    if (!pat) return NULL;
    pat->as.name = ish_strdup(name);
    if (!pat->as.name) { free(pat); return NULL; }
    return pat;
}

IshPattern *ish_pat_literal(IshValue value, IshSpan span) {
    IshPattern *pat = pat_alloc(ISH_PAT_LITERAL, span);
    if (!pat) return NULL;
    pat->as.literal = value;
    return pat;
}

IshPattern *ish_pat_pair(IshPattern *left, IshPattern *right, IshSpan span) {
    IshPattern *pat = pat_alloc(ISH_PAT_PAIR, span);
    if (!pat) return NULL;
    pat->as.pair.left = left;
    pat->as.pair.right = right;
    return pat;
}

IshPattern *ish_pat_sequence(IshPatternKind kind, IshPattern **items, size_t count, IshSpan span) {
    if (kind != ISH_PAT_LIST && kind != ISH_PAT_VECTOR && kind != ISH_PAT_TUPLE) return NULL;
    IshPattern *pat = pat_alloc(kind, span);
    if (!pat) return NULL;
    pat->as.seq.items = items;
    pat->as.seq.count = count;
    return pat;
}

IshPattern *ish_pat_sequence_rest(IshPatternKind kind, IshPattern **items, size_t count, IshPattern *rest, IshSpan span) {
    if (kind != ISH_PAT_VECTOR_REST && kind != ISH_PAT_TUPLE_REST) return NULL;
    IshPattern *pat = pat_alloc(kind, span);
    if (!pat) return NULL;
    pat->as.seq_rest.items = items;
    pat->as.seq_rest.count = count;
    pat->as.seq_rest.rest = rest;
    return pat;
}

IshPattern *ish_pat_dict(IshDictPatternEntry *entries, size_t count, IshSpan span) {
    IshPattern *pat = pat_alloc(ISH_PAT_DICT, span);
    if (!pat) return NULL;
    pat->as.dict.entries = entries;
    pat->as.dict.count = count;
    return pat;
}

void ish_pat_free(IshPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case ISH_PAT_BIND:
        case ISH_PAT_PIN:
            free(pat->as.name);
            break;
        case ISH_PAT_PAIR:
            ish_pat_free(pat->as.pair.left);
            ish_pat_free(pat->as.pair.right);
            break;
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) ish_pat_free(pat->as.seq.items[i]);
            free(pat->as.seq.items);
            break;
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) ish_pat_free(pat->as.seq_rest.items[i]);
            free(pat->as.seq_rest.items);
            ish_pat_free(pat->as.seq_rest.rest);
            break;
        case ISH_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) ish_pat_free(pat->as.dict.entries[i].pattern);
            free(pat->as.dict.entries);
            break;
        default:
            break;
    }
    free(pat);
}

IshPattern *ish_pat_clone(const IshPattern *pat) {
    if (!pat) return NULL;
    switch (pat->kind) {
        case ISH_PAT_WILDCARD:
            return ish_pat_wildcard(pat->span);
        case ISH_PAT_BIND:
            return ish_pat_bind(pat->as.name, pat->span);
        case ISH_PAT_PIN:
            return ish_pat_pin(pat->as.name, pat->span);
        case ISH_PAT_LITERAL:
            return ish_pat_literal(pat->as.literal, pat->span);
        case ISH_PAT_PAIR: {
            IshPattern *left = ish_pat_clone(pat->as.pair.left);
            IshPattern *right = ish_pat_clone(pat->as.pair.right);
            if (!left || !right) {
                ish_pat_free(left);
                ish_pat_free(right);
                return NULL;
            }
            return ish_pat_pair(left, right, pat->span);
        }
        case ISH_PAT_LIST:
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE: {
            IshPattern **items = NULL;
            if (pat->as.seq.count != 0) {
                items = calloc(pat->as.seq.count, sizeof(*items));
                if (!items) return NULL;
                for (size_t i = 0; i < pat->as.seq.count; i++) {
                    items[i] = ish_pat_clone(pat->as.seq.items[i]);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) ish_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
            }
            return ish_pat_sequence(pat->kind, items, pat->as.seq.count, pat->span);
        }
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST: {
            IshPattern **items = NULL;
            if (pat->as.seq_rest.count != 0) {
                items = calloc(pat->as.seq_rest.count, sizeof(*items));
                if (!items) return NULL;
                for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                    items[i] = ish_pat_clone(pat->as.seq_rest.items[i]);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) ish_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
            }
            IshPattern *rest = ish_pat_clone(pat->as.seq_rest.rest);
            if (!rest) {
                for (size_t i = 0; i < pat->as.seq_rest.count; i++) ish_pat_free(items[i]);
                free(items);
                return NULL;
            }
            return ish_pat_sequence_rest(pat->kind, items, pat->as.seq_rest.count, rest, pat->span);
        }
        case ISH_PAT_DICT: {
            IshDictPatternEntry *entries = NULL;
            if (pat->as.dict.count != 0) {
                entries = calloc(pat->as.dict.count, sizeof(*entries));
                if (!entries) return NULL;
                for (size_t i = 0; i < pat->as.dict.count; i++) {
                    entries[i].key = pat->as.dict.entries[i].key;
                    entries[i].pattern = ish_pat_clone(pat->as.dict.entries[i].pattern);
                    if (!entries[i].pattern) {
                        for (size_t j = 0; j < i; j++) ish_pat_free(entries[j].pattern);
                        free(entries);
                        return NULL;
                    }
                }
            }
            return ish_pat_dict(entries, pat->as.dict.count, pat->span);
        }
        case ISH_PAT_AS:
        case ISH_PAT_GUARD:
        case ISH_PAT_SYNTAX_ELLIPSIS:
            return NULL;
    }
    return NULL;
}

void ish_pattern_bindings_init(IshPatternBindings *bindings) {
    bindings->names = NULL;
    bindings->values = NULL;
    bindings->count = 0;
    bindings->cap = 0;
}

void ish_pattern_bindings_destroy(IshPatternBindings *bindings) {
    if (!bindings) return;
    for (size_t i = 0; i < bindings->count; i++) free(bindings->names[i]);
    free(bindings->names);
    free(bindings->values);
    bindings->names = NULL;
    bindings->values = NULL;
    bindings->count = 0;
    bindings->cap = 0;
}

const IshValue *ish_pattern_bindings_get(const IshPatternBindings *bindings, const char *name) {
    for (size_t i = 0; i < bindings->count; i++) {
        if (strcmp(bindings->names[i], name) == 0) return &bindings->values[i];
    }
    return NULL;
}

bool ish_pattern_bindings_add(IshPatternBindings *bindings, const char *name, IshValue value) {
    const IshValue *existing = ish_pattern_bindings_get(bindings, name);
    if (existing) return ish_value_equal(*existing, value);
    if (bindings->count == bindings->cap) {
        size_t cap = bindings->cap ? bindings->cap * 2u : 4u;
        char **names = realloc(bindings->names, cap * sizeof(*names));
        if (!names) return false;
        bindings->names = names;
        IshValue *values = realloc(bindings->values, cap * sizeof(*values));
        if (!values) return false;
        bindings->values = values;
        bindings->cap = cap;
    }
    bindings->names[bindings->count] = ish_strdup(name);
    if (!bindings->names[bindings->count]) return false;
    bindings->values[bindings->count] = value;
    bindings->count++;
    return true;
}

static void bindings_truncate(IshPatternBindings *bindings, size_t count) {
    while (bindings->count > count) free(bindings->names[--bindings->count]);
}

bool ish_pattern_match(IshRuntime *rt, IshPattern *pat, IshValue value, IshPatternBindings *bindings, IshError *err) {
    if (!pat) return ish_error_set(err, ish_span_unknown(NULL), "cannot match null pattern");
    size_t checkpoint = bindings->count;
    bool ok = false;
    switch (pat->kind) {
        case ISH_PAT_WILDCARD:
            ok = true;
            break;
        case ISH_PAT_BIND:
        {
            const IshValue *existing = ish_pattern_bindings_get(bindings, pat->as.name);
            if (existing) ok = ish_value_equal(*existing, value);
            else {
                ok = ish_pattern_bindings_add(bindings, pat->as.name, value);
                if (!ok) ish_error_oom(err, pat->span);
            }
            break;
        }
        case ISH_PAT_PIN: {
            const IshValue *pinned = ish_pattern_bindings_get(bindings, pat->as.name);
            ok = pinned && ish_value_equal(*pinned, value);
            break;
        }
        case ISH_PAT_LITERAL:
            ok = ish_value_equal(pat->as.literal, value);
            break;
        case ISH_PAT_PAIR: {
            if (!ish_is_pair(value)) {
                ok = false;
                break;
            }
            IshValue car = ish_car(value, err);
            IshValue cdr = ish_cdr(value, err);
            if (err && err->present) {
                ok = false;
                break;
            }
            ok = ish_pattern_match(rt, pat->as.pair.left, car, bindings, err) &&
                 ish_pattern_match(rt, pat->as.pair.right, cdr, bindings, err);
            break;
        }
        case ISH_PAT_LIST: {
            IshValue cur = value;
            ok = true;
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                if (!ish_is_pair(cur)) { ok = false; break; }
                IshValue car = ish_car(cur, err);
                IshValue cdr = ish_cdr(cur, err);
                if (err && err->present) { ok = false; break; }
                if (!ish_pattern_match(rt, pat->as.seq.items[i], car, bindings, err)) { ok = false; break; }
                cur = cdr;
            }
            ok = ok && ish_is_nil(cur);
            break;
        }
        case ISH_PAT_VECTOR:
        case ISH_PAT_TUPLE: {
            if ((pat->kind == ISH_PAT_VECTOR && !ish_is_vector(value)) || (pat->kind == ISH_PAT_TUPLE && !ish_is_tuple(value))) {
                ok = false;
                break;
            }
            if (ish_sequence_count(value) != pat->as.seq.count) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                IshValue item = ish_sequence_item(value, i, err);
                if (err && err->present) { ok = false; break; }
                if (!ish_pattern_match(rt, pat->as.seq.items[i], item, bindings, err)) { ok = false; break; }
            }
            break;
        }
        case ISH_PAT_VECTOR_REST:
        case ISH_PAT_TUPLE_REST: {
            bool want_vector = pat->kind == ISH_PAT_VECTOR_REST;
            if ((want_vector && !ish_is_vector(value)) || (!want_vector && !ish_is_tuple(value))) { ok = false; break; }
            size_t n = ish_sequence_count(value);
            if (n < pat->as.seq_rest.count) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                IshValue item = ish_sequence_item(value, i, err);
                if (err && err->present) { ok = false; break; }
                if (!ish_pattern_match(rt, pat->as.seq_rest.items[i], item, bindings, err)) { ok = false; break; }
            }
            if (!ok) break;
            size_t rest_count = n - pat->as.seq_rest.count;
            IshValue *rest_items = rest_count == 0 ? NULL : calloc(rest_count, sizeof(*rest_items));
            if (rest_count != 0 && !rest_items) return ish_error_oom(err, pat->span);
            for (size_t i = 0; i < rest_count; i++) rest_items[i] = ish_sequence_item(value, pat->as.seq_rest.count + i, err);
            if (!rt) {
                free(rest_items);
                return ish_error_set(err, pat->span, "runtime required for vector/tuple rest pattern");
            }
            IshValue rest_value = want_vector ? ish_vector(rt, rest_items, rest_count, err) : ish_tuple(rt, rest_items, rest_count, err);
            free(rest_items);
            if (err && err->present) return false;
            ok = ish_pattern_match(rt, pat->as.seq_rest.rest, rest_value, bindings, err);
            break;
        }
        case ISH_PAT_DICT: {
            if (!ish_is_dict(value)) { ok = false; break; }
            ok = true;
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                IshValue item = ish_nil();
                if (!ish_dict_get(value, pat->as.dict.entries[i].key, &item)) { ok = false; break; }
                if (!ish_pattern_match(rt, pat->as.dict.entries[i].pattern, item, bindings, err)) { ok = false; break; }
            }
            break;
        }
        case ISH_PAT_AS:
        case ISH_PAT_GUARD:
        case ISH_PAT_SYNTAX_ELLIPSIS:
            return ish_error_set(err, pat->span, "pattern kind is not implemented in matcher yet");
    }
    if (!ok) bindings_truncate(bindings, checkpoint);
    return ok;
}
