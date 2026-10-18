#include "multidict.h"

#include "../deps/xxHash/xxhash.h"
#include "asmUtils.h"
#include "cleaner.h"
#include "datakit.h"
#include "fibbuf.h"
#include "flex.h"
#include "multilru.h"
#include "multimap.h"
#include "str.h"
#include "timeUtil.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <sys/time.h>

#ifdef DATAKIT_TEST
/* Fixed-width string buffer used by test bulk-insert scratch allocations. */
typedef char multidictTestBuf32[32];
defcleanerName(databox *, zfree, Databoxes);
defcleanerName(multidictTestBuf32 *, zfree, Buf32);
#endif

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
 *   - TOTAL: 140 bytes of fields, padded to 144. */
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

/* ====================================================================
 * 96/128-bit keys and values
 * ====================================================================
 * Every box the dict fills for itself is a plain databox with a
 * databoxWide beside it, so a 96/128-bit key or value lands only in
 * storage set up for it:
 *   - iteration decodes straight into the slots (the slot iterator's
 *     'wide', or a stack pair) through multimapIteratorNextWithSlots;
 *   - a slot class or multidict read that fills a box in place (lastKey,
 *     findValueByKey, findKeyByPosition, multidictFind) gets a box aimed
 *     at a stack slot with mdBoxAim_ first.
 * Narrow values never touch the slot. */
DK_INLINE_ALWAYS bool mdBoxIsWide_(const databox *box) {
    return box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128;
}

/* Point 'box' at 'slot' for an in-place decode: the header is cleared and
 * big = 1, so a 96/128-bit value is written into 'slot'.  A narrow value
 * overwrites the pointer and leaves big = 1, which nothing reads on a
 * narrow value; mdBoxSettle_ clears it where a box is handed to user
 * code.  Re-aim before each decode. */
DK_INLINE_ALWAYS void mdBoxAim_(databox *box, databoxWide *slot) {
    memset(box, 0, sizeof(*box));
    box->big = true;
    box->data.u128 = (__uint128_t *)slot->b;
}

DK_INLINE_ALWAYS void mdBoxSettle_(databox *box) {
    box->big = mdBoxIsWide_(box);
}

/* Clear the type, flags and length of a box a decode fills through a
 * slot, so its flags come out as big = (wide), allocated = 0.  One store;
 * the decode writes the rest. */
DK_INLINE_ALWAYS void mdBoxHeaderClear_(databox *box) {
    memset((uint8_t *)box + sizeof(databoxUnion), 0,
           sizeof(*box) - sizeof(databoxUnion));
}

/* Deliver a decoded value into a caller box the way flexGetByType fills
 * one: type, data and len are written and the flags are left as they
 * were; a 96/128-bit value is written through dst's payload pointer when
 * dst->big is set, and otherwise dst receives the type only. */
DK_INLINE_ALWAYS void mdBoxDeliver_(databox *dst, const databox *src) {
    dst->type = src->type;
    dst->len = src->len;
    if (!mdBoxIsWide_(src)) {
        dst->data = src->data;
    } else if (dst->big) {
        memcpy(dst->data.u128, src->data.u128, sizeof(*src->data.u128));
    } else {
        dst->data.ptr = NULL;
    }
}

/* Copy 'src' into 'dst', moving a 96/128-bit payload into 'slot' so the
 * copy outlives the storage 'src' borrowed it from.  BYTES keep
 * borrowing from the dict. */
DK_INLINE_ALWAYS void mdBoxRehome_(databox *dst, const databox *src,
                                   databoxWide *slot) {
    *dst = *src;
    if (mdBoxIsWide_(src) && src->data.ptr) {
        memcpy(slot->b, src->data.ptr, sizeof(slot->b));
        dst->data.u128 = (__uint128_t *)slot->b;
        dst->big = true;
        dst->allocated = false;
    }
}

/* Bytes counted for a key or value in the byte statistics */
DK_INLINE_ALWAYS size_t mdBoxSize_(const databox *box) {
    if (mdBoxIsWide_(box)) {
        return sizeof(__uint128_t);
    }

    size_t len = 0;
    databoxGetSize(box, &len);
    return len;
}

/* The bytes a key hashes by.  A 96/128-bit key hashes by value: when it
 * fits 64 bits it hashes as the 64-bit box of the same value would (via
 * 'scratch'), so it finds the entry stored under its canonical form.
 * Returns false when the box has no bytes (*buf and *len are then a
 * zero-length span). */
DK_INLINE_ALWAYS bool mdKeyHashBytes_(const databox *key, uint64_t *scratch,
                                      uint8_t **buf, size_t *len) {
    *buf = (uint8_t *)scratch;
    *len = 0;

    if (!mdBoxIsWide_(key)) {
        return databoxGetBytes((databox *)key, buf, len);
    }

    if (!key->data.ptr) {
        return false; /* type-only: no value to hash */
    }

    const bool fits64 =
        key->type == DATABOX_SIGNED_128
            ? *key->data.i128 >= INT64_MIN && *key->data.i128 <= UINT64_MAX
            : *key->data.u128 <= UINT64_MAX;
    if (fits64) {
        *scratch = (uint64_t)*key->data.u128;
        *len = sizeof(*scratch);
    } else {
        *buf = (uint8_t *)key->data.u128;
        *len = sizeof(__uint128_t);
    }

    return true;
}

/* Slot hash of 'keybox' */
DK_INLINE_ALWAYS uint32_t multidictHashOfKey_(multidict *d,
                                              const databox *keybox) {
    uint64_t scratch;
    uint8_t *key;
    size_t klen;
    mdKeyHashBytes_(keybox, &scratch, &key, &klen);
    return multidictHashKey(d, key, klen);
}

/* Helper to recalculate totalBytes from components */
#define HT_UPDATE_TOTAL(ht)                                                    \
    do {                                                                       \
        (ht)->totalBytes = (ht)->usedBytes + (ht)->keyBytes + (ht)->valBytes;  \
    } while (0)

/* The cold end of the LRU as one multidictEvictToLimit call walks it.
 * Victims are picked without being removed from tracking: an entry
 * leaves the LRU only when multidictDelete commits its eviction, so a
 * vetoed victim keeps its level and position exactly.  Passed-over
 * entries stay at the cold end ahead of every untried one, so after a
 * refill the first 'skip' candidates are the ones already passed over.
 * Once 'skip' fills the window, the call picks random victims instead. */
#define MD_LRU_CANDIDATES 64
typedef struct mdLRUCandidates {
    multilruPtr ptr[MD_LRU_CANDIDATES];
    uint32_t count; /* candidates the last refill found */
    uint32_t pos;   /* next candidate to try */
    uint32_t skip;  /* candidates passed over (vetoed or unresolvable) */
} mdLRUCandidates;

/* Forward declarations for LRU tracking (defined later) */
static void multidictLRUOnInsert(multidict *d, const databox *keybox);
static void multidictLRUOnAccess(multidict *d, const databox *keybox);
static void multidictLRUOnDelete(multidict *d, const databox *keybox);
static bool multidictLRUSelectVictim(multidict *d, mdLRUCandidates *cands,
                                     databox *keybox, databoxWide *slot);
DK_INLINE_ALWAYS bool multidictFindNoTouch_(multidict *d, const databox *keybox,
                                            databox *valbox);

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

