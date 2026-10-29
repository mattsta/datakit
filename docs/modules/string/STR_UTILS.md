# Extended String Utilities

## Overview

The `str/` directory contains **specialized high-performance string utilities** optimized for specific operations. These include fast integer conversions, UTF-8 processing, bit manipulation, and numeric validation.

**Key Features:**

- SIMD-optimized digit validation (SSE2 and NEON)
- SWAR-based fast integer-to-string conversion
- A complete UTF-8 layer: validation, encode/decode, cursors, search,
  comparison, display width, and grapheme clusters
- Population count (popcount) with manual dependency breaking
- Bitmap position extraction
- Fast random number generators

**Location**: `src/str/` directory

**Included by**: `str.c`, which textually `#include`s each `str/*.c` file so
the whole set compiles as one translation unit. There is no separate
`strUtils.h` — everything here is declared in `str.h`.

**Scope**: this document is the implementation-oriented tour of `src/str/`.
[STR.md](STR.md) is the API reference for the same header; where the two
overlap, STR.md describes _what to call_ and this file describes _how it
works_.

## Fast Integer-to-String Conversion

File: `strToBufFast.c`

### SWAR-Based Conversion

Uses **SIMD Within A Register** (SWAR) techniques for parallel digit extraction:

```c
/* Convert 32-bit unsigned integer */
size_t StrUInt32ToBuf(void *dst, size_t dstLen, uint32_t value);

/* Convert 64-bit unsigned integer */
size_t StrUInt64ToBuf(void *dst, size_t dstLen, uint64_t value);

/* Convert signed integers */
size_t StrInt64ToBuf(void *dst, size_t dstLen, int64_t value);

/* Convert 128-bit integers */
size_t StrUInt128ToBuf(void *dst, size_t dstLen, __uint128_t value);
size_t StrInt128ToBuf(void *dst, size_t dstLen, __int128_t value);
```

### Fixed-Width Batch Conversions

```c
/* Write exactly 9 digits (requires 9 bytes) */
void StrUInt9DigitsToBuf(void *p, uint32_t u);

/* Write exactly 8 digits (requires 8 bytes) */
void StrUInt8DigitsToBuf(void *p, uint32_t u);

/* Write exactly 4 digits (requires 4 bytes) */
void StrUInt4DigitsToBuf(void *p, uint32_t u);
```

**Example:**

```c
char buf[20];

/* Convert 123456789 in 9-digit chunks */
StrUInt9DigitsToBuf(buf, 123456789);
/* buf = "123456789" (not null-terminated) */

/* Fast batch conversion for 128-bit */
__uint128_t huge = ((__uint128_t)1 << 127) - 1;
size_t len = StrUInt128ToBuf(buf, sizeof(buf), huge);
printf("%.*s\n", (int)len, buf);
```

### SWAR Magic Explained

The conversion uses fixed-point multiplication for SIMD-style digit extraction:

```c
/* Encode two 2-digit numbers into 4 BCD bytes */
static uint32_t encodeHundreds_(uint32_t hi, uint32_t lo) {
    /* Pack: merged = [hi 0 lo 0] */
    const uint32_t merged = hi | (lo << 16);

    /* Fixed-point multiply: 103/1024 ≈ 1/10 */
    uint32_t tens = (merged * 103UL) >> 10;
    tens &= (0xFUL << 16) | 0xFUL;

    /* Extract ones: merged - 10*tens */
    return tens + ((merged - 10UL * tens) << 8);
}
```

This processes **4 digits in parallel** using a single multiply and shift.

### Performance

The SWAR converter generates four digits per multiply-and-shift instead of
one digit per divide, and writes them without a `printf` format walk. That
is where the win over `snprintf` comes from. `StrUInt64ToBufTable` trades
the multiplies for a lookup table; which one wins is
platform-dependent — benchmark both under the `perf` CMake preset rather
than assuming.

## Table-Based Integer Conversion

File: `strToBufTable.c`

Alternative conversion using lookup tables:

```c
/* Table-based conversion (may be faster on some platforms) */
size_t StrUInt64ToBufTable(void *dst, size_t dstLen, uint64_t value);
size_t StrInt64ToBufTable(void *dst, size_t dstLen, int64_t value);
```

**When to use:**

- Platforms without efficient multiply instructions
- CPUs with large fast caches
- Benchmarking shows superiority on your platform

## Multi-Digit Splatting

File: `strToBufSplat.c`

Helpers for writing multiple digits at once (used internally by SWAR converters).

