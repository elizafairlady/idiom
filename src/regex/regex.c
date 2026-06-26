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

typedef struct RxProg RxProg;

typedef enum {
    RXI_MATCH,
    RXI_CHAR,
    RXI_DOT,
    RXI_CLASS,
    RXI_ASSERT_START,
    RXI_ASSERT_END,
    RXI_JUMP,
    RXI_SPLIT,
    RXI_SAVE,
    RXI_LOOK
} RxInstKind;

typedef enum {
    RX_LOOK_AHEAD_POS,
    RX_LOOK_AHEAD_NEG,
    RX_LOOK_BEHIND_POS,
    RX_LOOK_BEHIND_NEG
} RxLookKind;

typedef struct {
    RxInstKind kind;
    uint32_t flags;
    size_t accept_id;
    union {
        unsigned char literal;
        RxClass cls;
        size_t target;
        struct {
            size_t first;
            size_t second;
        } split;
        size_t save_slot;
        struct {
            RxLookKind kind;
            RxProg *prog;
        } look;
    } as;
} RxInst;

typedef struct {
    size_t *pcs;
    size_t count;
    bool has_match;
    bool dynamic;
} RxTestClosure;

typedef struct {
    bool any;
    bool bytes[256];
    bool nullable;
    bool anchored_start;
} RxStartInfo;

struct RxProg {
    RxInst *insts;
    size_t count;
    size_t cap;
    size_t capture_count;
    uint32_t flags;
    RxTestClosure *test_closures;
    RxStartInfo start;
};

struct IdmRegex {
    char *source;
    size_t source_len;
    uint32_t flags;
    RxNode *root;
    RxProg *prog;
    char **group_names;
    size_t group_count;
};

struct IdmRegexSet {
    RxProg *prog;
    size_t count;
    char ***group_names;
    size_t *group_counts;
};

struct IdmRegexResult {
    char *subject;
    size_t subject_len;
    IdmValue subject_value;
    RxCapture *captures;
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
    IdmError *err;
    size_t depth;
} RxParser;

typedef struct {
    size_t pc;
    size_t pos;
    size_t capture_index;
} RxVmState;

typedef struct {
    RxVmState *items;
    RxCapture *captures;
    size_t count;
    size_t cap;
    size_t capture_count;
} RxVmStateVec;

typedef struct {
    size_t *items;
    size_t count;
    size_t cap;
    uint32_t mark;
    bool heap_items;
} RxTestStateVec;

typedef struct {
    bool matched;
    size_t end;
    size_t accept_id;
    RxCapture *captures;
} RxMatch;

enum { RX_STACK_CAPTURE_LIMIT = 16 };
enum { RX_MAX_CLOSURE_DEPTH = 10000 };
enum { RX_PROG_WIRE_VERSION = 1 };

static void node_free(RxNode *node);
static bool parse_alt(RxParser *p, RxNode **out);
static void prog_free(RxProg *prog);
static bool prog_build_test_closures(RxProg *prog, IdmError *err);
static bool regex_start_candidate(const RxProg *prog, const char *s, size_t len, size_t pos);

