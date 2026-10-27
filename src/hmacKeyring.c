/* hmacKeyring — see hmacKeyring.h for the API contract and threading
 * model. Implementation is a fixed-capacity flat array of slots; lookup
 * is a linear scan because typical cluster rotations carry 2-3 active
 * keys (current + previous + sometimes staged-next). Scanning 16 slots
 * is faster than a hash-table probe at this size. */

#include "hmacKeyring.h"

#include "aesGcm.h" /* aesGcmKey + aesGcmInit for the cached schedule */
#include "cleaner.h"
#include "ctmem.h"   /* dkSecureZero */
#include "datakit.h" /* zmalloc / zfree */
#include "dkDiag.h"
#include "dkEndian.h" /* dkStoreLe64 */
#include "flex.h"
#include "persist.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* Scope-exit cleaners (see cleaner.h idioms). Buffers holding key
 * material are wiped before free by their owning code paths and so are
 * NOT managed here — only allocations whose disposal is a plain zfree
 * carry a cleaner. */
defcleanerName(hmacKeyring *, zfree, Ring);
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(uint64_t *, zfree, U64);
defcleaner(flex *, flexFree);

#define HMAC_KEYRING_CAPACITY ((size_t)16)

/* Layout: largest to smallest fields to minimize padding. */
typedef struct hmacKeyringEntry {
    const hmacHashAlgo *algo;  /* borrowed; descriptor is module-static */
    uint8_t *raw_key_bytes;    /* zmalloc'd; zero-cleaned on retire/free */
    uint8_t *prepared_storage; /* zmalloc'd; backs prepared.inner/outer_ctx */
    /* AEAD key-schedule cache . When the installed key
     * is a valid AES-GCM key length (16 == AES-128, 32 == AES-256), Install
     * pre-runs aesGcmInit and stashes the expanded schedule here so the
     * hot envelope-Open path can skip the per-frame key expansion. NULL
     * when the key length is not a valid AES-GCM length (e.g. 20- or 64-
     * byte HMAC keys) — callers must fall back to ad-hoc aesGcmInit on the
     * raw bytes. Lifetime is bound to the slot: hmacKeyringEntryClear
     * dkSecureZero's and frees this storage on Retire / re-Install so a
     * leaked schedule cannot recover the round keys. */
    aesGcmKey *aead_schedule;
    hmacKey prepared;
    _Atomic uint64_t aead_counter; /* next-to-be-issued AEAD nonce counter */
    /* Durable nonce reservation (used while the ring has a reservation
     * function): counter values below aead_limit may be issued, and
     * aead_reserved is the highest limit handed to the function,
     * confirmed or not, which Snapshot records.  Both change under the
     * write lock and are read under the lock. */
    uint64_t aead_limit;
    uint64_t aead_reserved;
    /* Generation counter — bumped on EVERY mutation of this slot
     * (Install, Retire, SetAeadSalt). Attach-style consumers cache the
     * value observed at attach time and refuse to use the key if the
     * generation has advanced. Stays monotonic across the slot's full
     * lifetime — Retire does NOT reset it to zero, so a retire-then-
     * reinstall under the same id is still distinguishable from "the
     * exact slot you attached to". */
    uint64_t generation;
    size_t raw_key_len;
    hmacKeyId id; /* HMAC_KEY_ID_INVALID iff slot is free */
    /* 4-byte salt suffix appended to every AEAD nonce. When the slot
     * was installed via hmacKeyringInstallUnsalted this stays all-zero
     * AND `is_unsalted` is set, which together suppress the fail-closed
     * check in hmacKeyringNextAeadNonce. */
    uint8_t aead_salt[HMAC_KEYRING_AEAD_SALT_BYTES];
    bool is_unsalted; /* true => zero-salt nonces are explicitly OK */
    uint8_t _pad[3];
} hmacKeyringEntry;

struct hmacKeyring {
    pthread_rwlock_t lock;
    hmacKeyringEntry entries[HMAC_KEYRING_CAPACITY];
    size_t count;
    /* Revoked key floor . Monotonically non-decreasing
     * minimum key id permitted by floor-aware consumers
     * (flexEnvelopeVerify reads it via hmacKeyringMinValidKeyId). Zero
     * means "no floor"; any installed id is acceptable. Persisted as the
     * snapshot's first entry so a restart cannot silently un-revoke
     * ids. */
    hmacKeyId min_valid_key_id;
    /* Durable nonce reservation (hmacKeyringSetAeadReservation).  The
     * function, its context and the window change under the write lock;
     * reserve_mu serializes reservations, so one runs at a time and a
     * change of function waits for one in flight.  Lock order: reserve_mu,
     * then `lock`. */
    pthread_mutex_t reserve_mu;
    hmacKeyringAeadReserveFn reserve_fn;
    void *reserve_ctx;
    uint64_t reserve_window;
};

/* Thin forwarder kept so call sites stay self-describing. The actual
 * DCE-resistant zero-write lives in `ctmem.h::dkSecureZero` and is shared
 * with every other crypto module. */
static inline void hmacKeyringSecureWipe(void *ptr, size_t len) {
    dkSecureZero(ptr, len);
}

/* Secret-holding heap scratch with its wipe length: the scope-exit
 * cleanup wipes DCE-resistantly then frees, so every early return
 * releases key material safely. Success paths disarm by zeroing the
 * holder ({0}) after ownership transfers into a keyring slot. */
typedef struct hkrSecretBuf {
    void *ptr;
    size_t len;
} hkrSecretBuf;

DK_FN_UNUSED static void hkrSecretBufWipeFree_cleanup(hkrSecretBuf *b) {
    if (b->ptr != nullptr) {
        hmacKeyringSecureWipe(b->ptr, b->len);
        zfree(b->ptr);
    }
}

/* Zero-clean + free everything an entry owns. Caller MUST hold the
 * write lock or be in the destructor path.
 *
 * Bumps the slot's `generation` counter so attach-style consumers that
 * cached the pre-clear generation can detect the rotation and refuse to
 * use the now-freed key bytes (see hmacKeyringGetGeneration docs). */
static void hmacKeyringEntryClear(hmacKeyringEntry *e) {
    if (e->raw_key_bytes != nullptr) {
        hmacKeyringSecureWipe(e->raw_key_bytes, e->raw_key_len);
        zfree(e->raw_key_bytes);
        e->raw_key_bytes = nullptr;
    }
    if (e->prepared_storage != nullptr && e->algo != nullptr) {
        hmacKeyringSecureWipe(e->prepared_storage, hmacPreparedSize(e->algo));
        zfree(e->prepared_storage);
        e->prepared_storage = nullptr;
    }
    /* AEAD schedule : the expanded round keys are key-equivalent
     * material — recovering them yields the AES key bits. Wipe DCE-
     * resistantly before free so a heap-scrubbing analyzer (or a careless
     * realloc) cannot harvest the schedule. */
    if (e->aead_schedule != nullptr) {
        dkSecureZero(e->aead_schedule, sizeof(*e->aead_schedule));
        zfree(e->aead_schedule);
        e->aead_schedule = nullptr;
    }
    e->algo = nullptr;
    e->raw_key_len = 0;
    e->id = HMAC_KEY_ID_INVALID;
    /* @sync: relaxed owner */
    atomic_store_explicit(&e->aead_counter, 0u, memory_order_relaxed);
    e->aead_limit = 0;
    e->aead_reserved = 0;
    /* Salt is sensitive: it scopes AEAD nonce derivation; wipe DCE-resistantly
     * even though most callers reach this via the destructor path. */
    dkSecureZero(e->aead_salt, sizeof(e->aead_salt));
    dkSecureZero(&e->prepared, sizeof(e->prepared));
    /* Reset the unsalted flag so a recycled slot defaults back to the
     * safe (fail-closed) state. A fresh Install must opt back in via
     * hmacKeyringInstallUnsalted if it wants zero-salt behavior. */
    e->is_unsalted = false;
    /* Bump generation last — the counter is monotonic across the slot's
     * full lifetime so a retire-then-reinstall under the same id stays
     * distinguishable from "the exact slot you attached to". */
    e->generation++;
}

/* Two variants of the slot lookup so the lock discipline shows up in
 * the type system rather than via a const-cast. Read-lock holders
 * call the Const form (and only get const access to the slot they
 * find); write-lock holders call the Mut form.
 *
 * Bodies are identical — the Const form is the actual scan, the Mut
 * form is a thin wrapper. The const-cast inside Mut is locally safe
 * because the caller has guaranteed write-lock ownership; the cast
 * is documented at the call site (lookup-then-mutate) rather than
 * hidden inside one merged function. */
static const hmacKeyringEntry *
hmacKeyringFindLockedConst(const hmacKeyring *ring, hmacKeyId id) {
    if (id == HMAC_KEY_ID_INVALID) {
        return nullptr;
    }
    for (size_t i = 0; i < HMAC_KEYRING_CAPACITY; i++) {
        if (ring->entries[i].id == id) {
            return &ring->entries[i];
        }
    }
    return nullptr;
}

static hmacKeyringEntry *hmacKeyringFindLockedMut(hmacKeyring *ring,
                                                  hmacKeyId id) {
    /* Caller holds the write lock — discard const safely. Keeps the
     * body single-source and avoids two identical scan loops drifting. */
    return (hmacKeyringEntry *)hmacKeyringFindLockedConst(ring, id);
}

/* Find a free slot (id == HMAC_KEY_ID_INVALID). Caller must hold the
 * write lock. */
static hmacKeyringEntry *hmacKeyringFindFreeLocked(hmacKeyring *ring) {
    for (size_t i = 0; i < HMAC_KEYRING_CAPACITY; i++) {
        if (ring->entries[i].id == HMAC_KEY_ID_INVALID) {
            return &ring->entries[i];
        }
    }
    return nullptr;
}

hmacKeyring *hmacKeyringCreate(void) {
    cleanerName(zfree, Ring) hmacKeyring *ring = zmalloc(sizeof(*ring));
    memset(ring, 0, sizeof(*ring));
    if (pthread_rwlock_init(&ring->lock, nullptr) != 0) {
        return nullptr;
    }
    if (pthread_mutex_init(&ring->reserve_mu, nullptr) != 0) {
        pthread_rwlock_destroy(&ring->lock);
        return nullptr;
    }
    hmacKeyring *out = ring;
    ring = nullptr;
    return out;
}

void hmacKeyringFree(hmacKeyring *ring) {
    if (ring == nullptr) {
        return;
    }
    /* No lock needed — destructor; caller guarantees no concurrent use. */
    for (size_t i = 0; i < HMAC_KEYRING_CAPACITY; i++) {
        hmacKeyringEntryClear(&ring->entries[i]);
    }
    pthread_mutex_destroy(&ring->reserve_mu);
    pthread_rwlock_destroy(&ring->lock);
    zfree(ring);
}

/* Constant-time check that the salt is all-zero. Constant-time isn't
 * load-bearing for security here (the salt is non-secret), but matches
 * the rest of the module's discipline against compiler short-circuits. */
static bool
hmacKeyringSaltIsZero(const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES]) {
    uint8_t acc = 0;
    for (size_t i = 0; i < HMAC_KEYRING_AEAD_SALT_BYTES; i++) {
        acc = (uint8_t)(acc | salt[i]);
    }
    return acc == 0u;
}

/* Shared install body for the salted/unsalted public entry points. The
 * caller has already validated `aead_salt` against the salted-API
 * contract (or chosen to pass all-zero via the unsalted entry point). */
static bool hmacKeyringInstallInternal(
    hmacKeyring *ring, hmacKeyId id, const hmacHashAlgo *algo,
    const uint8_t *key_bytes, size_t key_len,
    const uint8_t aead_salt[HMAC_KEYRING_AEAD_SALT_BYTES], bool is_unsalted) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || algo == nullptr ||
        key_bytes == nullptr || key_len == 0) {
        return false;
    }
    /* Refuse oversized keys at install time so the cap is enforced
     * uniformly across the in-memory and snapshot-restore entry paths.
     * 256 bytes covers every supported algorithm with headroom (see
     * HMAC_KEYRING_MAX_KEY_BYTES in the header). */
    if (key_len > HMAC_KEYRING_MAX_KEY_BYTES) {
        return false;
    }

    /* Pre-allocate everything OUTSIDE the lock — keeps the write critical
     * section short and avoids holding the lock across malloc. */
    const size_t prepared_size = hmacPreparedSize(algo);
    if (prepared_size == 0) {
        return false;
    }
    cleanerName(hkrSecretBuf, WipeFree) hkrSecretBuf rawCopyBuf = {};
    /* @own: container; free=zfree */
    rawCopyBuf.ptr = zmalloc(key_len);
    rawCopyBuf.len = key_len;
    cleanerName(hkrSecretBuf, WipeFree) hkrSecretBuf preparedBuf = {};
    /* @own: container; free=zfree */
    preparedBuf.ptr = zmalloc(prepared_size);
    preparedBuf.len = prepared_size;
    uint8_t *raw_copy = rawCopyBuf.ptr;
    uint8_t *prepared_storage = preparedBuf.ptr;
    memcpy(raw_copy, key_bytes, key_len);

    hmacKey prepared;
    hmacPrepareKey(&prepared, algo, prepared_storage, prepared_size, raw_copy,
                   key_len);

    /* pre-compute the AES-GCM key schedule whenever the
     * key length is a valid AES-GCM length (16 == AES-128, 32 == AES-256).
     * The schedule reuses the SAME aesGcmInit dispatcher the envelope-Open
     * path calls per-frame today, so cache hits produce bit-identical
     * ciphertext/tag to the cold path. We do this OUTSIDE the keyring
     * lock and bind to the slot after the duplicate-id check passes; if
     * either pre-flight (prepared_storage allocation, schedule init) trips
     * we free everything before unlocking.
     *
     * Note: a 32-byte HMAC-SHA256 key technically has a valid AES-256 key
     * length and so will be cached too — this costs ~280 bytes per slot
     * and is never consumed unless the envelope-Open path explicitly asks
     * via hmacKeyringGetAesGcmKey. */
    cleanerName(hkrSecretBuf, WipeFree) hkrSecretBuf schedBuf = {};
    aesGcmKey *aead_sched = nullptr;
    if (key_len == AES128_KEY_LEN || key_len == AES256_KEY_LEN) {
        /* @own: container; free=zfree */
        schedBuf.ptr = zmalloc(sizeof(*aead_sched));
        schedBuf.len = sizeof(*aead_sched);
        aead_sched = schedBuf.ptr;
        if (!aesGcmInit(aead_sched, raw_copy, key_len)) {
            /* Should be unreachable — aesGcmInit only rejects invalid
             * lengths and we just gated on the valid set. The scope-exit
             * wipe+free covers this and every other failure path. */
            return false;
        }
    }

    pthread_rwlock_wrlock(&ring->lock);

    if (hmacKeyringFindLockedMut(ring, id) != nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }

    hmacKeyringEntry *slot = hmacKeyringFindFreeLocked(ring);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }

    slot->algo = algo;
    slot->raw_key_bytes = raw_copy;
    slot->prepared_storage = prepared_storage;
    slot->prepared = prepared;
    slot->aead_schedule = aead_sched;
    slot->raw_key_len = key_len;
    slot->id = id;
    /* @sync: relaxed owner */
    atomic_store_explicit(&slot->aead_counter, 0u, memory_order_relaxed);
    /* With a reservation function, the first nonce reserves. */
    slot->aead_limit = 0;
    slot->aead_reserved = 0;
    memcpy(slot->aead_salt, aead_salt, HMAC_KEYRING_AEAD_SALT_BYTES);
    slot->is_unsalted = is_unsalted;
    /* Bump generation on Install — a previously-attached consumer holding
     * the pre-Install generation must reject the now-different key bound
     * to this id, even if the surrounding code never explicitly Retired
     * the slot (e.g. fresh slot acquired via a different path). */
    slot->generation++;
    ring->count++;

    /* Ownership now lives in the slot — disarm the scope-exit wipes. */
    rawCopyBuf = (hkrSecretBuf){};
    preparedBuf = (hkrSecretBuf){};
    schedBuf = (hkrSecretBuf){};

    pthread_rwlock_unlock(&ring->lock);
    return true;
}

