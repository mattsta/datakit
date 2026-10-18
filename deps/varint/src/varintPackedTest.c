#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>

#define PACK_STORAGE_BITS 12
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint32_t
#define PACK_STORAGE_VALUE_TYPE uint16_t
#define PACK_STORAGE_MICRO_PROMOTION_TYPE uint32_t
#include "varintPacked.h"

#define PACK_STORAGE_BITS 12
#if 1
/* Test impact of active SLOT_CAN_HOLD_ENTIRE_VALUE optimization */
#define PACK_STORAGE_COMPACT
#else
/* Test impact of diabled SLOT_CAN_HOLD_ENTIRE_VALUE optimization. */
#define PACK_FUNCTION_PREFIX varintPackedCompact
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint8_t
#endif
#define PACK_STORAGE_VALUE_TYPE uint16_t
#define PACK_STORAGE_MICRO_PROMOTION_TYPE uint64_t
#include "varintPacked.h"

#define PACK_STORAGE_BITS 13
#define PACK_STORAGE_VALUE_TYPE uint32_t
#include "varintPacked.h"

#define PACK_STORAGE_BITS 14
#define PACK_STORAGE_VALUE_TYPE uint32_t
#include "varintPacked.h"

#if 0
#define PACK_STORAGE_BITS 3
#include "varintPacked.h"
#endif

/* 16-bit and 64-bit slots for the misaligned-storage check */
#define PACK_STORAGE_BITS 12
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint16_t
#define PACK_STORAGE_MICRO_PROMOTION_TYPE uint16_t
#define PACK_FUNCTION_PREFIX varintPackedSlot16_
#include "varintPacked.h"

#define PACK_STORAGE_BITS 13
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint64_t
#define PACK_FUNCTION_PREFIX varintPackedSlot64_
#include "varintPacked.h"

#include "perf.h"

typedef void (*packedSetFn)(void *, uint32_t, uint16_t);
typedef uint16_t (*packedGetFn)(const void *, uint32_t);
typedef void (*packedIncrFn)(void *, uint32_t, int64_t);
typedef void (*packedHalfFn)(void *, uint32_t);

/* Packed storage may start at any address. Run the same Set/SetIncr/SetHalf
 * sequence on an aligned buffer and on the same storage shifted by every
 * misalignment of the slot type: the bytes must be identical, every value
 * must read back, and nothing outside the storage may be written. */
static int checkMisaligned(const char *name, const uint32_t bits,
                           const size_t slotBytes, const packedSetFn set,
                           const packedGetFn get, const packedIncrFn incr,
                           const packedHalfFn half) {
#define CHECK_COUNT 300
    uint64_t alignedBacking[CHECK_COUNT / 4 + 4];
    uint64_t shiftedBacking[CHECK_COUNT / 4 + 5];
    const size_t bytes = sizeof(alignedBacking);
    uint8_t *aligned = (uint8_t *)alignedBacking;
    const uint16_t mask = (uint16_t)((1u << bits) - 1);
    int failures = 0;

    for (size_t shift = 1; shift < slotBytes; shift++) {
        uint8_t *shifted = (uint8_t *)shiftedBacking + shift;
        memset(shiftedBacking, 0xee, sizeof(shiftedBacking));
        memset(shifted, 0, bytes);
        memset(aligned, 0, bytes);

        for (uint32_t i = 0; i < CHECK_COUNT; i++) {
            const uint16_t v = (uint16_t)((i * 40503u + 7u) & mask);
            set(aligned, i, v);
            set(shifted, i, v);
        }

        for (uint32_t i = 0; i < CHECK_COUNT; i++) {
            const uint16_t v = (uint16_t)((i * 40503u + 7u) & mask);
            if (get(shifted, i) != v || get(aligned, i) != v) {
                printf("%s shift %zu: [%u] read %u/%u, expected %u\n", name,
                       shift, i, get(shifted, i), get(aligned, i), v);
                failures++;
                break;
            }
        }

        for (uint32_t i = 0; i < CHECK_COUNT; i += 3) {
            if (get(aligned, i) <= mask - 5) {
                incr(aligned, i, 5);
                incr(shifted, i, 5);
            }
        }

        for (uint32_t i = 1; i < CHECK_COUNT; i += 4) {
            half(aligned, i);
            half(shifted, i);
        }

        if (memcmp(aligned, shifted, bytes) != 0) {
            printf("%s shift %zu: misaligned bytes differ from aligned\n", name,
                   shift);
            failures++;
        }

        for (size_t b = 0; b < shift; b++) {
            if (((uint8_t *)shiftedBacking)[b] != 0xee) {
                printf("%s shift %zu: wrote before the storage\n", name, shift);
                failures++;
                break;
            }
        }

        for (size_t b = shift + bytes; b < sizeof(shiftedBacking); b++) {
            if (((uint8_t *)shiftedBacking)[b] != 0xee) {
                printf("%s shift %zu: wrote past the storage\n", name, shift);
                failures++;
                break;
            }
        }
    }

#undef CHECK_COUNT
    return failures;
}