static bool require_string_arg(IdmRuntime *rt, const char *name, IdmValue v, const char **out, size_t *out_len, IdmError *err) {
    if (idm_value_tag(v) != IDM_VAL_STRING) {
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

static void start_info_add_class(RxStartInfo *info, const RxClass *cls) {
    for (unsigned i = 0; i < 256u; i++) {
        if (cls_has(cls, (unsigned char)i)) info->bytes[i] = true;
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
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG:
            info.nullable = true;
            return info;
        case RX_ANCHOR_START:
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

static RxProg *prog_new(size_t capture_count, uint32_t flags, IdmError *err) {
    RxProg *prog = calloc(1u, sizeof(*prog));
    if (!prog) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    prog->capture_count = capture_count;
    prog->flags = flags;
    return prog;
}

static bool prog_emit(RxProg *prog, RxInst inst, size_t *out_index, IdmError *err) {
    if (prog->count == prog->cap) {
        size_t cap = prog->cap ? prog->cap * 2u : 32u;
        RxInst *insts = realloc(prog->insts, cap * sizeof(*insts));
        if (!insts) return idm_error_oom(err, idm_span_unknown(NULL));
        prog->insts = insts;
        prog->cap = cap;
    }
    if (out_index) *out_index = prog->count;
    prog->insts[prog->count++] = inst;
    return true;
}

static bool prog_emit_simple(RxProg *prog, RxInstKind kind, size_t *out_index, IdmError *err) {
    RxInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.kind = kind;
    inst.flags = prog->flags;
    return prog_emit(prog, inst, out_index, err);
}

static bool prog_patch_target(RxProg *prog, size_t inst_index, size_t target, IdmError *err) {
    if (inst_index >= prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex compiler patch index out of bounds");
    if (target > prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex compiler patch target out of bounds");
    prog->insts[inst_index].as.target = target;
    return true;
}

static bool prog_patch_split(RxProg *prog, size_t inst_index, size_t first, size_t second, IdmError *err) {
    if (inst_index >= prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex compiler split patch index out of bounds");
    if (first > prog->count || second > prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex compiler split patch target out of bounds");
    prog->insts[inst_index].as.split.first = first;
    prog->insts[inst_index].as.split.second = second;
    return true;
}

static bool compile_node(RxProg *prog, const RxNode *node, IdmError *err);
static bool prog_build_test_closures(RxProg *prog, IdmError *err);
static bool size_vec_push(size_t **vec, size_t *count, size_t *cap, size_t value, IdmError *err);

static bool compile_alt_from(RxProg *prog, RxNode **items, size_t index, size_t count, IdmError *err) {
    if (index >= count) return true;
    size_t *jumps = NULL;
    size_t jump_count = 0;
    size_t jump_cap = 0;
    bool ok = true;
    for (size_t i = index; ok && i + 1u < count; i++) {
        RxInst split;
        memset(&split, 0, sizeof(split));
        split.kind = RXI_SPLIT;
        size_t split_index = 0;
        if (!prog_emit(prog, split, &split_index, err)) { ok = false; break; }
        size_t first = prog->count;
        if (!compile_node(prog, items[i], err)) { ok = false; break; }
        RxInst jump;
        memset(&jump, 0, sizeof(jump));
        jump.kind = RXI_JUMP;
        size_t jump_index = 0;
        if (!prog_emit(prog, jump, &jump_index, err)) { ok = false; break; }
        size_t second = prog->count;
        if (!prog_patch_split(prog, split_index, first, second, err)) { ok = false; break; }
        if (!size_vec_push(&jumps, &jump_count, &jump_cap, jump_index, err)) { ok = false; break; }
    }
    if (ok) ok = compile_node(prog, items[count - 1u], err);
    for (size_t i = 0; ok && i < jump_count; i++) ok = prog_patch_target(prog, jumps[i], prog->count, err);
    free(jumps);
    return ok;
}

static bool compile_star(RxProg *prog, const RxNode *child, IdmError *err) {
    RxInst split;
    memset(&split, 0, sizeof(split));
    split.kind = RXI_SPLIT;
    size_t split_index = 0;
    if (!prog_emit(prog, split, &split_index, err)) return false;
    size_t body = prog->count;
    if (!compile_node(prog, child, err)) return false;
    RxInst jump;
    memset(&jump, 0, sizeof(jump));
    jump.kind = RXI_JUMP;
    jump.as.target = split_index;
    if (!prog_emit(prog, jump, NULL, err)) return false;
    return prog_patch_split(prog, split_index, body, prog->count, err);
}

static bool compile_optional(RxProg *prog, const RxNode *child, IdmError *err) {
    RxInst split;
    memset(&split, 0, sizeof(split));
    split.kind = RXI_SPLIT;
    size_t split_index = 0;
    if (!prog_emit(prog, split, &split_index, err)) return false;
    size_t body = prog->count;
    if (!compile_node(prog, child, err)) return false;
    return prog_patch_split(prog, split_index, body, prog->count, err);
}

static bool compile_repeat(RxProg *prog, const RxNode *child, size_t min, size_t max, bool unbounded, IdmError *err) {
    if (min > RX_REPEAT_COMPILE_LIMIT || (!unbounded && max > RX_REPEAT_COMPILE_LIMIT)) {
        return idm_error_set(err, idm_span_unknown(NULL), "regex counted repeat expands past %u NFA copies", (unsigned)RX_REPEAT_COMPILE_LIMIT);
    }
    for (size_t i = 0; i < min; i++) {
        if (!compile_node(prog, child, err)) return false;
    }
    if (unbounded) return compile_star(prog, child, err);
    for (size_t i = min; i < max; i++) {
        if (!compile_optional(prog, child, err)) return false;
    }
    return true;
}

static bool compile_child_prog(const RxProg *parent, const RxNode *node, RxProg **out, IdmError *err) {
    *out = NULL;
    RxProg *child = prog_new(parent->capture_count, parent->flags, err);
    if (!child) return false;
    if (!compile_node(child, node, err) || !prog_emit_simple(child, RXI_MATCH, NULL, err) || !prog_build_test_closures(child, err)) {
        prog_free(child);
        return false;
    }
    *out = child;
    return true;
}

static bool compile_node(RxProg *prog, const RxNode *node, IdmError *err) {
    if (!node) return true;
    RxInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.flags = prog->flags;
    switch (node->kind) {
        case RX_EMPTY:
            return true;
        case RX_LITERAL:
            inst.kind = RXI_CHAR;
            inst.as.literal = node->as.literal;
            return prog_emit(prog, inst, NULL, err);
        case RX_DOT:
            return prog_emit_simple(prog, RXI_DOT, NULL, err);
        case RX_CLASS:
            inst.kind = RXI_CLASS;
            inst.as.cls = node->as.cls;
            return prog_emit(prog, inst, NULL, err);
        case RX_ANCHOR_START:
            return prog_emit_simple(prog, RXI_ASSERT_START, NULL, err);
        case RX_ANCHOR_END:
            return prog_emit_simple(prog, RXI_ASSERT_END, NULL, err);
        case RX_CONCAT:
            for (size_t i = 0; i < node->as.seq.count; i++) {
                if (!compile_node(prog, node->as.seq.items[i], err)) return false;
            }
            return true;
        case RX_ALT:
            return compile_alt_from(prog, node->as.seq.items, 0, node->as.seq.count, err);
        case RX_REPEAT:
            return compile_repeat(prog, node->as.repeat.child, node->as.repeat.min, node->as.repeat.max, node->as.repeat.unbounded, err);
        case RX_CAPTURE: {
            inst.kind = RXI_SAVE;
            inst.as.save_slot = node->as.capture.index * 2u;
            if (!prog_emit(prog, inst, NULL, err)) return false;
            if (!compile_node(prog, node->as.capture.child, err)) return false;
            inst.as.save_slot = node->as.capture.index * 2u + 1u;
            return prog_emit(prog, inst, NULL, err);
        }
        case RX_LOOK_POS:
        case RX_LOOK_NEG:
        case RX_LOOKBEHIND_POS:
        case RX_LOOKBEHIND_NEG: {
            RxProg *child = NULL;
            if (!compile_child_prog(prog, node->as.child, &child, err)) return false;
            inst.kind = RXI_LOOK;
            if (node->kind == RX_LOOK_POS) inst.as.look.kind = RX_LOOK_AHEAD_POS;
            else if (node->kind == RX_LOOK_NEG) inst.as.look.kind = RX_LOOK_AHEAD_NEG;
            else if (node->kind == RX_LOOKBEHIND_POS) inst.as.look.kind = RX_LOOK_BEHIND_POS;
            else inst.as.look.kind = RX_LOOK_BEHIND_NEG;
            inst.as.look.prog = child;
            if (!prog_emit(prog, inst, NULL, err)) {
                prog_free(child);
                return false;
            }
            return true;
        }
    }
    return true;
}

static bool test_closure_append(RxTestClosure *closure, size_t pc, IdmError *err) {
    size_t *pcs = realloc(closure->pcs, (closure->count + 1u) * sizeof(*pcs));
    if (!pcs) return idm_error_oom(err, idm_span_unknown(NULL));
    closure->pcs = pcs;
    closure->pcs[closure->count++] = pc;
    return true;
}

static bool size_vec_push(size_t **vec, size_t *count, size_t *cap, size_t value, IdmError *err) {
    if (*count == *cap) {
        size_t next_cap = *cap ? *cap * 2u : 16u;
        size_t *next = realloc(*vec, next_cap * sizeof(*next));
        if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
        *vec = next;
        *cap = next_cap;
    }
    (*vec)[(*count)++] = value;
    return true;
}

static bool build_test_closure_one(const RxProg *prog, size_t start, RxTestClosure *closure, IdmError *err) {
    unsigned char *seen = calloc(prog->count, sizeof(*seen));
    if (!seen) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t *stack = NULL;
    size_t stack_count = 0;
    size_t stack_cap = 0;
    bool ok = size_vec_push(&stack, &stack_count, &stack_cap, start, err);
    while (ok && stack_count != 0) {
        size_t pc = stack[--stack_count];
        if (pc >= prog->count) {
            ok = idm_error_set(err, idm_span_unknown(NULL), "regex compiler closure target out of bounds");
            break;
        }
        if (seen[pc]) continue;
        seen[pc] = 1u;
        const RxInst *inst = &prog->insts[pc];
        switch (inst->kind) {
            case RXI_JUMP:
                ok = size_vec_push(&stack, &stack_count, &stack_cap, inst->as.target, err);
                break;
            case RXI_SPLIT:
                ok = size_vec_push(&stack, &stack_count, &stack_cap, inst->as.split.second, err)
                    && size_vec_push(&stack, &stack_count, &stack_cap, inst->as.split.first, err);
                break;
            case RXI_SAVE:
                ok = size_vec_push(&stack, &stack_count, &stack_cap, pc + 1u, err);
                break;
            case RXI_MATCH:
                closure->has_match = true;
                break;
            case RXI_CHAR:
            case RXI_DOT:
            case RXI_CLASS:
                ok = test_closure_append(closure, pc, err);
                break;
            case RXI_ASSERT_START:
            case RXI_ASSERT_END:
            case RXI_LOOK:
                closure->dynamic = true;
                break;
        }
    }
    free(stack);
    free(seen);
    if (!ok) return false;
    if (closure->dynamic) {
        free(closure->pcs);
        closure->pcs = NULL;
        closure->count = 0;
        closure->has_match = false;
    }
    return true;
}

static bool prog_build_test_closures(RxProg *prog, IdmError *err) {
    if (prog->count == 0) return true;
    prog->test_closures = calloc(prog->count, sizeof(*prog->test_closures));
    if (!prog->test_closures) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < prog->count; i++) {
        if (!build_test_closure_one(prog, i, &prog->test_closures[i], err)) return false;
    }
    return true;
}

static bool compile_regex_program(IdmRegex *rx, IdmError *err) {
    RxProg *prog = prog_new(rx->group_count + 1u, rx->flags, err);
    if (!prog) return false;
    prog->start = start_info_node(rx->root, rx->flags);
    if (!compile_node(prog, rx->root, err) || !prog_emit_simple(prog, RXI_MATCH, NULL, err)) {
        prog_free(prog);
        return false;
    }
    if (!prog_build_test_closures(prog, err)) {
        prog_free(prog);
        return false;
    }
    rx->prog = prog;
    return true;
}

static bool rx_put_start_info(IdmBuffer *out, const RxStartInfo *start, IdmError *err) {
    if (!idm_buf_put_u8(out, start->any ? 1u : 0u) ||
        !idm_buf_put_u8(out, start->nullable ? 1u : 0u) ||
        !idm_buf_put_u8(out, start->anchored_start ? 1u : 0u) ||
        !idm_buf_append_n(out, (const char *)start->bytes, sizeof(start->bytes))) {
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool rx_read_bytes(IdmByteReader *r, void *dst, size_t len, IdmError *err, const char *what) {
    if (len > r->len - r->pos) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "truncated %s", what);
    }
    memcpy(dst, r->data + r->pos, len);
    r->pos += len;
    return true;
}

static bool rx_read_start_info(IdmByteReader *r, RxStartInfo *start, IdmError *err) {
    memset(start, 0, sizeof(*start));
    uint8_t any = idm_rd_u8(r);
    uint8_t nullable = idm_rd_u8(r);
    uint8_t anchored = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex program start info");
    if (any > 1u || nullable > 1u || anchored > 1u) return idm_error_set(err, idm_span_unknown(NULL), "invalid regex program start info");
    start->any = any != 0;
    start->nullable = nullable != 0;
    start->anchored_start = anchored != 0;
    return rx_read_bytes(r, start->bytes, sizeof(start->bytes), err, "regex program start bytes");
}

static bool rx_put_class(IdmBuffer *out, const RxClass *cls, IdmError *err) {
    if (!idm_buf_put_u8(out, cls->negated ? 1u : 0u) ||
        !idm_buf_append_n(out, (const char *)cls->bits, sizeof(cls->bits))) {
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    return true;
}

static bool rx_read_class(IdmByteReader *r, RxClass *cls, IdmError *err) {
    memset(cls, 0, sizeof(*cls));
    uint8_t negated = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex class");
    if (negated > 1u) return idm_error_set(err, idm_span_unknown(NULL), "invalid regex class");
    cls->negated = negated != 0;
    return rx_read_bytes(r, cls->bits, sizeof(cls->bits), err, "regex class");
}

static bool rx_prog_serialize_at(IdmBuffer *out, const RxProg *prog, IdmError *err, unsigned depth) {
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "regex program nested too deeply");
    if (!prog || prog->count > UINT32_MAX || prog->capture_count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "regex program is incomplete");
    if (!idm_buf_put_u32(out, RX_PROG_WIRE_VERSION) ||
        !idm_buf_put_u32(out, (uint32_t)prog->flags) ||
        !idm_buf_put_u32(out, (uint32_t)prog->capture_count) ||
        !rx_put_start_info(out, &prog->start, err) ||
        !idm_buf_put_u32(out, (uint32_t)prog->count)) return false;
    for (size_t i = 0; i < prog->count; i++) {
        const RxInst *inst = &prog->insts[i];
        if (!idm_buf_put_u8(out, (uint8_t)inst->kind) ||
            !idm_buf_put_u32(out, inst->flags) ||
            !idm_buf_put_u32(out, (uint32_t)inst->accept_id)) return idm_error_oom(err, idm_span_unknown(NULL));
        switch (inst->kind) {
            case RXI_MATCH:
                break;
            case RXI_CHAR:
                if (!idm_buf_put_u8(out, inst->as.literal)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case RXI_DOT:
            case RXI_ASSERT_START:
            case RXI_ASSERT_END:
                break;
            case RXI_CLASS:
                if (!rx_put_class(out, &inst->as.cls, err)) return false;
                break;
            case RXI_JUMP:
                if (!idm_buf_put_u32(out, (uint32_t)inst->as.target)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case RXI_SPLIT:
                if (!idm_buf_put_u32(out, (uint32_t)inst->as.split.first) ||
                    !idm_buf_put_u32(out, (uint32_t)inst->as.split.second)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case RXI_SAVE:
                if (!idm_buf_put_u32(out, (uint32_t)inst->as.save_slot)) return idm_error_oom(err, idm_span_unknown(NULL));
                break;
            case RXI_LOOK:
                if (!idm_buf_put_u8(out, (uint8_t)inst->as.look.kind) ||
                    !rx_prog_serialize_at(out, inst->as.look.prog, err, depth + 1u)) return false;
                break;
        }
    }
    return true;
}

static bool rx_prog_validate(const RxProg *prog, size_t accept_count, IdmError *err) {
    if (!prog || prog->count == 0 || !prog->insts) return idm_error_set(err, idm_span_unknown(NULL), "regex program is empty");
    for (size_t i = 0; i < prog->count; i++) {
        const RxInst *inst = &prog->insts[i];
        if (inst->kind > RXI_LOOK) return idm_error_set(err, idm_span_unknown(NULL), "invalid regex program opcode");
        if (inst->kind == RXI_MATCH && inst->accept_id >= accept_count) return idm_error_set(err, idm_span_unknown(NULL), "regex program accept id out of bounds");
        if (inst->kind == RXI_JUMP && inst->as.target >= prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex program jump target out of bounds");
        if (inst->kind == RXI_SPLIT && (inst->as.split.first >= prog->count || inst->as.split.second >= prog->count)) return idm_error_set(err, idm_span_unknown(NULL), "regex program split target out of bounds");
        if (inst->kind == RXI_SAVE && inst->as.save_slot / 2u >= prog->capture_count) return idm_error_set(err, idm_span_unknown(NULL), "regex program save slot out of bounds");
        if (inst->kind == RXI_LOOK) {
            if (inst->as.look.kind > RX_LOOK_BEHIND_NEG) return idm_error_set(err, idm_span_unknown(NULL), "regex program lookaround kind is invalid");
            if (!rx_prog_validate(inst->as.look.prog, accept_count, err)) return false;
        }
    }
    return true;
}

static bool rx_prog_deserialize_at(IdmByteReader *r, RxProg **out, IdmError *err, unsigned depth) {
    *out = NULL;
    if (depth > IDM_IC_MAX_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "regex program nested too deeply");
    uint32_t version = idm_rd_u32(r);
    uint32_t flags = idm_rd_u32(r);
    uint32_t capture_count = idm_rd_u32(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex program header");
    if (version != RX_PROG_WIRE_VERSION) return idm_error_set(err, idm_span_unknown(NULL), "regex program version %u unsupported", version);
    RxProg *prog = prog_new(capture_count, flags, err);
    if (!prog) return false;
    if (!rx_read_start_info(r, &prog->start, err)) {
        prog_free(prog);
        return false;
    }
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) {
        prog_free(prog);
        return idm_error_set(err, idm_span_unknown(NULL), "truncated regex program instruction count");
    }
    for (uint32_t i = 0; i < count; i++) {
        RxInst inst;
        memset(&inst, 0, sizeof(inst));
        inst.kind = (RxInstKind)idm_rd_u8(r);
        inst.flags = idm_rd_u32(r);
        inst.accept_id = idm_rd_u32(r);
        if (!r->ok || inst.kind > RXI_LOOK) {
            prog_free(prog);
            return idm_error_set(err, idm_span_unknown(NULL), "truncated regex program instruction");
        }
        switch (inst.kind) {
            case RXI_MATCH:
                break;
            case RXI_CHAR:
                inst.as.literal = idm_rd_u8(r);
                if (!r->ok) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated regex char instruction");
                }
                break;
            case RXI_DOT:
            case RXI_ASSERT_START:
            case RXI_ASSERT_END:
                break;
            case RXI_CLASS:
                if (!rx_read_class(r, &inst.as.cls, err)) {
                    prog_free(prog);
                    return false;
                }
                break;
            case RXI_JUMP:
                inst.as.target = idm_rd_u32(r);
                if (!r->ok) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated regex jump instruction");
                }
                break;
            case RXI_SPLIT:
                inst.as.split.first = idm_rd_u32(r);
                inst.as.split.second = idm_rd_u32(r);
                if (!r->ok) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated regex split instruction");
                }
                break;
            case RXI_SAVE:
                inst.as.save_slot = idm_rd_u32(r);
                if (!r->ok) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated regex save instruction");
                }
                break;
            case RXI_LOOK:
                inst.as.look.kind = (RxLookKind)idm_rd_u8(r);
                if (!r->ok) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "truncated regex look instruction");
                }
                if (inst.as.look.kind > RX_LOOK_BEHIND_NEG) {
                    prog_free(prog);
                    return idm_error_set(err, idm_span_unknown(NULL), "invalid regex look instruction");
                }
                if (!rx_prog_deserialize_at(r, &inst.as.look.prog, err, depth + 1u)) {
                    prog_free(prog);
                    return false;
                }
                break;
        }
        if (!prog_emit(prog, inst, NULL, err)) {
            if (inst.kind == RXI_LOOK) prog_free(inst.as.look.prog);
            prog_free(prog);
            return false;
        }
    }
    if (!prog_build_test_closures(prog, err)) {
        prog_free(prog);
        return false;
    }
    *out = prog;
    return true;
}

static void prog_free(RxProg *prog) {
    if (!prog) return;
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->insts[i].kind == RXI_LOOK) prog_free(prog->insts[i].as.look.prog);
    }
    if (prog->test_closures) {
        for (size_t i = 0; i < prog->count; i++) free(prog->test_closures[i].pcs);
        free(prog->test_closures);
    }
    free(prog->insts);
    free(prog);
}

static bool prog_copy_into(RxProg *dst, const RxProg *src, size_t accept_id, IdmError *err);

static RxProg *prog_clone_with_accept(const RxProg *src, size_t accept_id, IdmError *err) {
    if (!src) return NULL;
    RxProg *copy = prog_new(src->capture_count, src->flags, err);
    if (!copy) return NULL;
    copy->start = src->start;
    if (!prog_copy_into(copy, src, accept_id, err)) {
        prog_free(copy);
        return NULL;
    }
    return copy;
}

static bool prog_copy_into(RxProg *dst, const RxProg *src, size_t accept_id, IdmError *err) {
    if (!dst || !src) return idm_error_set(err, idm_span_unknown(NULL), "regex program copy requires source and destination");
    size_t offset = dst->count;
    for (size_t i = 0; i < src->count; i++) {
        RxInst inst = src->insts[i];
        if (inst.kind == RXI_JUMP) {
            inst.as.target += offset;
        } else if (inst.kind == RXI_SPLIT) {
            inst.as.split.first += offset;
            inst.as.split.second += offset;
        } else if (inst.kind == RXI_MATCH) {
            inst.accept_id = accept_id;
        } else if (inst.kind == RXI_LOOK) {
            inst.as.look.prog = prog_clone_with_accept(inst.as.look.prog, accept_id, err);
            if (!inst.as.look.prog) return false;
        }
        if (!prog_emit(dst, inst, NULL, err)) {
            if (inst.kind == RXI_LOOK) prog_free(inst.as.look.prog);
            return false;
        }
    }
    return true;
}

void idm_regex_set_free(IdmRegexSet *set) {
    if (!set) return;
    prog_free(set->prog);
    if (set->group_names) {
        for (size_t i = 0; i < set->count; i++) {
            if (!set->group_names[i]) continue;
            for (size_t g = 0; g <= set->group_counts[i]; g++) free(set->group_names[i][g]);
            free(set->group_names[i]);
        }
        free(set->group_names);
    }
    free(set->group_counts);
    free(set);
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
        if (!items[i] || !items[i]->prog) return idm_error_set(err, idm_span_unknown(NULL), "regex set item is incomplete");
        if (items[i]->prog->capture_count > capture_count) capture_count = items[i]->prog->capture_count;
    }
    IdmRegexSet *set = calloc(1u, sizeof(*set));
    if (!set) return idm_error_oom(err, idm_span_unknown(NULL));
    set->count = count;
    if (!regex_set_copy_group_names(set, items, count, err)) {
        idm_regex_set_free(set);
        return false;
    }
    set->prog = prog_new(capture_count, 0, err);
    if (!set->prog) {
        idm_regex_set_free(set);
        return false;
    }
    size_t split_count = count > 1u ? count - 1u : 0u;
    size_t *starts = calloc(count, sizeof(*starts));
    if (!starts) {
        idm_regex_set_free(set);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < split_count; i++) {
        RxInst split;
        memset(&split, 0, sizeof(split));
        split.kind = RXI_SPLIT;
        if (!prog_emit(set->prog, split, NULL, err)) {
            free(starts);
            idm_regex_set_free(set);
            return false;
        }
    }
    for (size_t i = 0; i < count; i++) {
        starts[i] = set->prog->count;
        if (!prog_copy_into(set->prog, items[i]->prog, i, err)) {
            free(starts);
            idm_regex_set_free(set);
            return false;
        }
    }
    for (size_t i = 0; i < split_count; i++) {
        set->prog->insts[i].as.split.first = starts[i];
        set->prog->insts[i].as.split.second = i + 1u < split_count ? i + 1u : starts[i + 1u];
    }
    free(starts);
    *out = set;
    return true;
}

static size_t prog_footprint(const RxProg *prog) {
    if (!prog) return 0;
    size_t total = sizeof(*prog) + prog->cap * sizeof(*prog->insts);
    if (prog->test_closures) {
        total += prog->count * sizeof(*prog->test_closures);
        for (size_t i = 0; i < prog->count; i++) total += prog->test_closures[i].count * sizeof(*prog->test_closures[i].pcs);
    }
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->insts[i].kind == RXI_LOOK) total += prog_footprint(prog->insts[i].as.look.prog);
    }
    return total;
}

