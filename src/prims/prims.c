#define _XOPEN_SOURCE 700

#include "idiom/prims.h"
#include "idiom/actor.h"
#include "idiom/bytecode.h"
#include "idiom/expand.h"
#include "idiom/ports.h"
#include "idiom/regex.h"
#include "idiom/syntax.h"
#include "idiom/tty.h"
#include "idiom/vm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

bool idm_primitive_type_error(IdmRuntime *rt, IdmError *err, const char *name, IdmValue got, const char *what) {
    idm_error_set(err, idm_span_unknown(NULL), "%s expects %s", name, what);
    return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, name), got);
}

static bool value_to_i64(IdmValue value, int64_t *out) {
    return idm_value_is_int(value) && idm_int_to_i64(value, out);
}

static IdmScheduler *job_sched(const char *name, IdmError *err);

bool idm_primitive_require_arity(IdmRuntime *rt, IdmPrimitive prim, uint32_t argc, IdmError *err) {
    const IdmPrimitiveInfo *info = idm_primitive_info(prim);
    if (!info) return idm_error_set(err, idm_span_unknown(NULL), "unknown primitive %u", (unsigned)prim);
    if (argc < info->min_arity || argc > info->max_arity) {
        idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' arity mismatch: got %u, want %u..%u", info->name, argc, info->min_arity, info->max_arity);
        return idm_error_reason(rt, err, "arity", 4, idm_atom(rt, info->name), idm_int((int64_t)argc), idm_int((int64_t)info->min_arity), idm_int((int64_t)info->max_arity));
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

static bool prim_cons(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    *out = idm_cons(rt, args[0], args[1], err);
    return !(err && err->present);
}

static bool prim_first(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_pair(args[0])) return idm_primitive_type_error(rt, err, "first", args[0], "a pair");
    *out = idm_car(args[0], err);
    return !(err && err->present);
}

static bool prim_rest(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_pair(args[0])) return idm_primitive_type_error(rt, err, "rest", args[0], "a pair");
    *out = idm_cdr(args[0], err);
    return !(err && err->present);
}

static bool prim_list(IdmRuntime *rt, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    IdmValue result = idm_empty_list();
    for (size_t i = argc; i > 0; i--) {
        result = idm_cons(rt, args[i - 1u], result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_append(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue head = args[0];
    IdmValue tail = args[1];
    if (idm_is_empty_list(head)) { *out = tail; return true; }
    if (!idm_is_pair(head)) return idm_primitive_type_error(rt, err, "append", head, "a list as first argument");
    size_t count = 0;
    IdmValue cur = head;
    while (idm_is_pair(cur)) {
        count++;
        cur = idm_cdr(cur, err);
        if (err && err->present) return false;
    }
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "append expects a proper list as first argument");
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

static bool prim_tuple(IdmRuntime *rt, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    *out = idm_tuple(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_vector(IdmRuntime *rt, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    *out = idm_vector(rt, args, argc, err);
    return !(err && err->present);
}

static bool prim_dict(IdmRuntime *rt, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
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
    if (idm_value_tag(v) == IDM_VAL_STRING) return idm_buf_append_n(buf, idm_string_bytes(v), idm_string_length(v));
    if (idm_value_tag(v) == IDM_VAL_ATOM || idm_value_tag(v) == IDM_VAL_WORD) return idm_buf_append(buf, idm_symbol_text(idm_value_symbol(v)));
    if (idm_value_tag(v) == IDM_VAL_NIL) return true;
    return idm_value_write(buf, v);
}

static bool prim_str(IdmRuntime *rt, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    return idm_string_concat_display(rt, args, argc, out, err);
}

static bool prim_chomp(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_STRING) { *out = args[0]; return true; }
    const char *bytes = idm_string_bytes(args[0]);
    size_t len = idm_string_length(args[0]);
    while (len > 0 && (bytes[len - 1u] == '\n' || bytes[len - 1u] == '\r')) len--;
    *out = idm_string_n(rt, bytes, len, err);
    return !(err && err->present);
}

static bool prim_tuple_get(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_tuple(args[0])) return idm_primitive_type_error(rt, err, "tuple-get", args[0], "a tuple");
    int64_t idx = 0;
    if (!value_to_i64(args[1], &idx) || idx < 0) return idm_primitive_type_error(rt, err, "tuple-get", args[1], "a non-negative integer index");
    size_t index = (size_t)idx;
    if (index >= idm_sequence_count(args[0])) {
        idm_error_set(err, idm_span_unknown(NULL), "tuple-get index out of range");
        return idm_error_reason(rt, err, "index-out-of-range", 1, args[1]);
    }
    *out = idm_sequence_item(args[0], index, err);
    return !(err && err->present);
}

static size_t utf8_decode(const unsigned char *s, size_t len, int32_t *cp) {
    unsigned char b = s[0];
    if (b < 0x80) { *cp = (int32_t)b; return 1u; }
    size_t need;
    int32_t min, c;
    if ((b & 0xE0u) == 0xC0u) { need = 2u; min = 0x80; c = b & 0x1F; }
    else if ((b & 0xF0u) == 0xE0u) { need = 3u; min = 0x800; c = b & 0x0F; }
    else if ((b & 0xF8u) == 0xF0u) { need = 4u; min = 0x10000; c = b & 0x07; }
    else { *cp = 0xFFFD; return 1u; }
    if (len < need) { *cp = 0xFFFD; return 1u; }
    for (size_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { *cp = 0xFFFD; return 1u; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) { *cp = 0xFFFD; return 1u; }
    *cp = c;
    return need;
}

static size_t utf8_encode(int64_t cp, char buf[4]) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0u;
    if (cp < 0x80) { buf[0] = (char)cp; return 1u; }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2u;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3u;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4u;
}

static bool prim_seq_count(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    size_t count = 0;
    if (idm_is_vector(v) || idm_is_tuple(v)) {
        count = idm_sequence_count(v);
    } else if (idm_is_dict(v)) {
        count = idm_dict_count(v);
    } else if (idm_is_string(v)) {
        count = idm_string_length(v);
    } else if (idm_is_empty_list(v) || idm_is_pair(v)) {
        IdmValue cur = v;
        while (idm_is_pair(cur)) {
            count++;
            cur = idm_cdr(cur, err);
            if (err && err->present) return false;
        }
        if (!idm_is_empty_list(cur)) return idm_primitive_type_error(rt, err, "seq-count", v, "a proper list, vector, tuple, dict, or string");
    } else {
        return idm_primitive_type_error(rt, err, "seq-count", v, "a proper list, vector, tuple, dict, or string");
    }
    *out = idm_int_promote(rt, (int64_t)count, err);
    return !(err && err->present);
}

static bool prim_seq_nth(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    int64_t index = 0;
    if (!idm_int_to_i64(args[1], &index) || index < 0) return idm_primitive_type_error(rt, err, "seq-nth", args[1], "a non-negative machine integer");
    if (idm_is_vector(v) || idm_is_tuple(v)) {
        if ((uint64_t)index >= idm_sequence_count(v)) return idm_primitive_type_error(rt, err, "seq-nth", args[1], "an index inside the sequence");
        *out = idm_sequence_item(v, (size_t)index, err);
        return !(err && err->present);
    }
    if (idm_is_string(v)) {
        if ((uint64_t)index >= idm_string_length(v)) return idm_primitive_type_error(rt, err, "seq-nth", args[1], "an index inside the string");
        *out = idm_int((int64_t)(unsigned char)idm_string_bytes(v)[index]);
        return true;
    }
    return idm_primitive_type_error(rt, err, "seq-nth", v, "a vector, tuple, or string");
}

static bool prim_dict_to_list(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_dict(v)) return idm_primitive_type_error(rt, err, "dict-to-list", v, "a dict");
    IdmValue result = idm_empty_list();
    IdmDictIter it;
    IdmValue key;
    IdmValue val;
    idm_dict_iter_init(v, &it);
    while (idm_dict_iter_next(&it, &key, &val)) {
        IdmValue pair_items[2] = {key, val};
        IdmValue pair = idm_tuple(rt, pair_items, 2u, err);
        if (err && err->present) return false;
        result = idm_cons(rt, pair, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_str_to_list(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_string(v)) return idm_primitive_type_error(rt, err, "runes-lossy", v, "a string");
    const unsigned char *bytes = (const unsigned char *)idm_string_bytes(v);
    size_t len = idm_string_length(v);
    IdmValue runes = idm_empty_list();
    for (size_t i = 0; i < len;) {
        int32_t cp;
        i += utf8_decode(bytes + i, len - i, &cp);
        runes = idm_cons(rt, idm_int((int64_t)cp), runes, err);
        if (err && err->present) return false;
    }
    IdmValue result = idm_empty_list();
    for (; idm_is_pair(runes); runes = idm_cdr(runes, err)) {
        result = idm_cons(rt, idm_car(runes, err), result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool expand_home_path(IdmExec *exec, char **path_io, IdmError *err) {
    char *path = *path_io;
    if (path[0] != '~' || (path[1] != '\0' && path[1] != '/')) return true;
    const char *home = idm_exec_env_get(exec, "HOME");
    if (!home) home = getenv("HOME");
    if (!home || home[0] == '\0') return idm_error_set(err, idm_span_unknown(NULL), "cd: HOME not set");
    IdmBuffer expanded;
    idm_buf_init(&expanded);
    bool ok = idm_buf_append(&expanded, home);
    if (ok && path[1] == '/') ok = idm_buf_append(&expanded, path + 1);
    if (!ok) {
        idm_buf_destroy(&expanded);
        return idm_error_oom(err, idm_span_unknown(NULL));
    }
    free(path);
    *path_io = idm_buf_take(&expanded);
    if (!*path_io) return idm_error_oom(err, idm_span_unknown(NULL));
    return true;
}

static bool prim_cd(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &bytes, &len, "cd", err)) return false;
    IdmExec *exec = idm_current_exec();
    if (!exec) return idm_error_set(err, idm_span_unknown(NULL), "cd requires an actor context");
    char *path = idm_strndup(bytes, len);
    if (!path) return idm_error_oom(err, idm_span_unknown(NULL));
    if (!expand_home_path(exec, &path, err)) { free(path); return false; }
    char *candidate = NULL;
    if (path[0] == '/') {
        candidate = path;
        path = NULL;
    } else {
        const char *base = idm_exec_cwd(exec);
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
    if (!idm_exec_set_cwd(exec, resolved)) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_pwd(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)args;
    const char *cwd = idm_exec_cwd(idm_current_exec());
    if (cwd) {
        *out = idm_string(rt, cwd, err);
        return !(err && err->present);
    }
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return idm_error_set(err, idm_span_unknown(NULL), "pwd: %s", strerror(errno));
    *out = idm_string(rt, buf, err);
    return !(err && err->present);
}

static bool prim_env_get(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &bytes, &len, "env-get", err)) return false;
    char *name = idm_strndup(bytes, len);
    if (!name) return idm_error_oom(err, idm_span_unknown(NULL));
    const char *value = idm_exec_env_get(idm_current_exec(), name);
    if (!value) value = getenv(name);
    free(name);
    if (!value) {
        *out = idm_nil();
        return true;
    }
    *out = idm_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_env_set(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t name_len = 0;
    const char *name_bytes = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &name_bytes, &name_len, "env-set", err)) return false;
    size_t value_len = 0;
    const char *value_bytes = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &value_bytes, &value_len, "env-set", err)) return false;
    if (!idm_current_exec()) return idm_error_set(err, idm_span_unknown(NULL), "env-set requires an actor context");
    char *name = idm_strndup(name_bytes, name_len);
    char *value = idm_strndup(value_bytes, value_len);
    bool ok = name && value && idm_exec_env_set(idm_current_exec(), name, value);
    free(name);
    free(value);
    if (!ok) return idm_error_oom(err, idm_span_unknown(NULL));
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_print_to(IdmRuntime *rt, FILE *stream, const IdmValue *args, uint32_t argc, bool newline, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (uint32_t i = 0; i < argc; i++) {
        if (i != 0 && !idm_buf_append_char(&buf, ' ')) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
        if (!append_display(&buf, args[i])) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (newline && !idm_buf_append_char(&buf, '\n')) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    if (buf.len != 0) fwrite(buf.data, 1u, buf.len, stream);
    fflush(stream);
    idm_buf_destroy(&buf);
    *out = idm_nil();
    return true;
}

static bool prim_print_impl(IdmRuntime *rt, const IdmValue *args, uint32_t argc, bool newline, IdmValue *out, IdmError *err) {
    return prim_print_to(rt, stdout, args, argc, newline, out, err);
}

bool idm_checked_add(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

bool idm_checked_sub(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
}

bool idm_checked_mul(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return true; }
    if (a == -1) {
        if (b == INT64_MIN) return false;
        *out = -b;
        return true;
    }
    if (b == -1) {
        if (a == INT64_MIN) return false;
        *out = -a;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return false;
        } else {
            if (b < INT64_MIN / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return false;
        } else {
            if (b < INT64_MAX / a) return false;
        }
    }
    *out = a * b;
    return true;
}

bool idm_checked_pow(int64_t base, int64_t exponent, int64_t *out) {
    if (exponent < 0) return false;
    int64_t result = 1;
    int64_t factor = base;
    int64_t exp = exponent;
    while (exp > 0) {
        if ((exp & 1) != 0) {
            if (!idm_checked_mul(result, factor, &result)) return false;
        }
        exp >>= 1;
        if (exp != 0 && !idm_checked_mul(factor, factor, &factor)) return false;
    }
    *out = result;
    return true;
}

static bool num_as_double(IdmValue v, double *out) {
    if (idm_value_is_int(v)) { *out = idm_int_to_double(v); return true; }
    if (idm_value_tag(v) == IDM_VAL_FLOAT) { *out = idm_float_value(v); return true; }
    return false;
}

static IdmValue float_result(IdmRuntime *rt, double value, IdmError *err) {
    if (isnan(value)) return idm_atom(rt, "nan");
    if (isinf(value)) return idm_atom(rt, "inf");
    return idm_float(rt, value, err);
}

static bool int_result(IdmRuntime *rt, int64_t v, const char *name, IdmValue *out, IdmError *err) {
    (void)name;
    if (idm_fixnum_fits(v)) { *out = idm_int(v); return true; }
    *out = idm_int_promote(rt, v, err);
    return !(err && err->present);
}

static bool div_zero_error(IdmRuntime *rt, const char *name, IdmError *err) {
    idm_error_set(err, idm_span_unknown(NULL), "division by zero in %s", name);
    return idm_error_reason(rt, err, "div-by-zero", 1, idm_atom(rt, name));
}

static bool is_int_zero(IdmValue v) {
    return idm_value_is_int(v) && idm_int_compare(v, idm_int(0)) == 0;
}

static bool prim_arith_big(IdmRuntime *rt, IdmPrimitive prim, const char *name, IdmValue a, IdmValue b, IdmValue *out, IdmError *err) {
    switch (prim) {
        case IDM_PRIM_ADD: return idm_int_add(rt, a, b, out, err);
        case IDM_PRIM_SUB: return idm_int_sub(rt, a, b, out, err);
        case IDM_PRIM_MUL: return idm_int_mul(rt, a, b, out, err);
        case IDM_PRIM_DIV:
        case IDM_PRIM_MOD: {
            if (is_int_zero(b)) return div_zero_error(rt, name, err);
            IdmValue q = idm_nil(), r = idm_nil();
            if (!idm_int_divmod(rt, a, b, &q, &r, err)) return false;
            *out = prim == IDM_PRIM_DIV ? q : r;
            return true;
        }
        case IDM_PRIM_POW: {
            int64_t e = 0;
            if (!idm_int_to_i64(b, &e) || e < 0) {
                idm_error_set(err, idm_span_unknown(NULL), "pow exponent must be a non-negative machine integer");
                return idm_error_reason(rt, err, "bad-arg", 2, idm_atom(rt, name), b);
            }
            return idm_int_pow(rt, a, e, out, err);
        }
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid integer primitive");
    }
}

static bool int_floor_divmod(IdmRuntime *rt, const char *name, IdmValue a, IdmValue b, IdmValue *q_out, IdmValue *r_out, IdmError *err) {
    if (is_int_zero(b)) return div_zero_error(rt, name, err);
    if (idm_value_tag(a) == IDM_VAL_INT && idm_value_tag(b) == IDM_VAL_INT) {
        int64_t ai = idm_int_value(a), bi = idm_int_value(b);
        int64_t q = ai / bi, r = ai % bi;
        if (r != 0 && ((r < 0) != (bi < 0))) { q -= 1; r += bi; }
        *q_out = idm_int_promote(rt, q, err);
        *r_out = idm_int_promote(rt, r, err);
        return !(err && err->present);
    }
    IdmValue q = idm_nil(), r = idm_nil();
    if (!idm_int_divmod(rt, a, b, &q, &r, err)) return false;
    bool r_neg = idm_int_compare(r, idm_int(0)) < 0;
    bool b_neg = idm_int_compare(b, idm_int(0)) < 0;
    if (!is_int_zero(r) && r_neg != b_neg) {
        if (!idm_int_sub(rt, q, idm_int(1), &q, err)) return false;
        if (!idm_int_add(rt, r, b, &r, err)) return false;
    }
    *q_out = q;
    *r_out = r;
    return true;
}

static bool prim_arith(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    IdmValueTag t0 = idm_value_tag(args[0]), t1 = idm_value_tag(args[1]);
    if (t0 == IDM_VAL_INT && t1 == IDM_VAL_INT) {
        int64_t a = idm_int_value(args[0]), b = idm_int_value(args[1]), r = 0;
        bool ok = true;
        switch (prim) {
            case IDM_PRIM_ADD: ok = idm_checked_add(a, b, &r); break;
            case IDM_PRIM_SUB: ok = idm_checked_sub(a, b, &r); break;
            case IDM_PRIM_MUL: ok = idm_checked_mul(a, b, &r); break;
            case IDM_PRIM_DIV:
            case IDM_PRIM_MOD:
                if (b == 0) return div_zero_error(rt, name, err);
                r = prim == IDM_PRIM_DIV ? a / b : a % b;
                return int_result(rt, r, name, out, err);
            case IDM_PRIM_POW:
                if (b < 0) {
                    idm_error_set(err, idm_span_unknown(NULL), "pow exponent must be non-negative");
                    return idm_error_reason(rt, err, "bad-arg", 2, idm_atom(rt, name), args[1]);
                }
                ok = idm_checked_pow(a, b, &r);
                break;
            default: return idm_error_set(err, idm_span_unknown(NULL), "invalid integer primitive");
        }
        if (ok) return int_result(rt, r, name, out, err);
        return prim_arith_big(rt, prim, name, args[0], args[1], out, err);
    }
    if ((t0 == IDM_VAL_INT || t0 == IDM_VAL_BIGNUM) && (t1 == IDM_VAL_INT || t1 == IDM_VAL_BIGNUM)) {
        return prim_arith_big(rt, prim, name, args[0], args[1], out, err);
    }
    double x = 0.0, y = 0.0;
    if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) {
        idm_error_set(err, idm_span_unknown(NULL), "%s expects numeric operands", name);
        return idm_error_reason(rt, err, "type-error", 3, idm_atom(rt, name), args[0], args[1]);
    }
    double f = 0.0;
    switch (prim) {
        case IDM_PRIM_ADD: f = x + y; break;
        case IDM_PRIM_SUB: f = x - y; break;
        case IDM_PRIM_MUL: f = x * y; break;
        case IDM_PRIM_DIV:
            if (y == 0.0) return div_zero_error(rt, name, err);
            f = x / y;
            break;
        case IDM_PRIM_MOD:
            if (y == 0.0) return div_zero_error(rt, name, err);
            f = fmod(x, y);
            break;
        case IDM_PRIM_POW: f = pow(x, y); break;
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid numeric primitive");
    }
    *out = float_result(rt, f, err);
    return !(err && err->present);
}

static bool prim_neg(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) == IDM_VAL_FLOAT) { *out = float_result(rt, -idm_float_value(args[0]), err); return !(err && err->present); }
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, "neg", args[0], "a number");
    return idm_int_neg(rt, args[0], out, err);
}

