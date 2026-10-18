/*
  Name:     gmp_compat.c
  Purpose:  Provide GMP compatiable routines for imath library
  Author:   David Peixotto

  Copyright (c) 2012 Qualcomm Innovation Center, Inc. All rights reserved.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 */
#include "gmp_compat.h"
#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#define CHECK(res) (res)
#else
#define CHECK(res) assert(((res) == MP_OK) && "expected MP_OK")
#endif

/* *(signed char *)&endian_test will thus either be:
 *     0b00000001 =  1 on big-endian
 *     0b11111111 = -1 on little-endian */
static const uint16_t endian_test = 0x1FF;
#define HOST_ENDIAN (*(signed char *)&endian_test)

/*************************************************************************
 *
 * Functions with direct translations
 *
 *************************************************************************/
/* gmp: mpq_clear */
void GMPQAPI(clear)(mp_rat x) {
    mp_rat_clear(x);
}

/* gmp: mpq_cmp */
int GMPQAPI(cmp)(mp_rat op1, mp_rat op2) {
    return mp_rat_compare(op1, op2);
}

/* gmp: mpq_init */
void GMPQAPI(init)(mp_rat x) {
    CHECK(mp_rat_init(x));
}

/* gmp: mpq_mul */
void GMPQAPI(mul)(mp_rat product, mp_rat multiplier, mp_rat multiplicand) {
    CHECK(mp_rat_mul(multiplier, multiplicand, product));
}

/* gmp: mpq_set*/
void GMPQAPI(set)(mp_rat rop, mp_rat op) {
    CHECK(mp_rat_copy(op, rop));
}

/* gmp: mpz_abs */
void GMPZAPI(abs)(mp_int rop, mp_int op) {
    CHECK(mp_int_abs(op, rop));
}

/* gmp: mpz_add */
void GMPZAPI(add)(mp_int rop, mp_int op1, mp_int op2) {
    CHECK(mp_int_add(op1, op2, rop));
}

/* gmp: mpz_clear */
void GMPZAPI(clear)(mp_int x) {
    mp_int_clear(x);
}

/* gmp: mpz_cmp_si */
int GMPZAPI(cmp_si)(mp_int op1, long op2) {
    return mp_int_compare_value(op1, op2);
}

/* gmp: mpz_cmpabs */
int GMPZAPI(cmpabs)(mp_int op1, mp_int op2) {
    return mp_int_compare_unsigned(op1, op2);
}

/* gmp: mpz_cmp */
int GMPZAPI(cmp)(mp_int op1, mp_int op2) {
    return mp_int_compare(op1, op2);
}

/* gmp: mpz_init */
void GMPZAPI(init)(mp_int x) {
    CHECK(mp_int_init(x));
}

/* gmp: mpz_mul */
void GMPZAPI(mul)(mp_int rop, mp_int op1, mp_int op2) {
    CHECK(mp_int_mul(op1, op2, rop));
}

/* gmp: mpz_neg */
void GMPZAPI(neg)(mp_int rop, mp_int op) {
    CHECK(mp_int_neg(op, rop));
}

/* gmp: mpz_set_si */
void GMPZAPI(set_si)(mp_int rop, long op) {
    CHECK(mp_int_set_value(rop, op));
}

/* gmp: mpz_set */
void GMPZAPI(set)(mp_int rop, mp_int op) {
    CHECK(mp_int_copy(op, rop));
}

/* gmp: mpz_sub */
void GMPZAPI(sub)(mp_int rop, mp_int op1, mp_int op2) {
    CHECK(mp_int_sub(op1, op2, rop));
}

/* gmp: mpz_swap */
void GMPZAPI(swap)(mp_int rop1, mp_int rop2) {
    mp_int_swap(rop1, rop2);
}

/* gmp: mpq_sgn */
int GMPQAPI(sgn)(mp_rat op) {
    return mp_rat_compare_zero(op);
}

/* gmp: mpz_sgn */
int GMPZAPI(sgn)(mp_int op) {
    return mp_int_compare_zero(op);
}

/* gmp: mpq_set_ui */
void GMPQAPI(set_ui)(mp_rat rop, unsigned long op1, unsigned long op2) {
    CHECK(mp_rat_set_uvalue(rop, op1, op2));
}

