/* aeadNonceLog — see aeadNonceLog.h.
 *
 * Internal layout: open-addressing hash set with linear probing for
 * (key_id, nonce) lookup, layered with a FIFO doubly-linked list
 * (indexed by slot) used purely to make eviction O(1).
 *
 * Insertion ordering:
 * - Probe FIRST. If the tuple is already present we return duplicate
 * and do NOT touch the table. This avoids the pre-fix bug where an
 * eviction unconditionally fired before the probe and could evict
 * the candidate's own pre-existing slot, then "accept" it as a
 * fresh insert.
 * - If not found and load >= 7/8 capacity, evict the FIFO head
 * (oldest gen) in O(1).
 * - Insert at the FIFO tail.
 *
 * gen is kept per-slot purely for persistence — the wire format
 * exposes it so a snapshot/restore preserves insertion order. The
 * live data structure no longer scans for the oldest gen on every
 * eviction; that field is now read-only at runtime.
 *
 * Thread-safety: single mutex covers the table, the FIFO list, and
 * next_gen. Snapshot/restore takes the same lock during serialization. */

#include "aeadNonceLog.h"

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
    uint64_t gen;    /* 0 = empty; else monotonic insertion seq */
    uint32_t key_id; /* HMAC_KEY_ID_INVALID (==0) marks empty alongside gen=0 */
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
    uint64_t next_gen;
    size_t cap;
    size_t mask;
    size_t count;
    size_t fifo_head; /* oldest occupied slot; AEAD_NONCE_LOG_NIL if empty */
    size_t fifo_tail; /* newest occupied slot; AEAD_NONCE_LOG_NIL if empty */
};

