#pragma once

#include "databox.h"
__BEGIN_DECLS

/* Pre-declare multimapAtom existence.
 * We don't want to include multimapAtom.h here because of
 * recursive dependencies. */
struct multimapAtom;

typedef enum flexEndpoint {
    FLEX_ENDPOINT_TAIL = -1,
    FLEX_ENDPOINT_HEAD = 0
} flexEndpoint;

/* That's right, an empty 'flex' is just 2 bytes! */
#define FLEX_EMPTY_SIZE 2

typedef uint8_t flex;
typedef uint8_t flexEntry;

typedef uint8_t cflex;

flex *flexNew(void);
void flexReset(flex **ff);
flex *flexDuplicate(const flex *f);
void flexFree(flex *f);

flexEntry *flexMiddle(const flex *f, uint_fast32_t elementsPerEntry);

/* Merge */
flex *flexMerge(flex **first, flex **second);
void flexBulkAppendFlex(flex **ff, const flex *zzb);
flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count);

/* Insert to head or tail of flex */
void flexPushBytes(flex **ff, const void *s, size_t len, flexEndpoint where);
void flexPushSigned(flex **ff, int64_t i, flexEndpoint where);
void flexPushUnsigned(flex **ff, uint64_t u, flexEndpoint where);
void flexPushFloat16(flex **ff, float f, flexEndpoint where);
void flexPushFloatB16(flex **ff, float f, flexEndpoint where);
void flexPushFloat(flex **ff, float f, flexEndpoint where);
void flexPushDouble(flex **ff, double d, flexEndpoint where);
void flexPushByType(flex **ff, const databox *box, flexEndpoint where);

/* Insert at existing position 'fe' */
void flexInsertBytes(flex **ff, flexEntry *fe, const void *s, const size_t len);
void flexInsertSigned(flex **ff, flexEntry *fe, int64_t i);
void flexInsertUnsigned(flex **ff, flexEntry *fe, uint64_t u);
void flexInsertHalfFloat(flex **ff, flexEntry *fe, float f);
void flexInsertFloat(flex **ff, flexEntry *fe, float f);
void flexInsertDouble(flex **ff, flexEntry *fe, double d);
void flexInsertByType(flex **ff, flexEntry *fe, const databox *box);
bool flexInsertByTypeSortedWithMiddle(flex **ff, const databox *box,
                                      flexEntry **middleEntry);
flexEntry *flexFindByTypeSortedWithMiddleGetEntry(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE);
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool replace);
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
    flex **ff, const uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **middleEntry, bool compareUsingKeyElementOnly,
    void **recoveredPointer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **const middleEntry, bool compareUsingKeyElementOnly,
    const struct multimapAtom *referenceContainer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
    flex **ff, const uint_fast32_t elementsPerEntry, const databox **box,
    const databox *boxInsertKey, flexEntry **middleEntry,
    bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer);
bool flexInsertByTypeSortedWithMiddleMultiDirect(flex **ff,
                                                 uint_fast32_t elementsPerEntry,
                                                 const databox *box[],
                                                 flexEntry **middleEntry);
void flexResizeEntry(flex **ff, flexEntry *fe, size_t newLenForEntry);

void flexAppendMultiple(flex **ff, const uint_fast32_t elementsPerEntry,
                        const databox **const box);

flexEntry *flexFindByTypeSortedWithMiddleFullWidthWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **const compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);

flexEntry *flexFindByTypeSortedWithMiddleWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);

/* Compare entire map entries */
int flexCompareEntries(const flex *f, const databox *const *elements,
                       uint_fast32_t elementsPerEntry, int_fast32_t offset);

/* Get pointer to entry */
flexEntry *flexIndexDirect(const flex *f, int32_t index);
flexEntry *flexIndex(const flex *f, int32_t index);
flexEntry *flexNext(const flex *f, flexEntry *fe);
flexEntry *flexPrev(const flex *f, flexEntry *fe);

bool flexEntryIsValid(const flex *f, flexEntry *fe);

/* Quick endpoint retrieval */
flexEntry DK_FN_PURE *flexHead(const flex *f);
flexEntry DK_FN_PURE *flexTail(const flex *f);
flexEntry DK_FN_PURE *flexTailWithElements(const flex *const f,
                                           uint_fast32_t elementsPerEntry);
#define flexHeadOrTail(f, endpoint)                                            \
    ((endpoint) == FLEX_ENDPOINT_TAIL ? flexTail(f) : flexHead(f))
#define flexHeadOrIndex(f, endpoint)                                           \
    ((endpoint) == FLEX_ENDPOINT_HEAD ? flexHead(f) : flexIndex(f, endpoint))

/* Retrieve data */
void flexGetByType(const flexEntry *fe, databox *outbox);
void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const struct multimapAtom *referenceContainer);
void flexGetByTypeCopy(const flexEntry *const fe, databox *box);
bool flexGetNextByType(flex *f, flexEntry **fe, databox *box);

/* Retrieve specific data */
bool flexGetSigned(flexEntry *fe, int64_t *value);
bool flexGetUnsigned(flexEntry *fe, uint64_t *value);

/* Replace */
void flexReplaceByType(flex **ff, flexEntry *fe, const databox *box);
void flexReplaceBytes(flex **ff, flexEntry *fe, const void *s,
                      const size_t slen);
bool flexReplaceSigned(flex **ff, flexEntry *fe, int64_t value);
bool flexReplaceUnsigned(flex **ff, flexEntry *fe, uint64_t value);
bool flexIncrbySigned(flex **ff, flexEntry *fe, int64_t incrby,
                      int64_t *newval);
