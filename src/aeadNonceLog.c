/* aeadNonceLog — see aeadNonceLog.h.
 *
 * Internal layout: open-addressing hash set with linear probing for
 * (key_id, nonce) lookup, layered with a FIFO doubly-linked list
 * (indexed by slot) that makes eviction O(1) and gives snapshots their
 * order (oldest first).
 *
 * Insertion ordering:
 * - Probe FIRST. If the tuple is already present we return duplicate
 * and do NOT touch the table, so an eviction can never remove the
 * candidate's own earlier observation before the probe sees it.
 * - If not found and load >= 7/8 capacity, evict the FIFO head
 * (oldest) in O(1).
 * - Insert at the FIFO tail.
 *
 * Removal (eviction, ClearKey) uses backward-shift deletion: the
 * entries after the emptied slot in its probe run move back over it
 * when their home slot allows, so every entry stays reachable from its
 * home slot without tombstones.  (Merely emptying the slot would end
 * the probe run there and hide the entries past it from lookup, and a
 * nonce reused within the window would go undetected.)
 *
 * A slot is occupied iff key_id != 0 (0 is never a valid key id).  Each
 * slot keeps its tuple's hash, so probes compare hashes before nonces
 * and backward shifts read home slots without rehashing.
 *
 * Thread-safety: single mutex covers the table and the FIFO list.
 * Snapshot/restore takes the same lock during serialization. */

#include "aeadNonceLog.h"

#include "cleaner.h"
#include "ctmem.h" /* dkCtMemEq for nonce compare */
#include "databox.h"
#include "datakit.h"
#include "dkEndian.h" /* dkStoreLe64 */
#include "dkRandom.h" /* dkSplitmix64Mix */
#include "flex.h"
#include "persist.h"

#include <pthread.h>
#include <string.h>

#define AEAD_NONCE_LOG_MIN_CAP ((size_t)64)

/* SIZE_MAX is the sentinel for "no link" in the FIFO list. */
#define AEAD_NONCE_LOG_NIL ((size_t)SIZE_MAX)

typedef struct aeadNonceLogSlot {
    uint64_t hash;       /* aeadNonceLogHash(key_id, generation, nonce) */
    uint64_t generation; /* which key key_id named (0: not told) */
    hmacKeyId key_id;    /* HMAC_KEY_ID_INVALID (0) marks an empty slot */
    uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
} aeadNonceLogSlot;

/* FIFO doubly-linked list nodes parallel to the slot array. Stored in
 * a separate allocation so the secure-wipe pass over slots doesn't
 * need to special-case link bytes, and so links survive eviction wipes
 * (we explicitly reset them on each eviction/insert anyway). */
typedef struct aeadNonceLogLink {
    size_t prev; /* AEAD_NONCE_LOG_NIL if no predecessor (FIFO head) */
    size_t next; /* AEAD_NONCE_LOG_NIL if no successor (FIFO tail) */
} aeadNonceLogLink;

struct aeadNonceLog {
    pthread_mutex_t lock;
    aeadNonceLogSlot *slots;
    aeadNonceLogLink *links;
    size_t cap;
    size_t mask;
    size_t count;
    size_t fifo_head; /* oldest occupied slot; AEAD_NONCE_LOG_NIL if empty */
    size_t fifo_tail; /* newest occupied slot; AEAD_NONCE_LOG_NIL if empty */
};

/* Scope-exit cleaners (see cleaner.h idioms). zfree-based cleaners are
 * for raw allocations before ownership transfers into a constructed
 * log; aeadNonceLogFree is only safe on a FULLY constructed log (it
 * wipes cap bytes and destroys the mutex), so partial-construction
 * paths scope the pieces individually instead. */
defcleanerName(aeadNonceLog *, zfree, Log);
defcleanerName(aeadNonceLogSlot *, zfree, Slots);
defcleanerName(aeadNonceLogLink *, zfree, Links);
defcleanerName(uint8_t *, zfree, Bytes);
defcleaner(aeadNonceLog *, aeadNonceLogFree);

/* Round up to next power of two, with a floor at AEAD_NONCE_LOG_MIN_CAP
 * (the caller has checked req <= AEAD_NONCE_LOG_MAX_CAP). */
static size_t aeadNonceLogRoundCap(size_t req) {
    static_assert((AEAD_NONCE_LOG_MIN_CAP & (AEAD_NONCE_LOG_MIN_CAP - 1)) == 0,
                  "the floor is itself a power of two");
    const size_t c = stdc_bit_ceil(req);
    return c < AEAD_NONCE_LOG_MIN_CAP ? AEAD_NONCE_LOG_MIN_CAP : c;
}

/* Most entries the table holds: at this load the oldest is evicted
 * before an insert, so a probe always meets an empty slot */
static size_t aeadNonceLogLoadLimit(size_t cap) {
    return cap - (cap >> 3u);
}

/* Hash the (key_id, generation, nonce) tuple with splitmix64's
 * finalizer: non-cryptographic, but it spreads the bits over the
 * buckets. */
static uint64_t aeadNonceLogHash(hmacKeyId key_id, uint64_t generation,
                                 const uint8_t *nonce) {
    uint64_t lo, hi;
    memcpy(&lo, nonce, 8);
    uint32_t hi32 = 0;
    memcpy(&hi32, nonce + 8, 4);
    hi = ((uint64_t)hi32 << 32) | (uint64_t)key_id;
    return dkSplitmix64Mix(lo ^
                           dkSplitmix64Mix(hi ^ dkSplitmix64Mix(generation)));
}

/* The occupied slot `s` holds (key_id, generation, nonce), whose hash is
 * `h` */
static bool aeadNonceLogSlotMatches(const aeadNonceLogSlot *s, uint64_t h,
                                    hmacKeyId key_id, uint64_t generation,
                                    const uint8_t *nonce) {
    return s->hash == h && s->key_id == key_id && s->generation == generation &&
           dkCtMemEq(s->nonce, nonce, AEAD_NONCE_LOG_NONCE_LEN);
}

/* ---- FIFO list helpers (caller must hold the lock) -------------------- */

/* Append slot `idx` at the FIFO tail. The slot must currently be
 * unlinked (i.e. just-evicted or freshly-occupied). */
