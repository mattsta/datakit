#include "varintChained.h"
#include "varintChainedSimple.h"

#include "varintExternal.h"
#include "varintExternalBigEndian.h"
#include "varintSplit.h"
#include "varintSplitFull.h"
#include "varintSplitFull16.h"
#include "varintSplitFullNoZero.h"
#include "varintTagged.h"

/*
** Compile this one file with the -DTEST_VARINT option to run the simple
** test case below.  The test program generates 10 million random 64-bit
** values, weighted toward smaller numbers, and for each value it encodes
** and then decodes the varint to verify that the same number comes back.
*/

static uint32_t randInt(void) {
    /* note: deterministic on every run. */
    static uint32_t rx = 1;
    static uint32_t ry = 0;
    rx = (rx >> 1) ^ (-(rx & 1) & 0xd0000001);
    ry = ry * 1103515245 + 12345;
    return rx ^ ry;
}

#define GIVE_XZ                                                                \
    uint64_t x = randInt();                                                    \
    x = (x << 32) + randInt();                                                 \
    int32_t nbit = randInt() % 65;                                             \
    if (nbit < 64) {                                                           \
        x &= (((uint64_t)1) << nbit) - 1;                                      \
    }                                                                          \
    uint8_t z[20] = {0}

#define GIVE_SMALL_XZ                                                          \
    GIVE_XZ;                                                                   \
    x = x % smallBias;

#define SETUP(name, small)                                                     \
    min = max = 0;                                                             \
    int _plen = printf("Testing " name "...\n");                               \
    char *_smallname = small;                                                  \
    char _eqs[_plen];                                                          \
    memset(_eqs, '=', _plen);                                                  \
    printf("%.*s\n", _plen, _eqs);                                             \
    PERF_TIMERS_SETUP;

#define ACCOUNT_LOOP                                                           \
    do {                                                                       \
        if (x > max) {                                                         \
            max = x;                                                           \
        } else if ((int64_t)x < min) {                                         \
            min = x;                                                           \
        }                                                                      \
    } while (0)

#define ACCOUNT_FINAL                                                          \
    PERF_TIMERS_FINISH_PRINT_RESULTS(i, _smallname);                           \
    printf("Largest tested number: %" PRIu64 "\n", max);                       \
    printf("Smallest tested number: %" PRIi64 "\n", min);                      \
    memset(_eqs, '-', _plen);                                                  \
    printf("%.*s\n\n", _plen, _eqs);

