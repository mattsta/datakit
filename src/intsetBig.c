#include "intsetBig.h"

#include "intersectInt.h"
#include "intsetU32.h"
#include "str.h"

struct intsetBigSet {
    multimap *i;
    size_t count;
};

/* If we limit our buckets to 2^20 (1 million) elements each,
 * their max size will be 4 MB.
 * If we don't set a limit and let them grow to max size (2^32),
 * their max size would be 16 GB each, which would be great for
 * searching but very bad for inserts and deletes. */
#define DIVISOR_WIDTH 20
_Static_assert(INTSET_BIG_INT128_MIN ==
                   -((((__int128_t)1) << (63 + DIVISOR_WIDTH)) - 1),
               "Didn't update INTSET_BIG_MIN?");
_Static_assert(INTSET_BIG_UINT128_MAX ==
                   ((((__uint128_t)1) << (64 + DIVISOR_WIDTH)) - 1),
               "Didn't update INTSET_BIG_MAX?");

static const uint64_t divisor = (1ULL << DIVISOR_WIDTH);
static const int64_t ndivisor = (1LL << DIVISOR_WIDTH);

_Static_assert(
    DIVISOR_WIDTH <= 32,
    "maximum bucket width must be <= 32 because because we are using intsetU32 "
    "for underlying storage and intsetU32 can store range [0, UINT32_MAX]");

/* We re-construct the bucket value by:
 *  - bucketIndex * divisor
 * But we need to make sure the multiplication won't overflow the math
 * storage value or else we need to use [u]int128_t math instead. */
static const uint64_t divisorOverflow = UINT64_MAX / divisor;
static const int64_t ndivisorOverflow = INT64_MAX / ndivisor;

/* The flex/multimap interface doesn't currently support 128 byte integers
 * as keys in maps */
#define USE_128_BIT_BUCKETS 0

static bool databoxToBucketAndOffset(const databoxBig *restrict box,
#if USE_128_BIT_BUCKETS
                                     __uint128_t *restrict bucket,
#else
                                     uint64_t *restrict bucket,
#endif
                                     uint32_t *restrict offset,
                                     bool *restrict isNegative) {
    switch (box->type) {
    case DATABOX_UNSIGNED_64: {
        /* Operate on given unsigned value directly. */
        const uint64_t input = box->data.u;
        *bucket = input / divisor;
        *offset = input % divisor;
        *isNegative = false;
        break;
    }
    case DATABOX_SIGNED_64: {
        /* Convert signed input to unsigned for storage and math */
        uint64_t input = box->data.i;
        if (box->data.i < 0) {
            input = DK_INT64_TO_UINT64(box->data.i);
        }

        *bucket = input / divisor;
        *offset = input % divisor;
        /* Yes this is a "double check" for negative, but we need to set
         * 'isNegative' to false when is NOT negative, so just isolate this to
         * one location instead of adding an else branch to the if above. */
        *isNegative = box->data.i < 0;
        break;
    }
    case DATABOX_UNSIGNED_128: {
        /* Operate on given unsigned value directly. */
        assert(box->big);
        const __uint128_t input = *box->data.u128;
        *bucket = input / divisor;
        *offset = input % divisor;
        *isNegative = false;
        break;
    }
    case DATABOX_SIGNED_128: {
        /* Convert signed input to unsigned for storage and math */
        assert(box->big);
        __uint128_t input = (__uint128_t)*box->data.i128;
        if (*box->data.i128 < 0) {
            input = DK_INT128_TO_UINT128(*box->data.i128);
        }

        *bucket = input / divisor;
        *offset = input % divisor;
        *isNegative = *box->data.i128 < 0;
        break;
    }
    default:
        /* Wrong intput type! */
        assert(NULL && "Wrong input?");
        return false;
    }

    return true;
}

DK_INLINE_ALWAYS void
valueFromBucketOffset(const databoxBig *restrict const bucket,
                      const uint32_t offset,
                      databoxBig *restrict const result) {
    result->type = bucket->type;

    switch (bucket->type) {
    case DATABOX_UNSIGNED_64:
        if (likely(bucket->data.u < divisorOverflow)) {
            result->data.u = (bucket->data.u * divisor) + offset;
        } else {
            DATABOX_BIG_INIT(result);
            result->type = DATABOX_UNSIGNED_128;
            *result->data.u128 =
                ((__uint128_t)bucket->data.u * divisor) + offset;
        }

        break;
    case DATABOX_SIGNED_64:
        assert(bucket->data.i < 0);
        if (likely(bucket->data.i > -ndivisorOverflow)) {
            result->data.i = -(((-bucket->data.i - 1) * ndivisor) + offset);
        } else {
            DATABOX_BIG_INIT(result);
            result->type = DATABOX_SIGNED_128;
            *result->data.i128 =
                -(((-(__int128_t)bucket->data.i - 1) * ndivisor) + offset);
        }

        break;

#if USE_128_BIT_BUCKETS
    case DATABOX_UNSIGNED_128:
        DATABOX_BIG_INIT(result);
        *result->data.u128 =
            ((__uint128_t)*bucket->data.u128 * divisor) + offset;
        break;
    case DATABOX_SIGNED_128:
        assert(*bucket->data.i128 < 0);
        DATABOX_BIG_INIT(result);
        *result->data.i128 =
            -(((-(__int128_t)*bucket->data.i128 - 1) * ndivisor) + offset);
        break;
#endif
    default:
        assert(NULL && "Improper bucket provided?");
        __builtin_unreachable();
    }
}

