/* Fenwick Tree - Double Full Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Full tier: (16 * 1024)+ elements, unlimited growth
 * Includes overflow protection and uint64_t indexes.
 */

#include "fenwickDouble.h"

#define FENWICK_SUFFIX Double
#define FENWICK_VALUE_TYPE double
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_IS_SIGNED 0
#define FENWICK_IS_FLOATING 1
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreFullImpl.h"