## String-to-Integer Conversion

File: `strToNative.c`

### Safe Conversions with Validation

```c
/* Convert with full validation and overflow checking */
bool StrBufToInt64(const void *s, size_t slen, int64_t *value);
bool StrBufToUInt64(const void *s, size_t slen, uint64_t *value);

/* 128-bit conversions */
bool StrBufToUInt128(const void *buf, size_t bufLen, __uint128_t *value);
bool StrBufToInt128(const void *buf, size_t bufLen, __int128_t *value);
```

**Validation Rules:**

- No leading zeros (except "0" alone)
- No leading/trailing spaces
- No + sign
- All bytes must be digits
- Must not overflow

**Example:**

```c
int64_t value;

/* These succeed */
assert(StrBufToInt64("0", 1, &value));
assert(StrBufToInt64("123", 3, &value));
assert(StrBufToInt64("-9223372036854775808", 20, &value));

/* These fail */
assert(!StrBufToInt64("00", 2, &value));           /* Leading zero */
assert(!StrBufToInt64(" 1", 2, &value));           /* Space */
assert(!StrBufToInt64("+1", 2, &value));           /* Plus sign */
assert(!StrBufToInt64("9223372036854775808", 19, &value));  /* Overflow */
```

### Fast Unsafe Conversions

```c
/* Fastest - no checks, must be all digits */
uint64_t StrBufToUInt64Fast(const void *buf, size_t len);

/* With basic numeric validation */
bool StrBufToUInt64FastCheckNumeric(const void *buf, size_t len,
                                    uint64_t *value);

/* With overflow checking only */
bool StrBufToUInt64FastCheckOverflow(const void *buf, size_t len,
                                     uint64_t *value);
```

**Example:**

```c
const char *digits = "123456789";

/* Fastest - assumes all digits */
uint64_t fast = StrBufToUInt64Fast(digits, 9);

/* Validates digits but not overflow */
uint64_t checked;
if (StrBufToUInt64FastCheckNumeric(digits, 9, &checked)) {
    printf("Value: %" PRIu64 "\n", checked);
}

/* Validates overflow but not digit characters */
uint64_t safe;
if (StrBufToUInt64FastCheckOverflow(digits, 9, &safe)) {
    printf("Safe: %" PRIu64 "\n", safe);
}
```

### 128-bit Integer Conversion

```c
/* Convert up to 39 decimal digits */
bool StrBufToUInt128(const void *buf, size_t bufLen, __uint128_t *value) {
    /* Process in 18-digit chunks (fits in uint64_t) */
    while (position < bufLen) {
        const size_t chunk = MIN(18, bufLen - position);
        const uint64_t converted = StrBufToUInt64Fast(&buf[position], chunk);
        const uint64_t multiplier = StrTenPow(chunk);

        result *= multiplier;
        result += converted;
        position += chunk;
    }
}
```

**Example:**

```c
const char *huge = "170141183460469231731687303715884105727";
__int128_t value;

if (StrBufToInt128(huge, strlen(huge), &value)) {
    /* Successfully converted INT128_MAX */
}
```

## UTF-8 Operations

File: `strUTF8.c`

### Character Counting

```c
/* Count UTF-8 characters (not bytes) */
size_t StrLenUtf8(const void *ss, size_t len);

/* Get byte count for N characters */
size_t StrLenUtf8CountBytes(const void *ss, size_t len,
                            size_t countCharacters);
```

### SWAR UTF-8 Counting

The implementation uses SIMD-like bit manipulation to count multibyte characters:

```c
/* Process 8 bytes at once */
size_t u = *(size_t *)s;

/* Identify continuation bytes (10xxxxxx) */
u = ((u & (ONEMASK * 0x80)) >> 7) & ((~u) >> 6);

/* Count them */
countMultibyteExtra += (u * ONEMASK) >> ((sizeof(size_t) - 1) * 8);
```

**Magic Breakdown:**

Given string `"ab💛cd"` (2 ASCII + 1 emoji (4 bytes) + 2 ASCII = 8 bytes):

```
Binary: 01100001 01100010 11110000 10011111 10010010 10011011 01100011 01100100
                           ^        ^^       ^^       ^^
                           |        continuation bytes
                           start

After processing:
- Identifies 3 continuation bytes
- Returns: 8 bytes - 3 extra = 5 characters ✓
```

**Example:**