#define boxPtrToU32(box) (intsetU32 *)(box.data.ptr)
#define boxPPtrToU32(box) (intsetU32 **)(&box.data.ptr)

intsetBig *intsetBigNew(void) {
    /* Create map of bucketIndex -> intsetU32 */
    intsetBig *isb = zmalloc(sizeof(*isb));
    isb->i = multimapSetNew(2);
    return isb;
}

void intsetBigFree(intsetBig *isb) {
    if (isb) {
        if (isb->i) {
            multimapIterator iter;
            multimapIteratorInit(isb->i, &iter, true);

            databoxBig bucket;
            databox vptr;
            databox *elements[] = {(databox *)&bucket, &vptr};
            while (multimapIteratorNext(&iter, elements)) {
                intsetU32Free(boxPtrToU32(vptr));
            }

            multimapFree(isb->i);
        }

        zfree(isb);
    }
}

intsetBig *intsetBigCopy(const intsetBig *const isb) {
    /* Copy 'isb' for the bucket->intsetU32 mappings,
     * then copy each intsetU32 from the source into
     * the matching bucket in the result. */
    intsetBig *result = zmalloc(sizeof(*result));
    result->count = isb->count;
    result->i = multimapCopy(isb->i);

    multimapIterator iter;
    multimapIteratorInit(result->i, &iter, true);

    databoxBig bucket;
    databox vptr;
    databox *elements[] = {(databox *)&bucket, &vptr};
    while (multimapIteratorNext(&iter, elements)) {
        vptr.data.ptr = intsetU32Copy(boxPtrToU32(vptr));
        multimapInsert(&result->i, (const databox **)elements);
    }

    return result;
}

size_t intsetBigCountBuckets(const intsetBig *isb) {
    return multimapCount(isb->i);
}

size_t intsetBigCountElements(const intsetBig *isb) {
    /* Steps:
     *  - Iterate over multimap
     *    - for each bucket, get pointer to intsetU32
     *    - accumulate intsetU32 counts
     */
    return isb->count;
}

size_t intsetBigBytes(const intsetBig *isb) {
    /* Steps:
     *  - Iterate over multimap
     *    - for each bucket, get pointer to intsetU32
     *    - accumulate intsetU32 sizes
     */
    size_t totalBytes = multimapBytes(isb->i) + sizeof(*isb);
    multimapIterator iter;
    multimapIteratorInit(isb->i, &iter, true);

    databoxBig bucket;
    databox vptr;
    databox *elements[] = {(databox *)&bucket, &vptr};
    while (multimapIteratorNext(&iter, elements)) {
        totalBytes += intsetU32Bytes(boxPtrToU32(vptr));
    }

    return totalBytes;
}

