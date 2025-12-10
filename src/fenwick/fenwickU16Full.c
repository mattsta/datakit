/* Fenwick Tree - U16 Full Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Full tier: (64 * 1024)+ elements, unlimited growth
 * Includes overflow protection and uint64_t indexes.
 */

#include "fenwickU16.h"

#define FENWICK_SUFFIX U16
#define FENWICK_VALUE_TYPE uint16_t
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_IS_SIGNED 0
#define FENWICK_IS_FLOATING 0
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreFullImpl.h"
