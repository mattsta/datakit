/* strUnicodeData.c - Unicode Character Property Tables
 *
 * Provides lookup tables for Unicode character properties:
 *   - East Asian Width (for display width calculation)
 *   - General Category (basic character classification)
 *   - Grapheme Break Property (for grapheme cluster segmentation)
 *
 * Uses compact range-based representation with binary search.
 * Based on Unicode 15.0 data.
 */

#include "../str.h"
#include <stddef.h>

/* ====================================================================
 * East Asian Width Property
 * ==================================================================== */

/* East Asian Width categories */
typedef enum {
    EAW_N = 0,  /* Neutral (not East Asian) */
    EAW_Na = 1, /* Narrow */
    EAW_H = 2,  /* Halfwidth */
    EAW_W = 3,  /* Wide */
    EAW_F = 4,  /* Fullwidth */
    EAW_A = 5,  /* Ambiguous */
} EastAsianWidth;

/* Range entry for East Asian Width */
typedef struct {
    uint32_t start;
    uint32_t end; /* Inclusive */
    uint8_t width;
} EawRange;

/* Wide and Fullwidth ranges (W and F categories)
 * These are the ranges where characters take 2 cells.
 * Most CJK characters fall into these ranges.
 */
static const EawRange eawWideRanges[] = {
    /* CJK Radicals Supplement..Kangxi Radicals */
    {0x2E80, 0x2EFF, EAW_W},
    /* Ideographic Description Characters */
    {0x2FF0, 0x2FFF, EAW_W},
    /* CJK Symbols and Punctuation */
    {0x3000, 0x303F, EAW_W},
    /* Hiragana */
    {0x3040, 0x309F, EAW_W},
    /* Katakana */
    {0x30A0, 0x30FF, EAW_W},
    /* Bopomofo */
    {0x3100, 0x312F, EAW_W},
    /* Hangul Compatibility Jamo */
    {0x3130, 0x318F, EAW_W},
    /* Kanbun..Bopomofo Extended */
    {0x3190, 0x31BF, EAW_W},
    /* CJK Strokes */
    {0x31C0, 0x31EF, EAW_W},
    /* Katakana Phonetic Extensions */
    {0x31F0, 0x31FF, EAW_W},
    /* Enclosed CJK Letters and Months */
    {0x3200, 0x32FF, EAW_W},
    /* CJK Compatibility */
    {0x3300, 0x33FF, EAW_W},
    /* CJK Unified Ideographs Extension A */
    {0x3400, 0x4DBF, EAW_W},
    /* CJK Unified Ideographs */
    {0x4E00, 0x9FFF, EAW_W},
    /* Yi Syllables */
    {0xA000, 0xA48F, EAW_W},
    /* Yi Radicals */
    {0xA490, 0xA4CF, EAW_W},
    /* Hangul Syllables */
    {0xAC00, 0xD7AF, EAW_W},
    /* CJK Compatibility Ideographs */
    {0xF900, 0xFAFF, EAW_W},
    /* Vertical Forms */
    {0xFE10, 0xFE1F, EAW_W},
    /* CJK Compatibility Forms */
    {0xFE30, 0xFE4F, EAW_W},
    /* Small Form Variants */
    {0xFE50, 0xFE6F, EAW_W},
    /* Fullwidth ASCII */
    {0xFF01, 0xFF60, EAW_F},
    /* Fullwidth punctuation */
    {0xFFE0, 0xFFE6, EAW_F},
    /* CJK Unified Ideographs Extension B..F */
    {0x20000, 0x2FFFF, EAW_W},
    /* CJK Compatibility Ideographs Supplement */
    {0x30000, 0x3FFFF, EAW_W},
};

#define EAW_WIDE_COUNT (sizeof(eawWideRanges) / sizeof(eawWideRanges[0]))

