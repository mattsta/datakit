#pragma once

#include "databox.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* str.{c,h} contain utilities extracted from sqlite3, luajit,
 * and implementations of wrapper helper functions otherwise not
 * provided by standard libraries.  Everything has been rewritten
 * for datakit coding standards and container types. */

/*
** Estimated quantities used for query planning are stored as 16-bit
** logarithms.  For quantity X, the value stored is 10*log2(X).  This
** gives a possible range of values of approximately 1.0e986 to 1e-986.
** But the allowed values are "grainy".  Not every value is representable.
** For example, quantities 16 and 17 are both represented by a LogEst
** of 40.  However, since LogEst quantities are suppose to be estimates,
** not exact values, this imprecision is not a problem.
**
** "LogEst" is short for "Logarithmic Estimate".
**
** Examples:
**      1 -> 0              20 -> 43          10000 -> 132
**      2 -> 10             25 -> 46          25000 -> 146
**      3 -> 16            100 -> 66        1000000 -> 199
**      4 -> 20           1000 -> 99        1048576 -> 200
**     10 -> 33           1024 -> 100    4294967296 -> 320
**
** The LogEst can be negative to indicate fractional values.
** Examples:
**
**    0.5 -> -10           0.1 -> -33        0.0625 -> -40
*/
typedef int16_t LogEst;

/*
** Round up a number to the next larger multiple of 8.  This is used
** to force 8-byte alignment on 64-bit architectures.
*/
#define ROUND8(x) (((x) + 7) & ~7)

/*
** Round down to the nearest multiple of 8
*/
#define ROUNDDOWN8(x) ((x) & ~7)

/* These global arrays are defined in str.c */
extern const char StrIdChar[];
extern const unsigned char StrCtypeMap[256];
extern const unsigned char StrUpperToLower[];

/*
** The following macros mimic the standard library functions toupper(),
** isspace(), isalnum(), isdigit() and isxdigit(), respectively. The
** sqlite versions only work for ASCII characters, regardless of locale.
*/

/* Only use C functions if we require locale conversions.
 * Otherwise, use our much faster lookup tables. */
#ifdef STRHELPER_STRICT_COMPLIANCE
#include <ctype.h> /* for avoiding use of built-in lookup table */
#define StrToupper(x) toupper((unsigned char)(x))
#define StrIsspace(x) isspace((unsigned char)(x))
#define StrIsalnum(x) isalnum((unsigned char)(x))
#define StrIsalpha(x) isalpha((unsigned char)(x))
#define StrIsdigit(x) isdigit((unsigned char)(x))
#define StrIsxdigit(x) isxdigit((unsigned char)(x))
#define StrTolower(x) tolower((unsigned char)(x))
#define StrIsquote(x) ((x) == '"' || (x) == '\'' || (x) == '[' || (x) == '`')
#else
#define StrToupper(x) ((x) & ~(StrCtypeMap[(unsigned char)(x)] & 0x20))
#define StrIsspace(x) (StrCtypeMap[(unsigned char)(x)] & 0x01)
#define StrIsalnum(x) (StrCtypeMap[(unsigned char)(x)] & 0x06)
#define StrIsalpha(x) (StrCtypeMap[(unsigned char)(x)] & 0x02)
#define StrIsdigit(x) (StrCtypeMap[(unsigned char)(x)] & 0x04)
#define StrIsxdigit(x) (StrCtypeMap[(unsigned char)(x)] & 0x08)
#define StrTolower(x) (StrUpperToLower[(unsigned char)(x)])
#define StrIsquote(x) (StrCtypeMap[(unsigned char)(x)] & 0x80)
#endif

#define StrIsIdChar(C)                                                         \
    (((c = (C)) & 0x80) != 0 || (c > 0x1f && StrIdChar[c - 0x20]))

int StrICmp(const char *zLeft, const char *zRight);
int StrNICmp(const char *zLeft, const char *zRight, int N);

/* These positions matter because StrAtoi64 does math based on them */
typedef enum strEnc {
    STR_INVALID = 0,
    STR_UTF8,
    STR_UTF16LE,
    STR_UTF16BE,
} strEnc;

