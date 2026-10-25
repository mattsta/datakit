#pragma once

#include "flexCapacityManagement.h"

/* The size limit a family's plain constructor gives each new container:
 * multimapNew and multimapSetNew read DK_TIER_MULTIMAP's,
 * multiOrderedSetNew reads DK_TIER_MULTIORDEREDSET's, multilistNew reads
 * DK_TIER_MULTILIST's.  Constructors that take a limit ignore these
 * defaults.
 *
 * Every family starts at DK_TIER_LIMIT_COMPILED.  `dktools tune`
 * measures each family's speed and memory per limit on the machine it
 * runs on; an operator sets the limit it recommends once at startup.  A
 * change applies to containers made after it: existing containers keep
 * the limit they were made with.  Reads and writes are safe from any
 * thread; a constructor racing a change uses either limit. */

#define DK_TIER_LIMIT_COMPILED FLEX_CAP_LEVEL_2048

typedef enum dkTierFamily {
    DK_TIER_MULTIMAP = 0,
    DK_TIER_MULTIORDEREDSET = 1,
    DK_TIER_MULTILIST = 2,
    DK_TIER_FAMILY_COUNT
} dkTierFamily;

typedef enum dkTierLimitResult {
    DK_TIER_LIMIT_SET = 0,      /* the family's default is now the limit */
    DK_TIER_LIMIT_BAD_FAMILY,   /* not a dkTierFamily; nothing changed */
    DK_TIER_LIMIT_OUT_OF_RANGE, /* not FLEX_CAP_LEVEL_64..FLEX_CAP_LEVEL_65536;
                                   nothing changed */
} dkTierLimitResult;

/* The family's current default (DK_TIER_LIMIT_COMPILED for a family
 * that isn't a dkTierFamily). */
flexCapSizeLimit dkTierDefaultLimit(dkTierFamily family);
dkTierLimitResult dkTierSetDefaultLimit(dkTierFamily family,
                                        flexCapSizeLimit limit);

#ifdef DATAKIT_TEST
int dkTierLimitTest(int argc, char *argv[]);
#endif
