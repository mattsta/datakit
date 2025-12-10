/* strUTF8Case.c - ASCII Case Conversion Operations
 *
 * Provides ASCII case conversion operations:
 *   - StrAsciiToLower: In-place lowercase conversion
 *   - StrAsciiToUpper: In-place uppercase conversion
 *   - StrAsciiToLowerCopy: Copy with lowercase conversion
 *   - StrAsciiToUpperCopy: Copy with uppercase conversion
 *
 * These operate only on ASCII letters (A-Z, a-z). Non-ASCII bytes
 * (including UTF-8 multibyte sequences) are passed through unchanged.
 *
 * SIMD-optimized for SSE2, NEON, and falls back to scalar with SWAR.
 */

#include "../str.h"
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* ====================================================================
 * StrAsciiToLower - In-place lowercase conversion
 * ==================================================================== */

/* Convert ASCII uppercase letters to lowercase in-place.
 * Non-ASCII bytes are unchanged.
 */
void StrAsciiToLower(void *str, size_t len) {
    uint8_t *data = (uint8_t *)str;

#if defined(__SSE2__)
    /* SSE2: Process 16 bytes at a time */
    const __m128i upperA = _mm_set1_epi8('A');
    const __m128i upperZ = _mm_set1_epi8('Z');
    const __m128i diff = _mm_set1_epi8(32); /* 'a' - 'A' = 32 */

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(data + i));

        /* Find bytes in range ['A', 'Z'] */
        __m128i geA =
            _mm_cmpgt_epi8(chunk, _mm_sub_epi8(upperA, _mm_set1_epi8(1)));
        __m128i leZ =
            _mm_cmplt_epi8(chunk, _mm_add_epi8(upperZ, _mm_set1_epi8(1)));
        __m128i mask = _mm_and_si128(geA, leZ);

        /* Add 32 only to uppercase letters */
        __m128i toAdd = _mm_and_si128(mask, diff);
        chunk = _mm_add_epi8(chunk, toAdd);

        _mm_storeu_si128((__m128i *)(data + i), chunk);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'A' && data[i] <= 'Z') {
            data[i] += 32;
        }
    }

#elif defined(__aarch64__) || defined(__ARM_NEON)
    /* NEON: Process 16 bytes at a time */
    const uint8x16_t upperA = vdupq_n_u8('A');
    const uint8x16_t upperZ = vdupq_n_u8('Z');
    const uint8x16_t diff = vdupq_n_u8(32);

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8(data + i);

        /* Find bytes in range ['A', 'Z'] */
        uint8x16_t geA = vcgeq_u8(chunk, upperA);
        uint8x16_t leZ = vcleq_u8(chunk, upperZ);
        uint8x16_t mask = vandq_u8(geA, leZ);

        /* Add 32 only to uppercase letters */
        uint8x16_t toAdd = vandq_u8(mask, diff);
        chunk = vaddq_u8(chunk, toAdd);

        vst1q_u8(data + i, chunk);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'A' && data[i] <= 'Z') {
            data[i] += 32;
        }
    }

#else
    /* Scalar fallback with SWAR optimization */
    size_t i = 0;

    /* Process 8 bytes at a time using SWAR */
    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, data + i, 8);

        /* For each byte, check if it's in ['A', 'Z'] range
         * and add 0x20 if so */
        uint64_t lower = chunk + 0x3f3f3f3f3f3f3f3fULL; /* A-1 = 0x40 -> 0x7f */
        uint64_t upper = chunk + 0x2525252525252525ULL; /* Z+1 = 0x5b -> 0x80 */

        /* Bytes where high bit set in lower but not upper are in range */
        uint64_t mask = (lower ^ upper) & 0x8080808080808080ULL;
        /* Create mask of 0x20 for uppercase letters */
        mask = (mask >> 2) | (mask >> 7);
        mask &= 0x2020202020202020ULL;

        chunk |= mask;
        memcpy(data + i, &chunk, 8);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'A' && data[i] <= 'Z') {
            data[i] += 32;
        }
    }
#endif
}

/* ====================================================================
 * StrAsciiToUpper - In-place uppercase conversion
 * ==================================================================== */

/* Convert ASCII lowercase letters to uppercase in-place.
 * Non-ASCII bytes are unchanged.
 */
