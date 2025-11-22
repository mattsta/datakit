#include "multidict.h"

#include "../deps/xxHash/xxhash.h"
#include "asmUtils.h"
#include "datakit.h"
#include "timeUtil.h"

#include <assert.h>
#include <limits.h>
#include <sys/time.h>

#ifndef NDEBUG
#include <stdio.h>
#endif

/* ====================================================================
 * Data Holders
 * ==================================================================== */

/* This is our hash table structure.
 * Every multidictionary has two multidictHT because of incremental rehashing.
 * 'multidictHT' is an 8+4+4+8+8=32 byte struct */
typedef struct multidictHT {
    multidictSlot **table; /* table[i] is pointer to hash slot 'i'. */
    uint32_t size;         /* length of *table allocations above
                            *  (hash slot count; power of 2) */
    uint32_t oversize;     /* number of slots over the median size */
    uint64_t count; /* number of elements in this HT across all hash slots */
    uint64_t usedBytes; /* sum of all uncompressed slot sizes. */
                        /* POTENTIALLY keep count of number of slots OVER
                         * our basic size limit.
                         * When count > percent over threshold, THEN rehash.
                         * After every rehash, find the smallest slot byte size,
                         * and re-normalize the "threshold"
                         * so we don't end up with 100% rehash if people insert
                         * ONLY large elements above our threshold.
                         * If all elements > our base threshold, each slot only
                         * holds one element. */
} multidictHT;

/* 'multidict' is a 96 byte struct:
 *   - ht[2] = 32 bytes * 2 = 64 bytes
 *   - *type = 8 bytes
 *   - *sharedClass = 8 bytes
 *   - rehashidx = 4 bytes
 *   - iterators = 4 bytes
 *   - bitfield = 4 bytes
 *   - TOTAL: 124 bytes. */
struct multidict {
    multidictHT ht[2];
    multidictType *type;
    multidictClass *shared;
    uint32_t rehashidx; /* table idx for ht[0]; invalid if rehashing==0 */
    uint32_t iterators; /* number of iterators currently running */
    uint32_t forceResizeRatio : 8; /* not sure if needed anymore? */
    uint32_t seed : 20;            /* seed for hash functions */
    uint32_t compress : 1;         /* BOOL. allow compression? */
    uint32_t rehashing : 1;        /* BOOL. true if currently rehashing. */
    uint32_t unused : 2;           /* available */
};

/* ====================================================================
 * Repetitive Helpers
 * ==================================================================== */
#define MULTIDICT_MEDIAN_TARGET_CONTAINER_BYTES 8192

#define HT(d, htidx) (&(d)->ht[(htidx)])
#define HTSIZE(d, htidx) (HT((d), (htidx))->size) /* number of slots */
/* MASK is SIZE-1, and SIZE is power of 2, so MASK is all 1s */
#define HTMASK(d, htidx) ((HT((d), (htidx))->size) - 1)
#define HTCOUNT(d, htidx) (HT((d), (htidx))->count) /* total # of elements */
#define HTBYTES(d, htidx) (HT((d), (htidx))->usedBytes)
#define SLOT(d, htidx, slot) (HT(d, htidx)->table[slot])
#define SLOT_IDX(d, htidx, hash) ((hash) & (HTMASK(d, htidx)))
#define SLOT_BY_HASH_PTR(d, htidx, hash)                                       \
    (SLOT(d, htidx, SLOT_IDX(d, htidx, hash)))
#define SLOT_BY_HASH_PTR_PTR(d, htidx, hash) (&SLOT_BY_HASH_PTR(d, htidx, hash))

#define RESIZE_DISABLED(d) (d->shared->disableResize)

/* ====================================================================
 * Hash Functions
 * ==================================================================== */
/* Thomas Wang's 32 bit Mix Function */
uint32_t multidictIntHashFunction(uint32_t key) {
    key += ~(key << 15);
    key ^= (key >> 10);
    key += (key << 3);
    key ^= (key >> 6);
    key += ~(key << 11);
    key ^= (key >> 16);
    return key;
}

