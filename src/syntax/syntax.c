#include "idiom/syntax.h"
#include "idiom/bignum.h"
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
        if (!idm_grow((void **)&scopes->items, &scopes->cap, sizeof(*scopes->items), 2u, scopes->count + 1u)) return NULL;
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
    syn->form_id = idm_syn_form_id_lookup(text);
    return syn;
}
static const char *const syn_form_names[] = {
    "expr",
    "body",
    "word",
    "group",
    "layout-group",
    "match",
    "try",
    "receive",
    "implements",
    "protocol-info",
    "string",
    "regex",
    "bitstring",
    "bitseg",
    "bitseg-bare",
    "bitrest",
    "dict-entry",
    "doc",
    "expression",
    "package-begin",
    "pin",
    "quasiquote",
    "quasisyntax",
    "quote",
    "syntax",
    "unquote",
    "unquote-splicing",
    "unsyntax",
    "unsyntax-splicing",
    "adjacent",
};

uint8_t idm_syn_form_id_lookup(const char *text) {
    if (!text || text[0] != '%' || text[1] != '-') return 0;
    for (size_t i = 0; i < sizeof(syn_form_names) / sizeof(syn_form_names[0]); i++) {
        if (strcmp(text + 2, syn_form_names[i]) == 0) return (uint8_t)(i + 1u);
    }
    return 0;
}

bool idm_syn_is_form_id(const IdmSyntax *syn, uint8_t form_id) {
    return syn && syn->kind == IDM_SYN_LIST && syn->as.seq.count > 0 &&
           syn->as.seq.items[0]->kind == IDM_SYN_WORD && syn->as.seq.items[0]->form_id == form_id;
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

IdmSyntax *idm_syn_bigint(const char *text, IdmSpan span) {
    IdmSyntax *syn = syn_alloc(IDM_SYN_BIGINT, span);
    if (!syn) return NULL;
    syn->as.text = idm_strdup(text);
    if (!syn->as.text) { free(syn); return NULL; }
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

IdmSyntax *idm_syn_seq(IdmSyntaxKind kind, IdmSpan span) {
    if (kind != IDM_SYN_LIST && kind != IDM_SYN_VECTOR && kind != IDM_SYN_TUPLE && kind != IDM_SYN_DICT) return NULL;
    return syn_alloc(kind, span);
}

IdmSyntax *idm_syn_list(IdmSpan span) {
    return idm_syn_seq(IDM_SYN_LIST, span);
}

IdmSyntax *idm_syn_vector(IdmSpan span) {
    return idm_syn_seq(IDM_SYN_VECTOR, span);
}

IdmSyntax *idm_syn_tuple(IdmSpan span) {
    return idm_syn_seq(IDM_SYN_TUPLE, span);
}

IdmSyntax *idm_syn_dict(IdmSpan span) {
    return idm_syn_seq(IDM_SYN_DICT, span);
}

const char *idm_syn_kind_name(IdmSyntaxKind kind) {
    static const IdmDatumKind datum_kinds[] = {
        [IDM_SYN_NIL] = IDM_DATUM_NIL,
        [IDM_SYN_WORD] = IDM_DATUM_WORD,
        [IDM_SYN_ATOM] = IDM_DATUM_ATOM,
        [IDM_SYN_INT] = IDM_DATUM_INT,
        [IDM_SYN_FLOAT] = IDM_DATUM_FLOAT,
        [IDM_SYN_STRING] = IDM_DATUM_STRING,
        [IDM_SYN_LIST] = IDM_DATUM_LIST,
        [IDM_SYN_VECTOR] = IDM_DATUM_VECTOR,
        [IDM_SYN_TUPLE] = IDM_DATUM_TUPLE,
        [IDM_SYN_DICT] = IDM_DATUM_DICT,
        [IDM_SYN_BIGINT] = IDM_DATUM_INT,
    };
    return (size_t)kind < sizeof(datum_kinds) / sizeof(datum_kinds[0]) ? idm_datum_kind_name(datum_kinds[kind]) : "unknown";
}

bool idm_syn_append(IdmSyntax *seq, IdmSyntax *item) {
    if (!seq || !item) return false;
    if (seq->kind != IDM_SYN_LIST && seq->kind != IDM_SYN_VECTOR && seq->kind != IDM_SYN_TUPLE && seq->kind != IDM_SYN_DICT) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        if (!idm_grow((void **)&seq->as.seq.items, &seq->as.seq.cap, sizeof(*seq->as.seq.items), 8u, seq->as.seq.count + 1u)) return false;
    }
    seq->as.seq.items[seq->as.seq.count++] = item;
    return true;
}

bool idm_syn_prepend_word(IdmSyntax *seq, const char *word) {
    if (!seq || seq->kind != IDM_SYN_LIST) return false;
    IdmSyntax *head = idm_syn_word(word, seq->span);
    if (!head) return false;
    if (seq->as.seq.count == seq->as.seq.cap) {
        if (!idm_grow((void **)&seq->as.seq.items, &seq->as.seq.cap, sizeof(*seq->as.seq.items), 8u, seq->as.seq.count + 1u)) { idm_syn_free(head); return false; }
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
        case IDM_SYN_BIGINT:
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
        if (!idm_grow((void **)&syn->properties, &syn->property_cap, sizeof(*syn->properties), 4u, syn->property_count + 1u)) return false;
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
        if (!idm_grow((void **)&syn->origins.items, &syn->origins.cap, sizeof(*syn->origins.items), 4u, syn->origins.count + 1u)) return false;
    }
    char *copy = idm_strdup(origin);
    if (!copy) return false;
    syn->origins.items[syn->origins.count++] = copy;
    return true;
}

static IdmSyntax *syn_clone_at(const IdmSyntax *syn, unsigned depth) {
    if (!syn) return NULL;
    if (depth > IDM_IC_MAX_DEPTH) return NULL;
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
        case IDM_SYN_BIGINT:
            clone = idm_syn_bigint(syn->as.text, syn->span);
            break;
        case IDM_SYN_FLOAT:
            clone = idm_syn_float(syn->as.real, syn->span);
            break;
        case IDM_SYN_STRING:
            clone = idm_syn_string(syn->as.text, syn->span);
            break;
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            clone = idm_syn_seq(syn->kind, syn->span);
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
            IdmSyntax *item = syn_clone_at(syn->as.seq.items[i], depth + 1u);
            if (!item || !idm_syn_append(clone, item)) {
                idm_syn_free(item);
                idm_syn_free(clone);
                return NULL;
            }
        }
    }
    return clone;
}

