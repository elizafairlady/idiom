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
    err->notes = NULL;
}

void ish_error_clear(IshError *err) {
    if (!err) return;
    free(err->message);
    free(err->notes);
    err->present = false;
    err->span = ish_span_unknown(NULL);
    err->message = NULL;
    err->notes = NULL;
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

bool ish_error_note(IshError *err, const char *fmt, ...) {
    if (!err || !err->present) return false;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t old_len = err->notes ? strlen(err->notes) : 0u;
    size_t add_len = strlen(line);
    char *grown = realloc(err->notes, old_len + add_len + 8u);
    if (!grown) return false;
    err->notes = grown;
    memcpy(err->notes + old_len, "\n  ", 3u);
    memcpy(err->notes + old_len + 3u, line, add_len + 1u);
    return false;
}

void ish_error_fprint(FILE *out, const IshError *err) {
    if (!err || !err->present) return;
    const char *file = err->span.file ? err->span.file : "<unknown>";
    if (err->span.line != 0) {
        fprintf(out, "%s:%u:%u: error: %s%s\n", file, err->span.line, err->span.column, err->message ? err->message : "error", err->notes ? err->notes : "");
    } else {
        fprintf(out, "%s: error: %s%s\n", file, err->message ? err->message : "error", err->notes ? err->notes : "");
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

bool ish_buf_put_u8(IshBuffer *buf, uint8_t v) {
    char c = (char)v;
    return ish_buf_append_n(buf, &c, 1u);
}

bool ish_buf_put_u32(IshBuffer *buf, uint32_t v) {
    char t[4];
    t[0] = (char)(v >> 24);
    t[1] = (char)(v >> 16);
    t[2] = (char)(v >> 8);
    t[3] = (char)v;
    return ish_buf_append_n(buf, t, 4u);
}

bool ish_buf_put_u64(IshBuffer *buf, uint64_t v) {
    char t[8];
    for (int i = 0; i < 8; i++) t[i] = (char)(v >> (56 - 8 * i));
    return ish_buf_append_n(buf, t, 8u);
}

bool ish_buf_put_str(IshBuffer *buf, const char *data, size_t len) {
    if (len > UINT32_MAX) return false;
    if (!ish_buf_put_u32(buf, (uint32_t)len)) return false;
    return len == 0 ? true : ish_buf_append_n(buf, data, len);
}

void ish_byte_reader_init(IshByteReader *r, const unsigned char *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->ok = true;
}

uint8_t ish_rd_u8(IshByteReader *r) {
    if (r->pos + 1u > r->len) {
        r->ok = false;
        return 0;
    }
    return r->data[r->pos++];
}

uint32_t ish_rd_u32(IshByteReader *r) {
    if (r->pos + 4u > r->len) {
        r->ok = false;
        return 0;
    }
    uint32_t v = ((uint32_t)r->data[r->pos] << 24) | ((uint32_t)r->data[r->pos + 1u] << 16) | ((uint32_t)r->data[r->pos + 2u] << 8) | (uint32_t)r->data[r->pos + 3u];
    r->pos += 4u;
    return v;
}

uint64_t ish_rd_u64(IshByteReader *r) {
    if (r->pos + 8u > r->len) {
        r->ok = false;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | r->data[r->pos + (size_t)i];
    r->pos += 8u;
    return v;
}

char *ish_rd_string(IshByteReader *r) {
    uint32_t n = ish_rd_u32(r);
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
    return s;
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

void ish_sha256(const void *data, size_t len, unsigned char out[32]) {
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

void ish_sha256_hex(const void *data, size_t len, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    ish_sha256(data, len, digest);
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0fu];
    }
    out[64] = '\0';
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
    size_t len = buf.len;
    *out = ish_buf_take(&buf);
    if (!*out) return ish_error_oom(err, ish_span_unknown(name));
    if (out_len) *out_len = len;
    return true;
}

bool ish_read_file(const char *path, char **out, size_t *out_len, IshError *err) {
    FILE *f = fopen(path, "rb");
    if (!f) return ish_error_set(err, ish_span_unknown(path), "open failed: %s", strerror(errno));
    bool ok = ish_read_stream(f, path, out, out_len, err);
    if (fclose(f) != 0 && ok) return ish_error_set(err, ish_span_unknown(path), "close failed: %s", strerror(errno));
    return ok;
}
