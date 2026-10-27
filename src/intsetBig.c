#include "intsetBig.h"

#include "cleaner.h"
#include "dkRandom.h"
#include "intersectInt.h"
#include "intsetU32.h"
#include "str.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(intsetBig *, intsetBigFree);
defcleaner(flex *, flexFree);

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
static_assert(INTSET_BIG_INT128_MIN ==
                  -((((__int128_t)1) << (63 + DIVISOR_WIDTH)) - 1),
              "Didn't update INTSET_BIG_MIN?");
static_assert(INTSET_BIG_UINT128_MAX ==
                  ((((__uint128_t)1) << (64 + DIVISOR_WIDTH)) - 1),
              "Didn't update INTSET_BIG_MAX?");

static constexpr uint64_t divisor = (1ULL << DIVISOR_WIDTH);
static constexpr int64_t ndivisor = (1LL << DIVISOR_WIDTH);

static_assert(
    DIVISOR_WIDTH <= 32,
    "maximum bucket width must be <= 32 because because we are using intsetU32 "
    "for underlying storage and intsetU32 can store range [0, UINT32_MAX]");

/* We re-construct the bucket value by:
 *  - bucketIndex * divisor
 * But we need to make sure the multiplication won't overflow the math
 * storage value or else we need to use [u]int128_t math instead. */
static constexpr uint64_t divisorOverflow = UINT64_MAX / divisor;
static constexpr int64_t ndivisorOverflow = INT64_MAX / ndivisor;

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
        /* Not an integer: it has no bucket */
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
        assert(false && "Improper bucket provided?");
        unreachable();
    }
}

#define boxPtrToU32(box) (intsetU32 *)(box.data.ptr)

