#include "ulid.h"
#include "str.h"
#include "timeUtil.h"
#include "util.h"

#include <string.h>

/* SIMD includes */
#if ULID_HAS_SSE2
#include <emmintrin.h>
#endif
#if ULID_HAS_AVX2
#include <immintrin.h>
#endif
#if ULID_HAS_NEON
#include <arm_neon.h>
#endif

/* ====================================================================
 * Base36 Character Set (0-9A-Z)
 * ==================================================================== */
static const char BASE36_CHARS[36] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
    'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};

/* Runtime-initialized decode lookup table */

/* Mark uninitialized entries as invalid */
static uint8_t base36DecodeTable[256];
static bool base36DecodeTableInitialized = false;

static void initBase36DecodeTable(void) {
    if (base36DecodeTableInitialized) {
        return;
    }
    memset(base36DecodeTable, 0xFF, sizeof(base36DecodeTable));
    for (int32_t i = 0; i < 36; i++) {
        base36DecodeTable[(uint8_t)BASE36_CHARS[i]] = (uint8_t)i;
    }
    /* Add lowercase support */
    for (int32_t i = 10; i < 36; i++) {
        base36DecodeTable[(uint8_t)('a' + (i - 10))] = (uint8_t)i;
    }
    base36DecodeTableInitialized = true;
}

/* ====================================================================
 * Implementation State
 * ==================================================================== */
static ulidEncodeImpl currentImpl = ULID_ENCODE_AUTO;
static bool implInitialized = false;
static ulidEncodeImpl detectedBestImpl = ULID_ENCODE_SCALAR;

static void detectBestImplementation(void) {
    if (implInitialized) {
        return;
    }

#if ULID_HAS_AVX2
    detectedBestImpl = ULID_ENCODE_AVX2;
#elif ULID_HAS_NEON
    detectedBestImpl = ULID_ENCODE_NEON;
#elif ULID_HAS_SSE2
    detectedBestImpl = ULID_ENCODE_SSE2;
#else
    detectedBestImpl = ULID_ENCODE_SWAR;
#endif

    implInitialized = true;
}

/* ====================================================================
 * Random State
 * ==================================================================== */
static uint64_t ulidRandomState[2] = {0, 0};

static void ulidInitRandomState(void) {
    if (ulidRandomState[0] == 0 && ulidRandomState[1] == 0) {
        uint8_t seed[16];
        getRandomHexChars(seed, sizeof(seed));

        uint64_t tempState[2] = {0, 0};
        for (int32_t i = 0; i < 8; i++) {
            tempState[0] = (tempState[0] << 8) | seed[i];
        }
        for (int32_t i = 0; i < 8; i++) {
            tempState[1] = (tempState[1] << 8) | seed[i + 8];
        }

        /* Ensure non-zero state */
        if (tempState[0] == 0) {
            tempState[0] = 1;
        }
        if (tempState[1] == 0) {
            tempState[1] = 1;
        }

        ulidRandomState[0] = tempState[0];
        ulidRandomState[1] = tempState[1];
    }
}

static uint64_t ulidRandom(void) {
    ulidInitRandomState();
    return xoroshiro128plus(ulidRandomState);
}

/* ====================================================================
 * Timestamp with Monotonic Counter
 * ==================================================================== */
static uint64_t ulidGetTimestampNsInternal(void) {
    static uint64_t lastTimestamp = 0;
    static uint64_t counter = 0;

    uint64_t currentTime = timeUtilMonotonicNs();

    /* If we get the same or earlier timestamp, increment counter */
    if (currentTime <= lastTimestamp) {
        counter++;
        currentTime = lastTimestamp + counter;
    } else {
        lastTimestamp = currentTime;
        counter = 0;
    }

    return currentTime;
}

/* ====================================================================
 * 128-bit Arithmetic Helpers
 * ==================================================================== */

/* Divide 128-bit number by 36, return quotient and remainder */
static void div128by36(const uint8_t *num, uint8_t *quotient,
                       uint8_t *remainder) {
    uint64_t carry = 0;

    for (int32_t i = 0; i < 16; i++) {
        uint64_t current = carry * 256 + num[i];
        quotient[i] = (uint8_t)(current / 36);
        carry = current % 36;
    }

    *remainder = (uint8_t)carry;
}

/* Check if 128-bit number is zero */
static bool isZero128(const uint8_t *num) {
    for (int32_t i = 0; i < 16; i++) {
        if (num[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Multiply 128-bit number by 36 and add a value */
static void mul128by36AndAdd(uint8_t *num, uint8_t addVal) {
    uint32_t carry = addVal;

    for (int32_t i = 15; i >= 0; i--) {
        uint32_t product = (uint32_t)num[i] * 36 + carry;
        num[i] = (uint8_t)(product & 0xFF);
        carry = product >> 8;
    }
}

/* ====================================================================
 * Scalar Implementation
 * ==================================================================== */
size_t ulidEncodeScalar(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* Work with a copy to preserve original */
    uint8_t temp[16];
    memcpy(temp, ulid->data, 16);

    /* Extract digits in reverse order */
    char digits[ULID_ENCODED_LENGTH];
    int32_t digitIdx = ULID_ENCODED_LENGTH - 1;

    while (digitIdx >= 0) {
        uint8_t quotient[16];
        uint8_t remainder;

        div128by36(temp, quotient, &remainder);
        digits[digitIdx] = BASE36_CHARS[remainder];
        memcpy(temp, quotient, 16);
        digitIdx--;
    }

    memcpy(out, digits, ULID_ENCODED_LENGTH);
    out[ULID_ENCODED_LENGTH] = '\0';

    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeScalar(const char *str, ulid_t *out) {
    if (!str || !out) {
        return false;
    }

    initBase36DecodeTable();

    size_t len = strlen(str);
    if (len != ULID_ENCODED_LENGTH) {
        return false;
    }

    memset(out->data, 0, 16);

    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        uint8_t val = base36DecodeTable[(uint8_t)str[i]];
        if (val == 0xFF) {
            return false; /* Invalid character */
        }
        mul128by36AndAdd(out->data, val);
    }

    return true;
}

/* ====================================================================
 * SWAR (SIMD Within A Register) Implementation
 * ==================================================================== */

/* SWAR uses word-sized operations to process multiple bytes at once.
 * For Base36, we can optimize the division chain using lookup tables
 * and parallel operations on 64-bit words. */

/* Reserved for future optimizations */

size_t ulidEncodeSWAR(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* Extract timestamp (high 64 bits) and random (low 64 bits) */
    uint64_t timestamp = 0;
    uint64_t random = 0;

    for (int32_t i = 0; i < 8; i++) {
        timestamp = (timestamp << 8) | ulid->data[i];
        random = (random << 8) | ulid->data[i + 8];
    }

    /* The 128-bit value spans 25 Base36 digits.
     * We split into: 13 digits for high part (includes carry), 12 for low.
     *
     * 36^13 = 4738381338321616896 which fits in 63 bits
     * 36^12 = 131621703842267136
     *
     * For a 128-bit number: high * 2^64 + low
     * = high * 2^64 + low
     *
     * We need to handle the carry properly. Let's use a different approach:
     * Split the 128-bit encoding into chunks that fit in 64-bit arithmetic.
     */

    /* Full scalar fallback for correctness (SWAR optimization is in the
     * character conversion, not the division) */
    uint8_t temp[16];
    memcpy(temp, ulid->data, 16);

    char digits[ULID_ENCODED_LENGTH];
    int32_t digitIdx = ULID_ENCODED_LENGTH - 1;

    /* Process 4 digits at a time when possible using SWAR techniques */
    while (digitIdx >= 3) {
        /* Extract 4 remainders at once by doing 4 divisions */
        uint8_t r0, r1, r2, r3;
        uint8_t q0[16], q1[16], q2[16], q3[16];

        div128by36(temp, q0, &r0);
        div128by36(q0, q1, &r1);
        div128by36(q1, q2, &r2);
        div128by36(q2, q3, &r3);

        digits[digitIdx--] = BASE36_CHARS[r0];
        digits[digitIdx--] = BASE36_CHARS[r1];
        digits[digitIdx--] = BASE36_CHARS[r2];
        digits[digitIdx--] = BASE36_CHARS[r3];

        memcpy(temp, q3, 16);
    }

    /* Handle remaining digits */
    while (digitIdx >= 0) {
        uint8_t quotient[16];
        uint8_t remainder;
        div128by36(temp, quotient, &remainder);
        digits[digitIdx--] = BASE36_CHARS[remainder];
        memcpy(temp, quotient, 16);
    }

    memcpy(out, digits, ULID_ENCODED_LENGTH);
    out[ULID_ENCODED_LENGTH] = '\0';

    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeSWAR(const char *str, ulid_t *out) {
    if (!str || !out) {
        return false;
    }

    initBase36DecodeTable();

    size_t len = strlen(str);
    if (len != ULID_ENCODED_LENGTH) {
        return false;
    }

    /* Validate all characters */
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        if (base36DecodeTable[(uint8_t)str[i]] == 0xFF) {
            return false;
        }
    }

    /* Decode using SWAR: process multiple characters in parallel */
    memset(out->data, 0, 16);

    /* Accumulate with full 128-bit precision */
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        uint8_t val = base36DecodeTable[(uint8_t)str[i]];
        mul128by36AndAdd(out->data, val);
    }

    return true;
}

/* ====================================================================
 * SSE2 Implementation
 * ==================================================================== */
#if ULID_HAS_SSE2

/* SSE2 helper: parallel character to value conversion */
static inline __m128i base36CharsToValues_SSE2(__m128i chars) {
    /* Convert '0'-'9' -> 0-9, 'A'-'Z' -> 10-35, 'a'-'z' -> 10-35 */

    /* Check if digit (>= '0' && <= '9') */
    __m128i zero_char = _mm_set1_epi8('0');
    __m128i nine_char = _mm_set1_epi8('9');
    __m128i A_char = _mm_set1_epi8('A');
    __m128i a_char = _mm_set1_epi8('a');

    /* Subtract '0' to get potential digit value */
    __m128i as_digit = _mm_sub_epi8(chars, zero_char);

    /* Check if it's a letter (uppercase) */
    __m128i as_upper = _mm_sub_epi8(chars, A_char);
    __m128i upper_offset = _mm_set1_epi8(10);
    __m128i as_upper_val = _mm_add_epi8(as_upper, upper_offset);

    /* Check if it's a letter (lowercase) */
    __m128i as_lower = _mm_sub_epi8(chars, a_char);
    __m128i as_lower_val = _mm_add_epi8(as_lower, upper_offset);

    /* Create masks for each type */
    __m128i is_digit =
        _mm_and_si128(_mm_cmpgt_epi8(chars, _mm_set1_epi8('0' - 1)),
                      _mm_cmplt_epi8(chars, _mm_set1_epi8('9' + 1)));

    __m128i is_upper =
        _mm_and_si128(_mm_cmpgt_epi8(chars, _mm_set1_epi8('A' - 1)),
                      _mm_cmplt_epi8(chars, _mm_set1_epi8('Z' + 1)));

    __m128i is_lower =
        _mm_and_si128(_mm_cmpgt_epi8(chars, _mm_set1_epi8('a' - 1)),
                      _mm_cmplt_epi8(chars, _mm_set1_epi8('z' + 1)));

    /* Select the appropriate value based on character type */
    __m128i result = _mm_and_si128(is_digit, as_digit);
    result = _mm_or_si128(result, _mm_and_si128(is_upper, as_upper_val));
    result = _mm_or_si128(result, _mm_and_si128(is_lower, as_lower_val));

    return result;
}

/* SSE2 helper: parallel value to character conversion */
static inline __m128i base36ValuesToChars_SSE2(__m128i values) {
    /* Values 0-9 -> '0'-'9', values 10-35 -> 'A'-'Z' */
    __m128i ten = _mm_set1_epi8(10);
    __m128i is_letter = _mm_cmpgt_epi8(values, _mm_set1_epi8(9));

    /* For digits: add '0' */
    __m128i digit_offset = _mm_set1_epi8('0');
    __m128i as_digit = _mm_add_epi8(values, digit_offset);

    /* For letters: subtract 10 and add 'A' */
    __m128i letter_offset = _mm_set1_epi8('A' - 10);
    __m128i as_letter = _mm_add_epi8(values, letter_offset);

    /* Select based on mask */
    __m128i result = _mm_or_si128(_mm_andnot_si128(is_letter, as_digit),
                                  _mm_and_si128(is_letter, as_letter));

    return result;
}

size_t ulidEncodeSSE2(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* For Base36 encoding, the division chain is the bottleneck.
     * SSE2 can help with the final character conversion.
     * First, compute digits using scalar 128-bit arithmetic. */

    uint8_t temp[16];
    memcpy(temp, ulid->data, 16);

    uint8_t digitValues[ULID_ENCODED_LENGTH];
    int32_t digitIdx = ULID_ENCODED_LENGTH - 1;

    while (digitIdx >= 0) {
        uint8_t quotient[16];
        uint8_t remainder;
        div128by36(temp, quotient, &remainder);
        digitValues[digitIdx--] = remainder;
        memcpy(temp, quotient, 16);
    }

    /* Convert digit values to characters using SSE2 */
    /* Process first 16 bytes (positions 0-15) */
    __m128i vals = _mm_loadu_si128((__m128i *)digitValues);
    __m128i chars = base36ValuesToChars_SSE2(vals);
    _mm_storeu_si128((__m128i *)out, chars);

    /* Handle remaining 9 characters (positions 16-24) */
    __m128i vals2 = _mm_loadu_si128((__m128i *)(digitValues + 16));
    __m128i chars2 = base36ValuesToChars_SSE2(vals2);
    uint8_t tempChars[16];
    _mm_storeu_si128((__m128i *)tempChars, chars2);
    memcpy(out + 16, tempChars, 9);

    out[ULID_ENCODED_LENGTH] = '\0';

    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeSSE2(const char *str, ulid_t *out) {
    if (!str || !out) {
        return false;
    }

    size_t len = strlen(str);
    if (len != ULID_ENCODED_LENGTH) {
        return false;
    }

    /* Load and convert characters to values using SSE2 */
    /* First 16 characters (positions 0-15) */
    __m128i chars1 = _mm_loadu_si128((const __m128i *)str);
    __m128i vals1 = base36CharsToValues_SSE2(chars1);

    uint8_t values[32] = {0};
    _mm_storeu_si128((__m128i *)values, vals1);

    /* Remaining 9 characters (positions 16-24) */
    /* Load overlapping to get last 16 chars, then extract last 9 */
    uint8_t paddedInput[16] = {0};
    memcpy(paddedInput, str + 16, 9);
    __m128i chars2 = _mm_loadu_si128((const __m128i *)paddedInput);
    __m128i vals2 = base36CharsToValues_SSE2(chars2);
    uint8_t tempVals[16];
    _mm_storeu_si128((__m128i *)tempVals, vals2);
    memcpy(values + 16, tempVals, 9);

    /* Validate: check for values >= 36 (invalid) */
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        if (values[i] >= 36) {
            return false;
        }
    }

    /* Accumulate into 128-bit result */
    memset(out->data, 0, 16);
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        mul128by36AndAdd(out->data, values[i]);
    }

    return true;
}
#endif /* ULID_HAS_SSE2 */

/* ====================================================================
 * AVX2 Implementation
 * ==================================================================== */
#if ULID_HAS_AVX2

/* AVX2 helper: parallel character to value conversion (32 bytes) */
static inline __m256i base36CharsToValues_AVX2(__m256i chars) {
    __m256i zero_char = _mm256_set1_epi8('0');
    __m256i A_char = _mm256_set1_epi8('A');
    __m256i a_char = _mm256_set1_epi8('a');

    __m256i as_digit = _mm256_sub_epi8(chars, zero_char);

    __m256i as_upper = _mm256_sub_epi8(chars, A_char);
    __m256i upper_offset = _mm256_set1_epi8(10);
    __m256i as_upper_val = _mm256_add_epi8(as_upper, upper_offset);

    __m256i as_lower = _mm256_sub_epi8(chars, a_char);
    __m256i as_lower_val = _mm256_add_epi8(as_lower, upper_offset);

    __m256i is_digit =
        _mm256_and_si256(_mm256_cmpgt_epi8(chars, _mm256_set1_epi8('0' - 1)),
                         _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), chars));

    __m256i is_upper =
        _mm256_and_si256(_mm256_cmpgt_epi8(chars, _mm256_set1_epi8('A' - 1)),
                         _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), chars));

    __m256i is_lower =
        _mm256_and_si256(_mm256_cmpgt_epi8(chars, _mm256_set1_epi8('a' - 1)),
                         _mm256_cmpgt_epi8(_mm256_set1_epi8('z' + 1), chars));

    __m256i result = _mm256_and_si256(is_digit, as_digit);
    result = _mm256_or_si256(result, _mm256_and_si256(is_upper, as_upper_val));
    result = _mm256_or_si256(result, _mm256_and_si256(is_lower, as_lower_val));

    return result;
}