/* ====================================================================
 * String to Native Type Conversions (from sqlite3)
 * ==================================================================== */
bool StrAtoF(const void *_z, double *pResult, int32_t length, strEnc enc,
             bool skipSpaces);
int32_t StrAtoi64(const void *zNum, int64_t *pNum, int32_t length, uint8_t enc,
                  bool skipSpaces);

int64_t StrDoubleToInt64(double r);
bool StrDoubleCanBeCastToInt64(double r);

/* ====================================================================
 * Log Estimation (from sqlite3)
 * ==================================================================== */
LogEst StrLogEstAdd(LogEst a, LogEst b);
LogEst StrLogEst(uint64_t x);
LogEst StrLogEstFromDouble(double x);
uint64_t StrLogEstToInt(LogEst x);

/* ====================================================================
 * UTF-8 Codepoint Counting
 * ==================================================================== */
size_t StrLenUtf8(const void *_ss, size_t len);
size_t StrLenUtf8Scalar(const void *_ss, size_t len); /* For benchmarking */
size_t StrLenUtf8CountBytes(const void *_ss, size_t len,
                            size_t countCharacters);

/* ====================================================================
 * UTF-8 Validation
 * ==================================================================== */
/* Validate UTF-8 encoding with known length (SIMD-optimized) */
bool StrUtf8Valid(const void *_str, size_t len);
/* Validate null-terminated UTF-8 string (SIMD-optimized) */
bool StrUtf8ValidCStr(const char *str);
/* Scalar implementations for benchmarking */
bool StrUtf8ValidScalar(const void *_str, size_t len);
bool StrUtf8ValidCStrScalar(const char *str);

/* Validate and count codepoints in one pass */
size_t StrUtf8ValidCount(const void *_str, size_t len, bool *valid);
/* Get byte length for N codepoints (validates while counting) */
size_t StrUtf8ValidCountBytes(const void *_str, size_t len,
                              size_t numCodepoints, bool *valid);

/* ====================================================================
 * UTF-8 Encoding/Decoding
 * ==================================================================== */
/* Decode a UTF-8 sequence to a Unicode codepoint.
 * Returns codepoint and advances *str, or 0xFFFFFFFF on error. */
uint32_t StrUtf8Decode(const uint8_t **str, size_t *remaining);
/* Encode a Unicode codepoint to UTF-8. Returns bytes written (1-4) or 0. */
size_t StrUtf8Encode(uint8_t *dst, uint32_t codepoint);
/* Get expected byte length for a codepoint (1-4, or 0 if invalid) */
size_t StrUtf8CodepointLen(uint32_t codepoint);
/* Get byte length of UTF-8 sequence from first byte (0 if invalid start) */
size_t StrUtf8SequenceLen(uint8_t firstByte);

/* ====================================================================
 * UTF-8 Cursor/Iterator Operations
 * ==================================================================== */
/* Advance cursor by N codepoints, returns new position */
const uint8_t *StrUtf8Advance(const void *str, size_t len, size_t n);
/* Move cursor back by N codepoints, returns new position */
const uint8_t *StrUtf8Retreat(const void *str, size_t len, const uint8_t *pos,
                              size_t n);
/* Get codepoint at position without advancing */
uint32_t StrUtf8Peek(const void *str, size_t len, const uint8_t *pos);
/* Get byte offset for Nth codepoint (0-indexed) */
size_t StrUtf8OffsetAt(const void *str, size_t len, size_t charIndex);
/* Get codepoint index for byte offset */
size_t StrUtf8IndexAt(const void *str, size_t len, size_t byteOffset);
/* Scalar implementation for benchmarking */
const uint8_t *StrUtf8AdvanceScalar(const void *str, size_t len, size_t n);

/* ====================================================================
 * UTF-8 Truncation/Substring Operations
 * ==================================================================== */