static void aeadNonceLogFifoPushTail(aeadNonceLog *log, size_t idx) {
    log->links[idx].next = AEAD_NONCE_LOG_NIL;
    log->links[idx].prev = log->fifo_tail;
    if (log->fifo_tail != AEAD_NONCE_LOG_NIL) {
        log->links[log->fifo_tail].next = idx;
    } else {
        /* List was empty — `idx` is now both head and tail. */
        log->fifo_head = idx;
    }
    log->fifo_tail = idx;
}

/* Unlink slot `idx` from the FIFO list. Slot remains occupied; caller
 * decides whether to wipe and free it afterwards. */
static void aeadNonceLogFifoUnlink(aeadNonceLog *log, size_t idx) {
    const size_t prev = log->links[idx].prev;
    const size_t next = log->links[idx].next;
    if (prev != AEAD_NONCE_LOG_NIL) {
        log->links[prev].next = next;
    } else {
        /* idx was the head */
        log->fifo_head = next;
    }
    if (next != AEAD_NONCE_LOG_NIL) {
        log->links[next].prev = prev;
    } else {
        /* idx was the tail */
        log->fifo_tail = prev;
    }
    log->links[idx].prev = AEAD_NONCE_LOG_NIL;
    log->links[idx].next = AEAD_NONCE_LOG_NIL;
}

aeadNonceLog *aeadNonceLogCreate(size_t min_capacity) {
    if (min_capacity > AEAD_NONCE_LOG_MAX_CAP) {
        return nullptr;
    }
    const size_t cap = aeadNonceLogRoundCap(min_capacity);
    cleanerName(zfree, Log) aeadNonceLog *log = zmalloc(sizeof(*log));
    cleanerName(zfree, Slots) aeadNonceLogSlot *slots =
        log ? zmalloc(cap * sizeof(*slots)) : nullptr;
    cleanerName(zfree, Links) aeadNonceLogLink *links =
        log ? zmalloc(cap * sizeof(*links)) : nullptr;
    if (!log || !slots || !links) {
        /* OOM: fail closed (return NULL) instead of dereferencing NULL below,
         * so the caller's fail-closed path runs — e.g. clusterMailboxCreate
         * refuses to construct a mailbox without replay protection rather than
         * crashing under memory pressure. */
        return nullptr;
    }
    memset(slots, 0, cap * sizeof(*slots));
    for (size_t i = 0; i < cap; i++) {
        links[i].prev = AEAD_NONCE_LOG_NIL;
        links[i].next = AEAD_NONCE_LOG_NIL;
    }
    if (pthread_mutex_init(&log->lock, nullptr) != 0) {
        return nullptr;
    }
    log->slots = slots;
    slots = nullptr;
    log->links = links;
    links = nullptr;
    log->cap = cap;
    log->mask = cap - 1u;
    log->count = 0;
    log->fifo_head = AEAD_NONCE_LOG_NIL;
    log->fifo_tail = AEAD_NONCE_LOG_NIL;
    aeadNonceLog *out = log;
    log = nullptr;
    return out;
}

void aeadNonceLogFree(aeadNonceLog *log) {
    if (log == nullptr) {
        return;
    }
    /* Wipe slot contents before free — leaked log should not leak
     * observed nonces. */
    dkSecureZero(log->slots, log->cap * sizeof(aeadNonceLogSlot));
    pthread_mutex_destroy(&log->lock);
    zfree(log->links);
    zfree(log->slots);
    zfree(log);
}

/* Zero a slot's bytes (observed nonces never linger in freed slots) */
static void aeadNonceLogWipeSlot(aeadNonceLog *log, size_t idx) {
    dkSecureZero(&log->slots[idx], sizeof(aeadNonceLogSlot));
}

/* Move the occupied slot `from` to the vacated slot `to`, keeping its
 * FIFO position (the neighbours, head and tail follow it).  `from` is
 * left for the next move to overwrite or the remover to wipe. */
static void aeadNonceLogMoveSlot(aeadNonceLog *log, size_t from, size_t to) {
    log->slots[to] = log->slots[from];
    log->links[to] = log->links[from];

    const size_t prev = log->links[to].prev;
    const size_t next = log->links[to].next;
    if (prev != AEAD_NONCE_LOG_NIL) {
        log->links[prev].next = to;
    } else {
        log->fifo_head = to;
    }
    if (next != AEAD_NONCE_LOG_NIL) {
        log->links[next].prev = to;
    } else {
        log->fifo_tail = to;
    }
}

/* Remove the entry in slot `idx` (caller holds the lock).  Backward-shift
 * deletion: each later entry of the probe run whose home slot is not in
 * (hole, entry] cyclically moves back into the hole, so lookups from any
 * home slot still reach every entry.  Returns the one slot the removal
 * leaves empty (wiped): `idx` itself, or the last slot an entry moved
 * out of. */
static size_t aeadNonceLogRemoveSlot(aeadNonceLog *log, size_t idx) {
    aeadNonceLogFifoUnlink(log, idx);
    log->count--;

    size_t hole = idx;
    size_t j = idx;
    for (;;) {
        j = (j + 1u) & log->mask;
        const aeadNonceLogSlot *s = &log->slots[j];
        if (s->key_id == 0u) {
            break;
        }
        const size_t home = (size_t)(s->hash & log->mask);
        const bool stays =
            hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (!stays) {
            aeadNonceLogMoveSlot(log, j, hole);
            hole = j;
        }
    }

    aeadNonceLogWipeSlot(log, hole);
    log->links[hole].prev = AEAD_NONCE_LOG_NIL;
    log->links[hole].next = AEAD_NONCE_LOG_NIL;
    return hole;
}

/* Probe for (key_id, nonce), whose hash is `h`, from its home slot.  True
 * when present; otherwise *empty is the first empty slot of its probe
 * run (SIZE_MAX only if the table has no empty slot, which the load
 * limit rules out). */
static bool aeadNonceLogFind(const aeadNonceLog *log, uint64_t h,
                             hmacKeyId key_id, uint64_t generation,
                             const uint8_t *nonce, size_t *empty) {
    size_t idx = (size_t)(h & log->mask);
    for (size_t step = 0; step < log->cap; step++) {
        const aeadNonceLogSlot *s = &log->slots[idx];
        if (s->key_id == 0u) {
            *empty = idx;
            return false;
        }
        if (aeadNonceLogSlotMatches(s, h, key_id, generation, nonce)) {
            return true;
        }
        idx = (idx + 1u) & log->mask;
    }
    *empty = SIZE_MAX;
    return false;
}

