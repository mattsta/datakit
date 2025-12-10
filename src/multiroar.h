#pragma once

#include "databox.h"
#include "multimap.h"

typedef struct multiroar multiroar;

multiroar *multiroarBitNew(void);
multiroar *multiroarValueNew(uint8_t bitWidth, uint64_t rows, uint64_t cols);
void multiroarFree(multiroar *r);

multiroar *multiroarDuplicate(const multiroar *r);

/* Modify 'r' in place */
bool multiroarBitSet(multiroar *r, uint64_t position);
bool multiroarBitGet(const multiroar *r, uint64_t position);
void multiroarBitSetRange(multiroar *r, uint64_t start, uint64_t extent);
bool multiroarRemove(multiroar *r, uint64_t position);

bool multiroarValueSet(multiroar *r, uint64_t position, databox *value);
bool multiroarValueSetGetPrevious(multiroar *r, uint64_t position,
                                  databox *value, databox *overwritten);
bool multiroarValueRemoveGetRemoved(multiroar *r, uint64_t position,
                                    databox *removed);

/* Bitcount - count total set bits */
uint64_t multiroarBitCount(const multiroar *r);

/* Min/Max/Extrema operations */
/* First set bit, returns false if empty */
bool multiroarMin(const multiroar *r, uint64_t *position);

/* Last set bit, returns false if empty */
bool multiroarMax(const multiroar *r, uint64_t *position);
bool multiroarIsEmpty(const multiroar *r); /* True if no bits set */

/* Comparison operations */
bool multiroarIntersects(const multiroar *a, const multiroar *b);
bool multiroarIsSubset(const multiroar *a, const multiroar *b);
bool multiroarEquals(const multiroar *a, const multiroar *b);

/* Rank/Select operations (succinct data structure support) */
/* Count set bits in [0, position) */
uint64_t multiroarRank(const multiroar *r, uint64_t position);
/* Find k-th set bit (1-indexed), returns false if k > count */
bool multiroarSelect(const multiroar *r, uint64_t k, uint64_t *position);

/* Range operations */
/* Count bits in [start, end) */
uint64_t multiroarRangeCount(const multiroar *r, uint64_t start, uint64_t end);
/* Clear range [start, start+extent) */
void multiroarBitClearRange(multiroar *r, uint64_t start, uint64_t extent);

/* Flip range [start, start+extent) */
/* A - B (bits in A but not in B) */
void multiroarBitFlipRange(multiroar *r, uint64_t start, uint64_t extent);
multiroar *multiroarNewAndNot(const multiroar *a, const multiroar *b);
/* r = r - b (in-place) */
void multiroarAndNot(multiroar *r, multiroar *b);

/* Binary set operations (2 operands) - modify first argument in place */
void multiroarXor(multiroar *r, multiroar *b);
void multiroarAnd(multiroar *r, multiroar *b);
void multiroarOr(multiroar *r, multiroar *b);
void multiroarNot(multiroar *r);

/* Binary set operations - return new multiroar */
multiroar *multiroarNewXor(const multiroar *r, const multiroar *b);
multiroar *multiroarNewAnd(const multiroar *r, const multiroar *b);
multiroar *multiroarNewOr(const multiroar *r, const multiroar *b);
multiroar *multiroarNewNot(const multiroar *r);

/* N-way set operations (N >= 2) - modify first roar in array in place */
void multiroarAndN(uint64_t n, multiroar **roars);
void multiroarOrN(uint64_t n, multiroar **roars);
void multiroarXorN(uint64_t n, multiroar **roars);

/* N-way set operations - return new multiroar */
multiroar *multiroarNewAndN(uint64_t n, multiroar **roars);
multiroar *multiroarNewOrN(uint64_t n, multiroar **roars);
multiroar *multiroarNewXorN(uint64_t n, multiroar **roars);

/* Iterator for efficient traversal */
typedef struct multiroarIterator {
    const multiroar *roar;
    multimapIterator mapIter;
    databox currentChunk;
    uint64_t chunkId;
    uint64_t positionInChunk;
    uint16_t countInChunk;
    uint16_t indexInChunk;
    bool valid;
} multiroarIterator;

void multiroarIteratorInit(const multiroar *r, multiroarIterator *iter);
bool multiroarIteratorNext(multiroarIterator *iter, uint64_t *position);
void multiroarIteratorReset(multiroarIterator *iter);

/* Bulk operations */
void multiroarBitSetMany(multiroar *r, const uint64_t *positions,
                         uint64_t count);
void multiroarBitGetMany(const multiroar *r, const uint64_t *positions,
                         uint64_t count, bool *results);
uint64_t multiroarToArray(const multiroar *r, uint64_t *positions,
                          uint64_t maxCount);
multiroar *multiroarFromArray(const uint64_t *positions, uint64_t count);

/* Similarity and distance metrics */
double multiroarJaccard(const multiroar *a, const multiroar *b);
uint64_t multiroarHammingDistance(const multiroar *a, const multiroar *b);
double multiroarOverlap(const multiroar *a, const multiroar *b);
double multiroarDice(const multiroar *a, const multiroar *b);

/* Statistics and memory */
uint64_t multiroarMemoryUsage(const multiroar *r);

/* Serialization */
uint64_t multiroarSerialize(const multiroar *r, void *buf, uint64_t bufSize);
multiroar *multiroarDeserialize(const void *buf, uint64_t bufSize);
uint64_t multiroarSerializedSize(const multiroar *r);

#ifdef DATAKIT_TEST
int multiroarTest(int argc, char *argv[]);
#endif /* DATAKIT_TEST */
