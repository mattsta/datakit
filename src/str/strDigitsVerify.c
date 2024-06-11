#include "../str.h"

/* ====================================================================
 * Verify all bytes in string are ASCII digits [0-9]
 * ==================================================================== */
/* StrIsDigits* adapted from:
 * https://github.com/WojciechMula/toys/blob/master/parse_decimal/validate_input.cpp
 * and
 * http://0x80.pl/articles/swar-digits-validate.html
 * (BSD) */
bool StrIsDigitsIndividual(const void *buf_, const size_t size) {
    const uint8_t *buf = (const uint8_t *)buf_;

    /* Compilers properly optimize this into the most efficient version */
#pragma unroll 8
    for (size_t i = 0; i < size; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
            return false;
        }
    }

    return true;
}

#if __SSE2__
/* All intrinsics! */
#include <x86intrin.h>

/* SSE2 StrIsDigitsFast() is about 4x faster than StrIsDigitsIndividual() */
bool StrIsDigitsFast(const void *buf_, size_t size) {
    /* Compilers properly create these two vectors of '0's and '9's at compile
     * time so they have no runtime overhead (but they are not "compile time"
     * _enough_ where they can be declared 'static') */
    const __m128i ascii0 = _mm_set1_epi8('0');
    const __m128i ascii9 = _mm_set1_epi8('9');

    const __m128i *mover = (const __m128i *)buf_;

    const size_t n = size / sizeof(__m128i);
    const size_t leftover = size % sizeof(__m128i);
    const __m128i *moverEnd = mover + n;

    for (; mover < moverEnd; mover++) {
        /* load 16 bytes from 'buf' (safe for unaligned reads) */
        const __m128i v = _mm_loadu_si128(mover);

        /* Calculate if any bytes are less than '0' */
        const __m128i lt0 = _mm_cmplt_epi8(v, ascii0);

        /* Calculate if any bytes are greater than '9' */
        const __m128i gt9 = _mm_cmpgt_epi8(v, ascii9);

        /* Calculate if any bytes matched < '0' || > '9' */
        const __m128i outside = _mm_or_si128(lt0, gt9);

        /* Convert 'outside' result vector to an integer for branching. */
        if (_mm_movemask_epi8(outside)) {
            return false;
        }
    }

    /* Cleanup < 16 bytes remaining */
    return StrIsDigitsIndividual((uint8_t *)mover, leftover);
}
#else
#warning "Using unoptimized StrIsDigitsFast()!"
bool StrIsDigitsFast(const void *buf_, const size_t size) {
    /* Note: fancy SWAR methods are not noticably faster than
     *       optimized-native-compiled StrIsDigitsIndividual! */
    return StrIsDigitsIndividual(buf_, size);
}
#endif
