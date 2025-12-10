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
