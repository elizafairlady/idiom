#ifndef IDM_COMMON_H
#define IDM_COMMON_H

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
} IdmSpan;

typedef struct {
    bool present;
    IdmSpan span;
    char *message;
    char *notes;
    void *reason;
} IdmError;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} IdmBuffer;

IdmSpan idm_span_unknown(const char *file);

void idm_error_init(IdmError *err);
void idm_error_clear(IdmError *err);
bool idm_error_set(IdmError *err, IdmSpan span, const char *fmt, ...);
bool idm_error_note(IdmError *err, const char *fmt, ...);
bool idm_error_setv(IdmError *err, IdmSpan span, const char *fmt, va_list ap);
bool idm_error_oom(IdmError *err, IdmSpan span);
void idm_error_fprint(FILE *out, const IdmError *err);
bool idm_error_render(const IdmError *err, IdmBuffer *out);

void idm_buf_init(IdmBuffer *buf);
void idm_buf_destroy(IdmBuffer *buf);
bool idm_buf_reserve(IdmBuffer *buf, size_t needed);
bool idm_buf_append(IdmBuffer *buf, const char *text);
bool idm_buf_append_n(IdmBuffer *buf, const char *text, size_t len);
bool idm_buf_append_char(IdmBuffer *buf, char ch);
bool idm_buf_appendf(IdmBuffer *buf, const char *fmt, ...);
char *idm_buf_take(IdmBuffer *buf);

bool idm_buf_put_u8(IdmBuffer *buf, uint8_t v);
bool idm_buf_put_u32(IdmBuffer *buf, uint32_t v);
bool idm_buf_put_u64(IdmBuffer *buf, uint64_t v);
bool idm_buf_put_str(IdmBuffer *buf, const char *data, size_t len);

#define IDM_IC_MAX_DEPTH 1024u

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
    bool ok;
} IdmByteReader;

void idm_byte_reader_init(IdmByteReader *r, const unsigned char *data, size_t len);
uint8_t idm_rd_u8(IdmByteReader *r);
uint32_t idm_rd_u32(IdmByteReader *r);
uint64_t idm_rd_u64(IdmByteReader *r);
char *idm_rd_string(IdmByteReader *r, size_t *out_len);

void idm_sha256(const void *data, size_t len, unsigned char out[32]);
void idm_sha256_hex(const void *data, size_t len, char out[65]);

char *idm_strdup(const char *s);
char *idm_strndup(const char *s, size_t n);
bool idm_read_file(const char *path, char **out, size_t *out_len, IdmError *err);
bool idm_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IdmError *err);

#endif
