#include "idiom/pattern.h"
#include "idiom/regex.h"

#include <ctype.h>
#include <stdatomic.h>
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
    SEL_PAT_DICT,
    SEL_PAT_SYNTAX,
    SEL_PAT_TYPE
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
        struct { SelDictEntry *entries; size_t count; SelPat *rest; } dict;
        IdmSyntaxPattern *syntax;
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
    IdmArity arity;
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
    SEL_CTOR_DICT,
    SEL_CTOR_TYPE
} SelCtorKind;

typedef struct {
    SelCtorKind kind;
    IdmValue literal;
    const char *type;
    uint32_t count;
} SelCtor;

typedef struct SelNode SelNode;

typedef struct {
    SelCtor ctor;
    SelNode *node;
} SelCase;

typedef struct {
    unsigned char bits[32];
    bool negated;
} SelCharClass;

typedef enum {
    SEL_GUARD_LINE_START,
    SEL_GUARD_LINE_END,
    SEL_GUARD_LOOKAHEAD_POS,
    SEL_GUARD_LOOKAHEAD_NEG,
    SEL_GUARD_LOOKBEHIND_POS,
    SEL_GUARD_LOOKBEHIND_NEG,
    SEL_GUARD_WORD_BOUNDARY,
    SEL_GUARD_NOT_WORD_BOUNDARY,
    SEL_GUARD_BUFFER_START,
    SEL_GUARD_BUFFER_END,
    SEL_GUARD_BUFFER_END_NL
} SelGuardKind;

typedef enum {
    SEL_NODE_FAIL,
    SEL_NODE_TRY,
    SEL_NODE_SWITCH,
    SEL_NODE_FORK,
    SEL_NODE_BYTE,
    SEL_NODE_SAVE,
    SEL_NODE_GUARD,
    SEL_NODE_ACCEPT
} SelNodeKind;

struct SelNode {
    SelNodeKind kind;
    uint32_t index;
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
        struct {
            uint32_t first;
            uint32_t second;
        } fork;
        struct {
            SelCharClass cls;
            uint32_t next;
        } byte;
        struct {
            uint32_t slot;
            uint32_t next;
        } save;
        struct {
            SelGuardKind kind;
            uint32_t flags;
            struct SelByteProg *sub;
            uint32_t next;
        } guard;
        uint32_t accept_id;
    } as;
};

typedef struct {
    IdmArity arity;
    SelNode *node;
    bool unconditional;
    uint32_t function_index;
} SelArityCase;

typedef struct {
    SelNode **nodes;
    size_t count;
    size_t cap;
} SelNodePool;

struct IdmPatternSelector {
    atomic_size_t refcount;
    SelAccess *accesses;
    size_t access_count;
    size_t access_cap;
    SelNodePool pool;
    SelPat **patterns;
    size_t pattern_count;
    SelArityCase *arities;
    size_t arity_count;
    bool has_unconditional;
};

static IdmPattern *pat_alloc(IdmPatternKind kind, IdmSpan span) {
    IdmPattern *pat = calloc(1u, sizeof(*pat));
    if (!pat) return NULL;
    pat->kind = kind;
    pat->span = span;
    return pat;
}

static IdmSyntaxPattern *syn_pat_alloc(IdmSyntaxPatternKind kind, IdmSpan span) {
    IdmSyntaxPattern *pat = calloc(1u, sizeof(*pat));
    if (!pat) return NULL;
    pat->kind = kind;
    pat->span = span;
    return pat;
}

IdmSyntaxPattern *idm_syn_pat_wildcard(IdmSpan span) {
    return syn_pat_alloc(IDM_SYN_PAT_WILDCARD, span);
}

IdmSyntaxPattern *idm_syn_pat_bind(const char *name, IdmSpan span) {
    IdmSyntaxPattern *pat = syn_pat_alloc(IDM_SYN_PAT_BIND, span);
    if (!pat) return NULL;
    pat->as.bind.name = idm_strdup(name);
    pat->as.bind.slot = UINT32_MAX;
    if (!pat->as.bind.name) { free(pat); return NULL; }
    return pat;
}

IdmSyntaxPattern *idm_syn_pat_literal_take(IdmSyntax *literal, IdmSpan span) {
    IdmSyntaxPattern *pat = syn_pat_alloc(IDM_SYN_PAT_LITERAL, span);
    if (!pat) return NULL;
    pat->as.literal = literal;
    return pat;
}

IdmSyntaxPattern *idm_syn_pat_sequence(IdmSyntaxKind kind, IdmSyntaxPattern **items, size_t count, size_t rest_index, const char *rest_name, IdmSpan span) {
    if (kind != IDM_SYN_LIST && kind != IDM_SYN_VECTOR && kind != IDM_SYN_TUPLE && kind != IDM_SYN_DICT) return NULL;
    if (rest_index != IDM_SYN_PAT_NO_REST && rest_index > count) return NULL;
    IdmSyntaxPattern *pat = syn_pat_alloc(IDM_SYN_PAT_SEQUENCE, span);
    if (!pat) return NULL;
    pat->as.seq.kind = kind;
    pat->as.seq.items = items;
    pat->as.seq.count = count;
    pat->as.seq.rest_index = rest_index;
    pat->as.seq.rest_slot = UINT32_MAX;
    if (rest_name) {
        pat->as.seq.rest_name = idm_strdup(rest_name);
        if (!pat->as.seq.rest_name) { free(pat); return NULL; }
    }
    return pat;
}

void idm_syn_pat_free(IdmSyntaxPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case IDM_SYN_PAT_BIND:
            free(pat->as.bind.name);
            break;
        case IDM_SYN_PAT_LITERAL:
            idm_syn_free(pat->as.literal);
            break;
        case IDM_SYN_PAT_SEQUENCE:
            for (size_t i = 0; i < pat->as.seq.count; i++) idm_syn_pat_free(pat->as.seq.items[i]);
            free(pat->as.seq.items);
            free(pat->as.seq.rest_name);
            break;
        case IDM_SYN_PAT_WILDCARD:
            break;
    }
    free(pat);
}

IdmSyntaxPattern *idm_syn_pat_clone(const IdmSyntaxPattern *pat) {
    if (!pat) return NULL;
    switch (pat->kind) {
        case IDM_SYN_PAT_WILDCARD:
            return idm_syn_pat_wildcard(pat->span);
        case IDM_SYN_PAT_BIND: {
            IdmSyntaxPattern *copy = idm_syn_pat_bind(pat->as.bind.name, pat->span);
            if (copy) copy->as.bind.slot = pat->as.bind.slot;
            return copy;
        }
        case IDM_SYN_PAT_LITERAL: {
            IdmSyntax *literal = idm_syn_clone(pat->as.literal);
            if (!literal) return NULL;
            IdmSyntaxPattern *copy = idm_syn_pat_literal_take(literal, pat->span);
            if (!copy) idm_syn_free(literal);
            return copy;
        }
        case IDM_SYN_PAT_SEQUENCE: {
            IdmSyntaxPattern **items = NULL;
            if (pat->as.seq.count != 0) {
                items = calloc(pat->as.seq.count, sizeof(*items));
                if (!items) return NULL;
                for (size_t i = 0; i < pat->as.seq.count; i++) {
                    items[i] = idm_syn_pat_clone(pat->as.seq.items[i]);
                    if (!items[i]) {
                        for (size_t j = 0; j < i; j++) idm_syn_pat_free(items[j]);
                        free(items);
                        return NULL;
                    }
                }
            }
            IdmSyntaxPattern *copy = idm_syn_pat_sequence(pat->as.seq.kind, items, pat->as.seq.count, pat->as.seq.rest_index, pat->as.seq.rest_name, pat->span);
            if (!copy) {
                for (size_t i = 0; i < pat->as.seq.count; i++) idm_syn_pat_free(items[i]);
                free(items);
                return NULL;
            }
            copy->as.seq.rest_slot = pat->as.seq.rest_slot;
            return copy;
        }
    }
    return NULL;
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

IdmPattern *idm_pat_dict(IdmDictPatternEntry *entries, size_t count, IdmPattern *rest, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_DICT, span);
    if (!pat) return NULL;
    pat->as.dict.entries = entries;
    pat->as.dict.count = count;
    pat->as.dict.rest = rest;
    return pat;
}