bool hmacKeyringInstall(hmacKeyring *ring, hmacKeyId id,
                        const hmacHashAlgo *algo, const uint8_t *key_bytes,
                        size_t key_len,
                        const uint8_t aead_salt[HMAC_KEYRING_AEAD_SALT_BYTES]) {
    /* Salted API: refuse NULL salt and all-zero salt. All-zero salt
     * defeats the cross-process collision protection that motivates
     * making `aead_salt` mandatory in the first place; callers wanting
     * that behavior must reach for the explicit Unsalted entry point. */
    if (aead_salt == nullptr || hmacKeyringSaltIsZero(aead_salt)) {
        DK_DIAG(DK_DIAG_HMAC_KEYRING_ZERO_SALT,
                "key %" PRIu32 ": %s salt (hmacKeyringInstallUnsalted is the "
                "unsalted entry point)",
                id, aead_salt == nullptr ? "a NULL" : "an all-zero");
        return false;
    }
    return hmacKeyringInstallInternal(ring, id, algo, key_bytes, key_len,
                                      aead_salt, /*is_unsalted=*/false);
}

bool hmacKeyringInstallUnsalted(hmacKeyring *ring, hmacKeyId id,
                                const hmacHashAlgo *algo,
                                const uint8_t *key_bytes, size_t key_len) {
    static constexpr uint8_t zero_salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {};
    return hmacKeyringInstallInternal(ring, id, algo, key_bytes, key_len,
                                      zero_salt, /*is_unsalted=*/true);
}

bool hmacKeyringRetire(hmacKeyring *ring, hmacKeyId id) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID) {
        return false;
    }
    pthread_rwlock_wrlock(&ring->lock);
    hmacKeyringEntry *slot = hmacKeyringFindLockedMut(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    hmacKeyringEntryClear(slot);
    ring->count--;
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

size_t hmacKeyringSize(const hmacKeyring *ring) {
    if (ring == nullptr) {
        return 0;
    }
    pthread_rwlock_rdlock((pthread_rwlock_t *)&ring->lock);
    const size_t n = ring->count;
    pthread_rwlock_unlock((pthread_rwlock_t *)&ring->lock);
    return n;
}

const hmacKey *hmacKeyringLookup(hmacKeyring *ring, hmacKeyId id) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID) {
        return nullptr;
    }
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    const hmacKey *result = (slot != nullptr) ? &slot->prepared : nullptr;
    pthread_rwlock_unlock(&ring->lock);
    return result;
}

bool hmacKeyringGetRaw(hmacKeyring *ring, hmacKeyId id,
                       const uint8_t **out_key_bytes, size_t *out_key_len,
                       const hmacHashAlgo **out_algo) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID ||
        out_key_bytes == nullptr || out_key_len == nullptr) {
        return false;
    }
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    *out_key_bytes = slot->raw_key_bytes;
    *out_key_len = slot->raw_key_len;
    if (out_algo != nullptr) {
        *out_algo = slot->algo;
    }
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

bool hmacKeyringGetRawCopy(hmacKeyring *ring, hmacKeyId id, uint8_t *out_buf,
                           size_t out_cap, size_t *out_len,
                           const hmacHashAlgo **out_algo) {
    if (out_len != nullptr) {
        *out_len = 0;
    }
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || out_buf == nullptr) {
        return false;
    }
    /* Copy under the read lock. Combined Find + memcpy is the entire
     * point of this entry point: a concurrent Retire is serialised
     * against the rdlock and cannot free the bytes between our find and
     * our copy. Once we unlock, the caller owns a fresh copy that is
     * independent of any subsequent keyring mutation. */
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    if (out_cap < slot->raw_key_len) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    memcpy(out_buf, slot->raw_key_bytes, slot->raw_key_len);
    const size_t copied = slot->raw_key_len;
    const hmacHashAlgo *algo = slot->algo;
    pthread_rwlock_unlock(&ring->lock);

    if (out_len != nullptr) {
        *out_len = copied;
    }
    if (out_algo != nullptr) {
        *out_algo = algo;
    }
    return true;
}

const aesGcmKey *hmacKeyringGetAesGcmKey(hmacKeyring *ring, hmacKeyId id) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID) {
        return nullptr;
    }
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    const aesGcmKey *result = (slot != nullptr) ? slot->aead_schedule : nullptr;
    pthread_rwlock_unlock(&ring->lock);
    return result;
}

bool hmacKeyringGetAesGcmKeyCopy(hmacKeyring *ring, hmacKeyId id,
                                 aesGcmKey *out_key) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || out_key == nullptr) {
        return false;
    }
    /* Copy the schedule under the read lock — a concurrent Retire is
     * serialised against the rdlock and cannot free the slot's
     * aead_schedule between our find and our memcpy. Once we unlock,
     * the caller owns a self-contained struct independent of any
     * subsequent keyring mutation. The round-key block is ~256 bytes;
     * cheap to copy under the lock. */
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    if (slot == nullptr || slot->aead_schedule == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    memcpy(out_key, slot->aead_schedule, sizeof(*out_key));
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

bool hmacKeyringGetGeneration(hmacKeyring *ring, hmacKeyId id,
                              uint64_t *out_generation) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID ||
        out_generation == nullptr) {
        return false;
    }
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    *out_generation = slot->generation;
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

/* ---- Revoked key floor ----------------------------------------- */

hmacKeyId hmacKeyringMinValidKeyId(const hmacKeyring *ring) {
    if (ring == nullptr) {
        return HMAC_KEY_ID_INVALID;
    }
    pthread_rwlock_rdlock((pthread_rwlock_t *)&ring->lock);
    const hmacKeyId floor = ring->min_valid_key_id;
    pthread_rwlock_unlock((pthread_rwlock_t *)&ring->lock);
    return floor;
}

bool hmacKeyringSetMinValidKeyId(hmacKeyring *ring, hmacKeyId floor) {
    if (ring == nullptr) {
        return false;
    }
    pthread_rwlock_wrlock(&ring->lock);
    /* Refuse backward moves: lowering the floor would silently re-permit
     * ids the operator already declared revoked, re-opening the replay
     * window this API exists to close. Equal value is treated as a
     * no-op success so callers can idempotently re-apply policy. */
    if (floor < ring->min_valid_key_id) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    ring->min_valid_key_id = floor;
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

/* ---- AEAD nonce-counter helpers --------------------------------------- */

/* Reservation slow path: `id`'s counter reached its confirmed limit.
 * Hands counter + window to the reservation function and, once it reports
 * the limit durable, confirms it for the same key incarnation.  Returns
 * false when the function refuses or the slot is gone; true when the
 * caller may try to issue again (also when another thread reserved first,
 * or reservation was turned off meanwhile). */
static bool hkrAeadReserve(hmacKeyring *ring, hmacKeyId id) {
    pthread_mutex_lock(&ring->reserve_mu);
    pthread_rwlock_wrlock(&ring->lock);
    hmacKeyringEntry *slot = hmacKeyringFindLockedMut(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        pthread_mutex_unlock(&ring->reserve_mu);
        return false;
    }
    const hmacKeyringAeadReserveFn fn = ring->reserve_fn;
    void *const ctx = ring->reserve_ctx;
    /* @sync: relaxed owner */
    const uint64_t counter =
        atomic_load_explicit(&slot->aead_counter, memory_order_relaxed);
    if (fn == nullptr || counter < slot->aead_limit) {
        pthread_rwlock_unlock(&ring->lock);
        pthread_mutex_unlock(&ring->reserve_mu);
        return true;
    }
    /* counter + window, saturating at UINT64_MAX */
    uint64_t limit;
    if (ckd_add(&limit, counter, ring->reserve_window)) {
        limit = UINT64_MAX;
    }
    /* Recorded before fn runs, so a snapshot taken while fn makes the
     * limit durable already restores the counter above it. */
    if (slot->aead_reserved < limit) {
        slot->aead_reserved = limit;
    }
    const size_t index = (size_t)(slot - ring->entries);
    const uint64_t generation = slot->generation;
    pthread_rwlock_unlock(&ring->lock);

    /* No keyring lock held: fn may snapshot this ring. */
    const bool durable = fn(ctx, id, limit);

    if (durable) {
        pthread_rwlock_wrlock(&ring->lock);
        hmacKeyringEntry *const e = &ring->entries[index];
        /* Only for the key that asked: a retire, reinstall or salt change
         * meanwhile bumped the generation. */
        if (e->id == id && e->generation == generation &&
            e->aead_limit < limit) {
            e->aead_limit = limit;
        }
        pthread_rwlock_unlock(&ring->lock);
    }
    pthread_mutex_unlock(&ring->reserve_mu);
    return durable;
}

bool hmacKeyringNextAeadNonce(hmacKeyring *ring, hmacKeyId id,
                              uint8_t out_nonce[12]) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || out_nonce == nullptr) {
        return false;
    }
    for (;;) {
        pthread_rwlock_rdlock(&ring->lock);
        const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
        if (slot == nullptr) {
            pthread_rwlock_unlock(&ring->lock);
            return false;
        }
        /* Fail-closed: refuse to issue a nonce whose salt is all-zero
         * unless the slot was explicitly installed as unsalted. A zero
         * salt on a "supposed-to-be-salted" slot would silently reuse
         * counter-0 nonces across processes that share the same key,
         * which destroys both ChaCha20-Poly1305 confidentiality and
         * integrity. */
        if (!slot->is_unsalted && hmacKeyringSaltIsZero(slot->aead_salt)) {
            pthread_rwlock_unlock(&ring->lock);
            return false;
        }
        /* CAS-loop to allocate the next counter atomically and refuse to
         * advance past UINT64_MAX (which would wrap on subsequent calls
         * and silently reuse a nonce), or, with a reservation function,
         * past the confirmed limit. Cast away const on the atomic field —
         * the rwlock keeps the slot alive (and the limit fixed); the
         * field itself is atomic. */
        _Atomic uint64_t *ctr = (_Atomic uint64_t *)&slot->aead_counter;
        const bool reserving = ring->reserve_fn != nullptr;
        bool reserve = false;
        /* @sync: relaxed seed */
        uint64_t prev = atomic_load_explicit(ctr, memory_order_relaxed);
        for (;;) {
            if (prev == UINT64_MAX) {
                /* Counter exhausted — caller must rotate the key. */
                pthread_rwlock_unlock(&ring->lock);
                return false;
            }
            if (reserving && prev >= slot->aead_limit) {
                reserve = true;
                break;
            }
            /* @sync: relaxed counter */
            if (atomic_compare_exchange_weak_explicit(ctr, &prev, prev + 1u,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                break;
            }
            /* prev updated by the failed CAS; retry. */
        }
        if (!reserve) {
            /* Build the nonce: 8 bytes counter LE || 4 bytes salt. */
            dkStoreLe64(out_nonce, prev);
            out_nonce[8] = slot->aead_salt[0];
            out_nonce[9] = slot->aead_salt[1];
            out_nonce[10] = slot->aead_salt[2];
            out_nonce[11] = slot->aead_salt[3];
            pthread_rwlock_unlock(&ring->lock);
            return true;
        }
        pthread_rwlock_unlock(&ring->lock);
        if (!hkrAeadReserve(ring, id)) {
            return false;
        }
    }
}

bool hmacKeyringSetAeadReservation(hmacKeyring *ring, uint64_t window,
                                   hmacKeyringAeadReserveFn fn, void *ctx) {
    if (ring == nullptr || (fn != nullptr && window == 0)) {
        return false;
    }
    /* Waits out a reservation in flight, so the old function is not
     * called after this returns. */
    pthread_mutex_lock(&ring->reserve_mu);
    pthread_rwlock_wrlock(&ring->lock);
    ring->reserve_fn = fn;
    ring->reserve_ctx = fn != nullptr ? ctx : nullptr;
    ring->reserve_window = fn != nullptr ? window : 0;
    for (size_t i = 0; i < HMAC_KEYRING_CAPACITY; i++) {
        hmacKeyringEntry *const e = &ring->entries[i];
        if (e->id == HMAC_KEY_ID_INVALID) {
            continue;
        }
        /* Every slot reserves before its next nonce. */
        /* @sync: relaxed owner */
        e->aead_limit =
            atomic_load_explicit(&e->aead_counter, memory_order_relaxed);
    }
    pthread_rwlock_unlock(&ring->lock);
    pthread_mutex_unlock(&ring->reserve_mu);
    return true;
}

/* Raise `id`'s counter (and recorded reservation) to at least `limit`:
 * nonces below it may have been issued.  For WAL replay of a reservation
 * and for Restore's per-slot counter (UINT64_MAX restores an exhausted
 * key as exhausted).  A retired id is skipped. */
static void hkrApplyReservation(hmacKeyring *ring, hmacKeyId id,
                                uint64_t limit) {
    pthread_rwlock_wrlock(&ring->lock);
    hmacKeyringEntry *slot = hmacKeyringFindLockedMut(ring, id);
    if (slot != nullptr) {
        /* @sync: relaxed owner */
        if (atomic_load_explicit(&slot->aead_counter, memory_order_relaxed) <
            limit) {
            /* @sync: relaxed owner */
            atomic_store_explicit(&slot->aead_counter, limit,
                                  memory_order_relaxed);
        }
        if (slot->aead_reserved < limit) {
            slot->aead_reserved = limit;
        }
    }
    pthread_rwlock_unlock(&ring->lock);
}

