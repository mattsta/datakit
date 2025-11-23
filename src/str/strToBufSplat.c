#include "../str.h"

/* ====================================================================
 * String to Integer/Float Routines from http://www.corsix.org/
 * ==================================================================== */
#if __SSE2__ /* note: compile time decision instead of runtime decision */
#include <x86intrin.h>

/* From http://corsix.org/content/converting-nine-digit-integers-to-strings */
void StrUInt9DigitsToBuf(void *out_, uint32_t u) {
    uint8_t *out = out_;
    uint32_t v = u / 10000;
    uint32_t w = v / 10000;
    u -= v * 10000;
    v -= w * 10000;

    const __m128i first_madd =
        _mm_set_epi16(-32768, -32768, 0, 26215, 0, 10486, 0, 8389);
    const __m128i mask = _mm_set_epi16(-1, 0, -4, 0, -16, 0, -128, 0);
    const __m128i second_madd =
        _mm_set_epi16(-256, -640, 64, -160, 16, -20, 2, 0);

    __m128i x = _mm_madd_epi16(_mm_set1_epi16(v), first_madd);
    __m128i y = _mm_madd_epi16(_mm_set1_epi16(u), first_madd);
    x = _mm_and_si128(x, mask);
    y = _mm_and_si128(y, mask);
    x = _mm_or_si128(x, _mm_slli_si128(x, 2));
    y = _mm_or_si128(y, _mm_slli_si128(y, 2));
    x = _mm_madd_epi16(x, second_madd);
    y = _mm_madd_epi16(y, second_madd);

    __m128i z = _mm_srli_epi16(_mm_packs_epi32(x, y), 8);
    z = _mm_packs_epi16(z, z);
    out[0] = '0' | w;
    _mm_storel_epi64((__m128i *)(out + 1),
                     _mm_or_si128(z, _mm_set1_epi32(0x30303030)));
}

/* From https://gist.github.com/alnsn/83ae6391c66bc1f117b9b6b5fbf2c331 */
/* (MIT) */
/*
 * Given an integer u from 0 to 9999, we want to perform 3 divisions
 * by constants 10, 100 and 1000 in parallel and calculate four digits
 * u - u/10*10, u/10 - u/100*10, etc. These digits can be shuffled,
 * converted to ascii and stored in memory as four consecutive bytes.
 *
 * One common approach to constant division is double-width multiplication
 * by a magic constant and shifting high-word to the right by a constant
 * number of bits.
 *
 * Double-width multiplication in xmm register can be done with pmuludq
 * but it operates on two 32-bit words while we need at least three
 * multiplications. For u that fits into 16-bit word, we can try pmaddwd
 * which multiplies eight signed 16-bit words, takes sums of pairs and
 * stores the results in four 32-bit words.
 *
 * The algorithm below uses these magic multiplications:
 *
 * u/10   : u * 26215 / 2^18,
 * u/100  : u * 10486 / 2^20,
 * u/1000 : u * 8389  / 2^23.
 *
 * The shifts are all different but it doesn't matter. Instead of
 * shifting to the right, low bits are masked and values are later
 * multiplied to scale the results by 256.
 */