/* Zero-width character ranges */
static const EawRange zeroWidthRanges[] = {
    /* Combining Diacritical Marks */
    {0x0300, 0x036F, 0},
    /* Combining Diacritical Marks Extended */
    {0x1AB0, 0x1AFF, 0},
    /* Combining Diacritical Marks Supplement */
    {0x1DC0, 0x1DFF, 0},
    /* Combining Diacritical Marks for Symbols */
    {0x20D0, 0x20FF, 0},
    /* Combining Half Marks */
    {0xFE20, 0xFE2F, 0},
    /* Zero Width Space */
    {0x200B, 0x200B, 0},
    /* Zero Width Non-Joiner */
    {0x200C, 0x200C, 0},
    /* Zero Width Joiner */
    {0x200D, 0x200D, 0},
    /* Word Joiner */
    {0x2060, 0x2060, 0},
    /* Function Application..Invisible Plus */
    {0x2061, 0x2064, 0},
    /* Inhibit Symmetric Swapping..Activate Symmetric Swapping */
    {0x206A, 0x206F, 0},
    /* Soft Hyphen */
    {0x00AD, 0x00AD, 0},
    /* Mongolian Free Variation Selectors */
    {0x180B, 0x180E, 0},
    /* Variation Selectors */
    {0xFE00, 0xFE0F, 0},
    /* Variation Selectors Supplement */
    {0xE0100, 0xE01EF, 0},
};

#define ZERO_WIDTH_COUNT (sizeof(zeroWidthRanges) / sizeof(zeroWidthRanges[0]))

/* Binary search in range table */
static bool inRangeTable(uint32_t cp, const EawRange *table, size_t count) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].start) {
            hi = mid;
        } else if (cp > table[mid].end) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

/* Get East Asian Width of a codepoint */
int StrUnicodeEastAsianWidth(uint32_t codepoint) {
    /* Check for zero-width first */
    if (inRangeTable(codepoint, zeroWidthRanges, ZERO_WIDTH_COUNT)) {
        return 0;
    }

    /* Check for wide/fullwidth */
    if (inRangeTable(codepoint, eawWideRanges, EAW_WIDE_COUNT)) {
        return 2;
    }

    /* Default: narrow (1 cell) */
    return 1;
}

/* ====================================================================
 * General Category
 * ==================================================================== */

/* Unicode General Categories (simplified) */
typedef enum {
    GC_Cn = 0,  /* Not assigned */
    GC_Lu = 1,  /* Letter, uppercase */
    GC_Ll = 2,  /* Letter, lowercase */
    GC_Lt = 3,  /* Letter, titlecase */
    GC_Lm = 4,  /* Letter, modifier */
    GC_Lo = 5,  /* Letter, other */
    GC_Mn = 6,  /* Mark, nonspacing */
    GC_Mc = 7,  /* Mark, spacing combining */
    GC_Me = 8,  /* Mark, enclosing */
    GC_Nd = 9,  /* Number, decimal digit */
    GC_Nl = 10, /* Number, letter */
    GC_No = 11, /* Number, other */
    GC_Pc = 12, /* Punctuation, connector */
    GC_Pd = 13, /* Punctuation, dash */
    GC_Ps = 14, /* Punctuation, open */
    GC_Pe = 15, /* Punctuation, close */
    GC_Pi = 16, /* Punctuation, initial quote */
    GC_Pf = 17, /* Punctuation, final quote */
    GC_Po = 18, /* Punctuation, other */
    GC_Sm = 19, /* Symbol, math */
    GC_Sc = 20, /* Symbol, currency */
    GC_Sk = 21, /* Symbol, modifier */
    GC_So = 22, /* Symbol, other */
    GC_Zs = 23, /* Separator, space */
    GC_Zl = 24, /* Separator, line */
    GC_Zp = 25, /* Separator, paragraph */
    GC_Cc = 26, /* Other, control */
    GC_Cf = 27, /* Other, format */
    GC_Cs = 28, /* Other, surrogate */
    GC_Co = 29, /* Other, private use */
} GeneralCategory;

