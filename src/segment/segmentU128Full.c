/* Segment Tree - U128 Full Tier (2-TIER SYSTEM)
 * Auto-generated from template. DO NOT EDIT MANUALLY.
 */

#include "segmentU128.h"

#ifdef __SIZEOF_INT128__

#define SEGMENT_SUFFIX U128
#define SEGMENT_VALUE_TYPE __uint128_t
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#define SEGMENT_INDEX_TYPE_FULL uint64_t
#define SEGMENT_TYPE_MIN 0
#define SEGMENT_TYPE_MAX ((__uint128_t) - 1)
#define SEGMENT_IS_SIGNED 0
#define SEGMENT_IS_FLOATING 0
#define SEGMENT_IMPL_SCOPE

#include "segmentCoreFullImpl.h"

#endif /* __SIZEOF_INT128__ */
