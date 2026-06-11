#include "ish/syntax.h"
#include "ish/value.h"

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
    syn->properties = NULL;
    syn->property_count = 0;
    syn->property_cap = 0;
    syn->origins.items = NULL;
    syn->origins.count = 0;
    syn->origins.cap = 0;
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

static void properties_destroy(IshSyntax *syn) {
    for (size_t i = 0; i < syn->property_count; i++) {
        free(syn->properties[i].key);
        free(syn->properties[i].value);
    }
    free(syn->properties);
    syn->properties = NULL;
    syn->property_count = 0;
    syn->property_cap = 0;
}

static void origins_destroy(IshOriginChain *origins) {
    for (size_t i = 0; i < origins->count; i++) free(origins->items[i]);
    free(origins->items);
    origins->items = NULL;
    origins->count = 0;
    origins->cap = 0;
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

IshSyntax *ish_syn_float(double value, IshSpan span) {
    IshSyntax *syn = syn_alloc(ISH_SYN_FLOAT, span);
    if (!syn) return NULL;
    syn->as.real = value;
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
        case ISH_SYN_FLOAT:
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

bool ish_syn_property_set(IshSyntax *syn, const char *key, const char *value) {
    if (!syn || !key || !value) return false;
    for (size_t i = 0; i < syn->property_count; i++) {
        if (strcmp(syn->properties[i].key, key) == 0) {
            char *copy = ish_strdup(value);
            if (!copy) return false;
            free(syn->properties[i].value);
            syn->properties[i].value = copy;
            return true;
        }
    }
    if (syn->property_count == syn->property_cap) {
        size_t cap = syn->property_cap ? syn->property_cap * 2u : 4u;
        IshSyntaxProperty *props = realloc(syn->properties, cap * sizeof(*props));
        if (!props) return false;
        syn->properties = props;
        syn->property_cap = cap;
    }
    char *key_copy = ish_strdup(key);
    char *value_copy = ish_strdup(value);
    if (!key_copy || !value_copy) {
        free(key_copy);
        free(value_copy);
        return false;
    }
    syn->properties[syn->property_count].key = key_copy;
    syn->properties[syn->property_count].value = value_copy;
    syn->property_count++;
    return true;
}

const char *ish_syn_property_get(const IshSyntax *syn, const char *key) {
    if (!syn || !key) return NULL;
    for (size_t i = 0; i < syn->property_count; i++) {
        if (strcmp(syn->properties[i].key, key) == 0) return syn->properties[i].value;
    }
    return NULL;
}

bool ish_syn_origin_push(IshSyntax *syn, const char *origin) {
    if (!syn || !origin) return false;
    if (syn->origins.count == syn->origins.cap) {
        size_t cap = syn->origins.cap ? syn->origins.cap * 2u : 4u;
        char **items = realloc(syn->origins.items, cap * sizeof(*items));
        if (!items) return false;
        syn->origins.items = items;
        syn->origins.cap = cap;
    }
    char *copy = ish_strdup(origin);
    if (!copy) return false;
    syn->origins.items[syn->origins.count++] = copy;
    return true;
}

bool ish_syn_origin_push_tree(IshSyntax *syn, const char *origin) {
    if (!ish_syn_origin_push(syn, origin)) return false;
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE || syn->kind == ISH_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            if (!ish_syn_origin_push_tree(syn->as.seq.items[i], origin)) return false;
        }
    }
    return true;
}