/* Check if codepoint is a letter */
bool StrUnicodeIsLetter(uint32_t cp) {
    /* ASCII fast path */
    if (cp < 0x80) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }

    /* Latin Extended-A */
    if (cp >= 0x0100 && cp <= 0x017F) {
        return true;
    }
    /* Latin Extended-B */
    if (cp >= 0x0180 && cp <= 0x024F) {
        return true;
    }
    /* Greek and Coptic */
    if (cp >= 0x0370 && cp <= 0x03FF) {
        /* Exclude some non-letter ranges */
        if (cp >= 0x0370 && cp <= 0x0373) {
            return true;
        }
        if (cp >= 0x0376 && cp <= 0x0377) {
            return true;
        }
        if (cp >= 0x037B && cp <= 0x037D) {
            return true;
        }
        if (cp >= 0x0386 && cp <= 0x0386) {
            return true;
        }
        if (cp >= 0x0388 && cp <= 0x03FF) {
            return true;
        }
    }
    /* Cyrillic */
    if (cp >= 0x0400 && cp <= 0x04FF) {
        return true;
    }
    /* Hebrew letters */
    if (cp >= 0x05D0 && cp <= 0x05EA) {
        return true;
    }
    /* Arabic letters */
    if (cp >= 0x0621 && cp <= 0x064A) {
        return true;
    }
    /* CJK Unified Ideographs */
    if (cp >= 0x4E00 && cp <= 0x9FFF) {
        return true;
    }
    /* Hiragana */
    if (cp >= 0x3041 && cp <= 0x3096) {
        return true;
    }
    /* Katakana */
    if (cp >= 0x30A1 && cp <= 0x30FA) {
        return true;
    }
    /* Hangul Syllables */
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        return true;
    }

    return false;
}

/* Check if codepoint is a digit */
bool StrUnicodeIsDigit(uint32_t cp) {
    /* ASCII digits */
    if (cp >= '0' && cp <= '9') {
        return true;
    }

    /* Fullwidth digits */
    if (cp >= 0xFF10 && cp <= 0xFF19) {
        return true;
    }

    /* Arabic-Indic digits */
    if (cp >= 0x0660 && cp <= 0x0669) {
        return true;
    }
    /* Extended Arabic-Indic digits */
    if (cp >= 0x06F0 && cp <= 0x06F9) {
        return true;
    }
    /* Devanagari digits */
    if (cp >= 0x0966 && cp <= 0x096F) {
        return true;
    }

    return false;
}

/* Check if codepoint is whitespace */
bool StrUnicodeIsSpace(uint32_t cp) {
    /* ASCII whitespace */
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
        cp == '\v') {
        return true;
    }

    /* Unicode whitespace */
    switch (cp) {
    case 0x00A0: /* No-Break Space */
    case 0x1680: /* Ogham Space Mark */
    case 0x2000: /* En Quad */
    case 0x2001: /* Em Quad */
    case 0x2002: /* En Space */
    case 0x2003: /* Em Space */
    case 0x2004: /* Three-Per-Em Space */
    case 0x2005: /* Four-Per-Em Space */
    case 0x2006: /* Six-Per-Em Space */
    case 0x2007: /* Figure Space */
    case 0x2008: /* Punctuation Space */
    case 0x2009: /* Thin Space */
    case 0x200A: /* Hair Space */
    case 0x2028: /* Line Separator */
    case 0x2029: /* Paragraph Separator */
    case 0x202F: /* Narrow No-Break Space */
    case 0x205F: /* Medium Mathematical Space */
    case 0x3000: /* Ideographic Space */
        return true;
    }
    return false;
}

/* Check if codepoint is alphanumeric */
bool StrUnicodeIsAlnum(uint32_t cp) {
    return StrUnicodeIsLetter(cp) || StrUnicodeIsDigit(cp);
}

/* ====================================================================
 * Grapheme Break Property
 * ==================================================================== */

/* Grapheme Break property values */
typedef enum {
    GBP_Other = 0,
    GBP_CR = 1,
    GBP_LF = 2,
    GBP_Control = 3,
    GBP_Extend = 4,
    GBP_ZWJ = 5,
    GBP_Regional_Indicator = 6,
    GBP_Prepend = 7,
    GBP_SpacingMark = 8,
    GBP_L = 9,    /* Hangul L */
    GBP_V = 10,   /* Hangul V */
    GBP_T = 11,   /* Hangul T */
    GBP_LV = 12,  /* Hangul LV */
    GBP_LVT = 13, /* Hangul LVT */
} GraphemeBreakProperty;