bool hmacKeyringReserveThroughPersist(void *persistPtr, hmacKeyId id,
                                      uint64_t limit) {
    persist *p = persistPtr;
    if (p == nullptr) {
        return false;
    }
    const databox idBox = DATABOX_UNSIGNED(id);
    const databox limitBox = DATABOX_UNSIGNED(limit);
    const databox *const args[2] = {&idBox, &limitBox};
    switch (persistLogOp(p, PERSIST_OP_UPDATE, args, 2)) {
    case PERSIST_LOG_DURABLE:
        return true;
    case PERSIST_LOG_BUFFERED:
        /* The sync policy would write it later; a reservation must be
         * durable before a nonce below it is issued. */
        return persistSync(p);
    default:
        /* REFUSED, or logged with a failed sync (durability unknown) */
        return false;
    }
}

bool hmacKeyringSetAeadSalt(hmacKeyring *ring, hmacKeyId id,
                            const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES]) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || salt == nullptr) {
        return false;
    }
    pthread_rwlock_wrlock(&ring->lock);
    hmacKeyringEntry *slot = hmacKeyringFindLockedMut(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    memcpy(slot->aead_salt, salt, HMAC_KEYRING_AEAD_SALT_BYTES);
    /* If the caller supplied a real (non-zero) salt, transition the slot
     * out of the "unsalted" state — the slot is now safe for fail-closed
     * nonce issuance. A zero salt leaves `is_unsalted` untouched: an
     * unsalted slot stays unsalted; a salted slot stays salted (and will
     * subsequently fail NextAeadNonce until a non-zero salt is set). */
    if (!hmacKeyringSaltIsZero(slot->aead_salt)) {
        slot->is_unsalted = false;
    }
    /* Bump generation — changing the salt rotates the AEAD nonce
     * derivation surface for this key, which is a meaningful semantic
     * change for attach-style consumers that may have cached the prior
     * (key, salt) pairing. */
    slot->generation++;
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

bool hmacKeyringPeekAeadCounter(hmacKeyring *ring, hmacKeyId id,
                                uint64_t *out_counter) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID ||
        out_counter == nullptr) {
        return false;
    }
    pthread_rwlock_rdlock(&ring->lock);
    const hmacKeyringEntry *slot = hmacKeyringFindLockedConst(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    _Atomic uint64_t *ctr = (_Atomic uint64_t *)&slot->aead_counter;
    /* @sync: relaxed counter */
    *out_counter = atomic_load_explicit(ctr, memory_order_relaxed);
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

bool hmacKeyringRestoreAeadCounter(hmacKeyring *ring, hmacKeyId id,
                                   uint64_t value) {
    if (ring == nullptr || id == HMAC_KEY_ID_INVALID || value == UINT64_MAX) {
        return false;
    }
    pthread_rwlock_wrlock(&ring->lock);
    hmacKeyringEntry *slot = hmacKeyringFindLockedMut(ring, id);
    if (slot == nullptr) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    /* Strict forward-only: never move the counter backwards. */
    /* @sync: relaxed owner */
    const uint64_t current =
        atomic_load_explicit(&slot->aead_counter, memory_order_relaxed);
    if (value < current) {
        pthread_rwlock_unlock(&ring->lock);
        return false;
    }
    /* @sync: relaxed owner */
    atomic_store_explicit(&slot->aead_counter, value, memory_order_relaxed);
    pthread_rwlock_unlock(&ring->lock);
    return true;
}

/* ---- Snapshot / restore via flex --------------------------------------
 *
 * Per-slot encoding into a single flex: 5 entries per slot, in order:
 *
 * 1. id (unsigned)
 * 2. algo_tag (unsigned: 0 = SHA-256, 1 = BLAKE3)
 * 3. salt (4-byte bytes)
 * 4. aead_counter (unsigned)
 * 5. key (bytes, raw_key_len)
 *
 * Total entry count is always 5*N. flex framing is the wire wrapper —
 * no hand-rolled magic/length integers, no custom LE writers. */

#define HMAC_KEYRING_ENTRIES_PER_SLOT ((size_t)5)
#define HMAC_KEYRING_ALGO_TAG_SHA256 ((uint64_t)0)
#define HMAC_KEYRING_ALGO_TAG_BLAKE3 ((uint64_t)1)

/* Map an algo descriptor pointer to its persisted tag value. */
static bool hkrAlgoToTag(const hmacHashAlgo *algo, uint64_t *out_tag) {
    if (algo == &hmacAlgoSHA256) {
        *out_tag = HMAC_KEYRING_ALGO_TAG_SHA256;
        return true;
    }
    if (algo == &hmacAlgoBLAKE3) {
        *out_tag = HMAC_KEYRING_ALGO_TAG_BLAKE3;
        return true;
    }
    return false;
}

static const hmacHashAlgo *hkrTagToAlgo(uint64_t tag) {
    switch (tag) {
    case HMAC_KEYRING_ALGO_TAG_SHA256:
        return &hmacAlgoSHA256;
    case HMAC_KEYRING_ALGO_TAG_BLAKE3:
        return &hmacAlgoBLAKE3;
    default:
        return nullptr;
    }
}

/* Build the flex encoding under the read lock. Returns a freshly
 * zmalloc'd flex; caller owns and frees via flexFree. Returns NULL if
 * any installed slot uses an algo we cannot tag. */
static flex *hkrBuildSnapshotFlex(const hmacKeyring *ring) {
    cleaner(flexFree) flex *f = flexNew();
    pthread_rwlock_rdlock((pthread_rwlock_t *)&ring->lock);
    /* [floor, then one row per installed slot]: the minimum valid key id
     * (0 when none is set), then rows of HMAC_KEYRING_ENTRIES_PER_SLOT
     * entries [id, algo tag, salt, counter, key], each appended in one
     * reallocation and stored exactly as given (a salt or key whose
     * bytes spell a number stays bytes; Restore accepts only bytes
     * there). */
    const databox floor = DATABOX_UNSIGNED(ring->min_valid_key_id);
    flexPushByTypeExact(&f, &floor, FLEX_ENDPOINT_TAIL);
    for (size_t i = 0; i < HMAC_KEYRING_CAPACITY; i++) {
        const hmacKeyringEntry *e = &ring->entries[i];
        if (e->id == HMAC_KEY_ID_INVALID) {
            continue;
        }
        uint64_t tag = 0;
        if (!hkrAlgoToTag(e->algo, &tag)) {
            pthread_rwlock_unlock((pthread_rwlock_t *)&ring->lock);
            return nullptr;
        }
        flexRow row = FLEX_ROW_INIT;
        flexRowUnsigned(&row, (uint64_t)e->id);
        flexRowUnsigned(&row, tag);
        flexRowBytes(&row, e->aead_salt, sizeof(e->aead_salt));
        /* Where a restore starts the counter: above every nonce issued,
         * and every limit handed to a reservation function (one may be
         * in flight; its nonces are not issued until it is durable). */
        /* @sync: relaxed counter */
        const uint64_t counter =
            atomic_load_explicit(&e->aead_counter, memory_order_relaxed);
        flexRowUnsigned(&row, counter > e->aead_reserved ? counter
                                                         : e->aead_reserved);
        flexRowBytes(&row, e->raw_key_bytes, e->raw_key_len);
        flexRowAppendExact(&f, &row);
    }
    pthread_rwlock_unlock((pthread_rwlock_t *)&ring->lock);
    /* @own: caller; free=flexFree */
    flex *out = f;
    f = nullptr;
    return out;
}

size_t hmacKeyringSnapshotSize(const hmacKeyring *ring) {
    if (ring == nullptr) {
        return 0;
    }
    flex *f = hkrBuildSnapshotFlex(ring);
    if (f == nullptr) {
        return 0;
    }
    const size_t n = flexBytes(f);
    flexFree(f);
    return n;
}

bool hmacKeyringSnapshot(const hmacKeyring *ring, uint8_t *out, size_t out_cap,
                         size_t *out_written) {
    if (ring == nullptr || out == nullptr) {
        return false;
    }
    flex *f = hkrBuildSnapshotFlex(ring);
    if (f == nullptr) {
        return false;
    }
    const size_t need = flexBytes(f);
    if (out_cap < need) {
        flexFree(f);
        return false;
    }
    memcpy(out, f, need);
    flexFree(f);
    if (out_written != nullptr) {
        *out_written = need;
    }
    return true;
}

/* Track installed ids so we can roll back on partial failure. */
typedef struct hkrRestoreUndoLog {
    hmacKeyId ids[HMAC_KEYRING_CAPACITY];
    size_t count;
} hkrRestoreUndoLog;

/* Restore's field decoders.  flexGetByTypeBig never reads the box's
 * prior state and gives it its own storage for a wide value, so a
 * crafted entry cannot steer a write.  No snapshot field can hold a
 * 96/128-bit value: anything other than the expected type is refused.
 *
 * hkrReadBytes borrows from 'fe', so the result is valid only while the
 * snapshot buffer is.  Borrowing also keeps raw key bytes from being
 * copied to the heap. */
static bool hkrReadU64(const flexEntry *fe, uint64_t *out) {
    databoxBig box;
    flexGetByTypeBig(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return false;
    }
    *out = box.data.u64;
    return true;
}

static bool hkrReadBytes(const flexEntry *fe, const uint8_t **start,
                         size_t *len) {
    databoxBig box;
    flexGetByTypeBig(fe, &box);
    if (box.type != DATABOX_BYTES) {
        return false;
    }
    *start = box.data.bytes.start;
    *len = box.len;
    return true;
}

bool hmacKeyringRestore(hmacKeyring *ring, const uint8_t *buf, size_t len) {
    if (ring == nullptr || buf == nullptr) {
        return false;
    }
    /* The bytes may come from disk or a peer: bound every entry, not
     * just the header, before walking them below */
    if (!flexValidateBufferDeep(buf, len)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    const size_t count = flexCount(f);
    /* [floor, then whole slot rows] */
    if (count < 1 || ((count - 1) % HMAC_KEYRING_ENTRIES_PER_SLOT) != 0u) {
        return false;
    }
    const size_t records = (count - 1) / HMAC_KEYRING_ENTRIES_PER_SLOT;
    hkrRestoreUndoLog undo = {};

    /* The floor applies after every slot has installed (a rollback then
     * only retires installed ids) */
    flexEntry *fe = flexHead(f);
    uint64_t floor_id = 0;
    if (!hkrReadU64(fe, &floor_id) || floor_id > UINT32_MAX) {
        return false;
    }
    const hmacKeyId parsed_floor = (hmacKeyId)floor_id;
    fe = flexNext(f, fe);

    for (size_t i = 0; i < records; i++) {
        if (fe == nullptr) {
            goto rollback;
        }

        /* Entry 1: id (unsigned). */
        uint64_t raw_id = 0;
        if (!hkrReadU64(fe, &raw_id) || raw_id > UINT32_MAX || raw_id == 0) {
            goto rollback;
        }
        const hmacKeyId id = (hmacKeyId)raw_id;
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            goto rollback;
        }

        /* Entry 2: algo tag (unsigned). */
        uint64_t tag = 0;
        if (!hkrReadU64(fe, &tag)) {
            goto rollback;
        }
        const hmacHashAlgo *algo = hkrTagToAlgo(tag);
        if (algo == nullptr) {
            goto rollback;
        }
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            goto rollback;
        }

        /* Entry 3: salt (bytes, exactly 4). */
        const uint8_t *salt_bytes = nullptr;
        size_t salt_len = 0;
        if (!hkrReadBytes(fe, &salt_bytes, &salt_len) || salt_len != 4) {
            goto rollback;
        }
        uint8_t salt[4];
        memcpy(salt, salt_bytes, 4);
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            goto rollback;
        }

        /* Entry 4: counter (unsigned). */
        uint64_t counter = 0;
        if (!hkrReadU64(fe, &counter)) {
            goto rollback;
        }
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            goto rollback;
        }

        /* Entry 5: key bytes. */
        const uint8_t *key_bytes = nullptr;
        size_t key_len = 0;
        if (!hkrReadBytes(fe, &key_bytes, &key_len) || key_len == 0 ||
            key_len > HMAC_KEYRING_MAX_KEY_BYTES) {
            /* Reject oversized keys explicitly. A tampered snapshot with
             * key_len in the GiB range would otherwise drive a
             * downstream Install allocation. The cap is the same one
             * the in-memory Install path enforces, so this is symmetric
             * not redundant. */
            goto rollback;
        }
        /* The salt says how the slot was installed: a salted install
         * refuses an all-zero salt, so an all-zero salt is exactly an
         * unsalted slot */
        bool installed;
        if (hmacKeyringSaltIsZero(salt)) {
            installed =
                hmacKeyringInstallUnsalted(ring, id, algo, key_bytes, key_len);
        } else {
            installed =
                hmacKeyringInstall(ring, id, algo, key_bytes, key_len, salt);
        }
        if (!installed) {
            goto rollback;
        }
        /* Raised directly, not through RestoreAeadCounter: that refuses
         * UINT64_MAX, and an exhausted key (or a reservation that
         * saturated) must come back exhausted, not at 0. */
        hkrApplyReservation(ring, id, counter);
        undo.ids[undo.count++] = id;
        fe = flexNext(f, fe);
    }
    /* Apply the parsed floor only after every slot has installed
     * cleanly. Done under the write lock directly (not via
     * hmacKeyringSetMinValidKeyId) because the latter refuses backward
     * moves — but a fresh keyring's floor starts at zero so any non-zero
     * persisted floor is by definition a forward move. Be explicit
     * about that via the helper rather than a raw assignment so the
     * monotonic invariant has a single owner. */
    if (parsed_floor != HMAC_KEY_ID_INVALID) {
        if (!hmacKeyringSetMinValidKeyId(ring, parsed_floor)) {
            /* Only reachable if a concurrent caller already raised the
             * floor above the persisted value — possible on a hot
             * restore over a non-empty keyring. Treat as malformed
             * restore: rollback so the keyring is left exactly as it
             * was before this call. */
            goto rollback;
        }
    }
    return true;

rollback:
    for (size_t i = 0; i < undo.count; i++) {
        hmacKeyringRetire(ring, undo.ids[i]);
    }
    return false;
}

/* ---- persistOps descriptor ----------------------------------------- */

static uint8_t *hkrPersistSnapshot(const void *structure, size_t *len) {
    const hmacKeyring *ring = (const hmacKeyring *)structure;
    const size_t need = hmacKeyringSnapshotSize(ring);
    if (need == 0) {
        *len = 0;
        return nullptr;
    }
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(need);
    size_t written = 0;
    if (!hmacKeyringSnapshot(ring, buf, need, &written)) {
        return nullptr;
    }
    *len = written;
    uint8_t *out = buf;
    buf = nullptr;
    return out;
}

static void *hkrPersistRestore(const uint8_t *data, size_t len) {
    hmacKeyring *ring = hmacKeyringCreate();
    if (ring == nullptr) {
        return nullptr;
    }
    if (!hmacKeyringRestore(ring, data, len)) {
        hmacKeyringFree(ring);
        return nullptr;
    }
    return ring;
}

static void *hkrPersistNewEmpty(void) {
    return hmacKeyringCreate();
}

static size_t hkrPersistCount(const void *structure) {
    return hmacKeyringSize((const hmacKeyring *)structure);
}