IshSyntax *ish_syn_clone(const IshSyntax *syn) {
    if (!syn) return NULL;
    IshSyntax *clone = NULL;
    switch (syn->kind) {
        case ISH_SYN_NIL:
            clone = ish_syn_nil(syn->span);
            break;
        case ISH_SYN_WORD:
            clone = ish_syn_word(syn->as.text, syn->span);
            break;
        case ISH_SYN_ATOM:
            clone = ish_syn_atom(syn->as.text, syn->span);
            break;
        case ISH_SYN_INT:
            clone = ish_syn_int(syn->as.integer, syn->span);
            break;
        case ISH_SYN_FLOAT:
            clone = ish_syn_float(syn->as.real, syn->span);
            break;
        case ISH_SYN_STRING:
            clone = ish_syn_string(syn->as.text, syn->span);
            break;
        case ISH_SYN_LIST:
            clone = ish_syn_list(syn->as.seq.shape, syn->span);
            break;
        case ISH_SYN_VECTOR:
            clone = ish_syn_vector(syn->span);
            break;
        case ISH_SYN_TUPLE:
            clone = ish_syn_tuple(syn->span);
            break;
        case ISH_SYN_DICT:
            clone = ish_syn_dict(syn->span);
            break;
    }
    if (!clone) return NULL;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        for (size_t j = 0; j < syn->scopes.items[i].scopes.count; j++) {
            if (!ish_syn_scope_add(clone, syn->scopes.items[i].phase, syn->scopes.items[i].scopes.items[j])) {
                ish_syn_free(clone);
                return NULL;
            }
        }
    }
    if (syn->token_raw) ish_syn_set_token(clone, syn->token_raw, syn->token_leading_space, syn->token_adjacent_previous);
    for (size_t i = 0; i < syn->property_count; i++) {
        if (!ish_syn_property_set(clone, syn->properties[i].key, syn->properties[i].value)) {
            ish_syn_free(clone);
            return NULL;
        }
    }
    for (size_t i = 0; i < syn->origins.count; i++) {
        if (!ish_syn_origin_push(clone, syn->origins.items[i])) {
            ish_syn_free(clone);
            return NULL;
        }
    }
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE || syn->kind == ISH_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            IshSyntax *item = ish_syn_clone(syn->as.seq.items[i]);
            if (!item || !ish_syn_append(clone, item)) {
                ish_syn_free(item);
                ish_syn_free(clone);
                return NULL;
            }
        }
    }
    return clone;
}

void ish_syn_free(IshSyntax *syn) {
    if (!syn) return;
    phase_scopes_destroy(&syn->scopes);
    properties_destroy(syn);
    origins_destroy(&syn->origins);
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
        case ISH_SYN_FLOAT:
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
        case ISH_SYN_FLOAT: return ish_buf_appendf(buf, "%g", syn->as.real);
        case ISH_SYN_STRING: return dump_escaped(buf, syn->as.text);
        case ISH_SYN_LIST:
            return syn->as.seq.shape == ISH_SEQ_BRACKET ? dump_seq(buf, syn, "[", "]") : dump_seq(buf, syn, "(", ")");
        case ISH_SYN_VECTOR: return dump_seq(buf, syn, "%[", "]");
        case ISH_SYN_TUPLE: return dump_seq(buf, syn, "{", "}");
        case ISH_SYN_DICT: return dump_seq(buf, syn, "%{", "}");
    }
    return false;
}

#define ISH_SYN_SER_MAX_DEPTH 1024u

