#include "idiom/regex.h"

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

typedef struct {
    unsigned char bits[32];
    bool negated;
} RxClass;

struct RxNode {
    RxNodeKind kind;
    union {
        unsigned char literal;
        RxClass cls;
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
    bool set;
    size_t start;
    size_t end;
} RxCapture;

struct IdmRegex {
    char *source;
    size_t source_len;
    uint32_t flags;
    RxNode *root;
    char **group_names;
    size_t group_count;
};

struct IdmRegexResult {
    char *subject;
    size_t subject_len;
    RxCapture *captures;
    char **group_names;
    size_t capture_count;
};

typedef struct {
    const char *source;
    size_t len;
    size_t pos;
    uint32_t flags;
    char **group_names;
    size_t group_count;
    IdmError *err;
} RxParser;

typedef struct {
    size_t pos;
    RxCapture *captures;
} RxState;

typedef struct {
    RxState *items;
    size_t count;
    size_t cap;
    size_t capture_count;
} RxStateVec;

static void node_free(RxNode *node);
static bool parse_alt(RxParser *p, RxNode **out);
static bool match_node(const IdmRegex *rx, const RxNode *node, const char *s, size_t len, const RxState *state, RxStateVec *out, IdmError *err);

static bool require_string_arg(IdmRuntime *rt, const char *name, IdmValue v, const char **out, size_t *out_len, IdmError *err) {
    if (v.tag != IDM_VAL_STRING) {
        idm_error_set(err, idm_span_unknown(NULL), "%s expects a string", name);
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, name), v);
    }
    *out = idm_string_bytes(v);
    if (out_len) *out_len = idm_string_length(v);
    return true;
}

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
        if (item.tag != IDM_VAL_ATOM && item.tag != IDM_VAL_STRING) {
            idm_error_set(err, idm_span_unknown(NULL), "regex option must be an atom or string");
            return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "regex-raw-compile"), item);
        }
        const char *text = item.tag == IDM_VAL_ATOM ? idm_symbol_text(item.as.symbol) : idm_string_bytes(item);
        if (!parse_flag_atom(text, flags)) return idm_error_set(err, idm_span_unknown(NULL), "unknown regex option '%s'", text);
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "regex options must be an atom or proper list");
    return true;
}