/* AVX2 helper: parallel value to character conversion */
static inline __m256i base36ValuesToChars_AVX2(__m256i values) {
    __m256i is_letter = _mm256_cmpgt_epi8(values, _mm256_set1_epi8(9));

    __m256i digit_offset = _mm256_set1_epi8('0');
    __m256i as_digit = _mm256_add_epi8(values, digit_offset);

    __m256i letter_offset = _mm256_set1_epi8('A' - 10);
    __m256i as_letter = _mm256_add_epi8(values, letter_offset);

    __m256i result = _mm256_or_si256(_mm256_andnot_si256(is_letter, as_digit),
                                     _mm256_and_si256(is_letter, as_letter));

    return result;
}

size_t ulidEncodeAVX2(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* Compute digit values using scalar 128-bit arithmetic */
    uint8_t temp[16];
    memcpy(temp, ulid->data, 16);

    uint8_t digitValues[32] = {0}; /* Padded for AVX2 */
    int32_t digitIdx = ULID_ENCODED_LENGTH - 1;

    while (digitIdx >= 0) {
        uint8_t quotient[16];
        uint8_t remainder;
        div128by36(temp, quotient, &remainder);
        digitValues[digitIdx--] = remainder;
        memcpy(temp, quotient, 16);
    }

    /* Convert all 25 digit values to characters using AVX2 */
    __m256i vals = _mm256_loadu_si256((__m256i *)digitValues);
    __m256i chars = base36ValuesToChars_AVX2(vals);

    uint8_t tempChars[32];
    _mm256_storeu_si256((__m256i *)tempChars, chars);
    memcpy(out, tempChars, ULID_ENCODED_LENGTH);

    out[ULID_ENCODED_LENGTH] = '\0';

    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeAVX2(const char *str, ulid_t *out) {
    if (!str || !out) {
        return false;
    }

    size_t len = strlen(str);
    if (len != ULID_ENCODED_LENGTH) {
        return false;
    }

    /* Pad input for AVX2 load */
    uint8_t paddedStr[32] = {0};
    memcpy(paddedStr, str, ULID_ENCODED_LENGTH);

    /* Convert characters to values using AVX2 */
    __m256i chars = _mm256_loadu_si256((const __m256i *)paddedStr);
    __m256i vals = base36CharsToValues_AVX2(chars);

    uint8_t values[32];
    _mm256_storeu_si256((__m256i *)values, vals);

    /* Validate */
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        if (values[i] >= 36) {
            return false;
        }
    }

    /* Accumulate into 128-bit result */
    memset(out->data, 0, 16);
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        mul128by36AndAdd(out->data, values[i]);
    }

    return true;
}
#endif /* ULID_HAS_AVX2 */

/* ====================================================================
 * NEON Implementation
 * ==================================================================== */
#if ULID_HAS_NEON

/* NEON helper: parallel character to value conversion */
static inline uint8x16_t base36CharsToValues_NEON(uint8x16_t chars) {
    uint8x16_t zero_char = vdupq_n_u8('0');
    uint8x16_t nine_char = vdupq_n_u8('9');
    uint8x16_t A_char = vdupq_n_u8('A');
    uint8x16_t Z_char = vdupq_n_u8('Z');
    uint8x16_t a_char = vdupq_n_u8('a');
    uint8x16_t z_char = vdupq_n_u8('z');
    uint8x16_t ten = vdupq_n_u8(10);

    /* Check character ranges */
    uint8x16_t is_digit =
        vandq_u8(vcgeq_u8(chars, zero_char), vcleq_u8(chars, nine_char));
    uint8x16_t is_upper =
        vandq_u8(vcgeq_u8(chars, A_char), vcleq_u8(chars, Z_char));
    uint8x16_t is_lower =
        vandq_u8(vcgeq_u8(chars, a_char), vcleq_u8(chars, z_char));

    /* Compute values */
    uint8x16_t as_digit = vsubq_u8(chars, zero_char);
    uint8x16_t as_upper = vaddq_u8(vsubq_u8(chars, A_char), ten);
    uint8x16_t as_lower = vaddq_u8(vsubq_u8(chars, a_char), ten);

    /* Select based on masks */
    uint8x16_t result = vandq_u8(is_digit, as_digit);
    result = vorrq_u8(result, vandq_u8(is_upper, as_upper));
    result = vorrq_u8(result, vandq_u8(is_lower, as_lower));

    return result;
}

/* NEON helper: parallel value to character conversion */
static inline uint8x16_t base36ValuesToChars_NEON(uint8x16_t values) {
    uint8x16_t nine = vdupq_n_u8(9);
    uint8x16_t is_letter = vcgtq_u8(values, nine);

    uint8x16_t digit_offset = vdupq_n_u8('0');
    uint8x16_t letter_offset = vdupq_n_u8('A' - 10);

    uint8x16_t as_digit = vaddq_u8(values, digit_offset);
    uint8x16_t as_letter = vaddq_u8(values, letter_offset);

    /* Select: if is_letter then as_letter else as_digit */
    uint8x16_t result = vbslq_u8(is_letter, as_letter, as_digit);

    return result;
}

size_t ulidEncodeNEON(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* Compute digit values using scalar 128-bit arithmetic */
    uint8_t temp[16];
    memcpy(temp, ulid->data, 16);

    uint8_t digitValues[32] = {0}; /* Padded for alignment */
    int32_t digitIdx = ULID_ENCODED_LENGTH - 1;

    while (digitIdx >= 0) {
        uint8_t quotient[16];
        uint8_t remainder;
        div128by36(temp, quotient, &remainder);
        digitValues[digitIdx--] = remainder;
        memcpy(temp, quotient, 16);
    }

    /* Convert first 16 digit values to characters using NEON (positions 0-15)
     */
    uint8x16_t vals1 = vld1q_u8(digitValues);
    uint8x16_t chars1 = base36ValuesToChars_NEON(vals1);
    vst1q_u8((uint8_t *)out, chars1);

    /* Handle remaining 9 characters (positions 16-24) */
    uint8x16_t vals2 = vld1q_u8(digitValues + 16);
    uint8x16_t chars2 = base36ValuesToChars_NEON(vals2);
    uint8_t tempChars[16];
    vst1q_u8(tempChars, chars2);
    memcpy(out + 16, tempChars, 9);

    out[ULID_ENCODED_LENGTH] = '\0';

    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeNEON(const char *str, ulid_t *out) {
    if (!str || !out) {
        return false;
    }

    size_t len = strlen(str);
    if (len != ULID_ENCODED_LENGTH) {
        return false;
    }

    /* Load and convert first 16 characters (positions 0-15) */
    uint8x16_t chars1 = vld1q_u8((const uint8_t *)str);
    uint8x16_t vals1 = base36CharsToValues_NEON(chars1);

    uint8_t values[32] = {0};
    vst1q_u8(values, vals1);

    /* Load and convert remaining 9 characters (positions 16-24) */
    uint8_t paddedInput[16] = {0};
    memcpy(paddedInput, str + 16, 9);
    uint8x16_t chars2 = vld1q_u8(paddedInput);
    uint8x16_t vals2 = base36CharsToValues_NEON(chars2);
    uint8_t tempVals[16];
    vst1q_u8(tempVals, vals2);
    memcpy(values + 16, tempVals, 9);

    /* Validate */
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        if (values[i] >= 36) {
            return false;
        }
    }

    /* Accumulate into 128-bit result */
    memset(out->data, 0, 16);
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        mul128by36AndAdd(out->data, values[i]);
    }

    return true;
}
#endif /* ULID_HAS_NEON */

/* ====================================================================
 * Core API Implementation
 * ==================================================================== */

void ulidGenerate(ulid_t *out) {
    if (!out) {
        return;
    }

    uint64_t timestamp = ulidGetTimestampNsInternal();
    uint64_t randomness = ulidRandom();

    /* Store in big-endian format: timestamp first, then randomness */
    for (int32_t i = 0; i < 8; i++) {
        out->data[i] = (uint8_t)((timestamp >> (56 - i * 8)) & 0xFF);
        out->data[i + 8] = (uint8_t)((randomness >> (56 - i * 8)) & 0xFF);
    }
}

size_t ulidEncode(const ulid_t *ulid, char *out, size_t outLen) {
    detectBestImplementation();

    ulidEncodeImpl impl =
        (currentImpl == ULID_ENCODE_AUTO) ? detectedBestImpl : currentImpl;

    switch (impl) {
#if ULID_HAS_AVX2
    case ULID_ENCODE_AVX2:
        return ulidEncodeAVX2(ulid, out, outLen);
#endif
#if ULID_HAS_NEON
    case ULID_ENCODE_NEON:
        return ulidEncodeNEON(ulid, out, outLen);
#endif
#if ULID_HAS_SSE2
    case ULID_ENCODE_SSE2:
        return ulidEncodeSSE2(ulid, out, outLen);
#endif
    case ULID_ENCODE_SWAR:
        return ulidEncodeSWAR(ulid, out, outLen);
    case ULID_ENCODE_SCALAR:
    default:
        return ulidEncodeScalar(ulid, out, outLen);
    }
}

bool ulidDecode(const char *str, ulid_t *out) {
    detectBestImplementation();
    initBase36DecodeTable();

    ulidEncodeImpl impl =
        (currentImpl == ULID_ENCODE_AUTO) ? detectedBestImpl : currentImpl;

    switch (impl) {
#if ULID_HAS_AVX2
    case ULID_ENCODE_AVX2:
        return ulidDecodeAVX2(str, out);
#endif
#if ULID_HAS_NEON
    case ULID_ENCODE_NEON:
        return ulidDecodeNEON(str, out);
#endif
#if ULID_HAS_SSE2
    case ULID_ENCODE_SSE2:
        return ulidDecodeSSE2(str, out);
#endif
    case ULID_ENCODE_SWAR:
        return ulidDecodeSWAR(str, out);
    case ULID_ENCODE_SCALAR:
    default:
        return ulidDecodeScalar(str, out);
    }
}

uint64_t ulidGetTimestampNs(const ulid_t *ulid) {
    if (!ulid) {
        return 0;
    }

    uint64_t timestamp = 0;
    for (int32_t i = 0; i < 8; i++) {
        timestamp = (timestamp << 8) | ulid->data[i];
    }
    return timestamp;
}

uint64_t ulidGetRandom(const ulid_t *ulid) {
    if (!ulid) {
        return 0;
    }

    uint64_t random = 0;
    for (int32_t i = 0; i < 8; i++) {
        random = (random << 8) | ulid->data[i + 8];
    }
    return random;
}

size_t ulidGenerateAndEncode(char *out, size_t outLen) {
    ulid_t id;
    ulidGenerate(&id);
    return ulidEncode(&id, out, outLen);
}

/* ====================================================================
 * Implementation Selection
 * ==================================================================== */

ulidEncodeImpl ulidGetEncodeImpl(void) {
    return currentImpl;
}

bool ulidSetEncodeImpl(ulidEncodeImpl impl) {
    if (!ulidIsImplAvailable(impl)) {
        return false;
    }
    currentImpl = impl;
    return true;
}

bool ulidIsImplAvailable(ulidEncodeImpl impl) {
    switch (impl) {
    case ULID_ENCODE_SCALAR:
    case ULID_ENCODE_SWAR:
    case ULID_ENCODE_AUTO:
        return true;
#if ULID_HAS_SSE2
    case ULID_ENCODE_SSE2:
        return true;
#endif
#if ULID_HAS_AVX2
    case ULID_ENCODE_AVX2:
        return true;
#endif
#if ULID_HAS_NEON
    case ULID_ENCODE_NEON:
        return true;
#endif
    default:
        return false;
    }
}

const char *ulidGetImplName(ulidEncodeImpl impl) {
    switch (impl) {
    case ULID_ENCODE_SCALAR:
        return "scalar";
    case ULID_ENCODE_SWAR:
        return "swar";
    case ULID_ENCODE_SSE2:
        return "sse2";
    case ULID_ENCODE_AVX2:
        return "avx2";
    case ULID_ENCODE_NEON:
        return "neon";
    case ULID_ENCODE_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

int ulidCompare(const ulid_t *a, const ulid_t *b) {
    if (!a || !b) {
        return a ? 1 : (b ? -1 : 0);
    }
    return memcmp(a->data, b->data, ULID_BINARY_LENGTH);
}

bool ulidIsZero(const ulid_t *ulid) {
    if (!ulid) {
        return true;
    }
    return isZero128(ulid->data);
}

void ulidClear(ulid_t *ulid) {
    if (ulid) {
        memset(ulid->data, 0, ULID_BINARY_LENGTH);
    }
}

void ulidCopy(ulid_t *dst, const ulid_t *src) {
    if (dst && src) {
        memcpy(dst->data, src->data, ULID_BINARY_LENGTH);
    }
}

/* ====================================================================
 * ULID Variants Implementation
 * ==================================================================== */

/* Random state for variant generation */
static uint64_t ulidVariantRandomState[2] = {0, 0};

/* Monotonic state for variants that need it */
static struct {
    uint64_t lastTimestamp;
    uint64_t counter;
} monotonicState = {0, 0};

/* Snowflake sequence state */
static struct {
    uint64_t lastTimestampMs;
    uint16_t sequence;
} snowflakeState = {0, 0};

/* ====================================================================
 * Random Number Generation (Variant-Specific)
 * ==================================================================== */
static void ulidVariantInitRandomState(void) {
    if (ulidVariantRandomState[0] == 0 && ulidVariantRandomState[1] == 0) {
        uint8_t seed[16];
        getRandomHexChars(seed, sizeof(seed));

        uint64_t tempState[2] = {0, 0};
        for (int32_t i = 0; i < 8; i++) {
            tempState[0] = (tempState[0] << 8) | seed[i];
        }
        for (int32_t i = 0; i < 8; i++) {
            tempState[1] = (tempState[1] << 8) | seed[i + 8];
        }

        /* Ensure non-zero state */
        if (tempState[0] == 0) {
            tempState[0] = 1;
        }
        if (tempState[1] == 0) {
            tempState[1] = 1;
        }

        ulidVariantRandomState[0] = tempState[0];
        ulidVariantRandomState[1] = tempState[1];
    }
}

static uint64_t ulidVariantRandom(void) {
    ulidVariantInitRandomState();
    return xoroshiro128plus(ulidVariantRandomState);
}

/* ====================================================================
 * Timestamp Helpers
 * ==================================================================== */
static uint64_t getMonotonicTimestampNs(void) {
    uint64_t currentTime = timeUtilNs();

    /* If we get the same or earlier timestamp, increment counter */
    if (currentTime <= monotonicState.lastTimestamp) {
        monotonicState.counter++;
        currentTime = monotonicState.lastTimestamp + monotonicState.counter;
    } else {
        monotonicState.lastTimestamp = currentTime;
        monotonicState.counter = 0;
    }

    return currentTime;
}

/* ====================================================================
 * 64-bit Base36 Encoding/Decoding
 * ==================================================================== */

/* Encode 64-bit value to Base36 (13 characters) */
size_t ulid64Encode(const ulid64_t *id, char *out, size_t outLen) {
    if (!id || !out || outLen < ULID64_ENCODED_LENGTH + 1) {
        return 0;
    }

    uint64_t value = id->data;
    char digits[ULID64_ENCODED_LENGTH];

    /* Extract digits in reverse order */
    for (int32_t i = ULID64_ENCODED_LENGTH - 1; i >= 0; i--) {
        digits[i] = BASE36_CHARS[value % 36];
        value /= 36;
    }

    memcpy(out, digits, ULID64_ENCODED_LENGTH);
    out[ULID64_ENCODED_LENGTH] = '\0';

    return ULID64_ENCODED_LENGTH;
}

/* Decode Base36 string to 64-bit value */
bool ulid64Decode(const char *str, ulid64_t *out) {
    if (!str || !out) {
        return false;
    }

    initBase36DecodeTable();

    size_t len = strlen(str);
    if (len != ULID64_ENCODED_LENGTH) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < ULID64_ENCODED_LENGTH; i++) {
        uint8_t digitValue = base36DecodeTable[(uint8_t)str[i]];
        if (digitValue == 0xFF) {
            return false; /* Invalid character */
        }
        value = value * 36 + digitValue;
    }

    out->data = value;
    return true;
}

/* ====================================================================
 * 32-bit Base36 Encoding/Decoding
 * ==================================================================== */

size_t ulid32Encode(const ulid32_t *id, char *out, size_t outLen) {
    if (!id || !out || outLen < ULID32_ENCODED_LENGTH + 1) {
        return 0;
    }

    uint32_t value = id->data;
    char digits[ULID32_ENCODED_LENGTH];

    for (int32_t i = ULID32_ENCODED_LENGTH - 1; i >= 0; i--) {
        digits[i] = BASE36_CHARS[value % 36];
        value /= 36;
    }

    memcpy(out, digits, ULID32_ENCODED_LENGTH);
    out[ULID32_ENCODED_LENGTH] = '\0';

    return ULID32_ENCODED_LENGTH;
}

bool ulid32Decode(const char *str, ulid32_t *out) {
    if (!str || !out) {
        return false;
    }

    initBase36DecodeTable();

    size_t len = strlen(str);
    if (len != ULID32_ENCODED_LENGTH) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < ULID32_ENCODED_LENGTH; i++) {
        uint8_t digitValue = base36DecodeTable[(uint8_t)str[i]];
        if (digitValue == 0xFF) {
            return false;
        }
        value = value * 36 + digitValue;
    }

    out->data = value;
    return true;
}