IdmSyntax *idm_syn_clone(const IdmSyntax *syn) {
    return syn_clone_at(syn, 0);
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
        case IDM_SYN_BIGINT:
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

bool idm_syn_equal(const IdmSyntax *a, const IdmSyntax *b) {
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case IDM_SYN_NIL:
            return true;
        case IDM_SYN_WORD:
        case IDM_SYN_ATOM:
        case IDM_SYN_STRING:
            return strcmp(a->as.text, b->as.text) == 0;
        case IDM_SYN_BIGINT: {
            bool eq = false;
            if (idm_bignum_decimal_equal(a->as.text, strlen(a->as.text), b->as.text, strlen(b->as.text), &eq)) return eq;
            return strcmp(a->as.text, b->as.text) == 0;
        }
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
                if (!idm_syn_equal(a->as.seq.items[i], b->as.seq.items[i])) return false;
            }
            return true;
    }
    return false;
}

static bool dump_seq_item(IdmBuffer *buf, size_t index, void *user) {
    const IdmSyntax *syn = user;
    return idm_syn_dump(buf, syn->as.seq.items[index]);
}

static bool dump_seq(IdmBuffer *buf, const IdmSyntax *syn, const char *open, const char *close) {
    return idm_surface_write_sequence(buf, open, close, syn->as.seq.count, dump_seq_item, (void *)syn);
}

bool idm_syn_dump(IdmBuffer *buf, const IdmSyntax *syn) {
    if (!syn) return idm_buf_append(buf, "#<null-syntax>");
    switch (syn->kind) {
        case IDM_SYN_NIL: return idm_buf_append(buf, ":nil");
        case IDM_SYN_WORD: return idm_buf_append(buf, syn->as.text);
        case IDM_SYN_ATOM: return idm_buf_append_char(buf, ':') && idm_buf_append(buf, syn->as.text);
        case IDM_SYN_INT: return idm_buf_appendf(buf, "%lld", (long long)syn->as.integer);
        case IDM_SYN_BIGINT: return idm_buf_append(buf, syn->as.text);
        case IDM_SYN_FLOAT: return idm_buf_appendf(buf, "%g", syn->as.real);
        case IDM_SYN_STRING: return idm_surface_write_escaped(buf, syn->as.text, strlen(syn->as.text));
        case IDM_SYN_LIST: return dump_seq(buf, syn, "(", ")");
        case IDM_SYN_VECTOR: return dump_seq(buf, syn, "[", "]");
        case IDM_SYN_TUPLE: return dump_seq(buf, syn, "{", "}");
        case IDM_SYN_DICT: return dump_seq(buf, syn, "%{", "}");
    }
    return false;
}