/* Thomas Wang's 64 bit Mix Function */
DK_STATIC uint64_t multidictMixLongLong(uint64_t key) {
    key += ~(key << 32);
    key ^= (key >> 22);
    key += ~(key << 13);
    key ^= (key >> 8);
    key += (key << 3);
    key ^= (key >> 15);
    key += ~(key << 27);
    key ^= (key >> 31);
    return key;
}

uint32_t multidictLongLongHashFunction(uint64_t key) {
    return multidictMixLongLong(key);
}

#define SEED_MAX (1 << 20)
bool multidictSetHashFunctionSeed(multidict *d, uint32_t seed) {
    d->seed = seed; /* we set it even if we are outside requested bounds */
    if (seed > SEED_MAX) {
        return false;
    } else {
        return true;
    }
}

uint32_t multidictGetHashFunctionSeed(multidict *d) {
    return d->seed;
}

uint32_t multidictGenHashFunction(multidict *d, const void *key, int32_t len) {
    if (DK_BITS == 64) {
        return XXH64(key, len, d->seed);
    } else {
        return XXH32(key, len, d->seed);
    }
}

/* And an ASCII case insensitive hash function (based on djb hash) */
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len) {
    uint32_t hash = d->seed;

    while (len--) {
        hash = ((hash << 5) + hash) + (DK_LOWER(*buf++)); /* hash * 33 + c */
    }

    return hash;
}

/* ====================================================================
 * Private API
 * ==================================================================== */
#define multidictResetHT(ht)                                                   \
    do {                                                                       \
        memset(ht, 0, sizeof(*ht));                                            \
    } while (0)

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * multidictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
#define multidictRehashStep(d)                                                 \
    do {                                                                       \
        if (multidictIsRehashing(d) && !(d)->iterators)                        \
            multidictRehash(d, 1);                                             \
    } while (0)

DK_STATIC multidictSlot **multidictSlotForKey(multidict *d, bool onlyLatest,
                                              const databox *keybox,
                                              int *htidx) {
    bool rehashing = multidictIsRehashing(d); /* hack; false = HT 0, true = 1 */

    /* If our SIZE is 1, then we only have one hash slot and
     * and can return it directly when either a.) we want ONLY LATEST
     * (latest is defined as '1' when rehashing, '0' otherwise) or
     * b.) we are not rehashing and can use HT '0' directly. */
    if ((onlyLatest || !rehashing) && HTSIZE(d, rehashing) == 1) {
        if (htidx) {
            *htidx = rehashing;
        }

        return &SLOT(d, rehashing, 0);
    }

    /* else, we look up the slot by hash value first by checking the
     * 'rehashing' table then by checking the other table (if requested) */
    uint8_t *key;
    size_t klen;
    databoxGetBytes((databox *)keybox, &key, &klen);

    uint32_t hash = multidictHashKey(d, key, klen);
#if 0
        printf("Hash for key %.*s is %u\n", klen, key, hash);
        printf("Selected table will be (0): %d\n", ((hash) & (HT(d, 0)->sizemask)));
        printf("Sizemask is: %d\n", HTMASK(d, 0));
#endif

    /* Initial search is always the new table if we are rehashing. */
    int useSlot = rehashing;
    multidictSlot **foundSlot = SLOT_BY_HASH_PTR_PTR(d, useSlot, hash);

    /* If we didn't find a slot and we are rehashing,
     * then check the other table (if it's requested that we check
     * other than the latest table) */
    if (!onlyLatest && (!*foundSlot && rehashing)) {
        useSlot = !!rehashing; /* try the opposite of rehashing now */
        foundSlot = SLOT_BY_HASH_PTR_PTR(d, useSlot, hash);
    }

    if (htidx) {
        *htidx = useSlot;
    }

    return foundSlot;
}

DK_STATIC void *multidictOperate(multidict *d, multidictOp *op,
                                 const databox *keybox, bool onlyLatest,
                                 void *(*operate)(multidictClass *qdc,
                                                  multidictSlot **slot,
                                                  const databox *keybox)) {
    multidictRehashStep(d);

    /* If we find an existing slot, then it hasn't been re-hash'd away yet.
     * So, if we find a slot, then it is safe to use. */

    /* If we are NOT rehashing and we do NOT find a slot, we may need to
     * create one.  If we ARE rehashing and we DO NOT find a slot in
     * table 0, then we search (and optionally create) in table 1. */
    int htidx;
    multidictSlot **foundSlot =
        multidictSlotForKey(d, onlyLatest, keybox, &htidx);
    void *result = operate(d->shared, foundSlot, keybox);

    if (op) {
        op->result = result;
        op->ht = HT(d, htidx);
        op->d = d;
    }

    return result;
}