IdmPattern *idm_pat_syntax_take(IdmSyntaxPattern *syntax, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_SYNTAX, span);
    if (!pat) return NULL;
    pat->as.syntax = syntax;
    return pat;
}

IdmPattern *idm_pat_type(const char *type, IdmSpan span) {
    IdmPattern *pat = pat_alloc(IDM_PAT_TYPE, span);
    if (!pat) return NULL;
    pat->as.name = idm_strdup(type);
    if (!pat->as.name) { free(pat); return NULL; }
    return pat;
}

void idm_pat_free(IdmPattern *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case IDM_PAT_BIND:
        case IDM_PAT_PIN:
        case IDM_PAT_TYPE:
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
            idm_pat_free(pat->as.dict.rest);
            break;
        case IDM_PAT_SYNTAX:
            idm_syn_pat_free(pat->as.syntax);
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
        case IDM_PAT_TYPE:
            return idm_pat_type(pat->as.name, pat->span);
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
            IdmPattern *rest = NULL;
            if (pat->as.dict.rest) {
                rest = idm_pat_clone(pat->as.dict.rest);
                if (!rest) {
                    for (size_t i = 0; i < pat->as.dict.count; i++) idm_pat_free(entries[i].pattern);
                    free(entries);
                    return NULL;
                }
            }
            return idm_pat_dict(entries, pat->as.dict.count, rest, pat->span);
        }
        case IDM_PAT_SYNTAX: {
            IdmSyntaxPattern *syntax = idm_syn_pat_clone(pat->as.syntax);
            if (!syntax) return NULL;
            IdmPattern *out = idm_pat_syntax_take(syntax, pat->span);
            if (!out) idm_syn_pat_free(syntax);
            return out;
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

static bool sel_dict_key_in_entries(IdmValue key, const SelDictEntry *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (idm_value_equal(key, entries[i].key)) return true;
    }
    return false;
}

static bool sel_dict_rest_value(IdmRuntime *rt, IdmValue value, const SelDictEntry *entries, size_t count, IdmValue *out, IdmError *err, IdmSpan span) {
    if (!rt) return idm_error_set(err, span, "runtime required for dict rest pattern");
    size_t n = idm_dict_count(value);
    IdmDictEntry *rest = n == 0 ? NULL : calloc(n, sizeof(*rest));
    if (n != 0 && !rest) return idm_error_oom(err, span);
    size_t rest_count = 0;
    for (size_t i = 0; i < n; i++) {
        IdmValue key = idm_nil();
        IdmValue val = idm_nil();
        if (!idm_dict_entry(value, i, &key, &val)) {
            free(rest);
            return idm_error_set(err, span, "dict rest pattern failed to read entry");
        }
        if (sel_dict_key_in_entries(key, entries, count)) continue;
        rest[rest_count].key = key;
        rest[rest_count].value = val;
        rest_count++;
    }
    *out = idm_dict(rt, rest, rest_count, err);
    free(rest);
    return !(err && err->present);
}

static bool syntax_literal_equal(const IdmSyntax *a, const IdmSyntax *b) {
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            return strcmp(a->as.text, b->as.text) == 0;
        case IDM_SYN_INT:
            return a->as.integer == b->as.integer;
        case IDM_SYN_FLOAT:
            return a->as.real == b->as.real;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if (a->as.seq.count != b->as.seq.count) return false;
            for (size_t i = 0; i < a->as.seq.count; i++) {
                if (!syntax_literal_equal(a->as.seq.items[i], b->as.seq.items[i])) return false;
            }
            return true;
    }
    return false;
}

static bool syntax_pattern_literal_word(const IdmSyntaxPattern *pat, const char *word) {
    return pat && pat->kind == IDM_SYN_PAT_LITERAL && pat->as.literal &&
           pat->as.literal->kind == IDM_SYN_WORD && strcmp(pat->as.literal->as.text, word) == 0;
}

static bool syntax_pattern_match(IdmRuntime *rt, const IdmSyntaxPattern *pat, const IdmSyntax *syn, IdmPatternBindings *bindings, IdmError *err);

static bool syntax_pattern_literal_key(const IdmSyntaxPattern *pat, const IdmSyntax **out) {
    if (!pat || pat->kind != IDM_SYN_PAT_LITERAL || !pat->as.literal) return false;
    if (out) *out = pat->as.literal;
    return true;
}

static bool syntax_pattern_keyed_dict_shape(const IdmSyntaxPattern *pat, size_t *out_pair_item_count, const IdmSyntaxPattern **out_rest) {
    if (!pat || pat->kind != IDM_SYN_PAT_SEQUENCE || pat->as.seq.kind != IDM_SYN_DICT ||
        pat->as.seq.rest_index != IDM_SYN_PAT_NO_REST) {
        return false;
    }
    size_t count = pat->as.seq.count;
    size_t dot_index = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!syntax_pattern_literal_word(pat->as.seq.items[i], ".")) continue;
        if (dot_index != SIZE_MAX) return false;
        dot_index = i;
    }
    *out_rest = NULL;
    if (dot_index != SIZE_MAX) {
        if ((dot_index % 2u) != 0 || dot_index + 2u != count) return false;
        *out_rest = pat->as.seq.items[dot_index + 1u];
        count = dot_index;
    }
    if ((count % 2u) != 0) return false;
    for (size_t i = 0; i < count; i += 2u) {
        if (!syntax_pattern_literal_key(pat->as.seq.items[i], NULL)) return false;
    }
    *out_pair_item_count = count;
    return true;
}

static const IdmSyntax *syntax_dict_value_for_key(const IdmSyntax *dict, const IdmSyntax *key) {
    if (!dict || dict->kind != IDM_SYN_DICT || (dict->as.seq.count % 2u) != 0) return NULL;
    for (size_t i = dict->as.seq.count; i > 0; i -= 2u) {
        if (syntax_literal_equal(dict->as.seq.items[i - 2u], key)) return dict->as.seq.items[i - 1u];
    }
    return NULL;
}

static bool syntax_pattern_dict_requires_key(const IdmSyntaxPattern *pat, size_t pair_item_count, const IdmSyntax *key) {
    for (size_t i = 0; i < pair_item_count; i += 2u) {
        const IdmSyntax *required = NULL;
        if (syntax_pattern_literal_key(pat->as.seq.items[i], &required) && syntax_literal_equal(required, key)) return true;
    }
    return false;
}

static IdmSyntax *syntax_pattern_dict_rest_syntax(const IdmSyntax *syn, const IdmSyntaxPattern *pat, size_t pair_item_count) {
    IdmSyntax *rest = idm_syn_dict(syn->span);
    if (!rest) return NULL;
    for (size_t i = 0; i < syn->as.seq.count; i += 2u) {
        if (syntax_pattern_dict_requires_key(pat, pair_item_count, syn->as.seq.items[i])) continue;
        IdmSyntax *key = idm_syn_clone(syn->as.seq.items[i]);
        IdmSyntax *value = idm_syn_clone(syn->as.seq.items[i + 1u]);
        if (!key || !value || !idm_syn_append(rest, key) || !idm_syn_append(rest, value)) {
            idm_syn_free(key);
            idm_syn_free(value);
            idm_syn_free(rest);
            return NULL;
        }
    }
    return rest;
}