static void state_vec_destroy(RxVmStateVec *vec) {
    free(vec->items);
    free(vec->captures);
    vec->items = NULL;
    vec->captures = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static void state_vec_clear(RxVmStateVec *vec) {
    vec->count = 0;
}

static void test_state_vec_destroy(RxTestStateVec *vec) {
    if (vec->heap_items) free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->cap = 0;
    vec->heap_items = false;
}

static void test_state_vec_clear(RxTestStateVec *vec, uint32_t mark) {
    vec->count = 0;
    vec->mark = mark;
}

static bool test_state_vec_push(RxTestStateVec *vec, size_t pc, IdmError *err) {
    if (vec->count == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2u : 8u;
        size_t *items = vec->heap_items ? realloc(vec->items, cap * sizeof(*items)) : malloc(cap * sizeof(*items));
        if (!items) return idm_error_oom(err, idm_span_unknown(NULL));
        if (!vec->heap_items && vec->count != 0) memcpy(items, vec->items, vec->count * sizeof(*items));
        vec->items = items;
        vec->cap = cap;
        vec->heap_items = true;
    }
    vec->items[vec->count++] = pc;
    return true;
}

static bool state_vec_push_copy(RxVmStateVec *vec, size_t pc, size_t pos, const RxCapture *captures, IdmError *err) {
    if (vec->count == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2u : 8u;
        if (vec->capture_count != 0) {
            RxCapture *capture_items = realloc(vec->captures, cap * vec->capture_count * sizeof(*capture_items));
            if (!capture_items) return idm_error_oom(err, idm_span_unknown(NULL));
            vec->captures = capture_items;
        }
        RxVmState *items = realloc(vec->items, cap * sizeof(*items));
        if (!items) return idm_error_oom(err, idm_span_unknown(NULL));
        vec->items = items;
        vec->cap = cap;
    }
    RxVmState *dst = &vec->items[vec->count];
    dst->pc = pc;
    dst->pos = pos;
    if (vec->capture_count != 0) {
        dst->capture_index = vec->count;
        memcpy(vec->captures + vec->count * vec->capture_count, captures, vec->capture_count * sizeof(*vec->captures));
    } else {
        dst->capture_index = 0;
    }
    vec->count++;
    return true;
}

static const RxCapture *state_captures(const RxVmStateVec *vec, const RxVmState *state) {
    return vec->capture_count == 0 ? NULL : vec->captures + state->capture_index * vec->capture_count;
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

static void match_destroy(RxMatch *match) {
    free(match->captures);
    match->captures = NULL;
    match->matched = false;
    match->end = 0;
    match->accept_id = 0;
}

static bool match_take_best(RxMatch *match, size_t end, size_t accept_id, const RxCapture *captures, size_t capture_count, IdmError *err) {
    if (match->matched && (end < match->end || (end == match->end && accept_id >= match->accept_id))) return true;
    RxCapture *copy = capture_count == 0 ? NULL : malloc(capture_count * sizeof(*copy));
    if (capture_count != 0 && !copy) return idm_error_oom(err, idm_span_unknown(NULL));
    if (capture_count != 0) memcpy(copy, captures, capture_count * sizeof(*copy));
    free(match->captures);
    match->captures = copy;
    match->matched = true;
    match->end = end;
    match->accept_id = accept_id;
    return true;
}

static size_t align_up_size(size_t value, size_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static IdmRegexResult *result_alloc_inline(size_t capture_count, IdmError *err) {
    size_t capture_offset = align_up_size(sizeof(IdmRegexResult), _Alignof(RxCapture));
    if (capture_count > (SIZE_MAX - capture_offset) / sizeof(RxCapture)) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    size_t total = capture_offset + capture_count * sizeof(RxCapture);
    IdmRegexResult *result = calloc(1u, total);
    if (!result) {
        idm_error_oom(err, idm_span_unknown(NULL));
        return NULL;
    }
    char *base = (char *)result;
    result->captures = (RxCapture *)(void *)(base + capture_offset);
    result->capture_count = capture_count;
    result->inline_storage = true;
    result->subject_value = idm_nil();
    return result;
}

static bool nfa_run(const RxProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, RxMatch *out, IdmError *err);
static bool nfa_test_run(const RxProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err);
static bool nfa_test_search(const RxProg *prog, const char *s, size_t len, size_t offset, bool *out_matched, IdmError *err);

static bool look_matches(const RxProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    RxMatch match = {0};
    bool ok = nfa_run(prog, s, len, pos, false, 0, false, &match, err);
    bool matched = ok && match.matched;
    match_destroy(&match);
    return ok && matched;
}

static bool lookbehind_matches(const RxProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    for (size_t start = 0; start <= pos; start++) {
        RxMatch match = {0};
        bool ok = nfa_run(prog, s, len, start, true, pos, false, &match, err);
        bool matched = ok && match.matched;
        match_destroy(&match);
        if (!ok) return false;
        if (matched) return true;
    }
    return false;
}

static bool look_test_matches(const RxProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    bool matched = false;
    return nfa_test_run(prog, s, len, pos, false, 0, &matched, err) && matched;
}

static bool lookbehind_test_matches(const RxProg *prog, const char *s, size_t len, size_t pos, IdmError *err) {
    for (size_t start = 0; start <= pos; start++) {
        bool matched = false;
        if (!nfa_test_run(prog, s, len, start, true, pos, &matched, err)) return false;
        if (matched) return true;
    }
    return false;
}

static bool nfa_add_test_closure(const RxProg *prog, RxTestStateVec *vec, uint32_t *marks, const char *s, size_t len, size_t pc, size_t pos, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    if (pc >= prog->count || pos > len) return idm_error_set(err, idm_span_unknown(NULL), "regex VM state out of bounds");
    if (marks[pc] == vec->mark) return true;
    marks[pc] = vec->mark;
    const RxTestClosure *closure = prog->test_closures ? &prog->test_closures[pc] : NULL;
    if (closure && !closure->dynamic) {
        if (closure->has_match && (!exact_end || pos == end_pos)) {
            *out_matched = true;
            return true;
        }
        for (size_t i = 0; i < closure->count; i++) {
            size_t item = closure->pcs[i];
            if (item >= prog->count) return idm_error_set(err, idm_span_unknown(NULL), "regex VM closure state out of bounds");
            if (item != pc) {
                if (marks[item] == vec->mark) continue;
                marks[item] = vec->mark;
            }
            if (!test_state_vec_push(vec, item, err)) return false;
        }
        return true;
    }

    const RxInst *inst = &prog->insts[pc];
    switch (inst->kind) {
        case RXI_JUMP:
            return nfa_add_test_closure(prog, vec, marks, s, len, inst->as.target, pos, exact_end, end_pos, out_matched, err);
        case RXI_SPLIT:
            return nfa_add_test_closure(prog, vec, marks, s, len, inst->as.split.first, pos, exact_end, end_pos, out_matched, err)
                && (*out_matched || nfa_add_test_closure(prog, vec, marks, s, len, inst->as.split.second, pos, exact_end, end_pos, out_matched, err));
        case RXI_SAVE:
            return nfa_add_test_closure(prog, vec, marks, s, len, pc + 1u, pos, exact_end, end_pos, out_matched, err);
        case RXI_ASSERT_START:
            if (pos == 0 || ((prog->flags & IDM_REGEX_MULTILINE) != 0 && at_line_start(s, pos)))
                return nfa_add_test_closure(prog, vec, marks, s, len, pc + 1u, pos, exact_end, end_pos, out_matched, err);
            return true;
        case RXI_ASSERT_END:
            if (pos == len || ((prog->flags & IDM_REGEX_MULTILINE) != 0 && at_line_end(s, len, pos)))
                return nfa_add_test_closure(prog, vec, marks, s, len, pc + 1u, pos, exact_end, end_pos, out_matched, err);
            return true;
        case RXI_LOOK: {
            bool matched = false;
            switch (inst->as.look.kind) {
                case RX_LOOK_AHEAD_POS:
                case RX_LOOK_AHEAD_NEG:
                    matched = look_test_matches(inst->as.look.prog, s, len, pos, err);
                    break;
                case RX_LOOK_BEHIND_POS:
                case RX_LOOK_BEHIND_NEG:
                    matched = lookbehind_test_matches(inst->as.look.prog, s, len, pos, err);
                    break;
            }
            if (err && err->present) return false;
            bool pass = (inst->as.look.kind == RX_LOOK_AHEAD_POS || inst->as.look.kind == RX_LOOK_BEHIND_POS) ? matched : !matched;
            if (pass) return nfa_add_test_closure(prog, vec, marks, s, len, pc + 1u, pos, exact_end, end_pos, out_matched, err);
            return true;
        }
        case RXI_MATCH:
            if (!exact_end || pos == end_pos) *out_matched = true;
            return true;
        case RXI_CHAR:
        case RXI_DOT:
        case RXI_CLASS:
            return test_state_vec_push(vec, pc, err);
    }
    return true;
}

static bool nfa_test_run(const RxProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool *out_matched, IdmError *err) {
    *out_matched = false;
    if (!prog || offset > len || (exact_end && end_pos > len)) return true;
    if (prog->count == 0) return true;
    if (prog->count > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, idm_span_unknown(NULL), "regex VM state set too large");
    uint32_t mark_stack[256];
    uint32_t *marks = prog->count <= (sizeof(mark_stack) / sizeof(mark_stack[0])) ? mark_stack : calloc(prog->count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, prog->count * sizeof(*mark_stack));

    uint32_t next_mark = 1;
    size_t active_stack[128];
    size_t next_stack[128];
    RxTestStateVec active = {
        .items = active_stack,
        .cap = sizeof(active_stack) / sizeof(active_stack[0]),
        .mark = next_mark++,
    };
    RxTestStateVec next = {
        .items = next_stack,
        .cap = sizeof(next_stack) / sizeof(next_stack[0]),
    };
    bool ok = nfa_add_test_closure(prog, &active, marks, s, len, 0, offset, exact_end, end_pos, out_matched, err);
    for (size_t pos = offset; ok && !*out_matched && active.count != 0; pos++) {
        test_state_vec_clear(&next, next_mark++);
        for (size_t i = 0; ok && !*out_matched && i < active.count; i++) {
            size_t pc = active.items[i];
            const RxInst *inst = &prog->insts[pc];
            switch (inst->kind) {
                case RXI_CHAR:
                    if (pos < len && char_eq((unsigned char)s[pos], inst->as.literal, prog->flags))
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, exact_end, end_pos, out_matched, err);
                    break;
                case RXI_DOT:
                    if (pos < len && ((prog->flags & IDM_REGEX_DOTALL) != 0 || s[pos] != '\n'))
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, exact_end, end_pos, out_matched, err);
                    break;
                case RXI_CLASS:
                    if (pos < len && cls_has(&inst->as.cls, (unsigned char)s[pos]))
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, exact_end, end_pos, out_matched, err);
                    break;
                case RXI_MATCH:
                case RXI_JUMP:
                case RXI_SPLIT:
                case RXI_SAVE:
                case RXI_ASSERT_START:
                case RXI_ASSERT_END:
                case RXI_LOOK:
                    break;
            }
        }
        RxTestStateVec tmp = active;
        active = next;
        next = tmp;
    }
    test_state_vec_destroy(&active);
    test_state_vec_destroy(&next);
    if (marks != mark_stack) free(marks);
    return ok;
}

