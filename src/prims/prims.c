#define _XOPEN_SOURCE 700

#include "idiom/prims.h"
#include "idiom/actor.h"
#include "idiom/bytecode.h"
#include "idiom/expand.h"
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
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static bool type_error(IdmRuntime *rt, IdmError *err, const char *name, IdmValue got, const char *what) {
    idm_error_set(err, idm_span_unknown(NULL), "%s expects %s", name, what);
    return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, name), got);
}

static bool require_arity(IdmRuntime *rt, IdmPrimitive prim, uint32_t argc, IdmError *err) {
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

static const char *require_string(IdmValue v, size_t *out_len, IdmError *err);

static bool prim_cons(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    *out = idm_cons(rt, args[0], args[1], err);
    return !(err && err->present);
}

static bool prim_first(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_pair(args[0])) return type_error(rt, err, "first", args[0], "a pair");
    *out = idm_car(args[0], err);
    return !(err && err->present);
}

static bool prim_rest(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!idm_is_pair(args[0])) return type_error(rt, err, "rest", args[0], "a pair");
    *out = idm_cdr(args[0], err);
    return !(err && err->present);
}

static bool prim_list(IdmRuntime *rt, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    IdmValue result = idm_empty_list();
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
    if (idm_is_empty_list(head)) { *out = tail; return true; }
    if (!idm_is_pair(head)) return type_error(rt, err, "append", head, "a list as first argument");
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
    IdmValue reason = idm_sequence_item(result, 1, &ignore);
    IdmValue stdout_v = idm_sequence_item(result, 2, &ignore);
    idm_error_clear(&ignore);
    if (idm_value_is_error(result) && reason.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(reason.as.symbol), "capture-overflow") == 0) {
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
    if (!idm_is_tuple(args[0])) return type_error(rt, err, "tuple-get", args[0], "a tuple");
    if (args[1].tag != IDM_VAL_INT || args[1].as.i < 0) return type_error(rt, err, "tuple-get", args[1], "a non-negative integer index");
    size_t index = (size_t)args[1].as.i;
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

static bool sequence_to_list(IdmRuntime *rt, IdmValue v, const char *name, IdmValue *out, IdmError *err) {
    if (!idm_is_vector(v) && !idm_is_tuple(v)) return type_error(rt, err, name, v, "a vector or tuple");
    size_t count = idm_sequence_count(v);
    IdmValue result = idm_empty_list();
    for (size_t i = count; i > 0; i--) {
        IdmValue item = idm_sequence_item(v, i - 1u, err);
        if (err && err->present) return false;
        result = idm_cons(rt, item, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_vector_to_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_vector(v)) return type_error(rt, err, "vector-to-list", v, "a vector");
    return sequence_to_list(rt, v, "vector-to-list", out, err);
}

static bool prim_tuple_to_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_tuple(v)) return type_error(rt, err, "tuple-to-list", v, "a tuple");
    return sequence_to_list(rt, v, "tuple-to-list", out, err);
}

static bool prim_dict_to_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_dict(v)) return type_error(rt, err, "dict-to-list", v, "a dict");
    size_t count = idm_dict_count(v);
    IdmValue result = idm_empty_list();
    for (size_t i = count; i > 0; i--) {
        IdmValue key;
        IdmValue val;
        if (!idm_dict_entry(v, i - 1u, &key, &val)) return idm_error_set(err, idm_span_unknown(NULL), "dict-to-list iteration failed");
        IdmValue pair_items[2] = {key, val};
        IdmValue pair = idm_tuple(rt, pair_items, 2u, err);
        if (err && err->present) return false;
        result = idm_cons(rt, pair, result, err);
        if (err && err->present) return false;
    }
    *out = result;
    return true;
}

static bool prim_str_to_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    if (!idm_is_string(v)) return type_error(rt, err, "str-to-list", v, "a string");
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

