#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stdlib.h>
#include <string.h>

static IdmSyntax *syn_alloc(IdmSyntaxKind kind, IdmSpan span) {
    IdmSyntax *syn = calloc(1u, sizeof(*syn));
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

static IdmScopeSet *phase_scope_find(IdmPhaseScopes *scopes, int phase) {
    for (size_t i = 0; i < scopes->count; i++) {
        if (scopes->items[i].phase == phase) return &scopes->items[i].scopes;
    }
    return NULL;
}

static const IdmScopeSet *phase_scope_find_const(const IdmPhaseScopes *scopes, int phase) {
    for (size_t i = 0; i < scopes->count; i++) {
        if (scopes->items[i].phase == phase) return &scopes->items[i].scopes;
    }
    return NULL;
}

static IdmScopeSet *phase_scope_get_or_add(IdmPhaseScopes *scopes, int phase) {
    IdmScopeSet *existing = phase_scope_find(scopes, phase);
    if (existing) return existing;
    if (scopes->count == scopes->cap) {
        size_t cap = scopes->cap ? scopes->cap * 2u : 2u;
        IdmSyntaxPhaseScope *items = realloc(scopes->items, cap * sizeof(*items));
        if (!items) return NULL;
        scopes->items = items;
        scopes->cap = cap;
    }
    IdmSyntaxPhaseScope *entry = &scopes->items[scopes->count++];
    entry->phase = phase;
    idm_scope_set_init(&entry->scopes);
    return &entry->scopes;
}

static void phase_scopes_destroy(IdmPhaseScopes *scopes) {
    for (size_t i = 0; i < scopes->count; i++) idm_scope_set_destroy(&scopes->items[i].scopes);
    free(scopes->items);
    scopes->items = NULL;
    scopes->count = 0;
    scopes->cap = 0;
}

static void properties_destroy(IdmSyntax *syn) {
    for (size_t i = 0; i < syn->property_count; i++) {
        free(syn->properties[i].key);
        free(syn->properties[i].value);
    }
    free(syn->properties);
    syn->properties = NULL;
    syn->property_count = 0;
    syn->property_cap = 0;
}

static void origins_destroy(IdmOriginChain *origins) {
    for (size_t i = 0; i < origins->count; i++) free(origins->items[i]);
    free(origins->items);
    origins->items = NULL;
    origins->count = 0;
    origins->cap = 0;
}

IdmSyntax *idm_syn_nil(IdmSpan span) {
    return syn_alloc(IDM_SYN_NIL, span);
}

IdmSyntax *idm_syn_word(const char *text, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_WORD, span);
    if (!syn) return NULL;
    syn->as.text = idm_strdup(text);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IdmSyntax *idm_syn_atom(const char *text, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_ATOM, span);
    if (!syn) return NULL;
    syn->as.text = idm_strdup(text);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IdmSyntax *idm_syn_int(int64_t value, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_INT, span);
    if (!syn) return NULL;
    syn->as.integer = value;
    return syn;
}

IdmSyntax *idm_syn_float(double value, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_FLOAT, span);
    if (!syn) return NULL;
    syn->as.real = value;
    return syn;
}

IdmSyntax *idm_syn_string_n(const char *text, size_t len, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_STRING, span);
    if (!syn) return NULL;
    syn->as.text = idm_strndup(text, len);
    if (!syn->as.text) { free(syn); return NULL; }
    return syn;
}

IdmSyntax *idm_syn_string(const char *text, IdmSpan span) {
    return idm_syn_string_n(text, strlen(text), span);
}

IdmSyntax *idm_syn_list(IdmSequenceShape shape, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_LIST, span);
    if (!syn) return NULL;
    syn->as.seq.shape = shape;
    return syn;
}

IdmSyntax *idm_syn_vector(IdmSpan span) {
    return syn_alloc(IDM_SYN_VECTOR, span);
}

IdmSyntax *idm_syn_tuple(IdmSpan span) {
    return syn_alloc(IDM_SYN_TUPLE, span);
}

IdmSyntax *idm_syn_dict(IdmSpan span) {
    return syn_alloc(IDM_SYN_DICT, span);
}