static bool prim_num_compare(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    IdmValueTag t0 = idm_value_tag(args[0]), t1 = idm_value_tag(args[1]);
    int c = 0;
    if (t0 == IDM_VAL_INT && t1 == IDM_VAL_INT) {
        int64_t a = idm_int_value(args[0]), b = idm_int_value(args[1]);
        c = a < b ? -1 : (a > b ? 1 : 0);
    } else if ((t0 == IDM_VAL_INT || t0 == IDM_VAL_BIGNUM) && (t1 == IDM_VAL_INT || t1 == IDM_VAL_BIGNUM)) {
        c = idm_int_compare(args[0], args[1]);
    } else {
        double x = 0.0, y = 0.0;
        if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) {
            idm_error_set(err, idm_span_unknown(NULL), "%s expects numeric operands", name);
            return idm_error_reason(rt, err, "type-error", 3, idm_atom(rt, name), args[0], args[1]);
        }
        c = x < y ? -1 : (x > y ? 1 : 0);
    }
    bool r = false;
    switch (prim) {
        case IDM_PRIM_LT: r = c < 0; break;
        case IDM_PRIM_GT: r = c > 0; break;
        case IDM_PRIM_LTE: r = c <= 0; break;
        case IDM_PRIM_GTE: r = c >= 0; break;
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid comparison primitive");
    }
    *out = idm_bool(rt, r);
    return true;
}

static bool prim_eq(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_value_equal(args[0], args[1]));
    return true;
}

static bool prim_neq(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, !idm_value_equal(args[0], args[1]));
    return true;
}

static int cmp_i64(int64_t a, int64_t b) {
    return (a > b) - (a < b);
}

static int cmp_size(size_t a, size_t b) {
    return (a > b) - (a < b);
}

static int cmp_text(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int r = n == 0 ? 0 : memcmp(a, b, n);
    if (r < 0) return -1;
    if (r > 0) return 1;
    return cmp_size(alen, blen);
}

static int cmp_ptr(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)a;
    uintptr_t y = (uintptr_t)b;
    return (x > y) - (x < y);
}

static int cmp_value_total(IdmValue a, IdmValue b);

static bool dict_collect_entries(IdmValue d, IdmDictEntry *out, size_t n) {
    IdmDictIter it;
    IdmValue key, val;
    idm_dict_iter_init(d, &it);
    size_t i = 0;
    while (i < n && idm_dict_iter_next(&it, &key, &val)) {
        out[i].key = key;
        out[i].value = val;
        i++;
    }
    return i == n;
}