intsetBig *intsetBigNew(void) {
    /* Create map of bucketIndex -> intsetU32 */
    /* @own: caller; free=intsetBigFree */
    intsetBig *isb = zmalloc(sizeof(*isb));
    /* Use multimapNew (not multimapSetNew) because we need key-only comparison
     * for insert/replace operations. multimapSetNew compares all elements
     * (key+value), so when the intsetU32 pointer changes after resize, the
     * replace operation fails to find the existing entry. */
    /* @own: container; free=multimapFree */
    isb->i = multimapNew(2);
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
    /* Build the result fresh by walking the (never mutated) SOURCE
     * map: every bucket key is numeric (held by value) and every
     * intsetU32 is deep-copied straight into the result, so no
     * iterate-while-mutate aliasing and no transient state where the
     * copy shares live intsetU32 pointers with the source. */
    /* @own: caller; free=intsetBigFree */
    intsetBig *result = zmalloc(sizeof(*result));
    result->count = isb->count;
    /* @own: container; free=multimapFree */
    result->i = multimapNew(2);

    multimapIterator iter;
    multimapIteratorInit(isb->i, &iter, true);

    databoxBig bucket;
    databox vptr;
    databox *elements[] = {(databox *)&bucket, &vptr};
    while (multimapIteratorNext(&iter, elements)) {
        /* @own: container; free=intsetU32Free */
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

/* The bucket key 'val' is stored under (in 'lookup') and its offset in
 * that bucket */
DK_INLINE_ALWAYS bool bucketKeyFromInput(const databoxBig *val,
                                         uint32_t *offsetIn, bool *isNegative,
                                         databoxBig *lookup) {
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
        assert(false);
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

    return true;
}

DK_INLINE_ALWAYS bool setFromInput(const intsetBig *isb, const databoxBig *val,
                                   uint32_t *offsetIn, bool *isNegative,
                                   databoxBig *lookup, databox *found) {
    if (!bucketKeyFromInput(val, offsetIn, isNegative, lookup)) {
        return false;
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
    intsetU32 *orig = nullptr;
    bool added;
    bool isNegative;

    /* The bucket key and offset; a value that is not an integer has none
     * and is refused */
    if (!bucketKeyFromInput(val, &offset, &isNegative, &lookup)) {
        return false;
    }

    /* Fetch the bucket's set, or create it below */
    databox *ptr[] = {&found};
    if (multimapLookup(isb->i, (databox *)&lookup, ptr)) {
        orig = boxPtrToU32(found);
        added = intsetU32Add(boxPtrToU32(found), offset);
    } else {
        /* Existing bucket not found, so create a new set and add it */
        found.type = DATABOX_PTR;
        /* @own: container; free=intsetU32Free */
        found.data.ptr = intsetU32NewLen(1);
        added = intsetU32Add(boxPtrToU32(found), offset);

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
    }

    /* At this point we've encountered no errors and did something. */
    if (added) {
        isb->count++;
    }

    return added;
}

bool intsetBigCanHold(const databoxBig *v) {
    switch (v->type) {
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
        return true;
    case DATABOX_SIGNED_128:
        return INTSET_BIG_RANGECHECK_INT128(*v->data.i128);
    case DATABOX_UNSIGNED_128:
        /* Unsigned: only the upper bound can be crossed */
        return *v->data.u128 <= INTSET_BIG_UINT128_MAX;
    default:
        return false;
    }
}

/* Free the bucket sets a [key, set] row flex points at, then the flex */
static void intsetBigFreeRows_(flex *rows) {
    const size_t n = flexCount(rows);
    flexEntry *fe = flexHead(rows);
    for (size_t i = 0; i + 1 < n; i += 2) {
        fe = flexNext(rows, fe);
        databox set;
        flexGetByType(fe, &set);
        intsetU32Free(boxPtrToU32(set));
        fe = flexNext(rows, fe);
    }
    flexFree(rows);
}

/* Append the finished bucket ['key' -> the 'count' offsets in 'offsets',
 * reversed for a negative bucket] as a row */
static void intsetBigEmitBucket_(flex **rows, const databoxBig *key,
                                 const uint32_t *offsets, uint32_t count,
                                 bool negative) {
    /* @own: container; free=intsetU32Free */
    intsetU32 *set = intsetU32NewLen(count);
    uint32_t *to = intsetU32Array(set);
    for (uint32_t i = 0; i < count; i++) {
        to[i] = offsets[negative ? count - 1 - i : i];
    }
    intsetU32UpdateCount(set, count);
    const databox setBox = {.type = DATABOX_PTR, .data.ptr = set};
    const databox *row[] = {(const databox *)key, &setBox};
    flexAppendMultipleExact(rows, 2, row);
}

intsetBig *intsetBigNewFromMembers(const flex *members) {
    /* Members arrive bucket by bucket in bucket-map order, as the iterator
     * walks them: a bucket's offsets ascend, except a negative bucket's,
     * which descend (its largest offset is its smallest value).  Each
     * bucket's offsets gather in 'scratch' and become its set in one
     * write; the bucket map is built once from the finished rows. */
    /* @own: container; free=flexFree */
    flex *rows = flexNew();
    uint32_t *scratch = nullptr;
    uint32_t scratchCap = 0;
    uint32_t inBucket = 0;
    databoxBig curKey = {};
    bool curNegative = false;
    size_t total = 0;

    const size_t n = flexCount(members);
    flexEntry *fe = flexHead(members);
    for (size_t i = 0; i < n; i++, fe = flexNext(members, fe)) {
        databoxBig v;
        flexGetByTypeBig(fe, &v);
        databoxBig key;
        uint32_t offset;
        bool negative;
        if (!intsetBigCanHold(&v) ||
            !bucketKeyFromInput(&v, &offset, &negative, &key)) {
            goto refuse;
        }

        const int cmp = inBucket ? databoxCompare((const databox *)&curKey,
                                                  (const databox *)&key)
                                 : -1;
        if (cmp > 0) {
            goto refuse; /* buckets out of order */
        }
        if (cmp == 0) {
            const uint32_t last = scratch[inBucket - 1];
            if (negative ? offset >= last : offset <= last) {
                goto refuse; /* offsets out of order */
            }
        } else {
            if (inBucket) {
                intsetBigEmitBucket_(&rows, &curKey, scratch, inBucket,
                                     curNegative);
            }
            curKey = key;
            curNegative = negative;
            inBucket = 0;
        }

        if (inBucket == scratchCap) {
            scratchCap = scratchCap ? scratchCap * 2 : 64;
            /* @own: container; free=zfree */
            scratch = zrealloc(scratch, scratchCap * sizeof(*scratch));
        }
        scratch[inBucket++] = offset;
        total++;
    }
    if (inBucket) {
        intsetBigEmitBucket_(&rows, &curKey, scratch, inBucket, curNegative);
    }
    zfree(scratch);

    /* @own: caller; free=intsetBigFree */
    intsetBig *isb = intsetBigNew();
    multimapFree(isb->i);
    /* The rows ascend by key (checked above), so this never refuses */
    /* @own: container; free=multimapFree */
    isb->i = multimapNewFromDump(2, false, false, FLEX_CAP_LEVEL_2048, rows);
    isb->count = total;
    return isb;

refuse:
    zfree(scratch);
    intsetBigFreeRows_(rows);
    return nullptr;
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
    intsetU32 *orig = nullptr;
    bool removed = false;
    bool isNegative;

    /* Create lookup/storage criteria and attempt to fetch a set */
    if (setFromInput(isb, val, &offset, &isNegative, &lookup, &found)) {
        orig = boxPtrToU32(found);
        removed = intsetU32Remove(boxPtrToU32(found), offset);
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
        isbIter->iter.entry = nullptr;
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
    databox vptr = {};
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

static bool intsetBigIteratorNextBucket(intsetBigIterator *restrict isbIter,
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
#ifdef __clang__
            __builtin_assume(iA != iB);
#endif

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

static bool intsetBigAddByBucketDirectOverwriteBulk(intsetBig *isb,
                                                    databoxBig *bucket,
                                                    intsetU32 *iu32) {
    /* NOTE: These MODIFY isb->i and requires INVALIDATING any iterator you
     *       currently have open! */
    if (intsetU32Count(iu32) > 0) {
        databox vptr = databoxNewPtr(iu32);
        const databox *insert[2] = {(databox *)bucket, &vptr};
        multimapInsert(&isb->i, insert);

        return true;
    }

    /* if count is zero, just delete bucket and release intsetU32. */
    multimapDelete(&isb->i, (databox *)bucket);
    intsetU32Free(iu32);

    return false;
}

static bool intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
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
    /* element-by-element zipper algorithm for intersecting two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared < 0) {
            if (a == result) {
                /* In-place: a bucket only in 'a' is NOT in the
                 * intersection — remove it (bucket keys are numeric,
                 * held by value, so they survive the delete).  The
                 * delete invalidates the iterator; re-seek to the next
                 * bucket past the removed key. */
                intsetU32 *eaai = val[0].data.ptr;
                result->count -= intsetU32Count(eaai);
                intsetU32Free(eaai);
                multimapDelete(&result->i, ea[0]);
                multimapIteratorInitAt(a->i, &ia, true, (databox *)&key[0]);
            }

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
            /* @own: container; free=intsetU32Free */
            intsetU32 *tmpU32 = intsetU32NewLen(smallest);

            /* Run the intersection with storage directly into 'tmpU32' */
            const size_t intersectedCount = intersectIntAuto(
                intsetU32Array(eaai), eaaiCount, intsetU32Array(ebbi),
                ebbiCount, intsetU32Array(tmpU32));

            /* Resize and update metadata of 'tmpU32' */
            intsetU32UpdateCount(tmpU32, intersectedCount);
#if 1
            intsetU32ShrinkToSize(tmpU32);
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
                /* The overwrite changed the underlying multimap, so the
                 * iterator must be re-seeded.  When the intersection
                 * emptied the bucket, the overwrite DELETED it: the
                 * re-seek then already lands on the next bucket, and a
                 * second advance would silently skip one. */
                assert(a->i == result->i);

                multimapIteratorInitAt(a->i, &ia, true, (databox *)&key[0]);
                foundA = multimapIteratorNext(&ia, ea);
                if (intersectedCount > 0 && foundA) {
                    /* current position is the bucket just processed */
                    foundA = multimapIteratorNext(&ia, ea);
                }
            } else {
                foundA = multimapIteratorNext(&ia, ea);
            }

            /* Continue finding next matching buckets. */
            foundB = multimapIteratorNext(&ib, eb);
        }
    }

    if (a == result) {
        /* In-place: every bucket remaining after 'b' is exhausted has
         * no partner in 'b' — none of it intersects. */
        while (foundA) {
            intsetU32 *eaai = val[0].data.ptr;
            result->count -= intsetU32Count(eaai);
            intsetU32Free(eaai);
            multimapDelete(&result->i, ea[0]);
            multimapIteratorInitAt(a->i, &ia, true, (databox *)&key[0]);
            foundA = multimapIteratorNext(&ia, ea);
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
    /* element-by-element zipper algorithm for merging two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        intsetU32 *eaai = val[0].data.ptr;
        intsetU32 *ebbi = val[1].data.ptr;

        if (compared < 0) {
            /* A < B: A's bucket is already in result (from initial copy), just
             * advance A */
            foundA = multimapIteratorNext(&ia, ea);
        } else if (compared > 0) {
            /* B < A: B's bucket not in result yet, add it using B's key */
            /* @own: container; free=intsetU32Free */
            intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
                a, &key[1], intsetU32Copy(ebbi), &ia);
            a->count += intsetU32Count(ebbi);
            foundB = multimapIteratorNext(&ib, eb);
        } else {
            /* Match! _Merge_ keys from B into A! */

            /* Track original 'eaai' pointer value */
            const intsetU32 *orig = eaai;

            /* Perform merge of b[bucket] into a[bucket] */
            a->count += intsetU32Merge(eaai, ebbi);

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

    /* After main zipper loop, add any remaining elements from B */
    while (foundB) {
        intsetU32 *ebbi = val[1].data.ptr;
        /* @own: container; free=intsetU32Free */
        intsetBigAddByBucketDirectOverwriteBulkUpdateIterator(
            a, &key[1], intsetU32Copy(ebbi), &ia);
        a->count += intsetU32Count(ebbi);
        foundB = multimapIteratorNext(&ib, eb);
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
    if (isb->count == 0) {
        return false;
    }

    /* A uniform position from the per-thread generator */
    const uint64_t selectedIndex = dkRandomBounded(isb->count);

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

        /* Strict '>': selectedIndex is 0-based, so a bucket owns global
         * indices [currentCount - localCount, currentCount - 1]; an
         * index equal to currentCount belongs to the NEXT bucket (and
         * would compute a -1 offset here). */
        if (currentCount > selectedIndex) {
            /* random element is INSIDE THE HOUSE */
            const ssize_t useOffset = currentCount - selectedIndex - 1;
            assert(useOffset >= 0);

            uint32_t localIVAL;
            [[maybe_unused]] const bool got =
                intsetU32Get(use, useOffset, &localIVAL);
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

    if (!intsetBigRandom(isb, deleted)) {
        /* empty set: nothing to delete, 'deleted' is not populated */
        return false;
    }

    /* TODO: we could improve the performance here by making 'delete' a special
     * case of intsetBigRandom() itself since intsetBigRandom() already has the
     * bucket and intsetU32 values directly when it creates the return value
     * 'deleted' here. */
    return intsetBigRemove(isb, deleted);
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "perf.h"

/* The __int128_t a decimal test literal spells (the parse must succeed;
 * it runs outside the assert) */
static __int128_t intsetBigTestParse_(const char *a) {
    __int128_t v = 0;
    [[maybe_unused]] const bool ok = StrBufToInt128(a, strlen(a), &v);
    assert(ok);
    return v;
}

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

DK_FN_NO_OPTIMIZE int intsetBigTest(int, char *[]) {
    int err = 0;

    /* Create and free empty set */
    TEST("create and free") {
        /* A new set is empty */
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (intsetBigCountElements(isb) != 0 ||
            intsetBigCountBuckets(isb) != 0 ||
            intsetBigExists(isb, &DATABOX_BIG_UNSIGNED(0))) {
            ERR("a new set holds %zu elements in %zu buckets",
                intsetBigCountElements(isb), intsetBigCountBuckets(isb));
        }
    }

    /* Add small (bucket 0) to new set */
    TEST("add to bucket 0") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_UNSIGNED(25))) {
            ERR("Failed to add to bucket 0!");
        }
    }

    /* Add small (bucket -1) to new set */
    TEST("add to bucket -1") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_SIGNED(-25))) {
            ERR("Failed to add to bucket -1!");
        }
    }

    /* Add bigger to new set (bucket 1) */
    TEST("add to bucket 1") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_UNSIGNED((1ULL << 35)))) {
            ERR("Failed to add to bucket 1!");
        }
    }

    /* Add bigger to new set (bucket -2) */
    TEST("add to bucket -2") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, &DATABOX_BIG_SIGNED(-(1LL << 35)))) {
            ERR("Failed to add to bucket -2!");
        }
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

        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, final)) {
            ERR("Failed to add smallest?");
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

        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        if (!intsetBigAdd(isb, final)) {
            ERR("Failed to add biggest?");
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
    }

    TEST("add from bucket -62 to 63") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();

        /* + 1 + 1 is for the two ±2^74 strings */
        databoxBig testers[(62 + 64) + 1 + 1] = {};

        /* This is -(2^74 + 332197) and goes first because our
         * tests are sorted from smallest to largest inputs
         * (and also because iteration happens in sorted order
         *  from smallest to largest inputs, so for proper validation,
         *  we test the testers[0] order against the retrieved iteration
         *  order to verify everything we requested actually can be
         *  retrieved and discovered when searching) */
        const char *unlarge = "-18889465931478581186981";

        /* Perform a quick speed test of string to __int128_t conversion. */
        {
            assert(0 == intsetBigTestParse_("0"));
            assert(10 == intsetBigTestParse_("10"));
            assert(INT64_MAX == intsetBigTestParse_("9223372036854775807"));
            assert(UINT64_MAX == intsetBigTestParse_("18446744073709551615"));

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
                    assert(false);
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
                (void)StrUInt64ToBuf(buf, sizeof(buf), val);
                if (val > UINT64_MAX / 100) {
                    val = 10;
                }
            }
            TIME_FINISH(count, "StrUInt64ToBuf");
        }

        /* + 1 below because testers[0] was manually set too */
        __uint128_t bigU;
        StrBufToUInt128(large, strlen(large), &bigU);
        DATABOX_BIG_UNSIGNED_128(&testers[(62 + 64) + 1], bigU);

        /* Add elements to set */
        for (size_t i = 0; i < countof(testers); i++) {
            databoxBig *use = &testers[i];

#if 0
            databoxReprSay("Storing", use);
#endif
            if (!intsetBigAdd(isb, use)) {
                ERR("Failed to add to bucket %zu!\n", i);
            }
        }

        /* Now check if added elements can be found again */
        for (size_t i = 0; i < countof(testers); i++) {
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
        for (size_t i = 0; i < countof(testers); i++) {
            databoxBig *use = &testers[i];

            if (!intsetBigRemove(isb, use)) {
                ERR("Failed to remove value for iteration %zu!", i);
            }
        }

        if (intsetBigCountBuckets(isb)) {
            ERR("Has buckets?");
        }

        if (intsetBigCountElements(isb)) {
            ERR("Has elements?");
        }
    }

    TEST("duplicate add returns false") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig val = DATABOX_BIG_UNSIGNED(12345);

        if (!intsetBigAdd(isb, &val)) {
            ERR("First add should succeed!");
        }

        if (intsetBigAdd(isb, &val)) {
            ERR("Duplicate add should return false!");
        }

        if (intsetBigCountElements(isb) != 1) {
            ERR("Count should be 1 after duplicate add!");
        }
    }

    TEST("a non-integer value is refused and changes nothing") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig val = DATABOX_BIG_UNSIGNED(12345);
        if (!intsetBigAdd(isb, &val)) {
            ERR("First add should succeed!");
        }

        const databoxBig notInteger = {.type = DATABOX_DOUBLE_64,
                                       .data.d64 = 1.5};
        if (intsetBigAdd(isb, &notInteger)) {
            ERR("Adding a double should be refused!");
        }

        if (intsetBigExists(isb, &notInteger)) {
            ERR("A double should never exist!");
        }

        if (intsetBigRemove(isb, &notInteger)) {
            ERR("Removing a double should be refused!");
        }

        if (intsetBigCountElements(isb) != 1 ||
            intsetBigCountBuckets(isb) != 1) {
            ERR("A refused value changed the set: %zu elements, %zu buckets",
                intsetBigCountElements(isb), intsetBigCountBuckets(isb));
        }
    }

    TEST("remove non-existent returns false") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig val = DATABOX_BIG_UNSIGNED(12345);

        if (intsetBigRemove(isb, &val)) {
            ERR("Remove from empty set should return false!");
        }

        /* Add one value, try to remove different value */
        intsetBigAdd(isb, &val);
        databoxBig other = DATABOX_BIG_UNSIGNED(99999);
        if (intsetBigRemove(isb, &other)) {
            ERR("Remove non-existent should return false!");
        }

        if (intsetBigCountElements(isb) != 1) {
            ERR("Count should still be 1!");
        }
    }

    TEST("exists on empty set returns false") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig val = DATABOX_BIG_UNSIGNED(12345);

        if (intsetBigExists(isb, &val)) {
            ERR("Exists on empty set should return false!");
        }
    }

    TEST("iterator on empty set yields nothing") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        intsetBigIterator iter;
        databoxBig val;

        intsetBigIteratorInit(isb, &iter);
        if (intsetBigIteratorNextBox(&iter, &val)) {
            ERR("Iterator on empty set should yield nothing!");
        }
    }

    TEST("intsetBigCopy creates independent copy") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        databoxBig vals[5] = {
            DATABOX_BIG_UNSIGNED(100), DATABOX_BIG_SIGNED(-200),
            DATABOX_BIG_UNSIGNED(1ULL << 30), DATABOX_BIG_SIGNED(-(1LL << 25)),
            DATABOX_BIG_UNSIGNED(0)};

        for (size_t i = 0; i < countof(vals); i++) {
            intsetBigAdd(isb, &vals[i]);
        }

        cleaner(intsetBigFree) intsetBig *copy = intsetBigCopy(isb);

        /* Verify copy has same elements */
        if (intsetBigCountElements(copy) != intsetBigCountElements(isb)) {
            ERR("Copy should have same count!");
        }

        for (size_t i = 0; i < countof(vals); i++) {
            if (!intsetBigExists(copy, &vals[i])) {
                ERR("Copy missing value %zu!", i);
            }
        }

        /* Modify original, verify copy unchanged */
        databoxBig newVal = DATABOX_BIG_UNSIGNED(999999);
        intsetBigAdd(isb, &newVal);

        if (intsetBigExists(copy, &newVal)) {
            ERR("Copy should be independent of original!");
        }

        if (intsetBigCountElements(copy) != 5) {
            ERR("Copy count should still be 5!");
        }
    }

    TEST("intsetBigEqual") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Empty sets are equal */
        if (!intsetBigEqual(a, b)) {
            ERR("Empty sets should be equal!");
        }

        /* Same set is equal to itself */
        if (!intsetBigEqual(a, a)) {
            ERR("Set should equal itself!");
        }

        /* Add same elements to both */
        databoxBig vals[3] = {DATABOX_BIG_UNSIGNED(100),
                              DATABOX_BIG_SIGNED(-50),
                              DATABOX_BIG_UNSIGNED(1ULL << 35)};

        for (size_t i = 0; i < countof(vals); i++) {
            intsetBigAdd(a, &vals[i]);
            intsetBigAdd(b, &vals[i]);
        }

        if (!intsetBigEqual(a, b)) {
            ERR("Sets with same elements should be equal!");
        }

        /* Add different element to one */
        databoxBig extra = DATABOX_BIG_UNSIGNED(777);
        intsetBigAdd(a, &extra);

        if (intsetBigEqual(a, b)) {
            ERR("Sets with different elements should not be equal!");
        }
    }

    TEST("intsetBigSubset") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Empty set is subset of any set */
        if (!intsetBigSubset(a, b)) {
            ERR("Empty set should be subset of empty set!");
        }

        /* Add elements to b */
        databoxBig vals[4] = {
            DATABOX_BIG_UNSIGNED(10), DATABOX_BIG_UNSIGNED(20),
            DATABOX_BIG_UNSIGNED(30), DATABOX_BIG_UNSIGNED(40)};

        for (size_t i = 0; i < countof(vals); i++) {
            intsetBigAdd(b, &vals[i]);
        }

        /* Empty set is subset of non-empty */
        if (!intsetBigSubset(a, b)) {
            ERR("Empty set should be subset of non-empty set!");
        }

        /* Add subset of elements to a */
        intsetBigAdd(a, &vals[0]);
        intsetBigAdd(a, &vals[2]);

        if (!intsetBigSubset(a, b)) {
            ERR("a should be subset of b!");
        }

        /* Add element not in b to a */
        databoxBig extra = DATABOX_BIG_UNSIGNED(999);
        intsetBigAdd(a, &extra);

        if (intsetBigSubset(a, b)) {
            ERR("a with extra element should not be subset of b!");
        }
    }

    TEST("intsetBigIntersect") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

        /* Intersect empty sets */
        size_t count = intsetBigIntersect(a, b, result);
        if (count != 0 || intsetBigCountElements(result) != 0) {
            ERR("Intersect of empty sets should be empty!");
        }

        /* Add elements: a = {1, 2, 3, 4}, b = {2, 4, 6, 8} */
        databoxBig aVals[4] = {DATABOX_BIG_UNSIGNED(1), DATABOX_BIG_UNSIGNED(2),
                               DATABOX_BIG_UNSIGNED(3),
                               DATABOX_BIG_UNSIGNED(4)};
        databoxBig bVals[4] = {DATABOX_BIG_UNSIGNED(2), DATABOX_BIG_UNSIGNED(4),
                               DATABOX_BIG_UNSIGNED(6),
                               DATABOX_BIG_UNSIGNED(8)};

        for (size_t i = 0; i < 4; i++) {
            intsetBigAdd(a, &aVals[i]);
            intsetBigAdd(b, &bVals[i]);
        }

        /* Clear result and intersect */
        intsetBigFree(result);
        result = intsetBigNew();

        count = intsetBigIntersect(a, b, result);
        if (count != 2) {
            ERR("Intersect count should be 2, got %zu!", count);
        }

        /* Verify intersection contains {2, 4} */
        databoxBig two = DATABOX_BIG_UNSIGNED(2);
        databoxBig four = DATABOX_BIG_UNSIGNED(4);
        databoxBig one = DATABOX_BIG_UNSIGNED(1);

        if (!intsetBigExists(result, &two) || !intsetBigExists(result, &four)) {
            ERR("Intersection should contain 2 and 4!");
        }

        if (intsetBigExists(result, &one)) {
            ERR("Intersection should not contain 1!");
        }
    }

    TEST("intsetBigMergeInto") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Add elements: a = {1, 2, 3}, b = {3, 4, 5} */
        databoxBig aVals[3] = {DATABOX_BIG_UNSIGNED(1), DATABOX_BIG_UNSIGNED(2),
                               DATABOX_BIG_UNSIGNED(3)};
        databoxBig bVals[3] = {DATABOX_BIG_UNSIGNED(3), DATABOX_BIG_UNSIGNED(4),
                               DATABOX_BIG_UNSIGNED(5)};

        for (size_t i = 0; i < 3; i++) {
            intsetBigAdd(a, &aVals[i]);
            intsetBigAdd(b, &bVals[i]);
        }

        /* Merge b into a: a should become {1, 2, 3, 4, 5} */
        intsetBigMergeInto(a, b);

        if (intsetBigCountElements(a) != 5) {
            ERR("After merge, a should have 5 elements, got %zu!",
                intsetBigCountElements(a));
        }

        /* Verify all elements present */
        for (size_t i = 0; i < 3; i++) {
            if (!intsetBigExists(a, &aVals[i])) {
                ERR("Merged set missing original element %zu!", i);
            }
            if (!intsetBigExists(a, &bVals[i])) {
                ERR("Merged set missing merged element %zu!", i);
            }
        }
    }

    TEST("intsetBigRandom and intsetBigRandomDelete") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();

        /* Add some elements */
        for (uint64_t i = 0; i < 100; i++) {
            databoxBig val = DATABOX_BIG_UNSIGNED(i * 1000);
            intsetBigAdd(isb, &val);
        }

        if (intsetBigCountElements(isb) != 100) {
            ERR("Should have 100 elements!");
        }

        /* Get random elements and verify they exist */
        for (int i = 0; i < 50; i++) {
            databoxBig randomVal;
            if (!intsetBigRandom(isb, &randomVal)) {
                ERR("Random should succeed on non-empty set!");
            }

            if (!intsetBigExists(isb, &randomVal)) {
                ERR("Random value should exist in set!");
            }
        }

        /* Random delete until empty */
        size_t originalCount = intsetBigCountElements(isb);
        for (size_t i = 0; i < originalCount; i++) {
            databoxBig deleted;
            if (!intsetBigRandomDelete(isb, &deleted)) {
                ERR("RandomDelete should succeed, iteration %zu!", i);
            }

            if (intsetBigExists(isb, &deleted)) {
                ERR("Deleted value should not exist anymore!");
            }
        }

        if (intsetBigCountElements(isb) != 0) {
            ERR("Set should be empty after deleting all elements!");
        }

        /* Empty set: Random and RandomDelete report false, no crash */
        databoxBig none;
        if (intsetBigRandom(isb, &none)) {
            ERR("Random on an empty set must return false!");
        }

        if (intsetBigRandomDelete(isb, &none)) {
            ERR("RandomDelete on an empty set must return false!");
        }
    }

    TEST("intsetBigRandom across multiple buckets") {
        /* Values spread across several 2^20-wide buckets: every global
         * index must map to a valid in-bucket offset, including the
         * bucket-boundary indices. */
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        const uint64_t bucketSpan = UINT64_C(1) << 20;
        for (uint64_t b = 0; b < 5; b++) {
            for (uint64_t i = 0; i < 7; i++) {
                databoxBig val = DATABOX_BIG_UNSIGNED((b * bucketSpan) + i);
                intsetBigAdd(isb, &val);
            }
        }

        assert(intsetBigCountElements(isb) == 35);

        for (int i = 0; i < 2000; i++) {
            databoxBig randomVal;
            if (!intsetBigRandom(isb, &randomVal)) {
                ERR("Random should succeed on multi-bucket set!");
            }

            if (!intsetBigExists(isb, &randomVal)) {
                ERR("Random value should exist in multi-bucket set!");
            }
        }
    }

    TEST("intsetBigBytes") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        size_t emptyBytes = intsetBigBytes(isb);

        if (emptyBytes == 0) {
            ERR("Even empty set should have some bytes!");
        }

        /* Add elements and verify bytes increase */
        for (uint64_t i = 0; i < 1000; i++) {
            databoxBig val = DATABOX_BIG_UNSIGNED(i);
            intsetBigAdd(isb, &val);
        }

        size_t fullBytes = intsetBigBytes(isb);
        if (fullBytes <= emptyBytes) {
            ERR("Bytes should increase with more elements!");
        }
    }

    TEST("bucket boundary values") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();

        /* Test values around bucket boundaries (divisor = 2^20 = 1048576) */
        uint64_t boundary = 1ULL << 20;
        databoxBig vals[] = {
            DATABOX_BIG_UNSIGNED(boundary - 1),     /* last in bucket 0 */
            DATABOX_BIG_UNSIGNED(boundary),         /* first in bucket 1 */
            DATABOX_BIG_UNSIGNED(boundary + 1),     /* second in bucket 1 */
            DATABOX_BIG_UNSIGNED(2 * boundary - 1), /* last in bucket 1 */
            DATABOX_BIG_UNSIGNED(2 * boundary),     /* first in bucket 2 */
            DATABOX_BIG_SIGNED(-(int64_t)(boundary - 1)), /* in bucket -1 */
            DATABOX_BIG_SIGNED(-(int64_t)boundary),       /* in bucket -1 */
            DATABOX_BIG_SIGNED(-(int64_t)(boundary + 1)), /* in bucket -2 */
        };

        for (size_t i = 0; i < countof(vals); i++) {
            if (!intsetBigAdd(isb, &vals[i])) {
                ERR("Failed to add boundary value %zu!", i);
            }
        }

        /* Verify all exist */
        for (size_t i = 0; i < countof(vals); i++) {
            if (!intsetBigExists(isb, &vals[i])) {
                ERR("Boundary value %zu not found!", i);
            }
        }

        /* Verify correct bucket count */
        size_t buckets = intsetBigCountBuckets(isb);
        if (buckets != 5) { /* buckets: -2, -1, 0, 1, 2 */
            ERR("Expected 5 buckets, got %zu!", buckets);
        }

        /* Remove and verify */
        for (size_t i = 0; i < countof(vals); i++) {
            if (!intsetBigRemove(isb, &vals[i])) {
                ERR("Failed to remove boundary value %zu!", i);
            }
        }

        if (intsetBigCountElements(isb) != 0) {
            ERR("Should be empty after removing all!");
        }
    }

    TEST("signed zero edge case") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();

        /* Test signed and unsigned zero */
        databoxBig uzero = DATABOX_BIG_UNSIGNED(0);
        databoxBig szero = DATABOX_BIG_SIGNED(0);

        intsetBigAdd(isb, &uzero);

        /* Both representations of zero should exist */
        if (!intsetBigExists(isb, &uzero)) {
            ERR("Unsigned zero should exist!");
        }

        /* Signed zero also goes to bucket 0 offset 0 */
        if (!intsetBigExists(isb, &szero)) {
            ERR("Signed zero should also be found!");
        }

        if (intsetBigCountElements(isb) != 1) {
            ERR("Should only have 1 element (zero)!");
        }
    }

    TEST("large scale add/remove/iterate") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        const size_t N = 10000;

        /* Add N elements with various patterns */
        for (size_t i = 0; i < N; i++) {
            uint64_t val =
                (i * 7919) % (1ULL << 40); /* Pseudo-random scatter */
            databoxBig box = DATABOX_BIG_UNSIGNED(val);
            intsetBigAdd(isb, &box);
        }

        /* Count via iteration should match */
        intsetBigIterator iter;
        databoxBig val;
        size_t iterCount = 0;

        intsetBigIteratorInit(isb, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            iterCount++;
        }

        if (iterCount != intsetBigCountElements(isb)) {
            ERR("Iterator count %zu != element count %zu!", iterCount,
                intsetBigCountElements(isb));
        }

        /* Verify sorted order in iteration */
        databoxBig prev = {};
        bool first = true;
        intsetBigIteratorInit(isb, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            if (!first) {
                if (databoxCompare((databox *)&prev, (databox *)&val) >= 0) {
                    ERR("Iterator should yield sorted ascending order!");
                }
            }
            prev = val;
            first = false;
        }
    }

    TEST("fuzzer - random operations") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        /* Simple reference set for validation (limited range) */
        bool refSet[10000] = {};
        const size_t refMax = 10000;

        /* Seed for reproducibility */
        srandom(12345);

        const size_t numOps = 5000;
        for (size_t op = 0; op < numOps; op++) {
            int opType = random() % 4;
            uint64_t val = random() % refMax;
            databoxBig box = DATABOX_BIG_UNSIGNED(val);

            switch (opType) {
            case 0: { /* Add */
                bool added = intsetBigAdd(isb, &box);
                if (refSet[val]) {
                    if (added) {
                        ERR("Add %" PRIu64 ": duplicate should return false!",
                            val);
                    }
                } else {
                    if (!added) {
                        ERR("Add %" PRIu64 ": new value should return true!",
                            val);
                    }
                    refSet[val] = true;
                }
                break;
            }
            case 1: { /* Remove */
                bool removed = intsetBigRemove(isb, &box);
                if (refSet[val]) {
                    if (!removed) {
                        ERR("Remove %" PRIu64 ": existing should return true!",
                            val);
                    }
                    refSet[val] = false;
                } else {
                    if (removed) {
                        ERR("Remove %" PRIu64
                            ": non-existent should return false!",
                            val);
                    }
                }
                break;
            }
            case 2: { /* Exists */
                bool exists = intsetBigExists(isb, &box);
                if (exists != refSet[val]) {
                    ERR("Exists %" PRIu64 ": mismatch (got %d, expected %d)!",
                        val, exists, refSet[val]);
                }
                break;
            }
            case 3: { /* Random (if non-empty) */
                size_t count = intsetBigCountElements(isb);
                if (count > 0) {
                    databoxBig randomVal;
                    if (!intsetBigRandom(isb, &randomVal)) {
                        ERR("Random failed on non-empty set!");
                    }
                    if (!intsetBigExists(isb, &randomVal)) {
                        ERR("Random value doesn't exist!");
                    }
                }
                break;
            }
            }
        }

        /* Final validation: count reference set vs intsetBig */
        size_t refCount = 0;
        for (size_t i = 0; i < refMax; i++) {
            if (refSet[i]) {
                refCount++;
                databoxBig box = DATABOX_BIG_UNSIGNED(i);
                if (!intsetBigExists(isb, &box)) {
                    ERR("Final check: %zu should exist!", i);
                }
            }
        }

        if (refCount != intsetBigCountElements(isb)) {
            ERR("Final count mismatch: ref=%zu, isb=%zu!", refCount,
                intsetBigCountElements(isb));
        }
    }

    TEST("fuzzer - mixed positive and negative values") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        srandom(54321);

        const size_t numOps = 3000;
        for (size_t op = 0; op < numOps; op++) {
            int64_t val =
                (random() % 200'000) - 100'000; /* Range: -100000 to 99999 */
            databoxBig box = DATABOX_BIG_SIGNED(val);

            int opType = random() % 3;
            switch (opType) {
            case 0:
                intsetBigAdd(isb, &box);
                break;
            case 1:
                intsetBigRemove(isb, &box);
                break;
            case 2:
                intsetBigExists(isb, &box);
                break;
            }
        }

        /* Verify iteration order is sorted */
        intsetBigIterator iter;
        databoxBig val, prev = {};
        bool first = true;
        size_t count = 0;

        intsetBigIteratorInit(isb, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            if (!first) {
                if (databoxCompare((databox *)&prev, (databox *)&val) >= 0) {
                    ERR("Iteration order should be ascending!");
                }
            }
            prev = val;
            first = false;
            count++;
        }

        if (count != intsetBigCountElements(isb)) {
            ERR("Iteration count %zu != element count %zu!", count,
                intsetBigCountElements(isb));
        }
    }

    TEST("fuzzer - 128-bit values") {
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        srandom(99999);

        databoxBig stored[100];
        size_t storedCount = 0;

        /* Add 128-bit values spread across the supported range */
        for (int i = 0; i < 100; i++) {
            /* Generate values in various parts of 128-bit range */
            __int128_t val;
            int range = random() % 6;
            switch (range) {
            case 0: /* Near zero */
                val = (random() % 10000) - 5000;
                break;
            case 1: /* Near INT64_MAX */
                val = INT64_MAX - (random() % 10000);
                break;
            case 2: /* Near INT64_MIN */
                val = INT64_MIN + (random() % 10000);
                break;
            case 3: /* Beyond INT64 positive */
                val = ((__int128_t)INT64_MAX) + (random() % 1'000'000);
                break;
            case 4: /* Beyond INT64 negative */
                val = ((__int128_t)INT64_MIN) - (random() % 1'000'000);
                break;
            case 5: /* Large 128-bit */
                val = ((__int128_t)1 << 70) + (random() % 1'000'000);
                if (random() % 2) {
                    val = -val;
                }
                break;
            default:
                val = random();
            }

            /* Clamp to supported range */
            if (val < INTSET_BIG_INT128_MIN) {
                val = INTSET_BIG_INT128_MIN + (random() % 1000);
            }
            if (val > INTSET_BIG_INT128_MAX) {
                val = INTSET_BIG_INT128_MAX - (random() % 1000);
            }

            DATABOX_BIG_SIGNED_128(&stored[storedCount], val);
            if (intsetBigAdd(isb, &stored[storedCount])) {
                storedCount++;
            }
        }

        /* Verify all stored values exist */
        for (size_t i = 0; i < storedCount; i++) {
            if (!intsetBigExists(isb, &stored[i])) {
                ERR("128-bit value %zu not found!", i);
            }
        }

        /* Remove half and verify */
        for (size_t i = 0; i < storedCount / 2; i++) {
            if (!intsetBigRemove(isb, &stored[i])) {
                ERR("Failed to remove 128-bit value %zu!", i);
            }
        }

        for (size_t i = 0; i < storedCount / 2; i++) {
            if (intsetBigExists(isb, &stored[i])) {
                ERR("Removed 128-bit value %zu should not exist!", i);
            }
        }

        for (size_t i = storedCount / 2; i < storedCount; i++) {
            if (!intsetBigExists(isb, &stored[i])) {
                ERR("Unremoved 128-bit value %zu should exist!", i);
            }
        }
    }

    /* ====================================================================
     * COMPREHENSIVE MULTI-BUCKET TESTS
     * These tests specifically exercise the zipper algorithm across
     * multiple buckets to catch edge cases like the key[0]/key[1] bug
     * and the missing remainder loop bug in intsetBigMergeInto.
     * ==================================================================== */

