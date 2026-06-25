#include "test_util.h"

#include "../../src/expand/internal.h"

#include "idiom/artifact.h"

static bool add_primitive_clause_fn(IdmBytecodeModule *module, IdmPrimitive primitive, uint32_t *out_index) {
    return idm_bc_add_primitive_function(module, idm_primitive_name(primitive), idm_primitive_arity(primitive), (uint32_t)primitive, out_index);
}

static bool emit_call(IdmBytecodeModule *module, uint32_t dst, uint32_t callee, uint32_t first_arg, uint32_t argc, bool tail) {
    return test_emit_call(module, dst, callee, first_arg, argc, tail);
}

static IdmCore *primitive_clause_call(IdmPrimitive primitive, IdmSpan span) {
    IdmCore *callee = idm_core_primitive_backed_fn(idm_primitive_name(primitive), primitive, idm_primitive_arity(primitive), span);
    CHECK(callee != NULL);
    IdmCore *call = idm_core_call(callee, span);
    CHECK(call != NULL);
    return call;
}

static char *dump_transparent_reader(const char *src) {
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;
    if (!idm_reader_read_terms_string("<transparent-test>", src, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return NULL;
    }
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_syn_dump(&buf, program));
    idm_syn_free(program);
    return idm_buf_take(&buf);
}

static IdmSyntax *test_read_one_term(const char *src) {
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;
    if (!idm_reader_read_terms_string("<artifact-term>", src, &program, &err)) {
        idm_error_fprint(stderr, &err);
        idm_error_clear(&err);
        return NULL;
    }
    CHECK(program != NULL && program->kind == IDM_SYN_LIST && program->as.seq.count == 1u);
    IdmSyntax *out = program && program->kind == IDM_SYN_LIST && program->as.seq.count == 1u ? idm_syn_clone(program->as.seq.items[0]) : NULL;
    CHECK(out != NULL);
    idm_syn_free(program);
    idm_error_clear(&err);
    return out;
}

static bool test_reader_artifact_token_rule(IdmGrammarRule *rule, const char *name, uint8_t terminal_kind, const char *terminal_text, const char *pattern_src, const char *constructor_src) {
    memset(rule, 0, sizeof(*rule));
    rule->name = idm_strdup(name);
    rule->kind = (uint8_t)IDM_GRAMMAR_RULE_TOKEN;
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *pattern = test_read_one_term(pattern_src);
    IdmSyntax *constructor = test_read_one_term(constructor_src);
    bool ok = rule->name && pattern && constructor &&
              idm_grammar_terminal_from_ir(pattern, &rule->terminal, &err) &&
              idm_reader_ctor_compile_ir(constructor, &rule->constructor, &err) &&
              rule->terminal.kind == terminal_kind &&
              rule->terminal.text && strcmp(rule->terminal.text, terminal_text) == 0;
    if (!ok) {
        if (err.present) idm_error_fprint(stderr, &err);
        idm_grammar_rule_destroy(rule);
    }
    idm_syn_free(pattern);
    idm_syn_free(constructor);
    idm_error_clear(&err);
    return ok;
}

static bool test_reader_artifact_form_rule(IdmGrammarRule *rule, const char *name, const char *pattern_src, const char *constructor_src) {
    memset(rule, 0, sizeof(*rule));
    rule->name = idm_strdup(name);
    rule->kind = (uint8_t)IDM_GRAMMAR_RULE_FORM;
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *pattern = test_read_one_term(pattern_src);
    IdmSyntax *constructor = test_read_one_term(constructor_src);
    bool ok = rule->name && pattern && constructor &&
              idm_reader_pattern_compile_ir(pattern, &rule->pattern, &err) &&
              idm_reader_ctor_compile_ir(constructor, &rule->constructor, &err);
    if (!ok) {
        if (err.present) idm_error_fprint(stderr, &err);
        idm_grammar_rule_destroy(rule);
    }
    idm_syn_free(pattern);
    idm_syn_free(constructor);
    idm_error_clear(&err);
    return ok;
}

static bool test_reader_artifact_skip_rule(IdmGrammarRule *rule, const char *name, uint8_t terminal_kind, const char *terminal_text, const char *pattern_src) {
    memset(rule, 0, sizeof(*rule));
    rule->name = idm_strdup(name);
    rule->kind = (uint8_t)IDM_GRAMMAR_RULE_SKIP;
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *pattern = test_read_one_term(pattern_src);
    bool ok = rule->name && pattern &&
              idm_grammar_terminal_from_ir(pattern, &rule->terminal, &err) &&
              rule->terminal.kind == terminal_kind &&
              rule->terminal.text && strcmp(rule->terminal.text, terminal_text) == 0;
    if (!ok) {
        if (err.present) idm_error_fprint(stderr, &err);
        idm_grammar_rule_destroy(rule);
    }
    idm_syn_free(pattern);
    idm_error_clear(&err);
    return ok;
}

static void test_values(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue a1 = idm_atom(&rt, "ok");
    IdmValue a2 = idm_atom(&rt, "ok");
    IdmValue w1 = idm_word(&rt, "ok");
    CHECK(idm_value_equal(a1, a2));
    CHECK(!idm_value_equal(a1, w1));
    IdmValue s1 = idm_string(&rt, "hello", &err);
    IdmValue s2 = idm_string(&rt, "hello", &err);
    CHECK(idm_value_equal(s1, s2));
    IdmValue list = idm_cons(&rt, idm_int(1), idm_cons(&rt, idm_int(2), idm_empty_list(), &err), &err);
    IdmBuffer buf;
    idm_buf_init(&buf);
    CHECK(idm_value_write(&buf, list));
    CHECK_STR(buf.data, "(1 2)");
    idm_buf_destroy(&buf);
    CHECK(idm_heap_object_count(&rt.heap) == 4u);
    IdmValue roots[1] = { list };
    idm_heap_collect(&rt.heap, roots, 1u);
    CHECK(idm_heap_object_count(&rt.heap) == 2u);
    IdmValue car = idm_car(list, &err);
    CHECK(car.tag == IDM_VAL_INT && car.as.i == 1);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_gc_deep_pair_chain(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue list = idm_empty_list();
    for (size_t i = 0; i < 100000u; i++) {
        list = idm_cons(&rt, idm_int((int64_t)i), list, &err);
        CHECK(!err.present);
    }
    IdmValue roots[1] = { list };
    idm_heap_collect(&rt.heap, roots, 1u);
    CHECK(idm_heap_object_count(&rt.heap) == 100000u);
    idm_heap_collect(&rt.heap, NULL, 0u);
    CHECK(idm_heap_object_count(&rt.heap) == 0u);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_basic(void) {
    char *s = dump_reader("foo bar\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr foo bar))");
    free(s);

    s = dump_reader("foo (bar baz)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr foo (%-group (%-expr bar baz))))");
    free(s);

    s = dump_reader("[1 2 :ok] (list a b) {:ok 1}\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr [1 2 :ok] (%-group (%-expr list a b)) {:ok 1}))");
    free(s);

    s = dump_reader("i > count\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr i > count))");
    free(s);

    s = dump_reader("config:\n  retries: 3\n  nested:\n    flag: true\nname: app\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr %{:config %{:retries 3 :nested %{:flag true}} :name app}))");
    free(s);
}

static void test_transparent_reader_basic(void) {
    char *s = dump_transparent_reader("foo (bar baz) [1 :ok \"x\"]\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(foo (bar baz) [1 :ok \"x\"])");
    free(s);

    s = dump_transparent_reader("r\"abc\" regex\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(r \"abc\" regex)");
    free(s);
}

static IdmSyntax *copy_core_grammar_form(const IdmSyntax *terms, const char *name) {
    if (!terms || terms->kind != IDM_SYN_LIST) return NULL;
    for (size_t i = 0; i < terms->as.seq.count; i++) {
        const IdmSyntax *form = terms->as.seq.items[i];
        const IdmSyntax *grammar_name = i + 1u < terms->as.seq.count ? terms->as.seq.items[i + 1u] : NULL;
        if (form && form->kind == IDM_SYN_WORD && strcmp(form->as.text, "core-grammar") == 0 &&
            grammar_name && grammar_name->kind == IDM_SYN_WORD && strcmp(grammar_name->as.text, name) == 0 &&
            i + 3u < terms->as.seq.count) {
            IdmSyntax *copy = idm_syn_list(form->span);
            if (!copy) return NULL;
            for (size_t j = 0; j < 4u; j++) {
                IdmSyntax *item = idm_syn_clone(terms->as.seq.items[i + j]);
                if (!item || !idm_syn_append(copy, item)) {
                    idm_syn_free(item);
                    idm_syn_free(copy);
                    return NULL;
                }
            }
            return copy;
        }
    }
    return NULL;
}

static void test_kernel_bootstrap_iridium_quine(void) {
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *terms = NULL;
    CHECK(idm_reader_read_terms_file("std/kernel/bootstrap.id", &terms, &err));
    CHECK(terms != NULL && terms->kind == IDM_SYN_LIST);
    IdmSyntax *iridium_form = copy_core_grammar_form(terms, "Iridium");
    CHECK(iridium_form != NULL);
    IdmPkgGrammar grammar;
    CHECK(idm_pkg_grammar_from_ir(iridium_form, &grammar, &err));
    CHECK(grammar.name != NULL && strcmp(grammar.name, "Iridium") == 0);
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars(grammar.name, &grammar, 1u, &artifact, &err));
    IdmBuffer wire;
    idm_buf_init(&wire);
    CHECK(idm_reader_artifact_serialize(artifact, &wire, &err));
    IdmReaderArtifact *loaded = NULL;
    CHECK(idm_reader_artifact_deserialize((const unsigned char *)wire.data, wire.len, &loaded, &err));
    char *source = NULL;
    size_t source_len = 0;
    CHECK(idm_read_file("std/kernel/bootstrap.id", &source, &source_len, &err));
    IdmSyntax *readback = NULL;
    CHECK(idm_reader_read_artifact_string(loaded, "std/kernel/bootstrap.id", source, &readback, &err));
    IdmSyntax *expected = idm_syn_list(terms->span);
    CHECK(expected != NULL);
    CHECK(idm_syn_append(expected, idm_syn_word("%-package-begin", terms->span)));
    for (size_t i = 0; i < terms->as.seq.count; i++) {
        IdmSyntax *item = idm_syn_clone(terms->as.seq.items[i]);
        CHECK(item != NULL);
        CHECK(idm_syn_append(expected, item));
    }
    IdmBuffer got;
    IdmBuffer want;
    idm_buf_init(&got);
    idm_buf_init(&want);
    CHECK(idm_syn_dump(&got, readback));
    CHECK(idm_syn_dump(&want, expected));
    CHECK_STR(got.data, want.data);
    idm_buf_destroy(&got);
    idm_buf_destroy(&want);
    idm_syn_free(expected);
    idm_syn_free(readback);
    free(source);
    idm_buf_destroy(&wire);
    idm_reader_artifact_destroy(loaded);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);
    idm_syn_free(iridium_form);
    idm_syn_free(terms);
    idm_error_clear(&err);
}

