/* strUTF8Search.c - UTF-8 String Search Operations
 *
 * Provides search operations for UTF-8 strings:
 *   - StrUtf8Find: Find first occurrence of substring
 *   - StrUtf8FindLast: Find last occurrence of substring
 *   - StrUtf8FindChar: Find first occurrence of codepoint
 *   - StrUtf8FindCharLast: Find last occurrence of codepoint
 *   - StrUtf8Contains: Check if substring exists
 *   - StrUtf8Count: Count occurrences of substring
 *   - StrUtf8CountChar: Count occurrences of codepoint
 *
 * Search operations work at byte level for valid UTF-8, which is correct
 * because UTF-8 is self-synchronizing (no valid sequence is a substring
 * of another valid sequence that starts at a different byte).
 */

#include "../str.h"
#include <string.h>

/* ====================================================================
 * StrUtf8Find - Find first occurrence of substring
 * ==================================================================== */

/* Find first occurrence of needle in haystack.
 * Returns byte offset of match, or SIZE_MAX if not found.
 *
 * This works correctly for UTF-8 because:
 * 1. UTF-8 is self-synchronizing
 * 2. A valid UTF-8 sequence cannot appear as a substring starting
 *    at a continuation byte of another sequence
 */
size_t StrUtf8Find(const void *haystack, size_t haystackLen, const void *needle,
                   size_t needleLen) {
    if (needleLen == 0) {
        return 0; /* Empty needle always found at start */
    }
    if (needleLen > haystackLen) {
        return SIZE_MAX;
    }

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    size_t limit = haystackLen - needleLen + 1;

    /* Simple search - could be optimized with Boyer-Moore for large needles */
    for (size_t i = 0; i < limit; i++) {
        if (memcmp(h + i, n, needleLen) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* ====================================================================
 * StrUtf8FindLast - Find last occurrence of substring
 * ==================================================================== */

/* Find last occurrence of needle in haystack.
 * Returns byte offset of match, or SIZE_MAX if not found.
 */
size_t StrUtf8FindLast(const void *haystack, size_t haystackLen,
                       const void *needle, size_t needleLen) {
    if (needleLen == 0) {
        return haystackLen; /* Empty needle at end */
    }
    if (needleLen > haystackLen) {
        return SIZE_MAX;
    }

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;

    /* Search from end */
    for (size_t i = haystackLen - needleLen + 1; i > 0; i--) {
        if (memcmp(h + i - 1, n, needleLen) == 0) {
            return i - 1;
        }
    }
    return SIZE_MAX;
}

/* ====================================================================
 * StrUtf8FindChar - Find first occurrence of codepoint
 * ==================================================================== */

/* Find first occurrence of a Unicode codepoint.
 * Returns byte offset of match, or SIZE_MAX if not found.
 */
size_t StrUtf8FindChar(const void *str, size_t len, uint32_t codepoint) {
    /* Encode the codepoint to UTF-8 */
    uint8_t encoded[4];
    size_t encodedLen = StrUtf8Encode(encoded, codepoint);
    if (encodedLen == 0) {
        return SIZE_MAX; /* Invalid codepoint */
    }

    return StrUtf8Find(str, len, encoded, encodedLen);
}

/* ====================================================================
 * StrUtf8FindCharLast - Find last occurrence of codepoint
 * ==================================================================== */

/* Find last occurrence of a Unicode codepoint.
 * Returns byte offset of match, or SIZE_MAX if not found.
 */
size_t StrUtf8FindCharLast(const void *str, size_t len, uint32_t codepoint) {
    /* Encode the codepoint to UTF-8 */
    uint8_t encoded[4];
    size_t encodedLen = StrUtf8Encode(encoded, codepoint);
    if (encodedLen == 0) {
        return SIZE_MAX; /* Invalid codepoint */
    }

    return StrUtf8FindLast(str, len, encoded, encodedLen);
}

/* ====================================================================
 * StrUtf8FindCharIndex - Find Nth occurrence of codepoint (by char index)
 * ==================================================================== */

/* Find Nth occurrence of codepoint (0-indexed).
 * Returns byte offset of match, or SIZE_MAX if not found.
 */
size_t StrUtf8FindCharNth(const void *str, size_t len, uint32_t codepoint,
                          size_t n) {
    /* Encode the codepoint to UTF-8 */
    uint8_t encoded[4];
    size_t encodedLen = StrUtf8Encode(encoded, codepoint);
    if (encodedLen == 0) {
        return SIZE_MAX;
    }

    const uint8_t *s = (const uint8_t *)str;
    size_t count = 0;

    for (size_t i = 0; i + encodedLen <= len;) {
        if (memcmp(s + i, encoded, encodedLen) == 0) {
            if (count == n) {
                return i;
            }
            count++;
            i += encodedLen;
        } else {
            /* Skip to next codepoint */
            size_t seqLen = StrUtf8SequenceLen(s[i]);
            i += (seqLen > 0) ? seqLen : 1;
        }
    }
    return SIZE_MAX;
}

/* ====================================================================
 * StrUtf8Contains - Check if substring exists
 * ==================================================================== */

/* Check if haystack contains needle.
 */
bool StrUtf8Contains(const void *haystack, size_t haystackLen,
                     const void *needle, size_t needleLen) {
    return StrUtf8Find(haystack, haystackLen, needle, needleLen) != SIZE_MAX;
}

/* ====================================================================
 * StrUtf8Count - Count occurrences of substring
 * ==================================================================== */

/* Count non-overlapping occurrences of needle in haystack.
 */
size_t StrUtf8Count(const void *haystack, size_t haystackLen,
                    const void *needle, size_t needleLen) {
    if (needleLen == 0) {
        /* Empty needle: return number of positions (chars + 1) */
        return StrLenUtf8(haystack, haystackLen) + 1;
    }
    if (needleLen > haystackLen) {
        return 0;
    }

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    size_t count = 0;
    size_t i = 0;

    while (i + needleLen <= haystackLen) {
        if (memcmp(h + i, n, needleLen) == 0) {
            count++;
            i += needleLen; /* Non-overlapping */
        } else {
            i++;
        }
    }
    return count;
}

/* ====================================================================
 * StrUtf8CountChar - Count occurrences of codepoint
 * ==================================================================== */

/* Count occurrences of a Unicode codepoint.
 */
size_t StrUtf8CountChar(const void *str, size_t len, uint32_t codepoint) {
    /* Encode the codepoint to UTF-8 */
    uint8_t encoded[4];
    size_t encodedLen = StrUtf8Encode(encoded, codepoint);
    if (encodedLen == 0) {
        return 0; /* Invalid codepoint */
    }

    return StrUtf8Count(str, len, encoded, encodedLen);
}

/* ====================================================================
 * StrUtf8FindAnyChar - Find first occurrence of any codepoint in set
 * ==================================================================== */

/* Find first occurrence of any codepoint from a set.
 * The set is a UTF-8 string containing the codepoints to search for.
 * Returns byte offset of match, or SIZE_MAX if not found.
 */
size_t StrUtf8FindAnyChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen) {
    if (len == 0 || charSetLen == 0) {
        return SIZE_MAX;
    }

    const uint8_t *s = (const uint8_t *)str;
    const uint8_t *end = s + len;

    while (s < end) {
        size_t seqLen = StrUtf8SequenceLen(*s);
        if (seqLen == 0 || s + seqLen > end) {
            seqLen = 1; /* Invalid, skip byte */
        }

        /* Check if this codepoint is in the set */
        if (StrUtf8Find(charSet, charSetLen, s, seqLen) != SIZE_MAX) {
            return (size_t)(s - (const uint8_t *)str);
        }
        s += seqLen;
    }
    return SIZE_MAX;
}

/* ====================================================================
 * StrUtf8FindNotChar - Find first codepoint NOT in set
 * ==================================================================== */

/* Find first occurrence of any codepoint NOT in the set.
 * Returns byte offset of match, or SIZE_MAX if all chars are in set.
 */
size_t StrUtf8FindNotChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen) {
    if (len == 0) {
        return SIZE_MAX;
    }
    if (charSetLen == 0) {
        return 0; /* First char is not in empty set */
    }

    const uint8_t *s = (const uint8_t *)str;
    const uint8_t *end = s + len;

    while (s < end) {
        size_t seqLen = StrUtf8SequenceLen(*s);
        if (seqLen == 0 || s + seqLen > end) {
            seqLen = 1;
        }

        /* Check if this codepoint is NOT in the set */
        if (StrUtf8Find(charSet, charSetLen, s, seqLen) == SIZE_MAX) {
            return (size_t)(s - (const uint8_t *)str);
        }
        s += seqLen;
    }
    return SIZE_MAX;
}

/* ====================================================================
 * StrUtf8SpanChar - Length of initial segment matching set
 * ==================================================================== */

/* Return byte length of initial segment containing only chars from set.
 * Similar to strspn() but for UTF-8.
 */
size_t StrUtf8SpanChar(const void *str, size_t len, const void *charSet,
                       size_t charSetLen) {
    size_t notFound = StrUtf8FindNotChar(str, len, charSet, charSetLen);
    return (notFound == SIZE_MAX) ? len : notFound;
}

/* ====================================================================
 * StrUtf8SpanNotChar - Length of initial segment NOT matching set
 * ==================================================================== */

/* Return byte length of initial segment containing no chars from set.
 * Similar to strcspn() but for UTF-8.
 */
size_t StrUtf8SpanNotChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen) {
    size_t found = StrUtf8FindAnyChar(str, len, charSet, charSetLen);
    return (found == SIZE_MAX) ? len : found;
}
