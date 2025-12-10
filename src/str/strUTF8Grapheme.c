/* strUTF8Grapheme.c - UTF-8 Grapheme Cluster Operations
 *
 * Provides functions for working with grapheme clusters (user-perceived chars):
 *   - StrUtf8GraphemeCount: Count grapheme clusters in string
 *   - StrUtf8GraphemeAdvance: Advance by N grapheme clusters
 *   - StrUtf8GraphemeAt: Get byte range of Nth grapheme cluster
 *   - StrUtf8GraphemeWidth: Width in cells accounting for graphemes
 *
 * Uses Unicode Grapheme Break Properties for proper segmentation.
 */

#include "../str.h"
#include <string.h>

/* Helper: decode a codepoint and return byte length (or 0 on error) */
static inline size_t decodeCodepointG(const uint8_t *s, size_t len,
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
 * StrUtf8GraphemeNext - Find end of next grapheme cluster
 * ==================================================================== */

/* Find the byte offset of the end of the next grapheme cluster.
 * Returns 0 if string is empty.
 */
size_t StrUtf8GraphemeNext(const void *str, size_t len) {
    if (len == 0) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;

    /* Get first codepoint */
    uint32_t prevCp;
    size_t cpLen = decodeCodepointG(s, len, &prevCp);
    if (cpLen == 0) {
        return 1; /* Invalid byte, treat as single grapheme */
    }
    i += cpLen;

    /* Continue while there's no grapheme break */
    while (i < len) {
        uint32_t cp;
        cpLen = decodeCodepointG(s + i, len - i, &cp);
        if (cpLen == 0) {
            break; /* Invalid byte, stop here */
        }

        /* Check if there's a grapheme break between prevCp and cp */
        if (StrUnicodeIsGraphemeBreak(prevCp, cp)) {
            break;
        }

        prevCp = cp;
        i += cpLen;
    }

    return i;
}

/* ====================================================================
 * StrUtf8GraphemeCount - Count grapheme clusters
 * ==================================================================== */

/* Count the number of grapheme clusters in a UTF-8 string. */
size_t StrUtf8GraphemeCount(const void *str, size_t len) {
    if (len == 0) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;
    size_t count = 0;
    size_t i = 0;

    uint32_t prevCp = 0;
    bool havePrev = false;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepointG(s + i, len - i, &cp);

        if (cpLen == 0) {
            /* Invalid byte, treat as separate grapheme */
            count++;
            i++;
            havePrev = false;
            continue;
        }

        if (!havePrev) {
            /* First codepoint starts a new grapheme */
            count++;
            prevCp = cp;
            havePrev = true;
        } else if (StrUnicodeIsGraphemeBreak(prevCp, cp)) {
            /* Grapheme break, new cluster */
            count++;
            prevCp = cp;
        } else {
            /* Extend current cluster */
            prevCp = cp;
        }

        i += cpLen;
    }

    return count;
}

/* ====================================================================
 * StrUtf8GraphemeAdvance - Advance by N grapheme clusters
 * ==================================================================== */

/* Advance by n grapheme clusters. Returns byte offset after nth cluster. */
size_t StrUtf8GraphemeAdvance(const void *str, size_t len, size_t n) {
    if (len == 0 || n == 0) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;
    size_t pos = 0;

    for (size_t i = 0; i < n && pos < len; i++) {
        pos += StrUtf8GraphemeNext(s + pos, len - pos);
    }

    return pos;
}

/* ====================================================================
 * StrUtf8GraphemeAt - Get byte range of Nth grapheme cluster
 * ==================================================================== */

/* Get the byte range of the nth grapheme cluster (0-indexed).
 * Returns false if n is out of bounds.
 */
bool StrUtf8GraphemeAt(const void *str, size_t len, size_t n, size_t *startOut,
                       size_t *endOut) {
    if (len == 0) {
        return false;
    }

    const uint8_t *s = (const uint8_t *)str;

    /* Find start of nth grapheme (advance by n) */
    size_t start = StrUtf8GraphemeAdvance(str, len, n);
    if (start >= len) {
        return false;
    }

    /* Find end by getting next grapheme */
    size_t graphemeLen = StrUtf8GraphemeNext(s + start, len - start);

    *startOut = start;
    *endOut = start + graphemeLen;
    return true;
}

