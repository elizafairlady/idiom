#define _XOPEN_SOURCE 700

#include "idiom/prims.h"
#include "idiom/bytecode.h"
#include "idiom/expand.h"
#include "idiom/syntax.h"
#include "idiom/vm.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool require_arity(IdmPrimitive prim, uint32_t argc, IdmError *err) {
    const IdmPrimitiveInfo *info = idm_primitive_info(prim);
    if (!info) return idm_error_set(err, idm_span_unknown(NULL), "unknown primitive %u", (unsigned)prim);
    if (argc < info->min_arity || argc > info->max_arity) {
        return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' arity mismatch: got %u, want %u..%u", info->name, argc, info->min_arity, info->max_arity);
    }
    return true;
}

static IdmSyntax *require_syntax(IdmValue value, IdmError *err) {
    IdmSyntax *syn = idm_syntax_value_get(value);
    if (!syn) {
        idm_error_set(err, idm_span_unknown(NULL), "expected a syntax value");
        return NULL;
    }
    return syn;
}

static const char *require_string(IdmValue v, size_t *out_len, IdmError *err);

static bool prim_cons(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    *out = idm_cons(rt, args[0], args[1], err);
    return !(err && err->present);
}

static bool prim_first(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (!idm_is_pair(args[0])) return idm_error_set(err, idm_span_unknown(NULL), "first expects a pair");
    *out = idm_car(args[0], err);
    return !(err && err->present);
}

static bool prim_rest(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (!idm_is_pair(args[0])) return idm_error_set(err, idm_span_unknown(NULL), "rest expects a pair");
    *out = idm_cdr(args[0], err);
    return !(err && err->present);
}

