/* Fenwick Tree - I64: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickI64.h"
#include "../datakit.h"

#define FENWICK_SUFFIX I64
#define FENWICK_VALUE_TYPE int64_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_I64
#include "fenwickCoreDispatchImpl.h"
