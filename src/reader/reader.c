#include "idiom/reader.h"

#include "idiom/artifact.h"
#include "idiom/regex.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define IDM_READER_START_RULE "program"

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_STRING,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE
} TokenKind;

typedef struct {
    TokenKind kind;
    char *lexeme;
    IdmSpan span;
    bool leading_space;
    bool adjacent_previous;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t cap;
} TokenVec;

typedef struct {
    const char *file;
    const char *src;
    size_t len;
    size_t pos;
    unsigned line;
    unsigned column;
    size_t previous_end;
    bool *incomplete;
} Lexer;

typedef struct {
    Token *tokens;
    size_t count;
    size_t pos;
    const char *file;
    IdmError *err;
    bool *incomplete;
} Parser;

static void reader_mark_incomplete(bool *incomplete) {
    if (incomplete) *incomplete = true;
}


static void tokens_destroy(TokenVec *vec) {
    for (size_t i = 0; i < vec->count; i++) free(vec->items[i].lexeme);
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static bool tokens_push(TokenVec *vec, Token tok) {
    if (vec->count == vec->cap) {
        if (!idm_grow((void **)&vec->items, &vec->cap, sizeof(*vec->items), 64u, vec->count + 1u)) return false;
    }
    vec->items[vec->count++] = tok;
    return true;
}

static char peek(Lexer *lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : '\0';
}

static char advance(Lexer *lx) {
    char ch = peek(lx);
    if (ch == '\0') return ch;
    lx->pos++;
    if (ch == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    return ch;
}

static bool add_token(TokenVec *vec, Lexer *lx, TokenKind kind, size_t start, unsigned line, unsigned column, bool leading_space) {
    Token tok;
    tok.kind = kind;
    tok.lexeme = idm_strndup(lx->src + start, lx->pos - start);
    if (!tok.lexeme) return false;
    tok.span.file = lx->file;
    tok.span.start = start;
    tok.span.end = lx->pos;
    tok.span.line = line;
    tok.span.column = column;
    tok.leading_space = leading_space;
    tok.adjacent_previous = start == lx->previous_end;
    lx->previous_end = lx->pos;
    if (!tokens_push(vec, tok)) {
        free(tok.lexeme);
        return false;
    }
    return true;
}

static size_t scan_string_end(const char *s, size_t start, size_t len, bool *closed);

static bool scan_stack_push(char **stack, size_t *count, size_t *cap, char close) {
    if (*count == *cap) {
        if (!idm_grow((void **)stack, cap, sizeof(**stack), 16u, *count + 1u)) return false;
    }
    (*stack)[(*count)++] = close;
    return true;
}

static size_t scan_interp_end(const char *s, size_t start, size_t len) {
    size_t j = start;
    char fixed[64];
    char *stack = fixed;
    size_t count = 0;
    size_t cap = sizeof(fixed);
    if (!scan_stack_push(&stack, &count, &cap, '}')) return len;
    while (j < len && s[j] != '\0') {
        char c = s[j];
        if (c == '"') {
            bool str_closed = false;
            size_t end = scan_string_end(s, j, len, &str_closed);
            if (!str_closed) {
                if (stack != fixed) free(stack);
                return len;
            }
            j = end;
            continue;
        }
        if (c == '\\') {
            j += j + 1u < len ? 2u : 1u;
            continue;
        }
        char close = 0;
        if (c == '{') close = '}';
        else if (c == '[') close = ']';
        else if (c == '(') close = ')';
        if (close != 0) {
            if (!scan_stack_push(&stack, &count, &cap, close)) {
                if (stack != fixed) free(stack);
                return len;
            }
            j++;
            continue;
        }
        if (c == '}' || c == ']' || c == ')') {
            if (count == 0 || stack[count - 1u] != c) {
                if (stack != fixed) free(stack);
                return len;
            }
            count--;
            if (count == 0) {
                if (stack != fixed) free(stack);
                return j;
            }
        }
        j++;
    }
    if (stack != fixed) free(stack);
    return len;
}

static size_t scan_string_end(const char *s, size_t start, size_t len, bool *closed) {
    if (closed) *closed = false;
    if (start >= len || s[start] != '"') return len;
    size_t j = start + 1u;
    while (j < len && s[j] != '\0') {
        char c = s[j];
        if (c == '\\') {
            j += j + 1u < len ? 2u : 1u;
            continue;
        }
        if ((c == '$' || c == '#') && j + 1u < len && s[j + 1u] == '{') {
            size_t close = scan_interp_end(s, j + 2u, len);
            if (close >= len) return len;
            j = close + 1u;
            continue;
        }
        if (c == '"') {
            if (closed) *closed = true;
            return j + 1u;
        }
        j++;
    }
    return len;
}

static size_t scan_bitstring_end(const char *s, size_t start, size_t len, bool *open_at_eof) {
    if (open_at_eof) *open_at_eof = false;
    if (start + 1u >= len || s[start] != '%' || s[start + 1u] != '<') return len;
    size_t j = start + 2u;
    long depth = 0;
    while (j < len && s[j] != '\0') {
        char c = s[j];
        if (c == '"') {
            bool str_closed = false;
            size_t close = scan_string_end(s, j, len, &str_closed);
            if (!str_closed) {
                if (open_at_eof) *open_at_eof = true;
                return len;
            }
            j = close;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') { depth++; j++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth == 0) return len; depth--; j++; continue; }
        if (c == '>' && depth == 0) return j + 1u;
        j++;
    }
    if (open_at_eof) *open_at_eof = true;
    return len;
}

static Token *cur(Parser *p) { return &p->tokens[p->pos]; }
static bool at(Parser *p, TokenKind kind) { return cur(p)->kind == kind; }
static Token *take(Parser *p) { return &p->tokens[p->pos++]; }

static IdmSyntax *set_form_token(IdmSyntax *syn, const Token *tok) {
    if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
    return syn;
}

static bool syntax_is_reader_form(const IdmSyntax *syn, const char *head) {
    return syn && syn->kind == IDM_SYN_LIST && syn->as.seq.count > 0 &&
           syn->as.seq.items[0]->kind == IDM_SYN_WORD &&
           strcmp(syn->as.seq.items[0]->as.text, head) == 0;
}

static bool sequence_has_reader_head(const IdmSyntax *seq) {
    return seq && seq->kind == IDM_SYN_LIST && seq->as.seq.count > 0 &&
           seq->as.seq.items[0]->kind == IDM_SYN_WORD &&
           strncmp(seq->as.seq.items[0]->as.text, "%-", 2u) == 0;
}

static bool append_reader_part(IdmSyntax *seq, IdmSyntax *part) {
    if (!seq || !part) return false;
    size_t min_count = sequence_has_reader_head(seq) ? 2u : 1u;
    if (part->token_adjacent_previous && seq->as.seq.count >= min_count) {
        IdmSyntax *last = seq->as.seq.items[seq->as.seq.count - 1u];
        if (syntax_is_reader_form(last, "%-adjacent")) {
            return idm_syn_append(last, part);
        }
        if (!last->token_raw) return idm_syn_append(seq, part);

        IdmSyntax *run = idm_syn_list(last->span);
        IdmSyntax *head = idm_syn_word("%-adjacent", last->span);
        if (!run || !head || !idm_syn_append(run, head)) {
            idm_syn_free(run);
            idm_syn_free(head);
            return false;
        }
        if (!idm_syn_append(run, last)) {
            idm_syn_free(run);
            return false;
        }
        seq->as.seq.items[seq->as.seq.count - 1u] = run;
        if (!idm_syn_append(run, part)) return false;
        return true;
    }
    return idm_syn_append(seq, part);
}

static bool is_transparent_delim(char ch) {
    return ch == '\0' || isspace((unsigned char)ch) || ch == '(' || ch == ')' ||
           ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == ';' || ch == '"';
}

static bool read_transparent_string_token(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space, IdmError *err) {
    (void)advance(lx);
    IdmBuffer decoded;
    idm_buf_init(&decoded);
    while (peek(lx) != '\0' && peek(lx) != '"') {
        char ch = advance(lx);
        if (ch == '\\') {
            char e = advance(lx);
            if (!idm_reader_string_escape(e, &ch)) {
                idm_buf_destroy(&decoded);
                return idm_error_set(err, (IdmSpan){lx->file, lx->pos - 1u, lx->pos, lx->line, lx->column - 1u}, "unknown string escape");
            }
        }
        if (!idm_buf_append_char(&decoded, ch)) {
            idm_buf_destroy(&decoded);
            return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
        }
    }
    if (peek(lx) != '"') {
        idm_buf_destroy(&decoded);
        reader_mark_incomplete(lx->incomplete);
        return idm_error_set(err, (IdmSpan){lx->file, start, lx->pos, line, column}, "unterminated string");
    }
    advance(lx);
    Token tok;
    tok.kind = TOK_STRING;
    tok.lexeme = idm_buf_take(&decoded);
    if (!tok.lexeme) return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
    tok.span = (IdmSpan){lx->file, start, lx->pos, line, column};
    tok.leading_space = leading_space;
    tok.adjacent_previous = start == lx->previous_end;
    lx->previous_end = lx->pos;
    if (!tokens_push(vec, tok)) {
        free(tok.lexeme);
        return idm_error_oom(err, tok.span);
    }
    return true;
}

static bool read_transparent_word_token(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space) {
    while (!is_transparent_delim(peek(lx))) advance(lx);
    return add_token(vec, lx, TOK_IDENT, start, line, column, leading_space);
}

static bool lex_transparent_from(const char *file, const char *source, size_t len, size_t start_pos, unsigned start_line, unsigned start_column, TokenVec *out, bool *incomplete, IdmError *err) {
    Lexer lx = {file, source, len, start_pos, start_line, start_column, start_pos, incomplete};
    bool leading_space = false;
    while (peek(&lx) != '\0') {
        char ch = peek(&lx);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ';') {
            leading_space = true;
            advance(&lx);
            continue;
        }
        if (ch == '#') {
            while (peek(&lx) != '\0' && peek(&lx) != '\n') advance(&lx);
            leading_space = true;
            continue;
        }
        size_t start = lx.pos;
        unsigned line = lx.line;
        unsigned column = lx.column;
        if (ch == '(') { advance(&lx); if (!add_token(out, &lx, TOK_LPAREN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ')') { advance(&lx); if (!add_token(out, &lx, TOK_RPAREN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '[') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACKET, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ']') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACKET, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '{') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '}') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '"') {
            if (!read_transparent_string_token(out, &lx, start, line, column, leading_space, err)) return false;
            leading_space = false;
            continue;
        }
        if (!read_transparent_word_token(out, &lx, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
        leading_space = false;
    }
    size_t start = lx.pos;
    unsigned line = lx.line;
    unsigned column = lx.column;
    return add_token(out, &lx, TOK_EOF, start, line, column, leading_space) || idm_error_oom(err, (IdmSpan){file, start, start, line, column});
}

static IdmSyntax *transparent_sequence(IdmSyntaxKind kind, IdmSpan span) {
    return idm_syn_seq(kind, span);
}

static bool transparent_parse_integer(const char *text, int64_t *out) {
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || end == text) return false;
    *out = value;
    return true;
}

static bool is_decimal_integer(const char *text) {
    if (!text || !*text) return false;
    size_t i = (text[0] == '+' || text[0] == '-') ? 1u : 0u;
    if (!text[i]) return false;
    for (; text[i]; i++) if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

static bool transparent_parse_float(const char *text, double *out) {
    if (!strchr(text, '.') && !strchr(text, 'e') && !strchr(text, 'E')) return false;
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0' || end == text) return false;
    *out = value;
    return true;
}

static IdmSyntax *reader_classify_token(const char *raw, size_t len, IdmSpan span, IdmError *err) {
    char *text = idm_strndup(raw, len);
    if (!text) {
        idm_error_oom(err, span);
        return NULL;
    }
    IdmSyntax *syn = NULL;
    if (text[0] == ':' && text[1] != '\0') {
        syn = strcmp(text, ":nil") == 0 ? idm_syn_nil(span) : idm_syn_atom(text + 1, span);
    } else {
        int64_t integer = 0;
        double real = 0.0;
        if (transparent_parse_integer(text, &integer)) syn = idm_syn_int(integer, span);
        else if (is_decimal_integer(text)) syn = idm_syn_bigint(text, span);
        else if (transparent_parse_float(text, &real)) syn = idm_syn_float(real, span);
        else syn = idm_syn_word(text, span);
    }
    free(text);
    if (!syn) idm_error_oom(err, span);
    return syn;
}

static IdmSyntax *parse_transparent_one(Parser *p);

static IdmSyntax *parse_transparent_container(Parser *p, IdmSyntaxKind kind, TokenKind close, IdmSpan span) {
    IdmSyntax *seq = transparent_sequence(kind, span);
    if (!seq) {
        idm_error_oom(p->err, span);
        return NULL;
    }
    while (!at(p, TOK_EOF) && !at(p, close)) {
        IdmSyntax *item = parse_transparent_one(p);
        if (!item || !idm_syn_append(seq, item)) {
            idm_syn_free(item);
            idm_syn_free(seq);
            if (!(p->err && p->err->present)) idm_error_oom(p->err, span);
            return NULL;
        }
    }
    if (!at(p, close)) {
        idm_syn_free(seq);
        reader_mark_incomplete(p->incomplete);
        idm_error_set(p->err, span, "unterminated transparent reader container");
        return NULL;
    }
    take(p);
    return seq;
}

static IdmSyntax *parse_transparent_token(Token *tok, IdmError *err) {
    if (tok->kind == TOK_STRING) {
        IdmSyntax *syn = idm_syn_string(tok->lexeme, tok->span);
        return set_form_token(syn, tok);
    }
    IdmSyntax *syn = reader_classify_token(tok->lexeme, strlen(tok->lexeme), tok->span, err);
    return set_form_token(syn, tok);
}

static IdmSyntax *parse_transparent_one(Parser *p) {
    Token *tok = take(p);
    switch (tok->kind) {
        case TOK_IDENT:
        case TOK_STRING:
            return parse_transparent_token(tok, p->err);
        case TOK_LPAREN:
            return set_form_token(parse_transparent_container(p, IDM_SYN_LIST, TOK_RPAREN, tok->span), tok);
        case TOK_LBRACKET:
            return set_form_token(parse_transparent_container(p, IDM_SYN_VECTOR, TOK_RBRACKET, tok->span), tok);
        case TOK_LBRACE:
            return set_form_token(parse_transparent_container(p, IDM_SYN_TUPLE, TOK_RBRACE, tok->span), tok);
        case TOK_RPAREN:
        case TOK_RBRACKET:
        case TOK_RBRACE:
            idm_error_set(p->err, tok->span, "unexpected transparent reader close delimiter");
            return NULL;
        case TOK_EOF:
            idm_error_set(p->err, tok->span, "expected transparent reader item");
            return NULL;
        default:
            idm_error_set(p->err, tok->span, "unsupported transparent reader token");
            return NULL;
    }
}

static IdmSyntax *parse_transparent_program(Parser *p) {
    IdmSyntax *program = idm_syn_list(idm_span_unknown(p->file));
    if (!program) {
        idm_error_oom(p->err, idm_span_unknown(p->file));
        return NULL;
    }
    while (!at(p, TOK_EOF)) {
        IdmSyntax *form = parse_transparent_one(p);
        if (!form || !idm_syn_append(program, form)) {
            idm_syn_free(form);
            idm_syn_free(program);
            if (!(p->err && p->err->present)) idm_error_oom(p->err, idm_span_unknown(p->file));
            return NULL;
        }
    }
    return program;
}

static void reader_artifact_advance_position(const char *source, size_t start, size_t end, unsigned *line, unsigned *column);

typedef struct ReaderArtifactRule ReaderArtifactRule;

typedef struct {
    uint8_t op;
    char *name;
    uint8_t target_kind;
    bool has_literal;
    bool nullable;
    size_t literal_index;
    size_t first_child;
    size_t child_count;
    size_t next;
    size_t target_index;
    size_t capture_slot;
    IdmSyntax *literal;
} ReaderReductionInst;

struct ReaderArtifactRule {
    char *name;
    char *provider;
    char *provider_key;
    IdmScopeSet binding_scopes;
    uint8_t kind;
    size_t order;
    size_t capture_slot;
    size_t literal_index;
    size_t *terminal_capture_slots;
    size_t terminal_capture_slot_count;
    IdmGrammarTerminal terminal;
    IdmReaderProgram pattern;
    ReaderReductionInst *reductions;
    size_t reduction_count;
    uint64_t *first;
    IdmReaderProgram constructor;
};

typedef struct {
    size_t open;
    size_t close;
} ReaderArtifactPair;

struct IdmReaderArtifact {
    char *surface;
    int phase;
    ReaderArtifactRule *tokens;
    size_t token_count;
    size_t token_cap;
    ReaderArtifactRule *forms;
    size_t form_count;
    size_t form_cap;
    size_t start_form;
    size_t first_words;
    ReaderArtifactPair *pairs;
    size_t pair_count;
    ReaderArtifactRule *skips;
    size_t skip_count;
    size_t skip_cap;
    IdmSyntax **literals;
    size_t literal_count;
    size_t literal_cap;
    const ReaderArtifactRule **lex_rules;
    size_t lex_count;
    size_t rule_order;
    IdmRegexSet *terminal_program;
    bool literal_token_indices_ready;
    char **capture_names;
    size_t capture_name_count;
    size_t capture_name_cap;
};

static bool reader_artifact_terminal_uses_regex_program(uint8_t kind) {
    return kind == (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX || kind == (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL;
}

typedef struct {
    const ReaderArtifactRule *rule;
    char *lexeme;
    IdmSyntax *syntax;
    IdmSpan span;
    bool leading_space;
    bool adjacent_previous;
    size_t literal_index;
} ReaderArtifactToken;

typedef struct {
    ReaderArtifactToken *items;
    size_t count;
    size_t cap;
} ReaderArtifactTokenVec;

typedef struct {
    IdmSyntax *syntax;
    char *text;
} ReaderArtifactCaptureItem;

typedef struct {
    ReaderArtifactCaptureItem *items;
    size_t count;
    size_t cap;
} ReaderArtifactCapture;

typedef struct {
    ReaderArtifactCapture *items;
    size_t count;
    size_t cap;
} ReaderArtifactCaptures;

typedef struct {
    size_t count;
    size_t *item_counts;
} ReaderArtifactCaptureCheckpoint;

static const ReaderArtifactRule *reader_artifact_find_form_rule(const IdmReaderArtifact *artifact, const char *name);
static bool reader_artifact_resolve_rules(IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_resolve_capture_slots(IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_validate_reductions(IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_build_lex_rules(IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_validate_terminal_program(const IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_compile_terminal_program(IdmReaderArtifact *artifact, IdmError *err);
static bool reader_artifact_index_literals(IdmReaderArtifact *artifact, IdmError *err);

static void reader_reduction_inst_destroy(ReaderReductionInst *inst) {
    if (!inst) return;
    free(inst->name);
    idm_syn_free(inst->literal);
    memset(inst, 0, sizeof(*inst));
}

static void reader_reductions_destroy(ReaderArtifactRule *rule) {
    if (!rule) return;
    for (size_t i = 0; i < rule->reduction_count; i++) reader_reduction_inst_destroy(&rule->reductions[i]);
    free(rule->reductions);
    rule->reductions = NULL;
    rule->reduction_count = 0;
    free(rule->first);
    rule->first = NULL;
}

static void reader_artifact_rule_destroy(ReaderArtifactRule *rule) {
    if (!rule) return;
    free(rule->name);
    free(rule->provider);
    free(rule->provider_key);
    free(rule->terminal_capture_slots);
    idm_scope_set_destroy(&rule->binding_scopes);
    idm_grammar_terminal_destroy(&rule->terminal);
    idm_reader_program_destroy(&rule->pattern);
    reader_reductions_destroy(rule);
    idm_reader_program_destroy(&rule->constructor);
    memset(rule, 0, sizeof(*rule));
}

void idm_reader_artifact_destroy(IdmReaderArtifact *artifact) {
    if (!artifact) return;
    free(artifact->surface);
    for (size_t i = 0; i < artifact->token_count; i++) reader_artifact_rule_destroy(&artifact->tokens[i]);
    for (size_t i = 0; i < artifact->form_count; i++) reader_artifact_rule_destroy(&artifact->forms[i]);
    for (size_t i = 0; i < artifact->skip_count; i++) reader_artifact_rule_destroy(&artifact->skips[i]);
    free(artifact->tokens);
    free(artifact->forms);
    free(artifact->skips);
    for (size_t i = 0; i < artifact->literal_count; i++) idm_syn_free(artifact->literals[i]);
    free(artifact->literals);
    free(artifact->lex_rules);
    for (size_t i = 0; i < artifact->capture_name_count; i++) free(artifact->capture_names[i]);
    free(artifact->capture_names);
    free(artifact->pairs);
    idm_regex_set_free(artifact->terminal_program);
    free(artifact);
}

static bool reader_artifact_literal_intern(IdmReaderArtifact *artifact, const IdmSyntax *literal, size_t *out_index, IdmError *err) {
    if (!artifact || !literal || !out_index) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact literal table is incomplete");
    for (size_t i = 0; i < artifact->literal_count; i++) {
        if (idm_syn_equal(artifact->literals[i], literal)) {
            *out_index = i;
            return true;
        }
    }
    if (artifact->literal_count == artifact->literal_cap) {
        if (!idm_grow((void **)&artifact->literals, &artifact->literal_cap, sizeof(*artifact->literals), 16u, artifact->literal_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    artifact->literals[artifact->literal_count] = idm_syn_clone(literal);
    if (!artifact->literals[artifact->literal_count]) return idm_error_oom(err, literal->span);
    *out_index = artifact->literal_count++;
    return true;
}

static bool reader_artifact_reduction_next_valid(const ReaderArtifactRule *rule, size_t pc, size_t *out_next, IdmError *err) {
    if (!rule || pc >= rule->reduction_count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact reduction table ended early");
    const ReaderReductionInst *inst = &rule->reductions[pc];
    if (inst->child_count == 0) {
        if (inst->first_child != SIZE_MAX || inst->next != pc + 1u) return idm_error_set(err, idm_span_unknown(NULL), "invalid reader reduction leaf range");
        *out_next = pc + 1u;
        return true;
    }
    if (inst->first_child != pc + 1u) return idm_error_set(err, idm_span_unknown(NULL), "invalid reader reduction child start");
    size_t next = inst->first_child;
    for (size_t i = 0; i < inst->child_count; i++) {
        if (!reader_artifact_reduction_next_valid(rule, next, &next, err)) return false;
    }
    if (inst->next != next) return idm_error_set(err, idm_span_unknown(NULL), "invalid reader reduction child range");
    *out_next = next;
    return true;
}

static bool reader_artifact_validate_reduction_table(const ReaderArtifactRule *rule, IdmError *err) {
    if (!rule || rule->kind != (uint8_t)IDM_GRAMMAR_RULE_FORM) {
        if (rule && rule->reduction_count != 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal rule has reduction table");
        return true;
    }
    if (rule->reduction_count == 0 || !rule->reductions) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact form rule has no reduction table");
    size_t next = 0;
    if (!reader_artifact_reduction_next_valid(rule, 0, &next, err)) return false;
    if (next != rule->reduction_count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact reduction table has trailing instructions");
    return true;
}


static void reader_artifact_tokens_destroy(ReaderArtifactTokenVec *vec) {
    for (size_t i = 0; i < vec->count; i++) {
        free(vec->items[i].lexeme);
        idm_syn_free(vec->items[i].syntax);
    }
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static bool reader_artifact_tokens_push(ReaderArtifactTokenVec *vec, ReaderArtifactToken token, IdmError *err) {
    if (vec->count == vec->cap) {
        if (!idm_grow((void **)&vec->items, &vec->cap, sizeof(*vec->items), 64u, vec->count + 1u)) return idm_error_oom(err, token.span);
    }
    vec->items[vec->count++] = token;
    return true;
}

static bool reader_artifact_decode_string(const char *raw, size_t len, IdmBuffer *out) {
    if (len < 2u || raw[0] != '"' || raw[len - 1u] != '"') return false;
    for (size_t i = 1u; i + 1u < len; i++) {
        char ch = raw[i];
        if (ch == '\\') {
            if (i + 2u >= len) return false;
            char e = raw[++i];
            if (!idm_reader_string_escape(e, &ch)) return false;
        }
        if (!idm_buf_append_char(out, ch)) return false;
    }
    return true;
}

static IdmSyntax *reader_artifact_default_token_syntax(const char *raw, size_t len, IdmSpan span, IdmError *err) {
    if (len >= 2u && raw[0] == '"') {
        IdmBuffer decoded;
        idm_buf_init(&decoded);
        bool ok = reader_artifact_decode_string(raw, len, &decoded);
        if (!ok) {
            idm_buf_destroy(&decoded);
            idm_error_set(err, span, "invalid artifact string token");
            return NULL;
        }
        IdmSyntax *syn = idm_syn_string_n(decoded.data ? decoded.data : "", decoded.len, span);
        idm_buf_destroy(&decoded);
        if (!syn) idm_error_oom(err, span);
        return syn;
    }
    return reader_classify_token(raw, len, span, err);
}

static void reader_artifact_capture_item_destroy(ReaderArtifactCaptureItem *item) {
    if (!item) return;
    idm_syn_free(item->syntax);
    free(item->text);
    memset(item, 0, sizeof(*item));
}

static void reader_artifact_capture_destroy(ReaderArtifactCapture *cap) {
    if (!cap) return;
    for (size_t i = 0; i < cap->count; i++) reader_artifact_capture_item_destroy(&cap->items[i]);
    free(cap->items);
    memset(cap, 0, sizeof(*cap));
}

static void reader_artifact_captures_destroy(ReaderArtifactCaptures *captures) {
    for (size_t i = 0; i < captures->count; i++) reader_artifact_capture_destroy(&captures->items[i]);
    free(captures->items);
    memset(captures, 0, sizeof(*captures));
}

static bool reader_artifact_captures_init(ReaderArtifactCaptures *captures, size_t slot_count, IdmError *err, IdmSpan span) {
    memset(captures, 0, sizeof(*captures));
    if (slot_count == 0) return true;
    captures->items = calloc(slot_count, sizeof(*captures->items));
    if (!captures->items) return idm_error_oom(err, span);
    captures->count = slot_count;
    captures->cap = slot_count;
    return true;
}

static ReaderArtifactCapture *reader_artifact_capture_at(ReaderArtifactCaptures *captures, size_t slot) {
    return slot < captures->count ? &captures->items[slot] : NULL;
}

static bool reader_artifact_capture_add_text(ReaderArtifactCaptures *captures, size_t slot, const IdmSyntax *value, const char *text, size_t text_len, IdmError *err, IdmSpan span) {
    ReaderArtifactCapture *cap = reader_artifact_capture_at(captures, slot);
    if (!cap) return idm_error_set(err, span, "reader artifact capture slot %zu out of bounds", slot);
    if (cap->count == cap->cap) {
        if (!idm_grow((void **)&cap->items, &cap->cap, sizeof(*cap->items), 4u, cap->count + 1u)) return idm_error_oom(err, span);
    }
    IdmSyntax *clone = idm_syn_clone(value);
    if (!clone) return idm_error_oom(err, span);
    char *copy = text ? idm_strndup(text, text_len) : NULL;
    if (text && !copy) {
        idm_syn_free(clone);
        return idm_error_oom(err, span);
    }
    ReaderArtifactCaptureItem *item = &cap->items[cap->count++];
    memset(item, 0, sizeof(*item));
    item->syntax = clone;
    item->text = copy;
    return true;
}

static bool reader_artifact_capture_add(ReaderArtifactCaptures *captures, size_t slot, const IdmSyntax *value, IdmError *err, IdmSpan span) {
    return reader_artifact_capture_add_text(captures, slot, value, NULL, 0, err, span);
}

static bool reader_artifact_capture_checkpoint(ReaderArtifactCaptures *captures, ReaderArtifactCaptureCheckpoint *checkpoint, IdmError *err, IdmSpan span) {
    checkpoint->count = captures->count;
    checkpoint->item_counts = NULL;
    if (captures->count == 0) return true;
    checkpoint->item_counts = calloc(captures->count, sizeof(*checkpoint->item_counts));
    if (!checkpoint->item_counts) return idm_error_oom(err, span);
    for (size_t i = 0; i < captures->count; i++) checkpoint->item_counts[i] = captures->items[i].count;
    return true;
}

static void reader_artifact_capture_checkpoint_commit(ReaderArtifactCaptureCheckpoint *checkpoint) {
    free(checkpoint->item_counts);
    checkpoint->item_counts = NULL;
    checkpoint->count = 0;
}

static void reader_artifact_capture_checkpoint_rollback(ReaderArtifactCaptures *captures, ReaderArtifactCaptureCheckpoint *checkpoint) {
    for (size_t i = 0; i < captures->count; i++) {
        ReaderArtifactCapture *cap = &captures->items[i];
        size_t keep = checkpoint->item_counts ? checkpoint->item_counts[i] : 0;
        while (cap->count > keep) reader_artifact_capture_item_destroy(&cap->items[--cap->count]);
    }
    reader_artifact_capture_checkpoint_commit(checkpoint);
}

static bool reader_artifact_is_dict_entry(const IdmSyntax *syn);

static bool reader_artifact_sequence_append(IdmSyntax *seq, IdmSyntax *item, IdmError *err, IdmSpan span) {
    if (!item) return idm_error_oom(err, span);
    if (seq && seq->kind == IDM_SYN_DICT && reader_artifact_is_dict_entry(item)) {
        IdmSyntax *key = item->as.seq.items[1];
        IdmSyntax *value = item->as.seq.items[2];
        item->as.seq.items[1] = NULL;
        item->as.seq.items[2] = NULL;
        idm_syn_free(item);
        bool ok = reader_artifact_sequence_append(seq, key, err, span) && reader_artifact_sequence_append(seq, value, err, span);
        return ok;
    }
    if (seq && seq->kind == IDM_SYN_LIST && seq->as.seq.count > 0 &&
        seq->as.seq.items[0]->kind == IDM_SYN_WORD &&
        strcmp(seq->as.seq.items[0]->as.text, "%-expr") == 0) {
        if (append_reader_part(seq, item)) return true;
    } else if (idm_syn_append(seq, item)) {
        return true;
    }
    idm_syn_free(item);
    return idm_error_oom(err, span);
}

static bool reader_artifact_is_package_begin(const IdmSyntax *syn) {
    return syn && syn->kind == IDM_SYN_LIST && syn->as.seq.count > 0 &&
           syn->as.seq.items[0]->kind == IDM_SYN_WORD &&
           strcmp(syn->as.seq.items[0]->as.text, "%-package-begin") == 0;
}

static bool reader_artifact_is_dict_entry(const IdmSyntax *syn) {
    return syn && syn->kind == IDM_SYN_LIST && syn->as.seq.count == 3u &&
           syn->as.seq.items[0]->kind == IDM_SYN_WORD &&
           strcmp(syn->as.seq.items[0]->as.text, "%-dict-entry") == 0;
}

static const ReaderArtifactRule *reader_artifact_find_token_rule(const IdmReaderArtifact *artifact, const char *name) {
    for (size_t i = 0; i < artifact->token_count; i++) {
        if (strcmp(artifact->tokens[i].name, name) == 0) return &artifact->tokens[i];
    }
    return NULL;
}

static const ReaderArtifactRule *reader_artifact_find_form_rule(const IdmReaderArtifact *artifact, const char *name) {
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (strcmp(artifact->forms[i].name, name) == 0) return &artifact->forms[i];
    }
    return NULL;
}

static const ReaderArtifactRule *reader_artifact_find_skip_rule(const IdmReaderArtifact *artifact, const char *name) {
    for (size_t i = 0; i < artifact->skip_count; i++) {
        if (strcmp(artifact->skips[i].name, name) == 0) return &artifact->skips[i];
    }
    return NULL;
}

static bool reader_artifact_form_rule_index(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t *out) {
    if (!rule || rule < artifact->forms || rule >= artifact->forms + artifact->form_count) return false;
    *out = (size_t)(rule - artifact->forms);
    return true;
}

static bool reader_artifact_token_rule_index(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t *out) {
    if (!rule || rule < artifact->tokens || rule >= artifact->tokens + artifact->token_count) return false;
    *out = (size_t)(rule - artifact->tokens);
    return true;
}

static bool reader_artifact_resolve_pattern_program(const IdmReaderArtifact *artifact, IdmReaderProgram *program, IdmError *err) {
    if (!program || program->count == 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact pattern is missing");
    for (size_t i = 0; i < program->count; i++) {
        IdmReaderInst *inst = &program->items[i];
        inst->target_kind = IDM_READER_PATTERN_TARGET_NONE;
        inst->target_index = 0;
        if (inst->op == IDM_READER_PATTERN_REF) {
            const ReaderArtifactRule *rule = reader_artifact_find_token_rule(artifact, inst->text);
            size_t index = 0;
            if (rule && reader_artifact_token_rule_index(artifact, rule, &index)) {
                inst->target_kind = IDM_READER_PATTERN_TARGET_TOKEN;
                inst->target_index = index;
                continue;
            }
            rule = reader_artifact_find_form_rule(artifact, inst->text);
            if (rule && reader_artifact_form_rule_index(artifact, rule, &index)) {
                inst->target_kind = IDM_READER_PATTERN_TARGET_FORM;
                inst->target_index = index;
                continue;
            }
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact references unknown rule '%s'", inst->text ? inst->text : "<bad>");
        }
        if (inst->op == IDM_READER_PATTERN_TOKEN) {
            const ReaderArtifactRule *rule = reader_artifact_find_token_rule(artifact, inst->text);
            size_t index = 0;
            if (!rule || !reader_artifact_token_rule_index(artifact, rule, &index)) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact token pattern references unknown token '%s'", inst->text ? inst->text : "<bad>");
            inst->target_kind = IDM_READER_PATTERN_TARGET_TOKEN;
            inst->target_index = index;
        }
    }
    return true;
}

static bool reader_artifact_resolve_rules(IdmReaderArtifact *artifact, IdmError *err) {
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_resolve_pattern_program(artifact, &artifact->forms[i].pattern, err)) return false;
    }
    return true;
}

static bool reader_artifact_node_nullable(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pc, bool *visiting);

static bool reader_artifact_rule_nullable(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, bool *visiting) {
    IDM_ASSERT_PROVED(rule->kind == (uint8_t)IDM_GRAMMAR_RULE_FORM && rule->reductions && rule->reduction_count != 0);
    size_t index = 0;
    bool have_index = reader_artifact_form_rule_index(artifact, rule, &index);
    IDM_ASSERT_PROVED(have_index);
    (void)have_index;
    if (visiting[index]) return true;
    visiting[index] = true;
    bool nullable = reader_artifact_node_nullable(artifact, rule, 0, visiting);
    visiting[index] = false;
    return nullable;
}

static bool reader_artifact_node_nullable(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pc, bool *visiting) {
    IDM_ASSERT_PROVED(pc < rule->reduction_count);
    const ReaderReductionInst *inst = &rule->reductions[pc];
    switch ((IdmReaderPatternOp)inst->op) {
        case IDM_READER_PATTERN_EMPTY:
            return true;
        case IDM_READER_PATTERN_TOKEN:
        case IDM_READER_PATTERN_LITERAL:
            return false;
        case IDM_READER_PATTERN_REF:
            if (inst->target_kind == IDM_READER_PATTERN_TARGET_TOKEN) return false;
            IDM_ASSERT_PROVED(inst->target_kind == IDM_READER_PATTERN_TARGET_FORM && inst->target_index < artifact->form_count);
            return reader_artifact_rule_nullable(artifact, &artifact->forms[inst->target_index], visiting);
        case IDM_READER_PATTERN_SEQ: {
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                if (!reader_artifact_node_nullable(artifact, rule, child, visiting)) return false;
                child = rule->reductions[child].next;
            }
            return true;
        }
        case IDM_READER_PATTERN_ALT: {
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                if (reader_artifact_node_nullable(artifact, rule, child, visiting)) return true;
                child = rule->reductions[child].next;
            }
            return false;
        }
        case IDM_READER_PATTERN_REPEAT:
        case IDM_READER_PATTERN_OPTIONAL:
        case IDM_READER_PATTERN_PEEK:
        case IDM_READER_PATTERN_NOT:
        case IDM_READER_PATTERN_PARAM:
            return true;
        case IDM_READER_PATTERN_REPEAT1:
        case IDM_READER_PATTERN_CAPTURE:
        case IDM_READER_PATTERN_INDENT_GT:
        case IDM_READER_PATTERN_INDENT_EQ:
        case IDM_READER_PATTERN_ADJACENT:
        case IDM_READER_PATTERN_NOT_ADJACENT:
            return reader_artifact_node_nullable(artifact, rule, inst->first_child, visiting);
    }
    IDM_ASSERT_PROVED(!"reader pattern opcode is proved at the artifact boundary");
    return false;
}

static void reader_artifact_reduction_start_forms_at(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pc, bool *starts, bool *nullable_visiting) {
    IDM_ASSERT_PROVED(pc < rule->reduction_count);
    const ReaderReductionInst *inst = &rule->reductions[pc];
    switch ((IdmReaderPatternOp)inst->op) {
        case IDM_READER_PATTERN_EMPTY:
        case IDM_READER_PATTERN_TOKEN:
        case IDM_READER_PATTERN_LITERAL:
            return;
        case IDM_READER_PATTERN_REF:
            if (inst->target_kind == IDM_READER_PATTERN_TARGET_FORM) {
                IDM_ASSERT_PROVED(inst->target_index < artifact->form_count);
                starts[inst->target_index] = true;
            }
            return;
        case IDM_READER_PATTERN_SEQ: {
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                reader_artifact_reduction_start_forms_at(artifact, rule, child, starts, nullable_visiting);
                memset(nullable_visiting, 0, artifact->form_count * sizeof(*nullable_visiting));
                if (!reader_artifact_node_nullable(artifact, rule, child, nullable_visiting)) break;
                child = rule->reductions[child].next;
            }
            return;
        }
        case IDM_READER_PATTERN_ALT: {
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                reader_artifact_reduction_start_forms_at(artifact, rule, child, starts, nullable_visiting);
                child = rule->reductions[child].next;
            }
            return;
        }
        case IDM_READER_PATTERN_REPEAT:
        case IDM_READER_PATTERN_REPEAT1:
        case IDM_READER_PATTERN_OPTIONAL:
        case IDM_READER_PATTERN_CAPTURE:
        case IDM_READER_PATTERN_INDENT_GT:
        case IDM_READER_PATTERN_INDENT_EQ:
        case IDM_READER_PATTERN_ADJACENT:
        case IDM_READER_PATTERN_NOT_ADJACENT:
        case IDM_READER_PATTERN_PEEK:
        case IDM_READER_PATTERN_NOT:
            reader_artifact_reduction_start_forms_at(artifact, rule, inst->first_child, starts, nullable_visiting);
            return;
        case IDM_READER_PATTERN_PARAM:
            return;
    }
    IDM_ASSERT_PROVED(!"reader pattern opcode is proved at the artifact boundary");
}

static bool reader_artifact_rule_graph_has_cycle(const IdmReaderArtifact *artifact, bool *edges, size_t index, bool *visiting, bool *visited, IdmError *err) {
    if (visiting[index]) return idm_error_set(err, idm_span_unknown(NULL), "left-recursive reader artifact rule '%s'", artifact->forms[index].name);
    if (visited[index]) return true;
    visiting[index] = true;
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (edges[index * artifact->form_count + i] && !reader_artifact_rule_graph_has_cycle(artifact, edges, i, visiting, visited, err)) return false;
    }
    visiting[index] = false;
    visited[index] = true;
    return true;
}

static bool reader_artifact_reject_nullable_repeats_at(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pc, bool *nullable_visiting, IdmError *err) {
    IDM_ASSERT_PROVED(pc < rule->reduction_count);
    const ReaderReductionInst *inst = &rule->reductions[pc];
    if (inst->op == IDM_READER_PATTERN_REPEAT || inst->op == IDM_READER_PATTERN_REPEAT1) {
        memset(nullable_visiting, 0, artifact->form_count * sizeof(*nullable_visiting));
        if (reader_artifact_node_nullable(artifact, rule, inst->first_child, nullable_visiting)) {
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact repeat in rule '%s' can match empty input", rule->name ? rule->name : "<bad>");
        }
    }
    size_t child = inst->first_child;
    for (size_t i = 0; i < inst->child_count; i++) {
        if (!reader_artifact_reject_nullable_repeats_at(artifact, rule, child, nullable_visiting, err)) return false;
        child = rule->reductions[child].next;
    }
    return true;
}

static bool reader_artifact_validate_reductions(IdmReaderArtifact *artifact, IdmError *err) {
    if (artifact->form_count != 0 && artifact->form_count > SIZE_MAX / artifact->form_count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact form graph is too large");
    bool *edges = calloc(artifact->form_count * artifact->form_count, sizeof(*edges));
    bool *visiting = calloc(artifact->form_count, sizeof(*visiting));
    bool *visited = calloc(artifact->form_count, sizeof(*visited));
    bool *nullable_visiting = calloc(artifact->form_count, sizeof(*nullable_visiting));
    if (!edges || !visiting || !visited || !nullable_visiting) {
        free(edges);
        free(visiting);
        free(visited);
        free(nullable_visiting);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < artifact->form_count; i++) {
        reader_artifact_reduction_start_forms_at(artifact, &artifact->forms[i], 0, edges + i * artifact->form_count, nullable_visiting);
    }
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_rule_graph_has_cycle(artifact, edges, i, visiting, visited, err)) {
            free(edges);
            free(visiting);
            free(visited);
            free(nullable_visiting);
            return false;
        }
    }
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_reject_nullable_repeats_at(artifact, &artifact->forms[i], 0, nullable_visiting, err)) {
            free(edges);
            free(visiting);
            free(visited);
            free(nullable_visiting);
            return false;
        }
    }
    free(edges);
    free(visiting);
    free(visited);
    free(nullable_visiting);
    return true;
}

static bool reader_artifact_compile_reduction_at(const IdmReaderProgram *program, size_t pc, ReaderReductionInst *out, size_t *out_next, IdmError *err) {
    if (!program || pc >= program->count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact pattern ended early");
    const IdmReaderInst *src = &program->items[pc];
    ReaderReductionInst *dst = &out[pc];
    dst->op = src->op;
    dst->target_kind = src->target_kind;
    dst->has_literal = src->has_literal;
    dst->literal_index = SIZE_MAX;
    dst->first_child = src->child_count == 0 ? SIZE_MAX : pc + 1u;
    dst->child_count = src->child_count;
    dst->target_index = src->target_index;
    dst->capture_slot = (src->op == IDM_READER_PATTERN_CAPTURE || src->op == IDM_READER_PATTERN_INDENT_GT || src->op == IDM_READER_PATTERN_INDENT_EQ) ? src->capture_slot : SIZE_MAX;
    if (src->op == IDM_READER_PATTERN_CAPTURE || src->op == IDM_READER_PATTERN_INDENT_GT || src->op == IDM_READER_PATTERN_INDENT_EQ) {
        dst->name = idm_strdup(src->text);
        if (!dst->name) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (src->has_literal) {
        dst->literal = idm_syn_clone(src->literal);
        if (!dst->literal) return idm_error_oom(err, src->literal ? src->literal->span : idm_span_unknown(NULL));
    }
    size_t next = pc + 1u;
    for (size_t i = 0; i < src->child_count; i++) {
        if (!reader_artifact_compile_reduction_at(program, next, out, &next, err)) return false;
    }
    dst->next = next;
    *out_next = next;
    return true;
}

static bool reader_artifact_compile_rule_reductions(ReaderArtifactRule *rule, IdmError *err) {
    reader_reductions_destroy(rule);
    if (rule->kind != (uint8_t)IDM_GRAMMAR_RULE_FORM) return true;
    if (rule->pattern.count == 0 || !rule->pattern.items) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact form rule has no pattern program");
    if (rule->pattern.count > UINT32_MAX) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact pattern program is too large");
    rule->reductions = calloc(rule->pattern.count, sizeof(*rule->reductions));
    if (!rule->reductions) return idm_error_oom(err, idm_span_unknown(NULL));
    rule->reduction_count = rule->pattern.count;
    size_t next = 0;
    if (!reader_artifact_compile_reduction_at(&rule->pattern, 0, rule->reductions, &next, err)) return false;
    if (next != rule->reduction_count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact pattern has trailing instructions");
    if (!reader_artifact_validate_reduction_table(rule, err)) return false;
    idm_reader_program_destroy(&rule->pattern);
    return true;
}

static bool reader_artifact_compile_reduction_tables(IdmReaderArtifact *artifact, IdmError *err) {
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_compile_rule_reductions(&artifact->forms[i], err)) return false;
    }
    return true;
}

static bool reader_artifact_index_rule_reduction_literals(IdmReaderArtifact *artifact, ReaderArtifactRule *rule, IdmError *err) {
    for (size_t i = 0; i < rule->reduction_count; i++) {
        ReaderReductionInst *inst = &rule->reductions[i];
        if (inst->op != IDM_READER_PATTERN_LITERAL) continue;
        if (!inst->has_literal) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact literal reduction has no literal");
        if (!reader_artifact_literal_intern(artifact, inst->literal, &inst->literal_index, err)) return false;
        idm_syn_free(inst->literal);
        inst->literal = NULL;
    }
    return true;
}

static bool reader_artifact_index_ctor_literals(IdmReaderArtifact *artifact, IdmReaderProgram *program, IdmError *err) {
    for (size_t i = 0; i < program->count; i++) {
        IdmReaderInst *inst = &program->items[i];
        if (inst->op != IDM_READER_CTOR_LITERAL) continue;
        if (!inst->has_literal) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact literal constructor has no literal");
        if (!reader_artifact_literal_intern(artifact, inst->literal, &inst->literal_index, err)) return false;
        idm_syn_free(inst->literal);
        inst->literal = NULL;
    }
    return true;
}

static bool reader_artifact_index_literals(IdmReaderArtifact *artifact, IdmError *err) {
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_index_rule_reduction_literals(artifact, &artifact->forms[i], err)) return false;
    }
    for (size_t i = 0; i < artifact->token_count; i++) {
        if (!reader_artifact_index_ctor_literals(artifact, &artifact->tokens[i].constructor, err)) return false;
    }
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_index_ctor_literals(artifact, &artifact->forms[i].constructor, err)) return false;
    }
    return true;
}

enum { READER_START_FACT_UNSTARTED, READER_START_FACT_COMPUTING, READER_START_FACT_DONE };

static void reader_artifact_first_add(uint64_t *set, size_t bit) {
    set[bit >> 6] |= 1ull << (bit & 63u);
}

static bool reader_artifact_first_has(const uint64_t *set, size_t bit) {
    return (set[bit >> 6] & (1ull << (bit & 63u))) != 0;
}

static void reader_artifact_first_union(uint64_t *dst, const uint64_t *src, size_t words) {
    for (size_t i = 0; i < words; i++) dst[i] |= src[i];
}

static void reader_artifact_node_start_facts(IdmReaderArtifact *artifact, ReaderArtifactRule *rule, size_t pc, uint8_t **states) {
    size_t rule_index = 0;
    bool in_forms = reader_artifact_form_rule_index(artifact, rule, &rule_index);
    IDM_ASSERT_PROVED(in_forms && pc < rule->reduction_count);
    (void)in_forms;
    uint8_t *state = &states[rule_index][pc];
    if (*state == READER_START_FACT_DONE) return;
    IDM_ASSERT_PROVED(*state != READER_START_FACT_COMPUTING);
    *state = READER_START_FACT_COMPUTING;
    ReaderReductionInst *inst = &rule->reductions[pc];
    uint64_t *first = rule->first + pc * artifact->first_words;
    switch ((IdmReaderPatternOp)inst->op) {
        case IDM_READER_PATTERN_EMPTY:
            inst->nullable = true;
            break;
        case IDM_READER_PATTERN_TOKEN:
            reader_artifact_first_add(first, inst->target_index);
            break;
        case IDM_READER_PATTERN_LITERAL:
            reader_artifact_first_add(first, artifact->token_count + inst->literal_index);
            break;
        case IDM_READER_PATTERN_REF:
            if (inst->target_kind == IDM_READER_PATTERN_TARGET_TOKEN) {
                reader_artifact_first_add(first, inst->target_index);
                break;
            }
            {
                ReaderArtifactRule *target = &artifact->forms[inst->target_index];
                reader_artifact_node_start_facts(artifact, target, 0, states);
                inst->nullable = target->reductions[0].nullable;
                reader_artifact_first_union(first, target->first, artifact->first_words);
            }
            break;
        case IDM_READER_PATTERN_SEQ: {
            inst->nullable = true;
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                reader_artifact_node_start_facts(artifact, rule, child, states);
                reader_artifact_first_union(first, rule->first + child * artifact->first_words, artifact->first_words);
                if (!rule->reductions[child].nullable) {
                    inst->nullable = false;
                    break;
                }
                child = rule->reductions[child].next;
            }
            break;
        }
        case IDM_READER_PATTERN_ALT: {
            size_t child = inst->first_child;
            for (size_t i = 0; i < inst->child_count; i++) {
                reader_artifact_node_start_facts(artifact, rule, child, states);
                reader_artifact_first_union(first, rule->first + child * artifact->first_words, artifact->first_words);
                if (rule->reductions[child].nullable) inst->nullable = true;
                child = rule->reductions[child].next;
            }
            break;
        }
        case IDM_READER_PATTERN_REPEAT:
        case IDM_READER_PATTERN_OPTIONAL:
            inst->nullable = true;
            reader_artifact_node_start_facts(artifact, rule, inst->first_child, states);
            reader_artifact_first_union(first, rule->first + inst->first_child * artifact->first_words, artifact->first_words);
            break;
        case IDM_READER_PATTERN_PARAM:
            inst->nullable = true;
            break;
        case IDM_READER_PATTERN_PEEK:
        case IDM_READER_PATTERN_NOT:
            inst->nullable = true;
            reader_artifact_node_start_facts(artifact, rule, inst->first_child, states);
            reader_artifact_first_union(first, rule->first + inst->first_child * artifact->first_words, artifact->first_words);
            break;
        case IDM_READER_PATTERN_REPEAT1:
        case IDM_READER_PATTERN_CAPTURE:
        case IDM_READER_PATTERN_INDENT_GT:
        case IDM_READER_PATTERN_INDENT_EQ:
        case IDM_READER_PATTERN_ADJACENT:
        case IDM_READER_PATTERN_NOT_ADJACENT:
            reader_artifact_node_start_facts(artifact, rule, inst->first_child, states);
            inst->nullable = rule->reductions[inst->first_child].nullable;
            reader_artifact_first_union(first, rule->first + inst->first_child * artifact->first_words, artifact->first_words);
            break;
    }
    *state = READER_START_FACT_DONE;
}

static bool reader_artifact_compile_start_facts(IdmReaderArtifact *artifact, IdmError *err) {
    artifact->first_words = (artifact->token_count + artifact->literal_count + 63u) / 64u;
    uint8_t **states = calloc(artifact->form_count, sizeof(*states));
    if (!states) return idm_error_oom(err, idm_span_unknown(NULL));
    bool ok = true;
    for (size_t i = 0; ok && i < artifact->form_count; i++) {
        ReaderArtifactRule *rule = &artifact->forms[i];
        rule->first = calloc(rule->reduction_count, artifact->first_words * sizeof(*rule->first));
        states[i] = calloc(rule->reduction_count, sizeof(**states));
        if (!rule->first || !states[i]) ok = idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; ok && i < artifact->form_count; i++) {
        ReaderArtifactRule *rule = &artifact->forms[i];
        for (size_t pc = 0; pc < rule->reduction_count; pc++) {
            if (rule->reductions[pc].op != (uint8_t)IDM_READER_PATTERN_ALT) continue;
            size_t child = rule->reductions[pc].first_child;
            for (size_t c = 0; c < rule->reductions[pc].child_count; c++) {
                reader_artifact_node_start_facts(artifact, rule, child, states);
                child = rule->reductions[child].next;
            }
        }
    }
    for (size_t i = 0; i < artifact->form_count; i++) free(states[i]);
    free(states);
    return ok;
}

static size_t reader_artifact_capture_base(const size_t *base_counts, size_t base_count, size_t slot) {
    return base_counts && slot < base_count ? base_counts[slot] : 0;
}

static IdmSyntax *reader_artifact_construct_at(const IdmReaderArtifact *artifact, const IdmReaderProgram *program, size_t *pc, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err);

static bool reader_artifact_append_constructed(const IdmReaderArtifact *artifact, IdmSyntax *seq, const IdmReaderProgram *program, size_t *pc, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    if (*pc >= program->count) return idm_error_set(err, span, "reader artifact constructor ended early");
    const IdmReaderInst *inst = &program->items[*pc];
    if (inst->op == IDM_READER_CTOR_SPLICE) {
        (*pc)++;
        ReaderArtifactCapture *cap = reader_artifact_capture_at(captures, inst->capture_slot);
        if (!cap) return idm_error_set(err, span, "unknown artifact capture '%s'", inst->text ? inst->text : "<bad>");
        size_t start = reader_artifact_capture_base(base_counts, base_count, inst->capture_slot);
        for (size_t i = start; i < cap->count; i++) {
            IdmSyntax *clone = idm_syn_clone(cap->items[i].syntax);
            if (!clone) return idm_error_oom(err, span);
            if (!reader_artifact_sequence_append(seq, clone, err, span)) return false;
        }
        return true;
    }
    IdmSyntax *item = reader_artifact_construct_at(artifact, program, pc, captures, base_counts, base_count, span, err);
    return item && reader_artifact_sequence_append(seq, item, err, span);
}

static bool reader_artifact_construct_children(const IdmReaderArtifact *artifact, IdmSyntax *seq, const IdmReaderProgram *program, size_t *pc, size_t count, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    for (size_t i = 0; i < count; i++) {
        if (!reader_artifact_append_constructed(artifact, seq, program, pc, captures, base_counts, base_count, span, err)) return false;
    }
    return true;
}

static IdmSyntax *reader_artifact_capture_value(const IdmReaderInst *inst, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    ReaderArtifactCapture *cap = reader_artifact_capture_at(captures, inst->capture_slot);
    size_t start = reader_artifact_capture_base(base_counts, base_count, inst->capture_slot);
    if (!cap || cap->count == start) {
        idm_error_set(err, span, "unknown artifact capture '%s'", inst->text ? inst->text : "<bad>");
        return NULL;
    }
    if (inst->op == IDM_READER_CTOR_CAPTURE && cap->count == start + 1u) {
        IdmSyntax *clone = idm_syn_clone(cap->items[start].syntax);
        if (!clone) idm_error_oom(err, span);
        return clone;
    }
    IdmSyntax *list = idm_syn_list(span);
    if (!list) {
        idm_error_oom(err, span);
        return NULL;
    }
    for (size_t i = start; i < cap->count; i++) {
        IdmSyntax *clone = idm_syn_clone(cap->items[i].syntax);
        if (!clone) {
            idm_error_oom(err, span);
            idm_syn_free(list);
            return NULL;
        }
        if (!reader_artifact_sequence_append(list, clone, err, span)) {
            idm_syn_free(list);
            return NULL;
        }
    }
    return list;
}

static const ReaderArtifactCaptureItem *reader_artifact_capture_single_text(const IdmReaderInst *inst, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    ReaderArtifactCapture *cap = reader_artifact_capture_at(captures, inst->capture_slot);
    size_t start = reader_artifact_capture_base(base_counts, base_count, inst->capture_slot);
    if (!cap || cap->count == start) {
        idm_error_set(err, span, "unknown artifact capture '%s'", inst->text ? inst->text : "<bad>");
        return NULL;
    }
    if (cap->count != start + 1u) {
        idm_error_set(err, span, "artifact capture '%s' has multiple values", inst->text ? inst->text : "<bad>");
        return NULL;
    }
    if (!cap->items[start].text) {
        idm_error_set(err, span, "artifact capture '%s' has no terminal text", inst->text ? inst->text : "<bad>");
        return NULL;
    }
    return &cap->items[start];
}

static IdmSyntax *reader_artifact_construct_capture_text(const IdmReaderInst *inst, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    const ReaderArtifactCaptureItem *item = reader_artifact_capture_single_text(inst, captures, base_counts, base_count, span, err);
    if (!item) return NULL;
    IdmSyntax *syn = (IdmSyntaxKind)inst->integer == IDM_SYN_ATOM ? idm_syn_atom(item->text, span)
                   : (IdmSyntaxKind)inst->integer == IDM_SYN_WORD ? idm_syn_word(item->text, span)
                   : idm_syn_string(item->text, span);
    if (!syn) idm_error_oom(err, span);
    return syn;
}

static bool reader_artifact_string_chunk_add(IdmSyntax *result, IdmBuffer *chunk, IdmSpan span, IdmError *err) {
    if (chunk->len == 0) return true;
    IdmSyntax *lit = idm_syn_string_n(chunk->data ? chunk->data : "", chunk->len, span);
    if (!lit) return idm_error_oom(err, span);
    if (!idm_syn_append(result, lit)) {
        idm_syn_free(lit);
        return idm_error_oom(err, span);
    }
    idm_buf_destroy(chunk);
    idm_buf_init(chunk);
    return true;
}

bool idm_reader_string_escape(char e, char *out) {
    switch (e) {
        case 'n': *out = '\n'; return true;
        case 'r': *out = '\r'; return true;
        case 't': *out = '\t'; return true;
        case '\\': *out = '\\'; return true;
        case '"': *out = '"'; return true;
        case '$': *out = '$'; return true;
        case '#': *out = '#'; return true;
        default: return false;
    }
}

static void syn_span_rebase(IdmSyntax *syn, const char *file, size_t base_off, unsigned base_line, unsigned base_col) {
    if (!syn) return;
    if (syn->span.line != 0) {
        syn->span.file = file;
        syn->span.start += base_off;
        syn->span.end += base_off;
        if (syn->span.line == 1u) syn->span.column += base_col - 1u;
        syn->span.line += base_line - 1u;
    }
    switch (syn->kind) {
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            for (size_t i = 0; i < syn->as.seq.count; i++) syn_span_rebase(syn->as.seq.items[i], file, base_off, base_line, base_col);
            break;
        default:
            break;
    }
}

static IdmSyntax *reader_artifact_read_interp_inner(const IdmReaderArtifact *artifact, const char *inner, size_t len, IdmSpan span, IdmError *err) {
    char *copy = idm_strndup(inner, len);
    if (!copy) {
        idm_error_oom(err, span);
        return NULL;
    }
    IdmSyntax *pkg = NULL;
    bool ok = idm_reader_read_artifact_string(artifact, span.file ? span.file : "<string-interpolation>", copy, 0, &pkg, NULL, err);
    free(copy);
    if (!ok) return NULL;
    if (!pkg || pkg->kind != IDM_SYN_LIST || pkg->as.seq.count != 2u) {
        idm_syn_free(pkg);
        idm_error_set(err, span, "string interpolation must contain a single expression");
        return NULL;
    }
    IdmSyntax *form = idm_syn_clone(pkg->as.seq.items[1]);
    idm_syn_free(pkg);
    if (!form) idm_error_oom(err, span);
    return form;
}

static IdmSyntax *reader_artifact_construct_interp_string(const IdmReaderArtifact *artifact, const IdmReaderInst *inst, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    const ReaderArtifactCaptureItem *item = reader_artifact_capture_single_text(inst, captures, base_counts, base_count, span, err);
    if (!item) return NULL;
    const char *body = item->text ? item->text : "";
    size_t len = strlen(body);
    size_t start = len >= 2u && body[0] == '"' ? 1u : 0u;
    size_t end = len >= 2u && body[0] == '"' && body[len - 1u] == '"' ? len - 1u : len;
    IdmBuffer chunk;
    idm_buf_init(&chunk);
    IdmSyntax *result = NULL;
    bool interpolation = false;
    size_t i = start;
    while (i < end) {
        char ch = body[i];
        if ((ch == '$' || ch == '#') && i + 1u < end && body[i + 1u] == '{') {
            if (!result) {
                result = idm_syn_list(span);
                IdmSyntax *head = idm_syn_word("%-string", span);
                if (!result || !head || !idm_syn_append(result, head)) {
                    idm_syn_free(result);
                    idm_syn_free(head);
                    idm_buf_destroy(&chunk);
                    idm_error_oom(err, span);
                    return NULL;
                }
            }
            if (!reader_artifact_string_chunk_add(result, &chunk, span, err)) {
                idm_syn_free(result);
                idm_buf_destroy(&chunk);
                return NULL;
            }
            size_t close = scan_interp_end(body, i + 2u, end);
            if (close >= end) {
                idm_syn_free(result);
                idm_buf_destroy(&chunk);
                idm_error_set(err, span, "unterminated string interpolation");
                return NULL;
            }
            IdmSyntax *part = reader_artifact_read_interp_inner(artifact, body + i + 2u, close - (i + 2u), span, err);
            if (part && start == 1u && span.line != 0) {
                unsigned frag_line = span.line;
                unsigned frag_col = span.column;
                reader_artifact_advance_position(body, 0, i + 2u, &frag_line, &frag_col);
                syn_span_rebase(part, span.file, span.start + i + 2u, frag_line, frag_col);
            }
            if (!part || !idm_syn_append(result, part)) {
                idm_syn_free(part);
                idm_syn_free(result);
                idm_buf_destroy(&chunk);
                if (err && err->present) return NULL;
                idm_error_oom(err, span);
                return NULL;
            }
            interpolation = true;
            i = close + 1u;
            continue;
        }
        i++;
        if (ch == '\\') {
            if (i >= end) {
                idm_buf_destroy(&chunk);
                idm_syn_free(result);
                idm_error_set(err, span, "unterminated string escape");
                return NULL;
            }
            char decoded = 0;
            if (!idm_reader_string_escape(body[i++], &decoded)) {
                idm_buf_destroy(&chunk);
                idm_syn_free(result);
                idm_error_set(err, span, "unknown string escape");
                return NULL;
            }
            ch = decoded;
        }
        if (!idm_buf_append_char(&chunk, ch)) {
            idm_buf_destroy(&chunk);
            idm_syn_free(result);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    if (!interpolation) {
        IdmSyntax *plain = idm_syn_string_n(chunk.data ? chunk.data : "", chunk.len, span);
        idm_buf_destroy(&chunk);
        if (!plain) idm_error_oom(err, span);
        return plain;
    }
    if (!reader_artifact_string_chunk_add(result, &chunk, span, err)) {
        idm_syn_free(result);
        idm_buf_destroy(&chunk);
        return NULL;
    }
    idm_buf_destroy(&chunk);
    return result;
}

static bool bstr_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static size_t bstr_seg_end(const char *body, size_t i, size_t end) {
    long depth = 0;
    while (i < end) {
        char c = body[i];
        if (c == '"') { size_t cl = scan_string_end(body, i, end, NULL); if (cl >= end) return end; i = cl; continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; i++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; i++; continue; }
        if (depth == 0 && bstr_is_ws(c)) return i;
        i++;
    }
    return end;
}

static size_t bstr_find_top(const char *body, size_t i, size_t end, char target) {
    long depth = 0;
    while (i < end) {
        char c = body[i];
        if (c == '"') { size_t cl = scan_string_end(body, i, end, NULL); if (cl >= end) return end; i = cl; continue; }
        if (c == '(' || c == '[' || c == '{') { depth++; i++; continue; }
        if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; i++; continue; }
        if (depth == 0 && c == target) return i;
        i++;
    }
    return end;
}

static IdmSyntax *bstr_reread_term(const IdmReaderArtifact *artifact, const char *text, size_t len, IdmSpan span, IdmError *err) {
    while (len > 0 && bstr_is_ws(text[0])) { text++; len--; }
    while (len > 0 && bstr_is_ws(text[len - 1u])) len--;
    if (len == 0) { idm_error_set(err, span, "bitstring segment is empty"); return NULL; }
    char *copy = idm_strndup(text, len);
    if (!copy) { idm_error_oom(err, span); return NULL; }
    IdmSyntax *pkg = NULL;
    bool ok = idm_reader_read_artifact_string(artifact, span.file ? span.file : "<bitstring>", copy, 0, &pkg, NULL, err);
    free(copy);
    if (!ok) return NULL;
    if (!pkg || pkg->kind != IDM_SYN_LIST || pkg->as.seq.count != 2u) {
        idm_syn_free(pkg);
        idm_error_set(err, span, "bitstring segment must be a single expression");
        return NULL;
    }
    IdmSyntax *expr = pkg->as.seq.items[1];
    IdmSyntax *term = NULL;
    if (expr && expr->kind == IDM_SYN_LIST && expr->as.seq.count == 2u &&
        expr->as.seq.items[0]->kind == IDM_SYN_WORD && strcmp(expr->as.seq.items[0]->as.text, "%-expr") == 0) {
        term = idm_syn_clone(expr->as.seq.items[1]);
    } else {
        term = idm_syn_clone(expr);
    }
    idm_syn_free(pkg);
    if (!term) idm_error_oom(err, span);
    return term;
}

static bool bstr_add_opts(IdmSyntax *seg, const char *body, size_t s, size_t e, IdmSpan span, IdmError *err) {
    size_t i = s;
    while (i < e) {
        while (i < e && (body[i] == '/' || body[i] == ',' || bstr_is_ws(body[i]))) i++;
        size_t start = i;
        while (i < e && body[i] != '/' && body[i] != ',' && !bstr_is_ws(body[i])) i++;
        if (i == start) break;
        size_t olen = i - start;
        if (!(olen == 3u && strncmp(body + start, "big", 3) == 0) &&
            !(olen == 6u && strncmp(body + start, "little", 6) == 0) &&
            !(olen == 6u && strncmp(body + start, "signed", 6) == 0) &&
            !(olen == 8u && strncmp(body + start, "unsigned", 8) == 0) &&
            !(olen == 5u && strncmp(body + start, "float", 5) == 0) &&
            !(olen == 4u && strncmp(body + start, "bits", 4) == 0)) {
            char *bad = idm_strndup(body + start, olen);
            idm_error_set(err, span, "unknown bitstring option '%s'", bad ? bad : "?");
            free(bad);
            return false;
        }
        char *atom_text = idm_strndup(body + start, olen);
        if (!atom_text) return idm_error_oom(err, span);
        IdmSyntax *atom = idm_syn_atom(atom_text, span);
        free(atom_text);
        if (!atom || !idm_syn_append(seg, atom)) { idm_syn_free(atom); return idm_error_oom(err, span); }
    }
    return true;
}

static IdmSyntax *bstr_make_normal(const IdmReaderArtifact *artifact, const char *body, size_t s, size_t e, IdmSpan span, IdmError *err) {
    size_t opts_pos = bstr_find_top(body, s, e, '/');
    size_t head_end = opts_pos < e ? opts_pos : e;
    size_t colon_pos = bstr_find_top(body, s, head_end, ':');
    bool has_size = colon_pos < head_end;
    size_t value_end = has_size ? colon_pos : head_end;
    IdmSyntax *seg = idm_syn_list(span);
    IdmSyntax *head = idm_syn_word(has_size ? "%-bitseg" : "%-bitseg-bare", span);
    if (!seg || !head || !idm_syn_append(seg, head)) { idm_syn_free(seg); idm_syn_free(head); idm_error_oom(err, span); return NULL; }
    IdmSyntax *value = bstr_reread_term(artifact, body + s, value_end - s, span, err);
    if (!value || !idm_syn_append(seg, value)) {
        idm_syn_free(value);
        idm_syn_free(seg);
        if (err && !err->present) idm_error_oom(err, span);
        return NULL;
    }
    if (has_size) {
        IdmSyntax *size = bstr_reread_term(artifact, body + colon_pos + 1u, head_end - (colon_pos + 1u), span, err);
        if (!size || !idm_syn_append(seg, size)) {
            idm_syn_free(size);
            idm_syn_free(seg);
            if (err && !err->present) idm_error_oom(err, span);
            return NULL;
        }
    }
    if (opts_pos < e && !bstr_add_opts(seg, body, opts_pos + 1u, e, span, err)) { idm_syn_free(seg); return NULL; }
    return seg;
}

static IdmSyntax *reader_artifact_construct_bitstring(const IdmReaderArtifact *artifact, const IdmReaderInst *inst, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    const ReaderArtifactCaptureItem *item = reader_artifact_capture_single_text(inst, captures, base_counts, base_count, span, err);
    if (!item) return NULL;
    const char *raw = item->text ? item->text : "";
    size_t rawlen = strlen(raw);
    if (rawlen < 3u || raw[0] != '%' || raw[1] != '<' || raw[rawlen - 1u] != '>') {
        idm_error_set(err, span, "unterminated bitstring literal, expected closing '>'");
        return NULL;
    }
    size_t start = 2u, end = rawlen - 1u;
    struct { size_t s, e; } segs[256];
    size_t nseg = 0;
    size_t i = start;
    while (i < end) {
        while (i < end && bstr_is_ws(raw[i])) i++;
        if (i >= end) break;
        size_t se = bstr_seg_end(raw, i, end);
        if (nseg >= 256u) { idm_error_set(err, span, "bitstring literal has too many segments"); return NULL; }
        segs[nseg].s = i;
        segs[nseg].e = se;
        nseg++;
        i = se;
    }
    size_t dot_idx = SIZE_MAX;
    for (size_t k = 0; k < nseg; k++) {
        if (segs[k].e - segs[k].s == 1u && raw[segs[k].s] == '.') {
            if (dot_idx != SIZE_MAX) { idm_error_set(err, span, "bitstring rest pattern may contain only one dot"); return NULL; }
            dot_idx = k;
        }
    }
    size_t normal_count = nseg;
    if (dot_idx != SIZE_MAX) {
        if (dot_idx + 2u != nseg) { idm_error_set(err, span, "bitstring rest pattern must have form %%<seg ... . rest>"); return NULL; }
        normal_count = dot_idx;
    }
    IdmSyntax *result = idm_syn_list(span);
    IdmSyntax *head = idm_syn_word("%-bitstring", span);
    if (!result || !head || !idm_syn_append(result, head)) { idm_syn_free(result); idm_syn_free(head); idm_error_oom(err, span); return NULL; }
    for (size_t k = 0; k < normal_count; k++) {
        IdmSyntax *seg = bstr_make_normal(artifact, raw, segs[k].s, segs[k].e, span, err);
        if (!seg || !idm_syn_append(result, seg)) {
            idm_syn_free(seg);
            idm_syn_free(result);
            if (err && !err->present) idm_error_oom(err, span);
            return NULL;
        }
    }
    if (dot_idx != SIZE_MAX) {
        IdmSyntax *rest = idm_syn_list(span);
        if (!rest) {
            idm_syn_free(result);
            idm_error_oom(err, span);
            return NULL;
        }
        IdmSyntax *rhead = idm_syn_word("%-bitrest", span);
        if (!rhead || !idm_syn_append(rest, rhead)) {
            idm_syn_free(rhead);
            idm_syn_free(rest);
            idm_syn_free(result);
            idm_error_oom(err, span);
            return NULL;
        }
        IdmSyntax *rval = bstr_reread_term(artifact, raw + segs[nseg - 1u].s, segs[nseg - 1u].e - segs[nseg - 1u].s, span, err);
        if (!rval || !idm_syn_append(rest, rval)) {
            idm_syn_free(rval);
            idm_syn_free(rest);
            idm_syn_free(result);
            if (err && !err->present) idm_error_oom(err, span);
            return NULL;
        }
        if (!idm_syn_append(result, rest)) {
            idm_syn_free(rest);
            idm_syn_free(result);
            idm_error_oom(err, span);
            return NULL;
        }
    }
    return result;
}

static IdmSyntax *reader_artifact_construct_at(const IdmReaderArtifact *artifact, const IdmReaderProgram *program, size_t *pc, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    IDM_ASSERT_PROVED(*pc < program->count);
    const IdmReaderInst *inst = &program->items[(*pc)++];
    switch ((IdmReaderCtorOp)inst->op) {
        case IDM_READER_CTOR_CAPTURE:
        case IDM_READER_CTOR_SPLICE:
            return reader_artifact_capture_value(inst, captures, base_counts, base_count, span, err);
        case IDM_READER_CTOR_CAPTURE_TEXT:
            return reader_artifact_construct_capture_text(inst, captures, base_counts, base_count, span, err);
        case IDM_READER_CTOR_INTERP_STRING:
            return reader_artifact_construct_interp_string(artifact, inst, captures, base_counts, base_count, span, err);
        case IDM_READER_CTOR_BITSTRING:
            return reader_artifact_construct_bitstring(artifact, inst, captures, base_counts, base_count, span, err);
        case IDM_READER_CTOR_LITERAL: {
            IDM_ASSERT_PROVED(inst->literal_index < artifact->literal_count);
            IdmSyntax *syn = idm_syn_clone(artifact->literals[inst->literal_index]);
            if (!syn) idm_error_oom(err, span);
            return syn;
        }
        case IDM_READER_CTOR_EMIT_TEXT: {
            IdmSyntax *syn = (IdmSyntaxKind)inst->integer == IDM_SYN_ATOM ? idm_syn_atom(inst->text, span)
                           : (IdmSyntaxKind)inst->integer == IDM_SYN_WORD ? idm_syn_word(inst->text, span)
                           : idm_syn_string(inst->text, span);
            if (!syn) idm_error_oom(err, span);
            return syn;
        }
        case IDM_READER_CTOR_EMIT_INT: {
            IdmSyntax *syn = idm_syn_int(inst->integer, span);
            if (!syn) idm_error_oom(err, span);
            return syn;
        }
        case IDM_READER_CTOR_FORM: {
            IdmSyntax *list = idm_syn_list(span);
            IdmSyntax *head = idm_syn_word(inst->text, span);
            if (!list || !head || !idm_syn_append(list, head)) {
                idm_syn_free(list);
                idm_syn_free(head);
                idm_error_oom(err, span);
                return NULL;
            }
            if (!reader_artifact_construct_children(artifact, list, program, pc, inst->child_count, captures, base_counts, base_count, span, err)) {
                idm_syn_free(list);
                return NULL;
            }
            return list;
        }
        case IDM_READER_CTOR_SEQ: {
            IdmSyntax *seq = idm_syn_seq((IdmSyntaxKind)inst->integer, span);
            if (!seq) {
                idm_error_oom(err, span);
                return NULL;
            }
            if (!reader_artifact_construct_children(artifact, seq, program, pc, inst->child_count, captures, base_counts, base_count, span, err)) {
                idm_syn_free(seq);
                return NULL;
            }
            return seq;
        }
    }
    IDM_ASSERT_PROVED(!"reader constructor opcode is proved at the artifact boundary");
    return NULL;
}

static IdmSyntax *reader_artifact_construct(const IdmReaderArtifact *artifact, const IdmReaderProgram *program, ReaderArtifactCaptures *captures, const size_t *base_counts, size_t base_count, IdmSpan span, IdmError *err) {
    size_t pc = 0;
    IdmSyntax *out = reader_artifact_construct_at(artifact, program, &pc, captures, base_counts, base_count, span, err);
    IDM_ASSERT_PROVED(!out || pc == program->count);
    return out;
}

static bool reader_artifact_construct_inherits_token(const IdmSyntax *syn) {
    if (!syn || syn->kind != IDM_SYN_LIST || syn->as.seq.count == 0) return true;
    const IdmSyntax *head = syn->as.seq.items[0];
    if (!head || head->kind != IDM_SYN_WORD || !head->as.text) return true;
    return strcmp(head->as.text, "%-expr") != 0 &&
           strcmp(head->as.text, "%-body") != 0 &&
           strcmp(head->as.text, "%-package-begin") != 0;
}

static void reader_artifact_clear_token(IdmSyntax *syn) {
    if (!syn) return;
    free(syn->token_raw);
    syn->token_raw = NULL;
    syn->token_leading_space = false;
    syn->token_adjacent_previous = false;
}

static bool reader_artifact_set_token_n(IdmSyntax *syn, const char *raw, size_t len, bool leading_space, bool adjacent_previous, IdmSpan span, IdmError *err) {
    if (!syn) return true;
    char *copy = idm_strndup(raw, len);
    if (!copy) return idm_error_oom(err, span);
    free(syn->token_raw);
    syn->token_raw = copy;
    syn->token_leading_space = leading_space;
    syn->token_adjacent_previous = adjacent_previous;
    return true;
}

static bool reader_artifact_capture_add_range(ReaderArtifactCaptures *captures, size_t slot, const char *source, const IdmByteCapture *range, IdmSpan span, IdmError *err) {
    if (!range || !range->set || range->end < range->start) return true;
    IdmSpan cap_span = span;
    cap_span.start = range->start;
    cap_span.end = range->end;
    IdmSyntax *base = reader_artifact_default_token_syntax(source + range->start, range->end - range->start, cap_span, err);
    if (!base) return false;
    bool ok = reader_artifact_set_token_n(base, source + range->start, range->end - range->start, false, false, cap_span, err) &&
              reader_artifact_capture_add_text(captures, slot, base, source + range->start, range->end - range->start, err, cap_span);
    idm_syn_free(base);
    return ok;
}

static IdmSyntax *reader_artifact_construct_token(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t lex_index, const IdmByteMatch *match, const char *source, size_t pos, size_t len, IdmSpan span, bool leading_space, bool adjacent_previous, IdmError *err) {
    const char *raw = source + pos;
    IdmSyntax *base = reader_artifact_default_token_syntax(raw, len, span, err);
    if (!base) return NULL;
    if (!reader_artifact_set_token_n(base, raw, len, leading_space, adjacent_previous, span, err)) {
        idm_syn_free(base);
        return NULL;
    }
    ReaderArtifactCaptures captures;
    bool ok = reader_artifact_captures_init(&captures, artifact->capture_name_count, err, span) &&
              reader_artifact_capture_add_text(&captures, rule->capture_slot, base, raw, len, err, span);
    for (size_t i = 1u; ok && match && i < match->capture_count; i++) {
        const char *group = idm_regex_set_group_name(artifact->terminal_program, lex_index, i);
        if (!group || group[0] == '\0') continue;
        if (i >= rule->terminal_capture_slot_count) {
            ok = idm_error_set(err, span, "reader artifact terminal capture slot is missing");
            break;
        }
        ok = reader_artifact_capture_add_range(&captures, rule->terminal_capture_slots[i], source, &match->captures[i], span, err);
    }
    IdmSyntax *out = ok ? reader_artifact_construct(artifact, &rule->constructor, &captures, NULL, 0, span, err) : NULL;
    if (out && !reader_artifact_set_token_n(out, raw, len, leading_space, adjacent_previous, span, err)) {
        idm_syn_free(out);
        out = NULL;
    }
    reader_artifact_captures_destroy(&captures);
    idm_syn_free(base);
    return out;
}

static void reader_artifact_find_literal_index(const IdmReaderArtifact *artifact, const IdmSyntax *syntax, size_t *out_index, uint64_t *scan_count) {
    *out_index = SIZE_MAX;
    for (size_t i = 0; i < artifact->literal_count; i++) {
        if (scan_count) (*scan_count)++;
        if (idm_syn_equal(artifact->literals[i], syntax)) {
            *out_index = i;
            return;
        }
    }
}

static bool reader_artifact_precompute_literal_token_indices(IdmReaderArtifact *artifact, IdmError *err) {
    artifact->literal_token_indices_ready = false;
    for (size_t i = 0; i < artifact->token_count; i++) {
        ReaderArtifactRule *rule = &artifact->tokens[i];
        rule->literal_index = SIZE_MAX;
    }
    for (size_t i = 0; i < artifact->token_count; i++) {
        ReaderArtifactRule *rule = &artifact->tokens[i];
        if (rule->terminal.kind != (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL) continue;
        const char *source = rule->terminal.text ? rule->terminal.text : "";
        IdmSpan span = idm_span_unknown(artifact->surface);
        IdmSyntax *syntax = reader_artifact_construct_token(artifact, rule, SIZE_MAX, NULL, source, 0, strlen(source), span, false, true, err);
        if (!syntax) return false;
        reader_artifact_find_literal_index(artifact, syntax, &rule->literal_index, NULL);
        idm_syn_free(syntax);
    }
    artifact->literal_token_indices_ready = true;
    return true;
}

static bool reader_artifact_regex_escape_literal(IdmBuffer *out, const char *literal) {
    for (size_t i = 0; literal && literal[i] != '\0'; i++) {
        char ch = literal[i];
        if (ch == '\\' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
            ch == '.' || ch == '^' || ch == '$' || ch == '|' || ch == '*' || ch == '+' || ch == '?') {
            if (!idm_buf_append_char(out, '\\')) return false;
        }
        if (!idm_buf_append_char(out, ch)) return false;
    }
    return true;
}

static void reader_artifact_advance_position(const char *source, size_t start, size_t end, unsigned *line, unsigned *column) {
    for (size_t i = start; i < end; i++) {
        if (source[i] == '\n') {
            (*line)++;
            *column = 1;
        } else {
            (*column)++;
        }
    }
}

static bool reader_artifact_custom_terminal_end(const ReaderArtifactRule *rule, const char *source, size_t len, size_t pos, size_t *out_end, bool *out_open_at_eof) {
    if (!rule) return false;
    if (rule->terminal.kind == (uint8_t)IDM_GRAMMAR_TERMINAL_STRING) {
        if (pos >= len || source[pos] != '"') return false;
        bool closed = false;
        size_t end = scan_string_end(source, pos, len, &closed);
        if (end <= pos || end > len) return false;
        *out_end = end;
        if (out_open_at_eof) *out_open_at_eof = !closed;
        return true;
    }
    if (rule->terminal.kind == (uint8_t)IDM_GRAMMAR_TERMINAL_BITSTRING) {
        if (pos + 1u >= len || source[pos] != '%' || source[pos + 1u] != '<') return false;
        size_t end = scan_bitstring_end(source, pos, len, out_open_at_eof);
        *out_end = end > len ? len : end;
        return true;
    }
    return false;
}

static bool reader_artifact_custom_best_in_rules(const ReaderArtifactRule *rules, size_t count, const char *source, size_t len, size_t pos, const ReaderArtifactRule **best, size_t *best_end, bool *best_open_at_eof) {
    for (size_t i = 0; i < count; i++) {
        size_t end = 0;
        bool open_at_eof = false;
        if (!reader_artifact_custom_terminal_end(&rules[i], source, len, pos, &end, &open_at_eof)) continue;
        if (!*best || end > *best_end || (end == *best_end && rules[i].order < (*best)->order)) {
            *best = &rules[i];
            *best_end = end;
            *best_open_at_eof = open_at_eof;
        }
    }
    return *best != NULL;
}

static bool reader_artifact_lex(const IdmReaderArtifact *artifact, const char *file, const char *source, size_t len, size_t start_pos, unsigned start_line, unsigned start_column, ReaderArtifactTokenVec *out, bool *incomplete, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.artifact.lex");
    size_t pos = start_pos;
    size_t previous_end = start_pos;
    unsigned line = start_line;
    unsigned column = start_column;
    bool leading_space = false;
    uint64_t regex_calls = 0;
    uint64_t custom_scans = 0;
    uint64_t literal_scans = 0;
    uint64_t skipped = 0;
    while (pos < len) {
        IdmByteMatch match;
        memset(&match, 0, sizeof(match));
        bool have_regex = false;
        regex_calls++;
        if (!idm_regex_set_exec_at(artifact->terminal_program, source, len, pos, &match, err)) {
            idm_profile_scope_end(&prof);
            return false;
        }
        if (match.matched && match.end > pos && match.index < artifact->lex_count) have_regex = true;
        const ReaderArtifactRule *best = have_regex ? artifact->lex_rules[match.index] : NULL;
        size_t best_end = have_regex ? match.end : pos;
        bool best_regex = have_regex;
        const ReaderArtifactRule *custom = NULL;
        size_t custom_end = pos;
        bool custom_open_at_eof = false;
        custom_scans += (uint64_t)artifact->token_count + (uint64_t)artifact->skip_count;
        reader_artifact_custom_best_in_rules(artifact->tokens, artifact->token_count, source, len, pos, &custom, &custom_end, &custom_open_at_eof);
        reader_artifact_custom_best_in_rules(artifact->skips, artifact->skip_count, source, len, pos, &custom, &custom_end, &custom_open_at_eof);
        if (custom && (!best || custom_end > best_end || (custom_end == best_end && custom->order < best->order))) {
            best = custom;
            best_end = custom_end;
            best_regex = false;
            if (custom_open_at_eof) reader_mark_incomplete(incomplete);
        }
        if (!best) {
            idm_byte_match_destroy(&match);
            idm_profile_scope_end(&prof);
            return idm_error_set(err, (IdmSpan){file, pos, pos + 1u, line, column}, "reader artifact has no terminal for '%c'", source[pos]);
        }
        IdmSpan span = {file, pos, best_end, line, column};
        if (best->kind == (uint8_t)IDM_GRAMMAR_RULE_SKIP) {
            leading_space = true;
            skipped++;
            reader_artifact_advance_position(source, pos, best_end, &line, &column);
            pos = best_end;
            idm_byte_match_destroy(&match);
            continue;
        }
        bool adjacent_previous = pos == previous_end;
        IdmSyntax *syntax = reader_artifact_construct_token(artifact, best, best_regex ? match.index : SIZE_MAX, best_regex ? &match : NULL, source, pos, best_end - pos, span, leading_space, adjacent_previous, err);
        if (!syntax) {
            idm_byte_match_destroy(&match);
            idm_profile_scope_end(&prof);
            return false;
        }
        char *lexeme = idm_strndup(source + pos, best_end - pos);
        if (!lexeme) {
            idm_byte_match_destroy(&match);
            idm_syn_free(syntax);
            idm_profile_scope_end(&prof);
            return idm_error_oom(err, span);
        }
        size_t literal_index = best->literal_index;
        if (literal_index == SIZE_MAX && (best->terminal.kind != (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL || !artifact->literal_token_indices_ready)) {
            reader_artifact_find_literal_index(artifact, syntax, &literal_index, &literal_scans);
        }
        ReaderArtifactToken token = {best, lexeme, syntax, span, leading_space, adjacent_previous, literal_index};
        if (!reader_artifact_tokens_push(out, token, err)) {
            idm_byte_match_destroy(&match);
            free(lexeme);
            idm_syn_free(syntax);
            idm_profile_scope_end(&prof);
            return false;
        }
        reader_artifact_advance_position(source, pos, best_end, &line, &column);
        previous_end = best_end;
        pos = best_end;
        leading_space = false;
        idm_byte_match_destroy(&match);
    }
    idm_profile_count("reader.artifact.lex.bytes", (uint64_t)len);
    idm_profile_count("reader.artifact.lex.regex_calls", regex_calls);
    idm_profile_count("reader.artifact.lex.custom_scans", custom_scans);
    idm_profile_count("reader.artifact.lex.literal_scans", literal_scans);
    idm_profile_count("reader.artifact.lex.skips", skipped);
    idm_profile_count("reader.artifact.lex.tokens", (uint64_t)out->count);
    idm_profile_scope_end(&prof);
    return true;
}

static IdmSyntax *reader_artifact_empty_match(IdmSpan span, IdmError *err) {
    IdmSyntax *list = idm_syn_list(span);
    if (!list) idm_error_oom(err, span);
    return list;
}

static IdmSyntax *reader_artifact_clone_token_at(const ReaderArtifactTokenVec *tokens, size_t pos, IdmError *err) {
    IdmSyntax *clone = idm_syn_clone(tokens->items[pos].syntax);
    if (!clone) idm_error_oom(err, tokens->items[pos].span);
    return clone;
}

static bool reader_artifact_capture_last_column(ReaderArtifactCaptures *captures, size_t slot, unsigned *out) {
    ReaderArtifactCapture *cap = reader_artifact_capture_at(captures, slot);
    if (!cap || cap->count == 0) return false;
    const IdmSyntax *syntax = cap->items[cap->count - 1u].syntax;
    if (!syntax || syntax->span.column == 0) return false;
    *out = syntax->span.column;
    return true;
}

static bool reader_artifact_indent_allows(const ReaderArtifactTokenVec *tokens, size_t pos, ReaderArtifactCaptures *captures, size_t slot, bool equal) {
    if (pos >= tokens->count) return false;
    unsigned base = 0;
    if (!reader_artifact_capture_last_column(captures, slot, &base)) return false;
    unsigned column = tokens->items[pos].span.column;
    return equal ? column == base : column > base;
}

typedef enum {
    READER_REDUCTION_FRAME_RULE,
    READER_REDUCTION_FRAME_NODE,
    READER_REDUCTION_FRAME_SEQ,
    READER_REDUCTION_FRAME_ALT,
    READER_REDUCTION_FRAME_OPTIONAL,
    READER_REDUCTION_FRAME_REPEAT,
    READER_REDUCTION_FRAME_REPEAT1,
    READER_REDUCTION_FRAME_NOT,
    READER_REDUCTION_FRAME_CAPTURE,
    READER_REDUCTION_FRAME_PEEK
} ReaderReductionFrameKind;

typedef struct {
    ReaderReductionFrameKind kind;
    uint8_t state;
    const ReaderArtifactRule *rule;
    size_t pc;
    size_t pos;
    size_t cursor;
    size_t child;
    size_t remaining;
    IdmSyntax *seq;
    ReaderArtifactCaptureCheckpoint checkpoint;
    bool checkpoint_active;
    bool memo_polluted;
    bool memo_starved;
} ReaderReductionFrame;

typedef struct {
    ReaderReductionFrame *items;
    size_t count;
    size_t cap;
} ReaderReductionStack;

static IdmSpan reader_artifact_token_span_at(const ReaderArtifactTokenVec *tokens, size_t pos) {
    return pos < tokens->count ? tokens->items[pos].span : idm_span_unknown(NULL);
}

static void reader_reduction_frame_abort(ReaderArtifactCaptures *captures, ReaderReductionFrame *frame) {
    if (frame->checkpoint_active) {
        reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
        frame->checkpoint_active = false;
    }
    idm_syn_free(frame->seq);
    frame->seq = NULL;
}

static void reader_reduction_stack_destroy(ReaderReductionStack *stack, ReaderArtifactCaptures *captures) {
    for (size_t i = 0; i < stack->count; i++) reader_reduction_frame_abort(captures, &stack->items[i]);
    free(stack->items);
    memset(stack, 0, sizeof(*stack));
}

static bool reader_reduction_push(ReaderReductionStack *stack, ReaderReductionFrame frame, IdmError *err, IdmSpan span) {
    if (stack->count == stack->cap) {
        if (!idm_grow((void **)&stack->items, &stack->cap, sizeof(*stack->items), 64u, stack->count + 1u)) return idm_error_oom(err, span);
    }
    stack->items[stack->count++] = frame;
    return true;
}

static bool reader_reduction_push_node(ReaderReductionStack *stack, const ReaderArtifactRule *rule, size_t pc, size_t pos, IdmError *err, IdmSpan span) {
    ReaderReductionFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.kind = READER_REDUCTION_FRAME_NODE;
    frame.rule = rule;
    frame.pc = pc;
    frame.pos = pos;
    return reader_reduction_push(stack, frame, err, span);
}

static bool reader_reduction_push_rule(ReaderReductionStack *stack, const ReaderArtifactRule *rule, size_t pos, IdmError *err, IdmSpan span) {
    ReaderReductionFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.kind = READER_REDUCTION_FRAME_RULE;
    frame.rule = rule;
    frame.pos = pos;
    return reader_reduction_push(stack, frame, err, span);
}

static void reader_reduction_return(ReaderReductionStack *stack, bool ok, IdmSyntax *syntax, size_t pos, bool *have_result, bool *result_ok, IdmSyntax **result, size_t *result_pos) {
    stack->count--;
    *have_result = true;
    *result_ok = ok;
    *result = syntax;
    *result_pos = pos;
}

static void reader_reduction_note_eof_demand(ReaderReductionStack *stack, bool *starved) {
    if (!starved || *starved) return;
    size_t stop = 0;
    bool stopped = false;
    bool set = false;
    for (size_t i = stack->count; i-- > 0 && !stopped;) {
        const ReaderReductionFrame *frame = &stack->items[i];
        switch (frame->kind) {
            case READER_REDUCTION_FRAME_REPEAT:
            case READER_REDUCTION_FRAME_REPEAT1:
            case READER_REDUCTION_FRAME_OPTIONAL:
            case READER_REDUCTION_FRAME_PEEK:
            case READER_REDUCTION_FRAME_NOT:
                stop = i;
                stopped = true;
                break;
            case READER_REDUCTION_FRAME_SEQ:
                if (frame->cursor > frame->pos) {
                    *starved = true;
                    set = true;
                    stop = i;
                    stopped = true;
                }
                break;
            default:
                break;
        }
    }
    for (size_t i = 0; i < stack->count; i++) {
        ReaderReductionFrame *frame = &stack->items[i];
        if (frame->kind != READER_REDUCTION_FRAME_RULE) continue;
        if (!stopped || i > stop) frame->memo_polluted = true;
        else if (set && i < stop) frame->memo_starved = true;
    }
}

static void reader_memo_note_indent(ReaderReductionStack *stack, const ReaderArtifactCaptures *captures, size_t slot) {
    size_t current = slot < captures->count ? captures->items[slot].count : 0;
    for (size_t i = stack->count; i-- > 0;) {
        ReaderReductionFrame *frame = &stack->items[i];
        if (frame->kind != READER_REDUCTION_FRAME_RULE || !frame->checkpoint_active) continue;
        size_t base = frame->checkpoint.item_counts && slot < frame->checkpoint.count ? frame->checkpoint.item_counts[slot] : 0;
        if (current > base) break;
        frame->memo_polluted = true;
    }
}

static void reader_memo_replay_starved(ReaderReductionStack *stack, bool *starved) {
    if (!starved || *starved) return;
    *starved = true;
    for (size_t i = 0; i < stack->count; i++) {
        if (stack->items[i].kind == READER_REDUCTION_FRAME_RULE) stack->items[i].memo_starved = true;
    }
}

typedef enum {
    READER_MEMO_EMPTY = 0,
    READER_MEMO_SEEN,
    READER_MEMO_FAIL,
    READER_MEMO_MATCH
} ReaderMemoState;

typedef struct {
    uint8_t state;
    bool starved;
    size_t end_pos;
    IdmSyntax *syntax;
} ReaderMemoEntry;

typedef struct {
    ReaderMemoEntry *entries;
    size_t stride;
    size_t count;
} ReaderMemo;

static bool reader_memo_init(ReaderMemo *memo, size_t form_count, size_t token_count, IdmError *err, IdmSpan span) {
    memset(memo, 0, sizeof(*memo));
    memo->stride = token_count + 1u;
    if (form_count == 0) return true;
    if (memo->stride > SIZE_MAX / sizeof(*memo->entries) / form_count) return idm_error_set(err, span, "reader artifact memo table too large");
    memo->count = form_count * memo->stride;
    memo->entries = calloc(memo->count, sizeof(*memo->entries));
    if (!memo->entries) return idm_error_oom(err, span);
    return true;
}

static void reader_memo_destroy(ReaderMemo *memo) {
    for (size_t i = 0; i < memo->count; i++) {
        if (memo->entries[i].state == READER_MEMO_MATCH) idm_syn_free(memo->entries[i].syntax);
    }
    free(memo->entries);
    memset(memo, 0, sizeof(*memo));
}

static ReaderMemoEntry *reader_memo_at(ReaderMemo *memo, const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pos) {
    if (!memo->entries) return NULL;
    size_t index = (size_t)(rule - artifact->forms);
    IDM_ASSERT_PROVED(index < artifact->form_count && pos < memo->stride);
    return &memo->entries[index * memo->stride + pos];
}

static bool reader_artifact_inst_can_start(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, size_t pc, const ReaderArtifactTokenVec *tokens, size_t pos) {
    IDM_ASSERT_PROVED(pc < rule->reduction_count && rule->first);
    if (rule->reductions[pc].nullable) return true;
    if (pos >= tokens->count) return false;
    const uint64_t *first = rule->first + pc * artifact->first_words;
    const ReaderArtifactToken *token = &tokens->items[pos];
    size_t token_index = (size_t)(token->rule - artifact->tokens);
    IDM_ASSERT_PROVED(token_index < artifact->token_count);
    if (reader_artifact_first_has(first, token_index)) return true;
    return token->literal_index != SIZE_MAX &&
           reader_artifact_first_has(first, artifact->token_count + token->literal_index);
}

static void reader_artifact_alt_skip_impossible(const IdmReaderArtifact *artifact, ReaderReductionFrame *frame, const ReaderArtifactTokenVec *tokens) {
    while (frame->remaining != 0 &&
           !reader_artifact_inst_can_start(artifact, frame->rule, frame->child, tokens, frame->pos)) {
        frame->child = frame->rule->reductions[frame->child].next;
        frame->remaining--;
    }
}

static IdmSyntax *reader_artifact_match_rule(const IdmReaderArtifact *artifact, const ReaderArtifactRule *rule, const ReaderArtifactTokenVec *tokens, size_t pos, ReaderArtifactCaptures *captures, size_t *out_pos, bool *starved, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.artifact.match_rule");
    ReaderReductionStack stack = {0};
    bool have_result = false;
    bool result_ok = false;
    IdmSyntax *result = NULL;
    size_t result_pos = pos;
    uint64_t frames = 0;
    uint64_t memo_hits = 0;
    ReaderMemo memo;
    if (!reader_memo_init(&memo, artifact->form_count, tokens->count, err, reader_artifact_token_span_at(tokens, pos))) {
        idm_profile_scope_end(&prof);
        return NULL;
    }
    if (!reader_reduction_push_rule(&stack, rule, pos, err, reader_artifact_token_span_at(tokens, pos))) {
        reader_memo_destroy(&memo);
        idm_profile_scope_end(&prof);
        return NULL;
    }
    while (stack.count != 0) {
        frames++;
        if (stack.count > IDM_IC_MAX_DEPTH * 8u) {
            idm_error_set(err, reader_artifact_token_span_at(tokens, result_pos), "reader artifact nested too deeply");
            break;
        }
        ReaderReductionFrame *frame = &stack.items[stack.count - 1u];
        IdmSpan span = reader_artifact_token_span_at(tokens, frame->pos);
        if (have_result) {
            bool child_ok = result_ok;
            IdmSyntax *child = result;
            size_t child_pos = result_pos;
            have_result = false;
            result = NULL;
            switch (frame->kind) {
                case READER_REDUCTION_FRAME_RULE:
                    if (!child_ok) {
                        reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                        frame->checkpoint_active = false;
                        if (!frame->memo_polluted) {
                            ReaderMemoEntry *entry = reader_memo_at(&memo, artifact, frame->rule, frame->pos);
                            if (entry) {
                                entry->state = READER_MEMO_FAIL;
                                entry->starved = frame->memo_starved;
                            }
                        }
                        reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    idm_syn_free(child);
                    child = NULL;
                    {
                        IdmSyntax *built = reader_artifact_construct(artifact, &frame->rule->constructor, captures, frame->checkpoint.item_counts, frame->checkpoint.count, span, err);
                        reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                        frame->checkpoint_active = false;
                        if (!built) break;
                        bool inherits_token = reader_artifact_construct_inherits_token(built);
                        if (!inherits_token) reader_artifact_clear_token(built);
                        if (frame->pos < tokens->count && inherits_token) {
                            ReaderArtifactToken *tok = &tokens->items[frame->pos];
                            if (!reader_artifact_set_token_n(built, tok->lexeme, strlen(tok->lexeme), tok->leading_space, tok->adjacent_previous, tok->span, err)) {
                                idm_syn_free(built);
                                break;
                            }
                        }
                        if (!frame->memo_polluted) {
                            ReaderMemoEntry *entry = reader_memo_at(&memo, artifact, frame->rule, frame->pos);
                            if (entry && entry->state == READER_MEMO_EMPTY) {
                                entry->state = READER_MEMO_SEEN;
                            } else if (entry && entry->state == READER_MEMO_SEEN) {
                                IdmSyntax *copy = idm_syn_clone(built);
                                if (copy) {
                                    entry->state = READER_MEMO_MATCH;
                                    entry->starved = frame->memo_starved;
                                    entry->end_pos = child_pos;
                                    entry->syntax = copy;
                                }
                            }
                        }
                        reader_reduction_return(&stack, true, built, child_pos, &have_result, &result_ok, &result, &result_pos);
                    }
                    break;
                case READER_REDUCTION_FRAME_NODE:
                    reader_reduction_return(&stack, child_ok, child_ok ? child : NULL, child_pos, &have_result, &result_ok, &result, &result_pos);
                    if (!child_ok) idm_syn_free(child);
                    break;
                case READER_REDUCTION_FRAME_SEQ:
                    if (!child_ok) {
                        idm_syn_free(child);
                        idm_syn_free(frame->seq);
                        frame->seq = NULL;
                        reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    if (!reader_artifact_sequence_append(frame->seq, child, err, span)) break;
                    frame->cursor = child_pos;
                    frame->child = frame->rule->reductions[frame->child].next;
                    frame->remaining--;
                    frame->state = 0;
                    break;
                case READER_REDUCTION_FRAME_ALT:
                    if (child_ok) {
                        reader_artifact_capture_checkpoint_commit(&frame->checkpoint);
                        frame->checkpoint_active = false;
                        reader_reduction_return(&stack, true, child, child_pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    idm_syn_free(child);
                    reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                    frame->checkpoint_active = false;
                    frame->child = frame->rule->reductions[frame->child].next;
                    frame->remaining--;
                    frame->state = 0;
                    break;
                case READER_REDUCTION_FRAME_OPTIONAL:
                    if (child_ok) {
                        reader_artifact_capture_checkpoint_commit(&frame->checkpoint);
                        frame->checkpoint_active = false;
                        reader_reduction_return(&stack, true, child, child_pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    idm_syn_free(child);
                    reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                    frame->checkpoint_active = false;
                    {
                        IdmSyntax *empty = reader_artifact_empty_match(span, err);
                        if (!empty) break;
                        reader_reduction_return(&stack, true, empty, frame->pos, &have_result, &result_ok, &result, &result_pos);
                    }
                    break;
                case READER_REDUCTION_FRAME_REPEAT1:
                case READER_REDUCTION_FRAME_REPEAT:
                    if (child_ok && child_pos > frame->cursor) {
                        reader_artifact_capture_checkpoint_commit(&frame->checkpoint);
                        frame->checkpoint_active = false;
                        if (!reader_artifact_sequence_append(frame->seq, child, err, span)) break;
                        frame->cursor = child_pos;
                        frame->state = 0;
                        break;
                    }
                    idm_syn_free(child);
                    reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                    frame->checkpoint_active = false;
                    {
                        IdmSyntax *seq = frame->seq;
                        frame->seq = NULL;
                        if (frame->kind == READER_REDUCTION_FRAME_REPEAT1 && frame->cursor == frame->pos) {
                            idm_syn_free(seq);
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        } else {
                            reader_reduction_return(&stack, true, seq, frame->cursor, &have_result, &result_ok, &result, &result_pos);
                        }
                    }
                    break;
                case READER_REDUCTION_FRAME_CAPTURE:
                    if (!child_ok) {
                        idm_syn_free(child);
                        reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    if (!reader_artifact_capture_add(captures, frame->rule->reductions[frame->pc].capture_slot, child, err, child->span)) break;
                    reader_reduction_return(&stack, true, child, child_pos, &have_result, &result_ok, &result, &result_pos);
                    break;
                case READER_REDUCTION_FRAME_NOT:
                    idm_syn_free(child);
                    reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                    frame->checkpoint_active = false;
                    if (child_ok) {
                        reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                    } else {
                        IdmSyntax *empty_not = reader_artifact_empty_match(span, err);
                        if (!empty_not) break;
                        reader_reduction_return(&stack, true, empty_not, frame->pos, &have_result, &result_ok, &result, &result_pos);
                    }
                    break;
                case READER_REDUCTION_FRAME_PEEK:
                    idm_syn_free(child);
                    if (child_ok) {
                        reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                        frame->checkpoint_active = false;
                        IdmSyntax *empty = reader_artifact_empty_match(span, err);
                        if (!empty) break;
                        reader_reduction_return(&stack, true, empty, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    reader_artifact_capture_checkpoint_rollback(captures, &frame->checkpoint);
                    frame->checkpoint_active = false;
                    reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                    break;
            }
            if (err && err->present) break;
            continue;
        }
        switch (frame->kind) {
            case READER_REDUCTION_FRAME_RULE:
                if (frame->state == 0) {
                    IDM_ASSERT_PROVED(frame->rule && frame->rule->reductions && frame->rule->reduction_count != 0);
                    ReaderMemoEntry *entry = reader_memo_at(&memo, artifact, frame->rule, frame->pos);
                    if (entry && entry->state == READER_MEMO_FAIL) {
                        memo_hits++;
                        if (entry->starved) reader_memo_replay_starved(&stack, starved);
                        reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    if (entry && entry->state == READER_MEMO_MATCH) {
                        IdmSyntax *clone = idm_syn_clone(entry->syntax);
                        if (!clone) {
                            idm_error_oom(err, span);
                            break;
                        }
                        memo_hits++;
                        if (entry->starved) reader_memo_replay_starved(&stack, starved);
                        reader_reduction_return(&stack, true, clone, entry->end_pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    if (!reader_artifact_capture_checkpoint(captures, &frame->checkpoint, err, span)) break;
                    frame->checkpoint_active = true;
                    frame->state = 1;
                    if (!reader_reduction_push_node(&stack, frame->rule, 0, frame->pos, err, span)) break;
                }
                break;
            case READER_REDUCTION_FRAME_NODE: {
                IDM_ASSERT_PROVED(frame->rule && frame->pc < frame->rule->reduction_count);
                const ReaderReductionInst *inst = &frame->rule->reductions[frame->pc];
                IDM_ASSERT_PROVED(inst->op < IDM_READER_PATTERN_OP_COUNT);
                switch ((IdmReaderPatternOp)inst->op) {
                    case IDM_READER_PATTERN_EMPTY: {
                        IdmSyntax *empty = reader_artifact_empty_match(span, err);
                        if (!empty) break;
                        reader_reduction_return(&stack, true, empty, frame->pos, &have_result, &result_ok, &result, &result_pos);
                        break;
                    }
                    case IDM_READER_PATTERN_REF:
                        if (inst->target_kind == IDM_READER_PATTERN_TARGET_FORM) {
                            IDM_ASSERT_PROVED(inst->target_index < artifact->form_count);
                            frame->state = 1;
                            if (!reader_reduction_push_rule(&stack, &artifact->forms[inst->target_index], frame->pos, err, span)) break;
                            break;
                        }
                        IDM_ASSERT_PROVED(inst->target_kind == IDM_READER_PATTERN_TARGET_TOKEN && inst->target_index < artifact->token_count);
                        if (frame->pos >= tokens->count || tokens->items[frame->pos].rule != &artifact->tokens[inst->target_index]) {
                            if (frame->pos >= tokens->count) reader_reduction_note_eof_demand(&stack, starved);
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                            break;
                        }
                        {
                            IdmSyntax *clone = reader_artifact_clone_token_at(tokens, frame->pos, err);
                            if (!clone) break;
                            reader_reduction_return(&stack, true, clone, frame->pos + 1u, &have_result, &result_ok, &result, &result_pos);
                        }
                        break;
                    case IDM_READER_PATTERN_TOKEN:
                        IDM_ASSERT_PROVED(inst->target_index < artifact->token_count);
                        if (frame->pos >= tokens->count || tokens->items[frame->pos].rule != &artifact->tokens[inst->target_index]) {
                            if (frame->pos >= tokens->count) reader_reduction_note_eof_demand(&stack, starved);
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                            break;
                        }
                        {
                            IdmSyntax *clone = reader_artifact_clone_token_at(tokens, frame->pos, err);
                            if (!clone) break;
                            reader_reduction_return(&stack, true, clone, frame->pos + 1u, &have_result, &result_ok, &result, &result_pos);
                        }
                        break;
                    case IDM_READER_PATTERN_LITERAL:
                        IDM_ASSERT_PROVED(inst->literal_index < artifact->literal_count);
                        if (frame->pos >= tokens->count || !idm_syn_equal(artifact->literals[inst->literal_index], tokens->items[frame->pos].syntax)) {
                            if (frame->pos >= tokens->count) reader_reduction_note_eof_demand(&stack, starved);
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                            break;
                        }
                        {
                            IdmSyntax *clone = reader_artifact_clone_token_at(tokens, frame->pos, err);
                            if (!clone) break;
                            reader_reduction_return(&stack, true, clone, frame->pos + 1u, &have_result, &result_ok, &result, &result_pos);
                        }
                        break;
                    case IDM_READER_PATTERN_SEQ:
                        frame->kind = READER_REDUCTION_FRAME_SEQ;
                        frame->cursor = frame->pos;
                        frame->child = inst->first_child;
                        frame->remaining = inst->child_count;
                        frame->seq = idm_syn_list(span);
                        if (!frame->seq) idm_error_oom(err, span);
                        break;
                    case IDM_READER_PATTERN_ALT:
                        frame->kind = READER_REDUCTION_FRAME_ALT;
                        frame->child = inst->first_child;
                        frame->remaining = inst->child_count;
                        break;
                    case IDM_READER_PATTERN_OPTIONAL:
                        frame->kind = READER_REDUCTION_FRAME_OPTIONAL;
                        frame->child = inst->first_child;
                        break;
                    case IDM_READER_PATTERN_REPEAT:
                        frame->kind = READER_REDUCTION_FRAME_REPEAT;
                        frame->cursor = frame->pos;
                        frame->child = inst->first_child;
                        frame->seq = idm_syn_list(span);
                        if (!frame->seq) idm_error_oom(err, span);
                        break;
                    case IDM_READER_PATTERN_REPEAT1:
                        frame->kind = READER_REDUCTION_FRAME_REPEAT1;
                        frame->cursor = frame->pos;
                        frame->child = inst->first_child;
                        frame->seq = idm_syn_list(span);
                        if (!frame->seq) idm_error_oom(err, span);
                        break;
                    case IDM_READER_PATTERN_NOT:
                        frame->kind = READER_REDUCTION_FRAME_NOT;
                        frame->child = inst->first_child;
                        break;
                    case IDM_READER_PATTERN_PARAM:
                        idm_error_set(err, span, "reader rule parameter was never instantiated%s", "");
                        break;
                    case IDM_READER_PATTERN_CAPTURE:
                        frame->kind = READER_REDUCTION_FRAME_CAPTURE;
                        frame->child = inst->first_child;
                        break;
                    case IDM_READER_PATTERN_INDENT_GT:
                    case IDM_READER_PATTERN_INDENT_EQ:
                        reader_memo_note_indent(&stack, captures, inst->capture_slot);
                        if (!reader_artifact_indent_allows(tokens, frame->pos, captures, inst->capture_slot, inst->op == IDM_READER_PATTERN_INDENT_EQ)) {
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                            break;
                        }
                        frame->state = 1;
                        if (!reader_reduction_push_node(&stack, frame->rule, inst->first_child, frame->pos, err, span)) break;
                        break;
                    case IDM_READER_PATTERN_ADJACENT:
                    case IDM_READER_PATTERN_NOT_ADJACENT:
                        if (frame->pos >= tokens->count || tokens->items[frame->pos].syntax->token_adjacent_previous != (inst->op == IDM_READER_PATTERN_ADJACENT)) {
                            reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                            break;
                        }
                        frame->state = 1;
                        if (!reader_reduction_push_node(&stack, frame->rule, inst->first_child, frame->pos, err, span)) break;
                        break;
                    case IDM_READER_PATTERN_PEEK:
                        frame->kind = READER_REDUCTION_FRAME_PEEK;
                        frame->child = inst->first_child;
                        break;
                }
                break;
            }
            case READER_REDUCTION_FRAME_SEQ:
                if (frame->remaining == 0) {
                    IdmSyntax *seq = frame->seq;
                    frame->seq = NULL;
                    reader_reduction_return(&stack, true, seq, frame->cursor, &have_result, &result_ok, &result, &result_pos);
                } else {
                    frame->state = 1;
                    if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->cursor, err, span)) break;
                }
                break;
            case READER_REDUCTION_FRAME_ALT:
                reader_artifact_alt_skip_impossible(artifact, frame, tokens);
                if (frame->remaining == 0) {
                    if (frame->pos >= tokens->count) reader_reduction_note_eof_demand(&stack, starved);
                    reader_reduction_return(&stack, false, NULL, frame->pos, &have_result, &result_ok, &result, &result_pos);
                } else {
                    if (!reader_artifact_capture_checkpoint(captures, &frame->checkpoint, err, span)) break;
                    frame->checkpoint_active = true;
                    frame->state = 1;
                    if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->pos, err, span)) break;
                }
                break;
            case READER_REDUCTION_FRAME_OPTIONAL:
                if (!reader_artifact_capture_checkpoint(captures, &frame->checkpoint, err, span)) break;
                frame->checkpoint_active = true;
                frame->state = 1;
                if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->pos, err, span)) break;
                break;
            case READER_REDUCTION_FRAME_REPEAT:
            case READER_REDUCTION_FRAME_REPEAT1:
                if (!reader_artifact_capture_checkpoint(captures, &frame->checkpoint, err, span)) break;
                frame->checkpoint_active = true;
                frame->state = 1;
                if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->cursor, err, span)) break;
                break;
            case READER_REDUCTION_FRAME_CAPTURE:
                frame->state = 1;
                if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->pos, err, span)) break;
                break;
            case READER_REDUCTION_FRAME_NOT:
            case READER_REDUCTION_FRAME_PEEK:
                if (!reader_artifact_capture_checkpoint(captures, &frame->checkpoint, err, span)) break;
                frame->checkpoint_active = true;
                frame->state = 1;
                if (!reader_reduction_push_node(&stack, frame->rule, frame->child, frame->pos, err, span)) break;
                break;
        }
        if (err && err->present) break;
    }
    reader_memo_destroy(&memo);
    idm_profile_count("reader.artifact.match_rule.frames", frames);
    idm_profile_count("reader.artifact.match_rule.memo_hits", memo_hits);
    if (err && err->present) {
        idm_syn_free(result);
        reader_reduction_stack_destroy(&stack, captures);
        idm_profile_scope_end(&prof);
        return NULL;
    }
    reader_reduction_stack_destroy(&stack, captures);
    if (!have_result || !result_ok) {
        idm_syn_free(result);
        idm_profile_scope_end(&prof);
        return NULL;
    }
    *out_pos = result_pos;
    idm_profile_scope_end(&prof);
    return result;
}

static bool reader_artifact_pairs_open(const IdmReaderArtifact *artifact, const ReaderArtifactTokenVec *tokens) {
    for (size_t p = 0; p < artifact->pair_count; p++) {
        const ReaderArtifactRule *open_rule = &artifact->tokens[artifact->pairs[p].open];
        const ReaderArtifactRule *close_rule = &artifact->tokens[artifact->pairs[p].close];
        size_t depth = 0;
        for (size_t i = 0; i < tokens->count; i++) {
            if (tokens->items[i].rule == open_rule) depth++;
            else if (tokens->items[i].rule == close_rule && depth != 0) depth--;
        }
        if (depth != 0) return true;
    }
    return false;
}

static bool reader_artifact_read_program(const IdmReaderArtifact *artifact, const char *file, const ReaderArtifactTokenVec *tokens, IdmSyntax **out, bool *starved, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.artifact.read_program");
    const ReaderArtifactRule *start_rule = &artifact->forms[artifact->start_form];
    ReaderArtifactCaptures captures;
    if (!reader_artifact_captures_init(&captures, artifact->capture_name_count, err, tokens->count ? tokens->items[0].span : idm_span_unknown(file))) {
        idm_profile_scope_end(&prof);
        return false;
    }
    IdmError trial;
    idm_error_init(&trial);
    size_t end = 0;
    bool match_starved = false;
    IdmSyntax *program = reader_artifact_match_rule(artifact, start_rule, tokens, 0, &captures, &end, &match_starved, &trial);
    reader_artifact_captures_destroy(&captures);
    idm_profile_count("reader.artifact.read_program.tokens", (uint64_t)tokens->count);
    idm_profile_count("reader.artifact.read_program.capture_slots", (uint64_t)artifact->capture_name_count);
    if (trial.present) {
        IdmSpan span = trial.span;
        const char *message = trial.message ? trial.message : "reader artifact program rule failed";
        bool reported = idm_error_set(err, span, "%s", message);
        if (starved && match_starved) *starved = true;
        idm_syn_free(program);
        idm_error_clear(&trial);
        idm_profile_scope_end(&prof);
        return reported;
    }
    if (!program) {
        IdmSpan span = tokens->count ? tokens->items[0].span : idm_span_unknown(file);
        if (starved && match_starved) *starved = true;
        idm_error_clear(&trial);
        idm_profile_scope_end(&prof);
        return idm_error_set(err, span, "reader artifact surface '%s' did not match program", artifact->surface);
    }
    idm_error_clear(&trial);
    if (end != tokens->count) {
        IdmSpan span = end < tokens->count ? tokens->items[end].span : idm_span_unknown(file);
        if (starved && match_starved) *starved = true;
        idm_syn_free(program);
        idm_profile_scope_end(&prof);
        return idm_error_set(err, span, "reader artifact surface '%s' left unmatched input at '%s'", artifact->surface, end < tokens->count ? tokens->items[end].lexeme : "<eof>");
    }
    IDM_ASSERT_PROVED(reader_artifact_is_package_begin(program));
    *out = program;
    idm_profile_scope_end(&prof);
    return true;
}

static void reader_artifact_regex_items_free(IdmRegex **items, size_t count) {
    if (!items) return;
    for (size_t i = 0; i < count; i++) idm_regex_free(items[i]);
    free(items);
}

static int reader_artifact_rule_order_cmp(const void *a, const void *b) {
    const ReaderArtifactRule *ra = *(const ReaderArtifactRule *const *)a;
    const ReaderArtifactRule *rb = *(const ReaderArtifactRule *const *)b;
    return (ra->order > rb->order) - (ra->order < rb->order);
}

static bool reader_artifact_build_lex_rules(IdmReaderArtifact *artifact, IdmError *err) {
    free(artifact->lex_rules);
    artifact->lex_rules = NULL;
    artifact->lex_count = 0;
    for (size_t i = 0; i < artifact->token_count; i++) {
        if (reader_artifact_terminal_uses_regex_program(artifact->tokens[i].terminal.kind)) artifact->lex_count++;
    }
    for (size_t i = 0; i < artifact->skip_count; i++) {
        if (reader_artifact_terminal_uses_regex_program(artifact->skips[i].terminal.kind)) artifact->lex_count++;
    }
    if (artifact->lex_count == 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no regex terminals", artifact->surface);
    artifact->lex_rules = calloc(artifact->lex_count, sizeof(*artifact->lex_rules));
    if (!artifact->lex_rules) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t index = 0;
    for (size_t i = 0; i < artifact->token_count; i++) {
        if (reader_artifact_terminal_uses_regex_program(artifact->tokens[i].terminal.kind)) artifact->lex_rules[index++] = &artifact->tokens[i];
    }
    for (size_t i = 0; i < artifact->skip_count; i++) {
        if (reader_artifact_terminal_uses_regex_program(artifact->skips[i].terminal.kind)) artifact->lex_rules[index++] = &artifact->skips[i];
    }
    qsort(artifact->lex_rules, artifact->lex_count, sizeof(*artifact->lex_rules), reader_artifact_rule_order_cmp);
    return true;
}

static bool reader_artifact_validate_terminal_program(const IdmReaderArtifact *artifact, IdmError *err) {
    if (!artifact || !artifact->terminal_program) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal program is missing");
    if (idm_regex_set_count(artifact->terminal_program) != artifact->lex_count) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal program count does not match rules");
    bool empty = false;
    if (!idm_regex_set_matches_empty(artifact->terminal_program, &empty, err)) return false;
    if (empty) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal program must not match empty input");
    for (size_t i = 0; i < artifact->lex_count; i++) {
        const ReaderArtifactRule *rule = artifact->lex_rules[i];
        size_t group_count = idm_regex_set_group_count(artifact->terminal_program, i);
        if (rule->terminal.kind == (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL && group_count != 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact literal terminal '%s' has capture groups", rule->name ? rule->name : "<bad>");
        for (size_t g = 1u; g <= group_count; g++) {
            const char *group = idm_regex_set_group_name(artifact->terminal_program, i, g);
            if (group && rule->name && strcmp(group, rule->name) == 0) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal '%s' capture group conflicts with rule name", rule->name);
        }
    }
    return true;
}

static bool reader_artifact_compile_rule_terminal(const ReaderArtifactRule *rule, IdmRegex **out, IdmError *err) {
    const char *source = rule->terminal.text ? rule->terminal.text : "";
    size_t source_len = strlen(source);
    uint32_t flags = rule->terminal.flags;
    IdmBuffer escaped;
    idm_buf_init(&escaped);
    if (rule->terminal.kind == IDM_GRAMMAR_TERMINAL_LITERAL) {
        if (!reader_artifact_regex_escape_literal(&escaped, source)) {
            idm_buf_destroy(&escaped);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        source = escaped.data ? escaped.data : "";
        source_len = escaped.len;
        flags = 0;
    }
    bool ok = idm_regex_compile(source, source_len, flags, out, err);
    if (ok && *out) {
        for (size_t i = 1u; i <= idm_regex_group_count(*out); i++) {
            const char *group = idm_regex_group_name(*out, i);
            if (group && strcmp(group, rule->name) == 0) {
                idm_regex_free(*out);
                *out = NULL;
                ok = idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal '%s' capture group conflicts with rule name", rule->name);
                break;
            }
        }
    }
    idm_buf_destroy(&escaped);
    return ok;
}

static bool reader_artifact_compile_terminal_program(IdmReaderArtifact *artifact, IdmError *err) {
    if (!reader_artifact_build_lex_rules(artifact, err)) return false;
    IdmRegex **items = calloc(artifact->lex_count, sizeof(*items));
    if (!items) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < artifact->lex_count; i++) {
        if (!reader_artifact_compile_rule_terminal(artifact->lex_rules[i], &items[i], err)) {
            reader_artifact_regex_items_free(items, artifact->lex_count);
            return false;
        }
    }
    bool ok = idm_regex_set_compile((const IdmRegex *const *)items, artifact->lex_count, &artifact->terminal_program, err);
    reader_artifact_regex_items_free(items, artifact->lex_count);
    return ok && reader_artifact_validate_terminal_program(artifact, err);
}

static bool reader_artifact_rule_name_exists(const IdmReaderArtifact *artifact, const char *name) {
    return reader_artifact_find_token_rule(artifact, name) || reader_artifact_find_form_rule(artifact, name) || reader_artifact_find_skip_rule(artifact, name);
}

static bool reader_artifact_scope_source_equal(const IdmScopeSet *a, const IdmScopeSet *b) {
    static const IdmScopeSet empty = {0};
    return idm_scope_set_equal(a ? a : &empty, b ? b : &empty);
}

static bool reader_artifact_capture_slot(IdmReaderArtifact *artifact, const char *name, size_t *out, IdmError *err) {
    if (!name || name[0] == '\0') return idm_error_set(err, idm_span_unknown(NULL), "reader artifact capture name is empty");
    for (size_t i = 0; i < artifact->capture_name_count; i++) {
        if (strcmp(artifact->capture_names[i], name) == 0) {
            *out = i;
            return true;
        }
    }
    if (artifact->capture_name_count == artifact->capture_name_cap) {
        if (!idm_grow((void **)&artifact->capture_names, &artifact->capture_name_cap, sizeof(*artifact->capture_names), 16u, artifact->capture_name_count + 1u)) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    char *copy = idm_strdup(name);
    if (!copy) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = artifact->capture_name_count;
    artifact->capture_names[artifact->capture_name_count++] = copy;
    return true;
}

static bool reader_artifact_capture_slot_lookup(const IdmReaderArtifact *artifact, const char *name, size_t *out) {
    if (!name || name[0] == '\0') return false;
    for (size_t i = 0; i < artifact->capture_name_count; i++) {
        if (strcmp(artifact->capture_names[i], name) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool reader_artifact_set_instruction_capture_slot(size_t *slot, size_t resolved, const char *name, IdmError *err) {
    if (*slot != SIZE_MAX && *slot != resolved) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact capture '%s' slot mismatch", name ? name : "<bad>");
    *slot = resolved;
    return true;
}

static bool reader_artifact_resolve_terminal_capture_slots(IdmReaderArtifact *artifact, IdmError *err) {
    for (size_t i = 0; i < artifact->lex_count; i++) {
        ReaderArtifactRule *rule = (ReaderArtifactRule *)artifact->lex_rules[i];
        size_t slot = 0;
        if (!reader_artifact_capture_slot(artifact, rule->name, &slot, err)) return false;
        rule->capture_slot = slot;
        size_t group_count = idm_regex_set_group_count(artifact->terminal_program, i);
        free(rule->terminal_capture_slots);
        rule->terminal_capture_slots = NULL;
        rule->terminal_capture_slot_count = group_count + 1u;
        rule->terminal_capture_slots = calloc(rule->terminal_capture_slot_count, sizeof(*rule->terminal_capture_slots));
        if (!rule->terminal_capture_slots) return idm_error_oom(err, idm_span_unknown(NULL));
        rule->terminal_capture_slots[0] = slot;
        for (size_t g = 1u; g <= group_count; g++) {
            const char *group = idm_regex_set_group_name(artifact->terminal_program, i, g);
            if (!group || group[0] == '\0') {
                rule->terminal_capture_slots[g] = SIZE_MAX;
                continue;
            }
            if (!reader_artifact_capture_slot(artifact, group, &rule->terminal_capture_slots[g], err)) return false;
        }
    }
    for (size_t section = 0; section < 2u; section++) {
        ReaderArtifactRule *rules = section == 0 ? artifact->tokens : artifact->skips;
        size_t count = section == 0 ? artifact->token_count : artifact->skip_count;
        for (size_t i = 0; i < count; i++) {
            ReaderArtifactRule *rule = &rules[i];
            if (reader_artifact_terminal_uses_regex_program(rule->terminal.kind)) continue;
            size_t slot = 0;
            if (!reader_artifact_capture_slot(artifact, rule->name, &slot, err)) return false;
            rule->capture_slot = slot;
            free(rule->terminal_capture_slots);
            rule->terminal_capture_slot_count = 1u;
            rule->terminal_capture_slots = calloc(1u, sizeof(*rule->terminal_capture_slots));
            if (!rule->terminal_capture_slots) return idm_error_oom(err, idm_span_unknown(NULL));
            rule->terminal_capture_slots[0] = slot;
        }
    }
    return true;
}

static bool reader_artifact_resolve_reduction_capture_slots(IdmReaderArtifact *artifact, ReaderArtifactRule *rule, IdmError *err) {
    for (size_t i = 0; i < rule->reduction_count; i++) {
        ReaderReductionInst *inst = &rule->reductions[i];
        if (inst->op != IDM_READER_PATTERN_CAPTURE && inst->op != IDM_READER_PATTERN_INDENT_GT && inst->op != IDM_READER_PATTERN_INDENT_EQ) continue;
        size_t slot = 0;
        if (!reader_artifact_capture_slot(artifact, inst->name, &slot, err) ||
            !reader_artifact_set_instruction_capture_slot(&inst->capture_slot, slot, inst->name, err)) return false;
        free(inst->name);
        inst->name = NULL;
    }
    return true;
}

static bool reader_artifact_resolve_ctor_capture_slots(const IdmReaderArtifact *artifact, IdmReaderProgram *program, IdmError *err) {
    for (size_t i = 0; i < program->count; i++) {
        IdmReaderInst *inst = &program->items[i];
        switch ((IdmReaderCtorOp)inst->op) {
            case IDM_READER_CTOR_CAPTURE:
            case IDM_READER_CTOR_SPLICE:
            case IDM_READER_CTOR_CAPTURE_TEXT:
            case IDM_READER_CTOR_INTERP_STRING:
            case IDM_READER_CTOR_BITSTRING: {
                size_t slot = 0;
                if (!reader_artifact_capture_slot_lookup(artifact, inst->text, &slot)) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact constructor references unknown capture '%s'", inst->text ? inst->text : "<bad>");
                if (!reader_artifact_set_instruction_capture_slot(&inst->capture_slot, slot, inst->text, err)) return false;
                break;
            }
            default:
                break;
        }
    }
    return true;
}

static bool reader_artifact_resolve_capture_slots(IdmReaderArtifact *artifact, IdmError *err) {
    if (!artifact->terminal_program || artifact->lex_count == 0 || !artifact->lex_rules) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact terminal program must be resolved before capture slots");
    for (size_t i = 0; i < artifact->capture_name_count; i++) free(artifact->capture_names[i]);
    free(artifact->capture_names);
    artifact->capture_names = NULL;
    artifact->capture_name_count = 0;
    artifact->capture_name_cap = 0;
    if (!reader_artifact_resolve_terminal_capture_slots(artifact, err)) return false;
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_resolve_reduction_capture_slots(artifact, &artifact->forms[i], err)) return false;
    }
    for (size_t i = 0; i < artifact->token_count; i++) {
        if (!reader_artifact_resolve_ctor_capture_slots(artifact, &artifact->tokens[i].constructor, err)) return false;
    }
    for (size_t i = 0; i < artifact->form_count; i++) {
        if (!reader_artifact_resolve_ctor_capture_slots(artifact, &artifact->forms[i].constructor, err)) return false;
    }
    return true;
}




static bool reader_artifact_add_rule(IdmReaderArtifact *artifact, const IdmGrammarRule *src, const char *provider, const char *provider_key, const IdmScopeSet *binding_scopes, IdmError *err, IdmSpan span) {
    if (reader_artifact_rule_name_exists(artifact, src->name)) return idm_error_set(err, span, "reader artifact surface '%s' declares rule '%s' more than once", artifact->surface, src->name);
    ReaderArtifactRule **items = NULL;
    size_t *count = NULL;
    size_t *cap = NULL;
    if (src->kind == (uint8_t)IDM_GRAMMAR_RULE_TOKEN) {
        items = &artifact->tokens;
        count = &artifact->token_count;
        cap = &artifact->token_cap;
    } else if (src->kind == (uint8_t)IDM_GRAMMAR_RULE_FORM) {
        items = &artifact->forms;
        count = &artifact->form_count;
        cap = &artifact->form_cap;
    } else if (src->kind == (uint8_t)IDM_GRAMMAR_RULE_SKIP) {
        items = &artifact->skips;
        count = &artifact->skip_count;
        cap = &artifact->skip_cap;
    } else {
        return idm_error_set(err, span, "reader artifact surface '%s' has invalid rule kind", artifact->surface);
    }
    if (*count == *cap) {
        if (!idm_grow((void **)items, cap, sizeof(**items), 8u, *count + 1u)) return idm_error_oom(err, span);
    }
    ReaderArtifactRule *dst = &(*items)[(*count)++];
    memset(dst, 0, sizeof(*dst));
    idm_scope_set_init(&dst->binding_scopes);
    dst->name = idm_strdup(src->name);
    dst->provider = provider ? idm_strdup(provider) : NULL;
    dst->provider_key = provider_key ? idm_strdup(provider_key) : NULL;
    dst->kind = src->kind;
    dst->order = artifact->rule_order++;
    dst->literal_index = SIZE_MAX;
    dst->terminal.kind = src->terminal.kind;
    dst->terminal.flags = src->terminal.flags;
    dst->terminal.text = src->terminal.text ? idm_strdup(src->terminal.text) : NULL;
    if (!dst->name || (provider && !dst->provider) || (provider_key && !dst->provider_key) || (src->terminal.text && !dst->terminal.text) ||
        (binding_scopes && !idm_scope_set_copy(&dst->binding_scopes, binding_scopes))) {
        reader_artifact_rule_destroy(dst);
        (*count)--;
        return idm_error_oom(err, span);
    }
    if (src->kind == (uint8_t)IDM_GRAMMAR_RULE_FORM) {
        if (!idm_reader_program_copy(&dst->pattern, &src->pattern, err, span)) {
            reader_artifact_rule_destroy(dst);
            (*count)--;
            return false;
        }
    }
    if (src->kind != (uint8_t)IDM_GRAMMAR_RULE_SKIP && !idm_reader_program_copy(&dst->constructor, &src->constructor, err, span)) {
        reader_artifact_rule_destroy(dst);
        (*count)--;
        return false;
    }
    return true;
}

bool idm_reader_artifact_from_sources(const char *surface, const IdmReaderGrammarSource *sources, size_t source_count, IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    if (!surface || !*surface) return idm_error_set(err, idm_span_unknown(NULL), "reader artifact requires a surface name");
    IdmReaderArtifact *artifact = calloc(1u, sizeof(*artifact));
    if (!artifact) return idm_error_oom(err, idm_span_unknown(NULL));
    artifact->surface = idm_strdup(surface);
    artifact->phase = source_count == 0 ? 0 : sources[0].phase;
    if (!artifact->surface) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    bool seen = false;
    bool exclusive = false;
    size_t pair_total = 0;
    const IdmScopeSet *surface_scopes = NULL;
    for (size_t i = 0; i < source_count; i++) {
        const IdmReaderGrammarSource *source = &sources[i];
        const IdmPkgGrammar *grammar = source->grammar;
        if (!grammar) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact source is missing grammar");
        }
        if (!grammar->name || strcmp(grammar->name, surface) != 0) continue;
        if (source->phase != artifact->phase) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has contributors from multiple phases", surface);
        }
        if (!surface_scopes) surface_scopes = source->binding_scopes;
        else if (!reader_artifact_scope_source_equal(surface_scopes, source->binding_scopes)) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has contributors from multiple binding scopes", surface);
        }
        if (grammar->rule_count == 0 || !grammar->rules) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' contributor has no rules", surface);
        }
        if (seen && (exclusive || grammar->mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE)) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has conflicting exclusive grammar contributors", surface);
        }
        if (grammar->mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE) exclusive = true;
        for (size_t r = 0; r < grammar->rule_count; r++) {
            if (!idm_grammar_rule_validate(&grammar->rules[r], err, idm_span_unknown(NULL)) ||
                !reader_artifact_add_rule(artifact, &grammar->rules[r], source->provider, source->provider_key, source->binding_scopes, err, idm_span_unknown(NULL))) {
                idm_reader_artifact_destroy(artifact);
                return false;
            }
        }
        pair_total += grammar->pair_count;
        seen = true;
    }
    if (!seen) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no grammar contributors", surface);
    }
    if (artifact->token_count == 0) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no terminals", surface);
    }
    if (artifact->form_count == 0) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no reductions", surface);
    }
    const ReaderArtifactRule *start_rule = reader_artifact_find_form_rule(artifact, IDM_READER_START_RULE);
    if (!start_rule || !reader_artifact_form_rule_index(artifact, start_rule, &artifact->start_form)) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' has no '%s' start rule", surface, IDM_READER_START_RULE);
    }
    if (start_rule->constructor.count == 0 ||
        start_rule->constructor.items[0].op != (uint8_t)IDM_READER_CTOR_FORM ||
        !start_rule->constructor.items[0].text ||
        strcmp(start_rule->constructor.items[0].text, "%-package-begin") != 0) {
        idm_reader_artifact_destroy(artifact);
        return idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' program rule constructor must emit %%-package-begin", surface);
    }
    if (pair_total != 0) {
        artifact->pairs = calloc(pair_total, sizeof(*artifact->pairs));
        if (!artifact->pairs) {
            idm_reader_artifact_destroy(artifact);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        for (size_t i = 0; i < source_count; i++) {
            const IdmPkgGrammar *grammar = sources[i].grammar;
            if (!grammar || !grammar->name || strcmp(grammar->name, surface) != 0) continue;
            for (size_t p = 0; p < grammar->pair_count; p++) {
                const IdmGrammarPair *pair = &grammar->pairs[p];
                ReaderArtifactPair *dst = &artifact->pairs[artifact->pair_count];
                const ReaderArtifactRule *open_rule = reader_artifact_find_token_rule(artifact, pair->open);
                const ReaderArtifactRule *close_rule = reader_artifact_find_token_rule(artifact, pair->close);
                if (!open_rule || !reader_artifact_token_rule_index(artifact, open_rule, &dst->open) ||
                    !close_rule || !reader_artifact_token_rule_index(artifact, close_rule, &dst->close)) {
                    bool reported = idm_error_set(err, idm_span_unknown(NULL), "reader artifact surface '%s' pair references unknown token '%s'", surface, open_rule ? pair->close : pair->open);
                    idm_reader_artifact_destroy(artifact);
                    return reported;
                }
                artifact->pair_count++;
            }
        }
    }
    if (!reader_artifact_resolve_rules(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_compile_reduction_tables(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_validate_reductions(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_compile_terminal_program(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_resolve_capture_slots(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_index_literals(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_compile_start_facts(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    if (!reader_artifact_precompute_literal_token_indices(artifact, err)) {
        idm_reader_artifact_destroy(artifact);
        return false;
    }
    *out = artifact;
    return true;
}

bool idm_reader_artifact_from_grammars(const char *surface, const IdmPkgGrammar *grammars, size_t grammar_count, IdmReaderArtifact **out, IdmError *err) {
    *out = NULL;
    IdmReaderGrammarSource *sources = NULL;
    if (grammar_count != 0) {
        sources = calloc(grammar_count, sizeof(*sources));
        if (!sources) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    for (size_t i = 0; i < grammar_count; i++) sources[i].grammar = &grammars[i];
    bool ok = idm_reader_artifact_from_sources(surface, sources, grammar_count, out, err);
    free(sources);
    return ok;
}

static void reader_start_position(const char *source, size_t offset, size_t *pos, unsigned *line, unsigned *column) {
    size_t len = strlen(source);
    if (offset > len) offset = len;
    *pos = offset;
    *line = 1;
    *column = 1;
    if (offset == 0) {
        if (source[0] == '#' && source[1] == '!') {
            size_t i = 2;
            while (source[i] != '\0' && source[i] != '\n') i++;
            *pos = source[i] == '\n' ? i + 1u : i;
            *line = 2;
        }
        return;
    }
    reader_artifact_advance_position(source, 0, offset, line, column);
}

bool idm_reader_read_artifact_string(const IdmReaderArtifact *artifact, const char *file, const char *source, size_t offset, IdmSyntax **out, bool *out_incomplete, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.artifact.read_string");
    *out = NULL;
    if (out_incomplete) *out_incomplete = false;
    if (!artifact) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(file), "reader artifact is required");
    }
    ReaderArtifactTokenVec tokens = {0};
    size_t start_pos = 0;
    unsigned start_line = 1;
    unsigned start_column = 1;
    reader_start_position(source, offset, &start_pos, &start_line, &start_column);
    if (!reader_artifact_lex(artifact, file, source, strlen(source), start_pos, start_line, start_column, &tokens, out_incomplete, err)) {
        reader_artifact_tokens_destroy(&tokens);
        idm_profile_scope_end(&prof);
        return false;
    }
    idm_profile_count("reader.artifact.tokens", (uint64_t)tokens.count);
    bool ok = reader_artifact_read_program(artifact, file, &tokens, out, out_incomplete, err);
    if (ok && out_incomplete && reader_artifact_pairs_open(artifact, &tokens)) *out_incomplete = true;
    reader_artifact_tokens_destroy(&tokens);
    idm_profile_scope_end(&prof);
    return ok;
}

bool idm_reader_read_terms_string(const char *file, const char *source, size_t offset, IdmSyntax **out, bool *out_incomplete, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.terms.read_string");
    if (out_incomplete) *out_incomplete = false;
    TokenVec tokens = {0};
    size_t start_pos = 0;
    unsigned start_line = 1;
    unsigned start_column = 1;
    reader_start_position(source, offset, &start_pos, &start_line, &start_column);
    if (!lex_transparent_from(file, source, strlen(source), start_pos, start_line, start_column, &tokens, out_incomplete, err)) {
        tokens_destroy(&tokens);
        idm_profile_scope_end(&prof);
        return false;
    }
    idm_profile_count("reader.terms.tokens", (uint64_t)tokens.count);
    Parser p = {tokens.items, tokens.count, 0, file, err, out_incomplete};
    IdmSyntax *program = parse_transparent_program(&p);
    tokens_destroy(&tokens);
    if (!program) {
        idm_profile_scope_end(&prof);
        return false;
    }
    *out = program;
    idm_profile_scope_end(&prof);
    return true;
}

bool idm_reader_read_terms_file(const char *path, IdmSyntax **out, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "reader.terms.read_file");
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, err)) {
        idm_profile_scope_end(&prof);
        return false;
    }
    (void)len;
    bool ok = idm_reader_read_terms_string(path, source, 0, out, NULL, err);
    free(source);
    idm_profile_scope_end(&prof);
    return ok;
}