static bool prim_list(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    IdmValue result = idm_nil();
    for (size_t i = argc; i > 0; i--) {
        result = idm_cons(rt, args[i - 1u], result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_append(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue head = args[0];
    IdmValue tail = args[1];
    if (idm_is_nil(head)) { *out = tail; return true; }
    if (!idm_is_pair(head)) return idm_error_set(err, idm_span_unknown(NULL), "append expects a list as first argument");
    size_t count = 0;
    IdmValue cur = head;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_nil(cur)) return idm_error_set(err, idm_span_unknown(NULL), "append expects a proper list as first argument");
    IdmValue *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
    cur = head;
    for (size_t i = 0; i < count; i++) {
        items[i] = idm_car(cur, err);
        if (err && err->present) { free(items); return false; }
        cur = idm_cdr(cur, err);
        if (err && err->present) { free(items); return false; }
    }
    IdmValue result = tail;
    for (size_t i = count; i > 0; i--) {
        result = idm_cons(rt, items[i - 1u], result, err);
        if (err && err->present) { free(items); return false; }
    }
    free(items);
    *out = result;
    return true;
}

static bool prim_tuple(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    *out = idm_tuple(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_vector(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    *out = idm_vector(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_dict(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (argc % 2u != 0) return idm_error_set(err, idm_span_unknown(NULL), "dict expects an even number of key/value arguments");
    size_t count = argc / 2u;
    IdmDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
    if (count != 0 && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
    for (size_t i = 0; i < count; i++) {
        entries[i].key = args[i * 2u];
        entries[i].value = args[i * 2u + 1u];
    }
    *out = idm_dict(rt, entries, count, err);
    free(entries);
    return !(err && err->present);
}

static bool append_display(IdmBuffer *buf, IdmValue v) {
    if (v.tag == IDM_VAL_STRING) return idm_buf_append_n(buf, idm_string_bytes(v), idm_string_length(v));
    if (v.tag == IDM_VAL_ATOM || v.tag == IDM_VAL_WORD) return idm_buf_append(buf, idm_symbol_text(v.as.symbol));
    if (v.tag == IDM_VAL_NIL) return true;
    return idm_value_write(buf, v);
}

static bool prim_str(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (argc == 1 && args[0].tag == IDM_VAL_STRING) { *out = args[0]; return true; }
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (uint32_t i = 0; i < argc; i++) {
        if (!append_display(&buf, args[i])) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

static bool prim_chomp(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_STRING) { *out = args[0]; return true; }
    const char *bytes = idm_string_bytes(args[0]);
    size_t len = idm_string_length(args[0]);
    while (len > 0 && (bytes[len - 1u] == '\n' || bytes[len - 1u] == '\r')) len--;
    *out = idm_string_n(rt, bytes, len, err);
    return !(err && err->present);
}

static bool prim_capture_stdout(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue result = args[0];
    if (!idm_is_tuple(result) || idm_sequence_count(result) < 3) return idm_error_set(err, idm_span_unknown(NULL), "capture-stdout expects a command result tuple");
    IdmError ignore;
    idm_error_init(&ignore);
    IdmValue tag = idm_sequence_item(result, 0, &ignore);
    IdmValue reason = idm_sequence_item(result, 1, &ignore);
    IdmValue stdout_v = idm_sequence_item(result, 2, &ignore);
    idm_error_clear(&ignore);
    if (tag.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(tag.as.symbol), "error") == 0 &&
        reason.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(reason.as.symbol), "capture-overflow") == 0) {
        return idm_error_set(err, idm_span_unknown(NULL), "command output exceeded the capture limit");
    }
    if (stdout_v.tag != IDM_VAL_STRING) { *out = stdout_v; return true; }
    const char *bytes = idm_string_bytes(stdout_v);
    size_t len = idm_string_length(stdout_v);
    while (len > 0 && (bytes[len - 1u] == '\n' || bytes[len - 1u] == '\r')) len--;
    *out = idm_string_n(rt, bytes, len, err);
    return !(err && err->present);
}

static bool prim_tuple_get(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (!idm_is_tuple(args[0])) return idm_error_set(err, idm_span_unknown(NULL), "tuple-get expects a tuple");
    if (args[1].tag != IDM_VAL_INT || args[1].as.i < 0) return idm_error_set(err, idm_span_unknown(NULL), "tuple-get index must be a non-negative integer");
    size_t index = (size_t)args[1].as.i;
    if (index >= idm_sequence_count(args[0])) return idm_error_set(err, idm_span_unknown(NULL), "tuple-get index out of range");
    *out = idm_sequence_item(args[0], index, err);
    return !(err && err->present);
}

static bool prim_to_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (idm_is_nil(v) || idm_is_pair(v)) { *out = v; return true; }
    if (idm_is_vector(v) || idm_is_tuple(v)) {
        size_t count = idm_sequence_count(v);
        IdmValue result = idm_nil();
        for (size_t i = count; i > 0; i--) {
            IdmValue item = idm_sequence_item(v, i - 1u, err);
            if (err && err->present) return false;
            result = idm_cons(rt, item, result, err);
            if (err && err->present) return false;
        }
        *out = result;
        return true;
    }
    if (idm_is_dict(v)) {
        size_t count = idm_dict_count(v);
        IdmValue result = idm_nil();
        for (size_t i = count; i > 0; i--) {
            IdmValue key;
            IdmValue val;
            if (!idm_dict_entry(v, i - 1u, &key, &val)) return idm_error_set(err, idm_span_unknown(NULL), "to-list dict iteration failed");
            IdmValue pair_items[2] = {key, val};
            IdmValue pair = idm_tuple(rt, pair_items, 2u, err);
            if (err && err->present) return false;
            result = idm_cons(rt, pair, result, err);
            if (err && err->present) return false;
        }
        *out = result;
        return true;
    }
    return idm_error_set(err, idm_span_unknown(NULL), "to-list expects a list, vector, tuple, or dict");
}

static bool prim_cd(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
    if (!rt->current_exec) return idm_error_set(err, idm_span_unknown(NULL), "cd requires an actor context");
    char *path = idm_strndup(bytes, len);
    if (!path) return idm_error_oom(err, idm_span_unknown(NULL));
    char *candidate = NULL;
    if (path[0] == '/') {
        candidate = path;
        path = NULL;
    } else {
        const char *base = idm_exec_cwd(rt->current_exec);
        char buf[PATH_MAX];
        if (!base) {
            if (!getcwd(buf, sizeof(buf))) {
                free(path);
                return idm_error_set(err, idm_span_unknown(NULL), "cd: %s", strerror(errno));
            }
            base = buf;
        }
        IdmBuffer joined;
        idm_buf_init(&joined);
        if (!idm_buf_append(&joined, base) || !idm_buf_append_char(&joined, '/') || !idm_buf_append(&joined, path)) {
            idm_buf_destroy(&joined);
            free(path);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        free(path);
        candidate = idm_buf_take(&joined);
        if (!candidate) return idm_error_oom(err, idm_span_unknown(NULL));
    }
    char resolved[PATH_MAX];
    if (!realpath(candidate, resolved)) {
        idm_error_set(err, idm_span_unknown(NULL), "cd: %s: %s", candidate, strerror(errno));
        free(candidate);
        return false;
    }
    free(candidate);
    if (!idm_exec_set_cwd(rt->current_exec, resolved)) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_pwd(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)args;
    const char *cwd = idm_exec_cwd(rt->current_exec);
    if (cwd) {
        *out = idm_string(rt, cwd, err);
        return !(err && err->present);
    }
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return idm_error_set(err, idm_span_unknown(NULL), "pwd: %s", strerror(errno));
    *out = idm_string(rt, buf, err);
    return !(err && err->present);
}

static bool prim_env_get(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
    char *name = idm_strndup(bytes, len);
    if (!name) return idm_error_oom(err, idm_span_unknown(NULL));
    const char *value = idm_exec_env_get(rt->current_exec, name);
    if (!value) value = getenv(name);
    free(name);
    if (!value) {
        *out = idm_nil();
        return true;
    }
    *out = idm_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_env_set(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t name_len = 0;
    const char *name_bytes = require_string(args[0], &name_len, err);
    if (!name_bytes) return false;
    size_t value_len = 0;
    const char *value_bytes = require_string(args[1], &value_len, err);
    if (!value_bytes) return false;
    if (!rt->current_exec) return idm_error_set(err, idm_span_unknown(NULL), "env-set requires an actor context");
    char *name = idm_strndup(name_bytes, name_len);
    char *value = idm_strndup(value_bytes, value_len);
    bool ok = name && value && idm_exec_env_set(rt->current_exec, name, value);
    free(name);
    free(value);
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_make_procsub_temp(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)args;
    char tmpl[] = "/tmp/idm_procsub_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return idm_error_set(err, idm_span_unknown(NULL), "process substitution: mkstemp failed: %s", strerror(errno));
    close(fd);
    if (!idm_runtime_own_temp(rt, tmpl)) {
        unlink(tmpl);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    *out = idm_string(rt, tmpl, err);
    return !(err && err->present);
}

static const char *name_value_text(IdmValue value, IdmError *err, const char *what) {
    if (value.tag == IDM_VAL_ATOM || value.tag == IDM_VAL_WORD) return idm_symbol_text(value.as.symbol);
    if (value.tag == IDM_VAL_STRING) return idm_string_bytes(value);
    idm_error_set(err, idm_span_unknown(NULL), "%s must be an atom, word, or string", what);
    return NULL;
}

static bool prim_make_record(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *type = name_value_text(args[0], err, "record type");
    if (!type) return false;
    if (!idm_is_dict(args[1])) return idm_error_set(err, idm_span_unknown(NULL), "make-record fields must be a dict");
    *out = idm_record(rt, type, args[1], err);
    return !(err && err->present);
}

static bool prim_record_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_atom(rt, idm_is_record(args[0]) ? "true" : "false");
    return true;
}

static bool prim_record_type(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *type = idm_record_type(args[0], err);
    if (!type) return false;
    *out = idm_atom(rt, type);
    return true;
}

static bool prim_record_field(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    return idm_record_field(args[0], args[1], out, err);
}

static bool prim_write_procsub_temp(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_tuple(args[0]) || idm_sequence_count(args[0]) < 3) return idm_error_set(err, idm_span_unknown(NULL), "process substitution: expected a command result tuple");
    IdmError ig;
    idm_error_init(&ig);
    IdmValue stdout_val = idm_sequence_item(args[0], 2, &ig);
    idm_error_clear(&ig);
    size_t len = stdout_val.tag == IDM_VAL_STRING ? idm_string_length(stdout_val) : 0;
    const char *bytes = stdout_val.tag == IDM_VAL_STRING ? idm_string_bytes(stdout_val) : "";
    char tmpl[] = "/tmp/idm_procsub_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return idm_error_set(err, idm_span_unknown(NULL), "process substitution: mkstemp failed: %s", strerror(errno));
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w < 0) { close(fd); unlink(tmpl); return idm_error_set(err, idm_span_unknown(NULL), "process substitution: write failed: %s", strerror(errno)); }
        off += (size_t)w;
    }
    close(fd);
    if (!idm_runtime_own_temp(rt, tmpl)) {
        unlink(tmpl);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    *out = idm_string(rt, tmpl, err);
    return !(err && err->present);
}

static bool prim_print_impl(IdmRuntime *rt, IdmValue *args, uint32_t argc, bool newline, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (uint32_t i = 0; i < argc; i++) {
        if (i != 0 && !idm_buf_append_char(&buf, ' ')) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
        if (!append_display(&buf, args[i])) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (newline && !idm_buf_append_char(&buf, '\n')) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (buf.len != 0) fwrite(buf.data, 1u, buf.len, stdout);
    fflush(stdout);
    idm_buf_destroy(&buf);
    *out = idm_nil();
    return true;
}

static bool checked_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool checked_sub(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
}

static bool checked_mul(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return true; }
    if (a == -1 && b == INT64_MIN) return false;
    if (b == -1 && a == INT64_MIN) return false;
    int64_t r = a * b;
    if (r / b != a) return false;
    *out = r;
    return true;
}

static bool checked_pow(int64_t base, int64_t exponent, int64_t *out) {
    if (exponent < 0) return false;
    int64_t result = 1;
    int64_t factor = base;
    int64_t exp = exponent;
    while (exp > 0) {
        if ((exp & 1) != 0) {
            if (!checked_mul(result, factor, &result)) return false;
        }
        exp >>= 1;
        if (exp != 0 && !checked_mul(factor, factor, &factor)) return false;
    }
    *out = result;
    return true;
}

static bool require_int_pair(const char *name, IdmValue *args, int64_t *a, int64_t *b, IdmError *err) {
    if (args[0].tag != IDM_VAL_INT || args[1].tag != IDM_VAL_INT) return idm_error_set(err, idm_span_unknown(NULL), "%s expects integer operands", name);
    *a = args[0].as.i;
    *b = args[1].as.i;
    return true;
}

static bool prim_add(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("add", args, &a, &b, err)) return false;
    if (!checked_add(a, b, &r)) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in add");
    *out = idm_int(r);
    return true;
}

static bool prim_sub(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("sub", args, &a, &b, err)) return false;
    if (!checked_sub(a, b, &r)) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in sub");
    *out = idm_int(r);
    return true;
}

static bool prim_mul(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("mul", args, &a, &b, err)) return false;
    if (!checked_mul(a, b, &r)) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in mul");
    *out = idm_int(r);
    return true;
}

static bool num_as_double(IdmValue v, double *out) {
    if (v.tag == IDM_VAL_INT) { *out = (double)v.as.i; return true; }
    if (v.tag == IDM_VAL_FLOAT) { *out = v.as.f; return true; }
    return false;
}

static bool prim_div(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (args[0].tag == IDM_VAL_FLOAT || args[1].tag == IDM_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return idm_error_set(err, idm_span_unknown(NULL), "div expects numeric operands");
        if (y == 0.0) return idm_error_set(err, idm_span_unknown(NULL), "division by zero in div");
        *out = idm_float(x / y);
        return true;
    }
    int64_t a = 0, b = 0;
    if (!require_int_pair("div", args, &a, &b, err)) return false;
    if (b == 0) return idm_error_set(err, idm_span_unknown(NULL), "division by zero in div");
    if (a == INT64_MIN && b == -1) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in div");
    *out = idm_int(a / b);
    return true;
}

static bool prim_mod(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (args[0].tag == IDM_VAL_FLOAT || args[1].tag == IDM_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return idm_error_set(err, idm_span_unknown(NULL), "mod expects numeric operands");
        if (y == 0.0) return idm_error_set(err, idm_span_unknown(NULL), "division by zero in mod");
        *out = idm_float(fmod(x, y));
        return true;
    }
    int64_t a = 0, b = 0;
    if (!require_int_pair("mod", args, &a, &b, err)) return false;
    if (b == 0) return idm_error_set(err, idm_span_unknown(NULL), "division by zero in mod");
    if (a == INT64_MIN && b == -1) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in mod");
    *out = idm_int(a % b);
    return true;
}

static bool prim_pow(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (args[0].tag == IDM_VAL_FLOAT || args[1].tag == IDM_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return idm_error_set(err, idm_span_unknown(NULL), "pow expects numeric operands");
        *out = idm_float(pow(x, y));
        return true;
    }
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("pow", args, &a, &b, err)) return false;
    if (b < 0) return idm_error_set(err, idm_span_unknown(NULL), "pow exponent must be non-negative");
    if (!checked_pow(a, b, &r)) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in pow");
    *out = idm_int(r);
    return true;
}

static bool prim_neg(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    if (args[0].tag == IDM_VAL_FLOAT) { *out = idm_float(-args[0].as.f); return true; }
    if (args[0].tag != IDM_VAL_INT) return idm_error_set(err, idm_span_unknown(NULL), "neg expects a number");
    if (args[0].as.i == INT64_MIN) return idm_error_set(err, idm_span_unknown(NULL), "integer overflow in neg");
    *out = idm_int(-args[0].as.i);
    return true;
}

static bool prim_eq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_atom(rt, idm_value_equal(args[0], args[1]) ? "true" : "false");
    return true;
}

