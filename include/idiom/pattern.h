#ifndef IDM_PATTERN_H
#define IDM_PATTERN_H

#include "idiom/value.h"

typedef enum {
    IDM_PAT_WILDCARD,
    IDM_PAT_BIND,
    IDM_PAT_PIN,
    IDM_PAT_LITERAL,
    IDM_PAT_PAIR,
    IDM_PAT_LIST,
    IDM_PAT_VECTOR,
    IDM_PAT_TUPLE,
    IDM_PAT_VECTOR_REST,
    IDM_PAT_TUPLE_REST,
    IDM_PAT_DICT
} IdmPatternKind;

typedef struct IdmPattern IdmPattern;

typedef struct {
    IdmValue key;
    IdmPattern *pattern;
} IdmDictPatternEntry;

typedef struct {
    char **names;
    IdmValue *values;
    size_t count;
    size_t cap;
} IdmPatternBindings;

struct IdmPattern {
    IdmPatternKind kind;
    IdmSpan span;
    union {
        char *name;
        IdmValue literal;
        struct { IdmPattern *left; IdmPattern *right; } pair;
        struct { IdmPattern **items; size_t count; } seq;
        struct { IdmPattern **items; size_t count; IdmPattern *rest; } seq_rest;
        struct { IdmDictPatternEntry *entries; size_t count; } dict;
    } as;
};

IdmPattern *idm_pat_wildcard(IdmSpan span);
IdmPattern *idm_pat_bind(const char *name, IdmSpan span);
IdmPattern *idm_pat_pin(const char *name, IdmSpan span);
IdmPattern *idm_pat_literal(IdmValue value, IdmSpan span);
IdmPattern *idm_pat_pair(IdmPattern *left, IdmPattern *right, IdmSpan span);
IdmPattern *idm_pat_sequence(IdmPatternKind kind, IdmPattern **items, size_t count, IdmSpan span);
IdmPattern *idm_pat_sequence_rest(IdmPatternKind kind, IdmPattern **items, size_t count, IdmPattern *rest, IdmSpan span);
IdmPattern *idm_pat_dict(IdmDictPatternEntry *entries, size_t count, IdmSpan span);
IdmPattern *idm_pat_clone(const IdmPattern *pat);
void idm_pat_free(IdmPattern *pat);

void idm_pattern_bindings_init(IdmPatternBindings *bindings);
void idm_pattern_bindings_destroy(IdmPatternBindings *bindings);
const IdmValue *idm_pattern_bindings_get(const IdmPatternBindings *bindings, const char *name);
bool idm_pattern_bindings_add(IdmPatternBindings *bindings, const char *name, IdmValue value);
bool idm_pattern_match(IdmRuntime *rt, IdmPattern *pat, IdmValue value, IdmPatternBindings *bindings, IdmError *err);

#endif