static bool syntax_pattern_match_keyed_dict(IdmRuntime *rt, const IdmSyntaxPattern *pat, const IdmSyntax *syn, IdmPatternBindings *bindings, IdmError *err) {
    size_t pair_item_count = 0;
    const IdmSyntaxPattern *rest_pat = NULL;
    if (!syntax_pattern_keyed_dict_shape(pat, &pair_item_count, &rest_pat)) return false;
    if (!syn || syn->kind != IDM_SYN_DICT || (syn->as.seq.count % 2u) != 0) return false;
    for (size_t i = 0; i < pair_item_count; i += 2u) {
        const IdmSyntax *key = NULL;
        syntax_pattern_literal_key(pat->as.seq.items[i], &key);
        const IdmSyntax *value = syntax_dict_value_for_key(syn, key);
        if (!value) return false;
        if (!syntax_pattern_match(rt, pat->as.seq.items[i + 1u], value, bindings, err)) return false;
        if (err && err->present) return false;
    }
    if (rest_pat) {
        IdmSyntax *rest = syntax_pattern_dict_rest_syntax(syn, pat, pair_item_count);
        if (!rest) return idm_error_oom(err, syn->span);
        bool ok = syntax_pattern_match(rt, rest_pat, rest, bindings, err);
        idm_syn_free(rest);
        return ok;
    }
    return true;
}

static const IdmSyntax *syntax_pattern_unwrap_expr_group(const IdmSyntaxPattern *pat, const IdmSyntax *syn) {
    if (!pat || pat->kind != IDM_SYN_PAT_SEQUENCE || pat->as.seq.kind != IDM_SYN_LIST ||
        pat->as.seq.count == 0 || !syntax_pattern_literal_word(pat->as.seq.items[0], "%-expr")) {
        return syn;
    }
    if (!syn || syn->kind != IDM_SYN_LIST || syn->as.seq.count != 2u ||
        syn->as.seq.items[0]->kind != IDM_SYN_WORD || strcmp(syn->as.seq.items[0]->as.text, "%-group") != 0) {
        return syn;
    }
    const IdmSyntax *inner = syn->as.seq.items[1];
    if (!inner || inner->kind != IDM_SYN_LIST || inner->as.seq.count == 0 ||
        inner->as.seq.items[0]->kind != IDM_SYN_WORD || strcmp(inner->as.seq.items[0]->as.text, "%-expr") != 0) {
        return syn;
    }
    return inner;
}

static bool syntax_bound_value_equal(IdmValue a, IdmValue b) {
    if (idm_value_tag(a) == IDM_VAL_SYNTAX && idm_value_tag(b) == IDM_VAL_SYNTAX) {
        return syntax_literal_equal(idm_syntax_value_get(a), idm_syntax_value_get(b));
    }
    if (idm_is_empty_list(a) && idm_is_empty_list(b)) return true;
    if (idm_is_pair(a) && idm_is_pair(b)) {
        return syntax_bound_value_equal(idm_car(a, NULL), idm_car(b, NULL)) &&
               syntax_bound_value_equal(idm_cdr(a, NULL), idm_cdr(b, NULL));
    }
    return idm_value_equal(a, b);
}

static const IdmValue *syntax_pattern_existing_binding(const IdmPatternBindings *bindings, const char *name, uint32_t slot) {
    const IdmValue *existing = slot != UINT32_MAX ? idm_pattern_bindings_get_slot(bindings, slot) : NULL;
    return existing ? existing : idm_pattern_bindings_get(bindings, name);
}

static bool syntax_pattern_bind_value(IdmRuntime *rt, IdmPatternBindings *bindings, const char *name, uint32_t slot, const IdmSyntax *syn, IdmError *err, IdmSpan span) {
    if (!rt) return idm_error_set(err, span, "runtime required for syntax pattern binding");
    IdmValue value = idm_syntax_value(rt, syn, err);
    if (err && err->present) return false;
    const IdmValue *existing = syntax_pattern_existing_binding(bindings, name, slot);
    if (existing) return syntax_bound_value_equal(*existing, value);
    if (!idm_pattern_bindings_add_slot(bindings, name, slot, value)) {
        return idm_error_oom(err, span);
    }
    return true;
}

static bool syntax_pattern_bind_rest(IdmRuntime *rt, IdmPatternBindings *bindings, const char *name, uint32_t slot, const IdmSyntax *seq, size_t start, size_t end, IdmError *err, IdmSpan span) {
    if (!name) return true;
    if (!rt) return idm_error_set(err, span, "runtime required for syntax rest pattern binding");
    IdmValue list = idm_empty_list();
    for (size_t i = end; i > start; i--) {
        IdmValue item = idm_syntax_value(rt, seq->as.seq.items[i - 1u], err);
        if (err && err->present) return false;
        list = idm_cons(rt, item, list, err);
        if (err && err->present) return false;
    }
    const IdmValue *existing = syntax_pattern_existing_binding(bindings, name, slot);
    if (existing) return syntax_bound_value_equal(*existing, list);
    if (!idm_pattern_bindings_add_slot(bindings, name, slot, list)) {
        return idm_error_oom(err, span);
    }
    return true;
}

