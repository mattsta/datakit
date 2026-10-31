/* Segment Tree - I128 Full Tier (2-TIER SYSTEM)
 * segmentCoreFullImpl.h instantiated for this type.
 */

#include "segmentI128.h"

#ifdef __SIZEOF_INT128__

#define SEGMENT_SUFFIX I128
#define SEGMENT_VALUE_TYPE __int128_t
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#define SEGMENT_INDEX_TYPE_FULL uint64_t
/* Built from the unsigned type: 1 << 127 is signed-overflow UB */
#define SEGMENT_TYPE_MAX ((__int128_t)(~(__uint128_t)0 >> 1))
#define SEGMENT_TYPE_MIN (-SEGMENT_TYPE_MAX - 1)
#define SEGMENT_IS_SIGNED 1
#define SEGMENT_IS_FLOATING 0
#define SEGMENT_IMPL_SCOPE

#include "segmentCoreFullImpl.h"

#endif /* __SIZEOF_INT128__ */
