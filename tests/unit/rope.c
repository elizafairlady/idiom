#include "idiom/regex.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "rope: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static char *pattern_bytes(size_t len) {
    char *buf = malloc(len + 1u);
    if (!buf) fail("pattern alloc");
    for (size_t i = 0; i < len; i++) buf[i] = (char)('a' + (i % 26u));
    buf[len] = '\0';
    return buf;
}

static bool string_content_is(IdmValue v, const char *expect, size_t len) {
    if (idm_string_length(v) != len) return false;
    IdmStringIter it;
    idm_string_iter_init(v, &it);
    const char *chunk = NULL;
    size_t chunk_len = 0;
    size_t at = 0;
    while (idm_string_iter_next(&it, &chunk, &chunk_len)) {
        if (at + chunk_len > len || memcmp(expect + at, chunk, chunk_len) != 0) return false;
        at += chunk_len;
    }
    return at == len;
}

int idm_unit_rope(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);

    size_t big_len = 1u << 20;
    char *big = pattern_bytes(big_len);
    IdmValue rope = idm_string_n(&rt, big, big_len, &err);
    check(!err.present, "big string builds");
    check(idm_string_length(rope) == big_len, "big string length");
    check(string_content_is(rope, big, big_len), "big string content via iterator");

    IdmStringIter it;
    idm_string_iter_init(rope, &it);
    const char *chunk = NULL;
    size_t chunk_len = 0;
    size_t chunks = 0;
    while (idm_string_iter_next(&it, &chunk, &chunk_len)) {
        check(chunk_len <= 2048u, "leaf bounded");
        chunks++;
    }
    check(chunks >= big_len / 2048u, "chunked at birth");

    const char *flat = idm_string_bytes(rope);
    check(flat && memcmp(flat, big, big_len) == 0 && flat[big_len] == '\0', "flatten cache content");
    check(flat == idm_string_bytes(rope), "flatten cache stable");

    IdmValue mid = idm_string_slice_value(&rt, rope, 100000u, 900000u, &err);
    check(!err.present, "slice builds");
    check(string_content_is(mid, big + 100000u, 800000u), "slice content");

    IdmValue whole = idm_string_slice_value(&rt, rope, 0u, big_len, &err);
    check(!err.present && idm_value_equal(whole, rope), "whole slice identity");

    IdmValue left = idm_string_slice_value(&rt, rope, 0u, 500000u, &err);
    IdmValue right = idm_string_slice_value(&rt, rope, 500000u, big_len, &err);
    check(!err.present, "split slices build");
    IdmValue joined = idm_nil();
    check(idm_string_concat2(&rt, left, right, &joined, &err), "concat joins");
    check(string_content_is(joined, big, big_len), "concat content");
    check(idm_value_equal(joined, rope), "rope equal rope");

    IdmValue flat_small = idm_string_n(&rt, big, 2000u, &err);
    IdmValue rope_small = idm_string_slice_value(&rt, rope, 0u, 2000u, &err);
    check(!err.present, "small pair builds");
    check(idm_value_equal(flat_small, rope_small), "flat equals sliced");

    IdmValue key_flat = idm_string_n(&rt, big, 5000u, &err);
    IdmValue key_rope = idm_string_slice_value(&rt, rope, 0u, 5000u, &err);
    IdmDictEntry entry = { key_flat, idm_int(42) };
    IdmValue d = idm_dict(&rt, &entry, 1u, &err);
    check(!err.present, "dict builds");
    IdmValue got = idm_nil();
    check(idm_dict_get(d, key_rope, &got) && idm_int_value(got) == 42, "rope key hashes like flat key");

    IdmValue edited = idm_nil();
    IdmValue ins = idm_string(&rt, "XYZ", &err);
    IdmValue pre = idm_string_slice_value(&rt, rope, 0u, 12345u, &err);
    IdmValue post = idm_string_slice_value(&rt, rope, 12345u, big_len, &err);
    check(idm_string_concat2(&rt, pre, ins, &edited, &err), "edit concat 1");
    check(idm_string_concat2(&rt, edited, post, &edited, &err), "edit concat 2");
    check(idm_string_length(edited) == big_len + 3u, "edit length");
    IdmValue probe = idm_string_slice_value(&rt, edited, 12345u, 12348u, &err);
    check(!err.present && string_content_is(probe, "XYZ", 3u), "edit content at point");

    IdmValue acc = idm_string(&rt, "", &err);
    char piece[16];
    for (size_t i = 0; i < 4000u; i++) {
        snprintf(piece, sizeof(piece), "%zu,", i % 10u);
        IdmValue p = idm_string(&rt, piece, &err);
        check(!err.present, "piece builds");
        check(idm_string_concat2(&rt, acc, p, &acc, &err), "repeated concat");
    }
    check(idm_string_length(acc) == 8000u, "repeated concat length");
    const char *acc_flat = idm_string_bytes(acc);
    check(acc_flat && acc_flat[0] == '0' && acc_flat[1] == ',' && acc_flat[7998] == '9', "repeated concat content");

    idm_string_iter_init(acc, &it);
    size_t acc_chunks = 0;
    while (idm_string_iter_next(&it, &chunk, &chunk_len)) acc_chunks++;
    check(acc_chunks <= 8000u / 1024u + 2u, "appends merge at spine");

    IdmRegex *rx = NULL;
    check(idm_regex_compile("X[A-Z]+", 7u, 0u, &rx, &err) && !err.present, "regex compiles");
    IdmValue rxv = idm_regex_value(&rt, rx, &err);
    check(!err.present, "regex value");
    size_t heap_before = idm_heap_bytes(&rt.immortal);
    IdmValue found = idm_nil();
    check(idm_regex_scan_from(&rt, rxv, edited, 0u, &found, &err) && !err.present, "regex scans rope");
    check(!idm_is_nil(found), "regex finds match in rope");
    size_t heap_after = idm_heap_bytes(&rt.immortal);
    check(heap_after - heap_before < idm_string_length(edited) / 2u, "regex scan does not flatten rope");
    IdmValue mstart = idm_nil();
    check(idm_regex_result_start_value(&rt, found, &mstart, &err) && idm_int_value(mstart) == 12345, "regex match position");
    IdmValue mtext = idm_nil();
    check(idm_regex_result_text_value(&rt, found, &mtext, &err) && !err.present, "regex match text");
    check(string_content_is(mtext, "XYZ", 3u), "regex match content");
    IdmValue tested = idm_nil();
    check(idm_regex_test(&rt, rxv, edited, &tested, &err) && !err.present, "regex test on rope");
    check(idm_value_equal(tested, idm_bool(&rt, true)), "regex test truth");

    size_t lines_len = 1u << 20;
    char *liney = malloc(lines_len);
    if (!liney) fail("liney alloc");
    for (size_t i = 0; i < lines_len; i++) liney[i] = (i % 100u == 99u) ? '\n' : 'x';
    IdmValue lrope = idm_string_n(&rt, liney, lines_len, &err);
    check(!err.present, "liney rope builds");
    size_t total_lines = lines_len / 100u;
    check(idm_string_newlines_before(lrope, lines_len) == total_lines, "rope newline total");
    check(idm_string_newlines_before(lrope, 0) == 0u, "newlines before zero");
    check(idm_string_newlines_before(lrope, 12345u) == 123u, "newlines before mid");
    size_t lstart = 0;
    check(idm_string_line_start(lrope, 0u, &lstart) && lstart == 0u, "line 0 start");
    check(idm_string_line_start(lrope, 123u, &lstart) && lstart == 12300u, "line 123 start");
    check(idm_string_line_start(lrope, total_lines, &lstart) && lstart == total_lines * 100u, "last line start");
    check(!idm_string_line_start(lrope, total_lines + 1u, &lstart), "line past end nil");

    IdmValue lpre = idm_string_slice_value(&rt, lrope, 0u, 500u, &err);
    IdmValue lnl = idm_string(&rt, "\n", &err);
    IdmValue lpost = idm_string_slice_value(&rt, lrope, 500u, lines_len, &err);
    IdmValue ledit = idm_nil();
    check(idm_string_concat2(&rt, lpre, lnl, &ledit, &err), "line edit concat 1");
    check(idm_string_concat2(&rt, ledit, lpost, &ledit, &err), "line edit concat 2");
    check(idm_string_newlines_before(ledit, lines_len + 1u) == total_lines + 1u, "edit adds line");
    check(idm_string_line_start(ledit, 6u, &lstart) && lstart == 501u, "edit shifts line starts");

    IdmValue lflat = idm_string(&rt, "a\nb\nc", &err);
    check(idm_string_newlines_before(lflat, 5u) == 2u, "flat newline count");
    check(idm_string_line_start(lflat, 1u, &lstart) && lstart == 2u, "flat line 1");
    check(idm_string_line_start(lflat, 2u, &lstart) && lstart == 4u, "flat line 2");
    check(!idm_string_line_start(lflat, 3u, &lstart), "flat line past end");
    free(liney);

    free(big);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}