/* Get Grapheme Break Property for a codepoint */
int StrUnicodeGraphemeBreak(uint32_t cp) {
    /* CR and LF */
    if (cp == 0x000D) {
        return GBP_CR;
    }
    if (cp == 0x000A) {
        return GBP_LF;
    }

    /* Control characters */
    if (cp < 0x0020) {
        return GBP_Control;
    }
    if (cp >= 0x007F && cp <= 0x009F) {
        return GBP_Control;
    }

    /* Zero Width Joiner */
    if (cp == 0x200D) {
        return GBP_ZWJ;
    }

    /* Regional Indicators */
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        return GBP_Regional_Indicator;
    }

    /* Hangul Jamo */
    if (cp >= 0x1100 && cp <= 0x115F) {
        return GBP_L;
    }
    if (cp >= 0xA960 && cp <= 0xA97C) {
        return GBP_L;
    }
    if (cp >= 0x1160 && cp <= 0x11A7) {
        return GBP_V;
    }
    if (cp >= 0xD7B0 && cp <= 0xD7C6) {
        return GBP_V;
    }
    if (cp >= 0x11A8 && cp <= 0x11FF) {
        return GBP_T;
    }
    if (cp >= 0xD7CB && cp <= 0xD7FB) {
        return GBP_T;
    }

    /* Hangul Syllables (LV and LVT) */
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        /* LV syllables are at positions 0, 28, 56, ... from base */
        uint32_t offset = cp - 0xAC00;
        if (offset % 28 == 0) {
            return GBP_LV;
        }
        return GBP_LVT;
    }

    /* Extend (combining marks) */
    if (cp >= 0x0300 && cp <= 0x036F) {
        return GBP_Extend;
    }
    if (cp >= 0x1AB0 && cp <= 0x1AFF) {
        return GBP_Extend;
    }
    if (cp >= 0x1DC0 && cp <= 0x1DFF) {
        return GBP_Extend;
    }
    if (cp >= 0x20D0 && cp <= 0x20FF) {
        return GBP_Extend;
    }
    if (cp >= 0xFE20 && cp <= 0xFE2F) {
        return GBP_Extend;
    }

    /* Variation selectors */
    if (cp >= 0xFE00 && cp <= 0xFE0F) {
        return GBP_Extend;
    }
    if (cp >= 0xE0100 && cp <= 0xE01EF) {
        return GBP_Extend;
    }

    /* Format characters that are Control */
    if (cp == 0x200B || cp == 0x200C) {
        return GBP_Control;
    }
    if (cp >= 0x2060 && cp <= 0x206F) {
        return GBP_Control;
    }
    if (cp == 0xFEFF) {
        return GBP_Control;
    }

    return GBP_Other;
}

/* Check if there's a grapheme break between two codepoints */
bool StrUnicodeIsGraphemeBreak(uint32_t cp1, uint32_t cp2) {
    int gbp1 = StrUnicodeGraphemeBreak(cp1);
    int gbp2 = StrUnicodeGraphemeBreak(cp2);

    /* GB3: CR x LF */
    if (gbp1 == GBP_CR && gbp2 == GBP_LF) {
        return false;
    }

    /* GB4: (Control | CR | LF) ÷ */
    if (gbp1 == GBP_Control || gbp1 == GBP_CR || gbp1 == GBP_LF) {
        return true;
    }

    /* GB5: ÷ (Control | CR | LF) */
    if (gbp2 == GBP_Control || gbp2 == GBP_CR || gbp2 == GBP_LF) {
        return true;
    }

    /* GB6: L x (L | V | LV | LVT) */
    if (gbp1 == GBP_L &&
        (gbp2 == GBP_L || gbp2 == GBP_V || gbp2 == GBP_LV || gbp2 == GBP_LVT)) {
        return false;
    }

    /* GB7: (LV | V) x (V | T) */
    if ((gbp1 == GBP_LV || gbp1 == GBP_V) && (gbp2 == GBP_V || gbp2 == GBP_T)) {
        return false;
    }

    /* GB8: (LVT | T) x T */
    if ((gbp1 == GBP_LVT || gbp1 == GBP_T) && gbp2 == GBP_T) {
        return false;
    }

    /* GB9: x (Extend | ZWJ) */
    if (gbp2 == GBP_Extend || gbp2 == GBP_ZWJ) {
        return false;
    }

    /* GB9a: x SpacingMark */
    if (gbp2 == GBP_SpacingMark) {
        return false;
    }

    /* GB9b: Prepend x */
    if (gbp1 == GBP_Prepend) {
        return false;
    }

    /* GB11: ZWJ x (Extended_Pictographic) - simplified, assume emoji follow ZWJ
     */
    /* GB12/13: Regional_Indicator handling needs state, simplified here */
    if (gbp1 == GBP_Regional_Indicator && gbp2 == GBP_Regional_Indicator) {
        /* This is a simplification - proper handling needs to count RI pairs */
        return false;
    }

    /* GB999: Any ÷ Any */
    return true;
}
