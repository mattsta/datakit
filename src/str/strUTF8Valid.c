/* strUTF8Valid.c - UTF-8 Validation with SIMD/SWAR optimizations
 *
 * Provides fast UTF-8 validation using:
 *   - SSE2 on x86-64
 *   - NEON on ARM64
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
static const uint8_t utf8FirstByteLUT[256] = {
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

/* Check if byte is a valid continuation byte (10xxxxxx = 0x80-0xBF) */
#define IS_CONTINUATION(b) (((b) & 0xC0) == 0x80)

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
        if (!IS_CONTINUATION(s[i])) {
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
    if (str == NULL) {
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
            if (s[i] == '\0' || !IS_CONTINUATION(s[i])) {
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

#if defined(__SSE2__)
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
            uint32_t firstNonAscii = __builtin_ctz(mask);

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
    if (str == NULL) {
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
            if (s[i] == '\0' || !IS_CONTINUATION(s[i])) {
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

#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* ARM NEON optimized validation - fast ASCII scanning with scalar fallback */
bool StrUtf8Valid(const void *_str, size_t len) {
    const uint8_t *s = (const uint8_t *)_str;

    /* Process 16 bytes at a time while we have ASCII data */
    while (len >= 16) {
        uint8x16_t chunk = vld1q_u8(s);

        /* Check if any byte has high bit set (non-ASCII) */
        /* Using vget_high/vget_low to check all 16 bytes */
        uint8x16_t highBits = vshrq_n_u8(chunk, 7);
        uint64_t hiSum = vaddlvq_u8(highBits);

        if (hiSum == 0) {
            /* All 16 bytes are ASCII - fast path */
            s += 16;
            len -= 16;
        } else {
            /* Found non-ASCII - process byte by byte until we clear it */
            while (len > 0 && *s < 0x80) {
                s++;
                len--;
            }

            if (len == 0) {
                return true;
            }

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

/* Null-terminated string validation with NEON */
bool StrUtf8ValidCStr(const char *str) {
    /* Use the scalar implementation for null-terminated strings
     * since we need to check for NUL terminator anyway */
    return StrUtf8ValidCStrScalar(str);
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
 * Returns 0xFFFFFFFF on error (and does not advance). */
uint32_t StrUtf8Decode(const uint8_t **str, size_t *remaining) {
    const uint8_t *s = *str;
    const size_t len = *remaining;

    if (len == 0) {
        return 0xFFFFFFFF;
    }

    const uint8_t b0 = s[0];

    /* ASCII fast path */
    if (b0 < 0x80) {
        *str = s + 1;
        *remaining = len - 1;
        return b0;
    }

    const uint8_t expectedLen = utf8FirstByteLUT[b0];
    if (expectedLen == 0 || expectedLen > len) {
        return 0xFFFFFFFF;
    }

    /* Validate and decode continuation bytes */
    uint32_t cp;

    switch (expectedLen) {
    case 2:
        if (!IS_CONTINUATION(s[1])) {
            return 0xFFFFFFFF;
        }
        cp = ((uint32_t)(b0 & 0x1F) << 6) | (s[1] & 0x3F);
        break;

    case 3:
        if (!IS_CONTINUATION(s[1]) || !IS_CONTINUATION(s[2])) {
            return 0xFFFFFFFF;
        }
        /* Check overlong and surrogate */
        if (b0 == 0xE0 && s[1] < 0xA0) {
            return 0xFFFFFFFF;
        }
        if (b0 == 0xED && s[1] > 0x9F) {
            return 0xFFFFFFFF;
        }
        cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
             (s[2] & 0x3F);
        break;

    case 4:
        if (!IS_CONTINUATION(s[1]) || !IS_CONTINUATION(s[2]) ||
            !IS_CONTINUATION(s[3])) {
            return 0xFFFFFFFF;
        }
        /* Check overlong and max codepoint */
        if (b0 == 0xF0 && s[1] < 0x90) {
            return 0xFFFFFFFF;
        }
        if (b0 == 0xF4 && s[1] > 0x8F) {
            return 0xFFFFFFFF;
        }
        cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        break;

    default:
        return 0xFFFFFFFF;
    }

    *str = s + expectedLen;
    *remaining = len - expectedLen;
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