/* Fill the empty slot `idx` and append it to the FIFO tail */
static void aeadNonceLogPlace(aeadNonceLog *log, size_t idx, uint64_t h,
                              hmacKeyId key_id, uint64_t generation,
                              const uint8_t *nonce) {
    aeadNonceLogSlot *s = &log->slots[idx];
    s->hash = h;
    s->generation = generation;
    s->key_id = key_id;
    memcpy(s->nonce, nonce, AEAD_NONCE_LOG_NONCE_LEN);
    aeadNonceLogFifoPushTail(log, idx);
    log->count++;
}

bool aeadNonceLogObserve(aeadNonceLog *log, hmacKeyId key_id,
                         const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]) {
    return aeadNonceLogObserveAt(log, key_id, 0, nonce);
}

bool aeadNonceLogObserveAt(aeadNonceLog *log, hmacKeyId key_id,
                           uint64_t generation,
                           const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]) {
    if (log == nullptr || nonce == nullptr || key_id == 0u) {
        return false;
    }
    const uint64_t h = aeadNonceLogHash(key_id, generation, nonce);
    pthread_mutex_lock(&log->lock);

    /* Probe first: a duplicate leaves the table untouched */
    size_t empty = SIZE_MAX;
    if (aeadNonceLogFind(log, h, key_id, generation, nonce, &empty)) {
        pthread_mutex_unlock(&log->lock);
        return false;
    }

    /* At the load limit, evict the oldest entry.  The removal leaves one
     * new empty slot (every other slot stays occupied or empty), so the
     * first empty of this probe run is that slot when it lies in
     * [home, empty), and `empty` otherwise. */
    if (log->count >= aeadNonceLogLoadLimit(log->cap) && empty != SIZE_MAX) {
        const size_t freed = aeadNonceLogRemoveSlot(log, log->fifo_head);
        const size_t home = (size_t)(h & log->mask);
        if (((freed - home) & log->mask) < ((empty - home) & log->mask)) {
            empty = freed;
        }
    }

    /* No empty slot means a broken table: refuse, as for a reuse */
    const bool placed = empty != SIZE_MAX;
    if (placed) {
        aeadNonceLogPlace(log, empty, h, key_id, generation, nonce);
    }
    pthread_mutex_unlock(&log->lock);
    return placed;
}

void aeadNonceLogClearKey(aeadNonceLog *log, hmacKeyId key_id) {
    if (log == nullptr || key_id == 0u) {
        return;
    }
    pthread_mutex_lock(&log->lock);
    /* A removal can shift a later entry into slot i, so i advances only
     * past a slot that stays */
    for (size_t i = 0; i < log->cap;) {
        if (log->slots[i].key_id == key_id) {
            aeadNonceLogRemoveSlot(log, i);
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&log->lock);
}

size_t aeadNonceLogSize(const aeadNonceLog *log) {
    if (log == nullptr) {
        return 0;
    }
    pthread_mutex_lock((pthread_mutex_t *)&log->lock);
    const size_t n = log->count;
    pthread_mutex_unlock((pthread_mutex_t *)&log->lock);
    return n;
}

size_t aeadNonceLogCapacity(const aeadNonceLog *log) {
    if (log == nullptr) {
        return 0;
    }
    return log->cap;
}

/* ========================================================================
 * Persistence (flex serialization + persistOps descriptor)
 * ======================================================================== */

/* Entries per record in the serialized flex: nonce, key_id, generation */
#define AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD ((size_t)3)

/* Build the flex encoding under the lock: [capacity], then each entry
 * oldest first.  Caller owns and frees via flexFree. */
static flex *anlBuildSnapshotFlex(const aeadNonceLog *log) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    pthread_mutex_lock((pthread_mutex_t *)&log->lock);

    flexRow row = FLEX_ROW_INIT;
    flexRowUnsigned(&row, (uint64_t)log->cap);
    flexRowAppendExact(&f, &row);

    for (size_t i = log->fifo_head; i != AEAD_NONCE_LOG_NIL;
         i = log->links[i].next) {
        const aeadNonceLogSlot *s = &log->slots[i];
        flexRowBytes(&row, s->nonce, AEAD_NONCE_LOG_NONCE_LEN);
        flexRowUnsigned(&row, (uint64_t)s->key_id);
        flexRowUnsigned(&row, s->generation);
        flexRowAppendExact(&f, &row);
    }

    pthread_mutex_unlock((pthread_mutex_t *)&log->lock);
    return f;
}

size_t aeadNonceLogToBytes(const aeadNonceLog *log, uint8_t *out,
                           size_t out_cap) {
    if (log == nullptr) {
        return 0;
    }
    flex *f = anlBuildSnapshotFlex(log);
    const size_t need = flexBytes(f);
    if (out == nullptr) {
        flexFree(f);
        return need;
    }
    if (out_cap < need) {
        flexFree(f);
        return 0;
    }
    memcpy(out, f, need);
    flexFree(f);
    return need;
}

