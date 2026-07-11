#ifndef IDM_COMMON_H
#define IDM_COMMON_H

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NDEBUG
#define IDM_ASSERT_PROVED(expr) ((void)sizeof((expr) ? 1 : 0))
#else
#define IDM_ASSERT_PROVED(expr) assert(expr)
#endif

typedef struct {
    const char *file;
    size_t start;
    size_t end;
    unsigned line;
    unsigned column;
} IdmSpan;

typedef enum {
    IDM_DATUM_NIL,
    IDM_DATUM_WORD,
    IDM_DATUM_ATOM,
    IDM_DATUM_INT,
    IDM_DATUM_FLOAT,
    IDM_DATUM_STRING,
    IDM_DATUM_LIST,
    IDM_DATUM_VECTOR,
    IDM_DATUM_TUPLE,
    IDM_DATUM_DICT
} IdmDatumKind;

typedef struct {
    char *message;
    IdmSpan span;
} IdmErrorNote;

typedef struct {
    bool present;
    IdmSpan span;
    char *message;
    IdmErrorNote *notes;
    size_t note_count;
    size_t note_cap;
    void *reason;
} IdmError;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} IdmBuffer;

typedef struct {
    const char *name;
    uint64_t start_ns;
    bool active;
} IdmProfileScope;

typedef uint32_t IdmProfileMetricHandle;

typedef struct {
    void **base;
    size_t elem_size;
} IdmGrowItem;

typedef bool (*IdmSurfaceItemWriter)(IdmBuffer *buf, size_t index, void *user);

IdmSpan idm_span_unknown(const char *file);
const char *idm_datum_kind_name(IdmDatumKind kind);

void idm_error_init(IdmError *err);
void idm_error_clear(IdmError *err);
void idm_error_set_span(IdmError *err, IdmSpan span);
bool idm_error_set(IdmError *err, IdmSpan span, const char *fmt, ...);
bool idm_error_note(IdmError *err, const char *fmt, ...);
bool idm_error_note_at(IdmError *err, IdmSpan span, const char *fmt, ...);
bool idm_error_setv(IdmError *err, IdmSpan span, const char *fmt, va_list ap);
bool idm_error_oom(IdmError *err, IdmSpan span);
void idm_error_fprint(FILE *out, const IdmError *err);
bool idm_error_render(const IdmError *err, IdmBuffer *out);

void idm_buf_init(IdmBuffer *buf);
void idm_buf_destroy(IdmBuffer *buf);
bool idm_buf_append(IdmBuffer *buf, const char *text);
bool idm_buf_append_n(IdmBuffer *buf, const char *text, size_t len);
bool idm_buf_append_char(IdmBuffer *buf, char ch);
bool idm_buf_appendf(IdmBuffer *buf, const char *fmt, ...);
char *idm_buf_take(IdmBuffer *buf);
bool idm_surface_write_escaped(IdmBuffer *buf, const char *text, size_t len);
bool idm_buf_append_json_string(IdmBuffer *buf, const char *text, size_t len);
bool idm_surface_write_sequence(IdmBuffer *buf, const char *open, const char *close, size_t count, IdmSurfaceItemWriter item, void *user);

bool idm_buf_put_u8(IdmBuffer *buf, uint8_t v);
bool idm_buf_put_u32(IdmBuffer *buf, uint32_t v);
bool idm_buf_put_u64(IdmBuffer *buf, uint64_t v);
bool idm_buf_put_str(IdmBuffer *buf, const char *data, size_t len);
bool idm_buf_put_opt_str(IdmBuffer *buf, const char *text);

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
bool idm_rd_opt_str(IdmByteReader *r, char **out, IdmError *err);

void idm_sha256(const void *data, size_t len, unsigned char out[32]);
void idm_sha256_hex(const void *data, size_t len, char out[65]);

#define IDM_WIRE_MAGIC "IDMW"
#define IDM_WIRE_FORMAT 7u
#define IDM_WIRE_SECTION_BYTECODE 1u
#define IDM_WIRE_SECTION_PACKAGE 2u
#define IDM_WIRE_SECTION_MAIN 3u

uint32_t idm_wire_version(void);
bool idm_wire_begin(IdmBuffer *out, uint32_t section_count, IdmError *err);
bool idm_wire_section(IdmBuffer *out, uint32_t kind, const void *payload, size_t len, IdmError *err);
bool idm_wire_open(IdmByteReader *r, const unsigned char *data, size_t len, uint32_t *out_section_count, IdmError *err);
bool idm_wire_next(IdmByteReader *r, uint32_t *out_kind, const unsigned char **out_payload, size_t *out_len, IdmError *err);
bool idm_wire_find(const unsigned char *data, size_t len, uint32_t kind, const unsigned char **out_payload, size_t *out_len, IdmError *err);

char *idm_strdup(const char *s);
char *idm_strndup(const char *s, size_t n);
bool idm_next_capacity(size_t current, size_t seed, size_t needed, size_t *out);
bool idm_grow(void **base, size_t *cap, size_t elem_size, size_t seed, size_t needed);
bool idm_growv(IdmGrowItem *items, size_t count, size_t *cap, size_t seed, size_t needed);
bool idm_read_file(const char *path, char **out, size_t *out_len, IdmError *err);
bool idm_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IdmError *err);
const char *idm_root(void);

bool idm_profile_enabled(void);
uint64_t idm_profile_now_ns(void);
void idm_profile_scope_begin(IdmProfileScope *scope, const char *name);
void idm_profile_scope_end(IdmProfileScope *scope);
IdmProfileMetricHandle idm_profile_metric_handle(const char *name);
void idm_profile_count_handle(IdmProfileMetricHandle handle, uint64_t amount);
void idm_profile_count(const char *name, uint64_t amount);

#endif