#define IDM_SYN_PRETTY_WIDTH 80

static bool syn_pretty_newline(IdmBuffer *buf, size_t indent) {
    if (!idm_buf_append_char(buf, '\n')) return false;
    for (size_t i = 0; i < indent; i++) {
        if (!idm_buf_append_char(buf, ' ')) return false;
    }
    return true;
}

static bool syn_is_compound(const IdmSyntax *syn) {
    return syn->kind == IDM_SYN_LIST || syn->kind == IDM_SYN_VECTOR ||
           syn->kind == IDM_SYN_TUPLE || syn->kind == IDM_SYN_DICT;
}

static bool syn_is_statement_seq(const IdmSyntax *syn) {
    return idm_syn_is_form_id(syn, IDM_FORM_PACKAGE_BEGIN) || idm_syn_is_form_id(syn, IDM_FORM_BODY);
}

typedef struct SynMeasure {
    size_t width;
    bool stmt_seq;
    struct SynMeasure **items;
    size_t count;
} SynMeasure;

static void syn_measure_free(SynMeasure *m) {
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) syn_measure_free(m->items[i]);
    free(m->items);
    free(m);
}

static SynMeasure *syn_measure(const IdmSyntax *syn) {
    SynMeasure *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (!syn || !syn_is_compound(syn)) {
        IdmBuffer tmp;
        idm_buf_init(&tmp);
        if (!idm_syn_dump(&tmp, syn)) {
            idm_buf_destroy(&tmp);
            free(m);
            return NULL;
        }
        m->width = tmp.len;
        idm_buf_destroy(&tmp);
        return m;
    }
    m->count = syn->as.seq.count;
    if (m->count != 0) {
        m->items = calloc(m->count, sizeof(*m->items));
        if (!m->items) {
            free(m);
            return NULL;
        }
    }
    m->stmt_seq = syn_is_statement_seq(syn);
    size_t width = syn->kind == IDM_SYN_DICT ? 3u : 2u;
    for (size_t i = 0; i < m->count; i++) {
        m->items[i] = syn_measure(syn->as.seq.items[i]);
        if (!m->items[i]) {
            syn_measure_free(m);
            return NULL;
        }
        width += m->items[i]->width + (i != 0 ? 1u : 0u);
        if (m->items[i]->stmt_seq) m->stmt_seq = true;
    }
    m->width = width;
    return m;
}

static size_t syn_cur_col(const IdmBuffer *buf) {
    size_t i = buf->len;
    while (i > 0 && buf->data[i - 1u] != '\n') i--;
    return buf->len - i;
}

static bool syn_pretty(IdmBuffer *buf, const IdmSyntax *syn, const SynMeasure *m, size_t indent) {
    if (!syn || !syn_is_compound(syn)) return idm_syn_dump(buf, syn);

    bool multiline = m->stmt_seq || indent + m->width > IDM_SYN_PRETTY_WIDTH;
    if (!multiline) return idm_syn_dump(buf, syn);

    const char *open = "(";
    const char *close = ")";
    if (syn->kind == IDM_SYN_VECTOR) { open = "["; close = "]"; }
    else if (syn->kind == IDM_SYN_TUPLE) { open = "{"; close = "}"; }
    else if (syn->kind == IDM_SYN_DICT) { open = "%{"; close = "}"; }

    if (!idm_buf_append(buf, open)) return false;

    if (syn_is_statement_seq(syn)) {
        if (!idm_syn_dump(buf, syn->as.seq.items[0])) return false;
        for (size_t i = 1; i < syn->as.seq.count; i++) {
            if (!syn_pretty_newline(buf, indent + 2u) || !syn_pretty(buf, syn->as.seq.items[i], m->items[i], indent + 2u)) return false;
        }
        return idm_buf_append(buf, close);
    }

    for (size_t i = 0; i < syn->as.seq.count; i++) {
        const IdmSyntax *child = syn->as.seq.items[i];
        if (i != 0 && syn_is_compound(child) && !m->items[i]->stmt_seq) {
            if (syn_cur_col(buf) + 1u + m->items[i]->width > IDM_SYN_PRETTY_WIDTH) {
                if (!syn_pretty_newline(buf, indent + 2u) || !syn_pretty(buf, child, m->items[i], indent + 2u)) return false;
                continue;
            }
        }
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!syn_pretty(buf, child, m->items[i], indent)) return false;
    }
    return idm_buf_append(buf, close);
}

