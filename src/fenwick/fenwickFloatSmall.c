/* Fenwick Tree - Float Small Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Small tier: 0-(32 * 1024) elements, contiguous allocation
 * Upgrades to Full tier when threshold exceeded.
 */

#include "fenwickFloat.h"

#define FENWICK_SUFFIX Float
#define FENWICK_VALUE_TYPE float
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_SMALL_MAX_COUNT (32 * 1024)
#define FENWICK_SMALL_MAX_BYTES (128 * 1024)
#define FENWICK_IS_SIGNED 0
#define FENWICK_IS_FLOATING 1
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreImpl.h"
