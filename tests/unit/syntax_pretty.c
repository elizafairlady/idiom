#include "idiom/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static IdmSpan span(void) {
    return idm_span_unknown("syntax_pretty");
}

static void fail(const char *name) {
    fprintf(stderr, "syntax_pretty: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static IdmSyntax *must(IdmSyntax *syn, const char *name) {
    if (!syn) fail(name);
    return syn;
}

static IdmSyntax *word(const char *text) { return must(idm_syn_word(text, span()), "word alloc"); }
static IdmSyntax *integer(int64_t value) { return must(idm_syn_int(value, span()), "int alloc"); }
static IdmSyntax *string(const char *text) { return must(idm_syn_string(text, span()), "string alloc"); }
static IdmSyntax *list(void) { return must(idm_syn_list(span()), "list alloc"); }

static void append(IdmSyntax *seq, IdmSyntax *item) {
    check(idm_syn_append(seq, item), "append");
}

static char *pretty(const IdmSyntax *syn) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_syn_dump_pretty(&buf, syn), "pretty ok");
    return idm_buf_take(&buf);
}

static void check_pretty(const IdmSyntax *syn, const char *expected, const char *name) {
    char *got = pretty(syn);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "syntax_pretty: %s\n  expected: %s\n  got:      %s\n", name, expected, got);
        exit(1);
    }
    free(got);
}

static IdmSyntax *call3(const char *fn, int64_t a, int64_t b) {
    IdmSyntax *seq = list();
    append(seq, word(fn));
    append(seq, integer(a));
    append(seq, integer(b));
    return seq;
}

int idm_unit_syntax_pretty(void);

int idm_unit_syntax_pretty(void) {
    IdmSyntax *fits = call3("add", 1, 2);
    check_pretty(fits, "(add 1 2)", "compact form stays on one line");
    idm_syn_free(fits);

    IdmSyntax *body = list();
    append(body, word("%-body"));
    append(body, call3("add", 1, 2));
    append(body, call3("mul", 3, 4));
    check_pretty(body, "(%-body\n  (add 1 2)\n  (mul 3 4))", "statement seq breaks per statement");

    IdmSyntax *outer = list();
    append(outer, word("wrap"));
    append(outer, body);
    check_pretty(outer, "(wrap (%-body\n  (add 1 2)\n  (mul 3 4)))", "a nested statement seq forces the parent multiline");
    idm_syn_free(outer);

    IdmSyntax *wide = list();
    append(wide, word("call"));
    for (int i = 0; i < 12; i++) append(wide, must(idm_syn_atom("segment", span()), "atom alloc"));
    IdmSyntax *tail = call3("add", 10, 20);
    append(wide, tail);
    char *got = pretty(wide);
    check(strstr(got, "\n  (add 10 20)") != NULL, "an overflowing compound child breaks to an indented line");
    free(got);
    idm_syn_free(wide);

    size_t leaf_len = 1u << 23;
    char *text = malloc(leaf_len + 1u);
    check(text != NULL, "leaf alloc");
    memset(text, 'x', leaf_len);
    text[leaf_len] = '\0';
    IdmSyntax *deep = string(text);
    free(text);
    size_t depth = 1024;
    for (size_t i = 0; i < depth; i++) {
        IdmSyntax *level = list();
        append(level, word("w"));
        append(level, deep);
        deep = level;
    }
    clock_t started = clock();
    got = pretty(deep);
    double elapsed = (double)(clock() - started) / CLOCKS_PER_SEC;
    size_t expected = depth * depth + 3u * depth + leaf_len + 2u;
    check(strlen(got) == expected, "deep nest output length");
    free(got);
    idm_syn_free(deep);
    if (elapsed > 5.0) {
        fprintf(stderr, "syntax_pretty: deep nest took %.1fs; width must be measured once per node\n", elapsed);
        exit(1);
    }

    return 0;
}
