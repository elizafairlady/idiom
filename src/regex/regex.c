#include "idiom/regex.h"
#include "idiom/prims.h"
#include "idiom/reader.h"

#include <stdio.h>

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    RX_EMPTY,
    RX_LITERAL,
    RX_DOT,
    RX_CLASS,
    RX_ANCHOR_START,
    RX_ANCHOR_END,
    RX_WORD_BOUNDARY,
    RX_NOT_WORD_BOUNDARY,
    RX_BUFFER_START,
    RX_BUFFER_END,
    RX_BUFFER_END_NL,
    RX_CONCAT,
    RX_ALT,
    RX_REPEAT,
    RX_CAPTURE,
    RX_LOOK_POS,
    RX_LOOK_NEG,
    RX_LOOKBEHIND_POS,
    RX_LOOKBEHIND_NEG
} RxNodeKind;

typedef struct RxNode RxNode;

typedef struct {
    RxNode **items;
    size_t count;
    size_t cap;
} RxNodeVec;

struct RxNode {
    RxNodeKind kind;
    union {
        unsigned char literal;
        IdmByteClass cls;
        RxNodeVec seq;
        struct {
            RxNode *child;
            size_t min;
            size_t max;
            bool unbounded;
        } repeat;
        struct {
            RxNode *child;
            size_t index;
        } capture;
        RxNode *child;
    } as;
};

typedef struct {
    bool any;
    bool bytes[256];
    bool nullable;
    bool anchored_start;
} RxStartInfo;

struct IdmRegex {
    char *source;
    size_t source_len;
    uint32_t flags;
    RxNode *root;
    SelByteProg *byteprog;
    size_t capture_count;
    RxStartInfo start;
    char **group_names;
    size_t group_count;
};

struct IdmRegexSet {
    SelByteProg *byteprog;
    size_t capture_count;
    size_t count;
    char ***group_names;
    size_t *group_counts;
    char **item_sources;
    size_t *item_source_lens;
    uint32_t *item_flags;
};

struct IdmRegexResult {
    char *subject;
    size_t subject_len;
    IdmValue subject_value;
    IdmByteCapture *captures;
    char **group_names;
    size_t capture_count;
    bool inline_storage;
    bool owns_subject;
};

typedef struct {
    const char *source;
    size_t len;
    size_t pos;
    uint32_t flags;
    char **group_names;
    size_t group_count;
    size_t group_cap;
    IdmError *err;
    size_t depth;
} RxParser;

static void node_free(RxNode *node);
static bool parse_alt(RxParser *p, RxNode **out);
static bool regex_start_candidate(const RxStartInfo *start, uint32_t flags, const char *s, size_t len, size_t pos);

static bool parse_flag_atom(const char *text, uint32_t *flags) {
    if (strcmp(text, "caseless") == 0 || strcmp(text, "ignore-case") == 0 || strcmp(text, "i") == 0) {
        *flags |= IDM_REGEX_CASELESS;
        return true;
    }
    if (strcmp(text, "multiline") == 0 || strcmp(text, "m") == 0) {
        *flags |= IDM_REGEX_MULTILINE;
        return true;
    }
    if (strcmp(text, "dotall") == 0 || strcmp(text, "s") == 0) {
        *flags |= IDM_REGEX_DOTALL;
        return true;
    }
    return false;
}

static bool parse_options_list(IdmRuntime *rt, IdmValue options, uint32_t *flags, IdmError *err) {
    IdmValue cur = options;
    while (idm_is_pair(cur)) {
        IdmValue item = idm_car(cur, err);
        if (err && err->present) return false;
        if (idm_value_tag(item) != IDM_VAL_ATOM && idm_value_tag(item) != IDM_VAL_STRING) {
            idm_error_set(err, idm_span_unknown(NULL), "regex option must be an atom or string");
            return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "raw-compile"), item);
        }
        const char *text = idm_value_tag(item) == IDM_VAL_ATOM ? idm_symbol_text(idm_value_symbol(item)) : idm_string_bytes(item);
        if (!parse_flag_atom(text, flags)) return idm_error_set(err, idm_span_unknown(NULL), "unknown regex option '%s'", text);
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "regex options must be an atom or proper list");
    return true;
}

static bool parse_options(IdmRuntime *rt, IdmValue options, uint32_t *out_flags, IdmError *err) {
    uint32_t flags = 0;
    if (idm_value_tag(options) == IDM_VAL_NIL || idm_is_empty_list(options)) {
        *out_flags = 0;
        return true;
    }
    if (idm_value_tag(options) == IDM_VAL_ATOM || idm_value_tag(options) == IDM_VAL_STRING) {
        const char *text = idm_value_tag(options) == IDM_VAL_ATOM ? idm_symbol_text(idm_value_symbol(options)) : idm_string_bytes(options);
        if (!parse_flag_atom(text, &flags)) return idm_error_set(err, idm_span_unknown(NULL), "unknown regex option '%s'", text);
        *out_flags = flags;
        return true;
    }
    if (idm_is_pair(options)) {
        if (!parse_options_list(rt, options, &flags, err)) return false;
        *out_flags = flags;
        return true;
    }
    if (idm_is_vector(options) || idm_is_tuple(options)) {
        for (size_t i = 0; i < idm_sequence_count(options); i++) {
            IdmValue item = idm_sequence_item(options, i, err);
            if (err && err->present) return false;
            if (idm_value_tag(item) != IDM_VAL_ATOM && idm_value_tag(item) != IDM_VAL_STRING) return idm_error_set(err, idm_span_unknown(NULL), "regex option must be an atom or string");
            const char *text = idm_value_tag(item) == IDM_VAL_ATOM ? idm_symbol_text(idm_value_symbol(item)) : idm_string_bytes(item);
            if (!parse_flag_atom(text, &flags)) return idm_error_set(err, idm_span_unknown(NULL), "unknown regex option '%s'", text);
        }
        *out_flags = flags;
        return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "regex options must be :nil, an atom, a list, tuple, or vector");
}

static bool result_tuple(IdmRuntime *rt, IdmValue tag, IdmValue payload, IdmValue *out, IdmError *err) {
    IdmValue items[2] = { tag, payload };
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}

static bool regex_compile_error_value(IdmRuntime *rt, const char *message, IdmValue source, IdmValue *out, IdmError *err) {
    IdmValue detail_items[4];
    detail_items[0] = idm_atom(rt, "regex");
    detail_items[1] = idm_atom(rt, "compile");
    detail_items[2] = idm_string(rt, message ? message : "invalid regex", err);
    detail_items[3] = source;
    if (err && err->present) return false;
    IdmValue detail = idm_tuple(rt, detail_items, 4u, err);
    if (err && err->present) return false;
    *out = idm_error_value(rt, detail);
    return !(err && err->present);
}

static RxNode *node_new(RxNodeKind kind) {
    RxNode *node = calloc(1u, sizeof(*node));
    if (node) node->kind = kind;
    return node;
}

static bool node_vec_push(RxNodeVec *vec, RxNode *node) {
    if (vec->count == vec->cap) {
        if (!idm_grow((void **)&vec->items, &vec->cap, sizeof(*vec->items), 4u, vec->count + 1u)) return false;
    }
    vec->items[vec->count++] = node;
    return true;
}