bool idm_syn_append(IdmSyntax *seq, IdmSyntax *item) {
    if (!seq || !item) return false;
    if (seq->kind != IDM_SYN_LIST && seq->kind != IDM_SYN_VECTOR && seq->kind != IDM_SYN_TUPLE && seq->kind != IDM_SYN_DICT) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        size_t cap = seq->as.seq.cap ? seq->as.seq.cap * 2u : 8u;
        IdmSyntax **items = realloc(seq->as.seq.items, cap * sizeof(*items));
        if (!items) return false;
        seq->as.seq.items = items;
        seq->as.seq.cap = cap;
    }
    seq->as.seq.items[seq->as.seq.count++] = item;
    return true;
}

bool idm_syn_prepend_word(IdmSyntax *seq, const char *word) {
    if (!seq || seq->kind != IDM_SYN_LIST) return false;
    IdmSyntax *head = idm_syn_word(word, seq->span);
    if (!head) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        size_t cap = seq->as.seq.cap ? seq->as.seq.cap * 2u : 8u;
        IdmSyntax **items = realloc(seq->as.seq.items, cap * sizeof(*items));
        if (!items) { idm_syn_free(head); return false; }
        seq->as.seq.items = items;
        seq->as.seq.cap = cap;
    }
    memmove(seq->as.seq.items + 1, seq->as.seq.items, seq->as.seq.count * sizeof(*seq->as.seq.items));
    seq->as.seq.items[0] = head;
    seq->as.seq.count++;
    return true;
}

void idm_syn_set_token(IdmSyntax *syn, const char *raw, bool leading_space, bool adjacent_previous) {
    if (!syn) return;
    free(syn->token_raw);
    syn->token_raw = raw ? idm_strdup(raw) : NULL;
    syn->token_leading_space = leading_space;
    syn->token_adjacent_previous = adjacent_previous;
}

bool idm_syn_scope_add(IdmSyntax *syn, int phase, IdmScopeId scope) {
    if (!syn) return false;
    IdmScopeSet *set = phase_scope_get_or_add(&syn->scopes, phase);
    return set && idm_scope_set_add(set, scope);
}

bool idm_syn_scope_flip(IdmSyntax *syn, int phase, IdmScopeId scope) {
    if (!syn) return false;
    IdmScopeSet *set = phase_scope_get_or_add(&syn->scopes, phase);
    return set && idm_scope_set_flip(set, scope);
}

bool idm_syn_scope_contains(const IdmSyntax *syn, int phase, IdmScopeId scope) {
    if (!syn) return false;
    const IdmScopeSet *set = phase_scope_find_const(&syn->scopes, phase);
    return set && idm_scope_set_contains(set, scope);
}

const IdmScopeSet *idm_syn_scope_set(const IdmSyntax *syn, int phase) {
    if (!syn) return NULL;
    return phase_scope_find_const(&syn->scopes, phase);
}

static bool syn_scope_walk(IdmSyntax *syn, int phase, IdmScopeId scope, bool flip) {
    if (!syn) return false;
    if (flip) {
        if (!idm_syn_scope_flip(syn, phase, scope)) return false;
    } else if (!idm_syn_scope_add(syn, phase, scope)) {
        return false;
    }
    switch (syn->kind) {
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!syn_scope_walk(syn->as.seq.items[i], phase, scope, flip)) return false;
            }
            break;
        case IDM_SYN_NIL:
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_INT:
        case IDM_SYN_FLOAT:
        case IDM_SYN_STRING:
            break;
    }
    return true;
}

bool idm_syn_scope_add_tree(IdmSyntax *syn, int phase, IdmScopeId scope) {
    return syn_scope_walk(syn, phase, scope, false);
}

bool idm_syn_scope_flip_tree(IdmSyntax *syn, int phase, IdmScopeId scope) {
    return syn_scope_walk(syn, phase, scope, true);
}

