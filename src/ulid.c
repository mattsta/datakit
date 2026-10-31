#include "ulid.h"
#include "dkEndian.h" /* the 64-bit halves of an id are big-endian */
#include "str.h"
#include "timeUtil.h"
#include "util.h"

#include <math.h> /* ldexp: a variant's range in years */
#include <stdatomic.h>
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
/* Digit values run 0 .. ULID_B36_RADIX - 1 */
static constexpr uint8_t ULID_B36_RADIX = 36;
static constexpr char BASE36_CHARS[ULID_B36_RADIX] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
    'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};

/* Each character's Base36 digit value plus one, upper and lower case
 * alike; 0 for every other byte.  Built at compile time: decoders share
 * it from any thread with no initialization. */
#define ULID_B36_(c, v) [(c)] = (v) + 1
#define ULID_B36_LETTER_(upper, v)                                             \
    ULID_B36_(upper, v), ULID_B36_((upper) + ('a' - 'A'), v)
static constexpr uint8_t base36ValuePlus1_[256] = {
    ULID_B36_('0', 0),         ULID_B36_('1', 1),
    ULID_B36_('2', 2),         ULID_B36_('3', 3),
    ULID_B36_('4', 4),         ULID_B36_('5', 5),
    ULID_B36_('6', 6),         ULID_B36_('7', 7),
    ULID_B36_('8', 8),         ULID_B36_('9', 9),
    ULID_B36_LETTER_('A', 10), ULID_B36_LETTER_('B', 11),
    ULID_B36_LETTER_('C', 12), ULID_B36_LETTER_('D', 13),
    ULID_B36_LETTER_('E', 14), ULID_B36_LETTER_('F', 15),
    ULID_B36_LETTER_('G', 16), ULID_B36_LETTER_('H', 17),
    ULID_B36_LETTER_('I', 18), ULID_B36_LETTER_('J', 19),
    ULID_B36_LETTER_('K', 20), ULID_B36_LETTER_('L', 21),
    ULID_B36_LETTER_('M', 22), ULID_B36_LETTER_('N', 23),
    ULID_B36_LETTER_('O', 24), ULID_B36_LETTER_('P', 25),
    ULID_B36_LETTER_('Q', 26), ULID_B36_LETTER_('R', 27),
    ULID_B36_LETTER_('S', 28), ULID_B36_LETTER_('T', 29),
    ULID_B36_LETTER_('U', 30), ULID_B36_LETTER_('V', 31),
    ULID_B36_LETTER_('W', 32), ULID_B36_LETTER_('X', 33),
    ULID_B36_LETTER_('Y', 34), ULID_B36_LETTER_('Z', 35),
};
#undef ULID_B36_LETTER_
#undef ULID_B36_

/* Whether all 25 values a vector conversion gave are Base36 digits (it
 * gives a value of ULID_B36_RADIX or more for any other character) */
DK_INLINE_ALWAYS bool
ulidB36DigitsValid_(const uint8_t values[ULID_ENCODED_LENGTH]) {
    for (size_t i = 0; i < ULID_ENCODED_LENGTH; i++) {
        if (values[i] >= ULID_B36_RADIX) {
            return false;
        }
    }
    return true;
}

/* The Base36 value of character 'c', or -1 when it is not one */
DK_INLINE_ALWAYS int ulidB36Value_(const char c) {
    return (int)base36ValuePlus1_[(uint8_t)c] - 1;
}

/* ====================================================================
 * Implementation State
 * ==================================================================== */
/* The encoder in use (ulidSetEncodeImpl); a stale read is harmless: every
 * implementation writes the same characters */
static _Atomic ulidEncodeImpl currentImpl = ULID_ENCODE_AUTO;

/* What AUTO uses: the fastest this build has (perfRegression module
 * ulid measures each) */
#if ULID_HAS_AVX2
static constexpr ulidEncodeImpl ulidBestImpl_ = ULID_ENCODE_AVX2;
#elif ULID_HAS_NEON
static constexpr ulidEncodeImpl ulidBestImpl_ = ULID_ENCODE_NEON;
#elif ULID_HAS_SSE2
static constexpr ulidEncodeImpl ulidBestImpl_ = ULID_ENCODE_SSE2;
#else
static constexpr ulidEncodeImpl ulidBestImpl_ = ULID_ENCODE_SWAR;
#endif

/* The implementation ulidEncode / ulidDecode run now */
static ulidEncodeImpl ulidImplInUse_(void) {
    /* @sync: relaxed hint */
    const ulidEncodeImpl impl =
        atomic_load_explicit(&currentImpl, memory_order_relaxed);
    return impl == ULID_ENCODE_AUTO ? ulidBestImpl_ : impl;
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
        now = g->lastEmittedNs + 1;
        /* Saturate instead of wrapping: epoch-ns reaches UINT64_MAX in
         * the year 2554, and a wrapped watermark would silently break
         * the sort-order guarantee */
        if (now == 0) {
            now = UINT64_MAX;
        }
    }

    g->lastEmittedNs = now;
    return now;
}

/* ====================================================================
 * 128-bit Arithmetic Helpers
 * ==================================================================== */

/* Divide 128-bit number by 36, return quotient and remainder */
/* 36^12: twelve Base36 digits, the most a 64-bit word holds (< 2^63) */
static constexpr uint64_t ULID_B36_POW12_ = UINT64_C(2176782336) * 2176782336;

/* The 25 Base36 digit values of a 128-bit id, most significant first:
 * two 128-bit divisions split it into 12 + 12 digits (each in a 64-bit
 * word, emitted with 64-bit divisions by 36) and the leading digit */
static void ulidDigits128_(const ulid_t *id, uint8_t d[ULID_ENCODED_LENGTH]) {
    __uint128_t v =
        ((__uint128_t)dkLoadBe64(id->data) << 64) | dkLoadBe64(id->data + 8);
    uint64_t low = (uint64_t)(v % ULID_B36_POW12_);
    v /= ULID_B36_POW12_;
    uint64_t mid = (uint64_t)(v % ULID_B36_POW12_);
    d[0] = (uint8_t)(v / ULID_B36_POW12_); /* < 16: 2^128 / 36^24 */

    /* Four independent chains of six digits (each word split once at
     * 36^6): a quarter of one chain's serial division latency */
    constexpr uint64_t pow6 = 2176782336; /* 36^6 */
    uint64_t chains[4] = {mid / pow6, mid % pow6, low / pow6, low % pow6};
    for (int k = 5; k >= 0; k--) {
        for (int c = 0; c < 4; c++) {
            d[1 + c * 6 + k] = (uint8_t)(chains[c] % ULID_B36_RADIX);
            chains[c] /= ULID_B36_RADIX;
        }
    }
}

/* The id the 25 digit values (each < 36, most significant first) encode:
 * false when that is past 2^128 - 1 (25 Base36 digits reach 36^25) */
static bool ulidFromDigits128_(const uint8_t d[ULID_ENCODED_LENGTH],
                               ulid_t *out) {
    uint64_t mid = 0;
    uint64_t low = 0;
    for (int i = 1; i <= 12; i++) {
        mid = mid * ULID_B36_RADIX + d[i];
    }
    for (int i = 13; i < ULID_ENCODED_LENGTH; i++) {
        low = low * ULID_B36_RADIX + d[i];
    }
    __uint128_t v = (__uint128_t)d[0] * ULID_B36_POW12_ + mid;
    if (ckd_mul(&v, v, (__uint128_t)ULID_B36_POW12_) ||
        ckd_add(&v, v, (__uint128_t)low)) {
        return false;
    }
    dkStoreBe64(out->data, (uint64_t)(v >> 64));
    dkStoreBe64(out->data + 8, (uint64_t)v);
    return true;
}

/* The 25 digit values of 'str' (exactly 25 Base36 characters, either
 * case) into 'd'; false for any other string */