static void test_bootstrap_source_reader_selection_is_data_driven(void) {
    const char *old = getenv("IDIOMROOT");
    char *saved = old ? idm_strdup(old) : NULL;
    CHECK(mkdir("build/source_select_std", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("build/source_select_std/kernel", 0755) == 0 || errno == EEXIST);
    CHECK(write_text_file("build/source_select_std/kernel/bootstrap.id",
        "core-grammar Idiom :exclusive [\n"
        "  {:skip space {:regex \" +\"}}\n"
        "  {:token nope {:literal \"nope\"} {:emit-word nope}}\n"
        "  {:form program {:capture item {:token nope}} {:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:capture item}]}]}}\n"
        "]\n"
        "\n"
        "core-reader :source Idiom\n"
        "\n"
        "core-grammar Pydiom :exclusive [\n"
        "  {:skip space {:regex \"[ \\t\\r\\n]+\"}}\n"
        "  {:token py {:literal \"py\"} {:emit-word py}}\n"
        "  {:token integer {:regex \"[0-9]+\"} {:capture integer}}\n"
        "  {:form program {:seq {:token py} {:capture value {:token integer}}} {:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:capture value}]}]}}\n"
        "]\n"
        "\n"
        "core-reader :source Pydiom\n"));
    setenv("IDIOMROOT", "build/source_select_std", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const IdmReaderArtifact *reader = NULL;
    bool got = idm_expand_source_reader(&rt, &reader, &err);
    if (!got) idm_error_fprint(stderr, &err);
    CHECK(got);
    if (got) {
        IdmReaderArtifactInfo info;
        memset(&info, 0, sizeof(info));
        CHECK(idm_reader_artifact_info(reader, &info));
        CHECK_STR(info.surface, "Pydiom");
        IdmSyntax *program = NULL;
        bool read = idm_reader_read_artifact_string(reader, "<pydiom-selection>", "py 42", &program, &err);
        if (!read) idm_error_fprint(stderr, &err);
        CHECK(read);
        if (read) {
            IdmBuffer dump;
            idm_buf_init(&dump);
            CHECK(idm_syn_dump(&dump, program));
            CHECK_STR(dump.data, "(%-package-begin (%-expr 42))");
            idm_buf_destroy(&dump);
            idm_syn_free(program);
        }
    }
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    if (saved) {
        setenv("IDIOMROOT", saved, 1);
        free(saved);
    } else {
        unsetenv("IDIOMROOT");
    }
}

static void test_surface_grammar_source_reader_artifact(void) {
    remove("build/surface_reader_pkg.ic");
    CHECK(write_text_file("build/surface_reader_pkg.id",
        "package surface_reader_pkg\n"
        "\n"
        "grammar SurfaceKernelReader exclusive do\n"
        "  skip space {:regex \"[ \\t\\r\\n]+\"}\n"
        "  token grammar_kw {:literal \"grammar\"} -> {:emit-word grammar}\n"
        "  token do_kw {:literal \"do\"} -> {:emit-word do}\n"
        "  token end_kw {:literal \"end\"} -> {:emit-word end}\n"
        "  token extend_kw {:literal \"extend\"} -> {:emit-word extend}\n"
        "  token ident {:regex \"[A-Za-z_][A-Za-z0-9_]*\"} -> {:capture ident}\n"
        "  form grammar_decl {:seq {:token grammar_kw} {:capture name {:token ident}} {:capture mode {:token extend_kw}} {:token do_kw} {:token end_kw}} -> {:emit-form \"%-expr\" [{:emit-word grammar} {:capture name} {:capture mode} {:emit-form \"%-body\" []}]}\n"
        "  form program {:capture decl {:ref grammar_decl}} -> {:emit-form \"%-package-begin\" [{:capture decl}]}\n"
        "end\n"
        "\n"
        "core-reader :source SurfaceKernelReader\n"));
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBuffer blob;
    idm_buf_init(&blob);
    bool serialized = idm_expand_package_artifact_serialize(&rt, "build/surface_reader_pkg", &blob, &err);
    if (!serialized) idm_error_fprint(stderr, &err);
    CHECK(serialized);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    bool loaded = serialized && idm_artifact_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &art, &err);
    if (!loaded) idm_error_fprint(stderr, &err);
    CHECK(loaded);
    if (loaded) {
        CHECK(art.source_reader != NULL);
        if (art.source_reader) CHECK_STR(art.source_reader, "SurfaceKernelReader");
    }
    IdmReaderArtifact *reader = NULL;
    bool built = loaded && idm_reader_artifact_from_grammars(art.source_reader, art.grammars, art.grammar_count, &reader, &err);
    if (!built) idm_error_fprint(stderr, &err);
    CHECK(built);
    IdmSyntax *program = NULL;
    bool read = reader && idm_reader_read_artifact_string(reader, "<surface-kernel-reader>", "grammar NextReader extend do end", &program, &err);
    if (!read) idm_error_fprint(stderr, &err);
    CHECK(read);
    if (read) {
        IdmBuffer dump;
        idm_buf_init(&dump);
        CHECK(idm_syn_dump(&dump, program));
        CHECK_STR(dump.data, "(%-package-begin (%-expr grammar NextReader extend (%-body)))");
        idm_buf_destroy(&dump);
    }
    idm_syn_free(program);
    idm_reader_artifact_destroy(reader);
    if (loaded) idm_artifact_destroy(&art);
    idm_buf_destroy(&blob);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_artifact(void) {
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("GateReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 7u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_skip_rule(&grammar.rules[0], "space", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, " +", "{:regex \" +\"}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[1], "ident", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[A-Za-z_][A-Za-z0-9_]*", "{:regex \"[A-Za-z_][A-Za-z0-9_]*\"}", "{:capture ident}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[2], "integer", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[0-9]+", "{:regex \"[0-9]+\"}", "{:capture integer}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[3], "equals", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "=", "{:regex \"=\"}", "{:emit-atom equals}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[4], "arrow", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "=>", "{:literal \"=>\"}", "{:emit-atom arrow}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[5], "pair", "{:seq {:capture left {:ref ident}} {:token arrow} {:capture right {:ref integer}}}", "{:emit-form \"%-expr\" [{:emit-word pair} {:capture left} {:capture right}]}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[6], "program", "{:capture item {:ref pair}}", "{:emit-form \"%-package-begin\" [{:capture item}]}"));

    IdmError err;
    idm_error_init(&err);
    IdmReaderArtifact *artifact = NULL;
    bool built = idm_reader_artifact_from_grammars("GateReader", &grammar, 1u, &artifact, &err);
    if (!built) idm_error_fprint(stderr, &err);
    CHECK(built);
    CHECK(artifact != NULL);
    IdmReaderArtifactInfo info;
    memset(&info, 0, sizeof(info));
    CHECK(idm_reader_artifact_info(artifact, &info));
    CHECK_STR(info.surface, "GateReader");
    CHECK(info.phase == 0);
    CHECK(info.format_version == 1u);
    CHECK(info.compiler_version == 1u);
    CHECK(info.mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE);
    CHECK(info.contributor_count == 1u);
    CHECK(info.token_count == 4u);
    CHECK(info.form_count == 2u);
    CHECK(info.skip_count == 1u);
    IdmReaderArtifactContributorInfo contributor;
    memset(&contributor, 0, sizeof(contributor));
    CHECK(idm_reader_artifact_contributor_info(artifact, 0, &contributor));
    CHECK(contributor.provider != NULL && contributor.provider[0] == '\0');
    CHECK(contributor.provider_key != NULL && contributor.provider_key[0] == '\0');
    CHECK(contributor.mode == (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE);
    CHECK(contributor.first_rule_order == 0u);
    CHECK(contributor.rule_count == grammar.rule_count);

    IdmSyntax *program = NULL;
    bool read = artifact && idm_reader_read_artifact_string(artifact, "<reader-artifact-input>", "alpha => 42", &program, &err);
    if (!read) idm_error_fprint(stderr, &err);
    CHECK(read);
    CHECK(program != NULL);
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr pair alpha 42))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_error_clear(&err);
    idm_pkg_grammar_destroy(&grammar);
}

static void test_reader_artifact_terminal_captures(void) {
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("CaptureReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 5u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_skip_rule(&grammar.rules[0], "space", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, " +", "{:regex \" +\"}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[1], "atom", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, ":(?<atom_name>[A-Za-z_][A-Za-z0-9_]*)", "{:regex \":(?<atom_name>[A-Za-z_][A-Za-z0-9_]*)\"}", "{:capture-atom atom_name}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[2], "word", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "@(?<word_name>[A-Za-z_][A-Za-z0-9_]*)", "{:regex \"@(?<word_name>[A-Za-z_][A-Za-z0-9_]*)\"}", "{:capture-word word_name}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[3], "string", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "#(?<string_text>[A-Za-z_][A-Za-z0-9_]*)", "{:regex \"#(?<string_text>[A-Za-z_][A-Za-z0-9_]*)\"}", "{:capture-string string_text}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[4], "program", "{:seq {:capture a {:token atom}} {:capture w {:token word}} {:capture s {:token string}}}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:capture a} {:capture w} {:capture s}]}]}"));

    IdmError err;
    idm_error_init(&err);
    IdmReaderArtifact *artifact = NULL;
    bool built = idm_reader_artifact_from_grammars("CaptureReader", &grammar, 1u, &artifact, &err);
    if (!built) idm_error_fprint(stderr, &err);
    CHECK(built);

    IdmSyntax *program = NULL;
    bool read = artifact && idm_reader_read_artifact_string(artifact, "<capture-reader>", ":ok @name #text", &program, &err);
    if (!read) idm_error_fprint(stderr, &err);
    CHECK(read);
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr :ok name \"text\"))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_error_clear(&err);
    idm_pkg_grammar_destroy(&grammar);
}

static void test_reader_artifact_metadata_reductions(void) {
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("MetadataReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 9u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_skip_rule(&grammar.rules[0], "space", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[ ]+", "{:regex \"[ ]+\"}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[1], "newline", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "\n", "{:literal \"\\n\"}", "{:emit-atom newline}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[2], "at", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "@", "{:literal \"@\"}", "{:emit-atom at}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[3], "bang", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "!", "{:literal \"!\"}", "{:emit-atom bang}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[4], "word", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[A-Za-z]+", "{:regex \"[A-Za-z]+\"}", "{:capture word}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[5], "continued", "{:seq {:capture first {:ref word}} {:token newline} {:indent-gt first {:capture second {:ref word}}}}", "{:emit-form \"%-expr\" [{:capture first} {:capture second}]}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[6], "adjacent", "{:seq {:capture left {:token at}} {:adjacent {:capture right {:token bang}}}}", "{:emit-form \"%-expr\" [{:capture left} {:capture right}]}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[7], "separated", "{:seq {:capture left {:token at}} {:not-adjacent {:capture right {:token bang}}}}", "{:emit-form \"%-expr\" [{:capture left} {:capture right}]}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[8], "program", "{:alt {:capture item {:ref continued}} {:capture item {:ref adjacent}} {:capture item {:ref separated}}}", "{:emit-form \"%-package-begin\" [{:capture item}]}"));

    IdmError err;
    idm_error_init(&err);
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars(grammar.name, &grammar, 1u, &artifact, &err));
    IdmSyntax *program = NULL;
    CHECK(idm_reader_read_artifact_string(artifact, "<metadata-reader>", "foo\n  bar", &program, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr foo bar))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    program = NULL;
    CHECK(!idm_reader_read_artifact_string(artifact, "<metadata-reader>", "foo\nbar", &program, &err));
    idm_error_clear(&err);
    CHECK(idm_reader_read_artifact_string(artifact, "<metadata-reader>", "@!", &program, &err));
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr (%-adjacent :at :bang)))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    program = NULL;
    CHECK(idm_reader_read_artifact_string(artifact, "<metadata-reader>", "@ !", &program, &err));
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr :at :bang))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);
}

static void test_reader_artifact_literal_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    idm_sha256("reader-literal-roundtrip", 24u, art.src_hash);
    art.grammar_count = 1u;
    art.grammars = calloc(art.grammar_count, sizeof(*art.grammars));
    CHECK(art.grammars != NULL);
    IdmPkgGrammar *grammar = &art.grammars[0];
    grammar->name = idm_strdup("LiteralReader");
    art.source_reader = idm_strdup("LiteralReader");
    grammar->mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar->rule_count = 2u;
    grammar->rules = calloc(grammar->rule_count, sizeof(*grammar->rules));
    CHECK(grammar->name != NULL);
    CHECK(art.source_reader != NULL);
    CHECK(grammar->rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar->rules[0], "x", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "x", "{:literal \"x\"}", "{:literal {:tag [1 \"two\"]}}"));
    CHECK(test_reader_artifact_form_rule(&grammar->rules[1], "program", "{:literal {:tag [1 \"two\"]}}", "{:emit-form \"%-package-begin\" [{:literal {:out [1 \"two\"]}}]}"));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_artifact_serialize(&art, &blob, &err));
    CHECK(!err.present);
    idm_artifact_destroy(&art);
    IdmArtifact back;
    CHECK(idm_artifact_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &back, &err));
    CHECK(!err.present);
    CHECK(back.grammar_count == 1u);
    CHECK_STR(back.source_reader, "LiteralReader");
    IdmReaderArtifact *reader = NULL;
    CHECK(idm_reader_artifact_from_grammars(back.source_reader, back.grammars, back.grammar_count, &reader, &err));
    IdmSyntax *program = NULL;
    CHECK(idm_reader_read_artifact_string(reader, "<literal-reader>", "x", &program, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin {:out [1 \"two\"]})");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(reader);
    idm_artifact_destroy(&back);
    idm_buf_destroy(&blob);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_artifact_binary_roundtrip(void) {
    IdmError err;
    idm_error_init(&err);
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("BinaryReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "x", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "x", "{:literal \"x\"}", "{:emit-atom ok}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "program", "{:token x}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:emit-atom ok}]}]}"));
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    CHECK(idm_scope_set_add(&scopes, 33u));
    IdmReaderGrammarSource source = {
        .grammar = &grammar,
        .provider = "binary-provider",
        .provider_key = "binary-key",
        .binding_scopes = &scopes,
        .phase = 0,
    };
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_sources("BinaryReader", &source, 1u, &artifact, &err));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_reader_artifact_serialize(artifact, &blob, &err));
    idm_reader_artifact_destroy(artifact);
    artifact = NULL;
    CHECK(idm_reader_artifact_deserialize((const unsigned char *)blob.data, blob.len, &artifact, &err));
    IdmReaderArtifactInfo info;
    CHECK(idm_reader_artifact_info(artifact, &info));
    CHECK_STR(info.surface, "BinaryReader");
    CHECK(info.contributor_count == 1u);
    IdmReaderArtifactContributorInfo contributor;
    CHECK(idm_reader_artifact_contributor_info(artifact, 0u, &contributor));
    CHECK_STR(contributor.provider, "binary-provider");
    CHECK_STR(contributor.provider_key, "binary-key");
    CHECK(contributor.binding_scopes && idm_scope_set_contains(contributor.binding_scopes, 33u));
    IdmSyntax *program = NULL;
    CHECK(idm_reader_read_artifact_string(artifact, "<binary-reader>", "x", &program, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr :ok))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_buf_destroy(&blob);
    idm_scope_set_destroy(&scopes);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);
}

static void test_reader_artifact_binary_preserves_terminal_descriptors(void) {
    IdmError err;
    idm_error_init(&err);
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("MachineReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "q", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "@", "{:regex \"@\"}", "{:emit-atom hit}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "program", "{:token q}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:emit-atom hit}]}]}"));
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars("MachineReader", &grammar, 1u, &artifact, &err));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_reader_artifact_serialize(artifact, &blob, &err));
    idm_reader_artifact_destroy(artifact);
    artifact = NULL;
    const unsigned char *bytes = (const unsigned char *)blob.data;
    bool framed_descriptor = false;
    for (size_t i = 0; i + 5u <= blob.len; i++) {
        if (bytes[i] == 0u && bytes[i + 1u] == 0u && bytes[i + 2u] == 0u && bytes[i + 3u] == 1u && bytes[i + 4u] == '@') {
            framed_descriptor = true;
            break;
        }
    }
    CHECK(framed_descriptor);
    CHECK(idm_reader_artifact_deserialize((const unsigned char *)blob.data, blob.len, &artifact, &err));
    IdmSyntax *program = NULL;
    CHECK(idm_reader_read_artifact_string(artifact, "<machine-reader>", "@", &program, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr :hit))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_buf_destroy(&blob);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);
}