```c
const char *text = "Hello 💛";  /* 6 ASCII + 1 emoji (4 bytes) */

size_t chars = StrLenUtf8(text, strlen(text));  /* 7 characters */
size_t bytes = strlen(text);                    /* 10 bytes */

printf("Text has %zu characters in %zu bytes\n", chars, bytes);

/* Get bytes for first 3 characters */
size_t threeCharBytes = StrLenUtf8CountBytes(text, bytes, 3);
printf("First 3 chars use %zu bytes\n", threeCharBytes);  /* 3 */

/* Extract substring */
char sub[8];
memcpy(sub, text, threeCharBytes);
sub[threeCharBytes] = '\0';
printf("Substring: %s\n", sub);  /* "Hel" */
```

### Substring Extraction

```c
/* Extract first N UTF-8 characters */
size_t extractUtf8Chars(const char *str, size_t maxChars,
                        char *output, size_t outputSize) {
    size_t len = strlen(str);
    size_t bytes = StrLenUtf8CountBytes(str, len, maxChars);

    if (bytes < outputSize) {
        memcpy(output, str, bytes);
        output[bytes] = '\0';
        return bytes;
    }

    return 0;  /* Buffer too small */
}
```

`StrLenUtf8Scalar` is the byte-at-a-time baseline, kept so the SWAR path can
be benchmarked against it.

### Validation

Counting assumes well-formed input. Validate anything from outside the
process first:

```c
/* Validate with a known length (SIMD-optimized) */
bool StrUtf8Valid(const void *str, size_t len);
bool StrUtf8ValidCStr(const char *str);       /* NUL-terminated */

/* Scalar baselines, for benchmarking */
bool StrUtf8ValidScalar(const void *str, size_t len);
bool StrUtf8ValidCStrScalar(const char *str);

/* Validate and count in one pass */
size_t StrUtf8ValidCount(const void *str, size_t len, bool *valid);

/* Byte length of N codepoints, validating on the way */
size_t StrUtf8ValidCountBytes(const void *str, size_t len,
                              size_t numCodepoints, bool *valid);
```

### Encoding and Decoding

```c
/* Decode one sequence; advances *str and *remaining.
 * Returns the codepoint, or 0xFFFFFFFF on a malformed sequence. */
uint32_t StrUtf8Decode(const uint8_t **str, size_t *remaining);

/* Encode a codepoint into dst; returns 1-4 bytes written, or 0 if the
 * codepoint is not encodable. */
size_t StrUtf8Encode(uint8_t *dst, uint32_t codepoint);

/* How many bytes this codepoint needs (1-4, or 0 if invalid) */
size_t StrUtf8CodepointLen(uint32_t codepoint);

/* How many bytes this sequence occupies, from its first byte
 * (0 if the byte is not a valid sequence start) */
size_t StrUtf8SequenceLen(uint8_t firstByte);
```

### Cursor Movement

```c
const uint8_t *StrUtf8Advance(const void *str, size_t len, size_t n);
const uint8_t *StrUtf8Retreat(const void *str, size_t len,
                              const uint8_t *pos, size_t n);
uint32_t StrUtf8Peek(const void *str, size_t len, const uint8_t *pos);
size_t StrUtf8OffsetAt(const void *str, size_t len, size_t charIndex);
size_t StrUtf8IndexAt(const void *str, size_t len, size_t byteOffset);

/* Scalar baseline */
const uint8_t *StrUtf8AdvanceScalar(const void *str, size_t len, size_t n);
```

### Truncation and Substrings

```c
size_t StrUtf8Truncate(const void *str, size_t len, size_t maxChars);
size_t StrUtf8TruncateBytes(const void *str, size_t len, size_t maxBytes);
void StrUtf8Substring(const void *str, size_t len, size_t startChar,
                      size_t endChar, size_t *outOffset, size_t *outLen);
size_t StrUtf8SubstringCopy(const void *str, size_t len, size_t startChar,
                            size_t endChar, void *buf, size_t bufLen);
size_t StrUtf8Split(const void *str, size_t len, size_t charIndex);
```

`StrUtf8TruncateBytes` is the one to reach for when a byte budget is fixed
(a fixed-width column, a wire field): it backs up to the nearest sequence
boundary instead of splitting a codepoint.

Substring ranges are `[startChar, endChar)` — start inclusive, end exclusive.

### Comparison