static bool syntax_pattern_match(IdmRuntime *rt, const IdmSyntaxPattern *pat, const IdmSyntax *syn, IdmPatternBindings *bindings, IdmError *err) {
    if (!pat || !syn) return false;
    size_t checkpoint = bindings->count;
    bool ok = false;
    switch (pat->kind) {
        case IDM_SYN_PAT_WILDCARD:
            ok = true;
            break;
        case IDM_SYN_PAT_BIND:
            ok = syntax_pattern_bind_value(rt, bindings, pat->as.bind.name, pat->as.bind.slot, syn, err, pat->span);
            break;
        case IDM_SYN_PAT_LITERAL:
            ok = syntax_literal_equal(pat->as.literal, syn);
            break;
        case IDM_SYN_PAT_SEQUENCE: {
            syn = syntax_pattern_unwrap_expr_group(pat, syn);
            if (pat->as.seq.kind == IDM_SYN_DICT) {
                size_t keyed_count = 0;
                const IdmSyntaxPattern *keyed_rest = NULL;
                if (syntax_pattern_keyed_dict_shape(pat, &keyed_count, &keyed_rest)) {
                    ok = syntax_pattern_match_keyed_dict(rt, pat, syn, bindings, err);
                    break;
                }
            }
            if (syn->kind != pat->as.seq.kind) {
                ok = false;
                break;
            }
            bool has_rest = pat->as.seq.rest_index != IDM_SYN_PAT_NO_REST;
            size_t fixed_count = pat->as.seq.count;
            size_t syn_count = syn->as.seq.count;
            if ((!has_rest && syn_count != fixed_count) || (has_rest && syn_count < fixed_count)) {
                ok = false;
                break;
            }
            ok = true;
            if (!has_rest) {
                for (size_t i = 0; i < fixed_count; i++) {
                    if (!syntax_pattern_match(rt, pat->as.seq.items[i], syn->as.seq.items[i], bindings, err)) { ok = false; break; }
                    if (err && err->present) break;
                }
                break;
            }
            size_t prefix = pat->as.seq.rest_index;
            size_t suffix = fixed_count - prefix;
            for (size_t i = 0; ok && i < prefix; i++) {
                ok = syntax_pattern_match(rt, pat->as.seq.items[i], syn->as.seq.items[i], bindings, err);
                if (err && err->present) break;
            }
            size_t suffix_start = syn_count - suffix;
            for (size_t i = 0; ok && i < suffix; i++) {
                ok = syntax_pattern_match(rt, pat->as.seq.items[prefix + i], syn->as.seq.items[suffix_start + i], bindings, err);
                if (err && err->present) break;
            }
            if (ok) ok = syntax_pattern_bind_rest(rt, bindings, pat->as.seq.rest_name, pat->as.seq.rest_slot, syn, prefix, suffix_start, err, pat->span);
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
        case SEL_PAT_TYPE:
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
            sel_pat_free(pat->as.dict.rest);
            break;
        case SEL_PAT_SYNTAX:
            idm_syn_pat_free(pat->as.syntax);
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

static void syntax_pattern_assign_slots(IdmSyntaxPattern *pat, const IdmPatternLocal *locals, uint32_t local_count) {
    if (!pat) return;
    switch (pat->kind) {
        case IDM_SYN_PAT_BIND:
            pat->as.bind.slot = selector_local_slot(locals, local_count, pat->as.bind.name);
            break;
        case IDM_SYN_PAT_SEQUENCE:
            if (pat->as.seq.rest_name) pat->as.seq.rest_slot = selector_local_slot(locals, local_count, pat->as.seq.rest_name);
            for (size_t i = 0; i < pat->as.seq.count; i++) syntax_pattern_assign_slots(pat->as.seq.items[i], locals, local_count);
            break;
        case IDM_SYN_PAT_WILDCARD:
        case IDM_SYN_PAT_LITERAL:
            break;
    }
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
        case IDM_PAT_TYPE:
            out = sel_pat_alloc(SEL_PAT_TYPE, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.name.name = idm_strdup(pat->as.name);
            out->as.name.slot = UINT32_MAX;
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
            if (pat->as.dict.rest) {
                out->as.dict.rest = sel_pat_from_idm(pat->as.dict.rest, locals, local_count, err);
                if (!out->as.dict.rest) { sel_pat_free(out); return NULL; }
            }
            return out;
        }
        case IDM_PAT_SYNTAX:
            out = sel_pat_alloc(SEL_PAT_SYNTAX, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.syntax = idm_syn_pat_clone(pat->as.syntax);
            if (!out->as.syntax) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            syntax_pattern_assign_slots(out->as.syntax, locals, local_count);
            return out;
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
    switch (idm_value_tag(value)) {
        case IDM_VAL_NIL:
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
        case IDM_VAL_INT:
        case IDM_VAL_FLOAT:
        case IDM_VAL_STRING:
        case IDM_VAL_BIGNUM:
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
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
            if (pat->as.dict.rest) return false;
            out->kind = SEL_CTOR_DICT;
            out->count = 0;
            return true;
        case SEL_PAT_TYPE:
            out->kind = SEL_CTOR_TYPE;
            out->type = pat->as.name.name;
            out->count = 0;
            return true;
        case SEL_PAT_WILDCARD:
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
        case SEL_PAT_SYNTAX:
            return false;
    }
    return false;
}

static bool sel_ctor_equal(SelCtor a, SelCtor b) {
    if (a.kind != b.kind || a.count != b.count) return false;
    if (a.kind == SEL_CTOR_TYPE) return strcmp(a.type ? a.type : "", b.type ? b.type : "") == 0;
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
        case SEL_CTOR_TYPE:
            return idm_value_matches_type_name(value, ctor.type);
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

static void sel_node_destroy_contents(SelNode *node) {
    if (!node) return;
    switch (node->kind) {
        case SEL_NODE_TRY:
            for (size_t i = 0; i < node->as.try_row.action_count; i++) sel_action_destroy(&node->as.try_row.actions[i]);
            free(node->as.try_row.actions);
            free(node->as.try_row.residuals);
            break;
        case SEL_NODE_SWITCH:
            free(node->as.sw.cases);
            break;
        case SEL_NODE_GUARD:
            idm_byteprog_free(node->as.guard.sub);
            break;
        case SEL_NODE_FAIL:
        case SEL_NODE_FORK:
        case SEL_NODE_BYTE:
        case SEL_NODE_SAVE:
        case SEL_NODE_ACCEPT:
            break;
    }
}

static SelNode *pool_new(SelNodePool *pool, SelNodeKind kind, IdmError *err) {
    if (pool->count == pool->cap) {
        size_t cap = pool->cap ? pool->cap * 2u : 16u;
        SelNode **grown = realloc(pool->nodes, cap * sizeof(*grown));
        if (!grown) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
        pool->nodes = grown;
        pool->cap = cap;
    }
    SelNode *node = calloc(1u, sizeof(*node));
    if (!node) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    node->kind = kind;
    node->index = (uint32_t)pool->count;
    pool->nodes[pool->count++] = node;
    return node;
}

static void pool_destroy(SelNodePool *pool) {
    for (size_t i = 0; i < pool->count; i++) {
        sel_node_destroy_contents(pool->nodes[i]);
        free(pool->nodes[i]);
    }
    free(pool->nodes);
    pool->nodes = NULL;
    pool->count = 0;
    pool->cap = 0;
}

static SelNode *sel_node_new(IdmPatternSelector *selector, SelNodeKind kind, IdmError *err) {
    return pool_new(&selector->pool, kind, err);
}

static SelNode *sel_node_fail(IdmPatternSelector *selector, IdmError *err) {
    return sel_node_new(selector, SEL_NODE_FAIL, err);
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
        case SEL_PAT_TYPE:
            return true;
        case SEL_PAT_WILDCARD:
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
        case SEL_PAT_SYNTAX:
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
    if (pat->kind == SEL_PAT_SYNTAX) {
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

static SelNode *make_try_node(IdmPatternSelector *selector, const SelRow *row, SelNode *next, IdmError *err) {
    SelNode *node = sel_node_new(selector, SEL_NODE_TRY, err);
    if (!node) return NULL;
    node->as.try_row.function_index = row->function_index;
    node->as.try_row.has_guard = row->has_guard;
    node->as.try_row.next = next;
    if (!copy_actions(&node->as.try_row.actions, &node->as.try_row.action_count, row->actions, row->action_count, err, idm_span_unknown(NULL))) {
        return NULL;
    }
    if (row->cell_count != 0) {
        node->as.try_row.residuals = malloc(row->cell_count * sizeof(*node->as.try_row.residuals));
        if (!node->as.try_row.residuals) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
        memcpy(node->as.try_row.residuals, row->cells, row->cell_count * sizeof(*node->as.try_row.residuals));
        node->as.try_row.residual_count = row->cell_count;
    }
    return node;
}

static SelNode *compile_rows(IdmPatternSelector *selector, const SelRow *rows, size_t row_count, IdmError *err) {
    if (row_count == 0) return sel_node_fail(selector, err);
    uint32_t access = 0;
    if (!choose_access(rows, row_count, &access)) {
        SelNode *next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (!next) return NULL;
        return make_try_node(selector, &rows[0], next, err);
    }

    SelCtor *ctors = NULL;
    size_t ctor_count = 0;
    if (!collect_ctors(rows, row_count, access, &ctors, &ctor_count, err)) return NULL;
    if (ctor_count == 0) {
        free(ctors);
        SelNode *next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (!next) return NULL;
        return make_try_node(selector, &rows[0], next, err);
    }

    SelNode *node = sel_node_new(selector, SEL_NODE_SWITCH, err);
    if (!node) {
        free(ctors);
        return NULL;
    }
    node->as.sw.access = access;
    node->as.sw.cases = calloc(ctor_count, sizeof(*node->as.sw.cases));
    if (!node->as.sw.cases) {
        free(ctors);
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
                return NULL;
            }
            if (include && !rows_append(&case_rows, &case_count, specialized, err, idm_span_unknown(NULL))) {
                rows_destroy(case_rows, case_count);
                free(ctors);
                return NULL;
            }
        }
        node->as.sw.cases[c].ctor = ctors[c];
        node->as.sw.cases[c].node = compile_rows(selector, case_rows, case_count, err);
        rows_destroy(case_rows, case_count);
        if (!node->as.sw.cases[c].node) {
            free(ctors);
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
            return NULL;
        }
        if (include && !rows_append(&default_rows, &default_count, specialized, err, idm_span_unknown(NULL))) {
            rows_destroy(default_rows, default_count);
            free(ctors);
            return NULL;
        }
    }
    node->as.sw.default_node = compile_rows(selector, default_rows, default_count, err);
    rows_destroy(default_rows, default_count);
    free(ctors);
    if (!node->as.sw.default_node) {
        return NULL;
    }
    return node;
}

static bool sel_node_unconditional(const SelNode *node, uint32_t *out_function_index) {
    if (!node || node->kind != SEL_NODE_TRY) return false;
    if (node->as.try_row.action_count != 0 || node->as.try_row.residual_count != 0 || node->as.try_row.has_guard) return false;
    *out_function_index = node->as.try_row.function_index;
    return true;
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
            if (!ok || (idm_value_tag(parent) != IDM_VAL_VECTOR && idm_value_tag(parent) != IDM_VAL_TUPLE) || access->index >= idm_sequence_count(parent)) return true;
            *out = idm_sequence_item(parent, access->index, err);
            if (err && err->present) return false;
            *out_available = true;
            return true;
        }
        case SEL_ACCESS_SEQ_REST: {
            IdmValue parent = idm_nil();
            bool ok = false;
            if (!selector_eval_access(rt, selector, args, argc, access->parent, &parent, &ok, err)) return false;
            if (!ok || idm_value_tag(parent) != access->seq_tag) return true;
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
            if (idm_value_tag(value) != tag || idm_sequence_count(value) != pat->as.seq.count) return false;
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
            if (idm_value_tag(value) != tag || idm_sequence_count(value) < pat->as.seq_rest.count) return false;
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
            if (pat->as.dict.rest) {
                IdmValue rest = idm_nil();
                if (!sel_dict_rest_value(rt, value, pat->as.dict.entries, pat->as.dict.count, &rest, err, pat->span)) return false;
                return selector_match_value(rt, rest, pat->as.dict.rest, bindings, err);
            }
            return true;
        case SEL_PAT_SYNTAX:
            if (idm_value_tag(value) != IDM_VAL_SYNTAX) return false;
            return syntax_pattern_match(rt, pat->as.syntax, idm_syntax_value_get(value), bindings, err);
        case SEL_PAT_TYPE:
            return idm_value_matches_type_name(value, pat->as.name.name);
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
        case SEL_NODE_FORK:
        case SEL_NODE_BYTE:
        case SEL_NODE_SAVE:
        case SEL_NODE_GUARD:
        case SEL_NODE_ACCEPT:
            break;
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
    atomic_init(&selector->refcount, 1u);

    IdmArity *arities = NULL;
    size_t arity_count = 0;
    for (size_t i = 0; i < clause_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < arity_count; j++) if (idm_arity_equal(&arities[j], &clauses[i].arity)) { seen = true; break; }
        if (seen) continue;
        IdmArity *next = realloc(arities, (arity_count + 1u) * sizeof(*next));
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
        IdmArity arity = arities[a];
        SelRow *rows = NULL;
        size_t row_count = 0;
        for (size_t c = 0; c < clause_count; c++) {
            if (!idm_arity_equal(&clauses[c].arity, &arity)) continue;
            if (clauses[c].pattern_count != 0 &&
                (arity.min != clauses[c].pattern_count ||
                 arity.max != clauses[c].pattern_count ||
                 !idm_arity_accepts(&arity, clauses[c].pattern_count))) {
                rows_destroy(rows, row_count);
                free(arities);
                idm_pattern_selector_free(selector);
                return idm_error_set(err, idm_span_unknown(NULL), "function pattern metadata arity mismatch");
            }
            uint32_t exact_arity = clauses[c].pattern_count;
            if (clauses[c].pattern_count == 0) exact_arity = arity.min;
            SelRow row;
            memset(&row, 0, sizeof(row));
            row.function_index = clauses[c].function_index;
            row.arity = arity;
            row.has_guard = clauses[c].has_guard;
            bool trivial_no_bindings = clauses[c].trivial_match && clauses[c].pattern_local_count == 0;
            if (clauses[c].pattern_count != 0 && !trivial_no_bindings) {
                row.cells = calloc(exact_arity, sizeof(*row.cells));
                if (!row.cells) {
                    rows_destroy(rows, row_count);
                    free(arities);
                    idm_pattern_selector_free(selector);
                    return idm_error_oom(err, idm_span_unknown(NULL));
                }
                row.cell_count = exact_arity;
                for (uint32_t p = 0; p < exact_arity; p++) {
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
        selector->arities[a].unconditional = sel_node_unconditional(selector->arities[a].node, &selector->arities[a].function_index);
        if (selector->arities[a].unconditional) selector->has_unconditional = true;
    }

    free(arities);
    *out = selector;
    return true;
}

void idm_pattern_selector_retain(IdmPatternSelector *selector) {
    if (selector) atomic_fetch_add_explicit(&selector->refcount, 1u, memory_order_relaxed);
}

void idm_pattern_selector_free(IdmPatternSelector *selector) {
    if (!selector) return;
    if (atomic_fetch_sub_explicit(&selector->refcount, 1u, memory_order_acq_rel) != 1u) return;
    pool_destroy(&selector->pool);
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
    const SelArityCase *arity_case = NULL;
    for (size_t i = 0; i < selector->arity_count; i++) {
        if (idm_arity_accepts(&selector->arities[i].arity, argc)) {
            arity_case = &selector->arities[i];
            break;
        }
    }
    if (!arity_case) return true;
    if (arity_case->unconditional) {
        *out_function_index = arity_case->function_index;
        *out_matched = true;
        return true;
    }
    size_t checkpoint = out_bindings->count;
    if (!selector_exec_node(rt, selector, arity_case->node, args, argc, guard, guard_user, out_function_index, out_bindings, out_matched, err)) {
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

typedef struct {
    bool set;
    size_t start;
    size_t end;
} SelCapture;

typedef struct {
    uint32_t *nodes;
    size_t count;
    bool has_match;
    bool dynamic;
} SelTestClosure;

struct SelByteProg {
    SelNodePool pool;
    uint32_t start;
    size_t capture_count;
    uint32_t flags;
    SelTestClosure *test_closures;
};

typedef struct {
    uint32_t node;
    size_t pos;
    size_t capture_index;
} SelByteState;

typedef struct {
    SelByteState *items;
    SelCapture *captures;
    size_t count;
    size_t cap;
    size_t capture_count;
} SelByteStateVec;

typedef struct {
    bool matched;
    size_t end;
    size_t accept_id;
    SelCapture *captures;
} SelByteMatch;

enum { SEL_STACK_CAPTURE_LIMIT = 16 };
enum { SEL_MAX_CLOSURE_DEPTH = 10000 };

static bool sel_class_has(const SelCharClass *cls, unsigned char c) {
    bool in = (cls->bits[c >> 3] & (unsigned char)(1u << (c & 7u))) != 0;
    return cls->negated ? !in : in;
}
static bool sel_at_line_start(const char *s, size_t pos) { return pos == 0 || s[pos - 1u] == '\n'; }
static bool sel_at_line_end(const char *s, size_t len, size_t pos) { return pos == len || s[pos] == '\n'; }
static bool sel_is_word_byte(unsigned char c) { return isalnum(c) || c == '_'; }
static bool sel_at_word_boundary(const char *s, size_t len, size_t pos) {
    bool before = pos > 0 && sel_is_word_byte((unsigned char)s[pos - 1u]);
    bool after = pos < len && sel_is_word_byte((unsigned char)s[pos]);
    return before != after;
}

static void sel_byte_state_vec_destroy(SelByteStateVec *vec) {
    free(vec->items);
    free(vec->captures);
    vec->items = NULL;
    vec->captures = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static bool sel_byte_state_push(SelByteStateVec *vec, uint32_t node, size_t pos, const SelCapture *captures, IdmError *err) {
    if (vec->count == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2u : 16u;
        SelByteState *items = realloc(vec->items, cap * sizeof(*items));
        if (!items) return idm_error_oom(err, idm_span_unknown(NULL));
        vec->items = items;
        if (vec->capture_count != 0) {
            SelCapture *caps = realloc(vec->captures, cap * vec->capture_count * sizeof(*caps));
            if (!caps) return idm_error_oom(err, idm_span_unknown(NULL));
            vec->captures = caps;
        }
        vec->cap = cap;
    }
    SelByteState *dst = &vec->items[vec->count];
    dst->node = node;
    dst->pos = pos;
    if (vec->capture_count != 0) {
        dst->capture_index = vec->count;
        memcpy(vec->captures + vec->count * vec->capture_count, captures, vec->capture_count * sizeof(*vec->captures));
    } else {
        dst->capture_index = 0;
    }
    vec->count++;
    return true;
}

static const SelCapture *sel_byte_state_captures(const SelByteStateVec *vec, const SelByteState *state) {
    return vec->capture_count == 0 ? NULL : vec->captures + state->capture_index * vec->capture_count;
}

static void sel_byte_match_destroy(SelByteMatch *m) {
    free(m->captures);
    m->captures = NULL;
    m->matched = false;
    m->end = 0;
    m->accept_id = 0;
}

static bool sel_byte_match_take_best(SelByteMatch *m, size_t end, size_t accept_id, const SelCapture *captures, size_t capture_count, IdmError *err) {
    if (m->matched && (end < m->end || (end == m->end && accept_id >= m->accept_id))) return true;
    SelCapture *copy = capture_count == 0 ? NULL : malloc(capture_count * sizeof(*copy));
    if (capture_count != 0 && !copy) return idm_error_oom(err, idm_span_unknown(NULL));
    if (capture_count != 0) memcpy(copy, captures, capture_count * sizeof(*copy));
    free(m->captures);
    m->captures = copy;
    m->matched = true;
    m->end = end;
    m->accept_id = accept_id;
    return true;
}

static bool sel_byte_run(const SelByteProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, SelByteMatch *out, IdmError *err);

static bool sel_look_matches(const SelByteProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    SelByteMatch m = {0};
    bool ok = sel_byte_run(prog, s, len, pos, false, 0, false, &m, err);
    bool matched = ok && m.matched;
    sel_byte_match_destroy(&m);
    return ok && matched;
}

static bool sel_lookbehind_matches(const SelByteProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    for (size_t start = 0; start <= pos; start++) {
        SelByteMatch m = {0};
        bool ok = sel_byte_run(prog, s, len, start, true, pos, false, &m, err);
        bool matched = ok && m.matched;
        sel_byte_match_destroy(&m);
        if (!ok) return false;
        if (matched) return true;
    }
    return false;
}

static bool sel_closure(const SelByteProg *prog, SelByteStateVec *vec, uint32_t *marks, uint32_t mark, const char *s, size_t len, uint32_t node_index, size_t pos, const SelCapture *captures, bool capture, unsigned depth, IdmError *err) {
    if (node_index >= prog->pool.count || pos > len) return idm_error_set(err, idm_span_unknown(NULL), "byte automaton state out of bounds");
    if (marks[node_index] == mark) return true;
    marks[node_index] = mark;
    if (depth > SEL_MAX_CLOSURE_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "regex too complex to evaluate");
    const SelNode *node = prog->pool.nodes[node_index];
    switch (node->kind) {
        case SEL_NODE_FORK:
            return sel_closure(prog, vec, marks, mark, s, len, node->as.fork.first, pos, captures, capture, depth + 1u, err)
                && sel_closure(prog, vec, marks, mark, s, len, node->as.fork.second, pos, captures, capture, depth + 1u, err);
        case SEL_NODE_SAVE: {
            if (!capture) return sel_closure(prog, vec, marks, mark, s, len, node->as.save.next, pos, captures, capture, depth + 1u, err);
            SelCapture stack_next[SEL_STACK_CAPTURE_LIMIT];
            SelCapture *next = prog->capture_count <= SEL_STACK_CAPTURE_LIMIT ? stack_next : malloc(prog->capture_count * sizeof(*next));
            if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
            memcpy(next, captures, prog->capture_count * sizeof(*next));
            size_t cap = node->as.save.slot / 2u;
            if (cap < prog->capture_count) {
                next[cap].set = true;
                if ((node->as.save.slot & 1u) == 0) { next[cap].start = pos; next[cap].end = pos; }
                else next[cap].end = pos;
            }
            bool ok = sel_closure(prog, vec, marks, mark, s, len, node->as.save.next, pos, next, capture, depth + 1u, err);
            if (next != stack_next) free(next);
            return ok;
        }
        case SEL_NODE_GUARD: {
            bool pass = false;
            switch (node->as.guard.kind) {
                case SEL_GUARD_LINE_START:
                    pass = pos == 0 || ((node->as.guard.flags & IDM_REGEX_MULTILINE) != 0 && sel_at_line_start(s, pos));
                    break;
                case SEL_GUARD_LINE_END:
                    pass = pos == len || ((node->as.guard.flags & IDM_REGEX_MULTILINE) != 0 && sel_at_line_end(s, len, pos));
                    break;
                case SEL_GUARD_WORD_BOUNDARY:
                    pass = sel_at_word_boundary(s, len, pos);
                    break;
                case SEL_GUARD_NOT_WORD_BOUNDARY:
                    pass = !sel_at_word_boundary(s, len, pos);
                    break;
                case SEL_GUARD_BUFFER_START:
                    pass = pos == 0;
                    break;
                case SEL_GUARD_BUFFER_END:
                    pass = pos == len;
                    break;
                case SEL_GUARD_BUFFER_END_NL:
                    pass = pos == len || (pos + 1u == len && s[pos] == '\n');
                    break;
                case SEL_GUARD_LOOKAHEAD_POS:
                case SEL_GUARD_LOOKAHEAD_NEG: {
                    bool m = sel_look_matches(node->as.guard.sub, s, len, pos, err);
                    if (err && err->present) return false;
                    pass = (node->as.guard.kind == SEL_GUARD_LOOKAHEAD_POS) ? m : !m;
                    break;
                }
                case SEL_GUARD_LOOKBEHIND_POS:
                case SEL_GUARD_LOOKBEHIND_NEG: {
                    bool m = sel_lookbehind_matches(node->as.guard.sub, s, len, pos, err);
                    if (err && err->present) return false;
                    pass = (node->as.guard.kind == SEL_GUARD_LOOKBEHIND_POS) ? m : !m;
                    break;
                }
            }
            if (pass) return sel_closure(prog, vec, marks, mark, s, len, node->as.guard.next, pos, captures, capture, depth + 1u, err);
            return true;
        }
        case SEL_NODE_BYTE:
        case SEL_NODE_ACCEPT:
            return sel_byte_state_push(vec, node_index, pos, captures, err);
        case SEL_NODE_FAIL:
        case SEL_NODE_TRY:
        case SEL_NODE_SWITCH:
            return idm_error_set(err, idm_span_unknown(NULL), "structural node in byte automaton");
    }
    return true;
}

static bool sel_byte_run(const SelByteProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, SelByteMatch *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!prog || prog->pool.count == 0 || offset > len || (exact_end && end_pos > len)) return true;
    if (prog->pool.count > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, idm_span_unknown(NULL), "byte automaton too large");
    uint32_t mark_stack[256];
    uint32_t *marks = prog->pool.count <= 256u ? mark_stack : calloc(prog->pool.count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, prog->pool.count * sizeof(*mark_stack));
    size_t capture_count = capture ? prog->capture_count : 0u;
    SelCapture initial_stack[SEL_STACK_CAPTURE_LIMIT];
    SelCapture *initial = capture_count <= SEL_STACK_CAPTURE_LIMIT ? initial_stack : calloc(capture_count, sizeof(*initial));
    if (!initial) { if (marks != mark_stack) free(marks); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (initial == initial_stack) memset(initial_stack, 0, capture_count * sizeof(*initial_stack));
    SelByteStateVec active = { .items = NULL, .captures = NULL, .count = 0, .cap = 0, .capture_count = capture_count };
    SelByteStateVec next = { .items = NULL, .captures = NULL, .count = 0, .cap = 0, .capture_count = capture_count };
    uint32_t mark = 1u;
    bool ok = sel_closure(prog, &active, marks, mark, s, len, prog->start, offset, initial, capture, 0u, err);
    if (initial != initial_stack) free(initial);
    while (ok && active.count != 0) {
        next.count = 0;
        mark++;
        if (mark == 0) { memset(marks, 0, prog->pool.count * sizeof(*marks)); mark = 1u; }
        for (size_t i = 0; ok && i < active.count; i++) {
            SelByteState *state = &active.items[i];
            const SelCapture *caps = sel_byte_state_captures(&active, state);
            const SelNode *node = prog->pool.nodes[state->node];
            if (node->kind == SEL_NODE_ACCEPT) {
                if ((!exact_end || state->pos == end_pos) && !sel_byte_match_take_best(out, state->pos, node->as.accept_id, caps, capture_count, err)) ok = false;
            } else if (node->kind == SEL_NODE_BYTE) {
                if (state->pos < len && sel_class_has(&node->as.byte.cls, (unsigned char)s[state->pos]))
                    ok = sel_closure(prog, &next, marks, mark, s, len, node->as.byte.next, state->pos + 1u, caps, capture, 0u, err);
            }
        }
        SelByteStateVec tmp = active;
        active = next;
        next = tmp;
    }
    sel_byte_state_vec_destroy(&active);
    sel_byte_state_vec_destroy(&next);
    if (marks != mark_stack) free(marks);
    if (!ok) sel_byte_match_destroy(out);
    return ok;
}

SelByteProg *idm_byteprog_new(IdmError *err) {
    SelByteProg *p = calloc(1u, sizeof(*p));
    if (!p) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    return p;
}

void idm_byteprog_free(SelByteProg *p) {
    if (!p) return;
    if (p->test_closures) {
        for (size_t i = 0; i < p->pool.count; i++) free(p->test_closures[i].nodes);
        free(p->test_closures);
    }
    pool_destroy(&p->pool);
    free(p);
}

static bool sel_size_push(uint32_t **vec, size_t *count, size_t *cap, uint32_t v, IdmError *err) {
    if (*count == *cap) {
        size_t nc = *cap ? *cap * 2u : 16u;
        uint32_t *g = realloc(*vec, nc * sizeof(*g));
        if (!g) return idm_error_oom(err, idm_span_unknown(NULL));
        *vec = g;
        *cap = nc;
    }
    (*vec)[(*count)++] = v;
    return true;
}

static bool sel_build_test_closure(const SelByteProg *p, uint32_t start, SelTestClosure *cl, IdmError *err) {
    unsigned char *seen = calloc(p->pool.count ? p->pool.count : 1u, 1u);
    if (!seen) return idm_error_oom(err, idm_span_unknown(NULL));
    uint32_t *stack = NULL;
    size_t sc = 0, scap = 0;
    uint32_t *nodes = NULL;
    size_t ncount = 0, ncap = 0;
    bool ok = sel_size_push(&stack, &sc, &scap, start, err);
    while (ok && sc != 0) {
        uint32_t node = stack[--sc];
        if (node >= p->pool.count) { ok = idm_error_set(err, idm_span_unknown(NULL), "byte test closure out of bounds"); break; }
        if (seen[node]) continue;
        seen[node] = 1u;
        const SelNode *n = p->pool.nodes[node];
        switch (n->kind) {
            case SEL_NODE_FORK:
                ok = sel_size_push(&stack, &sc, &scap, n->as.fork.second, err) && sel_size_push(&stack, &sc, &scap, n->as.fork.first, err);
                break;
            case SEL_NODE_SAVE:
                ok = sel_size_push(&stack, &sc, &scap, n->as.save.next, err);
                break;
            case SEL_NODE_ACCEPT:
                cl->has_match = true;
                break;
            case SEL_NODE_BYTE:
                ok = sel_size_push(&nodes, &ncount, &ncap, node, err);
                break;
            case SEL_NODE_GUARD:
                cl->dynamic = true;
                break;
            case SEL_NODE_FAIL:
            case SEL_NODE_TRY:
            case SEL_NODE_SWITCH:
                break;
        }
    }
    free(stack);
    free(seen);
    if (!ok) { free(nodes); return false; }
    if (cl->dynamic) {
        free(nodes);
        cl->nodes = NULL;
        cl->count = 0;
        cl->has_match = false;
    } else {
        cl->nodes = nodes;
        cl->count = ncount;
    }
    return true;
}

bool idm_byteprog_build_test_closures(SelByteProg *p, IdmError *err) {
    if (p->pool.count == 0) return true;
    p->test_closures = calloc(p->pool.count, sizeof(*p->test_closures));
    if (!p->test_closures) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < p->pool.count; i++) {
        if (!sel_build_test_closure(p, (uint32_t)i, &p->test_closures[i], err)) return false;
    }
    return true;
}

typedef struct {
    uint32_t *items;
    size_t count;
    size_t cap;
    uint32_t mark;
    bool heap;
} SelTestVec;

static bool sel_test_vec_push(SelTestVec *v, uint32_t node, IdmError *err) {
    if (v->count == v->cap) {
        size_t nc = v->cap ? v->cap * 2u : 128u;
        uint32_t *g = v->heap ? realloc(v->items, nc * sizeof(*g)) : malloc(nc * sizeof(*g));
        if (!g) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!v->heap) memcpy(g, v->items, v->count * sizeof(*g));
        v->items = g;
        v->cap = nc;
        v->heap = true;
    }
    v->items[v->count++] = node;
    return true;
}

static bool sel_add_test_closure(const SelByteProg *p, SelTestVec *vec, uint32_t *marks, const char *s, size_t len, uint32_t node, size_t pos, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    if (node >= p->pool.count || pos > len) return idm_error_set(err, idm_span_unknown(NULL), "byte test state out of bounds");
    if (marks[node] == vec->mark) return true;
    marks[node] = vec->mark;
    const SelTestClosure *cl = &p->test_closures[node];
    if (!cl->dynamic) {
        if (cl->has_match && (!exact_end || pos == end_pos)) { *out_matched = true; return true; }
        for (size_t i = 0; i < cl->count; i++) {
            uint32_t item = cl->nodes[i];
            if (item != node) {
                if (marks[item] == vec->mark) continue;
                marks[item] = vec->mark;
            }
            if (!sel_test_vec_push(vec, item, err)) return false;
        }
        return true;
    }
    const SelNode *n = p->pool.nodes[node];
    switch (n->kind) {
        case SEL_NODE_FORK:
            return sel_add_test_closure(p, vec, marks, s, len, n->as.fork.first, pos, exact_end, end_pos, out_matched, err)
                && (*out_matched || sel_add_test_closure(p, vec, marks, s, len, n->as.fork.second, pos, exact_end, end_pos, out_matched, err));
        case SEL_NODE_SAVE:
            return sel_add_test_closure(p, vec, marks, s, len, n->as.save.next, pos, exact_end, end_pos, out_matched, err);
        case SEL_NODE_GUARD: {
            bool pass = false;
            switch (n->as.guard.kind) {
                case SEL_GUARD_LINE_START: pass = pos == 0 || ((n->as.guard.flags & IDM_REGEX_MULTILINE) != 0 && sel_at_line_start(s, pos)); break;
                case SEL_GUARD_LINE_END: pass = pos == len || ((n->as.guard.flags & IDM_REGEX_MULTILINE) != 0 && sel_at_line_end(s, len, pos)); break;
                case SEL_GUARD_WORD_BOUNDARY: pass = sel_at_word_boundary(s, len, pos); break;
                case SEL_GUARD_NOT_WORD_BOUNDARY: pass = !sel_at_word_boundary(s, len, pos); break;
                case SEL_GUARD_BUFFER_START: pass = pos == 0; break;
                case SEL_GUARD_BUFFER_END: pass = pos == len; break;
                case SEL_GUARD_BUFFER_END_NL: pass = pos == len || (pos + 1u == len && s[pos] == '\n'); break;
                case SEL_GUARD_LOOKAHEAD_POS:
                case SEL_GUARD_LOOKAHEAD_NEG: {
                    bool m = sel_look_matches(n->as.guard.sub, s, len, pos, err);
                    if (err && err->present) return false;
                    pass = (n->as.guard.kind == SEL_GUARD_LOOKAHEAD_POS) ? m : !m;
                    break;
                }
                case SEL_GUARD_LOOKBEHIND_POS:
                case SEL_GUARD_LOOKBEHIND_NEG: {
                    bool m = sel_lookbehind_matches(n->as.guard.sub, s, len, pos, err);
                    if (err && err->present) return false;
                    pass = (n->as.guard.kind == SEL_GUARD_LOOKBEHIND_POS) ? m : !m;
                    break;
                }
            }
            if (pass) return sel_add_test_closure(p, vec, marks, s, len, n->as.guard.next, pos, exact_end, end_pos, out_matched, err);
            return true;
        }
        case SEL_NODE_ACCEPT:
            if (!exact_end || pos == end_pos) *out_matched = true;
            return true;
        case SEL_NODE_BYTE:
            return sel_test_vec_push(vec, node, err);
        case SEL_NODE_FAIL:
        case SEL_NODE_TRY:
        case SEL_NODE_SWITCH:
            return idm_error_set(err, idm_span_unknown(NULL), "structural node in byte automaton");
    }
    return true;
}

static bool sel_byte_test(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    *out_matched = false;
    if (offset > len || (exact_end && end_pos > len) || p->pool.count == 0) return true;
    uint32_t mark_stack[256];
    uint32_t *marks = p->pool.count <= 256u ? mark_stack : calloc(p->pool.count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, p->pool.count * sizeof(*mark_stack));
    uint32_t next_mark = 1u;
    uint32_t active_stack[128], next_stack[128];
    SelTestVec active = { active_stack, 0, 128, next_mark++, false };
    SelTestVec next = { next_stack, 0, 128, 0, false };
    bool ok = sel_add_test_closure(p, &active, marks, s, len, p->start, offset, exact_end, end_pos, out_matched, err);
    for (size_t pos = offset; ok && !*out_matched && active.count != 0; pos++) {
        next.count = 0;
        next.mark = next_mark++;
        for (size_t i = 0; ok && !*out_matched && i < active.count; i++) {
            const SelNode *n = p->pool.nodes[active.items[i]];
            if (n->kind == SEL_NODE_BYTE && pos < len && sel_class_has(&n->as.byte.cls, (unsigned char)s[pos]))
                ok = sel_add_test_closure(p, &next, marks, s, len, n->as.byte.next, pos + 1u, exact_end, end_pos, out_matched, err);
        }
        SelTestVec tmp = active;
        active = next;
        next = tmp;
    }
    if (active.heap) free(active.items);
    if (next.heap) free(next.items);
    if (marks != mark_stack) free(marks);
    return ok;
}

bool idm_byteprog_test(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    if (!p->test_closures) {
        SelByteMatch m = {0};
        bool ok = sel_byte_run(p, s, len, offset, exact_end, end_pos, false, &m, err);
        *out_matched = m.matched;
        sel_byte_match_destroy(&m);
        return ok;
    }
    return sel_byte_test(p, s, len, offset, exact_end, end_pos, out_matched, err);
}

static uint32_t byteprog_node(SelByteProg *p, SelNodeKind kind, IdmError *err) {
    SelNode *n = pool_new(&p->pool, kind, err);
    return n ? n->index : SEL_NO_NODE;
}

uint32_t idm_byteprog_fork(SelByteProg *p, IdmError *err) { return byteprog_node(p, SEL_NODE_FORK, err); }

uint32_t idm_byteprog_byte(SelByteProg *p, const unsigned char bits[32], bool negated, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_BYTE, err);
    if (i != SEL_NO_NODE) {
        memcpy(p->pool.nodes[i]->as.byte.cls.bits, bits, 32u);
        p->pool.nodes[i]->as.byte.cls.negated = negated;
    }
    return i;
}

uint32_t idm_byteprog_save(SelByteProg *p, uint32_t slot, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_SAVE, err);
    if (i != SEL_NO_NODE) p->pool.nodes[i]->as.save.slot = slot;
    return i;
}

uint32_t idm_byteprog_guard(SelByteProg *p, IdmByteGuardKind kind, uint32_t flags, SelByteProg *sub, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_GUARD, err);
    if (i != SEL_NO_NODE) {
        p->pool.nodes[i]->as.guard.kind = (SelGuardKind)kind;
        p->pool.nodes[i]->as.guard.flags = flags;
        p->pool.nodes[i]->as.guard.sub = sub;
    } else {
        idm_byteprog_free(sub);
    }
    return i;
}

uint32_t idm_byteprog_accept(SelByteProg *p, uint32_t accept_id, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_ACCEPT, err);
    if (i != SEL_NO_NODE) p->pool.nodes[i]->as.accept_id = accept_id;
    return i;
}

void idm_byteprog_set_byte_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node]->as.byte.next = target; }
void idm_byteprog_set_save_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node]->as.save.next = target; }
void idm_byteprog_set_guard_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node]->as.guard.next = target; }
void idm_byteprog_set_fork(SelByteProg *p, uint32_t node, uint32_t first, uint32_t second) {
    p->pool.nodes[node]->as.fork.first = first;
    p->pool.nodes[node]->as.fork.second = second;
}
void idm_byteprog_set_start(SelByteProg *p, uint32_t start) { p->start = start; }
void idm_byteprog_set_capture_count(SelByteProg *p, size_t n) { p->capture_count = n; }
void idm_byteprog_set_flags(SelByteProg *p, uint32_t flags) { p->flags = flags; }
size_t idm_byteprog_node_count(const SelByteProg *p) { return p->pool.count; }