static bool prim_neq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_atom(rt, idm_value_equal(args[0], args[1]) ? "false" : "true");
    return true;
}

static bool prim_lt(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("lt?", args, &a, &b, err)) return false;
    *out = idm_atom(rt, a < b ? "true" : "false");
    return true;
}

static bool prim_gt(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("gt?", args, &a, &b, err)) return false;
    *out = idm_atom(rt, a > b ? "true" : "false");
    return true;
}

static bool prim_lte(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("lte?", args, &a, &b, err)) return false;
    *out = idm_atom(rt, a <= b ? "true" : "false");
    return true;
}

static bool prim_gte(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("gte?", args, &a, &b, err)) return false;
    *out = idm_atom(rt, a >= b ? "true" : "false");
    return true;
}

static const char *kind_atom_text(IdmSyntaxKind kind) {
    switch (kind) {
        case IDM_SYN_NIL: return "nil";
        case IDM_SYN_WORD: return "word";
        case IDM_SYN_ATOM: return "atom";
        case IDM_SYN_INT: return "int";
        case IDM_SYN_FLOAT: return "float";
        case IDM_SYN_STRING: return "string";
        case IDM_SYN_LIST: return "list";
        case IDM_SYN_VECTOR: return "vector";
        case IDM_SYN_TUPLE: return "tuple";
        case IDM_SYN_DICT: return "dict";
    }
    return "unknown";
}

