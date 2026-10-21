/* Segment Tree - I128: the public API (segmentCoreDispatchImpl.h) */

#include "segmentI128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define SEGMENT_SUFFIX I128
#define SEGMENT_VALUE_TYPE __int128_t
#include "segmentCoreDispatchImpl.h"
#endif /* __SIZEOF_INT128__ */
