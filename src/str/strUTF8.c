#include "../str.h"

/* ====================================================================
 * UTF-8 strlen (with large custom modifications)
 * ==================================================================== */
/* UTF-8 counting initially adapted from Colin's website then modified
 * for more efficient datakit usage. */

/* We allow disabling the NULL checks while iterating over the string
 * because for all our use cases, we already know the full byte length.
 * All we need to determine is the count of how many UTF-8 characters are
 * in the byte array.
 *
 * Since we already know the length of our target byte string, we can
 * avoid the NULL check during each loop.  Avoiding O(N) NULL checks
 * increases throughput a significant amount. */
#define STRLEN_UTF8_CHECK_NULL 0

/* UTF8_STEP is a #define to make it easier to test different
 * step sizes. (e.g. replace with uint32_t or even __uint128_t to
 * see how various optimizations and fetching sizes impact iteration
 * throughput.) */
#define STRLEN_UTF8_STEP size_t
#define STRLEN_UTF8_STEP_SIZE sizeof(STRLEN_UTF8_STEP)

/* ONEMASK is a word (if STEP is size_t) of 0x01 bytes.
 *   64-bit: 0000000100000001000000010000000100000001000000010000000100000001
 *   32-bit: 00000001000000010000000100000001
 */
#define ONEMASK ((STRLEN_UTF8_STEP)(-1) / 0xFF)

/* ====================================================================
 * SIMD-optimized UTF-8 character counting
 * ====================================================================
 * Algorithm: Count UTF-8 continuation bytes (bytes matching 10xxxxxx)
 * and subtract from total byte length to get character count.
 *
 * A continuation byte has the pattern: high bit = 1, second bit = 0
 * In other words: (byte & 0xC0) == 0x80
 *
 * For SIMD, we process 16-32 bytes at a time by:
 * 1. Treating bytes as signed: continuation bytes are in range [-128, -65]
 * 2. Using signed comparison: byte > -65 (0xBF as signed) means NOT
 * continuation
 * 3. Count bytes that ARE continuation (i.e., byte <= -65)
 */

#if defined(__SSE2__)
#include <emmintrin.h>

/* SSE2 optimized: process 16 bytes at a time */
size_t StrLenUtf8(const void *_ss, size_t len) {
    const uint8_t *s = (const uint8_t *)_ss;
    size_t continuationBytes = 0;
    const size_t originalLen = len;

    /* Process 16 bytes at a time with SSE2 */
    if (len >= 16) {
        /* Threshold for continuation bytes: -65 (0xBF as signed int8)
         * Continuation bytes are 10xxxxxx = 0x80-0xBF = -128 to -65 signed
         * We count bytes where (signed)byte <= -65 */
        const __m128i threshold = _mm_set1_epi8(-65);

        while (len >= 16) {
            __m128i chunk = _mm_loadu_si128((const __m128i *)s);

            /* Compare greater than threshold: result is 0xFF where NOT cont */
            __m128i notCont = _mm_cmpgt_epi8(chunk, threshold);

            /* Count zeros (continuation bytes) = 16 - count of 0xFF bytes */
            /* movemask gives us 1 bit per byte where comparison was true */
            uint32_t mask = (uint32_t)_mm_movemask_epi8(notCont);

            /* popcount of mask = non-continuation bytes, so:
             * continuation bytes = 16 - popcount(mask) */
            continuationBytes += 16 - __builtin_popcount(mask);

            s += 16;
            len -= 16;
        }
    }

    /* Handle remaining bytes with scalar code */
    while (len--) {
        /* Continuation byte check: (byte & 0xC0) == 0x80 */
        continuationBytes += ((*s >> 7) & ((~*s) >> 6));
        s++;
    }

    return originalLen - continuationBytes;
}

#elif defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