static bool prim_syntax_kind(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = idm_atom(rt, kind_atom_text(syn->kind));
    return true;
}

static bool prim_syntax_to_datum_impl(IdmRuntime *rt, IdmSyntax *syn, IdmValue *out, IdmError *err) {
    switch (syn->kind) {
        case IDM_SYN_NIL: *out = idm_nil(); return true;
        case IDM_SYN_WORD: *out = idm_word(rt, syn->as.text); return true;
        case IDM_SYN_ATOM: *out = idm_atom(rt, syn->as.text); return true;
        case IDM_SYN_INT: *out = idm_int(syn->as.integer); return true;
        case IDM_SYN_FLOAT: *out = idm_float(syn->as.real); return true;
        case IDM_SYN_STRING: *out = idm_string(rt, syn->as.text, err); return !(err && err->present);
        case IDM_SYN_LIST: {
            IdmValue list = idm_nil();
            for (size_t i = syn->as.seq.count; i > 0; i--) {
                IdmValue item = idm_nil();
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i - 1u], &item, err)) return false;
                list = idm_cons(rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            return true;
        }
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE: {
            IdmValue *items = syn->as.seq.count == 0 ? NULL : calloc(syn->as.seq.count, sizeof(*items));
            if (syn->as.seq.count != 0 && !items) return idm_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i], &items[i], err)) { free(items); return false; }
            }
            *out = syn->kind == IDM_SYN_VECTOR ? idm_vector(rt, items, syn->as.seq.count, err) : idm_tuple(rt, items, syn->as.seq.count, err);
            free(items);
            return !(err && err->present);
        }
        case IDM_SYN_DICT: {
            if (syn->as.seq.count % 2u != 0) return idm_error_set(err, syn->span, "dict syntax requires key/value pairs");
            size_t count = syn->as.seq.count / 2u;
            IdmDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) return idm_error_oom(err, syn->span);
            for (size_t i = 0; i < count; i++) {
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i * 2u], &entries[i].key, err)) { free(entries); return false; }
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i * 2u + 1u], &entries[i].value, err)) { free(entries); return false; }
            }
            *out = idm_dict(rt, entries, count, err);
            free(entries);
            return !(err && err->present);
        }
    }
    return idm_error_set(err, syn->span, "unknown syntax kind");
}

static bool prim_syntax_to_datum(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    return prim_syntax_to_datum_impl(rt, syn, out, err);
}

static bool wrap_owned_syntax(IdmRuntime *rt, IdmSyntax *syn, IdmValue *out, IdmError *err) {
    if (!syn) return false;
    *out = idm_syntax_value(rt, syn, err);
    idm_syn_free(syn);
    return !(err && err->present);
}

static IdmSyntax *syntax_from_datum_impl(IdmSyntax *ctx, IdmValue datum, IdmError *err);

static bool copy_scopes_from(IdmSyntax *dst, const IdmSyntax *src) {
    if (!src) return true;
    for (size_t i = 0; i < src->scopes.count; i++) {
        const IdmSyntaxPhaseScope *p = &src->scopes.items[i];
        for (size_t j = 0; j < p->scopes.count; j++) {
            if (!idm_syn_scope_add(dst, p->phase, p->scopes.items[j])) return false;
        }
    }
    return true;
}

static bool add_active_intro_scope(IdmRuntime *rt, IdmSyntax *syn) {
    if (!rt->macro_intro_active) return true;
    const IdmScopeSet *set = idm_syn_scope_set(syn, 0);
    if (!set || !idm_scope_set_contains(set, rt->macro_intro_scope)) return true;
    return idm_syn_scope_flip(syn, 0, rt->macro_intro_scope);
}

static IdmSyntax *syntax_from_datum_impl(IdmSyntax *ctx, IdmValue datum, IdmError *err) {
    IdmSpan span = ctx ? ctx->span : idm_span_unknown(NULL);
    IdmSyntax *out = NULL;
    switch (datum.tag) {
        case IDM_VAL_NIL: out = idm_syn_nil(span); break;
        case IDM_VAL_INT: out = idm_syn_int(datum.as.i, span); break;
        case IDM_VAL_WORD: out = idm_syn_word(idm_symbol_text(datum.as.symbol), span); break;
        case IDM_VAL_ATOM: out = idm_syn_atom(idm_symbol_text(datum.as.symbol), span); break;
        case IDM_VAL_STRING: out = idm_syn_string(idm_string_bytes(datum), span); break;
        case IDM_VAL_PAIR: {
            out = idm_syn_list(IDM_SEQ_PAREN, span);
            if (!out) break;
            IdmValue cur = datum;
            while (idm_is_pair(cur)) {
                IdmValue car = idm_car(cur, err);
                if (err && err->present) { idm_syn_free(out); return NULL; }
                IdmSyntax *child = syntax_from_datum_impl(ctx, car, err);
                if (!child || !idm_syn_append(out, child)) { idm_syn_free(child); idm_syn_free(out); return NULL; }
                cur = idm_cdr(cur, err);
                if (err && err->present) { idm_syn_free(out); return NULL; }
            }
            if (!idm_is_nil(cur)) { idm_syn_free(out); idm_error_set(err, span, "datum->syntax requires a proper list"); return NULL; }
            break;
        }
        case IDM_VAL_TUPLE: {
            out = idm_syn_tuple(span);
            if (!out) break;
            for (size_t i = 0; i < idm_sequence_count(datum); i++) {
                IdmValue item = idm_sequence_item(datum, i, err);
                if (err && err->present) { idm_syn_free(out); return NULL; }
                IdmSyntax *child = syntax_from_datum_impl(ctx, item, err);
                if (!child || !idm_syn_append(out, child)) { idm_syn_free(child); idm_syn_free(out); return NULL; }
            }
            break;
        }
        case IDM_VAL_VECTOR: {
            out = idm_syn_vector(span);
            if (!out) break;
            for (size_t i = 0; i < idm_sequence_count(datum); i++) {
                IdmValue item = idm_sequence_item(datum, i, err);
                if (err && err->present) { idm_syn_free(out); return NULL; }
                IdmSyntax *child = syntax_from_datum_impl(ctx, item, err);
                if (!child || !idm_syn_append(out, child)) { idm_syn_free(child); idm_syn_free(out); return NULL; }
            }
            break;
        }
        case IDM_VAL_SYNTAX: {
            IdmSyntax *inner = idm_syntax_value_get(datum);
            return idm_syn_clone(inner);
        }
        default:
            idm_error_set(err, span, "datum->syntax cannot wrap this datum kind");
            return NULL;
    }
    if (!out) {
        idm_error_oom(err, span);
        return NULL;
    }
    if (!copy_scopes_from(out, ctx)) { idm_syn_free(out); idm_error_oom(err, span); return NULL; }
    return out;
}