DK_INLINE_ALWAYS bool setFromInput(const intsetBig *isb, const databoxBig *val,
                                   uint32_t *offsetIn, bool *isNegative,
                                   databoxBig *lookup, databox *found) {
#if USE_128_BIT_BUCKETS
    __uint128_t bucket;
#else
    uint64_t bucket;
#endif

    /* Determine base {{divisor}} leading digit/bucket and offset */
    if (!databoxToBucketAndOffset(val, &bucket, offsetIn, isNegative)) {
        /* Failed to convert 'val' to a bucket and offset */
        return false;
    }

#if !USE_128_BIT_BUCKETS
    assert(bucket <= UINT64_MAX);
#endif

    if (bucket <= UINT64_MAX) {
        /* Lookup bucket in isb */
        /* Initialize 'lookup' with the common case */
        *lookup = (databoxBig){.type = DATABOX_UNSIGNED_64, .data.u = bucket};

        const bool localNegative = *isNegative;
        assert(localNegative == 0 || localNegative == 1);

        /* But if negative, re-init lookup */
        if (localNegative) {
            lookup->type = DATABOX_SIGNED_64;

            /* For negative buckets, the buckets are -1 incremented because
             * we don't have "signed zero" to designate zero-div buckets for
             * the negative zone (e.g. "1" goes in bucket 0 offset 1, but "-1"
             * goes in bucket "-1" offset "1"). As a consequence, this operation
             * must be undone and re-accounted-for when doing retrievals and
             * removals as well. */
            /* originally: -((int64_t)bucket + 1) */
            lookup->data.i = bucket + 1;

            /* We must make 'data.i' negative if it isn't already (which only
             * happens by a side effect of "too big math" on 'bucket' when
             * 'bucket' is at its integer storage limit. */
            /* this is a weird way to handle our wraparound edge case of having
             * bucket be INT64_MAX where it turns negative by itself after
             * addition and we don't want to un-negate it. */
            if (lookup->data.i > 0) {
                lookup->data.i *= -1;
            }
        }
    } else {
#if !USE_128_BIT_BUCKETS
        assert(NULL);
#endif
        /* Lookup bucket in isb */
        /* Initialize 'lookup' with the common case */
        *lookup = (databoxBig){.type = DATABOX_UNSIGNED_128, .data.u = bucket};
        DATABOX_BIG_INIT(lookup);
        lookup->type = DATABOX_UNSIGNED_128;
        *lookup->data.u128 = bucket;

        /* But if negative, re-init lookup */
        if (*isNegative) {
            lookup->type = DATABOX_UNSIGNED_128;
            *lookup->data.i128 = -((__int128_t)bucket + 1);
        }
    }

    databox *ptr[] = {found};
    if (multimapLookup(isb->i, (databox *)lookup, ptr)) {
        return true;
    }

    return false;
}

/* TODO: write multi-add function so if we are, for example, adding a million
 * elements (sorted!) we can just add/merge in chunks so we don't need to run
 * the multimapLookup() inside setFromInput() a million times for adding. */
bool intsetBigAdd(intsetBig *isb, const databoxBig *val) {
    databoxBig lookup;
    databox found;
    uint32_t offset;
    intsetU32 *orig = NULL;
    bool added;
    bool isNegative;

    /* Create lookup/storage criteria and attempt to fetch a set */
    if (setFromInput(isb, val, &offset, &isNegative, &lookup, &found)) {
        orig = boxPtrToU32(found);
        added = intsetU32Add(boxPPtrToU32(found), offset);
    } else {
        /* Existing bucket not found, so create a new set and add it */
        found.type = DATABOX_PTR;
        found.data.ptr = intsetU32NewLen(1);
        added = intsetU32Add(boxPPtrToU32(found), offset);

        /* 'added' must be true here because we're inserting into empty set */
        assert(added);
    }

#if !USE_128_BIT_BUCKETS
    assert(!lookup.big);
#endif

    /* Add intsetU32 back to isb for bucket */
    if (orig != boxPtrToU32(found)) {
        /* Need to update multimap bucket again since the set pointer changed */
        const databox *insert[] = {(databox *)&lookup, &found};
        multimapInsert(&isb->i, insert);

#ifndef NDEBUG
        databox *check[1] = {&found};
        const bool foundM = multimapLookup(isb->i, (databox *)&lookup, check);
        assert(foundM);
        assert(found.type == DATABOX_UNSIGNED_64);
#endif
    }

    /* At this point we've encountered no errors and did something. */
    if (added) {
        isb->count++;
    }

    return added;
}

bool intsetBigRemove(intsetBig *isb, const databoxBig *val) {
    /* Steps:
     *  - lookup
     *    - if bucket exists, remove against bucket
     *      - if set now empty, free and delete bucket
     */

    databoxBig lookup;
    databox found;
    uint32_t offset;
    intsetU32 *orig = NULL;
    bool removed = false;
    bool isNegative;

    /* Create lookup/storage criteria and attempt to fetch a set */
    if (setFromInput(isb, val, &offset, &isNegative, &lookup, &found)) {
        orig = boxPtrToU32(found);
        removed = intsetU32Remove(boxPPtrToU32(found), offset);
        if (removed) {
            isb->count--;

            if (intsetU32Count(boxPtrToU32(found)) == 0) {
                /* no more elements, free this and delete from map */
                intsetU32Free(boxPtrToU32(found));
                multimapDelete(&isb->i, (databox *)&lookup);
            } else {
                /* removal caused intsetU32 to move, so we need to update map */
                if (orig != boxPtrToU32(found)) {
                    const databox *insert[] = {(databox *)&lookup, &found};
                    multimapInsert(&isb->i, insert);
                }
            }
        }
    }

    return removed;
}

bool intsetBigExists(const intsetBig *isb, const databoxBig *val) {
    databoxBig lookup;
    databox found;
    uint32_t offset;
    bool exists = false;
    bool isNegative;

    /* Create lookup/storage criteria and attempt to fetch a set */
    if (setFromInput(isb, val, &offset, &isNegative, &lookup, &found)) {
        exists = intsetU32Exists(boxPtrToU32(found), offset);
    }

    return exists;
}