aeadNonceLog *aeadNonceLogFromBytes(const uint8_t *buf, size_t len) {
    if (buf == nullptr) {
        return nullptr;
    }
    /* The bytes may come from disk or a peer: bound every entry, not
     * just the header, before walking them below */
    if (!flexValidateBufferDeep(buf, len)) {
        return nullptr;
    }
    const flex *f = (const flex *)buf;
    const size_t total = flexCount(f);
    if (total == 0 ||
        (total - 1) % AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD != 0u) {
        return nullptr;
    }
    const size_t records =
        (total - 1) / AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD;

    /* [0] capacity: a power of two in [MIN, MAX] (so a damaged field
     * cannot size an allocation), holding the records within the load
     * limit, as a live log does */
    flexEntry *fe = flexHead(f);
    uint64_t cap = 0;
    if (!flexGetUnsigned(fe, &cap) || cap < AEAD_NONCE_LOG_MIN_CAP ||
        cap > AEAD_NONCE_LOG_MAX_CAP || !stdc_has_single_bit(cap) ||
        records > aeadNonceLogLoadLimit((size_t)cap)) {
        return nullptr;
    }
    fe = flexNext(f, fe);

    cleaner(aeadNonceLogFree) aeadNonceLog *log =
        aeadNonceLogCreate((size_t)cap);
    if (log == nullptr) {
        return nullptr;
    }

    /* Records oldest first: [nonce (12 bytes), key_id (1..UINT32_MAX),
     * generation], each inserted at the FIFO tail; a repeated tuple is
     * refused */
    for (size_t i = 0; i < records; i++) {
        databoxBig nonceBox;
        flexGetByTypeBig(fe, &nonceBox);
        if (!DATABOX_IS_BYTES(&nonceBox) ||
            nonceBox.len != AEAD_NONCE_LOG_NONCE_LEN) {
            return nullptr;
        }
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
        memcpy(nonce, databoxBytes(&nonceBox), AEAD_NONCE_LOG_NONCE_LEN);
        fe = flexNext(f, fe);

        uint64_t key_id = 0;
        if (!flexGetUnsigned(fe, &key_id) || key_id == 0u ||
            key_id > UINT32_MAX) {
            return nullptr;
        }
        fe = flexNext(f, fe);

        uint64_t generation = 0;
        if (!flexGetUnsigned(fe, &generation)) {
            return nullptr;
        }
        fe = flexNext(f, fe);

        const uint64_t h =
            aeadNonceLogHash((uint32_t)key_id, generation, nonce);
        size_t empty = SIZE_MAX;
        if (aeadNonceLogFind(log, h, (uint32_t)key_id, generation, nonce,
                             &empty) ||
            empty == SIZE_MAX) {
            return nullptr;
        }
        aeadNonceLogPlace(log, empty, h, (uint32_t)key_id, generation, nonce);
    }

    aeadNonceLog *out = log;
    log = nullptr;
    return out;
}

/* ---- persistOps descriptor ----------------------------------------- */