static void node_vec_destroy(RxNodeVec *vec) {
    for (size_t i = 0; i < vec->count; i++) node_free(vec->items[i]);
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static void node_free(RxNode *node) {
    if (!node) return;
    switch (node->kind) {
        case RX_CONCAT:
        case RX_ALT:
            node_vec_destroy(&node->as.seq);
            break;
        case RX_REPEAT:
            node_free(node->as.repeat.child);
            break;
        case RX_CAPTURE:
            node_free(node->as.capture.child);
            break;
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG:
            node_free(node->as.child);
            break;
        case RX_EMPTY:
        case RX_LITERAL:
        case RX_DOT:
        case RX_CLASS:
        case RX_ANCHOR_START:
        case RX_ANCHOR_END:
        case RX_WORD_BOUNDARY:
        case RX_NOT_WORD_BOUNDARY:
        case RX_BUFFER_START:
        case RX_BUFFER_END:
        case RX_BUFFER_END_NL:
            break;
    }
    free(node);
}

static size_t node_footprint(const RxNode *node) {
    if (!node) return 0;
    size_t total = sizeof(*node);
    switch (node->kind) {
        case RX_CONCAT:
        case RX_ALT:
            total += node->as.seq.cap * sizeof(*node->as.seq.items);
            for (size_t i = 0; i < node->as.seq.count; i++) total += node_footprint(node->as.seq.items[i]);
            break;
        case RX_REPEAT:
            total += node_footprint(node->as.repeat.child);
            break;
        case RX_CAPTURE:
            total += node_footprint(node->as.capture.child);
            break;
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG:
            total += node_footprint(node->as.child);
            break;
        default:
            break;
    }
    return total;
}

static bool parser_error(RxParser *p, const char *message) {
    return idm_error_set(p->err, idm_span_unknown(NULL), "regex parse error at byte %zu: %s", p->pos, message);
}

static int parser_peek(RxParser *p) {
    return p->pos < p->len ? (unsigned char)p->source[p->pos] : -1;
}

static int parser_take(RxParser *p) {
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->source[p->pos++];
}

static bool regex_is_caseless(const RxParser *p) {
    return (p->flags & IDM_REGEX_CASELESS) != 0;
}

static int pred_word(int c) {
    return isalnum(c) || c == '_';
}

static bool cls_add_named(IdmByteClass *cls, const char *name, bool caseless) {
    if (strcmp(name, "alnum") == 0) idm_byteclass_add_pred(cls, isalnum, caseless);
    else if (strcmp(name, "alpha") == 0) idm_byteclass_add_pred(cls, isalpha, caseless);
    else if (strcmp(name, "blank") == 0) { idm_byteclass_add_char(cls, ' ', caseless); idm_byteclass_add_char(cls, '\t', caseless); }
    else if (strcmp(name, "cntrl") == 0) idm_byteclass_add_pred(cls, iscntrl, caseless);
    else if (strcmp(name, "digit") == 0) idm_byteclass_add_pred(cls, isdigit, caseless);
    else if (strcmp(name, "graph") == 0) idm_byteclass_add_pred(cls, isgraph, caseless);
    else if (strcmp(name, "lower") == 0) idm_byteclass_add_pred(cls, islower, caseless);
    else if (strcmp(name, "print") == 0) idm_byteclass_add_pred(cls, isprint, caseless);
    else if (strcmp(name, "punct") == 0) idm_byteclass_add_pred(cls, ispunct, caseless);
    else if (strcmp(name, "space") == 0) idm_byteclass_add_pred(cls, isspace, caseless);
    else if (strcmp(name, "upper") == 0) idm_byteclass_add_pred(cls, isupper, caseless);
    else if (strcmp(name, "word") == 0) idm_byteclass_add_pred(cls, pred_word, caseless);
    else if (strcmp(name, "xdigit") == 0) idm_byteclass_add_pred(cls, isxdigit, caseless);
    else return false;
    return true;
}

static RxNode *class_node_named(const char *name, bool negated, bool caseless) {
    RxNode *node = node_new(RX_CLASS);
    if (!node) return NULL;
    node->as.cls.negated = negated;
    if (!cls_add_named(&node->as.cls, name, caseless)) {
        node_free(node);
        return NULL;
    }
    return node;
}

static RxNode *literal_node(unsigned char c) {
    RxNode *node = node_new(RX_LITERAL);
    if (node) node->as.literal = c;
    return node;
}

static bool parse_decimal(RxParser *p, size_t *out) {
    if (!isdigit((unsigned char)parser_peek(p))) return false;
    size_t value = 0;
    while (isdigit((unsigned char)parser_peek(p))) {
        unsigned digit = (unsigned)(parser_take(p) - '0');
        if (value > (SIZE_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool parse_escape_atom(RxParser *p, RxNode **out) {
    if (parser_take(p) != '\\') return parser_error(p, "internal escape parser error");
    int c = parser_take(p);
    if (c < 0) return parser_error(p, "trailing backslash");
    char decoded = 0;
    if (idm_reader_string_escape((char)c, &decoded)) {
        *out = literal_node((unsigned char)decoded);
        return *out != NULL;
    }
    switch (c) {
        case 'd': *out = class_node_named("digit", false, regex_is_caseless(p)); return *out != NULL;
        case 'D': *out = class_node_named("digit", true, regex_is_caseless(p)); return *out != NULL;
        case 's': *out = class_node_named("space", false, regex_is_caseless(p)); return *out != NULL;
        case 'S': *out = class_node_named("space", true, regex_is_caseless(p)); return *out != NULL;
        case 'w': *out = class_node_named("word", false, regex_is_caseless(p)); return *out != NULL;
        case 'W': *out = class_node_named("word", true, regex_is_caseless(p)); return *out != NULL;
        case 'b': *out = node_new(RX_WORD_BOUNDARY); return *out != NULL;
        case 'B': *out = node_new(RX_NOT_WORD_BOUNDARY); return *out != NULL;
        case 'A': *out = node_new(RX_BUFFER_START); return *out != NULL;
        case 'z': *out = node_new(RX_BUFFER_END); return *out != NULL;
        case 'Z': *out = node_new(RX_BUFFER_END_NL); return *out != NULL;
        default: *out = literal_node((unsigned char)c); return *out != NULL;
    }
}

static bool parse_class_char(RxParser *p, IdmByteClass *cls, bool *out_is_char, unsigned char *out_ch) {
    int c = parser_take(p);
    if (c < 0) return parser_error(p, "unterminated character class");
    if (c == '\\') {
        int e = parser_take(p);
        if (e < 0) return parser_error(p, "trailing class escape");
        char decoded = 0;
        if (idm_reader_string_escape((char)e, &decoded)) {
            *out_is_char = true;
            *out_ch = (unsigned char)decoded;
            return true;
        }
        switch (e) {
            case 'd': cls_add_named(cls, "digit", regex_is_caseless(p)); *out_is_char = false; return true;
            case 'D': {
                IdmByteClass tmp = {0};
                tmp.negated = true;
                cls_add_named(&tmp, "digit", regex_is_caseless(p));
                for (unsigned i = 0; i < 256u; i++) if (idm_byteclass_has(&tmp, (unsigned char)i)) idm_byteclass_add_char(cls, (unsigned char)i, regex_is_caseless(p));
                *out_is_char = false;
                return true;
            }
            case 's': cls_add_named(cls, "space", regex_is_caseless(p)); *out_is_char = false; return true;
            case 'w': cls_add_named(cls, "word", regex_is_caseless(p)); *out_is_char = false; return true;
            default: *out_is_char = true; *out_ch = (unsigned char)e; return true;
        }
    }
    *out_is_char = true;
    *out_ch = (unsigned char)c;
    return true;
}

static bool parse_posix_class(RxParser *p, IdmByteClass *cls, bool *out_done) {
    *out_done = false;
    if (p->pos + 3u > p->len || p->source[p->pos] != '[') return true;
    char opener = p->source[p->pos + 1u];
    if (opener != ':' && opener != '=' && opener != '.') return true;
    size_t name_start = p->pos + 2u;
    size_t i = name_start;
    while (i + 1u < p->len && !(p->source[i] == opener && p->source[i + 1u] == ']')) i++;
    if (i + 1u >= p->len) return true;
    char *name = idm_strndup(p->source + name_start, i - name_start);
    if (!name) return parser_error(p, "out of memory");
    if (opener == ':') {
        bool ok = cls_add_named(cls, name, regex_is_caseless(p));
        free(name);
        if (!ok) return parser_error(p, "unknown POSIX character class");
    } else {
        if (name[0] != '\0') {
            for (size_t j = 0; name[j] != '\0'; j++) idm_byteclass_add_char(cls, (unsigned char)name[j], regex_is_caseless(p));
        }
        free(name);
    }
    p->pos = i + 2u;
    *out_done = true;
    return true;
}

static bool parse_class(RxParser *p, RxNode **out) {
    if (parser_take(p) != '[') return parser_error(p, "internal class parser error");
    RxNode *node = node_new(RX_CLASS);
    if (!node) return parser_error(p, "out of memory");
    if (parser_peek(p) == '^') {
        parser_take(p);
        node->as.cls.negated = true;
    }
    bool first = true;
    while (parser_peek(p) >= 0) {
        if (!first && parser_peek(p) == ']') {
            parser_take(p);
            *out = node;
            return true;
        }
        bool posix_done = false;
        if (!parse_posix_class(p, &node->as.cls, &posix_done)) {
            node_free(node);
            return false;
        }
        if (posix_done) {
            first = false;
            continue;
        }
        bool is_char = false;
        unsigned char lo = 0;
        if (!parse_class_char(p, &node->as.cls, &is_char, &lo)) {
            node_free(node);
            return false;
        }
        if (is_char && parser_peek(p) == '-' && p->pos + 1u < p->len && p->source[p->pos + 1u] != ']') {
            parser_take(p);
            bool hi_is_char = false;
            unsigned char hi = 0;
            if (!parse_class_char(p, &node->as.cls, &hi_is_char, &hi)) {
                node_free(node);
                return false;
            }
            if (!hi_is_char) {
                node_free(node);
                return parser_error(p, "class range endpoint must be a character");
            }
            idm_byteclass_add_range(&node->as.cls, lo, hi, regex_is_caseless(p));
        } else if (is_char) {
            idm_byteclass_add_char(&node->as.cls, lo, regex_is_caseless(p));
        }
        first = false;
    }
    node_free(node);
    return parser_error(p, "unterminated character class");
}

static bool group_name_seen(RxParser *p, const char *name) {
    for (size_t i = 1; i <= p->group_count; i++) {
        if (p->group_names[i] && strcmp(p->group_names[i], name) == 0) return true;
    }
    return false;
}

static bool add_group(RxParser *p, const char *name, size_t *out_index) {
    if (name && group_name_seen(p, name)) return parser_error(p, "duplicate capture group name");
    size_t next = p->group_count + 1u;
    if (!idm_grow((void **)&p->group_names, &p->group_cap, sizeof(*p->group_names), 4u, next + 1u)) return parser_error(p, "out of memory");
    p->group_names[next] = NULL;
    if (name) {
        p->group_names[next] = idm_strdup(name);
        if (!p->group_names[next]) return parser_error(p, "out of memory");
    }
    p->group_count = next;
    *out_index = next;
    return true;
}

static bool parse_group(RxParser *p, RxNode **out) {
    if (parser_take(p) != '(') return parser_error(p, "internal group parser error");
    if (parser_peek(p) == '?') {
        parser_take(p);
        int kind = parser_take(p);
        RxNodeKind look_kind = RX_EMPTY;
        if (kind == ':') {
            RxNode *child = NULL;
            if (!parse_alt(p, &child)) return false;
            if (parser_take(p) != ')') { node_free(child); return parser_error(p, "unterminated group"); }
            *out = child;
            return true;
        }
        if (kind == '=') look_kind = RX_LOOK_POS;
        else if (kind == '!') look_kind = RX_LOOK_NEG;
        else if (kind == '<') {
            int next = parser_take(p);
            if (next == '=') look_kind = RX_LOOKBEHIND_POS;
            else if (next == '!') look_kind = RX_LOOKBEHIND_NEG;
            else {
                size_t name_start = p->pos - 1u;
                while (parser_peek(p) >= 0 && parser_peek(p) != '>') {
                    int ch = parser_peek(p);
                    if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return parser_error(p, "invalid capture group name");
                    parser_take(p);
                }
                if (parser_take(p) != '>') return parser_error(p, "unterminated capture group name");
                char *name = idm_strndup(p->source + name_start, p->pos - name_start - 1u);
                if (!name) return parser_error(p, "out of memory");
                if (name[0] == '\0') { free(name); return parser_error(p, "empty capture group name"); }
                size_t index = 0;
                bool ok = add_group(p, name, &index);
                free(name);
                if (!ok) return false;
                RxNode *child = NULL;
                if (!parse_alt(p, &child)) return false;
                if (parser_take(p) != ')') { node_free(child); return parser_error(p, "unterminated group"); }
                RxNode *cap = node_new(RX_CAPTURE);
                if (!cap) { node_free(child); return parser_error(p, "out of memory"); }
                cap->as.capture.child = child;
                cap->as.capture.index = index;
                *out = cap;
                return true;
            }
        } else {
            return parser_error(p, "unknown group extension");
        }
        RxNode *child = NULL;
        if (!parse_alt(p, &child)) return false;
        if (parser_take(p) != ')') { node_free(child); return parser_error(p, "unterminated lookaround"); }
        RxNode *look = node_new(look_kind);
        if (!look) { node_free(child); return parser_error(p, "out of memory"); }
        look->as.child = child;
        *out = look;
        return true;
    }

    size_t index = 0;
    if (!add_group(p, NULL, &index)) return false;
    RxNode *child = NULL;
    if (!parse_alt(p, &child)) return false;
    if (parser_take(p) != ')') { node_free(child); return parser_error(p, "unterminated group"); }
    RxNode *cap = node_new(RX_CAPTURE);
    if (!cap) { node_free(child); return parser_error(p, "out of memory"); }
    cap->as.capture.child = child;
    cap->as.capture.index = index;
    *out = cap;
    return true;
}

static bool parse_atom(RxParser *p, RxNode **out) {
    int c = parser_peek(p);
    if (c < 0) return parser_error(p, "expected atom");
    if (c == '(') return parse_group(p, out);
    if (c == '[') return parse_class(p, out);
    if (c == '.') { parser_take(p); *out = node_new(RX_DOT); return *out != NULL || parser_error(p, "out of memory"); }
    if (c == '^') { parser_take(p); *out = node_new(RX_ANCHOR_START); return *out != NULL || parser_error(p, "out of memory"); }
    if (c == '$') { parser_take(p); *out = node_new(RX_ANCHOR_END); return *out != NULL || parser_error(p, "out of memory"); }
    if (c == '\\') return parse_escape_atom(p, out);
    if (c == '*' || c == '+' || c == '?') return parser_error(p, "quantifier has no atom");
    parser_take(p);
    *out = literal_node((unsigned char)c);
    return *out != NULL || parser_error(p, "out of memory");
}

static bool parse_interval_quantifier(RxParser *p, size_t *out_min, size_t *out_max, bool *out_unbounded, bool *out_present) {
    *out_present = false;
    if (parser_peek(p) != '{') return true;
    size_t save = p->pos;
    parser_take(p);
    size_t min = 0;
    if (!parse_decimal(p, &min)) {
        p->pos = save;
        return true;
    }
    size_t max = min;
    bool unbounded = false;
    if (parser_peek(p) == ',') {
        parser_take(p);
        if (parser_peek(p) == '}') unbounded = true;
        else if (!parse_decimal(p, &max)) {
            p->pos = save;
            return true;
        }
    }
    if (parser_peek(p) != '}') {
        p->pos = save;
        return true;
    }
    parser_take(p);
    if (!unbounded && max < min) return parser_error(p, "quantifier upper bound is smaller than lower bound");
    *out_min = min;
    *out_max = max;
    *out_unbounded = unbounded;
    *out_present = true;
    return true;
}

static bool parse_repeat(RxParser *p, RxNode **out) {
    RxNode *atom = NULL;
    if (!parse_atom(p, &atom)) return false;
    size_t min = 0;
    size_t max = 0;
    bool unbounded = false;
    bool quantified = true;
    int c = parser_peek(p);
    if (c == '*') {
        parser_take(p);
        min = 0;
        unbounded = true;
    } else if (c == '+') {
        parser_take(p);
        min = 1;
        unbounded = true;
    } else if (c == '?') {
        parser_take(p);
        min = 0;
        max = 1;
    } else {
        quantified = false;
        bool present = false;
        if (!parse_interval_quantifier(p, &min, &max, &unbounded, &present)) {
            node_free(atom);
            return false;
        }
        quantified = present;
    }
    if (!quantified) {
        *out = atom;
        return true;
    }
    RxNode *node = node_new(RX_REPEAT);
    if (!node) {
        node_free(atom);
        return parser_error(p, "out of memory");
    }
    node->as.repeat.child = atom;
    node->as.repeat.min = min;
    node->as.repeat.max = max;
    node->as.repeat.unbounded = unbounded;
    *out = node;
    return true;
}

static bool parse_concat(RxParser *p, RxNode **out) {
    RxNodeVec seq = {0};
    while (parser_peek(p) >= 0 && parser_peek(p) != '|' && parser_peek(p) != ')') {
        RxNode *node = NULL;
        if (!parse_repeat(p, &node)) {
            node_vec_destroy(&seq);
            return false;
        }
        if (!node_vec_push(&seq, node)) {
            node_free(node);
            node_vec_destroy(&seq);
            return parser_error(p, "out of memory");
        }
    }
    if (seq.count == 0) {
        *out = node_new(RX_EMPTY);
        return *out != NULL || parser_error(p, "out of memory");
    }
    if (seq.count == 1) {
        *out = seq.items[0];
        free(seq.items);
        return true;
    }
    RxNode *node = node_new(RX_CONCAT);
    if (!node) {
        node_vec_destroy(&seq);
        return parser_error(p, "out of memory");
    }
    node->as.seq = seq;
    *out = node;
    return true;
}

static bool parse_alt_body(RxParser *p, RxNode **out) {
    RxNodeVec seq = {0};
    for (;;) {
        RxNode *node = NULL;
        if (!parse_concat(p, &node)) {
            node_vec_destroy(&seq);
            return false;
        }
        if (!node_vec_push(&seq, node)) {
            node_free(node);
            node_vec_destroy(&seq);
            return parser_error(p, "out of memory");
        }
        if (parser_peek(p) != '|') break;
        parser_take(p);
    }
    if (seq.count == 1) {
        *out = seq.items[0];
        free(seq.items);
        return true;
    }
    RxNode *node = node_new(RX_ALT);
    if (!node) {
        node_vec_destroy(&seq);
        return parser_error(p, "out of memory");
    }
    node->as.seq = seq;
    *out = node;
    return true;
}

static bool parse_alt(RxParser *p, RxNode **out) {
    if (p->depth >= IDM_IC_MAX_DEPTH) return parser_error(p, "regex nested too deeply");
    p->depth++;
    bool ok = parse_alt_body(p, out);
    p->depth--;
    return ok;
}

#define RX_REPEAT_COMPILE_LIMIT 100000u

static void start_info_add_byte(RxStartInfo *info, unsigned char ch, uint32_t flags) {
    info->bytes[ch] = true;
    if ((flags & IDM_REGEX_CASELESS) != 0 && isalpha(ch)) {
        info->bytes[(unsigned char)tolower(ch)] = true;
        info->bytes[(unsigned char)toupper(ch)] = true;
    }
}

static void start_info_add_class(RxStartInfo *info, const IdmByteClass *cls) {
    for (unsigned i = 0; i < 256u; i++) {
        if (idm_byteclass_has(cls, (unsigned char)i)) info->bytes[i] = true;
    }
}

static void start_info_union(RxStartInfo *dst, const RxStartInfo *src) {
    dst->any = dst->any || src->any;
    dst->nullable = dst->nullable || src->nullable;
    for (size_t i = 0; i < 256u; i++) dst->bytes[i] = dst->bytes[i] || src->bytes[i];
}

static RxStartInfo start_info_node(const RxNode *node, uint32_t flags) {
    RxStartInfo info = {0};
    if (!node) {
        info.nullable = true;
        return info;
    }
    switch (node->kind) {
        case RX_EMPTY:
        case RX_ANCHOR_END:
        case RX_WORD_BOUNDARY:
        case RX_NOT_WORD_BOUNDARY:
        case RX_BUFFER_END:
        case RX_BUFFER_END_NL:
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG:
            info.nullable = true;
            return info;
        case RX_ANCHOR_START:
        case RX_BUFFER_START:
            info.nullable = true;
            info.anchored_start = true;
            return info;
        case RX_LITERAL:
            start_info_add_byte(&info, node->as.literal, flags);
            return info;
        case RX_DOT:
            info.any = true;
            return info;
        case RX_CLASS:
            start_info_add_class(&info, &node->as.cls);
            return info;
        case RX_CAPTURE:
            return start_info_node(node->as.capture.child, flags);
        case RX_REPEAT:
            info = start_info_node(node->as.repeat.child, flags);
            if (node->as.repeat.min == 0) {
                info.nullable = true;
                info.anchored_start = false;
            }
            return info;
        case RX_ALT:
            info.anchored_start = node->as.seq.count != 0;
            for (size_t i = 0; i < node->as.seq.count; i++) {
                RxStartInfo child = start_info_node(node->as.seq.items[i], flags);
                start_info_union(&info, &child);
                info.anchored_start = info.anchored_start && child.anchored_start;
            }
            return info;
        case RX_CONCAT: {
            info.nullable = true;
            for (size_t i = 0; i < node->as.seq.count; i++) {
                RxStartInfo child = start_info_node(node->as.seq.items[i], flags);
                bool prior_nullable = info.nullable;
                info.any = info.any || child.any;
                info.anchored_start = info.anchored_start || (i == 0 && child.anchored_start);
                for (size_t b = 0; b < 256u; b++) info.bytes[b] = info.bytes[b] || child.bytes[b];
                info.nullable = prior_nullable && child.nullable;
                if (!child.nullable) break;
            }
            return info;
        }
    }
    info.any = true;
    return info;
}

static bool bcompile_node(SelByteProg *bp, const RxNode *node, uint32_t flags, IdmError *err);

static bool bcompile_subprog(const RxNode *node, uint32_t flags, SelByteProg **out, IdmError *err) {
    *out = idm_byteprog_new(err);
    if (!*out) return false;
    idm_byteprog_set_flags(*out, flags);
    idm_byteprog_set_capture_count(*out, 0);
    if (!bcompile_node(*out, node, flags, err) || idm_byteprog_accept(*out, 0, err) == SEL_NO_NODE) {
        idm_byteprog_free(*out);
        *out = NULL;
        return false;
    }
    idm_byteprog_set_start(*out, 0);
    idm_byteprog_finalize_linear(*out);
    return true;
}

static bool bcompile_alt_from(SelByteProg *bp, RxNode **items, size_t index, size_t count, uint32_t flags, IdmError *err) {
    if (index >= count) return true;
    uint32_t *jumps = NULL;
    size_t jc = 0, jcap = 0;
    bool ok = true;
    for (size_t i = index; ok && i + 1u < count; i++) {
        uint32_t split = idm_byteprog_fork(bp, err);
        if (split == SEL_NO_NODE) { ok = false; break; }
        uint32_t first = (uint32_t)idm_byteprog_node_count(bp);
        if (!bcompile_node(bp, items[i], flags, err)) { ok = false; break; }
        uint32_t jump = idm_byteprog_fork(bp, err);
        if (jump == SEL_NO_NODE) { ok = false; break; }
        uint32_t second = (uint32_t)idm_byteprog_node_count(bp);
        idm_byteprog_set_fork(bp, split, first, second);
        if (jc == jcap) {
            if (!idm_grow((void **)&jumps, &jcap, sizeof(*jumps), 8u, jc + 1u)) { idm_error_oom(err, idm_span_unknown(NULL)); ok = false; break; }
        }
        jumps[jc++] = jump;
    }
    if (ok) ok = bcompile_node(bp, items[count - 1u], flags, err);
    uint32_t end = (uint32_t)idm_byteprog_node_count(bp);
    for (size_t i = 0; ok && i < jc; i++) idm_byteprog_set_fork(bp, jumps[i], end, end);
    free(jumps);
    return ok;
}

static bool bcompile_star(SelByteProg *bp, const RxNode *child, uint32_t flags, IdmError *err) {
    uint32_t split = idm_byteprog_fork(bp, err);
    if (split == SEL_NO_NODE) return false;
    uint32_t body = (uint32_t)idm_byteprog_node_count(bp);
    if (!bcompile_node(bp, child, flags, err)) return false;
    uint32_t jump = idm_byteprog_fork(bp, err);
    if (jump == SEL_NO_NODE) return false;
    idm_byteprog_set_fork(bp, jump, split, split);
    uint32_t after = (uint32_t)idm_byteprog_node_count(bp);
    idm_byteprog_set_fork(bp, split, body, after);
    return true;
}

static bool bcompile_optional(SelByteProg *bp, const RxNode *child, uint32_t flags, IdmError *err) {
    uint32_t split = idm_byteprog_fork(bp, err);
    if (split == SEL_NO_NODE) return false;
    uint32_t body = (uint32_t)idm_byteprog_node_count(bp);
    if (!bcompile_node(bp, child, flags, err)) return false;
    uint32_t after = (uint32_t)idm_byteprog_node_count(bp);
    idm_byteprog_set_fork(bp, split, body, after);
    return true;
}

static bool bcompile_repeat(SelByteProg *bp, const RxNode *child, size_t min, size_t max, bool unbounded, uint32_t flags, IdmError *err) {
    if (min > RX_REPEAT_COMPILE_LIMIT || (!unbounded && max > RX_REPEAT_COMPILE_LIMIT)) {
        return idm_error_set(err, idm_span_unknown(NULL), "regex counted repeat expands past %u NFA copies", (unsigned)RX_REPEAT_COMPILE_LIMIT);
    }
    for (size_t i = 0; i < min; i++) {
        if (!bcompile_node(bp, child, flags, err)) return false;
    }
    if (unbounded) return bcompile_star(bp, child, flags, err);
    for (size_t i = min; i < max; i++) {
        if (!bcompile_optional(bp, child, flags, err)) return false;
    }
    return true;
}

static bool bcompile_node(SelByteProg *bp, const RxNode *node, uint32_t flags, IdmError *err) {
    if (!node) return true;
    switch (node->kind) {
        case RX_EMPTY:
            return true;
        case RX_LITERAL: {
            IdmByteClass cls = {0};
            idm_byteclass_add_char(&cls, node->as.literal, (flags & IDM_REGEX_CASELESS) != 0);
            return idm_byteprog_byte(bp, &cls, err) != SEL_NO_NODE;
        }
        case RX_DOT: {
            IdmByteClass cls = {0};
            for (unsigned c = 0; c < 256u; c++) {
                if ((flags & IDM_REGEX_DOTALL) != 0 || c != (unsigned)'\n') idm_byteclass_set(&cls, (unsigned char)c);
            }
            return idm_byteprog_byte(bp, &cls, err) != SEL_NO_NODE;
        }
        case RX_CLASS:
            return idm_byteprog_byte(bp, &node->as.cls, err) != SEL_NO_NODE;
        case RX_ANCHOR_START:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_LINE_START, flags, NULL, err) != SEL_NO_NODE;
        case RX_ANCHOR_END:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_LINE_END, flags, NULL, err) != SEL_NO_NODE;
        case RX_WORD_BOUNDARY:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_WORD_BOUNDARY, flags, NULL, err) != SEL_NO_NODE;
        case RX_NOT_WORD_BOUNDARY:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_NOT_WORD_BOUNDARY, flags, NULL, err) != SEL_NO_NODE;
        case RX_BUFFER_START:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_BUFFER_START, flags, NULL, err) != SEL_NO_NODE;
        case RX_BUFFER_END:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_BUFFER_END, flags, NULL, err) != SEL_NO_NODE;
        case RX_BUFFER_END_NL:
            return idm_byteprog_guard(bp, IDM_BYTE_GUARD_BUFFER_END_NL, flags, NULL, err) != SEL_NO_NODE;
        case RX_CONCAT:
            for (size_t i = 0; i < node->as.seq.count; i++) {
                if (!bcompile_node(bp, node->as.seq.items[i], flags, err)) return false;
            }
            return true;
        case RX_ALT:
            return bcompile_alt_from(bp, node->as.seq.items, 0, node->as.seq.count, flags, err);
        case RX_REPEAT:
            return bcompile_repeat(bp, node->as.repeat.child, node->as.repeat.min, node->as.repeat.max, node->as.repeat.unbounded, flags, err);
        case RX_CAPTURE: {
            if (idm_byteprog_save(bp, (uint32_t)(node->as.capture.index * 2u), err) == SEL_NO_NODE) return false;
            if (!bcompile_node(bp, node->as.capture.child, flags, err)) return false;
            return idm_byteprog_save(bp, (uint32_t)(node->as.capture.index * 2u + 1u), err) != SEL_NO_NODE;
        }
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG: {
            SelByteProg *sub = NULL;
            if (!bcompile_subprog(node->as.child, flags, &sub, err)) return false;
            IdmByteGuardKind gk = node->kind == RX_LOOK_POS ? IDM_BYTE_GUARD_LOOKAHEAD_POS
                                : node->kind == RX_LOOK_NEG ? IDM_BYTE_GUARD_LOOKAHEAD_NEG
                                : node->kind == RX_LOOKBEHIND_POS ? IDM_BYTE_GUARD_LOOKBEHIND_POS
                                : IDM_BYTE_GUARD_LOOKBEHIND_NEG;
            return idm_byteprog_guard(bp, gk, flags, sub, err) != SEL_NO_NODE;
        }
    }
    return true;
}

