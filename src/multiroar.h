#pragma once

#include "databox.h"
#include "dkFormat.h"
#include "multimap.h"

typedef struct multiroar multiroar;

multiroar *multiroarBitNew(void);
multiroar *multiroarValueNew(uint8_t bitWidth, uint64_t rows, uint64_t cols);
void multiroarFree(multiroar *r);

multiroar *multiroarDuplicate(const multiroar *r);

/* Modes.  A roar is bit-mode (multiroarBitNew: a set of positions) or
 * value-mode (multiroarValueNew: position -> unsigned value of a fixed
 * width).  Every operation belongs to one mode and refuses a roar of the
 * other, safely and with one test: bit calls on a value roar answer
 * false / 0 / NULL or change nothing, value calls on a bit roar answer
 * MULTIROAR_VALUE_REFUSED / false.  Mode-free calls (New/Free, Duplicate,
 * Clear, IsEmpty, Equals, MemoryUsage, Serialize*) take either. */

/* Bit mode; 'r' modified in place.  BitSet and Remove return whether the
 * bit was set before the call (false too for a value-mode roar, which they
 * leave unchanged). */
bool multiroarBitSet(multiroar *r, uint64_t position);
bool multiroarBitGet(const multiroar *r, uint64_t position);
void multiroarBitSetRange(multiroar *r, uint64_t start, uint64_t extent);
bool multiroarRemove(multiroar *r, uint64_t position);

/* Value mode.  Values are unsigned integers that fit the declared bitWidth
 * (a signed value must be >= 0; booleans store as 0/1) and round-trip as
 * unsigned databoxes.  A set is refused, changing nothing, on a bit roar,
 * outside the declared rows * cols, or for a value that cannot be stored;
 * Get and Remove answer false in the same cases. */
typedef enum multiroarValueSetResult {
    MULTIROAR_VALUE_REFUSED = 0, /* nothing stored */
    MULTIROAR_VALUE_ADDED,       /* the position held no value */
    MULTIROAR_VALUE_REPLACED,    /* the position's value was replaced */
} multiroarValueSetResult;
multiroarValueSetResult multiroarValueSet(multiroar *r, uint64_t position,
                                          databox *value);
multiroarValueSetResult multiroarValueSetGetPrevious(multiroar *r,
                                                     uint64_t position,
                                                     databox *value,
                                                     databox *overwritten);
bool multiroarValueGet(const multiroar *r, uint64_t position, databox *value);
bool multiroarValueRemoveGetRemoved(multiroar *r, uint64_t position,
                                    databox *removed);

/* A value-mode roar's bit width (0: a bit-mode roar), declared rows and
 * cols (0: unbounded; both 0 for bit mode) and count of stored values (0
 * for bit mode; multiroarBitCount counts bits) */
uint8_t multiroarValueWidth(const multiroar *r);
void multiroarValueDimensions(const multiroar *r, uint64_t *rows,
                              uint64_t *cols);
uint64_t multiroarValueCount(const multiroar *r);

/* Whether `position` is inside the declared rows x cols (always, when
 * either is 0 or their product overflows) */
bool multiroarValueInBounds(const multiroar *r, uint64_t position);

/* Empty the roar, keeping its mode, width and dimensions */
void multiroarClear(multiroar *r);

/* Bitcount - count total set bits */
uint64_t multiroarBitCount(const multiroar *r);

/* Min/Max/Extrema operations */
/* First set bit, returns false if empty */
bool multiroarMin(const multiroar *r, uint64_t *position);

/* Last set bit, returns false if empty */
bool multiroarMax(const multiroar *r, uint64_t *position);
bool multiroarIsEmpty(const multiroar *r); /* True if no bits set */

/* Comparison operations.  Here and in the set operations, a NULL operand
 * is the empty set (an empty set is a subset of anything; NULL equals only
 * NULL); a NULL first set of a New* operation gives NULL, and a NULL target
 * of an in-place operation is left alone. */
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
    uint16_t
        unsetCursor; /* OVER_FULL: monotonic index into sorted unset list */
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
/* A new bit-mode roar of 'count' positions (any order, repeats allowed);
 * count 0 gives an empty roar, positions may then be NULL.  NULL only for
 * NULL positions with a nonzero count. */
multiroar *multiroarFromArray(const uint64_t *positions, uint64_t count);

/* Similarity and distance metrics.  Two empty sets have Jaccard and Dice
 * 1 (identical); the overlap coefficient is 0 when either set is empty
 * (there is no smaller set to divide by).  A NULL roar gives 0. */
double multiroarJaccard(const multiroar *a, const multiroar *b);
uint64_t multiroarHammingDistance(const multiroar *a, const multiroar *b);
double multiroarOverlap(const multiroar *a, const multiroar *b);
double multiroarDice(const multiroar *a, const multiroar *b);

/* Statistics and memory */
uint64_t multiroarMemoryUsage(const multiroar *r);

/* Serialization of either mode: the stream records the mode (with a value
 * roar's width and dimensions), then its bits or values.  Serialize
 * returns 0 when the buffer is too small.  Deserialize returns NULL for
 * malformed bytes: it checks the framing, that every byte belongs to the
 * stream, that chunk ids ascend and each position listing holds 1 to 629
 * ascending in-chunk positions, and that values ascend by position, fit
 * the width and stay inside declared dimensions.
 *
 * multiroarSerialize writes a standalone artifact: the dkFormat prefix of
 * multiroarFormat, then the stream; multiroarDeserialize refuses a stamp
 * this build does not read.  The Raw forms are the stream alone, for a
 * container that stamps it (persist files record the stamp in their
 * header). */
#define MULTIROAR_FORMAT_ID ((uint32_t)70) /* PERSIST_TYPE_MULTIROAR */
#define MULTIROAR_FORMAT_VERSION ((uint32_t)1)
extern const dkFormatSpec multiroarFormat;

uint64_t multiroarSerialize(const multiroar *r, void *buf, uint64_t bufSize);
multiroar *multiroarDeserialize(const void *buf, uint64_t bufSize);
uint64_t multiroarSerializedSize(const multiroar *r);
uint64_t multiroarSerializeRaw(const multiroar *r, void *buf, uint64_t bufSize);
multiroar *multiroarDeserializeRaw(const void *buf, uint64_t bufSize);
uint64_t multiroarSerializedSizeRaw(const multiroar *r);

#ifdef DATAKIT_TEST
int multiroarTest(int argc, char *argv[]);
#endif /* DATAKIT_TEST */
