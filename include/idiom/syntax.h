#ifndef IDM_SYNTAX_H
#define IDM_SYNTAX_H

#include "idiom/common.h"
#include "idiom/scope.h"

typedef struct IdmSyntax IdmSyntax;

typedef enum {
    IDM_SYN_NIL,
    IDM_SYN_WORD,
    IDM_SYN_ATOM,
    IDM_SYN_INT,
    IDM_SYN_FLOAT,
    IDM_SYN_STRING,
    IDM_SYN_LIST,
    IDM_SYN_VECTOR,
    IDM_SYN_TUPLE,
    IDM_SYN_DICT
} IdmSyntaxKind;

typedef struct {
    int phase;
    IdmScopeSet scopes;
} IdmSyntaxPhaseScope;

typedef struct {
    IdmSyntaxPhaseScope *items;
    size_t count;
    size_t cap;
} IdmPhaseScopes;

typedef struct {
    char *key;
    char *value;
} IdmSyntaxProperty;

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} IdmOriginChain;

struct IdmSyntax {
    IdmSyntaxKind kind;
    IdmSpan span;
    IdmPhaseScopes scopes;
    IdmSyntaxProperty *properties;
    size_t property_count;
    size_t property_cap;
    IdmOriginChain origins;
    char *token_raw;
    bool token_leading_space;
    bool token_adjacent_previous;
    union {
        char *text;
        int64_t integer;
        double real;
        struct {
            IdmSyntax **items;
            size_t count;
            size_t cap;
        } seq;
    } as;
};

IdmSyntax *idm_syn_nil(IdmSpan span);
IdmSyntax *idm_syn_word(const char *text, IdmSpan span);
IdmSyntax *idm_syn_atom(const char *text, IdmSpan span);
IdmSyntax *idm_syn_int(int64_t value, IdmSpan span);
IdmSyntax *idm_syn_float(double value, IdmSpan span);
IdmSyntax *idm_syn_string(const char *text, IdmSpan span);
IdmSyntax *idm_syn_string_n(const char *text, size_t len, IdmSpan span);
IdmSyntax *idm_syn_list(IdmSpan span);
IdmSyntax *idm_syn_vector(IdmSpan span);
IdmSyntax *idm_syn_tuple(IdmSpan span);
IdmSyntax *idm_syn_dict(IdmSpan span);
bool idm_syn_append(IdmSyntax *seq, IdmSyntax *item);
bool idm_syn_prepend_word(IdmSyntax *seq, const char *word);
void idm_syn_set_token(IdmSyntax *syn, const char *raw, bool leading_space, bool adjacent_previous);
bool idm_syn_scope_add(IdmSyntax *syn, int phase, IdmScopeId scope);
bool idm_syn_scope_flip(IdmSyntax *syn, int phase, IdmScopeId scope);
bool idm_syn_scope_contains(const IdmSyntax *syn, int phase, IdmScopeId scope);
const IdmScopeSet *idm_syn_scope_set(const IdmSyntax *syn, int phase);
bool idm_syn_scope_add_tree(IdmSyntax *syn, int phase, IdmScopeId scope);
bool idm_syn_scope_flip_tree(IdmSyntax *syn, int phase, IdmScopeId scope);
bool idm_syn_property_set(IdmSyntax *syn, const char *key, const char *value);
const char *idm_syn_property_get(const IdmSyntax *syn, const char *key);
bool idm_syn_origin_push(IdmSyntax *syn, const char *origin);
bool idm_syn_origin_push_tree(IdmSyntax *syn, const char *origin);
IdmSyntax *idm_syn_program_prepend_implement(const IdmSyntax *program, const char *protocol, const char *file);
IdmSyntax *idm_syn_clone(const IdmSyntax *syn);
void idm_syn_free(IdmSyntax *syn);
bool idm_syn_dump(IdmBuffer *buf, const IdmSyntax *syn);

typedef struct IdmRuntime IdmRuntime;
bool idm_syn_serialize(IdmBuffer *out, const IdmSyntax *syn, IdmError *err);
IdmSyntax *idm_syn_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmError *err);
void idm_syn_scope_relocate_tree(IdmSyntax *syn, IdmScopeId min_id, int64_t delta);
bool idm_syn_scope_visit_tree(const IdmSyntax *syn, bool (*visit)(void *user, IdmScopeId id), void *user);

#endif