static bool ulidStrDigits_(const char *str, uint8_t d[ULID_ENCODED_LENGTH]) {
    if (strlen(str) != ULID_ENCODED_LENGTH) {
        return false;
    }
    int bad = 0;
    for (int i = 0; i < ULID_ENCODED_LENGTH; i++) {
        const int v = ulidB36Value_(str[i]);
        bad |= v;
        d[i] = (uint8_t)v;
    }
    return bad >= 0; /* -1 anywhere sets the sign bit */
}

/* ====================================================================
 * Scalar Implementation
 * ==================================================================== */
size_t ulidEncodeScalar(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    uint8_t d[ULID_ENCODED_LENGTH];
    ulidDigits128_(ulid, d);
    for (int i = 0; i < ULID_ENCODED_LENGTH; i++) {
        out[i] = BASE36_CHARS[d[i]];
    }
    out[ULID_ENCODED_LENGTH] = '\0';
    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeScalar(const char *str, ulid_t *out) {
    uint8_t d[ULID_ENCODED_LENGTH];
    return str && out && ulidStrDigits_(str, d) && ulidFromDigits128_(d, out);
}

/* ====================================================================
 * SWAR (SIMD Within A Register) Implementation
 * ==================================================================== */

/* Encoding turns eight digit values into characters at once in a 64-bit
 * word: each byte v < 36 becomes v + '0', plus 7 more ('A' - '0' - 10)
 * when v >= 10 (v + 118 then sets the byte's top bit, with no carry
 * between bytes).  Decoding reads characters through the table, as
 * scalar does. */
size_t ulidEncodeSWAR(const ulid_t *ulid, char *out, size_t outLen) {
    if (!ulid || !out || outLen < ULID_ENCODED_LENGTH + 1) {
        return 0;
    }

    uint8_t d[32] = {};
    ulidDigits128_(ulid, d);
    constexpr uint64_t ones = UINT64_C(0x0101010101010101);
    for (int i = 0; i < 32; i += 8) {
        uint64_t w;
        memcpy(&w, d + i, sizeof(w));
        const uint64_t letter = ((w + 118 * ones) >> 7) & ones;
        w += '0' * ones + 7 * letter;
        memcpy(d + i, &w, sizeof(w));
    }
    memcpy(out, d, ULID_ENCODED_LENGTH);
    out[ULID_ENCODED_LENGTH] = '\0';
    return ULID_ENCODED_LENGTH;
}

bool ulidDecodeSWAR(const char *str, ulid_t *out) {
    return ulidDecodeScalar(str, out);
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

    /* 32 bytes: the second 16-byte load reaches past the 25 digits */
    uint8_t digitValues[32] = {};
    ulidDigits128_(ulid, digitValues);

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

    if (!ulidB36DigitsValid_(values)) {
        return false;
    }

    return ulidFromDigits128_(values, out);
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

    uint8_t digitValues[32] = {}; /* padded for the 32-byte load */
    ulidDigits128_(ulid, digitValues);

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

    if (!ulidB36DigitsValid_(values)) {
        return false;
    }

    return ulidFromDigits128_(values, out);
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
    uint8_t digitValues[32] = {}; /* the second 16-byte load reaches past 25 */
    ulidDigits128_(ulid, digitValues);

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

    if (!ulidB36DigitsValid_(values)) {
        return false;
    }

    return ulidFromDigits128_(values, out);
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
    switch (ulidImplInUse_()) {
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
    switch (ulidImplInUse_()) {
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
    /* @sync: relaxed hint */
    return atomic_load_explicit(&currentImpl, memory_order_relaxed);
}

bool ulidSetEncodeImpl(ulidEncodeImpl impl) {
    if (!ulidIsImplAvailable(impl)) {
        return false;
    }
    /* @sync: relaxed hint */
    atomic_store_explicit(&currentImpl, impl, memory_order_relaxed);
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
    return (dkLoadBe64(ulid->data) | dkLoadBe64(ulid->data + 8)) == 0;
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

/* Every 64- and 32-bit variant is a time field above a low field; the
 * time field counts units of 'unitNs' from an epoch.  One row per
 * variant, read by generation, extraction, the metadata and the range
 * checks alike, so they cannot disagree. */
typedef enum ulidLowKind_ {
    ULID_LOW_RANDOM_,    /* random bits */
    ULID_LOW_COUNTER_,   /* a count from 0 within each time unit */
    ULID_LOW_SUBUNIT_,   /* the nanoseconds within the time unit */
    ULID_LOW_SNOWFLAKE_, /* machine id above a per-ms sequence */
    ULID_LOW_DUAL_,      /* 128-bit dual timestamps: no 64-bit layout */
} ulidLowKind_;

typedef struct ulidLayout_ {
    const char *name;
    const char *description;
    uint8_t width;    /* bits in the id: 32, 64 or 128 */
    uint8_t timeBits; /* the time field */
    uint8_t lowBits;  /* the low field below it */
    ulidLowKind_ low;
    uint64_t unitNs;  /* one step of the time field */
    uint64_t epochNs; /* what it counts from (EPOCHCUSTOM: the config's) */
} ulidLayout_;

static constexpr int ULID_SNOWFLAKE_SEQUENCE_BITS = 13;
static constexpr int ULID_SNOWFLAKE_MACHINE_BITS = 10;
static constexpr uint64_t ULID_MS_NS_ = 1'000'000;

static const ulidLayout_ ulidLayouts_[ULID_VARIANT_COUNT] = {
    [ULID_VARIANT_EPOCH2020] = {"EPOCH2020",
                                "48-bit ms since 2020 + 16-bit random", 64, 48,
                                16, ULID_LOW_RANDOM_, ULID_MS_NS_,
                                ULID_EPOCH_2020_NS},
    [ULID_VARIANT_EPOCH2024] = {"EPOCH2024",
                                "48-bit ms since 2024 + 16-bit random", 64, 48,
                                16, ULID_LOW_RANDOM_, ULID_MS_NS_,
                                ULID_EPOCH_2024_NS},
    [ULID_VARIANT_EPOCHCUSTOM] = {"EPOCHCUSTOM",
                                  "48-bit ms since a configured epoch + "
                                  "16-bit random",
                                  64, 48, 16, ULID_LOW_RANDOM_, ULID_MS_NS_, 0},
    [ULID_VARIANT_NS] = {"NS", "62-bit ns + 2-bit random", 64, 62, 2,
                         ULID_LOW_RANDOM_, 1, 0},
    [ULID_VARIANT_US] = {"US", "52-bit us + 12-bit random", 64, 52, 12,
                         ULID_LOW_RANDOM_, 1000, 0},
    [ULID_VARIANT_MS] = {"MS", "42-bit ms + 22-bit random", 64, 42, 22,
                         ULID_LOW_RANDOM_, ULID_MS_NS_, 0},
    [ULID_VARIANT_DUALNS] = {"DUALNS",
                             "64-bit ns + 64-bit ns (128-bit dual timestamp)",
                             128, 64, 64, ULID_LOW_DUAL_, 1, 0},
    [ULID_VARIANT_DUALNS_INTERLEAVED] =
        {"DUALNS_INTERLEAVED",
         "128-bit bit-interleaved dual ns timestamps (sortable)", 128, 64, 64,
         ULID_LOW_DUAL_, 1, 0},
    [ULID_VARIANT_MSCOUNT] = {"MSCOUNT",
                              "42-bit ms + 22-bit counter from 0 each ms", 64,
                              42, 22, ULID_LOW_COUNTER_, ULID_MS_NS_, 0},
    [ULID_VARIANT_HYBRID] = {"HYBRID", "44-bit ms + 20-bit ns within the ms",
                             64, 44, 20, ULID_LOW_SUBUNIT_, ULID_MS_NS_, 0},
    [ULID_VARIANT_SNOWFLAKE] = {"SNOWFLAKE",
                                "41-bit ms since 2020 + 10-bit machine + "
                                "13-bit sequence",
                                64, 41,
                                ULID_SNOWFLAKE_MACHINE_BITS +
                                    ULID_SNOWFLAKE_SEQUENCE_BITS,
                                ULID_LOW_SNOWFLAKE_, ULID_MS_NS_,
                                ULID_EPOCH_2020_NS},
    [ULID_VARIANT_32MS] = {"32MS",
                           "26-bit ms (an 18.6-hour window) + 6-bit random", 32,
                           26, 6, ULID_LOW_RANDOM_, ULID_MS_NS_, 0},
    [ULID_VARIANT_32S] = {"32S", "22-bit s (a 48.5-day window) + 10-bit random",
                          32, 22, 10, ULID_LOW_RANDOM_, 1'000'000'000, 0},
};

/* The layout of 'type' when it is a variant of 'width' bits (0: any),
 * else NULL */
static const ulidLayout_ *ulidLayoutOf_(ulidVariantType type, int width) {
    if ((unsigned)type >= ULID_VARIANT_COUNT || !ulidLayouts_[type].name ||
        (width && ulidLayouts_[type].width != width)) {
        return nullptr;
    }
    return &ulidLayouts_[type];
}

/* The low 'bits' bits of v */
DK_INLINE_ALWAYS uint64_t ulidLow_(uint64_t v, int bits) {
    return bits >= 64 ? v : v & ((UINT64_C(1) << bits) - 1);
}

/* time above a 'lowBits'-bit low field (time must fit the rest) */
DK_INLINE_ALWAYS uint64_t ulidPack64_(uint64_t time, uint64_t low,
                                      int lowBits) {
    return (time << lowBits) | ulidLow_(low, lowBits);
}

/* The span of a layout's time field in ns, saturating at UINT64_MAX */
static uint64_t ulidLayoutSpanNs_(const ulidLayout_ *l) {
    uint64_t span = 0;
    if (l->timeBits >= UINT64_WIDTH ||
        ckd_mul(&span, UINT64_C(1) << l->timeBits, l->unitNs)) {
        return UINT64_MAX;
    }
    return span;
}

/* ====================================================================
 * 64-bit Base36 Encoding/Decoding
 * ==================================================================== */

/* Encode 64-bit value to Base36 (13 characters) */
/* 'value' as 'len' Base36 characters and a NUL: 'len', or 0 when 'out'
 * (of 'outLen' bytes) cannot take them */
DK_INLINE_ALWAYS size_t ulidEncodeWord_(uint64_t value, const size_t len,
                                        char *out, const size_t outLen) {
    if (!out || outLen < len + 1) {
        return 0;
    }
    for (size_t i = len; i-- > 0;) {
        out[i] = BASE36_CHARS[value % 36];
        value /= 36;
    }
    out[len] = '\0';
    return len;
}

size_t ulid64Encode(const ulid64_t *id, char *out, size_t outLen) {
    return id ? ulidEncodeWord_(id->data, ULID64_ENCODED_LENGTH, out, outLen)
              : 0;
}

/* Decode Base36 string to 64-bit value */
/* The value of 'str': exactly 'len' Base36 characters (either case)
 * naming at most 'max'; false for anything else */
DK_INLINE_ALWAYS bool ulidDecodeWord_(const char *str, const size_t len,
                                      const uint64_t max, uint64_t *out) {
    if (!str || strlen(str) != len) {
        return false;
    }
    /* Two independent multiply-add chains, the leading 'split' digits and
     * the rest (each under 36^7, so neither can overflow), joined once with
     * the one overflow check: half the serial latency of one chain */
    static constexpr uint64_t pow36[8] = {
        1, 36, 1296, 46656, 1679616, 60466176, 2176782336, 78364164096};
    const size_t split = len / 2;
    uint64_t high = 0;
    uint64_t low = 0;
    int bad = 0;
    for (size_t i = 0; i < split; i++) {
        const int digit = ulidB36Value_(str[i]);
        bad |= digit;
        high = high * 36 + (uint64_t)(uint8_t)digit;
    }
    for (size_t i = split; i < len; i++) {
        const int digit = ulidB36Value_(str[i]);
        bad |= digit;
        low = low * 36 + (uint64_t)(uint8_t)digit;
    }
    uint64_t value = 0;
    if (bad < 0 || ckd_mul(&value, high, pow36[len - split]) ||
        ckd_add(&value, value, low) || value > max) {
        return false;
    }
    *out = value;
    return true;
}

bool ulid64Decode(const char *str, ulid64_t *out) {
    uint64_t value = 0;
    if (!out ||
        !ulidDecodeWord_(str, ULID64_ENCODED_LENGTH, UINT64_MAX, &value)) {
        return false;
    }
    out->data = value;
    return true;
}

/* ====================================================================
 * 32-bit Base36 Encoding/Decoding
 * ==================================================================== */

size_t ulid32Encode(const ulid32_t *id, char *out, size_t outLen) {
    return id ? ulidEncodeWord_(id->data, ULID32_ENCODED_LENGTH, out, outLen)
              : 0;
}

bool ulid32Decode(const char *str, ulid32_t *out) {
    uint64_t value = 0;
    if (!out ||
        !ulidDecodeWord_(str, ULID32_ENCODED_LENGTH, UINT32_MAX, &value)) {
        return false;
    }
    out->data = (uint32_t)value;
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

bool ulid64Generate(ulidGen *g, ulid64_t *out, ulidVariantType type) {
    ulidVariantConfig config;
    ulidVariantConfigInit(&config, type);
    return ulid64GenerateWithConfig(g, out, &config);
}

/* SNOWFLAKE's machine id and sequence for millisecond '*ms' (moved to the
 * next ms when this one's sequence is spent) */
static uint64_t ulidSnowflakeLow_(ulidGen *g, uint64_t *ms, uint16_t machine,
                                  uint64_t epochNs) {
    /* The ms never goes backwards (emission times strictly increase), and
     * one already past this clock's (see below) is used as is */
    if (*ms <= g->snowflakeLastMs) {
        *ms = g->snowflakeLastMs;
        g->snowflakeSeq++;
        if (g->snowflakeSeq >= (1U << ULID_SNOWFLAKE_SEQUENCE_BITS)) {
            /* Out of sequence numbers: take the next ms now rather than
             * wait for it, and move the emission watermark there so later
             * ids sort after this one */
            (*ms)++;
            g->snowflakeLastMs = *ms;
            g->snowflakeSeq = 0;
            const uint64_t msStart = epochNs + *ms * ULID_MS_NS_;
            if (g->lastEmittedNs < msStart) {
                g->lastEmittedNs = msStart;
            }
        }
    } else {
        g->snowflakeLastMs = *ms;
        g->snowflakeSeq = 0;
    }
    return ulidPack64_(machine, g->snowflakeSeq, ULID_SNOWFLAKE_SEQUENCE_BITS);
}

bool ulid64GenerateWithConfig(ulidGen *g, ulid64_t *out,
                              const ulidVariantConfig *config) {
    if (out) {
        out->data = 0;
    }
    const ulidLayout_ *l = config ? ulidLayoutOf_(config->type, 64) : nullptr;
    if (!g || !out || !l ||
        (l->low == ULID_LOW_SNOWFLAKE_ &&
         config->machineId >> ULID_SNOWFLAKE_MACHINE_BITS)) {
        return false; /* (a machine id past its field) */
    }

    const uint64_t epochNs = config->type == ULID_VARIANT_EPOCHCUSTOM
                                 ? config->customEpochNs
                                 : l->epochNs;
    const uint64_t now = ulidGenNextNs(g);
    if (now < epochNs) {
        return false; /* before the variant's epoch */
    }
    const uint64_t sinceNs = now - epochNs;
    uint64_t unit = sinceNs / l->unitNs;
    if (unit >> l->timeBits) {
        return false; /* past what the time field holds */
    }

    uint64_t low = 0;
    switch (l->low) {
    case ULID_LOW_RANDOM_:
        /* The top bits: xoroshiro128+'s lowest bits are its weakest */
        low = ulidGenRandom(g) >> (64 - l->lowBits);
        break;
    case ULID_LOW_SUBUNIT_:
        low = sinceNs % l->unitNs;
        break;
    case ULID_LOW_SNOWFLAKE_:
        low = ulidSnowflakeLow_(g, &unit, config->machineId, epochNs);
        if (unit >> l->timeBits) {
            return false;
        }
        break;
    case ULID_LOW_COUNTER_: /* from 0 in a new unit; see below */
    case ULID_LOW_DUAL_:
        break;
    }

    uint64_t id = ulidPack64_(unit, low, l->lowBits);

    /* Within the last id's time unit (or behind it, after a unit was
     * borrowed) a random or counted id is the last one plus one, as
     * ULID's monotonic rule has it: ids of one variant from one generator
     * strictly increase, and a unit's random start bounds how many it
     * holds.  Spending the low field carries into the time field,
     * borrowing the next unit. */
    const uint8_t key = (uint8_t)(config->type + 1);
    if ((l->low == ULID_LOW_RANDOM_ || l->low == ULID_LOW_COUNTER_) &&
        g->last64Type == key && unit <= g->last64 >> l->lowBits) {
        id = g->last64 + 1;
        if (id == 0 || (id >> l->lowBits) >> l->timeBits) {
            return false; /* the time field is spent */
        }
    }

    g->last64 = id;
    g->last64Type = key;
    out->data = id;
    return true;
}

/* ====================================================================
 * 32-bit Variant Generation
 * ==================================================================== */

bool ulid32Generate(ulidGen *g, ulid32_t *out, ulidVariantType type) {
    if (out) {
        out->data = 0;
    }
    const ulidLayout_ *l = ulidLayoutOf_(type, 32);
    if (!g || !out || !l) {
        return false;
    }

    /* The time field is a window: the Unix time modulo its span */
    const uint64_t unit = ulidLow_(ulidGenNextNs(g) / l->unitNs, l->timeBits);
    /* The random field from the generator's top bits (its strongest) */
    uint32_t id = (uint32_t)ulidPack64_(
        unit, ulidGenRandom(g) >> (64 - l->lowBits), l->lowBits);

    /* Within the last id's time unit, the last one plus one while the low
     * field has room (a window wraps, so no unit is borrowed: past the
     * room the id is random again) */
    const uint8_t key = (uint8_t)(type + 1);
    if (g->last32Type == key && unit == g->last32 >> l->lowBits &&
        ulidLow_(g->last32, l->lowBits) != ulidLow_(UINT64_MAX, l->lowBits)) {
        id = g->last32 + 1;
    }

    g->last32 = id;
    g->last32Type = key;
    out->data = id;
    return true;
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
 * Bit Interleaving by Masks (any 64-bit machine)
 * ==================================================================== */
/* The 32 bits of 'x' spread to the even bit positions of a word (bit i
 * to bit 2i): five shift-and-mask steps, each halving the gap moved */
static inline uint64_t ulidSpreadBits32(uint32_t x) {
    uint64_t v = x;
    v = (v | (v << 16)) & UINT64_C(0x0000FFFF0000FFFF);
    v = (v | (v << 8)) & UINT64_C(0x00FF00FF00FF00FF);
    v = (v | (v << 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    v = (v | (v << 2)) & UINT64_C(0x3333333333333333);
    v = (v | (v << 1)) & UINT64_C(0x5555555555555555);
    return v;
}

/* The even bits of 'v' gathered into 32 bits (bit 2i to bit i): the
 * inverse of ulidSpreadBits32 */
static inline uint32_t ulidGatherEvenBits64(uint64_t v) {
    v &= UINT64_C(0x5555555555555555);
    v = (v | (v >> 1)) & UINT64_C(0x3333333333333333);
    v = (v | (v >> 2)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    v = (v | (v >> 4)) & UINT64_C(0x00FF00FF00FF00FF);
    v = (v | (v >> 8)) & UINT64_C(0x0000FFFF0000FFFF);
    v = (v | (v >> 16));
    return (uint32_t)v;
}

/* interleaveBits64's layout by spreading each 32-bit half */
static inline __uint128_t interleaveBits64Masks(uint64_t ts1, uint64_t ts2) {
    const uint64_t lo =
        ulidSpreadBits32((uint32_t)ts1) | ulidSpreadBits32((uint32_t)ts2) << 1;
    const uint64_t hi = ulidSpreadBits32((uint32_t)(ts1 >> 32)) |
                        ulidSpreadBits32((uint32_t)(ts2 >> 32)) << 1;
    return ((__uint128_t)hi << 64) | lo;
}

/* deinterleaveBits64 by gathering the even (ts1) and odd (ts2) bits of
 * each 64-bit half */
static inline void deinterleaveBits64Masks(__uint128_t interleaved,
                                           uint64_t *ts1, uint64_t *ts2) {
    const uint64_t lo = (uint64_t)interleaved;
    const uint64_t hi = (uint64_t)(interleaved >> 64);
    *ts1 = (uint64_t)ulidGatherEvenBits64(hi) << 32 | ulidGatherEvenBits64(lo);
    *ts2 = (uint64_t)ulidGatherEvenBits64(hi >> 1) << 32 |
           ulidGatherEvenBits64(lo >> 1);
}

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
 * Dispatch Functions - Select Best Implementation
 * ==================================================================== */

/* Implementation selection for interleaving */
typedef enum interleaveImpl {
    INTERLEAVE_SCALAR = 0, /* bit by bit: the reference */
    INTERLEAVE_MASKS,      /* shift-and-mask spreading, any machine */
    INTERLEAVE_AVX2,       /* BMI2 pdep/pext */
    INTERLEAVE_AUTO
} interleaveImpl;

static interleaveImpl g_interleaveImpl = INTERLEAVE_AUTO;

/* 'impl', with AUTO as the fastest this build has */
static inline interleaveImpl interleaveImplResolve(interleaveImpl impl) {
    if (impl != INTERLEAVE_AUTO) {
        return impl;
    }

#if defined(__AVX2__) && defined(__BMI2__)
    return INTERLEAVE_AVX2;
#else
    return INTERLEAVE_MASKS;
#endif
}

#ifdef DATAKIT_TEST
/* Set implementation (for benchmarking) */
static void setInterleaveImpl(interleaveImpl impl) {
    g_interleaveImpl = impl;
}

/* The implementation in use, AUTO resolved */
static interleaveImpl getInterleaveImpl(void) {
    return interleaveImplResolve(g_interleaveImpl);
}
#endif

/* Dispatch wrapper for interleave */
static inline __uint128_t interleaveBits64Dispatch(uint64_t ts1, uint64_t ts2) {
    switch (interleaveImplResolve(g_interleaveImpl)) {
#if defined(__AVX2__) && defined(__BMI2__)
    case INTERLEAVE_AVX2:
        return interleaveBits64AVX2(ts1, ts2);
#endif
    case INTERLEAVE_MASKS:
        return interleaveBits64Masks(ts1, ts2);
    case INTERLEAVE_SCALAR:
    default:
        return interleaveBits64(ts1, ts2);
    }
}

/* Dispatch wrapper for deinterleave */
static inline void deinterleaveBits64Dispatch(__uint128_t interleaved,
                                              uint64_t *ts1, uint64_t *ts2) {
    switch (interleaveImplResolve(g_interleaveImpl)) {
#if defined(__AVX2__) && defined(__BMI2__)
    case INTERLEAVE_AVX2:
        deinterleaveBits64AVX2(interleaved, ts1, ts2);
        break;
#endif
    case INTERLEAVE_MASKS:
        deinterleaveBits64Masks(interleaved, ts1, ts2);
        break;
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

/* The time an id of layout 'l' names, from its time field and (HYBRID)
 * its sub-unit nanoseconds: ns since the layout's epoch */
static uint64_t ulidLayoutSinceNs_(const ulidLayout_ *l, uint64_t bits) {
    uint64_t since = (bits >> l->lowBits) * l->unitNs;
    if (l->low == ULID_LOW_SUBUNIT_) {
        since += ulidLow_(bits, l->lowBits);
    }
    return since;
}

uint64_t ulid64GetTimestampNs(const ulid64_t *id, ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 64);
    if (!id || !l) {
        return 0;
    }
    /* EPOCHCUSTOM: since its epoch (the config holds it; the caller adds
     * it) */
    return l->epochNs + ulidLayoutSinceNs_(l, id->data);
}

uint64_t ulid64GetRandom(const ulid64_t *id, ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 64);
    if (!id || !l) {
        return 0;
    }
    return ulidLow_(id->data, l->lowBits);
}

/* ====================================================================
 * Variant-Specific Extraction
 * ==================================================================== */

uint32_t ulid64GetCounter(const ulid64_t *id) {
    return (uint32_t)ulid64GetRandom(id, ULID_VARIANT_MSCOUNT);
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
    return (uint32_t)ulid64GetRandom(id, ULID_VARIANT_HYBRID);
}

/* ====================================================================
 * 32-bit Timestamp Extraction
 * ==================================================================== */

uint64_t ulid32GetTimestampNs(const ulid32_t *id, ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 32);
    if (!id || !l) {
        return 0;
    }
    return ulidLayoutSinceNs_(l, id->data);
}

uint32_t ulid32GetRandom(const ulid32_t *id, ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 32);
    if (!id || !l) {
        return 0;
    }
    return (uint32_t)ulidLow_(id->data, l->lowBits);
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
    if (!ulid64Generate(g, &id, type)) {
        return 0;
    }
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
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    return l ? l->name : "UNKNOWN";
}

const char *ulid64GetVariantDescription(ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    return l ? l->description : "Unknown variant";
}

double ulid64GetVariantRangeYears(ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    if (!l) {
        return 0.0;
    }
    /* A Julian year of nanoseconds; the span exactly, in double */
    constexpr double nsPerYear = 365.25 * 86400.0 * 1e9;
    return ldexp((double)l->unitNs, l->timeBits) / nsPerYear;
}

uint8_t ulid64GetVariantRandomBits(ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    return l && l->low == ULID_LOW_RANDOM_ ? l->lowBits : 0;
}

const char *ulid64GetVariantPrecision(ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    if (!l) {
        return "unknown";
    }
    /* HYBRID's low field carries the nanoseconds below its unit */
    switch (l->low == ULID_LOW_SUBUNIT_ ? 1 : l->unitNs) {
    case 1:
        return "ns";
    case 1000:
        return "us";
    case 1'000'000:
        return "ms";
    default:
        return "s";
    }
}

/* ====================================================================
 * Validation API
 * ==================================================================== */

bool ulid64IsValidVariantType(ulidVariantType type) {
    return type >= 0 && type < ULID_VARIANT_COUNT;
}

bool ulid64ValidateTimestamp(uint64_t timestampNs, ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    if (!l) {
        return false;
    }
    if (l->width != 64) {
        return true; /* a window, or a full 64-bit dual timestamp */
    }
    return timestampNs >= l->epochNs &&
           timestampNs <= ulid64GetMaxTimestamp(type);
}

uint64_t ulid64GetMaxTimestamp(ulidVariantType type) {
    const ulidLayout_ *l = ulidLayoutOf_(type, 0);
    if (!l) {
        return 0;
    }
    if (l->width == 128) {
        return UINT64_MAX;
    }
    /* The time field's last nanosecond after its epoch (a 32-bit
     * variant's epoch is 0: its window) */
    const uint64_t span = ulidLayoutSpanNs_(l);
    uint64_t last = 0;
    if (span == UINT64_MAX || ckd_add(&last, l->epochNs, span - 1)) {
        return UINT64_MAX; /* past what 64-bit Unix ns holds */
    }
    return last;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "dkRandom.h" /* replayable test ids */
#include "perf.h"
#include <stdio.h>
#include <unistd.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(ulid_t *, zfree, Ulids);
defcleanerName(ulid64_t *, zfree, Ulid64s);

/* Add one to a fixed-width uppercase Base36 string, digit by digit.
 * Returns false if the carry runs off the front. */
/* The reference encoding: byte-wise long division of the 16 big-endian
 * bytes by 36, 25 times (slow, and independent of the codec's 128-bit
 * arithmetic) */
static void ulidTestReferenceEncode(const ulid_t *id,
                                    char out[ULID_ENCODED_LENGTH + 1]) {
    uint8_t num[16];
    memcpy(num, id->data, sizeof(num));
    for (int digit = ULID_ENCODED_LENGTH - 1; digit >= 0; digit--) {
        uint32_t carry = 0;
        for (int i = 0; i < 16; i++) {
            const uint32_t cur = carry * 256 + num[i];
            num[i] = (uint8_t)(cur / 36);
            carry = cur % 36;
        }
        out[digit] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[carry];
    }
    out[ULID_ENCODED_LENGTH] = '\0';
}

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
static bool ulidTestGenerateAt(ulid64_t *out, ulidVariantType type,
                               uint64_t atNs, uint64_t *before,
                               uint64_t *after) {
    ulidGen g;
    ulidGenInit(&g, ULID_CLOCK_MONOTONIC_OFFSET);
    /* Unsigned arithmetic: monotonic + offset is atNs modulo 2^64 */
    g.clockOffsetNs = atNs - timeUtilMonotonicNs();
    *before = timeUtilMonotonicNs() + g.clockOffsetNs;
    const bool made = ulid64Generate(&g, out, type);
    *after = timeUtilMonotonicNs() + g.clockOffsetNs;
    return made;
}

/* Each 64- and 32-bit variant's layout as the header states it, apart
 * from the code's table: epoch, time-field bits, unit, low-field bits,
 * random bits, precision and range in years */
static const struct ulidTestLayout {
    ulidVariantType type;
    uint64_t epochNs;
    uint8_t timeBits;
    uint64_t unitNs;
    uint8_t lowBits;
    uint8_t randomBits;
    const char *precision;
    double years;
} ulidTestLayouts_[] = {
    {ULID_VARIANT_EPOCH2020, ULID_EPOCH_2020_NS, 48, 1'000'000, 16, 16, "ms",
     8919.4},
    {ULID_VARIANT_EPOCH2024, ULID_EPOCH_2024_NS, 48, 1'000'000, 16, 16, "ms",
     8919.4},
    {ULID_VARIANT_NS, 0, 62, 1, 2, 2, "ns", 146.14},
    {ULID_VARIANT_US, 0, 52, 1000, 12, 12, "us", 142.71},
    {ULID_VARIANT_MS, 0, 42, 1'000'000, 22, 22, "ms", 139.37},
    {ULID_VARIANT_MSCOUNT, 0, 42, 1'000'000, 22, 0, "ms", 139.37},
    {ULID_VARIANT_HYBRID, 0, 44, 1'000'000, 20, 0, "ns", 557.46},
    {ULID_VARIANT_SNOWFLAKE, ULID_EPOCH_2020_NS, 41, 1'000'000, 23, 0, "ms",
     69.68},
    {ULID_VARIANT_32MS, 0, 26, 1'000'000, 6, 6, "ms", 0.0021266},
    {ULID_VARIANT_32S, 0, 22, 1'000'000'000, 10, 10, "s", 0.13291},
};

typedef __uint128_t ulidInterleaveFn(uint64_t ts1, uint64_t ts2);
typedef void ulidDeinterleaveFn(__uint128_t interleaved, uint64_t *ts1,
                                uint64_t *ts2);

/* 'name''s interleave against the bit-by-bit reference and its
 * deinterleave as the inverse, over the 'n' pairs (a[i], b[i]); returns
 * the failures */
static int32_t ulidInterleaveCheck_(const char *name, ulidInterleaveFn *inter,
                                    ulidDeinterleaveFn *deinter,
                                    const uint64_t *a, const uint64_t *b,
                                    size_t n) {
    int32_t err = 0;
    for (size_t i = 0; i < n; i++) {
        const __uint128_t got = inter(a[i], b[i]);
        if (got != interleaveBits64(a[i], b[i])) {
            ERR("%s: pair %zu interleaves unlike the reference", name, i);
        }

        uint64_t x;
        uint64_t y;
        deinter(got, &x, &y);
        if (x != a[i] || y != b[i]) {
            ERR("%s: pair %zu came back (%" PRIu64 ",%" PRIu64
                "), want (%" PRIu64 ",%" PRIu64 ")",
                name, i, x, y, a[i], b[i]);
        }
    }

    return err;
}

/* Times 'inter' then 'deinter' (called directly, so inlined as the
 * dispatch inlines them) over 'iterations' calls each */
#define ULID_INTERLEAVE_BENCH(name, inter, deinter, iterations, ts1, ts2)      \
    do {                                                                       \
        [[maybe_unused]] volatile __uint128_t sink_ = 0;                       \
        [[maybe_unused]] volatile uint64_t sink1_ = 0;                         \
        [[maybe_unused]] volatile uint64_t sink2_ = 0;                         \
        printf("\n      " name " interleave:\n");                              \
        PERF_TIMERS_SETUP;                                                     \
        for (size_t i_ = 0; i_ < (iterations); i_++) {                         \
            sink_ = inter((ts1) + i_, (ts2) + i_);                             \
        }                                                                      \
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations, name " interleave ops");  \
        printf("      " name " deinterleave:\n");                              \
        const __uint128_t from_ = inter(ts1, ts2);                             \
        PERF_TIMERS_SETUP;                                                     \
        for (size_t i_ = 0; i_ < (iterations); i_++) {                         \
            uint64_t a_;                                                       \
            uint64_t b_;                                                       \
            deinter(from_ + i_, &a_, &b_);                                     \
            sink1_ = a_;                                                       \
            sink2_ = b_;                                                       \
        }                                                                      \
        PERF_TIMERS_FINISH_PRINT_RESULTS(iterations,                           \
                                         name " deinterleave ops");            \
    } while (0)

int ulidTest(int, char *[]) {
    int32_t err = 0;

    TEST("the decode table gives every byte its Base36 value or -1") {
        for (int c = 0; c < 256; c++) {
            int want = -1;
            if (c >= '0' && c <= '9') {
                want = c - '0';
            } else if (c >= 'A' && c <= 'Z') {
                want = c - 'A' + 10;
            } else if (c >= 'a' && c <= 'z') {
                want = c - 'a' + 10;
            }
            if (ulidB36Value_((char)c) != want) {
                ERR("byte %d decodes as %d, want %d", c, ulidB36Value_((char)c),
                    want);
            }
        }
    }

    TEST("every implementation encodes as the reference long division and "
         "decodes back") {
        /* Edge values (0, 2^64 - 1, 2^64, 36^24 - 1, 36^24, 2^128 - 1)
         * and 20,000 replayable random ids: each available implementation
         * writes the reference's characters and reads them (and their
         * lowercase) back to the same id */
        uint64_t edges[][2] = {{0, 0}, {0, UINT64_MAX},         {1, 0}, {0, 0},
                               {0, 0}, {UINT64_MAX, UINT64_MAX}};
        /* 36^24 = 36^12 squared, as 128 bits */
        const __uint128_t p24 =
            (__uint128_t)ULID_B36_POW12_ * (__uint128_t)ULID_B36_POW12_;
        edges[3][0] = (uint64_t)((p24 - 1) >> 64);
        edges[3][1] = (uint64_t)(p24 - 1);
        edges[4][0] = (uint64_t)(p24 >> 64);
        edges[4][1] = (uint64_t)p24;
        uint64_t state = 0x1d;
        const ulidEncodeImpl was = ulidGetEncodeImpl();
        for (int impl = 0; impl < ULID_ENCODE_AUTO; impl++) {
            if (!ulidSetEncodeImpl((ulidEncodeImpl)impl)) {
                continue;
            }
            for (int i = 0; i < 20'000 + (int)countof(edges); i++) {
                ulid_t id;
                if (i < (int)countof(edges)) {
                    dkStoreBe64(id.data, edges[i][0]);
                    dkStoreBe64(id.data + 8, edges[i][1]);
                } else {
                    dkStoreBe64(id.data, dkSplitmix64Next(&state));
                    dkStoreBe64(id.data + 8, dkSplitmix64Next(&state));
                }
                char want[ULID_ENCODED_LENGTH + 1];
                char got[ULID_ENCODED_LENGTH + 1];
                ulidTestReferenceEncode(&id, want);
                ulid_t back;
                ulid_t backLower;
                char lower[ULID_ENCODED_LENGTH + 1];
                const size_t n = ulidEncode(&id, got, sizeof(got));
                for (int c = 0; c <= ULID_ENCODED_LENGTH; c++) {
                    lower[c] = (char)(got[c] >= 'A' && got[c] <= 'Z'
                                          ? got[c] - 'A' + 'a'
                                          : got[c]);
                }
                if (n != ULID_ENCODED_LENGTH || strcmp(got, want) ||
                    !ulidDecode(got, &back) || memcmp(back.data, id.data, 16) ||
                    !ulidDecode(lower, &backLower) ||
                    memcmp(backLower.data, id.data, 16)) {
                    ERR("%s, id %d: %s (reference %s)",
                        ulidGetImplName((ulidEncodeImpl)impl), i, got, want);
                    break;
                }
            }
        }
        ulidSetEncodeImpl(was);
    }

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
        /* EPOCHCUSTOM's bound is an offset: 2^48 ms, past 64-bit ns */
        if (ulid64GetMaxTimestamp(ULID_VARIANT_DUALNS) != UINT64_MAX ||
            ulid64GetMaxTimestamp(ULID_VARIANT_COUNT) != 0 ||
            ulid64GetMaxTimestamp(ULID_VARIANT_EPOCHCUSTOM) != UINT64_MAX) {
            ERR("max timestamps");
        }
    }

    TEST("every variant has a name and a description") {
        for (int t = 0; t < ULID_VARIANT_COUNT; t++) {
            if (strcmp(ulid64GetVariantName((ulidVariantType)t), "UNKNOWN") ==
                    0 ||
                strcmp(ulid64GetVariantDescription((ulidVariantType)t),
                       "Unknown variant") == 0) {
                ERR("variant %d has no name or description", t);
            }
        }

        if (strcmp(ulid64GetVariantName(ULID_VARIANT_COUNT), "UNKNOWN") != 0) {
            ERR("a type past the last variant has a name");
        }
    }

    TEST("every 64-bit variant round-trips a timestamp in its range") {
        /* At the middle of each variant's valid range and near its top,
         * the extracted timestamp is the generation instant to the
         * variant's precision, the random/sequence part fits its bits,
         * and the range check agrees */
        for (size_t v = 0; v < countof(ulidTestLayouts_); v++) {
            const struct ulidTestLayout *t = &ulidTestLayouts_[v];
            if (t->type == ULID_VARIANT_32MS || t->type == ULID_VARIANT_32S) {
                continue;
            }
            const ulidVariantType type = t->type;
            const uint64_t lo = t->epochNs;
            const uint64_t hi = ulid64GetMaxTimestamp(type);
            const uint64_t prec = strcmp(t->precision, "ns") ? t->unitNs : 1;
            const uint64_t points[2] = {lo + (hi - lo) / 2, hi - 10'000'000};
            for (size_t p = 0; p < 2; p++) {
                ulid64_t id;
                uint64_t before = 0;
                uint64_t after = 0;
                const bool made =
                    ulidTestGenerateAt(&id, type, points[p], &before, &after);
                const uint64_t got = ulid64GetTimestampNs(&id, type);
                const uint64_t rnd = ulid64GetRandom(&id, type);
                /* At precision: the instant less its sub-unit part */
                const uint64_t floorBefore = lo + (before - lo) / prec * prec;
                if (!made || got < floorBefore || got > after ||
                    !ulid64ValidateTimestamp(got, type) || rnd >> t->lowBits) {
                    ERR("%s at %" PRIu64 ": got %" PRIu64 " outside [%" PRIu64
                        ", %" PRIu64 "], low field %" PRIu64 " of %u bits",
                        ulid64GetVariantName(type), points[p], got, before,
                        after, rnd, t->lowBits);
                }
            }
            if (!ulid64ValidateTimestamp(hi, type) ||
                ulid64ValidateTimestamp(hi + 1, type) ||
                (lo > 0 && ulid64ValidateTimestamp(lo - 1, type))) {
                ERR("%s: range check wrong at its bounds",
                    ulid64GetVariantName(type));
            }
            if (!ulid64IsValidVariantType(type) ||
                strcmp(ulid64GetVariantName(type), "UNKNOWN") == 0 ||
                strcmp(ulid64GetVariantDescription(type), "Unknown variant") ==
                    0 ||
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

    TEST(
        "every 64-bit variant holds the real clock: its timestamp reads back") {
        /* Generated now, with the wall clock: each id is made, its
         * timestamp is the generation instant to the variant's precision
         * (EPOCHCUSTOM's offset plus its epoch), and now is in range */
        for (size_t v = 0; v < countof(ulidTestLayouts_); v++) {
            const struct ulidTestLayout *t = &ulidTestLayouts_[v];
            if (t->type == ULID_VARIANT_32MS || t->type == ULID_VARIANT_32S) {
                continue;
            }
            ulidGen gen;
            ulidGenInit(&gen, ULID_CLOCK_REALTIME);
            const uint64_t before = timeUtilNs();
            ulid64_t id;
            const bool made = ulid64Generate(&gen, &id, t->type);
            const uint64_t after = timeUtilNs();
            const uint64_t prec = strcmp(t->precision, "ns") ? t->unitNs : 1;
            const uint64_t got = ulid64GetTimestampNs(&id, t->type);
            const uint64_t floorBefore =
                t->epochNs + (before - t->epochNs) / prec * prec;
            if (!made || got < floorBefore || got > after ||
                !ulid64ValidateTimestamp(before, t->type)) {
                ERR("%s now: made %d, timestamp %" PRIu64 " not in [%" PRIu64
                    ", %" PRIu64 "]",
                    ulid64GetVariantName(t->type), made, got, floorBefore,
                    after);
            }
        }

        ulidGen gen;
        ulidGenInit(&gen, ULID_CLOCK_REALTIME);
        const ulidVariantConfig custom = {.type = ULID_VARIANT_EPOCHCUSTOM,
                                          .customEpochNs = ULID_EPOCH_2020_NS};
        const uint64_t before = timeUtilNs();
        ulid64_t id;
        const bool made = ulid64GenerateWithConfig(&gen, &id, &custom);
        const uint64_t after = timeUtilNs();
        const uint64_t at = ULID_EPOCH_2020_NS +
                            ulid64GetTimestampNs(&id, ULID_VARIANT_EPOCHCUSTOM);
        if (!made || at + 1'000'000 <= before || at > after) {
            ERR("EPOCHCUSTOM now: offset plus epoch %" PRIu64
                " not the generation instant",
                at);
        }
    }

    TEST("variant metadata matches the stated layouts") {
        /* Name, range in years (to 0.1%), random bits, precision, and the
         * range bounds: the epoch is the first instant held, the epoch
         * plus the span less 1 ns the last */
        for (size_t v = 0; v < countof(ulidTestLayouts_); v++) {
            const struct ulidTestLayout *t = &ulidTestLayouts_[v];
            const double years = ulid64GetVariantRangeYears(t->type);
            const uint64_t max = ulid64GetMaxTimestamp(t->type);
            const bool is64 =
                t->type != ULID_VARIANT_32MS && t->type != ULID_VARIANT_32S;
            /* Saturating at UINT64_MAX: 2^48 ms of EPOCH runs past what a
             * 64-bit Unix-ns timestamp holds (the year 2554) */
            const __uint128_t last128 =
                (__uint128_t)t->epochNs +
                ((__uint128_t)1 << t->timeBits) * t->unitNs - 1;
            const uint64_t wantMax =
                last128 > UINT64_MAX ? UINT64_MAX : (uint64_t)last128;
            if (fabs(years - t->years) > t->years * 0.001 ||
                ulid64GetVariantRandomBits(t->type) != t->randomBits ||
                strcmp(ulid64GetVariantPrecision(t->type), t->precision) ||
                max != wantMax ||
                (is64 && (!ulid64ValidateTimestamp(max, t->type) ||
                          ulid64ValidateTimestamp(max + 1, t->type)))) {
                ERR("%s: %.6f years (want %.6f), %u random bits, %s, max "
                    "%" PRIu64 " (want %" PRIu64 ")",
                    ulid64GetVariantName(t->type), years, t->years,
                    ulid64GetVariantRandomBits(t->type),
                    ulid64GetVariantPrecision(t->type), max, wantMax);
            }
        }
        if (fabs(ulid64GetVariantRangeYears(ULID_VARIANT_DUALNS) - 584.55) >
                0.6 ||
            ulid64GetVariantRandomBits(ULID_VARIANT_DUALNS) != 0 ||
            ulid64GetMaxTimestamp(ULID_VARIANT_DUALNS) != UINT64_MAX ||
            ulid64GetVariantRangeYears(ULID_VARIANT_COUNT) != 0.0) {
            ERR("dual-timestamp or unknown-type metadata");
        }
    }

    TEST("ids of one variant strictly increase within one time unit, and "
         "a spent low field borrows the next unit") {
        /* The watermark set to a ms boundary a second ahead: 70,000 ids
         * land in one ms (1 ns apart).  Every random or counted variant
         * gives strictly increasing ids; MSCOUNT's counter is the id's
         * position; EPOCH2024's 16 random bits run out within 65,536 ids
         * and the ids go on into the next ms (each ms's field starts at a
         * random value, so the burst may spend more than one) */
        static const ulidVariantType burst[] = {
            ULID_VARIANT_EPOCH2024, ULID_VARIANT_NS,      ULID_VARIANT_US,
            ULID_VARIANT_MS,        ULID_VARIANT_MSCOUNT, ULID_VARIANT_HYBRID};
        for (size_t v = 0; v < countof(burst); v++) {
            ulidGen gen;
            ulidGenInit(&gen, ULID_CLOCK_REALTIME);
            gen.lastEmittedNs = (timeUtilNs() / 1'000'000 + 1000) * 1'000'000;
            ulid64_t first = {};
            ulid64_t prev = {};
            for (uint32_t i = 0; i < 70'000; i++) {
                ulid64_t id;
                if (!ulid64Generate(&gen, &id, burst[v]) ||
                    (i && ulid64Compare(&prev, &id) >= 0) ||
                    (burst[v] == ULID_VARIANT_MSCOUNT &&
                     ulid64GetCounter(&id) != i)) {
                    ERR("%s burst id %" PRIu32 " made or ordered wrong",
                        ulid64GetVariantName(burst[v]), i);
                    break;
                }
                if (!i) {
                    first = id;
                }
                prev = id;
            }
            const uint64_t borrowed =
                ulid64GetTimestampNs(&prev, ULID_VARIANT_EPOCH2024) -
                ulid64GetTimestampNs(&first, ULID_VARIANT_EPOCH2024);
            if (burst[v] == ULID_VARIANT_EPOCH2024 &&
                (borrowed < 1'000'000 || borrowed % 1'000'000 != 0 ||
                 borrowed >= 1'000'000'000)) {
                ERR("EPOCH2024 did not borrow the next ms once its low "
                    "field was spent");
            }
        }

        /* 32-bit: within one unit, each id follows the last while the low
         * field has room above it (a window wraps: no unit is borrowed) */
        ulidGen gen;
        ulidGenInit(&gen, ULID_CLOCK_REALTIME);
        gen.lastEmittedNs = (timeUtilNs() / 1'000'000'000 + 10) * 1'000'000'000;
        ulid32_t prev32 = {};
        uint32_t followed = 0;
        for (uint32_t i = 0; i < 64; i++) {
            ulid32_t id;
            const bool room = i && ulid32GetRandom(&prev32, ULID_VARIANT_32S) <
                                       (1u << 10) - 1;
            if (!ulid32Generate(&gen, &id, ULID_VARIANT_32S) ||
                (room && id.data != prev32.data + 1)) {
                ERR("32S burst id %" PRIu32 " made or ordered wrong", i);
                break;
            }
            followed += room;
            prev32 = id;
        }
        if (!followed) {
            ERR("no 32S id followed the one before");
        }
    }

    TEST("a clock outside a variant's range generates no id") {
        /* Before EPOCH2024's epoch, one unit past NS's and MSCOUNT's
         * last instant, and SNOWFLAKE with a machine id past 10 bits:
         * false, and the id is zero */
        static const struct {
            ulidVariantType type;
            bool beforeEpoch;
        } outside[] = {{ULID_VARIANT_EPOCH2024, true},
                       {ULID_VARIANT_NS, false},
                       {ULID_VARIANT_MSCOUNT, false},
                       {ULID_VARIANT_SNOWFLAKE, false}};
        for (size_t o = 0; o < countof(outside); o++) {
            const ulidVariantType type = outside[o].type;
            const uint64_t at = outside[o].beforeEpoch
                                    ? ULID_EPOCH_2024_NS - 1'000'000'000
                                    : ulid64GetMaxTimestamp(type) + 2'000'000;
            ulid64_t id = {.data = 7};
            uint64_t before = 0;
            uint64_t after = 0;
            if (ulidTestGenerateAt(&id, type, at, &before, &after) ||
                !ulid64IsZero(&id)) {
                ERR("%s generated outside its range",
                    ulid64GetVariantName(type));
            }
        }
        ulidGen gen;
        ulidGenInit(&gen, ULID_CLOCK_REALTIME);
        const ulidVariantConfig wide = {.type = ULID_VARIANT_SNOWFLAKE,
                                        .machineId = 1024};
        ulid64_t id = {.data = 7};
        if (ulid64GenerateWithConfig(&gen, &id, &wide) || !ulid64IsZero(&id) ||
            ulid64Generate(&gen, &id, ULID_VARIANT_DUALNS) ||
            ulid32Generate(&gen, &(ulid32_t){}, ULID_VARIANT_MS)) {
            ERR("a bad machine id or a wrong-width type generated an id");
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
        ulid64Generate(&g, &a, ULID_VARIANT_MSCOUNT);
        ulid64Generate(&g, &b, ULID_VARIANT_MSCOUNT);
        /* MSCOUNT: 42-bit ms + 22-bit counter, ids rising */
        if (ulid64GetCounter(&a) >> 22 || ulid64Compare(&a, &b) >= 0) {
            ERR("MSCOUNT counter outside 22 bits or ids not rising");
        }
        ulid64Generate(&g, &a, ULID_VARIANT_HYBRID);
        if (ulid64GetNsDelta(&a) >= 1'000'000 ||
            ulid64GetTimestampNs(&a, ULID_VARIANT_HYBRID) % 1'000'000 !=
                ulid64GetNsDelta(&a)) {
            ERR("HYBRID's ns within the ms is not the timestamp's sub-ms part");
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
        /* MSCOUNT counts within each ms: no two ids collide */
        const int32_t numIds = 10000;
        cleanerName(zfree, Ulid64s) ulid64_t *ids =
            zcalloc(numIds, sizeof(ulid64_t));

        for (int32_t i = 0; i < numIds; i++) {
            ulid64Generate(&g, &ids[i], ULID_VARIANT_MSCOUNT);
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
            ERR("Found %d collisions in %d IDs (MSCOUNT should have zero)",
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
            ULID_VARIANT_MS,        ULID_VARIANT_MSCOUNT,
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
        const uint64_t before = timeUtilNs();
        const bool made = ulid64GenerateWithConfig(&g, &id, &config);
        const uint64_t after = timeUtilNs();

        /* The offset from the custom epoch, to the ms */
        const uint64_t offset =
            ulid64GetTimestampNs(&id, ULID_VARIANT_EPOCHCUSTOM);
        if (!made || offset % 1'000'000 ||
            ULID_EPOCH_2020_NS + offset + 1'000'000 <= before ||
            ULID_EPOCH_2020_NS + offset > after) {
            ERR("custom epoch offset %" PRIu64 " is not the time since 2020",
                offset);
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

        /* Then each single bit on either side, then replayable random
         * pairs: every output bit position is reached from both inputs */
        constexpr size_t pairs = 10 + 2 * 64 + 256;
        uint64_t a[pairs];
        uint64_t b[pairs];
        size_t n = 0;
        for (; n < num_tests; n++) {
            a[n] = test_ts1[n];
            b[n] = test_ts2[n];
        }

        for (uint32_t bit = 0; bit < 64; bit++) {
            a[n] = UINT64_C(1) << bit;
            b[n++] = 0;
            a[n] = 0;
            b[n++] = UINT64_C(1) << bit;
        }

        uint64_t lcg = 0x5EED;
        while (n < pairs) {
            a[n] = dkLcg64Next(&lcg);
            b[n++] = dkLcg64Next(&lcg);
        }

        err += ulidInterleaveCheck_("SCALAR", interleaveBits64,
                                    deinterleaveBits64, a, b, pairs);
        err += ulidInterleaveCheck_("MASKS", interleaveBits64Masks,
                                    deinterleaveBits64Masks, a, b, pairs);
#if defined(__AVX2__) && defined(__BMI2__)
        err += ulidInterleaveCheck_("AVX2", interleaveBits64AVX2,
                                    deinterleaveBits64AVX2, a, b, pairs);
#endif
    }

    /* ================================================================
     * DUALNS_INTERLEAVED Performance Benchmarks
     * ================================================================ */
    TEST("Bit interleaving performance - each implementation") {
        const size_t iterations = 1'000'000;
        printf("    Benchmarking bit interleaving implementations (%zu "
               "iterations):\n",
               iterations);

        const uint64_t ts1 = timeUtilNs();
        const uint64_t ts2 = timeUtilNs();
        ULID_INTERLEAVE_BENCH("SCALAR", interleaveBits64, deinterleaveBits64,
                              iterations, ts1, ts2);
        ULID_INTERLEAVE_BENCH("MASKS", interleaveBits64Masks,
                              deinterleaveBits64Masks, iterations, ts1, ts2);
#if defined(__AVX2__) && defined(__BMI2__)
        ULID_INTERLEAVE_BENCH("AVX2", interleaveBits64AVX2,
                              deinterleaveBits64AVX2, iterations, ts1, ts2);
#endif
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
        static const char *const implNames[] = {
            [INTERLEAVE_SCALAR] = "SCALAR",
            [INTERLEAVE_MASKS] = "MASKS",
            [INTERLEAVE_AVX2] = "AVX2",
        };
        const interleaveImpl selected = getInterleaveImpl();
        if ((size_t)selected >= countof(implNames)) {
            ERR("AUTO resolved to no implementation (%d)", (int)selected);
        } else {
            printf("\n      Auto-selected implementation: %s\n",
                   implNames[selected]);
        }

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