bool idm_syn_property_set(IdmSyntax *syn, const char *key, const char *value) {
    if (!syn || !key || !value) return false;
    for (size_t i = 0; i < syn->property_count; i++) {
        if (strcmp(syn->properties[i].key, key) == 0) {
            char *copy = idm_strdup(value);
            if (!copy) return false;
            free(syn->properties[i].value);
            syn->properties[i].value = copy;
            return true;
        }
    }
    if (syn->property_count == syn->property_cap) {
        size_t cap = syn->property_cap ? syn->property_cap * 2u : 4u;
        IdmSyntaxProperty *props = realloc(syn->properties, cap * sizeof(*props));
        if (!props) return false;
        syn->properties = props;
        syn->property_cap = cap;
    }
    char *key_copy = idm_strdup(key);
    char *value_copy = idm_strdup(value);
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

const char *idm_syn_property_get(const IdmSyntax *syn, const char *key) {
    if (!syn || !key) return NULL;
    for (size_t i = 0; i < syn->property_count; i++) {
        if (strcmp(syn->properties[i].key, key) == 0) return syn->properties[i].value;
    }
    return NULL;
}

bool idm_syn_origin_push(IdmSyntax *syn, const char *origin) {
    if (!syn || !origin) return false;
    if (syn->origins.count == syn->origins.cap) {
        size_t cap = syn->origins.cap ? syn->origins.cap * 2u : 4u;
        char **items = realloc(syn->origins.items, cap * sizeof(*items));
        if (!items) return false;
        syn->origins.items = items;
        syn->origins.cap = cap;
    }
    char *copy = idm_strdup(origin);
    if (!copy) return false;
    syn->origins.items[syn->origins.count++] = copy;
    return true;
}

bool idm_syn_origin_push_tree(IdmSyntax *syn, const char *origin) {
    if (!idm_syn_origin_push(syn, origin)) return false;
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            if (!idm_syn_origin_push_tree(syn->as.seq.items[i], origin)) return false;
        }
    }
    return true;
}

IdmSyntax *idm_syn_clone(const IdmSyntax *syn) {
    if (!syn) return NULL;
    IdmSyntax *clone = NULL;
    switch (syn->kind) {
        case IDM_SYN_NIL:
            clone = idm_syn_nil(syn->span);
            break;
        case IDM_SYN_WORD:
            clone = idm_syn_word(syn->as.text, syn->span);
            break;
        case IDM_SYN_ATOM:
            clone = idm_syn_atom(syn->as.text, syn->span);
            break;
        case IDM_SYN_INT:
            clone = idm_syn_int(syn->as.integer, syn->span);
            break;
        case IDM_SYN_FLOAT:
            clone = idm_syn_float(syn->as.real, syn->span);
            break;
        case IDM_SYN_STRING:
            clone = idm_syn_string(syn->as.text, syn->span);
            break;
        case IDM_SYN_LIST:
            clone = idm_syn_list(syn->as.seq.shape, syn->span);
            break;
        case IDM_SYN_VECTOR:
            clone = idm_syn_vector(syn->span);
            break;
        case IDM_SYN_TUPLE:
            clone = idm_syn_tuple(syn->span);
            break;
        case IDM_SYN_DICT:
            clone = idm_syn_dict(syn->span);
            break;
    }
    if (!clone) return NULL;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        for (size_t j = 0; j < syn->scopes.items[i].scopes.count; j++) {
            if (!idm_syn_scope_add(clone, syn->scopes.items[i].phase, syn->scopes.items[i].scopes.items[j])) {
                idm_syn_free(clone);
                return NULL;
            }
        }
    }
    if (syn->token_raw) idm_syn_set_token(clone, syn->token_raw, syn->token_leading_space, syn->token_adjacent_previous);
    for (size_t i = 0; i < syn->property_count; i++) {
        if (!idm_syn_property_set(clone, syn->properties[i].key, syn->properties[i].value)) {
            idm_syn_free(clone);
            return NULL;
        }
    }
    for (size_t i = 0; i < syn->origins.count; i++) {
        if (!idm_syn_origin_push(clone, syn->origins.items[i])) {
            idm_syn_free(clone);
            return NULL;
        }
    }
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            IdmSyntax *item = idm_syn_clone(syn->as.seq.items[i]);
            if (!item || !idm_syn_append(clone, item)) {
                idm_syn_free(item);
                idm_syn_free(clone);
                return NULL;
            }
        }
    }
    return clone;
}