/* gmp: mpz_set_ui */
void GMPZAPI(set_ui)(mp_int rop, unsigned long op) {
    CHECK(mp_int_set_uvalue(rop, op));
}

/* gmp: mpq_den_ref */
mp_int GMPQAPI(denref)(mp_rat op) {
    return mp_rat_denom_ref(op);
}

/* gmp: mpq_num_ref */
mp_int GMPQAPI(numref)(mp_rat op) {
    return mp_rat_numer_ref(op);
}

/* gmp: mpq_canonicalize */
void GMPQAPI(canonicalize)(mp_rat op) {
    CHECK(mp_rat_reduce(op));
}

/*************************************************************************
 *
 * Functions that can be implemented as a combination of imath functions
 *
 *************************************************************************/
/* gmp: mpz_addmul */
/* gmp: rop = rop + (op1 * op2) */
void GMPZAPI(addmul)(mp_int rop, mp_int op1, mp_int op2) {
    mpz_t tempz;
    mp_int temp = &tempz;
    mp_int_init(temp);

    CHECK(mp_int_mul(op1, op2, temp));
    CHECK(mp_int_add(rop, temp, rop));
    mp_int_clear(temp);
}

/* gmp: mpz_divexact */
/* gmp: only produces correct results when d divides n */
void GMPZAPI(divexact)(mp_int q, mp_int n, mp_int d) {
    CHECK(mp_int_div(n, d, q, NULL));
}

/* gmp: mpz_divisible_p */
/* gmp: return 1 if d divides n, 0 otherwise */
/* gmp: 0 is considered to divide 0*/
int GMPZAPI(divisible_p)(mp_int n, mp_int d) {
    /* variables to hold remainder */
    mpz_t rz;
    mp_int r = &rz;
    int r_is_zero;

    /* check for n = 0, d = 0 */
    int n_is_zero = mp_int_compare_zero(n) == 0;
    int d_is_zero = mp_int_compare_zero(d) == 0;
    if (n_is_zero && d_is_zero) {
        return 1;
    }

    /* return true if remainder is 0 */
    CHECK(mp_int_init(r));
    CHECK(mp_int_div(n, d, NULL, r));
    r_is_zero = mp_int_compare_zero(r) == 0;
    mp_int_clear(r);

    return r_is_zero;
}

/* gmp: mpz_submul */
/* gmp: rop = rop - (op1 * op2) */
void GMPZAPI(submul)(mp_int rop, mp_int op1, mp_int op2) {
    mpz_t tempz;
    mp_int temp = &tempz;
    mp_int_init(temp);

    CHECK(mp_int_mul(op1, op2, temp));
    CHECK(mp_int_sub(rop, temp, rop));

    mp_int_clear(temp);
}

/* gmp: mpz_add_ui */
void GMPZAPI(add_ui)(mp_int rop, mp_int op1, unsigned long op2) {
    mpz_t tempz;
    mp_int temp = &tempz;
    CHECK(mp_int_init_uvalue(temp, op2));

    CHECK(mp_int_add(op1, temp, rop));

    mp_int_clear(temp);
}

/* gmp: mpz_divexact_ui */
/* gmp: only produces correct results when d divides n */
void GMPZAPI(divexact_ui)(mp_int q, mp_int n, unsigned long d) {
    mpz_t tempz;
    mp_int temp = &tempz;
    CHECK(mp_int_init_uvalue(temp, d));

    CHECK(mp_int_div(n, temp, q, NULL));

    mp_int_clear(temp);
}

/* gmp: mpz_mul_ui */
void GMPZAPI(mul_ui)(mp_int rop, mp_int op1, unsigned long op2) {
    mpz_t tempz;
    mp_int temp = &tempz;
    CHECK(mp_int_init_uvalue(temp, op2));

    CHECK(mp_int_mul(op1, temp, rop));

    mp_int_clear(temp);
}

/* gmp: mpz_pow_ui */
/* gmp: 0^0 = 1 */
void GMPZAPI(pow_ui)(mp_int rop, mp_int base, unsigned long exp) {
    mpz_t tempz;
    mp_int temp = &tempz;

    /* check for 0^0 */
    if (exp == 0 && mp_int_compare_zero(base) == 0) {
        CHECK(mp_int_set_value(rop, 1));
        return;
    }

    /* rop = base^exp */
    CHECK(mp_int_init_uvalue(temp, exp));
    CHECK(mp_int_expt_full(base, temp, rop));
    mp_int_clear(temp);
}