size_t idm_byteprog_footprint(const SelByteProg *p) {
    if (!p) return 0;
    size_t total = sizeof(*p) + p->pool.cap * sizeof(SelNode *) + p->pool.count * sizeof(SelNode);
    for (size_t i = 0; i < p->pool.count; i++) {
        const SelNode *n = p->pool.nodes[i];
        if (n->kind == SEL_NODE_GUARD && n->as.guard.sub) total += idm_byteprog_footprint(n->as.guard.sub);
    }
    if (p->test_closures) {
        for (size_t i = 0; i < p->pool.count; i++) total += p->test_closures[i].count * sizeof(uint32_t);
        total += p->pool.count * sizeof(SelTestClosure);
    }
    return total;
}


void idm_byteprog_finalize_linear(SelByteProg *p) {
    for (size_t i = 0; i < p->pool.count; i++) {
        SelNode *n = p->pool.nodes[i];
        uint32_t nxt = (uint32_t)(i + 1u);
        if (n->kind == SEL_NODE_BYTE) n->as.byte.next = nxt;
        else if (n->kind == SEL_NODE_SAVE) n->as.save.next = nxt;
        else if (n->kind == SEL_NODE_GUARD) n->as.guard.next = nxt;
    }
}

bool idm_byteprog_exec(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, bool *out_matched, size_t *out_end, size_t *out_accept, IdmByteCapture *out_caps, size_t out_cap_count, IdmError *err) {
    SelByteMatch m = {0};
    if (!sel_byte_run(p, s, len, offset, exact_end, end_pos, capture, &m, err)) return false;
    *out_matched = m.matched;
    if (m.matched) {
        if (out_end) *out_end = m.end;
        if (out_accept) *out_accept = m.accept_id;
        for (size_t i = 0; i < out_cap_count; i++) {
            bool have = m.captures && i < p->capture_count && m.captures[i].set;
            out_caps[i].set = have;
            out_caps[i].start = have ? m.captures[i].start : 0u;
            out_caps[i].end = have ? m.captures[i].end : 0u;
        }
    }
    sel_byte_match_destroy(&m);
    return true;
}