static bool nfa_test_search(const RxProg *prog, const char *s, size_t len, size_t offset, bool *out_matched, IdmError *err) {
    *out_matched = false;
    if (!prog) return true;
    if (offset > len) offset = len;
    if (prog->count == 0) return true;
    if (prog->count > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, idm_span_unknown(NULL), "regex VM state set too large");
    uint32_t mark_stack[256];
    uint32_t *marks = prog->count <= (sizeof(mark_stack) / sizeof(mark_stack[0])) ? mark_stack : calloc(prog->count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, prog->count * sizeof(*mark_stack));

    uint32_t next_mark = 1;
    size_t active_stack[128];
    size_t next_stack[128];
    RxTestStateVec active = {
        .items = active_stack,
        .cap = sizeof(active_stack) / sizeof(active_stack[0]),
        .mark = next_mark++,
    };
    RxTestStateVec next = {
        .items = next_stack,
        .cap = sizeof(next_stack) / sizeof(next_stack[0]),
    };
    bool ok = true;
    for (size_t pos = offset; ok && !*out_matched && pos <= len; pos++) {
        if (regex_start_candidate(prog, s, len, pos))
            ok = nfa_add_test_closure(prog, &active, marks, s, len, 0, pos, false, 0, out_matched, err);
        if (!ok || *out_matched || pos == len) break;
        test_state_vec_clear(&next, next_mark++);
        for (size_t i = 0; ok && !*out_matched && i < active.count; i++) {
            size_t pc = active.items[i];
            const RxInst *inst = &prog->insts[pc];
            switch (inst->kind) {
                case RXI_CHAR:
                    if (char_eq((unsigned char)s[pos], inst->as.literal, prog->flags))
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, false, 0, out_matched, err);
                    break;
                case RXI_DOT:
                    if ((prog->flags & IDM_REGEX_DOTALL) != 0 || s[pos] != '\n')
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, false, 0, out_matched, err);
                    break;
                case RXI_CLASS:
                    if (cls_has(&inst->as.cls, (unsigned char)s[pos]))
                        ok = nfa_add_test_closure(prog, &next, marks, s, len, pc + 1u, pos + 1u, false, 0, out_matched, err);
                    break;
                case RXI_MATCH:
                case RXI_JUMP:
                case RXI_SPLIT:
                case RXI_SAVE:
                case RXI_ASSERT_START:
                case RXI_ASSERT_END:
                case RXI_LOOK:
                    break;
            }
        }
        RxTestStateVec tmp = active;
        active = next;
        next = tmp;
    }
    test_state_vec_destroy(&active);
    test_state_vec_destroy(&next);
    if (marks != mark_stack) free(marks);
    return ok;
}

