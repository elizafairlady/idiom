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
    char *notes;
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
bool ish_error_note(IshError *err, const char *fmt, ...);
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

bool ish_buf_put_u8(IshBuffer *buf, uint8_t v);
bool ish_buf_put_u32(IshBuffer *buf, uint32_t v);
bool ish_buf_put_u64(IshBuffer *buf, uint64_t v);
bool ish_buf_put_str(IshBuffer *buf, const char *data, size_t len);

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
    bool ok;
} IshByteReader;

void ish_byte_reader_init(IshByteReader *r, const unsigned char *data, size_t len);
uint8_t ish_rd_u8(IshByteReader *r);
uint32_t ish_rd_u32(IshByteReader *r);
uint64_t ish_rd_u64(IshByteReader *r);
char *ish_rd_string(IshByteReader *r);

void ish_sha256(const void *data, size_t len, unsigned char out[32]);
void ish_sha256_hex(const void *data, size_t len, char out[65]);

char *ish_strdup(const char *s);
char *ish_strndup(const char *s, size_t n);
bool ish_read_file(const char *path, char **out, size_t *out_len, IshError *err);
bool ish_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IshError *err);

#endif