```c
int StrUtf8Compare(const void *s1, size_t len1, const void *s2, size_t len2);
int StrUtf8CompareN(const void *s1, size_t len1, const void *s2, size_t len2,
                    size_t n);   /* first n codepoints */
int StrUtf8CompareCaseInsensitiveAscii(const void *s1, size_t len1,
                                       const void *s2, size_t len2);
bool StrUtf8StartsWith(const void *str, size_t strLen,
                       const void *prefix, size_t prefixLen);
bool StrUtf8EndsWith(const void *str, size_t strLen,
                     const void *suffix, size_t suffixLen);
bool StrUtf8Equal(const void *s1, size_t len1, const void *s2, size_t len2);
bool StrUtf8EqualCaseInsensitiveAscii(const void *s1, size_t len1,
                                      const void *s2, size_t len2);
```

Case-insensitive here means **ASCII** case folding only — no Unicode
case mapping, no locale awareness.

### Search

All search functions return a **byte offset**, or `SIZE_MAX` when not found.

```c
size_t StrUtf8Find(const void *haystack, size_t haystackLen,
                   const void *needle, size_t needleLen);
size_t StrUtf8FindLast(const void *haystack, size_t haystackLen,
                       const void *needle, size_t needleLen);
size_t StrUtf8FindChar(const void *str, size_t len, uint32_t codepoint);
size_t StrUtf8FindCharLast(const void *str, size_t len, uint32_t codepoint);
size_t StrUtf8FindCharNth(const void *str, size_t len, uint32_t codepoint,
                          size_t n);
bool StrUtf8Contains(const void *haystack, size_t haystackLen,
                     const void *needle, size_t needleLen);
size_t StrUtf8Count(const void *haystack, size_t haystackLen,
                    const void *needle, size_t needleLen);  /* non-overlapping */
size_t StrUtf8CountChar(const void *str, size_t len, uint32_t codepoint);

/* Character-set scanning, in the spirit of strspn / strcspn */
size_t StrUtf8FindAnyChar(const void *str, size_t len,
                          const void *charSet, size_t charSetLen);
size_t StrUtf8FindNotChar(const void *str, size_t len,
                          const void *charSet, size_t charSetLen);
size_t StrUtf8SpanChar(const void *str, size_t len,
                       const void *charSet, size_t charSetLen);
size_t StrUtf8SpanNotChar(const void *str, size_t len,
                          const void *charSet, size_t charSetLen);
```

### ASCII Case Conversion

```c
void StrAsciiToLower(void *str, size_t len);   /* in place */
void StrAsciiToUpper(void *str, size_t len);
size_t StrAsciiToLowerCopy(void *dst, size_t dstLen,
                           const void *src, size_t srcLen);
size_t StrAsciiToUpperCopy(void *dst, size_t dstLen,
                           const void *src, size_t srcLen);
bool StrAsciiIsLower(const void *str, size_t len);
bool StrAsciiIsUpper(const void *str, size_t len);
```

Non-ASCII bytes pass through untouched, so these are safe to run over UTF-8
without corrupting multibyte sequences.

### Unicode Character Properties

```c
/* 0 = zero-width, 1 = narrow, 2 = wide */
int StrUnicodeEastAsianWidth(uint32_t codepoint);

bool StrUnicodeIsLetter(uint32_t cp);
bool StrUnicodeIsDigit(uint32_t cp);
bool StrUnicodeIsSpace(uint32_t cp);
bool StrUnicodeIsAlnum(uint32_t cp);

int StrUnicodeGraphemeBreak(uint32_t cp);
bool StrUnicodeIsGraphemeBreak(uint32_t cp1, uint32_t cp2);
```

### Display Width

Terminal cells, not codepoints — East Asian wide characters count as two and
combining marks as zero.

```c
size_t StrUtf8Width(const void *str, size_t len);
size_t StrUtf8WidthN(const void *str, size_t len, size_t n);
size_t StrUtf8TruncateWidth(const void *str, size_t len, size_t maxWidth);
size_t StrUtf8IndexAtWidth(const void *str, size_t len, size_t targetWidth);
size_t StrUtf8WidthAt(const void *str, size_t len, size_t offset);
size_t StrUtf8WidthBetween(const void *str, size_t len,
                           size_t startOffset, size_t endOffset);
size_t StrUtf8PadWidth(const void *str, size_t len, size_t targetWidth);
bool StrUtf8IsNarrow(const void *str, size_t len);
bool StrUtf8HasWide(const void *str, size_t len);
```

`StrUtf8IsNarrow` is the fast path check: when it returns true, byte length,
codepoint count, and cell width all agree and the width functions can be
skipped entirely.

### Grapheme Clusters

A grapheme cluster is what a reader perceives as one character — a base
codepoint plus its combining marks, a regional-indicator pair, and so on.
This is the right unit for user-visible truncation.