void intsetBigIteratorInit(const intsetBig *const isb,
                           intsetBigIterator *const isbIter) {
#if USE_128_BIT_BUCKETS
    DATABOX_BIG_INIT(&isbIter->bucket.box);
#endif

    multimapIteratorInit(isb->i, &isbIter->iter, true);
    isbIter->forward = true;
    isbIter->elements.position = 0;
    isbIter->elements.count = 0;

    if (!isb->count) {
        /* Only fully initialize if we have elements, otherwise, with zero
         * elements, we have nothing to iterate and need to fail the first
         * Next() call. */

        /* Fails existing check, causing check of next multimap. */
        isbIter->elements.position = SSIZE_MAX;

        /* Fails multimap iteration, resulting in zero values iterated. */
        isbIter->iter.entry = NULL;
    }
}

bool intsetBigIteratorNextBox(intsetBigIterator *restrict isbIter,
                              databoxBig *restrict val) {
    /* Check if we are resuming iterating from a previous map  */
    /* If iterating forward, position grows towards element count.
     * If iterating backward, position is decremented towards zero. */
    if ((isbIter->forward &&
         (isbIter->elements.position < isbIter->elements.count)) ||
        ((!isbIter->forward && (isbIter->elements.position >= 0)))) {
        /* Just return 'bucket' * 'offset' */
        const uint32_t useOffset =
            isbIter->elements.array[isbIter->elements.position];

        valueFromBucketOffset(&isbIter->bucket.box, useOffset, val);

        /* Forward grow bigger, backward shrink smaller */
        if (isbIter->forward) {
            isbIter->elements.position++;
        } else {
            isbIter->elements.position--;
        }

        return true;
    }

    /* else, we need to fetch a new map and start iterating again */
    databox vptr = {{0}};
    databox *elements[] = {(databox *)&isbIter->bucket.box, &vptr};
    if (multimapIteratorNext(&isbIter->iter, elements)) {
        /* reset initial conditions for value retrieval */
        intsetU32 *is = boxPtrToU32(vptr);
        isbIter->elements.count = intsetU32Count(is);
        isbIter->elements.array = intsetU32Array(is);

        /* We must always have elements.
         * An empty intsetU32 should have been deleted when becoming empty. */
        assert(isbIter->elements.count > 0);

        /* Negative boxes iterate from largest index to smallest index */
        if ((isbIter->bucket.box.type == DATABOX_UNSIGNED_64) ||
            (isbIter->bucket.box.type == DATABOX_UNSIGNED_128)) {
            isbIter->forward = true;
            isbIter->elements.position = 0;
        } else {
            isbIter->forward = false;
            isbIter->elements.position = isbIter->elements.count - 1;
        }

        /* run a new value retrieval */
        return intsetBigIteratorNextBox(isbIter, val);
    }

    /* No more buckets, terminate iteration */
    return false;
}

bool intsetBigIteratorNextBucket(intsetBigIterator *restrict isbIter,
                                 databoxBig *restrict bucket,
                                 databox *restrict val) {
    /* else, we need to fetch a new map and start iterating again */
    databox *elements[] = {(databox *)bucket, val};
    if (multimapIteratorNext(&isbIter->iter, elements)) {
        return true;
    }

    /* No more buckets, terminate iteration */
    return false;
}

bool intsetBigEqual(const intsetBig *const a, const intsetBig *const b) {
    if (a == b) {
        return true;
    }

    if (intsetBigCountBuckets(a) == intsetBigCountBuckets(b)) {
        intsetBigIterator isbiA;
        intsetBigIterator isbiB;
        databoxBig bucketA;
        databoxBig bucketB;
        databox bA;
        databox bB;

        intsetBigIteratorInit(a, &isbiA);
        intsetBigIteratorInit(b, &isbiB);

        while (intsetBigIteratorNextBucket(&isbiA, &bucketA, &bA) &&
               intsetBigIteratorNextBucket(&isbiB, &bucketB, &bB)) {
            /* If buckets don't match, sets can't be equal, so bail early. */
            if (databoxCompare((databox *)&bucketA, (databox *)&bucketB) != 0) {
                return false;
            }

            /* Buckets *are* equal, so do a quick memcmp() of intsetU32. */
            intsetU32 *iA = boxPtrToU32(bA);
            intsetU32 *iB = boxPtrToU32(bB);

            /* Hopefully clang will inline intsetU32Equal(), but the first
             * check inside intsetU32Equal() is for (a == b), but we know
             * iA != iB here, so maybe clang can optimize away the initial
             * (iA == iB) branch at the start of intsetU32Equal() */
            __builtin_assume(iA != iB);

            if (!intsetU32Equal(iA, iB)) {
                return false;
            }
        }

        /* All looping compares passed, so 'a' and 'b' have equal elements! */
        return true;
    }

    return false;
}