/* Round up to next power of two, with a floor at AEAD_NONCE_LOG_MIN_CAP. */
static size_t aeadNonceLogRoundCap(size_t req) {
    size_t c = AEAD_NONCE_LOG_MIN_CAP;
    while (c < req) {
        c <<= 1u;
        if (c == 0) {
            /* Overflow — clamp to a very large sane value. */
            return ((size_t)1) << (sizeof(size_t) * 8u - 2u);
        }
    }
    return c;
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

static bool aeadNonceLogSlotMatches(const aeadNonceLogSlot *s, uint32_t key_id,
                                    const uint8_t *nonce) {
    if (s->gen == 0) {
        return false;
    }
    if (s->key_id != key_id) {
        return false;
    }
    return dkCtMemEq(s->nonce, nonce, AEAD_NONCE_LOG_NONCE_LEN);
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
    const size_t cap = aeadNonceLogRoundCap(min_capacity);
    aeadNonceLog *log = zmalloc(sizeof(*log));
    log->slots = zmalloc(cap * sizeof(aeadNonceLogSlot));
    log->links = zmalloc(cap * sizeof(aeadNonceLogLink));
    memset(log->slots, 0, cap * sizeof(aeadNonceLogSlot));
    for (size_t i = 0; i < cap; i++) {
        log->links[i].prev = AEAD_NONCE_LOG_NIL;
        log->links[i].next = AEAD_NONCE_LOG_NIL;
    }
    if (pthread_mutex_init(&log->lock, NULL) != 0) {
        zfree(log->links);
        zfree(log->slots);
        zfree(log);
        return NULL;
    }
    log->next_gen = 1u; /* gen 0 reserved for empty */
    log->cap = cap;
    log->mask = cap - 1u;
    log->count = 0;
    log->fifo_head = AEAD_NONCE_LOG_NIL;
    log->fifo_tail = AEAD_NONCE_LOG_NIL;
    return log;
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

/* Evict the FIFO head (oldest gen, by insertion order) in O(1).
 * Caller MUST hold the lock and MUST have verified the list is
 * non-empty. Returns the index that became free. */
static size_t aeadNonceLogEvictHead(aeadNonceLog *log) {
    const size_t victim = log->fifo_head;
    /* victim == AEAD_NONCE_LOG_NIL would imply count > 0 with empty list;
     * that's a structural invariant violation — clamp defensively. */
    if (victim == AEAD_NONCE_LOG_NIL) {
        return 0; /* unreachable when invariants hold */
    }
    aeadNonceLogFifoUnlink(log, victim);
    /* Wipe contents of evicted slot. */
    volatile uint8_t *p = (volatile uint8_t *)&log->slots[victim];
    for (size_t i = 0; i < sizeof(aeadNonceLogSlot); i++) {
        p[i] = 0;
    }
    log->count--;
    return victim;
}

bool aeadNonceLogObserve(aeadNonceLog *log, uint32_t key_id,
                         const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]) {
    if (log == NULL || nonce == NULL || key_id == 0u) {
        return false;
    }
    const uint64_t h = aeadNonceLogHash(key_id, nonce);
    pthread_mutex_lock(&log->lock);

    /* PROBE FIRST. Walk the chain until we find either:
     * - a matching (key_id, nonce) → duplicate, return false without
     * touching the table. This is the correctness fix for the
     * pre-existing "evict-then-probe" bug where the candidate's
     * own slot could be evicted before the probe noticed it.
     * - an empty slot → record its index for the eventual insert and
     * stop walking.
     * - the entire table → all-saturated; treat as needing eviction. */
    size_t idx = (size_t)(h & log->mask);
    size_t first_empty = SIZE_MAX;
    for (size_t step = 0; step < log->cap; step++) {
        aeadNonceLogSlot *s = &log->slots[idx];
        if (s->gen == 0u) {
            first_empty = idx;
            /* Wipe-on-evict makes an empty slot terminate the chain. */
            break;
        }
        if (aeadNonceLogSlotMatches(s, key_id, nonce)) {
            pthread_mutex_unlock(&log->lock);
            return false;
        }
        idx = (idx + 1u) & log->mask;
    }

    /* Not a duplicate. Apply eviction policy if we're at-or-over the
     * 7/8 load threshold. This is O(1): we evict the FIFO head. */
    if (log->count >= (log->cap - (log->cap >> 3u))) {
        const size_t evicted = aeadNonceLogEvictHead(log);
        /* Eviction may have wiped a slot anywhere in the table,
         * including INSIDE our probe chain at a position earlier than
         * `first_empty`. If we inserted at the original first_empty
         * a future lookup would walk from h, hit the newly-emptied
         * earlier slot, and terminate without finding our entry.
         *
         * Re-walk the chain to take the earliest empty. We already
         * proved no duplicate exists, so we only need to find the
         * first empty (skipping matches isn't needed). */
        idx = (size_t)(h & log->mask);
        first_empty = SIZE_MAX;
        for (size_t step = 0; step < log->cap; step++) {
            if (log->slots[idx].gen == 0u) {
                first_empty = idx;
                break;
            }
            idx = (idx + 1u) & log->mask;
        }
        if (first_empty == SIZE_MAX) {
            /* No empty on chain even after eviction (eviction freed a
             * slot we never probe). Fall back to the evicted slot,
             * which lives on its own (possibly different) chain — but
             * the natural insertion point for OUR hash is still the
             * evicted index because we'll never reach it via the
             * regular probe from h. This is the same compromise the
             * pre-fix code made for the saturation case; the only
             * lookup that finds it is by full-scan re-probe (we never
             * use one). Accept the loss as fail-safe degradation. */
            first_empty = evicted;
        }
    }

    if (first_empty == SIZE_MAX) {
        /* Table was full and load policy didn't trigger (e.g. all
         * cap=64 slots are full but count miscounted). Force one
         * eviction. */
        first_empty = aeadNonceLogEvictHead(log);
    }

    aeadNonceLogSlot *s = &log->slots[first_empty];
    s->gen = log->next_gen++;
    if (log->next_gen == 0u) {
        /* Wrap is astronomical (2^64 inserts) but be defensive: skip
         * 0 to keep "0 == empty" invariant. */
        log->next_gen = 1u;
    }
    s->key_id = key_id;
    memcpy(s->nonce, nonce, AEAD_NONCE_LOG_NONCE_LEN);
    aeadNonceLogFifoPushTail(log, first_empty);
    log->count++;
    pthread_mutex_unlock(&log->lock);
    return true;
}

void aeadNonceLogClearKey(aeadNonceLog *log, uint32_t key_id) {
    if (log == NULL || key_id == 0u) {
        return;
    }
    pthread_mutex_lock(&log->lock);
    for (size_t i = 0; i < log->cap; i++) {
        if (log->slots[i].gen != 0u && log->slots[i].key_id == key_id) {
            aeadNonceLogFifoUnlink(log, i);
            volatile uint8_t *p = (volatile uint8_t *)&log->slots[i];
            for (size_t j = 0; j < sizeof(aeadNonceLogSlot); j++) {
                p[j] = 0;
            }
            log->count--;
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

/* Entries-per-record in the serialized flex. */
#define AEAD_NONCE_LOG_PERSIST_HEADER_ENTRIES ((size_t)3)
#define AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD ((size_t)4)

/* Build the flex encoding under the lock. Returns a freshly-allocated
 * flex; caller owns and frees via flexFree. Returns NULL on failure. */
static flex *anlBuildSnapshotFlex(const aeadNonceLog *log) {
    if (log == NULL) {
        return NULL;
    }
    flex *f = flexNew();
    if (f == NULL) {
        return NULL;
    }
    pthread_mutex_lock((pthread_mutex_t *)&log->lock);

    /* Header: version, capacity, record_count. */
    flexPushUnsigned(&f, AEAD_NONCE_LOG_PERSIST_VERSION, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&f, (uint64_t)log->cap, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&f, (uint64_t)log->count, FLEX_ENDPOINT_TAIL);

    /* Per-record block: key_id, nonce_lo (8B LE), nonce_hi (4B LE), gen. */
    for (size_t i = 0; i < log->cap; i++) {
        const aeadNonceLogSlot *s = &log->slots[i];
        if (s->gen == 0u) {
            continue;
        }
        uint64_t nonce_lo = 0;
        uint32_t nonce_hi = 0;
        memcpy(&nonce_lo, s->nonce, 8);
        memcpy(&nonce_hi, s->nonce + 8, 4);
        flexPushUnsigned(&f, (uint64_t)s->key_id, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, nonce_lo, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, (uint64_t)nonce_hi, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, s->gen, FLEX_ENDPOINT_TAIL);
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
    if (f == NULL) {
        return 0;
    }
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

/* Restore helper: scratch record collected from the flex stream so we
 * can re-establish FIFO order in ascending gen across all records. */
typedef struct anlRestoreRec {
    uint64_t gen;
    uint32_t key_id;
    uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN];
} anlRestoreRec;

static int anlRestoreCmpGen(const void *a, const void *b) {
    const anlRestoreRec *ra = (const anlRestoreRec *)a;
    const anlRestoreRec *rb = (const anlRestoreRec *)b;
    if (ra->gen < rb->gen) {
        return -1;
    }
    if (ra->gen > rb->gen) {
        return 1;
    }
    return 0;
}

aeadNonceLog *aeadNonceLogFromBytes(const uint8_t *buf, size_t len) {
    if (buf == NULL) {
        return NULL;
    }
    if (!flexValidateBuffer(buf, len)) {
        return NULL;
    }
    const flex *f = (const flex *)buf;
    const size_t total = flexCount(f);
    if (total < AEAD_NONCE_LOG_PERSIST_HEADER_ENTRIES) {
        return NULL;
    }
    const size_t payload = total - AEAD_NONCE_LOG_PERSIST_HEADER_ENTRIES;
    if ((payload % AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD) != 0u) {
        return NULL;
    }
    const size_t declared_records =
        payload / AEAD_NONCE_LOG_PERSIST_ENTRIES_PER_RECORD;

    flexEntry *fe = flexHead(f);
    if (fe == NULL) {
        return NULL;
    }

    /* Header entry 0: version. */
    databox box;
    flexGetByTypeCopy(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64 ||
        box.data.u64 != AEAD_NONCE_LOG_PERSIST_VERSION) {
        return NULL;
    }
    fe = flexNext(f, fe);
    if (fe == NULL) {
        return NULL;
    }

    /* Header entry 1: capacity. */
    flexGetByTypeCopy(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return NULL;
    }
    const uint64_t saved_cap = box.data.u64;
    if (saved_cap == 0u || saved_cap > ((uint64_t)1 << 40u)) {
        return NULL; /* obviously bogus */
    }
    fe = flexNext(f, fe);
    if (fe == NULL) {
        return NULL;
    }

    /* Header entry 2: record_count. */
    flexGetByTypeCopy(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return NULL;
    }
    const uint64_t saved_count = box.data.u64;
    if (saved_count != (uint64_t)declared_records) {
        return NULL; /* header and payload disagree */
    }
    if (saved_count > saved_cap) {
        return NULL; /* count exceeds declared capacity */
    }
    fe = flexNext(f, fe);

    /* Allocate a fresh log with the saved capacity. */
    aeadNonceLog *log = aeadNonceLogCreate((size_t)saved_cap);
    if (log == NULL) {
        return NULL;
    }
    /* Round-up should match the original cap; if not the snapshot is
     * malformed. */
    if (aeadNonceLogCapacity(log) != (size_t)saved_cap) {
        aeadNonceLogFree(log);
        return NULL;
    }

    /* Collect every record from the flex into a scratch array so we
     * can sort by gen and rebuild the FIFO list in insertion order. */
    anlRestoreRec *recs = NULL;
    if (declared_records > 0u) {
        recs = zmalloc(declared_records * sizeof(*recs));
        if (recs == NULL) {
            aeadNonceLogFree(log);
            return NULL;
        }
    }
    uint64_t max_gen = 0;
    for (size_t i = 0; i < declared_records; i++) {
        if (fe == NULL) {
            if (recs) {
                zfree(recs);
            }
            aeadNonceLogFree(log);
            return NULL;
        }
        /* a) key_id (unsigned, range-checked to uint32_t, non-zero). */
        flexGetByTypeCopy(fe, &box);
        if (box.type != DATABOX_UNSIGNED_64) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        if (box.data.u64 == 0u || box.data.u64 > UINT32_MAX) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        recs[i].key_id = (uint32_t)box.data.u64;
        fe = flexNext(f, fe);
        if (fe == NULL) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }

        /* b) nonce_lo (unsigned 64-bit). */
        flexGetByTypeCopy(fe, &box);
        if (box.type != DATABOX_UNSIGNED_64) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        const uint64_t nonce_lo = box.data.u64;
        fe = flexNext(f, fe);
        if (fe == NULL) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }

        /* c) nonce_hi (unsigned, range-checked to uint32_t). */
        flexGetByTypeCopy(fe, &box);
        if (box.type != DATABOX_UNSIGNED_64) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        if (box.data.u64 > UINT32_MAX) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        const uint32_t nonce_hi = (uint32_t)box.data.u64;
        fe = flexNext(f, fe);
        if (fe == NULL) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }

        /* d) gen (unsigned 64-bit, non-zero). */
        flexGetByTypeCopy(fe, &box);
        if (box.type != DATABOX_UNSIGNED_64) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        if (box.data.u64 == 0u) {
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
        recs[i].gen = box.data.u64;
        if (recs[i].gen > max_gen) {
            max_gen = recs[i].gen;
        }
        fe = flexNext(f, fe);

        /* Reconstruct the nonce bytes from the two halves (little-endian
         * layout — matches the serializer above). */
        memcpy(recs[i].nonce, &nonce_lo, 8);
        memcpy(recs[i].nonce + 8, &nonce_hi, 4);
    }

    /* Sort by gen ascending so FIFO order in the rebuilt list matches
     * the original insertion order: oldest gen at head, newest at tail. */
    if (declared_records > 1u) {
        qsort(recs, declared_records, sizeof(*recs), anlRestoreCmpGen);
    }

    /* Install each record (in gen order) using the same hash + probe
     * logic as Observe, but write the saved gen rather than the fresh
     * counter, and link into the FIFO list in order. */
    for (size_t i = 0; i < declared_records; i++) {
        const uint64_t h = aeadNonceLogHash(recs[i].key_id, recs[i].nonce);
        size_t idx = (size_t)(h & log->mask);
        bool placed = false;
        for (size_t step = 0; step < log->cap; step++) {
            aeadNonceLogSlot *s = &log->slots[idx];
            if (s->gen == 0u) {
                s->gen = recs[i].gen;
                s->key_id = recs[i].key_id;
                memcpy(s->nonce, recs[i].nonce, AEAD_NONCE_LOG_NONCE_LEN);
                aeadNonceLogFifoPushTail(log, idx);
                log->count++;
                placed = true;
                break;
            }
            if (s->key_id == recs[i].key_id &&
                dkCtMemEq(s->nonce, recs[i].nonce, AEAD_NONCE_LOG_NONCE_LEN)) {
                /* Duplicate (key_id, nonce) pair — refuse the snapshot.
                 * validate also enforces this on top, but rejecting
                 * here keeps the freshly-built log internally consistent. */
                zfree(recs);
                aeadNonceLogFree(log);
                return NULL;
            }
            idx = (idx + 1u) & log->mask;
        }
        if (!placed) {
            /* probe chain saturated — impossible if header
             * record_count <= capacity, but be defensive. */
            zfree(recs);
            aeadNonceLogFree(log);
            return NULL;
        }
    }

    if (recs) {
        zfree(recs);
    }

    /* Advance next_gen past the highest restored gen so the first new
     * Observe doesn't collide with a restored slot. */
    if (max_gen + 1u != 0u) {
        log->next_gen = max_gen + 1u;
    } else {
        log->next_gen = 1u;
    }

    return log;
}

/* ---- persistOps descriptor ----------------------------------------- */

static uint8_t *anlPersistSnapshot(const void *structure, size_t *len) {
    const aeadNonceLog *log = (const aeadNonceLog *)structure;
    const size_t need = aeadNonceLogToBytes(log, NULL, 0);
    if (need == 0) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    uint8_t *out = zmalloc(need);
    if (out == NULL) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    const size_t written = aeadNonceLogToBytes(log, out, need);
    if (written != need) {
        zfree(out);
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (len != NULL) {
        *len = written;
    }
    return out;
}

static void *anlPersistRestore(const uint8_t *data, size_t len) {
    return aeadNonceLogFromBytes(data, len);
}

static void *anlPersistNewEmpty(void) {
    return aeadNonceLogCreate(0);
}

static size_t anlPersistCount(const void *structure) {
    return aeadNonceLogSize((const aeadNonceLog *)structure);
}

static size_t anlPersistEstimateSize(const void *structure) {
    return aeadNonceLogToBytes((const aeadNonceLog *)structure, NULL, 0);
}

/* validate hook: confirms the restored set has unique (key_id, nonce)
 * pairs and that count matches the live slot count. Called after
 * restore returns; takes the structure pointer. */
static bool anlPersistValidate(const void *structure) {
    const aeadNonceLog *log = (const aeadNonceLog *)structure;
    if (log == NULL) {
        return false;
    }
    /* Live-slot count must agree with the count field. */
    size_t live = 0;
    for (size_t i = 0; i < log->cap; i++) {
        if (log->slots[i].gen != 0u) {
            live++;
        }
    }
    if (live != log->count) {
        return false;
    }
    /* Capacity must be a power of two and >= the minimum. */
    if (log->cap < AEAD_NONCE_LOG_MIN_CAP) {
        return false;
    }
    if ((log->cap & (log->cap - 1u)) != 0u) {
        return false;
    }
    if (log->mask != log->cap - 1u) {
        return false;
    }
    /* No duplicate (key_id, nonce) pairs. O(N^2) but only runs once
     * post-restore. */
    for (size_t i = 0; i < log->cap; i++) {
        const aeadNonceLogSlot *a = &log->slots[i];
        if (a->gen == 0u) {
            continue;
        }
        if (a->key_id == 0u) {
            return false;
        }
        for (size_t j = i + 1; j < log->cap; j++) {
            const aeadNonceLogSlot *b = &log->slots[j];
            if (b->gen == 0u) {
                continue;
            }
            if (a->key_id == b->key_id &&
                dkCtMemEq(a->nonce, b->nonce, AEAD_NONCE_LOG_NONCE_LEN)) {
                return false;
            }
        }
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
            if (log->slots[cur].gen == 0u) {
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
    .encodeOp = NULL, /* nonce-log changes are observation-driven; full
                            re-snapshot is the durability model */
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

int aeadNonceLogTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("create/free round-trip") {
        aeadNonceLog *log = aeadNonceLogCreate(0);
        if (log == NULL) {
            ERRR("Create returned NULL");
        }
        if (aeadNonceLogCapacity(log) < 64u) {
            ERR("capacity below minimum: %zu", aeadNonceLogCapacity(log));
        }
        aeadNonceLogFree(log);
    }

    TEST("first observation accepted; second rejected") {
        aeadNonceLog *log = aeadNonceLogCreate(0);
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
        aeadNonceLogFree(log);
    }

    TEST("different key_id is a different tuple") {
        aeadNonceLog *log = aeadNonceLogCreate(0);
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
        aeadNonceLogFree(log);
    }

    TEST("clearKey forgets all entries for one key only") {
        aeadNonceLog *log = aeadNonceLogCreate(0);
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
        aeadNonceLogFree(log);
    }

    TEST("FIFO eviction kicks in past load threshold") {
        const size_t cap = 128u;
        aeadNonceLog *log = aeadNonceLogCreate(cap);
        uint8_t nonce[12] = {0};
        /* Fill past capacity — every observation should succeed (eviction
         * makes room) and size never exceeds cap. */
        for (uint64_t i = 0; i < cap * 4u; i++) {
            nonceFromCounter(i, nonce);
            if (!aeadNonceLogObserve(log, 1u, nonce)) {
                ERR("observe iter %llu refused", (unsigned long long)i);
                break;
            }
            if (aeadNonceLogSize(log) > aeadNonceLogCapacity(log)) {
                ERRR("size exceeded capacity");
            }
        }
        aeadNonceLogFree(log);
    }

    TEST("invalid inputs rejected") {
        aeadNonceLog *log = aeadNonceLogCreate(0);
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
        aeadNonceLogFree(log);
    }

    TEST("concurrent observers: every unique tuple observed exactly once") {
        /* Threads write disjoint nonce ranges under the same key. The
         * union should fit under capacity so no eviction kicks in;
         * post-join we verify every tuple is rejected as a duplicate. */
        aeadNonceLog *log = aeadNonceLogCreate(8192u);

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
                ERR("nonce %llu unexpectedly insertable post-fill",
                    (unsigned long long)i);
                break;
            }
        }
        aeadNonceLogFree(log);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */