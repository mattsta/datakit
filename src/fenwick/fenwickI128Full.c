/* Fenwick Tree - I128 Full Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 *
 * Full tier: (8 * 1024)+ elements, unlimited growth
 * Includes overflow protection and uint64_t indexes.
 */

#include "fenwickI128.h"

#ifdef __SIZEOF_INT128__

#define FENWICK_SUFFIX I128
#define FENWICK_VALUE_TYPE __int128_t
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#define FENWICK_INDEX_TYPE_FULL uint64_t
#define FENWICK_IS_SIGNED 1
#define FENWICK_IS_FLOATING 0
#define FENWICK_IMPL_SCOPE

#include "fenwickCoreFullImpl.h"

#endif /* __SIZEOF_INT128__ */
