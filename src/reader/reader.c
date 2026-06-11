#include "ish/reader.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_SEMI,
    TOK_IDENT,
    TOK_ATOM,
    TOK_KEYWORD,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_STRING_INTERP,
    TOK_SHELL_WORD,
    TOK_SHELL_VAR,
    TOK_FN_VALUE,
    TOK_OP,
    TOK_DOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_PERCENT_LBRACKET,
    TOK_PERCENT_LBRACE,
    TOK_QUOTE,
    TOK_QUASIQUOTE,
    TOK_COMMA,
    TOK_COMMA_AT,
    TOK_PERCENT_QUOTE,
    TOK_PERCENT_QUASIQUOTE,
    TOK_PERCENT_COMMA,
    TOK_PERCENT_COMMA_AT,
    TOK_HEREDOC
} TokenKind;

typedef struct {
    TokenKind kind;
    char *lexeme;
    IshSpan span;
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
} Lexer;

typedef struct {
    Token *tokens;
    size_t count;
    size_t pos;
    const char *file;
    IshError *err;
} Parser;

static void tokens_destroy(TokenVec *vec) {
    for (size_t i = 0; i < vec->count; i++) free(vec->items[i].lexeme);
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->cap = 0;
}

static bool tokens_push(TokenVec *vec, Token tok) {
    if (vec->count == vec->cap) {
        size_t cap = vec->cap ? vec->cap * 2u : 64u;
        Token *items = realloc(vec->items, cap * sizeof(*items));
        if (!items) return false;
        vec->items = items;
        vec->cap = cap;
    }
    vec->items[vec->count++] = tok;
    return true;
}

static char peek(Lexer *lx) {
    return lx->pos < lx->len ? lx->src[lx->pos] : '\0';
}

