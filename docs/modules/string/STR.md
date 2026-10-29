# str - Core String Utilities

## Overview

`str` provides **high-performance string manipulation and conversion utilities** extracted and optimized from SQLite and LuaJIT. It offers fast, reliable conversions between strings and native types with comprehensive UTF-8 support.

**Key Features:**

- Fast string-to-number conversions with overflow detection
- Reliable round-trip number parsing (converts back to exact same string)
- UTF-8 character counting and manipulation
- Bloom filters for fast string membership testing
- Case-insensitive string comparison
- Logarithmic estimation for query planning

**Header**: `str.h`

**Source**: `str.c`, which textually `#include`s every `src/str/*.c` file so
the whole set compiles as one translation unit.

**Scope**: this file is the API reference for `str.h`.
[STR_UTILS](STR_UTILS.md) is the implementation tour of `src/str/` — the SWAR
converters, the SIMD paths, the full UTF-8 family, popcount, bitmap position
extraction, and the random generators.

## Core Character Operations

### Fast Character Classification

```c
/* Lookup tables for ASCII character classification */
extern const char StrIdChar[];
extern const unsigned char StrCtypeMap[256];
extern const unsigned char StrUpperToLower[];

/* Fast character classification macros (ASCII only) */
#define StrToupper(x)   /* Convert to uppercase */
#define StrTolower(x)   /* Convert to lowercase */
#define StrIsspace(x)   /* Is whitespace */
#define StrIsalnum(x)   /* Is alphanumeric */
#define StrIsalpha(x)   /* Is alphabetic */
#define StrIsdigit(x)   /* Is digit */
#define StrIsxdigit(x)  /* Is hex digit */
#define StrIsquote(x)   /* Is quote character */

/* Example usage */
char c = 'a';
if (StrIsalpha(c)) {
    char upper = StrToupper(c);  /* 'A' */
}
```

### String Comparison

```c
/* Case-insensitive comparison */
int StrICmp(const char *zLeft, const char *zRight);
int StrNICmp(const char *zLeft, const char *zRight, int N);

/* Example usage */
if (StrICmp("Hello", "HELLO") == 0) {
    printf("Strings are equal (case-insensitive)\n");
}

if (StrNICmp("Hello", "HELP", 3) == 0) {
    printf("First 3 characters match\n");
}
```

## String to Native Type Conversions

### String to Integer (SQLite-based)

```c
/* String encoding types */
typedef enum strEnc {
    STR_INVALID = 0,
    STR_UTF8,
    STR_UTF16LE,
    STR_UTF16BE,
} strEnc;

/* Convert string to 64-bit integer: STR_INT64_FITS, STR_INT64_REFUSED
 * (empty, malformed, out of range) or STR_INT64_JUST_PAST_MAX
 * (9223372036854775808, which fits only negated) */
strInt64Read StrAtoi64(const void *zNum, int64_t *pNum, int32_t length,
                       strEnc enc, bool skipSpaces);

/* Example usage */
int64_t value;
const char *str = "12345";
if (StrAtoi64(str, &value, strlen(str), STR_UTF8, false) == STR_INT64_FITS) {
    printf("Converted: %" PRId64 "\n", value);  /* 12345 */
}
```

### Other Integer Readers, Hex, Checked Arithmetic

| Call                                               | Does                                                                                             |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------ | --- | ------------------------------------ |
| `StrDecOrHexToInt64(z, &v)`                        | decimal or `0x` hex (zero-terminated) to `int64_t`; hex is the bits (`0xffffffffffffffff` is -1) |
| `StrGetInt32(z, &v)`                               | decimal or `0x` hex to `int32_t`; true when it fits (text after the number is ignored)           |
| `StrAtoi(z)`                                       | the same value, or 0 when it does not fit                                                        |
| `StrHexToInt(c)`                                   | one hex digit's value (`0-9`, `a-f`, `A-F` only)                                                 |
| `StrHexToBytes(hex, hexLen, out, outLen)`          | exactly `2 * outLen` hex digits to bytes; false on a wrong length or non-digit                   |
| `StrHexToBlob(z, n)`                               | `n` hex digits to a zero-terminated `zmalloc` buffer (the caller `zfree`s)                       |
| `StrBytesToHex(in, n, out)`                        | `n` bytes to `2n` lowercase digits and a zero (`out` holds `2n + 1`)                             |
| `StrAddInt64(&a, b)`, `StrSubInt64`, `StrMulInt64` | `a op= b` when the result fits `int64_t`; false, `a` unchanged, on overflow                      |
| `StrAbsInt32(x)`                                   | `                                                                                                | x   | `, with `INT32_MIN`giving`INT32_MAX` |
| `StrDequote(z)`                                    | removes SQL-style quotes in place: `'a''b'` to `a'b`; `"x"`, `` `x` ``, `[x]` alike              |