static bool prim_datum_to_syntax(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax *syn = syntax_from_datum_impl(ctx, args[1], err);
    if (!syn) return false;
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); return idm_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_syntax_property(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = require_string(args[1], NULL, err);
    if (!key) return false;
    const char *value = idm_syn_property_get(syn, key);
    if (!value) { *out = idm_nil(); return true; }
    *out = idm_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_syntax_set_property(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = require_string(args[1], NULL, err);
    if (!key) return false;
    const char *value = require_string(args[2], NULL, err);
    if (!value) return false;
    IdmSyntax *copy = idm_syn_clone(syn);
    if (!copy) return idm_error_oom(err, syn->span);
    if (!idm_syn_property_set(copy, key, value)) { idm_syn_free(copy); return idm_error_oom(err, syn->span); }
    return wrap_owned_syntax(rt, copy, out, err);
}

static bool prim_syntax_origin(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    IdmValue result = idm_nil();
    for (size_t i = syn->origins.count; i > 0; i--) {
        IdmValue item = idm_string(rt, syn->origins.items[i - 1u], err);
        if (err && err->present) return false;
        result = idm_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static IdmValue truth(IdmRuntime *rt, bool v) {
    return idm_atom(rt, v ? "true" : "false");
}

static bool prim_syntax_list_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_LIST);
    return true;
}

static bool prim_syntax_length(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    size_t count = 0;
    switch (syn->kind) {
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            count = syn->as.seq.count;
            break;
        default:
            return idm_error_set(err, syn->span, "syntax-length expects a sequence syntax");
    }
    *out = idm_int((int64_t)count);
    return true;
}

static bool prim_syntax_nth(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (args[1].tag != IDM_VAL_INT) return idm_error_set(err, syn->span, "syntax-nth index must be int");
    int64_t idx = args[1].as.i;
    if (idx < 0) return idm_error_set(err, syn->span, "syntax-nth index must be non-negative");
    switch (syn->kind) {
        case IDM_SYN_LIST:
        case IDM_SYN_VECTOR:
        case IDM_SYN_TUPLE:
        case IDM_SYN_DICT:
            if ((size_t)idx >= syn->as.seq.count) return idm_error_set(err, syn->span, "syntax-nth index out of bounds");
            *out = idm_syntax_value(rt, syn->as.seq.items[(size_t)idx], err);
            return !(err && err->present);
        default:
            return idm_error_set(err, syn->span, "syntax-nth expects a sequence syntax");
    }
}

static bool prim_syntax_slice(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (args[1].tag != IDM_VAL_INT || args[2].tag != IDM_VAL_INT) return idm_error_set(err, syn->span, "syntax-slice indexes must be ints");
    int64_t start_i = args[1].as.i;
    int64_t end_i = args[2].as.i;
    if (start_i < 0 || end_i < start_i) return idm_error_set(err, syn->span, "invalid syntax-slice range");
    if (syn->kind != IDM_SYN_LIST && syn->kind != IDM_SYN_VECTOR && syn->kind != IDM_SYN_TUPLE && syn->kind != IDM_SYN_DICT) return idm_error_set(err, syn->span, "syntax-slice expects sequence syntax");
    size_t start = (size_t)start_i;
    size_t end = (size_t)end_i;
    if (end > syn->as.seq.count) return idm_error_set(err, syn->span, "syntax-slice range out of bounds");
    IdmValue result = idm_nil();
    for (size_t i = end; i > start; i--) {
        IdmValue item = idm_syntax_value(rt, syn->as.seq.items[i - 1u], err);
        if (err && err->present) return false;
        result = idm_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_syntax_word_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_WORD);
    return true;
}

static bool prim_syntax_word_text(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_WORD) return idm_error_set(err, syn->span, "syntax-word-text expects word syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_atom_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_ATOM);
    return true;
}

static bool prim_syntax_atom_text(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_ATOM) return idm_error_set(err, syn->span, "syntax-atom-text expects atom syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_adjacent_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->token_adjacent_previous);
    return true;
}

static bool prim_syntax_string_text(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_STRING) return idm_error_set(err, syn->span, "syntax-string-text expects string syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_expander_register_operator(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!rt->register_operator) return idm_error_set(err, idm_span_unknown(NULL), "expander-register-operator requires an active expansion");
    const IdmSyntax *name = idm_syntax_get(args[0], err);
    if (!name) return false;
    if (args[1].tag != IDM_VAL_INT) return idm_error_set(err, idm_span_unknown(NULL), "operator precedence must be an integer");
    if (args[2].tag != IDM_VAL_ATOM && args[2].tag != IDM_VAL_STRING) return idm_error_set(err, idm_span_unknown(NULL), "operator assoc must be an atom");
    if (args[3].tag != IDM_VAL_ATOM && args[3].tag != IDM_VAL_STRING) return idm_error_set(err, idm_span_unknown(NULL), "operator fixity must be an atom");
    const IdmSyntax *target = idm_syntax_get(args[4], err);
    if (!target) return false;
    const char *assoc = args[2].tag == IDM_VAL_ATOM ? idm_symbol_text(args[2].as.symbol) : idm_string_bytes(args[2]);
    const char *fixity = args[3].tag == IDM_VAL_ATOM ? idm_symbol_text(args[3].as.symbol) : idm_string_bytes(args[3]);
    if (!rt->register_operator(rt->register_operator_user, rt, name, args[1].as.i, assoc, fixity, target, err)) return false;
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_expander_register_macro(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!rt->register_macro) return idm_error_set(err, idm_span_unknown(NULL), "expander-register-macro requires an active expansion");
    const IdmSyntax *name = idm_syntax_get(args[0], err);
    if (!name) return false;
    if (!idm_is_closure(args[1])) return idm_error_set(err, idm_span_unknown(NULL), "expander-register-macro transformer must be a function value");
    if (!rt->register_macro(rt->register_macro_user, rt, name, args[1], err)) return false;
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_expander_surface(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!rt->expander_surface) return idm_error_set(err, idm_span_unknown(NULL), "expander-surface requires an active expansion");
    if (args[0].tag != IDM_VAL_ATOM) return idm_error_set(err, idm_span_unknown(NULL), "expander-surface expects an atom kind");
    return rt->expander_surface(rt->expander_surface_user, rt, idm_symbol_text(args[0].as.symbol), out, err);
}

static bool prim_inspect(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    if (!idm_value_write(&buf, args[0])) {
        idm_buf_destroy(&buf);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    *out = idm_string(rt, buf.data ? buf.data : "", err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

static bool prim_expand_check(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *source = require_string(args[0], NULL, err);
    if (!source) return false;
    IdmError inner;
    idm_error_init(&inner);
    IdmCore *core = NULL;
    bool expanded = idm_expand_string(rt, "<expand-check>", source, &core, &inner);
    bool compiled = false;
    if (expanded) {
        IdmBytecodeModule module;
        idm_bc_init(&module);
        uint32_t main_fn = 0;
        compiled = idm_core_compile_main(core, &module, &main_fn, &inner);
        idm_bc_destroy(&module);
    }
    idm_core_free(core);
    if (expanded && compiled) {
        *out = idm_atom(rt, "ok");
        idm_error_clear(&inner);
        return true;
    }
    IdmValue items[2];
    items[0] = idm_atom(rt, "error");
    items[1] = idm_string(rt, inner.message ? inner.message : "compile failed", err);
    idm_error_clear(&inner);
    if (err && err->present) return false;
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}

static bool prim_str_contains(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t hay_len = 0;
    const char *hay = require_string(args[0], &hay_len, err);
    if (!hay) return false;
    size_t needle_len = 0;
    const char *needle = require_string(args[1], &needle_len, err);
    if (!needle) return false;
    bool found = needle_len == 0;
    for (size_t i = 0; !found && needle_len <= hay_len && i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) found = true;
    }
    *out = truth(rt, found);
    return true;
}

static bool prim_syntax_int_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_INT);
    return true;
}

static bool prim_syntax_int_value(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_INT) return idm_error_set(err, syn->span, "syntax-int-value expects int syntax");
    *out = idm_int(syn->as.integer);
    return true;
}

