#include "idiom/common.h"
#include "idiom/scope.h"
#include "idiom/syntax.h"
#include "idiom/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *name) {
    fprintf(stderr, "wire_helpers: %s\n", name);
    exit(1);
}

static void check(bool ok, const char *name) {
    if (!ok) fail(name);
}

static void check_roundtrip(void) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_buf_put_opt_str(&buf, NULL), "put null");
    check(idm_buf_put_opt_str(&buf, "value"), "put value");

    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)buf.data, buf.len);
    IdmError err;
    idm_error_init(&err);
    char *out = (char *)1;
    check(idm_rd_opt_str(&r, &out, &err), "read null");
    check(out == NULL, "null output");
    check(idm_rd_opt_str(&r, &out, &err), "read value");
    check(out && strcmp(out, "value") == 0, "value output");
    free(out);
    check(r.ok && r.pos == r.len, "reader consumed");
    idm_error_clear(&err);
    idm_buf_destroy(&buf);
}

static void check_invalid_flag(void) {
    unsigned char data[] = { 2u };
    IdmByteReader r;
    idm_byte_reader_init(&r, data, sizeof(data));
    IdmError err;
    idm_error_init(&err);
    char *out = NULL;
    check(!idm_rd_opt_str(&r, &out, &err), "invalid flag rejected");
    check(err.present && !r.ok && out == NULL, "invalid flag state");
    idm_error_clear(&err);
}

static void check_truncated_string(void) {
    unsigned char data[] = { 1u, 0u, 0u, 0u, 5u, 'x' };
    IdmByteReader r;
    idm_byte_reader_init(&r, data, sizeof(data));
    IdmError err;
    idm_error_init(&err);
    char *out = NULL;
    check(!idm_rd_opt_str(&r, &out, &err), "truncated string rejected");
    check(err.present && !r.ok && out == NULL, "truncated string state");
    idm_error_clear(&err);
}

static void check_invalid_syntax_file_flag(void) {
    unsigned char data[] = { (unsigned char)IDM_SYN_NIL, 2u };
    IdmByteReader r;
    idm_byte_reader_init(&r, data, sizeof(data));
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *syn = idm_syn_deserialize(&rt, &r, &err);
    check(!syn, "syntax invalid file flag rejected");
    check(err.present && !r.ok, "syntax invalid file flag state");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
}

static void check_invalid_syntax_token_flag(void) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    check(idm_buf_put_u8(&buf, (uint8_t)IDM_SYN_NIL), "syntax kind");
    check(idm_buf_put_opt_str(&buf, NULL), "syntax file null");
    check(idm_buf_put_u64(&buf, 0u), "syntax span start");
    check(idm_buf_put_u64(&buf, 0u), "syntax span end");
    check(idm_buf_put_u32(&buf, 0u), "syntax line");
    check(idm_buf_put_u32(&buf, 0u), "syntax column");
    check(idm_buf_put_u32(&buf, 0u), "syntax phase count");
    check(idm_buf_put_u8(&buf, 2u), "syntax invalid token flag");

    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)buf.data, buf.len);
    IdmRuntime rt;
    idm_runtime_init(&rt);
    IdmError err;
    idm_error_init(&err);
    IdmSyntax *syn = idm_syn_deserialize(&rt, &r, &err);
    check(!syn, "syntax invalid token flag rejected");
    check(err.present && !r.ok, "syntax invalid token flag state");
    idm_error_clear(&err);
    idm_runtime_destroy(&rt);
    idm_buf_destroy(&buf);
}

static void check_arity_roundtrip(IdmArity arity, const char *name) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    IdmError err;
    idm_error_init(&err);
    check(idm_arity_serialize(&buf, arity, &err), name);
    check(buf.len == 17u, "arity wire width");
    IdmByteReader r;
    idm_byte_reader_init(&r, (const unsigned char *)buf.data, buf.len);
    IdmArity out = idm_arity_unknown();
    check(idm_arity_deserialize(&r, &out, &err), name);
    check(idm_arity_equal(&arity, &out), name);
    check(r.ok && r.pos == r.len, "arity reader consumed");
    idm_error_clear(&err);
    idm_buf_destroy(&buf);
}

static void check_arity_wire(void) {
    IdmArity set = idm_arity_unknown();
    check(idm_arity_add_exact(&set, 1u), "arity set add 1");
    check(idm_arity_add_exact(&set, 3u), "arity set add 3");
    check_arity_roundtrip(idm_arity_unknown(), "arity unknown");
    check_arity_roundtrip(idm_arity_range(2u, 5u), "arity range");
    check_arity_roundtrip(set, "arity set");
}

static void check_invalid_arity_kind(void) {
    unsigned char data[17] = { 99u };
    IdmByteReader r;
    idm_byte_reader_init(&r, data, sizeof(data));
    IdmError err;
    idm_error_init(&err);
    IdmArity out = idm_arity_unknown();
    check(!idm_arity_deserialize(&r, &out, &err), "invalid arity kind rejected");
    check(err.present && !r.ok, "invalid arity kind state");
    idm_error_clear(&err);
}

static void check_truncated_arity(void) {
    unsigned char data[] = { (unsigned char)IDM_ARITY_SET, 0u };
    IdmByteReader r;
    idm_byte_reader_init(&r, data, sizeof(data));
    IdmError err;
    idm_error_init(&err);
    IdmArity out = idm_arity_unknown();
    check(!idm_arity_deserialize(&r, &out, &err), "truncated arity rejected");
    check(err.present && !r.ok, "truncated arity state");
    idm_error_clear(&err);
}

int idm_unit_wire_helpers(void) {
    check_roundtrip();
    check_invalid_flag();
    check_truncated_string();
    check_invalid_syntax_file_flag();
    check_invalid_syntax_token_flag();
    check_arity_wire();
    check_invalid_arity_kind();
    check_truncated_arity();
    return 0;
}
