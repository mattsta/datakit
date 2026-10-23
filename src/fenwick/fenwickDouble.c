/* Fenwick Tree - Double: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickDouble.h"
#include "../datakit.h"

#define FENWICK_SUFFIX Double
#define FENWICK_VALUE_TYPE double
#define FENWICK_IS_FLOATING 1
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_DOUBLE
#include "fenwickCoreDispatchImpl.h"
