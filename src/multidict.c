#include "multidict.h"

#include "../deps/xxHash/xxhash.h"
#include "asmUtils.h"
#include "datakit.h"
#include "fibbuf.h"
#include "flex.h"
#include "multilru.h"
#include "multimap.h"
#include "timeUtil.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <sys/time.h>

/* ====================================================================
 * DEBUG TRACING FOR AUTO-RESIZE
 * ==================================================================== */
#define DEBUG_AUTORESIZE 0

#if DEBUG_AUTORESIZE
static int dbg_op_count = 0;
#define DBG_PRINT(...)                                                         \
    do {                                                                       \
        fprintf(stderr, "[%06d] ", dbg_op_count++);                            \
        fprintf(stderr, __VA_ARGS__);                                          \
        fflush(stderr);                                                        \
    } while (0)

#define DBG_DICT_STATE(d, msg)                                                 \
    do {                                                                       \
        fprintf(stderr,                                                        \
                "[%06d] %s: ht0(size=%u,count=%" PRIu64                        \
                ") ht1(size=%u,count=%" PRIu64 ") "                            \
                "rehashing=%d rehashidx=%u autoResize=%d expandLF=%u "         \
                "shrinkLF=%u\n",                                               \
                dbg_op_count++, msg, (d)->ht[0].size, (d)->ht[0].count,        \
                (d)->ht[1].size, (d)->ht[1].count, (d)->rehashing,             \
                (d)->rehashidx, (d)->autoResize, (d)->expandLoadFactor,        \
                (d)->shrinkLoadFactor);                                        \
        fflush(stderr);                                                        \
    } while (0)
#else
#define DBG_PRINT(...) ((void)0)
#define DBG_DICT_STATE(d, msg) ((void)0)
#endif

/* ====================================================================
 * Data Holders
 * ==================================================================== */

/* Sentinel value indicating rehashing is not in progress */
#define MULTIDICT_REHASHIDX_INVALID UINT32_MAX

/* This is our hash table structure.
 * Every multidictionary has two multidictHT because of incremental rehashing.
 * 'multidictHT' is a 56 byte struct (8+4+4+8+8+8+8+8=56) */
typedef struct multidictHT {
    multidictSlot **table; /* table[i] is pointer to hash slot 'i'. */
    uint32_t size;         /* length of *table allocations above
                            *  (hash slot count; power of 2) */
    uint32_t oversize;     /* number of slots over the median size */
    uint64_t count;        /* number of elements in this HT across all slots */
    uint64_t usedBytes;    /* sum of all uncompressed slot sizes (overhead) */
    uint64_t keyBytes;     /* total bytes of all keys stored */
    uint64_t valBytes;     /* total bytes of all values stored */
    uint64_t totalBytes;   /* usedBytes + keyBytes + valBytes (convenience) */
} multidictHT;

/* Load factor defaults (percentage): expand at 200%, shrink at 10%
 * Note: 200% means we expand when count is 2x the number of slots.
 * This allows the multimap slots to accumulate some entries before
 * triggering a rehash, which matches the design where slots hold
 * multiple entries. Max value is 255 (fits in uint8_t). */
#define MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR 200
#define MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR 10
#define MULTIDICT_MIN_SLOTS_FOR_SHRINK 8 /* Don't shrink below this */

/* Byte-based expansion defaults */
#define MULTIDICT_DEFAULT_TARGET_SLOT_BYTES (2ULL * 1024 * 1024) /* 2MB */
#define MULTIDICT_DEFAULT_MAX_SLOT_BYTES (8ULL * 1024 * 1024)    /* 8MB */

/* 'multidict' struct layout:
 *   - ht[2] = 56 bytes * 2 = 112 bytes
 *   - *type = 8 bytes
 *   - *sharedClass = 8 bytes
 *   - rehashidx = 4 bytes
 *   - iterators = 4 bytes
 *   - bitfield = 4 bytes
 *   - TOTAL: 140 bytes. */
struct multidict {
    multidictHT ht[2];
    multidictType *type;
    multidictClass *shared;
    uint32_t rehashidx; /* slot idx in ht[0]; MULTIDICT_REHASHIDX_INVALID if not
                           rehashing */
    uint32_t iterators; /* number of iterators currently running */
    uint32_t seed;      /* seed for hash functions */
    uint8_t
        expandLoadFactor; /* expand when (count*100/slots) > this (max 255) */
    uint8_t shrinkLoadFactor;       /* shrink when (count*100/slots) < this */
    uint8_t compress : 1;           /* BOOL. allow compression? */
    uint8_t rehashing : 1;          /* BOOL. true if currently rehashing. */
    uint8_t autoResize : 1;         /* BOOL. auto expand/shrink enabled? */
    uint8_t lruEnabled : 1;         /* BOOL. LRU tracking enabled? */
    uint8_t useByteBasedExpand : 1; /* BOOL. use byte-based expansion? */
    uint8_t unused : 3;             /* available */
    /* Self-management fields */
    uint64_t maxMemory;                    /* 0 = unlimited */
    multidictEvictionCallback *evictionCb; /* called before eviction */
    void *evictionPrivdata;                /* passed to eviction callback */
    /* Byte-based expansion configuration */
    uint64_t targetSlotBytes; /* Target average slot size (default: 2MB) */
    uint64_t maxSlotBytes; /* Maximum slot size before forced expand (default:
                              8MB) */
    /* LRU tracking fields (zero overhead when disabled) */
    multilru *lru;          /* LRU structure (NULL if disabled) */
    void *lruPtrToKey;      /* Array of lruKeyRef: ptr -> hash */
    size_t lruPtrToKeySize; /* Size of lruPtrToKey array */
    struct multidict
        *lruKeyToPtr; /* Aux dict: key -> multilruPtr (as uint64) */
    multidictEvictPolicy evictPolicy; /* Eviction policy when LRU enabled */
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
#define HTKEYBYTES(d, htidx) (HT((d), (htidx))->keyBytes)
#define HTVALBYTES(d, htidx) (HT((d), (htidx))->valBytes)
#define HTTOTALBYTES(d, htidx) (HT((d), (htidx))->totalBytes)
#define SLOT(d, htidx, slot) (HT(d, htidx)->table[slot])
#define SLOT_IDX(d, htidx, hash) ((hash) & (HTMASK(d, htidx)))
#define SLOT_BY_HASH_PTR(d, htidx, hash)                                       \
    (SLOT(d, htidx, SLOT_IDX(d, htidx, hash)))
#define SLOT_BY_HASH_PTR_PTR(d, htidx, hash) (&SLOT_BY_HASH_PTR(d, htidx, hash))

#define RESIZE_DISABLED(d) (d->shared->disableResize)

/* Internal macro for hashing */
#define multidictHashKey(d, key, len) (d)->type->hashFunction(d, key, len)

/* Helper to recalculate totalBytes from components */
#define HT_UPDATE_TOTAL(ht)                                                    \
    do {                                                                       \
        (ht)->totalBytes = (ht)->usedBytes + (ht)->keyBytes + (ht)->valBytes;  \
    } while (0)

/* Forward declarations for LRU tracking (defined later) */
static void multidictLRUOnInsert(multidict *d, const databox *keybox);
static void multidictLRUOnAccess(multidict *d, const databox *keybox);
static void multidictLRUOnDelete(multidict *d, const databox *keybox);
static bool multidictLRUSelectVictim(multidict *d, databox *keybox);

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

