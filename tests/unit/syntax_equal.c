#include "idiom/syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IdmSpan span(void) {
    return idm_span_unknown("syntax_equal");
}

static void fail(const char *name) {
    fprintf(stderr, "syntax_equal: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static IdmSyntax *must(IdmSyntax *syn, const char *name) {
    if (!syn) fail(name);
    return syn;
}

static IdmSyntax *nil(void) { return must(idm_syn_nil(span()), "nil alloc"); }
static IdmSyntax *word(const char *text) { return must(idm_syn_word(text, span()), "word alloc"); }
static IdmSyntax *atom(const char *text) { return must(idm_syn_atom(text, span()), "atom alloc"); }
static IdmSyntax *integer(int64_t value) { return must(idm_syn_int(value, span()), "int alloc"); }
static IdmSyntax *bigint(const char *text) { return must(idm_syn_bigint(text, span()), "bigint alloc"); }
static IdmSyntax *real(double value) { return must(idm_syn_float(value, span()), "float alloc"); }
static IdmSyntax *string(const char *text) { return must(idm_syn_string(text, span()), "string alloc"); }

static IdmSyntax *seq(IdmSyntaxKind kind, IdmSyntax **items, size_t count) {
    IdmSyntax *syn = must(idm_syn_seq(kind, span()), "seq alloc");
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) fail("seq append");
    }
    return syn;
}

static void equal_pair(IdmSyntax *a, IdmSyntax *b, const char *name) {
    check(idm_syn_equal(a, b), name);
    idm_syn_free(a);
    idm_syn_free(b);
}

static void not_equal_pair(IdmSyntax *a, IdmSyntax *b, const char *name) {
    check(!idm_syn_equal(a, b), name);
    idm_syn_free(a);
    idm_syn_free(b);
}

static IdmSyntax *list2(IdmSyntax *a, IdmSyntax *b) {
    IdmSyntax *items[] = { a, b };
    return seq(IDM_SYN_LIST, items, 2u);
}

static IdmSyntax *vector2(IdmSyntax *a, IdmSyntax *b) {
    IdmSyntax *items[] = { a, b };
    return seq(IDM_SYN_VECTOR, items, 2u);
}

static IdmSyntax *tuple2(IdmSyntax *a, IdmSyntax *b) {
    IdmSyntax *items[] = { a, b };
    return seq(IDM_SYN_TUPLE, items, 2u);
}

static IdmSyntax *dict2(IdmSyntax *a, IdmSyntax *b) {
    IdmSyntax *items[] = { a, b };
    return seq(IDM_SYN_DICT, items, 2u);
}

static void check_dump(IdmSyntax *syn, const char *expected, const char *name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_syn_dump(&buf, syn), name);
    check(buf.data && strcmp(buf.data, expected) == 0, name);
    idm_buf_destroy(&buf);
    idm_syn_free(syn);
}

int idm_unit_syntax_equal(void) {
    IdmSyntax *n = nil();
    check(!idm_syn_equal(NULL, n), "null unequal");
    idm_syn_free(n);

    equal_pair(nil(), nil(), "nil equal");
    not_equal_pair(nil(), atom("nil"), "kind mismatch");
    equal_pair(word("w"), word("w"), "word equal");
    not_equal_pair(word("w"), word("x"), "word unequal");
    equal_pair(atom("a"), atom("a"), "atom equal");
    not_equal_pair(atom("a"), atom("b"), "atom unequal");
    equal_pair(integer(42), integer(42), "int equal");
    not_equal_pair(integer(42), integer(43), "int unequal");
    equal_pair(real(3.5), real(3.5), "float equal");
    not_equal_pair(real(3.5), real(3.75), "float unequal");
    equal_pair(string("s"), string("s"), "string equal");
    not_equal_pair(string("s"), string("t"), "string unequal");
    equal_pair(bigint("000123"), bigint("+123"), "bigint decimal equal");
    not_equal_pair(bigint("-123"), bigint("123"), "bigint decimal unequal");
    equal_pair(bigint("not-decimal"), bigint("not-decimal"), "bigint fallback equal");
    not_equal_pair(bigint("not-decimal"), bigint("also-not-decimal"), "bigint fallback unequal");

    equal_pair(list2(word("a"), atom("b")), list2(word("a"), atom("b")), "list equal");
    equal_pair(vector2(integer(1), atom("v")), vector2(integer(1), atom("v")), "vector equal");
    equal_pair(tuple2(integer(2), string("t")), tuple2(integer(2), string("t")), "tuple equal");
    equal_pair(dict2(atom("k"), integer(9)), dict2(atom("k"), integer(9)), "dict equal");
    IdmSyntax *generic_list = must(idm_syn_seq(IDM_SYN_LIST, span()), "generic list alloc");
    IdmSyntax *generic_vector = must(idm_syn_seq(IDM_SYN_VECTOR, span()), "generic vector alloc");
    IdmSyntax *generic_tuple = must(idm_syn_seq(IDM_SYN_TUPLE, span()), "generic tuple alloc");
    IdmSyntax *generic_dict = must(idm_syn_seq(IDM_SYN_DICT, span()), "generic dict alloc");
    check(generic_list->kind == IDM_SYN_LIST, "generic list kind");
    check(generic_vector->kind == IDM_SYN_VECTOR, "generic vector kind");
    check(generic_tuple->kind == IDM_SYN_TUPLE, "generic tuple kind");
    check(generic_dict->kind == IDM_SYN_DICT, "generic dict kind");
    idm_syn_free(generic_list);
    idm_syn_free(generic_vector);
    idm_syn_free(generic_tuple);
    idm_syn_free(generic_dict);
    check(idm_syn_seq(IDM_SYN_WORD, span()) == NULL, "generic seq rejects scalar");
    not_equal_pair(list2(word("a"), atom("b")), vector2(word("a"), atom("b")), "sequence kind unequal");
    not_equal_pair(list2(word("a"), atom("b")), seq(IDM_SYN_LIST, (IdmSyntax *[]){ word("a") }, 1u), "sequence count unequal");
    equal_pair(list2(vector2(integer(1), atom("a")), dict2(atom("k"), string("v"))),
               list2(vector2(integer(1), atom("a")), dict2(atom("k"), string("v"))),
               "nested equal");
    not_equal_pair(list2(vector2(integer(1), atom("a")), dict2(atom("k"), string("v"))),
                   list2(vector2(integer(1), atom("b")), dict2(atom("k"), string("v"))),
                   "nested unequal");
    check_dump(string("a\n\t\\\""), "\"a\\n\\t\\\\\\\"\"", "dump escaped string");
    check_dump(string("#{x} ${x}"), "\"\\#{x} \\${x}\"", "dump escaped interpolation openers");
    check_dump(list2(word("a"), atom("b")), "(a :b)", "dump list");
    check_dump(vector2(integer(1), string("x")), "[1 \"x\"]", "dump vector");
    check_dump(tuple2(integer(2), string("t")), "{2 \"t\"}", "dump tuple");
    check_dump(dict2(atom("k"), integer(9)), "%{:k 9}", "dump dict");

    return 0;
}