static uint8_t *anlPersistSnapshot(const void *structure, size_t *len) {
    /* The snapshot flex is the persisted bytes; the caller frees it */
    flex *f = anlBuildSnapshotFlex((const aeadNonceLog *)structure);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *anlPersistRestore(const uint8_t *data, size_t len) {
    /* @own: caller; free=aeadNonceLogFree */
    return aeadNonceLogFromBytes(data, len);
}

static void *anlPersistNewEmpty(void) {
    /* @own: caller; free=aeadNonceLogFree */
    return aeadNonceLogCreate(0);
}

static size_t anlPersistCount(const void *structure) {
    return aeadNonceLogSize((const aeadNonceLog *)structure);
}

static size_t anlPersistEstimateSize(const void *structure) {
    return aeadNonceLogToBytes((const aeadNonceLog *)structure, nullptr, 0);
}

/* validate hook: the table's shape and load, every entry reachable from
 * its home slot as the first match (so no tuple repeats and no lookup
 * misses one), and a FIFO list through exactly the occupied slots. */
static bool anlPersistValidate(const void *structure) {
    const aeadNonceLog *log = (const aeadNonceLog *)structure;
    if (log == nullptr) {
        return false;
    }
    /* Capacity: a power of two in [MIN, MAX], within the load limit */
    if (log->cap < AEAD_NONCE_LOG_MIN_CAP ||
        log->cap > AEAD_NONCE_LOG_MAX_CAP || !stdc_has_single_bit(log->cap) ||
        log->mask != log->cap - 1u ||
        log->count > aeadNonceLogLoadLimit(log->cap)) {
        return false;
    }
    /* Every occupied slot holds its tuple's hash and is the first match
     * of a probe from its home, and the occupied slots number count */
    size_t live = 0;
    for (size_t i = 0; i < log->cap; i++) {
        const aeadNonceLogSlot *s = &log->slots[i];
        if (s->key_id == 0u) {
            continue;
        }
        live++;
        if (s->hash != aeadNonceLogHash(s->key_id, s->generation, s->nonce)) {
            return false;
        }
        for (size_t idx = (size_t)(s->hash & log->mask); idx != i;
             idx = (idx + 1u) & log->mask) {
            const aeadNonceLogSlot *o = &log->slots[idx];
            if (o->key_id == 0u ||
                aeadNonceLogSlotMatches(o, s->hash, s->key_id, s->generation,
                                        s->nonce)) {
                return false;
            }
        }
    }
    if (live != log->count) {
        return false;
    }
    /* FIFO list invariants: head/tail consistent with count;
     * traversing head->tail visits exactly `count` occupied slots. */
    if (log->count == 0u) {
        if (log->fifo_head != AEAD_NONCE_LOG_NIL ||
            log->fifo_tail != AEAD_NONCE_LOG_NIL) {
            return false;
        }
    } else {
        if (log->fifo_head == AEAD_NONCE_LOG_NIL ||
            log->fifo_tail == AEAD_NONCE_LOG_NIL) {
            return false;
        }
        if (log->fifo_head >= log->cap || log->fifo_tail >= log->cap) {
            return false;
        }
        if (log->links[log->fifo_head].prev != AEAD_NONCE_LOG_NIL) {
            return false;
        }
        if (log->links[log->fifo_tail].next != AEAD_NONCE_LOG_NIL) {
            return false;
        }
        size_t walked = 0;
        size_t cur = log->fifo_head;
        size_t prev = AEAD_NONCE_LOG_NIL;
        while (cur != AEAD_NONCE_LOG_NIL && walked <= log->count) {
            if (cur >= log->cap) {
                return false;
            }
            if (log->slots[cur].key_id == 0u) {
                return false;
            }
            if (log->links[cur].prev != prev) {
                return false;
            }
            walked++;
            prev = cur;
            cur = log->links[cur].next;
        }
        if (walked != log->count) {
            return false;
        }
    }
    return true;
}

static void anlPersistFree(void *structure) {
    aeadNonceLogFree((aeadNonceLog *)structure);
}

const persistOps persistOpsAeadNonceLog = {
    .type = PERSIST_TYPE_AEAD_NONCE_LOG,
    .name = "aeadNonceLog",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = anlPersistSnapshot,
    .restore = anlPersistRestore,
    .newEmpty = anlPersistNewEmpty,
    .count = anlPersistCount,
    .estimateSize = anlPersistEstimateSize,
    .encodeOp = nullptr, /* snapshot-only: each durable point is a snapshot */
    .applyOp = nullptr,
    .streamSnapshot = nullptr,
    .streamRestore = nullptr,
    .validate = anlPersistValidate,
    .free = anlPersistFree,
};

/* ========================================================================
 * Test driver
 * ======================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"

#include <stdio.h>
#include <stdlib.h>

static void nonceFromCounter(uint64_t v,
                             uint8_t out[AEAD_NONCE_LOG_NONCE_LEN]) {
    memset(out, 0, AEAD_NONCE_LOG_NONCE_LEN);
    dkStoreLe64(out, v);
}

typedef struct aeadNonceLogConcCtx {
    aeadNonceLog *log;
    uint64_t base;
    hmacKeyId key_id;
    uint32_t iters;
    uint32_t inserted;
} aeadNonceLogConcCtx;

static void *aeadNonceLogConcWorker(void *arg) {
    aeadNonceLogConcCtx *c = (aeadNonceLogConcCtx *)arg;
    uint8_t nonce[12] = {};
    for (uint32_t i = 0; i < c->iters; i++) {
        nonceFromCounter(c->base + (uint64_t)i, nonce);
        if (aeadNonceLogObserve(c->log, c->key_id, nonce)) {
            c->inserted++;
        }
    }
    return nullptr;
}

/* Always-failing allocators for the OOM fault-injection test below. zmalloc
 * routes through localCalloc unless DK_MALLOC_REAL is set, so fail both. */
static void *aeadOomFailMalloc(size_t) {
    return nullptr;
}
static void *aeadOomFailCalloc(size_t, size_t) {
    return nullptr;
}

defcleanerName(uint8_t *, zfree, TestBytes);
defcleaner(flex *, flexFree);

/* @model: src/aeadNonceLog.h */
/* Every call against a FIFO of (key id, generation, nonce) tuples
 * holding 7/8 of the capacity, the log rebuilt from its bytes now and
 * then and the run going on with the copy */
static int aeadNonceLogTestModel_(uint64_t rng) {
    int err = 0;
    aeadNonceLog *log = aeadNonceLogCreate(64u);
    const size_t window = aeadNonceLogCapacity(log) * 7u / 8u;
    uint32_t modelKey[56];
    uint64_t modelGen[56];
    uint64_t modelVal[56];
    size_t modelCount = 0;
    if (aeadNonceLogCapacity(log) != 64u || window != 56u) {
        ERR("capacity %zu", aeadNonceLogCapacity(log));
    }
    for (size_t step = 0; step < 20000u && !err; step++) {
        dkLcg64Next(&rng);
        const uint32_t key = 1u + (uint32_t)((rng >> 33) % 3u);
        const uint64_t gen = (rng >> 50) % 3u;
        const uint64_t val = (rng >> 40) % 120u;
        if ((rng >> 20) % 64u == 0) {
            aeadNonceLogClearKey(log, key); /* every generation of it */
            size_t w = 0;
            for (size_t m = 0; m < modelCount; m++) {
                if (modelKey[m] != key) {
                    modelKey[w] = modelKey[m];
                    modelGen[w] = modelGen[m];
                    modelVal[w++] = modelVal[m];
                }
            }
            modelCount = w;
        } else {
            bool present = false;
            for (size_t m = 0; m < modelCount && !present; m++) {
                present = modelKey[m] == key && modelGen[m] == gen &&
                          modelVal[m] == val;
            }
            uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
            nonceFromCounter(val, nonce);
            const bool inserted =
                gen == 0 && (rng >> 18) % 2
                    ? aeadNonceLogObserve(log, key, nonce)
                    : aeadNonceLogObserveAt(log, key, gen, nonce);
            if (inserted == present) {
                ERR("step %zu: observe(%u, %" PRIu64 ", %" PRIu64 ") disagrees",
                    step, key, gen, val);
            }
            if (!present) {
                if (modelCount == window) { /* the oldest goes */
                    memmove(modelKey, modelKey + 1, 55u * sizeof(*modelKey));
                    memmove(modelGen, modelGen + 1, 55u * sizeof(*modelGen));
                    memmove(modelVal, modelVal + 1, 55u * sizeof(*modelVal));
                    modelCount--;
                }
                modelKey[modelCount] = key;
                modelGen[modelCount] = gen;
                modelVal[modelCount++] = val;
            }
        }
        if (aeadNonceLogSize(log) != modelCount ||
            !persistOpsAeadNonceLog.validate(log)) {
            ERR("step %zu: size %zu (model %zu) or validate failed", step,
                aeadNonceLogSize(log), modelCount);
        }
        if (step % 500u == 499u) {
            /* Its bytes: sized, refused short, then a copy that carries
             * on (same tuples, same eviction order) */
            const size_t need = aeadNonceLogToBytes(log, nullptr, 0);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(need);
            if (!need || aeadNonceLogToBytes(log, buf, need - 1) != 0 ||
                aeadNonceLogToBytes(log, buf, need) != need) {
                ERR("step %zu: ToBytes sizes disagree", step);
                break;
            }
            aeadNonceLog *back = aeadNonceLogFromBytes(buf, need);
            if (!back || aeadNonceLogSize(back) != modelCount) {
                ERR("step %zu: FromBytes did not rebuild the log", step);
                aeadNonceLogFree(back);
                break;
            }
            aeadNonceLogFree(log);
            log = back;
        }
    }
    aeadNonceLogFree(log);
    return err;
}

int aeadNonceLogTest(int, char *[]) {
    int err = 0;

    TEST("FromBytes refuses a valid header over a corrupt last entry") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {};
        nonceFromCounter(5u, nonce);
        aeadNonceLogObserve(log, 7u, nonce);
        nonceFromCounter(6u, nonce);
        aeadNonceLogObserve(log, 9u, nonce);
        const size_t need = aeadNonceLogToBytes(log, nullptr, 0);
        cleanerName(zfree, TestBytes) uint8_t *snap = zcalloc(1, need);
        if (aeadNonceLogToBytes(log, snap, need) != need) {
            ERR("ToBytes failed");
        }

        {
            cleaner(aeadNonceLogFree) aeadNonceLog *back =
                aeadNonceLogFromBytes(snap, need);
            if (back == nullptr || aeadNonceLogSize(back) != 2u) {
                ERR("unmodified snapshot did not restore");
            }
        }

        /* Give the last entry (the second record's small generation) the
         * encoding byte of a full 64-bit integer: the total-bytes header
         * still matches the buffer, but the entry now claims payload bytes
         * past the end of it */
        cleaner(flexFree) flex *wide64 = flexNew();
        flexPushUnsigned(&wide64, UINT64_MAX, FLEX_ENDPOINT_TAIL);
        const uint8_t *wideEntry = (const uint8_t *)flexHead(wide64);
        const size_t wideLen =
            flexBytes(wide64) - (size_t)(wideEntry - (const uint8_t *)wide64);

        const flex *f = (const flex *)snap;
        uint8_t *last = (uint8_t *)flexIndex(f, (int32_t)(flexCount(f) - 1));
        const size_t lastLen = need - (size_t)(last - snap);
        if (lastLen >= wideLen) {
            ERR("last entry is %zu bytes, not shorter than %zu", lastLen,
                wideLen);
        }

        last[0] = wideEntry[0];
        if (!flexValidateBuffer(snap, need)) {
            ERR("the corrupt snapshot's header should still validate");
        }
        if (flexValidateBufferDeep(snap, need)) {
            ERR("the corrupt snapshot should fail deep validation");
        }

        cleaner(aeadNonceLogFree) aeadNonceLog *back =
            aeadNonceLogFromBytes(snap, need);
        if (back != nullptr) {
            ERR("FromBytes accepted a corrupt last entry");
        }
    }

    TEST("FromBytes refuses a 96/128-bit value in every field") {
        /* Two records, so the header and every per-record field are
         * covered. */
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {};
        nonceFromCounter(0x1122334455667788u, nonce);
        nonce[8] = 0x99u;
        aeadNonceLogObserve(log, 7u, nonce);
        nonceFromCounter(3u, nonce);
        aeadNonceLogObserve(log, 9u, nonce);
        const size_t need = aeadNonceLogToBytes(log, nullptr, 0);
        cleanerName(zfree, TestBytes) uint8_t *snap = zcalloc(1, need);
        if (aeadNonceLogToBytes(log, snap, need) != need) {
            ERR("ToBytes failed");
        }
        const flex *src = (const flex *)snap;
        const size_t entries = flexCount(src);
        if (entries != 1u + 2u * AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD) {
            ERR("snapshot has %zu entries", entries);
        }

        databoxBig wides[3];
        DATABOX_BIG_UNSIGNED_128(&wides[0], (__uint128_t)1 << 64);
        DATABOX_BIG_UNSIGNED_128(&wides[1], ((__uint128_t)1 << 127) | 1u);
        DATABOX_BIG_SIGNED_128(&wides[2], -((__int128_t)1 << 100));

        /* The rebuild itself must restore, so a refusal below is due to
         * the replaced field alone. */
        {
            cleaner(flexFree) flex *same =
                flexTestReplaceEntry(src, SIZE_MAX, nullptr);
            ctestPoisonStack();
            cleaner(aeadNonceLogFree) aeadNonceLog *back =
                aeadNonceLogFromBytes((const uint8_t *)same, flexBytes(same));
            if (back == nullptr || aeadNonceLogSize(back) != 2u) {
                ERR("unmodified rebuild did not restore");
            }
        }

        for (size_t w = 0; w < 3; w++) {
            for (size_t idx = 0; idx < entries; idx++) {
                cleaner(flexFree) flex *bad =
                    flexTestReplaceEntry(src, idx, &wides[w]);
                databoxBig check;
                flexGetByTypeBig(flexIndex(bad, (int32_t)idx), &check);
                if (!check.big) {
                    ERR("value %zu at entry %zu was not stored wide", w, idx);
                }
                ctestPoisonStack();
                cleaner(aeadNonceLogFree) aeadNonceLog *back =
                    aeadNonceLogFromBytes((const uint8_t *)bad, flexBytes(bad));
                if (back != nullptr) {
                    ERR("value %zu at entry %zu was accepted", w, idx);
                }
            }
        }
    }

    TEST("the restore validator refuses each broken invariant") {
        /* A log of 5 tuples validates; each corruption below (applied to
         * it, then undone) is refused: a capacity that is no power of two
         * or a mask that does not fit it, a count over the load limit or
         * off by one, a slot whose hash is not its tuple's, a FIFO head
         * missing or out of range, a head with a predecessor, a tail with
         * a successor, a list that loops, a link whose prev is wrong; and
         * an empty log whose head is set */
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        for (uint8_t i = 0; i < 5; i++) {
            uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {i, 0x5a};
            aeadNonceLogObserve(log, 7u, nonce);
        }
        const size_t cap = log->cap;
        const aeadNonceLog saved = *log;
        cleanerName(zfree, Slots) aeadNonceLogSlot *slotsCopy =
            zmalloc(cap * sizeof(*slotsCopy));
        cleanerName(zfree, Links) aeadNonceLogLink *linksCopy =
            zmalloc(cap * sizeof(*linksCopy));
        memcpy(slotsCopy, log->slots, cap * sizeof(*slotsCopy));
        memcpy(linksCopy, log->links, cap * sizeof(*linksCopy));
        if (!anlPersistValidate(log)) {
            ERR("a valid log did not validate");
        }
        const size_t head = log->fifo_head;
        const size_t second = log->links[head].next;
        for (int c = 0; c < 11; c++) {
            switch (c) {
            case 0:
                log->cap = cap + 1;
                break;
            case 1:
                log->mask = cap;
                break;
            case 2:
                log->count = aeadNonceLogLoadLimit(cap) + 1;
                break;
            case 3:
                log->count = saved.count - 1;
                break;
            case 4:
                log->slots[head].hash ^= 1u;
                break;
            case 5:
                log->fifo_head = AEAD_NONCE_LOG_NIL;
                break;
            case 6:
                log->fifo_head = cap;
                break;
            case 7:
                log->links[head].prev = second;
                break;
            case 8:
                log->links[log->fifo_tail].next = head;
                break;
            case 9:
                log->links[second].prev = second;
                break;
            case 10:
                log->fifo_tail = AEAD_NONCE_LOG_NIL;
                break;
            }
            if (anlPersistValidate(log)) {
                ERR("corruption %d was accepted", c);
            }
            /* undo: restore the fields and both arrays */
            aeadNonceLogSlot *slots = log->slots;
            aeadNonceLogLink *links = log->links;
            log->cap = saved.cap;
            log->mask = saved.mask;
            log->count = saved.count;
            log->fifo_head = saved.fifo_head;
            log->fifo_tail = saved.fifo_tail;
            memcpy(slots, slotsCopy, cap * sizeof(*slotsCopy));
            memcpy(links, linksCopy, cap * sizeof(*linksCopy));
        }
        if (!anlPersistValidate(log)) {
            ERR("the restored log did not validate");
        }
        cleaner(aeadNonceLogFree) aeadNonceLog *empty = aeadNonceLogCreate(0);
        empty->fifo_head = 0;
        if (anlPersistValidate(empty)) {
            ERR("an empty log with a FIFO head validated");
        }
        empty->fifo_head = AEAD_NONCE_LOG_NIL;
        if (!anlPersistValidate(empty) || anlPersistValidate(nullptr)) {
            ERR("an empty log, or NULL, validated wrongly");
        }
    }

    TEST("create/free round-trip") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        if (log == nullptr) {
            ERR("Create returned NULL");
        }
        if (aeadNonceLogCapacity(log) < 64u) {
            ERR("capacity below minimum: %zu", aeadNonceLogCapacity(log));
        }
    }

    TEST("first observation accepted; second rejected") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {};
        nonceFromCounter(42u, nonce);
        if (!aeadNonceLogObserve(log, 1u, nonce)) {
            ERR("first observe should succeed");
        }
        if (aeadNonceLogObserve(log, 1u, nonce)) {
            ERR("duplicate observe must be refused");
        }
        if (aeadNonceLogSize(log) != 1u) {
            ERR("size expected 1, got %zu", aeadNonceLogSize(log));
        }
    }

    TEST("different key_id is a different tuple") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {};
        nonceFromCounter(7u, nonce);
        if (!aeadNonceLogObserve(log, 1u, nonce)) {
            ERR("k=1");
        }
        if (!aeadNonceLogObserve(log, 2u, nonce)) {
            ERR("k=2");
        }
        if (aeadNonceLogObserve(log, 1u, nonce)) {
            ERR("dup k=1 refused");
        }
        if (aeadNonceLogObserve(log, 2u, nonce)) {
            ERR("dup k=2 refused");
        }
    }

    TEST("a rotated key under the same id is a different tuple, through a "
         "snapshot too") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {};
        nonceFromCounter(3u, nonce);
        const bool first = aeadNonceLogObserveAt(log, 4u, 10, nonce);
        const bool rotated = aeadNonceLogObserveAt(log, 4u, 11, nonce);
        const bool again = aeadNonceLogObserveAt(log, 4u, 10, nonce);
        const size_t need = aeadNonceLogToBytes(log, nullptr, 0);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(need ? need : 1);
        (void)aeadNonceLogToBytes(log, snap, need);
        cleaner(aeadNonceLogFree) aeadNonceLog *back =
            aeadNonceLogFromBytes(snap, need);
        const bool backOld = back && aeadNonceLogObserveAt(back, 4u, 11, nonce);
        const bool backNew = back && aeadNonceLogObserveAt(back, 4u, 12, nonce);
        aeadNonceLogClearKey(log, 4u);
        if (!first || !rotated || again || !back || backOld || !backNew ||
            aeadNonceLogSize(log) != 0) {
            ERR("first %d, rotated %d, repeat %d, restored repeat %d, "
                "restored new %d",
                first, rotated, again, backOld, backNew);
        }
    }

    TEST("clearKey forgets all entries for one key only") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {};
        for (uint64_t i = 0; i < 16; i++) {
            nonceFromCounter(i, nonce);
            aeadNonceLogObserve(log, 1u, nonce);
            aeadNonceLogObserve(log, 2u, nonce);
        }
        if (aeadNonceLogSize(log) != 32u) {
            ERR("expected 32 entries, got %zu", aeadNonceLogSize(log));
        }
        aeadNonceLogClearKey(log, 1u);
        if (aeadNonceLogSize(log) != 16u) {
            ERR("expected 16 after clear, got %zu", aeadNonceLogSize(log));
        }
        /* key=1 entries should now be insertable again. */
        nonceFromCounter(0u, nonce);
        if (!aeadNonceLogObserve(log, 1u, nonce)) {
            ERR("post-clear re-observe of k=1 should succeed");
        }
        /* key=2 entries should still be detected as duplicates. */
        if (aeadNonceLogObserve(log, 2u, nonce)) {
            ERR("k=2 entries must survive clearKey(1)");
        }
    }

    TEST("FIFO eviction kicks in past load threshold") {
        const size_t cap = 128u;
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(cap);
        uint8_t nonce[12] = {};
        /* Fill past capacity — every observation should succeed (eviction
         * makes room) and size never exceeds cap. */
        for (uint64_t i = 0; i < cap * 4u; i++) {
            nonceFromCounter(i, nonce);
            if (!aeadNonceLogObserve(log, 1u, nonce)) {
                ERR("observe iter %" PRIu64 " refused", i);
                break;
            }
            if (aeadNonceLogSize(log) > aeadNonceLogCapacity(log)) {
                ERR("size exceeded capacity");
            }
        }
    }

    /* Two tuples sharing a home slot sit in one probe run; removing the
     * first (by ClearKey or by eviction) must leave the second findable,
     * or its reuse would be accepted */
    TEST("removal keeps the rest of a probe run reachable") {
        uint8_t first[AEAD_NONCE_LOG_NONCE_LEN] = {};
        uint8_t second[AEAD_NONCE_LOG_NONCE_LEN] = {};
        nonceFromCounter(1u, first);
        const uint64_t home = aeadNonceLogHash(1u, 0, first) & 63u;
        uint64_t v = 2u;
        do {
            nonceFromCounter(v++, second);
        } while ((aeadNonceLogHash(2u, 0, second) & 63u) != home);

        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        aeadNonceLogObserve(log, 1u, first);
        aeadNonceLogObserve(log, 2u, second);
        aeadNonceLogClearKey(log, 1u);
        if (aeadNonceLogObserve(log, 2u, second)) {
            ERR("reuse accepted after ClearKey emptied its run's head");
        }

        /* Eviction: the oldest tuple heads the run; filling to the load
         * limit (56 of 64) evicts it and nothing else */
        cleaner(aeadNonceLogFree) aeadNonceLog *full = aeadNonceLogCreate(64u);
        aeadNonceLogObserve(full, 1u, first);
        aeadNonceLogObserve(full, 2u, second);
        uint8_t filler[AEAD_NONCE_LOG_NONCE_LEN] = {};
        for (uint64_t i = 0; i < 55u; i++) {
            nonceFromCounter(1'000'000u + i, filler);
            aeadNonceLogObserve(full, 3u, filler);
        }
        if (aeadNonceLogSize(full) != 56u) {
            ERR("expected 56 tuples, got %zu", aeadNonceLogSize(full));
        }
        if (aeadNonceLogObserve(full, 2u, second)) {
            ERR("reuse accepted after eviction emptied its run's head");
        }
        if (!persistOpsAeadNonceLog.validate(full)) {
            ERR("validate refused the log after an eviction");
        }
    }

    /* Differential: Observe and ClearKey against a FIFO list model with
     * the same 7/8 load limit, over a small tuple space so reuses and
     * evictions are frequent and probe runs collide; validate (every
     * tuple reachable, FIFO intact) holds after every step */
    TEST("model: every public call against a FIFO of tuples") {
        err += aeadNonceLogTestModel_(0xA5A5A5A5u);
        err += aeadNonceLogTestModel_(7u);
    }

    TEST("capacity limit and snapshot order") {
        {
            cleaner(aeadNonceLogFree) aeadNonceLog *huge =
                aeadNonceLogCreate(AEAD_NONCE_LOG_MAX_CAP + 1u);
            if (huge != nullptr) {
                ERR("a capacity past the limit was accepted");
            }
        }

        /* The snapshot lists tuples oldest first, so a restored log
         * evicts in the same order and snapshots to the same bytes */
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
        for (uint64_t i = 0; i < 300u; i++) {
            nonceFromCounter(i * 7919u, nonce);
            aeadNonceLogObserve(log, 1u + (uint32_t)(i % 3u), nonce);
            if (i % 50u == 49u) {
                aeadNonceLogClearKey(log, 2u);
            }
        }

        size_t len = 0;
        cleaner(flexFree) flex *snap =
            (flex *)persistOpsAeadNonceLog.snapshot(log, &len);
        cleaner(aeadNonceLogFree) aeadNonceLog *back =
            aeadNonceLogFromBytes((const uint8_t *)snap, len);
        if (!back) {
            ERR("restore refused a live snapshot");
        } else {
            size_t len2 = 0;
            cleaner(flexFree) flex *snap2 =
                (flex *)persistOpsAeadNonceLog.snapshot(back, &len2);
            if (len2 != len || memcmp(snap, snap2, len) != 0) {
                ERR("restored log does not snapshot to the same bytes");
            }

            /* Further evictions remove the same (oldest) tuples */
            for (uint64_t i = 0; i < 10u; i++) {
                nonceFromCounter(5000000u + i, nonce);
                aeadNonceLogObserve(log, 9u, nonce);
                aeadNonceLogObserve(back, 9u, nonce);
            }
            size_t lenA = 0;
            size_t lenB = 0;
            cleaner(flexFree) flex *a =
                (flex *)persistOpsAeadNonceLog.snapshot(log, &lenA);
            cleaner(flexFree) flex *b =
                (flex *)persistOpsAeadNonceLog.snapshot(back, &lenB);
            if (lenA != lenB || memcmp(a, b, lenA) != 0) {
                ERR("live and restored logs diverged after evictions");
            }
        }
    }

    TEST("invalid inputs rejected") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {};
        if (aeadNonceLogObserve(nullptr, 1u, nonce)) {
            ERR("NULL log accepted");
        }
        if (aeadNonceLogObserve(log, 0u, nonce)) {
            ERR("key_id=0 accepted");
        }
        if (aeadNonceLogObserve(log, 1u, nullptr)) {
            ERR("NULL nonce accepted");
        }
    }

    TEST("concurrent observers: every unique tuple observed exactly once") {
        /* Threads write disjoint nonce ranges under the same key. The
         * union should fit under capacity so no eviction kicks in;
         * post-join we verify every tuple is rejected as a duplicate. */
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(8192u);

        const uint32_t kThreads = 8u;
        const uint32_t kIters = 512u;
        aeadNonceLogConcCtx ctxs[8];
        pthread_t tids[8];
        for (uint32_t t = 0; t < kThreads; t++) {
            ctxs[t].log = log;
            ctxs[t].key_id = 1u;
            ctxs[t].base = (uint64_t)t * (uint64_t)kIters;
            ctxs[t].iters = kIters;
            ctxs[t].inserted = 0u;
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_create(&tids[t], nullptr, aeadNonceLogConcWorker, &ctxs[t]);
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_join(tids[t], nullptr);
        }
        uint32_t total_inserted = 0u;
        for (uint32_t t = 0; t < kThreads; t++) {
            total_inserted += ctxs[t].inserted;
        }
        if (total_inserted != kThreads * kIters) {
            ERR("expected %u inserts, got %u", kThreads * kIters,
                total_inserted);
        }
        /* Now every tuple must be detected as a duplicate. */
        uint8_t nonce[12] = {};
        for (uint64_t i = 0; i < (uint64_t)kThreads * (uint64_t)kIters; i++) {
            nonceFromCounter(i, nonce);
            if (aeadNonceLogObserve(log, 1u, nonce)) {
                ERR("nonce %" PRIu64 " unexpectedly insertable post-fill", i);
                break;
            }
        }
    }

    TEST("aeadNonceLogCreate fails closed under OOM (returns NULL, no crash)") {
        /* Swap the global allocator's malloc for a failing one across exactly
         * one Create call: it must return NULL (so a caller like
         * clusterMailboxCreate refuses to construct without replay protection)
         * rather than dereference NULL. Restore the allocator BEFORE asserting
         * (ERR/printf themselves allocate). */
        datakitConfig saved = datakitConfigMemory__;
        datakitConfig failing = saved;
        failing.localMalloc = aeadOomFailMalloc;
        failing.localCalloc = aeadOomFailCalloc;
        datakitConfigSet(&failing);
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(1024);
        datakitConfigSet(&saved); /* restore before any allocation */
        if (log != nullptr) {
            ERR("aeadNonceLogCreate must return NULL under OOM (fail closed)");
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
