/* strUTF8Width.c - UTF-8 Display Width Calculation
 *
 * Provides functions for calculating display width of UTF-8 strings:
 *   - StrUtf8Width: Total display width in terminal cells
 *   - StrUtf8WidthGrapheme: Width counting grapheme clusters
 *   - StrUtf8TruncateWidth: Truncate to maximum display width
 *   - StrUtf8PadWidth: Pad string to exact display width
 *
 * Uses East Asian Width property for proper CJK handling.
 */

#include "../str.h"
#include <string.h>

/* Helper: decode a codepoint and return byte length (or 0 on error) */
static inline size_t decodeCodepoint(const uint8_t *s, size_t len,
                                     uint32_t *cpOut) {
    const uint8_t *pos = s;
    size_t remaining = len;
    uint32_t cp = StrUtf8Decode(&pos, &remaining);

    if (cp == 0xFFFFFFFF) {
        *cpOut = 0;
        return 0;
    }

    *cpOut = cp;
    return (size_t)(pos - s);
}

/* ====================================================================
 * StrUtf8Width - Calculate display width of UTF-8 string
 * ==================================================================== */

/* Calculate display width in terminal cells.
 * ASCII: 1 cell, CJK: 2 cells, combining marks: 0 cells.
 */
size_t StrUtf8Width(const void *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;
    size_t width = 0;
    size_t i = 0;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            /* Invalid UTF-8, treat as 1-byte character */
            width += 1;
            i++;
            continue;
        }

        width += StrUnicodeEastAsianWidth(cp);
        i += cpLen;
    }

    return width;
}

/* ====================================================================
 * StrUtf8WidthN - Calculate width of first N codepoints
 * ==================================================================== */

/* Calculate display width of first n codepoints. */
size_t StrUtf8WidthN(const void *str, size_t len, size_t n) {
    const uint8_t *s = (const uint8_t *)str;
    size_t width = 0;
    size_t i = 0;
    size_t count = 0;

    while (i < len && count < n) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            width += 1;
            i++;
            count++;
            continue;
        }

        width += StrUnicodeEastAsianWidth(cp);
        i += cpLen;
        count++;
    }

    return width;
}

/* ====================================================================
 * StrUtf8TruncateWidth - Truncate to maximum display width
 * ==================================================================== */

/* Truncate string to fit within maxWidth display cells.
 * Returns byte length of truncated string.
 * Does not split multi-byte characters.
 */
size_t StrUtf8TruncateWidth(const void *str, size_t len, size_t maxWidth) {
    const uint8_t *s = (const uint8_t *)str;
    size_t width = 0;
    size_t i = 0;
    size_t lastGoodPos = 0;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            /* Invalid UTF-8 */
            if (width + 1 > maxWidth) {
                return lastGoodPos;
            }
            width += 1;
            i++;
            lastGoodPos = i;
            continue;
        }

        int cpWidth = StrUnicodeEastAsianWidth(cp);

        /* Check if adding this codepoint would exceed maxWidth */
        if (width + cpWidth > maxWidth) {
            return lastGoodPos;
        }

        width += cpWidth;
        i += cpLen;
        lastGoodPos = i;
    }

    return len;
}

/* ====================================================================
 * StrUtf8IndexAtWidth - Find byte index at display width
 * ==================================================================== */

/* Find byte index where display width reaches targetWidth.
 * Returns byte index, or len if not reached.
 */
size_t StrUtf8IndexAtWidth(const void *str, size_t len, size_t targetWidth) {
    const uint8_t *s = (const uint8_t *)str;
    size_t width = 0;
    size_t i = 0;

    while (i < len && width < targetWidth) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            width += 1;
            i++;
            continue;
        }

        int cpWidth = StrUnicodeEastAsianWidth(cp);
        if (width + cpWidth > targetWidth) {
            /* Would exceed target, stop here */
            break;
        }

        width += cpWidth;
        i += cpLen;
    }

    return i;
}

/* ====================================================================
 * StrUtf8WidthAt - Get width at byte offset
 * ==================================================================== */

/* Calculate display width from start to byte offset.
 * If offset is in the middle of a character, returns width up to
 * the start of that character.
 */
size_t StrUtf8WidthAt(const void *str, size_t len, size_t offset) {
    if (offset > len) {
        offset = len;
    }

    return StrUtf8Width(str, offset);
}

/* ====================================================================
 * StrUtf8PadWidth - Calculate padding needed for target width
 * ==================================================================== */

/* Calculate number of spaces needed to pad string to target width.
 * Returns 0 if string is already wider than or equal to target.
 */
size_t StrUtf8PadWidth(const void *str, size_t len, size_t targetWidth) {
    size_t currentWidth = StrUtf8Width(str, len);

    if (currentWidth >= targetWidth) {
        return 0;
    }

    return targetWidth - currentWidth;
}

/* ====================================================================
 * StrUtf8WidthBetween - Width of substring
 * ==================================================================== */

/* Calculate display width between two byte offsets.
 * Both offsets should be at valid codepoint boundaries.
 */
size_t StrUtf8WidthBetween(const void *str, size_t len, size_t startOffset,
                           size_t endOffset) {
    if (startOffset >= len) {
        return 0;
    }
    if (endOffset > len) {
        endOffset = len;
    }
    if (startOffset >= endOffset) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;
    return StrUtf8Width(s + startOffset, endOffset - startOffset);
}

/* ====================================================================
 * StrUtf8IsNarrow - Check if string contains only narrow characters
 * ==================================================================== */

/* Check if all characters in the string are narrow (width 1).
 * Returns true for ASCII-only strings.
 */
bool StrUtf8IsNarrow(const void *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            i++;
            continue;
        }

        if (StrUnicodeEastAsianWidth(cp) != 1) {
            return false;
        }

        i += cpLen;
    }

    return true;
}

/* ====================================================================
 * StrUtf8HasWide - Check if string contains wide characters
 * ==================================================================== */

/* Check if string contains any wide (width 2) characters. */
bool StrUtf8HasWide(const void *str, size_t len) {
    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepoint(s + i, len - i, &cp);

        if (cpLen == 0) {
            i++;
            continue;
        }

        if (StrUnicodeEastAsianWidth(cp) == 2) {
            return true;
        }

        i += cpLen;
    }

    return false;
}
