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
        struct { char *name; uint32_t slot; IdmSymbol *symbol; } name;
        IdmValue literal;
        struct { SelPat *left; SelPat *right; } pair;
        struct { SelPat **items; size_t count; } seq;
        struct { SelPat **items; size_t count; SelPat *rest; } seq_rest;
        struct { SelDictEntry *entries; size_t count; SelPat *rest; } dict;
        struct {
            IdmSyntaxPatternKind kind;
            IdmSyntaxKind seq_kind;
            bool keyed_dict;
            bool expr_group;
            char *name;
            uint32_t slot;
            IdmSyntax *literal;
            SelPat **items;
            IdmSyntax **keys;
            size_t count;
            size_t rest_index;
            SelPat *dict_rest;
        } syn;
    } as;
};

typedef enum {
    SEL_ACCESS_ARG,
    SEL_ACCESS_CAR,
    SEL_ACCESS_CDR,
    SEL_ACCESS_SEQ_ITEM,
    SEL_ACCESS_SEQ_REST,
    SEL_ACCESS_DICT_VALUE,
    SEL_ACCESS_DICT_REST,
    SEL_ACCESS_SYN_ITEM,
    SEL_ACCESS_SYN_ITEM_REV,
    SEL_ACCESS_SYN_REST,
    SEL_ACCESS_SYN_DICT_VALUE,
    SEL_ACCESS_SYN_DICT_REST,
    SEL_ACCESS_SYN_UNWRAP_EXPR
} SelAccessKind;

typedef struct {
    SelAccessKind kind;
    uint32_t parent;
    uint32_t index;
    uint32_t index2;
    IdmValueTag seq_tag;
    union {
        IdmValue key;
        const IdmSyntax *syn_key;
        const SelPat *dict_pat;
    } u;
} SelAccess;

typedef enum {
    SEL_CTOR_LITERAL,
    SEL_CTOR_PAIR,
    SEL_CTOR_VECTOR_EXACT,
    SEL_CTOR_VECTOR_REST,
    SEL_CTOR_TUPLE_EXACT,
    SEL_CTOR_TUPLE_REST,
    SEL_CTOR_DICT,
    SEL_CTOR_TYPE,
    SEL_CTOR_SYN_KIND,
    SEL_CTOR_SYN_LITERAL
} SelCtorKind;

typedef struct {
    SelCtorKind kind;
    IdmValue literal;
    IdmSymbol *type;
    uint32_t count;
    IdmSyntaxKind syn_kind;
    bool syn_rest;
    const IdmSyntax *syn_literal;
} SelCtor;

typedef enum {
    SEL_ACTION_BIND,
    SEL_ACTION_PIN,
    SEL_ACTION_CTOR_MATCH,
    SEL_ACTION_VALUE_EQUAL,
    SEL_ACTION_DICT_HAS,
    SEL_ACTION_SYN_BIND,
    SEL_ACTION_SYN_DICT_HAS
} SelActionKind;

typedef struct {
    SelActionKind kind;
    uint32_t access;
    char *name;
    uint32_t slot;
    IdmValue key;
    IdmValue value;
    SelCtor ctor;
    const IdmSyntax *syn_key;
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
    size_t cell_cap;
    SelAction *actions;
    size_t action_count;
    size_t action_cap;
} SelRow;

typedef struct SelNode SelNode;

typedef struct {
    SelCtor ctor;
    uint32_t node;
} SelCase;

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
            uint32_t next;
        } try_row;
        struct {
            uint32_t access;
            SelCase *cases;
            size_t case_count;
            uint32_t default_node;
            bool syn;
        } sw;
        struct {
            uint32_t first;
            uint32_t second;
        } fork;
        struct {
            IdmByteClass cls;
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
    uint32_t node;
    bool unconditional;
    uint32_t function_index;
} SelArityCase;

typedef struct {
    SelNode *nodes;
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
    size_t pattern_cap;
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

static bool pattern_bindings_reserve(IdmPatternBindings *bindings, size_t needed) {
    if (needed <= bindings->cap) return true;
    if (bindings->heap) {
        IdmGrowItem items[] = {
            { .base = (void **)&bindings->names, .elem_size = sizeof(*bindings->names) },
            { .base = (void **)&bindings->values, .elem_size = sizeof(*bindings->values) },
            { .base = (void **)&bindings->slots, .elem_size = sizeof(*bindings->slots) },
        };
        return idm_growv(items, 3u, &bindings->cap, IDM_PATTERN_INLINE_BINDINGS, needed);
    }
    size_t cap = 0;
    if (!idm_next_capacity(bindings->cap, IDM_PATTERN_INLINE_BINDINGS, needed, &cap) ||
        cap > SIZE_MAX / sizeof(*bindings->names) ||
        cap > SIZE_MAX / sizeof(*bindings->values) ||
        cap > SIZE_MAX / sizeof(*bindings->slots)) return false;
    char **names = malloc(cap * sizeof(*names));
    IdmValue *values = malloc(cap * sizeof(*values));
    uint32_t *slots = malloc(cap * sizeof(*slots));
    if (!names || !values || !slots) {
        free(names);
        free(values);
        free(slots);
        return false;
    }
    if (bindings->count != 0) {
        memcpy(names, bindings->names, bindings->count * sizeof(*names));
        memcpy(values, bindings->values, bindings->count * sizeof(*values));
        memcpy(slots, bindings->slots, bindings->count * sizeof(*slots));
    }
    bindings->names = names;
    bindings->values = values;
    bindings->slots = slots;
    bindings->cap = cap;
    bindings->heap = true;
    return true;
}

bool idm_pattern_bindings_add_slot(IdmPatternBindings *bindings, const char *name, uint32_t slot, IdmValue value) {
    if (slot != UINT32_MAX) {
        const IdmValue *existing = idm_pattern_bindings_get_slot(bindings, slot);
        if (existing) return idm_value_equal(*existing, value);
    }
    const IdmValue *existing = idm_pattern_bindings_get(bindings, name);
    if (existing) return idm_value_equal(*existing, value);
    if (!pattern_bindings_reserve(bindings, bindings->count + 1u)) return false;
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
    IdmDictIter it;
    IdmValue key = idm_nil();
    IdmValue val = idm_nil();
    idm_dict_iter_init(value, &it);
    while (idm_dict_iter_next(&it, &key, &val)) {
        if (sel_dict_key_in_entries(key, entries, count)) continue;
        rest[rest_count].key = key;
        rest[rest_count].value = val;
        rest_count++;
    }
    *out = idm_dict(rt, rest, rest_count, err);
    free(rest);
    return !(err && err->present);
}

static bool syntax_pattern_literal_word(const IdmSyntaxPattern *pat, const char *word) {
    return pat && pat->kind == IDM_SYN_PAT_LITERAL && pat->as.literal &&
           pat->as.literal->kind == IDM_SYN_WORD && strcmp(pat->as.literal->as.text, word) == 0;
}

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
        if (idm_syn_equal(dict->as.seq.items[i - 2u], key)) return dict->as.seq.items[i - 1u];
    }
    return NULL;
}

