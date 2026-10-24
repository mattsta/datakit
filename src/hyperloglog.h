#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hyperloglogHeader hyperloglog;

hyperloglog *hyperloglogNew(void);
hyperloglog *hyperloglogNewSparse(void);
hyperloglog *hyperloglogNewDense(void);
hyperloglog *hyperloglogCopy(const hyperloglog *src);
void hyperloglogFree(hyperloglog *h);
bool hyperloglogDetect(hyperloglog *h);

/* The estimated cardinality: the cached value when valid, else counted and
 * cached.  0 with *invalid set (when given) for a corrupt sketch. */
uint64_t hyperloglogCount(hyperloglog *hdr, bool *invalid);

/* Add one element: 1 if a register changed (the cached count is then
 * invalid), 0 if it was already represented, -1 for a corrupt sketch.
 * '*inHll' may move (sparse growth, promotion to dense). */
int hyperloglogAdd(hyperloglog **inHll, const void *data, size_t size);

/* The cardinality of the union of 'n' sketches, without building it; 0 with
 * *invalid set (when given) if any is corrupt, 0 for n == 0. */
uint64_t hyperloglogCountUnion(const hyperloglog *const *hlls, size_t n,
                               bool *invalid);

/* A new sketch holding the union of 'n' sketches (dense if any input is);
 * NULL if any is corrupt.  n == 0 gives an empty sketch. */
hyperloglog *hyperloglogNewUnion(const hyperloglog *const *hlls, size_t n);

/* Register-wise max-fold of sketch 'hdr' into the RAW scratch 'target'
 * (one byte per register; see hyperloglogNewFromRaw); false if 'hdr' is
 * corrupt.  To fold into a stored sketch use hyperloglogMergeInto. */
bool hyperloglogMerge(hyperloglog *target, const hyperloglog *hdr);
/* Materialize a RAW-encoded scratch (register-per-byte, as built by
 * hyperloglogMerge accumulation) into a freshly allocated storable HLL
 * (sparse, or dense when 'dense' is set). */
hyperloglog *hyperloglogNewFromRaw(const hyperloglog *raw, bool dense);
/* Register-wise max-fold of 'src' (SPARSE or DENSE, never RAW) into
 * '*dst', reallocating *dst as needed (sparse growth / promotion).
 * Idempotent and commutative; invalidates *dst's cached cardinality. */
bool hyperloglogMergeInto(hyperloglog **dst, const hyperloglog *src);
void hyperloglogInvalidateCache(hyperloglog *h);

#ifdef DATAKIT_TEST
int hyperloglogTest(int argc, char *argv[]);
#endif