bool ish_syn_serialize(IshBuffer *out, const IshSyntax *syn, IshError *err) {
    if (!syn) return ish_error_set(err, ish_span_unknown(NULL), "cannot serialize null syntax");
    if (!ish_buf_put_u8(out, (uint8_t)syn->kind)) return ish_error_oom(err, syn->span);
    bool ok = ish_buf_put_u8(out, syn->span.file ? 1u : 0u);
    if (ok && syn->span.file) ok = ish_buf_put_str(out, syn->span.file, strlen(syn->span.file));
    ok = ok && ish_buf_put_u64(out, (uint64_t)syn->span.start) && ish_buf_put_u64(out, (uint64_t)syn->span.end);
    ok = ok && ish_buf_put_u32(out, syn->span.line) && ish_buf_put_u32(out, syn->span.column);
    ok = ok && ish_buf_put_u32(out, (uint32_t)syn->scopes.count);
    for (size_t i = 0; ok && i < syn->scopes.count; i++) {
        ok = ish_buf_put_u32(out, (uint32_t)syn->scopes.items[i].phase);
        ok = ok && ish_buf_put_u32(out, (uint32_t)syn->scopes.items[i].scopes.count);
        for (size_t j = 0; ok && j < syn->scopes.items[i].scopes.count; j++) {
            ok = ish_buf_put_u32(out, syn->scopes.items[i].scopes.items[j]);
        }
    }
    ok = ok && ish_buf_put_u8(out, syn->token_raw ? 1u : 0u);
    if (ok && syn->token_raw) {
        ok = ish_buf_put_str(out, syn->token_raw, strlen(syn->token_raw));
        ok = ok && ish_buf_put_u8(out, syn->token_leading_space ? 1u : 0u);
        ok = ok && ish_buf_put_u8(out, syn->token_adjacent_previous ? 1u : 0u);
    }
    ok = ok && ish_buf_put_u32(out, (uint32_t)syn->property_count);
    for (size_t i = 0; ok && i < syn->property_count; i++) {
        ok = ish_buf_put_str(out, syn->properties[i].key, strlen(syn->properties[i].key));
        ok = ok && ish_buf_put_str(out, syn->properties[i].value, strlen(syn->properties[i].value));
    }
    ok = ok && ish_buf_put_u32(out, (uint32_t)syn->origins.count);
    for (size_t i = 0; ok && i < syn->origins.count; i++) {
        ok = ish_buf_put_str(out, syn->origins.items[i], strlen(syn->origins.items[i]));
    }
    if (!ok) return ish_error_oom(err, syn->span);
    switch (syn->kind) {
        case ISH_SYN_NIL:
            return true;
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_STRING:
            if (!ish_buf_put_str(out, syn->as.text, strlen(syn->as.text))) return ish_error_oom(err, syn->span);
            return true;
        case ISH_SYN_INT:
            if (!ish_buf_put_u64(out, (uint64_t)syn->as.integer)) return ish_error_oom(err, syn->span);
            return true;
        case ISH_SYN_FLOAT: {
            uint64_t bits;
            double d = syn->as.real;
            memcpy(&bits, &d, 8u);
            if (!ish_buf_put_u64(out, bits)) return ish_error_oom(err, syn->span);
            return true;
        }
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            if (!ish_buf_put_u8(out, (uint8_t)syn->as.seq.shape)) return ish_error_oom(err, syn->span);
            if (!ish_buf_put_u32(out, (uint32_t)syn->as.seq.count)) return ish_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!ish_syn_serialize(out, syn->as.seq.items[i], err)) return false;
            }
            return true;
    }
    return ish_error_set(err, syn->span, "unknown syntax kind %u cannot be serialized", (unsigned)syn->kind);
}

