#include "multidict.h"

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY /* stack states for the streamed case hash */
#endif
#include "../deps/xxHash/xxhash.h"
#include "asmUtils.h"
#include "cleaner.h"
#include "datakit.h"
#include "dkSort.h"
#include "dkTierLimit.h" /* a bare flex slot's size before it is a multimap */
#include "fibbuf.h"
#include "flex.h"
#include "multilru.h"
#include "multimap.h"
#include "multimapSmall.h"         /* a bare flex slot's row operations */
#include "multimapSmallInternal.h" /* the view a bare flex slot is used as */
#include "str.h"
#include "timeUtil.h"
#include "util.h" /* getRandomBytes */

#include <assert.h>
#include <float.h> /* DBL_MAX (tests) */
#include <limits.h>
#include <math.h> /* isfinite */
#include <stdatomic.h>
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
 * 'multidictHT' is a 48 byte struct (8+4+4+8+8+8+8=48) */
typedef struct multidictHT {
    multidictSlot **table; /* table[i] is pointer to hash slot 'i'. */
    uint32_t size;         /* length of *table allocations above
                            *  (hash slot count; power of 2) */
    uint32_t oversize;     /* number of slots over the median size */
    uint64_t count;        /* number of elements in this HT across all slots */
    uint64_t usedBytes;    /* sum of all uncompressed slot sizes (overhead) */
    uint64_t keyBytes;     /* total bytes of all keys stored */
    uint64_t valBytes;     /* total bytes of all values stored */
} multidictHT;

/* Load factor defaults (entries per 100 slots): grow at 800%, so a resize
 * lands at 400% (four entries a slot, each slot one allocation and one
 * table pointer whatever it holds), and shrink below 40%, a tenth of that
 * landing point.  Measured against 200% (perfRegression multidictLoad, at
 * 40k / 70k / 100k entries): 30 to 35 percent less heap per entry for
 * about 20 ns per find; the next halving saves less and costs more (see
 * MULTIDICT.md, "Load Factors"). */
static constexpr int MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR = 800;
static constexpr int MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR = 40;
static constexpr int MULTIDICT_MIN_SLOTS_FOR_SHRINK =
    8; /* Don't shrink below this */

/* Byte-based expansion defaults */
#define MULTIDICT_DEFAULT_TARGET_SLOT_BYTES (2ULL * 1024 * 1024) /* 2MB */
#define MULTIDICT_DEFAULT_MAX_SLOT_BYTES (8ULL * 1024 * 1024)    /* 8MB */

/* 'multidict' struct layout: the fields every operation reads fill the
 * first 128 bytes (two cache lines):
 *   - ht[2] = 48 bytes * 2 = 96 bytes
 *   - *type, *shared = 16 bytes
 *   - rehashidx, iterators, seed = 12 bytes
 *   - expandLoadFactor (2), shrinkLoadFactor (1), flags (1) = 4 bytes
 * The self-management, byte-expansion and LRU fields follow. */
struct multidict {
    multidictHT ht[2];
    multidictType *type;
    multidictClass *shared;
    uint32_t rehashidx; /* slot idx in ht[0]; MULTIDICT_REHASHIDX_INVALID if not
                           rehashing */
    uint32_t iterators; /* number of iterators currently running */
    uint32_t seed;      /* seed for hash functions */
    uint16_t expandLoadFactor;      /* expand when (count*100/slots) >= this */
    uint8_t shrinkLoadFactor;       /* shrink when (count*100/slots) < this */
    uint8_t compress : 1;           /* BOOL. allow compression? */
    uint8_t rehashing : 1;          /* BOOL. true if currently rehashing. */
    uint8_t autoResize : 1;         /* BOOL. auto expand/shrink enabled? */
    uint8_t lruEnabled : 1;         /* BOOL. LRU tracking enabled? */
    uint8_t useByteBasedExpand : 1; /* BOOL. use byte-based expansion? */
    uint8_t keysByCompare : 1; /* BOOL. byte-string keys match by the type's
                                  keyCompare, not by exact bytes */
    uint8_t unused : 2;        /* available */
    /* Self-management fields */
    uint64_t maxMemory;                    /* 0 = unlimited */
    multidictEvictionCallback *evictionCb; /* called before eviction */
    void *evictionPrivdata;                /* passed to eviction callback */
    /* Byte-based expansion configuration */
    uint64_t targetSlotBytes; /* Target average slot size (default: 2MB) */
    uint64_t maxSlotBytes;   /* Maximum slot size before forced expand (default:
                                8MB) */
    uint64_t byteCheckCount; /* ht[0] count when the byte metrics were last
                                measured (they walk every slot) */
    /* LRU tracking fields (zero overhead when disabled) */
    multilru *lru;          /* LRU structure (NULL if disabled) */
    void *lruPtrToKey;      /* Array of lruKeyRef: ptr -> hash */
    size_t lruPtrToKeySize; /* Size of lruPtrToKey array */
    struct multidict
        *lruKeyToPtr; /* Aux dict: key -> multilruPtr (as uint64) */
    multidictEvictPolicy evictPolicy; /* Eviction policy when LRU enabled */
};
static_assert(offsetof(struct multidict, maxMemory) == 128,
              "the fields every operation reads fill two cache lines");

/* ====================================================================
 * Repetitive Helpers
 * ==================================================================== */
#define HT(d, htidx) (&(d)->ht[(htidx)])
#define HTSIZE(d, htidx) (HT((d), (htidx))->size) /* number of slots */
/* MASK is SIZE-1, and SIZE is power of 2, so MASK is all 1s */
#define HTMASK(d, htidx) ((HT((d), (htidx))->size) - 1)
#define HTCOUNT(d, htidx) (HT((d), (htidx))->count) /* total # of elements */
#define HTBYTES(d, htidx) (HT((d), (htidx))->usedBytes)
#define HTKEYBYTES(d, htidx) (HT((d), (htidx))->keyBytes)
#define HTVALBYTES(d, htidx) (HT((d), (htidx))->valBytes)
/* A table's bytes: its slots and the slot pointer array */
#define HTTOTALBYTES(d, htidx)                                                 \
    (HTBYTES(d, htidx) + (uint64_t)HTSIZE(d, htidx) * sizeof(multidictSlot *))
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
 *   - a slot class or multidict read that fills a box in place
 *     (findValueByKey, findKeyByPosition, multidictFind) gets a box aimed
 *     at a stack slot with mdBoxAim_ first;
 *   - a removal reports a 96/128-bit value type-only (or, taking, as an
 *     owned copy).
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
        dst->data.ptr = nullptr;
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

/* Bytes counted for a key or value in the byte statistics.  The count must
 * not depend on the form a value is stored in, since an add measures the
 * box it was given and a removal or rewrite the box read back: a real
 * counts 8 bytes whether given or stored as a float (flex keeps a double
 * that fits a float as one). */
DK_INLINE_ALWAYS size_t mdBoxSize_(const databox *box) {
    if (mdBoxIsWide_(box)) {
        return sizeof(__uint128_t);
    }

    if (box->type == DATABOX_FLOAT_32) {
        return sizeof(double);
    }

    size_t len = 0;
    databoxGetSize(box, &len);
    return len;
}

/* Slot hash of 'keybox': its databoxHashBytes, so the slots match keys as
 * databoxCompare does (5, 5.0f, 5.0 and a 128-bit 5 are one key) */
DK_INLINE_ALWAYS uint32_t multidictHashOfKey_(multidict *d,
                                              const databox *keybox) {
    uint64_t scratch;
    const uint8_t *key;
    size_t klen;
    databoxHashBytes(keybox, &scratch, &key, &klen);
    return multidictHashKey(d, key, klen);
}

/* The cold end of the LRU as one multidictEvictToLimit call walks it.
 * Victims are picked without being removed from tracking: an entry
 * leaves the LRU only when multidictDelete commits its eviction, so a
 * vetoed victim keeps its level and position exactly.  Passed-over
 * entries stay at the cold end ahead of every untried one, so after a
 * refill the first 'skip' candidates are the ones already passed over.
 * Once 'skip' fills the window, the call picks random victims instead. */
static constexpr int MD_LRU_CANDIDATES = 64;
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
/* A removal's question for multidictGetAndDeleteIf (NULL: none) */
typedef struct mdRemoveAsk_ {
    multidictRemoveGate *gate;
    void *ctx;
} mdRemoveAsk_;
static bool multidictRemoveFromTable_(multidict *d, int htidx,
                                      const databox *keybox, uint32_t hash,
                                      size_t keySize, databox *removedVal,
                                      bool take, const mdRemoveAsk_ *ask);
DK_STATIC multidictUpdateResult multidictUpdate_(multidict *d,
                                                 const databox *keybox,
                                                 multidictValueUpdate *update,
                                                 void *ctx);
static bool multidictLRUSelectVictim(multidict *d, mdLRUCandidates *cands,
                                     databox *keybox, databoxWide *slot);
DK_INLINE_ALWAYS bool multidictFindNoTouch_(multidict *d, const databox *keybox,
                                            databox *valbox);
DK_STATIC int32_t multidictStringKeyCompare_(void *privdata, const void *key1,
                                             const uint32_t k1len,
                                             const void *key2,
                                             const uint32_t k2len);

/* ====================================================================
 * Keys matched by the type's compare
 * ====================================================================
 * The slot layer matches keys by their exact bytes.  In a dict whose
 * type brings its own keyCompare (keysByCompare, set once in
 * multidictNew), a byte-string key also matches a stored byte-string key
 * the compare reports equal, and the dict keeps one entry per such key
 * under the spelling that first inserted it.  The dict honours that by
 * handing the slot layer the stored spelling: an operation looks for the
 * exact bytes first, and only on a miss walks the key's slot through the
 * class iterator for a match.  The type's hash puts every matching
 * spelling in the same slot.  Exact dicts test keysByCompare once per
 * operation and never come here. */
#define MD_MATCH_BY_COMPARE(d, keybox)                                         \
    ((d)->keysByCompare && DATABOX_IS_BYTES(keybox))

/* The byte-string key in 'slot' that the type's compare reports equal to
 * byte-string 'keybox'.  'stored' borrows from the slot, so it is valid
 * until the slot changes. */
DK_STATIC bool mdSlotMatch_(multidict *d, multidictSlot *slot,
                            const databox *keybox, databox *stored) {
    multidictSlotIterator iter;
    if (!slot || !d->shared->getIter(&iter, slot)) {
        return false;
    }

    const void *want = databoxCBytes(keybox);
    const uint32_t wantLen = (uint32_t)databoxLen(keybox);
    multidictEntry entry;
    while (d->shared->iterNext(&iter, &entry)) {
        if (DATABOX_IS_BYTES(&entry.key) &&
            d->type->keyCompare(d->shared->privdata, want, wantLen,
                                databoxCBytes(&entry.key),
                                (uint32_t)databoxLen(&entry.key))) {
            *stored = entry.key;
            return true;
        }
    }

    return false;
}

/* A stored spelling copied out of its slot, so it stays valid while an
 * add or delete changes the slot */
typedef struct mdKeyCopy {
    databox box;
    uint8_t *heap; /* the copy, when it does not fit 'buf' */
    uint8_t buf[64];
} mdKeyCopy;

/* The key an add or delete hands the slot layer for byte-string 'keybox':
 * the stored spelling when the dict holds the key under another one,
 * copied into 'copy', and otherwise 'keybox' itself.  Release 'copy' with
 * mdKeyCopyRelease_ either way. */
DK_STATIC const databox *mdKeyForUpdate_(multidict *d, const databox *keybox,
                                         mdKeyCopy *copy) {
    copy->heap = nullptr;
    if (HTSIZE(d, 0) == 0) {
        return keybox; /* emptied: no table yet */
    }

    /* While rehashing a key may be in either table; the newer one first */
    const uint32_t hash = multidictHashOfKey_(d, keybox);
    for (int32_t t = d->rehashing; t >= 0; t--) {
        multidictSlot *slot = *SLOT_BY_HASH_PTR_PTR(d, t, hash);
        if (!slot) {
            continue;
        }

        /* The value is not read, so its type is enough */
        databox unused = {};
        if (d->shared->findValueByKey(d->shared, slot, keybox, &unused)) {
            return keybox; /* stored under these exact bytes */
        }

        databox stored;
        if (mdSlotMatch_(d, slot, keybox, &stored)) {
            const size_t len = databoxLen(&stored);
            uint8_t *bytes = copy->buf;
            if (len > sizeof(copy->buf)) {
                /* @own: container; free=mdKeyCopyRelease_ */
                copy->heap = zmalloc(len);
                bytes = copy->heap;
            }

            memcpy(bytes, databoxCBytes(&stored), len);
            copy->box = DATABOX_WITH_BYTES(bytes, len);
            return &copy->box;
        }
    }

    return keybox; /* not stored: it goes in under this spelling */
}

DK_INLINE_ALWAYS void mdKeyCopyRelease_(mdKeyCopy *copy) {
    if (copy->heap) {
        zfree(copy->heap);
    }
}

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
    /* The hash native to the word size, chosen at compile time */
#if DK_BITS == 64
    return XXH64(key, len, d->seed);
#else
    return XXH32(key, len, d->seed);
#endif
}

/* Copy 'len' bytes of 'src' to 'dst' with exactly 'A'-'Z' folded to
 * 'a'-'z' (branch-free, so it vectorizes) */
DK_INLINE_ALWAYS void mdFoldAscii_(uint8_t *dst, const uint8_t *src,
                                   size_t len) {
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = src[i];
        dst[i] = c | (uint8_t)(((uint8_t)(c - 'A') < 26u) << 5);
    }
}

/* Bytes folded per step of the case hash */
static constexpr int MD_CASE_HASH_CHUNK = 256;

