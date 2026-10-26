#pragma once

#include "databox.h"
#include "multimapCommon.h"

/* Opaque data type; no user visible data here */
typedef struct multimapFull multimapFull;
typedef void(multimapFullMapDeleter)(const flex *map, void *data);

[[nodiscard]] multimapFull *multimapFullNew(multimapElements elementsPerEntry);
[[nodiscard]] multimapFull *
multimapFullSetNew(multimapElements elementsPerEntry, uint32_t maxSize);
multimapFull *multimapFullNewFromManyGrow(void *m, flex *map[],
                                          const multimapFullMiddle middle[],
                                          size_t count,
                                          multimapElements elementsPerEntry,
                                          bool mapIsSet);
multimapFull *multimapFullNewFromManyGrowWithReference(
    void *m, flex *map[], const multimapFullMiddle middle[], size_t count,
    multimapElements elementsPerEntry, bool mapIsSet,
    const struct multimapAtom *referenceContainer);
multimapFull *multimapFullNewFromTwoGrow(void *m, flex *map[2],
                                         multimapFullMiddle middle[2],
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet);
multimapFull *multimapFullNewFromTwoGrowWithReference(
    void *m, flex *map[2], multimapFullMiddle middle[2],
    multimapElements elementsPerEntry, bool mapIsSet,
    const struct multimapAtom *referenceContainer);
multimapFull *multimapFullNewFromOneGrow(void *m, flex *one,
                                         multimapFullMiddle mid,
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet);
[[nodiscard]] multimapFull *multimapFullCopy(const multimapFull *m);
size_t multimapFullCount(const multimapFull *m);
size_t multimapFullBytes(const multimapFull *m);
size_t multimapFullNodeCount(const multimapFull *m);
size_t multimapFullBytesFull(const multimapFull *m);
flex *multimapFullDump(const multimapFull *m);

bool multimapFullInsert(multimapFull *m, const databox *elements[]);
void multimapFullInsertFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullInsertAllowExternalizeKeys(multimapFull *m,
                                            const databox *elements[],
                                            void **keyAllocation);
/* The replacing forms of the two inserts above: on a replace they report the
 * overwritten value entries through 'replacedOut' ('elementsPerEntry - 1'
 * slots in tuple order after the key, every one VOID when nothing was
 * replaced).  Integers up to 64 bits, reals and pointers are reported by
 * value and stay valid.  A BYTES or container slot points into the
 * rewritten map and must not be read.  A 96/128-bit integer does not fit a
 * databox, so its slot reports only the type (SIGNED_128 or UNSIGNED_128)
 * with a NULL payload pointer (data.i128 or data.u128); read such a value
 * before the insert if it is needed. */
bool multimapFullInsertReplacing(multimapFull *m, const databox *elements[],
                                 databox *replacedOut);
/* As multimapInsertIfAbsentEntry (multimap.h); 'me' may be NULL */
bool multimapFullInsertIfAbsent(multimapFull *m, const databox *elements[],
                                multimapEntry *me);
/* As multimapInsertWithSurrogateKeyIfAbsent (multimap.h) */
bool multimapFullInsertWithSurrogateKeyIfAbsent(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx);
bool multimapFullInsertAllowExternalizeKeysReplacing(multimapFull *m,
                                                     const databox *elements[],
                                                     void **keyAllocation,
                                                     databox *replacedOut);
void multimapFullAppend(multimapFull *m, const databox *elements[]);

bool multimapFullInsertWithSurrogateKey(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);

bool multimapFullExists(multimapFull *m, const databox *key);
bool multimapFullExistsFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullExistsWithReference(
    const multimapFull *m, const databox *key, databox *foundRef,
    const struct multimapAtom *referenceContainer);

bool multimapFullLookup(multimapFull *m, const databox *key,
                        databox *elements[]);
bool multimapFullLookupWithReference(
    multimapFull *m, const databox *key, databox *elements[],
    const struct multimapAtom *referenceContainer);
/* Select one row.  fromTail == false picks a row uniformly at random across
 * all sub-maps.  fromTail == true returns the container's last row (its
 * greatest key) on every call; it is a positional selector, not a sample.
 * Callers that want a random sample pass false.  'foundBox', when non-NULL,
 * receives elementsPerEntry non-copying boxes that reference the flex and
 * are invalidated by any mutation of the container. */
