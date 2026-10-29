/* strUTF8Valid.c - UTF-8 Validation with SIMD/SWAR optimizations
 *
 * Provides fast UTF-8 validation using:
 *   - lookup tables over 16-byte blocks with SSSE3 or NEON
 *   - an ASCII scan with SSE2 alone
 *   - SWAR (SIMD Within A Register) fallback
 *
 * UTF-8 encoding rules (RFC 3629):
 *   0xxxxxxx                            - 1 byte  (0x00-0x7F)
 *   110xxxxx 10xxxxxx                   - 2 bytes (0xC2-0xDF, then 0x80-0xBF)
 *   1110xxxx 10xxxxxx 10xxxxxx          - 3 bytes (0xE0-0xEF, then 2x
 * 0x80-0xBF) 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx - 4 bytes (0xF0-0xF4, then 3x
 * 0x80-0xBF)
 *
 * Invalid sequences:
 *   - Overlong encodings (e.g., 0xC0 0x80 for NUL)
 *   - Surrogates (U+D800 to U+DFFF)
 *   - Codepoints > U+10FFFF
 *   - Continuation bytes without start byte
 *   - Start byte without enough continuation bytes
 */

#include "../str.h"

/* ====================================================================
 * Lookup Table Approach (used by scalar and SWAR)
 * ==================================================================== */

/* Character class lookup table for first byte classification.
 * Returns the expected total byte length (1-4) for valid start bytes,
 * or 0 for invalid start bytes (continuation bytes, 0xC0-0xC1, 0xF5-0xFF). */
static constexpr uint8_t utf8FirstByteLUT[256] = {
    /* 0x00-0x7F: ASCII (1 byte) */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x00-0x0F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x10-0x1F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x20-0x2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x30-0x3F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40-0x4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x50-0x5F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x60-0x6F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x70-0x7F */
    /* 0x80-0xBF: Continuation bytes (invalid as start - return 0) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x80-0x8F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x90-0x9F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0xA0-0xAF */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0xB0-0xBF */
    /* 0xC0-0xC1: Overlong 2-byte (invalid - return 0) */
    0, 0, /* 0xC0-0xC1 */
    /* 0xC2-0xDF: Valid 2-byte start */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,       /* 0xC2-0xCF */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* 0xD0-0xDF */
    /* 0xE0-0xEF: 3-byte start */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, /* 0xE0-0xEF */
    /* 0xF0-0xF4: 4-byte start */
    4, 4, 4, 4, 4, /* 0xF0-0xF4 */
    /* 0xF5-0xFF: Invalid (would produce codepoints > U+10FFFF) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 /* 0xF5-0xFF */
};

/* ====================================================================
 * Scalar Implementation (baseline for comparison and fallback)
 * ==================================================================== */

/* Validate a single UTF-8 sequence starting at 's'.
 * Returns the number of bytes consumed if valid, or 0 if invalid. */
DK_INLINE_ALWAYS size_t utf8ValidateSequence(const uint8_t *s,
                                             size_t remaining) {
    const uint8_t b0 = s[0];
    const uint8_t expectedLen = utf8FirstByteLUT[b0];

    if (expectedLen == 0 || expectedLen > remaining) {
        return 0; /* Invalid start byte or not enough bytes remaining */
    }

    if (expectedLen == 1) {
        return 1; /* ASCII - always valid */
    }

    /* Validate continuation bytes */
    for (size_t i = 1; i < expectedLen; i++) {
        if (!StrUtf8IsContinuation(s[i])) {
            return 0;
        }
    }

    /* Check for overlong encodings and invalid ranges */
    if (expectedLen == 2) {
        /* 2-byte: already filtered 0xC0-0xC1 via LUT */
        return 2;
    }

    if (expectedLen == 3) {
        const uint8_t b1 = s[1];
        /* E0 requires second byte >= 0xA0 (avoid overlong) */
        if (b0 == 0xE0 && b1 < 0xA0) {
            return 0;
        }
        /* ED requires second byte <= 0x9F (avoid surrogates U+D800-U+DFFF) */
        if (b0 == 0xED && b1 > 0x9F) {
            return 0;
        }
        return 3;
    }

    /* expectedLen == 4 */
    {
        const uint8_t b1 = s[1];
        /* F0 requires second byte >= 0x90 (avoid overlong) */
        if (b0 == 0xF0 && b1 < 0x90) {
            return 0;
        }
        /* F4 requires second byte <= 0x8F (avoid codepoints > U+10FFFF) */
        if (b0 == 0xF4 && b1 > 0x8F) {
            return 0;
        }
    }

    return 4;
}