static bool parse_options(IdmRuntime *rt, IdmValue options, uint32_t *out_flags, IdmError *err) {
    uint32_t flags = 0;
    if (options.tag == IDM_VAL_NIL || idm_is_empty_list(options)) {
        *out_flags = 0;
        return true;
    }
    if (options.tag == IDM_VAL_ATOM || options.tag == IDM_VAL_STRING) {
        const char *text = options.tag == IDM_VAL_ATOM ? idm_symbol_text(options.as.symbol) : idm_string_bytes(options);
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
            if (item.tag != IDM_VAL_ATOM && item.tag != IDM_VAL_STRING) return idm_error_set(err, idm_span_unknown(NULL), "regex option must be an atom or string");
            const char *text = item.tag == IDM_VAL_ATOM ? idm_symbol_text(item.as.symbol) : idm_string_bytes(item);
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
        size_t cap = vec->cap ? vec->cap * 2u : 4u;
        RxNode **items = realloc(vec->items, cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = cap;
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

static void cls_set(RxClass *cls, unsigned char c) {
    cls->bits[c / 8u] |= (unsigned char)(1u << (c % 8u));
}

static bool cls_has(const RxClass *cls, unsigned char c) {
    bool in = (cls->bits[c / 8u] & (unsigned char)(1u << (c % 8u))) != 0;
    return cls->negated ? !in : in;
}

static void cls_add_char(RxClass *cls, unsigned char c, bool caseless) {
    cls_set(cls, c);
    if (caseless && isalpha(c)) {
        cls_set(cls, (unsigned char)tolower(c));
        cls_set(cls, (unsigned char)toupper(c));
    }
}

static void cls_add_range(RxClass *cls, unsigned char lo, unsigned char hi, bool caseless) {
    if (lo > hi) {
        unsigned char tmp = lo;
        lo = hi;
        hi = tmp;
    }
    for (unsigned i = lo; i <= hi; i++) cls_add_char(cls, (unsigned char)i, caseless);
}

static void cls_add_pred(RxClass *cls, int (*pred)(int), bool caseless) {
    for (unsigned i = 0; i < 256u; i++) {
        if (pred((int)i)) cls_add_char(cls, (unsigned char)i, caseless);
    }
}

static int pred_word(int c) {
    return isalnum(c) || c == '_';
}

static bool cls_add_named(RxClass *cls, const char *name, bool caseless) {
    if (strcmp(name, "alnum") == 0) cls_add_pred(cls, isalnum, caseless);
    else if (strcmp(name, "alpha") == 0) cls_add_pred(cls, isalpha, caseless);
    else if (strcmp(name, "blank") == 0) { cls_add_char(cls, ' ', caseless); cls_add_char(cls, '\t', caseless); }
    else if (strcmp(name, "cntrl") == 0) cls_add_pred(cls, iscntrl, caseless);
    else if (strcmp(name, "digit") == 0) cls_add_pred(cls, isdigit, caseless);
    else if (strcmp(name, "graph") == 0) cls_add_pred(cls, isgraph, caseless);
    else if (strcmp(name, "lower") == 0) cls_add_pred(cls, islower, caseless);
    else if (strcmp(name, "print") == 0) cls_add_pred(cls, isprint, caseless);
    else if (strcmp(name, "punct") == 0) cls_add_pred(cls, ispunct, caseless);
    else if (strcmp(name, "space") == 0) cls_add_pred(cls, isspace, caseless);
    else if (strcmp(name, "upper") == 0) cls_add_pred(cls, isupper, caseless);
    else if (strcmp(name, "word") == 0) cls_add_pred(cls, pred_word, caseless);
    else if (strcmp(name, "xdigit") == 0) cls_add_pred(cls, isxdigit, caseless);
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
    switch (c) {
        case 'n': *out = literal_node('\n'); return *out != NULL;
        case 'r': *out = literal_node('\r'); return *out != NULL;
        case 't': *out = literal_node('\t'); return *out != NULL;
        case 'd': *out = class_node_named("digit", false, regex_is_caseless(p)); return *out != NULL;
        case 'D': *out = class_node_named("digit", true, regex_is_caseless(p)); return *out != NULL;
        case 's': *out = class_node_named("space", false, regex_is_caseless(p)); return *out != NULL;
        case 'S': *out = class_node_named("space", true, regex_is_caseless(p)); return *out != NULL;
        case 'w': *out = class_node_named("word", false, regex_is_caseless(p)); return *out != NULL;
        case 'W': *out = class_node_named("word", true, regex_is_caseless(p)); return *out != NULL;
        case 'A': *out = node_new(RX_ANCHOR_START); return *out != NULL;
        case 'z': *out = node_new(RX_ANCHOR_END); return *out != NULL;
        default: *out = literal_node((unsigned char)c); return *out != NULL;
    }
}

static bool parse_class_char(RxParser *p, RxClass *cls, bool *out_is_char, unsigned char *out_ch) {
    int c = parser_take(p);
    if (c < 0) return parser_error(p, "unterminated character class");
    if (c == '\\') {
        int e = parser_take(p);
        if (e < 0) return parser_error(p, "trailing class escape");
        switch (e) {
            case 'n': *out_is_char = true; *out_ch = '\n'; return true;
            case 'r': *out_is_char = true; *out_ch = '\r'; return true;
            case 't': *out_is_char = true; *out_ch = '\t'; return true;
            case 'd': cls_add_named(cls, "digit", regex_is_caseless(p)); *out_is_char = false; return true;
            case 'D': {
                RxClass tmp = {0};
                tmp.negated = true;
                cls_add_named(&tmp, "digit", regex_is_caseless(p));
                for (unsigned i = 0; i < 256u; i++) if (cls_has(&tmp, (unsigned char)i)) cls_add_char(cls, (unsigned char)i, regex_is_caseless(p));
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

static bool parse_posix_class(RxParser *p, RxClass *cls, bool *out_done) {
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
            for (size_t j = 0; name[j] != '\0'; j++) cls_add_char(cls, (unsigned char)name[j], regex_is_caseless(p));
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
            cls_add_range(&node->as.cls, lo, hi, regex_is_caseless(p));
        } else if (is_char) {
            cls_add_char(&node->as.cls, lo, regex_is_caseless(p));
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
    char **names = realloc(p->group_names, (next + 1u) * sizeof(*names));
    if (!names) return parser_error(p, "out of memory");
    p->group_names = names;
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

static bool parse_alt(RxParser *p, RxNode **out) {
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

static void state_destroy(RxState *state) {
    free(state->captures);
    state->captures = NULL;
}

static void state_vec_destroy(RxStateVec *vec) {
    for (size_t i = 0; i < vec->count; i++) state_destroy(&vec->items[i]);
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static bool state_vec_push_copy(RxStateVec *vec, const RxState *state, IdmError *err) {
    if (vec->count == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2u : 8u;
        RxState *items = realloc(vec->items, cap * sizeof(*items));
        if (!items) return idm_error_oom(err, idm_span_unknown(NULL));
        vec->items = items;
        vec->cap = cap;
    }
    RxState *dst = &vec->items[vec->count];
    dst->pos = state->pos;
    dst->captures = NULL;
    if (vec->capture_count != 0) {
        dst->captures = malloc(vec->capture_count * sizeof(*dst->captures));
        if (!dst->captures) return idm_error_oom(err, idm_span_unknown(NULL));
        memcpy(dst->captures, state->captures, vec->capture_count * sizeof(*dst->captures));
    }
    vec->count++;
    return true;
}

static bool char_eq(unsigned char a, unsigned char b, uint32_t flags) {
    if ((flags & IDM_REGEX_CASELESS) == 0) return a == b;
    return tolower(a) == tolower(b);
}

static bool at_line_start(const char *s, size_t pos) {
    return pos == 0 || s[pos - 1u] == '\n';
}

static bool at_line_end(const char *s, size_t len, size_t pos) {
    return pos == len || s[pos] == '\n';
}

static bool look_matches(const IdmRegex *rx, const RxNode *child, const char *s, size_t len, size_t pos, IdmError *err) {
    RxCapture *caps = calloc(rx->group_count + 1u, sizeof(*caps));
    if (!caps) return idm_error_oom(err, idm_span_unknown(NULL));
    RxState state = { pos, caps };
    RxStateVec out = { .capture_count = rx->group_count + 1u };
    bool ok = match_node(rx, child, s, len, &state, &out, err);
    free(caps);
    bool matched = ok && out.count != 0;
    state_vec_destroy(&out);
    return ok && matched;
}

static bool lookbehind_matches(const IdmRegex *rx, const RxNode *child, const char *s, size_t len, size_t pos, IdmError *err) {
    for (size_t start = 0; start <= pos; start++) {
        RxCapture *caps = calloc(rx->group_count + 1u, sizeof(*caps));
        if (!caps) return idm_error_oom(err, idm_span_unknown(NULL));
        RxState state = { start, caps };
        RxStateVec out = { .capture_count = rx->group_count + 1u };
        bool ok = match_node(rx, child, s, len, &state, &out, err);
        free(caps);
        if (!ok) {
            state_vec_destroy(&out);
            return false;
        }
        for (size_t i = 0; i < out.count; i++) {
            if (out.items[i].pos == pos) {
                state_vec_destroy(&out);
                return true;
            }
        }
        state_vec_destroy(&out);
    }
    return false;
}

static bool match_repeat_collect(const IdmRegex *rx, const RxNode *child, size_t min, size_t max, bool unbounded, size_t count, const char *s, size_t len, const RxState *state, RxStateVec *out, IdmError *err) {
    bool pushed = false;
    bool can_more = unbounded || count < max;
    if (can_more) {
        RxStateVec next = { .capture_count = out->capture_count };
        if (!match_node(rx, child, s, len, state, &next, err)) {
            state_vec_destroy(&next);
            return false;
        }
        for (size_t i = 0; i < next.count; i++) {
            if (next.items[i].pos == state->pos) {
                if (count + 1u >= min && !state_vec_push_copy(out, &next.items[i], err)) {
                    state_vec_destroy(&next);
                    return false;
                }
                pushed = true;
                continue;
            }
            if (!match_repeat_collect(rx, child, min, max, unbounded, count + 1u, s, len, &next.items[i], out, err)) {
                state_vec_destroy(&next);
                return false;
            }
        }
        state_vec_destroy(&next);
    }
    if (count >= min && !pushed) return state_vec_push_copy(out, state, err);
    if (count >= min && pushed) return state_vec_push_copy(out, state, err);
    return true;
}

static bool match_node(const IdmRegex *rx, const RxNode *node, const char *s, size_t len, const RxState *state, RxStateVec *out, IdmError *err) {
    switch (node->kind) {
        case RX_EMPTY:
            return state_vec_push_copy(out, state, err);
        case RX_LITERAL:
            if (state->pos < len && char_eq((unsigned char)s[state->pos], node->as.literal, rx->flags)) {
                RxState next = *state;
                next.pos++;
                return state_vec_push_copy(out, &next, err);
            }
            return true;
        case RX_DOT:
            if (state->pos < len && ((rx->flags & IDM_REGEX_DOTALL) != 0 || s[state->pos] != '\n')) {
                RxState next = *state;
                next.pos++;
                return state_vec_push_copy(out, &next, err);
            }
            return true;
        case RX_CLASS:
            if (state->pos < len && cls_has(&node->as.cls, (unsigned char)s[state->pos])) {
                RxState next = *state;
                next.pos++;
                return state_vec_push_copy(out, &next, err);
            }
            return true;
        case RX_ANCHOR_START:
            if (state->pos == 0 || ((rx->flags & IDM_REGEX_MULTILINE) != 0 && at_line_start(s, state->pos))) return state_vec_push_copy(out, state, err);
            return true;
        case RX_ANCHOR_END:
            if (state->pos == len || ((rx->flags & IDM_REGEX_MULTILINE) != 0 && at_line_end(s, len, state->pos))) return state_vec_push_copy(out, state, err);
            return true;
        case RX_ALT:
            for (size_t i = 0; i < node->as.seq.count; i++) {
                if (!match_node(rx, node->as.seq.items[i], s, len, state, out, err)) return false;
            }
            return true;
        case RX_CONCAT: {
            RxStateVec cur = { .capture_count = out->capture_count };
            if (!state_vec_push_copy(&cur, state, err)) return false;
            for (size_t i = 0; i < node->as.seq.count; i++) {
                RxStateVec next = { .capture_count = out->capture_count };
                for (size_t j = 0; j < cur.count; j++) {
                    if (!match_node(rx, node->as.seq.items[i], s, len, &cur.items[j], &next, err)) {
                        state_vec_destroy(&cur);
                        state_vec_destroy(&next);
                        return false;
                    }
                }
                state_vec_destroy(&cur);
                cur = next;
                if (cur.count == 0) break;
            }
            bool ok = true;
            for (size_t i = 0; ok && i < cur.count; i++) ok = state_vec_push_copy(out, &cur.items[i], err);
            state_vec_destroy(&cur);
            return ok;
        }
        case RX_REPEAT:
            return match_repeat_collect(rx, node->as.repeat.child, node->as.repeat.min, node->as.repeat.max, node->as.repeat.unbounded, 0, s, len, state, out, err);
        case RX_CAPTURE: {
            RxState start = *state;
            bool owns_start_captures = false;
            if (node->as.capture.index < out->capture_count) {
                start.captures = malloc(out->capture_count * sizeof(*start.captures));
                if (!start.captures) return idm_error_oom(err, idm_span_unknown(NULL));
                owns_start_captures = true;
                memcpy(start.captures, state->captures, out->capture_count * sizeof(*start.captures));
                start.captures[node->as.capture.index].set = true;
                start.captures[node->as.capture.index].start = state->pos;
                start.captures[node->as.capture.index].end = state->pos;
            }
            RxStateVec inner = { .capture_count = out->capture_count };
            bool ok = match_node(rx, node->as.capture.child, s, len, &start, &inner, err);
            if (owns_start_captures) free(start.captures);
            if (!ok) {
                state_vec_destroy(&inner);
                return false;
            }
            for (size_t i = 0; i < inner.count; i++) {
                if (node->as.capture.index < out->capture_count) inner.items[i].captures[node->as.capture.index].end = inner.items[i].pos;
                if (!state_vec_push_copy(out, &inner.items[i], err)) {
                    state_vec_destroy(&inner);
                    return false;
                }
            }
            state_vec_destroy(&inner);
            return true;
        }
        case RX_LOOK_POS:
        case RX_LOOK_NEG: {
            bool matched = look_matches(rx, node->as.child, s, len, state->pos, err);
            if (err && err->present) return false;
            if ((node->kind == RX_LOOK_POS && matched) || (node->kind == RX_LOOK_NEG && !matched)) return state_vec_push_copy(out, state, err);
            return true;
        }
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG: {
            bool matched = lookbehind_matches(rx, node->as.child, s, len, state->pos, err);
            if (err && err->present) return false;
            if ((node->kind == RX_LOOKBEHIND_POS && matched) || (node->kind == RX_LOOKBEHIND_NEG && !matched)) return state_vec_push_copy(out, state, err);
            return true;
        }
    }
    return true;
}

static IdmRegexResult *result_new(const IdmRegex *rx, const char *s, size_t len, size_t start, const RxState *state, IdmError *err) {
    IdmRegexResult *result = calloc(1u, sizeof(*result));
    if (!result) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    result->subject = idm_strndup(s, len);
    result->subject_len = len;
    result->capture_count = rx->group_count + 1u;
    result->captures = calloc(result->capture_count, sizeof(*result->captures));
    result->group_names = calloc(result->capture_count, sizeof(*result->group_names));
    if (!result->subject || !result->captures || !result->group_names) {
        idm_regex_result_free(result);
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    memcpy(result->captures, state->captures, result->capture_count * sizeof(*result->captures));
    result->captures[0].set = true;
    result->captures[0].start = start;
    result->captures[0].end = state->pos;
    for (size_t i = 1; i < result->capture_count; i++) {
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

static bool scan_at_raw(const IdmRegex *rx, const char *s, size_t len, size_t offset, bool full, IdmRegexResult **out, IdmError *err) {
    *out = NULL;
    if (offset > len) return true;
    RxCapture *caps = calloc(rx->group_count + 1u, sizeof(*caps));
    if (!caps) return idm_error_oom(err, idm_span_unknown(NULL));
    RxState state = { offset, caps };
    RxStateVec states = { .capture_count = rx->group_count + 1u };
    bool ok = match_node(rx, rx->root, s, len, &state, &states, err);
    free(caps);
    if (!ok) {
        state_vec_destroy(&states);
        return false;
    }
    RxState *best = NULL;
    for (size_t i = 0; i < states.count; i++) {
        if (full && states.items[i].pos != len) continue;
        if (!best || states.items[i].pos > best->pos) best = &states.items[i];
    }
    if (best) *out = result_new(rx, s, len, offset, best, err);
    state_vec_destroy(&states);
    return !(err && err->present);
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
    return require_string_arg(rt, name, input, out_s, out_len, err);
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

    RxParser p = { source, source_len, 0, flags, NULL, 0, err };
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
    if (rx->group_names) {
        for (size_t i = 0; i <= rx->group_count; i++) free(rx->group_names[i]);
        free(rx->group_names);
    }
    free(rx);
}

size_t idm_regex_footprint(const IdmRegex *rx) {
    if (!rx) return 0;
    size_t total = sizeof(*rx) + rx->source_len + 1u + node_footprint(rx->root) + (rx->group_count + 1u) * sizeof(*rx->group_names);
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

void idm_regex_result_free(IdmRegexResult *result) {
    if (!result) return;
    free(result->subject);
    free(result->captures);
    if (result->group_names) {
        for (size_t i = 0; i < result->capture_count; i++) free(result->group_names[i]);
        free(result->group_names);
    }
    free(result);
}

IdmRegexResult *idm_regex_result_clone(const IdmRegexResult *src) {
    if (!src) return NULL;
    IdmRegexResult *r = calloc(1u, sizeof(*r));
    if (!r) return NULL;
    r->subject_len = src->subject_len;
    r->capture_count = src->capture_count;
    if (src->subject) {
        r->subject = malloc(src->subject_len + 1u);
        if (!r->subject) { idm_regex_result_free(r); return NULL; }
        memcpy(r->subject, src->subject, src->subject_len);
        r->subject[src->subject_len] = '\0';
    }
    if (src->capture_count != 0 && src->captures) {
        r->captures = malloc(src->capture_count * sizeof(*r->captures));
        if (!r->captures) { idm_regex_result_free(r); return NULL; }
        memcpy(r->captures, src->captures, src->capture_count * sizeof(*r->captures));
    }
    if (src->group_names) {
        r->group_names = calloc(src->capture_count, sizeof(*r->group_names));
        if (!r->group_names) { idm_regex_result_free(r); return NULL; }
        for (size_t i = 0; i < src->capture_count; i++) {
            if (!src->group_names[i]) continue;
            r->group_names[i] = idm_strdup(src->group_names[i]);
            if (!r->group_names[i]) { idm_regex_result_free(r); return NULL; }
        }
    }
    return r;
}

size_t idm_regex_result_footprint(const IdmRegexResult *result) {
    if (!result) return 0;
    size_t total = sizeof(*result) + result->subject_len + 1u + result->capture_count * sizeof(*result->captures) + result->capture_count * sizeof(*result->group_names);
    for (size_t i = 0; i < result->capture_count; i++) if (result->group_names[i]) total += strlen(result->group_names[i]) + 1u;
    return total;
}

bool idm_regex_compile_value(IdmRuntime *rt, IdmValue source, IdmValue options, IdmValue *out, IdmError *err) {
    const char *s = NULL;
    size_t len = 0;
    if (!require_string_arg(rt, "regex-raw-compile", source, &s, &len, err)) return false;
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
    if (!regex_and_input(rt, "regex-raw-scan-at", regex, input, &rx, &s, &len, err)) return false;
    IdmRegexResult *result = NULL;
    if (!scan_at_raw(rx, s, len, offset, false, &result, err)) return false;
    return wrap_result_or_nil(rt, result, out, err);
}

bool idm_regex_scan_from(IdmRuntime *rt, IdmValue regex, IdmValue input, size_t offset, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "regex-raw-scan-from", regex, input, &rx, &s, &len, err)) return false;
    if (offset > len) offset = len;
    for (size_t pos = offset; pos <= len; pos++) {
        IdmRegexResult *result = NULL;
        if (!scan_at_raw(rx, s, len, pos, false, &result, err)) return false;
        if (result) return wrap_result_or_nil(rt, result, out, err);
    }
    *out = idm_nil();
    return true;
}

bool idm_regex_scan_full(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "regex-raw-scan-full", regex, input, &rx, &s, &len, err)) return false;
    IdmRegexResult *result = NULL;
    if (!scan_at_raw(rx, s, len, 0, true, &result, err)) return false;
    return wrap_result_or_nil(rt, result, out, err);
}

bool idm_regex_test(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmValue result = idm_nil();
    if (!idm_regex_scan_from(rt, regex, input, 0, &result, err)) return false;
    *out = idm_atom(rt, result.tag == IDM_VAL_REGEX_RESULT ? "true" : "false");
    return true;
}

bool idm_regex_scan_all(IdmRuntime *rt, IdmValue regex, IdmValue input, IdmValue *out, IdmError *err) {
    IdmRegex *rx = NULL;
    const char *s = NULL;
    size_t len = 0;
    if (!regex_and_input(rt, "regex-raw-scan-all", regex, input, &rx, &s, &len, err)) return false;
    IdmValue acc = idm_empty_list();
    size_t pos = 0;
    while (pos <= len) {
        IdmRegexResult *result = NULL;
        if (!scan_at_raw(rx, s, len, pos, false, &result, err)) return false;
        if (!result) {
            pos++;
            continue;
        }
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
    RxCapture c = r->captures[0];
    *out = idm_string_n(rt, r->subject + c.start, c.end - c.start, err);
    return !(err && err->present);
}

static bool capture_index(IdmValue index, size_t max, size_t *out, IdmError *err) {
    if (index.tag != IDM_VAL_INT || index.as.i < 0 || (uint64_t)index.as.i >= max) {
        return idm_error_set(err, idm_span_unknown(NULL), "capture index out of range");
    }
    *out = (size_t)index.as.i;
    return true;
}

static bool capture_by_name(IdmRegexResult *r, IdmValue name, size_t *out, IdmError *err) {
    const char *text = NULL;
    if (name.tag == IDM_VAL_ATOM || name.tag == IDM_VAL_WORD) text = idm_symbol_text(name.as.symbol);
    else if (name.tag == IDM_VAL_STRING) text = idm_string_bytes(name);
    if (!text) return idm_error_set(err, idm_span_unknown(NULL), "capture name must be an atom, word, or string");
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
    RxCapture c = r->captures[index];
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
    RxCapture c = r->captures[index];
    return idm_buf_append_n(buf, r->subject + c.start, c.end - c.start);
}

static bool append_named_capture(IdmBuffer *buf, IdmRegexResult *r, const char *name) {
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
    if (!regex_and_input(rt, "regex-raw-replace", regex, input, &rx, &s, &len, err)) return false;
    const char *repl = NULL;
    size_t repl_len = 0;
    if (!require_string_arg(rt, "regex-raw-replace", replacement, &repl, &repl_len, err)) return false;
    IdmBuffer buf;
    idm_buf_init(&buf);
    size_t pos = 0;
    bool changed = false;
    while (pos <= len) {
        IdmRegexResult *result = NULL;
        if (!scan_at_raw(rx, s, len, pos, false, &result, err)) {
            idm_buf_destroy(&buf);
            return false;
        }
        if (!result) {
            if (pos < len) {
                if (!idm_buf_append_char(&buf, s[pos])) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
                pos++;
                continue;
            }
            break;
        }
        RxCapture whole = result->captures[0];
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
        if (next <= pos) {
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
    if (!regex_and_input(rt, "regex-raw-split-on", regex, input, &rx, &s, &len, err)) return false;
    IdmValue acc = idm_empty_list();
    size_t segment_start = 0;
    size_t scan_pos = 0;
    while (scan_pos <= len) {
        IdmRegexResult *result = NULL;
        if (!scan_at_raw(rx, s, len, scan_pos, false, &result, err)) return false;
        if (!result) {
            scan_pos++;
            if (scan_pos <= len) continue;
            IdmValue tail = idm_string_n(rt, s + segment_start, len - segment_start, err);
            if (err && err->present) return false;
            acc = idm_cons(rt, tail, acc, err);
            if (err && err->present) return false;
            break;
        }
        RxCapture whole = result->captures[0];
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
    if (!require_string_arg(rt, "regex-raw-escape", input, &s, &len, err)) return false;
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
