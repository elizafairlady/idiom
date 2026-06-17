#include "idiom/pattern.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef enum {
    SEL_PAT_WILDCARD,
    SEL_PAT_BIND,
    SEL_PAT_PIN,
    SEL_PAT_LITERAL,
    SEL_PAT_PAIR,
    SEL_PAT_VECTOR,
    SEL_PAT_TUPLE,
    SEL_PAT_VECTOR_REST,
    SEL_PAT_TUPLE_REST,
    SEL_PAT_DICT
} SelPatKind;

typedef struct SelPat SelPat;

typedef struct {
    IdmValue key;
    SelPat *pattern;
} SelDictEntry;

struct SelPat {
    SelPatKind kind;
    IdmSpan span;
    union {
        struct { char *name; uint32_t slot; } name;
        IdmValue literal;
        struct { SelPat *left; SelPat *right; } pair;
        struct { SelPat **items; size_t count; } seq;
        struct { SelPat **items; size_t count; SelPat *rest; } seq_rest;
        struct { SelDictEntry *entries; size_t count; } dict;
    } as;
};

typedef enum {
    SEL_ACCESS_ARG,
    SEL_ACCESS_CAR,
    SEL_ACCESS_CDR,
    SEL_ACCESS_SEQ_ITEM,
    SEL_ACCESS_SEQ_REST,
    SEL_ACCESS_DICT_VALUE
} SelAccessKind;

typedef struct {
    SelAccessKind kind;
    uint32_t parent;
    uint32_t index;
    IdmValue key;
    IdmValueTag seq_tag;
} SelAccess;

typedef enum {
    SEL_ACTION_BIND,
    SEL_ACTION_PIN,
    SEL_ACTION_DICT_HAS
} SelActionKind;

typedef struct {
    SelActionKind kind;
    uint32_t access;
    char *name;
    uint32_t slot;
    IdmValue key;
} SelAction;

typedef struct {
    uint32_t access;
    SelPat *pattern;
} SelCell;

typedef struct {
    uint32_t function_index;
    uint32_t arity;
    bool has_guard;
    SelCell *cells;
    size_t cell_count;
    SelAction *actions;
    size_t action_count;
} SelRow;

typedef enum {
    SEL_CTOR_LITERAL,
    SEL_CTOR_PAIR,
    SEL_CTOR_VECTOR_EXACT,
    SEL_CTOR_VECTOR_REST,
    SEL_CTOR_TUPLE_EXACT,
    SEL_CTOR_TUPLE_REST,
    SEL_CTOR_DICT
} SelCtorKind;

typedef struct {
    SelCtorKind kind;
    IdmValue literal;
    uint32_t count;
} SelCtor;

typedef struct SelNode SelNode;

typedef struct {
    SelCtor ctor;
    SelNode *node;
} SelCase;

typedef enum {
    SEL_NODE_FAIL,
    SEL_NODE_TRY,
    SEL_NODE_SWITCH
} SelNodeKind;

struct SelNode {
    SelNodeKind kind;
    union {
        struct {
            uint32_t function_index;
            bool has_guard;
            SelAction *actions;
            size_t action_count;
            SelCell *residuals;
            size_t residual_count;
            SelNode *next;
        } try_row;
        struct {
            uint32_t access;
            SelCase *cases;
            size_t case_count;
            SelNode *default_node;
        } sw;
    } as;
};

typedef struct {
    uint32_t arity;
    SelNode *node;
} SelArityCase;

struct IdmPatternSelector {
    size_t refcount;
    SelAccess *accesses;
    size_t access_count;
    size_t access_cap;
    SelPat **patterns;
    size_t pattern_count;
    SelArityCase *arities;
    size_t arity_count;
};

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
    bindings->names = bindings->inline_names;
    bindings->values = bindings->inline_values;
    bindings->slots = bindings->inline_slots;
    bindings->count = 0;
    bindings->cap = IDM_PATTERN_INLINE_BINDINGS;
    bindings->heap = false;
}

void idm_pattern_bindings_move(IdmPatternBindings *dst, IdmPatternBindings *src) {
    if (src->heap) {
        dst->names = src->names;
        dst->values = src->values;
        dst->slots = src->slots;
        dst->count = src->count;
        dst->cap = src->cap;
        dst->heap = true;
    } else {
        idm_pattern_bindings_init(dst);
        for (size_t i = 0; i < src->count; i++) {
            dst->inline_names[i] = src->inline_names[i];
            dst->inline_values[i] = src->inline_values[i];
            dst->inline_slots[i] = src->inline_slots[i];
        }
        dst->count = src->count;
    }
    idm_pattern_bindings_init(src);
}

void idm_pattern_bindings_destroy(IdmPatternBindings *bindings) {
    if (!bindings) return;
    if (bindings->heap) {
        free(bindings->names);
        free(bindings->values);
        free(bindings->slots);
    }
    bindings->names = bindings->inline_names;
    bindings->values = bindings->inline_values;
    bindings->slots = bindings->inline_slots;
    bindings->count = 0;
    bindings->cap = IDM_PATTERN_INLINE_BINDINGS;
    bindings->heap = false;
}

const IdmValue *idm_pattern_bindings_get(const IdmPatternBindings *bindings, const char *name) {
    for (size_t i = 0; i < bindings->count; i++) {
        if (strcmp(bindings->names[i], name) == 0) return &bindings->values[i];
    }
    return NULL;
}

const IdmValue *idm_pattern_bindings_get_slot(const IdmPatternBindings *bindings, uint32_t slot) {
    for (size_t i = 0; i < bindings->count; i++) {
        if (bindings->slots[i] == slot) return &bindings->values[i];
    }
    return NULL;
}

bool idm_pattern_bindings_add_slot(IdmPatternBindings *bindings, const char *name, uint32_t slot, IdmValue value) {
    if (slot != UINT32_MAX) {
        const IdmValue *existing = idm_pattern_bindings_get_slot(bindings, slot);
        if (existing) return idm_value_equal(*existing, value);
    }
    const IdmValue *existing = idm_pattern_bindings_get(bindings, name);
    if (existing) return idm_value_equal(*existing, value);
    if (bindings->count == bindings->cap) {
        size_t cap = bindings->cap ? bindings->cap * 2u : 4u;
        char **names = malloc(cap * sizeof(*names));
        if (!names) return false;
        IdmValue *values = malloc(cap * sizeof(*values));
        if (!values) {
            free(names);
            return false;
        }
        uint32_t *slots = malloc(cap * sizeof(*slots));
        if (!slots) {
            free(names);
            free(values);
            return false;
        }
        if (bindings->count != 0) {
            memcpy(names, bindings->names, bindings->count * sizeof(*names));
            memcpy(values, bindings->values, bindings->count * sizeof(*values));
            memcpy(slots, bindings->slots, bindings->count * sizeof(*slots));
        }
        if (bindings->heap) {
            free(bindings->names);
            free(bindings->values);
            free(bindings->slots);
        }
        bindings->names = names;
        bindings->values = values;
        bindings->slots = slots;
        bindings->cap = cap;
        bindings->heap = true;
    }
    bindings->names[bindings->count] = (char *)name;
    bindings->values[bindings->count] = value;
    bindings->slots[bindings->count] = slot;
    bindings->count++;
    return true;
}

