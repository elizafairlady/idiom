#include "idiom/bignum.h"

#include <stdlib.h>
#include <string.h>

static size_t mag_normalize(const uint32_t *a, size_t count) {
    while (count > 0u && a[count - 1u] == 0u) count--;
    return count;
}

static int mag_cmp(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    if (na != nb) return na < nb ? -1 : 1;
    for (size_t i = na; i > 0u; i--) {
        if (a[i - 1u] != b[i - 1u]) return a[i - 1u] < b[i - 1u] ? -1 : 1;
    }
    return 0;
}

static size_t mag_add(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
    size_t n = na > nb ? na : nb;
    uint64_t carry = 0u;
    for (size_t i = 0u; i < n; i++) {
        uint64_t s = carry + (i < na ? a[i] : 0u) + (i < nb ? b[i] : 0u);
        out[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry) out[n++] = (uint32_t)carry;
    return n;
}

static size_t mag_sub(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
    int64_t borrow = 0;
    for (size_t i = 0u; i < na; i++) {
        int64_t d = (int64_t)a[i] - (i < nb ? (int64_t)b[i] : 0) - borrow;
        if (d < 0) { d += ((int64_t)1 << 32); borrow = 1; } else borrow = 0;
        out[i] = (uint32_t)d;
    }
    return mag_normalize(out, na);
}

static size_t mag_mul(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
    for (size_t i = 0u; i < na + nb; i++) out[i] = 0u;
    for (size_t i = 0u; i < na; i++) {
        uint64_t carry = 0u;
        for (size_t j = 0u; j < nb; j++) {
            uint64_t s = (uint64_t)a[i] * b[j] + out[i + j] + carry;
            out[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        out[i + nb] = (uint32_t)carry;
    }
    return mag_normalize(out, na + nb);
}

static unsigned mag_clz32(uint32_t v) {
    unsigned n = 0u;
    if (v == 0u) return 32u;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static bool mag_divmod(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *q, size_t *q_count, uint32_t *rem, size_t *r_count) {
    for (size_t i = 0u; i < na; i++) q[i] = 0u;
    if (nb == 1u) {
        uint64_t d = b[0];
        uint64_t r = 0u;
        for (size_t i = na; i-- > 0u;) {
            uint64_t acc = (r << 32) | a[i];
            q[i] = (uint32_t)(acc / d);
            r = acc % d;
        }
        rem[0] = (uint32_t)r;
        *q_count = mag_normalize(q, na);
        *r_count = mag_normalize(rem, 1u);
        return true;
    }
    if (na > SIZE_MAX / sizeof(uint32_t) - 1u) return false;
    uint32_t *u = malloc((na + 1u) * sizeof(uint32_t));
    uint32_t *v = malloc(nb * sizeof(uint32_t));
    if (!u || !v) {
        free(u);
        free(v);
        return false;
    }
    unsigned s = mag_clz32(b[nb - 1u]);
    if (s == 0u) {
        memcpy(v, b, nb * sizeof(uint32_t));
        memcpy(u, a, na * sizeof(uint32_t));
        u[na] = 0u;
    } else {
        for (size_t i = nb; i-- > 1u;) v[i] = (b[i] << s) | (b[i - 1u] >> (32u - s));
        v[0] = b[0] << s;
        u[na] = a[na - 1u] >> (32u - s);
        for (size_t i = na; i-- > 1u;) u[i] = (a[i] << s) | (a[i - 1u] >> (32u - s));
        u[0] = a[0] << s;
    }
    for (size_t j = na - nb + 1u; j-- > 0u;) {
        uint64_t num = ((uint64_t)u[j + nb] << 32) | u[j + nb - 1u];
        uint64_t qhat = num / v[nb - 1u];
        uint64_t rhat = num % v[nb - 1u];
        while (qhat > 0xFFFFFFFFu || qhat * v[nb - 2u] > ((rhat << 32) | u[j + nb - 2u])) {
            qhat--;
            rhat += v[nb - 1u];
            if (rhat > 0xFFFFFFFFu) break;
        }
        int64_t k = 0;
        for (size_t i = 0u; i < nb; i++) {
            uint64_t p = qhat * v[i];
            int64_t t = (int64_t)u[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            u[i + j] = (uint32_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        int64_t t = (int64_t)u[j + nb] - k;
        u[j + nb] = (uint32_t)t;
        if (t < 0) {
            qhat--;
            uint64_t carry = 0u;
            for (size_t i = 0u; i < nb; i++) {
                uint64_t sum = (uint64_t)u[i + j] + v[i] + carry;
                u[i + j] = (uint32_t)sum;
                carry = sum >> 32;
            }
            u[j + nb] = (uint32_t)((uint64_t)u[j + nb] + carry);
        }
        q[j] = (uint32_t)qhat;
    }
    if (s == 0u) {
        memcpy(rem, u, nb * sizeof(uint32_t));
    } else {
        for (size_t i = 0u; i + 1u < nb; i++) rem[i] = (u[i] >> s) | (u[i + 1u] << (32u - s));
        rem[nb - 1u] = u[nb - 1u] >> s;
    }
    free(u);
    free(v);
    *q_count = mag_normalize(q, na);
    *r_count = mag_normalize(rem, nb);
    return true;
}

size_t idm_bignum_decimal_limb_cap(size_t digit_len) {
    return digit_len / 9u + 2u;
}

size_t idm_bignum_decimal_digit_cap(size_t limb_count) {
    if (limb_count > (SIZE_MAX - 1u) / 10u) return SIZE_MAX;
    return limb_count * 10u + 1u;
}

size_t idm_bignum_from_i64(int64_t value, uint32_t *out, int *out_sign) {
    uint64_t mag = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    *out_sign = value < 0 ? -1 : (value > 0 ? 1 : 0);
    out[0] = (uint32_t)mag;
    out[1] = (uint32_t)(mag >> 32);
    return mag_normalize(out, 2u);
}

bool idm_bignum_from_decimal(const char *text, size_t len, uint32_t *out, size_t *out_count, int *out_sign) {
    size_t i = 0u;
    int sign = 1;
    if (i < len && (text[i] == '+' || text[i] == '-')) { sign = text[i] == '-' ? -1 : 1; i++; }
    if (i >= len) return false;
    size_t count = 0u;
    for (; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        uint64_t carry = (uint64_t)(text[i] - '0');
        for (size_t j = 0u; j < count; j++) {
            uint64_t s = (uint64_t)out[j] * 10u + carry;
            out[j] = (uint32_t)s;
            carry = s >> 32;
        }
        if (carry) out[count++] = (uint32_t)carry;
    }
    count = mag_normalize(out, count);
    *out_count = count;
    *out_sign = count == 0u ? 0 : sign;
    return true;
}

bool idm_bignum_decimal_equal(const char *a, size_t na, const char *b, size_t nb, bool *out) {
    size_t ca = idm_bignum_decimal_limb_cap(na);
    size_t cb = idm_bignum_decimal_limb_cap(nb);
    if (ca > SIZE_MAX / sizeof(uint32_t) || cb > SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t *la = malloc(ca * sizeof(uint32_t));
    uint32_t *lb = malloc(cb * sizeof(uint32_t));
    if (!la || !lb) { free(la); free(lb); return false; }
    size_t la_count = 0u, lb_count = 0u;
    int sa = 0, sb = 0;
    bool ok = idm_bignum_from_decimal(a, na, la, &la_count, &sa) && idm_bignum_from_decimal(b, nb, lb, &lb_count, &sb);
    if (ok) *out = idm_bignum_cmp(la, la_count, sa, lb, lb_count, sb) == 0;
    free(la);
    free(lb);
    return ok;
}

int idm_bignum_cmp(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb) {
    if (sa != sb) return sa < sb ? -1 : 1;
    if (sa == 0) return 0;
    int m = mag_cmp(a, na, b, nb);
    return sa < 0 ? -m : m;
}

static void signed_add(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign) {
    if (sa == 0) { for (size_t i = 0u; i < nb; i++) out[i] = b[i]; *out_count = nb; *out_sign = sb; return; }
    if (sb == 0) { for (size_t i = 0u; i < na; i++) out[i] = a[i]; *out_count = na; *out_sign = sa; return; }
    if (sa == sb) {
        *out_count = mag_add(a, na, b, nb, out);
        *out_sign = sa;
        return;
    }
    int m = mag_cmp(a, na, b, nb);
    if (m == 0) { *out_count = 0u; *out_sign = 0; return; }
    if (m > 0) { *out_count = mag_sub(a, na, b, nb, out); *out_sign = sa; }
    else { *out_count = mag_sub(b, nb, a, na, out); *out_sign = sb; }
}

void idm_bignum_add(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign) {
    signed_add(a, na, sa, b, nb, sb, out, out_count, out_sign);
}

void idm_bignum_sub(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign) {
    signed_add(a, na, sa, b, nb, -sb, out, out_count, out_sign);
}

void idm_bignum_mul(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign) {
    if (sa == 0 || sb == 0) { *out_count = 0u; *out_sign = 0; return; }
    *out_count = mag_mul(a, na, b, nb, out);
    *out_sign = sa == sb ? 1 : -1;
}

bool idm_bignum_divmod(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *q, size_t *q_count, int *q_sign, uint32_t *r, size_t *r_count, int *r_sign) {
    if (sb == 0) return false;
    if (sa == 0 || mag_cmp(a, na, b, nb) < 0) {
        *q_count = 0u; *q_sign = 0;
        for (size_t i = 0u; i < na; i++) r[i] = a[i];
        *r_count = na;
        *r_sign = sa;
        return true;
    }
    if (!mag_divmod(a, na, b, nb, q, q_count, r, r_count)) return false;
    *q_sign = *q_count == 0u ? 0 : (sa == sb ? 1 : -1);
    *r_sign = *r_count == 0u ? 0 : sa;
    return true;
}

size_t idm_bignum_to_decimal(uint32_t *scratch, size_t count, int sign, char *buf) {
    count = mag_normalize(scratch, count);
    if (count == 0u) { buf[0] = '0'; buf[1] = '\0'; return 1u; }
    char tmp[16];
    size_t pos = 0u;
    while (count > 0u) {
        uint64_t rem = 0u;
        for (size_t i = count; i-- > 0u;) {
            uint64_t acc = (rem << 32) | scratch[i];
            scratch[i] = (uint32_t)(acc / 1000000000u);
            rem = acc % 1000000000u;
        }
        count = mag_normalize(scratch, count);
        int width = snprintf(tmp, sizeof(tmp), count > 0u ? "%09llu" : "%llu", (unsigned long long)rem);
        for (int k = width - 1; k >= 0; k--) buf[pos++] = tmp[k];
    }
    if (sign < 0) buf[pos++] = '-';
    for (size_t i = 0u; i < pos / 2u; i++) {
        char c = buf[i];
        buf[i] = buf[pos - 1u - i];
        buf[pos - 1u - i] = c;
    }
    buf[pos] = '\0';
    return pos;
}

double idm_bignum_to_double(const uint32_t *a, size_t count, int sign) {
    double d = 0.0;
    for (size_t i = count; i-- > 0u;) d = d * 4294967296.0 + (double)a[i];
    return sign < 0 ? -d : d;
}

bool idm_bignum_fits_i64(const uint32_t *a, size_t count, int sign, int64_t *out) {
    if (count > 2u) return false;
    uint64_t mag = count > 0u ? a[0] : 0u;
    if (count > 1u) mag |= (uint64_t)a[1] << 32;
    if (sign >= 0) {
        if (mag > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX + 1u) return false;
        *out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
    }
    return true;
}