static bool nfa_add_closure(const RxProg *prog, RxVmStateVec *vec, uint32_t *marks, uint32_t mark, const char *s, size_t len, size_t pc, size_t pos, const RxCapture *captures, bool capture, unsigned depth, IdmError *err) {
    if (pc >= prog->count || pos > len) return idm_error_set(err, idm_span_unknown(NULL), "regex VM state out of bounds");
    if (marks[pc] == mark) return true;
    marks[pc] = mark;
    if (depth > RX_MAX_CLOSURE_DEPTH) return idm_error_set(err, idm_span_unknown(NULL), "regex too complex to evaluate");

    const RxInst *inst = &prog->insts[pc];
    switch (inst->kind) {
        case RXI_JUMP:
            return nfa_add_closure(prog, vec, marks, mark, s, len, inst->as.target, pos, captures, capture, depth + 1u, err);
        case RXI_SPLIT:
            return nfa_add_closure(prog, vec, marks, mark, s, len, inst->as.split.first, pos, captures, capture, depth + 1u, err)
                && nfa_add_closure(prog, vec, marks, mark, s, len, inst->as.split.second, pos, captures, capture, depth + 1u, err);
        case RXI_SAVE: {
            if (!capture) return nfa_add_closure(prog, vec, marks, mark, s, len, pc + 1u, pos, captures, capture, depth + 1u, err);
            RxCapture stack_next[RX_STACK_CAPTURE_LIMIT];
            RxCapture *next = prog->capture_count <= RX_STACK_CAPTURE_LIMIT ? stack_next : malloc(prog->capture_count * sizeof(*next));
            if (!next) return idm_error_oom(err, idm_span_unknown(NULL));
            memcpy(next, captures, prog->capture_count * sizeof(*next));
            size_t capture = inst->as.save_slot / 2u;
            if (capture < prog->capture_count) {
                next[capture].set = true;
                if ((inst->as.save_slot & 1u) == 0) {
                    next[capture].start = pos;
                    next[capture].end = pos;
                } else {
                    next[capture].end = pos;
                }
            }
            bool ok = nfa_add_closure(prog, vec, marks, mark, s, len, pc + 1u, pos, next, capture, depth + 1u, err);
            if (next != stack_next) free(next);
            return ok;
        }
        case RXI_ASSERT_START:
            if (pos == 0 || ((inst->flags & IDM_REGEX_MULTILINE) != 0 && at_line_start(s, pos)))
                return nfa_add_closure(prog, vec, marks, mark, s, len, pc + 1u, pos, captures, capture, depth + 1u, err);
            return true;
        case RXI_ASSERT_END:
            if (pos == len || ((inst->flags & IDM_REGEX_MULTILINE) != 0 && at_line_end(s, len, pos)))
                return nfa_add_closure(prog, vec, marks, mark, s, len, pc + 1u, pos, captures, capture, depth + 1u, err);
            return true;
        case RXI_LOOK: {
            bool matched = false;
            switch (inst->as.look.kind) {
                case RX_LOOK_AHEAD_POS:
                case RX_LOOK_AHEAD_NEG:
                    matched = look_matches(inst->as.look.prog, s, len, pos, err);
                    break;
                case RX_LOOK_BEHIND_POS:
                case RX_LOOK_BEHIND_NEG:
                    matched = lookbehind_matches(inst->as.look.prog, s, len, pos, err);
                    break;
            }
            if (err && err->present) return false;
            bool pass = (inst->as.look.kind == RX_LOOK_AHEAD_POS || inst->as.look.kind == RX_LOOK_BEHIND_POS) ? matched : !matched;
            if (pass) return nfa_add_closure(prog, vec, marks, mark, s, len, pc + 1u, pos, captures, capture, depth + 1u, err);
            return true;
        }
        case RXI_MATCH:
        case RXI_CHAR:
        case RXI_DOT:
        case RXI_CLASS:
            return state_vec_push_copy(vec, pc, pos, captures, err);
    }
    return true;
}

