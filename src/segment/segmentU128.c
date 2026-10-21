/* Segment Tree - U128: the public API (segmentCoreDispatchImpl.h) */

#include "segmentU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define SEGMENT_SUFFIX U128
#define SEGMENT_VALUE_TYPE __uint128_t
#include "segmentCoreDispatchImpl.h"
#endif /* __SIZEOF_INT128__ */
