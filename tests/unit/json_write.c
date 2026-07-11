#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "json_write: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_json(IdmValue value, const char *expected, const char *name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_value_write_json(&buf, value), name);
    if (!buf.data || strcmp(buf.data, expected) != 0) {
        fprintf(stderr, "json_write: %s: got %s want %s\n", name, buf.data ? buf.data : "<null>", expected);
        exit(1);
    }
    idm_buf_destroy(&buf);
}

int idm_unit_json_write(void) {
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmHeap heap;
    idm_heap_init(&heap);
    idm_set_active_heap(&heap);
    IdmError err;
    idm_error_init(&err);

    check_json(idm_nil(), "null", "nil and empty list are one value");
    check_json(idm_int(42), "42", "int");
    check_json(idm_int(-7), "-7", "negative int");
    check_json(idm_atom(&rt, "ok"), "\"ok\"", "atom");
    check_json(idm_bool(&rt, true), "true", "true atom is JSON boolean");
    check_json(idm_bool(&rt, false), "false", "false atom is JSON boolean");
    check_json(idm_word(&rt, "count"), "\"count\"", "word");

    bool big_ok = false;
    IdmValue big = idm_int_from_decimal(&rt, "123456789012345678901234567890", 30u, &big_ok, &err);
    check(big_ok && !err.present, "bignum build");
    check_json(big, "123456789012345678901234567890", "bignum digits");

    IdmValue text = idm_string(&rt, "a\"b\\c\nd\x01", &err);
    check(!err.present, "string build");
    check_json(text, "\"a\\\"b\\\\c\\nd\\u0001\"", "string escapes");

    IdmValue items[3] = {idm_int(1), idm_int(2), idm_int(3)};
    check_json(idm_tuple(&rt, items, 3u, &err), "[1,2,3]", "tuple");
    check_json(idm_vector(&rt, items, 2u, &err), "[1,2]", "vector");

    IdmValue list = idm_cons(&rt, idm_int(1), idm_cons(&rt, idm_int(2), idm_empty_list(), &err), &err);
    check(!err.present, "list build");
    check_json(list, "[1,2]", "list");

    IdmDictEntry named[2] = {
        {idm_atom(&rt, "name"), idm_string(&rt, "idiom", &err)},
        {idm_atom(&rt, "arity"), idm_int(2)},
    };
    IdmValue named_dict = idm_dict(&rt, named, 2u, &err);
    check(!err.present, "named dict build");
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_value_write_json(&buf, named_dict), "named dict render");
    check(buf.data && strstr(buf.data, "\"name\":\"idiom\"") && strstr(buf.data, "\"arity\":2"), "named dict entries");
    check(buf.data && buf.data[0] == '{' && buf.data[buf.len - 1u] == '}', "named dict is object");
    idm_buf_destroy(&buf);

    IdmDictEntry keyed[1] = {{idm_int(5), idm_atom(&rt, "five")}};
    IdmValue int_dict = idm_dict(&rt, keyed, 1u, &err);
    check(!err.present, "int dict build");
    check_json(int_dict, "[[5,\"five\"]]", "non-name keys render as pairs");

    IdmSymbol *type = idm_intern(&rt.intern, IDM_SYMBOL_WORD, "Point");
    IdmSymbol *fields[2] = {
        idm_intern(&rt.intern, IDM_SYMBOL_WORD, "x"),
        idm_intern(&rt.intern, IDM_SYMBOL_WORD, "y"),
    };
    IdmRecordShape *shape = idm_record_shape_intern_symbols(&rt, type, fields, 2u, &err);
    check(shape && !err.present, "record shape");
    IdmValue field_values[2] = {idm_int(3), idm_int(4)};
    IdmValue record = idm_record_from_shape(&rt, shape, field_values, &err);
    check(!err.present, "record build");
    check_json(record, "{\"%type\":\"Point\",\"x\":3,\"y\":4}", "record object");

    IdmError render_err;
    idm_error_init(&render_err);
    idm_error_set(&render_err, (IdmSpan){"probe.id", 4u, 9u, 2u, 1u}, "boom %d", 7);
    idm_error_note(&render_err, "plain note");
    idm_error_note_at(&render_err, (IdmSpan){"probe.id", 12u, 15u, 3u, 2u}, "spanned note");
    check(render_err.note_count == 2u, "note count");
    check(render_err.notes[1].span.line == 3u, "note span kept");
    idm_buf_init(&buf);
    check(idm_error_render(&render_err, &buf), "error render");
    check(buf.data && strcmp(buf.data, "probe.id:2:1: error: boom 7\n  plain note\n  spanned note (probe.id:3:2)") == 0, "error render text");
    idm_buf_destroy(&buf);
    idm_error_clear(&render_err);

    idm_set_active_heap(NULL);
    idm_heap_destroy(&heap);
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    return 0;
}