/* gmp: mpz_sub_ui */
void GMPZAPI(sub_ui)(mp_int rop, mp_int op1, unsigned long op2) {
    mpz_t tempz;
    mp_int temp = &tempz;
    CHECK(mp_int_init_uvalue(temp, op2));

    CHECK(mp_int_sub(op1, temp, rop));

    mp_int_clear(temp);
}

/*************************************************************************
 *
 * Functions with different behavior in corner cases
 *
 *************************************************************************/

/* gmp: mpz_gcd */
void GMPZAPI(gcd)(mp_int rop, mp_int op1, mp_int op2) {
    int op1_is_zero = mp_int_compare_zero(op1) == 0;
    int op2_is_zero = mp_int_compare_zero(op2) == 0;

    if (op1_is_zero && op2_is_zero) {
        mp_int_zero(rop);
        return;
    }

    CHECK(mp_int_gcd(op1, op2, rop));
}

/* Map a gmp radix (negative means upper-case digits) to an imath radix,
   or 0 when imath cannot print in that base. */
static mp_size get_str_radix(int radix) {
    const int64_t r = radix < 0 ? -(int64_t)radix : (int64_t)radix;
    if (r < MP_MIN_RADIX || r > MP_MAX_RADIX) {
        return 0;
    }
    return (mp_size)r;
}

/* Apply gmp's digit case to the characters actually written. The
   buffer size comes from a length estimate that may over-count, so the
   terminator, not that estimate, bounds the loop. */
static void get_str_case(char *str, int radix) {
    for (char *p = str; *p != '\0'; p++) {
        const int c = (unsigned char)*p;
        *p = (char)(radix < 0 ? toupper(c) : tolower(c));
    }
}

/* gmp: mpz_get_str. Returns NULL for an unsupported radix, a failed
   allocation (when str is NULL) or a failed conversion. */
char *GMPZAPI(get_str)(char *str, int radix, mp_int op) {
    const mp_size r = get_str_radix(radix);
    if (r == 0) {
        return NULL;
    }

    /* The length needed to hold the int, terminator included */
    const mp_result need = mp_int_string_len(op, r);
    if (need < 2) {
        return NULL;
    }
    const size_t len = (size_t)need;

    char *out = str;
    if (out == NULL) {
        out = malloc(len);
        if (out == NULL) {
            return NULL;
        }
    }

    if (mp_int_to_string(op, r, out, len) != MP_OK) {
        if (str == NULL) {
            free(out);
        }
        return NULL;
    }

    get_str_case(out, radix);
    return out;
}

/* gmp: mpq_get_str. Returns NULL under the same conditions as
   mpz_get_str. */
char *GMPQAPI(get_str)(char *str, int radix, mp_rat op) {
    /* Only print numerator if it is a whole number */
    if (mp_int_compare_value(mp_rat_denom_ref(op), 1) == 0) {
        return GMPZAPI(get_str)(str, radix, mp_rat_numer_ref(op));
    }

    const mp_size r = get_str_radix(radix);
    if (r == 0) {
        return NULL;
    }

    /* The length needed to hold the rational, terminator included */
    const mp_result need = mp_rat_string_len(op, r);
    if (need < 2) {
        return NULL;
    }
    const size_t len = (size_t)need;

    char *out = str;
    if (out == NULL) {
        out = malloc(len);
        if (out == NULL) {
            return NULL;
        }
    }

    if (mp_rat_to_string(op, r, out, (int)len) != MP_OK) {
        if (str == NULL) {
            free(out);
        }
        return NULL;
    }

    get_str_case(out, radix);
    return out;
}

/* gmp: mpz_set_str */
int GMPZAPI(set_str)(mp_int rop, char *str, int base) {
    mp_result res = mp_int_read_string(rop, base, str);
    return ((res == MP_OK) ? 0 : -1);
}

