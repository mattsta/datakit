#include "dkTierLimit.h"

#include <stdatomic.h>

/* One limit per family.  A default publishes no other data, so plain
 * relaxed loads and stores carry it. */
static _Atomic uint8_t dkTierDefaultLimit_[DK_TIER_FAMILY_COUNT] = {
    [0 ... DK_TIER_FAMILY_COUNT - 1] = DK_TIER_LIMIT_COMPILED};

flexCapSizeLimit dkTierDefaultLimit(dkTierFamily family) {
    if ((uint32_t)family >= DK_TIER_FAMILY_COUNT) {
        return DK_TIER_LIMIT_COMPILED;
    }

    /* @sync: relaxed hint */
    return (flexCapSizeLimit)atomic_load_explicit(&dkTierDefaultLimit_[family],
                                                  memory_order_relaxed);
}

dkTierLimitResult dkTierSetDefaultLimit(dkTierFamily family,
                                        flexCapSizeLimit limit) {
    if ((uint32_t)family >= DK_TIER_FAMILY_COUNT) {
        return DK_TIER_LIMIT_BAD_FAMILY;
    }

    if ((uint32_t)limit < FLEX_CAP_LEVEL_64 ||
        (uint32_t)limit > FLEX_CAP_LEVEL_65536) {
        return DK_TIER_LIMIT_OUT_OF_RANGE;
    }

    /* @sync: relaxed hint */
    atomic_store_explicit(&dkTierDefaultLimit_[family], (uint8_t)limit,
                          memory_order_relaxed);
    return DK_TIER_LIMIT_SET;
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"

#include "multiOrderedSet.h"
#include "multilist.h"
#include "multimap.h"

defcleaner(multimap *, multimapFree);
defcleaner(multiOrderedSet *, multiOrderedSetFree);
defcleaner(multilist *, multilistFree);

/* Make a container with the family's plain constructor and read the
 * limit it was given */
static uint32_t dkTierLimitTestNewLimit_(dkTierFamily family) {
    dkTierStats s = {};
    switch (family) {
    case DK_TIER_MULTIMAP: {
        cleaner(multimapFree) multimap *m = multimapNew(2);
        multimapStats(m, &s);
        break;
    }
    case DK_TIER_MULTIORDEREDSET: {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        multiOrderedSetStats(mos, &s);
        break;
    }
    case DK_TIER_MULTILIST: {
        cleaner(multilistFree) multilist *ml = multilistNew();
        multilistStats(ml, &s);
        if (multilistDepth(ml) != 0) {
            return 0;
        }
        break;
    }
    default:
        break;
    }

    return s.unitLimit;
}

/* Set a default the test needs; a refusal fails the test */
#define TIER_SET(family, limit)                                                \
    do {                                                                       \
        if (dkTierSetDefaultLimit(family, limit) != DK_TIER_LIMIT_SET) {       \
            ERR("setting family %d to limit %d was refused", (int)(family),    \
                (int)(limit));                                                 \
        }                                                                      \
    } while (0)

int dkTierLimitTest(int, char *[]) {
    int err = 0;

    TEST("every family starts at the compiled default") {
        for (uint32_t f = 0; f < DK_TIER_FAMILY_COUNT; f++) {
            if (dkTierDefaultLimit((dkTierFamily)f) != DK_TIER_LIMIT_COMPILED) {
                ERR("family %u starts at limit %u", f,
                    dkTierDefaultLimit((dkTierFamily)f));
            }
        }
    }

    TEST("a set default reaches only its own family's new containers") {
        for (uint32_t f = 0; f < DK_TIER_FAMILY_COUNT; f++) {
            const dkTierFamily family = (dkTierFamily)f;
            const dkTierFamily other =
                (dkTierFamily)((f + 1) % DK_TIER_FAMILY_COUNT);
            for (uint32_t level = FLEX_CAP_LEVEL_64;
                 level <= FLEX_CAP_LEVEL_65536; level++) {
                if (dkTierSetDefaultLimit(family, (flexCapSizeLimit)level) !=
                    DK_TIER_LIMIT_SET) {
                    ERR("family %u refused level %u", f, level);
                }

                if (dkTierDefaultLimit(family) != (flexCapSizeLimit)level ||
                    dkTierLimitTestNewLimit_(family) !=
                        flexOptimizationSizeLimit[level]) {
                    ERR("family %u level %u: default %u, new container %u", f,
                        level, dkTierDefaultLimit(family),
                        dkTierLimitTestNewLimit_(family));
                }

                if (dkTierLimitTestNewLimit_(other) !=
                    flexOptimizationSizeLimit[DK_TIER_LIMIT_COMPILED]) {
                    ERR("setting family %u moved family %u", f, other);
                }
            }

            TIER_SET(family, DK_TIER_LIMIT_COMPILED);
        }
    }

    TEST("a refused default changes nothing") {
        const flexCapSizeLimit set = FLEX_CAP_LEVEL_1024;
        TIER_SET(DK_TIER_MULTIMAP, set);
        const struct {
            uint32_t family;
            uint32_t level;
            dkTierLimitResult want;
        } bad[] = {
            {DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_0, DK_TIER_LIMIT_OUT_OF_RANGE},
            {DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_65536 + 1,
             DK_TIER_LIMIT_OUT_OF_RANGE},
            {DK_TIER_MULTIMAP, 255, DK_TIER_LIMIT_OUT_OF_RANGE},
            {DK_TIER_FAMILY_COUNT, FLEX_CAP_LEVEL_512,
             DK_TIER_LIMIT_BAD_FAMILY},
            {UINT32_MAX, FLEX_CAP_LEVEL_512, DK_TIER_LIMIT_BAD_FAMILY},
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
            const dkTierLimitResult got = dkTierSetDefaultLimit(
                (dkTierFamily)bad[i].family, (flexCapSizeLimit)bad[i].level);
            if (got != bad[i].want) {
                ERR("case %zu: result %d, want %d", i, got, bad[i].want);
            }
        }

        if (dkTierDefaultLimit(DK_TIER_MULTIMAP) != set ||
            dkTierDefaultLimit(DK_TIER_MULTIORDEREDSET) !=
                DK_TIER_LIMIT_COMPILED ||
            dkTierDefaultLimit(DK_TIER_FAMILY_COUNT) !=
                DK_TIER_LIMIT_COMPILED) {
            ERR("a refused set moved a default: multimap %u, "
                "multiOrderedSet %u",
                dkTierDefaultLimit(DK_TIER_MULTIMAP),
                dkTierDefaultLimit(DK_TIER_MULTIORDEREDSET));
        }

        TIER_SET(DK_TIER_MULTIMAP, DK_TIER_LIMIT_COMPILED);
    }

    TEST("an existing container keeps the limit it was made with") {
        cleaner(multimapFree) multimap *m = multimapNew(2);
        cleaner(multimapFree) multimap *set = multimapSetNew(2);
        TIER_SET(DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_256);
        cleaner(multimapFree) multimap *after = multimapSetNew(2);
        dkTierStats s;
        multimapStats(m, &s);
        if (s.unitLimit != flexOptimizationSizeLimit[DK_TIER_LIMIT_COMPILED]) {
            ERR("map made before the change has limit %u", s.unitLimit);
        }

        multimapStats(set, &s);
        if (s.unitLimit != flexOptimizationSizeLimit[DK_TIER_LIMIT_COMPILED]) {
            ERR("set made before the change has limit %u", s.unitLimit);
        }

        multimapStats(after, &s);
        if (s.unitLimit != flexOptimizationSizeLimit[FLEX_CAP_LEVEL_256]) {
            ERR("set made after the change has limit %u", s.unitLimit);
        }

        TIER_SET(DK_TIER_MULTIMAP, DK_TIER_LIMIT_COMPILED);
    }

    TEST_FINAL_RESULT;
}
#endif