static bool nfa_run(const RxProg *prog, const char *s, size_t len, size_t offset, bool exact_end, size_t end_pos, bool capture, RxMatch *out, IdmError *err) {
    memset(out, 0, sizeof(*out));
    if (!prog || offset > len || (exact_end && end_pos > len)) return true;
    if (prog->count == 0) return true;
    if (prog->count > SIZE_MAX / sizeof(uint32_t)) return idm_error_set(err, idm_span_unknown(NULL), "regex VM state set too large");
    uint32_t mark_stack[256];
    uint32_t *marks = prog->count <= (sizeof(mark_stack) / sizeof(mark_stack[0])) ? mark_stack : calloc(prog->count, sizeof(*marks));
    if (!marks) return idm_error_oom(err, idm_span_unknown(NULL));
    if (marks == mark_stack) memset(mark_stack, 0, prog->count * sizeof(*mark_stack));

    size_t capture_count = capture ? prog->capture_count : 0u;
    RxCapture initial_stack[RX_STACK_CAPTURE_LIMIT];
    RxCapture *initial = capture_count <= RX_STACK_CAPTURE_LIMIT ? initial_stack : calloc(capture_count, sizeof(*initial));
    if (!initial) {
        if (marks != mark_stack) free(marks);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (initial == initial_stack) memset(initial_stack, 0, capture_count * sizeof(*initial_stack));

    RxVmStateVec active = { .capture_count = capture_count };
    RxVmStateVec next = { .capture_count = capture_count };
    idm_profile_count("regex.nfa_run.mark_slots", (uint64_t)prog->count);
    idm_profile_count("regex.nfa_run.program_insts", (uint64_t)prog->count);
    uint32_t mark = 1u;
    bool ok = nfa_add_closure(prog, &active, marks, mark, s, len, 0, offset, initial, capture, 0u, err);
    if (initial != initial_stack) free(initial);
    while (ok && active.count != 0) {
        state_vec_clear(&next);
        mark++;
        if (mark == 0) {
            memset(marks, 0, prog->count * sizeof(*marks));
            mark = 1u;
        }
        for (size_t i = 0; ok && i < active.count; i++) {
            RxVmState *state = &active.items[i];
            const RxCapture *captures = state_captures(&active, state);
            const RxInst *inst = &prog->insts[state->pc];
            switch (inst->kind) {
                case RXI_MATCH:
                    if ((!exact_end || state->pos == end_pos) && !match_take_best(out, state->pos, inst->accept_id, captures, capture_count, err)) ok = false;
                    break;
                case RXI_CHAR:
                    if (state->pos < len && char_eq((unsigned char)s[state->pos], inst->as.literal, inst->flags))
                        ok = nfa_add_closure(prog, &next, marks, mark, s, len, state->pc + 1u, state->pos + 1u, captures, capture, 0u, err);
                    break;
                case RXI_DOT:
                    if (state->pos < len && ((inst->flags & IDM_REGEX_DOTALL) != 0 || s[state->pos] != '\n'))
                        ok = nfa_add_closure(prog, &next, marks, mark, s, len, state->pc + 1u, state->pos + 1u, captures, capture, 0u, err);
                    break;
                case RXI_CLASS:
                    if (state->pos < len && cls_has(&inst->as.cls, (unsigned char)s[state->pos]))
                        ok = nfa_add_closure(prog, &next, marks, mark, s, len, state->pc + 1u, state->pos + 1u, captures, capture, 0u, err);
                    break;
                case RXI_JUMP:
                case RXI_SPLIT:
                case RXI_SAVE:
                case RXI_ASSERT_START:
                case RXI_ASSERT_END:
                case RXI_LOOK:
                    break;
            }
        }
        RxVmStateVec tmp = active;
        active = next;
        next = tmp;
    }
    state_vec_destroy(&active);
    state_vec_destroy(&next);
    if (marks != mark_stack) free(marks);
    if (!ok) match_destroy(out);
    return ok;
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

static IdmRegexResult *result_new(const IdmRegex *rx, IdmValue subject, const char *s, size_t len, size_t start, const RxMatch *match, IdmError *err) {
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
    RxMatch match = {0};
    bool ok = nfa_run(rx->prog, s, len, offset, full, len, true, &match, err);
    if (!ok) return false;
    if (match.matched) *out = result_new(rx, subject, s, len, offset, &match, err);
    match_destroy(&match);
    return !(err && err->present);
}

static bool regex_start_candidate(const RxProg *prog, const char *s, size_t len, size_t pos) {
    if (!prog) return false;
    const RxStartInfo *start = &prog->start;
    if (start->anchored_start && !(pos == 0 || ((prog->flags & IDM_REGEX_MULTILINE) != 0 && at_line_start(s, pos)))) return false;
    if (pos >= len) return start->nullable;
    if (start->any || start->nullable) return true;
    return start->bytes[(unsigned char)s[pos]];
}

static bool scan_from_raw(const IdmRegex *rx, IdmValue subject, const char *s, size_t len, size_t offset, IdmRegexResult **out, IdmError *err) {
    *out = NULL;
    if (offset > len) offset = len;
    for (size_t pos = offset; pos <= len; pos++) {
        if (!regex_start_candidate(rx->prog, s, len, pos)) continue;
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
    return nfa_test_search(rx->prog, input, input_len, 0, out_matched, err);
}

bool idm_regex_match_at(const IdmRegex *rx, const char *input, size_t input_len, size_t offset, size_t *out_end, IdmError *err) {
    if (out_end) *out_end = offset;
    if (!rx) return true;
    RxMatch match = {0};
    bool ok = nfa_run(rx->prog, input, input_len, offset, false, 0, false, &match, err);
    if (ok && match.matched && out_end) *out_end = match.end;
    match_destroy(&match);
    return ok;
}

bool idm_regex_set_match_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, size_t *out_index, size_t *out_end, bool *out_matched, IdmError *err) {
    if (out_index) *out_index = 0;
    if (out_end) *out_end = offset;
    if (out_matched) *out_matched = false;
    if (!set || !set->prog) return true;
    RxMatch match = {0};
    bool ok = nfa_run(set->prog, input, input_len, offset, false, 0, false, &match, err);
    if (ok && match.matched) {
        if (match.accept_id >= set->count) {
            match_destroy(&match);
            return idm_error_set(err, idm_span_unknown(NULL), "regex set accept id out of bounds");
        }
        if (out_index) *out_index = match.accept_id;
        if (out_end) *out_end = match.end;
        if (out_matched) *out_matched = true;
    }
    match_destroy(&match);
    return ok;
}

bool idm_regex_set_exec_at(const IdmRegexSet *set, const char *input, size_t input_len, size_t offset, IdmRegexSetResult *out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "regex.set_exec_at");
    if (!out) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(NULL), "regex set result is required");
    }
    memset(out, 0, sizeof(*out));
    out->end = offset;
    if (!set || !set->prog) {
        idm_profile_scope_end(&prof);
        return true;
    }
    RxMatch match = {0};
    bool ok = nfa_run(set->prog, input, input_len, offset, false, 0, true, &match, err);
    if (ok && match.matched) {
        if (match.accept_id >= set->count) {
            match_destroy(&match);
            idm_profile_scope_end(&prof);
            return idm_error_set(err, idm_span_unknown(NULL), "regex set accept id out of bounds");
        }
        size_t group_count = idm_regex_set_group_count(set, match.accept_id);
        size_t capture_count = group_count + 1u;
        IdmRegexCaptureRange *captures = calloc(capture_count, sizeof(*captures));
        if (!captures) {
            match_destroy(&match);
            idm_profile_scope_end(&prof);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        captures[0].set = true;
        captures[0].start = offset;
        captures[0].end = match.end;
        for (size_t i = 1; i < capture_count; i++) {
            if (!match.captures || i >= set->prog->capture_count || !match.captures[i].set) continue;
            captures[i].set = true;
            captures[i].start = match.captures[i].start;
            captures[i].end = match.captures[i].end;
        }
        out->matched = true;
        out->index = match.accept_id;
        out->end = match.end;
        out->captures = captures;
        out->capture_count = capture_count;
    }
    match_destroy(&match);
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
    if (!set || !set->prog) return true;
    RxMatch match = {0};
    bool ok = nfa_run(set->prog, "", 0, 0, false, 0, false, &match, err);
    if (ok && out) *out = match.matched && match.end == 0;
    match_destroy(&match);
    return ok;
}

