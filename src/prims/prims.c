#define _XOPEN_SOURCE 700

#include "ish/prims.h"
#include "ish/syntax.h"
#include "ish/vm.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool require_arity(IshPrimitive prim, uint32_t argc, IshError *err) {
    const IshPrimitiveInfo *info = ish_primitive_info(prim);
    if (!info) return ish_error_set(err, ish_span_unknown(NULL), "unknown primitive %u", (unsigned)prim);
    if (argc < info->min_arity || argc > info->max_arity) {
        return ish_error_set(err, ish_span_unknown(NULL), "primitive '%s' arity mismatch: got %u, want %u..%u", info->name, argc, info->min_arity, info->max_arity);
    }
    return true;
}

static IshSyntax *require_syntax(IshValue value, IshError *err) {
    IshSyntax *syn = ish_syntax_value_get(value);
    if (!syn) {
        ish_error_set(err, ish_span_unknown(NULL), "expected a syntax value");
        return NULL;
    }
    return syn;
}

static const char *require_string(IshValue v, size_t *out_len, IshError *err);

static bool prim_cons(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    *out = ish_cons(rt, args[0], args[1], err);
    return !(err && err->present);
}

static bool prim_first(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (!ish_is_pair(args[0])) return ish_error_set(err, ish_span_unknown(NULL), "first expects a pair");
    *out = ish_car(args[0], err);
    return !(err && err->present);
}

static bool prim_rest(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (!ish_is_pair(args[0])) return ish_error_set(err, ish_span_unknown(NULL), "rest expects a pair");
    *out = ish_cdr(args[0], err);
    return !(err && err->present);
}