/* ====================================================================
 * StrUtf8GraphemeWidth - Width accounting for grapheme clusters
 * ==================================================================== */

/* Calculate display width counting grapheme clusters correctly.
 * Each grapheme cluster contributes the width of its base character.
 */
size_t StrUtf8GraphemeWidth(const void *str, size_t len) {
    if (len == 0) {
        return 0;
    }

    const uint8_t *s = (const uint8_t *)str;
    size_t width = 0;
    size_t i = 0;

    uint32_t prevCp = 0;
    bool havePrev = false;
    int currentGraphemeWidth = 0;

    while (i < len) {
        uint32_t cp;
        size_t cpLen = decodeCodepointG(s + i, len - i, &cp);

        if (cpLen == 0) {
            /* Invalid byte, treat as width 1 */
            if (havePrev) {
                width += currentGraphemeWidth;
            }
            width += 1;
            i++;
            havePrev = false;
            continue;
        }

        if (!havePrev) {
            /* First codepoint - its width determines grapheme width */
            currentGraphemeWidth = StrUnicodeEastAsianWidth(cp);
            prevCp = cp;
            havePrev = true;
        } else if (StrUnicodeIsGraphemeBreak(prevCp, cp)) {
            /* New grapheme - add previous width and start new */
            width += currentGraphemeWidth;
            currentGraphemeWidth = StrUnicodeEastAsianWidth(cp);
            prevCp = cp;
        } else {
            /* Extending current grapheme - don't add to width */
            prevCp = cp;
        }

        i += cpLen;
    }

    /* Add final grapheme's width */
    if (havePrev) {
        width += currentGraphemeWidth;
    }

    return width;
}

/* ====================================================================
 * StrUtf8GraphemeTruncate - Truncate to N grapheme clusters
 * ==================================================================== */

/* Truncate string to at most n grapheme clusters.
 * Returns byte length of truncated string.
 */
size_t StrUtf8GraphemeTruncate(const void *str, size_t len, size_t n) {
    return StrUtf8GraphemeAdvance(str, len, n);
}

/* ====================================================================
 * StrUtf8GraphemeReverse - Reverse by grapheme clusters
 * ==================================================================== */

/* Reverse a UTF-8 string by grapheme clusters in-place.
 * This preserves combining marks with their base characters.
 */
void StrUtf8GraphemeReverse(void *str, size_t len) {
    if (len <= 1) {
        return;
    }

    uint8_t *s = (uint8_t *)str;

    /* First, find all grapheme boundaries */
    size_t boundaries[256]; /* Support up to 256 graphemes */
    size_t boundaryCount = 0;

    size_t i = 0;
    uint32_t prevCp = 0;
    bool havePrev = false;

    boundaries[boundaryCount++] = 0;

    while (i < len && boundaryCount < 255) {
        uint32_t cp;
        size_t cpLen = decodeCodepointG(s + i, len - i, &cp);

        if (cpLen == 0) {
            if (havePrev) {
                boundaries[boundaryCount++] = i;
            }
            i++;
            boundaries[boundaryCount++] = i;
            havePrev = false;
            continue;
        }

        if (!havePrev) {
            prevCp = cp;
            havePrev = true;
        } else if (StrUnicodeIsGraphemeBreak(prevCp, cp)) {
            boundaries[boundaryCount++] = i;
            prevCp = cp;
        } else {
            prevCp = cp;
        }

        i += cpLen;
    }
    boundaries[boundaryCount] = len;

    /* Reverse the graphemes using a temporary buffer */
    uint8_t temp[1024];
    if (len > sizeof(temp)) {
        return; /* String too long for stack buffer */
    }

    size_t writePos = 0;
    for (size_t g = boundaryCount; g > 0; g--) {
        size_t start = boundaries[g - 1];
        size_t end = boundaries[g];
        memcpy(temp + writePos, s + start, end - start);
        writePos += end - start;
    }

    memcpy(s, temp, len);
}