static const IdmSyntax *syn_subject_unwrap_expr(const IdmSyntax *syn) {
    if (!syn || syn->kind != IDM_SYN_LIST || syn->as.seq.count != 2u ||
        syn->as.seq.items[0]->kind != IDM_SYN_WORD ||
        (strcmp(syn->as.seq.items[0]->as.text, "%-group") != 0 && strcmp(syn->as.seq.items[0]->as.text, "%-layout-group") != 0)) {
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
        return idm_syn_equal(idm_syntax_value_get(a), idm_syntax_value_get(b));
    }
    if (idm_is_empty_list(a) && idm_is_empty_list(b)) return true;
    if (idm_is_pair(a) && idm_is_pair(b)) {
        return syntax_bound_value_equal(idm_car(a, NULL), idm_car(b, NULL)) &&
               syntax_bound_value_equal(idm_cdr(a, NULL), idm_cdr(b, NULL));
    }
    return idm_value_equal(a, b);
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
            free(pat->as.syn.name);
            idm_syn_free(pat->as.syn.literal);
            for (size_t i = 0; i < pat->as.syn.count; i++) {
                sel_pat_free(pat->as.syn.items[i]);
                if (pat->as.syn.keys) idm_syn_free(pat->as.syn.keys[i]);
            }
            free(pat->as.syn.items);
            free(pat->as.syn.keys);
            sel_pat_free(pat->as.syn.dict_rest);
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

static SelPat *sel_pat_from_idm(IdmRuntime *rt, const IdmPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err);

static SelPat *syn_sel_from_pat(const IdmSyntaxPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err);

static bool syn_sel_seq_lower(SelPat *out, const IdmSyntaxPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err) {
    size_t pair_item_count = 0;
    const IdmSyntaxPattern *rest_pat = NULL;
    if (pat->as.seq.kind == IDM_SYN_DICT && syntax_pattern_keyed_dict_shape(pat, &pair_item_count, &rest_pat)) {
        out->as.syn.keyed_dict = true;
        size_t pairs = pair_item_count / 2u;
        if (pairs != 0) {
            out->as.syn.items = calloc(pairs, sizeof(*out->as.syn.items));
            out->as.syn.keys = calloc(pairs, sizeof(*out->as.syn.keys));
            if (!out->as.syn.items || !out->as.syn.keys) return idm_error_oom(err, pat->span);
        }
        out->as.syn.count = pairs;
        for (size_t i = 0; i < pairs; i++) {
            const IdmSyntax *key = NULL;
            syntax_pattern_literal_key(pat->as.seq.items[2u * i], &key);
            out->as.syn.keys[i] = idm_syn_clone(key);
            if (!out->as.syn.keys[i]) return idm_error_oom(err, pat->span);
            out->as.syn.items[i] = syn_sel_from_pat(pat->as.seq.items[2u * i + 1u], locals, local_count, err);
            if (!out->as.syn.items[i]) return false;
        }
        if (rest_pat) {
            out->as.syn.dict_rest = syn_sel_from_pat(rest_pat, locals, local_count, err);
            if (!out->as.syn.dict_rest) return false;
        }
        return true;
    }
    if (pat->as.seq.kind == IDM_SYN_LIST && pat->as.seq.count != 0 && syntax_pattern_literal_word(pat->as.seq.items[0], "%-expr")) {
        out->as.syn.expr_group = true;
    }
    out->as.syn.rest_index = pat->as.seq.rest_index;
    if (pat->as.seq.rest_index != IDM_SYN_PAT_NO_REST && pat->as.seq.rest_name) {
        out->as.syn.name = idm_strdup(pat->as.seq.rest_name);
        if (!out->as.syn.name) return idm_error_oom(err, pat->span);
        out->as.syn.slot = selector_local_slot(locals, local_count, pat->as.seq.rest_name);
    }
    if (pat->as.seq.count != 0) {
        out->as.syn.items = calloc(pat->as.seq.count, sizeof(*out->as.syn.items));
        if (!out->as.syn.items) return idm_error_oom(err, pat->span);
    }
    out->as.syn.count = pat->as.seq.count;
    for (size_t i = 0; i < pat->as.seq.count; i++) {
        out->as.syn.items[i] = syn_sel_from_pat(pat->as.seq.items[i], locals, local_count, err);
        if (!out->as.syn.items[i]) return false;
    }
    return true;
}

static SelPat *syn_sel_from_pat(const IdmSyntaxPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err) {
    if (!pat) { idm_error_set(err, idm_span_unknown(NULL), "cannot lower null syntax pattern"); return NULL; }
    SelPat *out = sel_pat_alloc(SEL_PAT_SYNTAX, pat->span);
    if (!out) { idm_error_oom(err, pat->span); return NULL; }
    out->as.syn.kind = pat->kind;
    out->as.syn.rest_index = IDM_SYN_PAT_NO_REST;
    out->as.syn.slot = UINT32_MAX;
    switch (pat->kind) {
        case IDM_SYN_PAT_WILDCARD:
            break;
        case IDM_SYN_PAT_BIND:
            out->as.syn.name = idm_strdup(pat->as.bind.name);
            out->as.syn.slot = selector_local_slot(locals, local_count, pat->as.bind.name);
            if (!out->as.syn.name) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            break;
        case IDM_SYN_PAT_LITERAL:
            out->as.syn.literal = idm_syn_clone(pat->as.literal);
            if (!out->as.syn.literal) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            break;
        case IDM_SYN_PAT_SEQUENCE:
            out->as.syn.seq_kind = pat->as.seq.kind;
            if (!syn_sel_seq_lower(out, pat, locals, local_count, err)) { sel_pat_free(out); return NULL; }
            break;
    }
    return out;
}

static SelPat *sel_pat_list_from_idm_items(IdmRuntime *rt, IdmPattern *const *items, size_t count, const IdmPatternLocal *locals, uint32_t local_count, IdmSpan span, IdmError *err) {
    if (count == 0) return sel_pat_literal(idm_empty_list(), span);
    SelPat *head = sel_pat_from_idm(rt, items[0], locals, local_count, err);
    SelPat *tail = sel_pat_list_from_idm_items(rt, items + 1u, count - 1u, locals, local_count, span, err);
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

static SelPat *sel_pat_from_idm(IdmRuntime *rt, const IdmPattern *pat, const IdmPatternLocal *locals, uint32_t local_count, IdmError *err) {
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
            out->as.name.symbol = idm_intern(&rt->intern, IDM_SYMBOL_ATOM, pat->as.name);
            if (!out->as.name.name) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            if (!out->as.name.symbol) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            return out;
        case IDM_PAT_LITERAL:
            out = sel_pat_literal(pat->as.literal, pat->span);
            if (!out) idm_error_oom(err, pat->span);
            return out;
        case IDM_PAT_PAIR:
            out = sel_pat_alloc(SEL_PAT_PAIR, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.pair.left = sel_pat_from_idm(rt, pat->as.pair.left, locals, local_count, err);
            out->as.pair.right = sel_pat_from_idm(rt, pat->as.pair.right, locals, local_count, err);
            if (!out->as.pair.left || !out->as.pair.right) { sel_pat_free(out); return NULL; }
            return out;
        case IDM_PAT_LIST:
            return sel_pat_list_from_idm_items(rt, pat->as.seq.items, pat->as.seq.count, locals, local_count, pat->span, err);
        case IDM_PAT_VECTOR:
        case IDM_PAT_TUPLE: {
            out = sel_pat_alloc(pat->kind == IDM_PAT_VECTOR ? SEL_PAT_VECTOR : SEL_PAT_TUPLE, pat->span);
            if (!out) { idm_error_oom(err, pat->span); return NULL; }
            out->as.seq.count = pat->as.seq.count;
            out->as.seq.items = pat->as.seq.count == 0 ? NULL : calloc(pat->as.seq.count, sizeof(*out->as.seq.items));
            if (pat->as.seq.count != 0 && !out->as.seq.items) { sel_pat_free(out); idm_error_oom(err, pat->span); return NULL; }
            for (size_t i = 0; i < pat->as.seq.count; i++) {
                out->as.seq.items[i] = sel_pat_from_idm(rt, pat->as.seq.items[i], locals, local_count, err);
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
                out->as.seq_rest.items[i] = sel_pat_from_idm(rt, pat->as.seq_rest.items[i], locals, local_count, err);
                if (!out->as.seq_rest.items[i]) { sel_pat_free(out); return NULL; }
            }
            out->as.seq_rest.rest = sel_pat_from_idm(rt, pat->as.seq_rest.rest, locals, local_count, err);
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
                out->as.dict.entries[i].pattern = sel_pat_from_idm(rt, pat->as.dict.entries[i].pattern, locals, local_count, err);
                if (!out->as.dict.entries[i].pattern) { sel_pat_free(out); return NULL; }
            }
            if (pat->as.dict.rest) {
                out->as.dict.rest = sel_pat_from_idm(rt, pat->as.dict.rest, locals, local_count, err);
                if (!out->as.dict.rest) { sel_pat_free(out); return NULL; }
            }
            return out;
        }
        case IDM_PAT_SYNTAX:
            return syn_sel_from_pat(pat->as.syntax, locals, local_count, err);
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
    dst->cell_cap = src->cell_count;
    dst->action_count = src->action_count;
    dst->action_cap = src->action_count;
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

static void sel_row_remove_cell(SelRow *row, size_t index) {
    if (index + 1u < row->cell_count) memmove(row->cells + index, row->cells + index + 1u, (row->cell_count - index - 1u) * sizeof(*row->cells));
    row->cell_count--;
}

static bool sel_row_add_cell(SelRow *row, uint32_t access, SelPat *pattern, IdmError *err, IdmSpan span) {
    if (row->cell_count == row->cell_cap) {
        if (!idm_grow((void **)&row->cells, &row->cell_cap, sizeof(*row->cells), 4u, row->cell_count + 1u)) return idm_error_oom(err, span);
    }
    row->cells[row->cell_count].access = access;
    row->cells[row->cell_count].pattern = pattern;
    row->cell_count++;
    return true;
}

static bool sel_row_add_action(SelRow *row, SelAction action, IdmError *err, IdmSpan span) {
    if (row->action_count == row->action_cap &&
        !idm_grow((void **)&row->actions, &row->action_cap, sizeof(*row->actions), 4u, row->action_count + 1u)) {
        sel_action_destroy(&action);
        return idm_error_oom(err, span);
    }
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

static bool sel_row_add_value_equal(SelRow *row, uint32_t access, IdmValue value, IdmError *err, IdmSpan span) {
    SelAction action;
    memset(&action, 0, sizeof(action));
    action.kind = SEL_ACTION_VALUE_EQUAL;
    action.access = access;
    action.value = value;
    return sel_row_add_action(row, action, err, span);
}

static bool sel_row_add_ctor_match(SelRow *row, uint32_t access, SelCtor ctor, IdmError *err, IdmSpan span) {
    SelAction action;
    memset(&action, 0, sizeof(action));
    action.kind = SEL_ACTION_CTOR_MATCH;
    action.access = access;
    action.ctor = ctor;
    return sel_row_add_action(row, action, err, span);
}

static bool sel_row_add_syn_dict_has(SelRow *row, uint32_t access, const IdmSyntax *syn_key, IdmError *err, IdmSpan span) {
    SelAction action;
    memset(&action, 0, sizeof(action));
    action.kind = SEL_ACTION_SYN_DICT_HAS;
    action.access = access;
    action.syn_key = syn_key;
    return sel_row_add_action(row, action, err, span);
}

static bool sel_dict_rest_keys_equal(const SelPat *a, const SelPat *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != SEL_PAT_DICT || b->kind != SEL_PAT_DICT || a->as.dict.count != b->as.dict.count) return false;
    for (size_t i = 0; i < a->as.dict.count; i++) {
        bool found = false;
        for (size_t j = 0; j < b->as.dict.count; j++) {
            if (idm_value_equal(a->as.dict.entries[i].key, b->as.dict.entries[j].key)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool sel_syn_dict_rest_keys_equal(const SelPat *a, const SelPat *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != SEL_PAT_SYNTAX || b->kind != SEL_PAT_SYNTAX ||
        !a->as.syn.keyed_dict || !b->as.syn.keyed_dict || a->as.syn.count != b->as.syn.count) return false;
    for (size_t i = 0; i < a->as.syn.count; i++) {
        bool found = false;
        for (size_t j = 0; j < b->as.syn.count; j++) {
            if (idm_syn_equal(a->as.syn.keys[i], b->as.syn.keys[j])) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool selector_access_equal(const SelAccess *a, const SelAccess *b) {
    if (a->kind != b->kind || a->parent != b->parent || a->index != b->index || a->index2 != b->index2 || a->seq_tag != b->seq_tag) return false;
    switch (a->kind) {
        case SEL_ACCESS_DICT_VALUE:
            return idm_value_equal(a->u.key, b->u.key);
        case SEL_ACCESS_SYN_DICT_VALUE:
            return idm_syn_equal(a->u.syn_key, b->u.syn_key);
        case SEL_ACCESS_DICT_REST:
            return sel_dict_rest_keys_equal(a->u.dict_pat, b->u.dict_pat);
        case SEL_ACCESS_SYN_DICT_REST:
            return sel_syn_dict_rest_keys_equal(a->u.dict_pat, b->u.dict_pat);
        case SEL_ACCESS_ARG:
        case SEL_ACCESS_CAR:
        case SEL_ACCESS_CDR:
        case SEL_ACCESS_SEQ_ITEM:
        case SEL_ACCESS_SEQ_REST:
        case SEL_ACCESS_SYN_ITEM:
        case SEL_ACCESS_SYN_ITEM_REV:
        case SEL_ACCESS_SYN_REST:
        case SEL_ACCESS_SYN_UNWRAP_EXPR:
            return true;
    }
    return false;
}

static bool selector_add_access(IdmPatternSelector *selector, SelAccess access, uint32_t *out, IdmError *err, IdmSpan span) {
    for (size_t i = 0; i < selector->access_count; i++) {
        if (!selector_access_equal(&selector->accesses[i], &access)) continue;
        *out = (uint32_t)i;
        return true;
    }
    if (selector->access_count >= UINT32_MAX) return idm_error_set(err, span, "pattern selector has too many access paths");
    if (selector->access_count == selector->access_cap) {
        if (!idm_grow((void **)&selector->accesses, &selector->access_cap, sizeof(*selector->accesses), 16u, selector->access_count + 1u)) return idm_error_oom(err, span);
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
    access.u.key = key;
    return selector_add_access(selector, access, out, err, span);
}

static bool selector_syn_access(IdmPatternSelector *selector, SelAccessKind kind, uint32_t parent, uint32_t index, uint32_t index2, const IdmSyntax *syn_key, const SelPat *dict_pat, uint32_t *out, IdmError *err, IdmSpan span) {
    SelAccess access;
    memset(&access, 0, sizeof(access));
    access.kind = kind;
    access.parent = parent;
    access.index = index;
    access.index2 = index2;
    if (syn_key) access.u.syn_key = syn_key;
    else if (dict_pat) access.u.dict_pat = dict_pat;
    return selector_add_access(selector, access, out, err, span);
}

static bool syn_effective_access(IdmPatternSelector *selector, uint32_t base, const SelPat *child, uint32_t *out, IdmError *err, IdmSpan span) {
    if (child && child->kind == SEL_PAT_SYNTAX && child->as.syn.kind == IDM_SYN_PAT_SEQUENCE && child->as.syn.expr_group) {
        return selector_syn_access(selector, SEL_ACCESS_SYN_UNWRAP_EXPR, base, 0, 0, NULL, NULL, out, err, span);
    }
    *out = base;
    return true;
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
            out->kind = SEL_CTOR_DICT;
            out->count = 0;
            return true;
        case SEL_PAT_TYPE:
            out->kind = SEL_CTOR_TYPE;
            out->type = pat->as.name.symbol;
            out->count = 0;
            return out->type != NULL;
        case SEL_PAT_SYNTAX:
            switch (pat->as.syn.kind) {
                case IDM_SYN_PAT_LITERAL:
                    out->kind = SEL_CTOR_SYN_LITERAL;
                    out->syn_literal = pat->as.syn.literal;
                    out->count = 0;
                    return true;
                case IDM_SYN_PAT_SEQUENCE:
                    out->kind = SEL_CTOR_SYN_KIND;
                    out->syn_kind = pat->as.syn.keyed_dict ? IDM_SYN_DICT : pat->as.syn.seq_kind;
                    out->syn_rest = pat->as.syn.keyed_dict || pat->as.syn.rest_index != IDM_SYN_PAT_NO_REST;
                    out->count = (uint32_t)(pat->as.syn.keyed_dict ? 0u : pat->as.syn.count);
                    return pat->as.syn.count <= UINT32_MAX;
                case IDM_SYN_PAT_WILDCARD:
                case IDM_SYN_PAT_BIND:
                    return false;
            }
            return false;
        case SEL_PAT_WILDCARD:
        case SEL_PAT_BIND:
        case SEL_PAT_PIN:
            return false;
    }
    return false;
}

static bool sel_ctor_equal(SelCtor a, SelCtor b) {
    if (a.kind != b.kind || a.count != b.count) return false;
    if (a.kind == SEL_CTOR_TYPE) return a.type == b.type;
    if (a.kind == SEL_CTOR_SYN_KIND) return a.syn_kind == b.syn_kind && a.syn_rest == b.syn_rest;
    if (a.kind == SEL_CTOR_SYN_LITERAL) return idm_syn_equal(a.syn_literal, b.syn_literal);
    return a.kind != SEL_CTOR_LITERAL || idm_value_equal(a.literal, b.literal);
}

static bool sel_builtin_types_overlap(IdmBuiltinType a, IdmBuiltinType b) {
    if (a == b) return true;
    if (a == IDM_BUILTIN_TYPE_NONE || b == IDM_BUILTIN_TYPE_NONE) {
        return (a == IDM_BUILTIN_TYPE_RECORD && b == IDM_BUILTIN_TYPE_NONE) ||
               (b == IDM_BUILTIN_TYPE_RECORD && a == IDM_BUILTIN_TYPE_NONE);
    }
    if (a == IDM_BUILTIN_TYPE_INT) return b == IDM_BUILTIN_TYPE_FIXNUM || b == IDM_BUILTIN_TYPE_BIGNUM;
    if (b == IDM_BUILTIN_TYPE_INT) return a == IDM_BUILTIN_TYPE_FIXNUM || a == IDM_BUILTIN_TYPE_BIGNUM;
    if (a == IDM_BUILTIN_TYPE_LIST) return b == IDM_BUILTIN_TYPE_PAIR || b == IDM_BUILTIN_TYPE_EMPTY_LIST;
    if (b == IDM_BUILTIN_TYPE_LIST) return a == IDM_BUILTIN_TYPE_PAIR || a == IDM_BUILTIN_TYPE_EMPTY_LIST;
    return false;
}

static bool sel_type_symbols_overlap(IdmSymbol *a, IdmSymbol *b) {
    if (a == b) return true;
    return sel_builtin_types_overlap(idm_value_builtin_type_kind(a), idm_value_builtin_type_kind(b));
}

static bool sel_type_overlaps_ctor(IdmSymbol *type, SelCtor ctor) {
    IdmBuiltinType kind = idm_value_builtin_type_kind(type);
    switch (ctor.kind) {
        case SEL_CTOR_LITERAL:
            return idm_value_matches_type_symbol(ctor.literal, type);
        case SEL_CTOR_PAIR:
            return kind == IDM_BUILTIN_TYPE_PAIR || kind == IDM_BUILTIN_TYPE_LIST;
        case SEL_CTOR_VECTOR_EXACT:
        case SEL_CTOR_VECTOR_REST:
            return kind == IDM_BUILTIN_TYPE_VECTOR;
        case SEL_CTOR_TUPLE_EXACT:
        case SEL_CTOR_TUPLE_REST:
            return kind == IDM_BUILTIN_TYPE_TUPLE;
        case SEL_CTOR_DICT:
            return kind == IDM_BUILTIN_TYPE_DICT;
        case SEL_CTOR_TYPE:
            return sel_type_symbols_overlap(type, ctor.type);
        case SEL_CTOR_SYN_KIND:
        case SEL_CTOR_SYN_LITERAL:
            return kind == IDM_BUILTIN_TYPE_SYNTAX;
    }
    return false;
}

static bool sel_syn_literal_overlaps_kind(const IdmSyntax *syn, SelCtor ctor) {
    if (!syn || ctor.kind != SEL_CTOR_SYN_KIND || syn->kind != ctor.syn_kind) return false;
    if (syn->kind == IDM_SYN_DICT && (syn->as.seq.count % 2u) != 0) return false;
    if (syn->kind < IDM_SYN_LIST) return true;
    return ctor.syn_rest ? syn->as.seq.count >= ctor.count : syn->as.seq.count == ctor.count;
}

static bool sel_ctor_native_syntax(SelCtor ctor) {
    return ctor.kind == SEL_CTOR_SYN_KIND || ctor.kind == SEL_CTOR_SYN_LITERAL;
}

static bool sel_ctor_overlaps(SelCtor a, SelCtor b) {
    if (sel_ctor_equal(a, b)) return true;
    if (a.kind == SEL_CTOR_TYPE) return sel_type_overlaps_ctor(a.type, b);
    if (b.kind == SEL_CTOR_TYPE) return sel_type_overlaps_ctor(b.type, a);
    if (a.kind == SEL_CTOR_SYN_LITERAL && b.kind == SEL_CTOR_SYN_KIND) return sel_syn_literal_overlaps_kind(a.syn_literal, b);
    if (b.kind == SEL_CTOR_SYN_LITERAL && a.kind == SEL_CTOR_SYN_KIND) return sel_syn_literal_overlaps_kind(b.syn_literal, a);
    if (a.kind == SEL_CTOR_VECTOR_EXACT && b.kind == SEL_CTOR_VECTOR_REST) return a.count >= b.count;
    if (a.kind == SEL_CTOR_VECTOR_REST && b.kind == SEL_CTOR_VECTOR_EXACT) return b.count >= a.count;
    if (a.kind == SEL_CTOR_VECTOR_REST && b.kind == SEL_CTOR_VECTOR_REST) return true;
    if (a.kind == SEL_CTOR_TUPLE_EXACT && b.kind == SEL_CTOR_TUPLE_REST) return a.count >= b.count;
    if (a.kind == SEL_CTOR_TUPLE_REST && b.kind == SEL_CTOR_TUPLE_EXACT) return b.count >= a.count;
    if (a.kind == SEL_CTOR_TUPLE_REST && b.kind == SEL_CTOR_TUPLE_REST) return true;
    if (a.kind == SEL_CTOR_SYN_KIND && b.kind == SEL_CTOR_SYN_KIND && a.syn_kind == b.syn_kind) {
        if (a.syn_rest && !b.syn_rest) return b.count >= a.count;
        if (!a.syn_rest && b.syn_rest) return a.count >= b.count;
        if (a.syn_rest && b.syn_rest) return true;
    }
    return false;
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
            return idm_value_matches_type_symbol(value, ctor.type);
        case SEL_CTOR_SYN_KIND: {
            if (idm_value_tag(value) != IDM_VAL_SYNTAX) return false;
            const IdmSyntax *syn = idm_syntax_value_get(value);
            if (!syn || syn->kind != ctor.syn_kind) return false;
            if (syn->kind == IDM_SYN_DICT && (syn->as.seq.count % 2u) != 0) return false;
            if (syn->kind < IDM_SYN_LIST) return true;
            return ctor.syn_rest ? syn->as.seq.count >= ctor.count : syn->as.seq.count == ctor.count;
        }
        case SEL_CTOR_SYN_LITERAL:
            return idm_value_tag(value) == IDM_VAL_SYNTAX && idm_syn_equal(ctor.syn_literal, idm_syntax_value_get(value));
    }
    return false;
}

static bool sel_pat_defaultable(const SelPat *pat) {
    if (pat->kind == SEL_PAT_SYNTAX) {
        return pat->as.syn.kind == IDM_SYN_PAT_WILDCARD || pat->as.syn.kind == IDM_SYN_PAT_BIND;
    }
    return pat->kind == SEL_PAT_WILDCARD || pat->kind == SEL_PAT_BIND || pat->kind == SEL_PAT_PIN;
}

static bool sel_pat_complex_literal_compatible(const SelPat *pat, SelCtor ctor) {
    return pat->kind == SEL_PAT_LITERAL && !literal_can_be_disjoint_ctor(pat->as.literal) && sel_ctor_matches_value(ctor, pat->as.literal);
}

static bool sel_pat_compatible_with_any_case(const SelPat *pat, const SelCtor *ctors, size_t count) {
    SelCtor own;
    if (sel_pat_ctor(pat, &own)) {
        for (size_t i = 0; i < count; i++) if (sel_ctor_overlaps(own, ctors[i])) return true;
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

static uint32_t pool_new(SelNodePool *pool, SelNodeKind kind, IdmError *err) {
    if (pool->count == pool->cap) {
        if (!idm_grow((void **)&pool->nodes, &pool->cap, sizeof(*pool->nodes), 16u, pool->count + 1u)) { idm_error_oom(err, idm_span_unknown(NULL)); return SEL_NO_NODE; }
    }
    uint32_t idx = (uint32_t)pool->count;
    SelNode *node = &pool->nodes[idx];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->index = idx;
    pool->count++;
    return idx;
}

static void pool_destroy(SelNodePool *pool) {
    for (size_t i = 0; i < pool->count; i++) sel_node_destroy_contents(&pool->nodes[i]);
    free(pool->nodes);
    pool->nodes = NULL;
    pool->count = 0;
    pool->cap = 0;
}

static uint32_t sel_node_new(IdmPatternSelector *selector, SelNodeKind kind, IdmError *err) {
    return pool_new(&selector->pool, kind, err);
}

static uint32_t sel_node_fail(IdmPatternSelector *selector, IdmError *err) {
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
    if (pat->as.dict.rest) {
        uint32_t rest_access = 0;
        if (!selector_syn_access(selector, SEL_ACCESS_DICT_REST, access, 0, 0, NULL, pat, &rest_access, err, pat->span) ||
            !sel_row_add_cell(row, rest_access, pat->as.dict.rest, err, pat->span)) return false;
    }
    return true;
}

static bool row_add_syntax_subcells(IdmPatternSelector *selector, SelRow *row, uint32_t access, const SelPat *pat, IdmError *err) {
    IdmSpan span = pat->span;
    if (pat->as.syn.kind != IDM_SYN_PAT_SEQUENCE) return true;
    if (pat->as.syn.keyed_dict) {
        for (size_t i = 0; i < pat->as.syn.count; i++) {
            uint32_t value_access = 0, eff = 0;
            if (!sel_row_add_syn_dict_has(row, access, pat->as.syn.keys[i], err, span)) return false;
            if (!selector_syn_access(selector, SEL_ACCESS_SYN_DICT_VALUE, access, 0, 0, pat->as.syn.keys[i], NULL, &value_access, err, span)) return false;
            if (!syn_effective_access(selector, value_access, pat->as.syn.items[i], &eff, err, span)) return false;
            if (!sel_row_add_cell(row, eff, pat->as.syn.items[i], err, span)) return false;
        }
        if (pat->as.syn.dict_rest) {
            uint32_t rest_access = 0, eff = 0;
            if (!selector_syn_access(selector, SEL_ACCESS_SYN_DICT_REST, access, 0, 0, NULL, pat, &rest_access, err, span)) return false;
            if (!syn_effective_access(selector, rest_access, pat->as.syn.dict_rest, &eff, err, span)) return false;
            if (!sel_row_add_cell(row, eff, pat->as.syn.dict_rest, err, span)) return false;
        }
        return true;
    }
    bool has_rest = pat->as.syn.rest_index != IDM_SYN_PAT_NO_REST;
    size_t prefix = has_rest ? pat->as.syn.rest_index : pat->as.syn.count;
    size_t suffix = has_rest ? pat->as.syn.count - prefix : 0u;
    for (size_t i = 0; i < prefix; i++) {
        uint32_t item_access = 0, eff = 0;
        if (!selector_syn_access(selector, SEL_ACCESS_SYN_ITEM, access, (uint32_t)i, 0, NULL, NULL, &item_access, err, span)) return false;
        if (!syn_effective_access(selector, item_access, pat->as.syn.items[i], &eff, err, span)) return false;
        if (!sel_row_add_cell(row, eff, pat->as.syn.items[i], err, span)) return false;
    }
    for (size_t j = 0; j < suffix; j++) {
        uint32_t item_access = 0, eff = 0;
        if (!selector_syn_access(selector, SEL_ACCESS_SYN_ITEM_REV, access, (uint32_t)(suffix - 1u - j), 0, NULL, NULL, &item_access, err, span)) return false;
        if (!syn_effective_access(selector, item_access, pat->as.syn.items[prefix + j], &eff, err, span)) return false;
        if (!sel_row_add_cell(row, eff, pat->as.syn.items[prefix + j], err, span)) return false;
    }
    if (has_rest && pat->as.syn.name) {
        uint32_t rest_access = 0;
        if (!selector_syn_access(selector, SEL_ACCESS_SYN_REST, access, (uint32_t)prefix, (uint32_t)suffix, NULL, NULL, &rest_access, err, span)) return false;
        if (!sel_row_add_name_action(row, SEL_ACTION_SYN_BIND, rest_access, pat->as.syn.name, pat->as.syn.slot, err, span)) return false;
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
        case SEL_PAT_SYNTAX:
            return row_add_syntax_subcells(selector, row, access, pat, err);
        case SEL_PAT_TYPE:
            return true;
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
    if (pat->kind == SEL_PAT_SYNTAX && pat->as.syn.kind == IDM_SYN_PAT_BIND) return sel_row_add_name_action(dst, SEL_ACTION_SYN_BIND, access, pat->as.syn.name, pat->as.syn.slot, err, pat->span);
    return true;
}

static bool lower_try_cell_to_action(SelRow *row, size_t cell_index, IdmError *err) {
    SelCell cell = row->cells[cell_index];
    SelPat *pat = cell.pattern;
    bool ok = false;
    switch (pat->kind) {
        case SEL_PAT_WILDCARD:
            ok = true;
            break;
        case SEL_PAT_BIND:
            ok = sel_row_add_name_action(row, SEL_ACTION_BIND, cell.access, pat->as.name.name, pat->as.name.slot, err, pat->span);
            break;
        case SEL_PAT_PIN:
            ok = sel_row_add_name_action(row, SEL_ACTION_PIN, cell.access, pat->as.name.name, pat->as.name.slot, err, pat->span);
            break;
        case SEL_PAT_LITERAL:
            if (literal_can_be_disjoint_ctor(pat->as.literal)) return idm_error_set(err, pat->span, "constructor pattern reached selector try node");
            ok = sel_row_add_value_equal(row, cell.access, pat->as.literal, err, pat->span);
            break;
        case SEL_PAT_SYNTAX:
            if (pat->as.syn.kind == IDM_SYN_PAT_WILDCARD) {
                ok = true;
                break;
            }
            if (pat->as.syn.kind == IDM_SYN_PAT_BIND) {
                ok = sel_row_add_name_action(row, SEL_ACTION_SYN_BIND, cell.access, pat->as.syn.name, pat->as.syn.slot, err, pat->span);
                break;
            }
            return idm_error_set(err, pat->span, "syntax constructor pattern reached selector try node");
        case SEL_PAT_PAIR:
        case SEL_PAT_VECTOR:
        case SEL_PAT_TUPLE:
        case SEL_PAT_VECTOR_REST:
        case SEL_PAT_TUPLE_REST:
        case SEL_PAT_DICT:
        case SEL_PAT_TYPE:
            return idm_error_set(err, pat->span, "constructor pattern reached selector try node");
    }
    if (!ok) return false;
    sel_row_remove_cell(row, cell_index);
    return true;
}

static bool lower_try_cells_to_actions(SelRow *row, IdmError *err) {
    while (row->cell_count != 0) {
        if (!lower_try_cell_to_action(row, 0, err)) return false;
    }
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
    if (sel_pat_ctor(pat, &own)) {
        if (sel_ctor_equal(own, ctor)) {
            *out_include = true;
            return sel_row_clone_without_cell(src, (size_t)idx, dst, err, pat->span) &&
                   row_add_ctor_subcells(selector, dst, access, pat, err);
        }
        if (sel_ctor_overlaps(own, ctor)) {
            *out_include = true;
            return sel_row_clone_without_cell(src, (size_t)idx, dst, err, pat->span) &&
                   sel_row_add_ctor_match(dst, access, own, err, pat->span) &&
                   row_add_ctor_subcells(selector, dst, access, pat, err);
        }
    }
    if (sel_pat_complex_literal_compatible(pat, ctor)) {
        *out_include = true;
        return sel_row_clone_without_cell(src, (size_t)idx, dst, err, pat->span) &&
               sel_row_add_value_equal(dst, access, pat->as.literal, err, pat->span);
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
        return sel_row_clone_without_cell(src, (size_t)idx, dst, err, pat->span) &&
               sel_row_add_value_equal(dst, access, pat->as.literal, err, pat->span);
    }
    return true;
}

static bool rows_append(SelRow **rows, size_t *count, size_t *cap, SelRow row, IdmError *err, IdmSpan span) {
    if (*count == *cap &&
        !idm_grow((void **)rows, cap, sizeof(**rows), 8u, *count + 1u)) {
        sel_row_destroy(&row);
        return idm_error_oom(err, span);
    }
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
    bool found = false;
    size_t best_distinct = 0;
    size_t best_ctor_rows = 0;
    for (size_t i = 0; i < first->cell_count; i++) {
        SelCtor ignored;
        if (!sel_pat_ctor(first->cells[i].pattern, &ignored)) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; j++) {
            if (first->cells[j].access == first->cells[i].access) { duplicate = true; break; }
        }
        if (duplicate) continue;
        size_t ctor_rows = 0;
        size_t distinct = 0;
        for (size_t r = 0; r < row_count; r++) {
            ssize_t idx = sel_row_find_cell(&rows[r], first->cells[i].access);
            if (idx < 0) continue;
            SelCtor ctor;
            if (!sel_pat_ctor(rows[r].cells[idx].pattern, &ctor)) continue;
            ctor_rows++;
            bool seen = false;
            for (size_t p = 0; p < r; p++) {
                ssize_t prev = sel_row_find_cell(&rows[p], first->cells[i].access);
                if (prev < 0) continue;
                SelCtor prev_ctor;
                if (!sel_pat_ctor(rows[p].cells[prev].pattern, &prev_ctor)) continue;
                if (sel_ctor_equal(prev_ctor, ctor)) { seen = true; break; }
            }
            if (!seen) distinct++;
        }
        if (!found || distinct > best_distinct || (distinct == best_distinct && ctor_rows > best_ctor_rows)) {
            found = true;
            best_distinct = distinct;
            best_ctor_rows = ctor_rows;
            *out_access = first->cells[i].access;
        }
    }
    return found;
}

static bool collect_ctors(const SelRow *rows, size_t row_count, uint32_t access, SelCtor **out_ctors, size_t *out_count, IdmError *err) {
    *out_ctors = NULL;
    *out_count = 0;
    size_t cap = 0;
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
        if (*out_count == cap &&
            !idm_grow((void **)out_ctors, &cap, sizeof(**out_ctors), 8u, *out_count + 1u)) {
            free(*out_ctors);
            *out_ctors = NULL;
            *out_count = 0;
            return idm_error_oom(err, rows[r].cells[idx].pattern->span);
        }
        (*out_ctors)[(*out_count)++] = ctor;
    }
    return true;
}

static uint32_t compile_rows(IdmPatternSelector *selector, const SelRow *rows, size_t row_count, IdmError *err);

static uint32_t make_try_node(IdmPatternSelector *selector, const SelRow *row, uint32_t next, IdmError *err) {
    SelRow lowered;
    memset(&lowered, 0, sizeof(lowered));
    if (!sel_row_clone(row, &lowered, err, idm_span_unknown(NULL)) ||
        !lower_try_cells_to_actions(&lowered, err)) {
        sel_row_destroy(&lowered);
        return SEL_NO_NODE;
    }
    uint32_t idx = sel_node_new(selector, SEL_NODE_TRY, err);
    if (idx == SEL_NO_NODE) {
        sel_row_destroy(&lowered);
        return SEL_NO_NODE;
    }
    SelNode *node = &selector->pool.nodes[idx];
    node->as.try_row.function_index = lowered.function_index;
    node->as.try_row.has_guard = lowered.has_guard;
    node->as.try_row.next = next;
    if (!copy_actions(&node->as.try_row.actions, &node->as.try_row.action_count, lowered.actions, lowered.action_count, err, idm_span_unknown(NULL))) {
        sel_row_destroy(&lowered);
        return SEL_NO_NODE;
    }
    sel_row_destroy(&lowered);
    return idx;
}

static uint32_t compile_rows(IdmPatternSelector *selector, const SelRow *rows, size_t row_count, IdmError *err) {
    if (row_count == 0) return sel_node_fail(selector, err);
    uint32_t access = 0;
    if (!choose_access(rows, row_count, &access)) {
        uint32_t next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (next == SEL_NO_NODE) return SEL_NO_NODE;
        return make_try_node(selector, &rows[0], next, err);
    }

    SelCtor *ctors = NULL;
    size_t ctor_count = 0;
    if (!collect_ctors(rows, row_count, access, &ctors, &ctor_count, err)) return SEL_NO_NODE;
    if (ctor_count == 0) {
        free(ctors);
        uint32_t next = compile_rows(selector, rows + 1u, row_count - 1u, err);
        if (next == SEL_NO_NODE) return SEL_NO_NODE;
        return make_try_node(selector, &rows[0], next, err);
    }

    uint32_t idx = sel_node_new(selector, SEL_NODE_SWITCH, err);
    if (idx == SEL_NO_NODE) { free(ctors); return SEL_NO_NODE; }
    {
        SelNode *node = &selector->pool.nodes[idx];
        node->as.sw.access = access;
        node->as.sw.cases = calloc(ctor_count, sizeof(*node->as.sw.cases));
        if (!node->as.sw.cases) {
            free(ctors);
            idm_error_oom(err, idm_span_unknown(NULL));
            return SEL_NO_NODE;
        }
        node->as.sw.case_count = ctor_count;
        node->as.sw.syn = true;
        for (size_t c = 0; c < ctor_count; c++) {
            if (!sel_ctor_native_syntax(ctors[c])) {
                node->as.sw.syn = false;
                break;
            }
        }
    }

    for (size_t c = 0; c < ctor_count; c++) {
        SelRow *case_rows = NULL;
        size_t case_count = 0;
        size_t case_cap = 0;
        for (size_t r = 0; r < row_count; r++) {
            SelRow specialized;
            memset(&specialized, 0, sizeof(specialized));
            bool include = false;
            if (!specialize_row_for_case(selector, &rows[r], access, ctors[c], &specialized, &include, err)) {
                rows_destroy(case_rows, case_count);
                free(ctors);
                return SEL_NO_NODE;
            }
            if (include && !rows_append(&case_rows, &case_count, &case_cap, specialized, err, idm_span_unknown(NULL))) {
                rows_destroy(case_rows, case_count);
                free(ctors);
                return SEL_NO_NODE;
            }
        }
        uint32_t child = compile_rows(selector, case_rows, case_count, err);
        rows_destroy(case_rows, case_count);
        if (child == SEL_NO_NODE) {
            free(ctors);
            return SEL_NO_NODE;
        }
        selector->pool.nodes[idx].as.sw.cases[c].ctor = ctors[c];
        selector->pool.nodes[idx].as.sw.cases[c].node = child;
    }

    SelRow *default_rows = NULL;
    size_t default_count = 0;
    size_t default_cap = 0;
    for (size_t r = 0; r < row_count; r++) {
        SelRow specialized;
        memset(&specialized, 0, sizeof(specialized));
        bool include = false;
        if (!specialize_row_for_default(&rows[r], access, ctors, ctor_count, &specialized, &include, err)) {
            rows_destroy(default_rows, default_count);
            free(ctors);
            return SEL_NO_NODE;
        }
        if (include && !rows_append(&default_rows, &default_count, &default_cap, specialized, err, idm_span_unknown(NULL))) {
            rows_destroy(default_rows, default_count);
            free(ctors);
            return SEL_NO_NODE;
        }
    }
    uint32_t def = compile_rows(selector, default_rows, default_count, err);
    rows_destroy(default_rows, default_count);
    free(ctors);
    if (def == SEL_NO_NODE) return SEL_NO_NODE;
    selector->pool.nodes[idx].as.sw.default_node = def;
    return idx;
}

static bool sel_action_equal(const SelAction *a, const SelAction *b) {
    if (a->kind != b->kind || a->access != b->access || a->slot != b->slot) return false;
    switch (a->kind) {
        case SEL_ACTION_BIND:
        case SEL_ACTION_PIN:
        case SEL_ACTION_SYN_BIND:
            if ((a->name == NULL) != (b->name == NULL)) return false;
            return !a->name || strcmp(a->name, b->name) == 0;
        case SEL_ACTION_CTOR_MATCH:
            return sel_ctor_equal(a->ctor, b->ctor);
        case SEL_ACTION_VALUE_EQUAL:
            return idm_value_equal(a->value, b->value);
        case SEL_ACTION_DICT_HAS:
            return idm_value_equal(a->key, b->key);
        case SEL_ACTION_SYN_DICT_HAS:
            return idm_syn_equal(a->syn_key, b->syn_key);
    }
    return false;
}

static bool sel_node_equal(const SelNode *a, const SelNode *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case SEL_NODE_FAIL:
            return true;
        case SEL_NODE_TRY:
            if (a->as.try_row.function_index != b->as.try_row.function_index ||
                a->as.try_row.has_guard != b->as.try_row.has_guard ||
                a->as.try_row.next != b->as.try_row.next ||
                a->as.try_row.action_count != b->as.try_row.action_count) return false;
            for (size_t i = 0; i < a->as.try_row.action_count; i++) {
                if (!sel_action_equal(&a->as.try_row.actions[i], &b->as.try_row.actions[i])) return false;
            }
            return true;
        case SEL_NODE_SWITCH:
            if (a->as.sw.access != b->as.sw.access ||
                a->as.sw.default_node != b->as.sw.default_node ||
                a->as.sw.syn != b->as.sw.syn ||
                a->as.sw.case_count != b->as.sw.case_count) return false;
            for (size_t i = 0; i < a->as.sw.case_count; i++) {
                if (!sel_ctor_equal(a->as.sw.cases[i].ctor, b->as.sw.cases[i].ctor) ||
                    a->as.sw.cases[i].node != b->as.sw.cases[i].node) return false;
            }
            return true;
        case SEL_NODE_FORK:
            return a->as.fork.first == b->as.fork.first && a->as.fork.second == b->as.fork.second;
        case SEL_NODE_BYTE:
            return a->as.byte.next == b->as.byte.next &&
                   a->as.byte.cls.negated == b->as.byte.cls.negated &&
                   memcmp(a->as.byte.cls.bits, b->as.byte.cls.bits, sizeof(a->as.byte.cls.bits)) == 0;
        case SEL_NODE_SAVE:
            return a->as.save.slot == b->as.save.slot && a->as.save.next == b->as.save.next;
        case SEL_NODE_GUARD:
            return a->as.guard.kind == b->as.guard.kind &&
                   a->as.guard.flags == b->as.guard.flags &&
                   a->as.guard.sub == b->as.guard.sub &&
                   a->as.guard.next == b->as.guard.next;
        case SEL_NODE_ACCEPT:
            return a->as.accept_id == b->as.accept_id;
    }
    return false;
}

static uint32_t selector_canon_node(IdmPatternSelector *selector, uint32_t node_idx, uint32_t *map, unsigned char *state, IdmError *err) {
    if (node_idx == SEL_NO_NODE) return SEL_NO_NODE;
    if (node_idx >= selector->pool.count) {
        idm_error_set(err, idm_span_unknown(NULL), "pattern selector node out of bounds");
        return SEL_NO_NODE;
    }
    if (state[node_idx] == 2u) return map[node_idx];
    if (state[node_idx] == 1u) {
        idm_error_set(err, idm_span_unknown(NULL), "pattern selector cycle");
        return SEL_NO_NODE;
    }
    state[node_idx] = 1u;
    SelNode *node = &selector->pool.nodes[node_idx];
    switch (node->kind) {
        case SEL_NODE_TRY:
            node->as.try_row.next = selector_canon_node(selector, node->as.try_row.next, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            break;
        case SEL_NODE_SWITCH:
            node->as.sw.default_node = selector_canon_node(selector, node->as.sw.default_node, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            for (size_t i = 0; i < node->as.sw.case_count; i++) {
                node->as.sw.cases[i].node = selector_canon_node(selector, node->as.sw.cases[i].node, map, state, err);
                if (err && err->present) return SEL_NO_NODE;
            }
            break;
        case SEL_NODE_FORK:
            node->as.fork.first = selector_canon_node(selector, node->as.fork.first, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            node->as.fork.second = selector_canon_node(selector, node->as.fork.second, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            break;
        case SEL_NODE_BYTE:
            node->as.byte.next = selector_canon_node(selector, node->as.byte.next, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            break;
        case SEL_NODE_SAVE:
            node->as.save.next = selector_canon_node(selector, node->as.save.next, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            break;
        case SEL_NODE_GUARD:
            node->as.guard.next = selector_canon_node(selector, node->as.guard.next, map, state, err);
            if (err && err->present) return SEL_NO_NODE;
            break;
        case SEL_NODE_FAIL:
        case SEL_NODE_ACCEPT:
            break;
    }
    for (size_t i = 0; i < selector->pool.count; i++) {
        if (i == node_idx || state[i] != 2u || map[i] != i) continue;
        if (sel_node_equal(node, &selector->pool.nodes[i])) {
            map[node_idx] = (uint32_t)i;
            state[node_idx] = 2u;
            return map[node_idx];
        }
    }
    map[node_idx] = node_idx;
    state[node_idx] = 2u;
    return node_idx;
}

static void selector_mark_node(const IdmPatternSelector *selector, uint32_t node_idx, bool *mark) {
    if (node_idx == SEL_NO_NODE || node_idx >= selector->pool.count || mark[node_idx]) return;
    mark[node_idx] = true;
    const SelNode *node = &selector->pool.nodes[node_idx];
    switch (node->kind) {
        case SEL_NODE_TRY:
            selector_mark_node(selector, node->as.try_row.next, mark);
            break;
        case SEL_NODE_SWITCH:
            selector_mark_node(selector, node->as.sw.default_node, mark);
            for (size_t i = 0; i < node->as.sw.case_count; i++) selector_mark_node(selector, node->as.sw.cases[i].node, mark);
            break;
        case SEL_NODE_FORK:
            selector_mark_node(selector, node->as.fork.first, mark);
            selector_mark_node(selector, node->as.fork.second, mark);
            break;
        case SEL_NODE_BYTE:
            selector_mark_node(selector, node->as.byte.next, mark);
            break;
        case SEL_NODE_SAVE:
            selector_mark_node(selector, node->as.save.next, mark);
            break;
        case SEL_NODE_GUARD:
            selector_mark_node(selector, node->as.guard.next, mark);
            break;
        case SEL_NODE_FAIL:
        case SEL_NODE_ACCEPT:
            break;
    }
}

static void selector_remap_node(SelNode *node, const uint32_t *remap) {
    switch (node->kind) {
        case SEL_NODE_TRY:
            node->as.try_row.next = remap[node->as.try_row.next];
            break;
        case SEL_NODE_SWITCH:
            node->as.sw.default_node = remap[node->as.sw.default_node];
            for (size_t i = 0; i < node->as.sw.case_count; i++) node->as.sw.cases[i].node = remap[node->as.sw.cases[i].node];
            break;
        case SEL_NODE_FORK:
            node->as.fork.first = remap[node->as.fork.first];
            node->as.fork.second = remap[node->as.fork.second];
            break;
        case SEL_NODE_BYTE:
            node->as.byte.next = remap[node->as.byte.next];
            break;
        case SEL_NODE_SAVE:
            node->as.save.next = remap[node->as.save.next];
            break;
        case SEL_NODE_GUARD:
            node->as.guard.next = remap[node->as.guard.next];
            break;
        case SEL_NODE_FAIL:
        case SEL_NODE_ACCEPT:
            break;
    }
}

static bool selector_compact_nodes(IdmPatternSelector *selector, IdmError *err) {
    size_t old_count = selector->pool.count;
    bool *mark = old_count == 0 ? NULL : calloc(old_count, sizeof(*mark));
    uint32_t *remap = old_count == 0 ? NULL : malloc(old_count * sizeof(*remap));
    if ((old_count != 0 && (!mark || !remap))) {
        free(mark);
        free(remap);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < selector->arity_count; i++) selector_mark_node(selector, selector->arities[i].node, mark);
    size_t new_count = 0;
    for (size_t i = 0; i < old_count; i++) {
        if (mark[i]) remap[i] = (uint32_t)new_count++;
        else remap[i] = SEL_NO_NODE;
    }
    SelNode *nodes = new_count == 0 ? NULL : calloc(new_count, sizeof(*nodes));
    if (new_count != 0 && !nodes) {
        free(mark);
        free(remap);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < old_count; i++) {
        if (mark[i]) {
            nodes[remap[i]] = selector->pool.nodes[i];
            nodes[remap[i]].index = remap[i];
            selector_remap_node(&nodes[remap[i]], remap);
        } else {
            sel_node_destroy_contents(&selector->pool.nodes[i]);
        }
    }
    for (size_t i = 0; i < selector->arity_count; i++) selector->arities[i].node = remap[selector->arities[i].node];
    free(selector->pool.nodes);
    selector->pool.nodes = nodes;
    selector->pool.count = new_count;
    selector->pool.cap = new_count;
    free(mark);
    free(remap);
    return true;
}

static bool selector_merge_dag(IdmPatternSelector *selector, IdmError *err) {
    size_t n = selector->pool.count;
    uint32_t *map = n == 0 ? NULL : malloc(n * sizeof(*map));
    unsigned char *state = n == 0 ? NULL : calloc(n, sizeof(*state));
    if (n != 0 && (!map || !state)) {
        free(map);
        free(state);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < n; i++) map[i] = SEL_NO_NODE;
    for (size_t i = 0; i < selector->arity_count; i++) {
        selector->arities[i].node = selector_canon_node(selector, selector->arities[i].node, map, state, err);
        if (err && err->present) {
            free(map);
            free(state);
            return false;
        }
    }
    free(map);
    free(state);
    return selector_compact_nodes(selector, err);
}

static bool sel_node_unconditional(const IdmPatternSelector *selector, uint32_t node_idx, uint32_t *out_function_index) {
    if (node_idx == SEL_NO_NODE) return false;
    const SelNode *node = &selector->pool.nodes[node_idx];
    if (node->kind != SEL_NODE_TRY) return false;
    if (node->as.try_row.action_count != 0 || node->as.try_row.has_guard) return false;
    *out_function_index = node->as.try_row.function_index;
    return true;
}

typedef struct {
    bool value_ready;
    bool value_available;
    IdmValue value;
    bool syn_ready;
    bool syn_available;
    const IdmSyntax *syn;
} SelAccessCache;

typedef struct {
    IdmRuntime *rt;
    const IdmPatternSelector *selector;
    const IdmValue *args;
    uint32_t argc;
    SelAccessCache *accesses;
} SelRun;

static bool sel_run_init(SelRun *run, IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, IdmError *err) {
    memset(run, 0, sizeof(*run));
    run->rt = rt;
    run->selector = selector;
    run->args = args;
    run->argc = argc;
    if (selector->access_count != 0) {
        run->accesses = calloc(selector->access_count, sizeof(*run->accesses));
        if (!run->accesses) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static void sel_run_destroy(SelRun *run) {
    free(run->accesses);
    memset(run, 0, sizeof(*run));
}

static bool selector_eval_access(SelRun *run, uint32_t access_id, IdmValue *out, bool *out_available, IdmError *err) {
    *out_available = false;
    const IdmPatternSelector *selector = run->selector;
    if (access_id >= selector->access_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern access out of bounds");
    SelAccessCache *cache = &run->accesses[access_id];
    if (cache->value_ready) {
        *out_available = cache->value_available;
        if (cache->value_available) *out = cache->value;
        return true;
    }
    const SelAccess *access = &selector->accesses[access_id];
    bool ok = true;
    switch (access->kind) {
        case SEL_ACCESS_ARG:
            if (access->index >= run->argc) break;
            *out = run->args[access->index];
            *out_available = true;
            break;
        case SEL_ACCESS_CAR: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || !idm_is_pair(parent)) break;
            *out = idm_car(parent, err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_CDR: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || !idm_is_pair(parent)) break;
            *out = idm_cdr(parent, err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SEQ_ITEM: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || (idm_value_tag(parent) != IDM_VAL_VECTOR && idm_value_tag(parent) != IDM_VAL_TUPLE) || access->index >= idm_sequence_count(parent)) break;
            *out = idm_sequence_item(parent, access->index, err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SEQ_REST: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != access->seq_tag) break;
            size_t n = idm_sequence_count(parent);
            if (access->index > n) break;
            size_t rest_count = n - access->index;
            IdmValue *items = rest_count == 0 ? NULL : calloc(rest_count, sizeof(*items));
            if (rest_count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
            for (size_t i = 0; i < rest_count; i++) {
                items[i] = idm_sequence_item(parent, access->index + i, err);
                if (err && err->present) { free(items); return false; }
            }
            *out = access->seq_tag == IDM_VAL_VECTOR ? idm_vector(run->rt, items, rest_count, err) : idm_tuple(run->rt, items, rest_count, err);
            free(items);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_DICT_VALUE: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || !idm_is_dict(parent)) break;
            IdmValue value = idm_nil();
            if (!idm_dict_get(parent, access->u.key, &value)) break;
            *out = value;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_DICT_REST: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || !idm_is_dict(parent) || !access->u.dict_pat) break;
            const SelPat *pat = access->u.dict_pat;
            IdmValue rest = idm_nil();
            if (!sel_dict_rest_value(run->rt, parent, pat->as.dict.entries, pat->as.dict.count, &rest, err, pat->span)) return false;
            *out = rest;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_ITEM: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            const IdmSyntax *syn = idm_syntax_value_get(parent);
            if (!syn || syn->kind < IDM_SYN_LIST || access->index >= syn->as.seq.count) break;
            *out = idm_syntax_value(run->rt, syn->as.seq.items[access->index], err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_ITEM_REV: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            const IdmSyntax *syn = idm_syntax_value_get(parent);
            if (!syn || syn->kind < IDM_SYN_LIST || access->index >= syn->as.seq.count) break;
            *out = idm_syntax_value(run->rt, syn->as.seq.items[syn->as.seq.count - 1u - access->index], err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_REST: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            const IdmSyntax *syn = idm_syntax_value_get(parent);
            if (!syn || syn->kind < IDM_SYN_LIST || (size_t)access->index + (size_t)access->index2 > syn->as.seq.count) break;
            size_t stop = syn->as.seq.count - access->index2;
            IdmValue list = idm_empty_list();
            for (size_t i = stop; i > access->index; i--) {
                IdmValue item = idm_syntax_value(run->rt, syn->as.seq.items[i - 1u], err);
                if (err && err->present) return false;
                list = idm_cons(run->rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_DICT_VALUE: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            const IdmSyntax *value = syntax_dict_value_for_key(idm_syntax_value_get(parent), access->u.syn_key);
            if (!value) break;
            *out = idm_syntax_value(run->rt, value, err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_DICT_REST: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            const IdmSyntax *syn = idm_syntax_value_get(parent);
            if (!syn || syn->kind != IDM_SYN_DICT || (syn->as.seq.count % 2u) != 0) break;
            const SelPat *dp = access->u.dict_pat;
            IdmSyntax *rest = idm_syn_dict(syn->span);
            if (!rest) return idm_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i += 2u) {
                bool required = false;
                for (size_t k = 0; dp && k < dp->as.syn.count; k++) {
                    if (idm_syn_equal(dp->as.syn.keys[k], syn->as.seq.items[i])) { required = true; break; }
                }
                if (required) continue;
                IdmSyntax *key = idm_syn_clone(syn->as.seq.items[i]);
                IdmSyntax *value = idm_syn_clone(syn->as.seq.items[i + 1u]);
                if (!key || !value || !idm_syn_append(rest, key) || !idm_syn_append(rest, value)) {
                    idm_syn_free(key); idm_syn_free(value); idm_syn_free(rest);
                    return idm_error_oom(err, syn->span);
                }
            }
            *out = idm_syntax_value(run->rt, rest, err);
            idm_syn_free(rest);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        case SEL_ACCESS_SYN_UNWRAP_EXPR: {
            IdmValue parent = idm_nil();
            bool parent_available = false;
            if (!selector_eval_access(run, access->parent, &parent, &parent_available, err)) return false;
            if (!parent_available || idm_value_tag(parent) != IDM_VAL_SYNTAX) break;
            *out = idm_syntax_value(run->rt, syn_subject_unwrap_expr(idm_syntax_value_get(parent)), err);
            if (err && err->present) return false;
            *out_available = true;
            break;
        }
        default:
            ok = idm_error_set(err, idm_span_unknown(NULL), "unknown pattern access kind");
            break;
    }
    if (!ok) return false;
    cache->value_ready = true;
    cache->value_available = *out_available;
    if (*out_available) cache->value = *out;
    return true;
}

static bool selector_eval_syn(SelRun *run, uint32_t access_id, const IdmSyntax **out, bool *out_available, IdmError *err) {
    *out_available = false;
    const IdmPatternSelector *selector = run->selector;
    if (access_id >= selector->access_count) return idm_error_set(err, idm_span_unknown(NULL), "pattern access out of bounds");
    SelAccessCache *cache = &run->accesses[access_id];
    if (cache->syn_ready) {
        *out_available = cache->syn_available;
        if (cache->syn_available) *out = cache->syn;
        return true;
    }
    const SelAccess *a = &selector->accesses[access_id];
    if (a->kind == SEL_ACCESS_ARG) {
        if (a->index >= run->argc || idm_value_tag(run->args[a->index]) != IDM_VAL_SYNTAX) {
            cache->syn_ready = true;
            return true;
        }
        *out = idm_syntax_value_get(run->args[a->index]);
        *out_available = *out != NULL;
        cache->syn_ready = true;
        cache->syn_available = *out_available;
        cache->syn = *out_available ? *out : NULL;
        return true;
    }
    const IdmSyntax *parent = NULL;
    bool ok = false;
    if (!selector_eval_syn(run, a->parent, &parent, &ok, err)) return false;
    if (!ok || !parent) {
        cache->syn_ready = true;
        return true;
    }
    switch (a->kind) {
        case SEL_ACCESS_SYN_ITEM:
            if (parent->kind < IDM_SYN_LIST || a->index >= parent->as.seq.count) break;
            *out = parent->as.seq.items[a->index];
            break;
        case SEL_ACCESS_SYN_ITEM_REV:
            if (parent->kind < IDM_SYN_LIST || a->index >= parent->as.seq.count) break;
            *out = parent->as.seq.items[parent->as.seq.count - 1u - a->index];
            break;
        case SEL_ACCESS_SYN_DICT_VALUE: {
            const IdmSyntax *v = syntax_dict_value_for_key(parent, a->u.syn_key);
            if (!v) break;
            *out = v;
            break;
        }
        case SEL_ACCESS_SYN_UNWRAP_EXPR:
            *out = syn_subject_unwrap_expr(parent);
            break;
        default:
            return idm_error_set(err, idm_span_unknown(NULL), "non-structural access in native syntax eval");
    }
    *out_available = *out != NULL;
    cache->syn_ready = true;
    cache->syn_available = *out_available;
    cache->syn = *out_available ? *out : NULL;
    return true;
}

static bool sel_ctor_matches_syn(SelCtor ctor, const IdmSyntax *syn) {
    if (ctor.kind == SEL_CTOR_SYN_KIND) {
        if (!syn || syn->kind != ctor.syn_kind) return false;
        if (syn->kind == IDM_SYN_DICT && (syn->as.seq.count % 2u) != 0) return false;
        if (syn->kind < IDM_SYN_LIST) return true;
        return ctor.syn_rest ? syn->as.seq.count >= ctor.count : syn->as.seq.count == ctor.count;
    }
    if (ctor.kind == SEL_CTOR_SYN_LITERAL) return idm_syn_equal(ctor.syn_literal, syn);
    return false;
}

static bool selector_apply_action(SelRun *run, const SelAction *action, IdmPatternBindings *bindings, IdmError *err) {
    if (action->kind == SEL_ACTION_SYN_DICT_HAS) {
        const IdmSyntax *syn = NULL;
        bool avail = false;
        if (!selector_eval_syn(run, action->access, &syn, &avail, err)) return false;
        return avail && syntax_dict_value_for_key(syn, action->syn_key) != NULL;
    }
    IdmValue value = idm_nil();
    bool available = false;
    if (!selector_eval_access(run, action->access, &value, &available, err)) return false;
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
        case SEL_ACTION_CTOR_MATCH:
            return sel_ctor_matches_value(action->ctor, value);
        case SEL_ACTION_VALUE_EQUAL:
            return idm_value_equal(action->value, value);
        case SEL_ACTION_DICT_HAS:
            {
                IdmValue ignored = idm_nil();
                return idm_is_dict(value) && idm_dict_get(value, action->key, &ignored);
            }
        case SEL_ACTION_SYN_BIND: {
            const IdmValue *existing = action->slot != UINT32_MAX
                ? idm_pattern_bindings_get_slot(bindings, action->slot)
                : idm_pattern_bindings_get(bindings, action->name);
            if (existing) return syntax_bound_value_equal(*existing, value);
            if (!idm_pattern_bindings_add_slot(bindings, action->name, action->slot, value)) return idm_error_oom(err, idm_span_unknown(NULL));
            return true;
        }
        case SEL_ACTION_SYN_DICT_HAS:
            return idm_value_tag(value) == IDM_VAL_SYNTAX && syntax_dict_value_for_key(idm_syntax_value_get(value), action->syn_key) != NULL;
    }
    return false;
}

static bool selector_exec_node(SelRun *run, uint32_t node_idx, IdmPatternGuardFn guard, void *guard_user, uint32_t *out_function_index, IdmPatternBindings *bindings, bool *out_matched, IdmError *err) {
    if (node_idx == SEL_NO_NODE) {
        *out_matched = false;
        return true;
    }
    const IdmPatternSelector *selector = run->selector;
    const SelNode *node = &selector->pool.nodes[node_idx];
    switch (node->kind) {
        case SEL_NODE_FAIL:
            *out_matched = false;
            return true;
        case SEL_NODE_TRY: {
            if (node->as.try_row.action_count == 0 && !node->as.try_row.has_guard) {
                *out_function_index = node->as.try_row.function_index;
                *out_matched = true;
                return true;
            }
            size_t checkpoint = bindings->count;
            bool ok = true;
            for (size_t i = 0; ok && i < node->as.try_row.action_count; i++) {
                ok = selector_apply_action(run, &node->as.try_row.actions[i], bindings, err);
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
            return selector_exec_node(run, node->as.try_row.next, guard, guard_user, out_function_index, bindings, out_matched, err);
        }
        case SEL_NODE_SWITCH: {
            if (node->as.sw.syn) {
                const IdmSyntax *syn = NULL;
                bool available = false;
                if (!selector_eval_syn(run, node->as.sw.access, &syn, &available, err)) return false;
                if (available) {
                    for (size_t i = 0; i < node->as.sw.case_count; i++) {
                        if (!sel_ctor_matches_syn(node->as.sw.cases[i].ctor, syn)) continue;
                        return selector_exec_node(run, node->as.sw.cases[i].node, guard, guard_user, out_function_index, bindings, out_matched, err);
                    }
                }
                return selector_exec_node(run, node->as.sw.default_node, guard, guard_user, out_function_index, bindings, out_matched, err);
            }
            IdmValue value = idm_nil();
            bool available = false;
            if (!selector_eval_access(run, node->as.sw.access, &value, &available, err)) return false;
            if (available) {
                for (size_t i = 0; i < node->as.sw.case_count; i++) {
                    if (!sel_ctor_matches_value(node->as.sw.cases[i].ctor, value)) continue;
                    return selector_exec_node(run, node->as.sw.cases[i].node, guard, guard_user, out_function_index, bindings, out_matched, err);
                }
            }
            return selector_exec_node(run, node->as.sw.default_node, guard, guard_user, out_function_index, bindings, out_matched, err);
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
    if (selector->pattern_count == selector->pattern_cap &&
        !idm_grow((void **)&selector->patterns, &selector->pattern_cap, sizeof(*selector->patterns), 8u, selector->pattern_count + 1u)) {
        sel_pat_free(pat);
        return idm_error_oom(err, span);
    }
    selector->patterns[selector->pattern_count++] = pat;
    return true;
}

bool idm_pattern_selector_build(IdmRuntime *rt, const IdmPatternSelectorClause *clauses, size_t clause_count, IdmPatternSelector **out, IdmError *err) {
    *out = NULL;
    if (!rt) return idm_error_set(err, idm_span_unknown(NULL), "pattern selector requires a runtime");
    IdmPatternSelector *selector = calloc(1u, sizeof(*selector));
    if (!selector) return idm_error_oom(err, idm_span_unknown(NULL));
    atomic_init(&selector->refcount, 1u);

    IdmArity *arities = NULL;
    size_t arity_count = 0;
    size_t arity_cap = 0;
    for (size_t i = 0; i < clause_count; i++) {
        bool seen = false;
        for (size_t j = 0; j < arity_count; j++) if (idm_arity_equal(&arities[j], &clauses[i].arity)) { seen = true; break; }
        if (seen) continue;
        if (arity_count == arity_cap &&
            !idm_grow((void **)&arities, &arity_cap, sizeof(*arities), 4u, arity_count + 1u)) {
            free(arities);
            idm_pattern_selector_free(selector);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
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
        size_t row_cap = 0;
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
                row.cell_cap = exact_arity;
                for (uint32_t p = 0; p < exact_arity; p++) {
                    uint32_t root = 0;
                    SelPat *pat = sel_pat_from_idm(rt, clauses[c].patterns[p], clauses[c].pattern_locals, clauses[c].pattern_local_count, err);
                    uint32_t eff = 0;
                    if (!pat || !selector_add_pattern_root(selector, pat, err, clauses[c].patterns[p]->span) ||
                        !selector_root_access(selector, p, &root, err, clauses[c].patterns[p]->span) ||
                        !syn_effective_access(selector, root, pat, &eff, err, clauses[c].patterns[p]->span)) {
                        sel_row_destroy(&row);
                        rows_destroy(rows, row_count);
                        free(arities);
                        idm_pattern_selector_free(selector);
                        return false;
                    }
                    row.cells[p].access = eff;
                    row.cells[p].pattern = pat;
                }
            }
            if (!rows_append(&rows, &row_count, &row_cap, row, err, idm_span_unknown(NULL))) {
                rows_destroy(rows, row_count);
                free(arities);
                idm_pattern_selector_free(selector);
                return false;
            }
        }
        selector->arities[a].arity = arity;
        selector->arities[a].node = compile_rows(selector, rows, row_count, err);
        rows_destroy(rows, row_count);
        if (selector->arities[a].node == SEL_NO_NODE) {
            free(arities);
            idm_pattern_selector_free(selector);
            return false;
        }
    }

    if (!selector_merge_dag(selector, err)) {
        free(arities);
        idm_pattern_selector_free(selector);
        return false;
    }
    selector->has_unconditional = false;
    for (size_t a = 0; a < arity_count; a++) {
        selector->arities[a].unconditional = sel_node_unconditional(selector, selector->arities[a].node, &selector->arities[a].function_index);
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

size_t idm_pattern_selector_node_count(const IdmPatternSelector *selector) {
    return selector ? selector->pool.count : 0u;
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
    SelRun run;
    if (!sel_run_init(&run, rt, selector, args, argc, err)) return false;
    size_t checkpoint = out_bindings->count;
    if (!selector_exec_node(&run, arity_case->node, guard, guard_user, out_function_index, out_bindings, out_matched, err)) {
        sel_run_destroy(&run);
        bindings_truncate(out_bindings, checkpoint);
        return false;
    }
    sel_run_destroy(&run);
    if (!*out_matched) {
        bindings_truncate(out_bindings, checkpoint);
        return true;
    }
    *out_has_bindings = out_bindings->count != checkpoint;
    return true;
}

struct SelByteProg {
    SelNodePool pool;
    uint32_t start;
    size_t capture_count;
    uint32_t flags;
};

typedef struct {
    uint32_t node;
    size_t pos;
    size_t capture_index;
} SelByteState;

typedef struct {
    SelByteState *items;
    IdmByteCapture *captures;
    size_t count;
    size_t cap;
    size_t capture_count;
} SelByteStateVec;

enum { SEL_STACK_CAPTURE_LIMIT = 16 };
enum { SEL_MAX_CLOSURE_DEPTH = 10000 };

void idm_byteclass_set(IdmByteClass *cls, unsigned char c) {
    cls->bits[c >> 3] |= (unsigned char)(1u << (c & 7u));
}

bool idm_byteclass_has(const IdmByteClass *cls, unsigned char c) {
    bool in = (cls->bits[c >> 3] & (unsigned char)(1u << (c & 7u))) != 0;
    return cls->negated ? !in : in;
}

void idm_byteclass_add_char(IdmByteClass *cls, unsigned char c, bool caseless) {
    idm_byteclass_set(cls, c);
    if (caseless && isalpha(c)) {
        idm_byteclass_set(cls, (unsigned char)tolower(c));
        idm_byteclass_set(cls, (unsigned char)toupper(c));
    }
}

void idm_byteclass_add_range(IdmByteClass *cls, unsigned char lo, unsigned char hi, bool caseless) {
    if (lo > hi) {
        unsigned char tmp = lo;
        lo = hi;
        hi = tmp;
    }
    for (unsigned i = lo; i <= hi; i++) idm_byteclass_add_char(cls, (unsigned char)i, caseless);
}

void idm_byteclass_add_pred(IdmByteClass *cls, int (*pred)(int), bool caseless) {
    for (unsigned i = 0; i < 256u; i++) {
        if (pred((int)i)) idm_byteclass_add_char(cls, (unsigned char)i, caseless);
    }
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

static bool sel_byte_state_push(SelByteStateVec *vec, uint32_t node, size_t pos, const IdmByteCapture *captures, IdmError *err) {
    if (vec->count == vec->cap) {
        if (vec->capture_count != 0) {
            if (vec->capture_count > SIZE_MAX / sizeof(*vec->captures)) return idm_error_oom(err, idm_span_unknown(NULL));
            IdmGrowItem items[] = {
                { .base = (void **)&vec->items, .elem_size = sizeof(*vec->items) },
                { .base = (void **)&vec->captures, .elem_size = vec->capture_count * sizeof(*vec->captures) },
            };
            if (!idm_growv(items, 2u, &vec->cap, 16u, vec->count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
        } else {
            if (!idm_grow((void **)&vec->items, &vec->cap, sizeof(*vec->items), 16u, vec->count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
        }
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

static const IdmByteCapture *sel_byte_state_captures(const SelByteStateVec *vec, const SelByteState *state) {
    return vec->capture_count == 0 ? NULL : vec->captures + state->capture_index * vec->capture_count;
}

void idm_byte_match_destroy(IdmByteMatch *m) {
    free(m->captures);
    m->captures = NULL;
    m->matched = false;
    m->index = 0;
    m->end = 0;
    m->capture_count = 0;
}

static bool sel_byte_match_take_best(IdmByteMatch *m, size_t end, size_t accept_id, const IdmByteCapture *captures, size_t capture_count, IdmError *err) {
    if (m->matched && (end < m->end || (end == m->end && accept_id >= m->index))) return true;
    IdmByteCapture *copy = capture_count == 0 ? NULL : malloc(capture_count * sizeof(*copy));
    if (capture_count != 0 && !copy) return idm_error_oom(err, idm_span_unknown(NULL));
    if (capture_count != 0) memcpy(copy, captures, capture_count * sizeof(*copy));
    free(m->captures);
    m->captures = copy;
    m->matched = true;
    m->index = accept_id;
    m->end = end;
    m->capture_count = capture_count;
    return true;
}

static bool sel_byte_run(const SelByteProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, IdmByteMatch *out, IdmError *err);

static bool sel_look_matches(const SelByteProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    IdmByteMatch m = {0};
    bool ok = sel_byte_run(prog, s, len, pos, false, 0, false, &m, err);
    bool matched = ok && m.matched;
    idm_byte_match_destroy(&m);
    return ok && matched;
}

static bool sel_lookbehind_matches(const SelByteProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    for (size_t start = 0; start <= pos; start++) {
        IdmByteMatch m = {0};
        bool ok = sel_byte_run(prog, s, len, start, true, pos, false, &m, err);
        bool matched = ok && m.matched;
        idm_byte_match_destroy(&m);
        if (!ok) return false;
        if (matched) return true;
    }
    return false;
}

static bool sel_closure(const SelByteProg *prog, SelByteStateVec *vec, uint32_t *marks, uint32_t mark, const char *s, size_t len, uint32_t node_index, size_t pos, const IdmByteCapture *captures, bool capture, unsigned depth, IdmError *err) {
    if (node_index >= prog->pool.count || pos > len) return idm_error_set(err, idm_span_unknown(NULL), "byte automaton state out of bounds");
    if (marks[node_index] == mark) return true;
    marks[node_index] = mark;
    if (depth > SEL_MAX_CLOSURE_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "regex too complex to evaluate");
    const SelNode *node = &prog->pool.nodes[node_index];
    switch (node->kind) {
        case SEL_NODE_FORK:
            return sel_closure(prog, vec, marks, mark, s, len, node->as.fork.first, pos, captures, capture, depth + 1u, err)
                && sel_closure(prog, vec, marks, mark, s, len, node->as.fork.second, pos, captures, capture, depth + 1u, err);
        case SEL_NODE_SAVE: {
            if (!capture) return sel_closure(prog, vec, marks, mark, s, len, node->as.save.next, pos, captures, capture, depth + 1u, err);
            IdmByteCapture stack_next[SEL_STACK_CAPTURE_LIMIT];
            IdmByteCapture *next = prog->capture_count <= SEL_STACK_CAPTURE_LIMIT ? stack_next : malloc(prog->capture_count * sizeof(*next));
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

static bool sel_byte_run(const SelByteProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, IdmByteMatch *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!prog || prog->pool.count == 0 || offset > len || (exact_end && end_pos > len)) return true;
    if (prog->pool.count > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, idm_span_unknown(NULL), "byte automaton too large");
    uint32_t mark_stack[256];
    uint32_t *marks = prog->pool.count <= 256u ? mark_stack : calloc(prog->pool.count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, prog->pool.count * sizeof(*mark_stack));
    size_t capture_count = capture ? prog->capture_count : 0u;
    IdmByteCapture initial_stack[SEL_STACK_CAPTURE_LIMIT];
    IdmByteCapture *initial = capture_count <= SEL_STACK_CAPTURE_LIMIT ? initial_stack : calloc(capture_count, sizeof(*initial));
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
            const IdmByteCapture *caps = sel_byte_state_captures(&active, state);
            const SelNode *node = &prog->pool.nodes[state->node];
            if (node->kind == SEL_NODE_ACCEPT) {
                if ((!exact_end || state->pos == end_pos) && !sel_byte_match_take_best(out, state->pos, node->as.accept_id, caps, capture_count, err)) ok = false;
            } else if (node->kind == SEL_NODE_BYTE) {
                if (state->pos < len && idm_byteclass_has(&node->as.byte.cls, (unsigned char)s[state->pos]))
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
    if (!ok) idm_byte_match_destroy(out);
    return ok;
}

SelByteProg *idm_byteprog_new(IdmError *err) {
    SelByteProg *p = calloc(1u, sizeof(*p));
    if (!p) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    return p;
}

void idm_byteprog_free(SelByteProg *p) {
    if (!p) return;
    pool_destroy(&p->pool);
    free(p);
}

bool idm_byteprog_test(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    IdmByteMatch m = {0};
    bool ok = sel_byte_run(p, s, len, offset, exact_end, end_pos, false, &m, err);
    *out_matched = m.matched;
    idm_byte_match_destroy(&m);
    return ok;
}

static uint32_t byteprog_node(SelByteProg *p, SelNodeKind kind, IdmError *err) {
    return pool_new(&p->pool, kind, err);
}

uint32_t idm_byteprog_fork(SelByteProg *p, IdmError *err) { return byteprog_node(p, SEL_NODE_FORK, err); }

uint32_t idm_byteprog_byte(SelByteProg *p, const IdmByteClass *cls, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_BYTE, err);
    if (i != SEL_NO_NODE) {
        p->pool.nodes[i].as.byte.cls = *cls;
    }
    return i;
}

uint32_t idm_byteprog_save(SelByteProg *p, uint32_t slot, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_SAVE, err);
    if (i != SEL_NO_NODE) p->pool.nodes[i].as.save.slot = slot;
    return i;
}

uint32_t idm_byteprog_guard(SelByteProg *p, IdmByteGuardKind kind, uint32_t flags, SelByteProg *sub, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_GUARD, err);
    if (i != SEL_NO_NODE) {
        p->pool.nodes[i].as.guard.kind = (SelGuardKind)kind;
        p->pool.nodes[i].as.guard.flags = flags;
        p->pool.nodes[i].as.guard.sub = sub;
    } else {
        idm_byteprog_free(sub);
    }
    return i;
}

uint32_t idm_byteprog_accept(SelByteProg *p, uint32_t accept_id, IdmError *err) {
    uint32_t i = byteprog_node(p, SEL_NODE_ACCEPT, err);
    if (i != SEL_NO_NODE) p->pool.nodes[i].as.accept_id = accept_id;
    return i;
}

void idm_byteprog_set_byte_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node].as.byte.next = target; }
void idm_byteprog_set_save_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node].as.save.next = target; }
void idm_byteprog_set_guard_next(SelByteProg *p, uint32_t node, uint32_t target) { p->pool.nodes[node].as.guard.next = target; }
void idm_byteprog_set_fork(SelByteProg *p, uint32_t node, uint32_t first, uint32_t second) {
    p->pool.nodes[node].as.fork.first = first;
    p->pool.nodes[node].as.fork.second = second;
}
void idm_byteprog_set_start(SelByteProg *p, uint32_t start) { p->start = start; }
void idm_byteprog_set_capture_count(SelByteProg *p, size_t n) { p->capture_count = n; }
void idm_byteprog_set_flags(SelByteProg *p, uint32_t flags) { p->flags = flags; }
size_t idm_byteprog_node_count(const SelByteProg *p) { return p->pool.count; }

size_t idm_byteprog_footprint(const SelByteProg *p) {
    if (!p) return 0;
    size_t total = sizeof(*p) + p->pool.cap * sizeof(SelNode *) + p->pool.count * sizeof(SelNode);
    for (size_t i = 0; i < p->pool.count; i++) {
        const SelNode *n = &p->pool.nodes[i];
        if (n->kind == SEL_NODE_GUARD && n->as.guard.sub) total += idm_byteprog_footprint(n->as.guard.sub);
    }
    return total;
}


void idm_byteprog_finalize_linear(SelByteProg *p) {
    for (size_t i = 0; i < p->pool.count; i++) {
        SelNode *n = &p->pool.nodes[i];
        uint32_t nxt = (uint32_t)(i + 1u);
        if (n->kind == SEL_NODE_BYTE) n->as.byte.next = nxt;
        else if (n->kind == SEL_NODE_SAVE) n->as.save.next = nxt;
        else if (n->kind == SEL_NODE_GUARD) n->as.guard.next = nxt;
    }
}

bool idm_byteprog_match(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, IdmByteMatch *out, IdmError *err) {
    return sel_byte_run(p, s, len, offset, exact_end, end_pos, capture, out, err);
}