bool idm_syn_dump_pretty(IdmBuffer *buf, const IdmSyntax *syn) {
    SynMeasure *m = syn_measure(syn);
    if (!m) return false;
    bool ok = syn_pretty(buf, syn, m, 0);
    syn_measure_free(m);
    return ok;
}

static bool syn_serialize_bigint_payload(IdmBuffer *out, const IdmSyntax *syn, IdmError *err) {
    size_t len = strlen(syn->as.text);
    size_t cap = idm_bignum_decimal_limb_cap(len);
    if (cap > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, syn->span, "integer literal is too large");
    uint32_t *limbs = cap == 0u ? NULL : calloc(cap, sizeof(*limbs));
    if (cap != 0u && !limbs) return idm_error_oom(err, syn->span);
    size_t count = 0u;
    int sign = 0;
    bool parsed = idm_bignum_from_decimal(syn->as.text, len, limbs, &count, &sign);
    if (!parsed) {
        free(limbs);
        return idm_error_set(err, syn->span, "invalid integer literal");
    }
    if (count > UINT32_MAX) {
        free(limbs);
        return idm_error_set(err, syn->span, "integer literal is too large");
    }
    uint8_t sign_tag = sign < 0 ? 1u : (sign > 0 ? 2u : 0u);
    bool ok = idm_buf_put_u8(out, sign_tag) && idm_buf_put_u32(out, (uint32_t)count);
    for (size_t i = 0; ok && i < count; i++) ok = idm_buf_put_u32(out, limbs[i]);
    free(limbs);
    return ok || idm_error_oom(err, syn->span);
}

static bool syn_serialize_depth(IdmBuffer *out, const IdmSyntax *syn, unsigned depth, IdmError *err) {
    if (!syn) return idm_error_set(err, idm_span_unknown(NULL), "cannot serialize null syntax");
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, syn->span, "syntax nested too deeply to serialize");
    if (!idm_buf_put_u8(out, (uint8_t)syn->kind)) return idm_error_oom(err, syn->span);
    bool ok = idm_buf_put_opt_str(out, syn->span.file);
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
    ok = ok && idm_buf_put_opt_str(out, syn->token_raw);
    if (ok && syn->token_raw) {
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
        case IDM_SYN_BIGINT:
            return syn_serialize_bigint_payload(out, syn, err);
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
            if (!idm_buf_put_u32(out, (uint32_t)syn->as.seq.count)) return idm_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!syn_serialize_depth(out, syn->as.seq.items[i], depth + 1u, err)) return false;
            }
            return true;
    }
    return idm_error_set(err, syn->span, "unknown syntax kind %u cannot be serialized", (unsigned)syn->kind);
}

bool idm_syn_serialize(IdmBuffer *out, const IdmSyntax *syn, IdmError *err) {
    return syn_serialize_depth(out, syn, 0u, err);
}

static char *syn_deserialize_bigint_text(IdmByteReader *r, IdmSpan span, IdmError *err) {
    uint8_t sign_tag = idm_rd_u8(r);
    uint32_t wire_count = idm_rd_u32(r);
    if (!r->ok) {
        idm_error_set(err, span, "truncated serialized syntax bigint");
        return NULL;
    }
    if (sign_tag > 2u) {
        idm_error_set(err, span, "invalid serialized syntax bigint sign");
        return NULL;
    }
    int sign = sign_tag == 1u ? -1 : (sign_tag == 2u ? 1 : 0);
    size_t count = wire_count;
    if ((count == 0u) != (sign == 0)) {
        idm_error_set(err, span, "invalid serialized syntax bigint zero encoding");
        return NULL;
    }
    if (count > SIZE_MAX / sizeof(uint32_t)) {
        idm_error_oom(err, span);
        return NULL;
    }
    uint32_t *limbs = count == 0u ? NULL : malloc(count * sizeof(*limbs));
    if (count != 0u && !limbs) {
        idm_error_oom(err, span);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) limbs[i] = idm_rd_u32(r);
    if (!r->ok) {
        free(limbs);
        idm_error_set(err, span, "truncated serialized syntax bigint");
        return NULL;
    }
    uint32_t *scratch = count == 0u ? NULL : malloc(count * sizeof(*scratch));
    if (count != 0u && !scratch) {
        free(limbs);
        idm_error_oom(err, span);
        return NULL;
    }
    if (count != 0u) memcpy(scratch, limbs, count * sizeof(*scratch));
    size_t digit_cap = idm_bignum_decimal_digit_cap(count);
    if (digit_cap > SIZE_MAX - 2u) {
        free(scratch);
        free(limbs);
        idm_error_oom(err, span);
        return NULL;
    }
    char *text = malloc(digit_cap + 2u);
    if (!text) {
        free(scratch);
        free(limbs);
        idm_error_oom(err, span);
        return NULL;
    }
    (void)idm_bignum_to_decimal(scratch, count, sign, text);
    free(scratch);
    free(limbs);
    return text;
}