bool idm_pattern_bindings_add(IdmPatternBindings *bindings, const char *name, IdmValue value) {
    return idm_pattern_bindings_add_slot(bindings, name, UINT32_MAX, value);
}

static void bindings_truncate(IdmPatternBindings *bindings, size_t count) {
    bindings->count = count;
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
            ok = ok && idm_is_empty_list(cur);
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

static SelPat *sel_pat_alloc(SelPatKind kind, IdmSpan span) {
    SelPat *pat = calloc(1u, sizeof(*pat));
    if (!pat) return NULL;
    pat->kind = kind;
    pat->span = span;
    return pat;
}

static SelPat *sel_pat_literal(IdmValue value, IdmSpan span) {
    SelPat *pat = sel_pat_alloc(SEL_PAT_LITERAL, span);
    if (pat) pat->as.literal = value;
    return pat;
}

static void sel_pat_free(SelPat *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
            free(pat->as.name.name);
            break;
        case SEL_PAT_PAIR:
            sel_pat_free(pat->as.pair.left);
            sel_pat_free(pat->as.pair.right);
            break;
        case SEL_PAT_VECTOR:
        case SEL_PAT_TUPLE:
            for (size_t i = 0; i < pat->as.seq.count; i++) sel_pat_free(pat->as.seq.items[i]);
            free(pat->as.seq.items);
            break;
        case SEL_PAT_VECTOR_REST:
        case SEL_PAT_TUPLE_REST:
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) sel_pat_free(pat->as.seq_rest.items[i]);
            free(pat->as.seq_rest.items);
            sel_pat_free(pat->as.seq_rest.rest);
            break;
        case SEL_PAT_DICT:
            for (size_t i = 0; i < pat->as.dict.count; i++) sel_pat_free(pat->as.dict.entries[i].pattern);
            free(pat->as.dict.entries);
            break;
        case SEL_PAT_WILDCARD:
        case SEL_PAT_LITERAL:
            break;
    }
    free(pat);
}

static uint32_t selector_local_slot(const IdmPatternLocal *locals, uint32_t local_count, const char *name) {
    for (uint32_t i = 0; i < local_count; i++) {
        if (strcmp(locals[i].name, name) == 0) return locals[i].slot;
    }
    return UINT32_MAX;
}

static SelPat *sel_pat_from_idm(const IdmPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err);

