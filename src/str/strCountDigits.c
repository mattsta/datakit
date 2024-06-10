#include "../str.h"

size_t StrDigitCountUInt128(__uint128_t v) {
    if (v < 10) {
        return 1;
    }
    if (v < 100) {
        return 2;
    }
    if (v < 1000) {
        return 3;
    }
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000) {
                    return 4;
                }
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + StrDigitCountUInt128(v / 1000000000000UL);
}

size_t StrDigitCountUInt64(uint64_t v) {
    if (v < 10) {
        return 1;
    }
    if (v < 100) {
        return 2;
    }
    if (v < 1000) {
        return 3;
    }
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000) {
                    return 4;
                }
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + StrDigitCountUInt64(v / 1000000000000UL);
}

size_t StrDigitCountUInt32(uint32_t v) {
    if (v < 10) {
        return 1;
    }
    if (v < 100) {
        return 2;
    }
    if (v < 1000) {
        return 3;
    }
    if (v < 100000000UL) {
        if (v < 1000000) {
            if (v < 10000) {
                return 4;
            }
            return 5 + (v >= 100000);
        }
        return 7 + (v >= 10000000UL);
    }
    return 9 + (v >= 1000000000UL);
}

size_t StrDigitCountInt64(int64_t v) {
    if (v < 0) {
        /* Abs value of INT64_MIN requires special handling. */
        const uint64_t uv = DK_INT64_TO_UINT64(v);
        return StrDigitCountUInt64(uv) + 1; /* +1 for the minus. */
    }

    return StrDigitCountUInt64(v);
}
