/* strUTF8Substr.c - UTF-8 Character-Safe Truncation and Substring Operations
 *
 * Provides character-safe string operations:
 *   - StrUtf8Truncate: Truncate to N codepoints (returns byte length)
 *   - StrUtf8Substring: Extract substring by codepoint indices
 *   - StrUtf8TruncateBytes: Truncate to max bytes, ensuring valid UTF-8
 * boundary
 *
 * Uses SIMD-optimized cursor operations for efficient navigation.
 */

#include "../str.h"
#include <string.h>

/* ====================================================================
 * StrUtf8Truncate - Get byte length for first N codepoints
 * ==================================================================== */

/* Returns the byte length needed to store the first 'maxChars' codepoints.
 * This is essentially just a wrapper around StrUtf8Advance that returns
 * the byte offset instead of a pointer.
 *
 * If the string has fewer than maxChars codepoints, returns the full length.
 */
size_t StrUtf8Truncate(const void *str, size_t len, size_t maxChars) {
    if (maxChars == 0) {
        return 0;
    }

    const uint8_t *start = (const uint8_t *)str;
    const uint8_t *end = StrUtf8Advance(str, len, maxChars);

    return (size_t)(end - start);
}

/* ====================================================================
 * StrUtf8TruncateBytes - Truncate to max bytes at valid boundary
 * ==================================================================== */

/* Returns the largest byte length <= maxBytes that ends at a valid
 * UTF-8 codepoint boundary. This ensures we don't cut in the middle
 * of a multi-byte sequence.
 *
 * If maxBytes >= len, returns len.
 */
size_t StrUtf8TruncateBytes(const void *str, size_t len, size_t maxBytes) {
    if (maxBytes >= len) {
        return len;
    }

    if (maxBytes == 0) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;

    /* Check if we're already at a valid boundary */
    /* A valid boundary is at the start of a codepoint (not a continuation byte)
     */
    if ((s[maxBytes] & 0xC0) != 0x80) {
        /* Next byte is a start byte or ASCII, so maxBytes is valid */
        return maxBytes;
    }

    /* We're in the middle of a multi-byte sequence.
     * Back up to find the start of this sequence. */
    size_t pos = maxBytes;
    while (pos > 0 && (s[pos] & 0xC0) == 0x80) {
        pos--;
    }

    /* pos now points to the start byte of the sequence we were in the middle
     * of. We truncate just before this sequence. */
    return pos;
}

/* ====================================================================
 * StrUtf8Substring - Extract substring by codepoint indices
 * ==================================================================== */

/* Extracts a substring from codepoint index 'start' to 'end' (exclusive).
 * Returns the byte offset and length of the substring.
 *
 * If start >= string length (in codepoints), returns offset at end of string
 * with length 0.
 *
 * If end > string length or end == SIZE_MAX, extracts to end of string.
 *
 * Parameters:
 *   str       - Input string
 *   len       - Byte length of input string
 *   startChar - Starting codepoint index (0-based, inclusive)
 *   endChar   - Ending codepoint index (exclusive), or SIZE_MAX for end of
 * string outOffset - Output: byte offset where substring starts outLen    -
 * Output: byte length of substring
 */
void StrUtf8Substring(const void *str, size_t len, size_t startChar,
                      size_t endChar, size_t *outOffset, size_t *outLen) {
    const uint8_t *base = (const uint8_t *)str;

    /* Find start position */
    const uint8_t *startPos = StrUtf8Advance(str, len, startChar);
    size_t startOffset = (size_t)(startPos - base);

    /* Handle edge case: start beyond string */
    if (startOffset >= len) {
        *outOffset = len;
        *outLen = 0;
        return;
    }

    /* Handle end == SIZE_MAX (meaning "to end of string") */
    if (endChar == SIZE_MAX) {
        *outOffset = startOffset;
        *outLen = len - startOffset;
        return;
    }

    /* Handle invalid range */
    if (endChar <= startChar) {
        *outOffset = startOffset;
        *outLen = 0;
        return;
    }

    /* Find end position - advance from start position */
    size_t remainingLen = len - startOffset;
    size_t charsToAdvance = endChar - startChar;
    const uint8_t *endPos =
        StrUtf8Advance(startPos, remainingLen, charsToAdvance);
    size_t endOffset = (size_t)(endPos - base);

    *outOffset = startOffset;
    *outLen = endOffset - startOffset;
}

/* ====================================================================
 * StrUtf8SubstringCopy - Extract substring and copy to buffer
 * ==================================================================== */

/* Extracts a substring and copies it to the provided buffer.
 * Returns the number of bytes written (not including null terminator).
 *
 * If bufLen is 0, returns the number of bytes needed (not including null).
 * If bufLen is non-zero, writes at most bufLen-1 bytes plus null terminator.
 *
 * The extraction is always at valid UTF-8 boundaries.
 */
size_t StrUtf8SubstringCopy(const void *str, size_t len, size_t startChar,
                            size_t endChar, void *buf, size_t bufLen) {
    size_t offset, subLen;
    StrUtf8Substring(str, len, startChar, endChar, &offset, &subLen);

    /* Query mode: just return needed size */
    if (bufLen == 0) {
        return subLen;
    }

    /* Determine how many bytes we can actually copy */
    size_t copyLen = subLen;
    if (copyLen >= bufLen) {
        /* Need to truncate - ensure we truncate at valid boundary */
        copyLen = StrUtf8TruncateBytes((const uint8_t *)str + offset, subLen,
                                       bufLen - 1);
    }

    /* Copy and null-terminate */
    if (copyLen > 0) {
        memcpy(buf, (const uint8_t *)str + offset, copyLen);
    }
    ((uint8_t *)buf)[copyLen] = '\0';

    return copyLen;
}

/* ====================================================================
 * StrUtf8Split - Find split point at Nth codepoint
 * ==================================================================== */

/* Finds the byte offset that splits the string at the Nth codepoint.
 * Useful for splitting strings at character boundaries.
 *
 * This is equivalent to StrUtf8Truncate but with a clearer name
 * for splitting operations.
 *
 * Returns: byte offset of the split point
 */
size_t StrUtf8Split(const void *str, size_t len, size_t charIndex) {
    return StrUtf8Truncate(str, len, charIndex);
}
