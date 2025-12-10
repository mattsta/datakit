/* strUTF8Cursor.c - UTF-8 Cursor/Iterator Operations with SIMD optimization
 *
 * Provides efficient cursor navigation through UTF-8 strings:
 *   - StrUtf8Advance: Move forward by N codepoints
 *   - StrUtf8Retreat: Move backward by N codepoints
 *   - StrUtf8Peek: Get codepoint at position without advancing
 *   - StrUtf8OffsetAt: Get byte offset for Nth codepoint
 *   - StrUtf8IndexAt: Get codepoint index for byte offset
 *
 * Uses SIMD for fast ASCII detection, scalar fallback for multi-byte sequences.
 */

#include "../str.h"
#include <string.h>

/* ====================================================================
 * Shared Definitions
 * ==================================================================== */

/* Check if byte is a UTF-8 start byte (not a continuation byte) */
#define IS_START_BYTE(b) (((b) & 0xC0) != 0x80)

/* Check if byte is a continuation byte (10xxxxxx) */
#define IS_CONTINUATION(b) (((b) & 0xC0) == 0x80)

/* Get expected sequence length from first byte (same table as strUTF8Valid.c)
 */
static const uint8_t utf8SeqLenLUT[256] = {
    /* 0x00-0x7F: ASCII (1 byte) */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x00-0x0F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x10-0x1F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x20-0x2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x30-0x3F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x40-0x4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x50-0x5F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x60-0x6F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x70-0x7F */
    /* 0x80-0xBF: Continuation bytes (skip = 1, just move past) */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x80-0x8F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0x90-0x9F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0xA0-0xAF */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0xB0-0xBF */
    /* 0xC0-0xC1: Overlong 2-byte (invalid, but treat as 1 to skip) */
    1, 1, /* 0xC0-0xC1 */
    /* 0xC2-0xDF: Valid 2-byte start */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,       /* 0xC2-0xCF */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, /* 0xD0-0xDF */
    /* 0xE0-0xEF: 3-byte start */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, /* 0xE0-0xEF */
    /* 0xF0-0xF4: 4-byte start */
    4, 4, 4, 4, 4, /* 0xF0-0xF4 */
    /* 0xF5-0xFF: Invalid (treat as 1 to skip) */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 /* 0xF5-0xFF */
};

/* ====================================================================
 * Scalar Implementations (baseline)
 * ==================================================================== */

/* Advance cursor by one codepoint, returns bytes consumed */
DK_INLINE_ALWAYS size_t utf8AdvanceOne(const uint8_t *s, size_t remaining) {
    if (remaining == 0) {
        return 0;
    }

    size_t seqLen = utf8SeqLenLUT[s[0]];
    return (seqLen <= remaining) ? seqLen : remaining;
}

/* Move to the start of the previous codepoint */
DK_INLINE_ALWAYS size_t utf8RetreatOne(const uint8_t *start,
                                       const uint8_t *pos) {
    if (pos <= start) {
        return 0;
    }

    /* Move back at least one byte */
    const uint8_t *p = pos - 1;

    /* Skip continuation bytes (max 3 for UTF-8) */
    size_t backCount = 1;
    while (p > start && IS_CONTINUATION(*p) && backCount < 4) {
        p--;
        backCount++;
    }

    return backCount;
}

/* ====================================================================
 * SIMD-optimized Cursor Operations
 * ==================================================================== */

#if defined(__SSE2__)
#include <emmintrin.h>

