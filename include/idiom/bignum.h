#ifndef IDM_BIGNUM_H
#define IDM_BIGNUM_H

#include "idiom/common.h"

#define IDM_BIGNUM_I64_LIMBS 2u

size_t idm_bignum_decimal_limb_cap(size_t digit_len);
size_t idm_bignum_decimal_digit_cap(size_t limb_count);

size_t idm_bignum_from_i64(int64_t value, uint32_t *out, int *out_sign);
bool idm_bignum_from_decimal(const char *text, size_t len, uint32_t *out, size_t *out_count, int *out_sign);
bool idm_bignum_decimal_equal(const char *a, size_t na, const char *b, size_t nb, bool *out);

int idm_bignum_cmp(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb);

void idm_bignum_add(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign);
void idm_bignum_sub(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign);
void idm_bignum_mul(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *out, size_t *out_count, int *out_sign);
bool idm_bignum_divmod(const uint32_t *a, size_t na, int sa, const uint32_t *b, size_t nb, int sb, uint32_t *q, size_t *q_count, int *q_sign, uint32_t *r, size_t *r_count, int *r_sign);

size_t idm_bignum_to_decimal(uint32_t *scratch, size_t count, int sign, char *buf);
double idm_bignum_to_double(const uint32_t *a, size_t count, int sign);
bool idm_bignum_fits_i64(const uint32_t *a, size_t count, int sign, int64_t *out);

#endif