/* gmp: mpq_set_str */
int GMPQAPI(set_str)(mp_rat rop, char *s, int base) {
    char *slash;
    char *str;
    mp_result resN;
    mp_result resD;
    int res = 0;

    /* Copy string to temporary storage so we can modify it below */
    const size_t len = strlen(s) + 1;
    str = malloc(len);
    if (str == NULL) {
        return -1;
    }
    memcpy(str, s, len);

    /* Properly format the string as an int by terminating at the / */
    slash = strchr(str, '/');
    if (slash) {
        *slash = '\0';

        /* Parse numerator */
    }
    resN = mp_int_read_string(mp_rat_numer_ref(rop), base, str);

    /* Parse denomenator if given or set to 1 if not */
    if (slash) {
        resD = mp_int_read_string(mp_rat_denom_ref(rop), base, slash + 1);
    } else {
        resD = mp_int_set_uvalue(mp_rat_denom_ref(rop), 1);
    }

    /* Return failure if either parse failed */
    if (resN != MP_OK || resD != MP_OK) {
        res = -1;
    }

    free(str);
    return res;
}

static unsigned long get_long_bits(mp_int op) {
    /* Deal with integer that does not fit into unsigned long. We want to grab
     * the least significant digits that will fit into the long.  Read the
     * digits
     * into the long starting at the most significant digit that fits into a
     * long. The long is shifted over by MP_DIGIT_BIT before each digit is
     * added.
     * The shift is decomposed into two steps to follow the patten used in the
     * rest of the imath library. The two step shift is used to accomedate
     * architectures that don't deal well with 32-bit shifts. */
    mp_size num_digits_in_long = sizeof(unsigned long) / sizeof(mp_digit);
    mp_digit *digits = MP_DIGITS(op);
    unsigned long out = 0;
    int i;

    for (i = num_digits_in_long - 1; i >= 0; i--) {
        out <<= (MP_DIGIT_BIT / 2);
        out <<= (MP_DIGIT_BIT / 2);
        out |= digits[i];
    }

    return out;
}

/* gmp: mpz_get_ui */
unsigned long GMPZAPI(get_ui)(mp_int op) {
    mp_usmall out;

    /* Try a standard conversion that fits into an unsigned long */
    mp_result res = mp_int_to_uint(op, &out);
    if (res == MP_OK) {
        return out;
    }

    /* Abort the try if we don't have a range error in the conversion.
     * The range error indicates that the value cannot fit into a long. */
    CHECK(res == MP_RANGE ? MP_OK : MP_RANGE);
    if (res != MP_RANGE) {
        return 0;
    }

    return get_long_bits(op);
}

/* gmp: mpz_get_si */
long GMPZAPI(get_si)(mp_int op) {
    mp_small out;
    unsigned long uout;
    int long_msb;

    /* Try a standard conversion that fits into a long */
    mp_result res = mp_int_to_int(op, &out);
    if (res == MP_OK) {
        return out;
    }

    /* Abort the try if we don't have a range error in the conversion.
     * The range error indicates that the value cannot fit into a long. */
    CHECK(res == MP_RANGE ? MP_OK : MP_RANGE);
    if (res != MP_RANGE) {
        return 0;
    }

    /* get least significant bits into an unsigned long */
    uout = get_long_bits(op);

    /* clear the top bit */
    long_msb = (sizeof(unsigned long) * 8) - 1;
    uout &= (~(1UL << long_msb));

    /* convert to negative if needed based on sign of op */
    if (MP_SIGN(op) == MP_NEG) {
        uout = 0 - uout;
    }

    out = (long)uout;
    return out;
}

/* gmp: mpz_lcm */
void GMPZAPI(lcm)(mp_int rop, mp_int op1, mp_int op2) {
    int op1_is_zero = mp_int_compare_zero(op1) == 0;
    int op2_is_zero = mp_int_compare_zero(op2) == 0;

    if (op1_is_zero || op2_is_zero) {
        mp_int_zero(rop);
        return;
    }

    CHECK(mp_int_lcm(op1, op2, rop));
    CHECK(mp_int_abs(rop, rop));
}

/* gmp: mpz_mul_2exp */
/* gmp: allow big values for op2 when op1 == 0 */
void GMPZAPI(mul_2exp)(mp_int rop, mp_int op1, unsigned long op2) {
    if (mp_int_compare_zero(op1) == 0) {
        mp_int_zero(rop);
    } else {
        CHECK(mp_int_mul_pow2(op1, op2, rop));
    }
}

/*************************************************************************
 *
 * Functions needing expanded functionality
 *
 *************************************************************************/
