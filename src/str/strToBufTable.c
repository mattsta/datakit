#include "../str.h"

/* ====================================================================
 * Integer to String conversions
 * ==================================================================== */
/* Convert uint64_t to string.
 * Returns length of written converted string in buffer.
 * Returns 0 if buffer not big enough for string.
 *
 * https://www.facebook.com/notes/facebook-engineering/10151361643253920 */
size_t StrUInt64ToBufTable(void *dst_, size_t dstLen, uint64_t value) {
    static const char digits[201] = "0001020304050607080910111213141516171819"
                                    "2021222324252627282930313233343536373839"
                                    "4041424344454647484950515253545556575859"
                                    "6061626364656667686970717273747576777879"
                                    "8081828384858687888990919293949596979899";
    uint8_t *dst = dst_;
    const uint32_t length = StrDigitCountUInt64(value);

    if (length >= dstLen) {
        return 0;
    }

    /* Terminate eventual string in buffer */
    uint32_t next = length - 1;
    dst[next + 1] = '\0';

    while (value >= 100) {
        const int32_t i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    /* Handle last 1-2 digits */
    if (value < 10) {
        dst[next] = '0' + (uint32_t)value;
    } else {
        const uint32_t i = (uint32_t)value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }

    return length;
}

/* Convert int64_t to string.
 * Returns length of written converted string in buffer.
 * Returns 0 if buffer not big enough for string.
 *
 * https://www.facebook.com/notes/facebook-engineering/10151361643253920
 *
 * Modified in order to handle signed integers since the original code was
 * designed for unsigned integers. */
size_t StrInt64ToBufTable(void *dst_, size_t dstLen, int64_t svalue) {
    static const char digits[201] = "0001020304050607080910111213141516171819"
                                    "2021222324252627282930313233343536373839"
                                    "4041424344454647484950515253545556575859"
                                    "6061626364656667686970717273747576777879"
                                    "8081828384858687888990919293949596979899";
    uint8_t *dst = dst_;
    int negative;
    uint64_t value;

    /* The main loop works with 64bit unsigned integers for simplicity, so
     * we convert the number here and remember if it is negative. */
    if (svalue < 0) {
        value = DK_INT64_TO_UINT64(svalue);
        negative = true;
    } else {
        value = svalue;
        negative = false;
    }

    /* Check length */
    const uint32_t length = StrDigitCountUInt64(value) + negative;
    if (length >= dstLen) {
        return 0;
    }

    /* Terminate eventual string in buffer */
    uint32_t next = length - 1;
    dst[next + 1] = '\0';

    while (value >= 100) {
        const uint32_t i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    /* Handle last 1-2 digits. */
    if (value < 10) {
        dst[next] = '0' + (uint32_t)value;
    } else {
        const uint32_t i = (uint32_t)value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }

    /* Add sign. */
    if (negative) {
        dst[0] = '-';
    }

    return length;
}