/* SSE2 optimized: fast ASCII scanning for advance */
const uint8_t *StrUtf8Advance(const void *str, size_t len, size_t n) {
    const uint8_t *s = (const uint8_t *)str;

    if (n == 0 || len == 0) {
        return s;
    }

    /* Fast path: scan 16 bytes at a time for ASCII-only regions */
    while (n > 0 && len >= 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)s);
        uint32_t mask = (uint32_t)_mm_movemask_epi8(chunk);

        if (mask == 0) {
            /* All 16 bytes are ASCII - can we skip all of them? */
            if (n >= 16) {
                s += 16;
                len -= 16;
                n -= 16;
            } else {
                /* Only skip 'n' bytes */
                s += n;
                len -= n;
                n = 0;
            }
        } else {
            /* Found non-ASCII - process byte by byte */
            uint32_t firstNonAscii = __builtin_ctz(mask);

            /* Skip ASCII bytes before the non-ASCII */
            if (firstNonAscii > 0) {
                size_t skip = (firstNonAscii < n) ? firstNonAscii : n;
                s += skip;
                len -= skip;
                n -= skip;
            }

            if (n == 0) {
                break;
            }

            /* Handle the multi-byte sequence */
            size_t seqLen = utf8AdvanceOne(s, len);
            if (seqLen == 0) {
                break;
            }
            s += seqLen;
            len -= seqLen;
            n--;
        }
    }

    /* Handle remaining bytes */
    while (n > 0 && len > 0) {
        size_t seqLen = utf8AdvanceOne(s, len);
        if (seqLen == 0) {
            break;
        }
        s += seqLen;
        len -= seqLen;
        n--;
    }

    return s;
}

#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* NEON optimized: fast ASCII scanning for advance */
const uint8_t *StrUtf8Advance(const void *str, size_t len, size_t n) {
    const uint8_t *s = (const uint8_t *)str;

    if (n == 0 || len == 0) {
        return s;
    }

    /* Fast path: scan 16 bytes at a time for ASCII-only regions */
    while (n > 0 && len >= 16) {
        uint8x16_t chunk = vld1q_u8(s);

        /* Check if any byte has high bit set (non-ASCII) */
        uint8x16_t highBits = vshrq_n_u8(chunk, 7);
        uint64_t hiSum = vaddlvq_u8(highBits);

        if (hiSum == 0) {
            /* All 16 bytes are ASCII */
            if (n >= 16) {
                s += 16;
                len -= 16;
                n -= 16;
            } else {
                s += n;
                len -= n;
                n = 0;
            }
        } else {
            /* Found non-ASCII - process byte by byte through ASCII portion */
            while (n > 0 && len > 0 && *s < 0x80) {
                s++;
                len--;
                n--;
            }

            if (n == 0 || len == 0) {
                break;
            }

            /* Handle the multi-byte sequence */
            size_t seqLen = utf8AdvanceOne(s, len);
            if (seqLen == 0) {
                break;
            }
            s += seqLen;
            len -= seqLen;
            n--;
        }
    }

    /* Handle remaining bytes */
    while (n > 0 && len > 0) {
        size_t seqLen = utf8AdvanceOne(s, len);
        if (seqLen == 0) {
            break;
        }
        s += seqLen;
        len -= seqLen;
        n--;
    }

    return s;
}

#else
/* SWAR fallback */

#define UTF8_CURSOR_STEP size_t
#define UTF8_CURSOR_STEP_SIZE sizeof(UTF8_CURSOR_STEP)
#define UTF8_CURSOR_ONEMASK ((UTF8_CURSOR_STEP)(-1) / 0xFF)
#define UTF8_CURSOR_HIGHMASK (UTF8_CURSOR_ONEMASK * 0x80)
#define UTF8_CURSOR_ALL_ASCII(u) (((u) & UTF8_CURSOR_HIGHMASK) == 0)