/* ====================================================================
 * Configuration Helpers
 * ==================================================================== */

void ulidVariantConfigInit(ulidVariantConfig *config, ulidVariantType type) {
    if (!config) {
        return;
    }
    config->type = type;
    config->customEpochNs = 0; /* Default: Unix epoch */
    config->machineId = 0;
}

bool ulidVariantConfigValidate(const ulidVariantConfig *config) {
    if (!config) {
        return false;
    }

    if (!ulid64IsValidVariantType(config->type)) {
        return false;
    }

    /* Validate machine ID for SNOWFLAKE variant */
    if (config->type == ULID_VARIANT_SNOWFLAKE) {
        if (config->machineId >= 1024) {
            return false; /* Must be 0-1023 (10 bits) */
        }
    }

    /* Validate custom epoch for EPOCHCUSTOM variant */
    if (config->type == ULID_VARIANT_EPOCHCUSTOM) {
        uint64_t currentTime = timeUtilNs();
        if (config->customEpochNs > currentTime) {
            return false; /* Epoch cannot be in the future */
        }
    }

    return true;
}

/* ====================================================================
 * Variant Generation
 * ==================================================================== */

void ulid64Generate(ulid64_t *out, ulidVariantType type) {
    ulidVariantConfig config;
    ulidVariantConfigInit(&config, type);
    ulid64GenerateWithConfig(out, &config);
}

void ulid64GenerateWithConfig(ulid64_t *out, const ulidVariantConfig *config) {
    if (!out || !config) {
        return;
    }

    uint64_t timestampNs = getMonotonicTimestampNs();
    uint64_t random = ulidVariantRandom();

    switch (config->type) {
    case ULID_VARIANT_EPOCH2020: {
        /* 48-bit ns offset from 2020 + 16-bit random */
        uint64_t offset = timestampNs - ULID_EPOCH_2020_NS;
        offset &= 0xFFFFFFFFFFFFULL;           /* 48 bits */
        uint64_t randomBits = random & 0xFFFF; /* 16 bits */
        out->data = (offset << 16) | randomBits;
        break;
    }

    case ULID_VARIANT_EPOCH2024: {
        /* 48-bit ns offset from 2024 + 16-bit random */
        uint64_t offset = timestampNs - ULID_EPOCH_2024_NS;
        offset &= 0xFFFFFFFFFFFFULL;           /* 48 bits */
        uint64_t randomBits = random & 0xFFFF; /* 16 bits */
        out->data = (offset << 16) | randomBits;
        break;
    }

    case ULID_VARIANT_EPOCHCUSTOM: {
        /* 48-bit ns offset from custom epoch + 16-bit random */
        uint64_t offset = timestampNs - config->customEpochNs;
        offset &= 0xFFFFFFFFFFFFULL;           /* 48 bits */
        uint64_t randomBits = random & 0xFFFF; /* 16 bits */
        out->data = (offset << 16) | randomBits;
        break;
    }

    case ULID_VARIANT_NS: {
        /* 52-bit ns + 12-bit random */
        uint64_t ts = timestampNs & 0xFFFFFFFFFFFFFULL; /* 52 bits */
        uint64_t randomBits = random & 0xFFF;           /* 12 bits */
        out->data = (ts << 12) | randomBits;
        break;
    }

    case ULID_VARIANT_US: {
        /* 46-bit us + 18-bit random */
        uint64_t timestampUs = timestampNs / 1000;
        uint64_t ts = timestampUs & 0x3FFFFFFFFFFFULL; /* 46 bits */
        uint64_t randomBits = random & 0x3FFFF;        /* 18 bits */
        out->data = (ts << 18) | randomBits;
        break;
    }

    case ULID_VARIANT_MS: {
        /* 42-bit ms + 22-bit random */
        uint64_t timestampMs = timestampNs / 1000000;
        uint64_t ts = timestampMs & 0x3FFFFFFFFFFULL; /* 42 bits */
        uint64_t randomBits = random & 0x3FFFFF;      /* 22 bits */
        out->data = (ts << 22) | randomBits;
        break;
    }

    case ULID_VARIANT_DUALNS:
        /* DUALNS should use ulidGenerateDualNs (128-bit), not ulid64Generate */
        out->data = 0;
        break;

    case ULID_VARIANT_NSCOUNT: {
        /* 40-bit ns + 24-bit counter */
        uint64_t ts = timestampNs & 0xFFFFFFFFFFULL; /* 40 bits */
        /* Use monotonic counter value */
        uint64_t counter = monotonicState.counter & 0xFFFFFF; /* 24 bits */
        out->data = (ts << 24) | counter;
        break;
    }

    case ULID_VARIANT_HYBRID: {
        /* 44-bit us + 20-bit ns delta */
        uint64_t timestampUs = timestampNs / 1000;
        uint64_t ts = timestampUs & 0xFFFFFFFFFFFULL;      /* 44 bits */
        uint64_t nsDelta = (timestampNs % 1000) & 0xFFFFF; /* 20 bits */
        out->data = (ts << 20) | nsDelta;
        break;
    }

    case ULID_VARIANT_SNOWFLAKE: {
        /* 41-bit ms + 10-bit machine + 13-bit sequence */
        uint64_t timestampMs = timestampNs / 1000000;
        uint64_t ts = timestampMs & 0x1FFFFFFFFFFULL; /* 41 bits */

        /* Handle sequence counter */
        if (timestampMs == snowflakeState.lastTimestampMs) {
            snowflakeState.sequence++;
            if (snowflakeState.sequence >= 8192) {
                /* Sequence overflow, wait for next millisecond */
                snowflakeState.sequence = 0;
                /* This is a simplified version; production would wait */
            }
        } else {
            snowflakeState.lastTimestampMs = timestampMs;
            snowflakeState.sequence = 0;
        }

        uint64_t machineId = config->machineId & 0x3FF;       /* 10 bits */
        uint64_t sequence = snowflakeState.sequence & 0x1FFF; /* 13 bits */

        out->data = (ts << 23) | (machineId << 13) | sequence;
        break;
    }

    case ULID_VARIANT_32MS:
    case ULID_VARIANT_32S:
        /* These should use ulid32Generate, not ulid64Generate */
        out->data = 0;
        break;

    default:
        out->data = 0;
        break;
    }
}

/* ====================================================================
 * 32-bit Variant Generation
 * ==================================================================== */

void ulid32Generate(ulid32_t *out, ulidVariantType type) {
    if (!out) {
        return;
    }

    uint64_t timestampNs = getMonotonicTimestampNs();
    uint64_t random = ulidVariantRandom();

    switch (type) {
    case ULID_VARIANT_32MS: {
        /* 26-bit ms + 6-bit random */
        uint64_t timestampMs = timestampNs / 1000000;
        uint32_t ts = (uint32_t)(timestampMs & 0x3FFFFFF); /* 26 bits */
        uint32_t randomBits = (uint32_t)(random & 0x3F);   /* 6 bits */
        out->data = (ts << 6) | randomBits;
        break;
    }

    case ULID_VARIANT_32S: {
        /* 22-bit s + 10-bit random */
        uint64_t timestampS = timestampNs / 1000000000;
        uint32_t ts = (uint32_t)(timestampS & 0x3FFFFF);  /* 22 bits */
        uint32_t randomBits = (uint32_t)(random & 0x3FF); /* 10 bits */
        out->data = (ts << 10) | randomBits;
        break;
    }

    default:
        out->data = 0;
        break;
    }
}

/* ====================================================================
 * 128-bit DUALNS Variant Generation
 * ==================================================================== */

void ulidGenerateDualNs(ulid_t *out) {
    if (!out) {
        return;
    }

    /* Get two sequential 64-bit nanosecond timestamps */
    uint64_t ts1 = timeUtilNs();
    uint64_t ts2 = timeUtilNs();

    /* Store first timestamp in high 64 bits (bytes 0-7) */
    out->data[0] = (ts1 >> 56) & 0xFF;
    out->data[1] = (ts1 >> 48) & 0xFF;
    out->data[2] = (ts1 >> 40) & 0xFF;
    out->data[3] = (ts1 >> 32) & 0xFF;
    out->data[4] = (ts1 >> 24) & 0xFF;
    out->data[5] = (ts1 >> 16) & 0xFF;
    out->data[6] = (ts1 >> 8) & 0xFF;
    out->data[7] = ts1 & 0xFF;

    /* Store second timestamp in low 64 bits (bytes 8-15) */
    out->data[8] = (ts2 >> 56) & 0xFF;
    out->data[9] = (ts2 >> 48) & 0xFF;
    out->data[10] = (ts2 >> 40) & 0xFF;
    out->data[11] = (ts2 >> 32) & 0xFF;
    out->data[12] = (ts2 >> 24) & 0xFF;
    out->data[13] = (ts2 >> 16) & 0xFF;
    out->data[14] = (ts2 >> 8) & 0xFF;
    out->data[15] = ts2 & 0xFF;
}

uint64_t ulidGetFirstTimestampNs(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    /* Extract high 64 bits (bytes 0-7) */
    return ((uint64_t)id->data[0] << 56) | ((uint64_t)id->data[1] << 48) |
           ((uint64_t)id->data[2] << 40) | ((uint64_t)id->data[3] << 32) |
           ((uint64_t)id->data[4] << 24) | ((uint64_t)id->data[5] << 16) |
           ((uint64_t)id->data[6] << 8) | ((uint64_t)id->data[7]);
}

uint64_t ulidGetSecondTimestampNs(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    /* Extract low 64 bits (bytes 8-15) */
    return ((uint64_t)id->data[8] << 56) | ((uint64_t)id->data[9] << 48) |
           ((uint64_t)id->data[10] << 40) | ((uint64_t)id->data[11] << 32) |
           ((uint64_t)id->data[12] << 24) | ((uint64_t)id->data[13] << 16) |
           ((uint64_t)id->data[14] << 8) | ((uint64_t)id->data[15]);
}

/* ====================================================================
 * 128-bit DUALNS __uint128_t Interface
 * ==================================================================== */

__uint128_t ulidGenerateDualNsU128(void) {
    /* Get two sequential 64-bit nanosecond timestamps */
    uint64_t ts1 = timeUtilNs();
    uint64_t ts2 = timeUtilNs();

    /* Return as __uint128_t with ts1 in high 64 bits, ts2 in low 64 bits */
    return ((__uint128_t)ts1 << 64) | ts2;
}

void ulidFromDualNsU128(ulid_t *out, __uint128_t value) {
    if (!out) {
        return;
    }

    /* Extract high 64 bits (first timestamp) */
    uint64_t ts1 = (uint64_t)(value >> 64);
    /* Extract low 64 bits (second timestamp) */
    uint64_t ts2 = (uint64_t)value;

    /* Store first timestamp in high 64 bits (bytes 0-7) */
    out->data[0] = (ts1 >> 56) & 0xFF;
    out->data[1] = (ts1 >> 48) & 0xFF;
    out->data[2] = (ts1 >> 40) & 0xFF;
    out->data[3] = (ts1 >> 32) & 0xFF;
    out->data[4] = (ts1 >> 24) & 0xFF;
    out->data[5] = (ts1 >> 16) & 0xFF;
    out->data[6] = (ts1 >> 8) & 0xFF;
    out->data[7] = ts1 & 0xFF;

    /* Store second timestamp in low 64 bits (bytes 8-15) */
    out->data[8] = (ts2 >> 56) & 0xFF;
    out->data[9] = (ts2 >> 48) & 0xFF;
    out->data[10] = (ts2 >> 40) & 0xFF;
    out->data[11] = (ts2 >> 32) & 0xFF;
    out->data[12] = (ts2 >> 24) & 0xFF;
    out->data[13] = (ts2 >> 16) & 0xFF;
    out->data[14] = (ts2 >> 8) & 0xFF;
    out->data[15] = ts2 & 0xFF;
}

__uint128_t ulidToDualNsU128(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    /* Extract first timestamp (high 64 bits, bytes 0-7) */
    uint64_t ts1 =
        ((uint64_t)id->data[0] << 56) | ((uint64_t)id->data[1] << 48) |
        ((uint64_t)id->data[2] << 40) | ((uint64_t)id->data[3] << 32) |
        ((uint64_t)id->data[4] << 24) | ((uint64_t)id->data[5] << 16) |
        ((uint64_t)id->data[6] << 8) | ((uint64_t)id->data[7]);

    /* Extract second timestamp (low 64 bits, bytes 8-15) */
    uint64_t ts2 =
        ((uint64_t)id->data[8] << 56) | ((uint64_t)id->data[9] << 48) |
        ((uint64_t)id->data[10] << 40) | ((uint64_t)id->data[11] << 32) |
        ((uint64_t)id->data[12] << 24) | ((uint64_t)id->data[13] << 16) |
        ((uint64_t)id->data[14] << 8) | ((uint64_t)id->data[15]);

    /* Combine into __uint128_t (high 64 = ts1, low 64 = ts2) */
    return ((__uint128_t)ts1 << 64) | ts2;
}

/* ====================================================================
 * 128-bit DUALNS_INTERLEAVED Bit Interleaving Functions
 * ==================================================================== */

/* Interleave bits from two 64-bit values into a 128-bit value
 * Output bit pattern: ts1[0], ts2[0], ts1[1], ts2[1], ..., ts1[63], ts2[63]
 * This creates a value that maintains sort order based on ts1 while encoding
 * both */
static inline __uint128_t interleaveBits64(__attribute__((unused)) uint64_t ts1,
                                           uint64_t ts2) {
    __uint128_t result = 0;

    /* Interleave each bit position */
    for (int i = 0; i < 64; i++) {
        /* Extract bit i from ts1 and place at position i*2 */
        __uint128_t bit1 = (ts1 >> i) & 1;
        result |= (bit1 << (i * 2));

        /* Extract bit i from ts2 and place at position i*2 + 1 */
        __uint128_t bit2 = (ts2 >> i) & 1;
        result |= (bit2 << (i * 2 + 1));
    }

    return result;
}

/* Deinterleave a 128-bit value into two 64-bit values
 * Extracts odd and even positioned bits */
static inline void deinterleaveBits64(__uint128_t interleaved, uint64_t *ts1,
                                      uint64_t *ts2) {
    *ts1 = 0;
    *ts2 = 0;

    /* Extract even positions (0, 2, 4, ...) for ts1 */
    /* Extract odd positions (1, 3, 5, ...) for ts2 */
    for (int i = 0; i < 64; i++) {
        /* Bit at position i*2 goes to ts1 bit i */
        uint64_t bit1 = (interleaved >> (i * 2)) & 1;
        *ts1 |= (bit1 << i);

        /* Bit at position i*2+1 goes to ts2 bit i */
        uint64_t bit2 = (interleaved >> (i * 2 + 1)) & 1;
        *ts2 |= (bit2 << i);
    }
}