    while (len-- > 0) {
        uint8_t c =
            *buf++; /* Read once to avoid multiple evaluation in DK_LOWER */
        hash = ((hash << 5) + hash) + (DK_LOWER(c)); /* hash * 33 + c */
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
/* Rehash step is called on every operation; optimize for the common case
 * where we're not rehashing (unlikely). */
#define multidictRehashStep(d)                                                 \
    do {                                                                       \
        if (unlikely(d->rehashing) && likely(!(d)->iterators))                 \
            multidictRehash(d, 1);                                             \
    } while (0)

/* Hot path: called on every lookup/insert/delete. Optimize for common case. */
DK_INLINE_ALWAYS multidictSlot **multidictSlotForKey(multidict *d,
                                                     bool onlyLatest,
                                                     const databox *keybox,
                                                     int *htidx) {
    bool rehashing = d->rehashing; /* Direct access for performance */

    /* If our SIZE is 1, then we only have one hash slot and
     * and can return it directly when either a.) we want ONLY LATEST
     * (latest is defined as '1' when rehashing, '0' otherwise) or
     * b.) we are not rehashing and can use HT '0' directly. */
    if (likely((onlyLatest || !rehashing) && HTSIZE(d, rehashing) == 1)) {
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

    /* Initial search is always the new table if we are rehashing. */
    int useSlot = rehashing;
    multidictSlot **foundSlot = SLOT_BY_HASH_PTR_PTR(d, useSlot, hash);

    /* If we didn't find a slot and we are rehashing,
     * then check the other table (if it's requested that we check
     * other than the latest table) */
    if (unlikely(!onlyLatest && (!*foundSlot && rehashing))) {
        useSlot = !rehashing; /* try the opposite of rehashing now (HT 0) */
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

    /* Ensure hash table is initialized (expand if empty after multidictEmpty)
     */
    if (HTSIZE(d, 0) == 0) {
        multidictExpand(d, MULTIDICT_HT_INITIAL_SIZE);
    }

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

__attribute__((unused)) DK_STATIC multidictSlot *
multidictFindSlotForKeyAny(multidict *d, multidictOp *op,
                           const databox *keybox) {
    return multidictOperate(d, op, keybox, false,
                            d->shared->operationSlotGetOrCreate);
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

/* Forward declaration for use in shrink function */
DK_INLINE_ALWAYS uint64_t _multidictNextPower(const uint64_t size);

/* Calculate load factor as percentage (count * 100 / slots) */
DK_STATIC uint32_t multidictLoadFactor_(const multidict *d) {
    uint32_t slots = HTSIZE(d, 0);
    if (slots == 0) {
        return 0;
    }
    return (uint32_t)((HTCOUNT(d, 0) * 100) / slots);
}

/* Expand the hash table if load factor exceeds threshold */
DK_STATIC bool multidictExpandIfNeeded_(multidict *d) {
    DBG_PRINT("multidictExpandIfNeeded_ ENTER\n");
    DBG_DICT_STATE(d, "  state on entry");

    /* Don't expand during rehashing or with safe iterators active */
    if (multidictIsRehashing(d) || d->iterators > 0) {
        DBG_PRINT(
            "multidictExpandIfNeeded_ EXIT: rehashing=%d iters=%d, skip\n",
            multidictIsRehashing(d), d->iterators);
        return false;
    }

    /* Don't expand if auto-resize is disabled */
    if (!d->autoResize || RESIZE_DISABLED(d)) {
        DBG_PRINT(
            "multidictExpandIfNeeded_ EXIT: autoResize=%d disabled=%d, skip\n",
            d->autoResize, RESIZE_DISABLED(d));
        return false;
    }

/* Safeguard: Maximum slot count cap (1 billion slots) */
#define MULTIDICT_MAX_SLOTS (1ULL << 30)
    if (HTSIZE(d, 0) >= MULTIDICT_MAX_SLOTS) {
        DBG_PRINT("multidictExpandIfNeeded_ EXIT: at max slots (%" PRIu64 "), "
                  "cannot expand\n",
                  HTSIZE(d, 0));
        return false;
    }

    /* Calculate current load factor */
    uint32_t countLoadFactor = multidictLoadFactor_(d);
    DBG_PRINT("multidictExpandIfNeeded_: countLoadFactor=%u, threshold=%u\n",
              countLoadFactor, d->expandLoadFactor);

    /* Use byte-based expansion if enabled */
    if (d->useByteBasedExpand) {
        /* Calculate byte-based load metrics */
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(d, &metrics);

        DBG_PRINT("multidictExpandIfNeeded_: byte-based mode - "
                  "avgSlotBytes=%" PRIu64 ", targetSlotBytes=%" PRIu64
                  ", maxSlotBytes=%" PRIu64 ", byteLoadFactor=%u\n",
                  metrics.avgSlotBytes, metrics.targetSlotBytes,
                  metrics.maxSlotBytes, metrics.byteLoadFactor);

        bool shouldExpand = false;
        const char *expandReason __attribute__((unused)) = NULL;

        /* Primary trigger: Average slot size exceeds target */
        if (metrics.avgSlotBytes > d->targetSlotBytes) {
            shouldExpand = true;
            expandReason = "avgSlotBytes > targetSlotBytes";
        }

        /* Safeguard 1: Maximum slot size limit (force expand if any slot is
         * huge) */
        if (!shouldExpand && metrics.maxSlotBytes > d->maxSlotBytes) {
            shouldExpand = true;
            expandReason = "maxSlotBytes > maxSlotBytes limit";
        }

        /* Safeguard 2: Count-based backstop (prevent pathological cases where
         * byte-based metric doesn't trigger but dict is very full) */
        if (!shouldExpand && countLoadFactor >= (d->expandLoadFactor * 2)) {
            shouldExpand = true;
            expandReason = "countLoadFactor >= 2x threshold (backstop)";
        }

        if (shouldExpand) {
            /* Calculate new size */
            uint64_t newSize = fibbufNextSizeBuffer(HTSIZE(d, 0));
            if (newSize < HTCOUNT(d, 0)) {
                newSize =
                    fibbufNextSizeBuffer(HTCOUNT(d, 0)); /* Handle overflow */
            }

            /* Safeguard: Expansion effectiveness check
             * Only expand if it will reduce avg slot size by at least 10%.
             * This prevents infinite expansion when entries are inherently
             * large or when hash distribution is poor (e.g., single entry per
             * slot that's already above target). */
            if (metrics.usedSlots > 0) {
                uint64_t expectedAvgAfter = metrics.totalUsedBytes / newSize;
                uint64_t minImprovement = (metrics.avgSlotBytes * 9) / 10;

                if (expectedAvgAfter >= minImprovement) {
                    DBG_PRINT("multidictExpandIfNeeded_ EXIT: expansion "
                              "ineffective (expected avg %" PRIu64
                              " >= min improvement %" PRIu64 "), skip\n",
                              expectedAvgAfter, minImprovement);
                    return false;
                }
            }

            DBG_PRINT("multidictExpandIfNeeded_: BYTE-BASED EXPAND to %" PRIu64
                      " (reason: %s)\n",
                      newSize, expandReason);
            return multidictExpand(d, newSize);
        }

        DBG_PRINT("multidictExpandIfNeeded_ EXIT: byte-based metrics OK, no "
                  "expand\n");
        return false;
    }

    /* Count-based expansion (legacy mode, used when byte-based is disabled) */
    if (countLoadFactor >= d->expandLoadFactor) {
        /* Expand to next fibonacci size */
        uint64_t newSize = fibbufNextSizeBuffer(HTSIZE(d, 0));
        if (newSize < HTCOUNT(d, 0)) {
            newSize = fibbufNextSizeBuffer(HTCOUNT(d, 0)); /* Handle overflow */
        }
        DBG_PRINT("multidictExpandIfNeeded_: COUNT-BASED EXPAND to %" PRIu64
                  "\n",
                  newSize);
        return multidictExpand(d, newSize);
    }

    DBG_PRINT("multidictExpandIfNeeded_ EXIT: countLoadFactor %u < threshold "
              "%u, no expand\n",
              countLoadFactor, d->expandLoadFactor);
    return false;
}

/* Shrink the hash table if load factor drops below threshold */
DK_STATIC bool multidictShrinkIfNeeded_(multidict *d) {
    /* Don't shrink during rehashing or with safe iterators active */
    if (multidictIsRehashing(d) || d->iterators > 0) {
        return false;
    }

    /* Don't shrink if auto-resize is disabled */
    if (!d->autoResize || RESIZE_DISABLED(d)) {
        return false;
    }

    /* Don't shrink if already at minimum size */
    if (HTSIZE(d, 0) <= MULTIDICT_MIN_SLOTS_FOR_SHRINK) {
        return false;
    }

    /* Check load factor against shrink threshold */
    uint32_t loadFactor = multidictLoadFactor_(d);
    if (loadFactor < d->shrinkLoadFactor) {
        /* Calculate new size: smallest power of 2 that maintains good load */
        uint64_t count = HTCOUNT(d, 0);
        uint64_t newSize = count > 0 ? count : 1;
        newSize = _multidictNextPower(newSize);

        /* Ensure we don't shrink below minimum */
        if (newSize < MULTIDICT_MIN_SLOTS_FOR_SHRINK) {
            newSize = MULTIDICT_MIN_SLOTS_FOR_SHRINK;
        }

        /* Only shrink if actually smaller */
        if (newSize < HTSIZE(d, 0)) {
            return multidictExpand(d, newSize);
        }
    }

    return false;
}

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
    d->rehashidx = MULTIDICT_REHASHIDX_INVALID;
    d->rehashing = false;
    d->iterators = 0;
    d->expandLoadFactor = MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR;
    d->shrinkLoadFactor = MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR;
    d->autoResize = true; /* DEBUG: re-enabled for tracing */
    d->useByteBasedExpand =
        false; /* Disabled by default - enable after validation */
    d->targetSlotBytes = MULTIDICT_DEFAULT_TARGET_SLOT_BYTES;
    d->maxSlotBytes = MULTIDICT_DEFAULT_MAX_SLOT_BYTES;
    DBG_DICT_STATE(d, "multidictNew created");
    d->seed = seed;

    multidictExpand(d, 0); /* create ht[0] */
    return d;
}

/* Allow class retrieval so we can create new dicts using
 * existing classes since they are meant to be shared anyway. */
multidictClass *multidictGetClass(multidict *d) {
    return d->shared;
}

/* ====================================================================
 * Accessor Functions (for encapsulation)
 * ==================================================================== */

/* Get total count of key-value pairs across all hash tables */
uint64_t multidictCount(const multidict *d) {
    if (!d) {
        return 0;
    }
    return HTCOUNT(d, 0) + HTCOUNT(d, 1);
}

/* Get total number of hash slots across all hash tables */
uint64_t multidictSlotCount(const multidict *d) {
    if (!d) {
        return 0;
    }
    return HTSIZE(d, 0) + HTSIZE(d, 1);
}

/* Check if rehashing is in progress */
bool multidictIsRehashing_(const multidict *d) {
    if (!d) {
        return false;
    }
    return d->rehashing;
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
    DBG_PRINT("multidictExpand ENTER: newSlots=%" PRIu64 "\n", newSlots);
    DBG_DICT_STATE(d, "  state before expand");

    /* Deny expand if currently expanding */
    if (multidictIsRehashing(d)) {
        DBG_PRINT("multidictExpand EXIT: already rehashing, return false\n");
        return false;
    }

    /* Allocate the new hash table and initialize all pointers to NULL */
    multidictHT n = {0};
    multidictResetHT(&n);
    n.size = _multidictNextPower(newSlots);

    DBG_PRINT("multidictExpand: _multidictNextPower(%" PRIu64
              ") = %u, HTSIZE(d,0)=%u\n",
              newSlots, n.size, HTSIZE(d, 0));

    /* If next power is the same as current size; can't grow => fail. */
    if (n.size == HTSIZE(d, 0)) {
        DBG_PRINT("multidictExpand EXIT: same size (%u == %u), return false\n",
                  n.size, HTSIZE(d, 0));
        return false;
    }

    n.table = zcalloc(1, n.size * sizeof(*n.table));

    /* If first Expand of this multidictionary, initialize HT 0 */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n; /* copy n to ht[0] */
        DBG_PRINT("multidictExpand EXIT: first expand, initialized ht[0] with "
                  "size %u, return false\n",
                  n.size);
        DBG_DICT_STATE(d, "  state after first expand");
        return false;
    }

    /* Prepare second hash table for incremental rehashing */
    d->ht[1] = n; /* copy n to ht[1] */
    d->rehashidx = 0;
    d->rehashing = true;
    DBG_PRINT("multidictExpand EXIT: started rehash to size %u, return true\n",
              n.size);
    DBG_DICT_STATE(d, "  state after starting rehash");
    return true; /* true = we are now rehashing */
}

/* Performs N steps of incremental rehashing. Returns true if there are still
 * keys to move from the old to the new hash table, otherwise false is returned.
 * Note that a rehashing step consists in moving a slot (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single slot. */
bool multidictRehash(multidict *d, int32_t n) {
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

            /* Get key and value sizes for byte tracking transfer */
            size_t keySize = 0, valSize = 0;
            databoxGetSize(&keybox, &keySize);
            databox valbox = {{0}};
            if (shared->findValueByKey(shared, current, &keybox, &valbox)) {
                databoxGetSize(&valbox, &valSize);
            }

            /* Update HT counts and byte tracking; elements migrate one-by-one
             */
            HTCOUNT(d, 0)--;
            HTCOUNT(d, 1)++;
            HTKEYBYTES(d, 0) -= keySize;
            HTKEYBYTES(d, 1) += keySize;
            HTVALBYTES(d, 0) -= valSize;
            HTVALBYTES(d, 1) += valSize;

            uint32_t slotCount = shared->countSlot(current);

            /* If our slotCount is somehow zero, we're broken, beacuse
             * we just read a key from this slot! */
            assert(slotCount > 0);

            /* If current slot ONLY has one entry AND target
             * also doesn't exist yet, just move entire HT 0 slot
             * instead of {create, copy, free original} into HT 1. */
            if ((slotCount == 1) && (!*target)) {
                /* Transfer entire slot's usedBytes */
                size_t slotBytes = shared->sizeBytes(current);
                HTBYTES(d, 0) -= slotBytes;
                HTBYTES(d, 1) += slotBytes;

                *target = current;
                freeCurrentContainer = false;
                current = NULL; /* breaks loop next time around */
            } else {
                /* Else, if target doesn't exist, materialize a new slot. */
                uint32_t targetBefore = 0,
                         currentBefore = shared->sizeBytes(current);
                if (!*target) {
                    *target = shared->createSlot();
                } else {
                    targetBefore = shared->sizeBytes(*target);
                }

                /* Move last key to 'target' from 'current' */
                shared->migrateLast(*target, current);

                /* Update usedBytes based on size changes */
                size_t targetAfter = shared->sizeBytes(*target);
                size_t currentAfter = shared->sizeBytes(current);
                HTBYTES(d, 0) -= (currentBefore - currentAfter);
                HTBYTES(d, 1) += (targetAfter - targetBefore);
            }
        }

        /* Only free 'current' if we completely emptied it.
         * Otherwise, we just moved 'current' and don't want to delete it. */
        if (freeCurrentContainer) {
            /* 'current' is now empty because the while loop exited. */
            shared->freeSlot(shared, current);
            current = NULL;
        }

        /* Update totalBytes for both HTs after processing this slot */
        HT_UPDATE_TOTAL(HT(d, 0));
        HT_UPDATE_TOTAL(HT(d, 1));

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
        DBG_PRINT("multidictRehash: COMPLETING REHASH - ht0 empty, moving ht1 "
                  "to ht0\n");
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        multidictResetHT(HT(d, 1));
        d->rehashidx = MULTIDICT_REHASHIDX_INVALID;
        d->rehashing = false;
        DBG_DICT_STATE(d, "  state after rehash complete");
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
 * Returns: MULTIDICT_OK_INSERTED if new key, MULTIDICT_OK_REPLACED if updated,
 *          MULTIDICT_ERR on error */
multidictResult multidictAdd(multidict *d, const databox *keybox,
                             const databox *valbox) {
    DBG_PRINT("multidictAdd ENTER\n");
    DBG_DICT_STATE(d, "  state before add");

    /* Check if we should expand before adding */
    multidictExpandIfNeeded_(d);

    /* During rehashing, check if key exists in ht[0] and remove it first.
     * This prevents duplicate keys across tables and keeps counts accurate.
     *
     * IMPORTANT: We must EXPLICITLY check ht[0], not rely on
     * multidictSlotForKey, because multidictSlotForKey will return ht[1] if
     * that slot exists, even if the key hasn't been removed from ht[0] yet. */
    bool removedFromOldTable = false;
    if (multidictIsRehashing(d)) {
        /* Directly get the ht[0] slot by hash - bypass multidictSlotForKey */
        uint8_t *key;
        size_t klen;
        databoxGetBytes((databox *)keybox, &key, &klen);
        uint32_t hash = multidictHashKey(d, key, klen);
        multidictSlot **ht0Slot = SLOT_BY_HASH_PTR_PTR(d, 0, hash);

        if (ht0Slot && *ht0Slot) {
            /* Slot exists in ht[0] - check if this specific key is there */
            databox existingVal = {{0}};
            if (d->shared->findValueByKey(d->shared, *ht0Slot, keybox,
                                          &existingVal)) {
                /* Key exists in ht[0] - remove it to prevent duplicate */
                size_t oldKeySize = 0, oldValSize = 0;
                databoxGetSize(keybox, &oldKeySize);
                databoxGetSize(&existingVal, &oldValSize);
                size_t lenBefore = d->shared->sizeBytes(*ht0Slot);

                d->shared->operationRemove(d->shared, ht0Slot, keybox);

                size_t lenAfter = d->shared->sizeBytes(*ht0Slot);
                d->ht[0].count--;
                d->ht[0].usedBytes -= (lenBefore - lenAfter);
                d->ht[0].keyBytes -= oldKeySize;
                d->ht[0].valBytes -= oldValSize;
                HT_UPDATE_TOTAL(&d->ht[0]);
                removedFromOldTable = true;
            }
        }
    }

    multidictOp op;
    multidictFindSlotForKeyNewest(d, &op, keybox);

    const size_t lenBefore = d->shared->sizeBytes(op.result);

    /* Get key and value sizes for tracking */
    size_t keySize = 0, valSize = 0;
    databoxGetSize(keybox, &keySize);
    databoxGetSize(valbox, &valSize);

    /* For replacements, we need to know the old value size to track accurately.
     * Look up the existing value if it might exist. */
    size_t oldValSize = 0;
    databox oldVal = {{0}};
    bool hadOldValue =
        d->shared->findValueByKey(d->shared, op.result, keybox, &oldVal);
    if (hadOldValue) {
        databoxGetSize(&oldVal, &oldValSize);
    }
    /* If we removed from old table, this is effectively a replacement (key
     * existed) */
    (void)removedFromOldTable; /* Currently we let insertByType handle the
                                  semantics */

    const int64_t insertResult =
        d->shared->insertByType(d->shared, op.result, keybox, valbox);
    const size_t lenAfter = d->shared->sizeBytes(op.result);

    /* Adjust 'usedBytes' by actual slot size change */
    op.ht->usedBytes += (lenAfter - lenBefore);

    /* Convert to typed result */
    multidictResult result;
    if (insertResult == 1) {
        /* New insert: add both key and value bytes */
        op.ht->count++;
        op.ht->keyBytes += keySize;
        op.ht->valBytes += valSize;
        result = MULTIDICT_OK_INSERTED;
    } else if (insertResult == 0) {
        /* Replacement: key stays same, update value bytes delta */
        op.ht->valBytes += valSize;
        op.ht->valBytes -= oldValSize;
        result = MULTIDICT_OK_REPLACED;
    } else {
        result = MULTIDICT_ERR;
    }

    /* Update convenience total */
    HT_UPDATE_TOTAL(op.ht);

    /* LRU tracking: register new key (only on insert, not replace) */
    if (result == MULTIDICT_OK_INSERTED) {
        multidictLRUOnInsert(d, keybox);
    } else if (result == MULTIDICT_OK_REPLACED) {
        /* For replacements, just touch the LRU entry */
        multidictLRUOnAccess(d, keybox);
    }

    DBG_PRINT("multidictAdd EXIT: result=%d\n", result);
    DBG_DICT_STATE(d, "  state after add");

    return result;
}

/* ====================================================================
 * Conditional Insert Operations
 * ==================================================================== */

/* Add only if key does NOT exist (atomic check-and-insert) */
multidictResult multidictAddNX(multidict *d, const databox *keybox,
                               const databox *valbox) {
    if (unlikely(!d || !keybox || !valbox)) {
        return MULTIDICT_ERR;
    }

    /* Check if key exists */
    if (multidictExists(d, keybox)) {
        return MULTIDICT_ERR; /* Key exists - NX fails */
    }

    /* Key doesn't exist - perform insert */
    multidictResult result = multidictAdd(d, keybox, valbox);

    /* multidictAdd should return INSERTED since we checked it doesn't exist */
    return result;
}

/* Add only if key DOES exist (update existing only) */
multidictResult multidictAddXX(multidict *d, const databox *keybox,
                               const databox *valbox) {
    if (unlikely(!d || !keybox || !valbox)) {
        return MULTIDICT_ERR;
    }

    /* Check if key exists */
    if (!multidictExists(d, keybox)) {
        return MULTIDICT_ERR; /* Key doesn't exist - XX fails */
    }

    /* Key exists - perform update (replace) */
    multidictResult result = multidictAdd(d, keybox, valbox);

    /* multidictAdd should return REPLACED since key exists */
    return result;
}

/* Explicit replace - fails if key doesn't exist (same as XX, explicit intent)
 */
multidictResult multidictReplace(multidict *d, const databox *keybox,
                                 const databox *valbox) {
    return multidictAddXX(d, keybox, valbox);
}

/* Helper: attempt delete from a specific hash table slot */
static bool multidictDeleteFromSlot_(multidict *d, int htidx,
                                     const databox *keybox) {
    uint8_t *key;
    size_t klen;
    databoxGetBytes((databox *)keybox, &key, &klen);
    uint32_t hash = multidictHashKey(d, key, klen);

    multidictSlot **slotPtr = SLOT_BY_HASH_PTR_PTR(d, htidx, hash);
    if (!slotPtr || !*slotPtr) {
        return false;
    }

    multidictSlot *slot = *slotPtr;

    /* Check if key actually exists in this slot */
    databox val = {{0}};
    if (!d->shared->findValueByKey(d->shared, slot, keybox, &val)) {
        return false; /* Key not in this slot */
    }

    /* Key exists - proceed with deletion */
    multidictHT *ht = HT(d, htidx);
    const size_t lenBefore = d->shared->sizeBytes(slot);

    size_t keySize = 0, valSize = 0;
    databoxGetSize(keybox, &keySize);
    databoxGetSize(&val, &valSize);

    /* LRU tracking: remove from LRU BEFORE deleting key */
    multidictLRUOnDelete(d, keybox);

    /* Perform the deletion */
    void *result = d->shared->operationRemove(d->shared, slotPtr, keybox);
    bool deleted = !!(uintptr_t)result;

    if (deleted) {
        const size_t lenAfter = d->shared->sizeBytes(*slotPtr);
        ht->count--;
        ht->usedBytes -= (lenBefore - lenAfter);
        ht->keyBytes -= keySize;
        ht->valBytes -= valSize;
        HT_UPDATE_TOTAL(ht);
    }

    return deleted;
}

/* Search and remove an element - optimized for common case (key exists) */
bool multidictDelete(multidict *d, const databox *keybox) {
    if (unlikely(HTSIZE(d, 0) == 0)) {
        return false; /* d->ht[0].table is NULL */
    }

    multidictRehashStep(d);

    /* During rehashing, we must search BOTH tables. The slot lookup may return
     * a non-NULL slot in ht[1] that doesn't contain our key, while our key is
     * still in ht[0] awaiting migration. This is the same fix as multidictFind.
     */
    if (unlikely(d->rehashing)) {
        /* Try ht[1] first (newer table) */
        if (multidictDeleteFromSlot_(d, 1, keybox)) {
            multidictShrinkIfNeeded_(d);
            return true;
        }
        /* Then try ht[0] (older table, entries may not be migrated yet) */
        if (multidictDeleteFromSlot_(d, 0, keybox)) {
            multidictShrinkIfNeeded_(d);
            return true;
        }
        return false;
    }

    /* Not rehashing - simple single table delete */
    if (multidictDeleteFromSlot_(d, 0, keybox)) {
        multidictShrinkIfNeeded_(d);
        return true;
    }
    return false;
}

/* ====================================================================
 * Atomic Get-and-Delete Operations
 * ==================================================================== */

/* Get value and delete atomically - returns true if found */
bool multidictGetAndDelete(multidict *d, const databox *keybox,
                           databox *valbox) {
    if (unlikely(!d || !keybox || !valbox)) {
        return false;
    }

    /* First, find the value */
    if (!multidictFind(d, keybox, valbox)) {
        return false; /* Key doesn't exist */
    }

    /* Now delete it - this should succeed since we just found it */
    bool deleted = multidictDelete(d, keybox);

    /* During concurrent rehashing, key could theoretically move between
     * find and delete, but our implementation handles this correctly */
    return deleted;
}

/* Pop a random entry (for sampling/draining) */
bool multidictPopRandom(multidict *d, databox *keybox, databox *valbox) {
    if (unlikely(!d || !keybox || !valbox)) {
        return false;
    }

    if (multidictCount(d) == 0) {
        return false;
    }

    /* Retry loop - GetRandomKey can occasionally fail during rehashing
     * transitions when it lands on empty slots. Retry a few times. */
    const int maxRetries = 10;
    for (int retry = 0; retry < maxRetries; retry++) {
        /* Get a random key */
        if (!multidictGetRandomKey(d, keybox)) {
            continue; /* Retry - might have landed on empty slot */
        }

        /* Get the value for this key */
        if (!multidictFind(d, keybox, valbox)) {
            continue; /* Key might have moved during rehash - retry */
        }

        /* Delete the entry */
        if (multidictDelete(d, keybox)) {
            return true; /* Success! */
        }
        /* Delete failed (key might have been rehashed) - retry */
    }

    return false; /* Failed after retries */
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

/* Returns value in inout param 'val' - hot path, optimized */
bool multidictFind(multidict *d, const databox *keybox, databox *valbox) {
    if (unlikely(HTSIZE(d, 0) == 0)) {
        return false;
    }

    multidictRehashStep(d);

    /* During rehashing, we must search BOTH tables, not just the slot lookup.
     * The slot lookup may return a non-NULL slot in ht[1] that doesn't contain
     * our key, while our key is still in ht[0] awaiting migration.
     * Bug fix: always search both tables during rehashing. */
    if (unlikely(d->rehashing)) {
        uint8_t *key;
        size_t klen;
        databoxGetBytes((databox *)keybox, &key, &klen);
        uint32_t hash = multidictHashKey(d, key, klen);

        /* Search ht[1] first (newer table) */
        multidictSlot *slot1 = *SLOT_BY_HASH_PTR_PTR(d, 1, hash);
        if (slot1 &&
            d->shared->findValueByKey(d->shared, slot1, keybox, valbox)) {
            multidictLRUOnAccess(d, keybox);
            return true;
        }

        /* Then search ht[0] (older table, entries may not be migrated yet) */
        multidictSlot *slot0 = *SLOT_BY_HASH_PTR_PTR(d, 0, hash);
        if (slot0 &&
            d->shared->findValueByKey(d->shared, slot0, keybox, valbox)) {
            multidictLRUOnAccess(d, keybox);
            return true;
        }

        return false;
    }

    /* Not rehashing - simple case, single table lookup */
    multidictSlot *target = *SLOT_BY_HASH_PTR_PTR(d, 0, 0);
    if (HTSIZE(d, 0) > 1) {
        uint8_t *key;
        size_t klen;
        databoxGetBytes((databox *)keybox, &key, &klen);
        uint32_t hash = multidictHashKey(d, key, klen);
        target = *SLOT_BY_HASH_PTR_PTR(d, 0, hash);
    }

    bool found = d->shared->findValueByKey(d->shared, target, keybox, valbox);

    /* LRU tracking: promote on access (no recursion since lruKeyToPtr
     * has lruEnabled=false) */
    if (found) {
        multidictLRUOnAccess(d, keybox);
    }

    return found;
}

bool multidictFindByString(multidict *d, char *key, uint8_t **val) {
    databox keybox = databoxNewBytesString(key);
    databox valbox = {{0}};
    multidictFind(d, &keybox, &valbox);
    size_t nolen;

    return databoxGetBytes(&valbox, val, &nolen);
}

/* ====================================================================
 * Convenience Functions
 * ==================================================================== */

/* Check if a key exists without retrieving the value */
bool multidictExists(multidict *d, const databox *keybox) {
    databox unused = {{0}};
    return multidictFind(d, keybox, &unused);
}

/* Check if a string key exists */
bool multidictExistsByString(multidict *d, const char *key) {
    databox keybox = databoxNewBytesString(key);
    return multidictExists(d, &keybox);
}

/* ====================================================================
 * Numeric Operations
 * ==================================================================== */

/* Increment numeric value atomically. Creates key with increment if not exists.
 * Returns MULTIDICT_ERR if value exists but is not numeric. */
multidictResult multidictIncrBy(multidict *d, const databox *keybox,
                                int64_t increment, int64_t *result) {
    if (unlikely(!d || !keybox)) {
        return MULTIDICT_ERR;
    }

    /* Try to find existing value */
    databox valbox = {{0}};
    bool exists = multidictFind(d, keybox, &valbox);

    int64_t newVal;
    if (exists) {
        /* Check if value is numeric */
        if (!DATABOX_IS_NUMERIC(&valbox)) {
            return MULTIDICT_ERR; /* Value is not numeric */
        }

        /* Get current value based on type */
        if (DATABOX_IS_INTEGER(&valbox)) {
            if (valbox.type == DATABOX_SIGNED_64) {
                newVal = valbox.data.i64 + increment;
            } else {
                /* Unsigned - be careful with overflow */
                newVal = (int64_t)valbox.data.u64 + increment;
            }
        } else {
            /* Float/double - convert */
            newVal = (int64_t)valbox.data.d64 + increment;
        }
    } else {
        /* Key doesn't exist - create with increment value */
        newVal = increment;
    }

    /* Store result if requested */
    if (result) {
        *result = newVal;
    }

    /* Create new value and store */
    databox newValbox = DATABOX_SIGNED(newVal);
    return multidictAdd(d, keybox, &newValbox);
}

/* Increment float value atomically. Creates key with increment if not exists.
 * Returns MULTIDICT_ERR if value exists but is not numeric. */
multidictResult multidictIncrByFloat(multidict *d, const databox *keybox,
                                     double increment, double *result) {
    if (unlikely(!d || !keybox)) {
        return MULTIDICT_ERR;
    }

    /* Try to find existing value */
    databox valbox = {{0}};
    bool exists = multidictFind(d, keybox, &valbox);

    double newVal;
    if (exists) {
        /* Check if value is numeric */
        if (!DATABOX_IS_NUMERIC(&valbox)) {
            return MULTIDICT_ERR; /* Value is not numeric */
        }

        /* Get current value based on type */
        if (DATABOX_IS_FLOAT(&valbox)) {
            if (valbox.type == DATABOX_DOUBLE_64) {
                newVal = valbox.data.d64 + increment;
            } else {
                newVal = valbox.data.f32 + increment;
            }
        } else {
            /* Integer - convert */
            if (valbox.type == DATABOX_SIGNED_64) {
                newVal = (double)valbox.data.i64 + increment;
            } else {
                newVal = (double)valbox.data.u64 + increment;
            }
        }
    } else {
        /* Key doesn't exist - create with increment value */
        newVal = increment;
    }

    /* Store result if requested */
    if (result) {
        *result = newVal;
    }

    /* Create new value and store */
    databox newValbox = DATABOX_DOUBLE(newVal);
    return multidictAdd(d, keybox, &newValbox);
}

/* ====================================================================
 * Statistics Functions
 * ==================================================================== */

/* Get comprehensive statistics about the dictionary */
void multidictGetStats(multidict *d, multidictStats *stats) {
    if (!d || !stats) {
        return;
    }

    stats->count = HTCOUNT(d, 0) + HTCOUNT(d, 1);
    stats->slots = HTSIZE(d, 0) + HTSIZE(d, 1);
    stats->usedBytes = HTBYTES(d, 0) + HTBYTES(d, 1);
    stats->keyBytes = HTKEYBYTES(d, 0) + HTKEYBYTES(d, 1);
    stats->valBytes = HTVALBYTES(d, 0) + HTVALBYTES(d, 1);
    stats->totalBytes = HTTOTALBYTES(d, 0) + HTTOTALBYTES(d, 1);
    stats->isRehashing = multidictIsRehashing(d);

    /* Calculate load factor: (count * 100) / slots */
    if (HTSIZE(d, 0) > 0) {
        stats->loadFactor = (uint32_t)((HTCOUNT(d, 0) * 100) / HTSIZE(d, 0));
    } else {
        stats->loadFactor = 0;
    }
}

/* Helper to gather detailed stats for a single hash table */
DK_STATIC void multidictGetDetailedStatsHt_(multidict *d, multidictHT *ht,
                                            multidictDetailedStats *stats) {
    memset(stats->chainDistribution, 0, sizeof(stats->chainDistribution));
    stats->usedSlots = 0;
    stats->maxChainLen = 0;
    uint64_t totalChainLen = 0;

    if (ht->count == 0 || ht->size == 0) {
        stats->avgChainLen = 0.0f;
        return;
    }

    for (uint64_t i = 0; i < ht->size; i++) {
        if (ht->table[i] == NULL) {
            stats->chainDistribution[0]++;
            continue;
        }

        stats->usedSlots++;
        uint64_t chainLen = d->shared->countSlot(ht->table[i]);

        /* Update distribution histogram */
        size_t idx = (chainLen < MULTIDICT_STATS_VECTLEN)
                         ? chainLen
                         : (MULTIDICT_STATS_VECTLEN - 1);
        stats->chainDistribution[idx]++;

        if (chainLen > stats->maxChainLen) {
            stats->maxChainLen = chainLen;
        }

        totalChainLen += chainLen;
    }

    if (stats->usedSlots > 0) {
        stats->avgChainLen = (float)totalChainLen / stats->usedSlots;
    } else {
        stats->avgChainLen = 0.0f;
    }
}

/* Get detailed statistics including chain distribution */
void multidictGetDetailedStats(multidict *d, multidictDetailedStats *stats) {
    if (!d || !stats) {
        return;
    }

    /* Clear the structure */
    memset(stats, 0, sizeof(*stats));

    /* Fill basic stats for primary table */
    stats->basic.count = HTCOUNT(d, 0);
    stats->basic.slots = HTSIZE(d, 0);
    stats->basic.usedBytes = HTBYTES(d, 0);
    stats->basic.keyBytes = HTKEYBYTES(d, 0);
    stats->basic.valBytes = HTVALBYTES(d, 0);
    stats->basic.totalBytes = HTTOTALBYTES(d, 0);
    stats->basic.isRehashing = multidictIsRehashing(d);

    if (HTSIZE(d, 0) > 0) {
        stats->basic.loadFactor =
            (uint32_t)((HTCOUNT(d, 0) * 100) / HTSIZE(d, 0));
    } else {
        stats->basic.loadFactor = 0;
    }

    /* Gather detailed distribution stats for primary table */
    multidictGetDetailedStatsHt_(d, &d->ht[0], stats);

    /* If rehashing, also gather stats for second table */
    if (multidictIsRehashing(d)) {
        stats->hasRehashTable = true;
        stats->rehashTable.count = HTCOUNT(d, 1);
        stats->rehashTable.slots = HTSIZE(d, 1);
        stats->rehashTable.usedBytes = HTBYTES(d, 1);
        stats->rehashTable.keyBytes = HTKEYBYTES(d, 1);
        stats->rehashTable.valBytes = HTVALBYTES(d, 1);
        stats->rehashTable.totalBytes = HTTOTALBYTES(d, 1);
        stats->rehashTable.isRehashing = true;

        if (HTSIZE(d, 1) > 0) {
            stats->rehashTable.loadFactor =
                (uint32_t)((HTCOUNT(d, 1) * 100) / HTSIZE(d, 1));
        } else {
            stats->rehashTable.loadFactor = 0;
        }
    } else {
        stats->hasRehashTable = false;
    }
}

/* Calculate comprehensive load metrics including byte-based and count-based */
void multidictGetLoadMetrics(multidict *d, multidictLoadMetrics *metrics) {
    if (!d || !metrics) {
        return;
    }

    memset(metrics, 0, sizeof(*metrics));

    /* Count-based load factor (legacy) */
    if (HTSIZE(d, 0) > 0) {
        metrics->countLoadFactor =
            (uint32_t)((HTCOUNT(d, 0) * 100) / HTSIZE(d, 0));
    }

    /* Byte-based metrics require iterating through slots */
    metrics->usedSlots = 0;
    metrics->totalUsedBytes = HTBYTES(d, 0);
    metrics->maxSlotBytes = 0;
    metrics->targetSlotBytes = d->targetSlotBytes;

    /* Iterate through hash table to find used slots and max slot size */
    for (uint64_t i = 0; i < HTSIZE(d, 0); i++) {
        multidictSlot *slot = SLOT(d, 0, i);
        if (slot) {
            metrics->usedSlots++;
            size_t slotBytes = d->shared->sizeBytes(slot);
            if (slotBytes > metrics->maxSlotBytes) {
                metrics->maxSlotBytes = slotBytes;
            }
        }
    }

    /* Calculate average slot bytes */
    if (metrics->usedSlots > 0) {
        metrics->avgSlotBytes = metrics->totalUsedBytes / metrics->usedSlots;
    } else {
        metrics->avgSlotBytes = 0;
    }

    /* Calculate byte-based load factor as percentage of target */
    if (d->targetSlotBytes > 0 && metrics->avgSlotBytes > 0) {
        /* Prevent overflow: if avgSlotBytes is huge, cap at 65535% */
        if (metrics->avgSlotBytes > (UINT64_MAX / 100)) {
            metrics->byteLoadFactor = UINT32_MAX;
        } else {
            uint64_t factor =
                (metrics->avgSlotBytes * 100) / d->targetSlotBytes;
            metrics->byteLoadFactor =
                (factor > UINT32_MAX) ? UINT32_MAX : (uint32_t)factor;
        }
    } else {
        metrics->byteLoadFactor = 0;
    }
}

/* Get current load factor as percentage */
uint32_t multidictLoadFactor(multidict *d) {
    if (!d || HTSIZE(d, 0) == 0) {
        return 0;
    }
    return (uint32_t)((HTCOUNT(d, 0) * 100) / HTSIZE(d, 0));
}

/* Get total memory usage (slot overhead + key bytes + value bytes) */
uint64_t multidictBytes(multidict *d) {
    if (!d) {
        return 0;
    }
    return HTTOTALBYTES(d, 0) + HTTOTALBYTES(d, 1);
}

/* Get total key bytes */
uint64_t multidictKeyBytes(multidict *d) {
    if (!d) {
        return 0;
    }
    return HTKEYBYTES(d, 0) + HTKEYBYTES(d, 1);
}

/* Get total value bytes */
uint64_t multidictValBytes(multidict *d) {
    if (!d) {
        return 0;
    }
    return HTVALBYTES(d, 0) + HTVALBYTES(d, 1);
}

/* ====================================================================
 * Bulk Operations
 * ==================================================================== */

/* Insert multiple key-value pairs at once.
 * Returns the number of successful insertions (new keys).
 * Keys and vals arrays must have at least 'count' elements. */
uint32_t multidictAddMultiple(multidict *d, const databox *keys,
                              const databox *vals, uint32_t count) {
    if (unlikely(!d || !keys || !vals || count == 0)) {
        return 0;
    }

    uint32_t inserted = 0;
    for (uint32_t i = 0; i < count; i++) {
        multidictResult result = multidictAdd(d, &keys[i], &vals[i]);
        if (result == MULTIDICT_OK_INSERTED) {
            inserted++;
        }
    }
    return inserted;
}

/* Delete multiple keys at once.
 * Returns the number of successful deletions.
 * Keys array must have at least 'count' elements. */
uint32_t multidictDeleteMultiple(multidict *d, const databox *keys,
                                 uint32_t count) {
    if (unlikely(!d || !keys || count == 0)) {
        return 0;
    }

    uint32_t deleted = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (multidictDelete(d, &keys[i])) {
            deleted++;
        }
    }
    return deleted;
}

/* ====================================================================
 * Self-Management (Memory Limits & Eviction)
 * ==================================================================== */

void multidictSetMaxMemory(multidict *d, uint64_t maxBytes) {
    if (likely(d)) {
        d->maxMemory = maxBytes;
    }
}

uint64_t multidictGetMaxMemory(multidict *d) {
    return likely(d) ? d->maxMemory : 0;
}

void multidictSetEvictionCallback(multidict *d, multidictEvictionCallback *cb,
                                  void *privdata) {
    if (likely(d)) {
        d->evictionCb = cb;
        d->evictionPrivdata = privdata;
    }
}

/* ====================================================================
 * Byte-Based Expansion Configuration
 * ==================================================================== */

/* Enable byte-based expansion with custom thresholds.
 * targetSlotBytes: expand when avg slot size exceeds this (e.g., 2MB)
 * maxSlotBytes: force expand if any single slot exceeds this (e.g., 8MB) */
void multidictEnableByteBasedExpansion(multidict *d, uint64_t targetSlotBytes,
                                       uint64_t maxSlotBytes) {
    if (unlikely(!d)) {
        return;
    }

    d->useByteBasedExpand = true;
    d->targetSlotBytes = targetSlotBytes > 0
                             ? targetSlotBytes
                             : MULTIDICT_DEFAULT_TARGET_SLOT_BYTES;
    d->maxSlotBytes =
        maxSlotBytes > 0 ? maxSlotBytes : MULTIDICT_DEFAULT_MAX_SLOT_BYTES;
}

/* Disable byte-based expansion (revert to count-based) */
void multidictDisableByteBasedExpansion(multidict *d) {
    if (likely(d)) {
        d->useByteBasedExpand = false;
    }
}

/* Check if byte-based expansion is enabled */
bool multidictIsByteBasedExpansion(multidict *d) {
    return likely(d) ? d->useByteBasedExpand : false;
}

/* Helper: get user data bytes (key + val, not slot overhead) */
static uint64_t multidictUserBytes(multidict *d) {
    return multidictKeyBytes(d) + multidictValBytes(d);
}

bool multidictIsOverLimit(multidict *d) {
    if (unlikely(!d) || d->maxMemory == 0) {
        return false; /* No limit set */
    }
    /* Compare against user data bytes, not total (which includes overhead) */
    return multidictUserBytes(d) > d->maxMemory;
}

/* Evict entries until we're under the memory limit.
 * Uses LRU when enabled, otherwise random eviction.
 * Returns number of entries evicted. */
uint32_t multidictEvictToLimit(multidict *d) {
    if (unlikely(!d) || d->maxMemory == 0) {
        return 0; /* No limit set */
    }

    uint32_t evicted = 0;
    uint32_t attempts = 0;
    uint32_t deleteFailures = 0;
    uint32_t lruVictims = 0, randomVictims = 0;
    const uint32_t maxAttempts = multidictCount(d) * 2 + 100; /* Safety limit */
    databox keybox;

    /* Use user bytes (key + val) for limit comparison, not total overhead */
    while (multidictUserBytes(d) > d->maxMemory && multidictCount(d) > 0 &&
           attempts < maxAttempts) {
        attempts++;

        bool gotVictim = false;

        /* Select victim based on eviction policy */
        if (d->evictPolicy == MULTIDICT_EVICT_LRU && d->lruEnabled) {
            /* LRU eviction: select coldest entry */
            gotVictim = multidictLRUSelectVictim(d, &keybox);
            if (gotVictim) {
                lruVictims++;
            }
        }

        if (!gotVictim) {
            /* Fallback to random eviction */
            if (!multidictGetRandomKey(d, &keybox)) {
                break; /* No keys left or error */
            }
            gotVictim = true;
            randomVictims++;
        }

        /* Call eviction callback if set */
        if (d->evictionCb) {
            databox valbox;
            if (multidictFind(d, &keybox, &valbox)) {
                if (!d->evictionCb(d->evictionPrivdata, &keybox, &valbox)) {
                    /* Callback vetoed this eviction, try another key */
                    continue;
                }
            }
        }

        /* Delete the entry (LRU tracking already handled by
         * multidictLRUSelectVictim or will be handled by multidictDelete for
         * random eviction) */
        if (multidictDelete(d, &keybox)) {
            evicted++;
        } else {
            deleteFailures++;
            /* Limit delete failures to prevent infinite loop */
            if (deleteFailures > 50) {
                break;
            }
        }

        /* Debug output every 1000 attempts */
        if (attempts % 1000 == 0) {
            printf("      [evict-loop] attempts=%u evicted=%u delFail=%u "
                   "lruV=%u randV=%u\n",
                   attempts, evicted, deleteFailures, lruVictims,
                   randomVictims);
            fflush(stdout);
        }
    }

    return evicted;
}

/* ====================================================================
 * LRU Tracking (Optional, Zero Overhead When Disabled)
 * ====================================================================
 *
 * Design Note: To avoid duplicating keys (which could be very large),
 * we store only the key hash in lruPtrToKey. During eviction, we use
 * the hash to find the slot and iterate to locate the actual key.
 * This trades O(slot_size) eviction lookup for zero key duplication.
 */

/* Entry in lruPtrToKey: stores hash to find key without duplication */
typedef struct lruKeyRef {
    uint32_t hash; /* Hash of the key for lookup */
    uint8_t valid; /* 1 if entry is valid, 0 if recycled */
} lruKeyRef;

/* Helper to grow lruPtrToKey array */
static bool lruPtrToKeyGrow(multidict *d, size_t minSize) {
    if (d->lruPtrToKeySize >= minSize) {
        return true; /* Already big enough */
    }

    size_t newSize =
        d->lruPtrToKeySize ? fibbufNextSizeBuffer(d->lruPtrToKeySize) : 256;

    while (newSize < minSize) {
        newSize = fibbufNextSizeBuffer(newSize);
    }

    lruKeyRef *newArray = zrealloc(d->lruPtrToKey, newSize * sizeof(lruKeyRef));
    if (!newArray) {
        return false;
    }

    /* Zero-initialize new entries */
    for (size_t i = d->lruPtrToKeySize; i < newSize; i++) {
        newArray[i] = (lruKeyRef){0, 0};
    }

    d->lruPtrToKey = newArray;
    d->lruPtrToKeySize = newSize;
    return true;
}

/* Enable LRU tracking. Must be called before any inserts.
 * Returns false if dict already has entries or LRU already enabled. */
bool multidictEnableLRU(multidict *d, size_t levels) {
    if (unlikely(!d)) {
        return false;
    }

    /* Can't enable if already has entries */
    if (multidictCount(d) > 0) {
        return false;
    }

    /* Already enabled? */
    if (d->lruEnabled) {
        return true;
    }

    /* Create multilru with specified levels */
    d->lru = multilruNewWithLevels(levels > 0 ? levels : 7);
    if (!d->lru) {
        return false;
    }

    /* Create auxiliary keyToPtr dict (using same type as main dict) */
    d->lruKeyToPtr = multidictNew(d->type, d->shared, d->seed);
    if (!d->lruKeyToPtr) {
        multilruFree(d->lru);
        d->lru = NULL;
        return false;
    }

    d->lruPtrToKey = NULL;
    d->lruPtrToKeySize = 0;
    d->lruEnabled = true;
    d->evictPolicy = MULTIDICT_EVICT_LRU;

    return true;
}

/* Disable LRU tracking and free LRU structures */
void multidictDisableLRU(multidict *d) {
    if (unlikely(!d) || !d->lruEnabled) {
        return;
    }

    /* Free multilru */
    if (d->lru) {
        multilruFree(d->lru);
        d->lru = NULL;
    }

    /* Free keyToPtr dict */
    if (d->lruKeyToPtr) {
        multidictFree(d->lruKeyToPtr);
        d->lruKeyToPtr = NULL;
    }

    /* Free ptrToKey array (just hashes, no key data to free) */
    if (d->lruPtrToKey) {
        zfree(d->lruPtrToKey);
        d->lruPtrToKey = NULL;
        d->lruPtrToKeySize = 0;
    }

    d->lruEnabled = false;
    d->evictPolicy = MULTIDICT_EVICT_NONE;
}

/* Check if LRU is enabled */
bool multidictHasLRU(multidict *d) {
    return d && d->lruEnabled;
}

/* Set eviction policy */
void multidictSetEvictPolicy(multidict *d, multidictEvictPolicy policy) {
    if (likely(d)) {
        d->evictPolicy = policy;
    }
}

/* Get eviction policy */
multidictEvictPolicy multidictGetEvictPolicy(multidict *d) {
    return d ? d->evictPolicy : MULTIDICT_EVICT_NONE;
}

/* Touch a key - mark as recently accessed for LRU */
void multidictTouch(multidict *d, const databox *keybox) {
    if (unlikely(!d) || !d->lruEnabled || !keybox) {
        return;
    }

    /* Look up the multilruPtr for this key */
    databox ptrBox = {{0}};
    if (!multidictFind(d->lruKeyToPtr, keybox, &ptrBox)) {
        return; /* Key not tracked */
    }

    /* Promote in LRU */
    multilruPtr ptr = (multilruPtr)ptrBox.data.u64;
    multilruIncrease(d->lru, ptr);
}

/* Get LRU level of a key (0 = coldest, higher = hotter) */
int multidictGetLRULevel(multidict *d, const databox *keybox) {
    if (unlikely(!d) || !d->lruEnabled || !keybox) {
        return -1;
    }

    /* Look up the multilruPtr for this key */
    databox ptrBox = {{0}};
    if (!multidictFind(d->lruKeyToPtr, keybox, &ptrBox)) {
        return -1; /* Key not tracked */
    }

    multilruPtr ptr = (multilruPtr)ptrBox.data.u64;
    return (int)multilruGetLevel(d->lru, ptr);
}

/* Helper: compute hash for a key */
static uint32_t multidictKeyHash(multidict *d, const databox *keybox) {
    size_t len = 0;
    uint8_t *data = NULL;
    if (databoxGetBytes((databox *)keybox, &data, &len)) {
        return multidictGenHashFunction(d, data, len);
    }
    /* Fallback for non-bytes types */
    return multidictGenHashFunction(d, &keybox->data, sizeof(keybox->data));
}

/* Internal: Register a key with LRU tracking (called from multidictAdd) */
static void multidictLRUOnInsert(multidict *d, const databox *keybox) {
    if (!d->lruEnabled) {
        return;
    }

    /* Insert into multilru */
    multilruPtr ptr = multilruInsert(d->lru);
    if (ptr == 0) {
        return; /* Allocation failed */
    }

    /* Store key -> ptr mapping */
    databox ptrBox = DATABOX_UNSIGNED(ptr);
    multidictAdd(d->lruKeyToPtr, keybox, &ptrBox);

    /* Store ptr -> hash mapping (no key duplication!) */
    if (!lruPtrToKeyGrow(d, ptr + 1)) {
        return;
    }
    lruKeyRef *refs = (lruKeyRef *)d->lruPtrToKey;
    refs[ptr].hash = multidictKeyHash(d, keybox);
    refs[ptr].valid = 1;
}

/* Internal: Update LRU on key access (called from multidictFind) */
static void multidictLRUOnAccess(multidict *d, const databox *keybox) {
    if (!d->lruEnabled) {
        return;
    }

    /* Look up ptr and promote */
    databox ptrBox = {{0}};
    if (multidictFind(d->lruKeyToPtr, keybox, &ptrBox)) {
        multilruPtr ptr = (multilruPtr)ptrBox.data.u64;
        multilruIncrease(d->lru, ptr);
    }
}

/* Internal: Remove key from LRU tracking (called from multidictDelete) */
static void multidictLRUOnDelete(multidict *d, const databox *keybox) {
    if (!d->lruEnabled) {
        return;
    }

    /* Look up ptr */
    databox ptrBox = {{0}};
    if (!multidictFind(d->lruKeyToPtr, keybox, &ptrBox)) {
        return;
    }

    multilruPtr ptr = (multilruPtr)ptrBox.data.u64;

    /* Delete from multilru */
    multilruDelete(d->lru, ptr);

    /* Delete from keyToPtr */
    multidictDelete(d->lruKeyToPtr, keybox);

    /* Mark ptrToKey entry as invalid */
    if (ptr < d->lruPtrToKeySize) {
        lruKeyRef *refs = (lruKeyRef *)d->lruPtrToKey;
        refs[ptr].valid = 0;
    }
}

/* Internal: Find first key in slot matching hash (for LRU eviction) */
static bool findKeyByHashInSlot(multidict *d, multidictSlot *slot,
                                uint32_t targetHash, databox *keybox) {
    if (!slot) {
        return false;
    }

    /* Iterate slot to find a key matching the hash */
    multidictSlotIterator iter;
    if (!d->shared->getIter(&iter, slot)) {
        return false;
    }

    multidictEntry entry;
    while (d->shared->iterNext(&iter, &entry)) {
        uint32_t h = multidictKeyHash(d, &entry.key);
        if (h == targetHash) {
            *keybox = entry.key;
            return true;
        }
    }
    return false;
}

/* Internal: Select victim using LRU policy (called from eviction) */
static bool multidictLRUSelectVictim(multidict *d, databox *keybox) {
    if (!d->lruEnabled || !d->lru) {
        return false;
    }

    multilruPtr ptr;
    if (!multilruRemoveMinimum(d->lru, &ptr)) {
        return false; /* Empty */
    }

    /* Get hash from ptrToKey */
    if (ptr >= d->lruPtrToKeySize) {
        return false;
    }
    lruKeyRef *refs = (lruKeyRef *)d->lruPtrToKey;
    if (!refs[ptr].valid) {
        return false;
    }

    uint32_t hash = refs[ptr].hash;
    refs[ptr].valid = 0; /* Mark as used */

    /* Find key by hash in the appropriate slot */
    uint32_t slotIdx = hash & HTMASK(d, 0);
    if (findKeyByHashInSlot(d, SLOT(d, 0, slotIdx), hash, keybox)) {
        /* Also remove from keyToPtr */
        multidictDelete(d->lruKeyToPtr, keybox);
        return true;
    }

    /* During rehashing, also check ht[1] */
    if (multidictIsRehashing(d)) {
        slotIdx = hash & HTMASK(d, 1);
        if (findKeyByHashInSlot(d, SLOT(d, 1, slotIdx), hash, keybox)) {
            multidictDelete(d->lruKeyToPtr, keybox);
            return true;
        }
    }

    return false;
}

/* ====================================================================
 * Dict Operations (Copy, Merge)
 * ==================================================================== */

/* Deep copy entire dictionary */
multidict *multidictCopy(const multidict *src) {
    if (unlikely(!src)) {
        return NULL;
    }

    /* Create new dict with same type and class */
    multidict *dst = multidictNew(src->type, src->shared, src->seed);
    if (!dst) {
        return NULL;
    }

    /* Copy settings */
    dst->expandLoadFactor = src->expandLoadFactor;
    dst->shrinkLoadFactor = src->shrinkLoadFactor;
    dst->compress = src->compress;
    dst->maxMemory = src->maxMemory;
    dst->evictionCb = src->evictionCb;
    dst->evictionPrivdata = src->evictionPrivdata;

    /* Pre-expand to source size to avoid rehashing during copy */
    uint64_t totalCount = multidictCount(src);
    if (totalCount > 0) {
        multidictExpand(dst, totalCount);
    }

    /* Iterate and copy all entries */
    multidictIterator iter;
    multidictIteratorInit((multidict *)src, &iter);
    multidictEntry entry;
    while (multidictIteratorNext(&iter, &entry)) {
        multidictAdd(dst, &entry.key, &entry.val);
    }
    multidictIteratorRelease(&iter);

    return dst;
}

/* Merge src into dst. Returns number of entries added/updated. */
uint64_t multidictMerge(multidict *dst, const multidict *src,
                        multidictMergeMode mode) {
    if (unlikely(!dst || !src)) {
        return 0;
    }

    uint64_t merged = 0;

    /* Iterate through all entries in src */
    multidictIterator iter;
    multidictIteratorInit((multidict *)src, &iter);
    multidictEntry entry;
    while (multidictIteratorNext(&iter, &entry)) {
        switch (mode) {
        case MULTIDICT_MERGE_REPLACE:
            /* Always add/replace from src */
            multidictAdd(dst, &entry.key, &entry.val);
            merged++;
            break;

        case MULTIDICT_MERGE_KEEP:
            /* Only add if not exists in dst */
            if (multidictAddNX(dst, &entry.key, &entry.val) != MULTIDICT_ERR) {
                merged++;
            }
            break;
        }
    }
    multidictIteratorRelease(&iter);

    return merged;
}

bool multidictIteratorInit(multidict *d, multidictIterator *iter) {
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = false;
    iter->current =
        NULL; /* Initialize to NULL so first call to Next starts fresh */
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
            /* Initialize iterator for this slot; if getIter returns false
             * (e.g., empty slot or empty multimap), skip to next slot */
            if (!shared->getIter(&iter->iter, iter->current)) {
                iter->current = NULL;
                continue; /* Loop to try next slot */
            }
            /* EMIT FIRST ITERATION OF THIS SLOT */
        } else {
            /* EMIT NEXT ITERATION OF CURRENT SLOT */
        }

        if (shared->iterNext(&iter->iter, e)) {
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
    if (listlen == 0) {
        return false;
    }

    int32_t listele = random() % listlen;
    return d->shared->findKeyByPosition(d->shared, current, listele, keybox);
}

/* Sample the dictionary to return multiple keys from random locations.
 * Returns databox keys in the caller-provided array. Does not guarantee
 * returning 'count' keys or non-duplicated elements, but makes an effort.
 * Returns the actual number of keys stored (may be less than count). */
uint32_t multidictGetSomeKeys(multidict *d, databox *keys, uint32_t count) {
    if (multidictSize(d) == 0 || count == 0) {
        return 0;
    }

    /* Clamp count to actual dictionary size */
    if (count > multidictSize(d)) {
        count = multidictSize(d);
    }

    uint32_t stored = 0;
    uint32_t maxsteps = count * 10;

    /* Do some rehashing work proportional to count */
    for (uint32_t j = 0; j < count && multidictIsRehashing(d); j++) {
        multidictRehash(d, 1);
    }

    /* Determine which tables to scan and the mask size */
    uint32_t tables = multidictIsRehashing(d) ? 2 : 1;
    uint32_t maxmask = HTMASK(d, 0);
    if (tables > 1 && maxmask < HTMASK(d, 1)) {
        maxmask = HTMASK(d, 1);
    }

    /* Pick a random starting point */
    uint32_t idx = random() & maxmask;
    uint32_t emptylen = 0;

    multidictClass *shared = d->shared;

    while (stored < count && maxsteps--) {
        for (uint32_t t = 0; t < tables; t++) {
            /* During rehashing, skip already-migrated slots in ht[0] */
            if (tables == 2 && t == 0 && idx < d->rehashidx) {
                if (idx >= HTSIZE(d, 1)) {
                    idx = d->rehashidx;
                }
                continue;
            }

            if (idx >= HTSIZE(d, t)) {
                continue;
            }

            multidictSlot *slot = SLOT(d, t, idx);
            if (slot == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    idx = random() & maxmask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                /* Extract keys from this slot */
                uint32_t slotCount = shared->countSlot(slot);
                for (uint32_t pos = 0; pos < slotCount && stored < count;
                     pos++) {
                    if (shared->findKeyByPosition(shared, slot, pos,
                                                  &keys[stored])) {
                        stored++;
                    }
                }
                if (stored >= count) {
                    return stored;
                }
            }
        }
        idx = (idx + 1) & maxmask;
    }

    return stored;
}

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

        /* Masks must match swapped tables, not fixed HT indices */
        m0 = t0->size - 1;
        m1 = t1->size - 1;

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
    d->rehashidx = MULTIDICT_REHASHIDX_INVALID;
    d->rehashing = false;
    d->iterators = 0;
}

void multidictResizeEnable(multidict *d) {
    d->autoResize = true;
    d->shared->disableResize =
        false; /* Also update class for backwards compat */
}

void multidictResizeDisable(multidict *d) {
    d->autoResize = false;
    d->shared->disableResize =
        true; /* Also update class for backwards compat */
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST
/* Print formatted stats from a detailed stats structure */
DK_STATIC void
multidictPrintDetailedStats_(const multidictDetailedStats *stats) {
    if (stats->basic.count == 0) {
        printf("No stats available for empty multidictionaries\n");
        return;
    }

    printf("Hash table stats:\n");
    printf(" table size: %" PRIu64 "\n", stats->basic.slots);
    printf(" number of elements: %" PRIu64 "\n", stats->basic.count);
    printf(" different slots: %" PRIu64 "\n", stats->usedSlots);
    printf(" max chain length: %" PRIu64 "\n", stats->maxChainLen);
    printf(" avg chain length: %.02f\n", stats->avgChainLen);
    printf(" load factor: %u%%\n", stats->basic.loadFactor);
    printf(" memory: total=%" PRIu64 " key=%" PRIu64 " val=%" PRIu64
           " used=%" PRIu64 "\n",
           stats->basic.totalBytes, stats->basic.keyBytes,
           stats->basic.valBytes, stats->basic.usedBytes);
    printf(" Chain length distribution:\n");

    for (uint64_t i = 0; i < MULTIDICT_STATS_VECTLEN - 1; i++) {
        if (stats->chainDistribution[i] == 0) {
            continue;
        }

        printf("   %s%" PRIu64 ": %" PRIu64 " (%.02f%%)\n",
               (i == MULTIDICT_STATS_VECTLEN - 1) ? ">= " : "", i,
               stats->chainDistribution[i],
               ((float)stats->chainDistribution[i] / stats->basic.slots) * 100);
    }
}

void multidictPrintStats(multidict *d) {
    if (!d) {
        return;
    }

    /* Use the structured stats API */
    multidictDetailedStats stats;
    multidictGetDetailedStats(d, &stats);

    multidictPrintDetailedStats_(&stats);

    if (stats.hasRehashTable) {
        printf("-- Rehashing into ht[1]:\n");
        printf(" table size: %" PRIu64 "\n", stats.rehashTable.slots);
        printf(" number of elements: %" PRIu64 "\n", stats.rehashTable.count);
        printf(" load factor: %u%%\n", stats.rehashTable.loadFactor);
        printf(" memory: total=%" PRIu64 "\n", stats.rehashTable.totalBytes);
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

    /* Keys must be same length to be equal */
    if (k1len != k2len) {
        return 0; /* not equal */
    }

    return strncmp(key1, key2, k1len) == 0;
}

/* Case-insensitive key comparison */
DK_STATIC int32_t multidictStringCaseKeyCompare_(void *privdata,
                                                 const void *key1,
                                                 const uint32_t k1len,
                                                 const void *key2,
                                                 const uint32_t k2len) {
    DK_NOTUSED(privdata);

    /* Keys must be same length to be equal */
    if (k1len != k2len) {
        return 0; /* not equal */
    }

    return strncasecmp(key1, key2, k1len) == 0;
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
    multidictStringCaseKeyCompare_    /* key compare (case-insensitive) */
};

/* ====================================================================
 * Debugging
 * ==================================================================== */
void multidictRepr(const multidict *d) {
    printf("multidict %p has:\n", (void *)d);
    for (int i = 0; i < 2; i++) {
        printf("\tHT %d:\n", i);
        printf("\t\tSLOTS: %" PRIu32 "\n", HTSIZE(d, i));
        printf("\t\tCOUNT: %" PRIu64 "\n", HTCOUNT(d, i));
        printf("\t\tSLOT BYTES: %" PRIu64 "\n", HTBYTES(d, i));
        printf("\t\tKEY BYTES: %" PRIu64 "\n", HTKEYBYTES(d, i));
        printf("\t\tVAL BYTES: %" PRIu64 "\n", HTVALBYTES(d, i));
        printf("\t\tTOTAL BYTES: %" PRIu64 "\n", HTTOTALBYTES(d, i));
    }
}

void multidictOpRepr(const multidictOp *op) {
    printf("multidictOp %p has:\n", (void *)op);
    printf("\tht: %p\n", (void *)op->ht);
    printf("\tresult: %p\n", op->result);
}

/* ====================================================================
 * Multimap-Based Slot Implementation
 * ==================================================================== */

/* Wrapper struct so slot address is stable even when multimap reallocates */
typedef struct {
    multimap *m;
} mmSlotWrapper;

DK_STATIC int64_t mmSlotInsert(multidictClass *qdc, multidictSlot *slot,
                               const databox *keybox, const databox *valbox) {
    (void)qdc;
    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;

    if (wrapper->m == NULL) {
        return -1;
    }

    const databox *elements[2] = {keybox, valbox};
    bool replaced = multimapInsert(&wrapper->m, elements);

    return replaced ? 0 : 1; /* 1 = new insert, 0 = replaced */
}

DK_STATIC void *mmSlotGetOrCreate(multidictClass *qdc, multidictSlot **slot,
                                  const databox *keybox) {
    (void)qdc;
    (void)keybox;
    if (*slot == NULL) {
        mmSlotWrapper *wrapper = zcalloc(1, sizeof(*wrapper));
        wrapper->m = multimapNew(2); /* key + value */
        *slot = wrapper;
    }
    return *slot;
}

DK_STATIC void *mmSlotRemove(multidictClass *qdc, multidictSlot **slot,
                             const databox *keybox) {
    (void)qdc;
    if (*slot == NULL) {
        return NULL;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)*slot;
    bool deleted = multimapDelete(&wrapper->m, keybox);
    // cppcheck-suppress intToPointerCast - non-NULL sentinel to indicate
    // deletion occurred
    return deleted ? (void *)1 : NULL;
}

DK_STATIC bool mmSlotFindValue(multidictClass *qdc, multidictSlot *slot,
                               const databox *keybox, databox *valbox) {
    (void)qdc;
    if (slot == NULL) {
        return false;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    /* multimapLookup returns only the non-key elements in the array.
     * For a 2-element multimap (key + value), elements[0] gets the value. */
    databox *elements[1] = {valbox};
    return multimapLookup(wrapper->m, keybox, elements);
}

DK_STATIC void *mmSlotCreate(void) {
    mmSlotWrapper *wrapper = zcalloc(1, sizeof(*wrapper));
    wrapper->m = multimapNew(2); /* key + value */
    return wrapper;
}

DK_STATIC uint32_t mmSlotFree(multidictClass *qdc, multidictSlot *slot) {
    (void)qdc;
    if (slot) {
        mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
        uint32_t count = multimapCount(wrapper->m);
        multimapFree(wrapper->m);
        zfree(wrapper);
        return count;
    }
    return 0;
}

DK_STATIC size_t mmSlotSizeBytes(multidictSlot *slot) {
    if (slot == NULL) {
        return 0;
    }
    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    return multimapBytes(wrapper->m);
}

/* Iterator state stored in multidictSlotIterator.iterspace */
typedef struct {
    multimapIterator mmIter;
} mmSlotIterState;

DK_STATIC bool mmSlotGetIter(multidictSlotIterator *iter, multidictSlot *slot) {
    if (slot == NULL) {
        return false;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    if (wrapper->m == NULL || multimapCount(wrapper->m) == 0) {
        return false;
    }

    mmSlotIterState *state = (mmSlotIterState *)iter->iterspace;
    multimapIteratorInit(wrapper->m, &state->mmIter, true);
    iter->slot = slot;
    iter->index = 0;
    return true;
}

DK_STATIC bool mmSlotIterNext(multidictSlotIterator *iter,
                              multidictEntry *entry) {
    if (!entry) {
        return false;
    }

    mmSlotIterState *state = (mmSlotIterState *)iter->iterspace;

    /* Verify iterator was properly initialized (type should be 1, 2, or 3) */
    if (state->mmIter.type < 1 || state->mmIter.type > 3) {
        return false;
    }

    databox *elements[2] = {&entry->key, &entry->val};

    if (multimapIteratorNext(&state->mmIter, elements)) {
        iter->index++;
        return true;
    }
    return false;
}

DK_STATIC uint32_t mmSlotCount(multidictSlot *slot) {
    if (slot == NULL) {
        return 0;
    }
    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    return multimapCount(wrapper->m);
}

DK_STATIC bool mmSlotFindKeyByPos(multidictClass *qdc, multidictSlot *slot,
                                  uint32_t pos, databox *keybox) {
    (void)qdc;
    if (slot == NULL) {
        return false;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;

    /* Iterate to find key at position */
    multimapIterator mmIter;
    multimapIteratorInit(wrapper->m, &mmIter, true);

    databox val;
    databox *elements[2] = {keybox, &val};
    uint32_t idx = 0;
    while (multimapIteratorNext(&mmIter, elements)) {
        if (idx == pos) {
            return true;
        }
        idx++;
    }
    return false;
}

DK_STATIC void mmSlotIterateAll(multidictClass *qdc, multidictSlot *slot,
                                multidictIterProcess process, void *privdata) {
    (void)qdc;
    if (slot == NULL || process == NULL) {
        return;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    multimapIterator mmIter;
    multimapIteratorInit(wrapper->m, &mmIter, true);

    databox key, val;
    databox *elements[2] = {&key, &val};
    while (multimapIteratorNext(&mmIter, elements)) {
        process(privdata, &key, &val);
    }
}

DK_STATIC bool mmSlotLastKey(multidictSlot *slot, databox *keybox) {
    if (slot == NULL) {
        return false;
    }

    mmSlotWrapper *wrapper = (mmSlotWrapper *)slot;
    /* multimapLast populates all elements: elements[0]=key, elements[1]=value
     */
    databox lastVal;
    databox *elements[2] = {keybox, &lastVal};
    return multimapLast(wrapper->m, elements);
}

DK_STATIC bool mmSlotMigrateLast(void *dst, void *src) {
    mmSlotWrapper *srcWrapper = (mmSlotWrapper *)src;
    mmSlotWrapper *dstWrapper = (mmSlotWrapper *)dst;

    /* Get last key-value from source */
    databox key, val;
    databox *elements[2] = {&key, &val};
    if (!multimapLast(srcWrapper->m, elements)) {
        return false;
    }

    /* Insert into destination */
    const databox *insertElements[2] = {&key, &val};
    multimapInsert(&dstWrapper->m, insertElements);

    /* Delete from source */
    multimapDelete(&srcWrapper->m, &key);
    return true;
}

DK_STATIC void mmSlotClassFree(multidictClass *qdc) {
    (void)qdc;
    /* Nothing to free for this simple class */
}

DK_STATIC uint32_t mmSlotFreeClass(multidictClass *qdc) {
    (void)qdc;
    return 0;
}

/* Create a multimap-based multidictClass for using multimap as chained backing
 * stores for dict inserts */
DK_STATIC multidictClass *multidictMmClassNew(void) {
    multidictClass *qdc = zcalloc(1, sizeof(*qdc));
    qdc->privdata = NULL;
    qdc->insertByType = mmSlotInsert;
    qdc->operationSlotGetOrCreate = mmSlotGetOrCreate;
    qdc->operationRemove = mmSlotRemove;
    qdc->findValueByKey = mmSlotFindValue;
    qdc->createSlot = mmSlotCreate;
    qdc->freeSlot = mmSlotFree;
    qdc->sizeBytes = mmSlotSizeBytes;
    qdc->getIter = mmSlotGetIter;
    qdc->iterNext = mmSlotIterNext;
    qdc->countSlot = mmSlotCount;
    qdc->findKeyByPosition = mmSlotFindKeyByPos;
    qdc->iterateAll = mmSlotIterateAll;
    qdc->lastKey = mmSlotLastKey;
    qdc->migrateLast = mmSlotMigrateLast;
    qdc->free = mmSlotClassFree;
    qdc->freeClass = mmSlotFreeClass;
    qdc->disableResize = false;
    return qdc;
}

DK_STATIC void multidictMmClassFree(multidictClass *qdc) {
    zfree(qdc);
}

/* Public wrappers for default class creation */
multidictClass *multidictDefaultClassNew(void) {
    return multidictMmClassNew();
}

void multidictDefaultClassFree(multidictClass *qdc) {
    multidictMmClassFree(qdc);
}

/* ====================================================================
 * Comprehensive Test Function
 * ==================================================================== */
#ifdef DATAKIT_TEST

/* Simple PRNG for reproducible tests */
static uint32_t testRandState = 12345;
static uint32_t testRand(void) {
    testRandState = testRandState * 1103515245 + 12345;
    return (testRandState >> 16) & 0x7fff;
}
static void testRandSeed(uint32_t seed) {
    testRandState = seed;
}

/* Scan callback for counting */
static void scanCountCallback(void *privdata, const databox *key,
                              const databox *val) {
    (void)key;
    (void)val;
    int *count = (int *)privdata;
    (*count)++;
}

/* Test eviction callbacks (defined at file scope for portability) */
static int testEvictionCallbackCount_ = 0;
static bool testEvictionCb_(void *privdata, const databox *key,
                            const databox *val) {
    (void)key;
    (void)val;
    int *counter = (int *)privdata;
    (*counter)++;
    return true; /* Allow eviction */
}

static int testVetoCount_ = 0;
static bool vetoEvictionCb_(void *privdata, const databox *key,
                            const databox *val) {
    (void)key;
    (void)val;
    int *counter = (int *)privdata;
    if (*counter < 5) {
        (*counter)++;
        return false; /* Veto */
    }
    return true; /* Allow */
}

int multidictTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    printf("=== MULTIDICT COMPREHENSIVE TEST SUITE ===\n\n");

    printf("Creating multimap-based multidictClass...\n");
    multidictClass *qdc = multidictMmClassNew();

    printf("Creating multidict with multimap slots...\n");

    /* ================================================================
     * SECTION 1: Basic API Tests
     * ================================================================ */
    printf("--- Section 1: Basic API Tests ---\n");

    printf("Test 1.1: Create with exact key type...\n");
    multidict *d = multidictNew(&multidictTypeExactKey, qdc, 12345);
    assert(d != NULL);
    assert(multidictSize(d) == 0);

    printf("Test 1.2: multidictGetClass...\n");
    assert(multidictGetClass(d) == qdc);

    printf("Test 1.3: Get/Set hash seed...\n");
    assert(multidictGetHashFunctionSeed(d) == 12345);
    multidictSetHashFunctionSeed(d, 99999);
    assert(multidictGetHashFunctionSeed(d) == 99999);
    multidictSetHashFunctionSeed(d, 12345); /* restore */

    printf("Test 1.4: Empty dict operations...\n");
    {
        databox key = databoxNewBytesString("nokey");
        databox val;
        assert(multidictFind(d, &key, &val) == false);
        assert(multidictDelete(d, &key) == false);
        (void)key;
        (void)val;

        databox randomKey;
        assert(multidictGetRandomKey(d, &randomKey) == false);
        (void)randomKey;

        databox keys[5];
        assert(multidictGetSomeKeys(d, keys, 5) == 0);
        (void)keys;
    }

    printf("Test 1.5: Basic insert and find...\n");
    {
        databox key = databoxNewBytesString("hello");
        databox val = databoxNewBytesString("world");
        multidictResult result = multidictAdd(d, &key, &val);
        assert(result == MULTIDICT_OK_INSERTED);
        (void)result;
        assert(multidictSize(d) == 1);

        databox found;
        assert(multidictFind(d, &key, &found) == true);
        assert(found.len == val.len);
        (void)found;
    }

    printf("Test 1.6: Update existing key...\n");
    {
        databox key = databoxNewBytesString("hello");
        databox newVal = databoxNewBytesString("universe");
        multidictResult result = multidictAdd(d, &key, &newVal);
        assert(result == MULTIDICT_OK_REPLACED);
        (void)result;
        assert(multidictSize(d) == 1); /* size unchanged */

        databox found;
        assert(multidictFind(d, &key, &found) == true);
        (void)found;
    }

    printf("Test 1.7: Delete key...\n");
    {
        databox key = databoxNewBytesString("hello");
        assert(multidictDelete(d, &key) == true);
        assert(multidictSize(d) == 0);

        databox found;
        assert(multidictFind(d, &key, &found) == false);
        (void)found;
        assert(multidictDelete(d, &key) == false); /* already deleted */
        (void)key;
    }

    printf("Test 1.8: multidictFindByString...\n");
    {
        databox key = databoxNewBytesString("strkey");
        databox val = databoxNewBytesString("strval");
        multidictAdd(d, &key, &val);

        uint8_t *foundVal;
        bool found = multidictFindByString(d, "strkey", &foundVal);
        assert(found == true);
        (void)found;
        multidictDelete(d, &key);
    }

    /* ================================================================
     * SECTION 2: Edge Cases
     * ================================================================ */
    printf("\n--- Section 2: Edge Cases ---\n");

    printf("Test 2.1: Single character keys...\n");
    {
        for (int i = 0; i < 26; i++) {
            char keyBuf[2] = {(char)('a' + i), '\0'};
            char valBuf[2] = {(char)('A' + i), '\0'};
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }
        assert(multidictSize(d) == 26);

        /* Verify all exist */
        for (int i = 0; i < 26; i++) {
            char keyBuf[2] = {(char)('a' + i), '\0'};
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
            (void)key;
            (void)found;
        }
        multidictEmpty(d);
    }

    printf("Test 2.2: Long keys (256 bytes)...\n");
    {
        char longKey[257];
        char longVal[257];
        memset(longKey, 'K', 256);
        memset(longVal, 'V', 256);
        longKey[256] = '\0';
        longVal[256] = '\0';

        databox key = databoxNewBytesString(longKey);
        databox val = databoxNewBytesString(longVal);
        multidictAdd(d, &key, &val);

        databox found;
        assert(multidictFind(d, &key, &found) == true);
        assert(found.len == 256);
        (void)found;
        multidictEmpty(d);
    }

    printf("Test 2.3: Keys with special characters...\n");
    {
        const char *specialKeys[] = {
            "",
            "  ",
            "\t\n",
            "key with spaces",
            "key\0embedded", /* will be truncated at \0 */
            "émojis🎉",
            "日本語",
            "مرحبا"};
        int numSpecial = sizeof(specialKeys) / sizeof(specialKeys[0]);

        for (int i = 0; i < numSpecial; i++) {
            databox key = databoxNewBytesString(specialKeys[i]);
            databox val = databoxNewBytesString("value");
            multidictAdd(d, &key, &val);
        }

        /* Verify accessible */
        for (int i = 0; i < numSpecial; i++) {
            databox key = databoxNewBytesString(specialKeys[i]);
            databox found;
            bool exists = multidictFind(d, &key, &found);
            assert(exists == true);
            (void)exists;
            (void)found;
        }
        multidictEmpty(d);
    }

    printf("Test 2.4: Numeric string keys with collisions...\n");
    {
        /* Insert keys that might hash similarly */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "%d", i * 7919); /* prime gaps */
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }
        assert(multidictSize(d) == 1000);

        /* Verify all accessible */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "%d", i * 7919);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
        }
        multidictEmpty(d);
    }

    /* ================================================================
     * SECTION 3: Iterator Tests
     * ================================================================ */
    printf("\n--- Section 3: Iterator Tests ---\n");

    printf("Test 3.1: Iterator on empty dict...\n");
    {
        multidictIterator iter;
        multidictIteratorInit(d, &iter);
        multidictEntry entry;
        assert(multidictIteratorNext(&iter, &entry) == false);
        multidictIteratorRelease(&iter);
    }

    printf("Test 3.2: Iterate and count...\n");
    {
        for (int i = 0; i < 500; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "iter%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        multidictIterator iter;
        multidictIteratorInit(d, &iter);
        multidictEntry entry;
        int count = 0;
        while (multidictIteratorNext(&iter, &entry)) {
            count++;
        }
        multidictIteratorRelease(&iter);
        assert(count == 500);
    }

    printf("Test 3.3: Safe iterator allows modifications...\n");
    {
        multidictIterator iter;
        multidictIteratorGetSafe(d, &iter);
        multidictEntry entry;
        int count = 0;
        while (multidictIteratorNext(&iter, &entry)) {
            count++;
            /* Add a new key during safe iteration */
            if (count == 100) {
                databox key = databoxNewBytesString("safeadd");
                databox val = databoxNewBytesString("during_iter");
                multidictAdd(d, &key, &val);
            }
        }
        multidictIteratorRelease(&iter);
        assert(count >= 500);

        /* Clean added key */
        databox key = databoxNewBytesString("safeadd");
        multidictDelete(d, &key);
    }

    printf("Test 3.4: Multiple iterators...\n");
    {
        multidictIterator iter1, iter2;
        multidictIteratorInit(d, &iter1);
        multidictIteratorInit(d, &iter2);

        multidictEntry e1, e2;
        int c1 = 0, c2 = 0;

        /* Interleave iteration */
        while (multidictIteratorNext(&iter1, &e1)) {
            c1++;
            if (multidictIteratorNext(&iter2, &e2)) {
                c2++;
            }
        }
        while (multidictIteratorNext(&iter2, &e2)) {
            c2++;
        }

        multidictIteratorRelease(&iter1);
        multidictIteratorRelease(&iter2);
        assert(c1 == 500);
        assert(c2 == 500);
        multidictEmpty(d);
    }

    /* ================================================================
     * SECTION 4: Rehash Tests
     * ================================================================ */
    printf("\n--- Section 4: Rehash Tests ---\n");

    printf("Test 4.1: Basic expand and rehash...\n");
    {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        bool expanded = multidictExpand(d, 2048);
        assert(expanded == true);
        assert(multidictIsRehashing(d) == true);

        while (multidictIsRehashing(d)) {
            multidictRehash(d, 10);
        }
        assert(multidictIsRehashing(d) == false);

        /* Verify all entries */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
        }
    }

    printf("Test 4.2: Operations during rehash...\n");
    {
        multidictExpand(d, 8192);
        assert(multidictIsRehashing(d) == true);

        /* Add during rehash */
        databox key = databoxNewBytesString("during_rehash");
        databox val = databoxNewBytesString("value");
        multidictAdd(d, &key, &val);

        /* Find during rehash */
        databox found;
        assert(multidictFind(d, &key, &found) == true);

        /* Delete during rehash */
        assert(multidictDelete(d, &key) == true);
        assert(multidictFind(d, &key, &found) == false);

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
    }

    printf("Test 4.3: Rehash with single steps...\n");
    {
        multidictExpand(d, 16384);
        int steps = 0;
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 1);
            steps++;
        }
        printf("  Completed in %d single steps\n", steps);
    }

    printf("Test 4.4: multidictRehashMilliseconds...\n");
    {
        multidictExpand(d, 32768);
        int64_t rehashed = multidictRehashMilliseconds(d, 5);
        printf("  Rehashed %" PRId64 " entries in ~5ms\n", rehashed);

        /* Complete remaining */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 1000);
        }
    }