void idm_syn_free(IdmSyntax *syn) {
    if (!syn) return;
    phase_scopes_destroy(&syn->scopes);
    properties_destroy(syn);
    origins_destroy(&syn->origins);
    free(syn->token_raw);
    switch (syn->kind) {
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            free(syn->as.text);
            break;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) idm_syn_free(syn->as.seq.items[i]);
            free(syn->as.seq.items);
            break;
        case IDM_SYN_NIL:
        case IDM_SYN_INT:
        case IDM_SYN_FLOAT:
            break;
    }
    free(syn);
}

static bool dump_escaped(IdmBuffer *buf, const char *text) {
    if (!idm_buf_append_char(buf, '"')) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '\\': if (!idm_buf_append(buf, "\\\\")) return false; break;
            case '"': if (!idm_buf_append(buf, "\\\"")) return false; break;
            case '\n': if (!idm_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!idm_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!idm_buf_append(buf, "\\t")) return false; break;
            default:
                if (*p < 32u) {
                    if (!idm_buf_appendf(buf, "\\x%02x", *p)) return false;
                } else if (!idm_buf_append_char(buf, (char)*p)) return false;
        }
    }
    return idm_buf_append_char(buf, '"');
}

static bool dump_seq(IdmBuffer *buf, const IdmSyntax *syn, const char *open, const char *close) {
    if (!idm_buf_append(buf, open)) return false;
    for (size_t i = 0; i < syn->as.seq.count; i++) {
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!idm_syn_dump(buf, syn->as.seq.items[i])) return false;
    }
    return idm_buf_append(buf, close);
}

bool idm_syn_dump(IdmBuffer *buf, const IdmSyntax *syn) {
    if (!syn) return idm_buf_append(buf, "#<null-syntax>");
    switch (syn->kind) {
        case IDM_SYN_NIL: return idm_buf_append(buf, ":nil");
        case IDM_SYN_WORD: return idm_buf_append(buf, syn->as.text);
        case IDM_SYN_ATOM: return idm_buf_append_char(buf, ':') && idm_buf_append(buf, syn->as.text);
        case IDM_SYN_INT: return idm_buf_appendf(buf, "%lld", (long long)syn->as.integer);
        case IDM_SYN_FLOAT: return idm_buf_appendf(buf, "%g", syn->as.real);
        case IDM_SYN_STRING: return dump_escaped(buf, syn->as.text);
        case IDM_SYN_LIST:
            return syn->as.seq.shape == IDM_SEQ_BRACKET ? dump_seq(buf, syn, "[", "]") : dump_seq(buf, syn, "(", ")");
        case IDM_SYN_VECTOR: return dump_seq(buf, syn, "%[", "]");
        case IDM_SYN_TUPLE: return dump_seq(buf, syn, "{", "}");
        case IDM_SYN_DICT: return dump_seq(buf, syn, "%{", "}");
    }
    return false;
}

#define IDM_SYN_SER_MAX_DEPTH 1024u