#define BUCKET_SIZE (1ULL << 20) /* 1,048,576 - matches DIVISOR_WIDTH */

    TEST("intsetBigMergeInto - multiple buckets, B extends past A") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has buckets 0 and 1 */
        databoxBig a0 = DATABOX_BIG_UNSIGNED(100);              /* bucket 0 */
        databoxBig a1 = DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 50); /* bucket 1 */
        intsetBigAdd(a, &a0);
        intsetBigAdd(a, &a1);

        /* B has buckets 1, 2, and 3 (extends past A) */
        databoxBig b1 = DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 200); /* bucket 1 */
        databoxBig b2 =
            DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100); /* bucket 2 */
        databoxBig b3 =
            DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100); /* bucket 3 */
        intsetBigAdd(b, &b1);
        intsetBigAdd(b, &b2);
        intsetBigAdd(b, &b3);

        intsetBigMergeInto(a, b);

        /* Result should have 5 elements: a0, a1, b1, b2, b3 */
        if (intsetBigCountElements(a) != 5) {
            ERR("Expected 5 elements after merge, got %zu!",
                intsetBigCountElements(a));
        }

        /* Verify all elements present */
        if (!intsetBigExists(a, &a0)) {
            ERR("Missing a0!");
        }
        if (!intsetBigExists(a, &a1)) {
            ERR("Missing a1!");
        }
        if (!intsetBigExists(a, &b1)) {
            ERR("Missing b1!");
        }
        if (!intsetBigExists(a, &b2)) {
            ERR("Missing b2 (from B's bucket 2)!");
        }
        if (!intsetBigExists(a, &b3)) {
            ERR("Missing b3 (from B's bucket 3)!");
        }

        /* Verify bucket count */
        if (intsetBigCountBuckets(a) != 4) {
            ERR("Expected 4 buckets, got %zu!", intsetBigCountBuckets(a));
        }
    }

    TEST("intsetBigMergeInto - completely disjoint buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has only even buckets: 0, 2, 4 */
        databoxBig a0 = DATABOX_BIG_UNSIGNED(100);
        databoxBig a2 = DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100);
        databoxBig a4 = DATABOX_BIG_UNSIGNED(4 * BUCKET_SIZE + 100);
        intsetBigAdd(a, &a0);
        intsetBigAdd(a, &a2);
        intsetBigAdd(a, &a4);

        /* B has only odd buckets: 1, 3, 5 */
        databoxBig b1 = DATABOX_BIG_UNSIGNED(1 * BUCKET_SIZE + 100);
        databoxBig b3 = DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100);
        databoxBig b5 = DATABOX_BIG_UNSIGNED(5 * BUCKET_SIZE + 100);
        intsetBigAdd(b, &b1);
        intsetBigAdd(b, &b3);
        intsetBigAdd(b, &b5);

        intsetBigMergeInto(a, b);

        /* Result should have 6 elements across 6 buckets */
        if (intsetBigCountElements(a) != 6) {
            ERR("Expected 6 elements, got %zu!", intsetBigCountElements(a));
        }
        if (intsetBigCountBuckets(a) != 6) {
            ERR("Expected 6 buckets, got %zu!", intsetBigCountBuckets(a));
        }

        /* Verify all elements */
        if (!intsetBigExists(a, &a0) || !intsetBigExists(a, &a2) ||
            !intsetBigExists(a, &a4)) {
            ERR("Missing A elements!");
        }
        if (!intsetBigExists(a, &b1) || !intsetBigExists(a, &b3) ||
            !intsetBigExists(a, &b5)) {
            ERR("Missing B elements!");
        }
    }

    TEST("intsetBigMergeInto - interleaved buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A = buckets {0, 2, 4} */
        /* B = buckets {1, 2, 3} (partially overlapping) */
        databoxBig a0 = DATABOX_BIG_UNSIGNED(100);
        databoxBig a2 = DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100);
        databoxBig a4 = DATABOX_BIG_UNSIGNED(4 * BUCKET_SIZE + 100);
        intsetBigAdd(a, &a0);
        intsetBigAdd(a, &a2);
        intsetBigAdd(a, &a4);

        databoxBig b1 = DATABOX_BIG_UNSIGNED(1 * BUCKET_SIZE + 50);
        databoxBig b2 = DATABOX_BIG_UNSIGNED(
            2 * BUCKET_SIZE + 200); /* different offset in bucket 2 */
        databoxBig b3 = DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 50);
        intsetBigAdd(b, &b1);
        intsetBigAdd(b, &b2);
        intsetBigAdd(b, &b3);

        intsetBigMergeInto(a, b);

        /* Should have 6 elements (a2 and b2 are in same bucket but different
         * values) */
        if (intsetBigCountElements(a) != 6) {
            ERR("Expected 6 elements, got %zu!", intsetBigCountElements(a));
        }

        /* Should have 5 buckets: 0, 1, 2, 3, 4 */
        if (intsetBigCountBuckets(a) != 5) {
            ERR("Expected 5 buckets, got %zu!", intsetBigCountBuckets(a));
        }

        /* Verify all elements */
        databoxBig allVals[] = {a0, a2, a4, b1, b2, b3};
        for (size_t i = 0; i < countof(allVals); i++) {
            if (!intsetBigExists(a, &allVals[i])) {
                ERR("Missing element %zu!", i);
            }
        }
    }

    TEST("intsetBigMergeInto - A extends past B") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has buckets 0, 1, 5, 6 */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(5 * BUCKET_SIZE + 100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(6 * BUCKET_SIZE + 100));

        /* B has only bucket 2 */
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 50));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 60));

        intsetBigMergeInto(a, b);

        if (intsetBigCountElements(a) != 6) {
            ERR("Expected 6 elements, got %zu!", intsetBigCountElements(a));
        }
        if (intsetBigCountBuckets(a) != 5) { /* 0, 1, 2, 5, 6 */
            ERR("Expected 5 buckets, got %zu!", intsetBigCountBuckets(a));
        }
    }

    TEST("intsetBigMergeInto - B is empty") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));

        intsetBigMergeInto(a, b);

        if (intsetBigCountElements(a) != 2) {
            ERR("Expected 2 elements, got %zu!", intsetBigCountElements(a));
        }
    }

    TEST("intsetBigMergeInto - A is empty") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100));

        intsetBigMergeInto(a, b);

        if (intsetBigCountElements(a) != 3) {
            ERR("Expected 3 elements, got %zu!", intsetBigCountElements(a));
        }
        if (intsetBigCountBuckets(a) != 3) {
            ERR("Expected 3 buckets, got %zu!", intsetBigCountBuckets(a));
        }

        /* Verify values are correct */
        databoxBig v0 = DATABOX_BIG_UNSIGNED(100);
        databoxBig v1 = DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100);
        databoxBig v2 = DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100);
        if (!intsetBigExists(a, &v0) || !intsetBigExists(a, &v1) ||
            !intsetBigExists(a, &v2)) {
            ERR("Missing merged elements!");
        }
    }

    TEST("intsetBigMergeInto - negative buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has negative buckets -2 and -1 */
        intsetBigAdd(a, &DATABOX_BIG_SIGNED(-(int64_t)(2 * BUCKET_SIZE + 100)));
        intsetBigAdd(a, &DATABOX_BIG_SIGNED(-100));

        /* B has negative bucket -3 and positive bucket 0 */
        intsetBigAdd(b, &DATABOX_BIG_SIGNED(-(int64_t)(3 * BUCKET_SIZE + 100)));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(50));

        intsetBigMergeInto(a, b);

        if (intsetBigCountElements(a) != 4) {
            ERR("Expected 4 elements, got %zu!", intsetBigCountElements(a));
        }

        /* Verify negative values preserved correctly */
        databoxBig neg3 = DATABOX_BIG_SIGNED(-(int64_t)(3 * BUCKET_SIZE + 100));
        databoxBig neg2 = DATABOX_BIG_SIGNED(-(int64_t)(2 * BUCKET_SIZE + 100));
        databoxBig neg1 = DATABOX_BIG_SIGNED(-100);
        databoxBig pos0 = DATABOX_BIG_UNSIGNED(50);

        if (!intsetBigExists(a, &neg3) || !intsetBigExists(a, &neg2) ||
            !intsetBigExists(a, &neg1) || !intsetBigExists(a, &pos0)) {
            ERR("Missing elements after negative bucket merge!");
        }
    }

    TEST("intsetBigMergeInto - many elements per bucket") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has 100 elements in bucket 0, 50 in bucket 2 */
        for (uint64_t i = 0; i < 100; i++) {
            intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(i));
        }
        for (uint64_t i = 0; i < 50; i++) {
            intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + i));
        }

        /* B has 75 elements in bucket 1, 25 in bucket 2 (different offsets) */
        for (uint64_t i = 0; i < 75; i++) {
            intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + i));
        }
        for (uint64_t i = 0; i < 25; i++) {
            intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 1000 + i));
        }

        intsetBigMergeInto(a, b);

        /* 100 + 50 + 75 + 25 = 250 */
        if (intsetBigCountElements(a) != 250) {
            ERR("Expected 250 elements, got %zu!", intsetBigCountElements(a));
        }
        if (intsetBigCountBuckets(a) != 3) {
            ERR("Expected 3 buckets, got %zu!", intsetBigCountBuckets(a));
        }
    }

    TEST("intsetBigMergeInto - verify iteration order after merge") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Add in non-sorted order across buckets */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 500));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 200));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(50));

        intsetBigMergeInto(a, b);

        /* Verify sorted iteration order */
        intsetBigIterator iter;
        databoxBig val, prev = {};
        bool first = true;
        size_t count = 0;

        intsetBigIteratorInit(a, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            if (!first) {
                if (databoxCompare((databox *)&prev, (databox *)&val) >= 0) {
                    ERR("Iteration order not sorted after merge!");
                }
            }
            prev = val;
            first = false;
            count++;
        }

        if (count != 4) {
            ERR("Expected 4 elements in iteration, got %zu!", count);
        }
    }

    /* ====================================================================
     * COMPREHENSIVE MULTI-BUCKET INTERSECT TESTS
     * ==================================================================== */

    TEST("intsetBigIntersect - multiple buckets, partial overlap") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

        /* A has buckets 0, 1, 2 */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(200));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100));

        /* B has buckets 1, 2, 3 */
        intsetBigAdd(b,
                     &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100)); /* matches A */
        intsetBigAdd(b,
                     &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 200)); /* no match */
        intsetBigAdd(
            b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100)); /* matches A */
        intsetBigAdd(
            b, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100)); /* no match */

        size_t count = intsetBigIntersect(a, b, result);

        if (count != 2) {
            ERR("Expected intersection count 2, got %zu!", count);
        }
        if (intsetBigCountElements(result) != 2) {
            ERR("Expected 2 elements in result, got %zu!",
                intsetBigCountElements(result));
        }

        /* Verify correct values */
        databoxBig v1 = DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100);
        databoxBig v2 = DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100);
        if (!intsetBigExists(result, &v1) || !intsetBigExists(result, &v2)) {
            ERR("Intersection missing expected values!");
        }
    }

    TEST("intsetBigIntersect - in-place (a == result) matches fresh result") {
        /* Layout exercising every in-place branch: a leading a-only
         * bucket, an interleaved match, a bucket whose element
         * intersection is EMPTY (same bucket, different elements), a
         * second match, and trailing a-only buckets past b's last. */
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(50)); /* bucket 0: a-only */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 10)); /* match */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 20));
        intsetBigAdd(
            a, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 5)); /* empty isect */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 7)); /* match */
        intsetBigAdd(a,
                     &DATABOX_BIG_UNSIGNED(4 * BUCKET_SIZE + 1)); /* trailing */
        intsetBigAdd(a,
                     &DATABOX_BIG_UNSIGNED(5 * BUCKET_SIZE + 2)); /* trailing */

        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 10));
        intsetBigAdd(
            b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 6)); /* no overlap */
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 7));

        /* Oracle: fresh-result intersect */
        cleaner(intsetBigFree) intsetBig *fresh = intsetBigNew();
        const size_t freshCount = intsetBigIntersect(a, b, fresh);

        /* In-place intersect on a copy of 'a' */
        cleaner(intsetBigFree) intsetBig *inplace = intsetBigCopy(a);
        const size_t inplaceCount = intsetBigIntersect(inplace, b, inplace);

        if (inplaceCount != freshCount) {
            ERR("in-place count %zu != fresh count %zu!", inplaceCount,
                freshCount);
        }

        if (intsetBigCountElements(inplace) != intsetBigCountElements(fresh)) {
            ERR("in-place elements %zu != fresh elements %zu!",
                intsetBigCountElements(inplace), intsetBigCountElements(fresh));
        }

        /* Exact set equality both directions */
        if (!intsetBigSubset(inplace, fresh) ||
            !intsetBigSubset(fresh, inplace)) {
            ERR("in-place intersection differs from fresh intersection!");
        }

        /* The known intersection: exactly the two matching values */
        assert(intsetBigCountElements(inplace) == 2);
        databoxBig m1 = DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 10);
        databoxBig m2 = DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 7);
        assert(intsetBigExists(inplace, &m1));
        assert(intsetBigExists(inplace, &m2));
    }

    TEST("intsetBigIntersect - completely disjoint buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

        /* A has buckets 0, 2, 4 */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(4 * BUCKET_SIZE + 100));

        /* B has buckets 1, 3, 5 */
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(5 * BUCKET_SIZE + 100));

        size_t count = intsetBigIntersect(a, b, result);

        if (count != 0) {
            ERR("Expected intersection count 0, got %zu!", count);
        }
        if (intsetBigCountElements(result) != 0) {
            ERR("Expected 0 elements in result, got %zu!",
                intsetBigCountElements(result));
        }
    }

    TEST("intsetBigIntersect - same bucket different values") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

        /* A and B have same bucket but no overlapping values */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(200));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(300));

        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(400));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(500));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(600));

        size_t count = intsetBigIntersect(a, b, result);

        if (count != 0) {
            ERR("Expected 0, got %zu!", count);
        }
    }

    TEST("intsetBigIntersect - negative bucket overlap") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

        /* A has negative and positive buckets */
        intsetBigAdd(a, &DATABOX_BIG_SIGNED(-100));
        intsetBigAdd(a, &DATABOX_BIG_SIGNED(-(int64_t)(BUCKET_SIZE + 50)));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));

        /* B overlaps on negative bucket */
        intsetBigAdd(b, &DATABOX_BIG_SIGNED(-100)); /* matches */
        intsetBigAdd(b, &DATABOX_BIG_SIGNED(-(int64_t)(2 * BUCKET_SIZE + 50)));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(200));

        size_t count = intsetBigIntersect(a, b, result);

        if (count != 1) {
            ERR("Expected 1, got %zu!", count);
        }

        databoxBig expected = DATABOX_BIG_SIGNED(-100);
        if (!intsetBigExists(result, &expected)) {
            ERR("Missing expected intersection value!");
        }
    }

    /* ====================================================================
     * COMPREHENSIVE MULTI-BUCKET EQUAL/SUBSET TESTS
     * ==================================================================== */

    TEST("intsetBigEqual - multiple buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Add same values across multiple buckets */
        uint64_t vals[] = {100, BUCKET_SIZE + 50, 2 * BUCKET_SIZE + 200,
                           5 * BUCKET_SIZE + 1000};

        for (size_t i = 0; i < countof(vals); i++) {
            intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(vals[i]));
            intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(vals[i]));
        }

        if (!intsetBigEqual(a, b)) {
            ERR("Sets with same multi-bucket values should be equal!");
        }

        /* Add one more to a */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100));
        if (intsetBigEqual(a, b)) {
            ERR("Sets with different bucket counts should not be equal!");
        }
    }

    TEST("intsetBigSubset - across multiple buckets") {
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* B has elements across buckets 0, 1, 2, 3 */
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(200));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100));
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(3 * BUCKET_SIZE + 100));

        /* A has subset from buckets 0 and 2 */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(2 * BUCKET_SIZE + 100));

        if (!intsetBigSubset(a, b)) {
            ERR("A should be subset of B!");
        }

        /* Add element not in B */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(4 * BUCKET_SIZE + 999));
        if (intsetBigSubset(a, b)) {
            ERR("A with extra bucket should not be subset of B!");
        }
    }

    /* ====================================================================
     * FUZZER TESTS FOR SET OPERATIONS
     * ==================================================================== */

    TEST("fuzzer - MergeInto correctness across many buckets") {
        srandom(11111);

        for (int trial = 0; trial < 20; trial++) {
            cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *reference = intsetBigNew();

            /* Generate random elements across many buckets */
            size_t aCount = 10 + (random() % 50);
            size_t bCount = 10 + (random() % 50);

            for (size_t i = 0; i < aCount; i++) {
                uint64_t bucket = random() % 20;
                uint64_t offset = random() % 1000;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                databoxBig box = DATABOX_BIG_UNSIGNED(val);
                intsetBigAdd(a, &box);
                intsetBigAdd(reference, &box);
            }

            for (size_t i = 0; i < bCount; i++) {
                uint64_t bucket = random() % 20;
                uint64_t offset = random() % 1000;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                databoxBig box = DATABOX_BIG_UNSIGNED(val);
                intsetBigAdd(b, &box);
                intsetBigAdd(reference, &box);
            }

            intsetBigMergeInto(a, b);

            /* Verify merge result equals reference */
            if (!intsetBigEqual(a, reference)) {
                ERR("Trial %d: Merge result doesn't match reference!", trial);
                ERR("  a has %zu elements, reference has %zu",
                    intsetBigCountElements(a),
                    intsetBigCountElements(reference));
            }

            /* Verify all reference elements exist in merged set */
            intsetBigIterator iter;
            databoxBig val;
            intsetBigIteratorInit(reference, &iter);
            while (intsetBigIteratorNextBox(&iter, &val)) {
                if (!intsetBigExists(a, &val)) {
                    ERR("Trial %d: Merged set missing element!", trial);
                }
            }
        }
    }

    TEST("fuzzer - Intersect correctness across many buckets") {
        srandom(22222);

        for (int trial = 0; trial < 20; trial++) {
            cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *result = intsetBigNew();

            /* Generate random elements */
            size_t aCount = 20 + (random() % 100);
            size_t bCount = 20 + (random() % 100);

            for (size_t i = 0; i < aCount; i++) {
                uint64_t bucket = random() % 15;
                uint64_t offset = random() % 500;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(val));
            }

            for (size_t i = 0; i < bCount; i++) {
                uint64_t bucket = random() % 15;
                uint64_t offset = random() % 500;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(val));
            }

            size_t count = intsetBigIntersect(a, b, result);

            /* Verify every element in result exists in both A and B */
            intsetBigIterator iter;
            databoxBig val;
            size_t verifiedCount = 0;

            intsetBigIteratorInit(result, &iter);
            while (intsetBigIteratorNextBox(&iter, &val)) {
                if (!intsetBigExists(a, &val)) {
                    ERR("Trial %d: Intersection element not in A!", trial);
                }
                if (!intsetBigExists(b, &val)) {
                    ERR("Trial %d: Intersection element not in B!", trial);
                }
                verifiedCount++;
            }

            if (verifiedCount != count) {
                ERR("Trial %d: Count mismatch %zu vs %zu!", trial,
                    verifiedCount, count);
            }

            /* Verify no element in both A and B is missing from result */
            intsetBigIteratorInit(a, &iter);
            while (intsetBigIteratorNextBox(&iter, &val)) {
                if (intsetBigExists(b, &val)) {
                    if (!intsetBigExists(result, &val)) {
                        ERR("Trial %d: Element in both A and B missing from "
                            "result!",
                            trial);
                    }
                }
            }
        }
    }

    TEST("fuzzer - MergeInto with signed values across buckets") {
        srandom(33333);

        for (int trial = 0; trial < 15; trial++) {
            cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *b = intsetBigNew();
            cleaner(intsetBigFree) intsetBig *reference = intsetBigNew();

            /* Generate random signed values across positive and negative
             * buckets */
            size_t aCount = 10 + (random() % 40);
            size_t bCount = 10 + (random() % 40);

            for (size_t i = 0; i < aCount; i++) {
                int64_t bucket = (random() % 20) - 10; /* -10 to +9 */
                int64_t offset = random() % 1000;
                int64_t val;
                if (bucket < 0) {
                    val = bucket * (int64_t)BUCKET_SIZE - offset;
                } else {
                    val = bucket * (int64_t)BUCKET_SIZE + offset;
                }
                databoxBig box = DATABOX_BIG_SIGNED(val);
                intsetBigAdd(a, &box);
                intsetBigAdd(reference, &box);
            }

            for (size_t i = 0; i < bCount; i++) {
                int64_t bucket = (random() % 20) - 10;
                int64_t offset = random() % 1000;
                int64_t val;
                if (bucket < 0) {
                    val = bucket * (int64_t)BUCKET_SIZE - offset;
                } else {
                    val = bucket * (int64_t)BUCKET_SIZE + offset;
                }
                databoxBig box = DATABOX_BIG_SIGNED(val);
                intsetBigAdd(b, &box);
                intsetBigAdd(reference, &box);
            }

            intsetBigMergeInto(a, b);

            if (intsetBigCountElements(a) !=
                intsetBigCountElements(reference)) {
                ERR("Trial %d: Element count mismatch after signed merge!",
                    trial);
            }

            /* Verify sorted order preserved */
            intsetBigIterator iter;
            databoxBig val, prev = {};
            bool first = true;

            intsetBigIteratorInit(a, &iter);
            while (intsetBigIteratorNextBox(&iter, &val)) {
                if (!first) {
                    if (databoxCompare((databox *)&prev, (databox *)&val) >=
                        0) {
                        ERR("Trial %d: Sort order violated after signed merge!",
                            trial);
                    }
                }
                prev = val;
                first = false;
            }
        }
    }

    TEST("fuzzer - sequential operations stress test") {
        srandom(44444);
        intsetBig *sets[5];

        for (int i = 0; i < 5; i++) {
            /* @own: container; free=intsetBigFree */
            sets[i] = intsetBigNew();
        }

        /* Perform many random operations */
        for (int op = 0; op < 500; op++) {
            int opType = random() % 4;
            int setIdx = random() % 5;

            switch (opType) {
            case 0: { /* Add random value */
                uint64_t bucket = random() % 30;
                uint64_t offset = random() % 2000;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                intsetBigAdd(sets[setIdx], &DATABOX_BIG_UNSIGNED(val));
                break;
            }
            case 1: { /* Merge two sets */
                int other = random() % 5;
                if (other != setIdx) {
                    cleaner(intsetBigFree) intsetBig *copy =
                        intsetBigCopy(sets[other]);
                    intsetBigMergeInto(sets[setIdx], copy);
                }
                break;
            }
            case 2: { /* Intersect two sets */
                int other = random() % 5;
                if (other != setIdx &&
                    intsetBigCountElements(sets[setIdx]) > 0 &&
                    intsetBigCountElements(sets[other]) > 0) {
                    /* @own: container; free=intsetBigFree */
                    intsetBig *result = intsetBigNew();
                    intsetBigIntersect(sets[setIdx], sets[other], result);
                    intsetBigFree(sets[setIdx]);
                    sets[setIdx] = result;
                }
                break;
            }
            case 3: { /* Remove random value */
                uint64_t bucket = random() % 30;
                uint64_t offset = random() % 2000;
                uint64_t val = bucket * BUCKET_SIZE + offset;
                intsetBigRemove(sets[setIdx], &DATABOX_BIG_UNSIGNED(val));
                break;
            }
            }
        }

        /* Verify all sets are in valid state */
        for (int i = 0; i < 5; i++) {
            /* Check iteration works and is sorted */
            intsetBigIterator iter;
            databoxBig val, prev = {};
            bool first = true;
            size_t count = 0;

            intsetBigIteratorInit(sets[i], &iter);
            while (intsetBigIteratorNextBox(&iter, &val)) {
                if (!first) {
                    if (databoxCompare((databox *)&prev, (databox *)&val) >=
                        0) {
                        ERR("Set %d: Sort order violated after stress test!",
                            i);
                    }
                }
                prev = val;
                first = false;
                count++;
            }

            if (count != intsetBigCountElements(sets[i])) {
                ERR("Set %d: Count mismatch %zu vs %zu!", i, count,
                    intsetBigCountElements(sets[i]));
            }

            intsetBigFree(sets[i]);
        }
    }

    TEST("fuzzer - 128-bit values in multi-bucket operations") {
        srandom(55555);
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* Add 128-bit values that span multiple buckets */
        for (int i = 0; i < 50; i++) {
            __uint128_t bucket = random() % 100;
            __uint128_t offset = random() % 100'000;
            __uint128_t val = bucket * BUCKET_SIZE + offset;

            /* Also add some very large 128-bit values */
            if (random() % 3 == 0) {
                val = ((__uint128_t)1 << 70) + (random() % 1'000'000);
            }

            databoxBig box;
            DATABOX_BIG_UNSIGNED_128(&box, val);
            if (random() % 2) {
                intsetBigAdd(a, &box);
            } else {
                intsetBigAdd(b, &box);
            }
        }

        /* Test merge */
        size_t aOrigCount = intsetBigCountElements(a);
        size_t bOrigCount = intsetBigCountElements(b);
        cleaner(intsetBigFree) intsetBig *aCopy = intsetBigCopy(a);

        intsetBigMergeInto(aCopy, b);

        /* Merged count should be at least max(aCount, bCount) */
        size_t mergedCount = intsetBigCountElements(aCopy);
        size_t maxOrig = aOrigCount > bOrigCount ? aOrigCount : bOrigCount;
        if (mergedCount < maxOrig) {
            ERR("Merged count %zu less than max original %zu!", mergedCount,
                maxOrig);
        }

        /* Verify all original A and B elements exist in merge */
        intsetBigIterator iter;
        databoxBig val;

        intsetBigIteratorInit(a, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            if (!intsetBigExists(aCopy, &val)) {
                ERR("128-bit merge missing element from A!");
            }
        }

        intsetBigIteratorInit(b, &iter);
        while (intsetBigIteratorNextBox(&iter, &val)) {
            if (!intsetBigExists(aCopy, &val)) {
                ERR("128-bit merge missing element from B!");
            }
        }
    }

    TEST("regression - MergeInto uses correct bucket key") {
        /* This test specifically catches the bug where key[0] was used
         * instead of key[1] when inserting B's bucket into A */
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has bucket 0 only */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));

        /* B has bucket 5 only */
        uint64_t bVal = 5 * BUCKET_SIZE + 200;
        intsetBigAdd(b, &DATABOX_BIG_UNSIGNED(bVal));

        intsetBigMergeInto(a, b);

        /* After merge, we should find the exact value from B */
        databoxBig check = DATABOX_BIG_UNSIGNED(bVal);
        if (!intsetBigExists(a, &check)) {
            ERR("Value from B's bucket not found - wrong bucket key used!");
        }

        if (intsetBigCountElements(a) != 2) {
            ERR("Expected 2 elements, got %zu!", intsetBigCountElements(a));
        }
    }

    TEST("regression - MergeInto remainder loop") {
        /* This test specifically catches the missing remainder loop bug */
        cleaner(intsetBigFree) intsetBig *a = intsetBigNew();
        cleaner(intsetBigFree) intsetBig *b = intsetBigNew();

        /* A has bucket 0 */
        intsetBigAdd(a, &DATABOX_BIG_UNSIGNED(100));

        /* B has buckets 10, 11, 12 (all beyond A's highest bucket) */
        databoxBig b10 = DATABOX_BIG_UNSIGNED(10 * BUCKET_SIZE + 100);
        databoxBig b11 = DATABOX_BIG_UNSIGNED(11 * BUCKET_SIZE + 100);
        databoxBig b12 = DATABOX_BIG_UNSIGNED(12 * BUCKET_SIZE + 100);
        intsetBigAdd(b, &b10);
        intsetBigAdd(b, &b11);
        intsetBigAdd(b, &b12);

        intsetBigMergeInto(a, b);

        /* All 4 elements must exist */
        if (intsetBigCountElements(a) != 4) {
            ERR("Expected 4 elements, got %zu (remainder loop bug)!",
                intsetBigCountElements(a));
        }

        if (!intsetBigExists(a, &b10) || !intsetBigExists(a, &b11) ||
            !intsetBigExists(a, &b12)) {
            ERR("Missing B's high bucket elements (remainder loop bug)!");
        }
    }