static size_t hkrPersistEstimateSize(const void *structure) {
    return hmacKeyringSnapshotSize((const hmacKeyring *)structure);
}

static void hkrPersistFree(void *structure) {
    hmacKeyringFree((hmacKeyring *)structure);
}

/* The one WAL op: a nonce reservation [id, limit]. */
static uint8_t *hkrPersistEncodeOp(persistOp op, const void *args, size_t argc,
                                   size_t *len) {
    *len = 0;
    if (op != PERSIST_OP_UPDATE || argc != 2) {
        return nullptr;
    }
    return persistOpEncodeBoxes(args, argc, len);
}

static bool hkrPersistApplyOp(void *structure, persistOp op,
                              const uint8_t *data, size_t len) {
    hmacKeyring *ring = *(hmacKeyring **)structure;
    databoxBig id;
    databoxBig limit;
    if (op != PERSIST_OP_UPDATE ||
        !flexGetTwoDataboxesBig(data, len, &id, &limit) ||
        id.type != DATABOX_UNSIGNED_64 || id.data.u64 == 0 ||
        id.data.u64 > UINT32_MAX || limit.type != DATABOX_UNSIGNED_64) {
        return false;
    }
    hkrApplyReservation(ring, (hmacKeyId)id.data.u64, limit.data.u64);
    return true;
}

const persistOps persistOpsHmacKeyring = {
    .type = PERSIST_TYPE_HMAC_KEYRING,
    .name = "hmacKeyring",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = hkrPersistSnapshot,
    .restore = hkrPersistRestore,
    .newEmpty = hkrPersistNewEmpty,
    .count = hkrPersistCount,
    .estimateSize = hkrPersistEstimateSize,
    /* Installs, retires, salts and the floor are operator-rare and made
     * durable by a new snapshot; the WAL carries nonce reservations. */
    .encodeOp = hkrPersistEncodeOp,
    .applyOp = hkrPersistApplyOp,
    .streamSnapshot = nullptr,
    .streamRestore = nullptr,
    .validate = nullptr,
    .free = hkrPersistFree,
};

/* ============================================================================
 * Test driver
 * ============================================================================
 */

#ifdef DATAKIT_TEST

#include "ctest.h"

defcleaner(hmacKeyring *, hmacKeyringFree);

/* Worker context for the multi-threaded contention test. Each worker
 * runs `iters` lookups against a shared keyring; one designated writer
 * thread interleaves install/retire to keep the slot churning. */
typedef struct {
    hmacKeyring *ring;
    uint32_t iters;
    uint32_t lookup_misses;
    uint32_t digest_mismatches;
    bool is_writer;
    uint8_t _pad[3];
} hmacKeyringMtCtx;

/* Worker for the concurrent-nonce uniqueness test. Each worker drains
 * `iters` nonces against id=21 and stores their counter value into `out`. */
typedef struct hmacKeyringNonceWorkerCtx {
    hmacKeyring *ring;
    uint64_t *out;
    uint32_t iters;
    hmacKeyId id;
} hmacKeyringNonceWorkerCtx;

static void *hmacKeyringNonceWorker(void *arg) {
    hmacKeyringNonceWorkerCtx *c = (hmacKeyringNonceWorkerCtx *)arg;
    for (uint32_t i = 0; i < c->iters; i++) {
        uint8_t n[12];
        if (!hmacKeyringNextAeadNonce(c->ring, c->id, n)) {
            c->out[i] = UINT64_MAX;
            continue;
        }
        uint64_t v = 0;
        for (int32_t j = 7; j >= 0; j--) {
            v = (v << 8) | (uint64_t)n[(uint32_t)j];
        }
        c->out[i] = v;
    }
    return nullptr;
}

static void *hmacKeyringMtWorker(void *arg) {
    hmacKeyringMtCtx *ctx = (hmacKeyringMtCtx *)arg;
    const uint8_t key[32] = {0xA1, 0xB2, 0xC3, 0xD4};
    if (ctx->is_writer) {
        /* The writer rapidly installs and retires id=100, id=101, id=102
         * to exercise the install/retire path concurrently with
         * lookups against id=1..16 (installed by the harness). */
        for (uint32_t i = 0; i < ctx->iters; i++) {
            const hmacKeyId tmp_id = 100u + (i % 3u);
            /* Worker exercises install/retire concurrency; no AEAD nonces
             * issued, so InstallUnsalted is the lighter-weight choice. */
            if (hmacKeyringInstallUnsalted(ctx->ring, tmp_id, &hmacAlgoSHA256,
                                           key, sizeof(key))) {
                hmacKeyringRetire(ctx->ring, tmp_id);
            }
        }
        return nullptr;
    }
    /* Readers loop over ids 1..16 (pre-installed) and verify every
     * lookup either returns a handle that produces the expected digest
     * or returns NULL (which would be a regression — these ids are not
     * retired). */
    uint8_t expected[32];
    hmacCompute(&hmacAlgoSHA256, key, sizeof(key), "ping", 4, expected);
    for (uint32_t i = 0; i < ctx->iters; i++) {
        const hmacKeyId id = 1u + (i % 16u);
        const hmacKey *handle = hmacKeyringLookup(ctx->ring, id);
        if (handle == nullptr) {
            ctx->lookup_misses++;
            continue;
        }
        uint8_t observed[32];
        hmacComputeWithKey(handle, "ping", 4, observed);
        if (memcmp(expected, observed, sizeof(expected)) != 0) {
            ctx->digest_mismatches++;
        }
    }
    return nullptr;
}

#include <pthread.h>
#include <stdlib.h> /* qsort */

static int hkrTestCmpU64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* The counter a nonce carries (bytes 0..7, little-endian). */
static uint64_t hkrTestNonceCounter(const uint8_t n[12]) {
    uint64_t v = 0;
    for (int32_t j = 7; j >= 0; j--) {
        v = (v << 8) | (uint64_t)n[(uint32_t)j];
    }
    return v;
}

/* A reservation function for the tests: counts calls, keeps the last
 * limit, refuses while `refuse` is set, and can act on the ring from
 * inside the reservation once.  Calls are serialized by the ring's
 * reservation lock; the test reads the fields after its threads join. */
typedef enum hkrTestReserveAction {
    HKR_TEST_RESERVE_NONE,
    HKR_TEST_RESERVE_SNAPSHOT, /* snapshot the ring while reserving */
    HKR_TEST_RESERVE_REPLACE,  /* retire and reinstall the key meanwhile */
} hkrTestReserveAction;

typedef struct hkrTestReserve {
    hmacKeyring *ring;
    uint64_t calls;
    uint64_t lastLimit;
    bool refuse;
    hkrTestReserveAction action;
    uint8_t snap[512];
    size_t snapLen;
} hkrTestReserve;

static bool hkrTestReserveFn(void *ctx, hmacKeyId id, uint64_t limit) {
    hkrTestReserve *r = ctx;
    r->calls++;
    if (r->refuse) {
        return false;
    }
    r->lastLimit = limit;
    const hkrTestReserveAction action = r->action;
    r->action = HKR_TEST_RESERVE_NONE;
    if (action == HKR_TEST_RESERVE_SNAPSHOT) {
        if (!hmacKeyringSnapshot(r->ring, r->snap, sizeof(r->snap),
                                 &r->snapLen)) {
            r->snapLen = 0;
        }
    } else if (action == HKR_TEST_RESERVE_REPLACE) {
        const uint8_t key[32] = {0x5A};
        hmacKeyringRetire(r->ring, id);
        hmacKeyringInstallUnsalted(r->ring, id, &hmacAlgoSHA256, key,
                                   sizeof(key));
    }
    return true;
}

/* Fill the stack below the caller with 0xFF so a decoder that reads an
 * uninitialized box sees big = 1 and an all-ones pointer.  ASan is off
 * here so the array is not framed by redzones, which would leave the
 * top of the next frame unpoisoned. */
[[gnu::noinline, gnu::no_sanitize("address")]] static void
hkrTestPoisonStack(void) {
    volatile uint8_t junk[8192];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xFF;
    }
}

/* Copy of 'src' with entry 'idx' replaced by 'wide' (a SIZE_MAX index
 * copies every entry unchanged). */
static flex *hkrTestReplaceEntry(const flex *src, size_t idx,
                                 const databoxBig *wide) {
    /* @own: caller; free=flexFree */
    flex *out = flexNew();
    size_t i = 0;
    for (flexEntry *fe = flexHeadIfAny(src); fe != nullptr;
         fe = flexNext(src, fe)) {
        databoxBig box;
        flexGetByTypeBig(fe, &box);
        const databox *push =
            (i == idx) ? (const databox *)wide : (const databox *)&box;
        flexPushByType(&out, push, FLEX_ENDPOINT_TAIL);
        i++;
    }
    return out;
}