static bool build_byteprog(const RxNode *root, uint32_t flags, size_t capture_count, SelByteProg **out, IdmError *err) {
    *out = idm_byteprog_new(err);
    if (!*out) return false;
    idm_byteprog_set_flags(*out, flags);
    idm_byteprog_set_capture_count(*out, capture_count);
    if (!bcompile_node(*out, root, flags, err) || idm_byteprog_accept(*out, 0, err) == SEL_NO_NODE) {
        idm_byteprog_free(*out);
        *out = NULL;
        return false;
    }
    idm_byteprog_set_start(*out, 0);
    idm_byteprog_finalize_linear(*out);
    return true;
}

static bool build_byteprog_set(const IdmRegex *const *items, size_t count, size_t capture_count, SelByteProg **out, IdmError *err) {
    *out = idm_byteprog_new(err);
    if (!*out) return false;
    SelByteProg *bp = *out;
    idm_byteprog_set_capture_count(bp, capture_count);
    size_t split_count = count > 1u ? count - 1u : 0u;
    uint32_t *forks = NULL;
    uint32_t *starts = NULL;
    if (split_count != 0) {
        forks = calloc(split_count, sizeof(*forks));
        if (!forks) { idm_byteprog_free(bp); *out = NULL; return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    starts = calloc(count, sizeof(*starts));
    if (!starts) { free(forks); idm_byteprog_free(bp); *out = NULL; return idm_error_oom(err, idm_span_unknown(NULL)); }
    bool ok = true;
    for (size_t i = 0; ok && i < split_count; i++) {
        forks[i] = idm_byteprog_fork(bp, err);
        if (forks[i] == SEL_NO_NODE) ok = false;
    }
    for (size_t i = 0; ok && i < count; i++) {
        starts[i] = (uint32_t)idm_byteprog_node_count(bp);
        if (!bcompile_node(bp, items[i]->root, items[i]->flags, err) || idm_byteprog_accept(bp, (uint32_t)i, err) == SEL_NO_NODE) ok = false;
    }
    for (size_t i = 0; ok && i < split_count; i++) {
        uint32_t second = (i + 1u < split_count) ? forks[i + 1u] : starts[i + 1u];
        idm_byteprog_set_fork(bp, forks[i], starts[i], second);
    }
    if (ok) idm_byteprog_set_start(bp, count > 1u ? forks[0] : starts[0]);
    free(starts);
    free(forks);
    if (!ok) { idm_byteprog_free(bp); *out = NULL; return false; }
    idm_byteprog_finalize_linear(bp);
    return true;
}

static bool compile_regex_program(IdmRegex *rx, IdmError *err) {
    rx->start = start_info_node(rx->root, rx->flags);
    rx->capture_count = rx->group_count + 1u;
    return build_byteprog(rx->root, rx->flags, rx->group_count + 1u, &rx->byteprog, err);
}


void idm_regex_set_free(IdmRegexSet *set) {
    if (!set) return;
    idm_byteprog_free(set->byteprog);
    if (set->group_names) {
        for (size_t i = 0; i < set->count; i++) {
            if (!set->group_names[i]) continue;
            for (size_t g = 0; g <= set->group_counts[i]; g++) free(set->group_names[i][g]);
            free(set->group_names[i]);
        }
        free(set->group_names);
    }
    free(set->group_counts);
    if (set->item_sources) {
        for (size_t i = 0; i < set->count; i++) free(set->item_sources[i]);
        free(set->item_sources);
    }
    free(set->item_source_lens);
    free(set->item_flags);
    free(set);
}

static bool regex_set_copy_sources(IdmRegexSet *set, const IdmRegex *const *items, size_t count, IdmError *err) {
    set->item_sources = calloc(count, sizeof(*set->item_sources));
    set->item_source_lens = calloc(count, sizeof(*set->item_source_lens));
    set->item_flags = calloc(count, sizeof(*set->item_flags));
    if (!set->item_sources || !set->item_source_lens || !set->item_flags) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < count; i++) {
        set->item_source_lens[i] = items[i]->source_len;
        set->item_flags[i] = items[i]->flags;
        set->item_sources[i] = idm_strndup(items[i]->source ? items[i]->source : "", items[i]->source_len);
        if (!set->item_sources[i]) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool regex_set_copy_group_names(IdmRegexSet *set, const IdmRegex *const *items, size_t count, IdmError *err) {
    set->group_counts = calloc(count, sizeof(*set->group_counts));
    set->group_names = calloc(count, sizeof(*set->group_names));
    if (!set->group_counts || !set->group_names) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < count; i++) {
        set->group_counts[i] = items[i]->group_count;
        if (items[i]->group_count == 0) continue;
        set->group_names[i] = calloc(items[i]->group_count + 1u, sizeof(*set->group_names[i]));
        if (!set->group_names[i]) return idm_error_oom(err, idm_span_unknown(NULL));
        for (size_t g = 1; g <= items[i]->group_count; g++) {
            if (!items[i]->group_names[g]) continue;
            set->group_names[i][g] = idm_strdup(items[i]->group_names[g]);
            if (!set->group_names[i][g]) return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    return true;
}

bool idm_regex_set_compile(const IdmRegex *const *items, size_t count, IdmRegexSet **out, IdmError *err) {
    *out = NULL;
    if (count == 0 || !items) return idm_error_set(err, idm_span_unknown(NULL), "regex set requires at least one regex");
    size_t capture_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (!items[i] || !items[i]->byteprog) return idm_error_set(err, idm_span_unknown(NULL), "regex set item is incomplete");
        if (items[i]->capture_count > capture_count) capture_count = items[i]->capture_count;
    }
    IdmRegexSet *set = calloc(1u, sizeof(*set));
    if (!set) return idm_error_oom(err, idm_span_unknown(NULL));
    set->count = count;
    set->capture_count = capture_count;
    if (!regex_set_copy_group_names(set, items, count, err) || !regex_set_copy_sources(set, items, count, err)) {
        idm_regex_set_free(set);
        return false;
    }
    if (!build_byteprog_set(items, count, capture_count, &set->byteprog, err)) {
        idm_regex_set_free(set);
        return false;
    }
    *out = set;
    return true;
}

static bool at_line_start(const char *s, size_t pos) {
    return pos == 0 || s[pos - 1u] == '\n';
}

static size_t align_up_size(size_t value, size_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static IdmRegexResult *result_alloc_inline(size_t capture_count, IdmError *err) {
    size_t capture_offset = align_up_size(sizeof(IdmRegexResult), _Alignof(IdmByteCapture));
    if (capture_count > (SIZE_MAX - capture_offset) / sizeof(IdmByteCapture)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    size_t total = capture_offset + capture_count * sizeof(IdmByteCapture);
    IdmRegexResult *result = calloc(1u, total);
    if (!result) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    char *base = (char *)result;
    result->captures = (IdmByteCapture *)(void *)(base + capture_offset);
    result->capture_count = capture_count;
    result->inline_storage = true;
    result->subject_value = idm_nil();
    return result;
}


static bool rx_search_test(const SelByteProg *bp, const RxStartInfo *start, uint32_t flags, const char *s, size_t len, size_t offset, bool *out_matched, IdmError *err) {
    *out_matched = false;
    size_t scan_off = offset > len ? len : offset;
    for (size_t pos = scan_off; pos <= len; pos++) {
        if (!regex_start_candidate(start, flags, s, len, pos)) continue;
        bool m = false;
        if (!idm_byteprog_test(bp, s, len, pos, false, 0, &m, err)) return false;
        if (m) { *out_matched = true; return true; }
    }
    return true;
}

static bool result_set_subject(IdmRegexResult *result, IdmValue subject, const char *s, size_t len, IdmError *err) {
    result->subject_len = len;
    if (idm_value_tag(subject) == IDM_VAL_STRING && idm_string_bytes(subject) == s && idm_string_length(subject) == len) {
        result->subject_value = subject;
        result->subject = (char *)s;
        result->owns_subject = false;
        return true;
    }
    result->subject = idm_strndup(s, len);
    if (!result->subject) return idm_error_oom(err, idm_span_unknown(NULL));
    result->subject_value = idm_nil();
    result->owns_subject = true;
    return true;
}

static IdmRegexResult *result_new(const IdmRegex *rx, IdmValue subject, const char *s, size_t len, size_t start, const IdmByteMatch *match, IdmError *err) {
    size_t capture_count = rx->group_count + 1u;
    IdmRegexResult *result = result_alloc_inline(capture_count, err);
    if (!result) return NULL;
    if (!result_set_subject(result, subject, s, len, err)) {
        idm_regex_result_free(result);
        return NULL;
    }
    memcpy(result->captures, match->captures, result->capture_count * sizeof(*result->captures));
    result->captures[0].set = true;
    result->captures[0].start = start;
    result->captures[0].end = match->end;
    bool has_group_names = false;
    for (size_t i = 1; i < result->capture_count; i++) {
        if (rx->group_names[i]) {
            has_group_names = true;
            break;
        }
    }
    if (has_group_names) {
        result->group_names = calloc(result->capture_count, sizeof(*result->group_names));
        if (!result->group_names) {
            idm_regex_result_free(result);
            idm_error_oom(err, idm_span_unknown(NULL));
            return NULL;
        }
    }
    for (size_t i = 1; has_group_names && i < result->capture_count; i++) {
        if (rx->group_names[i]) {
            result->group_names[i] = idm_strdup(rx->group_names[i]);
            if (!result->group_names[i]) {
                idm_regex_result_free(result);
                idm_error_oom(err, idm_span_unknown(NULL));
                return NULL;
            }
        }
    }
    return result;
}

static bool scan_at_raw(const IdmRegex *rx, IdmValue subject, const char *s, size_t len, size_t offset, bool full, IdmRegexResult **out, IdmError *err) {
    *out = NULL;
    if (offset > len) return true;
    IdmByteMatch match = {0};
    bool ok = idm_byteprog_match(rx->byteprog, s, len, offset, full, len, true, &match, err);
    if (!ok) return false;
    if (match.matched) *out = result_new(rx, subject, s, len, offset, &match, err);
    idm_byte_match_destroy(&match);
    return !(err && err->present);
}

static bool regex_start_candidate(const RxStartInfo *start, uint32_t flags, const char *s, size_t len, size_t pos) {
    if (!start) return false;
    if (start->anchored_start && !(pos == 0 || ((flags & IDM_REGEX_MULTILINE) != 0 && at_line_start(s, pos)))) return false;
    if (pos >= len) return start->nullable;
    if (start->any || start->nullable) return true;
    return start->bytes[(unsigned char)s[pos]];
}

static bool scan_from_raw(const IdmRegex *rx, IdmValue subject, const char *s, size_t len, size_t offset, IdmRegexResult **out, IdmError *err) {
    *out = NULL;
    if (offset > len) offset = len;
    for (size_t pos = offset; pos <= len; pos++) {
        if (!regex_start_candidate(&rx->start, rx->flags, s, len, pos)) continue;
        if (!scan_at_raw(rx, subject, s, len, pos, false, out, err)) return false;
        if (*out) return true;
    }
    return true;
}

bool idm_regex_test_bytes(const IdmRegex *rx, const char *input, size_t input_len, bool *out_matched, IdmError *err) {
    if (!rx) {
        *out_matched = false;
        return true;
    }
    return rx_search_test(rx->byteprog, &rx->start, rx->flags, input, input_len, 0, out_matched, err);
}

bool idm_regex_match_at(const IdmRegex *rx, const char *input, size_t input_len, size_t offset, size_t *out_end, IdmError *err) {
    if (out_end) *out_end = offset;
    if (!rx) return true;
    IdmByteMatch match = {0};
    bool ok = idm_byteprog_match(rx->byteprog, input, input_len, offset, false, 0, false, &match, err);
    if (ok && match.matched && out_end) *out_end = match.end;
    idm_byte_match_destroy(&match);
    return ok;
}

bool idm_regex_set_match_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, size_t *out_index, size_t *out_end, bool *out_matched, IdmError *err) {
    if (out_index) *out_index = 0;
    if (out_end) *out_end = offset;
    if (out_matched) *out_matched = false;
    if (!set || !set->byteprog) return true;
    IdmByteMatch match = {0};
    bool ok = idm_byteprog_match(set->byteprog, input, input_len, offset, false, 0, false, &match, err);
    if (ok && match.matched) {
        if (match.index >= set->count) {
            idm_byte_match_destroy(&match);
            return idm_error_set(err, idm_span_unknown(NULL), "regex set accept id out of bounds");
        }
        if (out_index) *out_index = match.index;
        if (out_end) *out_end = match.end;
        if (out_matched) *out_matched = true;
    }
    idm_byte_match_destroy(&match);
    return ok;
}

bool idm_regex_set_exec_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, IdmByteMatch *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "regex.set_exec_at");
    if (!out) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "regex set result is required");
    }
    memset(out, 0, sizeof(*out));
    out->end = offset;
    if (!set || !set->byteprog) {
        idm_profile_scope_end(&prof);
        return true;
    }
    IdmByteMatch match = {0};
    bool ok = idm_byteprog_match(set->byteprog, input, input_len, offset, false, 0, true, &match, err);
    if (ok && match.matched) {
        if (match.index >= set->count) {
            idm_byte_match_destroy(&match);
            idm_profile_scope_end(&prof);
            return idm_error_set(err, idm_span_unknown(NULL), "regex set accept id out of bounds");
        }
        size_t group_count = idm_regex_set_group_count(set, match.index);
        size_t capture_count = group_count + 1u;
        IdmByteCapture *captures = calloc(capture_count, sizeof(*captures));
        if (!captures) {
            idm_byte_match_destroy(&match);
            idm_profile_scope_end(&prof);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        captures[0].set = true;
        captures[0].start = offset;
        captures[0].end = match.end;
        for (size_t i = 1; i < capture_count; i++) {
            if (!match.captures || i >= match.capture_count || !match.captures[i].set) continue;
            captures[i] = match.captures[i];
        }
        out->matched = true;
        out->index = match.index;
        out->end = match.end;
        out->captures = captures;
        out->capture_count = capture_count;
    }
    idm_byte_match_destroy(&match);
    idm_profile_count("regex.set_exec_at.items", set ? (uint64_t)set->count : 0u);
    idm_profile_count("regex.set_exec_at.bytes_remaining", offset <= input_len ? (uint64_t)(input_len - offset) : 0u);
    idm_profile_scope_end(&prof);
    return ok;
}

size_t idm_regex_set_count(const IdmRegexSet *set) {
    return set ? set->count : 0;
}

bool idm_regex_set_matches_empty(const IdmRegexSet *set, bool *out, IdmError *err) {
    if (out) *out = false;
    if (!set || !set->byteprog) return true;
    IdmByteMatch match = {0};
    bool ok = idm_byteprog_match(set->byteprog, "", 0, 0, false, 0, false, &match, err);
    if (ok && out) *out = match.matched && match.end == 0;
    idm_byte_match_destroy(&match);
    return ok;
}

bool idm_regex_set_serialize(IdmBuffer *out, const IdmRegexSet *set, IdmError *err) {
    if (!set || !set->byteprog || !set->item_sources || set->count == 0 || set->count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "regex set is incomplete");
    if (!idm_buf_put_u32(out, (uint32_t)set->count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < set->count; i++) {
        if (!idm_buf_put_u32(out, set->item_flags[i]) ||
            !idm_buf_put_str(out, set->item_sources[i], set->item_source_lens[i])) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}


bool idm_regex_exec_at_subject(const IdmRegex *rx, IdmValue subject, const char *input, size_t input_len, size_t offset, bool full, IdmRegexResult **out, IdmError *err) {
    if (!rx) {
        *out = NULL;
        return true;
    }
    return scan_at_raw(rx, subject, input, input_len, offset, full, out, err);
}

bool idm_regex_scan_subject(const IdmRegex *rx, IdmValue subject, const char *input, size_t input_len, size_t offset, IdmRegexResult **out, IdmError *err) {
    if (!rx) {
        *out = NULL;
        return true;
    }
    return scan_from_raw(rx, subject, input, input_len, offset, out, err);
}

static bool wrap_result_or_nil(IdmRuntime *rt, IdmRegexResult *result, IdmValue *out, IdmError *err) {
    if (!result) {
        *out = idm_nil();
        return true;
    }
    *out = idm_regex_result_value(rt, result, err);
    return !(err && err->present);
}

static bool regex_and_input(IdmRuntime *rt, const char *name, IdmValue regex, IdmValue input, IdmRegex **out_rx, const char **out_s, size_t *out_len, IdmError *err) {
    (void)rt;
    *out_rx = idm_regex_value_get(regex, err);
    if (!*out_rx) return false;
    return idm_primitive_require_string_arg(rt, input, out_s, out_len, name, err);
}

bool idm_regex_compile(const char *source, size_t source_len, uint32_t flags, IdmRegex **out, IdmError *err) {
    IdmRegex *rx = calloc(1u, sizeof(*rx));
    if (!rx) return idm_error_oom(err, idm_span_unknown(NULL));
    rx->source = idm_strndup(source, source_len);
    if (!rx->source) {
        idm_regex_free(rx);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    rx->source_len = source_len;
    rx->flags = flags;

    RxParser p = { .source = source, .len = source_len, .pos = 0, .flags = flags, .group_names = NULL, .group_count = 0, .group_cap = 1u, .err = err, .depth = 0 };
    p.group_names = calloc(1u, sizeof(*p.group_names));
    if (!p.group_names) {
        idm_regex_free(rx);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    bool ok = parse_alt(&p, &rx->root);
    if (ok && p.pos != p.len) ok = parser_error(&p, "unexpected trailing regex syntax");
    if (!ok) {
        for (size_t i = 0; i <= p.group_count; i++) free(p.group_names[i]);
        free(p.group_names);
        idm_regex_free(rx);
        return false;
    }
    rx->group_names = p.group_names;
    rx->group_count = p.group_count;
    if (!compile_regex_program(rx, err)) {
        idm_regex_free(rx);
        return false;
    }
    *out = rx;
    return true;
}

IdmRegex *idm_regex_clone(const IdmRegex *rx, IdmError *err) {
    if (!rx) return NULL;
    IdmRegex *copy = NULL;
    if (!idm_regex_compile(rx->source, rx->source_len, rx->flags, &copy, err)) return NULL;
    return copy;
}

void idm_regex_free(IdmRegex *rx) {
    if (!rx) return;
    free(rx->source);
    node_free(rx->root);
    idm_byteprog_free(rx->byteprog);
    if (rx->group_names) {
        for (size_t i = 0; i <= rx->group_count; i++) free(rx->group_names[i]);
        free(rx->group_names);
    }
    free(rx);
}

size_t idm_regex_footprint(const IdmRegex *rx) {
    if (!rx) return 0;
    size_t total = sizeof(*rx) + rx->source_len + 1u + node_footprint(rx->root) + idm_byteprog_footprint(rx->byteprog) + (rx->group_count + 1u) * sizeof(*rx->group_names);
    for (size_t i = 0; i <= rx->group_count; i++) if (rx->group_names[i]) total += strlen(rx->group_names[i]) + 1u;
    return total;
}

const char *idm_regex_source(const IdmRegex *rx, size_t *out_len) {
    if (out_len) *out_len = rx ? rx->source_len : 0;
    return rx ? rx->source : "";
}

uint32_t idm_regex_flags(const IdmRegex *rx) {
    return rx ? rx->flags : 0;
}

size_t idm_regex_group_count(const IdmRegex *rx) {
    return rx ? rx->group_count : 0;
}

const char *idm_regex_group_name(const IdmRegex *rx, size_t index) {
    if (!rx || index > rx->group_count) return NULL;
    return rx->group_names[index];
}

bool idm_regex_nullable(const IdmRegex *rx) {
    return rx && rx->byteprog && rx->start.nullable;
}

size_t idm_regex_set_group_count(const IdmRegexSet *set, size_t item_index) {
    if (!set || item_index >= set->count || !set->group_counts) return 0;
    return set->group_counts[item_index];
}

const char *idm_regex_set_group_name(const IdmRegexSet *set, size_t item_index, size_t group_index) {
    if (!set || item_index >= set->count || !set->group_names || !set->group_counts || group_index > set->group_counts[item_index]) return NULL;
    return set->group_names[item_index] ? set->group_names[item_index][group_index] : NULL;
}

void idm_regex_result_free(IdmRegexResult *result) {
    if (!result) return;
    if (result->owns_subject) free(result->subject);
    if (!result->inline_storage) {
        free(result->captures);
    }
    if (result->group_names) {
        for (size_t i = 0; i < result->capture_count; i++) free(result->group_names[i]);
        free(result->group_names);
    }
    free(result);
}

IdmRegexResult *idm_regex_result_clone_with_subject(const IdmRegexResult *src, IdmValue subject, IdmError *err) {
    if (!src) return NULL;
    IdmRegexResult *r = result_alloc_inline(src->capture_count, err);
    if (!r) return NULL;
    const char *subject_bytes = src->subject ? src->subject : "";
    size_t subject_len = src->subject ? src->subject_len : 0u;
    if (idm_value_tag(subject) == IDM_VAL_STRING) {
        subject_bytes = idm_string_bytes(subject);
        subject_len = idm_string_length(subject);
    }
    if (!result_set_subject(r, subject, subject_bytes, subject_len, err)) {
        idm_regex_result_free(r);
        return NULL;
    }
    if (src->capture_count != 0 && src->captures)
        memcpy(r->captures, src->captures, src->capture_count * sizeof(*r->captures));
    if (src->group_names) {
        r->group_names = calloc(src->capture_count, sizeof(*r->group_names));
        if (!r->group_names) { idm_regex_result_free(r); idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
        for (size_t i = 0; i < src->capture_count; i++) {
            if (!src->group_names[i]) continue;
            r->group_names[i] = idm_strdup(src->group_names[i]);
            if (!r->group_names[i]) { idm_regex_result_free(r); idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
        }
    }
    return r;
}

IdmRegexResult *idm_regex_result_clone(const IdmRegexResult *src) {
    IdmError err;
    idm_error_init(&err);
    IdmRegexResult *copy = idm_regex_result_clone_with_subject(src, idm_nil(), &err);
    idm_error_clear(&err);
    return copy;
}

size_t idm_regex_result_footprint(const IdmRegexResult *result) {
    if (!result) return 0;
    size_t total = sizeof(*result) + result->capture_count * sizeof(*result->captures);
    if (result->owns_subject) total += result->subject_len + 1u;
    if (result->group_names) {
        total += result->capture_count * sizeof(*result->group_names);
        for (size_t i = 0; i < result->capture_count; i++) if (result->group_names[i]) total += strlen(result->group_names[i]) + 1u;
    }
    return total;
}

IdmValue idm_regex_result_subject_value(const IdmRegexResult *result) {
    if (!result) return idm_nil();
    return result->subject_value;
}

bool idm_regex_compile_value(IdmRuntime *rt, IdmValue source, IdmValue options, IdmValue *out, IdmError *err) {
    const char *s = NULL;
    size_t len = 0;
    if (!idm_primitive_require_string_arg(rt, source, &s, &len, "raw-compile", err)) return false;
    uint32_t flags = 0;
    if (!parse_options(rt, options, &flags, err)) return false;
    IdmError inner;
    idm_error_init(&inner);
    IdmRegex *rx = NULL;
    bool ok = idm_regex_compile(s, len, flags, &rx, &inner);
    if (!ok) {
        bool built = regex_compile_error_value(rt, inner.message, source, out, err);
        idm_error_clear(&inner);
        return built;
    }
    idm_error_clear(&inner);
    IdmValue wrapped = idm_regex_value(rt, rx, err);
    if (err && err->present) return false;
    return result_tuple(rt, idm_atom(rt, "ok"), wrapped, out, err);
}

bool idm_regex_options_value(IdmRuntime *rt, IdmValue regex, IdmValue *out, IdmError *err) {
    IdmRegex *rx = idm_regex_value_get(regex, err);
    if (!rx) return false;
    IdmValue result = idm_empty_list();
    uint32_t flags = idm_regex_flags(rx);
    if (flags & IDM_REGEX_DOTALL) {
        result = idm_cons(rt, idm_atom(rt, "dotall"), result, err);
        if (err && err->present) return false;
    }
    if (flags & IDM_REGEX_MULTILINE) {
        result = idm_cons(rt, idm_atom(rt, "multiline"), result, err);
        if (err && err->present) return false;
    }
    if (flags & IDM_REGEX_CASELESS) {
        result = idm_cons(rt, idm_atom(rt, "caseless"), result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

bool idm_regex_group_names_value(IdmRuntime *rt, IdmValue regex, IdmValue *out, IdmError *err) {
    IdmRegex *rx = idm_regex_value_get(regex, err);
    if (!rx) return false;
    IdmValue result = idm_empty_list();
    for (size_t i = idm_regex_group_count(rx); i > 0; i--) {
        const char *name = idm_regex_group_name(rx, i);
        IdmValue item = name ? idm_string(rt, name, err) : idm_nil();
        if (err && err->present) return false;
        result = idm_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

bool idm_regex_scan_at(IdmRuntime *rt, IdmValue regex, IdmValue input, size_t offset, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-scan-at", regex, input, &rx, &s, &len, err)) return false;
    IdmRegexResult *result = NULL;
    if (!idm_regex_exec_at_subject(rx, input, s, len, offset, false, &result, err)) return false;
    return wrap_result_or_nil(rt, result, out, err);
}

bool idm_regex_scan_from(IdmRuntime *rt, IdmValue regex, IdmValue input, size_t offset, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-scan-from", regex, input, &rx, &s, &len, err)) return false;
    IdmRegexResult *result = NULL;
    if (!idm_regex_scan_subject(rx, input, s, len, offset, &result, err)) return false;
    return wrap_result_or_nil(rt, result, out, err);
}

bool idm_regex_scan_full(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-scan-full", regex, input, &rx, &s, &len, err)) return false;
    IdmRegexResult *result = NULL;
    if (!idm_regex_exec_at_subject(rx, input, s, len, 0, true, &result, err)) return false;
    return wrap_result_or_nil(rt, result, out, err);
}

bool idm_regex_test(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-test?", regex, input, &rx, &s, &len, err)) return false;
    bool matched = false;
    if (!idm_regex_test_bytes(rx, s, len, &matched, err)) return false;
    *out = idm_bool(rt, matched);
    return true;
}

bool idm_regex_scan_all(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-scan-all", regex, input, &rx, &s, &len, err)) return false;
    IdmValue acc = idm_empty_list();
    size_t pos = 0;
    while (pos <= len) {
        IdmRegexResult *result = NULL;
        if (!idm_regex_scan_subject(rx, input, s, len, pos, &result, err)) return false;
        if (!result) break;
        size_t start = result->captures[0].start;
        size_t end = result->captures[0].end;
        IdmValue wrapped = idm_regex_result_value(rt, result, err);
        if (err && err->present) return false;
        acc = idm_cons(rt, wrapped, acc, err);
        if (err && err->present) return false;
        if (end <= start) pos = start < len ? start + 1u : len + 1u;
        else pos = end;
    }
    IdmValue rev = idm_empty_list();
    for (IdmValue cur = acc; idm_is_pair(cur); cur = idm_cdr(cur, err)) {
        rev = idm_cons(rt, idm_car(cur, err), rev, err);
        if (err && err->present) return false;
    }
    *out = rev;
    return true;
}

static IdmRegexResult *require_result(IdmValue value, IdmError *err) {
    return idm_regex_result_value_get(value, err);
}

bool idm_regex_result_start_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    *out = idm_int((int64_t)r->captures[0].start);
    return true;
}

bool idm_regex_result_end_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    *out = idm_int((int64_t)r->captures[0].end);
    return true;
}

bool idm_regex_result_text_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err) {
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    IdmByteCapture c = r->captures[0];
    *out = idm_string_n(rt, r->subject + c.start, c.end - c.start, err);
    return !(err && err->present);
}

static bool capture_index(IdmValue index, size_t max, size_t *out, IdmError *err) {
    int64_t i = 0;
    if (!idm_value_is_int(index) || !idm_int_to_i64(index, &i) || i < 0 || (uint64_t)i >= max) {
        return idm_error_set(err, idm_span_unknown(NULL), "capture index out of range");
    }
    *out = (size_t)i;
    return true;
}

static bool capture_by_name(IdmRegexResult *r, IdmValue name, size_t *out, IdmError *err) {
    const char *text = NULL;
    if (idm_value_tag(name) == IDM_VAL_ATOM || idm_value_tag(name) == IDM_VAL_WORD) text = idm_symbol_text(idm_value_symbol(name));
    else if (idm_value_tag(name) == IDM_VAL_STRING) text = idm_string_bytes(name);
    if (!text) return idm_error_set(err, idm_span_unknown(NULL), "capture name must be an atom, word, or string");
    if (!r->group_names) return idm_error_set(err, idm_span_unknown(NULL), "unknown capture name '%s'", text);
    for (size_t i = 1; i < r->capture_count; i++) {
        if (r->group_names[i] && strcmp(r->group_names[i], text) == 0) {
            *out = i;
            return true;
        }
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unknown capture name '%s'", text);
}

static bool capture_text(IdmRuntime *rt, IdmRegexResult *r, size_t index, IdmValue *out, IdmError *err) {
    if (!r->captures[index].set) {
        *out = idm_nil();
        return true;
    }
    IdmByteCapture c = r->captures[index];
    *out = idm_string_n(rt, r->subject + c.start, c.end - c.start, err);
    return !(err && err->present);
}

bool idm_regex_capture_value(IdmRuntime *rt, IdmValue result, IdmValue index, IdmValue *out, IdmError *err) {
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    size_t i = 0;
    if (!capture_index(index, r->capture_count, &i, err)) return false;
    return capture_text(rt, r, i, out, err);
}

bool idm_regex_capture_range_value(IdmRuntime *rt, IdmValue result, IdmValue index, IdmValue *out, IdmError *err) {
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    size_t i = 0;
    if (!capture_index(index, r->capture_count, &i, err)) return false;
    if (!r->captures[i].set) {
        *out = idm_nil();
        return true;
    }
    IdmValue items[2] = { idm_int((int64_t)r->captures[i].start), idm_int((int64_t)r->captures[i].end) };
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}

bool idm_regex_capture_named_value(IdmRuntime *rt, IdmValue result, IdmValue name, IdmValue *out, IdmError *err) {
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    size_t i = 0;
    if (!capture_by_name(r, name, &i, err)) return false;
    return capture_text(rt, r, i, out, err);
}

bool idm_regex_captures_value(IdmRuntime *rt, IdmValue result, IdmValue *out, IdmError *err) {
    IdmRegexResult *r = require_result(result, err);
    if (!r) return false;
    IdmValue acc = idm_empty_list();
    for (size_t i = r->capture_count; i > 1u; i--) {
        IdmValue item = idm_nil();
        if (!capture_text(rt, r, i - 1u, &item, err)) return false;
        acc = idm_cons(rt, item, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool append_capture(IdmBuffer *buf, IdmRegexResult *r, size_t index) {
    if (index >= r->capture_count || !r->captures[index].set) return true;
    IdmByteCapture c = r->captures[index];
    return idm_buf_append_n(buf, r->subject + c.start, c.end - c.start);
}

static bool append_named_capture(IdmBuffer *buf, IdmRegexResult *r, const char *name) {
    if (!r->group_names) return true;
    for (size_t i = 1; i < r->capture_count; i++) {
        if (r->group_names[i] && strcmp(r->group_names[i], name) == 0) return append_capture(buf, r, i);
    }
    return true;
}

static bool append_replacement(IdmBuffer *buf, IdmRegexResult *r, const char *replacement, size_t repl_len) {
    for (size_t i = 0; i < repl_len; i++) {
        unsigned char ch = (unsigned char)replacement[i];
        if ((ch == '$' || ch == '\\') && i + 1u < repl_len) {
            unsigned char n = (unsigned char)replacement[i + 1u];
            if (isdigit(n)) {
                i++;
                if (!append_capture(buf, r, (size_t)(n - '0'))) return false;
                continue;
            }
            if (ch == '$' && n == '{') {
                size_t j = i + 2u;
                while (j < repl_len && replacement[j] != '}') j++;
                if (j < repl_len) {
                    char *name = idm_strndup(replacement + i + 2u, j - (i + 2u));
                    if (!name) return false;
                    bool ok = append_named_capture(buf, r, name);
                    free(name);
                    if (!ok) return false;
                    i = j;
                    continue;
                }
            }
            if (ch == '\\' && n == 'k' && i + 2u < repl_len && replacement[i + 2u] == '<') {
                size_t j = i + 3u;
                while (j < repl_len && replacement[j] != '>') j++;
                if (j < repl_len) {
                    char *name = idm_strndup(replacement + i + 3u, j - (i + 3u));
                    if (!name) return false;
                    bool ok = append_named_capture(buf, r, name);
                    free(name);
                    if (!ok) return false;
                    i = j;
                    continue;
                }
            }
            if ((ch == '$' && n == '$') || (ch == '\\' && n == '\\')) {
                i++;
                if (!idm_buf_append_char(buf, (char)n)) return false;
                continue;
            }
        }
        if (!idm_buf_append_char(buf, (char)ch)) return false;
    }
    return true;
}

bool idm_regex_replace(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue replacement, bool all, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-replace", regex, input, &rx, &s, &len, err)) return false;
    const char *repl = NULL;
    size_t repl_len = 0;
    if (!idm_primitive_require_string_arg(rt, replacement, &repl, &repl_len, "raw-replace", err)) return false;
    IdmBuffer buf;
    idm_buf_init(&buf);
    size_t pos = 0;
    bool changed = false;
    while (pos <= len) {
        IdmRegexResult *result = NULL;
        if (!scan_from_raw(rx, input, s, len, pos, &result, err)) {
            idm_buf_destroy(&buf);
            return false;
        }
        if (!result) {
            if (changed && !idm_buf_append_n(&buf, s + pos, len - pos)) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
            break;
        }
        IdmByteCapture whole = result->captures[0];
        if (!idm_buf_append_n(&buf, s + pos, whole.start - pos) || !append_replacement(&buf, result, repl, repl_len)) {
            idm_regex_result_free(result);
            idm_buf_destroy(&buf);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        changed = true;
        size_t next = whole.end;
        idm_regex_result_free(result);
        if (!all) {
            if (!idm_buf_append_n(&buf, s + next, len - next)) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
            pos = len + 1u;
            break;
        }
        if (next <= whole.start) {
            if (next < len && !idm_buf_append_char(&buf, s[next])) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
            pos = next < len ? next + 1u : len + 1u;
        } else {
            pos = next;
        }
    }
    if (!changed) {
        idm_buf_destroy(&buf);
        *out = input;
        return true;
    }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

bool idm_regex_split_on(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "raw-split-on", regex, input, &rx, &s, &len, err)) return false;
    IdmValue acc = idm_empty_list();
    size_t segment_start = 0;
    size_t scan_pos = 0;
    while (scan_pos <= len) {
        IdmRegexResult *result = NULL;
        if (!scan_from_raw(rx, input, s, len, scan_pos, &result, err)) return false;
        if (!result) {
            IdmValue tail = idm_string_n(rt, s + segment_start, len - segment_start, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, tail, acc, err);
            if (err && err->present) return false;
            break;
        }
        IdmByteCapture whole = result->captures[0];
        IdmValue part = idm_string_n(rt, s + segment_start, whole.start - segment_start, err);
        if (err && err->present) { idm_regex_result_free(result); return false; }
        acc = idm_cons(rt, part, acc, err);
        if (err && err->present) { idm_regex_result_free(result); return false; }
        size_t next = whole.end;
        idm_regex_result_free(result);
        if (next <= whole.start) {
            segment_start = next < len ? next + 1u : len;
            scan_pos = next < len ? next + 1u : len + 1u;
        } else {
            segment_start = next;
            scan_pos = next;
        }
    }
    IdmValue rev = idm_empty_list();
    for (IdmValue cur = acc; idm_is_pair(cur); cur = idm_cdr(cur, err)) {
        rev = idm_cons(rt, idm_car(cur, err), rev, err);
        if (err && err->present) return false;
    }
    *out = rev;
    return true;
}

bool idm_regex_escape(IdmRuntime *rt, IdmValue input, IdmValue *out, IdmError *err) {
    const char *s = NULL;
    size_t len = 0;
    if (!idm_primitive_require_string_arg(rt, input, &s, &len, "raw-escape", err)) return false;
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (strchr(".^$|()[]{}*+?\\", c) && !idm_buf_append_char(&buf, '\\')) {
            idm_buf_destroy(&buf);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        if (!idm_buf_append_char(&buf, c)) {
            idm_buf_destroy(&buf);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}