#include "perf.h"
#include <inttypes.h>
#include <stdio.h>
int32_t main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint64_t i;
    static const uint64_t smallBias = 131072; /* 17 bits for small testing */
    uint64_t max = 0;
    int64_t min = 0;
    /* 27: 1.5s to 3s per test
     * 28: 3.0s to 6s per test */
    static const uint64_t maxLoop = (1ULL << 27);

    printf("Each test will run against %" PRIu64 " random numbers.\n\n",
           maxLoop);
    {
        SETUP("baseline overhead with no encode/decode", "baseline")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            assert(x == x);
            assert(z[0] == z[0]);
            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

#if 1
    {
        SETUP("tagged varint (from sqlite4)", "tagged")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            int n1 = varintTaggedPut64(z, x);
            uint64_t y = 0;
            varintTaggedGet(z, n1, &y);
            assert(x == y);
/* Tagged varint is big endian and sorts in-order with memcmp(),
 * but we don't test this due to additional overhead not relevant
 * to other varint types. */
#if 0
        if (i > 0) {
            int32_t c = memcmp(z, zp, pn < n1 ? pn : n1);
            if (x < px) {
                assert(c < 0);
            } else if (x > px) {
                assert(c > 0);
            } else {
                assert(c == 0);
            }
        }
        memcpy(zp, z, n1);
#endif
            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("quick tagged varint using smaller numbers", "tagged quick small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            varintWidth width = varintTaggedLenQuick(x);
            varintTaggedPut64FixedWidthQuick_(z, x, width);
            uint64_t y = varintTaggedGet64Quick_(z);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("chained varint (from sqlite3)", "chained")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            int32_t n1 = varintChainedPutVarint(z, x);
            assert(n1 >= 1 && n1 <= 9);
            uint64_t y = 0;
            int32_t n2 = varintChainedGetVarint(z, &y);
            assert(n1 == n2);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("chained varint using smaller numbers", "chained small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            int32_t n1 = varintChained_putVarint32(z, x);
            assert(n1 >= 1 && n1 <= 9);
            uint64_t y = 0;
            varintChained_getVarint32(z, y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("chained simple varint (from leveldb)", "chained simple")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            int32_t n1 = varintChainedSimpleEncode64(z, x);
            assert(n1 >= 1 && n1 <= 9);
            uint64_t y = 0;
            int32_t n2 = varintChainedSimpleDecode64(z, &y);
            assert(n1 == n2);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("chained simple varint using smaller numbers",
              "chained simple small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            int32_t n1 = varintChainedSimpleEncode32(z, x);
            assert(n1 >= 1 && n1 <= 5);
            uint32_t y = 0;
            int32_t n2 = varintChainedSimpleDecode32(z, &y);
            assert(n1 == n2);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("external varint", "external")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            varintWidth encoding = varintExternalPut(z, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y = varintExternalGet(z, encoding);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("quick external varint using smaller numbers",
              "quick external small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            x = x % smallBias;
            varintWidth encoding;
            varintExternalUnsignedEncoding(x, encoding);

            varintExternalPutFixedWidthQuick_(z, x, encoding);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintExternalGetQuick_(z, encoding, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("external big endian varint", "external big endian")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            varintWidth encoding = varintExternalBigEndianPut(z, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y = varintExternalBigEndianGet(z, encoding);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("quick external big endian varint using smaller numbers",
              "quick external big endian small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            x = x % smallBias;
            varintWidth encoding;
            varintExternalBigEndianUnsignedEncoding(x, encoding);

            varintExternalBigEndianPutFixedWidthQuick_(z, x, encoding);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintExternalBigEndianGetQuick_(z, encoding, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full no zero varint", "split full no zero")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            varintSplitFullNoZeroPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    /* verify NoZero stores 64 as one byte */
    {
        SETUP("split full no zero check byte limits", "split nz byte limits")
        for (i = 1; i < 421050; i++) {
            uint8_t z[20];
            uint8_t len;

            varintSplitFullNoZeroPut_(z, len, i);

            if (i <= 64) {
                assert(len == 1);
            } else if (i <= 16447) {
                assert(len == 2);
            } else if (i <= 4210750) {
                assert(len == 3);
            }

            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroGet_(z, width, y);
            assert(i == y);
        }
        ACCOUNT_FINAL
    }

    {
        SETUP("split full no zero varint using smaller numbers",
              "split full no zero small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            varintSplitFullNoZeroPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full varint", "split full")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            varintSplitFullPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full varint using smaller numbers", "split full small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            varintSplitFullPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full 16 varint", "split full 16")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            varintSplitFull16Put_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFull16Get_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full 16 varint using smaller numbers",
              "split full 16 small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            varintSplitFull16Put_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFull16Get_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split varint", "split")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            varintSplitPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split varint using smaller numbers", "split small")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            varintSplitPut_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitGet_(z, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split reversed varint (forward)", "split reversed (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            varintSplitReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split reversed varint (reversed)", "split reversed (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split reversed varint (forward) using smaller numbers",
              "split reversed small (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            varintSplitReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split reversed varint (reversed) using smaller numbers",
              "split reversed small (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full reversed varint (forward)",
              "split full reversed (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            varintSplitFullReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full reversed varint (reversed)",
              "split full reversed (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitFullReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full reversed varint (forward) using smaller numbers",
              "split full reversed small (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            varintSplitFullReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full reversed varint (reversed) using smaller numbers",
              "split full reversed small (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitFullReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full no zero reversed varint (forward)",
              "split full no zero reversed (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            varintSplitFullNoZeroReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }
#endif

    {
        SETUP("split full no zero reversed varint (reversed)",
              "split full no zero reversed (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_XZ;

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitFullNoZeroReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, len);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full no zero reversed varint (forward) using smaller "
              "numbers",
              "split full no zero reversed small (forward)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            varintSplitFullNoZeroReversedPutForward_(z, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroReversedGet_(z + len - 1, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    {
        SETUP("split full no zero reversed varint (reversed) using smaller "
              "numbers",
              "split full no zero reversed small (reversed)")
        for (i = 0; i < maxLoop; i++) {
            GIVE_SMALL_XZ

            if (x == 0) {
                x = 1;
            }

            uint8_t len;
            uint8_t *zend = &z[sizeof(z) - 1];
            varintSplitFullNoZeroReversedPutReversed_(zend, len, x);
            // printf("encoded %llu with encoding %d\n", x, encoding);
            uint64_t y;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroReversedGet_(zend, width, y);
            // printf("decoded as: %llu\n", y);
            assert(x == y);

            ACCOUNT_LOOP;
        }

        ACCOUNT_FINAL
    }

    /* verify NoZero Reversed stores 64 as one byte */
    {
        SETUP("split full no zero reversed (forward) check byte limits",
              "split nz byte limits reversed (forward)")
        for (i = 1; i < 421050; i++) {
            uint8_t z[20];
            uint8_t len;

            varintSplitFullNoZeroReversedPutForward_(z, len, i);

            if (i <= 64) {
                assert(len == 1);
            } else if (i <= 16447) {
                assert(len == 2);
            } else if (i <= 4210750) {
                assert(len == 3);
            }

            uint64_t y = 0;
            varintWidth width;
            (void)width;
            varintSplitFullNoZeroReversedGet_(z + len - 1, width, y);
            assert(i == y);
        }

        ACCOUNT_FINAL
    }

    return 0;
}

/*
 ** Compile this one file with -DVARINT_TOOL to generate a command-line
 ** program that converts the integers it finds as arguments into varints
 ** and hex varints preceded by "=" into integers and then displays the
 ** results on standard output.
 */
#ifdef VARINT_TOOL
static int32_t hexToInt(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

int32_t main(int32_t argc, char **argv) {
    int32_t i, j, n;
    uint64_t x;
    char out[20];
    if (argc <= 1) {
        printf("Usage: %s N =X ...\n"
               "Convert integer values into varints.\n"
               "Convert hex varint values preceded by '=' into integers.\n",
               argv[0]);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *z = argv[i];
        x = 0;
        if (z[0] == '=') {
            for (j = 1; j / 2 < sizeof(out) && z[j] && z[j + 1]; j += 2) {
                out[j / 2] = hexToInt(z[j]) * 16 + hexToInt(z[j + 1]);
            }

            varintTaggedGet64(out, j / 2, &x);
        } else {
            while (z[0] >= '0' && z[0] <= '9') {
                x = x * 10 + z[0] - '0';
                z++;
            }
        }
        n = varintTaggedPut64(out, x);
        printf("%llu = ", (long long unsigned)x);
        for (j = 0; j < n; j++)
            printf("%02x", out[j] & 0xff);
        printf("\n");
    }

    return 0;
}
#endif