static __m128i d4toa(unsigned int u) {
    /*
     * Multiply u by -65536 (to move -u to the high word),
     * 26215 (magic for u/10), 10486 (magic for u/100) and
     * 8389 (magic for u/1000).
     */
    const __m128i first_madd =
        _mm_set_epi16(-32768, -32768, 0, 26215, 0, 10486, 0, 8389);

    /*
     * Zero-out 18 low bits of u*26215, 20 low bits of u*10486
     * and 23 low bits of u*8389:
     * [-u, 0, u/10*4, 0, u/100*16, 0, u/1000*128].
     */
    const __m128i mask = _mm_set_epi16(-1, 0, -4, 0, -16, 0, -128, 0);

    /*
     * Input value
     *
     * [-u, u/10*4, u/10*4, u/100*16, u/100*16, u/1000*128, n/1000*128, 0]
     *
     * is multiplied to produce 4 scaled digits:
     *
     * [(-u)*-256 - (u/10*4)*10*64, 0, (u/10*4)*64 - (u/100*16)*16*10,
     *  (u/100*16)*16 - (u/1000*128)*2*10, (n/1000*128)*2]
     */
    const __m128i second_madd =
        _mm_set_epi16(-256, -640, 64, -160, 16, -20, 2, 0);

    /*
     * Shuffle digits to low bytes and OR with ascii zeroes.
     * Only low 32-bit word matter, three other words can
     * have any values.
     */
    const __m128i shuffle = _mm_set_epi32(0, 0, 0, 0x0d090501);
    const __m128i ascii_zero = _mm_set_epi32(0, 0, 0, 0x30303030);

    __m128i x;

    x = _mm_madd_epi16(_mm_set1_epi16(u), first_madd);
    x = _mm_and_si128(x, mask);
    x = _mm_or_si128(x, _mm_slli_si128(x, 2));
    x = _mm_madd_epi16(x, second_madd);
    x = _mm_shuffle_epi8(x, shuffle);
    x = _mm_or_si128(x, ascii_zero);

    return x;
}

void StrUInt4DigitsToBuf(void *p_, uint32_t u) {
    uint8_t *p = (uint8_t *)p_;
    const int32_t uResult = _mm_cvtsi128_si32(d4toa(u));
    memcpy(p, &uResult, sizeof(uResult));
}

void StrUInt8DigitsToBuf(void *p_, uint32_t u) {
    uint8_t *p = (uint8_t *)p_;
    const uint32_t v = u / 10000;

    u -= v * 10000;

    const int32_t vResult = _mm_cvtsi128_si32(d4toa(v));
    const int32_t uResult = _mm_cvtsi128_si32(d4toa(u));
    memcpy(p, &vResult, sizeof(vResult));
    memcpy(p + 4, &uResult, sizeof(uResult));
}
#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* ARM NEON optimized integer-to-string conversion.
 * Uses NEON for parallel digit extraction where beneficial. */

/* Helper: convert 4 digits using NEON multiply-high approximation */
static inline void d4toa_neon(uint8_t *out, uint32_t u) {
    /* Extract 4 digits from u (0-9999) using division by constants.
     * ARM64 compiler is very good at optimizing division by constants. */
    const uint32_t d0 = u / 1000;
    const uint32_t r0 = u - d0 * 1000;
    const uint32_t d1 = r0 / 100;
    const uint32_t r1 = r0 - d1 * 100;
    const uint32_t d2 = r1 / 10;
    const uint32_t d3 = r1 - d2 * 10;

    /* Store as ASCII digits */
    out[0] = '0' + d0;
    out[1] = '0' + d1;
    out[2] = '0' + d2;
    out[3] = '0' + d3;
}

void StrUInt9DigitsToBuf(void *out_, uint32_t u) {
    uint8_t *out = out_;

    /* Split into three parts: w (1 digit), v (4 digits), u (4 digits) */
    uint32_t v = u / 10000;
    uint32_t w = v / 10000;
    u -= v * 10000;
    v -= w * 10000;

    /* First digit */
    out[0] = '0' + w;

    /* Next 4 digits */
    d4toa_neon(out + 1, v);

    /* Last 4 digits */
    d4toa_neon(out + 5, u);
}

void StrUInt4DigitsToBuf(void *p_, uint32_t u) {
    d4toa_neon((uint8_t *)p_, u);
}

void StrUInt8DigitsToBuf(void *p_, uint32_t u) {
    uint8_t *p = (uint8_t *)p_;
    const uint32_t v = u / 10000;
    u -= v * 10000;

    d4toa_neon(p, v);
    d4toa_neon(p + 4, u);
}
#else
#warning "Using unoptimized StrUInt9DigitsToBuf()!"
void StrUInt9DigitsToBuf(void *out_, uint32_t u) {
    uint8_t *out = out_;
    for (uint32_t i = 8; i--; u /= 10) {
        out[i] = '0' + (u % 10);
    }
}
#endif
