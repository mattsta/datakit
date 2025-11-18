# Extended String Utilities

## Overview

The `str/` directory contains **specialized high-performance string utilities** optimized for specific operations. These include fast integer conversions, UTF-8 processing, bit manipulation, and numeric validation.

**Key Features:**
- SIMD-optimized digit validation (SSE2)
- SWAR-based fast integer-to-string conversion
- Optimized UTF-8 character counting
- Population count (popcount) with manual dependency breaking
- Bitmap position extraction
- Fast random number generators

**Location**: `/src/str/` directory

**Included by**: `str.c` (automatically includes all str/*.c files)

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

Typical performance on modern CPUs:
- `StrUInt64ToBuf`: ~60 cycles
- `StrUInt128ToBuf`: ~100-200 cycles
- ~3-5x faster than `snprintf`

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
    printf("Value: %lu\n", checked);
}

/* Validates overflow but not digit characters */
uint64_t safe;
if (StrBufToUInt64FastCheckOverflow(digits, 9, &safe)) {
    printf("Safe: %lu\n", safe);
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

### Performance

Typical throughput:
- ASCII text: ~10-15 GB/s (modern CPU)
- UTF-8 text: ~8-12 GB/s
- ~5-10x faster than byte-by-byte iteration

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

### SSE2 Optimized (4x faster)

```c
/* SIMD-optimized digit validation */
bool StrIsDigitsFast(const void *buf, size_t size);
```

**Implementation:**
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

| Method | Speed | Notes |
|--------|-------|-------|
| Individual | 1.0x | Baseline |
| SSE2 | 4.0x | 16 bytes per iteration |

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

This avoids false register dependencies in Intel CPUs, providing **2x speedup**.

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
printf("Set bits: %lu\n", setBits);

/* For cache-line aligned data */
uint64_t *aligned = aligned_alloc(32, 1024);
uint64_t count = StrPopCntExact(aligned, 1024);
```

### Performance

| Method | Speed | Notes |
|--------|-------|-------|
| 8-bit lookup | 1.0x | No POPCNT instruction |
| Aligned (compiler) | 2.0x | POPCNT with false dependencies |
| Aligned (asm) | 4.0x | Manual dependency breaking |

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
void processSparseData(const uint64_t *bitmap, size_t bitmapSize) {
    uint32_t positions[8 * bitmapSize];

    uint32_t count = StrBitmapGetSetPositionsExact32(
        bitmap, bitmapSize * sizeof(uint64_t), positions);

    /* Process only set positions */
    for (uint32_t i = 0; i < count; i++) {
        processItem(positions[i]);
    }
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
    printf("Random: %lu\n", random);
}

/* For seeding other generators */
uint64_t seed = time(NULL);
for (int i = 0; i < 16; i++) {
    uint64_t state1024[16];
    state1024[i] = splitmix64(&seed);
}
```

### Performance

| Generator | Speed | Period | Quality |
|-----------|-------|--------|---------|
| splitmix64 | Fastest | 2^64 | Good for seeding |
| xorshift64star | Fast | 2^64-1 | Good |
| xorshift128plus | Fast | 2^128-1 | Good |
| xoroshiro128plus | Fast | 2^128-1 | Better |
| xorshift1024star | Slower | 2^1024-1 | Excellent |

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
        return strdup(text);
    }

    size_t bytes = StrLenUtf8CountBytes(text, len, maxChars);
    char *result = malloc(bytes + 4);  /* +3 for "..." */
    memcpy(result, text, bytes);
    strcpy(result + bytes, "...");

    return result;
}
```

### Example 4: Count Matching Items in Bitmap

```c
size_t countMatching(const uint64_t *bitmap, size_t bitmapWords,
                     const uint32_t *items, size_t itemCount) {
    size_t matches = 0;

    /* Extract all set positions */
    uint32_t *positions = malloc(bitmapWords * 64 * sizeof(uint32_t));
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

    free(positions);
    return matches;
}
```

## Thread Safety

All functions in str/ are **fully thread-safe** as they:
- Operate on caller-provided buffers
- Have no global mutable state
- Use only stack or heap allocations

The RNG functions modify state passed by pointer, so the caller must protect concurrent access.

## Performance Summary

| Operation | Throughput | vs. Standard |
|-----------|-----------|--------------|
| StrUInt64ToBuf | ~60 cycles | 3-5x faster than snprintf |
| StrBufToUInt64Fast | ~20 cycles | 2-3x faster than atoi |
| StrIsDigitsFast | 10-15 GB/s | 4x faster than loop |
| StrLenUtf8 | 8-12 GB/s | 5-10x faster than byte loop |
| StrPopCntAligned | ~4 GB/s | 2x faster than compiler builtin |

## See Also

- [STR](STR.md) - Core string utilities
- [DKS](DKS.md) - Dynamic string buffers
- [STR_DOUBLE_FORMAT](STR_DOUBLE_FORMAT.md) - Double formatting

## Testing

All utilities are tested as part of the str test suite:

```bash
./src/datakit-test test str
```