bool intsetBigSubset(const intsetBig *const a, const intsetBig *const b) {
    if (a == b) {
        return true;
    }

    intsetBigIterator isbIter;
    databoxBig val;

    intsetBigIteratorInit(a, &isbIter);
    while (intsetBigIteratorNextBox(&isbIter, &val)) {
        if (!intsetBigExists(b, &val)) {
            return false;
        }
    }

    return true;
}

bool intsetBigAddByBucketDirectOverwriteBulk(intsetBig *isb, databoxBig *bucket,
                                             intsetU32 *iu32) {
    /* NOTE: These MODIFY isb->i and requires INVALIDATING any iterator you
     *       currently have open! */
    if (intsetU32Count(iu32) > 0) {
        databox vptr = {.type = DATABOX_PTR, .data.ptr = iu32};
        const databox *insert[2] = {(databox *)bucket, &vptr};
        multimapInsert(&isb->i, insert);

        return true;
    }

    /* if count is zero, just delete bucket and release intsetU32. */
    multimapDelete(&isb->i, (databox *)bucket);
    intsetU32Free(iu32);

    return false;
}

bool intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
    intsetBig *isb, databoxBig *bucket, intsetU32 *iu32,
    multimapIterator *mmIter) {
    const bool didIt =
        intsetBigAddByBucketDirectOverwriteBulk(isb, bucket, iu32);

    /* Re-init iterator for 'isb' because the multimap changed. */
    multimapIteratorInitAt(isb->i, mmIter, true, (databox *)bucket);

    return didIt;
}

size_t intsetBigIntersect(const intsetBig *a, const intsetBig *b,
                          intsetBig *result) {
    size_t intersectCount = 0;

    /* First, get common buckets between 'a' and 'b' */
    multimapIterator ia;
    multimapIterator ib;
    databoxBig key[2];
    databox val[2];
    databox *ea[2] = {(databox *)&key[0], &val[0]};
    databox *eb[2] = {(databox *)&key[1], &val[1]};

    multimapIteratorInit(a->i, &ia, true);
    multimapIteratorInit(b->i, &ib, true);
    bool foundA = multimapIteratorNext(&ia, ea);
    bool foundB = multimapIteratorNext(&ib, eb);

    /* Now look up each common bucket in 'a' and 'b' and retrieve
     * both underlying intsetU32 pointers. */
    /* element-by-element zipper algoirthm for intersecting two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared < 0) {
            foundA = multimapIteratorNext(&ia, ea);
        } else if (compared > 0) {
            foundB = multimapIteratorNext(&ib, eb);
        } else {
            /* Keys compare equal, so add key to result map */

            /* Get intsetU32 for each bucket */
            intsetU32 *eaai = val[0].data.ptr;
            intsetU32 *ebbi = val[1].data.ptr;

            /* Count each intsetU32 */
            const size_t eaaiCount = intsetU32Count(eaai);
            const size_t ebbiCount = intsetU32Count(ebbi);

            /* Allocate maximum intersect size possible */
            const size_t smallest =
                eaaiCount < ebbiCount ? eaaiCount : ebbiCount;
            intsetU32 *tmpU32 = intsetU32NewLen(smallest);

            /* Run the intersection with storage directly into 'tmpU32' */
            const size_t intersectedCount = intersectIntAuto(
                intsetU32Array(eaai), eaaiCount, intsetU32Array(ebbi),
                ebbiCount, intsetU32Array(tmpU32));

            /* Resize and update metdata of 'tmpU32' */
            intsetU32UpdateCount(tmpU32, intersectedCount);
#if 1
            intsetU32ShrinkToSize(&tmpU32);
#endif

            /* Increment total incremented count for return value */
            intersectCount += intersectedCount;

            /* Add intersected values to result intsetBig */
            if (a == result) {
                /* If first input equals result, free value from first input
                 * because we're about to overwrite it for the result. */
                intsetU32Free(eaai);
                result->count -= eaaiCount;
            }

            result->count += intersectedCount;
            intsetBigAddByBucketDirectOverwriteBulk(result, &key[0], tmpU32);
            if (a == result) {
                /* Re-create (and advance) iterator because Overwrite changed
                 * underlying multimap */
                assert(a->i == result->i);

                multimapIteratorInitAt(a->i, &ia, true, (databox *)&key[0]);
                multimapIteratorNext(&ia, ea);
            }

            /* Continue finding next matching buckets. */
            foundA = multimapIteratorNext(&ia, ea);
            foundB = multimapIteratorNext(&ib, eb);
        }
    }

    /* Return count of intersected elements */
    return intersectCount;
}

