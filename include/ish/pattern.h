#ifndef ISH_PATTERN_H
#define ISH_PATTERN_H

#include "ish/value.h"

typedef enum {
    ISH_PAT_WILDCARD,
    ISH_PAT_BIND,
    ISH_PAT_PIN,
    ISH_PAT_LITERAL,
    ISH_PAT_PAIR,
    ISH_PAT_LIST,
    ISH_PAT_VECTOR,
    ISH_PAT_TUPLE,
    ISH_PAT_VECTOR_REST,
    ISH_PAT_TUPLE_REST,
    ISH_PAT_DICT,
    ISH_PAT_AS,
    ISH_PAT_GUARD,
    ISH_PAT_SYNTAX_ELLIPSIS
} IshPatternKind;

typedef struct IshPattern IshPattern;

typedef struct {
    IshValue key;
    IshPattern *pattern;
} IshDictPatternEntry;

typedef struct {
    char **names;
    IshValue *values;
    size_t count;
    size_t cap;
} IshPatternBindings;

struct IshPattern {
    IshPatternKind kind;
    IshSpan span;
    union {
        char *name;
        IshValue literal;
        struct { IshPattern *left; IshPattern *right; } pair;
        struct { IshPattern **items; size_t count; } seq;
        struct { IshPattern **items; size_t count; IshPattern *rest; } seq_rest;
        struct { IshDictPatternEntry *entries; size_t count; } dict;
    } as;
};

IshPattern *ish_pat_wildcard(IshSpan span);
IshPattern *ish_pat_bind(const char *name, IshSpan span);
IshPattern *ish_pat_pin(const char *name, IshSpan span);
IshPattern *ish_pat_literal(IshValue value, IshSpan span);
IshPattern *ish_pat_pair(IshPattern *left, IshPattern *right, IshSpan span);
IshPattern *ish_pat_sequence(IshPatternKind kind, IshPattern **items, size_t count, IshSpan span);
IshPattern *ish_pat_sequence_rest(IshPatternKind kind, IshPattern **items, size_t count, IshPattern *rest, IshSpan span);
IshPattern *ish_pat_dict(IshDictPatternEntry *entries, size_t count, IshSpan span);
IshPattern *ish_pat_clone(const IshPattern *pat);
void ish_pat_free(IshPattern *pat);

void ish_pattern_bindings_init(IshPatternBindings *bindings);
void ish_pattern_bindings_destroy(IshPatternBindings *bindings);
const IshValue *ish_pattern_bindings_get(const IshPatternBindings *bindings, const char *name);
bool ish_pattern_bindings_add(IshPatternBindings *bindings, const char *name, IshValue value);
bool ish_pattern_match(IshRuntime *rt, IshPattern *pat, IshValue value, IshPatternBindings *bindings, IshError *err);

#endif
