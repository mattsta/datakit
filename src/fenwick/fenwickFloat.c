/* Fenwick Tree - Float: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickFloat.h"
#include "../datakit.h"

#define FENWICK_SUFFIX Float
#define FENWICK_VALUE_TYPE float
#define FENWICK_IS_FLOATING 1
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_FLOAT
#include "fenwickCoreDispatchImpl.h"