static int cmp_dict_entry(const void *pa, const void *pb) {
    const IdmDictEntry *x = pa;
    const IdmDictEntry *y = pb;
    int r = cmp_value_total(x->key, y->key);
    return r != 0 ? r : cmp_value_total(x->value, y->value);
}

static int cmp_dict_total(IdmValue a, IdmValue b) {
    size_t ac = idm_dict_count(a);
    size_t bc = idm_dict_count(b);
    IdmDictEntry *a_entries = ac ? calloc(ac, sizeof(*a_entries)) : NULL;
    IdmDictEntry *b_entries = bc ? calloc(bc, sizeof(*b_entries)) : NULL;
    int r = 0;
    if ((ac && (!a_entries || !dict_collect_entries(a, a_entries, ac))) ||
        (bc && (!b_entries || !dict_collect_entries(b, b_entries, bc)))) {
        free(a_entries);
        free(b_entries);
        return cmp_size(ac, bc);
    }
    qsort(a_entries, ac, sizeof(*a_entries), cmp_dict_entry);
    qsort(b_entries, bc, sizeof(*b_entries), cmp_dict_entry);
    size_t n = ac < bc ? ac : bc;
    for (size_t i = 0; r == 0 && i < n; i++) {
        r = cmp_value_total(a_entries[i].key, b_entries[i].key);
        if (r == 0) r = cmp_value_total(a_entries[i].value, b_entries[i].value);
    }
    free(a_entries);
    free(b_entries);
    return r != 0 ? r : cmp_size(ac, bc);
}

static int cmp_sequence_total(IdmValue a, IdmValue b) {
    size_t ac = idm_sequence_count(a);
    size_t bc = idm_sequence_count(b);
    size_t n = ac < bc ? ac : bc;
    for (size_t i = 0; i < n; i++) {
        int r = cmp_value_total(idm_sequence_item(a, i, NULL), idm_sequence_item(b, i, NULL));
        if (r != 0) return r;
    }
    return cmp_size(ac, bc);
}

static int cmp_record_total(IdmValue a, IdmValue b) {
    IdmSymbol *at = idm_record_type_symbol(a, NULL);
    IdmSymbol *bt = idm_record_type_symbol(b, NULL);
    int r = cmp_size(at ? idm_symbol_id(at) : 0u, bt ? idm_symbol_id(bt) : 0u);
    if (r != 0) return r;
    size_t ac = idm_record_field_count(a, NULL);
    size_t bc = idm_record_field_count(b, NULL);
    size_t n = ac < bc ? ac : bc;
    for (size_t i = 0; i < n; i++) {
        IdmSymbol *an = idm_record_field_name_symbol(a, i, NULL);
        IdmSymbol *bn = idm_record_field_name_symbol(b, i, NULL);
        r = cmp_size(an ? idm_symbol_id(an) : 0u, bn ? idm_symbol_id(bn) : 0u);
        if (r != 0) return r;
        r = cmp_value_total(idm_record_field_value(a, i, NULL), idm_record_field_value(b, i, NULL));
        if (r != 0) return r;
    }
    return cmp_size(ac, bc);
}

static int cmp_value_total(IdmValue a, IdmValue b) {
    IdmValueTag ta = idm_value_tag(a), tb = idm_value_tag(b);
    if (ta != tb) return (int)ta < (int)tb ? -1 : 1;
    switch (ta) {
        case IDM_VAL_NIL:
            return 0;
        case IDM_VAL_BIGNUM:
            return idm_int_compare(a, b);
        case IDM_VAL_ATOM:
        case IDM_VAL_WORD:
            return strcmp(idm_symbol_text(idm_value_symbol(a)), idm_symbol_text(idm_value_symbol(b)));
        case IDM_VAL_INT:
            return cmp_i64(idm_int_value(a), idm_int_value(b));
        case IDM_VAL_FLOAT:
            if (isnan(idm_float_value(a)) || isnan(idm_float_value(b))) {
                if (isnan(idm_float_value(a)) && isnan(idm_float_value(b))) return 0;
                return isnan(idm_float_value(a)) ? -1 : 1;
            }
            return (idm_float_value(a) > idm_float_value(b)) - (idm_float_value(a) < idm_float_value(b));
        case IDM_VAL_STRING:
            return cmp_text(idm_string_bytes(a), idm_string_length(a), idm_string_bytes(b), idm_string_length(b));
        case IDM_VAL_PAIR: {
            int r = cmp_value_total(idm_car(a, NULL), idm_car(b, NULL));
            return r != 0 ? r : cmp_value_total(idm_cdr(a, NULL), idm_cdr(b, NULL));
        }
        case IDM_VAL_TUPLE:
        case IDM_VAL_VECTOR:
            return cmp_sequence_total(a, b);
        case IDM_VAL_DICT:
            return cmp_dict_total(a, b);
        case IDM_VAL_RECORD:
            return cmp_record_total(a, b);
        case IDM_VAL_PID:
        case IDM_VAL_REF:
        case IDM_VAL_PORT:
            return (idm_value_id(a) > idm_value_id(b)) - (idm_value_id(a) < idm_value_id(b));
        case IDM_VAL_SYNTAX:
        case IDM_VAL_CELL:
        case IDM_VAL_CLOSURE:
        case IDM_VAL_REGEX:
        case IDM_VAL_REGEX_RESULT:
            return cmp_ptr(idm_boxed_object(a), idm_boxed_object(b));
    }
    return 0;
}

static bool prim_term_compare(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    int r = cmp_value_total(args[0], args[1]);
    *out = idm_atom(rt, r < 0 ? "lt" : (r > 0 ? "gt" : "eq"));
    return true;
}

static bool prim_ok(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_value_ok(args[0]));
    return true;
}

static IdmSymbol *type_name_symbol(IdmRuntime *rt, IdmValue value) {
    const char *text = NULL;
    if (idm_value_tag(value) == IDM_VAL_ATOM) return idm_value_symbol(value);
    if (idm_value_tag(value) == IDM_VAL_WORD) text = idm_symbol_text(idm_value_symbol(value));
    else if (idm_value_tag(value) == IDM_VAL_STRING) text = idm_string_bytes(value);
    return text && *text ? idm_intern(&rt->intern, IDM_SYMBOL_ATOM, text) : NULL;
}

static bool prim_is_a(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSymbol *type = type_name_symbol(rt, args[1]);
    if (!type) return idm_primitive_type_error(rt, err, "is-a?", args[1], "a non-empty type name atom, word, or string");
    bool result = idm_value_matches_type_symbol(args[0], type);
    *out = idm_bool(rt, result);
    return true;
}

static IdmBuiltinType type_pred_type_kind(IdmPrimitive prim) {
    switch (prim) {
        case IDM_PRIM_NIL_P: return IDM_BUILTIN_TYPE_NIL;
        case IDM_PRIM_ATOM_P: return IDM_BUILTIN_TYPE_ATOM;
        case IDM_PRIM_WORD_P: return IDM_BUILTIN_TYPE_WORD;
        case IDM_PRIM_INT_P: return IDM_BUILTIN_TYPE_INT;
        case IDM_PRIM_FLOAT_P: return IDM_BUILTIN_TYPE_FLOAT;
        case IDM_PRIM_STRING_P: return IDM_BUILTIN_TYPE_STRING;
        case IDM_PRIM_PAIR_P: return IDM_BUILTIN_TYPE_PAIR;
        case IDM_PRIM_EMPTY_LIST_P: return IDM_BUILTIN_TYPE_EMPTY_LIST;
        case IDM_PRIM_LIST_P: return IDM_BUILTIN_TYPE_LIST;
        case IDM_PRIM_TUPLE_P: return IDM_BUILTIN_TYPE_TUPLE;
        case IDM_PRIM_VECTOR_P: return IDM_BUILTIN_TYPE_VECTOR;
        case IDM_PRIM_DICT_P: return IDM_BUILTIN_TYPE_DICT;
        case IDM_PRIM_SYNTAX_P: return IDM_BUILTIN_TYPE_SYNTAX;
        case IDM_PRIM_CELL_P: return IDM_BUILTIN_TYPE_CELL;
        case IDM_PRIM_CLOSURE_P: return IDM_BUILTIN_TYPE_CLOSURE;
        case IDM_PRIM_PID_P: return IDM_BUILTIN_TYPE_PID;
        case IDM_PRIM_REF_P: return IDM_BUILTIN_TYPE_REF;
        case IDM_PRIM_PORT_P: return IDM_BUILTIN_TYPE_PORT;
        case IDM_PRIM_REGEX_P: return IDM_BUILTIN_TYPE_REGEX;
        case IDM_PRIM_REGEX_RESULT_P: return IDM_BUILTIN_TYPE_REGEX_RESULT;
        default: return IDM_BUILTIN_TYPE_NONE;
    }
}

static bool prim_type_pred(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    IdmBuiltinType type = type_pred_type_kind(prim);
    bool result = idm_value_matches_builtin_type(args[0], type);
    *out = idm_bool(rt, result);
    return true;
}

static bool prim_raise_fallback(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    (void)args;
    (void)out;
    return idm_error_set(err, idm_span_unknown(NULL), "primitive 'raise' must be executed by the VM");
}

static bool prim_error_message(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    idm_error_describe(rt, args[0], &buf);
    *out = idm_string(rt, buf.data ? buf.data : "", err);
    idm_buf_destroy(&buf);
    return idm_value_tag(*out) == IDM_VAL_STRING;
}

static bool prim_make_error(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_error_value(rt, args[0]);
    return idm_value_tag(*out) == IDM_VAL_TUPLE;
}

static bool prim_syntax_kind(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = idm_atom(rt, idm_syn_kind_name(syn->kind));
    return true;
}

static bool wrap_owned_syntax(IdmRuntime *rt, IdmSyntax *syn, IdmValue *out, IdmError *err) {
    if (!syn) return false;
    *out = idm_syntax_value(rt, syn, err);
    idm_syn_free(syn);
    return !(err && err->present);
}

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

static bool prim_syntax_property(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &key, NULL, "syntax-property", err)) return false;
    const char *value = idm_syn_property_get(syn, key);
    if (!value) { *out = idm_nil(); return true; }
    *out = idm_string(rt, value, err);
    return !(err && err->present);
}

static bool prim_syntax_set_property(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *key = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &key, NULL, "syntax-set-property", err)) return false;
    const char *value = NULL;
    if (!idm_primitive_require_string_arg(rt, args[2], &value, NULL, "syntax-set-property", err)) return false;
    IdmSyntax *copy = idm_syn_clone(syn);
    if (!copy) return idm_error_oom(err, syn->span);
    if (!idm_syn_property_set(copy, key, value)) { idm_syn_free(copy); return idm_error_oom(err, syn->span); }
    return wrap_owned_syntax(rt, copy, out, err);
}

static bool prim_syntax_origin(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    IdmValue result = idm_empty_list();
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
    return idm_bool(rt, v);
}

static bool prim_syntax_list_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_LIST);
    return true;
}