/* [Note]Overview of division implementation

    All division operations (N / D) compute q and r such that

      N = q * D + r, with 0 <= abs(r) < abs(d)

    The q and r values are not uniquely specified by N and D. To specify which q
    and r values should be used, GMP implements three different rounding modes
    for integer division:

      ceiling  - round q twords +infinity, r has opposite sign as d
      floor    - round q twords -infinity, r has same sign as d
      truncate - round q twords zero,      r has same sign as n

    The imath library only supports truncate as a rounding mode. We need to
    implement the other rounding modes in terms of truncating division. We first
    perform the division in trucate mode and then adjust q accordingly. Once we
    know q, we can easily compute the correct r according the the formula above
    by computing:

      r = N - q * D

    The main task is to compute q. We can compute the correct q from a trucated
    version as follows.

    For ceiling rounding mode, if q is less than 0 then the truncated rounding
    mode is the same as the ceiling rounding mode.  If q is greater than zero
    then we need to round q up by one because the truncated version was rounded
    down to zero. If q equals zero then check to see if the result of the
    divison is positive. A positive result needs to increment q to one.

    For floor rounding mode, if q is greater than 0 then the trucated rounding
    mode is the same as the floor rounding mode. If q is less than zero then we
    need to round q down by one because the trucated mode rounded q up by one
    twords zero. If q is zero then we need to check to see if the result of the
    division is negative. A negative result needs to decrement q to negative
    one.
 */

/* gmp: mpz_cdiv_q */
void GMPZAPI(cdiv_q)(mp_int q, mp_int n, mp_int d) {
    mpz_t rz;
    mp_int r = &rz;
    int qsign, rsign, nsign, dsign;
    CHECK(mp_int_init(r));

    /* save signs before division because q can alias with n or d */
    nsign = mp_int_compare_zero(n);
    dsign = mp_int_compare_zero(d);

    /* truncating division */
    CHECK(mp_int_div(n, d, q, r));

    /* see: [Note]Overview of division implementation */
    qsign = mp_int_compare_zero(q);
    rsign = mp_int_compare_zero(r);
    if (qsign > 0) {      /* q > 0 */
        if (rsign != 0) { /* r != 0 */
            CHECK(mp_int_add_value(q, 1, q));
        }
    }

    else if (qsign == 0) { /* q == 0 */
        if (rsign != 0) {  /* r != 0 */
            if ((nsign > 0 && dsign > 0) || (nsign < 0 && dsign < 0)) {
                CHECK(mp_int_set_value(q, 1));
            }
        }
    }

    mp_int_clear(r);
}

/* gmp: mpz_fdiv_q */
void GMPZAPI(fdiv_q)(mp_int q, mp_int n, mp_int d) {
    mpz_t rz;
    mp_int r = &rz;
    int qsign, rsign, nsign, dsign;
    CHECK(mp_int_init(r));

    /* save signs before division because q can alias with n or d */
    nsign = mp_int_compare_zero(n);
    dsign = mp_int_compare_zero(d);

    /* truncating division */
    CHECK(mp_int_div(n, d, q, r));

    /* see: [Note]Overview of division implementation */
    qsign = mp_int_compare_zero(q);
    rsign = mp_int_compare_zero(r);
    if (qsign < 0) {      /* q  < 0 */
        if (rsign != 0) { /* r != 0 */
            CHECK(mp_int_sub_value(q, 1, q));
        }
    }

    else if (qsign == 0) { /* q == 0 */
        if (rsign != 0) {  /* r != 0 */
            if ((nsign < 0 && dsign > 0) || (nsign > 0 && dsign < 0)) {
                CHECK(mp_int_set_value(q, -1));
            }
        }
    }

    mp_int_clear(r);
}

/* gmp: mpz_fdiv_r */
void GMPZAPI(fdiv_r)(mp_int r, mp_int n, mp_int d) {
    mpz_t qz;
    mpz_t tempz;
    mpz_t orig_dz;
    mpz_t orig_nz;
    mp_int q = &qz;
    mp_int temp = &tempz;
    mp_int orig_d = &orig_dz;
    mp_int orig_n = &orig_nz;
    CHECK(mp_int_init(q));
    CHECK(mp_int_init(temp));
    /* Make a copy of n in case n and d in case they overlap with q */
    CHECK(mp_int_init_copy(orig_d, d));
    CHECK(mp_int_init_copy(orig_n, n));

    /* floor division */
    GMPZAPI(fdiv_q)(q, n, d);

    /* see: [Note]Overview of division implementation */
    /* n = q * d + r  ==>  r = n - q * d */
    mp_int_mul(q, orig_d, temp);
    mp_int_sub(orig_n, temp, r);

    mp_int_clear(q);
    mp_int_clear(temp);
    mp_int_clear(orig_d);
    mp_int_clear(orig_n);
}

