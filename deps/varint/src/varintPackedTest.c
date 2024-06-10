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

int main(int argc, char *argv[]) {
    int32_t i;
    uint64_t j;

    if (argc < 2) {
        printf("Need loop multply factor as argument\n");
        return -3;
    }

    uint64_t boosterMultiply = atoi(argv[1]);

    uint16_t holder[16384] = {0};
    for (i = 0; i < 32; i++) {
        varintPacked12Set(holder, i, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    varintPacked12Set(holder, i, i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "SET 12");
    }

    for (i = 0; i < 32; i++) {
        varintPacked12Get(holder, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    uint16_t got = varintPacked12Get(holder, i);
                    assert(got == i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "GET 12");
    }

    for (i = 0; i < 32; i++) {
        varintPackedCompact12Set(holder, i, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    varintPackedCompact12Set(holder, i, i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "SET 12 (compact)");
    }

    for (i = 0; i < 32; i++) {
        varintPackedCompact12Get(holder, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 4096; i++) {
                    uint16_t got = varintPackedCompact12Get(holder, i);
                    assert(got == i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "GET 12 (compact)");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Set(holder, i, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    varintPacked13Set(holder, i, i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "SET 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Get(holder, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    uint16_t got = varintPacked13Get(holder, i);
                    assert(got == i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "GET 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Set(holder, i, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply / 2; j++) {
                for (i = 0; i < 16384; i++) {
                    varintPacked14Set(holder, i, i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "SET 14");
    }

    for (i = 0; i < 32; i++) {
        varintPacked14Get(holder, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply / 2; j++) {
                for (i = 0; i < 16384; i++) {
                    uint16_t got = varintPacked14Get(holder, i);
                    assert(got == i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "GET 14");
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    varintPacked13InsertSorted(holder, i, i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j, "InsertSorted 13");
    }

    for (i = 0; i < 32; i++) {
        varintPacked13Get(holder, i);
    }

    {
        PERF_TIMERS_SETUP;
        {
            for (j = 0; j < boosterMultiply; j++) {
                for (i = 0; i < 8192; i++) {
                    uint16_t got = varintPacked13Member(holder, 8192, i);
                    assert(got == i);
                }
            }
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(i * j,
                                         "Member 13 (from InsertSorted)");
    }
}