static bool prim_syntax_length(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_syntax_nth(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    int64_t idx = 0;
    if (!value_to_i64(args[1], &idx)) return idm_error_set(err, syn->span, "syntax-nth index must be int");
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

static bool prim_syntax_slice(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    int64_t start_i = 0;
    int64_t end_i = 0;
    if (!value_to_i64(args[1], &start_i) || !value_to_i64(args[2], &end_i)) return idm_error_set(err, syn->span, "syntax-slice indexes must be ints");
    if (start_i < 0 || end_i < start_i) return idm_error_set(err, syn->span, "invalid syntax-slice range");
    if (syn->kind != IDM_SYN_LIST && syn->kind != IDM_SYN_VECTOR && syn->kind != IDM_SYN_TUPLE && syn->kind != IDM_SYN_DICT) return idm_error_set(err, syn->span, "syntax-slice expects sequence syntax");
    size_t start = (size_t)start_i;
    size_t end = (size_t)end_i;
    if (end > syn->as.seq.count) return idm_error_set(err, syn->span, "syntax-slice range out of bounds");
    IdmValue result = idm_empty_list();
    for (size_t i = end; i > start; i--) {
        IdmValue item = idm_syntax_value(rt, syn->as.seq.items[i - 1u], err);
        if (err && err->present) return false;
        result = idm_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_syntax_word_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_WORD);
    return true;
}

static bool prim_syntax_word_text(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_WORD) return idm_error_set(err, syn->span, "syntax-word-text expects word syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_atom_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_ATOM);
    return true;
}

static bool prim_syntax_atom_text(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_ATOM) return idm_error_set(err, syn->span, "syntax-atom-text expects atom syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_syntax_adjacent_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->token_adjacent_previous);
    return true;
}

static bool prim_syntax_string_text(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_STRING) return idm_error_set(err, syn->span, "syntax-string-text expects string syntax");
    *out = idm_string(rt, syn->as.text, err);
    return !(err && err->present);
}

static bool prim_internal_register_macro(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!rt->register_macro) return idm_error_set(err, idm_span_unknown(NULL), "internal macro registration requires an active expansion");
    const IdmSyntax *name = idm_syntax_get(args[0], err);
    if (!name) return false;
    if (!idm_is_closure(args[1])) return idm_error_set(err, idm_span_unknown(NULL), "internal macro registration transformer must be a function value");
    if (!rt->register_macro(rt->register_macro_user, rt, name, args[1], err)) return false;
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_inspect(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_expand_check(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *source = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &source, NULL, "expand-check", err)) return false;
    IdmError inner;
    idm_error_init(&inner);
    IdmCore *core = NULL;
    bool expanded = idm_expand_source_string(rt, "<expand-check>", source, &core, &inner);
    bool compiled = false;
    if (expanded) {
        IdmBytecodeModule module;
        idm_bc_init(&module);
        uint32_t main_fn = 0;
        compiled = idm_core_compile_main(rt, core, &module, &main_fn, &inner);
        idm_bc_destroy(&module);
    }
    idm_core_free(core);
    if (expanded && compiled) {
        *out = idm_atom(rt, "ok");
        idm_error_clear(&inner);
        return true;
    }
    IdmValue reason = idm_nil();
    (void)idm_error_take_reason(&inner, &reason);
    IdmValue detail[6];
    detail[0] = idm_atom(rt, "expand");
    detail[1] = idm_string(rt, inner.message ? inner.message : "compile failed", err);
    detail[2] = idm_int((int64_t)inner.span.line);
    detail[3] = idm_int((int64_t)inner.span.column);
    detail[4] = idm_string(rt, inner.span.file ? inner.span.file : "", err);
    detail[5] = reason;
    idm_error_clear(&inner);
    if (err && err->present) return false;
    IdmValue error_detail = idm_tuple(rt, detail, 6u, err);
    if (err && err->present) return false;
    *out = idm_error_value(rt, error_detail);
    return !(err && err->present);
}

bool idm_primitive_require_string_arg(IdmRuntime *rt, IdmValue v, const char **out_s, size_t *out_len, const char *what, IdmError *err) {
    if (idm_value_tag(v) != IDM_VAL_STRING) return idm_primitive_type_error(rt, err, what, v, "a string");
    *out_s = idm_string_bytes(v);
    if (out_len) *out_len = idm_string_length(v);
    return true;
}

static int64_t clamp_index(int64_t i, size_t len) {
    if (i < 0) i = 0;
    if ((size_t)i > len) i = (int64_t)len;
    return i;
}

static bool prim_str_len(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    if (!idm_primitive_require_string_arg(rt, args[0], &s, &len, "len", err)) return false;
    *out = idm_int((int64_t)len);
    return true;
}

static bool prim_str_slice(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *s; size_t len;
    if (!idm_primitive_require_string_arg(rt, args[0], &s, &len, "slice", err)) return false;
    int64_t a = 0;
    int64_t b = 0;
    if (!value_to_i64(args[1], &a)) return idm_primitive_type_error(rt, err, "slice", args[1], "integer bounds");
    if (!value_to_i64(args[2], &b)) return idm_primitive_type_error(rt, err, "slice", args[2], "integer bounds");
    if (a < 0 || b < a || (uint64_t)b > len) {
        idm_error_set(err, idm_span_unknown(NULL), "slice range %lld..%lld out of bounds for length %zu", (long long)a, (long long)b, len);
        return idm_error_reason(rt, err, "slice-out-of-range", 3, args[1], args[2], idm_int((int64_t)len));
    }
    *out = idm_string_n(rt, s + a, (size_t)(b - a), err);
    return !(err && err->present);
}

static bool prim_str_find(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    const char *needle; size_t nlen;
    if (!idm_primitive_require_string_arg(rt, args[0], &s, &len, "find", err)) return false;
    if (!idm_primitive_require_string_arg(rt, args[1], &needle, &nlen, "find", err)) return false;
    int64_t start = 0;
    if (!value_to_i64(args[2], &start)) return idm_primitive_type_error(rt, err, "find", args[2], "an integer start");
    size_t from = (size_t)clamp_index(start, len);
    if (nlen == 0) { *out = idm_int((int64_t)from); return true; }
    for (size_t i = from; i + nlen <= len; i++) {
        if (memcmp(s + i, needle, nlen) == 0) {
            *out = idm_int((int64_t)i);
            return true;
        }
    }
    *out = idm_nil();
    return true;
}

static bool prim_str_byte(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    if (!idm_primitive_require_string_arg(rt, args[0], &s, &len, "byte", err)) return false;
    int64_t i = 0;
    if (!value_to_i64(args[1], &i)) return idm_primitive_type_error(rt, err, "byte", args[1], "an integer index");
    if (i < 0 || (size_t)i >= len) { *out = idm_nil(); return true; }
    *out = idm_int((int64_t)(unsigned char)s[i]);
    return true;
}

static bool prim_byte_str(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t byte = 0;
    if (!value_to_i64(args[0], &byte) || byte < 0 || byte > 255) return idm_primitive_type_error(rt, err, "byte-str", args[0], "an integer 0..255");
    char c = (char)byte;
    *out = idm_string_n(rt, &c, 1u, err);
    return !(err && err->present);
}

static bool prim_regex_compile(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_compile_value(rt, args[0], args[1], out, err);
}

static bool prim_regex_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_is_regex(args[0]));
    return true;
}

static bool prim_regex_source(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmRegex *rx = idm_regex_value_get(args[0], err);
    if (!rx) return false;
    size_t len = 0;
    const char *source = idm_regex_source(rx, &len);
    *out = idm_string_n(rt, source, len, err);
    return !(err && err->present);
}

static bool prim_regex_options(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_options_value(rt, args[0], out, err);
}

static bool prim_regex_group_count(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmRegex *rx = idm_regex_value_get(args[0], err);
    if (!rx) return false;
    *out = idm_int((int64_t)idm_regex_group_count(rx));
    return true;
}

static bool prim_regex_group_names(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_group_names_value(rt, args[0], out, err);
}

static bool prim_regex_result_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_is_regex_result(args[0]));
    return true;
}

static bool regex_offset_arg(IdmRuntime *rt, const char *name, IdmValue value, size_t *out, IdmError *err) {
    int64_t offset = 0;
    if (!value_to_i64(value, &offset) || offset < 0) return idm_primitive_type_error(rt, err, name, value, "a non-negative integer offset");
    *out = (size_t)offset;
    return true;
}

static bool prim_regex_scan_at(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t offset = 0;
    if (!regex_offset_arg(rt, "raw-scan-at", args[2], &offset, err)) return false;
    return idm_regex_scan_at(rt, args[0], args[1], offset, out, err);
}

static bool prim_regex_scan_from(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t offset = 0;
    if (!regex_offset_arg(rt, "raw-scan-from", args[2], &offset, err)) return false;
    return idm_regex_scan_from(rt, args[0], args[1], offset, out, err);
}

static bool prim_regex_scan_full(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_scan_full(rt, args[0], args[1], out, err);
}

static bool prim_regex_test(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_test(rt, args[0], args[1], out, err);
}

static bool prim_regex_scan_all(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_scan_all(rt, args[0], args[1], out, err);
}

static bool prim_regex_replace(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err, bool all) {
    return idm_regex_replace(rt, args[0], args[1], args[2], all, out, err);
}

static IdmValue errno_reason(IdmRuntime *rt) {
    switch (errno) {
        case ENOENT: return idm_atom(rt, "enoent");
        case EACCES: return idm_atom(rt, "eacces");
        case EISDIR: return idm_atom(rt, "eisdir");
        case ENOTDIR: return idm_atom(rt, "enotdir");
        case EEXIST: return idm_atom(rt, "eexist");
        default: return idm_atom(rt, "eio");
    }
}

static bool result_ok(IdmRuntime *rt, IdmValue payload, IdmValue *out, IdmError *err) {
    IdmValue items[2] = { idm_atom(rt, "ok"), payload };
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}

static bool result_error(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    *out = idm_error_value(rt, errno_reason(rt));
    return !(err && err->present);
}

static const char *resolve_cwd(const char *path, char *buf, size_t cap) {
    const char *base = idm_exec_cwd(idm_current_exec());
    if (!base || !*path || path[0] == '/') return path;
    int n = snprintf(buf, cap, "%s/%s", base, path);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return NULL; }
    return buf;
}

static const char *resolve_cwd_record(IdmRuntime *rt, const char *path, char *buf, size_t cap) {
    const char *resolved = resolve_cwd(path, buf, cap);
    if (resolved) idm_phase_io_record(rt, resolved);
    return resolved;
}

static bool prim_file_read(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "read", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd_record(rt, path, pb, sizeof(pb)))) return result_error(rt, out, err);
    char *data = NULL;
    size_t len = 0;
    IdmError inner;
    idm_error_init(&inner);
    if (!idm_read_file(path, &data, &len, &inner)) {
        idm_error_clear(&inner);
        return result_error(rt, out, err);
    }
    IdmValue s = idm_string_n(rt, data, len, err);
    free(data);
    if (err && err->present) return false;
    return result_ok(rt, s, out, err);
}

