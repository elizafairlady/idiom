#include "ish/common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

IshSpan ish_span_unknown(const char *file) {
    IshSpan span;
    span.file = file;
    span.start = 0;
    span.end = 0;
    span.line = 0;
    span.column = 0;
    return span;
}

void ish_error_init(IshError *err) {
    if (!err) return;
    err->present = false;
    err->span = ish_span_unknown(NULL);
    err->message = NULL;
}

void ish_error_clear(IshError *err) {
    if (!err) return;
    free(err->message);
    err->present = false;
    err->span = ish_span_unknown(NULL);
    err->message = NULL;
}

bool ish_error_setv(IshError *err, IshSpan span, const char *fmt, va_list ap) {
    if (!err) return false;
    ish_error_clear(err);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        err->message = ish_strdup("failed to format error message");
        err->present = true;
        err->span = span;
        return false;
    }
    err->message = malloc((size_t)needed + 1u);
    if (!err->message) {
        err->message = NULL;
        err->present = true;
        err->span = span;
        return false;
    }
    vsnprintf(err->message, (size_t)needed + 1u, fmt, ap);
    err->present = true;
    err->span = span;
    return false;
}

bool ish_error_set(IshError *err, IshSpan span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool result = ish_error_setv(err, span, fmt, ap);
    va_end(ap);
    return result;
}

bool ish_error_oom(IshError *err, IshSpan span) {
    return ish_error_set(err, span, "out of memory");
}

void ish_error_fprint(FILE *out, const IshError *err) {
    if (!err || !err->present) return;
    const char *file = err->span.file ? err->span.file : "<unknown>";
    if (err->span.line != 0) {
        fprintf(out, "%s:%u:%u: error: %s\n", file, err->span.line, err->span.column, err->message ? err->message : "error");
    } else {
        fprintf(out, "%s: error: %s\n", file, err->message ? err->message : "error");
    }
}

void ish_buf_init(IshBuffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void ish_buf_destroy(IshBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

bool ish_buf_reserve(IshBuffer *buf, size_t needed) {
    if (needed <= buf->cap) return true;
    size_t cap = buf->cap ? buf->cap : 64u;
    while (cap < needed) {
        if (cap > (SIZE_MAX / 2u)) return false;
        cap *= 2u;
    }
    char *next = realloc(buf->data, cap);
    if (!next) return false;
    buf->data = next;
    buf->cap = cap;
    return true;
}

bool ish_buf_append_n(IshBuffer *buf, const char *text, size_t len) {
    if (!ish_buf_reserve(buf, buf->len + len + 1u)) return false;
    if (len != 0) memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

bool ish_buf_append(IshBuffer *buf, const char *text) {
    return ish_buf_append_n(buf, text, strlen(text));
}

bool ish_buf_append_char(IshBuffer *buf, char ch) {
    return ish_buf_append_n(buf, &ch, 1u);
}

bool ish_buf_appendf(IshBuffer *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return false;
    }
    size_t start = buf->len;
    if (!ish_buf_reserve(buf, start + (size_t)needed + 1u)) {
        va_end(ap);
        return false;
    }
    vsnprintf(buf->data + start, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    buf->len = start + (size_t)needed;
    return true;
}

char *ish_buf_take(IshBuffer *buf) {
    if (!buf->data) {
        char *empty = ish_strdup("");
        return empty;
    }
    char *data = buf->data;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return data;
}

char *ish_strdup(const char *s) {
    return ish_strndup(s, strlen(s));
}

char *ish_strndup(const char *s, size_t n) {
    char *copy = malloc(n + 1u);
    if (!copy) return NULL;
    if (n != 0) memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

bool ish_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IshError *err) {
    IshBuffer buf;
    ish_buf_init(&buf);
    char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1u, sizeof(chunk), stream);
        if (n != 0 && !ish_buf_append_n(&buf, chunk, n)) {
            ish_buf_destroy(&buf);
            return ish_error_oom(err, ish_span_unknown(name));
        }
        if (n < sizeof(chunk)) {
            if (ferror(stream)) {
                ish_buf_destroy(&buf);
                return ish_error_set(err, ish_span_unknown(name), "read failed: %s", strerror(errno));
            }
            break;
        }
    }
    *out = ish_buf_take(&buf);
    if (!*out) return ish_error_oom(err, ish_span_unknown(name));
    if (out_len) *out_len = strlen(*out);
    return true;
}

bool ish_read_file(const char *path, char **out, size_t *out_len, IshError *err) {
    FILE *f = fopen(path, "rb");
    if (!f) return ish_error_set(err, ish_span_unknown(path), "open failed: %s", strerror(errno));
    bool ok = ish_read_stream(f, path, out, out_len, err);
    if (fclose(f) != 0 && ok) return ish_error_set(err, ish_span_unknown(path), "close failed: %s", strerror(errno));
    return ok;
}