/* Pure scalar baseline implementation for benchmarking */
bool StrUtf8ValidScalar(const void *_str, size_t len) {
    const uint8_t *s = (const uint8_t *)_str;

    while (len > 0) {
        const size_t consumed = utf8ValidateSequence(s, len);
        if (consumed == 0) {
            return false;
        }
        s += consumed;
        len -= consumed;
    }

    return true;
}

/* Null-terminated string validation (scalar) */
bool StrUtf8ValidCStrScalar(const char *str) {
    if (str == nullptr) {
        return false;
    }

    const uint8_t *s = (const uint8_t *)str;

    while (*s != '\0') {
        const uint8_t b0 = *s;
        const uint8_t expectedLen = utf8FirstByteLUT[b0];

        if (expectedLen == 0) {
            return false;
        }

        if (expectedLen == 1) {
            s++;
            continue;
        }

        /* Check continuation bytes exist and are valid */
        for (size_t i = 1; i < expectedLen; i++) {
            if (s[i] == '\0' || !StrUtf8IsContinuation(s[i])) {
                return false;
            }
        }

        /* Overlong and range checks */
        if (expectedLen == 3) {
            if (b0 == 0xE0 && s[1] < 0xA0) {
                return false;
            }
            if (b0 == 0xED && s[1] > 0x9F) {
                return false;
            }
        } else if (expectedLen == 4) {
            if (b0 == 0xF0 && s[1] < 0x90) {
                return false;
            }
            if (b0 == 0xF4 && s[1] > 0x8F) {
                return false;
            }
        }

        s += expectedLen;
    }

    return true;
}

/* ====================================================================
 * SIMD-optimized UTF-8 Validation
 * ====================================================================
 * Strategy: Use SIMD to quickly scan for ASCII-only blocks, then fall
 * back to scalar validation when non-ASCII bytes are encountered.
 *
 * For long ASCII runs, this provides significant speedup. For heavily
 * non-ASCII text, we fall back to careful scalar checking. */

#if defined(__SSSE3__) || defined(__ARM_NEON)
/* Lookup-table validation (Keiser and Lemire, "Validating UTF-8 In Less
 * Than One Instruction Per Byte", 2021): each 16-byte block is checked
 * whole.  Three table lookups, on the previous byte's high and low nibbles
 * and this byte's high nibble, each give the error classes the pair could
 * belong to; their AND is the classes the pair does belong to.  A byte two
 * or three after a 3- or 4-byte lead must continue it: that requirement
 * cancels the "two continuations" class, and anything else left over is
 * an error.  An ASCII block costs one test, and a sequence cut off at the
 * end of the input, or before an ASCII block, is an error. */
#if defined(__SSSE3__)
#include <tmmintrin.h>
typedef __m128i utf8Vec;
#define UTF8_LOAD(p) _mm_loadu_si128((const __m128i *)(p))
#define UTF8_SPLAT(b) _mm_set1_epi8((char)(b))
#define UTF8_AND(a, b) _mm_and_si128(a, b)
#define UTF8_OR(a, b) _mm_or_si128(a, b)
#define UTF8_XOR(a, b) _mm_xor_si128(a, b)
#define UTF8_SUBSAT(a, b) _mm_subs_epu8(a, b)
#define UTF8_HIGH_NIBBLE(v)                                                    \
    _mm_and_si128(_mm_srli_epi16(v, 4), UTF8_SPLAT(0x0F))