static bool prim_file_write(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "write", err)) return false;
    if (!idm_primitive_require_string_arg(rt, args[1], &data, &dlen, "write", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    FILE *f = fopen(path, "wb");
    if (!f) return result_error(rt, out, err);
    bool ok = fwrite(data, 1u, dlen, f) == dlen;
    ok = (fclose(f) == 0) && ok;
    if (!ok) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool file_port_mode(IdmValue value, const char **out_mode, bool *out_readable, bool *out_writable) {
    if (idm_value_tag(value) != IDM_VAL_ATOM) return false;
    const char *text = idm_symbol_text(idm_value_symbol(value));
    if (strcmp(text, "read") == 0) {
        *out_mode = "rb";
        *out_readable = true;
        *out_writable = false;
        return true;
    }
    if (strcmp(text, "write") == 0) {
        *out_mode = "wb";
        *out_readable = false;
        *out_writable = true;
        return true;
    }
    if (strcmp(text, "append") == 0) {
        *out_mode = "ab";
        *out_readable = false;
        *out_writable = true;
        return true;
    }
    if (strcmp(text, "read-write") == 0) {
        *out_mode = "r+b";
        *out_readable = true;
        *out_writable = true;
        return true;
    }
    return false;
}

static bool prim_file_open(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    const char *mode = NULL;
    bool readable = false;
    bool writable = false;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "open", err)) return false;
    if (!file_port_mode(args[1], &mode, &readable, &writable)) return idm_primitive_type_error(rt, err, "open", args[1], ":read, :write, :append, or :read-write");
    char pb[PATH_MAX];
    const char *resolved = readable ? resolve_cwd_record(rt, path, pb, sizeof(pb)) : resolve_cwd(path, pb, sizeof(pb));
    if (!resolved) return result_error(rt, out, err);
    IdmPort *port = idm_port_open_file(resolved, mode, readable, writable, err);
    if (!port) {
        if (err && err->present) return false;
        return result_error(rt, out, err);
    }
    IdmScheduler *sched = job_sched("open", err);
    if (!sched) {
        idm_port_free(port);
        return false;
    }
    IdmValue port_value = idm_nil();
    if (!idm_sched_register_port(sched, port, &port_value, err)) {
        idm_port_free(port);
        return false;
    }
    return result_ok(rt, port_value, out, err);
}

static bool prim_file_exists(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "exists?", err)) return false;
    char pb[PATH_MAX];
    path = resolve_cwd_record(rt, path, pb, sizeof(pb));
    *out = idm_bool(rt, path && access(path, F_OK) == 0);
    return true;
}

static bool prim_file_stat(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "stat", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd_record(rt, path, pb, sizeof(pb)))) return result_error(rt, out, err);
    struct stat st;
    if (stat(path, &st) != 0) return result_error(rt, out, err);
    IdmValue items[4];
    items[0] = idm_atom(rt, "stat");
    items[1] = S_ISDIR(st.st_mode) ? idm_atom(rt, "dir") : (S_ISREG(st.st_mode) ? idm_atom(rt, "file") : idm_atom(rt, "other"));
    items[2] = idm_int((int64_t)st.st_size);
    items[3] = idm_int((int64_t)st.st_mtime * 1000);
    IdmValue stat_tuple = idm_tuple(rt, items, 4u, err);
    if (err && err->present) return false;
    return result_ok(rt, stat_tuple, out, err);
}

static bool prim_file_list(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "list", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    DIR *dir = opendir(path);
    if (!dir) return result_error(rt, out, err);
    IdmValue acc = idm_empty_list();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        IdmValue name = idm_string(rt, entry->d_name, err);
        if (err && err->present) { closedir(dir); return false; }
        acc = idm_cons(rt, name, acc, err);
        if (err && err->present) { closedir(dir); return false; }
    }
    closedir(dir);
    return result_ok(rt, acc, out, err);
}

static bool prim_file_remove(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "remove", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    if (remove(path) != 0) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_args(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)args;
    IdmValue acc = idm_empty_list();
    for (size_t i = rt->cli_arg_count; i > 0; i--) {
        IdmValue s = idm_string(rt, rt->cli_args[i - 1u], err);
        if (err && err->present) return false;
        acc = idm_cons(rt, s, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool prim_time_ms(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt; (void)args; (void)err;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *out = idm_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return true;
}

static bool prim_time_ns(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt; (void)args; (void)err;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *out = idm_int((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
    return true;
}

static bool prim_random(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t bound = 0;
    if (!value_to_i64(args[0], &bound) || bound <= 0) return idm_primitive_type_error(rt, err, "random", args[0], "a positive integer bound");
    *out = idm_int((int64_t)(random() % bound));
    return true;
}

static bool require_dict(IdmRuntime *rt, const char *name, IdmValue v, IdmError *err) {
    if (idm_value_tag(v) == IDM_VAL_DICT) return true;
    return idm_primitive_type_error(rt, err, name, v, "a dict");
}

static bool prim_dict_get(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-get", args[0], err)) return false;
    IdmValue found;
    *out = idm_dict_get(args[0], args[1], &found) ? found : args[2];
    return true;
}

static bool prim_dict_put(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-put", args[0], err)) return false;
    *out = idm_dict_put(rt, args[0], args[1], args[2], err);
    return !(err && err->present);
}

static bool prim_dict_del(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-del", args[0], err)) return false;
    *out = idm_dict_del(rt, args[0], args[1], err);
    return !(err && err->present);
}

static bool prim_dict_keys(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-keys", args[0], err)) return false;
    IdmValue acc = idm_empty_list();
    IdmDictIter it;
    IdmValue k, v;
    idm_dict_iter_init(args[0], &it);
    while (idm_dict_iter_next(&it, &k, &v)) {
        acc = idm_cons(rt, k, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool prim_dict_vals(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-vals", args[0], err)) return false;
    IdmValue acc = idm_empty_list();
    IdmDictIter it;
    IdmValue k, v;
    idm_dict_iter_init(args[0], &it);
    while (idm_dict_iter_next(&it, &k, &v)) {
        acc = idm_cons(rt, v, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool prim_dict_has(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-has?", args[0], err)) return false;
    IdmValue found;
    *out = idm_bool(rt, idm_dict_get(args[0], args[1], &found));
    return true;
}

static bool prim_dict_size(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-size", args[0], err)) return false;
    *out = idm_int((int64_t)idm_dict_count(args[0]));
    return true;
}

static bool prim_str_contains(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    size_t hay_len = 0;
    const char *hay = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &hay, &hay_len, "contains?", err)) return false;
    size_t needle_len = 0;
    const char *needle = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &needle, &needle_len, "contains?", err)) return false;
    bool found = needle_len == 0;
    for (size_t i = 0; !found && needle_len <= hay_len && i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) found = true;
    }
    *out = truth(rt, found);
    return true;
}

static bool prim_syntax_int_pred(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    *out = truth(rt, syn->kind == IDM_SYN_INT || syn->kind == IDM_SYN_BIGINT);
    return true;
}

static bool prim_syntax_int_value(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind == IDM_SYN_INT) { *out = idm_int_promote(rt, syn->as.integer, err); return !(err && err->present); }
    if (syn->kind == IDM_SYN_BIGINT) {
        bool ok = false;
        *out = idm_int_from_decimal(rt, syn->as.text, strlen(syn->as.text), &ok, err);
        if (err && err->present) return false;
        if (!ok) return idm_error_set(err, syn->span, "invalid integer literal");
        return true;
    }
    return idm_error_set(err, syn->span, "syntax-int-value expects int syntax");
}

static bool prim_syntax_float_value(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    if (syn->kind != IDM_SYN_FLOAT) return idm_error_set(err, syn->span, "syntax-float-value expects float syntax");
    *out = idm_float(rt, syn->as.real, err);
    return !(err && err->present);
}

static IdmSyntax *make_word_syntax_from(IdmSyntax *ctx, const char *text, IdmError *err) {
    IdmSyntax *out = idm_syn_word(text, ctx ? ctx->span : idm_span_unknown(NULL));
    if (!out) { idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    if (!copy_scopes_from(out, ctx)) { idm_syn_free(out); idm_error_oom(err, idm_span_unknown(NULL)); return NULL; }
    return out;
}

static bool prim_syntax_error(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    (void)out;
    IdmSyntax *syn = require_syntax(args[0], err);
    if (!syn) return false;
    const char *msg = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &msg, NULL, "syntax-error", err)) return false;
    return idm_error_set(err, syn->span, "%s", msg);
}

static bool prim_local_expand(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_bound_identifier_eq(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_free_identifier_eq(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_bind_bang(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmSyntax *ctx = require_syntax(args[0], err);
    if (!ctx) return false;
    const char *text = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &text, NULL, "bind!", err)) return false;
    IdmSyntax *syn = make_word_syntax_from(ctx, text, err);
    if (!syn) return false;
    if (rt->macro_intro_active && !idm_syn_scope_add(syn, 0, rt->macro_intro_scope)) {
        idm_syn_free(syn);
        return idm_error_oom(err, ctx->span);
    }
    return wrap_owned_syntax(rt, syn, out, err);
}

static bool prim_abs(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) == IDM_VAL_FLOAT) { *out = float_result(rt, fabs(idm_float_value(args[0])), err); return !(err && err->present); }
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, "abs", args[0], "a number");
    if (idm_int_compare(args[0], idm_int(0)) < 0) return idm_int_neg(rt, args[0], out, err);
    *out = args[0];
    return true;
}

static bool prim_float_unary(IdmRuntime *rt, const char *name, double (*fn)(double), const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_is_int(args[0])) { *out = args[0]; return true; }
    if (idm_value_tag(args[0]) != IDM_VAL_FLOAT) return idm_primitive_type_error(rt, err, name, args[0], "a number");
    *out = float_result(rt, fn(idm_float_value(args[0])), err);
    return !(err && err->present);
}

static bool prim_sqrt(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    double x = 0.0;
    if (!num_as_double(args[0], &x)) return idm_primitive_type_error(rt, err, "sqrt", args[0], "a number");
    if (x < 0.0) {
        idm_error_set(err, idm_span_unknown(NULL), "sqrt of a negative number");
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "sqrt"), args[0]);
    }
    *out = float_result(rt, sqrt(x), err);
    return !(err && err->present);
}

static bool prim_floor_divmod(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    if (idm_value_is_int(args[0]) && idm_value_is_int(args[1])) {
        IdmValue q = idm_nil(), r = idm_nil();
        if (!int_floor_divmod(rt, name, args[0], args[1], &q, &r, err)) return false;
        *out = prim == IDM_PRIM_FLOOR_DIV ? q : r;
        return true;
    }
    double x = 0.0, y = 0.0;
    if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) {
        idm_error_set(err, idm_span_unknown(NULL), "%s expects numeric operands", name);
        return idm_error_reason(rt, err, "type-error", 3, idm_atom(rt, name), args[0], args[1]);
    }
    if (y == 0.0) return div_zero_error(rt, name, err);
    if (prim == IDM_PRIM_FLOOR_DIV) { *out = float_result(rt, floor(x / y), err); return !(err && err->present); }
    double r = fmod(x, y);
    if (r != 0.0 && ((r < 0.0) != (y < 0.0))) r += y;
    *out = float_result(rt, r, err);
    return !(err && err->present);
}

static bool floor_divmod_values(IdmRuntime *rt, const char *name, IdmValue a0, IdmValue a1, IdmValue *q_out, IdmValue *r_out, IdmError *err) {
    if (idm_value_is_int(a0) && idm_value_is_int(a1)) return int_floor_divmod(rt, name, a0, a1, q_out, r_out, err);
    double x = 0.0, y = 0.0;
    if (!num_as_double(a0, &x) || !num_as_double(a1, &y)) {
        idm_error_set(err, idm_span_unknown(NULL), "%s expects numeric operands", name);
        return idm_error_reason(rt, err, "type-error", 3, idm_atom(rt, name), a0, a1);
    }
    if (y == 0.0) return div_zero_error(rt, name, err);
    double q = floor(x / y);
    double r = fmod(x, y);
    if (r != 0.0 && ((r < 0.0) != (y < 0.0))) r += y;
    *q_out = float_result(rt, q, err);
    *r_out = float_result(rt, r, err);
    return !(err && err->present);
}