static void test_reader_artifact_literal_drops_declaration_metadata(void) {
    IdmError err;
    idm_error_init(&err);
    IdmSpan decl_span = idm_span_unknown("decl.id");
    IdmSyntax *decl_literal = idm_syn_word("declared", decl_span);
    CHECK(decl_literal != NULL);
    CHECK(idm_syn_scope_add(decl_literal, 0, 99u));
    CHECK(idm_syn_property_set(decl_literal, "leak", "no"));
    CHECK(idm_syn_origin_push(decl_literal, "declaration-origin"));
    idm_syn_set_token(decl_literal, "declared", true, false);
    IdmSyntax *ctor_literal_ir = idm_syn_tuple(decl_span);
    CHECK(ctor_literal_ir != NULL);
    CHECK(idm_syn_append(ctor_literal_ir, idm_syn_atom("literal", decl_span)));
    CHECK(idm_syn_append(ctor_literal_ir, decl_literal));
    IdmSyntax *args = idm_syn_vector(decl_span);
    CHECK(args != NULL);
    CHECK(idm_syn_append(args, ctor_literal_ir));
    IdmSyntax *ctor = idm_syn_tuple(decl_span);
    CHECK(ctor != NULL);
    CHECK(idm_syn_append(ctor, idm_syn_atom("emit-form", decl_span)));
    CHECK(idm_syn_append(ctor, idm_syn_string("%-package-begin", decl_span)));
    CHECK(idm_syn_append(ctor, args));
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("MetadataLiteralReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "x", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "x", "{:literal \"x\"}", "{:capture x}"));
    IdmGrammarRule *program_rule = &grammar.rules[1];
    program_rule->name = idm_strdup("program");
    program_rule->kind = (uint8_t)IDM_GRAMMAR_RULE_FORM;
    IdmSyntax *pattern = test_read_one_term("{:token x}");
    CHECK(program_rule->name != NULL);
    CHECK(pattern != NULL);
    CHECK(idm_reader_pattern_compile_ir(pattern, &program_rule->pattern, &err));
    CHECK(idm_reader_ctor_compile_ir(ctor, &program_rule->constructor, &err));
    idm_syn_free(pattern);
    idm_syn_free(ctor);
    IdmReaderArtifact *reader = NULL;
    CHECK(idm_reader_artifact_from_grammars("MetadataLiteralReader", &grammar, 1u, &reader, &err));
    IdmSyntax *program = NULL;
    CHECK(idm_reader_read_artifact_string(reader, "<metadata-literal-reader>", "x", &program, &err));
    CHECK(program != NULL && program->kind == IDM_SYN_LIST && program->as.seq.count == 2u);
    IdmSyntax *emitted = program->as.seq.items[1];
    CHECK(emitted->kind == IDM_SYN_WORD && strcmp(emitted->as.text, "declared") == 0);
    CHECK(!idm_syn_scope_contains(emitted, 0, 99u));
    CHECK(idm_syn_property_get(emitted, "leak") == NULL);
    CHECK(emitted->origins.count == 0);
    CHECK(emitted->token_raw == NULL);
    CHECK(emitted->span.file != NULL && strcmp(emitted->span.file, "<metadata-literal-reader>") == 0);
    idm_syn_free(program);
    idm_reader_artifact_destroy(reader);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);
}

static void test_reader_artifact_rejects_nullable_terminals(void) {
    IdmError err;
    idm_error_init(&err);
    IdmGrammarTerminal terminal;
    memset(&terminal, 0, sizeof(terminal));
    IdmSyntax *pattern = test_read_one_term("{:regex \"a*\"}");
    CHECK(pattern != NULL);
    CHECK(!idm_grammar_terminal_from_ir(pattern, &terminal, &err));
    CHECK(err.present && err.message && strstr(err.message, "empty input") != NULL);
    idm_grammar_terminal_destroy(&terminal);
    idm_syn_free(pattern);
    idm_error_clear(&err);

    pattern = test_read_one_term("{:literal \"\"}");
    CHECK(pattern != NULL);
    CHECK(!idm_grammar_terminal_from_ir(pattern, &terminal, &err));
    CHECK(err.present && err.message && strstr(err.message, "empty") != NULL);
    idm_grammar_terminal_destroy(&terminal);
    idm_syn_free(pattern);
    idm_error_clear(&err);
}

