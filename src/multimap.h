#pragma once

#include "flexCapacityManagement.h"
#include "multimapCommon.h"

#include <assert.h>

/* opaque multimap type; there's no user accessible data here. */
typedef struct multimap multimap;

multimap *multimapNew(multimapElements elementsPerEntry);
multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const flexCapSizeLimit limit);
multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const flexCapSizeLimit limit);
multimap *multimapNewConfigure(multimapElements elementsPerEntry, bool isSet,
                               bool compress, flexCapSizeLimit sizeLimit);
multimap *multimapSetNew(multimapElements elementsPerEntry);
multimap *multimapCopy(const multimap *m);
size_t multimapCount(const multimap *m);
size_t multimapBytes(const multimap *m);
flex *multimapDump(const multimap *m);

bool multimapInsert(multimap **m, const databox *elements[]);
/* multimapInsert that also reports the value entries a replace overwrote
 * through 'replacedOut' ('elementsPerEntry - 1' slots in tuple order after
 * the key, every one VOID when nothing was replaced).  Integers up to 64
 * bits, reals and pointers are reported by value and stay valid.  A BYTES
 * or container slot points into the rewritten map and must not be read.  A
 * 96/128-bit integer does not fit a databox, so its slot reports only the
 * type (SIGNED_128 or UNSIGNED_128) with a NULL payload pointer (data.i128
 * or data.u128); read such a value before the insert if it is needed. */
bool multimapInsertReplacing(multimap **m, const databox *elements[],
                             databox *replacedOut);
void multimapInsertFullWidth(multimap **m, const databox *elements[]);
void multimapAppend(multimap **m, const databox *elements[]);

void multimapInsertWithSurrogateKey(
    multimap **m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me);
bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
void multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen);
void multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box);

bool multimapExists(const multimap *m, const databox *key);
bool multimapExistsFullWidth(const multimap *m, const databox *elements[]);
bool multimapExistsWithReference(const multimap *m, const databox *key,
                                 databox *foundRef,
                                 const struct multimapAtom *referenceContainer);

/* Output boxes.  Every read that fills caller boxes (Lookup, First, Last,
 * LookupByPosition, RandomValue, IteratorNext and the *Into helpers)
 * decodes in place under the databox.h rule: the caller owns the boxes
 * and sets up their wide capability.  To receive a 96/128-bit value, pass
 * a databoxBig armed with DATABOX_BIG_INIT, and re-arm it before each
 * read, since a narrow value overwrites the payload pointer.  A zeroed box
 * receives only the type (data.ptr NULL).  An uninitialized box is a
 * caller bug.  DeleteRandomValue copies through flexGetByTypeCopy, so its
 * boxes need an initialized 'big' too.  Rows the library decodes for
 * itself (ProcessUntil walkers, the key set operations) always carry
 * wide storage, with 'big' set exactly on 96/128-bit values. */
bool multimapLookup(const multimap *m, const databox *key, databox *elements[]);

/* NOTE: despite the scalar-looking `databox **found`, this fills a
 * FULL-WIDTH pointer array (elementsPerEntry boxes) when non-NULL -
 * pass NULL or an array sized to the map's width, never a single
 * box's address. */
bool multimapRandomValue(multimap *m, bool fromTail, databox **found,
                         multimapEntry *me);

bool multimapDelete(multimap **m, const databox *key);
bool multimapDeleteFullWidth(multimap **m, const databox *elements[]);
bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference);
bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference);
bool multimapDeleteRandomValue(multimap **m, bool fromTail, databox **deleted);

int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy);

void multimapReset(multimap *m);
void multimapFree(multimap *m);

void multimapEntryResize(multimap **m, const databox *key, size_t newSize);
void multimapEntryReplace(multimap **m, const databox *key,
                          const databox *elements[]);

bool multimapFirst(multimap *m, databox *elements[]);
bool multimapLast(multimap *m, databox *elements[]);

/* ====================================================================
 * Reads into a contiguous out-array
 *
 * These build the pointer array the raw API needs so call sites don't
 * hand-roll `databox *ptrs[N] = {&a, &b, ...}` wiring. `n` is the
 * number of boxes THE READ FILLS and each API fills a different count:
 *
 *   - First/Last/IteratorNext fill elementsPerEntry boxes (full width)
 *   - Lookup fills elementsPerEntry - 1 boxes (values only; the key is
 *     yours already)
 *
 * An undersized array gets written past its end (these are not partial
 * reads), so `n` is a contract, not a request. Bounded at
 * MULTIMAP_INTO_MAX; wider maps use the raw API. Zero-cost: with a
 * literal `n` the wiring loop unrolls to the same address-of stores
 * the hand-rolled initializer produced.
 * ==================================================================== */
enum { MULTIMAP_INTO_MAX = 16 };

/* n == elementsPerEntry - 1 (value elements only) */
static inline bool multimapLookupInto(const multimap *m, const databox *key,
                                      databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapLookup(m, key, ptrs);
}

/* n == elementsPerEntry (full width) */
static inline bool multimapFirstInto(multimap *m, databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapFirst(m, ptrs);
}

/* n == elementsPerEntry (full width) */
static inline bool multimapLastInto(multimap *m, databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapLast(m, ptrs);
}

/* Positional row access: keyIndex is 0-based from the head, negative
 * from the tail (-1 = last row).  Rows are located through maintained
 * anchors (sub-map row counts, then the nearest of each flex's head /
 * cached middle / tail), so the walk is over at most a quarter of the
 * owning flex.  Lookup fills FULL-width rows (key included); Delete
 * removes the EXACT addressed row. */
bool multimapDeleteByPosition(multimap **m, int64_t keyIndex);
bool multimapLookupByPosition(multimap *m, int64_t keyIndex,
                              databox *elements[]);

bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p);
bool multimapProcessPredicate(const multimapPredicate *p, const databox *value);
typedef bool(multimapElementWalker)(void *userData, const databox *elements[]);
size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData);

bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            bool forward, const databox *box);
void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          bool forward);
bool multimapIteratorNext(multimapIterator *iter, databox **elements);

/* Iterator companion to the *Into readers above: same contract, same
 * width rule (`n` == elementsPerEntry). */
static inline bool multimapIteratorNextInto(multimapIterator *iter,
                                            databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapIteratorNext(iter, ptrs);
}

void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b);

void multimapDifferenceKeys(multimap **restrict dst,
                            multimapIterator *restrict const a,
                            multimapIterator *restrict const b,
                            const bool symmetricDifference);

void multimapCopyKeys(multimap **restrict dst, const multimap *restrict src);

#ifdef DATAKIT_TEST
void multimapRepr(const multimap *m);
void multimapVerify(const multimap *m);
int multimapTest(int argc, char *argv[]);
/* Tests the ...InsertReplacing entry points of every layer (flex, Small,
 * Medium, Full, the generic multimap and its tier upgrades): a replace
 * reports the tuple it overwrote, a fresh insert reports VOID.  Returns the
 * number of failures.  Run by multimapTest and multimapFullTest. */
int32_t multimapInsertReplacingSweep(void);
/* Drives 96/128-bit keys and values through Small, Medium and Full maps
 * (insert, lookup, exists, iterate, range, positional reads, ProcessUntil,
 * the key set operations, FieldIncr, delete, trim by predicate, refill,
 * copy, and full-width rows) against an independent 128-bit oracle, with
 * 0xFF-poisoned output boxes.  Returns the number of failures.  Run by
 * multimapTest and multimapFullTest. */
int32_t multimapWideSweep(void);
#endif