```c
uint8_t key[32];
if (!StrHexToBytes(argHex, strlen(argHex), key, sizeof(key))) {
    return false; /* not 64 hex digits */
}

char hex[2 * sizeof(key) + 1];
StrBytesToHex(key, sizeof(key), hex);

int64_t total = 0;
for (size_t i = 0; i < n; i++) {
    if (!StrAddInt64(&total, amounts[i])) {
        return false; /* the sum left int64_t */
    }
}
```

### String to Double (SQLite-based)

```c
/* Convert string to double */
bool StrAtoF(const void *z, double *pResult, int32_t length,
             strEnc enc, bool skipSpaces);

/* Example usage */
double value;
const char *str = "3.14159";
if (StrAtoF(str, &value, strlen(str), STR_UTF8, false)) {
    printf("Converted: %f\n", value);  /* 3.141590 */
}
```

### Reliable Round-Trip Conversion

```c
/* Convert string to native type with guaranteed round-trip accuracy */
bool StrScanScanReliable(const void *p, const size_t len, databox *box);

/* Example usage */
databox result;
const char *str = "299.5";

if (StrScanScanReliable(str, strlen(str), &result)) {
    /* Result is stored in optimal type (float32 in this case) */
    if (result.type == DATABOX_FLOAT_32) {
        printf("Value: %f\n", result.data.f32);  /* 299.5 */

        /* Converting back produces EXACT same string */
        char buf[64];
        size_t len = StrDoubleFormatToBufNice(buf, sizeof(buf), result.data.f32);
        assert(memcmp(buf, str, strlen(str)) == 0);
    }
}
```

The reliable scanner guarantees:

- No leading zeros (except "0" or "0.xxx")
- No leading decimal points (".123" fails)
- No trailing zeros after decimal (except "X.0")
- Exact round-trip: converting back produces identical string
- Automatic type selection (int64, uint64, float, double)

### 128-bit Integer Support

```c
/* Convert string to 128-bit integer.
 * 'small' receives the value when it fits in 64 bits, 'box' when it does
 * not; '*use' is set to whichever one holds the result. */
bool StrScanScanReliableConvert128(const void *p, const size_t len,
                                   databox *small, databoxBig *box,
                                   databoxBig **use);

/* Same, for input already known to be a valid numeral — skips
 * re-validating the character content. */
bool StrScanScanReliableConvert128PreVerified(const void *p, const size_t len,
                                              databox *small, databoxBig *box,
                                              databoxBig **use);

/* Example usage */
const char *huge = "170141183460469231731687303715884105727";
databox sm = {{0}};
databoxBig bigbox = {{0}};
databoxBig *result = NULL;

if (StrScanScanReliableConvert128(huge, strlen(huge), &sm, &bigbox, &result)) {
    if (result->type == DATABOX_SIGNED_128) {
        printf("128-bit value stored\n");
    }
}
```

The two-box signature exists because a plain `databox` cannot hold a 128-bit
value inline — its `data.i128` is a pointer. Always read the result through
`*use`, never from whichever box you guessed.

### LuaJIT-style Scanning