/* Merge 'b' into 'a' */
void intsetBigMergeInto(intsetBig *a, const intsetBig *b) {
    multimapIterator ia;
    multimapIterator ib;
    databoxBig key[2];
    databox val[2];
    databox *ea[2] = {(databox *)&key[0], &val[0]};
    databox *eb[2] = {(databox *)&key[1], &val[1]};

    multimapIteratorInit(a->i, &ia, true);
    multimapIteratorInit(b->i, &ib, true);
    bool foundA = multimapIteratorNext(&ia, ea);
    bool foundB = multimapIteratorNext(&ib, eb);

    /* Now look up each common bucket in 'a' and 'b' and retrieve
     * both underlying intsetU32 pointers. */
    /* element-by-element zipper algoirthm for intersecting two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        intsetU32 *eaai = val[0].data.ptr;
        intsetU32 *ebbi = val[1].data.ptr;
        if (compared < 0) {
            /* No match! add B to A! */
            intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
                a, &key[0], intsetU32Copy(ebbi), &ia);
            a->count += intsetU32Count(ebbi);
            foundA = multimapIteratorNext(&ia, ea);
        } else if (compared > 0) {
            /* No match! Add B to A! */
            intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
                a, &key[0], intsetU32Copy(ebbi), &ia);
            a->count += intsetU32Count(ebbi);
            foundB = multimapIteratorNext(&ib, eb);
        } else {
            /* Match! _Merge_ keys from B into A! */

            /* Track original 'eaai' pointer value */
            const intsetU32 *orig = eaai;

            /* Perform merge of b[bucket] into a[bucket] */
            a->count += intsetU32Merge(&eaai, ebbi);

            /* If 'eaai' moved, we need to update the result map */
            if (orig != eaai) {
                intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
                    a, &key[0], eaai, &ia);
            }

            /* Continue merging next buckets */
            foundA = multimapIteratorNext(&ia, ea);
            foundB = multimapIteratorNext(&ib, eb);
        }
    }
}

bool intsetBigRandom(const intsetBig *isb, databoxBig *val) {
    /* Failure method: */
    /* Step 1: get random bucket
     * Step 2: get random element inside bucket */
    /* Using the above will improperly weight smaller buckets with higher
     * returns because we would have equal picks between buckets without
     * considering how many elements they contain. */

    /* Successful method: */
    /* Step 1: pick random position index from count of all elements
     * Step 2: return value for random position. */
    /* TODO: if this random() is too slow, we could augment with
     *       xorshift128plus() */
    const uint64_t bigRandom = ((uint64_t)random() << 32) | random();
    const uint64_t selectedIndex = bigRandom % isb->count;

    /* Set up initial conditions */
    intsetBigIterator isbIter;
    databoxBig bucket;
    databox pval;
    uint64_t currentCount = 0;

    intsetBigIteratorInit(isb, &isbIter);
    while (intsetBigIteratorNextBucket(&isbIter, &bucket, &pval)) {
        const intsetU32 *use = boxPtrToU32(pval);
        const size_t localCount = intsetU32Count(use);
        currentCount += localCount;

        if (currentCount >= selectedIndex) {
            /* random element is INSIDE THE HOUSE */
            const ssize_t useOffset = currentCount - selectedIndex - 1;
            assert(useOffset >= 0);

            uint32_t localIVAL;
            const bool got = intsetU32Get(use, useOffset, &localIVAL);
            (void)got;
            assert(got);

            valueFromBucketOffset(&bucket, localIVAL, val);
            return true;
        }
    }

    return false;
}

bool intsetBigRandomDelete(intsetBig *isb, databoxBig *deleted) {
    /* We are basically doing a double fetch of the bucket and intsetU32 here
     * (once for Random() and once for Remove()), but it's okay for now (not
     * perf tested though). */

    intsetBigRandom(isb, deleted);
    /* TODO: we could improve the performance here by making 'delete' a special
     * case of intsetBigRandom() itself since intsetBigRandom() already has the
     * bucket and intsetU32 values directly when it creates the return value
     * 'deleted' here. */
    return intsetBigRemove(isb, deleted);
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "perf.h"
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)

void intsetBigRepr(const intsetBig *isb) {
    if (isb) {
        if (isb->i) {
            multimapIterator iter;
            multimapIteratorInit(isb->i, &iter, true);

            databoxBig bucket;
            databox vptr;
            DATABOX_BIG_INIT(&bucket);
            databox *elements[] = {(databox *)&bucket, &vptr};
            while (multimapIteratorNext(&iter, elements)) {
                databoxReprSay("Bucket", (databox *)&bucket);
                intsetU32Repr(boxPtrToU32(vptr));
            }
        }
    }
}