DK_STATIC multidictSlot *multidictFindSlotForKeyNewest(multidict *d,
                                                       multidictOp *op,
                                                       const databox *keybox) {
    return multidictOperate(d, op, keybox, true,
                            d->shared->operationSlotGetOrCreate);
}

DK_STATIC multidictSlot *multidictFindSlotForKeyAny(multidict *d,
                                                    multidictOp *op,
                                                    const databox *keybox) {
    return multidictOperate(d, op, keybox, false,
                            d->shared->operationSlotGetOrCreate);
}

DK_STATIC bool _multidictDelete(multidict *d, multidictOp *op,
                                const databox *keybox) {
    return !!(uintptr_t)multidictOperate(d, op, keybox, false,
                                         d->shared->operationRemove);
}

/* A fingerprint is a 64 bit number that represents the state of the
 * multidictionary at a given time, it's just a few multidict
 * properties xored together.
 * When an unsafe iterator is initialized, we get multidict fingerprint,
 * and check the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the multidictionary while iterating.
 */
DK_STATIC uint64_t multidictFingerprint(const multidict *d) {
    uint64_t integers[6], hash = 0;
    int32_t j;

    integers[0] = (long)d->ht[0].table;
    integers[1] = HTSIZE(d, 0);
    integers[2] = HTCOUNT(d, 0);
    integers[3] = (long)d->ht[1].table;
    integers[4] = HTSIZE(d, 1);
    integers[5] = HTCOUNT(d, 1);

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        hash = multidictMixLongLong(hash);
    }

    return hash;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
DK_STATIC uint64_t rev(uint64_t v) {
    uint64_t s = 8 * sizeof(v); /* bit size; must be power of 2 */
    uint64_t mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }

    return v;
}

#if 0
/* Expand the hash table if needed */
DK_STATIC bool multidictExpandIfNeeded_(multidict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (multidictIsRehashing(d)) {
        return false;
    }

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/slots is over the "safe" threshold, we resize doubling
     * the number of slots. */

    return false;

    /* CURRENTLY NO REHASH DURING TESTING */
    /* need more elaborate "determine rehash threshold" metrics. */
    if (HTCOUNT(d, 0) >= HTSIZE(d, 0) &&
        (!RESIZE_DISABLED(d) ||
         HTCOUNT(d, 0) / HTSIZE(d, 0) > d->forceResizeRatio)) {
        return multidictExpand(d, HTCOUNT(d, 0) * 2);
    }

    return false;
}
#endif

/* (1 << 31) is the largest power of 2 in a 32 bit unsigned integer.
 * (1 << 31) == 2^31 == 1 billion(ish) */
/* Returns the next highest power of 2 for 'size'.
 * Note: This code considers 1 a power of 2.
 *       (e.g. 0 -> 1; 1 -> 2; 2 -> 4; ...; 512 -> 1024; ...) */
DK_INLINE_ALWAYS uint64_t _multidictNextPower(const uint64_t size) {
    uint64_t i = size + 1; /* we need the next highest power above _size_ */
    return pow2Ceiling64(i);
}

/* ====================================================================
 * User API
 * ==================================================================== */
multidict *multidictNew(multidictType *type, multidictClass *qdc,
                        int32_t seed) {
    if (!type || !qdc) {
        return NULL;
    }

    multidict *d = zcalloc(1, sizeof(*d));

    multidictResetHT(HT(d, 0));
    multidictResetHT(HT(d, 1));

    d->type = type;
    d->shared = qdc;
    d->rehashidx = 123456789;
    d->rehashing = false;
    d->iterators = 0;
    d->forceResizeRatio = 5;
    d->seed = seed;

    multidictExpand(d, 0); /* create ht[0] */
    return d;
}

/* Allow class retrieval so we can create new dicts using
 * existing classes since they are meant to be shared anyway. */