static void test_reader_artifact_literal_depth_guard(void) {
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<reader-literal-depth>");
    IdmSyntax *root = idm_syn_list(span);
    CHECK(root != NULL);
    IdmSyntax *cursor = root;
    for (unsigned i = 0; i <= IDM_IC_MAX_DEPTH + 1u; i++) {
        IdmSyntax *child = idm_syn_list(span);
        CHECK(child != NULL);
        CHECK(idm_syn_append(cursor, child));
        cursor = child;
    }
    IdmReaderLiteral literal;
    memset(&literal, 0, sizeof(literal));
    CHECK(!idm_reader_literal_from_syntax(&literal, root, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    idm_reader_literal_destroy(&literal);
    idm_syn_free(root);
    idm_error_clear(&err);
}

static void test_active_grammar_reader_artifact_freeze(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *source = "use tests/pkg/coregrammar_skip\nactivate GrammarSkip\n:ok\n";
    IdmSyntax *program = NULL;
    bool read_setup = idm_expand_read_source_string(&rt, "<active-reader-artifact-setup>", source, &program, &err);
    if (!read_setup) idm_error_fprint(stderr, &err);
    CHECK(read_setup);
    ExpandContext ctx;
    ctx_init(&ctx, &rt);
    IdmCore *core = NULL;
    IdmReaderArtifact *artifact = NULL;
    IdmSyntax *read = NULL;
    bool hooks = false;
    SavedHooks saved;
    memset(&saved, 0, sizeof(saved));
    if (program) {
        IdmBuffer ser;
        idm_buf_init(&ser);
        bool serialized = idm_syn_serialize(&ser, program, &err);
        CHECK(serialized);
        unsigned char unit_hash[32];
        idm_sha256(ser.data ? ser.data : "", ser.len, unit_hash);
        idm_buf_destroy(&ser);
        ctx_set_unit(&ctx, "<active-reader-artifact-setup>", unit_hash);
        IdmEnv *phase_runtime_env = idm_fresh_phase_runtime_env(&rt, &err);
        CHECK(phase_runtime_env != NULL);
        ctx.phase_env = phase_runtime_env ? idm_phase_env_create(&rt, phase_runtime_env) : NULL;
        CHECK(ctx.phase_env != NULL);
        ctx.runner = &ctx.local_runner;
        hooks_install(&rt, &ctx, &saved);
        hooks = true;
        bool seeded = ctx.phase_env && ctx_seed(&ctx, &err) && ctx_activate_kernel(&ctx, &err);
        if (!seeded) idm_error_fprint(stderr, &err);
        CHECK(seeded);
        if (seeded) {
            core = expand_body_items(&ctx, program->as.seq.items, 1u, program->as.seq.count, false, &err);
            if (!core) idm_error_fprint(stderr, &err);
            CHECK(core != NULL);
        }
        bool frozen = core && ctx_reader_artifact_from_active_grammars(&ctx, "ReaderSkip", &artifact, &err);
        if (!frozen) idm_error_fprint(stderr, &err);
        CHECK(frozen);
        bool reader_artifact_read = artifact && idm_reader_read_artifact_string(artifact, "<active-reader-artifact>", "alpha", &read, &err);
        if (!reader_artifact_read) idm_error_fprint(stderr, &err);
        CHECK(reader_artifact_read);
        if (read) {
            IdmBuffer dump;
            idm_buf_init(&dump);
            CHECK(idm_syn_dump(&dump, read));
            CHECK_STR(dump.data, "(%-package-begin (%-expr alpha))");
            idm_buf_destroy(&dump);
        }
    }
    if (hooks) hooks_restore(&rt, &saved);
    idm_syn_free(read);
    idm_reader_artifact_destroy(artifact);
    idm_core_free(core);
    ctx_destroy(&ctx);
    idm_syn_free(program);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_artifact_rejects_bad_reductions(void) {
    IdmError err;
    idm_error_init(&err);
    IdmReaderArtifact *artifact = NULL;
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("BadReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "ident", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[A-Za-z_][A-Za-z0-9_]*", "{:regex \"[A-Za-z_][A-Za-z0-9_]*\"}", "{:capture ident}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "program", "{:ref missing}", "{:emit-form \"%-package-begin\" [{:emit-word bad}]}"));
    CHECK(!idm_reader_artifact_from_grammars("BadReader", &grammar, 1u, &artifact, &err));
    CHECK(err.present && err.message && strstr(err.message, "unknown rule 'missing'") != NULL);
    idm_error_clear(&err);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);

    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("LoopReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 3u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "ident", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[A-Za-z_][A-Za-z0-9_]*", "{:regex \"[A-Za-z_][A-Za-z0-9_]*\"}", "{:capture ident}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "loop", "{:ref loop}", "{:emit-form \"%-expr\" [{:emit-word loop}]}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[2], "program", "{:ref loop}", "{:emit-form \"%-package-begin\" [{:emit-word bad}]}"));
    artifact = NULL;
    CHECK(!idm_reader_artifact_from_grammars("LoopReader", &grammar, 1u, &artifact, &err));
    CHECK(err.present && err.message && strstr(err.message, "left-recursive") != NULL);
    idm_error_clear(&err);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);

    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("NullableRepeatReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "ident", (uint8_t)IDM_GRAMMAR_TERMINAL_REGEX, "[A-Za-z_][A-Za-z0-9_]*", "{:regex \"[A-Za-z_][A-Za-z0-9_]*\"}", "{:capture ident}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "program", "{:seq {:repeat {:optional {:token ident}}}}", "{:emit-form \"%-package-begin\" [{:emit-atom ok}]}"));
    artifact = NULL;
    CHECK(!idm_reader_artifact_from_grammars("NullableRepeatReader", &grammar, 1u, &artifact, &err));
    CHECK(err.present && err.message && strstr(err.message, "can match empty input") != NULL);
    idm_error_clear(&err);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);
}

static void test_reader_artifact_rejects_mixed_scopes(void) {
    IdmError err;
    idm_error_init(&err);
    IdmPkgGrammar grammars[2];
    memset(grammars, 0, sizeof(grammars));
    IdmScopeSet scopes_a;
    IdmScopeSet scopes_b;
    idm_scope_set_init(&scopes_a);
    idm_scope_set_init(&scopes_b);
    CHECK(idm_scope_set_add(&scopes_a, 11));
    CHECK(idm_scope_set_add(&scopes_b, 22));
    for (size_t i = 0; i < 2u; i++) {
        grammars[i].name = idm_strdup("ScopedReader");
        grammars[i].mode = (uint8_t)IDM_GRAMMAR_MODE_EXTEND;
        grammars[i].rule_count = 1u;
        grammars[i].rules = calloc(1u, sizeof(*grammars[i].rules));
        CHECK(grammars[i].name != NULL);
        CHECK(grammars[i].rules != NULL);
    }
    CHECK(test_reader_artifact_token_rule(&grammars[0].rules[0], "a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}", "{:capture a}"));
    CHECK(test_reader_artifact_token_rule(&grammars[1].rules[0], "b", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "b", "{:literal \"b\"}", "{:capture b}"));
    IdmReaderGrammarSource sources[2];
    memset(sources, 0, sizeof(sources));
    sources[0].grammar = &grammars[0];
    sources[0].binding_scopes = &scopes_a;
    sources[1].grammar = &grammars[1];
    sources[1].binding_scopes = &scopes_b;
    IdmReaderArtifact *artifact = NULL;
    CHECK(!idm_reader_artifact_from_sources("ScopedReader", sources, 2u, &artifact, &err));
    CHECK(err.present && err.message && strstr(err.message, "multiple binding scopes") != NULL);
    idm_reader_artifact_destroy(artifact);
    idm_error_clear(&err);
    for (size_t i = 0; i < 2u; i++) idm_pkg_grammar_destroy(&grammars[i]);
    idm_scope_set_destroy(&scopes_a);
    idm_scope_set_destroy(&scopes_b);
}

static void test_reader_artifact_rejects_mixed_phases(void) {
    IdmError err;
    idm_error_init(&err);
    IdmPkgGrammar grammars[2];
    memset(grammars, 0, sizeof(grammars));
    IdmScopeSet scopes;
    idm_scope_set_init(&scopes);
    CHECK(idm_scope_set_add(&scopes, 33));
    for (size_t i = 0; i < 2u; i++) {
        grammars[i].name = idm_strdup("PhaseReader");
        grammars[i].mode = (uint8_t)IDM_GRAMMAR_MODE_EXTEND;
        grammars[i].rule_count = 1u;
        grammars[i].rules = calloc(1u, sizeof(*grammars[i].rules));
        CHECK(grammars[i].name != NULL);
        CHECK(grammars[i].rules != NULL);
    }
    CHECK(test_reader_artifact_token_rule(&grammars[0].rules[0], "a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}", "{:capture a}"));
    CHECK(test_reader_artifact_token_rule(&grammars[1].rules[0], "b", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "b", "{:literal \"b\"}", "{:capture b}"));
    IdmReaderGrammarSource sources[2];
    memset(sources, 0, sizeof(sources));
    sources[0].grammar = &grammars[0];
    sources[0].binding_scopes = &scopes;
    sources[0].phase = 0;
    sources[1].grammar = &grammars[1];
    sources[1].binding_scopes = &scopes;
    sources[1].phase = 1;
    IdmReaderArtifact *artifact = NULL;
    CHECK(!idm_reader_artifact_from_sources("PhaseReader", sources, 2u, &artifact, &err));
    CHECK(err.present && err.message && strstr(err.message, "multiple phases") != NULL);
    idm_reader_artifact_destroy(artifact);
    idm_error_clear(&err);
    for (size_t i = 0; i < 2u; i++) idm_pkg_grammar_destroy(&grammars[i]);
    idm_scope_set_destroy(&scopes);
}

static void test_reader_artifact_terminal_priority(void) {
    IdmError err;
    idm_error_init(&err);
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("PriorityReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 3u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}", "{:emit-atom ok}"));
    CHECK(test_reader_artifact_skip_rule(&grammar.rules[1], "skip_a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[2], "program", "{:token a}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:emit-atom ok}]}]}"));
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars("PriorityReader", &grammar, 1u, &artifact, &err));
    IdmSyntax *program = NULL;
    bool read = artifact && idm_reader_read_artifact_string(artifact, "<priority-reader>", "a", &program, &err);
    if (!read) idm_error_fprint(stderr, &err);
    CHECK(read);
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_syn_dump(&dump, program));
    CHECK_STR(dump.data, "(%-package-begin (%-expr :ok))");
    idm_buf_destroy(&dump);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);

    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("PriorityReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 3u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_skip_rule(&grammar.rules[0], "skip_a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}"));
    CHECK(test_reader_artifact_token_rule(&grammar.rules[1], "a", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "a", "{:literal \"a\"}", "{:emit-atom ok}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[2], "program", "{:token a}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:emit-atom ok}]}]}"));
    artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars("PriorityReader", &grammar, 1u, &artifact, &err));
    program = NULL;
    CHECK(!idm_reader_read_artifact_string(artifact, "<priority-reader>", "a", &program, &err));
    CHECK(err.present && err.message && strstr(err.message, "did not match program") != NULL);
    idm_syn_free(program);
    idm_reader_artifact_destroy(artifact);
    idm_pkg_grammar_destroy(&grammar);
    idm_error_clear(&err);
}

static void test_expand_reader_artifact_string_entrypoint(void) {
    IdmPkgGrammar grammar;
    memset(&grammar, 0, sizeof(grammar));
    grammar.name = idm_strdup("EvalReader");
    grammar.mode = (uint8_t)IDM_GRAMMAR_MODE_EXCLUSIVE;
    grammar.rule_count = 2u;
    grammar.rules = calloc(grammar.rule_count, sizeof(*grammar.rules));
    CHECK(grammar.name != NULL);
    CHECK(grammar.rules != NULL);
    CHECK(test_reader_artifact_token_rule(&grammar.rules[0], "ok", (uint8_t)IDM_GRAMMAR_TERMINAL_LITERAL, "ok", "{:literal \"ok\"}", "{:emit-atom ok}"));
    CHECK(test_reader_artifact_form_rule(&grammar.rules[1], "program", "{:token ok}", "{:emit-form \"%-package-begin\" [{:emit-form \"%-expr\" [{:emit-atom ok}]}]}"));
    IdmError err;
    idm_error_init(&err);
    IdmReaderArtifact *artifact = NULL;
    CHECK(idm_reader_artifact_from_grammars("EvalReader", &grammar, 1u, &artifact, &err));
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmCore *core = NULL;
    bool expanded = artifact && idm_expand_reader_artifact_string(&rt, artifact, "<reader-artifact-expand>", "ok", &core, &err);
    if (!expanded) idm_error_fprint(stderr, &err);
    CHECK(expanded);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(core && idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_value_write(&dump, out));
    CHECK_STR(dump.data, ":ok");
    idm_buf_destroy(&dump);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_runtime_destroy(&rt);
    idm_reader_artifact_destroy(artifact);
    idm_error_clear(&err);
    idm_pkg_grammar_destroy(&grammar);
}

