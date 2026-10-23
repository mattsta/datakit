/* Fenwick Tree - U16: the public API (fenwickCoreDispatchImpl.h) */

#include "fenwickU16.h"
#include "../datakit.h"

#define FENWICK_SUFFIX U16
#define FENWICK_VALUE_TYPE uint16_t
#define FENWICK_IS_FLOATING 0
#define FENWICK_PERSIST_TYPE PERSIST_TYPE_FENWICK_U16
#include "fenwickCoreDispatchImpl.h"