multidictClass *multidictGetClass(multidict *d) {
    return d->shared;
}

/* Resize table to minimal size containing all elements,
 * but with USED/slots ratio near <= 1 */
bool multidictResize(multidict *d) {
    if (RESIZE_DISABLED(d) || multidictIsRehashing(d)) {
        return false;
    }

    int32_t minimal = 0;
    if (HTSIZE(d, 0) > 0) {
        float avgZiplistSz = HTBYTES(d, 0) / HTSIZE(d, 0);
        /* We want: new size so after we rehash, ideally each slot is
         * at 50% of MULTIDICT_MEDIAN_TARGET_CONTAINER_BYTES */
        /* For that, we also have to track how many "megaentries" we have
         * and their average size, so we can subtract that from the
         * overall expectation calculation of how big we can expect
         * our resulting ziplists to become. */
        /* Given SIZE ziplists use avgZiplistSz bytes each, we want
         * to make size so avg = MULTIDICT_MEDIAN_TARGET_CONTAINER_BYTES/2 */
        minimal = avgZiplistSz / (MULTIDICT_MEDIAN_TARGET_CONTAINER_BYTES / 2);
    }

    return multidictExpand(d, minimal);
}

/* Expand or create the hash table */
bool multidictExpand(multidict *d, uint64_t newSlots) {
    /* Deny expand if currently expanding */
    if (multidictIsRehashing(d)) {
        return false;
    }

    /* Allocate the new hash table and initialize all pointers to NULL */
    multidictHT n = {0};
    multidictResetHT(&n);
    n.size = _multidictNextPower(newSlots);

#if 1
    char *which;
    if (n.size > 0 && HTSIZE(d, 0) == 0) {
        which = "Creating";
    } else if (n.size > HTSIZE(d, 0)) {
        which = "Expanding to";
    } else if (n.size == HTSIZE(d, 0)) {
        which = "Remaining the same at";
    } else {
        which = "Shrinking down to";
    }

    printf("%s %" PRIu32 " slot%s!\n", which, n.size, n.size > 1 ? "s" : "");
#endif

    /* If next power is the same as current size; can't grow => fail. */
    if (n.size == HTSIZE(d, 0)) {
        return false;
    }

    n.table = zcalloc(1, n.size * sizeof(*n.table));

    /* If first Expand of this multidictionary, initialize HT 0 */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n; /* copy n to ht[0] */
        return false;
    }

    /* Prepare second hash table for incremental rehashing */
    d->ht[1] = n; /* copy n to ht[1] */
    d->rehashidx = 0;
    d->rehashing = true;
    return true; /* true = we are now rehashing */
}

/* Performs N steps of incremental rehashing. Returns true if there are still
 * keys to move from the old to the new hash table, otherwise false is returned.
 * Note that a rehashing step consists in moving a slot (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single slot. */
