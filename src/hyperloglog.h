#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hyperloglogHeader hyperloglog;

/* Direct API */
hyperloglog *hyperloglogNew(void);
hyperloglog *hyperloglogNewSparse(void);
hyperloglog *hyperloglogNewDense(void);
hyperloglog *hyperloglogCopy(const hyperloglog *src);
void hyperloglogFree(hyperloglog *h);
bool hyperloglogDetect(hyperloglog *h);
uint64_t hyperloglogCount(hyperloglog *hdr, bool *invalid);
int hyperloglogAdd(hyperloglog **inHll, const void *data, size_t size);
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

/* User-friendly API */
int pfadd(hyperloglog **hh, ...);
uint64_t pfcount(hyperloglog *h, ...);
uint64_t pfcountSingle(hyperloglog *h);
hyperloglog *pfmerge(hyperloglog *h, ...);

/* We probably also want an API-friendly API where functions
 * can accept a direct array of hyperloglogs/data for operations. */

#ifdef DATAKIT_TEST
int hyperloglogTest(int argc, char *argv[]);
#endif