/* SetIncr saturates at [0, 2^bits - 1] and no write reaches a neighbouring
 * value. For every bit alignment of a target value, with several neighbour
 * patterns on both sides, check max+1, max+huge, 0-1, 0+INT64_MIN, plain
 * increments and decrements, and (assert-free builds only) an oversized Set. */
static int checkSaturation(const char *name, const uint32_t bits,
                           const packedSetFn set, const packedGetFn get,
                           const packedIncrFn incr) {
    uint64_t storage[64] = {0};
    const uint16_t mask = (uint16_t)((1u << bits) - 1);
    const uint16_t neighbours[][2] = {
        {0, 0},
        {mask, mask},
        {(uint16_t)(0x5555 & mask), (uint16_t)(0xaaaa & mask)}};
    const struct {
        uint16_t start;
        int64_t incrBy;
        uint16_t expect;
    } cases[] = {
        {mask, 1, mask},         /* max + 1 stays at max */
        {mask, INT64_MAX, mask}, /* max + huge stays at max */
        {(uint16_t)(mask - 1), 1, mask},
        {(uint16_t)(mask - 1), 2, mask},
        {0, -1, 0},        /* 0 - 1 stays at 0 */
        {0, INT64_MIN, 0}, /* 0 + INT64_MIN stays at 0 */
        {mask, -1, (uint16_t)(mask - 1)},
        {5, -2, 3}, /* a decrement lowers the value */
        {3, -5, 0},
        {mask, INT64_MIN, 0},
        {0, mask, mask},
        {1, 2, 3},
    };
    int failures = 0;

    for (uint32_t t = 1; t < 130; t++) {
        for (size_t n = 0; n < sizeof(neighbours) / sizeof(*neighbours); n++) {
            for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
                memset(storage, 0, sizeof(storage));
                set(storage, t - 1, neighbours[n][0]);
                set(storage, t + 1, neighbours[n][1]);
                set(storage, t, cases[c].start);
                incr(storage, t, cases[c].incrBy);

                const uint16_t got = get(storage, t);
                const uint16_t before = get(storage, t - 1);
                const uint16_t after = get(storage, t + 1);
                if (got != cases[c].expect || before != neighbours[n][0] ||
                    after != neighbours[n][1]) {
                    printf("%s [%u] %u%+" PRId64 ": got %u (expected %u), "
                           "neighbours %u/%u (expected %u/%u)\n",
                           name, t, cases[c].start, cases[c].incrBy, got,
                           cases[c].expect, before, after, neighbours[n][0],
                           neighbours[n][1]);
                    failures++;
                }
            }

#ifdef NDEBUG
            /* Set asserts that the value fits; without the assert it keeps
             * only the low 'bits' bits */
            memset(storage, 0, sizeof(storage));
            set(storage, t - 1, neighbours[n][0]);
            set(storage, t + 1, neighbours[n][1]);
            set(storage, t, 0xffff);
            if (get(storage, t) != mask ||
                get(storage, t - 1) != neighbours[n][0] ||
                get(storage, t + 1) != neighbours[n][1]) {
                printf("%s [%u]: oversized Set reached a neighbour\n", name, t);
                failures++;
            }
#endif
        }
    }

    return failures;
}