/* ARM NEON optimized: process 16 bytes at a time */
size_t StrLenUtf8(const void *_ss, size_t len) {
    const uint8_t *s = (const uint8_t *)_ss;
    size_t continuationBytes = 0;
    const size_t originalLen = len;

    /* Process 16 bytes at a time with NEON */
    if (len >= 16) {
        /* For NEON, we use unsigned comparison.
         * Continuation bytes: 0x80 <= byte <= 0xBF
         * We check: byte >= 0x80 AND byte <= 0xBF */
        const uint8x16_t low = vdupq_n_u8(0x80);
        const uint8x16_t high = vdupq_n_u8(0xBF);

        while (len >= 16) {
            uint8x16_t chunk = vld1q_u8(s);

            /* Check if byte >= 0x80 (high bit set) */
            uint8x16_t gelow = vcgeq_u8(chunk, low);

            /* Check if byte <= 0xBF */
            uint8x16_t lehigh = vcleq_u8(chunk, high);

            /* Continuation byte if both conditions true */
            uint8x16_t isCont = vandq_u8(gelow, lehigh);

            /* Count continuation bytes using horizontal add.
             * Each lane is 0xFF if continuation, 0x00 otherwise.
             * We need to count the 0xFF values. */
            /* Sum all bytes (each 0xFF = 255, but we want count of 1s) */
            /* First, convert 0xFF to 0x01 by right-shifting by 7 */
            uint8x16_t ones = vshrq_n_u8(isCont, 7);

            /* Horizontal sum: add all 16 bytes */
            continuationBytes += vaddvq_u8(ones);

            s += 16;
            len -= 16;
        }
    }

    /* Handle remaining bytes with scalar code */
    while (len--) {
        continuationBytes += ((*s >> 7) & ((~*s) >> 6));
        s++;
    }

    return originalLen - continuationBytes;
}