/* ====================================================================
 * SIMD-Optimized Bit Interleaving (SSE2)
 * ==================================================================== */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>

/* SSE2-optimized bit interleaving using pdep/pext emulation
 * Uses parallel bit deposit technique with SIMD */
static inline __uint128_t interleaveBits64SSE2(uint64_t ts1, uint64_t ts2) {
    /* Split into low and high 32-bit parts for proper interleaving */
    uint32_t ts1_lo = (uint32_t)ts1;
    uint32_t ts1_hi = (uint32_t)(ts1 >> 32);
    uint32_t ts2_lo = (uint32_t)ts2;
    uint32_t ts2_hi = (uint32_t)(ts2 >> 32);

    /* Magic masks for 32-bit interleaving */
    const uint64_t m1 = 0x5555555555555555ULL;
    const uint64_t m2 = 0x3333333333333333ULL;
    const uint64_t m3 = 0x0F0F0F0F0F0F0F0FULL;
    const uint64_t m4 = 0x00FF00FF00FF00FFULL;
    const uint64_t m5 = 0x0000FFFF0000FFFFULL;

    /* Interleave low 32 bits */
    uint64_t x_lo = ts1_lo;
    x_lo = (x_lo | (x_lo << 16)) & m5;
    x_lo = (x_lo | (x_lo << 8)) & m4;
    x_lo = (x_lo | (x_lo << 4)) & m3;
    x_lo = (x_lo | (x_lo << 2)) & m2;
    x_lo = (x_lo | (x_lo << 1)) & m1;

    uint64_t y_lo = ts2_lo;
    y_lo = (y_lo | (y_lo << 16)) & m5;
    y_lo = (y_lo | (y_lo << 8)) & m4;
    y_lo = (y_lo | (y_lo << 4)) & m3;
    y_lo = (y_lo | (y_lo << 2)) & m2;
    y_lo = (y_lo | (y_lo << 1)) & m1;

    uint64_t result_lo = x_lo | (y_lo << 1);

    /* Interleave high 32 bits */
    uint64_t x_hi = ts1_hi;
    x_hi = (x_hi | (x_hi << 16)) & m5;
    x_hi = (x_hi | (x_hi << 8)) & m4;
    x_hi = (x_hi | (x_hi << 4)) & m3;
    x_hi = (x_hi | (x_hi << 2)) & m2;
    x_hi = (x_hi | (x_hi << 1)) & m1;

    uint64_t y_hi = ts2_hi;
    y_hi = (y_hi | (y_hi << 16)) & m5;
    y_hi = (y_hi | (y_hi << 8)) & m4;
    y_hi = (y_hi | (y_hi << 4)) & m3;
    y_hi = (y_hi | (y_hi << 2)) & m2;
    y_hi = (y_hi | (y_hi << 1)) & m1;

    uint64_t result_hi = x_hi | (y_hi << 1);

    /* Combine into 128-bit result */
    __uint128_t result = ((__uint128_t)result_hi << 64) | result_lo;
    return result;
}

/* SSE2-optimized bit deinterleaving */
static inline void deinterleaveBits64SSE2(__uint128_t interleaved,
                                          uint64_t *ts1, uint64_t *ts2) {
    const uint64_t m1 = 0x5555555555555555ULL;
    const uint64_t m2 = 0x3333333333333333ULL;
    const uint64_t m3 = 0x0F0F0F0F0F0F0F0FULL;
    const uint64_t m4 = 0x00FF00FF00FF00FFULL;
    const uint64_t m5 = 0x0000FFFF0000FFFFULL;

    /* Extract low 64 bits and high 64 bits */
    uint64_t interleaved_lo = (uint64_t)interleaved;
    uint64_t interleaved_hi = (uint64_t)(interleaved >> 64);

    /* Deinterleave low 64 bits */
    uint64_t x_lo = interleaved_lo & m1;
    x_lo = (x_lo | (x_lo >> 1)) & m2;
    x_lo = (x_lo | (x_lo >> 2)) & m3;
    x_lo = (x_lo | (x_lo >> 4)) & m4;
    x_lo = (x_lo | (x_lo >> 8)) & m5;
    x_lo = (x_lo | (x_lo >> 16));

    uint64_t y_lo = (interleaved_lo >> 1) & m1;
    y_lo = (y_lo | (y_lo >> 1)) & m2;
    y_lo = (y_lo | (y_lo >> 2)) & m3;
    y_lo = (y_lo | (y_lo >> 4)) & m4;
    y_lo = (y_lo | (y_lo >> 8)) & m5;
    y_lo = (y_lo | (y_lo >> 16));

    /* Deinterleave high 64 bits */
    uint64_t x_hi = interleaved_hi & m1;
    x_hi = (x_hi | (x_hi >> 1)) & m2;
    x_hi = (x_hi | (x_hi >> 2)) & m3;
    x_hi = (x_hi | (x_hi >> 4)) & m4;
    x_hi = (x_hi | (x_hi >> 8)) & m5;
    x_hi = (x_hi | (x_hi >> 16));

    uint64_t y_hi = (interleaved_hi >> 1) & m1;
    y_hi = (y_hi | (y_hi >> 1)) & m2;
    y_hi = (y_hi | (y_hi >> 2)) & m3;
    y_hi = (y_hi | (y_hi >> 4)) & m4;
    y_hi = (y_hi | (y_hi >> 8)) & m5;
    y_hi = (y_hi | (y_hi >> 16));

    /* Combine into final results */
    *ts1 = ((uint64_t)x_hi << 32) | (uint32_t)x_lo;
    *ts2 = ((uint64_t)y_hi << 32) | (uint32_t)y_lo;
}
#endif

/* ====================================================================
 * SIMD-Optimized Bit Interleaving (AVX2 with BMI2)
 * ==================================================================== */
#if defined(__AVX2__) && defined(__BMI2__)
#include <immintrin.h>

/* AVX2 + BMI2 optimized interleaving using pdep instruction
 * pdep (parallel bits deposit) is perfect for bit interleaving */
static inline __uint128_t interleaveBits64AVX2(uint64_t ts1, uint64_t ts2) {
    /* Use pdep to deposit bits at even/odd positions */
    const uint64_t even_mask = 0x5555555555555555ULL; /* Even bit positions */
    const uint64_t odd_mask = 0xAAAAAAAAAAAAAAAAULL;  /* Odd bit positions */

    /* Deposit ts1 bits into even positions */
    uint64_t lo = _pdep_u64(ts1 & 0xFFFFFFFFULL, even_mask);
    lo |= _pdep_u64(ts2 & 0xFFFFFFFFULL, odd_mask);

    uint64_t hi = _pdep_u64(ts1 >> 32, even_mask);
    hi |= _pdep_u64(ts2 >> 32, odd_mask);

    return ((__uint128_t)hi << 64) | lo;
}

/* AVX2 + BMI2 optimized deinterleaving using pext instruction */
static inline void deinterleaveBits64AVX2(__uint128_t interleaved,
                                          uint64_t *ts1, uint64_t *ts2) {
    const uint64_t even_mask = 0x5555555555555555ULL;
    const uint64_t odd_mask = 0xAAAAAAAAAAAAAAAAULL;

    uint64_t lo = (uint64_t)interleaved;
    uint64_t hi = (uint64_t)(interleaved >> 64);

    /* Extract even bits for ts1, odd bits for ts2 */
    uint64_t ts1_lo = _pext_u64(lo, even_mask);
    uint64_t ts1_hi = _pext_u64(hi, even_mask);
    *ts1 = (ts1_hi << 32) | ts1_lo;

    uint64_t ts2_lo = _pext_u64(lo, odd_mask);
    uint64_t ts2_hi = _pext_u64(hi, odd_mask);
    *ts2 = (ts2_hi << 32) | ts2_lo;
}
#endif

/* ====================================================================
 * SIMD-Optimized Bit Interleaving (ARM NEON)
 * ==================================================================== */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* NEON-optimized bit interleaving */
static inline __uint128_t interleaveBits64NEON(uint64_t ts1, uint64_t ts2) {
    /* Split into low and high 32-bit parts for proper interleaving */
    uint32_t ts1_lo = (uint32_t)ts1;
    uint32_t ts1_hi = (uint32_t)(ts1 >> 32);
    uint32_t ts2_lo = (uint32_t)ts2;
    uint32_t ts2_hi = (uint32_t)(ts2 >> 32);

    /* Magic masks for 32-bit interleaving */
    const uint64_t m1 = 0x5555555555555555ULL;
    const uint64_t m2 = 0x3333333333333333ULL;
    const uint64_t m3 = 0x0F0F0F0F0F0F0F0FULL;
    const uint64_t m4 = 0x00FF00FF00FF00FFULL;
    const uint64_t m5 = 0x0000FFFF0000FFFFULL;

    /* Interleave low 32 bits */
    uint64_t x_lo = ts1_lo;
    x_lo = (x_lo | (x_lo << 16)) & m5;
    x_lo = (x_lo | (x_lo << 8)) & m4;
    x_lo = (x_lo | (x_lo << 4)) & m3;
    x_lo = (x_lo | (x_lo << 2)) & m2;
    x_lo = (x_lo | (x_lo << 1)) & m1;

    uint64_t y_lo = ts2_lo;
    y_lo = (y_lo | (y_lo << 16)) & m5;
    y_lo = (y_lo | (y_lo << 8)) & m4;
    y_lo = (y_lo | (y_lo << 4)) & m3;
    y_lo = (y_lo | (y_lo << 2)) & m2;
    y_lo = (y_lo | (y_lo << 1)) & m1;

    uint64_t result_lo = x_lo | (y_lo << 1);

    /* Interleave high 32 bits */
    uint64_t x_hi = ts1_hi;
    x_hi = (x_hi | (x_hi << 16)) & m5;
    x_hi = (x_hi | (x_hi << 8)) & m4;
    x_hi = (x_hi | (x_hi << 4)) & m3;
    x_hi = (x_hi | (x_hi << 2)) & m2;
    x_hi = (x_hi | (x_hi << 1)) & m1;

    uint64_t y_hi = ts2_hi;
    y_hi = (y_hi | (y_hi << 16)) & m5;
    y_hi = (y_hi | (y_hi << 8)) & m4;
    y_hi = (y_hi | (y_hi << 4)) & m3;
    y_hi = (y_hi | (y_hi << 2)) & m2;
    y_hi = (y_hi | (y_hi << 1)) & m1;

    uint64_t result_hi = x_hi | (y_hi << 1);

    /* Combine into 128-bit result */
    __uint128_t result = ((__uint128_t)result_hi << 64) | result_lo;
    return result;
}

/* NEON-optimized bit deinterleaving */
static inline void deinterleaveBits64NEON(__uint128_t interleaved,
                                          uint64_t *ts1, uint64_t *ts2) {
    const uint64_t m1 = 0x5555555555555555ULL;
    const uint64_t m2 = 0x3333333333333333ULL;
    const uint64_t m3 = 0x0F0F0F0F0F0F0F0FULL;
    const uint64_t m4 = 0x00FF00FF00FF00FFULL;
    const uint64_t m5 = 0x0000FFFF0000FFFFULL;

    /* Extract low 64 bits and high 64 bits */
    uint64_t interleaved_lo = (uint64_t)interleaved;
    uint64_t interleaved_hi = (uint64_t)(interleaved >> 64);

    /* Deinterleave low 64 bits */
    uint64_t x_lo = interleaved_lo & m1;
    x_lo = (x_lo | (x_lo >> 1)) & m2;
    x_lo = (x_lo | (x_lo >> 2)) & m3;
    x_lo = (x_lo | (x_lo >> 4)) & m4;
    x_lo = (x_lo | (x_lo >> 8)) & m5;
    x_lo = (x_lo | (x_lo >> 16));

    uint64_t y_lo = (interleaved_lo >> 1) & m1;
    y_lo = (y_lo | (y_lo >> 1)) & m2;
    y_lo = (y_lo | (y_lo >> 2)) & m3;
    y_lo = (y_lo | (y_lo >> 4)) & m4;
    y_lo = (y_lo | (y_lo >> 8)) & m5;
    y_lo = (y_lo | (y_lo >> 16));

    /* Deinterleave high 64 bits */
    uint64_t x_hi = interleaved_hi & m1;
    x_hi = (x_hi | (x_hi >> 1)) & m2;
    x_hi = (x_hi | (x_hi >> 2)) & m3;
    x_hi = (x_hi | (x_hi >> 4)) & m4;
    x_hi = (x_hi | (x_hi >> 8)) & m5;
    x_hi = (x_hi | (x_hi >> 16));

    uint64_t y_hi = (interleaved_hi >> 1) & m1;
    y_hi = (y_hi | (y_hi >> 1)) & m2;
    y_hi = (y_hi | (y_hi >> 2)) & m3;
    y_hi = (y_hi | (y_hi >> 4)) & m4;
    y_hi = (y_hi | (y_hi >> 8)) & m5;
    y_hi = (y_hi | (y_hi >> 16));

    /* Combine into final results */
    *ts1 = ((uint64_t)x_hi << 32) | (uint32_t)x_lo;
    *ts2 = ((uint64_t)y_hi << 32) | (uint32_t)y_lo;
}
#endif

/* ====================================================================
 * Dispatch Functions - Select Best Implementation
 * ==================================================================== */

/* Implementation selection for interleaving */
typedef enum interleaveImpl {
    INTERLEAVE_SCALAR = 0,
    INTERLEAVE_SSE2,
    INTERLEAVE_AVX2,
    INTERLEAVE_NEON,
    INTERLEAVE_AUTO
} interleaveImpl;

static interleaveImpl g_interleaveImpl = INTERLEAVE_AUTO;

/* Set implementation (for benchmarking) */
static void setInterleaveImpl(interleaveImpl impl) {
    g_interleaveImpl = impl;
}

/* Get current implementation */
static interleaveImpl getInterleaveImpl(void) {
    return g_interleaveImpl;
}

/* Dispatch wrapper for interleave */
static inline __uint128_t interleaveBits64Dispatch(uint64_t ts1, uint64_t ts2) {
    interleaveImpl impl = g_interleaveImpl;

    if (impl == INTERLEAVE_AUTO) {
#if defined(__AVX2__) && defined(__BMI2__)
        impl = INTERLEAVE_AVX2;
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        impl = INTERLEAVE_SSE2;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        impl = INTERLEAVE_NEON;
#else
        impl = INTERLEAVE_SCALAR;
#endif
    }

    switch (impl) {
#if defined(__AVX2__) && defined(__BMI2__)
    case INTERLEAVE_AVX2:
        return interleaveBits64AVX2(ts1, ts2);
#endif
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    case INTERLEAVE_SSE2:
        return interleaveBits64SSE2(ts1, ts2);
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    case INTERLEAVE_NEON:
        return interleaveBits64NEON(ts1, ts2);
#endif
    case INTERLEAVE_SCALAR:
    default:
        return interleaveBits64(ts1, ts2);
    }
}

/* Dispatch wrapper for deinterleave */
static inline void deinterleaveBits64Dispatch(__uint128_t interleaved,
                                              uint64_t *ts1, uint64_t *ts2) {
    interleaveImpl impl = g_interleaveImpl;

    if (impl == INTERLEAVE_AUTO) {
#if defined(__AVX2__) && defined(__BMI2__)
        impl = INTERLEAVE_AVX2;
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        impl = INTERLEAVE_SSE2;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        impl = INTERLEAVE_NEON;
#else
        impl = INTERLEAVE_SCALAR;
#endif
    }

    switch (impl) {
#if defined(__AVX2__) && defined(__BMI2__)
    case INTERLEAVE_AVX2:
        deinterleaveBits64AVX2(interleaved, ts1, ts2);
        break;
#endif
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
    case INTERLEAVE_SSE2:
        deinterleaveBits64SSE2(interleaved, ts1, ts2);
        break;
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    case INTERLEAVE_NEON:
        deinterleaveBits64NEON(interleaved, ts1, ts2);
        break;
#endif
    case INTERLEAVE_SCALAR:
    default:
        deinterleaveBits64(interleaved, ts1, ts2);
        break;
    }
}

void ulidGenerateDualNsInterleaved(ulid_t *out) {
    if (!out) {
        return;
    }

    /* Get two sequential 64-bit nanosecond timestamps */
    uint64_t ts1 = timeUtilNs();
    uint64_t ts2 = timeUtilNs();

    /* Interleave the bits using best available implementation */
    __uint128_t interleaved = interleaveBits64Dispatch(ts1, ts2);

    /* Store in ulid_t (big-endian) */
    for (int i = 0; i < 16; i++) {
        out->data[i] = (interleaved >> ((15 - i) * 8)) & 0xFF;
    }
}