bool multidictRehash(multidict *d, int32_t n) {
    static int rehasher = 0;
    printf("\t\t\tSOMEONE IS REHASHING! %d\n", rehasher++);

    if (!multidictIsRehashing(d)) {
        return d->rehashing;
    }

    multidictClass *shared = d->shared;
    while (n-- > 0 && HTCOUNT(d, 0) > 0) {
        /* rehashidx must never grow above HTSIZE-1 */
        assert(d->rehashidx < HTSIZE(d, 0));

        /* Test for dead slots.
         * If we have large ranges of NULL slots, we want to jump past
         * them as efficiently as possible instead of needing to call
         * this function over and over again just to do nothing. */
        uint32_t emptyVisits = n * 5; /* Max empty slots to visit. */
        multidictSlot *current;
        while ((current = SLOT(d, 0, d->rehashidx)) == NULL) {
            d->rehashidx++;
            emptyVisits--;

            /* Sanity check for rehash index not extending beyond slots */
            assert(d->rehashidx < HTSIZE(d, 0));

            if (emptyVisits == 0) {
                /* return true because not done rehashing. */
                return d->rehashing;
            }
        }
        /* At this point, we are *guaranteed* 'current' is NOT null */

        /* Now migrate the last entry of 'current' into its new slot in
         * HT 1.  Repeat until 'current' has no remaining elements.
         * This should be efficient since, on a well behaved multidict class,
         * 'current' will fit entirely into L1 cache. */
        databox keybox = {{0}};
        bool freeCurrentContainer = true;

        /* We check 'current' in the while condition because if we move
         * the entire slot, we need to abort the while loop with no cleanup. */
        while (current && shared->lastKey(current, &keybox)) {
            uint8_t *key;
            size_t klen;
            databoxGetBytes(&keybox, &key, &klen);

            /* Get target multimap for new hash slot; created on-demand below */
            multidictSlot **target =
                SLOT_BY_HASH_PTR_PTR(d, 1, multidictHashKey(d, key, klen));

            /* All slots should be previously allocated. Target must exist. */
            assert(target);

            /* Update HT counts; single ops; elements migrate one-by-one */
            HTCOUNT(d, 0)--;
            HTCOUNT(d, 1)++;

            uint32_t slotCount = shared->countSlot(current);

            /* If our slotCount is somehow zero, we're broken, beacuse
             * we just read a key from this slot! */
            assert(slotCount > 0);

            /* If current slot ONLY has one entry AND target
             * also doesn't exist yet, just move entire HT 0 slot
             * instead of {create, copy, free original} into HT 1. */
            if ((slotCount == 1) && (!*target)) {
                printf("Moving single for %.*s and slot size: %" PRIu32 "\n",
                       (int)klen, key, shared->countSlot(current));
                *target = current;
                freeCurrentContainer = false;
                current = NULL; /* breaks loop next time around */
            } else {
                /* Else, if target doesn't exist, materialize a new slot. */
                if (!*target) {
                    *target = shared->createSlot();
                }
                /* Move last key to 'target' from 'current' */
                shared->migrateLast(*target, current);
            }
        }

        /* Only free 'current' if we completely emptied it.
         * Otherwise, we just moved 'current' and don't want to delete it. */
        if (freeCurrentContainer) {
            /* 'current' is now empty because the while loop exited. */
            shared->freeSlot(shared, current);
            current = NULL;
        }

        /* Set slot to NULL for easier debugging/tracking/introspection. */
        SLOT(d, 0, d->rehashidx) = NULL;

        /* Increment rehash index for next iteration. */
        d->rehashidx++;
    }

    /* If rehashed everything in HT 0,
     *   - cleanup HT 0
     *   - move HT 1 to HT 0
     *   - reset HT 1
     *   - reset rehashing metadata*/
    if (HTCOUNT(d, 0) == 0) {
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        multidictResetHT(HT(d, 1));
        d->rehashidx = 123456789;
        d->rehashing = false;
    }

    /* Returns 'true' when still rehashing;
     * Returns 'false' when rehashing is complete. */
    return d->rehashing;
}

/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
int64_t multidictRehashMilliseconds(multidict *d, int64_t ms) {
    int64_t start = timeUtilMs();
    int32_t rehashes = 0;

    while (multidictRehash(d, 10)) {
        rehashes += 10;
        if (((int64_t)timeUtilMs() - start) > ms) {
            break;
        }
    }
    return rehashes;
}

/* Add new element to the target hash table.
 * Returns true if element added successfully. */