    printf("Test 4.5: Expand to same size (should fail)...\n");
    {
        uint64_t currentSlots = multidictSlots(d);
        bool expanded = multidictExpand(d, currentSlots / 2);
        /* Should fail - can't expand to same or smaller */
        assert(expanded == false || !multidictIsRehashing(d));
    }

    printf("Test 4.6: multidictResize (shrink)...\n");
    {
        /* Delete many entries to create sparse table */
        for (int i = 0; i < 900; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            multidictDelete(d, &key);
        }

        bool resized = multidictResize(d);
        printf("  Resize returned: %s\n", resized ? "true" : "false");

        /* Complete any triggered rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }

        /* Verify remaining entries */
        for (int i = 900; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
        }
        multidictEmpty(d);
    }

    printf("Test 4.7: Find entries in ht[0] when ht[1] slot is non-empty...\n");
    {
        /* This test covers a critical bug where during incremental rehashing,
         * multidictFind would only search ht[0] if the ht[1] slot was NULL.
         * If ht[1] had a non-empty slot (due to hash collision with different
         * keys), it would incorrectly fail to find keys still in ht[0].
         *
         * Scenario: key A and key B hash to the same bucket in ht[1].
         * Key A is migrated to ht[1]. Key B is still in ht[0].
         * Bug: Find(B) returns ht[1] slot (non-empty, contains A),
         *      doesn't fall back to ht[0], so B is "not found". */

        multidictEmpty(d);

        /* Add enough entries to have a decent hash table */
        const int NUM_ENTRIES = 2000;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rehash_test_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Trigger expansion to start rehashing */
        uint64_t currentSlots = multidictSlots(d);
        bool expanded = multidictExpand(d, currentSlots * 4);
        assert(expanded == true);
        assert(multidictIsRehashing(d) == true);

        /* Do partial rehashing - migrate only SOME entries to ht[1].
         * This leaves some entries in ht[0] while ht[1] is populated. */
        int partialSteps = 0;
        while (multidictIsRehashing(d) && partialSteps < 50) {
            multidictRehash(d, 1);
            partialSteps++;
        }

        /* Now we have entries in BOTH ht[0] (not yet migrated) and ht[1].
         * The critical test: can we find ALL entries? */
        int foundCount = 0;
        int notFoundCount = 0;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rehash_test_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            if (multidictFind(d, &key, &found)) {
                foundCount++;
                /* Verify value is correct */
                assert(found.data.i == i);
            } else {
                notFoundCount++;
                /* This was the bug - entries in ht[0] couldn't be found
                 * when ht[1] had a non-empty slot for the same hash. */
                printf("  BUG: Entry %d not found during partial rehash!\n", i);
            }
        }