#undef BUCKET_SIZE

    TEST("NewFromMembers rebuilds a walked set; refuses disorder and range") {
        /* Dense positive and negative buckets, one-member buckets and
         * 128-bit buckets, walked into a flex as a snapshot does */
        cleaner(intsetBigFree) intsetBig *isb = intsetBigNew();
        for (int64_t i = -3000; i < 3000; i += 7) {
            const databoxBig v = DATABOX_BIG_SIGNED(i);
            intsetBigAdd(isb, &v);
        }
        for (int64_t i = 1; i < 40; i++) {
            const databoxBig v = DATABOX_BIG_SIGNED(i * INT64_C(-5000000001));
            intsetBigAdd(isb, &v);
            const databoxBig u = DATABOX_BIG_UNSIGNED((uint64_t)i << 40);
            intsetBigAdd(isb, &u);
            databoxBig w;
            DATABOX_BIG_SIGNED_128(&w, INTSET_BIG_INT128_MIN + i * 3);
            intsetBigAdd(isb, &w);
        }

        cleaner(flexFree) flex *walked = flexNew();
        intsetBigIterator it = {};
        intsetBigIteratorInit(isb, &it);
        databoxBig v = {};
        while (intsetBigIteratorNextBox(&it, &v)) {
            flexPushDataboxBigExact(&walked, &v, FLEX_ENDPOINT_TAIL);
            v = (databoxBig){};
        }

        cleaner(intsetBigFree) intsetBig *back =
            intsetBigNewFromMembers(walked);
        if (!back || !intsetBigEqual(isb, back) ||
            intsetBigCountElements(back) != intsetBigCountElements(isb)) {
            ERR("A walked set did not rebuild equal");
        }

        /* Disorder: the walk backwards */
        cleaner(flexFree) flex *reversed = flexNew();
        intsetBigIteratorInit(isb, &it);
        while (intsetBigIteratorNextBox(&it, &v)) {
            flexPushDataboxBigExact(&reversed, &v, FLEX_ENDPOINT_HEAD);
            v = (databoxBig){};
        }
        /* Out of range: past INTSET_BIG_UINT128_MAX */
        cleaner(flexFree) flex *wide = flexNew();
        databoxBig huge;
        DATABOX_BIG_UNSIGNED_128(&huge, INTSET_BIG_UINT128_MAX + 1);
        flexPushDataboxBigExact(&wide, &huge, FLEX_ENDPOINT_TAIL);
        cleaner(intsetBigFree) intsetBig *bad1 =
            intsetBigNewFromMembers(reversed);
        cleaner(intsetBigFree) intsetBig *bad2 = intsetBigNewFromMembers(wide);
        if (bad1 || bad2) {
            ERR("Disordered or out-of-range members were accepted");
        }
    }

    TEST("Repr: empty and holding narrow and wide members") {
        intsetBig *isb = intsetBigNew();
        intsetBigRepr(isb);
        databoxBig v;
        DATABOX_BIG_SIGNED_128(&v, -((__int128_t)1 << 90));
        intsetBigAdd(isb, &v);
        DATABOX_BIG_UNSIGNED_128(&v, 42);
        intsetBigAdd(isb, &v);
        intsetBigRepr(isb);
        intsetBigFree(isb);
    }

    TEST_FINAL_RESULT;
}
#endif