static bool rx_put_nullable_string(IdmBuffer *out, const char *text, IdmError *err) {
    if (!idm_buf_put_u8(out, text ? 1u : 0u)) return idm_error_oom(err, idm_span_unknown(NULL));
    if (text && !idm_buf_put_str(out, text, strlen(text))) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool rx_read_nullable_string(IdmByteReader *r, char **out, IdmError *err) {
    *out = NULL;
    uint8_t has = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex set string");
    if (has > 1u) return idm_error_set(err, idm_span_unknown(NULL), "invalid regex set string marker");
    if (!has) return true;
    char *text = idm_rd_string(r, NULL);
    if (!text) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex set string");
    *out = text;
    return true;
}

bool idm_regex_set_serialize(IdmBuffer *out, const IdmRegexSet *set, IdmError *err) {
    if (!set || !set->prog || set->count == 0 || set->count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "regex set is incomplete");
    if (!idm_buf_append_n(out, "RXST", 4u) || !idm_buf_put_u32(out, (uint32_t)set->count)) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < set->count; i++) {
        size_t group_count = set->group_counts ? set->group_counts[i] : 0u;
        if (group_count > UINT32_MAX || !idm_buf_put_u32(out, (uint32_t)group_count)) return idm_error_oom(err, idm_span_unknown(NULL));
        for (size_t g = 1u; g <= group_count; g++) {
            const char *name = set->group_names && set->group_names[i] ? set->group_names[i][g] : NULL;
            if (!rx_put_nullable_string(out, name, err)) return false;
        }
    }
    return rx_prog_serialize_at(out, set->prog, err, 0u);
}

