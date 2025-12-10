/* strUTF8Compare.c - UTF-8 String Comparison Operations
 *
 * Provides comparison operations for UTF-8 strings:
 *   - StrUtf8Compare: Byte-level comparison (same as memcmp for valid UTF-8)
 *   - StrUtf8CompareN: Compare first N codepoints
 *   - StrUtf8CompareCaseInsensitiveAscii: ASCII case-insensitive comparison
 *
 * For valid UTF-8, byte-level comparison produces correct lexicographic order
 * because UTF-8 is designed such that bytewise comparison matches codepoint
 * order.
 */

#include "../str.h"
#include <string.h>

/* ====================================================================
 * StrUtf8Compare - Byte-level comparison
 * ==================================================================== */

/* Compare two UTF-8 strings byte-by-byte.
 * For valid UTF-8, this produces correct Unicode lexicographic ordering.
 *
 * Returns:
 *   < 0 if s1 < s2
 *   = 0 if s1 == s2
 *   > 0 if s1 > s2
 */
int StrUtf8Compare(const void *s1, size_t len1, const void *s2, size_t len2) {
    size_t minLen = (len1 < len2) ? len1 : len2;

    if (minLen > 0) {
        int cmp = memcmp(s1, s2, minLen);
        if (cmp != 0) {
            return cmp;
        }
    }

    /* Strings are equal up to minLen, compare by length */
    if (len1 < len2) {
        return -1;
    } else if (len1 > len2) {
        return 1;
    }
    return 0;
}

/* ====================================================================
 * StrUtf8CompareN - Compare first N codepoints
 * ==================================================================== */

/* Compare up to N codepoints of two UTF-8 strings.
 * This is useful for prefix matching by character count.
 *
 * Returns:
 *   < 0 if s1 < s2 (in first N codepoints)
 *   = 0 if first N codepoints are equal
 *   > 0 if s1 > s2 (in first N codepoints)
 */
int StrUtf8CompareN(const void *s1, size_t len1, const void *s2, size_t len2,
                    size_t n) {
    if (n == 0) {
        return 0;
    }

    /* Get byte lengths for first n codepoints in each string */
    size_t byteLen1 = StrUtf8Truncate(s1, len1, n);
    size_t byteLen2 = StrUtf8Truncate(s2, len2, n);

    /* Compare the truncated portions */
    return StrUtf8Compare(s1, byteLen1, s2, byteLen2);
}

/* ====================================================================
 * StrUtf8CompareCaseInsensitiveAscii - ASCII case-insensitive
 * ==================================================================== */

/* Lookup table for ASCII lowercase conversion */
static const uint8_t asciiLowerTable[256] = {
    /* 0x00-0x1F: Control characters (unchanged) */
    0x00,
    0x01,
    0x02,
    0x03,
    0x04,
    0x05,
    0x06,
    0x07,
    0x08,
    0x09,
    0x0A,
    0x0B,
    0x0C,
    0x0D,
    0x0E,
    0x0F,
    0x10,
    0x11,
    0x12,
    0x13,
    0x14,
    0x15,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1A,
    0x1B,
    0x1C,
    0x1D,
    0x1E,
    0x1F,
    /* 0x20-0x3F: Punctuation and digits (unchanged) */
    0x20,
    0x21,
    0x22,
    0x23,
    0x24,
    0x25,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2A,
    0x2B,
    0x2C,
    0x2D,
    0x2E,
    0x2F,
    0x30,
    0x31,
    0x32,
    0x33,
    0x34,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3A,
    0x3B,
    0x3C,
    0x3D,
    0x3E,
    0x3F,
    /* 0x40-0x5F: @ and uppercase letters -> lowercase */
    0x40,
    0x61,
    0x62,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67, /* @ABCDEFG -> @abcdefg */
    0x68,
    0x69,
    0x6A,
    0x6B,
    0x6C,
    0x6D,
    0x6E,
    0x6F, /* HIJKLMNO -> hijklmno */
    0x70,
    0x71,
    0x72,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77, /* PQRSTUVW -> pqrstuvw */
    0x78,
    0x79,
    0x7A,
    0x5B,
    0x5C,
    0x5D,
    0x5E,
    0x5F, /* XYZ[\]^_ -> xyz[\]^_ */
    /* 0x60-0x7F: Lowercase letters and control (unchanged) */
    0x60,
    0x61,
    0x62,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6A,
    0x6B,
    0x6C,
    0x6D,
    0x6E,
    0x6F,
    0x70,
    0x71,
    0x72,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7A,
    0x7B,
    0x7C,
    0x7D,
    0x7E,
    0x7F,
    /* 0x80-0xFF: High bytes (unchanged - non-ASCII) */
    0x80,
    0x81,
    0x82,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8A,
    0x8B,
    0x8C,
    0x8D,
    0x8E,
    0x8F,
    0x90,
    0x91,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9A,
    0x9B,
    0x9C,
    0x9D,
    0x9E,
    0x9F,
    0xA0,
    0xA1,
    0xA2,
    0xA3,
    0xA4,
    0xA5,
    0xA6,
    0xA7,
    0xA8,
    0xA9,
    0xAA,
    0xAB,
    0xAC,
    0xAD,
    0xAE,
    0xAF,
    0xB0,
    0xB1,
    0xB2,
    0xB3,
    0xB4,
    0xB5,
    0xB6,
    0xB7,
    0xB8,
    0xB9,
    0xBA,
    0xBB,
    0xBC,
    0xBD,
    0xBE,
    0xBF,
    0xC0,
    0xC1,
    0xC2,
    0xC3,
    0xC4,
    0xC5,
    0xC6,
    0xC7,
    0xC8,
    0xC9,
    0xCA,
    0xCB,
    0xCC,
    0xCD,
    0xCE,
    0xCF,
    0xD0,
    0xD1,
    0xD2,
    0xD3,
    0xD4,
    0xD5,
    0xD6,
    0xD7,
    0xD8,
    0xD9,
    0xDA,
    0xDB,
    0xDC,
    0xDD,
    0xDE,
    0xDF,
    0xE0,
    0xE1,
    0xE2,
    0xE3,
    0xE4,
    0xE5,
    0xE6,
    0xE7,
    0xE8,
    0xE9,
    0xEA,
    0xEB,
    0xEC,
    0xED,
    0xEE,
    0xEF,
    0xF0,
    0xF1,
    0xF2,
    0xF3,
    0xF4,
    0xF5,
    0xF6,
    0xF7,
    0xF8,
    0xF9,
    0xFA,
    0xFB,
    0xFC,
    0xFD,
    0xFE,
    0xFF,
};

