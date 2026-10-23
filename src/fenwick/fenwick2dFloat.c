/* 2D Fenwick Tree - Float: the public API (fenwick2dTypedImpl.h) */

#include "fenwick2dFloat.h"
#include "../datakit.h"

#define F2D_SUFFIX Float
#define F2D_VALUE_T float
#define F2D_IS_FLOATING 1
#define F2D_PERSIST_TYPE PERSIST_TYPE_FENWICK2D_FLOAT
#include "fenwick2dTypedImpl.h"
