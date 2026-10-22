/* 2D Fenwick Tree - U128: the public API (fenwick2dTypedImpl.h) */

#include "fenwick2dU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define F2D_SUFFIX U128
#define F2D_VALUE_T __uint128_t
#define F2D_IS_FLOATING 0
#include "fenwick2dTypedImpl.h"
#endif /* __SIZEOF_INT128__ */
