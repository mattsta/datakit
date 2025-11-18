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

**Headers**: `str.h`

**Source**: `str.c` (includes all str/*.c files)

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

/* Convert string to 64-bit integer */
int32_t StrAtoi64(const void *zNum, int64_t *pNum, int32_t length,
                  uint8_t enc, bool skipSpaces);

/* Example usage */
int64_t value;
const char *str = "12345";
int32_t result = StrAtoi64(str, &value, strlen(str), STR_UTF8, false);
if (result == 0) {
    printf("Converted: %ld\n", value);  /* 12345 */
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
/* Convert string to 128-bit integer */
bool StrScanScanReliableConvert128(const void *p, const size_t len,
                                   databox *small, databoxBig *big,
                                   databoxBig **use);

/* Example usage */
const char *huge = "170141183460469231731687303715884105727";
databox sm;
databoxBig bigbox;
databoxBig *result;

if (StrScanScanReliableConvert128(huge, strlen(huge), &sm, &bigbox, &result)) {
    if (result->type == DATABOX_SIGNED_128) {
        printf("128-bit value stored\n");
    }
}
```

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
    STRSCAN_OPT_LL = 0x08,     /* Allow long long */
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
    printf("Integer: %ld\n", result.data.i64);
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
/* Fixed-width digit conversions (for internal use) */
void StrUInt9DigitsToBuf(void *p, uint32_t u);   /* Requires 9 bytes */
void StrUInt8DigitsToBuf(void *p, uint32_t u);   /* Requires 8 bytes */
void StrUInt4DigitsToBuf(void *p, uint32_t u);   /* Requires 4 bytes */

/* Example usage - batch convert digits */
char buf[9];
StrUInt9DigitsToBuf(buf, 123456789);
/* buf contains "123456789" */
```

### String to Integer (Fast)

```c
/* Fast conversions with various safety levels */

/* Fastest - no checks, must be all digits */
uint64_t StrBufToUInt64Fast(const void *buf, size_t len);

/* With numeric validation */
bool StrBufToUInt64FastCheckNumeric(const void *buf, size_t len,
                                    uint64_t *value);

/* With overflow checking */
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
    printf("Valid number: %lu\n", checked);
}

int64_t safe;
if (StrBufToInt64(str, 5, &safe)) {
    printf("Safely converted: %ld\n", safe);
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
bool StrIsDigitsFast(const void *buf, size_t size);        /* SSE2 optimized */

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

### Count UTF-8 Characters

```c
/* Count characters in UTF-8 string */
size_t StrLenUtf8(const void *ss, size_t len);

/* Example usage */
const char *str = "Hello 💛";  /* 6 ASCII + 1 emoji (4 bytes) = 10 bytes */
size_t chars = StrLenUtf8(str, strlen(str));  /* 7 characters */
size_t bytes = strlen(str);  /* 10 bytes */

printf("Characters: %zu, Bytes: %zu\n", chars, bytes);
```

### Get Byte Count for N Characters

```c
/* How many bytes for first N UTF-8 characters? */
size_t StrLenUtf8CountBytes(const void *ss, size_t len,
                            size_t countCharacters);

/* Example usage */
const char *str = "ab💛cd";  /* 2 + 4 + 2 = 8 bytes, 5 chars */

/* Get bytes for first 3 characters */
size_t bytes = StrLenUtf8CountBytes(str, strlen(str), 3);  /* 6 bytes */

/* Extract substring */
char sub[7];
memcpy(sub, str, bytes);
sub[bytes] = '\0';
printf("First 3 chars: %s\n", sub);  /* "ab💛" */
```

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
printf("Estimated rows: %lu (actual: %lu)\n", estimated, rowCount);
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

## Double Conversion Helpers

```c
/* Check if double can be represented as int64 */
bool StrDoubleCanBeCastToInt64(double r);

/* Convert double to int64 with proper rounding */
int64_t StrDoubleToInt64(double r);

/* Example usage */
double d = 42.7;
if (StrDoubleCanBeCastToInt64(d)) {
    int64_t i = StrDoubleToInt64(d);  /* 43 (rounds) */
}
```

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
            printf("Integer: %ld\n", result.data.i64);
            break;
        case DATABOX_UNSIGNED_64:
            printf("Unsigned: %lu\n", result.data.u64);
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

        /* Convert back - guaranteed exact match */
        char buf[64];
        size_t len;
        if (result.type == DATABOX_DOUBLE_64) {
            len = StrDoubleFormatToBufNice(buf, sizeof(buf), result.data.d64);
        } else if (result.type == DATABOX_SIGNED_64) {
            len = StrInt64ToBuf(buf, sizeof(buf), result.data.i64);
        }

        assert(strlen(input) == len);
        assert(memcmp(input, buf, len) == 0);
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
        return strdup(str);
    }

    /* Find byte count for maxChars characters */
    size_t bytes = StrLenUtf8CountBytes(str, len, maxChars);

    char *result = malloc(bytes + 4);  /* +3 for "..." +1 for null */
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

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| StrIsDigitsFast | O(n) | SSE2: ~4x faster than individual checks |
| StrBufToUInt64Fast | O(n) | No validation, maximum speed |
| StrUInt64ToBuf | O(1) | Uses SWAR, ~60 cycles typical |
| StrLenUtf8 | O(n) | SIMD optimized for large strings |
| StrScanScanReliable | O(n) | With round-trip validation |
| Bloom filter test | O(1) | Single bit check |

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
char *buf = malloc(bytes + 1);
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