static const char *require_string(IdmValue v, size_t *out_len, IdmError *err) {
    if (!idm_is_string(v)) {
        idm_error_set(err, idm_span_unknown(NULL), "expected string");
        return NULL;
    }
    if (out_len) *out_len = idm_string_length(v);
    return idm_string_bytes(v);
}

static IdmSyntax *make_word_syntax_from(IdmSyntax *ctx, const char *text, IdmError *err) {
    IdmSyntax *out = idm_syn_word(text, ctx ? ctx->span : idm_span_unknown(NULL));
    if (!out) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    if (!copy_scopes_from(out, ctx)) { idm_syn_free(out); idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    return out;
}

static bool prim_make_syntax_word(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IdmSyntax *syn = make_word_syntax_from(ctx, text, err);
    if (!syn) return false;
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); return idm_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_atom(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IdmSyntax *syn = idm_syn_atom(text, ctx->span);
    if (!syn) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { idm_syn_free(syn); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); return idm_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_int(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    if (args[1].tag != IDM_VAL_INT) return idm_error_set(err, ctx->span, "make-syntax-int expects int");
    IdmSyntax *syn = idm_syn_int(args[1].as.i, ctx->span);
    if (!syn) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { idm_syn_free(syn); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); return idm_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_string(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    size_t len = 0;
    const char *text = require_string(args[1], &len, err);
    if (!text) return false;
    IdmSyntax *syn = idm_syn_string_n(text, len, ctx->span);
    if (!syn) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { idm_syn_free(syn); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); return idm_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool collect_list_of_syntax(IdmValue list, IdmSyntax ***out_items, size_t *out_count, IdmError *err) {
    size_t count = 0;
    IdmValue cur = list;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_nil(cur)) return idm_error_set(err, idm_span_unknown(NULL), "expected proper list of syntax");
    IdmSyntax **items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return idm_error_oom(err, idm_span_unknown(NULL));
    cur = list;
    for (size_t i = 0; i < count; i++) {
        IdmValue car = idm_car(cur, err);
        if (err && err->present) { free(items); return false; }
        IdmSyntax *inner = idm_syntax_value_get(car);
        if (!inner) {
            free(items);
            return idm_error_set(err, idm_span_unknown(NULL), "list item must be a syntax value");
        }
        items[i] = idm_syn_clone(inner);
        if (!items[i]) {
            for (size_t j = 0; j < i; j++) idm_syn_free(items[j]);
            free(items);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
        cur = idm_cdr(cur, err);
        if (err && err->present) {
            for (size_t j = 0; j <= i; j++) idm_syn_free(items[j]);
            free(items);
            return false;
        }
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static bool prim_make_syntax_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    if (args[1].tag != IDM_VAL_ATOM) return idm_error_set(err, ctx->span, "make-syntax-list shape must be atom");
    const char *shape_text = idm_symbol_text(args[1].as.symbol);
    IdmSequenceShape shape;
    if (strcmp(shape_text, "paren") == 0) shape = IDM_SEQ_PAREN;
    else if (strcmp(shape_text, "bracket") == 0) shape = IDM_SEQ_BRACKET;
    else return idm_error_set(err, ctx->span, "make-syntax-list shape must be :paren or :bracket");
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[2], &items, &count, err)) return false;
    IdmSyntax *syn = idm_syn_list(shape, ctx->span);
    if (!syn) {
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!copy_scopes_from(syn, ctx)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
            free(items);
            idm_syn_free(syn);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_vector(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IdmSyntax *syn = idm_syn_vector(ctx->span);
    if (!syn || !copy_scopes_from(syn, ctx)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
            free(items);
            idm_syn_free(syn);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_tuple(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IdmSyntax *syn = idm_syn_tuple(ctx->span);
    if (!syn || !copy_scopes_from(syn, ctx)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        idm_syn_free(syn);
        for (size_t i = 0; i < count; i++) idm_syn_free(items[i]);
        free(items);
        return idm_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
            free(items);
            idm_syn_free(syn);
            return idm_error_oom(err, idm_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static IdmSyntax *protocol_sequence(IdmRuntime *rt, IdmSyntax *ctx, const char *head, IdmSyntax **items, size_t count, IdmError *err) {
    (void)rt;
    IdmSyntax *syn = idm_syn_list(IDM_SEQ_PAREN, ctx->span);
    if (!syn) return NULL;
    if (!copy_scopes_from(syn, ctx)) { idm_syn_free(syn); return NULL; }
    IdmSyntax *head_syn = idm_syn_word(head, ctx->span);
    if (!head_syn || !copy_scopes_from(head_syn, ctx) || !idm_syn_append(syn, head_syn)) {
        idm_syn_free(head_syn);
        idm_syn_free(syn);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!idm_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) idm_syn_free(items[j]);
            idm_syn_free(syn);
            return NULL;
        }
    }
    if (!add_active_intro_scope(rt, syn)) { idm_syn_free(syn); idm_error_oom(err, ctx->span); return NULL; }
    return syn;
}

static bool prim_make_syntax_expr(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IdmSyntax *syn = protocol_sequence(rt, ctx, "%-expr", items, count, err);
    free(items);
    if (!syn) return idm_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_body(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IdmSyntax *syn = protocol_sequence(rt, ctx, "%-body", items, count, err);
    free(items);
    if (!syn) return idm_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_group(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IdmSyntax *expr = require_syntax(args[1], err);
    if (!expr) return false;
    IdmSyntax *item = idm_syn_clone(expr);
    if (!item) return idm_error_oom(err, ctx->span);
    IdmSyntax *items[1] = { item };
    IdmSyntax *syn = protocol_sequence(rt, ctx, "%-group", items, 1, err);
    if (!syn) return idm_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_syntax_error(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    (void)out;
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *msg = require_string(args[1], NULL, err);
    if (!msg) return false;
    return idm_error_set(err, syn->span, "%s", msg);
}

static bool prim_local_expand(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (!rt->local_expand) return idm_error_set(err, syn->span, "local-expand is only available during macro expansion");
    IdmSyntax *expanded = NULL;
    if (!rt->local_expand(rt->local_expand_user, rt, syn, &expanded, err)) return false;
    if (!expanded) return idm_error_set(err, syn->span, "local-expand returned no syntax");
    bool ok = wrap_owned_syntax(rt, expanded, out, err);
    return ok;
}

static bool identifier_text(IdmValue v, const char **out_text, IdmSyntax **out_syn, IdmError *err) {
    IdmSyntax *syn = require_syntax(v, err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_WORD) return idm_error_set(err, syn->span, "expected identifier syntax");
    *out_text = syn->as.text;
    *out_syn = syn;
    return true;
}

static bool prim_bound_identifier_eq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *a_text = NULL;
    const char *b_text = NULL;
    IdmSyntax *a_syn = NULL;
    IdmSyntax *b_syn = NULL;
    if (!identifier_text(args[0], &a_text, &a_syn, err) || !identifier_text(args[1], &b_text, &b_syn, err)) return false;
    bool same = strcmp(a_text, b_text) == 0;
    if (same) {
        const IdmScopeSet *as = idm_syn_scope_set(a_syn, 0);
        const IdmScopeSet *bs = idm_syn_scope_set(b_syn, 0);
        IdmScopeSet empty_a, empty_b;
        idm_scope_set_init(&empty_a);
        idm_scope_set_init(&empty_b);
        const IdmScopeSet *la = as ? as : &empty_a;
        const IdmScopeSet *lb = bs ? bs : &empty_b;
        same = idm_scope_set_equal(la, lb);
        idm_scope_set_destroy(&empty_a);
        idm_scope_set_destroy(&empty_b);
    }
    *out = truth(rt, same);
    return true;
}

static bool prim_free_identifier_eq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *a_text = NULL;
    const char *b_text = NULL;
    IdmSyntax *a_syn = NULL;
    IdmSyntax *b_syn = NULL;
    if (!identifier_text(args[0], &a_text, &a_syn, err) || !identifier_text(args[1], &b_text, &b_syn, err)) return false;
    bool same = false;
    if (rt->free_identifier_eq) {
        if (!rt->free_identifier_eq(rt->free_identifier_eq_user, rt, a_syn, b_syn, &same, err)) return false;
    } else {
        same = strcmp(a_text, b_text) == 0;
    }
    *out = truth(rt, same);
    return true;
}

static bool prim_bind_bang(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IdmSyntax *syn = make_word_syntax_from(ctx, text, err);
    if (!syn) return false;
    if (rt->macro_intro_active && !idm_syn_scope_add(syn, 0, rt->macro_intro_scope)) {
        idm_syn_free(syn);
        return idm_error_oom(err, ctx->span);
    }
    return wrap_owned_syntax(rt, syn, out, err);
}

bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!require_arity(prim, argc, err)) return false;
    switch (prim) {
        case IDM_PRIM_CONS: return prim_cons(rt, args, out, err);
        case IDM_PRIM_FIRST: return prim_first(rt, args, out, err);
        case IDM_PRIM_REST: return prim_rest(rt, args, out, err);
        case IDM_PRIM_LIST: return prim_list(rt, args, argc, out, err);
        case IDM_PRIM_TUPLE: return prim_tuple(rt, args, argc, out, err);
        case IDM_PRIM_VECTOR: return prim_vector(rt, args, argc, out, err);
        case IDM_PRIM_DICT: return prim_dict(rt, args, argc, out, err);
        case IDM_PRIM_TUPLE_GET: return prim_tuple_get(rt, args, out, err);
        case IDM_PRIM_APPEND: return prim_append(rt, args, out, err);
        case IDM_PRIM_TO_LIST: return prim_to_list(rt, args, out, err);
        case IDM_PRIM_SYNTAX_KIND: return prim_syntax_kind(rt, args, out, err);
        case IDM_PRIM_SYNTAX_TO_DATUM: return prim_syntax_to_datum(rt, args, out, err);
        case IDM_PRIM_DATUM_TO_SYNTAX: return prim_datum_to_syntax(rt, args, out, err);
        case IDM_PRIM_SYNTAX_PROPERTY: return prim_syntax_property(rt, args, out, err);
        case IDM_PRIM_SYNTAX_SET_PROPERTY: return prim_syntax_set_property(rt, args, out, err);
        case IDM_PRIM_SYNTAX_ORIGIN: return prim_syntax_origin(rt, args, out, err);
        case IDM_PRIM_SYNTAX_LIST_PRED: return prim_syntax_list_pred(rt, args, out, err);
        case IDM_PRIM_SYNTAX_LENGTH: return prim_syntax_length(rt, args, out, err);
        case IDM_PRIM_SYNTAX_NTH: return prim_syntax_nth(rt, args, out, err);
        case IDM_PRIM_SYNTAX_SLICE: return prim_syntax_slice(rt, args, out, err);
        case IDM_PRIM_SYNTAX_WORD_PRED: return prim_syntax_word_pred(rt, args, out, err);
        case IDM_PRIM_SYNTAX_WORD_TEXT: return prim_syntax_word_text(rt, args, out, err);
        case IDM_PRIM_SYNTAX_ATOM_PRED: return prim_syntax_atom_pred(rt, args, out, err);
        case IDM_PRIM_SYNTAX_ATOM_TEXT: return prim_syntax_atom_text(rt, args, out, err);
        case IDM_PRIM_SYNTAX_INT_PRED: return prim_syntax_int_pred(rt, args, out, err);
        case IDM_PRIM_SYNTAX_ADJACENT_PRED: return prim_syntax_adjacent_pred(rt, args, out, err);
        case IDM_PRIM_SYNTAX_STRING_TEXT: return prim_syntax_string_text(rt, args, out, err);
        case IDM_PRIM_STR_CONTAINS: return prim_str_contains(rt, args, out, err);
        case IDM_PRIM_EXPANDER_REGISTER_OPERATOR: return prim_expander_register_operator(rt, args, out, err);
        case IDM_PRIM_EXPANDER_REGISTER_MACRO: return prim_expander_register_macro(rt, args, out, err);
        case IDM_PRIM_EXPANDER_SURFACE: return prim_expander_surface(rt, args, out, err);
        case IDM_PRIM_EXPAND_CHECK: return prim_expand_check(rt, args, out, err);
        case IDM_PRIM_INSPECT: return prim_inspect(rt, args, out, err);
        case IDM_PRIM_SYNTAX_INT_VALUE: return prim_syntax_int_value(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_WORD: return prim_make_syntax_word(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_ATOM: return prim_make_syntax_atom(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_INT: return prim_make_syntax_int(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_STRING: return prim_make_syntax_string(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_LIST: return prim_make_syntax_list(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_VECTOR: return prim_make_syntax_vector(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_TUPLE: return prim_make_syntax_tuple(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_EXPR: return prim_make_syntax_expr(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_BODY: return prim_make_syntax_body(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_GROUP: return prim_make_syntax_group(rt, args, out, err);
        case IDM_PRIM_SYNTAX_ERROR: return prim_syntax_error(rt, args, out, err);
        case IDM_PRIM_LOCAL_EXPAND: return prim_local_expand(rt, args, out, err);
        case IDM_PRIM_FREE_IDENTIFIER_EQ: return prim_free_identifier_eq(rt, args, out, err);
        case IDM_PRIM_BOUND_IDENTIFIER_EQ: return prim_bound_identifier_eq(rt, args, out, err);
        case IDM_PRIM_BIND_BANG: return prim_bind_bang(rt, args, out, err);
        case IDM_PRIM_ADD: return prim_add(rt, args, out, err);
        case IDM_PRIM_SUB: return prim_sub(rt, args, out, err);
        case IDM_PRIM_MUL: return prim_mul(rt, args, out, err);
        case IDM_PRIM_DIV: return prim_div(rt, args, out, err);
        case IDM_PRIM_MOD: return prim_mod(rt, args, out, err);
        case IDM_PRIM_POW: return prim_pow(rt, args, out, err);
        case IDM_PRIM_NEG: return prim_neg(rt, args, out, err);
        case IDM_PRIM_EQ: return prim_eq(rt, args, out, err);
        case IDM_PRIM_NEQ: return prim_neq(rt, args, out, err);
        case IDM_PRIM_LT: return prim_lt(rt, args, out, err);
        case IDM_PRIM_GT: return prim_gt(rt, args, out, err);
        case IDM_PRIM_LTE: return prim_lte(rt, args, out, err);
        case IDM_PRIM_GTE: return prim_gte(rt, args, out, err);
        case IDM_PRIM_SELF:
        case IDM_PRIM_SPAWN:
        case IDM_PRIM_SEND:
        case IDM_PRIM_EXIT:
        case IDM_PRIM_LINK:
        case IDM_PRIM_UNLINK:
        case IDM_PRIM_MONITOR:
        case IDM_PRIM_DEMONITOR:
        case IDM_PRIM_TRAP_EXIT:
        case IDM_PRIM_EXEC:
        case IDM_PRIM_AWAIT:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", idm_primitive_name(prim));
        case IDM_PRIM_APPLY:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive 'apply' is compiled directly and cannot be invoked generically");
        case IDM_PRIM_STR: return prim_str(rt, args, argc, out, err);
        case IDM_PRIM_CHOMP: return prim_chomp(rt, args, out, err);
        case IDM_PRIM_CAPTURE_STDOUT: return prim_capture_stdout(rt, args, out, err);
        case IDM_PRIM_PRINT: return prim_print_impl(rt, args, argc, false, out, err);
        case IDM_PRIM_PRINTLN: return prim_print_impl(rt, args, argc, true, out, err);
        case IDM_PRIM_CD: return prim_cd(rt, args, out, err);
        case IDM_PRIM_PWD: return prim_pwd(rt, args, out, err);
        case IDM_PRIM_ENV_GET: return prim_env_get(rt, args, out, err);
        case IDM_PRIM_ENV_SET: return prim_env_set(rt, args, out, err);
        case IDM_PRIM_WRITE_PROCSUB_TEMP: return prim_write_procsub_temp(rt, args, out, err);
        case IDM_PRIM_MAKE_PROCSUB_TEMP: return prim_make_procsub_temp(rt, args, out, err);
        case IDM_PRIM_MAKE_RECORD: return prim_make_record(rt, args, out, err);
        case IDM_PRIM_RECORD_PRED: return prim_record_pred(rt, args, out, err);
        case IDM_PRIM_RECORD_TYPE: return prim_record_type(rt, args, out, err);
        case IDM_PRIM_RECORD_FIELD: return prim_record_field(rt, args, out, err);
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unimplemented primitive '%s'", idm_primitive_name(prim));
}

bool idm_prim_lookup_by_name(const char *name, IdmPrimitive *out) {
    size_t count = idm_primitive_count();
    for (size_t i = 0; i < count; i++) {
        const IdmPrimitiveInfo *info = idm_primitive_info((IdmPrimitive)i);
        if (info && strcmp(info->name, name) == 0) {
            *out = (IdmPrimitive)i;
            return true;
        }
    }
    return false;
}
