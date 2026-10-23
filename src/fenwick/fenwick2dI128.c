/* 2D Fenwick Tree - I128: the public API (fenwick2dTypedImpl.h) */

#include "fenwick2dI128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__
#define F2D_SUFFIX I128
#define F2D_VALUE_T __int128_t
#define F2D_IS_FLOATING 0
#define F2D_PERSIST_TYPE PERSIST_TYPE_FENWICK2D_I128
#include "fenwick2dTypedImpl.h"
#endif /* __SIZEOF_INT128__ */
