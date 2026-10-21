/* Fenwick Tree - U128: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define FENWICK_SUFFIX U128
#define FENWICK_VALUE_TYPE __uint128_t
#define FENWICK_IS_FLOATING 0
#include "fenwickCoreDispatchImpl.h"
#endif /* __SIZEOF_INT128__ */
