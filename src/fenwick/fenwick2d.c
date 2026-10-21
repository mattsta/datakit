/* 2D Fenwick Trees (typed): the I64, U64 and Double instantiations of
 * fenwick2dTypedImpl.h over fenwick2dCoreImpl.h.  Contract: fenwick2d.h */

#include "fenwick2d.h"
#include "../datakit.h"

#define F2D_SUFFIX I64
#define F2D_VALUE_T int64_t
#define F2D_IS_FLOATING 0
#include "fenwick2dTypedImpl.h"

#define F2D_SUFFIX U64
#define F2D_VALUE_T uint64_t
#define F2D_IS_FLOATING 0
#include "fenwick2dTypedImpl.h"

#define F2D_SUFFIX Double
#define F2D_VALUE_T double
#define F2D_IS_FLOATING 1
#include "fenwick2dTypedImpl.h"
