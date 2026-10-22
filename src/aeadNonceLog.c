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
#include "flex.h"
#include "persist.h"

#include <pthread.h>
#include <string.h>

#define AEAD_NONCE_LOG_MIN_CAP ((size_t)64)

/* SIZE_MAX is the sentinel for "no link" in the FIFO list. */
#define AEAD_NONCE_LOG_NIL ((size_t)SIZE_MAX)

typedef struct aeadNonceLogSlot {
    uint64_t hash;   /* aeadNonceLogHash(key_id, nonce) */
    uint32_t key_id; /* HMAC_KEY_ID_INVALID (0) marks an empty slot */
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
    size_t c = AEAD_NONCE_LOG_MIN_CAP;
    while (c < req) {
        c <<= 1u;
    }
    return c;
}

/* Most entries the table holds: at this load the oldest is evicted
 * before an insert, so a probe always meets an empty slot */
static size_t aeadNonceLogLoadLimit(size_t cap) {
    return cap - (cap >> 3u);
}

/* splitmix64 mix. Non-cryptographic but fine for hash-bucket
 * distribution. */
static uint64_t aeadNonceLogMix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

/* Hash the (key_id || nonce) tuple. */
static uint64_t aeadNonceLogHash(uint32_t key_id, const uint8_t *nonce) {
    uint64_t lo, hi;
    memcpy(&lo, nonce, 8);
    uint32_t hi32 = 0;
    memcpy(&hi32, nonce + 8, 4);
    hi = ((uint64_t)hi32 << 32) | (uint64_t)key_id;
    return aeadNonceLogMix64(lo ^ aeadNonceLogMix64(hi));
}

/* The occupied slot `s` holds (key_id, nonce), whose hash is `h` */
static bool aeadNonceLogSlotMatches(const aeadNonceLogSlot *s, uint64_t h,
                                    uint32_t key_id, const uint8_t *nonce) {
    return s->hash == h && s->key_id == key_id &&
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
        return NULL;
    }
    const size_t cap = aeadNonceLogRoundCap(min_capacity);
    cleanerName(zfree, Log) aeadNonceLog *log = zmalloc(sizeof(*log));
    cleanerName(zfree, Slots) aeadNonceLogSlot *slots =
        log ? zmalloc(cap * sizeof(*slots)) : NULL;
    cleanerName(zfree, Links) aeadNonceLogLink *links =
        log ? zmalloc(cap * sizeof(*links)) : NULL;
    if (!log || !slots || !links) {
        /* OOM: fail closed (return NULL) instead of dereferencing NULL below,
         * so the caller's fail-closed path runs — e.g. clusterMailboxCreate
         * refuses to construct a mailbox without replay protection rather than
         * crashing under memory pressure. */
        return NULL;
    }
    memset(slots, 0, cap * sizeof(*slots));
    for (size_t i = 0; i < cap; i++) {
        links[i].prev = AEAD_NONCE_LOG_NIL;
        links[i].next = AEAD_NONCE_LOG_NIL;
    }
    if (pthread_mutex_init(&log->lock, NULL) != 0) {
        return NULL;
    }
    log->slots = slots;
    slots = NULL;
    log->links = links;
    links = NULL;
    log->cap = cap;
    log->mask = cap - 1u;
    log->count = 0;
    log->fifo_head = AEAD_NONCE_LOG_NIL;
    log->fifo_tail = AEAD_NONCE_LOG_NIL;
    aeadNonceLog *out = log;
    log = NULL;
    return out;
}

void aeadNonceLogFree(aeadNonceLog *log) {
    if (log == NULL) {
        return;
    }
    /* Wipe slot contents before free — leaked log should not leak
     * observed nonces. */
    volatile uint8_t *p = (volatile uint8_t *)log->slots;
    const size_t bytes = log->cap * sizeof(aeadNonceLogSlot);
    for (size_t i = 0; i < bytes; i++) {
        p[i] = 0;
    }
    pthread_mutex_destroy(&log->lock);
    zfree(log->links);
    zfree(log->slots);
    zfree(log);
}