static bool prim_divmod(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue items[2];
    if (!floor_divmod_values(rt, "divmod", args[0], args[1], &items[0], &items[1], err)) return false;
    *out = idm_tuple(rt, items, 2u, err);
    return !(err && err->present);
}

static bool prim_math_unary(IdmRuntime *rt, const char *name, double (*fn)(double), const IdmValue *args, IdmValue *out, IdmError *err) {
    double x = 0.0;
    if (!num_as_double(args[0], &x)) return idm_primitive_type_error(rt, err, name, args[0], "a number");
    *out = float_result(rt, fn(x), err);
    return !(err && err->present);
}

static bool prim_math_binary(IdmRuntime *rt, const char *name, double (*fn)(double, double), const IdmValue *args, IdmValue *out, IdmError *err) {
    double x = 0.0, y = 0.0;
    if (!num_as_double(args[0], &x) || !num_as_double(args[1], &y)) return idm_primitive_type_error(rt, err, name, args[0], "a number");
    *out = float_result(rt, fn(x, y), err);
    return !(err && err->present);
}

static bool atom_named(IdmValue value, const char *name) {
    return idm_value_tag(value) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(value)), name) == 0;
}

static bool prim_number_classify(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    if (atom_named(args[0], "nan")) {
        *out = idm_bool(rt, prim == IDM_PRIM_NAN_P);
        return true;
    }
    if (atom_named(args[0], "inf")) {
        *out = idm_bool(rt, prim == IDM_PRIM_INFINITE_P);
        return true;
    }
    double x = 0.0;
    if (!num_as_double(args[0], &x)) return idm_primitive_type_error(rt, err, name, args[0], "a number");
    bool result = false;
    switch (prim) {
        case IDM_PRIM_NAN_P: result = isnan(x); break;
        case IDM_PRIM_FINITE_P: result = isfinite(x); break;
        case IDM_PRIM_INFINITE_P: result = isinf(x); break;
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid numeric classifier");
    }
    *out = idm_bool(rt, result);
    return true;
}

static bool require_nonnegative_int_arg(IdmRuntime *rt, IdmError *err, const char *name, IdmValue value, int64_t *out) {
    if (!idm_value_is_int(value) || !idm_int_to_i64(value, out) || *out < 0) return idm_primitive_type_error(rt, err, name, value, "a non-negative integer");
    return true;
}

static bool prim_bit_binary(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, name, args[0], "an integer");
    if (!idm_value_is_int(args[1])) return idm_primitive_type_error(rt, err, name, args[1], "an integer");
    switch (prim) {
        case IDM_PRIM_BIT_AND: return idm_int_bit_and(rt, args[0], args[1], out, err);
        case IDM_PRIM_BIT_OR: return idm_int_bit_or(rt, args[0], args[1], out, err);
        case IDM_PRIM_BIT_XOR: return idm_int_bit_xor(rt, args[0], args[1], out, err);
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid bit primitive");
    }
}

static bool prim_bit_not(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, "bit-not", args[0], "an integer");
    return idm_int_bit_not(rt, args[0], out, err);
}

static bool prim_shift_left(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t count = 0;
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, "shift-left", args[0], "an integer");
    if (!require_nonnegative_int_arg(rt, err, "shift-left", args[1], &count)) return false;
    *out = idm_int_shl(rt, args[0], count, err);
    return !(err && err->present);
}

static bool prim_shift_right(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    int64_t count = 0;
    if (!idm_value_is_int(args[0])) return idm_primitive_type_error(rt, err, "shift-right", args[0], "an integer");
    if (!require_nonnegative_int_arg(rt, err, "shift-right", args[1], &count)) return false;
    if (idm_value_tag(args[0]) == IDM_VAL_INT) {
        int64_t v = idm_int_value(args[0]);
        *out = idm_int(count >= 63 ? (v < 0 ? -1 : 0) : (v >> count));
        return true;
    }
    IdmValue divisor = idm_int_shl(rt, idm_int(1), count, err);
    if (err && err->present) return false;
    IdmValue q = idm_nil(), r = idm_nil();
    if (!int_floor_divmod(rt, "shift-right", args[0], divisor, &q, &r, err)) return false;
    *out = q;
    return true;
}

static bool prim_bit_count(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_value_is_int(args[0]) || idm_int_compare(args[0], idm_int(0)) < 0) return idm_primitive_type_error(rt, err, "bit-count", args[0], "a non-negative integer");
    return idm_int_bit_count_nonnegative(rt, args[0], out, err);
}

static bool prim_bit_length(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_value_is_int(args[0]) || idm_int_compare(args[0], idm_int(0)) < 0) return idm_primitive_type_error(rt, err, "bit-length", args[0], "a non-negative integer");
    return idm_int_bit_length_nonnegative(rt, args[0], out, err);
}

static bool prim_to_int(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_is_int(args[0])) { *out = args[0]; return true; }
    if (idm_value_tag(args[0]) != IDM_VAL_FLOAT) return idm_primitive_type_error(rt, err, "to-int", args[0], "a number");
    double x = idm_float_value(args[0]);
    if (!isfinite(x)) return idm_primitive_type_error(rt, err, "to-int", args[0], "a finite number");
    double t = trunc(x);
    if (t >= -9223372036854775808.0 && t < 9223372036854775808.0) return int_result(rt, (int64_t)t, "to-int", out, err);
    int e = 0;
    double m = frexp(t, &e);
    int64_t mant = (int64_t)ldexp(m, 53);
    IdmValue base = idm_int_promote(rt, mant, err);
    if (err && err->present) return false;
    *out = idm_int_shl(rt, base, (int64_t)e - 53, err);
    return !(err && err->present);
}

static bool prim_to_float(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) == IDM_VAL_FLOAT) { *out = args[0]; return true; }
    if (idm_value_is_int(args[0])) { *out = idm_float(rt, idm_int_to_double(args[0]), err); return !(err && err->present); }
    return idm_primitive_type_error(rt, err, "to-float", args[0], "a number");
}

static bool parse_result_error(IdmRuntime *rt, const char *what, IdmValue input, IdmValue *out, IdmError *err) {
    IdmValue d[3] = { idm_atom(rt, "parse"), idm_atom(rt, what), input };
    IdmValue detail = idm_tuple(rt, d, 3u, err);
    if (err && err->present) return false;
    *out = idm_error_value(rt, detail);
    return !(err && err->present);
}

static bool parse_prelude(IdmRuntime *rt, const char *name, IdmValue arg, char **out_buf, IdmError *err) {
    const char *s = NULL;
    size_t len = 0;
    if (!idm_primitive_require_string_arg(rt, arg, &s, &len, name, err)) return false;
    if (len == 0 || isspace((unsigned char)s[0])) { *out_buf = NULL; return true; }
    char *buf = idm_strndup(s, len);
    if (!buf) return idm_error_oom(err, idm_span_unknown(NULL));
    *out_buf = buf;
    return true;
}

static bool prim_parse_int(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    char *buf = NULL;
    if (!parse_prelude(rt, "parse-int", args[0], &buf, err)) return false;
    if (!buf) return parse_result_error(rt, "int", args[0], out, err);
    bool parsed = false;
    IdmValue value = idm_int_from_decimal(rt, buf, idm_string_length(args[0]), &parsed, err);
    free(buf);
    if (err && err->present) return false;
    if (!parsed) return parse_result_error(rt, "int", args[0], out, err);
    return result_ok(rt, value, out, err);
}

static bool prim_parse_float(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    char *buf = NULL;
    if (!parse_prelude(rt, "parse-float", args[0], &buf, err)) return false;
    if (!buf) return parse_result_error(rt, "float", args[0], out, err);
    char *end = NULL;
    double v = strtod(buf, &end);
    bool ok = end == buf + idm_string_length(args[0]);
    free(buf);
    if (!ok) return parse_result_error(rt, "float", args[0], out, err);
    return result_ok(rt, float_result(rt, v, err), out, err);
}