/* Get byte length for first N codepoints */
size_t StrUtf8Truncate(const void *str, size_t len, size_t maxChars);
/* Truncate to max bytes at valid UTF-8 boundary */
size_t StrUtf8TruncateBytes(const void *str, size_t len, size_t maxBytes);
/* Extract substring by codepoint indices (start inclusive, end exclusive) */
void StrUtf8Substring(const void *str, size_t len, size_t startChar,
                      size_t endChar, size_t *outOffset, size_t *outLen);
/* Extract substring and copy to buffer, returns bytes written */
size_t StrUtf8SubstringCopy(const void *str, size_t len, size_t startChar,
                            size_t endChar, void *buf, size_t bufLen);
/* Find byte offset that splits string at Nth codepoint */
size_t StrUtf8Split(const void *str, size_t len, size_t charIndex);

/* ====================================================================
 * UTF-8 String Comparison Operations
 * ==================================================================== */
/* Compare two UTF-8 strings byte-by-byte */
int StrUtf8Compare(const void *s1, size_t len1, const void *s2, size_t len2);
/* Compare first N codepoints */
int StrUtf8CompareN(const void *s1, size_t len1, const void *s2, size_t len2,
                    size_t n);
/* ASCII case-insensitive comparison */
int StrUtf8CompareCaseInsensitiveAscii(const void *s1, size_t len1,
                                       const void *s2, size_t len2);
/* Check if string starts with prefix (by codepoints) */
bool StrUtf8StartsWith(const void *str, size_t strLen, const void *prefix,
                       size_t prefixLen);
/* Check if string ends with suffix (by codepoints) */
bool StrUtf8EndsWith(const void *str, size_t strLen, const void *suffix,
                     size_t suffixLen);
/* Equality check (convenience wrapper) */
bool StrUtf8Equal(const void *s1, size_t len1, const void *s2, size_t len2);
/* Equality check (ASCII case-insensitive) */
bool StrUtf8EqualCaseInsensitiveAscii(const void *s1, size_t len1,
                                      const void *s2, size_t len2);

/* ====================================================================
 * UTF-8 Search Operations
 * ==================================================================== */
/* Find first occurrence of substring, returns byte offset or SIZE_MAX */
size_t StrUtf8Find(const void *haystack, size_t haystackLen, const void *needle,
                   size_t needleLen);
/* Find last occurrence of substring */
size_t StrUtf8FindLast(const void *haystack, size_t haystackLen,
                       const void *needle, size_t needleLen);
/* Find first occurrence of codepoint */
size_t StrUtf8FindChar(const void *str, size_t len, uint32_t codepoint);
/* Find last occurrence of codepoint */
size_t StrUtf8FindCharLast(const void *str, size_t len, uint32_t codepoint);
/* Find Nth occurrence of codepoint (0-indexed) */
size_t StrUtf8FindCharNth(const void *str, size_t len, uint32_t codepoint,
                          size_t n);
/* Check if substring exists */
bool StrUtf8Contains(const void *haystack, size_t haystackLen,
                     const void *needle, size_t needleLen);
/* Count non-overlapping occurrences of substring */
size_t StrUtf8Count(const void *haystack, size_t haystackLen,
                    const void *needle, size_t needleLen);
/* Count occurrences of codepoint */
size_t StrUtf8CountChar(const void *str, size_t len, uint32_t codepoint);
/* Find first occurrence of any codepoint from set */
size_t StrUtf8FindAnyChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen);
/* Find first codepoint NOT in set */
size_t StrUtf8FindNotChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen);
/* Byte length of initial segment containing only chars from set */
size_t StrUtf8SpanChar(const void *str, size_t len, const void *charSet,
                       size_t charSetLen);
/* Byte length of initial segment containing no chars from set */
size_t StrUtf8SpanNotChar(const void *str, size_t len, const void *charSet,
                          size_t charSetLen);

/* ====================================================================
 * ASCII Case Conversion
 * ==================================================================== */
/* In-place lowercase conversion (ASCII letters only) */
void StrAsciiToLower(void *str, size_t len);
/* In-place uppercase conversion (ASCII letters only) */
void StrAsciiToUpper(void *str, size_t len);
/* Copy with lowercase conversion */
size_t StrAsciiToLowerCopy(void *dst, size_t dstLen, const void *src,
                           size_t srcLen);