bool multimapFullRandomValue(multimapFull *m, const bool fromTail,
                             databox **foundBox, multimapEntry *me);
bool multimapFullGetUnderlyingEntry(multimapFull *m, const databox *key,
                                    multimapEntry *me);
bool multimapFullGetUnderlyingEntryWithReference(
    multimapFull *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
/* As multimapGetUnderlyingEntryFullWidth (multimap.h) */
bool multimapFullGetUnderlyingEntryFullWidth(multimapFull *m,
                                             const databox *elements[],
                                             multimapEntry *me);
void multimapFullResizeEntry(multimapFull *m, multimapEntry *me, size_t newLen);
void multimapFullReplaceEntry(multimapFull *m, multimapEntry *me,
                              const databox *box);
void multimapFullDeleteEntry(multimapFull *m, multimapEntry *me);
void multimapFullDeleteEntryWithReference(
    multimapFull *m, multimapEntry *me,
    const struct multimapAtom *referenceContainer);

bool multimapFullLookupByPosition(multimapFull *m, int64_t keyIndex,
                                  databox *elements[]);
bool multimapFullDeleteByPosition(multimapFull *m, int64_t keyIndex);

bool multimapFullRegularizeMap(multimapFull *m, multimapFullIdx mapIdx,
                               flex **map);
bool multimapFullRegularizeMapWithReference(
    multimapFull *m, multimapFullIdx mapIdx, flex **map,
    const struct multimapAtom *referenceContainer);

bool multimapFullDelete(multimapFull *m, const databox *key);
bool multimapFullDeleteReporting(multimapFull *m, const databox *key,
                                 databox *removedOut);
bool multimapFullDeleteTaking(multimapFull *m, const databox *key,
                              databox *takenOut);

/* Drop the lowest 'n' maps whole (their rows leave the count; the last map
 * empties in place, as a multimapFull keeps one), handing each to
 * 'mapIter' (may be NULL) before it goes.  True while rows remain. */
bool multimapFullDeleteNMaps(multimapFull *m, size_t n);
bool multimapFullDeleteNMapsIterate(multimapFull *m, const size_t n,
                                    multimapFullMapDeleter *mapIter,
                                    void *data);
bool multimapFullDeleteFullWidth(multimapFull *m, const databox *elements[]);
bool multimapFullDeleteFullWidthWithFound(multimapFull *m,
                                          const databox *elements[],
                                          databox *found);
bool multimapFullDeleteWithReference(
    multimapFull *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference);
bool multimapFullDeleteWithFound(multimapFull *m, const databox *key,
                                 databox *foundReference);
bool multimapFullDeleteRandomValue(multimapFull *m, const bool deleteFromTail,
                                   databox **deletedBox);

int64_t multimapFullFieldIncr(multimapFull *m, const databox *key,
                              uint32_t fieldOffset, int64_t incrBy);

void multimapFullReset(multimapFull *m);
/* Refill every inner map to the limit from the next one (see
 * multimapCompact) */
void multimapFullCompact(multimapFull *m);
void multimapFullCompactWithReference(
    multimapFull *m, const struct multimapAtom *referenceContainer);
void multimapFullFree(multimapFull *m);

bool multimapFullFirst(multimapFull *m, databox *elements[]);
bool multimapFullLast(multimapFull *m, databox *elements[]);

bool multimapFullIteratorInitAt(multimapFull *m, multimapIterator *iter,
                                bool forward, const databox *box);
bool multimapFullIteratorInit(multimapFull *m, multimapIterator *iter,
                              bool forward);
bool multimapFullIteratorNext(multimapIterator *iter, databox *elements[]);
/* See multimapIteratorNextWithSlots. */
bool multimapFullIteratorNextWithSlots(multimapIterator *iter,
                                       databox *elements[], databoxWide *slots);

bool multimapFullDeleteByPredicate(multimapFull *m, const multimapPredicate *p);

#ifdef DATAKIT_TEST
void multimapFullVerify(const multimapFull *m);
void multimapFullRepr(const multimapFull *m);
int multimapFullTest(int argc, char *argv[]);
#endif
