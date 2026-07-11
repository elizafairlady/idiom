#include "idiom/artifact.h"
#include "idiom/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "reader_artifact: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_rejected(bool ok, IdmError *err, const char *needle, const char *name) {
    check(!ok, name);
    check(err->present, name);
    if (!err->message || !strstr(err->message, needle)) {
        fprintf(stderr, "reader_artifact: %s: want '%s', got '%s'\n", name, needle, err->message ? err->message : "<no error>");
        exit(1);
    }
    idm_error_clear(err);
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

static IdmGrammarRule skip_rule(void) {
    IdmGrammarRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.name = "space";
    rule.kind = (uint8_t)IDM_GRAMMAR_RULE_SKIP;
    rule.terminal.kind = (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX;
    rule.terminal.text = "[ \t\r\n]+";
    return rule;
}

static IdmGrammarRule token_rule(IdmReaderInst *ctor, size_t ctor_count) {
    IdmGrammarRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.name = "tok";
    rule.kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
    rule.terminal.kind = (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX;
    rule.terminal.text = "[^ \t\r\n]+";
    rule.constructor.items = ctor;
    rule.constructor.count = ctor_count;
    return rule;
}

static IdmGrammarRule named_token_rule(const char *name, uint8_t terminal_kind, char *terminal_text, IdmReaderInst *ctor, size_t ctor_count) {
    IdmGrammarRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.name = (char *)name;
    rule.kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
    rule.terminal.kind = terminal_kind;
    rule.terminal.text = terminal_text;
    rule.constructor.items = ctor;
    rule.constructor.count = ctor_count;
    return rule;
}

static IdmGrammarRule form_rule(const char *name, IdmReaderInst *pattern, size_t pattern_count, IdmReaderInst *ctor, size_t ctor_count) {
    IdmGrammarRule rule;
    memset(&rule, 0, sizeof(rule));
    rule.name = (char *)name;
    rule.kind = (uint8_t)IDM_GRAMMAR_RULE_FORM;
    rule.pattern.items = pattern;
    rule.pattern.count = pattern_count;
    rule.constructor.items = ctor;
    rule.constructor.count = ctor_count;
    return rule;
}

static bool build_artifact(IdmGrammarRule *rules, size_t rule_count, IdmReaderArtifact **out, IdmError *err) {
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = "ReaderArtifactUnit";
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rules = rules;
    grammar.rule_count = rule_count;
    return idm_reader_artifact_from_grammars("ReaderArtifactUnit", &grammar, 1u, out, err);
}

static void check_unknown_pattern_op(IdmError *err) {
    IdmReaderInst pattern[] = { inst(250u, NULL) };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin") };
    IdmGrammarRule rule = form_rule("program", pattern, 1u, ctor, 1u);
    check_rejected(idm_grammar_rule_validate(&rule, err, idm_span_unknown(NULL)), err, "unknown reader pattern opcode", "unknown pattern op rejected");
}

static void check_unknown_ctor_op(IdmError *err) {
    IdmReaderInst ctor[] = { inst(250u, NULL) };
    IdmGrammarRule rule = token_rule(ctor, 1u);
    check_rejected(idm_grammar_rule_validate(&rule, err, idm_span_unknown(NULL)), err, "unknown reader constructor opcode", "unknown ctor op rejected");
}

static void check_bad_target_shape(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_REF, NULL) };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin") };
    IdmGrammarRule rule = form_rule("program", pattern, 1u, ctor, 1u);
    check_rejected(idm_grammar_rule_validate(&rule, err, idm_span_unknown(NULL)), err, "invalid reader pattern reference instruction", "target without name rejected");
}

static void check_trailing_pattern(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_TOKEN, "tok"), inst((uint8_t)IDM_READER_PATTERN_TOKEN, "tok") };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin") };
    IdmGrammarRule rule = form_rule("program", pattern, 2u, ctor, 1u);
    check_rejected(idm_grammar_rule_validate(&rule, err, idm_span_unknown(NULL)), err, "trailing instructions", "trailing pattern instruction rejected");
}

static void check_unresolved_ref(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_REF, "nope") };
    IdmReaderInst token_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin") };
    IdmGrammarRule rules[] = {
        skip_rule(),
        token_rule(token_ctor, 1u),
        form_rule("program", pattern, 1u, ctor, 1u),
    };
    IdmReaderArtifact *artifact = NULL;
    check_rejected(build_artifact(rules, 3u, &artifact, err), err, "references unknown rule 'nope'", "unresolved reference rejected");
    check(!artifact, "unresolved reference yields no artifact");
}

static void check_program_ctor_shape(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_CAPTURE, "tok"), inst((uint8_t)IDM_READER_PATTERN_TOKEN, "tok") };
    pattern[0].child_count = 1u;
    IdmReaderInst token_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    IdmGrammarRule rules[] = {
        skip_rule(),
        token_rule(token_ctor, 1u),
        form_rule("program", pattern, 2u, ctor, 1u),
    };
    IdmReaderArtifact *artifact = NULL;
    check_rejected(build_artifact(rules, 3u, &artifact, err), err, "program rule constructor must emit %-package-begin", "program ctor shape rejected");
    check(!artifact, "program ctor shape yields no artifact");
}