/* Copy with uppercase conversion */
size_t StrAsciiToUpperCopy(void *dst, size_t dstLen, const void *src,
                           size_t srcLen);
/* Check if all ASCII letters are lowercase */
bool StrAsciiIsLower(const void *str, size_t len);
/* Check if all ASCII letters are uppercase */
bool StrAsciiIsUpper(const void *str, size_t len);

/* ====================================================================
 * Unicode Character Properties
 * ==================================================================== */
/* Get East Asian Width (0=zero-width, 1=narrow, 2=wide) */
int StrUnicodeEastAsianWidth(uint32_t codepoint);
/* Check if codepoint is a letter */
bool StrUnicodeIsLetter(uint32_t cp);
/* Check if codepoint is a digit */
bool StrUnicodeIsDigit(uint32_t cp);
/* Check if codepoint is whitespace */
bool StrUnicodeIsSpace(uint32_t cp);
/* Check if codepoint is alphanumeric */
bool StrUnicodeIsAlnum(uint32_t cp);
/* Get Grapheme Break Property */
int StrUnicodeGraphemeBreak(uint32_t cp);
/* Check if there's a grapheme break between two codepoints */
bool StrUnicodeIsGraphemeBreak(uint32_t cp1, uint32_t cp2);

/* ====================================================================
 * UTF-8 Display Width
 * ==================================================================== */
/* Calculate display width in terminal cells */
size_t StrUtf8Width(const void *str, size_t len);
/* Calculate width of first n codepoints */
size_t StrUtf8WidthN(const void *str, size_t len, size_t n);
/* Truncate to fit within maxWidth display cells */
size_t StrUtf8TruncateWidth(const void *str, size_t len, size_t maxWidth);
/* Find byte index at target display width */
size_t StrUtf8IndexAtWidth(const void *str, size_t len, size_t targetWidth);
/* Get display width from start to byte offset */
size_t StrUtf8WidthAt(const void *str, size_t len, size_t offset);
/* Calculate padding needed for target width */
size_t StrUtf8PadWidth(const void *str, size_t len, size_t targetWidth);
/* Calculate width between two byte offsets */
size_t StrUtf8WidthBetween(const void *str, size_t len, size_t startOffset,
                           size_t endOffset);
/* Check if all characters are narrow (width 1) */
bool StrUtf8IsNarrow(const void *str, size_t len);
/* Check if string contains wide characters */
bool StrUtf8HasWide(const void *str, size_t len);

/* ====================================================================
 * UTF-8 Grapheme Cluster Operations
 * ==================================================================== */
/* Find byte offset of end of next grapheme cluster */
size_t StrUtf8GraphemeNext(const void *str, size_t len);
/* Count grapheme clusters in string */
size_t StrUtf8GraphemeCount(const void *str, size_t len);
/* Advance by n grapheme clusters, returns byte offset */
size_t StrUtf8GraphemeAdvance(const void *str, size_t len, size_t n);
/* Get byte range of nth grapheme cluster (0-indexed) */
bool StrUtf8GraphemeAt(const void *str, size_t len, size_t n, size_t *startOut,
                       size_t *endOut);
/* Display width counting grapheme clusters correctly */
size_t StrUtf8GraphemeWidth(const void *str, size_t len);
/* Truncate to n grapheme clusters */
size_t StrUtf8GraphemeTruncate(const void *str, size_t len, size_t n);
/* Reverse by grapheme clusters in-place */
void StrUtf8GraphemeReverse(void *str, size_t len);

/* ====================================================================
 * String to Native Type Conversions (from luajit)
 * ==================================================================== */
typedef enum StrScanFmt {
    STRSCAN_ERROR,
    STRSCAN_NUM,
    STRSCAN_IMAG,
    STRSCAN_INT,
    STRSCAN_U32,
    STRSCAN_I64,
    STRSCAN_U64,
} StrScanFmt;

