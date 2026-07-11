#include "idiom/reader.h"
#include "idiom/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "reader_offset: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static const IdmSyntax *term(const IdmSyntax *program, size_t index, const char *name) {
    check(program && program->kind == IDM_SYN_LIST && program->as.seq.count > index, name);
    return program->as.seq.items[index];
}

static void check_word_span(const IdmSyntax *syn, const char *text, size_t start, size_t end, unsigned line, unsigned column, const char *name) {
    check(syn && syn->kind == IDM_SYN_WORD, name);
    check(strcmp(syn->as.text, text) == 0, name);
    check(syn->span.start == start, name);
    check(syn->span.end == end, name);
    check(syn->span.line == line, name);
    check(syn->span.column == column, name);
}

int idm_unit_reader_offset(void) {
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;

    check(idm_reader_read_terms_string("reader_offset", "foo", 0, &program, NULL, &err), "baseline read");
    check_word_span(term(program, 0, "baseline term"), "foo", 0, 3, 1, 1, "baseline span");
    idm_syn_free(program);
    program = NULL;

    const char *source = "abc\ndef ghi";
    check(idm_reader_read_terms_string("reader_offset", source, 4, &program, NULL, &err), "resume read");
    check_word_span(term(program, 0, "resume first term"), "def", 4, 7, 2, 1, "resume line start span");
    check_word_span(term(program, 1, "resume second term"), "ghi", 8, 11, 2, 5, "resume same line span");
    idm_syn_free(program);
    program = NULL;

    check(idm_reader_read_terms_string("reader_offset", source, 8, &program, NULL, &err), "mid-line resume read");
    check_word_span(term(program, 0, "mid-line term"), "ghi", 8, 11, 2, 5, "mid-line resume span");
    idm_syn_free(program);
    program = NULL;

    check(idm_reader_read_terms_string("reader_offset", "#!/usr/bin/env idiomc\nfoo", 0, &program, NULL, &err), "shebang read");
    check_word_span(term(program, 0, "shebang term"), "foo", 22, 25, 2, 1, "shebang absolute span");
    idm_syn_free(program);

    idm_error_clear(&err);
    return 0;
}