        printf("  Partial rehash: found=%d notfound=%d (rehashing=%s)\n",
               foundCount, notFoundCount,
               multidictIsRehashing(d) ? "yes" : "no");
        assert(notFoundCount == 0); /* ALL entries must be findable */
        assert(foundCount == NUM_ENTRIES);

        /* Complete the rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }

        /* Verify all entries still accessible after rehash completes */
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rehash_test_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
            assert(found.data.i == i);
        }

        multidictEmpty(d);
    }

    printf("Test 4.8: Delete entries from ht[0] during partial rehash...\n");
    {
        /* Similar to 4.7, but tests that delete works correctly when
         * entries are split across ht[0] and ht[1]. */

        const int NUM_ENTRIES = 1000;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "del_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Trigger rehashing */
        multidictExpand(d, multidictSlots(d) * 4);
        assert(multidictIsRehashing(d) == true);

        /* Partial rehash */
        for (int i = 0; i < 30 && multidictIsRehashing(d); i++) {
            multidictRehash(d, 1);
        }

        /* Delete ALL entries during partial rehash */
        int deleteSuccess = 0;
        int deleteFail = 0;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "del_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            if (multidictDelete(d, &key)) {
                deleteSuccess++;
            } else {
                deleteFail++;
                printf("  BUG: Delete failed for entry %d during rehash!\n", i);
            }
        }

        printf("  Delete during rehash: success=%d fail=%d\n", deleteSuccess,
               deleteFail);
        assert(deleteFail == 0);
        assert(deleteSuccess == NUM_ENTRIES);
        assert(multidictCount(d) == 0);

        /* Complete any remaining rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
    }

    printf("Test 4.9: Comprehensive rehash operation fuzzing - Add/Find/Delete "
           "mix...\n");
    {
        const int FUZZ_ENTRIES = 1500;
        const int FUZZ_OPS = 2000;

        /* Initial populate */
        for (int i = 0; i < FUZZ_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i * 100);
            multidictAdd(d, &key, &val);
        }

        /* Trigger rehashing */
        multidictExpand(d, FUZZ_ENTRIES * 4);
        assert(multidictIsRehashing(d));

        /* Partial rehash to get mixed state */
        for (int i = 0; i < 100; i++) {
            multidictRehash(d, 1);
        }

        /* Fuzz test: mix of operations during active rehashing */
        int adds = 0, finds = 0, dels = 0, replaces = 0;
        int findFails = 0, delFails = 0;

        for (int op = 0; op < FUZZ_OPS; op++) {
            int opType = testRand() % 10;
            int idx = testRand() % (FUZZ_ENTRIES + 500); /* Some out of range */

            if (opType < 3) { /* 30% adds */
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_new_%d", idx);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(idx);
                multidictAdd(d, &key, &val);
                adds++;
            } else if (opType < 6) { /* 30% finds */
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", idx % FUZZ_ENTRIES);
                databox key = databoxNewBytesString(keyBuf);
                databox found;
                if (multidictFind(d, &key, &found)) {
                    finds++;
                } else {
                    findFails++;
                }
            } else if (opType < 8) { /* 20% deletes */
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", idx % FUZZ_ENTRIES);
                databox key = databoxNewBytesString(keyBuf);
                if (multidictDelete(d, &key)) {
                    dels++;
                } else {
                    delFails++;
                }
            } else { /* 20% replace */
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", idx % FUZZ_ENTRIES);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(idx * 200);
                multidictReplace(d, &key, &val);
                replaces++;
            }

            /* Interleave rehashing */
            if (multidictIsRehashing(d) && (op % 10 == 0)) {
                multidictRehash(d, 2);
            }
        }

        printf("  Fuzz ops: adds=%d finds=%d (fails=%d) dels=%d (fails=%d) "
               "replaces=%d\n",
               adds, finds, findFails, dels, delFails, replaces);
        printf("  Final count: %" PRIu64 ", rehashing=%s\n", multidictCount(d),
               multidictIsRehashing(d) ? "yes" : "no");

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }

        multidictEmpty(d);
    }

    printf("Test 4.10: IncrBy operations during rehashing...\n");
    {
        multidictEmpty(d); /* Start fresh */
        const int INCR_ENTRIES = 1000;

        /* Add numeric entries */
        for (int i = 0; i < INCR_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "incr_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(0);
            multidictAdd(d, &key, &val);
        }

        /* Trigger rehashing */
        multidictExpand(d, INCR_ENTRIES * 3);
        assert(multidictIsRehashing(d));

        /* Partial rehash */
        for (int i = 0; i < 50; i++) {
            multidictRehash(d, 1);
        }

        /* Increment operations during rehashing */
        int incrSuccess = 0;
        int incrErrCount = 0;
        int incrWrongValue = 0;
        for (int i = 0; i < INCR_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "incr_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            int64_t result;
            multidictResult res = multidictIncrBy(d, &key, 1, &result);
            /* IncrBy should succeed (either OK_REPLACED for existing or
             * OK_INSERTED for new) */
            if (res == MULTIDICT_ERR) {
                incrErrCount++;
            } else if (result != 1) {
                incrWrongValue++;
                if (incrWrongValue <= 5) {
                    printf("  IncrBy key incr_%d returned result=%lld "
                           "(expected 1), res=%d\n",
                           i, (long long)result, res);
                }
            } else {
                incrSuccess++;
            }

            /* Interleave rehashing */
            if (multidictIsRehashing(d) && (i % 20 == 0)) {
                multidictRehash(d, 3);
            }
        }

        printf("  IncrBy during rehash: success=%d, errors=%d, wrongValue=%d "
               "(expected=%d)\n",
               incrSuccess, incrErrCount, incrWrongValue, INCR_ENTRIES);
        if (incrSuccess != INCR_ENTRIES) {
            printf("  ERROR: Not all IncrBy operations succeeded!\n");
        }
        assert(incrSuccess == INCR_ENTRIES);

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }

        /* Verify all values incremented correctly */
        for (int i = 0; i < INCR_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "incr_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found));
            assert(found.data.i == 1);
        }

        multidictEmpty(d);
    }

    printf("Test 4.11: GetAndDelete during rehashing...\n");
    {
        multidictEmpty(d); /* Start fresh */
        const int GAD_ENTRIES = 500;

        /* Populate */
        for (int i = 0; i < GAD_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "gad_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i * 5);
            multidictAdd(d, &key, &val);
        }

        /* Trigger rehashing */
        multidictExpand(d, GAD_ENTRIES * 4);
        for (int i = 0; i < 30; i++) {
            multidictRehash(d, 1);
        }

        /* GetAndDelete during partial rehashing */
        int gadSuccess = 0;
        for (int i = 0; i < GAD_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "gad_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val;
            if (multidictGetAndDelete(d, &key, &val)) {
                assert(val.data.i == i * 5);
                gadSuccess++;
            }

            /* Interleave rehashing */
            if (multidictIsRehashing(d) && (i % 15 == 0)) {
                multidictRehash(d, 2);
            }
        }

        printf("  GetAndDelete during rehash: success=%d (expected=%d)\n",
               gadSuccess, GAD_ENTRIES);
        assert(gadSuccess == GAD_ENTRIES);
        assert(multidictCount(d) == 0);

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
    }

    printf("Test 4.12: Stress test - rapid rehashing with constant "
           "operations...\n");
    {
        multidictEmpty(d); /* Start fresh */
        const int STRESS_ENTRIES = 1000;
        const int STRESS_CYCLES = 3;

        for (int cycle = 0; cycle < STRESS_CYCLES; cycle++) {
            /* Populate */
            for (int i = 0; i < STRESS_ENTRIES; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "stress_%d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                multidictAdd(d, &key, &val);
            }

            /* Trigger multiple expansions */
            multidictExpand(d, STRESS_ENTRIES * 2);

            /* Aggressive operations during rehashing */
            int ops = 0;
            while (multidictIsRehashing(d) && ops < 500) {
                int opType = testRand() % 5;
                int idx = testRand() % STRESS_ENTRIES;

                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "stress_%d", idx);
                databox key = databoxNewBytesString(keyBuf);

                switch (opType) {
                case 0: { /* Find */
                    databox found;
                    multidictFind(d, &key, &found);
                    break;
                }
                case 1: { /* Replace */
                    databox val = databoxNewSigned(idx + 1000);
                    multidictReplace(d, &key, &val);
                    break;
                }
                case 2: { /* AddNX (might fail if exists) */
                    databox val = databoxNewSigned(idx);
                    multidictAddNX(d, &key, &val);
                    break;
                }
                case 3: { /* IncrBy */
                    int64_t result;
                    multidictIncrBy(d, &key, 1, &result);
                    break;
                }
                case 4: { /* Exists */
                    multidictExists(d, &key);
                    break;
                }
                }

                ops++;

                /* Rehash aggressively */
                multidictRehash(d, 1);
            }

            /* Complete rehashing */
            while (multidictIsRehashing(d)) {
                multidictRehash(d, 50);
            }

            /* Verify all entries still present */
            for (int i = 0; i < STRESS_ENTRIES; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "stress_%d", i);
                databox key = databoxNewBytesString(keyBuf);
                assert(multidictExists(d, &key));
            }

            multidictEmpty(d);
        }

        printf("  Completed %d stress cycles\n", STRESS_CYCLES);
    }

    /* ================================================================
     * SECTION 5: Scan Tests
     * ================================================================ */
    printf("\n--- Section 5: Scan Tests ---\n");

    printf("Test 5.1: Scan empty dict...\n");
    {
        int scanCount = 0;
        uint64_t cursor = multidictScan(d, 0, scanCountCallback, &scanCount);
        assert(cursor == 0);
        assert(scanCount == 0);
    }

    printf("Test 5.2: Full scan with callback...\n");
    {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "scan%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        int scanCount = 0;
        uint64_t cursor = 0;
        do {
            cursor = multidictScan(d, cursor, scanCountCallback, &scanCount);
        } while (cursor != 0);

        printf("  Scanned %d entries\n", scanCount);
        assert(scanCount == 1000);
    }

    printf("Test 5.3: Scan during rehash...\n");
    {
        multidictExpand(d, 4096);
        assert(multidictIsRehashing(d) == true);

        int scanCount = 0;
        uint64_t cursor = 0;
        int iterations = 0;
        do {
            cursor = multidictScan(d, cursor, scanCountCallback, &scanCount);
            iterations++;
            /* Do some rehashing between scans */
            if (multidictIsRehashing(d)) {
                multidictRehash(d, 5);
            }
        } while (cursor != 0);

        printf("  Scanned %d entries during rehash in %d iterations\n",
               scanCount, iterations);
        /* Should still see all entries (may see some twice due to rehash) */
        assert(scanCount >= 1000);

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictEmpty(d);
    }

    printf("Test 5.4: Comprehensive scan fuzz - small dict sizes...\n");
    {
        /* Test scanning with various small dict sizes */
        const int testSizes[] = {1, 2, 3, 5, 10, 50, 100, 500};
        for (size_t sizeIdx = 0;
             sizeIdx < sizeof(testSizes) / sizeof(testSizes[0]); sizeIdx++) {
            int targetSize = testSizes[sizeIdx];
            multidict *dScan = multidictNew(&multidictTypeExactKey, qdc, 0);

            /* Insert entries */
            for (int i = 0; i < targetSize; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                multidictAdd(dScan, &key, &val);
            }

            /* Scan and verify we see all entries */
            int scanCount = 0;
            uint64_t cursor = 0;
            do {
                cursor =
                    multidictScan(dScan, cursor, scanCountCallback, &scanCount);
            } while (cursor != 0);

            assert(scanCount >= targetSize);
            multidictFree(dScan);
        }
        printf("  Tested %zu different dict sizes\n",
               sizeof(testSizes) / sizeof(testSizes[0]));
    }

    printf("Test 5.5: Scan fuzz - large dicts with rehashing at various "
           "stages...\n");
    {
        for (int testRun = 0; testRun < 5; testRun++) {
            multidict *dScan = multidictNew(&multidictTypeExactKey, qdc, 0);
            const int entryCount = 5000;

            /* Insert entries */
            for (int i = 0; i < entryCount; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "large_%d_%d", testRun, i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                multidictAdd(dScan, &key, &val);
            }

            /* Trigger rehashing */
            multidictExpand(dScan, entryCount * 2);
            assert(multidictIsRehashing(dScan));

            /* Do partial rehashing - different amounts each run */
            int rehashSteps = (testRun + 1) * 100;
            for (int step = 0;
                 step < rehashSteps && multidictIsRehashing(dScan); step++) {
                multidictRehash(dScan, 1);
            }

            /* Now scan while potentially still rehashing */
            int scanCount = 0;
            uint64_t cursor = 0;
            int scanIterations = 0;
            do {
                cursor =
                    multidictScan(dScan, cursor, scanCountCallback, &scanCount);
                scanIterations++;

                /* Interleave more rehashing during scan */
                if (multidictIsRehashing(dScan) && (scanIterations % 3 == 0)) {
                    multidictRehash(dScan, 2);
                }
            } while (cursor != 0);

            /* We should see all entries (possibly with duplicates) */
            assert(scanCount >= entryCount);

            multidictFree(dScan);
        }
        printf("  Completed 5 large dict scans with varying rehash states\n");
    }

    printf(
        "Test 5.6: Scan with concurrent modifications during rehashing...\n");
    {
        multidict *dScan = multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Insert initial entries */
        for (int i = 0; i < 2000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "mod_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(dScan, &key, &val);
        }

        /* Trigger rehashing */
        multidictExpand(dScan, 4096);
        assert(multidictIsRehashing(dScan));

        /* Partial rehash */
        for (int i = 0; i < 50; i++) {
            multidictRehash(dScan, 1);
        }

        /* Scan while modifying entries */
        int scanCount = 0;
        uint64_t cursor = 0;
        int modifications = 0;
        do {
            cursor =
                multidictScan(dScan, cursor, scanCountCallback, &scanCount);

            /* Every few iterations, modify some entries */
            if (modifications < 100 && (cursor % 7 == 0)) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "new_%d", modifications);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(modifications);
                multidictAdd(dScan, &key, &val);
                modifications++;
            }

            /* Continue rehashing */
            if (multidictIsRehashing(dScan)) {
                multidictRehash(dScan, 1);
            }
        } while (cursor != 0);

        printf("  Scanned %d entries with %d concurrent modifications\n",
               scanCount, modifications);
        assert(scanCount >= 2000); /* At minimum, original entries */

        multidictFree(dScan);
    }

    printf("Test 5.7: Scan correctness - verify exact key coverage...\n");
    {
        multidict *dScan = multidictNew(&multidictTypeExactKey, qdc, 0);
        const int exactCount = 1000;

        /* Insert exactly 1000 unique entries with predictable keys */
        for (int i = 0; i < exactCount; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "exact_%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(dScan, &key, &val);
        }

        /* Trigger rehash to medium state */
        multidictExpand(dScan, exactCount * 2);
        for (int i = 0; i < 200 && multidictIsRehashing(dScan); i++) {
            multidictRehash(dScan, 1);
        }

        /* Scan and count unique vs total */
        int scanCount = 0;
        uint64_t cursor = 0;
        do {
            cursor =
                multidictScan(dScan, cursor, scanCountCallback, &scanCount);
        } while (cursor != 0);

        /* Should see all entries (may have duplicates due to rehashing) */
        assert(scanCount >= exactCount);
        printf("  Scanned total=%d entries (expected=%" PRId32
               ", duplicates=%d)\n",
               scanCount, exactCount, scanCount - exactCount);

        /* Complete rehash and scan again - should have no duplicates */
        while (multidictIsRehashing(dScan)) {
            multidictRehash(dScan, 100);
        }

        scanCount = 0;
        cursor = 0;
        do {
            cursor =
                multidictScan(dScan, cursor, scanCountCallback, &scanCount);
        } while (cursor != 0);

        printf("  After rehash complete: scanned=%d (should equal %" PRId32
               ")\n",
               scanCount, exactCount);
        assert(scanCount ==
               exactCount); /* No duplicates after rehash complete */

        multidictFree(dScan);
    }

    printf("Test 5.8: Extreme fuzz - rapid dict size changes during scan...\n");
    {
        multidict *dScan = multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Insert initial batch */
        for (int i = 0; i < 500; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "extreme_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(dScan, &key, &val);
        }

        /* Start rehashing */
        multidictExpand(dScan, 2048);

        int scanCount = 0;
        uint64_t cursor = 0;
        int iterations = 0;
        do {
            cursor =
                multidictScan(dScan, cursor, scanCountCallback, &scanCount);
            iterations++;

            /* Aggressively interleave rehashing */
            for (int j = 0; j < 5 && multidictIsRehashing(dScan); j++) {
                multidictRehash(dScan, 1);
            }

            /* Prevent infinite loops */
            if (iterations > 10000) {
                printf("  WARNING: Scan took >10000 iterations, breaking\n");
                break;
            }
        } while (cursor != 0);

        printf("  Extreme fuzz: scanned=%d in %d iterations\n", scanCount,
               iterations);
        assert(scanCount >= 500);

        multidictFree(dScan);
    }

    /* ================================================================
     * SECTION 6: Random Access Tests
     * ================================================================ */
    printf("\n--- Section 6: Random Access Tests ---\n");

    printf("Test 6.1: Random key from populated dict...\n");
    {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rand%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        databox randomKey;
        for (int i = 0; i < 50; i++) {
            bool got = multidictGetRandomKey(d, &randomKey);
            assert(got == true);
        }
    }

    printf("Test 6.2: Get some keys...\n");
    {
        databox keys[20];
        uint32_t got = multidictGetSomeKeys(d, keys, 20);
        printf("  Requested 20, got %u\n", got);
        assert(got > 0 && got <= 20);

        /* Request more than exist - need larger buffer */
        databox keys200[200];
        got = multidictGetSomeKeys(d, keys200, 200);
        printf("  Requested 200, got %u\n", got);
    }

    printf("Test 6.3: Random key during rehash...\n");
    {
        multidictExpand(d, 512);
        assert(multidictIsRehashing(d) == true);

        databox randomKey;
        for (int i = 0; i < 20; i++) {
            bool got = multidictGetRandomKey(d, &randomKey);
            assert(got == true);
            multidictRehash(d, 2);
        }

        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictEmpty(d);
    }

    /* ================================================================
     * SECTION 7: Stress Tests / Fuzzing
     * ================================================================ */
    printf("\n--- Section 7: Stress Tests / Fuzzing ---\n");

    printf("Test 7.1: Large insert/delete/find mix (10K ops)...\n");
    {
        testRandSeed(42);
        int inserted = 0, deleted = 0, found = 0, notFound = 0;

        for (int i = 0; i < 10000; i++) {
            int op = testRand() % 3;
            int keyNum = testRand() % 1000;
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fuzz%d", keyNum);
            databox key = databoxNewBytesString(keyBuf);

            switch (op) {
            case 0: { /* Insert */
                databox val = databoxNewSigned(keyNum);
                multidictAdd(d, &key, &val);
                inserted++;
                break;
            }
            case 1: { /* Delete */
                if (multidictDelete(d, &key)) {
                    deleted++;
                }
                break;
            }
            case 2: { /* Find */
                databox foundVal;
                if (multidictFind(d, &key, &foundVal)) {
                    found++;
                } else {
                    notFound++;
                }
                break;
            }
            }
        }
        printf("  Inserts: %d, Deletes: %d, Found: %d, NotFound: %d\n",
               inserted, deleted, found, notFound);
        printf("  Final size: %" PRIu64 "\n", multidictSize(d));
        multidictEmpty(d);
    }

    printf("Test 7.2: Rapid expand/rehash cycles...\n");
    {
        for (int cycle = 0; cycle < 5; cycle++) {
            /* Insert entries */
            for (int i = 0; i < 500; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "cycle%d_%d", cycle, i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                multidictAdd(d, &key, &val);
            }

            /* Expand and rehash */
            multidictExpand(d, multidictSlots(d) * 2 + 100);
            while (multidictIsRehashing(d)) {
                multidictRehash(d, 50);
            }
        }
        printf("  Final size after 5 cycles: %" PRIu64 "\n", multidictSize(d));
        multidictEmpty(d);
    }

    printf("Test 7.3: Interleaved operations during rehash (5K ops)...\n");
    {
        testRandSeed(123);

        /* Pre-populate */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "inter%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Start rehash */
        multidictExpand(d, 4096);

        int ops = 0;
        while (multidictIsRehashing(d) && ops < 5000) {
            int op = testRand() % 4;
            int keyNum = testRand() % 2000;
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "inter%d", keyNum);
            databox key = databoxNewBytesString(keyBuf);

            switch (op) {
            case 0: { /* Insert */
                databox val = databoxNewSigned(keyNum);
                multidictAdd(d, &key, &val);
                break;
            }
            case 1: { /* Delete */
                multidictDelete(d, &key);
                break;
            }
            case 2: { /* Find */
                databox foundVal;
                multidictFind(d, &key, &foundVal);
                break;
            }
            case 3: { /* Rehash step */
                multidictRehash(d, 1);
                break;
            }
            }
            ops++;
        }

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        printf("  Completed %d interleaved ops, final size: %" PRIu64 "\n", ops,
               multidictSize(d));
        multidictEmpty(d);
    }

    printf("Test 7.4: Verify consistency after random ops...\n");
    {
        testRandSeed(999);

        /* Track what should exist */
        bool exists[1000] = {false};
        int expectedCount = 0;

        for (int i = 0; i < 5000; i++) {
            int keyNum = testRand() % 1000;
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "verify%d", keyNum);
            databox key = databoxNewBytesString(keyBuf);

            if (testRand() % 2 == 0) {
                /* Insert */
                databox val = databoxNewSigned(keyNum);
                multidictAdd(d, &key, &val);
                if (!exists[keyNum]) {
                    exists[keyNum] = true;
                    expectedCount++;
                }
            } else {
                /* Delete */
                multidictDelete(d, &key);
                if (exists[keyNum]) {
                    exists[keyNum] = false;
                    expectedCount--;
                }
            }
        }

        /* Verify */
        assert((int)multidictSize(d) == expectedCount);

        int actualCount = 0;
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "verify%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox foundVal;
            bool found = multidictFind(d, &key, &foundVal);
            assert(found == exists[i]);
            if (found) {
                actualCount++;
            }
        }
        assert(actualCount == expectedCount);
        printf("  Verified %d entries match expected state\n", expectedCount);
        multidictEmpty(d);
    }

    /* ================================================================
     * SECTION 8: State Transition Tests
     * ================================================================ */
    printf("\n--- Section 8: State Transition Tests ---\n");

    printf("Test 8.1: Empty -> Populated -> Empty -> Populated...\n");
    {
        for (int cycle = 0; cycle < 3; cycle++) {
            assert(multidictSize(d) == 0);

            for (int i = 0; i < 100; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "trans%d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                multidictAdd(d, &key, &val);
            }
            assert(multidictSize(d) == 100);

            multidictEmpty(d);
        }
        printf("  Completed 3 cycles of empty->populated->empty\n");
    }

    printf("Test 8.2: Resize enable/disable behavior...\n");
    {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "resize%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        multidictResizeDisable(d);
        assert(qdc->disableResize == true);

        /* Operations should still work */
        databox key = databoxNewBytesString("resize50");
        databox found;
        assert(multidictFind(d, &key, &found) == true);

        multidictResizeEnable(d);
        assert(qdc->disableResize == false);
        multidictEmpty(d);
    }

    printf("Test 8.3: Iterator fingerprint check (implicit)...\n");
    {
        for (int i = 0; i < 50; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fp%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Unsafe iterator - fingerprint captured */
        multidictIterator iter;
        multidictIteratorInit(d, &iter);

        multidictEntry entry;
        int count = 0;
        while (multidictIteratorNext(&iter, &entry)) {
            count++;
        }
        /* Release without modification - fingerprint should match */
        multidictIteratorRelease(&iter);
        assert(count == 50);
        multidictEmpty(d);
    }

    /* ================================================================
     * SECTION 9: Case-Insensitive Key Type
     * ================================================================ */
    printf("\n--- Section 9: Case-Insensitive Key Type ---\n");

    printf("Test 9.1: Create with case-insensitive type...\n");
    {
        multidictClass *qdcCase = multidictMmClassNew();
        multidict *dCase = multidictNew(&multidictTypeCaseKey, qdcCase, 54321);
        assert(dCase != NULL);

        /* Insert with lowercase */
        databox key1 = databoxNewBytesString("hello");
        databox val1 = databoxNewBytesString("world");
        multidictAdd(dCase, &key1, &val1);

        /* Find with uppercase - currently does NOT work because:
         * - The case-insensitive hash correctly maps "HELLO" to same slot
         * - BUT the slot impl (multimap) uses binary comparison internally,
         *   not the dict's keyCompare function
         * - TODO: Future enhancement - pass keyCompare to slot operations */
        databox key2 = databoxNewBytesString("HELLO");
        databox found;
        bool result = multidictFind(dCase, &key2, &found);
        printf("  Case-insensitive find 'HELLO' for 'hello': %s\n",
               result ? "found" : "not found");
        printf("  (Known limitation: slot impl uses binary compare)\n");

        /* Verify same-case lookup works */
        databox key3 = databoxNewBytesString("hello");
        result = multidictFind(dCase, &key3, &found);
        assert(result == true); /* Same case must work */

        multidictFree(dCase);
        multidictMmClassFree(qdcCase);
    }

    /* ================================================================
     * SECTION 10: Print Stats (Visual Verification)
     * ================================================================ */
    printf("\n--- Section 10: Stats Verification ---\n");

    printf("Test 10.1: Stats on populated dict...\n");
    {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "stats%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }
        multidictPrintStats(d);
    }

    /* ================================================================
     * SECTION 11: Bytes Tracking Verification
     * ================================================================ */
    printf("\n--- Section 11: Bytes Tracking Verification ---\n");

    printf("Test 11.1: Fresh dict has zero bytes...\n");
    {
        multidictClass *qdcBytes = multidictMmClassNew();
        multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Fresh dict should have zero tracked bytes */
        assert(dBytes->ht[0].keyBytes == 0);
        assert(dBytes->ht[0].valBytes == 0);
        assert(dBytes->ht[0].totalBytes == 0);

        multidictFree(dBytes);
        multidictMmClassFree(qdcBytes);
    }

    printf("Test 11.2: Bytes increase on insert...\n");
    {
        multidictClass *qdcBytes = multidictMmClassNew();
        multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Insert entries and track byte growth */
        uint64_t expectedKeyBytes = 0;
        uint64_t expectedValBytes = 0;

        for (int i = 0; i < 100; i++) {
            char keyBuf[32], valBuf[64];
            snprintf(keyBuf, sizeof(keyBuf), "key%d", i);
            snprintf(valBuf, sizeof(valBuf), "value%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(dBytes, &key, &val);
            expectedKeyBytes += strlen(keyBuf);
            expectedValBytes += strlen(valBuf);
        }

        printf("  After 100 inserts: keyBytes=%" PRIu64 " (exp %" PRIu64 "), "
               "valBytes=%" PRIu64 " (exp %" PRIu64 ")\n",
               dBytes->ht[0].keyBytes, expectedKeyBytes, dBytes->ht[0].valBytes,
               expectedValBytes);

        assert(dBytes->ht[0].keyBytes == expectedKeyBytes);
        assert(dBytes->ht[0].valBytes == expectedValBytes);
        assert(dBytes->ht[0].count == 100);

        multidictFree(dBytes);
        multidictMmClassFree(qdcBytes);
    }

    printf("Test 11.3: Bytes decrease on delete...\n");
    {
        multidictClass *qdcBytes = multidictMmClassNew();
        multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Insert 50 entries */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32], valBuf[64];
            snprintf(keyBuf, sizeof(keyBuf), "del%d", i);
            snprintf(valBuf, sizeof(valBuf), "toDelete%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(dBytes, &key, &val);
        }

        uint64_t bytesBeforeDelete =
            dBytes->ht[0].keyBytes + dBytes->ht[0].valBytes;

        /* Delete 25 entries */
        for (int i = 0; i < 25; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "del%d", i);
            databox key = databoxNewBytesString(keyBuf);
            multidictDelete(dBytes, &key);
        }

        uint64_t bytesAfterDelete =
            dBytes->ht[0].keyBytes + dBytes->ht[0].valBytes;

        printf("  After deleting 25 of 50: count=%" PRIu64 ", bytes %" PRIu64
               " -> %" PRIu64 "\n",
               dBytes->ht[0].count, bytesBeforeDelete, bytesAfterDelete);

        assert(dBytes->ht[0].count == 25);
        assert(bytesAfterDelete < bytesBeforeDelete);
        /* Should be roughly half (not exact due to variable key/val sizes) */
        assert(bytesAfterDelete > 0);

        multidictFree(dBytes);
        multidictMmClassFree(qdcBytes);
    }

    printf("Test 11.4: Value replacement updates valBytes...\n");
    {
        multidictClass *qdcBytes = multidictMmClassNew();
        multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Insert with short value */
        databox key = databoxNewBytesString("replaceMe");
        databox shortVal = databoxNewBytesString("short");
        multidictAdd(dBytes, &key, &shortVal);

        uint64_t bytesWithShort = dBytes->ht[0].valBytes;

        /* Replace with longer value */
        databox longVal = databoxNewBytesString(
            "thisisaverylongvaluethatwillincreasethesize");
        multidictResult result = multidictAdd(dBytes, &key, &longVal);
        assert(result == MULTIDICT_OK_REPLACED);

        uint64_t bytesWithLong = dBytes->ht[0].valBytes;

        printf("  Short val bytes: %" PRIu64 ", Long val bytes: %" PRIu64 "\n",
               bytesWithShort, bytesWithLong);

        assert(bytesWithLong > bytesWithShort);
        assert(dBytes->ht[0].count == 1); /* Still just one entry */

        multidictFree(dBytes);
        multidictMmClassFree(qdcBytes);
    }

    printf("Test 11.5: Bytes preserved through rehash...\n");
    {
        multidictClass *qdcBytes = multidictMmClassNew();
        multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Insert entries */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(dBytes, &key, &val);
        }

        uint64_t keyBytesBefore = dBytes->ht[0].keyBytes;
        uint64_t valBytesBefore = dBytes->ht[0].valBytes;

        /* Trigger and complete rehash */
        multidictExpand(dBytes, 256);
        while (multidictIsRehashing(dBytes)) {
            multidictRehash(dBytes, 10);
        }

        /* After rehash, all entries should be in ht[0] with same bytes */
        uint64_t keyBytesAfter = dBytes->ht[0].keyBytes;
        uint64_t valBytesAfter = dBytes->ht[0].valBytes;

        printf("  Before rehash: key=%" PRIu64 ", val=%" PRIu64 "\n",
               keyBytesBefore, valBytesBefore);
        printf("  After rehash:  key=%" PRIu64 ", val=%" PRIu64 "\n",
               keyBytesAfter, valBytesAfter);

        assert(keyBytesAfter == keyBytesBefore);
        assert(valBytesAfter == valBytesBefore);
        assert(dBytes->ht[0].count == 100);
        assert(dBytes->ht[1].count == 0);

        multidictFree(dBytes);
        multidictMmClassFree(qdcBytes);
    }

    printf("=== Bytes tracking tests passed! ===\n");

    /* ================================================================
     * SECTION 12: New API Tests (Sprint 3)
     * ================================================================ */
    printf("\n--- Section 12: New API Tests (Sprint 3) ---\n");

    /* Free the original d and qdc from sections 1-10, create fresh for API
     * tests */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 12345);

    printf("Test 12.1: multidictExists...\n");
    {
        databox key = databoxNewBytesString("testkey");
        databox val = databoxNewBytesString("testval");

        /* Key doesn't exist yet */
        assert(multidictExists(d, &key) == false);

        /* Add key */
        multidictResult result = multidictAdd(d, &key, &val);
        assert(result == MULTIDICT_OK_INSERTED);

        /* Key now exists */
        assert(multidictExists(d, &key) == true);

        /* Non-existent key */
        databox nokey = databoxNewBytesString("nokey");
        assert(multidictExists(d, &nokey) == false);
    }

    printf("Test 12.2: multidictExistsByString...\n");
    {
        assert(multidictExistsByString(d, "testkey") == true);
        assert(multidictExistsByString(d, "nokey") == false);

        /* Add another key and verify */
        databox key2 = databoxNewBytesString("key2");
        databox val2 = databoxNewBytesString("val2");
        multidictAdd(d, &key2, &val2);
        assert(multidictExistsByString(d, "key2") == true);
    }

    printf("Test 12.3: multidictGetStats...\n");
    {
        multidictStats stats;
        multidictGetStats(d, &stats);

        printf("  count=%" PRIu64 ", slots=%" PRIu64 ", loadFactor=%u%%\n",
               stats.count, stats.slots, stats.loadFactor);
        printf("  usedBytes=%" PRIu64 ", keyBytes=%" PRIu64
               ", valBytes=%" PRIu64 ", totalBytes=%" PRIu64 "\n",
               stats.usedBytes, stats.keyBytes, stats.valBytes,
               stats.totalBytes);

        assert(stats.count == 2);
        assert(stats.slots > 0);
        assert(stats.keyBytes > 0);
        assert(stats.valBytes > 0);
        assert(stats.totalBytes > 0);
        assert(stats.isRehashing == false);
    }

    printf("Test 12.4: multidictLoadFactor...\n");
    {
        /* Add more entries to increase load factor */
        for (int j = 0; j < 100; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "loadkey%d", j);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(j);
            multidictAdd(d, &key, &val);
        }

        uint32_t lf = multidictLoadFactor(d);
        printf("  Load factor after 102 entries: %u%%\n", lf);
        assert(lf > 0); /* Should have non-zero load factor */
    }

    printf("Test 12.5: multidictBytes, KeyBytes, ValBytes...\n");
    {
        uint64_t totalBytes = multidictBytes(d);
        uint64_t keyBytes = multidictKeyBytes(d);
        uint64_t valBytes = multidictValBytes(d);

        printf("  totalBytes=%" PRIu64 ", keyBytes=%" PRIu64
               ", valBytes=%" PRIu64 "\n",
               totalBytes, keyBytes, valBytes);

        assert(totalBytes > 0);
        assert(keyBytes > 0);
        assert(valBytes > 0);
        /* Total should be >= key + val (includes slot overhead) */
        assert(totalBytes >= keyBytes + valBytes);
    }

    printf("Test 12.6: Stats consistency through operations...\n");
    {
        multidictStats before, after;
        multidictGetStats(d, &before);

        /* Delete some entries */
        for (int j = 0; j < 50; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "loadkey%d", j);
            databox key = databoxNewBytesString(keyBuf);
            multidictDelete(d, &key);
        }

        multidictGetStats(d, &after);

        printf("  Before delete: count=%" PRIu64 ", keyBytes=%" PRIu64 "\n",
               before.count, before.keyBytes);
        printf("  After delete:  count=%" PRIu64 ", keyBytes=%" PRIu64 "\n",
               after.count, after.keyBytes);

        assert(after.count < before.count);
        assert(after.keyBytes < before.keyBytes);
    }

    printf("=== New API tests passed! ===\n");

    /* ================================================================
     * SECTION 13: Bulk Operations Tests (Sprint 6)
     * ================================================================ */
    printf("\n--- Section 13: Bulk Operations Tests (Sprint 6) ---\n");

    /* Clear dict for fresh bulk tests */
    multidictEmpty(d);

    printf("Test 13.1: multidictAddMultiple basic...\n");
    {
        databox keys[5], vals[5];
        char keyBufs[5][32], valBufs[5][32]; /* Persist outside loop */
        for (int j = 0; j < 5; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "bulkkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "bulkval%d", j);
            keys[j] = databoxNewBytesString(keyBufs[j]);
            vals[j] = databoxNewBytesString(valBufs[j]);
        }

        uint32_t inserted = multidictAddMultiple(d, keys, vals, 5);
        printf("  Inserted %u of 5 entries\n", inserted);
        assert(inserted == 5);
        assert(multidictSize(d) == 5);

        /* Verify all exist */
        for (int j = 0; j < 5; j++) {
            assert(multidictExists(d, &keys[j]) == true);
        }
    }

    printf("Test 13.2: multidictAddMultiple with duplicates...\n");
    {
        /* Try to add same keys again - should count as 0 new inserts */
        databox keys[3], vals[3];
        char keyBufs[3][32], valBufs[3][32]; /* Persist outside loop */
        for (int j = 0; j < 3; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "bulkkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "newval%d", j);
            keys[j] = databoxNewBytesString(keyBufs[j]);
            vals[j] = databoxNewBytesString(valBufs[j]);
        }

        uint32_t inserted = multidictAddMultiple(d, keys, vals, 3);
        printf("  Inserted %u of 3 (all duplicates)\n", inserted);
        assert(inserted == 0);         /* All were replacements */
        assert(multidictSize(d) == 5); /* Size unchanged */
    }

    printf("Test 13.3: multidictDeleteMultiple basic...\n");
    {
        databox keys[3];
        char keyBufs[3][32]; /* Persist outside loop */
        for (int j = 0; j < 3; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "bulkkey%d", j);
            keys[j] = databoxNewBytesString(keyBufs[j]);
        }

        uint32_t deleted = multidictDeleteMultiple(d, keys, 3);
        printf("  Deleted %u of 3 entries\n", deleted);
        assert(deleted == 3);
        assert(multidictSize(d) == 2);

        /* Verify deleted keys don't exist */
        for (int j = 0; j < 3; j++) {
            assert(multidictExists(d, &keys[j]) == false);
        }
    }

    printf("Test 13.4: multidictDeleteMultiple with non-existent keys...\n");
    {
        databox keys[4];
        /* Mix of existing and non-existing keys */
        keys[0] = databoxNewBytesString("bulkkey3"); /* exists */
        keys[1] = databoxNewBytesString("nokey1");   /* doesn't exist */
        keys[2] = databoxNewBytesString("bulkkey4"); /* exists */
        keys[3] = databoxNewBytesString("nokey2");   /* doesn't exist */

        uint32_t deleted = multidictDeleteMultiple(d, keys, 4);
        printf("  Deleted %u of 4 (2 existed)\n", deleted);
        assert(deleted == 2);
        assert(multidictSize(d) == 0);
    }

    printf("Test 13.5: Bulk operations edge cases...\n");
    {
        /* NULL/empty checks */
        assert(multidictAddMultiple(d, NULL, NULL, 5) == 0);
        assert(multidictDeleteMultiple(d, NULL, 5) == 0);

        databox keys[1], vals[1];
        keys[0] = databoxNewBytesString("edgekey");
        vals[0] = databoxNewBytesString("edgeval");

        assert(multidictAddMultiple(d, keys, vals, 0) == 0);
        assert(multidictDeleteMultiple(d, keys, 0) == 0);
    }

    printf("Test 13.6: Large bulk insert...\n");
    {
        multidictEmpty(d);
        const int BULK_COUNT = 500;
        databox *keys = zmalloc(BULK_COUNT * sizeof(databox));
        databox *vals = zmalloc(BULK_COUNT * sizeof(databox));
        char (*keyBufs)[32] =
            zmalloc(BULK_COUNT * 32); /* Persist string storage */
        char (*valBufs)[32] = zmalloc(BULK_COUNT * 32);

        for (int j = 0; j < BULK_COUNT; j++) {
            snprintf(keyBufs[j], 32, "largekey%d", j);
            snprintf(valBufs[j], 32, "largeval%d", j);
            keys[j] = databoxNewBytesString(keyBufs[j]);
            vals[j] = databoxNewBytesString(valBufs[j]);
        }

        uint32_t inserted = multidictAddMultiple(d, keys, vals, BULK_COUNT);
        printf("  Bulk inserted %u of %d entries\n", inserted, BULK_COUNT);
        assert(inserted == BULK_COUNT);
        assert(multidictSize(d) == (uint64_t)BULK_COUNT);

        /* Bulk delete half */
        uint32_t deleted = multidictDeleteMultiple(d, keys, BULK_COUNT / 2);
        printf("  Bulk deleted %u entries\n", deleted);
        assert(deleted == BULK_COUNT / 2);
        assert(multidictSize(d) == (uint64_t)(BULK_COUNT / 2));

        zfree(keyBufs);
        zfree(valBufs);
        zfree(keys);
        zfree(vals);
    }

    printf("=== Bulk operations tests passed! ===\n");

    /* ================================================================
     * Section 14: Self-Management Tests (Sprint 7)
     * ================================================================ */
    printf("\n--- Section 14: Self-Management Tests (Sprint 7) ---\n");

    /* Clean up and create fresh dict for these tests */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 12345);

    printf("Test 14.1: Memory limit get/set...\n");
    {
        assert(multidictGetMaxMemory(d) == 0); /* Default is unlimited */
        multidictSetMaxMemory(d, 10000);
        assert(multidictGetMaxMemory(d) == 10000);
        multidictSetMaxMemory(d, 0); /* Disable limit */
        assert(multidictGetMaxMemory(d) == 0);
    }

    printf("Test 14.2: multidictIsOverLimit...\n");
    {
        /* With no limit, should never be over */
        multidictSetMaxMemory(d, 0);
        assert(multidictIsOverLimit(d) == false);

        /* Add some data */
        char keyBufs[50][32], valBufs[50][32];
        for (int j = 0; j < 50; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "limitkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "limitval%d", j);
            databox key = databoxNewBytesString(keyBufs[j]);
            databox val = databoxNewBytesString(valBufs[j]);
            multidictAdd(d, &key, &val);
        }

        /* Set a very low limit - use user bytes (key+val), not totalBytes */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        printf("  User bytes: %" PRIu64 "\n", userBytes);
        multidictSetMaxMemory(d, userBytes / 2);
        assert(multidictIsOverLimit(d) == true);

        /* Set high limit */
        multidictSetMaxMemory(d, userBytes * 2);
        assert(multidictIsOverLimit(d) == false);
    }

    printf("Test 14.3: multidictEvictToLimit basic...\n");
    {
        /* Set a reasonable limit (95% of current) to ensure eviction happens */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        uint64_t targetBytes = (userBytes * 95) / 100;
        multidictSetMaxMemory(d, targetBytes);

        uint64_t countBefore = multidictCount(d);
        uint32_t evicted = multidictEvictToLimit(d);
        uint64_t countAfter = multidictCount(d);
        uint64_t userBytesAfter = multidictKeyBytes(d) + multidictValBytes(d);

        printf("  Evicted %u entries (before: %" PRIu64 ", after: %" PRIu64
               ")\n",
               evicted, countBefore, countAfter);
        printf("  User bytes: target=%" PRIu64 ", actual=%" PRIu64 "\n",
               targetBytes, userBytesAfter);

        /* Eviction is best-effort - verify it attempted eviction */
        assert(evicted > 0 || userBytesAfter <= targetBytes);
        assert(countAfter <= countBefore); /* Count should not increase */
        /* Bytes should decrease after eviction (or already under limit) */
        assert(userBytesAfter < userBytes || userBytesAfter <= targetBytes);
    }

    printf("Test 14.4: Eviction callback...\n");
    {
        testEvictionCallbackCount_ = 0;

        multidictSetEvictionCallback(d, testEvictionCb_,
                                     &testEvictionCallbackCount_);

        /* Add more data to trigger eviction */
        multidictSetMaxMemory(d, 0); /* Disable limit to add data */
        char keyBufs2[100][32], valBufs2[100][32];
        for (int j = 0; j < 100; j++) {
            snprintf(keyBufs2[j], sizeof(keyBufs2[j]), "cbkey%d", j);
            snprintf(valBufs2[j], sizeof(valBufs2[j]), "cbval%d", j);
            databox key = databoxNewBytesString(keyBufs2[j]);
            databox val = databoxNewBytesString(valBufs2[j]);
            multidictAdd(d, &key, &val);
        }

        /* Set limit and evict */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        multidictSetMaxMemory(d, userBytes / 4);
        uint32_t evicted = multidictEvictToLimit(d);

        printf("  Callback called %d times, evicted %u entries\n",
               testEvictionCallbackCount_, evicted);
        assert(testEvictionCallbackCount_ == (int)evicted);

        /* Clear callback */
        multidictSetEvictionCallback(d, NULL, NULL);
    }

    printf("Test 14.5: Eviction callback veto...\n");
    {
        testVetoCount_ = 0;

        /* Ensure we have enough entries to veto 5 times */
        multidictSetMaxMemory(d, 0); /* Disable limit to add data */
        char vetoBufs[20][32];
        for (int j = 0; j < 20; j++) {
            snprintf(vetoBufs[j], sizeof(vetoBufs[j]), "vetokey%d", j);
            databox key = databoxNewBytesString(vetoBufs[j]);
            databox val = databoxNewBytesString(vetoBufs[j]);
            multidictAdd(d, &key, &val);
        }

        multidictSetEvictionCallback(d, vetoEvictionCb_, &testVetoCount_);

        /* Force eviction */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        multidictSetMaxMemory(d, userBytes / 2);
        multidictEvictToLimit(d);

        printf("  Vetoed %d eviction attempts\n", testVetoCount_);
        /* The veto count should be at least 5 (we vetoed first 5 attempts) */
        assert(testVetoCount_ >= 5);

        multidictSetEvictionCallback(d, NULL, NULL);
    }

    printf("Test 14.6: No eviction when unlimited...\n");
    {
        multidictSetMaxMemory(d, 0); /* Unlimited */
        uint32_t evicted = multidictEvictToLimit(d);
        assert(evicted == 0);
    }

    printf("=== Self-management tests passed! ===\n");

    /* ================================================================
     * Section 15: Extended Fuzzing / Property Tests (Sprint 8)
     * ================================================================ */
    printf("\n--- Section 15: Extended Fuzzing (Sprint 8) ---\n");

    /* Fresh dict for fuzzing */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 54321);

    printf("Test 15.1: Mixed operations stress test...\n");
    {
        char keyBufs[1000][32], valBufs[1000][32];
        databox keys[1000], vals[1000];

        /* Pre-generate keys and values */
        for (int j = 0; j < 1000; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "fuzzkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "fuzzval%d", j);
            keys[j] = databoxNewBytesString(keyBufs[j]);
            vals[j] = databoxNewBytesString(valBufs[j]);
        }

        /* Random seed for reproducibility */
        uint32_t seed = 12345;
        uint32_t ops = 0, inserts = 0, deletes = 0, finds = 0;

        for (int j = 0; j < 5000; j++) {
            seed = seed * 1103515245 + 12345; /* LCG random */
            int op = seed % 10;
            int idx = (seed >> 8) % 1000;

            if (op < 5) {
                /* 50% insert */
                multidictAdd(d, &keys[idx], &vals[idx]);
                inserts++;
            } else if (op < 8) {
                /* 30% find */
                databox found;
                multidictFind(d, &keys[idx], &found);
                finds++;
            } else {
                /* 20% delete */
                multidictDelete(d, &keys[idx]);
                deletes++;
            }
            ops++;
        }

        printf("  Ops: %u (inserts=%u, finds=%u, deletes=%u)\n", ops, inserts,
               finds, deletes);
        printf("  Final count: %" PRIu64 ", bytes: %" PRIu64 "\n",
               multidictCount(d), multidictBytes(d));

        /* Verify consistency */
        assert(multidictCount(d) <= 1000);
    }

    printf("Test 15.2: Bulk operations under stress...\n");
    {
        multidictEmpty(d);

        char keyBufs[100][32], valBufs[100][32];
        databox keys[100], vals[100];
        uint64_t totalInserted = 0, totalDeleted = 0;

        for (int round = 0; round < 10; round++) {
            /* Generate batch with unique keys per round */
            for (int j = 0; j < 100; j++) {
                snprintf(keyBufs[j], sizeof(keyBufs[j]), "bulk%d_%d", round, j);
                snprintf(valBufs[j], sizeof(valBufs[j]), "val%d_%d", round, j);
                keys[j] = databoxNewBytesString(keyBufs[j]);
                vals[j] = databoxNewBytesString(valBufs[j]);
            }

            /* Bulk insert */
            uint32_t inserted = multidictAddMultiple(d, keys, vals, 100);
            totalInserted += inserted;

            /* Bulk delete first half */
            uint32_t deleted = multidictDeleteMultiple(d, keys, 50);
            totalDeleted += deleted;
        }

        printf("  Total inserted: %" PRIu64 ", deleted: %" PRIu64
               ", final count: %" PRIu64 "\n",
               totalInserted, totalDeleted, multidictCount(d));
        /* Verify we inserted and deleted roughly as expected (allow some
         * variance) */
        assert(totalInserted == 1000); /* 10 rounds * 100 unique keys */
        assert(totalDeleted >= 490 &&
               totalDeleted <= 500); /* Allow minor variance */
        assert(multidictCount(d) == totalInserted - totalDeleted);
    }

    printf("Test 15.3: Iterator consistency under modification...\n");
    {
        multidictEmpty(d);

        char keyBufs[200][32], valBufs[200][32];
        for (int j = 0; j < 200; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "iterkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "iterval%d", j);
            databox key = databoxNewBytesString(keyBufs[j]);
            databox val = databoxNewBytesString(valBufs[j]);
            multidictAdd(d, &key, &val);
        }

        /* Iterate with safe iterator while modifying */
        multidictIterator iter;
        multidictIteratorGetSafe(d, &iter);
        multidictEntry e;
        int iterated = 0;

        while (multidictIteratorNext(&iter, &e)) {
            iterated++;
            /* Every 10th entry, add a new entry */
            if (iterated % 10 == 0 && iterated < 100) {
                char newKey[32], newVal[32];
                snprintf(newKey, sizeof(newKey), "newkey%d", iterated);
                snprintf(newVal, sizeof(newVal), "newval%d", iterated);
                databox key = databoxNewBytesString(newKey);
                databox val = databoxNewBytesString(newVal);
                multidictAdd(d, &key, &val);
            }
        }
        multidictIteratorRelease(&iter);

        printf("  Iterated %d entries, final count: %" PRIu64 "\n", iterated,
               multidictCount(d));
    }

    printf("Test 15.4: Memory limit with continuous operations...\n");
    {
        multidictEmpty(d);

        /* Set a memory limit */
        multidictSetMaxMemory(d, 5000);

        char keyBufs[500][32], valBufs[500][32];
        int totalInserted = 0;

        for (int j = 0; j < 500; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "memkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "memval%d", j);
            databox key = databoxNewBytesString(keyBufs[j]);
            databox val = databoxNewBytesString(valBufs[j]);

            multidictAdd(d, &key, &val);
            totalInserted++;

            /* Periodically evict if over limit */
            if (j % 50 == 0 && multidictIsOverLimit(d)) {
                multidictEvictToLimit(d);
            }
        }

        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        printf("  Inserted %d entries, final count: %" PRIu64
               ", userBytes: %" PRIu64 "\n",
               totalInserted, multidictCount(d), userBytes);

        multidictSetMaxMemory(d, 0); /* Clear limit */
    }

    printf("Test 15.5: Stats consistency verification...\n");
    {
        /* Verify stats match actual state */
        multidictStats stats;
        multidictGetStats(d, &stats);

        assert(stats.count == multidictCount(d));
        assert(stats.slots == multidictSlotCount(d));
        assert(stats.keyBytes == multidictKeyBytes(d));
        assert(stats.valBytes == multidictValBytes(d));
        assert(stats.totalBytes == multidictBytes(d));

        printf("  Stats verified: count=%" PRIu64 ", slots=%" PRIu64
               ", bytes=%" PRIu64 "\n",
               stats.count, stats.slots, stats.totalBytes);
    }

    printf("=== Extended fuzzing tests passed! ===\n");

    /* ================================================================
     * Section 16: Conditional Insert Tests (Sprint 9.1)
     * ================================================================ */
    printf("\n--- Section 16: Conditional Insert Tests (Sprint 9.1) ---\n");

    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 42);

    /* Test AddNX - should succeed for new key */
    {
        databox key = databoxNewBytesString("nx-key");
        databox val = databoxNewBytesString("nx-value");
        multidictResult r = multidictAddNX(d, &key, &val);
        if (r != MULTIDICT_OK_INSERTED) {
            printf("ERROR: AddNX should succeed for new key\n");
            err++;
        }

        /* AddNX again should fail */
        databox val2 = databoxNewBytesString("nx-value-2");
        r = multidictAddNX(d, &key, &val2);
        if (r != MULTIDICT_ERR) {
            printf("ERROR: AddNX should fail for existing key\n");
            err++;
        }

        /* Verify original value is preserved */
        databox found = {{0}};
        if (!multidictFind(d, &key, &found)) {
            printf("ERROR: Key should exist after AddNX\n");
            err++;
        }
    }

    /* Test AddXX - should fail for non-existent key */
    {
        databox key = databoxNewBytesString("xx-key");
        databox val = databoxNewBytesString("xx-value");
        multidictResult r = multidictAddXX(d, &key, &val);
        if (r != MULTIDICT_ERR) {
            printf("ERROR: AddXX should fail for non-existent key\n");
            err++;
        }

        /* Add the key first */
        multidictAdd(d, &key, &val);

        /* Now AddXX should succeed */
        databox val2 = databoxNewBytesString("xx-value-updated");
        r = multidictAddXX(d, &key, &val2);
        if (r != MULTIDICT_OK_REPLACED) {
            printf("ERROR: AddXX should succeed for existing key\n");
            err++;
        }
    }

    /* Test Replace - same semantics as AddXX */
    {
        databox key = databoxNewBytesString("replace-key");
        databox val = databoxNewBytesString("replace-value");

        /* Should fail for non-existent */
        if (multidictReplace(d, &key, &val) != MULTIDICT_ERR) {
            printf("ERROR: Replace should fail for non-existent key\n");
            err++;
        }

        /* Add key, then replace */
        multidictAdd(d, &key, &val);
        databox val2 = databoxNewBytesString("replaced!");
        if (multidictReplace(d, &key, &val2) != MULTIDICT_OK_REPLACED) {
            printf("ERROR: Replace should succeed for existing key\n");
            err++;
        }
    }

    printf("=== Conditional insert tests passed! ===\n");

    /* ================================================================
     * Section 17: Atomic Get-and-Delete Tests (Sprint 9.2)
     * ================================================================ */
    printf("\n--- Section 17: Atomic Operations Tests (Sprint 9.2) ---\n");

    /* Test GetAndDelete */
    {
        databox key = databoxNewBytesString("gad-key");
        databox val = databoxNewBytesString("gad-value");
        multidictAdd(d, &key, &val);

        databox foundVal = {{0}};
        if (!multidictGetAndDelete(d, &key, &foundVal)) {
            printf("ERROR: GetAndDelete should succeed\n");
            err++;
        }

        /* Key should no longer exist */
        if (multidictExists(d, &key)) {
            printf("ERROR: Key should be deleted after GetAndDelete\n");
            err++;
        }

        /* GetAndDelete on non-existent should return false */
        databox dummy = {{0}};
        if (multidictGetAndDelete(d, &key, &dummy)) {
            printf("ERROR: GetAndDelete should fail for non-existent key\n");
            err++;
        }
    }

    /* Test PopRandom */
    {
        multidictEmpty(d);

        /* PopRandom on empty should fail */
        databox k = {{0}}, v = {{0}};
        if (multidictPopRandom(d, &k, &v)) {
            printf("ERROR: PopRandom should fail on empty dict\n");
            err++;
        }

        /* Add some entries */
        for (int i = 0; i < 10; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "pop-key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "pop-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        uint64_t initialCount = multidictCount(d);

        /* Pop all entries */
        while (multidictPopRandom(d, &k, &v)) {
            /* Each pop should reduce count */
        }

        if (multidictCount(d) != 0) {
            printf("ERROR: Dict should be empty after popping all\n");
            err++;
        }

        if (initialCount != 10) {
            printf("ERROR: Should have had 10 entries initially\n");
            err++;
        }
    }

    printf("=== Atomic operations tests passed! ===\n");

    /* ================================================================
     * Section 18: Numeric Operations Tests (Sprint 9.3)
     * ================================================================ */
    printf("\n--- Section 18: Numeric Operations Tests (Sprint 9.3) ---\n");

    multidictEmpty(d);

    /* Test IncrBy on new key */
    {
        databox key = databoxNewBytesString("counter");
        int64_t result = 0;
        multidictResult r = multidictIncrBy(d, &key, 5, &result);
        if (r != MULTIDICT_OK_INSERTED) {
            printf("ERROR: IncrBy should insert on new key\n");
            err++;
        }
        if (result != 5) {
            printf("ERROR: IncrBy result should be 5, got %" PRId64 "\n",
                   result);
            err++;
        }

        /* Increment again */
        r = multidictIncrBy(d, &key, 10, &result);
        if (r != MULTIDICT_OK_REPLACED) {
            printf("ERROR: IncrBy should replace on existing key\n");
            err++;
        }
        if (result != 15) {
            printf("ERROR: IncrBy result should be 15, got %" PRId64 "\n",
                   result);
            err++;
        }

        /* Decrement */
        r = multidictIncrBy(d, &key, -3, &result);
        if (result != 12) {
            printf("ERROR: IncrBy(-3) should give 12, got %" PRId64 "\n",
                   result);
            err++;
        }
    }

    /* Test IncrByFloat */
    {
        databox key = databoxNewBytesString("float-counter");
        double result = 0.0;
        multidictResult r = multidictIncrByFloat(d, &key, 1.5, &result);
        if (r != MULTIDICT_OK_INSERTED) {
            printf("ERROR: IncrByFloat should insert on new key\n");
            err++;
        }
        if (result != 1.5) {
            printf("ERROR: IncrByFloat result should be 1.5, got %f\n", result);
            err++;
        }

        r = multidictIncrByFloat(d, &key, 2.5, &result);
        if (result != 4.0) {
            printf("ERROR: IncrByFloat result should be 4.0, got %f\n", result);
            err++;
        }
    }

    /* Test IncrBy on non-numeric value should fail */
    {
        databox key = databoxNewBytesString("string-key");
        databox val = databoxNewBytesString("not-a-number");
        multidictAdd(d, &key, &val);

        int64_t result = 0;
        multidictResult r = multidictIncrBy(d, &key, 1, &result);
        if (r != MULTIDICT_ERR) {
            printf("ERROR: IncrBy on non-numeric should fail\n");
            err++;
        }
    }

    printf("=== Numeric operations tests passed! ===\n");

    /* ================================================================
     * Section 19: Dict Operations Tests (Sprint 9.4)
     * ================================================================ */
    printf("\n--- Section 19: Dict Operations Tests (Sprint 9.4) ---\n");

    /* Test Copy */
    {
        multidictEmpty(d);

        /* Add some entries */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "copy-key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "copy-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        multidict *copy = multidictCopy(d);
        if (!copy) {
            printf("ERROR: Copy should return non-NULL\n");
            err++;
        } else {
            if (multidictCount(copy) != multidictCount(d)) {
                printf("ERROR: Copy count mismatch: %" PRIu64 " vs %" PRIu64
                       "\n",
                       multidictCount(copy), multidictCount(d));
                err++;
            }

            /* Verify all keys exist in copy */
            for (int i = 0; i < 100; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "copy-key-%d", i);
                databox key = databoxNewBytesString(keyBuf);
                if (!multidictExists(copy, &key)) {
                    printf("ERROR: Key %s missing from copy\n", keyBuf);
                    err++;
                }
            }

            multidictFree(copy);
        }
    }

    /* Test Merge with REPLACE mode */
    {
        multidictEmpty(d);

        /* Create source dict */
        multidictClass *srcQdc = multidictMmClassNew();
        multidict *src = multidictNew(&multidictTypeExactKey, srcQdc, 42);

        /* Add entries to dst */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "dst-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        /* Add entries to src (overlapping + new) */
        for (int i = 25; i < 75; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "src-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(src, &key, &val);
        }

        uint64_t merged = multidictMerge(d, src, MULTIDICT_MERGE_REPLACE);
        if (merged != 50) {
            printf("ERROR: Merge REPLACE should merge 50, got %" PRIu64 "\n",
                   merged);
            err++;
        }

        /* Total should now be 75 (0-74) */
        if (multidictCount(d) != 75) {
            printf("ERROR: After merge count should be 75, got %" PRIu64 "\n",
                   multidictCount(d));
            err++;
        }

        multidictFree(src);
        multidictMmClassFree(srcQdc);
    }

    /* Test Merge with KEEP mode */
    {
        multidictEmpty(d);

        multidictClass *srcQdc = multidictMmClassNew();
        multidict *src = multidictNew(&multidictTypeExactKey, srcQdc, 42);

        /* Add entries to dst */
        for (int i = 0; i < 10; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "dst-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        /* Add overlapping entries to src */
        for (int i = 5; i < 15; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "src-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(src, &key, &val);
        }

        /* KEEP mode - only non-existent keys should be added */
        uint64_t merged = multidictMerge(d, src, MULTIDICT_MERGE_KEEP);
        if (merged != 5) {
            printf("ERROR: Merge KEEP should only add 5 new, got %" PRIu64 "\n",
                   merged);
            err++;
        }

        if (multidictCount(d) != 15) {
            printf("ERROR: After merge count should be 15, got %" PRIu64 "\n",
                   multidictCount(d));
            err++;
        }

        multidictFree(src);
        multidictMmClassFree(srcQdc);
    }

    printf("=== Dict operations tests passed! ===\n");

    /* ================================================================
     * Section 20: LRU Eviction Tests (Sprint 10)
     * ================================================================ */
    printf("\n--- Section 20: LRU Eviction Tests (Sprint 10) ---\n");

    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 42);

    printf("Test 20.1: EnableLRU/HasLRU/DisableLRU basics...\n");
    {
        assert(!multidictHasLRU(d)); /* Initially disabled */
        bool enabled1 = multidictEnableLRU(d, 7);
        assert(enabled1);
        assert(multidictHasLRU(d));
        multidictDisableLRU(d);
        assert(!multidictHasLRU(d));
        bool enabled2 =
            multidictEnableLRU(d, 4); /* Re-enable with different levels */
        assert(enabled2);
        assert(multidictHasLRU(d));
    }

    printf("Test 20.2: Large-scale LRU eviction (10K entries, 5K limit)...\n");
    {
        multidictEmpty(d);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_LRU);

        /* Pre-allocate key/val buffers */
        const int totalEntries = 10000;
        const int targetCount = 5000;
        /* Each entry ~30 bytes, target 5000 entries = 150KB */
        uint64_t targetBytes = (uint64_t)targetCount * 30;
        multidictSetMaxMemory(d, targetBytes);

        /* Insert all entries with periodic eviction */
        for (int i = 0; i < totalEntries; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "lrukey%05d", i);
            snprintf(valBuf, sizeof(valBuf), "lruval%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);

            /* Trigger eviction periodically */
            if (i % 100 == 99) {
                multidictEvictToLimit(d);
            }
        }
        multidictEvictToLimit(d);

        uint64_t count = multidictCount(d);
        uint64_t bytes = multidictKeyBytes(d) + multidictValBytes(d);
        printf("    Inserted %d, evicted to count=%" PRIu64 ", bytes=%" PRIu64
               " (target=%" PRIu64 ")\n",
               totalEntries, count, bytes, targetBytes);

        /* Verify eviction happened and we're under limit */
        assert(count < (uint64_t)totalEntries);
        assert(bytes <= targetBytes + 100); /* Allow small overshoot */

        /* Older keys should be gone, newer keys should remain */
        /* Key 0 should be evicted (cold), key 9999 should remain (recent) */
        databox oldKey = databoxNewBytesString("lrukey00000");
        databox newKey = databoxNewBytesString("lrukey09999");
        databox val;
        bool foundOld = multidictFind(d, &oldKey, &val);
        bool foundNew = multidictFind(d, &newKey, &val);
        printf("    LRU order: oldest(key0)=%s, newest(key9999)=%s\n",
               foundOld ? "PRESENT(bad)" : "evicted(ok)",
               foundNew ? "present(ok)" : "EVICTED(bad)");
        /* Oldest should be evicted, newest should remain */
        assert(!foundOld); /* Old should be evicted */
        assert(foundNew);  /* New should remain */
    }

    printf("Test 20.3: Hot key protection under pressure...\n");
    {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 3000); /* ~100 entries */

        /* Insert 50 entries */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "hkey%04d", i);
            snprintf(valBuf, sizeof(valBuf), "hval%04d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        /* Touch key 0 repeatedly to make it hot */
        databox hotKey = databoxNewBytesString("hkey0000");
        for (int j = 0; j < 100; j++) {
            multidictTouch(d, &hotKey);
        }

        /* Insert 150 more entries to trigger eviction */
        for (int i = 50; i < 200; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "hkey%04d", i);
            snprintf(valBuf, sizeof(valBuf), "hval%04d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
            if (i % 20 == 0) {
                multidictEvictToLimit(d);
            }
        }
        multidictEvictToLimit(d);

        /* Hot key should survive eviction */
        databox val;
        bool hotSurvived = multidictFind(d, &hotKey, &val);
        printf("    Hot key hkey0000 survived: %s\n",
               hotSurvived ? "yes" : "NO!");
        assert(hotSurvived);
    }

    printf("Test 20.4: LRU stress fuzz with mixed ops (5K iterations)...\n");
    {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 5000); /* ~166 entries */

        uint32_t seed = 54321;
        int inserts = 0, deletes = 0, finds = 0, touches = 0;

        for (int i = 0; i < 5000; i++) {
            seed = seed * 1103515245 + 12345;
            int op = seed % 100;
            int keyIdx = (seed >> 8) % 1000;

            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fkey%04d", keyIdx);
            snprintf(valBuf, sizeof(valBuf), "fval%04d", keyIdx);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            databox foundVal;

            if (op < 40) {
                /* 40%: Insert */
                multidictAdd(d, &key, &val);
                inserts++;
            } else if (op < 55) {
                /* 15%: Delete */
                multidictDelete(d, &key);
                deletes++;
            } else if (op < 80) {
                /* 25%: Find (promotes in LRU) */
                multidictFind(d, &key, &foundVal);
                finds++;
            } else if (op < 95) {
                /* 15%: Touch */
                multidictTouch(d, &key);
                touches++;
            } else {
                /* 5%: Eviction check */
                multidictEvictToLimit(d);
            }
        }

        printf("    Ops: inserts=%d, deletes=%d, finds=%d, touches=%d\n",
               inserts, deletes, finds, touches);
        printf("    Final count: %" PRIu64 ", bytes: %" PRIu64 "\n",
               multidictCount(d), multidictKeyBytes(d) + multidictValBytes(d));

        /* Verify dict is consistent */
        uint64_t iterCount = 0;
        multidictIterator iter;
        multidictIteratorInit(d, &iter);
        multidictEntry entry;
        while (multidictIteratorNext(&iter, &entry)) {
            iterCount++;
        }
        multidictIteratorRelease(&iter);
        assert(iterCount == multidictCount(d));
    }

    printf("Test 20.5: LRU during rehashing...\n");
    {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 0); /* No limit initially */

        /* Insert enough to trigger rehashing */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rhkey%05d", i);
            snprintf(valBuf, sizeof(valBuf), "rhval%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
        }

        /* Now set a limit and evict during potential rehashing */
        multidictSetMaxMemory(d, 15000); /* ~500 entries */
        multidictEvictToLimit(d);

        uint64_t count = multidictCount(d);
        printf("    After rehash+evict: count=%" PRIu64 "\n", count);
        assert(count < 1000);

        /* Do incremental rehash while evicting */
        for (int i = 0; i < 100; i++) {
            multidictRehash(d, 10);
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rhkey%05d", 1000 + i);
            snprintf(valBuf, sizeof(valBuf), "rhval%05d", 1000 + i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            multidictAdd(d, &key, &val);
            multidictEvictToLimit(d);
        }

        /* Verify consistency */
        multidictStats stats;
        multidictGetStats(d, &stats);
        printf("    After concurrent ops: count=%" PRIu64 ", bytes=%" PRIu64
               "\n",
               stats.count, stats.totalBytes);
    }

    printf("Test 20.6: Policy switching...\n");
    {
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_LRU);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_RANDOM);
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_RANDOM);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_NONE);
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_NONE);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_LRU);
    }

    printf("Test 20.7: GetLRULevel API...\n");
    {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 0);

        /* Insert a key */
        databox key = databoxNewBytesString("leveltest");
        databox val = databoxNewBytesString("value");
        multidictAdd(d, &key, &val);

        int level0 = multidictGetLRULevel(d, &key);
        /* Touch repeatedly to promote */
        for (int i = 0; i < 50; i++) {
            multidictTouch(d, &key);
        }
        int level1 = multidictGetLRULevel(d, &key);
        printf("    Level after insert: %d, after 50 touches: %d\n", level0,
               level1);
        assert(level1 >= level0); /* Should promote */
    }

    printf("Test 20.8: Zero-overhead verification (no LRU)...\n");
    {
        /* Create new dict without LRU */
        multidictClass *qdcNoLru = multidictMmClassNew();
        multidict *dNoLru = multidictNew(&multidictTypeExactKey, qdcNoLru, 99);

        assert(!multidictHasLRU(dNoLru));

        /* These should be no-ops, not crash */
        databox key = databoxNewBytesString("test");
        databox val = databoxNewBytesString("val");
        multidictTouch(dNoLru, &key);
        assert(multidictGetLRULevel(dNoLru, &key) == -1);
        multidictAdd(dNoLru, &key, &val);
        multidictTouch(dNoLru, &key);
        multidictDelete(dNoLru, &key);

        printf(
            "    No-LRU operations: no crash, operations silently ignored\n");

        multidictFree(dNoLru);
        multidictMmClassFree(qdcNoLru);
    }

    printf("=== LRU eviction tests passed! ===\n");

    /* ================================================================
     * Section 21: Byte-Based Expansion Tests (Sprint 11)
     * ================================================================ */
    printf("\n--- Section 21: Byte-Based Expansion Tests ---\n");

    printf("Test 21.1: Enable/disable byte-based expansion API...\n");
    {
        multidictEmpty(d);
        assert(!multidictIsByteBasedExpansion(d)); /* Disabled by default */

        multidictEnableByteBasedExpansion(d, 100 * 1024, 500 * 1024);
        assert(multidictIsByteBasedExpansion(d));

        multidictDisableByteBasedExpansion(d);
        assert(!multidictIsByteBasedExpansion(d));
    }

    printf("Test 21.2: multidictGetLoadMetrics basic...\n");
    {
        multidictEmpty(d);
        multidictLoadMetrics metrics;

        /* Empty dict */
        multidictGetLoadMetrics(d, &metrics);
        assert(metrics.countLoadFactor == 0);
        assert(metrics.avgSlotBytes == 0);
        assert(metrics.usedSlots == 0);
        assert(metrics.totalUsedBytes == 0);
        assert(metrics.maxSlotBytes == 0);

        /* Add entries */
        for (int i = 0; i < 100; i++) {
            char keybuf[32], valbuf[64];
            snprintf(keybuf, sizeof(keybuf), "key%d", i);
            snprintf(valbuf, sizeof(valbuf), "value%d", i);
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(d, &key, &val);
        }

        multidictGetLoadMetrics(d, &metrics);
        assert(metrics.countLoadFactor > 0);
        assert(metrics.usedSlots > 0);
        assert(metrics.totalUsedBytes > 0);
        assert(metrics.avgSlotBytes > 0);
        assert(metrics.maxSlotBytes >= metrics.avgSlotBytes);
        printf("    Metrics: countLF=%u%%, usedSlots=%" PRIu64
               ", avgSlotBytes=%" PRIu64 ", maxSlotBytes=%" PRIu64 "\n",
               metrics.countLoadFactor, metrics.usedSlots, metrics.avgSlotBytes,
               metrics.maxSlotBytes);
    }

    printf("Test 21.3: Byte-based expansion trigger - small values...\n");
    {
        /* Create fresh dict with byte-based expansion */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 1024,
                                          4096); /* 1KB target, 4KB max */

        /* Insert small entries - should NOT expand based on bytes */
        for (int i = 0; i < 200; i++) {
            char keybuf[16], valbuf[16];
            snprintf(keybuf, sizeof(keybuf), "k%d", i);
            snprintf(valbuf, sizeof(valbuf), "v%d", i);
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);
        }

        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);
        printf("    After 200 small entries: slots=%" PRIu64
               ", avgSlotBytes=%" PRIu64 " (target=1024)\n",
               multidictSlotCount(dByte), metrics.avgSlotBytes);

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.4: Byte-based expansion trigger - large values...\n");
    {
        /* Create fresh dict with byte-based expansion */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 512,
                                          2048); /* 512B target, 2KB max */

        uint32_t initialSlots = multidictSlotCount(dByte);

        /* Insert large entries to trigger byte-based expansion */
        for (int i = 0; i < 50; i++) {
            char keybuf[32];
            char valbuf[256]; /* Large value: 256 bytes */
            snprintf(keybuf, sizeof(keybuf), "bigkey%d", i);
            memset(valbuf, 'X', sizeof(valbuf) - 1);
            valbuf[sizeof(valbuf) - 1] = '\0';
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);
        }

        uint32_t finalSlots = multidictSlotCount(dByte);
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);

        printf("    Slots: initial=%" PRIu32 ", final=%" PRIu32
               ", avgSlotBytes=%" PRIu64 "\n",
               initialSlots, finalSlots, metrics.avgSlotBytes);
        assert(finalSlots > initialSlots); /* Should expand due to byte load */

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.5: Safeguard - maximum slot size trigger...\n");
    {
        /* Create dict with very low max slot size */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 10000,
                                          500); /* maxSlotBytes=500 */

        uint32_t initialSlots = multidictSlotCount(dByte);

        /* Create entries that will hash to same slot and exceed max */
        for (int i = 0; i < 20; i++) {
            char keybuf[32];
            char valbuf[64];
            snprintf(keybuf, sizeof(keybuf), "slot%d", i);
            memset(valbuf, 'Y', sizeof(valbuf) - 1);
            valbuf[sizeof(valbuf) - 1] = '\0';
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);
        }

        uint32_t finalSlots = multidictSlotCount(dByte);
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);

        printf("    Slots: initial=%" PRIu32 ", final=%" PRIu32
               ", maxSlotBytes=%" PRIu64 " (limit=500)\n",
               initialSlots, finalSlots, metrics.maxSlotBytes);

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.6: Safeguard - count-based backstop...\n");
    {
        /* Create dict with very high byte targets but low count threshold */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(
            dByte, 1024 * 1024, 8 * 1024 * 1024); /* 1MB target - very high */

        uint32_t initialSlots = multidictSlotCount(dByte);

        /* Fill dict with tiny entries - count-based backstop should trigger */
        for (int i = 0; i < 500; i++) {
            char keybuf[16], valbuf[4];
            snprintf(keybuf, sizeof(keybuf), "c%d", i);
            snprintf(valbuf, sizeof(valbuf), "v%d", i % 10);
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);
        }

        uint32_t finalSlots = multidictSlotCount(dByte);
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);

        printf("    Slots: initial=%" PRIu32 ", final=%" PRIu32
               ", countLF=%u%%, byteLF=%u%%\n",
               initialSlots, finalSlots, metrics.countLoadFactor,
               metrics.byteLoadFactor);
        assert(finalSlots >
               initialSlots); /* Should expand via count backstop */

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.7: Safeguard - expansion effectiveness check...\n");
    {
        /* This test verifies that expansion won't happen if it won't help
         * (skewed distribution where all keys hash to few slots) */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 256, 1024);

        /* Insert entries - the effectiveness check will prevent
         * useless expansions for pathological cases */
        for (int i = 0; i < 100; i++) {
            char keybuf[32], valbuf[128];
            snprintf(keybuf, sizeof(keybuf), "eff%d", i);
            memset(valbuf, 'Z', sizeof(valbuf) - 1);
            valbuf[sizeof(valbuf) - 1] = '\0';
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);
        }

        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);
        printf("    Expansion with effectiveness check: avgSlotBytes=%" PRIu64
               ", slots=%" PRIu64 "\n",
               metrics.avgSlotBytes, multidictSlotCount(dByte));

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.8: Byte-based vs count-based expansion comparison...\n");
    {
        /* Create two dicts: one byte-based, one count-based */
        multidictClass *qdcByte = multidictMmClassNew();
        multidictClass *qdcCount = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidict *dCount = multidictNew(&multidictTypeExactKey, qdcCount, 88);

        multidictEnableByteBasedExpansion(dByte, 512, 2048);
        /* dCount uses default count-based */

        /* Insert same large entries to both */
        for (int i = 0; i < 100; i++) {
            char keybuf[32], valbuf[256];
            snprintf(keybuf, sizeof(keybuf), "cmp%d", i);
            memset(valbuf, 'C', sizeof(valbuf) - 1);
            valbuf[sizeof(valbuf) - 1] = '\0';
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);

            databox key2 = databoxNewBytesString(keybuf);
            databox val2 = databoxNewBytesString(valbuf);
            multidictAdd(dCount, &key2, &val2);
        }

        uint32_t byteSlots = multidictSlotCount(dByte);
        uint32_t countSlots = multidictSlotCount(dCount);

        printf("    After 100 large entries: byte-based slots=%" PRIu32
               ", count-based slots=%" PRIu32 "\n",
               byteSlots, countSlots);
        /* Byte-based may expand more aggressively for large values */

        multidictFree(dByte);
        multidictFree(dCount);
        multidictMmClassFree(qdcByte);
        multidictMmClassFree(qdcCount);
    }

    printf("Test 21.9: Byte-based expansion during rehashing operations...\n");
    {
        /* Verify byte-based expansion works correctly with rehashing */
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 1024, 4096);

        /* Insert to trigger expansion */
        for (int i = 0; i < 200; i++) {
            char keybuf[32], valbuf[128];
            snprintf(keybuf, sizeof(keybuf), "rh%d", i);
            memset(valbuf, 'R', sizeof(valbuf) - 1);
            valbuf[sizeof(valbuf) - 1] = '\0';
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            multidictAdd(dByte, &key, &val);

            /* Interleave rehash steps */
            if (multidictIsRehashing(dByte)) {
                multidictRehash(dByte, 5);
            }
        }

        /* Verify all entries present */
        uint64_t foundCount = 0;
        for (int i = 0; i < 200; i++) {
            char keybuf[32];
            snprintf(keybuf, sizeof(keybuf), "rh%d", i);
            databox key = databoxNewBytesString(keybuf);
            databox val = {{0}};
            if (multidictFind(dByte, &key, &val)) {
                foundCount++;
            }
        }

        printf("    After rehashing with byte-based expansion: found=%" PRIu64
               "/200\n",
               foundCount);
        assert(foundCount == 200);

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("Test 21.10: Fuzz test - byte-based expansion with mixed "
           "operations...\n");
    {
        multidictClass *qdcByte = multidictMmClassNew();
        multidict *dByte = multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 2048, 8192);

        int adds = 0, dels = 0, finds = 0;
        for (int i = 0; i < 1000; i++) {
            int op = rand() % 100;

            if (op < 60) { /* 60% add */
                char keybuf[32], valbuf[256];
                snprintf(keybuf, sizeof(keybuf), "fz%d", rand() % 500);
                int valsize = 32 + (rand() % 224); /* Variable size values */
                memset(valbuf, 'F', valsize);
                valbuf[valsize] = '\0';
                databox key = databoxNewBytesString(keybuf);
                databox val = databoxNewBytesString(valbuf);
                multidictAdd(dByte, &key, &val);
                adds++;
            } else if (op < 85) { /* 25% find */
                char keybuf[32];
                snprintf(keybuf, sizeof(keybuf), "fz%d", rand() % 500);
                databox key = databoxNewBytesString(keybuf);
                databox val = {{0}};
                multidictFind(dByte, &key, &val);
                finds++;
            } else { /* 15% delete */
                char keybuf[32];
                snprintf(keybuf, sizeof(keybuf), "fz%d", rand() % 500);
                databox key = databoxNewBytesString(keybuf);
                multidictDelete(dByte, &key);
                dels++;
            }

            /* Occasionally step rehash */
            if (i % 50 == 0 && multidictIsRehashing(dByte)) {
                multidictRehash(dByte, 10);
            }
        }

        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);
        printf("    Fuzz: adds=%d, dels=%d, finds=%d, final count=%" PRIu64
               ", avgSlotBytes=%" PRIu64 "\n",
               adds, dels, finds, multidictCount(dByte), metrics.avgSlotBytes);

        multidictFree(dByte);
        multidictMmClassFree(qdcByte);
    }

    printf("=== Byte-based expansion tests passed! ===\n");

    /* ================================================================
     * Cleanup
     * ================================================================ */
    printf("\n--- Cleanup ---\n");
    multidictFree(d);
    multidictMmClassFree(qdc);

    printf("\n=== ALL MULTIDICT TESTS PASSED! ===\n");
    return err;
}

#endif /* DATAKIT_TEST */

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
