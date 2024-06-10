#include "../str.h"

/* ====================================================================
 * Log Estimation (sqlite3)
 * ==================================================================== */
/* Find (an approximate) sum of two LogEst values.  This computation is
 * not a simple "+" operator because LogEst is stored as a logarithmic
 * value. */
LogEst StrLogEstAdd(LogEst a, LogEst b) {
    static const uint8_t x[] = {
        10, 10,                /* 0,1 */
        9,  9,                 /* 2,3 */
        8,  8,                 /* 4,5 */
        7,  7,  7,             /* 6,7,8 */
        6,  6,  6,             /* 9,10,11 */
        5,  5,  5,             /* 12-14 */
        4,  4,  4, 4,          /* 15-18 */
        3,  3,  3, 3, 3, 3,    /* 19-24 */
        2,  2,  2, 2, 2, 2, 2, /* 25-31 */
    };
    if (a >= b) {
        if (a > b + 49) {
            return a;
        }

        if (a > b + 31) {
            return a + 1;
        }

        return a + x[a - b];
    } else {
        if (b > a + 49) {
            return b;
        }

        if (b > a + 31) {
            return b + 1;
        }

        return b + x[b - a];
    }
}

/*
** Convert an integer into a LogEst.  In other words, compute an
** approximation for 10*log2(x).
*/
LogEst StrLogEst(uint64_t x) {
    static const LogEst a[] = {0, 2, 3, 5, 6, 7, 8, 9};
    LogEst y = 40;
    if (x < 8) {
        if (x < 2) {
            return 0;
        }

        while (x < 8) {
            y -= 10;
            x <<= 1;
        }
    } else {
        while (x > 255) {
            y += 40;
            x >>= 4;
        }

        while (x > 15) {
            y += 10;
            x >>= 1;
        }
    }
    return a[x & 7] + y - 10;
}

/*
** Convert a double into a LogEst
** In other words, compute an approximation for 10*log2(x).
*/
LogEst StrLogEstFromDouble(double x) {
    uint64_t a;
    assert(sizeof(x) == 8 && sizeof(a) == 8);
    if (x <= 1) {
        return 0;
    }

    if (x <= 2000000000) {
        return StrLogEst((uint64_t)x);
    }

    memcpy(&a, &x, 8);
    LogEst e = (a >> 52) - 1022;
    return StrValTimes10(e);
}

/*
** Convert a LogEst into an integer.
*/
uint64_t StrLogEstToInt(LogEst x) {
    if (x < 10) {
        return 1;
    }

    uint64_t n = x % 10;
    x /= 10;
    if (n >= 5) {
        n -= 2;
    } else if (n >= 1) {
        n -= 1;
    }

    if (x >= 3) {
        return x > 60 ? (uint64_t)INT64_MAX : (n + 8) << (x - 3);
    }

    return (n + 8) >> (3 - x);
}