#else
/* Fallback: use SWAR (SIMD Within A Register) on 8 bytes at a time */
size_t StrLenUtf8(const void *_ss, size_t len) {
    const uint8_t *_s = (const uint8_t *)_ss;
    const uint8_t *s = (const uint8_t *)_s;

    /* 'count' is *NOT* the total character count, but is
     * the number of bytes used after the first byte of a multibyte
     * unicode character.
     *
     * This function calculates total utf-8 characters by returning:
     *   (total byte length)
     *      MINUS
     *   (byte count used by multibyte characters)
     *
     * So, the return value is: len - count */
    size_t countMultibyteExtra = 0;

    /* Handle any initial misaligned bytes before we begin
     * stepping by 4 or 8 bytes at once. */
    while (len && !DK_IS_STEP_ALIGNED(STRLEN_UTF8_STEP_SIZE, s)) {
        const uint8_t b = *s;

#if STRLEN_UTF8_CHECK_NULL
        /* Exit if we hit a zero byte. */
        if (b == '\0') {
            goto done;
        }
#endif

        /* Is this byte an extra member byte of a multibyte sequence?
         * (i.e. this is NOT a start character byte) */
        countMultibyteExtra += (b >> 7) & ((~b) >> 6);
        len--;
        s++;
    }

    /* Handle complete blocks of 4 or 8 bytes at once. */
    while (len >= STRLEN_UTF8_STEP_SIZE) {
        /* Process 4 or 8 bytes in one go. */
        /* TODO: attempt with SSE2 streaming 16 byte optimization. */
        STRLEN_UTF8_STEP u = *(STRLEN_UTF8_STEP *)(s);

#if STRLEN_UTF8_CHECK_NULL
        /* Exit the loop if there are any zero bytes. */
        if ((u - ONEMASK) & (~u) & (ONEMASK * 0x80)) {
            break;
        }
#endif

        /* Count bytes which are NOT the first byte of a character. */
        /* This works because 'u' is 8 bytes:
         *   [A, B, C, D, E, F, G, H]
         * and ONEMASK is 8 bytes:
         *   [0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01]
         * 0x01 is 00000001
         *
         * 0x80 is 10000000
         * (ONEMASK * 0x80) transforms 0000001 into 1000000, becoming:
         * 64-bit:
         *   1000000010000000100000001000000010000000100000001000000010000000
         * 32-bit:
         *   10000000100000001000000010000000
         *
         * (u & (ONEMASK * 0x80)) gives us a word where the highest position
         * in all bytes starting with 1 are now the only non-zero elements in
         * the word.
         * */

        /* FULL EXAMPLE.
         *   Input string: abcd💛
         * 8 bytes total.
         * 4 bytes for a,b,c,d and four bytes for yellow heart emoji.
         *
         * In hex, that's 8 bytes:
         *  0x61 0x62 0x63 0x64 0xF0 0x9F 0x92 0x9B
         *
         * In binary, that's:
         *  0110000101100010011000110110010011110000100111111001001010011011
         *
         * In binary split by bytes, that's:
         *  01100001 01100010 01100011 01100100
         *  11110000 10011111 10010010 10011011
         *
         * Notice how the 4-byte character has all its bytes starting
         * with '1'.  UTF-8 is a variable width encoding scheme using
         * the following layout:
         * ===========
         * Table from RFC 3629
         *
         *    Char. number range  |        UTF-8 octet sequence
         *       (hexadecimal)    |              (binary)
         *    --------------------+---------------------------------------------
         *    0000 0000-0000 007F | 0xxxxxxx
         *    0000 0080-0000 07FF | 110xxxxx 10xxxxxx
         *    0000 0800-0000 FFFF | 1110xxxx 10xxxxxx 10xxxxxx
         *    0001 0000-0010 FFFF | 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
         * ===========
         *
         * UTF-8 characters are between 1 and 4 bytes long and the exact
         * number of characters in a string can only be discovered by
         * iterating over a byte string looking for multibyte sequences.
         *
         * Let 'u' be the binary representation of our example string.
         *
         * ((u & (ONEMASK * 0x80)):
         *  0000000000000000000000000000000010000000100000001000000010000000
         *
         * Notice how only the 1 positions of the multibyte character remain.
         *
         * Shift that right by 7:
         *  0000000000000000000000000000000000000001000000010000000100000001
         *
         * ((~u) >> 6):
         *  0000001001111010011101100111001001101100001111011000000110110101
         *
         * AND those together:
         *  0000000000000000000000000000000000000000000000010000000100000001
         *
         * So, our 8 bytes containing one 4-byte multibyte character is now
         * degenerated down into a marker of (4 - 1) 1s signifying our 4 byte
         * character has 3 extra bytes of encoding beyond a "regular"
         * 1-byte-per-character encoding.
         *
         * (u * ONEMASK): (using 'u' mutated by previous assignment)
         *  0000001100000011000000110000001100000011000000110000001000000001
         *
         * ((sizeof(size_t) - 1) * 8): 56
         *
         * Shift (u * ONEMASK) right by 56:
         *  0000000000000000000000000000000000000000000000000000000000000011
         *
         * And our result is 3.  The number of bytes used by multibyte
         * characters in this 8 byte quanity is 4, but we are only counting the
         * "overflow" character bytes.  Our 4-byte character has 3
         * bytes used by the full UTF-8 encoding.  And that's correct.
         *
         * We have 4 ASCII characters and one 4-byte emoji, for
         * a total of 5 characters in our 8 byte string, and (8 - 3) is
         * the count of *characters* in our 8 byte string.
         *
         * Now 'countMultibyteExtra' is incremented by 3 to track the
         * total number of "additional" bytes used by characters across
         * all characters.
         *
         * We return the total number of characters by subtracting the
         * total number of bytes by the number of "extra" bytes used
         * by UTF-8 characters (but never counting the initial UTF-8
         * character byte so it remains as a marker for the character
         * in the count). */
        u = ((u & (ONEMASK * 0x80)) >> 7) & ((~u) >> 6);
        countMultibyteExtra +=
            (u * ONEMASK) >> ((STRLEN_UTF8_STEP_SIZE - 1) * 8);
        len -= STRLEN_UTF8_STEP_SIZE;
        s += STRLEN_UTF8_STEP_SIZE;
    }

    /* Take care of remaining bytes. */
    while (len) {
        const uint8_t b = *s;

#if STRLEN_UTF8_CHECK_NULL
        /* Exit if we hit a zero byte. */
        if (b == '\0') {
            break;
        }
#endif

        /* Is this byte NOT the first byte of a character? */
        countMultibyteExtra += (b >> 7) & ((~b) >> 6);
        len--;
        s++;
    }

#if STRLEN_UTF8_CHECK_NULL
done:
#endif

    /* We iterated 'len' to zero already, so re-calculate
     * the total byte length using (end - start) */
    return (s - _s) - countMultibyteExtra;
}
#endif /* SIMD/NEON/fallback selection */