```c
/* Format types returned by scanner */
typedef enum StrScanFmt {
    STRSCAN_ERROR,
    STRSCAN_NUM,     /* General number */
    STRSCAN_IMAG,    /* Imaginary number */
    STRSCAN_INT,     /* Integer */
    STRSCAN_U32,     /* uint32_t */
    STRSCAN_I64,     /* int64_t */
    STRSCAN_U64,     /* uint64_t */
} StrScanFmt;

/* Scan options */
typedef enum StrScanOpt {
    STRSCAN_OPT_TOINT = 0x01,  /* Convert to int32_t if possible */
    STRSCAN_OPT_TONUM = 0x02,  /* Always convert to double */
    STRSCAN_OPT_IMAG = 0x04,   /* Allow imaginary numbers */
    STRSCAN_OPT_LL = 0x08,     /* Allow 64-bit integer suffixes */
    STRSCAN_OPT_C = 0x10,      /* C-style number parsing */
} StrScanOpt;

/* Scan string to databox */
StrScanFmt StrScanScan(const uint8_t *p, databox *box, StrScanOpt opt,
                       bool allowFloatWords, bool skipSpaces);

/* Convert to double only */
bool StrScanToDouble(const void *str, databox *box,
                     bool allowFloatWords, bool skipSpaces);

/* Example usage */
databox result;
const char *str = "42";
StrScanFmt fmt = StrScanScan((const uint8_t *)str, &result,
                              STRSCAN_OPT_TOINT, false, false);
if (fmt == STRSCAN_INT) {
    printf("Integer: %" PRId64 "\n", result.data.i64);
}
```

## Native Types to String Conversions

### Integer to String

```c
/* Convert integers to string */
size_t StrInt64ToBuf(void *dst, size_t dstLen, int64_t value);
size_t StrUInt32ToBuf(void *dst, size_t dstLen, uint32_t value);
size_t StrUInt64ToBuf(void *dst, size_t dstLen, uint64_t value);

/* 128-bit integers */
size_t StrUInt128ToBuf(void *dst, size_t dstLen, __uint128_t value);
size_t StrInt128ToBuf(void *dst, size_t dstLen, __int128_t value);

/* Example usage */
char buf[32];
size_t len;

/* Convert signed integer */
len = StrInt64ToBuf(buf, sizeof(buf), -12345);
printf("Result: %.*s\n", (int)len, buf);  /* -12345 */

/* Convert unsigned integer */
len = StrUInt64ToBuf(buf, sizeof(buf), UINT64_MAX);
printf("Result: %.*s\n", (int)len, buf);  /* 18446744073709551615 */

/* Convert 128-bit integer */
__uint128_t huge = ((__uint128_t)1 << 127) - 1;
len = StrUInt128ToBuf(buf, sizeof(buf), huge);
printf("Result: %.*s\n", (int)len, buf);
```

### Table-based Integer Conversion

```c
/* Alternative table-based conversion (may be faster on some platforms) */
size_t StrUInt64ToBufTable(void *dst, size_t dstLen, uint64_t value);
size_t StrInt64ToBufTable(void *dst, size_t dstLen, int64_t value);
```

### Fast Multi-Digit Conversion

```c
/* Fixed-width digit writers.  These write EXACTLY that many digits,
 * zero-padded, with no NUL and no length return — the caller must
 * guarantee the buffer space and know the value fits. */
void StrUInt9DigitsToBuf(void *p, uint32_t u);   /* needs 9 bytes */
void StrUInt8DigitsToBuf(void *p, uint32_t u);   /* needs 8 bytes */
void StrUInt4DigitsToBuf(void *p, uint32_t u);   /* needs 4 bytes */
```