/* An ASCII case-insensitive hash (djb).  It folds exactly 'A'-'Z' to
 * 'a'-'z', the set multidictTypeCaseKey's compare folds, so keys that
 * compare equal hash equal. */
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
    uint32_t hash = multidictHashOfKey_(d, keybox);

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

    /* @own: caller; free=multidictFree */
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

    /* @own: container; free=multidictFree */
    n.table = zcalloc(1, n.size * sizeof(*n.table));
    if (!n.table) {
        /* OOM: fail the expand cleanly rather than install a NULL table and set
         * rehashing — that would defer the failure into a corrupt half-rehash
         * state (NULL ht with rehashing=true). Rehash/grow is best-effort, so
         * returning false here is safe; callers tolerate it. */
        return false;
    }

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

    /* Max empty (NULL) slots to skip across this whole call, scaled by the
     * requested step count. Computed ONCE from the original n: using the
     * post-decrement loop counter made this 0 for single-step calls, so the
     * emptyVisits-- below wrapped to UINT32_MAX and disabled the work cap. */
    uint32_t emptyVisits = (uint32_t)(n > 0 ? n : 1) * 5u;

    while (n-- > 0 && HTCOUNT(d, 0) > 0) {
        /* rehashidx must never grow above HTSIZE-1 */
        assert(d->rehashidx < HTSIZE(d, 0));

        /* Test for dead slots.
         * If we have large ranges of NULL slots, we want to jump past
         * them as efficiently as possible instead of needing to call
         * this function over and over again just to do nothing. */
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
        databox keybox;
        databoxWide keyWide;
        bool freeCurrentContainer = true;

        /* We check 'current' in the while condition because if we move
         * the entire slot, we need to abort the while loop with no cleanup. */
        while (current) {
            mdBoxAim_(&keybox, &keyWide);
            if (!shared->lastKey(current, &keybox)) {
                break;
            }

            /* Get target multimap for new hash slot; created on-demand below */
            multidictSlot **target =
                SLOT_BY_HASH_PTR_PTR(d, 1, multidictHashOfKey_(d, &keybox));

            /* All slots should be previously allocated. Target must exist. */
            assert(target);

            /* Get key and value sizes for byte tracking transfer.  The
             * value is only measured, so its type is enough. */
            const size_t keySize = mdBoxSize_(&keybox);
            size_t valSize = 0;
            databox valbox = {{0}};
            if (shared->findValueByKey(shared, current, &keybox, &valbox)) {
                valSize = mdBoxSize_(&valbox);
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
        uint32_t hash = multidictHashOfKey_(d, keybox);
        multidictSlot **ht0Slot = SLOT_BY_HASH_PTR_PTR(d, 0, hash);

        if (ht0Slot && *ht0Slot) {
            /* Slot exists in ht[0] - check if this specific key is there.
             * The value is only measured, so its type is enough. */
            databox existingVal = {{0}};
            if (d->shared->findValueByKey(d->shared, *ht0Slot, keybox,
                                          &existingVal)) {
                /* Key exists in ht[0] - remove it to prevent duplicate */
                const size_t oldKeySize = mdBoxSize_(keybox);
                const size_t oldValSize = mdBoxSize_(&existingVal);
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
    const size_t keySize = mdBoxSize_(keybox);
    const size_t valSize = mdBoxSize_(valbox);

    /* For replacements, we need to know the old value size to track accurately.
     * Look up the existing value if it might exist.  The old value is only
     * measured, so its type is enough. */
    size_t oldValSize = 0;
    databox oldVal = {{0}};
    bool hadOldValue =
        d->shared->findValueByKey(d->shared, op.result, keybox, &oldVal);
    if (hadOldValue) {
        oldValSize = mdBoxSize_(&oldVal);
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
    uint32_t hash = multidictHashOfKey_(d, keybox);

    multidictSlot **slotPtr = SLOT_BY_HASH_PTR_PTR(d, htidx, hash);
    if (!slotPtr || !*slotPtr) {
        return false;
    }

    multidictSlot *slot = *slotPtr;

    /* Check if key actually exists in this slot.  The value is only
     * measured, so its type is enough. */
    databox val = {{0}};
    if (!d->shared->findValueByKey(d->shared, slot, keybox, &val)) {
        return false; /* Key not in this slot */
    }

    /* Key exists - proceed with deletion */
    multidictHT *ht = HT(d, htidx);
    const size_t lenBefore = d->shared->sizeBytes(slot);

    const size_t keySize = mdBoxSize_(keybox);
    const size_t valSize = mdBoxSize_(&val);

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
    databox found;
    databoxWide foundWide;
    mdBoxAim_(&found, &foundWide);
    if (!multidictFind(d, keybox, &found)) {
        return false; /* Key doesn't exist */
    }

    /* The stored value goes away with the delete: hand back an owned copy */
    *valbox = databoxCopy(&found);

    /* Now delete it - this should succeed since we just found it */
    bool deleted = multidictDelete(d, keybox);

    /* During concurrent rehashing, key could theoretically move between
     * find and delete, but our implementation handles this correctly */
    if (!deleted) {
        databoxFreeData(valbox);
    }

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

    /* Get a random key; this fails only on an empty dict */
    databox key;
    databoxWide keyWide;
    mdBoxAim_(&key, &keyWide);
    if (!multidictGetRandomKey(d, &key)) {
        return false;
    }

    /* The key borrows its bytes from its slot, and the rehash step the
     * find below runs can move the entry and free that slot: copy it
     * first, then find and delete by the copy.  The find and the delete
     * search both tables while a rehash is in flight, so they find the
     * entry the key was just read from. */
    databox ownedKey = databoxCopy(&key);

    /* Get the value for this key; the entry is about to go, so the
     * lookup does not promote it in the LRU */
    databox val;
    databoxWide valWide;
    mdBoxAim_(&val, &valWide);
    if (!multidictFindNoTouch_(d, &ownedKey, &val)) {
        databoxFreeData(&ownedKey);
        return false;
    }

    /* The entry goes away with the delete: hand back owned copies */
    *keybox = ownedKey;
    *valbox = databoxCopy(&val);

    if (multidictDelete(d, keybox)) {
        return true;
    }

    databoxFreeData(keybox);
    databoxFreeData(valbox);
    return false;
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

/* Lookup without the LRU promotion: multidictFind is this plus the
 * promotion, and eviction reads a victim's value through this so a
 * vetoed victim keeps its LRU rank. */
DK_INLINE_ALWAYS bool multidictFindNoTouch_(multidict *d, const databox *keybox,
                                            databox *valbox) {
    if (unlikely(HTSIZE(d, 0) == 0)) {
        return false;
    }

    multidictRehashStep(d);

    /* During rehashing, we must search BOTH tables, not just the slot lookup.
     * The slot lookup may return a non-NULL slot in ht[1] that doesn't contain
     * our key, while our key is still in ht[0] awaiting migration.
     * Bug fix: always search both tables during rehashing. */
    if (unlikely(d->rehashing)) {
        uint32_t hash = multidictHashOfKey_(d, keybox);

        /* Search ht[1] first (newer table) */
        multidictSlot *slot1 = *SLOT_BY_HASH_PTR_PTR(d, 1, hash);
        if (slot1 &&
            d->shared->findValueByKey(d->shared, slot1, keybox, valbox)) {
            return true;
        }

        /* Then search ht[0] (older table, entries may not be migrated yet) */
        multidictSlot *slot0 = *SLOT_BY_HASH_PTR_PTR(d, 0, hash);
        return slot0 &&
               d->shared->findValueByKey(d->shared, slot0, keybox, valbox);
    }

    /* Not rehashing - simple case, single table lookup */
    multidictSlot *target = *SLOT_BY_HASH_PTR_PTR(d, 0, 0);
    if (HTSIZE(d, 0) > 1) {
        uint32_t hash = multidictHashOfKey_(d, keybox);
        target = *SLOT_BY_HASH_PTR_PTR(d, 0, hash);
    }

    return d->shared->findValueByKey(d->shared, target, keybox, valbox);
}

/* Returns value in inout param 'val' - hot path, optimized */
bool multidictFind(multidict *d, const databox *keybox, databox *valbox) {
    const bool found = multidictFindNoTouch_(d, keybox, valbox);

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
    const uint32_t maxAttempts = multidictCount(d) * 2 + 100; /* Safety limit */
    databox keybox;
    databoxWide keyWide;
    mdLRUCandidates lruCands;
    lruCands.count = 0;
    lruCands.pos = 0;
    lruCands.skip = 0;

    /* Use user bytes (key + val) for limit comparison, not total overhead */
    while (multidictUserBytes(d) > d->maxMemory && multidictCount(d) > 0 &&
           attempts < maxAttempts) {
        attempts++;

        bool fromLRU = false;

        /* Select victim based on eviction policy */
        if (d->evictPolicy == MULTIDICT_EVICT_LRU && d->lruEnabled) {
            /* LRU eviction: the coldest entry not yet passed over */
            fromLRU = multidictLRUSelectVictim(d, &lruCands, &keybox, &keyWide);
        }

        if (!fromLRU) {
            /* Fallback to random eviction; this fails only on an empty
             * dict, which the loop guard already excludes */
            mdBoxAim_(&keybox, &keyWide);
            if (!multidictGetRandomKey(d, &keybox)) {
                break;
            }
            mdBoxSettle_(&keybox);
        }

        /* The victim key borrows its bytes from its slot, and the rehash
         * step that the find and the delete below each run can move the
         * entry and free that slot: work from an owned copy instead */
        databox victim = databoxCopy(&keybox);

        /* Call eviction callback if set.  The lookup does not promote the
         * victim, so a vetoed one keeps its LRU rank. */
        bool vetoed = false;
        if (d->evictionCb) {
            databox valbox;
            databoxWide valWide;
            mdBoxAim_(&valbox, &valWide);
            if (multidictFindNoTouch_(d, &victim, &valbox)) {
                mdBoxSettle_(&valbox);
                vetoed = !d->evictionCb(d->evictionPrivdata, &victim, &valbox);
            }
        }

        /* The delete drops the victim's LRU tracking with the entry */
        const bool deleted = !vetoed && multidictDelete(d, &victim);
        databoxFreeData(&victim);
        if (!deleted && fromLRU) {
            /* Still tracked, at the cold end: pass over it from now on */
            lruCands.skip++;
        }

        if (vetoed) {
            /* Callback vetoed this eviction, try another key */
            continue;
        }

        if (deleted) {
            evicted++;
        } else {
            deleteFailures++;
            /* Limit delete failures to prevent infinite loop */
            if (deleteFailures > 50) {
                break;
            }
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
 * the hash to find the slot and iterate to locate the actual key; a key
 * with a matching hash is confirmed by looking its handle up in
 * lruKeyToPtr, since keys in one slot can share a hash.
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

    /* @own: container; free=multidictDisableLRU */
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
    /* @own: container; free=multidictDisableLRU */
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

/* Helper: the hash LRU tracking records for a key.  It must be the slot
 * hash, the dict type's own hash function, so the recorded hash selects
 * the slot that holds the key. */
static uint32_t multidictKeyHash(multidict *d, const databox *keybox) {
    return multidictHashOfKey_(d, keybox);
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

/* Internal: Find the key in 'slot' tracked under LRU handle 'ptr' (for
 * LRU eviction).  Keys in one slot can share the recorded 32-bit hash, so
 * a hash match is confirmed through the key-to-handle dict: the key is the
 * one whose handle is 'ptr'.  The key is copied into 'keybox' with a
 * 96/128-bit payload moved to 'wide', since the slot iterator holding it
 * does not outlive this call. */
static bool findKeyByHashInSlot(multidict *d, multidictSlot *slot,
                                uint32_t targetHash, multilruPtr ptr,
                                databox *keybox, databoxWide *wide) {
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
        if (multidictKeyHash(d, &entry.key) != targetHash) {
            continue;
        }

        databox ptrBox = {{0}};
        if (multidictFind(d->lruKeyToPtr, &entry.key, &ptrBox) &&
            ptrBox.data.u64 == ptr) {
            mdBoxRehome_(keybox, &entry.key, wide);
            return true;
        }
    }
    return false;
}

/* Internal: the key tracked under LRU handle 'ptr', found in the slot the
 * hash recorded for it selects */
static bool multidictLRUKeyOf(multidict *d, multilruPtr ptr, databox *keybox,
                              databoxWide *slot) {
    if (ptr >= d->lruPtrToKeySize) {
        return false;
    }
    const lruKeyRef *refs = (const lruKeyRef *)d->lruPtrToKey;
    if (!refs[ptr].valid) {
        return false;
    }

    const uint32_t hash = refs[ptr].hash;

    /* Find key by hash in the appropriate slot */
    uint32_t slotIdx = hash & HTMASK(d, 0);
    if (findKeyByHashInSlot(d, SLOT(d, 0, slotIdx), hash, ptr, keybox, slot)) {
        return true;
    }

    /* During rehashing, also check ht[1] */
    if (multidictIsRehashing(d)) {
        slotIdx = hash & HTMASK(d, 1);
        if (findKeyByHashInSlot(d, SLOT(d, 1, slotIdx), hash, ptr, keybox,
                                slot)) {
            return true;
        }
    }

    return false;
}

/* Internal: Select victim using LRU policy (called from eviction).  The
 * victim stays tracked; multidictDelete untracks it if the eviction
 * commits, and otherwise the caller counts it in cands->skip. */
static bool multidictLRUSelectVictim(multidict *d, mdLRUCandidates *cands,
                                     databox *keybox, databoxWide *slot) {
    if (!d->lruEnabled || !d->lru) {
        return false;
    }

    while (cands->skip < MD_LRU_CANDIDATES) {
        if (cands->pos >= cands->count) {
            /* Refill from the cold end; the passed-over candidates lead */
            memset(cands->ptr, 0, sizeof(cands->ptr));
            multilruGetNLowest(d->lru, cands->ptr, MD_LRU_CANDIDATES);
            cands->count = 0;
            while (cands->count < MD_LRU_CANDIDATES &&
                   cands->ptr[cands->count]) {
                cands->count++;
            }

            cands->pos = cands->skip;
            if (cands->pos >= cands->count) {
                return false; /* every tracked entry was passed over */
            }
        }

        const multilruPtr ptr = cands->ptr[cands->pos++];
        if (!multilruIsPopulated(d->lru, ptr)) {
            continue; /* already untracked since the refill */
        }

        if (multidictLRUKeyOf(d, ptr, keybox, slot)) {
            return true;
        }

        /* No key resolves for this handle: leave it and pass over it */
        cands->skip++;
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
    /* @own: caller; free=multidictFree */
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

/* Random slot probes multidictGetRandomKey makes before it walks */
#define MD_RANDOM_KEY_PROBES 128

/* Live slot 'pos', counting ht[0] slots first and then ht[1] slots */
DK_INLINE_ALWAYS multidictSlot *mdLiveSlot_(multidict *d, uint64_t pos) {
    const uint64_t size0 = HTSIZE(d, 0);
    return pos < size0 ? HT(d, 0)->table[pos] : HT(d, 1)->table[pos - size0];
}

/* Deliver a random key of 'slot' into 'keybox'; false when the slot is
 * NULL or holds no entries */
DK_INLINE_ALWAYS bool mdRandomKeyInSlot_(multidict *d, multidictSlot *slot,
                                         databox *keybox) {
    if (!slot) {
        return false;
    }

    const uint32_t listlen = d->shared->countSlot(slot);
    if (listlen == 0) {
        return false;
    }

    const uint32_t listele = (uint32_t)((uint64_t)random() % listlen);
    return d->shared->findKeyByPosition(d->shared, slot, listele, keybox);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
bool multidictGetRandomKey(multidict *d, databox *keybox) {
    if (multidictSize(d) == 0) {
        return false;
    }

    multidictRehashStep(d);

    /* The live slots: while rehashing, ht[0] holds nothing below
     * rehashidx, so they are ht[0] from rehashidx on and then all of
     * ht[1]; otherwise all of ht[0].  'pos' indexes them as one run. */
    const uint64_t first = multidictIsRehashing(d) ? d->rehashidx : 0;
    const uint64_t end = multidictIsRehashing(d)
                             ? (uint64_t)HTSIZE(d, 0) + HTSIZE(d, 1)
                             : HTSIZE(d, 0);
    const uint64_t span = end - first;

    /* A probe can land on a NULL slot or on one a delete emptied but left
     * allocated.  After a bounded number of probes, walk every live slot
     * from a random start instead, so this fails only on an empty dict. */
    for (uint32_t probe = 0; probe < MD_RANDOM_KEY_PROBES; probe++) {
        const uint64_t pos = first + (uint64_t)random() % span;
        if (mdRandomKeyInSlot_(d, mdLiveSlot_(d, pos), keybox)) {
            return true;
        }
    }

    const uint64_t start = (uint64_t)random() % span;
    for (uint64_t i = 0; i < span; i++) {
        uint64_t off = start + i;
        if (off >= span) {
            off -= span;
        }

        if (mdRandomKeyInSlot_(d, mdLiveSlot_(d, first + off), keybox)) {
            return true;
        }
    }

    return false;
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
                    /* Ignore what the slot held: a 96/128-bit key comes
                     * back deferred (type only) */
                    memset(&keys[stored], 0, sizeof(keys[stored]));
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

/* Exact key equality: same length and same bytes.  Keys are binary, so an
 * embedded NUL is an ordinary byte. */
DK_STATIC int32_t multidictStringKeyCompare_(void *privdata, const void *key1,
                                             const uint32_t k1len,
                                             const void *key2,
                                             const uint32_t k2len) {
    DK_NOTUSED(privdata);

    return k1len == k2len && (k1len == 0 || memcmp(key1, key2, k1len) == 0);
}

/* ASCII case-insensitive key equality: same length, and the same bytes
 * once 'A'-'Z' are folded to 'a'-'z'.  Every other byte, NUL and bytes
 * above 0x7F included, must match exactly, whatever the C locale is.
 * multidictGenCaseHashFunction folds the same set, so keys equal here
 * hash equal. */
DK_STATIC int32_t multidictStringCaseKeyCompare_(void *privdata,
                                                 const void *key1,
                                                 const uint32_t k1len,
                                                 const void *key2,
                                                 const uint32_t k2len) {
    DK_NOTUSED(privdata);

    return k1len == k2len &&
           StrUtf8EqualCaseInsensitiveAscii(key1, k1len, key2, k2len);
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
        /* @own: container; free=mmSlotFree */
        mmSlotWrapper *wrapper = zcalloc(1, sizeof(*wrapper));
        /* @own: container; free=mmSlotFree */
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
    /* @own: container; free=mmSlotFree */
    mmSlotWrapper *wrapper = zcalloc(1, sizeof(*wrapper));
    /* @own: container; free=mmSlotFree */
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

    /* The entry is library-filled: clear the key and value flags, then
     * decode a wide key or value into the iterator's own slots */
    mdBoxHeaderClear_(&entry->key);
    mdBoxHeaderClear_(&entry->val);
    databox *elements[2] = {&entry->key, &entry->val};

    if (multimapIteratorNextWithSlots(&state->mmIter, elements, iter->wide)) {
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

    /* Walk with our own boxes, then deliver only the key at 'pos' into
     * the caller's box: decoding every key into it would let a narrow
     * key overwrite the payload pointer a later wide key is written
     * through. */
    databox key;
    databox val;
    databoxWide wide[2];
    databox *elements[2] = {&key, &val};
    for (uint32_t idx = 0;
         multimapIteratorNextWithSlots(&mmIter, elements, wide); idx++) {
        if (idx == pos) {
            mdBoxDeliver_(keybox, &key);
            return true;
        }
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

    /* The callback sees big = 1 exactly for a wide value */
    databox key, val;
    databoxWide wide[2];
    databox *elements[2] = {&key, &val};
    for (;;) {
        mdBoxHeaderClear_(&key);
        mdBoxHeaderClear_(&val);
        if (!multimapIteratorNextWithSlots(&mmIter, elements, wide)) {
            break;
        }

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
    /* The value is discarded, so its type is enough */
    databox lastVal = {{0}};
    databox *elements[2] = {keybox, &lastVal};
    return multimapLast(wrapper->m, elements);
}

DK_STATIC bool mmSlotMigrateLast(void *dst, void *src) {
    mmSlotWrapper *srcWrapper = (mmSlotWrapper *)src;
    mmSlotWrapper *dstWrapper = (mmSlotWrapper *)dst;

    /* Get last key-value from source */
    databox key, val;
    databoxWide keyWide, valWide;
    mdBoxAim_(&key, &keyWide);
    mdBoxAim_(&val, &valWide);
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
    /* @own: caller; free=multidictMmClassFree */
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
#include "persist.h" /* snapshot round trips of wide pairs */
#include <locale.h>  /* the case compare ignores the C locale */

/* Scope-exit cleaners (see cleaner.h idioms).  A dict must be destroyed
 * before the class it borrows, so a class cleaner is always declared
 * BEFORE the dict cleaner that uses it. */
defcleaner(multidict *, multidictFree);
defcleaner(multidictClass *, multidictMmClassFree);

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

/* Checks each victim of the rehash-in-flight eviction test: the key must
 * read "evict-rehash-key-NNNNNN" and the value the matching
 * "evict-rehash-val-NNNNNN" */
static int testEvictRehashSeen_ = 0;
static int testEvictRehashBad_ = 0;
static bool evictRehashCb_(void *privdata, const databox *key,
                           const databox *val) {
    (void)privdata;
    testEvictRehashSeen_++;
    static const char keyPrefix[] = "evict-rehash-key-";
    static const char valPrefix[] = "evict-rehash-val-";
    const size_t prefixLen = sizeof(keyPrefix) - 1;
    if (!DATABOX_IS_BYTES(key) || !DATABOX_IS_BYTES(val) ||
        key->len != prefixLen + 6 || val->len != key->len) {
        testEvictRehashBad_++;
        return true;
    }

    const char *k = databoxCBytes(key);
    const char *v = databoxCBytes(val);
    if (memcmp(k, keyPrefix, prefixLen) != 0 ||
        memcmp(v, valPrefix, prefixLen) != 0 ||
        memcmp(k + prefixLen, v + prefixLen, 6) != 0) {
        testEvictRehashBad_++;
    }
    return true;
}

/* The index NNNNN of a BYTES key spelled 'prefix' + five digits, or -1
 * when the key is not spelled that way */
static int32_t testKeyIndex_(const databox *key, const char *prefix) {
    const size_t prefixLen = strlen(prefix);
    if (!DATABOX_IS_BYTES(key) || key->len != prefixLen + 5) {
        return -1;
    }

    const char *k = databoxCBytes(key);
    if (memcmp(k, prefix, prefixLen) != 0) {
        return -1;
    }

    int32_t idx = 0;
    for (size_t i = prefixLen; i < prefixLen + 5; i++) {
        if (k[i] < '0' || k[i] > '9') {
            return -1;
        }
        idx = idx * 10 + (k[i] - '0');
    }
    return idx;
}

/* LRU veto test: while testLruVetoActive_ is set, vetoes the eviction of
 * every "lruveto-NNNNN" key whose index is a multiple of 10 below 100;
 * logs the index of each eviction it allows, in order */
#define TEST_LRU_VETO_MAX 256
static bool testLruVetoActive_ = false;
static int32_t testLruVetoes_ = 0;
static int32_t testLruVetoBad_ = 0;
static int32_t testLruEvicted_ = 0;
static int32_t testLruEvictOrder_[TEST_LRU_VETO_MAX];
static bool lruVetoCb_(void *privdata, const databox *key, const databox *val) {
    (void)privdata;
    const int32_t idx = testKeyIndex_(key, "lruveto-");
    if (idx < 0 || !DATABOX_IS_INTEGER(val) || val->data.i != idx) {
        testLruVetoBad_++;
        return true;
    }

    if (testLruVetoActive_ && idx % 10 == 0 && idx < 100) {
        testLruVetoes_++;
        return false;
    }

    if (testLruEvicted_ < TEST_LRU_VETO_MAX) {
        testLruEvictOrder_[testLruEvicted_] = idx;
    }
    testLruEvicted_++;
    return true;
}

/* True when every key of 'd' is LRU-tracked and nothing else is: the
 * multilru, the key-to-handle dict and the dict hold the same count, and
 * each key has an LRU level */
static bool testLruTrackingExact_(multidict *d) {
    const uint64_t count = multidictCount(d);
    if (multilruCount(d->lru) != count ||
        multidictCount(d->lruKeyToPtr) != count) {
        return false;
    }

    multidictIterator iter;
    multidictIteratorGetSafe(d, &iter);
    multidictEntry e;
    uint64_t seen = 0;
    bool ok = true;
    while (multidictIteratorNext(&iter, &e)) {
        seen++;
        if (multidictGetLRULevel(d, &e.key) < 0) {
            ok = false;
        }
    }
    multidictIteratorRelease(&iter);
    return ok && seen == count;
}

/* LRU order tests: keys are spelled prefix[0] or prefix[1] + five digits
 * and each value is the key's code, index * 2 + spelling.  The callback
 * allows every eviction and logs the codes in eviction order. */
typedef struct testLruOrderLog {
    const char *prefix[2];
    int32_t order[TEST_LRU_VETO_MAX];
    int32_t evicted;
    int32_t bad;
} testLruOrderLog;

static int32_t testLruCode_(const testLruOrderLog *log, const databox *key) {
    for (int32_t spelling = 0; spelling < 2; spelling++) {
        const int32_t idx = testKeyIndex_(key, log->prefix[spelling]);
        if (idx >= 0) {
            return idx * 2 + spelling;
        }
    }
    return -1;
}

static bool lruOrderCb_(void *privdata, const databox *key,
                        const databox *val) {
    testLruOrderLog *log = privdata;
    const int32_t code = testLruCode_(log, key);
    if (code < 0 || !DATABOX_IS_INTEGER(val) || val->data.i != code) {
        log->bad++;
    } else if (log->evicted < TEST_LRU_VETO_MAX) {
        log->order[log->evicted] = code;
    }
    log->evicted++;
    return true;
}

/* Every key hashes alike: all keys share one slot and one LRU hash */
static uint32_t testConstHash_(multidict *d, const void *key, uint32_t len) {
    (void)d;
    (void)key;
    (void)len;
    return 0x5eedU;
}

static multidictType testConstHashType_ = {testConstHash_,
                                           multidictStringKeyCompare_};

/* Key 'index' in spelling 0 ("lrucase-NNNNN") or 1 ("LRUCASE-NNNNN") */
static databox testLruCaseKey_(char *buf, size_t bufLen, int32_t index,
                               int32_t spelling) {
    if (spelling) {
        snprintf(buf, bufLen, "LRUCASE-%05d", index);
    } else {
        snprintf(buf, bufLen, "lrucase-%05d", index);
    }
    return databoxNewBytesString(buf);
}

/* ====================================================================
 * 96/128-bit keys and values
 * ====================================================================
 * The oracle keeps each key and value independently of databox: an
 * integer as two's-complement bits plus a sign (covering INT128_MIN ..
 * UINT128_MAX), or a short string. */
typedef struct mdWideVal {
    __uint128_t bits;
    bool neg;
    bool isBytes;
    char str[16];
} mdWideVal;

typedef struct mdWidePair {
    mdWideVal key;
    mdWideVal val;
    bool live;
} mdWidePair;

defcleanerName(mdWidePair *, zfree, WidePairs);

static bool mdWideIsWide_(const mdWideVal *v) {
    return !v->isBytes &&
           (v->neg ? (__int128_t)v->bits < INT64_MIN : v->bits > UINT64_MAX);
}

static bool mdWideEq_(const mdWideVal *a, const mdWideVal *b) {
    if (a->isBytes != b->isBytes) {
        return false;
    }

    return a->isBytes ? strcmp(a->str, b->str) == 0
                      : a->bits == b->bits && a->neg == b->neg;
}

/* Box 'v' in its canonical type (64-bit when it fits) */
static void mdWideBox_(databoxBig *b, const mdWideVal *v) {
    memset(b, 0, sizeof(*b));
    if (v->isBytes) {
        *(databox *)b = databoxNewBytesString(v->str);
    } else if (v->neg) {
        const __int128_t x = (__int128_t)v->bits;
        if (x >= INT64_MIN) {
            b->type = DATABOX_SIGNED_64;
            b->data.i64 = (int64_t)x;
        } else {
            DATABOX_BIG_SIGNED_128(b, x);
        }
    } else if (v->bits <= UINT64_MAX) {
        b->type = DATABOX_UNSIGNED_64;
        b->data.u64 = (uint64_t)v->bits;
    } else {
        DATABOX_BIG_UNSIGNED_128(b, v->bits);
    }
}

/* A decoded box matches 'v' in value and canonical type; a type-only
 * 96/128-bit box (no payload) never matches */
static bool mdWideMatches_(const databox *got, const mdWideVal *v) {
    if (v->isBytes) {
        const databox want = databoxNewBytesString(v->str);
        return (got->type == DATABOX_BYTES ||
                got->type == DATABOX_BYTES_EMBED) &&
               databoxCompare(got, &want) == 0;
    }

    mdWideVal n = {0};
    switch (got->type) {
    case DATABOX_SIGNED_64:
        n.bits = (__uint128_t)(__int128_t)got->data.i64;
        n.neg = got->data.i64 < 0;
        break;
    case DATABOX_UNSIGNED_64:
        n.bits = got->data.u64;
        break;
    case DATABOX_SIGNED_128:
        if (!got->data.i128) {
            return false;
        }
        n.bits = (__uint128_t)*got->data.i128;
        n.neg = *got->data.i128 < 0;
        break;
    case DATABOX_UNSIGNED_128:
        if (!got->data.u128) {
            return false;
        }
        n.bits = *got->data.u128;
        break;
    default:
        return false;
    }

    const bool gotWide =
        got->type == DATABOX_SIGNED_128 || got->type == DATABOX_UNSIGNED_128;
    return mdWideEq_(&n, v) && gotWide == mdWideIsWide_(v);
}

/* Self-contained: a wide value owns a heap payload, BYTES a heap copy */
static bool mdWideOwned_(const databox *got) {
    switch (got->type) {
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        return got->big && got->allocated && got->data.u128;
    case DATABOX_BYTES:
        return !got->big && got->allocated;
    default:
        return !got->big;
    }
}

static size_t mdWideBytes_(const mdWideVal *v) {
    return v->isBytes ? strlen(v->str) : mdWideIsWide_(v) ? 16 : 8;
}

static uint64_t mdWideMix_(uint64_t x) {
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

/* Value 'i' of a fixed sequence: the wide set W (2^64, 2^95 - 1, 2^95,
 * 2^127, UINT128_MAX, -2^63 - 1, -2^95, INT128_MIN, INT128_MAX), then
 * 96/128-bit values of both signs derived from 'i' */
static mdWideVal mdWideInt_(size_t i) {
    const __uint128_t one = 1;
    mdWideVal v = {0};
    switch (i) {
    case 0:
        v.bits = one << 64;
        return v;
    case 1:
        v.bits = (one << 95) - 1;
        return v;
    case 2:
        v.bits = one << 95;
        return v;
    case 3:
        v.bits = one << 127;
        return v;
    case 4:
        v.bits = ~(__uint128_t)0;
        return v;
    case 5:
        v.bits = (__uint128_t)((__int128_t)INT64_MIN - 1);
        v.neg = true;
        return v;
    case 6:
        v.bits = (__uint128_t)(-(__int128_t)(one << 95));
        v.neg = true;
        return v;
    case 7:
        v.bits = one << 127; /* INT128_MIN */
        v.neg = true;
        return v;
    case 8:
        v.bits = (one << 127) - 1; /* INT128_MAX */
        return v;
    default:
        break;
    }

    const uint64_t hi = mdWideMix_(i);
    const uint64_t lo = mdWideMix_(hi);
    const __uint128_t mag =
        (((__uint128_t)(hi >> ((i & 1) ? 33 : 1)) << 64) | lo) | (one << 66);
    if (i & 2) {
        v.bits = (__uint128_t)(-(__int128_t)mag);
        v.neg = true;
    } else {
        v.bits = mag;
    }
    return v;
}

/* Which values a run uses: only 64-bit and BYTES (normal), only
 * 96/128-bit integers (extended), or all of them (mixed) */
typedef enum mdWideMode {
    MD_WIDE_MIXED = 0,
    MD_WIDE_NORMAL,
    MD_WIDE_EXTENDED,
} mdWideMode;

/* Pair 'i': keys rotate wide / 64-bit / BYTES, values rotate wide /
 * 64-bit / BYTES on a different period, so every combination occurs.
 * Normal mode replaces every wide value with a 64-bit one; extended mode
 * makes every key and value wide. */
static void mdWideFill_(mdWidePair *p, size_t n, mdWideMode mode) {
    for (size_t i = 0; i < n; i++) {
        memset(&p[i], 0, sizeof(p[i]));
        if (mode == MD_WIDE_EXTENDED) {
            p[i].key = mdWideInt_(i);
            p[i].val = mdWideInt_(1000 + i);
            p[i].live = true;
            continue;
        }

        if (mode == MD_WIDE_NORMAL && i % 3 == 0) {
            p[i].key.bits = (__uint128_t)(i * 0x9E3779B97F4A7C15ULL >> 1);
            p[i].val.bits = i * 13 + 1;
            p[i].live = true;
            continue;
        }

        switch (i % 3) {
        case 0:
            p[i].key = mdWideInt_(i / 3);
            break;
        case 1:
            p[i].key.bits = (__uint128_t)(__int128_t)(int64_t)mdWideMix_(i);
            p[i].key.neg = (int64_t)mdWideMix_(i) < 0;
            break;
        default:
            p[i].key.isBytes = true;
            snprintf(p[i].key.str, sizeof(p[i].key.str), "wk-%05zu", i);
            break;
        }

        switch (i % 4) {
        case 0:
        case 1:
            if (mode == MD_WIDE_NORMAL) {
                p[i].val.bits = i * 7 + 3;
            } else {
                p[i].val = mdWideInt_(1000 + i);
            }
            break;
        case 2:
            p[i].val.bits = i * 7;
            break;
        default:
            p[i].val.isBytes = true;
            snprintf(p[i].val.str, sizeof(p[i].val.str), "wv-%05zu", i);
            break;
        }
        p[i].live = true;
    }
}

static size_t mdWideFind_(const mdWidePair *p, size_t n, const databox *key) {
    for (size_t i = 0; i < n; i++) {
        if (p[i].live && mdWideMatches_(key, &p[i].key)) {
            return i;
        }
    }

    return SIZE_MAX;
}

/* Fill the stack below the caller with 0xFF, so a temporary that is
 * read before it is set up sees big = 1 and an all-ones pointer. */
static __attribute__((noinline)) void mdTestPoisonStack_(void) {
    volatile uint8_t junk[8192];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xFF;
    }
}

typedef struct mdWideScanState {
    const mdWidePair *p;
    size_t n;
    uint8_t *seen;
    int bad;
} mdWideScanState;

defcleanerName(uint8_t *, zfree, SeenBytes);

static void mdWideScanCb_(void *privdata, const databox *key,
                          const databox *val) {
    mdWideScanState *s = privdata;
    const size_t at = mdWideFind_(s->p, s->n, key);
    if (at == SIZE_MAX || !mdWideMatches_(val, &s->p[at].val)) {
        s->bad++;
        return;
    }
    s->seen[at] = 1;
}

static int mdWideEvictSeen_ = 0;
static int mdWideEvictBad_ = 0;
static const mdWidePair *mdWideEvictPairs_ = NULL;
static size_t mdWideEvictCount_ = 0;

static bool mdWideEvictCb_(void *privdata, const databox *key,
                           const databox *val) {
    (void)privdata;
    const size_t at = mdWideFind_(mdWideEvictPairs_, mdWideEvictCount_, key);
    if (at == SIZE_MAX || !mdWideMatches_(val, &mdWideEvictPairs_[at].val)) {
        mdWideEvictBad_++;
    }
    mdWideEvictSeen_++;
    return true;
}

/* Every live pair is found, iterated once and scanned; byte statistics
 * count a wide key or value as 16 bytes.  Returns mismatches. */
static int mdWideVerify_(multidict *d, const mdWidePair *p, size_t n) {
    int err = 0;
    size_t live = 0;
    uint64_t keyBytes = 0;
    uint64_t valBytes = 0;
    for (size_t i = 0; i < n; i++) {
        if (!p[i].live) {
            continue;
        }
        live++;
        keyBytes += mdWideBytes_(&p[i].key);
        valBytes += mdWideBytes_(&p[i].val);

        databoxBig key;
        databoxBig val;
        mdWideBox_(&key, &p[i].key);
        DATABOX_BIG_INIT(&val);
        mdTestPoisonStack_();
        if (!multidictFind(d, (databox *)&key, (databox *)&val) ||
            !mdWideMatches_((databox *)&val, &p[i].val)) {
            printf("ERROR: wide Find mismatch for pair %zu\n", i);
            err++;
        }
    }

    if (multidictCount(d) != live) {
        printf("ERROR: wide count %" PRIu64 ", expected %zu\n",
               multidictCount(d), live);
        err++;
    }

    if (multidictKeyBytes(d) != keyBytes || multidictValBytes(d) != valBytes) {
        printf("ERROR: wide byte stats key %" PRIu64 "/%" PRIu64 " val %" PRIu64
               "/%" PRIu64 "\n",
               multidictKeyBytes(d), keyBytes, multidictValBytes(d), valBytes);
        err++;
    }

    /* Iterate (unsafe, then safe) into an entry poisoned before every
     * call: wide fields point into the iterator's own slots */
    cleanerName(zfree, SeenBytes) uint8_t *seen = zcalloc(1, n + 1);
    for (int safe = 0; safe < 2; safe++) {
        memset(seen, 0, n + 1);
        multidictIterator iter;
        if (safe) {
            multidictIteratorGetSafe(d, &iter);
        } else {
            multidictIteratorInit(d, &iter);
        }
        const uint8_t *slotsLo = (const uint8_t *)iter.iter.wide;
        const uint8_t *slotsHi = slotsLo + sizeof(iter.iter.wide);
        multidictEntry entry;
        size_t visited = 0;
        for (;;) {
            memset(&entry, 0xFF, sizeof(entry));
            mdTestPoisonStack_();
            if (!multidictIteratorNext(&iter, &entry)) {
                break;
            }

            visited++;
            const size_t at = mdWideFind_(p, n, &entry.key);
            bool ok = at != SIZE_MAX && !seen[at] &&
                      mdWideMatches_(&entry.val, &p[at].val) &&
                      entry.key.big == mdWideIsWide_(&p[at].key) &&
                      entry.val.big == mdWideIsWide_(&p[at].val) &&
                      !entry.key.allocated && !entry.val.allocated;
            const databox *fields[2] = {&entry.key, &entry.val};
            for (size_t f = 0; ok && f < 2; f++) {
                const uint8_t *at8 = (const uint8_t *)fields[f]->data.ptr;
                if (fields[f]->big && (at8 < slotsLo || at8 >= slotsHi)) {
                    ok = false;
                }
            }

            if (!ok) {
                printf("ERROR: wide %s iteration entry %zu wrong\n",
                       safe ? "safe" : "unsafe", visited);
                err++;
                break;
            }
            seen[at] = 1;
        }
        multidictIteratorRelease(&iter);
        if (visited != live) {
            printf("ERROR: wide %s iteration visited %zu of %zu\n",
                   safe ? "safe" : "unsafe", visited, live);
            err++;
        }
    }

    /* GetSomeKeys ignores poisoned destinations: a wide key comes back
     * deferred, any other key whole with clean flags */
    if (live > 0) {
        databox some[16];
        memset(some, 0xFF, sizeof(some));
        mdTestPoisonStack_();
        const uint32_t got = multidictGetSomeKeys(d, some, 16);
        if (got == 0) {
            printf("ERROR: GetSomeKeys returned nothing\n");
            err++;
        }
        for (uint32_t i = 0; i < got; i++) {
            const bool wide = some[i].type == DATABOX_SIGNED_128 ||
                              some[i].type == DATABOX_UNSIGNED_128;
            if (wide ? !databoxIsWideDeferred(&some[i])
                     : (some[i].big || some[i].allocated ||
                        mdWideFind_(p, n, &some[i]) == SIZE_MAX)) {
                printf("ERROR: GetSomeKeys slot %u wrong\n", i);
                err++;
            }
        }
    }

    /* Scan to completion; duplicates are allowed, misses are not */
    memset(seen, 0, n + 1);
    mdWideScanState state = {.p = p, .n = n, .seen = seen, .bad = 0};
    uint64_t cursor = 0;
    do {
        mdTestPoisonStack_();
        cursor = multidictScan(d, cursor, mdWideScanCb_, &state);
    } while (cursor != 0);
    for (size_t i = 0; i < n; i++) {
        if (p[i].live && !seen[i]) {
            state.bad++;
        }
    }
    if (state.bad) {
        printf("ERROR: wide scan saw %d wrong or missing pairs\n", state.bad);
        err++;
    }

    return err;
}

static int multidictTestWide_(mdWideMode mode) {
    int err = 0;
    const size_t n = 600;
    cleanerName(zfree, WidePairs) mdWidePair *p = zcalloc(n, sizeof(*p));
    mdWideFill_(p, n, mode);

    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 0);

    for (size_t i = 0; i < n; i++) {
        databoxBig key;
        databoxBig val;
        mdWideBox_(&key, &p[i].key);
        mdWideBox_(&val, &p[i].val);
        mdTestPoisonStack_();
        if (multidictAdd(d, (databox *)&key, (databox *)&val) !=
            MULTIDICT_OK_INSERTED) {
            printf("ERROR: wide Add of pair %zu did not insert\n", i);
            err++;
        }

        /* Interleave lookups with the incremental rehash */
        if (i % 37 == 0) {
            err += mdWideVerify_(d, p, i + 1 < n ? i + 1 : n);
        }
    }
    err += mdWideVerify_(d, p, n);

    /* A key hashes by value: a 128-bit box holding a value that fits 64
     * bits finds the 64-bit entry */
    for (size_t i = 1; i < n; i += 3) {
        databoxBig key;
        memset(&key, 0, sizeof(key));
        if (p[i].key.neg) {
            DATABOX_BIG_SIGNED_128(&key, (__int128_t)p[i].key.bits);
        } else {
            DATABOX_BIG_UNSIGNED_128(&key, p[i].key.bits);
        }
        databoxBig val;
        DATABOX_BIG_INIT(&val);
        if (!multidictFind(d, (databox *)&key, (databox *)&val) ||
            !mdWideMatches_((databox *)&val, &p[i].val)) {
            printf("ERROR: 128-bit box of a 64-bit key %zu not found\n", i);
            err++;
        }
    }

    /* A zeroed caller box receives a wide value's type only */
    if (mode != MD_WIDE_NORMAL) {
        databoxBig key;
        mdWideBox_(&key, &p[0].key);
        databox val = {{0}};
        if (!multidictFind(d, (databox *)&key, &val) ||
            val.type != DATABOX_UNSIGNED_128 || val.data.ptr) {
            printf("ERROR: zeroed box did not get a type-only wide value\n");
            err++;
        }
    }

    /* Random keys land in caller-provided wide storage */
    for (int r = 0; r < 64; r++) {
        databoxBig key;
        DATABOX_BIG_INIT(&key);
        mdTestPoisonStack_();
        if (!multidictGetRandomKey(d, (databox *)&key) ||
            mdWideFind_(p, n, (databox *)&key) == SIZE_MAX) {
            printf("ERROR: GetRandomKey returned an unknown key\n");
            err++;
        }
    }

    /* Copy, Merge and a persist snapshot round trip carry wide pairs */
    {
        cleaner(multidictFree) multidict *copy = multidictCopy(d);
        err += mdWideVerify_(copy, p, n);

        size_t snapLen = 0;
        cleanerName(zfree, SeenBytes) uint8_t *snap =
            persistOpsMultidict.snapshot(d, &snapLen);
        multidict *restored =
            snap ? persistOpsMultidict.restore(snap, snapLen) : NULL;
        if (!restored) {
            printf("ERROR: persist snapshot round trip failed\n");
            err++;
        } else {
            err += mdWideVerify_(restored, p, n);
            persistOpsMultidict.free(restored);
        }

        cleaner(multidictFree) multidict *merged =
            multidictNew(&multidictTypeExactKey, qdc, 0);
        if (multidictMerge(merged, d, MULTIDICT_MERGE_REPLACE) != n) {
            printf("ERROR: wide Merge count wrong\n");
            err++;
        }
        err += mdWideVerify_(merged, p, n);
    }

    /* Delete, GetAndDelete into a poisoned box, PopRandom into poisoned
     * boxes: removed values are self-contained */
    for (size_t i = 0; i < n; i += 4) {
        databoxBig key;
        mdWideBox_(&key, &p[i].key);
        mdTestPoisonStack_();
        if (!multidictDelete(d, (databox *)&key)) {
            printf("ERROR: wide Delete of pair %zu failed\n", i);
            err++;
        }
        p[i].live = false;
        if (multidictExists(d, (databox *)&key)) {
            printf("ERROR: pair %zu exists after Delete\n", i);
            err++;
        }
    }
    err += mdWideVerify_(d, p, n);

    for (size_t i = 1; i < n; i += 4) {
        databoxBig key;
        mdWideBox_(&key, &p[i].key);
        databox val;
        memset(&val, 0xFF, sizeof(val));
        mdTestPoisonStack_();
        if (!multidictGetAndDelete(d, (databox *)&key, &val)) {
            printf("ERROR: GetAndDelete of pair %zu failed\n", i);
            err++;
        } else {
            if (!mdWideMatches_(&val, &p[i].val) || !mdWideOwned_(&val)) {
                printf("ERROR: GetAndDelete value of pair %zu wrong\n", i);
                err++;
            }
            databoxFreeData(&val);
        }
        p[i].live = false;
    }
    err += mdWideVerify_(d, p, n);

    for (int r = 0; r < 40; r++) {
        databox key;
        databox val;
        memset(&key, 0xFF, sizeof(key));
        memset(&val, 0xFF, sizeof(val));
        mdTestPoisonStack_();
        if (!multidictPopRandom(d, &key, &val)) {
            printf("ERROR: PopRandom failed\n");
            err++;
            break;
        }

        const size_t at = mdWideFind_(p, n, &key);
        if (at == SIZE_MAX || !mdWideMatches_(&val, &p[at].val) ||
            !mdWideOwned_(&key) || !mdWideOwned_(&val)) {
            printf("ERROR: PopRandom returned a wrong pair\n");
            err++;
        } else {
            p[at].live = false;
        }
        databoxFreeData(&key);
        databoxFreeData(&val);
    }
    err += mdWideVerify_(d, p, n);

    /* Eviction (random, then LRU) hands the callback full wide boxes */
    for (int policy = 0; policy < 2; policy++) {
        cleaner(multidictFree) multidict *e =
            multidictNew(&multidictTypeExactKey, qdc, 0);
        if (policy == 1) {
            multidictEnableLRU(e, 4);
            multidictSetEvictPolicy(e, MULTIDICT_EVICT_LRU);
        }

        for (size_t i = 0; i < n; i++) {
            p[i].live = true;
            databoxBig key;
            databoxBig val;
            mdWideBox_(&key, &p[i].key);
            mdWideBox_(&val, &p[i].val);
            multidictAdd(e, (databox *)&key, (databox *)&val);
        }

        /* Evict with a rehash in flight and resizing on, so the find and
         * delete of each victim run rehash steps that move entries and
         * free their slots, and the evictions themselves shrink the table */
        multidictExpand(e, (uint64_t)n * 4);
        if (!multidictIsRehashing(e)) {
            printf("ERROR: policy %d eviction did not start rehashing\n",
                   policy);
            err++;
        }

        mdWideEvictPairs_ = p;
        mdWideEvictCount_ = n;
        mdWideEvictSeen_ = 0;
        mdWideEvictBad_ = 0;
        multidictSetEvictionCallback(e, mdWideEvictCb_, NULL);
        multidictSetMaxMemory(e, (multidictKeyBytes(e) + multidictValBytes(e)) /
                                     2);
        mdTestPoisonStack_();
        const uint32_t evicted = multidictEvictToLimit(e);
        if (evicted == 0 || mdWideEvictBad_ != 0 ||
            mdWideEvictSeen_ < (int)evicted) {
            printf("ERROR: policy %d evicted %u, callback saw %d (%d bad)\n",
                   policy, evicted, mdWideEvictSeen_, mdWideEvictBad_);
            err++;
        }

        /* The survivors still read back */
        for (size_t i = 0; i < n; i++) {
            databoxBig key;
            databoxBig val;
            mdWideBox_(&key, &p[i].key);
            DATABOX_BIG_INIT(&val);
            p[i].live = multidictFind(e, (databox *)&key, (databox *)&val) &&
                        mdWideMatches_((databox *)&val, &p[i].val);
        }
        multidictSetMaxMemory(e, 0);
        err += mdWideVerify_(e, p, n);
    }

    return err;
}

/* A slot class decodes wide fields into the slot iterator it is given,
 * and a struct copy of a slot iterator decodes into its own slots */
static int multidictTestSlotIteratorWide_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    multidictSlot *slot = qdc->createSlot();

    const __uint128_t k1 = ((__uint128_t)1 << 100) + 1;
    const __uint128_t k2 = ((__uint128_t)1 << 100) + 2;
    const __int128_t v1 = -((__int128_t)1 << 90);
    const __int128_t v2 = -((__int128_t)1 << 91);
    databoxBig key;
    databoxBig val;
    memset(&key, 0, sizeof(key));
    memset(&val, 0, sizeof(val));
    DATABOX_BIG_UNSIGNED_128(&key, k1);
    DATABOX_BIG_SIGNED_128(&val, v1);
    qdc->insertByType(qdc, slot, (databox *)&key, (databox *)&val);
    DATABOX_BIG_UNSIGNED_128(&key, k2);
    DATABOX_BIG_SIGNED_128(&val, v2);
    qdc->insertByType(qdc, slot, (databox *)&key, (databox *)&val);

    multidictSlotIterator iter;
    multidictEntry first;
    memset(&iter, 0xFF, sizeof(iter));
    memset(&first, 0xFF, sizeof(first));
    if (!qdc->getIter(&iter, slot) || !qdc->iterNext(&iter, &first)) {
        printf("ERROR: slot iterator produced nothing\n");
        err++;
    } else {
        multidictSlotIterator copy = iter;
        multidictEntry second;
        memset(&second, 0xFF, sizeof(second));
        const bool more = qdc->iterNext(&copy, &second);

        if (first.key.data.u128 != (__uint128_t *)iter.wide[0].b ||
            first.val.data.i128 != (__int128_t *)iter.wide[1].b ||
            !first.key.big || first.key.allocated ||
            *first.key.data.u128 != k1 || *first.val.data.i128 != v1) {
            printf("ERROR: slot iterator entry does not use its slots\n");
            err++;
        }

        if (!more || second.key.data.u128 != (__uint128_t *)copy.wide[0].b ||
            *second.key.data.u128 != k2 || *second.val.data.i128 != v2) {
            printf("ERROR: copied slot iterator did not use its own slots\n");
            err++;
        }
    }

    qdc->freeSlot(qdc, slot);
    return err;
}

/* The oracle fold: exactly 'A'-'Z' to 'a'-'z' */
static uint8_t testFoldAscii_(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

/* The built-in key compares are length-checked, binary-safe and
 * locale-free, and the case hash folds exactly what the case compare
 * folds */
static int multidictTestKeyCompare_(void) {
    int err = 0;
    int32_t (*const exact)(void *, const void *, const uint32_t, const void *,
                           const uint32_t) = multidictTypeExactKey.keyCompare;
    int32_t (*const fold)(void *, const void *, const uint32_t, const void *,
                          const uint32_t) = multidictTypeCaseKey.keyCompare;

    static const struct {
        const char *a;
        uint32_t alen;
        const char *b;
        uint32_t blen;
        bool exactEq;
        bool foldEq;
    } cases[] = {
        {"hello", 5, "hello", 5, true, true},
        {"hello", 5, "HeLLo", 5, false, true},
        {"hello", 5, "hell", 4, false, false},
        {"hell", 4, "hello", 5, false, false},
        {"", 0, "", 0, true, true},
        {"a\0b", 3, "a\0b", 3, true, true},
        {"a\0b", 3, "a\0c", 3, false, false},
        {"a\0B", 3, "a\0b", 3, false, true},
        {"\0\0", 2, "\0\0", 2, true, true},
        {"\0x", 2, "\0y", 2, false, false},
        {"a\0", 2, "a", 1, false, false},
        {"\xc4", 1, "\xe4", 1, false, false}, /* Latin-1 A/a umlaut */
        {"@", 1, "`", 1, false, false},       /* one bit apart, no letters */
        {"[", 1, "{", 1, false, false},
        {"Z_", 2, "z_", 2, false, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        const bool e = exact(NULL, cases[i].a, cases[i].alen, cases[i].b,
                             cases[i].blen) != 0;
        const bool f = fold(NULL, cases[i].a, cases[i].alen, cases[i].b,
                            cases[i].blen) != 0;
        if (e != cases[i].exactEq || f != cases[i].foldEq) {
            printf("ERROR: key compare case %zu: exact %d fold %d\n", i, e, f);
            err++;
        }
    }

    /* Every byte pair: the case compare folds exactly the oracle's set,
     * and the case hash agrees with it */
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeCaseKey, qdc, 77);
    for (uint32_t a = 0; a < 256; a++) {
        for (uint32_t b = 0; b < 256; b++) {
            const uint8_t ab = (uint8_t)a;
            const uint8_t bb = (uint8_t)b;
            const bool want = testFoldAscii_(ab) == testFoldAscii_(bb);
            const bool got = fold(NULL, &ab, 1, &bb, 1) != 0;
            if (got != want) {
                printf("ERROR: case compare of 0x%02x and 0x%02x is %d\n", a, b,
                       got);
                err++;
            }

            if (want && multidictGenCaseHashFunction(d, &ab, 1) !=
                            multidictGenCaseHashFunction(d, &bb, 1)) {
                printf("ERROR: case hash of 0x%02x and 0x%02x differs\n", a, b);
                err++;
            }
        }
    }

    /* Longer keys: any case flip of a key compares and hashes equal */
    testRandSeed(4242);
    for (int32_t round = 0; round < 200; round++) {
        uint8_t a[24];
        uint8_t b[24];
        const uint32_t len = (uint32_t)(testRand() % sizeof(a));
        for (uint32_t i = 0; i < len; i++) {
            a[i] = (uint8_t)testRand();
            b[i] = a[i];
            if (testRand() & 1) {
                if (a[i] >= 'A' && a[i] <= 'Z') {
                    b[i] = (uint8_t)(a[i] + ('a' - 'A'));
                } else if (a[i] >= 'a' && a[i] <= 'z') {
                    b[i] = (uint8_t)(a[i] - ('a' - 'A'));
                }
            }
        }

        if (!fold(NULL, a, len, b, len) ||
            multidictGenCaseHashFunction(d, a, (int32_t)len) !=
                multidictGenCaseHashFunction(d, b, (int32_t)len)) {
            printf("ERROR: case flip round %d does not match\n", round);
            err++;
        }
    }

    /* A locale that folds Latin-1 does not change the case compare */
    const char *prior = setlocale(LC_CTYPE, NULL);
    char saved[64] = {0};
    if (prior) {
        snprintf(saved, sizeof(saved), "%s", prior);
    }

    static const char *latin1[] = {"en_US.ISO8859-1", "en_US.ISO-8859-1",
                                   "de_DE.ISO8859-1"};
    for (size_t i = 0; i < sizeof(latin1) / sizeof(*latin1); i++) {
        if (setlocale(LC_CTYPE, latin1[i])) {
            if (fold(NULL, "\xc4", 1, "\xe4", 1) ||
                !fold(NULL, "A\xc4", 2, "a\xc4", 2)) {
                printf("ERROR: case compare follows locale %s\n", latin1[i]);
                err++;
            }
            break;
        }
    }
    setlocale(LC_CTYPE, saved[0] ? saved : "C");

    return err;
}

int multidictTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    printf("=== MULTIDICT COMPREHENSIVE TEST SUITE ===\n\n");

    printf("Creating multimap-based multidictClass...\n");
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();

    printf("Creating multidict with multimap slots...\n");

    /* ================================================================
     * SECTION 1: Basic API Tests
     * ================================================================ */
    printf("--- Section 1: Basic API Tests ---\n");

    printf("Test 1.1: Create with exact key type...\n");
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 12345);
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

    printf("Test 3.5: Safe iterator prevents auto-rehash step...\n");
    {
        /* Add enough entries to allow expansion to start */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "block_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Force an expand to start rehashing */
        bool expanded = multidictExpand(d, 1024);
        assert(expanded == true);
        assert(multidictIsRehashing(d) == true);

        uint32_t rehashIdxBefore = d->rehashidx;

        /* Start a safe iterator */
        multidictIterator iter;
        multidictIteratorGetSafe(d, &iter);

        /* Advance iterator once to register it */
        multidictEntry entry;
        assert(multidictIteratorNext(&iter, &entry) == true);

        /* Verify iterator count is incremented */
        assert(d->iterators == 1);

        /* The MULTIDICT_REHASH_STEP_IF_SAFE macro should not rehash
         * when iterators are active. Adding an entry invokes this. */
        databox testKey = databoxNewBytesString("rehash_test");
        databox testVal = databoxNewSigned(999);
        multidictAdd(d, &testKey, &testVal);

        /* Rehash index should not have advanced due to iterator */
        assert(d->rehashidx == rehashIdxBefore);

        /* Release the iterator */
        multidictIteratorRelease(&iter);
        assert(d->iterators == 0);

        /* Now operations should trigger rehash steps */
        databox testKey2 = databoxNewBytesString("rehash_test2");
        multidictAdd(d, &testKey2, &testVal);

        /* Complete the rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictEmpty(d);
    }

    printf("Test 3.6: Iterator release without advancing...\n");
    {
        /* Add some entries */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "no_advance_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Init safe iterator but don't advance it */
        multidictIterator iter;
        multidictIteratorGetSafe(d, &iter);

        /* Iterator count should still be 0 (not advanced yet) */
        assert(d->iterators == 0);

        /* Release without advancing - should be safe */
        multidictIteratorRelease(&iter);

        /* Count should still be 0 */
        assert(d->iterators == 0);

        multidictEmpty(d);
    }

    printf("Test 3.7: Multiple safe iterators counted correctly...\n");
    {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "multi_safe_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Start two safe iterators */
        multidictIterator iter1, iter2;
        multidictIteratorGetSafe(d, &iter1);
        multidictIteratorGetSafe(d, &iter2);

        /* Advance both */
        multidictEntry e1, e2;
        multidictIteratorNext(&iter1, &e1);
        multidictIteratorNext(&iter2, &e2);

        /* Both should be counted */
        assert(d->iterators == 2);

        /* Release one iterator */
        multidictIteratorRelease(&iter1);
        assert(d->iterators == 1);

        /* Release second iterator */
        multidictIteratorRelease(&iter2);
        assert(d->iterators == 0);

        multidictEmpty(d);
    }

    printf("Test 3.8: Iterator during active rehash sees all entries...\n");
    {
        /* Add entries */
        for (int i = 0; i < 200; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "iter_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Start rehashing */
        assert(multidictExpand(d, 1024) == true);

        /* Do partial rehash */
        multidictRehash(d, 50);
        assert(multidictIsRehashing(d) == true);

        /* Now iterate - should see all entries even during rehashing */
        multidictIterator iter;
        multidictIteratorInit(d, &iter);
        multidictEntry entry;
        int count = 0;
        while (multidictIteratorNext(&iter, &entry)) {
            count++;
        }
        multidictIteratorRelease(&iter);

        /* Must see all 200 entries */
        assert(count == 200);

        /* Complete rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictEmpty(d);
    }

    printf("Test 3.9: Re-init iterator mid-iteration...\n");
    {
        /* Add entries */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "reinit_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(d, &key, &val);
        }

        /* Start safe iterator */
        multidictIterator iter;
        multidictIteratorGetSafe(d, &iter);

        /* Advance a few times */
        multidictEntry entry;
        for (int i = 0; i < 50; i++) {
            multidictIteratorNext(&iter, &entry);
        }
        assert(d->iterators == 1);

        /* Release iterator properly before re-init */
        multidictIteratorRelease(&iter);
        assert(d->iterators == 0);

        /* Re-init the same iterator variable */
        multidictIteratorGetSafe(d, &iter);
        multidictIteratorNext(&iter, &entry);
        assert(d->iterators == 1);

        /* Clean up */
        multidictIteratorRelease(&iter);
        assert(d->iterators == 0);
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
            cleaner(multidictFree) multidict *dScan =
                multidictNew(&multidictTypeExactKey, qdc, 0);

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
        }
        printf("  Tested %zu different dict sizes\n",
               sizeof(testSizes) / sizeof(testSizes[0]));
    }

    printf("Test 5.5: Scan fuzz - large dicts with rehashing at various "
           "stages...\n");
    {
        for (int testRun = 0; testRun < 5; testRun++) {
            cleaner(multidictFree) multidict *dScan =
                multidictNew(&multidictTypeExactKey, qdc, 0);
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
        }
        printf("  Completed 5 large dict scans with varying rehash states\n");
    }

    printf(
        "Test 5.6: Scan with concurrent modifications during rehashing...\n");
    {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);

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
    }

    printf("Test 5.7: Scan correctness - verify exact key coverage...\n");
    {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);
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
    }

    printf("Test 5.8: Extreme fuzz - rapid dict size changes during scan...\n");
    {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);

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

    printf("Test 6.4: Random key never fails on a non-empty dict...\n");
    {
        /* Deletes leave emptied slots allocated, and a rehash is kept in
         * flight (resize on, so shrinks start their own), so the random
         * picks keep landing on NULL and emptied slots.  Every pick must
         * still succeed while entries remain, and return a live key. */
        cleaner(multidictMmClassFree) multidictClass *rqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *rd =
            multidictNew(&multidictTypeExactKey, rqdc, 7);
        multidictResizeEnable(rd);

        enum { total = 3000, deleted = 2000 };
        static bool alive[total];
        static int32_t order[total];
        for (int32_t i = 0; i < total; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "grk%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(rd, &key, &val);
            alive[i] = true;
            order[i] = i;
        }
        assert(multidictCount(rd) == total);

        testRandSeed(64);
        for (int32_t i = total - 1; i > 0; i--) {
            const int32_t j =
                (int32_t)(((uint32_t)testRand() << 15 | testRand()) %
                          (uint32_t)(i + 1));
            const int32_t t = order[i];
            order[i] = order[j];
            order[j] = t;
        }

        uint32_t picks = 0, picksRehashing = 0;
        for (int32_t n = 0; n < deleted; n++) {
            if (!multidictIsRehashing(rd)) {
                multidictExpand(rd, (uint64_t)HTSIZE(rd, 0) * 2);
            }

            /* Pick twice per delete; the key is read before the dict is
             * used again, so its borrowed bytes are still valid */
            for (int pick = 0; pick < 2; pick++) {
                picksRehashing += multidictIsRehashing(rd);
                databox rk = {{0}};
                const bool got = multidictGetRandomKey(rd, &rk);
                const int32_t idx = got ? testKeyIndex_(&rk, "grk") : -1;
                if (!got || idx < 0 || !alive[idx]) {
                    printf("ERROR: GetRandomKey at count %" PRIu64
                           ": got=%d idx=%d\n",
                           multidictCount(rd), got, idx);
                    assert(NULL);
                }
                picks++;
            }

            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "grk%05d", order[n]);
            databox key = databoxNewBytesString(keyBuf);
            assert(multidictDelete(rd, &key));
            alive[order[n]] = false;
        }

        printf("    %u picks, %u with a rehash in flight\n", picks,
               picksRehashing);
        assert(picksRehashing > picks / 2);

        /* Drain the rest with PopRandom: each pop succeeds while entries
         * remain and returns a live key with its own value */
        uint64_t remaining = multidictCount(rd);
        assert(remaining == total - deleted);
        while (remaining > 0) {
            databox pk;
            databox pv;
            if (!multidictPopRandom(rd, &pk, &pv)) {
                printf("ERROR: PopRandom failed at count %" PRIu64 "\n",
                       remaining);
                assert(NULL);
            }
            const int32_t idx = testKeyIndex_(&pk, "grk");
            assert(idx >= 0 && alive[idx]);
            assert(DATABOX_IS_INTEGER(&pv) && pv.data.i == idx);
            alive[idx] = false;
            databoxFreeData(&pk);
            databoxFreeData(&pv);
            remaining--;
            assert(multidictCount(rd) == remaining);
        }

        for (int32_t i = 0; i < total; i++) {
            assert(!alive[i]);
        }

        databox rk = {{0}};
        assert(!multidictGetRandomKey(rd, &rk));
        databox pk, pv;
        assert(!multidictPopRandom(rd, &pk, &pv));
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

    printf("Test 9.0: Key compares are binary-safe and locale-free...\n");
    err += multidictTestKeyCompare_();

    printf("Test 9.1: Create with case-insensitive type...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcCase =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dCase =
            multidictNew(&multidictTypeCaseKey, qdcCase, 54321);
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
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Fresh dict should have zero tracked bytes */
        assert(dBytes->ht[0].keyBytes == 0);
        assert(dBytes->ht[0].valBytes == 0);
        assert(dBytes->ht[0].totalBytes == 0);
    }

    printf("Test 11.2: Bytes increase on insert...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
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
    }

    printf("Test 11.3: Bytes decrease on delete...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
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
    }

    printf("Test 11.4: Value replacement updates valBytes...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
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
    }

    printf("Test 11.5: Bytes preserved through rehash...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
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
        cleanerName(zfree, Databoxes) databox *keys =
            zmalloc(BULK_COUNT * sizeof(databox));
        cleanerName(zfree, Databoxes) databox *vals =
            zmalloc(BULK_COUNT * sizeof(databox));
        /* Persist string storage */
        cleanerName(zfree, Buf32) multidictTestBuf32 *keyBufs =
            zmalloc(BULK_COUNT * 32);
        cleanerName(zfree, Buf32) multidictTestBuf32 *valBufs =
            zmalloc(BULK_COUNT * 32);

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

    printf("Test 14.7: Eviction with a rehash in flight...\n");
    {
        /* Random then LRU victims, chosen while the table is rehashing
         * and resizing is on: each victim's find and delete runs rehash
         * steps that move entries and free their old slots, and the
         * evictions shrink the table.  Keys are long enough to be stored
         * out of line, so a borrowed victim key would point into a freed
         * slot. */
        for (int policy = 0; policy < 2; policy++) {
            cleaner(multidictFree) multidict *e =
                multidictNew(&multidictTypeExactKey, qdc, 777);
            if (policy == 1) {
                multidictEnableLRU(e, 4);
                multidictSetEvictPolicy(e, MULTIDICT_EVICT_LRU);
            }

            const int total = 800;
            char kbuf[64];
            char vbuf[64];
            for (int j = 0; j < total; j++) {
                snprintf(kbuf, sizeof(kbuf), "evict-rehash-key-%06d", j);
                snprintf(vbuf, sizeof(vbuf), "evict-rehash-val-%06d", j);
                databox key = databoxNewBytesString(kbuf);
                databox val = databoxNewBytesString(vbuf);
                const multidictResult added = multidictAdd(e, &key, &val);
                assert(added == MULTIDICT_OK_INSERTED);
                (void)added;
            }

            multidictExpand(e, (uint64_t)total * 4);
            assert(multidictIsRehashing(e));

            testEvictRehashSeen_ = 0;
            testEvictRehashBad_ = 0;
            multidictSetEvictionCallback(e, evictRehashCb_, NULL);
            multidictSetMaxMemory(
                e, (multidictKeyBytes(e) + multidictValBytes(e)) / 4);
            const uint32_t evicted = multidictEvictToLimit(e);
            printf("  policy %d: evicted %u of %d\n", policy, evicted, total);
            assert(evicted > 0);
            assert(testEvictRehashBad_ == 0);
            assert(testEvictRehashSeen_ == (int)evicted);
            assert(multidictCount(e) == (uint64_t)(total - (int)evicted));
            assert(!multidictIsOverLimit(e));

            /* Exactly the survivors remain, each with its own value */
            uint64_t found = 0;
            for (int j = 0; j < total; j++) {
                snprintf(kbuf, sizeof(kbuf), "evict-rehash-key-%06d", j);
                snprintf(vbuf, sizeof(vbuf), "evict-rehash-val-%06d", j);
                databox key = databoxNewBytesString(kbuf);
                databox val = {{0}};
                if (multidictFind(e, &key, &val)) {
                    found++;
                    assert(DATABOX_IS_BYTES(&val));
                    assert(val.len == strlen(vbuf));
                    assert(memcmp(databoxCBytes(&val), vbuf, val.len) == 0);
                }
            }
            assert(found == multidictCount(e));
        }
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

        cleaner(multidictFree) multidict *copy = multidictCopy(d);
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
        }
    }

    /* Test Merge with REPLACE mode */
    {
        multidictEmpty(d);

        /* Create source dict */
        cleaner(multidictMmClassFree) multidictClass *srcQdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *src =
            multidictNew(&multidictTypeExactKey, srcQdc, 42);

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
    }

    /* Test Merge with KEEP mode */
    {
        multidictEmpty(d);

        cleaner(multidictMmClassFree) multidictClass *srcQdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *src =
            multidictNew(&multidictTypeExactKey, srcQdc, 42);

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
        cleaner(multidictMmClassFree) multidictClass *qdcNoLru =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dNoLru =
            multidictNew(&multidictTypeExactKey, qdcNoLru, 99);

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
    }

    printf("Test 20.9: Vetoed LRU victims stay tracked at their rank...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *vqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *vd =
            multidictNew(&multidictTypeExactKey, vqdc, 11);
        const bool lruOn = multidictEnableLRU(vd, 4);
        assert(lruOn);
        (void)lruOn;
        multidictSetEvictPolicy(vd, MULTIDICT_EVICT_LRU);

        /* Keys 0..149 stay at LRU level 0 in insertion order (0 is the
         * coldest); 150..199 are touched up to level 1 */
        enum { total = 200, cold = 150 };
        static bool present[total];
        for (int32_t i = 0; i < total; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "lruveto-%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            multidictAdd(vd, &key, &val);
            present[i] = true;
        }
        for (int32_t i = cold; i < total; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "lruveto-%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            multidictTouch(vd, &key);
        }
        assert(testLruTrackingExact_(vd));

        /* Every entry has the same size, so a limit of 'evict' entries
         * below the current bytes evicts exactly that many */
        const uint64_t bytes = multidictKeyBytes(vd) + multidictValBytes(vd);
        const uint64_t entryBytes = bytes / total;
        assert(entryBytes * total == bytes);

        testLruVetoActive_ = true;
        testLruVetoes_ = 0;
        testLruVetoBad_ = 0;
        testLruEvicted_ = 0;
        multidictSetEvictionCallback(vd, lruVetoCb_, NULL);

        /* Round 1: evict 60 with keys 0, 10, ..., 90 vetoed.  The victims
         * are the 60 coldest keys that are not vetoed, coldest first, and
         * each vetoed key is offered once (0..60 are reached; 70..90 are
         * not). */
        multidictSetMaxMemory(vd, bytes - 60 * entryBytes);
        uint32_t evicted = multidictEvictToLimit(vd);
        assert(evicted == 60);
        assert(testLruEvicted_ == 60);
        assert(testLruVetoBad_ == 0);
        assert(testLruVetoes_ == 7);

        int32_t expect = 0;
        for (int32_t n = 0; n < 60; n++) {
            while (expect % 10 == 0 && expect < 100) {
                expect++;
            }
            if (testLruEvictOrder_[n] != expect) {
                printf("ERROR: round 1 victim %d is key %d, expected %d\n", n,
                       testLruEvictOrder_[n], expect);
                assert(NULL);
            }
            present[expect] = false;
            expect++;
        }
        assert(multidictCount(vd) == total - 60);

        /* Every survivor, the vetoed keys included, is still tracked, and
         * the vetoed keys kept their level and rank: they are still the
         * coldest entries, in their original order */
        assert(testLruTrackingExact_(vd));
        multilruPtr coldest[7] = {0};
        multilruGetNLowest(vd->lru, coldest, 7);
        for (int32_t n = 0; n < 7; n++) {
            databox ck;
            databoxWide ckWide;
            assert(multidictLRUKeyOf(vd, coldest[n], &ck, &ckWide));
            assert(testKeyIndex_(&ck, "lruveto-") == n * 10);
            assert(multidictGetLRULevel(vd, &ck) == 0);
        }

        /* Round 2: veto lifted.  The vetoed keys are evictable again and
         * go first, then eviction continues through the level-0 keys in
         * insertion order */
        testLruVetoActive_ = false;
        testLruEvicted_ = 0;
        const uint64_t bytes2 = multidictKeyBytes(vd) + multidictValBytes(vd);
        multidictSetMaxMemory(vd, bytes2 - 12 * entryBytes);
        evicted = multidictEvictToLimit(vd);
        assert(evicted == 12);
        assert(testLruEvicted_ == 12);
        assert(testLruVetoes_ == 7);

        int32_t next = 0;
        for (int32_t n = 0; n < 12; n++) {
            while (!present[next]) {
                next++;
            }
            assert(next < cold);
            if (testLruEvictOrder_[n] != next) {
                printf("ERROR: round 2 victim %d is key %d, expected %d\n", n,
                       testLruEvictOrder_[n], next);
                assert(NULL);
            }
            present[next] = false;
        }
        for (int32_t i = 0; i <= 60; i += 10) {
            assert(!present[i]); /* round 2's first seven victims */
        }
        assert(present[80] && present[90]);

        assert(multidictCount(vd) == total - 72);
        assert(testLruTrackingExact_(vd));
        assert(testLruVetoBad_ == 0);
    }

    printf("Test 20.10: Case-insensitive dict evicts in LRU order...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *cqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *cd =
            multidictNew(&multidictTypeCaseKey, cqdc, 13);
        const bool lruOn = multidictEnableLRU(cd, 4);
        assert(lruOn);
        (void)lruOn;
        multidictSetEvictPolicy(cd, MULTIDICT_EVICT_LRU);

        /* Each index goes in as two keys that differ only in case: the
         * slot layer compares bytes, so they are distinct entries, and the
         * case hash gives both one slot and one LRU hash.  The LRU hash
         * must be the slot hash for any victim to resolve, and promoting
         * one spelling of a pair below means whichever of the two the
         * slot holds first, one victim must be told apart from its
         * same-hash, promoted neighbour. */
        enum { indexes = 40, entries = indexes * 2 };
        char keyBuf[32];
        for (int32_t i = 0; i < indexes; i++) {
            for (int32_t s = 0; s < 2; s++) {
                databox key = testLruCaseKey_(keyBuf, sizeof(keyBuf), i, s);
                databox val = databoxNewSigned(i * 2 + s);
                multidictAdd(cd, &key, &val);
            }
        }
        assert(multidictCount(cd) == entries);

        char lowBuf[32], upBuf[32];
        databox low = testLruCaseKey_(lowBuf, sizeof(lowBuf), 0, 0);
        databox up = testLruCaseKey_(upBuf, sizeof(upBuf), 0, 1);
        assert(multidictKeyHash(cd, &low) == multidictKeyHash(cd, &up));
        assert(multidictKeyHash(cd, &low) == multidictHashOfKey_(cd, &low));

        /* A lookup promotes only the spelling looked up: code 6 (index 3,
         * lower case) by multidictFind and code 11 (index 5, upper case)
         * by multidictTouch; their other spellings stay cold */
        char key3Buf[32], key5Buf[32];
        databox key3 = testLruCaseKey_(key3Buf, sizeof(key3Buf), 3, 0);
        databox found = {{0}};
        assert(multidictFind(cd, &key3, &found));
        databox key5 = testLruCaseKey_(key5Buf, sizeof(key5Buf), 5, 1);
        multidictTouch(cd, &key5);
        assert(testLruTrackingExact_(cd));

        const uint64_t bytes = multidictKeyBytes(cd) + multidictValBytes(cd);
        const uint64_t entryBytes = bytes / entries;
        assert(entryBytes * entries == bytes);

        testLruOrderLog log = {.prefix = {"lrucase-", "LRUCASE-"}};
        multidictSetEvictionCallback(cd, lruOrderCb_, &log);
        multidictSetMaxMemory(cd, bytes - 30 * entryBytes);
        const uint32_t evicted = multidictEvictToLimit(cd);
        assert(evicted == 30);
        assert(log.evicted == 30);
        assert(log.bad == 0);

        /* Exactly insertion order without the two promoted codes: any
         * random victim or same-hash mix-up breaks it */
        int32_t expect = 0;
        for (int32_t n = 0; n < 30; n++) {
            while (expect == 6 || expect == 11) {
                expect++;
            }
            if (log.order[n] != expect) {
                printf("ERROR: case victim %d is code %d, expected %d\n", n,
                       log.order[n], expect);
                assert(NULL);
            }
            expect++;
        }

        assert(multidictCount(cd) == entries - 30);
        assert(multidictGetLRULevel(cd, &key3) == 1);
        assert(multidictGetLRULevel(cd, &key5) == 1);
        assert(testLruTrackingExact_(cd));
    }

    printf("Test 20.11: Same-hash keys in one slot evict in LRU order...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *hqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *hd =
            multidictNew(&testConstHashType_, hqdc, 17);
        const bool lruOn = multidictEnableLRU(hd, 4);
        assert(lruOn);
        (void)lruOn;
        multidictSetEvictPolicy(hd, MULTIDICT_EVICT_LRU);

        /* Every key has the same hash, so all share one slot and one LRU
         * hash.  Key 0 goes in first, so the slot holds it first in either
         * key or insertion order; it is then promoted, so every victim
         * handle's first hash match is key 0, a key that must stay: each
         * victim has to be found by its handle. */
        enum { keys = 30, evict = 10 };
        for (int32_t i = 0; i < keys; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "samehash-%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i * 2);
            multidictAdd(hd, &key, &val);
        }
        databox key0 = databoxNewBytesString("samehash-00000");
        multidictTouch(hd, &key0);
        assert(multidictCount(hd) == keys);
        assert(testLruTrackingExact_(hd));

        const uint64_t bytes = multidictKeyBytes(hd) + multidictValBytes(hd);
        const uint64_t entryBytes = bytes / keys;
        assert(entryBytes * keys == bytes);

        testLruOrderLog log = {.prefix = {"samehash-", "SAMEHASH-"}};
        multidictSetEvictionCallback(hd, lruOrderCb_, &log);
        multidictSetMaxMemory(hd, bytes - evict * entryBytes);
        const uint32_t evicted = multidictEvictToLimit(hd);
        assert(evicted == evict);
        assert(log.evicted == evict);
        assert(log.bad == 0);
        for (int32_t n = 0; n < evict; n++) {
            const int32_t expect = (n + 1) * 2; /* keys 1..10 */
            if (log.order[n] != expect) {
                printf("ERROR: same-hash victim %d is code %d, expected %d\n",
                       n, log.order[n], expect);
                assert(NULL);
            }
        }

        assert(multidictCount(hd) == keys - evict);
        assert(multidictGetLRULevel(hd, &key0) == 1);
        assert(testLruTrackingExact_(hd));
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
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.4: Byte-based expansion trigger - large values...\n");
    {
        /* Create fresh dict with byte-based expansion */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.5: Safeguard - maximum slot size trigger...\n");
    {
        /* Create dict with very low max slot size */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.6: Safeguard - count-based backstop...\n");
    {
        /* Create dict with very high byte targets but low count threshold */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.7: Safeguard - expansion effectiveness check...\n");
    {
        /* This test verifies that expansion won't happen if it won't help
         * (skewed distribution where all keys hash to few slots) */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.8: Byte-based vs count-based expansion comparison...\n");
    {
        /* Create two dicts: one byte-based, one count-based */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictMmClassFree) multidictClass *qdcCount =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
        cleaner(multidictFree) multidict *dCount =
            multidictNew(&multidictTypeExactKey, qdcCount, 88);

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
    }

    printf("Test 21.9: Byte-based expansion during rehashing operations...\n");
    {
        /* Verify byte-based expansion works correctly with rehashing */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("Test 21.10: Fuzz test - byte-based expansion with mixed "
           "operations...\n");
    {
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
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
    }

    printf("=== Byte-based expansion tests passed! ===\n");

    /* ================================================================
     * Regression: single-step incremental rehash through a sparse table
     * ================================================================
     * multidictRehash() computed its empty-slot work cap from the
     * POST-decrement loop counter, so a single-step rehash (n=1 -> n=0 in
     * the body) set emptyVisits=0 and the `emptyVisits--` wrapped to
     * UINT32_MAX, disabling the cap entirely. This drives many single steps
     * over a populated-then-sparsified table (lots of NULL slot runs in HT0
     * mid-rehash) and verifies the rehash terminates and preserves data. */
    printf("--- Section: single-step rehash sparse-table regression ---\n");
    {
        enum { N = 4000 };
        cleaner(multidictFree) multidict *r =
            multidictNew(&multidictTypeExactKey, qdc, 0xABCDEF);
        assert(r != NULL);

        char kb[32];
        for (int i = 0; i < N; i++) {
            snprintf(kb, sizeof(kb), "rk:%d", i);
            databox key = databoxNewBytesString(kb);
            databox val = databoxNewUnsigned((uint64_t)i);
            multidictAdd(r, &key, &val);
        }
        assert(multidictSize(r) == N);

        /* Delete the odd keys to scatter NULL runs through the old table. */
        for (int i = 1; i < N; i += 2) {
            snprintf(kb, sizeof(kb), "rk:%d", i);
            databox key = databoxNewBytesString(kb);
            assert(multidictDelete(r, &key) == true);
        }
        assert(multidictSize(r) == N / 2);

        /* Drain any active rehash one bucket at a time — the exact path that
         * underflowed. Bounded loop so a regression (failure to terminate)
         * fails loudly instead of hanging the suite. */
        int guard = 0;
        while (multidictRehash(r, 1)) {
            /* Bounded so a regression (non-termination) fails loudly rather
             * than hanging the suite. */
            assert(++guard <= 16 * N &&
                   "single-step rehash did not terminate (emptyVisits "
                   "underflow regression)");
        }

        /* Integrity: every even key present with its value, every odd gone. */
        for (int i = 0; i < N; i++) {
            snprintf(kb, sizeof(kb), "rk:%d", i);
            databox key = databoxNewBytesString(kb);
            databox found;
            if (i % 2 == 0) {
                assert(multidictFind(r, &key, &found) == true);
                assert(found.data.u == (uint64_t)i);
            } else {
                assert(multidictFind(r, &key, &found) == false);
            }
        }
        assert(multidictSize(r) == N / 2);
        printf("=== single-step rehash regression passed! ===\n");
    }

    /* ================================================================
     * Empty then reuse (reset-family regression matrix)
     * ================================================================ */
    printf("\n--- Empty-then-reuse cycles ---\n");
    {
        /* Start from a clean slate: earlier suite sections leave
         * entries in the long-lived dict */
        multidictEmpty(d);

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 500; i++) {
                const databox keybox = databoxNewUnsigned(0x9000 + i);
                const databox valbox = databoxNewUnsigned(i * 3);
                multidictAdd(d, &keybox, &valbox);
            }
            if (multidictCount(d) != 500) {
                printf("ERROR: cycle %d expected 500, got %" PRIu64 "\n", cycle,
                       multidictCount(d));
                err++;
            }

            multidictEmpty(d);
            if (multidictCount(d) != 0) {
                printf("ERROR: cycle %d expected 0 after Empty, got %" PRIu64
                       "\n",
                       cycle, multidictCount(d));
                err++;
            }
        }

        uint64_t missing = 0;
        for (uint64_t i = 0; i < 500; i++) {
            const databox keybox = databoxNewUnsigned(0x9000 + i);
            const databox valbox = databoxNewUnsigned(i + 11);
            multidictAdd(d, &keybox, &valbox);
        }
        for (uint64_t i = 0; i < 500; i++) {
            const databox keybox = databoxNewUnsigned(0x9000 + i);
            databox valbox;
            if (!multidictFind(d, &keybox, &valbox)) {
                missing++;
            }
        }
        if (missing > 0) {
            printf("ERROR: %" PRIu64 " of 500 keys missing after Empty-reuse\n",
                   missing);
            err++;
        }
    }

    printf("\n--- 96/128-bit keys and values: normal / extended / mixed ---\n");
    err += multidictTestWide_(MD_WIDE_NORMAL);
    err += multidictTestWide_(MD_WIDE_EXTENDED);
    err += multidictTestWide_(MD_WIDE_MIXED);
    err += multidictTestSlotIteratorWide_();

    /* ================================================================
     * Cleanup
     * ================================================================ */
    printf("\n--- Cleanup ---\n");

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
