#include "ish/syntax.h"

#include <stdlib.h>
#include <string.h>

static IshSyntax *syn_alloc(IshSyntaxKind kind, IshSpan span) {
    IshSyntax *syn = calloc(1u, sizeof(*syn));
    if (!syn) return NULL;
    syn->kind = kind;
    syn->span = span;
    syn->scopes.items = NULL;
    syn->scopes.count = 0;
    syn->scopes.cap = 0;
    return syn;
}

static IshScopeSet *phase_scope_find(IshPhaseScopes *scopes, int phase) {
    for (size_t i = 0; i < scopes->count; i++) {
        if (scopes->items[i].phase == phase) return &scopes->items[i].scopes;
    }
    return NULL;
}

static const IshScopeSet *phase_scope_find_const(const IshPhaseScopes *scopes, int phase) {
    for (size_t i = 0; i < scopes->count; i++) {
        if (scopes->items[i].phase == phase) return &scopes->items[i].scopes;
    }
    return NULL;
}

static IshScopeSet *phase_scope_get_or_add(IshPhaseScopes *scopes, int phase) {
    IshScopeSet *existing = phase_scope_find(scopes, phase);
    if (existing) return existing;
    if (scopes->count == scopes->cap) {
        size_t cap = scopes->cap ? scopes->cap * 2u : 2u;
        IshSyntaxPhaseScope *items = realloc(scopes->items, cap * sizeof(*items));
        if (!items) return NULL;
        scopes->items = items;
        scopes->cap = cap;
    }
    IshSyntaxPhaseScope *entry = &scopes->items[scopes->count++];
    entry->phase = phase;
    ish_scope_set_init(&entry->scopes);
    return &entry->scopes;
}

static void phase_scopes_destroy(IshPhaseScopes *scopes) {
    for (size_t i = 0; i < scopes->count; i++) ish_scope_set_destroy(&scopes->items[i].scopes);
    free(scopes->items);
    scopes->items = NULL;
    scopes->count = 0;
    scopes->cap = 0;
}

IshSyntax *ish_syn_nil(IshSpan span) {
    return syn_alloc(ISH_SYN_NIL, span);
}

IshSyntax *ish_syn_word(const char *text, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_WORD, span);
    if (!syn) return NULL;
    syn->as.text = ish_strdup(text);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IshSyntax *ish_syn_atom(const char *text, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_ATOM, span);
    if (!syn) return NULL;
    syn->as.text = ish_strdup(text);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IshSyntax *ish_syn_int(int64_t value, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_INT, span);
    if (!syn) return NULL;
    syn->as.integer = value;
    return syn;
}

IshSyntax *ish_syn_string_n(const char *text, size_t len, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_STRING, span);
    if (!syn) return NULL;
    syn->as.text = ish_strndup(text, len);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IshSyntax *ish_syn_string(const char *text, IshSpan span) {
    return ish_syn_string_n(text, strlen(text), span);
}

IshSyntax *ish_syn_list(IshSequenceShape shape, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_LIST, span);
    if (!syn) return NULL;
    syn->as.seq.shape = shape;
    return syn;
}

IshSyntax *ish_syn_vector(IshSpan span) {
    return syn_alloc(ISH_SYN_VECTOR, span);
}

IshSyntax *ish_syn_tuple(IshSpan span) {
    return syn_alloc(ISH_SYN_TUPLE, span);
}

IshSyntax *ish_syn_dict(IshSpan span) {
    return syn_alloc(ISH_SYN_DICT, span);
}

bool ish_syn_append(IshSyntax *seq, IshSyntax *item) {
    if (!seq || !item) return false;
    if (seq->kind != ISH_SYN_LIST && seq->kind != ISH_SYN_VECTOR && seq->kind != ISH_SYN_TUPLE && seq->kind != ISH_SYN_DICT) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        size_t cap = seq->as.seq.cap ? seq->as.seq.cap * 2u : 8u;
        IshSyntax **items = realloc(seq->as.seq.items, cap * sizeof(*items));
        if (!items) return false;
        seq->as.seq.items = items;
        seq->as.seq.cap = cap;
    }
    seq->as.seq.items[seq->as.seq.count++] = item;
    return true;
}

bool ish_syn_prepend_word(IshSyntax *seq, const char *word) {
    if (!seq || seq->kind != ISH_SYN_LIST) return false;
    IshSyntax *head = ish_syn_word(word, seq->span);
    if (!head) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        size_t cap = seq->as.seq.cap ? seq->as.seq.cap * 2u : 8u;
        IshSyntax **items = realloc(seq->as.seq.items, cap * sizeof(*items));
        if (!items) { ish_syn_free(head); return false; }
        seq->as.seq.items = items;
        seq->as.seq.cap = cap;
    }
    memmove(seq->as.seq.items + 1, seq->as.seq.items, seq->as.seq.count * sizeof(*seq->as.seq.items));
    seq->as.seq.items[0] = head;
    seq->as.seq.count++;
    return true;
}