static bool prim_cd(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
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

static bool prim_pwd(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_env_get(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t len = 0;
    const char *bytes = require_string(args[0], &len, err);
    if (!bytes) return false;
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

static bool prim_env_set(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t name_len = 0;
    const char *name_bytes = require_string(args[0], &name_len, err);
    if (!name_bytes) return false;
    size_t value_len = 0;
    const char *value_bytes = require_string(args[1], &value_len, err);
    if (!value_bytes) return false;
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

static bool prim_make_record(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err, bool raw) {
    const char *type = name_value_text(args[0], err, "record type");
    if (!type) return false;
    if (raw && strchr(type, '#')) return idm_error_set(err, idm_span_unknown(NULL), "make-record type must not contain '#' (reserved for declared records)");
    if (!idm_is_dict(args[1])) return idm_error_set(err, idm_span_unknown(NULL), "make-record fields must be a dict");
    *out = idm_record(rt, type, args[1], err);
    return !(err && err->present);
}

static bool prim_record_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_is_record(args[0]));
    return true;
}

static bool prim_record_type(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *type = idm_record_type(args[0], err);
    if (!type) return false;
    *out = idm_atom(rt, type);
    return true;
}

static bool prim_record_field(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (idm_record_field(args[0], args[1], out, err)) return true;
    if (!idm_is_record(args[0])) return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "record-field"), args[0]);
    return idm_error_reason(rt, err, "key-not-found", 1, args[1]);
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

static bool prim_print_to(IdmRuntime *rt, FILE *stream, IdmValue *args, uint32_t argc, bool newline, IdmValue *out, IdmError *err) {
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

static bool prim_print_impl(IdmRuntime *rt, IdmValue *args, uint32_t argc, bool newline, IdmValue *out, IdmError *err) {
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
    if (v.tag == IDM_VAL_INT) { *out = (double)v.as.i; return true; }
    if (v.tag == IDM_VAL_FLOAT) { *out = v.as.f; return true; }
    return false;
}

static bool num_pair(IdmRuntime *rt, const char *name, IdmValue *args, bool *ints, int64_t *ia, int64_t *ib, double *fa, double *fb, IdmError *err) {
    if (args[0].tag == IDM_VAL_INT && args[1].tag == IDM_VAL_INT) {
        *ints = true;
        *ia = args[0].as.i;
        *ib = args[1].as.i;
        return true;
    }
    if (!num_as_double(args[0], fa) || !num_as_double(args[1], fb)) {
        idm_error_set(err, idm_span_unknown(NULL), "%s expects numeric operands", name);
        return idm_error_reason(rt, err, "type-error", 3, idm_atom(rt, name), args[0], args[1]);
    }
    *ints = false;
    return true;
}

static bool overflow_error(IdmRuntime *rt, const char *name, IdmError *err) {
    idm_error_set(err, idm_span_unknown(NULL), "integer overflow in %s", name);
    return idm_error_reason(rt, err, "overflow", 1, idm_atom(rt, name));
}

static bool div_zero_error(IdmRuntime *rt, const char *name, IdmError *err) {
    idm_error_set(err, idm_span_unknown(NULL), "division by zero in %s", name);
    return idm_error_reason(rt, err, "div-by-zero", 1, idm_atom(rt, name));
}

static bool prim_arith(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    bool ints = false;
    int64_t a = 0, b = 0, r = 0;
    double x = 0.0, y = 0.0;
    if (!num_pair(rt, name, args, &ints, &a, &b, &x, &y, err)) return false;
    if (!ints) {
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
        *out = idm_float(f);
        return true;
    }
    bool ok = true;
    switch (prim) {
        case IDM_PRIM_ADD: ok = idm_checked_add(a, b, &r); break;
        case IDM_PRIM_SUB: ok = idm_checked_sub(a, b, &r); break;
        case IDM_PRIM_MUL: ok = idm_checked_mul(a, b, &r); break;
        case IDM_PRIM_DIV:
        case IDM_PRIM_MOD:
            if (b == 0) return div_zero_error(rt, name, err);
            if (a == INT64_MIN && b == -1) return overflow_error(rt, name, err);
            r = prim == IDM_PRIM_DIV ? a / b : a % b;
            break;
        case IDM_PRIM_POW:
            if (b < 0) {
                idm_error_set(err, idm_span_unknown(NULL), "pow exponent must be non-negative");
                return idm_error_reason(rt, err, "bad-arg", 2, idm_atom(rt, name), args[1]);
            }
            ok = idm_checked_pow(a, b, &r);
            break;
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid integer primitive");
    }
    if (!ok) return overflow_error(rt, name, err);
    *out = idm_int(r);
    return true;
}

static bool prim_neg(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag == IDM_VAL_FLOAT) { *out = idm_float(-args[0].as.f); return true; }
    if (args[0].tag != IDM_VAL_INT) return type_error(rt, err, "neg", args[0], "a number");
    if (args[0].as.i == INT64_MIN) return overflow_error(rt, "neg", err);
    *out = idm_int(-args[0].as.i);
    return true;
}

static bool prim_compare(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    bool ints = false;
    int64_t a = 0, b = 0;
    double x = 0.0, y = 0.0;
    if (!num_pair(rt, name, args, &ints, &a, &b, &x, &y, err)) return false;
    bool r = false;
    switch (prim) {
        case IDM_PRIM_LT: r = ints ? a < b : x < y; break;
        case IDM_PRIM_GT: r = ints ? a > b : x > y; break;
        case IDM_PRIM_LTE: r = ints ? a <= b : x <= y; break;
        case IDM_PRIM_GTE: r = ints ? a >= b : x >= y; break;
        default: return idm_error_set(err, idm_span_unknown(NULL), "invalid comparison primitive");
    }
    *out = idm_bool(rt, r);
    return true;
}

static bool prim_eq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_value_equal(args[0], args[1]));
    return true;
}