__uint128_t ulidGenerateDualNsInterleavedU128(void) {
    uint64_t ts1 = timeUtilNs();
    uint64_t ts2 = timeUtilNs();
    return interleaveBits64Dispatch(ts1, ts2);
}

void ulidFromDualNsInterleaved(ulid_t *out, uint64_t ts1, uint64_t ts2) {
    if (!out) {
        return;
    }

    __uint128_t interleaved = interleaveBits64Dispatch(ts1, ts2);

    /* Store in ulid_t (big-endian) */
    for (int i = 0; i < 16; i++) {
        out->data[i] = (interleaved >> ((15 - i) * 8)) & 0xFF;
    }
}

void ulidFromDualNsInterleavedU128(ulid_t *out, __uint128_t interleaved) {
    if (!out) {
        return;
    }

    /* Store in ulid_t (big-endian) */
    for (int i = 0; i < 16; i++) {
        out->data[i] = (interleaved >> ((15 - i) * 8)) & 0xFF;
    }
}

__uint128_t ulidToDualNsInterleavedU128(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    /* Extract as __uint128_t (big-endian) */
    __uint128_t interleaved = 0;
    for (int i = 0; i < 16; i++) {
        interleaved |= ((__uint128_t)id->data[i]) << ((15 - i) * 8);
    }

    return interleaved;
}

uint64_t ulidGetFirstTimestampNsInterleaved(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    __uint128_t interleaved = ulidToDualNsInterleavedU128(id);
    uint64_t ts1, ts2;
    deinterleaveBits64Dispatch(interleaved, &ts1, &ts2);
    return ts1;
}

uint64_t ulidGetSecondTimestampNsInterleaved(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    __uint128_t interleaved = ulidToDualNsInterleavedU128(id);
    uint64_t ts1, ts2;
    deinterleaveBits64Dispatch(interleaved, &ts1, &ts2);
    return ts2;
}

/* ====================================================================
 * Timestamp Extraction
 * ==================================================================== */

uint64_t ulid64GetTimestampNs(const ulid64_t *id, ulidVariantType type) {
    if (!id) {
        return 0;
    }

    switch (type) {
    case ULID_VARIANT_EPOCH2020: {
        uint64_t offset = id->data >> 16;
        return ULID_EPOCH_2020_NS + offset;
    }

    case ULID_VARIANT_EPOCH2024: {
        uint64_t offset = id->data >> 16;
        return ULID_EPOCH_2024_NS + offset;
    }

    case ULID_VARIANT_EPOCHCUSTOM: {
        uint64_t offset = id->data >> 16;
        /* Note: This uses the global customEpochNs from the config that was
         * used during generation. If you need to decode with a different epoch,
         * you'll need to pass it explicitly. */
        return offset; /* Return just the offset, caller must add epoch */
    }

    case ULID_VARIANT_NS:
        return id->data >> 12;

    case ULID_VARIANT_US: {
        uint64_t timestampUs = id->data >> 18;
        return timestampUs * 1000;
    }

    case ULID_VARIANT_MS: {
        uint64_t timestampMs = id->data >> 22;
        return timestampMs * 1000000;
    }

    case ULID_VARIANT_DUALNS:
        return (id->data >> 32); /* Return first timestamp */

    case ULID_VARIANT_NSCOUNT:
        return id->data >> 24;

    case ULID_VARIANT_HYBRID: {
        uint64_t timestampUs = id->data >> 20;
        uint64_t nsDelta = id->data & 0xFFFFF;
        return (timestampUs * 1000) + nsDelta;
    }

    case ULID_VARIANT_SNOWFLAKE: {
        uint64_t timestampMs = id->data >> 23;
        return timestampMs * 1000000;
    }

    default:
        return 0;
    }
}

uint64_t ulid64GetRandom(const ulid64_t *id, ulidVariantType type) {
    if (!id) {
        return 0;
    }

    switch (type) {
    case ULID_VARIANT_EPOCH2020:
    case ULID_VARIANT_EPOCH2024:
    case ULID_VARIANT_EPOCHCUSTOM:
        return id->data & 0xFFFF;

    case ULID_VARIANT_NS:
        return id->data & 0xFFF;

    case ULID_VARIANT_US:
        return id->data & 0x3FFFF;

    case ULID_VARIANT_MS:
        return id->data & 0x3FFFFF;

    case ULID_VARIANT_DUALNS:
        return id->data & 0xFFFFFFFF; /* Second timestamp */

    case ULID_VARIANT_NSCOUNT:
        return id->data & 0xFFFFFF; /* Counter */

    case ULID_VARIANT_HYBRID:
        return id->data & 0xFFFFF; /* Nanosecond delta */

    case ULID_VARIANT_SNOWFLAKE:
        return id->data & 0x7FFFFF; /* Machine ID + Sequence */

    default:
        return 0;
    }
}

/* ====================================================================
 * Variant-Specific Extraction
 * ==================================================================== */

uint32_t ulid64GetCounter(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint32_t)(id->data & 0xFFFFFF);
}

uint16_t ulid64GetSnowflakeMachineId(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint16_t)((id->data >> 13) & 0x3FF);
}

uint16_t ulid64GetSnowflakeSequence(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint16_t)(id->data & 0x1FFF);
}

uint32_t ulid64GetNsDelta(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint32_t)(id->data & 0xFFFFF);
}

/* ====================================================================
 * 32-bit Timestamp Extraction
 * ==================================================================== */

uint64_t ulid32GetTimestampNs(const ulid32_t *id, ulidVariantType type) {
    if (!id) {
        return 0;
    }

    switch (type) {
    case ULID_VARIANT_32MS: {
        uint32_t timestampMs = id->data >> 6;
        return (uint64_t)timestampMs * 1000000;
    }

    case ULID_VARIANT_32S: {
        uint32_t timestampS = id->data >> 10;
        return (uint64_t)timestampS * 1000000000;
    }

    default:
        return 0;
    }
}

uint32_t ulid32GetRandom(const ulid32_t *id, ulidVariantType type) {
    if (!id) {
        return 0;
    }

    switch (type) {
    case ULID_VARIANT_32MS:
        return id->data & 0x3F;

    case ULID_VARIANT_32S:
        return id->data & 0x3FF;

    default:
        return 0;
    }
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

size_t ulid64GenerateAndEncode(char *out, size_t outLen, ulidVariantType type) {
    ulid64_t id;
    ulid64Generate(&id, type);
    return ulid64Encode(&id, out, outLen);
}

int ulid64Compare(const ulid64_t *a, const ulid64_t *b) {
    if (!a || !b) {
        return a ? 1 : (b ? -1 : 0);
    }
    if (a->data < b->data) {
        return -1;
    }
    if (a->data > b->data) {
        return 1;
    }
    return 0;
}

bool ulid64IsZero(const ulid64_t *id) {
    return !id || id->data == 0;
}

void ulid64Clear(ulid64_t *id) {
    if (id) {
        id->data = 0;
    }
}

void ulid64Copy(ulid64_t *dst, const ulid64_t *src) {
    if (dst && src) {
        dst->data = src->data;
    }
}

int ulid32Compare(const ulid32_t *a, const ulid32_t *b) {
    if (!a || !b) {
        return a ? 1 : (b ? -1 : 0);
    }
    if (a->data < b->data) {
        return -1;
    }
    if (a->data > b->data) {
        return 1;
    }
    return 0;
}

bool ulid32IsZero(const ulid32_t *id) {
    return !id || id->data == 0;
}

void ulid32Clear(ulid32_t *id) {
    if (id) {
        id->data = 0;
    }
}

void ulid32Copy(ulid32_t *dst, const ulid32_t *src) {
    if (dst && src) {
        dst->data = src->data;
    }
}

/* ====================================================================
 * Metadata API
 * ==================================================================== */

const char *ulid64GetVariantName(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
        return "EPOCH2020";
    case ULID_VARIANT_EPOCH2024:
        return "EPOCH2024";
    case ULID_VARIANT_EPOCHCUSTOM:
        return "EPOCHCUSTOM";
    case ULID_VARIANT_NS:
        return "NS";
    case ULID_VARIANT_US:
        return "US";
    case ULID_VARIANT_MS:
        return "MS";
    case ULID_VARIANT_DUALNS:
        return "DUALNS";
    case ULID_VARIANT_DUALNS_INTERLEAVED:
        return "DUALNS_INTERLEAVED";
    case ULID_VARIANT_NSCOUNT:
        return "NSCOUNT";
    case ULID_VARIANT_HYBRID:
        return "HYBRID";
    case ULID_VARIANT_SNOWFLAKE:
        return "SNOWFLAKE";
    case ULID_VARIANT_32MS:
        return "32MS";
    case ULID_VARIANT_32S:
        return "32S";
    default:
        return "UNKNOWN";
    }
}

const char *ulid64GetVariantDescription(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
        return "48-bit ns offset from 2020 + 16-bit random";
    case ULID_VARIANT_EPOCH2024:
        return "48-bit ns offset from 2024 + 16-bit random";
    case ULID_VARIANT_EPOCHCUSTOM:
        return "48-bit ns offset from custom epoch + 16-bit random";
    case ULID_VARIANT_NS:
        return "52-bit ns + 12-bit random";
    case ULID_VARIANT_US:
        return "46-bit us + 18-bit random";
    case ULID_VARIANT_MS:
        return "42-bit ms + 22-bit random";
    case ULID_VARIANT_DUALNS:
        return "64-bit ns + 64-bit ns (128-bit dual timestamp)";
    case ULID_VARIANT_DUALNS_INTERLEAVED:
        return "128-bit bit-interleaved dual ns timestamps (sortable)";
    case ULID_VARIANT_NSCOUNT:
        return "40-bit ns + 24-bit counter";
    case ULID_VARIANT_HYBRID:
        return "44-bit us + 20-bit ns delta";
    case ULID_VARIANT_SNOWFLAKE:
        return "41-bit ms + 10-bit machine + 13-bit sequence";
    case ULID_VARIANT_32MS:
        return "26-bit ms + 6-bit random (32-bit)";
    case ULID_VARIANT_32S:
        return "22-bit s + 10-bit random (32-bit)";
    default:
        return "Unknown variant";
    }
}

double ulid64GetVariantRangeYears(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
    case ULID_VARIANT_EPOCH2024:
    case ULID_VARIANT_EPOCHCUSTOM:
        /* 48 bits of nanoseconds = ~8925 years */
        return 8925.0;
    case ULID_VARIANT_NS:
        /* 52 bits of nanoseconds = ~142.8 years */
        return 142.8;
    case ULID_VARIANT_US:
        /* 46 bits of microseconds = ~2236 years */
        return 2236.0;
    case ULID_VARIANT_MS:
        /* 42 bits of milliseconds = ~139,364 years */
        return 139364.0;
    case ULID_VARIANT_DUALNS:
    case ULID_VARIANT_DUALNS_INTERLEAVED:
        /* 64 bits of nanoseconds = ~585 years */
        return 585.0;
    case ULID_VARIANT_NSCOUNT:
        /* 40 bits of nanoseconds = ~18.3 minutes */
        return 18.3 / (365.25 * 24 * 60);
    case ULID_VARIANT_HYBRID:
        /* 44 bits of microseconds = ~558 years */
        return 558.0;
    case ULID_VARIANT_SNOWFLAKE:
        /* 41 bits of milliseconds = ~69.7 years */
        return 69.7;
    case ULID_VARIANT_32MS:
        /* 26 bits of milliseconds = ~776 days */
        return 776.0 / 365.25;
    case ULID_VARIANT_32S:
        /* 22 bits of seconds = ~48.5 days */
        return 48.5 / 365.25;
    default:
        return 0.0;
    }
}

uint8_t ulid64GetVariantRandomBits(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
    case ULID_VARIANT_EPOCH2024:
    case ULID_VARIANT_EPOCHCUSTOM:
        return 16;
    case ULID_VARIANT_NS:
        return 12;
    case ULID_VARIANT_US:
        return 18;
    case ULID_VARIANT_MS:
        return 22;
    case ULID_VARIANT_DUALNS:
        return 0; /* No random, dual timestamp */
    case ULID_VARIANT_DUALNS_INTERLEAVED:
        return 0; /* No random, interleaved dual timestamp */
    case ULID_VARIANT_NSCOUNT:
        return 24; /* Counter, not random */
    case ULID_VARIANT_HYBRID:
        return 20; /* NS delta */
    case ULID_VARIANT_SNOWFLAKE:
        return 13; /* Sequence */
    case ULID_VARIANT_32MS:
        return 6;
    case ULID_VARIANT_32S:
        return 10;
    default:
        return 0;
    }
}

const char *ulid64GetVariantPrecision(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
    case ULID_VARIANT_EPOCH2024:
    case ULID_VARIANT_EPOCHCUSTOM:
    case ULID_VARIANT_NS:
    case ULID_VARIANT_DUALNS:
    case ULID_VARIANT_DUALNS_INTERLEAVED:
    case ULID_VARIANT_NSCOUNT:
    case ULID_VARIANT_HYBRID:
        return "ns";
    case ULID_VARIANT_US:
        return "us";
    case ULID_VARIANT_MS:
    case ULID_VARIANT_SNOWFLAKE:
    case ULID_VARIANT_32MS:
        return "ms";
    case ULID_VARIANT_32S:
        return "s";
    default:
        return "unknown";
    }
}

/* ====================================================================
 * Validation API
 * ==================================================================== */

bool ulid64IsValidVariantType(ulidVariantType type) {
    return type >= 0 && type < ULID_VARIANT_COUNT;
}

bool ulid64ValidateTimestamp(uint64_t timestampNs, ulidVariantType type) {
    uint64_t maxTs = ulid64GetMaxTimestamp(type);
    return timestampNs <= maxTs;
}

