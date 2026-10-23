/* Fenwick Tree - U64: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickU64.h"
#include "../datakit.h"

#define FENWICK_SUFFIX U64
#define FENWICK_VALUE_TYPE uint64_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_U64
#include "fenwickCoreDispatchImpl.h"