/* gmp: mpz_tdiv_q */
void GMPZAPI(tdiv_q)(mp_int q, mp_int n, mp_int d) {
    /* truncating division*/
    CHECK(mp_int_div(n, d, q, NULL));
}

/* gmp: mpz_fdiv_q_ui */
unsigned long GMPZAPI(fdiv_q_ui)(mp_int q, mp_int n, unsigned long d) {
    mpz_t tempz;
    mp_int temp = &tempz;
    mpz_t rz;
    mp_int r = &rz;
    mpz_t orig_nz;
    mp_int orig_n = &orig_nz;
    mp_usmall rl;
    CHECK(mp_int_init_uvalue(temp, d));
    CHECK(mp_int_init(r));
    /* Make a copy of n in case n and q overlap */
    CHECK(mp_int_init_copy(orig_n, n));

    /* use floor division mode to compute q and r */
    GMPZAPI(fdiv_q)(q, n, temp);
    GMPZAPI(fdiv_r)(r, orig_n, temp);
    CHECK(mp_int_to_uint(r, &rl));

    mp_int_clear(temp);
    mp_int_clear(r);
    mp_int_clear(orig_n);

    return rl;
}

/* gmp: mpz_export */
void *GMPZAPI(export)(void *rop, size_t *countp, int order, size_t size,
                      int endian, size_t nails, mp_int op) {
    size_t i, j;
    size_t num_used_bytes;
    size_t num_words, num_missing_bytes;
    ptrdiff_t word_offset;
    unsigned char *dst;
    mp_digit *src;
    int src_bits;

    /* We do not have a complete implementation. Assert to ensure our
     * restrictions are in place. */
    assert(nails == 0 && "Do not support non-full words");
    assert(endian == 1 || endian == 0 || endian == -1);
    assert(order == 1 || order == -1);

    /* Test for zero */
    if (mp_int_compare_zero(op) == 0) {
        if (countp) {
            *countp = 0;
        }

        return rop;
    }

    /* gmp requires a word size of at least one byte; the signed word
       stride below must also be representable. */
    if (size == 0 || size > PTRDIFF_MAX / 2) {
        if (countp) {
            *countp = 0;
        }
        return NULL;
    }

    /* Calculate how many words we need. mp_int_unsigned_len() reports
       failure as a negative mp_result; a nonzero value needs >= 1 byte. */
    const mp_result used_len = mp_int_unsigned_len(op);
    if (used_len <= 0) {
        if (countp) {
            *countp = 0;
        }
        return NULL;
    }
    num_used_bytes = (size_t)used_len;
    num_words = (num_used_bytes + (size - 1)) / size; /* ceil division */

    /* Check to see if we will have missing bytes in the last word.

       Missing bytes can only occur when the size of words we output is
       greater than the size of words used internally by imath. The number of
       missing bytes is the number of bytes needed to fill out the last word. If
       this number is greater than the size of a single mp_digit, then we need
       to
       pad the word with extra zeros. Otherwise, the missing bytes can be filled
       directly from the zeros in the last digit in the number.
     */
    num_missing_bytes = (size * num_words) - num_used_bytes;
    assert(num_missing_bytes < size);

    /* Allocate space for the result if needed */
    if (rop == NULL) {
        rop = malloc(num_words * size);
        if (rop == NULL) {
            if (countp) {
                *countp = 0;
            }
            return NULL;
        }
    }

    if (endian == 0) {
        endian = HOST_ENDIAN;
    }

    /* Initialize dst and src pointers */
    dst = (unsigned char *)rop + (order >= 0 ? (num_words - 1) * size : 0) +
          (endian >= 0 ? size - 1 : 0);
    src = MP_DIGITS(op);
    src_bits = MP_DIGIT_BIT;

    const ptrdiff_t stride = (ptrdiff_t)size;
    word_offset =
        (endian >= 0 ? stride : -stride) + (order < 0 ? stride : -stride);

    for (i = 0; i < num_words; i++) {
        for (j = 0; j < size && i * size + j < num_used_bytes; j++) {
            if (src_bits == 0) {
                ++src;
                src_bits = MP_DIGIT_BIT;
            }

            *dst = (*src >> (MP_DIGIT_BIT - src_bits)) & 0xFF;
            src_bits -= 8;
            dst -= endian;
        }

        for (; j < size; j++) {
            *dst = 0;
            dst -= endian;
        }

        dst += word_offset;
    }

    if (countp) {
        *countp = num_words;
    }

    return rop;
}