int64_t multidictAdd(multidict *d, const databox *keybox,
                     const databox *valbox) {
    multidictOp op;
    //    printf("Finding slot for key: %s\n", keybox->data.bytes.start);
    multidictFindSlotForKeyNewest(d, &op, keybox);
    //    multidictOpRepr(&op);

    const uint32_t lenBefore = d->shared->sizeBytes(op.result);

#if 0
    /* Check if we should expand the hash table before adding */
    if (!multidictIsRehashing(d)) {
        uint32_t keybytes;
        uint32_t valbytes;
        databoxGetSize(keybox, &keybytes);
        databoxGetSize(valbox, &valbytes);
        /* 'additiveGuess' is a rough estimate since it doesn't take into
         * account exact ziplist overhead.
         * Note: this also over estimates in the case we are just replacing
         *       a value.  It will both double count the 'key' as being added
         *       again, even though we keep the key and just replace the value,
         *       and it doesn't account for the size difference from the old
         *       value being deleted and the new value being inserted.
         * + 4 is because of estimating 2 bytes ZL metadata for K and V */
        uint64_t additiveGuess = lenBefore + keybytes + valbytes + 4;
        /* FIXME: this doesn't account for slots containing elements naturally
         * bigger than our target size, so this could result in infinite
         * expansion attempts. */
        if (additiveGuess > MULTIDICT_MEDIAN_TARGET_CONTAINER_BYTES) {
            if (!RESIZE_DISABLED(d)) {
                multidictExpand(d, HTSIZE(d, 0) + 1);

                /* Now we need to re-get our target HT in 'op' now because
                 * just expanded the hash space and are now inserting in HT 1 */
                multidictFindSlotForKeyNewest(d, &op, keybox);
                lenBefore = d->shared->sizeBytes(op.result);
            }
        }
    }
#endif

    const int64_t result =
        d->shared->insertByType(d->shared, op.result, keybox, valbox);
    const uint32_t lenAfter = d->shared->sizeBytes(op.result);

    /* Adjust 'usedBytes' by actual added amount. */
    op.ht->usedBytes += (lenAfter - lenBefore);
    op.ht->count++;

    return result;
}

/* Search and remove an element */
bool multidictDelete(multidict *d, const databox *keybox) {
    if (HTSIZE(d, 0) == 0) {
        return false; /* d->ht[0].table is NULL */
    }

    multidictOp op;
    bool deleted = _multidictDelete(d, &op, keybox);

    if (deleted) {
        op.ht->count--;
    }

    return deleted;
}

/* Destroy an entire multidictionary */
void _multidictClearHt(multidict *d, multidictHT *ht) {
    /* Free all slots */
    for (uint32_t i = 0; i < ht->size; i++) {
        ht->count -= d->shared->freeSlot(d->shared, ht->table[i]);
    }

    /* Free the table array itself. */
    zfree(ht->table);

    /* Reset the table to guard against future usage. */
    multidictResetHT(ht);
}

/* Clear & Free the hash table */
void multidictFree(multidict *d) {
    if (!d) {
        return;
    }

    _multidictClearHt(d, HT(d, 0));
    _multidictClearHt(d, HT(d, 1));
    zfree(d);
}

/* Returns value in inout param 'val' */
bool multidictFind(multidict *d, const databox *keybox, databox *valbox) {
    if (HTSIZE(d, 0) == 0) {
        return false;
    }

    /* Find slot for 'key' */
    multidictSlot *target = multidictFindSlotForKeyAny(d, NULL, keybox);

    return d->shared->findValueByKey(d->shared, target, keybox, valbox);
}

bool multidictFindByString(multidict *d, char *key, uint8_t **val) {
    databox keybox = databoxNewBytesString(key);
    databox valbox = {{0}};
    multidictFind(d, &keybox, &valbox);
    size_t nolen;

    return databoxGetBytes(&valbox, val, &nolen);
}

bool multidictIteratorInit(multidict *d, multidictIterator *iter) {
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = false;
    return true;
}

bool multidictIteratorGetSafe(multidict *d, multidictIterator *iter) {
    multidictIteratorInit(d, iter);
    iter->safe = true;
    return true;
}

bool multidictIteratorNext(multidictIterator *iter, multidictEntry *e) {
    multidictClass *shared = iter->d->shared;
    while (true) {
        if (!iter->current) {
            multidictHT *ht = HT(iter->d, iter->table);
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe) {
                    iter->d->iterators++;
                } else {
                    iter->fingerprint = multidictFingerprint(iter->d);
                }
            }

            iter->index++; /* index into the current HT */
            /* If the iterator index has grown larger than the current HT,
             * switch to the other HT. */
            if (iter->index >= ht->size) {
                if (multidictIsRehashing(iter->d) && iter->table == 0) {
                    /* If we're on table 0 and rehashing, start iterating 1. */
                    iter->table++;
                    iter->index = 0;
                    ht = HT(iter->d, 1);
                } else {
                    /* Else, we're done. We did HT 0 and 1. Game over. */
                    return false;
                }
            }

            iter->current = ht->table[iter->index];
            shared->getIter(iter->current, &iter->iter);
            /* EMIT FIRST ITERATION OF THIS SLOT */
        } else {
            /* EMIT NEXT ITERATION OF CURRENT SLOT */
        }

        if (iter->current && shared->iterNext(&iter->iter, e)) {
            return true;
        }

        /* Loop again so the top level picks up the next HT or
         * terminates this iteration by returning NULL. */
        iter->current = NULL;
    }
}

