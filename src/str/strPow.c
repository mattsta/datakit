#include "../datakit.h"

/* ====================================================================
 * Powers
 * ==================================================================== */
#define tenPow64Count COUNT_ARRAY(tenPow64)
static const uint64_t tenPow64[] = {1,
                                    10,
                                    100,
                                    1000,
                                    10000,
                                    100000,
                                    1000000,
                                    10000000,
                                    100000000,
                                    1000000000,
                                    10000000000,
                                    100000000000,
                                    1000000000000,
                                    10000000000000,
                                    100000000000000,
                                    1000000000000000,
                                    10000000000000000,
                                    100000000000000000,
                                    1000000000000000000};

uint64_t StrTenPow(const size_t exp) {
    assert(exp < tenPow64Count);
    return tenPow64[exp];
}

__uint128_t StrTenPowBig(const size_t exp) {
    if (exp < tenPow64Count) {
        return StrTenPow(exp);
    }

    /* Compiler doesn't allow 128 bit integer constants, so we have to
     * re-build the 128 bit integers from two 64 bit parts */
    /* All this continues where 'tenPow64' leaves off, with the
     * final value being 10^38 (using 127 bits) */
    static const __uint128_t table[] = {
        (((__uint128_t)5ULL) << 64) | 7766279631452241920ULL,
        (((__uint128_t)54ULL) << 64) | 3875820019684212736ULL,
        (((__uint128_t)542ULL) << 64) | 1864712049423024128ULL,
        (((__uint128_t)5421ULL) << 64) | 200376420520689664ULL,
        (((__uint128_t)54210ULL) << 64) | 2003764205206896640ULL,
        (((__uint128_t)542101ULL) << 64) | 1590897978359414784ULL,
        (((__uint128_t)5421010ULL) << 64) | 15908979783594147840ULL,
        (((__uint128_t)54210108ULL) << 64) | 11515845246265065472ULL,
        (((__uint128_t)542101086ULL) << 64) | 4477988020393345024ULL,
        (((__uint128_t)5421010862ULL) << 64) | 7886392056514347008ULL,
        (((__uint128_t)54210108624ULL) << 64) | 5076944270305263616ULL,
        (((__uint128_t)542101086242ULL) << 64) | 13875954555633532928ULL,
        (((__uint128_t)5421010862427ULL) << 64) | 9632337040368467968ULL,
        (((__uint128_t)54210108624275ULL) << 64) | 4089650035136921600ULL,
        (((__uint128_t)542101086242752ULL) << 64) | 4003012203950112768ULL,
        (((__uint128_t)5421010862427522ULL) << 64) | 3136633892082024448ULL,
        (((__uint128_t)54210108624275221ULL) << 64) | 12919594847110692864ULL,
        (((__uint128_t)542101086242752217ULL) << 64) | 68739955140067328ULL,
        (((__uint128_t)5421010862427522170ULL) << 64) | 687399551400673280ULL};

    assert(exp <= (tenPow64Count + COUNT_ARRAY(table)));
    return table[exp - tenPow64Count];
}
