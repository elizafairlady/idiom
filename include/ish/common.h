#ifndef ISH_COMMON_H
#define ISH_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *file;
    size_t start;
    size_t end;
    unsigned line;
    unsigned column;
} IshSpan;

typedef struct {
    bool present;
    IshSpan span;
    char *message;
} IshError;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} IshBuffer;

IshSpan ish_span_unknown(const char *file);

void ish_error_init(IshError *err);
void ish_error_clear(IshError *err);
bool ish_error_set(IshError *err, IshSpan span, const char *fmt, ...);
bool ish_error_setv(IshError *err, IshSpan span, const char *fmt, va_list ap);
bool ish_error_oom(IshError *err, IshSpan span);
void ish_error_fprint(FILE *out, const IshError *err);

void ish_buf_init(IshBuffer *buf);
void ish_buf_destroy(IshBuffer *buf);
bool ish_buf_reserve(IshBuffer *buf, size_t needed);
bool ish_buf_append(IshBuffer *buf, const char *text);
bool ish_buf_append_n(IshBuffer *buf, const char *text, size_t len);
bool ish_buf_append_char(IshBuffer *buf, char ch);
bool ish_buf_appendf(IshBuffer *buf, const char *fmt, ...);
char *ish_buf_take(IshBuffer *buf);

char *ish_strdup(const char *s);
char *ish_strndup(const char *s, size_t n);
bool ish_read_file(const char *path, char **out, size_t *out_len, IshError *err);
bool ish_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IshError *err);

#endif