void multidictIteratorRelease(multidictIterator *iter) {
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe) {
            iter->d->iterators--;
        } else {
            assert(iter->fingerprint == multidictFingerprint(iter->d));
        }
    }
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
bool multidictGetRandomKey(multidict *d, databox *keybox) {
    multidictSlot *current = NULL;
    int32_t listlen;

    if (multidictSize(d) == 0) {
        return false;
    }

    multidictRehashStep(d);

    if (multidictIsRehashing(d)) {
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            uint32_t h =
                d->rehashidx +
                (random() % (HTSIZE(d, 0) + HTSIZE(d, 1) - d->rehashidx));
            current = (h >= HTSIZE(d, 0)) ? HT(d, 1)->table[h - HTSIZE(d, 0)]
                                          : HT(d, 0)->table[h];
        } while (!current);
    } else {
        do {
            current = HT(d, 0)->table[SLOT_IDX(d, 0, random())];
        } while (!current);
    }

    /* Now we found a non empty slot, so count the elements and
     * select a random index. */
    listlen = d->shared->countSlot(current);

    int32_t listele = random() % listlen;
    return d->shared->findKeyByPosition(d->shared, current, listele, keybox);
}

#if 0
/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned int j; /* internal hash table id, 0 or 1. */
    unsigned int tables; /* 1 or 2 tables? */
    unsigned int stored = 0, maxsizemask;
    unsigned int maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d)) {
            _dictRehashStep(d);
        } else {
            break;
        }
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask) {
        maxsizemask = d->ht[1].sizemask;
    }

    /* Pick a random point inside the larger table. */
    unsigned int i = random() & maxsizemask;
    unsigned int emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * slots, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size) i = d->rehashidx;
                continue;
            }
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty slots, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
/* TODO FIX ME */
/*
 * How to return "keys" inside a map when the map can be recompressed?
 * Just copy the keys?  The keys can also be strings or integers or floats */
                while (he) {
                    /* Collect all the elements of the slots found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}
#endif

/* multidictScan() is used to iterate over the elements of a multidictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * multidictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the multidictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * multidict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old slot: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new slots you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the slot 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given slot, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
uint64_t multidictScan(multidict *d, uint64_t v, multidictScanFunction *fn,
                       void *privdata) {
    multidictHT *t0;
    multidictHT *t1;
    multidictSlot *current;
    uint64_t m0;
    uint64_t m1;

    if (multidictSize(d) == 0) {
        return 0;
    }

    multidictClass *shared = d->shared;
    if (!multidictIsRehashing(d)) {
        t0 = HT(d, 0);
        m0 = HTMASK(d, 0);

        current = t0->table[v & m0];
        shared->iterateAll(shared, current, fn, privdata);
    } else {
        t0 = HT(d, 0);
        t1 = HT(d, 1);

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = HT(d, 1);
            t1 = HT(d, 0);
        }

        m0 = HTMASK(d, 1);
        m1 = HTMASK(d, 0);

        /* Emit entries at cursor */
        current = t0->table[v & m0];
        shared->iterateAll(shared, current, fn, privdata);

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            current = t1->table[v & m1];
            shared->iterateAll(shared, current, fn, privdata);

            /* Increment bits not covered by the smaller mask */
            v = (((v | m0) + 1) & ~m0) | (v & m0);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /* Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits of the smaller table */
    v |= ~m0;

    /* Increment the reverse cursor */
    v = rev(v);
    v++;
    v = rev(v);

    return v;
}

void multidictEmpty(multidict *d) {
    _multidictClearHt(d, HT(d, 0));
    _multidictClearHt(d, HT(d, 1));
    d->rehashidx = 123456789;
    d->rehashing = false;
    d->iterators = 0;
}

void multidictResizeEnable(multidict *d) {
    d->shared->disableResize = false;
}