#define UTF8_LOW_NIBBLE(v) _mm_and_si128(v, UTF8_SPLAT(0x0F))
#define UTF8_LOOKUP(table, idx) _mm_shuffle_epi8(table, idx)
/* The 16 bytes ending 'n' before the end of 'cur', 'prev' before it */
#define UTF8_PREV(cur, prev, n) _mm_alignr_epi8(cur, prev, 16 - (n))
#define UTF8_IS_ASCII(v) (_mm_movemask_epi8(v) == 0)
#define UTF8_ANY(v)                                                            \
    (_mm_movemask_epi8(_mm_cmpeq_epi8(v, UTF8_SPLAT(0))) != 0xFFFF)
#else
#include <arm_neon.h>
typedef uint8x16_t utf8Vec;
#define UTF8_LOAD(p) vld1q_u8((const uint8_t *)(p))
#define UTF8_SPLAT(b) vdupq_n_u8(b)
#define UTF8_AND(a, b) vandq_u8(a, b)
#define UTF8_OR(a, b) vorrq_u8(a, b)
#define UTF8_XOR(a, b) veorq_u8(a, b)
#define UTF8_SUBSAT(a, b) vqsubq_u8(a, b)
#define UTF8_HIGH_NIBBLE(v) vshrq_n_u8(v, 4)
#define UTF8_LOW_NIBBLE(v) vandq_u8(v, vdupq_n_u8(0x0F))
#define UTF8_LOOKUP(table, idx) vqtbl1q_u8(table, idx)
#define UTF8_PREV(cur, prev, n) vextq_u8(prev, cur, 16 - (n))
#define UTF8_IS_ASCII(v) (vmaxvq_u8(v) < 0x80)
#define UTF8_ANY(v) (vmaxvq_u8(v) != 0)
#endif

/* Error classes of a byte pair (previous byte, this byte) */
enum {
    UTF8_TOO_SHORT = 1 << 0,      /* a lead not followed by a continuation */
    UTF8_TOO_LONG = 1 << 1,       /* ASCII followed by a continuation */
    UTF8_OVERLONG_3 = 1 << 2,     /* E0 80..9F */
    UTF8_TOO_LARGE = 1 << 3,      /* F4 90..BF, or F5 and up */
    UTF8_SURROGATE = 1 << 4,      /* ED A0..BF */
    UTF8_OVERLONG_2 = 1 << 5,     /* C0 or C1 */
    UTF8_TOO_LARGE_1000 = 1 << 6, /* F5..FF 80..8F */
    UTF8_OVERLONG_4 = 1 << 6,     /* F0 80..8F (never with the above) */
    UTF8_TWO_CONTS = 1 << 7,      /* two continuations in a row */
    UTF8_CARRY = UTF8_TOO_SHORT | UTF8_TOO_LONG | UTF8_TWO_CONTS,
};

/* By the previous byte's high nibble */
static constexpr uint8_t utf8Prev1High_[16] = {
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TOO_LONG,
    UTF8_TWO_CONTS,
    UTF8_TWO_CONTS,
    UTF8_TWO_CONTS,
    UTF8_TWO_CONTS,
    UTF8_TOO_SHORT | UTF8_OVERLONG_2,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT | UTF8_OVERLONG_3 | UTF8_SURROGATE,
    UTF8_TOO_SHORT | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000 | UTF8_OVERLONG_4,
};

/* By the previous byte's low nibble */
static constexpr uint8_t utf8Prev1Low_[16] = {
    UTF8_CARRY | UTF8_OVERLONG_3 | UTF8_OVERLONG_2 | UTF8_OVERLONG_4,
    UTF8_CARRY | UTF8_OVERLONG_2,
    UTF8_CARRY,
    UTF8_CARRY,
    UTF8_CARRY | UTF8_TOO_LARGE,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000 | UTF8_SURROGATE,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
    UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000,
};

/* By this byte's high nibble */
static constexpr uint8_t utf8CurHigh_[16] = {
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS | UTF8_OVERLONG_3 |
        UTF8_TOO_LARGE_1000 | UTF8_OVERLONG_4,
    UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS | UTF8_OVERLONG_3 |
        UTF8_TOO_LARGE,
    UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS | UTF8_SURROGATE |
        UTF8_TOO_LARGE,
    UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS | UTF8_SURROGATE |
        UTF8_TOO_LARGE,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
    UTF8_TOO_SHORT,
};

/* A block's last three bytes leave a sequence open when the third from
 * the end starts a 4-byte one, the second a 3- or 4-byte one, or the last
 * any multibyte one */
