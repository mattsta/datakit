/* Segment Tree - Double Small Tier (2-TIER SYSTEM)
 * segmentCoreImpl.h instantiated for this type.
 */

#include "segmentDouble.h"

#define SEGMENT_SUFFIX Double
#define SEGMENT_VALUE_TYPE double
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#define SEGMENT_INDEX_TYPE_FULL uint64_t
#define SEGMENT_SMALL_MAX_COUNT (8 * 1024)
#define SEGMENT_TYPE_MIN -DBL_MAX
#define SEGMENT_TYPE_MAX DBL_MAX
#define SEGMENT_IS_SIGNED 0
#define SEGMENT_IS_FLOATING 1
#define SEGMENT_IMPL_SCOPE

#include "segmentCoreImpl.h"