__attribute__((optnone)) int intsetBigTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* Create and free empty set */
    TEST("create and free") {
        intsetBig *isb = intsetBigNew();
        intsetBigFree(isb);
    }

    /* Add small (bucket 0) to new set */
    TEST("add to bucket 0") {
        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_UNSIGNED(25))) {
            ERRR("Failed to add to bucket 0!");
        }

        intsetBigFree(isb);
    }

    /* Add small (bucket -1) to new set */
    TEST("add to bucket -1") {
        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_SIGNED(-25))) {
            ERRR("Failed to add to bucket -1!");
        }

        intsetBigFree(isb);
    }

    /* Add bigger to new set (bucket 1) */
    TEST("add to bucket 1") {
        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_UNSIGNED((1ULL << 35)))) {
            ERRR("Failed to add to bucket 1!");
        }

        intsetBigFree(isb);
    }

    /* Add bigger to new set (bucket -2) */
    TEST("add to bucket -2") {
        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_SIGNED(-(1LL << 35)))) {
            ERRR("Failed to add to bucket -2!");
        }

        intsetBigFree(isb);
    }

    TEST("set and retrieve smallest") {
#if 0
        /* This is -2^127 which we don't support yet */
        const char *v = "-170141183460469231731687303715884105728";
#else
        /* This is -(2^(63 + 20) - 1) which we currently support */
        const char *v = "-9671406556917033397649407";
#endif
        const size_t vlen = strlen(v);
        databox sm;
        databoxBig smb;
        databoxBig *final;
        const bool converted =
            StrScanScanReliableConvert128(v, vlen, &sm, &smb, &final);
        assert(converted);
        assert(final->type);
        assert(*final->data.i128 == INTSET_BIG_INT128_MIN);

        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, final)) {
            ERRR("Failed to add smallest?");
        }

        intsetBigIterator isbIter;
        intsetBigIteratorInit(isb, &isbIter);
        databoxBig result;
        for (int ii = 0; intsetBigIteratorNextBox(&isbIter, &result); ii++) {
            if (databoxCompare((databox *) final, (databox *)&result) != 0) {
                ERR("Stored value != original at iteration %d!\n", ii);
                databoxReprSay("Original:", (databox *) final);
                databoxReprSay("  Result:", (databox *)&result);
                printf("==========================\n");
                intsetBigRepr(isb);
            }
        }

        intsetBigFree(isb);
    }

    TEST("set and retrieve biggest") {
#if 0
        /* This is 2^128 - 1 which we currently don't support */
        const char *v = "340282366920938463463374607431768211455";
#else
        /* This is 2^(64 + 20) - 1 which we currently support */
        const char *v = "19342813113834066795298815";
#endif
        const size_t vlen = strlen(v);
        databox sm;
        databoxBig smb;
        databoxBig *final;
        const bool converted =
            StrScanScanReliableConvert128(v, vlen, &sm, &smb, &final);
        assert(converted);
        assert(final->type);
        assert(*final->data.u128 == INTSET_BIG_UINT128_MAX);

        intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, final)) {
            ERRR("Failed to add biggest?");
        }

        intsetBigIterator isbIter;
        intsetBigIteratorInit(isb, &isbIter);
        databoxBig result;
        for (int ii = 0; intsetBigIteratorNextBox(&isbIter, &result); ii++) {
            if (databoxCompare((databox *) final, (databox *)&result) != 0) {
                ERR("Stored value != original at iteration %d!\n", ii);
                databoxReprSay("Original:", (databox *) final);
                databoxReprSay("  Result:", (databox *)&result);
                printf("==========================\n");
                intsetBigRepr(isb);
            }
        }

        intsetBigFree(isb);
    }

    TEST("add from bucket -62 to 63") {
        intsetBig *isb = intsetBigNew();

        /* + 1 + 1 is for the two ±2^74 strings */
        databoxBig testers[(62 + 64) + 1 + 1] = {{{0}}};

        /* This is -(2^74 + 332197) and goes first because our
         * tests are sorted from smallest to largest inputs
         * (and also because iteration happens in sorted order
         *  from smallest to largest inputs, so for proper validation,
         *  we test the testers[0] order against the retrieved iteration
         *  order to verify everything we requested actually can be
         *  retrieved and discovered when searching) */
        const char *unlarge = "-18889465931478581186981";

#define quickBufToInt(a)                                                       \
    ({                                                                         \
        __int128_t aVal;                                                       \
        assert(StrBufToInt128(a, strlen(a), &aVal));                           \
        aVal;                                                                  \
    })

        /* Perform a quick speed test of string to __int128_t conversion. */
        {
            assert(0 == quickBufToInt("0"));
            assert(10 == quickBufToInt("10"));
            assert(INT64_MAX == quickBufToInt("9223372036854775807"));
            assert(UINT64_MAX == quickBufToInt("18446744073709551615"));

            TIME_INIT;
            size_t count = 2650000;
            __int128_t iVal;
            for (size_t i = 0; i < count; i++) {
                StrBufToInt128(unlarge, strlen(unlarge), &iVal);
            }
            TIME_FINISH(count, "StrBufToInt128");
        }

        __int128_t bigI;
        StrBufToInt128(unlarge, strlen(unlarge), &bigI);
        DATABOX_BIG_SIGNED_128(&testers[0], bigI);

        /* Prepare databoxes to use for setting and retrieval */
        /* Test setting from -((2^62) + ish) up to 2^63 + ish */
        for (int i = -62; i < 64; i++) {
            databoxBig *use = &testers[i + 62 + 1]; /* + 1 because testers[0] */
            if (i < 0) {
                const int64_t iuse = (1LL << -i) + i;
                *use = DATABOX_BIG_SIGNED(-iuse);
            } else {
                const uint64_t uuse = (1ULL << i) + i;
                *use = DATABOX_BIG_UNSIGNED(uuse);
            }
        }

        /* This is 2^74 + 332197 */
        const char *large = "18889465931478581186981";
        {
            TIME_INIT;
            size_t count = 2650000;
            __uint128_t val;
            for (size_t i = 0; i < count; i++) {
                StrBufToUInt128(large, strlen(large), &val);
            }
            TIME_FINISH(count, "StrBufToUInt128");
        }
        {
            __uint128_t val;
            StrBufToUInt128(large, strlen(large), &val);
            assert(strlen(large) == StrDigitCountUInt128(val));
            uint8_t buf[40];
            size_t count = 2650000;
            TIME_INIT;
            for (size_t i = 0; i < count; i++) {
                const size_t written = StrUInt128ToBuf(buf, sizeof(buf), val);
#if 1
                assert(written == strlen(large));
                if (memcmp(large, buf, written)) {
                    ERR("Expected %s but got %.*s!", large, (int)written, buf);
                    assert(NULL);
                }
#endif
            }
            TIME_FINISH(count, "StrUInt128ToBuf");
        }
        {
            uint64_t val = 1;
            uint8_t buf[40];
            size_t count = 2650000;
            TIME_INIT;
            for (size_t i = 0; i < count; i++, val *= 10) {
                const size_t written = StrUInt64ToBuf(buf, sizeof(buf), val);
                if (val > UINT64_MAX / 100) {
                    val = 10;
                }
                (void)written;
            }
            TIME_FINISH(count, "StrUInt64ToBuf");
        }

        /* + 1 below because testers[0] was manually set too */
        __uint128_t bigU;
        StrBufToUInt128(large, strlen(large), &bigU);
        DATABOX_BIG_UNSIGNED_128(&testers[(62 + 64) + 1], bigU);

        /* Add elements to set */
        for (size_t i = 0; i < COUNT_ARRAY(testers); i++) {
            databoxBig *use = &testers[i];

#if 0
            databoxReprSay("Storing", use);
#endif
            if (!intsetBigAdd(isb, use)) {
                ERR("Failed to add to bucket %zu!\n", i);
            }
        }

        /* Now check if added elements can be found again */
        for (size_t i = 0; i < COUNT_ARRAY(testers); i++) {
            databoxBig *use = &testers[i];

            if (!intsetBigExists(isb, use)) {
                ERR("Failed to find value for iteration %zu!", i);
            }
        }

        intsetBigIterator isbIter;
        intsetBigIteratorInit(isb, &isbIter);
        databoxBig result;
        for (int ii = 0; intsetBigIteratorNextBox(&isbIter, &result); ii++) {
            if (databoxCompare((databox *)&testers[ii], (databox *)&result) !=
                0) {
                ERR("Stored value != original at iteration %d!\n", ii);
                databoxReprSay("Original:", (databox *)&testers[ii]);
                databoxReprSay("  Result:", (databox *)&result);
                printf("==========================\n");
            }
        }

        /* Now remove added elements */
        for (size_t i = 0; i < COUNT_ARRAY(testers); i++) {
            databoxBig *use = &testers[i];

            if (!intsetBigRemove(isb, use)) {
                ERR("Failed to remove value for iteration %zu!", i);
            }
        }

        if (intsetBigCountBuckets(isb)) {
            ERRR("Has buckets?");
        }

        if (intsetBigCountElements(isb)) {
            ERRR("Has elements?");
        }

        intsetBigFree(isb);
    }

    TEST_FINAL_RESULT;
}
#endif