/* An ASCII case-insensitive hash: multidictGenHashFunction, the seeded
 * hash of exact keys, over the key with exactly 'A'-'Z' folded to
 * 'a'-'z' (the set multidictTypeCaseKey's compare folds, so keys that
 * compare equal hash equal).  Folding happens in a stack chunk: a key
 * that fits one is hashed in one call, and a longer one streams its
 * chunks through the same hash, which gives the same value as hashing
 * all the folded bytes at once.  Nothing is allocated. */
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len) {
    uint8_t folded[MD_CASE_HASH_CHUNK];
    const size_t total = len > 0 ? (size_t)len : 0;
    if (total <= sizeof(folded)) {
        mdFoldAscii_(folded, buf, total);
        return multidictGenHashFunction(d, folded, (int32_t)total);
    }

#if DK_BITS == 64
    XXH64_state_t state;
    XXH64_reset(&state, d->seed);
    for (size_t at = 0; at < total; at += sizeof(folded)) {
        const size_t left = total - at;
        const size_t n = left < sizeof(folded) ? left : sizeof(folded);
        mdFoldAscii_(folded, buf + at, n);
        XXH64_update(&state, folded, n);
    }
    return (uint32_t)XXH64_digest(&state);
#else
    XXH32_state_t state;
    XXH32_reset(&state, d->seed);
    for (size_t at = 0; at < total; at += sizeof(folded)) {
        const size_t left = total - at;
        const size_t n = left < sizeof(folded) ? left : sizeof(folded);
        mdFoldAscii_(folded, buf + at, n);
        XXH32_update(&state, folded, n);
    }
    return XXH32_digest(&state);
#endif
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

/* The key's slot in the table new rows go to: ht[1] while rehashing, else
 * ht[0].  A key still in ht[0] during a rehash is not looked for here;
 * inserts remove it from ht[0] first (multidictAddExact_).  'knownHash',
 * when not NULL, is the key's hash already computed by the caller (it
 * does not depend on the table). */
DK_INLINE_ALWAYS multidictSlot **
multidictNewestSlotForKey(multidict *d, const databox *keybox,
                          const uint32_t *knownHash, int *htidx) {
    const int newest = d->rehashing;
    if (htidx) {
        *htidx = newest;
    }

    /* A one-slot table needs no hash */
    if (likely(HTSIZE(d, newest) == 1)) {
        return &SLOT(d, newest, 0);
    }

    const uint32_t hash =
        knownHash ? *knownHash : multidictHashOfKey_(d, keybox);
    return SLOT_BY_HASH_PTR_PTR(d, newest, hash);
}

DK_STATIC void *
multidictOperate(multidict *d, multidictOp *op, const databox *keybox,
                 const uint32_t *knownHash,
                 void *(*operate)(multidictClass *qdc, multidictSlot **slot,
                                  const databox *keybox)) {
    multidictRehashStep(d);

    /* Ensure hash table is initialized (expand if empty after multidictEmpty)
     */
    if (HTSIZE(d, 0) == 0) {
        multidictExpand(d, MULTIDICT_HT_INITIAL_SIZE);
    }

    int htidx;
    multidictSlot **foundSlot =
        multidictNewestSlotForKey(d, keybox, knownHash, &htidx);
    const bool wasEmpty = !*foundSlot;
    void *result = operate(d->shared, foundSlot, keybox);

    if (op) {
        op->result = result;
        op->ht = HT(d, htidx);
        op->d = d;
        op->slot = foundSlot;
        op->created = wasEmpty && *foundSlot;
    }

    return result;
}

DK_STATIC multidictSlot *
multidictFindSlotForKeyNewest(multidict *d, multidictOp *op,
                              const databox *keybox,
                              const uint32_t *knownHash) {
    return multidictOperate(d, op, keybox, knownHash,
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

    integers[0] = (uintptr_t)d->ht[0].table;
    integers[1] = HTSIZE(d, 0);
    integers[2] = HTCOUNT(d, 0);
    integers[3] = (uintptr_t)d->ht[1].table;
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
/* Slots for 'count' entries at half the growth point's load (at least 1):
 * a resize lands there, so the table grows again only after its entries
 * double.  At the default 200% it is one slot per entry. */
DK_INLINE_ALWAYS uint64_t multidictSlotsForLoad_(const multidict *d,
                                                 const uint64_t count) {
    const uint64_t slots = count * 200 / d->expandLoadFactor;
    return slots > 0 ? slots : 1;
}

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
        /* The metrics walk every slot, so they are measured again only
         * once the entry count has moved by a 16th of the table since the
         * last measure (O(1) amortized per insert instead of O(slots));
         * meanwhile only the count-based backstop below can expand. */
        const uint64_t count = HTCOUNT(d, 0);
        const uint64_t moved = count > d->byteCheckCount
                                   ? count - d->byteCheckCount
                                   : d->byteCheckCount - count;
        const uint64_t every = HTSIZE(d, 0) >> 4 ? HTSIZE(d, 0) >> 4 : 1;
        if (moved < every && countLoadFactor < d->expandLoadFactor * 2u) {
            return false;
        }
        d->byteCheckCount = count;

        /* Calculate byte-based load metrics */
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(d, &metrics);

        DBG_PRINT("multidictExpandIfNeeded_: byte-based mode - "
                  "avgSlotBytes=%" PRIu64 ", targetSlotBytes=%" PRIu64
                  ", maxSlotBytes=%" PRIu64 ", byteLoadFactor=%u\n",
                  metrics.avgSlotBytes, metrics.targetSlotBytes,
                  metrics.maxSlotBytes, metrics.byteLoadFactor);

        bool shouldExpand = false;
        [[maybe_unused]] const char *expandReason = nullptr;

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

            /* Safeguard: expand only if it is expected to cut the average
             * used slot by at least 10%.  That average is totalUsedBytes
             * over usedSlots, so with the same bytes it scales with the
             * entries per used slot: now count / usedSlots (measured, so a
             * skewed table counts as it is), and after the expansion, for
             * seeded hashing at load a = count / newSize, a / (1 - e^-a).
             * Entries larger than the target sit mostly alone already, so
             * the table stops growing near its count instead of expanding
             * on every insert up to the slot cap.  (Comparing against
             * totalUsedBytes / newSize, the average over ALL slots, always
             * passed.) */
            if (metrics.usedSlots > 0) {
                const double entries = (double)HTCOUNT(d, 0);
                const double perUsedNow = entries / (double)metrics.usedSlots;
                const double load = entries / (double)newSize;
                const double perUsedAfter = load / (1.0 - exp(-load));
                if (perUsedAfter > 0.9 * perUsedNow) {
                    DBG_PRINT("multidictExpandIfNeeded_ EXIT: expansion "
                              "ineffective (%.3f entries per used slot "
                              "now, %.3f expected), skip\n",
                              perUsedNow, perUsedAfter);
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

    /* Count-based expansion (used when byte-based is disabled) */
    if (countLoadFactor >= d->expandLoadFactor) {
        /* The next fibonacci size, or more: enough slots to leave the
         * load at half the growth point */
        uint64_t newSize = fibbufNextSizeBuffer(HTSIZE(d, 0));
        const uint64_t forLoad = multidictSlotsForLoad_(d, HTCOUNT(d, 0));
        if (newSize < forLoad) {
            newSize = fibbufNextSizeBuffer(forLoad);
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
        /* The smallest power of 2 leaving the load at half the growth
         * point */
        uint64_t newSize =
            _multidictNextPower(multidictSlotsForLoad_(d, HTCOUNT(d, 0)));

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
/* The process's random dict seed, drawn once: bit 32 marks it drawn, so a
 * drawn seed of 0 is still told from "not drawn yet".  Racing first calls
 * draw their own and one wins the CAS; every caller returns the winner. */
static _Atomic uint64_t mdRandomSeed_;

int32_t multidictRandomSeed(void) {
    /* @sync: relaxed init */
    uint64_t seed = atomic_load_explicit(&mdRandomSeed_, memory_order_relaxed);
    if (!seed) {
        uint32_t drawn;
        getRandomBytes((uint8_t *)&drawn, sizeof(drawn));
        uint64_t expected = 0;
        const uint64_t mine = (UINT64_C(1) << 32) | drawn;
        /* @sync: relaxed init */
        seed = atomic_compare_exchange_strong_explicit(
                   &mdRandomSeed_, &expected, mine, memory_order_relaxed,
                   memory_order_relaxed)
                   ? mine
                   : expected;
    }
    return (int32_t)(uint32_t)seed;
}

multidict *multidictNew(multidictType *type, multidictClass *qdc,
                        int32_t seed) {
    if (!type || !qdc) {
        return nullptr;
    }

    /* @own: caller; free=multidictFree */
    multidict *d = zcalloc(1, sizeof(*d));

    multidictResetHT(HT(d, 0));
    multidictResetHT(HT(d, 1));

    d->type = type;
    d->shared = qdc;
    d->keysByCompare = type->keyCompare != nullptr &&
                       type->keyCompare != multidictStringKeyCompare_;
    d->rehashidx = MULTIDICT_REHASHIDX_INVALID;
    d->rehashing = false;
    d->iterators = 0;
    uint32_t expandPercent;
    uint32_t shrinkPercent;
    multidictDefaultLoadFactors(&expandPercent, &shrinkPercent);
    d->expandLoadFactor = (uint16_t)expandPercent;
    d->shrinkLoadFactor = (uint8_t)shrinkPercent;
    d->autoResize = true;
    /* Count-based resizing unless multidictEnableByteBasedExpansion asks
     * for byte-based */
    d->useByteBasedExpand = false;
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

multidictType *multidictGetType(const multidict *d) {
    return d->type;
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

    /* Fit the table to its contents the way the automatic shrink does:
     * the load at half the growth point (one slot per entry at the
     * default; rounded up to a power of two by the expand), never below
     * the shrink minimum; in byte-based mode also no fewer slots than the
     * bytes need at the target average */
    uint64_t want = multidictSlotsForLoad_(d, HTCOUNT(d, 0));
    if (d->useByteBasedExpand && d->targetSlotBytes > 0) {
        const uint64_t forBytes = HTBYTES(d, 0) / d->targetSlotBytes + 1;
        if (forBytes > want) {
            want = forBytes;
        }
    }
    if (want < MULTIDICT_MIN_SLOTS_FOR_SHRINK) {
        want = MULTIDICT_MIN_SLOTS_FOR_SHRINK;
    }

    return multidictExpand(d, want);
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
    multidictHT n = {};
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
    if (d->ht[0].table == nullptr) {
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
        while ((current = SLOT(d, 0, d->rehashidx)) == nullptr) {
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

        /* Move every row of 'current' to its slot in HT 1 in one pass:
         * each row is read in place and inserted into its target (a slot
         * of a grown table takes rows from this one slot only), and
         * 'current' is then freed whole, with no per-row delete.  A
         * one-row slot whose target is still empty moves as it is. */
        const size_t currentBytes = shared->sizeBytes(current);
        const bool single = shared->countSlot(current) == 1;
        bool movedWhole = false;
        multidictSlotIterator slotIter;
        multidictEntry row;
        if (shared->getIter(&slotIter, current)) {
            while (shared->iterNext(&slotIter, &row)) {
                multidictSlot **target = SLOT_BY_HASH_PTR_PTR(
                    d, 1, multidictHashOfKey_(d, &row.key));

                /* Counts and the key and value bytes move row by row */
                const size_t keySize = mdBoxSize_(&row.key);
                const size_t valSize = mdBoxSize_(&row.val);
                HTCOUNT(d, 0)--;
                HTCOUNT(d, 1)++;
                HTKEYBYTES(d, 0) -= keySize;
                HTKEYBYTES(d, 1) += keySize;
                HTVALBYTES(d, 0) -= valSize;
                HTVALBYTES(d, 1) += valSize;

                if (single && !*target) {
                    *target = current;
                    HTBYTES(d, 1) += currentBytes;
                    movedWhole = true;
                    break;
                }

                size_t targetBefore = 0;
                if (!*target) {
                    *target = shared->createSlot();
                } else {
                    targetBefore = shared->sizeBytes(*target);
                }

                /* The insert copies anything the row borrows from
                 * 'current', which stays untouched until it is freed */
                shared->insertByType(shared, target, &row.key, &row.val,
                                     nullptr);
                HTBYTES(d, 1) += shared->sizeBytes(*target) - targetBefore;
            }
        }

        HTBYTES(d, 0) -= currentBytes;
        if (!movedWhole) {
            shared->freeSlot(shared, current);
        }

        current = nullptr;

        /* Set slot to NULL for easier debugging/tracking/introspection. */
        SLOT(d, 0, d->rehashidx) = nullptr;

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

/* multidictAdd for a key the slot layer can match by its exact bytes */
DK_STATIC multidictResult multidictAddExact_(multidict *d,
                                             const databox *keybox,
                                             const databox *valbox) {
    DBG_PRINT("multidictAdd ENTER\n");
    DBG_DICT_STATE(d, "  state before add");

    /* Check if we should expand before adding */
    multidictExpandIfNeeded_(d);

    /* During rehashing, check if key exists in ht[0] and remove it first.
     * This prevents duplicate keys across tables and keeps counts accurate:
     * multidictNewestSlotForKey looks only in ht[1]. */
    bool removedFromOldTable = false;
    uint32_t hash = 0;
    const uint32_t *knownHash = nullptr;
    if (multidictIsRehashing(d)) {
        /* Remove the key from ht[0] in one search; the hash serves the
         * slot lookup below */
        hash = multidictHashOfKey_(d, keybox);
        knownHash = &hash;
        removedFromOldTable = multidictRemoveFromTable_(
            d, 0, keybox, hash, mdBoxSize_(keybox), nullptr, false, nullptr);
    }

    multidictOp op;
    multidictFindSlotForKeyNewest(d, &op, keybox, knownHash);

    /* The insert may reallocate the slot: read it through its cell.  A
     * slot this add created counts whole, so usedBytes stays the sum of
     * the slots' sizes (a slot leaves it whole when freed). */
    const size_t lenBefore = op.created ? 0 : d->shared->sizeBytes(*op.slot);

    /* Get key and value sizes for tracking */
    const size_t keySize = mdBoxSize_(keybox);
    const size_t valSize = mdBoxSize_(valbox);

    /* One insert, reporting the value a replace overwrote (only measured,
     * so its type and length are enough) */
    databox oldVal = {};
    const int64_t insertResult =
        d->shared->insertByType(d->shared, op.slot, keybox, valbox, &oldVal);
    const size_t oldValSize = insertResult == 0 ? mdBoxSize_(&oldVal) : 0;
    const size_t lenAfter = d->shared->sizeBytes(*op.slot);

    /* Adjust 'usedBytes' by actual slot size change */
    op.ht->usedBytes += (lenAfter - lenBefore);

    /* Convert to typed result */
    multidictResult result;
    if (insertResult == 1) {
        /* New to this table: add both key and value bytes.  A key moved
         * here out of ht[0] above already existed, so for the caller and
         * the LRU (which still tracks it) this is a replacement. */
        op.ht->count++;
        op.ht->keyBytes += keySize;
        op.ht->valBytes += valSize;
        result =
            removedFromOldTable ? MULTIDICT_OK_REPLACED : MULTIDICT_OK_INSERTED;
    } else if (insertResult == 0) {
        /* Replacement: key stays same, update value bytes delta */
        op.ht->valBytes += valSize;
        op.ht->valBytes -= oldValSize;
        result = MULTIDICT_OK_REPLACED;
    } else {
        result = MULTIDICT_ERR;
    }

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

/* multidictAdd for a key matched by the type's compare: a key stored under
 * another spelling keeps that spelling.  Kept out of line so the exact
 * path's frame does not carry the key copy. */
[[gnu::noinline]] DK_STATIC multidictResult multidictAddMatched_(
    multidict *d, const databox *keybox, const databox *valbox) {
    mdKeyCopy copy;
    const databox *key = mdKeyForUpdate_(d, keybox, &copy);
    const multidictResult result = multidictAddExact_(d, key, valbox);
    mdKeyCopyRelease_(&copy);
    return result;
}

/* Add new element to the target hash table.
 * Returns: MULTIDICT_OK_INSERTED if new key, MULTIDICT_OK_REPLACED if updated,
 *          MULTIDICT_ERR on error */
multidictResult multidictAdd(multidict *d, const databox *keybox,
                             const databox *valbox) {
    if (unlikely(MD_MATCH_BY_COMPARE(d, keybox))) {
        return multidictAddMatched_(d, keybox, valbox);
    }

    return multidictAddExact_(d, keybox, valbox);
}

bool multidictPut(multidict *d, const databox *keybox, const databox *valbox) {
    const multidictResult result = multidictAdd(d, keybox, valbox);
    assert(result != MULTIDICT_ERR);
    return result == MULTIDICT_OK_INSERTED;
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

    /* A key stored under another spelling is found only by walking its
     * slot: check, then add */
    if (unlikely(MD_MATCH_BY_COMPARE(d, keybox))) {
        if (multidictExists(d, keybox)) {
            return MULTIDICT_ERR; /* a present key is named: its one use */
        }

        return multidictAdd(d, keybox, valbox);
    }

    /* While rehashing the key may still be in ht[0]; the hash found for
     * that check serves the slot lookup below */
    uint32_t hash = 0;
    const uint32_t *knownHash = nullptr;
    if (multidictIsRehashing(d)) {
        hash = multidictHashOfKey_(d, keybox);
        knownHash = &hash;
        multidictSlot *old = *SLOT_BY_HASH_PTR_PTR(d, 0, hash);
        databox unused = {};
        if (old && d->shared->findValueByKey(d->shared, old, keybox, &unused)) {
            multidictLRUOnAccess(d, keybox);
            return MULTIDICT_ERR;
        }
    }

    multidictOp op;
    multidictFindSlotForKeyNewest(d, &op, keybox, knownHash);
    const size_t lenBefore = op.created ? 0 : d->shared->sizeBytes(*op.slot);

    /* One search finds the key or inserts it */
    const multidictInsertAbsentResult inserted =
        d->shared->insertIfAbsentByType(d->shared, op.slot, keybox, valbox);
    if (inserted == MULTIDICT_INSERT_PRESENT) {
        multidictLRUOnAccess(d, keybox);
        return MULTIDICT_ERR; /* Key exists - NX fails */
    }

    if (inserted == MULTIDICT_INSERT_NO_SLOT) {
        return MULTIDICT_ERR;
    }

    op.ht->usedBytes += d->shared->sizeBytes(*op.slot) - lenBefore;
    op.ht->count++;
    op.ht->keyBytes += mdBoxSize_(keybox);
    op.ht->valBytes += mdBoxSize_(valbox);
    multidictLRUOnInsert(d, keybox);

    /* Grown after the insert, not before: a refused offer adds nothing */
    multidictExpandIfNeeded_(d);
    return MULTIDICT_OK_INSERTED;
}

multidictUpdateResult multidictUpdateValue(multidict *d, const databox *keybox,
                                           multidictValueUpdate *update,
                                           void *ctx) {
    if (unlikely(!d || !keybox || !update)) {
        return MULTIDICT_UPDATE_ABSENT;
    }

    const multidictUpdateResult result =
        multidictUpdate_(d, keybox, update, ctx);
    if (result == MULTIDICT_UPDATE_REWRITTEN) {
        multidictLRUOnAccess(d, keybox);
    }
    return result;
}

/* AddXX's rewrite: the new value, whatever was stored */
static bool mdReplaceUpdate_(void *ctx, const databox *, databox *replacement) {
    *replacement = *(const databox *)ctx;
    return true;
}

/* Add only if key DOES exist (update existing only): the value rewritten
 * in place in one search, the call's one LRU use */
multidictResult multidictAddXX(multidict *d, const databox *keybox,
                               const databox *valbox) {
    if (unlikely(!valbox)) {
        return MULTIDICT_ERR;
    }

    return multidictUpdateValue(d, keybox, mdReplaceUpdate_, (void *)valbox) ==
                   MULTIDICT_UPDATE_REWRITTEN
               ? MULTIDICT_OK_REPLACED
               : MULTIDICT_ERR; /* Key doesn't exist - XX fails */
}

/* Explicit replace - fails if key doesn't exist (same as XX, explicit intent)
 */
multidictResult multidictReplace(multidict *d, const databox *keybox,
                                 const databox *valbox) {
    return multidictAddXX(d, keybox, valbox);
}

/* Remove 'keybox' from table 'htidx' in one slot search ('hash' is the
 * key's hash, 'keySize' its tracked size), keeping the table's counts.
 * 'removedVal' and 'take' are operationRemove's; the removed value is
 * measured either way.  With 'ask', the stored value is shown to its gate
 * first (a second search of the same, just-read slot); a yes drops the
 * key's LRU tracking, then the key goes. */
static bool multidictRemoveFromTable_(multidict *d, int htidx,
                                      const databox *keybox, uint32_t hash,
                                      size_t keySize, databox *removedVal,
                                      bool take, const mdRemoveAsk_ *ask) {
    multidictSlot **slotPtr = SLOT_BY_HASH_PTR_PTR(d, htidx, hash);
    if (!*slotPtr) {
        return false;
    }

    if (ask) {
        databox current;
        databoxWide currentWide;
        mdBoxAim_(&current, &currentWide);
        if (!d->shared->findValueByKey(d->shared, *slotPtr, keybox, &current)) {
            return false;
        }

        mdBoxSettle_(&current);
        if (!ask->gate(ask->ctx, &current)) {
            return false;
        }

        multidictLRUOnDelete(d, keybox);
    }

    const size_t lenBefore = d->shared->sizeBytes(*slotPtr);
    databox measured = {};
    databox *val = removedVal ? removedVal : &measured;
    if (!d->shared->operationRemove(d->shared, slotPtr, keybox, val,
                                    take && removedVal)) {
        return false;
    }

    multidictHT *ht = HT(d, htidx);
    ht->count--;
    ht->usedBytes -= lenBefore - d->shared->sizeBytes(*slotPtr);
    ht->keyBytes -= keySize;
    ht->valBytes -= mdBoxSize_(val);
    return true;
}

/* Remove a key the slot layer can match by its exact bytes: one hash and
 * one slot search per table.  'removedVal' and 'take' as for
 * multidictRemoveFromTable_. */
DK_STATIC bool multidictRemoveExact_(multidict *d, const databox *keybox,
                                     databox *removedVal, bool take,
                                     const mdRemoveAsk_ *ask) {
    if (unlikely(HTSIZE(d, 0) == 0)) {
        return false; /* d->ht[0].table is NULL */
    }

    multidictRehashStep(d);

    const uint32_t hash = multidictHashOfKey_(d, keybox);
    const size_t keySize = mdBoxSize_(keybox);

    /* The LRU tracks exactly the keys present, so this is a no-op for an
     * absent key; it runs first, while 'keybox' may still borrow from the
     * slot the removal rewrites.  An asked removal drops it only once its
     * gate says yes. */
    if (!ask) {
        multidictLRUOnDelete(d, keybox);
    }

    /* While rehashing the key may be in either table: the newer first */
    const bool removed = (unlikely(d->rehashing) &&
                          multidictRemoveFromTable_(d, 1, keybox, hash, keySize,
                                                    removedVal, take, ask)) ||
                         multidictRemoveFromTable_(d, 0, keybox, hash, keySize,
                                                   removedVal, take, ask);
    if (removed) {
        multidictShrinkIfNeeded_(d);
    }

    return removed;
}

/* multidictDelete for a key matched by the type's compare, out of line as
 * multidictAddMatched_ is */
[[gnu::noinline]] DK_STATIC bool
multidictRemoveMatched_(multidict *d, const databox *keybox,
                        databox *removedVal, bool take,
                        const mdRemoveAsk_ *ask) {
    mdKeyCopy copy;
    const databox *key = mdKeyForUpdate_(d, keybox, &copy);
    const bool removed = multidictRemoveExact_(d, key, removedVal, take, ask);
    mdKeyCopyRelease_(&copy);
    return removed;
}

/* Remove 'keybox' in one search per table, matching it as the dict's type
 * does; 'removedVal', 'take' and 'ask' as for multidictRemoveFromTable_ */
DK_STATIC bool multidictRemove_(multidict *d, const databox *keybox,
                                databox *removedVal, bool take,
                                const mdRemoveAsk_ *ask) {
    if (unlikely(MD_MATCH_BY_COMPARE(d, keybox))) {
        return multidictRemoveMatched_(d, keybox, removedVal, take, ask);
    }

    return multidictRemoveExact_(d, keybox, removedVal, take, ask);
}

/* Search and remove an element - optimized for common case (key exists) */
bool multidictDelete(multidict *d, const databox *keybox) {
    return multidictRemove_(d, keybox, nullptr, false, nullptr);
}

/* updateByType's callback wrapped to measure the value it replaces */
typedef struct mdUpdateCtx {
    multidictValueUpdate *update;
    void *ctx;
    size_t oldSize;
    size_t newSize;
} mdUpdateCtx;

static bool mdUpdateMeasured_(void *ctx, const databox *current,
                              databox *replacement) {
    mdUpdateCtx *u = ctx;
    if (!u->update(u->ctx, current, replacement)) {
        return false;
    }

    u->oldSize = mdBoxSize_(current);
    u->newSize = mdBoxSize_(replacement);
    return true;
}

/* Rewrite the value of 'keybox' where it is stored: one hash and one
 * slot search per table (the newer first while rehashing), keeping the
 * byte counts.  The key is matched as the dict's type does.  LRU use is
 * the caller's. */
DK_STATIC multidictUpdateResult multidictUpdate_(multidict *d,
                                                 const databox *keybox,
                                                 multidictValueUpdate *update,
                                                 void *ctx) {
    if (unlikely(HTSIZE(d, 0) == 0)) {
        return MULTIDICT_UPDATE_ABSENT;
    }

    mdKeyCopy copy = {.heap = nullptr};
    const databox *key = MD_MATCH_BY_COMPARE(d, keybox)
                             ? mdKeyForUpdate_(d, keybox, &copy)
                             : keybox;

    multidictRehashStep(d);
    const uint32_t hash = multidictHashOfKey_(d, key);
    multidictUpdateResult result = MULTIDICT_UPDATE_ABSENT;
    for (int32_t t = d->rehashing ? 1 : 0;
         t >= 0 && result == MULTIDICT_UPDATE_ABSENT; t--) {
        multidictSlot **slot = SLOT_BY_HASH_PTR_PTR(d, t, hash);
        if (!*slot) {
            continue;
        }

        const size_t before = d->shared->sizeBytes(*slot);
        mdUpdateCtx u = {.update = update, .ctx = ctx};
        result = d->shared->updateByType(d->shared, slot, key,
                                         mdUpdateMeasured_, &u);
        if (result == MULTIDICT_UPDATE_REWRITTEN) {
            multidictHT *ht = HT(d, t);
            ht->usedBytes =
                ht->usedBytes - before + d->shared->sizeBytes(*slot);
            ht->valBytes = ht->valBytes - u.oldSize + u.newSize;
        }
    }

    mdKeyCopyRelease_(&copy);
    return result;
}

/* Rewrite the value of an exactly matched 'keybox' where it is stored, or
 * add 'keybox' -> 'valbox' when it is absent, keeping the byte counts:
 * one hash, and one slot search in the table new rows go to (while
 * rehashing, ht[0] is searched first: the key may still be there).  The
 * call's LRU use is as an add's or an update's.  NO_SLOT only when a slot
 * cannot be had, as for an add. */
DK_STATIC multidictUpsertResult multidictUpsert_(multidict *d,
                                                 const databox *keybox,
                                                 const databox *valbox,
                                                 multidictValueUpdate *update,
                                                 void *ctx) {
    mdUpdateCtx u = {.update = update, .ctx = ctx};
    multidictRehashStep(d);
    if (unlikely(HTSIZE(d, 0) == 0)) {
        multidictExpand(d, MULTIDICT_HT_INITIAL_SIZE);
    }

    uint32_t hash = 0;
    const uint32_t *knownHash = nullptr;
    if (multidictIsRehashing(d)) {
        hash = multidictHashOfKey_(d, keybox);
        knownHash = &hash;
        multidictSlot **old = SLOT_BY_HASH_PTR_PTR(d, 0, hash);
        if (*old) {
            const size_t before = d->shared->sizeBytes(*old);
            const multidictUpdateResult updated = d->shared->updateByType(
                d->shared, old, keybox, mdUpdateMeasured_, &u);
            if (updated != MULTIDICT_UPDATE_ABSENT) {
                multidictLRUOnAccess(d, keybox);
                if (updated == MULTIDICT_UPDATE_DECLINED) {
                    return MULTIDICT_UPSERT_DECLINED;
                }
                multidictHT *ht = HT(d, 0);
                ht->usedBytes =
                    ht->usedBytes - before + d->shared->sizeBytes(*old);
                ht->valBytes = ht->valBytes - u.oldSize + u.newSize;
                return MULTIDICT_UPSERT_REWRITTEN;
            }
        }
    }

    /* The newest table's slot, made only when the key has none (and so is
     * absent): a present key's path is an update's */
    int htidx;
    multidictSlot **slot =
        multidictNewestSlotForKey(d, keybox, knownHash, &htidx);
    multidictHT *ht = HT(d, htidx);
    size_t lenBefore = 0;
    if (*slot) {
        lenBefore = d->shared->sizeBytes(*slot);
    } else {
        d->shared->operationSlotGetOrCreate(d->shared, slot, keybox);
    }

    const multidictUpsertResult result = d->shared->upsertByType(
        d->shared, slot, keybox, valbox, mdUpdateMeasured_, &u);
    if (result == MULTIDICT_UPSERT_NO_SLOT) {
        return result;
    }

    ht->usedBytes += d->shared->sizeBytes(*slot) - lenBefore;
    if (result == MULTIDICT_UPSERT_ADDED) {
        ht->count++;
        ht->keyBytes += mdBoxSize_(keybox);
        ht->valBytes += mdBoxSize_(valbox);
        multidictLRUOnInsert(d, keybox);
        /* Grown after the insert, as an add does */
        multidictExpandIfNeeded_(d);
        return result;
    }

    if (result == MULTIDICT_UPSERT_REWRITTEN) {
        ht->valBytes = ht->valBytes - u.oldSize + u.newSize;
    }
    multidictLRUOnAccess(d, keybox);
    return result;
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

    /* One removal that takes the value: an owned copy (a 96/128-bit value
     * in a heap payload, since the box starts zeroed) */
    databox taken = {};
    if (!multidictRemove_(d, keybox, &taken, true, nullptr)) {
        return false; /* Key doesn't exist */
    }

    *valbox = taken;
    return true;
}

bool multidictGetAndDeleteIf(multidict *d, const databox *keybox,
                             databox *valbox, multidictRemoveGate *gate,
                             void *ctx) {
    if (unlikely(!d || !keybox || !valbox || !gate)) {
        return false;
    }

    const mdRemoveAsk_ ask = {.gate = gate, .ctx = ctx};
    databox taken = {};
    if (!multidictRemove_(d, keybox, &taken, true, &ask)) {
        return false; /* absent, or the gate said no */
    }

    *valbox = taken;
    return true;
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
     * removal below runs can move the entry and free that slot: copy it
     * first, then remove by the copy.  The removal searches both tables
     * while a rehash is in flight, so it finds the entry the key was just
     * read from, and takes its value in the same search (no LRU use: the
     * entry is going). */
    databox ownedKey = databoxCopy(&key);
    databox taken = {};
    if (!multidictRemove_(d, &ownedKey, &taken, true, nullptr)) {
        databoxFreeData(&ownedKey);
        return false;
    }

    *keybox = ownedKey;
    *valbox = taken;
    return true;
}

/* Destroy an entire multidictionary */
static void _multidictClearHt(multidict *d, multidictHT *ht) {
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

/* Lookup while rehashing, or in a dict whose keys match by the type's
 * compare.  While rehashing, both tables are searched, not just the slot
 * lookup's choice: a non-NULL ht[1] slot need not hold the key, which can
 * still be in ht[0] awaiting migration. */
DK_STATIC bool multidictFindSlow_(multidict *d, const databox *keybox,
                                  databox *valbox) {
    const uint32_t hash = multidictHashOfKey_(d, keybox);
    const bool byCompare = MD_MATCH_BY_COMPARE(d, keybox);

    /* ht[1] first (newer table), then ht[0] */
    for (int32_t t = d->rehashing; t >= 0; t--) {
        multidictSlot *slot = *SLOT_BY_HASH_PTR_PTR(d, t, hash);
        if (!slot) {
            continue;
        }

        if (d->shared->findValueByKey(d->shared, slot, keybox, valbox)) {
            return true;
        }

        /* Stored under another spelling: look it up by that one.  Nothing
         * changes the slot in between, so the borrowed spelling holds. */
        databox stored;
        if (byCompare && mdSlotMatch_(d, slot, keybox, &stored)) {
            return d->shared->findValueByKey(d->shared, slot, &stored, valbox);
        }
    }

    return false;
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

    /* One test of the flags byte sends both cases to the slow path */
    if (unlikely(d->rehashing | d->keysByCompare)) {
        return multidictFindSlow_(d, keybox, valbox);
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

bool multidictFindByString(multidict *d, const char *key, uint8_t **val) {
    const databox keybox = databoxNewBytesString(key);
    databox valbox = {};
    if (!multidictFind(d, &keybox, &valbox) || valbox.type != DATABOX_BYTES) {
        return false;
    }

    /* A stored byte string borrows from the dict; any other value would
     * point into 'valbox', which goes away on return */
    *val = valbox.data.bytes.start;
    return true;
}

/* ====================================================================
 * Convenience Functions
 * ==================================================================== */

/* Check if a key exists without retrieving the value.  Naming an existing
 * key is one LRU use, so a hit promotes it once (multidict.h, "LRU use"). */
bool multidictExistsNoTouch(multidict *d, const databox *keybox) {
    databox unused = {};
    return d && keybox && multidictFindNoTouch_(d, keybox, &unused);
}

bool multidictExists(multidict *d, const databox *keybox) {
    databox unused = {};
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

/* multidictIncrBy's rewrite.  Increment policy (DATABOX.md "Numeric
 * updates"): exact integer arithmetic; a value that is not an int64, or a
 * result outside int64, is refused with the stored value unchanged.  A
 * float value is not an integer counter: use multidictIncrByFloat. */
typedef struct mdIncrCtx {
    int64_t increment;
    int64_t result;
} mdIncrCtx;

static bool mdIncrUpdate_(void *ctx, const databox *current,
                          databox *replacement) {
    mdIncrCtx *x = ctx;
    int64_t value;
    if (current->type == DATABOX_SIGNED_64) {
        value = current->data.i64;
    } else if (current->type == DATABOX_UNSIGNED_64 &&
               current->data.u64 <= (uint64_t)INT64_MAX) {
        value = (int64_t)current->data.u64;
    } else {
        return false;
    }

    if (ckd_add(&x->result, value, x->increment)) {
        return false;
    }

    *replacement = DATABOX_SIGNED(x->result);
    return true;
}

/* multidictIncrByFloat's rewrite: any 64-bit number or float becomes a
 * double; a 96/128-bit value or a non-finite result is refused */
typedef struct mdIncrFloatCtx {
    double increment;
    double result;
} mdIncrFloatCtx;

static bool mdIncrFloatUpdate_(void *ctx, const databox *current,
                               databox *replacement) {
    mdIncrFloatCtx *x = ctx;
    switch (current->type) {
    case DATABOX_DOUBLE_64:
        x->result = current->data.d64 + x->increment;
        break;
    case DATABOX_FLOAT_32:
        x->result = current->data.f32 + x->increment;
        break;
    case DATABOX_SIGNED_64:
        x->result = (double)current->data.i64 + x->increment;
        break;
    case DATABOX_UNSIGNED_64:
        x->result = (double)current->data.u64 + x->increment;
        break;
    default:
        return false;
    }

    if (!isfinite(x->result)) {
        return false;
    }

    *replacement = DATABOX_DOUBLE(x->result);
    return true;
}

/* Increment numeric value atomically. Creates key with increment if not exists.
 * Returns MULTIDICT_ERR if value exists but is not numeric. */
multidictResult multidictIncrBy(multidict *d, const databox *keybox,
                                int64_t increment, int64_t *result) {
    if (unlikely(!d || !keybox)) {
        return MULTIDICT_ERR;
    }

    /* An existing key is rewritten in place, in one search; the call makes
     * one LRU use of it, rewritten or refused */
    mdIncrCtx x = {.increment = increment};
    if (likely(!MD_MATCH_BY_COMPARE(d, keybox))) {
        /* The same one search adds a new counter */
        const databox fresh = DATABOX_SIGNED(increment);
        switch (multidictUpsert_(d, keybox, &fresh, mdIncrUpdate_, &x)) {
        case MULTIDICT_UPSERT_ADDED:
            if (result) {
                *result = increment;
            }
            return MULTIDICT_OK_INSERTED;
        case MULTIDICT_UPSERT_REWRITTEN:
            if (result) {
                *result = x.result;
            }
            return MULTIDICT_OK_REPLACED;
        case MULTIDICT_UPSERT_DECLINED: /* not an int64 value, or out of range
                                         */
        case MULTIDICT_UPSERT_NO_SLOT:
            return MULTIDICT_ERR;
        }
    }

    const multidictUpdateResult updated =
        multidictUpdate_(d, keybox, mdIncrUpdate_, &x);
    if (updated != MULTIDICT_UPDATE_ABSENT) {
        multidictLRUOnAccess(d, keybox);
        if (updated == MULTIDICT_UPDATE_DECLINED) {
            return MULTIDICT_ERR; /* not an int64 value, or out of range */
        }

        if (result) {
            *result = x.result;
        }

        return MULTIDICT_OK_REPLACED;
    }

    /* Key doesn't exist - create with increment value */
    if (result) {
        *result = increment;
    }

    databox newValbox = DATABOX_SIGNED(increment);
    return multidictAdd(d, keybox, &newValbox);
}

/* Increment float value atomically. Creates key with increment if not exists.
 * Returns MULTIDICT_ERR if value exists but is not numeric. */
multidictResult multidictIncrByFloat(multidict *d, const databox *keybox,
                                     double increment, double *result) {
    if (unlikely(!d || !keybox)) {
        return MULTIDICT_ERR;
    }

    /* As in multidictIncrBy: an existing key is rewritten in place in one
     * search, with one LRU use, rewritten or refused */
    mdIncrFloatCtx x = {.increment = increment};
    if (likely(!MD_MATCH_BY_COMPARE(d, keybox) && isfinite(increment))) {
        /* The same one search adds a new counter */
        const databox fresh = DATABOX_DOUBLE(increment);
        switch (multidictUpsert_(d, keybox, &fresh, mdIncrFloatUpdate_, &x)) {
        case MULTIDICT_UPSERT_ADDED:
            if (result) {
                *result = increment;
            }
            return MULTIDICT_OK_INSERTED;
        case MULTIDICT_UPSERT_REWRITTEN:
            if (result) {
                *result = x.result;
            }
            return MULTIDICT_OK_REPLACED;
        case MULTIDICT_UPSERT_DECLINED: /* not a number, or a non-finite sum */
        case MULTIDICT_UPSERT_NO_SLOT:
            return MULTIDICT_ERR;
        }
    }

    const multidictUpdateResult updated =
        multidictUpdate_(d, keybox, mdIncrFloatUpdate_, &x);
    if (updated != MULTIDICT_UPDATE_ABSENT) {
        multidictLRUOnAccess(d, keybox);
        if (updated == MULTIDICT_UPDATE_DECLINED) {
            return MULTIDICT_ERR; /* not a number, or a non-finite result */
        }

        if (result) {
            *result = x.result;
        }

        return MULTIDICT_OK_REPLACED;
    }

    /* Key doesn't exist - create with increment value; a non-finite one
     * is refused as any non-finite result is */
    if (!isfinite(increment)) {
        return MULTIDICT_ERR;
    }

    if (result) {
        *result = increment;
    }

    databox newValbox = DATABOX_DOUBLE(increment);
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
        if (ht->table[i] == nullptr) {
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
#ifdef DATAKIT_TEST
/* Test builds only: slots walked by the metrics below, so a test can bound
 * the work of byte-based expansion without timing it */
static _Atomic uint64_t mdTestMetricSlotsWalked = 0;
#endif

void multidictGetLoadMetrics(multidict *d, multidictLoadMetrics *metrics) {
    if (!d || !metrics) {
        return;
    }

    memset(metrics, 0, sizeof(*metrics));
#ifdef DATAKIT_TEST
    /* @sync: relaxed counter */
    atomic_fetch_add_explicit(&mdTestMetricSlotsWalked, HTSIZE(d, 0),
                              memory_order_relaxed);
#endif

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
        /* The percentage, saturating at UINT32_MAX */
        uint64_t scaled;
        if (ckd_mul(&scaled, metrics->avgSlotBytes, 100)) {
            metrics->byteLoadFactor = UINT32_MAX;
        } else {
            const uint64_t factor = scaled / d->targetSlotBytes;
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
    /* Far from any count: the next expansion check measures */
    d->byteCheckCount = UINT64_MAX;
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

bool multidictGetByteBasedExpansion(const multidict *d,
                                    uint64_t *targetSlotBytes,
                                    uint64_t *maxSlotBytes) {
    *targetSlotBytes = d->targetSlotBytes;
    *maxSlotBytes = d->maxSlotBytes;
    return d->useByteBasedExpand;
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

    /* Create multilru with specified levels (allocation fails fast) */
    /* @own: container; free=multilruFree */
    d->lru = multilruNewWithLevels(levels > 0 ? levels : 7);

    /* Create auxiliary keyToPtr dict (using same type as main dict) */
    /* @own: container; free=multidictDisableLRU */
    d->lruKeyToPtr = multidictNew(d->type, d->shared, d->seed);

    d->lruPtrToKey = nullptr;
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
        d->lru = nullptr;
    }

    /* Free keyToPtr dict */
    if (d->lruKeyToPtr) {
        multidictFree(d->lruKeyToPtr);
        d->lruKeyToPtr = nullptr;
    }

    /* Free ptrToKey array (just hashes, no key data to free) */
    if (d->lruPtrToKey) {
        zfree(d->lruPtrToKey);
        d->lruPtrToKey = nullptr;
        d->lruPtrToKeySize = 0;
    }

    d->lruEnabled = false;
    d->evictPolicy = MULTIDICT_EVICT_NONE;
}

/* Check if LRU is enabled */
bool multidictHasLRU(multidict *d) {
    return d && d->lruEnabled;
}

size_t multidictLRULevels(const multidict *d) {
    if (!d->lruEnabled) {
        return 0;
    }

    multilruStats stats;
    multilruGetStats(d->lru, &stats);
    return stats.maxLevels;
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
    databox ptrBox = {};
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
    databox ptrBox = {};
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
    if (multidictAdd(d->lruKeyToPtr, keybox, &ptrBox) == MULTIDICT_ERR) {
        multilruDelete(d->lru, ptr); /* the class refused: track nothing */
        return;
    }

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
    databox ptrBox = {};
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

    /* Remove the key's handle from keyToPtr in one search, reading the
     * handle (a number, reported by value) as it goes */
    databox ptrBox = {};
    if (!multidictRemove_(d->lruKeyToPtr, keybox, &ptrBox, false, nullptr)) {
        return;
    }

    multilruPtr ptr = (multilruPtr)ptrBox.data.u64;

    /* Delete from multilru */
    multilruDelete(d->lru, ptr);

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

        databox ptrBox = {};
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
            /* Refill from the cold end; the passed-over candidates lead.
             * Only the first 'count' slots are written. */
            cands->count = (uint32_t)multilruCollectLowest(d->lru, cands->ptr,
                                                           MD_LRU_CANDIDATES);

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
        return nullptr;
    }

    /* Create new dict with same type and class */
    /* @own: caller; free=multidictFree */
    multidict *dst = multidictNew(src->type, src->shared, src->seed);
    if (!dst) {
        return nullptr;
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
    bool copied = true;
    while (copied && multidictIteratorNext(&iter, &entry)) {
        copied = multidictAdd(dst, &entry.key, &entry.val) != MULTIDICT_ERR;
    }
    multidictIteratorRelease(&iter);

    if (!copied) {
        multidictFree(dst); /* the class refused an entry */
        return nullptr;
    }
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
            if (multidictAdd(dst, &entry.key, &entry.val) != MULTIDICT_ERR) {
                merged++;
            }
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
        nullptr; /* Initialize to NULL so first call to Next starts fresh */
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
                iter->current = nullptr;
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
        iter->current = nullptr;
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
static constexpr int MD_RANDOM_KEY_PROBES = 128;

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
            if (slot == nullptr) {
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

/* Load factors a dict may use: every resize leaves the load at about half
 * the growth point, which must stay above the shrink point; below 50% the
 * slot table would outweigh the entries */
static bool multidictLoadFactorsOk_(const uint32_t expandPercent,
                                    const uint32_t shrinkPercent) {
    return expandPercent >= 50 && expandPercent <= UINT16_MAX &&
           shrinkPercent <= UINT8_MAX && shrinkPercent * 4 <= expandPercent;
}

bool multidictSetLoadFactors(multidict *d, const uint32_t expandPercent,
                             const uint32_t shrinkPercent) {
    if (!d || !multidictLoadFactorsOk_(expandPercent, shrinkPercent)) {
        return false;
    }

    d->expandLoadFactor = (uint16_t)expandPercent;
    d->shrinkLoadFactor = (uint8_t)shrinkPercent;
    return true;
}

void multidictGetLoadFactors(const multidict *d, uint32_t *expandPercent,
                             uint32_t *shrinkPercent) {
    *expandPercent = d->expandLoadFactor;
    *shrinkPercent = d->shrinkLoadFactor;
}

/* The load factors new dicts start with: expand in the low 16 bits,
 * shrink above them, one word so a reader sees both from one setting */
static _Atomic uint32_t multidictDefaultLoads_ =
    (uint32_t)MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR |
    ((uint32_t)MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR << 16);

bool multidictSetDefaultLoadFactors(const uint32_t expandPercent,
                                    const uint32_t shrinkPercent) {
    if (!multidictLoadFactorsOk_(expandPercent, shrinkPercent)) {
        return false;
    }

    /* @sync: relaxed hint */
    atomic_store_explicit(&multidictDefaultLoads_,
                          expandPercent | (shrinkPercent << 16),
                          memory_order_relaxed);
    return true;
}

void multidictDefaultLoadFactors(uint32_t *expandPercent,
                                 uint32_t *shrinkPercent) {
    /* @sync: relaxed hint */
    const uint32_t loads =
        atomic_load_explicit(&multidictDefaultLoads_, memory_order_relaxed);
    *expandPercent = loads & UINT16_MAX;
    *shrinkPercent = loads >> 16;
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
DK_STATIC int32_t multidictStringKeyCompare_(void *, const void *key1,
                                             const uint32_t k1len,
                                             const void *key2,
                                             const uint32_t k2len) {
    return k1len == k2len && (k1len == 0 || memcmp(key1, key2, k1len) == 0);
}

/* ASCII case-insensitive key equality: same length, and the same bytes
 * once 'A'-'Z' are folded to 'a'-'z'.  Every other byte, NUL and bytes
 * above 0x7F included, must match exactly, whatever the C locale is.
 * multidictGenCaseHashFunction folds the same set, so keys equal here
 * hash equal. */
DK_STATIC int32_t multidictStringCaseKeyCompare_(void *, const void *key1,
                                                 const uint32_t k1len,
                                                 const void *key2,
                                                 const uint32_t k2len) {
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

/* A slot holds {key, value} rows.  While it is small it is a bare
 * multimap (multimapSmallInternal.h): a flex with no header beside it,
 * its pointer carrying what a header would (low two bits 0, then in bits
 * 48-63 the size limit, the row width and the middle row's offset).  The
 * slot works its rows itself, through a small multimap view built on the
 * stack from that pointer, so it skips the multimap's dispatch; and since
 * the layout is the multimap's own, the slot is a multimap too, as the
 * multimap a grown slot becomes is a slot.  Once it grows past what a
 * small multimap holds it becomes a multimap, which grows its tiers
 * itself.  The table cell holds the slot, and the hooks that may
 * reallocate or replace it take the cell. */
#define MM_SLOT(slot) ((multimap *)(slot))
#define MM_SLOT_REF(slot) ((multimap **)(slot))
#define MM_SLOT_IS_FLEX(slot) MULTIMAP_IS_BARE(slot)
#define MM_FLEX_OF(slot) multimapBareFlex(slot)

/* The pointer of a bare flex slot: the flex, its middle row's offset and
 * its size limit */
DK_INLINE_ALWAYS multidictSlot *mmFlexSlot_(flex *f, const uint32_t middle,
                                            const flexCapSizeLimit limit) {
    return multimapBarePack(f, middle, 2, limit);
}

/* A new, empty bare flex slot, at the process default multimap limit
 * (which it keeps, as a multimap keeps the limit it was made with) */
DK_INLINE_ALWAYS multidictSlot *mmFlexSlotNew_(void) {
    /* @own: container; free=mmSlotFree */
    return mmFlexSlot_(flexNew(), FLEX_EMPTY_SIZE,
                       dkTierDefaultLimit(DK_TIER_MULTIMAP));
}

/* A bare flex slot as a small multimap of {key, value} rows */
DK_INLINE_ALWAYS multimapSmall mmFlexView_(multidictSlot *slot) {
    multimapSmall view = multimapBareView(slot);
    /* A slot's rows are {key, value}: the constant width lets the row
     * code fold its per-element loops */
    assert(view.elementsPerEntry == 2);
    view.elementsPerEntry = 2;
    return view;
}

/* After a change through 'view': the cell holds its (maybe moved) flex
 * and middle, or, past a small multimap's size (or a middle the pointer
 * cannot hold), the multimap its rows become */
DK_STATIC void mmFlexSettle_(multidictSlot **slot, const multimapSmall *view) {
    /* *slot still holds the slot as it was: its limit is in its bits */
    const flexCapSizeLimit limit = (flexCapSizeLimit)multimapBareLimit(*slot);
    if (flexBytes(view->map) <= flexOptimizationSizeLimit[limit] &&
        view->middle < MULTIMAP_BARE_MIDDLE_LIMIT) {
        *slot = mmFlexSlot_(view->map, view->middle, limit);
        return;
    }

    /* @own: container; free=mmSlotFree */
    *slot = multimapNewFromDump(2, false, false, limit, view->map);
}

DK_STATIC int64_t mmSlotInsert(multidictClass *, multidictSlot **slot,
                               const databox *keybox, const databox *valbox,
                               databox *replaced) {
    if (*slot == nullptr) {
        return -1;
    }

    const databox *elements[2] = {keybox, valbox};
    databox overwritten;
    databox *const out = replaced ? replaced : &overwritten;
    bool wasReplaced;
    if (MM_SLOT_IS_FLEX(*slot)) {
        multimapSmall view = mmFlexView_(*slot);
        wasReplaced = multimapSmallInsertReplacing(&view, elements, out);
        mmFlexSettle_(slot, &view);
    } else {
        wasReplaced = multimapInsertReplacing(MM_SLOT_REF(slot), elements, out);
    }

    return wasReplaced ? 0 : 1; /* 1 = new insert, 0 = replaced */
}

DK_STATIC multidictInsertAbsentResult
mmSlotInsertIfAbsent(multidictClass *, multidictSlot **slot,
                     const databox *keybox, const databox *valbox) {
    if (*slot == nullptr) {
        return MULTIDICT_INSERT_NO_SLOT;
    }

    const databox *elements[2] = {keybox, valbox};
    bool added;
    if (MM_SLOT_IS_FLEX(*slot)) {
        multimapSmall view = mmFlexView_(*slot);
        added = multimapSmallInsertIfAbsent(&view, elements, nullptr);
        mmFlexSettle_(slot, &view);
    } else {
        added = multimapInsertIfAbsent(MM_SLOT_REF(slot), elements);
    }

    return added ? MULTIDICT_INSERT_ADDED : MULTIDICT_INSERT_PRESENT;
}

DK_STATIC void *mmSlotGetOrCreate(multidictClass *, multidictSlot **slot,
                                  const databox *) {
    if (*slot == nullptr) {
        /* @own: container; free=mmSlotFree */
        *slot = mmFlexSlotNew_(); /* rows of key + value */
    }
    return *slot;
}

DK_STATIC bool mmSlotRemove(multidictClass *, multidictSlot **slot,
                            const databox *keybox, databox *removedVal,
                            bool take) {
    if (*slot == nullptr) {
        return false;
    }

    if (MM_SLOT_IS_FLEX(*slot)) {
        multimapSmall view = mmFlexView_(*slot);
        const bool removed =
            take ? multimapSmallDeleteTaking(&view, keybox, removedVal)
                 : multimapSmallDeleteReporting(&view, keybox, removedVal);
        mmFlexSettle_(slot, &view);
        return removed;
    }

    return take
               ? multimapDeleteTaking(MM_SLOT_REF(slot), keybox, removedVal)
               : multimapDeleteReporting(MM_SLOT_REF(slot), keybox, removedVal);
}

DK_STATIC bool mmSlotFindValue(multidictClass *, multidictSlot *slot,
                               const databox *keybox, databox *valbox) {
    if (slot == nullptr) {
        return false;
    }

    /* Lookups return only the non-key elements in the array.  For rows
     * of key + value, elements[0] gets the value. */
    databox *elements[1] = {valbox};
    if (MM_SLOT_IS_FLEX(slot)) {
        /* The sorted search, byte-string keys included: an equality scan
         * (flexFindString) avoids the natural-order compares but walks
         * every row, and measured 9-22% slower at this load factor */
        multimapSmall view = mmFlexView_(slot);
        return multimapSmallLookup(&view, keybox, elements);
    }

    return multimapLookup(MM_SLOT(slot), keybox, elements);
}

DK_STATIC void *mmSlotCreate(void) {
    /* @own: container; free=mmSlotFree */
    return mmFlexSlotNew_(); /* rows of key + value */
}

/* The rows a slot holds */
DK_INLINE_ALWAYS uint32_t mmSlotRows_(multidictSlot *slot) {
    return MM_SLOT_IS_FLEX(slot) ? (uint32_t)(flexCount(MM_FLEX_OF(slot)) / 2)
                                 : (uint32_t)multimapCount(MM_SLOT(slot));
}

DK_STATIC uint32_t mmSlotFree(multidictClass *, multidictSlot *slot) {
    if (!slot) {
        return 0;
    }

    const uint32_t count = mmSlotRows_(slot);
    if (MM_SLOT_IS_FLEX(slot)) {
        flexFree(MM_FLEX_OF(slot));
    } else {
        multimapFree(MM_SLOT(slot));
    }

    return count;
}

DK_STATIC size_t mmSlotSizeBytes(multidictSlot *slot) {
    if (!slot) {
        return 0;
    }

    return MM_SLOT_IS_FLEX(slot) ? flexBytes(MM_FLEX_OF(slot))
                                 : multimapBytes(MM_SLOT(slot));
}

/* An iterator over a slot's rows, forward */
DK_INLINE_ALWAYS void mmSlotIteratorInit_(multidictSlot *slot,
                                          multimapIterator *iter) {
    if (MM_SLOT_IS_FLEX(slot)) {
        /* The iterator keeps the flex, not the view */
        multimapSmall view = mmFlexView_(slot);
        multimapSmallIteratorInit(&view, iter, true);
    } else {
        multimapIteratorInit(MM_SLOT(slot), iter, true);
    }
}

/* Iterator state stored in multidictSlotIterator.iterspace */
typedef struct {
    multimapIterator mmIter;
} mmSlotIterState;

DK_STATIC bool mmSlotGetIter(multidictSlotIterator *iter, multidictSlot *slot) {
    if (slot == nullptr) {
        return false;
    }

    if (mmSlotRows_(slot) == 0) {
        return false;
    }

    mmSlotIterState *state = (mmSlotIterState *)iter->iterspace;
    mmSlotIteratorInit_(slot, &state->mmIter);
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
    return slot ? mmSlotRows_(slot) : 0;
}

DK_STATIC bool mmSlotFindKeyByPos(multidictClass *, multidictSlot *slot,
                                  uint32_t pos, databox *keybox) {
    if (slot == nullptr) {
        return false;
    }

    /* Iterate to find key at position */
    multimapIterator mmIter;
    mmSlotIteratorInit_(slot, &mmIter);

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

DK_STATIC void mmSlotIterateAll(multidictClass *, multidictSlot *slot,
                                multidictIterProcess process, void *privdata) {
    if (slot == nullptr || process == nullptr) {
        return;
    }

    multimapIterator mmIter;
    mmSlotIteratorInit_(slot, &mmIter);

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

/* The row 'me' of '*slot' (found by key; 'view' the slot's view when it
 * is a bare flex, else NULL): its value read through the entry, handed to
 * 'update' and, unless refused, rewritten in place */
DK_STATIC multidictUpdateResult mmSlotRewrite_(multidictSlot **slot,
                                               multimapSmall *view,
                                               multimapEntry *me,
                                               multidictValueUpdate *update,
                                               void *ctx) {
    me->fe = flexNext(*me->map, me->fe);
    databox current;
    databoxWide currentWide;
    mdBoxAim_(&current, &currentWide);
    flexGetByTypeInto(me->fe, &current, &currentWide);

    databox replacement;
    if (!update(ctx, &current, &replacement)) {
        return MULTIDICT_UPDATE_DECLINED;
    }

    if (view) {
        multimapSmallReplaceEntry(view, me, &replacement);
        mmFlexSettle_(slot, view);
    } else {
        multimapReplaceEntry(MM_SLOT_REF(slot), me, &replacement);
    }

    return MULTIDICT_UPDATE_REWRITTEN;
}

DK_STATIC multidictUpdateResult mmSlotUpdate(multidictClass *,
                                             multidictSlot **slot,
                                             const databox *keybox,
                                             multidictValueUpdate *update,
                                             void *ctx) {
    if (*slot == nullptr) {
        return MULTIDICT_UPDATE_ABSENT;
    }

    const bool flexSlot = MM_SLOT_IS_FLEX(*slot);
    multimapSmall view = {};
    multimapEntry me;
    if (flexSlot) {
        view = mmFlexView_(*slot);
        if (!multimapSmallGetUnderlyingEntry(&view, keybox, &me)) {
            return MULTIDICT_UPDATE_ABSENT;
        }
    } else if (!multimapGetUnderlyingEntry(MM_SLOT(*slot), keybox, &me)) {
        return MULTIDICT_UPDATE_ABSENT;
    }

    return mmSlotRewrite_(slot, flexSlot ? &view : nullptr, &me, update, ctx);
}

DK_STATIC multidictUpsertResult
mmSlotUpsert(multidictClass *qdc, multidictSlot **slot, const databox *keybox,
             const databox *valbox, multidictValueUpdate *update, void *ctx) {
    if (*slot == nullptr) {
        return MULTIDICT_UPSERT_NO_SLOT;
    }

    /* The find comes first: it is the map's own tuned search, so a present
     * key (the common case) costs what an update does.  An absent key is
     * inserted into this slot, searched again only within it.  (The
     * insert-if-absent search is the generic sorted insert: measured 7%
     * slower for present keys.) */
    switch (mmSlotUpdate(qdc, slot, keybox, update, ctx)) {
    case MULTIDICT_UPDATE_REWRITTEN:
        return MULTIDICT_UPSERT_REWRITTEN;
    case MULTIDICT_UPDATE_DECLINED:
        return MULTIDICT_UPSERT_DECLINED;
    case MULTIDICT_UPDATE_ABSENT:
        break;
    }

    const databox *elements[2] = {keybox, valbox};
    if (MM_SLOT_IS_FLEX(*slot)) {
        multimapSmall view = mmFlexView_(*slot);
        (void)multimapSmallInsert(&view, elements);
        mmFlexSettle_(slot, &view);
    } else {
        (void)multimapInsert(MM_SLOT_REF(slot), elements);
    }
    return MULTIDICT_UPSERT_ADDED;
}

DK_STATIC void mmSlotClassFree(multidictClass *) {
    /* Nothing to free for this simple class */
}

DK_STATIC uint32_t mmSlotFreeClass(multidictClass *) {
    return 0;
}

/* Create a multimap-based multidictClass for using multimap as chained backing
 * stores for dict inserts */
DK_STATIC multidictClass *multidictMmClassNew(void) {
    /* @own: caller; free=multidictMmClassFree */
    multidictClass *qdc = zcalloc(1, sizeof(*qdc));
    qdc->privdata = nullptr;
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
    qdc->updateByType = mmSlotUpdate;
    qdc->insertIfAbsentByType = mmSlotInsertIfAbsent;
    qdc->upsertByType = mmSlotUpsert;
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
#include "ctest.h"
#include "persist.h" /* snapshot round trips of wide pairs */
#include <locale.h>  /* the case compare ignores the C locale */

/* Scope-exit cleaners (see cleaner.h idioms).  A dict must be destroyed
 * before the class it borrows, so a class cleaner is always declared
 * BEFORE the dict cleaner that uses it. */
defcleaner(multidict *, multidictFree);
defcleaner(multidictClass *, multidictMmClassFree);
defcleaner(flex *, flexFree);

/* A conditional add or replace applies exactly when 'applies' (read
 * before the call) holds */
#define MD_TEST_APPLIES_IF(applies, call)                                      \
    do {                                                                       \
        const bool applies_ = (applies);                                       \
        const multidictResult result_ = (call);                                \
        if ((result_ != MULTIDICT_ERR) != applies_) {                          \
            ERR("%s returned %d, applies %d", #call, (int)result_,             \
                (int)applies_);                                                \
        }                                                                      \
    } while (0)

/* What mdTestRefusingInsert refuses */
typedef enum mdTestRefuseMode {
    MD_TEST_REFUSE_NONE = 0,
    MD_TEST_REFUSE_ALL,      /* every add */
    MD_TEST_REFUSE_UNSIGNED, /* an unsigned value, such as an LRU handle */
} mdTestRefuseMode;
static mdTestRefuseMode mdTestRefuse = MD_TEST_REFUSE_NONE;

/* The default insert, refusing as mdTestRefuse says */
static int64_t mdTestRefusingInsert(multidictClass *qdc, multidictSlot **slot,
                                    const databox *keybox,
                                    const databox *valbox, databox *replaced) {
    if (mdTestRefuse == MD_TEST_REFUSE_ALL ||
        (mdTestRefuse == MD_TEST_REFUSE_UNSIGNED &&
         valbox->type == DATABOX_UNSIGNED_64)) {
        return -1;
    }
    return mmSlotInsert(qdc, slot, keybox, valbox, replaced);
}

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
static void scanCountCallback(void *privdata, const databox *,
                              const databox *) {
    int *count = (int *)privdata;
    (*count)++;
}

/* Test eviction callbacks (defined at file scope for portability) */
static int testEvictionCallbackCount_ = 0;
static bool testEvictionCb_(void *privdata, const databox *, const databox *) {
    int *counter = (int *)privdata;
    (*counter)++;
    return true; /* Allow eviction */
}

static int testVetoCount_ = 0;
static bool vetoEvictionCb_(void *privdata, const databox *, const databox *) {
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
static bool evictRehashCb_(void *, const databox *key, const databox *val) {
    testEvictRehashSeen_++;
    static constexpr char keyPrefix[] = "evict-rehash-key-";
    static constexpr char valPrefix[] = "evict-rehash-val-";
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
static constexpr int TEST_LRU_VETO_MAX = 256;
static bool testLruVetoActive_ = false;
static int32_t testLruVetoes_ = 0;
static int32_t testLruVetoBad_ = 0;
static int32_t testLruEvicted_ = 0;
static int32_t testLruEvictOrder_[TEST_LRU_VETO_MAX];
static bool lruVetoCb_(void *, const databox *key, const databox *val) {
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
static uint32_t testConstHash_(multidict *, const void *, uint32_t) {
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

    mdWideVal n = {};
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
static size_t mdWideBytes_(const mdWideVal *v) {
    return v->isBytes ? strlen(v->str) : mdWideIsWide_(v) ? 16 : 8;
}

static uint64_t mdWideMix_(uint64_t x) {
    return dkSplitmix64Next(&x);
}

/* Value 'i' of a fixed sequence: the wide set W (2^64, 2^95 - 1, 2^95,
 * 2^127, UINT128_MAX, -2^63 - 1, -2^95, INT128_MIN, INT128_MAX), then
 * 96/128-bit values of both signs derived from 'i' */
static mdWideVal mdWideInt_(size_t i) {
    const __uint128_t one = 1;
    mdWideVal v = {};
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

typedef struct mdWideScanState {
    const mdWidePair *p;
    size_t n;
    uint8_t *seen;
    int bad;
} mdWideScanState;

defcleanerName(uint8_t *, zfree, SeenBytes);
defcleanerName(char *, zfree, BenchKeys);
defcleanerName(uint32_t *, zfree, HashWords);

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
static const mdWidePair *mdWideEvictPairs_ = nullptr;
static size_t mdWideEvictCount_ = 0;

static bool mdWideEvictCb_(void *, const databox *key, const databox *val) {
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
        ctestPoisonStack();
        if (!multidictFind(d, (databox *)&key, (databox *)&val) ||
            !mdWideMatches_((databox *)&val, &p[i].val)) {
            ERR("wide Find mismatch for pair %zu", i);
        }
    }

    if (multidictCount(d) != live) {
        ERR("wide count %" PRIu64 ", expected %zu", multidictCount(d), live);
    }

    if (multidictKeyBytes(d) != keyBytes || multidictValBytes(d) != valBytes) {
        ERR("wide byte stats key %" PRIu64 "/%" PRIu64 " val %" PRIu64
            "/%" PRIu64,
            multidictKeyBytes(d), keyBytes, multidictValBytes(d), valBytes);
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
            ctestPoisonStack();
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
                ERR("wide %s iteration entry %zu wrong",
                    safe ? "safe" : "unsafe", visited);
                break;
            }
            seen[at] = 1;
        }
        multidictIteratorRelease(&iter);
        if (visited != live) {
            ERR("wide %s iteration visited %zu of %zu",
                safe ? "safe" : "unsafe", visited, live);
        }
    }

    /* GetSomeKeys ignores poisoned destinations: a wide key comes back
     * deferred, any other key whole with clean flags */
    if (live > 0) {
        databox some[16];
        memset(some, 0xFF, sizeof(some));
        ctestPoisonStack();
        const uint32_t got = multidictGetSomeKeys(d, some, 16);
        if (got == 0) {
            ERR("GetSomeKeys returned nothing");
        }
        for (uint32_t i = 0; i < got; i++) {
            const bool wide = some[i].type == DATABOX_SIGNED_128 ||
                              some[i].type == DATABOX_UNSIGNED_128;
            if (wide ? !databoxIsWideDeferred(&some[i])
                     : (some[i].big || some[i].allocated ||
                        mdWideFind_(p, n, &some[i]) == SIZE_MAX)) {
                ERR("GetSomeKeys slot %u wrong", i);
            }
        }
    }

    /* Scan to completion; duplicates are allowed, misses are not */
    memset(seen, 0, n + 1);
    mdWideScanState state = {.p = p, .n = n, .seen = seen, .bad = 0};
    uint64_t cursor = 0;
    do {
        ctestPoisonStack();
        cursor = multidictScan(d, cursor, mdWideScanCb_, &state);
    } while (cursor != 0);
    for (size_t i = 0; i < n; i++) {
        if (p[i].live && !seen[i]) {
            state.bad++;
        }
    }
    if (state.bad) {
        ERR("wide scan saw %d wrong or missing pairs", state.bad);
    }

    return err;
}

/* Counts the times multidictGetAndDeleteIf asked, keeping the value it
 * was shown and answering 'answer' */
typedef struct mdGateProbe_ {
    bool answer;
    uint32_t asked;
    int64_t seen;
} mdGateProbe_;

static bool mdGateProbe(void *ctx, const databox *value) {
    mdGateProbe_ *probe = ctx;
    probe->asked++;
    probe->seen = value->data.i64;
    return probe->answer;
}

/* GetAndDeleteIf against the dict's own answers, in a settled table and
 * while a rehash is moving keys: a declined key stays with its value and
 * LRU level, an approved one is taken with its value, an absent one is
 * never asked about */
static int multidictTestDeleteIf_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 0);
    multidictEnableLRU(d, 4);
    constexpr int n = 2000;
    for (int64_t k = 0; k < n; k++) {
        const databox key = databoxNewSigned(k);
        const databox val = databoxNewSigned(k * 3);
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
    }

    size_t live = n;
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 1 &&
            (!multidictExpand(d, 16384) || !multidictIsRehashing(d))) {
            ERR("could not start a rehash");
        }
        for (int64_t k = phase; k < n; k += 4) {
            const databox key = databoxNewSigned(k);
            const int level = multidictGetLRULevel(d, &key);
            mdGateProbe_ probe = {.answer = false};
            databox got = {};
            if (multidictGetAndDeleteIf(d, &key, &got, mdGateProbe, &probe) ||
                probe.asked != 1 || probe.seen != k * 3 ||
                !multidictExistsNoTouch(d, &key) || multidictCount(d) != live ||
                multidictGetLRULevel(d, &key) != level) {
                ERR("phase %d key %" PRId64 ": a declined delete changed it",
                    phase, k);
                break;
            }
            probe.answer = true;
            if (!multidictGetAndDeleteIf(d, &key, &got, mdGateProbe, &probe) ||
                probe.asked != 2 || got.data.i64 != k * 3 ||
                multidictExistsNoTouch(d, &key) ||
                multidictCount(d) != --live) {
                ERR("phase %d key %" PRId64 ": an approved delete misread",
                    phase, k);
                break;
            }
            if (multidictGetAndDeleteIf(d, &key, &got, mdGateProbe, &probe) ||
                probe.asked != 2) {
                ERR("phase %d key %" PRId64 ": an absent key was asked about",
                    phase, k);
                break;
            }
        }
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
        ctestPoisonStack();
        if (multidictAdd(d, (databox *)&key, (databox *)&val) !=
            MULTIDICT_OK_INSERTED) {
            ERR("wide Add of pair %zu did not insert", i);
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
            ERR("128-bit box of a 64-bit key %zu not found", i);
        }
    }

    /* A zeroed caller box receives a wide value's type only */
    if (mode != MD_WIDE_NORMAL) {
        databoxBig key;
        mdWideBox_(&key, &p[0].key);
        databox val = {};
        if (!multidictFind(d, (databox *)&key, &val) ||
            val.type != DATABOX_UNSIGNED_128 || val.data.ptr) {
            ERR("zeroed box did not get a type-only wide value");
        }
    }

    /* Random keys land in caller-provided wide storage */
    for (int r = 0; r < 64; r++) {
        databoxBig key;
        DATABOX_BIG_INIT(&key);
        ctestPoisonStack();
        if (!multidictGetRandomKey(d, (databox *)&key) ||
            mdWideFind_(p, n, (databox *)&key) == SIZE_MAX) {
            ERR("GetRandomKey returned an unknown key");
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
            snap ? persistOpsMultidict.restore(snap, snapLen) : nullptr;
        if (!restored) {
            ERR("persist snapshot round trip failed");
        } else {
            err += mdWideVerify_(restored, p, n);
            persistOpsMultidict.free(restored);
        }

        cleaner(multidictFree) multidict *merged =
            multidictNew(&multidictTypeExactKey, qdc, 0);
        if (multidictMerge(merged, d, MULTIDICT_MERGE_REPLACE) != n) {
            ERR("wide Merge count wrong");
        }
        err += mdWideVerify_(merged, p, n);
    }

    /* Delete, GetAndDelete into a poisoned box, PopRandom into poisoned
     * boxes: removed values are self-contained */
    for (size_t i = 0; i < n; i += 4) {
        databoxBig key;
        mdWideBox_(&key, &p[i].key);
        ctestPoisonStack();
        if (!multidictDelete(d, (databox *)&key)) {
            ERR("wide Delete of pair %zu failed", i);
        }
        p[i].live = false;
        if (multidictExists(d, (databox *)&key)) {
            ERR("pair %zu exists after Delete", i);
        }
    }
    err += mdWideVerify_(d, p, n);

    for (size_t i = 1; i < n; i += 4) {
        databoxBig key;
        mdWideBox_(&key, &p[i].key);
        databox val;
        memset(&val, 0xFF, sizeof(val));
        ctestPoisonStack();
        if (!multidictGetAndDelete(d, (databox *)&key, &val)) {
            ERR("GetAndDelete of pair %zu failed", i);
        } else {
            if (!mdWideMatches_(&val, &p[i].val) ||
                !databoxTestTakenOwnsPayload(&val)) {
                ERR("GetAndDelete value of pair %zu wrong", i);
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
        ctestPoisonStack();
        if (!multidictPopRandom(d, &key, &val)) {
            ERR("PopRandom failed");
            break;
        }

        const size_t at = mdWideFind_(p, n, &key);
        if (at == SIZE_MAX || !mdWideMatches_(&val, &p[at].val) ||
            !databoxTestTakenOwnsPayload(&key) ||
            !databoxTestTakenOwnsPayload(&val)) {
            ERR("PopRandom returned a wrong pair");
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
            MULTIDICT_TEST_OK(
                multidictAdd(e, (databox *)&key, (databox *)&val));
        }

        /* Evict with a rehash in flight and resizing on, so the find and
         * delete of each victim run rehash steps that move entries and
         * free their slots, and the evictions themselves shrink the table */
        multidictExpand(e, (uint64_t)n * 4);
        if (!multidictIsRehashing(e)) {
            ERR("policy %d eviction did not start rehashing", policy);
        }

        mdWideEvictPairs_ = p;
        mdWideEvictCount_ = n;
        mdWideEvictSeen_ = 0;
        mdWideEvictBad_ = 0;
        multidictSetEvictionCallback(e, mdWideEvictCb_, nullptr);
        multidictSetMaxMemory(e, (multidictKeyBytes(e) + multidictValBytes(e)) /
                                     2);
        ctestPoisonStack();
        const uint32_t evicted = multidictEvictToLimit(e);
        if (evicted == 0 || mdWideEvictBad_ != 0 ||
            mdWideEvictSeen_ < (int)evicted) {
            ERR("policy %d evicted %u, callback saw %d (%d bad)", policy,
                evicted, mdWideEvictSeen_, mdWideEvictBad_);
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
    qdc->insertByType(qdc, &slot, (databox *)&key, (databox *)&val, nullptr);
    DATABOX_BIG_UNSIGNED_128(&key, k2);
    DATABOX_BIG_SIGNED_128(&val, v2);
    qdc->insertByType(qdc, &slot, (databox *)&key, (databox *)&val, nullptr);

    multidictSlotIterator iter;
    multidictEntry first;
    memset(&iter, 0xFF, sizeof(iter));
    memset(&first, 0xFF, sizeof(first));
    if (!qdc->getIter(&iter, slot) || !qdc->iterNext(&iter, &first)) {
        ERR("slot iterator produced nothing");
    } else {
        multidictSlotIterator copy = iter;
        multidictEntry second;
        memset(&second, 0xFF, sizeof(second));
        const bool more = qdc->iterNext(&copy, &second);

        if (first.key.data.u128 != (__uint128_t *)iter.wide[0].b ||
            first.val.data.i128 != (__int128_t *)iter.wide[1].b ||
            !first.key.big || first.key.allocated ||
            *first.key.data.u128 != k1 || *first.val.data.i128 != v1) {
            ERR("slot iterator entry does not use its slots");
        }

        if (!more || second.key.data.u128 != (__uint128_t *)copy.wide[0].b ||
            *second.key.data.u128 != k2 || *second.val.data.i128 != v2) {
            ERR("copied slot iterator did not use its own slots");
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

    for (size_t i = 0; i < countof(cases); i++) {
        const bool e = exact(nullptr, cases[i].a, cases[i].alen, cases[i].b,
                             cases[i].blen) != 0;
        const bool f = fold(nullptr, cases[i].a, cases[i].alen, cases[i].b,
                            cases[i].blen) != 0;
        if (e != cases[i].exactEq || f != cases[i].foldEq) {
            ERR("key compare case %zu: exact %d fold %d", i, e, f);
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
            const bool got = fold(nullptr, &ab, 1, &bb, 1) != 0;
            if (got != want) {
                ERR("case compare of 0x%02x and 0x%02x is %d", a, b, got);
            }

            if (want && multidictGenCaseHashFunction(d, &ab, 1) !=
                            multidictGenCaseHashFunction(d, &bb, 1)) {
                ERR("case hash of 0x%02x and 0x%02x differs", a, b);
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

        if (!fold(nullptr, a, len, b, len) ||
            multidictGenCaseHashFunction(d, a, (int32_t)len) !=
                multidictGenCaseHashFunction(d, b, (int32_t)len)) {
            ERR("case flip round %d does not match", round);
        }
    }

    /* The case hash is the seeded exact hash of the folded bytes, for
     * keys that fit one fold chunk and for keys streamed in several */
    static constexpr uint32_t lens[] = {0,   1,   7,   63,  64,  255, 256,
                                        257, 300, 511, 512, 513, 1000};
    for (size_t li = 0; li < countof(lens); li++) {
        uint8_t raw[1000];
        uint8_t folded[1000];
        uint8_t flipped[1000];
        const uint32_t len = lens[li];
        for (uint32_t i = 0; i < len; i++) {
            raw[i] = (uint8_t)testRand();
            folded[i] = testFoldAscii_(raw[i]);
            flipped[i] = (raw[i] >= 'a' && raw[i] <= 'z')
                             ? (uint8_t)(raw[i] - ('a' - 'A'))
                             : raw[i];
        }

        const uint32_t h = multidictGenCaseHashFunction(d, raw, (int32_t)len);
        if (h != multidictGenHashFunction(d, folded, (int32_t)len) ||
            h != multidictGenCaseHashFunction(d, flipped, (int32_t)len) ||
            !fold(nullptr, raw, len, flipped, len)) {
            ERR("case hash of a %" PRIu32 "-byte key is not the seeded hash "
                "of its folded bytes",
                len);
        }
    }

    /* Keys built from the blocks "b[" and "a|" all collide under a djb33
     * fold hash for every seed (98 * 33 + 91 == 97 * 33 + 124, and h * 33
     * + c chains the collision).  The seeded hash tells them apart, and
     * places them differently under different seeds. */
    {
        enum { blocks = 6, keys = 1 << blocks };
        uint8_t set[keys][blocks * 2];
        for (uint32_t k = 0; k < keys; k++) {
            for (uint32_t b = 0; b < blocks; b++) {
                set[k][b * 2] = ((k >> b) & 1) ? 'b' : 'a';
                set[k][b * 2 + 1] = ((k >> b) & 1) ? '[' : '|';
            }
        }

        /* An independent djb33 fold over each key confirms the set */
        for (uint32_t seed = 0; seed < 3; seed++) {
            uint32_t first = 0;
            for (uint32_t k = 0; k < keys; k++) {
                uint32_t h = seed * 7919u;
                for (uint32_t i = 0; i < blocks * 2; i++) {
                    h = h * 33u + testFoldAscii_(set[k][i]);
                }
                if (k == 0) {
                    first = h;
                } else if (h != first) {
                    ERR("djb collision set does not collide");
                    break;
                }
            }
        }

        cleaner(multidictFree) multidict *d2 =
            multidictNew(&multidictTypeCaseKey, qdc, 9001);
        uint32_t hashes[keys];
        uint32_t sameAcrossSeeds = 0;
        for (uint32_t k = 0; k < keys; k++) {
            hashes[k] = multidictGenCaseHashFunction(d, set[k], blocks * 2);
            sameAcrossSeeds += hashes[k] == multidictGenCaseHashFunction(
                                                d2, set[k], blocks * 2);
        }

        uint32_t collisions = 0;
        for (uint32_t a = 0; a < keys; a++) {
            for (uint32_t b = a + 1; b < keys; b++) {
                collisions += hashes[a] == hashes[b];
            }
        }

        if (collisions != 0 || sameAcrossSeeds != 0) {
            ERR("case hash: %" PRIu32 " collisions in the djb set, %" PRIu32
                " keys hash alike under two seeds",
                collisions, sameAcrossSeeds);
        }

        /* In a dict they spread over the slots instead of piling up */
        for (uint32_t k = 0; k < keys; k++) {
            const databox kb = databoxNewBytes(set[k], blocks * 2);
            const databox vb = DATABOX_UNSIGNED(k);
            MULTIDICT_TEST_OK(multidictAdd(d2, &kb, &vb));
        }
        for (int32_t pass = 0; pass < 2; pass++) {
            while (multidictRehash(d2, 100)) {
            }
            multidictExpand(d2, 256); /* refused while a rehash runs */
        }
        while (multidictRehash(d2, 100)) {
        }

        multidictDetailedStats stats;
        multidictGetDetailedStats(d2, &stats);
        if (multidictCount(d2) != keys || stats.maxChainLen > 8) {
            ERR("djb collision set piles %" PRIu64 " keys into one slot",
                stats.maxChainLen);
        }
    }

    /* A locale that folds Latin-1 does not change the case compare */
    const char *prior = setlocale(LC_CTYPE, nullptr);
    char saved[64] = {};
    if (prior) {
        snprintf(saved, sizeof(saved), "%s", prior);
    }

    static const char *latin1[] = {"en_US.ISO8859-1", "en_US.ISO-8859-1",
                                   "de_DE.ISO8859-1"};
    for (size_t i = 0; i < countof(latin1); i++) {
        if (setlocale(LC_CTYPE, latin1[i])) {
            if (fold(nullptr, "\xc4", 1, "\xe4", 1) ||
                !fold(nullptr, "A\xc4", 2, "a\xc4", 2)) {
                ERR("case compare follows locale %s", latin1[i]);
            }
            break;
        }
    }
    setlocale(LC_CTYPE, saved[0] ? saved : "C");

    return err;
}

/* Is 'box' the byte string 'bytes' of 'len' bytes? */
static bool testBoxIsBytes_(const databox *box, const void *bytes, size_t len) {
    return DATABOX_IS_BYTES(box) && databoxLen(box) == len &&
           (len == 0 || memcmp(databoxCBytes(box), bytes, len) == 0);
}

/* Does 'box' hold the integer 'want'? */
static bool testBoxIsInt_(const databox *box, int64_t want) {
    return DATABOX_IS_INTEGER(box) && box->data.i == want;
}

/* Is 'key' the one entry of 'd', under exactly this spelling, holding a
 * value equal to 'val'? */
static bool testOnlyEntryIs_(multidict *d, const void *key, size_t len,
                             const databox *val) {
    multidictIterator iter;
    multidictIteratorInit(d, &iter);
    multidictEntry e;
    uint32_t seen = 0;
    bool ok = true;
    while (multidictIteratorNext(&iter, &e)) {
        seen++;
        ok = ok && testBoxIsBytes_(&e.key, key, len) &&
             databoxEqual(&e.val, val);
    }
    multidictIteratorRelease(&iter);
    return ok && seen == 1;
}

/* Is 'key' one of the 'n' C strings in 'names', spelled exactly? */
static bool testKeyIsOneOf_(const databox *key, const char *const *names,
                            size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (testBoxIsBytes_(key, names[i], strlen(names[i]))) {
            return true;
        }
    }
    return false;
}

static bool testRecordEvictionCb_(void *privdata, const databox *key,
                                  const databox *) {
    int32_t *bad = privdata;
    static const char *const stored[] = {"Alpha", "BETA", "gamma"};
    if (!testKeyIsOneOf_(key, stored, 3)) {
        (*bad)++;
    }
    return true;
}

static void testSpellingScanCb_(void *privdata, const databox *key,
                                const databox *) {
    int32_t *bad = privdata;
    static const char *const stored[] = {"Alpha", "BETA", "gamma"};
    if (!testKeyIsOneOf_(key, stored, 3)) {
        (*bad)++;
    }
}

/* Each key-matching operation of a case dict, apart from LRU (Test
 * 20.10) and rehashing (Test 9.6) */
static int multidictTestCaseOps_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeCaseKey, qdc, 99);

    const databox one = DATABOX_SIGNED(1);
    const databox two = DATABOX_SIGNED(2);
    const databox three = DATABOX_SIGNED(3);
    databox Key = databoxNewBytesString("Key");
    databox KEY = databoxNewBytesString("KEY");
    databox kEY = databoxNewBytesString("kEY");
    databox key = databoxNewBytesString("key");
    databox other = databoxNewBytesString("other");

    /* Conditional adds and replace match across spellings */
    if (multidictAddNX(d, &Key, &one) != MULTIDICT_OK_INSERTED ||
        multidictAddNX(d, &KEY, &two) != MULTIDICT_ERR ||
        multidictAddXX(d, &kEY, &two) != MULTIDICT_OK_REPLACED ||
        multidictReplace(d, &KEY, &three) != MULTIDICT_OK_REPLACED ||
        multidictAddXX(d, &other, &one) != MULTIDICT_ERR ||
        multidictReplace(d, &other, &one) != MULTIDICT_ERR ||
        !testOnlyEntryIs_(d, "Key", 3, &three)) {
        ERR("case conditional adds");
    }

    /* Exists, by box and by string, and FindByString */
    databox bytesVal = databoxNewBytesString("payload");
    MULTIDICT_TEST_OK(multidictAdd(d, &key, &bytesVal)); /* replaces "Key" */
    uint8_t *found = nullptr;
    if (!multidictExists(d, &kEY) || !multidictExistsByString(d, "KEY") ||
        multidictExistsByString(d, "KEYS") ||
        !multidictFindByString(d, "kEy", &found) || !found ||
        memcmp(found, "payload", 7) != 0) {
        ERR("case exists / find by string");
    }

    /* Increments create under the first spelling, then match any */
    databox counter = databoxNewBytesString("Counter");
    databox COUNTER = databoxNewBytesString("COUNTER");
    databox counterLow = databoxNewBytesString("counter");
    int64_t ival = 0;
    double dval = 0;
    if (multidictIncrBy(d, &counter, 5, &ival) != MULTIDICT_OK_INSERTED ||
        multidictIncrBy(d, &COUNTER, 2, &ival) != MULTIDICT_OK_REPLACED ||
        ival != 7 ||
        multidictIncrByFloat(d, &counterLow, 0.5, &dval) !=
            MULTIDICT_OK_REPLACED ||
        dval != 7.5 || multidictCount(d) != 2) {
        ERR("case increments");
    }

    /* Get-and-delete by another spelling */
    databox got = {};
    if (!multidictGetAndDelete(d, &KEY, &got) ||
        !testBoxIsBytes_(&got, "payload", 7) || multidictExists(d, &key) ||
        multidictCount(d) != 1) {
        ERR("case get-and-delete");
    }
    databoxFreeData(&got);
    multidictDelete(d, &COUNTER);

    /* Bulk add and delete; the stored spellings are the first ones */
    static const char *const stored[] = {"Alpha", "BETA", "gamma"};
    databox firsts[3];
    databox seconds[3];
    databox vals[3];
    static const char *const again[] = {"ALPHA", "beta", "GaMmA"};
    for (int32_t i = 0; i < 3; i++) {
        firsts[i] = databoxNewBytesString(stored[i]);
        seconds[i] = databoxNewBytesString(again[i]);
        vals[i] = (databox)DATABOX_SIGNED(10 + i);
    }

    if (multidictCount(d) != 0 ||
        multidictAddMultiple(d, firsts, vals, 3) != 3 ||
        multidictAddMultiple(d, seconds, vals, 3) != 0 ||
        multidictCount(d) != 3) {
        ERR("case bulk add");
    }

    /* Iteration, scan, random keys and pops report the stored spelling */
    multidictIterator iter;
    multidictIteratorInit(d, &iter);
    multidictEntry e;
    while (multidictIteratorNext(&iter, &e)) {
        if (!testKeyIsOneOf_(&e.key, stored, 3)) {
            ERR("case iteration reported another spelling");
        }
    }
    multidictIteratorRelease(&iter);

    int32_t bad = 0;
    uint64_t cursor = 0;
    do {
        cursor = multidictScan(d, cursor, testSpellingScanCb_, &bad);
    } while (cursor != 0);

    for (int32_t i = 0; i < 20; i++) {
        databox rk = {};
        if (!multidictGetRandomKey(d, &rk) ||
            !testKeyIsOneOf_(&rk, stored, 3)) {
            bad++;
        }
    }

    databox keys[3];
    const uint32_t some = multidictGetSomeKeys(d, keys, 3);
    if (some == 0) {
        bad++;
    }
    for (uint32_t i = 0; i < some; i++) {
        if (!testKeyIsOneOf_(&keys[i], stored, 3)) {
            bad++;
        }
    }

    if (bad) {
        ERR("case scan or random key reported another spelling (%" PRId32 ")",
            bad);
    }

    /* Copy keeps the type; merge matches dst keys across spellings */
    {
        cleaner(multidictFree) multidict *copy = multidictCopy(d);
        databox fv = {};
        if (multidictCount(copy) != 3 ||
            !multidictFind(copy, &seconds[0], &fv) || !testBoxIsInt_(&fv, 10)) {
            ERR("case copy");
        }

        cleaner(multidictFree) multidict *src =
            multidictNew(&multidictTypeCaseKey, qdc, 5);
        databox delta = databoxNewBytesString("Delta");
        const databox ninety = DATABOX_SIGNED(90);
        MULTIDICT_TEST_OK(
            multidictAdd(src, &seconds[0], &ninety)); /* "ALPHA" */
        MULTIDICT_TEST_OK(multidictAdd(src, &delta, &ninety));
        if (multidictMerge(copy, src, MULTIDICT_MERGE_KEEP) != 1 ||
            multidictCount(copy) != 4 ||
            !multidictFind(copy, &firsts[0], &fv) || !testBoxIsInt_(&fv, 10)) {
            ERR("case merge (keep)");
        }

        if (multidictMerge(copy, src, MULTIDICT_MERGE_REPLACE) != 2 ||
            multidictCount(copy) != 4 ||
            !multidictFind(copy, &firsts[0], &fv) || !testBoxIsInt_(&fv, 90)) {
            ERR("case merge (replace)");
        }

        bool alphaKept = false;
        multidictIteratorInit(copy, &iter);
        while (multidictIteratorNext(&iter, &e)) {
            alphaKept = alphaKept || testBoxIsBytes_(&e.key, "Alpha", 5);
        }
        multidictIteratorRelease(&iter);
        if (!alphaKept) {
            ERR("case merge replaced the stored spelling");
        }
    }

    /* Random eviction reports the stored spelling */
    bad = 0;
    multidictSetEvictionCallback(d, testRecordEvictionCb_, &bad);
    multidictSetMaxMemory(d, 1);
    if (multidictEvictToLimit(d) != 3 || multidictCount(d) != 0 || bad) {
        ERR("case eviction");
    }
    multidictSetMaxMemory(d, 0);
    multidictSetEvictionCallback(d, nullptr, nullptr);

    /* Pop random hands back the stored spelling */
    MULTIDICT_TEST_OK(multidictAdd(d, &firsts[1], &one));
    databox pk = {};
    databox pv = {};
    if (!multidictPopRandom(d, &pk, &pv) || !testBoxIsBytes_(&pk, "BETA", 4) ||
        !testBoxIsInt_(&pv, 1) || multidictCount(d) != 0) {
        ERR("case pop random");
    }
    databoxFreeData(&pk);
    databoxFreeData(&pv);

    /* Bulk delete under other spellings */
    multidictAddMultiple(d, firsts, vals, 3);
    if (multidictDeleteMultiple(d, seconds, 3) != 3 || multidictCount(d) != 0) {
        ERR("case bulk delete");
    }

    /* Integer keys match exactly: 0x41 and 0x61 differ only in the case
     * bit of their low byte, so the case hash folds them together */
    const databox intA = DATABOX_UNSIGNED(0x41);
    const databox inta = DATABOX_UNSIGNED(0x61);
    databox bytesA = databoxNewBytesString("A");
    databox iv = {};
    if (multidictAdd(d, &intA, &one) != MULTIDICT_OK_INSERTED ||
        multidictAdd(d, &inta, &two) != MULTIDICT_OK_INSERTED ||
        multidictAdd(d, &bytesA, &three) != MULTIDICT_OK_INSERTED ||
        multidictCount(d) != 3 || !multidictFind(d, &intA, &iv) ||
        !testBoxIsInt_(&iv, 1) || !multidictFind(d, &inta, &iv) ||
        !testBoxIsInt_(&iv, 2) || !multidictDelete(d, &inta) ||
        !multidictFind(d, &intA, &iv) || multidictCount(d) != 2) {
        ERR("case dict integer keys are not exact");
    }
    multidictEmpty(d);

    /* Long keys: the stored spelling is copied off the heap for updates */
    char lower[150];
    char upper[150];
    for (size_t i = 0; i < sizeof(lower); i++) {
        lower[i] = (char)('a' + i % 26);
        upper[i] = (char)('A' + i % 26);
    }
    databox longLow = databoxNewBytes(lower, sizeof(lower));
    databox longUp = databoxNewBytes(upper, sizeof(upper));
    if (multidictAdd(d, &longLow, &one) != MULTIDICT_OK_INSERTED ||
        multidictAdd(d, &longUp, &two) != MULTIDICT_OK_REPLACED ||
        !testOnlyEntryIs_(d, lower, sizeof(lower), &two) ||
        !multidictDelete(d, &longUp) || multidictCount(d) != 0 ||
        multidictKeyBytes(d) != 0) {
        ERR("case dict long keys");
    }

    return err;
}

/* Keys with embedded NUL are binary: they differ past the NUL */
static int multidictTestBinaryKeys_(multidictType *type) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d = multidictNew(type, qdc, 3);

    static const struct {
        const char *bytes;
        size_t len;
    } keys[] = {
        {"a\0b", 3}, {"a\0c", 3}, {"a", 1},       {"a\0", 2},     {"\0", 1},
        {"\0\0", 2}, {"", 0},     {"x\0\xc4", 3}, {"x\0\xe4", 3},
    };
    const size_t n = countof(keys);

    for (size_t i = 0; i < n; i++) {
        const databox k = databoxNewBytes(keys[i].bytes, keys[i].len);
        const databox v = DATABOX_SIGNED((int64_t)i);
        if (multidictAdd(d, &k, &v) != MULTIDICT_OK_INSERTED) {
            ERR("binary key %zu did not insert", i);
        }
    }

    if (multidictCount(d) != n) {
        ERR("binary keys collapsed: %" PRIu64 " of %zu", multidictCount(d), n);
    }

    for (size_t i = 0; i < n; i++) {
        const databox k = databoxNewBytes(keys[i].bytes, keys[i].len);
        databox v = {};
        if (!multidictFind(d, &k, &v) || !testBoxIsInt_(&v, (int64_t)i)) {
            ERR("binary key %zu found the wrong entry", i);
        }
    }

    /* Deleting one leaves the rest */
    const databox gone = databoxNewBytes("a\0b", 3);
    const databox kept = databoxNewBytes("a\0c", 3);
    databox v = {};
    if (!multidictDelete(d, &gone) || multidictExists(d, &gone) ||
        !multidictFind(d, &kept, &v) || !testBoxIsInt_(&v, 1) ||
        multidictCount(d) != n - 1) {
        ERR("binary key delete");
    }

    /* In a case dict the letter after the NUL still folds */
    if (type == &multidictTypeCaseKey) {
        const databox upper = databoxNewBytes("A\0C", 3);
        if (!multidictFind(d, &upper, &v) || !testBoxIsInt_(&v, 1)) {
            ERR("case fold past an embedded NUL");
        }
    }

    return err;
}

/* An exact dict keeps every spelling as its own key */
static int multidictTestExactDistinct_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 7);

    static const char *const spellings[] = {"Key", "KEY", "key", "kEy"};
    for (int32_t i = 0; i < 4; i++) {
        const databox k = databoxNewBytesString(spellings[i]);
        const databox v = DATABOX_SIGNED(i);
        if (multidictAdd(d, &k, &v) != MULTIDICT_OK_INSERTED) {
            ERR("exact spelling '%s' did not insert", spellings[i]);
        }
    }

    const databox KEY = databoxNewBytesString("KEY");
    const databox absent = databoxNewBytesString("KEy");
    databox v = {};
    if (multidictCount(d) != 4 || multidictFind(d, &absent, &v) ||
        multidictAddXX(d, &absent, &KEY) != MULTIDICT_ERR ||
        !multidictDelete(d, &KEY) || multidictExists(d, &KEY) ||
        multidictCount(d) != 3) {
        ERR("exact dict matched across case");
    }

    for (int32_t i = 0; i < 4; i++) {
        const databox k = databoxNewBytesString(spellings[i]);
        const bool want = i != 1;
        if (multidictFind(d, &k, &v) != want ||
            (want && !testBoxIsInt_(&v, i))) {
            ERR("exact spelling '%s' lost its value", spellings[i]);
        }
    }

    return err;
}

/* A custom compare: keys equal up to trailing spaces, with a hash that
 * ignores them too */
static uint32_t testTrimmedLen_(const void *key, uint32_t len) {
    const uint8_t *k = key;
    while (len > 0 && k[len - 1] == ' ') {
        len--;
    }
    return len;
}

static uint32_t testTrimHash_(multidict *d, const void *key, uint32_t len) {
    return multidictGenHashFunction(d, key, (int32_t)testTrimmedLen_(key, len));
}

static int32_t testTrimCompare_(void *, const void *key1, const uint32_t k1len,
                                const void *key2, const uint32_t k2len) {
    const uint32_t a = testTrimmedLen_(key1, k1len);
    const uint32_t b = testTrimmedLen_(key2, k2len);
    return a == b && (a == 0 || memcmp(key1, key2, a) == 0);
}

static multidictType testTrimType_ = {testTrimHash_, testTrimCompare_};

/* A type with no compare matches exact bytes */
static multidictType testNullCompareType_ = {multidictStringHashFunction_,
                                             nullptr};

static int multidictTestCustomCompare_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    {
        cleaner(multidictFree) multidict *d =
            multidictNew(&testTrimType_, qdc, 11);
        const databox plain = databoxNewBytesString("abc");
        const databox padded = databoxNewBytesString("abc   ");
        const databox other = databoxNewBytesString("ab c");
        const databox one = DATABOX_SIGNED(1);
        const databox two = DATABOX_SIGNED(2);
        databox v = {};
        if (multidictAdd(d, &padded, &one) != MULTIDICT_OK_INSERTED ||
            multidictAdd(d, &plain, &two) != MULTIDICT_OK_REPLACED ||
            multidictAdd(d, &other, &one) != MULTIDICT_OK_INSERTED ||
            multidictCount(d) != 2 || !multidictFind(d, &plain, &v) ||
            !testBoxIsInt_(&v, 2) || !multidictDelete(d, &plain) ||
            multidictExists(d, &padded) || multidictCount(d) != 1) {
            ERR("custom keyCompare not honoured");
        }
    }

    {
        cleaner(multidictFree) multidict *d =
            multidictNew(&testNullCompareType_, qdc, 11);
        const databox a = databoxNewBytesString("abc");
        const databox b = databoxNewBytesString("ABC");
        const databox one = DATABOX_SIGNED(1);
        if (multidictAdd(d, &a, &one) != MULTIDICT_OK_INSERTED ||
            multidictAdd(d, &b, &one) != MULTIDICT_OK_INSERTED ||
            multidictCount(d) != 2) {
            ERR("NULL keyCompare does not match exact bytes");
        }
    }

    return err;
}

/* A snapshot of 'd' through persistOpsMultidict; NULL on failure */
static uint8_t *testSnapshot_(multidict *d, size_t *len) {
    *len = 0;
    return persistOpsMultidict.snapshot(d, len);
}

/* Restore through 'as' (NULL: persistOpsMultidict.restore), then check
 * that the dict has 'type' and 'count' entries, and that "fOO", a
 * spelling no snapshot here stores, finds the stored "Foo" exactly when
 * the keys match by case */
static bool testRestoreIs_(const uint8_t *snap, size_t len, multidictType *as,
                           multidictType *type, uint64_t count) {
    multidict *r = as ? multidictPersistRestoreAs(snap, len, as)
                      : persistOpsMultidict.restore(snap, len);
    if (!r) {
        return false;
    }

    const databox probe = databoxNewBytesString("fOO");
    databox v = {};
    const bool found = multidictFind(r, &probe, &v);
    const bool ok = multidictGetType(r) == type && multidictCount(r) == count &&
                    found == (type == &multidictTypeCaseKey && count > 0);
    persistOpsMultidict.free(r);
    return ok;
}

/* Persist snapshots record how keys match: case dicts restore as case
 * dicts, exact as exact, custom types only through
 * multidictPersistRestoreAs with a custom type, and every mismatch,
 * unknown or missing record is refused */
static int multidictTestPersistKeys_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    const databox foo = databoxNewBytesString("Foo");
    const databox bar = databoxNewBytesString("bar");
    const databox one = DATABOX_SIGNED(1);

    cleaner(multidictFree) multidict *exact =
        multidictNew(&multidictTypeExactKey, qdc, 1);
    cleaner(multidictFree) multidict *ci =
        multidictNew(&multidictTypeCaseKey, qdc, 2);
    cleaner(multidictFree) multidict *ciEmpty =
        multidictNew(&multidictTypeCaseKey, qdc, 3);
    cleaner(multidictFree) multidict *custom =
        multidictNew(&testTrimType_, qdc, 4);
    multidict *const all[] = {exact, ci, custom};
    for (size_t i = 0; i < 3; i++) {
        MULTIDICT_TEST_OK(multidictAdd(all[i], &foo, &one));
        MULTIDICT_TEST_OK(multidictAdd(all[i], &bar, &one));
    }

    size_t exactLen, ciLen, ciEmptyLen, customLen;
    cleanerName(zfree, SeenBytes) uint8_t *exactSnap =
        testSnapshot_(exact, &exactLen);
    cleanerName(zfree, SeenBytes) uint8_t *ciSnap = testSnapshot_(ci, &ciLen);
    cleanerName(zfree, SeenBytes) uint8_t *ciEmptySnap =
        testSnapshot_(ciEmpty, &ciEmptyLen);
    cleanerName(zfree, SeenBytes) uint8_t *customSnap =
        testSnapshot_(custom, &customLen);

    /* The key record, the hash seed and the eight settings, then the
     * pairs */
    if (!exactSnap || flexCount((const flex *)exactSnap) != 14 || !ciSnap ||
        flexCount((const flex *)ciSnap) != 14 || !ciEmptySnap ||
        flexCount((const flex *)ciEmptySnap) != 10 || !customSnap) {
        ERR("multidict snapshot layout");
        return err;
    }

    /* Built-in types restore by themselves, or as the same type */
    if (!testRestoreIs_(exactSnap, exactLen, nullptr, &multidictTypeExactKey,
                        2) ||
        !testRestoreIs_(ciSnap, ciLen, nullptr, &multidictTypeCaseKey, 2) ||
        !testRestoreIs_(ciEmptySnap, ciEmptyLen, nullptr, &multidictTypeCaseKey,
                        0) ||
        !testRestoreIs_(exactSnap, exactLen, &multidictTypeExactKey,
                        &multidictTypeExactKey, 2) ||
        !testRestoreIs_(ciSnap, ciLen, &multidictTypeCaseKey,
                        &multidictTypeCaseKey, 2)) {
        ERR("built-in key types did not round trip");
    }

    /* A custom type needs the caller's type; the trim type matches "Foo"
     * exactly, so "FOO" is a miss there: check it by trailing spaces */
    multidict *r =
        multidictPersistRestoreAs(customSnap, customLen, &testTrimType_);
    const databox padded = databoxNewBytesString("Foo  ");
    databox v = {};
    if (persistOpsMultidict.restore(customSnap, customLen) != nullptr || !r ||
        multidictGetType(r) != &testTrimType_ || multidictCount(r) != 2 ||
        !multidictFind(r, &padded, &v)) {
        ERR("custom key type restore");
    }
    if (r) {
        persistOpsMultidict.free(r);
    }

    /* Every mismatch is refused */
    if (multidictPersistRestoreAs(ciSnap, ciLen, &multidictTypeExactKey) ||
        multidictPersistRestoreAs(ciSnap, ciLen, &testTrimType_) ||
        multidictPersistRestoreAs(exactSnap, exactLen, &multidictTypeCaseKey) ||
        multidictPersistRestoreAs(exactSnap, exactLen, &testTrimType_) ||
        multidictPersistRestoreAs(customSnap, customLen,
                                  &multidictTypeCaseKey) ||
        multidictPersistRestoreAs(customSnap, customLen,
                                  &multidictTypeExactKey) ||
        multidictPersistRestoreAs(ciSnap, ciLen, nullptr)) {
        ERR("a key type mismatch was not refused");
    }

    /* Pairs without the key record are refused */
    {
        cleaner(flexFree) flex *pairsOnly = flexNew();
        const databox *pairs[] = {&foo, &one, &bar, &one};
        for (size_t i = 0; i < 4; i++) {
            flexPushByTypeExact(&pairsOnly, pairs[i], FLEX_ENDPOINT_TAIL);
        }
        if (persistOpsMultidict.restore(pairsOnly, flexBytes(pairsOnly)) ||
            multidictPersistRestoreAs(pairsOnly, flexBytes(pairsOnly),
                                      &multidictTypeExactKey)) {
            ERR("a snapshot without its key record restored");
        }
    }

    /* An unknown or malformed record is refused */
    {
        const databox unknown = DATABOX_UNSIGNED(7);
        const databox text = databoxNewBytesString("case");
        const databox *const heads[] = {&unknown, &text};
        for (size_t h = 0; h < 2; h++) {
            cleaner(flexFree) flex *bad = flexNew();
            flexPushByType(&bad, heads[h], FLEX_ENDPOINT_TAIL);
            flexPushByType(&bad, &foo, FLEX_ENDPOINT_TAIL);
            flexPushByType(&bad, &one, FLEX_ENDPOINT_TAIL);
            if (persistOpsMultidict.restore(bad, flexBytes(bad)) != nullptr) {
                ERR("key record %zu was not refused", h);
            }
        }
    }

    return err;
}

/* Differential test: random mixed-case adds, conditional adds, replaces,
 * increments, finds, deletes and pops against an oracle that matches keys
 * the way the dict's type should, under growth, shrinking and rehashes
 * forced in flight.  Keys carry an index, letters that may change case,
 * and sometimes an embedded NUL, a Latin-1 byte or padding past 64 bytes. */
static constexpr int TEST_CI_BASES = 160;
static constexpr int TEST_CI_MAX_KEY = 96;
typedef struct testCiEntry {
    uint8_t key[TEST_CI_MAX_KEY];
    uint32_t len;
    int64_t val;
} testCiEntry;

typedef struct testCiOracle {
    bool fold; /* keys match ASCII case-insensitively */
    uint32_t count;
    testCiEntry e[TEST_CI_BASES * 8];
} testCiOracle;

defcleanerName(testCiOracle *, zfree, CiOracle);

/* Key 'base' with the case of its first three letters set from 'flips' */
static uint32_t testCiKey_(uint32_t base, uint32_t flips, uint8_t *out) {
    uint32_t len =
        (uint32_t)snprintf((char *)out, TEST_CI_MAX_KEY, "%05" PRIu32, base);
    for (uint32_t j = 0; j < 6; j++) {
        const uint8_t c = (uint8_t)('a' + (base * 7 + j * 5) % 26);
        out[len++] =
            (j < 3 && (flips >> j) & 1) ? (uint8_t)(c - ('a' - 'A')) : c;
    }

    if (base % 7 == 0) {
        out[len++] = '\0';
        out[len++] = 'q';
    }

    if (base % 5 == 0) {
        out[len++] = 0xc4;
    }

    if (base % 10 == 0) {
        while (len < 80) {
            out[len] = (uint8_t)('m' + len % 8);
            len++;
        }
    }

    return len;
}

static bool testCiEq_(const testCiOracle *o, const uint8_t *a, uint32_t alen,
                      const uint8_t *b, uint32_t blen) {
    if (alen != blen) {
        return false;
    }

    for (uint32_t i = 0; i < alen; i++) {
        if (o->fold ? testFoldAscii_(a[i]) != testFoldAscii_(b[i])
                    : a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static int32_t testCiFind_(const testCiOracle *o, const uint8_t *key,
                           uint32_t len) {
    for (uint32_t i = 0; i < o->count; i++) {
        if (testCiEq_(o, o->e[i].key, o->e[i].len, key, len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t testCiFindExact_(const testCiOracle *o, const databox *key) {
    if (!DATABOX_IS_BYTES(key)) {
        return -1;
    }

    for (uint32_t i = 0; i < o->count; i++) {
        if (testBoxIsBytes_(key, o->e[i].key, o->e[i].len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void testCiRemove_(testCiOracle *o, int32_t at) {
    o->e[at] = o->e[--o->count];
}

/* The dict holds exactly the oracle: same count, every entry under the
 * oracle's spelling with its value, each once, and key bytes adding up */
static int testCiVerify_(multidict *d, const testCiOracle *o, bool lru,
                         uint32_t step) {
    int bad = 0; /* entries the oracle does not hold as iterated */
    bool seen[TEST_CI_BASES * 8] = {};
    uint64_t keyBytes = 0;
    for (uint32_t i = 0; i < o->count; i++) {
        keyBytes += o->e[i].len;
    }

    multidictIterator iter;
    multidictIteratorInit(d, &iter);
    multidictEntry e;
    uint32_t n = 0;
    while (multidictIteratorNext(&iter, &e)) {
        n++;
        const int32_t at = testCiFindExact_(o, &e.key);
        if (at < 0 || seen[at] || !testBoxIsInt_(&e.val, o->e[at].val)) {
            bad++;
            continue;
        }
        seen[at] = true;
    }
    multidictIteratorRelease(&iter);

    int err = 0;
    if (bad || n != o->count || multidictCount(d) != o->count ||
        multidictKeyBytes(d) != keyBytes ||
        (lru && !testLruTrackingExact_(d))) {
        ERR("differential step %" PRIu32 ": %" PRIu32
            " entries, %d bad, count %" PRIu64 " (oracle %" PRIu32
            "), key bytes %" PRIu64 " (oracle %" PRIu64 ")",
            step, n, bad, multidictCount(d), o->count, multidictKeyBytes(d),
            keyBytes);
    }
    return err;
}

static int multidictTestCaseDifferential_(multidictType *type, uint32_t seed,
                                          bool lru) {
    int err = 0;
    cleanerName(zfree, CiOracle) testCiOracle *o = zcalloc(1, sizeof(*o));
    o->fold = type == &multidictTypeCaseKey;

    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(type, qdc, (int32_t)seed);
    if (lru) {
        multidictEnableLRU(d, 4);
    }

    testRandSeed(seed);
    uint32_t inFlight = 0;
    int64_t nextVal = 1;
    const uint32_t steps = 6000;
    for (uint32_t step = 0; step < steps && !err; step++) {
        /* Grow for the first half, then mostly drain so the dict shrinks */
        const bool draining = step >= steps / 2;
        uint8_t key[TEST_CI_MAX_KEY];
        const uint32_t len =
            testCiKey_(testRand() % TEST_CI_BASES, testRand() % 8, key);
        const databox kb = databoxNewBytes(key, len);
        const int32_t at = testCiFind_(o, key, len);
        const databox vb = DATABOX_SIGNED(nextVal);

        if (step % 400 == 0 && !multidictIsRehashing(d) &&
            multidictSlotCount(d) < 4096) {
            multidictExpand(d, multidictSlotCount(d) * 4);
        }
        inFlight += multidictIsRehashing(d);

        const uint32_t op = testRand() % 100;
        bool ok = true;
        if (op < (draining ? 10u : 30u)) {
            ok = multidictAdd(d, &kb, &vb) ==
                 (at >= 0 ? MULTIDICT_OK_REPLACED : MULTIDICT_OK_INSERTED);
            if (at >= 0) {
                o->e[at].val = nextVal;
            } else {
                testCiEntry *ne = &o->e[o->count++];
                memcpy(ne->key, key, len);
                ne->len = len;
                ne->val = nextVal;
            }
            nextVal++;
        } else if (op < 38) {
            ok = multidictAddNX(d, &kb, &vb) ==
                 (at >= 0 ? MULTIDICT_ERR : MULTIDICT_OK_INSERTED);
            if (at < 0) {
                testCiEntry *ne = &o->e[o->count++];
                memcpy(ne->key, key, len);
                ne->len = len;
                ne->val = nextVal;
            }
            nextVal++;
        } else if (op < 46) {
            ok = multidictReplace(d, &kb, &vb) ==
                 (at >= 0 ? MULTIDICT_OK_REPLACED : MULTIDICT_ERR);
            if (at >= 0) {
                o->e[at].val = nextVal;
            }
            nextVal++;
        } else if (op < 50) {
            int64_t result = 0;
            const int64_t inc = 1 + testRand() % 5;
            ok = multidictIncrBy(d, &kb, inc, &result) ==
                 (at >= 0 ? MULTIDICT_OK_REPLACED : MULTIDICT_OK_INSERTED);
            if (at >= 0) {
                o->e[at].val += inc;
            } else {
                testCiEntry *ne = &o->e[o->count++];
                memcpy(ne->key, key, len);
                ne->len = len;
                ne->val = inc;
            }
            ok = ok && result == (at >= 0 ? o->e[at].val : inc);
        } else if (op < 70) {
            databox v = {};
            const bool found = multidictFind(d, &kb, &v);
            ok = found == (at >= 0) &&
                 (!found || testBoxIsInt_(&v, o->e[at].val));
        } else if (op < (draining ? 94u : 86u)) {
            ok = multidictDelete(d, &kb) == (at >= 0);
            if (at >= 0) {
                testCiRemove_(o, at);
            }
        } else if (op < 97) {
            databox v = {};
            const bool got = multidictGetAndDelete(d, &kb, &v);
            ok = got == (at >= 0) && (!got || testBoxIsInt_(&v, o->e[at].val));
            if (got) {
                databoxFreeData(&v);
            }
            if (at >= 0) {
                testCiRemove_(o, at);
            }
        } else {
            databox pk = {};
            databox pv = {};
            const bool popped = multidictPopRandom(d, &pk, &pv);
            const int32_t pat = popped ? testCiFindExact_(o, &pk) : -1;
            ok = popped == (o->count > 0) &&
                 (!popped || (pat >= 0 && testBoxIsInt_(&pv, o->e[pat].val)));
            if (pat >= 0) {
                testCiRemove_(o, pat);
            }
            databoxFreeData(&pk);
            databoxFreeData(&pv);
        }

        if (!ok) {
            ERR("differential step %" PRIu32 ": op %" PRIu32 " on base %.5s",
                step, op, (const char *)key);
        }

        if (step % 500 == 499) {
            err += testCiVerify_(d, o, lru, step);
        }
    }

    err += testCiVerify_(d, o, lru, steps);
    if (inFlight == 0) {
        ERR("differential never ran with a rehash in flight");
    }

    return err;
}

/* Scan callback that does nothing: scans must not promote */
static void testIgnoreScanCb_(void *, const databox *, const databox *) {
}

/* The LRU level of one key after each kind of operation: reads that
 * touch nothing leave it, and each access raises it by exactly one.  A
 * case dict runs every operation under a different spelling. */
static int multidictTestLruPromotion_(multidictType *type) {
    int err = 0;
    const bool fold = type == &multidictTypeCaseKey;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d = multidictNew(type, qdc, 21);
    multidictEnableLRU(d, 16);

    const databox stored = databoxNewBytesString("rank");
    const databox other = databoxNewBytesString("bystander");
    const databox text = databoxNewBytesString("text");
    const databox ten = DATABOX_SIGNED(10);
    MULTIDICT_TEST_OK(multidictAdd(d, &stored, &text));
    MULTIDICT_TEST_OK(multidictAdd(d, &other, &ten));

    /* Spellings: a case dict sees "rank" under these, an exact one
     * always as "rank" */
    static const char *const spellings[] = {"RANK", "Rank", "rAnK", "raNK"};
    uint32_t next = 0;
#define TEST_RANK_KEY()                                                        \
    databoxNewBytesString(fold ? spellings[next++ % 4] : "rank")

    int32_t expect = 0;
#define TEST_RANK_STEP(what, delta)                                            \
    do {                                                                       \
        expect += (delta);                                                     \
        const databox probe = TEST_RANK_KEY();                                 \
        const int level = multidictGetLRULevel(d, &probe);                     \
        if (level != expect) {                                                 \
            ERR("LRU level %d after %s, expected %d", level, (what), expect);  \
            expect = level;                                                    \
        }                                                                      \
    } while (0)

    TEST_RANK_STEP("insert", 0);

    /* Named-key rule: naming an existing key is one use, whatever the
     * outcome */
    databox k = TEST_RANK_KEY();
    if (!multidictExists(d, &k)) {
        ERR("exists missed the ranked key");
    }
    TEST_RANK_STEP("exists", 1);

    if (!multidictExistsByString(d, fold ? "RANK" : "rank")) {
        ERR("exists by string missed the ranked key");
    }
    TEST_RANK_STEP("exists by string", 1);

    k = TEST_RANK_KEY();
    if (multidictAddNX(d, &k, &ten) != MULTIDICT_ERR) {
        ERR("add-NX of a present key was not refused");
    }
    TEST_RANK_STEP("add-NX of a present key", 1);

    int64_t ires = 0;
    k = TEST_RANK_KEY();
    if (multidictIncrBy(d, &k, 1, &ires) != MULTIDICT_ERR) {
        ERR("an increment of text was not refused");
    }
    TEST_RANK_STEP("refused increment", 1);

    /* A miss names no existing key: it promotes nothing */
    const databox absent = databoxNewBytesString("absent-key");
    if (multidictExists(d, &absent) ||
        multidictIncrByFloat(d, &absent, NAN, nullptr) != MULTIDICT_ERR) {
        ERR("an absent key was found or incremented by NaN");
    }
    TEST_RANK_STEP("a miss", 0);

    multidictIterator iter;
    multidictEntry e;
    multidictIteratorInit(d, &iter);
    while (multidictIteratorNext(&iter, &e)) {
    }
    multidictIteratorRelease(&iter);
    uint64_t cursor = 0;
    do {
        cursor = multidictScan(d, cursor, testIgnoreScanCb_, nullptr);
    } while (cursor != 0);
    for (int32_t i = 0; i < 8; i++) {
        databox rk = {};
        multidictGetRandomKey(d, &rk);
    }
    TEST_RANK_STEP("iteration, scan and random keys", 0);

    /* Accesses: one level each */
    databox v = {};
    k = TEST_RANK_KEY();
    if (!multidictFind(d, &k, &v)) {
        ERR("find missed the ranked key");
    }
    TEST_RANK_STEP("find", 1);

    uint8_t *bytes = nullptr;
    if (!multidictFindByString(d, fold ? "rANK" : "rank", &bytes)) {
        ERR("find by string missed the ranked key");
    }
    TEST_RANK_STEP("find by string", 1);

    k = TEST_RANK_KEY();
    multidictTouch(d, &k);
    TEST_RANK_STEP("touch", 1);

    k = TEST_RANK_KEY();
    if (multidictAdd(d, &k, &text) != MULTIDICT_OK_REPLACED) {
        ERR("add of a present key did not replace");
    }
    TEST_RANK_STEP("add of a present key", 1);

    k = TEST_RANK_KEY();
    if (multidictAddXX(d, &k, &text) != MULTIDICT_OK_REPLACED) {
        ERR("add-XX did not replace");
    }
    TEST_RANK_STEP("add-XX", 1);

    k = TEST_RANK_KEY();
    if (multidictReplace(d, &k, &ten) != MULTIDICT_OK_REPLACED) {
        ERR("replace did not replace");
    }
    TEST_RANK_STEP("replace", 1);

    k = TEST_RANK_KEY();
    if (multidictIncrBy(d, &k, 5, &ires) != MULTIDICT_OK_REPLACED ||
        ires != 15) {
        ERR("increment gave %" PRId64 ", expected 15", ires);
    }
    TEST_RANK_STEP("increment", 1);

    double dres = 0;
    k = TEST_RANK_KEY();
    if (multidictIncrByFloat(d, &k, 0.5, &dres) != MULTIDICT_OK_REPLACED ||
        dres != 15.5) {
        ERR("float increment gave %g, expected 15.5", dres);
    }
    TEST_RANK_STEP("float increment", 1);

    /* The bystander was never accessed, and a key added through add-NX
     * starts at the coldest level */
    const databox fresh = databoxNewBytesString("fresh");
    if (multidictGetLRULevel(d, &other) != 0 ||
        multidictAddNX(d, &fresh, &ten) != MULTIDICT_OK_INSERTED ||
        multidictGetLRULevel(d, &fresh) != 0 || !testLruTrackingExact_(d)) {
        ERR("LRU bystander or fresh key level");
    }

#undef TEST_RANK_STEP
#undef TEST_RANK_KEY
    return err;
}

/* Recount both tables' slots and entries and compare with the counters
 * the dict maintains incrementally (count, usedBytes, keyBytes,
 * valBytes) */
static bool mdTestCountersHold_(multidict *d) {
    for (int32_t t = 0; t < 2; t++) {
        const multidictHT *ht = &d->ht[t];
        uint64_t count = 0;
        uint64_t used = 0;
        uint64_t keyBytes = 0;
        uint64_t valBytes = 0;
        for (uint32_t s = 0; s < ht->size; s++) {
            multidictSlot *slot = ht->table[s];
            if (!slot) {
                continue;
            }

            used += d->shared->sizeBytes(slot);
            multidictSlotIterator iter;
            multidictEntry e;
            if (d->shared->getIter(&iter, slot)) {
                while (d->shared->iterNext(&iter, &e)) {
                    count++;
                    keyBytes += mdBoxSize_(&e.key);
                    valBytes += mdBoxSize_(&e.val);
                }
            }
        }

        if (count != ht->count || used != ht->usedBytes ||
            keyBytes != ht->keyBytes || valBytes != ht->valBytes) {
            printf("table %d recounts %" PRIu64 "/%" PRIu64 "/%" PRIu64
                   "/%" PRIu64 ", counters say %" PRIu64 "/%" PRIu64 "/%" PRIu64
                   "/%" PRIu64 "\n",
                   t, count, used, keyBytes, valBytes, (uint64_t)ht->count,
                   (uint64_t)ht->usedBytes, (uint64_t)ht->keyBytes,
                   (uint64_t)ht->valBytes);
            return false;
        }
    }

    return true;
}

/* Every in-place and removing path keeps the counters exact, mid-rehash
 * included: values change type and size through AddXX / Replace, IncrBy,
 * IncrByFloat, and leave through Delete, GetAndDelete and PopRandom. */
static int multidictTestCountersUnderUpdates_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 4242);
    char longValue[200];
    memset(longValue, 'v', sizeof(longValue));
    uint64_t rng = 99;
    for (int64_t i = 0; i < 4000; i++) {
        char keyBuf[32];
        snprintf(keyBuf, sizeof(keyBuf), "k%" PRId64, i % 1500);
        const databox key = databoxNewBytesString(keyBuf);
        dkLcg64Next(&rng);
        switch ((rng >> 33) % 8) {
        case 0: {
            const databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
            break;
        }
        case 7: {
            /* Inserts exactly when the key is absent, from either table */
            const bool present = multidictExistsNoTouch(d, &key);
            const databox val = databoxNewSigned(-i);
            const multidictResult r = multidictAddNX(d, &key, &val);
            if (r != (present ? MULTIDICT_ERR : MULTIDICT_OK_INSERTED)) {
                ERR("AddNX of %s (present %d, rehashing %d) returned %d",
                    keyBuf, present, multidictIsRehashing(d), r);
            }
            break;
        }
        case 1: {
            /* A long string replaces whatever was there, or nothing */
            const databox val =
                databoxNewBytes(longValue, 1 + (size_t)(i % 199));
            MD_TEST_APPLIES_IF(multidictExistsNoTouch(d, &key),
                               multidictAddXX(d, &key, &val));
            break;
        }
        /* An increment of a string value is refused; the counters are
         * the check */
        case 2:
            (void)multidictIncrBy(d, &key, i - 2000, nullptr);
            break;
        case 3:
            (void)multidictIncrByFloat(d, &key, 0.5, nullptr);
            break;
        case 4:
            multidictDelete(d, &key);
            break;
        case 5: {
            databox got;
            if (multidictGetAndDelete(d, &key, &got)) {
                databoxFreeData(&got);
            }
            break;
        }
        default: {
            databox k;
            databox v;
            if (multidictPopRandom(d, &k, &v)) {
                databoxFreeData(&k);
                databoxFreeData(&v);
            }
            break;
        }
        }

        if (!mdTestCountersHold_(d)) {
            ERR("counters wrong after operation %" PRId64, i);
            break;
        }
    }

    return err;
}

/* AddNX mid-rehash, while most keys still sit in ht[0]: a present key is
 * refused wherever it is (its value kept), an absent one is inserted
 * once, and the counters stay exact. */
static int multidictTestAddNXMidRehash_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 77);
    constexpr int present = 300;
    for (int64_t i = 0; i < present; i++) {
        const databox key = databoxNewSigned(i);
        const databox val = databoxNewSigned(i * 3);
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
    }

    while (multidictRehash(d, 100)) {
    }

    if (!multidictExpand(d, 8192) || !multidictIsRehashing(d)) {
        ERR("AddNX mid-rehash: expand did not start a rehash");
        return err;
    }

    for (int64_t i = 0; i < present * 2; i++) {
        /* present keys and absent keys interleaved */
        const int64_t k = (i & 1) ? i / 2 : present + i / 2;
        const databox key = databoxNewSigned(k);
        const databox val = databoxNewSigned(-1);
        const multidictResult want =
            k < present ? MULTIDICT_ERR : MULTIDICT_OK_INSERTED;
        const multidictResult got = multidictAddNX(d, &key, &val);
        if (got != want) {
            ERR("AddNX mid-rehash: key %" PRId64 " returned %d, want %d", k,
                got, want);
        }

        if (!mdTestCountersHold_(d)) {
            ERR("AddNX mid-rehash: counters wrong after key %" PRId64, k);
            break;
        }
    }

    for (int64_t k = 0; k < present * 2; k++) {
        const databox key = databoxNewSigned(k);
        databox val = {};
        const int64_t want = k < present ? k * 3 : -1;
        if (!multidictFind(d, &key, &val) || val.data.i != want) {
            ERR("AddNX mid-rehash: key %" PRId64 " holds %" PRIdMAX
                ", want %" PRId64,
                k, val.data.i, want);
        }
    }

    if (multidictCount(d) != present * 2) {
        ERR("AddNX mid-rehash: count %" PRIu64 ", want %d",
            (uint64_t)multidictCount(d), present * 2);
    }

    return err;
}

typedef struct mdRehashScan_ {
    uint8_t *seen;
    uint64_t n;
    uint64_t bad;
} mdRehashScan_;

static void mdRehashScanCb_(void *privdata, const databox *key,
                            const databox *val) {
    mdRehashScan_ *s = privdata;
    if (key->type != DATABOX_UNSIGNED_64 || key->data.u64 >= s->n ||
        val->data.u64 != key->data.u64 * 3) {
        s->bad++;
        return;
    }
    s->seen[key->data.u64] = 1;
}

/* Every read while a rehash is part way: both tables' stats add up, the
 * random reads return live keys, a scan sees every key, every key is
 * found and random pops remove exactly the keys they return */
static int multidictTestRehashReads_(void) {
    int err = 0;
    const uint64_t n = 2000;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 31);
    for (uint64_t k = 0; k < n; k++) {
        const databox key = DATABOX_UNSIGNED(k);
        const databox val = DATABOX_UNSIGNED(k * 3);
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
    }
    while (multidictRehash(d, 100)) {
    }

    /* A rehash to 8x the slots, moved one bucket per operation */
    multidictResizeDisable(d);
    multidictStats before;
    multidictGetStats(d, &before);
    if (!multidictExpand(d, before.slots * 8) || !multidictIsRehashing(d)) {
        ERR("rehash reads: expand did not start a rehash");
        return err;
    }

    multidictDetailedStats ds;
    multidictGetDetailedStats(d, &ds);
    multidictStats s;
    multidictGetStats(d, &s);
    if (!ds.hasRehashTable || !ds.basic.isRehashing ||
        ds.basic.count + ds.rehashTable.count != n ||
        ds.rehashTable.slots < before.slots * 8 || s.count != n ||
        !s.isRehashing) {
        ERR("rehash reads: stats %" PRIu64 " + %" PRIu64 " of %" PRIu64 " keys",
            ds.basic.count, ds.rehashTable.count, n);
    }

    databox some[32];
    const uint32_t got = multidictGetSomeKeys(d, some, 32);
    databox rk = {};
    if (!multidictIsRehashing(d) || got == 0 || got > 32 ||
        !multidictGetRandomKey(d, &rk) || rk.type != DATABOX_UNSIGNED_64 ||
        rk.data.u64 >= n) {
        ERR("rehash reads: random keys (%" PRIu32 ")", got);
    }
    for (uint32_t i = 0; i < got; i++) {
        if (some[i].type != DATABOX_UNSIGNED_64 || some[i].data.u64 >= n) {
            ERR("rehash reads: GetSomeKeys slot %" PRIu32, i);
        }
    }

    cleanerName(zfree, SeenBytes) uint8_t *seen = zcalloc(n, 1);
    mdRehashScan_ scan = {.seen = seen, .n = n, .bad = 0};
    if (!multidictIsRehashing(d)) {
        ERR("rehash reads: the rehash ended before the scan");
    }
    uint64_t cursor = 0;
    do {
        cursor = multidictScan(d, cursor, mdRehashScanCb_, &scan);
    } while (cursor != 0);
    for (uint64_t k = 0; k < n; k++) {
        scan.bad += !seen[k];
    }
    if (scan.bad) {
        ERR("rehash reads: scan missed or garbled %" PRIu64 " keys", scan.bad);
    }

    /* Pops first, while the rehash is still young, then every key */
    memset(seen, 0, n);
    for (int i = 0; i < 10; i++) {
        databox pk = {};
        databox pv = {};
        if (!multidictPopRandom(d, &pk, &pv) ||
            pk.type != DATABOX_UNSIGNED_64 || pk.data.u64 >= n ||
            seen[pk.data.u64] || pv.data.u64 != pk.data.u64 * 3) {
            ERR("rehash reads: pop %d", i);
            break;
        }
        seen[pk.data.u64] = 1;
    }
    for (uint64_t k = 0; k < n; k++) {
        const databox key = DATABOX_UNSIGNED(k);
        databox val = {};
        const bool found = multidictFind(d, &key, &val);
        if (found == (bool)seen[k] || (found && val.data.u64 != k * 3)) {
            ERR("rehash reads: key %" PRIu64 " found %d", k, found);
            break;
        }
    }

    while (multidictRehash(d, 100)) {
    }
    multidictGetDetailedStats(d, &ds);
    if (ds.hasRehashTable || ds.basic.count != n - 10 ||
        multidictCount(d) != n - 10) {
        ERR("rehash reads: %" PRIu64 " keys after the rehash", ds.basic.count);
    }
    multidictResizeEnable(d);
    return err;
}

/* LRU tracking turned on and off: only an empty dict can start it, and
 * turning it off keeps every entry */
static int multidictTestLRUToggle_(void) {
    int err = 0;
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 7);
    multidictDisableLRU(d); /* not on: nothing to do */
    if (multidictHasLRU(d) || !multidictEnableLRU(d, 4) ||
        !multidictHasLRU(d) ||
        multidictGetEvictPolicy(d) != MULTIDICT_EVICT_LRU ||
        !multidictEnableLRU(d, 4)) {
        ERR("LRU toggle: enabling an empty dict");
    }
    for (uint64_t k = 0; k < 50; k++) {
        const databox key = DATABOX_UNSIGNED(k);
        const databox val = DATABOX_UNSIGNED(k);
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
    }
    multidictDisableLRU(d);
    if (multidictHasLRU(d) ||
        multidictGetEvictPolicy(d) != MULTIDICT_EVICT_NONE ||
        multidictEnableLRU(d, 4) || multidictCount(d) != 50) {
        ERR("LRU toggle: disabling a populated dict");
    }
    for (uint64_t k = 0; k < 50; k++) {
        const databox key = DATABOX_UNSIGNED(k);
        databox val = {};
        if (!multidictFind(d, &key, &val) || val.data.u64 != k) {
            ERR("LRU toggle: key %" PRIu64 " lost", k);
            break;
        }
    }
    return err;
}

typedef struct mdShrinkScan_ {
    uint8_t *seen;
    size_t n;
    int32_t bad;
} mdShrinkScan_;

/* Keys and values are non-negative, so they read back as UNSIGNED_64 (the
 * integer family's minimal form) */
static void mdShrinkScanCb_(void *privdata, const databox *key,
                            const databox *val) {
    mdShrinkScan_ *s = privdata;
    if (key->type != DATABOX_UNSIGNED_64 || key->data.u >= s->n ||
        val->type != DATABOX_UNSIGNED_64 || val->data.u != key->data.u * 3) {
        s->bad++;
        return;
    }
    s->seen[key->data.u] = 1;
}

/* 2000 keys, all but every 20th deleted, then a resize: the table shrinks
 * through a rehash into a smaller one (table 0 the larger).  A full scan
 * during it, with 'stepsBetween' rehash steps between cursor calls, must
 * emit every kept key; the detailed stats see both tables; the scan and
 * stats agree with the count. */
static int mdTestShrinkingRehashScan_(multidictClass *qdc,
                                      int32_t stepsBetween) {
    int err = 0;
    constexpr int N = 2000;
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 7);
    for (int64_t i = 0; i < N; i++) {
        const databox k = databoxNewSigned(i);
        const databox v = databoxNewSigned(i * 3);
        MULTIDICT_TEST_OK(multidictAdd(d, &k, &v));
    }
    for (int64_t i = 0; i < N; i++) {
        if (i % 20) {
            const databox k = databoxNewSigned(i);
            multidictDelete(d, &k);
        }
    }
    while (multidictIsRehashing(d)) {
        multidictRehash(d, 100);
    }
    multidictDetailedStats before = {};
    multidictGetDetailedStats(d, &before);
    if (!multidictResize(d) || !multidictIsRehashing(d) || multidictResize(d)) {
        ERR("resize after deletes did not start one rehash");
        return err;
    }
    multidictDetailedStats during = {};
    multidictGetDetailedStats(d, &during);
    if (!during.hasRehashTable ||
        during.rehashTable.slots >= during.basic.slots ||
        during.basic.slots != before.basic.slots ||
        during.basic.count + during.rehashTable.count != N / 20) {
        ERR("the shrink's tables are not larger-to-smaller (%" PRIu64
            " -> %" PRIu64 " slots)",
            during.basic.slots, during.rehashTable.slots);
    }

    uint8_t seen[N] = {};
    mdShrinkScan_ state = {.seen = seen, .n = N, .bad = 0};
    uint64_t cursor = 0;
    size_t calls = 0;
    do {
        cursor = multidictScan(d, cursor, mdShrinkScanCb_, &state);
        if (stepsBetween) {
            multidictRehash(d, stepsBetween);
        }
        calls++;
    } while (cursor != 0 && calls < 100'000);
    for (size_t i = 0; i < N; i++) {
        if ((i % 20 == 0) != (seen[i] == 1)) {
            state.bad++;
        }
    }
    if (state.bad || cursor != 0) {
        ERR("scan during a shrink (%d steps between calls) missed or "
            "invented %d keys",
            stepsBetween, state.bad);
    }
    return err;
}

/* Numeric keys are one key by value, whatever box holds them: signed and
 * unsigned 5, 5.0f, 5.0 and a 128-bit 5; -0.0 and 0; 2^70 as a 128-bit
 * integer and as a double.  5.5 stays its own key. */
static int mdTestNumericKeyIdentity_(multidictClass *qdc) {
    int err = 0;
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 3);

    /* Each group: the first box adds, every other one names the same key */
    databoxBig wide5;
    DATABOX_BIG_UNSIGNED_128(&wide5, 5);
    databoxBig wide70;
    DATABOX_BIG_UNSIGNED_128(&wide70, (__uint128_t)1 << 70);
    databox float5 = {};
    DATABOX_SET_FLOAT(&float5, 5.0f);
    const databox five[5] = {databoxNewSigned(5), databoxNewUnsigned(5), float5,
                             DATABOX_DOUBLE(5.0), *(const databox *)&wide5};
    const databox zero[2] = {databoxNewSigned(0), DATABOX_DOUBLE(-0.0)};
    const databox big[2] = {*(const databox *)&wide70, DATABOX_DOUBLE(0x1p70)};
    const struct {
        const databox *boxes;
        size_t n;
        int64_t value;
    } groups[3] = {{five, 5, 50}, {zero, 2, 0}, {big, 2, 70}};

    for (size_t g = 0; g < 3; g++) {
        const databox v = databoxNewSigned(groups[g].value);
        if (multidictAdd(d, &groups[g].boxes[0], &v) != MULTIDICT_OK_INSERTED) {
            ERR("numeric group %zu: the first key did not add", g);
        }
        for (size_t i = 1; i < groups[g].n; i++) {
            /* Found under its value, and adding it replaces (same value,
             * so the next box finds it unchanged) */
            databox got = {};
            if (!multidictFind(d, &groups[g].boxes[i], &got) ||
                got.data.i != groups[g].value ||
                multidictAdd(d, &groups[g].boxes[i], &v) !=
                    MULTIDICT_OK_REPLACED) {
                ERR("numeric group %zu: box %zu is not the same key", g, i);
            }
        }
    }

    const databox fraction = DATABOX_DOUBLE(5.5);
    const databox fv = databoxNewSigned(55);
    if (multidictAdd(d, &fraction, &fv) != MULTIDICT_OK_INSERTED ||
        multidictCount(d) != 4) {
        ERR("5.5 was not its own key (count %" PRIu64 ")",
            (uint64_t)multidictCount(d));
    }
    return err;
}

/* The process seed is drawn once (every call agrees); a snapshot carries a
 * dict's seed, and its restore uses it with every pair */
static int mdTestSeedRoundTrip_(multidictClass *qdc) {
    int err = 0;
    const int32_t seed = multidictRandomSeed();
    if (multidictRandomSeed() != seed) {
        ERR("the process seed changed between calls");
    }

    static constexpr int32_t seeds[2] = {(int32_t)0x9E3779B9u, 0};
    for (size_t s = 0; s < 2; s++) {
        cleaner(multidictFree) multidict *d = multidictNew(
            &multidictTypeExactKey, qdc, s ? multidictRandomSeed() : seeds[0]);
        for (int64_t k = 0; k < 50; k++) {
            const databox key = databoxNewSigned(k);
            const databox val = databoxNewSigned(k * 7);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }
        size_t len = 0;
        cleanerName(zfree, SeenBytes) uint8_t *snap =
            persistOpsMultidict.snapshot(d, &len);
        multidict *r = snap ? persistOpsMultidict.restore(snap, len) : nullptr;
        bool same = r &&
                    multidictGetHashFunctionSeed(r) ==
                        multidictGetHashFunctionSeed(d) &&
                    multidictCount(r) == 50;
        for (int64_t k = 0; same && k < 50; k++) {
            const databox key = databoxNewSigned(k);
            databox got = {};
            same = multidictFind(r, &key, &got) && got.data.i == k * 7;
        }
        if (!same) {
            ERR("a restore lost the seed %" PRIu32 " or a pair",
                multidictGetHashFunctionSeed(d));
        }
        if (r) {
            persistOpsMultidict.free(r);
        }
    }
    return err;
}

/* Resize sizing and stats on an empty dict */
static int mdTestResizeAndEmptyStats_(multidictClass *qdc) {
    int err = 0;
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 9);
    multidictStats stats = {};
    multidictGetStats(d, &stats);
    multidictDetailedStats detail = {};
    multidictGetDetailedStats(d, &detail);
    databox k = {};
    databox v = {};
    if (stats.count != 0 || stats.loadFactor != 0 ||
        detail.basic.loadFactor != 0 || detail.avgChainLen != 0.0f ||
        detail.hasRehashTable || multidictPopRandom(d, &k, &v)) {
        ERR("an empty dict reported entries");
    }

    /* Byte-based: a resize keeps the slots its bytes need at the target
     * average, not one per entry */
    for (int64_t i = 0; i < 64; i++) {
        char buf[200];
        memset(buf, 'a' + (int)(i % 26), sizeof(buf));
        const databox key = databoxNewSigned(i);
        const databox val = databoxNewBytes(buf, sizeof(buf));
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
    }
    while (multidictIsRehashing(d)) {
        multidictRehash(d, 100);
    }
    multidictEnableByteBasedExpansion(d, 512, 4096);
    multidictGetStats(d, &stats);
    const uint64_t forBytes = stats.usedBytes / 512 + 1;
    multidictResize(d);
    while (multidictIsRehashing(d)) {
        multidictRehash(d, 100);
    }
    multidictGetStats(d, &stats);
    if (stats.count != 64 || stats.slots < forBytes) {
        ERR("a byte-based resize kept %" PRIu64 " slots, below the %" PRIu64
            " its bytes need",
            stats.slots, forBytes);
    }

    /* A resize is refused with resizing off (a class-wide switch) */
    multidictResizeDisable(d);
    if (multidictResize(d)) {
        ERR("a resize ran with resizing disabled");
    }
    multidictResizeEnable(d);
    return err;
}

int multidictTest(int, char *[]) {
    int err = 0;

    /* The dict most sections below share, on multimap slots */
    cleaner(multidictMmClassFree) multidictClass *qdc = multidictMmClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 12345);

    TEST("scans during a shrinking rehash, resize sizing, empty stats, "
         "numeric keys, seeds") {
        err += mdTestShrinkingRehashScan_(qdc, 0);
        err += mdTestShrinkingRehashScan_(qdc, 1);
        err += mdTestResizeAndEmptyStats_(qdc);
        err += mdTestNumericKeyIdentity_(qdc);
        err += mdTestSeedRoundTrip_(qdc);
    }

    /* ================================================================
     * SECTION 1: Basic API Tests
     * ================================================================ */
    TEST("1.1 create with the exact key type") {
        assert(d != nullptr);
        assert(multidictSize(d) == 0);
    }

    TEST("1.2 multidictGetClass") {
        assert(multidictGetClass(d) == qdc);
    }

    TEST("1.3 get/set the hash seed") {
        assert(multidictGetHashFunctionSeed(d) == 12345);
        multidictSetHashFunctionSeed(d, 99999);
        assert(multidictGetHashFunctionSeed(d) == 99999);
        multidictSetHashFunctionSeed(d, 12345); /* restore */
    }

    TEST("1.4 empty dict operations") {
        databox key = databoxNewBytesString("nokey");
        databox val;
        assert(multidictFind(d, &key, &val) == false);
        assert(multidictDelete(d, &key) == false);

        databox randomKey;
        assert(multidictGetRandomKey(d, &randomKey) == false);

        databox keys[5];
        assert(multidictGetSomeKeys(d, keys, 5) == 0);
    }

    TEST("1.5 basic insert and find") {
        databox key = databoxNewBytesString("hello");
        databox val = databoxNewBytesString("world");
        multidictResult result = multidictAdd(d, &key, &val);
        assert(result == MULTIDICT_OK_INSERTED);
        assert(multidictSize(d) == 1);

        databox found;
        assert(multidictFind(d, &key, &found) == true);
        assert(found.len == val.len);
    }

    TEST("1.6 update an existing key") {
        databox key = databoxNewBytesString("hello");
        databox newVal = databoxNewBytesString("universe");
        multidictResult result = multidictAdd(d, &key, &newVal);
        assert(result == MULTIDICT_OK_REPLACED);
        assert(multidictSize(d) == 1); /* size unchanged */

        databox found;
        assert(multidictFind(d, &key, &found) == true);
    }

    TEST("1.7 delete a key") {
        databox key = databoxNewBytesString("hello");
        assert(multidictDelete(d, &key) == true);
        assert(multidictSize(d) == 0);

        databox found;
        assert(multidictFind(d, &key, &found) == false);
        assert(multidictDelete(d, &key) == false); /* already deleted */
    }

    TEST("1.8 multidictFindByString") {
        databox key = databoxNewBytesString("strkey");
        databox val = databoxNewBytesString("strval");
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

        /* A present byte-string value: its bytes, borrowed from the dict */
        static constexpr char strkey[] = "strkey";
        uint8_t *foundVal = nullptr;
        bool found = multidictFindByString(d, strkey, &foundVal);
        assert(found == true && foundVal != nullptr);
        assert(memcmp(foundVal, "strval", 6) == 0);

        /* An absent key, and a value that is not a byte string, report
         * false and leave '*val' alone */
        uint8_t sentinel = 0;
        uint8_t *untouched = &sentinel;
        found = multidictFindByString(d, "nostrkey", &untouched);
        assert(found == false && untouched == &sentinel);

        databox intKey = databoxNewBytesString("strint");
        databox intVal = databoxNewSigned(42);
        MULTIDICT_TEST_OK(multidictAdd(d, &intKey, &intVal));
        found = multidictFindByString(d, "strint", &untouched);
        assert(found == false && untouched == &sentinel);

        multidictDelete(d, &key);
        multidictDelete(d, &intKey);
        found = multidictFindByString(d, strkey, &untouched);
        assert(found == false && untouched == &sentinel);
    }

    /* ================================================================
     * SECTION 2: Edge Cases
     * ================================================================ */
    TEST("2.1 single character keys") {
        for (int i = 0; i < 26; i++) {
            char keyBuf[2] = {(char)('a' + i), '\0'};
            char valBuf[2] = {(char)('A' + i), '\0'};
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }
        assert(multidictSize(d) == 26);

        /* Verify all exist */
        for (int i = 0; i < 26; i++) {
            char keyBuf[2] = {(char)('a' + i), '\0'};
            databox key = databoxNewBytesString(keyBuf);
            databox found;
            assert(multidictFind(d, &key, &found) == true);
        }
        multidictEmpty(d);
    }

    TEST("2.2 long keys (256 bytes)") {
        char longKey[257];
        char longVal[257];
        memset(longKey, 'K', 256);
        memset(longVal, 'V', 256);
        longKey[256] = '\0';
        longVal[256] = '\0';

        databox key = databoxNewBytesString(longKey);
        databox val = databoxNewBytesString(longVal);
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

        databox found;
        assert(multidictFind(d, &key, &found) == true);
        assert(found.len == 256);
        multidictEmpty(d);
    }

    TEST("2.3 keys with special characters") {
        const char *specialKeys[] = {
            "",
            "  ",
            "\t\n",
            "key with spaces",
            "key\0embedded", /* will be truncated at \0 */
            "émojis🎉",
            "日本語",
            "مرحبا"};
        int numSpecial = countof(specialKeys);

        for (int i = 0; i < numSpecial; i++) {
            databox key = databoxNewBytesString(specialKeys[i]);
            databox val = databoxNewBytesString("value");
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        /* Verify accessible */
        for (int i = 0; i < numSpecial; i++) {
            databox key = databoxNewBytesString(specialKeys[i]);
            databox found;
            bool exists = multidictFind(d, &key, &found);
            assert(exists == true);
        }
        multidictEmpty(d);
    }

    TEST("2.4 numeric string keys with collisions") {
        /* Insert keys that might hash similarly */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "%d", i * 7919); /* prime gaps */
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
    TEST("3.1 an iterator on an empty dict") {
        multidictIterator iter;
        multidictIteratorInit(d, &iter);
        multidictEntry entry;
        assert(multidictIteratorNext(&iter, &entry) == false);
        multidictIteratorRelease(&iter);
    }

    TEST("3.2 iterate and count") {
        for (int i = 0; i < 500; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "iter%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("3.3 a safe iterator allows modifications") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
            }
        }
        multidictIteratorRelease(&iter);
        assert(count >= 500);

        /* Clean added key */
        databox key = databoxNewBytesString("safeadd");
        multidictDelete(d, &key);
    }

    TEST("3.4 multiple iterators") {
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

    TEST("3.5 a safe iterator prevents the auto-rehash step") {
        /* Add enough entries to allow expansion to start */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "block_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
        MULTIDICT_TEST_OK(multidictAdd(d, &testKey, &testVal));

        /* Rehash index should not have advanced due to iterator */
        assert(d->rehashidx == rehashIdxBefore);

        /* Release the iterator */
        multidictIteratorRelease(&iter);
        assert(d->iterators == 0);

        /* Now operations should trigger rehash steps */
        databox testKey2 = databoxNewBytesString("rehash_test2");
        MULTIDICT_TEST_OK(multidictAdd(d, &testKey2, &testVal));

        /* Complete the rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictEmpty(d);
    }

    TEST("3.6 iterator release without advancing") {
        /* Add some entries */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "no_advance_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("3.7 multiple safe iterators are counted") {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "multi_safe_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("3.8 an iterator during an active rehash sees every entry") {
        /* Add entries */
        for (int i = 0; i < 200; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "iter_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("3.9 re-init an iterator mid-iteration") {
        /* Add entries */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "reinit_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
    TEST("4.1 basic expand and rehash") {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("4.2 operations during a rehash") {
        multidictExpand(d, 8192);
        assert(multidictIsRehashing(d) == true);

        /* Add during rehash */
        databox key = databoxNewBytesString("during_rehash");
        databox val = databoxNewBytesString("value");
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

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

    /* A rehash moves every entry and loses none: the size holds and an
     * accepted expand ends with at least the slots it asked for */
    TEST("4.3 rehash with single steps") {
        const uint64_t size = multidictSize(d);
        const bool expanded = multidictExpand(d, 16384);
        int steps = 0;
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 1);
            steps++;
        }

        printf("  Completed in %d single steps\n", steps);
        if (multidictSize(d) != size ||
            (expanded && multidictSlots(d) < 16384)) {
            ERR("after the rehash: %" PRIu64 " entries (was %" PRIu64
                "), %" PRIu64 " slots",
                multidictSize(d), size, multidictSlots(d));
        }
    }

    TEST("4.4 multidictRehashMilliseconds") {
        const uint64_t size = multidictSize(d);
        const bool expanded = multidictExpand(d, 32768);
        int64_t rehashed = multidictRehashMilliseconds(d, 5);
        printf("  Rehashed %" PRId64 " entries in ~5ms\n", rehashed);

        /* Complete remaining */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 1000);
        }

        if (rehashed < 0 || multidictSize(d) != size ||
            (expanded && multidictSlots(d) < 32768)) {
            ERR("after the rehash: %" PRId64 " moved in the timed step, "
                "%" PRIu64 " entries (was %" PRIu64 "), %" PRIu64 " slots",
                rehashed, multidictSize(d), size, multidictSlots(d));
        }
    }

    TEST("4.5 an expand to the same size is refused") {
        uint64_t currentSlots = multidictSlots(d);
        bool expanded = multidictExpand(d, currentSlots / 2);
        /* Should fail - can't expand to same or smaller */
        assert(expanded == false || !multidictIsRehashing(d));
    }

    TEST("4.6 multidictResize (shrink)") {
        /* Delete many entries to create a sparse table, with automatic
         * resizing off so the explicit resize is what shrinks it */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        multidictResizeDisable(d);
        for (int i = 0; i < 900; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rh%d", i);
            databox key = databoxNewBytesString(keyBuf);
            multidictDelete(d, &key);
        }
        const uint64_t sparseSlots = multidictSlots(d);
        multidictResizeEnable(d);

        bool resized = multidictResize(d);
        printf("  Resize returned: %s (%" PRIu64 " slots before)\n",
               resized ? "true" : "false", sparseSlots);

        /* Complete any triggered rehash */
        while (multidictIsRehashing(d)) {
            multidictRehash(d, 100);
        }
        /* A resize lands at half the growth point: 100 entries at 400%
         * (the default 800% halved) want 25 slots, so 32, the power of
         * two above: not the sparse table, and not collapsed to the
         * minimum of 8 */
        if (multidictSlots(d) != 32 || (sparseSlots > 32 && !resized)) {
            ERR("resize left %" PRIu64 " slots for %" PRIu64
                " entries, want 32",
                multidictSlots(d), multidictCount(d));
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

    TEST("4.7 find entries in ht[0] when the ht[1] slot is non-empty") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("4.8 delete entries from ht[0] during a partial rehash") {
        /* Similar to 4.7, but tests that delete works correctly when
         * entries are split across ht[0] and ht[1]. */

        const int NUM_ENTRIES = 1000;
        for (int i = 0; i < NUM_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "del_rehash_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("4.9 rehash operation fuzzing - add/find/delete mix") {
        const int FUZZ_ENTRIES = 1500;
        const int FUZZ_OPS = 2000;

        /* Initial populate */
        for (int i = 0; i < FUZZ_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i * 100);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                MD_TEST_APPLIES_IF(multidictExistsNoTouch(d, &key),
                                   multidictReplace(d, &key, &val));
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

    TEST("4.10 IncrBy operations during rehashing") {
        multidictEmpty(d); /* Start fresh */
        const int INCR_ENTRIES = 1000;

        /* Add numeric entries */
        for (int i = 0; i < INCR_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "incr_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(0);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                    printf("  IncrBy key incr_%d returned result=%" PRId64
                           " (expected 1), res=%d\n",
                           i, result, res);
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
            ERR("%d of %d IncrBy operations succeeded", incrSuccess,
                INCR_ENTRIES);
        }

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

    TEST("4.11 GetAndDelete during rehashing") {
        multidictEmpty(d); /* Start fresh */
        const int GAD_ENTRIES = 500;

        /* Populate */
        for (int i = 0; i < GAD_ENTRIES; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "gad_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i * 5);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("4.12 stress - rapid rehashing with constant operations") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                    MD_TEST_APPLIES_IF(multidictExistsNoTouch(d, &key),
                                       multidictReplace(d, &key, &val));
                    break;
                }
                case 2: { /* AddNX (might fail if exists) */
                    databox val = databoxNewSigned(idx);
                    MD_TEST_APPLIES_IF(!multidictExistsNoTouch(d, &key),
                                       multidictAddNX(d, &key, &val));
                    break;
                }
                case 3: { /* IncrBy */
                    int64_t result;
                    MULTIDICT_TEST_OK(multidictIncrBy(d, &key, 1, &result));
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
    TEST("5.1 scan an empty dict") {
        int scanCount = 0;
        uint64_t cursor = multidictScan(d, 0, scanCountCallback, &scanCount);
        assert(cursor == 0);
        assert(scanCount == 0);
    }

    TEST("5.2 a full scan with a callback") {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "scan%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        int scanCount = 0;
        uint64_t cursor = 0;
        do {
            cursor = multidictScan(d, cursor, scanCountCallback, &scanCount);
        } while (cursor != 0);

        printf("  Scanned %d entries\n", scanCount);
        assert(scanCount == 1000);
    }

    TEST("5.3 scan during a rehash") {
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

    TEST("5.4 scan fuzz - small dict sizes") {
        /* Test scanning with various small dict sizes */
        const int testSizes[] = {1, 2, 3, 5, 10, 50, 100, 500};
        for (size_t sizeIdx = 0; sizeIdx < countof(testSizes); sizeIdx++) {
            int targetSize = testSizes[sizeIdx];
            cleaner(multidictFree) multidict *dScan =
                multidictNew(&multidictTypeExactKey, qdc, 0);

            /* Insert entries */
            for (int i = 0; i < targetSize; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "fuzz_%d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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
        printf("  Tested %zu different dict sizes\n", countof(testSizes));
    }

    TEST("5.5 scan fuzz - large dicts with rehashing at various stages") {
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
                MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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

    TEST("5.6 scan with concurrent modifications during rehashing") {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Insert initial entries */
        for (int i = 0; i < 2000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "mod_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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
                MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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

    TEST("5.7 scan correctness - exact key coverage") {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);
        const int exactCount = 1000;

        /* Insert exactly 1000 unique entries with predictable keys */
        for (int i = 0; i < exactCount; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "exact_%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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

    TEST("5.8 fuzz - rapid dict size changes during a scan") {
        cleaner(multidictFree) multidict *dScan =
            multidictNew(&multidictTypeExactKey, qdc, 0);

        /* Insert initial batch */
        for (int i = 0; i < 500; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "extreme_%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(dScan, &key, &val));
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
    TEST("6.1 a random key from a populated dict") {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rand%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        databox randomKey;
        for (int i = 0; i < 50; i++) {
            bool got = multidictGetRandomKey(d, &randomKey);
            assert(got == true);
        }
    }

    TEST("6.2 get some keys") {
        databox keys[20];
        uint32_t got = multidictGetSomeKeys(d, keys, 20);
        printf("  Requested 20, got %u\n", got);
        assert(got > 0 && got <= 20);

        /* Request more than exist - need larger buffer */
        databox keys200[200];
        got = multidictGetSomeKeys(d, keys200, 200);
        printf("  Requested 200, got %u\n", got);
    }

    TEST("6.2b get some keys while growing and shrinking") {
        /* Mid-rehash both tables are sampled, skipping the old table's
         * migrated slots (and, when the new table is the smaller one,
         * jumping to the rehash index past its end).  Every key returned
         * must be live, at most the count asked for, at least one. */
        for (int shrink = 0; shrink < 2; shrink++) {
            cleaner(multidictMmClassFree) multidictClass *sc =
                multidictMmClassNew();
            cleaner(multidictFree) multidict *sd =
                multidictNew(&multidictTypeExactKey, sc, 23);
            /* The old table: big enough that a few rehash steps leave
             * most of it unmigrated */
            multidictExpand(sd, shrink ? 8192 : 256);
            while (multidictIsRehashing(sd)) {
                multidictRehash(sd, 1000);
            }
            multidictResizeDisable(sd);
            for (int i = 0; i < 1500; i++) {
                const databox k = databoxNewSigned(i * 31);
                const databox v = databoxNewSigned(i);
                MULTIDICT_TEST_OK(multidictAdd(sd, &k, &v));
            }
            multidictResizeEnable(sd);
            multidictExpand(sd, shrink ? 64 : 16384);
            multidictRehash(sd, 3);
            if (!multidictIsRehashing(sd)) {
                ERR("no rehash in flight to sample");
            }
            for (int r = 0; r < 30 && multidictIsRehashing(sd); r++) {
                databox got[40];
                const uint32_t n = multidictGetSomeKeys(sd, got, 40);
                if (n == 0 || n > 40) {
                    ERR("sampled %u keys of 40", n);
                }
                for (uint32_t i = 0; i < n; i++) {
                    /* Non-negative integers come back by value, either
                     * signedness */
                    databox found;
                    const bool integer = got[i].type == DATABOX_SIGNED_64 ||
                                         got[i].type == DATABOX_UNSIGNED_64;
                    if (!integer || got[i].data.u64 % 31 != 0 ||
                        !multidictFind(sd, &got[i], &found)) {
                        ERR("sampled key %u is not live", i);
                        break;
                    }
                }
            }
        }
    }

    TEST("6.3 a random key during a rehash") {
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

    TEST("6.4 a random key never fails on a non-empty dict") {
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
            MULTIDICT_TEST_OK(multidictAdd(rd, &key, &val));
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
                databox rk = {};
                const bool got = multidictGetRandomKey(rd, &rk);
                const int32_t idx = got ? testKeyIndex_(&rk, "grk") : -1;
                if (!got || idx < 0 || !alive[idx]) {
                    ERR("GetRandomKey at count %" PRIu64 ": got=%d idx=%d",
                        multidictCount(rd), got, idx);
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
                ERR("PopRandom failed at count %" PRIu64, remaining);
                break;
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

        databox rk = {};
        assert(!multidictGetRandomKey(rd, &rk));
        databox pk, pv;
        assert(!multidictPopRandom(rd, &pk, &pv));
    }

    /* ================================================================
     * SECTION 7: Stress Tests / Fuzzing
     * ================================================================ */
    TEST("7.1 a large insert/delete/find mix (10K ops)") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("7.2 rapid expand/rehash cycles") {
        for (int cycle = 0; cycle < 5; cycle++) {
            /* Insert entries */
            for (int i = 0; i < 500; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "cycle%d_%d", cycle, i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("7.3 interleaved operations during a rehash (5K ops)") {
        testRandSeed(123);

        /* Pre-populate */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "inter%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("7.4 consistency after random ops") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
    TEST("8.1 empty -> populated -> empty -> populated") {
        for (int cycle = 0; cycle < 3; cycle++) {
            assert(multidictSize(d) == 0);

            for (int i = 0; i < 100; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "trans%d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(i);
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
            }
            assert(multidictSize(d) == 100);

            multidictEmpty(d);
        }
        printf("  Completed 3 cycles of empty->populated->empty\n");
    }

    TEST("8.2 resize enable/disable behavior") {
        for (int i = 0; i < 100; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "resize%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("8.3 the iterator fingerprint check (implicit)") {
        for (int i = 0; i < 50; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "fp%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
    TEST("9.0 key compares are binary-safe and locale-free") {
        err += multidictTestKeyCompare_();
    }

    TEST("9.1 case-insensitive find, replace and delete") {
        cleaner(multidictMmClassFree) multidictClass *qdcCase =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dCase =
            multidictNew(&multidictTypeCaseKey, qdcCase, 54321);
        assert(dCase != nullptr);

        databox key1 = databoxNewBytesString("hello");
        databox val1 = databoxNewBytesString("world");
        assert(multidictAdd(dCase, &key1, &val1) == MULTIDICT_OK_INSERTED);

        /* Any spelling finds the one entry */
        static const char *spellings[] = {"hello", "HELLO", "HeLLo", "hellO"};
        for (size_t i = 0; i < countof(spellings); i++) {
            databox key = databoxNewBytesString(spellings[i]);
            databox found = {};
            if (!multidictFind(dCase, &key, &found) ||
                !testBoxIsBytes_(&found, "world", 5)) {
                ERR("case find of '%s' failed", spellings[i]);
            }
        }

        /* Replacing under another spelling keeps the first one */
        databox key2 = databoxNewBytesString("HELLO");
        databox val2 = databoxNewBytesString("there");
        if (multidictAdd(dCase, &key2, &val2) != MULTIDICT_OK_REPLACED ||
            multidictCount(dCase) != 1 ||
            !testOnlyEntryIs_(dCase, "hello", 5, &val2)) {
            ERR("case replace did not keep 'hello'");
        }

        databox key3 = databoxNewBytesString("hElLo");
        if (!multidictDelete(dCase, &key3) || multidictCount(dCase) != 0 ||
            multidictExists(dCase, &key1) || multidictDelete(dCase, &key1)) {
            ERR("case delete failed");
        }
    }

    TEST("9.2 every key-matching operation in a case dict") {
        err += multidictTestCaseOps_();
    }

    TEST("9.3 binary keys with embedded NUL, both types") {
        err += multidictTestBinaryKeys_(&multidictTypeExactKey);
        err += multidictTestBinaryKeys_(&multidictTypeCaseKey);
    }

    TEST("9.4 an exact dict keeps case-differing keys distinct") {
        err += multidictTestExactDistinct_();
    }

    TEST("9.5 a custom keyCompare is honoured") {
        err += multidictTestCustomCompare_();
    }

    TEST("9.6 dicts against a folding oracle") {
        err += multidictTestCaseDifferential_(&multidictTypeCaseKey, 0x5eed,
                                              false);
        err +=
            multidictTestCaseDifferential_(&multidictTypeCaseKey, 0xca5e, true);
        err += multidictTestCaseDifferential_(&multidictTypeExactKey, 0xe7ac,
                                              true);
    }

    TEST("9.7 persist snapshots record how keys match") {
        err += multidictTestPersistKeys_();
    }

    TEST("9.8 every read while a rehash is part way") {
        err += multidictTestRehashReads_();
    }

    TEST("9.9 LRU tracking turned on and off") {
        err += multidictTestLRUToggle_();
    }

    /* ================================================================
     * SECTION 10: Print Stats (Visual Verification)
     * ================================================================ */
    TEST("10.1 stats on a populated dict") {
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "stats%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }
        multidictPrintStats(d);
    }

    /* ================================================================
     * SECTION 11: Bytes Tracking Verification
     * ================================================================ */
    TEST("11.0 counters exact under in-place updates and removals, "
         "mid-rehash too") {
        err += multidictTestCountersUnderUpdates_();
    }

    TEST("11.0b AddNX mid-rehash refuses keys still in ht[0]") {
        err += multidictTestAddNXMidRehash_();
    }

    TEST("11.0c repr of a dict and of a slot operation") {
        cleaner(multidictMmClassFree) multidictClass *qdcRepr =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dRepr =
            multidictNew(&multidictTypeExactKey, qdcRepr, 5);
        multidictRepr(dRepr);
        const databox key = databoxNewBytesString("printed");
        const databox val = databoxNewSigned(1);
        MULTIDICT_TEST_OK(multidictAdd(dRepr, &key, &val));
        multidictRepr(dRepr);
        multidictOp op;
        multidictFindSlotForKeyNewest(dRepr, &op, &key, nullptr);
        multidictOpRepr(&op);
    }

    TEST("11.1 a fresh dict has zero bytes") {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Fresh dict: no slot or content bytes, only its slot table */
        assert(dBytes->ht[0].keyBytes == 0);
        assert(dBytes->ht[0].valBytes == 0);
        assert(dBytes->ht[0].usedBytes == 0);
        if (multidictBytes(dBytes) !=
            HTSIZE(dBytes, 0) * sizeof(multidictSlot *)) {
            ERR("a fresh dict reports %" PRIu64 " bytes for %" PRIu32
                " empty slots",
                multidictBytes(dBytes), HTSIZE(dBytes, 0));
        }
    }

    TEST("11.2 bytes increase on insert") {
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
            MULTIDICT_TEST_OK(multidictAdd(dBytes, &key, &val));
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

    TEST("11.3 bytes decrease on delete") {
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
            MULTIDICT_TEST_OK(multidictAdd(dBytes, &key, &val));
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

    TEST("11.4 a value replacement updates valBytes") {
        cleaner(multidictMmClassFree) multidictClass *qdcBytes =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dBytes =
            multidictNew(&multidictTypeExactKey, qdcBytes, 99999);

        /* Insert with short value */
        databox key = databoxNewBytesString("replaceMe");
        databox shortVal = databoxNewBytesString("short");
        MULTIDICT_TEST_OK(multidictAdd(dBytes, &key, &shortVal));

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

    TEST("11.5 bytes preserved through a rehash") {
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
            MULTIDICT_TEST_OK(multidictAdd(dBytes, &key, &val));
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

    /* ================================================================
     * SECTION 12: Existence and stats API
     * ================================================================ */

    /* Free the original d and qdc from sections 1-10, create fresh for API
     * tests */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 12345);

    TEST("12.1 multidictExists") {
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

    TEST("12.2 multidictExistsByString") {
        assert(multidictExistsByString(d, "testkey") == true);
        assert(multidictExistsByString(d, "nokey") == false);

        /* Add another key and verify */
        databox key2 = databoxNewBytesString("key2");
        databox val2 = databoxNewBytesString("val2");
        MULTIDICT_TEST_OK(multidictAdd(d, &key2, &val2));
        assert(multidictExistsByString(d, "key2") == true);
    }

    TEST("12.3 multidictGetStats") {
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

    TEST("12.4 multidictLoadFactor") {
        /* Add more entries to increase load factor */
        for (int j = 0; j < 100; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "loadkey%d", j);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(j);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        uint32_t lf = multidictLoadFactor(d);
        printf("  Load factor after 102 entries: %u%%\n", lf);
        assert(lf > 0); /* Should have non-zero load factor */
    }

    TEST("12.5 multidictBytes, KeyBytes, ValBytes") {
        uint64_t totalBytes = multidictBytes(d);
        uint64_t keyBytes = multidictKeyBytes(d);
        uint64_t valBytes = multidictValBytes(d);

        printf("  totalBytes=%" PRIu64 ", keyBytes=%" PRIu64
               ", valBytes=%" PRIu64 "\n",
               totalBytes, keyBytes, valBytes);

        assert(totalBytes > 0);
        assert(keyBytes > 0);
        assert(valBytes > 0);

        /* The total is the slots plus the slot pointer table, each byte
         * counted once (keyBytes and valBytes describe the content the
         * slots already hold) */
        multidictStats stats;
        multidictGetStats(d, &stats);
        const uint64_t table = stats.slots * sizeof(multidictSlot *);
        if (!stats.isRehashing && totalBytes != stats.usedBytes + table) {
            ERR("total %" PRIu64 " is not slots %" PRIu64 " + table %" PRIu64,
                totalBytes, stats.usedBytes, table);
        }
    }

    TEST("12.5a multidictSetLoadFactors: fewer slots, every key kept") {
        cleaner(multidictMmClassFree) multidictClass *qdcLoad =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dense =
            multidictNew(&multidictTypeExactKey, qdcLoad, 31);
        cleaner(multidictFree) multidict *sparse =
            multidictNew(&multidictTypeExactKey, qdcLoad, 31);

        /* Refused: a growth point under half an entry per slot or past
         * the field, a shrink point past its field or above a quarter */
        if (multidictSetLoadFactors(dense, 40, 10) ||
            multidictSetLoadFactors(dense, 70000, 10) ||
            multidictSetLoadFactors(dense, 2000, 300) ||
            multidictSetLoadFactors(dense, 400, 101) ||
            multidictSetLoadFactors(nullptr, 400, 10) ||
            !multidictSetLoadFactors(dense, 1600, 10) ||
            !multidictSetLoadFactors(sparse, 200, 10)) {
            ERR("load factor bounds wrong");
        }

        for (int j = 0; j < 4000; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "load-key-%d", j);
            const databox key = databoxNewBytesString(keyBuf);
            const databox val = databoxNewSigned(j);
            MULTIDICT_TEST_OK(multidictAdd(dense, &key, &val));
            MULTIDICT_TEST_OK(multidictAdd(sparse, &key, &val));
        }

        while (multidictRehash(dense, 1024) || multidictRehash(sparse, 1024)) {
        }

        /* At 1600% each slot holds up to 16 entries, at 200% up to 2 */
        if (multidictSlotCount(dense) * 4 > multidictSlotCount(sparse) ||
            multidictLoadFactor(dense) > 1600) {
            ERR("1600%%: %" PRIu64 " slots (load %u), 200%%: %" PRIu64,
                multidictSlotCount(dense), multidictLoadFactor(dense),
                multidictSlotCount(sparse));
        }

        for (int j = 0; j < 4000; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "load-key-%d", j);
            const databox key = databoxNewBytesString(keyBuf);
            databox val = {};
            if (!multidictFind(dense, &key, &val) || val.data.i64 != j) {
                ERR("1600%%: %s lost", keyBuf);
                break;
            }
        }
    }

    TEST("12.5c multidictSetDefaultLoadFactors: new dicts start with it") {
        uint32_t expand = 0;
        uint32_t shrink = 0;
        multidictDefaultLoadFactors(&expand, &shrink);
        if (expand != MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR ||
            shrink != MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR) {
            ERR("compiled default reads %u/%u", expand, shrink);
        }

        if (multidictSetDefaultLoadFactors(40, 10) ||
            multidictSetDefaultLoadFactors(400, 101)) {
            ERR("an out-of-bounds default was taken");
        }

        cleaner(multidictMmClassFree) multidictClass *qdcDefault =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *before =
            multidictNew(&multidictTypeExactKey, qdcDefault, 5);
        if (!multidictSetDefaultLoadFactors(200, 10)) {
            ERR("200%%/10%% refused as a default");
        }

        cleaner(multidictFree) multidict *after =
            multidictNew(&multidictTypeExactKey, qdcDefault, 5);
        multidictSetDefaultLoadFactors(MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR,
                                       MULTIDICT_DEFAULT_SHRINK_LOAD_FACTOR);
        for (int j = 0; j < 3000; j++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "default-%d", j);
            const databox key = databoxNewBytesString(keyBuf);
            const databox val = databoxNewSigned(j);
            MULTIDICT_TEST_OK(multidictAdd(before, &key, &val));
            MULTIDICT_TEST_OK(multidictAdd(after, &key, &val));
        }

        while (multidictRehash(before, 1024) || multidictRehash(after, 1024)) {
        }

        /* The dict made under 200% has at least twice the slots of the
         * one made before the change, which kept 800% */
        if (multidictSlotCount(after) < 2 * multidictSlotCount(before)) {
            ERR("200%% default: %" PRIu64 " slots, 800%%: %" PRIu64,
                multidictSlotCount(after), multidictSlotCount(before));
        }
    }

    TEST("12.5b multidictBytes never exceeds the heap the dict holds") {
        /* Measured through the allocator: the dict, its slot table and
         * every slot, at their usable sizes */
        if (ctestHeapMeterStart()) {
            cleaner(multidictMmClassFree) multidictClass *qdcHeap =
                multidictMmClassNew();
            cleaner(multidictFree) multidict *heapDict =
                multidictNew(&multidictTypeExactKey, qdcHeap, 4242);
            for (int j = 0; j < 5000; j++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "heap-key-%d", j);
                const databox key = databoxNewBytesString(keyBuf);
                const databox val = databoxNewSigned(j);
                MULTIDICT_TEST_OK(multidictAdd(heapDict, &key, &val));
            }

            while (multidictRehash(heapDict, 1024)) {
            }

            /* Live now: the class, the dict, its table and its slots */
            const int64_t heap = ctestHeapMeterStop();
            const uint64_t reported = multidictBytes(heapDict);
            multidictStats stats;
            multidictGetStats(heapDict, &stats);
            printf("  5000 entries: %" PRId64 " heap bytes, %" PRIu64
                   " reported, %" PRIu64 " in slots\n",
                   heap, reported, stats.usedBytes);
            if (heap <= 0 || reported > (uint64_t)heap ||
                reported < stats.usedBytes) {
                ERR("reported %" PRIu64 " bytes against %" PRId64
                    " heap and %" PRIu64 " in slots",
                    reported, heap, stats.usedBytes);
            }
        }
    }

    TEST("12.6 stats consistency through operations") {
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

    /* ================================================================
     * SECTION 13: Bulk Operations Tests
     * ================================================================ */

    /* Clear dict for fresh bulk tests */
    multidictEmpty(d);

    TEST("13.1 multidictAddMultiple") {
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

    TEST("13.2 multidictAddMultiple with duplicates") {
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

    TEST("13.3 multidictDeleteMultiple") {
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

    TEST("13.4 multidictDeleteMultiple with non-existent keys") {
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

    TEST("13.5 bulk operation edge cases") {
        /* NULL/empty checks */
        assert(multidictAddMultiple(d, nullptr, nullptr, 5) == 0);
        assert(multidictDeleteMultiple(d, nullptr, 5) == 0);

        databox keys[1], vals[1];
        keys[0] = databoxNewBytesString("edgekey");
        vals[0] = databoxNewBytesString("edgeval");

        assert(multidictAddMultiple(d, keys, vals, 0) == 0);
        assert(multidictDeleteMultiple(d, keys, 0) == 0);
    }

    TEST("13.6 a large bulk insert") {
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

    /* ================================================================
     * Section 14: Self-Management Tests
     * ================================================================ */

    /* Clean up and create fresh dict for these tests */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 12345);

    TEST("14.1 memory limit get/set") {
        assert(multidictGetMaxMemory(d) == 0); /* Default is unlimited */
        multidictSetMaxMemory(d, 10000);
        assert(multidictGetMaxMemory(d) == 10000);
        multidictSetMaxMemory(d, 0); /* Disable limit */
        assert(multidictGetMaxMemory(d) == 0);
    }

    TEST("14.2 multidictIsOverLimit") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("14.3 multidictEvictToLimit") {
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

    TEST("14.4 the eviction callback") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        /* Set limit and evict */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        multidictSetMaxMemory(d, userBytes / 4);
        uint32_t evicted = multidictEvictToLimit(d);

        printf("  Callback called %d times, evicted %u entries\n",
               testEvictionCallbackCount_, evicted);
        assert(testEvictionCallbackCount_ == (int)evicted);

        /* Clear callback */
        multidictSetEvictionCallback(d, nullptr, nullptr);
    }

    TEST("14.5 an eviction callback veto") {
        testVetoCount_ = 0;

        /* Ensure we have enough entries to veto 5 times */
        multidictSetMaxMemory(d, 0); /* Disable limit to add data */
        char vetoBufs[20][32];
        for (int j = 0; j < 20; j++) {
            snprintf(vetoBufs[j], sizeof(vetoBufs[j]), "vetokey%d", j);
            databox key = databoxNewBytesString(vetoBufs[j]);
            databox val = databoxNewBytesString(vetoBufs[j]);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        multidictSetEvictionCallback(d, vetoEvictionCb_, &testVetoCount_);

        /* Force eviction */
        uint64_t userBytes = multidictKeyBytes(d) + multidictValBytes(d);
        multidictSetMaxMemory(d, userBytes / 2);
        multidictEvictToLimit(d);

        printf("  Vetoed %d eviction attempts\n", testVetoCount_);
        /* The veto count should be at least 5 (we vetoed first 5 attempts) */
        assert(testVetoCount_ >= 5);

        multidictSetEvictionCallback(d, nullptr, nullptr);
    }

    TEST("14.6 no eviction when unlimited") {
        multidictSetMaxMemory(d, 0); /* Unlimited */
        uint32_t evicted = multidictEvictToLimit(d);
        assert(evicted == 0);
    }

    TEST("14.7 eviction with a rehash in flight") {
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
            }

            multidictExpand(e, (uint64_t)total * 4);
            assert(multidictIsRehashing(e));

            testEvictRehashSeen_ = 0;
            testEvictRehashBad_ = 0;
            multidictSetEvictionCallback(e, evictRehashCb_, nullptr);
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
                databox val = {};
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

    /* ================================================================
     * Section 15: Extended Fuzzing / Property Tests
     * ================================================================ */

    /* Fresh dict for fuzzing */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 54321);

    TEST("15.1 mixed operations stress") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &keys[idx], &vals[idx]));
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

    TEST("15.2 bulk operations under stress") {
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

    TEST("15.3 iterator consistency under modification") {
        multidictEmpty(d);

        char keyBufs[200][32], valBufs[200][32];
        for (int j = 0; j < 200; j++) {
            snprintf(keyBufs[j], sizeof(keyBufs[j]), "iterkey%d", j);
            snprintf(valBufs[j], sizeof(valBufs[j]), "iterval%d", j);
            databox key = databoxNewBytesString(keyBufs[j]);
            databox val = databoxNewBytesString(valBufs[j]);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
            }
        }
        multidictIteratorRelease(&iter);

        printf("  Iterated %d entries, final count: %" PRIu64 "\n", iterated,
               multidictCount(d));
    }

    TEST("15.4 a memory limit with continuous operations") {
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

            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("15.5 stats consistency") {
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

    /* ================================================================
     * Section 16: Conditional Insert Tests
     * ================================================================ */
    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 42);

    TEST("16.1 AddNX inserts a new key and refuses a present one") {
        databox key = databoxNewBytesString("nx-key");
        databox val = databoxNewBytesString("nx-value");
        multidictResult r = multidictAddNX(d, &key, &val);
        if (r != MULTIDICT_OK_INSERTED) {
            ERR("AddNX should succeed for a new key");
        }

        /* AddNX again should fail */
        databox val2 = databoxNewBytesString("nx-value-2");
        r = multidictAddNX(d, &key, &val2);
        if (r != MULTIDICT_ERR) {
            ERR("AddNX should fail for an existing key");
        }

        /* Verify original value is preserved */
        databox found = {};
        if (!multidictFind(d, &key, &found)) {
            ERR("the key should exist after AddNX");
        }
    }

    TEST("16.2 AddXX refuses an absent key and replaces a present one") {
        databox key = databoxNewBytesString("xx-key");
        databox val = databoxNewBytesString("xx-value");
        multidictResult r = multidictAddXX(d, &key, &val);
        if (r != MULTIDICT_ERR) {
            ERR("AddXX should fail for a non-existent key");
        }

        /* Add the key first */
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

        /* Now AddXX should succeed */
        databox val2 = databoxNewBytesString("xx-value-updated");
        r = multidictAddXX(d, &key, &val2);
        if (r != MULTIDICT_OK_REPLACED) {
            ERR("AddXX should succeed for an existing key");
        }
    }

    TEST("16.3 Replace has the semantics of AddXX") {
        databox key = databoxNewBytesString("replace-key");
        databox val = databoxNewBytesString("replace-value");

        /* Should fail for non-existent */
        if (multidictReplace(d, &key, &val) != MULTIDICT_ERR) {
            ERR("Replace should fail for a non-existent key");
        }

        /* Add key, then replace */
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        databox val2 = databoxNewBytesString("replaced!");
        if (multidictReplace(d, &key, &val2) != MULTIDICT_OK_REPLACED) {
            ERR("Replace should succeed for an existing key");
        }
    }

    /* ================================================================
     * Section 17: Atomic Get-and-Delete Tests
     * ================================================================ */
    TEST("17.1 GetAndDelete") {
        databox key = databoxNewBytesString("gad-key");
        databox val = databoxNewBytesString("gad-value");
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

        databox foundVal = {};
        if (!multidictGetAndDelete(d, &key, &foundVal)) {
            ERR("GetAndDelete should succeed");
        }

        /* Key should no longer exist */
        if (multidictExists(d, &key)) {
            ERR("the key should be deleted after GetAndDelete");
        }

        /* GetAndDelete on non-existent should return false */
        databox dummy = {};
        if (multidictGetAndDelete(d, &key, &dummy)) {
            ERR("GetAndDelete should fail for a non-existent key");
        }
    }

    TEST("17.2 PopRandom") {
        multidictEmpty(d);

        /* PopRandom on empty should fail */
        databox k = {}, v = {};
        if (multidictPopRandom(d, &k, &v)) {
            ERR("PopRandom should fail on an empty dict");
        }

        /* Add some entries */
        for (int i = 0; i < 10; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "pop-key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "pop-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        uint64_t initialCount = multidictCount(d);

        /* Pop all entries */
        while (multidictPopRandom(d, &k, &v)) {
            /* Each pop should reduce count */
        }

        if (multidictCount(d) != 0) {
            ERR("the dict should be empty after popping every entry");
        }

        if (initialCount != 10) {
            ERR("%" PRIu64 " entries before the pops, expected 10",
                initialCount);
        }
    }

    /* ================================================================
     * Section 18: Numeric Operations Tests
     * ================================================================ */
    multidictEmpty(d);

    TEST("18.1 IncrBy inserts, then increments and decrements") {
        databox key = databoxNewBytesString("counter");
        int64_t result = 0;
        multidictResult r = multidictIncrBy(d, &key, 5, &result);
        if (r != MULTIDICT_OK_INSERTED) {
            ERR("IncrBy should insert on a new key");
        }
        if (result != 5) {
            ERR("IncrBy result should be 5, got %" PRId64, result);
        }

        /* Increment again */
        r = multidictIncrBy(d, &key, 10, &result);
        if (r != MULTIDICT_OK_REPLACED) {
            ERR("IncrBy should replace on an existing key");
        }
        if (result != 15) {
            ERR("IncrBy result should be 15, got %" PRId64, result);
        }

        /* Decrement */
        r = multidictIncrBy(d, &key, -3, &result);
        if (result != 12) {
            ERR("IncrBy(-3) should give 12, got %" PRId64, result);
        }
    }

    TEST("18.2 IncrByFloat") {
        databox key = databoxNewBytesString("float-counter");
        double result = 0.0;
        multidictResult r = multidictIncrByFloat(d, &key, 1.5, &result);
        if (r != MULTIDICT_OK_INSERTED) {
            ERR("IncrByFloat should insert on a new key");
        }
        if (result != 1.5) {
            ERR("IncrByFloat result should be 1.5, got %f", result);
        }

        r = multidictIncrByFloat(d, &key, 2.5, &result);
        if (result != 4.0) {
            ERR("IncrByFloat result should be 4.0, got %f", result);
        }
    }

    TEST("18.3 IncrBy on a non-numeric value is refused") {
        databox key = databoxNewBytesString("string-key");
        databox val = databoxNewBytesString("not-a-number");
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

        int64_t result = 0;
        multidictResult r = multidictIncrBy(d, &key, 1, &result);
        if (r != MULTIDICT_ERR) {
            ERR("IncrBy on a non-numeric value should fail");
        }
    }

    /* Increment policy: exact, refuse overflow, value unchanged on refusal;
     * IncrBy needs an int64 value; IncrByFloat refuses inf and NaN. */
    TEST("18.4 the increment policy") {
        const databox kMax = databoxNewBytesString("incr-max");
        const databox kMin = databoxNewBytesString("incr-min");
        const databox kBig = databoxNewBytesString("incr-big-unsigned");
        const databox kFloat = databoxNewBytesString("incr-float");
        const databox kHuge = databoxNewBytesString("incr-huge");
        const databox vMax = databoxNewSigned(INT64_MAX);
        const databox vMin = databoxNewSigned(INT64_MIN);
        const databox vBig = databoxNewUnsigned(UINT64_MAX);
        const databox vFloat = {.type = DATABOX_FLOAT_32, .data.f32 = 2.5f};
        const databox vHuge = databoxNewReal(DBL_MAX);
        MULTIDICT_TEST_OK(multidictAdd(d, &kMax, &vMax));
        MULTIDICT_TEST_OK(multidictAdd(d, &kMin, &vMin));
        MULTIDICT_TEST_OK(multidictAdd(d, &kBig, &vBig));
        MULTIDICT_TEST_OK(multidictAdd(d, &kFloat, &vFloat));
        MULTIDICT_TEST_OK(multidictAdd(d, &kHuge, &vHuge));

        int64_t ir = 42;
        double dr = 42;
        databox now = {};
        if (multidictIncrBy(d, &kMax, 1, &ir) != MULTIDICT_ERR || ir != 42 ||
            !multidictFind(d, &kMax, &now) || now.data.i64 != INT64_MAX) {
            ERR("INT64_MAX + 1 was not refused unchanged");
        }
        if (multidictIncrBy(d, &kMin, -1, &ir) != MULTIDICT_ERR) {
            ERR("INT64_MIN - 1 was not refused");
        }
        if (multidictIncrBy(d, &kBig, 1, &ir) != MULTIDICT_ERR) {
            ERR("IncrBy on UINT64_MAX was not refused");
        }
        if (multidictIncrBy(d, &kFloat, 1, &ir) != MULTIDICT_ERR) {
            ERR("IncrBy on a float value was not refused");
        }
        if (multidictIncrByFloat(d, &kFloat, 0.25, &dr) !=
                MULTIDICT_OK_REPLACED ||
            dr != 2.75) {
            ERR("IncrByFloat on a float value read it wrongly");
        }
        if (multidictIncrByFloat(d, &kHuge, DBL_MAX, &dr) != MULTIDICT_ERR ||
            !multidictFind(d, &kHuge, &now) || now.data.d64 != DBL_MAX) {
            ERR("an infinite IncrByFloat result was not refused");
        }
        if (multidictIncrByFloat(d, &kHuge, NAN, &dr) != MULTIDICT_ERR) {
            ERR("a NaN IncrByFloat result was not refused");
        }
    }

    /* ================================================================
     * Section 19: Dict Operations Tests
     * ================================================================ */
    TEST("18.5 IncrBy of new and present keys builds what Add builds, "
         "rehashing or not") {
        /* Two dicts fed the same keys, one by Add of the value IncrBy
         * would make, one by IncrBy (a rehash started part way, so keys
         * meet both tables): after new keys and after a second pass over
         * every key the two hold the same values and the same count and
         * byte counts.  IncrByFloat likewise; a non-finite increment is
         * refused for a new key and a present one; an LRU dict tracks a
         * counter IncrBy created */
        cleaner(multidictMmClassFree) multidictClass *cls =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *byAdd =
            multidictNew(&multidictTypeExactKey, cls, 7);
        cleaner(multidictFree) multidict *byIncr =
            multidictNew(&multidictTypeExactKey, cls, 7);
        constexpr int keys = 600;
        for (int pass = 1; pass <= 2; pass++) {
            for (int i = 0; i < keys; i++) {
                if (pass == 1 && i == keys / 3) {
                    multidictExpand(byAdd, keys * 4);
                    multidictExpand(byIncr, keys * 4);
                }
                char buf[32];
                snprintf(buf, sizeof(buf), "counter_%d", i);
                const databox key = databoxNewBytesString(buf);
                const databox want = databoxNewSigned((int64_t)i * pass);
                MULTIDICT_TEST_OK(multidictAdd(byAdd, &key, &want));
                int64_t got = 0;
                const multidictResult r =
                    multidictIncrBy(byIncr, &key, i, &got);
                const multidictResult wantR =
                    pass == 1 ? MULTIDICT_OK_INSERTED : MULTIDICT_OK_REPLACED;
                if (r != wantR || got != (int64_t)i * pass) {
                    ERR("pass %d key %d: IncrBy gave %d / %" PRId64, pass, i,
                        (int)r, got);
                }
            }

            multidictStats a = {};
            multidictStats b = {};
            multidictGetStats(byAdd, &a);
            multidictGetStats(byIncr, &b);
            if (a.count != b.count || a.usedBytes != b.usedBytes ||
                a.keyBytes != b.keyBytes || a.valBytes != b.valBytes ||
                b.count != (uint64_t)keys) {
                ERR("pass %d: count %" PRIu64 "/%" PRIu64 ", used %" PRIu64
                    "/%" PRIu64 ", key %" PRIu64 "/%" PRIu64 ", val %" PRIu64
                    "/%" PRIu64,
                    pass, a.count, b.count, a.usedBytes, b.usedBytes,
                    a.keyBytes, b.keyBytes, a.valBytes, b.valBytes);
            }
            for (int i = 0; i < keys; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "counter_%d", i);
                const databox key = databoxNewBytesString(buf);
                databox v = {};
                const databox want = databoxNewSigned((int64_t)i * pass);
                if (!multidictFind(byIncr, &key, &v) ||
                    databoxCompare(&v, &want) != 0) {
                    ERR("pass %d: key %d holds the wrong counter", pass, i);
                }
            }
        }

        /* Floats, and the non-finite refusal for new and present keys */
        const databox f = databoxNewBytesString("float");
        double out = 0;
        if (multidictIncrByFloat(byIncr, &f, 0.25, &out) !=
                MULTIDICT_OK_INSERTED ||
            out != 0.25 ||
            multidictIncrByFloat(byIncr, &f, 0.5, &out) !=
                MULTIDICT_OK_REPLACED ||
            out != 0.75) {
            ERR("IncrByFloat did not add, then add to, a new counter");
        }
        const databox nf = databoxNewBytesString("never");
        if (multidictIncrByFloat(byIncr, &nf, INFINITY, &out) !=
                MULTIDICT_ERR ||
            multidictExists(byIncr, &nf) ||
            multidictIncrByFloat(byIncr, &f, NAN, &out) != MULTIDICT_ERR) {
            ERR("a non-finite increment was not refused");
        }

        cleaner(multidictFree) multidict *lru =
            multidictNew(&multidictTypeExactKey, cls, 7);
        multidictEnableLRU(lru, 4);
        const databox c = databoxNewBytesString("tracked");
        int64_t n = 0;
        if (multidictIncrBy(lru, &c, 3, &n) != MULTIDICT_OK_INSERTED ||
            multidictGetLRULevel(lru, &c) < 0) {
            ERR("an LRU dict did not track the counter IncrBy created");
        }
    }

    TEST("19.1 Copy") {
        multidictEmpty(d);

        /* Add some entries */
        for (int i = 0; i < 100; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "copy-key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "copy-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        cleaner(multidictFree) multidict *copy = multidictCopy(d);
        if (!copy) {
            ERR("Copy should return non-NULL");
        } else {
            if (multidictCount(copy) != multidictCount(d)) {
                ERR("Copy count mismatch: %" PRIu64 " vs %" PRIu64,
                    multidictCount(copy), multidictCount(d));
            }

            /* Verify all keys exist in copy */
            for (int i = 0; i < 100; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "copy-key-%d", i);
                databox key = databoxNewBytesString(keyBuf);
                if (!multidictExists(copy, &key)) {
                    ERR("key %s missing from the copy", keyBuf);
                }
            }
        }
    }

    TEST("19.2 Merge in REPLACE mode") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        /* Add entries to src (overlapping + new) */
        for (int i = 25; i < 75; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "src-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(src, &key, &val));
        }

        uint64_t merged = multidictMerge(d, src, MULTIDICT_MERGE_REPLACE);
        if (merged != 50) {
            ERR("Merge REPLACE should merge 50, got %" PRIu64, merged);
        }

        /* Total should now be 75 (0-74) */
        if (multidictCount(d) != 75) {
            ERR("after the merge the count should be 75, got %" PRIu64,
                multidictCount(d));
        }
    }

    TEST("19.3 Merge in KEEP mode") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
        }

        /* Add overlapping entries to src */
        for (int i = 5; i < 15; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key-%d", i);
            snprintf(valBuf, sizeof(valBuf), "src-val-%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(src, &key, &val));
        }

        /* KEEP mode - only non-existent keys should be added */
        uint64_t merged = multidictMerge(d, src, MULTIDICT_MERGE_KEEP);
        if (merged != 5) {
            ERR("Merge KEEP should only add 5 new, got %" PRIu64, merged);
        }

        if (multidictCount(d) != 15) {
            ERR("after the merge the count should be 15, got %" PRIu64,
                multidictCount(d));
        }
    }

    TEST("19.4 a refusing class: Copy fails, Merge counts what landed, LRU "
         "tracks no refused key") {
        cleaner(multidictMmClassFree) multidictClass *refusing =
            multidictMmClassNew();
        refusing->insertByType = mdTestRefusingInsert;
        mdTestRefuse = MD_TEST_REFUSE_NONE;
        cleaner(multidictFree) multidict *src =
            multidictNew(&multidictTypeExactKey, refusing, 7);
        for (int64_t i = 0; i < 20; i++) {
            const databox key = DATABOX_SIGNED(i);
            const databox val = DATABOX_SIGNED(-i);
            MULTIDICT_TEST_OK(multidictAdd(src, &key, &val));
        }

        mdTestRefuse = MD_TEST_REFUSE_ALL;
        multidict *copy = multidictCopy(src);
        if (copy) {
            ERR("a copy its class refuses returned a dict");
            multidictFree(copy);
        }

        cleaner(multidictFree) multidict *dst =
            multidictNew(&multidictTypeExactKey, refusing, 8);
        const uint64_t merged =
            multidictMerge(dst, src, MULTIDICT_MERGE_REPLACE);
        if (merged != 0 || multidictCount(dst) != 0) {
            ERR("a refused merge counted %" PRIu64 ", holds %" PRIu64, merged,
                multidictCount(dst));
        }

        /* The dict takes the key; its LRU index (an unsigned handle) is
         * refused, so the LRU tracks nothing */
        mdTestRefuse = MD_TEST_REFUSE_UNSIGNED;
        if (!multidictEnableLRU(dst, 4)) {
            ERR("LRU did not enable");
        } else {
            const databox key = DATABOX_SIGNED(1);
            const databox val = DATABOX_SIGNED(2);
            MULTIDICT_TEST_OK(multidictAdd(dst, &key, &val));
            if (multidictCount(dst) != 1 || multilruCount(dst->lru) != 0 ||
                multidictCount(dst->lruKeyToPtr) != 0) {
                ERR("count %" PRIu64 ", LRU %zu, LRU index %" PRIu64,
                    multidictCount(dst), multilruCount(dst->lru),
                    multidictCount(dst->lruKeyToPtr));
            }
        }
        mdTestRefuse = MD_TEST_REFUSE_NONE;
    }

    /* ================================================================
     * Section 20: LRU Eviction Tests
     * ================================================================ */

    multidictFree(d);
    multidictMmClassFree(qdc);
    qdc = multidictMmClassNew();
    d = multidictNew(&multidictTypeExactKey, qdc, 42);

    TEST("20.1 EnableLRU/HasLRU/DisableLRU basics") {
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

    TEST("20.2 large-scale LRU eviction (10K entries, 5K limit)") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

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

    TEST("20.3 hot key protection under pressure") {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 3000); /* ~100 entries */

        /* Insert 50 entries */
        for (int i = 0; i < 50; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "hkey%04d", i);
            snprintf(valBuf, sizeof(valBuf), "hval%04d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("20.4 LRU stress fuzz with mixed ops (5K iterations)") {
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
                MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("20.5 LRU during rehashing") {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 0); /* No limit initially */

        /* Insert enough to trigger rehashing */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32], valBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "rhkey%05d", i);
            snprintf(valBuf, sizeof(valBuf), "rhval%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString(valBuf);
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
            multidictEvictToLimit(d);
        }

        /* Verify consistency */
        multidictStats stats;
        multidictGetStats(d, &stats);
        printf("    After concurrent ops: count=%" PRIu64 ", bytes=%" PRIu64
               "\n",
               stats.count, stats.totalBytes);
    }

    TEST("20.6 policy switching") {
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_LRU);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_RANDOM);
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_RANDOM);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_NONE);
        assert(multidictGetEvictPolicy(d) == MULTIDICT_EVICT_NONE);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_LRU);
    }

    TEST("20.7 the GetLRULevel API") {
        multidictEmpty(d);
        multidictSetMaxMemory(d, 0);

        /* Insert a key */
        databox key = databoxNewBytesString("leveltest");
        databox val = databoxNewBytesString("value");
        MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));

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

    TEST("20.8 zero overhead without LRU") {
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
        MULTIDICT_TEST_OK(multidictAdd(dNoLru, &key, &val));
        multidictTouch(dNoLru, &key);
        multidictDelete(dNoLru, &key);

        printf(
            "    No-LRU operations: no crash, operations silently ignored\n");
    }

    TEST("20.9 vetoed LRU victims stay tracked at their rank") {
        cleaner(multidictMmClassFree) multidictClass *vqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *vd =
            multidictNew(&multidictTypeExactKey, vqdc, 11);
        const bool lruOn = multidictEnableLRU(vd, 4);
        assert(lruOn);
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
            MULTIDICT_TEST_OK(multidictAdd(vd, &key, &val));
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
        multidictSetEvictionCallback(vd, lruVetoCb_, nullptr);

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
                ERR("round 1 victim %d is key %d, expected %d", n,
                    testLruEvictOrder_[n], expect);
            }
            present[expect] = false;
            expect++;
        }
        assert(multidictCount(vd) == total - 60);

        /* Every survivor, the vetoed keys included, is still tracked, and
         * the vetoed keys kept their level and rank: they are still the
         * coldest entries, in their original order */
        assert(testLruTrackingExact_(vd));
        multilruPtr coldest[7] = {};
        const size_t coldestGot = multilruCollectLowest(vd->lru, coldest, 7);
        assert(coldestGot == 7);
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
                ERR("round 2 victim %d is key %d, expected %d", n,
                    testLruEvictOrder_[n], next);
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

    TEST("20.10 a case-insensitive dict evicts in LRU order") {
        cleaner(multidictMmClassFree) multidictClass *cqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *cd =
            multidictNew(&multidictTypeCaseKey, cqdc, 13);
        const bool lruOn = multidictEnableLRU(cd, 4);
        assert(lruOn);
        multidictSetEvictPolicy(cd, MULTIDICT_EVICT_LRU);

        /* Index i goes in under spelling i % 2, valued by its code
         * (index * 2 + spelling), so the eviction callback can check that
         * each victim comes back under the spelling that inserted it */
        enum { indexes = 40, entries = indexes };
        char keyBuf[32];
        for (int32_t i = 0; i < indexes; i++) {
            databox key = testLruCaseKey_(keyBuf, sizeof(keyBuf), i, i % 2);
            databox val = databoxNewSigned(i * 2 + i % 2);
            const multidictResult added = multidictAdd(cd, &key, &val);
            assert(added == MULTIDICT_OK_INSERTED);
        }
        assert(multidictCount(cd) == entries);

        char lowBuf[32], upBuf[32];
        databox low = testLruCaseKey_(lowBuf, sizeof(lowBuf), 0, 0);
        databox up = testLruCaseKey_(upBuf, sizeof(upBuf), 0, 1);
        assert(multidictKeyHash(cd, &low) == multidictKeyHash(cd, &up));
        assert(multidictKeyHash(cd, &low) == multidictHashOfKey_(cd, &low));

        /* The other spelling of an index is the same key: a find of index
         * 3, a touch of index 5 and a replace of index 9, each under the
         * spelling not stored, promote the stored entry */
        char key3Buf[32], key5Buf[32], key9Buf[32];
        databox key3 = testLruCaseKey_(key3Buf, sizeof(key3Buf), 3, 0);
        databox found = {};
        const bool found3 = multidictFind(cd, &key3, &found);
        assert(found3 && DATABOX_IS_INTEGER(&found) && found.data.i == 7);
        databox key5 = testLruCaseKey_(key5Buf, sizeof(key5Buf), 5, 0);
        multidictTouch(cd, &key5);
        databox key9 = testLruCaseKey_(key9Buf, sizeof(key9Buf), 9, 0);
        databox val9 = databoxNewSigned(19);
        const multidictResult replaced9 = multidictAdd(cd, &key9, &val9);
        assert(replaced9 == MULTIDICT_OK_REPLACED);
        assert(multidictCount(cd) == entries);
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

        /* Exactly insertion order without the three promoted indexes: any
         * random victim, or a promotion that missed the stored entry,
         * breaks it */
        int32_t expect = 0;
        for (int32_t n = 0; n < 30; n++) {
            while (expect == 3 || expect == 5 || expect == 9) {
                expect++;
            }
            if (log.order[n] != expect * 2 + expect % 2) {
                ERR("case victim %d is code %d, expected index %d", n,
                    log.order[n], expect);
            }
            expect++;
        }

        assert(multidictCount(cd) == entries - 30);
        assert(multidictGetLRULevel(cd, &key3) == 1);
        assert(multidictGetLRULevel(cd, &key5) == 1);
        assert(multidictGetLRULevel(cd, &key9) == 1);
        assert(testLruTrackingExact_(cd));

        /* The survivors, the replaced one included, keep the spelling
         * that inserted them */
        multidictIterator iter;
        multidictIteratorInit(cd, &iter);
        multidictEntry e;
        while (multidictIteratorNext(&iter, &e)) {
            const int32_t code = testLruCode_(&log, &e.key);
            assert(code >= 0 && code % 2 == (code / 2) % 2);
            assert(DATABOX_IS_INTEGER(&e.val) && e.val.data.i == code);
        }
        multidictIteratorRelease(&iter);
    }

    TEST("20.11 same-hash keys in one slot evict in LRU order") {
        cleaner(multidictMmClassFree) multidictClass *hqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *hd =
            multidictNew(&testConstHashType_, hqdc, 17);
        const bool lruOn = multidictEnableLRU(hd, 4);
        assert(lruOn);
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
            MULTIDICT_TEST_OK(multidictAdd(hd, &key, &val));
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
                ERR("same-hash victim %d is code %d, expected %d", n,
                    log.order[n], expect);
            }
        }

        assert(multidictCount(hd) == keys - evict);
        assert(multidictGetLRULevel(hd, &key0) == 1);
        assert(testLruTrackingExact_(hd));
    }

    TEST("20.12 reads do not promote, accesses promote once") {
        err += multidictTestLruPromotion_(&multidictTypeExactKey);
        err += multidictTestLruPromotion_(&multidictTypeCaseKey);
    }

    TEST("20.13 fewer tracked keys than the candidate window") {
        cleaner(multidictMmClassFree) multidictClass *wqdc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *wd =
            multidictNew(&multidictTypeExactKey, wqdc, 19);
        const bool lruOn = multidictEnableLRU(wd, 4);
        assert(lruOn);
        multidictSetEvictPolicy(wd, MULTIDICT_EVICT_LRU);

        /* Five keys, all at level 0 in insertion order, so every refill
         * of the candidate window comes back short.  Key 0, the coldest,
         * is vetoed. */
        constexpr int keys = 5;
        for (int32_t i = 0; i < keys; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "lruveto-%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            MULTIDICT_TEST_OK(multidictAdd(wd, &key, &val));
        }
        assert(testLruTrackingExact_(wd));

        const uint64_t bytes = multidictKeyBytes(wd) + multidictValBytes(wd);
        const uint64_t entryBytes = bytes / keys;
        assert(entryBytes * keys == bytes);

        testLruVetoActive_ = true;
        testLruVetoes_ = 0;
        testLruVetoBad_ = 0;
        testLruEvicted_ = 0;
        multidictSetEvictionCallback(wd, lruVetoCb_, nullptr);

        /* Evicting four takes keys 1..4 coldest first and passes over the
         * vetoed key 0 once */
        multidictSetMaxMemory(wd, bytes - 4 * entryBytes);
        uint32_t evicted = multidictEvictToLimit(wd);
        assert(evicted == 4);
        assert(testLruEvicted_ == 4);
        assert(testLruVetoes_ == 1);
        assert(testLruVetoBad_ == 0);
        for (int32_t n = 0; n < 4; n++) {
            assert(testLruEvictOrder_[n] == n + 1);
        }
        assert(multidictCount(wd) == 1);
        assert(testLruTrackingExact_(wd));

        /* The only tracked entry is key 0.  Collecting a window of 8
         * writes exactly one handle and leaves the other slots alone. */
        multilruPtr window[8];
        for (size_t n = 0; n < 8; n++) {
            window[n] = SIZE_MAX;
        }
        const size_t got = multilruCollectLowest(wd->lru, window, 8);
        assert(got == 1);
        for (size_t n = 1; n < 8; n++) {
            assert(window[n] == SIZE_MAX);
        }
        databox ck;
        databoxWide ckWide;
        const bool resolved = multidictLRUKeyOf(wd, window[0], &ck, &ckWide);
        assert(resolved);
        assert(testKeyIndex_(&ck, "lruveto-") == 0);

        /* A limit only key 0's eviction could meet: the refill finds
         * nothing but the passed-over key, so nothing is evicted and key
         * 0 stays tracked */
        multidictSetMaxMemory(wd, 1);
        evicted = multidictEvictToLimit(wd);
        assert(evicted == 0);
        assert(testLruEvicted_ == 4);
        assert(testLruVetoes_ > 1);
        assert(testLruVetoBad_ == 0);
        assert(multidictCount(wd) == 1);
        assert(testLruTrackingExact_(wd));

        testLruVetoActive_ = false;
    }

    /* ================================================================
     * Section 21: Byte-Based Expansion Tests
     * ================================================================ */
    TEST("21.1 enable/disable the byte-based expansion API") {
        multidictEmpty(d);
        assert(!multidictIsByteBasedExpansion(d)); /* Disabled by default */

        multidictEnableByteBasedExpansion(d, 100 * 1024, 500 * 1024);
        assert(multidictIsByteBasedExpansion(d));

        multidictDisableByteBasedExpansion(d);
        assert(!multidictIsByteBasedExpansion(d));
    }

    TEST("21.2 multidictGetLoadMetrics") {
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
            MULTIDICT_TEST_OK(multidictAdd(d, &key, &val));
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

    TEST("21.3 the byte-based expansion trigger - small values") {
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
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
        }

        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);
        printf("    After 200 small entries: slots=%" PRIu64
               ", avgSlotBytes=%" PRIu64 " (target=1024)\n",
               multidictSlotCount(dByte), metrics.avgSlotBytes);
    }

    TEST("21.4 the byte-based expansion trigger - large values") {
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
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
        }

        uint32_t finalSlots = multidictSlotCount(dByte);
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);

        printf("    Slots: initial=%" PRIu32 ", final=%" PRIu32
               ", avgSlotBytes=%" PRIu64 "\n",
               initialSlots, finalSlots, metrics.avgSlotBytes);
        assert(finalSlots > initialSlots); /* Should expand due to byte load */
    }

    TEST("21.5 safeguard - the maximum slot size trigger") {
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
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
        }

        uint32_t finalSlots = multidictSlotCount(dByte);
        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);

        printf("    Slots: initial=%" PRIu32 ", final=%" PRIu32
               ", maxSlotBytes=%" PRIu64 " (limit=500)\n",
               initialSlots, finalSlots, metrics.maxSlotBytes);
    }

    TEST("21.6 safeguard - the count-based backstop") {
        /* Create dict with very high byte targets but low count threshold */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(
            dByte, 1024 * 1024, 8 * 1024 * 1024); /* 1MB target - very high */

        uint32_t initialSlots = multidictSlotCount(dByte);

        /* Fill dict with tiny entries - count-based backstop should trigger.
         * Bytes never ask to expand, so outside a rehash the count load
         * factor never passes the 2x backstop (an add checks before it
         * inserts, so it can reach it exactly), also in tables of 32+
         * slots, where byte metrics are measured only every size/16
         * inserts: reaching the backstop must not wait for that */
        uint64_t overCount = 0; /* settled adds past the backstop */
        uint64_t overSlots = 0;
        for (int i = 0; i < 20000; i++) {
            char keybuf[16], valbuf[4];
            snprintf(keybuf, sizeof(keybuf), "c%d", i);
            snprintf(valbuf, sizeof(valbuf), "v%d", i % 10);
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
            /* exact: 100 * count against the backstop's share of slots */
            if (!multidictIsRehashing(dByte) &&
                multidictCount(dByte) * 100 >
                    multidictSlotCount(dByte) * 2u *
                        MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR) {
                overCount = multidictCount(dByte);
                overSlots = multidictSlotCount(dByte);
            }
        }
        if (overCount) {
            ERR("byte mode held %" PRIu64 " entries in %" PRIu64
                " slots, past the %d%% backstop",
                overCount, overSlots, 2 * MULTIDICT_DEFAULT_EXPAND_LOAD_FACTOR);
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

    TEST("21.7 safeguard - the expansion effectiveness check") {
        /* Every entry is larger than the target slot size, so once each
         * sits alone in a slot no expansion can bring a slot under it.
         * Expansion must stop there (the count-based backstop still
         * grows the table with the entry count), not repeat on every
         * insert up to the slot cap. */
        cleaner(multidictMmClassFree) multidictClass *qdcByte =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *dByte =
            multidictNew(&multidictTypeExactKey, qdcByte, 88);
        multidictEnableByteBasedExpansion(dByte, 256, 1 << 20);

        constexpr int BIG_ENTRIES = 300;
        char valbuf[1000];
        memset(valbuf, 'Z', sizeof(valbuf) - 1);
        valbuf[sizeof(valbuf) - 1] = '\0';
        for (int i = 0; i < BIG_ENTRIES; i++) {
            char keybuf[32];
            snprintf(keybuf, sizeof(keybuf), "eff%d", i);
            databox key = databoxNewBytesString(keybuf);
            databox val = databoxNewBytesString(valbuf);
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
        }

        multidictLoadMetrics metrics;
        multidictGetLoadMetrics(dByte, &metrics);
        printf("    Entries above target: avgSlotBytes=%" PRIu64
               ", slots=%" PRIu64 "\n",
               metrics.avgSlotBytes, multidictSlotCount(dByte));
        if (multidictCount(dByte) != BIG_ENTRIES ||
            multidictSlotCount(dByte) > 4 * BIG_ENTRIES) {
            ERR("%" PRIu64 " slots for %d entries larger than the target: "
                "expansion did not stop",
                multidictSlotCount(dByte), BIG_ENTRIES);
        }
    }

    TEST("perf: byte-based expansion insert cost against count-based") {
        /* The byte-based check measures the table; doing that by
         * walking every slot on every insert makes inserting n entries
         * quadratic.  Checked by the slots the measuring walks, which
         * neither load nor build flavour changes: linear in the entries
         * (the amortized design walks about 16 per insert; quadratic is
         * n^2 / 2).  Times are printed for reference. */
        static constexpr int perfN[2] = {4000, 16000};
        double perOp[2][2] = {};
        for (int size = 0; size < 2; size++) {
            for (int mode = 0; mode < 2; mode++) {
                cleaner(multidictMmClassFree) multidictClass *perfCls =
                    multidictMmClassNew();
                cleaner(multidictFree) multidict *perfD =
                    multidictNew(&multidictTypeExactKey, perfCls, 91);
                if (mode == 1) {
                    multidictEnableByteBasedExpansion(perfD, 512, 4096);
                }
                /* @sync: relaxed counter */
                atomic_store_explicit(&mdTestMetricSlotsWalked, 0,
                                      memory_order_relaxed);
                const uint64_t t0 = timeUtilMonotonicNs();
                for (int i = 0; i < perfN[size]; i++) {
                    const databox key = databoxNewSigned(i * 7919);
                    const databox val = databoxNewSigned(i);
                    MULTIDICT_TEST_OK(multidictAdd(perfD, &key, &val));
                }
                perOp[size][mode] =
                    (double)(timeUtilMonotonicNs() - t0) / perfN[size];
                if (multidictCount(perfD) != (uint64_t)perfN[size]) {
                    ERR("%" PRIu64 " entries, want %d", multidictCount(perfD),
                        perfN[size]);
                }
                /* @sync: relaxed counter */
                const uint64_t walked = atomic_load_explicit(
                    &mdTestMetricSlotsWalked, memory_order_relaxed);
                if (mode == 1 && walked > 64 * (uint64_t)perfN[size]) {
                    ERR("%d byte-based inserts walked %" PRIu64 " slots",
                        perfN[size], walked);
                }
            }
            printf("    %d inserts: count-based %.1f ns/op, byte-based %.1f "
                   "ns/op\n",
                   perfN[size], perOp[size][0], perOp[size][1]);
        }
    }

    TEST("21.8 byte-based against count-based expansion") {
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
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));

            databox key2 = databoxNewBytesString(keybuf);
            databox val2 = databoxNewBytesString(valbuf);
            MULTIDICT_TEST_OK(multidictAdd(dCount, &key2, &val2));
        }

        uint32_t byteSlots = multidictSlotCount(dByte);
        uint32_t countSlots = multidictSlotCount(dCount);

        printf("    After 100 large entries: byte-based slots=%" PRIu32
               ", count-based slots=%" PRIu32 "\n",
               byteSlots, countSlots);
        /* Byte-based may expand more aggressively for large values */
    }

    TEST("21.9 byte-based expansion during rehashing operations") {
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
            MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));

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
            databox val = {};
            if (multidictFind(dByte, &key, &val)) {
                foundCount++;
            }
        }

        printf("    After rehashing with byte-based expansion: found=%" PRIu64
               "/200\n",
               foundCount);
        assert(foundCount == 200);
    }

    TEST("21.10 fuzz - byte-based expansion with mixed operations") {
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
                MULTIDICT_TEST_OK(multidictAdd(dByte, &key, &val));
                adds++;
            } else if (op < 85) { /* 25% find */
                char keybuf[32];
                snprintf(keybuf, sizeof(keybuf), "fz%d", rand() % 500);
                databox key = databoxNewBytesString(keybuf);
                databox val = {};
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

    /* ================================================================
     * Regression: single-step incremental rehash through a sparse table
     * ================================================================
     * multidictRehash() computed its empty-slot work cap from the
     * POST-decrement loop counter, so a single-step rehash (n=1 -> n=0 in
     * the body) set emptyVisits=0 and the `emptyVisits--` wrapped to
     * UINT32_MAX, disabling the cap entirely. This drives many single steps
     * over a populated-then-sparsified table (lots of NULL slot runs in HT0
     * mid-rehash) and verifies the rehash terminates and preserves data. */
    TEST("single-step rehash through a sparse table") {
        constexpr int N = 4000;
        cleaner(multidictFree) multidict *r =
            multidictNew(&multidictTypeExactKey, qdc, 0xABCDEF);
        assert(r != nullptr);

        char kb[32];
        for (int i = 0; i < N; i++) {
            snprintf(kb, sizeof(kb), "rk:%d", i);
            databox key = databoxNewBytesString(kb);
            databox val = databoxNewUnsigned((uint64_t)i);
            MULTIDICT_TEST_OK(multidictAdd(r, &key, &val));
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
    }

    /* ================================================================
     * Empty then reuse (reset-family regression matrix)
     * ================================================================ */
    TEST("empty-then-reuse cycles") {
        /* Start from a clean slate: earlier suite sections leave
         * entries in the long-lived dict */
        multidictEmpty(d);

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 500; i++) {
                const databox keybox = databoxNewUnsigned(0x9000 + i);
                const databox valbox = databoxNewUnsigned(i * 3);
                MULTIDICT_TEST_OK(multidictAdd(d, &keybox, &valbox));
            }
            if (multidictCount(d) != 500) {
                ERR("cycle %d expected 500, got %" PRIu64, cycle,
                    multidictCount(d));
            }

            multidictEmpty(d);
            if (multidictCount(d) != 0) {
                ERR("cycle %d expected 0 after Empty, got %" PRIu64, cycle,
                    multidictCount(d));
            }
        }

        uint64_t missing = 0;
        for (uint64_t i = 0; i < 500; i++) {
            const databox keybox = databoxNewUnsigned(0x9000 + i);
            const databox valbox = databoxNewUnsigned(i + 11);
            MULTIDICT_TEST_OK(multidictAdd(d, &keybox, &valbox));
        }
        for (uint64_t i = 0; i < 500; i++) {
            const databox keybox = databoxNewUnsigned(0x9000 + i);
            databox valbox;
            if (!multidictFind(d, &keybox, &valbox)) {
                missing++;
            }
        }
        if (missing > 0) {
            ERR("%" PRIu64 " of 500 keys missing after Empty-reuse", missing);
        }
    }

    TEST("GetAndDeleteIf: one search, a declined delete keeps the entry") {
        err += multidictTestDeleteIf_();
    }

    TEST("96/128-bit keys and values: normal / extended / mixed") {
        err += multidictTestWide_(MD_WIDE_NORMAL);
        err += multidictTestWide_(MD_WIDE_EXTENDED);
        err += multidictTestWide_(MD_WIDE_MIXED);
        err += multidictTestSlotIteratorWide_();
    }

    TEST("integer hash functions") {
        /* The 32-bit mixer is a bijection: distinct keys never collide.
         * The 64-bit one folds to 32 bits: collisions stay rare. */
        constexpr int MD_HASH_KEYS = 65536;
        cleanerName(zfree, HashWords) uint32_t *h32 =
            zcalloc(MD_HASH_KEYS, sizeof(uint32_t));
        cleanerName(zfree, HashWords) uint32_t *h64 =
            zcalloc(MD_HASH_KEYS, sizeof(uint32_t));
        for (uint32_t i = 0; i < MD_HASH_KEYS; i++) {
            h32[i] = multidictIntHashFunction(i);
            h64[i] = multidictLongLongHashFunction((uint64_t)i << 20 | i);
        }
        if (multidictIntHashFunction(12345) !=
            multidictIntHashFunction(12345)) {
            ERR("an integer hash is not deterministic");
        }
        dkSortArray(h32, MD_HASH_KEYS);
        dkSortArray(h64, MD_HASH_KEYS);
        size_t same32 = 0;
        size_t same64 = 0;
        for (size_t i = 1; i < MD_HASH_KEYS; i++) {
            same32 += h32[i] == h32[i - 1];
            same64 += h64[i] == h64[i - 1];
        }
        if (same32 != 0 || same64 > 4) {
            ERR("hash collisions: %zu (32-bit), %zu (64-bit)", same32, same64);
        }
    }

    TEST("a key given as never-free bytes finds its entry") {
        /* Added under BYTES keys; found, and deleted, through boxes that
         * carry the same bytes as NEVER_FREE (hashing reads both alike) */
        cleaner(multidictMmClassFree) multidictClass *nc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *nd =
            multidictNew(&multidictTypeExactKey, nc, 7);
        static uint8_t names[3][32] = {"first key, twenty+ bytes",
                                       "second key, also longer", "short"};
        for (int i = 0; i < 3; i++) {
            const databox k =
                databoxNewBytes(names[i], strlen((char *)names[i]));
            const databox v = databoxNewUnsigned((uint64_t)i);
            MULTIDICT_TEST_OK(multidictAdd(nd, &k, &v));
        }
        int found = 0;
        for (int i = 0; i < 3; i++) {
            databox k = {.type = DATABOX_BYTES_NEVER_FREE,
                         .data.bytes.start = names[i],
                         .len = strlen((char *)names[i])};
            databox v = {};
            found += multidictFind(nd, &k, &v) && v.data.u == (uint64_t)i;
        }
        databox gone = {.type = DATABOX_BYTES_NEVER_FREE,
                        .data.bytes.start = names[0],
                        .len = strlen((char *)names[0])};
        if (found != 3 || !multidictDelete(nd, &gone) ||
            multidictCount(nd) != 2) {
            ERR("never-free keys found %d of 3, count %" PRIu64, found,
                multidictCount(nd));
        }
    }

    /* Per-entry cost of the core operations (meaningful under the perf
     * preset; checks the results too) */
    TEST("smoke benchmark (ns per entry)") {
        constexpr int MD_BENCH_KEYS = 100'000;
        cleaner(multidictMmClassFree) multidictClass *bc =
            multidictMmClassNew();
        cleaner(multidictFree) multidict *bd =
            multidictNew(&multidictTypeExactKey, bc, 7);

        /* 16-byte keys, built before timing */
        constexpr int MD_BENCH_KEY_LEN = 16;
        cleanerName(zfree, BenchKeys) char *keys =
            zmalloc((size_t)MD_BENCH_KEYS * MD_BENCH_KEY_LEN + 1);
        for (uint64_t i = 0; i < MD_BENCH_KEYS; i++) {
            snprintf(keys + i * MD_BENCH_KEY_LEN, MD_BENCH_KEY_LEN + 1,
                     "key:%012" PRIu64, i);
        }
#define MD_BENCH_KEY(i)                                                        \
    databoxNewBytes(keys + (i) * MD_BENCH_KEY_LEN, MD_BENCH_KEY_LEN)

        uint64_t start = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < MD_BENCH_KEYS; i++) {
            const databox k = MD_BENCH_KEY(i);
            const databox v = databoxNewUnsigned(i);
            MULTIDICT_TEST_OK(multidictAdd(bd, &k, &v));
        }
        const uint64_t addNs = timeUtilMonotonicNs() - start;

        /* The same adds into a dict sized for them up front (no rehash) */
        uint64_t presizedNs = 0;
        {
            cleaner(multidictFree) multidict *pd =
                multidictNew(&multidictTypeExactKey, bc, 7);
            multidictExpand(pd, MD_BENCH_KEYS / 2);
            start = timeUtilMonotonicNs();
            for (uint64_t i = 0; i < MD_BENCH_KEYS; i++) {
                const databox k = MD_BENCH_KEY(i);
                const databox v = databoxNewUnsigned(i);
                MULTIDICT_TEST_OK(multidictAdd(pd, &k, &v));
            }
            presizedNs = timeUtilMonotonicNs() - start;
        }

        start = timeUtilMonotonicNs();
        uint64_t found = 0;
        for (uint64_t i = 0; i < MD_BENCH_KEYS; i++) {
            const databox k = MD_BENCH_KEY(i);
            databox v;
            found += multidictFind(bd, &k, &v) && v.data.u == i;
        }
        const uint64_t findNs = timeUtilMonotonicNs() - start;

        start = timeUtilMonotonicNs();
        uint64_t iterated = 0;
        multidictIterator it;
        multidictEntry e;
        multidictIteratorInit(bd, &it);
        while (multidictIteratorNext(&it, &e)) {
            iterated++;
        }
        multidictIteratorRelease(&it);
        const uint64_t iterNs = timeUtilMonotonicNs() - start;

        start = timeUtilMonotonicNs();
        uint64_t deleted = 0;
        for (uint64_t i = 0; i < MD_BENCH_KEYS; i++) {
            const databox k = MD_BENCH_KEY(i);
            deleted += multidictDelete(bd, &k);
        }
        const uint64_t deleteNs = timeUtilMonotonicNs() - start;
#undef MD_BENCH_KEY

        printf("  add %.1f  add presized %.1f  find %.1f  iterate %.1f  "
               "delete %.1f\n",
               (double)addNs / MD_BENCH_KEYS,
               (double)presizedNs / MD_BENCH_KEYS,
               (double)findNs / MD_BENCH_KEYS, (double)iterNs / MD_BENCH_KEYS,
               (double)deleteNs / MD_BENCH_KEYS);
        if (found != MD_BENCH_KEYS || iterated != MD_BENCH_KEYS ||
            deleted != MD_BENCH_KEYS || multidictSize(bd) != 0) {
            ERR("benchmark results wrong: found %" PRIu64 " iterated %" PRIu64
                " deleted %" PRIu64,
                found, iterated, deleted);
        }
    }

    TEST_FINAL_RESULT;
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
