#ifndef IDM_PATTERN_H
#define IDM_PATTERN_H

#include "idiom/syntax.h"
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
    IDM_PAT_DICT,
    IDM_PAT_SYNTAX,
    IDM_PAT_TYPE
} IdmPatternKind;

typedef struct IdmPattern IdmPattern;
typedef struct IdmPatternSelector IdmPatternSelector;

typedef enum {
    IDM_SYN_PAT_WILDCARD,
    IDM_SYN_PAT_BIND,
    IDM_SYN_PAT_LITERAL,
    IDM_SYN_PAT_SEQUENCE
} IdmSyntaxPatternKind;

#define IDM_SYN_PAT_NO_REST ((size_t)-1)

typedef struct IdmSyntaxPattern IdmSyntaxPattern;

struct IdmSyntaxPattern {
    IdmSyntaxPatternKind kind;
    IdmSpan span;
    union {
        struct { char *name; uint32_t slot; } bind;
        IdmSyntax *literal;
        struct {
            IdmSyntaxKind kind;
            IdmSyntaxPattern **items;
            size_t count;
            size_t rest_index;
            char *rest_name;
            uint32_t rest_slot;
        } seq;
    } as;
};

typedef struct {
    char *name;
    uint32_t slot;
} IdmPatternLocal;

typedef struct {
    IdmValue key;
    IdmPattern *pattern;
} IdmDictPatternEntry;

#define IDM_PATTERN_INLINE_BINDINGS 4u

typedef struct {
    char **names;
    IdmValue *values;
    uint32_t *slots;
    size_t count;
    size_t cap;
    bool heap;
    char *inline_names[IDM_PATTERN_INLINE_BINDINGS];
    IdmValue inline_values[IDM_PATTERN_INLINE_BINDINGS];
    uint32_t inline_slots[IDM_PATTERN_INLINE_BINDINGS];
} IdmPatternBindings;

typedef struct {
    uint32_t function_index;
    IdmArity arity;
    IdmPattern *const *patterns;
    uint32_t pattern_count;
    const IdmPatternLocal *pattern_locals;
    uint32_t pattern_local_count;
    bool trivial_match;
    bool has_guard;
} IdmPatternSelectorClause;

typedef bool (*IdmPatternGuardFn)(void *user, uint32_t function_index, const IdmPatternBindings *bindings, bool *out_pass, IdmError *err);

struct IdmPattern {
    IdmPatternKind kind;
    IdmSpan span;
    union {
        char *name;
        IdmValue literal;
        struct { IdmPattern *left; IdmPattern *right; } pair;
        struct { IdmPattern **items; size_t count; } seq;
        struct { IdmPattern **items; size_t count; IdmPattern *rest; } seq_rest;
        struct { IdmDictPatternEntry *entries; size_t count; IdmPattern *rest; } dict;
        IdmSyntaxPattern *syntax;
    } as;
};

IdmSyntaxPattern *idm_syn_pat_wildcard(IdmSpan span);
IdmSyntaxPattern *idm_syn_pat_bind(const char *name, IdmSpan span);
IdmSyntaxPattern *idm_syn_pat_literal_take(IdmSyntax *literal, IdmSpan span);
IdmSyntaxPattern *idm_syn_pat_sequence(IdmSyntaxKind kind, IdmSyntaxPattern **items, size_t count, size_t rest_index, const char *rest_name, IdmSpan span);
IdmSyntaxPattern *idm_syn_pat_clone(const IdmSyntaxPattern *pat);
void idm_syn_pat_free(IdmSyntaxPattern *pat);

IdmPattern *idm_pat_wildcard(IdmSpan span);
IdmPattern *idm_pat_bind(const char *name, IdmSpan span);
IdmPattern *idm_pat_pin(const char *name, IdmSpan span);
IdmPattern *idm_pat_literal(IdmValue value, IdmSpan span);
IdmPattern *idm_pat_pair(IdmPattern *left, IdmPattern *right, IdmSpan span);
IdmPattern *idm_pat_sequence(IdmPatternKind kind, IdmPattern **items, size_t count, IdmSpan span);
IdmPattern *idm_pat_sequence_rest(IdmPatternKind kind, IdmPattern **items, size_t count, IdmPattern *rest, IdmSpan span);
IdmPattern *idm_pat_dict(IdmDictPatternEntry *entries, size_t count, IdmPattern *rest, IdmSpan span);
IdmPattern *idm_pat_syntax_take(IdmSyntaxPattern *syntax, IdmSpan span);
IdmPattern *idm_pat_type(const char *type, IdmSpan span);
IdmPattern *idm_pat_clone(const IdmPattern *pat);
void idm_pat_free(IdmPattern *pat);

