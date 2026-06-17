#include "idiom/reader.h"

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
    TOK_REGEX,
    TOK_SHELL_WORD,
    TOK_SHELL_VAR,
    TOK_FN_VALUE,
    TOK_PIN,
    TOK_OP,
    TOK_DOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_PERCENT_LBRACE,
    TOK_QUOTE,
    TOK_QUASIQUOTE,
    TOK_COMMA,
    TOK_COMMA_AT,
    TOK_PERCENT_QUOTE,
    TOK_PERCENT_QUASIQUOTE,
    TOK_PERCENT_COMMA,
    TOK_PERCENT_COMMA_AT,
    TOK_DOLLAR_LPAREN,
    TOK_HEREDOC
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
} Lexer;

typedef struct {
    Token *tokens;
    size_t count;
    size_t pos;
    const char *file;
    IdmError *err;
    bool body_paused_on_else;
    unsigned depth;
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

static bool is_wordish(TokenKind kind) {
    switch (kind) {
        case TOK_IDENT:
        case TOK_SHELL_WORD:
        case TOK_SHELL_VAR:
        case TOK_INT:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_STRING_INTERP:
        case TOK_REGEX:
            return true;
        default:
            return false;
    }
}

static bool previous_token_allows_negative_literal(const TokenVec *out) {
    if (out->count == 0) return true;
    switch (out->items[out->count - 1u].kind) {
        case TOK_NEWLINE:
        case TOK_SEMI:
        case TOK_LPAREN:
        case TOK_LBRACKET:
        case TOK_LBRACE:
        case TOK_PERCENT_LBRACE:
        case TOK_QUOTE:
        case TOK_QUASIQUOTE:
        case TOK_COMMA:
        case TOK_COMMA_AT:
        case TOK_PERCENT_QUOTE:
        case TOK_PERCENT_QUASIQUOTE:
        case TOK_PERCENT_COMMA:
        case TOK_PERCENT_COMMA_AT:
        case TOK_DOLLAR_LPAREN:
        case TOK_PIN:
            return true;
        default:
            return false;
    }
}

static bool continues_word(const Lexer *lx, const TokenVec *out) {
    return out->count > 0 && lx->pos == lx->previous_end && is_wordish(out->items[out->count - 1u].kind);
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

static bool read_string_token(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space, IdmError *err) {
    (void)advance(lx);
    IdmBuffer decoded;
    idm_buf_init(&decoded);
    bool interpolation_seen = false;
    while (peek(lx) != '\0' && peek(lx) != '"') {
        char ch = peek(lx);
        if ((ch == '$' && (peek_n(lx, 1) == '{' || peek_n(lx, 1) == '(')) || (ch == '#' && peek_n(lx, 1) == '{')) {
            interpolation_seen = true;
            char open = peek_n(lx, 1);
            char close = open == '{' ? '}' : ')';
            if (!idm_buf_append_char(&decoded, ch) || !idm_buf_append_char(&decoded, open)) {
                idm_buf_destroy(&decoded);
                return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
            }
            advance(lx);
            advance(lx);
            int depth = 1;
            while (peek(lx) != '\0' && depth > 0) {
                char c = advance(lx);
                if (c == open) depth++;
                else if (c == close) depth--;
                if (!idm_buf_append_char(&decoded, c)) {
                    idm_buf_destroy(&decoded);
                    return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
                }
            }
            if (depth != 0) {
                idm_buf_destroy(&decoded);
                return idm_error_set(err, (IdmSpan){lx->file, start, lx->pos, line, column}, "unterminated string interpolation");
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
        return idm_error_set(err, (IdmSpan){lx->file, start, lx->pos, line, column}, "unterminated string");
    }
    advance(lx);
    Token tok;
    tok.kind = interpolation_seen ? TOK_STRING_INTERP : TOK_STRING;
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

static bool read_regex_token(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space, IdmError *err) {
    (void)advance(lx);
    (void)advance(lx);
    IdmBuffer body;
    idm_buf_init(&body);
    while (peek(lx) != '\0' && peek(lx) != '"') {
        char ch = peek(lx);
        if (ch == '\\' && peek_n(lx, 1) == '"') {
            advance(lx);
            advance(lx);
            if (!idm_buf_append_char(&body, '"')) {
                idm_buf_destroy(&body);
                return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
            }
            continue;
        }
        advance(lx);
        if (!idm_buf_append_char(&body, ch)) {
            idm_buf_destroy(&body);
            return idm_error_oom(err, (IdmSpan){lx->file, start, lx->pos, line, column});
        }
    }
    if (peek(lx) != '"') {
        idm_buf_destroy(&body);
        return idm_error_set(err, (IdmSpan){lx->file, start, lx->pos, line, column}, "unterminated regex literal");
    }
    advance(lx);
    Token tok;
    tok.kind = TOK_REGEX;
    tok.lexeme = idm_buf_take(&body);
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

static bool read_shell_word(TokenVec *vec, Lexer *lx, size_t start, unsigned line, unsigned column, bool leading_space) {
    while (!is_delim(peek(lx))) {
        char ch = peek(lx);
        if (ch == '|' || ch == '>' || ch == '<' || ch == '=') break;
        advance(lx);
    }
    return add_token(vec, lx, TOK_SHELL_WORD, start, line, column, leading_space);
}

static bool capture_heredoc_body(Lexer *lx, const char *delim, bool strip, char **out_body, IdmError *err) {
    size_t dlen = strlen(delim);
    IdmBuffer body;
    idm_buf_init(&body);
    for (;;) {
        if (peek(lx) == '\0') { idm_buf_destroy(&body); return idm_error_set(err, (IdmSpan){lx->file, lx->pos, lx->pos, lx->line, lx->column}, "unterminated heredoc (missing closing '%s')", delim); }
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
        if (!idm_buf_append_n(&body, lx->src + line_start + off, content_len) || !idm_buf_append_char(&body, '\n')) { idm_buf_destroy(&body); return idm_error_oom(err, (IdmSpan){lx->file, line_start, line_end, lx->line, lx->column}); }
    }
    char *taken = idm_buf_take(&body);
    *out_body = taken ? taken : idm_strdup("");
    return *out_body != NULL;
}

static bool lex_source_from(const char *file, const char *source, size_t len, unsigned start_line, TokenVec *out, IdmError *err) {
    Lexer lx = {file, source, len, 0, start_line, 1, 0};
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
            if (!add_token(out, &lx, TOK_NEWLINE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
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
            if (lx.pos == dstart) return idm_error_set(err, (IdmSpan){file, start, lx.pos, line, column}, "heredoc requires a delimiter after <<");
            char *delim = idm_strndup(lx.src + dstart, lx.pos - dstart);
            if (!delim) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            if (pending_heredoc_count >= 8) { free(delim); return idm_error_set(err, (IdmSpan){file, start, lx.pos, line, column}, "too many heredocs on one line"); }
            if (!add_token(out, &lx, TOK_HEREDOC, start, line, column, leading_space)) { free(delim); return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); }
            pending_heredoc[pending_heredoc_count].index = out->count - 1u;
            pending_heredoc[pending_heredoc_count].delim = delim;
            pending_heredoc[pending_heredoc_count].strip = strip;
            pending_heredoc_count++;
            leading_space = false;
            continue;
        }
        if (ch == ';') { advance(&lx); if (!add_token(out, &lx, TOK_SEMI, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '(') { advance(&lx); if (!add_token(out, &lx, TOK_LPAREN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ')') { advance(&lx); if (!add_token(out, &lx, TOK_RPAREN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '[') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACKET, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ']') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACKET, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '{') { advance(&lx); if (!add_token(out, &lx, TOK_LBRACE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '}') { advance(&lx); if (!add_token(out, &lx, TOK_RBRACE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '"') {
            if (!read_string_token(out, &lx, start, line, column, leading_space, err)) return false;
            leading_space = false;
            continue;
        }
        if (ch == 'r' && peek_n(&lx, 1) == '"') {
            if (!read_regex_token(out, &lx, start, line, column, leading_space, err)) return false;
            leading_space = false;
            continue;
        }
        if (ch == '\'') { advance(&lx); if (!add_token(out, &lx, TOK_QUOTE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '`') { advance(&lx); if (!add_token(out, &lx, TOK_QUASIQUOTE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ',' && peek_n(&lx, 1) == '@') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_COMMA_AT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ',') { advance(&lx); if (!add_token(out, &lx, TOK_COMMA, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '{') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_LBRACE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '\'') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_QUOTE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == '`') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_QUASIQUOTE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == ',' && peek_n(&lx, 2) == '@') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_COMMA_AT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '%' && peek_n(&lx, 1) == ',') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_PERCENT_COMMA, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '$' && peek_n(&lx, 1) == '(') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_DOLLAR_LPAREN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '^') { advance(&lx); if (!add_token(out, &lx, TOK_PIN, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == ':' && continues_word(&lx, out) && !is_delim(peek_n(&lx, 1))) {
            advance(&lx);
            if (!read_shell_word(out, &lx, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == ':' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (is_ident_part(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_ATOM, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '=' && continues_word(&lx, out) && is_delim(peek_n(&lx, 1))) {
            advance(&lx);
            if (!add_token(out, &lx, TOK_SHELL_WORD, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '$' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (isalnum((unsigned char)peek(&lx)) || peek(&lx) == '_') advance(&lx);
            if (!add_token(out, &lx, TOK_SHELL_VAR, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '&' && is_ident_start(peek_n(&lx, 1))) {
            advance(&lx);
            while (is_ident_part(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_FN_VALUE, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (isdigit((unsigned char)ch) || (ch == '-' && isdigit((unsigned char)peek_n(&lx, 1)) &&
            (leading_space || previous_token_allows_negative_literal(out)))) {
            if (ch == '-') advance(&lx);
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
            if (!add_token(out, &lx, is_float ? TOK_FLOAT : TOK_INT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
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
            if (peek(&lx) == ':' && peek_n(&lx, 1) != ':' && is_delim(peek_n(&lx, 1))) {
                advance(&lx);
                if (!add_token(out, &lx, TOK_KEYWORD, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
                leading_space = false;
                continue;
            }
            if (!add_token(out, &lx, TOK_IDENT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '.' && peek_n(&lx, 1) == '.' && peek_n(&lx, 2) == '.') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_IDENT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '.') { advance(&lx); if (!add_token(out, &lx, TOK_DOT, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '*' && peek_n(&lx, 1) == '*') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '/' && peek_n(&lx, 1) == '/' && is_delim(peek_n(&lx, 2))) { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if ((ch == '-' && !isdigit((unsigned char)peek_n(&lx, 1)) && peek_n(&lx, 1) != '>' && !isspace((unsigned char)peek_n(&lx, 1))) ||
            ch == '~' ||
            ((ch == '/' || ch == '+' || ch == '*') && !is_delim(peek_n(&lx, 1)))) {
            if (!read_shell_word(out, &lx, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        if (ch == '-' && peek_n(&lx, 1) == '>') { advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (ch == '|' && peek_n(&lx, 1) == '>' && peek_n(&lx, 2) == '>') { advance(&lx); advance(&lx); advance(&lx); if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column}); leading_space = false; continue; }
        if (is_operator_char(ch)) {
            advance(&lx);
            while (is_operator_char(peek(&lx))) advance(&lx);
            if (!add_token(out, &lx, TOK_OP, start, line, column, leading_space)) return idm_error_oom(err, (IdmSpan){file, start, lx.pos, line, column});
            leading_space = false;
            continue;
        }
        return idm_error_set(err, (IdmSpan){file, start, start + 1u, line, column}, "unexpected character '%c'", ch);
    }
    Token eof;
    eof.kind = TOK_EOF;
    eof.lexeme = idm_strdup("");
    if (!eof.lexeme) return idm_error_oom(err, (IdmSpan){file, lx.pos, lx.pos, lx.line, lx.column});
    eof.span = (IdmSpan){file, lx.pos, lx.pos, lx.line, lx.column};
    eof.leading_space = leading_space;
    eof.adjacent_previous = lx.pos == lx.previous_end;
    return tokens_push(out, eof) || idm_error_oom(err, eof.span);
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

static IdmSyntax *form1(const char *head, IdmSyntax *a, IdmSpan span) {
    IdmSyntax *list = idm_syn_list(span);
    if (!list || !idm_syn_append(list, idm_syn_word(head, span)) || !idm_syn_append(list, a)) {
        idm_syn_free(list);
        idm_syn_free(a);
        return NULL;
    }
    return list;
}


static IdmSyntax *read_interp_inner(const char *file, const char *inner, bool command, IdmSpan span, IdmError *err) {
    IdmSyntax *pkg = NULL;
    if (!idm_reader_read_string(file, inner, &pkg, err)) return NULL;
    if (!pkg || pkg->as.seq.count != 2) {
        idm_syn_free(pkg);
        idm_error_set(err, span, "string interpolation must contain a single expression");
        return NULL;
    }
    IdmSyntax *form = idm_syn_clone(pkg->as.seq.items[1]);
    idm_syn_free(pkg);
    if (!form) { idm_error_oom(err, span); return NULL; }
    if (command) {
        IdmSyntax *wrapped = form1("%-command-sub", form, span);
        if (!wrapped) { idm_error_oom(err, span); return NULL; }
        return wrapped;
    }
    return form;
}

static IdmSyntax *parse_string_interp(Parser *p, const char *body, IdmSpan span) {
    IdmSyntax *result = idm_syn_list(span);
    if (!result || !idm_syn_append(result, idm_syn_word("%-string", span))) {
        idm_syn_free(result);
        idm_error_oom(p->err, span);
        return NULL;
    }
    IdmBuffer chunk;
    idm_buf_init(&chunk);
    size_t i = 0;
    while (body[i] != '\0') {
        char ch = body[i];
        bool command = ch == '$' && body[i + 1u] == '(';
        bool interp = command || (ch == '$' && body[i + 1u] == '{') || (ch == '#' && body[i + 1u] == '{');
        if (interp) {
            if (chunk.len > 0) {
                IdmSyntax *lit = idm_syn_string_n(chunk.data, chunk.len, span);
                if (!lit || !idm_syn_append(result, lit)) { idm_syn_free(lit); goto fail; }
                idm_buf_destroy(&chunk);
                idm_buf_init(&chunk);
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
            char *inner = idm_strndup(body + i + 2u, j - (i + 2u));
            if (!inner) { idm_error_oom(p->err, span); goto fail; }
            IdmSyntax *part = read_interp_inner(p->file, inner, command, span, p->err);
            free(inner);
            if (!part || !idm_syn_append(result, part)) { idm_syn_free(part); goto fail; }
            i = body[j] == '\0' ? j : j + 1u;
        } else {
            if (!idm_buf_append_char(&chunk, ch)) { idm_error_oom(p->err, span); goto fail; }
            i++;
        }
    }
    if (chunk.len > 0) {
        IdmSyntax *lit = idm_syn_string_n(chunk.data, chunk.len, span);
        if (!lit || !idm_syn_append(result, lit)) { idm_syn_free(lit); goto fail; }
    }
    idm_buf_destroy(&chunk);
    return result;
fail:
    idm_buf_destroy(&chunk);
    idm_syn_free(result);
    return NULL;
}

static IdmSyntax *parse_expr(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IdmSyntax *parse_expr_delimited(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IdmSyntax *parse_expr_body(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3);
static IdmSyntax *parse_primary(Parser *p);
static IdmSyntax *parse_primary_at_depth(Parser *p);

static bool ident_is(Parser *p, const char *text) {
    return cur(p)->kind == TOK_IDENT && strcmp(cur(p)->lexeme, text) == 0;
}





static IdmSyntax *parse_container(Parser *p, IdmSyntaxKind kind, TokenKind close, IdmSpan span) {
    IdmSyntax *seq = NULL;
    if (kind == IDM_SYN_VECTOR) seq = idm_syn_vector(span);
    else if (kind == IDM_SYN_TUPLE) seq = idm_syn_tuple(span);
    else if (kind == IDM_SYN_DICT) seq = idm_syn_dict(span);
    if (!seq) return NULL;
    skip_separators(p);
    while (!at(p, close) && !at(p, TOK_EOF)) {
        IdmSyntax *item = parse_primary(p);
        if (!item || !idm_syn_append(seq, item)) {
            idm_syn_free(seq);
            idm_syn_free(item);
            return NULL;
        }
        skip_separators(p);
    }
    if (!at(p, close)) {
        idm_syn_free(seq);
        idm_error_set(p->err, cur(p)->span, "unterminated container");
        return NULL;
    }
    take(p);
    return seq;
}

static IdmSyntax *parse_body(Parser *p, IdmSpan span) {
    IdmSyntax *body = idm_syn_list(span);
    if (!body || !idm_syn_append(body, idm_syn_word("%-body", span))) {
        idm_syn_free(body);
        return NULL;
    }
    skip_separators(p);
    while (!at(p, TOK_EOF) && !ident_is(p, "end") && !ident_is(p, "else")) {
        IdmSyntax *form = parse_expr_body(p, TOK_EOF, TOK_EOF, TOK_EOF);
        if (!form || !idm_syn_append(body, form)) {
            idm_syn_free(form);
            idm_syn_free(body);
            return NULL;
        }
        skip_separators(p);
    }
    if (ident_is(p, "else")) {
        p->body_paused_on_else = true;
        return body;
    }
    if (!ident_is(p, "end")) {
        idm_syn_free(body);
        idm_error_set(p->err, span, "unterminated do/end body");
        return NULL;
    }
    take(p);
    return body;
}

static IdmSyntax *parse_primary(Parser *p) {
    if (p->depth >= IDM_IC_MAX_DEPTH) {
        idm_error_set(p->err, cur(p)->span, "source nested too deeply");
        return NULL;
    }
    p->depth++;
    IdmSyntax *syn = parse_primary_at_depth(p);
    p->depth--;
    return syn;
}

static IdmSyntax *parse_primary_at_depth(Parser *p) {
    Token *tok = cur(p);
    switch (tok->kind) {
        case TOK_IDENT: {
            take(p);
            if (strcmp(tok->lexeme, "do") == 0) return parse_body(p, tok->span);
            if (strcmp(tok->lexeme, "end") == 0) {
                idm_error_set(p->err, tok->span, "unexpected end");
                return NULL;
            }
            IdmSyntax *syn = idm_syn_word(tok->lexeme, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_OP:
        case TOK_DOT: {
            if (tok->kind == TOK_OP && (strcmp(tok->lexeme, "<") == 0 || strcmp(tok->lexeme, ">") == 0) &&
                p->pos + 1u < p->count && p->tokens[p->pos + 1u].kind == TOK_LPAREN && p->tokens[p->pos + 1u].adjacent_previous) {
                bool write = strcmp(tok->lexeme, ">") == 0;
                take(p);
                take(p);
                IdmSyntax *inner = parse_expr_delimited(p, TOK_RPAREN, TOK_EOF, TOK_EOF);
                if (!inner) return NULL;
                if (!at(p, TOK_RPAREN)) { idm_syn_free(inner); idm_error_set(p->err, tok->span, "unterminated process substitution"); return NULL; }
                take(p);
                return form1(write ? "%-procsub-write" : "%-procsub-read", inner, tok->span);
            }
            take(p);
            IdmSyntax *syn = idm_syn_word(tok->lexeme, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_ATOM: {
            take(p);
            IdmSyntax *syn = strcmp(tok->lexeme, ":nil") == 0 ? idm_syn_nil(tok->span) : idm_syn_atom(tok->lexeme + 1, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_KEYWORD: {
            take(p);
            size_t len = strlen(tok->lexeme);
            char *name = idm_strndup(tok->lexeme, len > 0 ? len - 1u : 0);
            if (!name) { idm_error_oom(p->err, tok->span); return NULL; }
            IdmSyntax *syn = idm_syn_atom(name, tok->span);
            free(name);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_INT: {
            take(p);
            errno = 0;
            long long value = strtoll(tok->lexeme, NULL, 10);
            if (errno != 0) {
                idm_error_set(p->err, tok->span, "invalid integer literal");
                return NULL;
            }
            IdmSyntax *syn = idm_syn_int(value, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_FLOAT: {
            take(p);
            errno = 0;
            double value = strtod(tok->lexeme, NULL);
            if (errno != 0) {
                idm_error_set(p->err, tok->span, "invalid float literal");
                return NULL;
            }
            IdmSyntax *syn = idm_syn_float(value, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_STRING: {
            take(p);
            IdmSyntax *syn = idm_syn_string(tok->lexeme, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_STRING_INTERP: {
            take(p);
            return parse_string_interp(p, tok->lexeme, tok->span);
        }
        case TOK_REGEX: {
            take(p);
            IdmSyntax *syn = form1("%-regex", idm_syn_string(tok->lexeme, tok->span), tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_SHELL_WORD: {
            take(p);
            IdmSyntax *syn = form1("%-word", idm_syn_string(tok->lexeme, tok->span), tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_HEREDOC: {
            take(p);
            return form1("%-heredoc", idm_syn_string(tok->lexeme, tok->span), tok->span);
        }
        case TOK_SHELL_VAR: {
            take(p);
            IdmSyntax *syn = form1("%-shell-var", idm_syn_word(tok->lexeme + 1, tok->span), tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        case TOK_FN_VALUE: {
            take(p);
            return form1("%-expression", idm_syn_word(tok->lexeme, tok->span), tok->span);
        }
        case TOK_PIN: {
            take(p);
            return form1("%-pin", parse_primary(p), tok->span);
        }
        case TOK_LPAREN: {
            take(p);
            IdmSyntax *inner = parse_expr_delimited(p, TOK_RPAREN, TOK_EOF, TOK_EOF);
            if (!inner) return NULL;
            if (!at(p, TOK_RPAREN)) {
                idm_syn_free(inner);
                idm_error_set(p->err, tok->span, "unterminated group");
                return NULL;
            }
            take(p);
            return form1("%-group", inner, tok->span);
        }
        case TOK_LBRACKET: take(p); return parse_container(p, IDM_SYN_VECTOR, TOK_RBRACKET, tok->span);
        case TOK_LBRACE: take(p); return parse_container(p, IDM_SYN_TUPLE, TOK_RBRACE, tok->span);
        case TOK_PERCENT_LBRACE: take(p); return parse_container(p, IDM_SYN_DICT, TOK_RBRACE, tok->span);
        case TOK_QUOTE: {
            take(p);
            if (at(p, TOK_LPAREN) && p->pos + 1u < p->count && p->tokens[p->pos + 1u].kind == TOK_RPAREN) {
                take(p);
                take(p);
                return form1("%-quote", idm_syn_list(tok->span), tok->span);
            }
            return form1("%-quote", parse_primary(p), tok->span);
        }
        case TOK_QUASIQUOTE: take(p); return form1("%-quasiquote", parse_primary(p), tok->span);
        case TOK_COMMA: take(p); return form1("%-unquote", parse_primary(p), tok->span);
        case TOK_COMMA_AT: take(p); return form1("%-unquote-splicing", parse_primary(p), tok->span);
        case TOK_PERCENT_QUOTE: take(p); return form1("%-syntax", parse_primary(p), tok->span);
        case TOK_PERCENT_QUASIQUOTE: take(p); return form1("%-quasisyntax", parse_primary(p), tok->span);
        case TOK_PERCENT_COMMA: take(p); return form1("%-unsyntax", parse_primary(p), tok->span);
        case TOK_PERCENT_COMMA_AT: take(p); return form1("%-unsyntax-splicing", parse_primary(p), tok->span);
        case TOK_DOLLAR_LPAREN: {
            take(p);
            IdmSyntax *inner = parse_expr_delimited(p, TOK_RPAREN, TOK_EOF, TOK_EOF);
            if (!inner) return NULL;
            if (!at(p, TOK_RPAREN)) {
                idm_syn_free(inner);
                idm_error_set(p->err, tok->span, "unterminated command substitution");
                return NULL;
            }
            take(p);
            IdmSyntax *syn = form1("%-command-sub", inner, tok->span);
            if (syn) idm_syn_set_token(syn, tok->lexeme, tok->leading_space, tok->adjacent_previous);
            return syn;
        }
        default:
            idm_error_set(p->err, tok->span, "expected syntax item");
            return NULL;
    }
}

static IdmSyntax *parse_expr_impl(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3, bool separators_stop, bool end_stops, bool else_stops) {
    IdmSpan span = cur(p)->span;
    IdmSyntax *expr = idm_syn_list(span);
    if (!expr || !idm_syn_append(expr, idm_syn_word("%-expr", span))) {
        idm_syn_free(expr);
        return NULL;
    }
    while (true) {
        if (separators_stop && at(p, TOK_SEMI)) break;
        if (separators_stop && at(p, TOK_NEWLINE)) {
            if (!indented_newline_continues(p, span.column, end1, end2, end3, end_stops, else_stops)) break;
            skip_separators(p);
            continue;
        }
        if (p->body_paused_on_else && cur(p)->kind == TOK_IDENT && strcmp(cur(p)->lexeme, "else") == 0) {
            p->body_paused_on_else = false;
        } else if (is_stop(p, end1, end2, end3, false, end_stops, else_stops)) {
            break;
        }
        if (!separators_stop && (at(p, TOK_NEWLINE) || at(p, TOK_SEMI))) {
            skip_separators(p);
            continue;
        }
        IdmSyntax *part = parse_primary(p);
        if (!part || !idm_syn_append(expr, part)) {
            idm_syn_free(part);
            idm_syn_free(expr);
            return NULL;
        }
    }
    if (expr->as.seq.count == 1) {
        idm_error_set(p->err, span, "expected expression");
        idm_syn_free(expr);
        return NULL;
    }
    return expr;
}

static IdmSyntax *parse_expr(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    return parse_expr_impl(p, end1, end2, end3, true, true, false);
}

static IdmSyntax *parse_expr_delimited(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    skip_separators(p);
    return parse_expr_impl(p, end1, end2, end3, false, false, false);
}

static IdmSyntax *parse_expr_body(Parser *p, TokenKind end1, TokenKind end2, TokenKind end3) {
    return parse_expr_impl(p, end1, end2, end3, true, true, true);
}

static IdmSyntax *parse_program(Parser *p) {
    IdmSyntax *program = idm_syn_list(idm_span_unknown(p->file));
    if (!program || !idm_syn_append(program, idm_syn_word("%-package-begin", idm_span_unknown(p->file)))) {
        idm_syn_free(program);
        return NULL;
    }
    skip_separators(p);
    while (!at(p, TOK_EOF)) {
        IdmSyntax *form = parse_expr(p, TOK_EOF, TOK_EOF, TOK_EOF);
        if (!form || !idm_syn_append(program, form)) {
            idm_syn_free(form);
            idm_syn_free(program);
            return NULL;
        }
        skip_separators(p);
    }
    return program;
}

bool idm_reader_read_string(const char *file, const char *source, IdmSyntax **out, IdmError *err) {
    TokenVec tokens = {0};
    unsigned start_line = 1;
    if (source[0] == '#' && source[1] == '!') {
        while (*source != '\0' && *source != '\n') source++;
        if (*source == '\n') source++;
        start_line = 2;
    }
    if (!lex_source_from(file, source, strlen(source), start_line, &tokens, err)) {
        tokens_destroy(&tokens);
        return false;
    }
    Parser p = {tokens.items, tokens.count, 0, file, err, false, 0};
    IdmSyntax *program = parse_program(&p);
    tokens_destroy(&tokens);
    if (!program) return false;
    *out = program;
    return true;
}

bool idm_reader_read_file(const char *path, IdmSyntax **out, IdmError *err) {
    char *source = NULL;
    size_t len = 0;
    if (!idm_read_file(path, &source, &len, err)) return false;
    (void)len;
    bool ok = idm_reader_read_string(path, source, out, err);
    free(source);
    return ok;
}