/* gmp: mpz_import */
void GMPZAPI(import)(mp_int rop, size_t count, int order, size_t size,
                     int endian, size_t nails, const void *op) {
    mpz_t tmpz;
    mp_int tmp = &tmpz;
    size_t total_size;
    size_t num_digits;
    ptrdiff_t word_offset;
    const unsigned char *src;
    mp_digit *dst;
    int dst_bits;
    size_t i, j;
    if (count == 0 || op == NULL) {
        return;
    }

    /* gmp requires a word size of at least one byte. The byte total and
       the signed word stride must not overflow, and the digit count
       must fit imath's mp_size; otherwise rop is left unchanged. */
    if (size == 0 || size > PTRDIFF_MAX / 2 || count > SIZE_MAX / size) {
        return;
    }

    /* We do not have a complete implementation. Assert to ensure our
     * restrictions are in place. */
    assert(nails == 0 && "Do not support non-full words");
    assert(endian == 1 || endian == 0 || endian == -1);
    assert(order == 1 || order == -1);

    if (endian == 0) {
        endian = HOST_ENDIAN;
    }

    /* Compute number of needed digits by ceil division */
    total_size = count * size;
    num_digits = total_size / sizeof(mp_digit) +
                 (total_size % sizeof(mp_digit) != 0 ? 1 : 0);
    if (num_digits > (mp_size) ~(mp_size)0) {
        return;
    }

    /* Init temporary */
    if (mp_int_init_size(tmp, (mp_size)num_digits) != MP_OK) {
        return;
    }
    for (i = 0; i < num_digits; i++) {
        MP_DIGITS(tmp)[i] = 0;
    }

    /* Copy bytes */
    src = (const unsigned char *)op + (order >= 0 ? (count - 1) * size : 0) +
          (endian >= 0 ? size - 1 : 0);
    dst = MP_DIGITS(tmp);
    dst_bits = 0;

    const ptrdiff_t stride = (ptrdiff_t)size;
    word_offset =
        (endian >= 0 ? stride : -stride) + (order < 0 ? stride : -stride);

    for (i = 0; i < count; i++) {
        for (j = 0; j < size; j++) {
            if (dst_bits == MP_DIGIT_BIT) {
                ++dst;
                dst_bits = 0;
            }

            *dst |= ((mp_digit)*src) << dst_bits;
            dst_bits += 8;
            src -= endian;
        }

        src += word_offset;
    }

    (tmp)->used = (mp_size)num_digits; /* range-checked above */

    /* Remove leading zeros from number */
    {
        mp_size uz_ = (tmp)->used;
        mp_digit *dz_ = MP_DIGITS(tmp) + uz_ - 1;
        while (uz_ > 1 && (*dz_-- == 0)) {
            --uz_;
        }

        (tmp)->used = uz_;
    }

    /* Copy to destination */
    mp_int_copy(tmp, rop);
    mp_int_clear(tmp);
}

/* gmp: mpz_sizeinbase */
size_t GMPZAPI(sizeinbase)(mp_int op, int base) {
    mp_result res;
    size_t size;

    /* If op == 0, return 1 */
    if (mp_int_compare_zero(op) == 0) {
        return 1;
    }

    /* Compute string length in base */
    res = mp_int_string_len(op, base);
    CHECK((res > 0) == MP_OK);

    /* Now adjust the final size by getting rid of string artifacts */
    size = res;

    /* subtract one for the null terminator */
    size -= 1;

    /* subtract one for the negative sign */
    if (mp_int_compare_zero(op) < 0) {
        size -= 1;
    }

    return size;
}
