#include "ulid.h"
#include "dkEndian.h" /* the 64-bit halves of an id are big-endian */
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
static constexpr char BASE36_CHARS[36] = {
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
 * Generator State
 * ==================================================================== */

static void ulidGenSeedRng(ulidGen *g) {
    /* 16 whole bytes: all 128 bits of state from the OS CSPRNG (or its
     * predictable fallback before the kernel pool is ready) */
    uint8_t seed[16];
    getRandomBytes(seed, sizeof(seed));

    uint64_t tempState[2] = {dkLoadBe64(seed), dkLoadBe64(seed + 8)};

    /* xoroshiro128+ requires nonzero state */
    if (tempState[0] == 0) {
        tempState[0] = 1;
    }
    if (tempState[1] == 0) {
        tempState[1] = 1;
    }

    g->rng[0] = tempState[0];
    g->rng[1] = tempState[1];
}

bool ulidGenInit(ulidGen *g, ulidClockSource clock) {
    if (!g) {
        return false;
    }

    *g = (ulidGen){};
    g->clockSource = (uint32_t)clock;
    if (clock == ULID_CLOCK_MONOTONIC_OFFSET) {
        g->clockOffsetNs = timeUtilNs() - timeUtilMonotonicNs();
    }

    ulidGenSeedRng(g);
    return true;
}

ulidGen *ulidGenNew(ulidClockSource clock) {
    /* @own: caller; free=ulidGenFree */
    ulidGen *g = zmalloc(sizeof(*g));
    ulidGenInit(g, clock);
    return g;
}

void ulidGenFree(ulidGen *g) {
    if (g) {
        zfree(g);
    }
}

static uint64_t ulidGenRandom(ulidGen *g) {
    /* {0} generators self-seed on first use */
    if (g->rng[0] == 0 && g->rng[1] == 0) {
        ulidGenSeedRng(g);
    }

    return xoroshiro128plus(g->rng);
}

/* Current Unix-epoch nanoseconds under the generator's clock source */
static uint64_t ulidGenNowNs(const ulidGen *g) {
    return g->clockSource == ULID_CLOCK_MONOTONIC_OFFSET
               ? timeUtilMonotonicNs() + g->clockOffsetNs
               : timeUtilNs();
}

/* Next emission timestamp: strictly greater than every previous one
 * from this generator, so ids sort in generation order even when the
 * clock reads the same instant twice or steps backwards. */
static uint64_t ulidGenNextNs(ulidGen *g) {
    uint64_t now = ulidGenNowNs(g);

    if (now <= g->lastEmittedNs) {
        g->collisionCount++;
        now = g->lastEmittedNs + 1;
        /* Saturate instead of wrapping: epoch-ns reaches UINT64_MAX in
         * the year 2554, and a wrapped watermark would silently break
         * the sort-order guarantee */
        if (now == 0) {
            now = UINT64_MAX;
        }
    } else {
        g->collisionCount = 0;
    }

    g->lastEmittedNs = now;
    return now;
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

/* Multiply 128-bit number by 36 and add a value.
 * Returns true if the result did not fit in 128 bits. */
static bool mul128by36AndAdd(uint8_t *num, uint8_t addVal) {
    uint32_t carry = addVal;

    for (int32_t i = 15; i >= 0; i--) {
        uint32_t product = (uint32_t)num[i] * 36 + carry;
        num[i] = (uint8_t)(product & 0xFF);
        carry = product >> 8;
    }

    return carry != 0;
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

    bool overflow = false;
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        uint8_t val = base36DecodeTable[(uint8_t)str[i]];
        if (val == 0xFF) {
            return false; /* Invalid character */
        }
        overflow |= mul128by36AndAdd(out->data, val);
    }

    /* 25 base36 digits reach past 2^128 */
    return !overflow;
}

/* ====================================================================
 * SWAR (SIMD Within A Register) Implementation
 * ==================================================================== */

/* Four digits per loop pass over the same byte-wise division */

size_t ulidEncodeSWAR(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    /* The 128-bit value spans 25 base36 digits, peeled off by repeated
     * division of the 16 big-endian bytes */
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
    bool overflow = false;
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        uint8_t val = base36DecodeTable[(uint8_t)str[i]];
        overflow |= mul128by36AndAdd(out->data, val);
    }

    /* 25 base36 digits reach past 2^128 */
    return !overflow;
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

    /* Select the appropriate value based on character type; any other
     * byte becomes 0xFF, which the decoder refuses (with no lane selected
     * it would read as '0') */
    __m128i result = _mm_and_si128(is_digit, as_digit);
    result = _mm_or_si128(result, _mm_and_si128(is_upper, as_upper_val));
    result = _mm_or_si128(result, _mm_and_si128(is_lower, as_lower_val));
    const __m128i valid =
        _mm_or_si128(_mm_or_si128(is_digit, is_upper), is_lower);
    result = _mm_or_si128(result, _mm_andnot_si128(valid, _mm_set1_epi8(-1)));

    return result;
}

/* SSE2 helper: parallel value to character conversion */
static inline __m128i base36ValuesToChars_SSE2(__m128i values) {
    /* Values 0-9 -> '0'-'9', values 10-35 -> 'A'-'Z' */
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

    uint8_t values[32] = {};
    _mm_storeu_si128((__m128i *)values, vals1);

    /* Remaining 9 characters (positions 16-24) */
    /* Load overlapping to get last 16 chars, then extract last 9 */
    uint8_t paddedInput[16] = {};
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

    /* Accumulate into 128-bit result; 25 base36 digits reach past 2^128 */
    memset(out->data, 0, 16);
    bool overflow = false;
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        overflow |= mul128by36AndAdd(out->data, values[i]);
    }

    return !overflow;
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

    /* Any byte in none of the three ranges becomes 0xFF, which the
     * decoder refuses (with no lane selected it would read as '0') */
    __m256i result = _mm256_and_si256(is_digit, as_digit);
    result = _mm256_or_si256(result, _mm256_and_si256(is_upper, as_upper_val));
    result = _mm256_or_si256(result, _mm256_and_si256(is_lower, as_lower_val));
    const __m256i valid =
        _mm256_or_si256(_mm256_or_si256(is_digit, is_upper), is_lower);
    result = _mm256_or_si256(result,
                             _mm256_andnot_si256(valid, _mm256_set1_epi8(-1)));

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

    uint8_t digitValues[32] = {}; /* Padded for AVX2 */
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
    uint8_t paddedStr[32] = {};
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

    /* Accumulate into 128-bit result; 25 base36 digits reach past 2^128 */
    memset(out->data, 0, 16);
    bool overflow = false;
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        overflow |= mul128by36AndAdd(out->data, values[i]);
    }

    return !overflow;
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

    /* Select based on masks; any other byte becomes 0xFF, which the
     * decoder refuses (with no lane selected it would read as '0') */
    uint8x16_t result = vandq_u8(is_digit, as_digit);
    result = vorrq_u8(result, vandq_u8(is_upper, as_upper));
    result = vorrq_u8(result, vandq_u8(is_lower, as_lower));
    const uint8x16_t valid = vorrq_u8(vorrq_u8(is_digit, is_upper), is_lower);
    result = vorrq_u8(result, vmvnq_u8(valid));

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

    uint8_t digitValues[32] = {}; /* Padded for alignment */
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

    uint8_t values[32] = {};
    vst1q_u8(values, vals1);

    /* Load and convert remaining 9 characters (positions 16-24) */
    uint8_t paddedInput[16] = {};
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

    /* Accumulate into 128-bit result; 25 base36 digits reach past 2^128 */
    memset(out->data, 0, 16);
    bool overflow = false;
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        overflow |= mul128by36AndAdd(out->data, values[i]);
    }

    return !overflow;
}
#endif /* ULID_HAS_NEON */

/* ====================================================================
 * Core API Implementation
 * ==================================================================== */

void ulidGenerate(ulidGen *g, ulid_t *out) {
    if (!g || !out) {
        return;
    }

    uint64_t timestamp = ulidGenNextNs(g);
    uint64_t randomness = ulidGenRandom(g);

    /* Big-endian: timestamp first, then randomness */
    dkStoreBe64(out->data, timestamp);
    dkStoreBe64(out->data + 8, randomness);
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

    return dkLoadBe64(ulid->data);
}

uint64_t ulidGetRandom(const ulid_t *ulid) {
    if (!ulid) {
        return 0;
    }

    return dkLoadBe64(ulid->data + 8);
}

