#include "idiom/common.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *name;
    uint64_t calls;
    uint64_t total_ns;
    uint64_t count;
} IdmProfileMetric;

static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;
static IdmProfileMetric *profile_metrics = NULL;
static size_t profile_metric_count = 0;
static size_t profile_metric_cap = 0;
static int profile_state = -1;
static bool profile_report_registered = false;

const char *idm_datum_kind_name(IdmDatumKind kind) {
    static const char *const names[] = {
        [IDM_DATUM_NIL] = "nil",
        [IDM_DATUM_WORD] = "word",
        [IDM_DATUM_ATOM] = "atom",
        [IDM_DATUM_INT] = "int",
        [IDM_DATUM_FLOAT] = "float",
        [IDM_DATUM_STRING] = "string",
        [IDM_DATUM_LIST] = "list",
        [IDM_DATUM_VECTOR] = "vector",
        [IDM_DATUM_TUPLE] = "tuple",
        [IDM_DATUM_DICT] = "dict",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) && names[kind] ? names[kind] : "unknown";
}

static void idm_profile_report_stderr(void) {
    idm_profile_report(stderr);
}

bool idm_profile_enabled(void) {
    if (profile_state >= 0) return profile_state != 0;
    const char *v = getenv("IDIOMPROFILE");
    profile_state = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    if (profile_state && !profile_report_registered) {
        atexit(idm_profile_report_stderr);
        profile_report_registered = true;
    }
    return profile_state != 0;
}

uint64_t idm_profile_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static IdmProfileMetric *profile_metric_get(const char *name) {
    for (size_t i = 0; i < profile_metric_count; i++) {
        if (strcmp(profile_metrics[i].name, name) == 0) return &profile_metrics[i];
    }
    if (profile_metric_count == profile_metric_cap) {
        if (!idm_grow((void **)&profile_metrics, &profile_metric_cap, sizeof(*profile_metrics), 64u, profile_metric_count + 1u)) return NULL;
    }
    IdmProfileMetric *m = &profile_metrics[profile_metric_count];
    memset(m, 0, sizeof(*m));
    m->name = idm_strdup(name);
    if (!m->name) return NULL;
    profile_metric_count++;
    return m;
}

void idm_profile_scope_begin(IdmProfileScope *scope, const char *name) {
    if (!scope) return;
    scope->name = name;
    scope->active = false;
    scope->start_ns = 0;
    if (!idm_profile_enabled() || !name || !*name) return;
    scope->start_ns = idm_profile_now_ns();
    scope->active = true;
}

void idm_profile_scope_end(IdmProfileScope *scope) {
    if (!scope || !scope->active) return;
    uint64_t end_ns = idm_profile_now_ns();
    uint64_t elapsed = end_ns >= scope->start_ns ? end_ns - scope->start_ns : 0;
    pthread_mutex_lock(&profile_lock);
    IdmProfileMetric *m = profile_metric_get(scope->name);
    if (m) {
        m->calls++;
        m->total_ns += elapsed;
    }
    pthread_mutex_unlock(&profile_lock);
    scope->active = false;
}

void idm_profile_count(const char *name, uint64_t amount) {
    if (!idm_profile_enabled() || !name || !*name) return;
    pthread_mutex_lock(&profile_lock);
    IdmProfileMetric *m = profile_metric_get(name);
    if (m) m->count += amount;
    pthread_mutex_unlock(&profile_lock);
}

void idm_profile_report(FILE *out) {
    if (!out || !idm_profile_enabled()) return;
    pthread_mutex_lock(&profile_lock);
    for (size_t i = 0; i < profile_metric_count; i++) {
        const IdmProfileMetric *m = &profile_metrics[i];
        uint64_t avg = m->calls ? m->total_ns / m->calls : 0;
        fprintf(out, "idiom-profile\t%s\tcalls\t%llu\ttotal_ns\t%llu\tavg_ns\t%llu\tcount\t%llu\n",
                m->name ? m->name : "",
                (unsigned long long)m->calls,
                (unsigned long long)m->total_ns,
                (unsigned long long)avg,
                (unsigned long long)m->count);
    }
    pthread_mutex_unlock(&profile_lock);
}

IdmSpan idm_span_unknown(const char *file) {
    IdmSpan span;
    span.file = file;
    span.start = 0;
    span.end = 0;
    span.line = 0;
    span.column = 0;
    return span;
}