bool idm_regex_set_deserialize(IdmByteReader *r, IdmRegexSet **out, IdmError *err) {
    *out = NULL;
    unsigned char magic[4];
    if (!rx_read_bytes(r, magic, sizeof(magic), err, "regex set magic")) return false;
    if (memcmp(magic, "RXST", 4u) != 0) return idm_error_set(err, idm_span_unknown(NULL), "not a regex set");
    uint32_t count = idm_rd_u32(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated regex set header");
    if (count == 0) return idm_error_set(err, idm_span_unknown(NULL), "regex set is empty");
    IdmRegexSet *set = calloc(1u, sizeof(*set));
    if (!set) return idm_error_oom(err, idm_span_unknown(NULL));
    set->count = count;
    set->group_counts = calloc(count, sizeof(*set->group_counts));
    set->group_names = calloc(count, sizeof(*set->group_names));
    if (!set->group_counts || !set->group_names) {
        idm_regex_set_free(set);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    size_t max_group_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t group_count = idm_rd_u32(r);
        if (!r->ok) {
            idm_regex_set_free(set);
            return idm_error_set(err, idm_span_unknown(NULL), "truncated regex set group count");
        }
        set->group_counts[i] = group_count;
        if (group_count > max_group_count) max_group_count = group_count;
        if (group_count != 0) {
            set->group_names[i] = calloc((size_t)group_count + 1u, sizeof(*set->group_names[i]));
            if (!set->group_names[i]) {
                idm_regex_set_free(set);
                return idm_error_oom(err, idm_span_unknown(NULL));
            }
        }
        for (uint32_t g = 1u; g <= group_count; g++) {
            if (!rx_read_nullable_string(r, &set->group_names[i][g], err)) {
                idm_regex_set_free(set);
                return false;
            }
        }
    }
    if (!rx_prog_deserialize_at(r, &set->prog, err, 0u)) {
        idm_regex_set_free(set);
        return false;
    }
    if (set->prog->capture_count < max_group_count + 1u) {
        idm_regex_set_free(set);
        return idm_error_set(err, idm_span_unknown(NULL), "regex set capture table is too small");
    }
    if (!rx_prog_validate(set->prog, set->count, err)) {
        idm_regex_set_free(set);
        return false;
    }
    *out = set;
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

    RxParser p = { source, source_len, 0, flags, NULL, 0, err, 0 };
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
    node_free(rx->root);
    rx->root = NULL;
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
    prog_free(rx->prog);
    if (rx->group_names) {
        for (size_t i = 0; i <= rx->group_count; i++) free(rx->group_names[i]);
        free(rx->group_names);
    }
    free(rx);
}

size_t idm_regex_footprint(const IdmRegex *rx) {
    if (!rx) return 0;
    size_t total = sizeof(*rx) + rx->source_len + 1u + node_footprint(rx->root) + prog_footprint(rx->prog) + (rx->group_count + 1u) * sizeof(*rx->group_names);
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
    return rx && rx->prog && rx->prog->start.nullable;
}

size_t idm_regex_set_group_count(const IdmRegexSet *set, size_t item_index) {
    if (!set || item_index >= set->count || !set->group_counts) return 0;
    return set->group_counts[item_index];
}

const char *idm_regex_set_group_name(const IdmRegexSet *set, size_t item_index, size_t group_index) {
    if (!set || item_index >= set->count || !set->group_names || !set->group_counts || group_index > set->group_counts[item_index]) return NULL;
    return set->group_names[item_index] ? set->group_names[item_index][group_index] : NULL;
}

void idm_regex_set_result_destroy(IdmRegexSetResult *result) {
    if (!result) return;
    free(result->captures);
    memset(result, 0, sizeof(*result));
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
    if (!require_string_arg(rt, "raw-compile", source, &s, &len, err)) return false;
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
    RxCapture c = r->captures[0];
    *out = idm_string_n(rt, r->subject + c.start, c.end - c.start, err);
    return !(err && err->present);
}

static bool capture_index(IdmValue index, size_t max, size_t *out, IdmError *err) {
    if (idm_value_tag(index) != IDM_VAL_INT || idm_int_value(index) < 0 || (uint64_t)idm_int_value(index) >= max) {
        return idm_error_set(err, idm_span_unknown(NULL), "capture index out of range");
    }
    *out = (size_t)idm_int_value(index);
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
    if (!require_string_arg(rt, "raw-replace", replacement, &repl, &repl_len, err)) return false;
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
    if (!require_string_arg(rt, "raw-escape", input, &s, &len, err)) return false;
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