static IshSyntax *syn_deserialize_depth(IshRuntime *rt, IshByteReader *r, unsigned depth, IshError *err) {
    if (depth > ISH_SYN_SER_MAX_DEPTH) {
        ish_error_set(err, ish_span_unknown(NULL), "serialized syntax nested too deeply");
        return NULL;
    }
    uint8_t kind = ish_rd_u8(r);
    uint8_t has_file = ish_rd_u8(r);
    if (!r->ok || kind > (uint8_t)ISH_SYN_DICT || has_file > 1u) {
        ish_error_set(err, ish_span_unknown(NULL), "truncated or invalid serialized syntax header");
        return NULL;
    }
    IshSpan span = ish_span_unknown(NULL);
    if (has_file) {
        char *file = ish_rd_string(r);
        if (!file) {
            ish_error_set(err, ish_span_unknown(NULL), "truncated serialized syntax span file");
            return NULL;
        }
        const IshSymbol *sym = ish_intern(&rt->intern, ISH_SYMBOL_WORD, file);
        free(file);
        if (!sym) {
            ish_error_oom(err, ish_span_unknown(NULL));
            return NULL;
        }
        span.file = ish_symbol_text(sym);
    }
    span.start = (size_t)ish_rd_u64(r);
    span.end = (size_t)ish_rd_u64(r);
    span.line = ish_rd_u32(r);
    span.column = ish_rd_u32(r);
    uint32_t phase_count = ish_rd_u32(r);
    if (!r->ok) {
        ish_error_set(err, span, "truncated serialized syntax span");
        return NULL;
    }
    IshSyntax *syn = NULL;
    switch ((IshSyntaxKind)kind) {
        case ISH_SYN_NIL: syn = ish_syn_nil(span); break;
        default: syn = syn_alloc((IshSyntaxKind)kind, span); break;
    }
    if (!syn) {
        ish_error_oom(err, span);
        return NULL;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < phase_count; i++) {
        int phase = (int)(int32_t)ish_rd_u32(r);
        uint32_t scope_count = ish_rd_u32(r);
        if (!r->ok) { ok = false; break; }
        for (uint32_t j = 0; ok && j < scope_count; j++) {
            IshScopeId id = ish_rd_u32(r);
            if (!r->ok || !ish_syn_scope_add(syn, phase, id)) ok = false;
        }
    }
    uint8_t has_token = ok ? ish_rd_u8(r) : 0u;
    if (ok && has_token) {
        char *raw = ish_rd_string(r);
        uint8_t leading = ish_rd_u8(r);
        uint8_t adjacent = ish_rd_u8(r);
        if (!raw || !r->ok) ok = false;
        else ish_syn_set_token(syn, raw, leading != 0u, adjacent != 0u);
        free(raw);
        if (ok && !syn->token_raw) ok = false;
    }
    uint32_t property_count = ok ? ish_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < property_count; i++) {
        char *key = ish_rd_string(r);
        char *value = key ? ish_rd_string(r) : NULL;
        if (!key || !value || !ish_syn_property_set(syn, key, value)) ok = false;
        free(key);
        free(value);
    }
    uint32_t origin_count = ok ? ish_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < origin_count; i++) {
        char *origin = ish_rd_string(r);
        if (!origin || !ish_syn_origin_push(syn, origin)) ok = false;
        free(origin);
    }
    if (!ok) {
        if (!err->present) ish_error_set(err, span, "truncated serialized syntax");
        ish_syn_free(syn);
        return NULL;
    }
    switch ((IshSyntaxKind)kind) {
        case ISH_SYN_NIL:
            return syn;
        case ISH_SYN_WORD:
        case ISH_SYN_ATOM:
        case ISH_SYN_STRING: {
            char *text = ish_rd_string(r);
            if (!text) {
                ish_error_set(err, span, "truncated serialized syntax text");
                ish_syn_free(syn);
                return NULL;
            }
            syn->as.text = text;
            return syn;
        }
        case ISH_SYN_INT:
            syn->as.integer = (int64_t)ish_rd_u64(r);
            break;
        case ISH_SYN_FLOAT: {
            uint64_t bits = ish_rd_u64(r);
            double d;
            memcpy(&d, &bits, 8u);
            syn->as.real = d;
            break;
        }
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT: {
            uint8_t shape = ish_rd_u8(r);
            uint32_t child_count = ish_rd_u32(r);
            if (!r->ok || shape > (uint8_t)ISH_SEQ_BRACKET) {
                ish_error_set(err, span, "truncated serialized syntax sequence");
                ish_syn_free(syn);
                return NULL;
            }
            syn->as.seq.shape = (IshSequenceShape)shape;
            for (uint32_t i = 0; i < child_count; i++) {
                IshSyntax *child = syn_deserialize_depth(rt, r, depth + 1u, err);
                if (!child || !ish_syn_append(syn, child)) {
                    if (child && !err->present) ish_error_oom(err, span);
                    ish_syn_free(child);
                    ish_syn_free(syn);
                    return NULL;
                }
            }
            return syn;
        }
    }
    if (!r->ok) {
        ish_error_set(err, span, "truncated serialized syntax payload");
        ish_syn_free(syn);
        return NULL;
    }
    return syn;
}

IshSyntax *ish_syn_deserialize(IshRuntime *rt, IshByteReader *r, IshError *err) {
    return syn_deserialize_depth(rt, r, 0u, err);
}

void ish_syn_scope_relocate_tree(IshSyntax *syn, IshScopeId min_id, int64_t delta) {
    if (!syn || delta == 0) return;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        ish_scope_set_relocate(&syn->scopes.items[i].scopes, min_id, delta);
    }
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE || syn->kind == ISH_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            ish_syn_scope_relocate_tree(syn->as.seq.items[i], min_id, delta);
        }
    }
}

bool ish_syn_scope_visit_tree(const IshSyntax *syn, bool (*visit)(void *user, IshScopeId id), void *user) {
    if (!syn) return true;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        for (size_t j = 0; j < syn->scopes.items[i].scopes.count; j++) {
            if (!visit(user, syn->scopes.items[i].scopes.items[j])) return false;
        }
    }
    if (syn->kind == ISH_SYN_LIST || syn->kind == ISH_SYN_VECTOR || syn->kind == ISH_SYN_TUPLE || syn->kind == ISH_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            if (!ish_syn_scope_visit_tree(syn->as.seq.items[i], visit, user)) return false;
        }
    }
    return true;
}