static IdmSyntax *syn_deserialize_depth(IdmRuntime *rt, IdmByteReader *r, unsigned depth, IdmError *err) {
    if (depth > IDM_IC_MAX_DEPTH) {
        idm_error_set(err, idm_span_unknown(NULL), "serialized syntax nested too deeply");
        return NULL;
    }
    uint8_t kind = idm_rd_u8(r);
    if (!r->ok || kind > (uint8_t)IDM_SYN_BIGINT) {
        idm_error_set(err, idm_span_unknown(NULL), "truncated or invalid serialized syntax header");
        return NULL;
    }
    IdmSpan span = idm_span_unknown(NULL);
    char *file = NULL;
    if (!idm_rd_opt_str(r, &file, err)) return NULL;
    if (file) {
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
    char *raw = NULL;
    if (ok && !idm_rd_opt_str(r, &raw, err)) ok = false;
    if (ok && raw) {
        uint8_t leading = idm_rd_u8(r);
        uint8_t adjacent = idm_rd_u8(r);
        if (!r->ok) ok = false;
        else idm_syn_set_token(syn, raw, leading != 0u, adjacent != 0u);
        if (ok && !syn->token_raw) ok = false;
    }
    free(raw);
    uint32_t property_count = ok ? idm_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < property_count; i++) {
        char *key = idm_rd_string(r, NULL);
        char *value = key ? idm_rd_string(r, NULL) : NULL;
        if (!key || !value || !idm_syn_property_set(syn, key, value)) ok = false;
        free(key);
        free(value);
    }
    uint32_t origin_count = ok ? idm_rd_u32(r) : 0u;
    for (uint32_t i = 0; ok && i < origin_count; i++) {
        char *origin = idm_rd_string(r, NULL);
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
            char *text = idm_rd_string(r, NULL);
            if (!text) {
                idm_error_set(err, span, "truncated serialized syntax text");
                idm_syn_free(syn);
                return NULL;
            }
            syn->as.text = text;
            return syn;
        }
        case IDM_SYN_BIGINT: {
            char *text = syn_deserialize_bigint_text(r, span, err);
            if (!text) {
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
            uint32_t child_count = idm_rd_u32(r);
            if (!r->ok) {
                idm_error_set(err, span, "truncated serialized syntax sequence");
                idm_syn_free(syn);
                return NULL;
            }
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

IdmSyntax *idm_syn_program_prepend_program(const IdmSyntax *program, const IdmSyntax *prelude, const char *file) {
    IdmSpan span = idm_span_unknown(file);
    span.line = 1;
    span.column = 1;
    IdmSyntax *wrapped = idm_syn_list(span);
    bool ok = wrapped && idm_syn_append(wrapped, idm_syn_word("%-package-begin", span));
    for (size_t i = 1; ok && prelude && i < prelude->as.seq.count; i++) {
        IdmSyntax *item = idm_syn_clone(prelude->as.seq.items[i]);
        ok = item != NULL && idm_syn_append(wrapped, item);
        if (!ok) idm_syn_free(item);
    }
    for (size_t i = 1; ok && i < program->as.seq.count; i++) {
        IdmSyntax *item = idm_syn_clone(program->as.seq.items[i]);
        ok = item != NULL && idm_syn_append(wrapped, item);
        if (!ok) idm_syn_free(item);
    }
    if (!ok) {
        idm_syn_free(wrapped);
        return NULL;
    }
    return wrapped;
}