void StrAsciiToUpper(void *str, size_t len) {
    uint8_t *data = (uint8_t *)str;

#if defined(__SSE2__)
    /* SSE2: Process 16 bytes at a time */
    const __m128i lowerA = _mm_set1_epi8('a');
    const __m128i lowerZ = _mm_set1_epi8('z');
    const __m128i diff = _mm_set1_epi8(32);

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(data + i));

        /* Find bytes in range ['a', 'z'] */
        __m128i geA =
            _mm_cmpgt_epi8(chunk, _mm_sub_epi8(lowerA, _mm_set1_epi8(1)));
        __m128i leZ =
            _mm_cmplt_epi8(chunk, _mm_add_epi8(lowerZ, _mm_set1_epi8(1)));
        __m128i mask = _mm_and_si128(geA, leZ);

        /* Subtract 32 only from lowercase letters */
        __m128i toSub = _mm_and_si128(mask, diff);
        chunk = _mm_sub_epi8(chunk, toSub);

        _mm_storeu_si128((__m128i *)(data + i), chunk);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'a' && data[i] <= 'z') {
            data[i] -= 32;
        }
    }

#elif defined(__aarch64__) || defined(__ARM_NEON)
    /* NEON: Process 16 bytes at a time */
    const uint8x16_t lowerA = vdupq_n_u8('a');
    const uint8x16_t lowerZ = vdupq_n_u8('z');
    const uint8x16_t diff = vdupq_n_u8(32);

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8(data + i);

        /* Find bytes in range ['a', 'z'] */
        uint8x16_t geA = vcgeq_u8(chunk, lowerA);
        uint8x16_t leZ = vcleq_u8(chunk, lowerZ);
        uint8x16_t mask = vandq_u8(geA, leZ);

        /* Subtract 32 only from lowercase letters */
        uint8x16_t toSub = vandq_u8(mask, diff);
        chunk = vsubq_u8(chunk, toSub);

        vst1q_u8(data + i, chunk);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'a' && data[i] <= 'z') {
            data[i] -= 32;
        }
    }

#else
    /* Scalar fallback with SWAR optimization */
    size_t i = 0;

    /* Process 8 bytes at a time using SWAR */
    for (; i + 8 <= len; i += 8) {
        uint64_t chunk;
        memcpy(&chunk, data + i, 8);

        /* For each byte, check if it's in ['a', 'z'] range
         * and subtract 0x20 if so */
        uint64_t lower = chunk + 0x1f1f1f1f1f1f1f1fULL; /* a-1 = 0x60 -> 0x7f */
        uint64_t upper = chunk + 0x0505050505050505ULL; /* z+1 = 0x7b -> 0x80 */

        /* Bytes where high bit set in lower but not upper are in range */
        uint64_t mask = (lower ^ upper) & 0x8080808080808080ULL;
        /* Create mask of 0x20 for lowercase letters */
        mask = (mask >> 2) | (mask >> 7);
        mask &= 0x2020202020202020ULL;

        chunk &= ~mask; /* Clear bit 5 for lowercase letters */
        memcpy(data + i, &chunk, 8);
    }

    /* Handle remaining bytes */
    for (; i < len; i++) {
        if (data[i] >= 'a' && data[i] <= 'z') {
            data[i] -= 32;
        }
    }
#endif
}

/* ====================================================================
 * StrAsciiToLowerCopy - Copy with lowercase conversion
 * ==================================================================== */

/* Copy string to buffer with ASCII lowercase conversion.
 * Returns bytes written (not including null terminator if added).
 * Does NOT null-terminate unless dstLen > srcLen.
 */
size_t StrAsciiToLowerCopy(void *dst, size_t dstLen, const void *src,
                           size_t srcLen) {
    size_t copyLen = (srcLen < dstLen) ? srcLen : dstLen;

    if (copyLen > 0) {
        memcpy(dst, src, copyLen);
        StrAsciiToLower(dst, copyLen);
    }

    return copyLen;
}

/* ====================================================================
 * StrAsciiToUpperCopy - Copy with uppercase conversion
 * ==================================================================== */

/* Copy string to buffer with ASCII uppercase conversion.
 * Returns bytes written (not including null terminator if added).
 * Does NOT null-terminate unless dstLen > srcLen.
 */
size_t StrAsciiToUpperCopy(void *dst, size_t dstLen, const void *src,
                           size_t srcLen) {
    size_t copyLen = (srcLen < dstLen) ? srcLen : dstLen;

    if (copyLen > 0) {
        memcpy(dst, src, copyLen);
        StrAsciiToUpper(dst, copyLen);
    }

    return copyLen;
}

/* ====================================================================
 * StrAsciiIsLower / StrAsciiIsUpper - Case checking
 * ==================================================================== */

/* Check if all ASCII letters in the string are lowercase.
 * Non-ASCII bytes and non-letter ASCII are ignored.
 */
bool StrAsciiIsLower(const void *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;

    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            return false;
        }
    }
    return true;
}

/* Check if all ASCII letters in the string are uppercase.
 * Non-ASCII bytes and non-letter ASCII are ignored.
 */
bool StrAsciiIsUpper(const void *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;

    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            return false;
        }
    }
    return true;
}