static bool prim_neq(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, !idm_value_equal(args[0], args[1]));
    return true;
}

static bool prim_ok(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_value_ok(args[0]));
    return true;
}

static bool prim_error_message(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmBuffer buf;
    idm_buf_init(&buf);
    idm_error_describe(rt, args[0], &buf);
    *out = idm_string(rt, buf.data ? buf.data : "", err);
    idm_buf_destroy(&buf);
    return out->tag == IDM_VAL_STRING;
}

static bool prim_make_error(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_error_value(rt, args[0]);
    return out->tag == IDM_VAL_TUPLE;
}

static bool prim_trait_implements(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *trait = NULL;
    if (args[1].tag == IDM_VAL_ATOM || args[1].tag == IDM_VAL_WORD) trait = idm_symbol_text(args[1].as.symbol);
    else if (args[1].tag == IDM_VAL_STRING) trait = idm_string_bytes(args[1]);
    if (!trait) return type_error(rt, err, "%trait-implements?", args[1], "a trait identity");
    const char *type = idm_value_dispatch_type_name(args[0]);
    *out = idm_bool(rt, idm_trait_implements(rt, trait, type));
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
            IdmValue list = idm_empty_list();
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
        case IDM_VAL_EMPTY_LIST: out = idm_syn_list(span); break;
        case IDM_VAL_INT: out = idm_syn_int(datum.as.i, span); break;
        case IDM_VAL_WORD: out = idm_syn_word(idm_symbol_text(datum.as.symbol), span); break;
        case IDM_VAL_ATOM: out = idm_syn_atom(idm_symbol_text(datum.as.symbol), span); break;
        case IDM_VAL_STRING: out = idm_syn_string(idm_string_bytes(datum), span); break;
        case IDM_VAL_PAIR: {
            out = idm_syn_list(span);
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
            if (!idm_is_empty_list(cur)) { idm_syn_free(out); idm_error_set(err, span, "datum->syntax requires a proper list"); return NULL; }
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
    IdmValue detail[2];
    detail[0] = idm_atom(rt, "expand");
    detail[1] = idm_string(rt, inner.message ? inner.message : "compile failed", err);
    idm_error_clear(&inner);
    if (err && err->present) return false;
    IdmValue error_detail = idm_tuple(rt, detail, 2u, err);
    if (err && err->present) return false;
    *out = idm_error_value(rt, error_detail);
    return !(err && err->present);
}

static bool require_string_arg(IdmRuntime *rt, IdmValue v, const char **out_s, size_t *out_len, const char *what, IdmError *err) {
    if (v.tag != IDM_VAL_STRING) return type_error(rt, err, what, v, "a string");
    *out_s = idm_string_bytes(v);
    *out_len = idm_string_length(v);
    return true;
}

static int64_t clamp_index(int64_t i, size_t len) {
    if (i < 0) i = 0;
    if ((size_t)i > len) i = (int64_t)len;
    return i;
}

static bool prim_str_len(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    if (!require_string_arg(rt, args[0], &s, &len, "str-len", err)) return false;
    *out = idm_int((int64_t)len);
    return true;
}

static bool prim_str_slice(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *s; size_t len;
    if (!require_string_arg(rt, args[0], &s, &len, "str-slice", err)) return false;
    if (args[1].tag != IDM_VAL_INT) return type_error(rt, err, "str-slice", args[1], "integer bounds");
    if (args[2].tag != IDM_VAL_INT) return type_error(rt, err, "str-slice", args[2], "integer bounds");
    int64_t a = args[1].as.i;
    int64_t b = args[2].as.i;
    if (a < 0 || b < a || (uint64_t)b > len) {
        idm_error_set(err, idm_span_unknown(NULL), "str-slice range %lld..%lld out of bounds for length %zu", (long long)a, (long long)b, len);
        return idm_error_reason(rt, err, "slice-out-of-range", 3, args[1], args[2], idm_int((int64_t)len));
    }
    *out = idm_string_n(rt, s + a, (size_t)(b - a), err);
    return !(err && err->present);
}

static bool prim_str_find(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    const char *needle; size_t nlen;
    if (!require_string_arg(rt, args[0], &s, &len, "str-find", err)) return false;
    if (!require_string_arg(rt, args[1], &needle, &nlen, "str-find", err)) return false;
    if (args[2].tag != IDM_VAL_INT) return type_error(rt, err, "str-find", args[2], "an integer start");
    size_t from = (size_t)clamp_index(args[2].as.i, len);
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

static bool prim_str_byte(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    const char *s; size_t len;
    if (!require_string_arg(rt, args[0], &s, &len, "str-byte", err)) return false;
    if (args[1].tag != IDM_VAL_INT) return type_error(rt, err, "str-byte", args[1], "an integer index");
    int64_t i = args[1].as.i;
    if (i < 0 || (size_t)i >= len) { *out = idm_nil(); return true; }
    *out = idm_int((int64_t)(unsigned char)s[i]);
    return true;
}

static bool prim_byte_str(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_INT || args[0].as.i < 0 || args[0].as.i > 255) return type_error(rt, err, "byte-str", args[0], "an integer 0..255");
    char c = (char)args[0].as.i;
    *out = idm_string_n(rt, &c, 1u, err);
    return !(err && err->present);
}

static bool prim_regex_compile(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_compile_value(rt, args[0], args[1], out, err);
}

static bool prim_regex_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_is_regex(args[0]));
    return true;
}