static bool prim_list(IshRuntime *rt, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    IshValue result = ish_nil();
    for (size_t i = argc; i > 0; i--) {
        result = ish_cons(rt, args[i - 1u], result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_append(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshValue head = args[0];
    IshValue tail = args[1];
    if (ish_is_nil(head)) { *out = tail; return true; }
    if (!ish_is_pair(head)) return ish_error_set(err, ish_span_unknown(NULL), "append expects a list as first argument");
    size_t count = 0;
    IshValue cur = head;
    while (ish_is_pair(cur)) {
        count++;
        cur = ish_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!ish_is_nil(cur)) return ish_error_set(err, ish_span_unknown(NULL), "append expects a proper list as first argument");
    IshValue *items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return ish_error_oom(err, ish_span_unknown(NULL));
    cur = head;
    for (size_t i = 0; i < count; i++) {
        items[i] = ish_car(cur, err);
        if (err && err->present) { free(items); return false; }
        cur = ish_cdr(cur, err);
        if (err && err->present) { free(items); return false; }
    }
    IshValue result = tail;
    for (size_t i = count; i > 0; i--) {
        result = ish_cons(rt, items[i - 1u], result, err);
        if (err && err->present) { free(items); return false; }
    }
    free(items);
    *out = result;
    return true;
}

static bool prim_tuple(IshRuntime *rt, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    *out = ish_tuple(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_vector(IshRuntime *rt, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    *out = ish_vector(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_dict(IshRuntime *rt, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    if (argc % 2u != 0) return ish_error_set(err, ish_span_unknown(NULL), "dict expects an even number of key/value arguments");
    size_t count = argc / 2u;
    IshDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
    if (count != 0 && !entries) return ish_error_oom(err, ish_span_unknown(NULL));
    for (size_t i = 0; i < count; i++) {
        entries[i].key = args[i * 2u];
        entries[i].value = args[i * 2u + 1u];
    }
    *out = ish_dict(rt, entries, count, err);
    free(entries);
    return !(err && err->present);
}

static bool append_display(IshBuffer *buf, IshValue v) {
    if (v.tag == ISH_VAL_STRING) return ish_buf_append_n(buf, ish_string_bytes(v), ish_string_length(v));
    if (v.tag == ISH_VAL_ATOM || v.tag == ISH_VAL_WORD) return ish_buf_append(buf, ish_symbol_text(v.as.symbol));
    if (v.tag == ISH_VAL_NIL) return true;
    return ish_value_write(buf, v);
}

static bool prim_str(IshRuntime *rt, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    if (argc == 1 && args[0].tag == ISH_VAL_STRING) { *out = args[0]; return true; }
    IshBuffer buf;
    ish_buf_init(&buf);
    for (uint32_t i = 0; i < argc; i++) {
        if (!append_display(&buf, args[i])) { ish_buf_destroy(&buf); return ish_error_oom(err, ish_span_unknown(NULL)); }
    }
    *out = ish_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    ish_buf_destroy(&buf);
    return !(err && err->present);
}

static bool prim_chomp(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    if (args[0].tag != ISH_VAL_STRING) { *out = args[0]; return true; }
    const char *bytes = ish_string_bytes(args[0]);
    size_t len = ish_string_length(args[0]);
    while (len > 0 && (bytes[len - 1u] == '\n' || bytes[len - 1u] == '\r')) len--;
    *out = ish_string_n(rt, bytes, len, err);
    return !(err && err->present);
}

static bool prim_capture_stdout(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshValue result = args[0];
    if (!ish_is_tuple(result) || ish_sequence_count(result) < 3) return ish_error_set(err, ish_span_unknown(NULL), "capture-stdout expects a command result tuple");
    IshError ignore;
    ish_error_init(&ignore);
    IshValue tag = ish_sequence_item(result, 0, &ignore);
    IshValue reason = ish_sequence_item(result, 1, &ignore);
    IshValue stdout_v = ish_sequence_item(result, 2, &ignore);
    ish_error_clear(&ignore);
    if (tag.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(tag.as.symbol), "error") == 0 &&
        reason.tag == ISH_VAL_ATOM && strcmp(ish_symbol_text(reason.as.symbol), "capture-overflow") == 0) {
        return ish_error_set(err, ish_span_unknown(NULL), "command output exceeded the capture limit");
    }
    if (stdout_v.tag != ISH_VAL_STRING) { *out = stdout_v; return true; }
    const char *bytes = ish_string_bytes(stdout_v);
    size_t len = ish_string_length(stdout_v);
    while (len > 0 && (bytes[len - 1u] == '\n' || bytes[len - 1u] == '\r')) len--;
    *out = ish_string_n(rt, bytes, len, err);
    return !(err && err->present);
}

static bool prim_tuple_get(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (!ish_is_tuple(args[0])) return ish_error_set(err, ish_span_unknown(NULL), "tuple-get expects a tuple");
    if (args[1].tag != ISH_VAL_INT || args[1].as.i < 0) return ish_error_set(err, ish_span_unknown(NULL), "tuple-get index must be a non-negative integer");
    size_t index = (size_t)args[1].as.i;
    if (index >= ish_sequence_count(args[0])) return ish_error_set(err, ish_span_unknown(NULL), "tuple-get index out of range");
    *out = ish_sequence_item(args[0], index, err);
    return !(err && err->present);
}

static bool prim_to_list(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshValue v = args[0];
    if (ish_is_nil(v) || ish_is_pair(v)) { *out = v; return true; }
    if (ish_is_vector(v) || ish_is_tuple(v)) {
        size_t count = ish_sequence_count(v);
        IshValue result = ish_nil();
        for (size_t i = count; i > 0; i--) {
            IshValue item = ish_sequence_item(v, i - 1u, err);
            if (err && err->present) return false;
            result = ish_cons(rt, item, result, err);
            if (err && err->present) return false;
        }
        *out = result;
        return true;
    }
    if (ish_is_dict(v)) {
        size_t count = ish_dict_count(v);
        IshValue result = ish_nil();
        for (size_t i = count; i > 0; i--) {
            IshValue key;
            IshValue val;
            if (!ish_dict_entry(v, i - 1u, &key, &val)) return ish_error_set(err, ish_span_unknown(NULL), "to-list dict iteration failed");
            IshValue pair_items[2] = {key, val};
            IshValue pair = ish_tuple(rt, pair_items, 2u, err);
            if (err && err->present) return false;
            result = ish_cons(rt, pair, result, err);
            if (err && err->present) return false;
        }
        *out = result;
        return true;
    }
    return ish_error_set(err, ish_span_unknown(NULL), "to-list expects a list, vector, tuple, or dict");
}

static bool prim_cd(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
    if (!rt->current_exec) return ish_error_set(err, ish_span_unknown(NULL), "cd requires an actor context");
    char *path = ish_strndup(bytes, len);
    if (!path) return ish_error_oom(err, ish_span_unknown(NULL));
    char *candidate = NULL;
    if (path[0] == '/') {
        candidate = path;
        path = NULL;
    } else {
        const char *base = ish_exec_cwd(rt->current_exec);
        char buf[PATH_MAX];
        if (!base) {
            if (!getcwd(buf, sizeof(buf))) {
                free(path);
                return ish_error_set(err, ish_span_unknown(NULL), "cd: %s", strerror(errno));
            }
            base = buf;
        }
        IshBuffer joined;
        ish_buf_init(&joined);
        if (!ish_buf_append(&joined, base) || !ish_buf_append_char(&joined, '/') || !ish_buf_append(&joined, path)) {
            ish_buf_destroy(&joined);
            free(path);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
        free(path);
        candidate = ish_buf_take(&joined);
        if (!candidate) return ish_error_oom(err, ish_span_unknown(NULL));
    }
    char resolved[PATH_MAX];
    if (!realpath(candidate, resolved)) {
        ish_error_set(err, ish_span_unknown(NULL), "cd: %s: %s", candidate, strerror(errno));
        free(candidate);
        return false;
    }
    free(candidate);
    if (!ish_exec_set_cwd(rt->current_exec, resolved)) return ish_error_oom(err, ish_span_unknown(NULL));
    *out = ish_atom(rt, "ok");
    return true;
}

static bool prim_pwd(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)args;
    const char *cwd = ish_exec_cwd(rt->current_exec);
    if (cwd) {
        *out = ish_string(rt, cwd, err);
        return !(err && err->present);
    }
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return ish_error_set(err, ish_span_unknown(NULL), "pwd: %s", strerror(errno));
    *out = ish_string(rt, buf, err);
    return !(err && err->present);
}

static bool prim_env_get(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
    char *name = ish_strndup(bytes, len);
    if (!name) return ish_error_oom(err, ish_span_unknown(NULL));
    const char *value = ish_exec_env_get(rt->current_exec, name);
    if (!value) value = getenv(name);
    free(name);
    if (!value) {
        *out = ish_nil();
        return true;
    }
    *out = ish_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_env_set(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    size_t name_len = 0;
    const char *name_bytes = require_string(args[0], &name_len, err);
    if (!name_bytes) return false;
    size_t value_len = 0;
    const char *value_bytes = require_string(args[1], &value_len, err);
    if (!value_bytes) return false;
    if (!rt->current_exec) return ish_error_set(err, ish_span_unknown(NULL), "env-set requires an actor context");
    char *name = ish_strndup(name_bytes, name_len);
    char *value = ish_strndup(value_bytes, value_len);
    bool ok = name && value && ish_exec_env_set(rt->current_exec, name, value);
    free(name);
    free(value);
    if (!ok) return ish_error_oom(err, ish_span_unknown(NULL));
    *out = ish_atom(rt, "ok");
    return true;
}

static bool prim_make_procsub_temp(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)args;
    char tmpl[] = "/tmp/ish_procsub_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return ish_error_set(err, ish_span_unknown(NULL), "process substitution: mkstemp failed: %s", strerror(errno));
    close(fd);
    if (!ish_runtime_own_temp(rt, tmpl)) {
        unlink(tmpl);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    *out = ish_string(rt, tmpl, err);
    return !(err && err->present);
}

static const char *name_value_text(IshValue value, IshError *err, const char *what) {
    if (value.tag == ISH_VAL_ATOM || value.tag == ISH_VAL_WORD) return ish_symbol_text(value.as.symbol);
    if (value.tag == ISH_VAL_STRING) return ish_string_bytes(value);
    ish_error_set(err, ish_span_unknown(NULL), "%s must be an atom, word, or string", what);
    return NULL;
}

static bool prim_make_record(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    const char *type = name_value_text(args[0], err, "record type");
    if (!type) return false;
    if (!ish_is_dict(args[1])) return ish_error_set(err, ish_span_unknown(NULL), "make-record fields must be a dict");
    *out = ish_record(rt, type, args[1], err);
    return !(err && err->present);
}

static bool prim_record_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)err;
    *out = ish_atom(rt, ish_is_record(args[0]) ? "true" : "false");
    return true;
}

static bool prim_record_type(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    const char *type = ish_record_type(args[0], err);
    if (!type) return false;
    *out = ish_atom(rt, type);
    return true;
}

static bool prim_record_field(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    return ish_record_field(args[0], args[1], out, err);
}

static bool prim_write_procsub_temp(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    if (!ish_is_tuple(args[0]) || ish_sequence_count(args[0]) < 3) return ish_error_set(err, ish_span_unknown(NULL), "process substitution: expected a command result tuple");
    IshError ig;
    ish_error_init(&ig);
    IshValue stdout_val = ish_sequence_item(args[0], 2, &ig);
    ish_error_clear(&ig);
    size_t len = stdout_val.tag == ISH_VAL_STRING ? ish_string_length(stdout_val) : 0;
    const char *bytes = stdout_val.tag == ISH_VAL_STRING ? ish_string_bytes(stdout_val) : "";
    char tmpl[] = "/tmp/ish_procsub_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return ish_error_set(err, ish_span_unknown(NULL), "process substitution: mkstemp failed: %s", strerror(errno));
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w < 0) { close(fd); unlink(tmpl); return ish_error_set(err, ish_span_unknown(NULL), "process substitution: write failed: %s", strerror(errno)); }
        off += (size_t)w;
    }
    close(fd);
    if (!ish_runtime_own_temp(rt, tmpl)) {
        unlink(tmpl);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    *out = ish_string(rt, tmpl, err);
    return !(err && err->present);
}

static bool prim_print_impl(IshRuntime *rt, IshValue *args, uint32_t argc, bool newline, IshValue *out, IshError *err) {
    (void)rt;
    IshBuffer buf;
    ish_buf_init(&buf);
    for (uint32_t i = 0; i < argc; i++) {
        if (i != 0 && !ish_buf_append_char(&buf, ' ')) { ish_buf_destroy(&buf); return ish_error_oom(err, ish_span_unknown(NULL)); }
        if (!append_display(&buf, args[i])) { ish_buf_destroy(&buf); return ish_error_oom(err, ish_span_unknown(NULL)); }
    }
    if (newline && !ish_buf_append_char(&buf, '\n')) { ish_buf_destroy(&buf); return ish_error_oom(err, ish_span_unknown(NULL)); }
    if (buf.len != 0) fwrite(buf.data, 1u, buf.len, stdout);
    fflush(stdout);
    ish_buf_destroy(&buf);
    *out = ish_nil();
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

static bool require_int_pair(const char *name, IshValue *args, int64_t *a, int64_t *b, IshError *err) {
    if (args[0].tag != ISH_VAL_INT || args[1].tag != ISH_VAL_INT) return ish_error_set(err, ish_span_unknown(NULL), "%s expects integer operands", name);
    *a = args[0].as.i;
    *b = args[1].as.i;
    return true;
}

static bool prim_add(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("add", args, &a, &b, err)) return false;
    if (!checked_add(a, b, &r)) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in add");
    *out = ish_int(r);
    return true;
}

static bool prim_sub(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("sub", args, &a, &b, err)) return false;
    if (!checked_sub(a, b, &r)) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in sub");
    *out = ish_int(r);
    return true;
}

static bool prim_mul(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("mul", args, &a, &b, err)) return false;
    if (!checked_mul(a, b, &r)) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in mul");
    *out = ish_int(r);
    return true;
}

static bool num_as_double(IshValue v, double *out) {
    if (v.tag == ISH_VAL_INT) { *out = (double)v.as.i; return true; }
    if (v.tag == ISH_VAL_FLOAT) { *out = v.as.f; return true; }
    return false;
}

static bool prim_div(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (args[0].tag == ISH_VAL_FLOAT || args[1].tag == ISH_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return ish_error_set(err, ish_span_unknown(NULL), "div expects numeric operands");
        if (y == 0.0) return ish_error_set(err, ish_span_unknown(NULL), "division by zero in div");
        *out = ish_float(x / y);
        return true;
    }
    int64_t a = 0, b = 0;
    if (!require_int_pair("div", args, &a, &b, err)) return false;
    if (b == 0) return ish_error_set(err, ish_span_unknown(NULL), "division by zero in div");
    if (a == INT64_MIN && b == -1) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in div");
    *out = ish_int(a / b);
    return true;
}

static bool prim_mod(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (args[0].tag == ISH_VAL_FLOAT || args[1].tag == ISH_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return ish_error_set(err, ish_span_unknown(NULL), "mod expects numeric operands");
        if (y == 0.0) return ish_error_set(err, ish_span_unknown(NULL), "division by zero in mod");
        *out = ish_float(fmod(x, y));
        return true;
    }
    int64_t a = 0, b = 0;
    if (!require_int_pair("mod", args, &a, &b, err)) return false;
    if (b == 0) return ish_error_set(err, ish_span_unknown(NULL), "division by zero in mod");
    if (a == INT64_MIN && b == -1) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in mod");
    *out = ish_int(a % b);
    return true;
}

static bool prim_pow(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (args[0].tag == ISH_VAL_FLOAT || args[1].tag == ISH_VAL_FLOAT) {
        double x, y;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return ish_error_set(err, ish_span_unknown(NULL), "pow expects numeric operands");
        *out = ish_float(pow(x, y));
        return true;
    }
    int64_t a = 0, b = 0, r = 0;
    if (!require_int_pair("pow", args, &a, &b, err)) return false;
    if (b < 0) return ish_error_set(err, ish_span_unknown(NULL), "pow exponent must be non-negative");
    if (!checked_pow(a, b, &r)) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in pow");
    *out = ish_int(r);
    return true;
}

static bool prim_neg(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    if (args[0].tag == ISH_VAL_FLOAT) { *out = ish_float(-args[0].as.f); return true; }
    if (args[0].tag != ISH_VAL_INT) return ish_error_set(err, ish_span_unknown(NULL), "neg expects a number");
    if (args[0].as.i == INT64_MIN) return ish_error_set(err, ish_span_unknown(NULL), "integer overflow in neg");
    *out = ish_int(-args[0].as.i);
    return true;
}

static bool prim_eq(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)err;
    *out = ish_atom(rt, ish_value_equal(args[0], args[1]) ? "true" : "false");
    return true;
}

static bool prim_neq(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)err;
    *out = ish_atom(rt, ish_value_equal(args[0], args[1]) ? "false" : "true");
    return true;
}