/* Compare two UTF-8 strings with ASCII case-insensitive matching.
 *
 * Only ASCII letters (A-Z) are treated as equal to their lowercase
 * counterparts. Non-ASCII characters (including accented Latin letters)
 * are compared byte-by-byte without case folding.
 *
 * This is suitable for protocols and identifiers that need ASCII
 * case-insensitivity while preserving exact matching for Unicode.
 *
 * Returns:
 *   < 0 if s1 < s2 (case-insensitive for ASCII)
 *   = 0 if s1 == s2 (case-insensitive for ASCII)
 *   > 0 if s1 > s2 (case-insensitive for ASCII)
 */
int StrUtf8CompareCaseInsensitiveAscii(const void *s1, size_t len1,
                                       const void *s2, size_t len2) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    size_t minLen = (len1 < len2) ? len1 : len2;

    for (size_t i = 0; i < minLen; i++) {
        uint8_t c1 = asciiLowerTable[p1[i]];
        uint8_t c2 = asciiLowerTable[p2[i]];
        if (c1 != c2) {
            return (int)c1 - (int)c2;
        }
    }

    /* Equal up to minLen, compare by length */
    if (len1 < len2) {
        return -1;
    } else if (len1 > len2) {
        return 1;
    }
    return 0;
}

/* ====================================================================
 * StrUtf8StartsWith / StrUtf8EndsWith - Prefix/suffix matching
 * ==================================================================== */

/* Check if string starts with prefix (by codepoints).
 *
 * Returns true if the first N codepoints of str match prefix,
 * where N is the number of codepoints in prefix.
 */
bool StrUtf8StartsWith(const void *str, size_t strLen, const void *prefix,
                       size_t prefixLen) {
    /* Count codepoints in prefix */
    size_t prefixChars = StrLenUtf8(prefix, prefixLen);

    /* Get byte length for same number of codepoints in str */
    size_t strByteLen = StrUtf8Truncate(str, strLen, prefixChars);

    /* If str has fewer bytes than prefix for same char count, no match */
    if (strByteLen < prefixLen) {
        return false;
    }

    /* Compare the bytes */
    return memcmp(str, prefix, prefixLen) == 0;
}

/* Check if string ends with suffix (by codepoints).
 *
 * Returns true if the last N codepoints of str match suffix,
 * where N is the number of codepoints in suffix.
 */
bool StrUtf8EndsWith(const void *str, size_t strLen, const void *suffix,
                     size_t suffixLen) {
    /* Quick check: suffix can't be longer than string */
    if (suffixLen > strLen) {
        return false;
    }

    /* Count codepoints in both */
    size_t strChars = StrLenUtf8(str, strLen);
    size_t suffixChars = StrLenUtf8(suffix, suffixLen);

    if (suffixChars > strChars) {
        return false;
    }

    /* Find where suffix would start (by codepoints) */
    size_t startChar = strChars - suffixChars;
    size_t startOffset = StrUtf8OffsetAt(str, strLen, startChar);

    /* Compare remaining bytes */
    size_t remainingLen = strLen - startOffset;
    if (remainingLen != suffixLen) {
        return false;
    }

    return memcmp((const uint8_t *)str + startOffset, suffix, suffixLen) == 0;
}

/* ====================================================================
 * StrUtf8Equal - Equality check (convenience wrapper)
 * ==================================================================== */

/* Check if two UTF-8 strings are exactly equal.
 * This is a convenience wrapper around StrUtf8Compare.
 */
bool StrUtf8Equal(const void *s1, size_t len1, const void *s2, size_t len2) {
    if (len1 != len2) {
        return false;
    }
    if (len1 == 0) {
        return true;
    }
    return memcmp(s1, s2, len1) == 0;
}

/* Check if two UTF-8 strings are equal (ASCII case-insensitive).
 */
bool StrUtf8EqualCaseInsensitiveAscii(const void *s1, size_t len1,
                                      const void *s2, size_t len2) {
    return StrUtf8CompareCaseInsensitiveAscii(s1, len1, s2, len2) == 0;
}