static char peek_n(Lexer *lx, size_t n) {
    return lx->pos + n < lx->len ? lx->src[lx->pos + n] : '\0';
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

static bool is_ident_start(char ch) {
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_ident_part(char ch) {
    return isalnum((unsigned char)ch) || ch == '_' || ch == '?' || ch == '!' || ch == '/' || ch == '-';
}

static bool is_operator_char(char ch) {
    return ch == '>' || ch == '<' || ch == '=' || ch == '|' || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '!' || ch == '&';
}

static bool is_delim(char ch) {
    return ch == '\0' || isspace((unsigned char)ch) || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == ';' || ch == '"' || ch == '\'' || ch == '`' || ch == ',';
}

static bool add_token(TokenVec *vec, Lexer *lx, TokenKind kind, size_t start, unsigned line, unsigned column, bool leading_space) {
    Token tok;
    tok.kind = kind;
    tok.lexeme = ish_strndup(lx->src + start, lx->pos - start);
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

static bool read_string_token(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space, IshError *err) {
    (void)advance(lx);
    IshBuffer decoded;
    ish_buf_init(&decoded);
    bool interpolation_seen = false;
    while (peek(lx) != '\0' && peek(lx) != '"') {
        char ch = peek(lx);
        if ((ch == '$' && (peek_n(lx, 1) == '{' || peek_n(lx, 1) == '(')) || (ch == '#' && peek_n(lx, 1) == '{')) {
            interpolation_seen = true;
            char open = peek_n(lx, 1);
            char close = open == '{' ? '}' : ')';
            if (!ish_buf_append_char(&decoded, ch) || !ish_buf_append_char(&decoded, open)) {
                ish_buf_destroy(&decoded);
                return ish_error_oom(err, (IshSpan){lx->file, start, lx->pos, line, column});
            }
            advance(lx);
            advance(lx);
            int depth = 1;
            while (peek(lx) != '\0' && depth > 0) {
                char c = advance(lx);
                if (c == open) depth++;
                else if (c == close) depth--;
                if (!ish_buf_append_char(&decoded, c)) {
                    ish_buf_destroy(&decoded);
                    return ish_error_oom(err, (IshSpan){lx->file, start, lx->pos, line, column});
                }
            }
            if (depth != 0) {
                ish_buf_destroy(&decoded);
                return ish_error_set(err, (IshSpan){lx->file, start, lx->pos, line, column}, "unterminated string interpolation");
            }
            continue;
        }
        advance(lx);
        if (ch == '\\') {
            char e = advance(lx);
            switch (e) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                case '$': ch = '$'; break;
                case '#': ch = '#'; break;
                default:
                    ish_buf_destroy(&decoded);
                    return ish_error_set(err, (IshSpan){lx->file, lx->pos - 1u, lx->pos, lx->line, lx->column - 1u}, "unknown string escape");
            }
        }
        if (!ish_buf_append_char(&decoded, ch)) {
            ish_buf_destroy(&decoded);
            return ish_error_oom(err, (IshSpan){lx->file, start, lx->pos, line, column});
        }
    }
    if (peek(lx) != '"') {
        ish_buf_destroy(&decoded);
        return ish_error_set(err, (IshSpan){lx->file, start, lx->pos, line, column}, "unterminated string");
    }
    advance(lx);
    Token tok;
    tok.kind = interpolation_seen ? TOK_STRING_INTERP : TOK_STRING;
    tok.lexeme = ish_buf_take(&decoded);
    if (!tok.lexeme) return ish_error_oom(err, (IshSpan){lx->file, start, lx->pos, line, column});
    tok.span = (IshSpan){lx->file, start, lx->pos, line, column};
    tok.leading_space = leading_space;
    tok.adjacent_previous = start == lx->previous_end;
    lx->previous_end = lx->pos;
    if (!tokens_push(vec, tok)) {
        free(tok.lexeme);
        return ish_error_oom(err, tok.span);
    }
    return true;
}

static bool read_shell_word(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space) {
    while (!is_delim(peek(lx))) {
        char ch = peek(lx);
        if (ch == '|' || ch == '>' || ch == '<' || ch == '=') break;
        advance(lx);
    }
    return add_token(vec, lx, TOK_SHELL_WORD, start, line, column, leading_space);
}

static bool capture_heredoc_body(Lexer *lx, const char *delim, bool strip, char **out_body, IshError *err) {
    size_t dlen = strlen(delim);
    IshBuffer body;
    ish_buf_init(&body);
    for (;;) {
        if (peek(lx) == '\0') { ish_buf_destroy(&body); return ish_error_set(err, (IshSpan){lx->file, lx->pos, lx->pos, lx->line, lx->column}, "unterminated heredoc (missing closing '%s')", delim); }
        size_t line_start = lx->pos;
        while (peek(lx) != '\0' && peek(lx) != '\n') advance(lx);
        size_t line_end = lx->pos;
        size_t off = 0;
        if (strip) while (line_start + off < line_end && lx->src[line_start + off] == '\t') off++;
        size_t content_len = line_end - line_start - off;
        bool is_delim = content_len == dlen && memcmp(lx->src + line_start + off, delim, dlen) == 0;
        bool had_newline = peek(lx) == '\n';
        if (had_newline) advance(lx);
        if (is_delim) break;
        if (!ish_buf_append_n(&body, lx->src + line_start + off, content_len) || !ish_buf_append_char(&body, '\n')) { ish_buf_destroy(&body); return ish_error_oom(err, (IshSpan){lx->file, line_start, line_end, lx->line, lx->column}); }
    }
    char *taken = ish_buf_take(&body);
    *out_body = taken ? taken : ish_strdup("");
    return *out_body != NULL;
}

static bool lex_source(const char *file, const char *source, size_t len, TokenVec *out, IshError *err) {
    Lexer lx = {file, source, len, 0, 1, 1, 0};
    bool leading_space = false;
    struct { size_t index; char *delim; bool strip; } pending_heredoc[8];
    size_t pending_heredoc_count = 0;
    while (peek(&lx) != '\0') {
        char ch = peek(&lx);
        if (ch == ' ' || ch == '\t' || ch == '\r') {
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
        if (ch == '\n') {
            advance(&lx);
            if (!add_token(out, &lx, TOK_NEWLINE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            for (size_t h = 0; h < pending_heredoc_count; h++) {
                char *bodytext = NULL;
                if (!capture_heredoc_body(&lx, pending_heredoc[h].delim, pending_heredoc[h].strip, &bodytext, err)) {
                    for (size_t j = h; j < pending_heredoc_count; j++) free(pending_heredoc[j].delim);
                    return false;
                }
                free(out->items[pending_heredoc[h].index].lexeme);
                out->items[pending_heredoc[h].index].lexeme = bodytext;
                free(pending_heredoc[h].delim);
            }
            pending_heredoc_count = 0;
            lx.previous_end = lx.pos;
            leading_space = true;
            continue;
        }
        if (ch == '<' && peek_n(&lx, 1) == '<' && peek_n(&lx, 2) != '<') {
            advance(&lx); advance(&lx);
            bool strip = false;
            if (peek(&lx) == '-') { strip = true; advance(&lx); }
            while (peek(&lx) == ' ' || peek(&lx) == '\t') advance(&lx);
            size_t dstart = lx.pos;
            while (peek(&lx) != '\0' && !isspace((unsigned char)peek(&lx))) advance(&lx);
            if (lx.pos == dstart) return ish_error_set(err, (IshSpan){file, start, lx.pos, line, column}, "heredoc requires a delimiter after <<");
            char *delim = ish_strndup(lx.src + dstart, lx.pos - dstart);
            if (!delim) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            if (pending_heredoc_count >= 8) { free(delim); return ish_error_set(err, (IshSpan){file, start, lx.pos, line, column}, "too many heredocs on one line"); }
            if (!add_token(out, &lx, TOK_HEREDOC, start, line, column, leading_space)) { free(delim); return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); }
            pending_heredoc[pending_heredoc_count].index = out->count - 1u;
            pending_heredoc[pending_heredoc_count].delim = delim;
            pending_heredoc[pending_heredoc_count].strip = strip;
            pending_heredoc_count++;
            leading_space = false;
            continue;
        }
        if (ch == ';') { advance(&lx); if (!add_token(out, &lx, TOK_SEMI, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '(') { advance(&lx); if (!add_token(out, &lx, TOK_LPAREN, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ')') { advance(&lx); if (!add_token(out, &lx, TOK_RPAREN, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '[') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACKET, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ']') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACKET, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '{') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '}') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '"') {
            if (!read_string_token(out, &lx, start, line, column, leading_space, err)) return false;
            leading_space = false;
            continue;
        }
        if (ch == '\'') { advance(&lx); if (!add_token(out, &lx, TOK_QUOTE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '`') { advance(&lx); if (!add_token(out, &lx, TOK_QUASIQUOTE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ',' && peek_n(&lx, 1) == '@') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_COMMA_AT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ',') { advance(&lx); if (!add_token(out, &lx, TOK_COMMA, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '[') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_LBRACKET, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '{') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_LBRACE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '\'') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_QUOTE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '`') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_QUASIQUOTE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == ',' && peek_n(&lx, 2) == '@') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_COMMA_AT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == ',') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_COMMA, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ':' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (is_ident_part(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_ATOM, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '$' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (is_ident_part(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_SHELL_VAR, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '&' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (is_ident_part(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_FN_VALUE, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (isdigit((unsigned char)ch)) {
            while (isdigit((unsigned char)peek(&lx))) advance(&lx);
            bool is_float = false;
            if (peek(&lx) == '.' && isdigit((unsigned char)peek_n(&lx, 1))) {
                is_float = true;
                advance(&lx);
                while (isdigit((unsigned char)peek(&lx))) advance(&lx);
            }
            if (peek(&lx) == 'e' || peek(&lx) == 'E') {
                char e1 = peek_n(&lx, 1);
                if (isdigit((unsigned char)e1) || ((e1 == '+' || e1 == '-') && isdigit((unsigned char)peek_n(&lx, 2)))) {
                    is_float = true;
                    advance(&lx);
                    if (peek(&lx) == '+' || peek(&lx) == '-') advance(&lx);
                    while (isdigit((unsigned char)peek(&lx))) advance(&lx);
                }
            }
            if (!add_token(out, &lx, is_float ? TOK_FLOAT : TOK_INT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (is_ident_start(ch)) {
            advance(&lx);
            while (true) {
                while (is_ident_part(peek(&lx))) advance(&lx);
                if (lx.pos > start && lx.src[lx.pos - 1u] == '-' && peek(&lx) == '>' && is_ident_start(peek_n(&lx, 1))) {
                    advance(&lx);
                    continue;
                }
                break;
            }
            if (peek(&lx) == '=' && peek_n(&lx, 1) == '?') {
                advance(&lx);
                advance(&lx);
            }
            if (peek(&lx) == ':' && peek_n(&lx, 1) != ':') {
                advance(&lx);
                if (!add_token(out, &lx, TOK_KEYWORD, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
                leading_space = false;
                continue;
            }
            if (!add_token(out, &lx, TOK_IDENT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '.' && peek_n(&lx, 1) == '.' && peek_n(&lx, 2) == '.') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_IDENT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '.') { advance(&lx); if (!add_token(out, &lx, TOK_DOT, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '*' && peek_n(&lx, 1) == '*') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if ((ch == '-' && !isdigit((unsigned char)peek_n(&lx, 1)) && peek_n(&lx, 1) != '>' && !isspace((unsigned char)peek_n(&lx, 1))) ||
            ch == '~' ||
            ((ch == '/' || ch == '+' || ch == '*') && !is_delim(peek_n(&lx, 1)))) {
            if (!read_shell_word(out, &lx, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '-' && peek_n(&lx, 1) == '>') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '|' && peek_n(&lx, 1) == '>' && peek_n(&lx, 2) == '>') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (is_operator_char(ch)) {
            advance(&lx);
            while (is_operator_char(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return ish_error_oom(err, (IshSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        return ish_error_set(err, (IshSpan){file, start, start + 1u, line, column}, "unexpected character '%c'", ch);
    }
    Token eof;
    eof.kind = TOK_EOF;
    eof.lexeme = ish_strdup("");
    if (!eof.lexeme) return ish_error_oom(err, (IshSpan){file, lx.pos, lx.pos, lx.line, lx.column});
    eof.span = (IshSpan){file, lx.pos, lx.pos, lx.line, lx.column};
    eof.leading_space = leading_space;
    eof.adjacent_previous = lx.pos == lx.previous_end;
    return tokens_push(out, eof) || ish_error_oom(err, eof.span);
}

static Token *cur(Parser *p) { return &p->tokens[p->pos]; }
static bool at(Parser *p, TokenKind kind) { return cur(p)->kind == kind; }
static Token *take(Parser *p) { return &p->tokens[p->pos++]; }
static void skip_separators(Parser *p) { while (at(p, TOK_NEWLINE) || at(p, TOK_SEMI)) p->pos++; }

static bool is_stop(Parser *p, TokenKind a, TokenKind b, TokenKind c, bool separators_stop, bool end_stops, bool else_stops) {
    TokenKind k = cur(p)->kind;
    if (end_stops && k == TOK_IDENT && strcmp(cur(p)->lexeme, "end") == 0) return true;
    if (else_stops && k == TOK_IDENT && strcmp(cur(p)->lexeme, "else") == 0) return true;
    if (separators_stop && (k == TOK_NEWLINE || k == TOK_SEMI)) return true;
    return k == TOK_EOF || k == a || k == b || k == c;
}

static bool indented_newline_continues(Parser *p, unsigned expr_column, TokenKind a, TokenKind b, TokenKind c, bool end_stops, bool else_stops) {
    if (!at(p, TOK_NEWLINE)) return false;
    size_t i = p->pos;
    while (i < p->count && p->tokens[i].kind == TOK_NEWLINE) i++;
    if (i >= p->count) return false;
    TokenKind k = p->tokens[i].kind;
    if (k == TOK_EOF || k == TOK_SEMI || k == a || k == b || k == c) return false;
    if (end_stops && k == TOK_IDENT && strcmp(p->tokens[i].lexeme, "end") == 0) return false;
    if (else_stops && k == TOK_IDENT && strcmp(p->tokens[i].lexeme, "else") == 0) return false;
    return p->tokens[i].span.column > expr_column;
}

static IshSyntax *protocol1(const char *head, IshSyntax *a, IshSpan span) {
    IshSyntax *list = ish_syn_list(ISH_SEQ_PAREN, span);
    if (!list || !ish_syn_append(list, ish_syn_word(head, span)) || !ish_syn_append(list, a)) {
        ish_syn_free(list);
        ish_syn_free(a);
        return NULL;
    }
    return list;
}


static IshSyntax *read_interp_inner(const char *file, const char *inner, bool command, IshSpan span, IshError *err) {
    IshSyntax *pkg = NULL;
    if (!ish_reader_read_string(file, inner, &pkg, err)) return NULL;
    if (!pkg || pkg->as.seq.count != 2) {
        ish_syn_free(pkg);
        ish_error_set(err, span, "string interpolation must contain a single expression");
        return NULL;
    }
    IshSyntax *form = ish_syn_clone(pkg->as.seq.items[1]);
    ish_syn_free(pkg);
    if (!form) { ish_error_oom(err, span); return NULL; }
    if (command) {
        IshSyntax *wrapped = protocol1("%-command-sub", form, span);
        if (!wrapped) { ish_error_oom(err, span); return NULL; }
        return wrapped;
    }
    return form;
}

static IshSyntax *parse_string_interp(Parser *p, const char *body, IshSpan span) {
    IshSyntax *result = ish_syn_list(ISH_SEQ_PAREN, span);
    if (!result || !ish_syn_append(result, ish_syn_word("%-string", span))) {
        ish_syn_free(result);
        ish_error_oom(p->err, span);
        return NULL;
    }
    IshBuffer chunk;
    ish_buf_init(&chunk);
    size_t i = 0;
    while (body[i] != '\0') {
        char ch = body[i];
        bool command = ch == '$' && body[i + 1u] == '(';
        bool interp = command || (ch == '$' && body[i + 1u] == '{') || (ch == '#' && body[i + 1u] == '{');
        if (interp) {
            if (chunk.len > 0) {
                IshSyntax *lit = ish_syn_string_n(chunk.data, chunk.len, span);
                if (!lit || !ish_syn_append(result, lit)) { ish_syn_free(lit); goto fail; }
                ish_buf_destroy(&chunk);
                ish_buf_init(&chunk);
            }
            char open = command ? '(' : '{';
            char close = command ? ')' : '}';
            size_t j = i + 2u;
            int depth = 1;
            while (body[j] != '\0' && depth > 0) {
                if (body[j] == open) depth++;
                else if (body[j] == close) { depth--; if (depth == 0) break; }
                j++;
            }
            char *inner = ish_strndup(body + i + 2u, j - (i + 2u));
            if (!inner) { ish_error_oom(p->err, span); goto fail; }
            IshSyntax *part = read_interp_inner(p->file, inner, command, span, p->err);
            free(inner);
            if (!part || !ish_syn_append(result, part)) { ish_syn_free(part); goto fail; }
            i = body[j] == '\0' ? j : j + 1u;
        } else {
            if (!ish_buf_append_char(&chunk, ch)) { ish_error_oom(p->err, span); goto fail; }
            i++;
        }
    }
    if (chunk.len > 0) {
        IshSyntax *lit = ish_syn_string_n(chunk.data, chunk.len, span);
        if (!lit || !ish_syn_append(result, lit)) { ish_syn_free(lit); goto fail; }
    }
    ish_buf_destroy(&chunk);
    return result;
fail:
    ish_buf_destroy(&chunk);
    ish_syn_free(result);
    return NULL;
}

static IshSyntax *parse_expr(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IshSyntax *parse_expr_delimited(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IshSyntax *parse_expr_body(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IshSyntax *parse_primary(Parser *p);

static bool ident_is(Parser *p, const char *text) {
    return cur(p)->kind == TOK_IDENT && strcmp(cur(p)->lexeme, text) == 0;
}





static IshSyntax *parse_container(Parser *p, IshSyntaxKind kind, TokenKind close, IshSpan span) {
    IshSyntax *seq = NULL;
    if (kind == ISH_SYN_LIST) seq = ish_syn_list(ISH_SEQ_BRACKET, span);
    else if (kind == ISH_SYN_VECTOR) seq = ish_syn_vector(span);
    else if (kind == ISH_SYN_TUPLE) seq = ish_syn_tuple(span);
    else if (kind == ISH_SYN_DICT) seq = ish_syn_dict(span);
    if (!seq) return NULL;
    skip_separators(p);
    while (!at(p, close) && !at(p, TOK_EOF)) {
        IshSyntax *item = parse_primary(p);
        if (!item || !ish_syn_append(seq, item)) {
            ish_syn_free(seq);
            ish_syn_free(item);
            return NULL;
        }
        skip_separators(p);
    }
    if (!at(p, close)) {
        ish_syn_free(seq);
        ish_error_set(p->err, cur(p)->span, "unterminated container");
        return NULL;
    }
    take(p);
    return seq;
}

static IshSyntax *parse_body(Parser *p, IshSpan span) {
    IshSyntax *body = ish_syn_list(ISH_SEQ_PAREN, span);
    if (!body || !ish_syn_append(body, ish_syn_word("%-body", span))) {
        ish_syn_free(body);
        return NULL;
    }
    skip_separators(p);
    while (!at(p, TOK_EOF) && !ident_is(p, "end") && !ident_is(p, "else")) {
        IshSyntax *form = parse_expr_body(p, TOK_EOF, TOK_EOF, TOK_EOF);
        if (!form || !ish_syn_append(body, form)) {
            ish_syn_free(form);
            ish_syn_free(body);
            return NULL;
        }
        skip_separators(p);
    }
    if (ident_is(p, "else")) return body;
    if (!ident_is(p, "end")) {
        ish_syn_free(body);
        ish_error_set(p->err, span, "unterminated do/end body");
        return NULL;
    }
    take(p);
    return body;
}

static IshSyntax *parse_primary(Parser *p) {
    Token *tok = cur(p);
    switch (tok->kind) {
        case TOK_IDENT: {
            take(p);
            if (strcmp(tok->lexeme, "do") == 0) return parse_body(p, tok->span);
            if (strcmp(tok->lexeme, "end") == 0) {
                ish_error_set(p->err, tok->span, "unexpected end");
                return NULL;
            }
            IshSyntax *syn = ish_syn_word(tok->lexeme, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_OP:
        case TOK_DOT: {
            if (tok->kind == TOK_OP && (strcmp(tok->lexeme, "<") == 0 || strcmp(tok->lexeme, ">") == 0) &&
                p->pos + 1u < p->count && p->tokens[p->pos + 1u].kind == TOK_LPAREN && p->tokens[p->pos + 1u].adjacent_previous) {
                bool write = strcmp(tok->lexeme, ">") == 0;
                take(p);
                take(p);
                IshSyntax *inner = parse_expr_delimited(p, TOK_RPAREN, TOK_EOF, TOK_EOF);
                if (!inner) return NULL;
                if (!at(p, TOK_RPAREN)) { ish_syn_free(inner); ish_error_set(p->err, tok->span, "unterminated process substitution"); return NULL; }
                take(p);
                return protocol1(write ? "%-procsub-write" : "%-procsub-read", inner, tok->span);
            }
            take(p);
            IshSyntax *syn = ish_syn_word(tok->lexeme, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_ATOM: {
            take(p);
            IshSyntax *syn = strcmp(tok->lexeme, ":nil") == 0 ? ish_syn_nil(tok->span) : ish_syn_atom(tok->lexeme + 1, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_KEYWORD: {
            take(p);
            size_t len = strlen(tok->lexeme);
            char *name = ish_strndup(tok->lexeme, len > 0 ? len - 1u : 0);
            if (!name) { ish_error_oom(p->err, tok->span); return NULL; }
            IshSyntax *syn = ish_syn_atom(name, tok->span);
            free(name);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_INT: {
            take(p);
            errno = 0;
            long long value = strtoll(tok->lexeme, NULL, 10);
            if (errno != 0) {
                ish_error_set(p->err, tok->span, "invalid integer literal");
                return NULL;
            }
            IshSyntax *syn = ish_syn_int(value, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_FLOAT: {
            take(p);
            errno = 0;
            double value = strtod(tok->lexeme, NULL);
            if (errno != 0) {
                ish_error_set(p->err, tok->span, "invalid float literal");
                return NULL;
            }
            IshSyntax *syn = ish_syn_float(value, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_STRING: {
            take(p);
            IshSyntax *syn = ish_syn_string(tok->lexeme, tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_STRING_INTERP: {
            take(p);
            return parse_string_interp(p, tok->lexeme, tok->span);
        }
        case TOK_SHELL_WORD: {
            take(p);
            IshSyntax *syn = protocol1("%-word", ish_syn_string(tok->lexeme, tok->span), tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_HEREDOC: {
            take(p);
            return protocol1("%-heredoc", ish_syn_string(tok->lexeme, tok->span), tok->span);
        }
        case TOK_SHELL_VAR: {
            take(p);
            IshSyntax *syn = protocol1("%-shell-var", ish_syn_word(tok->lexeme + 1, tok->span), tok->span);
            if (syn) ish_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_FN_VALUE: {
            take(p);
            return protocol1("%-expression", ish_syn_word(tok->lexeme, tok->span), tok->span);
        }
        case TOK_LPAREN: {
            take(p);
            IshSyntax *inner = parse_expr_delimited(p, TOK_RPAREN, TOK_EOF, TOK_EOF);
            if (!inner) return NULL;
            if (!at(p, TOK_RPAREN)) {
                ish_syn_free(inner);
                ish_error_set(p->err, tok->span, "unterminated group");
                return NULL;
            }
            take(p);
            return protocol1("%-group", inner, tok->span);
        }
        case TOK_LBRACKET: take(p); return parse_container(p, ISH_SYN_LIST, TOK_RBRACKET, tok->span);
        case TOK_PERCENT_LBRACKET: take(p); return parse_container(p, ISH_SYN_VECTOR, TOK_RBRACKET, tok->span);
        case TOK_LBRACE: take(p); return parse_container(p, ISH_SYN_TUPLE, TOK_RBRACE, tok->span);
        case TOK_PERCENT_LBRACE: take(p); return parse_container(p, ISH_SYN_DICT, TOK_RBRACE, tok->span);
        case TOK_QUOTE: take(p); return protocol1("%-quote", parse_primary(p), tok->span);
        case TOK_QUASIQUOTE: take(p); return protocol1("%-quasiquote", parse_primary(p), tok->span);
        case TOK_COMMA: take(p); return protocol1("%-unquote", parse_primary(p), tok->span);
        case TOK_COMMA_AT: take(p); return protocol1("%-unquote-splicing", parse_primary(p), tok->span);
        case TOK_PERCENT_QUOTE: take(p); return protocol1("%-syntax", parse_primary(p), tok->span);
        case TOK_PERCENT_QUASIQUOTE: take(p); return protocol1("%-quasisyntax", parse_primary(p), tok->span);
        case TOK_PERCENT_COMMA: take(p); return protocol1("%-unsyntax", parse_primary(p), tok->span);
        case TOK_PERCENT_COMMA_AT: take(p); return protocol1("%-unsyntax-splicing", parse_primary(p), tok->span);
        default:
            ish_error_set(p->err, tok->span, "expected syntax item");
            return NULL;
    }
}

static IshSyntax *parse_expr_impl(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3, bool separators_stop, bool end_stops, bool else_stops) {
    IshSpan span = cur(p)->span;
    IshSyntax *expr = ish_syn_list(ISH_SEQ_PAREN, span);
    if (!expr || !ish_syn_append(expr, ish_syn_word("%-expr", span))) {
        ish_syn_free(expr);
        return NULL;
    }
    while (true) {
        if (separators_stop && at(p, TOK_SEMI)) break;
        if (separators_stop && at(p, TOK_NEWLINE)) {
            if (!indented_newline_continues(p, span.column, end1, end2, end3, end_stops, else_stops)) break;
            skip_separators(p);
            continue;
        }
        if (is_stop(p, end1, end2, end3, false, end_stops, else_stops)) break;
        if (!separators_stop && (at(p, TOK_NEWLINE) || at(p, TOK_SEMI))) {
            skip_separators(p);
            continue;
        }
        IshSyntax *part = parse_primary(p);
        if (!part || !ish_syn_append(expr, part)) {
            ish_syn_free(part);
            ish_syn_free(expr);
            return NULL;
        }
    }
    if (expr->as.seq.count == 1) {
        ish_error_set(p->err, span, "expected expression");
        ish_syn_free(expr);
        return NULL;
    }
    return expr;
}

static IshSyntax *parse_expr(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    return parse_expr_impl(p, end1, end2, end3, true, true, false);
}

static IshSyntax *parse_expr_delimited(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    skip_separators(p);
    return parse_expr_impl(p, end1, end2, end3, false, false, false);
}

static IshSyntax *parse_expr_body(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    return parse_expr_impl(p, end1, end2, end3, true, true, true);
}

static IshSyntax *parse_program(Parser *p) {
    IshSyntax *program = ish_syn_list(ISH_SEQ_PAREN, ish_span_unknown(p->file));
    if (!program || !ish_syn_append(program, ish_syn_word("%-package-begin", ish_span_unknown(p->file)))) {
        ish_syn_free(program);
        return NULL;
    }
    skip_separators(p);
    while (!at(p, TOK_EOF)) {
        IshSyntax *form = parse_expr(p, TOK_EOF, TOK_EOF, TOK_EOF);
        if (!form || !ish_syn_append(program, form)) {
            ish_syn_free(form);
            ish_syn_free(program);
            return NULL;
        }
        skip_separators(p);
    }
    return program;
}

bool ish_reader_read_string(const char *file, const char *source, IshSyntax **out, IshError *err) {
    TokenVec tokens = {0};
    if (!lex_source(file, source, strlen(source), &tokens, err)) {
        tokens_destroy(&tokens);
        return false;
    }
    Parser p = {tokens.items, tokens.count, 0, file, err};
    IshSyntax *program = parse_program(&p);
    tokens_destroy(&tokens);
    if (!program) return false;
    *out = program;
    return true;
}

bool ish_reader_read_file(const char *path, IshSyntax **out, IshError *err) {
    char *source = NULL;
    size_t len = 0;
    if (!ish_read_file(path, &source, &len, err)) return false;
    (void)len;
    bool ok = ish_reader_read_string(path, source, out, err);
    free(source);
    return ok;
}
