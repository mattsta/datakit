/* Segment Tree - U128: the public API (segmentCoreDispatchImpl.h) */

#include "segmentU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define SEGMENT_SUFFIX U128
#define SEGMENT_VALUE_TYPE __uint128_t
#define SEGMENT_IS_FLOATING 0
#define SEGMENT_PERSIST_TYPE PERSIST_TYPE_SEGMENT_U128
#include "segmentCoreDispatchImpl.h"
#endif /* __SIZEOF_INT128__ */