```c
size_t StrUtf8GraphemeNext(const void *str, size_t len);
size_t StrUtf8GraphemeCount(const void *str, size_t len);
size_t StrUtf8GraphemeAdvance(const void *str, size_t len, size_t n);
bool StrUtf8GraphemeAt(const void *str, size_t len, size_t n,
                       size_t *startOut, size_t *endOut);
size_t StrUtf8GraphemeWidth(const void *str, size_t len);
size_t StrUtf8GraphemeTruncate(const void *str, size_t len, size_t n);
void StrUtf8GraphemeReverse(void *str, size_t len);   /* in place */
```

### Four Different Lengths

| Question                      | Function               |
| ----------------------------- | ---------------------- |
| How much memory?              | `len` (bytes)          |
| How many characters to index? | `StrLenUtf8`           |
| How many terminal cells?      | `StrUtf8Width`         |
| How many user-visible marks?  | `StrUtf8GraphemeCount` |

For `"é"` written as `e` plus a combining acute, those are 3, 2, 1, and 1.
Using the wrong one is the usual source of UTF-8 layout bugs.

## Digit Validation

File: `strDigitsVerify.c`

### Simple Loop

```c
/* Validate all bytes are ASCII digits */
bool StrIsDigitsIndividual(const void *buf, size_t size);
```

**Example:**

```c
assert(StrIsDigitsIndividual("123456", 6));
assert(!StrIsDigitsIndividual("12a456", 6));
```

### SIMD Optimized

```c
/* SIMD-optimized digit validation: SSE2 on x86, NEON on ARM64,
 * falling back to StrIsDigitsIndividual elsewhere and for the tail */
bool StrIsDigitsFast(const void *buf, size_t size);
```

**Implementation (x86 arm; the NEON arm is the same idea over 16-byte
vectors):**

```c
#if __SSE2__
bool StrIsDigitsFast(const void *buf, size_t size) {
    const __m128i ascii0 = _mm_set1_epi8('0');
    const __m128i ascii9 = _mm_set1_epi8('9');
    const __m128i *mover = (const __m128i *)buf;

    while (/* process 16 bytes */) {
        const __m128i v = _mm_loadu_si128(mover);

        /* Check: v < '0' || v > '9' */
        const __m128i lt0 = _mm_cmplt_epi8(v, ascii0);
        const __m128i gt9 = _mm_cmpgt_epi8(v, ascii9);
        const __m128i outside = _mm_or_si128(lt0, gt9);

        if (_mm_movemask_epi8(outside)) {
            return false;  /* Non-digit found */
        }

        mover++;
    }

    /* Check remaining < 16 bytes */
    return StrIsDigitsIndividual(...);
}
#endif
```

**Example:**

```c
const char *bigNumber = "12345678901234567890";

/* Fast validation */
if (StrIsDigitsFast(bigNumber, strlen(bigNumber))) {
    /* All digits - safe to convert */
    uint64_t value = StrBufToUInt64Fast(bigNumber, strlen(bigNumber));
}
```

### Performance

`StrIsDigitsFast` checks 16 bytes per iteration where SIMD is available and
delegates the sub-16-byte tail to `StrIsDigitsIndividual`. For short inputs
the two are effectively the same; the gap opens with length. Measure with
the `perf` CMake preset if the choice matters to you.

## Digit Counting

File: `strCountDigits.c`

### Count Digits in Integer

```c
/* How many decimal digits needed? */
size_t StrDigitCountUInt32(uint32_t v);
size_t StrDigitCountUInt64(uint64_t v);
size_t StrDigitCountUInt128(__uint128_t v);
size_t StrDigitCountInt64(int64_t v);  /* Includes minus sign */
```

**Implementation:**

```c
size_t StrDigitCountUInt64(uint64_t v) {
    if (v < 10) return 1;
    if (v < 100) return 2;
    if (v < 1000) return 3;
    /* ... binary search tree style ... */
    if (v < 1000000000000UL) {
        /* ... more comparisons ... */
    }
    return 12 + StrDigitCountUInt64(v / 1000000000000UL);
}
```

**Example:**

```c
size_t d1 = StrDigitCountUInt64(123);        /* 3 */
size_t d2 = StrDigitCountUInt64(UINT64_MAX); /* 20 */
size_t d3 = StrDigitCountInt64(-999);        /* 4 (includes '-') */

/* Pre-allocate exact buffer size */
char buf[StrDigitCountUInt64(value) + 1];
StrUInt64ToBuf(buf, sizeof(buf), value);
```