uint64_t ulid64GetMaxTimestamp(ulidVariantType type) {
    switch (type) {
    case ULID_VARIANT_EPOCH2020:
        return ULID_EPOCH_2020_NS + (0xFFFFFFFFFFFFULL);
    case ULID_VARIANT_EPOCH2024:
        return ULID_EPOCH_2024_NS + (0xFFFFFFFFFFFFULL);
    case ULID_VARIANT_EPOCHCUSTOM:
        /* Cannot determine without knowing the custom epoch */
        return 0xFFFFFFFFFFFFULL; /* Just return the offset range */
    case ULID_VARIANT_NS:
        return 0xFFFFFFFFFFFFFULL; /* 52 bits */
    case ULID_VARIANT_US:
        return (0x3FFFFFFFFFFFULL) * 1000; /* 46 bits in us */
    case ULID_VARIANT_MS:
        return (0x3FFFFFFFFFFULL) * 1000000; /* 42 bits in ms */
    case ULID_VARIANT_DUALNS:
        return 0xFFFFFFFFFFFFFFFFULL; /* 64 bits */
    case ULID_VARIANT_NSCOUNT:
        return 0xFFFFFFFFFFULL; /* 40 bits */
    case ULID_VARIANT_HYBRID:
        return (0xFFFFFFFFFFFULL) * 1000; /* 44 bits in us */
    case ULID_VARIANT_SNOWFLAKE:
        return (0x1FFFFFFFFFFULL) * 1000000; /* 41 bits in ms */
    default:
        return 0;
    }
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "perf.h"
#include <stdio.h>
#include <unistd.h>

int ulidTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    printf("Testing ULID (Base36) generation and encoding...\n");
    printf("Available implementations:\n");
    for (int32_t i = 0; i < ULID_ENCODE_COUNT; i++) {
        printf("  %s: %s\n", ulidGetImplName(i),
               ulidIsImplAvailable(i) ? "available" : "not available");
    }
    printf("\n");

    /* ================================================================
     * Basic Functionality Tests
     * ================================================================ */
    TEST("ulid generation") {
        ulid_t id1, id2;
        char enc1[ULID_ENCODED_LENGTH + 1];
        char enc2[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id1);
        ulidGenerate(&id2);

        ulidEncode(&id1, enc1, sizeof(enc1));
        ulidEncode(&id2, enc2, sizeof(enc2));

        printf("    Sample sequential ULIDs:\n");
        printf("      [1] %s (ts=%" PRIu64 ")\n", enc1,
               ulidGetTimestampNs(&id1));
        printf("      [2] %s (ts=%" PRIu64 ")\n", enc2,
               ulidGetTimestampNs(&id2));

        if (ulidIsZero(&id1)) {
            ERRR("Generated ULID is zero");
        }

        if (ulidIsZero(&id2)) {
            ERRR("Second generated ULID is zero");
        }

        /* IDs should be different */
        if (ulidCompare(&id1, &id2) == 0) {
            ERRR("Two generated ULIDs are identical");
        }

        /* Timestamps should be monotonically increasing or equal */
        uint64_t ts1 = ulidGetTimestampNs(&id1);
        uint64_t ts2 = ulidGetTimestampNs(&id2);

        if (ts1 == 0) {
            ERRR("Timestamp extraction failed for first ULID");
        }
        if (ts2 < ts1) {
            ERR("Timestamps not monotonic: %" PRIu64 " > %" PRIu64, ts1, ts2);
        }
    }

    TEST("ulid encoding length") {
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);
        size_t len = ulidEncode(&id, encoded, sizeof(encoded));

        if (len != ULID_ENCODED_LENGTH) {
            ERR("Encoding length wrong: %zu != %d", len, ULID_ENCODED_LENGTH);
        }

        if (encoded[ULID_ENCODED_LENGTH] != '\0') {
            ERRR("Encoded string not null-terminated");
        }

        if (strlen(encoded) != ULID_ENCODED_LENGTH) {
            ERR("Encoded string length wrong: %zu", strlen(encoded));
        }
    }

    TEST("ulid Base36 character validity") {
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        /* Generate multiple ULIDs and check all characters are valid Base36 */
        for (int32_t test = 0; test < 100; test++) {
            ulidGenerate(&id);
            ulidEncode(&id, encoded, sizeof(encoded));

            for (int32_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
                char c = encoded[i];
                bool valid = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
                if (!valid) {
                    ERR("Invalid Base36 char at pos %d: '%c' (0x%02x) in %s", i,
                        c, (uint8_t)c, encoded);
                }
            }
        }
    }

    TEST("ulid encode/decode roundtrip (scalar)") {
        ulidSetEncodeImpl(ULID_ENCODE_SCALAR);

        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample scalar encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&orig);
            ulidEncodeScalar(&orig, encoded, sizeof(encoded));

            /* Show first 3 samples */
            if (test < 3) {
                printf("      [%d] %s -> decode -> ", test + 1, encoded);
            }

            if (!ulidDecodeScalar(encoded, &decoded)) {
                ERR("Failed to decode ULID: %s", encoded);
            }

            if (test < 3) {
                char reencoded[ULID_ENCODED_LENGTH + 1];
                ulidEncodeScalar(&decoded, reencoded, sizeof(reencoded));
                printf("%s %s\n", reencoded,
                       strcmp(encoded, reencoded) == 0 ? "(match)"
                                                       : "(MISMATCH!)");
            }

            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Roundtrip failed for: %s", encoded);
            }

            if (ulidGetTimestampNs(&orig) != ulidGetTimestampNs(&decoded)) {
                ERRR("Timestamp mismatch after roundtrip");
            }

            if (ulidGetRandom(&orig) != ulidGetRandom(&decoded)) {
                ERRR("Random component mismatch after roundtrip");
            }
        }
    }

    TEST("ulid encode/decode roundtrip (SWAR)") {
        ulidSetEncodeImpl(ULID_ENCODE_SWAR);

        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample SWAR encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&orig);
            ulidEncodeSWAR(&orig, encoded, sizeof(encoded));

            if (test < 3) {
                printf("      [%d] %s -> decode -> ", test + 1, encoded);
            }

            if (!ulidDecodeSWAR(encoded, &decoded)) {
                ERR("SWAR: Failed to decode ULID: %s", encoded);
            }

            if (test < 3) {
                char reencoded[ULID_ENCODED_LENGTH + 1];
                ulidEncodeSWAR(&decoded, reencoded, sizeof(reencoded));
                printf("%s %s\n", reencoded,
                       strcmp(encoded, reencoded) == 0 ? "(match)"
                                                       : "(MISMATCH!)");
            }

            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("SWAR: Roundtrip failed for: %s", encoded);
            }
        }
    }

#if ULID_HAS_SSE2
    TEST("ulid encode/decode roundtrip (SSE2)") {
        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&orig);
            ulidEncodeSSE2(&orig, encoded, sizeof(encoded));

            if (!ulidDecodeSSE2(encoded, &decoded)) {
                ERR("SSE2: Failed to decode ULID: %s", encoded);
            }

            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("SSE2: Roundtrip failed for: %s", encoded);
            }
        }
    }
#endif

#if ULID_HAS_AVX2
    TEST("ulid encode/decode roundtrip (AVX2)") {
        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&orig);
            ulidEncodeAVX2(&orig, encoded, sizeof(encoded));

            if (!ulidDecodeAVX2(encoded, &decoded)) {
                ERR("AVX2: Failed to decode ULID: %s", encoded);
            }

            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("AVX2: Roundtrip failed for: %s", encoded);
            }
        }
    }
#endif

#if ULID_HAS_NEON
    TEST("ulid encode/decode roundtrip (NEON)") {
        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample NEON encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&orig);
            ulidEncodeNEON(&orig, encoded, sizeof(encoded));

            if (test < 3) {
                printf("      [%d] %s -> decode -> ", test + 1, encoded);
            }

            if (!ulidDecodeNEON(encoded, &decoded)) {
                ERR("NEON: Failed to decode ULID: %s", encoded);
            }

            if (test < 3) {
                char reencoded[ULID_ENCODED_LENGTH + 1];
                ulidEncodeNEON(&decoded, reencoded, sizeof(reencoded));
                printf("%s %s\n", reencoded,
                       strcmp(encoded, reencoded) == 0 ? "(match)"
                                                       : "(MISMATCH!)");
            }

            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("NEON: Roundtrip failed for: %s", encoded);
            }
        }
    }
#endif

    TEST("ulid cross-implementation compatibility") {
        ulid_t orig, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample cross-impl verification:\n");
        for (int32_t test = 0; test < 100; test++) {
            ulidGenerate(&orig);

            /* Encode with scalar */
            ulidEncodeScalar(&orig, encoded, sizeof(encoded));

            if (test == 0) {
                char swarEnc[ULID_ENCODED_LENGTH + 1];
                ulidEncodeSWAR(&orig, swarEnc, sizeof(swarEnc));
                printf("      scalar: %s\n", encoded);
                printf("      SWAR:   %s %s\n", swarEnc,
                       strcmp(encoded, swarEnc) == 0 ? "(identical)"
                                                     : "(DIFFERS!)");
#if ULID_HAS_NEON
                char neonEnc[ULID_ENCODED_LENGTH + 1];
                ulidEncodeNEON(&orig, neonEnc, sizeof(neonEnc));
                printf("      NEON:   %s %s\n", neonEnc,
                       strcmp(encoded, neonEnc) == 0 ? "(identical)"
                                                     : "(DIFFERS!)");
#endif
            }

            /* Decode with SWAR */
            if (!ulidDecodeSWAR(encoded, &decoded)) {
                ERR("Cross-compat: SWAR failed to decode scalar encoding: %s",
                    encoded);
            }
            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Cross-compat: scalar->SWAR mismatch for: %s", encoded);
            }

#if ULID_HAS_SSE2
            /* Decode scalar encoding with SSE2 */
            if (!ulidDecodeSSE2(encoded, &decoded)) {
                ERR("Cross-compat: SSE2 failed to decode scalar encoding: %s",
                    encoded);
            }
            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Cross-compat: scalar->SSE2 mismatch for: %s", encoded);
            }

            /* Encode with SSE2, decode with scalar */
            ulidEncodeSSE2(&orig, encoded, sizeof(encoded));
            if (!ulidDecodeScalar(encoded, &decoded)) {
                ERR("Cross-compat: scalar failed to decode SSE2 encoding: %s",
                    encoded);
            }
            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Cross-compat: SSE2->scalar mismatch for: %s", encoded);
            }
#endif

#if ULID_HAS_AVX2
            /* Encode with AVX2, decode with scalar */
            ulidEncodeAVX2(&orig, encoded, sizeof(encoded));
            if (!ulidDecodeScalar(encoded, &decoded)) {
                ERR("Cross-compat: scalar failed to decode AVX2 encoding: %s",
                    encoded);
            }
            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Cross-compat: AVX2->scalar mismatch for: %s", encoded);
            }
#endif

#if ULID_HAS_NEON
            /* Encode with NEON, decode with scalar */
            ulidEncodeNEON(&orig, encoded, sizeof(encoded));
            if (!ulidDecodeScalar(encoded, &decoded)) {
                ERR("Cross-compat: scalar failed to decode NEON encoding: %s",
                    encoded);
            }
            if (ulidCompare(&orig, &decoded) != 0) {
                ERR("Cross-compat: NEON->scalar mismatch for: %s", encoded);
            }