static bool prim_regex_source(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmRegex *rx = idm_regex_value_get(args[0], err);
    if (!rx) return false;
    size_t len = 0;
    const char *source = idm_regex_source(rx, &len);
    *out = idm_string_n(rt, source, len, err);
    return !(err && err->present);
}

static bool prim_regex_options(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_options_value(rt, args[0], out, err);
}

static bool prim_regex_group_count(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt;
    IdmRegex *rx = idm_regex_value_get(args[0], err);
    if (!rx) return false;
    *out = idm_int((int64_t)idm_regex_group_count(rx));
    return true;
}

static bool prim_regex_group_names(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_group_names_value(rt, args[0], out, err);
}

static bool prim_regex_result_pred(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)err;
    *out = idm_bool(rt, idm_is_regex_result(args[0]));
    return true;
}

static bool regex_offset_arg(IdmRuntime *rt, const char *name, IdmValue value, size_t *out, IdmError *err) {
    if (value.tag != IDM_VAL_INT || value.as.i < 0) return type_error(rt, err, name, value, "a non-negative integer offset");
    *out = (size_t)value.as.i;
    return true;
}

static bool prim_regex_scan_at(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t offset = 0;
    if (!regex_offset_arg(rt, "regex-raw-scan-at", args[2], &offset, err)) return false;
    return idm_regex_scan_at(rt, args[0], args[1], offset, out, err);
}

static bool prim_regex_scan_from(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    size_t offset = 0;
    if (!regex_offset_arg(rt, "regex-raw-scan-from", args[2], &offset, err)) return false;
    return idm_regex_scan_from(rt, args[0], args[1], offset, out, err);
}

static bool prim_regex_scan_full(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_scan_full(rt, args[0], args[1], out, err);
}

static bool prim_regex_test(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_test(rt, args[0], args[1], out, err);
}

static bool prim_regex_scan_all(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    return idm_regex_scan_all(rt, args[0], args[1], out, err);
}