static bool prim_lt(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("lt?", args, &a, &b, err)) return false;
    *out = ish_atom(rt, a < b ? "true" : "false");
    return true;
}

static bool prim_gt(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("gt?", args, &a, &b, err)) return false;
    *out = ish_atom(rt, a > b ? "true" : "false");
    return true;
}

static bool prim_lte(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("lte?", args, &a, &b, err)) return false;
    *out = ish_atom(rt, a <= b ? "true" : "false");
    return true;
}

static bool prim_gte(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    int64_t a = 0, b = 0;
    if (!require_int_pair("gte?", args, &a, &b, err)) return false;
    *out = ish_atom(rt, a >= b ? "true" : "false");
    return true;
}

static const char *kind_atom_text(IshSyntaxKind kind) {
    switch (kind) {
        case ISH_SYN_NIL: return "nil";
        case ISH_SYN_WORD: return "word";
        case ISH_SYN_ATOM: return "atom";
        case ISH_SYN_INT: return "int";
        case ISH_SYN_FLOAT: return "float";
        case ISH_SYN_STRING: return "string";
        case ISH_SYN_LIST: return "list";
        case ISH_SYN_VECTOR: return "vector";
        case ISH_SYN_TUPLE: return "tuple";
        case ISH_SYN_DICT: return "dict";
    }
    return "unknown";
}