static SelPat *sel_pat_list_from_idm_items(IdmPattern *const *items, size_t count, const IdmPatternLocal *locals, uint32_t local_count, IdmSpan span, IdmError *err) {
    if (count == 0) return sel_pat_literal(idm_empty_list(), span);
    SelPat *head = sel_pat_from_idm(items[0], locals, local_count, err);
    SelPat *tail = sel_pat_list_from_idm_items(items + 1u, count - 1u, locals, local_count, span, err);
    SelPat *out = head && tail ? sel_pat_alloc(SEL_PAT_PAIR, span) : NULL;
    if (!out) {
        sel_pat_free(head);
        sel_pat_free(tail);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    out->as.pair.left = head;
    out->as.pair.right = tail;
    return out;
}

static SelPat *sel_pat_from_idm(const IdmPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err) {
    if (!pat) {
        idm_error_set(err, idm_span_unknown(NULL), "cannot lower null pattern");
        return NULL;
    }
    SelPat *out = NULL;
    switch (pat->kind) {
        case IDM_PAT_WILDCARD:
            return sel_pat_alloc(SEL_PAT_WILDCARD, pat->span);
        case IDM_PAT_BIND:
        case IDM_PAT_PIN:
            out = sel_pat_alloc(pat->kind == IDM_PAT_BIND ? SEL_PAT_BIND : SEL_PAT_PIN, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.name.name = idm_strdup(pat->as.name);
            out->as.name.slot = selector_local_slot(locals, local_count, pat->as.name);
            if (!out->as.name.name) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            return out;
        case IDM_PAT_LITERAL:
            out = sel_pat_literal(pat->as.literal, pat->span);
            if (!out) idm_error_oom(err, pat->span);
            return out;
        case IDM_PAT_PAIR:
            out = sel_pat_alloc(SEL_PAT_PAIR, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.pair.left = sel_pat_from_idm(pat->as.pair.left, locals, local_count, err);
            out->as.pair.right = sel_pat_from_idm(pat->as.pair.right, locals, local_count, err);
            if (!out->as.pair.left || !out->as.pair.right) { sel_pat_free(out); return NULL; }
            return out;
        case IDM_PAT_LIST:
            return sel_pat_list_from_idm_items(pat->as.seq.items, pat->as.seq.count, locals, local_count, pat->span, err);
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            out = sel_pat_alloc(pat->kind == IDM_PAT_VECTOR ? SEL_PAT_VECTOR : SEL_PAT_TUPLE, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.seq.count = pat->as.seq.count;
            out->as.seq.items = pat->as.seq.count == 0 ? NULL : calloc(pat->as.seq.count, sizeof(*out->as.seq.items));
            if (pat->as.seq.count != 0 && !out->as.seq.items) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                out->as.seq.items[i] = sel_pat_from_idm(pat->as.seq.items[i], locals, local_count, err);
                if (!out->as.seq.items[i]) { sel_pat_free(out); return NULL; }
            }
            return out;
        }
        case IDM_PAT_VECTOR_REST:
        case IDM_PAT_TUPLE_REST: {
            out = sel_pat_alloc(pat->kind == IDM_PAT_VECTOR_REST ? SEL_PAT_VECTOR_REST : SEL_PAT_TUPLE_REST, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.seq_rest.count = pat->as.seq_rest.count;
            out->as.seq_rest.items = pat->as.seq_rest.count == 0 ? NULL : calloc(pat->as.seq_rest.count, sizeof(*out->as.seq_rest.items));
            if (pat->as.seq_rest.count != 0 && !out->as.seq_rest.items) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                out->as.seq_rest.items[i] = sel_pat_from_idm(pat->as.seq_rest.items[i], locals, local_count, err);
                if (!out->as.seq_rest.items[i]) { sel_pat_free(out); return NULL; }
            }
            out->as.seq_rest.rest = sel_pat_from_idm(pat->as.seq_rest.rest, locals, local_count, err);
            if (!out->as.seq_rest.rest) { sel_pat_free(out); return NULL; }
            return out;
        }
        case IDM_PAT_DICT: {
            out = sel_pat_alloc(SEL_PAT_DICT, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.dict.count = pat->as.dict.count;
            out->as.dict.entries = pat->as.dict.count == 0 ? NULL : calloc(pat->as.dict.count, sizeof(*out->as.dict.entries));
            if (pat->as.dict.count != 0 && !out->as.dict.entries) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                out->as.dict.entries[i].key = pat->as.dict.entries[i].key;
                out->as.dict.entries[i].pattern = sel_pat_from_idm(pat->as.dict.entries[i].pattern, locals, local_count, err);
                if (!out->as.dict.entries[i].pattern) { sel_pat_free(out); return NULL; }
            }
            return out;
        }
    }
    idm_error_set(err, pat->span, "unknown pattern kind");
    return NULL;
}

static void sel_action_destroy(SelAction *action) {
    free(action->name);
    action->name = NULL;
}

static void sel_row_destroy(SelRow *row) {
    if (!row) return;
    free(row->cells);
    for (size_t i = 0; i < row->action_count; i++) sel_action_destroy(&row->actions[i]);
    free(row->actions);
    memset(row, 0, sizeof(*row));
}

static bool sel_action_copy(SelAction *dst, const SelAction *src, IdmError *err, IdmSpan span) {
    *dst = *src;
    dst->name = NULL;
    if (src->name) {
        dst->name = idm_strdup(src->name);
        if (!dst->name) return idm_error_oom(err, span);
    }
    return true;
}

static bool sel_row_clone(const SelRow *src, SelRow *dst, IdmError *err, IdmSpan span) {
    memset(dst, 0, sizeof(*dst));
    dst->function_index = src->function_index;
    dst->arity = src->arity;
    dst->has_guard = src->has_guard;
    dst->cell_count = src->cell_count;
    dst->action_count = src->action_count;
    if (src->cell_count != 0) {
        dst->cells = malloc(src->cell_count * sizeof(*dst->cells));
        if (!dst->cells) return idm_error_oom(err, span);
        memcpy(dst->cells, src->cells, src->cell_count * sizeof(*dst->cells));
    }
    if (src->action_count != 0) {
        dst->actions = calloc(src->action_count, sizeof(*dst->actions));
        if (!dst->actions) { sel_row_destroy(dst); return idm_error_oom(err, span); }
        for (size_t i = 0; i < src->action_count; i++) {
            if (!sel_action_copy(&dst->actions[i], &src->actions[i], err, span)) {
                dst->action_count = i;
                sel_row_destroy(dst);
                return false;
            }
        }
    }
    return true;
}

static bool sel_row_clone_without_cell(const SelRow *src, size_t skip, SelRow *dst, IdmError *err, IdmSpan span) {
    if (!sel_row_clone(src, dst, err, span)) return false;
    if (skip >= dst->cell_count) return true;
    memmove(dst->cells + skip, dst->cells + skip + 1u, (dst->cell_count - skip - 1u) * sizeof(*dst->cells));
    dst->cell_count--;
    return true;
}

static bool sel_row_add_cell(SelRow *row, uint32_t access, SelPat *pattern, IdmError *err, IdmSpan span) {
    SelCell *next = realloc(row->cells, (row->cell_count + 1u) * sizeof(*row->cells));
    if (!next) return idm_error_oom(err, span);
    row->cells = next;
    row->cells[row->cell_count].access = access;
    row->cells[row->cell_count].pattern = pattern;
    row->cell_count++;
    return true;
}

static bool sel_row_add_action(SelRow *row, SelAction action, IdmError *err, IdmSpan span) {
    SelAction *next = realloc(row->actions, (row->action_count + 1u) * sizeof(*row->actions));
    if (!next) {
        sel_action_destroy(&action);
        return idm_error_oom(err, span);
    }
    row->actions = next;
    row->actions[row->action_count++] = action;
    return true;
}

static bool sel_row_add_name_action(SelRow *row, SelActionKind kind, uint32_t access, const char *name, uint32_t slot, IdmError *err, IdmSpan span) {
    SelAction action;
    memset(&action, 0, sizeof(action));
    action.kind = kind;
    action.access = access;
    action.slot = slot;
    action.name = idm_strdup(name);
    if (!action.name) return idm_error_oom(err, span);
    return sel_row_add_action(row, action, err, span);
}

static bool sel_row_add_dict_has(SelRow *row, uint32_t access, IdmValue key, IdmError *err, IdmSpan span) {
    SelAction action;
    memset(&action, 0, sizeof(action));
    action.kind = SEL_ACTION_DICT_HAS;
    action.access = access;
    action.key = key;
    return sel_row_add_action(row, action, err, span);
}

static bool selector_add_access(IdmPatternSelector *selector, SelAccess access, uint32_t *out, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < selector->access_count; i++) {
        SelAccess *cur = &selector->accesses[i];
        if (cur->kind != access.kind || cur->parent != access.parent || cur->index != access.index || cur->seq_tag != access.seq_tag) continue;
        if (access.kind == SEL_ACCESS_DICT_VALUE && !idm_value_equal(cur->key, access.key)) continue;
        *out = (uint32_t)i;
        return true;
    }
    if (selector->access_count >= UINT32_MAX) return idm_error_set(err, span, "pattern selector has too many access paths");
    if (selector->access_count == selector->access_cap) {
        size_t cap = selector->access_cap ? selector->access_cap * 2u : 16u;
        SelAccess *next = realloc(selector->accesses, cap * sizeof(*selector->accesses));
        if (!next) return idm_error_oom(err, span);
        selector->accesses = next;
        selector->access_cap = cap;
    }
    selector->accesses[selector->access_count] = access;
    *out = (uint32_t)selector->access_count++;
    return true;
}

static bool selector_root_access(IdmPatternSelector *selector, uint32_t arg, uint32_t *out, IdmError *err, IdmSpan span) {
    SelAccess access;
    memset(&access, 0, sizeof(access));
    access.kind = SEL_ACCESS_ARG;
    access.index = arg;
    return selector_add_access(selector, access, out, err, span);
}

static bool selector_child_access(IdmPatternSelector *selector, SelAccessKind kind, uint32_t parent, uint32_t index, IdmValueTag seq_tag, IdmValue key, uint32_t *out, IdmError *err, IdmSpan span) {
    SelAccess access;
    memset(&access, 0, sizeof(access));
    access.kind = kind;
    access.parent = parent;
    access.index = index;
    access.seq_tag = seq_tag;
    access.key = key;
    return selector_add_access(selector, access, out, err, span);
}

static bool literal_can_be_disjoint_ctor(IdmValue value) {
    switch (value.tag) {
        case IDM_VAL_NIL:
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
        case IDM_VAL_INT:
        case IDM_VAL_FLOAT:
        case IDM_VAL_STRING:
        case IDM_VAL_EMPTY_LIST:
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
        case IDM_VAL_PRIMITIVE:
            return true;
        case IDM_VAL_PAIR:
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR:
        case IDM_VAL_DICT:
        case IDM_VAL_SYNTAX:
        case IDM_VAL_CELL:
        case IDM_VAL_CLOSURE:
        case IDM_VAL_RECORD:
        case IDM_VAL_REGEX:
        case IDM_VAL_REGEX_RESULT:
            return false;
    }
    return false;
}

static bool sel_pat_ctor(const SelPat *pat, SelCtor *out) {
    switch (pat->kind) {
        case SEL_PAT_LITERAL:
            if (!literal_can_be_disjoint_ctor(pat->as.literal)) return false;
            out->kind = SEL_CTOR_LITERAL;
            out->literal = pat->as.literal;
            out->count = 0;
            return true;
        case SEL_PAT_PAIR:
            out->kind = SEL_CTOR_PAIR;
            out->count = 0;
            return true;
        case SEL_PAT_VECTOR:
            out->kind = SEL_CTOR_VECTOR_EXACT;
            out->count = (uint32_t)pat->as.seq.count;
            return pat->as.seq.count <= UINT32_MAX;
        case SEL_PAT_TUPLE:
            out->kind = SEL_CTOR_TUPLE_EXACT;
            out->count = (uint32_t)pat->as.seq.count;
            return pat->as.seq.count <= UINT32_MAX;
        case SEL_PAT_VECTOR_REST:
            out->kind = SEL_CTOR_VECTOR_REST;
            out->count = (uint32_t)pat->as.seq_rest.count;
            return pat->as.seq_rest.count <= UINT32_MAX;
        case SEL_PAT_TUPLE_REST:
            out->kind = SEL_CTOR_TUPLE_REST;
            out->count = (uint32_t)pat->as.seq_rest.count;
            return pat->as.seq_rest.count <= UINT32_MAX;
        case SEL_PAT_DICT:
            out->kind = SEL_CTOR_DICT;
            out->count = 0;
            return true;
        case SEL_PAT_WILDCARD:
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
            return false;
    }
    return false;
}

static bool sel_ctor_equal(SelCtor a, SelCtor b) {
    if (a.kind != b.kind || a.count != b.count) return false;
    return a.kind != SEL_CTOR_LITERAL || idm_value_equal(a.literal, b.literal);
}

static bool sel_ctor_matches_value(SelCtor ctor, IdmValue value) {
    switch (ctor.kind) {
        case SEL_CTOR_LITERAL:
            return idm_value_equal(ctor.literal, value);
        case SEL_CTOR_PAIR:
            return idm_is_pair(value);
        case SEL_CTOR_VECTOR_EXACT:
            return idm_is_vector(value) && idm_sequence_count(value) == ctor.count;
        case SEL_CTOR_VECTOR_REST:
            return idm_is_vector(value) && idm_sequence_count(value) >= ctor.count;
        case SEL_CTOR_TUPLE_EXACT:
            return idm_is_tuple(value) && idm_sequence_count(value) == ctor.count;
        case SEL_CTOR_TUPLE_REST:
            return idm_is_tuple(value) && idm_sequence_count(value) >= ctor.count;
        case SEL_CTOR_DICT:
            return idm_is_dict(value);
    }
    return false;
}

static bool sel_pat_defaultable(const SelPat *pat) {
    return pat->kind == SEL_PAT_WILDCARD || pat->kind == SEL_PAT_BIND || pat->kind == SEL_PAT_PIN;
}

static bool sel_pat_complex_literal_compatible(const SelPat *pat, SelCtor ctor) {
    return pat->kind == SEL_PAT_LITERAL && !literal_can_be_disjoint_ctor(pat->as.literal) && sel_ctor_matches_value(ctor, pat->as.literal);
}

static bool sel_pat_compatible_with_any_case(const SelPat *pat, const SelCtor *ctors, size_t count) {
    SelCtor own;
    if (sel_pat_ctor(pat, &own)) {
        for (size_t i = 0; i < count; i++) if (sel_ctor_equal(own, ctors[i])) return true;
        return false;
    }
    if (pat->kind == SEL_PAT_LITERAL && !literal_can_be_disjoint_ctor(pat->as.literal)) {
        for (size_t i = 0; i < count; i++) if (sel_ctor_matches_value(ctors[i], pat->as.literal)) return true;
        return false;
    }
    return false;
}

static ssize_t sel_row_find_cell(const SelRow *row, uint32_t access) {
    for (size_t i = 0; i < row->cell_count; i++) if (row->cells[i].access == access) return (ssize_t)i;
    return -1;
}

static void sel_node_free(SelNode *node) {
    if (!node) return;
    switch (node->kind) {
        case SEL_NODE_TRY:
            for (size_t i = 0; i < node->as.try_row.action_count; i++) sel_action_destroy(&node->as.try_row.actions[i]);
            free(node->as.try_row.actions);
            free(node->as.try_row.residuals);
            sel_node_free(node->as.try_row.next);
            break;
        case SEL_NODE_SWITCH:
            for (size_t i = 0; i < node->as.sw.case_count; i++) sel_node_free(node->as.sw.cases[i].node);
            free(node->as.sw.cases);
            sel_node_free(node->as.sw.default_node);
            break;
        case SEL_NODE_FAIL:
            break;
    }
    free(node);
}

static SelNode *sel_node_fail(void) {
    SelNode *node = calloc(1u, sizeof(*node));
    if (node) node->kind = SEL_NODE_FAIL;
    return node;
}

static bool copy_actions(SelAction **out, size_t *out_count, const SelAction *actions, size_t count, IdmError *err, IdmSpan span) {
    *out = NULL;
    *out_count = 0;
    if (count == 0) return true;
    SelAction *copy = calloc(count, sizeof(*copy));
    if (!copy) return idm_error_oom(err, span);
    for (size_t i = 0; i < count; i++) {
        if (!sel_action_copy(&copy[i], &actions[i], err, span)) {
            for (size_t j = 0; j < i; j++) sel_action_destroy(&copy[j]);
            free(copy);
            return false;
        }
    }
    *out = copy;
    *out_count = count;
    return true;
}

static bool row_add_pair_subcells(IdmPatternSelector *selector, SelRow *row, uint32_t access, const SelPat *pat, IdmError *err) {
    uint32_t car_access = 0, cdr_access = 0;
    if (!selector_child_access(selector, SEL_ACCESS_CAR, access, 0, IDM_VAL_NIL, idm_nil(), &car_access, err, pat->span) ||
        !selector_child_access(selector, SEL_ACCESS_CDR, access, 0, IDM_VAL_NIL, idm_nil(), &cdr_access, err, pat->span)) return false;
    return sel_row_add_cell(row, car_access, pat->as.pair.left, err, pat->span) &&
           sel_row_add_cell(row, cdr_access, pat->as.pair.right, err, pat->span);
}

static bool row_add_sequence_subcells(IdmPatternSelector *selector, SelRow *row, uint32_t access, const SelPat *pat, IdmError *err) {
    bool rest = pat->kind == SEL_PAT_VECTOR_REST || pat->kind == SEL_PAT_TUPLE_REST;
    IdmValueTag seq_tag = (pat->kind == SEL_PAT_VECTOR || pat->kind == SEL_PAT_VECTOR_REST) ? IDM_VAL_VECTOR : IDM_VAL_TUPLE;
    size_t count = rest ? pat->as.seq_rest.count : pat->as.seq.count;
    SelPat **items = rest ? pat->as.seq_rest.items : pat->as.seq.items;
    for (size_t i = 0; i < count; i++) {
        uint32_t item_access = 0;
        if (!selector_child_access(selector, SEL_ACCESS_SEQ_ITEM, access, (uint32_t)i, seq_tag, idm_nil(), &item_access, err, pat->span) ||
            !sel_row_add_cell(row, item_access, items[i], err, pat->span)) return false;
    }
    if (rest) {
        uint32_t rest_access = 0;
        if (!selector_child_access(selector, SEL_ACCESS_SEQ_REST, access, (uint32_t)count, seq_tag, idm_nil(), &rest_access, err, pat->span) ||
            !sel_row_add_cell(row, rest_access, pat->as.seq_rest.rest, err, pat->span)) return false;
    }
    return true;
}

static bool row_add_dict_subcells(IdmPatternSelector *selector, SelRow *row, uint32_t access, const SelPat *pat, IdmError *err) {
    for (size_t i = 0; i < pat->as.dict.count; i++) {
        IdmValue key = pat->as.dict.entries[i].key;
        uint32_t value_access = 0;
        if (!sel_row_add_dict_has(row, access, key, err, pat->span) ||
            !selector_child_access(selector, SEL_ACCESS_DICT_VALUE, access, 0, IDM_VAL_NIL, key, &value_access, err, pat->span) ||
            !sel_row_add_cell(row, value_access, pat->as.dict.entries[i].pattern, err, pat->span)) return false;
    }
    return true;
}

static bool row_add_ctor_subcells(IdmPatternSelector *selector, SelRow *row, uint32_t access, const SelPat *pat, IdmError *err) {
    switch (pat->kind) {
        case SEL_PAT_LITERAL:
            return true;
        case SEL_PAT_PAIR:
            return row_add_pair_subcells(selector, row, access, pat, err);
        case SEL_PAT_VECTOR:
        case SEL_PAT_TUPLE:
        case SEL_PAT_VECTOR_REST:
        case SEL_PAT_TUPLE_REST:
            return row_add_sequence_subcells(selector, row, access, pat, err);
        case SEL_PAT_DICT:
            return row_add_dict_subcells(selector, row, access, pat, err);
        case SEL_PAT_WILDCARD:
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
            return true;
    }
    return true;
}

static bool specialize_defaultable_cell(const SelRow *src, size_t cell_index, SelRow *dst, IdmError *err, IdmSpan span) {
    SelPat *pat = src->cells[cell_index].pattern;
    uint32_t access = src->cells[cell_index].access;
    if (!sel_row_clone_without_cell(src, cell_index, dst, err, span)) return false;
    if (pat->kind == SEL_PAT_BIND) return sel_row_add_name_action(dst, SEL_ACTION_BIND, access, pat->as.name.name, pat->as.name.slot, err, pat->span);
    if (pat->kind == SEL_PAT_PIN) return sel_row_add_name_action(dst, SEL_ACTION_PIN, access, pat->as.name.name, pat->as.name.slot, err, pat->span);
    return true;
}

static bool specialize_row_for_case(IdmPatternSelector *selector, const SelRow *src, uint32_t access, SelCtor ctor, SelRow *dst, bool *out_include, IdmError *err) {
    *out_include = false;
    ssize_t idx = sel_row_find_cell(src, access);
    if (idx < 0) {
        *out_include = true;
        return sel_row_clone(src, dst, err, idm_span_unknown(NULL));
    }
    SelPat *pat = src->cells[idx].pattern;
    if (sel_pat_defaultable(pat)) {
        *out_include = true;
        return specialize_defaultable_cell(src, (size_t)idx, dst, err, pat->span);
    }
    SelCtor own;
    if (sel_pat_ctor(pat, &own) && sel_ctor_equal(own, ctor)) {
        *out_include = true;
        return sel_row_clone_without_cell(src, (size_t)idx, dst, err, pat->span) &&
               row_add_ctor_subcells(selector, dst, access, pat, err);
    }
    if (sel_pat_complex_literal_compatible(pat, ctor)) {
        *out_include = true;
        return sel_row_clone(src, dst, err, pat->span);
    }
    return true;
}

static bool specialize_row_for_default(const SelRow *src, uint32_t access, const SelCtor *ctors, size_t ctor_count, SelRow *dst, bool *out_include, IdmError *err) {
    *out_include = false;
    ssize_t idx = sel_row_find_cell(src, access);
    if (idx < 0) {
        *out_include = true;
        return sel_row_clone(src, dst, err, idm_span_unknown(NULL));
    }
    SelPat *pat = src->cells[idx].pattern;
    if (sel_pat_defaultable(pat)) {
        *out_include = true;
        return specialize_defaultable_cell(src, (size_t)idx, dst, err, pat->span);
    }
    if (pat->kind == SEL_PAT_LITERAL && !literal_can_be_disjoint_ctor(pat->as.literal) && !sel_pat_compatible_with_any_case(pat, ctors, ctor_count)) {
        *out_include = true;
        return sel_row_clone(src, dst, err, pat->span);
    }
    return true;
}

static bool rows_append(SelRow **rows, size_t *count, SelRow row, IdmError *err, IdmSpan span) {
    SelRow *next = realloc(*rows, (*count + 1u) * sizeof(*next));
    if (!next) {
        sel_row_destroy(&row);
        return idm_error_oom(err, span);
    }
    *rows = next;
    (*rows)[(*count)++] = row;
    return true;
}

static void rows_destroy(SelRow *rows, size_t count) {
    for (size_t i = 0; i < count; i++) sel_row_destroy(&rows[i]);
    free(rows);
}

static bool choose_access(const SelRow *rows, size_t row_count, uint32_t *out_access) {
    if (row_count == 0) return false;
    const SelRow *first = &rows[0];
    for (size_t i = 0; i < first->cell_count; i++) {
        SelCtor ignored;
        if (sel_pat_ctor(first->cells[i].pattern, &ignored)) {
            *out_access = first->cells[i].access;
            return true;
        }
    }
    return false;
}

static bool collect_ctors(const SelRow *rows, size_t row_count, uint32_t access, SelCtor **out_ctors, size_t *out_count, IdmError *err) {
    *out_ctors = NULL;
    *out_count = 0;
    for (size_t r = 0; r < row_count; r++) {
        ssize_t idx = sel_row_find_cell(&rows[r], access);
        if (idx < 0) continue;
        SelCtor ctor;
        if (!sel_pat_ctor(rows[r].cells[idx].pattern, &ctor)) continue;
        bool seen = false;
        for (size_t i = 0; i < *out_count; i++) {
            if (sel_ctor_equal((*out_ctors)[i], ctor)) { seen = true; break; }
        }
        if (seen) continue;
        SelCtor *next = realloc(*out_ctors, (*out_count + 1u) * sizeof(*next));
        if (!next) {
            free(*out_ctors);
            *out_ctors = NULL;
            *out_count = 0;
            return idm_error_oom(err, rows[r].cells[idx].pattern->span);
        }
        *out_ctors = next;
        (*out_ctors)[(*out_count)++] = ctor;
    }
    return true;
}

static SelNode *compile_rows(IdmPatternSelector *selector, const SelRow *rows, size_t row_count, IdmError *err);

static SelNode *make_try_node(const SelRow *row, SelNode *next, IdmError *err) {
    SelNode *node = calloc(1u, sizeof(*node));
    if (!node) {
        sel_node_free(next);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    node->kind = SEL_NODE_TRY;
    node->as.try_row.function_index = row->function_index;
    node->as.try_row.has_guard = row->has_guard;
    node->as.try_row.next = next;
    if (!copy_actions(&node->as.try_row.actions, &node->as.try_row.action_count, row->actions, row->action_count, err, idm_span_unknown(NULL))) {
        free(node);
        sel_node_free(next);
        return NULL;
    }
    if (row->cell_count != 0) {
        node->as.try_row.residuals = malloc(row->cell_count * sizeof(*node->as.try_row.residuals));
        if (!node->as.try_row.residuals) {
            sel_node_free(node);
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
        memcpy(node->as.try_row.residuals, row->cells, row->cell_count * sizeof(*node->as.try_row.residuals));
        node->as.try_row.residual_count = row->cell_count;
    }
    return node;
}

static SelNode *compile_rows(IdmPatternSelector *selector, const SelRow *rows, size_t row_count, IdmError *err) {
    if (row_count == 0) return sel_node_fail();
    uint32_t access = 0;
    if (!choose_access(rows, row_count, &access)) {
        SelNode *next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (!next) return NULL;
        return make_try_node(&rows[0], next, err);
    }

    SelCtor *ctors = NULL;
    size_t ctor_count = 0;
    if (!collect_ctors(rows, row_count, access, &ctors, &ctor_count, err)) return NULL;
    if (ctor_count == 0) {
        free(ctors);
        SelNode *next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (!next) return NULL;
        return make_try_node(&rows[0], next, err);
    }

    SelNode *node = calloc(1u, sizeof(*node));
    if (!node) {
        free(ctors);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    node->kind = SEL_NODE_SWITCH;
    node->as.sw.access = access;
    node->as.sw.cases = calloc(ctor_count, sizeof(*node->as.sw.cases));
    if (!node->as.sw.cases) {
        free(ctors);
        sel_node_free(node);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    node->as.sw.case_count = ctor_count;

    for (size_t c = 0; c < ctor_count; c++) {
        SelRow *case_rows = NULL;
        size_t case_count = 0;
        for (size_t r = 0; r < row_count; r++) {
            SelRow specialized;
            memset(&specialized, 0, sizeof(specialized));
            bool include = false;
            if (!specialize_row_for_case(selector, &rows[r], access, ctors[c], &specialized, &include, err)) {
                rows_destroy(case_rows, case_count);
                free(ctors);
                sel_node_free(node);
                return NULL;
            }
            if (include && !rows_append(&case_rows, &case_count, specialized, err, idm_span_unknown(NULL))) {
                rows_destroy(case_rows, case_count);
                free(ctors);
                sel_node_free(node);
                return NULL;
            }
        }
        node->as.sw.cases[c].ctor = ctors[c];
        node->as.sw.cases[c].node = compile_rows(selector, case_rows, case_count, err);
        rows_destroy(case_rows, case_count);
        if (!node->as.sw.cases[c].node) {
            free(ctors);
            sel_node_free(node);
            return NULL;
        }
    }

    SelRow *default_rows = NULL;
    size_t default_count = 0;
    for (size_t r = 0; r < row_count; r++) {
        SelRow specialized;
        memset(&specialized, 0, sizeof(specialized));
        bool include = false;
        if (!specialize_row_for_default(&rows[r], access, ctors, ctor_count, &specialized, &include, err)) {
            rows_destroy(default_rows, default_count);
            free(ctors);
            sel_node_free(node);
            return NULL;
        }
        if (include && !rows_append(&default_rows, &default_count, specialized, err, idm_span_unknown(NULL))) {
            rows_destroy(default_rows, default_count);
            free(ctors);
            sel_node_free(node);
            return NULL;
        }
    }
    node->as.sw.default_node = compile_rows(selector, default_rows, default_count, err);
    rows_destroy(default_rows, default_count);
    free(ctors);
    if (!node->as.sw.default_node) {
        sel_node_free(node);
        return NULL;
    }
    return node;
}

static bool selector_eval_access(IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, uint32_t access_id, IdmValue *out, bool *out_available, IdmError *err) {
    *out_available = false;
    if (access_id >= selector->access_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern access out of bounds");
    const SelAccess *access = &selector->accesses[access_id];
    switch (access->kind) {
        case SEL_ACCESS_ARG:
            if (access->index >= argc) return true;
            *out = args[access->index];
            *out_available = true;
            return true;
        case SEL_ACCESS_CAR: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || !idm_is_pair(parent)) return true;
            *out = idm_car(parent, err);
            if (err && err->present) return false;
            *out_available = true;
            return true;
        }
        case SEL_ACCESS_CDR: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || !idm_is_pair(parent)) return true;
            *out = idm_cdr(parent, err);
            if (err && err->present) return false;
            *out_available = true;
            return true;
        }
        case SEL_ACCESS_SEQ_ITEM: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || (parent.tag != IDM_VAL_VECTOR && parent.tag != IDM_VAL_TUPLE) || access->index >= idm_sequence_count(parent)) return true;
            *out = idm_sequence_item(parent, access->index, err);
            if (err && err->present) return false;
            *out_available = true;
            return true;
        }
        case SEL_ACCESS_SEQ_REST: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || parent.tag != access->seq_tag) return true;
            size_t n = idm_sequence_count(parent);
            if (access->index > n) return true;
            size_t rest_count = n - access->index;
            IdmValue *items = rest_count == 0 ? NULL : calloc(rest_count, sizeof(*items));
            if (rest_count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < rest_count; i++) {
                items[i] = idm_sequence_item(parent, access->index + i, err);
                if (err && err->present) { free(items); return false; }
            }
            *out = access->seq_tag == IDM_VAL_VECTOR ? idm_vector(rt, items, rest_count, err) : idm_tuple(rt, items, rest_count, err);
            free(items);
            if (err && err->present) return false;
            *out_available = true;
            return true;
        }
        case SEL_ACCESS_DICT_VALUE: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || !idm_is_dict(parent)) return true;
            IdmValue value = idm_nil();
            if (!idm_dict_get(parent, access->key, &value)) return true;
            *out = value;
            *out_available = true;
            return true;
        }
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unknown pattern access kind");
}