static constexpr uint8_t utf8OpenAtEnd_[16] = {
    255, 255, 255, 255, 255, 255,      255,      255,
    255, 255, 255, 255, 255, 0xF0 - 1, 0xE0 - 1, 0xC0 - 1,
};

/* The errors of block 'cur' after block 'prev' (nonzero: invalid) */
DK_INLINE_ALWAYS utf8Vec utf8BlockErrors_(const utf8Vec cur,
                                          const utf8Vec prev) {
    const utf8Vec prev1 = UTF8_PREV(cur, prev, 1);
    const utf8Vec classes = UTF8_AND(
        UTF8_AND(
            UTF8_LOOKUP(UTF8_LOAD(utf8Prev1High_), UTF8_HIGH_NIBBLE(prev1)),
            UTF8_LOOKUP(UTF8_LOAD(utf8Prev1Low_), UTF8_LOW_NIBBLE(prev1))),
        UTF8_LOOKUP(UTF8_LOAD(utf8CurHigh_), UTF8_HIGH_NIBBLE(cur)));

    /* Only E0..FF two back, or F0..FF three back, reach 0x80 here */
    const utf8Vec third =
        UTF8_SUBSAT(UTF8_PREV(cur, prev, 2), UTF8_SPLAT(0xE0 - 0x80));
    const utf8Vec fourth =
        UTF8_SUBSAT(UTF8_PREV(cur, prev, 3), UTF8_SPLAT(0xF0 - 0x80));
    const utf8Vec mustContinue =
        UTF8_AND(UTF8_OR(third, fourth), UTF8_SPLAT(0x80));
    return UTF8_XOR(mustContinue, classes);
}

bool StrUtf8Valid(const void *_str, size_t len) {
    const uint8_t *s = (const uint8_t *)_str;
    const utf8Vec zero = UTF8_SPLAT(0);
    utf8Vec prev = zero;
    utf8Vec errors = zero;
    utf8Vec open = zero; /* the last block left a sequence open */
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        const utf8Vec cur = UTF8_LOAD(s + i);
        if (UTF8_IS_ASCII(cur)) {
            errors = UTF8_OR(errors, open);
            open = zero;
        } else {
            errors = UTF8_OR(errors, utf8BlockErrors_(cur, prev));
            open = UTF8_SUBSAT(cur, UTF8_LOAD(utf8OpenAtEnd_));
        }

        prev = cur;
    }

    if (i < len) {
        /* The tail, padded with NULs: a sequence it leaves open is cut
         * short by the padding */
        uint8_t tail[16] = {};
        memcpy(tail, s + i, len - i);
        errors = UTF8_OR(errors, utf8BlockErrors_(UTF8_LOAD(tail), prev));
    } else {
        errors = UTF8_OR(errors, open);
    }

    return !UTF8_ANY(errors);
}

bool StrUtf8ValidCStr(const char *str) {
    return StrUtf8ValidCStrScalar(str);
}

#elif defined(__SSE2__)
#include <emmintrin.h>

/* SSE2 optimized validation - fast ASCII scanning with scalar fallback */
bool StrUtf8Valid(const void *_str, size_t len) {
    const uint8_t *s = (const uint8_t *)_str;

    /* Process 16 bytes at a time while we have ASCII data */
    while (len >= 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)s);

        /* Check if all bytes are ASCII (high bit clear) */
        /* movemask returns 1 bit per byte where high bit is set */
        uint32_t mask = (uint32_t)_mm_movemask_epi8(chunk);

        if (mask == 0) {
            /* All 16 bytes are ASCII - fast path */
            s += 16;
            len -= 16;
        } else {
            /* Found non-ASCII byte(s) - find first non-ASCII position */
            uint32_t firstNonAscii = stdc_trailing_zeros(mask);

            /* Skip past any leading ASCII bytes */
            s += firstNonAscii;
            len -= firstNonAscii;

            /* Validate the multibyte sequence at this position */
            const size_t consumed = utf8ValidateSequence(s, len);
            if (consumed == 0) {
                return false;
            }
            s += consumed;
            len -= consumed;
        }
    }

    /* Handle remaining bytes with scalar code */
    while (len > 0) {
        const size_t consumed = utf8ValidateSequence(s, len);
        if (consumed == 0) {
            return false;
        }
        s += consumed;
        len -= consumed;
    }

    return true;
}