static bool prim_file_mkdir(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "mkdir", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    if (mkdir(path, 0777) != 0) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_file_append(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!idm_primitive_require_string_arg(rt, args[0], &path, &plen, "append", err)) return false;
    if (!idm_primitive_require_string_arg(rt, args[1], &data, &dlen, "append", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    FILE *f = fopen(path, "ab");
    if (!f) return result_error(rt, out, err);
    bool ok = fwrite(data, 1u, dlen, f) == dlen;
    ok = (fclose(f) == 0) && ok;
    if (!ok) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_ord_str(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    char buf[4];
    int64_t cp = 0;
    size_t n = value_to_i64(args[0], &cp) ? utf8_encode(cp, buf) : 0u;
    if (n == 0u) return idm_primitive_type_error(rt, err, "rune-str", args[0], "a Unicode codepoint");
    *out = idm_string_n(rt, buf, n, err);
    return !(err && err->present);
}

static bool prim_from_runes(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (; idm_is_pair(v); v = idm_cdr(v, err)) {
        IdmValue item = idm_car(v, err);
        if (err && err->present) { idm_buf_destroy(&buf); return false; }
        char enc[4];
        int64_t cp = 0;
        size_t n = value_to_i64(item, &cp) ? utf8_encode(cp, enc) : 0u;
        if (n == 0u) { idm_buf_destroy(&buf); return idm_primitive_type_error(rt, err, "from-runes", item, "a Unicode codepoint"); }
        if (!idm_buf_append_n(&buf, enc, n)) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (!idm_is_empty_list(v)) { idm_buf_destroy(&buf); return idm_primitive_type_error(rt, err, "from-runes", v, "a list of codepoints"); }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

static bool require_session(IdmRuntime *rt, const char *name, IdmError *err) {
    if (rt->repl) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "%s requires a session context", name);
}

static bool prim_repl_compile(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-compile", err)) return false;
    size_t len = 0;
    const char *source = NULL;
    if (!idm_primitive_require_string_arg(rt, args[0], &source, &len, "repl-compile", err)) return false;
    IdmValue thunk = idm_nil();
    uint64_t token = 0;
    IdmReplStatus status = idm_repl_compile(rt->repl, source, &thunk, &token, err);
    if (status == IDM_REPL_INCOMPLETE) {
        *out = idm_atom(rt, "incomplete");
        return true;
    }
    if (status == IDM_REPL_ERROR) {
        IdmBuffer buf;
        idm_buf_init(&buf);
        bool rendered = idm_error_render(err, &buf);
        idm_error_clear(err);
        IdmValue detail = idm_string_n(rt, rendered && buf.data ? buf.data : "", buf.len, err);
        idm_buf_destroy(&buf);
        if (err && err->present) return false;
        *out = idm_error_value(rt, detail);
        return !(err && err->present);
    }
    IdmValue items[3];
    items[0] = idm_atom(rt, "ok");
    items[1] = thunk;
    items[2] = idm_int((int64_t)token);
    *out = idm_tuple(rt, items, 3u, err);
    return !(err && err->present);
}

static bool prim_repl_abort(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-abort", err)) return false;
    int64_t token = 0;
    if (!value_to_i64(args[0], &token) || token < 0) return idm_primitive_type_error(rt, err, "repl-abort", args[0], "a compile token");
    idm_repl_abort(rt->repl, (uint64_t)token);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_repl_spawn(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-spawn", err)) return false;
    if (!idm_is_closure(args[0])) return idm_primitive_type_error(rt, err, "repl-spawn", args[0], "a function value");
    IdmScheduler *sched = idm_repl_scheduler(rt->repl);
    IdmValue pid = idm_nil();
    if (!idm_sched_spawn(sched, args[0], idm_current_exec(), &pid, err)) return false;
    idm_sched_watch(sched, idm_value_id(pid));
    *out = pid;
    return true;
}

static bool prim_repl_diagnostic(IdmRuntime *rt, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-diagnostic", err)) return false;
    char *diag = idm_sched_take_diagnostic(idm_repl_scheduler(rt->repl));
    if (!diag) {
        *out = idm_nil();
        return true;
    }
    *out = idm_string(rt, diag, err);
    free(diag);
    return !(err && err->present);
}

static bool prim_ish_session(IdmRuntime *rt, IdmValue *out) {
    uint64_t pid = rt->repl ? idm_repl_session_pid(rt->repl) : 0;
    *out = pid != 0 ? idm_pid(pid) : idm_nil();
    return true;
}

static IdmScheduler *job_sched(const char *name, IdmError *err) {
    IdmScheduler *sched = idm_exec_scheduler(idm_current_exec());
    if (!sched) idm_error_set(err, idm_span_unknown(NULL), "%s requires an actor context", name);
    return sched;
}

static bool job_arg_atom(IdmValue v, const char *name) {
    return idm_value_tag(v) == IDM_VAL_ATOM && strcmp(idm_symbol_text(idm_value_symbol(v)), name) == 0;
}

static bool prim_port_status(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "port-status", args[0], "a port");
    IdmScheduler *sched = job_sched("port-status", err);
    if (!sched) return false;
    int state = 0;
    if (!idm_sched_port_status(sched, idm_value_id(args[0]), &state)) return idm_primitive_type_error(rt, err, "port-status", args[0], "a live port");
    *out = idm_atom(rt, state == 2 ? "done" : state == 1 ? "stopped" : "running");
    return true;
}

static bool port_stream_arg(IdmValue v, const char **out_stream) {
    if (job_arg_atom(v, "stdout")) {
        *out_stream = "stdout";
        return true;
    }
    if (job_arg_atom(v, "stderr")) {
        *out_stream = "stderr";
        return true;
    }
    return false;
}

static bool prim_port_read(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "port-read", args[0], "a port");
    const char *stream = NULL;
    if (!port_stream_arg(args[1], &stream)) return idm_primitive_type_error(rt, err, "port-read", args[1], ":stdout or :stderr");
    int64_t max = 0;
    if (!value_to_i64(args[2], &max) || max <= 0) return idm_primitive_type_error(rt, err, "port-read", args[2], "a positive byte count");
    IdmScheduler *sched = job_sched("port-read", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_read(sched, idm_value_id(args[0]), stream, (size_t)max, out, &found, err)) return false;
    if (!found) return idm_primitive_type_error(rt, err, "port-read", args[0], "a live port");
    return true;
}

static bool prim_port_write(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "port-write", args[0], "a port");
    size_t len = 0;
    const char *data = NULL;
    if (!idm_primitive_require_string_arg(rt, args[1], &data, &len, "port-write", err)) return false;
    IdmScheduler *sched = job_sched("port-write", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_write(sched, idm_value_id(args[0]), data, len, out, &found, err)) return false;
    if (!found) return idm_primitive_type_error(rt, err, "port-write", args[0], "a live port");
    return true;
}

static bool prim_port_close_input(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "port-close-input", args[0], "a port");
    IdmScheduler *sched = job_sched("port-close-input", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_close_input(sched, idm_value_id(args[0]), out, &found, err)) return false;
    if (!found) return idm_primitive_type_error(rt, err, "port-close-input", args[0], "a live port");
    return true;
}

static bool prim_job_resume(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "job-resume", args[0], "a port");
    bool fg = job_arg_atom(args[1], "fg");
    if (!fg && !job_arg_atom(args[1], "bg")) return idm_primitive_type_error(rt, err, "job-resume", args[1], ":fg or :bg");
    IdmScheduler *sched = job_sched("job-resume", err);
    if (!sched) return false;
    if (!idm_sched_job_resume(sched, idm_value_id(args[0]), fg)) return idm_primitive_type_error(rt, err, "job-resume", args[0], "a live port");
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_job_signal(IdmRuntime *rt, const IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_value_tag(args[0]) != IDM_VAL_PORT) return idm_primitive_type_error(rt, err, "job-signal", args[0], "a port");
    int signo = job_arg_atom(args[1], "hup") ? SIGHUP
              : job_arg_atom(args[1], "cont") ? SIGCONT
              : job_arg_atom(args[1], "term") ? SIGTERM
              : job_arg_atom(args[1], "kill") ? SIGKILL
              : job_arg_atom(args[1], "int") ? SIGINT
              : 0;
    if (signo == 0) return idm_primitive_type_error(rt, err, "job-signal", args[1], ":hup, :cont, :term, :kill, or :int");
    IdmScheduler *sched = job_sched("job-signal", err);
    if (!sched) return false;
    if (!idm_sched_job_signal(sched, idm_value_id(args[0]), signo)) return idm_primitive_type_error(rt, err, "job-signal", args[0], "a live port");
    *out = idm_atom(rt, "ok");
    return true;
}

bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, const IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!idm_primitive_require_arity(rt, prim, argc, err)) return false;
    switch (prim) {
        case IDM_PRIM_COND:
            *out = idm_value_ok(args[0]) ? args[1] : (argc == 3u ? args[2] : idm_nil());
            (void)rt;
            (void)err;
            return true;
        case IDM_PRIM_CONS: return prim_cons(rt, args, out, err);
        case IDM_PRIM_FIRST: return prim_first(rt, args, out, err);
        case IDM_PRIM_REST: return prim_rest(rt, args, out, err);
        case IDM_PRIM_LIST: return prim_list(rt, args, argc, out, err);
        case IDM_PRIM_TUPLE: return prim_tuple(rt, args, argc, out, err);
        case IDM_PRIM_VECTOR: return prim_vector(rt, args, argc, out, err);
        case IDM_PRIM_DICT: return prim_dict(rt, args, argc, out, err);
        case IDM_PRIM_TUPLE_GET: return prim_tuple_get(rt, args, out, err);
        case IDM_PRIM_APPEND: return prim_append(rt, args, out, err);
        case IDM_PRIM_STR_TO_LIST: return prim_str_to_list(rt, args, out, err);
        case IDM_PRIM_DICT_TO_LIST: return prim_dict_to_list(rt, args, out, err);
        case IDM_PRIM_SEQ_COUNT: return prim_seq_count(rt, args, out, err);
        case IDM_PRIM_SEQ_NTH: return prim_seq_nth(rt, args, out, err);
        case IDM_PRIM_SYNTAX_KIND: return prim_syntax_kind(rt, args, out, err);
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
        case IDM_PRIM_INTERNAL_REGISTER_MACRO: return prim_internal_register_macro(rt, args, out, err);
        case IDM_PRIM_EXPAND_CHECK: return prim_expand_check(rt, args, out, err);
        case IDM_PRIM_INSPECT: return prim_inspect(rt, args, out, err);
        case IDM_PRIM_STR_LEN: return prim_str_len(rt, args, out, err);
        case IDM_PRIM_STR_SLICE: return prim_str_slice(rt, args, out, err);
        case IDM_PRIM_STR_FIND: return prim_str_find(rt, args, out, err);
        case IDM_PRIM_STR_BYTE: return prim_str_byte(rt, args, out, err);
        case IDM_PRIM_BYTE_STR: return prim_byte_str(rt, args, out, err);
        case IDM_PRIM_REGEX_COMPILE: return prim_regex_compile(rt, args, out, err);
        case IDM_PRIM_REGEX_PRED: return prim_regex_pred(rt, args, out, err);
        case IDM_PRIM_REGEX_SOURCE: return prim_regex_source(rt, args, out, err);
        case IDM_PRIM_REGEX_OPTIONS: return prim_regex_options(rt, args, out, err);
        case IDM_PRIM_REGEX_GROUP_COUNT: return prim_regex_group_count(rt, args, out, err);
        case IDM_PRIM_REGEX_GROUP_NAMES: return prim_regex_group_names(rt, args, out, err);
        case IDM_PRIM_REGEX_RESULT_PRED: return prim_regex_result_pred(rt, args, out, err);
        case IDM_PRIM_REGEX_SCAN_AT: return prim_regex_scan_at(rt, args, out, err);
        case IDM_PRIM_REGEX_SCAN_FROM: return prim_regex_scan_from(rt, args, out, err);
        case IDM_PRIM_REGEX_SCAN_FULL: return prim_regex_scan_full(rt, args, out, err);
        case IDM_PRIM_REGEX_TEST: return prim_regex_test(rt, args, out, err);
        case IDM_PRIM_REGEX_RESULT_START: return idm_regex_result_start_value(rt, args[0], out, err);
        case IDM_PRIM_REGEX_RESULT_END: return idm_regex_result_end_value(rt, args[0], out, err);
        case IDM_PRIM_REGEX_RESULT_TEXT: return idm_regex_result_text_value(rt, args[0], out, err);
        case IDM_PRIM_REGEX_CAPTURE: return idm_regex_capture_value(rt, args[0], args[1], out, err);
        case IDM_PRIM_REGEX_CAPTURE_RANGE: return idm_regex_capture_range_value(rt, args[0], args[1], out, err);
        case IDM_PRIM_REGEX_CAPTURE_NAMED: return idm_regex_capture_named_value(rt, args[0], args[1], out, err);
        case IDM_PRIM_REGEX_CAPTURES: return idm_regex_captures_value(rt, args[0], out, err);
        case IDM_PRIM_REGEX_SCAN_ALL: return prim_regex_scan_all(rt, args, out, err);
        case IDM_PRIM_REGEX_REPLACE: return prim_regex_replace(rt, args, out, err, false);
        case IDM_PRIM_REGEX_REPLACE_ALL: return prim_regex_replace(rt, args, out, err, true);
        case IDM_PRIM_REGEX_SPLIT_ON: return idm_regex_split_on(rt, args[0], args[1], out, err);
        case IDM_PRIM_REGEX_ESCAPE: return idm_regex_escape(rt, args[0], out, err);
        case IDM_PRIM_FILE_READ: return prim_file_read(rt, args, out, err);
        case IDM_PRIM_FILE_WRITE: return prim_file_write(rt, args, out, err);
        case IDM_PRIM_FILE_EXISTS: return prim_file_exists(rt, args, out, err);
        case IDM_PRIM_FILE_STAT: return prim_file_stat(rt, args, out, err);
        case IDM_PRIM_FILE_LIST: return prim_file_list(rt, args, out, err);
        case IDM_PRIM_FILE_REMOVE: return prim_file_remove(rt, args, out, err);
        case IDM_PRIM_ARGS: return prim_args(rt, args, out, err);
        case IDM_PRIM_TIME_MS: return prim_time_ms(rt, args, out, err);
        case IDM_PRIM_TIME_NS: return prim_time_ns(rt, args, out, err);
        case IDM_PRIM_RANDOM: return prim_random(rt, args, out, err);
        case IDM_PRIM_DICT_GET: return prim_dict_get(rt, args, out, err);
        case IDM_PRIM_DICT_PUT: return prim_dict_put(rt, args, out, err);
        case IDM_PRIM_DICT_DEL: return prim_dict_del(rt, args, out, err);
        case IDM_PRIM_DICT_KEYS: return prim_dict_keys(rt, args, out, err);
        case IDM_PRIM_DICT_VALS: return prim_dict_vals(rt, args, out, err);
        case IDM_PRIM_DICT_HAS: return prim_dict_has(rt, args, out, err);
        case IDM_PRIM_DICT_SIZE: return prim_dict_size(rt, args, out, err);
        case IDM_PRIM_SYNTAX_INT_VALUE: return prim_syntax_int_value(rt, args, out, err);
        case IDM_PRIM_SYNTAX_FLOAT_VALUE: return prim_syntax_float_value(rt, args, out, err);
        case IDM_PRIM_MAKE_SYNTAX_NIL: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_NIL, args[0], idm_nil(), out, err);
        case IDM_PRIM_MAKE_SYNTAX_WORD: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_WORD, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_ATOM: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_ATOM, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_INT: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_INT, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_STRING: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_STRING, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_LIST: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_LIST, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_VECTOR: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_VECTOR, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_TUPLE: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_TUPLE, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_DICT: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_DICT, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_EXPR: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_EXPR, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_BODY: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_BODY, args[0], args[1], out, err);
        case IDM_PRIM_MAKE_SYNTAX_GROUP: return idm_syntax_build(rt, IDM_SYNTAX_BUILD_GROUP, args[0], args[1], out, err);
        case IDM_PRIM_SYNTAX_ERROR: return prim_syntax_error(rt, args, out, err);
        case IDM_PRIM_LOCAL_EXPAND: return prim_local_expand(rt, args, out, err);
        case IDM_PRIM_FREE_IDENTIFIER_EQ: return prim_free_identifier_eq(rt, args, out, err);
        case IDM_PRIM_BOUND_IDENTIFIER_EQ: return prim_bound_identifier_eq(rt, args, out, err);
        case IDM_PRIM_BIND_BANG: return prim_bind_bang(rt, args, out, err);
        case IDM_PRIM_ADD:
        case IDM_PRIM_SUB:
        case IDM_PRIM_MUL:
        case IDM_PRIM_DIV:
        case IDM_PRIM_MOD:
        case IDM_PRIM_POW: return prim_arith(rt, prim, args, out, err);
        case IDM_PRIM_NEG: return prim_neg(rt, args, out, err);
        case IDM_PRIM_EQ: return prim_eq(rt, args, out, err);
        case IDM_PRIM_NEQ: return prim_neq(rt, args, out, err);
        case IDM_PRIM_LT:
        case IDM_PRIM_GT:
        case IDM_PRIM_LTE:
        case IDM_PRIM_GTE: return prim_num_compare(rt, prim, args, out, err);
        case IDM_PRIM_COMPARE: return prim_term_compare(rt, args, out, err);
        case IDM_PRIM_RAISE: return prim_raise_fallback(rt, args, out, err);
        case IDM_PRIM_OK: return prim_ok(rt, args, out, err);
        case IDM_PRIM_IS_A_P: return prim_is_a(rt, args, out, err);
        case IDM_PRIM_NIL_P:
        case IDM_PRIM_ATOM_P:
        case IDM_PRIM_WORD_P:
        case IDM_PRIM_INT_P:
        case IDM_PRIM_FLOAT_P:
        case IDM_PRIM_STRING_P:
        case IDM_PRIM_PAIR_P:
        case IDM_PRIM_EMPTY_LIST_P:
        case IDM_PRIM_LIST_P:
        case IDM_PRIM_TUPLE_P:
        case IDM_PRIM_VECTOR_P:
        case IDM_PRIM_DICT_P:
        case IDM_PRIM_SYNTAX_P:
        case IDM_PRIM_CELL_P:
        case IDM_PRIM_CLOSURE_P:
        case IDM_PRIM_PID_P:
        case IDM_PRIM_REF_P:
        case IDM_PRIM_PORT_P:
        case IDM_PRIM_REGEX_P:
        case IDM_PRIM_REGEX_RESULT_P:
            return prim_type_pred(rt, prim, args, out, err);
        case IDM_PRIM_ERROR_MESSAGE: return prim_error_message(rt, args, out, err);
        case IDM_PRIM_MAKE_ERROR: return prim_make_error(rt, args, out, err);
        case IDM_PRIM_SELF:
        case IDM_PRIM_SPAWN:
        case IDM_PRIM_SPAWN_LINK:
        case IDM_PRIM_SPAWN_MONITOR:
        case IDM_PRIM_SEND:
        case IDM_PRIM_EXIT:
        case IDM_PRIM_LINK:
        case IDM_PRIM_UNLINK:
        case IDM_PRIM_MONITOR:
        case IDM_PRIM_DEMONITOR:
        case IDM_PRIM_TRAP_EXIT:
        case IDM_PRIM_TTY_READ:
        case IDM_PRIM_TTY_READ_LINE:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", idm_primitive_name(prim));
        case IDM_PRIM_APPLY:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive 'apply' is compiled directly and cannot be invoked generically");
        case IDM_PRIM_STR: return prim_str(rt, args, argc, out, err);
        case IDM_PRIM_CHOMP: return prim_chomp(rt, args, out, err);
        case IDM_PRIM_PRINT: return prim_print_impl(rt, args, argc, false, out, err);
        case IDM_PRIM_PRINTLN: return prim_print_impl(rt, args, argc, true, out, err);
        case IDM_PRIM_CD: return prim_cd(rt, args, out, err);
        case IDM_PRIM_CHDIR: return prim_cd(rt, args, out, err);
        case IDM_PRIM_PWD: return prim_pwd(rt, args, out, err);
        case IDM_PRIM_ENV_GET: return prim_env_get(rt, args, out, err);
        case IDM_PRIM_ENV_SET: return prim_env_set(rt, args, out, err);
        case IDM_PRIM_ABS: return prim_abs(rt, args, out, err);
        case IDM_PRIM_FLOOR: return prim_float_unary(rt, "floor", floor, args, out, err);
        case IDM_PRIM_ROUND: return prim_float_unary(rt, "round", round, args, out, err);
        case IDM_PRIM_CEIL: return prim_float_unary(rt, "ceil", ceil, args, out, err);
        case IDM_PRIM_TRUNCATE: return prim_float_unary(rt, "truncate", trunc, args, out, err);
        case IDM_PRIM_SQRT: return prim_sqrt(rt, args, out, err);
        case IDM_PRIM_SIN: return prim_math_unary(rt, "sin", sin, args, out, err);
        case IDM_PRIM_COS: return prim_math_unary(rt, "cos", cos, args, out, err);
        case IDM_PRIM_TAN: return prim_math_unary(rt, "tan", tan, args, out, err);
        case IDM_PRIM_ASIN: return prim_math_unary(rt, "asin", asin, args, out, err);
        case IDM_PRIM_ACOS: return prim_math_unary(rt, "acos", acos, args, out, err);
        case IDM_PRIM_ATAN: return prim_math_unary(rt, "atan", atan, args, out, err);
        case IDM_PRIM_ATAN2: return prim_math_binary(rt, "atan2", atan2, args, out, err);
        case IDM_PRIM_EXP: return prim_math_unary(rt, "exp", exp, args, out, err);
        case IDM_PRIM_LOG: return prim_math_unary(rt, "log", log, args, out, err);
        case IDM_PRIM_LOG2: return prim_math_unary(rt, "log2", log2, args, out, err);
        case IDM_PRIM_LOG10: return prim_math_unary(rt, "log10", log10, args, out, err);
        case IDM_PRIM_HYPOT: return prim_math_binary(rt, "hypot", hypot, args, out, err);
        case IDM_PRIM_NAN_P:
        case IDM_PRIM_FINITE_P:
        case IDM_PRIM_INFINITE_P: return prim_number_classify(rt, prim, args, out, err);
        case IDM_PRIM_NAN: *out = idm_atom(rt, "nan"); return true;
        case IDM_PRIM_INF: *out = idm_atom(rt, "inf"); return true;
        case IDM_PRIM_FLOOR_DIV:
        case IDM_PRIM_FLOOR_MOD: return prim_floor_divmod(rt, prim, args, out, err);
        case IDM_PRIM_DIVMOD: return prim_divmod(rt, args, out, err);
        case IDM_PRIM_BIT_AND:
        case IDM_PRIM_BIT_OR:
        case IDM_PRIM_BIT_XOR: return prim_bit_binary(rt, prim, args, out, err);
        case IDM_PRIM_BIT_NOT: return prim_bit_not(rt, args, out, err);
        case IDM_PRIM_SHIFT_LEFT: return prim_shift_left(rt, args, out, err);
        case IDM_PRIM_SHIFT_RIGHT: return prim_shift_right(rt, args, out, err);
        case IDM_PRIM_BIT_COUNT: return prim_bit_count(rt, args, out, err);
        case IDM_PRIM_BIT_LENGTH: return prim_bit_length(rt, args, out, err);
        case IDM_PRIM_TO_INT: return prim_to_int(rt, args, out, err);
        case IDM_PRIM_TO_FLOAT: return prim_to_float(rt, args, out, err);
        case IDM_PRIM_PARSE_INT: return prim_parse_int(rt, args, out, err);
        case IDM_PRIM_PARSE_FLOAT: return prim_parse_float(rt, args, out, err);
        case IDM_PRIM_FILE_MKDIR: return prim_file_mkdir(rt, args, out, err);
        case IDM_PRIM_FILE_APPEND: return prim_file_append(rt, args, out, err);
        case IDM_PRIM_FILE_OPEN: return prim_file_open(rt, args, out, err);
        case IDM_PRIM_ORD_STR: return prim_ord_str(rt, args, out, err);
        case IDM_PRIM_FROM_RUNES: return prim_from_runes(rt, args, out, err);
        case IDM_PRIM_REPL_COMPILE: return prim_repl_compile(rt, args, out, err);
        case IDM_PRIM_REPL_ABORT: return prim_repl_abort(rt, args, out, err);
        case IDM_PRIM_REPL_SPAWN: return prim_repl_spawn(rt, args, out, err);
        case IDM_PRIM_REPL_DIAGNOSTIC: return prim_repl_diagnostic(rt, out, err);
        case IDM_PRIM_ISH_SESSION: return prim_ish_session(rt, out);
        case IDM_PRIM_TTY_PRED: return idm_prim_tty_pred(rt, out, err);
        case IDM_PRIM_TTY_RAW: return idm_prim_tty_raw(rt, out, err);
        case IDM_PRIM_TTY_RESTORE: return idm_prim_tty_restore(rt, out, err);
        case IDM_PRIM_TTY_WRITE: return idm_prim_tty_write(rt, args, out, err);
        case IDM_PRIM_TTY_SIZE: return idm_prim_tty_size(rt, out, err);
        case IDM_PRIM_EPRINTLN: return prim_print_to(rt, stderr, args, argc, true, out, err);
        case IDM_PRIM_PORT_STATUS: return prim_port_status(rt, args, out, err);
        case IDM_PRIM_PORT_READ: return prim_port_read(rt, args, out, err);
        case IDM_PRIM_PORT_WRITE: return prim_port_write(rt, args, out, err);
        case IDM_PRIM_PORT_CLOSE_INPUT: return prim_port_close_input(rt, args, out, err);
        case IDM_PRIM_JOB_RESUME: return prim_job_resume(rt, args, out, err);
        case IDM_PRIM_JOB_SIGNAL: return prim_job_signal(rt, args, out, err);
    }
    return idm_error_set(err, idm_span_unknown(NULL), "unimplemented primitive '%s'", idm_primitive_name(prim));
}