## Population Count (Popcount)

File: `strPopcnt.c`

Count set bits in memory with optimized implementations.

### Lookup Table Method

```c
/* Count using 8-bit lookup table */
uint64_t StrPopCnt8Bit(const void *data, size_t len);
```

### Aligned POPCNT

```c
/* POPCNT instruction with manual dependency breaking */
uint64_t StrPopCntAligned(const void *data, size_t len);
```

**Implementation breaks false dependencies:**

```c
#if __x86_64__
/* Use separate accumulators to avoid CPU pipeline stalls */
uint64_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;

while (len >= 32) {
    __asm__("popcnt %4, %4  \n\t"
            "add %4, %0     \n\t"
            "popcnt %5, %5  \n\t"
            "add %5, %1     \n\t"
            "popcnt %6, %6  \n\t"
            "add %6, %2     \n\t"
            "popcnt %7, %7  \n\t"
            "add %7, %3     \n\t"
            : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3)
            : "r"(v[0]), "r"(v[1]), "r"(v[2]), "r"(v[3]));
    v += 4;
}

return c0 + c1 + c2 + c3;
#endif
```

This avoids the false output dependency `popcnt` has on some Intel CPUs, which
otherwise serializes back-to-back `popcnt` instructions. The gain has not been
measured here (it needs an affected x86-64 host).

### Exact POPCNT

```c
/* For exactly aligned 32-byte chunks */
uint64_t StrPopCntExact(const void *data, size_t len);
```

**Example:**

```c
/* Count set bits in bitmap */
uint64_t bitmap[1024];  /* 64 KB */
/* ... populate bitmap ... */

uint64_t setBits = StrPopCntAligned(bitmap, sizeof(bitmap));
printf("Set bits: %" PRIu64 "\n", setBits);

/* StrPopCntExact expects exact 32-byte chunks */
uint64_t count = StrPopCntExact(bitmap, sizeof(bitmap));
```

### Why the Hand-Written Assembly

On several Intel generations the `POPCNT` instruction carries a false
dependency on its destination register: the result does not actually depend
on the previous value, but the scheduler treats it as if it does, serializing
a chain of popcounts. Using four independent accumulators breaks that chain
so the counts issue in parallel. That is the whole reason this file contains
inline assembly instead of `__builtin_popcountll` in a loop.

`StrPopCntScalar` is the plain baseline, kept for benchmarking.

## Bitmap Position Extraction

File: `strBitmapGetSetPositionsExact.c`

Extract positions of set/unset bits from bitmaps.

### Get Set Bit Positions

```c
/* Extract positions of 1 bits */
uint32_t StrBitmapGetSetPositionsExact8(const void *data, size_t len,
                                       uint8_t position[]);
uint32_t StrBitmapGetSetPositionsExact16(const void *data, size_t len,
                                        uint16_t position[]);
uint32_t StrBitmapGetSetPositionsExact32(const void *data, size_t len,
                                        uint32_t position[]);
uint64_t StrBitmapGetSetPositionsExact64(const void *data, size_t len,
                                        uint64_t position[]);
```

### Get Unset Bit Positions

```c
/* Extract positions of 0 bits */
uint32_t StrBitmapGetUnsetPositionsExact8(const void *data, size_t len,
                                         uint8_t position[]);
/* ... similar for 16, 32, 64 ... */
```

**Example:**

```c
/* Bitmap: 0b11000000110000001100000011000000... (repeated) */
const uint64_t bitmap = 0xc0c0c0c0c0c0c0c0ULL;

uint8_t positions[64];
uint32_t count = StrBitmapGetSetPositionsExact8(&bitmap, sizeof(bitmap),
                                                positions);

/* positions[] = {6, 7, 14, 15, 22, 23, 30, 31, 38, 39, ...} */
printf("Found %u set bits\n", count);

for (uint32_t i = 0; i < count; i++) {
    printf("Bit %u is set\n", positions[i]);
}
```

### Use Case: Sparse Bitmap Iteration

```c
/* Iterate only over set positions in large bitmap */
void processSparseData(const uint64_t *bitmap, size_t bitmapWords) {
    /* Worst case is every bit set: 64 positions per word.
     * Heap-allocate — a VLA here is a stack overflow waiting to happen. */
    const size_t bytes = bitmapWords * sizeof(uint64_t);
    uint32_t *positions = zmalloc(bytes * 8 * sizeof(uint32_t));

    const uint32_t count =
        StrBitmapGetSetPositionsExact32(bitmap, bytes, positions);

    /* Process only set positions */
    for (uint32_t i = 0; i < count; i++) {
        processItem(positions[i]);
    }

    zfree(positions);
}
```