void multidictResizeDisable(multidict *d) {
    d->shared->disableResize = true;
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST
#define MULTIDICT_STATS_VECTLEN 50
static void multidictPrintStatsHt_(multidict *d, multidictHT *ht) {
    uint64_t i, slots = 0, chainlen, maxchainlen = 0;
    uint64_t totchainlen = 0;
    uint64_t clvector[MULTIDICT_STATS_VECTLEN] = {0};

    if (ht->count == 0) {
        printf("No stats available for empty multidictionaries\n");
        return;
    }

    for (i = 0; i < ht->size; i++) {
        multidictSlot *current;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }

        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        current = ht->table[i];

        chainlen += d->shared->countSlot(current);

        clvector[(chainlen < MULTIDICT_STATS_VECTLEN)
                     ? chainlen
                     : (MULTIDICT_STATS_VECTLEN - 1)]++;

        if (chainlen > maxchainlen) {
            maxchainlen = chainlen;
        }

        totchainlen += chainlen;
    }

    printf("Hash table stats:\n");
    printf(" table size: %" PRIu32 "\n", ht->size);
    printf(" number of elements: %" PRIu64 "\n", ht->count);
    printf(" different slots: %" PRIi64 "\n", slots);
    printf(" max chain length: %" PRIi64 "\n", maxchainlen);
    printf(" avg chain length (counted): %.02f\n", (float)totchainlen / slots);
    printf(" avg chain length (computed): %.02f\n", (float)ht->count / slots);
    printf(" Chain length distribution:\n");

    for (i = 0; i < MULTIDICT_STATS_VECTLEN - 1; i++) {
        if (clvector[i] == 0) {
            continue;
        }

        printf("   %s%" PRIu64 ": %" PRIu64 " (%.02f%%)\n",
               (i == MULTIDICT_STATS_VECTLEN - 1) ? ">= " : "", i, clvector[i],
               ((float)clvector[i] / ht->size) * 100);
    }
}

void multidictPrintStats(multidict *d) {
    multidictPrintStatsHt_(d, &d->ht[0]);
    if (multidictIsRehashing(d)) {
        printf("-- Rehashing into ht[1]:\n");
        multidictPrintStatsHt_(d, &d->ht[1]);
    }
}
#endif

/* ====================================================================
 * Public Shared Hash Types
 * ==================================================================== */
DK_STATIC uint32_t multidictStringHashFunction_(multidict *d, const void *key,
                                                uint32_t len) {
    return multidictGenHashFunction(d, key, len);
}

DK_STATIC int32_t multidictStringKeyCompare_(void *privdata, const void *key1,
                                             const uint32_t k1len,
                                             const void *key2,
                                             const uint32_t k2len) {
    DK_NOTUSED(privdata);
    DK_NOTUSED(k2len);

    return strncmp(key1, key2, k1len) == 0;
}

DK_STATIC uint32_t multidictStringCaseHashFunction_(multidict *d,
                                                    const void *key,
                                                    uint32_t len) {
    return multidictGenCaseHashFunction(d, key, len);
}

multidictType multidictTypeExactKey = {
    multidictStringHashFunction_, /* hash function */
    multidictStringKeyCompare_    /* key compare */
};

multidictType multidictTypeCaseKey = {
    multidictStringCaseHashFunction_, /* hash function */
    multidictStringKeyCompare_        /* key compare */
};

/* ====================================================================
 * Debugging
 * ==================================================================== */
void multidictRepr(const multidict *d) {
    printf("multidict %p has:\n", (void *)d);
    for (int i = 0; i < 2; i++) {
        printf("\tHT %d:\n", i);
        printf("\t\tSIZE: %" PRIu32 "\n", HTSIZE(d, i));
        printf("\t\tCOUNT: %" PRIu64 "\n", HTCOUNT(d, i));
        printf("\t\tUSED BYTES: %" PRIu64 "\n", HTBYTES(d, i));
    }
}

void multidictOpRepr(const multidictOp *op) {
    printf("multidictOp %p has:\n", (void *)op);
    printf("\tht: %p\n", (void *)op->ht);
    printf("\tresult: %p\n", op->result);
}

/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2016, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