static void check_missing_program_rule(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_TOKEN, "tok") };
    IdmReaderInst token_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin") };
    IdmGrammarRule rules[] = {
        skip_rule(),
        token_rule(token_ctor, 1u),
        form_rule("start", pattern, 1u, ctor, 1u),
    };
    IdmReaderArtifact *artifact = NULL;
    check_rejected(build_artifact(rules, 3u, &artifact, err), err, "has no 'program' start rule", "missing program rule rejected");
    check(!artifact, "missing program rule yields no artifact");
}

static void check_valid_grammar_matches(IdmError *err) {
    IdmReaderInst pattern[] = { inst((uint8_t)IDM_READER_PATTERN_CAPTURE, "tok"), inst((uint8_t)IDM_READER_PATTERN_TOKEN, "tok") };
    pattern[0].child_count = 1u;
    IdmReaderInst token_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    IdmReaderInst ctor[] = { inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin"), inst((uint8_t)IDM_READER_CTOR_CAPTURE, "tok") };
    ctor[0].child_count = 1u;
    IdmGrammarRule rules[] = {
        skip_rule(),
        token_rule(token_ctor, 1u),
        form_rule("program", pattern, 2u, ctor, 2u),
    };
    IdmReaderArtifact *artifact = NULL;
    if (!build_artifact(rules, 3u, &artifact, err)) {
        fprintf(stderr, "reader_artifact: valid grammar build: %s\n", err->message ? err->message : "<no error>");
        exit(1);
    }
    IdmSyntax *program = NULL;
    check(idm_reader_read_artifact_string(artifact, "reader_artifact", "word", 0, &program, NULL, err), "valid grammar reads");
    check(program && program->kind == IDM_SYN_LIST && program->as.seq.count == 2u, "valid grammar program shape");
    check(strcmp(program->as.seq.items[0]->as.text, "%-package-begin") == 0, "valid grammar package begin");
    check(program->as.seq.items[1]->kind == IDM_SYN_WORD && strcmp(program->as.seq.items[1]->as.text, "word") == 0, "valid grammar token value");
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
}

static void check_literal_terminal_alt_start(IdmError *err) {
    IdmSpan span = idm_span_unknown("reader_artifact");
    IdmReaderInst kw_ctor[] = { inst((uint8_t)IDM_READER_CTOR_EMIT_TEXT, "kw") };
    kw_ctor[0].integer = IDM_SYN_WORD;
    IdmReaderInst other_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "other") };
    IdmReaderInst item_pattern[] = {
        inst((uint8_t)IDM_READER_PATTERN_CAPTURE, "value"),
        inst((uint8_t)IDM_READER_PATTERN_ALT, NULL),
        inst((uint8_t)IDM_READER_PATTERN_LITERAL, NULL),
        inst((uint8_t)IDM_READER_PATTERN_TOKEN, "other"),
    };
    item_pattern[0].child_count = 1u;
    item_pattern[1].child_count = 2u;
    item_pattern[2].has_literal = true;
    item_pattern[2].literal = idm_syn_word("kw", span);
    check(item_pattern[2].literal != NULL, "literal terminal alt literal alloc");
    IdmReaderInst item_ctor[] = { inst((uint8_t)IDM_READER_CTOR_CAPTURE, "value") };
    IdmReaderInst program_pattern[] = {
        inst((uint8_t)IDM_READER_PATTERN_CAPTURE, "item"),
        inst((uint8_t)IDM_READER_PATTERN_REF, "item"),
    };
    program_pattern[0].child_count = 1u;
    IdmReaderInst program_ctor[] = {
        inst((uint8_t)IDM_READER_CTOR_FORM, "%-package-begin"),
        inst((uint8_t)IDM_READER_CTOR_CAPTURE, "item"),
    };
    program_ctor[0].child_count = 1u;
    IdmGrammarRule rules[] = {
        skip_rule(),
        named_token_rule("kw", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "kw", kw_ctor, 1u),
        named_token_rule("other", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "other", other_ctor, 1u),
        form_rule("item", item_pattern, 4u, item_ctor, 1u),
        form_rule("program", program_pattern, 2u, program_ctor, 2u),
    };
    IdmReaderArtifact *artifact = NULL;
    if (!build_artifact(rules, 5u, &artifact, err)) {
        fprintf(stderr, "reader_artifact: literal terminal alt build: %s\n", err->message ? err->message : "<no error>");
        exit(1);
    }
    IdmSyntax *program = NULL;
    check(idm_reader_read_artifact_string(artifact, "reader_artifact", "kw", 0, &program, NULL, err), "literal terminal alt reads");
    check(program && program->kind == IDM_SYN_LIST && program->as.seq.count == 2u, "literal terminal alt program shape");
    check(program->as.seq.items[1]->kind == IDM_SYN_WORD && strcmp(program->as.seq.items[1]->as.text, "kw") == 0, "literal terminal alt value");
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_syn_free(item_pattern[2].literal);
}

int idm_unit_reader_artifact(void) {
    IdmError err;
    idm_error_init(&err);
    check_unknown_pattern_op(&err);
    check_unknown_ctor_op(&err);
    check_bad_target_shape(&err);
    check_trailing_pattern(&err);
    check_unresolved_ref(&err);
    check_missing_program_rule(&err);
    check_program_ctor_shape(&err);
    check_valid_grammar_matches(&err);
    check_literal_terminal_alt_start(&err);
    idm_error_clear(&err);
    return 0;
}