static bool selector_match_value(IdmRuntime *rt, IdmValue value, const SelPat *pat, IdmPatternBindings *bindings, IdmError *err) {
    switch (pat->kind) {
        case SEL_PAT_WILDCARD:
            return true;
        case SEL_PAT_BIND:
            if (!idm_pattern_bindings_add_slot(bindings, pat->as.name.name, pat->as.name.slot, value)) {
                const IdmValue *existing = pat->as.name.slot != UINT32_MAX
                    ? idm_pattern_bindings_get_slot(bindings, pat->as.name.slot)
                    : idm_pattern_bindings_get(bindings, pat->as.name.name);
                if (existing) return false;
                return idm_error_oom(err, pat->span);
            }
            return true;
        case SEL_PAT_PIN: {
            const IdmValue *pinned = pat->as.name.slot != UINT32_MAX
                ? idm_pattern_bindings_get_slot(bindings, pat->as.name.slot)
                : idm_pattern_bindings_get(bindings, pat->as.name.name);
            return pinned && idm_value_equal(*pinned, value);
        }
        case SEL_PAT_LITERAL:
            return idm_value_equal(pat->as.literal, value);
        case SEL_PAT_PAIR:
            if (!idm_is_pair(value)) return false;
            return selector_match_value(rt, idm_car(value, err), pat->as.pair.left, bindings, err) &&
                   !(err && err->present) &&
                   selector_match_value(rt, idm_cdr(value, err), pat->as.pair.right, bindings, err);
        case SEL_PAT_VECTOR:
        case SEL_PAT_TUPLE: {
            IdmValueTag tag = pat->kind == SEL_PAT_VECTOR ? IDM_VAL_VECTOR : IDM_VAL_TUPLE;
            if (value.tag != tag || idm_sequence_count(value) != pat->as.seq.count) return false;
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                IdmValue item = idm_sequence_item(value, i, err);
                if (err && err->present) return false;
                if (!selector_match_value(rt, item, pat->as.seq.items[i], bindings, err)) return false;
            }
            return true;
        }
        case SEL_PAT_VECTOR_REST:
        case SEL_PAT_TUPLE_REST: {
            IdmValueTag tag = pat->kind == SEL_PAT_VECTOR_REST ? IDM_VAL_VECTOR : IDM_VAL_TUPLE;
            if (value.tag != tag || idm_sequence_count(value) < pat->as.seq_rest.count) return false;
            for (size_t i = 0; i < pat->as.seq_rest.count; i++) {
                IdmValue item = idm_sequence_item(value, i, err);
                if (err && err->present) return false;
                if (!selector_match_value(rt, item, pat->as.seq_rest.items[i], bindings, err)) return false;
            }
            size_t n = idm_sequence_count(value);
            size_t rest_count = n - pat->as.seq_rest.count;
            IdmValue *items = rest_count == 0 ? NULL : calloc(rest_count, sizeof(*items));
            if (rest_count != 0 && !items) return idm_error_oom(err, pat->span);
            for (size_t i = 0; i < rest_count; i++) {
                items[i] = idm_sequence_item(value, pat->as.seq_rest.count + i, err);
                if (err && err->present) { free(items); return false; }
            }
            IdmValue rest = tag == IDM_VAL_VECTOR ? idm_vector(rt, items, rest_count, err) : idm_tuple(rt, items, rest_count, err);
            free(items);
            if (err && err->present) return false;
            return selector_match_value(rt, rest, pat->as.seq_rest.rest, bindings, err);
        }
        case SEL_PAT_DICT:
            if (!idm_is_dict(value)) return false;
            for (size_t i = 0; i < pat->as.dict.count; i++) {
                IdmValue item = idm_nil();
                if (!idm_dict_get(value, pat->as.dict.entries[i].key, &item)) return false;
                if (!selector_match_value(rt, item, pat->as.dict.entries[i].pattern, bindings, err)) return false;
            }
            return true;
    }
    return false;
}

