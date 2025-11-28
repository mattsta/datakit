#include <assert.h>
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

#include "perf.h"

int main(int argc, const char *argv[]) {
    int32_t i;
    uint64_t j;

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
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS((uint64_t)i * (uint64_t)j,
                                         "Member 13 (from InsertSorted)");
    }
}