static bool prim_syntax_kind(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = ish_atom(rt, kind_atom_text(syn->kind));
    return true;
}

static bool prim_syntax_to_datum_impl(IshRuntime *rt, IshSyntax *syn, IshValue *out, IshError *err) {
    switch (syn->kind) {
        case ISH_SYN_NIL: *out = ish_nil(); return true;
        case ISH_SYN_WORD: *out = ish_word(rt, syn->as.text); return true;
        case ISH_SYN_ATOM: *out = ish_atom(rt, syn->as.text); return true;
        case ISH_SYN_INT: *out = ish_int(syn->as.integer); return true;
        case ISH_SYN_FLOAT: *out = ish_float(syn->as.real); return true;
        case ISH_SYN_STRING: *out = ish_string(rt, syn->as.text, err); return !(err && err->present);
        case ISH_SYN_LIST: {
            IshValue list = ish_nil();
            for (size_t i = syn->as.seq.count; i > 0; i--) {
                IshValue item = ish_nil();
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i - 1u], &item, err)) return false;
                list = ish_cons(rt, item, list, err);
                if (err && err->present) return false;
            }
            *out = list;
            return true;
        }
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE: {
            IshValue *items = syn->as.seq.count == 0 ? NULL : calloc(syn->as.seq.count, sizeof(*items));
            if (syn->as.seq.count != 0 && !items) return ish_error_oom(err, syn->span);
            for (size_t i = 0; i < syn->as.seq.count; i++) {
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i], &items[i], err)) { free(items); return false; }
            }
            *out = syn->kind == ISH_SYN_VECTOR ? ish_vector(rt, items, syn->as.seq.count, err) : ish_tuple(rt, items, syn->as.seq.count, err);
            free(items);
            return !(err && err->present);
        }
        case ISH_SYN_DICT: {
            if (syn->as.seq.count % 2u != 0) return ish_error_set(err, syn->span, "dict syntax requires key/value pairs");
            size_t count = syn->as.seq.count / 2u;
            IshDictEntry *entries = count == 0 ? NULL : calloc(count, sizeof(*entries));
            if (count != 0 && !entries) return ish_error_oom(err, syn->span);
            for (size_t i = 0; i < count; i++) {
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i * 2u], &entries[i].key, err)) { free(entries); return false; }
                if (!prim_syntax_to_datum_impl(rt, syn->as.seq.items[i * 2u + 1u], &entries[i].value, err)) { free(entries); return false; }
            }
            *out = ish_dict(rt, entries, count, err);
            free(entries);
            return !(err && err->present);
        }
    }
    return ish_error_set(err, syn->span, "unknown syntax kind");
}

static bool prim_syntax_to_datum(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    return prim_syntax_to_datum_impl(rt, syn, out, err);
}

static bool wrap_owned_syntax(IshRuntime *rt, IshSyntax *syn, IshValue *out, IshError *err) {
    if (!syn) return false;
    *out = ish_syntax_value(rt, syn, err);
    ish_syn_free(syn);
    return !(err && err->present);
}

static IshSyntax *syntax_from_datum_impl(IshSyntax *ctx, IshValue datum, IshError *err);

static bool copy_scopes_from(IshSyntax *dst, const IshSyntax *src) {
    if (!src) return true;
    for (size_t i = 0; i < src->scopes.count; i++) {
        const IshSyntaxPhaseScope *p = &src->scopes.items[i];
        for (size_t j = 0; j < p->scopes.count; j++) {
            if (!ish_syn_scope_add(dst, p->phase, p->scopes.items[j])) return false;
        }
    }
    return true;
}

static bool add_active_intro_scope(IshRuntime *rt, IshSyntax *syn) {
    if (!rt->macro_intro_active) return true;
    const IshScopeSet *set = ish_syn_scope_set(syn, 0);
    if (!set || !ish_scope_set_contains(set, rt->macro_intro_scope)) return true;
    return ish_syn_scope_flip(syn, 0, rt->macro_intro_scope);
}

static IshSyntax *syntax_from_datum_impl(IshSyntax *ctx, IshValue datum, IshError *err) {
    IshSpan span = ctx ? ctx->span : ish_span_unknown(NULL);
    IshSyntax *out = NULL;
    switch (datum.tag) {
        case ISH_VAL_NIL: out = ish_syn_nil(span); break;
        case ISH_VAL_INT: out = ish_syn_int(datum.as.i, span); break;
        case ISH_VAL_WORD: out = ish_syn_word(ish_symbol_text(datum.as.symbol), span); break;
        case ISH_VAL_ATOM: out = ish_syn_atom(ish_symbol_text(datum.as.symbol), span); break;
        case ISH_VAL_STRING: out = ish_syn_string(ish_string_bytes(datum), span); break;
        case ISH_VAL_PAIR: {
            out = ish_syn_list(ISH_SEQ_PAREN, span);
            if (!out) break;
            IshValue cur = datum;
            while (ish_is_pair(cur)) {
                IshValue car = ish_car(cur, err);
                if (err && err->present) { ish_syn_free(out); return NULL; }
                IshSyntax *child = syntax_from_datum_impl(ctx, car, err);
                if (!child || !ish_syn_append(out, child)) { ish_syn_free(child); ish_syn_free(out); return NULL; }
                cur = ish_cdr(cur, err);
                if (err && err->present) { ish_syn_free(out); return NULL; }
            }
            if (!ish_is_nil(cur)) { ish_syn_free(out); ish_error_set(err, span, "datum->syntax requires a proper list"); return NULL; }
            break;
        }
        case ISH_VAL_TUPLE: {
            out = ish_syn_tuple(span);
            if (!out) break;
            for (size_t i = 0; i < ish_sequence_count(datum); i++) {
                IshValue item = ish_sequence_item(datum, i, err);
                if (err && err->present) { ish_syn_free(out); return NULL; }
                IshSyntax *child = syntax_from_datum_impl(ctx, item, err);
                if (!child || !ish_syn_append(out, child)) { ish_syn_free(child); ish_syn_free(out); return NULL; }
            }
            break;
        }
        case ISH_VAL_VECTOR: {
            out = ish_syn_vector(span);
            if (!out) break;
            for (size_t i = 0; i < ish_sequence_count(datum); i++) {
                IshValue item = ish_sequence_item(datum, i, err);
                if (err && err->present) { ish_syn_free(out); return NULL; }
                IshSyntax *child = syntax_from_datum_impl(ctx, item, err);
                if (!child || !ish_syn_append(out, child)) { ish_syn_free(child); ish_syn_free(out); return NULL; }
            }
            break;
        }
        case ISH_VAL_SYNTAX: {
            IshSyntax *inner = ish_syntax_value_get(datum);
            return ish_syn_clone(inner);
        }
        default:
            ish_error_set(err, span, "datum->syntax cannot wrap this datum kind");
            return NULL;
    }
    if (!out) {
        ish_error_oom(err, span);
        return NULL;
    }
    if (!copy_scopes_from(out, ctx)) { ish_syn_free(out); ish_error_oom(err, span); return NULL; }
    return out;
}