See [STR_UTILS](STR_UTILS.md#fast-integer-to-string-conversion) for how the
SWAR digit generation works and when the table variant is preferable.

### String to Integer (Fast)

```c
/* Fast conversions with various safety levels */

/* Fastest - no checks, must be all digits */
uint64_t StrBufToUInt64Fast(const void *buf, size_t len);

/* With numeric validation: false on a non-digit or a value above
 * UINT64_MAX; leading zeros are allowed */
bool StrBufToUInt64FastCheckNumeric(const void *buf, size_t len,
                                    uint64_t *value);

/* With overflow checking only (bytes must be digits): false, with
 * *value = UINT64_MAX, for a value above UINT64_MAX */
bool StrBufToUInt64FastCheckOverflow(const void *buf, size_t len,
                                     uint64_t *value);

/* Full validation with error checking */
bool StrBufToInt64(const void *s, size_t slen, int64_t *value);
bool StrBufToUInt64(const void *s, size_t slen, uint64_t *value);

/* 128-bit conversions */
bool StrBufToUInt128(const void *buf, size_t bufLen, __uint128_t *value);
bool StrBufToInt128(const void *buf, size_t bufLen, __int128_t *value);

/* Example usage */
const char *str = "12345";
uint64_t fast = StrBufToUInt64Fast(str, 5);  /* Fastest, no checks */

uint64_t checked;
if (StrBufToUInt64FastCheckNumeric(str, 5, &checked)) {
    printf("Valid number: %" PRIu64 "\n", checked);
}

int64_t safe;
if (StrBufToInt64(str, 5, &safe)) {
    printf("Safely converted: %" PRId64 "\n", safe);
}
```

## Digit Counting and Validation

### Count Digits in Number

```c
/* Count decimal digits required to represent a number */
size_t StrDigitCountUInt32(uint32_t v);
size_t StrDigitCountInt64(int64_t v);
size_t StrDigitCountUInt64(uint64_t v);
size_t StrDigitCountUInt128(__uint128_t v);

/* Example usage */
size_t digits = StrDigitCountUInt64(12345);  /* 5 */
size_t neg_digits = StrDigitCountInt64(-999);  /* 4 (includes minus sign) */
```

### Verify All Digits

```c
/* Check if buffer contains only ASCII digits [0-9] */
bool StrIsDigitsIndividual(const void *buf, size_t size);  /* Simple loop */
bool StrIsDigitsFast(const void *buf, size_t size);        /* SSE2 / NEON */

/* Example usage */
const char *digits = "123456789";
if (StrIsDigitsFast(digits, strlen(digits))) {
    printf("All digits!\n");
}

const char *mixed = "123abc";
if (!StrIsDigitsFast(mixed, strlen(mixed))) {
    printf("Contains non-digits\n");
}
```

## UTF-8 Support

`str.h` carries a full UTF-8 layer: counting, validation, encode/decode,
cursor movement, truncation and substrings, comparison, search, ASCII case
conversion, Unicode character properties, terminal display width, and
grapheme clusters. The complete listing lives in
[STR_UTILS](STR_UTILS.md#utf-8-operations); the entry points most callers
need are:

```c
/* Codepoint count */
size_t StrLenUtf8(const void *ss, size_t len);

/* Byte count for the first N codepoints */
size_t StrLenUtf8CountBytes(const void *ss, size_t len,
                            size_t countCharacters);

/* Validate before trusting anything from outside this process */
bool StrUtf8Valid(const void *str, size_t len);
```

```c
/* Example usage */
const char *str = "Hello 💛";  /* 6 ASCII + 1 emoji (4 bytes) = 10 bytes */

printf("Bytes: %zu\n", strlen(str));                 /* 10 */
printf("Codepoints: %zu\n", StrLenUtf8(str, 10));    /* 7  */
printf("Cells: %zu\n", StrUtf8Width(str, 10));       /* 8  */
```

The counting functions assume well-formed input — run `StrUtf8Valid` first
on anything untrusted, or use `StrUtf8ValidCount` to validate and count in
one pass.

The `mds` / `mdsc` string types wrap all of these so the length argument
comes from the string's own header; see [DKS](DKS.md#utf-8-support).

## Logarithmic Estimation

Used for query planning and statistical estimation:

```c
/* LogEst: 16-bit logarithmic representation */
typedef int16_t LogEst;

/* Operations */
LogEst StrLogEst(uint64_t x);                /* Convert integer to LogEst */
LogEst StrLogEstFromDouble(double x);        /* Convert double to LogEst */
LogEst StrLogEstAdd(LogEst a, LogEst b);    /* Add two LogEsts */
uint64_t StrLogEstToInt(LogEst x);          /* Convert back to integer */

/* Example usage - query planning */
uint64_t rowCount = 1000000;
LogEst logRows = StrLogEst(rowCount);  /* Store as 10*log2(1000000) */

/* Later, retrieve estimate */
uint64_t estimated = StrLogEstToInt(logRows);
printf("Estimated rows: %" PRIu64 " (actual: %" PRIu64 ")\n",
       estimated, rowCount);
```

LogEst stores quantities as `10 * log2(X)`, giving a range from ~1e-986 to ~1e986:

- 1 → 0
- 10 → 33
- 100 → 66
- 1000 → 99
- 1000000 → 199

## Bloom Filters

Fast probabilistic set membership testing:

```c
/* Fixed-width Bloom filters */
typedef uintptr_t StrBloomFilter;           /* 64-bit on 64-bit systems */
typedef __uint128_t StrBloomFilterBig;      /* 128-bit */

/* Operations */
#define StrBloomSet(b, x)    /* Add element to filter */
#define StrBloomTest(b, x)   /* Test if element might be in set */

/* Big filter operations */
#define StrBloomBigSet(b, x)   /* 128-bit filter */
#define StrBloomBigTest(b, x)  /* 128-bit filter */

/* Example usage */
StrBloomFilter filter = 0;

/* Add elements */
StrBloomSet(filter, 42);
StrBloomSet(filter, 100);
StrBloomSet(filter, 255);

/* Test membership */
if (StrBloomTest(filter, 42)) {
    /* Might be in set (or false positive) */
    /* Do expensive lookup */
}

if (!StrBloomTest(filter, 99)) {
    /* Definitely NOT in set */
    /* Skip expensive lookup */
}
```

**False Positive Rates (64-bit filter):**

- 8 elements: 11%
- 16 elements: 22%
- 32 elements: 39%
- 64 elements: 63%

**False Positive Rates (128-bit filter):**

- 8 elements: 6%
- 16 elements: 11%
- 32 elements: 22%
- 64 elements: 39%

Rates are `1 - e^(-elements/bits)`: this is a single-hash filter with one bit
per element, so it degrades quickly. It is a "definitely not present" fast
path, nothing more — always confirm a positive with a real lookup.

**Caveat on the 128-bit variant.** `StrBloomFilterBig` is a `__uint128_t`,
but `_StrBloomBigBit` builds its bit from a 64-bit `StrBloomFilter` shifted
by a mask of up to 127. Shifts of 64 or more on a 64-bit type are undefined,
so bit selection for the upper half is not reliable. Prefer the 64-bit
filter until that is fixed.

## Double Conversion Helpers

```c
/* True only when the double is EXACTLY an int64 value:
 * the real->int->real round trip is a no-op, and the result is
 * neither INT64_MIN nor INT64_MAX. */
bool StrDoubleCanBeCastToInt64(double r);

/* Saturating TRUNCATION (not rounding):
 *   r <= (double)INT64_MIN -> INT64_MIN
 *   r >= (double)INT64_MAX -> INT64_MAX
 *   otherwise              -> (int64_t)r, toward zero */
int64_t StrDoubleToInt64(double r);

/* Example usage */
double d = 42.7;
StrDoubleCanBeCastToInt64(d);        /* false: 42.7 is not an integer */
int64_t i = StrDoubleToInt64(d);     /* 42 — TRUNCATED, not rounded */

double e = 42.0;
StrDoubleCanBeCastToInt64(e);        /* true */
int64_t j = StrDoubleToInt64(e);     /* 42 */

/* Saturation, not wraparound or UB */
int64_t sat = StrDoubleToInt64(1e300);   /* INT64_MAX */
```

The exclusion of `INT64_MIN` and `INT64_MAX` from
`StrDoubleCanBeCastToInt64` is deliberate: those are exactly the values the
saturating clamp produces for out-of-range inputs, so accepting them would
make an overflow indistinguishable from a real conversion.

## Power of 10 Functions

```c
/* Fast power of 10 calculations */
uint64_t StrTenPow(size_t exp);        /* 10^exp (64-bit) */
__uint128_t StrTenPowBig(size_t exp);  /* 10^exp (128-bit) */

/* Example usage */
uint64_t thousand = StrTenPow(3);      /* 1000 */
uint64_t million = StrTenPow(6);       /* 1000000 */
__uint128_t huge = StrTenPowBig(30);   /* 10^30 */
```

## Bit and Random Utilities

`str.h` also declares the population-count, bitmap-position, and PRNG
families. They are listed and explained in
[STR_UTILS](STR_UTILS.md#population-count-popcount):

```c
uint64_t StrPopCnt8Bit(const void *data, size_t len);
uint64_t StrPopCntAligned(const void *data, size_t len);
uint64_t StrPopCntExact(const void *data, size_t len);
uint64_t StrPopCntScalar(const void *data, size_t len);  /* baseline */

/* The indices of the set (or clear) bits of 'len' bytes into position[],
 * returning how many; the width names the index type, so the 8-bit form
 * reads up to 32 bytes, the 16-bit up to 8 KB */
uint32_t StrBitmapGetSetPositionsExact8(const void *data, size_t len, uint8_t position[]);
uint32_t StrBitmapGetSetPositionsExact16(const void *data, size_t len, uint16_t position[]);
uint32_t StrBitmapGetSetPositionsExact32(const void *data, size_t len, uint32_t position[]);
uint64_t StrBitmapGetSetPositionsExact64(const void *data, size_t len, uint64_t position[]);
uint32_t StrBitmapGetUnsetPositionsExact8(const void *data, size_t len, uint8_t position[]);
uint32_t StrBitmapGetUnsetPositionsExact16(const void *data, size_t len, uint16_t position[]);
uint32_t StrBitmapGetUnsetPositionsExact32(const void *data, size_t len, uint32_t position[]);
uint64_t StrBitmapGetUnsetPositionsExact64(const void *data, size_t len, uint64_t position[]);

uint64_t splitmix64(uint64_t *x);
uint64_t xoroshiro128plus(uint64_t s[2]);
/* ...and the other xorshift variants */
```

None of the generators are cryptographically secure. For anything
security-relevant use the crypto module, not these.

## Utility Macros

```c
/* Alignment and rounding */
#define ROUND8(x)      /* Round up to next multiple of 8 */
#define ROUNDDOWN8(x)  /* Round down to multiple of 8 */

/* Multiplication helpers */
#define StrValTimes10(val)   /* Multiply by 10 */
#define StrValTimes100(val)  /* Multiply by 100 */

/* Example usage */
size_t rounded = ROUND8(23);  /* 24 */
uint64_t result = StrValTimes10(42);  /* 420 */
```

## Real-World Examples

### Example 1: Parse User Input Reliably

```c
/* Parse user input with guaranteed round-trip accuracy */
void parseUserInput(const char *input) {
    databox result;

    if (StrScanScanReliable(input, strlen(input), &result)) {
        switch (result.type) {
        case DATABOX_SIGNED_64:
            printf("Integer: %" PRId64 "\n", result.data.i64);
            break;
        case DATABOX_UNSIGNED_64:
            printf("Unsigned: %" PRIu64 "\n", result.data.u64);
            break;
        case DATABOX_FLOAT_32:
            printf("Float: %f\n", result.data.f32);
            break;
        case DATABOX_DOUBLE_64:
            printf("Double: %f\n", result.data.d64);
            break;
        default:
            printf("Unsupported type\n");
        }

        /* Convert back.  Because the reliable scanner ACCEPTS ONLY the
         * canonical spelling of a number, the formatted output for an
         * accepted input reproduces that input byte for byte. */
        char buf[64];
        size_t len = 0;
        if (result.type == DATABOX_DOUBLE_64) {
            len = StrDoubleFormatToBufNice(buf, sizeof(buf), result.data.d64);
        } else if (result.type == DATABOX_SIGNED_64) {
            len = StrInt64ToBuf(buf, sizeof(buf), result.data.i64);
        }

        if (len) {
            assert(strlen(input) == len);
            assert(memcmp(input, buf, len) == 0);
        }
    } else {
        printf("Not a valid number\n");
    }
}
```

### Example 2: Fast Batch Integer Conversion

```c
/* Convert many integers to strings efficiently */
void batchConvert(uint64_t *numbers, size_t count) {
    char buf[21];  /* Max 20 digits + null */

    for (size_t i = 0; i < count; i++) {
        size_t len = StrUInt64ToBuf(buf, sizeof(buf), numbers[i]);

        /* Process string (e.g., write to file) */
        fwrite(buf, 1, len, stdout);
        fwrite("\n", 1, 1, stdout);
    }
}
```

### Example 3: UTF-8 String Truncation

```c
/* Safely truncate UTF-8 string to N characters */
char *truncateUtf8(const char *str, size_t maxChars) {
    size_t len = strlen(str);
    size_t chars = StrLenUtf8(str, len);

    if (chars <= maxChars) {
        /* datakit deprecates strdup(); copy through zmalloc */
        char *copy = zmalloc(len + 1);
        memcpy(copy, str, len + 1);
        return copy;
    }

    /* Find byte count for maxChars characters */
    size_t bytes = StrLenUtf8CountBytes(str, len, maxChars);

    char *result = zmalloc(bytes + 4);  /* +3 for "..." +1 for NUL */
    memcpy(result, str, bytes);
    memcpy(result + bytes, "...", 3);
    result[bytes + 3] = '\0';

    return result;
}
```

### Example 4: Validate All-Digit String

```c
/* Check if string is a valid positive integer */
bool isValidPositiveInteger(const char *str, size_t len) {
    /* No leading zeros (except "0" alone) */
    if (len > 1 && str[0] == '0') {
        return false;
    }

    /* All digits? */
    if (!StrIsDigitsFast(str, len)) {
        return false;
    }

    /* Not too large for uint64? */
    uint64_t value;
    return StrBufToUInt64(str, len, &value);
}
```

## Performance Characteristics

| Operation           | Complexity | Notes                                              |
| ------------------- | ---------- | -------------------------------------------------- |
| StrIsDigitsFast     | O(n)       | 16 bytes per step via SSE2 or NEON                 |
| StrBufToUInt64Fast  | O(n)       | no validation at all; caller must guarantee digits |
| StrUInt64ToBuf      | O(digits)  | SWAR digit generation                              |
| StrLenUtf8          | O(n)       | SIMD; `StrLenUtf8Scalar` is the baseline           |
| StrScanScanReliable | O(n)       | includes canonical-form validation                 |
| Bloom filter test   | O(1)       | single bit check                                   |

Each SIMD-accelerated function has a `*Scalar` twin
(`StrLenUtf8Scalar`, `StrUtf8ValidScalar`, `StrBufToUInt64Scalar`,
`StrPopCntScalar`, `StrUtf8AdvanceScalar`) kept specifically so the two can
be benchmarked against each other. Build benchmarks with the `perf` CMake
preset — sanitizers invalidate the comparison.

## Thread Safety

All str functions are **thread-safe** as they operate on caller-provided buffers without global state. The global lookup tables (`StrCtypeMap`, etc.) are read-only constants.

## Common Pitfalls

### 1. Buffer Size for Integer Conversion

```c
/* WRONG - buffer too small */
char buf[10];
StrInt64ToBuf(buf, sizeof(buf), INT64_MIN);  /* Needs 20 bytes! */

/* RIGHT */
char buf[21];  /* 20 digits + sign */
StrInt64ToBuf(buf, sizeof(buf), INT64_MIN);
```

### 2. UTF-8 vs Byte Length

```c
/* WRONG - confusing bytes and characters */
const char *str = "💛";
char buf[2];
memcpy(buf, str, 2);  /* Partial emoji! Invalid UTF-8 */

/* RIGHT */
size_t bytes = StrLenUtf8CountBytes(str, strlen(str), 1);
char *buf = zmalloc(bytes + 1);
memcpy(buf, str, bytes);
buf[bytes] = '\0';
```

### 3. Reliable Scan Rejecting Valid Numbers

```c
/* These all FAIL reliable scan (by design) */
StrScanScanReliable("0003", 4, &box);    /* Leading zeros */
StrScanScanReliable(".123", 4, &box);    /* Leading decimal */
StrScanScanReliable("1.50", 4, &box);    /* Trailing zero */

/* These succeed */
StrScanScanReliable("3", 1, &box);       /* OK */
StrScanScanReliable("0.123", 5, &box);   /* OK */
StrScanScanReliable("1.5", 3, &box);     /* OK */
```

## See Also

- [DKS](DKS.md) - Dynamic string buffers using DKS template
- [STR_DOUBLE_FORMAT](STR_DOUBLE_FORMAT.md) - Fast double-to-string conversion
- [STR_UTILS](STR_UTILS.md) - Extended string utilities

## Testing

Run the str test suite:

```bash
./src/datakit-test test str
```

The test suite includes:

- Round-trip number conversion validation
- UTF-8 character counting accuracy
- Overflow detection
- Edge cases (INT64_MIN, UINT64_MAX, etc.)
- Performance benchmarks
