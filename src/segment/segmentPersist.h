#pragma once

/* persistOps for the typed segment trees (segmentCoreDispatchImpl.h).
 *
 * Every node is its children's fold, so the elements determine a tree,
 * float sums included:
 *   snapshot: flex [operation (SUM 0, MIN 1, MAX 2), element 0, ...]
 *   restore:  segment<T>NewFromArray(operation, elements, count)
 * WAL (args: const databox *const *):
 *   UPDATE [index (UNSIGNED_64), value]          segment<T>Update
 *   UPDATE [left, right (UNSIGNED_64), value]    segment<T>RangeUpdate
 * A value box is any integer box whose value fits T (128-bit boxes for
 * the 128-bit trees), FLOAT_32 for Float, DOUBLE_64 or FLOAT_32 for
 * Double. */

#include "../persist.h"

extern const persistOps persistOpsSegmentI16;
extern const persistOps persistOpsSegmentI32;
extern const persistOps persistOpsSegmentI64;
extern const persistOps persistOpsSegmentU16;
extern const persistOps persistOpsSegmentU32;
extern const persistOps persistOpsSegmentU64;
extern const persistOps persistOpsSegmentFloat;
extern const persistOps persistOpsSegmentDouble;
#ifdef __SIZEOF_INT128__
extern const persistOps persistOpsSegmentI128;
extern const persistOps persistOpsSegmentU128;
#endif