/* Options for accepted/returned formats. */
typedef enum StrScanOpt {
    STRSCAN_OPT_TOINT = 0x01, /* Convert to int32_t, if possible. */
    STRSCAN_OPT_TONUM = 0x02, /* Always convert to double. */
    STRSCAN_OPT_IMAG = 0x04,
    STRSCAN_OPT_LL = 0x08,
    STRSCAN_OPT_C = 0x10,
} StrScanOpt;

StrScanFmt StrScanScan(const uint8_t *p, databox *box, StrScanOpt opt,
                       bool allowFloatWords, bool skipSpaces);
bool StrScanToDouble(const void *str, databox *box, bool allowFloatWords,
                     bool skipSpaces);

bool StrScanScanReliable(const void *p, const size_t len, databox *box);
bool StrScanScanReliableConvert128(const void *p, const size_t len,
                                   databox *small, databoxBig *box,
                                   databoxBig **use);
bool StrScanScanReliableConvert128PreVerified(const void *p, const size_t len,
                                              databox *small, databoxBig *box,
                                              databoxBig **use);

/* ====================================================================
 * Fixed-With Bloom Static Filter (from luajit)
 * ==================================================================== */
/* A really naive Bloom filter.
 * Can use as a simple "not in container" filter for small
 * element counts.
 *
 * Assuming a 64 bit container:
 * 8 elements   = 11% false positive rate.
 * 16 elements  = 22% false positive rate.
 * 32 elements  = 39% false positive rate.
 * 64 elements  = 63% false positive rate.
 * 128 elements = 87% false positive rate.
 * 256 elements = 98% false positive rate.
 * 512 elements = 99.9% false positive rate.
 *
 * (calculated by (1 - e^(-(elements/bits)))) */
typedef uintptr_t StrBloomFilter;
#define _STR_BLOOM_MASK ((8 * sizeof(StrBloomFilter)) - 1)
#define _StrBloomBit(x) ((StrBloomFilter)1 << ((x) & _STR_BLOOM_MASK))
#define StrBloomSet(b, x) ((b) |= _StrBloomBit((x)))
#define StrBloomTest(b, x) ((b) & _StrBloomBit((x)))

/* Assuming a 128 bit container:
 * 8 elements   =  6% false positive rate.
 * 16 elements  = 11% false positive rate.
 * 32 elements  = 22% false positive rate.
 * 64 elements  = 39% false positive rate.
 * 128 elements = 63% false positive rate.
 * 256 elements = 86% false positive rate.
 * 512 elements = 98% false positive rate.
 *
 * calculated by:
 * [[elements,
 *  (1 - math.exp(-(elements)/128.0))]
 *  for elements in [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]] */
typedef __uint128_t StrBloomFilterBig;
#define _STR_BIG_BLOOM_MASK ((8 * sizeof(StrBloomFilterBig)) - 1)
#define _StrBloomBigBit(x) ((StrBloomFilter)1 << ((x) & _STR_BIG_BLOOM_MASK))
#define StrBloomBigSet(b, x) ((b) |= _StrBloomBigBit((x)))
#define StrBloomBigTest(b, x) ((b) & _StrBloomBigBit((x)))

/* ====================================================================
 * Alternative String to Native Types and Native Types to Strings
 * ==================================================================== */
#if 0
/* NOTE: Some compilers generate _more_ efficient code using the shifting
 *       method than the direct multiply method. If you're on one of the
 *       compilers that tests better with shifting, you can enable it here. */
/* This is a fancy (and slightly more faster and time stable) way of
 * doing "* 10" or "* 100" against a value. */
#define StrValTimes10(val) (((val) << 1) + ((val) << 3))
#define StrValTimes100(val) StrValTimes10(StrValTimes10(val))
#else
#define StrValTimes10(val) ((val) * 10)
#define StrValTimes100(val) ((val) * 100)
#endif

size_t StrInt64ToBuf(void *dst, size_t dstLen, int64_t svalue);
size_t StrUInt32ToBuf(void *dst, size_t dstLen, uint32_t value);
size_t StrUInt64ToBuf(void *dst, size_t dstLen, uint64_t svalue);