/* Null-terminated string validation with SSE2 */
bool StrUtf8ValidCStr(const char *str) {
    if (str == nullptr) {
        return false;
    }

    const uint8_t *s = (const uint8_t *)str;

    /* Fast scan for ASCII while also checking for NUL */
    while (1) {
        /* Check if we can safely load 16 bytes */
        /* For null-terminated strings, we need to be more careful */
        /* Check 8 bytes at a time using scalar first to find NUL */

        /* Quick check: is current position NUL? */
        if (*s == '\0') {
            return true;
        }

        /* Is this ASCII? */
        if (*s < 0x80) {
            s++;
            continue;
        }

        /* Non-ASCII: validate the sequence */
        const uint8_t b0 = *s;
        const uint8_t expectedLen = utf8FirstByteLUT[b0];

        if (expectedLen == 0) {
            return false;
        }

        /* Check continuation bytes exist and are valid */
        for (size_t i = 1; i < expectedLen; i++) {
            if (s[i] == '\0' || !StrUtf8IsContinuation(s[i])) {
                return false;
            }
        }

        /* Overlong and range checks */
        if (expectedLen == 3) {
            if (b0 == 0xE0 && s[1] < 0xA0) {
                return false;
            }
            if (b0 == 0xED && s[1] > 0x9F) {
                return false;
            }
        } else if (expectedLen == 4) {
            if (b0 == 0xF0 && s[1] < 0x90) {
                return false;
            }
            if (b0 == 0xF4 && s[1] > 0x8F) {
                return false;
            }
        }

        s += expectedLen;
    }
}

#else
/* Fallback: use SWAR (SIMD Within A Register) on 8 bytes at a time */

/* SWAR constants for 8-byte processing */
#define UTF8_VALID_STEP size_t
#define UTF8_VALID_STEP_SIZE sizeof(UTF8_VALID_STEP)
#define UTF8_VALID_ONEMASK ((UTF8_VALID_STEP)(-1) / 0xFF)
#define UTF8_VALID_HIGHMASK (UTF8_VALID_ONEMASK * 0x80)

/* Check if all bytes in a word have high bit clear (all ASCII) */
#define UTF8_VALID_ALL_ASCII(u) (((u) & UTF8_VALID_HIGHMASK) == 0)

bool StrUtf8Valid(const void *_str, size_t len) {
    const uint8_t *s = (const uint8_t *)_str;

    /* Handle initial misaligned bytes */
    while (len > 0 && !DK_IS_STEP_ALIGNED(UTF8_VALID_STEP_SIZE, s)) {
        if (*s < 0x80) {
            s++;
            len--;
        } else {
            const size_t consumed = utf8ValidateSequence(s, len);
            if (consumed == 0) {
                return false;
            }
            s += consumed;
            len -= consumed;
        }
    }

    /* Process aligned blocks of 8 bytes */
    while (len >= UTF8_VALID_STEP_SIZE) {
        UTF8_VALID_STEP u;
        memcpy(&u, s, UTF8_VALID_STEP_SIZE);

        if (UTF8_VALID_ALL_ASCII(u)) {
            /* All 8 bytes are ASCII - fast path */
            s += UTF8_VALID_STEP_SIZE;
            len -= UTF8_VALID_STEP_SIZE;
        } else {
            /* Found non-ASCII - process byte by byte */
            while (len > 0 && *s < 0x80) {
                s++;
                len--;
            }

            if (len == 0) {
                return true;
            }

            const size_t consumed = utf8ValidateSequence(s, len);
            if (consumed == 0) {
                return false;
            }
            s += consumed;
            len -= consumed;
        }
    }

    /* Handle remaining bytes */
    while (len > 0) {
        const size_t consumed = utf8ValidateSequence(s, len);
        if (consumed == 0) {
            return false;
        }
        s += consumed;
        len -= consumed;
    }

    return true;
}

bool StrUtf8ValidCStr(const char *str) {
    return StrUtf8ValidCStrScalar(str);
}

