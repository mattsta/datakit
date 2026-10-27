#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct hyperloglogHeader hyperloglog;

[[nodiscard]] hyperloglog *hyperloglogNew(void);
[[nodiscard]] hyperloglog *hyperloglogNewSparse(void);
[[nodiscard]] hyperloglog *hyperloglogNewDense(void);
[[nodiscard]] hyperloglog *hyperloglogCopy(const hyperloglog *src);
void hyperloglogFree(hyperloglog *h);
bool hyperloglogDetect(hyperloglog *h);

/* The estimated cardinality: the cached value when valid, else counted and
 * cached.  0 with *invalid set (when given) for a corrupt sketch. */
uint64_t hyperloglogCount(hyperloglog *hdr, bool *invalid);

/* What one add did to a sketch */
typedef enum hyperloglogAddResult {
    HYPERLOGLOG_ADD_CORRUPT = -1,  /* the sketch is corrupt: nothing changed */
    HYPERLOGLOG_ADD_UNCHANGED = 0, /* the element was already represented */
    HYPERLOGLOG_ADD_CHANGED = 1,   /* a register grew: the cached count is
                                      invalid */
} hyperloglogAddResult;

/* Add one element.  '*inHll' may move (sparse growth, promotion to
 * dense). */
hyperloglogAddResult hyperloglogAdd(hyperloglog **inHll, const void *data,
                                    size_t size);

/* Add 'n' elements (data[i], lens[i]) in one call: how many changed a
 * register, or -1 at the first corrupt-sketch report (the adds stop there).
 * '*inHll' may move as for hyperloglogAdd. */
int64_t hyperloglogAddMany(hyperloglog **inHll, const void *const *data,
                           const size_t *lens, size_t n);

/* The cardinality of the union of 'n' sketches, without building it; 0 with
 * *invalid set (when given) if any is corrupt, 0 for n == 0. */
uint64_t hyperloglogCountUnion(const hyperloglog *const *hlls, size_t n,
                               bool *invalid);

/* A new sketch holding the union of 'n' sketches (dense if any input is);
 * NULL if any is corrupt.  n == 0 gives an empty sketch. */
[[nodiscard]] hyperloglog *hyperloglogNewUnion(const hyperloglog *const *hlls,
                                               size_t n);

/* Register-wise max-fold of sketch 'hdr' into the RAW scratch 'target'
 * (one byte per register; see hyperloglogNewFromRaw); false if 'hdr' is
 * corrupt.  To fold into a stored sketch use hyperloglogMergeInto. */
bool hyperloglogMerge(hyperloglog *target, const hyperloglog *hdr);
/* Materialize a RAW-encoded scratch (register-per-byte, as built by
 * hyperloglogMerge accumulation) into a freshly allocated storable HLL
 * (sparse, or dense when 'dense' is set). */
[[nodiscard]] hyperloglog *hyperloglogNewFromRaw(const hyperloglog *raw,
                                                 bool dense);
/* Register-wise max-fold of 'src' (SPARSE or DENSE, never RAW) into
 * '*dst', reallocating *dst as needed (sparse growth / promotion).
 * Idempotent and commutative; invalidates *dst's cached cardinality. */
bool hyperloglogMergeInto(hyperloglog **dst, const hyperloglog *src);
void hyperloglogInvalidateCache(hyperloglog *h);

#ifdef DATAKIT_TEST
int hyperloglogTest(int argc, char *argv[]);
#endif
