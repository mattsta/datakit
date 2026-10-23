/* Fenwick Tree - I128: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickI128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define FENWICK_SUFFIX I128
#define FENWICK_VALUE_TYPE __int128_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_I128
#include "fenwickCoreDispatchImpl.h"
#endif /* __SIZEOF_INT128__ */