## Fast Random Number Generators

File: `strRandom.c`

Non-cryptographic PRNGs for simulations and testing.

### XorShift Variants

```c
/* XorShift128 - 128-bit state */
void xorshift128(uint32_t *x, uint32_t *y, uint32_t *z, uint32_t *w);

/* XorShift64* - 64-bit state with multiplier */
uint64_t xorshift64star(uint64_t *x);

/* XorShift1024* - 1024-bit state, longest period */
uint64_t xorshift1024star(uint64_t s[16], uint8_t *sIndex);

/* XorShift128+ - 128-bit state, fast */
uint64_t xorshift128plus(uint64_t s[2]);

/* Xoroshiro128+ - Improved XorShift128+ */
uint64_t xoroshiro128plus(uint64_t s[2]);

/* SplitMix64 - Simple, fast */
uint64_t splitmix64(uint64_t *x);
```

**Example:**

```c
/* Initialize */
uint64_t state[2] = {12345, 67890};

/* Generate random numbers */
for (int i = 0; i < 1000; i++) {
    uint64_t random = xoroshiro128plus(state);
    printf("Random: %" PRIu64 "\n", random);
}

/* For seeding other generators */
uint64_t seed = time(NULL);
for (int i = 0; i < 16; i++) {
    uint64_t state1024[16];
    state1024[i] = splitmix64(&seed);
}
```

### Performance

| Generator        | Speed   | Period   | Quality          |
| ---------------- | ------- | -------- | ---------------- |
| splitmix64       | Fastest | 2^64     | Good for seeding |
| xorshift64star   | Fast    | 2^64-1   | Good             |
| xorshift128plus  | Fast    | 2^128-1  | Good             |
| xoroshiro128plus | Fast    | 2^128-1  | Better           |
| xorshift1024star | Slower  | 2^1024-1 | Excellent        |

**Note:** None are cryptographically secure. Use for simulations, testing, and non-security applications only.

## Power Functions

File: `strPow.c`

### Power of 10

```c
/* 10^exp for 64-bit */
uint64_t StrTenPow(size_t exp);

/* 10^exp for 128-bit */
__uint128_t StrTenPowBig(size_t exp);
```

**Example:**

```c
uint64_t thousand = StrTenPow(3);      /* 1000 */
uint64_t million = StrTenPow(6);       /* 1000000 */
__uint128_t huge = StrTenPowBig(30);   /* 10^30 */
```

## SQLite Helpers

Files: `strSqliteLog.c`, `strSqliteNumeric.c`, `strSqliteStr.c`

Functions adapted from SQLite for database-style operations (covered in [STR.md](STR.md)).

## Patterns, Human Units and Random Ids (`util.h`)