static bool prim_regex_replace(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err, bool all) {
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

static bool prim_file_read(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-read", err)) return false;
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

static bool prim_file_write(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-write", err)) return false;
    if (!require_string_arg(rt, args[1], &data, &dlen, "file-write", err)) return false;
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

static bool prim_file_exists(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-exists?", err)) return false;
    char pb[PATH_MAX];
    path = resolve_cwd_record(rt, path, pb, sizeof(pb));
    *out = idm_bool(rt, path && access(path, F_OK) == 0);
    return true;
}

static bool prim_file_stat(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-stat", err)) return false;
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

static bool prim_file_list(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-list", err)) return false;
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

static bool prim_file_remove(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-remove", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    if (remove(path) != 0) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_args(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
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

static bool prim_time_ms(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    (void)rt; (void)args; (void)err;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *out = idm_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return true;
}

static bool prim_random(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_INT || args[0].as.i <= 0) return type_error(rt, err, "random", args[0], "a positive integer bound");
    *out = idm_int((int64_t)(random() % args[0].as.i));
    return true;
}

static bool require_dict(IdmRuntime *rt, const char *name, IdmValue v, IdmError *err) {
    if (v.tag == IDM_VAL_DICT) return true;
    return type_error(rt, err, name, v, "a dict");
}

static bool prim_dict_get(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-get", args[0], err)) return false;
    IdmValue found;
    *out = idm_dict_get(args[0], args[1], &found) ? found : args[2];
    return true;
}

static bool dict_rebuild(IdmRuntime *rt, IdmValue d, IdmValue key, bool put, IdmValue val, IdmValue *out, IdmError *err) {
    size_t n = idm_dict_count(d);
    size_t cap = n + (put ? 1u : 0u);
    IdmDictEntry *entries = cap ? calloc(cap, sizeof(*entries)) : NULL;
    if (cap && !entries) return idm_error_oom(err, idm_span_unknown(NULL));
    size_t count = 0;
    bool replaced = false;
    for (size_t i = 0; i < n; i++) {
        IdmValue k, v;
        if (!idm_dict_entry(d, i, &k, &v)) continue;
        if (idm_value_equal(k, key)) {
            if (!put) continue;
            v = val;
            replaced = true;
        }
        entries[count].key = k;
        entries[count].value = v;
        count++;
    }
    if (put && !replaced) {
        entries[count].key = key;
        entries[count].value = val;
        count++;
    }
    *out = idm_dict(rt, entries, count, err);
    free(entries);
    return !(err && err->present);
}

static bool prim_dict_put(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-put", args[0], err)) return false;
    return dict_rebuild(rt, args[0], args[1], true, args[2], out, err);
}

static bool prim_dict_del(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-del", args[0], err)) return false;
    return dict_rebuild(rt, args[0], args[1], false, idm_nil(), out, err);
}

static bool prim_dict_keys(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-keys", args[0], err)) return false;
    IdmValue acc = idm_empty_list();
    for (size_t i = idm_dict_count(args[0]); i > 0; i--) {
        IdmValue k, v;
        if (!idm_dict_entry(args[0], i - 1u, &k, &v)) continue;
        acc = idm_cons(rt, k, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool prim_dict_vals(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-vals", args[0], err)) return false;
    IdmValue acc = idm_empty_list();
    for (size_t i = idm_dict_count(args[0]); i > 0; i--) {
        IdmValue k, v;
        if (!idm_dict_entry(args[0], i - 1u, &k, &v)) continue;
        acc = idm_cons(rt, v, acc, err);
        if (err && err->present) return false;
    }
    *out = acc;
    return true;
}

static bool prim_dict_has(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-has?", args[0], err)) return false;
    IdmValue found;
    *out = idm_bool(rt, idm_dict_get(args[0], args[1], &found));
    return true;
}

static bool prim_dict_size(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_dict(rt, "dict-size", args[0], err)) return false;
    *out = idm_int((int64_t)idm_dict_count(args[0]));
    return true;
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
    if (!idm_is_empty_list(cur)) return idm_error_set(err, idm_span_unknown(NULL), "expected proper list of syntax");
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
    IdmSyntax **items = NULL;
    size_t count = 0;
    if (!collect_list_of_syntax(args[1], &items, &count, err)) return false;
    IdmSyntax *syn = idm_syn_list(ctx->span);
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

static IdmSyntax *syntax_form_sequence(IdmRuntime *rt, IdmSyntax *ctx, const char *head, IdmSyntax **items, size_t count, IdmError *err) {
    (void)rt;
    IdmSyntax *syn = idm_syn_list(ctx->span);
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
    IdmSyntax *syn = syntax_form_sequence(rt, ctx, "%-expr", items, count, err);
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
    IdmSyntax *syn = syntax_form_sequence(rt, ctx, "%-body", items, count, err);
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
    IdmSyntax *syn = syntax_form_sequence(rt, ctx, "%-group", items, 1, err);
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

static bool prim_abs(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag == IDM_VAL_FLOAT) { *out = idm_float(fabs(args[0].as.f)); return true; }
    if (args[0].tag != IDM_VAL_INT) return type_error(rt, err, "abs", args[0], "a number");
    if (args[0].as.i == INT64_MIN) return overflow_error(rt, "abs", err);
    *out = idm_int(args[0].as.i < 0 ? -args[0].as.i : args[0].as.i);
    return true;
}

static bool prim_float_unary(IdmRuntime *rt, const char *name, double (*fn)(double), IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag == IDM_VAL_INT) { *out = args[0]; return true; }
    if (args[0].tag != IDM_VAL_FLOAT) return type_error(rt, err, name, args[0], "a number");
    *out = idm_float(fn(args[0].as.f));
    return true;
}

static bool prim_sqrt(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    double x = 0.0;
    if (!num_as_double(args[0], &x)) return type_error(rt, err, "sqrt", args[0], "a number");
    if (x < 0.0) {
        idm_error_set(err, idm_span_unknown(NULL), "sqrt of a negative number");
        return idm_error_reason(rt, err, "type-error", 2, idm_atom(rt, "sqrt"), args[0]);
    }
    *out = idm_float(sqrt(x));
    return true;
}

static bool prim_floor_divmod(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *name = idm_primitive_name(prim);
    bool ints = false;
    int64_t a = 0, b = 0;
    double x = 0.0, y = 0.0;
    if (!num_pair(rt, name, args, &ints, &a, &b, &x, &y, err)) return false;
    if (!ints) {
        if (y == 0.0) return div_zero_error(rt, name, err);
        if (prim == IDM_PRIM_FLOOR_DIV) { *out = idm_float(floor(x / y)); return true; }
        double r = fmod(x, y);
        if (r != 0.0 && ((r < 0.0) != (y < 0.0))) r += y;
        *out = idm_float(r);
        return true;
    }
    if (b == 0) return div_zero_error(rt, name, err);
    if (a == INT64_MIN && b == -1) return overflow_error(rt, name, err);
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) { q -= 1; r += b; }
    *out = idm_int(prim == IDM_PRIM_FLOOR_DIV ? q : r);
    return true;
}

static bool parse_result_error(IdmRuntime *rt, const char *what, IdmValue input, IdmValue *out, IdmError *err) {
    IdmValue d[3] = { idm_atom(rt, "parse"), idm_atom(rt, what), input };
    IdmValue detail = idm_tuple(rt, d, 3u, err);
    if (err && err->present) return false;
    *out = idm_error_value(rt, detail);
    return !(err && err->present);
}

static bool parse_prelude(IdmRuntime *rt, const char *name, IdmValue arg, char **out_buf, IdmError *err) {
    const char *s; size_t len;
    if (!require_string_arg(rt, arg, &s, &len, name, err)) return false;
    if (len == 0 || isspace((unsigned char)s[0])) { *out_buf = NULL; return true; }
    char *buf = idm_strndup(s, len);
    if (!buf) return idm_error_oom(err, idm_span_unknown(NULL));
    *out_buf = buf;
    return true;
}

static bool prim_parse_int(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    char *buf = NULL;
    if (!parse_prelude(rt, "parse-int", args[0], &buf, err)) return false;
    if (!buf) return parse_result_error(rt, "int", args[0], out, err);
    errno = 0;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    bool ok = end == buf + idm_string_length(args[0]) && errno == 0;
    free(buf);
    if (!ok) return parse_result_error(rt, "int", args[0], out, err);
    return result_ok(rt, idm_int((int64_t)v), out, err);
}

static bool prim_parse_float(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    char *buf = NULL;
    if (!parse_prelude(rt, "parse-float", args[0], &buf, err)) return false;
    if (!buf) return parse_result_error(rt, "float", args[0], out, err);
    char *end = NULL;
    double v = strtod(buf, &end);
    bool ok = end == buf + idm_string_length(args[0]);
    free(buf);
    if (!ok) return parse_result_error(rt, "float", args[0], out, err);
    return result_ok(rt, idm_float(v), out, err);
}

static bool prim_file_mkdir(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-mkdir", err)) return false;
    char pb[PATH_MAX];
    if (!(path = resolve_cwd(path, pb, sizeof(pb)))) return result_error(rt, out, err);
    if (mkdir(path, 0777) != 0) return result_error(rt, out, err);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_file_append(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *path; size_t plen;
    const char *data; size_t dlen;
    if (!require_string_arg(rt, args[0], &path, &plen, "file-append", err)) return false;
    if (!require_string_arg(rt, args[1], &data, &dlen, "file-append", err)) return false;
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

static bool prim_ord_str(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    char buf[4];
    size_t n = args[0].tag == IDM_VAL_INT ? utf8_encode(args[0].as.i, buf) : 0u;
    if (n == 0u) return type_error(rt, err, "ord->str", args[0], "a Unicode codepoint");
    *out = idm_string_n(rt, buf, n, err);
    return !(err && err->present);
}

static bool prim_str_ord(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    const char *s; size_t len;
    if (!require_string_arg(rt, args[0], &s, &len, "str->ord", err)) return false;
    if (len == 0) { *out = idm_nil(); return true; }
    int32_t cp;
    utf8_decode((const unsigned char *)s, len, &cp);
    *out = idm_int((int64_t)cp);
    return true;
}

static bool prim_from_runes(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    IdmValue v = args[0];
    IdmBuffer buf;
    idm_buf_init(&buf);
    for (; idm_is_pair(v); v = idm_cdr(v, err)) {
        IdmValue item = idm_car(v, err);
        if (err && err->present) { idm_buf_destroy(&buf); return false; }
        char enc[4];
        size_t n = item.tag == IDM_VAL_INT ? utf8_encode(item.as.i, enc) : 0u;
        if (n == 0u) { idm_buf_destroy(&buf); return type_error(rt, err, "from-runes", item, "a Unicode codepoint"); }
        if (!idm_buf_append_n(&buf, enc, n)) { idm_buf_destroy(&buf); return idm_error_oom(err, idm_span_unknown(NULL)); }
    }
    if (!idm_is_empty_list(v)) { idm_buf_destroy(&buf); return type_error(rt, err, "from-runes", v, "a list of codepoints"); }
    *out = idm_string_n(rt, buf.data ? buf.data : "", buf.len, err);
    idm_buf_destroy(&buf);
    return !(err && err->present);
}

static bool require_session(IdmRuntime *rt, const char *name, IdmError *err) {
    if (rt->repl) return true;
    return idm_error_set(err, idm_span_unknown(NULL), "%s requires a session context", name);
}

static bool prim_repl_compile(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-compile", err)) return false;
    size_t len = 0;
    const char *source = require_string(args[0], &len, err);
    if (!source) return false;
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

static bool prim_repl_abort(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-abort", err)) return false;
    if (args[0].tag != IDM_VAL_INT) return type_error(rt, err, "repl-abort", args[0], "a compile token");
    idm_repl_abort(rt->repl, (uint64_t)args[0].as.i);
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_repl_spawn(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (!require_session(rt, "repl-spawn", err)) return false;
    if (!idm_is_closure(args[0])) return type_error(rt, err, "repl-spawn", args[0], "a function value");
    IdmScheduler *sched = idm_repl_scheduler(rt->repl);
    IdmValue pid = idm_nil();
    if (!idm_sched_spawn(sched, args[0], idm_current_exec(), &pid, err)) return false;
    idm_sched_watch(sched, pid.as.id);
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
    return v.tag == IDM_VAL_ATOM && strcmp(idm_symbol_text(v.as.symbol), name) == 0;
}

static bool prim_port_status(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "port-status", args[0], "a port");
    IdmScheduler *sched = job_sched("port-status", err);
    if (!sched) return false;
    int state = 0;
    if (!idm_sched_port_status(sched, args[0].as.id, &state)) return type_error(rt, err, "port-status", args[0], "a live port");
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

static bool prim_port_read(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "port-read", args[0], "a port");
    const char *stream = NULL;
    if (!port_stream_arg(args[1], &stream)) return type_error(rt, err, "port-read", args[1], ":stdout or :stderr");
    if (args[2].tag != IDM_VAL_INT || args[2].as.i <= 0) return type_error(rt, err, "port-read", args[2], "a positive byte count");
    IdmScheduler *sched = job_sched("port-read", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_read(sched, args[0].as.id, stream, (size_t)args[2].as.i, out, &found, err)) return false;
    if (!found) return type_error(rt, err, "port-read", args[0], "a live port");
    return true;
}

static bool prim_port_write(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "port-write", args[0], "a port");
    size_t len = 0;
    const char *data = require_string(args[1], &len, err);
    if (!data) return false;
    IdmScheduler *sched = job_sched("port-write", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_write(sched, args[0].as.id, data, len, out, &found, err)) return false;
    if (!found) return type_error(rt, err, "port-write", args[0], "a live port");
    return true;
}

static bool prim_port_close_input(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "port-close-input", args[0], "a port");
    IdmScheduler *sched = job_sched("port-close-input", err);
    if (!sched) return false;
    bool found = false;
    if (!idm_sched_port_close_input(sched, args[0].as.id, out, &found, err)) return false;
    if (!found) return type_error(rt, err, "port-close-input", args[0], "a live port");
    return true;
}

static bool prim_job_resume(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "job-resume", args[0], "a port");
    bool fg = job_arg_atom(args[1], "fg");
    if (!fg && !job_arg_atom(args[1], "bg")) return type_error(rt, err, "job-resume", args[1], ":fg or :bg");
    IdmScheduler *sched = job_sched("job-resume", err);
    if (!sched) return false;
    if (!idm_sched_job_resume(sched, args[0].as.id, fg)) return type_error(rt, err, "job-resume", args[0], "a live port");
    *out = idm_atom(rt, "ok");
    return true;
}

static bool prim_job_signal(IdmRuntime *rt, IdmValue *args, IdmValue *out, IdmError *err) {
    if (args[0].tag != IDM_VAL_PORT) return type_error(rt, err, "job-signal", args[0], "a port");
    int signo = job_arg_atom(args[1], "hup") ? SIGHUP
              : job_arg_atom(args[1], "cont") ? SIGCONT
              : job_arg_atom(args[1], "term") ? SIGTERM
              : job_arg_atom(args[1], "kill") ? SIGKILL
              : job_arg_atom(args[1], "int") ? SIGINT
              : 0;
    if (signo == 0) return type_error(rt, err, "job-signal", args[1], ":hup, :cont, :term, :kill, or :int");
    IdmScheduler *sched = job_sched("job-signal", err);
    if (!sched) return false;
    if (!idm_sched_job_signal(sched, args[0].as.id, signo)) return type_error(rt, err, "job-signal", args[0], "a live port");
    *out = idm_atom(rt, "ok");
    return true;
}

bool idm_prim_invoke(IdmRuntime *rt, IdmPrimitive prim, IdmValue *args, uint32_t argc, IdmValue *out, IdmError *err) {
    if (!require_arity(rt, prim, argc, err)) return false;
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
        case IDM_PRIM_STR_TO_LIST: return prim_str_to_list(rt, args, out, err);
        case IDM_PRIM_DICT_TO_LIST: return prim_dict_to_list(rt, args, out, err);
        case IDM_PRIM_VECTOR_TO_LIST: return prim_vector_to_list(rt, args, out, err);
        case IDM_PRIM_TUPLE_TO_LIST: return prim_tuple_to_list(rt, args, out, err);
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
        case IDM_PRIM_RANDOM: return prim_random(rt, args, out, err);
        case IDM_PRIM_DICT_GET: return prim_dict_get(rt, args, out, err);
        case IDM_PRIM_DICT_PUT: return prim_dict_put(rt, args, out, err);
        case IDM_PRIM_DICT_DEL: return prim_dict_del(rt, args, out, err);
        case IDM_PRIM_DICT_KEYS: return prim_dict_keys(rt, args, out, err);
        case IDM_PRIM_DICT_VALS: return prim_dict_vals(rt, args, out, err);
        case IDM_PRIM_DICT_HAS: return prim_dict_has(rt, args, out, err);
        case IDM_PRIM_DICT_SIZE: return prim_dict_size(rt, args, out, err);
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
        case IDM_PRIM_GTE: return prim_compare(rt, prim, args, out, err);
        case IDM_PRIM_OK: return prim_ok(rt, args, out, err);
        case IDM_PRIM_ERROR_MESSAGE: return prim_error_message(rt, args, out, err);
        case IDM_PRIM_MAKE_ERROR: return prim_make_error(rt, args, out, err);
        case IDM_PRIM_TRAIT_IMPLEMENTED_P: return prim_trait_implements(rt, args, out, err);
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
        case IDM_PRIM_EXEC:
        case IDM_PRIM_AWAIT:
        case IDM_PRIM_TTY_READ:
        case IDM_PRIM_TTY_READ_LINE:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive '%s' must be invoked under the actor scheduler", idm_primitive_name(prim));
        case IDM_PRIM_APPLY:
            return idm_error_set(err, idm_span_unknown(NULL), "primitive 'apply' is compiled directly and cannot be invoked generically");
        case IDM_PRIM_STR: return prim_str(rt, args, argc, out, err);
        case IDM_PRIM_CHOMP: return prim_chomp(rt, args, out, err);
        case IDM_PRIM_CAPTURE_STDOUT: return prim_capture_stdout(rt, args, out, err);
        case IDM_PRIM_PRINT: return prim_print_impl(rt, args, argc, false, out, err);
        case IDM_PRIM_PRINTLN: return prim_print_impl(rt, args, argc, true, out, err);
        case IDM_PRIM_CD: return prim_cd(rt, args, out, err);
        case IDM_PRIM_CHDIR: return prim_cd(rt, args, out, err);
        case IDM_PRIM_PWD: return prim_pwd(rt, args, out, err);
        case IDM_PRIM_ENV_GET: return prim_env_get(rt, args, out, err);
        case IDM_PRIM_ENV_SET: return prim_env_set(rt, args, out, err);
        case IDM_PRIM_WRITE_PROCSUB_TEMP: return prim_write_procsub_temp(rt, args, out, err);
        case IDM_PRIM_MAKE_PROCSUB_TEMP: return prim_make_procsub_temp(rt, args, out, err);
        case IDM_PRIM_MAKE_RECORD: return prim_make_record(rt, args, out, err, true);
        case IDM_PRIM_RECORD_NEW: return prim_make_record(rt, args, out, err, false);
        case IDM_PRIM_RECORD_PRED: return prim_record_pred(rt, args, out, err);
        case IDM_PRIM_RECORD_TYPE: return prim_record_type(rt, args, out, err);
        case IDM_PRIM_RECORD_FIELD: return prim_record_field(rt, args, out, err);
        case IDM_PRIM_ABS: return prim_abs(rt, args, out, err);
        case IDM_PRIM_FLOOR: return prim_float_unary(rt, "floor", floor, args, out, err);
        case IDM_PRIM_ROUND: return prim_float_unary(rt, "round", round, args, out, err);
        case IDM_PRIM_SQRT: return prim_sqrt(rt, args, out, err);
        case IDM_PRIM_FLOOR_DIV:
        case IDM_PRIM_FLOOR_MOD: return prim_floor_divmod(rt, prim, args, out, err);
        case IDM_PRIM_PARSE_INT: return prim_parse_int(rt, args, out, err);
        case IDM_PRIM_PARSE_FLOAT: return prim_parse_float(rt, args, out, err);
        case IDM_PRIM_FILE_MKDIR: return prim_file_mkdir(rt, args, out, err);
        case IDM_PRIM_FILE_APPEND: return prim_file_append(rt, args, out, err);
        case IDM_PRIM_ORD_STR: return prim_ord_str(rt, args, out, err);
        case IDM_PRIM_STR_ORD: return prim_str_ord(rt, args, out, err);
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
