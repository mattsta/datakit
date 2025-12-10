/* Fenwick Tree - U16 Small Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Small tier: 0-(64 * 1024) elements, contiguous allocation
 * Upgrades to Full tier when threshold exceeded.
 */

#include "fenwickU16.h"

#define FENWICK_SUFFIX U16
#define FENWICK_VALUE_TYPE uint16_t
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_SMALL_MAX_COUNT (64 * 1024)
#define FENWICK_SMALL_MAX_BYTES (128 * 1024)
#define FENWICK_IS_SIGNED 0
#define FENWICK_IS_FLOATING 0
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreImpl.h"