#endif /* SIMD/NEON/fallback selection */

/* ====================================================================
 * Additional UTF-8 Utilities
 * ==================================================================== */

/* Count the number of UTF-8 codepoints in a valid UTF-8 string.
 * Returns 0 if the string is invalid.
 * This combines validation with counting for efficiency. */
size_t StrUtf8ValidCount(const void *_str, size_t len, bool *valid) {
    const uint8_t *s = (const uint8_t *)_str;
    size_t count = 0;

    while (len > 0) {
        const uint8_t b0 = *s;

        if (b0 < 0x80) {
            /* ASCII - fast path for common case */
            s++;
            len--;
            count++;
            continue;
        }

        const size_t consumed = utf8ValidateSequence(s, len);
        if (consumed == 0) {
            *valid = false;
            return count;
        }
        s += consumed;
        len -= consumed;
        count++;
    }

    *valid = true;
    return count;
}

/* Get the byte length needed for a specific number of UTF-8 codepoints.
 * Validates while counting. Returns 0 if invalid or not enough codepoints. */
size_t StrUtf8ValidCountBytes(const void *_str, size_t len,
                              size_t numCodepoints, bool *valid) {
    const uint8_t *s = (const uint8_t *)_str;
    const uint8_t *start = s;
    size_t count = 0;

    while (len > 0 && count < numCodepoints) {
        const size_t consumed = utf8ValidateSequence(s, len);
        if (consumed == 0) {
            *valid = false;
            return 0;
        }
        s += consumed;
        len -= consumed;
        count++;
    }

    *valid = true;
    return (size_t)(s - start);
}

/* Decode a UTF-8 sequence to a Unicode codepoint.
 * Returns the codepoint and advances *str by the number of bytes consumed.
 * Returns STR_UTF8_INVALID on error (and does not advance). */
uint32_t StrUtf8Decode(const uint8_t **str, size_t *remaining) {
    uint32_t cp;
    const size_t taken = StrUtf8DecodeOne(*str, *remaining, &cp);
    if (!taken) {
        return STR_UTF8_INVALID;
    }

    *str += taken;
    *remaining -= taken;
    return cp;
}

/* Encode a Unicode codepoint to UTF-8.
 * Returns the number of bytes written (1-4), or 0 on error.
 * 'dst' must have at least 4 bytes available. */
size_t StrUtf8Encode(uint8_t *dst, uint32_t codepoint) {
    if (codepoint < 0x80) {
        dst[0] = (uint8_t)codepoint;
        return 1;
    }

    if (codepoint < 0x800) {
        dst[0] = 0xC0 | (uint8_t)(codepoint >> 6);
        dst[1] = 0x80 | (uint8_t)(codepoint & 0x3F);
        return 2;
    }

    if (codepoint < 0x10000) {
        /* Check for surrogates */
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return 0; /* Surrogates are invalid */
        }
        dst[0] = 0xE0 | (uint8_t)(codepoint >> 12);
        dst[1] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
        dst[2] = 0x80 | (uint8_t)(codepoint & 0x3F);
        return 3;
    }

    if (codepoint <= 0x10FFFF) {
        dst[0] = 0xF0 | (uint8_t)(codepoint >> 18);
        dst[1] = 0x80 | (uint8_t)((codepoint >> 12) & 0x3F);
        dst[2] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
        dst[3] = 0x80 | (uint8_t)(codepoint & 0x3F);
        return 4;
    }

    return 0; /* Codepoint too large */
}

/* Get the expected byte length for a codepoint */
size_t StrUtf8CodepointLen(uint32_t codepoint) {
    if (codepoint < 0x80) {
        return 1;
    }
    if (codepoint < 0x800) {
        return 2;
    }
    if (codepoint < 0x10000) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return 0; /* Invalid surrogate */
        }
        return 3;
    }
    if (codepoint <= 0x10FFFF) {
        return 4;
    }
    return 0; /* Invalid */
}

/* Get the byte length of a UTF-8 sequence from its first byte.
 * Returns 0 for invalid start bytes. */
size_t StrUtf8SequenceLen(uint8_t firstByte) {
    return utf8FirstByteLUT[firstByte];
}