void idm_error_init(IdmError *err) {
    if (!err) return;
    err->present = false;
    err->span = idm_span_unknown(NULL);
    err->message = NULL;
    err->notes = NULL;
    err->reason = NULL;
}

void idm_error_clear(IdmError *err) {
    if (!err) return;
    free(err->message);
    free(err->notes);
    free(err->reason);
    err->present = false;
    err->span = idm_span_unknown(NULL);
    err->message = NULL;
    err->notes = NULL;
    err->reason = NULL;
}

bool idm_error_setv(IdmError *err, IdmSpan span, const char *fmt, va_list ap) {
    if (!err) return false;
    idm_error_clear(err);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        err->message = idm_strdup("failed to format error message");
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

bool idm_error_set(IdmError *err, IdmSpan span, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool result = idm_error_setv(err, span, fmt, ap);
    va_end(ap);
    return result;
}

bool idm_error_oom(IdmError *err, IdmSpan span) {
    return idm_error_set(err, span, "out of memory");
}

bool idm_error_note(IdmError *err, const char *fmt, ...) {
    if (!err || !err->present) return false;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t old_len = err->notes ? strlen(err->notes) : 0u;
    size_t add_len = strlen(line);
    if (old_len > SIZE_MAX - add_len || old_len + add_len > SIZE_MAX - 4u) return false;
    size_t cap = old_len;
    size_t needed = old_len + add_len + 4u;
    if (!idm_grow((void **)&err->notes, &cap, sizeof(*err->notes), needed, needed)) return false;
    memcpy(err->notes + old_len, "\n  ", 3u);
    memcpy(err->notes + old_len + 3u, line, add_len + 1u);
    return false;
}

void idm_error_fprint(FILE *out, const IdmError *err) {
    if (!err || !err->present) return;
    const char *file = err->span.file ? err->span.file : "<unknown>";
    if (err->span.line != 0) {
        fprintf(out, "%s:%u:%u: error: %s%s\n", file, err->span.line, err->span.column, err->message ? err->message : "error", err->notes ? err->notes : "");
    } else {
        fprintf(out, "%s: error: %s%s\n", file, err->message ? err->message : "error", err->notes ? err->notes : "");
    }
}

bool idm_error_render(const IdmError *err, IdmBuffer *out) {
    if (!err || !err->present) return false;
    const char *file = err->span.file ? err->span.file : "<unknown>";
    if (err->span.line != 0) {
        return idm_buf_appendf(out, "%s:%u:%u: error: %s%s", file, err->span.line, err->span.column, err->message ? err->message : "error", err->notes ? err->notes : "");
    }
    return idm_buf_appendf(out, "%s: error: %s%s", file, err->message ? err->message : "error", err->notes ? err->notes : "");
}

void idm_buf_init(IdmBuffer *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void idm_buf_destroy(IdmBuffer *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

bool idm_buf_reserve(IdmBuffer *buf, size_t needed) {
    return idm_grow((void **)&buf->data, &buf->cap, sizeof(*buf->data), 64u, needed);
}

bool idm_buf_append_n(IdmBuffer *buf, const char *text, size_t len) {
    if (!idm_buf_reserve(buf, buf->len + len + 1u)) return false;
    if (len != 0) memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

bool idm_buf_append(IdmBuffer *buf, const char *text) {
    return idm_buf_append_n(buf, text, strlen(text));
}

bool idm_buf_append_char(IdmBuffer *buf, char ch) {
    return idm_buf_append_n(buf, &ch, 1u);
}

bool idm_buf_appendf(IdmBuffer *buf, const char *fmt, ...) {
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
    if (!idm_buf_reserve(buf, start + (size_t)needed + 1u)) {
        va_end(ap);
        return false;
    }
    vsnprintf(buf->data + start, (size_t)needed + 1u, fmt, ap);
    va_end(ap);
    buf->len = start + (size_t)needed;
    return true;
}

char *idm_buf_take(IdmBuffer *buf) {
    if (!buf->data) {
        char *empty = idm_strdup("");
        return empty;
    }
    char *data = buf->data;
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    return data;
}

bool idm_surface_write_escaped(IdmBuffer *buf, const char *text, size_t len) {
    if (!idm_buf_append_char(buf, '"')) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        switch (ch) {
            case '\\': if (!idm_buf_append(buf, "\\\\")) return false; break;
            case '"': if (!idm_buf_append(buf, "\\\"")) return false; break;
            case '\n': if (!idm_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!idm_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!idm_buf_append(buf, "\\t")) return false; break;
            default:
                if (ch < 32u) {
                    if (!idm_buf_appendf(buf, "\\x%02x", ch)) return false;
                } else if (!idm_buf_append_char(buf, (char)ch)) return false;
        }
    }
    return idm_buf_append_char(buf, '"');
}

bool idm_surface_write_sequence(IdmBuffer *buf, const char *open, const char *close, size_t count, IdmSurfaceItemWriter item, void *user) {
    if (!idm_buf_append(buf, open)) return false;
    for (size_t i = 0; i < count; i++) {
        if (i != 0 && !idm_buf_append_char(buf, ' ')) return false;
        if (!item(buf, i, user)) return false;
    }
    return idm_buf_append(buf, close);
}

bool idm_buf_put_u8(IdmBuffer *buf, uint8_t v) {
    char c = (char)v;
    return idm_buf_append_n(buf, &c, 1u);
}

bool idm_buf_put_u32(IdmBuffer *buf, uint32_t v) {
    char t[4];
    t[0] = (char)(v >> 24);
    t[1] = (char)(v >> 16);
    t[2] = (char)(v >> 8);
    t[3] = (char)v;
    return idm_buf_append_n(buf, t, 4u);
}

bool idm_buf_put_u64(IdmBuffer *buf, uint64_t v) {
    char t[8];
    for (int i = 0; i < 8; i++) t[i] = (char)(v >> (56 - 8 * i));
    return idm_buf_append_n(buf, t, 8u);
}

bool idm_buf_put_str(IdmBuffer *buf, const char *data, size_t len) {
    if (len > UINT32_MAX) return false;
    if (!idm_buf_put_u32(buf, (uint32_t)len)) return false;
    return len == 0 ? true : idm_buf_append_n(buf, data, len);
}

bool idm_buf_put_opt_str(IdmBuffer *buf, const char *text) {
    if (!idm_buf_put_u8(buf, text ? 1u : 0u)) return false;
    return text ? idm_buf_put_str(buf, text, strlen(text)) : true;
}

void idm_byte_reader_init(IdmByteReader *r, const unsigned char *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->ok = true;
}

uint8_t idm_rd_u8(IdmByteReader *r) {
    if (r->pos + 1u > r->len) {
        r->ok = false;
        return 0;
    }
    return r->data[r->pos++];
}

uint32_t idm_rd_u32(IdmByteReader *r) {
    if (r->pos + 4u > r->len) {
        r->ok = false;
        return 0;
    }
    uint32_t v = ((uint32_t)r->data[r->pos] << 24) | ((uint32_t)r->data[r->pos + 1u] << 16) | ((uint32_t)r->data[r->pos + 2u] << 8) | (uint32_t)r->data[r->pos + 3u];
    r->pos += 4u;
    return v;
}

uint64_t idm_rd_u64(IdmByteReader *r) {
    if (r->pos + 8u > r->len) {
        r->ok = false;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | r->data[r->pos + (size_t)i];
    r->pos += 8u;
    return v;
}

char *idm_rd_string(IdmByteReader *r, size_t *out_len) {
    uint32_t n = idm_rd_u32(r);
    if (!r->ok || r->pos + n > r->len) {
        r->ok = false;
        return NULL;
    }
    char *s = malloc((size_t)n + 1u);
    if (!s) {
        r->ok = false;
        return NULL;
    }
    memcpy(s, r->data + r->pos, n);
    s[n] = '\0';
    r->pos += n;
    if (out_len) *out_len = n;
    return s;
}

bool idm_rd_opt_str(IdmByteReader *r, char **out, IdmError *err) {
    *out = NULL;
    uint8_t has = idm_rd_u8(r);
    if (!r->ok) return idm_error_set(err, idm_span_unknown(NULL), "truncated optional string");
    if (has > 1u) {
        r->ok = false;
        return idm_error_set(err, idm_span_unknown(NULL), "invalid optional string flag");
    }
    if (!has) return true;
    char *text = idm_rd_string(r, NULL);
    if (!text) return idm_error_set(err, idm_span_unknown(NULL), "truncated optional string");
    *out = text;
    return true;
}

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t state[8], const unsigned char block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROTR(w[i - 15], 7) ^ SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA256_ROTR(w[i - 2], 17) ^ SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void idm_sha256(const void *data, size_t len, unsigned char out[32]) {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    const unsigned char *p = data;
    size_t remaining = len;
    while (remaining >= 64u) {
        sha256_block(state, p);
        p += 64u;
        remaining -= 64u;
    }
    unsigned char tail[128];
    memset(tail, 0, sizeof(tail));
    if (remaining != 0) memcpy(tail, p, remaining);
    tail[remaining] = 0x80u;
    size_t tail_len = remaining + 1u <= 56u ? 64u : 128u;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) tail[tail_len - 8u + (size_t)i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_block(state, tail);
    if (tail_len == 128u) sha256_block(state, tail + 64u);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)state[i];
    }
}

void idm_sha256_hex(const void *data, size_t len, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    idm_sha256(data, len, digest);
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

char *idm_strdup(const char *s) {
    return idm_strndup(s, strlen(s));
}

char *idm_strndup(const char *s, size_t n) {
    char *copy = malloc(n + 1u);
    if (!copy) return NULL;
    if (n != 0) memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

bool idm_next_capacity(size_t current, size_t seed, size_t needed, size_t *out) {
    if (!out) return false;
    if (needed <= current) {
        *out = current;
        return true;
    }
    size_t next = current ? current : (seed ? seed : 1u);
    while (next < needed) {
        if (next > SIZE_MAX / 2u) return false;
        next *= 2u;
    }
    *out = next;
    return true;
}

bool idm_growv(IdmGrowItem *items, size_t count, size_t *cap, size_t seed, size_t needed) {
    if (!items || count == 0 || !cap) return false;
    if (needed <= *cap) return true;
    size_t next = 0;
    if (!idm_next_capacity(*cap, seed, needed, &next)) return false;
    for (size_t i = 0; i < count; i++) {
        if (!items[i].base || items[i].elem_size == 0 || next > SIZE_MAX / items[i].elem_size) return false;
    }
    for (size_t i = 0; i < count; i++) {
        void *grown = realloc(*items[i].base, next * items[i].elem_size);
        if (!grown) return false;
        *items[i].base = grown;
    }
    *cap = next;
    return true;
}

bool idm_grow(void **base, size_t *cap, size_t elem_size, size_t seed, size_t needed) {
    IdmGrowItem item = { .base = base, .elem_size = elem_size };
    return idm_growv(&item, 1u, cap, seed, needed);
}

bool idm_read_stream(FILE *stream, const char *name, char **out, size_t *out_len, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "io.read_stream");
    IdmBuffer buf;
    idm_buf_init(&buf);
    char chunk[4096];
    for (;;) {
        size_t n = fread(chunk, 1u, sizeof(chunk), stream);
        if (n != 0 && !idm_buf_append_n(&buf, chunk, n)) {
            idm_buf_destroy(&buf);
            idm_profile_scope_end(&prof);
            return idm_error_oom(err, idm_span_unknown(name));
        }
        if (n < sizeof(chunk)) {
            if (ferror(stream)) {
                idm_buf_destroy(&buf);
                idm_profile_scope_end(&prof);
                return idm_error_set(err, idm_span_unknown(name), "read failed: %s", strerror(errno));
            }
            break;
        }
    }
    size_t len = buf.len;
    *out = idm_buf_take(&buf);
    if (!*out) {
        idm_profile_scope_end(&prof);
        return idm_error_oom(err, idm_span_unknown(name));
    }
    if (out_len) *out_len = len;
    idm_profile_count("io.read_bytes", (uint64_t)len);
    idm_profile_scope_end(&prof);
    return true;
}

bool idm_read_file(const char *path, char **out, size_t *out_len, IdmError *err) {
    IdmProfileScope prof;
    idm_profile_scope_begin(&prof, "io.read_file");
    FILE *f = fopen(path, "rb");
    if (!f) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(path), "open failed: %s", strerror(errno));
    }
    bool ok = idm_read_stream(f, path, out, out_len, err);
    if (fclose(f) != 0 && ok) {
        idm_profile_scope_end(&prof);
        return idm_error_set(err, idm_span_unknown(path), "close failed: %s", strerror(errno));
    }
    idm_profile_scope_end(&prof);
    return ok;
}