bool idm_syn_serialize(IdmBuffer *out, const IdmSyntax *syn, IdmError *err) {
    if (!syn) return idm_error_set(err, idm_span_unknown(NULL), "cannot serialize null syntax");
    if (!idm_buf_put_u8(out, (uint8_t)syn->kind)) return idm_error_oom(err, syn->span);
    bool ok = idm_buf_put_u8(out, syn->span.file ? 1u : 0u);
    if (ok && syn->span.file) ok = idm_buf_put_str(out, syn->span.file, strlen(syn->span.file));
    ok = ok && idm_buf_put_u64(out, (uint64_t)syn->span.start) && idm_buf_put_u64(out, (uint64_t)syn->span.end);
    ok = ok && idm_buf_put_u32(out, syn->span.line) && idm_buf_put_u32(out, syn->span.column);
    ok = ok && idm_buf_put_u32(out, (uint32_t)syn->scopes.count);
    for (size_t i = 0; ok && i < syn->scopes.count; i++) {
        ok = idm_buf_put_u32(out, (uint32_t)syn->scopes.items[i].phase);
        ok = ok && idm_buf_put_u32(out, (uint32_t)syn->scopes.items[i].scopes.count);
        for (size_t j = 0; ok && j < syn->scopes.items[i].scopes.count; j++) {
            ok = idm_buf_put_u32(out, syn->scopes.items[i].scopes.items[j]);
        }
    }
    ok = ok && idm_buf_put_u8(out, syn->token_raw ? 1u : 0u);
    if (ok && syn->token_raw) {
        ok = idm_buf_put_str(out, syn->token_raw, strlen(syn->token_raw));
        ok = ok && idm_buf_put_u8(out, syn->token_leading_space ? 1u : 0u);
        ok = ok && idm_buf_put_u8(out, syn->token_adjacent_previous ? 1u : 0u);
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)syn->property_count);
    for (size_t i = 0; ok && i < syn->property_count; i++) {
        ok = idm_buf_put_str(out, syn->properties[i].key, strlen(syn->properties[i].key));
        ok = ok && idm_buf_put_str(out, syn->properties[i].value, strlen(syn->properties[i].value));
    }
    ok = ok && idm_buf_put_u32(out, (uint32_t)syn->origins.count);
    for (size_t i = 0; ok && i < syn->origins.count; i++) {
        ok = idm_buf_put_str(out, syn->origins.items[i], strlen(syn->origins.items[i]));
    }
    if (!ok) return idm_error_oom(err, syn->span);
    switch (syn->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            if (!idm_buf_put_str(out, syn->as.text, strlen(syn->as.text))) return idm_error_oom(err, syn->span);
            return true;
        case IDM_SYN_INT:
            if (!idm_buf_put_u64(out, (uint64_t)syn->as.integer)) return idm_error_oom(err, syn->span);
            return true;
        case IDM_SYN_FLOAT: {
            uint64_t bits;
            double d = syn->as.real;
            memcpy(&bits, &d, 8u);
            if (!idm_buf_put_u64(out, bits)) return idm_error_oom(err, syn->span);
            return true;
        }
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if (!idm_buf_put_u8(out, (uint8_t)syn->as.seq.shape)) return idm_error_oom(err, syn->span);
            if (!idm_buf_put_u32(out, (uint32_t)syn->as.seq.count)) return idm_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!idm_syn_serialize(out, syn->as.seq.items[i], err)) return false;
            }
            return true;
    }
    return idm_error_set(err, syn->span, "unknown syntax kind %u cannot be serialized", (unsigned)syn->kind);
}