static void test_pythonish_reader_artifact_over_language(void) {
    IdmError err;
    idm_error_init(&err);
    const char *old_cache = getenv("IDIOMCACHE");
    char *saved_cache = old_cache ? idm_strdup(old_cache) : NULL;
    setenv("IDIOMCACHE", "0", 1);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmBuffer blob;
    idm_buf_init(&blob);
    bool serialized = idm_expand_package_artifact_serialize(&rt, "tests/pkg/pydiom_reader", &blob, &err);
    if (!serialized) idm_error_fprint(stderr, &err);
    CHECK(serialized);
    IdmArtifact art;
    memset(&art, 0, sizeof(art));
    bool loaded = serialized && idm_artifact_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &art, &err);
    if (!loaded) idm_error_fprint(stderr, &err);
    CHECK(loaded);
    CHECK(art.source_reader != NULL);
    if (art.source_reader) CHECK_STR(art.source_reader, "PythonishReader");
    IdmReaderArtifact *artifact = NULL;
    CHECK(loaded && idm_reader_artifact_from_grammars(art.source_reader, art.grammars, art.grammar_count, &artifact, &err));
    const char *source = "block:\n  answer = 40\n  answer + 2\n";
    IdmSyntax *program = NULL;
    bool read = artifact && idm_reader_read_artifact_string(artifact, "<pythonish-reader>", source, &program, &err);
    if (!read) idm_error_fprint(stderr, &err);
    CHECK(read);
    IdmBuffer syntax_dump;
    idm_buf_init(&syntax_dump);
    CHECK(idm_syn_dump(&syntax_dump, program));
    CHECK_STR(syntax_dump.data, "(%-package-begin (%-expr (%-body (%-expr answer = 40) (%-expr answer + 2))))");
    idm_buf_destroy(&syntax_dump);
    idm_syn_free(program);
    IdmCore *core = NULL;
    bool expanded = artifact && idm_expand_reader_artifact_string(&rt, artifact, "<pythonish-reader>", source, &core, &err);
    if (!expanded) idm_error_fprint(stderr, &err);
    CHECK(expanded);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(core && idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    IdmBuffer value_dump;
    idm_buf_init(&value_dump);
    CHECK(idm_value_write(&value_dump, out));
    CHECK_STR(value_dump.data, "42");
    idm_buf_destroy(&value_dump);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_reader_artifact_destroy(artifact);
    idm_artifact_destroy(&art);
    idm_buf_destroy(&blob);
    idm_runtime_destroy(&rt);
    idm_error_clear(&err);
    if (saved_cache) {
        setenv("IDIOMCACHE", saved_cache, 1);
        free(saved_cache);
    } else {
        unsetenv("IDIOMCACHE");
    }
}

static void test_reader_shell_protocols(void) {
    char *s = dump_reader("grep -c *.c > out.txt\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr grep (%-word \"-c\") (%-word \"*.c\") > (%-adjacent out . txt)))");
    free(s);

    s = dump_reader("wc $n - 1\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr wc (%-word \"$n\") - 1))");
    free(s);

    s = dump_reader("echo $(printf hi)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr echo (%-adjacent (%-word \"$\") (%-group (%-expr printf hi)))))");
    free(s);

    s = dump_reader("cat <(echo hi)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr cat (%-adjacent < (%-group (%-expr echo hi)))))");
    free(s);
}

static void test_reader_quote(void) {
    char *s = dump_reader("'(foo bar) %`(add %,x 1)\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr (%-quote (%-group (%-expr foo bar))) (%-quasisyntax (%-group (%-expr add (%-unsyntax x) 1)))))");
    free(s);
}

static void test_reader_body_and_function_value(void) {
    char *s = dump_reader("match value do\n  {:ok x} -> send &handler x\nend\n");
    CHECK(s != NULL);
    CHECK_STR(s, "(%-package-begin (%-expr match value (%-body (%-expr {:ok x} -> send (%-expression &handler) x))))");
    free(s);
}

static void test_bytecode_verifier(void) {
    IdmBytecodeModule module;
    idm_bc_init(&module);
    IdmError err;
    idm_error_init(&err);
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(test_emit_load_const(&module, 0, 99));
    CHECK(test_emit_return(&module, 0));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 1));
    CHECK(!idm_bc_verify(&module, &err));
    CHECK(err.present);
    idm_error_clear(&err);
    idm_bc_destroy(&module);

    idm_bc_init(&module);
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(test_emit_move(&module, 1, 0));
    CHECK(test_emit_return(&module, 0));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 1));
    CHECK(!idm_bc_verify(&module, &err));
    CHECK(err.present);
    CHECK(err.message && strstr(err.message, "MOVE destination register r1 out of bounds"));
    idm_error_clear(&err);
    idm_bc_destroy(&module);
}

static void test_vm_call_and_locals(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t add_prim = 0;
    uint32_t sub_prim = 0;
    uint32_t add2 = 0;
    uint32_t main_fn = 0;
    CHECK(add_primitive_clause_fn(&module, IDM_PRIM_ADD, &add_prim));
    CHECK(add_primitive_clause_fn(&module, IDM_PRIM_SUB, &sub_prim));
    CHECK(idm_bc_add_function(&module, "add2", 2, 0, 0, &add2));
    CHECK(idm_bc_add_function(&module, "main", 0, 1, 0, &main_fn));
    CHECK(idm_bc_set_function_entry(&module, add2, module.code_count));
    CHECK(test_emit_make_closure(&module, 2, add_prim));
    CHECK(emit_call(&module, 3, 2, 0, 2, false));
    CHECK(test_emit_return(&module, 3));
    CHECK(idm_bc_set_function_register_count(&module, add2, 4));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c20 = 0;
    uint32_t c22 = 0;
    uint32_t c1 = 0;
    CHECK(idm_bc_add_const(&module, idm_int(20), &c20));
    CHECK(idm_bc_add_const(&module, idm_int(22), &c22));
    CHECK(idm_bc_add_const(&module, idm_int(1), &c1));
    CHECK(test_emit_make_closure(&module, 1, add2));
    CHECK(test_emit_load_const(&module, 2, c20));
    CHECK(test_emit_load_const(&module, 3, c22));
    CHECK(emit_call(&module, 0, 1, 2, 2, false));
    CHECK(test_emit_make_closure(&module, 1, sub_prim));
    CHECK(test_emit_move(&module, 2, 0));
    CHECK(test_emit_load_const(&module, 3, c1));
    CHECK(emit_call(&module, 4, 1, 2, 2, false));
    CHECK(test_emit_return(&module, 4));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 5));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 41);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_vm_tail_call_reuses_frame(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t id_fn = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_function(&module, "id", 1, 0, 0, &id_fn));
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_set_function_entry(&module, id_fn, module.code_count));
    CHECK(test_emit_return(&module, 0));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    uint32_t c99 = 0;
    CHECK(idm_bc_add_const(&module, idm_int(99), &c99));
    CHECK(test_emit_make_closure(&module, 0, id_fn));
    CHECK(test_emit_load_const(&module, 1, c99));
    CHECK(emit_call(&module, 2, 0, 1, 1, true));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 3));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmVmLimits limits = idm_vm_default_limits();
    limits.max_frames = 1;
    IdmValue out = idm_nil();
    CHECK(idm_vm_run_limited(&rt, &module, main_fn, limits, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 99);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_vm_guard_error_is_clause_failure(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t lt_prim = 0;
    uint32_t guarded = 0;
    uint32_t fallback = 0;
    uint32_t guard = 0;
    uint32_t main_fn = 0;
    CHECK(add_primitive_clause_fn(&module, IDM_PRIM_LT, &lt_prim));
    CHECK(idm_bc_add_function(&module, "guarded", 1, 0, 0, &guarded));
    CHECK(idm_bc_add_function(&module, "fallback", 1, 0, 0, &fallback));
    CHECK(idm_bc_add_function(&module, "guard", 1, 0, 0, &guard));
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(idm_bc_set_function_guard(&module, guarded, guard));
    uint32_t c0 = 0;
    uint32_t c1 = 0;
    uint32_t c42 = 0;
    uint32_t c_bad = 0;
    CHECK(idm_bc_add_const(&module, idm_int(0), &c0));
    CHECK(idm_bc_add_const(&module, idm_int(1), &c1));
    CHECK(idm_bc_add_const(&module, idm_int(42), &c42));
    CHECK(idm_bc_add_const(&module, idm_string(&rt, "bad", &err), &c_bad));
    CHECK(!err.present);

    CHECK(idm_bc_set_function_entry(&module, guard, module.code_count));
    CHECK(test_emit_load_const(&module, 1, c0));
    CHECK(test_emit_make_closure(&module, 2, lt_prim));
    CHECK(emit_call(&module, 3, 2, 0, 2, false));
    CHECK(test_emit_return(&module, 3));
    CHECK(idm_bc_set_function_register_count(&module, guard, 4));
    CHECK(idm_bc_set_function_entry(&module, guarded, module.code_count));
    CHECK(test_emit_load_const(&module, 1, c1));
    CHECK(test_emit_return(&module, 1));
    CHECK(idm_bc_set_function_register_count(&module, guarded, 2));
    CHECK(idm_bc_set_function_entry(&module, fallback, module.code_count));
    CHECK(test_emit_load_const(&module, 1, c42));
    CHECK(test_emit_return(&module, 1));
    CHECK(idm_bc_set_function_register_count(&module, fallback, 2));
    CHECK(idm_bc_set_function_entry(&module, main_fn, module.code_count));
    CHECK(idm_bc_emit_u32(&module, IDM_OP_MAKE_MULTI_CLOSURE, 0, NULL));
    CHECK(idm_bc_emit(&module, 2, NULL));
    CHECK(idm_bc_emit(&module, 0, NULL));
    CHECK(idm_bc_emit(&module, 0, NULL));
    CHECK(idm_bc_emit(&module, guarded, NULL));
    CHECK(idm_bc_emit(&module, fallback, NULL));
    CHECK(test_emit_load_const(&module, 1, c_bad));
    CHECK(emit_call(&module, 2, 0, 1, 1, false));
    CHECK(test_emit_return(&module, 2));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 3));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));

    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_bytecode_link_finalizes_after_intern(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule src;
    idm_bc_init(&src);
    uint32_t c7 = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_const(&src, idm_int(7), &c7));
    CHECK(idm_bc_add_function(&src, "main", 0, 0, 0, &main_fn));
    CHECK(test_emit_load_const(&src, 0, c7));
    CHECK(test_emit_return(&src, 0));
    CHECK(idm_bc_set_function_register_count(&src, main_fn, 1));
    IdmBytecodeModule linked;
    idm_bc_init(&linked);
    uint32_t fn_off = 0;
    CHECK(idm_bc_link(&linked, &src, NULL, &fn_off, NULL, &err));
    CHECK(!linked.selectors_prepared);
    CHECK(idm_bc_intern_literals(&rt, &linked, &err));
    CHECK(idm_bc_is_finalized(&linked));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &linked, fn_off + main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 7);
    CHECK(!err.present);
    idm_bc_destroy(&linked);
    idm_bc_destroy(&src);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_stale_closure_selector_fails(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t c7 = 0;
    uint32_t main_fn = 0;
    CHECK(idm_bc_add_const(&module, idm_int(7), &c7));
    CHECK(idm_bc_add_function(&module, "main", 0, 0, 0, &main_fn));
    CHECK(test_emit_load_const(&module, 0, c7));
    CHECK(test_emit_return(&module, 0));
    CHECK(idm_bc_set_function_register_count(&module, main_fn, 1));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue closure = idm_closure_in_module(&rt, &module, main_fn, NULL, 0, rt.main_env, &err);
    CHECK(!err.present);
    CHECK(idm_bc_add_const(&module, idm_int(8), NULL));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(!idm_vm_call_closure(&rt, closure, NULL, 0, &out, &err));
    CHECK(err.present);
    CHECK(err.message != NULL && strstr(err.message, "stale") != NULL);
    idm_bc_destroy(&module);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_compile_and_vm(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<core-test>");
    IdmCore *add = primitive_clause_call(IDM_PRIM_ADD, span);
    CHECK(idm_core_call_add_arg(add, idm_core_literal(idm_int(20), span)));
    CHECK(idm_core_call_add_arg(add, idm_core_literal(idm_int(22), span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(add, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    idm_bc_destroy(&module);
    idm_core_free(add);

    IdmCore *cond_expr = idm_core_cond(
        idm_core_literal(idm_atom(&rt, "false"), span),
        idm_core_literal(idm_int(1), span),
        idm_core_literal(idm_int(2), span),
        span);
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(cond_expr, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 2);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(cond_expr);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_scope_sets(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId a = idm_scope_fresh(&store);
    IdmScopeId b = idm_scope_fresh(&store);
    IdmScopeSet set;
    IdmScopeSet sup;
    idm_scope_set_init(&set);
    idm_scope_set_init(&sup);
    CHECK(idm_scope_set_add(&set, b));
    CHECK(idm_scope_set_add(&set, a));
    CHECK(idm_scope_set_add(&set, a));
    CHECK(set.count == 2u);
    CHECK(set.items[0] == a && set.items[1] == b);
    CHECK(idm_scope_set_add(&sup, a));
    CHECK(idm_scope_set_add(&sup, b));
    CHECK(idm_scope_set_subset(&set, &sup));
    CHECK(idm_scope_set_equal(&set, &sup));
    CHECK(idm_scope_set_flip(&sup, b));
    CHECK(!idm_scope_set_contains(&sup, b));
    CHECK(idm_scope_set_subset(&sup, &set));
    CHECK(!idm_scope_set_subset(&set, &sup));
    CHECK(idm_scope_set_flip(&sup, b));
    CHECK(idm_scope_set_equal(&set, &sup));
    idm_scope_set_destroy(&set);
    idm_scope_set_destroy(&sup);
}

static void test_binding_resolution(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId s1 = idm_scope_fresh(&store);
    IdmScopeId s2 = idm_scope_fresh(&store);
    IdmScopeSet empty;
    IdmScopeSet one;
    IdmScopeSet two;
    IdmScopeSet both;
    idm_scope_set_init(&empty);
    idm_scope_set_init(&one);
    idm_scope_set_init(&two);
    idm_scope_set_init(&both);
    CHECK(idm_scope_set_add(&one, s1));
    CHECK(idm_scope_set_add(&two, s2));
    CHECK(idm_scope_set_add(&both, s1));
    CHECK(idm_scope_set_add(&both, s2));

    IdmBindingTable table;
    idm_binding_table_init(&table);
    IdmBindingId root = 0;
    IdmBindingId inner = 0;
    CHECK(idm_binding_table_add(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &empty, 10, 0u, &root));
    CHECK(idm_binding_table_add(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one, 20, 0u, &inner));
    const IdmBinding *binding = NULL;
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, &empty, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->id == root && binding->payload == 10);
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_DEFAULT, &one, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->id == inner && binding->payload == 20);
    CHECK(idm_binding_resolve(&table, "x", 1, IDM_BIND_SPACE_DEFAULT, &one, &binding) == IDM_RESOLVE_UNBOUND);
    CHECK(idm_binding_resolve(&table, "x", 0, IDM_BIND_SPACE_OPERATOR, &one, &binding) == IDM_RESOLVE_UNBOUND);

    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one, 1, 0u, NULL));
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &two, 2, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &both, &binding) == IDM_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);

    IdmScopeId s3 = idm_scope_fresh(&store);
    IdmScopeSet one_three;
    IdmScopeSet all;
    idm_scope_set_init(&one_three);
    idm_scope_set_init(&all);
    CHECK(idm_scope_set_add(&one_three, s1));
    CHECK(idm_scope_set_add(&one_three, s3));
    CHECK(idm_scope_set_add(&all, s1));
    CHECK(idm_scope_set_add(&all, s2));
    CHECK(idm_scope_set_add(&all, s3));
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &one_three, 3, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &all, &binding) == IDM_RESOLVE_AMBIGUOUS);
    CHECK(binding == NULL);
    CHECK(idm_binding_table_add(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, IDM_BIND_VALUE, &both, 4, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "amb", 0, IDM_BIND_SPACE_DEFAULT, &both, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->payload == 4);
    idm_scope_set_destroy(&one_three);
    idm_scope_set_destroy(&all);

    CHECK(idm_binding_table_add(&table, "op", 0, IDM_BIND_SPACE_OPERATOR, IDM_BIND_OPERATOR, &empty, 99, 0u, NULL));
    CHECK(idm_binding_resolve(&table, "op", 0, IDM_BIND_SPACE_OPERATOR, &empty, &binding) == IDM_RESOLVE_OK);
    CHECK(binding && binding->kind == IDM_BIND_OPERATOR && binding->payload == 99);

    idm_binding_table_destroy(&table);
    idm_scope_set_destroy(&empty);
    idm_scope_set_destroy(&one);
    idm_scope_set_destroy(&two);
    idm_scope_set_destroy(&both);
}

