/* Fenwick Tree - U32: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickU32.h"
#include "../datakit.h"

#define FENWICK_SUFFIX U32
#define FENWICK_VALUE_TYPE uint32_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_U32
#include "fenwickCoreDispatchImpl.h"
