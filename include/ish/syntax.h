#ifndef ISH_SYNTAX_H
#define ISH_SYNTAX_H

#include "ish/common.h"
#include "ish/scope.h"

typedef struct IshSyntax IshSyntax;

typedef enum {
    ISH_SYN_NIL,
    ISH_SYN_WORD,
    ISH_SYN_ATOM,
    ISH_SYN_INT,
    ISH_SYN_STRING,
    ISH_SYN_LIST,
    ISH_SYN_VECTOR,
    ISH_SYN_TUPLE,
    ISH_SYN_DICT
} IshSyntaxKind;

typedef enum {
    ISH_SEQ_PAREN,
    ISH_SEQ_BRACKET
} IshSequenceShape;

typedef struct {
    int phase;
    IshScopeSet scopes;
} IshSyntaxPhaseScope;

typedef struct {
    IshSyntaxPhaseScope *items;
    size_t count;
    size_t cap;
} IshPhaseScopes;

struct IshSyntax {
    IshSyntaxKind kind;
    IshSpan span;
    IshPhaseScopes scopes;
    char *token_raw;
    bool token_leading_space;
    bool token_adjacent_previous;
    union {
        char *text;
        int64_t integer;
        struct {
            IshSyntax **items;
            size_t count;
            size_t cap;
            IshSequenceShape shape;
        } seq;
    } as;
};

IshSyntax *ish_syn_nil(IshSpan span);
IshSyntax *ish_syn_word(const char *text, IshSpan span);
IshSyntax *ish_syn_atom(const char *text, IshSpan span);
IshSyntax *ish_syn_int(int64_t value, IshSpan span);
IshSyntax *ish_syn_string(const char *text, IshSpan span);
IshSyntax *ish_syn_string_n(const char *text, size_t len, IshSpan span);
IshSyntax *ish_syn_list(IshSequenceShape shape, IshSpan span);
IshSyntax *ish_syn_vector(IshSpan span);
IshSyntax *ish_syn_tuple(IshSpan span);
IshSyntax *ish_syn_dict(IshSpan span);
bool ish_syn_append(IshSyntax *seq, IshSyntax *item);
bool ish_syn_prepend_word(IshSyntax *seq, const char *word);
void ish_syn_set_token(IshSyntax *syn, const char *raw, bool leading_space, bool adjacent_previous);
bool ish_syn_scope_add(IshSyntax *syn, int phase, IshScopeId scope);
bool ish_syn_scope_flip(IshSyntax *syn, int phase, IshScopeId scope);
bool ish_syn_scope_contains(const IshSyntax *syn, int phase, IshScopeId scope);
const IshScopeSet *ish_syn_scope_set(const IshSyntax *syn, int phase);
bool ish_syn_scope_add_tree(IshSyntax *syn, int phase, IshScopeId scope);
bool ish_syn_scope_flip_tree(IshSyntax *syn, int phase, IshScopeId scope);
void ish_syn_free(IshSyntax *syn);
bool ish_syn_dump(IshBuffer *buf, const IshSyntax *syn);

#endif