static bool prim_datum_to_syntax(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax *syn = syntax_from_datum_impl(ctx, args[1], err);
    if (!syn) return false;
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); return ish_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_syntax_property(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = require_string(args[1], NULL, err);
    if (!key) return false;
    const char *value = ish_syn_property_get(syn, key);
    if (!value) { *out = ish_nil(); return true; }
    *out = ish_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_syntax_set_property(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = require_string(args[1], NULL, err);
    if (!key) return false;
    const char *value = require_string(args[2], NULL, err);
    if (!value) return false;
    IshSyntax *copy = ish_syn_clone(syn);
    if (!copy) return ish_error_oom(err, syn->span);
    if (!ish_syn_property_set(copy, key, value)) { ish_syn_free(copy); return ish_error_oom(err, syn->span); }
    return wrap_owned_syntax(rt, copy, out, err);
}

static bool prim_syntax_origin(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    IshValue result = ish_nil();
    for (size_t i = syn->origins.count; i > 0; i--) {
        IshValue item = ish_string(rt, syn->origins.items[i - 1u], err);
        if (err && err->present) return false;
        result = ish_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static IshValue truth(IshRuntime *rt, bool v) {
    return ish_atom(rt, v ? "true" : "false");
}

static bool prim_syntax_list_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == ISH_SYN_LIST);
    return true;
}

static bool prim_syntax_length(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    size_t count = 0;
    switch (syn->kind) {
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            count = syn->as.seq.count;
            break;
        default:
            return ish_error_set(err, syn->span, "syntax-length expects a sequence syntax");
    }
    *out = ish_int((int64_t)count);
    return true;
}

static bool prim_syntax_nth(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (args[1].tag != ISH_VAL_INT) return ish_error_set(err, syn->span, "syntax-nth index must be int");
    int64_t idx = args[1].as.i;
    if (idx < 0) return ish_error_set(err, syn->span, "syntax-nth index must be non-negative");
    switch (syn->kind) {
        case ISH_SYN_LIST:
        case ISH_SYN_VECTOR:
        case ISH_SYN_TUPLE:
        case ISH_SYN_DICT:
            if ((size_t)idx >= syn->as.seq.count) return ish_error_set(err, syn->span, "syntax-nth index out of bounds");
            *out = ish_syntax_value(rt, syn->as.seq.items[(size_t)idx], err);
            return !(err && err->present);
        default:
            return ish_error_set(err, syn->span, "syntax-nth expects a sequence syntax");
    }
}

static bool prim_syntax_slice(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (args[1].tag != ISH_VAL_INT || args[2].tag != ISH_VAL_INT) return ish_error_set(err, syn->span, "syntax-slice indexes must be ints");
    int64_t start_i = args[1].as.i;
    int64_t end_i = args[2].as.i;
    if (start_i < 0 || end_i < start_i) return ish_error_set(err, syn->span, "invalid syntax-slice range");
    if (syn->kind != ISH_SYN_LIST && syn->kind != ISH_SYN_VECTOR && syn->kind != ISH_SYN_TUPLE && syn->kind != ISH_SYN_DICT) return ish_error_set(err, syn->span, "syntax-slice expects sequence syntax");
    size_t start = (size_t)start_i;
    size_t end = (size_t)end_i;
    if (end > syn->as.seq.count) return ish_error_set(err, syn->span, "syntax-slice range out of bounds");
    IshValue result = ish_nil();
    for (size_t i = end; i > start; i--) {
        IshValue item = ish_syntax_value(rt, syn->as.seq.items[i - 1u], err);
        if (err && err->present) return false;
        result = ish_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_syntax_word_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == ISH_SYN_WORD);
    return true;
}

static bool prim_syntax_word_text(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != ISH_SYN_WORD) return ish_error_set(err, syn->span, "syntax-word-text expects word syntax");
    *out = ish_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_atom_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == ISH_SYN_ATOM);
    return true;
}

static bool prim_syntax_atom_text(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != ISH_SYN_ATOM) return ish_error_set(err, syn->span, "syntax-atom-text expects atom syntax");
    *out = ish_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_adjacent_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->token_adjacent_previous);
    return true;
}

static bool prim_syntax_string_text(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != ISH_SYN_STRING) return ish_error_set(err, syn->span, "syntax-string-text expects string syntax");
    *out = ish_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_expander_register_operator(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    if (!rt->register_operator) return ish_error_set(err, ish_span_unknown(NULL), "expander-register-operator requires an active expansion");
    const IshSyntax *name = ish_syntax_get(args[0], err);
    if (!name) return false;
    if (args[1].tag != ISH_VAL_INT) return ish_error_set(err, ish_span_unknown(NULL), "operator precedence must be an integer");
    if (args[2].tag != ISH_VAL_ATOM && args[2].tag != ISH_VAL_STRING) return ish_error_set(err, ish_span_unknown(NULL), "operator assoc must be an atom");
    if (args[3].tag != ISH_VAL_ATOM && args[3].tag != ISH_VAL_STRING) return ish_error_set(err, ish_span_unknown(NULL), "operator fixity must be an atom");
    const IshSyntax *target = ish_syntax_get(args[4], err);
    if (!target) return false;
    const char *assoc = args[2].tag == ISH_VAL_ATOM ? ish_symbol_text(args[2].as.symbol) : ish_string_bytes(args[2]);
    const char *fixity = args[3].tag == ISH_VAL_ATOM ? ish_symbol_text(args[3].as.symbol) : ish_string_bytes(args[3]);
    if (!rt->register_operator(rt->register_operator_user, rt, name, args[1].as.i, assoc, fixity, target, err)) return false;
    *out = ish_atom(rt, "ok");
    return true;
}

