#include "idiom/artifact.h"
#include "idiom/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "reader_escape: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error_message(IdmError *err, const char *needle, const char *name) {
    check(err->present, name);
    check(err->message && strstr(err->message, needle), name);
    idm_error_clear(err);
}

static void check_ok_or_error(bool ok, IdmError *err, const char *name) {
    if (ok) return;
    fprintf(stderr, "reader_escape: %s: %s\n", name, err->message ? err->message : "<no error>");
    exit(1);
}

static IdmReaderInst inst(uint8_t op, char *text) {
    IdmReaderInst out;
    memset(&out, 0, sizeof(out));
    out.op = op;
    out.text = text;
    out.literal_index = SIZE_MAX;
    out.capture_slot = SIZE_MAX;
    return out;
}

static bool make_artifact(uint8_t terminal_kind, char *terminal_text, uint8_t ctor_op, IdmReaderArtifact **out, IdmError *err) {
    IdmReaderInst token_ctor[] = { inst(ctor_op, "tok") };
    IdmReaderInst program_pattern[] = { inst(IDM_READER_PATTERN_CAPTURE, "tok"), inst(IDM_READER_PATTERN_TOKEN, "tok") };
    program_pattern[0].child_count = 1u;
    IdmReaderInst program_ctor[] = { inst(IDM_READER_CTOR_FORM, "%-package-begin"), inst(IDM_READER_CTOR_CAPTURE, "tok") };
    program_ctor[0].child_count = 1u;

    IdmGrammarRule rules[3];
    memset(rules, 0, sizeof(rules));
    rules[0].name = "space";
    rules[0].kind = (uint8_t)IDM_GRAMMAR_RULE_SKIP;
    rules[0].terminal.kind = (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX;
    rules[0].terminal.text = "[ \t\r\n]+";

    rules[1].name = "tok";
    rules[1].kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
    rules[1].terminal.kind = terminal_kind;
    rules[1].terminal.text = terminal_text;
    rules[1].constructor.items = token_ctor;
    rules[1].constructor.count = 1u;

    rules[2].name = "program";
    rules[2].kind = (uint8_t)IDM_GRAMMAR_RULE_FORM;
    rules[2].pattern.items = program_pattern;
    rules[2].pattern.count = 2u;
    rules[2].constructor.items = program_ctor;
    rules[2].constructor.count = 2u;

    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = "ReaderEscapeUnit";
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rules = rules;
    grammar.rule_count = 3u;
    return idm_reader_artifact_from_grammars("ReaderEscapeUnit", &grammar, 1u, out, err);
}

static const IdmSyntax *transparent_leaf(const IdmSyntax *program, const char *name) {
    check(program && program->kind == IDM_SYN_LIST && program->as.seq.count == 1u, name);
    return program->as.seq.items[0];
}

static const IdmSyntax *artifact_leaf(const IdmSyntax *program, const char *name) {
    check(program && program->kind == IDM_SYN_LIST && program->as.seq.count == 2u, name);
    check(program->as.seq.items[0]->kind == IDM_SYN_WORD, name);
    check(strcmp(program->as.seq.items[0]->as.text, "%-package-begin") == 0, name);
    return program->as.seq.items[1];
}

static void check_kind(const IdmSyntax *syn, IdmSyntaxKind kind, const char *text, int64_t integer, double real, const char *name) {
    check(syn && syn->kind == kind, name);
    if (kind == IDM_SYN_WORD || kind == IDM_SYN_ATOM || kind == IDM_SYN_BIGINT) check(strcmp(syn->as.text, text) == 0, name);
    if (kind == IDM_SYN_INT) check(syn->as.integer == integer, name);
    if (kind == IDM_SYN_FLOAT) check(syn->as.real == real, name);
}

static void check_classified(IdmReaderArtifact *artifact, const char *raw, IdmSyntaxKind kind, const char *text, int64_t integer, double real, IdmError *err, const char *name) {
    IdmSyntax *transparent = NULL;
    IdmSyntax *custom = NULL;
    check_ok_or_error(idm_reader_read_terms_string("reader_escape", raw, &transparent, err), err, "classifier transparent read");
    check(!err->present, "classifier transparent error clear");
    check_ok_or_error(idm_reader_read_artifact_string(artifact, "reader_escape", raw, &custom, err), err, "classifier artifact read");
    check(!err->present, "classifier artifact error clear");
    const IdmSyntax *a = transparent_leaf(transparent, name);
    const IdmSyntax *b = artifact_leaf(custom, name);
    check(idm_syn_equal(a, b), "classifier frontend equality");
    check_kind(a, kind, text, integer, real, name);
    idm_syn_free(transparent);
    idm_syn_free(custom);
}

int idm_unit_reader_escape(void) {
    IdmError err;
    idm_error_init(&err);
    char decoded = 0;
    check(idm_reader_string_escape('n', &decoded), "decode newline");
    check(decoded == '\n', "newline value");
    check(!idm_reader_string_escape('q', &decoded), "reject unknown helper escape");

    IdmSyntax *syn = NULL;
    check(!idm_reader_read_terms_string("reader_escape", "\"x\\q\"", &syn, &err), "transparent reject");
    check_error_message(&err, "unknown string escape", "transparent reject message");
    idm_syn_free(syn);

    IdmReaderArtifact *artifact = NULL;
    check_ok_or_error(make_artifact((uint8_t)IDM_GRAMMAR_TERMINAL_STRING, "", (uint8_t)IDM_READER_CTOR_CAPTURE, &artifact, &err), &err, "string artifact build");
    check(!idm_reader_read_artifact_string(artifact, "reader_escape", "\"x\\q\"", &syn, &err), "artifact string reject");
    check_error_message(&err, "invalid artifact string token", "artifact string reject message");
    idm_reader_artifact_destroy(artifact);
    idm_syn_free(syn);
    syn = NULL;

    check_ok_or_error(make_artifact((uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "S[^ \t\r\n]+", (uint8_t)IDM_READER_CTOR_INTERP_STRING, &artifact, &err), &err, "interp artifact build");
    check(!idm_reader_read_artifact_string(artifact, "reader_escape", "S\\q", &syn, &err), "artifact interp reject");
    check_error_message(&err, "unknown string escape", "artifact interp reject message");
    idm_reader_artifact_destroy(artifact);
    idm_syn_free(syn);
    syn = NULL;

    check_ok_or_error(make_artifact((uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[^ \t\r\n]+", (uint8_t)IDM_READER_CTOR_CAPTURE, &artifact, &err), &err, "classifier artifact build");
    check_classified(artifact, ":nil", IDM_SYN_NIL, NULL, 0, 0.0, &err, "classify nil");
    check_classified(artifact, ":atom", IDM_SYN_ATOM, "atom", 0, 0.0, &err, "classify atom");
    check_classified(artifact, "42", IDM_SYN_INT, NULL, 42, 0.0, &err, "classify int");
    check_classified(artifact, "9223372036854775808", IDM_SYN_BIGINT, "9223372036854775808", 0, 0.0, &err, "classify bigint");
    check_classified(artifact, "3.5", IDM_SYN_FLOAT, NULL, 0, 3.5, &err, "classify float");
    check_classified(artifact, "word", IDM_SYN_WORD, "word", 0, 0.0, &err, "classify word");
    idm_reader_artifact_destroy(artifact);

    idm_error_clear(&err);
    return 0;
}