bool flexIncrbyUnsigned(flex **ff, flexEntry *fe, int64_t incrby,
                        uint64_t *newval);
/* ADD incrbyfloat */
/* ADD automatic float vs. double detection */

/* Compares */
bool flexCompareBytes(flex *fe, const void *s, size_t slen);
bool flexCompareString(flexEntry *fe, const void *sstr, size_t slen);
bool flexCompareUnsigned(flexEntry *fe, uint64_t sval);
bool flexCompareSigned(flexEntry *fe, int64_t sval);

/* Finding (head to tail) */
flexEntry *flexFind(flex *f, const void *vstr, uint32_t vlen, uint32_t skip);
flexEntry *flexFindSigned(const flex *f, flexEntry *fe, int64_t sval,
                          uint32_t skip);
flexEntry *flexFindUnsigned(const flex *f, flexEntry *fe, uint64_t sval,
                            uint32_t skip);
flexEntry *flexFindString(const flex *f, flexEntry *fe, const void *sval,
                          const size_t slen, uint32_t skip);
flexEntry *flexFindByType(flex *f, flexEntry *fe, const databox *box,
                          uint32_t skip);
flexEntry *flexFindByTypeSorted(const flex *f, uint_fast32_t nextElementOffset,
                                const databox *compareAgainst);
flexEntry *flexFindByTypeSortedFullWidth(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox **compareAgainst);
flexEntry *flexGetByTypeSortedWithMiddle(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox *compareAgainst,
                                         const flexEntry *middleP);
flexEntry *flexFindByTypeSortedWithMiddle(const flex *f,
                                          uint_fast32_t elementsPerEntry,
                                          const databox *compareAgainst,
                                          const flexEntry *middleP);
flexEntry *flexFindByTypeSortedWithMiddleFullWidth(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, const flexEntry *middleP);
flexEntry *flexFindByTypeHead(flex *f, const databox *box, uint32_t skip);

/* Finding (tail to head) */
flexEntry *flexFindSignedReverse(const flex *f, flexEntry *fe, int64_t sval,
                                 uint32_t skip);
flexEntry *flexFindUnsignedReverse(const flex *f, flexEntry *fe, uint64_t sval,
                                   uint32_t skip);
flexEntry *flexFindStringReverse(const flex *f, flexEntry *fe, const void *sval,
                                 const size_t slen, uint32_t skip);
flexEntry *flexFindByTypeReverse(flex *f, flexEntry *fe, const databox *box,
                                 uint32_t skip);

/* Metadata */
bool DK_FN_PURE flexIsEmpty(const flex *f);
size_t DK_FN_PURE flexCount(const flex *f);
size_t DK_FN_PURE flexBytes(const flex *f);
size_t DK_FN_PURE flexBytesLength(const flex *f);

/* Verify metadata and physical entries are equal in both flexes. */
bool flexEqual(const flex *a, const flex *b);

/* Deleting */
void flexDelete(flex **ff, flexEntry **fe);
void flexDeleteNoUpdateEntry(flex **ff, flexEntry *fe);
void flexDeleteDrain(flex **ff, flexEntry **fe);
void flexDeleteCount(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCount(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRange(flex **ff, int32_t index, uint32_t num);
void flexDeleteUpToInclusive(flex **ff, flexEntry *fe);
void flexDeleteUpToInclusivePlusN(flex **ff, flexEntry *fe,
                                  const int32_t nMore);
void flexDeleteSortedValueWithMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                                     flexEntry *fe, flexEntry **middleEntry);
flex *flexSplitRange(flex **ff, int32_t index, uint32_t num);
flex *flexSplitMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                      const flexEntry *middleEntry);
flex *flexSplit(flex **ff, uint_fast32_t elementsPerEntry);
#define flexDeleteHead(ff) flexDeleteNoUpdateEntry(ff, flexHead(*(ff)))
#define flexDeleteTail(ff) flexDeleteNoUpdateEntry(ff, flexTail(*(ff)))

/* Draining a flex (doesn't shrink allocation for each delete) */
void flexDeleteCountDrain(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCountDrain(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRangeDrain(flex **ff, int32_t index, uint32_t num);

/* Do math on one flex of all the same type element */
int64_t flexAddSigned(const flex *f);
uint64_t flexAddUnsigned(const flex *f);
int64_t flexSubtractSigned(const flex *f);
uint64_t flexSubtractUnsigned(const flex *f);
int64_t flexMultiplySigned(const flex *f);
uint64_t flexMultiplyUnsigned(const flex *f);
double flexAddFloat(const flex *f);
double flexSubtractFloat(const flex *f);
double flexMultiplyFloat(const flex *f);
double flexAddDouble(const flex *f);
double flexSubtractDouble(const flex *f);
double flexMultiplyDouble(const flex *f);

/* CFlex */
size_t cflexBytesCompressed(const cflex *c);
size_t cflexBytes(const cflex *c);
cflex *cflexDuplicate(const cflex *c);
bool flexConvertToCFlex(const flex *f, cflex *cBuffer, size_t cBufferLen);
bool cflexConvertToFlex(const cflex *c, flex **fBuffer, size_t *fBufferLen);

#ifdef DATAKIT_TEST
void flexRepr(const flex *f);
int32_t flexTest(int32_t argc, char *argv[]);
#endif

__END_DECLS