int main(int argc, const char *argv[]) {
    int32_t i;
    uint64_t j;

    const int misaligned =
        checkMisaligned("slot16", 12, sizeof(uint16_t),
                        varintPackedSlot16_12Set, varintPackedSlot16_12Get,
                        varintPackedSlot16_12SetIncr,
                        varintPackedSlot16_12SetHalf) +
        checkMisaligned("slot32", 12, sizeof(uint32_t), varintPacked12Set,
                        varintPacked12Get, varintPacked12SetIncr,
                        varintPacked12SetHalf) +
        checkMisaligned("slot64", 13, sizeof(uint64_t),
                        varintPackedSlot64_13Set, varintPackedSlot64_13Get,
                        varintPackedSlot64_13SetIncr,
                        varintPackedSlot64_13SetHalf);
    if (misaligned) {
        printf("misaligned storage: %d failures\n", misaligned);
        return 1;
    }

    const int saturation =
        checkSaturation("slot8", 12, varintPackedCompact12Set,
                        varintPackedCompact12Get,
                        varintPackedCompact12SetIncr) +
        checkSaturation("slot16", 12, varintPackedSlot16_12Set,
                        varintPackedSlot16_12Get,
                        varintPackedSlot16_12SetIncr) +
        checkSaturation("slot32", 12, varintPacked12Set, varintPacked12Get,
                        varintPacked12SetIncr) +
        checkSaturation("slot64", 13, varintPackedSlot64_13Set,
                        varintPackedSlot64_13Get, varintPackedSlot64_13SetIncr);
    if (saturation) {
        printf("SetIncr saturation: %d failures\n", saturation);
        return 1;
    }

    if (argc < 2) {
        printf("Need loop multply factor as argument\n");
        return -3;
    }

    uint64_t boosterMultiply = (uint64_t)atoi(argv[1]);

    uint16_t holder[16384] = {0};
    for (i = 0; i < 32; i++) {
        varintPacked12Set(holder, (uint32_t)i, (uint16_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    varintPacked12Set(holder, (uint32_t)i, (uint16_t)i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "SET 12");
    }

    for (i = 0; i < 32; i++) {
        varintPacked12Get(holder, (uint32_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    uint16_t got = varintPacked12Get(holder, (uint32_t)i);
                    assert(got == (uint16_t)i);
                    (void)got;
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "GET 12");
    }

    for (i = 0; i < 32; i++) {
        varintPackedCompact12Set(holder, (uint32_t)i, (uint16_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    varintPackedCompact12Set(holder, (uint32_t)i, (uint16_t)i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j,
                                         "SET 12 (compact)");
    }

    for (i = 0; i < 32; i++) {
        varintPackedCompact12Get(holder, (uint32_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    uint16_t got =
                        varintPackedCompact12Get(holder, (uint32_t)i);
                    assert(got == (uint16_t)i);
                    (void)got;
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j,
                                         "GET 12 (compact)");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Set(holder, (uint32_t)i, (uint16_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    varintPacked13Set(holder, (uint32_t)i, (uint16_t)i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "SET 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Get(holder, (uint32_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    uint16_t got =
                        (uint16_t)varintPacked13Get(holder, (uint32_t)i);
                    assert(got == (uint16_t)i);
                    (void)got;
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "GET 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Set(holder, (uint32_t)i, (uint16_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply / 2; j++) {
                for (i = 0; i < 16384; i++) {
                    varintPacked14Set(holder, (uint32_t)i, (uint16_t)i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "SET 14");
    }

    for (i = 0; i < 32; i++) {
        varintPacked14Get(holder, (uint32_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply / 2; j++) {
                for (i = 0; i < 16384; i++) {
                    uint16_t got =
                        (uint16_t)varintPacked14Get(holder, (uint32_t)i);
                    assert(got == (uint16_t)i);
                    (void)got;
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j, "GET 14");
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    varintPacked13InsertSorted(holder, (uint32_t)i,
                                               (uint16_t)i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j,
                                         "InsertSorted 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Get(holder, (uint32_t)i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    uint16_t got = (uint16_t)varintPacked13Member(holder, 8192,
                                                                  (uint16_t)i);
                    assert(got == (uint16_t)i);
                    (void)got;
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j,
                                         "Member 13 (from InsertSorted)");
    }
}