void idm_pattern_bindings_init(IdmPatternBindings *bindings);
void idm_pattern_bindings_destroy(IdmPatternBindings *bindings);
void idm_pattern_bindings_move(IdmPatternBindings *dst, IdmPatternBindings *src);
const IdmValue *idm_pattern_bindings_get(const IdmPatternBindings *bindings, const char *name);
const IdmValue *idm_pattern_bindings_get_slot(const IdmPatternBindings *bindings, uint32_t slot);
bool idm_pattern_bindings_add(IdmPatternBindings *bindings, const char *name, IdmValue value);
bool idm_pattern_bindings_add_slot(IdmPatternBindings *bindings, const char *name, uint32_t slot, IdmValue value);

#define SEL_NO_NODE ((uint32_t)0xFFFFFFFFu)

typedef enum {
    IDM_BYTE_GUARD_LINE_START,
    IDM_BYTE_GUARD_LINE_END,
    IDM_BYTE_GUARD_LOOKAHEAD_POS,
    IDM_BYTE_GUARD_LOOKAHEAD_NEG,
    IDM_BYTE_GUARD_LOOKBEHIND_POS,
    IDM_BYTE_GUARD_LOOKBEHIND_NEG,
    IDM_BYTE_GUARD_WORD_BOUNDARY,
    IDM_BYTE_GUARD_NOT_WORD_BOUNDARY,
    IDM_BYTE_GUARD_BUFFER_START,
    IDM_BYTE_GUARD_BUFFER_END,
    IDM_BYTE_GUARD_BUFFER_END_NL
} IdmByteGuardKind;

typedef struct {
    unsigned char bits[32];
    bool negated;
} IdmByteClass;

typedef struct {
    bool set;
    size_t start;
    size_t end;
} IdmByteCapture;

typedef struct {
    bool matched;
    size_t index;
    size_t end;
    IdmByteCapture *captures;
    size_t capture_count;
} IdmByteMatch;

typedef struct SelByteProg SelByteProg;

SelByteProg *idm_byteprog_new(IdmError *err);
void idm_byteprog_free(SelByteProg *p);
uint32_t idm_byteprog_fork(SelByteProg *p, IdmError *err);
uint32_t idm_byteprog_byte(SelByteProg *p, const IdmByteClass *cls, IdmError *err);
uint32_t idm_byteprog_save(SelByteProg *p, uint32_t slot, IdmError *err);
uint32_t idm_byteprog_guard(SelByteProg *p, IdmByteGuardKind kind, uint32_t flags, SelByteProg *sub, IdmError *err);
uint32_t idm_byteprog_accept(SelByteProg *p, uint32_t accept_id, IdmError *err);
void idm_byteclass_set(IdmByteClass *cls, unsigned char c);
bool idm_byteclass_has(const IdmByteClass *cls, unsigned char c);
void idm_byteclass_add_char(IdmByteClass *cls, unsigned char c, bool caseless);
void idm_byteclass_add_range(IdmByteClass *cls, unsigned char lo, unsigned char hi, bool caseless);
void idm_byteclass_add_pred(IdmByteClass *cls, int (*pred)(int), bool caseless);
void idm_byteprog_set_byte_next(SelByteProg *p, uint32_t node, uint32_t target);
void idm_byteprog_set_save_next(SelByteProg *p, uint32_t node, uint32_t target);
void idm_byteprog_set_guard_next(SelByteProg *p, uint32_t node, uint32_t target);
void idm_byteprog_set_fork(SelByteProg *p, uint32_t node, uint32_t first, uint32_t second);
void idm_byteprog_set_start(SelByteProg *p, uint32_t start);
void idm_byteprog_set_capture_count(SelByteProg *p, size_t n);
void idm_byteprog_set_flags(SelByteProg *p, uint32_t flags);
size_t idm_byteprog_node_count(const SelByteProg *p);
size_t idm_byteprog_footprint(const SelByteProg *p);
void idm_byteprog_finalize_linear(SelByteProg *p);
bool idm_byteprog_test(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err);
bool idm_byteprog_match(const SelByteProg *p, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, IdmByteMatch *out, IdmError *err);
void idm_byte_match_destroy(IdmByteMatch *m);

bool idm_pattern_selector_build(IdmRuntime *rt, const IdmPatternSelectorClause *clauses, size_t clause_count, IdmPatternSelector **out, IdmError *err);
void idm_pattern_selector_retain(IdmPatternSelector *selector);
void idm_pattern_selector_free(IdmPatternSelector *selector);
size_t idm_pattern_selector_node_count(const IdmPatternSelector *selector);
bool idm_pattern_selector_select(IdmRuntime *rt, const IdmPatternSelector *selector, const IdmValue *args, uint32_t argc, IdmPatternGuardFn guard, void *guard_user, uint32_t *out_function_index, IdmPatternBindings *out_bindings, bool *out_has_bindings, bool *out_matched, IdmError *err);

#endif
