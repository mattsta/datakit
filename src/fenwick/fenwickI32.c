/* Fenwick Tree - I32: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickI32.h"
#include "../datakit.h"

#define FENWICK_SUFFIX I32
#define FENWICK_VALUE_TYPE int32_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_I32
#include "fenwickCoreDispatchImpl.h"