/* Zero a slot's bytes (observed nonces never linger in freed slots) */
static void aeadNonceLogWipeSlot(aeadNonceLog *log, size_t idx) {
    volatile uint8_t *p = (volatile uint8_t *)&log->slots[idx];
    for (size_t i = 0; i < sizeof(aeadNonceLogSlot); i++) {
        p[i] = 0;
    }
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
                             uint32_t key_id, const uint8_t *nonce,
                             size_t *empty) {
    size_t idx = (size_t)(h & log->mask);
    for (size_t step = 0; step < log->cap; step++) {
        const aeadNonceLogSlot *s = &log->slots[idx];
        if (s->key_id == 0u) {
            *empty = idx;
            return false;
        }
        if (aeadNonceLogSlotMatches(s, h, key_id, nonce)) {
            return true;
        }
        idx = (idx + 1u) & log->mask;
    }
    *empty = SIZE_MAX;
    return false;
}

/* Fill the empty slot `idx` and append it to the FIFO tail */
static void aeadNonceLogPlace(aeadNonceLog *log, size_t idx, uint64_t h,
                              uint32_t key_id, const uint8_t *nonce) {
    aeadNonceLogSlot *s = &log->slots[idx];
    s->hash = h;
    s->key_id = key_id;
    memcpy(s->nonce, nonce, AEAD_NONCE_LOG_NONCE_LEN);
    aeadNonceLogFifoPushTail(log, idx);
    log->count++;
}

bool aeadNonceLogObserve(aeadNonceLog *log, uint32_t key_id,
                         const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]) {
    if (log == NULL || nonce == NULL || key_id == 0u) {
        return false;
    }
    const uint64_t h = aeadNonceLogHash(key_id, nonce);
    pthread_mutex_lock(&log->lock);

    /* Probe first: a duplicate leaves the table untouched */
    size_t empty = SIZE_MAX;
    if (aeadNonceLogFind(log, h, key_id, nonce, &empty)) {
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
        aeadNonceLogPlace(log, empty, h, key_id, nonce);
    }
    pthread_mutex_unlock(&log->lock);
    return placed;
}

void aeadNonceLogClearKey(aeadNonceLog *log, uint32_t key_id) {
    if (log == NULL || key_id == 0u) {
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
    if (log == NULL) {
        return 0;
    }
    pthread_mutex_lock((pthread_mutex_t *)&log->lock);
    const size_t n = log->count;
    pthread_mutex_unlock((pthread_mutex_t *)&log->lock);
    return n;
}

size_t aeadNonceLogCapacity(const aeadNonceLog *log) {
    if (log == NULL) {
        return 0;
    }
    return log->cap;
}

/* ========================================================================
 * Persistence (flex serialization + persistOps descriptor)
 * ======================================================================== */

/* Entries per record in the serialized flex: nonce, key_id */
#define AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD ((size_t)2)

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
        flexRowAppendExact(&f, &row);
    }

    pthread_mutex_unlock((pthread_mutex_t *)&log->lock);
    return f;
}