static bool selector_match_pat(IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, uint32_t access, const SelPat *pat, IdmPatternBindings *bindings, IdmError *err) {
    IdmValue value = idm_nil();
    bool available = false;
    if (!selector_eval_access(rt, selector, args, argc, access, &value, &available, err)) return false;
    if (!available) return false;
    return selector_match_value(rt, value, pat, bindings, err);
}

static bool selector_apply_action(IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, const SelAction *action, IdmPatternBindings *bindings, IdmError *err) {
    IdmValue value = idm_nil();
    bool available = false;
    if (!selector_eval_access(rt, selector, args, argc, action->access, &value, &available, err)) return false;
    if (!available) return false;
    switch (action->kind) {
        case SEL_ACTION_BIND:
            if (!idm_pattern_bindings_add_slot(bindings, action->name, action->slot, value)) {
                const IdmValue *existing = action->slot != UINT32_MAX
                    ? idm_pattern_bindings_get_slot(bindings, action->slot)
                    : idm_pattern_bindings_get(bindings, action->name);
                if (existing) return false;
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
            return true;
        case SEL_ACTION_PIN: {
            const IdmValue *pinned = action->slot != UINT32_MAX
                ? idm_pattern_bindings_get_slot(bindings, action->slot)
                : idm_pattern_bindings_get(bindings, action->name);
            return pinned && idm_value_equal(*pinned, value);
        }
        case SEL_ACTION_DICT_HAS:
            (void)rt;
            {
                IdmValue ignored = idm_nil();
                return idm_is_dict(value) && idm_dict_get(value, action->key, &ignored);
            }
    }
    return false;
}

static bool selector_exec_node(IdmRuntime *rt, const IdmPatternSelector *selector, const SelNode *node, const IdmValue *args, uint32_t argc, IdmPatternGuardFn guard, void *guard_user, uint32_t *out_function_index, IdmPatternBindings *bindings, bool *out_matched, IdmError *err) {
    if (!node) {
        *out_matched = false;
        return true;
    }
    switch (node->kind) {
        case SEL_NODE_FAIL:
            *out_matched = false;
            return true;
        case SEL_NODE_TRY: {
            if (node->as.try_row.action_count == 0 && node->as.try_row.residual_count == 0 && !node->as.try_row.has_guard) {
                *out_function_index = node->as.try_row.function_index;
                *out_matched = true;
                return true;
            }
            size_t checkpoint = bindings->count;
            bool ok = true;
            for (size_t i = 0; ok && i < node->as.try_row.action_count; i++) {
                ok = selector_apply_action(rt, selector, args, argc, &node->as.try_row.actions[i], bindings, err);
                if (err && err->present) return false;
            }
            for (size_t i = 0; ok && i < node->as.try_row.residual_count; i++) {
                ok = selector_match_pat(rt, selector, args, argc, node->as.try_row.residuals[i].access, node->as.try_row.residuals[i].pattern, bindings, err);
                if (err && err->present) return false;
            }
            if (ok && node->as.try_row.has_guard) {
                if (!guard) return idm_error_set(err, idm_span_unknown(NULL), "selector guard callback missing");
                bool pass = true;
                if (!guard(guard_user, node->as.try_row.function_index, bindings, &pass, err)) return false;
                ok = pass;
            }
            if (ok) {
                *out_function_index = node->as.try_row.function_index;
                *out_matched = true;
                return true;
            }
            bindings_truncate(bindings, checkpoint);
            return selector_exec_node(rt, selector, node->as.try_row.next, args, argc, guard, guard_user, out_function_index, bindings, out_matched, err);
        }
        case SEL_NODE_SWITCH: {
            IdmValue value = idm_nil();
            bool available = false;
            if (!selector_eval_access(rt, selector, args, argc, node->as.sw.access, &value, &available, err)) return false;
            if (available) {
                for (size_t i = 0; i < node->as.sw.case_count; i++) {
                    if (!sel_ctor_matches_value(node->as.sw.cases[i].ctor, value)) continue;
                    if (!selector_exec_node(rt, selector, node->as.sw.cases[i].node, args, argc, guard, guard_user, out_function_index, bindings, out_matched, err)) return false;
                    if (*out_matched) return true;
                }
            }
            return selector_exec_node(rt, selector, node->as.sw.default_node, args, argc, guard, guard_user, out_function_index, bindings, out_matched, err);
        }
    }
    *out_matched = false;
    return true;
}

static bool selector_add_pattern_root(IdmPatternSelector *selector, SelPat *pat, IdmError *err, IdmSpan span) {
    SelPat **next = realloc(selector->patterns, (selector->pattern_count + 1u) * sizeof(*next));
    if (!next) {
        sel_pat_free(pat);
        return idm_error_oom(err, span);
    }
    selector->patterns = next;
    selector->patterns[selector->pattern_count++] = pat;
    return true;
}

bool idm_pattern_selector_build(const IdmPatternSelectorClause *clauses, size_t clause_count, IdmPatternSelector **out, IdmError *err) {
    *out = NULL;
    IdmPatternSelector *selector = calloc(1u, sizeof(*selector));
    if (!selector) return idm_error_oom(err, idm_span_unknown(NULL));
    selector->refcount = 1u;

    uint32_t *arities = NULL;
    size_t arity_count = 0;
    for (size_t i = 0; i < clause_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < arity_count; j++) if (arities[j] == clauses[i].arity) { seen = true; break; }
        if (seen) continue;
        uint32_t *next = realloc(arities, (arity_count + 1u) * sizeof(*next));
        if (!next) { free(arities); idm_pattern_selector_free(selector); return idm_error_oom(err, idm_span_unknown(NULL)); }
        arities = next;
        arities[arity_count++] = clauses[i].arity;
    }

    selector->arities = arity_count == 0 ? NULL : calloc(arity_count, sizeof(*selector->arities));
    if (arity_count != 0 && !selector->arities) {
        free(arities);
        idm_pattern_selector_free(selector);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    selector->arity_count = arity_count;

    for (size_t a = 0; a < arity_count; a++) {
        uint32_t arity = arities[a];
        SelRow *rows = NULL;
        size_t row_count = 0;
        for (size_t c = 0; c < clause_count; c++) {
            if (clauses[c].arity != arity) continue;
            if (clauses[c].pattern_count != 0 && clauses[c].pattern_count != arity) {
                rows_destroy(rows, row_count);
                free(arities);
                idm_pattern_selector_free(selector);
                return idm_error_set(err, idm_span_unknown(NULL), "function pattern metadata arity mismatch");
            }
            SelRow row;
            memset(&row, 0, sizeof(row));
            row.function_index = clauses[c].function_index;
            row.arity = arity;
            row.has_guard = clauses[c].has_guard;
            bool trivial_no_bindings = clauses[c].trivial_match && clauses[c].pattern_local_count == 0;
            if (clauses[c].pattern_count != 0 && !trivial_no_bindings) {
                row.cells = calloc(arity, sizeof(*row.cells));
                if (!row.cells) {
                    rows_destroy(rows, row_count);
                    free(arities);
                    idm_pattern_selector_free(selector);
                    return idm_error_oom(err, idm_span_unknown(NULL));
                }
                row.cell_count = arity;
                for (uint32_t p = 0; p < arity; p++) {
                    uint32_t root = 0;
                    SelPat *pat = sel_pat_from_idm(clauses[c].patterns[p], clauses[c].pattern_locals, clauses[c].pattern_local_count, err);
                    if (!pat || !selector_add_pattern_root(selector, pat, err, clauses[c].patterns[p]->span) ||
                        !selector_root_access(selector, p, &root, err, clauses[c].patterns[p]->span)) {
                        sel_row_destroy(&row);
                        rows_destroy(rows, row_count);
                        free(arities);
                        idm_pattern_selector_free(selector);
                        return false;
                    }
                    row.cells[p].access = root;
                    row.cells[p].pattern = pat;
                }
            }
            if (!rows_append(&rows, &row_count, row, err, idm_span_unknown(NULL))) {
                rows_destroy(rows, row_count);
                free(arities);
                idm_pattern_selector_free(selector);
                return false;
            }
        }
        selector->arities[a].arity = arity;
        selector->arities[a].node = compile_rows(selector, rows, row_count, err);
        rows_destroy(rows, row_count);
        if (!selector->arities[a].node) {
            free(arities);
            idm_pattern_selector_free(selector);
            return false;
        }
    }

    free(arities);
    *out = selector;
    return true;
}

void idm_pattern_selector_retain(IdmPatternSelector *selector) {
    if (selector) selector->refcount++;
}

void idm_pattern_selector_free(IdmPatternSelector *selector) {
    if (!selector) return;
    if (selector->refcount > 1u) {
        selector->refcount--;
        return;
    }
    for (size_t i = 0; i < selector->arity_count; i++) sel_node_free(selector->arities[i].node);
    free(selector->arities);
    for (size_t i = 0; i < selector->pattern_count; i++) sel_pat_free(selector->patterns[i]);
    free(selector->patterns);
    free(selector->accesses);
    free(selector);
}

bool idm_pattern_selector_select(IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, IdmPatternGuardFn guard, void *guard_user, uint32_t *out_function_index, IdmPatternBindings *out_bindings, bool *out_has_bindings, bool *out_matched, IdmError *err) {
    *out_function_index = UINT32_MAX;
    *out_has_bindings = false;
    *out_matched = false;
    if (!selector) return true;
    const SelNode *root = NULL;
    for (size_t i = 0; i < selector->arity_count; i++) {
        if (selector->arities[i].arity == argc) {
            root = selector->arities[i].node;
            break;
        }
    }
    if (!root) return true;
    size_t checkpoint = out_bindings->count;
    if (!selector_exec_node(rt, selector, root, args, argc, guard, guard_user, out_function_index, out_bindings, out_matched, err)) {
        bindings_truncate(out_bindings, checkpoint);
        return false;
    }
    if (!*out_matched) {
        bindings_truncate(out_bindings, checkpoint);
        return true;
    }
    *out_has_bindings = out_bindings->count != checkpoint;
    return true;
}