const uint8_t *StrUtf8Advance(const void *str, size_t len, size_t n) {
    const uint8_t *s = (const uint8_t *)str;

    if (n == 0 || len == 0) {
        return s;
    }

    /* Align to step boundary */
    while (n > 0 && len > 0 && !DK_IS_STEP_ALIGNED(UTF8_CURSOR_STEP_SIZE, s)) {
        size_t seqLen = utf8AdvanceOne(s, len);
        if (seqLen == 0) {
            break;
        }
        s += seqLen;
        len -= seqLen;
        n--;
    }

    /* Process aligned blocks */
    while (n > 0 && len >= UTF8_CURSOR_STEP_SIZE) {
        UTF8_CURSOR_STEP u;
        memcpy(&u, s, UTF8_CURSOR_STEP_SIZE);

        if (UTF8_CURSOR_ALL_ASCII(u)) {
            /* All bytes are ASCII */
            if (n >= UTF8_CURSOR_STEP_SIZE) {
                s += UTF8_CURSOR_STEP_SIZE;
                len -= UTF8_CURSOR_STEP_SIZE;
                n -= UTF8_CURSOR_STEP_SIZE;
            } else {
                s += n;
                len -= n;
                n = 0;
            }
        } else {
            /* Found non-ASCII - process byte by byte */
            while (n > 0 && len > 0 && *s < 0x80) {
                s++;
                len--;
                n--;
            }

            if (n == 0 || len == 0) {
                break;
            }

            size_t seqLen = utf8AdvanceOne(s, len);
            if (seqLen == 0) {
                break;
            }
            s += seqLen;
            len -= seqLen;
            n--;
        }
    }

    /* Handle remaining bytes */
    while (n > 0 && len > 0) {
        size_t seqLen = utf8AdvanceOne(s, len);
        if (seqLen == 0) {
            break;
        }
        s += seqLen;
        len -= seqLen;
        n--;
    }

    return s;
}

#endif /* SIMD/NEON/fallback */

/* ====================================================================
 * Non-SIMD Cursor Functions (these don't benefit as much from SIMD)
 * ==================================================================== */

/* Move cursor backward by N codepoints */
const uint8_t *StrUtf8Retreat(const void *str, size_t len, const uint8_t *pos,
                              size_t n) {
    const uint8_t *start = (const uint8_t *)str;
    const uint8_t *end = start + len;

    (void)end; /* Unused, but kept for bounds checking if needed */

    if (pos == NULL || pos < start) {
        return start;
    }

    if (n == 0) {
        return pos;
    }

    const uint8_t *p = pos;

    while (n > 0 && p > start) {
        size_t back = utf8RetreatOne(start, p);
        if (back == 0) {
            break;
        }
        p -= back;
        n--;
    }

    return p;
}

/* Get codepoint at position without advancing */
uint32_t StrUtf8Peek(const void *str, size_t len, const uint8_t *pos) {
    const uint8_t *start = (const uint8_t *)str;
    const uint8_t *end = start + len;

    if (pos == NULL || pos < start || pos >= end) {
        return 0xFFFFFFFF;
    }

    size_t remaining = (size_t)(end - pos);
    const uint8_t *p = pos;

    return StrUtf8Decode(&p, &remaining);
}

/* Get byte offset for Nth codepoint (0-indexed) */
size_t StrUtf8OffsetAt(const void *str, size_t len, size_t charIndex) {
    const uint8_t *start = (const uint8_t *)str;
    const uint8_t *pos = StrUtf8Advance(str, len, charIndex);
    return (size_t)(pos - start);
}

/* Get codepoint index for byte offset */
size_t StrUtf8IndexAt(const void *str, size_t len, size_t byteOffset) {
    if (byteOffset == 0) {
        return 0;
    }

    if (byteOffset >= len) {
        return StrLenUtf8(str, len);
    }

    /* Count codepoints from start to byteOffset */
    return StrLenUtf8(str, byteOffset);
}

/* ====================================================================
 * Scalar Baseline for Benchmarking
 * ==================================================================== */

const uint8_t *StrUtf8AdvanceScalar(const void *str, size_t len, size_t n) {
    const uint8_t *s = (const uint8_t *)str;

    while (n > 0 && len > 0) {
        size_t seqLen = utf8AdvanceOne(s, len);
        if (seqLen == 0) {
            break;
        }
        s += seqLen;
        len -= seqLen;
        n--;
    }

    return s;
}