/* ====================================================================
 * UTF-8 bytes used by a requested number of characters
 * ==================================================================== */
/* Note: 'Exactly' only applies when iterating character-by-character.
 *       It gives *invalid* results if you are evaluating a fixed byte
 *       width on each iteration. */
#define countedExactlyCharacters(current, start, currentCount, desiredCount)   \
    ((((current) - (start)) - (currentCount)) == (desiredCount))

/* 'GTE' is for iterating by 'STRLEN_UTF8_STEP_SIZE'. We can't detect *exact*
 * character matches because we're traversing by multibyte steps and we could
 * end up on a mid-character boundary. */
#define countedGTECharacters(current, start, currentCount, desiredCount)       \
    ((((current) - (start)) - (currentCount)) >= (desiredCount))

/* Returns number of bytes in '*_ss' holding the first 'countCharacters'
 * characters. */
/* This is kinda the inverse of counting characters.  We're iterating over
 * characters and returning how many bytes they occupy. */
size_t StrLenUtf8CountBytes(const void *_ss, size_t len,
                            const size_t countCharacters) {
    const uint8_t *_s = _ss;
    const uint8_t *s = _s;
    size_t countMultibyteExtra = 0;

    /* Handle complete sets of (STRLEN_UTF8_STEP_SIZE) bytes at once. */
    while (len >= STRLEN_UTF8_STEP_SIZE) {
        STRLEN_UTF8_STEP u = 0;
        memcpy(&u, s, STRLEN_UTF8_STEP_SIZE);

        /* See comment in StrLenUtf8 to understand this */
        u = ((u & (ONEMASK * 0x80)) >> 7) & ((~u) >> 6);
        countMultibyteExtra +=
            (u * ONEMASK) >> ((STRLEN_UTF8_STEP_SIZE - 1) * 8);

        len -= STRLEN_UTF8_STEP_SIZE;
        s += STRLEN_UTF8_STEP_SIZE;

        if (countedGTECharacters(s, _s, countMultibyteExtra, countCharacters)) {
            /* If we hit this branch, we *always* must back up at least one
             * character to reach a proper byte count.
             *
             * We can only detect we've reached a whole character (in this
             * 'STRLEN_UTF8_STEP_SIZE' iterating) by going beyond our target
             * count then backing up to the proper size.
             * Otherwise, if we tried to match exactly, we may end up with
             * partial characters and uncounted bytes. */

            /* We know we've reached our return condition, but we need to
             * account for how much to back up our final byte count. */

            /* (Total Counted Characters) - (Target Character Count) */
            size_t backupBy =
                ((s - _s) - countMultibyteExtra) - countCharacters;

            while (backupBy) {
                /* We overshot 's' so we need to start
                 * iterating backwards... */
                s--;
                len++;

                /* Iterate over the bytes backwards and discard
                 * exactly 'backupBy' characters since that's
                 * the number of characters we overshot by. */

                /* (*s & 0xC0) selects the top two bits of *s */
                switch (*s & 0xC0) {
                case 0x80:
                    /* if top two bits are 10, skip because
                     * it's a member byte, not a start byte. */
                    break;
                case 0x00:
                case 0x40:
                /* if the top two bits are 00 or 01,
                 * count it for removal because it's ASCII */
                case 0xC0:
                    /* all bytes starting with 11 are multibyte
                     * start indicators, so we've iterated over
                     * a full character to count for removal. */
                    backupBy--;
                    break;
                default:
                    break;
                }
            }

            /* If the current end position is a member byte,
             * iterate up until the next character start byte so
             * we consume a full character (otherwise we could be returning
             * byte counts with the last UTF-8 character not fully counted). */
            while (len-- && ((*s & 0xC0) == 0x80)) {
                s++;
            }

            return s - _s;
        }
    }

    /* This loop gets triggered when:
     *   - len < STRLEN_UTF8_STEP_SIZE */
    /* Use 'extraLen' instead of 'len' because below we end up decremeting
     * 'len' by multiple character steps, so if we have a malformed utf8
     * input, we don't want to skip over our 'len == 0' terminating
     * condition by subtracting more bytes than exist in our byte length. */
    int64_t extraLen = len;

    /* Take care of remaining bytes. */
    /* This is a non-fast-path loop iterating between one and eight steps,
     * so excessive optimization isn't a priority below. */
    while (extraLen > 0) {
        /* Assume an ASCII character by default.
         * There's no 'default' in the switch statement,
         * so if nothing matches, this is our accounting data. */
        uint8_t byteStep = 1;    /* byteStep == 1 == 1 byte */
        uint8_t memberBytes = 0; /* memberBytes == 0 == no UTF-8 bytes */

        /* For counting byte offsets we step over complete characters. */
        /* The 'default' case where nothing matches assumes a
         * one byte UTF-8/ascii character. */
        switch (*s & 0xF0) {
        /* These first four special cases check the upper four
         * bits of a member byte when the byte starts with '10'.
         * Member bytes starting with '10' are left over from our
         * step-by-8 processing, but we have to account for them
         * individually here since we step by entire characters below.
         * These four catch every combination of '10xx0000' */
        case 0x80:
        /* 10000000 */
        case 0x90:
        /* 10010000 */
        case 0xA0:
        /* 10100000 */
        case 0xB0:
            /* 10110000 */
            /* RESIDUAL MEMBER BYTE ACCOUNTING */
            /* 1 character member byte, not character boundary. */
            /* These catch special cases where we have trailing
             * member bytes left over from 'STRLEN_UTF8_STEP_SIZE' iterations.
             * We need to include the "extra character" but
             * not count any full characters. */

            /* use default 'byteStep' assigned above. */
            memberBytes = 1;
            break;
        case 0xC0:
            /* 11000000 */
            /* 2 character utf8 */
            byteStep = 2;
            memberBytes = 1;
            break;
        case 0xE0:
            /* 11100000 */
            /* 3 character utf8 */
            byteStep = 3;
            memberBytes = 2;
            break;
        case 0xF0:
            /* 11110000 */
            /* 4 character utf8 */
            byteStep = 4;
            memberBytes = 3;
            break;
        }
        countMultibyteExtra += memberBytes;
        extraLen -= byteStep;
        s += byteStep;

        if (countedExactlyCharacters(s, _s, countMultibyteExtra,
                                     countCharacters)) {
            return s - _s;
        }
    }

    /* Note: this returns the *byte* difference we walked,
     * and *NOT* the character count like StrLenUtf8 */

    /* This return is only reached if the requested character count
     * is larger than the number of characters in the byte string.
     * We just return the byte size of all characters found.
     * Note: we aren't signaling to the user we are early terminating
     * their requested character count search because we ran out of bytes. */
    return s - _s;
}

/* ====================================================================
 * Scalar baseline for benchmarking comparison
 * ==================================================================== */
/* Pure byte-by-byte scalar implementation for performance comparison */
size_t StrLenUtf8Scalar(const void *_ss, size_t len) {
    const uint8_t *s = (const uint8_t *)_ss;
    size_t continuationBytes = 0;

    for (size_t i = 0; i < len; i++) {
        /* Count continuation bytes: (byte & 0xC0) == 0x80 */
        continuationBytes += ((s[i] >> 7) & ((~s[i]) >> 6));
    }

    return len - continuationBytes;
}
