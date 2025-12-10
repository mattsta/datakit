/* Fenwick Tree - I64 Full Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Full tier: (16 * 1024)+ elements, unlimited growth
 * Includes overflow protection and uint64_t indexes.
 */

#include "fenwickI64.h"

#define FENWICK_SUFFIX I64
#define FENWICK_VALUE_TYPE int64_t
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_IS_SIGNED 1
#define FENWICK_IS_FLOATING 0
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreFullImpl.h"
