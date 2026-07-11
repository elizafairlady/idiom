#include "idiom/pattern.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *name) {
    fprintf(stderr, "byteprog: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_error(IdmError *err, const char *name) {
    if (err->present) fail(name);
}

static SelByteProg *make_prog(IdmError *err) {
    SelByteProg *p = idm_byteprog_new(err);
    if (!p) fail("new");
    idm_byteprog_set_capture_count(p, 1u);
    IdmByteClass a = {0};
    IdmByteClass b = {0};
    idm_byteclass_set(&a, 'a');
    idm_byteclass_set(&b, 'b');

    uint32_t fork = idm_byteprog_fork(p, err);
    uint32_t guard = idm_byteprog_guard(p, IDM_BYTE_GUARD_LINE_START, 0u, NULL, err);
    uint32_t save_start = idm_byteprog_save(p, 0u, err);
    uint32_t byte_a = idm_byteprog_byte(p, &a, err);
    uint32_t save_end = idm_byteprog_save(p, 1u, err);
    uint32_t accept_a = idm_byteprog_accept(p, 7u, err);
    uint32_t byte_b = idm_byteprog_byte(p, &b, err);
    uint32_t accept_b = idm_byteprog_accept(p, 9u, err);
    if (fork == SEL_NO_NODE || guard == SEL_NO_NODE || save_start == SEL_NO_NODE ||
        byte_a == SEL_NO_NODE || save_end == SEL_NO_NODE || accept_a == SEL_NO_NODE ||
        byte_b == SEL_NO_NODE || accept_b == SEL_NO_NODE) {
        fail("node");
    }
    idm_byteprog_set_fork(p, fork, guard, byte_b);
    idm_byteprog_set_start(p, fork);
    idm_byteprog_finalize_linear(p);
    check_error(err, "build");
    return p;
}

static void test_no_capture(SelByteProg *p, IdmError *err) {
    bool matched = false;
    check(idm_byteprog_test(p, "a", 1u, 0u, true, 1u, &matched, err), "test a");
    check_error(err, "test a error");
    check(matched, "test a matched");
    matched = true;
    check(idm_byteprog_test(p, "x", 1u, 0u, true, 1u, &matched, err), "test x");
    check(!matched, "test x unmatched");
    matched = false;
    check(idm_byteprog_test(p, "b", 1u, 0u, true, 1u, &matched, err), "test b");
    check(matched, "test b matched");
    matched = true;
    check(idm_byteprog_test(p, "xa", 2u, 1u, true, 2u, &matched, err), "test guard fail");
    check(!matched, "guard failed");
}

static void test_capture(SelByteProg *p, IdmError *err) {
    IdmByteMatch m = {0};
    check(idm_byteprog_match(p, "a", 1u, 0u, true, 1u, true, &m, err), "match a");
    check(m.matched, "match a matched");
    check(m.index == 7u, "match a index");
    check(m.capture_count == 1u, "match a capture count");
    check(m.captures[0].set && m.captures[0].start == 0u && m.captures[0].end == 1u, "match a capture");
    idm_byte_match_destroy(&m);

    check(idm_byteprog_match(p, "b", 1u, 0u, true, 1u, true, &m, err), "match b");
    check(m.matched, "match b matched");
    check(m.index == 9u, "match b index");
    check(m.capture_count == 1u, "match b capture count");
    check(!m.captures[0].set, "match b no capture");
    idm_byte_match_destroy(&m);
}

static void test_lookbehind_boundary(void) {
    IdmError err;
    idm_error_init(&err);
    SelByteProg *p = idm_byteprog_new(&err);
    if (!p) fail("lookbehind prog");
    check(idm_byteprog_guard(p, IDM_BYTE_GUARD_LOOKBEHIND_POS, 0u, NULL, &err) == SEL_NO_NODE, "generic lookbehind rejected");
    check(err.present, "generic lookbehind error");
    idm_error_clear(&err);
    idm_error_init(&err);
    check(idm_byteprog_lookbehind_guard(p, IDM_BYTE_GUARD_LOOKBEHIND_POS, 0u, NULL, 0u, SIZE_MAX, &err) == SEL_NO_NODE, "unbounded lookbehind rejected");
    check(err.present, "unbounded lookbehind error");
    idm_error_clear(&err);
    idm_error_init(&err);
    check(idm_byteprog_lookbehind_guard(p, IDM_BYTE_GUARD_LINE_START, 0u, NULL, 0u, 4u, &err) == SEL_NO_NODE, "non-lookbehind kind rejected");
    check(err.present, "non-lookbehind kind error");
    idm_error_clear(&err);
    idm_byteprog_free(p);
}

int idm_unit_byteprog(void) {
    IdmError err;
    idm_error_init(&err);
    SelByteProg *p = make_prog(&err);
    test_no_capture(p, &err);
    test_capture(p, &err);
    idm_byteprog_free(p);
    idm_error_clear(&err);
    test_lookbehind_boundary();
    return 0;
}