void ish_syn_set_token(IshSyntax *syn, const char *raw, bool leading_space, bool adjacent_previous) {
    if (!syn) return;
    free(syn->token_raw);
    syn->token_raw = raw ? ish_strdup(raw) : NULL;
    syn->token_leading_space = leading_space;
    syn->token_adjacent_previous = adjacent_previous;
}

bool ish_syn_scope_add(IshSyntax *syn, int phase, IshScopeId scope) {
    if (!syn) return false;
    IshScopeSet *set = phase_scope_get_or_add(&syn->scopes, phase);
    return set && ish_scope_set_add(set, scope);
}

bool ish_syn_scope_flip(IshSyntax *syn, int phase, IshScopeId scope) {
    if (!syn) return false;
    IshScopeSet *set = phase_scope_get_or_add(&syn->scopes, phase);
    return set && ish_scope_set_flip(set, scope);
}

bool ish_syn_scope_contains(const IshSyntax *syn, int phase, IshScopeId scope) {
    if (!syn) return false;
    const IshScopeSet *set = phase_scope_find_const(&syn->scopes, phase);
    return set && ish_scope_set_contains(set, scope);
}

const IshScopeSet *ish_syn_scope_set(const IshSyntax *syn, int phase) {
    if (!syn) return NULL;
    return phase_scope_find_const(&syn->scopes, phase);
}

static bool syn_scope_walk(IshSyntax *syn, int phase, IshScopeId scope, bool flip) {
    if (!syn) return false;
    if (flip) {
        if (!ish_syn_scope_flip(syn, phase, scope)) return false;
    } else if (!ish_syn_scope_add(syn, phase, scope)) {
        return false;
    }
    switch (syn->kind) {
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!syn_scope_walk(syn->as.seq.items[i], phase, scope, flip)) return false;
            }
            break;
        case ISH_SYN_NIL:
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_INT:
        case ISH_SYN_STRING:
            break;
    }
    return true;
}

bool ish_syn_scope_add_tree(IshSyntax *syn, int phase, IshScopeId scope) {
    return syn_scope_walk(syn, phase, scope, false);
}

bool ish_syn_scope_flip_tree(IshSyntax *syn, int phase, IshScopeId scope) {
    return syn_scope_walk(syn, phase, scope, true);
}

void ish_syn_free(IshSyntax *syn) {
    if (!syn) return;
    phase_scopes_destroy(&syn->scopes);
    free(syn->token_raw);
    switch (syn->kind) {
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_STRING:
            free(syn->as.text);
            break;
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) ish_syn_free(syn->as.seq.items[i]);
            free(syn->as.seq.items);
            break;
        case ISH_SYN_NIL:
        case ISH_SYN_INT:
            break;
    }
    free(syn);
}

static bool dump_escaped(IshBuffer *buf, const char *text) {
    if (!ish_buf_append_char(buf, '"')) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '\\': if (!ish_buf_append(buf, "\\\\")) return false; break;
            case '"': if (!ish_buf_append(buf, "\\\"")) return false; break;
            case '\n': if (!ish_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!ish_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!ish_buf_append(buf, "\\t")) return false; break;
            default:
                if (*p < 32u) {
                    if (!ish_buf_appendf(buf, "\\x%02x", *p)) return false;
                } else if (!ish_buf_append_char(buf, (char)*p)) return false;
        }
    }
    return ish_buf_append_char(buf, '"');
}

static bool dump_seq(IshBuffer *buf, const IshSyntax *syn, const char *open, const char *close) {
    if (!ish_buf_append(buf, open)) return false;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        if (i != 0 && !ish_buf_append_char(buf, ' ')) return false;
        if (!ish_syn_dump(buf, syn->as.seq.items[i])) return false;
    }
    return ish_buf_append(buf, close);
}

bool ish_syn_dump(IshBuffer *buf, const IshSyntax *syn) {
    if (!syn) return ish_buf_append(buf, "#<null-syntax>");
    switch (syn->kind) {
        case ISH_SYN_NIL: return ish_buf_append(buf, ":nil");
        case ISH_SYN_WORD: return ish_buf_append(buf, syn->as.text);
        case ISH_SYN_ATOM: return ish_buf_append_char(buf, ':') && ish_buf_append(buf, syn->as.text);
        case ISH_SYN_INT: return ish_buf_appendf(buf, "%lld", (long long)syn->as.integer);
        case ISH_SYN_STRING: return dump_escaped(buf, syn->as.text);
        case ISH_SYN_LIST:
            return syn->as.seq.shape == ISH_SEQ_BRACKET ? dump_seq(buf, syn, "[", "]") : dump_seq(buf, syn, "(", ")");
        case ISH_SYN_VECTOR: return dump_seq(buf, syn, "%[", "]");
        case ISH_SYN_TUPLE: return dump_seq(buf, syn, "{", "}");
        case ISH_SYN_DICT: return dump_seq(buf, syn, "%{", "}");
    }
    return false;
}