size_t ulidGenerateAndEncode(ulidGen *g, char *out, size_t outLen) {
    if (!g) {
        return 0;
    }

    ulid_t id;
    ulidGenerate(g, &id);
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

/* Every 64- and 32-bit variant is a time field above a low field (random
 * bits, a counter, a nanosecond delta, or machine + sequence); the time
 * field takes the rest of the word. Widths of the low fields: */
static constexpr int ULID_EPOCH_LOW_BITS = 16;
static constexpr int ULID_NS_LOW_BITS = 12;
static constexpr int ULID_US_LOW_BITS = 18;
static constexpr int ULID_MS_LOW_BITS = 22;
static constexpr int ULID_NSCOUNT_LOW_BITS = 24;
static constexpr int ULID_HYBRID_LOW_BITS = 20;
static constexpr int ULID_SNOWFLAKE_SEQUENCE_BITS = 13;
static constexpr int ULID_SNOWFLAKE_MACHINE_BITS = 10;
static constexpr int ULID_SNOWFLAKE_LOW_BITS =
    ULID_SNOWFLAKE_MACHINE_BITS + ULID_SNOWFLAKE_SEQUENCE_BITS;
static constexpr int ULID_32MS_LOW_BITS = 6;
static constexpr int ULID_32S_LOW_BITS = 10;
static constexpr int ULID_DUALNS_LOW_BITS = 32;

/* The low 'bits' bits of v */
DK_INLINE_ALWAYS uint64_t ulidLow_(uint64_t v, int bits) {
    return v & ((UINT64_C(1) << bits) - 1);
}

/* time above a 'lowBits'-bit low field; time's bits past the word drop */
DK_INLINE_ALWAYS uint64_t ulidPack64_(uint64_t time, uint64_t low,
                                      int lowBits) {
    return (time << lowBits) | ulidLow_(low, lowBits);
}

DK_INLINE_ALWAYS uint32_t ulidPack32_(uint64_t time, uint64_t low,
                                      int lowBits) {
    return (uint32_t)ulidLow_(ulidPack64_(time, low, lowBits), 32);
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

    /* 13 base36 digits reach past 2^64, so refuse values that overflow */
    uint64_t value = 0;
    for (size_t i = 0; i < ULID64_ENCODED_LENGTH; i++) {
        uint8_t digitValue = base36DecodeTable[(uint8_t)str[i]];
        if (digitValue == 0xFF) {
            return false; /* Invalid character */
        }

        if (ckd_mul(&value, value, 36) || ckd_add(&value, value, digitValue)) {
            return false;
        }
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

    /* 7 base36 digits reach past 2^32, so refuse values that overflow */
    uint32_t value = 0;
    for (size_t i = 0; i < ULID32_ENCODED_LENGTH; i++) {
        uint8_t digitValue = base36DecodeTable[(uint8_t)str[i]];
        if (digitValue == 0xFF) {
            return false;
        }

        if (ckd_mul(&value, value, 36) || ckd_add(&value, value, digitValue)) {
            return false;
        }
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
        if (config->machineId >= (1U << ULID_SNOWFLAKE_MACHINE_BITS)) {
            return false; /* Must fit the machine field */
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

void ulid64Generate(ulidGen *g, ulid64_t *out, ulidVariantType type) {
    ulidVariantConfig config;
    ulidVariantConfigInit(&config, type);
    ulid64GenerateWithConfig(g, out, &config);
}

void ulid64GenerateWithConfig(ulidGen *g, ulid64_t *out,
                              const ulidVariantConfig *config) {
    if (!g || !out || !config) {
        return;
    }

    uint64_t timestampNs = ulidGenNextNs(g);
    uint64_t random = ulidGenRandom(g);

    switch (config->type) {
    case ULID_VARIANT_EPOCH2020: {
        /* 48-bit ns offset from 2020 + 16-bit random */
        out->data = ulidPack64_(timestampNs - ULID_EPOCH_2020_NS, random,
                                ULID_EPOCH_LOW_BITS);
        break;
    }

    case ULID_VARIANT_EPOCH2024: {
        /* 48-bit ns offset from 2024 + 16-bit random */
        out->data = ulidPack64_(timestampNs - ULID_EPOCH_2024_NS, random,
                                ULID_EPOCH_LOW_BITS);
        break;
    }

    case ULID_VARIANT_EPOCHCUSTOM: {
        /* 48-bit ns offset from custom epoch + 16-bit random */
        out->data = ulidPack64_(timestampNs - config->customEpochNs, random,
                                ULID_EPOCH_LOW_BITS);
        break;
    }

    case ULID_VARIANT_NS:
        /* 52-bit ns + 12-bit random */
        out->data = ulidPack64_(timestampNs, random, ULID_NS_LOW_BITS);
        break;

    case ULID_VARIANT_US:
        /* 46-bit us + 18-bit random */
        out->data = ulidPack64_(timestampNs / 1000, random, ULID_US_LOW_BITS);
        break;

    case ULID_VARIANT_MS:
        /* 42-bit ms + 22-bit random */
        out->data =
            ulidPack64_(timestampNs / 1'000'000, random, ULID_MS_LOW_BITS);
        break;

    case ULID_VARIANT_DUALNS:
        /* DUALNS should use ulidGenerateDualNs (128-bit), not ulid64Generate */
        out->data = 0;
        break;

    case ULID_VARIANT_NSCOUNT:
        /* 40-bit ns + 24-bit counter: the same-instant burst position
         * from the generator */
        out->data =
            ulidPack64_(timestampNs, g->collisionCount, ULID_NSCOUNT_LOW_BITS);
        break;

    case ULID_VARIANT_HYBRID:
        /* 44-bit us + 20-bit ns delta */
        out->data = ulidPack64_(timestampNs / 1000, timestampNs % 1000,
                                ULID_HYBRID_LOW_BITS);
        break;

    case ULID_VARIANT_SNOWFLAKE: {
        /* 41-bit ms + 10-bit machine + 13-bit sequence */
        uint64_t timestampMs = timestampNs / 1'000'000;

        /* Handle sequence counter.  The ms never goes backwards (emission
         * times strictly increase), and one already past this clock's (see
         * below) is used as is. */
        if (timestampMs <= g->snowflakeLastMs) {
            timestampMs = g->snowflakeLastMs;
            g->snowflakeSeq++;
            if (g->snowflakeSeq >= (1U << ULID_SNOWFLAKE_SEQUENCE_BITS)) {
                /* The ms is out of sequence numbers: take the next ms
                 * now rather than wait for it, and move the emission
                 * watermark there so later ids sort after this one */
                timestampMs++;
                g->snowflakeLastMs = timestampMs;
                g->snowflakeSeq = 0;
                const uint64_t msStart = timestampMs * 1'000'000;
                if (g->lastEmittedNs < msStart) {
                    g->lastEmittedNs = msStart;
                }
            }
        } else {
            g->snowflakeLastMs = timestampMs;
            g->snowflakeSeq = 0;
        }
        const uint64_t machineAndSequence = ulidPack64_(
            config->machineId, g->snowflakeSeq, ULID_SNOWFLAKE_SEQUENCE_BITS);
        out->data = ulidPack64_(timestampMs, machineAndSequence,
                                ULID_SNOWFLAKE_LOW_BITS);
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

void ulid32Generate(ulidGen *g, ulid32_t *out, ulidVariantType type) {
    if (!g || !out) {
        return;
    }

    uint64_t timestampNs = ulidGenNextNs(g);
    uint64_t random = ulidGenRandom(g);

    switch (type) {
    case ULID_VARIANT_32MS:
        /* 26-bit ms + 6-bit random */
        out->data =
            ulidPack32_(timestampNs / 1'000'000, random, ULID_32MS_LOW_BITS);
        break;

    case ULID_VARIANT_32S:
        /* 22-bit s + 10-bit random */
        out->data =
            ulidPack32_(timestampNs / 1'000'000'000, random, ULID_32S_LOW_BITS);
        break;

    default:
        out->data = 0;
        break;
    }
}

/* ====================================================================
 * 128-bit DUALNS Variant Generation
 * ==================================================================== */

void ulidGenerateDualNs(ulidGen *g, ulid_t *out) {
    if (!g || !out) {
        return;
    }

    /* Two sequential emission timestamps; watermark makes ts2 > ts1 */
    uint64_t ts1 = ulidGenNextNs(g);
    uint64_t ts2 = ulidGenNextNs(g);

    /* First timestamp in bytes 0-7, second in 8-15, both big-endian so
     * the bytes sort as the timestamps do */
    dkStoreBe64(out->data, ts1);
    dkStoreBe64(out->data + 8, ts2);
}

uint64_t ulidGetFirstTimestampNs(const ulid_t *id) {
    return id ? dkLoadBe64(id->data) : 0;
}

uint64_t ulidGetSecondTimestampNs(const ulid_t *id) {
    return id ? dkLoadBe64(id->data + 8) : 0;
}

/* ====================================================================
 * 128-bit DUALNS __uint128_t Interface
 * ==================================================================== */

__uint128_t ulidGenerateDualNsU128(ulidGen *g) {
    if (!g) {
        return 0;
    }

    /* Two sequential emission timestamps; watermark makes ts2 > ts1 */
    uint64_t ts1 = ulidGenNextNs(g);
    uint64_t ts2 = ulidGenNextNs(g);

    /* Return as __uint128_t with ts1 in high 64 bits, ts2 in low 64 bits */
    return ((__uint128_t)ts1 << 64) | ts2;
}

void ulidFromDualNsU128(ulid_t *out, __uint128_t value) {
    if (!out) {
        return;
    }

    /* The high 64 bits (first timestamp) in bytes 0-7, the low 64 bits in
     * 8-15 */
    dkStoreBe64(out->data, (uint64_t)(value >> 64));
    dkStoreBe64(out->data + 8, (uint64_t)value);
}

__uint128_t ulidToDualNsU128(const ulid_t *id) {
    if (!id) {
        return 0;
    }

    /* High 64 bits = first timestamp (bytes 0-7), low = second (8-15) */
    return ((__uint128_t)dkLoadBe64(id->data) << 64) | dkLoadBe64(id->data + 8);
}

/* ====================================================================
 * 128-bit DUALNS_INTERLEAVED Bit Interleaving Functions
 * ==================================================================== */

/* Interleave bits from two 64-bit values into a 128-bit value
 * Output bit pattern: ts1[0], ts2[0], ts1[1], ts2[1], ..., ts1[63], ts2[63]
 * The result sorts in Z-order: the most significant bit where either
 * timestamp differs decides, with ts2's bit above ts1's at each position.
 * It is not ordered by ts1 alone. */
static inline __uint128_t interleaveBits64([[maybe_unused]] uint64_t ts1,
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
#if defined(__SSE2__)
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
#ifdef __ARM_NEON
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

#ifdef DATAKIT_TEST
/* Set implementation (for benchmarking) */
static void setInterleaveImpl(interleaveImpl impl) {
    g_interleaveImpl = impl;
}

/* Get current implementation */
static interleaveImpl getInterleaveImpl(void) {
    return g_interleaveImpl;
}
#endif

/* Dispatch wrapper for interleave */
static inline __uint128_t interleaveBits64Dispatch(uint64_t ts1, uint64_t ts2) {
    interleaveImpl impl = g_interleaveImpl;

    if (impl == INTERLEAVE_AUTO) {
#if defined(__AVX2__) && defined(__BMI2__)
        impl = INTERLEAVE_AVX2;
#elifdef __SSE2__
        impl = INTERLEAVE_SSE2;
#elifdef __ARM_NEON
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
#if defined(__SSE2__)
    case INTERLEAVE_SSE2:
        return interleaveBits64SSE2(ts1, ts2);
#endif
#ifdef __ARM_NEON
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
#elifdef __SSE2__
        impl = INTERLEAVE_SSE2;
#elifdef __ARM_NEON
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
#if defined(__SSE2__)
    case INTERLEAVE_SSE2:
        deinterleaveBits64SSE2(interleaved, ts1, ts2);
        break;
#endif
#ifdef __ARM_NEON
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

void ulidGenerateDualNsInterleaved(ulidGen *g, ulid_t *out) {
    if (!g || !out) {
        return;
    }

    /* Two sequential emission timestamps; watermark makes ts2 > ts1 */
    uint64_t ts1 = ulidGenNextNs(g);
    uint64_t ts2 = ulidGenNextNs(g);

    /* Interleave the bits using best available implementation */
    __uint128_t interleaved = interleaveBits64Dispatch(ts1, ts2);

    /* Store in ulid_t (big-endian) */
    for (int i = 0; i < 16; i++) {
        out->data[i] = (interleaved >> ((15 - i) * 8)) & 0xFF;
    }
}

__uint128_t ulidGenerateDualNsInterleavedU128(ulidGen *g) {
    if (!g) {
        return 0;
    }

    uint64_t ts1 = ulidGenNextNs(g);
    uint64_t ts2 = ulidGenNextNs(g);
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
    case ULID_VARIANT_EPOCH2020:
        return ULID_EPOCH_2020_NS + (id->data >> ULID_EPOCH_LOW_BITS);

    case ULID_VARIANT_EPOCH2024:
        return ULID_EPOCH_2024_NS + (id->data >> ULID_EPOCH_LOW_BITS);

    case ULID_VARIANT_EPOCHCUSTOM:
        /* Just the offset: the epoch lives in the generating config, so
         * the caller adds it. */
        return id->data >> ULID_EPOCH_LOW_BITS;

    case ULID_VARIANT_NS:
        return id->data >> ULID_NS_LOW_BITS;

    case ULID_VARIANT_US:
        return (id->data >> ULID_US_LOW_BITS) * 1000;

    case ULID_VARIANT_MS:
        return (id->data >> ULID_MS_LOW_BITS) * 1'000'000;

    case ULID_VARIANT_DUALNS:
        return id->data >> ULID_DUALNS_LOW_BITS; /* first timestamp */

    case ULID_VARIANT_NSCOUNT:
        return id->data >> ULID_NSCOUNT_LOW_BITS;

    case ULID_VARIANT_HYBRID:
        return (id->data >> ULID_HYBRID_LOW_BITS) * 1000 +
               ulidLow_(id->data, ULID_HYBRID_LOW_BITS);

    case ULID_VARIANT_SNOWFLAKE:
        return (id->data >> ULID_SNOWFLAKE_LOW_BITS) * 1'000'000;

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
        return ulidLow_(id->data, ULID_EPOCH_LOW_BITS);

    case ULID_VARIANT_NS:
        return ulidLow_(id->data, ULID_NS_LOW_BITS);

    case ULID_VARIANT_US:
        return ulidLow_(id->data, ULID_US_LOW_BITS);

    case ULID_VARIANT_MS:
        return ulidLow_(id->data, ULID_MS_LOW_BITS);

    case ULID_VARIANT_DUALNS:
        return ulidLow_(id->data, ULID_DUALNS_LOW_BITS); /* 2nd timestamp */

    case ULID_VARIANT_NSCOUNT:
        return ulidLow_(id->data, ULID_NSCOUNT_LOW_BITS); /* Counter */

    case ULID_VARIANT_HYBRID:
        return ulidLow_(id->data, ULID_HYBRID_LOW_BITS); /* ns delta */

    case ULID_VARIANT_SNOWFLAKE:
        /* Machine ID + Sequence */
        return ulidLow_(id->data, ULID_SNOWFLAKE_LOW_BITS);

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
    return (uint32_t)ulidLow_(id->data, ULID_NSCOUNT_LOW_BITS);
}

uint16_t ulid64GetSnowflakeMachineId(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint16_t)ulidLow_(id->data >> ULID_SNOWFLAKE_SEQUENCE_BITS,
                              ULID_SNOWFLAKE_MACHINE_BITS);
}

uint16_t ulid64GetSnowflakeSequence(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint16_t)ulidLow_(id->data, ULID_SNOWFLAKE_SEQUENCE_BITS);
}

uint32_t ulid64GetNsDelta(const ulid64_t *id) {
    if (!id) {
        return 0;
    }
    return (uint32_t)ulidLow_(id->data, ULID_HYBRID_LOW_BITS);
}

/* ====================================================================
 * 32-bit Timestamp Extraction
 * ==================================================================== */

uint64_t ulid32GetTimestampNs(const ulid32_t *id, ulidVariantType type) {
    if (!id) {
        return 0;
    }

    switch (type) {
    case ULID_VARIANT_32MS:
        return (uint64_t)(id->data >> ULID_32MS_LOW_BITS) * 1'000'000;

    case ULID_VARIANT_32S:
        return (uint64_t)(id->data >> ULID_32S_LOW_BITS) * 1'000'000'000;

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
        return (uint32_t)ulidLow_(id->data, ULID_32MS_LOW_BITS);

    case ULID_VARIANT_32S:
        return (uint32_t)ulidLow_(id->data, ULID_32S_LOW_BITS);

    default:
        return 0;
    }
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

size_t ulid64GenerateAndEncode(ulidGen *g, char *out, size_t outLen,
                               ulidVariantType type) {
    if (!g) {
        return 0;
    }

    ulid64_t id;
    ulid64Generate(g, &id, type);
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
        return ULID_EPOCH_LOW_BITS;
    case ULID_VARIANT_NS:
        return ULID_NS_LOW_BITS;
    case ULID_VARIANT_US:
        return ULID_US_LOW_BITS;
    case ULID_VARIANT_MS:
        return ULID_MS_LOW_BITS;
    case ULID_VARIANT_DUALNS:
        return 0; /* No random, dual timestamp */
    case ULID_VARIANT_DUALNS_INTERLEAVED:
        return 0; /* No random, interleaved dual timestamp */
    case ULID_VARIANT_NSCOUNT:
        return ULID_NSCOUNT_LOW_BITS; /* Counter, not random */
    case ULID_VARIANT_HYBRID:
        return ULID_HYBRID_LOW_BITS; /* NS delta */
    case ULID_VARIANT_SNOWFLAKE:
        return ULID_SNOWFLAKE_SEQUENCE_BITS; /* Sequence */
    case ULID_VARIANT_32MS:
        return ULID_32MS_LOW_BITS;
    case ULID_VARIANT_32S:
        return ULID_32S_LOW_BITS;
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
    /* The epoch variants store timestamp - epoch: an earlier timestamp
     * would underflow into a garbage offset. */
    if ((type == ULID_VARIANT_EPOCH2020 && timestampNs < ULID_EPOCH_2020_NS) ||
        (type == ULID_VARIANT_EPOCH2024 && timestampNs < ULID_EPOCH_2024_NS)) {
        return false;
    }
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
        return (0x3FFFFFFFFFFULL) * 1'000'000; /* 42 bits in ms */
    case ULID_VARIANT_DUALNS:
        return 0xFFFFFFFFFFFFFFFFULL; /* 64 bits */
    case ULID_VARIANT_NSCOUNT:
        return 0xFFFFFFFFFFULL; /* 40 bits */
    case ULID_VARIANT_HYBRID:
        return (0xFFFFFFFFFFFULL) * 1000; /* 44 bits in us */
    case ULID_VARIANT_SNOWFLAKE:
        return (0x1FFFFFFFFFFULL) * 1'000'000; /* 41 bits in ms */
    default:
        return 0;
    }
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "perf.h"
#include <stdio.h>
#include <unistd.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(ulid_t *, zfree, Ulids);
defcleanerName(ulid64_t *, zfree, Ulid64s);

/* Add one to a fixed-width uppercase Base36 string, digit by digit.
 * Returns false if the carry runs off the front. */
static bool ulidTestBase36Increment(char *s, size_t len) {
    static constexpr char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (size_t i = len; i-- > 0;) {
        const char *at = strchr(digits, s[i]);
        assert(at && *at);
        if (at[1] != '\0') {
            s[i] = at[1];
            return true;
        }
        s[i] = '0';
    }
    return false;
}

/* Z-order comparison of (a1, a2) and (b1, b2), worked out without
 * interleaving: the pair's most significant differing bit decides, and at
 * equal bit positions ts2's bit sits above ts1's. */
static int ulidTestZOrderCompare(uint64_t a1, uint64_t a2, uint64_t b1,
                                 uint64_t b2) {
    const uint64_t d1 = a1 ^ b1;
    const uint64_t d2 = a2 ^ b2;
    if (d1 == 0 && d2 == 0) {
        return 0;
    }

    /* stdc_leading_zeros(0) is 64, so d1 == 0 needs no case of its own */
    const bool ts2Decides =
        d2 != 0 && stdc_leading_zeros(d2) <= stdc_leading_zeros(d1);
    if (ts2Decides) {
        return a2 < b2 ? -1 : 1;
    }

    return a1 < b1 ? -1 : 1;
}

static int ulidTestSign(int v) {
    return (v > 0) - (v < 0);
}

/* Generate one id of `type` with the generator's clock steered to about
 * `atNs`; *before / *after bracket the instant it read. */
static void ulidTestGenerateAt(ulid64_t *out, ulidVariantType type,
                               uint64_t atNs, uint64_t *before,
                               uint64_t *after) {
    ulidGen g;
    ulidGenInit(&g, ULID_CLOCK_MONOTONIC_OFFSET);
    /* Unsigned arithmetic: monotonic + offset is atNs modulo 2^64 */
    g.clockOffsetNs = atNs - timeUtilMonotonicNs();
    *before = timeUtilMonotonicNs() + g.clockOffsetNs;
    ulid64Generate(&g, out, type);
    *after = timeUtilMonotonicNs() + g.clockOffsetNs;
}

int ulidTest(int, char *[]) {
    int32_t err = 0;

    TEST("every codec refuses bad characters, NULL and short buffers") {
        /* Each decoder this build has: a bad byte at either end of either
         * SIMD half (positions 0, 15, 16, 24) is refused; the untouched
         * text decodes, and every encoder writes it back the same.  NULLs
         * and a buffer one byte short are refused by every codec. */
        typedef struct {
            size_t (*enc)(const ulid_t *, char *, size_t);
            bool (*dec)(const char *, ulid_t *);
            const char *name;
        } codec;
        static const codec codecs[] = {
            {ulidEncodeScalar, ulidDecodeScalar, "scalar"},
            {ulidEncodeSWAR, ulidDecodeSWAR, "swar"},
#if ULID_HAS_SSE2
            {ulidEncodeSSE2, ulidDecodeSSE2, "sse2"},
#endif
#if ULID_HAS_AVX2
            {ulidEncodeAVX2, ulidDecodeAVX2, "avx2"},
#endif
#if ULID_HAS_NEON
            {ulidEncodeNEON, ulidDecodeNEON, "neon"},
#endif
        };
        static constexpr char good[ULID_ENCODED_LENGTH + 1] =
            "0123456789ABCDEFGHIJKLMNO";
        static constexpr uint8_t bad[] = {'!', '/', ':',  '@',  '[', '`',
                                          '{', ' ', 0x7f, 0x80, 0xff};
        static constexpr size_t at[] = {0, 15, 16, 24};
        ulid_t want = {};
        if (!ulidDecodeScalar(good, &want)) {
            ERR("the scalar decoder refused a valid string");
        }
        for (size_t c = 0; c < countof(codecs); c++) {
            const codec *k = &codecs[c];
            ulid_t got = {};
            char text[ULID_ENCODED_LENGTH + 1];
            if (!k->dec(good, &got) || memcmp(&got, &want, sizeof(got)) ||
                k->enc(&want, text, sizeof(text)) != ULID_ENCODED_LENGTH ||
                strcmp(text, good) != 0) {
                ERR("%s: the valid string did not round-trip", k->name);
            }
            for (size_t p = 0; p < countof(at); p++) {
                for (size_t b = 0; b < sizeof(bad); b++) {
                    char probe[ULID_ENCODED_LENGTH + 1];
                    memcpy(probe, good, sizeof(probe));
                    probe[at[p]] = (char)bad[b];
                    if (k->dec(probe, &got)) {
                        ERR("%s: accepted byte 0x%02x at %zu", k->name, bad[b],
                            at[p]);
                    }
                }
            }
            if (k->dec(nullptr, &got) || k->dec(good, nullptr) ||
                k->enc(nullptr, text, sizeof(text)) ||
                k->enc(&want, nullptr, sizeof(text)) ||
                k->enc(&want, text, ULID_ENCODED_LENGTH)) {
                ERR("%s: took a NULL or a short buffer", k->name);
            }
        }

        /* The 64- and 32-bit codecs, and the accessors, refuse the same */
        ulid64_t id64 = {};
        ulid32_t id32 = {};
        char t64[ULID64_ENCODED_LENGTH + 1];
        char t32[ULID32_ENCODED_LENGTH + 1];
        if (ulid64Decode("0000000000!00", &id64) ||
            ulid32Decode("000!000", &id32) || ulid64Decode(nullptr, &id64) ||
            ulid32Decode(nullptr, &id32) ||
            ulid64Encode(&id64, t64, ULID64_ENCODED_LENGTH) ||
            ulid32Encode(&id32, t32, ULID32_ENCODED_LENGTH) ||
            ulid64Encode(nullptr, t64, sizeof(t64)) ||
            ulid32Encode(nullptr, t32, sizeof(t32)) ||
            ulidGetTimestampNs(nullptr) || ulidGetRandom(nullptr) ||
            ulid64GetCounter(nullptr) || ulid64GetSnowflakeMachineId(nullptr) ||
            ulid64GetSnowflakeSequence(nullptr) || ulid64GetNsDelta(nullptr) ||
            ulidGetFirstTimestampNs(nullptr) ||
            ulidGetSecondTimestampNs(nullptr) ||
            ulidGetFirstTimestampNsInterleaved(nullptr) ||
            ulidGetSecondTimestampNsInterleaved(nullptr)) {
            ERR("a 64/32-bit codec or an accessor took bad input");
        }

        /* An encoder this build lacks cannot be chosen; the choice stays */
        [[maybe_unused]] const ulidEncodeImpl before = ulidGetEncodeImpl();
#if !ULID_HAS_AVX2
        if (ulidSetEncodeImpl(ULID_ENCODE_AVX2) ||
            ulidGetEncodeImpl() != before) {
            ERR("an unavailable encoder was chosen");
        }
#endif
        if (strcmp(ulidGetImplName(ULID_ENCODE_AUTO), "auto") != 0 ||
            strcmp(ulidGetImplName((ulidEncodeImpl)99), "unknown") != 0 ||
            ulidIsImplAvailable((ulidEncodeImpl)99)) {
            ERR("encoder names or availability are wrong");
        }
    }

    TEST("generator seeds carry whole bytes from the OS random source") {
        /* Hex characters carry 4 bits a byte: a seed of 16 of them would
         * leave all 16 state bytes inside "0-9a-f" (certain), while 16
         * whole random bytes all land there with probability 2^-64 */
        for (int g = 0; g < 4; g++) {
            ulidGen gen;
            ulidGenInit(&gen, ULID_CLOCK_REALTIME);
            ulidGenSeedRng(&gen);
            uint8_t state[sizeof(gen.rng)];
            memcpy(state, gen.rng, sizeof(state));
            bool allHex = true;
            for (size_t i = 0; i < sizeof(state); i++) {
                const uint8_t c = state[i];
                allHex = allHex &&
                         ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            }
            if (allHex) {
                ERR("generator %d seeded from hex characters only", g);
            }
        }
    }

    TEST("SNOWFLAKE ids stay unique and ordered past 8192 in one ms") {
        /* With the watermark a second ahead of the clock every id lands
         * in the same millisecond, so the 13-bit sequence runs out */
        ulidGen g;
        ulidGenInit(&g, ULID_CLOCK_REALTIME);
        g.lastEmittedNs = timeUtilNs() + 1'000'000'000ULL;
        const ulidVariantConfig cfg = {.type = ULID_VARIANT_SNOWFLAKE,
                                       .machineId = 77};
        ulid64_t prev = {};
        for (int i = 0; i < 20000; i++) {
            ulid64_t id;
            ulid64GenerateWithConfig(&g, &id, &cfg);
            if ((i && ulid64Compare(&prev, &id) >= 0) ||
                ulid64GetSnowflakeMachineId(&id) != 77) {
                ERR("snowflake id %d not after the one before", i);
                break;
            }
            prev = id;
        }
    }

    TEST("32-bit variants, compares and config validation") {
        static const ulidVariantType v32[] = {ULID_VARIANT_32MS,
                                              ULID_VARIANT_32S};
        static constexpr uint64_t prec[] = {1'000'000, 1'000'000'000};
        for (size_t v = 0; v < 2; v++) {
            ulidGen g;
            ulidGenInit(&g, ULID_CLOCK_REALTIME);
            const uint64_t before = timeUtilNs();
            ulid32_t id;
            ulid32Generate(&g, &id, v32[v]);
            const uint64_t after = timeUtilNs();
            /* The timestamp field wraps: compare it modulo its range */
            const uint64_t span = prec[v] << (v ? 22 : 26);
            const uint64_t got = ulid32GetTimestampNs(&id, v32[v]);
            const uint64_t lo = (before / prec[v] * prec[v]) % span;
            const uint64_t hi = (after / prec[v] * prec[v]) % span;
            const uint32_t rnd = ulid32GetRandom(&id, v32[v]);
            char text[16];
            ulid32_t back;
            if ((lo <= hi ? (got < lo || got > hi) : (got < lo && got > hi)) ||
                rnd >> (v ? 10 : 6) ||
                ulid32Encode(&id, text, sizeof(text)) != 7 ||
                !ulid32Decode(text, &back) || ulid32Compare(&back, &id) != 0) {
                ERR("32-bit variant %d round trip", (int)v32[v]);
            }
            if (ulid32GetTimestampNs(&id, ULID_VARIANT_NS) != 0 ||
                ulid32GetRandom(&id, ULID_VARIANT_NS) != 0) {
                ERR("32-bit variant %d: wrong-variant read", (int)v32[v]);
            }
            ulid32_t none;
            ulid32Generate(&g, &none, ULID_VARIANT_NS);
            if (none.data != 0) {
                ERR("ulid32Generate made a 64-bit variant");
            }
        }

        const ulid32_t a32 = {.data = 5};
        const ulid32_t b32 = {.data = 9};
        const ulid64_t a64 = {.data = 5};
        const ulid64_t b64 = {.data = 9};
        ulid_t a128 = {};
        ulid_t b128 = {};
        b128.data[0] = 1;
        if (ulid32Compare(&a32, &b32) >= 0 || ulid32Compare(&b32, &a32) <= 0 ||
            ulid32Compare(nullptr, &a32) >= 0 ||
            ulid32Compare(&a32, nullptr) <= 0 ||
            ulid32Compare(nullptr, nullptr) != 0 ||
            ulid64Compare(&a64, &b64) >= 0 || ulid64Compare(&b64, &a64) <= 0 ||
            ulid64Compare(nullptr, &a64) >= 0 ||
            ulid64Compare(&a64, nullptr) <= 0 ||
            ulidCompare(&a128, &b128) >= 0 ||
            ulidCompare(nullptr, &a128) >= 0 ||
            ulidCompare(&a128, nullptr) <= 0 ||
            ulidCompare(nullptr, nullptr) != 0) {
            ERR("compares");
        }

        ulidVariantConfig cfg = {.type = ULID_VARIANT_SNOWFLAKE,
                                 .machineId = 1023};
        const bool snowOk = ulidVariantConfigValidate(&cfg);
        cfg.machineId = 1024;
        const bool snowBad = ulidVariantConfigValidate(&cfg);
        cfg = (ulidVariantConfig){.type = ULID_VARIANT_EPOCHCUSTOM,
                                  .customEpochNs = ULID_EPOCH_2020_NS};
        const bool epochOk = ulidVariantConfigValidate(&cfg);
        cfg.customEpochNs = timeUtilNs() + 1000000000000ULL;
        const bool epochBad = ulidVariantConfigValidate(&cfg);
        cfg = (ulidVariantConfig){.type = ULID_VARIANT_COUNT};
        if (!snowOk || snowBad || !epochOk || epochBad ||
            ulidVariantConfigValidate(&cfg) ||
            ulidVariantConfigValidate(nullptr)) {
            ERR("config validation");
        }
        if (ulid64GetMaxTimestamp(ULID_VARIANT_DUALNS) != UINT64_MAX ||
            ulid64GetMaxTimestamp(ULID_VARIANT_COUNT) != 0 ||
            ulid64GetMaxTimestamp(ULID_VARIANT_EPOCHCUSTOM) !=
                0xFFFFFFFFFFFFULL) {
            ERR("max timestamps");
        }
    }

    TEST("every 64-bit variant round-trips a timestamp in its range") {
        /* At the middle of each variant's valid range and near its top,
         * the extracted timestamp is the generation instant to the
         * variant's precision, the random/sequence part fits its bits,
         * and the range check agrees */
        static const struct {
            ulidVariantType type;
            uint64_t lowNs;
            uint64_t precisionNs;
        } variants[] = {
            {ULID_VARIANT_EPOCH2020, ULID_EPOCH_2020_NS, 1},
            {ULID_VARIANT_EPOCH2024, ULID_EPOCH_2024_NS, 1},
            {ULID_VARIANT_NS, 0, 1},
            {ULID_VARIANT_US, 0, 1000},
            {ULID_VARIANT_MS, 0, 1'000'000},
            {ULID_VARIANT_NSCOUNT, 0, 1},
            {ULID_VARIANT_HYBRID, 0, 1},
            {ULID_VARIANT_SNOWFLAKE, 0, 1'000'000},
        };
        for (size_t v = 0; v < countof(variants); v++) {
            const ulidVariantType type = variants[v].type;
            const uint64_t lo = variants[v].lowNs;
            const uint64_t hi = ulid64GetMaxTimestamp(type);
            const uint64_t prec = variants[v].precisionNs;
            const uint64_t points[2] = {lo + (hi - lo) / 2, hi - 10'000'000};
            for (size_t p = 0; p < 2; p++) {
                ulid64_t id;
                uint64_t before = 0;
                uint64_t after = 0;
                ulidTestGenerateAt(&id, type, points[p], &before, &after);
                const uint64_t got = ulid64GetTimestampNs(&id, type);
                const uint8_t bits = ulid64GetVariantRandomBits(type);
                const uint64_t rnd = ulid64GetRandom(&id, type);
                if (got < before / prec * prec || got > after / prec * prec ||
                    !ulid64ValidateTimestamp(got, type) ||
                    (bits < 64 && rnd >> bits) || bits == 0) {
                    ERR("%s at %" PRIu64 ": got %" PRIu64 " outside [%" PRIu64
                        ", %" PRIu64 "], random %" PRIu64 " of %u bits",
                        ulid64GetVariantName(type), points[p], got, before,
                        after, rnd, bits);
                }
            }
            if (!ulid64ValidateTimestamp(hi, type) ||
                ulid64ValidateTimestamp(hi + 1, type) ||
                (lo > 0 && ulid64ValidateTimestamp(lo - 1, type))) {
                ERR("%s: range check wrong at its bounds",
                    ulid64GetVariantName(type));
            }
            if (!ulid64IsValidVariantType(type) ||
                !ulid64GetVariantDescription(type) ||
                !ulid64GetVariantPrecision(type) ||
                !(ulid64GetVariantRangeYears(type) > 0.0)) {
                ERR("variant %d: metadata missing", (int)type);
            }
        }
        if (ulid64ValidateTimestamp(ULID_EPOCH_2024_NS - 1,
                                    ULID_VARIANT_EPOCH2024)) {
            ERR("a timestamp before the 2024 epoch read as valid");
        }
    }

    /* Shared generator for tests that don't exercise state itself */
    ulidGen g;
    ulidGenInit(&g, ULID_CLOCK_REALTIME);

    printf("Testing ULID (Base36) generation and encoding...\n");
    printf("Available implementations:\n");
    for (int32_t i = 0; i < ULID_ENCODE_COUNT; i++) {
        printf("  %s: %s\n", ulidGetImplName(i),
               ulidIsImplAvailable(i) ? "available" : "not available");
    }
    printf("\n");

    /* ================================================================
     * Generator State Contract Tests
     * ================================================================ */
    TEST("64/32-bit accessors: fields fit their layouts; zero/copy/clear") {
        ulid64_t a;
        ulid64_t b;
        ulid64Generate(&g, &a, ULID_VARIANT_NSCOUNT);
        ulid64Generate(&g, &b, ULID_VARIANT_NSCOUNT);
        /* NSCOUNT: 40-bit ns + 24-bit counter, ids rising */
        if (ulid64GetCounter(&a) >> 24 || ulid64Compare(&a, &b) >= 0) {
            ERR("NSCOUNT counter outside 24 bits or ids not rising");
        }
        ulid64Generate(&g, &a, ULID_VARIANT_HYBRID);
        if (ulid64GetNsDelta(&a) >> 20) {
            ERR("HYBRID ns delta outside 20 bits");
        }
        ulid64Generate(&g, &a, ULID_VARIANT_MS);
        if (ulid64GetRandom(&a, ULID_VARIANT_MS) >> 22) {
            ERR("MS random outside 22 bits");
        }
        ulid64Copy(&b, &a);
        if (ulid64IsZero(&a) || ulid64Compare(&a, &b) != 0) {
            ERR("a generated id is zero or did not copy");
        }
        ulid64Clear(&b);
        if (!ulid64IsZero(&b)) {
            ERR("a cleared id is not zero");
        }

        /* The largest timestamp a variant holds validates; one past not */
        const uint64_t max = ulid64GetMaxTimestamp(ULID_VARIANT_EPOCH2024);
        if (!ulid64ValidateTimestamp(ULID_EPOCH_2024_NS,
                                     ULID_VARIANT_EPOCH2024) ||
            !ulid64ValidateTimestamp(max, ULID_VARIANT_EPOCH2024) ||
            ulid64ValidateTimestamp(max + 1, ULID_VARIANT_EPOCH2024) ||
            max <= ULID_EPOCH_2024_NS) {
            ERR("the EPOCH2024 timestamp bounds are wrong");
        }

        ulid32_t s;
        ulid32_t t;
        ulid32Generate(&g, &s, ULID_VARIANT_32S);
        ulid32Copy(&t, &s);
        if (ulid32GetRandom(&s, ULID_VARIANT_32S) >> 10 ||
            ulid32Compare(&s, &t) != 0 ||
            ulid32GetTimestampNs(&s, ULID_VARIANT_32S) % 1'000'000'000 != 0) {
            ERR("a 32S id's random or second-resolution timestamp is wrong");
        }
        ulid32Clear(&t);
        if (!ulid32IsZero(&t) || ulid32IsZero(&s)) {
            ERR("ulid32 IsZero / Clear disagree");
        }

        if ((unsigned)ulidGetEncodeImpl() >= ULID_ENCODE_COUNT) {
            ERR("the encoding implementation is not a known one");
        }
    }

    TEST("generator: stack zero-init self-seeds and generates") {
        ulidGen z = {};
        ulid_t id;
        ulidGenerate(&z, &id);

        if (ulidIsZero(&id)) {
            ERR("Zero-init generator produced zero ULID");
        }
        if (z.rng[0] == 0 && z.rng[1] == 0) {
            ERR("Zero-init generator did not self-seed");
        }
        if (z.lastEmittedNs == 0) {
            ERR("Zero-init generator did not advance its watermark");
        }
    }

    TEST("generator: heap New/Free round trip") {
        ulidGen *h = ulidGenNew(ULID_CLOCK_REALTIME);
        ulid_t id;
        ulidGenerate(h, &id);
        if (ulidIsZero(&id)) {
            ERR("Heap generator produced zero ULID");
        }
        ulidGenFree(h);
    }

    TEST("generator: independent generators, independent streams") {
        ulidGen a;
        ulidGen b;
        ulidGenInit(&a, ULID_CLOCK_REALTIME);
        ulidGenInit(&b, ULID_CLOCK_REALTIME);

        constexpr int STREAM_N = 8;
        for (int32_t i = 0; i < STREAM_N; i++) {
            ulid_t ida;
            ulid_t idb;
            ulidGenerate(&a, &ida);
            ulidGenerate(&b, &idb);
            if (ulidGetRandom(&ida) == ulidGetRandom(&idb)) {
                ERR("Generators share a random stream at step %d", i);
            }
        }
    }

    TEST("generator: strict monotonic emission under bursts") {
        ulidGen m;
        ulidGenInit(&m, ULID_CLOCK_REALTIME);

        ulid_t prev;
        ulidGenerate(&m, &prev);
        for (int32_t i = 0; i < 10000; i++) {
            ulid_t cur;
            ulidGenerate(&m, &cur);
            if (ulidGetTimestampNs(&cur) <= ulidGetTimestampNs(&prev)) {
                ERR("Emission not strictly monotonic at %d "
                    "(%" PRIu64 " <= %" PRIu64 ")",
                    i, ulidGetTimestampNs(&cur), ulidGetTimestampNs(&prev));
                break;
            }
            prev = cur;
        }
    }

    TEST("generator: clock regression cannot regress emission") {
        /* A wall-clock step (or same-instant burst) must never emit a
         * timestamp at or below one already issued: force the
         * watermark far ahead of the real clock and keep generating */
        ulidGen m;
        ulidGenInit(&m, ULID_CLOCK_REALTIME);

        ulid_t first;
        ulidGenerate(&m, &first);
        const uint64_t forced = ulidGetTimestampNs(&first) + 1'000'000'000ULL;
        m.lastEmittedNs = forced;

        ulid_t second;
        ulidGenerate(&m, &second);
        if (ulidGetTimestampNs(&second) != forced + 1) {
            ERR("Watermark bump wrong: got %" PRIu64 " want %" PRIu64,
                ulidGetTimestampNs(&second), forced + 1);
        }

        ulid_t third;
        ulidGenerate(&m, &third);
        if (ulidGetTimestampNs(&third) <= ulidGetTimestampNs(&second)) {
            ERR("Emission regressed after forced watermark");
        }
    }

    TEST("generator: both clock sources yield epoch timestamps") {
        ulidGen rt;
        ulidGen mo;
        ulidGenInit(&rt, ULID_CLOCK_REALTIME);
        ulidGenInit(&mo, ULID_CLOCK_MONOTONIC_OFFSET);

        ulid_t idRt;
        ulid_t idMo;
        ulidGenerate(&rt, &idRt);
        ulidGenerate(&mo, &idMo);

        const uint64_t nowNs = timeUtilNs();
        const uint64_t tolNs = 5000000000ULL; /* 5s */
        const uint64_t tsRt = ulidGetTimestampNs(&idRt);
        const uint64_t tsMo = ulidGetTimestampNs(&idMo);

        if (tsRt + tolNs < nowNs || tsRt > nowNs + tolNs) {
            ERR("REALTIME timestamp not near wall clock: %" PRIu64, tsRt);
        }
        if (tsMo + tolNs < nowNs || tsMo > nowNs + tolNs) {
            ERR("MONOTONIC_OFFSET timestamp not near wall clock: %" PRIu64,
                tsMo);
        }
    }

    TEST("generator: dual timestamps strictly ordered") {
        ulidGen m;
        ulidGenInit(&m, ULID_CLOCK_REALTIME);

        for (int32_t i = 0; i < 1000; i++) {
            ulid_t id;
            ulidGenerateDualNs(&m, &id);
            if (ulidGetSecondTimestampNs(&id) <= ulidGetFirstTimestampNs(&id)) {
                ERR("DualNs ts2 <= ts1 at %d", i);
                break;
            }
        }
    }

    /* ================================================================
     * Basic Functionality Tests
     * ================================================================ */
    TEST("ulid generation") {
        ulid_t id1, id2;
        char enc1[ULID_ENCODED_LENGTH + 1];
        char enc2[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id1);
        ulidGenerate(&g, &id2);

        ulidEncode(&id1, enc1, sizeof(enc1));
        ulidEncode(&id2, enc2, sizeof(enc2));

        printf("    Sample sequential ULIDs:\n");
        printf("      [1] %s (ts=%" PRIu64 ")\n", enc1,
               ulidGetTimestampNs(&id1));
        printf("      [2] %s (ts=%" PRIu64 ")\n", enc2,
               ulidGetTimestampNs(&id2));

        if (ulidIsZero(&id1)) {
            ERR("Generated ULID is zero");
        }

        if (ulidIsZero(&id2)) {
            ERR("Second generated ULID is zero");
        }

        /* IDs should be different */
        if (ulidCompare(&id1, &id2) == 0) {
            ERR("Two generated ULIDs are identical");
        }

        /* Timestamps should be monotonically increasing or equal */
        uint64_t ts1 = ulidGetTimestampNs(&id1);
        uint64_t ts2 = ulidGetTimestampNs(&id2);

        if (ts1 == 0) {
            ERR("Timestamp extraction failed for first ULID");
        }
        if (ts2 < ts1) {
            ERR("Timestamps not monotonic: %" PRIu64 " > %" PRIu64, ts1, ts2);
        }
    }

    TEST("ulid encoding length") {
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);
        size_t len = ulidEncode(&id, encoded, sizeof(encoded));

        if (len != ULID_ENCODED_LENGTH) {
            ERR("Encoding length wrong: %zu != %d", len, ULID_ENCODED_LENGTH);
        }

        if (encoded[ULID_ENCODED_LENGTH] != '\0') {
            ERR("Encoded string not null-terminated");
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
            ulidGenerate(&g, &id);
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

        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample scalar encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&g, &orig);
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
                ERR("Timestamp mismatch after roundtrip");
            }

            if (ulidGetRandom(&orig) != ulidGetRandom(&decoded)) {
                ERR("Random component mismatch after roundtrip");
            }
        }
    }

    TEST("ulid encode/decode roundtrip (SWAR)") {
        ulidSetEncodeImpl(ULID_ENCODE_SWAR);

        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample SWAR encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&g, &orig);
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
        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&g, &orig);
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
        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&g, &orig);
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
        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample NEON encode/decode:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulidGenerate(&g, &orig);
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
        ulid_t orig, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];

        printf("    Sample cross-impl verification:\n");
        for (int32_t test = 0; test < 100; test++) {
            ulidGenerate(&g, &orig);

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
        ulid_t id, decoded = {};
        char encoded[ULID_ENCODED_LENGTH + 1];
        char lowercase[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);
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
            ERR("Lowercase decode produced different result");
        }
    }

    TEST("ulid ordering with timestamps") {
        ulid_t id1, id2;
        char enc1[ULID_ENCODED_LENGTH + 1];
        char enc2[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id1);
        usleep(1000); /* 1ms delay */
        ulidGenerate(&g, &id2);

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
        cleanerName(zfree, Ulids) ulid_t *ids = zcalloc(numIds, sizeof(ulid_t));

        for (int32_t i = 0; i < numIds; i++) {
            ulidGenerate(&g, &ids[i]);
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

        if (collisions > 0) {
            ERR("Found %d collisions in %d ULIDs", collisions, numIds);
        }
    }

    TEST("ulid decode invalid input") {
        ulid_t id;

        /* Wrong length */
        if (ulidDecode("ABC", &id)) {
            ERR("Accepted too-short input");
        }

        if (ulidDecode("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", &id)) {
            ERR("Accepted too-long input");
        }

        /* Invalid characters */
        char invalid[ULID_ENCODED_LENGTH + 1] = "0000000000000000000000000";
        invalid[0] = '!';
        if (ulidDecode(invalid, &id)) {
            ERR("Accepted invalid character '!'");
        }

        invalid[0] = '[';
        if (ulidDecode(invalid, &id)) {
            ERR("Accepted invalid character '['");
        }

        invalid[0] = ' ';
        if (ulidDecode(invalid, &id)) {
            ERR("Accepted space character");
        }
    }

    TEST("ulid utility functions") {
        ulid_t id1, id2, copy;

        ulidGenerate(&g, &id1);

        /* Test copy */
        ulidCopy(&copy, &id1);
        if (ulidCompare(&id1, &copy) != 0) {
            ERR("Copy produced different ULID");
        }

        /* Test clear */
        ulidClear(&id2);
        if (!ulidIsZero(&id2)) {
            ERR("Clear did not zero ULID");
        }

        /* Test isZero */
        if (ulidIsZero(&id1)) {
            ERR("Non-zero ULID reported as zero");
        }
    }

    TEST("ulid convenience function") {
        char encoded[ULID_ENCODED_LENGTH + 1];
        size_t len = ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

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

        /* No generator: nothing generated, nothing encoded */
        if (ulidGenerateAndEncode(nullptr, encoded, sizeof(encoded)) != 0) {
            ERR("GenerateAndEncode without a generator should return 0");
        }

        char encoded64[ULID64_ENCODED_LENGTH + 1];
        if (ulid64GenerateAndEncode(nullptr, encoded64, sizeof(encoded64),
                                    ULID_VARIANT_NS) != 0) {
            ERR("ulid64GenerateAndEncode without a generator should return 0");
        }
    }

    /* ================================================================
     * Performance Tests
     * ================================================================ */
    printf("\n--- Performance Benchmarks ---\n\n");

    TEST("ulid generation performance") {
        const size_t iterations = 1'000'000;
        ulid_t id;

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerate(&g, &id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "ulid generations");
    }

    TEST("ulid encode performance (scalar)") {
        const size_t iterations = 1'000'000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeScalar(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "scalar encodings");
    }

    TEST("ulid decode performance (scalar)") {
        const size_t iterations = 1'000'000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeScalar(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "scalar decodings");
    }

    TEST("ulid encode performance (SWAR)") {
        const size_t iterations = 1'000'000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeSWAR(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SWAR encodings");
    }

    TEST("ulid decode performance (SWAR)") {
        const size_t iterations = 1'000'000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeSWAR(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SWAR decodings");
    }

#if ULID_HAS_SSE2
    TEST("ulid encode performance (SSE2)") {
        const size_t iterations = 1'000'000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeSSE2(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 encodings");
    }

    TEST("ulid decode performance (SSE2)") {
        const size_t iterations = 1'000'000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeSSE2(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SSE2 decodings");
    }
#endif

#if ULID_HAS_AVX2
    TEST("ulid encode performance (AVX2)") {
        const size_t iterations = 1'000'000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeAVX2(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 encodings");
    }

    TEST("ulid decode performance (AVX2)") {
        const size_t iterations = 1'000'000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeAVX2(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AVX2 decodings");
    }
#endif

#if ULID_HAS_NEON
    TEST("ulid encode performance (NEON)") {
        const size_t iterations = 1'000'000;
        ulid_t id;
        char encoded[ULID_ENCODED_LENGTH + 1];

        ulidGenerate(&g, &id);

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidEncodeNEON(&id, encoded, sizeof(encoded));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON encodings");
    }

    TEST("ulid decode performance (NEON)") {
        const size_t iterations = 1'000'000;
        char encoded[ULID_ENCODED_LENGTH + 1];
        ulid_t decoded;

        ulidGenerateAndEncode(&g, encoded, sizeof(encoded));

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidDecodeNEON(encoded, &decoded);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NEON decodings");
    }
#endif

    TEST("ulid full roundtrip performance") {
        const size_t iterations = 500'000;
        ulid_t id, decoded;
        char encoded[ULID_ENCODED_LENGTH + 1];

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerate(&g, &id);
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
        ulid64_t orig, decoded = {};
        char encoded[ULID64_ENCODED_LENGTH + 1];

        printf("    Sample EPOCH2020 IDs:\n");
        for (int32_t test = 0; test < 1000; test++) {
            ulid64Generate(&g, &orig, ULID_VARIANT_EPOCH2020);
            ulid64Encode(&orig, encoded, sizeof(encoded));

            if (test < 3) {
                uint64_t ts =
                    ulid64GetTimestampNs(&orig, ULID_VARIANT_EPOCH2020);
                printf("      [%d] %s (ts=%" PRIu64 " ns, data=0x%016" PRIx64
                       ")\n",
                       test + 1, encoded, ts, orig.data);
            }

            if (!ulid64Decode(encoded, &decoded)) {
                ERR("Failed to decode: %s", encoded);
            }

            if (ulid64Compare(&orig, &decoded) != 0) {
                if (test < 5) {
                    printf("      DEBUG: orig=0x%016" PRIx64
                           " decoded=0x%016" PRIx64 " encoded=%s\n",
                           orig.data, decoded.data, encoded);
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

        ulid64Generate(&g, &id1, ULID_VARIANT_NS);
        ulid64Generate(&g, &id2, ULID_VARIANT_NS);
        ulid64Generate(&g, &id3, ULID_VARIANT_NS);

        ulid64Encode(&id1, enc1, sizeof(enc1));
        ulid64Encode(&id2, enc2, sizeof(enc2));
        ulid64Encode(&id3, enc3, sizeof(enc3));

        printf("    Sequential NS IDs:\n");
        printf("      [1] %s\n", enc1);
        printf("      [2] %s\n", enc2);
        printf("      [3] %s\n", enc3);

        if (ulid64Compare(&id1, &id2) >= 0) {
            ERR("IDs not monotonically increasing: id1 >= id2");
        }
        if (ulid64Compare(&id2, &id3) >= 0) {
            ERR("IDs not monotonically increasing: id2 >= id3");
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
        cleanerName(zfree, Ulid64s) ulid64_t *ids =
            zcalloc(numIds, sizeof(ulid64_t));

        for (int32_t i = 0; i < numIds; i++) {
            ulid64Generate(&g, &ids[i], ULID_VARIANT_NSCOUNT);
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
            ulidGenerateDualNs(&g, &id);
            ulidEncode(&id, encoded, sizeof(encoded));

            uint64_t ts1 = ulidGetFirstTimestampNs(&id);
            uint64_t ts2 = ulidGetSecondTimestampNs(&id);

            printf("      [%d] %s\n", i + 1, encoded);
            printf("          ts1=%" PRIu64 ", ts2=%" PRIu64 ", delta=%" PRId64
                   " ns\n",
                   ts1, ts2, (int64_t)ts2 - (int64_t)ts1);

            /* Second timestamp should be >= first */
            if (ts2 < ts1) {
                ERR("DUALNS: second timestamp (%" PRIu64 ") < first (%" PRIu64
                    ")",
                    ts2, ts1);
            }

            /* Verify roundtrip */
            ulid_t decoded = {};
            if (!ulidDecode(encoded, &decoded)) {
                ERR("Failed to decode DUALNS: %s", encoded);
            }

            TEST_BYTES(err, &id, &decoded, sizeof(ulid_t),
                       "DUALNS roundtrip failed");
        }
    }

    TEST("DUALNS __uint128_t interface") {
        printf("    Testing __uint128_t interface:\n");

        /* Test 1: Direct generation as __uint128_t */
        __uint128_t val128 = ulidGenerateDualNsU128(&g);
        uint64_t ts1_direct = (uint64_t)(val128 >> 64);
        uint64_t ts2_direct = (uint64_t)val128;

        printf("      Direct generation:\n");
        printf("        __uint128_t value: ts1=%" PRIu64 ", ts2=%" PRIu64 "\n",
               ts1_direct, ts2_direct);
        printf("        delta=%" PRId64 " ns\n",
               (int64_t)ts2_direct - (int64_t)ts1_direct);

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
        ulid_t decoded = {};
        if (!ulidDecode(encoded, &decoded)) {
            ERR("Failed to decode DUALNS from __uint128_t: %s", encoded);
        }

        TEST_BYTES(err, &id_from_u128, &decoded, sizeof(ulid_t),
                   "DUALNS __uint128_t string roundtrip failed");

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
            __uint128_t rapid = ulidGenerateDualNsU128(&g);
            uint64_t r_ts1 = (uint64_t)(rapid >> 64);
            uint64_t r_ts2 = (uint64_t)rapid;

            printf("        [%d] ts1=%" PRIu64 ", ts2=%" PRIu64
                   ", delta=%" PRId64 " ns\n",
                   i + 1, r_ts1, r_ts2, (int64_t)r_ts2 - (int64_t)r_ts1);

            if (r_ts2 < r_ts1) {
                ERR("Rapid gen %d: ts2 < ts1", i);
            }
        }
    }

    TEST("DUALNS_INTERLEAVED bit interleaving") {
        printf("    Testing bit-interleaved dual timestamp variant:\n");

        for (uint32_t z = 0; z < 3; z++) {
            /* Test 1: Basic generation and extraction */
            __uint128_t interleaved = ulidGenerateDualNsInterleavedU128(&g);
            uint64_t ts1_orig, ts2_orig;
            deinterleaveBits64Dispatch(interleaved, &ts1_orig, &ts2_orig);

            printf("      Generated interleaved value:\n");
            printf("        ts1=%" PRIu64 ", ts2=%" PRIu64 "\n", ts1_orig,
                   ts2_orig);
            printf("        delta=%" PRId64 " ns\n",
                   (int64_t)ts2_orig - (int64_t)ts1_orig);

            /* Test 2: Verify bit interleaving correctness */
            __uint128_t reinterleaved =
                interleaveBits64Dispatch(ts1_orig, ts2_orig);
            if (reinterleaved != interleaved) {
                ERR("Bit interleaving roundtrip failed");
            }
            printf("      Bit interleaving roundtrip: ts -> interleave -> "
                   "deinterleave -> ts ✓\n");

            /* Test 3: Create ulid_t and verify extraction */
            ulid_t id_interleaved;
            ulidGenerateDualNsInterleaved(&g, &id_interleaved);

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

            ulid_t decoded = {};
            if (!ulidDecode(encoded, &decoded)) {
                ERR("Failed to decode DUALNS_INTERLEAVED: %s", encoded);
            }

            TEST_BYTES(err, &id_from_u128, &decoded, sizeof(ulid_t),
                       "DUALNS_INTERLEAVED string roundtrip failed");

            /* Test 6: Extract from decoded and verify */
            __uint128_t extracted_u128 = ulidToDualNsInterleavedU128(&decoded);
            if (extracted_u128 != interleaved) {
                ERR("Extracted __uint128_t doesn't match original");
            }

            printf("      Full roundtrip: __uint128_t -> ulid_t -> string -> "
                   "ulid_t -> __uint128_t ✓\n");

            /* Test 7: Sort order. Interleaving gives Z-order: IDs sort by
             * the full interleaved value, where the most significant
             * differing bit of either timestamp decides. Neither timestamp
             * alone orders them. The ulid_t bytes and the Base36 string
             * must sort the same way as the value. */
            printf("      Testing Z-order sort contract:\n");

            const uint64_t ts1_a = 1700000000000000000ULL;
            const uint64_t ts2_a = ts1_a + 100;

            /* {a1, a2, b1, b2, expected sign of a vs b} */
            const struct {
                uint64_t a1, a2, b1, b2;
                int want;
            } zcases[] = {
                /* ts1's highest differing bit is above ts2's: ts1 decides */
                {0x1000, 0x10, 0x2000, 0x08, -1},
                /* ts1_b > ts1_a, but ts2 differs at a higher bit and
                 * decides: b sorts first */
                {1000, 0x800, 1001, 0x400, 1},
                /* Same highest bit in both: ts2's bit is the higher one */
                {1, 0, 0, 1, -1},
                {0, 0, 1, 1, -1},
                {ts1_a, ts2_a, ts1_a, ts2_a, 0},
            };

            uint64_t zseed[2] = {0x9E3779B97F4A7C15ULL, 0xD1B54A32D192ED03ULL};
            for (size_t zc = 0; zc < countof(zcases) + 2000; zc++) {
                uint64_t a1, a2, b1, b2;
                int want;
                if (zc < countof(zcases)) {
                    a1 = zcases[zc].a1;
                    a2 = zcases[zc].a2;
                    b1 = zcases[zc].b1;
                    b2 = zcases[zc].b2;
                    want = zcases[zc].want;
                } else {
                    /* Pairs near each other, so both timestamps get to
                     * decide across the run */
                    a1 = xoroshiro128plus(zseed);
                    a2 = xoroshiro128plus(zseed);
                    const uint64_t r = xoroshiro128plus(zseed);
                    b1 = a1 ^ (r & ((1ULL << (r % 64)) - 1));
                    b2 = a2 ^ ((r >> 7) & ((1ULL << ((r >> 32) % 64)) - 1));
                    want = ulidTestZOrderCompare(a1, a2, b1, b2);
                }

                if (ulidTestZOrderCompare(a1, a2, b1, b2) != want) {
                    ERR("Z-order oracle disagrees with case %zu", zc);
                }

                const __uint128_t va = interleaveBits64Dispatch(a1, a2);
                const __uint128_t vb = interleaveBits64Dispatch(b1, b2);
                const int byValue = (va > vb) - (va < vb);

                ulid_t ida, idb;
                ulidFromDualNsInterleaved(&ida, a1, a2);
                ulidFromDualNsInterleaved(&idb, b1, b2);
                const int byBytes =
                    ulidTestSign(memcmp(ida.data, idb.data, sizeof(ida.data)));

                char sa[ULID_ENCODED_LENGTH + 1];
                char sb[ULID_ENCODED_LENGTH + 1];
                ulidEncode(&ida, sa, sizeof(sa));
                ulidEncode(&idb, sb, sizeof(sb));
                const int byString = ulidTestSign(strcmp(sa, sb));

                if (byValue != want || byBytes != want || byString != want) {
                    ERR("Z-order case %zu: value %d bytes %d string %d, "
                        "expected %d",
                        zc, byValue, byBytes, byString, want);
                    break;
                }
            }

            printf("        interleaved value, bytes and Base36 string all "
                   "follow Z-order ✓\n");

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
            ulidGenerateDualNsInterleaved(&g, &prev_id);

            for (int32_t i = 0; i < 3; i++) {
                ulid_t curr_id;
                ulidGenerateDualNsInterleaved(&g, &curr_id);

                uint64_t curr_ts1 =
                    ulidGetFirstTimestampNsInterleaved(&curr_id);
                uint64_t curr_ts2 =
                    ulidGetSecondTimestampNsInterleaved(&curr_id);

                printf("        [%d] ts1=%" PRIu64 ", ts2=%" PRIu64
                       ", delta=%" PRId64 " ns\n",
                       i + 1, curr_ts1, curr_ts2,
                       (int64_t)curr_ts2 - (int64_t)curr_ts1);

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
            ulid64GenerateWithConfig(&g, &id, &config);
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
        ulid64_t orig, decoded = {};
        char encoded[ULID64_ENCODED_LENGTH + 1];

        /* Test all 64-bit variants (DUALNS is 128-bit, tested separately) */
        ulidVariantType variants[] = {
            ULID_VARIANT_EPOCH2020, ULID_VARIANT_EPOCH2024,
            ULID_VARIANT_NS,        ULID_VARIANT_US,
            ULID_VARIANT_MS,        ULID_VARIANT_NSCOUNT,
            ULID_VARIANT_HYBRID,    ULID_VARIANT_SNOWFLAKE};

        for (size_t v = 0; v < countof(variants); v++) {
            ulidVariantType type = variants[v];

            for (int32_t test = 0; test < 100; test++) {
                ulid64Generate(&g, &orig, type);
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
        ulid32_t orig, decoded = {};
        char encoded[ULID32_ENCODED_LENGTH + 1];

        printf("    Sample 32-bit IDs:\n");
        ulid32Generate(&g, &orig, ULID_VARIANT_32MS);
        ulid32Encode(&orig, encoded, sizeof(encoded));
        printf("      32MS: %s\n", encoded);

        if (!ulid32Decode(encoded, &decoded)) {
            ERR("Failed to decode 32MS: %s", encoded);
        }

        if (ulid32Compare(&orig, &decoded) != 0) {
            ERR("32MS roundtrip failed");
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
            ERR("Failed to validate custom epoch config");
        }

        ulid64_t id;
        ulid64GenerateWithConfig(&g, &id, &config);

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
        const size_t iterations = 1'000'000;
        ulid64_t id;

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulid64Generate(&g, &id, ULID_VARIANT_NS);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "NS generations");
    }

    TEST("ulid64 encode performance") {
        const size_t iterations = 1'000'000;
        ulid64_t id;
        char encoded[ULID64_ENCODED_LENGTH + 1];
        volatile size_t totalLen = 0;

        ulid64Generate(&g, &id, ULID_VARIANT_MS);

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
        const size_t iterations = 1'000'000;
        char encoded[ULID64_ENCODED_LENGTH + 1];
        ulid64_t decoded;

        ulid64GenerateAndEncode(&g, encoded, sizeof(encoded), ULID_VARIANT_MS);

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
                               timeUtilNs() - 1'000'000};
        uint64_t test_ts2[] = {0ULL,
                               0xFFFFFFFFFFFFFFFFULL,
                               1ULL,
                               0xAAAAAAAAAAAAAAAAULL,
                               0x5555555555555555ULL,
                               0xF0F0F0F0F0F0F0F0ULL,
                               0xFEDCBA9876543210ULL,
                               timeUtilNs(),
                               timeUtilNs() + 2000,
                               timeUtilNs() - 2'000'000};

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

#if defined(__SSE2__)
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

#ifdef __ARM_NEON
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
        const size_t iterations = 1'000'000;
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

#if defined(__SSE2__)
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

#ifdef __ARM_NEON
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
        const size_t iterations = 100'000;
        ulid_t id;

        printf("    End-to-end DUALNS_INTERLEAVED generation benchmark:\n");
        printf("    Iterations: %zu\n\n", iterations);

        /* Benchmark with SCALAR implementation */
        printf("      SCALAR implementation:\n");
        setInterleaveImpl(INTERLEAVE_SCALAR);
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            ulidGenerateDualNsInterleaved(&g, &id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "SCALAR generations");

        /* Test extraction with SCALAR */
        printf("      SCALAR extraction:\n");
        ulidGenerateDualNsInterleaved(&g, &id);
        uint64_t ts1 = 0, ts2 = 0;
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
            ulidGenerateDualNsInterleaved(&g, &id);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, "AUTO generations");

        /* Test extraction with AUTO */
        printf("      AUTO extraction:\n");
        ulidGenerateDualNsInterleaved(&g, &id);
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
#if defined(__SSE2__)
        else if (selected == INTERLEAVE_SSE2) {
            impl_name = "SSE2";
        }
#endif
#if defined(__AVX2__) && defined(__BMI2__)
        else if (selected == INTERLEAVE_AVX2) {
            impl_name = "AVX2";
        }
#endif
#ifdef __ARM_NEON
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

    TEST("decoders refuse values past the type and wrong lengths") {
        /* ulid64: the encoding of UINT64_MAX decodes, one more is refused */
        {
            const ulid64_t max = {.data = UINT64_MAX};
            char s[ULID64_ENCODED_LENGTH + 2];
            ulid64Encode(&max, s, sizeof(s));
            if (strcasecmp(s, "3w5e11264sgsf") != 0) {
                ERR("UINT64_MAX encodes as %s", s);
            }

            ulid64_t got = {};
            if (!ulid64Decode(s, &got) || got.data != UINT64_MAX) {
                ERR("ulid64 max %s did not decode", s);
            }

            if (!ulid64Decode("3w5e11264sgsf", &got) ||
                got.data != UINT64_MAX) {
                ERR("ulid64 lowercase max did not decode");
            }

            char over[ULID64_ENCODED_LENGTH + 2];
            memcpy(over, s, sizeof(over));
            if (!ulidTestBase36Increment(over, ULID64_ENCODED_LENGTH)) {
                ERR("ulid64 max has no successor");
            }
            got.data = 7;
            if (ulid64Decode(over, &got) || got.data != 7) {
                ERR("ulid64 max+1 %s was accepted", over);
            }

            if (ulid64Decode("ZZZZZZZZZZZZZ", &got)) {
                ERR("ulid64 all-Z was accepted");
            }

            if (ulid64Decode("00000000000000", &got) ||
                ulid64Decode("000000000000", &got)) {
                ERR("ulid64 accepted a wrong length");
            }
        }

        /* ulid32 */
        {
            const ulid32_t max = {.data = UINT32_MAX};
            char s[ULID32_ENCODED_LENGTH + 2];
            ulid32Encode(&max, s, sizeof(s));
            if (strcasecmp(s, "1z141z3") != 0) {
                ERR("UINT32_MAX encodes as %s", s);
            }

            ulid32_t got = {};
            if (!ulid32Decode(s, &got) || got.data != UINT32_MAX) {
                ERR("ulid32 max %s did not decode", s);
            }

            char over[ULID32_ENCODED_LENGTH + 2];
            memcpy(over, s, sizeof(over));
            if (!ulidTestBase36Increment(over, ULID32_ENCODED_LENGTH)) {
                ERR("ulid32 max has no successor");
            }
            got.data = 7;
            if (ulid32Decode(over, &got) || got.data != 7) {
                ERR("ulid32 max+1 %s was accepted", over);
            }

            if (ulid32Decode("ZZZZZZZ", &got) ||
                ulid32Decode("00000000", &got) ||
                ulid32Decode("000000", &got)) {
                ERR("ulid32 accepted all-Z or a wrong length");
            }
        }

        /* 128-bit: every decoder variant */
        {
            bool (*decoders[])(const char *, ulid_t *) = {
                ulidDecode,     ulidDecodeScalar, ulidDecodeSWAR,
#if ULID_HAS_SSE2
                ulidDecodeSSE2,
#endif
#if ULID_HAS_AVX2
                ulidDecodeAVX2,
#endif
#if ULID_HAS_NEON
                ulidDecodeNEON,
#endif
            };

            ulid_t max;
            memset(max.data, 0xFF, sizeof(max.data));
            char s[ULID_ENCODED_LENGTH + 2];
            ulidEncodeScalar(&max, s, sizeof(s));

            char over[ULID_ENCODED_LENGTH + 2];
            memcpy(over, s, sizeof(over));
            if (!ulidTestBase36Increment(over, ULID_ENCODED_LENGTH)) {
                ERR("ulid max has no successor");
            }

            char tooLong[ULID_ENCODED_LENGTH + 2];
            memset(tooLong, '0', ULID_ENCODED_LENGTH + 1);
            tooLong[ULID_ENCODED_LENGTH + 1] = '\0';

            char allZ[ULID_ENCODED_LENGTH + 1];
            memset(allZ, 'Z', ULID_ENCODED_LENGTH);
            allZ[ULID_ENCODED_LENGTH] = '\0';

            for (size_t d = 0; d < countof(decoders); d++) {
                ulid_t got = {};
                if (!decoders[d](s, &got) ||
                    memcmp(got.data, max.data, sizeof(max.data)) != 0) {
                    ERR("decoder %zu: max %s did not decode", d, s);
                }

                if (decoders[d](over, &got)) {
                    ERR("decoder %zu: max+1 %s was accepted", d, over);
                }

                if (decoders[d](allZ, &got) || decoders[d](tooLong, &got) ||
                    decoders[d](tooLong + 2, &got)) {
                    ERR("decoder %zu: accepted all-Z or a wrong length", d);
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