static void test_syntax_scope_tree(void) {
    IdmScopeStore store;
    idm_scope_store_init(&store);
    IdmScopeId scope = idm_scope_fresh(&store);
    IdmSpan span = idm_span_unknown("<syntax-scope-test>");
    IdmSyntax *root = idm_syn_list(span);
    IdmSyntax *word = idm_syn_word("root", span);
    IdmSyntax *nested = idm_syn_vector(span);
    IdmSyntax *inner = idm_syn_word("inner", span);
    CHECK(root && word && nested && inner);
    CHECK(idm_syn_append(nested, inner));
    CHECK(idm_syn_append(root, word));
    CHECK(idm_syn_append(root, nested));
    CHECK(idm_syn_scope_add_tree(root, 0, scope));
    CHECK(idm_syn_scope_contains(root, 0, scope));
    CHECK(idm_syn_scope_contains(word, 0, scope));
    CHECK(idm_syn_scope_contains(nested, 0, scope));
    CHECK(idm_syn_scope_contains(inner, 0, scope));
    CHECK(!idm_syn_scope_contains(inner, 1, scope));
    CHECK(idm_syn_scope_flip_tree(root, 0, scope));
    CHECK(!idm_syn_scope_contains(root, 0, scope));
    CHECK(!idm_syn_scope_contains(word, 0, scope));
    CHECK(!idm_syn_scope_contains(nested, 0, scope));
    CHECK(!idm_syn_scope_contains(inner, 0, scope));
    idm_syn_free(root);
}