static IdmSyntax *syn_deserialize_depth(IdmRuntime *rt, IdmByteReader *r, unsigned depth, IdmError *err) {
    if (depth > IDM_SYN_SER_MAX_DEPTH) {
        idm_error_set(err, idm_span_unknown(NULL), "serialized syntax nested too deeply");
        return NULL;
    }
    uint8_t kind = idm_rd_u8(r);
    uint8_t has_file = idm_rd_u8(r);
    if (!r->ok || kind > (uint8_t)IDM_SYN_DICT || has_file > 1u) {
        idm_error_set(err, idm_span_unknown(NULL), "truncated or invalid serialized syntax header");
        return NULL;
    }
    IdmSpan span = idm_span_unknown(NULL);
    if (has_file) {
        char *file = idm_rd_string(r);
        if (!file) {
            idm_error_set(err, idm_span_unknown(NULL), "truncated serialized syntax span file");
            return NULL;
        }
        const IdmSymbol *sym = idm_intern(&rt->intern, IDM_SYMBOL_WORD, file);
        free(file);
        if (!sym) {
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
        span.file = idm_symbol_text(sym);
    }
    span.start = (size_t)idm_rd_u64(r);
    span.end = (size_t)idm_rd_u64(r);
    span.line = idm_rd_u32(r);
    span.column = idm_rd_u32(r);
    uint32_t phase_count = idm_rd_u32(r);
    if (!r->ok) {
        idm_error_set(err, span, "truncated serialized syntax span");
        return NULL;
    }
    IdmSyntax *syn = NULL;
    switch ((IdmSyntaxKind)kind) {
        case IDM_SYN_NIL: syn = idm_syn_nil(span); break;
        default: syn = syn_alloc((IdmSyntaxKind)kind, span); break;
    }
    if (!syn) {
        idm_error_oom(err, span);
        return NULL;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < phase_count; i++) {
        int phase = (int)(int32_t)idm_rd_u32(r);
        uint32_t scope_count = idm_rd_u32(r);
        if (!r->ok) { ok = false; break; }
        for (uint32_t j = 0; ok && j < scope_count; j++) {
            IdmScopeId id = idm_rd_u32(r);
            if (!r->ok || !idm_syn_scope_add(syn, phase, id)) ok = false;
        }
    }
    uint8_t has_token = ok ? idm_rd_u8(r) : 0u;
    if (ok && has_token) {
        char *raw = idm_rd_string(r);
        uint8_t leading = idm_rd_u8(r);
        uint8_t adjacent = idm_rd_u8(r);
        if (!raw || !r->ok) ok = false;
        else idm_syn_set_token(syn, raw, leading != 0u, adjacent != 0u);
        free(raw);
        if (ok && !syn->token_raw) ok = false;
    }
    uint32_t property_count = ok ? idm_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < property_count; i++) {
        char *key = idm_rd_string(r);
        char *value = key ? idm_rd_string(r) : NULL;
        if (!key || !value || !idm_syn_property_set(syn, key, value)) ok = false;
        free(key);
        free(value);
    }
    uint32_t origin_count = ok ? idm_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < origin_count; i++) {
        char *origin = idm_rd_string(r);
        if (!origin || !idm_syn_origin_push(syn, origin)) ok = false;
        free(origin);
    }
    if (!ok) {
        if (!err->present) idm_error_set(err, span, "truncated serialized syntax");
        idm_syn_free(syn);
        return NULL;
    }
    switch ((IdmSyntaxKind)kind) {
        case IDM_SYN_NIL:
            return syn;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING: {
            char *text = idm_rd_string(r);
            if (!text) {
                idm_error_set(err, span, "truncated serialized syntax text");
                idm_syn_free(syn);
                return NULL;
            }
            syn->as.text = text;
            return syn;
        }
        case IDM_SYN_INT:
            syn->as.integer = (int64_t)idm_rd_u64(r);
            break;
        case IDM_SYN_FLOAT: {
            uint64_t bits = idm_rd_u64(r);
            double d;
            memcpy(&d, &bits, 8u);
            syn->as.real = d;
            break;
        }
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT: {
            uint8_t shape = idm_rd_u8(r);
            uint32_t child_count = idm_rd_u32(r);
            if (!r->ok || shape > (uint8_t)IDM_SEQ_BRACKET) {
                idm_error_set(err, span, "truncated serialized syntax sequence");
                idm_syn_free(syn);
                return NULL;
            }
            syn->as.seq.shape = (IdmSequenceShape)shape;
            for (uint32_t i = 0; i < child_count; i++) {
                IdmSyntax *child = syn_deserialize_depth(rt, r, depth + 1u, err);
                if (!child || !idm_syn_append(syn, child)) {
                    if (child && !err->present) idm_error_oom(err, span);
                    idm_syn_free(child);
                    idm_syn_free(syn);
                    return NULL;
                }
            }
            return syn;
        }
    }
    if (!r->ok) {
        idm_error_set(err, span, "truncated serialized syntax payload");
        idm_syn_free(syn);
        return NULL;
    }
    return syn;
}

IdmSyntax *idm_syn_deserialize(IdmRuntime *rt, IdmByteReader *r, IdmError *err) {
    return syn_deserialize_depth(rt, r, 0u, err);
}

void idm_syn_scope_relocate_tree(IdmSyntax *syn, IdmScopeId min_id, int64_t delta) {
    if (!syn || delta == 0) return;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        idm_scope_set_relocate(&syn->scopes.items[i].scopes, min_id, delta);
    }
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            idm_syn_scope_relocate_tree(syn->as.seq.items[i], min_id, delta);
        }
    }
}

bool idm_syn_scope_visit_tree(const IdmSyntax *syn, bool (*visit)(void *user, IdmScopeId id), void *user) {
    if (!syn) return true;
    for (size_t i = 0; i < syn->scopes.count; i++) {
        for (size_t j = 0; j < syn->scopes.items[i].scopes.count; j++) {
            if (!visit(user, syn->scopes.items[i].scopes.items[j])) return false;
        }
    }
    if (syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR || syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT) {
        for (size_t i = 0; i < syn->as.seq.count; i++) {
            if (!idm_syn_scope_visit_tree(syn->as.seq.items[i], visit, user)) return false;
        }
    }
    return true;
}