static bool prim_expander_register_macro(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    if (!rt->register_macro) return ish_error_set(err, ish_span_unknown(NULL), "expander-register-macro requires an active expansion");
    const IshSyntax *name = ish_syntax_get(args[0], err);
    if (!name) return false;
    if (!ish_is_closure(args[1])) return ish_error_set(err, ish_span_unknown(NULL), "expander-register-macro transformer must be a function value");
    if (!rt->register_macro(rt->register_macro_user, rt, name, args[1], err)) return false;
    *out = ish_atom(rt, "ok");
    return true;
}

static bool prim_expander_surface(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    if (!rt->expander_surface) return ish_error_set(err, ish_span_unknown(NULL), "expander-surface requires an active expansion");
    if (args[0].tag != ISH_VAL_ATOM) return ish_error_set(err, ish_span_unknown(NULL), "expander-surface expects an atom kind");
    return rt->expander_surface(rt->expander_surface_user, rt, ish_symbol_text(args[0].as.symbol), out, err);
}

static bool prim_str_contains(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
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

static bool prim_syntax_int_pred(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == ISH_SYN_INT);
    return true;
}

static bool prim_syntax_int_value(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != ISH_SYN_INT) return ish_error_set(err, syn->span, "syntax-int-value expects int syntax");
    *out = ish_int(syn->as.integer);
    return true;
}

static const char *require_string(IshValue v, size_t *out_len, IshError *err) {
    if (!ish_is_string(v)) {
        ish_error_set(err, ish_span_unknown(NULL), "expected string");
        return NULL;
    }
    if (out_len) *out_len = ish_string_length(v);
    return ish_string_bytes(v);
}

static IshSyntax *make_word_syntax_from(IshSyntax *ctx, const char *text, IshError *err) {
    IshSyntax *out = ish_syn_word(text, ctx ? ctx->span : ish_span_unknown(NULL));
    if (!out) { ish_error_oom(err, ish_span_unknown(NULL)); return NULL; }
    if (!copy_scopes_from(out, ctx)) { ish_syn_free(out); ish_error_oom(err, ish_span_unknown(NULL)); return NULL; }
    return out;
}