size_t StrUInt128ToBuf(void *dst, size_t dstLen, __uint128_t value);
size_t StrInt128ToBuf(void *dst, size_t dstLen, __int128_t svalue);

size_t StrUInt64ToBufTable(void *dst, size_t dstLen, uint64_t value);
size_t StrInt64ToBufTable(void *dst, size_t dstLen, int64_t svalue);
int32_t StrDoubleToBuf(void *buf, size_t len, double value);
bool StrBufToInt64(const void *s, size_t slen, int64_t *value);
bool StrBufToUInt64(const void *s, size_t slen, uint64_t *value);
bool StrBufToLongDouble(const void *s, size_t slen, long double *dp);
uint64_t StrBufToUInt64Fast(const void *buf, size_t len);
uint64_t StrBufToUInt64Scalar(const void *buf,
                              size_t len); /* For benchmarking */
bool StrBufToUInt64FastCheckNumeric(const void *buf_, size_t len,
                                    uint64_t *value);
bool StrBufToUInt64FastCheckOverflow(const void *buf, size_t len,
                                     uint64_t *value);
bool StrBufToUInt128(const void *buf, const size_t bufLen, __uint128_t *value);
bool StrBufToInt128(const void *buf, const size_t bufLen, __int128_t *value);

bool StrIsDigitsIndividual(const void *buf_, const size_t size);
bool StrIsDigitsFast(const void *buf_, size_t size);

/* Note: your buf must have at least 9 or 8 or 4 bytes available for: */
void StrUInt9DigitsToBuf(void *p_, uint32_t u);
void StrUInt8DigitsToBuf(void *p_, uint32_t u);
void StrUInt4DigitsToBuf(void *p_, uint32_t u);

size_t StrDigitCountUInt32(uint32_t v);
size_t StrDigitCountInt64(int64_t v);
size_t StrDigitCountUInt64(uint64_t v);
size_t StrDigitCountUInt128(__uint128_t v);

/* ====================================================================
 * Fast Bit Population Counting
 * ==================================================================== */
uint64_t StrPopCnt8Bit(const void *_data, const size_t len);
uint64_t StrPopCntAligned(const void *_data, size_t len);
uint64_t StrPopCntExact(const void *_data, size_t len);
uint64_t StrPopCntScalar(const void *_data, size_t len); /* For benchmarking */

/* ====================================================================
 * Fast Bitmap Readers
 * ==================================================================== */
uint32_t StrBitmapGetSetPositionsExact8(const void *data, size_t len,
                                        uint8_t position[]);
uint32_t StrBitmapGetSetPositionsExact16(const void *data, size_t len,
                                         uint16_t position[]);
uint32_t StrBitmapGetSetPositionsExact32(const void *data, size_t len,
                                         uint32_t position[]);
uint64_t StrBitmapGetSetPositionsExact64(const void *data, size_t len,
                                         uint64_t position[]);

uint32_t StrBitmapGetUnsetPositionsExact8(const void *data, size_t len,
                                          uint8_t position[]);
uint32_t StrBitmapGetUnsetPositionsExact16(const void *data, size_t len,
                                           uint16_t position[]);
uint32_t StrBitmapGetUnsetPositionsExact32(const void *data, size_t len,
                                           uint32_t position[]);
uint64_t StrBitmapGetUnsetPositionsExact64(const void *data, size_t len,
                                           uint64_t position[]);

/* ====================================================================
 * Fast (but not secure) Random Number Generators
 * ==================================================================== */
void xorshift128(uint32_t *restrict x, uint32_t *restrict y,
                 uint32_t *restrict z, uint32_t *restrict w);
uint64_t xorshift64star(uint64_t *x);
uint64_t xorshift1024star(uint64_t s[16], uint8_t *restrict sIndex);
uint64_t xorshift128plus(uint64_t s[2]);
uint64_t xoroshiro128plus(uint64_t s[2]);
uint64_t splitmix64(uint64_t *x);

uint64_t StrTenPow(size_t exp);
__uint128_t StrTenPowBig(size_t exp);

#ifdef DATAKIT_TEST
int strTest(int argc, char *argv[]);
#endif