**Glob matching**: `stringmatch(pattern, string, nocase)` and
`stringmatchlen(pattern, patternLen, string, stringLen, nocase)` (lengths
given, no NUL needed) match a glob: `*` any run, `?` one byte, `[...]` a
class (`^` negates it, `a-z` a range, `\` escapes), `\x` a literal `x`.
Nonzero `nocase` folds ASCII case. A pattern whose stars nest past 1000 deep
matches nothing, and runs of stars cost polynomial, not exponential, time.

```c
if (stringmatch("user:*:session", key, 0)) { /* ... */ }
```

**Human units**:

- `humanToBytes(text, len, &err)` reads `"512"`, `"64kb"`, `"1.5g"`,
  `"2gib"`: k/kb, m/mb, g/gb, t/tb, p/pb, e/eb are powers of 1000; kib
  through eib powers of 1024; no unit is bytes. It returns 0 with `err` true
  for a missing number, an unknown unit, or a result past `UINT64_MAX`.
- `bytesToHuman(buf, len, n)` writes `"512 B"`, or five decimals in the
  largest binary unit that keeps the value at least 1 (`"1.50000 GiB"`).
- `secondsToHuman(buf, len, sec)` writes four decimals in ns, us, ms,
  seconds, minutes, hours or days, whichever fits.

**Paths**: `pathIsBaseName(path)` is true for a name with no `/` or `\`.

**Random ids** (identifiers and seeds, not key material; for secrets see
[Random numbers](../utils/RANDOM.md)):

- `getRandomBytes(p, len)` fills from the OS source, falling back to a
  predictable time and pid mix only when that source fails;
  `getRandomHexChars(p, len)` writes `len` lowercase hex characters.
- `getRandomHexCharsCounterInit(seed, SHA1_DIGEST_LENGTH)` draws a seed once;
  `getRandomHexCharsCounter(seed, &counter, p, len)` then writes `len` hex
  characters per call by hashing the seed with a counter: cheap, never
  repeating for one seed, and not secret.

## Real-World Examples

### Example 1: Fast Batch Integer Conversion

```c
/* Convert array of integers to CSV */
void intArrayToCSV(const uint64_t *values, size_t count, FILE *output) {
    char buf[21];  /* Max 20 digits + null */

    for (size_t i = 0; i < count; i++) {
        size_t len = StrUInt64ToBuf(buf, sizeof(buf), values[i]);
        fwrite(buf, 1, len, output);

        if (i < count - 1) {
            fwrite(",", 1, 1, output);
        }
    }
}
```

### Example 2: Validate and Parse Numeric String

```c
bool parseNumericString(const char *str, size_t len, uint64_t *result) {
    /* Quick validation */
    if (!StrIsDigitsFast(str, len)) {
        return false;
    }

    /* Convert */
    return StrBufToUInt64(str, len, result);
}
```

### Example 3: UTF-8 Safe Truncation

```c
char *truncateUtf8Safe(const char *text, size_t maxChars) {
    size_t len = strlen(text);
    size_t chars = StrLenUtf8(text, len);

    if (chars <= maxChars) {
        /* datakit deprecates strdup(); copy through zmalloc */
        char *copy = zmalloc(len + 1);
        memcpy(copy, text, len + 1);
        return copy;
    }

    size_t bytes = StrLenUtf8CountBytes(text, len, maxChars);
    char *result = zmalloc(bytes + 4);  /* +3 for "..." +1 for NUL */
    memcpy(result, text, bytes);
    memcpy(result + bytes, "...", 4);   /* copies the NUL too */

    return result;
}
```

Truncating by **codepoints** can still split a grapheme cluster and leave a
dangling combining mark. When the result is shown to a person, use
`StrUtf8GraphemeTruncate` instead; when it has to fit a fixed-width display,
use `StrUtf8TruncateWidth`.

### Example 4: Count Matching Items in Bitmap

```c
size_t countMatching(const uint64_t *bitmap, size_t bitmapWords,
                     const uint32_t *items, size_t itemCount) {
    size_t matches = 0;

    /* Extract all set positions */
    uint32_t *positions = zmalloc(bitmapWords * 64 * sizeof(uint32_t));
    uint32_t setPosCount = StrBitmapGetSetPositionsExact32(
        bitmap, bitmapWords * 8, positions);

    /* Count matches */
    for (size_t i = 0; i < itemCount; i++) {
        for (uint32_t j = 0; j < setPosCount; j++) {
            if (items[i] == positions[j]) {
                matches++;
                break;
            }
        }
    }

    zfree(positions);
    return matches;
}
```

## Thread Safety

All functions in str/ are **fully thread-safe** as they:

- Operate on caller-provided buffers
- Have no global mutable state
- Use only stack or heap allocations

The RNG functions modify state passed by pointer, so the caller must protect concurrent access.

## Benchmarking

Every accelerated routine here ships with a scalar twin so the two can be
compared directly on the machine that matters:

| Fast path            | Scalar baseline          |
| -------------------- | ------------------------ |
| `StrIsDigitsFast`    | `StrIsDigitsIndividual`  |
| `StrLenUtf8`         | `StrLenUtf8Scalar`       |
| `StrUtf8Valid`       | `StrUtf8ValidScalar`     |
| `StrUtf8ValidCStr`   | `StrUtf8ValidCStrScalar` |
| `StrUtf8Advance`     | `StrUtf8AdvanceScalar`   |
| `StrBufToUInt64Fast` | `StrBufToUInt64Scalar`   |
| `StrPopCntAligned`   | `StrPopCntScalar`        |
| `StrUInt64ToBuf`     | `StrUInt64ToBufTable`    |

Build benchmarks with the `perf` CMake preset. Sanitizer builds add several
times the overhead and will make the scalar and SIMD paths look identical.

## See Also

- [STR](STR.md) - Core string utilities
- [DKS](DKS.md) - Dynamic string buffers
- [STR_DOUBLE_FORMAT](STR_DOUBLE_FORMAT.md) - Double formatting

## Testing

All utilities are tested as part of the str test suite:

```bash
./src/datakit-test test str
```
