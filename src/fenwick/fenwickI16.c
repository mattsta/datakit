/* Fenwick Tree - I16: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickI16.h"
#include "../datakit.h"

#define FENWICK_SUFFIX I16
#define FENWICK_VALUE_TYPE int16_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_I16
#include "fenwickCoreDispatchImpl.h"