int hmacKeyringTest(int, char *[]) {
    int err = 0;

    TEST("Restore of every truncation and byte flip: all or nothing") {
        /* A ring of four keys (two algorithms, a salted one, an advanced
         * AEAD counter) is snapshotted; each damaged copy is restored into
         * a ring holding another key: a refusal leaves that ring's
         * snapshot byte-identical, and an accepted copy gives a ring whose
         * snapshot restores again to the same bytes */
        hmacKeyring *src = hmacKeyringCreate();
        uint8_t key[32];
        for (size_t i = 0; i < sizeof(key); i++) {
            key[i] = (uint8_t)(i * 9 + 4);
        }
        hmacKeyringInstallUnsalted(src, 11, &hmacAlgoSHA256, key, 32);
        hmacKeyringInstallUnsalted(src, 12, &hmacAlgoBLAKE3, key, 32);
        hmacKeyringInstallUnsalted(src, 13, &hmacAlgoSHA256, key, 16);
        hmacKeyringInstallUnsalted(src, 14, &hmacAlgoSHA256, key + 8, 24);
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {1, 2, 3, 4};
        hmacKeyringSetAeadSalt(src, 14, salt);
        (void)hmacKeyringRestoreAeadCounter(src, 11, 1000);
        const size_t cap = hmacKeyringSnapshotSize(src);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(cap);
        cleanerName(zfree, Bytes) uint8_t *before = zmalloc(cap + 256);
        cleanerName(zfree, Bytes) uint8_t *after = zmalloc(cap * 2 + 256);
        size_t len = 0;
        if (!hmacKeyringSnapshot(src, snap, cap, &len)) {
            ERR("damage sweep: snapshot failed");
        }

        size_t changedOnRefusal = 0;
        size_t unstable = 0;
        for (size_t step = 0; step < 2 * len; step++) {
            const bool flip = step >= len;
            const size_t at = flip ? step - len : step;
            hmacKeyring *dst = hmacKeyringCreate();
            hmacKeyringInstallUnsalted(dst, 900, &hmacAlgoSHA256, key, 32);
            size_t beforeLen = 0;
            hmacKeyringSnapshot(dst, before, cap + 256, &beforeLen);
            if (flip) {
                snap[at] ^= 0x01;
            }
            const bool ok = hmacKeyringRestore(dst, snap, flip ? len : at);
            if (flip) {
                snap[at] ^= 0x01;
            }
            size_t afterLen = 0;
            const bool snapOk =
                hmacKeyringSnapshot(dst, after, cap * 2 + 256, &afterLen);
            if (!ok && (!snapOk || afterLen != beforeLen ||
                        memcmp(before, after, beforeLen) != 0)) {
                changedOnRefusal++;
            }
            if (ok) {
                hmacKeyring *again = hmacKeyringCreate();
                size_t againLen = 0;
                const bool reOk =
                    snapOk && hmacKeyringRestore(again, after, afterLen) &&
                    hmacKeyringSnapshot(again, after + afterLen,
                                        cap * 2 + 256 - afterLen, &againLen);
                if (!reOk || againLen != afterLen ||
                    memcmp(after, after + afterLen, afterLen) != 0) {
                    unstable++;
                }
                hmacKeyringFree(again);
            }
            hmacKeyringFree(dst);
        }
        if (changedOnRefusal || unstable) {
            ERR("damage sweep: %zu refusals changed the ring, %zu accepted "
                "restores were unstable",
                changedOnRefusal, unstable);
        }
        hmacKeyringFree(src);
    }

    TEST("snapshot/restore: salts and keys that spell numbers stay bytes") {
        /* A 4-byte random salt is all ASCII digits about once in 400 K
         * installs, and a caller's key may be digits; the snapshot must
         * store both exactly or Restore refuses them as non-bytes. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {'1', '2', '3', '4', '5', '6', '7', '8',
                                 '9', '0', '1', '2', '3', '4', '5', '6'};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {'1', '2', '3', '4'};
        if (!hmacKeyringInstall(ring, 3u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERR("install failed");
        }

        const size_t sz = hmacKeyringSnapshotSize(ring);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz ? sz : 1);
        size_t written = 0;
        hmacKeyring *dst = hmacKeyringCreate();
        uint8_t got[sizeof(key)] = {};
        size_t gotLen = 0;
        if (!hmacKeyringSnapshot(ring, buf, sz, &written) ||
            !hmacKeyringRestore(dst, buf, written)) {
            ERR("a keyring with a digit salt and key did not restore");
        } else if (!hmacKeyringGetRawCopy(dst, 3u, got, sizeof(got), &gotLen,
                                          nullptr) ||
                   gotLen != sizeof(key) ||
                   memcmp(got, key, sizeof(key)) != 0) {
            ERR("the restored key differs");
        }

        hmacKeyringFree(dst);
        hmacKeyringFree(ring);
    }

    TEST("snapshot/restore: a 96/128-bit value in any field is refused") {
        /* A floor plus a salted and an unsalted slot, so every field
         * Restore decodes is covered: 11 entries. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {0x5A, 0x5A, 0x5A, 0x01};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x0A, 0x0B, 0x0C,
                                                            0x0D};
        hmacKeyringSetMinValidKeyId(ring, 5u);
        hmacKeyringInstall(ring, 10u, &hmacAlgoSHA256, key, sizeof(key), salt);
        hmacKeyringInstallUnsalted(ring, 11u, &hmacAlgoBLAKE3, key,
                                   sizeof(key));
        hmacKeyringRestoreAeadCounter(ring, 10u, 77u);
        const size_t sz = hmacKeyringSnapshotSize(ring);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!hmacKeyringSnapshot(ring, buf, sz, &written)) {
            ERR("snapshot failed");
        }
        hmacKeyringFree(ring);
        const flex *src = (const flex *)buf;
        const size_t entries = flexCount(src);
        if (entries != 1u + 2u * HMAC_KEYRING_ENTRIES_PER_SLOT) {
            ERR("snapshot has %zu entries, expected 11", entries);
        }

        databoxBig wides[3];
        DATABOX_BIG_UNSIGNED_128(&wides[0], (__uint128_t)1 << 64);
        DATABOX_BIG_UNSIGNED_128(&wides[1], ((__uint128_t)1 << 127) | 1u);
        DATABOX_BIG_SIGNED_128(&wides[2], -((__int128_t)1 << 100));

        /* The rebuild itself must restore, so a refusal below is due to
         * the replaced field alone. */
        {
            cleaner(flexFree) flex *same =
                hkrTestReplaceEntry(src, SIZE_MAX, nullptr);
            hmacKeyring *dst = hmacKeyringCreate();
            hkrTestPoisonStack();
            if (!hmacKeyringRestore(dst, same, flexBytes(same)) ||
                hmacKeyringSize(dst) != 2u ||
                hmacKeyringMinValidKeyId(dst) != 5u) {
                ERR("unmodified rebuild did not restore");
            }
            hmacKeyringFree(dst);
        }

        for (size_t w = 0; w < 3; w++) {
            for (size_t idx = 0; idx < entries; idx++) {
                cleaner(flexFree) flex *bad =
                    hkrTestReplaceEntry(src, idx, &wides[w]);
                databoxBig check;
                flexGetByTypeBig(flexIndex(bad, (int32_t)idx), &check);
                if (!check.big) {
                    ERR("value %zu at entry %zu was not stored wide", w, idx);
                }
                hmacKeyring *dst = hmacKeyringCreate();
                hkrTestPoisonStack();
                if (hmacKeyringRestore(dst, bad, flexBytes(bad))) {
                    ERR("value %zu at entry %zu was accepted", w, idx);
                }
                /* Refusal rolls back: nothing installed, no floor. */
                if (hmacKeyringSize(dst) != 0u ||
                    hmacKeyringMinValidKeyId(dst) != HMAC_KEY_ID_INVALID) {
                    ERR("value %zu at entry %zu left partial state", w, idx);
                }
                hmacKeyringFree(dst);
            }
        }
    }

    TEST("create + free roundtrip on empty keyring") {
        hmacKeyring *ring = hmacKeyringCreate();
        if (ring == nullptr) {
            ERR("Create returned NULL");
        }
        if (hmacKeyringSize(ring) != 0) {
            ERR("Empty keyring should report size 0");
        }
        hmacKeyringFree(ring);
    }

    TEST("every id-taking call refuses an invalid or absent id") {
        /* Beside an installed key 3 (32 bytes, salted): ids 0 (invalid)
         * and 4 (never installed) are refused by every lookup, copy,
         * nonce and salt call, and key 3 is left exactly as it was */
        cleaner(hmacKeyringFree) hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x31, 0x41, 0x59};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {1, 2, 3, 4};
        uint64_t gen = 0;
        uint64_t counter = 0;
        if (!hmacKeyringInstall(ring, 3u, &hmacAlgoSHA256, key, sizeof(key),
                                salt) ||
            !hmacKeyringGetGeneration(ring, 3u, &gen) ||
            !hmacKeyringPeekAeadCounter(ring, 3u, &counter)) {
            ERR("key 3 setup failed");
        }
        static const hmacKeyId ids[] = {HMAC_KEY_ID_INVALID, 4u};
        for (size_t i = 0; i < countof(ids); i++) {
            const hmacKeyId id = ids[i];
            const uint8_t *bytes = nullptr;
            size_t len = 0;
            uint8_t buf[64];
            uint64_t v = 0;
            uint8_t nonce[12];
            aesGcmKey schedule;
            const int accepted =
                hmacKeyringRetire(ring, id) +
                (hmacKeyringLookup(ring, id) != nullptr) +
                hmacKeyringGetRaw(ring, id, &bytes, &len, nullptr) +
                hmacKeyringGetRawCopy(ring, id, buf, sizeof(buf), &len,
                                      nullptr) +
                hmacKeyringGetGeneration(ring, id, &v) +
                (hmacKeyringGetAesGcmKey(ring, id) != nullptr) +
                hmacKeyringGetAesGcmKeyCopy(ring, id, &schedule) +
                hmacKeyringNextAeadNonce(ring, id, nonce) +
                hmacKeyringSetAeadSalt(ring, id, salt) +
                hmacKeyringPeekAeadCounter(ring, id, &v) +
                hmacKeyringRestoreAeadCounter(ring, id, 99);
            if (accepted) {
                ERR("id %u: %d calls accepted", (unsigned)id, accepted);
            }
        }
        uint64_t genAfter = 0;
        uint64_t counterAfter = 0;
        if (hmacKeyringSize(ring) != 1 ||
            !hmacKeyringGetGeneration(ring, 3u, &genAfter) || genAfter != gen ||
            !hmacKeyringPeekAeadCounter(ring, 3u, &counterAfter) ||
            counterAfter != counter) {
            ERR("refused calls changed key 3");
        }
    }

    TEST("install + lookup + retire one key") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x11, 0x22, 0x33, 0x44};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xC1, 0xA1, 0xC1,
                                                            0xA1};
        if (!hmacKeyringInstall(ring, 7u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERR("Install of id=7 should succeed");
        }
        if (hmacKeyringSize(ring) != 1) {
            ERR("Size should be 1 after install");
        }
        const hmacKey *handle = hmacKeyringLookup(ring, 7u);
        if (handle == nullptr) {
            ERR("Lookup of installed id should return a handle");
        }
        /* Round-trip the prepared key through hmacComputeWithKey and
         * verify it matches a fresh one-shot computation. */
        uint8_t expected[32], observed[32];
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), "abc", 3, expected);
        hmacComputeWithKey(handle, "abc", 3, observed);
        TEST_BYTES(err, expected, observed, sizeof(expected),
                   "Prepared key disagreed with fresh hmacCompute");
        if (!hmacKeyringRetire(ring, 7u)) {
            ERR("Retire of id=7 should succeed");
        }
        if (hmacKeyringLookup(ring, 7u) != nullptr) {
            ERR("Retired id should no longer look up");
        }
        if (hmacKeyringSize(ring) != 0) {
            ERR("Size should be 0 after retire");
        }
        hmacKeyringFree(ring);
    }

    TEST("install refuses invalid id and duplicate id") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {0xAA};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x12, 0x34, 0x56,
                                                            0x78};
        if (hmacKeyringInstall(ring, HMAC_KEY_ID_INVALID, &hmacAlgoSHA256, key,
                               sizeof(key), salt)) {
            ERR("Install with id=0 must be refused");
        }
        if (!hmacKeyringInstall(ring, 1u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERR("Install of id=1 should succeed");
        }
        if (hmacKeyringInstall(ring, 1u, &hmacAlgoSHA256, key, sizeof(key),
                               salt)) {
            ERR("Duplicate install of id=1 must be refused");
        }
        hmacKeyringFree(ring);
    }

    TEST("salted install refuses a NULL or all-zero salt, and reports it") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {0xAA};
        const uint8_t zero[HMAC_KEYRING_AEAD_SALT_BYTES] = {};
        const uint64_t diagBefore = dkDiagCount();
        if (hmacKeyringInstall(ring, 2u, &hmacAlgoSHA256, key, sizeof(key),
                               nullptr) ||
            hmacKeyringInstall(ring, 2u, &hmacAlgoSHA256, key, sizeof(key),
                               zero)) {
            ERR("a NULL or all-zero salt must be refused");
        }
        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 2 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_HMAC_KEYRING_ZERO_SALT) {
            ERR("each refused salt must be reported once");
        }
        /* The unsalted entry point is the sanctioned way: no report */
        const uint64_t diagUnsalted = dkDiagCount();
        if (!hmacKeyringInstallUnsalted(ring, 2u, &hmacAlgoSHA256, key,
                                        sizeof(key)) ||
            dkDiagCount() != diagUnsalted) {
            ERR("an unsalted install must succeed without a report");
        }
        hmacKeyringFree(ring);
    }

    TEST("multiple algorithms coexist") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0xC0, 0xDE};
        const uint8_t salt_a[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x0A, 0x0B, 0x0C,
                                                              0x0D};
        const uint8_t salt_b[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x10, 0x11, 0x12,
                                                              0x13};
        if (!hmacKeyringInstall(ring, 10u, &hmacAlgoSHA256, key, 32, salt_a)) {
            ERR("Install SHA256 id=10");
        }
        if (!hmacKeyringInstall(ring, 11u, &hmacAlgoBLAKE3, key, 32, salt_b)) {
            ERR("Install BLAKE3 id=11");
        }
        if (hmacKeyringSize(ring) != 2) {
            ERR("Expected size=2, got %zu", hmacKeyringSize(ring));
        }
        const hmacKey *h_sha = hmacKeyringLookup(ring, 10u);
        const hmacKey *h_b3 = hmacKeyringLookup(ring, 11u);
        if (h_sha == nullptr || h_b3 == nullptr) {
            ERR("Both lookups should succeed");
        }
        /* Both must produce different digests on the same input — they
         * are different MAC constructions. */
        uint8_t out_sha[32], out_b3[32];
        hmacComputeWithKey(h_sha, "hello", 5, out_sha);
        hmacComputeWithKey(h_b3, "hello", 5, out_b3);
        if (memcmp(out_sha, out_b3, 32) == 0) {
            ERR("HMAC-SHA256 and HMAC-BLAKE3 must produce different output");
        }
        hmacKeyringFree(ring);
    }

    TEST("install fills capacity and refuses beyond") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {0x5A};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x5A, 0x5A, 0x5A,
                                                            0x5A};
        for (uint32_t i = 1u; i <= 16u; i++) {
            if (!hmacKeyringInstall(ring, i, &hmacAlgoSHA256, key, 16, salt)) {
                ERR("Install %u of 16 should succeed", i);
            }
        }
        if (hmacKeyringInstall(ring, 17u, &hmacAlgoSHA256, key, 16, salt)) {
            ERR("17th install must be refused (capacity)");
        }
        /* Retire one and re-install should now succeed. */
        if (!hmacKeyringRetire(ring, 5u)) {
            ERR("Retire id=5");
        }
        if (!hmacKeyringInstall(ring, 17u, &hmacAlgoSHA256, key, 16, salt)) {
            ERR("Re-install after retire should succeed");
        }
        hmacKeyringFree(ring);
    }

    TEST("concurrent lookups under installer churn stay consistent") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0xA1, 0xB2, 0xC3, 0xD4};
        /* Pre-install ids 1..16; readers lookup these. */
        for (uint32_t i = 1u; i <= 16u; i++) {
            /* Test only exercises lookup/install/retire, not nonce issuance,
             * so InstallUnsalted is the lighter-weight choice. */
            if (!hmacKeyringInstallUnsalted(ring, i, &hmacAlgoSHA256, key,
                                            sizeof(key))) {
                ERR("Pre-install id=%u failed", i);
            }
        }
        /* Capacity is full; the writer must retire-then-install
         * a temporary id (handled inside the worker by retiring its
         * own previous insert). Drop one slot first so the writer
         * has room. */
        if (!hmacKeyringRetire(ring, 16u)) {
            ERR("Pre-retire id=16 failed");
        }

        const uint32_t kReaders = 7u;
        const uint32_t kIters = 4096u;
        hmacKeyringMtCtx ctxs[8];
        pthread_t tids[8];
        for (uint32_t i = 0; i < kReaders; i++) {
            ctxs[i] = (hmacKeyringMtCtx){.ring = ring,
                                         .iters = kIters,
                                         .lookup_misses = 0,
                                         .digest_mismatches = 0,
                                         .is_writer = false};
        }
        ctxs[kReaders] = (hmacKeyringMtCtx){.ring = ring,
                                            .iters = kIters,
                                            .lookup_misses = 0,
                                            .digest_mismatches = 0,
                                            .is_writer = true};
        for (uint32_t i = 0; i <= kReaders; i++) {
            if (pthread_create(&tids[i], nullptr, hmacKeyringMtWorker,
                               &ctxs[i]) != 0) {
                ERR("pthread_create thread %u", i);
            }
        }
        for (uint32_t i = 0; i <= kReaders; i++) {
            pthread_join(tids[i], nullptr);
        }
        for (uint32_t i = 0; i < kReaders; i++) {
            if (ctxs[i].digest_mismatches > 0) {
                ERR("reader %u observed %u digest mismatches", i,
                    ctxs[i].digest_mismatches);
            }
            /* Reader iterates over ids 1..16; id 16 was retired before
             * spawning threads, so ~1/16 of its lookups will miss.
             * That gives an expected miss count of kIters/16; double
             * it to kIters/8 as the failure threshold to absorb noise
             * from the concurrent installer churn on ids 100..102. */
            const uint32_t expected_misses = kIters / 16u;
            const uint32_t miss_threshold = kIters / 8u;
            if (ctxs[i].lookup_misses > miss_threshold) {
                ERR("reader %u observed %u misses "
                    "(expected ≈%u, threshold %u)",
                    i, ctxs[i].lookup_misses, expected_misses, miss_threshold);
            }
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD nonce counter: starts at 0, monotonic per call") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0xAA};
        /* Test asserts nonce[8..11] == 0; use the Unsalted entry point so
         * NextAeadNonce will issue zero-salt nonces under the new
         * fail-closed contract. */
        hmacKeyringInstallUnsalted(ring, 7u, &hmacAlgoSHA256, key, sizeof(key));
        uint8_t n0[12], n1[12], n2[12];
        if (!hmacKeyringNextAeadNonce(ring, 7u, n0)) {
            ERR("NextAeadNonce 0 should succeed");
        }
        if (!hmacKeyringNextAeadNonce(ring, 7u, n1)) {
            ERR("NextAeadNonce 1 should succeed");
        }
        if (!hmacKeyringNextAeadNonce(ring, 7u, n2)) {
            ERR("NextAeadNonce 2 should succeed");
        }
        /* Counter low byte should be 0, 1, 2; remaining bytes all zero
         * (no salt installed). */
        const uint8_t expect0[12] = {};
        uint8_t expect1[12] = {};
        expect1[0] = 1u;
        uint8_t expect2[12] = {};
        expect2[0] = 2u;
        TEST_BYTES(err, n0, expect0, 12, "nonce0 layout");
        TEST_BYTES(err, n1, expect1, 12, "nonce1 layout");
        TEST_BYTES(err, n2, expect2, 12, "nonce2 layout");
        uint64_t peek = 0;
        if (!hmacKeyringPeekAeadCounter(ring, 7u, &peek)) {
            ERR("Peek should succeed");
        }
        if (peek != 3u) {
            ERR("Peek expected 3, got %" PRIu64, peek);
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD nonce salt: prefixed correctly, counter unaffected") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x11};
        const uint8_t initial_salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x01, 0x02,
                                                                    0x03, 0x04};
        hmacKeyringInstall(ring, 9u, &hmacAlgoSHA256, key, sizeof(key),
                           initial_salt);
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xDE, 0xAD, 0xBE,
                                                            0xEF};
        if (!hmacKeyringSetAeadSalt(ring, 9u, salt)) {
            ERR("SetAeadSalt should succeed");
        }
        uint8_t n[12];
        if (!hmacKeyringNextAeadNonce(ring, 9u, n)) {
            ERR("NextAeadNonce");
        }
        /* Counter LE in bytes 0..7, salt in bytes 8..11. */
        if (n[0] != 0u || n[1] != 0u) {
            ERR("counter low bytes");
        }
        if (n[8] != 0xDE || n[9] != 0xAD || n[10] != 0xBE || n[11] != 0xEF) {
            ERR("salt bytes not propagated to nonce");
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD counter restore: forward ok, backward refused") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x22};
        /* Test calls NextAeadNonce after RestoreAeadCounter without
         * setting a salt; use Unsalted to bypass the fail-closed check. */
        hmacKeyringInstallUnsalted(ring, 11u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        if (!hmacKeyringRestoreAeadCounter(ring, 11u, 1000u)) {
            ERR("Restore to 1000 should succeed");
        }
        uint8_t n[12];
        if (!hmacKeyringNextAeadNonce(ring, 11u, n)) {
            ERR("NextAeadNonce");
        }
        if (n[0] != (uint8_t)(1000u & 0xFFu) ||
            n[1] != (uint8_t)((1000u >> 8) & 0xFFu)) {
            ERR("counter did not pick up restored value");
        }
        /* Now at 1001; restore to 1500 forward — ok. */
        if (!hmacKeyringRestoreAeadCounter(ring, 11u, 1500u)) {
            ERR("Forward restore should succeed");
        }
        /* Restore to 500 backward — refused. */
        if (hmacKeyringRestoreAeadCounter(ring, 11u, 500u)) {
            ERR("Backward restore must be refused");
        }
        /* Restore to UINT64_MAX — refused (would exhaust). */
        if (hmacKeyringRestoreAeadCounter(ring, 11u, UINT64_MAX)) {
            ERR("Restore to UINT64_MAX must be refused");
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD counter exhaustion: refuses past UINT64_MAX") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x33};
        hmacKeyringInstallUnsalted(ring, 13u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        if (!hmacKeyringRestoreAeadCounter(ring, 13u, UINT64_MAX - 1u)) {
            ERR("Restore to UINT64_MAX-1 should succeed");
        }
        uint8_t n[12];
        if (!hmacKeyringNextAeadNonce(ring, 13u, n)) {
            ERR("Last NextAeadNonce should succeed");
        }
        /* Counter now == UINT64_MAX; next call must refuse. */
        if (hmacKeyringNextAeadNonce(ring, 13u, n)) {
            ERR("Past-exhaustion NextAeadNonce must refuse");
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD counter: concurrent NextAeadNonce yields unique values") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x44};
        /* Workers sort by counter prefix only — install Unsalted so the
         * zero suffix is permitted under the fail-closed contract. */
        hmacKeyringInstallUnsalted(ring, 21u, &hmacAlgoSHA256, key,
                                   sizeof(key));

        const uint32_t kThreads = 8u;
        const uint32_t kIters = 4096u;
        const size_t total = (size_t)kThreads * (size_t)kIters;
        cleanerName(zfree, U64) uint64_t *collected =
            zmalloc(total * sizeof(uint64_t));
        hmacKeyringNonceWorkerCtx ctxs[8];
        pthread_t tids[8];
        for (uint32_t t = 0; t < kThreads; t++) {
            ctxs[t].ring = ring;
            ctxs[t].iters = kIters;
            ctxs[t].out = collected + (size_t)t * kIters;
            ctxs[t].id = 21u;
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_create(&tids[t], nullptr, hmacKeyringNonceWorker, &ctxs[t]);
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_join(tids[t], nullptr);
        }
        /* Sort and check uniqueness. */
        qsort(collected, total, sizeof(*collected), hkrTestCmpU64);
        for (size_t i = 1; i < total; i++) {
            if (collected[i] == collected[i - 1]) {
                ERR("duplicate nonce at index %zu (value %" PRIu64 ")", i,
                    collected[i]);
                break;
            }
        }
        if (collected[0] != 0u ||
            collected[total - 1] != (uint64_t)(total - 1)) {
            ERR("expected range [0..%zu], got [%" PRIu64 "..%" PRIu64 "]",
                total - 1, collected[0], collected[total - 1]);
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation: issues below a durable limit, one per window") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x31};
        hmacKeyringInstallUnsalted(ring, 31u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hkrTestReserve r = {.ring = ring};
        if (hmacKeyringSetAeadReservation(ring, 0, hkrTestReserveFn, &r) ||
            hmacKeyringSetAeadReservation(nullptr, 4, hkrTestReserveFn, &r)) {
            ERR("a zero window or a NULL ring must be refused");
        }
        if (!hmacKeyringSetAeadReservation(ring, 4, hkrTestReserveFn, &r)) {
            ERR("SetAeadReservation");
        }
        for (uint64_t i = 0; i < 10; i++) {
            uint8_t n[12];
            if (!hmacKeyringNextAeadNonce(ring, 31u, n) ||
                hkrTestNonceCounter(n) != i) {
                ERR("nonce %" PRIu64 " not issued in order", i);
            }
        }
        /* Limits 4, 8, 12: a reservation before nonces 0, 4 and 8 */
        if (r.calls != 3 || r.lastLimit != 12) {
            ERR("expected 3 reservations up to 12, got %" PRIu64
                " up to %" PRIu64,
                r.calls, r.lastLimit);
        }
        /* The counter stays 10; the snapshot starts a restore at 12 */
        uint64_t peek = 0;
        hmacKeyringPeekAeadCounter(ring, 31u, &peek);
        uint8_t snap[256];
        size_t snapLen = 0;
        hmacKeyring *back = hmacKeyringCreate();
        uint64_t restored = 0;
        if (peek != 10 ||
            !hmacKeyringSnapshot(ring, snap, sizeof(snap), &snapLen) ||
            !hmacKeyringRestore(back, snap, snapLen) ||
            !hmacKeyringPeekAeadCounter(back, 31u, &restored) ||
            restored != 12) {
            ERR("counter %" PRIu64 ", restored %" PRIu64 " (want 10, 12)", peek,
                restored);
        }
        hmacKeyringFree(back);
        /* Off: unlimited again, no more reservations */
        hmacKeyringSetAeadReservation(ring, 0, nullptr, nullptr);
        for (int i = 0; i < 20; i++) {
            uint8_t n[12];
            if (!hmacKeyringNextAeadNonce(ring, 31u, n)) {
                ERR("issuance with reservation off");
            }
        }
        if (r.calls != 3) {
            ERR("a reservation ran after it was turned off");
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD counter: an exhausted key restores exhausted") {
        /* Restore once routed the counter through RestoreAeadCounter,
         * which refuses UINT64_MAX, so an exhausted key came back at 0
         * and issued every nonce again. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x37};
        hmacKeyringInstallUnsalted(ring, 37u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hmacKeyringRestoreAeadCounter(ring, 37u, UINT64_MAX - 1u);
        uint8_t n[12];
        hmacKeyringNextAeadNonce(ring, 37u, n); /* the last nonce */
        uint8_t snap[256];
        size_t snapLen = 0;
        hmacKeyring *back = hmacKeyringCreate();
        uint64_t restored = 0;
        if (!hmacKeyringSnapshot(ring, snap, sizeof(snap), &snapLen) ||
            !hmacKeyringRestore(back, snap, snapLen) ||
            !hmacKeyringPeekAeadCounter(back, 37u, &restored) ||
            restored != UINT64_MAX || hmacKeyringNextAeadNonce(back, 37u, n)) {
            ERR("restored counter %" PRIu64 " (want UINT64_MAX, refusing)",
                restored);
        }
        hmacKeyringFree(back);
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation: a refused reservation issues nothing") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x32};
        hmacKeyringInstallUnsalted(ring, 32u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hkrTestReserve r = {.ring = ring, .refuse = true};
        hmacKeyringSetAeadReservation(ring, 8, hkrTestReserveFn, &r);
        uint8_t n[12];
        uint64_t peek = UINT64_MAX;
        if (hmacKeyringNextAeadNonce(ring, 32u, n) ||
            !hmacKeyringPeekAeadCounter(ring, 32u, &peek) || peek != 0) {
            ERR("a refused reservation must fail closed, counter unmoved");
        }
        r.refuse = false;
        if (!hmacKeyringNextAeadNonce(ring, 32u, n) ||
            hkrTestNonceCounter(n) != 0 || r.calls != 2) {
            ERR("after the refusal, the next nonce reserves and is 0");
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation: a snapshot during it restores above its limit") {
        /* The limit is recorded before the function runs, so a snapshot
         * taken while it makes the limit durable already covers it. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x33};
        hmacKeyringInstallUnsalted(ring, 33u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hkrTestReserve r = {.ring = ring, .action = HKR_TEST_RESERVE_SNAPSHOT};
        hmacKeyringSetAeadReservation(ring, 16, hkrTestReserveFn, &r);
        uint8_t n[12];
        hmacKeyring *back = hmacKeyringCreate();
        uint64_t restored = 0;
        if (!hmacKeyringNextAeadNonce(ring, 33u, n) || r.snapLen == 0 ||
            !hmacKeyringRestore(back, r.snap, r.snapLen) ||
            !hmacKeyringPeekAeadCounter(back, 33u, &restored) ||
            restored != 16) {
            ERR("restored at %" PRIu64 ", want the in-flight limit 16",
                restored);
        }
        hmacKeyringFree(back);
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation: a key replaced during it is not given it") {
        /* The function retires and reinstalls the id before confirming:
         * the confirmation belongs to the old key, so the new key
         * reserves for itself before its first nonce. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x34};
        hmacKeyringInstallUnsalted(ring, 34u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hkrTestReserve r = {.ring = ring, .action = HKR_TEST_RESERVE_REPLACE};
        hmacKeyringSetAeadReservation(ring, 8, hkrTestReserveFn, &r);
        uint8_t n[12];
        if (!hmacKeyringNextAeadNonce(ring, 34u, n) ||
            hkrTestNonceCounter(n) != 0 || r.calls != 2) {
            ERR("calls %" PRIu64 " (want 2: the new key reserves again)",
                r.calls);
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation: concurrent issuers, one reservation per window") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x35};
        hmacKeyringInstallUnsalted(ring, 21u, &hmacAlgoSHA256, key,
                                   sizeof(key));
        hkrTestReserve r = {.ring = ring};
        const uint64_t window = 64;
        hmacKeyringSetAeadReservation(ring, window, hkrTestReserveFn, &r);
        enum { kThreads = 8, kIters = 1024 };
        const size_t total = (size_t)kThreads * kIters;
        cleanerName(zfree, U64) uint64_t *collected =
            zmalloc(total * sizeof(uint64_t));
        hmacKeyringNonceWorkerCtx ctxs[kThreads];
        pthread_t tids[kThreads];
        for (uint32_t t = 0; t < kThreads; t++) {
            ctxs[t] = (hmacKeyringNonceWorkerCtx){.ring = ring,
                                                  .iters = kIters,
                                                  .out = collected +
                                                         (size_t)t * kIters,
                                                  .id = 21u};
            pthread_create(&tids[t], nullptr, hmacKeyringNonceWorker, &ctxs[t]);
        }
        for (uint32_t t = 0; t < kThreads; t++) {
            pthread_join(tids[t], nullptr);
        }
        qsort(collected, total, sizeof(*collected), hkrTestCmpU64);
        bool contiguous = true;
        for (size_t i = 0; i < total; i++) {
            contiguous = contiguous && collected[i] == (uint64_t)i;
        }
        /* The counter never passes a confirmed limit, so each reservation
         * starts exactly at the previous limit: total / window of them. */
        if (!contiguous || r.calls != total / window ||
            r.lastLimit != (uint64_t)total) {
            ERR("contiguous %d, %" PRIu64 " reservations up to %" PRIu64
                " (want %" PRIu64 " up to %zu)",
                contiguous, r.calls, r.lastLimit, (uint64_t)total / window,
                total);
        }
        hmacKeyringFree(ring);
    }

    TEST("AEAD reservation through persist: recovery never reissues") {
        /* Snapshot at counter 0, then issue past it with reservations in
         * the WAL; a second snapshot halfway (the WAL before it is folded
         * in), more nonces, then recover from the same stores as a restart
         * would: the counter must start above every nonce issued. */
        persist *p = persistCreate(&persistOpsHmacKeyring, nullptr);
        persistAttachSnapshot(p, persistStoreMemory(0));
        persistAttachWAL(p, persistStoreMemory(0));
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[32] = {0x36};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {1, 2, 3, 4};
        hmacKeyringInstall(ring, 36u, &hmacAlgoSHA256, key, sizeof(key), salt);
        if (!persistSnapshot(p, ring) ||
            !hmacKeyringSetAeadReservation(
                ring, 16, hmacKeyringReserveThroughPersist, p)) {
            ERR("persist setup");
        }
        uint64_t highest = 0;
        for (int i = 0; i < 40; i++) {
            uint8_t n[12];
            if (!hmacKeyringNextAeadNonce(ring, 36u, n)) {
                ERR("issuance through persist reservations");
                break;
            }
            highest = hkrTestNonceCounter(n);
            if (i == 19 && !persistSnapshot(p, ring)) {
                ERR("mid-way snapshot");
            }
        }
        hmacKeyring *recovered = persistRecover(p);
        uint64_t start = 0;
        if (recovered == nullptr ||
            !hmacKeyringPeekAeadCounter(recovered, 36u, &start) ||
            start <= highest || start != 48) {
            ERR("recovered counter %" PRIu64 " after issuing up to %" PRIu64
                " (want 48: three windows of 16)",
                start, highest);
        }
        hmacKeyringFree(recovered);
        hmacKeyringFree(ring);
        persistClose(p);
    }

    TEST("AEAD reservation through persist: no nonce until the limit is "
         "synced") {
        /* Both sync policies: the reservation logged durable at once, or
         * buffered and synced by the reservation.  When the WAL's sync
         * fails the reservation fails and no nonce is issued past the
         * window already reserved; once it syncs, issuance resumes. */
        if (hmacKeyringReserveThroughPersist(nullptr, 37u, 8)) {
            ERR("a NULL persist reserved");
        }
        static const persistSyncMode modes[2] = {PERSIST_SYNC_ALWAYS,
                                                 PERSIST_SYNC_NONE};
        for (int m = 0; m < 2; m++) {
            persistConfig cfg = persistDefaultConfig();
            cfg.syncMode = modes[m];
            persist *p = persistCreate(&persistOpsHmacKeyring, &cfg);
            persistStore *wal =
                persistStoreFaulty(persistStoreMemory(0), false);
            persistAttachSnapshot(p, persistStoreMemory(0));
            persistAttachWAL(p, wal);
            hmacKeyring *ring = hmacKeyringCreate();
            const uint8_t key[32] = {0x37};
            const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {9, 8, 7, 6};
            hmacKeyringInstall(ring, 37u, &hmacAlgoSHA256, key, sizeof(key),
                               salt);
            if (!persistSnapshot(p, ring) ||
                !hmacKeyringSetAeadReservation(
                    ring, 8, hmacKeyringReserveThroughPersist, p)) {
                ERR("persist setup");
            }
            uint8_t n[12];
            bool ok = true;
            for (int i = 0; i < 8; i++) {
                ok = ok && hmacKeyringNextAeadNonce(ring, 37u, n);
            }
            persistStoreFaultyArm(wal, PERSIST_FAULT_SYNC, 0);
            const bool pastWindow = hmacKeyringNextAeadNonce(ring, 37u, n);
            uint64_t counter = 0;
            hmacKeyringPeekAeadCounter(ring, 37u, &counter);
            persistStoreFaultyDisarm(wal);
            const bool resumed = hmacKeyringNextAeadNonce(ring, 37u, n);
            if (!ok || pastWindow || counter != 8 || !resumed ||
                hkrTestNonceCounter(n) != 8) {
                ERR("sync mode %d: window issued %d, past an unsynced "
                    "reservation %d (counter %" PRIu64 "), resumed %d",
                    m, ok, pastWindow, counter, resumed);
            }
            hmacKeyringFree(ring);
            persistClose(p);
        }
    }

    TEST("snapshot/restore: round-trip with counter + salt + multi-algo") {
        hmacKeyring *src = hmacKeyringCreate();
        const uint8_t key_a[32] = {0x10, 0x20, 0x30};
        const uint8_t key_b[24] = {0x40, 0x50, 0x60};
        /* id=11 is salted (test asserts nonce[8..11] == 0xAABBCCDD);
         * id=22 is Unsalted because the test does not issue nonces from
         * it and the snapshot must survive a slot with a zero salt. */
        const uint8_t initial_salt_a[HMAC_KEYRING_AEAD_SALT_BYTES] = {
            0x77, 0x77, 0x77, 0x77};
        hmacKeyringInstall(src, 11u, &hmacAlgoSHA256, key_a, sizeof(key_a),
                           initial_salt_a);
        hmacKeyringInstallUnsalted(src, 22u, &hmacAlgoBLAKE3, key_b,
                                   sizeof(key_b));
        /* Set salt + advance counter on id=11. */
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAA, 0xBB, 0xCC,
                                                            0xDD};
        hmacKeyringSetAeadSalt(src, 11u, salt);
        hmacKeyringRestoreAeadCounter(src, 11u, 7777u);

        const size_t sz = hmacKeyringSnapshotSize(src);
        if (sz < 8u) {
            ERR("snapshot size too small");
        }
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!hmacKeyringSnapshot(src, buf, sz, &written)) {
            ERR("snapshot should succeed");
        }
        if (written != sz) {
            ERR("written %zu != sz %zu", written, sz);
        }

        hmacKeyring *dst = hmacKeyringCreate();
        if (!hmacKeyringRestore(dst, buf, sz)) {
            ERR("restore should succeed on fresh keyring");
        }
        if (hmacKeyringSize(dst) != 2u) {
            ERR("dst size expected 2, got %zu", hmacKeyringSize(dst));
        }

        /* Verify id=11 has the same MAC output. */
        const hmacKey *prep = hmacKeyringLookup(dst, 11u);
        uint8_t exp[32], got[32];
        hmacCompute(&hmacAlgoSHA256, key_a, sizeof(key_a), "x", 1, exp);
        hmacComputeWithKey(prep, "x", 1, got);
        TEST_BYTES(err, exp, got, 32, "restored id=11 MAC mismatch");

        /* Counter + salt survived. */
        uint64_t ctr = 0;
        hmacKeyringPeekAeadCounter(dst, 11u, &ctr);
        if (ctr != 7777u) {
            ERR("restored counter expected 7777, got %" PRIu64, ctr);
        }
        uint8_t nonce[12];
        hmacKeyringNextAeadNonce(dst, 11u, nonce);
        if (nonce[8] != 0xAA || nonce[9] != 0xBB || nonce[10] != 0xCC ||
            nonce[11] != 0xDD) {
            ERR("restored salt did not propagate to nonce");
        }
        /* The issued nonce's counter should equal 7777 (the saved
         * value); subsequent peek should be 7778. */
        uint8_t expect_lo = (uint8_t)(7777u & 0xFFu);
        uint8_t expect_hi = (uint8_t)((7777u >> 8) & 0xFFu);
        if (nonce[0] != expect_lo || nonce[1] != expect_hi) {
            ERR("restored counter did not advance from saved value");
        }

        hmacKeyringFree(src);
        hmacKeyringFree(dst);
    }

    TEST("snapshot/restore: malformed inputs rejected, no partial state") {
        hmacKeyring *src = hmacKeyringCreate();
        const uint8_t key[16] = {0x77};
        hmacKeyringInstallUnsalted(src, 5u, &hmacAlgoSHA256, key, sizeof(key));

        uint8_t buf[256];
        size_t written = 0;
        hmacKeyringSnapshot(src, buf, sizeof(buf), &written);

        /* Truncate by one byte. */
        hmacKeyring *dst = hmacKeyringCreate();
        if (hmacKeyringRestore(dst, buf, written - 1u)) {
            ERR("Truncated buffer must be rejected");
        }
        if (hmacKeyringSize(dst) != 0u) {
            ERR("Rollback failed: dst should be empty after malformed restore");
        }

        /* Bad magic. */
        uint8_t bad[64];
        memcpy(bad, buf, written);
        bad[0] ^= 0xFF;
        if (hmacKeyringRestore(dst, bad, written)) {
            ERR("Bad magic must be rejected");
        }

        /* Trailing junk. */
        uint8_t big[256];
        memcpy(big, buf, written);
        big[written] = 0xCC;
        if (hmacKeyringRestore(dst, big, written + 1u)) {
            ERR("Trailing junk must be rejected");
        }

        hmacKeyringFree(src);
        hmacKeyringFree(dst);
    }

    TEST("snapshot/restore: well-formed flexes of the wrong shape, short "
         "buffers and bad reservations are refused") {
        cleaner(hmacKeyringFree) hmacKeyring *src = hmacKeyringCreate();
        cleaner(hmacKeyringFree) hmacKeyring *dst = hmacKeyringCreate();
        const uint8_t key[16] = {0x42};
        hmacKeyringInstallUnsalted(src, 9u, &hmacAlgoSHA256, key, sizeof(key));

        /* No entries; and a floor followed by one entry of a slot row */
        cleaner(flexFree) flex *none = flexNew();
        cleaner(flexFree) flex *partial = flexNew();
        flexPushUnsigned(&partial, 0, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&partial, 9, FLEX_ENDPOINT_TAIL);
        if (hmacKeyringRestore(dst, (const uint8_t *)none, flexBytes(none)) ||
            hmacKeyringRestore(dst, (const uint8_t *)partial,
                               flexBytes(partial)) ||
            hmacKeyringSize(dst) != 0u) {
            ERR("a flex with no floor or a partial slot row restored");
        }

        /* A buffer one byte short of the snapshot: refused, nothing
         * written or reported */
        uint8_t buf[256];
        size_t written = 0;
        if (!hmacKeyringSnapshot(src, buf, sizeof(buf), &written)) {
            ERR("snapshot into a roomy buffer failed");
        }
        uint8_t shortBuf[256];
        memset(shortBuf, 0xEE, sizeof(shortBuf));
        size_t shortWritten = 77;
        if (hmacKeyringSnapshot(src, shortBuf, written - 1, &shortWritten) ||
            shortWritten != 77 || shortBuf[0] != 0xEE) {
            ERR("a short snapshot buffer was written or reported");
        }

        /* A reservation record names a 32-bit nonzero key id and an
         * unsigned limit; anything else is refused */
        const struct {
            databox id;
            databox limit;
        } bad[] = {
            {databoxNewUnsigned(0), databoxNewUnsigned(10)},
            {databoxNewUnsigned((uint64_t)UINT32_MAX + 1),
             databoxNewUnsigned(10)},
            {databoxNewSigned(-3), databoxNewUnsigned(10)},
            {databoxNewUnsigned(9), databoxNewSigned(-10)},
        };
        hmacKeyring *target = src;
        for (size_t i = 0; i < countof(bad); i++) {
            cleaner(flexFree) flex *rec = flexNew();
            flexPushByType(&rec, &bad[i].id, FLEX_ENDPOINT_TAIL);
            flexPushByType(&rec, &bad[i].limit, FLEX_ENDPOINT_TAIL);
            if (persistOpsHmacKeyring.applyOp(&target, PERSIST_OP_UPDATE,
                                              (const uint8_t *)rec,
                                              flexBytes(rec))) {
                ERR("reservation record %zu was applied", i);
            }
        }
        cleaner(flexFree) flex *good = flexNew();
        flexPushUnsigned(&good, 9, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&good, 10, FLEX_ENDPOINT_TAIL);
        if (persistOpsHmacKeyring.applyOp(&target, PERSIST_OP_INSERT,
                                          (const uint8_t *)good,
                                          flexBytes(good))) {
            ERR("a reservation under another op was applied");
        }
    }

    TEST("snapshot/restore: a valid header over a corrupt key entry is "
         "refused") {
        hmacKeyring *src = hmacKeyringCreate();
        const uint8_t key[16] = {0x5A};
        hmacKeyringInstallUnsalted(src, 5u, &hmacAlgoSHA256, key, sizeof(key));

        uint8_t buf[256];
        size_t written = 0;
        if (!hmacKeyringSnapshot(src, buf, sizeof(buf), &written)) {
            ERR("Snapshot failed");
        }

        /* Give the last entry (the 16-byte key) the encoding byte of a
         * 64-byte string: the total-bytes header still matches the
         * buffer, but the key now claims bytes past the end of it */
        cleaner(flexFree) flex *longer = flexNew();
        uint8_t filler[64];
        memset(filler, 0xAB, sizeof(filler));
        flexPushBytes(&longer, filler, sizeof(filler), FLEX_ENDPOINT_TAIL);
        const uint8_t *longEntry = (const uint8_t *)flexHead(longer);
        const size_t longLen =
            flexBytes(longer) - (size_t)(longEntry - (const uint8_t *)longer);

        const flex *f = (const flex *)buf;
        uint8_t *last = (uint8_t *)flexIndex(f, (int32_t)(flexCount(f) - 1));
        const size_t lastLen = written - (size_t)(last - buf);
        if (lastLen >= longLen) {
            ERR("key entry is %zu bytes, not shorter than %zu", lastLen,
                longLen);
        }

        last[0] = longEntry[0];
        if (!flexValidateBuffer(buf, written)) {
            ERR("the corrupt snapshot's header should still validate");
        }
        if (flexValidateBufferDeep(buf, written)) {
            ERR("the corrupt snapshot should fail deep validation");
        }

        hmacKeyring *dst = hmacKeyringCreate();
        if (hmacKeyringRestore(dst, buf, written)) {
            ERR("Restore accepted a corrupt key entry");
        }
        if (hmacKeyringSize(dst) != 0u) {
            ERR("Refused restore left keys installed");
        }

        hmacKeyringFree(src);
        hmacKeyringFree(dst);
    }

    TEST("testHmacKeyringRestoreOversizedKeyReject") {
        /* Build a synthetic flex snapshot whose single slot declares a
         * key longer than HMAC_KEYRING_MAX_KEY_BYTES. Restore must
         * refuse, leaving the target keyring untouched. The cap is the
         * defense against a tampered persist file driving an attacker-
         * controlled zmalloc through Install. */
        cleaner(flexFree) flex *f = flexNew();
        const hmacKeyId id = 9u;
        const uint64_t algo_tag = 0u; /* SHA-256 */
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x01, 0x02, 0x03,
                                                            0x04};
        const uint64_t counter = 0u;

        /* Oversized key: HMAC_KEYRING_MAX_KEY_BYTES + 1 bytes. */
        const size_t oversize_len = HMAC_KEYRING_MAX_KEY_BYTES + 1u;
        cleanerName(zfree, Bytes) uint8_t *oversize_key = zmalloc(oversize_len);
        memset(oversize_key, 0xAB, oversize_len);

        flexPushUnsigned(&f, 0u, FLEX_ENDPOINT_TAIL); /* no floor */
        flexPushUnsigned(&f, (uint64_t)id, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, algo_tag, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, salt, sizeof(salt), FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f, counter, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f, oversize_key, oversize_len, FLEX_ENDPOINT_TAIL);

        const size_t flex_bytes = flexBytes(f);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(flex_bytes);
        memcpy(snap, f, flex_bytes);

        hmacKeyring *dst = hmacKeyringCreate();
        if (hmacKeyringRestore(dst, snap, flex_bytes)) {
            ERR("Restore of oversized-key snapshot MUST be rejected");
        }
        if (hmacKeyringSize(dst) != 0u) {
            ERR("Oversized-key reject left %zu slots installed (must be 0)",
                hmacKeyringSize(dst));
        }

        /* Build a valid snapshot with key_len == HMAC_KEYRING_MAX_KEY_BYTES
         * exactly — this must SUCCEED (cap is inclusive of equality). */
        cleaner(flexFree) flex *f_ok = flexNew();
        cleanerName(zfree, Bytes) uint8_t *max_key =
            zmalloc(HMAC_KEYRING_MAX_KEY_BYTES);
        memset(max_key, 0xCD, HMAC_KEYRING_MAX_KEY_BYTES);
        flexPushUnsigned(&f_ok, 0u, FLEX_ENDPOINT_TAIL); /* no floor */
        flexPushUnsigned(&f_ok, (uint64_t)id, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f_ok, algo_tag, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f_ok, salt, sizeof(salt), FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&f_ok, counter, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&f_ok, max_key, HMAC_KEYRING_MAX_KEY_BYTES,
                      FLEX_ENDPOINT_TAIL);
        const size_t ok_bytes = flexBytes(f_ok);
        cleanerName(zfree, Bytes) uint8_t *ok_snap = zmalloc(ok_bytes);
        memcpy(ok_snap, f_ok, ok_bytes);

        hmacKeyring *dst_ok = hmacKeyringCreate();
        if (!hmacKeyringRestore(dst_ok, ok_snap, ok_bytes)) {
            ERR("Restore at the cap boundary must succeed");
        }
        if (hmacKeyringSize(dst_ok) != 1u) {
            ERR("Boundary restore expected size 1, got %zu",
                hmacKeyringSize(dst_ok));
        }

        hmacKeyringFree(dst);
        hmacKeyringFree(dst_ok);

        /* Direct Install must also refuse oversized keys (symmetric
         * enforcement so the API cannot be used to seed a slot that
         * Restore would have refused). */
        hmacKeyring *direct = hmacKeyringCreate();
        cleanerName(zfree, Bytes) uint8_t *over =
            zmalloc(HMAC_KEYRING_MAX_KEY_BYTES + 1u);
        memset(over, 0xEF, HMAC_KEYRING_MAX_KEY_BYTES + 1u);
        if (hmacKeyringInstallUnsalted(direct, 1u, &hmacAlgoSHA256, over,
                                       HMAC_KEYRING_MAX_KEY_BYTES + 1u)) {
            ERR("InstallUnsalted with oversized key MUST be refused");
        }
        const uint8_t s[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x11, 0x22, 0x33,
                                                         0x44};
        if (hmacKeyringInstall(direct, 1u, &hmacAlgoSHA256, over,
                               HMAC_KEYRING_MAX_KEY_BYTES + 1u, s)) {
            ERR("Install with oversized key MUST be refused");
        }
        hmacKeyringFree(direct);
    }

    TEST("snapshot/restore: duplicate id in target collides and rolls back") {
        hmacKeyring *src = hmacKeyringCreate();
        const uint8_t key[16] = {0xAB};
        hmacKeyringInstallUnsalted(src, 9u, &hmacAlgoSHA256, key, sizeof(key));
        hmacKeyringInstallUnsalted(src, 10u, &hmacAlgoBLAKE3, key, sizeof(key));
        uint8_t buf[256];
        size_t written = 0;
        hmacKeyringSnapshot(src, buf, sizeof(buf), &written);

        hmacKeyring *dst = hmacKeyringCreate();
        /* Pre-install id=10 to force a collision on second record. */
        hmacKeyringInstallUnsalted(dst, 10u, &hmacAlgoSHA256, key, sizeof(key));
        if (hmacKeyringRestore(dst, buf, written)) {
            ERR("Collision must be rejected");
        }
        /* Rollback must have un-installed any partially-loaded records. */
        if (hmacKeyringSize(dst) != 1u) {
            ERR("Expected dst size 1 (pre-existing only), got %zu",
                hmacKeyringSize(dst));
        }
        /* Pre-existing id=10 must remain untouched (SHA256 algo). */
        const uint8_t *kb = nullptr;
        size_t kb_len = 0;
        const hmacHashAlgo *algo = nullptr;
        hmacKeyringGetRaw(dst, 10u, &kb, &kb_len, &algo);
        if (algo != &hmacAlgoSHA256) {
            ERR("Pre-existing id=10 should still be SHA256");
        }

        hmacKeyringFree(src);
        hmacKeyringFree(dst);
    }

    TEST("getRaw exposes the bytes that were stored") {
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[16] = {0x01, 0x02, 0x03, 0x04};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAB, 0xCD, 0xEF,
                                                            0x01};
        hmacKeyringInstall(ring, 42u, &hmacAlgoSHA256, key, sizeof(key), salt);
        const uint8_t *bytes = nullptr;
        size_t blen = 0;
        const hmacHashAlgo *algo = nullptr;
        if (!hmacKeyringGetRaw(ring, 42u, &bytes, &blen, &algo)) {
            ERR("getRaw of installed id should succeed");
        }
        if (blen != sizeof(key) || memcmp(bytes, key, blen) != 0) {
            ERR("getRaw bytes did not match installed bytes");
        }
        if (algo != &hmacAlgoSHA256) {
            ERR("getRaw algo pointer did not match");
        }
        hmacKeyringFree(ring);
    }

    TEST("testKeyringGetRawCopy") {
        /* hmacKeyringGetRawCopy makes a snapshot of the raw key bytes
         * under the rwlock so callers can hold the bytes past a Retire/
         * Install/SetAeadSalt without UAF. Validates the success path,
         * the too-small-buffer reject, and the missing-id reject. Also
         * confirms the copy is independent of subsequent keyring
         * mutations (Retire after copy leaves the local buffer intact). */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key[24] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
                                 0xDD, 0xEE, 0xFF, 0x01, 0x02, 0x03,
                                 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x10, 0x20, 0x30,
                                                            0x40};
        if (!hmacKeyringInstall(ring, 17u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERR("setup install must succeed");
        }

        /* Success path: copy into a buffer large enough. */
        uint8_t buf[32];
        memset(buf, 0xCC, sizeof(buf));
        size_t copied = 0;
        const hmacHashAlgo *algo = nullptr;
        if (!hmacKeyringGetRawCopy(ring, 17u, buf, sizeof(buf), &copied,
                                   &algo)) {
            ERR("GetRawCopy of installed id must succeed");
        }
        if (copied != sizeof(key)) {
            ERR("GetRawCopy wrote %zu bytes, expected %zu", copied,
                sizeof(key));
        }
        TEST_BYTES(err, buf, key, sizeof(key),
                   "GetRawCopy bytes did not match installed bytes");
        if (algo != &hmacAlgoSHA256) {
            ERR("GetRawCopy algo pointer did not match");
        }

        /* Too-small buffer rejects with out_len set to zero. */
        uint8_t tiny[8];
        size_t tiny_len = 999u;
        if (hmacKeyringGetRawCopy(ring, 17u, tiny, sizeof(tiny), &tiny_len,
                                  nullptr)) {
            ERR("GetRawCopy into too-small buffer must reject");
        }
        if (tiny_len != 0u) {
            ERR("rejected GetRawCopy left out_len = %zu (must be 0)", tiny_len);
        }

        /* Missing id rejects. */
        size_t missing_len = 999u;
        if (hmacKeyringGetRawCopy(ring, 9999u, buf, sizeof(buf), &missing_len,
                                  nullptr)) {
            ERR("GetRawCopy of missing id must reject");
        }
        if (missing_len != 0u) {
            ERR("missing-id reject left out_len = %zu (must be 0)",
                missing_len);
        }

        /* NULL out_buf rejects. */
        if (hmacKeyringGetRawCopy(ring, 17u, nullptr, 16u, nullptr, nullptr)) {
            ERR("GetRawCopy with NULL out_buf must reject");
        }

        /* HMAC_KEY_ID_INVALID rejects. */
        if (hmacKeyringGetRawCopy(ring, HMAC_KEY_ID_INVALID, buf, sizeof(buf),
                                  nullptr, nullptr)) {
            ERR("GetRawCopy with id == 0 must reject");
        }

        /* NULL ring rejects. */
        if (hmacKeyringGetRawCopy(nullptr, 17u, buf, sizeof(buf), nullptr,
                                  nullptr)) {
            ERR("GetRawCopy on NULL ring must reject");
        }

        /* After Retire, the local copy still holds the original bytes
         * — independence from keyring mutation is the whole point. */
        if (!hmacKeyringRetire(ring, 17u)) {
            ERR("Retire after copy must succeed");
        }
        TEST_BYTES(err, buf, key, sizeof(key),
                   "local copy was disturbed by Retire — independence broken");
        size_t after_retire_len = 999u;
        if (hmacKeyringGetRawCopy(ring, 17u, buf, sizeof(buf),
                                  &after_retire_len, nullptr)) {
            ERR("post-Retire GetRawCopy must reject");
        }
        if (after_retire_len != 0u) {
            ERR("post-Retire reject left out_len non-zero");
        }

        hmacKeyringFree(ring);
    }

    TEST("testHmacKeyringGetAesGcmKeyCopy") {
        /* hmacKeyringGetAesGcmKeyCopy mirrors GetRawCopy for the AES-GCM
         * key schedule cache: copies the slot's prepared aesGcmKey into
         * a caller-owned buffer under the rwlock so a concurrent Retire
         * cannot race the cipher's read of the round keys. Validates
         * the success path, missing-id reject, non-AES-key-length reject
         * (the slot has no cached schedule), and that the copy is
         * independent of post-call keyring mutations. */
        hmacKeyring *ring = hmacKeyringCreate();
        /* AES-256 key length triggers schedule caching. */
        uint8_t key[AES256_KEY_LEN];
        for (size_t i = 0; i < sizeof(key); i++) {
            key[i] = (uint8_t)(i * 7u + 11u);
        }
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAA, 0xBB, 0xCC,
                                                            0xDD};
        if (!hmacKeyringInstall(ring, 23u, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERR("setup install with AES-256-length key must succeed");
        }

        /* Success path: copy populates a self-contained struct. Compare
         * to the borrowed pointer to confirm bit-identical copy. */
        const aesGcmKey *borrowed = hmacKeyringGetAesGcmKey(ring, 23u);
        if (borrowed == nullptr) {
            ERR("setup: borrowed AES schedule was NULL (slot lacks cache)");
        }
        aesGcmKey local;
        memset(&local, 0xCC, sizeof(local));
        if (!hmacKeyringGetAesGcmKeyCopy(ring, 23u, &local)) {
            ERR("GetAesGcmKeyCopy of installed AES-class id must succeed");
        }
        TEST_BYTES(err, &local, borrowed, sizeof(local),
                   "Copy did not match borrowed schedule bytes");
        if (local.key_size != AES256_KEY_LEN) {
            ERR("Copy reported key_size %u, expected %u",
                (unsigned)local.key_size, (unsigned)AES256_KEY_LEN);
        }

        /* Missing id rejects. */
        aesGcmKey miss;
        memset(&miss, 0x77, sizeof(miss));
        if (hmacKeyringGetAesGcmKeyCopy(ring, 9999u, &miss)) {
            ERR("GetAesGcmKeyCopy of missing id must reject");
        }
        /* Non-AES-key-length slot has no cached schedule — reject. */
        const uint8_t hmac_key[20] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                      11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
        if (!hmacKeyringInstall(ring, 24u, &hmacAlgoSHA256, hmac_key,
                                sizeof(hmac_key), salt)) {
            ERR("setup install with 20-byte HMAC key must succeed");
        }
        if (hmacKeyringGetAesGcmKeyCopy(ring, 24u, &miss)) {
            ERR("GetAesGcmKeyCopy must reject slots without cached schedule");
        }

        /* NULL guards. */
        if (hmacKeyringGetAesGcmKeyCopy(nullptr, 23u, &miss)) {
            ERR("GetAesGcmKeyCopy on NULL ring must reject");
        }
        if (hmacKeyringGetAesGcmKeyCopy(ring, 23u, nullptr)) {
            ERR("GetAesGcmKeyCopy with NULL out_key must reject");
        }
        if (hmacKeyringGetAesGcmKeyCopy(ring, HMAC_KEY_ID_INVALID, &miss)) {
            ERR("GetAesGcmKeyCopy with id == 0 must reject");
        }

        /* Independence: Retire the slot after the copy. The local copy
         * must remain intact (this is the entire point of the API). */
        if (!hmacKeyringRetire(ring, 23u)) {
            ERR("Retire after copy must succeed");
        }
        /* Re-snapshot a fresh copy from a known-clean local — proves the
         * existing `local` is unchanged by the keyring teardown. */
        aesGcmKey reference;
        if (!aesGcmInit(&reference, key, sizeof(key))) {
            ERR("aesGcmInit of reference must succeed");
        }
        TEST_BYTES(err, &local, &reference, sizeof(local),
                   "local copy was disturbed by Retire — independence broken");
        if (hmacKeyringGetAesGcmKeyCopy(ring, 23u, &miss)) {
            ERR("post-Retire GetAesGcmKeyCopy must reject");
        }
        dkSecureZero(&local, sizeof(local));
        dkSecureZero(&reference, sizeof(reference));
        dkSecureZero(&miss, sizeof(miss));

        hmacKeyringFree(ring);
    }

    TEST("revoked key floor: default zero, monotonic, snapshot round-trip") {
        hmacKeyring *ring = hmacKeyringCreate();

        /* Default floor is zero. */
        if (hmacKeyringMinValidKeyId(ring) != HMAC_KEY_ID_INVALID) {
            ERR("default floor should be HMAC_KEY_ID_INVALID (0)");
        }

        /* NULL-safe. */
        if (hmacKeyringMinValidKeyId(nullptr) != HMAC_KEY_ID_INVALID) {
            ERR("NULL ring should return floor=0");
        }
        if (hmacKeyringSetMinValidKeyId(nullptr, 5u)) {
            ERR("Set on NULL ring must refuse");
        }

        /* Raise to 7; observe value. */
        if (!hmacKeyringSetMinValidKeyId(ring, 7u)) {
            ERR("Set floor=7 should succeed from default 0");
        }
        if (hmacKeyringMinValidKeyId(ring) != 7u) {
            ERR("floor expected 7 got %u",
                (unsigned)hmacKeyringMinValidKeyId(ring));
        }

        /* Equal value is idempotent success. */
        if (!hmacKeyringSetMinValidKeyId(ring, 7u)) {
            ERR("re-setting same floor must be idempotent success");
        }

        /* Forward move succeeds. */
        if (!hmacKeyringSetMinValidKeyId(ring, 10u)) {
            ERR("forward move 7->10 should succeed");
        }
        if (hmacKeyringMinValidKeyId(ring) != 10u) {
            ERR("floor did not advance to 10");
        }

        /* Backward move refused. */
        if (hmacKeyringSetMinValidKeyId(ring, 5u)) {
            ERR("backward move 10->5 MUST be refused");
        }
        if (hmacKeyringMinValidKeyId(ring) != 10u) {
            ERR("refused backward move corrupted floor");
        }

        /* Snapshot/restore preserves the floor. */
        const uint8_t key[16] = {0xC3, 0xC3, 0xC3};
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0x01, 0x02, 0x03,
                                                            0x04};
        hmacKeyringInstall(ring, 50u, &hmacAlgoSHA256, key, sizeof(key), salt);
        const size_t sz = hmacKeyringSnapshotSize(ring);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!hmacKeyringSnapshot(ring, buf, sz, &written)) {
            ERR("snapshot with floor set must succeed");
        }
        hmacKeyring *dst = hmacKeyringCreate();
        if (!hmacKeyringRestore(dst, buf, written)) {
            ERR("restore with a floor must succeed");
        }
        if (hmacKeyringMinValidKeyId(dst) != 10u) {
            ERR("restored floor expected 10 got %u",
                (unsigned)hmacKeyringMinValidKeyId(dst));
        }
        if (hmacKeyringSize(dst) != 1u) {
            ERR("restored slot count expected 1 got %zu", hmacKeyringSize(dst));
        }
        hmacKeyringFree(ring);
        hmacKeyringFree(dst);
    }

    TEST("revoked key floor: no floor snapshots as 0 and restores as none") {
        /* A keyring with no floor still writes its floor entry (0), and
         * restoring it sets no floor */
        hmacKeyring *src = hmacKeyringCreate();
        const uint8_t key[16] = {0x88};
        hmacKeyringInstallUnsalted(src, 1u, &hmacAlgoSHA256, key, sizeof(key));
        const size_t sz = hmacKeyringSnapshotSize(src);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        hmacKeyringSnapshot(src, buf, sz, nullptr);

        if (flexCount((const flex *)buf) != 1 + HMAC_KEYRING_ENTRIES_PER_SLOT) {
            ERR("expected %zu flex entries, got %zu",
                1 + HMAC_KEYRING_ENTRIES_PER_SLOT,
                flexCount((const flex *)buf));
        }
        hmacKeyring *dst = hmacKeyringCreate();
        if (!hmacKeyringRestore(dst, buf, sz)) {
            ERR("restore of a no-floor snapshot must succeed");
        }
        if (hmacKeyringMinValidKeyId(dst) != HMAC_KEY_ID_INVALID) {
            ERR("no-floor restore expected floor=0 got %u",
                (unsigned)hmacKeyringMinValidKeyId(dst));
        }
        hmacKeyringFree(src);
        hmacKeyringFree(dst);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