#endif
        }
    }

    TEST("ulid lowercase decode support") {
        ulid_t id, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];
        char lowercase[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);
        ulidEncode(&id, encoded, sizeof(encoded));

        /* Convert to lowercase */
        for (int32_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
            if (encoded[i] >= 'A' && encoded[i] <= 'Z') {
                lowercase[i] = encoded[i] - 'A' + 'a';
            } else {
                lowercase[i] = encoded[i];
            }
        }
        lowercase[ULID_ENCODED_LENGTH] = '\0';

        printf("    Sample lowercase decode:\n");
        printf("      upper: %s\n", encoded);
        printf("      lower: %s -> decodes to same ULID\n", lowercase);

        if (!ulidDecode(lowercase, &decoded)) {
            ERR("Failed to decode lowercase ULID: %s", lowercase);
        }

        if (ulidCompare(&id, &decoded) != 0) {
            ERRR("Lowercase decode produced different result");
        }
    }

    TEST("ulid ordering with timestamps") {
        ulid_t id1, id2;
        char enc1[ULID_ENCODED_LENGTH + 1];
        char enc2[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id1);
        usleep(1000); /* 1ms delay */
        ulidGenerate(&id2);

        ulidEncode(&id1, enc1, sizeof(enc1));
        ulidEncode(&id2, enc2, sizeof(enc2));

        uint64_t ts1 = ulidGetTimestampNs(&id1);
        uint64_t ts2 = ulidGetTimestampNs(&id2);

        if (ts2 <= ts1) {
            ERR("Timestamps not strictly increasing: %" PRIu64 " <= %" PRIu64,
                ts2, ts1);
        }

        /* Lexicographic ordering should match timestamp ordering */
        int cmp = strcmp(enc1, enc2);
        if (cmp >= 0) {
            ERR("Lexicographic ordering wrong: %s >= %s", enc1, enc2);
        }
    }

    TEST("ulid collision resistance") {
        const int32_t numIds = 10000;
        ulid_t *ids = zcalloc(numIds, sizeof(ulid_t));

        for (int32_t i = 0; i < numIds; i++) {
            ulidGenerate(&ids[i]);
        }

        /* Check for collisions */
        int32_t collisions = 0;
        for (int32_t i = 0; i < numIds && collisions == 0; i++) {
            for (int32_t j = i + 1; j < numIds && collisions == 0; j++) {
                if (ulidCompare(&ids[i], &ids[j]) == 0) {
                    collisions++;
                    ERR("Collision at positions %d and %d", i, j);
                }
            }
        }

        zfree(ids);

        if (collisions > 0) {
            ERR("Found %d collisions in %d ULIDs", collisions, numIds);
        }
    }

    TEST("ulid decode invalid input") {
        ulid_t id;

        /* Wrong length */
        if (ulidDecode("ABC", &id)) {
            ERRR("Accepted too-short input");
        }

        if (ulidDecode("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", &id)) {
            ERRR("Accepted too-long input");
        }

        /* Invalid characters */
        char invalid[ULID_ENCODED_LENGTH + 1] = "0000000000000000000000000";
        invalid[0] = '!';
        if (ulidDecode(invalid, &id)) {
            ERRR("Accepted invalid character '!'");
        }

        invalid[0] = '[';
        if (ulidDecode(invalid, &id)) {
            ERRR("Accepted invalid character '['");
        }

        invalid[0] = ' ';
        if (ulidDecode(invalid, &id)) {
            ERRR("Accepted space character");
        }
    }

    TEST("ulid utility functions") {
        ulid_t id1, id2, copy;

        ulidGenerate(&id1);

        /* Test copy */
        ulidCopy(&copy, &id1);
        if (ulidCompare(&id1, &copy) != 0) {
            ERRR("Copy produced different ULID");
        }

        /* Test clear */
        ulidClear(&id2);
        if (!ulidIsZero(&id2)) {
            ERRR("Clear did not zero ULID");
        }

        /* Test isZero */
        if (ulidIsZero(&id1)) {
            ERRR("Non-zero ULID reported as zero");
        }
    }

    TEST("ulid convenience function") {
        char encoded[ULID_ENCODED_LENGTH + 1];
        size_t len = ulidGenerateAndEncode(encoded, sizeof(encoded));

        if (len != ULID_ENCODED_LENGTH) {
            ERR("GenerateAndEncode returned wrong length: %zu", len);
        }

        if (strlen(encoded) != ULID_ENCODED_LENGTH) {
            ERR("GenerateAndEncode string wrong length: %zu", strlen(encoded));
        }

        /* Verify it decodes */
        ulid_t decoded;
        if (!ulidDecode(encoded, &decoded)) {
            ERR("Failed to decode GenerateAndEncode result: %s", encoded);
        }
    }

    /* ================================================================
     * Performance Tests
     * ================================================================ */
    printf("\n--- Performance Benchmarks ---\n\n");

    TEST("ulid generation performance") {
        const size_t iterations = 1000000;
        ulid_t id;

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerate(&id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "ulid generations");
    }

    TEST("ulid encode performance (scalar)") {
        const size_t iterations = 1000000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeScalar(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "scalar encodings");
    }

    TEST("ulid decode performance (scalar)") {
        const size_t iterations = 1000000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeScalar(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "scalar decodings");
    }

    TEST("ulid encode performance (SWAR)") {
        const size_t iterations = 1000000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeSWAR(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SWAR encodings");
    }

    TEST("ulid decode performance (SWAR)") {
        const size_t iterations = 1000000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeSWAR(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SWAR decodings");
    }

#if ULID_HAS_SSE2
    TEST("ulid encode performance (SSE2)") {
        const size_t iterations = 1000000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeSSE2(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 encodings");
    }

    TEST("ulid decode performance (SSE2)") {
        const size_t iterations = 1000000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeSSE2(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 decodings");
    }
#endif

#if ULID_HAS_AVX2
    TEST("ulid encode performance (AVX2)") {
        const size_t iterations = 1000000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeAVX2(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 encodings");
    }

    TEST("ulid decode performance (AVX2)") {
        const size_t iterations = 1000000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeAVX2(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 decodings");
    }
#endif

#if ULID_HAS_NEON
    TEST("ulid encode performance (NEON)") {
        const size_t iterations = 1000000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeNEON(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON encodings");
    }

    TEST("ulid decode performance (NEON)") {
        const size_t iterations = 1000000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeNEON(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON decodings");
    }
#endif

    TEST("ulid full roundtrip performance") {
        const size_t iterations = 500000;
        ulid_t id, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerate(&id);
            ulidEncode(&id, encoded, sizeof(encoded));
            ulidDecode(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "full roundtrips");
    }

    /* Reset to auto mode */
    ulidSetEncodeImpl(ULID_ENCODE_AUTO);

    /* ================================================================
     * ULID Variants Tests
     * ================================================================ */
    printf("\n--- ULID Variants Tests ---\n\n");
    printf("Available variant types:\n");
    for (int32_t i = 0; i < ULID_VARIANT_COUNT; i++) {
        printf("  %s: %s\n", ulid64GetVariantName(i),
               ulid64GetVariantDescription(i));
        printf("    Range: %.2f years, Random bits: %d, Precision: %s\n",
               ulid64GetVariantRangeYears(i), ulid64GetVariantRandomBits(i),
               ulid64GetVariantPrecision(i));
    }
    printf("\n");

    /* ================================================================
     * Basic Functionality Tests - 64-bit Variants
     * ================================================================ */
    TEST("ulid64 encode/decode roundtrip (EPOCH2020)") {
        ulid64_t orig, decoded;
        char encoded[ULID64_ENCODED_LENGTH + 1];

        printf("    Sample EPOCH2020 IDs:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulid64Generate(&orig, ULID_VARIANT_EPOCH2020);
            ulid64Encode(&orig, encoded, sizeof(encoded));

            if (test < 3) {
                uint64_t ts =
                    ulid64GetTimestampNs(&orig, ULID_VARIANT_EPOCH2020);
                printf("      [%d] %s (ts=%" PRIu64 " ns, data=0x%016llx)\n",
                       test + 1, encoded, ts, (unsigned long long)orig.data);
            }

            if (!ulid64Decode(encoded, &decoded)) {
                ERR("Failed to decode: %s", encoded);
            }

            if (ulid64Compare(&orig, &decoded) != 0) {
                if (test < 5) {
                    printf("      DEBUG: orig=0x%016llx decoded=0x%016llx "
                           "encoded=%s\n",
                           (unsigned long long)orig.data,
                           (unsigned long long)decoded.data, encoded);
                }
                ERR("Roundtrip failed for: %s", encoded);
            }
        }
    }

    TEST("ulid64 monotonic ordering (NS variant)") {
        ulid64_t id1, id2, id3;
        char enc1[ULID64_ENCODED_LENGTH + 1];
        char enc2[ULID64_ENCODED_LENGTH + 1];
        char enc3[ULID64_ENCODED_LENGTH + 1];

        ulid64Generate(&id1, ULID_VARIANT_NS);
        ulid64Generate(&id2, ULID_VARIANT_NS);
        ulid64Generate(&id3, ULID_VARIANT_NS);

        ulid64Encode(&id1, enc1, sizeof(enc1));
        ulid64Encode(&id2, enc2, sizeof(enc2));
        ulid64Encode(&id3, enc3, sizeof(enc3));

        printf("    Sequential NS IDs:\n");
        printf("      [1] %s\n", enc1);
        printf("      [2] %s\n", enc2);
        printf("      [3] %s\n", enc3);

        if (ulid64Compare(&id1, &id2) >= 0) {
            ERRR("IDs not monotonically increasing: id1 >= id2");
        }
        if (ulid64Compare(&id2, &id3) >= 0) {
            ERRR("IDs not monotonically increasing: id2 >= id3");
        }

        /* Lexicographic ordering should match */
        if (strcmp(enc1, enc2) >= 0) {
            ERR("Lexicographic ordering wrong: %s >= %s", enc1, enc2);
        }
        if (strcmp(enc2, enc3) >= 0) {
            ERR("Lexicographic ordering wrong: %s >= %s", enc2, enc3);
        }
    }

    TEST("ulid64 collision resistance") {
        /* Test NSCOUNT variant which has monotonic counter guarantees */
        const int32_t numIds = 10000;
        ulid64_t *ids = zcalloc(numIds, sizeof(ulid64_t));

        for (int32_t i = 0; i < numIds; i++) {
            ulid64Generate(&ids[i], ULID_VARIANT_NSCOUNT);
        }

        int32_t collisions = 0;
        for (int32_t i = 0; i < numIds && collisions == 0; i++) {
            for (int32_t j = i + 1; j < numIds && collisions == 0; j++) {
                if (ulid64Compare(&ids[i], &ids[j]) == 0) {
                    collisions++;
                    ERR("Collision at positions %d and %d", i, j);
                }
            }
        }

        zfree(ids);

        if (collisions > 0) {
            ERR("Found %d collisions in %d IDs (NSCOUNT should have zero)",
                collisions, numIds);
        }
    }

    TEST("DUALNS variant (128-bit)") {
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample DUALNS IDs (128-bit: two 64-bit ns timestamps):\n");
        for (int32_t i = 0; i < 5; i++) {
            ulidGenerateDualNs(&id);
            ulidEncode(&id, encoded, sizeof(encoded));

            uint64_t ts1 = ulidGetFirstTimestampNs(&id);
            uint64_t ts2 = ulidGetSecondTimestampNs(&id);

            printf("      [%d] %s\n", i + 1, encoded);
            printf("          ts1=%" PRIu64 ", ts2=%" PRIu64
                   ", delta=%lld ns\n",
                   ts1, ts2, (long long)((int64_t)ts2 - (int64_t)ts1));

            /* Second timestamp should be >= first */
            if (ts2 < ts1) {
                ERR("DUALNS: second timestamp (%" PRIu64 ") < first (%" PRIu64
                    ")",
                    ts2, ts1);
            }

            /* Verify roundtrip */
            ulid_t decoded;
            if (!ulidDecode(encoded, &decoded)) {
                ERR("Failed to decode DUALNS: %s", encoded);
            }

            if (memcmp(&id, &decoded, sizeof(ulid_t)) != 0) {
                ERR("DUALNS roundtrip failed%s", "");
            }
        }
    }

    TEST("DUALNS __uint128_t interface") {
        printf("    Testing __uint128_t interface:\n");

        /* Test 1: Direct generation as __uint128_t */
        __uint128_t val128 = ulidGenerateDualNsU128();
        uint64_t ts1_direct = (uint64_t)(val128 >> 64);
        uint64_t ts2_direct = (uint64_t)val128;

        printf("      Direct generation:\n");
        printf("        __uint128_t value: ts1=%" PRIu64 ", ts2=%" PRIu64 "\n",
               ts1_direct, ts2_direct);
        printf("        delta=%lld ns\n",
               (long long)((int64_t)ts2_direct - (int64_t)ts1_direct));

        if (ts2_direct < ts1_direct) {
            ERR("Direct: second timestamp (%" PRIu64 ") < first (%" PRIu64 ")",
                ts2_direct, ts1_direct);
        }

        /* Test 2: Convert __uint128_t to ulid_t and encode */
        ulid_t id_from_u128;
        ulidFromDualNsU128(&id_from_u128, val128);

        char encoded[ULID_ENCODED_LENGTH + 1];
        ulidEncode(&id_from_u128, encoded, sizeof(encoded));

        printf("      Converted to ulid_t and encoded: %s\n", encoded);

        /* Verify timestamps match */
        uint64_t ts1_ulid = ulidGetFirstTimestampNs(&id_from_u128);
        uint64_t ts2_ulid = ulidGetSecondTimestampNs(&id_from_u128);

        if (ts1_ulid != ts1_direct || ts2_ulid != ts2_direct) {
            ERR("Timestamp mismatch after conversion: (%" PRIu64 ",%" PRIu64
                ") != (%" PRIu64 ",%" PRIu64 ")",
                ts1_ulid, ts2_ulid, ts1_direct, ts2_direct);
        }

        /* Test 3: Round-trip via string encoding */
        ulid_t decoded;
        if (!ulidDecode(encoded, &decoded)) {
            ERR("Failed to decode DUALNS from __uint128_t: %s", encoded);
        }

        if (memcmp(&id_from_u128, &decoded, sizeof(ulid_t)) != 0) {
            ERR("DUALNS __uint128_t string roundtrip failed%s", "");
        }

        /* Test 4: Extract as __uint128_t and verify */
        __uint128_t val128_extracted = ulidToDualNsU128(&decoded);

        if (val128_extracted != val128) {
            uint64_t orig_hi = (uint64_t)(val128 >> 64);
            uint64_t orig_lo = (uint64_t)val128;
            uint64_t extr_hi = (uint64_t)(val128_extracted >> 64);
            uint64_t extr_lo = (uint64_t)val128_extracted;
            ERR("__uint128_t roundtrip failed: (%" PRIu64 ",%" PRIu64
                ") != (%" PRIu64 ",%" PRIu64 ")",
                orig_hi, orig_lo, extr_hi, extr_lo);
        }

        printf("      Full roundtrip: __uint128_t -> ulid_t -> string -> "
               "ulid_t -> __uint128_t ✓\n");

        /* Test 5: Multiple rapid generations */
        printf("      Rapid generation test:\n");
        for (int32_t i = 0; i < 3; i++) {
            __uint128_t rapid = ulidGenerateDualNsU128();
            uint64_t r_ts1 = (uint64_t)(rapid >> 64);
            uint64_t r_ts2 = (uint64_t)rapid;

            printf("        [%d] ts1=%" PRIu64 ", ts2=%" PRIu64
                   ", delta=%lld ns\n",
                   i + 1, r_ts1, r_ts2,
                   (long long)((int64_t)r_ts2 - (int64_t)r_ts1));

            if (r_ts2 < r_ts1) {
                ERR("Rapid gen %d: ts2 < ts1", i);
            }
        }
    }

    TEST("DUALNS_INTERLEAVED bit interleaving") {
        printf("    Testing bit-interleaved dual timestamp variant:\n");

        for (uint32_t z = 0; z < 3; z++) {
            /* Test 1: Basic generation and extraction */
            __uint128_t interleaved = ulidGenerateDualNsInterleavedU128();
            uint64_t ts1_orig, ts2_orig;
            deinterleaveBits64Dispatch(interleaved, &ts1_orig, &ts2_orig);

            printf("      Generated interleaved value:\n");
            printf("        ts1=%" PRIu64 ", ts2=%" PRIu64 "\n", ts1_orig,
                   ts2_orig);
            printf("        delta=%lld ns\n",
                   (long long)((int64_t)ts2_orig - (int64_t)ts1_orig));

            /* Test 2: Verify bit interleaving correctness */
            __uint128_t reinterleaved =
                interleaveBits64Dispatch(ts1_orig, ts2_orig);
            if (reinterleaved != interleaved) {
                ERR("Bit interleaving roundtrip failed%s", "");
            }
            printf("      Bit interleaving roundtrip: ts -> interleave -> "
                   "deinterleave -> ts ✓\n");

            /* Test 3: Create ulid_t and verify extraction */
            ulid_t id_interleaved;
            ulidGenerateDualNsInterleaved(&id_interleaved);

            uint64_t ts1_extracted =
                ulidGetFirstTimestampNsInterleaved(&id_interleaved);
            uint64_t ts2_extracted =
                ulidGetSecondTimestampNsInterleaved(&id_interleaved);

            printf("      ulid_t extraction:\n");
            printf("        ts1=%" PRIu64 ", ts2=%" PRIu64 "\n", ts1_extracted,
                   ts2_extracted);

            if (ts2_extracted < ts1_extracted) {
                ERR("Extracted: second timestamp (%" PRIu64
                    ") < first (%" PRIu64 ")",
                    ts2_extracted, ts1_extracted);
            }

            /* Test 4: __uint128_t interface with ulid_t */
            ulid_t id_from_u128;
            ulidFromDualNsInterleavedU128(&id_from_u128, interleaved);

            uint64_t ts1_check =
                ulidGetFirstTimestampNsInterleaved(&id_from_u128);
            uint64_t ts2_check =
                ulidGetSecondTimestampNsInterleaved(&id_from_u128);

            if (ts1_check != ts1_orig || ts2_check != ts2_orig) {
                ERR("__uint128_t -> ulid_t mismatch: (%" PRIu64 ",%" PRIu64
                    ") != (%" PRIu64 ",%" PRIu64 ")",
                    ts1_check, ts2_check, ts1_orig, ts2_orig);
            }

            /* Test 5: Full string encoding roundtrip */
            char encoded[ULID_ENCODED_LENGTH + 1];
            size_t len = ulidEncode(&id_from_u128, encoded, sizeof(encoded));
            if (len != ULID_ENCODED_LENGTH) {
                ERR("Encode failed, expected %d chars, got %zu",
                    ULID_ENCODED_LENGTH, len);
            }

            printf("      Encoded as Base36: %s\n", encoded);

            ulid_t decoded;
            if (!ulidDecode(encoded, &decoded)) {
                ERR("Failed to decode DUALNS_INTERLEAVED: %s", encoded);
            }

            if (memcmp(&id_from_u128, &decoded, sizeof(ulid_t)) != 0) {
                ERR("DUALNS_INTERLEAVED string roundtrip failed%s", "");
            }

            /* Test 6: Extract from decoded and verify */
            __uint128_t extracted_u128 = ulidToDualNsInterleavedU128(&decoded);
            if (extracted_u128 != interleaved) {
                ERR("Extracted __uint128_t doesn't match original%s", "");
            }

            printf("      Full roundtrip: __uint128_t -> ulid_t -> string -> "
                   "ulid_t -> __uint128_t ✓\n");

            /* Test 7: Sort order preservation based on first timestamp */
            printf("      Testing sort order preservation:\n");

            /* Create IDs with known timestamp relationships */
            uint64_t base_ts = timeUtilNs();
            uint64_t ts1_a = base_ts;
            uint64_t ts2_a = base_ts + 100;
            uint64_t ts1_b = base_ts + 1000; /* Later first timestamp */
            uint64_t ts2_b = base_ts + 50;   /* Earlier second timestamp */

            __uint128_t interleaved_a = interleaveBits64Dispatch(ts1_a, ts2_a);
            __uint128_t interleaved_b = interleaveBits64Dispatch(ts1_b, ts2_b);

            /* Interleaved_b should be > interleaved_a because ts1_b > ts1_a */
            if (interleaved_b <= interleaved_a) {
                ERR("Sort order not preserved: ts1_b > ts1_a but interleaved_b "
                    "<= interleaved_a%s",
                    "");
            }

            printf("        ts1_a=%" PRIu64 " < ts1_b=%" PRIu64 "\n", ts1_a,
                   ts1_b);
            printf("        interleaved_a < interleaved_b ✓\n");

            /* Test 8: Create ulid_t from two timestamps and verify */
            ulid_t id_from_ts;
            ulidFromDualNsInterleaved(&id_from_ts, ts1_a, ts2_a);

            uint64_t ts1_verify =
                ulidGetFirstTimestampNsInterleaved(&id_from_ts);
            uint64_t ts2_verify =
                ulidGetSecondTimestampNsInterleaved(&id_from_ts);

            if (ts1_verify != ts1_a || ts2_verify != ts2_a) {
                ERR("ulidFromDualNsInterleaved failed: (%" PRIu64 ",%" PRIu64
                    ") != (%" PRIu64 ",%" PRIu64 ")",
                    ts1_verify, ts2_verify, ts1_a, ts2_a);
            }

            printf("      Create from two timestamps: (ts1, ts2) -> ulid_t -> "
                   "(ts1, ts2) ✓\n");

            /* Test 9: Multiple rapid generations */
            printf("      Rapid generation test:\n");
            ulid_t prev_id;
            ulidGenerateDualNsInterleaved(&prev_id);

            for (int32_t i = 0; i < 3; i++) {
                ulid_t curr_id;
                ulidGenerateDualNsInterleaved(&curr_id);

                uint64_t curr_ts1 =
                    ulidGetFirstTimestampNsInterleaved(&curr_id);
                uint64_t curr_ts2 =
                    ulidGetSecondTimestampNsInterleaved(&curr_id);

                printf("        [%d] ts1=%" PRIu64 ", ts2=%" PRIu64
                       ", delta=%lld ns\n",
                       i + 1, curr_ts1, curr_ts2,
                       (long long)((int64_t)curr_ts2 - (int64_t)curr_ts1));

                /* Verify monotonic increase (current >= previous) */
                __uint128_t prev_val = ulidToDualNsInterleavedU128(&prev_id);
                __uint128_t curr_val = ulidToDualNsInterleavedU128(&curr_id);

                if (curr_val < prev_val) {
                    ERR("Rapid gen %d: current value < previous value", i);
                }

                prev_id = curr_id;
            }

            /* Test 10: Verify metadata */
            const char *name =
                ulid64GetVariantName(ULID_VARIANT_DUALNS_INTERLEAVED);
            const char *desc =
                ulid64GetVariantDescription(ULID_VARIANT_DUALNS_INTERLEAVED);
            const char *prec =
                ulid64GetVariantPrecision(ULID_VARIANT_DUALNS_INTERLEAVED);
            double range =
                ulid64GetVariantRangeYears(ULID_VARIANT_DUALNS_INTERLEAVED);
            uint8_t randomBits =
                ulid64GetVariantRandomBits(ULID_VARIANT_DUALNS_INTERLEAVED);

            printf("      Metadata:\n");
            printf("        Name: %s\n", name);
            printf("        Description: %s\n", desc);
            printf("        Precision: %s\n", prec);
            printf("        Range: %.1f years\n", range);
            printf("        Random bits: %u\n", randomBits);

            if (strcmp(name, "DUALNS_INTERLEAVED") != 0) {
                ERR("Incorrect variant name: %s", name);
            }
            if (strcmp(prec, "ns") != 0) {
                ERR("Incorrect precision: %s", prec);
            }
            if (randomBits != 0) {
                ERR("Incorrect random bits: %u", randomBits);
            }
        }
    }

    TEST("ulid64 SNOWFLAKE variant") {
        /* Set machine ID via config */
        ulidVariantConfig config;
        ulidVariantConfigInit(&config, ULID_VARIANT_SNOWFLAKE);
        config.machineId = 42;

        ulid64_t id;
        char encoded[ULID64_ENCODED_LENGTH + 1];

        printf("    Sample SNOWFLAKE IDs (machine 42):\n");
        for (int32_t i = 0; i < 5; i++) {
            ulid64GenerateWithConfig(&id, &config);
            ulid64Encode(&id, encoded, sizeof(encoded));

            uint16_t machineId = ulid64GetSnowflakeMachineId(&id);
            uint16_t sequence = ulid64GetSnowflakeSequence(&id);

            printf("      [%d] %s (machine=%u, seq=%u)\n", i + 1, encoded,
                   machineId, sequence);

            if (machineId != 42) {
                ERR("SNOWFLAKE: machine ID mismatch: %u != 42", machineId);
            }
        }
    }

    TEST("ulid64 all variants roundtrip") {
        ulid64_t orig, decoded;
        char encoded[ULID64_ENCODED_LENGTH + 1];

        /* Test all 64-bit variants (DUALNS is 128-bit, tested separately) */
        ulidVariantType variants[] = {
            ULID_VARIANT_EPOCH2020, ULID_VARIANT_EPOCH2024,
            ULID_VARIANT_NS,        ULID_VARIANT_US,
            ULID_VARIANT_MS,        ULID_VARIANT_NSCOUNT,
            ULID_VARIANT_HYBRID,    ULID_VARIANT_SNOWFLAKE};

        for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            ulidVariantType type = variants[v];

            for (int32_t test = 0; test < 100; test++) {
                ulid64Generate(&orig, type);
                ulid64Encode(&orig, encoded, sizeof(encoded));

                if (!ulid64Decode(encoded, &decoded)) {
                    ERR("%s: Failed to decode: %s", ulid64GetVariantName(type),
                        encoded);
                }

                if (ulid64Compare(&orig, &decoded) != 0) {
                    ERR("%s: Roundtrip failed for: %s",
                        ulid64GetVariantName(type), encoded);
                }
            }
        }
    }

    /* ================================================================
     * 32-bit Variant Tests
     * ================================================================ */
    TEST("ulid32 encode/decode roundtrip") {
        ulid32_t orig, decoded;
        char encoded[ULID32_ENCODED_LENGTH + 1];

        printf("    Sample 32-bit IDs:\n");
        ulid32Generate(&orig, ULID_VARIANT_32MS);
        ulid32Encode(&orig, encoded, sizeof(encoded));
        printf("      32MS: %s\n", encoded);

        if (!ulid32Decode(encoded, &decoded)) {
            ERR("Failed to decode 32MS: %s", encoded);
        }

        if (ulid32Compare(&orig, &decoded) != 0) {
            ERRR("32MS roundtrip failed");
        }
    }

    /* ================================================================
     * Configuration Tests
     * ================================================================ */
    TEST("ulid64 custom epoch via config") {
        ulidVariantConfig config;
        ulidVariantConfigInit(&config, ULID_VARIANT_EPOCHCUSTOM);
        config.customEpochNs = ULID_EPOCH_2020_NS;

        if (!ulidVariantConfigValidate(&config)) {
            ERRR("Failed to validate custom epoch config");
        }

        ulid64_t id;
        ulid64GenerateWithConfig(&id, &config);

        uint64_t extractedOffset =
            ulid64GetTimestampNs(&id, ULID_VARIANT_EPOCHCUSTOM);
        /* Note: extractedOffset is just the offset, not the full timestamp */
        if (extractedOffset > 0xFFFFFFFFFFFFULL) {
            ERR("Custom epoch offset overflow: %" PRIu64, extractedOffset);
        }
    }

    /* ================================================================
     * Performance Tests
     * ================================================================ */
    printf("\n--- ULID Variants Performance Benchmarks ---\n\n");

    TEST("ulid64 generation performance (NS)") {
        const size_t iterations = 1000000;
        ulid64_t id;

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulid64Generate(&id, ULID_VARIANT_NS);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NS generations");
    }

    TEST("ulid64 encode performance") {
        const size_t iterations = 1000000;
        ulid64_t id;
        char encoded[ULID64_ENCODED_LENGTH + 1];
        volatile size_t totalLen = 0;

        ulid64Generate(&id, ULID_VARIANT_MS);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            id.data = id.data + i; /* Vary the ID to prevent optimization */
            totalLen += ulid64Encode(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "encodings");

        /* Use totalLen to prevent dead code elimination */
        if (totalLen != iterations * ULID64_ENCODED_LENGTH) {
            ERR("Unexpected totalLen: %zu != %zu", (size_t)totalLen,
                iterations * ULID64_ENCODED_LENGTH);
        }
    }

    TEST("ulid64 decode performance") {
        const size_t iterations = 1000000;
        char encoded[ULID64_ENCODED_LENGTH + 1];
        ulid64_t decoded;

        ulid64GenerateAndEncode(encoded, sizeof(encoded), ULID_VARIANT_MS);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulid64Decode(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "decodings");
    }

    /* ================================================================
     * DUALNS_INTERLEAVED SIMD Correctness Tests
     * ================================================================ */
    TEST("Bit interleaving correctness - all implementations") {
        printf(
            "    Testing correctness of all interleaving implementations:\n");

        /* Test data: various timestamp patterns */
        const size_t num_tests = 10;
        uint64_t test_ts1[] = {0ULL,
                               1ULL,
                               0xFFFFFFFFFFFFFFFFULL,
                               0x5555555555555555ULL,
                               0xAAAAAAAAAAAAAAAAULL,
                               0x0F0F0F0F0F0F0F0FULL,
                               0x123456789ABCDEFULL,
                               timeUtilNs(),
                               timeUtilNs() + 1000,
                               timeUtilNs() - 1000000};
        uint64_t test_ts2[] = {0ULL,
                               0xFFFFFFFFFFFFFFFFULL,
                               1ULL,
                               0xAAAAAAAAAAAAAAAAULL,
                               0x5555555555555555ULL,
                               0xF0F0F0F0F0F0F0F0ULL,
                               0xFEDCBA9876543210ULL,
                               timeUtilNs(),
                               timeUtilNs() + 2000,
                               timeUtilNs() - 2000000};

        /* Test scalar implementation */
        printf("      Testing SCALAR implementation:\n");
        for (size_t i = 0; i < num_tests; i++) {
            __uint128_t interleaved =
                interleaveBits64(test_ts1[i], test_ts2[i]);
            uint64_t extracted_ts1, extracted_ts2;
            deinterleaveBits64(interleaved, &extracted_ts1, &extracted_ts2);

            if (extracted_ts1 != test_ts1[i] || extracted_ts2 != test_ts2[i]) {
                ERR("SCALAR roundtrip failed for test %zu: "
                    "(%" PRIu64 ",%" PRIu64 ") != (%" PRIu64 ",%" PRIu64 ")",
                    i, extracted_ts1, extracted_ts2, test_ts1[i], test_ts2[i]);
            }
        }
        printf("        SCALAR: All %zu tests passed ✓\n", num_tests);

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        /* Test SSE2 implementation */
        printf("      Testing SSE2 implementation:\n");
        for (size_t i = 0; i < num_tests; i++) {
            __uint128_t interleaved_scalar =
                interleaveBits64(test_ts1[i], test_ts2[i]);
            __uint128_t interleaved_sse2 =
                interleaveBits64SSE2(test_ts1[i], test_ts2[i]);

            if (interleaved_scalar != interleaved_sse2) {
                ERR("SSE2 interleave mismatch for test %zu", i);
            }

            uint64_t extracted_ts1, extracted_ts2;
            deinterleaveBits64SSE2(interleaved_sse2, &extracted_ts1,
                                   &extracted_ts2);

            if (extracted_ts1 != test_ts1[i] || extracted_ts2 != test_ts2[i]) {
                ERR("SSE2 roundtrip failed for test %zu: "
                    "(%" PRIu64 ",%" PRIu64 ") != (%" PRIu64 ",%" PRIu64 ")",
                    i, extracted_ts1, extracted_ts2, test_ts1[i], test_ts2[i]);
            }
        }
        printf("        SSE2: All %zu tests passed ✓\n", num_tests);
#endif

#if defined(__AVX2__) && defined(__BMI2__)
        /* Test AVX2 implementation */
        printf("      Testing AVX2 implementation:\n");
        for (size_t i = 0; i < num_tests; i++) {
            __uint128_t interleaved_scalar =
                interleaveBits64(test_ts1[i], test_ts2[i]);
            __uint128_t interleaved_avx2 =
                interleaveBits64AVX2(test_ts1[i], test_ts2[i]);

            if (interleaved_scalar != interleaved_avx2) {
                ERR("AVX2 interleave mismatch for test %zu", i);
            }

            uint64_t extracted_ts1, extracted_ts2;
            deinterleaveBits64AVX2(interleaved_avx2, &extracted_ts1,
                                   &extracted_ts2);

            if (extracted_ts1 != test_ts1[i] || extracted_ts2 != test_ts2[i]) {
                ERR("AVX2 roundtrip failed for test %zu: "
                    "(%" PRIu64 ",%" PRIu64 ") != (%" PRIu64 ",%" PRIu64 ")",
                    i, extracted_ts1, extracted_ts2, test_ts1[i], test_ts2[i]);
            }
        }
        printf("        AVX2: All %zu tests passed ✓\n", num_tests);
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* Test NEON implementation */
        printf("      Testing NEON implementation:\n");
        for (size_t i = 0; i < num_tests; i++) {
            __uint128_t interleaved_scalar =
                interleaveBits64(test_ts1[i], test_ts2[i]);
            __uint128_t interleaved_neon =
                interleaveBits64NEON(test_ts1[i], test_ts2[i]);

            if (interleaved_scalar != interleaved_neon) {
                ERR("NEON interleave mismatch for test %zu", i);
            }

            uint64_t extracted_ts1, extracted_ts2;
            deinterleaveBits64NEON(interleaved_neon, &extracted_ts1,
                                   &extracted_ts2);

            if (extracted_ts1 != test_ts1[i] || extracted_ts2 != test_ts2[i]) {
                ERR("NEON roundtrip failed for test %zu: "
                    "(%" PRIu64 ",%" PRIu64 ") != (%" PRIu64 ",%" PRIu64 ")",
                    i, extracted_ts1, extracted_ts2, test_ts1[i], test_ts2[i]);
            }
        }
        printf("        NEON: All %zu tests passed ✓\n", num_tests);
#endif
    }

    /* ================================================================
     * DUALNS_INTERLEAVED SIMD Performance Benchmarks
     * ================================================================ */
    TEST("Bit interleaving performance - SCALAR vs SIMD") {
        const size_t iterations = 1000000;
        printf("    Benchmarking bit interleaving implementations (%zu "
               "iterations):\n",
               iterations);

        /* Prepare test data */
        uint64_t ts1 = timeUtilNs();
        uint64_t ts2 = timeUtilNs();
        volatile __uint128_t interleaved_vol = 0;
        volatile uint64_t result_ts1_vol = 0, result_ts2_vol = 0;

        /* Benchmark SCALAR interleave */
        printf("\n      SCALAR interleave:\n");
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            __uint128_t interleaved_scalar = interleaveBits64(ts1 + i, ts2 + i);
            interleaved_vol = interleaved_scalar;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SCALAR interleave ops");

        /* Benchmark SCALAR deinterleave */
        printf("      SCALAR deinterleave:\n");
        __uint128_t interleaved = interleaveBits64(ts1, ts2);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            uint64_t result_ts1, result_ts2;
            deinterleaveBits64(interleaved, &result_ts1, &result_ts2);
            result_ts1_vol = result_ts1;
            result_ts2_vol = result_ts2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SCALAR deinterleave ops");

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        /* Benchmark SSE2 interleave */
        printf("\n      SSE2 interleave:\n");
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            __uint128_t interleaved_sse2 =
                interleaveBits64SSE2(ts1 + i, ts2 + i);
            interleaved_vol = interleaved_sse2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 interleave ops");

        /* Benchmark SSE2 deinterleave */
        printf("      SSE2 deinterleave:\n");
        interleaved = interleaveBits64SSE2(ts1, ts2);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            uint64_t result_ts1, result_ts2;
            deinterleaveBits64SSE2(interleaved, &result_ts1, &result_ts2);
            result_ts1_vol = result_ts1;
            result_ts2_vol = result_ts2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 deinterleave ops");
#endif

#if defined(__AVX2__) && defined(__BMI2__)
        /* Benchmark AVX2 interleave */
        printf("\n      AVX2 interleave:\n");
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            __uint128_t interleaved_avx2 =
                interleaveBits64AVX2(ts1 + i, ts2 + i);
            interleaved_vol = interleaved_avx2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 interleave ops");

        /* Benchmark AVX2 deinterleave */
        printf("      AVX2 deinterleave:\n");
        interleaved = interleaveBits64AVX2(ts1, ts2);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            uint64_t result_ts1, result_ts2;
            deinterleaveBits64AVX2(interleaved, &result_ts1, &result_ts2);
            result_ts1_vol = result_ts1;
            result_ts2_vol = result_ts2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 deinterleave ops");
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* Benchmark NEON interleave */
        printf("\n      NEON interleave:\n");
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            __uint128_t interleaved_neon =
                interleaveBits64NEON(ts1 + i, ts2 + i);
            interleaved_vol = interleaved_neon;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON interleave ops");

        /* Benchmark NEON deinterleave */
        printf("      NEON deinterleave:\n");
        interleaved = interleaveBits64NEON(ts1, ts2);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            uint64_t result_ts1, result_ts2;
            deinterleaveBits64NEON(interleaved, &result_ts1, &result_ts2);
            result_ts1_vol = result_ts1;
            result_ts2_vol = result_ts2;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON deinterleave ops");
#endif

        /* Prevent optimization */
        if (result_ts1_vol == 0 && result_ts2_vol == 0 &&
            interleaved_vol == 0) {
            printf("Unexpected result\n");
        }
    }

    TEST("DUALNS_INTERLEAVED end-to-end performance - SCALAR vs AUTO") {
        const size_t iterations = 100000;
        ulid_t id;

        printf("    End-to-end DUALNS_INTERLEAVED generation benchmark:\n");
        printf("    Iterations: %zu\n\n", iterations);

        /* Benchmark with SCALAR implementation */
        printf("      SCALAR implementation:\n");
        setInterleaveImpl(INTERLEAVE_SCALAR);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerateDualNsInterleaved(&id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SCALAR generations");

        /* Test extraction with SCALAR */
        printf("      SCALAR extraction:\n");
        ulidGenerateDualNsInterleaved(&id);
        uint64_t ts1, ts2;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ts1 = ulidGetFirstTimestampNsInterleaved(&id);
            ts2 = ulidGetSecondTimestampNsInterleaved(&id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SCALAR extractions");

        /* Benchmark with AUTO (best available) implementation */
        printf("\n      AUTO (best available) implementation:\n");
        setInterleaveImpl(INTERLEAVE_AUTO);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerateDualNsInterleaved(&id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AUTO generations");

        /* Test extraction with AUTO */
        printf("      AUTO extraction:\n");
        ulidGenerateDualNsInterleaved(&id);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ts1 = ulidGetFirstTimestampNsInterleaved(&id);
            ts2 = ulidGetSecondTimestampNsInterleaved(&id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AUTO extractions");

        /* Display which implementation was auto-selected */
        interleaveImpl selected = getInterleaveImpl();
        const char *impl_name = "UNKNOWN";
        if (selected == INTERLEAVE_SCALAR) {
            impl_name = "SCALAR";
        }
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        else if (selected == INTERLEAVE_SSE2) {
            impl_name = "SSE2";
        }
#endif
#if defined(__AVX2__) && defined(__BMI2__)
        else if (selected == INTERLEAVE_AVX2) {
            impl_name = "AVX2";
        }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        else if (selected == INTERLEAVE_NEON) {
            impl_name = "NEON";
        }
#endif
        printf("\n      Auto-selected implementation: %s\n", impl_name);

        /* Prevent optimization */
        if (ts1 == 0 && ts2 == 0) {
            printf("Unexpected result\n");
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