static void test_pattern_selector(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<pattern-test>");
    uint32_t fn = UINT32_MAX;
    bool has = false;
    bool matched = false;
    IdmValue pair = idm_cons(&rt, idm_int(1), idm_int(2), &err);
    IdmPattern *pat = idm_pat_pair(idm_pat_bind("h", span), idm_pat_bind("t", span), span);
    IdmPattern *patterns[1] = { pat };
    IdmPatternSelectorClause clause = {
        .function_index = 7,
        .arity = idm_arity_exact(1u),
        .patterns = patterns,
        .pattern_count = 1,
    };
    IdmPatternSelector *selector = NULL;
    CHECK(idm_pattern_selector_build(&clause, 1u, &selector, &err));
    IdmPatternBindings bindings;
    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_selector_select(&rt, selector, &pair, 1u, NULL, NULL, &fn, &bindings, &has, &matched, &err));
    CHECK(matched && has && fn == 7u);
    const IdmValue *h = idm_pattern_bindings_get(&bindings, "h");
    const IdmValue *t = idm_pattern_bindings_get(&bindings, "t");
    CHECK(h && h->tag == IDM_VAL_INT && h->as.i == 1);
    CHECK(t && t->tag == IDM_VAL_INT && t->as.i == 2);
    CHECK(!err.present);
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
    idm_pat_free(pat);

    IdmValue same = idm_cons(&rt, idm_int(7), idm_cons(&rt, idm_int(7), idm_nil(), &err), &err);
    IdmValue different = idm_cons(&rt, idm_int(7), idm_cons(&rt, idm_int(8), idm_nil(), &err), &err);
    pat = idm_pat_pair(idm_pat_bind("x", span), idm_pat_pair(idm_pat_bind("x", span), idm_pat_literal(idm_nil(), span), span), span);
    patterns[0] = pat;
    selector = NULL;
    CHECK(idm_pattern_selector_build(&clause, 1u, &selector, &err));
    fn = UINT32_MAX;
    has = false;
    matched = false;
    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_selector_select(&rt, selector, &same, 1u, NULL, NULL, &fn, &bindings, &has, &matched, &err));
    CHECK(matched && has && fn == 7u);
    CHECK(bindings.count == 1u);
    idm_pattern_bindings_destroy(&bindings);
    fn = UINT32_MAX;
    has = false;
    matched = false;
    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_selector_select(&rt, selector, &different, 1u, NULL, NULL, &fn, &bindings, &has, &matched, &err));
    CHECK(!matched);
    CHECK(bindings.count == 0u);
    CHECK(!err.present);
    idm_pattern_bindings_destroy(&bindings);
    idm_pattern_selector_free(selector);
    idm_pat_free(pat);

    idm_pattern_bindings_init(&bindings);
    CHECK(idm_pattern_bindings_add(&bindings, "p", idm_int(5)));
    pat = idm_pat_pin("p", span);
    patterns[0] = pat;
    selector = NULL;
    CHECK(idm_pattern_selector_build(&clause, 1u, &selector, &err));
    IdmValue pinned = idm_int(5);
    fn = UINT32_MAX;
    has = false;
    matched = false;
    CHECK(idm_pattern_selector_select(&rt, selector, &pinned, 1u, NULL, NULL, &fn, &bindings, &has, &matched, &err));
    CHECK(matched && fn == 7u);
    pinned = idm_int(6);
    fn = UINT32_MAX;
    has = false;
    matched = false;
    CHECK(idm_pattern_selector_select(&rt, selector, &pinned, 1u, NULL, NULL, &fn, &bindings, &has, &matched, &err));
    CHECK(!matched);
    idm_pattern_selector_free(selector);
    idm_pat_free(pat);
    idm_pattern_bindings_destroy(&bindings);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_do_and_local_bind(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<local-core-test>");

    IdmCore *add = primitive_clause_call(IDM_PRIM_ADD, span);
    CHECK(idm_core_call_add_arg(add, idm_core_local_ref("x", 0, span)));
    CHECK(idm_core_call_add_arg(add, idm_core_literal(idm_int(1), span)));
    IdmCore *bind = idm_core_bind_local("x", 0, idm_core_literal(idm_int(41), span), add, span);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(bind, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(module.functions[main_fn].local_count == 1u);
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(bind);

    IdmCore *do_expr = idm_core_do(span);
    CHECK(idm_core_do_add(do_expr, idm_core_literal(idm_int(1), span)));
    CHECK(idm_core_do_add(do_expr, idm_core_literal(idm_int(2), span)));
    idm_bc_init(&module);
    CHECK(idm_core_compile_main(do_expr, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 2);
    idm_bc_destroy(&module);
    idm_core_free(do_expr);

    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_core_fn_literal_and_call(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<fn-core-test>");

    IdmCore *body_add = primitive_clause_call(IDM_PRIM_ADD, span);
    CHECK(idm_core_call_add_arg(body_add, idm_core_arg_ref("n", 0, span)));
    CHECK(idm_core_call_add_arg(body_add, idm_core_literal(idm_int(1), span)));
    IdmCore *fn = idm_core_fn("inc", 1, body_add, span);
    IdmCore *call = idm_core_call(fn, span);
    CHECK(idm_core_call_add_arg(call, idm_core_literal(idm_int(41), span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(call, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &module));
    CHECK(strstr(dis.data, "MAKE_CLOSURE") != NULL);
    CHECK(strstr(dis.data, "CALL             r") != NULL);
    CHECK(strstr(dis.data, "argc=1 tail=1") != NULL);
    CHECK(strstr(dis.data, "direct") == NULL);
    idm_buf_destroy(&dis);
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(call);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static IdmCore *literal_arg_prim(IdmPrimitive prim, uint32_t argc, IdmSpan span) {
    IdmCore *call = primitive_clause_call(prim, span);
    for (uint32_t i = 0; i < argc; i++) {
        CHECK(idm_core_call_add_arg(call, idm_core_literal(idm_nil(), span)));
    }
    return call;
}

static size_t count_substr(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t n = strlen(needle);
    const char *cursor = haystack;
    while (cursor && *cursor) {
        cursor = strstr(cursor, needle);
        if (!cursor) break;
        count++;
        cursor += n;
    }
    return count;
}

static void test_regex_primitives_compile_to_clause_calls(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<regex-opcode-test>");
    IdmCore *core = idm_core_do(span);
    CHECK(core != NULL);
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_REGEX_TEST, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_REGEX_SCAN_FROM, 3u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_REGEX_SCAN_AT, 3u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_REGEX_SCAN_FULL, 2u, span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    (void)main_fn;
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &module));
    CHECK(count_substr(dis.data, "primitive=") == 4u);
    CHECK(strstr(dis.data, "PRIM_CALL") == NULL);
    CHECK(strstr(dis.data, "REGEX_TEST") == NULL);
    CHECK(strstr(dis.data, "REGEX_SCAN") == NULL);
    CHECK(strstr(dis.data, "REGEX_EXEC") == NULL);
    idm_buf_destroy(&dis);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_arithmetic_primitives_compile_to_clause_calls(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<arithmetic-opcode-test>");
    IdmCore *core = idm_core_do(span);
    CHECK(core != NULL);
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_ADD, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_SUB, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_MUL, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_DIV, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_MOD, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_POW, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_NEG, 1u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_LT, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_GT, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_LTE, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_GTE, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_EQ, 2u, span)));
    CHECK(idm_core_do_add(core, literal_arg_prim(IDM_PRIM_NEQ, 2u, span)));
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    (void)main_fn;
    IdmBuffer dis;
    idm_buf_init(&dis);
    CHECK(idm_bc_disassemble(&dis, &module));
    CHECK(count_substr(dis.data, "primitive=") == 13u);
    CHECK(strstr(dis.data, "PRIM_CALL") == NULL);
    CHECK(strstr(dis.data, "NUM_ADD") == NULL);
    CHECK(strstr(dis.data, "NUM_SUB") == NULL);
    CHECK(strstr(dis.data, "NUM_MUL") == NULL);
    CHECK(strstr(dis.data, "NUM_DIV") == NULL);
    CHECK(strstr(dis.data, "NUM_MOD") == NULL);
    CHECK(strstr(dis.data, "NUM_POW") == NULL);
    CHECK(strstr(dis.data, "NUM_NEG") == NULL);
    CHECK(strstr(dis.data, "NUM_LT") == NULL);
    CHECK(strstr(dis.data, "NUM_GT") == NULL);
    CHECK(strstr(dis.data, "NUM_LTE") == NULL);
    CHECK(strstr(dis.data, "NUM_GTE") == NULL);
    idm_buf_destroy(&dis);
    idm_bc_destroy(&module);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static IdmCore *binary_prim(IdmPrimitive prim, IdmCore *a, IdmCore *b, IdmSpan span) {
    IdmCore *call = primitive_clause_call(prim, span);
    CHECK(idm_core_call_add_arg(call, a));
    CHECK(idm_core_call_add_arg(call, b));
    return call;
}

static void test_core_letrec_recursion(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("<letrec-core-test>");

    IdmCore *cond = binary_prim(IDM_PRIM_LT, idm_core_arg_ref("n", 0, span), idm_core_literal(idm_int(1), span), span);
    IdmCore *minus_one = binary_prim(IDM_PRIM_SUB, idm_core_arg_ref("n", 0, span), idm_core_literal(idm_int(1), span), span);
    IdmCore *recursive_call = idm_core_call(idm_core_capture_ref("sumdown", 0, span), span);
    CHECK(idm_core_call_add_arg(recursive_call, minus_one));
    IdmCore *sum = binary_prim(IDM_PRIM_ADD, idm_core_arg_ref("n", 0, span), recursive_call, span);
    IdmCore *body = idm_core_cond(cond, idm_core_literal(idm_int(0), span), sum, span);
    IdmCore *fn = idm_core_fn("sumdown", 1, body, span);
    CHECK(idm_core_fn_add_capture(fn, IDM_CAP_LOCAL, "sumdown", 0));
    IdmCore *letrec = idm_core_letrec(span);
    CHECK(idm_core_letrec_add(letrec, "sumdown", 0, fn));
    IdmCore *call = idm_core_call(idm_core_local_ref("sumdown", 0, span), span);
    CHECK(idm_core_call_add_arg(call, idm_core_literal(idm_int(5), span)));
    CHECK(idm_core_letrec_set_body(letrec, call));

    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(letrec, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 15);
    CHECK(!err.present);
    idm_bc_destroy(&module);
    idm_core_free(letrec);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_source_expansion_capabilities(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<expand-test>", "x = 40\nadd x 2\n", &core, &err));
    IdmBuffer dump;
    idm_buf_init(&dump);
    CHECK(idm_core_dump(&dump, core));
    CHECK(strstr(dump.data, "(bind-local x#0 40") != NULL);
    CHECK(strstr(dump.data, "((fn-multi add (/2..2 primitive add)") != NULL);
    CHECK(strstr(dump.data, "(prim ") == NULL);
    idm_buf_destroy(&dump);
    IdmBytecodeModule module;
    idm_bc_init(&module);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &module, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &module, &err));
    IdmValue out = idm_nil();
    CHECK(idm_vm_run(&rt, &module, main_fn, &out, &err));
    CHECK(out.tag == IDM_VAL_INT && out.as.i == 42);
    idm_bc_destroy(&module);
    idm_core_free(core);
    CHECK(!err.present);

    core = NULL;
    CHECK(!idm_expand_source_string(&rt, "<expand-test>", "echo hello\n", &core, &err));
    CHECK(err.present);
    idm_error_clear(&err);

    core = NULL;
    CHECK(idm_expand_source_string(&rt, "<expand-test>", "use app/ish\nactivate Shell\necho hello\n", &core, &err));
    CHECK(!err.present);
    CHECK(core != NULL);
    {
        IdmBytecodeModule cmd_module;
        idm_bc_init(&cmd_module);
        uint32_t cmd_fn = 0;
        CHECK(idm_core_compile_main(core, &cmd_module, &cmd_fn, &err));
        CHECK(idm_bc_intern_literals(&rt, &cmd_module, &err));
        CHECK(!err.present);
        idm_bc_destroy(&cmd_module);
    }
    idm_core_free(core);
    idm_runtime_destroy(&rt);
}

static void test_sha256_vectors(void) {
    char hex[65];
    idm_sha256_hex("", 0u, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    idm_sha256_hex("abc", 3u, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    idm_sha256_hex(two_block, strlen(two_block), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char block63[64];
    memset(block63, 'a', 63u);
    block63[63] = '\0';
    idm_sha256_hex(block63, 63u, hex);
    CHECK_STR(hex, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
}

static void test_intern_stability(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmValue first = idm_atom(&rt, "first-symbol");
    char name[32];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "sym%d", i);
        (void)idm_atom(&rt, name);
    }
    IdmValue again = idm_atom(&rt, "first-symbol");
    CHECK(idm_value_equal(first, again));
    CHECK_STR(idm_symbol_text(first.as.symbol), "first-symbol");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_bytecode_serialize_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    const char *src = "defn pick x do match x do [a b] -> add a b end end\npick [40 2]\n";
    IdmCore *core = NULL;
    CHECK(idm_expand_source_string(&rt, "<ishc-test>", src, &core, &err));
    CHECK(!err.present);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    uint32_t main_fn = 0;
    CHECK(idm_core_compile_main(core, &m1, &main_fn, &err));
    CHECK(idm_bc_intern_literals(&rt, &m1, &err));
    CHECK(!err.present);
    IdmValue out1 = idm_nil();
    CHECK(idm_vm_run(&rt, &m1, main_fn, &out1, &err));
    CHECK(!err.present);
    CHECK(out1.tag == IDM_VAL_INT && out1.as.i == 42);
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    IdmBuffer d1;
    IdmBuffer d2;
    idm_buf_init(&d1);
    idm_buf_init(&d2);
    CHECK(idm_bc_disassemble(&d1, &m1));
    CHECK(idm_bc_disassemble(&d2, &m2));
    CHECK_STR(d2.data, d1.data);
    IdmValue out2 = idm_nil();
    CHECK(idm_vm_run(&rt, &m2, main_fn, &out2, &err));
    CHECK(!err.present);
    CHECK(out2.tag == IDM_VAL_INT && out2.as.i == 42);
    IdmBytecodeModule m3;
    idm_bc_init(&m3);
    CHECK(!idm_ic_deserialize(&rt, (const unsigned char *)"XX", 2u, &m3, &err));
    idm_error_clear(&err);
    idm_bc_destroy(&m3);
    idm_buf_destroy(&d1);
    idm_buf_destroy(&d2);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_core_free(core);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_syntax_serialize_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("fixture.id");
    span.start = 3;
    span.end = 9;
    span.line = 2;
    span.column = 5;
    IdmSyntax *root = idm_syn_list(span);
    CHECK(root != NULL);
    IdmSyntax *word = idm_syn_word("hello", span);
    CHECK(word != NULL);
    CHECK(idm_syn_scope_add(word, 0, 7u));
    CHECK(idm_syn_scope_add(word, 0, 42u));
    CHECK(idm_syn_scope_add(word, 1, 9u));
    idm_syn_set_token(word, "hello", true, false);
    CHECK(idm_syn_property_set(word, "value-context", "true"));
    CHECK(idm_syn_origin_push(word, "macro-x"));
    CHECK(idm_syn_append(root, word));
    CHECK(idm_syn_append(root, idm_syn_atom("ok", span)));
    CHECK(idm_syn_append(root, idm_syn_int(-12, span)));
    CHECK(idm_syn_append(root, idm_syn_float(2.5, span)));
    CHECK(idm_syn_append(root, idm_syn_string("s\"x", span)));
    IdmSyntax *inner = idm_syn_tuple(span);
    CHECK(inner != NULL);
    CHECK(idm_syn_append(inner, idm_syn_word("deep", span)));
    CHECK(idm_syn_append(root, inner));

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_syn_serialize(&blob, root, &err));
    CHECK(!err.present);
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)blob.data, blob.len);
    IdmSyntax *back = idm_syn_deserialize(&rt, &r, &err);
    CHECK(back != NULL);
    CHECK(!err.present);
    CHECK(r.pos == blob.len);

    IdmBuffer d1, d2;
    idm_buf_init(&d1);
    idm_buf_init(&d2);
    CHECK(idm_syn_dump(&d1, root));
    CHECK(idm_syn_dump(&d2, back));
    CHECK_STR(d2.data, d1.data);
    IdmSyntax *back_word = back->as.seq.items[0];
    CHECK(back_word->kind == IDM_SYN_WORD);
    CHECK(idm_syn_scope_contains(back_word, 0, 7u));
    CHECK(idm_syn_scope_contains(back_word, 0, 42u));
    CHECK(idm_syn_scope_contains(back_word, 1, 9u));
    CHECK(!idm_syn_scope_contains(back_word, 0, 9u));
    CHECK(back_word->token_raw != NULL && strcmp(back_word->token_raw, "hello") == 0);
    CHECK(back_word->token_leading_space && !back_word->token_adjacent_previous);
    const char *prop = idm_syn_property_get(back_word, "value-context");
    CHECK(prop != NULL && strcmp(prop, "true") == 0);
    CHECK(back_word->origins.count == 1 && strcmp(back_word->origins.items[0], "macro-x") == 0);
    CHECK(back->as.seq.items[1]->kind == IDM_SYN_ATOM);
    CHECK(back->as.seq.items[2]->as.integer == -12);
    CHECK(back->as.seq.items[3]->as.real == 2.5);
    CHECK(back_word->span.file != NULL && strcmp(back_word->span.file, "fixture.id") == 0);
    CHECK(back_word->span.start == 3 && back_word->span.end == 9 && back_word->span.line == 2 && back_word->span.column == 5);

    idm_syn_scope_relocate_tree(back, 10u, 100u);
    CHECK(idm_syn_scope_contains(back_word, 0, 7u));
    CHECK(idm_syn_scope_contains(back_word, 0, 142u));
    CHECK(!idm_syn_scope_contains(back_word, 0, 42u));
    CHECK(idm_syn_scope_contains(back_word, 1, 9u));

    IdmByteReader bad;
    idm_byte_reader_init(&bad, (const unsigned char *)blob.data, blob.len > 8u ? blob.len - 8u : 1u);
    IdmSyntax *trunc = idm_syn_deserialize(&rt, &bad, &err);
    CHECK(trunc == NULL && err.present);
    idm_error_clear(&err);

    idm_buf_destroy(&d1);
    idm_buf_destroy(&d2);
    idm_buf_destroy(&blob);
    idm_syn_free(root);
    idm_syn_free(back);
    idm_runtime_destroy(&rt);
}

static void test_module_syntax_constant_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    IdmSpan span = idm_span_unknown("tmpl.id");
    IdmSyntax *template_syn = idm_syn_list(span);
    CHECK(template_syn != NULL);
    IdmSyntax *head = idm_syn_word("cond", span);
    CHECK(head != NULL);
    CHECK(idm_syn_scope_add(head, 0, 21u));
    CHECK(idm_syn_append(template_syn, head));
    CHECK(idm_syn_append(template_syn, idm_syn_int(1, span)));
    IdmValue template_value = idm_syntax_value(&rt, template_syn, &err);
    CHECK(!err.present);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m1, template_value, &const_index));
    uint32_t fn = 0;
    CHECK(idm_bc_add_function(&m1, "main", 0, 0, 0, &fn));
    CHECK(test_emit_load_const(&m1, 0, const_index));
    CHECK(test_emit_return(&m1, 0));
    CHECK(idm_bc_set_function_register_count(&m1, fn, 1));

    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    CHECK(m2.const_count == 1 && m2.constants[0].tag == IDM_VAL_SYNTAX);
    const IdmSyntax *back = idm_syntax_get(m2.constants[0], &err);
    CHECK(back != NULL && !err.present);
    CHECK(idm_syn_scope_contains(back->as.seq.items[0], 0, 21u));

    const IdmSyntax *original = idm_syntax_get(m1.constants[0], &err);
    CHECK(original != NULL && !err.present);
    CHECK(idm_bc_relocate_syntax_scopes(&rt, &m2, 10u, 50u, &err));
    CHECK(!err.present);
    const IdmSyntax *moved = idm_syntax_get(m2.constants[0], &err);
    CHECK(moved != NULL && !err.present);
    CHECK(idm_syn_scope_contains(moved->as.seq.items[0], 0, 71u));
    CHECK(!idm_syn_scope_contains(moved->as.seq.items[0], 0, 21u));
    CHECK(idm_syn_scope_contains(original->as.seq.items[0], 0, 21u));

    IdmBytecodeModule linked;
    idm_bc_init(&linked);
    uint32_t const_off = 0, fn_off = 0, code_off = 0;
    CHECK(idm_bc_link(&linked, &m1, &const_off, &fn_off, &code_off, &err));
    CHECK(idm_bc_relocate_syntax_scopes(&rt, &linked, 10u, 1000u, &err));
    const IdmSyntax *aliased = idm_syntax_get(m1.constants[0], &err);
    CHECK(aliased != NULL && idm_syn_scope_contains(aliased->as.seq.items[0], 0, 21u));
    const IdmSyntax *relinked = idm_syntax_get(linked.constants[0], &err);
    CHECK(relinked != NULL && idm_syn_scope_contains(relinked->as.seq.items[0], 0, 1021u));

    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_bc_destroy(&linked);
    idm_syn_free(template_syn);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_string_constant_nul_roundtrip(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmBytecodeModule m1;
    idm_bc_init(&m1);
    const char bytes[] = "nul\0byte";
    IdmValue s = idm_string_n(&rt, bytes, sizeof(bytes) - 1u, &err);
    CHECK(!err.present);
    CHECK(idm_string_length(s) == sizeof(bytes) - 1u);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m1, s, &const_index));
    uint32_t fn = 0;
    CHECK(idm_bc_add_function(&m1, "main", 0, 0, 0, &fn));
    CHECK(test_emit_load_const(&m1, 0, const_index));
    CHECK(test_emit_return(&m1, 0));
    CHECK(idm_bc_set_function_register_count(&m1, fn, 1));
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(idm_ic_serialize(&m1, &blob, &err));
    CHECK(!err.present);
    IdmBytecodeModule m2;
    idm_bc_init(&m2);
    CHECK(idm_ic_deserialize(&rt, (const unsigned char *)blob.data, blob.len, &m2, &err));
    CHECK(!err.present);
    CHECK(m2.const_count == 1 && m2.constants[0].tag == IDM_VAL_STRING);
    CHECK(idm_string_length(m2.constants[0]) == sizeof(bytes) - 1u);
    CHECK(memcmp(idm_string_bytes(m2.constants[0]), bytes, sizeof(bytes) - 1u) == 0);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m1);
    idm_bc_destroy(&m2);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void test_reader_depth_guard(void) {
    size_t n = 5000;
    IdmBuffer src;
    idm_buf_init(&src);
    for (size_t i = 0; i < n; i++) CHECK(idm_buf_append_char(&src, '['));
    for (size_t i = 0; i < n; i++) CHECK(idm_buf_append_char(&src, ']'));
    CHECK(idm_buf_append_char(&src, '\n'));
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *program = NULL;
    IdmRuntime rt;
    idm_runtime_init(&rt);
    CHECK(!idm_expand_read_source_string(&rt, "<deep>", src.data, &program, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    CHECK(err.span.file != NULL && strcmp(err.span.file, "<deep>") == 0);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    idm_buf_destroy(&src);
}

static void test_serialize_depth_guard(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSpan span = idm_span_unknown("deep.id");
    IdmSyntax *deep = idm_syn_int(1, span);
    CHECK(deep != NULL);
    for (size_t i = 0; i < 2000; i++) {
        IdmSyntax *wrap = idm_syn_list(span);
        CHECK(wrap != NULL && idm_syn_append(wrap, deep));
        deep = wrap;
    }
    IdmBuffer blob;
    idm_buf_init(&blob);
    CHECK(!idm_syn_serialize(&blob, deep, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    idm_error_clear(&err);
    idm_syn_free(deep);
    idm_buf_destroy(&blob);

    IdmValue list = idm_nil();
    for (size_t i = 0; i < 2000; i++) {
        list = idm_cons(&rt, idm_int((int64_t)i), list, &err);
        CHECK(!err.present);
    }
    IdmBytecodeModule m;
    idm_bc_init(&m);
    uint32_t const_index = 0;
    CHECK(idm_bc_add_const(&m, list, &const_index));
    idm_buf_init(&blob);
    CHECK(!idm_ic_serialize(&m, &blob, &err));
    CHECK(err.present && err.message != NULL && strstr(err.message, "nested too deeply") != NULL);
    idm_error_clear(&err);
    idm_buf_destroy(&blob);
    idm_bc_destroy(&m);
    idm_runtime_destroy(&rt);
}

static void test_value_copy(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    IdmValue s = idm_string(&rt, "hi", &err);
    IdmValue lst = idm_cons(&rt, idm_int(1), idm_cons(&rt, idm_int(2), idm_empty_list(), &err), &err);
    IdmValue items[2] = { s, lst };
    IdmValue orig = idm_tuple(&rt, items, 2u, &err);
    CHECK(!err.present);

    IdmValue copy = idm_value_copy(&rt, &rt.heap, orig, &err);
    CHECK(!err.present);
    CHECK(idm_value_equal(orig, copy));
    CHECK(orig.as.obj != copy.as.obj);
    IdmValue cs = idm_sequence_item(copy, 0u, &err);
    IdmValue os = idm_sequence_item(orig, 0u, &err);
    CHECK(cs.as.obj != os.as.obj);
    CHECK(idm_value_equal(cs, os));

    IdmValue k = idm_cell(&rt, idm_nil(), &err);
    IdmValue titems[2] = { idm_int(1), k };
    IdmValue t = idm_tuple(&rt, titems, 2u, &err);
    CHECK(idm_cell_set(k, t, &err));
    IdmValue tcopy = idm_value_copy(&rt, &rt.heap, t, &err);
    CHECK(!err.present);
    CHECK(tcopy.as.obj != t.as.obj);
    IdmValue kcopy = idm_sequence_item(tcopy, 1u, &err);
    CHECK(kcopy.tag == IDM_VAL_CELL);
    CHECK(kcopy.as.obj != k.as.obj);
    IdmValue inner = idm_cell_get(kcopy, &err);
    CHECK(inner.as.obj == tcopy.as.obj);

    idm_runtime_destroy(&rt);
}

void run_substrate_suite(void) {
    test_values();
    test_value_copy();
    test_gc_deep_pair_chain();
    test_transparent_reader_basic();
    test_kernel_bootstrap_iridium_quine();
    test_bootstrap_source_reader_selection_is_data_driven();
    test_surface_grammar_source_reader_artifact();
    test_reader_artifact();
    test_reader_artifact_terminal_captures();
    test_reader_artifact_metadata_reductions();
    test_reader_artifact_literal_roundtrip();
    test_reader_artifact_binary_roundtrip();
    test_reader_artifact_binary_preserves_terminal_descriptors();
    test_reader_artifact_literal_drops_declaration_metadata();
    test_reader_artifact_literal_depth_guard();
    test_reader_artifact_rejects_nullable_terminals();
    test_active_grammar_reader_artifact_freeze();
    test_reader_artifact_rejects_bad_reductions();
    test_reader_artifact_rejects_mixed_scopes();
    test_reader_artifact_rejects_mixed_phases();
    test_reader_artifact_terminal_priority();
    test_expand_reader_artifact_string_entrypoint();
    test_pythonish_reader_artifact_over_language();
    test_reader_basic();
    test_reader_shell_protocols();
    test_reader_quote();
    test_reader_body_and_function_value();
    test_bytecode_verifier();
    test_vm_call_and_locals();
    test_vm_tail_call_reuses_frame();
    test_vm_guard_error_is_clause_failure();
    test_bytecode_link_finalizes_after_intern();
    test_stale_closure_selector_fails();
    test_core_compile_and_vm();
    test_scope_sets();
    test_binding_resolution();
    test_syntax_scope_tree();
    test_pattern_selector();
    test_core_do_and_local_bind();
    test_core_fn_literal_and_call();
    test_regex_primitives_compile_to_clause_calls();
    test_arithmetic_primitives_compile_to_clause_calls();
    test_core_letrec_recursion();
    test_source_expansion_capabilities();
    test_sha256_vectors();
    test_intern_stability();
    test_bytecode_serialize_roundtrip();
    test_syntax_serialize_roundtrip();
    test_module_syntax_constant_roundtrip();
    test_string_constant_nul_roundtrip();
    test_reader_depth_guard();
    test_serialize_depth_guard();
}