size_t aeadNonceLogToBytes(const aeadNonceLog *log, uint8_t *out,
                           size_t out_cap) {
    if (log == NULL) {
        return 0;
    }
    flex *f = anlBuildSnapshotFlex(log);
    const size_t need = flexBytes(f);
    if (out == NULL) {
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

/* Decode one snapshot field, which must be a 64-bit unsigned integer.
 * flexGetByTypeBig never reads the box's prior state and gives it its
 * own storage for a wide value, and it borrows instead of allocating.
 * A 96/128-bit entry (no field can hold one), a signed value, bytes, or
 * any other type is refused. */
static bool anlReadU64(const flexEntry *fe, uint64_t *out) {
    databoxBig box;
    flexGetByTypeBig(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return false;
    }
    *out = box.data.u64;
    return true;
}

aeadNonceLog *aeadNonceLogFromBytes(const uint8_t *buf, size_t len) {
    if (buf == NULL) {
        return NULL;
    }
    /* The bytes may come from disk or a peer: bound every entry, not
     * just the header, before walking them below */
    if (!flexValidateBufferDeep(buf, len)) {
        return NULL;
    }
    const flex *f = (const flex *)buf;
    const size_t total = flexCount(f);
    if (total == 0 ||
        (total - 1) % AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD != 0u) {
        return NULL;
    }
    const size_t records =
        (total - 1) / AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD;

    /* [0] capacity: a power of two in [MIN, MAX] (so a damaged field
     * cannot size an allocation), holding the records within the load
     * limit, as a live log does */
    flexEntry *fe = flexHead(f);
    uint64_t cap = 0;
    if (!anlReadU64(fe, &cap) || cap < AEAD_NONCE_LOG_MIN_CAP ||
        cap > AEAD_NONCE_LOG_MAX_CAP || (cap & (cap - 1u)) != 0u ||
        records > aeadNonceLogLoadLimit((size_t)cap)) {
        return NULL;
    }
    fe = flexNext(f, fe);

    cleaner(aeadNonceLogFree) aeadNonceLog *log =
        aeadNonceLogCreate((size_t)cap);
    if (log == NULL) {
        return NULL;
    }

    /* Records oldest first: [nonce (12 bytes), key_id (1..UINT32_MAX)],
     * each inserted at the FIFO tail; a repeated tuple is refused */
    for (size_t i = 0; i < records; i++) {
        databoxBig nonceBox;
        flexGetByTypeBig(fe, &nonceBox);
        if (!DATABOX_IS_BYTES(&nonceBox) ||
            nonceBox.len != AEAD_NONCE_LOG_NONCE_LEN) {
            return NULL;
        }
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
        memcpy(nonce, databoxBytes(&nonceBox), AEAD_NONCE_LOG_NONCE_LEN);
        fe = flexNext(f, fe);

        uint64_t key_id = 0;
        if (!anlReadU64(fe, &key_id) || key_id == 0u || key_id > UINT32_MAX) {
            return NULL;
        }
        fe = flexNext(f, fe);

        const uint64_t h = aeadNonceLogHash((uint32_t)key_id, nonce);
        size_t empty = SIZE_MAX;
        if (aeadNonceLogFind(log, h, (uint32_t)key_id, nonce, &empty) ||
            empty == SIZE_MAX) {
            return NULL;
        }
        aeadNonceLogPlace(log, empty, h, (uint32_t)key_id, nonce);
    }

    aeadNonceLog *out = log;
    log = NULL;
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
    return aeadNonceLogToBytes((const aeadNonceLog *)structure, NULL, 0);
}

/* validate hook: the table's shape and load, every entry reachable from
 * its home slot as the first match (so no tuple repeats and no lookup
 * misses one), and a FIFO list through exactly the occupied slots. */
static bool anlPersistValidate(const void *structure) {
    const aeadNonceLog *log = (const aeadNonceLog *)structure;
    if (log == NULL) {
        return false;
    }
    /* Capacity: a power of two in [MIN, MAX], within the load limit */
    if (log->cap < AEAD_NONCE_LOG_MIN_CAP ||
        log->cap > AEAD_NONCE_LOG_MAX_CAP ||
        (log->cap & (log->cap - 1u)) != 0u || log->mask != log->cap - 1u ||
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
        if (s->hash != aeadNonceLogHash(s->key_id, s->nonce)) {
            return false;
        }
        for (size_t idx = (size_t)(s->hash & log->mask); idx != i;
             idx = (idx + 1u) & log->mask) {
            const aeadNonceLogSlot *o = &log->slots[idx];
            if (o->key_id == 0u ||
                aeadNonceLogSlotMatches(o, s->hash, s->key_id, s->nonce)) {
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
    .snapshot = anlPersistSnapshot,
    .restore = anlPersistRestore,
    .newEmpty = anlPersistNewEmpty,
    .count = anlPersistCount,
    .estimateSize = anlPersistEstimateSize,
    .encodeOp = NULL, /* snapshot-only: each durable point is a snapshot */
    .applyOp = NULL,
    .streamSnapshot = NULL,
    .streamRestore = NULL,
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
    for (size_t i = 0; i < 8; i++) {
        out[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
    }
}

typedef struct aeadNonceLogConcCtx {
    aeadNonceLog *log;
    uint64_t base;
    uint32_t key_id;
    uint32_t iters;
    uint32_t inserted;
} aeadNonceLogConcCtx;

static void *aeadNonceLogConcWorker(void *arg) {
    aeadNonceLogConcCtx *c = (aeadNonceLogConcCtx *)arg;
    uint8_t nonce[12] = {0};
    for (uint32_t i = 0; i < c->iters; i++) {
        nonceFromCounter(c->base + (uint64_t)i, nonce);
        if (aeadNonceLogObserve(c->log, c->key_id, nonce)) {
            c->inserted++;
        }
    }
    return NULL;
}

/* Always-failing allocators for the OOM fault-injection test below. zmalloc
 * routes through localCalloc unless DK_MALLOC_REAL is set, so fail both. */
static void *aeadOomFailMalloc(size_t n) {
    (void)n;
    return NULL;
}
static void *aeadOomFailCalloc(size_t count, size_t n) {
    (void)count;
    (void)n;
    return NULL;
}

defcleanerName(uint8_t *, zfree, TestBytes);
defcleaner(flex *, flexFree);

/* Fill the stack below the caller with 0xFF so a decoder that reads an
 * uninitialized box sees big = 1 and an all-ones pointer.  ASan is off
 * here so the array is not framed by redzones, which would leave the
 * top of the next frame unpoisoned. */
static __attribute__((noinline, no_sanitize("address"))) void
anlTestPoisonStack(void) {
    volatile uint8_t junk[8192];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xFF;
    }
}

/* Copy of 'src' with entry 'idx' replaced by 'wide' (a SIZE_MAX index
 * copies every entry unchanged). */
static flex *anlTestReplaceEntry(const flex *src, size_t idx,
                                 const databoxBig *wide) {
    /* @own: caller; free=flexFree */
    flex *out = flexNew();
    size_t i = 0;
    for (flexEntry *fe = flexHead(src); fe != NULL; fe = flexNext(src, fe)) {
        databoxBig box;
        flexGetByTypeBig(fe, &box);
        const databox *push =
            (i == idx) ? (const databox *)wide : (const databox *)&box;
        flexPushByType(&out, push, FLEX_ENDPOINT_TAIL);
        i++;
    }
    return out;
}

int aeadNonceLogTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("FromBytes refuses a valid header over a corrupt last entry") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {0};
        nonceFromCounter(5u, nonce);
        aeadNonceLogObserve(log, 7u, nonce);
        nonceFromCounter(6u, nonce);
        aeadNonceLogObserve(log, 9u, nonce);
        const size_t need = aeadNonceLogToBytes(log, NULL, 0);
        cleanerName(zfree, TestBytes) uint8_t *snap = zcalloc(1, need);
        if (aeadNonceLogToBytes(log, snap, need) != need) {
            ERRR("ToBytes failed");
        }

        {
            cleaner(aeadNonceLogFree) aeadNonceLog *back =
                aeadNonceLogFromBytes(snap, need);
            if (back == NULL || aeadNonceLogSize(back) != 2u) {
                ERRR("unmodified snapshot did not restore");
            }
        }

        /* Give the last entry (the second record's small key_id) the
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
            ERRR("the corrupt snapshot's header should still validate");
        }
        if (flexValidateBufferDeep(snap, need)) {
            ERRR("the corrupt snapshot should fail deep validation");
        }

        cleaner(aeadNonceLogFree) aeadNonceLog *back =
            aeadNonceLogFromBytes(snap, need);
        if (back != NULL) {
            ERRR("FromBytes accepted a corrupt last entry");
        }
    }

    TEST("FromBytes refuses a 96/128-bit value in every field") {
        /* Two records, so the header and every per-record field are
         * covered. */
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {0};
        nonceFromCounter(0x1122334455667788u, nonce);
        nonce[8] = 0x99u;
        aeadNonceLogObserve(log, 7u, nonce);
        nonceFromCounter(3u, nonce);
        aeadNonceLogObserve(log, 9u, nonce);
        const size_t need = aeadNonceLogToBytes(log, NULL, 0);
        cleanerName(zfree, TestBytes) uint8_t *snap = zcalloc(1, need);
        if (aeadNonceLogToBytes(log, snap, need) != need) {
            ERRR("ToBytes failed");
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
                anlTestReplaceEntry(src, SIZE_MAX, NULL);
            anlTestPoisonStack();
            cleaner(aeadNonceLogFree) aeadNonceLog *back =
                aeadNonceLogFromBytes((const uint8_t *)same, flexBytes(same));
            if (back == NULL || aeadNonceLogSize(back) != 2u) {
                ERRR("unmodified rebuild did not restore");
            }
        }

        for (size_t w = 0; w < 3; w++) {
            for (size_t idx = 0; idx < entries; idx++) {
                cleaner(flexFree) flex *bad =
                    anlTestReplaceEntry(src, idx, &wides[w]);
                databoxBig check;
                flexGetByTypeBig(flexIndex(bad, (int32_t)idx), &check);
                if (!check.big) {
                    ERR("value %zu at entry %zu was not stored wide", w, idx);
                }
                anlTestPoisonStack();
                cleaner(aeadNonceLogFree) aeadNonceLog *back =
                    aeadNonceLogFromBytes((const uint8_t *)bad, flexBytes(bad));
                if (back != NULL) {
                    ERR("value %zu at entry %zu was accepted", w, idx);
                }
            }
        }
    }

    TEST("create/free round-trip") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        if (log == NULL) {
            ERRR("Create returned NULL");
        }
        if (aeadNonceLogCapacity(log) < 64u) {
            ERR("capacity below minimum: %zu", aeadNonceLogCapacity(log));
        }
    }

    TEST("first observation accepted; second rejected") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {0};
        nonceFromCounter(42u, nonce);
        if (!aeadNonceLogObserve(log, 1u, nonce)) {
            ERRR("first observe should succeed");
        }
        if (aeadNonceLogObserve(log, 1u, nonce)) {
            ERRR("duplicate observe must be refused");
        }
        if (aeadNonceLogSize(log) != 1u) {
            ERR("size expected 1, got %zu", aeadNonceLogSize(log));
        }
    }

    TEST("different key_id is a different tuple") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {0};
        nonceFromCounter(7u, nonce);
        if (!aeadNonceLogObserve(log, 1u, nonce)) {
            ERRR("k=1");
        }
        if (!aeadNonceLogObserve(log, 2u, nonce)) {
            ERRR("k=2");
        }
        if (aeadNonceLogObserve(log, 1u, nonce)) {
            ERRR("dup k=1 refused");
        }
        if (aeadNonceLogObserve(log, 2u, nonce)) {
            ERRR("dup k=2 refused");
        }
    }

    TEST("clearKey forgets all entries for one key only") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {0};
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
            ERRR("post-clear re-observe of k=1 should succeed");
        }
        /* key=2 entries should still be detected as duplicates. */
        if (aeadNonceLogObserve(log, 2u, nonce)) {
            ERRR("k=2 entries must survive clearKey(1)");
        }
    }

    TEST("FIFO eviction kicks in past load threshold") {
        const size_t cap = 128u;
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(cap);
        uint8_t nonce[12] = {0};
        /* Fill past capacity — every observation should succeed (eviction
         * makes room) and size never exceeds cap. */
        for (uint64_t i = 0; i < cap * 4u; i++) {
            nonceFromCounter(i, nonce);
            if (!aeadNonceLogObserve(log, 1u, nonce)) {
                ERR("observe iter %" PRIu64 " refused", i);
                break;
            }
            if (aeadNonceLogSize(log) > aeadNonceLogCapacity(log)) {
                ERRR("size exceeded capacity");
            }
        }
    }

    /* Two tuples sharing a home slot sit in one probe run; removing the
     * first (by ClearKey or by eviction) must leave the second findable,
     * or its reuse would be accepted */
    TEST("removal keeps the rest of a probe run reachable") {
        uint8_t first[AEAD_NONCE_LOG_NONCE_LEN] = {0};
        uint8_t second[AEAD_NONCE_LOG_NONCE_LEN] = {0};
        nonceFromCounter(1u, first);
        const uint64_t home = aeadNonceLogHash(1u, first) & 63u;
        uint64_t v = 2u;
        do {
            nonceFromCounter(v++, second);
        } while ((aeadNonceLogHash(2u, second) & 63u) != home);

        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        aeadNonceLogObserve(log, 1u, first);
        aeadNonceLogObserve(log, 2u, second);
        aeadNonceLogClearKey(log, 1u);
        if (aeadNonceLogObserve(log, 2u, second)) {
            ERRR("reuse accepted after ClearKey emptied its run's head");
        }

        /* Eviction: the oldest tuple heads the run; filling to the load
         * limit (56 of 64) evicts it and nothing else */
        cleaner(aeadNonceLogFree) aeadNonceLog *full = aeadNonceLogCreate(64u);
        aeadNonceLogObserve(full, 1u, first);
        aeadNonceLogObserve(full, 2u, second);
        uint8_t filler[AEAD_NONCE_LOG_NONCE_LEN] = {0};
        for (uint64_t i = 0; i < 55u; i++) {
            nonceFromCounter(1000000u + i, filler);
            aeadNonceLogObserve(full, 3u, filler);
        }
        if (aeadNonceLogSize(full) != 56u) {
            ERR("expected 56 tuples, got %zu", aeadNonceLogSize(full));
        }
        if (aeadNonceLogObserve(full, 2u, second)) {
            ERRR("reuse accepted after eviction emptied its run's head");
        }
        if (!persistOpsAeadNonceLog.validate(full)) {
            ERRR("validate refused the log after an eviction");
        }
    }

    /* Differential: Observe and ClearKey against a FIFO list model with
     * the same 7/8 load limit, over a small tuple space so reuses and
     * evictions are frequent and probe runs collide; validate (every
     * tuple reachable, FIFO intact) holds after every step */
    TEST("observe and clear match a FIFO model") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(64u);
        uint32_t modelKey[56];
        uint64_t modelVal[56];
        size_t modelCount = 0;
        uint64_t rng = 0xA5A5A5A5u;
        for (size_t step = 0; step < 20000u && !err; step++) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            const uint32_t key = 1u + (uint32_t)((rng >> 33) % 3u);
            const uint64_t val = (rng >> 40) % 120u;
            if ((rng >> 20) % 64u == 0) {
                aeadNonceLogClearKey(log, key);
                size_t w = 0;
                for (size_t m = 0; m < modelCount; m++) {
                    if (modelKey[m] != key) {
                        modelKey[w] = modelKey[m];
                        modelVal[w++] = modelVal[m];
                    }
                }
                modelCount = w;
            } else {
                bool present = false;
                for (size_t m = 0; m < modelCount && !present; m++) {
                    present = modelKey[m] == key && modelVal[m] == val;
                }
                uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
                nonceFromCounter(val, nonce);
                if (aeadNonceLogObserve(log, key, nonce) == present) {
                    ERR("step %zu: observe(%u, %" PRIu64 ") disagrees", step,
                        key, val);
                }
                if (!present) {
                    if (modelCount == 56u) {
                        memmove(modelKey, modelKey + 1,
                                55u * sizeof(*modelKey));
                        memmove(modelVal, modelVal + 1,
                                55u * sizeof(*modelVal));
                        modelCount--;
                    }
                    modelKey[modelCount] = key;
                    modelVal[modelCount++] = val;
                }
            }
            if (aeadNonceLogSize(log) != modelCount ||
                !persistOpsAeadNonceLog.validate(log)) {
                ERR("step %zu: size %zu (model %zu) or validate failed", step,
                    aeadNonceLogSize(log), modelCount);
            }
        }
    }

    TEST("capacity limit and snapshot order") {
        {
            cleaner(aeadNonceLogFree) aeadNonceLog *huge =
                aeadNonceLogCreate(AEAD_NONCE_LOG_MAX_CAP + 1u);
            if (huge != NULL) {
                ERRR("a capacity past the limit was accepted");
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
            ERRR("restore refused a live snapshot");
        } else {
            size_t len2 = 0;
            cleaner(flexFree) flex *snap2 =
                (flex *)persistOpsAeadNonceLog.snapshot(back, &len2);
            if (len2 != len || memcmp(snap, snap2, len) != 0) {
                ERRR("restored log does not snapshot to the same bytes");
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
                ERRR("live and restored logs diverged after evictions");
            }
        }
    }

    TEST("invalid inputs rejected") {
        cleaner(aeadNonceLogFree) aeadNonceLog *log = aeadNonceLogCreate(0);
        uint8_t nonce[12] = {0};
        if (aeadNonceLogObserve(NULL, 1u, nonce)) {
            ERRR("NULL log accepted");
        }
        if (aeadNonceLogObserve(log, 0u, nonce)) {
            ERRR("key_id=0 accepted");
        }
        if (aeadNonceLogObserve(log, 1u, NULL)) {
            ERRR("NULL nonce accepted");
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
            pthread_create(&tids[t], NULL, aeadNonceLogConcWorker, &ctxs[t]);
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_join(tids[t], NULL);
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
        uint8_t nonce[12] = {0};
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
        if (log != NULL) {
            ERRR("aeadNonceLogCreate must return NULL under OOM (fail closed)");
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
