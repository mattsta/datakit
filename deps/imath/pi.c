/*
  Name:     pi.c
  Purpose:  Computes digits of the physical constant pi.
  Author:   M. J. Fromberger <http://spinning-yarns.org/michael/>

  Copyright (C) 2002-2008 Michael J. Fromberger, All Rights Reserved.

  Notes:
  Uses Machin's formula, which should be suitable for a few thousand digits.

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

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "imath.h"

/* Largest digit count accepted. The printed form needs about one byte
   per digit, and mp_int_string_len() reports that size as an int-sized
   mp_result, so the cap keeps it far from overflow. */
#define PI_MAX_DIGITS ((mp_small)(INT32_MAX / 2))

int g_radix = 10; /* use this radix for output */

mp_result arctan(mp_small radix, mp_small mul, mp_small x, mp_small prec,
                 mp_int sum);

/* Parse all of str as a base-10 integer in [lo, hi]. Rejects empty
   input, trailing characters, out-of-range values and overflow. */
static bool parseBounded(const char *str, intmax_t lo, intmax_t hi,
                         intmax_t *out) {
    char *end = NULL;

    errno = 0;
    const intmax_t v = strtoimax(str, &end, 10);
    if (end == str || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
        return false;
    }

    *out = v;
    return true;
}

int main(int argc, char *argv[]) {
    mp_result res;
    mpz_t sum1, sum2;
    mp_small ndigits;
    intmax_t parsed;
    int out = 0;
    clock_t start, end;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <num-digits> [<radix>]\n", argv[0]);
        return 1;
    }

    if (!parseBounded(argv[1], 1, PI_MAX_DIGITS, &parsed)) {
        fprintf(stderr,
                "%s: the digit count must be an integer from 1 to %" PRId64
                "\n",
                argv[0], (int64_t)PI_MAX_DIGITS);
        return 1;
    }
    ndigits = (mp_small)parsed;

    if (argc > 2) {
        if (!parseBounded(argv[2], MP_MIN_RADIX, MP_MAX_RADIX, &parsed)) {
            fprintf(stderr,
                    "%s: you may only specify a radix between %d and %d\n",
                    argv[0], MP_MIN_RADIX, MP_MAX_RADIX);
            return 1;
        }

        g_radix = (int)parsed;
    }

    mp_int_init(&sum1);
    mp_int_init(&sum2);
    start = clock();

    /* sum1 = 16 * arctan(1/5) */
    if ((res = arctan(g_radix, 16, 5, ndigits, &sum1)) != MP_OK) {
        fprintf(stderr, "%s: error computing arctan: %d\n", argv[0], res);
        out = 1;
        goto CLEANUP;
    }

    /* sum2 = 4 * arctan(1/239) */
    if ((res = arctan(g_radix, 4, 239, ndigits, &sum2)) != MP_OK) {
        fprintf(stderr, "%s: error computing arctan: %d\n", argv[0], res);
        out = 1;
        goto CLEANUP;
    }

    /* pi = sum1 - sum2 */
    if ((res = mp_int_sub(&sum1, &sum2, &sum1)) != MP_OK) {
        fprintf(stderr, "%s: error computing pi: %d\n", argv[0], res);
        out = 1;
        goto CLEANUP;
    }

    end = clock();

    /* Size the buffer from the value itself: digits plus terminator. */
    const mp_result len = mp_int_string_len(&sum1, g_radix);
    if (len < 2) {
        fprintf(stderr, "%s: error sizing output: %d\n", argv[0], len);
        out = 1;
        goto CLEANUP;
    }

    char *g_buf = malloc((size_t)len);
    if (g_buf == NULL) {
        fprintf(stderr, "%s: out of memory\n", argv[0]);
        out = 1;
        goto CLEANUP;
    }

    if ((res = mp_int_to_string(&sum1, g_radix, g_buf, (size_t)len)) != MP_OK) {
        fprintf(stderr, "%s: error converting pi: %d\n", argv[0], res);
        free(g_buf);
        out = 1;
        goto CLEANUP;
    }
    printf("%c.%s\n", g_buf[0], g_buf + 1);
    free(g_buf);

    fprintf(stderr, "Computation took %.2f sec.\n",
            (double)(end - start) / CLOCKS_PER_SEC);

CLEANUP:
    mp_int_clear(&sum1);
    mp_int_clear(&sum2);

    return out;
}

/*
  Compute mul * atan(1/x) to prec digits of precision, and store the
  result in sum.

  Computes atan(1/x) using the formula:

               1     1      1      1
  atan(1/x) = --- - ---- + ---- - ---- + ...
               x    3x^3   5x^5   7x^7

 */
mp_result arctan(mp_small radix, mp_small mul, mp_small x, mp_small prec,
                 mp_int sum) {
    mpz_t t, v;
    mp_result res;
    mp_small rem, sign = 1, coeff = 1;

    mp_int_init(&t);
    mp_int_init(&v);
    ++prec;

    /* Compute mul * radix^prec * x
       The initial multiplication by x saves a special case in the loop for
       the first term of the series.
     */
    if ((res = mp_int_expt_value(radix, prec, &t)) != MP_OK ||
        (res = mp_int_mul_value(&t, mul, &t)) != MP_OK ||
        (res = mp_int_mul_value(&t, x, &t)) != MP_OK) {
        goto CLEANUP;
    }

    x *= x; /* assumes x <= sqrt(MP_SMALL_MAX) */
    mp_int_zero(sum);

    do {
        if ((res = mp_int_div_value(&t, x, &t, &rem)) != MP_OK) {
            goto CLEANUP;
        }

        if ((res = mp_int_div_value(&t, coeff, &v, &rem)) != MP_OK) {
            goto CLEANUP;
        }

        /* Add or subtract the result depending on the current sign (1 = add) */
        if (sign > 0) {
            res = mp_int_add(sum, &v, sum);
        } else {
            res = mp_int_sub(sum, &v, sum);
        }

        if (res != MP_OK) {
            goto CLEANUP;
        }

        sign = -sign;
        coeff += 2;

    } while (mp_int_compare_zero(&t) != 0);

    res = mp_int_div_value(sum, radix, sum, NULL);

CLEANUP:
    mp_int_clear(&v);
    mp_int_clear(&t);

    return res;
}

/* Here there be dragons */