static bool prim_make_syntax_word(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IshSyntax *syn = make_word_syntax_from(ctx, text, err);
    if (!syn) return false;
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); return ish_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_atom(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IshSyntax *syn = ish_syn_atom(text, ctx->span);
    if (!syn) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { ish_syn_free(syn); return ish_error_oom(err, ish_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); return ish_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_int(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    if (args[1].tag != ISH_VAL_INT) return ish_error_set(err, ctx->span, "make-syntax-int expects int");
    IshSyntax *syn = ish_syn_int(args[1].as.i, ctx->span);
    if (!syn) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { ish_syn_free(syn); return ish_error_oom(err, ish_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); return ish_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_string(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    size_t len = 0;
    const char *text = require_string(args[1], &len, err);
    if (!text) return false;
    IshSyntax *syn = ish_syn_string_n(text, len, ctx->span);
    if (!syn) return ish_error_oom(err, ish_span_unknown(NULL));
    if (!copy_scopes_from(syn, ctx)) { ish_syn_free(syn); return ish_error_oom(err, ish_span_unknown(NULL)); }
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); return ish_error_oom(err, ctx->span); }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool collect_list_of_syntax(IshValue list, IshSyntax ***out_items, size_t *out_count, IshError *err) {
    size_t count = 0;
    IshValue cur = list;
    while (ish_is_pair(cur)) {
        count++;
        cur = ish_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!ish_is_nil(cur)) return ish_error_set(err, ish_span_unknown(NULL), "expected proper list of syntax");
    IshSyntax **items = count == 0 ? NULL : calloc(count, sizeof(*items));
    if (count != 0 && !items) return ish_error_oom(err, ish_span_unknown(NULL));
    cur = list;
    for (size_t i = 0; i < count; i++) {
        IshValue car = ish_car(cur, err);
        if (err && err->present) { free(items); return false; }
        IshSyntax *inner = ish_syntax_value_get(car);
        if (!inner) {
            free(items);
            return ish_error_set(err, ish_span_unknown(NULL), "list item must be a syntax value");
        }
        items[i] = ish_syn_clone(inner);
        if (!items[i]) {
            for (size_t j = 0; j < i; j++) ish_syn_free(items[j]);
            free(items);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
        cur = ish_cdr(cur, err);
        if (err && err->present) {
            for (size_t j = 0; j <= i; j++) ish_syn_free(items[j]);
            free(items);
            return false;
        }
    }
    *out_items = items;
    *out_count = count;
    return true;
}

static bool prim_make_syntax_list(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    if (args[1].tag != ISH_VAL_ATOM) return ish_error_set(err, ctx->span, "make-syntax-list shape must be atom");
    const char *shape_text = ish_symbol_text(args[1].as.symbol);
    IshSequenceShape shape;
    if (strcmp(shape_text, "paren") == 0) shape = ISH_SEQ_PAREN;
    else if (strcmp(shape_text, "bracket") == 0) shape = ISH_SEQ_BRACKET;
    else return ish_error_set(err, ctx->span, "make-syntax-list shape must be :paren or :bracket");
    IshSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[2], &items, &count, err)) return false;
    IshSyntax *syn = ish_syn_list(shape, ctx->span);
    if (!syn) {
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    if (!copy_scopes_from(syn, ctx)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!ish_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) ish_syn_free(items[j]);
            free(items);
            ish_syn_free(syn);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_vector(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IshSyntax *syn = ish_syn_vector(ctx->span);
    if (!syn || !copy_scopes_from(syn, ctx)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!ish_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) ish_syn_free(items[j]);
            free(items);
            ish_syn_free(syn);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_tuple(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IshSyntax *syn = ish_syn_tuple(ctx->span);
    if (!syn || !copy_scopes_from(syn, ctx)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ish_span_unknown(NULL));
    }
    if (!add_active_intro_scope(rt, syn)) {
        ish_syn_free(syn);
        for (size_t i = 0; i < count; i++) ish_syn_free(items[i]);
        free(items);
        return ish_error_oom(err, ctx->span);
    }
    for (size_t i = 0; i < count; i++) {
        if (!ish_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) ish_syn_free(items[j]);
            free(items);
            ish_syn_free(syn);
            return ish_error_oom(err, ish_span_unknown(NULL));
        }
    }
    free(items);
    return wrap_owned_syntax(rt, syn, out, err);
}

static IshSyntax *protocol_sequence(IshRuntime *rt, IshSyntax *ctx, const char *head, IshSyntax **items, size_t count, IshError *err) {
    (void)rt;
    IshSyntax *syn = ish_syn_list(ISH_SEQ_PAREN, ctx->span);
    if (!syn) return NULL;
    if (!copy_scopes_from(syn, ctx)) { ish_syn_free(syn); return NULL; }
    IshSyntax *head_syn = ish_syn_word(head, ctx->span);
    if (!head_syn || !copy_scopes_from(head_syn, ctx) || !ish_syn_append(syn, head_syn)) {
        ish_syn_free(head_syn);
        ish_syn_free(syn);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!ish_syn_append(syn, items[i])) {
            for (size_t j = i; j < count; j++) ish_syn_free(items[j]);
            ish_syn_free(syn);
            return NULL;
        }
    }
    if (!add_active_intro_scope(rt, syn)) { ish_syn_free(syn); ish_error_oom(err, ctx->span); return NULL; }
    return syn;
}

static bool prim_make_syntax_expr(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IshSyntax *syn = protocol_sequence(rt, ctx, "%-expr", items, count, err);
    free(items);
    if (!syn) return ish_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_body(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IshSyntax *syn = protocol_sequence(rt, ctx, "%-body", items, count, err);
    free(items);
    if (!syn) return ish_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_make_syntax_group(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    IshSyntax *expr = require_syntax(args[1], err);
    if (!expr) return false;
    IshSyntax *item = ish_syn_clone(expr);
    if (!item) return ish_error_oom(err, ctx->span);
    IshSyntax *items[1] = { item };
    IshSyntax *syn = protocol_sequence(rt, ctx, "%-group", items, 1, err);
    if (!syn) return ish_error_oom(err, ctx->span);
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_syntax_error(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    (void)rt;
    (void)out;
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *msg = require_string(args[1], NULL, err);
    if (!msg) return false;
    return ish_error_set(err, syn->span, "%s", msg);
}

static bool prim_local_expand(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (!rt->local_expand) return ish_error_set(err, syn->span, "local-expand is only available during macro expansion");
    IshSyntax *expanded = NULL;
    if (!rt->local_expand(rt->local_expand_user, rt, syn, &expanded, err)) return false;
    if (!expanded) return ish_error_set(err, syn->span, "local-expand returned no syntax");
    bool ok = wrap_owned_syntax(rt, expanded, out, err);
    return ok;
}

static bool identifier_text(IshValue v, const char **out_text, IshSyntax **out_syn, IshError *err) {
    IshSyntax *syn = require_syntax(v, err);
    if (!syn) return false;
    if (syn->kind != ISH_SYN_WORD) return ish_error_set(err, syn->span, "expected identifier syntax");
    *out_text = syn->as.text;
    *out_syn = syn;
    return true;
}

static bool prim_bound_identifier_eq(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    const char *a_text = NULL;
    const char *b_text = NULL;
    IshSyntax *a_syn = NULL;
    IshSyntax *b_syn = NULL;
    if (!identifier_text(args[0], &a_text, &a_syn, err) || !identifier_text(args[1], &b_text, &b_syn, err)) return false;
    bool same = strcmp(a_text, b_text) == 0;
    if (same) {
        const IshScopeSet *as = ish_syn_scope_set(a_syn, 0);
        const IshScopeSet *bs = ish_syn_scope_set(b_syn, 0);
        IshScopeSet empty_a, empty_b;
        ish_scope_set_init(&empty_a);
        ish_scope_set_init(&empty_b);
        const IshScopeSet *la = as ? as : &empty_a;
        const IshScopeSet *lb = bs ? bs : &empty_b;
        same = ish_scope_set_equal(la, lb);
        ish_scope_set_destroy(&empty_a);
        ish_scope_set_destroy(&empty_b);
    }
    *out = truth(rt, same);
    return true;
}

static bool prim_free_identifier_eq(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    const char *a_text = NULL;
    const char *b_text = NULL;
    IshSyntax *a_syn = NULL;
    IshSyntax *b_syn = NULL;
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

static bool prim_bind_bang(IshRuntime *rt, IshValue *args, IshValue *out, IshError *err) {
    IshSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = require_string(args[1], NULL, err);
    if (!text) return false;
    IshSyntax *syn = make_word_syntax_from(ctx, text, err);
    if (!syn) return false;
    if (rt->macro_intro_active && !ish_syn_scope_add(syn, 0, rt->macro_intro_scope)) {
        ish_syn_free(syn);
        return ish_error_oom(err, ctx->span);
    }
    return wrap_owned_syntax(rt, syn, out, err);
}

bool ish_prim_invoke(IshRuntime *rt, IshPrimitive prim, IshValue *args, uint32_t argc, IshValue *out, IshError *err) {
    if (!require_arity(prim, argc, err)) return false;
    switch (prim) {
        case ISH_PRIM_CONS: return prim_cons(rt, args, out, err);
        case ISH_PRIM_FIRST: return prim_first(rt, args, out, err);
        case ISH_PRIM_REST: return prim_rest(rt, args, out, err);
        case ISH_PRIM_LIST: return prim_list(rt, args, argc, out, err);
        case ISH_PRIM_TUPLE: return prim_tuple(rt, args, argc, out, err);
        case ISH_PRIM_VECTOR: return prim_vector(rt, args, argc, out, err);
        case ISH_PRIM_DICT: return prim_dict(rt, args, argc, out, err);
        case ISH_PRIM_TUPLE_GET: return prim_tuple_get(rt, args, out, err);
        case ISH_PRIM_APPEND: return prim_append(rt, args, out, err);
        case ISH_PRIM_TO_LIST: return prim_to_list(rt, args, out, err);
        case ISH_PRIM_SYNTAX_KIND: return prim_syntax_kind(rt, args, out, err);
        case ISH_PRIM_SYNTAX_TO_DATUM: return prim_syntax_to_datum(rt, args, out, err);
        case ISH_PRIM_DATUM_TO_SYNTAX: return prim_datum_to_syntax(rt, args, out, err);
        case ISH_PRIM_SYNTAX_PROPERTY: return prim_syntax_property(rt, args, out, err);
        case ISH_PRIM_SYNTAX_SET_PROPERTY: return prim_syntax_set_property(rt, args, out, err);
        case ISH_PRIM_SYNTAX_ORIGIN: return prim_syntax_origin(rt, args, out, err);
        case ISH_PRIM_SYNTAX_LIST_PRED: return prim_syntax_list_pred(rt, args, out, err);
        case ISH_PRIM_SYNTAX_LENGTH: return prim_syntax_length(rt, args, out, err);
        case ISH_PRIM_SYNTAX_NTH: return prim_syntax_nth(rt, args, out, err);
        case ISH_PRIM_SYNTAX_SLICE: return prim_syntax_slice(rt, args, out, err);
        case ISH_PRIM_SYNTAX_WORD_PRED: return prim_syntax_word_pred(rt, args, out, err);
        case ISH_PRIM_SYNTAX_WORD_TEXT: return prim_syntax_word_text(rt, args, out, err);
        case ISH_PRIM_SYNTAX_ATOM_PRED: return prim_syntax_atom_pred(rt, args, out, err);
        case ISH_PRIM_SYNTAX_ATOM_TEXT: return prim_syntax_atom_text(rt, args, out, err);
        case ISH_PRIM_SYNTAX_INT_PRED: return prim_syntax_int_pred(rt, args, out, err);
        case ISH_PRIM_SYNTAX_ADJACENT_PRED: return prim_syntax_adjacent_pred(rt, args, out, err);
        case ISH_PRIM_SYNTAX_STRING_TEXT: return prim_syntax_string_text(rt, args, out, err);
        case ISH_PRIM_STR_CONTAINS: return prim_str_contains(rt, args, out, err);
        case ISH_PRIM_EXPANDER_REGISTER_OPERATOR: return prim_expander_register_operator(rt, args, out, err);
        case ISH_PRIM_EXPANDER_REGISTER_MACRO: return prim_expander_register_macro(rt, args, out, err);
        case ISH_PRIM_EXPANDER_SURFACE: return prim_expander_surface(rt, args, out, err);
        case ISH_PRIM_SYNTAX_INT_VALUE: return prim_syntax_int_value(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_WORD: return prim_make_syntax_word(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_ATOM: return prim_make_syntax_atom(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_INT: return prim_make_syntax_int(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_STRING: return prim_make_syntax_string(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_LIST: return prim_make_syntax_list(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_VECTOR: return prim_make_syntax_vector(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_TUPLE: return prim_make_syntax_tuple(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_EXPR: return prim_make_syntax_expr(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_BODY: return prim_make_syntax_body(rt, args, out, err);
        case ISH_PRIM_MAKE_SYNTAX_GROUP: return prim_make_syntax_group(rt, args, out, err);
        case ISH_PRIM_SYNTAX_ERROR: return prim_syntax_error(rt, args, out, err);
        case ISH_PRIM_LOCAL_EXPAND: return prim_local_expand(rt, args, out, err);
        case ISH_PRIM_FREE_IDENTIFIER_EQ: return prim_free_identifier_eq(rt, args, out, err);
        case ISH_PRIM_BOUND_IDENTIFIER_EQ: return prim_bound_identifier_eq(rt, args, out, err);
        case ISH_PRIM_BIND_BANG: return prim_bind_bang(rt, args, out, err);
        case ISH_PRIM_ADD: return prim_add(rt, args, out, err);
        case ISH_PRIM_SUB: return prim_sub(rt, args, out, err);
        case ISH_PRIM_MUL: return prim_mul(rt, args, out, err);
        case ISH_PRIM_DIV: return prim_div(rt, args, out, err);
        case ISH_PRIM_MOD: return prim_mod(rt, args, out, err);
        case ISH_PRIM_POW: return prim_pow(rt, args, out, err);
        case ISH_PRIM_NEG: return prim_neg(rt, args, out, err);
        case ISH_PRIM_EQ: return prim_eq(rt, args, out, err);
        case ISH_PRIM_NEQ: return prim_neq(rt, args, out, err);
        case ISH_PRIM_LT: return prim_lt(rt, args, out, err);
        case ISH_PRIM_GT: return prim_gt(rt, args, out, err);
        case ISH_PRIM_LTE: return prim_lte(rt, args, out, err);
        case ISH_PRIM_GTE: return prim_gte(rt, args, out, err);
        case ISH_PRIM_SELF:
        case ISH_PRIM_SPAWN:
        case ISH_PRIM_SEND:
        case ISH_PRIM_EXIT:
        case ISH_PRIM_LINK:
        case ISH_PRIM_UNLINK:
        case ISH_PRIM_MONITOR:
        case ISH_PRIM_DEMONITOR:
        case ISH_PRIM_TRAP_EXIT:
        case ISH_PRIM_EXEC:
        case ISH_PRIM_AWAIT:
            return ish_error_set(err, ish_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", ish_primitive_name(prim));
        case ISH_PRIM_APPLY:
            return ish_error_set(err, ish_span_unknown(NULL), "primitive 'apply' is compiled directly and cannot be invoked generically");
        case ISH_PRIM_STR: return prim_str(rt, args, argc, out, err);
        case ISH_PRIM_CHOMP: return prim_chomp(rt, args, out, err);
        case ISH_PRIM_CAPTURE_STDOUT: return prim_capture_stdout(rt, args, out, err);
        case ISH_PRIM_PRINT: return prim_print_impl(rt, args, argc, false, out, err);
        case ISH_PRIM_PRINTLN: return prim_print_impl(rt, args, argc, true, out, err);
        case ISH_PRIM_CD: return prim_cd(rt, args, out, err);
        case ISH_PRIM_PWD: return prim_pwd(rt, args, out, err);
        case ISH_PRIM_ENV_GET: return prim_env_get(rt, args, out, err);
        case ISH_PRIM_ENV_SET: return prim_env_set(rt, args, out, err);
        case ISH_PRIM_WRITE_PROCSUB_TEMP: return prim_write_procsub_temp(rt, args, out, err);
        case ISH_PRIM_MAKE_PROCSUB_TEMP: return prim_make_procsub_temp(rt, args, out, err);
        case ISH_PRIM_MAKE_RECORD: return prim_make_record(rt, args, out, err);
        case ISH_PRIM_RECORD_PRED: return prim_record_pred(rt, args, out, err);
        case ISH_PRIM_RECORD_TYPE: return prim_record_type(rt, args, out, err);
        case ISH_PRIM_RECORD_FIELD: return prim_record_field(rt, args, out, err);
    }
    return ish_error_set(err, ish_span_unknown(NULL), "unimplemented primitive '%s'", ish_primitive_name(prim));
}

bool ish_prim_lookup_by_name(const char *name, IshPrimitive *out) {
    size_t count = ish_primitive_count();
    for (size_t i = 0; i < count; i++) {
        const IshPrimitiveInfo *info = ish_primitive_info((IshPrimitive)i);
        if (info && strcmp(info->name, name) == 0) {
            *out = (IshPrimitive)i;
            return true;
        }
    }
    return false;
}
