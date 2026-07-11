#include "idiom/regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "regex_set: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static IdmRegex *compile_rx(const char *src, IdmError *err) {
    IdmRegex *rx = NULL;
    check(idm_regex_compile(src, strlen(src), 0u, &rx, err), src);
    check_error(err, src);
    return rx;
}

static void check_range(IdmByteCapture c, size_t start, size_t end, const char *name) {
    check(c.set, name);
    check(c.start == start, name);
    check(c.end == end, name);
}

int idm_unit_regex_set(void) {
    IdmError err;
    idm_error_init(&err);
    IdmRegex *first = compile_rx("(?<word>[a-z]+)-(?<num>[0-9]+)", &err);
    IdmRegex *second = compile_rx("(?<upper>[A-Z]+)", &err);
    const IdmRegex *items[2] = { first, second };
    IdmRegexSet *set = NULL;
    check(idm_regex_set_compile(items, 2u, &set, &err), "compile set");
    check_error(&err, "compile set error");

    size_t index = 99u;
    size_t end = 99u;
    bool matched = false;
    check(idm_regex_set_match_at(set, "abc-42", 6u, 0u, &index, &end, &matched, &err), "match at first");
    check_error(&err, "match at first error");
    check(matched && index == 0u && end == 6u, "match at first result");

    IdmByteMatch m = {0};
    check(idm_regex_set_exec_at(set, "abc-42", 6u, 0u, &m, &err), "exec first");
    check_error(&err, "exec first error");
    check(m.matched && m.index == 0u && m.end == 6u && m.capture_count == 3u, "exec first result");
    check_range(m.captures[0], 0u, 6u, "first whole");
    check_range(m.captures[1], 0u, 3u, "first word");
    check_range(m.captures[2], 4u, 6u, "first num");
    idm_byte_match_destroy(&m);

    check(idm_regex_set_exec_at(set, "XYZ", 3u, 0u, &m, &err), "exec second");
    check_error(&err, "exec second error");
    check(m.matched && m.index == 1u && m.end == 3u && m.capture_count == 2u, "exec second result");
    check_range(m.captures[0], 0u, 3u, "second whole");
    check_range(m.captures[1], 0u, 3u, "second upper");
    idm_byte_match_destroy(&m);

    check(idm_regex_set_exec_at(set, "!", 1u, 0u, &m, &err), "exec none");
    check_error(&err, "exec none error");
    check(!m.matched && m.end == 0u && m.capture_count == 0u && m.captures == NULL, "exec none result");
    idm_byte_match_destroy(&m);

    idm_regex_set_free(set);
    idm_regex_free(first);
    idm_regex_free(second);
    idm_error_clear(&err);
    return 0;
}
