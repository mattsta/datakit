/* Segment Tree - Float Small Tier (2-TIER SYSTEM)
 * segmentCoreImpl.h instantiated for this type.
 */

#include "segmentFloat.h"

#define SEGMENT_SUFFIX Float
#define SEGMENT_VALUE_TYPE float
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#define SEGMENT_INDEX_TYPE_FULL uint64_t
#define SEGMENT_SMALL_MAX_COUNT (16 * 1024)
#define SEGMENT_TYPE_MIN -FLT_MAX
#define SEGMENT_TYPE_MAX FLT_MAX
#define SEGMENT_IS_SIGNED 0
#define SEGMENT_IS_FLOATING 1
#define SEGMENT_IMPL_SCOPE

#include "segmentCoreImpl.h"
